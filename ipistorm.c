/*
 * Stress smp_call_function
 *
 * Copyright 2017-2020 Anton Blanchard, IBM Corporation <anton@linux.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) "ipistorm: " fmt

#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/delay.h>

static long timeout = 10;
module_param(timeout, long, S_IRUGO);
MODULE_PARM_DESC(timeout, "Timeout in seconds (default = 10)");

static bool wait = true;
module_param(wait, bool, S_IRUGO);
MODULE_PARM_DESC(wait, "Wait for IPI to finish? (default true)");

static unsigned long source = 0;
module_param(source, ulong, S_IRUGO);
MODULE_PARM_DESC(source, "IPI source CPU (default 0)");

static unsigned long target = 1;
module_param(target, ulong, S_IRUGO);
MODULE_PARM_DESC(target, "IPI target CPU (default 1)");

static unsigned long delay = 0;
module_param(delay, ulong, S_IRUGO);
MODULE_PARM_DESC(delay, "Delay between calls in us (default 0)");

static atomic_t stop_test;
static atomic_t source_running;
static atomic_t target_running;

static DECLARE_COMPLETION(ipistorm_done);

static unsigned long delta_hist[32];

static unsigned long remote_tb;
static void do_nothing_ipi(void *dummy)
{
	remote_tb = mftb();
}

static int ipistorm_source_thread(void *data)
{
	unsigned long tb_start, tb;
	unsigned long min = ~0UL, max = 0, total = 0;
	unsigned long i = 0;

	atomic_inc(&source_running);
	smp_mb();
	while (!atomic_read(&target_running))
		cpu_relax();

	tb_start = mftb();

	while ((tb = mftb()) - tb_start < tb_ticks_per_usec * timeout * USEC_PER_SEC) {
		unsigned long delta; 

		smp_call_function_single(target, do_nothing_ipi, NULL, wait);

		delta = tb_to_ns(remote_tb - tb);

		total += delta;
		if (delta < min)
			min = delta;
		if (delta > max)
			max = delta;
		delta_hist[ilog2(delta)]++;

		if (delay)
			usleep_range(delay, delay+1);
		else
//			cond_resched();
			;

		i++;
	}

	atomic_inc(&stop_test);

	printk("%lu IPIs completed\n", i);
	printk("min=%luns max=%luns avg=%luns\n", min, max, total / i);
	for (i = 0; i < 32; i++)
		printk("ns < %lu = %lu\n", 1UL << i, delta_hist[i]);

	complete(&ipistorm_done);

	return 0;
}

static int ipistorm_target_thread(void *data)
{
	atomic_inc(&target_running);
	smp_mb();
	while (!atomic_read(&stop_test))
		;

	return 0;
}

static int __init ipistorm_init(void)
{
	struct task_struct *s, *t;

	pr_info("CPU%lu -> CPU%lu\n", source, target);

	init_completion(&ipistorm_done);

	t = kthread_create(ipistorm_target_thread, NULL,
			   "ipistorm_target/%lu", target);
	if (IS_ERR(t)) {
		pr_err("kthread_create on CPU %lu failed\n", target);
		return PTR_ERR(t);
	}

	kthread_bind(t, target);
	wake_up_process(t);

	s = kthread_create(ipistorm_source_thread, NULL,
			   "ipistorm/%lu", source);
	if (IS_ERR(s)) {
		pr_err("kthread_create on CPU %lu failed\n", source);
		atomic_inc(&stop_test);
		kthread_stop(t);
		return PTR_ERR(s);
	}

	kthread_bind(s, source);
	wake_up_process(s);

	wait_for_completion(&ipistorm_done);

	kthread_stop(s);
	kthread_stop(t);

	return -EAGAIN;
}

static void __exit ipistorm_exit(void)
{
}

module_init(ipistorm_init)
module_exit(ipistorm_exit)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Anton Blanchard");
MODULE_DESCRIPTION("Stress smp_call_function");
