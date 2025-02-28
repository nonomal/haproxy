#define _GNU_SOURCE

#include <dirent.h>
#include <sched.h>
#include <string.h>

#include <haproxy/api.h>
#include <haproxy/cfgparse.h>
#include <haproxy/cpuset.h>
#include <haproxy/cpu_topo.h>
#include <haproxy/global.h>
#include <haproxy/tools.h>

/* for cpu_set.flags below */
#define CPU_SET_FL_NONE       0x0000
#define CPU_SET_FL_DO_RESET   0x0001

/* CPU topology information, ha_cpuset_size() entries, allocated at boot */
struct ha_cpu_topo *ha_cpu_topo = NULL;
struct cpu_map *cpu_map;

/* "cpu-set" global configuration */
struct cpu_set_cfg {
	uint flags; // CPU_SET_FL_XXX above
	/* CPU numbers to accept / reject */
	struct hap_cpuset only_cpus;
	struct hap_cpuset drop_cpus;
	/* node numbers to accept / reject */
	struct hap_cpuset only_nodes;
	struct hap_cpuset drop_nodes;
	/* cluster numbers to accept / reject */
	struct hap_cpuset only_clusters;
	struct hap_cpuset drop_clusters;
	/* core numbers to accept / reject */
	struct hap_cpuset only_cores;
	struct hap_cpuset drop_cores;
	/* thread numbers to accept / reject */
	struct hap_cpuset only_threads;
	struct hap_cpuset drop_threads;
} cpu_set_cfg;

/* list of CPU selection strategies for "cpu-selection". The default one
 * is the first one.
 */
static struct ha_cpu_selection ha_cpu_selection[] = {
	[0] = { .name = "balanced",    .desc = "Use biggest CPUs grouped by locality first",    _cmp_cpu_balanced    },
	[1] = { .name = "performance", .desc = "Optimize for maximized CPU performance",        _cmp_cpu_performance },
	[2] = { .name = "low-latency", .desc = "Optimize for minimized CPU latency",            _cmp_cpu_low_latency },
	[3] = { .name = "locality",    .desc = "Arrange by locality only",                      _cmp_cpu_locality    },
	[4] = { .name = "resource",    .desc = "Lowest resource usage",                         _cmp_cpu_resource    },
	[5] = { .name = "all",         .desc = "Use all available CPUs in the system's order",  _cmp_cpu_index       },
};


/* Detects CPUs that are online on the system. It may rely on FS access (e.g.
 * /sys on Linux). Returns the number of CPUs detected or 0 if the detection
 * failed.
 */
int ha_cpuset_detect_online(struct hap_cpuset *set)
{
#if defined(__linux__)

	ha_cpuset_zero(set);

	/* contains a list of CPUs in the format <low>[-<high>][,...] */
	if (read_line_to_trash("%s/cpu/online", NUMA_DETECT_SYSTEM_SYSFS_PATH) >= 0) {
		const char *parse_cpu_set_args[2] = { trash.area, "\0" };

		if (parse_cpu_set(parse_cpu_set_args, set, NULL) != 0)
			ha_cpuset_zero(set);
	}

#elif defined(__FreeBSD__)

	struct hap_cpuset node_cpu_set;
	int ndomains, domain;
	size_t len = sizeof(ndomains);

	ha_cpuset_zero(set);

	/* retrieve the union of NUMA nodes as online CPUs */
	if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == 0) {
		BUG_ON(ndomains > MAXMEMDOM);

		for (domain = 0; domain < ndomains; domain++) {
			ha_cpuset_zero(&node_cpu_set);

			if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_DOMAIN, domain,
					       sizeof(node_cpu_set.cpuset), &node_cpu_set.cpuset) == -1)
				continue;

			ha_cpuset_or(set, &node_cpu_set);
		}
	}

#else // !__linux__, !__FreeBSD__

	ha_cpuset_zero(set);

#endif
	return ha_cpuset_count(set);
}

/* Detects the CPUs that will be used based on the ones the process is bound to
 * at boot. The principle is the following: all CPUs from the boot cpuset will
 * be used since we don't know upfront how individual threads will be mapped to
 * groups and CPUs.
 *
 * Returns non-zero on success, zero on failure. Note that it may not be
 * performed in the function above because some calls may rely on other items
 * being allocated (e.g. trash).
 */
int cpu_detect_usable(void)
{
	struct hap_cpuset boot_set = { };
	int maxcpus = ha_cpuset_size();
	int cpu;

	if (!(cpu_set_cfg.flags & CPU_SET_FL_DO_RESET)) {
		/* update the list with the CPUs currently bound to the current process */
		ha_cpuset_detect_bound(&boot_set);

		/* remove the known-excluded CPUs */
		for (cpu = 0; cpu < maxcpus; cpu++)
			if (!ha_cpuset_isset(&boot_set, cpu))
				ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}

	/* remove CPUs in the drop-cpu set or not in the only-cpu set */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if ( ha_cpuset_isset(&cpu_set_cfg.drop_cpus, cpu) ||
		    !ha_cpuset_isset(&cpu_set_cfg.only_cpus, cpu))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}

	/* Update the list of currently offline CPUs. Normally it's a subset
	 * of the unbound ones, but we cannot infer anything if we don't have
	 * the info so we only update what we know.
	 */
	if (ha_cpuset_detect_online(&boot_set)) {
		for (cpu = 0; cpu < maxcpus; cpu++) {
			if (!ha_cpuset_isset(&boot_set, cpu))
				ha_cpu_topo[cpu].st |= HA_CPU_F_OFFLINE;
		}
	}

	return 0;
}

/* Detects CPUs that are bound to the current process. Returns the number of
 * CPUs detected or 0 if the detection failed.
 */
int ha_cpuset_detect_bound(struct hap_cpuset *set)
{
	ha_cpuset_zero(set);

	/* detect bound CPUs depending on the OS's API */
	if (0
#if defined(__linux__)
	    || sched_getaffinity(0, sizeof(set->cpuset), &set->cpuset) != 0
#elif defined(__FreeBSD__)
	    || cpuset_getaffinity(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, sizeof(set->cpuset), &set->cpuset) != 0
#else
	    || 1 // unhandled platform
#endif
	    ) {
		/* detection failed */
		return 0;
	}

	return ha_cpuset_count(set);
}

/* Returns true if at least one cpu-map directive was configured, otherwise
 * false.
 */
int cpu_map_configured(void)
{
	int grp, thr;

	for (grp = 0; grp < MAX_TGROUPS; grp++) {
		for (thr = 0; thr < MAX_THREADS_PER_GROUP; thr++)
			if (ha_cpuset_count(&cpu_map[grp].thread[thr]))
				return 1;
	}
	return 0;
}

/* Dump the CPU topology <topo> for up to <maxcpus> for debugging purposes.
 * Offline CPUs are skipped.
 */
void cpu_dump_topology(const struct ha_cpu_topo *topo, int maxcpus)
{
	int lastcpu = 0;
	int has_smt = 0;
	int cpu, lvl;

	for (cpu = 0; cpu < maxcpus; cpu++) {
		if (!(ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE))
			lastcpu = cpu;
		if (ha_cpu_topo[cpu].th_cnt > 1)
			has_smt = 1;
	}

	for (cpu = 0; cpu <= lastcpu; cpu++) {
		if (ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE)
			continue;

		printf("[%s] thr=%3d -> cpu=%3d pk=%02d no=%02d cl=%03d(%03d)",
		       (ha_cpu_topo[cpu].st & HA_CPU_F_EXCLUDED) ? "----" : "keep",
		       cpu, ha_cpu_topo[cpu].idx,
		       ha_cpu_topo[cpu].pk_id,
		       ha_cpu_topo[cpu].no_id,
		       ha_cpu_topo[cpu].cl_gid,
		       ha_cpu_topo[cpu].cl_lid);

		/* list only relevant cache levels */
		for (lvl = 4; lvl >= 0; lvl--) {
			if (ha_cpu_topo[cpu].ca_id[lvl] < 0)
				continue;
			printf(lvl < 3 ? " l%d=%02d" : " l%d=%03d", lvl, ha_cpu_topo[cpu].ca_id[lvl]);
		}

		printf(" ts=%03d capa=%d",
		       ha_cpu_topo[cpu].ts_id,
		       ha_cpu_topo[cpu].capa);

		if (has_smt) {
			if (ha_cpu_topo[cpu].th_cnt > 1)
				printf(" smt=%d/%d",
				       ha_cpu_topo[cpu].th_id,
				       ha_cpu_topo[cpu].th_cnt);
			else
				printf(" smt=%d",
				       ha_cpu_topo[cpu].th_cnt);
		}
		putchar('\n');
	}
}


/* function used by qsort to re-arrange CPUs by index only, to restore original
 * ordering.
 */
int _cmp_cpu_index(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent, should not happend) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by vicinity
 * only. -1 says a<b, 1 says a>b. The goal is to arrange the closest CPUs
 * together, preferring locality over performance in order to keep latency
 * as low as possible, so that when picking a fixed number of threads, the
 * closest ones are used in priority. It's also used to help arranging groups
 * at the end.
 */
int _cmp_cpu_locality(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by vicinity
 * then capacity. -1 says a<b, 1 says a>b. The goal is to detect different
 * CPU capacities among clusters.
 */
int _cmp_cpu_cluster_capa(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* Same cluster. For CPU capacity, we tolerate a +/- 5% margin however
	 * so that if some values come from measurement we don't end up
	 * reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * first, then by vicinity. -1 says a<b, 1 says a>b. The goal is to use the
 * biggest CPUs and memory channels first before using the smallest ones, so
 * that when picking a fixed number of threads, the best ones are used in
 * priority. It's almost a reversal of the low-latency one that tries to avoid
 * as much as possible to share resources (noisy neighbors).
 */
int _cmp_cpu_performance(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, sibling ID: by keeping SMT threads apart, we can arrange to
	 * favor the maximum number of cores for a small thread count.
	 */
	if (l->th_id >= 0 && l->th_id < r->th_id)
		return -1;
	if (l->th_id > r->th_id && r->th_id >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by vicinity
 * and capacity -1 says a<b, 1 says a>b. The goal is to arrange the closest
 * CPUs together, preferring locality over performance in order to keep latency
 * as low as possible, so that when picking a fixed number of threads, the
 * closest ones are used in priority.
 */
int _cmp_cpu_low_latency(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * and vicinity. -1 says a<b, 1 says a>b. The goal is to use the biggest CPUs
 * from the first CCDs first before using the ones from the second node, and
 * finally the smallest ones, so that when picking a fixed number of threads,
 * the best ones are used in priority, and from the same node if possible.
 */
int _cmp_cpu_balanced(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Higher is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return -1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones.
	 */
	if (l->th_cnt > r->th_cnt)
		return -1;
	if (l->th_cnt < r->th_cnt)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, sibling ID: by keeping SMT threads apart, we can arrange to
	 * favor the maximum number of cores for a small thread count.
	 */
	if (l->th_id >= 0 && l->th_id < r->th_id)
		return -1;
	if (l->th_id > r->th_id && r->th_id >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by capacity
 * and vicinity. -1 says a<b, 1 says a>b. The goal is to use the smallest
 * number of CPUs and the cheapest ones from the first CCDs first before using
 * the ones from the second node, so that when picking a fixed number of
 * threads, the lowest costs are applied.
 */
int _cmp_cpu_resource(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, CPU capacity, used by big.little arm/arm64. Lower is better.
	 * We tolerate a +/- 5% margin however so that if some values come from
	 * measurement we don't end up reorganizing everything.
	 */
	if (l->capa > 0 && (int)l->capa * 19 > (int)r->capa * 20)
		return 1;
	if (r->capa > 0 && (int)l->capa * 20 < (int)r->capa * 19)
		return  -1;

	/* next, CPU SMT, generally useful when capacity is not known: cores
	 * supporting SMT are usually bigger than the other ones, so prefer
	 * the ones without.
	 */
	if (l->th_cnt > r->th_cnt)
		return 1;
	if (l->th_cnt < r->th_cnt)
		return  -1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L4 */
	if (l->ca_id[4] >= 0 && l->ca_id[4] < r->ca_id[4])
		return -1;
	if (l->ca_id[4] > r->ca_id[4] && r->ca_id[4] >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, thread set */
	if (l->ts_id >= 0 && l->ts_id < r->ts_id)
		return -1;
	if (l->ts_id > r->ts_id && r->ts_id >= 0)
		return  1;

	/* next, L1 */
	if (l->ca_id[1] >= 0 && l->ca_id[1] < r->ca_id[1])
		return -1;
	if (l->ca_id[1] > r->ca_id[1] && r->ca_id[1] >= 0)
		return  1;

	/* next, L0 */
	if (l->ca_id[0] >= 0 && l->ca_id[0] < r->ca_id[0])
		return -1;
	if (l->ca_id[0] > r->ca_id[0] && r->ca_id[0] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* function used by qsort to compare two hwcpus and arrange them by cluster to
 * make sure no cluster crosses L3 boundaries. -1 says a<b, 1 says a>b. It's
 * only used during topology detection.
 */
int _cmp_cpu_cluster(const void *a, const void *b)
{
	const struct ha_cpu_topo *l = (const struct ha_cpu_topo *)a;
	const struct ha_cpu_topo *r = (const struct ha_cpu_topo *)b;

	/* first, online vs offline */
	if (!(l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return -1;

	if (!(r->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)) && (l->st & (HA_CPU_F_OFFLINE | HA_CPU_F_EXCLUDED)))
		return 1;

	/* next, cluster */
	if (l->cl_gid >= 0 && l->cl_gid < r->cl_gid)
		return -1;
	if (l->cl_gid > r->cl_gid && r->cl_gid >= 0)
		return  1;

	/* next, package ID */
	if (l->pk_id >= 0 && l->pk_id < r->pk_id)
		return -1;
	if (l->pk_id > r->pk_id && r->pk_id >= 0)
		return  1;

	/* next, node ID */
	if (l->no_id >= 0 && l->no_id < r->no_id)
		return -1;
	if (l->no_id > r->no_id && r->no_id >= 0)
		return  1;

	/* next, L3 */
	if (l->ca_id[3] >= 0 && l->ca_id[3] < r->ca_id[3])
		return -1;
	if (l->ca_id[3] > r->ca_id[3] && r->ca_id[3] >= 0)
		return  1;

	/* if no L3, then L2 */
	if (l->ca_id[2] >= 0 && l->ca_id[2] < r->ca_id[2])
		return -1;
	if (l->ca_id[2] > r->ca_id[2] && r->ca_id[2] >= 0)
		return  1;

	/* next, IDX, so that SMT ordering is preserved */
	if (l->idx >= 0 && l->idx < r->idx)
		return -1;
	if (l->idx > r->idx && r->idx >= 0)
		return  1;

	/* exactly the same (e.g. absent) */
	return 0;
}

/* re-order a CPU topology array by CPU index only. This is mostly used before
 * listing CPUs regardless of their characteristics.
 */
void cpu_reorder_by_index(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_index);
}

/* arrange a CPU topology array optimally to consider vicinity and performance
 * so that cutting this into thread groups can be done linearly.
 */
void cpu_reorder_by_performance(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_performance);
}

/* re-order a CPU topology array by locality to help form groups. */
void cpu_reorder_by_locality(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_locality);
}

/* re-order a CPU topology array by cluster id. */
void cpu_reorder_by_cluster(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_cluster);
}

/* re-order a CPU topology array by locality and capacity to detect clusters. */
void cpu_reorder_by_cluster_capa(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), _cmp_cpu_cluster_capa);
}

/* arrange a CPU topology array according to the configured selection strategy */
void cpu_optimize_topology(struct ha_cpu_topo *topo, int entries)
{
	qsort(topo, entries, sizeof(*topo), ha_cpu_selection[global.cpu_sel].cmp_cpu);
}

/* This function is responsible for trying to fill in the missing info after
 * topology detection.
 */
void cpu_fixup_topology(void)
{
	int cpu, cpu2, lastcpu, maxcpus;
	int curr_gid, prev_gid;
	int curr_lid, prev_lid;

	maxcpus = ha_cpuset_size();

	lastcpu = 0;
	for (cpu = 0; cpu < maxcpus; cpu++)
		if (!(ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE))
			lastcpu = cpu;

	/* assign capacity if not filled, based on the number of threads on the
	 * core: in a same package, SMT-capable cores are generally those
	 * optimized for performers while non-SMT ones are generally those
	 * optimized for efficiency. We'll reflect that by assigning 100 and 50
	 * respectively to those.
	 */
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		if (ha_cpu_topo[cpu].capa < 0)
			ha_cpu_topo[cpu].capa = (ha_cpu_topo[cpu].th_cnt > 1) ? 100 : 50;
	}

	/* Now we'll sort CPUs by topology and assign cluster IDs to those that
	 * don't yet have one, based on the die/pkg/llc.
	 */
	cpu_reorder_by_locality(ha_cpu_topo, maxcpus);

	/* First, on some machines, L3 is not reported. But some also don't
	 * have L3.  However, no L3 when there are more than 2 L2 is quite
	 * unheard of, and while we don't really care about firing 2 groups for
	 * 2 L2, we'd rather avoid this if there are 8! In this case we'll add
	 * an L3 instance to fix the situation.
	 */
	prev_gid = -2; // make sure it cannot match even unassigned ones
	curr_gid = -1;
	for (cpu = cpu2 = 0; cpu <= lastcpu; cpu++) {
		if (ha_cpu_topo[cpu].ca_id[3] >= 0)
			continue;

		/* L3 not assigned, count L2 instances */
		if (!cpu ||
		    (ha_cpu_topo[cpu].pk_id != ha_cpu_topo[cpu-1].pk_id) ||
		    (ha_cpu_topo[cpu].no_id != ha_cpu_topo[cpu-1].no_id) ||
		    (ha_cpu_topo[cpu].ca_id[4] != ha_cpu_topo[cpu-1].ca_id[4])) {
			curr_gid = 0;
			prev_gid = -2;
			cpu2 = cpu;
		}
		else if (ha_cpu_topo[cpu].ca_id[2] != prev_gid) {
			curr_gid++;
			if (curr_gid >= 2) {
				/* let's assign L3 id to zero for all those.
				 * We can go till the end since we'll just skip
				 * them on next passes above.
				 */
				for (; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpu_topo[cpu2].ca_id[3] < 0 &&
					    ha_cpu_topo[cpu2].pk_id == ha_cpu_topo[cpu].pk_id &&
					    ha_cpu_topo[cpu2].no_id == ha_cpu_topo[cpu].no_id &&
					    ha_cpu_topo[cpu2].ca_id[4] == ha_cpu_topo[cpu].ca_id[4])
						ha_cpu_topo[cpu2].ca_id[3] = 0;
				}
			}
		}
	}

	/* Second, double-check that capacity within a cluster doesn't vary by
	 * +/- 5%, otherwise it indicates different clusters (typically
	 * big.little).
	 */
	cpu_reorder_by_cluster_capa(ha_cpu_topo, maxcpus);

	prev_gid = prev_lid = -2; // make sure it cannot match even unassigned ones
	curr_gid = curr_lid = -1;
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		/* renumber clusters and assign unassigned ones at the same
		 * time. For this, we'll compare pkg/die/llc with the last
		 * CPU's and verify if we need to create a new cluster ID.
		 * Note that some platforms don't report cache. The locao value
		 * is local to the pkg+node combination so that we reset it
		 * when changing, contrary to the global one which grows.
		 */
		if (!cpu ||
		    (ha_cpu_topo[cpu].pk_id != ha_cpu_topo[cpu-1].pk_id) ||
		    (ha_cpu_topo[cpu].no_id != ha_cpu_topo[cpu-1].no_id)) {
			curr_gid++;
			curr_lid = 0;
		}
		else if (ha_cpu_topo[cpu].cl_gid != prev_gid ||
			 ha_cpu_topo[cpu].ca_id[4] != ha_cpu_topo[cpu-1].ca_id[4] ||
			 (ha_cpu_topo[cpu].ca_id[4] < 0 && // no l4 ? check L3
			  ((ha_cpu_topo[cpu].ca_id[3] != ha_cpu_topo[cpu-1].ca_id[3]) ||
			   (ha_cpu_topo[cpu].ca_id[3] < 0 && // no l3 ? check L2
			    (ha_cpu_topo[cpu].ca_id[2] != ha_cpu_topo[cpu-1].ca_id[2])))) ||
			 (ha_cpu_topo[cpu].capa > 0 && ha_cpu_topo[cpu-1].capa > 0 &&
			  (ha_cpu_topo[cpu].capa * 100 < ha_cpu_topo[cpu-1].capa * 95 ||
			   ha_cpu_topo[cpu].capa * 95  > ha_cpu_topo[cpu-1].capa * 100))) {
			curr_gid++;
			curr_lid++;
		}
		prev_gid = ha_cpu_topo[cpu].cl_gid;
		prev_lid = ha_cpu_topo[cpu].cl_lid;
		ha_cpu_topo[cpu].cl_gid = curr_gid;
		ha_cpu_topo[cpu].cl_lid = curr_lid;
	}

	cpu_reorder_by_locality(ha_cpu_topo, maxcpus);

	/* let's make core numbers contiguous and per (pkg,node) as well, as
	 * holes may exist due to SMT.
	 */
	prev_lid = -2; // make sure it cannot match even unassigned ones
	curr_lid = -1;
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		/* renumber clusters and assign unassigne ones at the same
		 * time. For this, we'll compare pkg/die/llc with the last
		 * CPU's and verify if we need to create a new cluster ID.
		 * Note that some platforms don't report cache. The value is
		 * local to the pkg+node combination so that we reset it when
		 * changing.
		 */
		if (!cpu ||
		    (ha_cpu_topo[cpu].pk_id != ha_cpu_topo[cpu-1].pk_id) ||
		    (ha_cpu_topo[cpu].no_id != ha_cpu_topo[cpu-1].no_id)) {
			curr_lid = 0;
		}
		else if (ha_cpu_topo[cpu].ts_id != prev_lid ||
			 ha_cpu_topo[cpu].ca_id[4] != ha_cpu_topo[cpu-1].ca_id[4] ||
			 (ha_cpu_topo[cpu].ca_id[4] < 0 && // no l4 ? check L3
			  ((ha_cpu_topo[cpu].ca_id[3] != ha_cpu_topo[cpu-1].ca_id[3]) ||
			   (ha_cpu_topo[cpu].ca_id[3] < 0 && // no l3 ? check L2
			    (ha_cpu_topo[cpu].ca_id[2] != ha_cpu_topo[cpu-1].ca_id[2]))))) {
			curr_lid++;
		}
		prev_lid = ha_cpu_topo[cpu].ts_id;
		ha_cpu_topo[cpu].ts_id = curr_lid;
	}

	cpu_reorder_by_index(ha_cpu_topo, maxcpus);
}

/* apply remaining topology-based cpu set restrictions */
void cpu_refine_cpusets(void)
{
	int cpu, maxcpus;

	maxcpus = ha_cpuset_size();

	/* remove CPUs in the drop-node set or not in the only-node set */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if ( ha_cpuset_isset(&cpu_set_cfg.drop_nodes, ha_cpu_topo[cpu].no_id) ||
		    !ha_cpuset_isset(&cpu_set_cfg.only_nodes, ha_cpu_topo[cpu].no_id))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}

	/* remove CPUs in the drop-cluster set or not in the only-cluster set */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if ( ha_cpuset_isset(&cpu_set_cfg.drop_clusters, ha_cpu_topo[cpu].cl_lid) ||
		    !ha_cpuset_isset(&cpu_set_cfg.only_clusters, ha_cpu_topo[cpu].cl_lid))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}

	/* remove CPUs in the drop-core set or not in the only-core set */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if ( ha_cpuset_isset(&cpu_set_cfg.drop_cores, ha_cpu_topo[cpu].ts_id) ||
		    !ha_cpuset_isset(&cpu_set_cfg.only_cores, ha_cpu_topo[cpu].ts_id))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}

	/* remove CPUs in the drop-thread set or not in the only-thread set */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		if ( ha_cpuset_isset(&cpu_set_cfg.drop_threads, ha_cpu_topo[cpu].th_id) ||
		    !ha_cpuset_isset(&cpu_set_cfg.only_threads, ha_cpu_topo[cpu].th_id))
			ha_cpu_topo[cpu].st |= HA_CPU_F_EXCLUDED;
	}
}

/* CPU topology detection below, OS-specific */

#if defined(__linux__)

/* detect the CPU topology based on info in /sys */
int cpu_detect_topology(void)
{
	const char *parse_cpu_set_args[2];
	struct ha_cpu_topo cpu_id = { }; /* all zeroes */
	struct hap_cpuset node_cpu_set;
	struct dirent *de;
	int no_cache, no_topo, no_capa, no_clust, no_pkg;
	int no_cppc, no_freq;
	int maxcpus = 0;
	int lastcpu = 0;
	DIR *dir;
	int cpu;

	maxcpus = ha_cpuset_size();

	for (cpu = 0; cpu < maxcpus; cpu++)
		if (!(ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE))
			lastcpu = cpu;

	/* now let's only focus on bound CPUs to learn more about their
	 * topology, their siblings, their cache affinity etc. We can stop
	 * at lastcpu which matches the ID of the last known bound CPU
	 * when it's set. We'll pre-assign and auto-increment indexes for
	 * thread_set_id, cluster_id, l1/l2/l3 id, etc. We don't revisit entries
	 * already filled from the list provided by another CPU.
	 */

	if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu"))
		goto skip_cpu;

	/* detect the presence of some kernel-specific fields */
	no_cache = no_topo = no_capa = no_clust = no_pkg = no_freq = no_cppc = -1;
	for (cpu = 0; cpu <= lastcpu; cpu++) {
		struct hap_cpuset siblings_list = { 0 };
		struct hap_cpuset cpus_list;
		int next_level = 1; // assume L1 if unknown
		int idx, level;
		int cpu2;

		if (ha_cpu_topo[cpu].st & HA_CPU_F_OFFLINE)
			continue;

		if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d", cpu))
			continue;

		/* First, let's check the cache hierarchy. On systems exposing
		 * it, index0 generally is the L1D cache, index1 the L1I, index2
		 * the L2 and index3 the L3. But sometimes L1I/D are reversed,
		 * and some CPUs also have L0 or L4. Maybe some heterogenous
		 * SoCs even have inconsistent levels between clusters... Thus
		 * we'll scan all entries that we can find for each CPU and
		 * assign levels based on what is reported. The types generally
		 * are "Data", "Instruction", "Unified". We just ignore inst if
		 * found.
		 */
		if (no_cache < 0)
			no_cache = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cache", cpu);

		if (no_cache)
			goto skip_cache;

		for (idx = 0; idx < 10; idx++) {
			if (!is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cache/index%d", cpu, idx))
				break;

			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/type", cpu, idx) >= 0 &&
			    strcmp(trash.area, "Instruction") == 0)
				continue;

			level = next_level;
			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/level", cpu, idx) >= 0) {
				level = atoi(trash.area);
				next_level = level + 1;
			}

			if (level < 0 || level > 4)
				continue; // level out of bounds

			if (ha_cpu_topo[cpu].ca_id[level] >= 0)
				continue; // already filled

			if (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH
			                       "/cpu/cpu%d/cache/index%d/shared_cpu_list", cpu, idx) >= 0) {
				parse_cpu_set_args[0] = trash.area;
				parse_cpu_set_args[1] = "\0";
				if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
					for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
						if (ha_cpuset_isset(&cpus_list, cpu2))
							ha_cpu_topo[cpu2].ca_id[level] = cpu_id.ca_id[level];
					}
					cpu_id.ca_id[level]++;
				}
			}
		}

	skip_cache:
		if (no_topo < 0)
			no_topo = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology", cpu);

		if (no_topo)
			goto skip_topo;

		/* Now let's try to get more info about how the cores are
		 * arranged in packages, clusters, cores, threads etc. It
		 * overlaps a bit with the cache above, but as not all systems
		 * provide all of these, they're quite complementary in fact.
		 */

		/* thread siblings list will allow to figure which CPU threads
		 * share the same cores, and also to tell apart cores that
		 * support SMT from those which do not. When mixed, generally
		 * the ones with SMT are big cores and the ones without are the
		 * small ones. We also read the entry if the cluster_id is not
		 * known because we'll have to compare both values.
		 */
		if ((ha_cpu_topo[cpu].ts_id < 0 || ha_cpu_topo[cpu].cl_gid < 0) &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/thread_siblings_list", cpu) >= 0) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &siblings_list, NULL) == 0) {
				int sib_id = 0;

				cpu_id.th_cnt = ha_cpuset_count(&siblings_list);
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&siblings_list, cpu2)) {
						ha_cpu_topo[cpu2].ts_id  = cpu_id.ts_id;
						ha_cpu_topo[cpu2].th_cnt = cpu_id.th_cnt;
						ha_cpu_topo[cpu2].th_id  = sib_id++;
					}
				}
				cpu_id.ts_id++;
			}
		}

		/* clusters of cores when they exist, can be smaller and more
		 * precise than core lists (e.g. big.little), otherwise use
		 * core lists as a fall back, which may also have been used
		 * above as a fallback for package but we don't care here. We
		 * only consider these values if there's more than one CPU per
		 * cluster (some kernels such as 6.1 report one cluster per CPU).
		 * Note that we purposely ignore clusters that are reportedly
		 * equal to the siblings list, because some machines report one
		 * distinct cluster per *core* (e.g. some armv7 and intel 14900).
		 */
		if (no_clust < 0) {
			no_clust = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/cluster_cpus_list", cpu) &&
				   !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/core_siblings_list", cpu);
		}

		if (!no_clust && ha_cpu_topo[cpu].cl_gid < 0 &&
		    (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/cluster_cpus_list", cpu) >= 0 ||
		     read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/core_siblings_list", cpu) >= 0)) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0 && ha_cpuset_count(&cpus_list) > 1 &&
			    (memcmp(&cpus_list, &siblings_list, sizeof(cpus_list)) != 0)) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2)) {
						ha_cpu_topo[cpu2].cl_lid = cpu_id.cl_lid;
						ha_cpu_topo[cpu2].cl_gid = cpu_id.cl_gid;
					}
				}
				cpu_id.cl_lid++;
				cpu_id.cl_gid++;
			}
		}

		/* package CPUs list, like nodes, are generally a hard limit
		 * for groups, which must not span over multiple of them. On
		 * some systems, the package_cpus_list is not always provided,
		 * so we may first fall back to core_siblings_list which also
		 * exists, then to the physical package id from each CPU, whose
		 * number starts at 0. The first one is preferred because it
		 * provides a list in a single read().
		 */
		if (no_pkg < 0) {
			no_pkg = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/package_cpus_list", cpu) &&
				 !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/core_siblings_list", cpu);
		}

		if (!no_pkg && ha_cpu_topo[cpu].pk_id < 0 &&
		    (read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/package_cpus_list", cpu) >= 0 ||
		     read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/core_siblings_list", cpu) >= 0)) {
			parse_cpu_set_args[0] = trash.area;
			parse_cpu_set_args[1] = "\0";
			if (parse_cpu_set(parse_cpu_set_args, &cpus_list, NULL) == 0) {
				for (cpu2 = 0; cpu2 <= lastcpu; cpu2++) {
					if (ha_cpuset_isset(&cpus_list, cpu2))
						ha_cpu_topo[cpu2].pk_id = cpu_id.pk_id;
				}
				cpu_id.pk_id++;
			}
		}

		if (ha_cpu_topo[cpu].pk_id < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/topology/physical_package_id", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].pk_id = str2uic(trash.area);
		}

	skip_topo:
		if (no_capa < 0)
			no_capa = !is_file_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpu_capacity", cpu);

		/* CPU capacity is a relative notion to compare little and big
		 * cores. Usually the values encountered in field set the big
		 * CPU's nominal capacity to 1024 and the other ones below.
		 */
		if (!no_capa && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpu_capacity", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].capa = str2uic(trash.area);
		}

		/* When cpu_capacity is not available, sometimes acpi_cppc is
		 * available on servers to provide an equivalent metric allowing
		 * to distinguish big from small cores. Values as low as 15 and
		 * as high as 260 were seen there. Note that only nominal_perf
		 * is trustable, as nominal_freq may return zero. It's also
		 * more reliable than the max cpufreq values because it doesn't
		 * seem to take into account the die quality.
		 */
		if (no_cppc < 0)
			no_cppc = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/acpi_cppc", cpu);

		if (!no_cppc && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/acpi_cppc/nominal_perf", cpu) >= 0) {
			if (trash.data)
				ha_cpu_topo[cpu].capa = str2uic(trash.area);
		}

		/* Finally if none of them is available we can have a look at
		 * cpufreq's max cpu frequency.
		 */
		if (no_freq < 0)
			no_freq = !is_dir_present(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpufreq", cpu);

		if (!no_freq && ha_cpu_topo[cpu].capa < 0 &&
		    read_line_to_trash(NUMA_DETECT_SYSTEM_SYSFS_PATH "/cpu/cpu%d/cpufreq/scaling_max_freq", cpu) >= 0) {
			/* This is in kHz turn it to MHz to stay below 32k */
			if (trash.data)
				ha_cpu_topo[cpu].capa = (str2uic(trash.area) + 999U) / 1000U;
		}
	}

 skip_cpu:
	/* Now locate NUMA node IDs if any */

	dir = opendir(NUMA_DETECT_SYSTEM_SYSFS_PATH "/node");
	if (dir) {
		while ((de = readdir(dir))) {
			long node_id;
			char *endptr;

			/* dir name must start with "node" prefix */
			if (strncmp(de->d_name, "node", 4) != 0)
				continue;

			/* dir name must be at least 5 characters long */
			if (!de->d_name[4])
				continue;

			/* dir name must end with a non-negative numeric id */
			node_id = strtol(&de->d_name[4], &endptr, 10);
			if (*endptr || node_id < 0)
				continue;

			/* all tests succeeded, it's in the form "node%d" */
			if (read_line_to_trash("%s/node/%s/cpulist", NUMA_DETECT_SYSTEM_SYSFS_PATH, de->d_name) >= 0) {
				parse_cpu_set_args[0] = trash.area;
				parse_cpu_set_args[1] = "\0";
				if (parse_cpu_set(parse_cpu_set_args, &node_cpu_set, NULL) == 0) {
					for (cpu = 0; cpu < maxcpus; cpu++)
						if (ha_cpuset_isset(&node_cpu_set, cpu))
							ha_cpu_topo[cpu].no_id = node_id;
				}
			}
		}
		/* done */
		closedir(dir);
	}
	return 1;
}

#elif defined(__FreeBSD__)

int cpu_detect_topology(void)
{
	struct hap_cpuset node_cpu_set;
	int maxcpus = ha_cpuset_size();
	int ndomains, domain, cpu;
	size_t len = sizeof(ndomains);

	/* Try to detect NUMA nodes */
	if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == 0) {
		BUG_ON(ndomains > MAXMEMDOM);

		/* For each domain we'll reference the domain ID in the belonging
		 * CPUs.
		 */
		for (domain = 0; domain < ndomains; domain++) {
			ha_cpuset_zero(&node_cpu_set);

			if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_DOMAIN, domain,
					       sizeof(node_cpu_set.cpuset), &node_cpu_set.cpuset) == -1)
				continue;

			for (cpu = 0; cpu < maxcpus; cpu++)
				if (ha_cpuset_isset(&node_cpu_set, cpu))
					ha_cpu_topo[cpu].no_id = domain;
		}
	}
	return 1;
}

#else // !__linux__, !__FreeBSD__

int cpu_detect_topology(void)
{
	return 1;
}

#endif // OS-specific cpu_detect_topology()

/* Parse the "cpu-set" global directive, which takes action names and
 * optional values, and fills the cpu_set structure above.
 */
static int cfg_parse_cpu_set(char **args, int section_type, struct proxy *curpx,
                                   const struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	const char *cpu_set_str[2] = { "", "" };
	struct hap_cpuset tmp_cpuset = { };
	int arg;

	for (arg = 1; *args[arg]; arg++) {
		if (strcmp(args[arg], "reset") == 0) {
			/* reset the excluded CPUs first (undo "taskset") */
			cpu_set_cfg.flags |= CPU_SET_FL_DO_RESET;
		}
		else if (strcmp(args[arg], "drop-cpu") == 0 || strcmp(args[arg], "only-cpu") == 0) {
			if (!*args[arg + 1]) {
				memprintf(err, "missing CPU set");
				goto parse_err;
			}

			cpu_set_str[0] = args[arg + 1];
			if (parse_cpu_set(cpu_set_str, &tmp_cpuset, err) != 0)
				goto parse_err;

			if (*args[arg] == 'd') // cpus to drop
				ha_cpuset_or(&cpu_set_cfg.drop_cpus, &tmp_cpuset);
			else // cpus to keep
				ha_cpuset_and(&cpu_set_cfg.only_cpus, &tmp_cpuset);
			arg++;
		}
		else if (strcmp(args[arg], "drop-node") == 0 || strcmp(args[arg], "only-node") == 0) {
			if (!*args[arg + 1]) {
				memprintf(err, "missing node set");
				goto parse_err;
			}

			cpu_set_str[0] = args[arg + 1];
			if (parse_cpu_set(cpu_set_str, &tmp_cpuset, err) != 0)
				goto parse_err;

			if (*args[arg] == 'd') // nodes to drop
				ha_cpuset_or(&cpu_set_cfg.drop_nodes, &tmp_cpuset);
			else // nodes to keep
				ha_cpuset_and(&cpu_set_cfg.only_nodes, &tmp_cpuset);
			arg++;
		}
		else if (strcmp(args[arg], "drop-cluster") == 0 || strcmp(args[arg], "only-cluster") == 0) {
			if (!*args[arg + 1]) {
				memprintf(err, "missing cluster set");
				goto parse_err;
			}

			cpu_set_str[0] = args[arg + 1];
			if (parse_cpu_set(cpu_set_str, &tmp_cpuset, err) != 0)
				goto parse_err;

			if (*args[arg] == 'd') // clusters to drop
				ha_cpuset_or(&cpu_set_cfg.drop_clusters, &tmp_cpuset);
			else // clusters to keep
				ha_cpuset_and(&cpu_set_cfg.only_clusters, &tmp_cpuset);
			arg++;
		}
		else if (strcmp(args[arg], "drop-core") == 0 || strcmp(args[arg], "only-core") == 0) {
			if (!*args[arg + 1]) {
				memprintf(err, "missing core set");
				goto parse_err;
			}

			cpu_set_str[0] = args[arg + 1];
			if (parse_cpu_set(cpu_set_str, &tmp_cpuset, err) != 0)
				goto parse_err;

			if (*args[arg] == 'd') // cores to drop
				ha_cpuset_or(&cpu_set_cfg.drop_cores, &tmp_cpuset);
			else // cores to keep
				ha_cpuset_and(&cpu_set_cfg.only_cores, &tmp_cpuset);
			arg++;
		}
		else if (strcmp(args[arg], "drop-thread") == 0 || strcmp(args[arg], "only-thread") == 0) {
			if (!*args[arg + 1]) {
				memprintf(err, "missing thread set");
				goto parse_err;
			}

			cpu_set_str[0] = args[arg + 1];
			if (parse_cpu_set(cpu_set_str, &tmp_cpuset, err) != 0)
				goto parse_err;

			if (*args[arg] == 'd') // threads to drop
				ha_cpuset_or(&cpu_set_cfg.drop_threads, &tmp_cpuset);
			else // threads to keep
				ha_cpuset_and(&cpu_set_cfg.only_threads, &tmp_cpuset);
			arg++;
		}
		else {
			/* fall back with default error message */
			memprintf(err, "'%s' passed an unknown directive '%s'", args[0], args[arg]);
			goto leave_with_err;
		}
	}

	if (arg == 1) {
		memprintf(err, "'%s' requires a directive and an optional value", args[0]);
		goto leave_with_err;
	}

	/* all done */
	return 0;

 parse_err:
	/* displays args[0] and args[arg] followed by *err so as to remind the
	 * option name, the sub-directive and the reported error.
	 */
	memprintf(err, "'%s %s': %s\n.", args[0], args[arg], *err);
	goto leave;

 leave_with_err:
	/* complete with supported directives */
	memprintf(err, "%s (only 'reset', 'only-cpu', 'drop-cpu', 'only-node', 'drop-node', 'only-cluster', 'drop-cluster', 'only-core', 'drop-core', 'only-thread', 'drop-thread' supported).", *err);
 leave:
	return -1;
}

/* Parse the "cpu-selection" global directive, which takes the name of one
 * of the ha_cpu_selection[] names, and sets the associated index in
 * global.cpusel.
 */
static int cfg_parse_cpu_selection(char **args, int section_type, struct proxy *curpx,
                                   const struct proxy *defpx, const char *file, int line,
                                   char **err)
{
	int i;

	if (too_many_args(1, args, err, NULL))
		return -1;

	for (i = 0; i < sizeof(ha_cpu_selection) / sizeof(ha_cpu_selection[0]); i++) {
		if (strcmp(args[1], ha_cpu_selection[i].name) == 0) {
			global.cpu_sel = i;
			return 0;
		}
	}

	memprintf(err, "'%s' passed an unknown CPU selection strategy '%s'. Supported values are:", args[0], args[1]);
	for (i = 0; i < sizeof(ha_cpu_selection) / sizeof(ha_cpu_selection[0]); i++) {
		memprintf(err, "%s%s '%s'%s", *err,
		          (i > 0 && i == sizeof(ha_cpu_selection) / sizeof(ha_cpu_selection[0]) - 1) ? " and" : "",
		          ha_cpu_selection[i].name,
		          (i == sizeof(ha_cpu_selection) / sizeof(ha_cpu_selection[0]) - 1) ? ".\n" : ",");
	}
	return -1;
}

/* Allocates everything needed to store CPU topology at boot.
 * Returns non-zero on success, zero on failure.
 */
static int cpu_topo_alloc(void)
{
	int maxcpus = ha_cpuset_size();
	int cpu;

	/* allocate the structures used to store CPU topology info */
	cpu_map = calloc(MAX_TGROUPS, sizeof(*cpu_map));
	if (!cpu_map)
		return 0;

	/* allocate the structures used to store CPU topology info */
	ha_cpu_topo = (struct ha_cpu_topo*)malloc(maxcpus * sizeof(*ha_cpu_topo));
	if (!ha_cpu_topo)
		return 0;

	/* preset all fields to -1 except the index and the state flags which
	 * are assumed to all be bound and online unless detected otherwise.
	 */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		memset(&ha_cpu_topo[cpu], 0xff, sizeof(*ha_cpu_topo));
		ha_cpu_topo[cpu].st  = 0;
		ha_cpu_topo[cpu].idx = cpu;
	}

	/* pre-inizialize the configured CPU sets */
	ha_cpuset_zero(&cpu_set_cfg.drop_cpus);
	ha_cpuset_zero(&cpu_set_cfg.only_cpus);
	ha_cpuset_zero(&cpu_set_cfg.drop_nodes);
	ha_cpuset_zero(&cpu_set_cfg.only_nodes);
	ha_cpuset_zero(&cpu_set_cfg.drop_clusters);
	ha_cpuset_zero(&cpu_set_cfg.only_clusters);
	ha_cpuset_zero(&cpu_set_cfg.drop_cores);
	ha_cpuset_zero(&cpu_set_cfg.only_cores);
	ha_cpuset_zero(&cpu_set_cfg.drop_threads);
	ha_cpuset_zero(&cpu_set_cfg.only_threads);

	/* preset all CPUs in the "only-XXX" sets */
	for (cpu = 0; cpu < maxcpus; cpu++) {
		ha_cpuset_set(&cpu_set_cfg.only_cpus, cpu);
		ha_cpuset_set(&cpu_set_cfg.only_nodes, cpu);
		ha_cpuset_set(&cpu_set_cfg.only_clusters, cpu);
		ha_cpuset_set(&cpu_set_cfg.only_cores, cpu);
		ha_cpuset_set(&cpu_set_cfg.only_threads, cpu);
	}

	return 1;
}

static void cpu_topo_deinit(void)
{
	ha_free(&ha_cpu_topo);
	ha_free(&cpu_map);
}

INITCALL0(STG_ALLOC, cpu_topo_alloc);
REGISTER_POST_DEINIT(cpu_topo_deinit);

/* config keyword parsers */
static struct cfg_kw_list cfg_kws = {ILH, {
	{ CFG_GLOBAL, "cpu-selection",  cfg_parse_cpu_selection, 0 },
	{ CFG_GLOBAL, "cpu-set",  cfg_parse_cpu_set, 0 },
	{ 0, NULL, NULL }
}};

INITCALL1(STG_REGISTER, cfg_register_keywords, &cfg_kws);
