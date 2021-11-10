#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_hwpmc_hooks.h"
#include "opt_kdtrace.h"
#include "opt_sched.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/ktr.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/smp.h>
#include <sys/sysctl.h>
#include <sys/bus.h>
#include <sys/cpu.h>
#include <sys/eventhandler.h>

#include "sched.h"

#if __LONG_BIT > 32
#define HAVE_CMPSET64
#else
#ifdef __i386__
#define HAVE_CMPSET64
#endif
#endif

#define CPU_TIME_MAY_NOT_STABLE

int cpu_time_stable;
static int tick_ns;
static int sched_time_running;
static unsigned long cputick2nsec_scale;

#define CPUTICK2NSEC_SCALE_FACTOR	10	/* 2^10, carefully chosen */

static inline void
set_cputick2nsec_scale(unsigned long cpu_mhz)
{
	unsigned long scale = (1000 << CPUTICK2NSEC_SCALE_FACTOR) / cpu_mhz;
	
	if (scale != cputick2nsec_scale)
		cputick2nsec_scale = scale;
#if 0
	printf("SFQ: cputick2nsec_scale %ld, freq %ld mHz\n",
		cputick2nsec_scale, cpu_mhz);
#endif
}

static inline uint64_t
cputick2nsec(uint64_t cputicks)
{
	return (cputicks * cputick2nsec_scale) >> CPUTICK2NSEC_SCALE_FACTOR;
}

static void
freq_changed(void *arg, const struct cf_level *level, int status)
{
	set_cputick2nsec_scale(cpu_tickrate()/1000000);
}

/*
 * Return current cpu time in nanoseconds, XXX this function may should
 * be in arch-dependent code, so it can be implemented optimistically.
 */
static uint64_t
cputime_ns(void)
{
	return cputick2nsec(cpu_ticks());
}

#ifdef CPU_TIME_MAY_NOT_STABLE

/* 
 * If cpu tick is not stable, use a mixed way to get cpu time.
 */
struct sched_time_data {
#ifndef HAVE_CMPSET64
	struct mtx	lock;
#endif
	/* saved cputime */
	uint64_t	tick_cpu_time;
	/* saved system up-time */
	uint64_t	tick_sys_time;
	/* calcuated time */
	uint64_t	time;
};

static DPCPU_DEFINE(struct sched_time_data, sched_time_data);

static inline struct sched_time_data *
local_timedata(void)
{
	return DPCPU_PTR(sched_time_data);
}

static inline struct
sched_time_data *cpu_timedata(int cpuid)
{
	return DPCPU_ID_PTR(cpuid, sched_time_data);
}

static inline uint64_t
systime_ns(void)
{
	struct timespec	ts;

	nanouptime(&ts);
	return (timespec_to_ns(&ts));
}

void
sched_time_init(void)
{
	uint64_t sys_now;
	int cpu;
 
	sched_time_running = 1;
	tick_ns = 1000000000 / hz;
	set_cputick2nsec_scale(cpu_tickrate()/1000000);
	EVENTHANDLER_REGISTER(cpufreq_post_change,
		freq_changed, NULL, EVENTHANDLER_PRI_ANY);

	sys_now = systime_ns();
	CPU_FOREACH(cpu) {
		struct sched_time_data *scd = cpu_timedata(cpu);
 
#ifndef HAVE_CMPSET64
		mtx_init(&scd->lock, "sched time update", NULL, MTX_SPIN|MTX_RECURSED);
#endif
		scd->tick_cpu_time = 0;
		scd->tick_sys_time = sys_now;
		scd->time = sys_now;
	}
}

static uint64_t
sched_time_local(struct sched_time_data *data)
{
	uint64_t now, time, old_time, min_time, max_time;
	int64_t delta;

#ifndef HAVE_CMPSET64
	mtx_lock_spin(&data->lock);
#else
again:
#endif
	now = cputime_ns();
	delta = now - data->tick_cpu_time;
	if (delta < 0)
		delta = 0;
	old_time = data->time;
	time = data->tick_sys_time + delta;
	min_time = wrap_max_64(data->tick_sys_time, old_time);
	max_time = wrap_max_64(old_time, data->tick_sys_time + tick_ns);
	time = wrap_max_64(time, min_time);
	time = wrap_min_64(time, max_time);
#ifdef HAVE_CMPSET64
	if (!atomic_cmpset_64(&data->time, old_time, time))
		goto again;
#else
	data->time = time;
	mtx_unlock_spin(&data->lock);
#endif
	return (time);
}

static uint64_t
sched_time_remote(struct sched_time_data *remote)
{
	struct sched_time_data *local = local_timedata();
	uint64_t local_time, remote_time;
	uint64_t *ptr, old_val, val;

#ifndef HAVE_CMPSET64
	if (local < remote) {
		mtx_lock_spin(&local->lock);
		mtx_lock_spin(&remote->lock);
	} else {
		mtx_lock_spin(&remote->lock);
		mtx_lock_spin(&local->lock);
	}
#endif

	sched_time_local(local);

#ifdef HAVE_CMPSET64
again:
#endif
	local_time = local->time;
	remote_time = remote->time;

	if ((int64_t)(remote_time - local_time) < 0) {
		ptr = &remote->time;
		old_val = remote_time;
		val = local_time;
        } else {
		ptr = &local->time;
		old_val = local_time;
		val = remote_time;
	}

#ifdef HAVE_CMPSET64
	if (!atomic_cmpset_64(ptr, old_val, val))
		goto again;
#else
	*ptr = val;
	mtx_unlock_spin(&local->lock);
	mtx_unlock_spin(&remote->lock);
#endif
	return (val);
}

uint64_t
sched_time_cpu(int cpu)
{
	struct sched_time_data *data;
	uint64_t time;
 
	if (cpu_time_stable)
		return (cputime_ns());
 
	if (!sched_time_running)
		return (0ull);
        
	data = cpu_timedata(cpu);

	if (cpu != PCPU_GET(cpuid))
		time = sched_time_remote(data);
        else
		time = sched_time_local(data);
  
	return (time);
}

void
sched_time_tick(void)
{
	struct sched_time_data *data;
	uint64_t cpu_now, sys_now;

	if (cpu_time_stable)
		return;

	if (!sched_time_running)
		return;

	data = local_timedata();
	sys_now = systime_ns();
	cpu_now = cputime_ns();
	data->tick_cpu_time = cpu_now;
	data->tick_sys_time = sys_now;
	sched_time_local(data);
}

#else

void
sched_time_init(void)
{
	sched_time_running = 1;
	tick_ns = 1000000000 / hz;
	set_cputick2nsec_scale(cpu_tickrate()/1000000);
	EVENTHANDLER_REGISTER(cpufreq_post_change,
		freq_changed, NULL, EVENTHANDLER_PRI_ANY);
}

uint64_t
sched_time_cpu(int cpu)
{
	if (!sched_time_running)
		return (0);

	return (cputime_ns());
}

void
sched_time_tick(void)
{
}
#endif /* CPU_TIME_MAY_NOT_STABLE */
