/*
 * p4.c - code specific to initializing MPICH/P4 (mpich version 1 that is)
 *
 * $Id: p4.c 326 2006-01-24 21:35:26Z pw $
 *
 * Copyright (C) 2000-5 Pete Wyckoff <pw@osc.edu>
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

static int mport_fd;

/*
 * See corresponding code in mpid/ch_p4/p4/lib/p4_utils.c:
 * put_execer_port().
 */
int
prepare_p4_master_port(void)
{
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    mport_fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (mport_fd < 0)
	error_errno("%s: socket", __func__);
    memset(&sin, 0, len);
    sin.sin_family = myaddr.sin_family;
    /*
     * Unfortunately we released mpich-1.2.4 with big master
     * assuming it will connect via loopback, restricting this use
     * only to cases where pbs node#0 hosts mpi task#0.  Until that is
     * fixed, leave sin_addr zeroed so that any interface can receive
     * the big master startup message.
     */
    /* sin.sin_addr = myaddr.sin_addr; */
    sin.sin_port = 0;
    if (bind(mport_fd, (struct sockaddr *)&sin, len) < 0)
	error_errno("%s: bind", __func__);
    if (getsockname(mport_fd, (struct sockaddr *) &sin, &len) < 0)
	error_errno("%s: getsockname", __func__);
    return ntohs(sin.sin_port);
}

int
read_p4_master_port(int *port)
{
    int cc, flags;
    int ret = 0;

    debug(1, "%s: waiting for port from master", __func__);
    *port = -1;

    /*
     * Poll for connection while checking if process died to avoid
     * hanging due to startup problems.
     */
    flags = fcntl(mport_fd, F_GETFL);
    if (flags < 0)
	error_errno("%s: get listen socket flags", __func__);
    if (fcntl(mport_fd, F_SETFL, flags | O_NONBLOCK) < 0)
	error_errno("%s: set listen socket nonblocking", __func__);

    for (;;) {
	cc = read(mport_fd, port, sizeof(*port));
	if (cc >= 0)
	    break;
	if (errno != EAGAIN)
	    error_errno("%s: read", __func__);

	/* check to see if any process died, and abort if so */
	if (poll_events_until_obit()) {
	    ret = 1;
	    goto out;
	}
	usleep(200000);
    }
    if (cc != sizeof(*port))
	error("%s: got %d bytes, expecting %d", __func__, cc,
	  (int) sizeof(*port));
  out:
    if (close(mport_fd) < 0)
	error_errno("%s: close", __func__);
    debug(1, "%s: got port %d", __func__, *port);
    return ret;
}

