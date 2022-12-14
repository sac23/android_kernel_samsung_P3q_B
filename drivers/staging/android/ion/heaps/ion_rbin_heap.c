/*
 * drivers/staging/android/ion/ion_rbin_heap.c
 *
 * Copyright (C) 2011 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/err.h>
#include <linux/device.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/rbinregion.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <linux/ion.h>
#include <asm/cacheflush.h>

#ifndef CONFIG_ION_MSM_HEAPS
#define CREATE_TRACE_POINTS
#endif
#include <trace/events/ion.h>

#include "ion_page_pool.h"
#include "../ion_bltin.h"
#include "../exynos/ion_exynos_prot.h"

#define ION_HEAP_TYPE_RBIN (ION_HEAP_TYPE_MAX - 1)

#define NUM_ORDERS ARRAY_SIZE(orders)

static const unsigned int orders[] = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};

static int order_to_index(unsigned int order)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		if (order == orders[i])
			return i;
	BUG();
	return -1;
}

struct ion_rbin_heap {
	struct ion_bltin_heap bltin_heap;
	unsigned long count;
	wait_queue_head_t waitqueue;
	struct task_struct *task;
	struct task_struct *task_shrink;
	bool task_run;
	bool shrink_run;
	struct ion_page_pool *pools[NUM_ORDERS];
	struct device *dev;
};

#define to_rbin_heap(x) (container_of(to_bltin_heap(x), \
			struct ion_rbin_heap, bltin_heap))

static struct page *alloc_rbin_page(struct ion_rbin_heap *rbin_heap,
				    unsigned long size, bool may_dirty,
				    unsigned long last_size)
{
	struct page *page = ERR_PTR(-ENOMEM);
	phys_addr_t paddr = -ENOMEM;
	void *addr;
	int order;

	trace_ion_rbin_partial_alloc_start(NULL, NULL, size, NULL);
	order = min(get_order(last_size), get_order(size));
	for (; order >= 0; order--) {
		size = min_t(unsigned long, size, PAGE_SIZE << order);
		paddr = ion_rbin_allocate(size);
		if (paddr == -ENOMEM)
			continue;
		if (paddr == -EBUSY)
			page = ERR_PTR(-EBUSY);
		break;
	}

	if (!IS_ERR_VALUE(paddr)) {
		page = phys_to_page(paddr);
		INIT_LIST_HEAD(&page->lru);
		addr = page_address(page);
		if (!may_dirty)
			memset(addr, 0, size);
		set_page_private(page, size);
#if IS_ENABLED(CONFIG_ION_EXYNOS)
		__dma_flush_area(addr, size);
#else
		ion_pages_sync_for_device(rbin_heap->dev, page, size, DMA_BIDIRECTIONAL);
#endif
	}
	trace_ion_rbin_partial_alloc_end(NULL, NULL, size, page);
	return page;
}

static inline void do_expand(struct ion_rbin_heap *rbin_heap,
			     struct page *page, unsigned int nr_pages)
{
	unsigned int rem_nr_pages;
	unsigned int order;
	unsigned int total_nr_pages;
	unsigned int free_nr_page;
	struct page *free_page;
	struct ion_page_pool *pool;

	total_nr_pages = page_private(page) >> PAGE_SHIFT;
	rem_nr_pages = total_nr_pages - nr_pages;
	free_page = page + total_nr_pages;

	while (rem_nr_pages) {
		order = ilog2(rem_nr_pages);
		free_nr_page = 1 << order;
		free_page -= free_nr_page;
		set_page_private(free_page, free_nr_page << PAGE_SHIFT);
		pool = rbin_heap->pools[order_to_index(order)];
		ion_page_pool_free(pool, free_page);
		rem_nr_pages -= free_nr_page;
	}
	set_page_private(page, nr_pages << PAGE_SHIFT);
}

static struct page *alloc_rbin_page_from_pool(struct ion_rbin_heap *rbin_heap,
					      unsigned long size)
{
	struct page *page = NULL;
	unsigned int size_order;
	unsigned int nr_pages;
	int i;

	trace_ion_rbin_pool_alloc_start(rbin_heap->bltin_heap.heap.name, NULL, size, NULL);
	size_order = get_order(size);
	nr_pages = size >> PAGE_SHIFT;

	/* try the same or higher order */
	for (i = NUM_ORDERS - 1; i >= 0; i--) {
		if (orders[i] < size_order)
			continue;
		page = ion_page_pool_only_alloc(rbin_heap->pools[i]);
		if (!page)
			continue;
		if (nr_pages < (1 << orders[i]))
			do_expand(rbin_heap, page, nr_pages);
		goto done;
	}

	/* try lower order */
	for (i = 0; i < NUM_ORDERS; i++) {
		if (orders[i] >= size_order)
			continue;
		page = ion_page_pool_only_alloc(rbin_heap->pools[i]);
		if (!page)
			continue;
		goto done;
	}
done:
	trace_ion_rbin_pool_alloc_end(rbin_heap->bltin_heap.heap.name, NULL,
				      page ? page_private(page) : 0, page);
	if (page)
		atomic_sub(page_private(page) >> PAGE_SHIFT, &rbin_pool_pages);
	return page;
}

static int ion_rbin_heap_allocate(struct ion_heap *heap,
				  struct ion_buffer *buffer,
				  unsigned long size,
				  unsigned long flags)
{
	struct ion_rbin_heap *rbin_heap = to_rbin_heap(heap);
	struct sg_table *table;
	struct scatterlist *sg;
	struct list_head pages;
	struct page *page, *tmp;
	unsigned long size_remain = PAGE_ALIGN(size);
	unsigned long last_size = size_remain;
	unsigned long nr_free;
	int i = 0;
#ifdef CONFIG_ION_EXYNOS
	bool may_dirty = (buffer->flags & ION_FLAG_NOZEROED) ? true : false;
#else
	bool may_dirty = false;
#endif

	ion_bltin_event_begin();

	if (ion_buffer_protected(buffer)) {
		pr_err("ION_EXYNOS_FLAG_PROTECTED is set to non-secure heap %s",
		       heap->name);
		return -EINVAL;
	}

	nr_free = rbin_heap->count - atomic_read(&rbin_allocated_pages);
	if (size_remain > nr_free << PAGE_SHIFT)
		return -ENOMEM;

	trace_ion_rbin_alloc_start(heap->name, buffer, size, NULL);
	INIT_LIST_HEAD(&pages);
	while (size_remain > 0) {
		if (atomic_read(&rbin_pool_pages)) {
			page = alloc_rbin_page_from_pool(rbin_heap,
							 size_remain);
			if (page)
				goto got_pg;
		}
		page = alloc_rbin_page(rbin_heap, size_remain, may_dirty,
				       last_size);
		if (IS_ERR(page))
			goto free_pages;
		else
			last_size = page_private(page);
got_pg:
		list_add_tail(&page->lru, &pages);
		size_remain -= page_private(page);
		i++;
	}
	table = kmalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		goto free_pages;
	if (sg_alloc_table(table, i, GFP_KERNEL))
		goto free_table;

	sg = table->sgl;
	list_for_each_entry_safe(page, tmp, &pages, lru) {
		sg_set_page(sg, page, page_private(page), 0);
		sg = sg_next(sg);
		list_del(&page->lru);
	}
	buffer->sg_table = table;

	trace_ion_rbin_alloc_end(heap->name, buffer, size, NULL);
	ion_add_bltin_buffer(&rbin_heap->bltin_heap, buffer);
	ion_bltin_event_record(ION_BLTIN_EVENT_ALLOC, buffer, begin);
	atomic_add(size >> PAGE_SHIFT, &rbin_allocated_pages);
	return 0;

free_table:
	kfree(table);
free_pages:
	list_for_each_entry_safe(page, tmp, &pages, lru)
		ion_rbin_free(page_to_phys(page), page_private(page));
	trace_ion_rbin_alloc_end(heap->name, buffer, size, (void *)-1UL);
	return -ENOMEM;
}

static void ion_rbin_heap_free(struct ion_buffer *buffer)
{
	struct sg_table *table = buffer->sg_table;
	struct scatterlist *sg;
	struct page *page;
	unsigned long size = buffer->size;
	int i;

	ion_bltin_event_begin();

	trace_ion_rbin_free_start(buffer->heap->name, buffer, size, NULL);
	for_each_sg(table->sgl, sg, table->nents, i) {
		page = sg_page(sg);
		ion_rbin_free(page_to_phys(page), page_private(page));
	}
	sg_free_table(table);
	kfree(table);
	trace_ion_rbin_free_end(buffer->heap->name, buffer, size, NULL);
	ion_bltin_event_record(ION_BLTIN_EVENT_FREE, buffer, begin);
	atomic_sub(size >> PAGE_SHIFT, &rbin_allocated_pages);
}

static struct ion_heap_ops rbin_heap_ops = {
	.allocate = ion_rbin_heap_allocate,
	.free = ion_rbin_heap_free,
};

static struct ion_rbin_heap *g_rbin_heap;

void wake_ion_rbin_heap_prereclaim(void)
{
	if (g_rbin_heap) {
		g_rbin_heap->task_run = 1;
		wake_up(&g_rbin_heap->waitqueue);
	}
}

void wake_ion_rbin_heap_shrink(void)
{
	if (g_rbin_heap) {
		g_rbin_heap->shrink_run = 1;
		wake_up(&g_rbin_heap->waitqueue);
	}
}

static void ion_rbin_heap_destroy_pools(struct ion_page_pool **pools)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++)
		if (pools[i])
			ion_page_pool_destroy(pools[i]);
}

static int ion_rbin_heap_create_pools(struct ion_page_pool **pools)
{
	int i;

	for (i = 0; i < NUM_ORDERS; i++) {
		pools[i] = ion_page_pool_create(GFP_KERNEL, orders[i]);
		if (!pools[i])
			goto err_create_pool;
	}
	return 0;

err_create_pool:
	ion_rbin_heap_destroy_pools(pools);
	return -ENOMEM;
}

static int ion_rbin_heap_prereclaim(void *data)
{
	struct ion_rbin_heap *rbin_heap = data;
	unsigned int order;
	unsigned long total_size;
	unsigned long size = PAGE_SIZE << orders[0];
	unsigned long last_size;
	struct ion_page_pool *pool;
	struct page *page;
	unsigned long jiffies_bstop;

	while (true) {
		wait_event_freezable(rbin_heap->waitqueue, rbin_heap->task_run);
		jiffies_bstop = jiffies + (HZ / 10);
		trace_printk("%s\n", "start");
		total_size = 0;
		last_size = size;
		while (true) {
			page = alloc_rbin_page(rbin_heap, size, false,
					       last_size);
			if (PTR_ERR(page) == -ENOMEM)
				break;
			if (PTR_ERR(page) == -EBUSY) {
				if (time_is_after_jiffies(jiffies_bstop))
					continue;
				else
					break;
			}
			last_size = page_private(page);
			order = get_order(page_private(page));
			pool = rbin_heap->pools[order_to_index(order)];
			ion_page_pool_free(pool, page);
			total_size += page_private(page);
			atomic_add(1 << order, &rbin_pool_pages);
		}
		trace_printk("end %lu\n", total_size);
		rbin_heap->task_run = 0;
	}
	return 0;
}

static int ion_rbin_heap_shrink(void *data)
{
	struct ion_rbin_heap *rbin_heap = data;
	unsigned long total_size;
	unsigned long size = PAGE_SIZE << orders[0];
	struct page *page;

	while (true) {
		wait_event_freezable(rbin_heap->waitqueue, rbin_heap->shrink_run);
		trace_printk("%s", "start\n");
		total_size = 0;
		while (true) {
			page = alloc_rbin_page_from_pool(rbin_heap, size);
			if (!page)
				break;
			ion_rbin_free(page_to_phys(page), page_private(page));
			total_size += page_private(page);
		}
		trace_printk("%lu\n", total_size);
		rbin_heap->shrink_run = 0;
	}
	return 0;
}

static int ion_rbin_heap_probe(struct platform_device *pdev)
{
	struct ion_rbin_heap *rbin_heap;
	struct reserved_mem *rmem;
	struct device_node *rmem_np;
	int ret;

	rmem_np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	rmem = of_reserved_mem_lookup(rmem_np);
	if (!rmem) {
		pr_err("%s: memory-region handle not found", pdev->name);
		return -ENODEV;
	}
	if (!rmem->size) {
		pr_err("%s: memory-region has no size", pdev->name);
		return -ENOMEM;
	}

	rbin_heap = kzalloc(sizeof(struct ion_rbin_heap), GFP_KERNEL);
	if (!rbin_heap)
		return -ENOMEM;

	rbin_heap->bltin_heap.heap.ops = &rbin_heap_ops;
	rbin_heap->bltin_heap.heap.type = (enum ion_heap_type)ION_HEAP_TYPE_RBIN;
	rbin_heap->bltin_heap.heap.name = kstrndup(rmem->name, MAX_HEAP_NAME - 1, GFP_KERNEL);
	rbin_heap->count = rmem->size >> PAGE_SHIFT;
	rbin_heap->dev = &pdev->dev;

	if (ion_rbin_heap_create_pools(rbin_heap->pools)) {
		kfree(rbin_heap);
		return -ENOMEM;
	}
	if (init_rbinregion(rmem->base, rmem->size)) {
		ion_rbin_heap_destroy_pools(rbin_heap->pools);
		kfree(rbin_heap);
		return -ENOMEM;
	}
	init_waitqueue_head(&rbin_heap->waitqueue);
	rbin_heap->task = kthread_run(ion_rbin_heap_prereclaim, rbin_heap, "rbin");
	rbin_heap->task_shrink = kthread_run(ion_rbin_heap_shrink, rbin_heap, "rbin_shrink");
	g_rbin_heap = rbin_heap;

	ret = ion_device_add_heap(&rbin_heap->bltin_heap.heap);
	if (ret) {
		return ret;
	}

	// should be called after ion_device_add_heap
	ion_bltin_heap_init(&rbin_heap->bltin_heap);

	platform_set_drvdata(pdev, rbin_heap);
	pr_info("Registered ion rbin heap successfully\n");
	return 0;
}

static int ion_rbin_heap_remove(struct platform_device *pdev)
{
	struct ion_rbin_heap *rbin_heap = platform_get_drvdata(pdev);

	ion_device_remove_heap(&rbin_heap->bltin_heap.heap);
	ion_rbin_heap_destroy_pools(rbin_heap->pools);
	kfree(rbin_heap);

	return 0;
}

static const struct of_device_id ion_rbin_heap_of_match[] = {
	{ .compatible = "ion,rbin_heap", },
	{ },
};

MODULE_DEVICE_TABLE(of, ion_rbin_heap_of_match);

static struct platform_driver ion_rbin_heap_driver = {
	.driver		= {
		.name	= "ion_rbin_heap",
		.of_match_table = ion_rbin_heap_of_match,
	},
	.probe		= ion_rbin_heap_probe,
	.remove		= ion_rbin_heap_remove,
};

int __init ion_rbin_heap_init(void)
{
	return platform_driver_register(&ion_rbin_heap_driver);
}

void ion_rbin_heap_exit(void)
{
	platform_driver_unregister(&ion_rbin_heap_driver);
}

module_init(ion_rbin_heap_init);
module_exit(ion_rbin_heap_exit);
MODULE_LICENSE("GPL v2");
