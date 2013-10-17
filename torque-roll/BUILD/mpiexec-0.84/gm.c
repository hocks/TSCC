/*
 * gm.c - code specific to initializing MPICH/GM or MPICH/MX
 *
 * $Id: gm.c 395 2006-11-29 15:22:26Z pw $
 *
 * Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "mpiexec.h"

#ifdef HAVE_POLL
#  include <sys/poll.h>
#endif

/*
 * Global shared with start_tasks.c
 */
int gmpi_fd[2];

typedef struct {
    int port;
    int board;
    unsigned int node;  /* mpich-gm2 will use _all_ 32 bits in here */
    int numanode;
    int pid;
    int remote_port;
} gmpi_info_t;
static gmpi_info_t *gmpi_info;

static int mpich_gm_version = -1;

/* state of all the sockets */
static int num_waiting_to_accept;  /* first accept all numtasks */
static int num_waiting_to_read;    /* then read all the numtasks */
#ifdef HAVE_POLL
static struct pollfd *pfs;
#else
static fd_set rfs;
static int fdmax;
#endif

static tm_event_t scatter_gm_startup_ports(void);

/*
 * GM or MX listening sockets startup.  gmpi_fd holds the fd on which the stdio
 * listener should listen for aborts, but it won't do so until we tell it to
 * start.  In fact, it won't even know which one until we figure out the
 * mpich-gm version number, then we tell it.  In vers 12510 the second is never
 * actually used by anything.
 */
void
prepare_gm_startup_ports(int gmpi_port[2])
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int flags, i;

    for (i=0; i<2; i++) {
	gmpi_fd[i] = socket(PF_INET, SOCK_STREAM, 0);
	if (gmpi_fd[i] < 0)
	    error_errno("%s: socket", __func__);
	if (gmpi_fd[i] == 0) {
	    /* Avoid using 0 since we must close it if it is connected
	     * to stdin, although we have no way to tell if it is when
	     * we go to do so in stdio_fork.
	     */
	    int newfd = dup(gmpi_fd[i]);
	    if (newfd < 0)
		error_errno("%s: dup to avoid fd 0", __func__);
	    close(gmpi_fd[i]);
	    gmpi_fd[i] = newfd;
	}
	memset(&sin, 0, len);
	sin.sin_family = myaddr.sin_family;
	sin.sin_addr = myaddr.sin_addr;
	sin.sin_port = 0;
	if (bind(gmpi_fd[i], (struct sockaddr *)&sin, len) < 0)
	    error_errno("%s: bind", __func__);
	if (getsockname(gmpi_fd[i], (struct sockaddr *) &sin, &len) < 0)
	    error_errno("%s: getsockname", __func__);
	gmpi_port[i] = ntohs(sin.sin_port);
	if (listen(gmpi_fd[i], 1024) < 0)
	    error_errno("%s: listen", __func__);
    }

    /*
     * Poll for connection while checking if process died to avoid
     * hanging due to gm startup problems.
     */
    flags = fcntl(gmpi_fd[0], F_GETFL);
    if (flags < 0)
	error_errno("%s: get listen socket flags", __func__);
    if (fcntl(gmpi_fd[0], F_SETFL, flags | O_NONBLOCK) < 0)
	error_errno("%s: set listen socket nonblocking", __func__);

    /* alloc */
    gmpi_info = Malloc(numtasks * sizeof(*gmpi_info));
    for (i=0; i<numtasks; i++)
	gmpi_info[i].port = -1;

#ifdef HAVE_POLL
    pfs = Malloc((numtasks+1) * sizeof(*pfs));
#else
    FD_ZERO(&rfs);
    fdmax = 0;
#endif

    num_waiting_to_accept = 0;  /* incremented on each call to service... */
    num_waiting_to_read = 0;
}

/*
 * Read a newly accepted socket.
 */
static void read_gm_one(int fd)
{
    int magic, id, port, board, numanode, pid, remote_port;
    int cc;
    unsigned int node;
    char s[1024];

    cc = read_until(fd, s, sizeof(s), ">>>", 0);
    if (cc < 0)
	error_errno("%s: read", __func__);
    if (cc == 0)
	error("%s: eof", __func__);
    if (sscanf(s, "<<<%d:%d:%d:%d:%u:%d:%d::%d>>>", &magic, &id,
               &port, &board, &node, &numanode, &pid, &remote_port) == 8) {
	/* format used by mpich-gm-1.2.5..10 and later */
	if (mpich_gm_version < 0) {
	    mpich_gm_version = 12510;
	    debug(1, "%s: mpich gm or mx version %d", __func__,
	          mpich_gm_version);
	}
	else if (mpich_gm_version != 12510)
	    error("%s: expecting version 12150 got %d", __func__,
	          mpich_gm_version);
    } else if (sscanf(s, "<<<%d:%d:%d:%d:%u:%d>>>", &magic, &id, &port,
                      &board, &node, &pid) == 6) {
	/* format used by mpich-gm-1.2.4..8a */
	if (mpich_gm_version < 0) {
	    mpich_gm_version = 1248;
	    debug(1, "%s: mpich gm or mx version %d", __func__,
	          mpich_gm_version);
	}
	else if (mpich_gm_version != 1248)
	    error("%s: expecting version 1248 got %d", __func__,
	          mpich_gm_version);
	numanode = 0;
	remote_port = 0;
    } else {
	error("%s: read gmpi_port#1 <<<...>>> string not recognized",
	      __func__);
    }
    if (magic != atoi(jobid))
	error("%s: received bad magic %d", __func__, magic);
    if (id < 0 || id >= numtasks)
	error("%s: received id %d out of range", __func__, id);
    if (gmpi_info[id].port != -1)
	error("%s: received duplicate response for id %d", __func__, id);
    gmpi_info[id].port = port;
    gmpi_info[id].board = board;
    gmpi_info[id].node = node;
    gmpi_info[id].numanode = numanode;
    gmpi_info[id].remote_port = remote_port;
    gmpi_info[id].pid = pid;
    debug(1, "%s: rank %d in, %d + %d left", __func__, id,
	  num_waiting_to_read + num_waiting_to_accept,
	  numtasks - numspawned);
    if (cl_args->verbose >= 2) {
	printf("%s: id %d port %d board %d node_id 0x%08x\n",
	  __func__, id, port, board, node);
	printf("  numanode %d pid %5d remote_port %5d\n",
	  numanode, pid, remote_port);
    }
    close(fd);
}

/*
 * Two big steps here.  Listen for info from all processes, then put it
 * together, and send it out when requested.
 *
 * Stay listening to port#2 for abort messages later in stdio handler.
 */
int
read_gm_startup_ports(void)
{
    int flags;

    debug(1, "%s: waiting for checkin: %d to accept, %d to read", __func__,
      num_waiting_to_accept, num_waiting_to_read);

    /*
     * Watch the sockets until all clients have been accepted and sent
     * their data.
     */
    while (num_waiting_to_accept + num_waiting_to_read > 0) {
	int ret = service_gm_startup(0);
	if (ret < 0)
	    return 1;
	if (ret == 0)  /* did nothing, sleep a bit */
	    usleep(200000);
    }

    /*
     * Put listen socket back in blocking, in case this is an old gm
     * version that uses it for abort.
     */
    flags = fcntl(gmpi_fd[0], F_GETFL);
    if (flags < 0)
	error_errno("%s: get socket flags", __func__);
    if (fcntl(gmpi_fd[0], F_SETFL, flags & ~O_NONBLOCK) < 0)
	error_errno("%s: set listen socket blocking", __func__);
    close(gmpi_fd[0]);

    return scatter_gm_startup_ports();
}

/*
 * Second step:  send all this info back out as they request new
 * connections on the second part.  No I do not understand why a
 * second connection is necessary, but that's gm.
 */
static int
scatter_gm_startup_ports(void)
{
    growstr_t *g;
    int i, fret = 0;

    g = growstr_init();
    growstr_append(g, "[[[");
    for (i=0; i<numtasks; i++) {
	if (mpich_gm_version == 1248)
	    /* scanner in gmpi code uses %d everywhere */
	    growstr_printf(g, "<%d:%d:%u>", gmpi_info[i].port,
	      gmpi_info[i].board, gmpi_info[i].node);
	else if (mpich_gm_version == 12510)
	    /* scanner in gmpi code uses %u everywhere */
	    growstr_printf(g, "<%d:%d:%u:%d>", gmpi_info[i].port,
	      gmpi_info[i].board, gmpi_info[i].node, gmpi_info[i].numanode);
	else
	    error("%s: unknown mpich_gm_version %d", __func__,
	      mpich_gm_version);
    }
    growstr_append(g, "|||");

    for (i=0; i<numtasks; i++) {
	int id, j, cc, fd = 0;
	growstr_t *h;
	
	if (mpich_gm_version == 1248) {
	    /*
	     * Wait for some node to connect.  Figure out which it is and
	     * send it its localized data.
	     */
	    int magic;
	    char s[2048];

	    fd = accept(gmpi_fd[1], 0, 0);
	    if (fd < 0)
		error_errno("%s: accept gmpi_port#2 iter %d", __func__, i);
	    /* damn they make this hard.  cannot just search for <->, must
	     * search for one of those after : */
	    cc = read_until(fd, s, sizeof(s), ":", 0);
	    if (cc < 0)
		error_errno("%s: read gmpi_port#2 iter %d", __func__, i);
	    if (cc == 0)
		error("%s: eof in gmpi_port#2 iter %d", __func__, i);
	    cc = read_until(fd, s, sizeof(s), "<->", strstr(s, ":") - s);
	    if (cc < 0)
		error_errno("%s: read gmpi_port#2 iter %d", __func__, i);
	    if (cc == 0)
		error("%s: eof in gmpi_port#2 iter %d", __func__, i);
	    if (sscanf(s, "<->%d:%d<->", &magic, &id) != 2)
		error("%s: read gmpi_port#2 iter %d not 2 values", __func__, i);
	    if (magic != atoi(jobid))
		error("%s: received bad magic %d", __func__, magic);
	    if (id < 0 || id >= numtasks)
		error("%s: received id %d out of range", __func__, id);
	    if (gmpi_info[id].port == -1)
		error("%s: received duplicate response for id %d", __func__,
		  id);
	    gmpi_info[id].port = -1;

	} else if (mpich_gm_version == 12510) {
	    /*
	     * This one more difficult.  Master must resolve each and every
	     * remote node and connect _to_ it, to send the info.
	     */
	    struct sockaddr_in sin;
	    struct hostent *he;
	    int one = 1;
	    int flags, ret;

	    he = gethostbyname(nodes[tasks[i].node].name);
	    if (!he)
		error("%s: gethostbyname cannot resolve %s", __func__,
		  nodes[tasks[i].node].name);
	    if (he->h_length != sizeof(sin.sin_addr))
		error("%s: gethostbyname returns %d-byte addresses, hoped %d",
		  __func__, he->h_length, (int) sizeof(sin.sin_addr));
	    memset(&sin, 0, sizeof(sin));
	    sin.sin_family = (unsigned short) he->h_addrtype;
	    memcpy(&sin.sin_addr, he->h_addr_list[0], sizeof(sin.sin_addr));
	    sin.sin_port = htons(gmpi_info[i].remote_port);

	    fd = socket(PF_INET, SOCK_STREAM, 0);
	    if (fd < 0)
		error_errno("%s: socket", __func__);
	    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
		error_errno("%s: setsockopt SO_REUSEADDR", __func__);
	    flags = fcntl(fd, F_GETFL);
	    if (flags < 0)
		error_errno("%s: get connect socket flags", __func__);
	    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		error_errno("%s: set connect socket nonblocking", __func__);

	    /* must do this asynchronously */
	    ret = connect(fd, (struct sockaddr *) &sin, sizeof(sin));
	    if (ret == 0)
		goto connected;
	    if (errno != EINPROGRESS)
		error_errno("%s: connect remote iter %d", __func__, i);
	    for (;;) {
#ifdef HAVE_POLL
		struct pollfd pfs;
		const char *const syscall = "poll";
		pfs.fd = fd;
		pfs.events = POLLOUT;
		pfs.revents = 0;
		ret = poll(&pfs, 1, 200);
		
#else
		struct timeval tv = { 0, 200000 };
		fd_set wfs;
		const char *const syscall = "select";
		FD_ZERO(&wfs);
		FD_SET(fd, &wfs);
		ret = select(fd+1, 0, &wfs, 0, &tv);
#endif
		if (ret == 1) {
		    int f;
		    socklen_t flen = sizeof(f);
		    ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &f, &flen);
		    if (ret < 0)
			error_errno("%s: getsockopt SO_ERROR after connect",
			  __func__);
		    if (f != 0) {
			errno = f;
			error_errno("%s: connect (%s, %d) failed",
			  __func__, nodes[tasks[i].node].name, sin.sin_port);
		    }
		    break;
		}
		if (ret < 0)
		    error_errno("%s: %s waiting connect iter %d",
		      __func__, syscall, i);
		if (ret != 0)
		    error("%s: really not expecting %s to return %d",
		      __func__, syscall, ret);

		/* check to see if any process died, and abort if so */
		if (poll_events_until_obit()) {
		    close(fd);
		    fret = 1;
		    goto out;
		}
	    }
	  connected:
	    id = i;
	    /* go back to blocking for the write_full() */
	    flags = fcntl(fd, F_GETFL);
	    if (flags < 0)
		error_errno("%s: get connect socket flags #2", __func__);
	    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
		error_errno("%s: set connect socket blocking", __func__);
	}

	h = growstr_init();
	growstr_append(h, g->s);  /* h = "[[[<global stuff>|||" */

	/*
	 * Search for SMP nodes and send the list of ids using that node.
	 * Note that it is not sufficient to compare tm_node_id values
	 * since PBS hands out one per each "virtual CPU".
	 */
	for (j=0; j<numtasks; j++) {
	    if (!strcmp(nodes[tasks[id].node].name, nodes[tasks[j].node].name))
		growstr_printf(h, "<%d>", j);
	}
	growstr_append(h, "]]]");
	cc = write_full(fd, h->s, h->len);
	if (cc < 0)
	    error_errno("%s: write gmpi_port#2 iter %d", __func__, i);
	close(fd);
	growstr_free(h);
    }  /* for i in numtasks */

  out:
    close(gmpi_fd[1]);
    growstr_free(g);
    free(gmpi_info);
#ifdef HAVE_POLL
    free(pfs);
#endif

    /* signal stdio that it should pay attention to the abort_fd now */
    if (fret == 0)
	stdio_msg_parent_say_abort_fd(mpich_gm_version == 1248 ? 1 : 0);

    return fret;
}

/*
 * Check for incoming connections and read-readiness of existing sockets
 * to keep process checking moving along.  Called after every process
 * startup to make sure no previously started tasks time out in their
 * connect phase.
 *
 * Returns negative if error, 0 if did nothing, >0 if did something.
 */
int
service_gm_startup(int created_new_task)
{
    int fd, ret = 0;
    int numspawned_entry = numspawned;

    if (created_new_task)
	++num_waiting_to_accept;

    debug(2, "%s: %snew task, now accept wait %d", __func__,
          created_new_task ? "" : "no ", num_waiting_to_accept);

    /*
     * If anything died, give up.
     */
    ret = poll_events_until_obit();
    if (ret || numspawned_entry != numspawned) {
	close(gmpi_fd[0]);
	ret = -1;
	goto out;
    }

    /*
     * If there's a new connection to accept, do so and add it to the
     * poll list for later reading.
     */
    fd = accept(gmpi_fd[0], 0, 0);
    if (fd == -1) {
	if (errno != EAGAIN)
	    error_errno("%s: accept", __func__);
    } else {
	int flags;

	/*
	 * Explictly turn off nonblocking.  Some OSes (Mac and perhaps its
	 * BSD ancestors) inherit socket flags from the listening one to
	 * the newly accepted one.  Others (like linux) reset all F_GETFL
	 * flags to default.  This should be harmless even if O_NONBLOCK
	 * was already turned off.
	 */
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
	    error_errno("%s: get new socket flags", __func__);
	if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
	    error_errno("%s: set new socket blocking", __func__);

	--num_waiting_to_accept;
	++ret;
	debug(2, "%s: accepted fd %d, accept wait %d", __func__, fd,
	  num_waiting_to_accept);

	/* add to poll list */
#ifdef HAVE_POLL
	pfs[num_waiting_to_read].fd = fd;
	pfs[num_waiting_to_read].events = POLLIN;
	pfs[num_waiting_to_read].revents = 0;
#else
	FD_SET(fd, &rfs);
	if (fd > fdmax)
	    fdmax = fd;
#endif
	++num_waiting_to_read;
    }

    /*
     * Poll for something to read.
     */
    {
#ifdef HAVE_POLL
    int k;
    int pret = poll(pfs, num_waiting_to_read, 0);
    if (pret < 0)
	error_errno("%s: poll", __func__);
    for (k=0; k<num_waiting_to_read; k++) {
	if (pfs[k].revents & (POLLIN | POLLHUP)) {
	    fd = pfs[k].fd;
	    pfs[k] = pfs[num_waiting_to_read-1];  /* bubble up */
	    --k;
#else  /* }} */
    struct timeval tv = { 0, 0 };
    fd_set trfs = rfs;
    int sret = select(fdmax+1, &trfs, 0, 0, &tv);
    if (sret < 0)
	error_errno("%s: select", __func__);
    for (fd=0; fd <= fdmax; fd++) {
	if (FD_ISSET(fd, &trfs)) {
	    FD_CLR(fd, &rfs);
#endif
	    --num_waiting_to_read;
	    ++ret;
	    debug(2, "%s: reading fd %d, read wait %d", __func__, fd,
	          num_waiting_to_read);
	    read_gm_one(fd);
	}
    }
    }

  out:
    return ret;
}

