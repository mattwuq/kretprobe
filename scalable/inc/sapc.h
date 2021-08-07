/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/vmalloc.h>
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
	uint32_t id;
};

struct freelist_slot {
	uint32_t                fs_head;	/* */
	uint32_t                fs_tail;	/* */
	uint32_t                fs_mask;	/* */
	uint32_t                fs_size;	/* */
	uint32_t               *fs_ages;
	struct freelist_node  **fs_ents;	/* */
};

struct freelist_head {
	uint32_t                fh_record;
	uint32_t                fh_nrecords;	/* */
	uint32_t                fh_nents;
	uint16_t                fh_ncores; /* num of possible cores */
	uint16_t                fh_bulk_mode:1;	/* user-specified buffer */
	uint16_t		fh_in_slot:1;
	uint16_t                fh_vmem:1;	/* allocated from vmem zone */
	gfp_t  			fh_gfp_flags;
	void                   *fh_bulk_buffer;	/* */
	struct freelist_slot  **fh_slots;	/* */
};

static inline uint32_t freelist_num_of_items(uint32_t size)
{
	return rounddown_pow_of_two((size - sizeof(struct freelist_slot))
				          / (sizeof(uint32_t) + sizeof(void *)));
}

static inline int freelist_init_slots(struct freelist_head *list)
{
	uint32_t i, j, size, nents, record = 0, nrecords;

	list->fh_slots = kzalloc(list->fh_ncores * sizeof(void *), GFP_KERNEL);
	if (!list->fh_slots)
		return -ENOMEM;

	nents = list->fh_nents;
	record = ALIGN(list->fh_record, sizeof(void *));

	for (i = 0; i < list->fh_ncores; i++) {

		struct freelist_slot *slot;

		nrecords = list->fh_nrecords / list->fh_ncores;
		if (i < (list->fh_nrecords % list->fh_ncores))
			nrecords++;
		size = sizeof(struct freelist_slot) + sizeof(void *) * nents +
		       sizeof(uint32_t) * nents + record * nrecords;

		// printk("slot %u:\n\trecords: %u * %u  size: %u\n", i, nrecords, record, size);
		if (0 == i) {
			if ((list->fh_gfp_flags & GFP_ATOMIC) || size < PAGE_SIZE)
				list->fh_vmem = 0;
			else
				list->fh_vmem = 1;
			if (record)
				list->fh_in_slot = 1;
		}

		if (list->fh_vmem)
			slot = vmalloc_node(size, cpu_to_node(i));
		else
			slot = kmalloc_node(size, list->fh_gfp_flags, cpu_to_node(i));
		if (!slot)
			return -ENOMEM;

		memset(slot, 0, size);
		slot->fs_size = list->fh_nents;
		slot->fs_mask = slot->fs_size - 1;

		/*
		 * start from 2nd round to avoid conflict of 1st item.
		 * we assume that the head item is ready for retrieval
		 * iff head is equal to ages[head & mask]. but ages is
		 * initialized as 0, so in view of the caller of pop(),
		 * the 1st item (0th) is always ready, but fact could
		 * be: push() is stalled before the final update, thus
		 * the item being inserted will be lost forever.
		 */
		slot->fs_head = slot->fs_tail = slot->fs_size;

		slot->fs_ages = (void *)((char *)slot + sizeof(struct freelist_slot));
		slot->fs_ents = (void *)&slot->fs_ages[slot->fs_size];
		for (j = 0; record && j < nrecords; j++) {
			slot->fs_ents[slot->fs_tail] = (void *)&slot->fs_ents[slot->fs_size] +
						       j * record;
			slot->fs_ages[slot->fs_tail] = slot->fs_tail;
			slot->fs_tail++;
		}
		list->fh_slots[i] = slot;
	}

	return 0;
}

static inline void freelist_free_slots(struct freelist_head *list)
{
	uint32_t i;

	if (!list->fh_slots)
		return;

	for (i = 0; i < list->fh_ncores; i++) {
		if (!list->fh_slots[i])
			continue;
		if (list->fh_vmem)
			vfree(list->fh_slots[i]);
		else
			kfree(list->fh_slots[i]);
	}
	kfree(list->fh_slots);
	list->fh_slots = NULL;
}

static inline int freelist_init_scattered(struct freelist_head *list, int nrecords, int asym, int record, gfp_t gfp)
{
	uint32_t nents, cpus = num_possible_cpus();
	int rc;

	/* initialize freelist_head as zero */
	memset(list, 0, sizeof(struct freelist_head));

	/* caculate per-cpu slot size and num of pre-allocated items */
	if (!asym)
		nents = nrecords / cpus;
	else
		nents = nrecords;
	if (nents < freelist_num_of_items(2 * L1_CACHE_BYTES))
		nents = freelist_num_of_items(2 * L1_CACHE_BYTES);
	nents = roundup_pow_of_two(nents);
	while (nents * cpus < nrecords)
		nents = nents << 1;
	list->fh_ncores = cpus;
	list->fh_record = record;
	list->fh_nents = nents;
	list->fh_nrecords = nrecords;
	list->fh_gfp_flags = gfp & ~__GFP_ZERO;

	/* initialize per-cpu slots */
	rc = freelist_init_slots(list);
	if (rc)
		freelist_free_slots(list);

	return rc;
}

static inline int freelist_init(struct freelist_head *list, int nrecords)
{
	return freelist_init_scattered(list, nrecords, 1, 0, GFP_KERNEL);
}

static inline int __freelist_add_percpu(struct freelist_node *node, struct freelist_slot *slot)
{
	uint32_t tail = atomic_inc_return((atomic_t *)&slot->fs_tail) - 1;

	WRITE_ONCE(slot->fs_ents[tail & slot->fs_mask], node);
	smp_store_release(&slot->fs_ages[tail & slot->fs_mask], tail);
	return 0;
}

static inline int __freelist_try_add_percpu(struct freelist_node *node, struct freelist_slot *slot)
{
	uint32_t head, tail;

	do {
		head = smp_load_acquire(&slot->fs_head);
		tail = READ_ONCE(slot->fs_tail);
		if (tail >= head + slot->fs_size)
			return -ENOENT;
		if (try_cmpxchg_acquire(&slot->fs_tail, &tail, tail + 1))
			break;
	} while (1);

	WRITE_ONCE(slot->fs_ents[tail & slot->fs_mask], node);
	smp_store_release(&slot->fs_ages[tail & slot->fs_mask], tail);
	return 0;
}

static inline int freelist_add_scattered(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t i, cpu;

	if (!node || list->fh_nrecords >= list->fh_ncores * list->fh_nents)
		return -EINVAL;

	cpu = list->fh_nrecords % list->fh_ncores;
	for (i = 0; i < list->fh_ncores; i++) {
		if (!__freelist_add_percpu(node, list->fh_slots[cpu])) {
			list->fh_nrecords++;
			return 0;
		}
		cpu = (cpu + 1) % list->fh_ncores;
	}

	return -ENOENT;
}
#define freelist_try_add freelist_add_scattered

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	uint32_t cpu = raw_smp_processor_id();

 	do {
		if (list->fh_nrecords > list->fh_nents) {
			if (!__freelist_try_add_percpu(node, list->fh_slots[cpu])) {
				return 0;
			}
		} else {
			if (!__freelist_add_percpu(node, list->fh_slots[cpu])) {
				return 0;
			}
		}
		if (++cpu >= list->fh_ncores)
			cpu = 0;
	} while (1);

	return -ENOENT;
}

static inline struct freelist_node *__freelist_try_get_percpu(struct freelist_slot *slot)
{
	uint32_t head = smp_load_acquire(&slot->fs_head);

	while (head != READ_ONCE(slot->fs_tail)) {
		uint32_t id = head & slot->fs_mask, prev = head;
		prefetch(&slot->fs_ents[id]);
		if (smp_load_acquire(&slot->fs_ages[id]) == head){
			struct freelist_node *node;
			node = READ_ONCE(slot->fs_ents[id]);
			if (!node)
				printk("sapc: null node.\n");
			/* commit and move forward head of the slot */
			if (try_cmpxchg_release(&slot->fs_head, &head, head + 1))
				return node;
		}

		/* re-load head from memory continue trying */
		head = READ_ONCE(slot->fs_head);
		/*
		 * head stays unchanged, so it's very likely current pop()
		 * just preempted/interrupted an ongoing push() operation
		 */
		if (head == prev)
			break;
	}

	return NULL;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *s)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < s->fh_ncores; i++) {
		struct freelist_slot *slot = s->fh_slots[cpu];
		struct freelist_node *node;
		node = __freelist_try_get_percpu(slot);
		if (node)
			return node;
		if (++cpu >= s->fh_ncores)
			cpu = 0;
		// prefetch(s->fh_slots[cpu]);
	}

	return NULL;
}

static inline void freelist_destroy(struct freelist_head *s, void *context,
                                    int (*release)(void *, void *, int, int))
{
	uint32_t i;

	if (!s->fh_slots)
		return;

 	for (i = 0; i < s->fh_ncores; i++) {
		struct freelist_node *node;
		do {
			node = __freelist_try_get_percpu(s->fh_slots[i]);
			if (node && release)
				release(context, node, 1, 1);
		} while (node);
	}

	if (s->fh_bulk_buffer) {
		if (release)
			release(context, s->fh_bulk_buffer, 1, 0);
		s->fh_bulk_buffer = NULL;
	}

	freelist_free_slots(s);
}

#endif /* FREELIST_H */
