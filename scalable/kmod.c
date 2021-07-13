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

#include "freelist.h"

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
	atomic_long_t nhits;
	atomic_long_t nmiss;
} ____cacheline_aligned_in_smp g_rs_tasks[RS_NR_CPUS];

static ktime_t g_rs_hrt_cycle;
static atomic_t g_rs_ncpus;
static atomic_t g_rs_ntasks;
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

module_param(cycleus,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(hrtimer,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(interval, long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(threads,  long, S_IRUSR|S_IRGRP|S_IROTH);
module_param(max,      int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(bulk,     int,  S_IRUSR|S_IRGRP|S_IROTH);
module_param(preempt,  int,  S_IRUSR|S_IRGRP|S_IROTH);

int rs_init_ring(int maxactive)
{
    int i;

	if (freelist_init(&g_rs_freelist, maxactive)) {
		printk("rs_init_ring: failed to init freelist.\n");
		return -ENOMEM;
	}

	for (i = 0; i < maxactive; i++) {
        struct freelist_node *ri = kzalloc(sizeof(struct freelist_node) + 16, GFP_KERNEL);
		if (ri == NULL || freelist_try_add(ri, &g_rs_freelist)) {
			if (ri)
				kfree(ri);
			return -ENOMEM;
		}
	}

    return 0;
}

static int release_ri(void *context, void *node)
{
	kfree(node);
	if (context)
		(*((int *)context))++;
	return 0;
}

void rs_fini_ring(void)
{
	int count = 0;
	freelist_destroy(&g_rs_freelist, &count, release_ri);
}

int rs_usleep(long us)
{
	usleep_range(us, us);
	return 0;
}

static int rs_ring_push(struct freelist_node *ri)
{
	int rc;

	if (!ri)
		return 0;

	get_cpu();
	rc = freelist_add(ri, &g_rs_freelist);
	put_cpu();
	return rc;
}

static struct freelist_node *rs_ring_pop(void)
{
	struct freelist_node *ri;

	get_cpu();
	ri = freelist_try_get(&g_rs_freelist);
	if (ri)
		atomic_long_inc(&g_rs_tasks[raw_smp_processor_id()].nhits);
	else
		atomic_long_inc(&g_rs_tasks[raw_smp_processor_id()].nmiss);
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
        int nns = 0;
	int rc = atomic_inc_return(&g_rs_ntasks);

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

	if (rc == 1) {
		g_rs_ns_start = ktime_get_ns();
		WRITE_ONCE(g_rs_task_started, 1);
	} else if (rc > threads)
		goto quit;

	// kthread_bind_mask(current, cpu_online_mask);

	while (!kthread_should_stop()) {

		int n;

		if (g_rs_task_stop)
			break;

		for (n = 0; n < nns; n++)
			nodes[n] = rs_ring_pop();
		if (cycleus)
			rs_usleep(cycleus);
		for (n = 0; n < nns; n++) {
			if (nodes[n])
				rs_ring_push(nodes[n]);
		}
	}

quit:
	if (atomic_dec_and_test(&g_rs_ntasks)) {
		g_rs_ns_stop = ktime_get_ns();
		complete(&g_rs_task_exit);
	}

	if (nodes && nodes != stack)
		kfree(nodes);

	return 0;
}

static int rs_cpu_online(unsigned int cpu)
{
	struct task_struct *thread;
	int rc = 0;

	if (g_rs_tasks[cpu].task != NULL)
		return 0;

	mutex_lock(&g_rs_task_mutex);
	thread = kthread_create_on_node(rs_task_exec,
					(void *) (long)cpu,
					cpu_to_node(cpu),
					QUEUE_METHOD);
	if (IS_ERR(thread)) {
		rc = PTR_ERR(thread);
		goto errorout;
	}

	g_rs_tasks[cpu].task = thread;
	// kthread_bind(thread, cpu);
	atomic_inc(&g_rs_ncpus);

	tasklet_init(&g_rs_tasks[cpu].tasklet, rs_tasklet_work,
	(unsigned long)&g_rs_tasks[cpu].tasklet);
	rs_start_hrtimer(&g_rs_tasks[cpu].hrtimer);

errorout:
	mutex_unlock(&g_rs_task_mutex);
	return rc;
}

static int rs_cpu_offline(unsigned int cpu)
{
	struct task_struct *thread = NULL;

	if (!g_rs_tasks[cpu].task)
		return 0;

	rs_stop_hrtimer(&g_rs_tasks[cpu].hrtimer);
	tasklet_kill(&g_rs_tasks[cpu].tasklet);
	mutex_lock(&g_rs_task_mutex);
	thread = g_rs_tasks[cpu].task;
	g_rs_tasks[cpu].task = NULL;
	mutex_unlock(&g_rs_task_mutex);

	if (thread)
		kthread_stop(thread);
	return 0;
}

static void rs_start_tasks(void)
{
        int i;

        g_rs_ns_start = ktime_get_ns();
        for (i = 0 ; i < RS_NR_CPUS; i++) {
                if (!g_rs_tasks[i].task)
                        break;
                get_task_struct(g_rs_tasks[i].task);
                wake_up_process(g_rs_tasks[i].task);
        }
}

static void rs_stop_tasks(void)
{
	int i;
	g_rs_task_stop = 1;

	for (i = 0 ; i < RS_NR_CPUS; i++) {
		if (g_rs_tasks[i].task) {
			kthread_stop(g_rs_tasks[i].task);
			put_task_struct(g_rs_tasks[i].task);
			g_rs_tasks[i].task = NULL;
		}
	}
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
		nhits += atomic_long_read(&g_rs_tasks[i].nhits);
		nmiss += atomic_long_read(&g_rs_tasks[i].nmiss);
	}
	printk("%s:\tthreads:%2lu preempt:%d max:%4d cycle:%2lu bulk:%d delta: %llu  hits: %13llu missed: %llu\n", 
		QUEUE_METHOD, threads, preempt, max, cycleus, bulk, g_rs_ns_stop - g_rs_ns_start, nhits, nmiss);
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
	init_completion(&g_rs_task_exit);
	g_rs_cpuhp = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "krpperf",
					rs_cpu_online, rs_cpu_offline);
	if (g_rs_cpuhp < 0) {
		rc = (int)g_rs_cpuhp;
		goto errorout;
	}

	/* starting percpu tasks */
	rs_start_tasks();

	msleep(interval * 1000);
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
