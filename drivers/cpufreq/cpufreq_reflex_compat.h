/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Reflex CPUFreq governor: the only kernel-version differences.
 * cpufreq_reflex.c is byte-identical across versions; this file is not.
 *
 * Linux 7.2: cpufreq_driver_adjust_perf() takes the policy but no
 * max_perf.
 */
#ifndef _CPUFREQ_REFLEX_COMPAT_H
#define _CPUFREQ_REFLEX_COMPAT_H

static inline void rfx_adjust_perf(struct cpufreq_policy *policy,
				   unsigned long min_perf,
				   unsigned long target_perf,
				   unsigned long max_perf,
				   unsigned long capacity)
{
	cpufreq_driver_adjust_perf(policy, min_perf, target_perf, capacity);
}

#endif /* _CPUFREQ_REFLEX_COMPAT_H */
