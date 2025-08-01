/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _NET_GRO_H
#define _NET_GRO_H

#include <linux/indirect_call_wrapper.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <net/ip6_checksum.h>
#include <linux/skbuff.h>
#include <net/udp.h>
#include <net/hotdata.h>

/* This should be increased if a protocol with a bigger head is added. */
#define GRO_MAX_HEAD (MAX_HEADER + 128)

struct napi_gro_cb {
	union {
		struct {
			/* Virtual address of skb_shinfo(skb)->frags[0].page + offset. */
			void	*frag0;

			/* Length of frag0. */
			unsigned int frag0_len;
		};

		struct {
			/* used in skb_gro_receive() slow path */
			struct sk_buff *last;

			/* jiffies when first packet was created/queued */
			unsigned long age;
		};
	};

	/* This indicates where we are processing relative to skb->data. */
	int	data_offset;

	/* This is non-zero if the packet cannot be merged with the new skb. */
	u16	flush;

	/* Number of segments aggregated. */
	u16	count;

	/* Used in ipv6_gro_receive() and foo-over-udp and esp-in-udp */
	u16	proto;

	u16	pad;

/* Used in napi_gro_cb::free */
#define NAPI_GRO_FREE             1
#define NAPI_GRO_FREE_STOLEN_HEAD 2
	/* portion of the cb set to zero at every gro iteration */
	struct_group(zeroed,

		/* Start offset for remote checksum offload */
		u16	gro_remcsum_start;

		/* This is non-zero if the packet may be of the same flow. */
		u8	same_flow:1;

		/* Used in tunnel GRO receive */
		u8	encap_mark:1;

		/* GRO checksum is valid */
		u8	csum_valid:1;

		/* Number of checksums via CHECKSUM_UNNECESSARY */
		u8	csum_cnt:3;

		/* Free the skb? */
		u8	free:2;

		/* Used in foo-over-udp, set in udp[46]_gro_receive */
		u8	is_ipv6:1;

		/* Used in GRE, set in fou/gue_gro_receive */
		u8	is_fou:1;

		/* Used to determine if ipid_offset can be ignored */
		u8	ip_fixedid:1;

		/* Number of gro_receive callbacks this packet already went through */
		u8 recursion_counter:4;

		/* GRO is done by frag_list pointer chaining. */
		u8	is_flist:1;
	);

	/* used to support CHECKSUM_COMPLETE for tunneling protocols */
	__wsum	csum;

	/* L3 offsets */
	union {
		struct {
			u16 network_offset;
			u16 inner_network_offset;
		};
		u16 network_offsets[2];
	};
};

#define NAPI_GRO_CB(skb) ((struct napi_gro_cb *)(skb)->cb)

#define GRO_RECURSION_LIMIT 15
static inline int gro_recursion_inc_test(struct sk_buff *skb)
{
	return ++NAPI_GRO_CB(skb)->recursion_counter == GRO_RECURSION_LIMIT;
}

typedef struct sk_buff *(*gro_receive_t)(struct list_head *, struct sk_buff *);
static inline struct sk_buff *call_gro_receive(gro_receive_t cb,
					       struct list_head *head,
					       struct sk_buff *skb)
{
	if (unlikely(gro_recursion_inc_test(skb))) {
		NAPI_GRO_CB(skb)->flush |= 1;
		return NULL;
	}

	return cb(head, skb);
}

typedef struct sk_buff *(*gro_receive_sk_t)(struct sock *, struct list_head *,
					    struct sk_buff *);
static inline struct sk_buff *call_gro_receive_sk(gro_receive_sk_t cb,
						  struct sock *sk,
						  struct list_head *head,
						  struct sk_buff *skb)
{
	if (unlikely(gro_recursion_inc_test(skb))) {
		NAPI_GRO_CB(skb)->flush |= 1;
		return NULL;
	}

	return cb(sk, head, skb);
}

static inline unsigned int skb_gro_offset(const struct sk_buff *skb)
{
	return NAPI_GRO_CB(skb)->data_offset;
}

static inline unsigned int skb_gro_len(const struct sk_buff *skb)
{
	return skb->len - NAPI_GRO_CB(skb)->data_offset;
}

static inline void skb_gro_pull(struct sk_buff *skb, unsigned int len)
{
	NAPI_GRO_CB(skb)->data_offset += len;
}

static inline void *skb_gro_header_fast(const struct sk_buff *skb,
					unsigned int offset)
{
	return NAPI_GRO_CB(skb)->frag0 + offset;
}

static inline bool skb_gro_may_pull(const struct sk_buff *skb,
				    unsigned int hlen)
{
	return likely(hlen <= NAPI_GRO_CB(skb)->frag0_len);
}

static inline void *skb_gro_header_slow(struct sk_buff *skb, unsigned int hlen,
					unsigned int offset)
{
	if (!pskb_may_pull(skb, hlen))
		return NULL;

	return skb->data + offset;
}

static inline void *skb_gro_header(struct sk_buff *skb, unsigned int hlen,
				   unsigned int offset)
{
	void *ptr;

	ptr = skb_gro_header_fast(skb, offset);
	if (!skb_gro_may_pull(skb, hlen))
		ptr = skb_gro_header_slow(skb, hlen, offset);
	return ptr;
}

static inline int skb_gro_receive_network_offset(const struct sk_buff *skb)
{
	return NAPI_GRO_CB(skb)->network_offsets[NAPI_GRO_CB(skb)->encap_mark];
}

static inline void *skb_gro_network_header(const struct sk_buff *skb)
{
	if (skb_gro_may_pull(skb, skb_gro_offset(skb)))
		return skb_gro_header_fast(skb, skb_gro_receive_network_offset(skb));

	return skb->data + skb_gro_receive_network_offset(skb);
}

static inline __wsum inet_gro_compute_pseudo(const struct sk_buff *skb,
					     int proto)
{
	const struct iphdr *iph = skb_gro_network_header(skb);

	return csum_tcpudp_nofold(iph->saddr, iph->daddr,
				  skb_gro_len(skb), proto, 0);
}

static inline void skb_gro_postpull_rcsum(struct sk_buff *skb,
					const void *start, unsigned int len)
{
	if (NAPI_GRO_CB(skb)->csum_valid)
		NAPI_GRO_CB(skb)->csum = wsum_negate(csum_partial(start, len,
						wsum_negate(NAPI_GRO_CB(skb)->csum)));
}

/* GRO checksum functions. These are logical equivalents of the normal
 * checksum functions (in skbuff.h) except that they operate on the GRO
 * offsets and fields in sk_buff.
 */

__sum16 __skb_gro_checksum_complete(struct sk_buff *skb);

static inline bool skb_at_gro_remcsum_start(struct sk_buff *skb)
{
	return (NAPI_GRO_CB(skb)->gro_remcsum_start == skb_gro_offset(skb));
}

static inline bool __skb_gro_checksum_validate_needed(struct sk_buff *skb,
						      bool zero_okay,
						      __sum16 check)
{
	return ((skb->ip_summed != CHECKSUM_PARTIAL ||
		skb_checksum_start_offset(skb) <
		 skb_gro_offset(skb)) &&
		!skb_at_gro_remcsum_start(skb) &&
		NAPI_GRO_CB(skb)->csum_cnt == 0 &&
		(!zero_okay || check));
}

static inline __sum16 __skb_gro_checksum_validate_complete(struct sk_buff *skb,
							   __wsum psum)
{
	if (NAPI_GRO_CB(skb)->csum_valid &&
	    !csum_fold(csum_add(psum, NAPI_GRO_CB(skb)->csum)))
		return 0;

	NAPI_GRO_CB(skb)->csum = psum;

	return __skb_gro_checksum_complete(skb);
}

static inline void skb_gro_incr_csum_unnecessary(struct sk_buff *skb)
{
	if (NAPI_GRO_CB(skb)->csum_cnt > 0) {
		/* Consume a checksum from CHECKSUM_UNNECESSARY */
		NAPI_GRO_CB(skb)->csum_cnt--;
	} else {
		/* Update skb for CHECKSUM_UNNECESSARY and csum_level when we
		 * verified a new top level checksum or an encapsulated one
		 * during GRO. This saves work if we fallback to normal path.
		 */
		__skb_incr_checksum_unnecessary(skb);
	}
}

#define __skb_gro_checksum_validate(skb, proto, zero_okay, check,	\
				    compute_pseudo)			\
({									\
	__sum16 __ret = 0;						\
	if (__skb_gro_checksum_validate_needed(skb, zero_okay, check))	\
		__ret = __skb_gro_checksum_validate_complete(skb,	\
				compute_pseudo(skb, proto));		\
	if (!__ret)							\
		skb_gro_incr_csum_unnecessary(skb);			\
	__ret;								\
})

#define skb_gro_checksum_validate(skb, proto, compute_pseudo)		\
	__skb_gro_checksum_validate(skb, proto, false, 0, compute_pseudo)

#define skb_gro_checksum_validate_zero_check(skb, proto, check,		\
					     compute_pseudo)		\
	__skb_gro_checksum_validate(skb, proto, true, check, compute_pseudo)

#define skb_gro_checksum_simple_validate(skb)				\
	__skb_gro_checksum_validate(skb, 0, false, 0, null_compute_pseudo)

static inline bool __skb_gro_checksum_convert_check(struct sk_buff *skb)
{
	return (NAPI_GRO_CB(skb)->csum_cnt == 0 &&
		!NAPI_GRO_CB(skb)->csum_valid);
}

static inline void __skb_gro_checksum_convert(struct sk_buff *skb,
					      __wsum pseudo)
{
	NAPI_GRO_CB(skb)->csum = ~pseudo;
	NAPI_GRO_CB(skb)->csum_valid = 1;
}

#define skb_gro_checksum_try_convert(skb, proto, compute_pseudo)	\
do {									\
	if (__skb_gro_checksum_convert_check(skb))			\
		__skb_gro_checksum_convert(skb, 			\
					   compute_pseudo(skb, proto));	\
} while (0)

struct gro_remcsum {
	int offset;
	__wsum delta;
};

static inline void skb_gro_remcsum_init(struct gro_remcsum *grc)
{
	grc->offset = 0;
	grc->delta = 0;
}

static inline void *skb_gro_remcsum_process(struct sk_buff *skb, void *ptr,
					    unsigned int off, size_t hdrlen,
					    int start, int offset,
					    struct gro_remcsum *grc,
					    bool nopartial)
{
	__wsum delta;
	size_t plen = hdrlen + max_t(size_t, offset + sizeof(u16), start);

	BUG_ON(!NAPI_GRO_CB(skb)->csum_valid);

	if (!nopartial) {
		NAPI_GRO_CB(skb)->gro_remcsum_start = off + hdrlen + start;
		return ptr;
	}

	ptr = skb_gro_header(skb, off + plen, off);
	if (!ptr)
		return NULL;

	delta = remcsum_adjust(ptr + hdrlen, NAPI_GRO_CB(skb)->csum,
			       start, offset);

	/* Adjust skb->csum since we changed the packet */
	NAPI_GRO_CB(skb)->csum = csum_add(NAPI_GRO_CB(skb)->csum, delta);

	grc->offset = off + hdrlen + offset;
	grc->delta = delta;

	return ptr;
}

static inline void skb_gro_remcsum_cleanup(struct sk_buff *skb,
					   struct gro_remcsum *grc)
{
	void *ptr;
	size_t plen = grc->offset + sizeof(u16);

	if (!grc->delta)
		return;

	ptr = skb_gro_header(skb, plen, grc->offset);
	if (!ptr)
		return;

	remcsum_unadjust((__sum16 *)ptr, grc->delta);
}

#ifdef CONFIG_XFRM_OFFLOAD
static inline void skb_gro_flush_final(struct sk_buff *skb, struct sk_buff *pp, int flush)
{
	if (PTR_ERR(pp) != -EINPROGRESS)
		NAPI_GRO_CB(skb)->flush |= flush;
}
static inline void skb_gro_flush_final_remcsum(struct sk_buff *skb,
					       struct sk_buff *pp,
					       int flush,
					       struct gro_remcsum *grc)
{
	if (PTR_ERR(pp) != -EINPROGRESS) {
		NAPI_GRO_CB(skb)->flush |= flush;
		skb_gro_remcsum_cleanup(skb, grc);
		skb->remcsum_offload = 0;
	}
}
#else
static inline void skb_gro_flush_final(struct sk_buff *skb, struct sk_buff *pp, int flush)
{
	NAPI_GRO_CB(skb)->flush |= flush;
}
static inline void skb_gro_flush_final_remcsum(struct sk_buff *skb,
					       struct sk_buff *pp,
					       int flush,
					       struct gro_remcsum *grc)
{
	NAPI_GRO_CB(skb)->flush |= flush;
	skb_gro_remcsum_cleanup(skb, grc);
	skb->remcsum_offload = 0;
}
#endif

INDIRECT_CALLABLE_DECLARE(struct sk_buff *ipv6_gro_receive(struct list_head *,
							   struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(int ipv6_gro_complete(struct sk_buff *, int));
INDIRECT_CALLABLE_DECLARE(struct sk_buff *inet_gro_receive(struct list_head *,
							   struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(int inet_gro_complete(struct sk_buff *, int));

INDIRECT_CALLABLE_DECLARE(struct sk_buff *udp4_gro_receive(struct list_head *,
							   struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(int udp4_gro_complete(struct sk_buff *, int));

INDIRECT_CALLABLE_DECLARE(struct sk_buff *udp6_gro_receive(struct list_head *,
							   struct sk_buff *));
INDIRECT_CALLABLE_DECLARE(int udp6_gro_complete(struct sk_buff *, int));

#define indirect_call_gro_receive_inet(cb, f2, f1, head, skb)	\
({								\
	unlikely(gro_recursion_inc_test(skb)) ?			\
		NAPI_GRO_CB(skb)->flush |= 1, NULL :		\
		INDIRECT_CALL_INET(cb, f2, f1, head, skb);	\
})

struct sk_buff *udp_gro_receive(struct list_head *head, struct sk_buff *skb,
				struct udphdr *uh, struct sock *sk);
int udp_gro_complete(struct sk_buff *skb, int nhoff, udp_lookup_t lookup);

static inline struct udphdr *udp_gro_udphdr(struct sk_buff *skb)
{
	struct udphdr *uh;
	unsigned int hlen, off;

	off  = skb_gro_offset(skb);
	hlen = off + sizeof(*uh);
	uh   = skb_gro_header(skb, hlen, off);

	return uh;
}

static inline __wsum ip6_gro_compute_pseudo(const struct sk_buff *skb,
					    int proto)
{
	const struct ipv6hdr *iph = skb_gro_network_header(skb);

	return ~csum_unfold(csum_ipv6_magic(&iph->saddr, &iph->daddr,
					    skb_gro_len(skb), proto, 0));
}

static inline int inet_gro_flush(const struct iphdr *iph, const struct iphdr *iph2,
				 struct sk_buff *p, bool outer)
{
	const u32 id = ntohl(*(__be32 *)&iph->id);
	const u32 id2 = ntohl(*(__be32 *)&iph2->id);
	const u16 ipid_offset = (id >> 16) - (id2 >> 16);
	const u16 count = NAPI_GRO_CB(p)->count;
	const u32 df = id & IP_DF;
	int flush;

	/* All fields must match except length and checksum. */
	flush = (iph->ttl ^ iph2->ttl) | (iph->tos ^ iph2->tos) | (df ^ (id2 & IP_DF));

	if (flush | (outer && df))
		return flush;

	/* When we receive our second frame we can make a decision on if we
	 * continue this flow as an atomic flow with a fixed ID or if we use
	 * an incrementing ID.
	 */
	if (count == 1 && df && !ipid_offset)
		NAPI_GRO_CB(p)->ip_fixedid = true;

	return ipid_offset ^ (count * !NAPI_GRO_CB(p)->ip_fixedid);
}

static inline int ipv6_gro_flush(const struct ipv6hdr *iph, const struct ipv6hdr *iph2)
{
	/* <Version:4><Traffic_Class:8><Flow_Label:20> */
	__be32 first_word = *(__be32 *)iph ^ *(__be32 *)iph2;

	/* Flush if Traffic Class fields are different. */
	return !!((first_word & htonl(0x0FF00000)) |
		(__force __be32)(iph->hop_limit ^ iph2->hop_limit));
}

static inline int __gro_receive_network_flush(const void *th, const void *th2,
					      struct sk_buff *p, const u16 diff,
					      bool outer)
{
	const void *nh = th - diff;
	const void *nh2 = th2 - diff;

	if (((struct iphdr *)nh)->version == 6)
		return ipv6_gro_flush(nh, nh2);
	else
		return inet_gro_flush(nh, nh2, p, outer);
}

static inline int gro_receive_network_flush(const void *th, const void *th2,
					    struct sk_buff *p)
{
	const bool encap_mark = NAPI_GRO_CB(p)->encap_mark;
	int off = skb_transport_offset(p);
	int flush;

	flush = __gro_receive_network_flush(th, th2, p, off - NAPI_GRO_CB(p)->network_offset, encap_mark);
	if (encap_mark)
		flush |= __gro_receive_network_flush(th, th2, p, off - NAPI_GRO_CB(p)->inner_network_offset, false);

	return flush;
}

int skb_gro_receive(struct sk_buff *p, struct sk_buff *skb);
int skb_gro_receive_list(struct sk_buff *p, struct sk_buff *skb);
void __gro_flush(struct gro_node *gro, bool flush_old);

static inline void gro_flush(struct gro_node *gro, bool flush_old)
{
	if (!gro->bitmask)
		return;

	__gro_flush(gro, flush_old);
}

static inline void napi_gro_flush(struct napi_struct *napi, bool flush_old)
{
	gro_flush(&napi->gro, flush_old);
}

/* Pass the currently batched GRO_NORMAL SKBs up to the stack. */
static inline void gro_normal_list(struct gro_node *gro)
{
	if (!gro->rx_count)
		return;
	netif_receive_skb_list_internal(&gro->rx_list);
	INIT_LIST_HEAD(&gro->rx_list);
	gro->rx_count = 0;
}

static inline void gro_flush_normal(struct gro_node *gro, bool flush_old)
{
	gro_flush(gro, flush_old);
	gro_normal_list(gro);
}

/* Queue one GRO_NORMAL SKB up for list processing. If batch size exceeded,
 * pass the whole batch up to the stack.
 */
static inline void gro_normal_one(struct gro_node *gro, struct sk_buff *skb,
				  int segs)
{
	list_add_tail(&skb->list, &gro->rx_list);
	gro->rx_count += segs;
	if (gro->rx_count >= READ_ONCE(net_hotdata.gro_normal_batch))
		gro_normal_list(gro);
}

void gro_init(struct gro_node *gro);
void gro_cleanup(struct gro_node *gro);

/* This function is the alternative of 'inet_iif' and 'inet_sdif'
 * functions in case we can not rely on fields of IPCB.
 *
 * The caller must verify skb_valid_dst(skb) is false and skb->dev is initialized.
 * The caller must hold the RCU read lock.
 */
static inline void inet_get_iif_sdif(const struct sk_buff *skb, int *iif, int *sdif)
{
	*iif = inet_iif(skb) ?: skb->dev->ifindex;
	*sdif = 0;

#if IS_ENABLED(CONFIG_NET_L3_MASTER_DEV)
	if (netif_is_l3_slave(skb->dev)) {
		struct net_device *master = netdev_master_upper_dev_get_rcu(skb->dev);

		*sdif = *iif;
		*iif = master ? master->ifindex : 0;
	}
#endif
}

/* This function is the alternative of 'inet6_iif' and 'inet6_sdif'
 * functions in case we can not rely on fields of IP6CB.
 *
 * The caller must verify skb_valid_dst(skb) is false and skb->dev is initialized.
 * The caller must hold the RCU read lock.
 */
static inline void inet6_get_iif_sdif(const struct sk_buff *skb, int *iif, int *sdif)
{
	/* using skb->dev->ifindex because skb_dst(skb) is not initialized */
	*iif = skb->dev->ifindex;
	*sdif = 0;

#if IS_ENABLED(CONFIG_NET_L3_MASTER_DEV)
	if (netif_is_l3_slave(skb->dev)) {
		struct net_device *master = netdev_master_upper_dev_get_rcu(skb->dev);

		*sdif = *iif;
		*iif = master ? master->ifindex : 0;
	}
#endif
}

struct packet_offload *gro_find_receive_by_type(__be16 type);
struct packet_offload *gro_find_complete_by_type(__be16 type);

#endif /* _NET_GRO_H */
