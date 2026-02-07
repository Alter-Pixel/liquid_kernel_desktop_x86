// SPDX-License-Identifier: GPL-2.0
/*
 *  Cache-Aware Scheduling Heuristic (CASH)
 *  Copyright (C) 2025 shygosh <shygosh@proton.me>
 *  Copyright (C) 2025 UsiFX <xprjkts@gmail.com>
 */

static unsigned int sched_cash_aggro_ns __read_mostly = 5000000;
static unsigned int sched_cash_tempo_ns __read_mostly = 12000000;
static unsigned int sched_cash_warm_ns  __read_mostly = 20000000;
static unsigned int sched_cash_smt_bonus __read_mostly = 128;
static unsigned int sched_cash_cluster_bonus __read_mostly = 64;


static DEFINE_PER_CPU(struct sched_group *, cash_sg_ptr);
static DEFINE_PER_CPU(struct sched_domain *, cash_cluster_sd);
bool cash_up __read_mostly;
bool cash_sg __read_mostly;
static bool cash_has_clusters __read_mostly;

struct cash_stats {
	atomic64_t	total_placements;
	atomic64_t	smt_hits;
	atomic64_t	cluster_hits;
	atomic64_t	cache_hot;
	atomic64_t	cache_warm;
	atomic64_t	cache_cold;
	atomic64_t	migrations;
};

static struct cash_stats __cacheline_aligned_in_smp cash_stats;

struct cash_cpu {
	long factor;
	int cpu;
};

struct cash_group {
	long factor;
	struct sched_group *groups;
};

/* Returns: 2 = hot, 1 = warm, 0 = cold */
static inline int cash_cache_state(struct task_struct *p, int cpu, u64 now)
{
	u64 delta;

	if (p->cash_warm_cpu != cpu || p->cash_warm_until == 0)
		return 0;

	if (now < p->cash_warm_until) {
		delta = p->cash_warm_until - now;
		if (delta > (sched_cash_warm_ns >> 1))
			return 2;
		return 1;
	}

	return 0;
}

static inline void cash_update_warmness(struct task_struct *p, int cpu)
{
	u64 now = sched_clock();

	p->cash_warm_cpu = cpu;
	p->cash_warm_until = now + sched_cash_warm_ns;
}

static inline bool cash_same_cluster(int cpu1, int cpu2)
{
	struct sched_domain *sd;

	if (!cash_has_clusters)
		return false;

	sd = per_cpu(cash_cluster_sd, cpu1);
	if (sd && cpumask_test_cpu(cpu2, sched_domain_span(sd)))
		return true;

	return false;
}

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
	int cache_state = 0;
	int warm_cpu;
	u64 now;

	if (unlikely(!cpumask_and(&m_scope, cpu_active_mask, p->cpus_ptr)))
		return cpumask_first(p->cpus_ptr);

	now = sched_clock();
	warm_cpu = READ_ONCE(p->cash_warm_cpu);

	if (wake_flags & WF_TTWU) {
		u64 delta = now - smp_load_acquire(&p->last_ts);
		int this_cpu = raw_smp_processor_id();

		record_wakee(p);

		if (warm_cpu >= 0 && cpumask_test_cpu(warm_cpu, &m_scope))
			cache_state = cash_cache_state(p, warm_cpu, now);

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

		if (cache_state > 0 && warm_cpu >= 0) {
			if (cpumask_test_cpu(cpu, cpu_smt_mask(warm_cpu)))
				factor += sched_cash_smt_bonus * cache_state;
			if (cash_has_clusters && cash_same_cluster(cpu, warm_cpu))
				factor += sched_cash_cluster_bonus * cache_state;
		}

		if (factor > best.factor) {
			best.cpu = cpu;
			best.factor = factor;
		}
	}

	if (aggro && (best.factor - p_est < 64L)) {
		aggro = 0;
		goto retry;
	}

	atomic64_inc(&cash_stats.total_placements);

	if (cache_state == 2)
		atomic64_inc(&cash_stats.cache_hot);
	else if (cache_state == 1)
		atomic64_inc(&cash_stats.cache_warm);
	else
		atomic64_inc(&cash_stats.cache_cold);

	if (warm_cpu >= 0) {
		if (cpumask_test_cpu(best.cpu, cpu_smt_mask(warm_cpu)))
			atomic64_inc(&cash_stats.smt_hits);
		if (cash_has_clusters && cash_same_cluster(best.cpu, warm_cpu))
			atomic64_inc(&cash_stats.cluster_hits);
		else if (cash_has_clusters && warm_cpu != best.cpu)
			atomic64_inc(&cash_stats.migrations);
	}

	cash_update_warmness(p, best.cpu);
	p->cash_migrations++;

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
	struct sched_domain *sd, *tmp, *cluster_sd;
	struct sched_group *sg;
	int cpu = smp_processor_id();
	int i;

	for_each_domain(cpu, tmp) {
		sd = tmp;
	}

	/* Cache cluster domain pointers for all CPUs */
	for_each_possible_cpu(i) {
		cluster_sd = NULL;
		for_each_domain(i, tmp) {
			if (tmp->flags & SD_CLUSTER) {
				cluster_sd = tmp;
				break;
			}
		}
		per_cpu(cash_cluster_sd, i) = cluster_sd;
		if (cluster_sd && i == cpu)
			WRITE_ONCE(cash_has_clusters, true);
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
	pr_info("sched_cash: enabled=%s clusters=%s\n",
		READ_ONCE(cash_sg) ? "true" : "false",
		READ_ONCE(cash_has_clusters) ? "true" : "false");
	pr_info("sched_cash: domain weight=%u level=%d flags=0x%x\n",
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
	{
		.procname	= "sched_cash_warm_ns",
		.data		= &sched_cash_warm_ns,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
	{
		.procname	= "sched_cash_smt_bonus",
		.data		= &sched_cash_smt_bonus,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
	{
		.procname	= "sched_cash_cluster_bonus",
		.data		= &sched_cash_cluster_bonus,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec,
	},
};

static int __init sched_cash_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_cash_sysctls);
	return 0;
}
late_initcall(sched_cash_sysctl_init);
#endif

#ifdef CONFIG_PROC_FS
static int cash_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "total_placements %llu\n", atomic64_read(&cash_stats.total_placements));
	seq_printf(m, "smt_hits %llu\n", atomic64_read(&cash_stats.smt_hits));
	seq_printf(m, "cluster_hits %llu\n", atomic64_read(&cash_stats.cluster_hits));
	seq_printf(m, "cache_hot %llu\n", atomic64_read(&cash_stats.cache_hot));
	seq_printf(m, "cache_warm %llu\n", atomic64_read(&cash_stats.cache_warm));
	seq_printf(m, "cache_cold %llu\n", atomic64_read(&cash_stats.cache_cold));
	seq_printf(m, "migrations %llu\n", atomic64_read(&cash_stats.migrations));
	seq_printf(m, "aggro_ns %u\n", sched_cash_aggro_ns);
	seq_printf(m, "tempo_ns %u\n", sched_cash_tempo_ns);
	seq_printf(m, "warm_ns %u\n", sched_cash_warm_ns);
	seq_printf(m, "smt_bonus %u\n", sched_cash_smt_bonus);
	seq_printf(m, "cluster_bonus %u\n", sched_cash_cluster_bonus);
	seq_printf(m, "enabled %d\n", READ_ONCE(cash_up));
	seq_printf(m, "groups %d\n", READ_ONCE(cash_sg));
	seq_printf(m, "clusters %d\n", READ_ONCE(cash_has_clusters));

	return 0;
}

static int cash_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cash_proc_show, NULL);
}

static ssize_t cash_proc_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	atomic64_set(&cash_stats.total_placements, 0);
	atomic64_set(&cash_stats.smt_hits, 0);
	atomic64_set(&cash_stats.cluster_hits, 0);
	atomic64_set(&cash_stats.cache_hot, 0);
	atomic64_set(&cash_stats.cache_warm, 0);
	atomic64_set(&cash_stats.cache_cold, 0);
	atomic64_set(&cash_stats.migrations, 0);

	return count;
}

static const struct proc_ops cash_proc_ops = {
	.proc_open	= cash_proc_open,
	.proc_read	= seq_read,
	.proc_write	= cash_proc_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init sched_cash_proc_init(void)
{
	proc_create("sched_cash_stats", 0644, NULL, &cash_proc_ops);
	return 0;
}
late_initcall(sched_cash_proc_init);
#endif
