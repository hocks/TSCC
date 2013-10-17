/*
 * stdio.c - handle standard streams for processes
 *
 * $Id: stdio.c 418 2008-03-16 21:15:35Z pw $
 *
 * Copyright (C) 2000-8 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>      /* sun location */
#include <sys/signal.h>  /* linux location */
#include <arpa/inet.h>
#include "mpiexec.h"

#ifdef HAVE_POLL
#  include <sys/poll.h>
#endif

/* pid of the stdio handler so parent can kill it */
static int pid = 0;

/* max allowed by system, we'll store room for them all */
static int maxfd;

typedef enum { NONE = -1, IN = 0, OUT = 1, ERR = 2, BOGUS = 3 } fd_which_t;

/*
 * Actual and expected number of stdin/out/err connectees.
 * Magic number 3 just enumerates stdin + stdout + stderr, this will
 * appear frequently below.
 */
static int connected[3], expected[3];

/*
 * Listener fds accepting new connections, and local port number where
 * litstening.  port[i] < 0 means invalid for all fds.
 */
static int listener[3];
static int listener_port[3];

/*
 * Keep track of the aggregate streams 0,1,2 in case they get closed
 * mid-run, or were never opened.  Don't want to go writing/reading
 * on some other random socket that happened to get opened onto 0,1,2.
 */
static int aggregate[3];
static int aggregate_in_ready;  /* if it is in the poll set */

/*
 * To try to avoid mixing output from different processes on the same
 * line, keep track of the last fd which used an aggregate stream.
 * Data must be printed no later than flush_time after it arrived.
 */
static int last_user[3];
static const struct timeval flush_time = { 1, 0 };

/*
 * To read or write between the parent process and stdio process.
 * Used by various parent functions.
 */
int pipe_with_stdio;

/*
 * Structure to track the state of all remote process sockets.
 * Note that there is no correlation between one of these sockets
 * and a particular hostname, but we keep the source address gathered
 * during accept() although there is no connection between that and
 * the hostnames.  A host lookup would be necessary.
 */
typedef struct {
    fd_which_t which;  /* mux this fd onto which aggregate stream */
    growstr_t *buf;    /* collect partial output waiting for \n or timeout */
    int next;          /* linked list from last user of stream to waiters */
    struct timeval tv; /* time input was generated */
    struct sockaddr_in sin;  /* source address of connection */
} fd_state_t;
static fd_state_t *fds;

/* global set of readables */
#ifdef HAVE_POLL
static struct pollfd *pfs;  /* array of poll descriptors */
static int *pfsmap;         /* map from fd to entry in pfs */
static int pfsnum;
static fd_set rfs;          /* empty unused argument for prettier code */
#else
static fd_set rfs;          /* used locally, passed to pmi routines sometimes */
#endif

/* extra fd where MPI_Abort callers will connect in mpich-gm */
static int abort_fd_array[2];
static int abort_fd_used = 0;
static int abort_fd = -1;

static int build_listener(int *port);
static void do_child(void) ATTR_NORETURN;
static void goodbye_from_parent(int sig);
static void listen_abort_fd(int abort_fd_index);
static void catch_alarm(int sig);
static void walk_buf_list(fd_which_t which);

/*
 * Define to sleep() in the child, and avoid alarm() at the end
 * so it can be debugged.
 */
#undef DEBUG_FORK

/*
 * Debugging functions: int->string for which names, fd_state dump.
 */
static const char *
which_name(fd_which_t i)
{
    /* -1, 0, 1, 2, 3... */
    static const char *names[] = { "NONE", "IN", "OUT", "ERR", "???" };
    if (i < -1 || i > 2)
	i = BOGUS;
    return names[i+1];
}

static void
dump_fd_state(void)
{
    int i;

    if (fds && cl_args->verbose >= 3)
	for (i=0; i<maxfd; i++)
	    if (fds[i].buf && fds[i].buf->len)
		debug(3, "%s: buf on %d state %s next %d has \"%s\"", __func__,
		  i, which_name(fds[i].which), fds[i].next, fds[i].buf->s);
}

/*
 * Two sets of fd set manipulation functions, for readables only.
 * One for poll() that scales to larger fd counts, and another for good
 * old select() for compatibility.  Major thanks to Alex Korobka for
 * implemeting the poll() versions.  Note that not all OSes have poll,
 * Apple Mac Darwin OSX being a notable one to lack this feature.
 */
#ifdef HAVE_POLL
#if 0
static void dump_pfsmap(const char *where)
{
    int i, maxfd;

    printf("%s: %s: pfsnum %d\n", __func__, where, pfsnum);
    printf("%s: pfs[].fd =", __func__);
    maxfd = 0;
    for (i=0; i<pfsnum; i++) {
	printf(" %d", pfs[i].fd);
	if (pfs[i].fd > maxfd)
	    maxfd = pfs[i].fd;
    }
    printf("\n");
    printf("%s: pfsmap[] = ", __func__);
    for (i=0; i<=maxfd; i++)
	printf(" %d", pfsmap[i]);
    printf("\n");
}
#endif

void poll_set(int fd, fd_set *fds ATTR_UNUSED)
{
    int n = pfsmap[fd];

    if (n >= 0)
	error("%s: fd %d already set", __func__, fd);
    if (pfsnum == maxfd)
	error("%s: out of fd space at %d", __func__, pfsnum);
    pfs[pfsnum].fd = fd;
    pfs[pfsnum].events = POLLIN;
    pfs[pfsnum].revents = 0;  /* since event loop may look at ..pfsnum */
    pfsmap[fd] = pfsnum;
    ++pfsnum;
}

static int poll_isset(int fd, fd_set *fds ATTR_UNUSED)
{
    int n = pfsmap[fd];

    if (n < 0)
	error("%s: fd %d not in poll map", __func__, fd);
    return (pfs[n].revents & (POLLIN | POLLHUP));
}

static void poll_clr(int fd, fd_set *fds ATTR_UNUSED)
{
    int n = pfsmap[fd];

    if (n < 0)
	error("%s: fd %d not in poll map", __func__, fd);
    pfs[n].revents &= ~(POLLIN | POLLHUP);
}

void poll_del(int fd, fd_set *fds ATTR_UNUSED)
{
    int n = pfsmap[fd];

    if (n < 0)
	error("%s: fd %d not in poll map", __func__, fd);
    /* throw away this fd */
    pfsmap[fd] = -1;
    --pfsnum;
    /* bubble up the last one into its hole to preserve a contiguous space
     * for poll() */
    if (n != pfsnum) {
	pfs[n] = pfs[pfsnum];  /* move last entry into the hole */
	pfsmap[pfs[n].fd] = n; /* update reverse mapping */
    }
}

#else  /* not HAVE_POLL below */

void poll_set(int fd, fd_set *fds)   { FD_SET(fd, fds); }
static int  poll_isset(int fd, fd_set *fds) { return FD_ISSET(fd, fds); }
static void poll_clr(int fd, fd_set *fds)   { FD_CLR(fd, fds); }
void poll_del(int fd, fd_set *fds)   { FD_CLR(fd, fds); }

#endif  /* HAVE_POLL */


/*
 * Main entry point:  fork a process to handle process streams.
 * The fds for stdin,stdout,stderr were already remembered early
 * on by stdio_notice_streams().  Everything else will get
 * closed except for one pipe shared acrossr the fork.
 */
void
stdio_fork(int expected_in[3], int abort_fd_in[2], int pmi_fd_in)
{
    int i, n;
    int ret, sv[2];

    n = 0;
    for (i=0; i<3; i++) {
	connected[i] = 0;
	expected[i] = expected_in[i];
	listener_port[i] = -1;
	n += expected[i];
    }

    /* mpich/gm: not sure which to use yet, depends on gm version */
    /* mpich2/pmi wants all of them */
    /* mpich/ib wants just #0 */
    /* all will send a message when ready for handoff */
    abort_fd_array[0] = abort_fd_in[0];
    abort_fd_array[1] = abort_fd_in[1];
    pmi_listen_fd = pmi_fd_in;

    /*
     * Must handle MPI_Abort socket for mpich/gm even if no real stdio,
     * convenient to do so here.
     */
    pipe_with_stdio = -1;
    if (n == 0 && abort_fd_array[0] == -1 && pmi_listen_fd == -1)
	return;  /* nothing to do */

    /*
     * Ensure we can be connected to this many things at once:
     *   3 aggregated stdio streams to shell or some local output
     *   3 sockets listening for new connections from remote processes
     *   2 * num-processes  per-process connections for stdout/stderr
     *   Some more for stdin:
     *     -nostdin:  0
     *     (default): 1
     *     -allstdin: num_processes
     * On linux, maxfd can be pushed up to 1024*1024, perhaps using a
     * setuid-wrapper program to do:
     *   #include <sys/resource.h>
     *   struct rlimit rlim;
     *   rlim.rlim_cur = rlim.rlim_max = 1024*1024;
     *   setrlimit(RLIMIT_NOFILE, &rlim);
     * That would allow for 350,000 processes with three connected
     * streams each, or 524,000 processs with just out/err.  Default
     * 1024 maxfd only allows 340 process, or 510 processes with just
     * out/err.
     *
     * On Apple Mac Darwin OSX, take a look at kern.maxfilesperproc to change
     * the system default limit, but there are other factors that may change
     * the limit.  Put a call to ulimit in the /Libraries/StartupItems/ script
     * used to start pbs_mom on the nodes, perhaps.  This area is quite in flux
     * on that OS.
     */
    maxfd = sysconf(_SC_OPEN_MAX);
    if (n+6 > maxfd)
	error("%s: need %d sockets, only %d available", __func__, n+6, maxfd);

    /*
     * Must setup listener sockets before fork so valid port numbers
     * can be handed back and the spawn can continue.
     */
    for (i=0; i<3; i++) {
	if (expected[i])
	    listener[i] = build_listener(&listener_port[i]);
	else
	    listener[i] = -1;

	debug(3, "%s: built listener %d in fd %d on port %d",
	  __func__, i, listener[i], listener_port[i]);
    }

    /*
     * Forget about stdin if it was used for --config command line option.
     * Already closed after config file read.
     */
    if (!expected[IN])
	aggregate[IN] = -1;

    /*
     * Share a pipe between the first process and this forked one for
     * some upcalls from stdio listener to main:  abort and spawn, and
     * maybe some downcalls related to spawn too.
     */
    ret = socketpair(AF_UNIX, SOCK_STREAM, PF_UNSPEC, sv);
    if (ret < 0)
	error_errno("%s: socketpair", __func__);

    pid = fork();
    if (pid < 0)
	error_errno("%s: fork", __func__);
    if (pid > 0) {
	/*
	 * Parent: do not listen to stdin but leave 1,2 open for
	 * debugging and error output (to pbs batch output files
	 * or to tty for interactive).  Be careful to check that this
	 * really is the old stdin and not, say, the IB or GM listening
	 * socket.
	 */
	if (aggregate[0] >= 0)
	    close(aggregate[0]);

	/* close the listener sockets, child has them now */
	for (i=0; i<3; i++)
	    if (listener[i] >= 0)
		close(listener[i]);

	/* just keep our half of the socket pair */
	pipe_with_stdio = sv[0];
	close(sv[1]);
	/* wait for him to startup */
	stdio_msg_parent_read();
    } else {
	close(sv[0]);
	pipe_with_stdio = sv[1];
	do_child();
    }
}

/*
 * Return the port number of a listener stream, so that it can be encoded
 * in an environment variable that the pbs moms can parse.  Parent knows
 * these; this is just for encapsulation.
 *
 * Negative return number is invalid.
 */
int
stdio_port(int n)
{
    return listener_port[n];
}

/*
 * Called from early in main() so we can isolate exactly the fds
 * passed in from the calling program (like PBS shell script, which
 * may have been evil:  mpiexec foo 1<&- ).  Must do this before
 * opening anything new else we may find those instead.
 */
void
stdio_notice_streams(void)
{
    int i;
    struct stat sb;

    for (i=0; i<3; i++) {
	if (fstat(i, &sb) < 0) {
	    if (errno == EBADF)
		aggregate[i] = -1;
	    else
		error_errno("%s: fstat initial fd %d", __func__, i);
	} else {
	    /* assume it's okay if stat returns happily */
	    aggregate[i] = i;
	}
	last_user[i] = -1;
    }
    aggregate_in_ready = 0;
    debug(3, "%s: aggregate = %d %d %d", __func__, aggregate[0], aggregate[1],
      aggregate[2]);
}

typedef enum {
    STDIO_MSG_LISTENER_SAYS_HELLO,
    STDIO_MSG_LISTENER_SAYS_GOODBYE,
    STDIO_MSG_LISTENER_SAYS_ABORT,
    STDIO_MSG_LISTENER_SAYS_SPAWN,
    STDIO_MSG_LISTENER_SAYS_MORE_TASKS_RESULT,
    STDIO_MSG_PARENT_SAYS_LISTEN_TO_ABORT_FD,
    STDIO_MSG_PARENT_SAYS_SPAWN_RESULT,
    STDIO_MSG_PARENT_SAYS_MORE_TASKS,
} stdio_message_t;

static void stdio_msg_parent_read_spawn(void);
static void stdio_msg_parent_say_spawn_result(int rank, int ok);

/*
 * Called from parent.  Something happened that the stdio process wants to
 * say something.
 */
void
stdio_msg_parent_read(void)
{
    int ret;
    stdio_message_t msg;

    ret = read_full_ret(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: read", __func__);
    if (ret == 0) {
	debug(2, "%s: pipe closed", __func__);
	close(pipe_with_stdio);
	pipe_with_stdio = -1;
	return;
    }
    if (msg == STDIO_MSG_LISTENER_SAYS_HELLO) {
	debug(2, "%s: got hello from listener", __func__);
    } else if (msg == STDIO_MSG_LISTENER_SAYS_GOODBYE) {
	debug(2, "%s: got goodbye from listener", __func__);
	close(pipe_with_stdio);
	pipe_with_stdio = -1;
    } else if (msg == STDIO_MSG_LISTENER_SAYS_ABORT) {
	debug(2, "%s: got abort from listener", __func__);
	killall(SIGTERM);  /* looks like a ctrl-c */
    } else if (msg == STDIO_MSG_LISTENER_SAYS_SPAWN) {
	stdio_msg_parent_read_spawn();
    } else
	error("%s: unknown message from stdio listener %d", __func__,
	      (int) msg);
}

/*
 * Called from child.  Parent says something to it.
 */
static void
stdio_msg_listener_read(void)
{
    int ret;
    stdio_message_t msg;

    ret = read_full_ret(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: read", __func__);
    if (ret == 0) {
	debug(2, "%s: pipe closed, exiting too", __func__);
	goodbye_from_parent(SIGTERM);
	close(pipe_with_stdio);
	pipe_with_stdio = -1;
	return;
    }
    if (msg == STDIO_MSG_PARENT_SAYS_LISTEN_TO_ABORT_FD) {
	int abort_fd_index;
	ret = read_full_ret(pipe_with_stdio, &abort_fd_index,
	                    sizeof(abort_fd_index));
	if (ret < 0)
	    error_errno("%s: read abort_fd_index", __func__);
	if (ret == 0)
	    error("%s: pipe closed reading abort_fd_index", __func__);
	listen_abort_fd(abort_fd_index);
    } else if (msg == STDIO_MSG_PARENT_SAYS_SPAWN_RESULT) {
	int rank, ok;

	ret = read_full_ret(pipe_with_stdio, &rank, sizeof(rank));
	if (ret < 0)
	    error_errno("%s: read rank", __func__);
	ret = read_full_ret(pipe_with_stdio, &ok, sizeof(ok));
	if (ret < 0)
	    error_errno("%s: read ok", __func__);
	pmi_send_spawn_result(rank, ok);
    } else if (msg == STDIO_MSG_PARENT_SAYS_MORE_TASKS) {
	int i, num, port, expected_in[3];
	void *x;

	ret = read_full_ret(pipe_with_stdio, &num, sizeof(num));
	if (ret < 0)
	    error_errno("%s: read num", __func__);
	ret = read_full_ret(pipe_with_stdio, expected_in,
	                    3 * sizeof(*expected_in));
	if (ret < 0)
	    error_errno("%s: read expected_in", __func__);

	/* remember my new spawns set */
	x = spawns;
	spawns = Malloc((numspawns + 1) * sizeof(*spawns));
	memcpy(spawns, x, numspawns * sizeof(*spawns));
	free(x);
	memset(&spawns[numspawns], 0, sizeof(*spawns));
	spawns[numspawns].task_start = numtasks;
	spawns[numspawns].task_end = numtasks + num;
	++numspawns;

	/* reopen pmi */
	if (pmi_listen_fd == -1) {
	    port = prepare_pmi_startup_port(&pmi_listen_fd);
	    poll_set(pmi_listen_fd, &rfs);
	} else
	    port = pmi_listen_port;

	/* reopen stdio listeners */
	for (i=0; i<3; i++) {
	    if (expected_in[i] > 0) {
		if (listener[i] == -1) {
		    listener[i] = build_listener(&listener_port[i]);
		    poll_set(listener[i], &rfs);
		}
	    } else {
		listener_port[i] = -1;
	    }
	}

	/* enlarge fd arrays */
	x = pmi_fds;
	pmi_fds = Malloc((numtasks + num) * sizeof(*pmi_fds));
	memcpy(pmi_fds, x, numtasks * sizeof(*pmi_fds));
	free(x);
	for (i=numtasks; i<numtasks+num; i++)
	    pmi_fds[i] = -1;

	x = pmi_barrier;
	pmi_barrier = Malloc((numtasks + num) * sizeof(*pmi_barrier));
	memcpy(pmi_barrier, x, numtasks * sizeof(*pmi_barrier));
	free(x);
	for (i=numtasks; i<numtasks+num; i++)
	    pmi_barrier[i] = 0;

	/* commit to handling these new ones */
	numtasks += num;

	msg = STDIO_MSG_LISTENER_SAYS_MORE_TASKS_RESULT;
	ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
	if (ret < 0)
	    error_errno("%s: write", __func__);
	ret = write_full(pipe_with_stdio, &port, sizeof(port));
	if (ret < 0)
	    error_errno("%s: write port", __func__);
	ret = write_full(pipe_with_stdio, listener_port,
	                 3 * sizeof(*listener_port));
	if (ret < 0)
	    error_errno("%s: write listener_port", __func__);

    } else
	error("%s: unknown message from parent %d", __func__, (int) msg);
}

/*
 * Send a message to the child that the abort_fd of choice is ready
 * to be listened to.
 */
void
stdio_msg_parent_say_abort_fd(int abort_fd_index)
{
    int ret;
    stdio_message_t msg = STDIO_MSG_PARENT_SAYS_LISTEN_TO_ABORT_FD;

    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
    ret = write_full(pipe_with_stdio, &abort_fd_index, sizeof(abort_fd_index));
    if (ret < 0)
	error_errno("%s: write abort_fd_index", __func__);
}

/*
 * Synchronize parent/listener fork.
 */
static void
stdio_msg_listener_say_hello(void)
{
    int ret;
    stdio_message_t msg = STDIO_MSG_LISTENER_SAYS_HELLO;

    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
}

/*
 * Let parent know gracefully that stdio process is exiting.
 */
static void
stdio_msg_listener_say_goodbye(void)
{
    int ret;
    stdio_message_t msg = STDIO_MSG_LISTENER_SAYS_GOODBYE;

    /* parent may have already disappeared, like when -server hangs up */
    if (pipe_with_stdio >= 0) {
	ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
	if (ret < 0)
	    error_errno("%s: write", __func__);
    }
}

/*
 * Let parent know that one of the tasks called MPI_Abort.
 */
static void
stdio_msg_listener_say_abort(void)
{
    int ret;
    stdio_message_t msg = STDIO_MSG_LISTENER_SAYS_ABORT;

    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
}

/*
 * Send a spawn request from stdio listener to parent.  Called from handle_pmi.
 */
void
stdio_msg_listener_spawn(int rank, int nprocs, const char *execname,
                         int numarg, const char *const *args,
                         int numinfo, const char *const *infokeys,
			              const char *const *infovals)
{
    int i, ret;
    stdio_message_t msg = STDIO_MSG_LISTENER_SAYS_SPAWN;

    debug(2, "%s: spawn %d %s", __func__, nprocs, execname);
    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
    ret = write_full(pipe_with_stdio, &rank, sizeof(rank));
    if (ret < 0)
	error_errno("%s: write rank", __func__);
    ret = write_full(pipe_with_stdio, &nprocs, sizeof(nprocs));
    if (ret < 0)
	error_errno("%s: write nprocs", __func__);
    ret = write_full_string(pipe_with_stdio, execname);
    if (ret < 0)
	error_errno("%s: write execname", __func__);

    ret = write_full(pipe_with_stdio, &numarg, sizeof(numarg));
    if (ret < 0)
	error_errno("%s: write numarg", __func__);
    for (i=0; i<numarg; i++) {
	ret = write_full_string(pipe_with_stdio, args[i]);
	if (ret < 0)
	    error_errno("%s: write args[%d]", __func__, i);
    }

    ret = write_full(pipe_with_stdio, &numinfo, sizeof(numinfo));
    if (ret < 0)
	error_errno("%s: write numinfo", __func__);
    for (i=0; i<numinfo; i++) {
	ret = write_full_string(pipe_with_stdio, infokeys[i]);
	if (ret < 0)
	    error_errno("%s: write infokeys[%d]", __func__, i);
	ret = write_full_string(pipe_with_stdio, infovals[i]);
	if (ret < 0)
	    error_errno("%s: write infovals[%d]", __func__, i);
    }
}

/*
 * Read and deal with a spawn request.
 */
static void
stdio_msg_parent_read_spawn(void)
{
    int i, ret;
    int rank;
    int nprocs;
    char *execname;
    int numarg;
    char **args;
    int numinfo;
    char **infokeys;
    char **infovals;

    ret = read_full_ret(pipe_with_stdio, &rank, sizeof(rank));
    if (ret < 0)
	error_errno("%s: read rank", __func__);
    ret = read_full_ret(pipe_with_stdio, &nprocs, sizeof(nprocs));
    if (ret < 0)
	error_errno("%s: read nprocs", __func__);
    ret = read_full_string(pipe_with_stdio, &execname);
    if (ret < 0)
	error_errno("%s: read execname", __func__);

    ret = read_full_ret(pipe_with_stdio, &numarg, sizeof(numarg));
    if (ret < 0)
	error_errno("%s: read numarg", __func__);
    args = NULL;
    if (numarg > 0)
	args = Malloc(numarg * sizeof(*args));
    for (i=0; i<numarg; i++) {
	ret = read_full_string(pipe_with_stdio, &args[i]);
	if (ret < 0)
	    error_errno("%s: read args[%d]", __func__, i);
    }

    ret = read_full_ret(pipe_with_stdio, &numinfo, sizeof(numinfo));
    if (ret < 0)
	error_errno("%s: read numinfo", __func__);
    infokeys = NULL;
    infovals = NULL;
    if (numinfo > 0) {
	infokeys = Malloc(numinfo * sizeof(*infokeys));
	infovals = Malloc(numinfo * sizeof(*infovals));
    }
    for (i=0; i<numinfo; i++) {
	ret = read_full_string(pipe_with_stdio, &infokeys[i]);
	if (ret < 0)
	    error_errno("%s: read infokeys[%d]", __func__, i);
	ret = read_full_string(pipe_with_stdio, &infovals[i]);
	if (ret < 0)
	    error_errno("%s: read infovals[%d]", __func__, i);
    }
    ret = spawn(nprocs, execname, numarg, args, numinfo, infokeys, infovals);
    stdio_msg_parent_say_spawn_result(rank, ret);
}

/*
 * Send a message to the child that the abort_fd of choice is ready
 * to be listened to.
 */
static void
stdio_msg_parent_say_spawn_result(int rank, int ok)
{
    int ret;
    stdio_message_t msg = STDIO_MSG_PARENT_SAYS_SPAWN_RESULT;

    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
    ret = write_full(pipe_with_stdio, &rank, sizeof(rank));
    if (ret < 0)
	error_errno("%s: write rank", __func__);
    ret = write_full(pipe_with_stdio, &ok, sizeof(ok));
    if (ret < 0)
	error_errno("%s: write ok", __func__);
}

int
stdio_msg_parent_say_more_tasks(int num, int expected_in[3])
{
    int ret, port;
    stdio_message_t msg = STDIO_MSG_PARENT_SAYS_MORE_TASKS;

    ret = write_full(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: write", __func__);
    ret = write_full(pipe_with_stdio, &num, sizeof(num));
    if (ret < 0)
	error_errno("%s: write num", __func__);
    ret = write_full(pipe_with_stdio, expected_in, 3 * sizeof(*expected_in));
    if (ret < 0)
	error_errno("%s: write expected_in", __func__);

    ret = read_full_ret(pipe_with_stdio, &msg, sizeof(msg));
    if (ret < 0)
	error_errno("%s: read", __func__);
    if (msg != STDIO_MSG_LISTENER_SAYS_MORE_TASKS_RESULT)
	error("%s: expected listener to give more tasks result, got %d",
	      __func__, (int) msg);
    ret = read_full_ret(pipe_with_stdio, &port, sizeof(port));
    if (ret < 0)
	error_errno("%s: read port", __func__);
    ret = read_full_ret(pipe_with_stdio, listener_port,
                        3 * sizeof(*listener_port));
    if (ret < 0)
	error_errno("%s: read listener_port", __func__);
    return port;
}

/*--- Below here are mostly file-internal functions. ---*/

/*
 * Create and listen on a socket, returning it, and sticking the port
 * number in the passed-in pointer.
 */
static int
build_listener(int *port)
{
    int s;
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if ((s = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	error_errno("%s: socket", __func__);
    if (listen(s, 1024) < 0)
	error_errno("%s: listen", __func__);
    if (getsockname(s, (struct sockaddr *)&addr, &len) < 0)
	error_errno("%s: getsockname", __func__);
    *port = ntohs(addr.sin_port);
    return s;
}

/*
 * Close a socket and remove it from the select set.  (Not for
 * listener sockets or aggregate fds.)
 */
static void
close_clear(int s)
{
    if (close(s))
	error_errno("%s: close", __func__);

    /* decrease open connection count */
    --connected[fds[s].which];
    debug(3, "%s: bye %d type %s connected %d %d %d"
      " expected %d %d %d port %d %d %d", __func__,
      s, which_name(fds[s].which), connected[0], connected[1], connected[2],
      expected[0], expected[1], expected[2],
      listener_port[0], listener_port[1], listener_port[2]);

    /* perhaps just exit */
    if (connected[fds[s].which] == 0)
	maybe_exit_stdio();

    /* ignore this socket, but leave it linked onto the print queue, if it is */
    poll_del(s, &rfs);
    fds[s].which = NONE;
}

/*
 * If all streams have closed and no listeners are open, no reason to
 * hang around anymore.  Only check for this if we just produced a
 * stream with no listeners, i.e. closed all those who might produce
 * input/output to this stream.
 */
void
maybe_exit_stdio(void)
{
    int i;

    for (i=0; i<3; i++) {
	if (connected[i])
	    return;
	if (listener[i] >= 0)
	    return;
    }

    /* stay around for aborts, even if not told to listen by parent yet */
    if (abort_fd >= 0 || abort_fd_array[0] >= 0)
	return;

    /* if pmi listener open or any connected pmi clients */
    if (pmi_listen_fd >= 0)
	return;

    if (pmi_fds)
	for (i=0; i<numtasks; i++)
	    if (pmi_fds[i] >= 0)
		return;

    debug(3, "%s: stdio process: nobody left", __func__);

    /* flush stdout, stderr */
    while (last_user[OUT] >= 0)
	walk_buf_list(OUT);
    while (last_user[ERR] >= 0)
	walk_buf_list(ERR);

    dump_fd_state();
    stdio_msg_listener_say_goodbye();
    exit(0);
}

/*
 * Close a listener socket (or stdin file/socket).
 * Also remove it from rfs.  n == 0,1,2.
 */
static void
close_listener(int n)
{
    if (close(listener[n]))  /* stop listening */
	error_errno("%s: close %d at fd %d", __func__, n, listener[n]);
    poll_del(listener[n], &rfs);
    listener[n] = -1;
#if 0
    if (n == OUT || n == ERR)  /* just the _real_ listeners, not my own stdin */
	listener_port[n] = -1;  /* XXX: can this ever expect future spawnees??? */
#endif
}

/*
 * Tell processes there is no more stdin.
 */
static void
close_stdin_hangup_remote(void)
{
    int i;

    for (i=0; connected[IN] && i<maxfd; i++) {
	if (fds[i].which == IN) {
	    debug(3, "%s: to close_clear %d", __func__, i);
	    close_clear(i);
	}
    }
}

/*
 * Close the stdin socket and the stdins of the processes to which it is
 * connected.  The listener had better already be closed, since we don't
 * pay attention to stdin until it is fully connected.
 */
static void
close_stdin(void)
{
    close(aggregate[IN]);
    poll_del(aggregate[IN], &rfs);
    aggregate[IN] = -1;
    aggregate_in_ready = 0;
    close_stdin_hangup_remote();
}

/*
 * Over the timeout?
 */
static inline int
expired(struct timeval *then)
{
    struct timeval now;
    gettimeofday(&now, 0);
    now.tv_sec -= then->tv_sec;
    now.tv_usec -= then->tv_usec;
    now.tv_usec += 1000000 * now.tv_sec;
    return (now.tv_usec > 1000000 * flush_time.tv_sec + flush_time.tv_usec);
}

/*
 * Generate some text to stdout or stderr.
 */
static void
aggregate_output(fd_which_t which, int s, const char *buf, int len)
{
    debug(3, "%s: output to stream %s from %s", __func__,
      which_name(which), inet_ntoa(fds[s].sin.sin_addr));
#if 0
    {
	/* debug code: announce writer */
	static int last_writer = -1;
	static growstr_t *g = 0;

	if (last_writer != s) {
	    last_writer = s;
	    if (!g)
		g = growstr_init();
	    else
		growstr_zero(g);
	    growstr_printf(g, "<%s>", inet_ntoa(fds[s].sin.sin_addr));
	    write_full(aggregate[which], g->s, g->len);
	}
    }
#endif
    write_full(aggregate[which], buf, len);
}

/*
 * Run the linked list of buffers and flush them if possible.
 */
static void
walk_buf_list(fd_which_t which)
{
    int last_char;
    int s = last_user[which];
    int next_user, next_expired, empty_buf;

    aggregate_output(which, s, fds[s].buf->s, fds[s].buf->len);

    empty_buf = fds[s].buf->len == 0;
    last_char = empty_buf ? 0 : fds[s].buf->s[fds[s].buf->len-1];

    growstr_zero(fds[s].buf);
    fds[s].tv.tv_sec = 0;

    next_user = fds[s].next;
    next_expired = (next_user >= 0) && expired(&fds[next_user].tv);

    if (empty_buf || last_char == '\n' || next_expired) {
	debug(3, "%s: advance to next %d", __func__, next_user);
	fds[s].next = -1;
	last_user[which] = next_user;
	if (next_user >= 0)
	    walk_buf_list(which);
    }
}

/*
 * A stdout/stderr socket from a process is readable.
 */
static void
readsome(int s)
{
    int cc, last_char;
    char buf[1024];
    fd_which_t which;

    if (fds[s].which == IN)
	error("%s: data on IN socket %d", __func__, s);

    cc = read(s, buf, sizeof(buf)-1);
    if (cc < 0) {
	if (errno == EINTR)
	    return;
	error_errno("%s: read", __func__);
    }
    debug(3, "%s: fd %d has %d bytes", __func__, s, cc);
    if (cc == 0) {
	close_clear(s);
	return;
    }
    buf[cc] = 0;
    which = fds[s].which;

    if (last_user[which] >= 0 && last_user[which] != s) {
	/* pending data from another proc has been printed already, must
	 * wait for that to finish (unless a timeout is triggered) */
	if (fds[s].tv.tv_sec == 0) {  /* not queued */
	    for (cc = last_user[which]; fds[cc].next >= 0; cc = fds[cc].next) ;
	    fds[cc].next = s;
	    gettimeofday(&fds[s].tv, 0);
	    debug(3, "%s: linked %d onto %d", __func__, s, cc);
	}
	growstr_append(fds[s].buf, buf);
	debug(3, "%s: queued for %d on %s: \"%s\"", __func__, s,
	  which_name(which), buf);
	return;
    }

    /* okay to print my data */
    aggregate_output(which, s, buf, cc);

    /* take (or continue) ownership of the stream */
    last_char = buf[cc-1];
    if (last_char != '\n')
	last_user[which] = s;
    else if (last_user[which] == s) {
	/* flush others now that we have ended this line */
	debug(3, "%s: release ownership, give to %d", __func__, fds[s].next);
	last_user[which] = fds[s].next;
	fds[s].next = -1;
	if (last_user[which] >= 0)
	    walk_buf_list(which);
    }
}

/*
 * Read from stdin and broadcast to all connected readers.
 */
static void
read_stdin(void)
{
    int cc, n, i;
    char buf[1024];

    cc = read(aggregate[IN], buf, sizeof(buf));
    if (cc < 0) {
	if (errno == EINTR)
	    return;
	error_errno("%s: read", __func__);
    }
    if (cc == 0) {
	close_stdin();
	return;
    }
    n = connected[IN];
    for (i=0; n && i<maxfd; i++) {
	if (fds[i].which == IN) {
	    --n;
	    if (write_full(i, buf, cc) < 0)
		error_errno("%s: write %d bytes to %d", __func__, cc, i);
	}
    }
}

/*
 * Listening socket has found a new one, as reported by select.
 * Accept it and pay attention to it.
 */
static void
accept_new_conn(fd_which_t which)
{
    int t;
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    debug(3, "%s: getting new %s from fd %d", __func__, which_name(which),
      listener[which]);
    t = accept(listener[which], (struct sockaddr *) &sin, &len);
    if (t < 0)
	error_errno("%s: accept listener %d at fd %d", __func__, which,
	  listener[which]);
    fds[t].which = which;
    fds[t].sin = sin;
    if (which == OUT || which == ERR)
	fds[t].buf = growstr_init_empty();
    ++connected[which];
    --expected[which];
    debug(3, "%s: fd %d type %3s, connected = %d %d %d, expected = %d %d %d",
      __func__, t, which_name(fds[t].which),
      connected[0], connected[1], connected[2],
      expected[0], expected[1], expected[2]);
    if (expected[which] == 0) {
	debug(3, "%s: full listener, closing %s; it was on fd %d", __func__,
	  which_name(which), listener[which]);
	close_listener(which);
	if (which == IN) {
	    if (aggregate[IN] >= 0) {
		/* begin to listen to the stdin of mpiexec itself */
		poll_set(aggregate[IN], &rfs);
		aggregate_in_ready = 1;
	    } else {
		/* tell them there is no stdin */
		poll_set(t, &rfs);
		close_stdin_hangup_remote();
		return;  /* set before hangup for simplicity already */
	    }
	}
    }
    /* even set IN sockets so we know when they are closed */
    poll_set(t, &rfs);
}

/*
 * One of the compute nodes is trying to connect to us to signal
 * an MPI_Abort.  Accept it, read the string, and cause everybody
 * else to die.  For MPICH2, this is general PMI activity, not necessarily
 * an abort.
 */
static void
accept_abort_conn(void)
{
    int fd, cc;
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);

    fd = accept(abort_fd, (struct sockaddr *) &sin, &len);
    if (fd < 0)
	error_errno("%s: accept abort_fd", __func__);

    if (cl_args->comm == COMM_MPICH_GM) {
	char s[64*1024];
	int magic;

	cc = read_until(fd, s, sizeof(s), ">>>", 0);
	if (cc < 0)
	    error_errno("%s: read abort message from IP %s", __func__,
	      inet_ntoa(sin.sin_addr));
	if (cc == 0)
	    error("%s: eof in abort message from IP %s", __func__,
	      inet_ntoa(sin.sin_addr));
	if (sscanf(s, "<<<ABORT_%d_ABORT>>>", &magic) != 1)
	    error("%s: parse abort message from IP %s: \"%s\"",
	      __func__, inet_ntoa(sin.sin_addr), s);
	if (magic != atoi(jobid))
	    error("%s: bad magic in abort message from IP %s: \"%s\"",
	      __func__, inet_ntoa(sin.sin_addr), s);
	warning("%s: MPI_Abort from IP %s, killing all",
	  __func__, inet_ntoa(sin.sin_addr));
    } else if (cl_args->comm == COMM_MPICH_IB) {
	int ret, rank;

	ret = read_full_ret(fd, &rank, sizeof(rank));
	if (ret < 0)
	    error_errno("%s: read rank in abort message from IP %s", __func__,
	                inet_ntoa(sin.sin_addr));
	warning("%s: MPI_Abort from IP %s, rank %d, killing all",
	  __func__, inet_ntoa(sin.sin_addr), rank);

    } else if (cl_args->comm == COMM_MPICH_PSM) {
	char hdr[12];
	uint16_t *eshut_ptype, eshut_type;
	uint32_t *eshut_hdr, eshut_jobid;
	char *errdata, *err_extra;
	const char *err_reason;
	uint32_t message_len, mpi_rank;

	read_full(fd, hdr, sizeof(hdr));
	eshut_ptype = (uint16_t *) hdr;
	if (ntohs(eshut_ptype[1]) != 5 /* EPID_SHUTDOWN */)
	    error("%s: Unknown protocol message received in abort path\n",
		  __func__);

	eshut_hdr   = (uint32_t *) (eshut_ptype + 2);
	message_len = ntohl(eshut_hdr[0]);
	mpi_rank    = ntohl(eshut_hdr[1]);

	errdata = Malloc(message_len);
	read_full(fd, errdata, message_len);
	eshut_jobid = ntohl(*(uint32_t *) errdata);

	if (eshut_jobid != (uint32_t) atoi(jobid))
	    error("%s: Incorrect jobid received from rank %d: \"%d\"",
		  __func__, mpi_rank, eshut_jobid);
	if (message_len < 8)
	    error("%s: Incorrect message length of %d bytes for EPID_SHUTDOWN "
		  "received from rank %d", __func__, message_len, mpi_rank);

	eshut_type = ntohs(*((uint16_t *) (errdata + 4)));
	switch (eshut_type) {
	    case 2:
		err_reason = "MPI_Abort";
		break;
	    case 3:
		err_reason = "MPI Library abort";
		break;
	    case 5:
		err_reason = "Socket connection error";
		break;
	    default:
		err_reason = "Unknown abort reason";
	}

	err_extra = NULL;
	if (message_len > 8) {  /* extra error message attached */
	    err_extra = Malloc(message_len);  /* 8 extra chars we need */
	    snprintf(err_extra, message_len-1, " (%s) ", errdata+8);
	    err_extra[message_len-1] = '\0';
	}

	warning("%s: %s%sfrom rank %d. Killing all",
		__func__, err_reason, err_extra ? err_extra : " ", mpi_rank);

	if (err_extra)
	    free(err_extra);
	free(errdata);
    } else {
	warning("%s: MPI_Abort was not expected for this communication library",
	  __func__);
    }
    close(fd);
    close(abort_fd);
    poll_del(abort_fd, &rfs);
    abort_fd = -1;
    abort_fd_used = 1;
    /* Let parent know; he'll tell us to exit later. */
    stdio_msg_listener_say_abort();
}

/*
 * Accept new connections on the listening sockets, and shuttle bytes
 * around on existing ones.
 */
static void ATTR_NORETURN
do_child(void)
{
    int i;

    /*
     * Kill off everything except our listener[] sockets and
     * the "real" aggregate streams (probably 0,1,2) and other
     * useful sockets.  Random stuff to close includes a connection
     * to the pbs_mom.
     */
    for (i=0; i<maxfd; i++) {
	int j;
	for (j=0; j<3; j++) {
	    if (listener[j] == i) break;
	    if (aggregate[j] == i) break;
	}
	if (j < 3)
	    continue;

	if (abort_fd_array[0] == i) continue;
	if (abort_fd_array[1] == i) continue;
	if (pmi_listen_fd == i) continue;

	if (pipe_with_stdio == i) continue;

	(void) close(i);
    }

    /* get ready for PMI connections */
    if (pmi_listen_fd >= 0) {
	pmi_fds = Malloc(numtasks * sizeof(*pmi_fds));
	pmi_barrier = Malloc(numtasks * sizeof(*pmi_fds));
	for (i=0; i<numtasks; i++) {
	    pmi_fds[i] = -1;
	    pmi_barrier[i] = 0;
	}
    } else {
	pmi_fds = NULL;
    }

    /* allocate socket storage */
    fds = (fd_state_t *) Malloc(maxfd * sizeof(*fds));
    for (i=0; i<maxfd; i++) {
	fds[i].which = NONE;
	fds[i].buf = 0;
	fds[i].next = -1;
	fds[i].tv.tv_sec = 0;  /* not yet linked */
	memset(&fds[i].sin, 0, sizeof(fds[i].sin));
    }

    /* initial select source */
#ifdef HAVE_POLL
    pfs = Malloc(maxfd * sizeof(*pfs));
    pfsmap = Malloc(maxfd * sizeof(*pfsmap));
    for (i=0; i<maxfd; i++)
	pfsmap[i] = -1;  /* indicates no entry in pfs[] for this fd */
    pfsnum = 0;
#else
    FD_ZERO(&rfs);
#endif

    /* put in listeners and parent pipe */
    for (i=0; i<3; i++)
	if (listener[i] >= 0)
	    poll_set(listener[i], &rfs);
    if (pmi_listen_fd >= 0)
	poll_set(pmi_listen_fd, &rfs);
    poll_set(pipe_with_stdio, &rfs);

    /* writeable sockets do not get selected on, just hope they can take it */

    {
	const int term_list[] = { SIGTERM };
	const int hup_int_list[] = { SIGHUP, SIGINT };

	/* parent telling us to go gracefully */
	handle_signals(term_list, list_count(term_list), goodbye_from_parent);

	/* ignore these */
	handle_signals(hup_int_list, list_count(hup_int_list), SIG_IGN);
    }

#ifdef DEBUG_FORK
    printf("pid %d sleeping...\n", getpid());
    sleep(10);
    printf("done\n");
#endif

    /* tell parent we're ready */
    stdio_msg_listener_say_hello();

    /*
     * Infinite blocking select- (or poll-) driven loop.
     */
    for (;;) {
	int n;

#ifdef HAVE_POLL
	int j;
	fd_set rfsd;  /* dummy argument */
	const char *const syscall = "poll";
	n = poll(pfs, pfsnum, 100);
#else
	struct timeval tv = { 0, 100000 };  /* check for timeouts every 100ms */
	const char *const syscall = "select";
	fd_set rfsd = rfs; /* copy in the static set */
	n = select(FD_SETSIZE, &rfsd, 0, 0, &tv);
#endif

	if (n < 0) {
	    if (errno == EINTR)
		continue;
	    error_errno("%s: %s", __func__, syscall);
	}

	if (n == 0) {
	    for (i=1; i<3; i++) {
		int next_user;
		if (last_user[i] < 0)
		    continue;
		next_user = fds[last_user[i]].next;
		if (next_user < 0)
		    continue;
		if (expired(&fds[next_user].tv)) {
		    /*
		    const char *s = "timeout\n";
		    write(aggregate[OUT], s, strlen(s));
		    */
		    debug(3, "%s: hold data timeout", __func__);
		    walk_buf_list((fd_which_t)i);
		}
	    }
	    continue;
	}

	debug(3, "%s: %s got %d", __func__, syscall, n);

	/* my aggregate stdin */
	if (n && aggregate_in_ready) {
	    if (poll_isset(aggregate[IN], &rfsd)) {
		--n;
		poll_clr(aggregate[IN], &rfsd);
		debug(3, "%s: input at aggregate[IN], %d select bits left",
			 __func__, n);
		read_stdin();
	    }
#ifdef HAVE_POLL
	    /* Mac OSX apparently returns POLLNVAL for this when tty odd;
	     * this avoids an infinite loop at least, but would be nice to
	     * understand why it is broken.  Update:  perhaps check the Torque
	     * build for #define __TDARWIN_8 on Darwin 8 systems.  Defining
	     * __TDARWIN is not correct on that version of the Mac OS.
	     */
	    if (aggregate[IN] >= 0
	     && pfs[pfsmap[aggregate[IN]]].revents & POLLNVAL) {
		warning("%s: aggregate input fd %d returns POLLNVAL, closing",
		        __func__, aggregate[IN]);
		close_stdin();
	    }
#endif
	}

	/* new incoming connections */
	for (i=0; n && i<3; i++) {
	    if (listener[i] >= 0 && poll_isset(listener[i], &rfsd)) {
		/* so processes don't see it */
		poll_clr(listener[i], &rfsd);
		--n;
		debug(3, "%s: input at listener[%s], %d select bits left",
		  __func__, which_name((fd_which_t)i), n);
		accept_new_conn((fd_which_t)i);
	    }
	}

	/* abort fd connection */
	if (n && abort_fd >= 0 && poll_isset(abort_fd, &rfsd)) {
	    poll_clr(abort_fd, &rfsd);
	    --n;
	    accept_abort_conn();
	}

	/* PMI listener */
	if (n && pmi_listen_fd >= 0 && poll_isset(pmi_listen_fd, &rfsd)) {
	    poll_clr(pmi_listen_fd, &rfsd);  /* before the function */
	    --n;
	    accept_pmi_conn(&rfs);
	}

	/* existing PMI connections */
	if (pmi_fds)
	    for (i=0; n && i<numtasks; i++) {
		if (pmi_fds[i] >= 0 && poll_isset(pmi_fds[i], &rfsd)) {
		    poll_clr(pmi_fds[i], &rfsd);
		    --n;
		    handle_pmi(i, &rfs);
		}
	    }

	/* message from parent */
	if (n && poll_isset(pipe_with_stdio, &rfsd)) {
	    poll_clr(pipe_with_stdio, &rfsd);
	    --n;
	    stdio_msg_listener_read();
	}

	/* out,err from processes */
#ifdef HAVE_POLL
	for (j=0; n && j<pfsnum; j++) {
	    if (pfs[j].revents & POLLIN) {
		i = pfs[j].fd;
#else  /* }} */
	for (i=0; n && i<maxfd; i++) {
	    if (FD_ISSET(i, &rfsd)) {
#endif
		--n;
		debug(3, "%s: output from process at %d type %s,"
		  " %d select bits left", __func__, i,
		  which_name(fds[i].which), n);
		if (fds[i].which == OUT || fds[i].which == ERR)
		    readsome(i);
		else if (fds[i].which == IN)
		    /* stdin should never become readable, except when
		     * it has been closed by the remote process.
		     */
		    close_clear(i);
		else
		    error("%s: input on unexpected fd %d", __func__, i);
	    }
	}
    }
    /*NOTREACHED*/
}

/*
 * Called by parent code when it wants to tell the child stdio process
 * to finish up and exit.  Parent sleeps in wait() until child is done.
 * If child has already exited, it will remain as zombie and parent
 * will pick it up in the waitpid, which is just fine.  Alternative would
 * be to have parent collect SIGCHLD, but that would involve catching
 * a signal while deep somewhere in the buggy PBS library via tm_poll.
 */
void
kill_stdio(void)
{
    int stat;

    if (!pid) return;
#if 0
    printf("%s: debug pid %d now, parent is all done\n", __func__, pid);
    pause();
#endif
    if (kill(pid, SIGTERM) < 0)
	error_errno("%s: kill(%d)", __func__, pid);
    debug(3, "%s: sent SIGTERM, waiting on %d", __func__, pid);
    if (waitpid(pid, &stat, 0) < 0)
	error_errno("%s: wait", __func__);
    if (!WIFEXITED(stat)) {
	if (WIFSIGNALED(stat)) {
	    if (WTERMSIG(stat) == SIGTERM)
		; /* could be the process was so fast it never started fully */
	    else
		error("%s: stdio process died with signal %d (%s)", __func__,
		  WTERMSIG(stat), parse_signal_number(WTERMSIG(stat)));
	} else
	    error("%s: wait stat 0x%x, ifsignaled %d, termsig %d,"
	      " ifstopped %d, stopsig %d", __func__, stat, WIFSIGNALED(stat),
	      WTERMSIG(stat), WIFSTOPPED(stat), WSTOPSIG(stat));
    }
}

/*
 * Try to kill stdio, but fail quietly if unsuccessful.
 */
void
try_kill_stdio(void)
{
    /*
     * This routine may be called with a pid equal to 0 (i.e. pre-forking)
     * or -1 (i.e. when a fork() fails), so we must check for an actual pid.
     * We do not want a 0 or -1 to be passed to kill, because they kill the
     * whole process group (0) or all processes which the user has
     * privileges to kill with the exception of init and this process (-1).
     */
    if (pid <= 0)
	return;
    kill(pid, SIGTERM);
}

/*
 * Child catches SIGTERM from parent here, sets alarm and attempts to drain
 * output for a bit.
 */
static void
goodbye_from_parent(int sig ATTR_UNUSED)
{
    if (abort_fd_used)
	/* do not wait, just go */
	catch_alarm(0);
    else {
	int i, n = 0;
	/* only count actual connections, do not wait for any more since
	 * parent says they're dead if they got the obits */
	for (i=0; i<3; i++) {
	    n += connected[i];
	}
	if (n > 0) {
#ifndef DEBUG_FORK
	    const int list[] = { SIGALRM };
	    handle_signals(list, list_count(list), catch_alarm);
	    alarm(5);
#endif
	    debug(3, "%s: got signal %d, sleeping a bit", __func__, sig);
	} else {
	    debug(3, "%s: got signal %d, exiting now", __func__, sig);
	    dump_fd_state();
	    exit(0);
	}
    }
}

/*
 * Draining the output hopefully would have left time for the child processes
 * to close their sockets and exit.  This is only called if a normal
 * "nothing left to do" exit never gets reached.
 */
static void
catch_alarm(int sig ATTR_UNUSED)
{
    if (sig == SIGALRM)
	debug(3, "%s: gave up waiting for myself to exit", __func__);
    if (cl_args->verbose >= 3) {
	int i;

	debug(3, "%s: aggregate = %4d %4d %4d", __func__,
	  aggregate[0], aggregate[1], aggregate[2]);
	debug(3, "%s: listener  = %4d %4d %4d", __func__,
	  listener[0], listener[1], listener[2]);
	for (i=0; i<maxfd; i++)
	    if (fds[i].which != NONE)
		debug(3, "%s: fd %4d which %4s", __func__,
		  i, which_name(fds[i].which));
    }
    dump_fd_state();
    exit(1);
}

/*
 * Parent signals when this guy should start listening on the abort_fd
 * which was passed in at the start.  Until then parent is still busy
 * starting up the processes, which use the same socket.
 */
static void
listen_abort_fd(int abord_fd_index)
{
    if (abord_fd_index == 0) {
	abort_fd = abort_fd_array[0];
	if (cl_args->comm == COMM_MPICH_GM) {
	    close(abort_fd_array[1]);
	    abort_fd_array[1] = -1;
	}
    } else { /* mpich/gm new version only */
	abort_fd = abort_fd_array[1];
	close(abort_fd_array[0]);
	abort_fd_array[0] = -1;
    }
    debug(2, "%s: parent says via index %d to listen to abort fd %d",
      __func__, abord_fd_index, abort_fd);

    poll_set(abort_fd, &rfs);
}

