/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/atomic.h>

#define QUEUE_METHOD "fl"

/*
 * Copyright: cameron@moodycamel.com
 *
 * A simple CAS-based lock-free free list. Not the fastest thing in the world
 * under heavy contention, but simple and correct (assuming nodes are never
 * freed until after the free list is destroyed), and fairly speedy under low
 * contention.
 *
 * Adapted from: https://moodycamel.com/blog/2014/solving-the-aba-problem-for-lock-free-free-lists
 */

struct freelist_node {
	atomic_t		refs;
	struct freelist_node	*next;
};

struct freelist_head {
	struct freelist_node	*head;
};

static inline int freelist_init(struct freelist_head *head, int max)
{
	return 0;
}

#define REFS_ON_FREELIST 0x80000000
#define REFS_MASK	 0x7FFFFFFF

static inline void __freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	/*
	 * Since the refcount is zero, and nobody can increase it once it's
	 * zero (except us, and we run only one copy of this method per node at
	 * a time, i.e. the single thread case), then we know we can safely
	 * change the next pointer of the node; however, once the refcount is
	 * back above zero, then other threads could increase it (happens under
	 * heavy contention, when the refcount goes to zero in between a load
	 * and a refcount increment of a node in try_get, then back up to
	 * something non-zero, then the refcount increment is done by the other
	 * thread) -- so if the CAS to add the node to the actual list fails,
	 * decrese the refcount and leave the add operation to the next thread
	 * who puts the refcount back to zero (which could be us, hence the
	 * loop).
	 */
	struct freelist_node *head = READ_ONCE(list->head);

	atomic_set_release(&node->refs, 1);
	for (;;) {
		WRITE_ONCE(node->next, head);

		if (try_cmpxchg_release(&list->head, &head, node)) {
			break;
		}

		/*
		 * Hmm, the add failed, but we can only try again when
		 * the refcount goes back to zero.
		 */
		if (1 != atomic_fetch_add_unless(&node->refs, REFS_ON_FREELIST - 1, 1))
			break;
	}
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
	/*
	 * We know that the should-be-on-freelist bit is 0 at this point, so
	 * it's safe to set it using a fetch_add.
	 */
	if (!atomic_fetch_add_release(REFS_ON_FREELIST, &node->refs)) {
		/*
		 * Oh look! We were the last ones referencing this node, and we
		 * know we want to add it to the free list, so let's do it!
		 */
		__freelist_add(node, list);
	}

	return 0;
}

#define freelist_try_add freelist_add

static inline struct freelist_node *freelist_try_get(struct freelist_head *list)
{
	struct freelist_node *prev, *next, *head = smp_load_acquire(&list->head);
	unsigned int refs;

	while (head) {
		prev = head;
		refs = atomic_read(&head->refs);
		if ((refs & REFS_MASK) == 0 ||
		    !atomic_try_cmpxchg_acquire(&head->refs, &refs, refs+1)) {
			head = smp_load_acquire(&list->head);
			continue;
		}

		/*
		 * Good, reference count has been incremented (it wasn't at
		 * zero), which means we can read the next and not worry about
		 * it changing between now and the time we do the CAS.
		 */
		next = READ_ONCE(head->next);
		if (try_cmpxchg_acquire(&list->head, &head, next)) {
			/*
			 * Yay, got the node. This means it was on the list,
			 * which means should-be-on-freelist must be false no
			 * matter the refcount (because nobody else knows it's
			 * been taken off yet, it can't have been put back on).
			 */
			WARN_ON_ONCE(atomic_read(&head->refs) & REFS_ON_FREELIST);

			/*
			 * Decrease refcount twice, once for our ref, and once
			 * for the list's ref.
			 */
			atomic_fetch_add(-2, &head->refs);

			return head;
		}

		/*
		 * OK, the head must have changed on us, but we still need to decrement
		 * the refcount we increased.
		 */
		refs = atomic_fetch_add(-1, &prev->refs);
		if (refs == REFS_ON_FREELIST + 1)
			__freelist_add(prev, list);
	}

	return NULL;
}

static inline void freelist_destroy(struct freelist_head *list, void *context, int (*release)(void *, void *))
{
	while (list->head) {
		struct freelist_node *item = list->head;
		list->head = item->next;
		if (release)
			release(context, item);
	}
}

#endif /* FREELIST_H */
