/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <linux/slab.h>
#include <linux/atomic.h>

#define QUEUE_METHOD "sq"

/*
 * lockless queue for kretprobe instances
 *
 * It's an array-based MPMC lockless queue, solely for better scalability
 * and contention mitigation. It's simple in implementation and compact in
 * memory efficiency. The only concern is to retrieve kretprobe instance
 * records as fast as possible, with both order and fairness ignored.
 */

struct freelist_node {
	struct freelist_node    *next;
	uint32_t id;
};
struct freelist_head {
	uint32_t                fh_size;	/* rounded to power of 2 */
	uint32_t                fh_mask;
	uint16_t                fh_bits;	/* log2(fh_size) */
	uint16_t                fh_step;	/* per-core shift stride */
	uint32_t                fh_used;	/* num of elements in list */
	struct freelist_node  **fh_ents;	/* array for krp instances */
};

static inline int freelist_init(struct freelist_head *list, int max)
{
	uint32_t bits = ilog2(roundup_pow_of_two(max)), size;

	list->fh_size = 1UL << bits;
	list->fh_mask = list->fh_size - 1;
	list->fh_bits = (uint16_t)bits;
	size = list->fh_size * sizeof(struct freelist_node *);
	list->fh_ents = kzalloc(size, GFP_KERNEL);
	if (!list->fh_ents)
		return -ENOMEM;
	list->fh_step = ilog2(L1_CACHE_BYTES / sizeof(void *));
	if (list->fh_size < num_possible_cpus() << list->fh_step) {
		if (list->fh_size <= num_possible_cpus())
			list->fh_step = 0;
		else
			list->fh_step = ilog2(list->fh_size / num_possible_cpus());
	}

	return 0;
}

static inline int freelist_try_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i, hint = list->fh_used << list->fh_step;

	for (i = 0; i < list->fh_size; i++) {
		struct freelist_node *item = NULL;
		uint32_t slot = (i + hint) & list->fh_mask;
		if (try_cmpxchg_release(&list->fh_ents[slot], &item, node)) {
			list->fh_used++;
			break;
		}
	}

	return (i >= list->fh_size);
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i = 0, hint = raw_smp_processor_id() << list->fh_step;

	while (1) {
		struct freelist_node *item = NULL;
		uint32_t slot = (hint + i++) & list->fh_mask;
		if (try_cmpxchg_release(&list->fh_ents[slot], &item, node))
			return 0;
	}

	return -1;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *list)
{
	struct freelist_node *node = NULL;
	uint32_t i, hint = raw_smp_processor_id() << list->fh_step;

	for (i = 0; i < list->fh_size; i++) {
		uint32_t slot = (hint + i) & list->fh_mask;
		struct freelist_node *item = smp_load_acquire(&list->fh_ents[slot]);
		if (item && try_cmpxchg_release(&list->fh_ents[slot], &item, NULL)) {
			node = item;
			break;
		}
	}

	return node;
}

static inline void freelist_destroy(struct freelist_head *list, void *context,
                                    int (*release)(void *, void *))
{
	uint32_t i;

	if (!list->fh_ents)
		return;

	for (i = 0; i < list->fh_size; i++) {
		uint32_t slot = i & list->fh_mask;
		struct freelist_node *item = smp_load_acquire(&list->fh_ents[slot]);
		while (item) {
			if (try_cmpxchg_release(&list->fh_ents[slot], &item, NULL)) {
				if (release)
					release(context, item);
				break;
			}
		}
	}

	if (list->fh_ents) {
		kfree(list->fh_ents);
		list->fh_ents = NULL;
	}
}
