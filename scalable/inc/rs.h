/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */

#ifndef _RINGSLOT_OBJECT_POOL_H_
#define _RINGSLOT_OBJECT_POOL_H_

/* 
 * object-pool: ring-array based lockless MPMC/FIFO queues
 *
 * The object pool is a scalable implementaion of high performance queue
 * for objects allocation and reclamation, such as kretprobe instances.
 *
 * With leveraging per-cpu ring-array to mitigate the hot spots of memory
 * contention, it could deliver near-linear scalability for high parallel
 * cases. Meanwhile, it also achieves high throughput with benifiting from
 * warmed cache on each core.
 *
 * The object pool are best suited for the following cases:
 * 1) memory allocation or reclamation is prohibited or too expensive
 * 2) the objects are allocated/used/reclaimed very frequently
 *
 * Before using, you must be aware of it's limitations:
 * 1) Maximum number of objects is determined during pool initializing
 * 2) The memory of objects won't be freed until the poll is de-allocated
 * 3) Both allocation and reclamation could be nested
 *
 * Mixing different objects of self-managed/batched/manually-added is NOT
 * recommended, though it's supported.
 *
 * Typical use cases:
 *
 * 1) self-managed objects
 *
 * obj_init():
 *    static int obj_init(void *obj, struct objpool_head *oh)
 *    {
 *		struct my_node *node;
 *              node = container_of(obj, struct my_node, obj);
 * 		do_init_node(node);
 * 		return 0;
 *    }
 *
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 16, 1, GFP_KERNEL, obj_init);
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
 * obj_init():
 *    static int obj_init(void *obj, struct objpool_head *oh)
 *    {
 *		struct my_node *node;
 *              node = container_of(obj, struct my_node, obj);
 * 		do_init_node(node);
 * 		return 0;
 *    }
 *
 * free_buf():
 *    static int free_buf(void *context, void *obj, int user, int element)
 *    {
 * 		if (obj && user && !element)
 * 			kfree(obj);
 *    }
 *
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 0, 0, GFP_KERNEL, NULL);
 *    buffer = kmalloc(size, ...);
 *    objpool_populate(&oh, buffer, size, 16, obj_init);
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
 *  free_obj():
 *    static int free_obj(void *context, void *obj, int user, int element)
 *    {
 * 		if (obj && user && element)
 * 			kfree(obj);
 *    }
 *
 * main():
 *    objpool_init(&oh, num_possible_cpus() * 4, 0, 0, GFP_KERNEL, NULL);
 *    for () {
 *      obj = kmalloc(objsz, ...);
 *      user_init_object(obj);
 *      objpool_add_scattered(obj, oh);
 *    }
 *    <object pool initialized>
 *
 *    obj = objpool_pop(&oh);
 *    do_something_with(obj);
 *    objpool_push(obj, &oh);
 *
 *    <object pool to be destroyed>
 *    objpool_fini(&oh, NULL, free_obj);
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
	uint16_t                oh_in_slot:1;	/* objs alloced with slots */
	uint16_t                oh_vmalloc:1;	/* alloc from vmalloc zone */
	gfp_t                   oh_gfp;		/* k/vmalloc gfp flags */
	uint32_t                oh_sz_pool;	/* user pool size in byes */
	void                   *oh_pool;	/* user managed memory pool */
	struct objpool_slot   **oh_slots;	/* array of percpu slots */
	uint32_t               *oh_sz_slots;	/* size in bytes of slots */
};

/*
 * for benchmark only
 */

struct freelist_node {
	struct freelist_node   *next;
	uint32_t id;
};

#define QUEUE_METHOD "rs"
#define freelist_head objpool_head
#define freelist_try_add objpool_add_scattered
#define freelist_add objpool_push
#define freelist_try_get objpool_pop
#define objpool_pop_nested objpool_pop
#define freelist_destroy objpool_fini

typedef int (*objpool_init_cb)(void *, struct objpool_head *);

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
static inline int
__objpool_init_percpu_slots(struct objpool_head *oh, uint32_t nobjs,
                            objpool_init_cb objinit)
{
	uint32_t i, j, size, objsz, nents = oh->oh_nents;

	/* aligned object size by sizeof(void *) */
	objsz = ALIGN(oh->oh_objsz, sizeof(void *));
	/* shall we allocate objects along with objpool_slot */
	if (objsz)
		oh->oh_in_slot = 1;

	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *os;
		uint32_t n;

		/* compute how many objects to be managed by this slot */
		n = nobjs / oh->oh_ncpus;
		if (i < (nobjs % oh->oh_ncpus))
			n++;
		size = sizeof(struct objpool_slot) + sizeof(void *) * nents +
		       sizeof(uint32_t) * nents + objsz * n;

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
		oh->oh_slots[i] = os;
		oh->oh_sz_slots[i] = size;

		/*
		 * start from 2nd round to avoid conflict of 1st item.
		 * we assume that the head item is ready for retrieval
		 * iff head is equal to ages[head & mask]. but ages is
		 * initialized as 0, so in view of the caller of pop(),
		 * the 1st item (0th) is always ready, but fact could
		 * be: push() is stalled before the final update, thus
		 * the item being inserted will be lost forever.
		 */
		os->os_head = os->os_tail = oh->oh_nents;

		for (j = 0; oh->oh_in_slot && j < n; j++) {
			uint32_t *ages = SLOT_AGES(os);
			void **ents = SLOT_ENTS(os);
			void *obj = SLOT_OBJS(os) + j * objsz;
			uint32_t ie = os->os_tail & os->os_mask;

			/* perform object initialization */
			if (objinit) {
				int rc = objinit(obj, oh);
				if (rc)
					return rc;
			}

			/* add obj into the ring array */
			ents[ie] = obj;
			ages[ie] = os->os_tail;
			os->os_tail++;
		        oh->oh_nobjs++;
		}
	}

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
static inline int objpool_init(struct objpool_head *oh, int nobjs, int objsz,
                               int asym, gfp_t gfp, objpool_init_cb objinit)
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
	oh->oh_gfp = gfp & ~__GFP_ZERO;

	/* allocate array for percpu slots */
	oh->oh_slots = kzalloc(oh->oh_ncpus * sizeof(void *) +
			       oh->oh_ncpus * sizeof(uint32_t), oh->oh_gfp);
	if (!oh->oh_slots)
		return -ENOMEM;
	oh->oh_sz_slots = (uint32_t *)&oh->oh_slots[oh->oh_ncpus];

	/* initialize per-cpu slots */
	rc = __objpool_init_percpu_slots(oh, nobjs, objinit);
	if (rc)
		__objpool_fini_percpu_slots(oh);

	return rc;
}

static inline int freelist_init(struct freelist_head *list, int nrecords)
{
	return objpool_init(list, nrecords, 0, 0, GFP_KERNEL, NULL);
}

/* adding object to slot tail, the given slot mustn't be full */
static inline int __objpool_add_slot(void *obj, struct objpool_slot *os)
{
	uint32_t *ages = SLOT_AGES(os);
	void **ents = SLOT_ENTS(os);
	uint32_t tail = atomic_inc_return((atomic_t *)&os->os_tail) - 1;

	WRITE_ONCE(ents[tail & os->os_mask], obj);
	/* obj must be updated before tail updating. orders matters, and
	   the implicated smp_wmb() in smp_store_release assures of that */
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
		head = READ_ONCE(os->os_head);
		tail = READ_ONCE(os->os_tail);
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
		if (++cpu >= oh->oh_ncpus)
			cpu = 0;
	}

	return -ENOENT;
}

/**
 * objpool_populate: add objects from user provided pool in batch
 *
 * args:
 * @oh:  object pool
 * @obj: object pointer to be pushed to object pool
 *
 * return:
 *     0 or error code: it fails only when objects pool are full
 */
static inline int
objpool_populate(struct objpool_head *oh, void *buf, int size, int objsz,
                 objpool_init_cb objinit)
{
	int used = 0;

	if (oh->oh_pool || !buf || !objsz || size < objsz)
		return -EINVAL;
	if (oh->oh_objsz && oh->oh_objsz != objsz)
		return -EINVAL;

	WARN_ON_ONCE(((unsigned long)buf) & (sizeof(void *) - 1));
	WARN_ON_ONCE(((uint32_t)objsz) & (sizeof(void *) - 1));

	while (used + objsz <= size) {
		void *obj = buf + used;

		/* perform object initialization */
		if (objinit) {
			int rc = objinit(obj, oh);
			if (rc)
				return rc;
		}

		/* add object into slot: oh */
		if (objpool_add_scattered(obj, oh))
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
static inline void *__objpool_try_get_slot(struct objpool_slot *os)
{
	uint32_t *ages = SLOT_AGES(os);
	void **ents = SLOT_ENTS(os);
	/* do memory load of os_head to local head */
	uint32_t head = smp_load_acquire(&os->os_head);

	/* loop if slot isn't empty */
	while (head != READ_ONCE(os->os_tail)) {
		uint32_t id = head & os->os_mask, prev = head;

		/* do prefetching of object ents */
		prefetch(&ents[id]);

		/*
		 * check whether this item was ready for retrieval ? There's
		 * possibility * in theory * we might retrieve wrong object,
		 * in case ages[id] overflows while current task is sleeping,
		 * but it will take much longer time than task scheduler time
		 * slice to overflow an uint32_t
		 */
		if (smp_load_acquire(&ages[id]) == head) {
			struct freelist_node *node;
			/* node must have been udpated by push() */
			node = READ_ONCE(ents[id]);
			if (!node)
				printk("NULL node\n");
			/* commit and move forward head of the slot */
			if (try_cmpxchg_release(&os->os_head, &head, head + 1))
				return node;
		}

		/* re-load head from memory continue trying */
		head = READ_ONCE(os->os_head);
		/*
		 * head stays unchanged, so it's very likely current pop()
		 * just preempted/interrupted an ongoing push() operation
		 */
		if (head == prev)
			break;
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
 * objpool_pop can be nested, so can be used in any context.
 */
static inline void *objpool_pop(struct objpool_head *oh)
{
	uint32_t i, cpu = raw_smp_processor_id();
	void *obj = NULL;

 	for (i = 0; i < oh->oh_ncpus; i++) {
		struct objpool_slot *slot = oh->oh_slots[cpu];
		obj = __objpool_try_get_slot(slot);
		if (obj)
			break;
		if (++cpu >= oh->oh_ncpus)
			cpu = 0;
	}

	return obj;
}

/* whether this object is from user buffer (batched adding) */
static inline int objpool_is_inpool(void *obj, struct objpool_head *oh)
{
	return (obj && obj >= oh->oh_pool &&
		obj < oh->oh_pool + oh->oh_sz_pool);
}

/* whether this object is pre-allocated along with percpu slots */
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

	for (i = 0; release && i < oh->oh_ncpus; i++) {
		void *obj;
		do {
			obj = __objpool_try_get_slot(oh->oh_slots[i]);
			if (obj) {
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

#endif /* _RINGSLOT_OBJECT_POOL_H_ */
