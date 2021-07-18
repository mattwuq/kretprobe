/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2016 Facebook */
/* copied from kernel/bpf/percpu_freelist */
#ifndef __PC_FREELIST_H__
#define __PC_FREELIST_H__
#include <linux/spinlock.h>
#include <linux/percpu.h>

#define QUEUE_METHOD "pc"

struct pc_freelist_head {
	struct pc_freelist_node *first;
	raw_spinlock_t lock;
};

struct pc_freelist {
	struct pc_freelist_head __percpu *freelist;
	struct pc_freelist_head extralist;
	atomic_t 		nodes;
};
#define freelist_head pc_freelist

struct pc_freelist_node {
	struct pc_freelist_node *next;
	uint32_t id;
};
#define freelist_node pc_freelist_node

static inline int pc_freelist_init(struct pc_freelist *s)
{
	int cpu;

	s->freelist = alloc_percpu(struct pc_freelist_head);
	if (!s->freelist)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct pc_freelist_head *head = per_cpu_ptr(s->freelist, cpu);

		raw_spin_lock_init(&head->lock);
		head->first = NULL;
	}
	raw_spin_lock_init(&s->extralist.lock);
        atomic_set(&s->nodes, 0);
	s->extralist.first = NULL;
	return 0;
}
#define freelist_init(s, max) pc_freelist_init(s)

static inline void pc_freelist_destroy(struct pc_freelist *s)
{
	free_percpu(s->freelist);
}

static inline void pc_freelist_push_node(struct pc_freelist_head *head,
					   struct pc_freelist_node *node)
{
	node->next = head->first;
	head->first = node;
}

static inline void ___pc_freelist_push(struct pc_freelist_head *head,
					 struct pc_freelist_node *node)
{
	raw_spin_lock(&head->lock);
	pc_freelist_push_node(head, node);
	raw_spin_unlock(&head->lock);
}

static inline bool pc_freelist_try_push_extra(struct pc_freelist *s,
						struct pc_freelist_node *node)
{
	if (!raw_spin_trylock(&s->extralist.lock))
		return false;

	pc_freelist_push_node(&s->extralist, node);
	raw_spin_unlock(&s->extralist.lock);
	return true;
}

static inline void ___pc_freelist_push_nmi(struct pc_freelist *s,
					     struct pc_freelist_node *node)
{
	int cpu, orig_cpu;

	orig_cpu = cpu = raw_smp_processor_id();
	while (1) {
		struct pc_freelist_head *head;

		head = per_cpu_ptr(s->freelist, cpu);
		if (raw_spin_trylock(&head->lock)) {
			pc_freelist_push_node(head, node);
			raw_spin_unlock(&head->lock);
			return;
		}
		cpu = cpumask_next(cpu, cpu_possible_mask);
		if (cpu >= nr_cpu_ids)
			cpu = 0;

		/* cannot lock any per cpu lock, try extralist */
		if (cpu == orig_cpu &&
		    pc_freelist_try_push_extra(s, node))
			return;
	}
}

static inline int freelist_try_add(struct pc_freelist_node *node,
				   struct pc_freelist *s)
{
	struct pc_freelist_head *head;
	int cpu = atomic_read(&s->nodes) % num_possible_cpus();

	head = per_cpu_ptr(s->freelist, cpu);
	pc_freelist_push_node(head, node);
	atomic_inc(&s->nodes);

	return 0;
}

static inline void __pc_freelist_push(struct pc_freelist *s,
			                struct pc_freelist_node *node)
{
	if (in_nmi())
		___pc_freelist_push_nmi(s, node);
	else
		___pc_freelist_push(this_cpu_ptr(s->freelist), node);
}

static inline void pc_freelist_push(struct pc_freelist *s,
			              struct pc_freelist_node *node)
{
	unsigned long flags;

	local_irq_save(flags);
	__pc_freelist_push(s, node);
	local_irq_restore(flags);
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *head)
{
	pc_freelist_push(head, node);
	return 0;
}

static inline void pc_freelist_populate(struct pc_freelist *s, void *buf, u32 elem_size,
			                  u32 nr_elems)
{
	struct pc_freelist_head *head;
	int i, cpu, pc_entries;

	pc_entries = nr_elems / num_possible_cpus() + 1;
	i = 0;

	for_each_possible_cpu(cpu) {
again:
		head = per_cpu_ptr(s->freelist, cpu);
		/* No locking required as this is not visible yet. */
		pc_freelist_push_node(head, buf);
		i++;
		buf += elem_size;
		if (i == nr_elems)
			break;
		if (i % pc_entries)
			goto again;
	}
}

static inline struct pc_freelist_node *___pc_freelist_pop(struct pc_freelist *s)
{
	struct pc_freelist_head *head;
	struct pc_freelist_node *node;
	int orig_cpu, cpu;

	orig_cpu = cpu = raw_smp_processor_id();
	while (1) {
		head = per_cpu_ptr(s->freelist, cpu);
		raw_spin_lock(&head->lock);
		node = head->first;
		if (node) {
			head->first = node->next;
			raw_spin_unlock(&head->lock);
			return node;
		}
		raw_spin_unlock(&head->lock);
		cpu = cpumask_next(cpu, cpu_possible_mask);
		if (cpu >= nr_cpu_ids)
			cpu = 0;
		if (cpu == orig_cpu)
			break;
	}

	/* per cpu lists are all empty, try extralist */
	raw_spin_lock(&s->extralist.lock);
	node = s->extralist.first;
	if (node)
		s->extralist.first = node->next;
	raw_spin_unlock(&s->extralist.lock);
	return node;
}

static inline struct pc_freelist_node *
___pc_freelist_pop_nmi(struct pc_freelist *s)
{
	struct pc_freelist_head *head;
	struct pc_freelist_node *node;
	int orig_cpu, cpu;

	orig_cpu = cpu = raw_smp_processor_id();
	while (1) {
		head = per_cpu_ptr(s->freelist, cpu);
		if (raw_spin_trylock(&head->lock)) {
			node = head->first;
			if (node) {
				head->first = node->next;
				raw_spin_unlock(&head->lock);
				return node;
			}
			raw_spin_unlock(&head->lock);
		}
		cpu = cpumask_next(cpu, cpu_possible_mask);
		if (cpu >= nr_cpu_ids)
			cpu = 0;
		if (cpu == orig_cpu)
			break;
	}

	/* cannot pop from per cpu lists, try extralist */
	if (!raw_spin_trylock(&s->extralist.lock))
		return NULL;
	node = s->extralist.first;
	if (node)
		s->extralist.first = node->next;
	raw_spin_unlock(&s->extralist.lock);
	return node;
}

static inline struct pc_freelist_node *__pc_freelist_pop(struct pc_freelist *s)
{
	if (in_nmi())
		return ___pc_freelist_pop_nmi(s);
	return ___pc_freelist_pop(s);
}

static inline struct pc_freelist_node *pc_freelist_pop(struct pc_freelist *s)
{
	struct pc_freelist_node *ret;
	unsigned long flags;

	local_irq_save(flags);
	ret = __pc_freelist_pop(s);
	local_irq_restore(flags);
	return ret;
}

#define freelist_try_get  pc_freelist_pop

static inline void freelist_destroy(struct freelist_head *s, void *context,
                                    int (*release)(void *, void *))
{
	struct pc_freelist_node *node;
	int cpu;

	for_each_possible_cpu(cpu) {
		struct pc_freelist_head *head = per_cpu_ptr(s->freelist, cpu);

		// raw_spin_lock(&head->lock))
		while ((node = head->first)) {
			head->first = node->next;
			if (release)
				release(context, node);
		}
		// raw_spin_unlock(&head->lock);
	}

	// raw_spin_lock(&s->extralist.lock);
	while ((node = s->extralist.first)) {
		s->extralist.first = node->next;
		if (release)
			release(context, node);
	}
	//raw_spin_unlock(&s->extralist.lock);

	free_percpu(s->freelist);
        s->freelist = NULL;
}

#endif /* __PC_FREELIST_H__ */

