/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>

#define QUEUE_METHOD "zz"

struct freelist_head {
    uint32_t                fh_size;
    uint32_t                fh_mask;
    uint16_t                fh_step;
    uint16_t                fh_swap;
    uint32_t                fh_used;
    struct freelist_node  **fh_ents;
};

struct freelist_node {
    struct freelist_node	*next;
};


static inline int freelist_init(struct freelist_head *head, int max)
{
    uint32_t bits = ilog2(roundup_pow_of_two(max)), size;
    head->fh_size = 1UL << bits;
    head->fh_mask = head->fh_size - 1;
    size = head->fh_size * sizeof(struct freelist_node *);
    head->fh_ents = kzalloc(size, GFP_KERNEL);
    if (!head->fh_ents) {
        printk("freelist_init: failed to alloc ents.\n");
        return -ENOMEM;
    }
    head->fh_step = ilog2(L1_CACHE_BYTES / sizeof(void *));
    if (head->fh_step * 2 > bits)
	    head->fh_step = bits / 2;
    head->fh_swap = (1UL << head->fh_step) - 1;
    return 0;
}

#define FH_MAP(l, id) (((((id) >> ((l)->fh_step << 1)) << ((list)->fh_step << 1)) |     \
                        (((id) & (l)->fh_swap) << (l)->fh_step) |                       \
                        (((id) & ((l)->fh_swap << (l)->fh_step)) >> (l)->fh_step)) &    \
                        (l)->fh_mask)

static inline int freelist_try_add(struct freelist_node *node, struct freelist_head *list)
{
    uint32_t i;

    for (i = 0; i < list->fh_size; i++) {
        
        struct freelist_node *item = NULL;
        uint32_t slot =  FH_MAP(list, i + list->fh_used); 
        if (try_cmpxchg_release(&list->fh_ents[slot], &item, node)) {
		list->fh_used++;
		break;
	} else {
	}
    }

    return (i >= list->fh_size);
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
    uint32_t i = raw_smp_processor_id();

    while (1) {

        struct freelist_node *item = NULL;
        uint32_t slot = FH_MAP(list, i); 
        if (try_cmpxchg_release(&list->fh_ents[slot], &item, node))
            return 0;
        i++;
    }

    return -1;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *list)
{
    struct freelist_node *node = NULL;
    uint32_t i, start = raw_smp_processor_id();

    for (i = 0; i < list->fh_size; i++) {
        
        uint32_t slot = FH_MAP(list, i + start); 
	    struct freelist_node *item = smp_load_acquire(&list->fh_ents[slot]);
        if (item && try_cmpxchg_release(&list->fh_ents[slot], &item, NULL)) {
            node = item;
            break;
        }
    }

    return node;
}

static inline void freelist_destroy(struct freelist_head *list, void *context, int (*release)(void *, void *))
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

#endif /* FREELIST_H */
