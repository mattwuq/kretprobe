/* SPDX-License-Identifier: GPL-3.0 */

/*
 * insmod krp.ko threads=4 cycleus=0 max=16 preempt=0; dmesg -c
 * dmesg -c > /dev/null; insmod krp.ko interval=10 threads=4 cycleus=0 max=16 preempt=0; dmesg -c
 * 
 */

#include <linux/version.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/hrtimer.h>
#include <linux/cpumask.h>
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

#ifndef try_cmpxchg
#define try_cmpxchg(_ptr, _oldp, _new) \
({ \
        typeof(*(_ptr)) *___op = (_oldp), ___o = *___op, ___r; \
        ___r = arch_cmpxchg((_ptr), ___o, (_new)); \
        if (unlikely(___r != ___o)) \
                *___op = ___r; \
        likely(___r == ___o); \
})
#endif /* try_cmpxchg */

#ifndef try_cmpxchg_acquire
#define try_cmpxchg_acquire(_ptr, _oldp, _new) \
({ \
        typeof(*(_ptr)) *___op = (_oldp), ___o = *___op, ___r; \
        ___r = arch_cmpxchg_acquire((_ptr), ___o, (_new)); \
        if (unlikely(___r != ___o)) \
                *___op = ___r; \
        likely(___r == ___o); \
})
#endif /* try_cmpxchg_acquire */

#ifndef try_cmpxchg_release
#define try_cmpxchg_release(_ptr, _oldp, _new) \
({ \
        typeof(*(_ptr)) *___op = (_oldp), ___o = *___op, ___r; \
        ___r = arch_cmpxchg_release((_ptr), ___o, (_new)); \
        if (unlikely(___r != ___o)) \
                *___op = ___r; \
        likely(___r == ___o); \
})
#endif

#include "freelist.h"

/*
 * global definitions
 */

#define RS_NR_CPUS  (96)
#define RS_BULK_MAX (96)

/*
 * global variables
 */
static DEFINE_MUTEX(g_rs_task_mutex);
static enum cpuhp_state g_rs_cpuhp;

struct rs_taskitem_percpu {
	struct task_struct *task;
	struct hrtimer hrtimer;
	struct tasklet_struct tasklet;
	unsigned long nhits;
	unsigned long nmiss;
	int           started;
	char          dummy[64];
} ____cacheline_aligned_in_smp g_rs_tasks[RS_NR_CPUS];

static ktime_t g_rs_hrt_cycle;
static atomic_t g_rs_ncpus;
static atomic_t g_rs_ntasks;
struct completion g_rs_task_exec;
struct completion g_rs_task_exit;
static int g_rs_task_started;
static int g_rs_task_stop;
static u64 g_rs_ns_start, g_rs_ns_stop;
static struct freelist_head g_rs_freelist; 

/*
 * module parameters
 */
static long cycleus  = 10;                   /* thread internal: us */
static long hrtimer  = 10000000UL;           /* hrtimer internal: 1ms */
static long interval = 1;                    /* seconds */
static long threads  = 1;
static int  preempt  = 0;
static int  max = 0;
static int  bulk = 1;
static int  numa = 1;
static int  stride=2;
static int  ncpus=0;

module_param(cycleus,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(hrtimer,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(interval, long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(threads,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(max,      int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(numa,     int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(bulk,     int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(preempt,  int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(stride,   int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(ncpus,    int,  S_IRUSR|S_IRGRP|S_IROTH);

int *g_freelist_items = NULL;

int rs_init_ring(int maxactive)
{
	int i;

	g_freelist_items = kzalloc(sizeof(int) * maxactive, GFP_KERNEL);
	if (!g_freelist_items)
		return -ENOMEM;

	if (freelist_init(&g_rs_freelist, maxactive)) {
		printk("rs_init_ring: failed to init freelist.\n");
		return -ENOMEM;
	}

	for (i = 0; i < maxactive; i++) {
		if (strstr(QUEUE_METHOD, "fl") || 0 == strcmp(QUEUE_METHOD, "pc")) {
			struct freelist_node *ri;
			ri = kzalloc(sizeof(struct freelist_node) + 16, GFP_KERNEL);
			if (ri == NULL)
			       continue;
			ri->id = i + 1;
			if (freelist_try_add(ri, &g_rs_freelist)) {
				if (ri)
					 kfree(ri);
			}
			g_freelist_items[i] = 1;
		} else {
			struct freelist_node *ri = ((void *) -1) - i;
			if (freelist_try_add(ri, &g_rs_freelist))
				return -ENOMEM;
			g_freelist_items[i] = 1;
		}
	}

    return 0;
}

static int release_ri(void *context, void *node)
{
	struct freelist_node *ri = node;
	int id;

	if (strstr(QUEUE_METHOD, "fl") || 0 == strcmp(QUEUE_METHOD, "pc")) {
		id = ri->id - 1;
	} else {
		id = (int)(((void *)-1) - (void *)node);
		node = NULL;
	}
	if (id >=0 && id < max) {
		if (g_freelist_items[id]) {
			g_freelist_items[id] = 0;
		} else {
			printk("doulbe free node: %px id: %d\n", node, id);
			node = NULL;
		}
	} else {
		printk("wrong node: %px id: %d\n", node, id);
	}
	if (node)
		kfree(node);
	if (context)
		(*((int *)context))++;
	return 0;
}

void rs_fini_ring(void)
{
	int count = 0;
	freelist_destroy(&g_rs_freelist, &count, release_ri);

	if (g_freelist_items)
		kfree(g_freelist_items);
}

int rs_usleep(long us)
{
	usleep_range(us, us);
	return 0;
}

static int rs_ring_push(struct freelist_node *ri)
{
	int rc, id;

	if (!ri)
		return 0;

	get_cpu();

        id = (int)(((void *)-1) - (void *)ri);
        if (id < 0 || id >= max)
		id = ri->id - 1;
	if (id >=0 && id < max) {
		if (!g_freelist_items[id])
			g_freelist_items[id] = 1;
		else
		        printk("node %px ud: %d  already in.\n", ri, id);
	} else {
		printk("wrong node: %px id: %d\n", ri, id);
        }

	rc = freelist_add(ri, &g_rs_freelist);
	put_cpu();
	return rc;
}

static struct freelist_node *rs_ring_pop(void)
{
	struct freelist_node *ri;

	get_cpu();
	ri = freelist_try_get(&g_rs_freelist);
	if (ri) {
		int id = (int)(((void *)-1) - (void *)ri);
		if (id < 0 || id >= max)
			id = ri->id - 1;
		if (id < 0 || id >= max) {
			printk("wrong node poped: %px id: %d\n", ri, id);
		} else {
			if (g_freelist_items[id])
				g_freelist_items[id] = 0;
			else
				printk("node %px ud: %d was taken.\n", ri, id);
		}
	}
	put_cpu();
	return ri;
}

void rs_tasklet_work(unsigned long data)
{
	if (g_rs_task_stop) {
	} else if (preempt) {
		struct freelist_node *ri = rs_ring_pop();
		rs_ring_push(ri);
	}

	return;
	(data = data);
}

static enum hrtimer_restart rs_hrtimer_handler(struct hrtimer *hrt)
{
	if (READ_ONCE(g_rs_task_stop) ||!READ_ONCE(g_rs_task_started))
		goto restart;

	if (ktime_get_ns() - g_rs_ns_start >= interval * 1000000000UL) {
		WRITE_ONCE(g_rs_task_stop, 1);
	} else {
		tasklet_schedule(&g_rs_tasks[raw_smp_processor_id()].tasklet);
	}

restart:
	hrtimer_forward(hrt, hrt->base->get_time(), g_rs_hrt_cycle);
	return HRTIMER_RESTART;
}

static void rs_start_hrtimer(struct hrtimer *hrt)
{
	if (!hrt)
		return;

	hrtimer_init(hrt, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	hrt->function = rs_hrtimer_handler;
	hrtimer_start(hrt, g_rs_hrt_cycle, HRTIMER_MODE_REL);
}

static void rs_stop_hrtimer(struct hrtimer *hrt)
{
	if (hrt)
		hrtimer_cancel(hrt);
}

uint32_t rs_reset_slot(int cpu);
static int rs_task_exec(void *arg)
{
        struct freelist_node  *stack[64] = {0};
	struct freelist_node **nodes = NULL;
	struct rs_taskitem_percpu *ti;
	unsigned nhits =0, nmiss = 0;
        int nns = 0, rc, cpu = (int)(long)arg;

	ti = (void *)&g_rs_tasks[cpu];
	ti->started = 1;

        if (bulk > 64) {
	        nodes = kzalloc(sizeof(void *) * bulk, GFP_KERNEL);
	        if (nodes) {
                        nns = bulk;
                } else {
                        nns = 64;
                        nodes = stack;
                }
	} else {
                nns = bulk;
                nodes = stack;
        }

	do {
		if (kthread_should_stop() || g_rs_task_stop)
			goto quit;
	} while (!wait_for_completion_timeout(&g_rs_task_exec, 10));

	rc = atomic_inc_return(&g_rs_ntasks);
	if (rc < threads)
		complete(&g_rs_task_exec);

	while (!kthread_should_stop()) {
		int n;

		if (g_rs_task_stop)
			break;

		for (n = 0; n < nns; n++)
			nodes[n] = rs_ring_pop();
		if (cycleus)
			rs_usleep(cycleus);
		for (n = 0; n < nns; n++) {
			if (nodes[n]) {
				nhits++;
				rs_ring_push(nodes[n]);
			} else {
				nmiss++;
			}
		}
	}

	ti->nhits = nhits;
	ti->nmiss = nmiss;
	if (atomic_dec_and_test(&g_rs_ntasks)) {
		g_rs_ns_stop = ktime_get_ns();
		complete(&g_rs_task_exit);
	}

quit:
	if (nodes && nodes != stack)
		kfree(nodes);

	mutex_lock(&g_rs_task_mutex);
	ti->task = NULL;
	ti->started = 0;
	mutex_unlock(&g_rs_task_mutex);

	do_exit(0);
	return 0;
}

static int rs_cpu_online(unsigned int cpu)
{
	atomic_inc(&g_rs_ncpus);
	tasklet_init(&g_rs_tasks[cpu].tasklet, rs_tasklet_work,
	(unsigned long)&g_rs_tasks[cpu].tasklet);
	rs_start_hrtimer(&g_rs_tasks[cpu].hrtimer);

	return 0;
}

static int rs_cpu_offline(unsigned int cpu)
{
	rs_stop_hrtimer(&g_rs_tasks[cpu].hrtimer);
	tasklet_kill(&g_rs_tasks[cpu].tasklet);
	return 0;
}

struct task_struct *rs_start_thread(int cpu)
{
	struct task_struct *thread;
	thread = kthread_create_on_node(rs_task_exec,
					(void *) (long)cpu,
					cpu_to_node(cpu),
					QUEUE_METHOD);
	if (IS_ERR(thread))
		return NULL;

	kthread_bind(thread, cpu);
	// kthread_bind_mask(current, cpu_online_mask);
	wake_up_process(thread);

	return thread;
}

static void rs_start_tasks(void)
{
        int i, n = 0, cpus = ncpus;
	unsigned long node = 0;

	mutex_lock(&g_rs_task_mutex);
	if (!numa) {
		int s;
		for (s = stride; s > 0 && n < threads; s--) {
			for (i = cpus + s - 1 - stride; i >= 0 && n < threads; i-=stride) {
				while (!g_rs_tasks[i].task) {
               				g_rs_tasks[i].task = rs_start_thread(i);
					if (g_rs_tasks[i].task)
						n++;
				}
			}
		}
	} else {
		while (n < threads) {
			for (i = cpus - 1 ; i >= 0 && n < threads; i--) {
				if (g_rs_tasks[i].task) {
					if (i == cpus - 1)
						cpus--;
					continue;
				}
				if (0 == (node & (1 << cpu_to_node(i)))) {
					node |= (1 << cpu_to_node(i));
					if (hweight_long(node) == nr_online_nodes)
						node = 0;
					while (!g_rs_tasks[i].task) {
						g_rs_tasks[i].task = rs_start_thread(i);
						if (g_rs_tasks[i].task)
							n++;
					}
				}
			}
		}
	}
	mutex_unlock(&g_rs_task_mutex);

	g_rs_ns_start = ktime_get_ns();
	WRITE_ONCE(g_rs_task_started, 1);
	complete(&g_rs_task_exec);
}


static void rs_stop_tasks(void)
{
	int i;

	g_rs_task_stop = 1;
	mutex_lock(&g_rs_task_mutex);
	for (i = 0 ; i < RS_NR_CPUS; i++) {
		if (g_rs_tasks[i].task) {
			complete(&g_rs_task_exec);
			g_rs_tasks[i].task = NULL;
		}
	}
	mutex_unlock(&g_rs_task_mutex);
	wait_for_completion(&g_rs_task_exit);
}

static void __init rs_check_params(void)
{
	if (!max)
		max = num_possible_cpus();

	if (cycleus > 1000UL * 1000 * 1000 * 60)
		cycleus = 1000UL * 1000 * 1000 * 60;  /* 1 minute */

	if (hrtimer < 1000000)
		hrtimer = 1000000;                    /* 1 milliisecond */
	if (hrtimer > 1000000000UL * 60)
		hrtimer = 1000000000UL * 60;

	if (bulk <= 0)
		bulk = 1;

	if (!threads)
		threads = 1;
	if (threads > num_online_cpus())
		threads = num_online_cpus();

	if (nr_online_nodes <= 1)
		numa = 0;
	if (!numa) {
		if (!ncpus)
			ncpus = num_possible_cpus();
		if (!stride)
			stride = 2;
	}
}

static void rs_cleanup(void)
{
	int i;
	u64 nhits = 0, nmiss = 0;
	if (g_rs_cpuhp >= 0)
		cpuhp_remove_state(g_rs_cpuhp);
	rs_stop_tasks();
	rs_fini_ring();
	for (i = 0; i < RS_NR_CPUS; i++) {
		nhits += g_rs_tasks[i].nhits;
		nmiss += g_rs_tasks[i].nmiss;
		if (g_rs_tasks[i].nhits) {
			printk("task %d: nhits: %ld nmiss: %ld\n", i,
				g_rs_tasks[i].nhits, g_rs_tasks[i].nmiss);
		}
	}
	printk("%s:\tnuma:%u threads:%2lu preempt:%d max:%4d cycle:%2lu bulk:%d delta: %llu  hits: %13llu missed: %llu\n",
		QUEUE_METHOD, numa, threads, preempt, max, cycleus, bulk, g_rs_ns_stop - g_rs_ns_start, nhits, nmiss);
}

static int __init rs_mod_init(void)
{
	int rc = 0;

	/* checking module parmaeters */
	rs_check_params();

	/* initializing krp ring */
	rc = rs_init_ring(max);
	if (rc)
		goto errorout;

	/* starting hrtimer */
	g_rs_hrt_cycle = ktime_set(0, hrtimer);
	init_completion(&g_rs_task_exec);
	init_completion(&g_rs_task_exit);
	g_rs_cpuhp = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "krpperf",
					rs_cpu_online, rs_cpu_offline);
	if (g_rs_cpuhp < 0) {
		rc = (int)g_rs_cpuhp;
		goto errorout;
	}

	/* starting percpu tasks */
	rs_start_tasks();

	msleep((interval + 1) * 1000);
	rs_cleanup();   
	return -1;

errorout:

	if (rc)
		rs_fini_ring();
	return rc;
}

static void __exit rs_mod_exit(void)
{
	rs_cleanup();
}

module_init(rs_mod_init);
module_exit(rs_mod_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matt Wu");
