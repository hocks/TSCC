/*
 * psm.c - code specific to initializing MPICH PSM
 *
 * $Id: psm.c 362 2007-04-25 19:58:04Z pw $
 *
 * Copyright (C) 2007-8 QLogic Corp
 * Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 *
 *
 * MPICH/PSM Startup protocol.
 *
 * Excerpt from version 0.8 of the protocol.  This protocol is maintained at
 * QLogic under bug 10383.
 *
 * Only the base protocol (Simple form) is documented here.
 *
 * This document describes how external spawners can integrate with
 * InfiniPath-MPI liked MPI applications by implementing the mpirun protocol
 * employed to distribute endpoint (or rank) information at startup.  The
 * mpirun protocol is available in its simple form or a more featured mode.
 * The simple form implements the minimal amount of communication necessary to
 * successfully spawn MPI-1 jobs.  The full-featured mode adds protocol flags
 * (or protocol options) which can be selectively implemented by spawners.  In
 * both modes, however, it is assumed that external spawners provide at least
 * the following:
 *
 *   * Remotely execute a process linked against InfiniPath MPI.
 *   * Forwarding some environment variables set in the process run from which
 *     the spawner is invoked. These include environment variables that begin
 *     with the prefix INFINIPATH_, IPATH_, PSM_ and MPI_ (for best operation,
 *     the spawner should also forward LD_LIBRARY_PATH).
 *   * Set and forward the MPI_SPAWNER environment variable according the the
 *     mpirun protocol explained below.
 *
 *
 * MPI_SPAWNER= <proto> <proto_params>
 * <proto> is a uint16_t with the protocol revision (currently only 0)
 *
 * For proto=0, <proto_params> are the following:
 *   <flags> <spawner_host> <spawner_port> <spawner_jobid> \
 *           <numranks> <myrank> <numlocalranks> <mylocalrank> \
 * 	     <timeout> <uuid>
 *
 * <proto_flags>   protocol options (explained below)
 * <spawner_host>  Spawner Host or ipv4 IP address
 * <spawner_port>  Spawner port
 * <spawner_jobid> Spawner jobid (unique job number can be spawner's PID)
 * <numranks>      Number of MPI ranks (COMM_WORLD size)
 * <myrank>        MPI Rank number (COMM_WORLD rank for that process)
 * <numlocalranks> Number of MPI ranks on the node
 * <mylocalrank>   Rank of that process on the node (0..<numlocalranks>-1)
 * <timeout>       How long to wait for reply to EPID_INFO_SINGLE message,
 *                 if < 1, InfiniPath MPI waits forever
 * <uuid>	   16-byte unpacked UUID of the form
 * 		   <NNNNNNNN-NNNN-NNNN-NNNN-NNNNNNNNNNNN>, example is
 * 		   b7087326-1bc9-4125-a818-d79a070c7657
 * 		   UUID can be obtained from the PSM library as
 * 		   psm_generate_uuid, or from /usr/bin/uuidgen or simply cast
 * 		   into that form by reading 16 bytes from /dev/random
 *
 *
 * The basic protocol with proto_flags == 0 is the following:
 *
 * 0. The spawner uses whatever remote execution mechanism it desires to start
 *    an InfiniPath MPI linked process but always ensures that MPI_SPAWNER is
 *    set in the environment with the parameters above.
 *
 * 1. Each MPI process reads MPI_SPAWNER and connects back to the spawner's
 *    <spawner_host> and <spawner_port> through TCP and sends its port
 *    information.  The message format is the following, and each field is in
 *    big endian format.
 *
 *   <proto_version> uint16_t
 *   <message_type>  uint16_t  (message_type is EPID_INFO_SINGLE at value 1)
 *   <message_len>   uint32_t  (length of remaining bytes to read)
 *   <mpi_rank>      uint32_t  (my mpi rank)
 *   <proto_flags>   uint32_t
 *   <spawner_jobid> uint32_t
 *   <my_process_id> uint32_t  (procid obtained with getpid())
 *   <psm_epid>      uint64_t
 *   [error message] string (optional and of length <message_len> -
 * 		      len (<proto_flags>+<spawner_jobid>+<myrank>+<psm_epid>))
 *
 *   * <psm_epid> is the epid obtained from psm_ep_open when opening a port.
 *   * <psm_epid> is equal to 0 If the port open fails.
 *   * [error message] is optional and only filled in if <psm_epid> == 0.
 *   * When replying with <proto_flags>, InfiniPath MPI only sets the protocol
 *     flags <proto_flags> that were originally set in MPI_SPAWNER (the spawner
 *     will never see protocol flags it does not intend to support).
 *   * If the MPI_SPAWNER format is wrong, InfiniPath MPI will not connect back
 *     to the spawner and will instead print an error message on stderr and
 *     exit.
 *   * InfiniPath MPI will, by default, advertise some protocol flags but it is
 *     not necessary for the spawner to implement them all (or any of them, for
 *     that matter).  Discussion on protocol flags below.
 *
 * 2. The spawner gathers EPID_INFO_SINGLE from all the nodes and eventually
 *    broadcasts an EPID_INFO_GLOBAL message to every node.  In gathering
 *    EPID_INFO_SINGLE messages, the spawner must do the following:
 *    1. Verify that <spawner_jobid> matches its jobid.
 *    2. Reduce <proto_flags> with a bitwise AND operator so as to support only
 *       the common set of protocol flags on each rank.
 *    3. Keep track of the association of <rank> to <psm_epid> and <ip_addr>.
 *    4. If <psm_epid> == 0, the spawner can print the error message supplied
 *       in the EPID_INFO_SINGLE message and either exit or communicate back to
 *       the nodes that some ranks could not get a port.
 *
 *    Once <rank> messages are received, the spawner composes a
 *    EPID_INFO_GLOBAL message and sends it to each rank.
 *
 *    <proto_version> uint16_t
 *    <message_type>  uint16_t  (message_type is EPID_INFO_GLOBAL at value 2)
 *    <message_len>   uint32_t  (length of remaining bytes to read)
 *    <mpi_rank>      uint32_t  (rank of intended receiver)
 *    <proto_flags>   uint32_t
 *    <num_epids>     uint32_t
 *    <psm_epids>     [] uint64_t
 *    <ip_addrs>      [] uint32_t
 *
 *
 * 3. After sending the EPID_INFO_SINGLE message, each InfiniPath MPI rank
 *    waits until <timeout> to receive the EPID_INFO_GLOBAL message from the
 *    spawner.
 *
 *    If any of the following conditions are met, InfiniPath MPI prints an
 *    error message, closes the socket and exits:
 *    * If <proto_version> is not an implemented protocol version.
 *    * If <timeout> seconds occur before EPID_INFO_GLOBAL is received
 *    * If any <psm_epids>[i] is 0
 *    * If <proto_flags> == 0, InfiniPath MPI assumes that the spawner
 *      implements the minimal spawner protocol of exchanging EPID information
 *      and closes the socket.  It is assumed that in these cases, the spawner
 *      employs an out-of-band mechanism to ensure that error conditions in MPI
 *      or other abnormal behavior does not lead to stray processes.
 *
 *
 * Supported Protocol flags (or features)
 * (complete extended features documentation available from QLogic)
 *
 * If the protocol flags are non-zero, the spawner *must* include support for
 * the EPID_SHUTDOWN message:
 *
 *    EPID_SHUTDOWN
 *    This message can be used either from spawner->library or
 *    library->spawner.
 *    * When sent by the library, the spawner should interpret the message to
 *      mean that the MPI library has exited.
 *
 *    * When sent by the spawner, the MPI library interprets the message as an
 *      unexpected request to quit.
 *
 *    The spawner can use EPID_SHUTDOWN messages to control shutting down MPI
 *    processes.
 *    <proto_version> uint16_t  (ignored post connection, here for consistency)
 *    <message_type>  uint16_t  (message_type is EPID_SHUTDOWN at value 5)
 *    <message_len>   uint32_t  (length of remaining bytes to read)
 *    <mpi_rank>      uint32_t  (in spawner->library case, mpi rank of intended
 * 			         receiver.  In opposite case, rank of sender)
 *    <spawner_jobid> uint32_t  (job id of the spawner.  Can be used to verify
 *                               the shutdown message is for a known job.  In
 *                               the connected EPID_PING case, this is
 *                               unnecessary, but when ASYNC_SHUTDOWN is
 *                               supported this
 *                               prevents spoofing the spawner.)
 *    <exittype>      uint16_t  (Where the exit came from)
 *    <exitcode>      uint16_t  (Exit code, where 0 is a successful exit)
 *    [error message] string    (Opt. string determined to be present based on
 * 			         <message_len>)
 *
 *    The MPI library implements the EPID_SHUTDOWN message as part of an
 *    atexit handler.  If the MPI application internally aborts, calls _exit or
 *    has abnormal behaviour that can cause the atexit handler to not be
 *    executed, the spawner is still expected to correctly handle job
 *    termination (in this case, the socket will be closed).
 *
 *    Multiple types of exits are distinguished by the 'exittype' argument to
 *    EPID_SHUTDOWN.  Supported types are:
 *
 *    EXIT_MPI_FINALIZE    (exittype = 0) : Standard MPI exit
 *    EXIT_MPI_NO_FINALIZE (exittype = 1) : Process exit, but no MPI_Finalize
 *                                          called
 *    EXIT_MPI_ABORT       (exittype = 2) : MPI_Abort called by user process
 *    EXIT_LIB_ABORT       (exittype = 3) : MPI runtime library initiated abort
 *    EXIT_EPID_TIMEOUT    (exittype = 4) : Remote process timed out
 * 		   		            for EPID_INFO_GLOBAL.
 *    EXIT_SOCKET_ERR      (exittype = 5) : Exit due to sockets error
 *    EXIT_QUIESCENCE      (exittype = 6) : Exit due to quiescence
 *
 *    When receiving the EPID_SHUTDOWN message from mpirun, the MPI library
 *    will exit with the supplied exit code and print the error message to
 *    stderr if present.
 *
 *    (skip flags 0x1 and 0x2, not documented here)
 *
 *    Protocol flag ASYNC_SHUTDOWN (0x4). Spawner supports receiving the
 *    EPID_SHUTDOWN message by having the library reconnect to its
 *    spawner_host:spawner_port.  This flag is ignored if EPID_PING is set.
 *    An example client of this is OSC's mpiexec, which uses the PBS provided
 *    TMS interface to replace most of the functionality defined by EPID_PING,
 *    but requires sockets notification of MPI_Abort.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <endian.h>
#include <byteswap.h>
#include "mpiexec.h"

#ifdef HAVE_POLL
#  include <sys/poll.h>
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
#  define ntohu64(x) (x)
#  define htonu64(x) (x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#  define ntohu64(x) __bswap_64(x)
#  define htonu64(x) __bswap_64(x)
#endif

#define EPID_INFO_GLOBAL 2

struct psm_mpspawn_hdr {
    uint16_t proto;
    uint16_t message_type;
    uint32_t message_len;
    uint32_t mpi_rank;
};

static int psm_fd;
static uint64_t *epids = NULL;

/* state of all the sockets */
static int num_waiting_to_accept;  /* first accept all numtasks */
static int num_waiting_to_read;    /* then read all the numtasks */

#ifdef HAVE_POLL
static struct pollfd *pfs;
#else
static fd_set rfs;
static int fdmax;
#endif

static int *fds;

static void scatter_psm_lids(void);

static uint32_t psm_lookup_ip(const char *name)
{
    uint32_t ret;
    struct hostent *he;
    
    he = gethostbyname(name);
    if (!he)
	error("%s: gethostbyname cannot resolve %s", __func__, name);
    if (he->h_length != sizeof(ret))
	error("%s: gethostbyname returns %d-byte addresses but expected "
	      "%d-byte addresses", __func__, he->h_length, (int) sizeof(ret));
    memcpy(&ret, he->h_addr_list[0], he->h_length);
    return ret;
}

/*
 * Each PSM process is spawned with environment variables that tell it its
 * place in the world, and give hostname/port of a socket where it can
 * reach the master.
 */
int prepare_psm_startup_port(int *fd)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    int i, flags;

    psm_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (psm_fd < 0)
	error_errno("%s: socket", __func__);
    memset(&sin, 0, len);
    sin.sin_family = myaddr.sin_family;
    sin.sin_addr = myaddr.sin_addr;
    sin.sin_port = 0;
    if (bind(psm_fd, (struct sockaddr *) &sin, len) < 0)
	error_errno("%s: bind", __func__);
    if (getsockname(psm_fd, (struct sockaddr *) &sin, &len) < 0)
	error_errno("%s: getsockname", __func__);
    if (listen(psm_fd, 32767) < 0)
	error_errno("%s: listen", __func__);
    *fd = psm_fd;

    fds = Malloc(numtasks * sizeof(*fds));
    for (i=0; i<numtasks; i++)
	fds[i] = -1;

    /*
     * Poll for connection while checking if process died to avoid
     * hanging due to gm startup problems.
     */
    flags = fcntl(psm_fd, F_GETFL);
    if (flags < 0)
	error_errno("%s: get listen socket flags", __func__);
    if (fcntl(psm_fd, F_SETFL, flags | O_NONBLOCK) < 0)
	error_errno("%s: set listen socket nonblocking", __func__);

#ifdef HAVE_POLL
    pfs = Malloc((numtasks+1) * sizeof(*pfs));
#else
    FD_ZERO(&rfs);
    fdmax = 0;
#endif

    num_waiting_to_accept = 0;  /* incremented on each call to service... */
    num_waiting_to_read = 0;

    return ntohs(sin.sin_port);
}

static int read_psm_one(int fd)
{
    int rank;
    struct psm_mpspawn_hdr hdr;
    uint64_t *u64ptr;
    uint32_t len;
    char *data;

    if (read_full_ret(fd, &hdr, sizeof(hdr)) != sizeof(hdr))
	error_errno("%s: read", __func__);

    len = ntohl(hdr.message_len);
    data = Malloc(len);

    if (read_full_ret(fd, data, len) != (int) len) {
	free(data);
	error_errno("%s: read", __func__);
    }

    rank = ntohl(hdr.mpi_rank);

    if (!epids) {
	epids = Malloc(sizeof(uint64_t) * numtasks);
	memset(epids, 0, sizeof(uint64_t) * numtasks);
    }
    if (epids[rank] != 0)
	error("%s: duplicate epids %llx at rank %d", __func__,
	      (unsigned long long) epids[rank], rank);

    u64ptr = (uint64_t *) (data + 3 * sizeof(uint32_t));
    epids[rank] = ntohu64(*u64ptr);

    free(data);
    return rank;
}

/*
 * Two big steps here.  Listen for info from all processes, then put it
 * together, and send it out when requested.
 *
 * Stay listening to port#2 for abort messages later in stdio handler.
 */
int read_psm_startup_ports(void)
{
    debug(1, "%s: waiting for checkin: %d to accept, %d to read", __func__,
	  num_waiting_to_accept, num_waiting_to_read);

    /*
     * Watch the sockets until all clients have been accepted and sent
     * their data.
     */
    while (num_waiting_to_accept + num_waiting_to_read > 0) {
	int ret = service_psm_startup(0);
	if (ret < 0)
	    return 1;
	if (ret == 0)  /* did nothing, sleep a bit */
	    usleep(200000);
    }

    scatter_psm_lids();
    return 0;
}

/*
 * Second step:  send all this info back out as they request new
 * connections on the second part.
 */
static void scatter_psm_lids(void)
{
    int i, err, len;

    /* EPID_INFO_GLOBAL message sent back to each node
     *  0 <proto_version> uint16_t
     *  2 <message_type>  uint16_t  (EPID_INFO_GLOBAL=2)
     *  4 <message_len>   uint32_t  (length of remaining bytes to read)
     *  8 <mpi_rank>      uint32_t  (rank of intended receiver)
     * 12 <proto_flags>   uint32_t  (4 for disconnect-after-info-global)
     * 16 <num_epids>     uint32_t  (numtasks in mpiexec)
     * 20 <psm_epids>     [] uint64_t
     * .. <ip_addrs>      [] uint32_t
     */
    uint16_t *einfo_ptype;
    uint32_t *einfo_hdr;
    uint64_t *einfo_epids;
    uint32_t *einfo_ipaddrs;
    uint8_t *msg;

    /*
     * Create the message template for each node, we update <rank> in
     * the template before sending out the message to each node.
     */
    len = 4 /* ptype */ + 16 /* hdr */ +
	    numtasks * (8 /* for epids */ + 4 /* for ipaddrs */);
    msg = Malloc(len);
    einfo_ptype = (uint16_t *) msg;
    einfo_ptype[0] = htons(0);
    einfo_ptype[1] = htons(EPID_INFO_GLOBAL);

    einfo_hdr = (uint32_t *) (einfo_ptype + 2);
    einfo_hdr[0] = htonl(len - 8 /* length after <message_len> */);
    /* einfo_hdr[1] contains rank, updated below */
    einfo_hdr[2] = htonl(4);  /* protocol extension 4 */
    einfo_hdr[3] = htonl(numtasks);

    einfo_epids = (uint64_t *) (einfo_hdr + 4);
    einfo_ipaddrs = (uint32_t *) (einfo_epids + numtasks);

    for (i=0; i<numtasks; i++) {
	einfo_epids[i] = htonu64(epids[i]);
	einfo_ipaddrs[i] = htonl(psm_lookup_ip(nodes[tasks[i].node].name));
    }

    /*
     * Update rank before sending message to each task
     */
    for (i=0; i<numtasks; i++) {
	einfo_hdr[1] = htonl(i); /* update rank before write */
	err = write_full(fds[i], msg, len);
	if (err < 0)
	    error_errno("%s: write psm_port iter %d", __func__, i);
	close(fds[i]);
    }

    free(msg);

#ifdef HAVE_POLL
    free(pfs);
#endif

    /*
     * Signal stdio that it should pay attention to the abort_fd now.
     * This is an index into the gmpi_fd array (we want 0), not the
     * actual fd.
     */
    stdio_msg_parent_say_abort_fd(0);
}

/*
 * Check for incoming connections and read-readiness of existing sockets
 * to keep process checking moving along.  Called after every process
 * startup to make sure no previously started tasks time out in their
 * connect phase.
 *
 * Returns negative if error, 0 if did nothing, >0 if did something.
 */
int service_psm_startup(int created_new_task)
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
	close(psm_fd);
	ret = -1;
	goto out;
    }

    /*
     * If there's a new connection to accept, do so and add it to the
     * poll list for later reading.
     */
    fd = accept(psm_fd, 0, 0);
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
	    int rank = read_psm_one(fd);
	    fds[rank] = fd;
	}
    }

out:
    return ret;
}
