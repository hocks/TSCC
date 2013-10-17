/*
 * get_hosts.c - read hostnames from pbs, mark which ones we'll use
 *
 * $Id: get_hosts.c 403 2007-05-28 11:18:39Z pw $
 *
 * Copyright (C) 2000-3 Ohio Supercomputer Center.
 * Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include "mpiexec.h"

#ifndef HAVE_STRSEP
static char *strsep(char **stringp, const char *delim);
#endif
static void transform_with_program(int use_sed);

/*
 * Little helper to set up and query attributes from PBS server.
 * All this malloc/free is because PBS has #defined ATTR_* instead of
 * const char * as they should be, and struct attrl has no "const" in it.
 */
static struct batch_status *
query_server_attr(int fd, const char *attr_name)
{
    struct attrl attrl;
    struct batch_status *bstat;

    memset(&attrl, 0, sizeof(attrl));
    attrl.name = strsave(attr_name);
    attrl.value = strsave("");
    bstat = pbs_statjob(fd, jobid, &attrl, 0);
    if (!bstat)
	error_pbs("%s: pbs_statjob did not return \"%s\" info", __func__,
	          attr_name);
    free(attrl.name);
    free(attrl.value);
    return bstat;
}

/*
 * Temporary storage while figuring out how many unique nodes.
 */
struct host_accum {
    const char *name;  /* into big exechost string */
    int cpu_id;        /* part after / specifying virtual CPU number */
    int parent;
    int children;
};

/*
 * Talk to pbs to get the host names and cpu numbers for this job, do
 * various sanity checks.
 */
void
get_hosts(void)
{
    struct tm_roots task_root;
    tm_node_id *tasklist;
    int tasklist_len;
    struct batch_status *bstat;
    struct attrl *jattr;
    struct host_accum *host;
    char *hostlist, *s;
    char *cp;
    int have_ncpus = 0;
    int have_nodect = 0;
    int fd, i, j, k, l, err, n;

    /*
     * Read the list of taskids from TM.  Even though the call is tm_nodeinfo,
     * it returns an entry for each possible task, i.e numnodes * ncpus.
     */
    err = tm_init(0, &task_root);
    if (err != TM_SUCCESS)
	error_tm(err, "%s: tm_init", __func__);
    err = tm_nodeinfo(&tasklist, &tasklist_len);
    if (err != TM_SUCCESS)
	error_tm(err, "%s: tm_nodeinfo", __func__);
    if (tasklist_len != task_root.tm_nnodes)
	error("%s: tm_nodeinfo says %d nodes, but tm_init said %d", __func__,
	  tasklist_len, task_root.tm_nnodes);

    /*
     * Now go talk to PBS.  Get the hostnames in the job and compress it
     * down to our idea of nodes, matching up against the tasklist as we go.
     */
    fd = pbs_connect(0);
    if (fd < 0)
	error_pbs("%s: pbs_connect", __func__);

    /*
     * Make sure we actually have exechost information.
     */
    bstat = query_server_attr(fd, ATTR_exechost);
    for (jattr = bstat->attribs; jattr; jattr = jattr->next)
	if (!strcmp(jattr->name, ATTR_exechost))
	    break;
    if (!jattr)
	error("%s: pbs_statjob did not return \"%s\" info", __func__,
	  ATTR_exechost);
    hostlist = jattr->value;

    /*
     * Separate this big string into a temp array of name/cpu_id pairs.
     */
    host = Malloc(tasklist_len * sizeof(*host));
    n = 0;
    while ((s = strsep(&hostlist, "+"))) {

	int numcpus = 1;

	if (n >= tasklist_len)
	    error("%s: PBS reports more tasks %d than TM %d", __func__, n,
	          tasklist_len);

	/*
	 * PBSPro introduced a new scheme for these entries that
	 * aggregates multiple hosts together, with their ncpus values.
	 * If we find this, guess at the CPU numbers ourselves.
	 */
	cp = strstr(s, ":ncpus=");
	if (cp) {
	    char *cq;
	    numcpus = strtoul(cp + 7, &cq, 10);
	    if (cq == cp + 7)
		error("%s: invalid :ncpus= string in exec_host \"%s\"",
		      __func__, s);
	}

	/*
	 * PBSPro-provided hostnames have other junk tacked on, e.g.
	 *     exec_host = altix:ssinodes=2:mem=7974912kb:ncpus=4
	 */
	cp = strchr(s, ':');
	if (cp)
	    *cp = '\0';


	/* separate into hostname and CPU number */
	host[n].cpu_id = 0;
	cp = strchr(s, '/');
	if (cp) {
	    char *cq;
	    host[n].cpu_id = strtoul(cp + 1, &cq, 10);
	    if (cq == cp + 1)
		error("%s: invalid /<cpu> string in exec_host \"%s\"",
		      __func__, s);
	    *cp = '\0';
	}

	host[n].name = s;  /* no copy yet */
	host[n].parent = -1;
	host[n].children = 0;
	++n;

	/* fake more entries for PBSPro */
	while (numcpus > 1) {
	    if (n >= tasklist_len)
		error("%s: PBSpro reports more tasks %d than TM %d", __func__,
		      n, tasklist_len);
	    host[n].name = host[n-1].name;
	    host[n].cpu_id = host[n-1].cpu_id + 1;
	    host[n].parent = -1;
	    host[n].children = 0;
	    ++n;
	    --numcpus;
	}
    }

    /*
     * Did we get them all?
     */
    if (n < tasklist_len)
	error("%s: PBS reports fewer tasks %d than TM %d", __func__, n,
	      tasklist_len);

    /*
     * Find identical hostnames and count how many CPUs in each.
     */
    for (i=0; i<tasklist_len; i++) {
	if (host[i].parent >= 0)
	    continue;
	for (j=i+1; j<tasklist_len; j++) {
	    if (host[j].parent >= 0)
		continue;
	    if (strcmp(host[i].name, host[j].name) == 0) {
		host[j].parent = i;
		++host[i].children;
	    }
	}
    }

    /*
     * Allocate the global array of nodes and fill it from the temp array.
     */
    numnodes = 0;
    for (i=0; i<tasklist_len; i++) {
	if (host[i].parent >= 0)
	    continue;
	++numnodes;
    }
    nodes = Malloc(numnodes * sizeof(*nodes));
    memset(nodes, 0, numnodes * sizeof(*nodes));

    k = 0;
    for (i=0; i<tasklist_len; i++) {
	if (host[i].parent >= 0)
	    continue;
	nodes[k].name = strsave(host[i].name);
	nodes[k].numcpu = host[i].children + 1;
	nodes[k].ids = Malloc(nodes[k].numcpu * sizeof(*nodes[k].ids));
	nodes[k].cpu_ids = Malloc(nodes[k].numcpu * sizeof(*nodes[k].ids));
	nodes[k].ids[0] = tasklist[i];
	nodes[k].cpu_ids[0] = host[i].cpu_id;
	l = 1;
	for (j=i+1; j<tasklist_len; j++) {
	    if (host[j].parent == i) {
		nodes[k].ids[l] = tasklist[j];
		nodes[k].cpu_ids[l] = host[j].cpu_id;
		++l;
	    }
	}
	++k;
    }

    free(host);  /* our temp array */
    pbs_statfree(bstat);  /* attribs associated with PBS server query */
    free(tasklist);  /* array allocated by TM */

    /*
     * On a single shared memory node, many comms want to know the
     * number of cpus on the node to spawn correctly, including shmem and
     * mpich-p4-no-shmem.  We also need this to figure out the configuration
     * for single-node SMPs.  Query this info from PBS.
     */
    bstat = query_server_attr(fd, ATTR_l);
    for (jattr = bstat->attribs; jattr; jattr = jattr->next) {
	if (!strcmp(jattr->resource, "ncpus")) {
	    have_ncpus = strtoul(jattr->value, &cp, 10);
	    if (cp == jattr->value)
		error("%s: invalid ncpus string \"%s\"",
		  __func__, jattr->value);
	}
	if (!strcmp(jattr->resource, "nodect")) {
	    have_nodect = strtoul(jattr->value, &cp, 10);
	    if (cp == jattr->value)
		error("%s: invalid nodect string \"%s\"",
		  __func__, jattr->value);
	}
    }
    pbs_statfree(bstat);
    pbs_disconnect(fd);  /* hang up on the PBS server */

    /*
     * Various PBSes disagree about what should appear here, even the
     * same PBS under different configuration params.  Try to do the
     * right thing.
     *   OpenPBS, non-ts: nodes=20:ppn=2 nodect=20
     *   PBSPro, not-ts:  nodes=20:ppn=2 nodect=20 ncpus=40
     *   OpenPBS, ts:     ncpus=2
     *   PBSPro, ts:      ??
     */
    if (!(have_ncpus || have_nodect))
	error("%s: pbs_statjob returned neither \"ncpus\" nor \"nodect\"",
	  __func__);
    if (have_ncpus > 1) {
	if (cl_args->verbose > 2) {
	    printf("%s: numnodes=%d ncpus=%d nodect=%d\n", __func__, numnodes,
	      have_ncpus, have_nodect);
	}
	if (have_nodect > 1 || numnodes > 1) {
	    /* ignore the ncpus setting, trust nodect and exec_host */
	    ;
	} else {
	    /*
	     * Single-node job with ncpus setting.  Do what it says.  Fake
	     * the cpu ids and copy the TM node ids.
	     */
	    int *old_ids = nodes[0].ids;
	    int *old_cpu_ids = nodes[0].cpu_ids;
	    nodes[0].numcpu = have_ncpus;
	    nodes[0].ids = Malloc(nodes[0].numcpu * sizeof(*nodes[0].ids));
	    nodes[0].cpu_ids = Malloc(nodes[0].numcpu
	                              * sizeof(*nodes[0].cpu_ids));
	    for (i=0; i<nodes[0].numcpu; i++) {
		nodes[0].ids[i] = old_ids[0];
		nodes[0].cpu_ids[i] = old_cpu_ids[0] + i;
	    }
	    free(old_ids);
	    free(old_cpu_ids);
	}
    }

    /*
     * Init available CPUs.
     */
    for (i=0; i<numnodes; i++) {
	nodes[i].cpu_free = Malloc(nodes[i].numcpu
	                           * sizeof(*nodes[i].cpu_free));
	nodes[i].cm_cpu_free = Malloc(nodes[i].numcpu
	                              * sizeof(*nodes[i].cm_cpu_free));
	for (j=0; j<nodes[i].numcpu; j++) {
	    nodes[i].cpu_free[j] = 1;
	    nodes[i].cm_cpu_free[j] = 1;
	}
	nodes[i].availcpu = nodes[i].numcpu;
	nodes[i].cm_availcpu = nodes[i].numcpu;
    }
}

/*
 * Copy the nodes array into tasks, selectively, according to what nodes
 * are available given command-line option constraints.
 */
void
constrain_nodes(void)
{
    int i, j, numleft;
    const char *complaint;

    /*
     * Max we can choose from.
     */
    numleft = 0;
    for (i=0; i<numnodes; i++)
	numleft += nodes[i].availcpu;

    /* could happen if concurrent master gives us a list with 0 available */
    if (numleft == 0)
	error("%s: no processors left in overall allocation", __func__);

    /*
     * If -nolocal, do not run anything on the machine that has the
     * mpiexec process.
     */
    if (cl_args->nolocal) {
	if (cl_args->comm == COMM_MPICH_P4)
	    error("%s: -nolocal will not work with mpich/p4", __func__);

	if (cl_args->verbose)
	    printf("removing host %s from consideration for -nolocal\n",
	      nodes[0].name);

	numleft -= nodes[0].availcpu;
	nodes[0].availcpu = 0;
	for (j=0; j<nodes[0].numcpu; j++)
	    nodes[0].cpu_free[j] = 0;

	if (numleft == 0)
	    error("%s: no processors left after processing -nolocal flag",
	      __func__);
    }

    /* enforce one process (or some other limit) per physical node */
    if (cl_args->pernode) {
	for (i=0; i<numnodes; i++) {
	    int must_not_use = nodes[i].availcpu - cl_args->pernode;
	    if (must_not_use > 0)
		/* get rid of CPUs from the high end, for symmetry */
		for (j=nodes[i].numcpu-1; j>=0; j--)
		    if (nodes[i].cpu_free[j]) {
			nodes[i].cpu_free[j] = 0;
			--nodes[i].availcpu;
			--numleft;
			--must_not_use;
			if (must_not_use == 0)
			    break;
		    }
	}
    }

    /* only used if there's a problem */
    if (cl_args->nolocal) {
	if (cl_args->pernode)
	    complaint = "-nolocal and -[n]pernode flags";
	else
	    complaint = "-nolocal flag";
    } else
	complaint = "-[n]pernode flag";

    if (numleft == 0) {
	error("%s: no processors left after processing %s", __func__,
	  complaint);
    }

    /*
     * User-specified numproc considered later too in either command-line
     * or config file processing.  See those functions.
     */
    if (cl_args->numproc) {
	if (cl_args->numproc > numleft) {
	    growstr_t *g = growstr_init();
	    if (cl_args->pernode || cl_args->nolocal)
		growstr_printf(g, " after processing %s", complaint);
	    error(
	     "%s: argument -n specifies %d processors, but\n"
	     "  only %d available%s",
	      __func__, cl_args->numproc, numleft, g->s);
	}
    }

    /*
     * If -transform-hostname, user wants to use a different interface
     * (corresponding to a different name or IP) for message
     * passing.  Different, that is, from the name PBS uses.  Here we
     * shell out to "sed", passing it a list of hostnames, one per line,
     * and letting the user's sed script argument process the list.
     * We do all the hosts, even if will actually use fewer.
     *
     * Another option supports calling any external program, as named on
     * the command line, not just sed to do this task.
     */
    if (cl_args->transform_hostname) {
	transform_with_program(1);
    } else if (cl_args->transform_hostname_program) {
	transform_with_program(0);
    } else {
	/* default MPI name is same as PBS name */
	for (i=0; i<numnodes; i++)
	    nodes[i].mpname = nodes[i].name;
    }
}

/*
 * Try to reconnect to a mom after an error, such as it exiting and
 * restarting.
 */
void
reconnect_to_mom(void)
{
    int i, err;

    if (cl_args->verbose > 0)
	printf("%s: mom died, trying continually to reconnect\n", __func__);

    /*
     * Poll waiting for mom to come back up.
     */
    for (;;) {
	/* Even a failed tm_init will build some internal state that must be
	 * deleted by this call to finalize. */
	struct tm_roots task_root;
	tm_finalize();
        err = tm_init(0, &task_root);
        if (err == TM_SUCCESS)
            break;
	if (cl_args->verbose > 0)
	    warning_tm(err, "%s: waiting for mom to come back", __func__);
        sleep(2);
    }
    if (cl_args->verbose > 0)
	printf("%s: walking existing task list and resubmitting obits\n",
	  __func__);
    for (i=0; i<numtasks; i++) {
	if (tasks[i].done != DONE_NOT) {
	    if (cl_args->verbose > 0)
		printf("%s: task %d already done\n", __func__, i);
	    continue;
	}
#if 0
	/*
	 * Clear pending events, restart.
	 * XXX: Fix this.
	 */
	tasks[i].evt = 0;
	tasks[i].evt_obit = 0;
	err = tm_obit(tasks[i].tid, tasks[i].status, &tasks[i].evt);
	if (err == TM_ENOTFOUND) {
	    warning("%s: task %u not found, assuming done\n",
	      __func__, tasks[i].tid);
	    tasks[i].done = DONE_NO_EXIT_STATUS;
	    continue;
	}
#endif
	if (err != TM_SUCCESS)
	    error_tm(err, "%s: sending obit for task %d", __func__, i);
	if (cl_args->verbose > 0)
	    printf("%s: new obit for task %u\n", __func__,
	      tasks[i].tid);
    }
}

#ifndef HAVE_STRSEP
static char *
strsep(char **stringp, const char *delim)
{
    char *s = *stringp, *end;

    if (!s)
	return 0;

    end = strpbrk(s, delim);
    if (end)
	*end++ = 0;
    *stringp = end;
    return s;
}
#endif

/*
 * Pipe, fork, etc. to use an external sed binary to transform the hostnames
 * from their PBS form into the message passing form.  Or if not use_sed,
 * use the named program from command line argument.
 */
static void
transform_with_program(int use_sed)
{
    int fdr[2], fdw[2], fde[2], pid, i;
    int rptr, wptr, roff;
    char s[2048];
    int wbufpos;
    growstr_t *g;

    if (pipe(fdr) < 0 || pipe(fdw) < 0 || pipe(fde) < 0)
	error_errno("%s: pipe", __func__);
    pid = fork();
    if (pid < 0)
	error_errno("%s: fork", __func__);
    if (pid == 0) {
	/* child */
	const char *codename, *cp;
	close(0);
	close(1);
	close(2);
	close(fdw[1]);
	close(fdr[0]);
	close(fde[0]);
	if (dup2(fdw[0], 0) < 0 || dup2(fdr[1], 1) < 0 || dup2(fde[1], 2) < 0)
	    error_errno("%s: child dup2", __func__);
	if (use_sed) {
	    /* invoke sed directly where it was found at configure time */
	    for (cp=codename=SED_PATH; *cp; cp++)
		if (*cp == '/')
		    codename = cp+1;
	    execl(SED_PATH, codename, "-e", cl_args->transform_hostname, NULL);
	} else {
	    /* use shell to lookup the program in the path; know /bin/sh is
	     * available since it was used to run configure */
	    execl("/bin/sh", "sh", "-c", cl_args->transform_hostname_program,
	      NULL);
	}
	error_errno("%s: child execl", __func__);
    }

    /* parent */
    close(fdw[0]);
    close(fdr[1]);
    close(fde[1]);

    /* set non-blocking r,w since program might use hold-space tricks */
    i = fcntl(fdw[1], F_GETFL);
    if (i < 0)
	error_errno("%s: fcntl F_GETFL fdw", __func__);
    if (fcntl(fdw[1], F_SETFL, i | O_NONBLOCK))
	error_errno("%s: fcntl F_SETFL fdw", __func__);

    i = fcntl(fdr[0], F_GETFL);
    if (i < 0)
	error_errno("%s: fcntl F_GETFL fdr", __func__);
    if (fcntl(fdr[0], F_SETFL, i | O_NONBLOCK))
	error_errno("%s: fcntl F_SETFL fdr", __func__);

    i = fcntl(fde[0], F_GETFL);
    if (i < 0)
	error_errno("%s: fcntl F_GETFL fde", __func__);
    if (fcntl(fde[0], F_SETFL, i | O_NONBLOCK))
	error_errno("%s: fcntl F_SETFL fde", __func__);

    rptr = wptr = roff = 0;
    g = growstr_init();
    wbufpos = 0;
    for (;;) {
	i = read(fde[0], s, sizeof(s)-1);
	if (i > 0) {
	    /* ignore pipe errors, just print complaints, have own \n */
	    s[i] = '\0';
	    fprintf(stderr, "%s: %s: error: %s", progname,  __func__, s);
	}
	if (rptr < numnodes) {
	    i = read(fdr[0], s + roff, sizeof(s) - roff);
	    if (i < 0) {
		if (errno != EAGAIN)
		    error_errno("%s: read pipe", __func__);
	    } else if (i == 0) {
		error("%s: read pipe closed", __func__);
	    } else {
		if (roff + i == sizeof(s))
		    error("%s: out of space in read", __func__);
		roff += i;
		s[roff] = '\0';
		/* parse out whole lines */
		for (;;) {
		    char *cp, *cq;
		    for (cp=s; *cp && *cp != '\n'; cp++) ;
		    if (!*cp)
			break;
		    /* thus cp is on a newline */
		    *cp = '\0';
		    nodes[rptr].mpname = strsave(s);
		    ++rptr;
		    if (rptr == numnodes) {
			close(fdr[0]);
			break;  /* hopefully not necessary */
		    }
		    /* move up unused part of string */
		    for (++cp, cq=s;; cp++, cq++) {
			*cq = *cp;
			if (!*cp)
			    break;
		    }
		    roff = cq - s;  /* cq is on \0 */
		}
	    }
	}
	if (wptr < numnodes) {
	    if (!g->len) {
		growstr_append(g, nodes[wptr].name);
		growstr_append(g, "\n");
		wbufpos = 0;
	    }
	    i = write(fdw[1], g->s + wbufpos, g->len - wbufpos);
	    if (i < 0) {
		if (errno != EAGAIN)
		    error_errno("%s: write pipe", __func__);
	    } else if (i == 0) {
		error("%s: write pipe closed", __func__);
	    } else if (wbufpos + i != g->len) {
		wbufpos += i;   /* partial write */
	    } else {
		growstr_zero(g);
		++wptr;
		if (wptr == numnodes)
		    close(fdw[1]);
	    }
	}
	if (rptr == numnodes && wptr == numnodes)
	    break;
	/* could yield here, but sched_yield may not be very standard */
	usleep(1000);
    }
    if (waitpid(pid, &i, 0) < 0)
	error_errno("%s: waitpid", __func__);
}

