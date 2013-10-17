/*
 * config.c - read config files
 *
 * $Id: config.c 418 2008-03-16 21:15:35Z pw $
 *
 * Copyright (C) 2000-8 Pete Wyckoff <pw@osc.edu>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <fnmatch.h>
#include "mpiexec.h"

/*
 * This handy gnu extension avoids case in the glob check below, but
 * probably no one will write a node config file using a case which
 * does not agree with the PBS node names.
 */
#ifndef FNM_CASEFOLD
#  define FNM_CASEFOLD 0
#endif

static LIST_HEAD(config_spec_list);

/*
 * Attach another to the linked list.
 */
config_spec_t *
new_config_spec(void)
{
    config_spec_t *cfg;
    
    cfg = Malloc(sizeof(*cfg));
    memset(cfg, 0, sizeof(*cfg));
    list_add_tail(&cfg->list, &config_spec_list);
    return cfg;
}

/*
 * Find the first cpu free on this node and allocate it into this new task.
 */
void allocate_cpu_to_task(int node, tasks_t *task)
{
    int k;

    for (k=0; k<nodes[node].numcpu; k++)
	if (nodes[node].cpu_free[k])
	    break;
    nodes[node].cpu_free[k] = 0;
    --nodes[node].availcpu;
    task->node = node;
    task->cpu_index = &task->cpu_index_one;
    task->cpu_index[0] = k;
    task->num_copies = 1;
}

/*
 * Read the heterogenous config file, making sure it's proper and all the
 * executables exist.  Command-line node limits have already been applied
 * and the tasks[] list reduced accordingly, except for -numproc.
 */
void
parse_config(void)
{
    FILE *fp;
    char buf[16384];
    int i, line, curtask;
    config_spec_t *cfg;

    /*
     * Alloc some space for tasks.  If -np given, must match exactly, else
     * no more than what is available can be allocated.
     */
    numtasks = 0;
    for (i=0; i<numnodes; i++)
	numtasks += nodes[i].availcpu;
    if (cl_args->numproc)
	if (cl_args->numproc < numtasks)
	    numtasks = cl_args->numproc;
    tasks = Malloc(numtasks * sizeof(*tasks));
    memset(tasks, 0, numtasks * sizeof(*tasks));

    curtask = 0;
    cfg = NULL;
    line = 0;
    if (!strcmp(cl_args->config_file, "-"))
	fp = stdin;
    else {
	if (!(fp = fopen(cl_args->config_file, "r")))
	    error_errno("%s: open \"%s\"", __func__, cl_args->config_file);
    }
    while (fgets(buf, sizeof(buf), fp)) {
	char *cp;
	++line;
	if (strlen(buf) == sizeof(buf)-1)
	    error("%s: line %d too long", __func__, line);
	debug(2, "%s: line %d: %s", __func__, line, buf);

	/*
	 * These isspace() casts avoid a warning about
	 * "subscript has type char" on old gcc on suns.
	 */
	for (cp=buf; *cp && isspace((int)*cp); cp++) ;
	if (*cp == '#' || !*cp) continue;  /* comment or eol */
	cfg = new_config_spec();
	cfg->line = line;

	/* run up and find the executable (after the ':') and save it */
	{
	    char *cq, *cr, c;
	    for (cq=cp; *cq && *cq != '#' && *cq != ':'; cq++) ;
	    if (*cq != ':')
		error("%s: line %d: no ':' separating executable",
		  __func__, line);
	    *cq = 0;  /* colon -> 0, further parsing easier */
	    for (++cq; *cq && isspace((int)*cq); cq++) ;
	    if (!*cq || *cq == '#')
		error("%s: line %d: no executable after the ':'",
		  __func__, line);
	    for (cr=cq+1; *cr && *cr != '#'; cr++) ;
	    if (*cr == '#')  /* delete trailing comment */
		*cr = 0;
	    for (--cr; cr > cq && isspace((int)*cr); cr--)
		*cr = '\0';  /* delete trailing space */
	    for (cr=cq+1; *cr && !isspace((int)*cr); cr++) ;
	    c = *cr;
	    *cr = 0;
	    cfg->exe = resolve_exe(cq, 0);
	    *(cq = cr) = c;
	    for (; *cq && isspace((int)*cq); cq++) ;
	    if (*cq)
		cfg->args = strsave(cq);
	}

	/*
	 * Two possible left hand sides:
	 *   -n <numproc> : exe1
	 *   <hostnameglob> [<hostnameglob>...] : exe2
	 */
	if (*cp == '-') {
	    if (*++cp == 'n') {
		long l;
		char *cq;
		for (++cp; *cp && isspace((int)*cp); cp++) ;
		l = strtol(cp, &cq, 10);
		if (l <= 0)
		    error("%s: line %d: \"-n <num>\" must be positive integer",
		     __func__, line);
		for (cp=cq; *cp && isspace((int)*cp); cp++) ;
		if (*cp)
		    error("%s: line %d: junk after \"-n <num>\"",
		      __func__, line);
		/* allocate it now, as many as we can, no prob if not */
		for (i=0; i<numnodes; i++) {
		    if (nodes[i].availcpu == 0) continue;
		    while (l > 0 && nodes[i].availcpu > 0
		      && curtask < numtasks) {
			debug(2, "%s: allocate task %d to host %s", __func__,
			  curtask, nodes[i].name);
			allocate_cpu_to_task(i, &tasks[curtask]);
			tasks[curtask].conf = cfg;
			++curtask;
			--l;
			++cfg->allocated;
		    }
		}
	    } else
		error("%s: line %d: unknown \"-\" argument", __func__, line);
	} else {
	    /* node list */
	    for (cp=buf; *cp; ) {
		char *cq, c;
		/* select a word */
		for (cq=cp+1; *cq && !isspace((int)*cq); cq++) ;
		c = *cq;
		*cq = 0;
		/* shell-style glob searching for nodenames */
		for (i=0; i<numnodes; i++) {
		    if (nodes[i].availcpu == 0) continue;
		    if (fnmatch(cp, nodes[i].name, FNM_CASEFOLD) == 0) {
			/* allocate one task on the matching node */
			if (curtask < numtasks) {
			    allocate_cpu_to_task(i, &tasks[curtask]);
			    tasks[curtask].conf = cfg;
			    ++curtask;
			    ++cfg->allocated;
			}
		    }
		}
		*cq = c;  /* put back delimiter */
		cp = cq;  /* advance to next word, and skip space */
		for (; *cp && isspace((int)*cp); cp++) ;
	    }
	}
    }
    if (fp != stdin)
	fclose(fp);
    if (list_empty(&config_spec_list))
	error("%s: no specification lines in file", __func__);

    /* if "-np" was specified, make sure we found exactly that many */
    if (cl_args->numproc) {
	if (curtask != numtasks)
	    error("%s: argument -n specifies %d processors, but\n"
	      "  config file only matched %d", __func__, cl_args->numproc,
	      curtask);
    } else {
	/*
	 * Okay to get fewer than what was available, either direction.
	 * Either config file specifies fewer tasks than are available in PBS,
	 * or config file is very generic but PBS allocation is limited.
	 * Don't bother to shrink the allocation.
	 */
	numtasks = curtask;
    }
}

/*
 * Generate a single config entry from the argc/argv commandline.
 */
void
argcv_config(int argc, const char *const argv[])
{
    int i, j;
    config_spec_t *cfg;

    /* build a single config spec for all of them to share */
    cfg = new_config_spec();
    cfg->exe = strsave(*argv);
    --argc, ++argv;
    if (!argc)
	cfg->args = NULL;
    else {
	growstr_t *g = growstr_init();
	for (i=0; i<argc; i++) {
	    if (i > 0)
		growstr_append(g, " ");
	    growstr_append(g, argv[i]);
	}
	cfg->args = strsave(g->s);
	growstr_free(g);
    }

    /* already checked that it would fit */
    numtasks = 0;
    for (i=0; i<numnodes; i++)
	numtasks += nodes[i].availcpu;
    if (cl_args->numproc)
	if (cl_args->numproc < numtasks)
	    numtasks = cl_args->numproc;
    tasks = Malloc(numtasks * sizeof(*tasks));
    memset(tasks, 0, numtasks * sizeof(*tasks));

    j = 0;
    for (i=0; i<numnodes; i++) {
	while (nodes[i].availcpu > 0 && j < numtasks) {
	    allocate_cpu_to_task(i, &tasks[j]);
	    tasks[j].conf = cfg;
	    ++j;
	}
    }

}

/*
 * "numtasks" is the number of entries in the tasks[] array, and will
 * be the number of calls to tm_spawn.  ".num_copies" is for comms
 * that fork themselves into other processes.  While we only have to
 * start one per machine, this says how many tasks as far as MPI is
 * concerned will end up running.
 */
void
tasks_shmem_reduce(void)
{
    int i, j;
    const int num_copies_chunk = 10;

    if ((cl_args->comm == COMM_MPICH_P4 && cl_args->mpich_p4_shmem)
      || cl_args->comm == COMM_SHMEM) {
	/*
	 * Find tasks which have the same node and same config but
	 * possibly different cpu# and squeeze them together.
	 */
	for (i=0; i<numtasks; i++) {
	    if (tasks[i].node == -1) continue;  /* deleted */
	    for (j=i+1; j<numtasks; j++) {
		if (tasks[j].node == -1) continue;  /* deleted */
		if (tasks[i].node == tasks[j].node
		 && tasks[i].conf == tasks[j].conf) {
		    if (tasks[i].num_copies == 1) {
			/* initialize array */
			tasks[i].cpu_index = Malloc(num_copies_chunk
			                   * sizeof(*tasks[i].cpu_index));
			tasks[i].cpu_index[0] = tasks[i].cpu_index_one;
		    }
		    if (((tasks[i].num_copies + 1) % num_copies_chunk) == 0) {
			/* if would overflow, grow array */
			int len = tasks[i].num_copies + 1 + num_copies_chunk;
			int *x = Malloc(len * sizeof(*tasks[i].cpu_index));
			memcpy(x, tasks[i].cpu_index, tasks[i].num_copies
			       * sizeof(*tasks[i].cpu_index));
			free(tasks[i].cpu_index);
			tasks[i].cpu_index = x;
		    }

		    tasks[i].cpu_index[tasks[i].num_copies]
			= tasks[j].cpu_index[0];
		    ++tasks[i].num_copies;
		    tasks[j].node = -1;  /* mark deleted */
		}
	    }
	}

	/*
	 * Bubble up list to throw away nulls.
	 */
	i = 0;
	for (j=0; j<numtasks; j++) {
	    if (tasks[j].node != -1) {
		if (i != j)
		    memcpy(&tasks[i], &tasks[j], sizeof(*tasks));
		++i;
	    }
	}
	numtasks = i;

	if (cl_args->comm == COMM_SHMEM) {
	    /* check only one node after squeezing */
	    if (numtasks > 1) {
		warning("%s: SHMEM device only works on one node"
		  " (but many cpus); just using the first node", __func__);
		numtasks = 1;
	    }
	}

	/* do not shrink the allocation, not worth it */
    }

    /*
     * Also verify that mpich/p4 task 0 is on the same node as this
     * running mpiexec.  See put_execer_port() in mpid/ch_p4/p4/lib/p4_utils.c
     * where the connection destination is hardcoded to INADDR_LOOPBACK.
     */
    if (cl_args->comm == COMM_MPICH_P4 && numtasks > 0) {
	if (tasks[0].node != 0)
	    error("%s: When using mpich/p4, the first task\n"
	      "must be on the same machine as mpiexec itself."
	      "  You ended up trying to\nrun task 0 on %s, not %s", __func__,
	      nodes[tasks[0].node].name, nodes[0].name);
    }
}

/*
 * Walk through the configs, and if there is only one executable, return
 * it, else return NULL.  Used for fast executable distribution.
 */
const char *
config_get_unique_executable(void)
{
    const char *s = NULL;
    config_spec_t *c;

    list_for_each_entry(c, &config_spec_list, list) {
	if (s == NULL)
	    s = c->exe;
	else if (strcmp(s, c->exe) != 0) {
	    s = NULL;
	    break;
	}
    }
    return s;
}

/*
 * Executable distribution has succeeded.  Use this new executable.
 * Assumes _get_ was called earlier and did not return NULL.
 */
void
config_set_unique_executable(const char *s)
{
    config_spec_t *c;

    list_for_each_entry(c, &config_spec_list, list) {
	c->exe = s;
    }
}

