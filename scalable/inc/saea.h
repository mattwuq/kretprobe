/* SPDX-License-Identifier: GPL-2.0-only OR BSD-2-Clause */
#ifndef FREELIST_H
#define FREELIST_H

#include <linux/slab.h>
#include <linux/atomic.h>


struct freelist_c1_aligned {
    union {
        struct freelist_node *node;
        char   dummy[L1_CACHE_BYTES];
    };
};
    
struct freelist_ents {
	//struct freelist_node *ents[NR_CPUS];
    struct freelist_c1_aligned ents[NR_CPUS];
};


struct freelist_head {
    uint32_t                fh_size;
    uint32_t                fh_mask;
    uint32_t                fh_step;
    atomic_t                fh_used;
    struct freelist_ents   *fh_ents;
};

struct freelist_node {
	struct freelist_node	*next;
};


static inline int freelist_init(struct freelist_head *head, int max)
{
    uint32_t bits = ilog2(roundup_pow_of_two(max)), size;
    head->fh_size = 1UL << bits;
    head->fh_mask = head->fh_size - 1;
    atomic_set(&head->fh_used, 0);
    size = head->fh_size * sizeof(struct freelist_ents) / NR_CPUS;
    head->fh_ents = (struct freelist_ents *)kzalloc(size, GFP_KERNEL);
    if (!head->fh_ents)
        return -ENOMEM;
    head->fh_step = 0;
    return 0;
}

static inline int freelist_try_add(struct freelist_node *node, struct freelist_head *list)
{
    uint32_t i, start = ((uint32_t)atomic_read(&list->fh_used)) << list->fh_step;

    for (i = 0; i < list->fh_size; i++) {
        
	    struct freelist_node *item = NULL;
        uint32_t slot = (i + start) & list->fh_mask; 
        if (try_cmpxchg_release(&list->fh_ents->ents[slot].node, &item, node)) {
            atomic_inc(&list->fh_used);
            break;
        }
    }

    return (i >= list->fh_size);
}

static inline int freelist_add(struct freelist_node *node, struct freelist_head *list)
{
    uint32_t slot = (raw_smp_processor_id() << list->fh_step) & list->fh_mask;

    while (1) {
        
	    struct freelist_node *item = NULL;
        if (try_cmpxchg_release(&list->fh_ents->ents[slot].node, &item, node)) {
            atomic_inc(&list->fh_used);
            break;
        }
        slot = (slot + 1) & list->fh_mask; 
    }

    return 0;
}

static inline struct freelist_node *freelist_try_get(struct freelist_head *list)
{
	struct freelist_node *node = NULL;
    uint32_t i, start = raw_smp_processor_id() << list->fh_step;

    for (i = 0; i < list->fh_size; i++) {
        
        uint32_t slot = (i + start) & list->fh_mask; 
	    struct freelist_node *item = READ_ONCE(list->fh_ents->ents[slot].node);
        if (item && try_cmpxchg_release(&list->fh_ents->ents[slot].node, &item, NULL)) {
            node = item;
            break;
        }
    }

    return node;
}

static inline void freelist_destroy(struct freelist_head *list, void *context, int (*release)(void *, void *))
{
    uint32_t i;

    for (i = 0; list->fh_ents && i < list->fh_size; i++) {
        uint32_t slot = i & list->fh_mask; 
	    struct freelist_node *item = READ_ONCE(list->fh_ents->ents[slot].node);
        while (item) {
            if (try_cmpxchg_release(&list->fh_ents->ents[slot].node, &item, NULL)) {
                if (release)
                    release(context, item);
                atomic_dec(&list->fh_used);
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
