/*-
 * Copyright (c) 2013, David Xu <davidxu@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

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
#include <sys/rwlock.h>
#include <sys/smp.h>
#include <sys/sysctl.h>

#include "sched.h"

#ifdef SFQ_GROUP_SCHED
LIST_HEAD(sghashhead, sched_group);
static struct sghashhead *sghashtbl;
static u_long sghashsize;
static struct  rwlock sghash_lock;
static struct mtx sg_id_lock;
static struct unrhdr *sg_id_unrhdr;

#define SGHASH(id)    (&sghashtbl[(id) & sghashsize])

void
sched_group_hash_init(void)
{
	mtx_init(&sg_id_lock, "sched_group id lock", NULL, MTX_DEF);
	rw_init(&sghash_lock, "sched_group lock");
	sghashtbl = hashinit(128, M_SCHED, &sghashsize);
	sg_id_unrhdr = new_unrhdr(1, INT_MAX, &sg_id_lock);
}

struct sched_group *
sched_group_find(int id)
{
	struct sched_group *sg;

	rw_rlock(&sghash_lock);
	LIST_FOREACH(sg, SGHASH(id), hash_link) {
		if (sg->id == id)
			break;
	}
	rw_runlock(&sghash_lock);
	return (sg);
}

void
sched_group_hash_add(struct sched_group *sg)
{
	rw_wlock(&sghash_lock);
	LIST_INSERT_HEAD(SGHASH(sg->id), sg, hash_link);
	rw_wunlock(&sghash_lock);
}

void
sched_group_hash_remove(struct sched_group *sg)
{
	rw_wlock(&sghash_lock);
	LIST_REMOVE(sg, hash_link);
	rw_wunlock(&sghash_lock);
}
#endif
