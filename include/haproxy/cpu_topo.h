#ifndef _HAPROXY_CPU_TOPO_H
#define _HAPROXY_CPU_TOPO_H

#include <haproxy/api.h>
#include <haproxy/cpuset-t.h>
#include <haproxy/cpu_topo-t.h>

extern struct ha_cpu_topo *ha_cpu_topo;

/* Detects the CPUs that will be used based on the ones the process is bound to.
 * Returns non-zero on success, zero on failure. Note that it may not be
 * performed in the function above because some calls may rely on other items
 * being allocated (e.g. trash).
 */
int cpu_detect_usable(void);

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

#endif /* _HAPROXY_CPU_TOPO_H */
