/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>

struct freelist_slot {
    uint32_t                head;
    uint32_t                tail;
}

struct freelist_head {
    struct freelist_slot    fh_head ____cacheline_aligned_in_smp;
    struct freelist_slot    fh_tail ____cacheline_aligned_in_smp;
    uint32_t                fh_size;
    uint32_t                fh_mask;
    struct freelist_node  **fh_ents;
};

struct freelist_node {
	struct freelist_node	*next;
};

#define FL_ENT(l, i)  (l)->fh_ents[(i) & (l)->fh_mask]

static inline int freelist_init(struct freelist_head *list, int max)
{
    uint32_t bits = ilog2(roundup_pow_of_two(max)), size;
    list->fh_size = 1UL << bits;
    list->fh_mask = list->fh_size - 1;
    list->fh_head = list->fh_tail = 0;
    size = list->fh_size * sizeof(struct freelist_node *);
    list->fh_ents = (struct freelist_node **)kzalloc(size, GFP_KERNEL);
    if (!list->fh_ents)
        return -ENOMEM;
    printk("freelist inited for %d: size %u (%u)\n", max, list->fh_size, bits);
    return 0;
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
    uint32_t tail = READ_ONCE(list->fh_tail.head);

    while (1) {
        if (try_cmpxchg(&list->fh_tail.head, &tail, tail + 1)) {
            WRITE_ONCE(FL_ENT(list, tail), node);
            if (try_cmpxchg(&list->fh_tail.tail, &tail, tail + 1)) {
                return 0;
            } else {
                BUG();
            }
        }
    }

    return -1;
}

#define freelist_try_add freelist_add

static inline struct freelist_node *freelist_try_get(struct freelist_head *list)
{
    struct freelist_node *node;
    uint32_t head, tail;

    do {
        struct freelist_node *item;

        head = READ_ONCE(list->fh_head.head);
        if (READ_ONCE(list->fh_tail.tail) - head >= list->fh_size)
            break;

        if (try_cmpxchg(&list->fh_head.head, &head, head + 1)) {
            node = READ_ONCE(FL_ENT(list, head));
            if (try_cmpxchg(&list->fh_head.tail, &head, head + 1)) {
            	return node;
            } else {
                printk("BUG: failed to move head forward!\n");
		BUG();
	    }
        }
    } while (1);

    return NULL;
}

static inline void freelist_destroy(struct freelist_head *list, void *context, int (*release)(void *, void *))
{

    struct freelist_node *item;

    do {
       item = freelist_try_get(list);
       if (item && release)
           release(context, item);
    } while(item);

    if (list->fh_ents) {
        kfree(list->fh_ents);
        list->fh_ents = NULL;
    }
}

#endif /* FREELIST_H */
