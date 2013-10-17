/*
 * ib.c - code specific to initializing MPICH/IB aka MVAPICH
 *
 * $Id: ib.c 421 2008-04-21 14:29:46Z pw $
 *
 * Copyright (C) 2003-8 Pete Wyckoff <pw@osc.edu>
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
#include "mpiexec.h"
#include "pmgr_collective_mpirun.h"

#ifdef HAVE_POLL
#  include <sys/poll.h>
#endif

/* listening socket */
static int mport_fd;

/* parameters for initial data read from each process */
static int version = -1;
static int version_as_read = -1;  /* to check for mixed version only */
static char *address = 0;
static int address_size = 0;
static char *pids = 0;
static int pids_size = 0;
static int phase = 0;  /* for two-phase versions 5 and 6 */
static int *hca_type = NULL;
static int hca_first_rank = -1;  /* to determine if homogeneous or not */
static int is_homogeneous = 1;   /* until proven otherwise */

/* state of all the sockets */
static int num_waiting_to_accept;  /* first accept all numtasks */
static int num_waiting_to_read;    /* then read all the numtasks */
static int *fds;
#ifdef HAVE_POLL
static struct pollfd *pfs;
static int *pfsmap;
#else
static fd_set rfs;
static int fdmax;
#endif


/*
 * Each IB process is spawned with environment variables which tell it its
 * place in the world, and give hostname/port of a socket where it can
 * reach the master.
 */
int
prepare_ib_startup_port(int *fd)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int i, flags;

    mport_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (mport_fd < 0)
	error_errno("%s: socket", __func__);
    memset(&sin, 0, len);
    sin.sin_family = myaddr.sin_family;
    sin.sin_addr = myaddr.sin_addr;
    sin.sin_port = 0;
    if (bind(mport_fd, (struct sockaddr *)&sin, len) < 0)
	error_errno("%s: bind", __func__);
    if (getsockname(mport_fd, (struct sockaddr *) &sin, &len) < 0)
	error_errno("%s: getsockname", __func__);
    if (listen(mport_fd, 32767) < 0)
	error_errno("%s: listen", __func__);
    *fd = mport_fd;

    fds = Malloc(numtasks * sizeof(*fds));
    for (i=0; i<numtasks; i++)
	fds[i] = -1;
    /*
     * Poll for connection while checking if process died to avoid
     * hanging due to gm startup problems.
     */
    flags = fcntl(mport_fd, F_GETFL);
    if (flags < 0)
	error_errno("%s: get listen socket flags", __func__);
    if (fcntl(mport_fd, F_SETFL, flags | O_NONBLOCK) < 0)
	error_errno("%s: set listen socket nonblocking", __func__);

#ifdef HAVE_POLL
    pfs = Malloc((numtasks+1) * sizeof(*pfs));
    pfsmap = Malloc((numtasks+1) * sizeof(*pfsmap));
#else
    FD_ZERO(&rfs);
    fdmax = 0;
#endif

    num_waiting_to_accept = 0;  /* incremented on each call to service... */
    num_waiting_to_read = 0;

    return ntohs(sin.sin_port);
}

/*
 * Read the entire address info for one process.  Since there
 * exists the non-versioned 0.9.2, read a bunch of ints first
 * then try to guess at what version we may be dealing with.
 *
 * Non-versioned 0.9.2 says:
 *    rank     # 0..np-1
 *    addrlen  # 32 + np * 10
 *    addrs[]  # %010d lid, (np-1) %010d qpn, %32s hostname
 *
 * Versioned 0.9.2 says:
 *    version  # 1
 *    rank     # 0..np-1
 *    addrlen  # 32 + np * 8
 *    addrs[]  # %010d lid, (np-1) %010d qpn, %32s hostname
 *
 * >= 0.9.4 say:
 *    version  # 1 or 2
 *    rank     # 0..np-1
 *    addrlen  # np * 4 + 4
 *    addrs[]  # np * <4-byte binary qpn>..., <4-byte hostid>
 *
 * >= 0.9.5 (with at least patch 112) say:
 *    version  # 3
 *    rank     # 0..np-1
 *    addrlen  # np * 4 + 4
 *    addrs[]  # np * <4-byte binary qpn>..., <4-byte hostid>
 *    pidlen   # 4-byte number of characters in pid
 *    pid[]    # binary pid
 *
 * In the MVAPICH source, this version is called "pmgr_version".
 *
 * Version 1:
 *   Read all addrs[], concatenate them in process order, send the
 *   whole lot to back to each process.
 *
 * Verison 2:
 *   Uses a binary format.  The incoming addrs[] array is a list
 *   of the qpns to be used by the other processes to talk to this
 *   node, except addrs[id] is his lid, not a qpn.  And tacked on
 *   the end is a 4-byte hostid, actually the IPv4 address of the
 *   node, used to find other tasks on the same SMP.
 *
 *   We send back 3 * np 4-byte ints in the following format:
 *     0..np-1      : lids of each task
 *     np..2*np-1   : personalized qp info
 *     2*np..3*np-1 : hostids
 *
 * Verison 3:
 *   Same as 2 with addition of pid array.  We send back the
 *   entire array of pids (unpersonalized) after the addresses array.
 *
 * Version 5:
 *   Added another phase, with socket close/reaccept between the two.  This
 *   could be very bad for scalability, but is necessary to support multiple
 *   NICs per node, and multiple network paths, according to OSU.
 *   First phase distributes hostids:
 *     version   # 5
 *     rank      # 0..np-1
 *     hostidlen # 4 bytes
 *     hostid    # <hostidlen> bytes
 *   Write back entire hostid[] array.
 *   Close fds, go to phase 2.  At each accept, gather:
 *     rank      # 0..np-1
 *     addrlen   # 4 bytes, could be 0
 *     addrs[]   # <addrlen> bytes
 *     pidlen    # 4 bytes
 *     pids[]    # <pidlen> bytes
 *   Write back personalized out_addrs[] and full pids[].
 *
 * Version 6:
 *   It's 2 phase based like version 5 with a major change that the socket
 *   to each task stays open across phases, and some data tweaks.
 *  First phase distributes hostids and hca_types:
 *    version    # 6
 *    rank       # 0..np-1
 *    hostidlen  # 4 bytes
 *    hostid     # <hostidlen> bytes
 *    hca_type   # 4 bytes
 *  Write back is_homogeneous (4 bytes)  and the entire hostid[] array. 
 *  Keep the fds open, go to phase 2 and gather:
 *    addrlen    # 4 bytes, could be 0
 *    addrs[]    # <addrlen> bytes
 *    pidlen     # 4 bytes
 *    pids[]     # <pidlen> bytes
 *  Write back personalized out_addrs[] and full pids[].
 *
 *  Version 8:
 *    Completely rewritten (by mvapich) opcode-based handshake protocol
 *    using custom collective operations to improve startup.
 *    Only version of protocol and rank of processes are received
 *    in the old way.  Everything else is handled in pmgr_collective*
 *
 * Return negative on error, or new rank number for success.
 */
static int read_ib_one(int fd)
{
    int testvers, rank, addrlen;
    int non_versioned_092;
    int j, ret = -1;
    pid_t pidlen;

    if ((version == 5 || version == 6) && phase == 1) {
	/* no version again on second phase */
	testvers = version;
    } else {
	if (read_full_ret(fd, &testvers, sizeof(int)) != sizeof(int))
	    goto out;
    }

    if (version == 6 && phase == 1) {
	/* since socket stays open, tasks do not send rank, so figure it out
	 * from the fd the slow way.  This can not fail.
	 */
	for (j = 0; j < numtasks; j++)
	    if (fds[j] == fd) {
		rank = j;
		break;
	    }
    } else {
	if (read_full_ret(fd, &rank, sizeof(int)) != sizeof(int))
	    goto out;
    }
    if (testvers < 8)
	    if (read_full_ret(fd, &addrlen, sizeof(int)) != sizeof(int))
		    goto out;

    non_versioned_092 = 0;
    if (rank == 32 + numtasks * 8) {
	/*
	 * Likely we are dealing with a non-versioned 0.9.2, but this
	 * might be a legitimate checkin of process 42 in a versioned
	 * scheme, e.g.  If this is a non-versioned 0.9.2, the "addrlen"
	 * we just read will be the first 4 bytes of addrs[] actually,
	 * the lid number in 10-digit decimal.  Since it is in ASCII,
	 * the first four characters are in the range 0x30..0x39.
	 * If this number, interpreted as binary, really happened to be
	 * a valid addrlen, it would correspond to a numtasks of over 200
	 * million for v2 or 80 million for v1.  Let's hope we can phase
	 * out 0.9.2 support by the time clusters become that big.  :)
	 */
	char *cp = (char *) &addrlen;

	for (j=0; j<4; j++)
	    if (!(cp[j] >= '0' && cp[j] <= '9')) break;
	if (j == 4) {
	    addrlen = rank;
	    rank = testvers;
	    testvers = 1;
	    non_versioned_092 = 1;
	}
    }

    if (version == -1) {
	version = testvers;
	version_as_read = testvers;
	if (!(version == 1 || version == 2 || version == 3 || version == 5 ||
	      version == 6 || version == 8)) {
	    warning("%s: protocol version %d not known, but might still work",
	            __func__, version);
	    version = 8;  /* guess the latest still works */
	}
	debug(1, "%s: version %d startup%s", __func__, version,
	      non_versioned_092 ? " (unversioned)" : "");
    } else {
	if (version_as_read != testvers)
	    error("%s: mixed version executables (%d and %d), no hope",
	          __func__, version_as_read, testvers);
    }
    if (rank < 0 || rank >= numtasks)
	error("%s: rank %d out of bounds [0..%d)", __func__, rank, numtasks);
    if (version == 8) {
	ret = rank;
	goto out;
    }

    if (!address) {
	/*
	 * Allocate once for all processes, entire array, same size each.
	 * Round up to 4-byte boundary since version 2 will treat these
	 * as 4-byte integers.
	 */
	address = Malloc(addrlen * numtasks + 4);
	address = (char *)(((unsigned long) address + 3) & ~3);
	address_size = addrlen;
    } else {
	if (addrlen != address_size)
	    error("%s: wrong address size from rank %d, got %d, expected %d",
	          __func__, rank, addrlen, address_size);
    }

    /* New to protocol version 6 of MVAPICH-1.0 */
    if (version == 6 && hca_type == NULL)
	    hca_type = malloc(numtasks * sizeof(int));

    if (non_versioned_092) {
	/* push back the bit we accidentally read in guessing the version */
	for (j=0; j<4; j++)
	    address[rank * address_size + j] = 0x30;
	if (read_full_ret(fd, address + rank * address_size + 4,
	                  address_size - 4) != address_size - 4)
	    goto out;
    } else {
	if (read_full_ret(fd, address + rank * address_size, address_size)
	             != address_size)
	    goto out;
    }

    if (version == 6 && phase == 0) {
	if (read_full_ret(fd, &hca_type[rank], sizeof(int)) != sizeof(int))
	    goto out;
	if (hca_first_rank == -1)
	    hca_first_rank = rank;
	if (hca_type[hca_first_rank] != hca_type[rank])
	    is_homogeneous = 0;
    }

    if (version == 3 || ((version == 5 || version == 6) && phase == 1)) {
	read_full(fd, &pidlen, sizeof(pidlen));
	if (!pids) {
	    pids_size = pidlen;
	    pids = Malloc(pids_size * numtasks);
	} else {
	    if (pidlen != pids_size)
		error(
		  "%s: wrong pid size from rank %d, got %d, expected %d",
		  __func__, rank, pidlen, pids_size);
	}
	if (pids_size > 0)
	    if (read_full_ret(fd, &pids[rank * pids_size], pids_size)
	                      != pids_size)
		goto out;
    }

    /* success */
    ret = rank;

  out:
    return ret;
}

/*
 * Each IB process connects to our socket, then does four writes:
 *     int version (missing in mvapich-0.9.2)
 *     int mpi_rank (from MPIRUN_RANK)
 *     int addrlen
 *     u8  address[addrlen]  (IB particulars)
 * Then each expects to read back np * address[addrlen] corresponding
 * to the addresses of all of the processes, including itself.
 * After this exchange, still sit around waiting for one more
 * operation, a barrier, after all the QPs are up.
 *
 * One more complication is that the contents of address[] must be
 * rearranged for each process in a particular way, for version 2.
 *
 * Never actually close the listening socket, as that is where a process
 * will call when it needs to cause an MPI_Abort later.
 *
 * Returns 0 if okay, 1 if an obit happened while waiting for connections.
 *
 * Finalize the process started earlier and poked periodically by
 * service_ib_startup().
 */
int
read_ib_startup_ports(void)
{
    int i, j, flags;
    int numleft;
    int ret = 0;

next_phase:
    debug(1, "%s: waiting for checkin: %d to accept, %d to read", __func__,
      num_waiting_to_accept, num_waiting_to_read);

    /*
     * Watch the sockets until all clients have been accepted and sent
     * their data.
     */
    while (num_waiting_to_accept + num_waiting_to_read > 0) {
	ret = service_ib_startup(0);
	if (ret < 0) {
	    ret = 1;
	    goto out;
	}
	if (ret == 0)  /* did nothing, sleep a bit */
	    usleep(200000);
    }

    /*
     * Now send the information back to all of them.
     */
    if (version == 1) {
	for (i=0; i<numtasks; i++) {
	    if (write_full(fds[i], address, numtasks * address_size) < 0)
		error_errno("%s: write addresses to rank %d", __func__, i);
	}
    } else if (version == 2 || version == 3) {
	int outsize = 3 * numtasks * sizeof(int);
	int *outaddrs = Malloc(outsize);
	int *inaddrs = (int *) (unsigned long) address;
	int inaddrs_size = address_size / sizeof(int);
	/* fill in the common information first: lids, hostids */
	for (i=0; i<numtasks; i++)
	    outaddrs[i] = inaddrs[i*inaddrs_size + i];
	for (i=0; i<numtasks; i++)
	    outaddrs[2*numtasks+i] = inaddrs[i*inaddrs_size + numtasks];
	/* personalize the array with qp info for each */
	for (i=0; i<numtasks; i++) {
	    for (j=0; j<numtasks; j++)
		outaddrs[numtasks+j] = inaddrs[j*inaddrs_size + i];
	    if (write_full(fds[i], outaddrs, outsize) < 0)
		error_errno("%s: write addresses to rank %d", __func__, i);
	}
	free(outaddrs);
	if (version == 3) {
	    for (i=0; i<numtasks; i++) {
		if (write_full(fds[i], pids, pids_size * numtasks) < 0)
		    error_errno("%s: write pids to rank %d", __func__, i);
	    }
	    free(pids);
	}
    } else if (version == 5 || version == 6) {
	if (phase == 0) {
	    /* These are actually the hostids, in mvapich parlance.  Next
	     * phase will be the personalized addresses. */
	    for (i=0; i<numtasks; i++) {
		if (version == 6)
		    if (write_full(fds[i], &is_homogeneous, sizeof(int)) < 0)
			error_errno("%s: write homogeneous flag to rank %d",
				    __func__, i);
		if (write_full(fds[i], address, numtasks * address_size) < 0)
		    error_errno("%s: write addresses to rank %d", __func__, i);
	    }
	    phase = 1;
	    if (version == 5) {
		for (i=0; i<numtasks; i++) {
		    close(fds[i]);
		    fds[i] = -1;
		}
		num_waiting_to_accept = numtasks;  /* will reconnect */
	    } else {
		/* Hackity hack.  Put these back on the ready-to-read list. */
		for (i=0; i<numtasks; i++) {
#ifdef HAVE_POLL
		    pfs[i].fd = fds[i];
		    pfs[i].events = POLLIN;
		    pfs[i].revents = 0;
#else
		    FD_SET(fds[i], &rfs);
#endif
		}
		num_waiting_to_read = numtasks;  /* socks stay open */
	    }
	    address_size = 0;
	    free(address);
	    address = NULL;
	    if (version == 6) {
		free(hca_type);
		hca_type = NULL;
	    }
	    goto next_phase;
	} else if (phase == 1) {
	    /*
	     * Very similar to version 3, but with -1 for i == j in
	     * outaddrs.  Not sure if that matters.
	     */
	    int outsize = 3 * numtasks * sizeof(int);
	    int *outaddrs = Malloc(outsize);
	    int *inaddrs = (int *) (unsigned long) address;
	    int inaddrs_size = address_size / sizeof(int);
	    /* fill in the common information first: lids, hostids */
	    for (i=0; i<numtasks; i++)
		outaddrs[i] = inaddrs[i*inaddrs_size + i];
	    for (i=0; i<numtasks; i++)
		outaddrs[2*numtasks+i] = inaddrs[i*inaddrs_size + numtasks];
	    /* personalize the array with qp info for each */
	    for (i=0; i<numtasks; i++) {
		for (j=0; j<numtasks; j++)
		    outaddrs[numtasks+j] = inaddrs[j*inaddrs_size + i];
		outaddrs[numtasks + i] = -1;
		if (write_full(fds[i], outaddrs, outsize) < 0)
		    error_errno("%s: write addresses to rank %d", __func__, i);
	    }
	    free(outaddrs);
	    for (i=0; i<numtasks; i++) {
		if (write_full(fds[i], pids, pids_size * numtasks) < 0)
		    error_errno("%s: write pids to rank %d", __func__, i);
	    }
	    free(pids);
	} else
	    error("%s: programmer error, unknown version %d phase %d",
		  __func__, version, phase);
    } else if (version == 8) {
	/*
	 * For version 8 of MVAPICH startup protocol, call pmgr_processops()
	 * to handle the remaining communication with MPI processes.
	 */
	ret = pmgr_processops(fds, numtasks);
    } else
	error("%s: programmer error, unknown version %d", __func__, version);

    /*
     * Put listen socket back in blocking, and give it to the stdio listener.
     */
    flags = fcntl(mport_fd, F_GETFL);
    if (flags < 0)
	error_errno("%s: get socket flags", __func__);
    if (fcntl(mport_fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
	error_errno("%s: set listen socket blocking", __func__);
    close(mport_fd);
    stdio_msg_parent_say_abort_fd(0);

    if (version == 8)
	goto out;

    /*
     * Finally, implement a simple barrier.  Use a select loop to avoid
     * hanging on a sequential read from #0 which is always quite busy and
     * slow to respond.
     */
    debug(1, "%s: barrier start", __func__);

#ifdef HAVE_POLL
    for (i=0; i<numtasks; i++) {
	pfs[i].fd = fds[i];
	pfs[i].events = POLLIN;
	pfs[i].revents = 0;
	pfsmap[i] = i;  /* get from pfs index to fds index */
    }
#else
    FD_ZERO(&rfs);
    fdmax = 0;
    for (i=0; i<numtasks; i++) {
	if (fds[i] > fdmax)
	    fdmax = fds[i];
	FD_SET(fds[i], &rfs);
    }
#endif

    numleft = numtasks;
    while (numleft > 0) {
#ifdef HAVE_POLL
	int k;
	const char *const syscall = "poll";
	ret = poll(pfs, numleft, 200);
#else
	struct timeval tv = { 0, 200000 };
	const char *const syscall = "select";
	fd_set trfs = rfs;
	ret = select(fdmax+1, &trfs, 0, 0, &tv);
#endif

	if (ret < 0)
	    error_errno("%s: barrier %s", __func__, syscall);

#ifdef HAVE_POLL
	for (k=0; k<numleft; k++) {
	    if (pfs[k].revents & (POLLIN | POLLHUP)) {
		i = pfsmap[k];
		/* bubble up */
		pfs[k] = pfs[numleft-1];
		pfsmap[k] = pfsmap[numleft-1];
		--k;
#else  /* }} */
	for (i=0; i<numtasks; i++) {
	    if (FD_ISSET(fds[i], &trfs)) {
		FD_CLR(fds[i], &rfs);
#endif
		if (read_full_ret(fds[i], &j, sizeof(int)) != sizeof(int)) {
		    ret = 1;
		    goto out;
		}
		if (j != i)
		    error("%s: barrier expecting rank %d, got %d",
		      __func__, i, j);
		--numleft;
		debug(3, "%s: barrier read rank %d, %d left", __func__, i,
		  numleft);
	    }
	}

	/* check to see if any process died; abort if so */
	if (poll_events_until_obit()) {
	    ret = 1;
	    goto out;
	}
    }

    for (i=0; i<numtasks; i++) {
	if (write_full(fds[i], &i, sizeof(int)) != sizeof(int)) {
	    warning("%s: writing barrier to rank %d failed", __func__, i);
	    ret = 1;
	    goto out;
	}
	if (close(fds[i]) < 0)
	    error("%s: close socket to rank %d", __func__, i);
    }
    debug(1, "%s: barrier done", __func__);
    ret = 0;

  out:
#ifdef HAVE_POLL
    free(pfs);
    free(pfsmap);
#endif
    free(fds);
    if (address)
	free(address);
    return ret;
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
service_ib_startup(int created_new_task)
{
    int fd, rank, ret = 0;
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
	close(mport_fd);
	ret = -1;
	goto out;
    }

    /*
     * If there's a new connection to accept, do so and add it to the
     * poll list for later reading.
     */
    fd = accept(mport_fd, 0, 0);
    if (fd == -1) {
	if (errno != EAGAIN)
	    error_errno("%s: accept", __func__);
    } else {
	int flags;

	/* explicitly mark newly accepted sockets as blocking */
	flags = fcntl(fd, F_GETFL);
	if (flags < 0)
	    error_errno("%s: get socket flags", __func__);
	if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)
	    error_errno("%s: set socket blocking", __func__);

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
	    rank = read_ib_one(fd);

	    if (rank < 0) {
		close(fd);
		ret = rank;
		goto out;  /* let obit poll catch it later */
	    }
		
	    /*
	     * Sanity check that the same rank isn't trying to check in
	     * again.  But don't do this for the second phase in version
	     * 6 because the fds were left open after phase 0.
	     */
	    if (!(version == 6 && phase == 1)) {
		if (fds[rank] != -1)
		    error("%s: rank %d checked in twice", __func__, rank);
		fds[rank] = fd;
	    }
	    debug(1, "%s: rank %d in, %d + %d left", __func__, rank,
	          num_waiting_to_read + num_waiting_to_accept,
		  numtasks - numspawned);
	}
    }
    }

  out:
    return ret;
}

