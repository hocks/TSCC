/*
 * pmi.c - handle process startup and stdio for PMI interface
 *
 * $Id: pmi.c 420 2008-04-10 21:40:21Z pw $
 *
 * Copyright (C) 2003-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include "mpiexec.h"

/* shared with stdio.c */
int pmi_listen_fd, pmi_listen_port, *pmi_fds, *pmi_barrier;

/* Global representing which spawn we are working on; starts at 0. */
static int curspawn;

/*
 * Used to pass key=val arrays around.  Pointers are in-place to
 * a string which has just been parsed, destructively.
 */
#define MAX_KEYVAL 64
#define KEYVAL_LEN 2048
typedef struct {
    int num;
    char *key[MAX_KEYVAL];
    char *val[MAX_KEYVAL];
} pmi_keyval_t;

/* list of registered key pairs, via PMI cmd=put */
typedef struct {
    struct list_head list;
    char *key;
    char *val;
} pmi_keypair_t;

/* the keypair space is unique to each kvsname (each spawn) */
typedef struct {
    struct list_head list;
    char *kvsname;
    struct list_head keypair_list;
} pmi_keypair_space_t;
static LIST_HEAD(pmi_keypair_space_list);

/*
 * Published names to suport MPI_Publish_name, MPI_Lookup_name etc.
 */
typedef struct {
    struct list_head list;
    char *service;
    char *port;
} published_name_t;
static LIST_HEAD(published_name_list);

/*
 * Lookup or create the keypair space for this rank.
 */
static pmi_keypair_space_t *
keypair_space_get(int spawn)
{
    growstr_t *g;
    pmi_keypair_space_t *ks;

    g = growstr_init();
    growstr_printf(g, "%s-spawn-%d", jobid, spawn);
    list_for_each_entry(ks, &pmi_keypair_space_list, list) {
	if (strcmp(ks->kvsname, g->s) == 0)
	    break;
    }
    if (&ks->list == &pmi_keypair_space_list)
	ks = NULL;
    if (!ks) {
	ks = Malloc(sizeof(*ks));
	ks->kvsname = strsave(g->s);
	INIT_LIST_HEAD(&ks->keypair_list);
	list_add_tail(&ks->list, &pmi_keypair_space_list);
    }
    growstr_free(g);
    return ks;
}

static pmi_keyval_t *
read_keyvals(int fd, int multi_cmd_offset)
{
    /* only one in use at a time, if that changes, remember to strsave, free */
    static char s[KEYVAL_LEN];
    static pmi_keyval_t kv_static;
    int cc;
    char *cp;
    pmi_keyval_t *kv = &kv_static;

    cc = read_until(fd, s, sizeof(s), "\n", 0);
    if (cc < 0)
	error_errno("%s: read until", __func__);
    if (cc == 0)
	return 0;
    debug(4, "%s: read %d chars: %s", __func__, cc, s);

    /* scan words into key/val pairs */
    kv->num = multi_cmd_offset;
    cp = s;
    for (;;) {
	char *end, *cq;
	/* cast to int makes gcc 2.95.2 on sun happy */
	while (isspace((int)*cp) && *cp != '\n') cp++;
	if (!*cp)  /* possibly overwritten \n */
	    break;
	if (*cp == '\n') {  /* ate the line, point to next ready one */
	    ++cp;
	    break;
	}
	if (kv->num >= MAX_KEYVAL)
	    error("%s: number of keyvals exceeds compiled-in"
	      " constant %d in keyval line %s", __func__, MAX_KEYVAL, s);
	kv->key[kv->num] = cp;
	if (multi_cmd_offset)
	    /* read through end-of-line */
	    for (end=cp; *end && *end != '\n'; end++) ;
	else
	    for (end=cp; *end && !isspace((int)*end); end++) ;
	for (cq=cp; cq<end; cq++)
	    if (*cq == '=') {
		*cq = '\0';
		break;
	    }
	if (cq == end) {
	    if (multi_cmd_offset > 0 && !strcmp(s, "endcmd\n"))
		;  /* special no-equals end of a mcmd series of lines */
	    else
		error("%s: no '=' found in keyval %d of line: %s", __func__,
		  kv->num, s);
	}
	++cq;
	kv->val[kv->num] = cq;
	*end = '\0';

	debug(3, "%s: keyval %d key %s val %s", __func__, kv->num,
	  kv->key[kv->num], kv->val[kv->num]);

	++kv->num;
	cp = end + 1;
    }

    if (kv->num == multi_cmd_offset)
	error("%s: no keyvals in line: %s", __func__, s);

    return kv;
}

/*
 * Each PMI process connects to our socket, then asks for some
 * information.
 *
 * compare to mpich2/src/pmi/simple/simple_pmi.c
 *
 * mpich2 0.94b1 does not support MPI_Abort properly yet.
 * mpich2 1.0.1 with pm/smpd won't work as it defines
 *   USE_HUMAN_READABLE_TOKENS, but that code looks deprecated, so
 *   ignore it.
 * mpich2 1.0.3 works, except not MPI_Abort still.
 *
 * This version supports ch3 device, channels sock and shm and ssm,
 * and OSU's mvapich2 and probably others if they use the common
 * PMI interface provided by mpich2.
 */
void
accept_pmi_conn(fd_set *rfs)
{
    int i, fd, rank;
    int pmi_version, pmi_subversion;
    growstr_t *g = growstr_init();
    pmi_keyval_t *kv;
    char *cq;
    static const int my_pmi_version = 1;
    static const int my_pmi_subversion = 1;

    debug(2, "%s: waiting in accept", __func__);

    /*
     * Accept a new PMI connection, blocking in stdio listener.
     */
    for (;;) {
	fd = accept(pmi_listen_fd, 0, 0);
	if (fd >= 0)
	    break;
	if (errno != EAGAIN)
	    error_errno("%s: accept", __func__);
    }

    /*
     * Do the entire handshake with one process.  These are all
     * blocking reads and writes, and maybe should not be, but messy to
     * test everywhere in here.
     */

    /*
     * PMII_Set_from_port
     */

    /* request: cmd=initack pmiid=%d */
    kv = read_keyvals(fd, 0);
    if (!kv)
	error("%s: eof", __func__);
    if (strcmp(kv->key[0], "cmd"))
	error("%s: no \"cmd\" key in keyvals", __func__);
    if (strcmp(kv->val[0], "initack"))
	error("%s: expecting cmd=initack, got %s", __func__, kv->val[0]);
    if (kv->num != 2)
	error("%s: in cmd=%s, expecting 2 keyvals, got %d", __func__,
	      kv->val[0], kv->num);
    if (strcmp(kv->key[1], "pmiid"))
	error("%s: in cmd=%s, expecting pmiid", __func__, kv->val[0]);
    rank = strtoul(kv->val[1], &cq, 10);
    if (cq == kv->val[1])
	error("%s: in cmd=%s, invalid value to pmiid \"%s\"",
	  __func__, kv->val[0], kv->val[1]);
    debug(1, "%s: cmd=%s pmiid=%d", __func__, kv->val[0], rank);

    /* verify rank okay */
    if (rank < 0
     || rank >= spawns[curspawn].task_end - spawns[curspawn].task_start)
	error("%s: rank %d out of bounds [0..%d)", __func__,
	  rank, spawns[curspawn].task_end - spawns[curspawn].task_start);
    /* rank checked in already? */
    if (pmi_fds[rank + spawns[curspawn].task_start] != -1)
	error("%s: rank %d (spawn %d) checked in twice", __func__, rank, curspawn);
    pmi_fds[rank + spawns[curspawn].task_start] = fd;
    poll_set(pmi_fds[rank + spawns[curspawn].task_start], rfs);
    debug(1, "%s: rank %d (spawn %d) checks in", __func__, rank, curspawn);

    /* response: cmd=initack */
    growstr_zero(g);
    growstr_printf(g, "cmd=initack\n");
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: response cmd=initack", __func__);

    /* response: cmd=set size=numtasks */
    growstr_zero(g);
    growstr_printf(g, "cmd=set size=%d\n",
                   spawns[curspawn].task_end - spawns[curspawn].task_start);
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: response cmd=set size=%d", __func__,
	            spawns[curspawn].task_end - spawns[curspawn].task_start);

    /* response: cmd=set rank=rank */
    growstr_zero(g);
    growstr_printf(g, "cmd=set rank=%d\n", rank);
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: response cmd=set rank=%d", __func__, rank);

    /* response: cmd=set debug=[0|1] */
    {
    int mpi_task_debug = (cl_args->verbose > 2);
    growstr_zero(g);
    growstr_printf(g, "cmd=set debug=%d\n", mpi_task_debug);
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: response cmd=set debug=%d", __func__, mpi_task_debug);
    }

    if (cl_args->tview) {
       growstr_zero(g);
       growstr_printf(g, "cmd=tv_ready\n");
       if (write_full(fd, g->s, g->len) < 0)
	   error_errno("%s: response cmd=tv_ready", __func__);
    }

    /*
     * PMII_getmaxes
     */

    /* request: cmd=init pmi_version=%d pmi_subversion=%d */
    kv = read_keyvals(fd, 0);
    if (!kv)
	error("%s: eof in rank %d (spawn %d)", __func__, rank, curspawn);
    if (strcmp(kv->key[0], "cmd"))
	error("%s: expecting cmd=init, no \"cmd\" key in keyvals",
	  __func__);
    if (strcmp(kv->val[0], "init"))
	error("%s: expecting cmd=init, got cmd=%s", __func__,
	  kv->val[0]);
    if (kv->num == 3) {
	/*
	 * Modern mpich2 version:
	 *   cmd=init pmi_version=1 pmi_subversion=1
	 */
	if (strcmp(kv->key[1], "pmi_version"))
	    error("%s: cmd=init, no \"pmi_version\" key", __func__);
	pmi_version = strtoul(kv->val[1], &cq, 10);
	if (cq == kv->val[1])
	    error("%s: cmd=init, invalid value to pmi_version \"%s\"",
	      __func__, kv->val[1]);
	if (strcmp(kv->key[2], "pmi_subversion"))
	    error("%s: cmd=init, no \"pmi_subversion\" key", __func__);
	pmi_subversion = strtoul(kv->val[2], &cq, 10);
	if (cq == kv->val[2])
	    error("%s: cmd=init, invalid value to pmi_subversion \"%s\"",
	      __func__, kv->val[2]);
	debug(1, "%s: cmd=init pmi_version=%d pmi_subversion=%d",
	  __func__, pmi_version, pmi_subversion);
    } else if (kv->num == 2) {
	/*
	 * Old mpich2 and modern intel version.  These say:
	 *   cmd=init pmi_version=1.1
	 * Historical note:  Intel actually suggested this format version
	 * string to ANL, who used it in their v0.97 but later unilaterally
	 * changed it for their v1.0.  It will all wash out with time.
	 */
	char *cp;
	if (strcmp(kv->key[1], "pmi_version"))
	    error("%s: cmd=init, 2 keyvals, no \"pmi_version\" key", __func__);
	cp = strchr(kv->val[1], '.');
	if (!cp)
	    error("%s: cmd=init, 2 keyvals, no '.' in \"pmi_version\" val",
	      __func__);
	*cp = '\0';
	pmi_version = strtoul(kv->val[1], &cq, 10);
	if (cq == kv->val[1])
	    error("%s: cmd=init, 2 keyvals, invalid pmi_version \"%s\"",
	      __func__, kv->val[1]);
	pmi_subversion = strtoul(cp+1, &cq, 10);
	if (cq == cp+1)
	    error("%s: cmd=init, 2 keyvals, invalid pmi_subversion \"%s\"",
	      __func__, cp+1);
	debug(1, "%s: got request: cmd=init pmi_version=%d.%d",
	  __func__, pmi_version, pmi_subversion);
    } else
	error("%s: in cmd=init, expecting 2 or 3 keyvals total, got %d",
	  __func__, kv->num);

    /* response: cmd=response_to_init rc=0 if okay */
    growstr_zero(g);
    i = !(pmi_version == my_pmi_version && pmi_subversion == my_pmi_subversion);
    growstr_printf(g, "cmd=response_to_init rc=%d", i);
    if (i)
	growstr_printf(g, " pmi_version=%d pmi_subversion=%d",
	  my_pmi_version, my_pmi_subversion);
    growstr_printf(g, "\n");
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: write response %s", __func__, g->s);

    if (i) {
	warning("%s: cmd=init, expecting version %d subversion %d, got %d %d",
	  __func__, my_pmi_version, my_pmi_subversion,
	  pmi_version, pmi_subversion);
	goto skip;
    }

    /* request: cmd=get_maxes */
    kv = read_keyvals(fd, 0);
    if (!kv)
	error("%s: eof in rank %d (spawn %d)", __func__, rank, curspawn);
    if (strcmp(kv->key[0], "cmd"))
	error("%s: expecting cmd=get_maxes, no \"cmd\" key in keyvals",
	  __func__);
    if (strcmp(kv->val[0], "get_maxes"))
	error("%s: expecting cmd=get_maxes, got cmd=%s", __func__,
	  kv->val[0]);
    if (kv->num != 1)
	error("%s: in cmd=get_maxes, expecting 1 keyval total", __func__);
    debug(2, "%s: cmd=get_maxes", __func__);

    /* response: cmd=maxes kvsname_max=%d keylen_max=%d vallen_max=%d */
    growstr_zero(g);
    growstr_printf(g,
      "cmd=maxes kvsname_max=%d keylen_max=%d vallen_max=%d\n",
      KEYVAL_LEN, KEYVAL_LEN, KEYVAL_LEN);
    if (write_full(fd, g->s, g->len) < 0)
	error_errno("%s: response cmd=maxes ...", __func__);

    /*
     * The startup sequence will do some number of put here, then
     * finally a barrier for all.  Instead of trying to conditionalize
     * it all here, we just let the normal event loop continue the
     * initialization of each process.
     */

  skip:
    /* if all are connected, close listener */
    for (i=spawns[curspawn].task_start; i<spawns[curspawn].task_end; i++)
	if (pmi_fds[i] == -1) break;
    if (i == spawns[curspawn].task_end) {
	close(pmi_listen_fd);
	poll_del(pmi_listen_fd, rfs);
	pmi_listen_fd = -1;
    }

    growstr_free(g);
}

struct task_node_pair {
    int taskid;
    int nodeid;
};

/*
 * Sort an array of task/node integer pairs for get_ranks2hosts.
 */
static int sort_tnpair(const void *va, const void *vb)
{
    const struct task_node_pair *a = va;
    const struct task_node_pair *b = vb;

    if (a->nodeid < b->nodeid)
	return -1;
    if (a->nodeid > b->nodeid)
	return 1;
    if (a->taskid < b->taskid)
	return -1;
    if (a->taskid > b->taskid)
	return 1;
    error("%s: found duplicate node %d task %d",
          __func__, a->nodeid, a->taskid);
}

/*
 * Generate the response to a get_ranks2hosts PMI command, for
 * Intel MPI >= v3.0.
 *
 * response: cmd=put_ranks2hosts <msglen> <num_of_hosts>\n
 * then <num_of_hosts> more three-word triplets that look like:
 *   <hnlen> <hostname> <rank1>,...,<rankN>,
 * where:
 *   msglen: number of characters to follow, plus 1
 *   num_of_hosts: total number of non-recurring host names
 *   hnlen: number of characters in the next hostname field
 *   hostname: node name
 *   rank1,...,rankN,: comma-separated list of task numbers that
 *                     run on this node.
 * Intel docs demand spaces between the three-word triplets (including
 * after the last of those), and a newline after the initial put
 * line and after all the triplets.
 */
static growstr_t *ranks2hosts_lookup_or_build(int spawn)
{
    struct task_node_pair *tnpair;
    growstr_t *g, *h;
    int i, task_start, task_end;
    int working_nodeid, total_nodeid;

    /*
     * Since every task will ask for this, we keep a cache so we don't
     * have to recalculate it every time.
     */
    if (spawns[spawn].ranks2hosts_response)
    	return spawns[spawn].ranks2hosts_response;

    task_start = spawns[spawn].task_start;
    task_end = spawns[spawn].task_end;

    /*
     * Allocate a temp array to hold node/task mapping info, just
     * for the set of tasks in this spawn, and sort it so it will
     * be easy to walk through to construct the response string.
     */
    tnpair = Malloc((task_end - task_start) * sizeof(*tnpair));
    for (i=task_start; i<task_end; i++) {
	tnpair[i-task_start].taskid = i;
	tnpair[i-task_start].nodeid = tasks[i].node;
    }
    qsort(tnpair, task_end - task_start, sizeof(*tnpair), sort_tnpair);

    /*
     * Build the second part of the response first, as we need to
     * send the overall length.
     */
    h = growstr_init();
    working_nodeid = -1;
    total_nodeid = 0;
    for (i=task_start; i<task_end; i++) {
	if (tnpair[i-task_start].nodeid != working_nodeid) {
	    /* terminate working triplet */
	    if (i > task_start)
		growstr_append(h, " ");
	    /* start a new triplet */
	    ++total_nodeid;
	    working_nodeid = tnpair[i-task_start].nodeid;
	    growstr_printf(h, "%d %s ",
			   (int) strlen(nodes[working_nodeid].mpname),
			   nodes[working_nodeid].mpname);
	}
	/* append another task to this node list */
	growstr_printf(h, "%d,", tnpair[i-task_start].taskid);
    }
    /* terminate working triplet, and eol for entire list */
    growstr_append(h, " \n");

    /*
     * Now build the first part of the message and tack on the list
     * of triplets we just figured out.  Note there is no "cmd=" on
     * this response as for other PMI commands.  It works as is with
     * Intel MPI 3.0, and adding cmd= seems like the right thing to
     * do for consistency, but perhaps that breaks things.
     */
    g = growstr_init();
    growstr_printf(g, "put_ranks2hosts %d %d\n", h->len+1, total_nodeid);
    growstr_append(g, h->s);

    debug(2, "%s: ranks2hosts reply: %s", __func__, g->s);

    /* cache it */
    spawns[spawn].ranks2hosts_response = g;

    growstr_free(h);
    free(tnpair);
    return g;
}


/*
 * Handle an incoming PMI request.  This argument is not really rank from
 * the point of view of the process, but actually the index into tasks[].
 * In the case of multiple spawns, there are multiple, e.g. rank 0.
 */
void
handle_pmi(int rank, fd_set *rfs)
{
    pmi_keyval_t *kv;
    int spawn;
    growstr_t *g;
    pmi_keypair_space_t *ks;

    spawn = 0;
    for (;;) {
	if (rank >= spawns[spawn].task_start && rank < spawns[spawn].task_end)
	    break;
	++spawn;
	if (spawn == numspawns)
	    error("%s: rank %d not in spawns list", __func__, rank);
    }
    ks = keypair_space_get(spawn);
    debug(2, "%s: rank %d spawn %d kvsname %s", __func__, rank, spawn,
          ks->kvsname);

    /*
     * Read the cmd
     */
    kv = read_keyvals(pmi_fds[rank], 0);
    if (!kv)
	goto finalize;
    if (strcmp(kv->key[0], "mcmd") == 0) {
	/*
	 * Multi-line command, more key/val pairs on separate lines until an
	 * end marker appears.
	 */
	for (;;) {
	    kv->key[kv->num - 1] = strsave(kv->key[kv->num - 1]);
	    kv->val[kv->num - 1] = strsave(kv->val[kv->num - 1]);
	    kv = read_keyvals(pmi_fds[rank], kv->num);
	    if (strcmp(kv->key[kv->num - 1], "endcmd") == 0) {
		--kv->num;
		break;
	    }
	}
	/* fall through to normal handling, but free them afterwards */

    } else if (strcmp(kv->key[0], "cmd") != 0)
	error("%s: hoping for \"cmd\" or \"mcmd\" as first key, got \"%s\"",
	      __func__, kv->key[0]);

    if (!strcmp(kv->val[0], "get")) {
	pmi_keypair_t *kp;
	int found;

	/* request: cmd=get kvsname=%s key=%s */
	if (kv->num != 3)
	    error("%s: in cmd=%s, expecting 3 keyvals, got %d", __func__,
	          kv->val[0], kv->num);
	if (strcmp(kv->key[1], "kvsname"))
	    error("%s: in cmd=%s, expecting key \"kvsname\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	if (strcmp(kv->key[2], "key"))
	    error("%s: in cmd=%s, expecting key \"key\" got %s",
	      __func__, kv->val[0], kv->key[2]);
	if (strcmp(kv->val[1], ks->kvsname))
	    error("%s: in cmd=%s, expecting kvsname %s, got %s", __func__,
	      kv->val[0], ks->kvsname, kv->val[1]);
	debug(2, "%s: cmd=%s kvsname=%s key=%s", __func__,
	      kv->val[0], kv->val[1], kv->val[2]);

	/* look it up */
	found = 0;
	list_for_each_entry(kp, &ks->keypair_list, list) {
	    if (!strcmp(kp->key, kv->val[2])) {
		found = 1;
		break;
	    }
	}

	/* response: get_result rc=%d [value=%s] */
	g = growstr_init();
	growstr_printf(g, "cmd=get_result rc=%d", found ? 0 : 1);
	if (found)
	    growstr_printf(g, " value=%s", kp->val);
	growstr_printf(g, "\n");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response cmd=get_result", __func__);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "put")) {
	pmi_keypair_t *kp;
	int found;

	/* request: cmd=put kvsname=%s key=%s value=%s */
	if (kv->num != 4)
	    error("%s: in cmd=%s, expecting 4 keyvals, got %d", __func__,
	          kv->val[0], kv->num);
	if (strcmp(kv->key[1], "kvsname"))
	    error("%s: in cmd=%s, expecting key \"kvsname\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	if (strcmp(kv->key[2], "key"))
	    error("%s: in cmd=%s, expecting key \"key\" got %s",
	      __func__, kv->val[0], kv->key[2]);
	if (strcmp(kv->key[3], "value"))
	    error("%s: in cmd=%s, expecting key \"value\" got %s",
	      __func__, kv->val[0], kv->key[3]);
	if (strcmp(kv->val[1], ks->kvsname) != 0)
	    error("%s: in cmd=%s, expecting value %s, got %s", __func__,
	      kv->val[0], ks->kvsname, kv->val[1]);
	debug(2, "%s: cmd=%s kvsname=%s key=%s val=%s", __func__,
	      kv->val[0], kv->val[1], kv->val[2], kv->val[3]);

	/*
	 * Clients seem to assume a duplicate put overrides the previous
	 * key.  Do so without bothering to check who set it etc.  And
	 * ignore the kvsname, assume all are the same.
	 */
	found = 0;
	list_for_each_entry(kp, &ks->keypair_list, list) {
	    if (!strcmp(kp->key, kv->val[2])) {
		found = 1;
		break;
	    }
	}

	if (found) {
	    /* replace old val */
	    free(kp->val);
	    kp->val = strsave(kv->val[3]);
	} else {
	    /* add new value */
	    kp = Malloc(sizeof(*kp));
	    kp->key = strsave(kv->val[2]);
	    kp->val = strsave(kv->val[3]);
	    list_add_tail(&kp->list, &ks->keypair_list);
	}

	/* response: cmd=put_result rc=0 */
	g = growstr_init();
	growstr_printf(g, "cmd=put_result rc=0\n");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response cmd=put_result rc=0", __func__);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "get_my_kvsname")) {
	/*
	 * PMI_KVS_Get_my_name from PMI_Get_id or PMI_Get_kvs_domain_id
	 */

	/* request: cmd=get_my_kvsname */
	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d",
	      __func__, kv->val[0], kv->num);
	debug(2, "%s: cmd=%s", __func__, kv->val[0]);

	/* response: cmd=my_kvsname kvsname=%s */
	g = growstr_init();
	growstr_printf(g, "cmd=my_kvsname kvsname=%s\n", ks->kvsname);
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response %s", __func__, g->s);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "get_ranks2hosts")) {
	/*
	 * PMI extension in Intel MPI 3.0
	 */

	/* request: cmd=get_ranks2hosts */
	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d",
		 __func__, kv->val[0], kv->num);
	debug(2, "%s: cmd=%s", __func__, kv->val[0]);

	g = ranks2hosts_lookup_or_build(spawn);

	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: write put_ranks2hosts", __func__);
	/* do not free it, cached for future tasks */

    } else if (!strcmp(kv->val[0], "get_universe_size")) {
	/*
	 * PMI_Get_universe_size used by sock but not shm.  Says how
	 * many processes total might be started, like with MPI_Spawn.
	 * XXX: This should be global across the name space, i.e. go to
	 * the concurrent master for the answer.
	 */
	int i, totcpu;

	/* request: cmd=get_universe_size */
	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d", __func__,
	          kv->val[0], kv->num);
	debug(2, "%s: cmd=%s", __func__, kv->val[0]);

	totcpu = 0;
	for (i=0; i<numnodes; i++)
	    totcpu += nodes[i].availcpu;

	/* response: cmd=universe_size size=%d */
	g = growstr_init();
	growstr_printf(g, "cmd=universe_size size=%d\n", totcpu);
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response %s", __func__, g->s);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "get_appnum")) {
	/*
	 * PMI_Get_appnum.  See 5.5.3 of the mpi-2.0 spec.
	 * XXX: should give different answers depending on how it
	 * was started, spawn versus spawn_multiple.  Going with 0
	 * for now and see if anything cares.
	 */

	/* request: cmd=get_appnum */
	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d", __func__,
	          kv->val[0], kv->num);
	debug(2, "%s: cmd=%s", __func__, kv->val[0]);

	/* response: cmd=appnum size=%d */
	g = growstr_init();
	growstr_printf(g, "cmd=appnum appnum=0\n");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response %s", __func__, g->s);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "getbyidx")) {
	/*
	 * Walk the "put" keypairs to return the requested index.  Happens
	 * after a spawn.
	 */
	pmi_keypair_t *kp;
	int i, idx;
	char *cq;

	/* request: cmd=getbyidx kvsname=%s idx=%d */
	if (kv->num != 3)
	    error("%s: in cmd=%s, expecting 3 keyval, got %d", __func__,
	          kv->val[0], kv->num);
	if (strcmp(kv->key[1], "kvsname"))
	    error("%s: in cmd=%s, expecting key \"kvsname\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	if (strcmp(kv->val[1], ks->kvsname) != 0)
	    error("%s: in cmd=%s, expecting kvsname %s, got %s", __func__,
	      kv->val[0], ks->kvsname, kv->val[1]);
	idx = strtoul(kv->val[2], &cq, 10);
	if (cq == kv->val[2])
	    error("%s: in cmd=%s, invalid value for idx \"%s\"",
	      __func__, kv->val[0], kv->val[2]);
	debug(2, "%s: cmd=%s kvsname=%s idx=%d", __func__,
	      kv->val[0], kv->val[1], idx);

	i = 0;
	list_for_each_entry(kp, &ks->keypair_list, list) {
	    if (i == idx)
		break;
	    ++i;
	}

	g = growstr_init();
	growstr_printf(g, "cmd=getbyidx_results ");
	if (&kp->list != &ks->keypair_list) {
	    /* response: cmd=getbyidx_results rc=0 nextidx=%d key=%s val=%s */
	    growstr_printf(g, "rc=0 nextidx=%d key=%s val=%s\n",
			   idx+1, kp->key, kp->val);
	} else {
	    /* response: cmd=getbyidx_results rc=1 reason=no_more_keyvals */
	    growstr_printf(g, "rc=1 reason=no_more_keyvals\n");
	}
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response %s", __func__, g->s);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "barrier_in")) {

	/* request: cmd=barrier_in, ch3:shm at least will do another barrier
	 * after shmem initialization */
	int i;

	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d", __func__,
	          kv->val[0], kv->num);
	debug(2, "%s: cmd=%s rank %d", __func__, kv->val[0], rank);
	if (pmi_barrier[rank] != 0)
	    error("%s: cmd=%s rank %d already in barrier", __func__,
	          kv->val[0], rank);
	pmi_barrier[rank] = 1;

	for (i=spawns[spawn].task_start; i<spawns[spawn].task_end; i++)
	    if (pmi_barrier[i] == 0)
		break;
	if (i == numtasks) {
	    /* release barrier */
	    debug(2, "%s: all entered barrier (spawn %d), release", __func__,
	          spawn);
	    g = growstr_init();
	    growstr_printf(g, "cmd=barrier_out\n");
	    for (i=spawns[spawn].task_start; i<spawns[spawn].task_end;
	         i++) {
		pmi_barrier[i] = 0;
		if (write_full(pmi_fds[i], g->s, g->len) < 0)
		    error_errno("%s: response cmd=barrier_out", __func__);
	    }
	    growstr_free(g);
	}

    } else if (!strcmp(kv->val[0], "finalize")) {

	/* request: cmd=finalize */
	if (kv->num != 1)
	    error("%s: in cmd=%s, expecting 1 keyval, got %d", __func__,
	          kv->val[0], kv->num);
	debug(2, "%s: cmd=%s", __func__, kv->val[0]);
	/* response: cmd=finalize_ack */
	g = growstr_init();
	growstr_printf(g, "cmd=finalize_ack\n");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: response cmd=barrier_out", __func__);
	growstr_free(g);

      finalize:
	close(pmi_fds[rank]);
	poll_del(pmi_fds[rank], rfs);
	pmi_fds[rank] = -1;
	/* checks if no more connections, including pmi, stdout, etc */
	maybe_exit_stdio();

    } else if (!strcmp(kv->val[0], "publish_name")) {

	/*
	 * XXX: these three name publishing routines must go across
	 * the concurrent boundary.  Communicate up to master who has
	 * a single database.
	 */
	published_name_t *pubname;

	/* request: cmd=publish_name service=%s port=%s */
	if (kv->num != 3)
	    error("%s: in cmd=%s, expecting 3 keyvals, got %d", __func__,
	          kv->val[0], kv->num);
	if (strcmp(kv->key[1], "service"))
	    error("%s: in cmd=%s, expecting key \"service\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	if (strcmp(kv->key[2], "port"))
	    error("%s: in cmd=%s, expecting key \"port\" got %s",
	      __func__, kv->val[0], kv->key[2]);
	debug(2, "%s: cmd=%s service=%s port=%s", __func__,
	      kv->val[0], kv->val[1], kv->val[2]);

	pubname = Malloc(sizeof(*pubname));
	pubname->service = strsave(kv->val[1]);
	pubname->port = strsave(kv->val[2]);
	list_add_tail(&pubname->list, &published_name_list);

	/* response: cmd=publish_result info=ok */
	g = growstr_init();
	growstr_printf(g, "cmd=publish_result info=ok\n");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: write cmd=publish_result", __func__);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "unpublish_name")) {

	published_name_t *pubname;
	int found;

	/* request: cmd=unpublish_name service=%s */
	if (kv->num != 2)
	    error("%s: in cmd=%s, expecting 2 keyvals, got %d", __func__,
	          kv->val[0], kv->num);
	if (strcmp(kv->key[1], "service"))
	    error("%s: in cmd=%s, expecting key \"service\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	debug(2, "%s: cmd=%s service=%s", __func__, kv->val[0], kv->val[1]);

	found = 0;
	list_for_each_entry(pubname, &published_name_list, list) {
	    if (!strcmp(pubname->service, kv->val[1])) {
		found = 1;
		break;
	    }
	}
	if (found) {
	    list_del(&pubname->list);
	    free(pubname->service);
	    free(pubname->port);
	    free(pubname);
	}

	/* response: cmd=unpublish_result info=ok */
	g = growstr_init();
	growstr_printf(g, "cmd=unpublish_result info=%s\n",
	               found ? "ok" : "unknown_service");
	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: write cmd=unpublish_result", __func__);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "lookup_name")) {

	published_name_t *pubname;
	int found;

	/* request: cmd=lookup_name service=%s */
	if (kv->num != 2)
	    error("%s: in cmd=%s, expecting 2 keyvals, got %d",
	      __func__, kv->val[0], kv->num);
	if (strcmp(kv->key[1], "service"))
	    error("%s: in cmd=%s, expecting key \"service\" got %s",
	      __func__, kv->val[0], kv->key[1]);
	debug(2, "%s: cmd=%s service=%s", __func__, kv->val[0], kv->val[1]);

	found = 0;
	list_for_each_entry(pubname, &published_name_list, list) {
	    if (!strcmp(pubname->service, kv->val[1])) {
		found = 1;
		break;
	    }
	}

	/* response: cmd=lookup_name info=ok port=%s */
	g = growstr_init();
	growstr_printf(g, "cmd=lookup_result info=");
	if (found)
	    growstr_printf(g, "ok port=%s\n", pubname->port);
	else
	    growstr_printf(g, "unknown_service\n");

	if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	    error_errno("%s: write cmd=lookup_result", __func__);
	growstr_free(g);

    } else if (!strcmp(kv->val[0], "spawn")) {

	char *cq;
	int kvoff, kvneed, i;

	/* number of procs for this spawn and the executable they run */
	int nprocs;
	const char *execname;

	/* inside a loop from _spawn_multiple, total number of
	 * eventual loop iterations, and 1..totspawns as it goes */
	int totspawns;
	int spawnssofar;

	/* args for this spawn */
	int argcnt;

	/* stored keyvals are the same across totspawns */
	int preput_num;

	/* infos are particular for this spawn */
	int info_num;

	const char **args, **infokeys, **infovals;

	debug(2, "%s: cmd=%s", __func__, kv->val[0]);

	g = growstr_init();

	/* request: mcmd=spawn nprocs=%d execname=%s totspawns=%d
	 *          spawnssofar=%d argcnt=%d preput_num=%d
	 *          preput_key_%d=%s preput_val_%d=%s info_num=%d
	 */
	kvneed = 8;
	if (kv->num < kvneed)
	    error("%s: in cmd=%s, expecting at least %d keyvals, got %d",
	      __func__, kv->val[0], kvneed, kv->num);

	kvoff = 1;
	if (strcmp(kv->key[kvoff], "nprocs"))
	    error("%s: in cmd=%s, expecting key \"nprocs\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	nprocs = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for nprocs \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	++kvoff;
	if (strcmp(kv->key[kvoff], "execname"))
	    error("%s: in cmd=%s, expecting key \"execname\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	execname = kv->val[kvoff];

	++kvoff;
	if (strcmp(kv->key[kvoff], "totspawns"))
	    error("%s: in cmd=%s, expecting key \"totspawns\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	totspawns = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for totspawns \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	++kvoff;
	if (strcmp(kv->key[kvoff], "spawnssofar"))
	    error("%s: in cmd=%s, expecting key \"spawnssofar\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	spawnssofar = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for spawnssofar \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	++kvoff;
	if (strcmp(kv->key[kvoff], "argcnt"))
	    error("%s: in cmd=%s, expecting key \"argcnt\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	argcnt = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for argcnt \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	/* need 1 more keyval for each arg */
	kvneed += argcnt;
	if (kv->num < kvneed)
	    error("%s: in cmd=%s (arg), expecting at least %d keyvals",
	      __func__, kv->val[0], kvneed);

	args = NULL;
	if (argcnt > 0)
	    args = Malloc(argcnt * sizeof(*args));
	for (i=0; i<argcnt; i++) {
	    growstr_zero(g);
	    growstr_printf(g, "arg%d", i+1);
	    ++kvoff;
	    if (strcmp(kv->key[kvoff], g->s))
		error("%s: in cmd=%s, expecting key \"%s\" got %s",
		  __func__, kv->val[0], g->s, kv->key[kvoff]);
	    args[i] = kv->val[kvoff];
	}

	++kvoff;
	if (strcmp(kv->key[kvoff], "preput_num"))
	    error("%s: in cmd=%s, expecting key \"preput_num\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	preput_num = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for preput_num \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	if (preput_num > 0) {
	    pmi_keypair_space_t *ksnext;

	    /* need 2 more keyvals for each preput */
	    kvneed += 2 * preput_num;
	    if (kv->num < kvneed)
		error("%s: in cmd=%s (preput), expecting at least %d keyvals",
		  __func__, kv->val[0], kvneed);

	    /* build new keypair space */
	    ksnext = keypair_space_get(curspawn + 1);

	    for (i=0; i<preput_num; i++) {
		pmi_keypair_t *kp;

		growstr_zero(g);
		growstr_printf(g, "preput_key_%d", i);
		++kvoff;
		if (strcmp(kv->key[kvoff], g->s))
		    error("%s: in cmd=%s, expecting key \"%s\" got %s",
		      __func__, kv->val[0], g->s, kv->key[kvoff]);

		growstr_zero(g);
		growstr_printf(g, "preput_val_%d", i);
		++kvoff;
		if (strcmp(kv->key[kvoff], g->s))
		    error("%s: in cmd=%s, expecting key \"%s\" got %s",
		      __func__, kv->val[0], g->s, kv->key[kvoff]);

		/* call "put" on these into the new keypair space */
		kp = Malloc(sizeof(*kp));
		kp->key = strsave(kv->val[kvoff-1]);
		kp->val = strsave(kv->val[kvoff]);
		list_add_tail(&kp->list, &ksnext->keypair_list);
	    }
	}

	++kvoff;
	if (strcmp(kv->key[kvoff], "info_num"))
	    error("%s: in cmd=%s, expecting key \"info_num\" got %s",
	      __func__, kv->val[0], kv->key[kvoff]);
	info_num = strtoul(kv->val[kvoff], &cq, 10);
	if (cq == kv->val[kvoff])
	    error("%s: in cmd=%s, invalid value for info_num \"%s\"",
	      __func__, kv->val[0], kv->val[kvoff]);

	/* need 2 more keyvals for each info */
	kvneed += 2 * info_num;
	if (kv->num < kvneed)
	    error("%s: in cmd=%s (info), expecting at least %d keyvals",
	      __func__, kv->val[0], kvneed);

	infokeys = NULL;
	infovals = NULL;
	if (info_num > 0) {
	    infokeys = Malloc(info_num * sizeof(*infokeys));
	    infovals = Malloc(info_num * sizeof(*infovals));
	}
	for (i=0; i<info_num; i++) {
	    growstr_zero(g);
	    growstr_printf(g, "info_key_%d", i);
	    ++kvoff;
	    if (strcmp(kv->key[kvoff], g->s))
		error("%s: in cmd=%s, expecting key \"%s\" got %s",
		  __func__, kv->val[0], g->s, kv->key[kvoff]);

	    growstr_zero(g);
	    growstr_printf(g, "info_val_%d", i);
	    ++kvoff;
	    if (strcmp(kv->key[kvoff], g->s))
		error("%s: in cmd=%s, expecting key \"%s\" got %s",
		  __func__, kv->val[0], g->s, kv->key[kvoff]);

	    infokeys[i] = kv->key[kvoff-1];
	    infovals[i] = kv->key[kvoff];
	}

	/* make sure no more args */
	if (kv->num != kvneed)
	    error("%s: in cmd=%s (done), expecting exactly %d keyvals",
	      __func__, kv->val[0], kvneed);

	/*
	 * Tell the parent to handle the spawn.  He'll tell us to set up
	 * ports, etc. as necessary elsewhere.
	 */
	++curspawn;
	stdio_msg_listener_spawn(rank, nprocs, execname, argcnt, args,
	                         info_num, infokeys, infovals);

	if (argcnt)
	    free(args);
	if (info_num) {
	    free(infokeys);
	    free(infovals);
	}

	/* will respond later once spawns happen; requesting task blocks */

    } else
	error("%s: unknown cmd %s", __func__, kv->val[0]);

    if (kv && !strcmp(kv->key[0], "mcmd")) {
	int i;
	for (i=0; i<kv->num; i++) {
	    free(kv->key[i]);
	    free(kv->val[i]);
	}
    }
}

void
pmi_send_spawn_result(int rank, int ok)
{
    growstr_t *g;

    /* response: cmd=spawn_result rc=%d */
    g = growstr_init();
    growstr_printf(g, "cmd=spawn_result rc=%d\n", ok);
    if (write_full(pmi_fds[rank], g->s, g->len) < 0)
	error_errno("%s: write cmd=spawn_result", __func__);
    growstr_free(g);
}

/*
 * Each PMI process is spawned with an environment variable with
 * hostname/port of a socket where it can reach the master.  Called
 * from start_tasks() or later from a spawn activity.
 */
int
prepare_pmi_startup_port(int *pmi_fd)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    *pmi_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (*pmi_fd < 0)
	error_errno("%s: socket", __func__);
    memset(&sin, 0, len);
    sin.sin_family = myaddr.sin_family;
    sin.sin_addr = myaddr.sin_addr;
    sin.sin_port = 0;
    if (bind(*pmi_fd, (struct sockaddr *)&sin, len) < 0)
	error_errno("%s: bind", __func__);
    if (getsockname(*pmi_fd, (struct sockaddr *) &sin, &len) < 0)
	error_errno("%s: getsockname", __func__);
    if (listen(*pmi_fd, 1024) < 0)
	error_errno("%s: listen", __func__);
    pmi_listen_port = ntohs(sin.sin_port);
    return pmi_listen_port;
}

