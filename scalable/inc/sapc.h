/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>

/*
 * lockless queue for kretprobe instances
 *
 * It's an array-based MPMC lockless queue, solely for better scalability
 * and contention mitigation. It's simple in implementation and compact in
 * memory efficiency. The only concern is to retrieve kretprobe instance
 * records as fast as possible, with both order and fairness ignored.
 */

struct freelist_node {
	struct freelist_node   *next;
};

struct freelist_slot {
	uint32_t                fs_slot;
	atomic_t                fs_used;
	uint32_t                fs_size;
	uint32_t                fs_mask;
	struct freelist_node  **fs_ents;
	struct freelist_head   *fs_list;
} __attribute__ ((aligned (L1_CACHE_BYTES)));

struct freelist_head {
	uint32_t                fh_size;	/* rounded to power of 2 */
	uint32_t                fh_used;
	uint32_t                fh_cpus;	/* num of possible cores */
	void                   *fh_buff;
	struct freelist_slot  **fh_slot;
};

static inline int freelist_init(struct freelist_head *list, int max)
{
	uint32_t size, mask, i, cpus = num_possible_cpus();

	size = roundup_pow_of_two(max);
	mask = size - 1;
	size = size * sizeof(struct freelist_node *);
	size = ALIGN(size, L1_CACHE_BYTES) + sizeof(struct freelist_slot);
	list->fh_size = (size + sizeof(void *)) * cpus;
	list->fh_buff = kzalloc(list->fh_size, GFP_KERNEL);
	if (!list->fh_buff)
		return -ENOMEM;
	list->fh_slot = (struct freelist_slot **)((char *)list->fh_buff + size * cpus);
	list->fh_cpus = cpus;
	for (i = 0; i < cpus; i++) {
		list->fh_slot[i] = (struct freelist_slot *)((char *)list->fh_buff +
	                                                    i * size);
		list->fh_slot[i]->fs_mask = mask;
		list->fh_slot[i]->fs_size = mask + 1;
		list->fh_slot[i]->fs_ents = (struct freelist_node  **)((char *)list->fh_slot[i]
		                             + sizeof(struct freelist_slot));
		list->fh_slot[i]->fs_list = list;
	}

	return 0;
}

static inline int __try_add_percpu(struct freelist_node *node, struct freelist_slot *slot)
{
	uint32_t i, tail = atomic_read(&slot->fs_used);

	// if (atomic_read(&slot->fs_used) >= slot->fs_size)
        //		return -ENOENT;
	for (i = 0; i < slot->fs_size; i++) {
		struct freelist_node *item = NULL;
		if (try_cmpxchg_release(&slot->fs_ents[tail], &item, node)) {
			atomic_inc(&slot->fs_used);
			return 0;
		}
		tail = (tail + 1) & slot->fs_mask;
	}

	return -ENOENT;
}

static inline int freelist_try_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i, cpu;

	cpu = list->fh_used % list->fh_cpus;
	for (i = 0; i < list->fh_cpus; i++) {
		if (!__try_add_percpu(node, list->fh_slot[cpu])) {
			list->fh_used++;
			return 0;
		}
		cpu = (cpu + 1) % list->fh_cpus;
	}

	return -ENOENT;
}

static inline int __add_percpu(struct freelist_node *node, struct freelist_slot *slot)
{
	uint32_t i, next = READ_ONCE(slot->fs_slot);

	if (atomic_read(&slot->fs_used) >= slot->fs_size)
		return -ENOENT;

	for (i = 0; i < slot->fs_size; i++) {
		struct freelist_node *item = NULL;
		uint32_t tail = (next - i) & slot->fs_mask;
		if (try_cmpxchg_release(&slot->fs_ents[tail], &item, node)) {
			if (tail != next)
				WRITE_ONCE(slot->fs_slot, tail);
			atomic_inc(&slot->fs_used);
			return 0;
		}
	}

	return -ENOENT;
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < list->fh_cpus; i++) {
		if (!__add_percpu(node, list->fh_slot[cpu])) {
			return 0;
		}
		if (++cpu >= list->fh_cpus)
			cpu = 0;
		// prefetch(list->fh_slot[cpu]);
	}

	return -ENOENT;
}

static inline struct freelist_node *__try_get_percpu(struct freelist_slot *slot)
{
	uint32_t i, next = READ_ONCE(slot->fs_slot);

	if (!atomic_read(&slot->fs_used))
		return NULL;

	for (i = 0; i < slot->fs_size; i++) {
		struct freelist_node *item;
		uint32_t tail = (next + i) & slot->fs_mask;
		item = smp_load_acquire(&slot->fs_ents[tail]);
		if (!item)
			continue;
		if (try_cmpxchg_release(&slot->fs_ents[tail], &item, NULL)) {
			if (tail != next)
				WRITE_ONCE(slot->fs_slot, tail);
			atomic_dec(&slot->fs_used);
			return item;
		}
	}

	return NULL;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *s)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < s->fh_cpus; i++) {
		struct freelist_slot *slot = s->fh_slot[cpu];
		struct freelist_node *node;
		if (++cpu >= s->fh_cpus)
			cpu = 0;
		prefetch(s->fh_slot[cpu]);
		node = __try_get_percpu(slot);
		if (node)
			return node;
	}

	return NULL;
}

static inline void freelist_destroy(struct freelist_head *s, void *context,
                                    int (*release)(void *, void *))
{
	uint32_t i;

	if (!s->fh_slot)
		return;

 	for (i = 0; i < s->fh_cpus; i++) {
		struct freelist_node *node;
		do {
			node = __try_get_percpu(s->fh_slot[i]);
			if (node && release)
				release(context, node);
		} while (node);
	}

	if (s->fh_buff) {
		kfree(s->fh_buff);
		s->fh_buff = NULL;
		s->fh_slot = NULL;
	}
}

#endif /* FREELIST_H */