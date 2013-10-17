/*
 * rai.c - code specific to initializing MPICH/RAI for Cray Rapid Array
 * Interconnect in XD1, bought from OctigaBay.
 *
 * $Id: rai.c 418 2008-03-16 21:15:35Z pw $
 *
 * Copyright (C) 2005-8 Pete Wyckoff <pw@osc.edu>
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

#ifdef HAVE_POLL
#  include <sys/poll.h>
#endif

static int mport_fd;

/*
 * Each RAI process is spawned with environment variables which tell it its
 * place in the world, and give hostname/port of a socket where it can
 * reach the master.
 */
int
prepare_rai_startup_port(void)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

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
    if (listen(mport_fd, 1024) < 0)
	error_errno("%s: listen", __func__);
    return ntohs(sin.sin_port);
}

/*
 * Most of the code in this file is copied from ib.c.  It applies only
 * to their mpich-1.2.5/rai tree.  Note it is an unversioned protocol
 * like the old IB code used to be.  If they change it, we'll have to
 * guess around like in the old IB days.
 *
 * Each RAI process connects to our socket, then does four writes:
 *     int mpi_rank (from MPIRUN_RANK)
 *     int addrlen
 *     u8  address[addrlen]  (RAI particulars)
 * Then each expects to read back np * address[addrlen] corresponding
 * to the addresses of all of the processes, including itself.
 */
int
read_rai_startup_ports(void)
{
    char *address = 0;
    int address_size = 0;
    int i, flags, *fds;
    int ret = 0;

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

    if (cl_args->verbose)
	printf("%s: waiting for checkins\n", __func__);

    for (i=0; i<numtasks; i++) {
	int fd, rank, addrlen;

	/*
	 * Wait for a connection.
	 */
	for (;;) {
	    fd = accept(mport_fd, 0, 0);
	    if (fd >= 0)
		break;
	    if (errno != EAGAIN)
		error_errno("%s: accept iter %d", __func__, i);

	    if (poll_events_until_obit()) {
		close(mport_fd);
		ret = 1;
		goto out;
	    }
	    usleep(200000);
	}

	/*
	 * Read the entire address info for one process, concatenate them in
	 * process order, send the whole lot to back to each process.
	 */
	read_full(fd, &rank, sizeof(int));
	read_full(fd, &addrlen, sizeof(int));

	if (rank < 0 || rank >= numtasks)
	    error("%s: rank %d out of bounds [0..%d)", __func__,
	      rank, numtasks);
	/* rank checked in already? */
	if (fds[rank] != -1)
	    error("%s: rank %d checked in twice", __func__, rank);
	fds[rank] = fd;

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
		error(
		  "%s: wrong address size from rank %d, got %d, expected %d",
		  __func__, rank, addrlen, address_size);
	}

	read_full(fd, address + rank * address_size, address_size);
	if (cl_args->verbose)
	    printf("%s: rank %d checked in, %d left\n", __func__, rank,
	      numtasks-1-i);
    }

    /* close and forget this socket, no MPI_Abort handling */
    close(mport_fd);

    /*
     * Now send the information back to all of them and shut down.
     */
    for (i=0; i<numtasks; i++)
	if (write_full(fds[i], address, numtasks * address_size) < 0)
	    error_errno("%s: write addresses to rank %d", __func__, i);
    for (i=0; i<numtasks; i++)
	if (close(fds[i]) < 0)
	    error("%s: close socket to rank %d", __func__, i);

  out:
    free(address);
    free(fds);
    return ret;
}

