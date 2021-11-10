#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_hwpmc_hooks.h"
#include "opt_kdtrace.h"
#include "opt_sched.h"

#ifdef SMP
#error "SMP is not supported yet"
#endif

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/limits.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/sysctl.h>

#include "sched.h"

#define CPUPRI_INVALID		(-1)

/*
 * Convert thread priority to cpu priority.
 */
static inline int
convert_pri(int pri)
{
	if (pri == IDLE_PRI)
		return (0);
	if (pri == INVALID_PRI)
		return (CPUPRI_INVALID);
	if (pri > MAX_RT_PRI)
		return (1);
	return (pri - MAX_RT_PRI) + 2;
}

int
cpupri_find(struct cpupri *cp, struct thread *td,
	cpuset_t *result)
{
	int i, pri = convert_pri(td->td_priority);
	struct cpupri_vec *vec;

	MPASS(pri < CPUPRI_NR);

	for (i = 0; i < pri; ++i) {
		vec = &cp->pri_to_cpu[i];

		if (atomic_load_acq_int(&vec->count) == 0)
			continue;

		if (!CPU_OVERLAP(&td->td_cpuset->cs_mask, &vec->mask))
			continue;

		if (result != NULL) {
			CPU_COPY(&vec->mask, result);
			CPU_AND(result, &td->td_cpuset->cs_mask);
			if (CPU_FFS(result) == 0)
				continue;
		}

		return (1);
	}

	return (0);
}

void
cpupri_set(struct cpupri *cp, int cpu, int newpri)
{
	int *cpu_slot = &cp->cpu_to_pri[cpu];
	int oldpri = *cpu_slot;
	struct cpupri_vec *vec;
	int do_mb = 0;

	newpri = convert_pri(newpri);

	MPASS(newpri < CPUPRI_NR);

	if (newpri != CPUPRI_INVALID) {
		vec = &cp->pri_to_cpu[newpri];
		CPU_SET_ATOMIC(cpu, &vec->mask);
		SMP_MB();
		atomic_add_int(&vec->count, 1);
		do_mb = 1;
	}

	if (oldpri != CPUPRI_INVALID) {
		if (do_mb)
			SMP_MB();
		vec = &cp->pri_to_cpu[oldpri];
		atomic_subtract_int(&vec->count, 1);
		SMP_MB();
		CPU_CLR_ATOMIC(cpu, &vec->mask);
	}

	*cpu_slot = newpri;
}

int
cpupri_init(struct cpupri *cp)
{
	int i;

	bzero(cp, sizeof(*cp));
	CPU_FOREACH(i) {
		cp->cpu_to_pri[i] = CPUPRI_INVALID;
	}
	return (0);
}
