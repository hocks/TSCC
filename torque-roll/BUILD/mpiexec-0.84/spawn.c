/*
 * spawn.c - handle dynamic process spawning for MPI-2.
 *
 * $Id: spawn.c 388 2006-11-27 17:09:48Z pw $
 *
 * Copyright (C) 2005-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <string.h>
#include "mpiexec.h"

/*
 * Called on parent (not stdio listener) after the arguments for the
 * spawn have been gathered.  Deallocate them when done.
 *
 * Only mpich2/pmi is known to support MPI_Spawn.  And only particular devices
 * of mpich2/pmi do.  In mpich2-1.0.3, ch3:sock works, and perhaps others.
 * OSU's mvapich2 is based on old mpich2-1.0.1 and does not support spawning.
 */
int
spawn(int nprocs, char *execname,
      int numarg, char **args,
      int numinfo, char **infokeys, char **infovals)
{
    const char *target_host;
    int i, ret = 1;
    tasks_t *newtasks;
    growstr_t *g;
    config_spec_t *cfg;
    char *cfg_exe, *cfg_args;

    debug(2, "%s: spawn %d %s", __func__, nprocs, execname);

    /*
     * Look at the info args to figure out what to do.
     */
    target_host = NULL;

    for (i=0; i<numinfo; i++) {
	if (!strcmp(infokeys[i], "host")) {
	    if (target_host)
		warning("%s: duplicate \"host\" info key ignored", __func__);
	    else
		target_host = infovals[i];
	} else {
	    warning("%s: unknown info key \"%s\" ignored", __func__,
	            infokeys[i]);
	}
    }

    /*
     * XXX: Figure out how to request more CPUs from the concurrent master.
     */
    if (!concurrent_master) {
	warning("%s: no code to handle non-concurrent_master case", __func__);
	goto outfree;
    }

    /* XXX: have this return a code and terminate the spawn; as it stands,
     * the mpiexec program exits, leaving other tasks hanging around
     */
    cfg_exe = resolve_exe(execname, 0);

    /*
     * Malloc up the new structures assuming this is going to work out.
     */
    newtasks = Malloc((numtasks + nprocs) * sizeof(*newtasks));
    memcpy(newtasks, tasks, numtasks * sizeof(*newtasks));
    memset(newtasks + numtasks, 0, nprocs * sizeof(*newtasks));
    cfg = new_config_spec();
    g = growstr_init();
    for (i=0; i<numarg; i++) {
	if (i > 0)
	    growstr_append(g, " ");
	growstr_append(g, args[i]);
    }
    cfg_args = strsave(g->s);
    growstr_free(g);

    /*
     * Command-line constraints are still in force here.  Run on the specified
     * host or find free ones.
     */
    if (target_host) {
	int target_node;

	for (i=0; i<numnodes; i++)
	    if (!strcmp(target_host, nodes[i].name))
		break;
	if (i == numnodes) {
	    warning("%s: no host \"%s\" as specified in info key", __func__,
	            target_host);
	    goto outfree_tasks;
	}
	if (nodes[i].availcpu < nprocs) {
	    warning("%s: need %d tasks on host \"%s\", only %d available",
	            __func__, nprocs, target_host, nodes[i].availcpu);
	    goto outfree_tasks;
	}
	target_node = i;

	for (i=numtasks; i<numtasks+nprocs; i++) {
	    allocate_cpu_to_task(target_node, &newtasks[i]);
	    newtasks[i].conf = cfg;
	}
    } else {
	int j;
	int avail = 0;

	for (i=0; i<numnodes; i++)
	    avail += nodes[i].availcpu;
	if (avail < nprocs) {
	    warning("%s: need %d tasks, only %d available", __func__, nprocs,
	            avail);
	    goto outfree_tasks;
	}

	j = 0;
	for (i=numtasks; i<numtasks+nprocs; i++) {
	    for (; j<numnodes; j++)
		if (nodes[j].availcpu > 0)
		    break;
	    allocate_cpu_to_task(j, &newtasks[i]);
	    newtasks[i].conf = cfg;
	}
    }

    /*
     * Accept these tasks and try to start them.
     * For now, don't tasks_shmem_reduce: only for mpich/p4 that will not
     * ever support spawn.  And don't bother with distribute_executable
     * yet either.
     */
    cfg->exe = cfg_exe;
    cfg->args = cfg_args;
    free(tasks);
    
    /*
     * Build the next spawn group.
     */
    {
	void *x = spawns;
	spawns = Malloc((numspawns + 1) * sizeof(*spawns));
	memcpy(spawns, x, numspawns * sizeof(*spawns));
	free(x);
	memset(&spawns[numspawns], 0, sizeof(*spawns));
	spawns[numspawns].task_start = numtasks;
	spawns[numspawns].task_end = numtasks + nprocs;
	spawns[numspawns].obits = Malloc(nprocs
	                                 * sizeof(*spawns[numspawns].obits));
	spawns[numspawns].ranks2hosts_response = NULL;
	for (i=numtasks; i<numtasks + nprocs; i++)
	    newtasks[i].status = &spawns[numspawns].obits[i-numtasks];
	++numspawns;
    }

    /* commit to new full tasks list */
    tasks = newtasks;
    numtasks += nprocs;

    ret = start_tasks(numspawns-1);
    goto outfree;

outfree_tasks:
    free(cfg_exe);
    free(cfg_args);
    list_del(&cfg->list);
    free(cfg);
    free(newtasks);

outfree:
    free(execname);
    for (i=0; i<numarg; i++)
	free(args[i]);
    if (numarg)
	free(args);
    for (i=0; i<numinfo; i++) {
	free(infokeys[i]);
	free(infovals[i]);
    }
    if (numinfo) {
	free(infokeys);
	free(infovals);
    }
    return ret;
}
