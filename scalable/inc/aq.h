/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */

#ifndef _PERCPU_OBJECT_POOL_H_
#define _PERCPU_OBJECT_POOL_H_

/*
 * Object Pool: ring-array based lockless MPMC queues
 *
 * Copyright: Matt Wu <wuqiang.matt@bytedance.com>
 *
 * The object pool is a scalable implementaion of high performance queue
 * for objects allocation and reclamation, such as kretprobe instances.
 * 
 * With leveraging per-cpu ring-array to mitigate the hot spots of memory
 * contention, it could deliver near-linear scalability for high parallel
 * cases. Meanwhile, it also achieves high throughput with benifiting from
 * warmed cache on each node. Object allocations and reclamations could be
 * nested and guaranteed to be deadlock-free since the solution of bounded
 * array can nicely avoid ABA issues with order and fairness being ignored.
 *  
 * The object pool are best suited for the following cases:
 * 1) memory allocation or reclamation is prohibited or too expensive
 * 2) the objects are allocated/used/reclaimed very frequently
 * 
 * Before using, you must be aware of it's limitations:
 * 1) Maximum number of objects is determined during pool initializing
 * 2) The memory of objects won't be freed until the poll is de-allocated
 * 3) Order and fairness are ignored. Some threads might stay hungry much
 *    longer than others
 * 
 * Mixing different objects of self-managed/batched/manually-added is NOT
 * recommended, though it's supported. For mixed case, the caller should
 * take care of the releasing of objects or user pool.
 * 
 * Typical use cases:
 * 
 * 1) self-managed objects
 * 
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 16, 1, GFP_KERNEL);
 *    <object pool initialized>
 * 
 *    obj = objpool_pop(&oh);
 *    do_something_with(obj);
 *    objpool_push(obj, &oh);
 * 
 *    <object pool to be destroyed>
 *    objpool_fini(&oh, NULL, NULL);
 * 
 * 2) batced with user's buffer
 * 
 * free_buf():
 *    static int free_buf(void *context, void *obj, int user, int element)
 *    {
 * 		if (obj && user && !element)
 * 			kfree(obj);
 *    }
 * 
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 0, 0, GFP_KERNEL);
 *    buffer = kmalloc(size, ...);
 *    objpool_populate(&oh, buffer, size, 16);
 *    <object pool initialized>
 * 
 *    obj = objpool_pop(&oh);
 *    do_something_with(obj);
 *    objpool_push(obj, &oh);
 * 
 *    <object pool to be destroyed>
 *    objpool_fini(&oh, NULL, free_buf);
 * 
 * 3) manually added with user objects
 *
 *  free_buf():
 *    static int free_buf(void *context, void *obj, int user, int element)
 *    {
 * 		if (obj && user && element)
 * 			kfree(obj);
 *    }
 * 
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 0, 0, GFP_KERNEL);
 *    for () {
 *      obj = kmalloc(objsz, ...);
 *      objpool_add_scattered(obj, oh);
 *    }
 *    <object pool initialized>
 * 
 *    obj = objpool_pop(&oh);
 *    do_something_with(obj);
 *    objpool_push(obj, &oh);
 *
 *    <object pool to be destroyed>
 *    objpool_fini(&oh, NULL, free_buf);
 * 
 */

#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>

/*
 * objpool_slot: per-cpu ring array of (void *) pointers
 *
 * Represents a cpu-local array-based ring buffer, its size is specialized
 * during initialization of object pool.
 * 
 * The objpool_slot is allocated from the memory pool of local node. The
 * default (and minimal) size is a single cacheline, aka. 64 bytes. So:
 * 
 * 64bit system: contains  8 entries (64 / 8)
 * 32bit system: contains 16 entries (64 / 4)
 */
 struct objpool_slot {
        atomic_t                os_used;        /* num of objects in slot */
        uint32_t                os_size;        /* max per slot, pow of 2 */
        uint32_t                os_mask;        /* os_size - 1 */
        uint32_t                os_res1;        /* reserved */
#if 0
        void *                  os_ents[];      /* objects array */
#endif
};

#define SLOT_ENTS(s)    (void *)((char *)(s) + sizeof(struct objpool_slot))
#define SLOT_OBJS(s, n) (void *)((char *)SLOT_ENTS(s) + sizeof(void *) * (n))

/*
 * objpool_head: object pooling metadata
 */
struct objpool_head {
	uint32_t                oh_objsz;	/* object & element size */
	uint32_t                oh_nobjs;	/* total objs (pre-allocated) */
	uint32_t                oh_nents;	/* max objects per cpuslot */
	uint16_t                oh_ncpus;	/* num of possible cpus */
	uint16_t                oh_in_user:1;	/* user-specified buffer */
	uint16_t		oh_in_slot:1;	/* objs alloced with slots */
	uint16_t                oh_vmalloc:1;	/* alloc from vmalloc zone */
	gfp_t  			oh_gfp;		/* k/vmalloc gfp flags */
	uint32_t                oh_sz_pool;	/* user pool size in byes */
	void                   *oh_pool;	/* user managed memory pool */
	struct objpool_slot   **oh_slots;	/* array of percpu slots */
	uint32_t               *oh_sz_slots;	/* size in bytes of slots */
};

/*
 * for benchmark
 */

struct freelist_node {
	struct freelist_node   *next;
	uint32_t id;
};

#define QUEUE_METHOD "op"
#define freelist_head objpool_head
#define freelist_try_add objpool_add_scattered
#define freelist_add objpool_push
#define freelist_try_get objpool_pop
#define freelist_destroy objpool_fini
#define objpool_pop_nested objpool_pop

/* compute the suitable num of objects to be managed by slot */
static inline uint32_t __objpool_num_of_objs(uint32_t size)
{
	return rounddown_pow_of_two((size - sizeof(struct objpool_slot)) /
			            sizeof(void *));
}

/* allocate and initialize percpu slots */
static inline int __objpool_init_percpu_slots(struct objpool_head *oh)
{
	uint32_t i, j, size, nobjs, objsz, id = 0, nents = oh->oh_nents;

	/* aligned object size by sizeof(void *) */
	objsz = ALIGN(oh->oh_objsz, sizeof(void *));
	/* shall we allocate objects along with objpool_slot */
	if (objsz)
		oh->oh_in_slot = 1;

	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *os;

		/* compute how many objects to be managed by this slot */
		nobjs = oh->oh_nobjs / oh->oh_ncpus;
		if (i < (oh->oh_nobjs % oh->oh_ncpus))
			nobjs++;
		size = sizeof(*os) + sizeof(void *) * nents + objsz * nobjs;

		/* decide which pool shall the slot be allocated from */
		if (0 == i) {
			if ((oh->oh_gfp & GFP_ATOMIC) || size < PAGE_SIZE)
				oh->oh_vmalloc = 0;
			else
				oh->oh_vmalloc = 1;
		}

		/* allocate percpu slot & objects from local memory */
		if (oh->oh_vmalloc)
			os = vmalloc_node(size, cpu_to_node(i));
		else
			os = kmalloc_node(size, oh->oh_gfp, cpu_to_node(i));
		if (!os)
			return -ENOMEM;

		/* initialize percpu slot for the i-th cpu */
		memset(os, 0, size);

		os->os_size = nents;
		os->os_mask = nents - 1;
		/* initialize pre-allocated record entries */
		for (j = 0; oh->oh_in_slot && j < nobjs; j++) {
			void **ents = SLOT_ENTS(os);
			ents[j] = SLOT_OBJS(os, nents) + j * objsz;
			atomic_inc(&os->os_used);
			{
				/* benchmark tagging */
				struct freelist_node *node;
				node = (void *)ents[j];
				node->id = ++id;
			}
		}
		oh->oh_slots[i] = os;
		oh->oh_sz_slots[i] = size;
	}

	/* set oh->oh_nobjs as 0 if no entry is pre-allcoated */
	if (!oh->oh_in_slot)
		oh->oh_nobjs = 0;

	return 0;
}

/* cleanup all percpu slots of the object pool */
static inline void __objpool_fini_percpu_slots(struct objpool_head *oh)
{
	uint32_t i;

	if (!oh->oh_slots)
		return;

	for (i = 0; i < oh->oh_ncpus; i++) {
		if (!oh->oh_slots[i])
			continue;
		if (oh->oh_vmalloc)
			vfree(oh->oh_slots[i]);
		else
			kfree(oh->oh_slots[i]);
	}
	kfree(oh->oh_slots);
	oh->oh_slots = NULL;
	oh->oh_sz_slots = NULL;
}

/**
 * objpool_init: initialize object pool and pre-allocate objects
 * 
 * args:
 * @oh:    the object pool to be initialized, declared by the caller
 * @nojbs: total objects to be managed by this object pool
 * @ojbsz: size of an object, to be pre-allocated if objsz is not 0
 * @asym:  the imbalance degree of parallel threads, can be 0 - nobjs
 *         = 1: performance mode, means any thread could occupy all the
 *              pre-allocated objects of the object pool
 *         = 0: balanced mode, objects (equally) scatter among all cpus
 *         > 1: each slot could contain (nobjs/asym) objects
 * @gfp:   gfp flags of caller's context for memory allocation
 * 
 * return:
 *         0 for success, otherwise error code
 * 
 * All pre-allocated objects are to be zeroed. Caller should do extra
 * initialization before using.
 */
static inline int
objpool_init(struct objpool_head *oh, int nobjs, int objsz, int asym, gfp_t gfp)
{
	uint32_t nents, cpus = num_possible_cpus();
	int rc;

	memset(oh, 0, sizeof(struct objpool_head));

	/* cpu numbers exceeds maxium counts we support */
	if (cpus >= (1UL << 16))
		return -ENOTSUPP;

	/* calculate percpu slot size (rounded to pow of 2) */
	if (asym)
		nents = nobjs / asym;
	else
		nents = nobjs / cpus;
	if (nents < __objpool_num_of_objs(L1_CACHE_BYTES))
		nents = __objpool_num_of_objs(L1_CACHE_BYTES);
	nents = roundup_pow_of_two(nents);
	while (nents * cpus < nobjs)
		nents = nents << 1;

	oh->oh_ncpus = cpus;
	oh->oh_objsz = objsz;
	oh->oh_nents = nents;
	oh->oh_nobjs = nobjs;
	oh->oh_gfp = gfp & ~__GFP_ZERO;

	/* allocate array for percpu slots */
	oh->oh_slots = kzalloc(oh->oh_ncpus * sizeof(void *) +
			       oh->oh_ncpus * sizeof(uint32_t), oh->oh_gfp);
	if (!oh->oh_slots)
		return -ENOMEM;
	oh->oh_sz_slots = (uint32_t *)&oh->oh_slots[oh->oh_ncpus];

	/* initialize per-cpu slots */
	rc = __objpool_init_percpu_slots(oh);
	if (rc)
		__objpool_fini_percpu_slots(oh);

	return rc;
}

static inline int freelist_init(struct freelist_head *list, int nrecords)
{
	return objpool_init(list, nrecords, 0, 0, GFP_KERNEL);
}

/* adding object to slot, the given slot mustn't be full */
static inline int __objpool_add_slot(void *obj, struct objpool_slot *os)
{
	void **ents = SLOT_ENTS(os);
	uint32_t i = 0, mask = os->os_mask;

	while (atomic_read(&os->os_used) < os->os_size) {
		/* always perform memory loading */
		void *ent = smp_load_acquire(&ents[i]);

		/* do CAS if the entry is not yet occupied */
		if (!ent && try_cmpxchg_release(&ents[i], &ent, obj)) {
			atomic_inc(&os->os_used);
			return 0;
		}

		/* try next entry */
		i = (i + 1) & mask;
	}

	return -ENOENT;
}

/* adding object to slot, abort if the slot was already full */
static inline int __objpool_try_add_slot(void *obj, struct objpool_slot *os)
{
	void **ents = SLOT_ENTS(os);
	uint32_t i, size = os->os_size;

	for (i = 0; i < size && atomic_read(&os->os_used) < size; i++) {
		/* always perform memory loading */
		void *ent = smp_load_acquire(&ents[i]);

		/* do CAS if the entry is not yet occupied */
		if (!ent && try_cmpxchg_release(&ents[i], &ent, obj)) {
			atomic_inc(&os->os_used);
			return 0;
		}
	}

	return -ENOENT;
}

/**
 * objpool_add_scattered: adding pre-allocated objects to objects pool
 * during initialization. it will try to balance the object numbers of
 * all slots. 
 *
 * args:
 * @obj: object pointer to be added to object pool
 * @oh:  object pool
 * 
 * return:
 *     0 or error code
 * 
 * objpool_add_scattered doesn't handle race conditions, can only be
 * called during object pool initialization 
 */
static inline int objpool_add_scattered(void *obj, struct objpool_head *oh)
{
	uint32_t i, cpu;

	if (!obj || oh->oh_nobjs >= oh->oh_ncpus * oh->oh_nents)
		return -EINVAL;

	cpu = oh->oh_nobjs % oh->oh_ncpus;
	for (i = 0; i < oh->oh_ncpus; i++) {
		if (!__objpool_add_slot(obj, oh->oh_slots[cpu])) {
			oh->oh_nobjs++;
			return 0;
		}
		cpu = (cpu + 1) % oh->oh_ncpus;
	}

	return -ENOENT;
}

/**
 * objpool_populate: add objects from user provided pool in batch
 *  *
 * args:
 * @oh:  object pool
 * @obj: object pointer to be pushed to object pool
 * 
 * return:
 *     0 or error code: it fails only when objects pool are full
 * 
 * objpool_push is non-blockable, and can be nested 
 */
static inline int
objpool_populate(struct objpool_head *oh, void *buf, int size, int objsz)
{
	int used = 0;

	if (oh->oh_pool || !buf || !objsz || size < objsz)
		return -EINVAL;
	if (oh->oh_objsz && oh->oh_objsz != objsz)
		return -EINVAL;

	WARN((((unsigned long)buf) & (sizeof(void *) - 1)), "buf unaligned");
	WARN((((uint32_t)objsz) & (sizeof(void *) - 1)), "objsz unaligned");

	while (used + objsz <= size) {
                struct freelist_node *node = buf + used;
                node->id = oh->oh_nobjs + 1;
		if (objpool_add_scattered(buf + used, oh))
			break;
		used += objsz;
	}

	if (!used)
		return -ENOENT;

	oh->oh_pool = buf;
	oh->oh_sz_pool = size;
	oh->oh_objsz = objsz;
	return 0;
}

static inline uint32_t __objpool_cpu_next(uint32_t cpu, uint32_t ncpus)
{
	if (cpu == ncpus - 1)
		return 0;
	else
		return (cpu + 1);
}

static inline uint32_t __objpool_cpu_prev(uint32_t cpu, uint32_t ncpus)
{
	if (cpu == 0)
		return (ncpus - 1);
	else
		return (cpu - 1);
}

/**
 * objpool_push: reclaim the object and return back to objects pool
 *
 * args:
 * @obj: object pointer to be pushed to object pool
 * @oh:  object pool
 * 
 * return:
 *     0 or error code: it fails only when objects pool are full
 */
static inline int objpool_push(void *obj, struct objpool_head *oh)
{
	int (*add_slot)(void *, struct objpool_slot *);
	uint32_t (*cpu_next)(uint32_t, uint32_t);
	uint32_t cpu = raw_smp_processor_id();

	if (oh->oh_nobjs > oh->oh_nents)
		add_slot = __objpool_try_add_slot;
	else
		add_slot = __objpool_add_slot;

	/* minimize memory conflicts: odd and even */
	if (cpu & 1)
		cpu_next = __objpool_cpu_prev;
	else
		cpu_next = __objpool_cpu_next;

 	do {
		struct objpool_slot *os = oh->oh_slots[cpu];
		if (!add_slot(obj, os))
			return 0;
		cpu = cpu_next(cpu, oh->oh_ncpus);
	} while (1);

	return -ENOENT;
}

/* try to retrieve object from slot */
static inline void *
__objpool_try_get_slot(struct objpool_slot *os)
{
	void **ents = SLOT_ENTS(os);
	uint32_t i, size = os->os_size;

	for (i = 0; i < size && atomic_read(&os->os_used); i++) {
		/* always perform memory loading */
		void *ent = smp_load_acquire(&ents[i]);

		/* do CAS if there exists an item */
		if (ent && try_cmpxchg_release(&ents[i], &ent, 0)) {
			atomic_dec(&os->os_used);
			return ent;
		}
	}

	return NULL;
}

/**
 * objpool_pop: allocate an object from objects pool
 *
 * args:
 * @oh:  object pool
 * 
 * return:
 *   object: NULL if failed (object pool is empty)
 * 
 * objpool_pop can be nesed, and guaranteed to be deadlock-free.
 * So it can be called in any context, like irq/softirq/nmi.
 */
static inline void *objpool_pop(struct objpool_head *oh)
{
	uint32_t (*cpu_next)(uint32_t, uint32_t);
	uint32_t i, cpu = raw_smp_processor_id();
	void *obj = NULL;

	if (cpu & 1)
		cpu_next = __objpool_cpu_prev;
	else
		cpu_next = __objpool_cpu_next;

 	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *os = oh->oh_slots[cpu];
		obj = __objpool_try_get_slot(os);
		if (obj)
			break;
		cpu = cpu_next(cpu, oh->oh_ncpus);
	}

	return obj;
}

/* whether this object is from user buffer (batched adding) */
static inline int objpool_is_inpool(void *obj, struct objpool_head *oh)
{
	return (obj && obj >= oh->oh_pool &&
		obj < oh->oh_pool + oh->oh_sz_pool);
}

/* whether this object is pre-allocated with percpu slots */
static inline int objpool_is_inslot(void *obj, struct objpool_head *oh)
{
	uint32_t i;
	for (i = 0; i < oh->oh_ncpus; i++) {
		void *ptr = oh->oh_slots[i];
		if (obj && obj >= ptr && obj < ptr + oh->oh_sz_slots[i])
		    return 1;
	}
	return 0;
}

/**
 * objpool_fini: cleanup the whole object pool (releasing all objects)
 * 
 * args:
 * @oh: object pool
 * @context: user provided value for the callback of release() funciton
 * @release: user provided callback for resource cleanup or statistics
 * 
 * the protocol of release callback:
 * static int release(void *context, void *obj, int user, int element);
 * args:
 *  context: user provided value
 *  obj: the object (element or buffer) to be cleaned up
 *  user: the object is provided by user or pre-allocated by objpool
 *  element: the obj is an element (object), not user provided buffer
 */
static inline void objpool_fini(struct objpool_head *oh, void *context,
                                int (*release)(void *, void *, int, int))
{
	uint32_t i;

	if (!oh->oh_slots)
		return;

 	for (i = 0; i < oh->oh_ncpus; i++) {
		void *obj;
		do {
			obj = __objpool_try_get_slot(oh->oh_slots[i]);
			if (obj && release) {
				int user = !objpool_is_inpool(obj, oh) &&
				           !objpool_is_inslot(obj, oh);
				release(context, obj, user, 1);
			}
		} while (obj);
	}

	if (oh->oh_pool) {
		if (release)
			release(context, oh->oh_pool, 1, 0);
		oh->oh_pool = NULL;
		oh->oh_sz_pool = 0;
	}

	__objpool_fini_percpu_slots(oh);
}

#endif /* _PERCPU_OBJECT_POOL_H_ */