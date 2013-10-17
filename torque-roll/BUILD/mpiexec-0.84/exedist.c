/*
 * exedist.c - use FAST scalable executable distribution to move the exe to
 * the compute nodes
 *
 * Returns 0 on success and 1 in the event that the distribution could
 * not be handled.
 *
 * $Id: exedist.c 400 2007-03-05 22:47:23Z pw $
 *
 * Copyright (C) 2005-6 Pete Wyckoff <pw@osc.edu>
 * Copyright (C) 2005 Dennis Dalessandro <dennis@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include "mpiexec.h"

/*
 * Overall executable distribution using fast.
 */
int
distribute_executable(void)
{
    int ret = 1;  /* failure */

#if HAVE_FAST_DIST
    const char *fast_command = FAST_DIST_PATH;  /* from configure */
    int i;
    int numtasks_save;
    int local_numtasks;
    tasks_t *tasks_save;
    cl_args_t cl_args_save;
    config_spec_t cs, root_cs;
    growstr_t *g, *root_g;
    int temp_fd;
    char *file_template;
    int port_num;
    FILE *fp;
    const char *exec_to_dist;
    int *usenodes;

    exec_to_dist = config_get_unique_executable();
    if (!exec_to_dist)
	return ret;

    if (!stat_exe(fast_command, 0))
	return ret;

    /* analyze nodes */
    usenodes = Malloc(numnodes * sizeof(*usenodes));
    memset(usenodes, 0, numnodes * sizeof(*usenodes));
    local_numtasks = 0;
    for (i=0; i<numtasks; i++) {
	if (!usenodes[tasks[i].node]) {
	    usenodes[tasks[i].node] = 1;
	    ++local_numtasks;
	}
    }

    /* don't bother if there is only one node */
    if (local_numtasks <= 1) {
	free(usenodes);
	return ret;
    }

    /* create temporary node file */
    file_template = strsave("/tmp/mpiexec-fast-XXXXXX");
    temp_fd = mkstemp(file_template);
    if (!temp_fd)
	goto out;
    debug(1, "%s: temp node list file is %d",__func__, temp_fd);
    fp = fdopen(temp_fd, "w");
    if (!fp)
	goto out;

    /* add nodes to the node file */
    for (i=0; i<numnodes; i++) {
	if (!usenodes[i])
	    continue;
	if (fprintf(fp, "%s\n", nodes[i].name) <= 0) {
	    fclose(fp);
	    goto out;
	}
    }
    if (fclose(fp) != 0)
	goto out;

    /* pick a random port number between 6 and 8 thousand */
    srand(time(NULL));
    port_num = rand() % 2000 + 6000;

    /*
     * Back up the tasks structure and number of tasks as well as command
     * line args.
     */
    tasks_save = tasks;
    numtasks_save = numtasks;
    memcpy(&cl_args_save, cl_args, sizeof(*cl_args));

    /* set the fast_dist executable name */
    cs.exe = fast_command;
    root_cs.exe = cs.exe;

    /* set up the args to pass to the non-root nodes */
    g = growstr_init();
    growstr_printf(g, "-p %d", port_num);
    cs.args = g->s;
    debug(1, "%s: arg string for non root: %s", __func__, g->s);

    /* and to the root node */
    root_g = growstr_init();
    growstr_printf(root_g, "-p %d -r %s -e %s -n %s",
      port_num, nodes[tasks[0].node].name, exec_to_dist, file_template);
    root_cs.args = root_g->s;
    debug(1, "%s: arg string for root: %s", __func__, root_g->s);

    /* build new tasks */
    cl_args->which_stdin = STDIN_NONE;
    cl_args->comm = COMM_NONE;
    tasks = Malloc(local_numtasks * sizeof(*tasks));
    numtasks = local_numtasks;
    for (i=0; i < numtasks; i++) {
	tasks[i].num_copies = 1;
	tasks[i].done = DONE_NOT_STARTED;
	*tasks[i].status = -1;
	/*
	 * Slight race condition in that the root wants to actively connect
	 * to some other nodes, but it will retry a bit.  Put root last to
	 * hope that there is a bit of delay in startup.
	 */
	if (i == numtasks - 1) {
	    tasks[i].node = tasks_save[0].node;
	    tasks[i].conf = &root_cs;
	} else {
	    tasks[i].node = tasks_save[i+1].node;
	    tasks[i].conf = &cs;
	}
	debug(1, "%s: task %d on %d", __func__, i, tasks[i].node);
    }

    /* spawn tasks */
    start_tasks(0);
    debug(1, "%s: tasks started", __func__);

    /* wait for them to exit */
    wait_tasks();

    /* make sure everyone finished successfully */
    ret = 0;
    for (i=0; i<numtasks; i++) {
	if (tasks[i].done == DONE_NO_EXIT_STATUS)
	    continue;
	if (*tasks[i].status != 0) {
	    ret = 1;
	    break;
	}
    }
    debug(1, "%s: done, ret = %d", __func__, ret);

    /* put back original tasks structures */
    free(tasks);
    tasks = tasks_save;
    numtasks = numtasks_save;
    memcpy(cl_args, &cl_args_save, sizeof(*cl_args));
    growstr_free(g);
    growstr_free(root_g);

    /*
     * Update executable in old config structure to point to new /tmp exec,
     * using the same algorithm as fast_dist.  It is not deleted upon
     * completion but relies on $TMPDIR being deleted when PBS cleans up the
     * job or normal /tmp cleaning.
     */
    if (ret == 0) {
	const char *cp, *base;
	growstr_t *h;

	h = growstr_init();
	cp = getenv("TMPDIR");
	if (!cp || !*cp)
	    cp = "/tmp";
	growstr_append(h, cp);

	for (cp=base=exec_to_dist; *cp; cp++)
	    if (*cp == '/')
		base = cp+1;
	growstr_append(h, "/");
	growstr_append(h, base);

	config_set_unique_executable(strsave(h->s));
	growstr_free(h);
    }

  out:
    unlink(file_template);
    free(file_template);
    free(usenodes);
#endif /* HAVE_FAST_DIST */

    return ret;
}

