// SPDX-License-Identifier: GPL-2.0
/*
 * DAMON Code for The Physical Address Space
 *
 * Author: SeongJae Park <sj@kernel.org>
 */

#define pr_fmt(fmt) "damon-pa: " fmt

#include <linux/mmu_notifier.h>
#include <linux/page_idle.h>
#include <linux/pagemap.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/memory-tiers.h>
#include <linux/mm_inline.h>

#include "../internal.h"
#include "ops-common.h"

static void damon_pa_mkold(unsigned long paddr)
{
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));

	if (!folio)
		return;

	damon_folio_mkold(folio);
	folio_put(folio);
}

static void __damon_pa_prepare_access_check(struct damon_region *r)
{
	r->sampling_addr = damon_rand(r->ar.start, r->ar.end);

	damon_pa_mkold(r->sampling_addr);
}

static void damon_pa_prepare_access_checks(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t)
			__damon_pa_prepare_access_check(r);
	}
}

static bool damon_pa_young(unsigned long paddr, unsigned long *folio_sz)
{
	struct folio *folio = damon_get_folio(PHYS_PFN(paddr));
	bool accessed;

	if (!folio)
		return false;

	accessed = damon_folio_young(folio);
	*folio_sz = folio_size(folio);
	folio_put(folio);
	return accessed;
}

static void __damon_pa_check_access(struct damon_region *r,
		struct damon_attrs *attrs)
{
	static unsigned long last_addr;
	static unsigned long last_folio_sz = PAGE_SIZE;
	static bool last_accessed;

	/* If the region is in the last checked page, reuse the result */
	if (ALIGN_DOWN(last_addr, last_folio_sz) ==
				ALIGN_DOWN(r->sampling_addr, last_folio_sz)) {
		damon_update_region_access_rate(r, last_accessed, attrs);
		return;
	}

	last_accessed = damon_pa_young(r->sampling_addr, &last_folio_sz);
	damon_update_region_access_rate(r, last_accessed, attrs);

	last_addr = r->sampling_addr;
}

static unsigned int damon_pa_check_accesses(struct damon_ctx *ctx)
{
	struct damon_target *t;
	struct damon_region *r;
	unsigned int max_nr_accesses = 0;

	damon_for_each_target(t, ctx) {
		damon_for_each_region(r, t) {
			__damon_pa_check_access(r, &ctx->attrs);
			max_nr_accesses = max(r->nr_accesses, max_nr_accesses);
		}
	}

	return max_nr_accesses;
}

/*
 * damos_pa_filter_out - Return true if the page should be filtered out.
 */
static bool damos_pa_filter_out(struct damos *scheme, struct folio *folio)
{
	struct damos_filter *filter;

	if (scheme->core_filters_allowed)
		return false;

	damos_for_each_ops_filter(filter, scheme) {
		if (damos_folio_filter_match(filter, folio))
			return !filter->allow;
	}
	return scheme->ops_filters_default_reject;
}

static bool damon_pa_invalid_damos_folio(struct folio *folio, struct damos *s)
{
	if (!folio)
		return true;
	if (folio == s->last_applied) {
		folio_put(folio);
		return true;
	}
	return false;
}

static unsigned long damon_pa_pageout(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied;
	LIST_HEAD(folio_list);
	bool install_young_filter = true;
	struct damos_filter *filter;
	struct folio *folio;

	/* check access in page level again by default */
	damos_for_each_ops_filter(filter, s) {
		if (filter->type == DAMOS_FILTER_TYPE_YOUNG) {
			install_young_filter = false;
			break;
		}
	}
	if (install_young_filter) {
		filter = damos_new_filter(
				DAMOS_FILTER_TYPE_YOUNG, true, false);
		if (!filter)
			return 0;
		damos_add_filter(s, filter);
	}

	addr = r->ar.start;
	while (addr < r->ar.end) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio);

		folio_clear_referenced(folio);
		folio_test_clear_young(folio);
		if (!folio_isolate_lru(folio))
			goto put_folio;
		if (folio_test_unevictable(folio))
			folio_putback_lru(folio);
		else
			list_add(&folio->lru, &folio_list);
put_folio:
		addr += folio_size(folio);
		folio_put(folio);
	}
	if (install_young_filter)
		damos_destroy_filter(filter);
	applied = reclaim_pages(&folio_list);
	cond_resched();
	s->last_applied = folio;
	return applied * PAGE_SIZE;
}

static inline unsigned long damon_pa_mark_accessed_or_deactivate(
		struct damon_region *r, struct damos *s, bool mark_accessed,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied = 0;
	struct folio *folio;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio);

		if (mark_accessed)
			folio_mark_accessed(folio);
		else
			folio_deactivate(folio);
		applied += folio_nr_pages(folio);
put_folio:
		addr += folio_size(folio);
		folio_put(folio);
	}
	s->last_applied = folio;
	return applied * PAGE_SIZE;
}

static unsigned long damon_pa_mark_accessed(struct damon_region *r,
	struct damos *s, unsigned long *sz_filter_passed)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, true,
			sz_filter_passed);
}

static unsigned long damon_pa_deactivate_pages(struct damon_region *r,
	struct damos *s, unsigned long *sz_filter_passed)
{
	return damon_pa_mark_accessed_or_deactivate(r, s, false,
			sz_filter_passed);
}

static unsigned long damon_pa_migrate(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr, applied;
	LIST_HEAD(folio_list);
	struct folio *folio;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (damos_pa_filter_out(s, folio))
			goto put_folio;
		else
			*sz_filter_passed += folio_size(folio);

		if (!folio_isolate_lru(folio))
			goto put_folio;
		list_add(&folio->lru, &folio_list);
put_folio:
		addr += folio_size(folio);
		folio_put(folio);
	}
	applied = damon_migrate_pages(&folio_list, s->target_nid);
	cond_resched();
	s->last_applied = folio;
	return applied * PAGE_SIZE;
}

static bool damon_pa_scheme_has_filter(struct damos *s)
{
	struct damos_filter *f;

	damos_for_each_ops_filter(f, s)
		return true;
	return false;
}

static unsigned long damon_pa_stat(struct damon_region *r, struct damos *s,
		unsigned long *sz_filter_passed)
{
	unsigned long addr;
	struct folio *folio;

	if (!damon_pa_scheme_has_filter(s))
		return 0;

	addr = r->ar.start;
	while (addr < r->ar.end) {
		folio = damon_get_folio(PHYS_PFN(addr));
		if (damon_pa_invalid_damos_folio(folio, s)) {
			addr += PAGE_SIZE;
			continue;
		}

		if (!damos_pa_filter_out(s, folio))
			*sz_filter_passed += folio_size(folio);
		addr += folio_size(folio);
		folio_put(folio);
	}
	s->last_applied = folio;
	return 0;
}

static unsigned long damon_pa_apply_scheme(struct damon_ctx *ctx,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme, unsigned long *sz_filter_passed)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_pa_pageout(r, scheme, sz_filter_passed);
	case DAMOS_LRU_PRIO:
		return damon_pa_mark_accessed(r, scheme, sz_filter_passed);
	case DAMOS_LRU_DEPRIO:
		return damon_pa_deactivate_pages(r, scheme, sz_filter_passed);
	case DAMOS_MIGRATE_HOT:
	case DAMOS_MIGRATE_COLD:
		return damon_pa_migrate(r, scheme, sz_filter_passed);
	case DAMOS_STAT:
		return damon_pa_stat(r, scheme, sz_filter_passed);
	default:
		/* DAMOS actions that not yet supported by 'paddr'. */
		break;
	}
	return 0;
}

static int damon_pa_scheme_score(struct damon_ctx *context,
		struct damon_target *t, struct damon_region *r,
		struct damos *scheme)
{
	switch (scheme->action) {
	case DAMOS_PAGEOUT:
		return damon_cold_score(context, r, scheme);
	case DAMOS_LRU_PRIO:
		return damon_hot_score(context, r, scheme);
	case DAMOS_LRU_DEPRIO:
		return damon_cold_score(context, r, scheme);
	case DAMOS_MIGRATE_HOT:
		return damon_hot_score(context, r, scheme);
	case DAMOS_MIGRATE_COLD:
		return damon_cold_score(context, r, scheme);
	default:
		break;
	}

	return DAMOS_MAX_SCORE;
}

static int __init damon_pa_initcall(void)
{
	struct damon_operations ops = {
		.id = DAMON_OPS_PADDR,
		.init = NULL,
		.update = NULL,
		.prepare_access_checks = damon_pa_prepare_access_checks,
		.check_accesses = damon_pa_check_accesses,
		.target_valid = NULL,
		.cleanup = NULL,
		.apply_scheme = damon_pa_apply_scheme,
		.get_scheme_score = damon_pa_scheme_score,
	};

	return damon_register_ops(&ops);
};

subsys_initcall(damon_pa_initcall);
