/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _PERCPU_OBJECT_POOL_H_
#define _PERCPU_OBJECT_POOL_H_

/* 
 * object-pool: ring-array based lockless MPMC/FIFO queues
 *
 * The object pool is a scalable implementaion of high performance queue
 * for objects allocation and reclamation, such as kretprobe instances.
 * 
 * With leveraging per-cpu ring-array to mitigate the hot spots of memory
 * contention, it could deliver near-linear scalability for high parallel
 * cases. Meanwhile, it also achieves high throughput with benifiting from
 * warmed cache on each node.
 *  
 * The object pool are best suited for the following cases:
 * 1) memory allocation or reclamation is prohibited or too expensive
 * 2) the objects are allocated/used/reclaimed very frequently
 * 
 * Before using, you must be aware of it's limitations:
 * 1) The maximum number of objects is determined during pool initialzing
 * 2) The memory of objects won't be freed until the poll is de-allocated
 * 3) Object allocations could be nested, but "more" likely to fail than
 *    the non-nested allocatoins, because non-nested allocation will wait
 *    when ring-array is inconsistency, but the nested version couldn't 
 * 
 * Mixing different objects of self-managed/batched/manually-added is NOT
 * recommended, though it's supported.
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
 * main:
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
 * main:
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
 * objpool_slot: per-cpu ring array
 *
 * Represents a cpu-local array-based ring buffer, its size is specialized
 * during initialization of object pool.
 * 
 * The objpool_slot is allocated from local memory for numa system, and to
 * be kept compact in a single cacheline. ages[] is stored just after the
 * body of objpool_slot, and ents[] is after ages[]. ages[] describes the
 * revision of epoch of the item, solely used to avoid ABA. ents[] contains
 * the object pointers.
 * 
 * The default size of objpool_slot is a single cacheline, aka. 64 bytes.
 * 
 * 64bit:
 *        4      8      12     16        32                 64
 * | head | tail | size | mask | ages[4] | ents[4]: (8 * 4) |
 * 
 * 32bit:
 *        4      8      12     16        32        48       64
 * | head | tail | size | mask | ages[4] | ents[4] | unused |
 * 
 */
struct objpool_slot {
	uint32_t                os_head;	/* head of ring array */
	uint32_t                os_tail;	/* tail of ring array */
	uint32_t                os_size;	/* max item slots, pow of 2 */
	uint32_t                os_mask;	/* os_size - 1 */
#if 0
	uint32_t                os_ages[];	/* ring epoch id */
	void *                  os_ents[];	/* objects array */
#endif
};

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
	uint32_t                oh_batch_size;	/* user pool size in byes */
	void                   *oh_batch_user;	/* user managed memory pool */
	struct objpool_slot   **oh_slots;	/* array of percpu slots */
	uint32_t               *oh_sz_slots;	/* size in bytes of slots */
};

struct freelist_node {
	struct freelist_node   *next;
	uint32_t id;
};

/* compute the suitable num of objects to be managed by slot */
static inline uint32_t __objpool_num_of_objs(uint32_t size)
{
	return rounddown_pow_of_two((size - sizeof(struct objpool_slot)) /
	                            (sizeof(uint32_t) + sizeof(void *)));
}

#define SLOT_AGES(s) (uint32_t *)((char *)(s) + sizeof(struct objpool_slot))
#define SLOT_ENTS(s) (void **)((char *)(s) + sizeof(struct objpool_slot) + \
		                            sizeof(uint32_t) * (s)->os_size)
#define SLOT_OBJS(s) (void *)((char *)(s) + sizeof(struct objpool_slot) + \
		        (sizeof(uint32_t) + sizeof(void *)) * (s)->os_size)

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
		uint32_t *ages;
		void **ents;

		/* compute how many objects to be managed by this slot */
		nobjs = oh->oh_nobjs / oh->oh_ncpus;
		if (i < (oh->oh_nobjs % oh->oh_ncpus))
			nobjs++;
		size = sizeof(struct objpool_slot) + sizeof(void *) * nents +
		       sizeof(uint32_t) * nents + objsz * nobjs;

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
		os->os_size = oh->oh_nents;
		os->os_mask = os->os_size - 1;
		ages = SLOT_AGES(os);
		ents = SLOT_ENTS(os);
		for (j = 0; oh->oh_in_slot && j < nobjs; j++) {
			struct freelist_node *node;
			ents[os->os_tail] = SLOT_OBJS(os) + j * objsz;
			ages[os->os_tail] = os->os_tail;
			node = (void *)ents[os->os_tail];
			node->id = ++id;
			os->os_tail++;
		}
		oh->oh_slots[i] = os;
		oh->oh_sz_slots[i] = size;
	}

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
 *         = 0: balanced mode, objects (equally) scatter among all slots
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

/* adding object to slot tail, the given slot mustn't be full */
static inline int __objpool_add_slot(void *obj, struct objpool_slot *os)
{
	uint32_t *ages = SLOT_AGES(os);
	void **ents = SLOT_ENTS(os);
	uint32_t tail = atomic_inc_return((atomic_t *)&os->os_tail) - 1;

	WRITE_ONCE(ents[tail & os->os_mask], obj);
	/* an implicated smp_wmb() in smp_store_release */
	smp_store_release(&ages[tail & os->os_mask], tail);
	return 0;
}

/* adding object to slot, abort if the slot was already full */
static inline int __objpool_try_add_slot(void *obj, struct objpool_slot *os)
{
	uint32_t *ages = SLOT_AGES(os);
	void **ents = SLOT_ENTS(os);
	uint32_t head, tail;

	do {
		/* perform memory loading for both head and tail */
		head = smp_load_acquire(&os->os_head);
		tail = smp_load_acquire(&os->os_tail);
		/* just abort if slot is full */
		if (tail >= head + os->os_size)
			return -ENOENT;
		/* try to extend tail by 1 using CAS to avoid races */
		if (try_cmpxchg_acquire(&os->os_tail, &tail, tail + 1))
			break;
	} while (1);

	/* the tail-th of slot is reserved for the given obj */
	WRITE_ONCE(ents[tail & os->os_mask], obj);
	/* update epoch id to make this object available for pop() */
	smp_store_release(&ages[tail & os->os_mask], tail);
	return 0;
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
	int used = 0, id = 0;

	if (oh->oh_batch_user || !buf || !objsz || size < objsz)
		return -EINVAL;
	if (oh->oh_objsz && oh->oh_objsz != objsz)
		return -EINVAL;

	WARN((((unsigned long)buf) & (sizeof(void *) - 1)), "buf isn't aligned by pointer-size.\n");
	WARN((((uint32_t)objsz) & (sizeof(void *) - 1)), "objsz isn't aligned by pointer-size\n");

	while (used + objsz < size) {
		struct freelist_node *node = buf + used;
		node->id = ++id;
		if (objpool_add_scattered(buf + used, oh))
			break;
		used += objsz;
	}

	if (!used)
		return -ENOENT;

	oh->oh_batch_user = buf;
	oh->oh_batch_size = size;
	oh->oh_objsz = objsz;
	return 0;
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
 * 
 * objpool_push is non-blockable, and can be nested 
 */
static inline int objpool_push(void *obj, struct objpool_head *oh)
{
	uint32_t cpu = raw_smp_processor_id();

 	do {
		if (oh->oh_nobjs > oh->oh_nents) {
			if (!__objpool_try_add_slot(obj, oh->oh_slots[cpu]))
				return 0;
		} else {
			if (!__objpool_add_slot(obj, oh->oh_slots[cpu]))
				return 0;
		}
		if (++cpu >= oh->oh_ncpus)
			cpu = 0;
	} while (1);

	return -ENOENT;
}

/* try to retrieve object from slot */
static inline void *__objpool_try_get_slot(struct objpool_slot *os, int nested)
{
	uint32_t *ages = SLOT_AGES(os);
	void **ents = SLOT_ENTS(os);
	/* do memory load of os_head to local head */
	uint32_t head = smp_load_acquire(&os->os_head);

	/* loop if slot isn't empty */
	while (head != READ_ONCE(os->os_tail)) {
		uint32_t id = head & os->os_mask, prev = head;

		/* mark this item as reserved by decrement epoch id  */
		if (try_cmpxchg_acquire(&ages[id], &head, head - 1)) {
			struct freelist_node *node;
			node = READ_ONCE(ents[id]);
			if (!node)
				BUG();
			/* commit and move forward head of the slot */
			smp_store_release(&os->os_head, head + 1);
			return node;
		}

		/* re-load head from memory continue trying */
		head = READ_ONCE(os->os_head);

		/* quit if it's a nested calling to avoid possible deadlock */
		if (nested && head == prev) {
			/*
			 * head stays unchanged, so it's very likely current pop()
			 * just preempted an unfinished pop() or push() operation.
			 */
			break;
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
 * objpool_pop can not be nesed, and it's safe to use objpool_pop()
 * if the callers are all equal (not preemptible by each other, or
 * in same irq). otherwise, should use objpool_pop_nested in the
 * callers that could preempt or interrupt.
 */
static inline void *objpool_pop(struct objpool_head *oh)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *slot = oh->oh_slots[cpu];
		void *obj = __objpool_try_get_slot(slot, 0);
		if (obj)
			return obj;
		if (++cpu >= oh->oh_ncpus)
			cpu = 0;
	}

	return NULL;
}

/**
 * objpool_pop_nested: allocate an object from objects pool
 *
 * args:
 * @oh:  object pool
 * 
 * return:
 *   object: NULL if failed (object pool is empty)
 * 
 * objpool_pop_nested supports nested calling. It's "more" likely to
 * fail than objpool_pop, since the fromer will quit current slot and
 * move forward to next if it detects inconsistency.
 * 
 * The very possible usecase of objpool_pop_nested is irq handler or
 * softirq handler, while normal threads are calling objpool_pop. 
 */
static inline void *objpool_pop_nested(struct objpool_head *oh)
{
	uint32_t i, cpu = raw_smp_processor_id();

 	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *slot = oh->oh_slots[cpu];
		void *obj = __objpool_try_get_slot(slot, 1);
		if (obj)
			return obj;
		if (++cpu >= oh->oh_ncpus)
			cpu = 0;
	}

	return NULL;
}

/* whether this object is from user buffer (batched adding) */
int __objpool_is_userobj(void *obj, struct objpool_head *oh)
{
	return (obj && obj >= oh->oh_batch_user &&
		obj < oh->oh_batch_user + oh->oh_batch_size);
}

/* whether this object is pre-allocated with percpu slots */
int __objpool_is_embeded(void *obj, struct objpool_head *oh)
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
			obj = __objpool_try_get_slot(oh->oh_slots[i], 0);
			if (obj && release) {
				int user = !__objpool_is_userobj(obj, oh) &&
				           !__objpool_is_embeded(obj, oh);
				release(context, obj, user, 1);
			}
		} while (obj);
	}

	if (oh->oh_batch_user) {
		if (release)
			release(context, oh->oh_batch_user, 1, 0);
		oh->oh_batch_user = NULL;
		oh->oh_batch_size = 0;
	}

	__objpool_fini_percpu_slots(oh);
}



#define QUEUE_METHOD "rs"

#define  freelist_head objpool_head

static inline int freelist_init(struct freelist_head *list, int nrecords)
{
	return objpool_init(list, nrecords, 0, 0, GFP_KERNEL);
}

#define freelist_try_add objpool_add_scattered
#define freelist_add objpool_push
#define freelist_try_get objpool_pop
#define freelist_destroy objpool_fini

#endif /* _PERCPU_OBJECT_POOL_H_ */
