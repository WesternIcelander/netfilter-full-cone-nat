/*
 * Copyright (c) 2018 Chion Tang <tech@chionlab.moe>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/list.h>
#include <linux/hashtable.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/workqueue.h>
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
#include <linux/notifier.h>
#endif
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/netfilter/x_tables.h>
#include <net/netfilter/nf_nat.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_ecache.h>

#define HASH_2(x, y) ((x + y) / 2 * (x + y + 1) + y)

#define HASHTABLE_BUCKET_BITS 10

#ifndef NF_NAT_RANGE_PROTO_RANDOM_FULLY
#define NF_NAT_RANGE_PROTO_RANDOM_FULLY (1 << 4)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)

static inline int nf_ct_netns_get(struct net *net, u8 nfproto) { return 0; }

static inline void nf_ct_netns_put(struct net *net, u8 nfproto) {}

static inline struct net_device *xt_in(const struct xt_action_param *par) {
  return par->in;
}

static inline struct net_device *xt_out(const struct xt_action_param *par) {
  return par->out;
}

static inline unsigned int xt_hooknum(const struct xt_action_param *par) {
  return par->hooknum;
}

#endif

struct nat_mapping_original_tuple {
  struct nf_conntrack_tuple tuple;

  struct list_head node;
};

struct nat_mapping {
  uint16_t port;     /* external UDP port */
  int ifindex;       /* external interface index*/

  __be32 int_addr;   /* internal source ip address */
  uint16_t int_port; /* internal source port */

  int refer_count;   /* how many references linked to this mapping
                      * aka. length of original_tuple_list */

  struct list_head original_tuple_list;

  struct hlist_node node_by_ext_port;
  struct hlist_node node_by_int_src;

};

struct tuple_list {
  struct nf_conntrack_tuple tuple_original;
  struct nf_conntrack_tuple tuple_reply;
  struct list_head list;
};

#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
struct notifier_block ct_event_notifier;
#else
struct nf_ct_event_notifier ct_event_notifier;
#endif
int tg_refer_count = 0;
int ct_event_notifier_registered = 0;

static DEFINE_MUTEX(nf_ct_net_event_lock);

static DEFINE_HASHTABLE(mapping_table_by_ext_port, HASHTABLE_BUCKET_BITS);
static DEFINE_HASHTABLE(mapping_table_by_int_src, HASHTABLE_BUCKET_BITS);

static DEFINE_SPINLOCK(fullconenat_lock);

static LIST_HEAD(dying_tuple_list);
static DEFINE_SPINLOCK(dying_tuple_list_lock);
static void gc_worker(struct work_struct *work);
static struct workqueue_struct *wq __read_mostly = NULL;
static DECLARE_DELAYED_WORK(gc_worker_wk, gc_worker);

static char tuple_tmp_string[512];
/* non-atomic: can only be called serially within lock zones. */
static char* nf_ct_stringify_tuple(const struct nf_conntrack_tuple *t) {
  snprintf(tuple_tmp_string, sizeof(tuple_tmp_string), "%pI4:%hu -> %pI4:%hu",
         &t->src.u3.ip, be16_to_cpu(t->src.u.all),
         &t->dst.u3.ip, be16_to_cpu(t->dst.u.all));
  return tuple_tmp_string;
}

static struct nat_mapping* allocate_mapping(const __be32 int_addr, const uint16_t int_port, const uint16_t port, const int ifindex) {
  struct nat_mapping *p_new;
  u32 hash_src;

  p_new = kmalloc(sizeof(struct nat_mapping), GFP_ATOMIC);
  if (p_new == NULL) {
    pr_debug("xt_FULLCONENAT: ERROR: kmalloc() for new nat_mapping failed.\n");
    return NULL;
  }
  p_new->port = port;
  p_new->int_addr = int_addr;
  p_new->int_port = int_port;
  p_new->ifindex = ifindex;
  p_new->refer_count = 0;
  (p_new->original_tuple_list).next = &(p_new->original_tuple_list);
  (p_new->original_tuple_list).prev = &(p_new->original_tuple_list);

  hash_src = HASH_2(int_addr, (u32)int_port);

  hash_add(mapping_table_by_ext_port, &p_new->node_by_ext_port, port);
  hash_add(mapping_table_by_int_src, &p_new->node_by_int_src, hash_src);

  pr_debug("xt_FULLCONENAT: new mapping allocated for %pI4:%d ==> %d\n", 
    &p_new->int_addr, p_new->int_port, p_new->port);

  return p_new;
}

static void add_original_tuple_to_mapping(struct nat_mapping *mapping, const struct nf_conntrack_tuple* original_tuple) {
  struct nat_mapping_original_tuple *item = kmalloc(sizeof(struct nat_mapping_original_tuple), GFP_ATOMIC);
  if (item == NULL) {
    pr_debug("xt_FULLCONENAT: ERROR: kmalloc() for nat_mapping_original_tuple failed.\n");
    return;
  }
  memcpy(&item->tuple, original_tuple, sizeof(struct nf_conntrack_tuple));
  list_add(&item->node, &mapping->original_tuple_list);
  (mapping->refer_count)++;
}

static struct nat_mapping* get_mapping_by_ext_port(const uint16_t port, const int ifindex) {
  struct nat_mapping *p_current;

  hash_for_each_possible(mapping_table_by_ext_port, p_current, node_by_ext_port, port) {
    if (p_current->port == port && p_current->ifindex == ifindex) {
      return p_current;
    }
  }

  return NULL;
}

static struct nat_mapping* get_mapping_by_int_src(const __be32 src_ip, const uint16_t src_port) {
  struct nat_mapping *p_current;
  u32 hash_src = HASH_2(src_ip, (u32)src_port);

  hash_for_each_possible(mapping_table_by_int_src, p_current, node_by_int_src, hash_src) {
    if (p_current->int_addr == src_ip && p_current->int_port == src_port) {
      return p_current;
    }
  }

  return NULL;
}

static void kill_mapping(struct nat_mapping *mapping) {
  struct list_head *iter, *tmp;
  struct nat_mapping_original_tuple *original_tuple_item;

  if (mapping == NULL) {
    return;
  }

  list_for_each_safe(iter, tmp, &mapping->original_tuple_list) {
    original_tuple_item = list_entry(iter, struct nat_mapping_original_tuple, node);
    list_del(&original_tuple_item->node);
    kfree(original_tuple_item);
  }

  hash_del(&mapping->node_by_ext_port);
  hash_del(&mapping->node_by_int_src);
  kfree(mapping);
}

static void destroy_mappings(void) {
  struct nat_mapping *p_current;
  struct hlist_node *tmp;
  int i;

  spin_lock_bh(&fullconenat_lock);

  hash_for_each_safe(mapping_table_by_ext_port, i, tmp, p_current, node_by_ext_port) {
    kill_mapping(p_current);
  }

  spin_unlock_bh(&fullconenat_lock);
}

/* check if a mapping is valid.
 * possibly delete and free an invalid mapping.
 * the mapping should not be used anymore after check_mapping() returns 0. */
static int check_mapping(struct nat_mapping* mapping, struct net *net, const struct nf_conntrack_zone *zone) {
  struct list_head *iter, *tmp;
  struct nat_mapping_original_tuple *original_tuple_item;
  struct nf_conntrack_tuple_hash *tuple_hash;
  struct nf_conn *ct;

  if (mapping == NULL) {
    return 0;
  }

  if (mapping->port == 0 || mapping->int_addr == 0 || mapping->int_port == 0 || mapping->ifindex == -1) {
    return 0;
  }

  /* for dying/unconfirmed conntrack tuples, an IPCT_DESTROY event may NOT be fired.
   * so we manually kill one of those tuples once we acquire one. */

  list_for_each_safe(iter, tmp, &mapping->original_tuple_list) {
    original_tuple_item = list_entry(iter, struct nat_mapping_original_tuple, node);

    tuple_hash = nf_conntrack_find_get(net, zone, &original_tuple_item->tuple);

    if (tuple_hash == NULL) {
      pr_debug("xt_FULLCONENAT: check_mapping(): tuple %s dying/unconfirmed. free this tuple.\n", nf_ct_stringify_tuple(&original_tuple_item->tuple));

      list_del(&original_tuple_item->node);
      kfree(original_tuple_item);
      (mapping->refer_count)--;
    } else {
      ct = nf_ct_tuplehash_to_ctrack(tuple_hash);
      if (ct != NULL)
        nf_ct_put(ct);
    }

  }

  /* kill the mapping if need */
  pr_debug("xt_FULLCONENAT: check_mapping() refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
  if (mapping->refer_count <= 0) {
    pr_debug("xt_FULLCONENAT: check_mapping(): kill dying/unconfirmed mapping at ext port %d\n", mapping->port);
    kill_mapping(mapping);
    return 0;
  } else {
    return 1;
  }
}

static void handle_dying_tuples(void) {
  struct list_head *iter, *tmp, *iter_2, *tmp_2;
  struct tuple_list *item;
  struct nf_conntrack_tuple *ct_tuple;
  struct nat_mapping *mapping;
  __be32 ip;
  uint16_t port;
  struct nat_mapping_original_tuple *original_tuple_item;

  spin_lock_bh(&fullconenat_lock);
  spin_lock_bh(&dying_tuple_list_lock);

  list_for_each_safe(iter, tmp, &dying_tuple_list) {
    item = list_entry(iter, struct tuple_list, list);

    /* we dont know the conntrack direction for now so we try in both ways. */
    ct_tuple = &(item->tuple_original); 
    ip = (ct_tuple->src).u3.ip;
    port = be16_to_cpu((ct_tuple->src).u.udp.port);
    mapping = get_mapping_by_int_src(ip, port);
    if (mapping == NULL) {
      ct_tuple = &(item->tuple_reply);
      ip = (ct_tuple->src).u3.ip;
      port = be16_to_cpu((ct_tuple->src).u.udp.port);
      mapping = get_mapping_by_int_src(ip, port);
      if (mapping != NULL) {
        pr_debug("xt_FULLCONENAT: handle_dying_tuples(): INBOUND dying conntrack at ext port %d\n", mapping->port);
      }
    } else {
      pr_debug("xt_FULLCONENAT: handle_dying_tuples(): OUTBOUND dying conntrack at ext port %d\n", mapping->port);
    }

    if (mapping == NULL) {
      goto next;
    }

    /* look for the corresponding out-dated tuple and free it */
    list_for_each_safe(iter_2, tmp_2, &mapping->original_tuple_list) {
      original_tuple_item = list_entry(iter_2, struct nat_mapping_original_tuple, node);

      if (nf_ct_tuple_equal(&original_tuple_item->tuple, &(item->tuple_original))) {
        pr_debug("xt_FULLCONENAT: handle_dying_tuples(): tuple %s expired. free this tuple.\n",
          nf_ct_stringify_tuple(&original_tuple_item->tuple));
        list_del(&original_tuple_item->node);
        kfree(original_tuple_item);
        (mapping->refer_count)--;
      }
    }

    /* then kill the mapping if needed*/
    pr_debug("xt_FULLCONENAT: handle_dying_tuples(): refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
    if (mapping->refer_count <= 0) {
      pr_debug("xt_FULLCONENAT: handle_dying_tuples(): kill expired mapping at ext port %d\n", mapping->port);
      kill_mapping(mapping);
    }

next:
    list_del(&item->list);
    kfree(item);
  }

  spin_unlock_bh(&dying_tuple_list_lock);
  spin_unlock_bh(&fullconenat_lock);
}

static void gc_worker(struct work_struct *work) {
  handle_dying_tuples();
}

/* conntrack destroy event callback function */
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
static int ct_event_cb(struct notifier_block *this, unsigned long events, void *ptr) {
  struct nf_ct_event *item = ptr;
#else
static int ct_event_cb(unsigned int events, const struct nf_ct_event *item) {
#endif
  struct nf_conn *ct;
  struct nf_conntrack_tuple *ct_tuple_reply, *ct_tuple_original;
  uint8_t protonum;
  struct tuple_list *dying_tuple_item;

  ct = item->ct;
  /* we handle only conntrack destroy events */
  if (ct == NULL || !(events & (1 << IPCT_DESTROY))) {
    return 0;
  }

  ct_tuple_original = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

  ct_tuple_reply = &(ct->tuplehash[IP_CT_DIR_REPLY].tuple);

  protonum = (ct_tuple_original->dst).protonum;
  if (protonum != IPPROTO_UDP) {
    return 0;
  }

  dying_tuple_item = kmalloc(sizeof(struct tuple_list), GFP_ATOMIC);

  if (dying_tuple_item == NULL) {
    pr_debug("xt_FULLCONENAT: warning: ct_event_cb(): kmalloc failed.\n");
    return 0;
  }

  memcpy(&(dying_tuple_item->tuple_original), ct_tuple_original, sizeof(struct nf_conntrack_tuple));
  memcpy(&(dying_tuple_item->tuple_reply), ct_tuple_reply, sizeof(struct nf_conntrack_tuple));

  spin_lock_bh(&dying_tuple_list_lock);

  list_add(&(dying_tuple_item->list), &dying_tuple_list);

  spin_unlock_bh(&dying_tuple_list_lock);

  if (wq != NULL)
    queue_delayed_work(wq, &gc_worker_wk, msecs_to_jiffies(100));

  return 0;
}

static __be32 get_device_ip(const struct net_device* dev) {
  struct in_device* in_dev;
  struct in_ifaddr* if_info;
  __be32 result;

  if (dev == NULL) {
    return 0;
  }

  rcu_read_lock();
  in_dev = dev->ip_ptr;
  if (in_dev == NULL) {
    rcu_read_unlock();
    return 0;
  }
  if_info = in_dev->ifa_list;
  if (if_info) {
    result = if_info->ifa_local;
    rcu_read_unlock();
    return result;
  } else {
    rcu_read_unlock();
    return 0;
  }
}

static uint16_t find_appropriate_port(struct net *net, const struct nf_conntrack_zone *zone, const uint16_t original_port, const int ifindex, const struct nf_nat_ipv4_range *range) {
  uint16_t min, start, selected, range_size, i;
  struct nat_mapping* mapping = NULL;

  if (range->flags & NF_NAT_RANGE_PROTO_SPECIFIED) {
    min = be16_to_cpu((range->min).udp.port);
    range_size = be16_to_cpu((range->max).udp.port) - min + 1;
  } else {
    /* minimum port is 1024. same behavior as default linux NAT. */
    min = 1024;
    range_size = 65535 - min + 1;
  }

  if ((range->flags & NF_NAT_RANGE_PROTO_RANDOM)
    || (range->flags & NF_NAT_RANGE_PROTO_RANDOM_FULLY)) {
    /* for now we do the same thing for both --random and --random-fully */

    /* select a random starting point */
    start = (uint16_t)(get_random_u32() % (u32)range_size);
  } else {

    if ((original_port >= min && original_port <= min + range_size - 1)
      || !(range->flags & NF_NAT_RANGE_PROTO_SPECIFIED)) {
      /* 1. try to preserve the port if it's available */
      mapping = get_mapping_by_ext_port(original_port, ifindex);
      if (mapping == NULL || !(check_mapping(mapping, net, zone))) {
        return original_port;
      }
    }

    /* otherwise, we start from zero */
    start = 0;
  }

  for (i = 0; i < range_size; i++) {
    /* 2. try to find an available port */
    selected = min + ((start + i) % range_size);
    mapping = get_mapping_by_ext_port(selected, ifindex);
    if (mapping == NULL || !(check_mapping(mapping, net, zone))) {
      return selected;
    }
  }

  /* 3. at least we tried. override a previous mapping. */
  selected = min + start;
  mapping = get_mapping_by_ext_port(selected, ifindex);
  kill_mapping(mapping);

  return selected;
}

static unsigned int fullconenat_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
  const struct nf_nat_ipv4_multi_range_compat *mr;
  const struct nf_nat_ipv4_range *range;

  const struct nf_conntrack_zone *zone;
  struct net *net;
  struct nf_conn *ct;
  enum ip_conntrack_info ctinfo;
  struct nf_conntrack_tuple *ct_tuple, *ct_tuple_origin;

  struct net_device *net_dev;

  struct nat_mapping *mapping, *src_mapping;
  unsigned int ret;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 18, 0)
  struct nf_nat_range2 newrange;
#else
  struct nf_nat_range newrange;
#endif

  __be32 new_ip, ip;
  uint16_t port, original_port, want_port;
  uint8_t protonum;
  int ifindex;

  ip = 0;
  original_port = 0;
  src_mapping = NULL;

  mr = par->targinfo;
  range = &mr->range[0];

  mapping = NULL;
  ret = XT_CONTINUE;

  ct = nf_ct_get(skb, &ctinfo);
  net = nf_ct_net(ct);
  zone = nf_ct_zone(ct);

  memset(&newrange.min_addr, 0, sizeof(newrange.min_addr));
  memset(&newrange.max_addr, 0, sizeof(newrange.max_addr));
  newrange.flags       = mr->range[0].flags | NF_NAT_RANGE_MAP_IPS;
  newrange.min_proto   = mr->range[0].min;
  newrange.max_proto   = mr->range[0].max;

  if (xt_hooknum(par) == NF_INET_PRE_ROUTING) {
    /* inbound packets */
    ifindex = xt_in(par)->ifindex;

    ct_tuple_origin = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);

    protonum = (ct_tuple_origin->dst).protonum;
    if (protonum != IPPROTO_UDP) {
      return ret;
    }
    ip = (ct_tuple_origin->dst).u3.ip;
    port = be16_to_cpu((ct_tuple_origin->dst).u.udp.port);

    /* get the corresponding ifindex by the dst_ip (aka. external ip of this host),
     * in case the packet needs to be forwarded from another inbound interface. */
    net_dev = ip_dev_find(net, ip);
    if (net_dev != NULL) {
      ifindex = net_dev->ifindex;
      dev_put(net_dev);
    }

    spin_lock_bh(&fullconenat_lock);

    /* find an active mapping based on the inbound port */
    mapping = get_mapping_by_ext_port(port, ifindex);
    if (mapping == NULL) {
      spin_unlock_bh(&fullconenat_lock);
      return ret;
    }
    if (check_mapping(mapping, net, zone)) {
      newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
      newrange.min_addr.ip = mapping->int_addr;
      newrange.max_addr.ip = mapping->int_addr;
      newrange.min_proto.udp.port = cpu_to_be16(mapping->int_port);
      newrange.max_proto = newrange.min_proto;

      pr_debug("xt_FULLCONENAT: <INBOUND DNAT> %s ==> %pI4:%d\n", nf_ct_stringify_tuple(ct_tuple_origin), &mapping->int_addr, mapping->int_port);

      ret = nf_nat_setup_info(ct, &newrange, HOOK2MANIP(xt_hooknum(par)));

      if (ret == NF_ACCEPT) {
        add_original_tuple_to_mapping(mapping, ct_tuple_origin);
        pr_debug("xt_FULLCONENAT: fullconenat_tg(): INBOUND: refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
      }
    }
    spin_unlock_bh(&fullconenat_lock);
    return ret;


  } else if (xt_hooknum(par) == NF_INET_POST_ROUTING) {
    /* outbound packets */
    ifindex = xt_out(par)->ifindex;

    ct_tuple_origin = &(ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple);
    protonum = (ct_tuple_origin->dst).protonum;

    spin_lock_bh(&fullconenat_lock);

    if (protonum == IPPROTO_UDP) {
      ip = (ct_tuple_origin->src).u3.ip;
      original_port = be16_to_cpu((ct_tuple_origin->src).u.udp.port);

      src_mapping = get_mapping_by_int_src(ip, original_port);
      if (src_mapping != NULL && check_mapping(src_mapping, net, zone)) {

        /* outbound nat: if a previously established mapping is active,
         * we will reuse that mapping. */

        newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
        newrange.min_proto.udp.port = cpu_to_be16(src_mapping->port);
        newrange.max_proto = newrange.min_proto;

      } else {

        /* if not, we find a new external port to map to.
         * the SNAT may fail so we should re-check the mapped port later. */
        want_port = find_appropriate_port(net, zone, original_port, ifindex, range);

        newrange.flags = NF_NAT_RANGE_MAP_IPS | NF_NAT_RANGE_PROTO_SPECIFIED;
        newrange.min_proto.udp.port = cpu_to_be16(want_port);
        newrange.max_proto = newrange.min_proto;

        src_mapping = NULL;

      }
    }

    if(mr->range[0].flags & NF_NAT_RANGE_MAP_IPS) {
      newrange.min_addr.ip = mr->range[0].min_ip;
      newrange.max_addr.ip = mr->range[0].max_ip;
    } else {
      new_ip = get_device_ip(skb->dev);
      newrange.min_addr.ip = new_ip;
      newrange.max_addr.ip = new_ip;
    }

    /* do SNAT now */
    ret = nf_nat_setup_info(ct, &newrange, HOOK2MANIP(xt_hooknum(par)));

    if (protonum != IPPROTO_UDP || ret != NF_ACCEPT) {
      /* for non-UDP packets and failed SNAT, bailout */
      spin_unlock_bh(&fullconenat_lock);
      return ret;
    }

    /* the reply tuple contains the mapped port. */
    ct_tuple = &(ct->tuplehash[IP_CT_DIR_REPLY].tuple);
    /* this is the resulted mapped port. */
    port = be16_to_cpu((ct_tuple->dst).u.udp.port);

    pr_debug("xt_FULLCONENAT: <OUTBOUND SNAT> %s ==> %d\n", nf_ct_stringify_tuple(ct_tuple_origin), port);

    /* save the mapping information into our mapping table */
    mapping = src_mapping;
    if (mapping == NULL || !check_mapping(mapping, net, zone)) {
      mapping = allocate_mapping(ip, original_port, port, ifindex);
    }
    if (mapping != NULL) {
      add_original_tuple_to_mapping(mapping, ct_tuple_origin);
      pr_debug("xt_FULLCONENAT: fullconenat_tg(): OUTBOUND: refer_count for mapping at ext_port %d is now %d\n", mapping->port, mapping->refer_count);
    }

    spin_unlock_bh(&fullconenat_lock);
    return ret;
  }

  return ret;
}

static int fullconenat_tg_check(const struct xt_tgchk_param *par)
{
  mutex_lock(&nf_ct_net_event_lock);

  tg_refer_count++;

  pr_debug("xt_FULLCONENAT: fullconenat_tg_check(): tg_refer_count is now %d\n", tg_refer_count);

  if (tg_refer_count == 1) {
    nf_ct_netns_get(par->net, par->family);
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
    ct_event_notifier.notifier_call = ct_event_cb;
#else
    ct_event_notifier.ct_event = ct_event_cb;
#endif

    nf_conntrack_register_notifier(par->net, &ct_event_notifier);
        ct_event_notifier_registered = 1;
    pr_debug("xt_FULLCONENAT: fullconenat_tg_check(): ct_event_notifier "
             "registered\n");
  }

  mutex_unlock(&nf_ct_net_event_lock);

  return 0;
}

static void fullconenat_tg_destroy(const struct xt_tgdtor_param *par)
{
  mutex_lock(&nf_ct_net_event_lock);

  tg_refer_count--;

  pr_debug("xt_FULLCONENAT: fullconenat_tg_destroy(): tg_refer_count is now %d\n", tg_refer_count);

  if (tg_refer_count == 0) {
    if (ct_event_notifier_registered) {
      nf_conntrack_unregister_notifier(par->net);
      ct_event_notifier_registered = 0;

      pr_debug("xt_FULLCONENAT: fullconenat_tg_destroy(): ct_event_notifier unregistered\n");

    }
    nf_ct_netns_put(par->net, par->family);
  }

  mutex_unlock(&nf_ct_net_event_lock);
}

static struct xt_target tg_reg[] __read_mostly = {
 {
  .name       = "FULLCONENAT",
  .family     = NFPROTO_IPV4,
  .revision   = 0,
  .target     = fullconenat_tg,
  .targetsize = sizeof(struct nf_nat_ipv4_multi_range_compat),
  .table      = "nat",
  .hooks      = (1 << NF_INET_PRE_ROUTING) |
                (1 << NF_INET_POST_ROUTING),
  .checkentry = fullconenat_tg_check,
  .destroy    = fullconenat_tg_destroy,
  .me         = THIS_MODULE,
 },
};

static int __init fullconenat_tg_init(void)
{
  wq = create_singlethread_workqueue("xt_FULLCONENAT");
  if (wq == NULL) {
    printk("xt_FULLCONENAT: warning: failed to create workqueue\n");
  }

  return xt_register_targets(tg_reg, ARRAY_SIZE(tg_reg));
}

static void fullconenat_tg_exit(void)
{
  xt_unregister_targets(tg_reg, ARRAY_SIZE(tg_reg));

  if (wq) {
    cancel_delayed_work_sync(&gc_worker_wk);
    flush_workqueue(wq);
    destroy_workqueue(wq);
  }

  handle_dying_tuples();
  destroy_mappings();
}

module_init(fullconenat_tg_init);
module_exit(fullconenat_tg_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xtables: implementation of RFC3489 full cone NAT");
MODULE_AUTHOR("Chion Tang <tech@chionlab.moe>");
MODULE_ALIAS("ipt_FULLCONENAT");
