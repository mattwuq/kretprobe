/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>

#define QUEUE_METHOD "sapc"

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

struct freelist_item {
	struct freelist_node   *node;
	uint32_t		age;
#if __BITS_PER_LONG == 64
	uint32_t                reserved;
#endif
};
struct freelist_slot {
	uint32_t                fs_head;	/* */
	uint32_t                fs_tail;	/* */
	uint32_t                fs_size;	/* */
	uint32_t                fs_mask;	/* */
	uint32_t               *fs_ages;
	struct freelist_item   *fs_ents;	/* */
};

struct freelist_head {
	uint32_t                fh_size;	/* rounded to power of 2 */
	uint32_t                fh_used;	/* */
	uint32_t                fh_cpus;	/* num of possible cores */
	void                   *fh_buff;	/* */
	struct freelist_slot  **fh_slot;	/* */
};

static inline int freelist_init(struct freelist_head *list, int max)
{
	uint32_t size, mask, i, cpus = num_possible_cpus();

	size = roundup_pow_of_two(max);
	mask = size - 1;
	size = sizeof(struct freelist_slot) + sizeof(struct freelist_item) * size;
	size = ALIGN(size, L1_CACHE_BYTES);
	list->fh_size = (size + sizeof(struct freelist_slot *)) * cpus;
	list->fh_buff = kzalloc(list->fh_size, GFP_KERNEL);
	if (!list->fh_buff)
		return -ENOMEM;
	list->fh_slot = (void *)((char *)list->fh_buff + size * cpus);
	list->fh_cpus = cpus;
	for (i = 0; i < cpus; i++) {
		struct freelist_slot **slot = list->fh_slot;
		slot[i] = (void *)((char *)list->fh_buff + i * size);
		slot[i]->fs_mask = mask;
		slot[i]->fs_size = mask + 1;
		slot[i]->fs_ents = (void *)((char *)slot[i] +
				    sizeof(struct freelist_slot));
	}

	return 0;
}

static inline int __try_add_percpu(struct freelist_node *node, struct freelist_slot *slot)
{
	uint32_t tail = atomic_inc_return((atomic_t *)&slot->fs_tail) - 1;

	WRITE_ONCE(slot->fs_ents[tail].node, node);
	smp_store_release(&slot->fs_ents[tail].age, tail);
	return 0;
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
	uint32_t tail = atomic_inc_return((atomic_t *)&slot->fs_tail) - 1;

	WRITE_ONCE(slot->fs_ents[tail & slot->fs_mask].node, node);
	smp_store_release(&slot->fs_ents[tail & slot->fs_mask].age, tail);
	return 0;
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t cpu = raw_smp_processor_id();

	// if (!node)
	//	return -EINVAL;
 	do {
		if (!__add_percpu(node, list->fh_slot[cpu])) {
			return 0;
		}
		if (++cpu >= list->fh_cpus)
			cpu = 0;
		// prefetch(list->fh_slot[cpu]);
	} while (1);

	return -ENOENT;
}

static inline struct freelist_node *__try_get_percpu(struct freelist_slot *slot)
{
	uint32_t head = smp_load_acquire(&slot->fs_head), times = 0;

	while (head != READ_ONCE(slot->fs_tail)) {
		uint32_t id = head & slot->fs_mask;
		struct freelist_item *item = &slot->fs_ents[id];
		if (try_cmpxchg_acquire(&item->age, &head, head - 1)) {
			struct freelist_node *node;
			node = READ_ONCE(item->node);
			if (!node) {
				BUG();
			}
			smp_store_release(&slot->fs_head, head + 1);
			return node;
		}
		/* break if it's in irq/softirq or nmi */
		if (irq_count())
			break;
		if (times++ > 100000) {
			times = 0;
			printk("%u: head: %u/%u tail: %u age: %u node: %px/%px\n",
			       raw_smp_processor_id(), head, READ_ONCE(slot->fs_head),
			       READ_ONCE(slot->fs_tail), item->age, item->node, item);
		}
		head = READ_ONCE(slot->fs_head);
	}

	return NULL;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *s)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < s->fh_cpus; i++) {
		struct freelist_slot *slot = s->fh_slot[cpu];
		struct freelist_node *node;
		node = __try_get_percpu(slot);
		if (node)
			return node;
		if (++cpu >= s->fh_cpus)
			cpu = 0;
		// prefetch(s->fh_slot[cpu]);
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
