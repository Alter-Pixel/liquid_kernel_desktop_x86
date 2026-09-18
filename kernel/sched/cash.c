// SPDX-License-Identifier: GPL-2.0
/*
 *  Cache-Aware Scheduling Heuristic (CASH)
 *  Copyright (C) 2025 shygosh <shygosh@proton.me>
 *  Copyright (C) 2025 UsiFX <xprjkts@gmail.com>
 */
#include <linux/cpuhotplug.h>
#include <linux/topology.h>
#include <linux/sched/topology.h>

#ifndef SD_SHARE_LLC
#ifdef SD_SHARE_PKG_RESOURCES
#define SD_SHARE_LLC SD_SHARE_PKG_RESOURCES
#else
#define SD_SHARE_LLC 0
#endif
#endif

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
	u64	total_placements;
	u64	smt_hits;
	u64	cluster_hits;
	u64	cache_hot;
	u64	cache_warm;
	u64	cache_cold;
	u64	migrations;
};

static DEFINE_PER_CPU(struct cash_stats, cash_stats);

/* Sum per-CPU stats into *out. Only used from slow paths (proc reads). */
static void cash_stats_sum(struct cash_stats *out)
{
	int cpu;

	memset(out, 0, sizeof(*out));
	for_each_possible_cpu(cpu)
	{
		struct cash_stats *s = per_cpu_ptr(&cash_stats, cpu);

		out->total_placements += s->total_placements;
		out->smt_hits += s->smt_hits;
		out->cluster_hits += s->cluster_hits;
		out->cache_hot += s->cache_hot;
		out->cache_warm += s->cache_warm;
		out->cache_cold += s->cache_cold;
		out->migrations += s->migrations;
	}
}

/*
 * Per-path instrumentation for the WF_SYNC branch in
 * cash_select_task_rq_fair(). cash_account()/cash_stats alone can't
 * distinguish "returned prev_cpu because it was idle" from "returned
 * this_cpu because it was contended" - both look identical in the
 * aggregate stats (chosen_cpu == warm_cpu in both cases). These counters
 * make that visible.
 */
enum cash_wfsync_path {
	CASH_WFSYNC_PREV_IDLE = 0,
	CASH_WFSYNC_THIS_BUSY,
	CASH_WFSYNC_DEFER_CFS,
	CASH_WFSYNC_PATH_MAX,
};

static DEFINE_PER_CPU(unsigned long[CASH_WFSYNC_PATH_MAX], cash_wfsync_hits);

static inline void cash_wfsync_hit(enum cash_wfsync_path path)
{
	this_cpu_inc(cash_wfsync_hits[path]);
}

static void cash_wfsync_sum(unsigned long out[CASH_WFSYNC_PATH_MAX])
{
	int cpu, i;

	memset(out, 0, sizeof(unsigned long) * CASH_WFSYNC_PATH_MAX);
	for_each_possible_cpu(cpu) {
		unsigned long *hits = per_cpu(cash_wfsync_hits, cpu);

		for (i = 0; i < CASH_WFSYNC_PATH_MAX; i++)
			out[i] += hits[i];
	}
}

/* Single point of truth for cash_stats accounting. */
static inline void cash_account(int cache_state, int chosen_cpu, int prev_cpu,
			       int warm_cpu, bool multi_cluster)
{
	struct cash_stats *s = this_cpu_ptr(&cash_stats);

	s->total_placements++;

	if (cache_state == 2)
		s->cache_hot++;
	else if (cache_state == 1)
		s->cache_warm++;
	else
		s->cache_cold++;

	if (warm_cpu >= 0)
	{
		if (cpumask_test_cpu(chosen_cpu, cpu_smt_mask(warm_cpu)))
			s->smt_hits++;
		if (multi_cluster && cash_same_cluster(chosen_cpu, warm_cpu))
			s->cluster_hits++;
	}

	if (chosen_cpu != prev_cpu)
		s->migrations++;
}

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
	if (cpu1 < 0 || cpu1 >= nr_cpu_ids || cpu2 < 0 || cpu2 >= nr_cpu_ids)
		return false;

	/* Query native LLC/CCX topology mask directly */
	return cpumask_test_cpu(cpu2, topology_cluster_cpumask(cpu1)) ||
	       cpumask_test_cpu(cpu2, cpu_coregroup_mask(cpu1));
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

static int cash_cpu_online(unsigned int cpu)
{
	struct sched_domain *sd;
	struct sched_group *sg;
	bool found_cluster = false;
	unsigned int smt_weight = cpumask_weight(cpu_smt_mask(cpu));

	rcu_read_lock();
	for_each_domain(cpu, sd) {
		/* Match x86 LLC/MC domains as well as ARM SD_CLUSTER */
		if (sd->flags & (SD_CLUSTER | SD_SHARE_LLC))
		{
			per_cpu(cash_cluster_sd, cpu) = sd;
			found_cluster = true;

			if (sd->groups) {
				sg = sd->groups;
				per_cpu(cash_sg_ptr, cpu) = sg;
				sg->factor = arch_scale_cpu_capacity(cpu);

				if (!READ_ONCE(cash_sg) && per_cpu(cash_sg_ptr, cpu))
				{
					if (cpumask_weight(sched_group_span(per_cpu(cash_sg_ptr, cpu))) > smt_weight)
						WRITE_ONCE(cash_sg, true);
				}
			}
			break;
		}
	}
	rcu_read_unlock();

	if (found_cluster || cpumask_weight(topology_cluster_cpumask(cpu)) > 1)
		WRITE_ONCE(cash_has_clusters, true);

	pr_info("sched_cash: CPU %d online | cluster=%d | sg_weight=%u\n", cpu, READ_ONCE(cash_has_clusters), per_cpu(cash_sg_ptr, cpu) ? cpumask_weight(sched_group_span(per_cpu(cash_sg_ptr, cpu))) : 0);

	return 0;
}

static int cash_cpu_offline(unsigned int cpu)
{
	per_cpu(cash_sg_ptr, cpu) = NULL;
	per_cpu(cash_cluster_sd, cpu) = NULL;
	pr_info("sched_cash: CPU %d offline and structures cleared\n", cpu);
	return 0;
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

	cash_account(cache_state, best.cpu, prev_cpu, warm_cpu, cash_has_clusters);

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
	int cpu;
	int ret;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "sched/cash:online", cash_cpu_online, cash_cpu_offline);
	if (ret < 0)
	{
		pr_err("sched_cash: failed to register CPU hotplug state\n");
		return;
	}

	WRITE_ONCE(cash_up, true);
	pr_info("sched_cash: initialized via cpuhp\n");
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
	struct cash_stats snap;

	unsigned long wfsync[CASH_WFSYNC_PATH_MAX];

	cash_stats_sum(&snap);
	cash_wfsync_sum(wfsync);

	seq_printf(m, "total_placements %llu\n", snap.total_placements);
	seq_printf(m, "smt_hits %llu\n", snap.smt_hits);
	seq_printf(m, "cluster_hits %llu\n", snap.cluster_hits);
	seq_printf(m, "cache_hot %llu\n", snap.cache_hot);
	seq_printf(m, "cache_warm %llu\n", snap.cache_warm);
	seq_printf(m, "cache_cold %llu\n", snap.cache_cold);
	seq_printf(m, "migrations %llu\n", snap.migrations);
	seq_printf(m, "aggro_ns %u\n", sched_cash_aggro_ns);
	seq_printf(m, "tempo_ns %u\n", sched_cash_tempo_ns);
	seq_printf(m, "warm_ns %u\n", sched_cash_warm_ns);
	seq_printf(m, "smt_bonus %u\n", sched_cash_smt_bonus);
	seq_printf(m, "cluster_bonus %u\n", sched_cash_cluster_bonus);
	seq_printf(m, "enabled %d\n", READ_ONCE(cash_up));
	seq_printf(m, "groups %d\n", READ_ONCE(cash_sg));
	seq_printf(m, "clusters %d\n", READ_ONCE(cash_has_clusters));
	seq_printf(m, "wfsync_prev_idle %lu\n", wfsync[CASH_WFSYNC_PREV_IDLE]);
	seq_printf(m, "wfsync_this_busy %lu\n", wfsync[CASH_WFSYNC_THIS_BUSY]);
	seq_printf(m, "wfsync_defer_cfs %lu\n", wfsync[CASH_WFSYNC_DEFER_CFS]);

	return 0;
}

static int cash_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, cash_proc_show, NULL);
}

static ssize_t cash_proc_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	int cpu;

	for_each_possible_cpu(cpu)
		memset(per_cpu_ptr(&cash_stats, cpu), 0, sizeof(struct cash_stats));

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
