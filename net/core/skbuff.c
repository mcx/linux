// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *	Routines having to do with the 'struct sk_buff' memory handlers.
 *
 *	Authors:	Alan Cox <alan@lxorguk.ukuu.org.uk>
 *			Florian La Roche <rzsfl@rz.uni-sb.de>
 *
 *	Fixes:
 *		Alan Cox	:	Fixed the worst of the load
 *					balancer bugs.
 *		Dave Platt	:	Interrupt stacking fix.
 *	Richard Kooijman	:	Timestamp fixes.
 *		Alan Cox	:	Changed buffer format.
 *		Alan Cox	:	destructor hook for AF_UNIX etc.
 *		Linus Torvalds	:	Better skb_clone.
 *		Alan Cox	:	Added skb_copy.
 *		Alan Cox	:	Added all the changed routines Linus
 *					only put in the headers
 *		Ray VanTassle	:	Fixed --skb->lock in free
 *		Alan Cox	:	skb_copy copy arp field
 *		Andi Kleen	:	slabified it.
 *		Robert Olsson	:	Removed skb_head_pool
 *
 *	NOTE:
 *		The __skb_ routines should be called with interrupts
 *	disabled, or you better be *real* sure that the operation is atomic
 *	with respect to whatever list is being frobbed (e.g. via lock_sock()
 *	or via disabling bottom half handlers, etc).
 */

/*
 *	The functions in this file will not compile correctly with gcc 2.4.x
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/slab.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/sctp.h>
#include <linux/netdevice.h>
#ifdef CONFIG_NET_CLS_ACT
#include <net/pkt_sched.h>
#endif
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/skbuff_ref.h>
#include <linux/splice.h>
#include <linux/cache.h>
#include <linux/rtnetlink.h>
#include <linux/init.h>
#include <linux/scatterlist.h>
#include <linux/errqueue.h>
#include <linux/prefetch.h>
#include <linux/bitfield.h>
#include <linux/if_vlan.h>
#include <linux/mpls.h>
#include <linux/kcov.h>
#include <linux/iov_iter.h>
#include <linux/crc32.h>

#include <net/protocol.h>
#include <net/dst.h>
#include <net/sock.h>
#include <net/checksum.h>
#include <net/gro.h>
#include <net/gso.h>
#include <net/hotdata.h>
#include <net/ip6_checksum.h>
#include <net/xfrm.h>
#include <net/mpls.h>
#include <net/mptcp.h>
#include <net/mctp.h>
#include <net/page_pool/helpers.h>
#include <net/dropreason.h>

#include <linux/uaccess.h>
#include <trace/events/skb.h>
#include <linux/highmem.h>
#include <linux/capability.h>
#include <linux/user_namespace.h>
#include <linux/indirect_call_wrapper.h>
#include <linux/textsearch.h>

#include "dev.h"
#include "devmem.h"
#include "netmem_priv.h"
#include "sock_destructor.h"

#ifdef CONFIG_SKB_EXTENSIONS
static struct kmem_cache *skbuff_ext_cache __ro_after_init;
#endif

#define GRO_MAX_HEAD_PAD (GRO_MAX_HEAD + NET_SKB_PAD + NET_IP_ALIGN)
#define SKB_SMALL_HEAD_SIZE SKB_HEAD_ALIGN(max(MAX_TCP_HEADER, \
					       GRO_MAX_HEAD_PAD))

/* We want SKB_SMALL_HEAD_CACHE_SIZE to not be a power of two.
 * This should ensure that SKB_SMALL_HEAD_HEADROOM is a unique
 * size, and we can differentiate heads from skb_small_head_cache
 * vs system slabs by looking at their size (skb_end_offset()).
 */
#define SKB_SMALL_HEAD_CACHE_SIZE					\
	(is_power_of_2(SKB_SMALL_HEAD_SIZE) ?			\
		(SKB_SMALL_HEAD_SIZE + L1_CACHE_BYTES) :	\
		SKB_SMALL_HEAD_SIZE)

#define SKB_SMALL_HEAD_HEADROOM						\
	SKB_WITH_OVERHEAD(SKB_SMALL_HEAD_CACHE_SIZE)

/* kcm_write_msgs() relies on casting paged frags to bio_vec to use
 * iov_iter_bvec(). These static asserts ensure the cast is valid is long as the
 * netmem is a page.
 */
static_assert(offsetof(struct bio_vec, bv_page) ==
	      offsetof(skb_frag_t, netmem));
static_assert(sizeof_field(struct bio_vec, bv_page) ==
	      sizeof_field(skb_frag_t, netmem));

static_assert(offsetof(struct bio_vec, bv_len) == offsetof(skb_frag_t, len));
static_assert(sizeof_field(struct bio_vec, bv_len) ==
	      sizeof_field(skb_frag_t, len));

static_assert(offsetof(struct bio_vec, bv_offset) ==
	      offsetof(skb_frag_t, offset));
static_assert(sizeof_field(struct bio_vec, bv_offset) ==
	      sizeof_field(skb_frag_t, offset));

#undef FN
#define FN(reason) [SKB_DROP_REASON_##reason] = #reason,
static const char * const drop_reasons[] = {
	[SKB_CONSUMED] = "CONSUMED",
	DEFINE_DROP_REASON(FN, FN)
};

static const struct drop_reason_list drop_reasons_core = {
	.reasons = drop_reasons,
	.n_reasons = ARRAY_SIZE(drop_reasons),
};

const struct drop_reason_list __rcu *
drop_reasons_by_subsys[SKB_DROP_REASON_SUBSYS_NUM] = {
	[SKB_DROP_REASON_SUBSYS_CORE] = RCU_INITIALIZER(&drop_reasons_core),
};
EXPORT_SYMBOL(drop_reasons_by_subsys);

/**
 * drop_reasons_register_subsys - register another drop reason subsystem
 * @subsys: the subsystem to register, must not be the core
 * @list: the list of drop reasons within the subsystem, must point to
 *	a statically initialized list
 */
void drop_reasons_register_subsys(enum skb_drop_reason_subsys subsys,
				  const struct drop_reason_list *list)
{
	if (WARN(subsys <= SKB_DROP_REASON_SUBSYS_CORE ||
		 subsys >= ARRAY_SIZE(drop_reasons_by_subsys),
		 "invalid subsystem %d\n", subsys))
		return;

	/* must point to statically allocated memory, so INIT is OK */
	RCU_INIT_POINTER(drop_reasons_by_subsys[subsys], list);
}
EXPORT_SYMBOL_GPL(drop_reasons_register_subsys);

/**
 * drop_reasons_unregister_subsys - unregister a drop reason subsystem
 * @subsys: the subsystem to remove, must not be the core
 *
 * Note: This will synchronize_rcu() to ensure no users when it returns.
 */
void drop_reasons_unregister_subsys(enum skb_drop_reason_subsys subsys)
{
	if (WARN(subsys <= SKB_DROP_REASON_SUBSYS_CORE ||
		 subsys >= ARRAY_SIZE(drop_reasons_by_subsys),
		 "invalid subsystem %d\n", subsys))
		return;

	RCU_INIT_POINTER(drop_reasons_by_subsys[subsys], NULL);

	synchronize_rcu();
}
EXPORT_SYMBOL_GPL(drop_reasons_unregister_subsys);

/**
 *	skb_panic - private function for out-of-line support
 *	@skb:	buffer
 *	@sz:	size
 *	@addr:	address
 *	@msg:	skb_over_panic or skb_under_panic
 *
 *	Out-of-line support for skb_put() and skb_push().
 *	Called via the wrapper skb_over_panic() or skb_under_panic().
 *	Keep out of line to prevent kernel bloat.
 *	__builtin_return_address is not used because it is not always reliable.
 */
static void skb_panic(struct sk_buff *skb, unsigned int sz, void *addr,
		      const char msg[])
{
	pr_emerg("%s: text:%px len:%d put:%d head:%px data:%px tail:%#lx end:%#lx dev:%s\n",
		 msg, addr, skb->len, sz, skb->head, skb->data,
		 (unsigned long)skb->tail, (unsigned long)skb->end,
		 skb->dev ? skb->dev->name : "<NULL>");
	BUG();
}

static void skb_over_panic(struct sk_buff *skb, unsigned int sz, void *addr)
{
	skb_panic(skb, sz, addr, __func__);
}

static void skb_under_panic(struct sk_buff *skb, unsigned int sz, void *addr)
{
	skb_panic(skb, sz, addr, __func__);
}

#define NAPI_SKB_CACHE_SIZE	64
#define NAPI_SKB_CACHE_BULK	16
#define NAPI_SKB_CACHE_HALF	(NAPI_SKB_CACHE_SIZE / 2)

struct napi_alloc_cache {
	local_lock_t bh_lock;
	struct page_frag_cache page;
	unsigned int skb_count;
	void *skb_cache[NAPI_SKB_CACHE_SIZE];
};

static DEFINE_PER_CPU(struct page_frag_cache, netdev_alloc_cache);
static DEFINE_PER_CPU(struct napi_alloc_cache, napi_alloc_cache) = {
	.bh_lock = INIT_LOCAL_LOCK(bh_lock),
};

void *__napi_alloc_frag_align(unsigned int fragsz, unsigned int align_mask)
{
	struct napi_alloc_cache *nc = this_cpu_ptr(&napi_alloc_cache);
	void *data;

	fragsz = SKB_DATA_ALIGN(fragsz);

	local_lock_nested_bh(&napi_alloc_cache.bh_lock);
	data = __page_frag_alloc_align(&nc->page, fragsz,
				       GFP_ATOMIC | __GFP_NOWARN, align_mask);
	local_unlock_nested_bh(&napi_alloc_cache.bh_lock);
	return data;

}
EXPORT_SYMBOL(__napi_alloc_frag_align);

void *__netdev_alloc_frag_align(unsigned int fragsz, unsigned int align_mask)
{
	void *data;

	if (in_hardirq() || irqs_disabled()) {
		struct page_frag_cache *nc = this_cpu_ptr(&netdev_alloc_cache);

		fragsz = SKB_DATA_ALIGN(fragsz);
		data = __page_frag_alloc_align(nc, fragsz,
					       GFP_ATOMIC | __GFP_NOWARN,
					       align_mask);
	} else {
		local_bh_disable();
		data = __napi_alloc_frag_align(fragsz, align_mask);
		local_bh_enable();
	}
	return data;
}
EXPORT_SYMBOL(__netdev_alloc_frag_align);

static struct sk_buff *napi_skb_cache_get(void)
{
	struct napi_alloc_cache *nc = this_cpu_ptr(&napi_alloc_cache);
	struct sk_buff *skb;

	local_lock_nested_bh(&napi_alloc_cache.bh_lock);
	if (unlikely(!nc->skb_count)) {
		nc->skb_count = kmem_cache_alloc_bulk(net_hotdata.skbuff_cache,
						      GFP_ATOMIC | __GFP_NOWARN,
						      NAPI_SKB_CACHE_BULK,
						      nc->skb_cache);
		if (unlikely(!nc->skb_count)) {
			local_unlock_nested_bh(&napi_alloc_cache.bh_lock);
			return NULL;
		}
	}

	skb = nc->skb_cache[--nc->skb_count];
	local_unlock_nested_bh(&napi_alloc_cache.bh_lock);
	kasan_mempool_unpoison_object(skb, kmem_cache_size(net_hotdata.skbuff_cache));

	return skb;
}

/**
 * napi_skb_cache_get_bulk - obtain a number of zeroed skb heads from the cache
 * @skbs: pointer to an at least @n-sized array to fill with skb pointers
 * @n: number of entries to provide
 *
 * Tries to obtain @n &sk_buff entries from the NAPI percpu cache and writes
 * the pointers into the provided array @skbs. If there are less entries
 * available, tries to replenish the cache and bulk-allocates the diff from
 * the MM layer if needed.
 * The heads are being zeroed with either memset() or %__GFP_ZERO, so they are
 * ready for {,__}build_skb_around() and don't have any data buffers attached.
 * Must be called *only* from the BH context.
 *
 * Return: number of successfully allocated skbs (@n if no actual allocation
 *	   needed or kmem_cache_alloc_bulk() didn't fail).
 */
u32 napi_skb_cache_get_bulk(void **skbs, u32 n)
{
	struct napi_alloc_cache *nc = this_cpu_ptr(&napi_alloc_cache);
	u32 bulk, total = n;

	local_lock_nested_bh(&napi_alloc_cache.bh_lock);

	if (nc->skb_count >= n)
		goto get;

	/* No enough cached skbs. Try refilling the cache first */
	bulk = min(NAPI_SKB_CACHE_SIZE - nc->skb_count, NAPI_SKB_CACHE_BULK);
	nc->skb_count += kmem_cache_alloc_bulk(net_hotdata.skbuff_cache,
					       GFP_ATOMIC | __GFP_NOWARN, bulk,
					       &nc->skb_cache[nc->skb_count]);
	if (likely(nc->skb_count >= n))
		goto get;

	/* Still not enough. Bulk-allocate the missing part directly, zeroed */
	n -= kmem_cache_alloc_bulk(net_hotdata.skbuff_cache,
				   GFP_ATOMIC | __GFP_ZERO | __GFP_NOWARN,
				   n - nc->skb_count, &skbs[nc->skb_count]);
	if (likely(nc->skb_count >= n))
		goto get;

	/* kmem_cache didn't allocate the number we need, limit the output */
	total -= n - nc->skb_count;
	n = nc->skb_count;

get:
	for (u32 base = nc->skb_count - n, i = 0; i < n; i++) {
		u32 cache_size = kmem_cache_size(net_hotdata.skbuff_cache);

		skbs[i] = nc->skb_cache[base + i];

		kasan_mempool_unpoison_object(skbs[i], cache_size);
		memset(skbs[i], 0, offsetof(struct sk_buff, tail));
	}

	nc->skb_count -= n;
	local_unlock_nested_bh(&napi_alloc_cache.bh_lock);

	return total;
}
EXPORT_SYMBOL_GPL(napi_skb_cache_get_bulk);

static inline void __finalize_skb_around(struct sk_buff *skb, void *data,
					 unsigned int size)
{
	struct skb_shared_info *shinfo;

	size -= SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	/* Assumes caller memset cleared SKB */
	skb->truesize = SKB_TRUESIZE(size);
	refcount_set(&skb->users, 1);
	skb->head = data;
	skb->data = data;
	skb_reset_tail_pointer(skb);
	skb_set_end_offset(skb, size);
	skb->mac_header = (typeof(skb->mac_header))~0U;
	skb->transport_header = (typeof(skb->transport_header))~0U;
	skb->alloc_cpu = raw_smp_processor_id();
	/* make sure we initialize shinfo sequentially */
	shinfo = skb_shinfo(skb);
	memset(shinfo, 0, offsetof(struct skb_shared_info, dataref));
	atomic_set(&shinfo->dataref, 1);

	skb_set_kcov_handle(skb, kcov_common_handle());
}

static inline void *__slab_build_skb(void *data, unsigned int *size)
{
	void *resized;

	/* Must find the allocation size (and grow it to match). */
	*size = ksize(data);
	/* krealloc() will immediately return "data" when
	 * "ksize(data)" is requested: it is the existing upper
	 * bounds. As a result, GFP_ATOMIC will be ignored. Note
	 * that this "new" pointer needs to be passed back to the
	 * caller for use so the __alloc_size hinting will be
	 * tracked correctly.
	 */
	resized = krealloc(data, *size, GFP_ATOMIC);
	WARN_ON_ONCE(resized != data);
	return resized;
}

/* build_skb() variant which can operate on slab buffers.
 * Note that this should be used sparingly as slab buffers
 * cannot be combined efficiently by GRO!
 */
struct sk_buff *slab_build_skb(void *data)
{
	struct sk_buff *skb;
	unsigned int size;

	skb = kmem_cache_alloc(net_hotdata.skbuff_cache,
			       GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!skb))
		return NULL;

	memset(skb, 0, offsetof(struct sk_buff, tail));
	data = __slab_build_skb(data, &size);
	__finalize_skb_around(skb, data, size);

	return skb;
}
EXPORT_SYMBOL(slab_build_skb);

/* Caller must provide SKB that is memset cleared */
static void __build_skb_around(struct sk_buff *skb, void *data,
			       unsigned int frag_size)
{
	unsigned int size = frag_size;

	/* frag_size == 0 is considered deprecated now. Callers
	 * using slab buffer should use slab_build_skb() instead.
	 */
	if (WARN_ONCE(size == 0, "Use slab_build_skb() instead"))
		data = __slab_build_skb(data, &size);

	__finalize_skb_around(skb, data, size);
}

/**
 * __build_skb - build a network buffer
 * @data: data buffer provided by caller
 * @frag_size: size of data (must not be 0)
 *
 * Allocate a new &sk_buff. Caller provides space holding head and
 * skb_shared_info. @data must have been allocated from the page
 * allocator or vmalloc(). (A @frag_size of 0 to indicate a kmalloc()
 * allocation is deprecated, and callers should use slab_build_skb()
 * instead.)
 * The return is the new skb buffer.
 * On a failure the return is %NULL, and @data is not freed.
 * Notes :
 *  Before IO, driver allocates only data buffer where NIC put incoming frame
 *  Driver should add room at head (NET_SKB_PAD) and
 *  MUST add room at tail (SKB_DATA_ALIGN(skb_shared_info))
 *  After IO, driver calls build_skb(), to allocate sk_buff and populate it
 *  before giving packet to stack.
 *  RX rings only contains data buffers, not full skbs.
 */
struct sk_buff *__build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb;

	skb = kmem_cache_alloc(net_hotdata.skbuff_cache,
			       GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!skb))
		return NULL;

	memset(skb, 0, offsetof(struct sk_buff, tail));
	__build_skb_around(skb, data, frag_size);

	return skb;
}

/* build_skb() is wrapper over __build_skb(), that specifically
 * takes care of skb->head and skb->pfmemalloc
 */
struct sk_buff *build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb = __build_skb(data, frag_size);

	if (likely(skb && frag_size)) {
		skb->head_frag = 1;
		skb_propagate_pfmemalloc(virt_to_head_page(data), skb);
	}
	return skb;
}
EXPORT_SYMBOL(build_skb);

/**
 * build_skb_around - build a network buffer around provided skb
 * @skb: sk_buff provide by caller, must be memset cleared
 * @data: data buffer provided by caller
 * @frag_size: size of data
 */
struct sk_buff *build_skb_around(struct sk_buff *skb,
				 void *data, unsigned int frag_size)
{
	if (unlikely(!skb))
		return NULL;

	__build_skb_around(skb, data, frag_size);

	if (frag_size) {
		skb->head_frag = 1;
		skb_propagate_pfmemalloc(virt_to_head_page(data), skb);
	}
	return skb;
}
EXPORT_SYMBOL(build_skb_around);

/**
 * __napi_build_skb - build a network buffer
 * @data: data buffer provided by caller
 * @frag_size: size of data
 *
 * Version of __build_skb() that uses NAPI percpu caches to obtain
 * skbuff_head instead of inplace allocation.
 *
 * Returns a new &sk_buff on success, %NULL on allocation failure.
 */
static struct sk_buff *__napi_build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb;

	skb = napi_skb_cache_get();
	if (unlikely(!skb))
		return NULL;

	memset(skb, 0, offsetof(struct sk_buff, tail));
	__build_skb_around(skb, data, frag_size);

	return skb;
}

/**
 * napi_build_skb - build a network buffer
 * @data: data buffer provided by caller
 * @frag_size: size of data
 *
 * Version of __napi_build_skb() that takes care of skb->head_frag
 * and skb->pfmemalloc when the data is a page or page fragment.
 *
 * Returns a new &sk_buff on success, %NULL on allocation failure.
 */
struct sk_buff *napi_build_skb(void *data, unsigned int frag_size)
{
	struct sk_buff *skb = __napi_build_skb(data, frag_size);

	if (likely(skb) && frag_size) {
		skb->head_frag = 1;
		skb_propagate_pfmemalloc(virt_to_head_page(data), skb);
	}

	return skb;
}
EXPORT_SYMBOL(napi_build_skb);

/*
 * kmalloc_reserve is a wrapper around kmalloc_node_track_caller that tells
 * the caller if emergency pfmemalloc reserves are being used. If it is and
 * the socket is later found to be SOCK_MEMALLOC then PFMEMALLOC reserves
 * may be used. Otherwise, the packet data may be discarded until enough
 * memory is free
 */
static void *kmalloc_reserve(unsigned int *size, gfp_t flags, int node,
			     bool *pfmemalloc)
{
	bool ret_pfmemalloc = false;
	size_t obj_size;
	void *obj;

	obj_size = SKB_HEAD_ALIGN(*size);
	if (obj_size <= SKB_SMALL_HEAD_CACHE_SIZE &&
	    !(flags & KMALLOC_NOT_NORMAL_BITS)) {
		obj = kmem_cache_alloc_node(net_hotdata.skb_small_head_cache,
				flags | __GFP_NOMEMALLOC | __GFP_NOWARN,
				node);
		*size = SKB_SMALL_HEAD_CACHE_SIZE;
		if (obj || !(gfp_pfmemalloc_allowed(flags)))
			goto out;
		/* Try again but now we are using pfmemalloc reserves */
		ret_pfmemalloc = true;
		obj = kmem_cache_alloc_node(net_hotdata.skb_small_head_cache, flags, node);
		goto out;
	}

	obj_size = kmalloc_size_roundup(obj_size);
	/* The following cast might truncate high-order bits of obj_size, this
	 * is harmless because kmalloc(obj_size >= 2^32) will fail anyway.
	 */
	*size = (unsigned int)obj_size;

	/*
	 * Try a regular allocation, when that fails and we're not entitled
	 * to the reserves, fail.
	 */
	obj = kmalloc_node_track_caller(obj_size,
					flags | __GFP_NOMEMALLOC | __GFP_NOWARN,
					node);
	if (obj || !(gfp_pfmemalloc_allowed(flags)))
		goto out;

	/* Try again but now we are using pfmemalloc reserves */
	ret_pfmemalloc = true;
	obj = kmalloc_node_track_caller(obj_size, flags, node);

out:
	if (pfmemalloc)
		*pfmemalloc = ret_pfmemalloc;

	return obj;
}

/* 	Allocate a new skbuff. We do this ourselves so we can fill in a few
 *	'private' fields and also do memory statistics to find all the
 *	[BEEP] leaks.
 *
 */

/**
 *	__alloc_skb	-	allocate a network buffer
 *	@size: size to allocate
 *	@gfp_mask: allocation mask
 *	@flags: If SKB_ALLOC_FCLONE is set, allocate from fclone cache
 *		instead of head cache and allocate a cloned (child) skb.
 *		If SKB_ALLOC_RX is set, __GFP_MEMALLOC will be used for
 *		allocations in case the data is required for writeback
 *	@node: numa node to allocate memory on
 *
 *	Allocate a new &sk_buff. The returned buffer has no headroom and a
 *	tail room of at least size bytes. The object has a reference count
 *	of one. The return is the buffer. On a failure the return is %NULL.
 *
 *	Buffers may only be allocated from interrupts using a @gfp_mask of
 *	%GFP_ATOMIC.
 */
struct sk_buff *__alloc_skb(unsigned int size, gfp_t gfp_mask,
			    int flags, int node)
{
	struct kmem_cache *cache;
	struct sk_buff *skb;
	bool pfmemalloc;
	u8 *data;

	cache = (flags & SKB_ALLOC_FCLONE)
		? net_hotdata.skbuff_fclone_cache : net_hotdata.skbuff_cache;

	if (sk_memalloc_socks() && (flags & SKB_ALLOC_RX))
		gfp_mask |= __GFP_MEMALLOC;

	/* Get the HEAD */
	if ((flags & (SKB_ALLOC_FCLONE | SKB_ALLOC_NAPI)) == SKB_ALLOC_NAPI &&
	    likely(node == NUMA_NO_NODE || node == numa_mem_id()))
		skb = napi_skb_cache_get();
	else
		skb = kmem_cache_alloc_node(cache, gfp_mask & ~GFP_DMA, node);
	if (unlikely(!skb))
		return NULL;
	prefetchw(skb);

	/* We do our best to align skb_shared_info on a separate cache
	 * line. It usually works because kmalloc(X > SMP_CACHE_BYTES) gives
	 * aligned memory blocks, unless SLUB/SLAB debug is enabled.
	 * Both skb->head and skb_shared_info are cache line aligned.
	 */
	data = kmalloc_reserve(&size, gfp_mask, node, &pfmemalloc);
	if (unlikely(!data))
		goto nodata;
	/* kmalloc_size_roundup() might give us more room than requested.
	 * Put skb_shared_info exactly at the end of allocated zone,
	 * to allow max possible filling before reallocation.
	 */
	prefetchw(data + SKB_WITH_OVERHEAD(size));

	/*
	 * Only clear those fields we need to clear, not those that we will
	 * actually initialise below. Hence, don't put any more fields after
	 * the tail pointer in struct sk_buff!
	 */
	memset(skb, 0, offsetof(struct sk_buff, tail));
	__build_skb_around(skb, data, size);
	skb->pfmemalloc = pfmemalloc;

	if (flags & SKB_ALLOC_FCLONE) {
		struct sk_buff_fclones *fclones;

		fclones = container_of(skb, struct sk_buff_fclones, skb1);

		skb->fclone = SKB_FCLONE_ORIG;
		refcount_set(&fclones->fclone_ref, 1);
	}

	return skb;

nodata:
	kmem_cache_free(cache, skb);
	return NULL;
}
EXPORT_SYMBOL(__alloc_skb);

/**
 *	__netdev_alloc_skb - allocate an skbuff for rx on a specific device
 *	@dev: network device to receive on
 *	@len: length to allocate
 *	@gfp_mask: get_free_pages mask, passed to alloc_skb
 *
 *	Allocate a new &sk_buff and assign it a usage count of one. The
 *	buffer has NET_SKB_PAD headroom built in. Users should allocate
 *	the headroom they think they need without accounting for the
 *	built in space. The built in space is used for optimisations.
 *
 *	%NULL is returned if there is no free memory.
 */
struct sk_buff *__netdev_alloc_skb(struct net_device *dev, unsigned int len,
				   gfp_t gfp_mask)
{
	struct page_frag_cache *nc;
	struct sk_buff *skb;
	bool pfmemalloc;
	void *data;

	len += NET_SKB_PAD;

	/* If requested length is either too small or too big,
	 * we use kmalloc() for skb->head allocation.
	 */
	if (len <= SKB_WITH_OVERHEAD(SKB_SMALL_HEAD_CACHE_SIZE) ||
	    len > SKB_WITH_OVERHEAD(PAGE_SIZE) ||
	    (gfp_mask & (__GFP_DIRECT_RECLAIM | GFP_DMA))) {
		skb = __alloc_skb(len, gfp_mask, SKB_ALLOC_RX, NUMA_NO_NODE);
		if (!skb)
			goto skb_fail;
		goto skb_success;
	}

	len = SKB_HEAD_ALIGN(len);

	if (sk_memalloc_socks())
		gfp_mask |= __GFP_MEMALLOC;

	if (in_hardirq() || irqs_disabled()) {
		nc = this_cpu_ptr(&netdev_alloc_cache);
		data = page_frag_alloc(nc, len, gfp_mask);
		pfmemalloc = page_frag_cache_is_pfmemalloc(nc);
	} else {
		local_bh_disable();
		local_lock_nested_bh(&napi_alloc_cache.bh_lock);

		nc = this_cpu_ptr(&napi_alloc_cache.page);
		data = page_frag_alloc(nc, len, gfp_mask);
		pfmemalloc = page_frag_cache_is_pfmemalloc(nc);

		local_unlock_nested_bh(&napi_alloc_cache.bh_lock);
		local_bh_enable();
	}

	if (unlikely(!data))
		return NULL;

	skb = __build_skb(data, len);
	if (unlikely(!skb)) {
		skb_free_frag(data);
		return NULL;
	}

	if (pfmemalloc)
		skb->pfmemalloc = 1;
	skb->head_frag = 1;

skb_success:
	skb_reserve(skb, NET_SKB_PAD);
	skb->dev = dev;

skb_fail:
	return skb;
}
EXPORT_SYMBOL(__netdev_alloc_skb);

/**
 *	napi_alloc_skb - allocate skbuff for rx in a specific NAPI instance
 *	@napi: napi instance this buffer was allocated for
 *	@len: length to allocate
 *
 *	Allocate a new sk_buff for use in NAPI receive.  This buffer will
 *	attempt to allocate the head from a special reserved region used
 *	only for NAPI Rx allocation.  By doing this we can save several
 *	CPU cycles by avoiding having to disable and re-enable IRQs.
 *
 *	%NULL is returned if there is no free memory.
 */
struct sk_buff *napi_alloc_skb(struct napi_struct *napi, unsigned int len)
{
	gfp_t gfp_mask = GFP_ATOMIC | __GFP_NOWARN;
	struct napi_alloc_cache *nc;
	struct sk_buff *skb;
	bool pfmemalloc;
	void *data;

	DEBUG_NET_WARN_ON_ONCE(!in_softirq());
	len += NET_SKB_PAD + NET_IP_ALIGN;

	/* If requested length is either too small or too big,
	 * we use kmalloc() for skb->head allocation.
	 */
	if (len <= SKB_WITH_OVERHEAD(SKB_SMALL_HEAD_CACHE_SIZE) ||
	    len > SKB_WITH_OVERHEAD(PAGE_SIZE) ||
	    (gfp_mask & (__GFP_DIRECT_RECLAIM | GFP_DMA))) {
		skb = __alloc_skb(len, gfp_mask, SKB_ALLOC_RX | SKB_ALLOC_NAPI,
				  NUMA_NO_NODE);
		if (!skb)
			goto skb_fail;
		goto skb_success;
	}

	len = SKB_HEAD_ALIGN(len);

	if (sk_memalloc_socks())
		gfp_mask |= __GFP_MEMALLOC;

	local_lock_nested_bh(&napi_alloc_cache.bh_lock);
	nc = this_cpu_ptr(&napi_alloc_cache);

	data = page_frag_alloc(&nc->page, len, gfp_mask);
	pfmemalloc = page_frag_cache_is_pfmemalloc(&nc->page);
	local_unlock_nested_bh(&napi_alloc_cache.bh_lock);

	if (unlikely(!data))
		return NULL;

	skb = __napi_build_skb(data, len);
	if (unlikely(!skb)) {
		skb_free_frag(data);
		return NULL;
	}

	if (pfmemalloc)
		skb->pfmemalloc = 1;
	skb->head_frag = 1;

skb_success:
	skb_reserve(skb, NET_SKB_PAD + NET_IP_ALIGN);
	skb->dev = napi->dev;

skb_fail:
	return skb;
}
EXPORT_SYMBOL(napi_alloc_skb);

void skb_add_rx_frag_netmem(struct sk_buff *skb, int i, netmem_ref netmem,
			    int off, int size, unsigned int truesize)
{
	DEBUG_NET_WARN_ON_ONCE(size > truesize);

	skb_fill_netmem_desc(skb, i, netmem, off, size);
	skb->len += size;
	skb->data_len += size;
	skb->truesize += truesize;
}
EXPORT_SYMBOL(skb_add_rx_frag_netmem);

void skb_coalesce_rx_frag(struct sk_buff *skb, int i, int size,
			  unsigned int truesize)
{
	skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

	DEBUG_NET_WARN_ON_ONCE(size > truesize);

	skb_frag_size_add(frag, size);
	skb->len += size;
	skb->data_len += size;
	skb->truesize += truesize;
}
EXPORT_SYMBOL(skb_coalesce_rx_frag);

static void skb_drop_list(struct sk_buff **listp)
{
	kfree_skb_list(*listp);
	*listp = NULL;
}

static inline void skb_drop_fraglist(struct sk_buff *skb)
{
	skb_drop_list(&skb_shinfo(skb)->frag_list);
}

static void skb_clone_fraglist(struct sk_buff *skb)
{
	struct sk_buff *list;

	skb_walk_frags(skb, list)
		skb_get(list);
}

int skb_pp_cow_data(struct page_pool *pool, struct sk_buff **pskb,
		    unsigned int headroom)
{
#if IS_ENABLED(CONFIG_PAGE_POOL)
	u32 size, truesize, len, max_head_size, off;
	struct sk_buff *skb = *pskb, *nskb;
	int err, i, head_off;
	void *data;

	/* XDP does not support fraglist so we need to linearize
	 * the skb.
	 */
	if (skb_has_frag_list(skb))
		return -EOPNOTSUPP;

	max_head_size = SKB_WITH_OVERHEAD(PAGE_SIZE - headroom);
	if (skb->len > max_head_size + MAX_SKB_FRAGS * PAGE_SIZE)
		return -ENOMEM;

	size = min_t(u32, skb->len, max_head_size);
	truesize = SKB_HEAD_ALIGN(size) + headroom;
	data = page_pool_dev_alloc_va(pool, &truesize);
	if (!data)
		return -ENOMEM;

	nskb = napi_build_skb(data, truesize);
	if (!nskb) {
		page_pool_free_va(pool, data, true);
		return -ENOMEM;
	}

	skb_reserve(nskb, headroom);
	skb_copy_header(nskb, skb);
	skb_mark_for_recycle(nskb);

	err = skb_copy_bits(skb, 0, nskb->data, size);
	if (err) {
		consume_skb(nskb);
		return err;
	}
	skb_put(nskb, size);

	head_off = skb_headroom(nskb) - skb_headroom(skb);
	skb_headers_offset_update(nskb, head_off);

	off = size;
	len = skb->len - off;
	for (i = 0; i < MAX_SKB_FRAGS && off < skb->len; i++) {
		struct page *page;
		u32 page_off;

		size = min_t(u32, len, PAGE_SIZE);
		truesize = size;

		page = page_pool_dev_alloc(pool, &page_off, &truesize);
		if (!page) {
			consume_skb(nskb);
			return -ENOMEM;
		}

		skb_add_rx_frag(nskb, i, page, page_off, size, truesize);
		err = skb_copy_bits(skb, off, page_address(page) + page_off,
				    size);
		if (err) {
			consume_skb(nskb);
			return err;
		}

		len -= size;
		off += size;
	}

	consume_skb(skb);
	*pskb = nskb;

	return 0;
#else
	return -EOPNOTSUPP;
#endif
}
EXPORT_SYMBOL(skb_pp_cow_data);

int skb_cow_data_for_xdp(struct page_pool *pool, struct sk_buff **pskb,
			 const struct bpf_prog *prog)
{
	if (!prog->aux->xdp_has_frags)
		return -EINVAL;

	return skb_pp_cow_data(pool, pskb, XDP_PACKET_HEADROOM);
}
EXPORT_SYMBOL(skb_cow_data_for_xdp);

#if IS_ENABLED(CONFIG_PAGE_POOL)
bool napi_pp_put_page(netmem_ref netmem)
{
	netmem = netmem_compound_head(netmem);

	if (unlikely(!netmem_is_pp(netmem)))
		return false;

	page_pool_put_full_netmem(netmem_get_pp(netmem), netmem, false);

	return true;
}
EXPORT_SYMBOL(napi_pp_put_page);
#endif

static bool skb_pp_recycle(struct sk_buff *skb, void *data)
{
	if (!IS_ENABLED(CONFIG_PAGE_POOL) || !skb->pp_recycle)
		return false;
	return napi_pp_put_page(page_to_netmem(virt_to_page(data)));
}

/**
 * skb_pp_frag_ref() - Increase fragment references of a page pool aware skb
 * @skb:	page pool aware skb
 *
 * Increase the fragment reference count (pp_ref_count) of a skb. This is
 * intended to gain fragment references only for page pool aware skbs,
 * i.e. when skb->pp_recycle is true, and not for fragments in a
 * non-pp-recycling skb. It has a fallback to increase references on normal
 * pages, as page pool aware skbs may also have normal page fragments.
 */
static int skb_pp_frag_ref(struct sk_buff *skb)
{
	struct skb_shared_info *shinfo;
	netmem_ref head_netmem;
	int i;

	if (!skb->pp_recycle)
		return -EINVAL;

	shinfo = skb_shinfo(skb);

	for (i = 0; i < shinfo->nr_frags; i++) {
		head_netmem = netmem_compound_head(shinfo->frags[i].netmem);
		if (likely(netmem_is_pp(head_netmem)))
			page_pool_ref_netmem(head_netmem);
		else
			page_ref_inc(netmem_to_page(head_netmem));
	}
	return 0;
}

static void skb_kfree_head(void *head, unsigned int end_offset)
{
	if (end_offset == SKB_SMALL_HEAD_HEADROOM)
		kmem_cache_free(net_hotdata.skb_small_head_cache, head);
	else
		kfree(head);
}

static void skb_free_head(struct sk_buff *skb)
{
	unsigned char *head = skb->head;

	if (skb->head_frag) {
		if (skb_pp_recycle(skb, head))
			return;
		skb_free_frag(head);
	} else {
		skb_kfree_head(head, skb_end_offset(skb));
	}
}

static void skb_release_data(struct sk_buff *skb, enum skb_drop_reason reason)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int i;

	if (!skb_data_unref(skb, shinfo))
		goto exit;

	if (skb_zcopy(skb)) {
		bool skip_unref = shinfo->flags & SKBFL_MANAGED_FRAG_REFS;

		skb_zcopy_clear(skb, true);
		if (skip_unref)
			goto free_head;
	}

	for (i = 0; i < shinfo->nr_frags; i++)
		__skb_frag_unref(&shinfo->frags[i], skb->pp_recycle);

free_head:
	if (shinfo->frag_list)
		kfree_skb_list_reason(shinfo->frag_list, reason);

	skb_free_head(skb);
exit:
	/* When we clone an SKB we copy the reycling bit. The pp_recycle
	 * bit is only set on the head though, so in order to avoid races
	 * while trying to recycle fragments on __skb_frag_unref() we need
	 * to make one SKB responsible for triggering the recycle path.
	 * So disable the recycling bit if an SKB is cloned and we have
	 * additional references to the fragmented part of the SKB.
	 * Eventually the last SKB will have the recycling bit set and it's
	 * dataref set to 0, which will trigger the recycling
	 */
	skb->pp_recycle = 0;
}

/*
 *	Free an skbuff by memory without cleaning the state.
 */
static void kfree_skbmem(struct sk_buff *skb)
{
	struct sk_buff_fclones *fclones;

	switch (skb->fclone) {
	case SKB_FCLONE_UNAVAILABLE:
		kmem_cache_free(net_hotdata.skbuff_cache, skb);
		return;

	case SKB_FCLONE_ORIG:
		fclones = container_of(skb, struct sk_buff_fclones, skb1);

		/* We usually free the clone (TX completion) before original skb
		 * This test would have no chance to be true for the clone,
		 * while here, branch prediction will be good.
		 */
		if (refcount_read(&fclones->fclone_ref) == 1)
			goto fastpath;
		break;

	default: /* SKB_FCLONE_CLONE */
		fclones = container_of(skb, struct sk_buff_fclones, skb2);
		break;
	}
	if (!refcount_dec_and_test(&fclones->fclone_ref))
		return;
fastpath:
	kmem_cache_free(net_hotdata.skbuff_fclone_cache, fclones);
}

void skb_release_head_state(struct sk_buff *skb)
{
	skb_dst_drop(skb);
	if (skb->destructor) {
		DEBUG_NET_WARN_ON_ONCE(in_hardirq());
		skb->destructor(skb);
	}
#if IS_ENABLED(CONFIG_NF_CONNTRACK)
	nf_conntrack_put(skb_nfct(skb));
#endif
	skb_ext_put(skb);
}

/* Free everything but the sk_buff shell. */
static void skb_release_all(struct sk_buff *skb, enum skb_drop_reason reason)
{
	skb_release_head_state(skb);
	if (likely(skb->head))
		skb_release_data(skb, reason);
}

/**
 *	__kfree_skb - private function
 *	@skb: buffer
 *
 *	Free an sk_buff. Release anything attached to the buffer.
 *	Clean the state. This is an internal helper function. Users should
 *	always call kfree_skb
 */

void __kfree_skb(struct sk_buff *skb)
{
	skb_release_all(skb, SKB_DROP_REASON_NOT_SPECIFIED);
	kfree_skbmem(skb);
}
EXPORT_SYMBOL(__kfree_skb);

static __always_inline
bool __sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb,
			  enum skb_drop_reason reason)
{
	if (unlikely(!skb_unref(skb)))
		return false;

	DEBUG_NET_WARN_ON_ONCE(reason == SKB_NOT_DROPPED_YET ||
			       u32_get_bits(reason,
					    SKB_DROP_REASON_SUBSYS_MASK) >=
				SKB_DROP_REASON_SUBSYS_NUM);

	if (reason == SKB_CONSUMED)
		trace_consume_skb(skb, __builtin_return_address(0));
	else
		trace_kfree_skb(skb, __builtin_return_address(0), reason, sk);
	return true;
}

/**
 *	sk_skb_reason_drop - free an sk_buff with special reason
 *	@sk: the socket to receive @skb, or NULL if not applicable
 *	@skb: buffer to free
 *	@reason: reason why this skb is dropped
 *
 *	Drop a reference to the buffer and free it if the usage count has hit
 *	zero. Meanwhile, pass the receiving socket and drop reason to
 *	'kfree_skb' tracepoint.
 */
void __fix_address
sk_skb_reason_drop(struct sock *sk, struct sk_buff *skb, enum skb_drop_reason reason)
{
	if (__sk_skb_reason_drop(sk, skb, reason))
		__kfree_skb(skb);
}
EXPORT_SYMBOL(sk_skb_reason_drop);

#define KFREE_SKB_BULK_SIZE	16

struct skb_free_array {
	unsigned int skb_count;
	void *skb_array[KFREE_SKB_BULK_SIZE];
};

static void kfree_skb_add_bulk(struct sk_buff *skb,
			       struct skb_free_array *sa,
			       enum skb_drop_reason reason)
{
	/* if SKB is a clone, don't handle this case */
	if (unlikely(skb->fclone != SKB_FCLONE_UNAVAILABLE)) {
		__kfree_skb(skb);
		return;
	}

	skb_release_all(skb, reason);
	sa->skb_array[sa->skb_count++] = skb;

	if (unlikely(sa->skb_count == KFREE_SKB_BULK_SIZE)) {
		kmem_cache_free_bulk(net_hotdata.skbuff_cache, KFREE_SKB_BULK_SIZE,
				     sa->skb_array);
		sa->skb_count = 0;
	}
}

void __fix_address
kfree_skb_list_reason(struct sk_buff *segs, enum skb_drop_reason reason)
{
	struct skb_free_array sa;

	sa.skb_count = 0;

	while (segs) {
		struct sk_buff *next = segs->next;

		if (__sk_skb_reason_drop(NULL, segs, reason)) {
			skb_poison_list(segs);
			kfree_skb_add_bulk(segs, &sa, reason);
		}

		segs = next;
	}

	if (sa.skb_count)
		kmem_cache_free_bulk(net_hotdata.skbuff_cache, sa.skb_count, sa.skb_array);
}
EXPORT_SYMBOL(kfree_skb_list_reason);

/* Dump skb information and contents.
 *
 * Must only be called from net_ratelimit()-ed paths.
 *
 * Dumps whole packets if full_pkt, only headers otherwise.
 */
void skb_dump(const char *level, const struct sk_buff *skb, bool full_pkt)
{
	struct skb_shared_info *sh = skb_shinfo(skb);
	struct net_device *dev = skb->dev;
	struct sock *sk = skb->sk;
	struct sk_buff *list_skb;
	bool has_mac, has_trans;
	int headroom, tailroom;
	int i, len, seg_len;

	if (full_pkt)
		len = skb->len;
	else
		len = min_t(int, skb->len, MAX_HEADER + 128);

	headroom = skb_headroom(skb);
	tailroom = skb_tailroom(skb);

	has_mac = skb_mac_header_was_set(skb);
	has_trans = skb_transport_header_was_set(skb);

	printk("%sskb len=%u headroom=%u headlen=%u tailroom=%u\n"
	       "mac=(%d,%d) mac_len=%u net=(%d,%d) trans=%d\n"
	       "shinfo(txflags=%u nr_frags=%u gso(size=%hu type=%u segs=%hu))\n"
	       "csum(0x%x start=%u offset=%u ip_summed=%u complete_sw=%u valid=%u level=%u)\n"
	       "hash(0x%x sw=%u l4=%u) proto=0x%04x pkttype=%u iif=%d\n"
	       "priority=0x%x mark=0x%x alloc_cpu=%u vlan_all=0x%x\n"
	       "encapsulation=%d inner(proto=0x%04x, mac=%u, net=%u, trans=%u)\n",
	       level, skb->len, headroom, skb_headlen(skb), tailroom,
	       has_mac ? skb->mac_header : -1,
	       has_mac ? skb_mac_header_len(skb) : -1,
	       skb->mac_len,
	       skb->network_header,
	       has_trans ? skb_network_header_len(skb) : -1,
	       has_trans ? skb->transport_header : -1,
	       sh->tx_flags, sh->nr_frags,
	       sh->gso_size, sh->gso_type, sh->gso_segs,
	       skb->csum, skb->csum_start, skb->csum_offset, skb->ip_summed,
	       skb->csum_complete_sw, skb->csum_valid, skb->csum_level,
	       skb->hash, skb->sw_hash, skb->l4_hash,
	       ntohs(skb->protocol), skb->pkt_type, skb->skb_iif,
	       skb->priority, skb->mark, skb->alloc_cpu, skb->vlan_all,
	       skb->encapsulation, skb->inner_protocol, skb->inner_mac_header,
	       skb->inner_network_header, skb->inner_transport_header);

	if (dev)
		printk("%sdev name=%s feat=%pNF\n",
		       level, dev->name, &dev->features);
	if (sk)
		printk("%ssk family=%hu type=%u proto=%u\n",
		       level, sk->sk_family, sk->sk_type, sk->sk_protocol);

	if (full_pkt && headroom)
		print_hex_dump(level, "skb headroom: ", DUMP_PREFIX_OFFSET,
			       16, 1, skb->head, headroom, false);

	seg_len = min_t(int, skb_headlen(skb), len);
	if (seg_len)
		print_hex_dump(level, "skb linear:   ", DUMP_PREFIX_OFFSET,
			       16, 1, skb->data, seg_len, false);
	len -= seg_len;

	if (full_pkt && tailroom)
		print_hex_dump(level, "skb tailroom: ", DUMP_PREFIX_OFFSET,
			       16, 1, skb_tail_pointer(skb), tailroom, false);

	for (i = 0; len && i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		u32 p_off, p_len, copied;
		struct page *p;
		u8 *vaddr;

		if (skb_frag_is_net_iov(frag)) {
			printk("%sskb frag %d: not readable\n", level, i);
			len -= skb_frag_size(frag);
			if (!len)
				break;
			continue;
		}

		skb_frag_foreach_page(frag, skb_frag_off(frag),
				      skb_frag_size(frag), p, p_off, p_len,
				      copied) {
			seg_len = min_t(int, p_len, len);
			vaddr = kmap_atomic(p);
			print_hex_dump(level, "skb frag:     ",
				       DUMP_PREFIX_OFFSET,
				       16, 1, vaddr + p_off, seg_len, false);
			kunmap_atomic(vaddr);
			len -= seg_len;
			if (!len)
				break;
		}
	}

	if (full_pkt && skb_has_frag_list(skb)) {
		printk("skb fraglist:\n");
		skb_walk_frags(skb, list_skb)
			skb_dump(level, list_skb, true);
	}
}
EXPORT_SYMBOL(skb_dump);

/**
 *	skb_tx_error - report an sk_buff xmit error
 *	@skb: buffer that triggered an error
 *
 *	Report xmit error if a device callback is tracking this skb.
 *	skb must be freed afterwards.
 */
void skb_tx_error(struct sk_buff *skb)
{
	if (skb) {
		skb_zcopy_downgrade_managed(skb);
		skb_zcopy_clear(skb, true);
	}
}
EXPORT_SYMBOL(skb_tx_error);

#ifdef CONFIG_TRACEPOINTS
/**
 *	consume_skb - free an skbuff
 *	@skb: buffer to free
 *
 *	Drop a ref to the buffer and free it if the usage count has hit zero
 *	Functions identically to kfree_skb, but kfree_skb assumes that the frame
 *	is being dropped after a failure and notes that
 */
void consume_skb(struct sk_buff *skb)
{
	if (!skb_unref(skb))
		return;

	trace_consume_skb(skb, __builtin_return_address(0));
	__kfree_skb(skb);
}
EXPORT_SYMBOL(consume_skb);
#endif

/**
 *	__consume_stateless_skb - free an skbuff, assuming it is stateless
 *	@skb: buffer to free
 *
 *	Alike consume_skb(), but this variant assumes that this is the last
 *	skb reference and all the head states have been already dropped
 */
void __consume_stateless_skb(struct sk_buff *skb)
{
	trace_consume_skb(skb, __builtin_return_address(0));
	skb_release_data(skb, SKB_CONSUMED);
	kfree_skbmem(skb);
}

static void napi_skb_cache_put(struct sk_buff *skb)
{
	struct napi_alloc_cache *nc = this_cpu_ptr(&napi_alloc_cache);
	u32 i;

	if (!kasan_mempool_poison_object(skb))
		return;

	local_lock_nested_bh(&napi_alloc_cache.bh_lock);
	nc->skb_cache[nc->skb_count++] = skb;

	if (unlikely(nc->skb_count == NAPI_SKB_CACHE_SIZE)) {
		for (i = NAPI_SKB_CACHE_HALF; i < NAPI_SKB_CACHE_SIZE; i++)
			kasan_mempool_unpoison_object(nc->skb_cache[i],
						kmem_cache_size(net_hotdata.skbuff_cache));

		kmem_cache_free_bulk(net_hotdata.skbuff_cache, NAPI_SKB_CACHE_HALF,
				     nc->skb_cache + NAPI_SKB_CACHE_HALF);
		nc->skb_count = NAPI_SKB_CACHE_HALF;
	}
	local_unlock_nested_bh(&napi_alloc_cache.bh_lock);
}

void __napi_kfree_skb(struct sk_buff *skb, enum skb_drop_reason reason)
{
	skb_release_all(skb, reason);
	napi_skb_cache_put(skb);
}

void napi_skb_free_stolen_head(struct sk_buff *skb)
{
	if (unlikely(skb->slow_gro)) {
		nf_reset_ct(skb);
		skb_dst_drop(skb);
		skb_ext_put(skb);
		skb_orphan(skb);
		skb->slow_gro = 0;
	}
	napi_skb_cache_put(skb);
}

void napi_consume_skb(struct sk_buff *skb, int budget)
{
	/* Zero budget indicate non-NAPI context called us, like netpoll */
	if (unlikely(!budget)) {
		dev_consume_skb_any(skb);
		return;
	}

	DEBUG_NET_WARN_ON_ONCE(!in_softirq());

	if (!skb_unref(skb))
		return;

	/* if reaching here SKB is ready to free */
	trace_consume_skb(skb, __builtin_return_address(0));

	/* if SKB is a clone, don't handle this case */
	if (skb->fclone != SKB_FCLONE_UNAVAILABLE) {
		__kfree_skb(skb);
		return;
	}

	skb_release_all(skb, SKB_CONSUMED);
	napi_skb_cache_put(skb);
}
EXPORT_SYMBOL(napi_consume_skb);

/* Make sure a field is contained by headers group */
#define CHECK_SKB_FIELD(field) \
	BUILD_BUG_ON(offsetof(struct sk_buff, field) !=		\
		     offsetof(struct sk_buff, headers.field));	\

static void __copy_skb_header(struct sk_buff *new, const struct sk_buff *old)
{
	new->tstamp		= old->tstamp;
	/* We do not copy old->sk */
	new->dev		= old->dev;
	memcpy(new->cb, old->cb, sizeof(old->cb));
	skb_dst_copy(new, old);
	__skb_ext_copy(new, old);
	__nf_copy(new, old, false);

	/* Note : this field could be in the headers group.
	 * It is not yet because we do not want to have a 16 bit hole
	 */
	new->queue_mapping = old->queue_mapping;

	memcpy(&new->headers, &old->headers, sizeof(new->headers));
	CHECK_SKB_FIELD(protocol);
	CHECK_SKB_FIELD(csum);
	CHECK_SKB_FIELD(hash);
	CHECK_SKB_FIELD(priority);
	CHECK_SKB_FIELD(skb_iif);
	CHECK_SKB_FIELD(vlan_proto);
	CHECK_SKB_FIELD(vlan_tci);
	CHECK_SKB_FIELD(transport_header);
	CHECK_SKB_FIELD(network_header);
	CHECK_SKB_FIELD(mac_header);
	CHECK_SKB_FIELD(inner_protocol);
	CHECK_SKB_FIELD(inner_transport_header);
	CHECK_SKB_FIELD(inner_network_header);
	CHECK_SKB_FIELD(inner_mac_header);
	CHECK_SKB_FIELD(mark);
#ifdef CONFIG_NETWORK_SECMARK
	CHECK_SKB_FIELD(secmark);
#endif
#ifdef CONFIG_NET_RX_BUSY_POLL
	CHECK_SKB_FIELD(napi_id);
#endif
	CHECK_SKB_FIELD(alloc_cpu);
#ifdef CONFIG_XPS
	CHECK_SKB_FIELD(sender_cpu);
#endif
#ifdef CONFIG_NET_SCHED
	CHECK_SKB_FIELD(tc_index);
#endif

}

/*
 * You should not add any new code to this function.  Add it to
 * __copy_skb_header above instead.
 */
static struct sk_buff *__skb_clone(struct sk_buff *n, struct sk_buff *skb)
{
#define C(x) n->x = skb->x

	n->next = n->prev = NULL;
	n->sk = NULL;
	__copy_skb_header(n, skb);

	C(len);
	C(data_len);
	C(mac_len);
	n->hdr_len = skb->nohdr ? skb_headroom(skb) : skb->hdr_len;
	n->cloned = 1;
	n->nohdr = 0;
	n->peeked = 0;
	C(pfmemalloc);
	C(pp_recycle);
	n->destructor = NULL;
	C(tail);
	C(end);
	C(head);
	C(head_frag);
	C(data);
	C(truesize);
	refcount_set(&n->users, 1);

	atomic_inc(&(skb_shinfo(skb)->dataref));
	skb->cloned = 1;

	return n;
#undef C
}

/**
 * alloc_skb_for_msg() - allocate sk_buff to wrap frag list forming a msg
 * @first: first sk_buff of the msg
 */
struct sk_buff *alloc_skb_for_msg(struct sk_buff *first)
{
	struct sk_buff *n;

	n = alloc_skb(0, GFP_ATOMIC);
	if (!n)
		return NULL;

	n->len = first->len;
	n->data_len = first->len;
	n->truesize = first->truesize;

	skb_shinfo(n)->frag_list = first;

	__copy_skb_header(n, first);
	n->destructor = NULL;

	return n;
}
EXPORT_SYMBOL_GPL(alloc_skb_for_msg);

/**
 *	skb_morph	-	morph one skb into another
 *	@dst: the skb to receive the contents
 *	@src: the skb to supply the contents
 *
 *	This is identical to skb_clone except that the target skb is
 *	supplied by the user.
 *
 *	The target skb is returned upon exit.
 */
struct sk_buff *skb_morph(struct sk_buff *dst, struct sk_buff *src)
{
	skb_release_all(dst, SKB_CONSUMED);
	return __skb_clone(dst, src);
}
EXPORT_SYMBOL_GPL(skb_morph);

int mm_account_pinned_pages(struct mmpin *mmp, size_t size)
{
	unsigned long max_pg, num_pg, new_pg, old_pg, rlim;
	struct user_struct *user;

	if (capable(CAP_IPC_LOCK) || !size)
		return 0;

	rlim = rlimit(RLIMIT_MEMLOCK);
	if (rlim == RLIM_INFINITY)
		return 0;

	num_pg = (size >> PAGE_SHIFT) + 2;	/* worst case */
	max_pg = rlim >> PAGE_SHIFT;
	user = mmp->user ? : current_user();

	old_pg = atomic_long_read(&user->locked_vm);
	do {
		new_pg = old_pg + num_pg;
		if (new_pg > max_pg)
			return -ENOBUFS;
	} while (!atomic_long_try_cmpxchg(&user->locked_vm, &old_pg, new_pg));

	if (!mmp->user) {
		mmp->user = get_uid(user);
		mmp->num_pg = num_pg;
	} else {
		mmp->num_pg += num_pg;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mm_account_pinned_pages);

void mm_unaccount_pinned_pages(struct mmpin *mmp)
{
	if (mmp->user) {
		atomic_long_sub(mmp->num_pg, &mmp->user->locked_vm);
		free_uid(mmp->user);
	}
}
EXPORT_SYMBOL_GPL(mm_unaccount_pinned_pages);

static struct ubuf_info *msg_zerocopy_alloc(struct sock *sk, size_t size,
					    bool devmem)
{
	struct ubuf_info_msgzc *uarg;
	struct sk_buff *skb;

	WARN_ON_ONCE(!in_task());

	skb = sock_omalloc(sk, 0, GFP_KERNEL);
	if (!skb)
		return NULL;

	BUILD_BUG_ON(sizeof(*uarg) > sizeof(skb->cb));
	uarg = (void *)skb->cb;
	uarg->mmp.user = NULL;

	if (likely(!devmem) && mm_account_pinned_pages(&uarg->mmp, size)) {
		kfree_skb(skb);
		return NULL;
	}

	uarg->ubuf.ops = &msg_zerocopy_ubuf_ops;
	uarg->id = ((u32)atomic_inc_return(&sk->sk_zckey)) - 1;
	uarg->len = 1;
	uarg->bytelen = size;
	uarg->zerocopy = 1;
	uarg->ubuf.flags = SKBFL_ZEROCOPY_FRAG | SKBFL_DONT_ORPHAN;
	refcount_set(&uarg->ubuf.refcnt, 1);
	sock_hold(sk);

	return &uarg->ubuf;
}

static inline struct sk_buff *skb_from_uarg(struct ubuf_info_msgzc *uarg)
{
	return container_of((void *)uarg, struct sk_buff, cb);
}

struct ubuf_info *msg_zerocopy_realloc(struct sock *sk, size_t size,
				       struct ubuf_info *uarg, bool devmem)
{
	if (uarg) {
		struct ubuf_info_msgzc *uarg_zc;
		const u32 byte_limit = 1 << 19;		/* limit to a few TSO */
		u32 bytelen, next;

		/* there might be non MSG_ZEROCOPY users */
		if (uarg->ops != &msg_zerocopy_ubuf_ops)
			return NULL;

		/* realloc only when socket is locked (TCP, UDP cork),
		 * so uarg->len and sk_zckey access is serialized
		 */
		if (!sock_owned_by_user(sk)) {
			WARN_ON_ONCE(1);
			return NULL;
		}

		uarg_zc = uarg_to_msgzc(uarg);
		bytelen = uarg_zc->bytelen + size;
		if (uarg_zc->len == USHRT_MAX - 1 || bytelen > byte_limit) {
			/* TCP can create new skb to attach new uarg */
			if (sk->sk_type == SOCK_STREAM)
				goto new_alloc;
			return NULL;
		}

		next = (u32)atomic_read(&sk->sk_zckey);
		if ((u32)(uarg_zc->id + uarg_zc->len) == next) {
			if (likely(!devmem) &&
			    mm_account_pinned_pages(&uarg_zc->mmp, size))
				return NULL;
			uarg_zc->len++;
			uarg_zc->bytelen = bytelen;
			atomic_set(&sk->sk_zckey, ++next);

			/* no extra ref when appending to datagram (MSG_MORE) */
			if (sk->sk_type == SOCK_STREAM)
				net_zcopy_get(uarg);

			return uarg;
		}
	}

new_alloc:
	return msg_zerocopy_alloc(sk, size, devmem);
}
EXPORT_SYMBOL_GPL(msg_zerocopy_realloc);

static bool skb_zerocopy_notify_extend(struct sk_buff *skb, u32 lo, u16 len)
{
	struct sock_exterr_skb *serr = SKB_EXT_ERR(skb);
	u32 old_lo, old_hi;
	u64 sum_len;

	old_lo = serr->ee.ee_info;
	old_hi = serr->ee.ee_data;
	sum_len = old_hi - old_lo + 1ULL + len;

	if (sum_len >= (1ULL << 32))
		return false;

	if (lo != old_hi + 1)
		return false;

	serr->ee.ee_data += len;
	return true;
}

static void __msg_zerocopy_callback(struct ubuf_info_msgzc *uarg)
{
	struct sk_buff *tail, *skb = skb_from_uarg(uarg);
	struct sock_exterr_skb *serr;
	struct sock *sk = skb->sk;
	struct sk_buff_head *q;
	unsigned long flags;
	bool is_zerocopy;
	u32 lo, hi;
	u16 len;

	mm_unaccount_pinned_pages(&uarg->mmp);

	/* if !len, there was only 1 call, and it was aborted
	 * so do not queue a completion notification
	 */
	if (!uarg->len || sock_flag(sk, SOCK_DEAD))
		goto release;

	len = uarg->len;
	lo = uarg->id;
	hi = uarg->id + len - 1;
	is_zerocopy = uarg->zerocopy;

	serr = SKB_EXT_ERR(skb);
	memset(serr, 0, sizeof(*serr));
	serr->ee.ee_errno = 0;
	serr->ee.ee_origin = SO_EE_ORIGIN_ZEROCOPY;
	serr->ee.ee_data = hi;
	serr->ee.ee_info = lo;
	if (!is_zerocopy)
		serr->ee.ee_code |= SO_EE_CODE_ZEROCOPY_COPIED;

	q = &sk->sk_error_queue;
	spin_lock_irqsave(&q->lock, flags);
	tail = skb_peek_tail(q);
	if (!tail || SKB_EXT_ERR(tail)->ee.ee_origin != SO_EE_ORIGIN_ZEROCOPY ||
	    !skb_zerocopy_notify_extend(tail, lo, len)) {
		__skb_queue_tail(q, skb);
		skb = NULL;
	}
	spin_unlock_irqrestore(&q->lock, flags);

	sk_error_report(sk);

release:
	consume_skb(skb);
	sock_put(sk);
}

static void msg_zerocopy_complete(struct sk_buff *skb, struct ubuf_info *uarg,
				  bool success)
{
	struct ubuf_info_msgzc *uarg_zc = uarg_to_msgzc(uarg);

	uarg_zc->zerocopy = uarg_zc->zerocopy & success;

	if (refcount_dec_and_test(&uarg->refcnt))
		__msg_zerocopy_callback(uarg_zc);
}

void msg_zerocopy_put_abort(struct ubuf_info *uarg, bool have_uref)
{
	struct sock *sk = skb_from_uarg(uarg_to_msgzc(uarg))->sk;

	atomic_dec(&sk->sk_zckey);
	uarg_to_msgzc(uarg)->len--;

	if (have_uref)
		msg_zerocopy_complete(NULL, uarg, true);
}
EXPORT_SYMBOL_GPL(msg_zerocopy_put_abort);

const struct ubuf_info_ops msg_zerocopy_ubuf_ops = {
	.complete = msg_zerocopy_complete,
};
EXPORT_SYMBOL_GPL(msg_zerocopy_ubuf_ops);

int skb_zerocopy_iter_stream(struct sock *sk, struct sk_buff *skb,
			     struct msghdr *msg, int len,
			     struct ubuf_info *uarg,
			     struct net_devmem_dmabuf_binding *binding)
{
	int err, orig_len = skb->len;

	if (uarg->ops->link_skb) {
		err = uarg->ops->link_skb(skb, uarg);
		if (err)
			return err;
	} else {
		struct ubuf_info *orig_uarg = skb_zcopy(skb);

		/* An skb can only point to one uarg. This edge case happens
		 * when TCP appends to an skb, but zerocopy_realloc triggered
		 * a new alloc.
		 */
		if (orig_uarg && uarg != orig_uarg)
			return -EEXIST;
	}

	err = __zerocopy_sg_from_iter(msg, sk, skb, &msg->msg_iter, len,
				      binding);
	if (err == -EFAULT || (err == -EMSGSIZE && skb->len == orig_len)) {
		struct sock *save_sk = skb->sk;

		/* Streams do not free skb on error. Reset to prev state. */
		iov_iter_revert(&msg->msg_iter, skb->len - orig_len);
		skb->sk = sk;
		___pskb_trim(skb, orig_len);
		skb->sk = save_sk;
		return err;
	}

	skb_zcopy_set(skb, uarg, NULL);
	return skb->len - orig_len;
}
EXPORT_SYMBOL_GPL(skb_zerocopy_iter_stream);

void __skb_zcopy_downgrade_managed(struct sk_buff *skb)
{
	int i;

	skb_shinfo(skb)->flags &= ~SKBFL_MANAGED_FRAG_REFS;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		skb_frag_ref(skb, i);
}
EXPORT_SYMBOL_GPL(__skb_zcopy_downgrade_managed);

static int skb_zerocopy_clone(struct sk_buff *nskb, struct sk_buff *orig,
			      gfp_t gfp_mask)
{
	if (skb_zcopy(orig)) {
		if (skb_zcopy(nskb)) {
			/* !gfp_mask callers are verified to !skb_zcopy(nskb) */
			if (!gfp_mask) {
				WARN_ON_ONCE(1);
				return -ENOMEM;
			}
			if (skb_uarg(nskb) == skb_uarg(orig))
				return 0;
			if (skb_copy_ubufs(nskb, GFP_ATOMIC))
				return -EIO;
		}
		skb_zcopy_set(nskb, skb_uarg(orig), NULL);
	}
	return 0;
}

/**
 *	skb_copy_ubufs	-	copy userspace skb frags buffers to kernel
 *	@skb: the skb to modify
 *	@gfp_mask: allocation priority
 *
 *	This must be called on skb with SKBFL_ZEROCOPY_ENABLE.
 *	It will copy all frags into kernel and drop the reference
 *	to userspace pages.
 *
 *	If this function is called from an interrupt gfp_mask() must be
 *	%GFP_ATOMIC.
 *
 *	Returns 0 on success or a negative error code on failure
 *	to allocate kernel memory to copy to.
 */
int skb_copy_ubufs(struct sk_buff *skb, gfp_t gfp_mask)
{
	int num_frags = skb_shinfo(skb)->nr_frags;
	struct page *page, *head = NULL;
	int i, order, psize, new_frags;
	u32 d_off;

	if (skb_shared(skb) || skb_unclone(skb, gfp_mask))
		return -EINVAL;

	if (!skb_frags_readable(skb))
		return -EFAULT;

	if (!num_frags)
		goto release;

	/* We might have to allocate high order pages, so compute what minimum
	 * page order is needed.
	 */
	order = 0;
	while ((PAGE_SIZE << order) * MAX_SKB_FRAGS < __skb_pagelen(skb))
		order++;
	psize = (PAGE_SIZE << order);

	new_frags = (__skb_pagelen(skb) + psize - 1) >> (PAGE_SHIFT + order);
	for (i = 0; i < new_frags; i++) {
		page = alloc_pages(gfp_mask | __GFP_COMP, order);
		if (!page) {
			while (head) {
				struct page *next = (struct page *)page_private(head);
				put_page(head);
				head = next;
			}
			return -ENOMEM;
		}
		set_page_private(page, (unsigned long)head);
		head = page;
	}

	page = head;
	d_off = 0;
	for (i = 0; i < num_frags; i++) {
		skb_frag_t *f = &skb_shinfo(skb)->frags[i];
		u32 p_off, p_len, copied;
		struct page *p;
		u8 *vaddr;

		skb_frag_foreach_page(f, skb_frag_off(f), skb_frag_size(f),
				      p, p_off, p_len, copied) {
			u32 copy, done = 0;
			vaddr = kmap_atomic(p);

			while (done < p_len) {
				if (d_off == psize) {
					d_off = 0;
					page = (struct page *)page_private(page);
				}
				copy = min_t(u32, psize - d_off, p_len - done);
				memcpy(page_address(page) + d_off,
				       vaddr + p_off + done, copy);
				done += copy;
				d_off += copy;
			}
			kunmap_atomic(vaddr);
		}
	}

	/* skb frags release userspace buffers */
	for (i = 0; i < num_frags; i++)
		skb_frag_unref(skb, i);

	/* skb frags point to kernel buffers */
	for (i = 0; i < new_frags - 1; i++) {
		__skb_fill_netmem_desc(skb, i, page_to_netmem(head), 0, psize);
		head = (struct page *)page_private(head);
	}
	__skb_fill_netmem_desc(skb, new_frags - 1, page_to_netmem(head), 0,
			       d_off);
	skb_shinfo(skb)->nr_frags = new_frags;

release:
	skb_zcopy_clear(skb, false);
	return 0;
}
EXPORT_SYMBOL_GPL(skb_copy_ubufs);

/**
 *	skb_clone	-	duplicate an sk_buff
 *	@skb: buffer to clone
 *	@gfp_mask: allocation priority
 *
 *	Duplicate an &sk_buff. The new one is not owned by a socket. Both
 *	copies share the same packet data but not structure. The new
 *	buffer has a reference count of 1. If the allocation fails the
 *	function returns %NULL otherwise the new buffer is returned.
 *
 *	If this function is called from an interrupt gfp_mask() must be
 *	%GFP_ATOMIC.
 */

struct sk_buff *skb_clone(struct sk_buff *skb, gfp_t gfp_mask)
{
	struct sk_buff_fclones *fclones = container_of(skb,
						       struct sk_buff_fclones,
						       skb1);
	struct sk_buff *n;

	if (skb_orphan_frags(skb, gfp_mask))
		return NULL;

	if (skb->fclone == SKB_FCLONE_ORIG &&
	    refcount_read(&fclones->fclone_ref) == 1) {
		n = &fclones->skb2;
		refcount_set(&fclones->fclone_ref, 2);
		n->fclone = SKB_FCLONE_CLONE;
	} else {
		if (skb_pfmemalloc(skb))
			gfp_mask |= __GFP_MEMALLOC;

		n = kmem_cache_alloc(net_hotdata.skbuff_cache, gfp_mask);
		if (!n)
			return NULL;

		n->fclone = SKB_FCLONE_UNAVAILABLE;
	}

	return __skb_clone(n, skb);
}
EXPORT_SYMBOL(skb_clone);

void skb_headers_offset_update(struct sk_buff *skb, int off)
{
	/* Only adjust this if it actually is csum_start rather than csum */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		skb->csum_start += off;
	/* {transport,network,mac}_header and tail are relative to skb->head */
	skb->transport_header += off;
	skb->network_header   += off;
	if (skb_mac_header_was_set(skb))
		skb->mac_header += off;
	skb->inner_transport_header += off;
	skb->inner_network_header += off;
	skb->inner_mac_header += off;
}
EXPORT_SYMBOL(skb_headers_offset_update);

void skb_copy_header(struct sk_buff *new, const struct sk_buff *old)
{
	__copy_skb_header(new, old);

	skb_shinfo(new)->gso_size = skb_shinfo(old)->gso_size;
	skb_shinfo(new)->gso_segs = skb_shinfo(old)->gso_segs;
	skb_shinfo(new)->gso_type = skb_shinfo(old)->gso_type;
}
EXPORT_SYMBOL(skb_copy_header);

static inline int skb_alloc_rx_flag(const struct sk_buff *skb)
{
	if (skb_pfmemalloc(skb))
		return SKB_ALLOC_RX;
	return 0;
}

/**
 *	skb_copy	-	create private copy of an sk_buff
 *	@skb: buffer to copy
 *	@gfp_mask: allocation priority
 *
 *	Make a copy of both an &sk_buff and its data. This is used when the
 *	caller wishes to modify the data and needs a private copy of the
 *	data to alter. Returns %NULL on failure or the pointer to the buffer
 *	on success. The returned buffer has a reference count of 1.
 *
 *	As by-product this function converts non-linear &sk_buff to linear
 *	one, so that &sk_buff becomes completely private and caller is allowed
 *	to modify all the data of returned buffer. This means that this
 *	function is not recommended for use in circumstances when only
 *	header is going to be modified. Use pskb_copy() instead.
 */

struct sk_buff *skb_copy(const struct sk_buff *skb, gfp_t gfp_mask)
{
	struct sk_buff *n;
	unsigned int size;
	int headerlen;

	if (!skb_frags_readable(skb))
		return NULL;

	if (WARN_ON_ONCE(skb_shinfo(skb)->gso_type & SKB_GSO_FRAGLIST))
		return NULL;

	headerlen = skb_headroom(skb);
	size = skb_end_offset(skb) + skb->data_len;
	n = __alloc_skb(size, gfp_mask,
			skb_alloc_rx_flag(skb), NUMA_NO_NODE);
	if (!n)
		return NULL;

	/* Set the data pointer */
	skb_reserve(n, headerlen);
	/* Set the tail pointer and length */
	skb_put(n, skb->len);

	BUG_ON(skb_copy_bits(skb, -headerlen, n->head, headerlen + skb->len));

	skb_copy_header(n, skb);
	return n;
}
EXPORT_SYMBOL(skb_copy);

/**
 *	__pskb_copy_fclone	-  create copy of an sk_buff with private head.
 *	@skb: buffer to copy
 *	@headroom: headroom of new skb
 *	@gfp_mask: allocation priority
 *	@fclone: if true allocate the copy of the skb from the fclone
 *	cache instead of the head cache; it is recommended to set this
 *	to true for the cases where the copy will likely be cloned
 *
 *	Make a copy of both an &sk_buff and part of its data, located
 *	in header. Fragmented data remain shared. This is used when
 *	the caller wishes to modify only header of &sk_buff and needs
 *	private copy of the header to alter. Returns %NULL on failure
 *	or the pointer to the buffer on success.
 *	The returned buffer has a reference count of 1.
 */

struct sk_buff *__pskb_copy_fclone(struct sk_buff *skb, int headroom,
				   gfp_t gfp_mask, bool fclone)
{
	unsigned int size = skb_headlen(skb) + headroom;
	int flags = skb_alloc_rx_flag(skb) | (fclone ? SKB_ALLOC_FCLONE : 0);
	struct sk_buff *n = __alloc_skb(size, gfp_mask, flags, NUMA_NO_NODE);

	if (!n)
		goto out;

	/* Set the data pointer */
	skb_reserve(n, headroom);
	/* Set the tail pointer and length */
	skb_put(n, skb_headlen(skb));
	/* Copy the bytes */
	skb_copy_from_linear_data(skb, n->data, n->len);

	n->truesize += skb->data_len;
	n->data_len  = skb->data_len;
	n->len	     = skb->len;

	if (skb_shinfo(skb)->nr_frags) {
		int i;

		if (skb_orphan_frags(skb, gfp_mask) ||
		    skb_zerocopy_clone(n, skb, gfp_mask)) {
			kfree_skb(n);
			n = NULL;
			goto out;
		}
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			skb_shinfo(n)->frags[i] = skb_shinfo(skb)->frags[i];
			skb_frag_ref(skb, i);
		}
		skb_shinfo(n)->nr_frags = i;
	}

	if (skb_has_frag_list(skb)) {
		skb_shinfo(n)->frag_list = skb_shinfo(skb)->frag_list;
		skb_clone_fraglist(n);
	}

	skb_copy_header(n, skb);
out:
	return n;
}
EXPORT_SYMBOL(__pskb_copy_fclone);

/**
 *	pskb_expand_head - reallocate header of &sk_buff
 *	@skb: buffer to reallocate
 *	@nhead: room to add at head
 *	@ntail: room to add at tail
 *	@gfp_mask: allocation priority
 *
 *	Expands (or creates identical copy, if @nhead and @ntail are zero)
 *	header of @skb. &sk_buff itself is not changed. &sk_buff MUST have
 *	reference count of 1. Returns zero in the case of success or error,
 *	if expansion failed. In the last case, &sk_buff is not changed.
 *
 *	All the pointers pointing into skb header may change and must be
 *	reloaded after call to this function.
 */

int pskb_expand_head(struct sk_buff *skb, int nhead, int ntail,
		     gfp_t gfp_mask)
{
	unsigned int osize = skb_end_offset(skb);
	unsigned int size = osize + nhead + ntail;
	long off;
	u8 *data;
	int i;

	BUG_ON(nhead < 0);

	BUG_ON(skb_shared(skb));

	skb_zcopy_downgrade_managed(skb);

	if (skb_pfmemalloc(skb))
		gfp_mask |= __GFP_MEMALLOC;

	data = kmalloc_reserve(&size, gfp_mask, NUMA_NO_NODE, NULL);
	if (!data)
		goto nodata;
	size = SKB_WITH_OVERHEAD(size);

	/* Copy only real data... and, alas, header. This should be
	 * optimized for the cases when header is void.
	 */
	memcpy(data + nhead, skb->head, skb_tail_pointer(skb) - skb->head);

	memcpy((struct skb_shared_info *)(data + size),
	       skb_shinfo(skb),
	       offsetof(struct skb_shared_info, frags[skb_shinfo(skb)->nr_frags]));

	/*
	 * if shinfo is shared we must drop the old head gracefully, but if it
	 * is not we can just drop the old head and let the existing refcount
	 * be since all we did is relocate the values
	 */
	if (skb_cloned(skb)) {
		if (skb_orphan_frags(skb, gfp_mask))
			goto nofrags;
		if (skb_zcopy(skb))
			refcount_inc(&skb_uarg(skb)->refcnt);
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
			skb_frag_ref(skb, i);

		if (skb_has_frag_list(skb))
			skb_clone_fraglist(skb);

		skb_release_data(skb, SKB_CONSUMED);
	} else {
		skb_free_head(skb);
	}
	off = (data + nhead) - skb->head;

	skb->head     = data;
	skb->head_frag = 0;
	skb->data    += off;

	skb_set_end_offset(skb, size);
#ifdef NET_SKBUFF_DATA_USES_OFFSET
	off           = nhead;
#endif
	skb->tail	      += off;
	skb_headers_offset_update(skb, nhead);
	skb->cloned   = 0;
	skb->hdr_len  = 0;
	skb->nohdr    = 0;
	atomic_set(&skb_shinfo(skb)->dataref, 1);

	skb_metadata_clear(skb);

	/* It is not generally safe to change skb->truesize.
	 * For the moment, we really care of rx path, or
	 * when skb is orphaned (not attached to a socket).
	 */
	if (!skb->sk || skb->destructor == sock_edemux)
		skb->truesize += size - osize;

	return 0;

nofrags:
	skb_kfree_head(data, size);
nodata:
	return -ENOMEM;
}
EXPORT_SYMBOL(pskb_expand_head);

/* Make private copy of skb with writable head and some headroom */

struct sk_buff *skb_realloc_headroom(struct sk_buff *skb, unsigned int headroom)
{
	struct sk_buff *skb2;
	int delta = headroom - skb_headroom(skb);

	if (delta <= 0)
		skb2 = pskb_copy(skb, GFP_ATOMIC);
	else {
		skb2 = skb_clone(skb, GFP_ATOMIC);
		if (skb2 && pskb_expand_head(skb2, SKB_DATA_ALIGN(delta), 0,
					     GFP_ATOMIC)) {
			kfree_skb(skb2);
			skb2 = NULL;
		}
	}
	return skb2;
}
EXPORT_SYMBOL(skb_realloc_headroom);

/* Note: We plan to rework this in linux-6.4 */
int __skb_unclone_keeptruesize(struct sk_buff *skb, gfp_t pri)
{
	unsigned int saved_end_offset, saved_truesize;
	struct skb_shared_info *shinfo;
	int res;

	saved_end_offset = skb_end_offset(skb);
	saved_truesize = skb->truesize;

	res = pskb_expand_head(skb, 0, 0, pri);
	if (res)
		return res;

	skb->truesize = saved_truesize;

	if (likely(skb_end_offset(skb) == saved_end_offset))
		return 0;

	/* We can not change skb->end if the original or new value
	 * is SKB_SMALL_HEAD_HEADROOM, as it might break skb_kfree_head().
	 */
	if (saved_end_offset == SKB_SMALL_HEAD_HEADROOM ||
	    skb_end_offset(skb) == SKB_SMALL_HEAD_HEADROOM) {
		/* We think this path should not be taken.
		 * Add a temporary trace to warn us just in case.
		 */
		pr_err_once("__skb_unclone_keeptruesize() skb_end_offset() %u -> %u\n",
			    saved_end_offset, skb_end_offset(skb));
		WARN_ON_ONCE(1);
		return 0;
	}

	shinfo = skb_shinfo(skb);

	/* We are about to change back skb->end,
	 * we need to move skb_shinfo() to its new location.
	 */
	memmove(skb->head + saved_end_offset,
		shinfo,
		offsetof(struct skb_shared_info, frags[shinfo->nr_frags]));

	skb_set_end_offset(skb, saved_end_offset);

	return 0;
}

/**
 *	skb_expand_head - reallocate header of &sk_buff
 *	@skb: buffer to reallocate
 *	@headroom: needed headroom
 *
 *	Unlike skb_realloc_headroom, this one does not allocate a new skb
 *	if possible; copies skb->sk to new skb as needed
 *	and frees original skb in case of failures.
 *
 *	It expect increased headroom and generates warning otherwise.
 */

struct sk_buff *skb_expand_head(struct sk_buff *skb, unsigned int headroom)
{
	int delta = headroom - skb_headroom(skb);
	int osize = skb_end_offset(skb);
	struct sock *sk = skb->sk;

	if (WARN_ONCE(delta <= 0,
		      "%s is expecting an increase in the headroom", __func__))
		return skb;

	delta = SKB_DATA_ALIGN(delta);
	/* pskb_expand_head() might crash, if skb is shared. */
	if (skb_shared(skb) || !is_skb_wmem(skb)) {
		struct sk_buff *nskb = skb_clone(skb, GFP_ATOMIC);

		if (unlikely(!nskb))
			goto fail;

		if (sk)
			skb_set_owner_w(nskb, sk);
		consume_skb(skb);
		skb = nskb;
	}
	if (pskb_expand_head(skb, delta, 0, GFP_ATOMIC))
		goto fail;

	if (sk && is_skb_wmem(skb)) {
		delta = skb_end_offset(skb) - osize;
		refcount_add(delta, &sk->sk_wmem_alloc);
		skb->truesize += delta;
	}
	return skb;

fail:
	kfree_skb(skb);
	return NULL;
}
EXPORT_SYMBOL(skb_expand_head);

/**
 *	skb_copy_expand	-	copy and expand sk_buff
 *	@skb: buffer to copy
 *	@newheadroom: new free bytes at head
 *	@newtailroom: new free bytes at tail
 *	@gfp_mask: allocation priority
 *
 *	Make a copy of both an &sk_buff and its data and while doing so
 *	allocate additional space.
 *
 *	This is used when the caller wishes to modify the data and needs a
 *	private copy of the data to alter as well as more space for new fields.
 *	Returns %NULL on failure or the pointer to the buffer
 *	on success. The returned buffer has a reference count of 1.
 *
 *	You must pass %GFP_ATOMIC as the allocation priority if this function
 *	is called from an interrupt.
 */
struct sk_buff *skb_copy_expand(const struct sk_buff *skb,
				int newheadroom, int newtailroom,
				gfp_t gfp_mask)
{
	/*
	 *	Allocate the copy buffer
	 */
	int head_copy_len, head_copy_off;
	struct sk_buff *n;
	int oldheadroom;

	if (!skb_frags_readable(skb))
		return NULL;

	if (WARN_ON_ONCE(skb_shinfo(skb)->gso_type & SKB_GSO_FRAGLIST))
		return NULL;

	oldheadroom = skb_headroom(skb);
	n = __alloc_skb(newheadroom + skb->len + newtailroom,
			gfp_mask, skb_alloc_rx_flag(skb),
			NUMA_NO_NODE);
	if (!n)
		return NULL;

	skb_reserve(n, newheadroom);

	/* Set the tail pointer and length */
	skb_put(n, skb->len);

	head_copy_len = oldheadroom;
	head_copy_off = 0;
	if (newheadroom <= head_copy_len)
		head_copy_len = newheadroom;
	else
		head_copy_off = newheadroom - head_copy_len;

	/* Copy the linear header and data. */
	BUG_ON(skb_copy_bits(skb, -head_copy_len, n->head + head_copy_off,
			     skb->len + head_copy_len));

	skb_copy_header(n, skb);

	skb_headers_offset_update(n, newheadroom - oldheadroom);

	return n;
}
EXPORT_SYMBOL(skb_copy_expand);

/**
 *	__skb_pad		-	zero pad the tail of an skb
 *	@skb: buffer to pad
 *	@pad: space to pad
 *	@free_on_error: free buffer on error
 *
 *	Ensure that a buffer is followed by a padding area that is zero
 *	filled. Used by network drivers which may DMA or transfer data
 *	beyond the buffer end onto the wire.
 *
 *	May return error in out of memory cases. The skb is freed on error
 *	if @free_on_error is true.
 */

int __skb_pad(struct sk_buff *skb, int pad, bool free_on_error)
{
	int err;
	int ntail;

	/* If the skbuff is non linear tailroom is always zero.. */
	if (!skb_cloned(skb) && skb_tailroom(skb) >= pad) {
		memset(skb->data+skb->len, 0, pad);
		return 0;
	}

	ntail = skb->data_len + pad - (skb->end - skb->tail);
	if (likely(skb_cloned(skb) || ntail > 0)) {
		err = pskb_expand_head(skb, 0, ntail, GFP_ATOMIC);
		if (unlikely(err))
			goto free_skb;
	}

	/* FIXME: The use of this function with non-linear skb's really needs
	 * to be audited.
	 */
	err = skb_linearize(skb);
	if (unlikely(err))
		goto free_skb;

	memset(skb->data + skb->len, 0, pad);
	return 0;

free_skb:
	if (free_on_error)
		kfree_skb(skb);
	return err;
}
EXPORT_SYMBOL(__skb_pad);

/**
 *	pskb_put - add data to the tail of a potentially fragmented buffer
 *	@skb: start of the buffer to use
 *	@tail: tail fragment of the buffer to use
 *	@len: amount of data to add
 *
 *	This function extends the used data area of the potentially
 *	fragmented buffer. @tail must be the last fragment of @skb -- or
 *	@skb itself. If this would exceed the total buffer size the kernel
 *	will panic. A pointer to the first byte of the extra data is
 *	returned.
 */

void *pskb_put(struct sk_buff *skb, struct sk_buff *tail, int len)
{
	if (tail != skb) {
		skb->data_len += len;
		skb->len += len;
	}
	return skb_put(tail, len);
}
EXPORT_SYMBOL_GPL(pskb_put);

/**
 *	skb_put - add data to a buffer
 *	@skb: buffer to use
 *	@len: amount of data to add
 *
 *	This function extends the used data area of the buffer. If this would
 *	exceed the total buffer size the kernel will panic. A pointer to the
 *	first byte of the extra data is returned.
 */
void *skb_put(struct sk_buff *skb, unsigned int len)
{
	void *tmp = skb_tail_pointer(skb);
	SKB_LINEAR_ASSERT(skb);
	skb->tail += len;
	skb->len  += len;
	if (unlikely(skb->tail > skb->end))
		skb_over_panic(skb, len, __builtin_return_address(0));
	return tmp;
}
EXPORT_SYMBOL(skb_put);

/**
 *	skb_push - add data to the start of a buffer
 *	@skb: buffer to use
 *	@len: amount of data to add
 *
 *	This function extends the used data area of the buffer at the buffer
 *	start. If this would exceed the total buffer headroom the kernel will
 *	panic. A pointer to the first byte of the extra data is returned.
 */
void *skb_push(struct sk_buff *skb, unsigned int len)
{
	skb->data -= len;
	skb->len  += len;
	if (unlikely(skb->data < skb->head))
		skb_under_panic(skb, len, __builtin_return_address(0));
	return skb->data;
}
EXPORT_SYMBOL(skb_push);

/**
 *	skb_pull - remove data from the start of a buffer
 *	@skb: buffer to use
 *	@len: amount of data to remove
 *
 *	This function removes data from the start of a buffer, returning
 *	the memory to the headroom. A pointer to the next data in the buffer
 *	is returned. Once the data has been pulled future pushes will overwrite
 *	the old data.
 */
void *skb_pull(struct sk_buff *skb, unsigned int len)
{
	return skb_pull_inline(skb, len);
}
EXPORT_SYMBOL(skb_pull);

/**
 *	skb_pull_data - remove data from the start of a buffer returning its
 *	original position.
 *	@skb: buffer to use
 *	@len: amount of data to remove
 *
 *	This function removes data from the start of a buffer, returning
 *	the memory to the headroom. A pointer to the original data in the buffer
 *	is returned after checking if there is enough data to pull. Once the
 *	data has been pulled future pushes will overwrite the old data.
 */
void *skb_pull_data(struct sk_buff *skb, size_t len)
{
	void *data = skb->data;

	if (skb->len < len)
		return NULL;

	skb_pull(skb, len);

	return data;
}
EXPORT_SYMBOL(skb_pull_data);

/**
 *	skb_trim - remove end from a buffer
 *	@skb: buffer to alter
 *	@len: new length
 *
 *	Cut the length of a buffer down by removing data from the tail. If
 *	the buffer is already under the length specified it is not modified.
 *	The skb must be linear.
 */
void skb_trim(struct sk_buff *skb, unsigned int len)
{
	if (skb->len > len)
		__skb_trim(skb, len);
}
EXPORT_SYMBOL(skb_trim);

/* Trims skb to length len. It can change skb pointers.
 */

int ___pskb_trim(struct sk_buff *skb, unsigned int len)
{
	struct sk_buff **fragp;
	struct sk_buff *frag;
	int offset = skb_headlen(skb);
	int nfrags = skb_shinfo(skb)->nr_frags;
	int i;
	int err;

	if (skb_cloned(skb) &&
	    unlikely((err = pskb_expand_head(skb, 0, 0, GFP_ATOMIC))))
		return err;

	i = 0;
	if (offset >= len)
		goto drop_pages;

	for (; i < nfrags; i++) {
		int end = offset + skb_frag_size(&skb_shinfo(skb)->frags[i]);

		if (end < len) {
			offset = end;
			continue;
		}

		skb_frag_size_set(&skb_shinfo(skb)->frags[i++], len - offset);

drop_pages:
		skb_shinfo(skb)->nr_frags = i;

		for (; i < nfrags; i++)
			skb_frag_unref(skb, i);

		if (skb_has_frag_list(skb))
			skb_drop_fraglist(skb);
		goto done;
	}

	for (fragp = &skb_shinfo(skb)->frag_list; (frag = *fragp);
	     fragp = &frag->next) {
		int end = offset + frag->len;

		if (skb_shared(frag)) {
			struct sk_buff *nfrag;

			nfrag = skb_clone(frag, GFP_ATOMIC);
			if (unlikely(!nfrag))
				return -ENOMEM;

			nfrag->next = frag->next;
			consume_skb(frag);
			frag = nfrag;
			*fragp = frag;
		}

		if (end < len) {
			offset = end;
			continue;
		}

		if (end > len &&
		    unlikely((err = pskb_trim(frag, len - offset))))
			return err;

		if (frag->next)
			skb_drop_list(&frag->next);
		break;
	}

done:
	if (len > skb_headlen(skb)) {
		skb->data_len -= skb->len - len;
		skb->len       = len;
	} else {
		skb->len       = len;
		skb->data_len  = 0;
		skb_set_tail_pointer(skb, len);
	}

	if (!skb->sk || skb->destructor == sock_edemux)
		skb_condense(skb);
	return 0;
}
EXPORT_SYMBOL(___pskb_trim);

/* Note : use pskb_trim_rcsum() instead of calling this directly
 */
int pskb_trim_rcsum_slow(struct sk_buff *skb, unsigned int len)
{
	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		int delta = skb->len - len;

		skb->csum = csum_block_sub(skb->csum,
					   skb_checksum(skb, len, delta, 0),
					   len);
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		int hdlen = (len > skb_headlen(skb)) ? skb_headlen(skb) : len;
		int offset = skb_checksum_start_offset(skb) + skb->csum_offset;

		if (offset + sizeof(__sum16) > hdlen)
			return -EINVAL;
	}
	return __pskb_trim(skb, len);
}
EXPORT_SYMBOL(pskb_trim_rcsum_slow);

/**
 *	__pskb_pull_tail - advance tail of skb header
 *	@skb: buffer to reallocate
 *	@delta: number of bytes to advance tail
 *
 *	The function makes a sense only on a fragmented &sk_buff,
 *	it expands header moving its tail forward and copying necessary
 *	data from fragmented part.
 *
 *	&sk_buff MUST have reference count of 1.
 *
 *	Returns %NULL (and &sk_buff does not change) if pull failed
 *	or value of new tail of skb in the case of success.
 *
 *	All the pointers pointing into skb header may change and must be
 *	reloaded after call to this function.
 */

/* Moves tail of skb head forward, copying data from fragmented part,
 * when it is necessary.
 * 1. It may fail due to malloc failure.
 * 2. It may change skb pointers.
 *
 * It is pretty complicated. Luckily, it is called only in exceptional cases.
 */
void *__pskb_pull_tail(struct sk_buff *skb, int delta)
{
	/* If skb has not enough free space at tail, get new one
	 * plus 128 bytes for future expansions. If we have enough
	 * room at tail, reallocate without expansion only if skb is cloned.
	 */
	int i, k, eat = (skb->tail + delta) - skb->end;

	if (!skb_frags_readable(skb))
		return NULL;

	if (eat > 0 || skb_cloned(skb)) {
		if (pskb_expand_head(skb, 0, eat > 0 ? eat + 128 : 0,
				     GFP_ATOMIC))
			return NULL;
	}

	BUG_ON(skb_copy_bits(skb, skb_headlen(skb),
			     skb_tail_pointer(skb), delta));

	/* Optimization: no fragments, no reasons to preestimate
	 * size of pulled pages. Superb.
	 */
	if (!skb_has_frag_list(skb))
		goto pull_pages;

	/* Estimate size of pulled pages. */
	eat = delta;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int size = skb_frag_size(&skb_shinfo(skb)->frags[i]);

		if (size >= eat)
			goto pull_pages;
		eat -= size;
	}

	/* If we need update frag list, we are in troubles.
	 * Certainly, it is possible to add an offset to skb data,
	 * but taking into account that pulling is expected to
	 * be very rare operation, it is worth to fight against
	 * further bloating skb head and crucify ourselves here instead.
	 * Pure masohism, indeed. 8)8)
	 */
	if (eat) {
		struct sk_buff *list = skb_shinfo(skb)->frag_list;
		struct sk_buff *clone = NULL;
		struct sk_buff *insp = NULL;

		do {
			if (list->len <= eat) {
				/* Eaten as whole. */
				eat -= list->len;
				list = list->next;
				insp = list;
			} else {
				/* Eaten partially. */
				if (skb_is_gso(skb) && !list->head_frag &&
				    skb_headlen(list))
					skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;

				if (skb_shared(list)) {
					/* Sucks! We need to fork list. :-( */
					clone = skb_clone(list, GFP_ATOMIC);
					if (!clone)
						return NULL;
					insp = list->next;
					list = clone;
				} else {
					/* This may be pulled without
					 * problems. */
					insp = list;
				}
				if (!pskb_pull(list, eat)) {
					kfree_skb(clone);
					return NULL;
				}
				break;
			}
		} while (eat);

		/* Free pulled out fragments. */
		while ((list = skb_shinfo(skb)->frag_list) != insp) {
			skb_shinfo(skb)->frag_list = list->next;
			consume_skb(list);
		}
		/* And insert new clone at head. */
		if (clone) {
			clone->next = list;
			skb_shinfo(skb)->frag_list = clone;
		}
	}
	/* Success! Now we may commit changes to skb data. */

pull_pages:
	eat = delta;
	k = 0;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int size = skb_frag_size(&skb_shinfo(skb)->frags[i]);

		if (size <= eat) {
			skb_frag_unref(skb, i);
			eat -= size;
		} else {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[k];

			*frag = skb_shinfo(skb)->frags[i];
			if (eat) {
				skb_frag_off_add(frag, eat);
				skb_frag_size_sub(frag, eat);
				if (!i)
					goto end;
				eat = 0;
			}
			k++;
		}
	}
	skb_shinfo(skb)->nr_frags = k;

end:
	skb->tail     += delta;
	skb->data_len -= delta;

	if (!skb->data_len)
		skb_zcopy_clear(skb, false);

	return skb_tail_pointer(skb);
}
EXPORT_SYMBOL(__pskb_pull_tail);

/**
 *	skb_copy_bits - copy bits from skb to kernel buffer
 *	@skb: source skb
 *	@offset: offset in source
 *	@to: destination buffer
 *	@len: number of bytes to copy
 *
 *	Copy the specified number of bytes from the source skb to the
 *	destination buffer.
 *
 *	CAUTION ! :
 *		If its prototype is ever changed,
 *		check arch/{*}/net/{*}.S files,
 *		since it is called from BPF assembly code.
 */
int skb_copy_bits(const struct sk_buff *skb, int offset, void *to, int len)
{
	int start = skb_headlen(skb);
	struct sk_buff *frag_iter;
	int i, copy;

	if (offset > (int)skb->len - len)
		goto fault;

	/* Copy header. */
	if ((copy = start - offset) > 0) {
		if (copy > len)
			copy = len;
		skb_copy_from_linear_data_offset(skb, offset, to, copy);
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
		to     += copy;
	}

	if (!skb_frags_readable(skb))
		goto fault;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;
		skb_frag_t *f = &skb_shinfo(skb)->frags[i];

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(f);
		if ((copy = end - offset) > 0) {
			u32 p_off, p_len, copied;
			struct page *p;
			u8 *vaddr;

			if (copy > len)
				copy = len;

			skb_frag_foreach_page(f,
					      skb_frag_off(f) + offset - start,
					      copy, p, p_off, p_len, copied) {
				vaddr = kmap_atomic(p);
				memcpy(to + copied, vaddr + p_off, p_len);
				kunmap_atomic(vaddr);
			}

			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			to     += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			if (skb_copy_bits(frag_iter, offset - start, to, copy))
				goto fault;
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			to     += copy;
		}
		start = end;
	}

	if (!len)
		return 0;

fault:
	return -EFAULT;
}
EXPORT_SYMBOL(skb_copy_bits);

/*
 * Callback from splice_to_pipe(), if we need to release some pages
 * at the end of the spd in case we error'ed out in filling the pipe.
 */
static void sock_spd_release(struct splice_pipe_desc *spd, unsigned int i)
{
	put_page(spd->pages[i]);
}

static struct page *linear_to_page(struct page *page, unsigned int *len,
				   unsigned int *offset,
				   struct sock *sk)
{
	struct page_frag *pfrag = sk_page_frag(sk);

	if (!sk_page_frag_refill(sk, pfrag))
		return NULL;

	*len = min_t(unsigned int, *len, pfrag->size - pfrag->offset);

	memcpy(page_address(pfrag->page) + pfrag->offset,
	       page_address(page) + *offset, *len);
	*offset = pfrag->offset;
	pfrag->offset += *len;

	return pfrag->page;
}

static bool spd_can_coalesce(const struct splice_pipe_desc *spd,
			     struct page *page,
			     unsigned int offset)
{
	return	spd->nr_pages &&
		spd->pages[spd->nr_pages - 1] == page &&
		(spd->partial[spd->nr_pages - 1].offset +
		 spd->partial[spd->nr_pages - 1].len == offset);
}

/*
 * Fill page/offset/length into spd, if it can hold more pages.
 */
static bool spd_fill_page(struct splice_pipe_desc *spd, struct page *page,
			  unsigned int *len, unsigned int offset, bool linear,
			  struct sock *sk)
{
	if (unlikely(spd->nr_pages == MAX_SKB_FRAGS))
		return true;

	if (linear) {
		page = linear_to_page(page, len, &offset, sk);
		if (!page)
			return true;
	}
	if (spd_can_coalesce(spd, page, offset)) {
		spd->partial[spd->nr_pages - 1].len += *len;
		return false;
	}
	get_page(page);
	spd->pages[spd->nr_pages] = page;
	spd->partial[spd->nr_pages].len = *len;
	spd->partial[spd->nr_pages].offset = offset;
	spd->nr_pages++;

	return false;
}

static bool __splice_segment(struct page *page, unsigned int poff,
			     unsigned int plen, unsigned int *off,
			     unsigned int *len,
			     struct splice_pipe_desc *spd, bool linear,
			     struct sock *sk)
{
	if (!*len)
		return true;

	/* skip this segment if already processed */
	if (*off >= plen) {
		*off -= plen;
		return false;
	}

	/* ignore any bits we already processed */
	poff += *off;
	plen -= *off;
	*off = 0;

	do {
		unsigned int flen = min(*len, plen);

		if (spd_fill_page(spd, page, &flen, poff, linear, sk))
			return true;
		poff += flen;
		plen -= flen;
		*len -= flen;
	} while (*len && plen);

	return false;
}

/*
 * Map linear and fragment data from the skb to spd. It reports true if the
 * pipe is full or if we already spliced the requested length.
 */
static bool __skb_splice_bits(struct sk_buff *skb, struct pipe_inode_info *pipe,
			      unsigned int *offset, unsigned int *len,
			      struct splice_pipe_desc *spd, struct sock *sk)
{
	struct sk_buff *iter;
	int seg;

	/* map the linear part :
	 * If skb->head_frag is set, this 'linear' part is backed by a
	 * fragment, and if the head is not shared with any clones then
	 * we can avoid a copy since we own the head portion of this page.
	 */
	if (__splice_segment(virt_to_page(skb->data),
			     (unsigned long) skb->data & (PAGE_SIZE - 1),
			     skb_headlen(skb),
			     offset, len, spd,
			     skb_head_is_locked(skb),
			     sk))
		return true;

	/*
	 * then map the fragments
	 */
	if (!skb_frags_readable(skb))
		return false;

	for (seg = 0; seg < skb_shinfo(skb)->nr_frags; seg++) {
		const skb_frag_t *f = &skb_shinfo(skb)->frags[seg];

		if (WARN_ON_ONCE(!skb_frag_page(f)))
			return false;

		if (__splice_segment(skb_frag_page(f),
				     skb_frag_off(f), skb_frag_size(f),
				     offset, len, spd, false, sk))
			return true;
	}

	skb_walk_frags(skb, iter) {
		if (*offset >= iter->len) {
			*offset -= iter->len;
			continue;
		}
		/* __skb_splice_bits() only fails if the output has no room
		 * left, so no point in going over the frag_list for the error
		 * case.
		 */
		if (__skb_splice_bits(iter, pipe, offset, len, spd, sk))
			return true;
	}

	return false;
}

/*
 * Map data from the skb to a pipe. Should handle both the linear part,
 * the fragments, and the frag list.
 */
int skb_splice_bits(struct sk_buff *skb, struct sock *sk, unsigned int offset,
		    struct pipe_inode_info *pipe, unsigned int tlen,
		    unsigned int flags)
{
	struct partial_page partial[MAX_SKB_FRAGS];
	struct page *pages[MAX_SKB_FRAGS];
	struct splice_pipe_desc spd = {
		.pages = pages,
		.partial = partial,
		.nr_pages_max = MAX_SKB_FRAGS,
		.ops = &nosteal_pipe_buf_ops,
		.spd_release = sock_spd_release,
	};
	int ret = 0;

	__skb_splice_bits(skb, pipe, &offset, &tlen, &spd, sk);

	if (spd.nr_pages)
		ret = splice_to_pipe(pipe, &spd);

	return ret;
}
EXPORT_SYMBOL_GPL(skb_splice_bits);

static int sendmsg_locked(struct sock *sk, struct msghdr *msg)
{
	struct socket *sock = sk->sk_socket;
	size_t size = msg_data_left(msg);

	if (!sock)
		return -EINVAL;

	if (!sock->ops->sendmsg_locked)
		return sock_no_sendmsg_locked(sk, msg, size);

	return sock->ops->sendmsg_locked(sk, msg, size);
}

static int sendmsg_unlocked(struct sock *sk, struct msghdr *msg)
{
	struct socket *sock = sk->sk_socket;

	if (!sock)
		return -EINVAL;
	return sock_sendmsg(sock, msg);
}

typedef int (*sendmsg_func)(struct sock *sk, struct msghdr *msg);
static int __skb_send_sock(struct sock *sk, struct sk_buff *skb, int offset,
			   int len, sendmsg_func sendmsg, int flags)
{
	int more_hint = sk_is_tcp(sk) ? MSG_MORE : 0;
	unsigned int orig_len = len;
	struct sk_buff *head = skb;
	unsigned short fragidx;
	int slen, ret;

do_frag_list:

	/* Deal with head data */
	while (offset < skb_headlen(skb) && len) {
		struct kvec kv;
		struct msghdr msg;

		slen = min_t(int, len, skb_headlen(skb) - offset);
		kv.iov_base = skb->data + offset;
		kv.iov_len = slen;
		memset(&msg, 0, sizeof(msg));
		msg.msg_flags = MSG_DONTWAIT | flags;
		if (slen < len)
			msg.msg_flags |= more_hint;

		iov_iter_kvec(&msg.msg_iter, ITER_SOURCE, &kv, 1, slen);
		ret = INDIRECT_CALL_2(sendmsg, sendmsg_locked,
				      sendmsg_unlocked, sk, &msg);
		if (ret <= 0)
			goto error;

		offset += ret;
		len -= ret;
	}

	/* All the data was skb head? */
	if (!len)
		goto out;

	/* Make offset relative to start of frags */
	offset -= skb_headlen(skb);

	/* Find where we are in frag list */
	for (fragidx = 0; fragidx < skb_shinfo(skb)->nr_frags; fragidx++) {
		skb_frag_t *frag  = &skb_shinfo(skb)->frags[fragidx];

		if (offset < skb_frag_size(frag))
			break;

		offset -= skb_frag_size(frag);
	}

	for (; len && fragidx < skb_shinfo(skb)->nr_frags; fragidx++) {
		skb_frag_t *frag  = &skb_shinfo(skb)->frags[fragidx];

		slen = min_t(size_t, len, skb_frag_size(frag) - offset);

		while (slen) {
			struct bio_vec bvec;
			struct msghdr msg = {
				.msg_flags = MSG_SPLICE_PAGES | MSG_DONTWAIT |
					     flags,
			};

			if (slen < len)
				msg.msg_flags |= more_hint;
			bvec_set_page(&bvec, skb_frag_page(frag), slen,
				      skb_frag_off(frag) + offset);
			iov_iter_bvec(&msg.msg_iter, ITER_SOURCE, &bvec, 1,
				      slen);

			ret = INDIRECT_CALL_2(sendmsg, sendmsg_locked,
					      sendmsg_unlocked, sk, &msg);
			if (ret <= 0)
				goto error;

			len -= ret;
			offset += ret;
			slen -= ret;
		}

		offset = 0;
	}

	if (len) {
		/* Process any frag lists */

		if (skb == head) {
			if (skb_has_frag_list(skb)) {
				skb = skb_shinfo(skb)->frag_list;
				goto do_frag_list;
			}
		} else if (skb->next) {
			skb = skb->next;
			goto do_frag_list;
		}
	}

out:
	return orig_len - len;

error:
	return orig_len == len ? ret : orig_len - len;
}

/* Send skb data on a socket. Socket must be locked. */
int skb_send_sock_locked(struct sock *sk, struct sk_buff *skb, int offset,
			 int len)
{
	return __skb_send_sock(sk, skb, offset, len, sendmsg_locked, 0);
}
EXPORT_SYMBOL_GPL(skb_send_sock_locked);

int skb_send_sock_locked_with_flags(struct sock *sk, struct sk_buff *skb,
				    int offset, int len, int flags)
{
	return __skb_send_sock(sk, skb, offset, len, sendmsg_locked, flags);
}
EXPORT_SYMBOL_GPL(skb_send_sock_locked_with_flags);

/* Send skb data on a socket. Socket must be unlocked. */
int skb_send_sock(struct sock *sk, struct sk_buff *skb, int offset, int len)
{
	return __skb_send_sock(sk, skb, offset, len, sendmsg_unlocked, 0);
}

/**
 *	skb_store_bits - store bits from kernel buffer to skb
 *	@skb: destination buffer
 *	@offset: offset in destination
 *	@from: source buffer
 *	@len: number of bytes to copy
 *
 *	Copy the specified number of bytes from the source buffer to the
 *	destination skb.  This function handles all the messy bits of
 *	traversing fragment lists and such.
 */

int skb_store_bits(struct sk_buff *skb, int offset, const void *from, int len)
{
	int start = skb_headlen(skb);
	struct sk_buff *frag_iter;
	int i, copy;

	if (offset > (int)skb->len - len)
		goto fault;

	if ((copy = start - offset) > 0) {
		if (copy > len)
			copy = len;
		skb_copy_to_linear_data_offset(skb, offset, from, copy);
		if ((len -= copy) == 0)
			return 0;
		offset += copy;
		from += copy;
	}

	if (!skb_frags_readable(skb))
		goto fault;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(frag);
		if ((copy = end - offset) > 0) {
			u32 p_off, p_len, copied;
			struct page *p;
			u8 *vaddr;

			if (copy > len)
				copy = len;

			skb_frag_foreach_page(frag,
					      skb_frag_off(frag) + offset - start,
					      copy, p, p_off, p_len, copied) {
				vaddr = kmap_atomic(p);
				memcpy(vaddr + p_off, from + copied, p_len);
				kunmap_atomic(vaddr);
			}

			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			from += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			if (skb_store_bits(frag_iter, offset - start,
					   from, copy))
				goto fault;
			if ((len -= copy) == 0)
				return 0;
			offset += copy;
			from += copy;
		}
		start = end;
	}
	if (!len)
		return 0;

fault:
	return -EFAULT;
}
EXPORT_SYMBOL(skb_store_bits);

/* Checksum skb data. */
__wsum skb_checksum(const struct sk_buff *skb, int offset, int len, __wsum csum)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;
	int pos = 0;

	/* Checksum header. */
	if (copy > 0) {
		if (copy > len)
			copy = len;
		csum = csum_partial(skb->data + offset, copy, csum);
		if ((len -= copy) == 0)
			return csum;
		offset += copy;
		pos	= copy;
	}

	if (WARN_ON_ONCE(!skb_frags_readable(skb)))
		return 0;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(frag);
		if ((copy = end - offset) > 0) {
			u32 p_off, p_len, copied;
			struct page *p;
			__wsum csum2;
			u8 *vaddr;

			if (copy > len)
				copy = len;

			skb_frag_foreach_page(frag,
					      skb_frag_off(frag) + offset - start,
					      copy, p, p_off, p_len, copied) {
				vaddr = kmap_atomic(p);
				csum2 = csum_partial(vaddr + p_off, p_len, 0);
				kunmap_atomic(vaddr);
				csum = csum_block_add(csum, csum2, pos);
				pos += p_len;
			}

			if (!(len -= copy))
				return csum;
			offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			__wsum csum2;
			if (copy > len)
				copy = len;
			csum2 = skb_checksum(frag_iter, offset - start, copy,
					     0);
			csum = csum_block_add(csum, csum2, pos);
			if ((len -= copy) == 0)
				return csum;
			offset += copy;
			pos    += copy;
		}
		start = end;
	}
	BUG_ON(len);

	return csum;
}
EXPORT_SYMBOL(skb_checksum);

/* Both of above in one bottle. */

__wsum skb_copy_and_csum_bits(const struct sk_buff *skb, int offset,
				    u8 *to, int len)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;
	int pos = 0;
	__wsum csum = 0;

	/* Copy header. */
	if (copy > 0) {
		if (copy > len)
			copy = len;
		csum = csum_partial_copy_nocheck(skb->data + offset, to,
						 copy);
		if ((len -= copy) == 0)
			return csum;
		offset += copy;
		to     += copy;
		pos	= copy;
	}

	if (!skb_frags_readable(skb))
		return 0;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(&skb_shinfo(skb)->frags[i]);
		if ((copy = end - offset) > 0) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			u32 p_off, p_len, copied;
			struct page *p;
			__wsum csum2;
			u8 *vaddr;

			if (copy > len)
				copy = len;

			skb_frag_foreach_page(frag,
					      skb_frag_off(frag) + offset - start,
					      copy, p, p_off, p_len, copied) {
				vaddr = kmap_atomic(p);
				csum2 = csum_partial_copy_nocheck(vaddr + p_off,
								  to + copied,
								  p_len);
				kunmap_atomic(vaddr);
				csum = csum_block_add(csum, csum2, pos);
				pos += p_len;
			}

			if (!(len -= copy))
				return csum;
			offset += copy;
			to     += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		__wsum csum2;
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (copy > len)
				copy = len;
			csum2 = skb_copy_and_csum_bits(frag_iter,
						       offset - start,
						       to, copy);
			csum = csum_block_add(csum, csum2, pos);
			if ((len -= copy) == 0)
				return csum;
			offset += copy;
			to     += copy;
			pos    += copy;
		}
		start = end;
	}
	BUG_ON(len);
	return csum;
}
EXPORT_SYMBOL(skb_copy_and_csum_bits);

#ifdef CONFIG_NET_CRC32C
u32 skb_crc32c(const struct sk_buff *skb, int offset, int len, u32 crc)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;

	if (copy > 0) {
		copy = min(copy, len);
		crc = crc32c(crc, skb->data + offset, copy);
		len -= copy;
		if (len == 0)
			return crc;
		offset += copy;
	}

	if (WARN_ON_ONCE(!skb_frags_readable(skb)))
		return 0;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(frag);
		copy = end - offset;
		if (copy > 0) {
			u32 p_off, p_len, copied;
			struct page *p;
			u8 *vaddr;

			copy = min(copy, len);
			skb_frag_foreach_page(frag,
					      skb_frag_off(frag) + offset - start,
					      copy, p, p_off, p_len, copied) {
				vaddr = kmap_atomic(p);
				crc = crc32c(crc, vaddr + p_off, p_len);
				kunmap_atomic(vaddr);
			}
			len -= copy;
			if (len == 0)
				return crc;
			offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		copy = end - offset;
		if (copy > 0) {
			copy = min(copy, len);
			crc = skb_crc32c(frag_iter, offset - start, copy, crc);
			len -= copy;
			if (len == 0)
				return crc;
			offset += copy;
		}
		start = end;
	}
	BUG_ON(len);

	return crc;
}
EXPORT_SYMBOL(skb_crc32c);
#endif /* CONFIG_NET_CRC32C */

__sum16 __skb_checksum_complete_head(struct sk_buff *skb, int len)
{
	__sum16 sum;

	sum = csum_fold(skb_checksum(skb, 0, len, skb->csum));
	/* See comments in __skb_checksum_complete(). */
	if (likely(!sum)) {
		if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE) &&
		    !skb->csum_complete_sw)
			netdev_rx_csum_fault(skb->dev, skb);
	}
	if (!skb_shared(skb))
		skb->csum_valid = !sum;
	return sum;
}
EXPORT_SYMBOL(__skb_checksum_complete_head);

/* This function assumes skb->csum already holds pseudo header's checksum,
 * which has been changed from the hardware checksum, for example, by
 * __skb_checksum_validate_complete(). And, the original skb->csum must
 * have been validated unsuccessfully for CHECKSUM_COMPLETE case.
 *
 * It returns non-zero if the recomputed checksum is still invalid, otherwise
 * zero. The new checksum is stored back into skb->csum unless the skb is
 * shared.
 */
__sum16 __skb_checksum_complete(struct sk_buff *skb)
{
	__wsum csum;
	__sum16 sum;

	csum = skb_checksum(skb, 0, skb->len, 0);

	sum = csum_fold(csum_add(skb->csum, csum));
	/* This check is inverted, because we already knew the hardware
	 * checksum is invalid before calling this function. So, if the
	 * re-computed checksum is valid instead, then we have a mismatch
	 * between the original skb->csum and skb_checksum(). This means either
	 * the original hardware checksum is incorrect or we screw up skb->csum
	 * when moving skb->data around.
	 */
	if (likely(!sum)) {
		if (unlikely(skb->ip_summed == CHECKSUM_COMPLETE) &&
		    !skb->csum_complete_sw)
			netdev_rx_csum_fault(skb->dev, skb);
	}

	if (!skb_shared(skb)) {
		/* Save full packet checksum */
		skb->csum = csum;
		skb->ip_summed = CHECKSUM_COMPLETE;
		skb->csum_complete_sw = 1;
		skb->csum_valid = !sum;
	}

	return sum;
}
EXPORT_SYMBOL(__skb_checksum_complete);

 /**
 *	skb_zerocopy_headlen - Calculate headroom needed for skb_zerocopy()
 *	@from: source buffer
 *
 *	Calculates the amount of linear headroom needed in the 'to' skb passed
 *	into skb_zerocopy().
 */
unsigned int
skb_zerocopy_headlen(const struct sk_buff *from)
{
	unsigned int hlen = 0;

	if (!from->head_frag ||
	    skb_headlen(from) < L1_CACHE_BYTES ||
	    skb_shinfo(from)->nr_frags >= MAX_SKB_FRAGS) {
		hlen = skb_headlen(from);
		if (!hlen)
			hlen = from->len;
	}

	if (skb_has_frag_list(from))
		hlen = from->len;

	return hlen;
}
EXPORT_SYMBOL_GPL(skb_zerocopy_headlen);

/**
 *	skb_zerocopy - Zero copy skb to skb
 *	@to: destination buffer
 *	@from: source buffer
 *	@len: number of bytes to copy from source buffer
 *	@hlen: size of linear headroom in destination buffer
 *
 *	Copies up to `len` bytes from `from` to `to` by creating references
 *	to the frags in the source buffer.
 *
 *	The `hlen` as calculated by skb_zerocopy_headlen() specifies the
 *	headroom in the `to` buffer.
 *
 *	Return value:
 *	0: everything is OK
 *	-ENOMEM: couldn't orphan frags of @from due to lack of memory
 *	-EFAULT: skb_copy_bits() found some problem with skb geometry
 */
int
skb_zerocopy(struct sk_buff *to, struct sk_buff *from, int len, int hlen)
{
	int i, j = 0;
	int plen = 0; /* length of skb->head fragment */
	int ret;
	struct page *page;
	unsigned int offset;

	BUG_ON(!from->head_frag && !hlen);

	/* dont bother with small payloads */
	if (len <= skb_tailroom(to))
		return skb_copy_bits(from, 0, skb_put(to, len), len);

	if (hlen) {
		ret = skb_copy_bits(from, 0, skb_put(to, hlen), hlen);
		if (unlikely(ret))
			return ret;
		len -= hlen;
	} else {
		plen = min_t(int, skb_headlen(from), len);
		if (plen) {
			page = virt_to_head_page(from->head);
			offset = from->data - (unsigned char *)page_address(page);
			__skb_fill_netmem_desc(to, 0, page_to_netmem(page),
					       offset, plen);
			get_page(page);
			j = 1;
			len -= plen;
		}
	}

	skb_len_add(to, len + plen);

	if (unlikely(skb_orphan_frags(from, GFP_ATOMIC))) {
		skb_tx_error(from);
		return -ENOMEM;
	}
	skb_zerocopy_clone(to, from, GFP_ATOMIC);

	for (i = 0; i < skb_shinfo(from)->nr_frags; i++) {
		int size;

		if (!len)
			break;
		skb_shinfo(to)->frags[j] = skb_shinfo(from)->frags[i];
		size = min_t(int, skb_frag_size(&skb_shinfo(to)->frags[j]),
					len);
		skb_frag_size_set(&skb_shinfo(to)->frags[j], size);
		len -= size;
		skb_frag_ref(to, j);
		j++;
	}
	skb_shinfo(to)->nr_frags = j;

	return 0;
}
EXPORT_SYMBOL_GPL(skb_zerocopy);

void skb_copy_and_csum_dev(const struct sk_buff *skb, u8 *to)
{
	__wsum csum;
	long csstart;

	if (skb->ip_summed == CHECKSUM_PARTIAL)
		csstart = skb_checksum_start_offset(skb);
	else
		csstart = skb_headlen(skb);

	BUG_ON(csstart > skb_headlen(skb));

	skb_copy_from_linear_data(skb, to, csstart);

	csum = 0;
	if (csstart != skb->len)
		csum = skb_copy_and_csum_bits(skb, csstart, to + csstart,
					      skb->len - csstart);

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		long csstuff = csstart + skb->csum_offset;

		*((__sum16 *)(to + csstuff)) = csum_fold(csum);
	}
}
EXPORT_SYMBOL(skb_copy_and_csum_dev);

/**
 *	skb_dequeue - remove from the head of the queue
 *	@list: list to dequeue from
 *
 *	Remove the head of the list. The list lock is taken so the function
 *	may be used safely with other locking list functions. The head item is
 *	returned or %NULL if the list is empty.
 */

struct sk_buff *skb_dequeue(struct sk_buff_head *list)
{
	unsigned long flags;
	struct sk_buff *result;

	spin_lock_irqsave(&list->lock, flags);
	result = __skb_dequeue(list);
	spin_unlock_irqrestore(&list->lock, flags);
	return result;
}
EXPORT_SYMBOL(skb_dequeue);

/**
 *	skb_dequeue_tail - remove from the tail of the queue
 *	@list: list to dequeue from
 *
 *	Remove the tail of the list. The list lock is taken so the function
 *	may be used safely with other locking list functions. The tail item is
 *	returned or %NULL if the list is empty.
 */
struct sk_buff *skb_dequeue_tail(struct sk_buff_head *list)
{
	unsigned long flags;
	struct sk_buff *result;

	spin_lock_irqsave(&list->lock, flags);
	result = __skb_dequeue_tail(list);
	spin_unlock_irqrestore(&list->lock, flags);
	return result;
}
EXPORT_SYMBOL(skb_dequeue_tail);

/**
 *	skb_queue_purge_reason - empty a list
 *	@list: list to empty
 *	@reason: drop reason
 *
 *	Delete all buffers on an &sk_buff list. Each buffer is removed from
 *	the list and one reference dropped. This function takes the list
 *	lock and is atomic with respect to other list locking functions.
 */
void skb_queue_purge_reason(struct sk_buff_head *list,
			    enum skb_drop_reason reason)
{
	struct sk_buff_head tmp;
	unsigned long flags;

	if (skb_queue_empty_lockless(list))
		return;

	__skb_queue_head_init(&tmp);

	spin_lock_irqsave(&list->lock, flags);
	skb_queue_splice_init(list, &tmp);
	spin_unlock_irqrestore(&list->lock, flags);

	__skb_queue_purge_reason(&tmp, reason);
}
EXPORT_SYMBOL(skb_queue_purge_reason);

/**
 *	skb_rbtree_purge - empty a skb rbtree
 *	@root: root of the rbtree to empty
 *	Return value: the sum of truesizes of all purged skbs.
 *
 *	Delete all buffers on an &sk_buff rbtree. Each buffer is removed from
 *	the list and one reference dropped. This function does not take
 *	any lock. Synchronization should be handled by the caller (e.g., TCP
 *	out-of-order queue is protected by the socket lock).
 */
unsigned int skb_rbtree_purge(struct rb_root *root)
{
	struct rb_node *p = rb_first(root);
	unsigned int sum = 0;

	while (p) {
		struct sk_buff *skb = rb_entry(p, struct sk_buff, rbnode);

		p = rb_next(p);
		rb_erase(&skb->rbnode, root);
		sum += skb->truesize;
		kfree_skb(skb);
	}
	return sum;
}

void skb_errqueue_purge(struct sk_buff_head *list)
{
	struct sk_buff *skb, *next;
	struct sk_buff_head kill;
	unsigned long flags;

	__skb_queue_head_init(&kill);

	spin_lock_irqsave(&list->lock, flags);
	skb_queue_walk_safe(list, skb, next) {
		if (SKB_EXT_ERR(skb)->ee.ee_origin == SO_EE_ORIGIN_ZEROCOPY ||
		    SKB_EXT_ERR(skb)->ee.ee_origin == SO_EE_ORIGIN_TIMESTAMPING)
			continue;
		__skb_unlink(skb, list);
		__skb_queue_tail(&kill, skb);
	}
	spin_unlock_irqrestore(&list->lock, flags);
	__skb_queue_purge(&kill);
}
EXPORT_SYMBOL(skb_errqueue_purge);

/**
 *	skb_queue_head - queue a buffer at the list head
 *	@list: list to use
 *	@newsk: buffer to queue
 *
 *	Queue a buffer at the start of the list. This function takes the
 *	list lock and can be used safely with other locking &sk_buff functions
 *	safely.
 *
 *	A buffer cannot be placed on two lists at the same time.
 */
void skb_queue_head(struct sk_buff_head *list, struct sk_buff *newsk)
{
	unsigned long flags;

	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_head(list, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}
EXPORT_SYMBOL(skb_queue_head);

/**
 *	skb_queue_tail - queue a buffer at the list tail
 *	@list: list to use
 *	@newsk: buffer to queue
 *
 *	Queue a buffer at the tail of the list. This function takes the
 *	list lock and can be used safely with other locking &sk_buff functions
 *	safely.
 *
 *	A buffer cannot be placed on two lists at the same time.
 */
void skb_queue_tail(struct sk_buff_head *list, struct sk_buff *newsk)
{
	unsigned long flags;

	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_tail(list, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}
EXPORT_SYMBOL(skb_queue_tail);

/**
 *	skb_unlink	-	remove a buffer from a list
 *	@skb: buffer to remove
 *	@list: list to use
 *
 *	Remove a packet from a list. The list locks are taken and this
 *	function is atomic with respect to other list locked calls
 *
 *	You must know what list the SKB is on.
 */
void skb_unlink(struct sk_buff *skb, struct sk_buff_head *list)
{
	unsigned long flags;

	spin_lock_irqsave(&list->lock, flags);
	__skb_unlink(skb, list);
	spin_unlock_irqrestore(&list->lock, flags);
}
EXPORT_SYMBOL(skb_unlink);

/**
 *	skb_append	-	append a buffer
 *	@old: buffer to insert after
 *	@newsk: buffer to insert
 *	@list: list to use
 *
 *	Place a packet after a given packet in a list. The list locks are taken
 *	and this function is atomic with respect to other list locked calls.
 *	A buffer cannot be placed on two lists at the same time.
 */
void skb_append(struct sk_buff *old, struct sk_buff *newsk, struct sk_buff_head *list)
{
	unsigned long flags;

	spin_lock_irqsave(&list->lock, flags);
	__skb_queue_after(list, old, newsk);
	spin_unlock_irqrestore(&list->lock, flags);
}
EXPORT_SYMBOL(skb_append);

static inline void skb_split_inside_header(struct sk_buff *skb,
					   struct sk_buff* skb1,
					   const u32 len, const int pos)
{
	int i;

	skb_copy_from_linear_data_offset(skb, len, skb_put(skb1, pos - len),
					 pos - len);
	/* And move data appendix as is. */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		skb_shinfo(skb1)->frags[i] = skb_shinfo(skb)->frags[i];

	skb_shinfo(skb1)->nr_frags = skb_shinfo(skb)->nr_frags;
	skb1->unreadable	   = skb->unreadable;
	skb_shinfo(skb)->nr_frags  = 0;
	skb1->data_len		   = skb->data_len;
	skb1->len		   += skb1->data_len;
	skb->data_len		   = 0;
	skb->len		   = len;
	skb_set_tail_pointer(skb, len);
}

static inline void skb_split_no_header(struct sk_buff *skb,
				       struct sk_buff* skb1,
				       const u32 len, int pos)
{
	int i, k = 0;
	const int nfrags = skb_shinfo(skb)->nr_frags;

	skb_shinfo(skb)->nr_frags = 0;
	skb1->len		  = skb1->data_len = skb->len - len;
	skb->len		  = len;
	skb->data_len		  = len - pos;

	for (i = 0; i < nfrags; i++) {
		int size = skb_frag_size(&skb_shinfo(skb)->frags[i]);

		if (pos + size > len) {
			skb_shinfo(skb1)->frags[k] = skb_shinfo(skb)->frags[i];

			if (pos < len) {
				/* Split frag.
				 * We have two variants in this case:
				 * 1. Move all the frag to the second
				 *    part, if it is possible. F.e.
				 *    this approach is mandatory for TUX,
				 *    where splitting is expensive.
				 * 2. Split is accurately. We make this.
				 */
				skb_frag_ref(skb, i);
				skb_frag_off_add(&skb_shinfo(skb1)->frags[0], len - pos);
				skb_frag_size_sub(&skb_shinfo(skb1)->frags[0], len - pos);
				skb_frag_size_set(&skb_shinfo(skb)->frags[i], len - pos);
				skb_shinfo(skb)->nr_frags++;
			}
			k++;
		} else
			skb_shinfo(skb)->nr_frags++;
		pos += size;
	}
	skb_shinfo(skb1)->nr_frags = k;

	skb1->unreadable = skb->unreadable;
}

/**
 * skb_split - Split fragmented skb to two parts at length len.
 * @skb: the buffer to split
 * @skb1: the buffer to receive the second part
 * @len: new length for skb
 */
void skb_split(struct sk_buff *skb, struct sk_buff *skb1, const u32 len)
{
	int pos = skb_headlen(skb);
	const int zc_flags = SKBFL_SHARED_FRAG | SKBFL_PURE_ZEROCOPY;

	skb_zcopy_downgrade_managed(skb);

	skb_shinfo(skb1)->flags |= skb_shinfo(skb)->flags & zc_flags;
	skb_zerocopy_clone(skb1, skb, 0);
	if (len < pos)	/* Split line is inside header. */
		skb_split_inside_header(skb, skb1, len, pos);
	else		/* Second chunk has no header, nothing to copy. */
		skb_split_no_header(skb, skb1, len, pos);
}
EXPORT_SYMBOL(skb_split);

/* Shifting from/to a cloned skb is a no-go.
 *
 * Caller cannot keep skb_shinfo related pointers past calling here!
 */
static int skb_prepare_for_shift(struct sk_buff *skb)
{
	return skb_unclone_keeptruesize(skb, GFP_ATOMIC);
}

/**
 * skb_shift - Shifts paged data partially from skb to another
 * @tgt: buffer into which tail data gets added
 * @skb: buffer from which the paged data comes from
 * @shiftlen: shift up to this many bytes
 *
 * Attempts to shift up to shiftlen worth of bytes, which may be less than
 * the length of the skb, from skb to tgt. Returns number bytes shifted.
 * It's up to caller to free skb if everything was shifted.
 *
 * If @tgt runs out of frags, the whole operation is aborted.
 *
 * Skb cannot include anything else but paged data while tgt is allowed
 * to have non-paged data as well.
 *
 * TODO: full sized shift could be optimized but that would need
 * specialized skb free'er to handle frags without up-to-date nr_frags.
 */
int skb_shift(struct sk_buff *tgt, struct sk_buff *skb, int shiftlen)
{
	int from, to, merge, todo;
	skb_frag_t *fragfrom, *fragto;

	BUG_ON(shiftlen > skb->len);

	if (skb_headlen(skb))
		return 0;
	if (skb_zcopy(tgt) || skb_zcopy(skb))
		return 0;

	DEBUG_NET_WARN_ON_ONCE(tgt->pp_recycle != skb->pp_recycle);
	DEBUG_NET_WARN_ON_ONCE(skb_cmp_decrypted(tgt, skb));

	todo = shiftlen;
	from = 0;
	to = skb_shinfo(tgt)->nr_frags;
	fragfrom = &skb_shinfo(skb)->frags[from];

	/* Actual merge is delayed until the point when we know we can
	 * commit all, so that we don't have to undo partial changes
	 */
	if (!skb_can_coalesce(tgt, to, skb_frag_page(fragfrom),
			      skb_frag_off(fragfrom))) {
		merge = -1;
	} else {
		merge = to - 1;

		todo -= skb_frag_size(fragfrom);
		if (todo < 0) {
			if (skb_prepare_for_shift(skb) ||
			    skb_prepare_for_shift(tgt))
				return 0;

			/* All previous frag pointers might be stale! */
			fragfrom = &skb_shinfo(skb)->frags[from];
			fragto = &skb_shinfo(tgt)->frags[merge];

			skb_frag_size_add(fragto, shiftlen);
			skb_frag_size_sub(fragfrom, shiftlen);
			skb_frag_off_add(fragfrom, shiftlen);

			goto onlymerged;
		}

		from++;
	}

	/* Skip full, not-fitting skb to avoid expensive operations */
	if ((shiftlen == skb->len) &&
	    (skb_shinfo(skb)->nr_frags - from) > (MAX_SKB_FRAGS - to))
		return 0;

	if (skb_prepare_for_shift(skb) || skb_prepare_for_shift(tgt))
		return 0;

	while ((todo > 0) && (from < skb_shinfo(skb)->nr_frags)) {
		if (to == MAX_SKB_FRAGS)
			return 0;

		fragfrom = &skb_shinfo(skb)->frags[from];
		fragto = &skb_shinfo(tgt)->frags[to];

		if (todo >= skb_frag_size(fragfrom)) {
			*fragto = *fragfrom;
			todo -= skb_frag_size(fragfrom);
			from++;
			to++;

		} else {
			__skb_frag_ref(fragfrom);
			skb_frag_page_copy(fragto, fragfrom);
			skb_frag_off_copy(fragto, fragfrom);
			skb_frag_size_set(fragto, todo);

			skb_frag_off_add(fragfrom, todo);
			skb_frag_size_sub(fragfrom, todo);
			todo = 0;

			to++;
			break;
		}
	}

	/* Ready to "commit" this state change to tgt */
	skb_shinfo(tgt)->nr_frags = to;

	if (merge >= 0) {
		fragfrom = &skb_shinfo(skb)->frags[0];
		fragto = &skb_shinfo(tgt)->frags[merge];

		skb_frag_size_add(fragto, skb_frag_size(fragfrom));
		__skb_frag_unref(fragfrom, skb->pp_recycle);
	}

	/* Reposition in the original skb */
	to = 0;
	while (from < skb_shinfo(skb)->nr_frags)
		skb_shinfo(skb)->frags[to++] = skb_shinfo(skb)->frags[from++];
	skb_shinfo(skb)->nr_frags = to;

	BUG_ON(todo > 0 && !skb_shinfo(skb)->nr_frags);

onlymerged:
	/* Most likely the tgt won't ever need its checksum anymore, skb on
	 * the other hand might need it if it needs to be resent
	 */
	tgt->ip_summed = CHECKSUM_PARTIAL;
	skb->ip_summed = CHECKSUM_PARTIAL;

	skb_len_add(skb, -shiftlen);
	skb_len_add(tgt, shiftlen);

	return shiftlen;
}

/**
 * skb_prepare_seq_read - Prepare a sequential read of skb data
 * @skb: the buffer to read
 * @from: lower offset of data to be read
 * @to: upper offset of data to be read
 * @st: state variable
 *
 * Initializes the specified state variable. Must be called before
 * invoking skb_seq_read() for the first time.
 */
void skb_prepare_seq_read(struct sk_buff *skb, unsigned int from,
			  unsigned int to, struct skb_seq_state *st)
{
	st->lower_offset = from;
	st->upper_offset = to;
	st->root_skb = st->cur_skb = skb;
	st->frag_idx = st->stepped_offset = 0;
	st->frag_data = NULL;
	st->frag_off = 0;
}
EXPORT_SYMBOL(skb_prepare_seq_read);

/**
 * skb_seq_read - Sequentially read skb data
 * @consumed: number of bytes consumed by the caller so far
 * @data: destination pointer for data to be returned
 * @st: state variable
 *
 * Reads a block of skb data at @consumed relative to the
 * lower offset specified to skb_prepare_seq_read(). Assigns
 * the head of the data block to @data and returns the length
 * of the block or 0 if the end of the skb data or the upper
 * offset has been reached.
 *
 * The caller is not required to consume all of the data
 * returned, i.e. @consumed is typically set to the number
 * of bytes already consumed and the next call to
 * skb_seq_read() will return the remaining part of the block.
 *
 * Note 1: The size of each block of data returned can be arbitrary,
 *       this limitation is the cost for zerocopy sequential
 *       reads of potentially non linear data.
 *
 * Note 2: Fragment lists within fragments are not implemented
 *       at the moment, state->root_skb could be replaced with
 *       a stack for this purpose.
 */
unsigned int skb_seq_read(unsigned int consumed, const u8 **data,
			  struct skb_seq_state *st)
{
	unsigned int block_limit, abs_offset = consumed + st->lower_offset;
	skb_frag_t *frag;

	if (unlikely(abs_offset >= st->upper_offset)) {
		if (st->frag_data) {
			kunmap_atomic(st->frag_data);
			st->frag_data = NULL;
		}
		return 0;
	}

next_skb:
	block_limit = skb_headlen(st->cur_skb) + st->stepped_offset;

	if (abs_offset < block_limit && !st->frag_data) {
		*data = st->cur_skb->data + (abs_offset - st->stepped_offset);
		return block_limit - abs_offset;
	}

	if (!skb_frags_readable(st->cur_skb))
		return 0;

	if (st->frag_idx == 0 && !st->frag_data)
		st->stepped_offset += skb_headlen(st->cur_skb);

	while (st->frag_idx < skb_shinfo(st->cur_skb)->nr_frags) {
		unsigned int pg_idx, pg_off, pg_sz;

		frag = &skb_shinfo(st->cur_skb)->frags[st->frag_idx];

		pg_idx = 0;
		pg_off = skb_frag_off(frag);
		pg_sz = skb_frag_size(frag);

		if (skb_frag_must_loop(skb_frag_page(frag))) {
			pg_idx = (pg_off + st->frag_off) >> PAGE_SHIFT;
			pg_off = offset_in_page(pg_off + st->frag_off);
			pg_sz = min_t(unsigned int, pg_sz - st->frag_off,
						    PAGE_SIZE - pg_off);
		}

		block_limit = pg_sz + st->stepped_offset;
		if (abs_offset < block_limit) {
			if (!st->frag_data)
				st->frag_data = kmap_atomic(skb_frag_page(frag) + pg_idx);

			*data = (u8 *)st->frag_data + pg_off +
				(abs_offset - st->stepped_offset);

			return block_limit - abs_offset;
		}

		if (st->frag_data) {
			kunmap_atomic(st->frag_data);
			st->frag_data = NULL;
		}

		st->stepped_offset += pg_sz;
		st->frag_off += pg_sz;
		if (st->frag_off == skb_frag_size(frag)) {
			st->frag_off = 0;
			st->frag_idx++;
		}
	}

	if (st->frag_data) {
		kunmap_atomic(st->frag_data);
		st->frag_data = NULL;
	}

	if (st->root_skb == st->cur_skb && skb_has_frag_list(st->root_skb)) {
		st->cur_skb = skb_shinfo(st->root_skb)->frag_list;
		st->frag_idx = 0;
		goto next_skb;
	} else if (st->cur_skb->next) {
		st->cur_skb = st->cur_skb->next;
		st->frag_idx = 0;
		goto next_skb;
	}

	return 0;
}
EXPORT_SYMBOL(skb_seq_read);

/**
 * skb_abort_seq_read - Abort a sequential read of skb data
 * @st: state variable
 *
 * Must be called if skb_seq_read() was not called until it
 * returned 0.
 */
void skb_abort_seq_read(struct skb_seq_state *st)
{
	if (st->frag_data)
		kunmap_atomic(st->frag_data);
}
EXPORT_SYMBOL(skb_abort_seq_read);

/**
 * skb_copy_seq_read() - copy from a skb_seq_state to a buffer
 * @st: source skb_seq_state
 * @offset: offset in source
 * @to: destination buffer
 * @len: number of bytes to copy
 *
 * Copy @len bytes from @offset bytes into the source @st to the destination
 * buffer @to. `offset` should increase (or be unchanged) with each subsequent
 * call to this function. If offset needs to decrease from the previous use `st`
 * should be reset first.
 *
 * Return: 0 on success or -EINVAL if the copy ended early
 */
int skb_copy_seq_read(struct skb_seq_state *st, int offset, void *to, int len)
{
	const u8 *data;
	u32 sqlen;

	for (;;) {
		sqlen = skb_seq_read(offset, &data, st);
		if (sqlen == 0)
			return -EINVAL;
		if (sqlen >= len) {
			memcpy(to, data, len);
			return 0;
		}
		memcpy(to, data, sqlen);
		to += sqlen;
		offset += sqlen;
		len -= sqlen;
	}
}
EXPORT_SYMBOL(skb_copy_seq_read);

#define TS_SKB_CB(state)	((struct skb_seq_state *) &((state)->cb))

static unsigned int skb_ts_get_next_block(unsigned int offset, const u8 **text,
					  struct ts_config *conf,
					  struct ts_state *state)
{
	return skb_seq_read(offset, text, TS_SKB_CB(state));
}

static void skb_ts_finish(struct ts_config *conf, struct ts_state *state)
{
	skb_abort_seq_read(TS_SKB_CB(state));
}

/**
 * skb_find_text - Find a text pattern in skb data
 * @skb: the buffer to look in
 * @from: search offset
 * @to: search limit
 * @config: textsearch configuration
 *
 * Finds a pattern in the skb data according to the specified
 * textsearch configuration. Use textsearch_next() to retrieve
 * subsequent occurrences of the pattern. Returns the offset
 * to the first occurrence or UINT_MAX if no match was found.
 */
unsigned int skb_find_text(struct sk_buff *skb, unsigned int from,
			   unsigned int to, struct ts_config *config)
{
	unsigned int patlen = config->ops->get_pattern_len(config);
	struct ts_state state;
	unsigned int ret;

	BUILD_BUG_ON(sizeof(struct skb_seq_state) > sizeof(state.cb));

	config->get_next_block = skb_ts_get_next_block;
	config->finish = skb_ts_finish;

	skb_prepare_seq_read(skb, from, to, TS_SKB_CB(&state));

	ret = textsearch_find(config, &state);
	return (ret + patlen <= to - from ? ret : UINT_MAX);
}
EXPORT_SYMBOL(skb_find_text);

int skb_append_pagefrags(struct sk_buff *skb, struct page *page,
			 int offset, size_t size, size_t max_frags)
{
	int i = skb_shinfo(skb)->nr_frags;

	if (skb_can_coalesce(skb, i, page, offset)) {
		skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1], size);
	} else if (i < max_frags) {
		skb_zcopy_downgrade_managed(skb);
		get_page(page);
		skb_fill_page_desc_noacc(skb, i, page, offset, size);
	} else {
		return -EMSGSIZE;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(skb_append_pagefrags);

/**
 *	skb_pull_rcsum - pull skb and update receive checksum
 *	@skb: buffer to update
 *	@len: length of data pulled
 *
 *	This function performs an skb_pull on the packet and updates
 *	the CHECKSUM_COMPLETE checksum.  It should be used on
 *	receive path processing instead of skb_pull unless you know
 *	that the checksum difference is zero (e.g., a valid IP header)
 *	or you are setting ip_summed to CHECKSUM_NONE.
 */
void *skb_pull_rcsum(struct sk_buff *skb, unsigned int len)
{
	unsigned char *data = skb->data;

	BUG_ON(len > skb->len);
	__skb_pull(skb, len);
	skb_postpull_rcsum(skb, data, len);
	return skb->data;
}
EXPORT_SYMBOL_GPL(skb_pull_rcsum);

static inline skb_frag_t skb_head_frag_to_page_desc(struct sk_buff *frag_skb)
{
	skb_frag_t head_frag;
	struct page *page;

	page = virt_to_head_page(frag_skb->head);
	skb_frag_fill_page_desc(&head_frag, page, frag_skb->data -
				(unsigned char *)page_address(page),
				skb_headlen(frag_skb));
	return head_frag;
}

struct sk_buff *skb_segment_list(struct sk_buff *skb,
				 netdev_features_t features,
				 unsigned int offset)
{
	struct sk_buff *list_skb = skb_shinfo(skb)->frag_list;
	unsigned int tnl_hlen = skb_tnl_header_len(skb);
	unsigned int delta_truesize = 0;
	unsigned int delta_len = 0;
	struct sk_buff *tail = NULL;
	struct sk_buff *nskb, *tmp;
	int len_diff, err;

	skb_push(skb, -skb_network_offset(skb) + offset);

	/* Ensure the head is writeable before touching the shared info */
	err = skb_unclone(skb, GFP_ATOMIC);
	if (err)
		goto err_linearize;

	skb_shinfo(skb)->frag_list = NULL;

	while (list_skb) {
		nskb = list_skb;
		list_skb = list_skb->next;

		err = 0;
		delta_truesize += nskb->truesize;
		if (skb_shared(nskb)) {
			tmp = skb_clone(nskb, GFP_ATOMIC);
			if (tmp) {
				consume_skb(nskb);
				nskb = tmp;
				err = skb_unclone(nskb, GFP_ATOMIC);
			} else {
				err = -ENOMEM;
			}
		}

		if (!tail)
			skb->next = nskb;
		else
			tail->next = nskb;

		if (unlikely(err)) {
			nskb->next = list_skb;
			goto err_linearize;
		}

		tail = nskb;

		delta_len += nskb->len;

		skb_push(nskb, -skb_network_offset(nskb) + offset);

		skb_release_head_state(nskb);
		len_diff = skb_network_header_len(nskb) - skb_network_header_len(skb);
		__copy_skb_header(nskb, skb);

		skb_headers_offset_update(nskb, skb_headroom(nskb) - skb_headroom(skb));
		nskb->transport_header += len_diff;
		skb_copy_from_linear_data_offset(skb, -tnl_hlen,
						 nskb->data - tnl_hlen,
						 offset + tnl_hlen);

		if (skb_needs_linearize(nskb, features) &&
		    __skb_linearize(nskb))
			goto err_linearize;
	}

	skb->truesize = skb->truesize - delta_truesize;
	skb->data_len = skb->data_len - delta_len;
	skb->len = skb->len - delta_len;

	skb_gso_reset(skb);

	skb->prev = tail;

	if (skb_needs_linearize(skb, features) &&
	    __skb_linearize(skb))
		goto err_linearize;

	skb_get(skb);

	return skb;

err_linearize:
	kfree_skb_list(skb->next);
	skb->next = NULL;
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(skb_segment_list);

/**
 *	skb_segment - Perform protocol segmentation on skb.
 *	@head_skb: buffer to segment
 *	@features: features for the output path (see dev->features)
 *
 *	This function performs segmentation on the given skb.  It returns
 *	a pointer to the first in a list of new skbs for the segments.
 *	In case of error it returns ERR_PTR(err).
 */
struct sk_buff *skb_segment(struct sk_buff *head_skb,
			    netdev_features_t features)
{
	struct sk_buff *segs = NULL;
	struct sk_buff *tail = NULL;
	struct sk_buff *list_skb = skb_shinfo(head_skb)->frag_list;
	unsigned int mss = skb_shinfo(head_skb)->gso_size;
	unsigned int doffset = head_skb->data - skb_mac_header(head_skb);
	unsigned int offset = doffset;
	unsigned int tnl_hlen = skb_tnl_header_len(head_skb);
	unsigned int partial_segs = 0;
	unsigned int headroom;
	unsigned int len = head_skb->len;
	struct sk_buff *frag_skb;
	skb_frag_t *frag;
	__be16 proto;
	bool csum, sg;
	int err = -ENOMEM;
	int i = 0;
	int nfrags, pos;

	if ((skb_shinfo(head_skb)->gso_type & SKB_GSO_DODGY) &&
	    mss != GSO_BY_FRAGS && mss != skb_headlen(head_skb)) {
		struct sk_buff *check_skb;

		for (check_skb = list_skb; check_skb; check_skb = check_skb->next) {
			if (skb_headlen(check_skb) && !check_skb->head_frag) {
				/* gso_size is untrusted, and we have a frag_list with
				 * a linear non head_frag item.
				 *
				 * If head_skb's headlen does not fit requested gso_size,
				 * it means that the frag_list members do NOT terminate
				 * on exact gso_size boundaries. Hence we cannot perform
				 * skb_frag_t page sharing. Therefore we must fallback to
				 * copying the frag_list skbs; we do so by disabling SG.
				 */
				features &= ~NETIF_F_SG;
				break;
			}
		}
	}

	__skb_push(head_skb, doffset);
	proto = skb_network_protocol(head_skb, NULL);
	if (unlikely(!proto))
		return ERR_PTR(-EINVAL);

	sg = !!(features & NETIF_F_SG);
	csum = !!can_checksum_protocol(features, proto);

	if (sg && csum && (mss != GSO_BY_FRAGS))  {
		if (!(features & NETIF_F_GSO_PARTIAL)) {
			struct sk_buff *iter;
			unsigned int frag_len;

			if (!list_skb ||
			    !net_gso_ok(features, skb_shinfo(head_skb)->gso_type))
				goto normal;

			/* If we get here then all the required
			 * GSO features except frag_list are supported.
			 * Try to split the SKB to multiple GSO SKBs
			 * with no frag_list.
			 * Currently we can do that only when the buffers don't
			 * have a linear part and all the buffers except
			 * the last are of the same length.
			 */
			frag_len = list_skb->len;
			skb_walk_frags(head_skb, iter) {
				if (frag_len != iter->len && iter->next)
					goto normal;
				if (skb_headlen(iter) && !iter->head_frag)
					goto normal;

				len -= iter->len;
			}

			if (len != frag_len)
				goto normal;
		}

		/* GSO partial only requires that we trim off any excess that
		 * doesn't fit into an MSS sized block, so take care of that
		 * now.
		 * Cap len to not accidentally hit GSO_BY_FRAGS.
		 */
		partial_segs = min(len, GSO_BY_FRAGS - 1) / mss;
		if (partial_segs > 1)
			mss *= partial_segs;
		else
			partial_segs = 0;
	}

normal:
	headroom = skb_headroom(head_skb);
	pos = skb_headlen(head_skb);

	if (skb_orphan_frags(head_skb, GFP_ATOMIC))
		return ERR_PTR(-ENOMEM);

	nfrags = skb_shinfo(head_skb)->nr_frags;
	frag = skb_shinfo(head_skb)->frags;
	frag_skb = head_skb;

	do {
		struct sk_buff *nskb;
		skb_frag_t *nskb_frag;
		int hsize;
		int size;

		if (unlikely(mss == GSO_BY_FRAGS)) {
			len = list_skb->len;
		} else {
			len = head_skb->len - offset;
			if (len > mss)
				len = mss;
		}

		hsize = skb_headlen(head_skb) - offset;

		if (hsize <= 0 && i >= nfrags && skb_headlen(list_skb) &&
		    (skb_headlen(list_skb) == len || sg)) {
			BUG_ON(skb_headlen(list_skb) > len);

			nskb = skb_clone(list_skb, GFP_ATOMIC);
			if (unlikely(!nskb))
				goto err;

			i = 0;
			nfrags = skb_shinfo(list_skb)->nr_frags;
			frag = skb_shinfo(list_skb)->frags;
			frag_skb = list_skb;
			pos += skb_headlen(list_skb);

			while (pos < offset + len) {
				BUG_ON(i >= nfrags);

				size = skb_frag_size(frag);
				if (pos + size > offset + len)
					break;

				i++;
				pos += size;
				frag++;
			}

			list_skb = list_skb->next;

			if (unlikely(pskb_trim(nskb, len))) {
				kfree_skb(nskb);
				goto err;
			}

			hsize = skb_end_offset(nskb);
			if (skb_cow_head(nskb, doffset + headroom)) {
				kfree_skb(nskb);
				goto err;
			}

			nskb->truesize += skb_end_offset(nskb) - hsize;
			skb_release_head_state(nskb);
			__skb_push(nskb, doffset);
		} else {
			if (hsize < 0)
				hsize = 0;
			if (hsize > len || !sg)
				hsize = len;

			nskb = __alloc_skb(hsize + doffset + headroom,
					   GFP_ATOMIC, skb_alloc_rx_flag(head_skb),
					   NUMA_NO_NODE);

			if (unlikely(!nskb))
				goto err;

			skb_reserve(nskb, headroom);
			__skb_put(nskb, doffset);
		}

		if (segs)
			tail->next = nskb;
		else
			segs = nskb;
		tail = nskb;

		__copy_skb_header(nskb, head_skb);

		skb_headers_offset_update(nskb, skb_headroom(nskb) - headroom);
		skb_reset_mac_len(nskb);

		skb_copy_from_linear_data_offset(head_skb, -tnl_hlen,
						 nskb->data - tnl_hlen,
						 doffset + tnl_hlen);

		if (nskb->len == len + doffset)
			goto perform_csum_check;

		if (!sg) {
			if (!csum) {
				if (!nskb->remcsum_offload)
					nskb->ip_summed = CHECKSUM_NONE;
				SKB_GSO_CB(nskb)->csum =
					skb_copy_and_csum_bits(head_skb, offset,
							       skb_put(nskb,
								       len),
							       len);
				SKB_GSO_CB(nskb)->csum_start =
					skb_headroom(nskb) + doffset;
			} else {
				if (skb_copy_bits(head_skb, offset, skb_put(nskb, len), len))
					goto err;
			}
			continue;
		}

		nskb_frag = skb_shinfo(nskb)->frags;

		skb_copy_from_linear_data_offset(head_skb, offset,
						 skb_put(nskb, hsize), hsize);

		skb_shinfo(nskb)->flags |= skb_shinfo(head_skb)->flags &
					   SKBFL_SHARED_FRAG;

		if (skb_zerocopy_clone(nskb, frag_skb, GFP_ATOMIC))
			goto err;

		while (pos < offset + len) {
			if (i >= nfrags) {
				if (skb_orphan_frags(list_skb, GFP_ATOMIC) ||
				    skb_zerocopy_clone(nskb, list_skb,
						       GFP_ATOMIC))
					goto err;

				i = 0;
				nfrags = skb_shinfo(list_skb)->nr_frags;
				frag = skb_shinfo(list_skb)->frags;
				frag_skb = list_skb;
				if (!skb_headlen(list_skb)) {
					BUG_ON(!nfrags);
				} else {
					BUG_ON(!list_skb->head_frag);

					/* to make room for head_frag. */
					i--;
					frag--;
				}

				list_skb = list_skb->next;
			}

			if (unlikely(skb_shinfo(nskb)->nr_frags >=
				     MAX_SKB_FRAGS)) {
				net_warn_ratelimited(
					"skb_segment: too many frags: %u %u\n",
					pos, mss);
				err = -EINVAL;
				goto err;
			}

			*nskb_frag = (i < 0) ? skb_head_frag_to_page_desc(frag_skb) : *frag;
			__skb_frag_ref(nskb_frag);
			size = skb_frag_size(nskb_frag);

			if (pos < offset) {
				skb_frag_off_add(nskb_frag, offset - pos);
				skb_frag_size_sub(nskb_frag, offset - pos);
			}

			skb_shinfo(nskb)->nr_frags++;

			if (pos + size <= offset + len) {
				i++;
				frag++;
				pos += size;
			} else {
				skb_frag_size_sub(nskb_frag, pos + size - (offset + len));
				goto skip_fraglist;
			}

			nskb_frag++;
		}

skip_fraglist:
		nskb->data_len = len - hsize;
		nskb->len += nskb->data_len;
		nskb->truesize += nskb->data_len;

perform_csum_check:
		if (!csum) {
			if (skb_has_shared_frag(nskb) &&
			    __skb_linearize(nskb))
				goto err;

			if (!nskb->remcsum_offload)
				nskb->ip_summed = CHECKSUM_NONE;
			SKB_GSO_CB(nskb)->csum =
				skb_checksum(nskb, doffset,
					     nskb->len - doffset, 0);
			SKB_GSO_CB(nskb)->csum_start =
				skb_headroom(nskb) + doffset;
		}
	} while ((offset += len) < head_skb->len);

	/* Some callers want to get the end of the list.
	 * Put it in segs->prev to avoid walking the list.
	 * (see validate_xmit_skb_list() for example)
	 */
	segs->prev = tail;

	if (partial_segs) {
		struct sk_buff *iter;
		int type = skb_shinfo(head_skb)->gso_type;
		unsigned short gso_size = skb_shinfo(head_skb)->gso_size;

		/* Update type to add partial and then remove dodgy if set */
		type |= (features & NETIF_F_GSO_PARTIAL) / NETIF_F_GSO_PARTIAL * SKB_GSO_PARTIAL;
		type &= ~SKB_GSO_DODGY;

		/* Update GSO info and prepare to start updating headers on
		 * our way back down the stack of protocols.
		 */
		for (iter = segs; iter; iter = iter->next) {
			skb_shinfo(iter)->gso_size = gso_size;
			skb_shinfo(iter)->gso_segs = partial_segs;
			skb_shinfo(iter)->gso_type = type;
			SKB_GSO_CB(iter)->data_offset = skb_headroom(iter) + doffset;
		}

		if (tail->len - doffset <= gso_size)
			skb_shinfo(tail)->gso_size = 0;
		else if (tail != segs)
			skb_shinfo(tail)->gso_segs = DIV_ROUND_UP(tail->len - doffset, gso_size);
	}

	/* Following permits correct backpressure, for protocols
	 * using skb_set_owner_w().
	 * Idea is to tranfert ownership from head_skb to last segment.
	 */
	if (head_skb->destructor == sock_wfree) {
		swap(tail->truesize, head_skb->truesize);
		swap(tail->destructor, head_skb->destructor);
		swap(tail->sk, head_skb->sk);
	}
	return segs;

err:
	kfree_skb_list(segs);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(skb_segment);

#ifdef CONFIG_SKB_EXTENSIONS
#define SKB_EXT_ALIGN_VALUE	8
#define SKB_EXT_CHUNKSIZEOF(x)	(ALIGN((sizeof(x)), SKB_EXT_ALIGN_VALUE) / SKB_EXT_ALIGN_VALUE)

static const u8 skb_ext_type_len[] = {
#if IS_ENABLED(CONFIG_BRIDGE_NETFILTER)
	[SKB_EXT_BRIDGE_NF] = SKB_EXT_CHUNKSIZEOF(struct nf_bridge_info),
#endif
#ifdef CONFIG_XFRM
	[SKB_EXT_SEC_PATH] = SKB_EXT_CHUNKSIZEOF(struct sec_path),
#endif
#if IS_ENABLED(CONFIG_NET_TC_SKB_EXT)
	[TC_SKB_EXT] = SKB_EXT_CHUNKSIZEOF(struct tc_skb_ext),
#endif
#if IS_ENABLED(CONFIG_MPTCP)
	[SKB_EXT_MPTCP] = SKB_EXT_CHUNKSIZEOF(struct mptcp_ext),
#endif
#if IS_ENABLED(CONFIG_MCTP_FLOWS)
	[SKB_EXT_MCTP] = SKB_EXT_CHUNKSIZEOF(struct mctp_flow),
#endif
};

static __always_inline unsigned int skb_ext_total_length(void)
{
	unsigned int l = SKB_EXT_CHUNKSIZEOF(struct skb_ext);
	int i;

	for (i = 0; i < ARRAY_SIZE(skb_ext_type_len); i++)
		l += skb_ext_type_len[i];

	return l;
}

static void skb_extensions_init(void)
{
	BUILD_BUG_ON(SKB_EXT_NUM >= 8);
#if !IS_ENABLED(CONFIG_KCOV_INSTRUMENT_ALL)
	BUILD_BUG_ON(skb_ext_total_length() > 255);
#endif

	skbuff_ext_cache = kmem_cache_create("skbuff_ext_cache",
					     SKB_EXT_ALIGN_VALUE * skb_ext_total_length(),
					     0,
					     SLAB_HWCACHE_ALIGN|SLAB_PANIC,
					     NULL);
}
#else
static void skb_extensions_init(void) {}
#endif

/* The SKB kmem_cache slab is critical for network performance.  Never
 * merge/alias the slab with similar sized objects.  This avoids fragmentation
 * that hurts performance of kmem_cache_{alloc,free}_bulk APIs.
 */
#ifndef CONFIG_SLUB_TINY
#define FLAG_SKB_NO_MERGE	SLAB_NO_MERGE
#else /* CONFIG_SLUB_TINY - simple loop in kmem_cache_alloc_bulk */
#define FLAG_SKB_NO_MERGE	0
#endif

void __init skb_init(void)
{
	net_hotdata.skbuff_cache = kmem_cache_create_usercopy("skbuff_head_cache",
					      sizeof(struct sk_buff),
					      0,
					      SLAB_HWCACHE_ALIGN|SLAB_PANIC|
						FLAG_SKB_NO_MERGE,
					      offsetof(struct sk_buff, cb),
					      sizeof_field(struct sk_buff, cb),
					      NULL);
	net_hotdata.skbuff_fclone_cache = kmem_cache_create("skbuff_fclone_cache",
						sizeof(struct sk_buff_fclones),
						0,
						SLAB_HWCACHE_ALIGN|SLAB_PANIC,
						NULL);
	/* usercopy should only access first SKB_SMALL_HEAD_HEADROOM bytes.
	 * struct skb_shared_info is located at the end of skb->head,
	 * and should not be copied to/from user.
	 */
	net_hotdata.skb_small_head_cache = kmem_cache_create_usercopy("skbuff_small_head",
						SKB_SMALL_HEAD_CACHE_SIZE,
						0,
						SLAB_HWCACHE_ALIGN | SLAB_PANIC,
						0,
						SKB_SMALL_HEAD_HEADROOM,
						NULL);
	skb_extensions_init();
}

static int
__skb_to_sgvec(struct sk_buff *skb, struct scatterlist *sg, int offset, int len,
	       unsigned int recursion_level)
{
	int start = skb_headlen(skb);
	int i, copy = start - offset;
	struct sk_buff *frag_iter;
	int elt = 0;

	if (unlikely(recursion_level >= 24))
		return -EMSGSIZE;

	if (copy > 0) {
		if (copy > len)
			copy = len;
		sg_set_buf(sg, skb->data + offset, copy);
		elt++;
		if ((len -= copy) == 0)
			return elt;
		offset += copy;
	}

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		int end;

		WARN_ON(start > offset + len);

		end = start + skb_frag_size(&skb_shinfo(skb)->frags[i]);
		if ((copy = end - offset) > 0) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			if (unlikely(elt && sg_is_last(&sg[elt - 1])))
				return -EMSGSIZE;

			if (copy > len)
				copy = len;
			sg_set_page(&sg[elt], skb_frag_page(frag), copy,
				    skb_frag_off(frag) + offset - start);
			elt++;
			if (!(len -= copy))
				return elt;
			offset += copy;
		}
		start = end;
	}

	skb_walk_frags(skb, frag_iter) {
		int end, ret;

		WARN_ON(start > offset + len);

		end = start + frag_iter->len;
		if ((copy = end - offset) > 0) {
			if (unlikely(elt && sg_is_last(&sg[elt - 1])))
				return -EMSGSIZE;

			if (copy > len)
				copy = len;
			ret = __skb_to_sgvec(frag_iter, sg+elt, offset - start,
					      copy, recursion_level + 1);
			if (unlikely(ret < 0))
				return ret;
			elt += ret;
			if ((len -= copy) == 0)
				return elt;
			offset += copy;
		}
		start = end;
	}
	BUG_ON(len);
	return elt;
}

/**
 *	skb_to_sgvec - Fill a scatter-gather list from a socket buffer
 *	@skb: Socket buffer containing the buffers to be mapped
 *	@sg: The scatter-gather list to map into
 *	@offset: The offset into the buffer's contents to start mapping
 *	@len: Length of buffer space to be mapped
 *
 *	Fill the specified scatter-gather list with mappings/pointers into a
 *	region of the buffer space attached to a socket buffer. Returns either
 *	the number of scatterlist items used, or -EMSGSIZE if the contents
 *	could not fit.
 */
int skb_to_sgvec(struct sk_buff *skb, struct scatterlist *sg, int offset, int len)
{
	int nsg = __skb_to_sgvec(skb, sg, offset, len, 0);

	if (nsg <= 0)
		return nsg;

	sg_mark_end(&sg[nsg - 1]);

	return nsg;
}
EXPORT_SYMBOL_GPL(skb_to_sgvec);

/* As compared with skb_to_sgvec, skb_to_sgvec_nomark only map skb to given
 * sglist without mark the sg which contain last skb data as the end.
 * So the caller can mannipulate sg list as will when padding new data after
 * the first call without calling sg_unmark_end to expend sg list.
 *
 * Scenario to use skb_to_sgvec_nomark:
 * 1. sg_init_table
 * 2. skb_to_sgvec_nomark(payload1)
 * 3. skb_to_sgvec_nomark(payload2)
 *
 * This is equivalent to:
 * 1. sg_init_table
 * 2. skb_to_sgvec(payload1)
 * 3. sg_unmark_end
 * 4. skb_to_sgvec(payload2)
 *
 * When mapping multiple payload conditionally, skb_to_sgvec_nomark
 * is more preferable.
 */
int skb_to_sgvec_nomark(struct sk_buff *skb, struct scatterlist *sg,
			int offset, int len)
{
	return __skb_to_sgvec(skb, sg, offset, len, 0);
}
EXPORT_SYMBOL_GPL(skb_to_sgvec_nomark);



/**
 *	skb_cow_data - Check that a socket buffer's data buffers are writable
 *	@skb: The socket buffer to check.
 *	@tailbits: Amount of trailing space to be added
 *	@trailer: Returned pointer to the skb where the @tailbits space begins
 *
 *	Make sure that the data buffers attached to a socket buffer are
 *	writable. If they are not, private copies are made of the data buffers
 *	and the socket buffer is set to use these instead.
 *
 *	If @tailbits is given, make sure that there is space to write @tailbits
 *	bytes of data beyond current end of socket buffer.  @trailer will be
 *	set to point to the skb in which this space begins.
 *
 *	The number of scatterlist elements required to completely map the
 *	COW'd and extended socket buffer will be returned.
 */
int skb_cow_data(struct sk_buff *skb, int tailbits, struct sk_buff **trailer)
{
	int copyflag;
	int elt;
	struct sk_buff *skb1, **skb_p;

	/* If skb is cloned or its head is paged, reallocate
	 * head pulling out all the pages (pages are considered not writable
	 * at the moment even if they are anonymous).
	 */
	if ((skb_cloned(skb) || skb_shinfo(skb)->nr_frags) &&
	    !__pskb_pull_tail(skb, __skb_pagelen(skb)))
		return -ENOMEM;

	/* Easy case. Most of packets will go this way. */
	if (!skb_has_frag_list(skb)) {
		/* A little of trouble, not enough of space for trailer.
		 * This should not happen, when stack is tuned to generate
		 * good frames. OK, on miss we reallocate and reserve even more
		 * space, 128 bytes is fair. */

		if (skb_tailroom(skb) < tailbits &&
		    pskb_expand_head(skb, 0, tailbits-skb_tailroom(skb)+128, GFP_ATOMIC))
			return -ENOMEM;

		/* Voila! */
		*trailer = skb;
		return 1;
	}

	/* Misery. We are in troubles, going to mincer fragments... */

	elt = 1;
	skb_p = &skb_shinfo(skb)->frag_list;
	copyflag = 0;

	while ((skb1 = *skb_p) != NULL) {
		int ntail = 0;

		/* The fragment is partially pulled by someone,
		 * this can happen on input. Copy it and everything
		 * after it. */

		if (skb_shared(skb1))
			copyflag = 1;

		/* If the skb is the last, worry about trailer. */

		if (skb1->next == NULL && tailbits) {
			if (skb_shinfo(skb1)->nr_frags ||
			    skb_has_frag_list(skb1) ||
			    skb_tailroom(skb1) < tailbits)
				ntail = tailbits + 128;
		}

		if (copyflag ||
		    skb_cloned(skb1) ||
		    ntail ||
		    skb_shinfo(skb1)->nr_frags ||
		    skb_has_frag_list(skb1)) {
			struct sk_buff *skb2;

			/* Fuck, we are miserable poor guys... */
			if (ntail == 0)
				skb2 = skb_copy(skb1, GFP_ATOMIC);
			else
				skb2 = skb_copy_expand(skb1,
						       skb_headroom(skb1),
						       ntail,
						       GFP_ATOMIC);
			if (unlikely(skb2 == NULL))
				return -ENOMEM;

			if (skb1->sk)
				skb_set_owner_w(skb2, skb1->sk);

			/* Looking around. Are we still alive?
			 * OK, link new skb, drop old one */

			skb2->next = skb1->next;
			*skb_p = skb2;
			kfree_skb(skb1);
			skb1 = skb2;
		}
		elt++;
		*trailer = skb1;
		skb_p = &skb1->next;
	}

	return elt;
}
EXPORT_SYMBOL_GPL(skb_cow_data);

static void sock_rmem_free(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;

	atomic_sub(skb->truesize, &sk->sk_rmem_alloc);
}

static void skb_set_err_queue(struct sk_buff *skb)
{
	/* pkt_type of skbs received on local sockets is never PACKET_OUTGOING.
	 * So, it is safe to (mis)use it to mark skbs on the error queue.
	 */
	skb->pkt_type = PACKET_OUTGOING;
	BUILD_BUG_ON(PACKET_OUTGOING == 0);
}

/*
 * Note: We dont mem charge error packets (no sk_forward_alloc changes)
 */
int sock_queue_err_skb(struct sock *sk, struct sk_buff *skb)
{
	if (atomic_read(&sk->sk_rmem_alloc) + skb->truesize >=
	    (unsigned int)READ_ONCE(sk->sk_rcvbuf))
		return -ENOMEM;

	skb_orphan(skb);
	skb->sk = sk;
	skb->destructor = sock_rmem_free;
	atomic_add(skb->truesize, &sk->sk_rmem_alloc);
	skb_set_err_queue(skb);

	/* before exiting rcu section, make sure dst is refcounted */
	skb_dst_force(skb);

	skb_queue_tail(&sk->sk_error_queue, skb);
	if (!sock_flag(sk, SOCK_DEAD))
		sk_error_report(sk);
	return 0;
}
EXPORT_SYMBOL(sock_queue_err_skb);

static bool is_icmp_err_skb(const struct sk_buff *skb)
{
	return skb && (SKB_EXT_ERR(skb)->ee.ee_origin == SO_EE_ORIGIN_ICMP ||
		       SKB_EXT_ERR(skb)->ee.ee_origin == SO_EE_ORIGIN_ICMP6);
}

struct sk_buff *sock_dequeue_err_skb(struct sock *sk)
{
	struct sk_buff_head *q = &sk->sk_error_queue;
	struct sk_buff *skb, *skb_next = NULL;
	bool icmp_next = false;
	unsigned long flags;

	if (skb_queue_empty_lockless(q))
		return NULL;

	spin_lock_irqsave(&q->lock, flags);
	skb = __skb_dequeue(q);
	if (skb && (skb_next = skb_peek(q))) {
		icmp_next = is_icmp_err_skb(skb_next);
		if (icmp_next)
			sk->sk_err = SKB_EXT_ERR(skb_next)->ee.ee_errno;
	}
	spin_unlock_irqrestore(&q->lock, flags);

	if (is_icmp_err_skb(skb) && !icmp_next)
		sk->sk_err = 0;

	if (skb_next)
		sk_error_report(sk);

	return skb;
}
EXPORT_SYMBOL(sock_dequeue_err_skb);

/**
 * skb_clone_sk - create clone of skb, and take reference to socket
 * @skb: the skb to clone
 *
 * This function creates a clone of a buffer that holds a reference on
 * sk_refcnt.  Buffers created via this function are meant to be
 * returned using sock_queue_err_skb, or free via kfree_skb.
 *
 * When passing buffers allocated with this function to sock_queue_err_skb
 * it is necessary to wrap the call with sock_hold/sock_put in order to
 * prevent the socket from being released prior to being enqueued on
 * the sk_error_queue.
 */
struct sk_buff *skb_clone_sk(struct sk_buff *skb)
{
	struct sock *sk = skb->sk;
	struct sk_buff *clone;

	if (!sk || !refcount_inc_not_zero(&sk->sk_refcnt))
		return NULL;

	clone = skb_clone(skb, GFP_ATOMIC);
	if (!clone) {
		sock_put(sk);
		return NULL;
	}

	clone->sk = sk;
	clone->destructor = sock_efree;

	return clone;
}
EXPORT_SYMBOL(skb_clone_sk);

static void __skb_complete_tx_timestamp(struct sk_buff *skb,
					struct sock *sk,
					int tstype,
					bool opt_stats)
{
	struct sock_exterr_skb *serr;
	int err;

	BUILD_BUG_ON(sizeof(struct sock_exterr_skb) > sizeof(skb->cb));

	serr = SKB_EXT_ERR(skb);
	memset(serr, 0, sizeof(*serr));
	serr->ee.ee_errno = ENOMSG;
	serr->ee.ee_origin = SO_EE_ORIGIN_TIMESTAMPING;
	serr->ee.ee_info = tstype;
	serr->opt_stats = opt_stats;
	serr->header.h4.iif = skb->dev ? skb->dev->ifindex : 0;
	if (READ_ONCE(sk->sk_tsflags) & SOF_TIMESTAMPING_OPT_ID) {
		serr->ee.ee_data = skb_shinfo(skb)->tskey;
		if (sk_is_tcp(sk))
			serr->ee.ee_data -= atomic_read(&sk->sk_tskey);
	}

	err = sock_queue_err_skb(sk, skb);

	if (err)
		kfree_skb(skb);
}

static bool skb_may_tx_timestamp(struct sock *sk, bool tsonly)
{
	bool ret;

	if (likely(tsonly || READ_ONCE(sock_net(sk)->core.sysctl_tstamp_allow_data)))
		return true;

	read_lock_bh(&sk->sk_callback_lock);
	ret = sk->sk_socket && sk->sk_socket->file &&
	      file_ns_capable(sk->sk_socket->file, &init_user_ns, CAP_NET_RAW);
	read_unlock_bh(&sk->sk_callback_lock);
	return ret;
}

void skb_complete_tx_timestamp(struct sk_buff *skb,
			       struct skb_shared_hwtstamps *hwtstamps)
{
	struct sock *sk = skb->sk;

	if (!skb_may_tx_timestamp(sk, false))
		goto err;

	/* Take a reference to prevent skb_orphan() from freeing the socket,
	 * but only if the socket refcount is not zero.
	 */
	if (likely(refcount_inc_not_zero(&sk->sk_refcnt))) {
		*skb_hwtstamps(skb) = *hwtstamps;
		__skb_complete_tx_timestamp(skb, sk, SCM_TSTAMP_SND, false);
		sock_put(sk);
		return;
	}

err:
	kfree_skb(skb);
}
EXPORT_SYMBOL_GPL(skb_complete_tx_timestamp);

static bool skb_tstamp_tx_report_so_timestamping(struct sk_buff *skb,
						 struct skb_shared_hwtstamps *hwtstamps,
						 int tstype)
{
	switch (tstype) {
	case SCM_TSTAMP_SCHED:
		return skb_shinfo(skb)->tx_flags & SKBTX_SCHED_TSTAMP;
	case SCM_TSTAMP_SND:
		return skb_shinfo(skb)->tx_flags & (hwtstamps ? SKBTX_HW_TSTAMP_NOBPF :
						    SKBTX_SW_TSTAMP);
	case SCM_TSTAMP_ACK:
		return TCP_SKB_CB(skb)->txstamp_ack & TSTAMP_ACK_SK;
	case SCM_TSTAMP_COMPLETION:
		return skb_shinfo(skb)->tx_flags & SKBTX_COMPLETION_TSTAMP;
	}

	return false;
}

static void skb_tstamp_tx_report_bpf_timestamping(struct sk_buff *skb,
						  struct skb_shared_hwtstamps *hwtstamps,
						  struct sock *sk,
						  int tstype)
{
	int op;

	switch (tstype) {
	case SCM_TSTAMP_SCHED:
		op = BPF_SOCK_OPS_TSTAMP_SCHED_CB;
		break;
	case SCM_TSTAMP_SND:
		if (hwtstamps) {
			op = BPF_SOCK_OPS_TSTAMP_SND_HW_CB;
			*skb_hwtstamps(skb) = *hwtstamps;
		} else {
			op = BPF_SOCK_OPS_TSTAMP_SND_SW_CB;
		}
		break;
	case SCM_TSTAMP_ACK:
		op = BPF_SOCK_OPS_TSTAMP_ACK_CB;
		break;
	default:
		return;
	}

	bpf_skops_tx_timestamping(sk, skb, op);
}

void __skb_tstamp_tx(struct sk_buff *orig_skb,
		     const struct sk_buff *ack_skb,
		     struct skb_shared_hwtstamps *hwtstamps,
		     struct sock *sk, int tstype)
{
	struct sk_buff *skb;
	bool tsonly, opt_stats = false;
	u32 tsflags;

	if (!sk)
		return;

	if (skb_shinfo(orig_skb)->tx_flags & SKBTX_BPF)
		skb_tstamp_tx_report_bpf_timestamping(orig_skb, hwtstamps,
						      sk, tstype);

	if (!skb_tstamp_tx_report_so_timestamping(orig_skb, hwtstamps, tstype))
		return;

	tsflags = READ_ONCE(sk->sk_tsflags);
	if (!hwtstamps && !(tsflags & SOF_TIMESTAMPING_OPT_TX_SWHW) &&
	    skb_shinfo(orig_skb)->tx_flags & SKBTX_IN_PROGRESS)
		return;

	tsonly = tsflags & SOF_TIMESTAMPING_OPT_TSONLY;
	if (!skb_may_tx_timestamp(sk, tsonly))
		return;

	if (tsonly) {
#ifdef CONFIG_INET
		if ((tsflags & SOF_TIMESTAMPING_OPT_STATS) &&
		    sk_is_tcp(sk)) {
			skb = tcp_get_timestamping_opt_stats(sk, orig_skb,
							     ack_skb);
			opt_stats = true;
		} else
#endif
			skb = alloc_skb(0, GFP_ATOMIC);
	} else {
		skb = skb_clone(orig_skb, GFP_ATOMIC);

		if (skb_orphan_frags_rx(skb, GFP_ATOMIC)) {
			kfree_skb(skb);
			return;
		}
	}
	if (!skb)
		return;

	if (tsonly) {
		skb_shinfo(skb)->tx_flags |= skb_shinfo(orig_skb)->tx_flags &
					     SKBTX_ANY_TSTAMP;
		skb_shinfo(skb)->tskey = skb_shinfo(orig_skb)->tskey;
	}

	if (hwtstamps)
		*skb_hwtstamps(skb) = *hwtstamps;
	else
		__net_timestamp(skb);

	__skb_complete_tx_timestamp(skb, sk, tstype, opt_stats);
}
EXPORT_SYMBOL_GPL(__skb_tstamp_tx);

void skb_tstamp_tx(struct sk_buff *orig_skb,
		   struct skb_shared_hwtstamps *hwtstamps)
{
	return __skb_tstamp_tx(orig_skb, NULL, hwtstamps, orig_skb->sk,
			       SCM_TSTAMP_SND);
}
EXPORT_SYMBOL_GPL(skb_tstamp_tx);

#ifdef CONFIG_WIRELESS
void skb_complete_wifi_ack(struct sk_buff *skb, bool acked)
{
	struct sock *sk = skb->sk;
	struct sock_exterr_skb *serr;
	int err = 1;

	skb->wifi_acked_valid = 1;
	skb->wifi_acked = acked;

	serr = SKB_EXT_ERR(skb);
	memset(serr, 0, sizeof(*serr));
	serr->ee.ee_errno = ENOMSG;
	serr->ee.ee_origin = SO_EE_ORIGIN_TXSTATUS;

	/* Take a reference to prevent skb_orphan() from freeing the socket,
	 * but only if the socket refcount is not zero.
	 */
	if (likely(refcount_inc_not_zero(&sk->sk_refcnt))) {
		err = sock_queue_err_skb(sk, skb);
		sock_put(sk);
	}
	if (err)
		kfree_skb(skb);
}
EXPORT_SYMBOL_GPL(skb_complete_wifi_ack);
#endif /* CONFIG_WIRELESS */

/**
 * skb_partial_csum_set - set up and verify partial csum values for packet
 * @skb: the skb to set
 * @start: the number of bytes after skb->data to start checksumming.
 * @off: the offset from start to place the checksum.
 *
 * For untrusted partially-checksummed packets, we need to make sure the values
 * for skb->csum_start and skb->csum_offset are valid so we don't oops.
 *
 * This function checks and sets those values and skb->ip_summed: if this
 * returns false you should drop the packet.
 */
bool skb_partial_csum_set(struct sk_buff *skb, u16 start, u16 off)
{
	u32 csum_end = (u32)start + (u32)off + sizeof(__sum16);
	u32 csum_start = skb_headroom(skb) + (u32)start;

	if (unlikely(csum_start >= U16_MAX || csum_end > skb_headlen(skb))) {
		net_warn_ratelimited("bad partial csum: csum=%u/%u headroom=%u headlen=%u\n",
				     start, off, skb_headroom(skb), skb_headlen(skb));
		return false;
	}
	skb->ip_summed = CHECKSUM_PARTIAL;
	skb->csum_start = csum_start;
	skb->csum_offset = off;
	skb->transport_header = csum_start;
	return true;
}
EXPORT_SYMBOL_GPL(skb_partial_csum_set);

static int skb_maybe_pull_tail(struct sk_buff *skb, unsigned int len,
			       unsigned int max)
{
	if (skb_headlen(skb) >= len)
		return 0;

	/* If we need to pullup then pullup to the max, so we
	 * won't need to do it again.
	 */
	if (max > skb->len)
		max = skb->len;

	if (__pskb_pull_tail(skb, max - skb_headlen(skb)) == NULL)
		return -ENOMEM;

	if (skb_headlen(skb) < len)
		return -EPROTO;

	return 0;
}

#define MAX_TCP_HDR_LEN (15 * 4)

static __sum16 *skb_checksum_setup_ip(struct sk_buff *skb,
				      typeof(IPPROTO_IP) proto,
				      unsigned int off)
{
	int err;

	switch (proto) {
	case IPPROTO_TCP:
		err = skb_maybe_pull_tail(skb, off + sizeof(struct tcphdr),
					  off + MAX_TCP_HDR_LEN);
		if (!err && !skb_partial_csum_set(skb, off,
						  offsetof(struct tcphdr,
							   check)))
			err = -EPROTO;
		return err ? ERR_PTR(err) : &tcp_hdr(skb)->check;

	case IPPROTO_UDP:
		err = skb_maybe_pull_tail(skb, off + sizeof(struct udphdr),
					  off + sizeof(struct udphdr));
		if (!err && !skb_partial_csum_set(skb, off,
						  offsetof(struct udphdr,
							   check)))
			err = -EPROTO;
		return err ? ERR_PTR(err) : &udp_hdr(skb)->check;
	}

	return ERR_PTR(-EPROTO);
}

/* This value should be large enough to cover a tagged ethernet header plus
 * maximally sized IP and TCP or UDP headers.
 */
#define MAX_IP_HDR_LEN 128

static int skb_checksum_setup_ipv4(struct sk_buff *skb, bool recalculate)
{
	unsigned int off;
	bool fragment;
	__sum16 *csum;
	int err;

	fragment = false;

	err = skb_maybe_pull_tail(skb,
				  sizeof(struct iphdr),
				  MAX_IP_HDR_LEN);
	if (err < 0)
		goto out;

	if (ip_is_fragment(ip_hdr(skb)))
		fragment = true;

	off = ip_hdrlen(skb);

	err = -EPROTO;

	if (fragment)
		goto out;

	csum = skb_checksum_setup_ip(skb, ip_hdr(skb)->protocol, off);
	if (IS_ERR(csum))
		return PTR_ERR(csum);

	if (recalculate)
		*csum = ~csum_tcpudp_magic(ip_hdr(skb)->saddr,
					   ip_hdr(skb)->daddr,
					   skb->len - off,
					   ip_hdr(skb)->protocol, 0);
	err = 0;

out:
	return err;
}

/* This value should be large enough to cover a tagged ethernet header plus
 * an IPv6 header, all options, and a maximal TCP or UDP header.
 */
#define MAX_IPV6_HDR_LEN 256

#define OPT_HDR(type, skb, off) \
	(type *)(skb_network_header(skb) + (off))

static int skb_checksum_setup_ipv6(struct sk_buff *skb, bool recalculate)
{
	int err;
	u8 nexthdr;
	unsigned int off;
	unsigned int len;
	bool fragment;
	bool done;
	__sum16 *csum;

	fragment = false;
	done = false;

	off = sizeof(struct ipv6hdr);

	err = skb_maybe_pull_tail(skb, off, MAX_IPV6_HDR_LEN);
	if (err < 0)
		goto out;

	nexthdr = ipv6_hdr(skb)->nexthdr;

	len = sizeof(struct ipv6hdr) + ntohs(ipv6_hdr(skb)->payload_len);
	while (off <= len && !done) {
		switch (nexthdr) {
		case IPPROTO_DSTOPTS:
		case IPPROTO_HOPOPTS:
		case IPPROTO_ROUTING: {
			struct ipv6_opt_hdr *hp;

			err = skb_maybe_pull_tail(skb,
						  off +
						  sizeof(struct ipv6_opt_hdr),
						  MAX_IPV6_HDR_LEN);
			if (err < 0)
				goto out;

			hp = OPT_HDR(struct ipv6_opt_hdr, skb, off);
			nexthdr = hp->nexthdr;
			off += ipv6_optlen(hp);
			break;
		}
		case IPPROTO_AH: {
			struct ip_auth_hdr *hp;

			err = skb_maybe_pull_tail(skb,
						  off +
						  sizeof(struct ip_auth_hdr),
						  MAX_IPV6_HDR_LEN);
			if (err < 0)
				goto out;

			hp = OPT_HDR(struct ip_auth_hdr, skb, off);
			nexthdr = hp->nexthdr;
			off += ipv6_authlen(hp);
			break;
		}
		case IPPROTO_FRAGMENT: {
			struct frag_hdr *hp;

			err = skb_maybe_pull_tail(skb,
						  off +
						  sizeof(struct frag_hdr),
						  MAX_IPV6_HDR_LEN);
			if (err < 0)
				goto out;

			hp = OPT_HDR(struct frag_hdr, skb, off);

			if (hp->frag_off & htons(IP6_OFFSET | IP6_MF))
				fragment = true;

			nexthdr = hp->nexthdr;
			off += sizeof(struct frag_hdr);
			break;
		}
		default:
			done = true;
			break;
		}
	}

	err = -EPROTO;

	if (!done || fragment)
		goto out;

	csum = skb_checksum_setup_ip(skb, nexthdr, off);
	if (IS_ERR(csum))
		return PTR_ERR(csum);

	if (recalculate)
		*csum = ~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
					 &ipv6_hdr(skb)->daddr,
					 skb->len - off, nexthdr, 0);
	err = 0;

out:
	return err;
}

/**
 * skb_checksum_setup - set up partial checksum offset
 * @skb: the skb to set up
 * @recalculate: if true the pseudo-header checksum will be recalculated
 */
int skb_checksum_setup(struct sk_buff *skb, bool recalculate)
{
	int err;

	switch (skb->protocol) {
	case htons(ETH_P_IP):
		err = skb_checksum_setup_ipv4(skb, recalculate);
		break;

	case htons(ETH_P_IPV6):
		err = skb_checksum_setup_ipv6(skb, recalculate);
		break;

	default:
		err = -EPROTO;
		break;
	}

	return err;
}
EXPORT_SYMBOL(skb_checksum_setup);

/**
 * skb_checksum_maybe_trim - maybe trims the given skb
 * @skb: the skb to check
 * @transport_len: the data length beyond the network header
 *
 * Checks whether the given skb has data beyond the given transport length.
 * If so, returns a cloned skb trimmed to this transport length.
 * Otherwise returns the provided skb. Returns NULL in error cases
 * (e.g. transport_len exceeds skb length or out-of-memory).
 *
 * Caller needs to set the skb transport header and free any returned skb if it
 * differs from the provided skb.
 */
static struct sk_buff *skb_checksum_maybe_trim(struct sk_buff *skb,
					       unsigned int transport_len)
{
	struct sk_buff *skb_chk;
	unsigned int len = skb_transport_offset(skb) + transport_len;
	int ret;

	if (skb->len < len)
		return NULL;
	else if (skb->len == len)
		return skb;

	skb_chk = skb_clone(skb, GFP_ATOMIC);
	if (!skb_chk)
		return NULL;

	ret = pskb_trim_rcsum(skb_chk, len);
	if (ret) {
		kfree_skb(skb_chk);
		return NULL;
	}

	return skb_chk;
}

/**
 * skb_checksum_trimmed - validate checksum of an skb
 * @skb: the skb to check
 * @transport_len: the data length beyond the network header
 * @skb_chkf: checksum function to use
 *
 * Applies the given checksum function skb_chkf to the provided skb.
 * Returns a checked and maybe trimmed skb. Returns NULL on error.
 *
 * If the skb has data beyond the given transport length, then a
 * trimmed & cloned skb is checked and returned.
 *
 * Caller needs to set the skb transport header and free any returned skb if it
 * differs from the provided skb.
 */
struct sk_buff *skb_checksum_trimmed(struct sk_buff *skb,
				     unsigned int transport_len,
				     __sum16(*skb_chkf)(struct sk_buff *skb))
{
	struct sk_buff *skb_chk;
	unsigned int offset = skb_transport_offset(skb);
	__sum16 ret;

	skb_chk = skb_checksum_maybe_trim(skb, transport_len);
	if (!skb_chk)
		goto err;

	if (!pskb_may_pull(skb_chk, offset))
		goto err;

	skb_pull_rcsum(skb_chk, offset);
	ret = skb_chkf(skb_chk);
	skb_push_rcsum(skb_chk, offset);

	if (ret)
		goto err;

	return skb_chk;

err:
	if (skb_chk && skb_chk != skb)
		kfree_skb(skb_chk);

	return NULL;

}
EXPORT_SYMBOL(skb_checksum_trimmed);

void __skb_warn_lro_forwarding(const struct sk_buff *skb)
{
	net_warn_ratelimited("%s: received packets cannot be forwarded while LRO is enabled\n",
			     skb->dev->name);
}
EXPORT_SYMBOL(__skb_warn_lro_forwarding);

void kfree_skb_partial(struct sk_buff *skb, bool head_stolen)
{
	if (head_stolen) {
		skb_release_head_state(skb);
		kmem_cache_free(net_hotdata.skbuff_cache, skb);
	} else {
		__kfree_skb(skb);
	}
}
EXPORT_SYMBOL(kfree_skb_partial);

/**
 * skb_try_coalesce - try to merge skb to prior one
 * @to: prior buffer
 * @from: buffer to add
 * @fragstolen: pointer to boolean
 * @delta_truesize: how much more was allocated than was requested
 */
bool skb_try_coalesce(struct sk_buff *to, struct sk_buff *from,
		      bool *fragstolen, int *delta_truesize)
{
	struct skb_shared_info *to_shinfo, *from_shinfo;
	int i, delta, len = from->len;

	*fragstolen = false;

	if (skb_cloned(to))
		return false;

	/* In general, avoid mixing page_pool and non-page_pool allocated
	 * pages within the same SKB. In theory we could take full
	 * references if @from is cloned and !@to->pp_recycle but its
	 * tricky (due to potential race with the clone disappearing) and
	 * rare, so not worth dealing with.
	 */
	if (to->pp_recycle != from->pp_recycle)
		return false;

	if (skb_frags_readable(from) != skb_frags_readable(to))
		return false;

	if (len <= skb_tailroom(to) && skb_frags_readable(from)) {
		if (len)
			BUG_ON(skb_copy_bits(from, 0, skb_put(to, len), len));
		*delta_truesize = 0;
		return true;
	}

	to_shinfo = skb_shinfo(to);
	from_shinfo = skb_shinfo(from);
	if (to_shinfo->frag_list || from_shinfo->frag_list)
		return false;
	if (skb_zcopy(to) || skb_zcopy(from))
		return false;

	if (skb_headlen(from) != 0) {
		struct page *page;
		unsigned int offset;

		if (to_shinfo->nr_frags +
		    from_shinfo->nr_frags >= MAX_SKB_FRAGS)
			return false;

		if (skb_head_is_locked(from))
			return false;

		delta = from->truesize - SKB_DATA_ALIGN(sizeof(struct sk_buff));

		page = virt_to_head_page(from->head);
		offset = from->data - (unsigned char *)page_address(page);

		skb_fill_page_desc(to, to_shinfo->nr_frags,
				   page, offset, skb_headlen(from));
		*fragstolen = true;
	} else {
		if (to_shinfo->nr_frags +
		    from_shinfo->nr_frags > MAX_SKB_FRAGS)
			return false;

		delta = from->truesize - SKB_TRUESIZE(skb_end_offset(from));
	}

	WARN_ON_ONCE(delta < len);

	memcpy(to_shinfo->frags + to_shinfo->nr_frags,
	       from_shinfo->frags,
	       from_shinfo->nr_frags * sizeof(skb_frag_t));
	to_shinfo->nr_frags += from_shinfo->nr_frags;

	if (!skb_cloned(from))
		from_shinfo->nr_frags = 0;

	/* if the skb is not cloned this does nothing
	 * since we set nr_frags to 0.
	 */
	if (skb_pp_frag_ref(from)) {
		for (i = 0; i < from_shinfo->nr_frags; i++)
			__skb_frag_ref(&from_shinfo->frags[i]);
	}

	to->truesize += delta;
	to->len += len;
	to->data_len += len;

	*delta_truesize = delta;
	return true;
}
EXPORT_SYMBOL(skb_try_coalesce);

/**
 * skb_scrub_packet - scrub an skb
 *
 * @skb: buffer to clean
 * @xnet: packet is crossing netns
 *
 * skb_scrub_packet can be used after encapsulating or decapsulating a packet
 * into/from a tunnel. Some information have to be cleared during these
 * operations.
 * skb_scrub_packet can also be used to clean a skb before injecting it in
 * another namespace (@xnet == true). We have to clear all information in the
 * skb that could impact namespace isolation.
 */
void skb_scrub_packet(struct sk_buff *skb, bool xnet)
{
	skb->pkt_type = PACKET_HOST;
	skb->skb_iif = 0;
	skb->ignore_df = 0;
	skb_dst_drop(skb);
	skb_ext_reset(skb);
	nf_reset_ct(skb);
	nf_reset_trace(skb);

#ifdef CONFIG_NET_SWITCHDEV
	skb->offload_fwd_mark = 0;
	skb->offload_l3_fwd_mark = 0;
#endif
	ipvs_reset(skb);

	if (!xnet)
		return;

	skb->mark = 0;
	skb_clear_tstamp(skb);
}
EXPORT_SYMBOL_GPL(skb_scrub_packet);

static struct sk_buff *skb_reorder_vlan_header(struct sk_buff *skb)
{
	int mac_len, meta_len;
	void *meta;

	if (skb_cow(skb, skb_headroom(skb)) < 0) {
		kfree_skb(skb);
		return NULL;
	}

	mac_len = skb->data - skb_mac_header(skb);
	if (likely(mac_len > VLAN_HLEN + ETH_TLEN)) {
		memmove(skb_mac_header(skb) + VLAN_HLEN, skb_mac_header(skb),
			mac_len - VLAN_HLEN - ETH_TLEN);
	}

	meta_len = skb_metadata_len(skb);
	if (meta_len) {
		meta = skb_metadata_end(skb) - meta_len;
		memmove(meta + VLAN_HLEN, meta, meta_len);
	}

	skb->mac_header += VLAN_HLEN;
	return skb;
}

struct sk_buff *skb_vlan_untag(struct sk_buff *skb)
{
	struct vlan_hdr *vhdr;
	u16 vlan_tci;

	if (unlikely(skb_vlan_tag_present(skb))) {
		/* vlan_tci is already set-up so leave this for another time */
		return skb;
	}

	skb = skb_share_check(skb, GFP_ATOMIC);
	if (unlikely(!skb))
		goto err_free;
	/* We may access the two bytes after vlan_hdr in vlan_set_encap_proto(). */
	if (unlikely(!pskb_may_pull(skb, VLAN_HLEN + sizeof(unsigned short))))
		goto err_free;

	vhdr = (struct vlan_hdr *)skb->data;
	vlan_tci = ntohs(vhdr->h_vlan_TCI);
	__vlan_hwaccel_put_tag(skb, skb->protocol, vlan_tci);

	skb_pull_rcsum(skb, VLAN_HLEN);
	vlan_set_encap_proto(skb, vhdr);

	skb = skb_reorder_vlan_header(skb);
	if (unlikely(!skb))
		goto err_free;

	skb_reset_network_header(skb);
	if (!skb_transport_header_was_set(skb))
		skb_reset_transport_header(skb);
	skb_reset_mac_len(skb);

	return skb;

err_free:
	kfree_skb(skb);
	return NULL;
}
EXPORT_SYMBOL(skb_vlan_untag);

int skb_ensure_writable(struct sk_buff *skb, unsigned int write_len)
{
	if (!pskb_may_pull(skb, write_len))
		return -ENOMEM;

	if (!skb_cloned(skb) || skb_clone_writable(skb, write_len))
		return 0;

	return pskb_expand_head(skb, 0, 0, GFP_ATOMIC);
}
EXPORT_SYMBOL(skb_ensure_writable);

int skb_ensure_writable_head_tail(struct sk_buff *skb, struct net_device *dev)
{
	int needed_headroom = dev->needed_headroom;
	int needed_tailroom = dev->needed_tailroom;

	/* For tail taggers, we need to pad short frames ourselves, to ensure
	 * that the tail tag does not fail at its role of being at the end of
	 * the packet, once the conduit interface pads the frame. Account for
	 * that pad length here, and pad later.
	 */
	if (unlikely(needed_tailroom && skb->len < ETH_ZLEN))
		needed_tailroom += ETH_ZLEN - skb->len;
	/* skb_headroom() returns unsigned int... */
	needed_headroom = max_t(int, needed_headroom - skb_headroom(skb), 0);
	needed_tailroom = max_t(int, needed_tailroom - skb_tailroom(skb), 0);

	if (likely(!needed_headroom && !needed_tailroom && !skb_cloned(skb)))
		/* No reallocation needed, yay! */
		return 0;

	return pskb_expand_head(skb, needed_headroom, needed_tailroom,
				GFP_ATOMIC);
}
EXPORT_SYMBOL(skb_ensure_writable_head_tail);

/* remove VLAN header from packet and update csum accordingly.
 * expects a non skb_vlan_tag_present skb with a vlan tag payload
 */
int __skb_vlan_pop(struct sk_buff *skb, u16 *vlan_tci)
{
	int offset = skb->data - skb_mac_header(skb);
	int err;

	if (WARN_ONCE(offset,
		      "__skb_vlan_pop got skb with skb->data not at mac header (offset %d)\n",
		      offset)) {
		return -EINVAL;
	}

	err = skb_ensure_writable(skb, VLAN_ETH_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, skb->data + (2 * ETH_ALEN), VLAN_HLEN);

	vlan_remove_tag(skb, vlan_tci);

	skb->mac_header += VLAN_HLEN;

	if (skb_network_offset(skb) < ETH_HLEN)
		skb_set_network_header(skb, ETH_HLEN);

	skb_reset_mac_len(skb);

	return err;
}
EXPORT_SYMBOL(__skb_vlan_pop);

/* Pop a vlan tag either from hwaccel or from payload.
 * Expects skb->data at mac header.
 */
int skb_vlan_pop(struct sk_buff *skb)
{
	u16 vlan_tci;
	__be16 vlan_proto;
	int err;

	if (likely(skb_vlan_tag_present(skb))) {
		__vlan_hwaccel_clear_tag(skb);
	} else {
		if (unlikely(!eth_type_vlan(skb->protocol)))
			return 0;

		err = __skb_vlan_pop(skb, &vlan_tci);
		if (err)
			return err;
	}
	/* move next vlan tag to hw accel tag */
	if (likely(!eth_type_vlan(skb->protocol)))
		return 0;

	vlan_proto = skb->protocol;
	err = __skb_vlan_pop(skb, &vlan_tci);
	if (unlikely(err))
		return err;

	__vlan_hwaccel_put_tag(skb, vlan_proto, vlan_tci);
	return 0;
}
EXPORT_SYMBOL(skb_vlan_pop);

/* Push a vlan tag either into hwaccel or into payload (if hwaccel tag present).
 * Expects skb->data at mac header.
 */
int skb_vlan_push(struct sk_buff *skb, __be16 vlan_proto, u16 vlan_tci)
{
	if (skb_vlan_tag_present(skb)) {
		int offset = skb->data - skb_mac_header(skb);
		int err;

		if (WARN_ONCE(offset,
			      "skb_vlan_push got skb with skb->data not at mac header (offset %d)\n",
			      offset)) {
			return -EINVAL;
		}

		err = __vlan_insert_tag(skb, skb->vlan_proto,
					skb_vlan_tag_get(skb));
		if (err)
			return err;

		skb->protocol = skb->vlan_proto;
		skb->network_header -= VLAN_HLEN;

		skb_postpush_rcsum(skb, skb->data + (2 * ETH_ALEN), VLAN_HLEN);
	}
	__vlan_hwaccel_put_tag(skb, vlan_proto, vlan_tci);
	return 0;
}
EXPORT_SYMBOL(skb_vlan_push);

/**
 * skb_eth_pop() - Drop the Ethernet header at the head of a packet
 *
 * @skb: Socket buffer to modify
 *
 * Drop the Ethernet header of @skb.
 *
 * Expects that skb->data points to the mac header and that no VLAN tags are
 * present.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_eth_pop(struct sk_buff *skb)
{
	if (!pskb_may_pull(skb, ETH_HLEN) || skb_vlan_tagged(skb) ||
	    skb_network_offset(skb) < ETH_HLEN)
		return -EPROTO;

	skb_pull_rcsum(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	skb_reset_mac_len(skb);

	return 0;
}
EXPORT_SYMBOL(skb_eth_pop);

/**
 * skb_eth_push() - Add a new Ethernet header at the head of a packet
 *
 * @skb: Socket buffer to modify
 * @dst: Destination MAC address of the new header
 * @src: Source MAC address of the new header
 *
 * Prepend @skb with a new Ethernet header.
 *
 * Expects that skb->data points to the mac header, which must be empty.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_eth_push(struct sk_buff *skb, const unsigned char *dst,
		 const unsigned char *src)
{
	struct ethhdr *eth;
	int err;

	if (skb_network_offset(skb) || skb_vlan_tag_present(skb))
		return -EPROTO;

	err = skb_cow_head(skb, sizeof(*eth));
	if (err < 0)
		return err;

	skb_push(skb, sizeof(*eth));
	skb_reset_mac_header(skb);
	skb_reset_mac_len(skb);

	eth = eth_hdr(skb);
	ether_addr_copy(eth->h_dest, dst);
	ether_addr_copy(eth->h_source, src);
	eth->h_proto = skb->protocol;

	skb_postpush_rcsum(skb, eth, sizeof(*eth));

	return 0;
}
EXPORT_SYMBOL(skb_eth_push);

/* Update the ethertype of hdr and the skb csum value if required. */
static void skb_mod_eth_type(struct sk_buff *skb, struct ethhdr *hdr,
			     __be16 ethertype)
{
	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		__be16 diff[] = { ~hdr->h_proto, ethertype };

		skb->csum = csum_partial((char *)diff, sizeof(diff), skb->csum);
	}

	hdr->h_proto = ethertype;
}

/**
 * skb_mpls_push() - push a new MPLS header after mac_len bytes from start of
 *                   the packet
 *
 * @skb: buffer
 * @mpls_lse: MPLS label stack entry to push
 * @mpls_proto: ethertype of the new MPLS header (expects 0x8847 or 0x8848)
 * @mac_len: length of the MAC header
 * @ethernet: flag to indicate if the resulting packet after skb_mpls_push is
 *            ethernet
 *
 * Expects skb->data at mac header.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_mpls_push(struct sk_buff *skb, __be32 mpls_lse, __be16 mpls_proto,
		  int mac_len, bool ethernet)
{
	struct mpls_shim_hdr *lse;
	int err;

	if (unlikely(!eth_p_mpls(mpls_proto)))
		return -EINVAL;

	/* Networking stack does not allow simultaneous Tunnel and MPLS GSO. */
	if (skb->encapsulation)
		return -EINVAL;

	err = skb_cow_head(skb, MPLS_HLEN);
	if (unlikely(err))
		return err;

	if (!skb->inner_protocol) {
		skb_set_inner_network_header(skb, skb_network_offset(skb));
		skb_set_inner_protocol(skb, skb->protocol);
	}

	skb_push(skb, MPLS_HLEN);
	memmove(skb_mac_header(skb) - MPLS_HLEN, skb_mac_header(skb),
		mac_len);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, mac_len);
	skb_reset_mac_len(skb);

	lse = mpls_hdr(skb);
	lse->label_stack_entry = mpls_lse;
	skb_postpush_rcsum(skb, lse, MPLS_HLEN);

	if (ethernet && mac_len >= ETH_HLEN)
		skb_mod_eth_type(skb, eth_hdr(skb), mpls_proto);
	skb->protocol = mpls_proto;

	return 0;
}
EXPORT_SYMBOL_GPL(skb_mpls_push);

/**
 * skb_mpls_pop() - pop the outermost MPLS header
 *
 * @skb: buffer
 * @next_proto: ethertype of header after popped MPLS header
 * @mac_len: length of the MAC header
 * @ethernet: flag to indicate if the packet is ethernet
 *
 * Expects skb->data at mac header.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_mpls_pop(struct sk_buff *skb, __be16 next_proto, int mac_len,
		 bool ethernet)
{
	int err;

	if (unlikely(!eth_p_mpls(skb->protocol)))
		return 0;

	err = skb_ensure_writable(skb, mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, mpls_hdr(skb), MPLS_HLEN);
	memmove(skb_mac_header(skb) + MPLS_HLEN, skb_mac_header(skb),
		mac_len);

	__skb_pull(skb, MPLS_HLEN);
	skb_reset_mac_header(skb);
	skb_set_network_header(skb, mac_len);

	if (ethernet && mac_len >= ETH_HLEN) {
		struct ethhdr *hdr;

		/* use mpls_hdr() to get ethertype to account for VLANs. */
		hdr = (struct ethhdr *)((void *)mpls_hdr(skb) - ETH_HLEN);
		skb_mod_eth_type(skb, hdr, next_proto);
	}
	skb->protocol = next_proto;

	return 0;
}
EXPORT_SYMBOL_GPL(skb_mpls_pop);

/**
 * skb_mpls_update_lse() - modify outermost MPLS header and update csum
 *
 * @skb: buffer
 * @mpls_lse: new MPLS label stack entry to update to
 *
 * Expects skb->data at mac header.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_mpls_update_lse(struct sk_buff *skb, __be32 mpls_lse)
{
	int err;

	if (unlikely(!eth_p_mpls(skb->protocol)))
		return -EINVAL;

	err = skb_ensure_writable(skb, skb->mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		__be32 diff[] = { ~mpls_hdr(skb)->label_stack_entry, mpls_lse };

		skb->csum = csum_partial((char *)diff, sizeof(diff), skb->csum);
	}

	mpls_hdr(skb)->label_stack_entry = mpls_lse;

	return 0;
}
EXPORT_SYMBOL_GPL(skb_mpls_update_lse);

/**
 * skb_mpls_dec_ttl() - decrement the TTL of the outermost MPLS header
 *
 * @skb: buffer
 *
 * Expects skb->data at mac header.
 *
 * Returns 0 on success, -errno otherwise.
 */
int skb_mpls_dec_ttl(struct sk_buff *skb)
{
	u32 lse;
	u8 ttl;

	if (unlikely(!eth_p_mpls(skb->protocol)))
		return -EINVAL;

	if (!pskb_may_pull(skb, skb_network_offset(skb) + MPLS_HLEN))
		return -ENOMEM;

	lse = be32_to_cpu(mpls_hdr(skb)->label_stack_entry);
	ttl = (lse & MPLS_LS_TTL_MASK) >> MPLS_LS_TTL_SHIFT;
	if (!--ttl)
		return -EINVAL;

	lse &= ~MPLS_LS_TTL_MASK;
	lse |= ttl << MPLS_LS_TTL_SHIFT;

	return skb_mpls_update_lse(skb, cpu_to_be32(lse));
}
EXPORT_SYMBOL_GPL(skb_mpls_dec_ttl);

/**
 * alloc_skb_with_frags - allocate skb with page frags
 *
 * @header_len: size of linear part
 * @data_len: needed length in frags
 * @order: max page order desired.
 * @errcode: pointer to error code if any
 * @gfp_mask: allocation mask
 *
 * This can be used to allocate a paged skb, given a maximal order for frags.
 */
struct sk_buff *alloc_skb_with_frags(unsigned long header_len,
				     unsigned long data_len,
				     int order,
				     int *errcode,
				     gfp_t gfp_mask)
{
	unsigned long chunk;
	struct sk_buff *skb;
	struct page *page;
	int nr_frags = 0;

	*errcode = -EMSGSIZE;
	if (unlikely(data_len > MAX_SKB_FRAGS * (PAGE_SIZE << order)))
		return NULL;

	*errcode = -ENOBUFS;
	skb = alloc_skb(header_len, gfp_mask);
	if (!skb)
		return NULL;

	while (data_len) {
		if (nr_frags == MAX_SKB_FRAGS - 1)
			goto failure;
		while (order && PAGE_ALIGN(data_len) < (PAGE_SIZE << order))
			order--;

		if (order) {
			page = alloc_pages((gfp_mask & ~__GFP_DIRECT_RECLAIM) |
					   __GFP_COMP |
					   __GFP_NOWARN,
					   order);
			if (!page) {
				order--;
				continue;
			}
		} else {
			page = alloc_page(gfp_mask);
			if (!page)
				goto failure;
		}
		chunk = min_t(unsigned long, data_len,
			      PAGE_SIZE << order);
		skb_fill_page_desc(skb, nr_frags, page, 0, chunk);
		nr_frags++;
		skb->truesize += (PAGE_SIZE << order);
		data_len -= chunk;
	}
	return skb;

failure:
	kfree_skb(skb);
	return NULL;
}
EXPORT_SYMBOL(alloc_skb_with_frags);

/* carve out the first off bytes from skb when off < headlen */
static int pskb_carve_inside_header(struct sk_buff *skb, const u32 off,
				    const int headlen, gfp_t gfp_mask)
{
	int i;
	unsigned int size = skb_end_offset(skb);
	int new_hlen = headlen - off;
	u8 *data;

	if (skb_pfmemalloc(skb))
		gfp_mask |= __GFP_MEMALLOC;

	data = kmalloc_reserve(&size, gfp_mask, NUMA_NO_NODE, NULL);
	if (!data)
		return -ENOMEM;
	size = SKB_WITH_OVERHEAD(size);

	/* Copy real data, and all frags */
	skb_copy_from_linear_data_offset(skb, off, data, new_hlen);
	skb->len -= off;

	memcpy((struct skb_shared_info *)(data + size),
	       skb_shinfo(skb),
	       offsetof(struct skb_shared_info,
			frags[skb_shinfo(skb)->nr_frags]));
	if (skb_cloned(skb)) {
		/* drop the old head gracefully */
		if (skb_orphan_frags(skb, gfp_mask)) {
			skb_kfree_head(data, size);
			return -ENOMEM;
		}
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
			skb_frag_ref(skb, i);
		if (skb_has_frag_list(skb))
			skb_clone_fraglist(skb);
		skb_release_data(skb, SKB_CONSUMED);
	} else {
		/* we can reuse existing recount- all we did was
		 * relocate values
		 */
		skb_free_head(skb);
	}

	skb->head = data;
	skb->data = data;
	skb->head_frag = 0;
	skb_set_end_offset(skb, size);
	skb_set_tail_pointer(skb, skb_headlen(skb));
	skb_headers_offset_update(skb, 0);
	skb->cloned = 0;
	skb->hdr_len = 0;
	skb->nohdr = 0;
	atomic_set(&skb_shinfo(skb)->dataref, 1);

	return 0;
}

static int pskb_carve(struct sk_buff *skb, const u32 off, gfp_t gfp);

/* carve out the first eat bytes from skb's frag_list. May recurse into
 * pskb_carve()
 */
static int pskb_carve_frag_list(struct skb_shared_info *shinfo, int eat,
				gfp_t gfp_mask)
{
	struct sk_buff *list = shinfo->frag_list;
	struct sk_buff *clone = NULL;
	struct sk_buff *insp = NULL;

	do {
		if (!list) {
			pr_err("Not enough bytes to eat. Want %d\n", eat);
			return -EFAULT;
		}
		if (list->len <= eat) {
			/* Eaten as whole. */
			eat -= list->len;
			list = list->next;
			insp = list;
		} else {
			/* Eaten partially. */
			if (skb_shared(list)) {
				clone = skb_clone(list, gfp_mask);
				if (!clone)
					return -ENOMEM;
				insp = list->next;
				list = clone;
			} else {
				/* This may be pulled without problems. */
				insp = list;
			}
			if (pskb_carve(list, eat, gfp_mask) < 0) {
				kfree_skb(clone);
				return -ENOMEM;
			}
			break;
		}
	} while (eat);

	/* Free pulled out fragments. */
	while ((list = shinfo->frag_list) != insp) {
		shinfo->frag_list = list->next;
		consume_skb(list);
	}
	/* And insert new clone at head. */
	if (clone) {
		clone->next = list;
		shinfo->frag_list = clone;
	}
	return 0;
}

/* carve off first len bytes from skb. Split line (off) is in the
 * non-linear part of skb
 */
static int pskb_carve_inside_nonlinear(struct sk_buff *skb, const u32 off,
				       int pos, gfp_t gfp_mask)
{
	int i, k = 0;
	unsigned int size = skb_end_offset(skb);
	u8 *data;
	const int nfrags = skb_shinfo(skb)->nr_frags;
	struct skb_shared_info *shinfo;

	if (skb_pfmemalloc(skb))
		gfp_mask |= __GFP_MEMALLOC;

	data = kmalloc_reserve(&size, gfp_mask, NUMA_NO_NODE, NULL);
	if (!data)
		return -ENOMEM;
	size = SKB_WITH_OVERHEAD(size);

	memcpy((struct skb_shared_info *)(data + size),
	       skb_shinfo(skb), offsetof(struct skb_shared_info, frags[0]));
	if (skb_orphan_frags(skb, gfp_mask)) {
		skb_kfree_head(data, size);
		return -ENOMEM;
	}
	shinfo = (struct skb_shared_info *)(data + size);
	for (i = 0; i < nfrags; i++) {
		int fsize = skb_frag_size(&skb_shinfo(skb)->frags[i]);

		if (pos + fsize > off) {
			shinfo->frags[k] = skb_shinfo(skb)->frags[i];

			if (pos < off) {
				/* Split frag.
				 * We have two variants in this case:
				 * 1. Move all the frag to the second
				 *    part, if it is possible. F.e.
				 *    this approach is mandatory for TUX,
				 *    where splitting is expensive.
				 * 2. Split is accurately. We make this.
				 */
				skb_frag_off_add(&shinfo->frags[0], off - pos);
				skb_frag_size_sub(&shinfo->frags[0], off - pos);
			}
			skb_frag_ref(skb, i);
			k++;
		}
		pos += fsize;
	}
	shinfo->nr_frags = k;
	if (skb_has_frag_list(skb))
		skb_clone_fraglist(skb);

	/* split line is in frag list */
	if (k == 0 && pskb_carve_frag_list(shinfo, off - pos, gfp_mask)) {
		/* skb_frag_unref() is not needed here as shinfo->nr_frags = 0. */
		if (skb_has_frag_list(skb))
			kfree_skb_list(skb_shinfo(skb)->frag_list);
		skb_kfree_head(data, size);
		return -ENOMEM;
	}
	skb_release_data(skb, SKB_CONSUMED);

	skb->head = data;
	skb->head_frag = 0;
	skb->data = data;
	skb_set_end_offset(skb, size);
	skb_reset_tail_pointer(skb);
	skb_headers_offset_update(skb, 0);
	skb->cloned   = 0;
	skb->hdr_len  = 0;
	skb->nohdr    = 0;
	skb->len -= off;
	skb->data_len = skb->len;
	atomic_set(&skb_shinfo(skb)->dataref, 1);
	return 0;
}

/* remove len bytes from the beginning of the skb */
static int pskb_carve(struct sk_buff *skb, const u32 len, gfp_t gfp)
{
	int headlen = skb_headlen(skb);

	if (len < headlen)
		return pskb_carve_inside_header(skb, len, headlen, gfp);
	else
		return pskb_carve_inside_nonlinear(skb, len, headlen, gfp);
}

/* Extract to_copy bytes starting at off from skb, and return this in
 * a new skb
 */
struct sk_buff *pskb_extract(struct sk_buff *skb, int off,
			     int to_copy, gfp_t gfp)
{
	struct sk_buff  *clone = skb_clone(skb, gfp);

	if (!clone)
		return NULL;

	if (pskb_carve(clone, off, gfp) < 0 ||
	    pskb_trim(clone, to_copy)) {
		kfree_skb(clone);
		return NULL;
	}
	return clone;
}
EXPORT_SYMBOL(pskb_extract);

/**
 * skb_condense - try to get rid of fragments/frag_list if possible
 * @skb: buffer
 *
 * Can be used to save memory before skb is added to a busy queue.
 * If packet has bytes in frags and enough tail room in skb->head,
 * pull all of them, so that we can free the frags right now and adjust
 * truesize.
 * Notes:
 *	We do not reallocate skb->head thus can not fail.
 *	Caller must re-evaluate skb->truesize if needed.
 */
void skb_condense(struct sk_buff *skb)
{
	if (skb->data_len) {
		if (skb->data_len > skb->end - skb->tail ||
		    skb_cloned(skb) || !skb_frags_readable(skb))
			return;

		/* Nice, we can free page frag(s) right now */
		__pskb_pull_tail(skb, skb->data_len);
	}
	/* At this point, skb->truesize might be over estimated,
	 * because skb had a fragment, and fragments do not tell
	 * their truesize.
	 * When we pulled its content into skb->head, fragment
	 * was freed, but __pskb_pull_tail() could not possibly
	 * adjust skb->truesize, not knowing the frag truesize.
	 */
	skb->truesize = SKB_TRUESIZE(skb_end_offset(skb));
}
EXPORT_SYMBOL(skb_condense);

#ifdef CONFIG_SKB_EXTENSIONS
static void *skb_ext_get_ptr(struct skb_ext *ext, enum skb_ext_id id)
{
	return (void *)ext + (ext->offset[id] * SKB_EXT_ALIGN_VALUE);
}

/**
 * __skb_ext_alloc - allocate a new skb extensions storage
 *
 * @flags: See kmalloc().
 *
 * Returns the newly allocated pointer. The pointer can later attached to a
 * skb via __skb_ext_set().
 * Note: caller must handle the skb_ext as an opaque data.
 */
struct skb_ext *__skb_ext_alloc(gfp_t flags)
{
	struct skb_ext *new = kmem_cache_alloc(skbuff_ext_cache, flags);

	if (new) {
		memset(new->offset, 0, sizeof(new->offset));
		refcount_set(&new->refcnt, 1);
	}

	return new;
}

static struct skb_ext *skb_ext_maybe_cow(struct skb_ext *old,
					 unsigned int old_active)
{
	struct skb_ext *new;

	if (refcount_read(&old->refcnt) == 1)
		return old;

	new = kmem_cache_alloc(skbuff_ext_cache, GFP_ATOMIC);
	if (!new)
		return NULL;

	memcpy(new, old, old->chunks * SKB_EXT_ALIGN_VALUE);
	refcount_set(&new->refcnt, 1);

#ifdef CONFIG_XFRM
	if (old_active & (1 << SKB_EXT_SEC_PATH)) {
		struct sec_path *sp = skb_ext_get_ptr(old, SKB_EXT_SEC_PATH);
		unsigned int i;

		for (i = 0; i < sp->len; i++)
			xfrm_state_hold(sp->xvec[i]);
	}
#endif
#ifdef CONFIG_MCTP_FLOWS
	if (old_active & (1 << SKB_EXT_MCTP)) {
		struct mctp_flow *flow = skb_ext_get_ptr(old, SKB_EXT_MCTP);

		if (flow->key)
			refcount_inc(&flow->key->refs);
	}
#endif
	__skb_ext_put(old);
	return new;
}

/**
 * __skb_ext_set - attach the specified extension storage to this skb
 * @skb: buffer
 * @id: extension id
 * @ext: extension storage previously allocated via __skb_ext_alloc()
 *
 * Existing extensions, if any, are cleared.
 *
 * Returns the pointer to the extension.
 */
void *__skb_ext_set(struct sk_buff *skb, enum skb_ext_id id,
		    struct skb_ext *ext)
{
	unsigned int newlen, newoff = SKB_EXT_CHUNKSIZEOF(*ext);

	skb_ext_put(skb);
	newlen = newoff + skb_ext_type_len[id];
	ext->chunks = newlen;
	ext->offset[id] = newoff;
	skb->extensions = ext;
	skb->active_extensions = 1 << id;
	return skb_ext_get_ptr(ext, id);
}

/**
 * skb_ext_add - allocate space for given extension, COW if needed
 * @skb: buffer
 * @id: extension to allocate space for
 *
 * Allocates enough space for the given extension.
 * If the extension is already present, a pointer to that extension
 * is returned.
 *
 * If the skb was cloned, COW applies and the returned memory can be
 * modified without changing the extension space of clones buffers.
 *
 * Returns pointer to the extension or NULL on allocation failure.
 */
void *skb_ext_add(struct sk_buff *skb, enum skb_ext_id id)
{
	struct skb_ext *new, *old = NULL;
	unsigned int newlen, newoff;

	if (skb->active_extensions) {
		old = skb->extensions;

		new = skb_ext_maybe_cow(old, skb->active_extensions);
		if (!new)
			return NULL;

		if (__skb_ext_exist(new, id))
			goto set_active;

		newoff = new->chunks;
	} else {
		newoff = SKB_EXT_CHUNKSIZEOF(*new);

		new = __skb_ext_alloc(GFP_ATOMIC);
		if (!new)
			return NULL;
	}

	newlen = newoff + skb_ext_type_len[id];
	new->chunks = newlen;
	new->offset[id] = newoff;
set_active:
	skb->slow_gro = 1;
	skb->extensions = new;
	skb->active_extensions |= 1 << id;
	return skb_ext_get_ptr(new, id);
}
EXPORT_SYMBOL(skb_ext_add);

#ifdef CONFIG_XFRM
static void skb_ext_put_sp(struct sec_path *sp)
{
	unsigned int i;

	for (i = 0; i < sp->len; i++)
		xfrm_state_put(sp->xvec[i]);
}
#endif

#ifdef CONFIG_MCTP_FLOWS
static void skb_ext_put_mctp(struct mctp_flow *flow)
{
	if (flow->key)
		mctp_key_unref(flow->key);
}
#endif

void __skb_ext_del(struct sk_buff *skb, enum skb_ext_id id)
{
	struct skb_ext *ext = skb->extensions;

	skb->active_extensions &= ~(1 << id);
	if (skb->active_extensions == 0) {
		skb->extensions = NULL;
		__skb_ext_put(ext);
#ifdef CONFIG_XFRM
	} else if (id == SKB_EXT_SEC_PATH &&
		   refcount_read(&ext->refcnt) == 1) {
		struct sec_path *sp = skb_ext_get_ptr(ext, SKB_EXT_SEC_PATH);

		skb_ext_put_sp(sp);
		sp->len = 0;
#endif
	}
}
EXPORT_SYMBOL(__skb_ext_del);

void __skb_ext_put(struct skb_ext *ext)
{
	/* If this is last clone, nothing can increment
	 * it after check passes.  Avoids one atomic op.
	 */
	if (refcount_read(&ext->refcnt) == 1)
		goto free_now;

	if (!refcount_dec_and_test(&ext->refcnt))
		return;
free_now:
#ifdef CONFIG_XFRM
	if (__skb_ext_exist(ext, SKB_EXT_SEC_PATH))
		skb_ext_put_sp(skb_ext_get_ptr(ext, SKB_EXT_SEC_PATH));
#endif
#ifdef CONFIG_MCTP_FLOWS
	if (__skb_ext_exist(ext, SKB_EXT_MCTP))
		skb_ext_put_mctp(skb_ext_get_ptr(ext, SKB_EXT_MCTP));
#endif

	kmem_cache_free(skbuff_ext_cache, ext);
}
EXPORT_SYMBOL(__skb_ext_put);
#endif /* CONFIG_SKB_EXTENSIONS */

static void kfree_skb_napi_cache(struct sk_buff *skb)
{
	/* if SKB is a clone, don't handle this case */
	if (skb->fclone != SKB_FCLONE_UNAVAILABLE) {
		__kfree_skb(skb);
		return;
	}

	local_bh_disable();
	__napi_kfree_skb(skb, SKB_CONSUMED);
	local_bh_enable();
}

/**
 * skb_attempt_defer_free - queue skb for remote freeing
 * @skb: buffer
 *
 * Put @skb in a per-cpu list, using the cpu which
 * allocated the skb/pages to reduce false sharing
 * and memory zone spinlock contention.
 */
void skb_attempt_defer_free(struct sk_buff *skb)
{
	int cpu = skb->alloc_cpu;
	struct softnet_data *sd;
	unsigned int defer_max;
	bool kick;

	if (cpu == raw_smp_processor_id() ||
	    WARN_ON_ONCE(cpu >= nr_cpu_ids) ||
	    !cpu_online(cpu)) {
nodefer:	kfree_skb_napi_cache(skb);
		return;
	}

	DEBUG_NET_WARN_ON_ONCE(skb_dst(skb));
	DEBUG_NET_WARN_ON_ONCE(skb->destructor);

	sd = &per_cpu(softnet_data, cpu);
	defer_max = READ_ONCE(net_hotdata.sysctl_skb_defer_max);
	if (READ_ONCE(sd->defer_count) >= defer_max)
		goto nodefer;

	spin_lock_bh(&sd->defer_lock);
	/* Send an IPI every time queue reaches half capacity. */
	kick = sd->defer_count == (defer_max >> 1);
	/* Paired with the READ_ONCE() few lines above */
	WRITE_ONCE(sd->defer_count, sd->defer_count + 1);

	skb->next = sd->defer_list;
	/* Paired with READ_ONCE() in skb_defer_free_flush() */
	WRITE_ONCE(sd->defer_list, skb);
	spin_unlock_bh(&sd->defer_lock);

	/* Make sure to trigger NET_RX_SOFTIRQ on the remote CPU
	 * if we are unlucky enough (this seems very unlikely).
	 */
	if (unlikely(kick))
		kick_defer_list_purge(sd, cpu);
}

static void skb_splice_csum_page(struct sk_buff *skb, struct page *page,
				 size_t offset, size_t len)
{
	const char *kaddr;
	__wsum csum;

	kaddr = kmap_local_page(page);
	csum = csum_partial(kaddr + offset, len, 0);
	kunmap_local(kaddr);
	skb->csum = csum_block_add(skb->csum, csum, skb->len);
}

/**
 * skb_splice_from_iter - Splice (or copy) pages to skbuff
 * @skb: The buffer to add pages to
 * @iter: Iterator representing the pages to be added
 * @maxsize: Maximum amount of pages to be added
 *
 * This is a common helper function for supporting MSG_SPLICE_PAGES.  It
 * extracts pages from an iterator and adds them to the socket buffer if
 * possible, copying them to fragments if not possible (such as if they're slab
 * pages).
 *
 * Returns the amount of data spliced/copied or -EMSGSIZE if there's
 * insufficient space in the buffer to transfer anything.
 */
ssize_t skb_splice_from_iter(struct sk_buff *skb, struct iov_iter *iter,
			     ssize_t maxsize)
{
	size_t frag_limit = READ_ONCE(net_hotdata.sysctl_max_skb_frags);
	struct page *pages[8], **ppages = pages;
	ssize_t spliced = 0, ret = 0;
	unsigned int i;

	while (iter->count > 0) {
		ssize_t space, nr, len;
		size_t off;

		ret = -EMSGSIZE;
		space = frag_limit - skb_shinfo(skb)->nr_frags;
		if (space < 0)
			break;

		/* We might be able to coalesce without increasing nr_frags */
		nr = clamp_t(size_t, space, 1, ARRAY_SIZE(pages));

		len = iov_iter_extract_pages(iter, &ppages, maxsize, nr, 0, &off);
		if (len <= 0) {
			ret = len ?: -EIO;
			break;
		}

		i = 0;
		do {
			struct page *page = pages[i++];
			size_t part = min_t(size_t, PAGE_SIZE - off, len);

			ret = -EIO;
			if (WARN_ON_ONCE(!sendpage_ok(page)))
				goto out;

			ret = skb_append_pagefrags(skb, page, off, part,
						   frag_limit);
			if (ret < 0) {
				iov_iter_revert(iter, len);
				goto out;
			}

			if (skb->ip_summed == CHECKSUM_NONE)
				skb_splice_csum_page(skb, page, off, part);

			off = 0;
			spliced += part;
			maxsize -= part;
			len -= part;
		} while (len > 0);

		if (maxsize <= 0)
			break;
	}

out:
	skb_len_add(skb, spliced);
	return spliced ?: ret;
}
EXPORT_SYMBOL(skb_splice_from_iter);

static __always_inline
size_t memcpy_from_iter_csum(void *iter_from, size_t progress,
			     size_t len, void *to, void *priv2)
{
	__wsum *csum = priv2;
	__wsum next = csum_partial_copy_nocheck(iter_from, to + progress, len);

	*csum = csum_block_add(*csum, next, progress);
	return 0;
}

static __always_inline
size_t copy_from_user_iter_csum(void __user *iter_from, size_t progress,
				size_t len, void *to, void *priv2)
{
	__wsum next, *csum = priv2;

	next = csum_and_copy_from_user(iter_from, to + progress, len);
	*csum = csum_block_add(*csum, next, progress);
	return next ? 0 : len;
}

bool csum_and_copy_from_iter_full(void *addr, size_t bytes,
				  __wsum *csum, struct iov_iter *i)
{
	size_t copied;

	if (WARN_ON_ONCE(!i->data_source))
		return false;
	copied = iterate_and_advance2(i, bytes, addr, csum,
				      copy_from_user_iter_csum,
				      memcpy_from_iter_csum);
	if (likely(copied == bytes))
		return true;
	iov_iter_revert(i, copied);
	return false;
}
EXPORT_SYMBOL(csum_and_copy_from_iter_full);

void get_netmem(netmem_ref netmem)
{
	struct net_iov *niov;

	if (netmem_is_net_iov(netmem)) {
		niov = netmem_to_net_iov(netmem);
		if (net_is_devmem_iov(niov))
			net_devmem_get_net_iov(netmem_to_net_iov(netmem));
		return;
	}
	get_page(netmem_to_page(netmem));
}
EXPORT_SYMBOL(get_netmem);

void put_netmem(netmem_ref netmem)
{
	struct net_iov *niov;

	if (netmem_is_net_iov(netmem)) {
		niov = netmem_to_net_iov(netmem);
		if (net_is_devmem_iov(niov))
			net_devmem_put_net_iov(netmem_to_net_iov(netmem));
		return;
	}

	put_page(netmem_to_page(netmem));
}
EXPORT_SYMBOL(put_netmem);
