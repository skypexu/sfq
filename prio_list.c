#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include "sched.h"

#if 0
#include <stddef.h>
TAILQ_HEAD(tdprioq, td_sched);
struct td_sched {
	struct tdprioq qhead;
	TAILQ_ENTRY(td_sched) qlink;
	TAILQ_ENTRY(td_sched) llink;
};

struct thread {
	int td_priority;
	struct td_sched *td_sched;
	struct td_sched td_sched_data;
};

struct prio_list {
	TAILQ_HEAD(,td_sched) head;
};

struct thread *ts_to_td(struct td_sched *ts)
{
	return __containerof(ts, struct thread, td_sched_data);
}
#endif

void
prio_list_init(struct prio_list *list)
{
	TAILQ_INIT(&list->head);
}

struct thread *
prio_list_first(struct prio_list *list)
{
	struct td_sched *ts;

	ts = TAILQ_FIRST(&list->head);
	if (ts != NULL)
		return (ts_to_td(ts));
	return (NULL);
}

int
prio_list_empty(struct prio_list *list)
{
	return (TAILQ_EMPTY(&list->head));
}

void
prio_list_add(struct prio_list *list, struct thread *td)
{
	struct thread *td2;
	struct td_sched *ts, *ts2;

	ts = td->td_sched;
	ts->prio = td->td_priority;
	TAILQ_INIT(&ts->qhead);
	TAILQ_FOREACH(ts2, &list->head, llink) {
		td2 = ts_to_td(ts2);
		if (ts2->prio == ts->prio) {
			TAILQ_INSERT_TAIL(&ts2->qhead, ts, qlink);
			return;
		} else if (ts2->prio > ts->prio) {
			TAILQ_INSERT_BEFORE(ts2, ts, llink);
			goto out;
		}
	}
	TAILQ_INSERT_TAIL(&list->head, ts, llink);
out:
	TAILQ_INSERT_HEAD(&ts->qhead, ts, qlink);
}

void
prio_list_del(struct prio_list *list, struct thread *td)
{
	struct td_sched *head, *next, *ts;

	ts = td->td_sched;
	head = ts;
	while (TAILQ_PREV(head, tdprioq, qlink) != NULL)
		head = TAILQ_PREV(head, tdprioq, qlink);
	TAILQ_REMOVE(&head->qhead, ts, qlink);
	if (TAILQ_EMPTY(&head->qhead)) {
		KASSERT(head == ts, ("queue corrupted"));
		TAILQ_REMOVE(&list->head, head, llink);
	} else if (ts == head) {
		next = TAILQ_FIRST(&head->qhead);
		TAILQ_SWAP(&head->qhead, &next->qhead, td_sched, qlink);
		TAILQ_INSERT_BEFORE(head, next, llink);
		TAILQ_REMOVE(&list->head, head, llink);
	}
}

#if 0
int
main(int argc, char **argv)
{
	struct thread td1, td2, td3, td4;
	struct prio_list list;

	prio_list_init(&list);
	td1.td_sched = &td1.td_sched_data;
	td2.td_sched = &td2.td_sched_data;
	td3.td_sched = &td3.td_sched_data;
	td4.td_sched = &td4.td_sched_data;
	td1.td_priority = 1;
	td2.td_priority = 3;
	td3.td_priority = 1;
	td4.td_priority = 2;

	prio_list_add(&list, &td1);
	prio_list_add(&list, &td2);
	prio_list_add(&list, &td3);
	prio_list_add(&list, &td4);

	prio_list_del(&list, &td1);
	prio_list_del(&list, &td2);
	prio_list_del(&list, &td3);
	prio_list_add(&list, &td4);
	return (0);
}
#endif
