/*
 * CPUFreq governor based on scheduler-provided CPU utilization data.
 *
 * Copyright (C) 2016, Intel Corporation
 * Author: Rafael J. Wysocki <rafael.j.wysocki@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/cpu_pm.h>

#include <trace/events/power.h>

#include "sched.h"
#include "tune.h"

unsigned long boosted_cpu_util(int cpu);

/* Stub out fast switch routines present on mainline to reduce the backport
 * overhead. */
#define cpufreq_driver_fast_switch(x, y) 0
#define cpufreq_enable_fast_switch(x)
#define cpufreq_disable_fast_switch(x)
#define LATENCY_MULTIPLIER			(1000)

struct sugov_tunables {
	struct gov_attr_set attr_set;
	unsigned int up_rate_limit_us;
	unsigned int down_rate_limit_us;
};

struct sugov_policy {
	struct cpufreq_policy *policy;

	struct sugov_tunables *tunables;
	struct list_head tunables_hook;

	raw_spinlock_t update_lock;  /* For shared policies */
	u64 last_freq_update_time;
	s64 min_rate_limit_ns;
	s64 up_rate_delay_ns;
	s64 down_rate_delay_ns;
	unsigned int next_freq;
	unsigned int cached_raw_freq;

	/* The next fields are only needed if fast switch cannot be used. */
	struct irq_work irq_work;
	struct work_struct work;
	struct mutex work_lock;
	bool work_in_progress;

	bool need_freq_update;
};

struct sugov_cpu {
	struct update_util_data update_util;
	struct sugov_policy *sg_policy;

	unsigned long iowait_boost;
	unsigned long iowait_boost_max;
	u64 last_update;

	/* The fields below are only needed when sharing a policy. */
	unsigned long util;
	unsigned long max;
	unsigned int flags;

	/* The field below is for single-CPU policies only. */
#ifdef CONFIG_NO_HZ_COMMON
	unsigned long saved_idle_calls;
#endif
};

static DEFINE_PER_CPU(struct sugov_cpu, sugov_cpu);

/******************* exynos specific function *******************/
#define DEFAULT_EXPIRED_TIME	70
struct sugov_exynos {
	/* for slack timer */
	unsigned long min;
	int enabled;
	bool started;
	int expired_time;
	struct timer_list timer;

	/* pm_qos_class */
	int qos_min_class;
};
static DEFINE_PER_CPU(struct sugov_exynos, sugov_exynos);
static void sugov_stop_slack(int cpu);
static void sugov_start_slack(int cpu);
static void sugov_update_min(struct cpufreq_policy *policy);

/************************ Governor internals ***********************/

static bool sugov_should_update_freq(struct sugov_policy *sg_policy, u64 time)
{
	s64 delta_ns;

	if (sg_policy->work_in_progress)
		return false;

	if (unlikely(sg_policy->need_freq_update)) {
		sg_policy->need_freq_update = false;
		/*
		 * This happens when limits change, so forget the previous
		 * next_freq value and force an update.
		 */
		sg_policy->next_freq = UINT_MAX;
		return true;
	}

	delta_ns = time - sg_policy->last_freq_update_time;

	/* No need to recalculate next freq for min_rate_limit_us at least */
	return delta_ns >= sg_policy->min_rate_limit_ns;
}

static bool sugov_up_down_rate_limit(struct sugov_policy *sg_policy, u64 time,
				     unsigned int next_freq)
{
	s64 delta_ns;

	delta_ns = time - sg_policy->last_freq_update_time;

	if (next_freq > sg_policy->next_freq &&
	    delta_ns < sg_policy->up_rate_delay_ns)
			return true;

	if (next_freq < sg_policy->next_freq &&
	    delta_ns < sg_policy->down_rate_delay_ns)
			return true;

	return false;
}

static int sugov_select_scaling_cpu(void)
{
	int cpu;
	cpumask_t mask;

	cpumask_clear(&mask);
	cpumask_and(&mask, cpu_coregroup_mask(0), cpu_online_mask);

	/* Idle core of the boot cluster is selected to scaling cpu */
	for_each_cpu(cpu, &mask)
		if (idle_cpu(cpu))
			return cpu;

	/* if panic_cpu is not Little core, mask will be empty */
	if (unlikely(!cpumask_weight(&mask))) {
		cpu = atomic_read(&panic_cpu);
		if (cpu != PANIC_CPU_INVALID)
			return cpu;
	}

	return cpumask_weight(&mask) - 1;
}

static void sugov_update_commit(struct sugov_policy *sg_policy, u64 time,
				unsigned int next_freq)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	int cpu;

	if (sugov_up_down_rate_limit(sg_policy, time, next_freq)) {
		/* Reset cached freq as next_freq isn't changed */
		sg_policy->cached_raw_freq = 0;
		return;
	}

	if (sg_policy->next_freq == next_freq)
		return;

	sg_policy->next_freq = next_freq;
	sg_policy->last_freq_update_time = time;

	if (policy->fast_switch_enabled) {
		next_freq = cpufreq_driver_fast_switch(policy, next_freq);
		if (next_freq == CPUFREQ_ENTRY_INVALID)
			return;

		policy->cur = next_freq;
		trace_cpu_frequency(next_freq, smp_processor_id());
	} else {
		cpu = sugov_select_scaling_cpu();
		if (cpu < 0)
			return;

		sg_policy->work_in_progress = true;
		irq_work_queue_on(&sg_policy->irq_work, cpu);
	}
}

#ifdef CONFIG_FREQVAR_TUNE
unsigned int freqvar_tipping_point(int cpu, unsigned int freq);
#else
static inline unsigned int freqvar_tipping_point(int cpu, unsigned int freq)
{
	return  freq + (freq >> 2);
}
#endif

/**
 * get_next_freq - Compute a new frequency for a given cpufreq policy.
 * @sg_policy: schedutil policy object to compute the new frequency for.
 * @util: Current CPU utilization.
 * @max: CPU capacity.
 *
 * If the utilization is frequency-invariant, choose the new frequency to be
 * proportional to it, that is
 *
 * next_freq = C * max_freq * util / max
 *
 * Otherwise, approximate the would-be frequency-invariant utilization by
 * util_raw * (curr_freq / max_freq) which leads to
 *
 * next_freq = C * curr_freq * util_raw / max
 *
 * Take C = 1.25 for the frequency tipping point at (util / max) = 0.8.
 *
 * The lowest driver-supported frequency which is equal or greater than the raw
 * next_freq (as calculated above) is returned, subject to policy min/max and
 * cpufreq driver limitations.
 */
static unsigned int get_next_freq(struct sugov_policy *sg_policy,
				  unsigned long util, unsigned long max)
{
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned int freq = arch_scale_freq_invariant() ?
				policy->max : policy->cur;

	freq = freqvar_tipping_point(policy->cpu, freq) * util / max;

	if (freq == sg_policy->cached_raw_freq && sg_policy->next_freq != UINT_MAX)
		return sg_policy->next_freq;
	sg_policy->cached_raw_freq = freq;
	return cpufreq_driver_resolve_freq(policy, freq);
}

static inline bool use_pelt(void)
{
#ifdef CONFIG_SCHED_WALT
	return (!sysctl_sched_use_walt_cpu_util || walt_disabled);
#else
	return true;
#endif
}

extern unsigned int sched_rt_remove_ratio_for_freq;

static void sugov_get_util(unsigned long *util, unsigned long *max, u64 time)
{
	int cpu = smp_processor_id();
	unsigned long max_cap;
	unsigned long rt_avg = cpu_rq(cpu)->rt.avg.util_avg;

	max_cap = arch_scale_cpu_capacity(NULL, cpu);

	*util = boosted_cpu_util(cpu);
	
	if (sched_feat(UTIL_EST)) {
		*util = max_t(unsigned long, *util,
			     READ_ONCE(cpu_rq(cpu)->cfs.avg.util_est.enqueued));
	}
	
	if (sched_rt_remove_ratio_for_freq)
		*util -= ((rt_avg * sched_rt_remove_ratio_for_freq) / 100);
	if (likely(use_pelt()))
		*util = min(*util, max_cap);

	*max = max_cap;
}

static void sugov_set_iowait_boost(struct sugov_cpu *sg_cpu, u64 time,
				   unsigned int flags)
{
	if (flags & SCHED_CPUFREQ_IOWAIT) {
		sg_cpu->iowait_boost = sg_cpu->iowait_boost_max;
	} else if (sg_cpu->iowait_boost) {
		s64 delta_ns = time - sg_cpu->last_update;

		/* Clear iowait_boost if the CPU apprears to have been idle. */
		if (delta_ns > TICK_NSEC)
			sg_cpu->iowait_boost = 0;
	}

	/* HACK: block iowait boost to avoid unnecessary setting max frequency */
	sg_cpu->iowait_boost = 0;
}

static void sugov_iowait_boost(struct sugov_cpu *sg_cpu, unsigned long *util,
			       unsigned long *max)
{
	unsigned long boost_util = sg_cpu->iowait_boost;
	unsigned long boost_max = sg_cpu->iowait_boost_max;

	if (!boost_util)
		return;

	if (*util * boost_max < *max * boost_util) {
		*util = boost_util;
		*max = boost_max;
	}
	sg_cpu->iowait_boost >>= 1;
}

#ifdef CONFIG_NO_HZ_COMMON
static bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu)
{
	unsigned long idle_calls = tick_nohz_get_idle_calls();
	bool ret = idle_calls == sg_cpu->saved_idle_calls;

	sg_cpu->saved_idle_calls = idle_calls;
	return ret;
}
#else
static inline bool sugov_cpu_is_busy(struct sugov_cpu *sg_cpu) { return false; }
#endif /* CONFIG_NO_HZ_COMMON */

static void sugov_update_single(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util, max;
	unsigned int next_f;
	bool busy;

	sugov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	if (!sugov_should_update_freq(sg_policy, time))
		return;

	busy = sugov_cpu_is_busy(sg_cpu);

	if (flags & SCHED_CPUFREQ_DL) {
		next_f = policy->cpuinfo.max_freq;
	} else {
		sugov_get_util(&util, &max, time);
		sugov_iowait_boost(sg_cpu, &util, &max);
		next_f = get_next_freq(sg_policy, util, max);
		/*
		 * Do not reduce the frequency if the CPU has not been idle
		 * recently, as the reduction is likely to be premature then.
		 */
		if (busy && next_f < sg_policy->next_freq) {
			next_f = sg_policy->next_freq;

			/* Reset cached freq as next_freq has changed */
			sg_policy->cached_raw_freq = 0;
		}
	}
	sugov_update_commit(sg_policy, time, next_f);
}

static unsigned int sugov_next_freq_shared(struct sugov_cpu *sg_cpu, u64 time)
{
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	struct cpufreq_policy *policy = sg_policy->policy;
	unsigned long util = 0, max = 1;
	unsigned int j;

	for_each_cpu_and(j, policy->related_cpus, cpu_online_mask) {
		struct sugov_cpu *j_sg_cpu = &per_cpu(sugov_cpu, j);
		unsigned long j_util, j_max;
		s64 delta_ns;

		/*
		 * If the CPU utilization was last updated before the previous
		 * frequency update and the time elapsed between the last update
		 * of the CPU utilization and the last frequency update is long
		 * enough, don't take the CPU into account as it probably is
		 * idle now (and clear iowait_boost for it).
		 */
		delta_ns = time - j_sg_cpu->last_update;
		if (delta_ns > TICK_NSEC) {
			j_sg_cpu->iowait_boost = 0;
			continue;
		}
		if (j_sg_cpu->flags & SCHED_CPUFREQ_DL)
			return policy->cpuinfo.max_freq;

		j_util = j_sg_cpu->util;
		j_max = j_sg_cpu->max;
		if (j_util * max > j_max * util) {
			util = j_util;
			max = j_max;
		}

		sugov_iowait_boost(j_sg_cpu, &util, &max);
	}

	return get_next_freq(sg_policy, util, max);
}

static void sugov_update_shared(struct update_util_data *hook, u64 time,
				unsigned int flags)
{
	struct sugov_cpu *sg_cpu = container_of(hook, struct sugov_cpu, update_util);
	struct sugov_policy *sg_policy = sg_cpu->sg_policy;
	unsigned long util, max;
	unsigned int next_f;

	sugov_get_util(&util, &max, time);

	raw_spin_lock(&sg_policy->update_lock);

	sg_cpu->util = util;
	sg_cpu->max = max;
	sg_cpu->flags = flags;

	sugov_set_iowait_boost(sg_cpu, time, flags);
	sg_cpu->last_update = time;

	if (sugov_should_update_freq(sg_policy, time)) {
		if (flags & SCHED_CPUFREQ_DL)
			next_f = sg_policy->policy->cpuinfo.max_freq;
		else
			next_f = sugov_next_freq_shared(sg_cpu, time);

		sugov_update_commit(sg_policy, time, next_f);
	}

	raw_spin_unlock(&sg_policy->update_lock);
}

static void sugov_work(struct work_struct *work)
{
	struct sugov_policy *sg_policy = container_of(work, struct sugov_policy, work);

	mutex_lock(&sg_policy->work_lock);
	__cpufreq_driver_target(sg_policy->policy, sg_policy->next_freq,
				CPUFREQ_RELATION_L);
	mutex_unlock(&sg_policy->work_lock);

	sg_policy->work_in_progress = false;
}

static void sugov_irq_work(struct irq_work *irq_work)
{
	struct sugov_policy *sg_policy;

	sg_policy = container_of(irq_work, struct sugov_policy, irq_work);
	schedule_work_on(smp_processor_id(), &sg_policy->work);
}

/************************ Governor externals ***********************/
static void update_min_rate_limit_us(struct sugov_policy *sg_policy);
void sugov_update_rate_limit_us(struct cpufreq_policy *policy,
			int up_rate_limit_ms, int down_rate_limit_ms)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;

	sg_policy = policy->governor_data;
	if (!sg_policy)
		return;

	tunables = sg_policy->tunables;
	if (!tunables)
		return;

	tunables->up_rate_limit_us = (unsigned int)(up_rate_limit_ms * USEC_PER_MSEC);
	tunables->down_rate_limit_us = (unsigned int)(down_rate_limit_ms * USEC_PER_MSEC);

	sg_policy->up_rate_delay_ns = up_rate_limit_ms * NSEC_PER_MSEC;
	sg_policy->down_rate_delay_ns = down_rate_limit_ms * NSEC_PER_MSEC;

	update_min_rate_limit_us(sg_policy);
}

int sugov_sysfs_add_attr(struct cpufreq_policy *policy, const struct attribute *attr)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;

	sg_policy = policy->governor_data;
	if (!sg_policy)
		return -ENODEV;

	tunables = sg_policy->tunables;
	if (!tunables)
		return -ENODEV;

	return sysfs_create_file(&tunables->attr_set.kobj, attr);
}

struct cpufreq_policy *sugov_get_attr_policy(struct gov_attr_set *attr_set)
{
	struct sugov_policy *sg_policy = list_first_entry(&attr_set->policy_list,
						typeof(*sg_policy), tunables_hook);
	return sg_policy->policy;
}

/************************** sysfs interface ************************/

static struct sugov_tunables *global_tunables;
static DEFINE_MUTEX(global_tunables_lock);

static inline struct sugov_tunables *to_sugov_tunables(struct gov_attr_set *attr_set)
{
	return container_of(attr_set, struct sugov_tunables, attr_set);
}

static DEFINE_MUTEX(min_rate_lock);

static void update_min_rate_limit_us(struct sugov_policy *sg_policy)
{
	mutex_lock(&min_rate_lock);
	sg_policy->min_rate_limit_ns = min(sg_policy->up_rate_delay_ns,
					   sg_policy->down_rate_delay_ns);
	mutex_unlock(&min_rate_lock);
}

static ssize_t up_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->up_rate_limit_us);
}

static ssize_t down_rate_limit_us_show(struct gov_attr_set *attr_set, char *buf)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);

	return sprintf(buf, "%u\n", tunables->down_rate_limit_us);
}

static ssize_t up_rate_limit_us_store(struct gov_attr_set *attr_set,
				      const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->up_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->up_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}

	return count;
}

static ssize_t down_rate_limit_us_store(struct gov_attr_set *attr_set,
					const char *buf, size_t count)
{
	struct sugov_tunables *tunables = to_sugov_tunables(attr_set);
	struct sugov_policy *sg_policy;
	unsigned int rate_limit_us;

	if (kstrtouint(buf, 10, &rate_limit_us))
		return -EINVAL;

	tunables->down_rate_limit_us = rate_limit_us;

	list_for_each_entry(sg_policy, &attr_set->policy_list, tunables_hook) {
		sg_policy->down_rate_delay_ns = rate_limit_us * NSEC_PER_USEC;
		update_min_rate_limit_us(sg_policy);
	}

	return count;
}

static struct governor_attr up_rate_limit_us = __ATTR_RW(up_rate_limit_us);
static struct governor_attr down_rate_limit_us = __ATTR_RW(down_rate_limit_us);

static struct attribute *sugov_attributes[] = {
	&up_rate_limit_us.attr,
	&down_rate_limit_us.attr,
	NULL
};

static struct kobj_type sugov_tunables_ktype = {
	.default_attrs = sugov_attributes,
	.sysfs_ops = &governor_sysfs_ops,
};

/********************** cpufreq governor interface *********************/

static struct cpufreq_governor schedutil_gov;

static struct sugov_policy *sugov_policy_alloc(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;

	sg_policy = kzalloc(sizeof(*sg_policy), GFP_KERNEL);
	if (!sg_policy)
		return NULL;

	sg_policy->policy = policy;
	init_irq_work(&sg_policy->irq_work, sugov_irq_work);
	INIT_WORK(&sg_policy->work, sugov_work);
	mutex_init(&sg_policy->work_lock);
	raw_spin_lock_init(&sg_policy->update_lock);
	return sg_policy;
}

static void sugov_policy_free(struct sugov_policy *sg_policy)
{
	mutex_destroy(&sg_policy->work_lock);
	kfree(sg_policy);
}

static struct sugov_tunables *sugov_tunables_alloc(struct sugov_policy *sg_policy)
{
	struct sugov_tunables *tunables;

	tunables = kzalloc(sizeof(*tunables), GFP_KERNEL);
	if (tunables) {
		gov_attr_set_init(&tunables->attr_set, &sg_policy->tunables_hook);
		if (!have_governor_per_policy())
			global_tunables = tunables;
	}
	return tunables;
}

static void sugov_tunables_free(struct sugov_tunables *tunables)
{
	if (!have_governor_per_policy())
		global_tunables = NULL;

	kfree(tunables);
}

static int sugov_init(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy;
	struct sugov_tunables *tunables;
	int ret = 0;

	/* State should be equivalent to EXIT */
	if (policy->governor_data)
		return -EBUSY;

	cpufreq_enable_fast_switch(policy);

	sg_policy = sugov_policy_alloc(policy);
	if (!sg_policy) {
		ret = -ENOMEM;
		goto disable_fast_switch;
	}

	mutex_lock(&global_tunables_lock);

	if (global_tunables) {
		if (WARN_ON(have_governor_per_policy())) {
			ret = -EINVAL;
			goto free_sg_policy;
		}
		policy->governor_data = sg_policy;
		sg_policy->tunables = global_tunables;

		gov_attr_set_get(&global_tunables->attr_set, &sg_policy->tunables_hook);
		goto out;
	}

	tunables = sugov_tunables_alloc(sg_policy);
	if (!tunables) {
		ret = -ENOMEM;
		goto free_sg_policy;
	}

	if (policy->up_transition_delay_us && policy->down_transition_delay_us) {
		tunables->up_rate_limit_us = policy->up_transition_delay_us;
		tunables->down_rate_limit_us = policy->down_transition_delay_us;
	} else {
		unsigned int lat;

                tunables->up_rate_limit_us = UP_LATENCY_MULTIPLIER;
                tunables->down_rate_limit_us = DOWN_LATENCY_MULTIPLIER;
		lat = policy->cpuinfo.transition_latency / NSEC_PER_USEC;
		if (lat) {
                        tunables->up_rate_limit_us *= lat;
                        tunables->down_rate_limit_us *= lat;
                }
	}

	policy->governor_data = sg_policy;
	sg_policy->tunables = tunables;

	ret = kobject_init_and_add(&tunables->attr_set.kobj, &sugov_tunables_ktype,
				   get_governor_parent_kobj(policy), "%s",
				   schedutil_gov.name);
	if (ret)
		goto fail;

out:
	mutex_unlock(&global_tunables_lock);
	return 0;

fail:
	policy->governor_data = NULL;
	sugov_tunables_free(tunables);

 free_sg_policy:
	mutex_unlock(&global_tunables_lock);

	sugov_policy_free(sg_policy);

disable_fast_switch:
	cpufreq_disable_fast_switch(policy);

	pr_err("initialization failed (error %d)\n", ret);
	return ret;
}

static void sugov_exit(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	struct sugov_tunables *tunables = sg_policy->tunables;
	unsigned int count;

	mutex_lock(&global_tunables_lock);

	count = gov_attr_set_put(&tunables->attr_set, &sg_policy->tunables_hook);
	policy->governor_data = NULL;
	if (!count)
		sugov_tunables_free(tunables);

	sugov_policy_free(sg_policy);
	mutex_unlock(&global_tunables_lock);
}

#ifdef CONFIG_EXYNOS_HOTPLUG_GOVERNOR
int sugov_fast_start(struct cpufreq_policy *policy, unsigned int cpu)
{
	struct sugov_policy *sg_policy;
	struct sugov_cpu *sg_cpu;

	down_write(&policy->rwsem);
	cpumask_set_cpu(cpu, policy->cpus);

	sg_policy = policy->governor_data;
	sg_cpu = &per_cpu(sugov_cpu, cpu);

	memset(sg_cpu, 0, sizeof(*sg_cpu));
	sg_cpu->sg_policy = sg_policy;
	sg_cpu->util = 0;
	sg_cpu->max = 0;
	sg_cpu->flags = 0;
	sg_cpu->last_update = 0;
	sg_cpu->iowait_boost = 0;
	sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq;
	cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
				     sugov_update_shared);

	up_write(&policy->rwsem);

	return 1;
}
#else
int sugov_fast_start(struct cpufreq_policy *policy, unsigned int cpu)
{
	return 0;
}
#endif

static int sugov_start(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	sg_policy->up_rate_delay_ns =
		sg_policy->tunables->up_rate_limit_us * NSEC_PER_USEC;
	sg_policy->down_rate_delay_ns =
		sg_policy->tunables->down_rate_limit_us * NSEC_PER_USEC;
	update_min_rate_limit_us(sg_policy);
	sg_policy->last_freq_update_time = 0;
	sg_policy->next_freq = UINT_MAX;
	sg_policy->work_in_progress = false;
	sg_policy->need_freq_update = false;
	sg_policy->cached_raw_freq = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);

		memset(sg_cpu, 0, sizeof(*sg_cpu));
		sg_cpu->sg_policy = sg_policy;
		sg_cpu->flags = 0;
		sugov_start_slack(cpu);
		sg_cpu->iowait_boost_max = policy->cpuinfo.max_freq;
		cpufreq_add_update_util_hook(cpu, &sg_cpu->update_util,
					     policy_is_shared(policy) ?
							sugov_update_shared :
							sugov_update_single);
	}

	return 0;
}

static void sugov_stop(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;
	unsigned int cpu;

	for_each_cpu(cpu, policy->cpus) {
		sugov_stop_slack(cpu);
		cpufreq_remove_update_util_hook(cpu);
	}

	synchronize_sched();

	irq_work_sync(&sg_policy->irq_work);
	cancel_work_sync(&sg_policy->work);
}

static void sugov_limits(struct cpufreq_policy *policy)
{
	struct sugov_policy *sg_policy = policy->governor_data;

	mutex_lock(&global_tunables_lock);

	if (!sg_policy) {
		mutex_unlock(&global_tunables_lock);
		return;
	}

	if (!policy->fast_switch_enabled) {
		mutex_lock(&sg_policy->work_lock);
		cpufreq_policy_apply_limits(policy);
		mutex_unlock(&sg_policy->work_lock);
	}

	sugov_update_min(policy);

	sg_policy->need_freq_update = true;

	mutex_unlock(&global_tunables_lock);
}

static struct cpufreq_governor schedutil_gov = {
	.name = "schedutil",
	.owner = THIS_MODULE,
	.init = sugov_init,
	.exit = sugov_exit,
	.start = sugov_start,
	.stop = sugov_stop,
	.limits = sugov_limits,
};

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SCHEDUTIL
struct cpufreq_governor *cpufreq_default_governor(void)
{
	return &schedutil_gov;
}
#endif
static void sugov_update_min(struct cpufreq_policy *policy)
{
	int cpu, max_cap;
	struct sugov_exynos *sg_exynos;
	int min_cap;

	max_cap = arch_scale_cpu_capacity(NULL, policy->cpu);

	/* min_cap is minimum value making higher frequency than policy->min */
	min_cap = max_cap * policy->min / policy->max;
	min_cap = (min_cap * 4 / 5) + 1;

	for_each_cpu(cpu, policy->cpus) {
		sg_exynos = &per_cpu(sugov_exynos, cpu);
		sg_exynos->min = min_cap;
	}
}

static void sugov_nop_timer(unsigned long data)
{
	/*
	 * The purpose of slack-timer is to wake up the CPU from IDLE, in order
	 * to decrease its frequency if it is not set to minimum already.
	 *
	 * This is important for platforms where CPU with higher frequencies
	 * consume higher power even at IDLE.
	 */
	trace_sugov_slack_func(smp_processor_id());
}

static void sugov_start_slack(int cpu)
{
	struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

	if (!sg_exynos->enabled)
		return;

	sg_exynos->min = ULONG_MAX;
	sg_exynos->started = true;
}

static void sugov_stop_slack(int cpu)
{
	struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

	sg_exynos->started = false;
	if (timer_pending(&sg_exynos->timer))
		del_timer_sync(&sg_exynos->timer);
}

static s64 get_next_event_time_ms(void)
{
	return ktime_to_us(tick_nohz_get_sleep_length());
}

static int sugov_need_slack_timer(unsigned int cpu)
{
	struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
	struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

	if (schedtune_cpu_boost(cpu))
		return 0;

	if (sg_cpu->util > sg_exynos->min &&
		get_next_event_time_ms() > sg_exynos->expired_time)
		return 1;

	return 0;
}

static int sugov_pm_notifier(struct notifier_block *self,
						unsigned long action, void *v)
{
	unsigned int cpu = raw_smp_processor_id();
	struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);
	struct sugov_cpu *sg_cpu = &per_cpu(sugov_cpu, cpu);
	struct timer_list *timer = &sg_exynos->timer;

	if (!sg_exynos->started)
		return NOTIFY_OK;

	switch (action) {
	case CPU_PM_ENTER_PREPARE:
		if (timer_pending(timer))
			del_timer_sync(timer);

		if (sugov_need_slack_timer(cpu)) {
			timer->expires = jiffies + msecs_to_jiffies(sg_exynos->expired_time);
			add_timer_on(timer, cpu);
			trace_sugov_slack(cpu, sg_cpu->util, sg_exynos->min, action, 1);
		}
		break;

	case CPU_PM_ENTER:
		if (timer_pending(timer) && !sugov_need_slack_timer(cpu)) {
			del_timer_sync(timer);
			trace_sugov_slack(cpu, sg_cpu->util, sg_exynos->min, action, -1);
		}
		break;

	case CPU_PM_EXIT_POST:
		if (timer_pending(timer) && (time_after(timer->expires, jiffies))) {
			del_timer_sync(timer);
			trace_sugov_slack(cpu, sg_cpu->util, sg_exynos->min, action, -1);
		}
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block sugov_pm_nb = {
	.notifier_call = sugov_pm_notifier,
};

static int find_cpu_pm_qos_class(int pm_qos_class)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

		if ((sg_exynos->qos_min_class == pm_qos_class) &&
				cpumask_test_cpu(cpu, cpu_active_mask))
			return cpu;
	}

	pr_err("cannot find cpu of PM QoS class\n");
	return -EINVAL;
}

static int sugov_pm_qos_callback(struct notifier_block *nb,
					unsigned long val, void *v)
{
	struct sugov_cpu *sg_cpu;
	struct cpufreq_policy *policy;
	int pm_qos_class = *((int *)v);
	unsigned int next_freq;
	int cpu;

	cpu = find_cpu_pm_qos_class(pm_qos_class);
	if (cpu < 0)
		return NOTIFY_BAD;

	sg_cpu = &per_cpu(sugov_cpu, cpu);
	if (!sg_cpu || !sg_cpu->sg_policy || !sg_cpu->sg_policy->policy)
		return NOTIFY_BAD;

	next_freq = sg_cpu->sg_policy->next_freq;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return NOTIFY_BAD;

	if (val >= policy->cur) {
		cpufreq_cpu_put(policy);
		return NOTIFY_BAD;
	}

	__cpufreq_driver_target(policy, next_freq, CPUFREQ_RELATION_L);

	cpufreq_cpu_put(policy);

	return NOTIFY_OK;
}

static struct notifier_block sugov_min_qos_notifier = {
	.notifier_call = sugov_pm_qos_callback,
	.priority = INT_MIN,
};

static int __init sugov_parse_dt(struct device_node *dn, int cpu)
{
	struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

	/* parsing slack info */
	if (of_property_read_u32(dn, "enabled", &sg_exynos->enabled))
		return -EINVAL;
	if (sg_exynos->enabled)
		if (of_property_read_u32(dn, "expired_time", &sg_exynos->expired_time))
			sg_exynos->expired_time = DEFAULT_EXPIRED_TIME;

	/* parsing pm_qos_class info */
	if (of_property_read_u32(dn, "qos_min_class", &sg_exynos->qos_min_class))
		return -EINVAL;

	return 0;
}

static void __init sugov_exynos_init(void)
{
	int cpu, ret;
	struct device_node *dn = NULL;
	const char *buf;

	while ((dn = of_find_node_by_type(dn, "schedutil-domain"))) {
		struct cpumask shared_mask;
		/* Get shared cpus */
		ret = of_property_read_string(dn, "shared-cpus", &buf);
		if (ret)
			goto exit;

		cpulist_parse(buf, &shared_mask);
		for_each_cpu(cpu, &shared_mask)
			if (sugov_parse_dt(dn, cpu))
				goto exit;
	}

	for_each_possible_cpu(cpu) {
		struct sugov_exynos *sg_exynos = &per_cpu(sugov_exynos, cpu);

		if (!sg_exynos->enabled)
			continue;

		/* Initialize slack-timer */
		init_timer_pinned(&sg_exynos->timer);
		sg_exynos->timer.function = sugov_nop_timer;
	}

	pm_qos_add_notifier(PM_QOS_CLUSTER0_FREQ_MIN, &sugov_min_qos_notifier);
	pm_qos_add_notifier(PM_QOS_CLUSTER1_FREQ_MIN, &sugov_min_qos_notifier);
	cpu_pm_register_notifier(&sugov_pm_nb);

	return;
exit:
	pr_info("%s: failed to initialized slack_timer, pm_qos handler\n", __func__);
}

static int __init sugov_register(void)
{
	sugov_exynos_init();

	return cpufreq_register_governor(&schedutil_gov);
}
fs_initcall(sugov_register);
