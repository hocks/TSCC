/*
 * task.c - manage tasks for other clients, not myself
 *
 * $Id: task.c 418 2008-03-16 21:15:35Z pw $
 *
 * Copyright (C) 2005-8 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "mpiexec.h"

/* global variable so anybody can walk it */
static LIST_HEAD(tids_list);
struct list_head *tids = &tids_list;

tids_t *
tid_add(int tid, int client, int task)
{
    tids_t *tp;

    tp = Malloc(sizeof(*tp));
    tp->tid = tid;
    tp->client = client;
    tp->task = task;
    tp->status = 0;
    list_add_tail(&tp->list, tids);
    return tp;
}

/*
 * Lookup the tid entry for a given client's task number.
 */
extern tids_t *
tid_find(int client, int task)
{
    tids_t *tp;

    list_for_each_entry(tp, tids, list)
	if (tp->client == client && tp->task == task)
	    return tp;
    return NULL;
}

/*
 * Forget about a tid and delete all events associated with it.
 */
void
tid_del(tids_t *tp)
{
    evts_t *ep;

    list_for_each_entry(ep, evts, list)
	if (ep->client == tp->client && ep->task == tp->task)
	    ep->dead = 1;
    list_del(&tp->list);
    free(tp);
}

/*
 * Debugging
 */
void
tid_dump(void)
{
    tids_t *tp;

    printf("%s\n", __func__);
    list_for_each_entry(tp, tids, list)
	printf("tid %d client %d task %d status %d\n", tp->tid, tp->client,
	  tp->task, tp->status);
}

/*
 * For debugging printfs, find the node name for a given tm_node_id.
 */
const char *
node_name_from_nid(tm_node_id nid)
{
    int i, j;

    for (i=0; i<numnodes; i++)
	for (j=0; j<nodes[i].numcpu; j++)
	    if (nodes[i].ids[j] == nid)
		return nodes[i].name;
    error("%s: no such node id %d", __func__, nid);
    return NULL;  /* not reached */
}

/*
 * Get rid of this task, but keep around any events it might generate.
 * Return the new event number that will report when the kill is completed.
 */
tm_event_t
kill_tid(tids_t *tp)
{
    tm_event_t evt = -1;
    int ret;

    debug(2, "%s: kill client %d task %d", __func__, tp->client, tp->task);
    ret = tm_kill(tp->tid, SIGKILL, &evt);
    if (ret == TM_SUCCESS)
	evt_add(evt, tp->client, tp->task, EVT_KILL);
    else if (ret == TM_ENOTFOUND) {
	debug(2, "%s: delete already dead client %d task %d",
	  __func__, tp->client, tp->task);
	tid_del(tp);
    } else
	error_tm(ret, "%s: tm_kill client %d task %d", __func__,
	  tp->client, tp->task);
    return evt;
}

/* set once to true if somebody's early exit caused all the others to
 * be killed */
int have_killed = 0;

/*
 * Use tm to send a signal to all tasks.
 */
void
kill_tasks(int signum)
{
    int i;

    debug(1, "%s: killing all tasks", __func__);
    for (i=0; i<numtasks; i++) {
	/* only try to tasks that are running */
	if (tasks[i].done != DONE_NOT)
	    continue;
	if (concurrent_master) {
	    tm_event_t evt;
	    int ret;
	    debug(2, "%s: kill my task %d on %s", __func__,
	      i, nodes[tasks[i].node].name);
	    ret = tm_kill(tasks[i].tid, SIGKILL, &evt);
	    if (ret == TM_SUCCESS)
		evt_add(evt, -1, i, EVT_KILL);
	    else if (ret == TM_ENOTFOUND) {
		debug(2, "%s: tried to kill my already dead task %d",
		  __func__, i);
		/* but no tid to delete, and don't mark done until obit */
	    } else
		error_tm(ret, "%s: tm_kill my task %d", __func__, i);
	} else {
	    concurrent_request_kill(i, signum);
	}
    }
    have_killed = 1;
}

/*
 * Wait for tasks to finish, if any exit with non-zero status, perhaps
 * kill the rest.  Also, if concurrent_master, pay attention to other
 * mpiexec requests.
 */
void
wait_tasks(void)
{
    int last_numspawned = numspawned+1;
    int done;
    evts_t *ep;

    /*
     * Wait for all tasks to die, and all events to dry up.
     */
    for (;;) {
	done = 1;
	if (numspawned)
	    done = 0;
	else {
	    /* see if any events left, if so, try to drain them */
	    list_for_each_entry(ep, evts, list) {
		if (ep->client == -1) {  /* self, any task */
		    done = 0;
		    break;
		}
	    }
	}
	if (done)
	    break;

	if (cl_args->verbose) {
	    if (numspawned > 0 && last_numspawned != numspawned) {
		int i, chars, more;
		const int width = 80;
		const int reserve = 18;
		last_numspawned = numspawned;
		fprintf(stderr, "%s: %s: waiting for", progname, __func__);
		chars = 32;
		more = 0;
		for (i=0; i<numtasks; i++)
		    if (tasks[i].done == DONE_NOT) {
			int len = strlen(nodes[tasks[i].node].name) + 1;
			if (more || chars + len > width - reserve) {
			    ++more;
			} else {
			    fprintf(stderr, " %s", nodes[tasks[i].node].name);
			    chars += len;
			}
		    }
		if (more)
		    fprintf(stderr, " and %d others", more);
		fprintf(stderr, ".\n");
	    }
	    if (numspawned == 0)
		debug(2, "%s: tasks done, but waiting on events", __func__);
	}

	/* look for and handle an event */
	if (concurrent_master) {
	    for (;;) {
		ep = poll_event();
		if (!ep)
		    break;
		dispatch_event(ep);
	    }
	    cm_check_clients();  /* includes timeout */
	} else {
	    ep = block_event();
	    if (ep)
		dispatch_event(ep);
	}
    }
}

