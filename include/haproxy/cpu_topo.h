#ifndef _HAPROXY_CPU_TOPO_H
#define _HAPROXY_CPU_TOPO_H

#include <haproxy/api.h>
#include <haproxy/cpuset-t.h>
#include <haproxy/cpu_topo-t.h>

extern struct ha_cpu_topo *ha_cpu_topo;

/* Detects CPUs that are online on the system. It may rely on FS access (e.g.
 * /sys on Linux). Returns the number of CPUs detected or 0 if the detection
 * failed.
 */
int ha_cpuset_detect_online(struct hap_cpuset *set);

/* Detects the CPUs that will be used based on the ones the process is bound to.
 * Returns non-zero on success, zero on failure. Note that it may not be
 * performed in the function above because some calls may rely on other items
 * being allocated (e.g. trash).
 */
int cpu_detect_usable(void);

/* detect the CPU topology based on info in /sys */
int cpu_detect_topology(void);

/* fix missing info in the CPU topology */
void cpu_fixup_topology(void);

/* apply remaining topology-based cpu set restrictions */
void cpu_refine_cpusets(void);

/* Detects CPUs that are bound to the current process. Returns the number of
 * CPUs detected or 0 if the detection failed.
 */
int ha_cpuset_detect_bound(struct hap_cpuset *set);

/* Returns true if at least one cpu-map directive was configured, otherwise
 * false.
 */
int cpu_map_configured(void);

/* Dump the CPU topology <topo> for up to <maxcpus> for debugging purposes.
 * Offline CPUs are skipped.
 */
void cpu_dump_topology(const struct ha_cpu_topo *topo, int maxcpus);

/* re-order a CPU topology array by locality to help form groups. */
void cpu_reorder_by_locality(struct ha_cpu_topo *topo, int entries);

/* re-order a CPU topology array by CPU index only, to undo the function above,
 * in case other calls need to be made on top of this.
 */
void cpu_reorder_by_index(struct ha_cpu_topo *topo, int entries);

/* arrange a CPU topology array optimally to consider vicinity and performance
 * so that cutting this into thread groups can be done linearly.
 */
void cpu_reorder_by_performance(struct ha_cpu_topo *topo, int entries);

/* re-order a CPU topology array by cluster id. */
void cpu_reorder_by_cluster(struct ha_cpu_topo *topo, int entries);

/* Functions used by qsort to compare hardware CPUs (not meant to be used from
 * outside cpu_topo).
 */
int _cmp_cpu_index(const void *a, const void *b);
int _cmp_cpu_locality(const void *a, const void *b);
int _cmp_cpu_performance(const void *a, const void *b);
int _cmp_cpu_low_latency(const void *a, const void *b);
int _cmp_cpu_balanced(const void *a, const void *b);
int _cmp_cpu_resource(const void *a, const void *b);
int _cmp_cpu_cluster(const void *a, const void *b);

#endif /* _HAPROXY_CPU_TOPO_H */
