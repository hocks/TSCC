/*
 * tv_attach.c - variables and routines for TotalView attachment.
 *
 * This code is only used for COMM_MPICH2_PMI.  Totalview attachment
 * for other MPI libraries is handled just by modifying command-line
 * arguments in start_tasks.c.
 * 
 * See: http://www-unix.mcs.anl.gov/mpi/mpi-debug/mpich-attach.txt
 *
 * Created: 02/2008 Frank Mietke <frank.mietke@s1998.tu-chemnitz.de>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>  /* nanosleep */
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "util.h"
#include "mpiexec.h"
#include "tv_attach.h"

static int tv_socket;

/*
 * Totalview will read this structure directly from our address space,
 * along with some other variables.
 * 
 * Changed the strings to const, hoping that it doesn't matter to totalview.
 */
typedef struct {
    const char *host_name;
    const char *executable_name;
    int pid;
} MPIR_PROCDESC;

MPIR_PROCDESC *MPIR_proctable = NULL;
int MPIR_proctable_size = 0;

volatile int MPIR_debug_state = 0;
volatile int MPIR_being_debugged = 0;
int MPIR_i_am_starter = 0;
int MPIR_partial_attach_ok = 0;

/*
 * This function is used by TotalView to detect a new event where is something
 * to do by the debugger.  Don't forget to set MPIR_debug_state to something
 * useful before calling this function.  Empty nanosleep in here is to make
 * sure this code is not optimized away---totalview needs to find code here.
 */
void MPIR_Breakpoint(void)
{
	struct timespec ts = { 0, 1000 };
	nanosleep(&ts, NULL);
}

/*
 * This does not play well with MPI_Comm_spawn.  Probably Totalview does
 * not support such a usage model anyway.
 */
int tv_startup(int ntasks)
{
    int rc;
    growstr_t *g;
    struct sockaddr_in tv_sockaddr;
    socklen_t tv_sockaddr_len = sizeof(tv_sockaddr);
    struct timespec ts = { 0, 20L * 1000 * 1000 };  /* 20 ms */

    MPIR_proctable = Malloc(ntasks * sizeof(*MPIR_proctable));

    /* Creating socket for pid exchange of remote processes */
    tv_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (tv_socket < 0)
	error_errno("%s: socket ", __func__);

    memset(&tv_sockaddr, 0, sizeof(tv_sockaddr));
    tv_sockaddr.sin_family = AF_INET;
    tv_sockaddr.sin_addr = myaddr.sin_addr;

    rc = bind(tv_socket, (struct sockaddr *) &tv_sockaddr, tv_sockaddr_len);
    if (rc)
	error_errno("%s: bind", __func__);
    rc = getsockname(tv_socket, (struct sockaddr *) &tv_sockaddr,
		     &tv_sockaddr_len);
    if (rc)
	error_errno("%s: getsockname", __func__);
    rc = listen(tv_socket, 32767);
    if (rc)
	error_errno("%s: listen", __func__);

    /* start totalview and tell it to attach to this mpiexec process */
    g = growstr_init();
    growstr_printf(g, "%s -e \"dattach mpiexec %d; dgo; "
		      "dassign MPIR_being_debugged 1\" &", tvname, getpid());
    system(g->s);
    growstr_free(g);

    /* wait for totalview to find us */
    while (!MPIR_being_debugged)
	nanosleep(&ts, NULL);

    return ntohs(tv_sockaddr.sin_port);
}

/*
 * This is called once for each task startup.  Wait until the started
 * task sends its pid, then enter that into the totalview table.  It
 * might be better to do this in parallel with a service... handler
 * like some other libraries have, but since this is only for the
 * debugging case, maybe it is not too terrible to be slow.
 */
void tv_accept_one(int n)
{
    int pid, ps1;
    char rpid[11];

    ps1 = accept(tv_socket, 0, 0);
    if (ps1 < 0)
	error_errno("%s: accept totalview pid", __func__);
    read_full(ps1, rpid, 10);
    close(ps1);
    rpid[10] = '\0';
    pid = atoi(rpid);
    MPIR_proctable[n].host_name = nodes[tasks[n].node].name;
    MPIR_proctable[n].executable_name = tasks[n].conf->exe;
    MPIR_proctable[n].pid = pid;
    MPIR_proctable_size++;
}

void tv_complete(void)
{
    const int MPIR_DEBUG_SPAWNED = 1;

    MPIR_debug_state = MPIR_DEBUG_SPAWNED;
    MPIR_Breakpoint();
    close(tv_socket);
}

