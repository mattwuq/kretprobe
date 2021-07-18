/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>

#define QUEUE_METHOD "sah"

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
	uint32_t id;
};

struct freelist_slot {
	uint32_t                slot;
	uint32_t                size;
	uint32_t                mask;
	uint16_t                step;
	uint16_t                bits;
	atomic_t                used;
} __attribute__ ((aligned (L1_CACHE_BYTES)));

struct freelist_head {
	uint32_t                fh_size;	/* rounded to power of 2 */
	uint32_t                fh_mask;	/* (fh_size - 1) */
	uint16_t                fh_bits;	/* log2(fh_size) */
	uint16_t                fh_step;	/* per-core shift stride */
	uint32_t                fh_used;	/* num of elements in list */
	struct freelist_node  **fh_ents;	/* array for krp instances */
	struct freelist_slot   *fh_slot;
};

static inline int freelist_init(struct freelist_head *list, int max)
{
	uint32_t size, cores = roundup_pow_of_two(num_possible_cpus());
	int i;

	list->fh_used = 0;
	list->fh_step = ilog2(L1_CACHE_BYTES / sizeof(void *));
	if (max < (cores << list->fh_step))
		list->fh_size = cores << list->fh_step;
	else
		list->fh_size = roundup_pow_of_two(max);
	list->fh_mask = list->fh_size - 1;
	list->fh_bits = (uint16_t)ilog2(list->fh_size);
	list->fh_step = list->fh_bits - (uint16_t)ilog2(cores);
	size = list->fh_size * sizeof(struct freelist_node *);
	size += num_possible_cpus() * sizeof(struct freelist_slot);
	list->fh_ents = kzalloc(size, GFP_KERNEL);
	if (!list->fh_ents)
		return -ENOMEM;
	list->fh_slot = (struct freelist_slot *)&list->fh_ents[list->fh_size];
	for (i = 0; i < num_possible_cpus(); i++) {
		list->fh_slot[i].slot = (i << list->fh_step) & list->fh_mask;
		list->fh_slot[i].mask = list->fh_mask;
		list->fh_slot[i].step = list->fh_step;
		list->fh_slot[i].bits = list->fh_bits;
		list->fh_slot[i].size = list->fh_size;
	}

	return 0;
}

static inline int freelist_try_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i, tail;

	tail = (list->fh_used << list->fh_step) +
	       (list->fh_used >> (list->fh_bits - list->fh_step));
	for (i = 0; i < list->fh_size; i++) {
		struct freelist_node *item = NULL;
		uint32_t slot = (i + tail) & list->fh_mask;
		if (try_cmpxchg_release(&list->fh_ents[slot], &item, node)) {
			list->fh_used++;
			break;
		}
	}

	return (i >= list->fh_size);
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	struct freelist_slot *cs = &list->fh_slot[raw_smp_processor_id()];
	struct freelist_node **ns = list->fh_ents;
	uint32_t slot, tail;
	int rc = -1;

 	tail = slot = READ_ONCE(cs->slot) & cs->mask;
	do {
		struct freelist_node *item = NULL;
		if (try_cmpxchg_release(&ns[slot], &item, node)) {
			if (slot != tail)
				WRITE_ONCE(cs->slot, slot);
			rc = 0;
			break;
		}
		slot = (slot - 1) & cs->mask;
	} while (1);

	return rc;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *s)
{
	struct freelist_slot *cs = &s->fh_slot[raw_smp_processor_id()];
	struct freelist_node **ns = s->fh_ents;
	struct freelist_node *node = NULL;
	uint32_t i, head;

	head = READ_ONCE(cs->slot);
	for (i = 0; i < cs->size; i++) {
		uint32_t slot = (head + i) & cs->mask;
		struct freelist_node *item = smp_load_acquire(&ns[slot]);
		if (item && try_cmpxchg_release(&ns[slot], &item, NULL)) {
			if (slot != head)
				WRITE_ONCE(cs->slot, slot);
			node = item;
			break;
		}
		if (!READ_ONCE(s->fh_used))
			return NULL;
	}

	return node;
}

static inline void freelist_destroy(struct freelist_head *s, void *context,
                                    int (*release)(void *, void *))
{
	uint32_t i;

	if (!s->fh_ents)
		return;

	for (i = 0; i < s->fh_size; i++) {
		uint32_t slot = i & s->fh_mask;
		struct freelist_node *item = smp_load_acquire(&s->fh_ents[slot]);
		while (item) {
			if (try_cmpxchg_release(&s->fh_ents[slot], &item, NULL)) {
				if (release)
					release(context, item);
				break;
			}
		}
	}

	if (s->fh_ents) {
		kfree(s->fh_ents);
		s->fh_ents = NULL;
	}
}
#endif /* FREELIST_H */
