/**************************************************************************
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#define pr_fmt(fmt) "[TTM] " fmt

#include <drm/ttm/ttm_memory.h>
#include <drm/ttm/ttm_module.h>
#include <drm/ttm/ttm_page_alloc.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#define TTM_MEMORY_ALLOC_RETRIES 4

struct ttm_mem_zone {
	struct kobject kobj;
	struct ttm_mem_global *glob;
	const char *name;
	uint64_t zone_mem;
	uint64_t emer_mem;
	uint64_t max_mem;
	uint64_t swap_limit;
	uint64_t used_mem;
};

static struct attribute ttm_mem_sys = {
	.name = "zone_memory",
	.mode = S_IRUGO
};
static struct attribute ttm_mem_emer = {
	.name = "emergency_memory",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_max = {
	.name = "available_memory",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_swap = {
	.name = "swap_limit",
	.mode = S_IRUGO | S_IWUSR
};
static struct attribute ttm_mem_used = {
	.name = "used_memory",
	.mode = S_IRUGO
};

static void ttm_mem_zone_kobj_release(struct kobject *kobj)
{
	struct ttm_mem_zone *zone =
		container_of(kobj, struct ttm_mem_zone, kobj);

	pr_info("Zone %7s: Used memory at exit: %llu kiB\n",
		zone->name, (unsigned long long)zone->used_mem >> 10);
	kfree(zone);
}

static ssize_t ttm_mem_zone_show(struct kobject *kobj,
				 struct attribute *attr,
				 char *buffer)
{
	struct ttm_mem_zone *zone =
		container_of(kobj, struct ttm_mem_zone, kobj);
	uint64_t val = 0;

	lockmgr(&zone->glob->lock, LK_EXCLUSIVE);
	if (attr == &ttm_mem_sys)
		val = zone->zone_mem;
	else if (attr == &ttm_mem_emer)
		val = zone->emer_mem;
	else if (attr == &ttm_mem_max)
		val = zone->max_mem;
	else if (attr == &ttm_mem_swap)
		val = zone->swap_limit;
	else if (attr == &ttm_mem_used)
		val = zone->used_mem;
	lockmgr(&zone->glob->lock, LK_RELEASE);

	return ksnprintf(buffer, PAGE_SIZE, "%llu\n",
			(unsigned long long) val >> 10);
}

static void ttm_check_swapping(struct ttm_mem_global *glob);

static ssize_t ttm_mem_zone_store(struct kobject *kobj,
				  struct attribute *attr,
				  const char *buffer,
				  size_t size)
{
	struct ttm_mem_zone *zone =
		container_of(kobj, struct ttm_mem_zone, kobj);
	int chars;
	unsigned long val;
	uint64_t val64;

	chars = ksscanf(buffer, "%lu", &val);
	if (chars == 0)
		return size;

	val64 = val;
	val64 <<= 10;

	lockmgr(&zone->glob->lock, LK_EXCLUSIVE);
	if (val64 > zone->zone_mem)
		val64 = zone->zone_mem;
	if (attr == &ttm_mem_emer) {
		zone->emer_mem = val64;
		if (zone->max_mem > val64)
			zone->max_mem = val64;
	} else if (attr == &ttm_mem_max) {
		zone->max_mem = val64;
		if (zone->emer_mem < val64)
			zone->emer_mem = val64;
	} else if (attr == &ttm_mem_swap)
		zone->swap_limit = val64;
	lockmgr(&zone->glob->lock, LK_RELEASE);

	ttm_check_swapping(zone->glob);

	return size;
}

static struct attribute *ttm_mem_zone_attrs[] = {
	&ttm_mem_sys,
	&ttm_mem_emer,
	&ttm_mem_max,
	&ttm_mem_swap,
	&ttm_mem_used,
	NULL
};

static const struct sysfs_ops ttm_mem_zone_ops = {
	.show = &ttm_mem_zone_show,
	.store = &ttm_mem_zone_store
};

static struct kobj_type ttm_mem_zone_kobj_type = {
	.release = &ttm_mem_zone_kobj_release,
	.sysfs_ops = &ttm_mem_zone_ops,
	.default_attrs = ttm_mem_zone_attrs,
};

static void ttm_mem_global_kobj_release(struct kobject *kobj)
{
	struct ttm_mem_global *glob =
		container_of(kobj, struct ttm_mem_global, kobj);

	kfree(glob);
}

static struct kobj_type ttm_mem_glob_kobj_type = {
	.release = &ttm_mem_global_kobj_release,
};

static bool ttm_zones_above_swap_target(struct ttm_mem_global *glob,
					bool from_wq, uint64_t extra)
{
	unsigned int i;
	struct ttm_mem_zone *zone;
	uint64_t target;

	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];

		if (from_wq)
			target = zone->swap_limit;
		else if (caps_priv_check_self(SYSCAP_NOVM_MLOCK) == 0)
			target = zone->emer_mem;
		else
			target = zone->max_mem;

		target = (extra > target) ? 0ULL : target;

		if (zone->used_mem > target)
			return true;
	}
	return false;
}

/**
 * At this point we only support a single shrink callback.
 * Extend this if needed, perhaps using a linked list of callbacks.
 * Note that this function is reentrant:
 * many threads may try to swap out at any given time.
 */

static void ttm_shrink(struct ttm_mem_global *glob, bool from_wq,
			uint64_t extra, struct ttm_operation_ctx *ctx)
{
	int ret;

	lockmgr(&glob->lock, LK_EXCLUSIVE);

	while (ttm_zones_above_swap_target(glob, from_wq, extra)) {
		lockmgr(&glob->lock, LK_RELEASE);
		ret = ttm_bo_swapout(glob->bo_glob, ctx);
		lockmgr(&glob->lock, LK_EXCLUSIVE);
		if (unlikely(ret != 0))
			break;
	}

	lockmgr(&glob->lock, LK_RELEASE);
}

static void ttm_shrink_work(struct work_struct *work)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false
	};
	struct ttm_mem_global *glob =
	    container_of(work, struct ttm_mem_global, work);

	ttm_shrink(glob, true, 0ULL, &ctx);
}

static int ttm_mem_init_kernel_zone(struct ttm_mem_global *glob,
    uint64_t mem)
{
	struct ttm_mem_zone *zone = kzalloc(sizeof(*zone), GFP_KERNEL);
	int ret;

	zone->name = "kernel";
	zone->zone_mem = mem;
	zone->max_mem = mem >> 1;
	zone->emer_mem = (mem >> 1) + (mem >> 2);
	zone->swap_limit = zone->max_mem - (mem >> 3);
	zone->used_mem = 0;
	zone->glob = glob;
	glob->zone_kernel = zone;
	ret = kobject_init_and_add(
		&zone->kobj, &ttm_mem_zone_kobj_type, &glob->kobj, zone->name);
	if (unlikely(ret != 0)) {
		kobject_put(&zone->kobj);
		return ret;
	}
	glob->zones[glob->num_zones++] = zone;
	return 0;
}

#ifdef CONFIG_HIGHMEM
#else
static int ttm_mem_init_dma32_zone(struct ttm_mem_global *glob,
    uint64_t mem)
{
	struct ttm_mem_zone *zone = kzalloc(sizeof(*zone), GFP_KERNEL);
	int ret;

	/**
	 * No special dma32 zone needed.
	 */

	if ((physmem * PAGE_SIZE) <= ((uint64_t) 1ULL << 32)) {
		kfree(zone);
		return 0;
	}

	/*
	 * Limit max dma32 memory to 4GB for now
	 * until we can figure out how big this
	 * zone really is.
	 */
	if (mem > ((uint64_t) 1ULL << 32))
		mem = ((uint64_t) 1ULL << 32);

	zone->name = "dma32";
	zone->zone_mem = mem;
	zone->max_mem = mem >> 1;
	zone->emer_mem = (mem >> 1) + (mem >> 2);
	zone->swap_limit = zone->max_mem - (mem >> 3);
	zone->used_mem = 0;
	zone->glob = glob;
	glob->zone_dma32 = zone;
	ret = kobject_init_and_add(
		&zone->kobj, &ttm_mem_zone_kobj_type, &glob->kobj, zone->name);
	if (unlikely(ret != 0)) {
		kobject_put(&zone->kobj);
		return ret;
	}
	glob->zones[glob->num_zones++] = zone;
	return 0;
}
#endif

int ttm_mem_global_init(struct ttm_mem_global *glob)
{
	u_int64_t mem;
	int ret;
	int i;
	struct ttm_mem_zone *zone;

	lockinit(&glob->lock, "ttmemglob", 0, 0);
	glob->swap_queue = create_singlethread_workqueue("ttm_swap");
	INIT_WORK(&glob->work, ttm_shrink_work);
	ret = kobject_init_and_add(
		&glob->kobj, &ttm_mem_glob_kobj_type, ttm_get_kobj(), "memory_accounting");
	if (unlikely(ret != 0)) {
		kobject_put(&glob->kobj);
		return ret;
	}

	/*
	 * Managed contiguous memory for TTM.  Only use kernel-reserved
	 * dma memory for TTM, which can be controlled via /boot/loader.conf
	 * (e.g. vm.dma_reserved=256m).  This is the only truly dependable
	 * DMA memory.
	 */
	mem = (uint64_t)vm_contig_avail_pages() * PAGE_SIZE;

	ret = ttm_mem_init_kernel_zone(glob, mem);
	if (unlikely(ret != 0))
		goto out_no_zone;
#ifdef CONFIG_HIGHMEM
	ret = ttm_mem_init_highmem_zone(glob, &si);
	if (unlikely(ret != 0))
		goto out_no_zone;
#else
	ret = ttm_mem_init_dma32_zone(glob, mem);
	if (unlikely(ret != 0))
		goto out_no_zone;
#endif
	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];
		pr_info("Zone %7s: Available graphics memory: %llu kiB\n",
			zone->name, (unsigned long long)zone->max_mem >> 10);
	}
	ttm_page_alloc_init(glob, glob->zone_kernel->max_mem/(2*PAGE_SIZE));
	ttm_dma_page_alloc_init(glob, glob->zone_kernel->max_mem/(2*PAGE_SIZE));
	return 0;
out_no_zone:
	ttm_mem_global_release(glob);
	return ret;
}
EXPORT_SYMBOL(ttm_mem_global_init);

void ttm_mem_global_release(struct ttm_mem_global *glob)
{
	unsigned int i;
	struct ttm_mem_zone *zone;

	/* let the page allocator first stop the shrink work. */
	ttm_page_alloc_fini();
	ttm_dma_page_alloc_fini();

	flush_workqueue(glob->swap_queue);
	destroy_workqueue(glob->swap_queue);
	glob->swap_queue = NULL;
	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];
		kobject_del(&zone->kobj);
		kobject_put(&zone->kobj);
			}
	kobject_del(&glob->kobj);
	kobject_put(&glob->kobj);
}
EXPORT_SYMBOL(ttm_mem_global_release);

static void ttm_check_swapping(struct ttm_mem_global *glob)
{
	bool needs_swapping = false;
	unsigned int i;
	struct ttm_mem_zone *zone;

	lockmgr(&glob->lock, LK_EXCLUSIVE);
	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];
		if (zone->used_mem > zone->swap_limit) {
			needs_swapping = true;
			break;
		}
	}

	lockmgr(&glob->lock, LK_RELEASE);

	if (unlikely(needs_swapping))
		(void)queue_work(glob->swap_queue, &glob->work);

}

static void ttm_mem_global_free_zone(struct ttm_mem_global *glob,
				     struct ttm_mem_zone *single_zone,
				     uint64_t amount)
{
	unsigned int i;
	struct ttm_mem_zone *zone;

	lockmgr(&glob->lock, LK_EXCLUSIVE);
	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];
		if (single_zone && zone != single_zone)
			continue;
		zone->used_mem -= amount;
	}
	lockmgr(&glob->lock, LK_RELEASE);
}

void ttm_mem_global_free(struct ttm_mem_global *glob,
			 uint64_t amount)
{
	return ttm_mem_global_free_zone(glob, NULL, amount);
}
EXPORT_SYMBOL(ttm_mem_global_free);

/*
 * check if the available mem is under lower memory limit
 *
 * a. if no swap disk at all or free swap space is under swap_mem_limit
 * but available system mem is bigger than sys_mem_limit, allow TTM
 * allocation;
 *
 * b. if the available system mem is less than sys_mem_limit but free
 * swap disk is bigger than swap_mem_limit, allow TTM allocation.
 */
bool
ttm_check_under_lowerlimit(struct ttm_mem_global *glob,
			uint64_t num_pages,
			struct ttm_operation_ctx *ctx)
{
	return false;
#if 0
	int64_t available;

	if (ctx->flags & TTM_OPT_FLAG_FORCE_ALLOC)
		return false;

	available = get_nr_swap_pages() + si_mem_available();
	available -= num_pages;
	if (available < glob->lower_mem_limit)
		return true;

	return false;
#endif
}
EXPORT_SYMBOL(ttm_check_under_lowerlimit);

static int ttm_mem_global_reserve(struct ttm_mem_global *glob,
				  struct ttm_mem_zone *single_zone,
				  uint64_t amount, bool reserve)
{
	uint64_t limit;
	int ret = -ENOMEM;
	unsigned int i;
	struct ttm_mem_zone *zone;

	lockmgr(&glob->lock, LK_EXCLUSIVE);
	for (i = 0; i < glob->num_zones; ++i) {
		zone = glob->zones[i];
		if (single_zone && zone != single_zone)
			continue;

		limit = (caps_priv_check_self(SYSCAP_NOVM_MLOCK) == 0) ?
			zone->emer_mem : zone->max_mem;

		if (zone->used_mem > limit)
			goto out_unlock;
	}

	if (reserve) {
		for (i = 0; i < glob->num_zones; ++i) {
			zone = glob->zones[i];
			if (single_zone && zone != single_zone)
				continue;
			zone->used_mem += amount;
		}
	}

	ret = 0;
out_unlock:
	lockmgr(&glob->lock, LK_RELEASE);
	ttm_check_swapping(glob);

	return ret;
}


static int ttm_mem_global_alloc_zone(struct ttm_mem_global *glob,
				     struct ttm_mem_zone *single_zone,
				     uint64_t memory,
				     struct ttm_operation_ctx *ctx)
{
	int count = TTM_MEMORY_ALLOC_RETRIES;

	while (unlikely(ttm_mem_global_reserve(glob,
					       single_zone,
					       memory, true)
			!= 0)) {
		if (ctx->no_wait_gpu)
			return -ENOMEM;
		if (unlikely(count-- == 0))
			return -ENOMEM;
		ttm_shrink(glob, false, memory + (memory >> 2) + 16, ctx);
	}

	return 0;
}

int ttm_mem_global_alloc(struct ttm_mem_global *glob, uint64_t memory,
			 struct ttm_operation_ctx *ctx)
{
	/**
	 * Normal allocations of kernel memory are registered in
	 * all zones.
	 */

	return ttm_mem_global_alloc_zone(glob, NULL, memory, ctx);
}
EXPORT_SYMBOL(ttm_mem_global_alloc);

int ttm_mem_global_alloc_page(struct ttm_mem_global *glob,
			      struct page *page, uint64_t size,
			      struct ttm_operation_ctx *ctx)
{
	struct ttm_mem_zone *zone = NULL;

	/**
	 * Page allocations may be registed in a single zone
	 * only if highmem or !dma32.
	 */

#ifdef CONFIG_HIGHMEM
	if (PageHighMem(page) && glob->zone_highmem != NULL)
		zone = glob->zone_highmem;
#else
	if (glob->zone_dma32 && page_to_pfn(page) > 0x00100000UL)
		zone = glob->zone_kernel;
#endif
	return ttm_mem_global_alloc_zone(glob, zone, size, ctx);
}

void ttm_mem_global_free_page(struct ttm_mem_global *glob, struct page *page,
			      uint64_t size)
{
	struct ttm_mem_zone *zone = NULL;

#ifdef CONFIG_HIGHMEM
	if (PageHighMem(page) && glob->zone_highmem != NULL)
		zone = glob->zone_highmem;
#else
	if (glob->zone_dma32 && page_to_pfn(page) > 0x00100000UL)
		zone = glob->zone_kernel;
#endif
	ttm_mem_global_free_zone(glob, zone, size);
}

size_t ttm_round_pot(size_t size)
{
	if ((size & (size - 1)) == 0)
		return size;
	else if (size > PAGE_SIZE)
		return PAGE_ALIGN(size);
	else {
		size_t tmp_size = 4;

		while (tmp_size < size)
			tmp_size <<= 1;

		return tmp_size;
	}
	return 0;
}
EXPORT_SYMBOL(ttm_round_pot);

uint64_t ttm_get_kernel_zone_memory_size(struct ttm_mem_global *glob)
{
	return glob->zone_kernel->max_mem;
}
EXPORT_SYMBOL(ttm_get_kernel_zone_memory_size);
