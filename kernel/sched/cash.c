// SPDX-License-Identifier: GPL-2.0
/*
 *  Cache-Aware Scheduling Heuristic (CASH)
 *  Copyright (C) 2025 shygosh <shygosh@proton.me>
 */

static unsigned int sched_cash_aggro_ns __read_mostly = 5000000;
static unsigned int sched_cash_tempo_ns __read_mostly = 12000000;


static DEFINE_PER_CPU(struct sched_group *, cash_sg_ptr);
bool cash_up __read_mostly;
bool cash_sg __read_mostly;

struct cash_cpu {
	long factor;
	int cpu;
};

struct cash_group {
	long factor;
	struct sched_group *groups;
};

static struct cpumask *cash_best_group(int cpu, struct cpumask *scope)
{
	struct cash_group cand, best;
	struct sched_group *start;

	cand.groups = start = per_cpu(cash_sg_ptr, cpu);
	best.factor = -SCHED_CAPACITY_SCALE;

	do {
		if (!cpumask_intersects(sched_group_span(cand.groups), scope))
			continue;

		cand.factor = READ_ONCE(cand.groups->factor);
		if (cand.factor * 100 > best.factor * 125) {
			best.factor = cand.factor;
			best.groups = cand.groups;
		}
	} while ((cand.groups = cand.groups->next) != start);

	return sched_group_span(best.groups);
}

static struct cpumask *cash_find_group(int cpu, int wake_flags, struct cpumask *scope)
{
	if (wake_flags & WF_TTWU) {
		struct cpumask *mask = sched_group_span(per_cpu(cash_sg_ptr, cpu));

		if (likely(cpumask_intersects(mask, scope)))
			return mask;
	}

	return cash_best_group(cpu, scope);
}

static int cash_select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	if (unlikely(!READ_ONCE(cash_up)))
		return select_task_rq_fair(p, prev_cpu, wake_flags);
	struct cpumask m_group, m_scope;
	struct cash_cpu best;
	unsigned int p_est;
	int cpu, p_cpu, p_que;
	int aggro = 0, tempo = 0;

	if (unlikely(!cpumask_and(&m_scope, cpu_active_mask, p->cpus_ptr)))
		return cpumask_first(p->cpus_ptr);

	if (wake_flags & WF_TTWU) {
		u64 delta = sched_clock() - smp_load_acquire(&p->last_ts);
		int this_cpu = raw_smp_processor_id();

		record_wakee(p);

		if (unlikely((wake_flags & WF_CURRENT_CPU) &&
			     cpumask_test_cpu(this_cpu, &m_scope)))
			return this_cpu;

		if (!wake_wide(p)) {
			cpumask_or(&m_group, cpu_smt_mask(prev_cpu), cpu_smt_mask(this_cpu));
			tempo = aggro = 1;
		} else if (delta <= (u64)sched_cash_aggro_ns) {
			cpumask_copy(&m_group, cpu_smt_mask(prev_cpu));
			tempo = aggro = 1;
		} else if (delta <= (u64)sched_cash_tempo_ns) {
			tempo = 1;
		}

		if (aggro && unlikely(!cpumask_intersects(&m_group, &m_scope)))
			aggro = 0;
	}

	p_est = _task_util_est(p);
	p_cpu = task_cpu(p);
	p_que = current == p || task_on_rq_queued(p);

retry:
	if (!aggro) {
		if (READ_ONCE(cash_sg))
			cpumask_copy(&m_group, cash_find_group(prev_cpu, wake_flags, &m_scope));
		else
			cpumask_copy(&m_group, cpu_present_mask);
	}

rescan:
	best.factor = -SCHED_CAPACITY_SCALE;
	for_each_cpu_and(cpu, &m_scope, &m_group) {
		struct rq *rq = cpu_rq(cpu);
		long factor;

		if (static_branch_unlikely(&sched_asym_cpucapacity))
			factor = arch_scale_cpu_capacity(cpu);
		else
			factor = SCHED_CAPACITY_SCALE;

		factor -= (long)(READ_ONCE(rq->cfs.avg.util_est) +
				 READ_ONCE(rq->avg_rt.util_avg));

		if (p_que && cpu == p_cpu)
			factor += p_est;

		if (factor > best.factor) {
			best.cpu = cpu;
			best.factor = factor;
		}
	}

	if (aggro && (best.factor - p_est < 64L)) {
		aggro = 0;
		goto retry;
	}

	if (!READ_ONCE(cash_sg))
		return best.cpu;

	if (!aggro) {
		if (likely(cpumask_subset(&m_group, &m_scope))) {
			struct sched_group *sg = per_cpu(cash_sg_ptr, best.cpu);
			long ewma = (READ_ONCE(sg->factor) * 3 + best.factor) >> 2;
			WRITE_ONCE(sg->factor, ewma);
		}

		if (!tempo && (wake_flags & WF_TTWU)) {
			struct cpumask *new = cash_best_group(best.cpu, &m_scope);

			if (!cpumask_intersects(&m_group, new)) {
				cpumask_copy(&m_group, new);
				tempo = 1;
				goto rescan;
			}
		}
	}

	return best.cpu;
}

void sched_cash_init(void)
{
	struct sched_domain *sd, *tmp;
	struct sched_group *sg;
	int cpu = smp_processor_id();

	for_each_domain(cpu, tmp) {
		sd = tmp;
	}

	sg = sd->groups;

	do {
		cpu = cpumask_first(sched_group_span(sg));
		sg->factor = arch_scale_cpu_capacity(cpu);
		for_each_cpu(cpu, sched_group_span(sg)) {
			per_cpu(cash_sg_ptr, cpu) = sg;
		}
	} while ((sg = sg->next) != sd->groups);

	do {
		cpu = cpumask_first(sched_group_span(sg));
		if (cpumask_weight(sched_group_span(sg)) >
		    cpumask_weight(cpu_smt_mask(cpu))) {
			WRITE_ONCE(cash_sg, true);
			break;
		}
	} while ((sg = sg->next) != sd->groups);

	smp_mb();
	pr_warn("sched_cash: group: enabled=%s\n",
		READ_ONCE(cash_sg) ? "true" : "false");
	pr_warn("sched_cash: domain: weight=%u level=%d flags=0x%x\n",
		sd->span_weight, sd->level, sd->flags);
	WRITE_ONCE(cash_up, true);
}

#ifdef CONFIG_SYSCTL
static const struct ctl_table sched_cash_sysctls[] = {
	{
		.procname	= "sched_cash_tempo_ns",
		.data		= &sched_cash_tempo_ns,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= (void *)&sched_cash_aggro_ns,
	},
	{
		.procname	= "sched_cash_aggro_ns",
		.data		= &sched_cash_aggro_ns,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra2		= (void *)&sched_cash_tempo_ns,
	},
};

static int __init sched_cash_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_cash_sysctls);
	return 0;
}
late_initcall(sched_cash_sysctl_init);
#endif
