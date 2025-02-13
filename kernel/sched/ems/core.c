/*
 * Core Exynos Mobile Scheduler
 *
 * Copyright (C) 2018 Samsung Electronics Co., Ltd
 * Park Bumgyu <bumgyu.park@samsung.com>
 */

#include <linux/ems.h>

#include "ems.h"
#include "../sched.h"
#include "../tune.h"

#define CREATE_TRACE_POINTS
#include <trace/events/ems.h>

unsigned long task_util(struct task_struct *p)
{
	if (rt_task(p))
		return p->rt.avg.util_avg;
	else
		return p->se.avg.util_avg;
}

extern int wake_cap(struct task_struct *p, int cpu, int prev_cpu);
bool is_cpu_preemptible(struct task_struct *p, int prev_cpu, int cpu, int sync)
{
	struct rq *rq = cpu_rq(cpu);
#ifdef CONFIG_SCHED_TUNE
	struct task_struct *curr = READ_ONCE(rq->curr);

	if (curr && schedtune_prefer_perf(curr) > 0)
		return false;
#endif

	if (sync && (rq->nr_running != 1 || wake_cap(p, cpu, prev_cpu)))
		return false;

	return true;
}

static int select_proper_cpu(struct task_struct *p, int prev_cpu)
{
	int cpu;
	unsigned long best_spare_util = 0;
	unsigned long best_active_util = ULONG_MAX;
	unsigned long best_idle_util = ULONG_MAX;
	int best_idle_cstate = INT_MAX;
	int best_active_cpu = -1;
	int best_idle_cpu = -1;
	int best_cpu = -1;

	for_each_cpu(cpu, cpu_active_mask) {
		int i;

		/* visit each coregroup only once */
		if (cpu != cpumask_first(cpu_coregroup_mask(cpu)))
			continue;

		/* skip if task cannot be assigned to coregroup */
		if (!cpumask_intersects(&p->cpus_allowed, cpu_coregroup_mask(cpu)))
			continue;

		for_each_cpu_and(i, tsk_cpus_allowed(p), cpu_coregroup_mask(cpu)) {
			unsigned long capacity_orig = capacity_orig_of(i);
			unsigned long new_util, spare_util;

			new_util = ml_task_attached_cpu_util(i, p);
			new_util = max(new_util, ml_boosted_task_util(p));

			/* skip over-capacity cpu */
			if (lbt_util_bring_overutilize(i, new_util))
				continue;

			if (idle_cpu(i)) {
				int idle_idx = idle_get_state_idx(cpu_rq(i));

				/* find shallowest idle state cpu */
				if (idle_idx > best_idle_cstate)
					continue;

				/* if same cstate, select lower util */
				if (idle_idx == best_idle_cstate &&
				    (best_idle_cpu == prev_cpu ||
				    (i != prev_cpu &&
				    new_util >= best_idle_util)))
					continue;

				/* Keep track of best idle CPU */
				best_idle_cstate = idle_idx;
				best_idle_util = new_util;
				best_idle_cpu = i;
				continue;
			}

			/*
			 * Best target) lowest utilization among lowest-cap cpu
			 *
			 * If the sequence reaches this function, the wakeup task
			 * does not require performance and the prev cpu is over-
			 * utilized, so it should do load balancing without
			 * considering energy side. Therefore, it selects cpu
			 * with smallest cpapacity or highest spare capacity
			 * and the least utilization among cpus that fits the task.
			 */
			spare_util = capacity_orig - new_util;
			if (spare_util <= best_spare_util)
				continue;

			best_active_util = new_util;
			best_spare_util = spare_util;
			best_active_cpu = i;
		}

		/*
		 * if it fails to find the best cpu in this coregroup, visit next
		 * coregroup.
		 */
		if (cpu_selected(best_active_cpu) &&
		    is_cpu_preemptible(p, -1, best_active_cpu, 0)) {
			best_cpu = best_active_cpu;
			break;
		}

		if (cpu_selected(best_idle_cpu)) {
			best_cpu = best_idle_cpu;
			break;
		}
	}

	trace_ems_select_proper_cpu(p, best_cpu,
		best_cpu == best_idle_cpu ? best_idle_util : best_active_util);

	/*
	 * if it fails to find the vest cpu, choosing any cpu is meaningless.
	 * Return prev cpu.
	 */
	return cpu_selected(best_cpu) ? best_cpu : prev_cpu;
}

extern void sync_entity_load_avg(struct sched_entity *se);

static int eff_mode;

int exynos_wakeup_balance(struct task_struct *p, int prev_cpu, int sd_flag, int sync)
{
	int target_cpu = -1;
	char state[30] = "fail";

	/*
	 * Since the utilization of a task is accumulated before sleep, it updates
	 * the utilization to determine which cpu the task will be assigned to.
	 * Exclude new task.
	 */
	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	target_cpu = select_service_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "service cpu");
		goto out;
	}

	/*
	 * Priority 1 : ontime task
	 *
	 * If task which has more utilization than threshold wakes up, the task is
	 * classified as "ontime task" and assigned to performance cpu. Conversely,
	 * if heavy task that has been classified as ontime task sleeps for a long
	 * time and utilization becomes small, it is excluded from ontime task and
	 * is no longer guaranteed to operate on performance cpu.
	 *
	 * Ontime task is very sensitive to performance because it is usually the
	 * main task of application. Therefore, it has the highest priority.
	 */
	target_cpu = ontime_task_wakeup(p, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "ontime migration");
		goto out;
	}

	/*
	 * Priority 2 : prefer-perf
	 *
	 * Prefer-perf is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-perf is set to 1, the tasks in the group are
	 * preferentially assigned to the performance cpu.
	 *
	 * It has a high priority because it is a function that is turned on
	 * temporarily in scenario requiring reactivity(touch, app laucning).
	 */
	target_cpu = prefer_perf_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-perf");
		goto out;
	}

	/*
	 * Priority 3 : global boosting
	 *
	 * Global boost is a function that preferentially assigns all tasks in the
	 * system to the performance cpu. Unlike prefer-perf, which targets only
	 * group tasks, global boost targets all tasks. So, it maximizes performance
	 * cpu utilization.
	 *
	 * Typically, prefer-perf operates on groups that contains UX related tasks,
	 * such as "top-app" or "foreground", so that major tasks are likely to be
	 * assigned to performance cpu. On the other hand, global boost assigns
	 * all tasks to performance cpu, which is not as effective as perfer-perf.
	 * For this reason, global boost has a lower priority than prefer-perf.
	 */
	target_cpu = global_boosting(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "global boosting");
		goto out;
	}

	if (eff_mode) {
		target_cpu = select_best_cpu(p, prev_cpu, sd_flag, sync);
		if (cpu_selected(target_cpu)) {
			strcpy(state, "best");
			goto out;
		}
	}

	/*
	 * Priority 4 : prefer-idle
	 *
	 * Prefer-idle is a function that operates on cgroup basis managed by
	 * schedtune. When perfer-idle is set to 1, the tasks in the group are
	 * preferentially assigned to the idle cpu.
	 *
	 * Prefer-idle has a smaller performance impact than the above. Therefore
	 * it has a relatively low priority.
	 */
	target_cpu = prefer_idle_cpu(p);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "prefer-idle");
		goto out;
	}

	/*
	 * Priority 5 : energy cpu
	 *
	 * A scheduling scheme based on cpu energy, find the least power consumption
	 * cpu with energy table when assigning task.
	 */
	target_cpu = select_energy_cpu(p, prev_cpu, sd_flag, sync);
	if (cpu_selected(target_cpu)) {
		strcpy(state, "energy cpu");
		goto out;
	}

	/*
	 * Priority 6 : proper cpu
	 *
	 * If the task failed to find a cpu to assign from the above conditions,
	 * it means that assigning task to any cpu does not have performance and
	 * power benefit. In this case, select cpu for balancing cpu utilization.
	 */
	target_cpu = select_proper_cpu(p, prev_cpu);
	if (cpu_selected(target_cpu))
		strcpy(state, "proper cpu");

out:
	trace_ems_wakeup_balance(p, target_cpu, state);
	return target_cpu;
}

static ssize_t show_eff_mode(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int ret = 0;

	ret += snprintf(buf + ret, 10, "%d\n", eff_mode);

	return ret;
}

static ssize_t store_eff_mode(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf,
		size_t count)
{
	unsigned int input;

	if (!sscanf(buf, "%d", &input))
		return -EINVAL;

	eff_mode = input;

	return count;
}

static struct kobj_attribute eff_mode_attr =
__ATTR(eff_mode, 0644, show_eff_mode, store_eff_mode);

static ssize_t show_sched_topology(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	int cpu;
	struct sched_domain *sd;
	int ret = 0;

	rcu_read_lock();
	for_each_possible_cpu(cpu) {
		int sched_domain_level = 0;

		sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd);
		while (sd->parent) {
			sched_domain_level++;
			sd = sd->parent;
		}

		for_each_lower_domain(sd) {
			ret += snprintf(buf + ret, 50,
				"[lv%d] cpu%d: sd->span=%#x sg->span=%#x\n",
				sched_domain_level, cpu,
				*(unsigned int *)cpumask_bits(sched_domain_span(sd)),
				*(unsigned int *)cpumask_bits(sched_group_span(sd->groups)));
			sched_domain_level--;
		}
		ret += snprintf(buf + ret,
			50, "----------------------------------------\n");
	}
	rcu_read_unlock();

	return ret;
}

static struct kobj_attribute sched_topology_attr =
__ATTR(sched_topology, 0444, show_sched_topology, NULL);

struct kobject *ems_kobj;

static int __init init_sysfs(void)
{
	ems_kobj = kobject_create_and_add("ems", kernel_kobj);

	sysfs_create_file(ems_kobj, &sched_topology_attr.attr);
	sysfs_create_file(ems_kobj, &eff_mode_attr.attr);

	return 0;
}
core_initcall(init_sysfs);

void __init init_ems(void)
{
	init_part();
}
