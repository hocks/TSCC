/*
 * start_tasks.c - call tm_spawn repeatedly with the correct argv and envp
 *
 * $Id: start_tasks.c 420 2008-04-10 21:40:21Z pw $
 *
 * Copyright (C) 2000-3 Ohio Supercomputer Center.
 * Copyright (C) 2000-8 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <sys/time.h>
#include <signal.h>
#include <netdb.h>  /* gethostbyname for portals */

#include "mpiexec.h"
#include "tv_attach.h"

#ifdef HAVE_PATH_H
#  include <paths.h>
#endif
#ifndef _PATH_DEFPATH
#  define _PATH_DEFPATH "/usr/bin:/bin"
#endif

#ifdef HAVE___ENVIRON
   /* defn in unistd.h */
#  define compat_environ __environ
#elif HAVE_ENVIRON
   extern char **environ;
#  define compat_environ environ
#endif

/* a big command line */
#define NARGV_LEN 32768

static char *envbuf, **envp;
static int envbuf_len, envbuf_pos, envp_len, envp_pos;
static const int envbuf_inc = 4096;
static const int envp_inc = 16;

static int envbuf_pos_save, envp_pos_save;

static int wait_task_start(void);

/*
 * Define some globals here.
 */
int numspawned;
int numtasks_waiting_start;
int startup_complete;

/*
 * Prepare envbuf, envp and related pointers
 */
static void
env_init(void)
{
    envbuf_len = 2 * envbuf_inc;
    envbuf_pos = 0;
    envbuf = Malloc(envbuf_len * sizeof(*envbuf));
    envp_len = 2 * envp_inc;
    envp_pos = 0;
    envp = Malloc(envp_len * sizeof(*envp));
}

/*
 * Possibly grow envp array of pointers into envbuf.
 */
static void
ensure_envp(void)
{
    if (envp_pos == envp_len) {
	char **x = envp;
	int oldlen = envp_len;
	envp_len += envp_inc;
	envp = Malloc(envp_len * sizeof(*envp));
	memcpy(envp, x, oldlen * sizeof(*envp));
	free(x);
    }
}

/*
 * Add another environment variable.  Does not check for duplicates.
 */
static void
env_add(const char *name, const char *value)
{
    int len = strlen(name) + 2;  /* "<name>=\0" */

    if (value)
	len += strlen(value);
    while (envbuf_pos + len >= envbuf_len) {
	int i;
	char *x = envbuf;
	int oldlen = envbuf_len;
	envbuf_len += envbuf_inc;
	envbuf = Malloc(envbuf_len * sizeof(*envbuf));
	memcpy(envbuf, x, oldlen);
	/* adjust envp pointers to point to new storage */
	for (i=0; i<envp_pos; i++)
	    envp[i] += envbuf - x;
	free(x);
    }
    ensure_envp();
    envp[envp_pos++] = envbuf + envbuf_pos;
    strcpy(envbuf + envbuf_pos, name);
    strcat(envbuf + envbuf_pos, "=");
    if (value)
	strcat(envbuf + envbuf_pos, value);
    envbuf_pos += len;
}

/*
 * env_add with an integer.
 */
static void
env_add_int(const char *name, int value)
{
    char s[64];

    sprintf(s, "%d", value);
    env_add(name, s);
}

/*
 * env_add, but only if it does not already exist
 */
static void
env_add_if_not(const char *name, const char *value)
{
    int i, len;

    len = strlen(name);
    for (i=0; i<envp_pos; i++) {
	if (strchr(envp[i], '=') - envp[i] != len)
	    continue;
	if (!strncmp(envp[i], name, len))
	    return;
    }
    env_add(name, value);
}

/*
 * env_add the entire contents of environ, but don't overwrite
 * things already set.
 */
static void
env_add_environ(void)
{
    char **e;
    int len, maxlen = 0, j, doit;
    char *s = 0;

    /*
     * Added by PBS when the task starts, do not copy these over as
     * they will override the ones assigned by the daemon (in spite of
     * the comment there stating otherwise; see the code).  Most are
     * the same, but PBS_NODENUM and PBS_TASKNUM in particular are
     * different.
     *
     * This list from PBS/src/resmom/start_exec.c, but only a subset
     * of those listed at the top: start_process() sets only some.
     */
    static const char *const variables_else[] = {
        "HOME",
        "PBS_JOBNAME",
        "PBS_JOBID", 
        "PBS_QUEUE",
        "USER",
        "PBS_JOBCOOKIE",
        "PBS_NODENUM",
        "PBS_VNODENUM",
        "PBS_TASKNUM",
        "PBS_MOMPORT",
    };
    static int num_variables_else = sizeof(variables_else)
      / sizeof(variables_else[0]);

    for (e=compat_environ; *e; ++e) {
	const char *cp = strchr(*e, '=');
	if (cp) {
	    ++cp;
	    len = cp - *e;
	} else {
	    len = strlen(*e) + 1;
	}
	if (len > maxlen) {
	    if (s)
		free(s);
	    maxlen = len;
	    s = Malloc(maxlen * sizeof(*s));
	}
	strncpy(s, *e, len-1);
	s[len-1] = '\0';
	doit = 1;
	for (j=0; j<num_variables_else; j++)
	    if (!strcmp(s, variables_else[j])) {
		doit = 0;
		break;
	    }
	if (doit)
	    env_add_if_not(s, cp);
    }
    if (s)
	free(s);
}

/*
 * Make sure there's a NULL at the end of the envp.  Does not
 * increment envp_pos.
 */
static void
env_terminate(void)
{
    ensure_envp();
    envp[envp_pos] = 0;
}

/*
 * Save/restore env state (only one deep).  Don't worry about _len, they'll
 * be as big as they need to be.
 */
static void
env_push(void)
{
    envbuf_pos_save = envbuf_pos;
    envp_pos_save = envp_pos;
}

static void
env_pop(void)
{
    envbuf_pos = envbuf_pos_save;
    envp_pos = envp_pos_save;
}

/*
 * Build the environment strings by which portals processes find
 * each other, at least in the case of the TCP NAL.
 */
static void portals_build_nidpid_maps(int spawn, growstr_t *nidmap,
				      growstr_t *pidmap)
{
    int task_start = spawns[spawn].task_start;
    int task_end = spawns[spawn].task_end;
    int i;

    for (i=task_start; i<task_end; i++) {
	struct hostent *he;
	uint32_t addr;

	if (i > task_start) {
	    growstr_append(nidmap, ":");
	    growstr_append(pidmap, ":");
	}
	/* lookup hostname */
	he = gethostbyname(nodes[tasks[i].node].mpname);
	if (!he)
	    error("%s: gethostbyname cannot resolve %s", __func__,
	          nodes[tasks[i].node].mpname);
	if (he->h_length != 4)
	    error("%s: gethostbyname returns %d-byte addresses, hoped for 4",
	          __func__, he->h_length);
	memcpy(&addr, he->h_addr_list[0], 4);
	growstr_printf(nidmap, "0x%08x", htonl(addr));
	growstr_printf(pidmap, "%d", tasks[i].cpu_index[0]);
    }
}

/*
 * Start the tasks, with much stuff in the environment.  If concurrent
 * master, this could be on behalf of some other mpiexec to which we
 * will forward any event/error results.
 */
int
start_tasks(int spawn)
{
    int i, ret = 0;
    char *nargv[3];
    char pwd[PATH_MAX];
    char *cp;
    int conns[3];  /* expected connections to the stdio process */
    int master_port = 0;
    const char *user_shell;
    growstr_t *g;
    int gmpi_port[2];
    int pmi_fd;
    int task_start, task_end;
    const char *mpiexec_redir_helper_path;
    char *psm_uuid = NULL;
    int tv_port = 0;

    /* for looping from 0..numtasks in the case of MPI_Spawn */
    task_start = spawns[spawn].task_start;
    task_end = spawns[spawn].task_end;

    /*
     * Get the pwd.  Probably can trust libc not to overflow this,
     * but who knows.
     */
    if (!getcwd(pwd, sizeof(pwd)))
	error("%s: no current working directory", __func__);
    pwd[sizeof(pwd)-1] = '\0';

    /*
     * Eventually use the user's preferred shell.
     */
    if ((cp = getenv("SHELL")))
	user_shell = cp;
    else if (pswd->pw_shell)
	user_shell = pswd->pw_shell;
    else
	user_shell = "/bin/sh";  /* assume again */

    /*
     * Rewrite argv to go through user's shell, just like rsh.
     *   $SHELL, "-c", "cd <path>; exec <argv0> <argv1>..."
     * But to change the working dir and not frighten weak shells like tcsh,
     * we must detect that the dir actually exists on the far side before
     * committing to the cd.  Use /bin/sh for this task, hoping it exists
     * everywhere we'll be.  Then there's also a bit of quoting nightmare
     * to handle too.  So we'll end up with:
     * rsh node "/bin/sh -c 'if test -d $dir ; then cd $dir ; fi ; $SHELL -c
     *         \'exec argv0 argv1 ...\''"
     * but with argv* (including the executable, argv0) changed to replace
     * all occurrences of ' with '\''.
     */
    nargv[0] = strsave("/bin/sh");  /* assume this exists everywhere */
    nargv[1] = strsave("-c");

    /* exec_line constructed for each process */
    g = growstr_init();

    /*
     * Start stdio stream handler process, if anybody gets stdin,
     * or !nostdout.
     */
    if (cl_args->which_stdin == STDIN_NONE)
	conns[0] = 0;
    else if (cl_args->which_stdin == STDIN_ONE) {
	if (spawn == 0)
	    conns[0] = 1;
	else
	    conns[0] = 0;  /* already connected the single stdin */
    } else if (cl_args->which_stdin == STDIN_ALL) {
	/* total processes which connect stdin */
	conns[0] = 0;
	for (i=task_start; i<task_end; i++)
	    conns[0] += tasks[i].num_copies;
    }

    if (cl_args->nostdout)
	conns[1] = conns[2] = 0;
    else
	/* even for p4 and shmem, not with multiplicity */
  	conns[1] = conns[2] = task_end - task_start;

    /*
     * Initialize listener sockets for gm and ib, since these will be
     * used to implement MPI_Abort in the stdio listener later.
     */
    if (cl_args->comm == COMM_MPICH_GM) {
	prepare_gm_startup_ports(gmpi_port);
    } else if (cl_args->comm == COMM_MPICH_IB) {
	master_port = prepare_ib_startup_port(&gmpi_fd[0]);
	gmpi_fd[1] = -1;
    } else if (cl_args->comm == COMM_MPICH_PSM) {
	master_port = prepare_psm_startup_port(&gmpi_fd[0]);
	gmpi_fd[1] = -1;
    } else if (cl_args->comm == COMM_MPICH_RAI) {
	master_port = prepare_rai_startup_port();
	gmpi_fd[0] = -1;
	gmpi_fd[1] = -1;
    } else {
	gmpi_fd[0] = -1;
	gmpi_fd[1] = -1;
    }

    pmi_fd = -1;
    if (cl_args->comm == COMM_MPICH2_PMI) {
	/* stdio listener handles all PMI activity, even startup */
	if (spawn == 0)
	    master_port = prepare_pmi_startup_port(&pmi_fd);
	else
	    master_port = stdio_msg_parent_say_more_tasks(
	                    task_end - task_start, conns);
    }

    /* flush output buffer, else forked child will have the output too */
    fflush(stdout);

    /* fork the listener (unless we're just spawning more tasks) */
    if (spawn == 0)
	stdio_fork(conns, gmpi_fd, pmi_fd);

    if (pmi_fd >= 0)
	close(pmi_fd);  /* child has it now */

    numtasks_waiting_start = 0;
    if (cl_args->comm == COMM_NONE)
	/* do not complain if they exit before all other tasks are up */
	startup_complete = 1;
    else
	startup_complete = 0;

    /*
     * Start signal handling _after_ stdio child is up.
     */
    handle_signals(0, 0, killall);

    /*
     * environment variables common to all tasks
     */
    env_init();

    /* override user env with these */
    if (cl_args->comm == COMM_MPICH_GM) {
	env_add_int("GMPI_MAGIC", atoi(jobid));
	/* PBS always gives us the "mother superior" node first in the list */
	env_add("GMPI_MASTER", nodes[0].name);
	env_add_int("GMPI_PORT", gmpi_port[0]);   /* 1.2.5..10 */
	env_add_int("GMPI_PORT1", gmpi_port[0]);  /* 1.2.4..8a */
	env_add_int("GMPI_PORT2", gmpi_port[1]);
	env_add_int("GMPI_NP", numtasks);
	env_add_int("GMPI_BOARD", -1);

	/* ditto for new MX version */
	env_add_int("MXMPI_MAGIC", atoi(jobid));
	env_add("MXMPI_MASTER", nodes[0].name);
	env_add_int("MXMPI_PORT", gmpi_port[0]);
	env_add_int("MXMPI_NP", numtasks);
	env_add_int("MXMPI_BOARD", -1);

	/* for MACOSX to override default malloc */
	env_add_int("DYLD_FORCE_FLAT_NAMESPACE", 1);
    }

    if (cl_args->comm == COMM_EMP) {
	growstr_t *emphosts = growstr_init();
	for (i=0; i<numtasks; i++)
	    growstr_printf(emphosts, "%s%s", (i > 0 ? " " : ""),
	      nodes[tasks[i].node].mpname);
	env_add("EMPHOSTS", emphosts->s);
	growstr_free(emphosts);
    }
    
    if (cl_args->comm == COMM_MPICH_IB || cl_args->comm == COMM_MPICH_RAI) {
	int len;
	char *cq, *cr;
	env_add("MPIRUN_HOST", nodes[0].name);  /* master address */
	env_add_int("MPIRUN_PORT", master_port);
	env_add_int("MPIRUN_NPROCS", numtasks);
	env_add_int("MPIRUN_ID", atoi(jobid));  /* global job id */
	/*
	 * pmgr_version >= 3 needs this terribly long string in every task.
	 * Since it may be quite large, we do the allocation by hand and
	 * skip some growstr overhead.
	 */
	len = numtasks;  /* separating colons and terminal \0 */
	for (i=0; i<numtasks; i++)
	    len += strlen(nodes[tasks[i].node].name);
	cq = cp = Malloc(len);
	for (i=0; i<numtasks; i++) {
	    for (cr=nodes[tasks[i].node].name; *cr; cr++)
		*cq++ = *cr;
	    *cq++ = ':';
	}
	--cq;
	*cq = '\0';
	env_add("MPIRUN_PROCESSES", cp);
	free(cp);
    }

    if (cl_args->comm == COMM_MPICH2_PMI) {
	growstr_t *hp = growstr_init();
	growstr_printf(hp, "%s:%d", nodes[0].name, master_port);
	env_add("PMI_PORT", hp->s);
	growstr_free(hp);
	if (spawn > 0)
	    env_add_int("PMI_SPAWNED", 1);
    }

    if (cl_args->comm == COMM_PORTALS) {
	growstr_t *nidmap = growstr_init();
	growstr_t *pidmap = growstr_init();
	portals_build_nidpid_maps(spawn, nidmap, pidmap);
	env_add("PTL_NIDMAP", nidmap->s);
	env_add("PTL_PIDMAP", pidmap->s);
	growstr_free(nidmap);
	growstr_free(pidmap);
    	env_add("PTL_IFACE", "eth0");  /* XXX: no way to know */
    }

    if (cl_args->comm == COMM_MPICH_P4 && numtasks > 1)
	master_port = prepare_p4_master_port();

    if (cl_args->comm == COMM_MPICH_PSM) {
	/* We need to generate a uuid of the form
	 * 9dea0f22-39a4-462a-80c9-b60b28cdfd38.  If /usr/bin/uuidgen exists,
	 * we should probably just use that.
	 * 4bytes-2bytes-2bytes-2bytes-6bytes
	 */
	char uuid_packed[16];
	unsigned char *p = (unsigned char *) uuid_packed;
	int fd, rret;
	
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
	    error_errno("%s: open /dev/urandom", __func__);
	rret = read_full_ret(fd, uuid_packed, sizeof(uuid_packed));
	if (rret < 0)
	    error_errno("%s: read /dev/urandom", __func__);
	if (rret != sizeof(uuid_packed))
	    error("%s: short read /dev/urandom", __func__);
	close(fd);
	psm_uuid = Malloc(37);  /* 16 * 2 + 4 + 1 */
	snprintf(psm_uuid, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x-%02x%02x%02x%02x%02x%02x",
		 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
		 p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
	psm_uuid[36] = '\0';
    }

    /*
     * Ports on which to talk to listener process for stdout/stderr
     * connection (if !-nostdout).
     */
    if (stdio_port(1) >= 0)
	env_add_int("MPIEXEC_STDOUT_PORT", stdio_port(1));
    if (stdio_port(2) >= 0)
	env_add_int("MPIEXEC_STDERR_PORT", stdio_port(2));

    /*
     * Add our hostname too, for use by the redir-helper.  And resolve
     * it now via the user's path for use by the spawns.
     */
    if (HAVE_PBSPRO_HELPER) {
	env_add("MPIEXEC_HOST", nodes[0].name);
	mpiexec_redir_helper_path = resolve_exe("mpiexec-redir-helper", 1);
    }


    /* now the env as given from pbs */
    env_add_environ();

    /* if pbs did not give us these, put in some defaults */
    env_add_if_not("PATH", _PATH_DEFPATH);
    env_add_if_not("USER", pswd->pw_name);


    /*
     * Set up for totalview attach.  Returns local port number that will be
     * used in command startup to tell processes how to find us.  These two
     * env vars are necessary in all processes.  The first tells them to
     * consume the tv_ready message.  The second is checked in MPI_Init to
     * determine if they should wait for all processes to be attached by
     * totalview.
     */
    if (cl_args->tview && cl_args->comm == COMM_MPICH2_PMI) {
	env_add_int("PMI_TOTALVIEW", 1);
	env_add_int("MPIEXEC_DEBUG", 1);
	tv_port = tv_startup(task_end - task_start);
    }

    /*
     * Spawn each task, adding its private env vars.
     * numspawned set to zero earlier before signal handler setup;
     * both it and i walk the iterations in the loop.
     */
    for (i=task_start; i<task_end; i++) {
	env_push();
	if (cl_args->comm == COMM_MPICH_GM) {
	    /* build proc-specific gmpi_opts in envp */
	    env_add_int("GMPI_ID", i);
	    env_add_int("MXMPI_ID", i);
	    env_add("GMPI_SLAVE", nodes[tasks[i].node].name);  /* 1.2.5..10 */
	}
	if (cl_args->comm == COMM_SHMEM) {
	    /* earlier in get_hosts we checked that there is only one task */
	    env_add_int("MPICH_NP", tasks[0].num_copies);
	}

	if (cl_args->comm == COMM_MPICH_IB || cl_args->comm == COMM_MPICH_RAI)
	    env_add_int("MPIRUN_RANK", i);

	if (cl_args->comm == COMM_MPICH_IB) {
	    /* hack for topspin adaptation of mvapich 0.9.2 */
	    env_add("MPIRUN_NODENAME", nodes[tasks[i].node].name);
	}

	if (cl_args->comm == COMM_MPICH2_PMI) {
	    /* task id is always 0-based, even for spawn  */
	    env_add_int("PMI_ID", i - task_start);
	    if (strcmp(nodes[tasks[i].node].mpname,
	               nodes[tasks[i].node].name) != 0)
		env_add("MPICH_INTERFACE_HOSTNAME",
		        nodes[tasks[i].node].mpname);
	}

	if (cl_args->comm == COMM_MPICH_PSM) {
	    /* build one big string with everything in it */
	    char buf[2048];
	    snprintf(buf, sizeof(buf) - 1,
		     "%d %d %s %d %d %d %d %d %d %d %s",
		     0,    /* protocol version */
		     0x4,  /* protocol flags, ASYNC_SHUTDOWN=0x4 */
		     nodes[0].name,  /* spawner host */
		     master_port,    /* spawner port */
		     atoi(jobid),    /* spawner jobid */
		     numtasks,       /* COMM_WORLD size */
		     i - task_start, /* COMM_WORLD rank for this process */
		     nodes[tasks[i].node].numcpu, /* num local ranks */
		     tasks[i].cpu_index[0],       /* my local rank */
		     60, /* timeout... */
		     psm_uuid);
	    buf[sizeof(buf) - 1] = '\0';
	    env_add("MPI_SPAWNER", buf);
	}

	if (cl_args->comm == COMM_PORTALS)
	    env_add_int("PTL_MY_RID", i);

	if (cl_args->comm == COMM_NONE)
	    env_add_int("MPIEXEC_RANK", i);
        
	/* either no stdin, or just to proc #0, or to all of them */
	if (cl_args->which_stdin == STDIN_ONE && i == 0) {
	    env_add_int("MPIEXEC_STDIN_PORT", stdio_port(0));
	    /* do not add _HOST for p4, since we don't want
	     * the children of the big or remote master to
	     * connect.  This _PORT is just for PBS, not for MPICH.  */
	}
	if (cl_args->which_stdin == STDIN_ALL) {
	    env_add_int("MPIEXEC_STDIN_PORT", stdio_port(0));
	    if (cl_args->comm == COMM_MPICH_P4)
		/* slave processes need to be told which host, as the stdin
		 * connection happens not in pbs_mom, but in mpich/p4 library
		 * code when it spawns each of the other tasks. */
		env_add("MPIEXEC_STDIN_HOST", nodes[0].name);
	}

	env_terminate();

	/* build proc-specific command line */
	growstr_zero(g);
	g->translate_single_quote = 0;

	/*
	 * Totalview is a bit odd, even hackish perhaps.  Send the pid
	 * the just-starting process to ourselves via /dev/tcp, some sort
	 * of virtual device that makes a TCP connection as told and sends
	 * the echoed data.
	 */
	if (cl_args->tview && cl_args->comm == COMM_MPICH2_PMI)
	    growstr_printf(g, "if hash nc > /dev/null; then printf %%10d $$ | nc %s %d; else printf %%10d $$ > /dev/tcp/%s/%d; fi; "
	    		   "if test -d \"%s\"; then cd \"%s\"; fi; exec %s -c ",
			   nodes[0].name, tv_port,
			   nodes[0].name, tv_port,
			   pwd, pwd, user_shell);
	else
	    growstr_printf(g,
			   "if test -d \"%s\"; then cd \"%s\"; fi; exec %s -c ",
			   pwd, pwd, user_shell);
	growstr_append(g, "'exec ");
	g->translate_single_quote = 1;

	/*
	 * PBSPro environments do not know how to redirect standard streams.
	 * So we fork a helper program that lives in the user's PATH, hopefully
	 * the same place as mpiexec, that does the redirection then execs the
	 * actual executable.  This will break on OpenPBS or Torque, although
	 * I guess the redir helper could unset the env vars, but I'd rather
	 * people just didn't use the redir helper in that case.
	 */
	if (HAVE_PBSPRO_HELPER)
	    growstr_printf(g, "%s ", mpiexec_redir_helper_path);

	/*
	 * The executable, or a debugger wrapper around it.  In the mpich2
	 * case we don't need any special args.
	 */
	if (cl_args->tview && cl_args->comm != COMM_MPICH2_PMI) {
	    if (i == 0)
		growstr_printf(g, "%s %s -a -mpichtv", tvname,
			       tasks[i].conf->exe);
	    else
		growstr_printf(g, "%s -mpichtv", tasks[i].conf->exe);
	} else
	    growstr_printf(g, "%s", tasks[i].conf->exe);

	/* process arguments _before_ p4 arguments to allow xterm/gdb hack */
	if (tasks[i].conf->args)
	    growstr_printf(g, " %s", tasks[i].conf->args);

	if (cl_args->comm == COMM_MPICH_P4) {
	    /*
	     * Pass the cwd to ch_p4, else it tries to chdir(exedir).  Thanks
	     * to Ben Webb <ben@bellatrix.pcl.ox.ac.uk> for fixing this.
	     */
	    growstr_printf(g, " -p4wd %s", pwd);

	    /* The actual flag names are just for debugging; they're not used
	     * but the order is important. */
	    growstr_printf(g, " -execer_id mpiexec");
	    growstr_printf(g, " -master_host %s", nodes[tasks[0].node].mpname);
	    growstr_printf(g, " -my_hostname %s", nodes[tasks[i].node].mpname);
	    growstr_printf(g, " -my_nodenum %d", i);
	    growstr_printf(g, " -my_numprocs %d", tasks[i].num_copies);
	    growstr_printf(g, " -total_numnodes %d", numtasks);
	    growstr_printf(g, " -master_port %d", master_port);
	    if (i == 0 && numtasks > 1) {
		int j;
		/* list of: <hostname> <procs-on-that-node> */
		growstr_printf(g, " -remote_info");
		for (j=1; j<numtasks; j++)
		    growstr_printf(g, " %s %d",
		      nodes[tasks[j].node].mpname, tasks[j].num_copies);
	    }
	}

	g->translate_single_quote = 0;
	growstr_printf(g, "'");  /* close quote for 'exec myjob ...' */
	nargv[2] = g->s;

	/*
	 * Dump all the info if sufficiently verbose.
	 */
	debug(2, "%s: command to %d/%d %s: %s", __func__, i, numtasks,
	  nodes[tasks[i].node].name, nargv[2]);
	if (cl_args->verbose > 2) {
	    int j;
	    debug(3, "%s: environment to %d/%d %s", __func__, i,
	      numtasks, nodes[tasks[i].node].name);
	    for (j=0; (cp = envp[j]); j++)
		printf("env %2d %s\n", j, cp);
	}

	if (concurrent_master) {
	    tm_event_t evt;
	    int err;

	    /* Note, would like to add obit immediately, but that is
	     * not allowed until the START message is polled.
	     */
	    err = tm_spawn(list_count(nargv), nargv, envp,
	                   nodes[tasks[i].node].ids[tasks[i].cpu_index[0]],
			   &tasks[i].tid, &evt);
	    if (err != TM_SUCCESS)
		error_tm(err, "%s: tm_spawn task %d", __func__, i);
	    evt_add(evt, -1, i, EVT_START);
	} else {
	    concurrent_request_spawn(i, list_count(nargv), nargv, envp,
	      nodes[tasks[i].node].ids[tasks[i].cpu_index[0]]);
	}
	tasks[i].done = DONE_NOT;  /* has now been started */
	env_pop();
	++numspawned;
	++numtasks_waiting_start;

	if (cl_args->comm == COMM_MPICH_P4 && i == 0 && numtasks > 1) {
	    ret = wait_task_start();
	    if (ret)
		break;  /* don't bother trying to start the rest */
	    ret = read_p4_master_port(&master_port);
	    if (ret)
		break;
	}

	/*
	 * Pay attention to incoming tasks so they don't time out while
	 * we're starting up all the others, non blocking.
	 */
	if (cl_args->comm == COMM_MPICH_IB) {
	    int one = 1;
	    for (;;) {
		ret = service_ib_startup(one);
		one = 0;  /* only report the new task that first time */
		if (ret < 0) {
		    ret = 1;
		    goto out;
		}
		if (ret == 0)  /* nothing accomplished */
		    break;
	    }
	}
	if (cl_args->comm == COMM_MPICH_GM) {
	    int one = 1;
	    for (;;) {
		ret = service_gm_startup(one);
		one = 0;  /* only report the new task that first time */
		if (ret < 0) {
		    ret = 1;
		    goto out;
		}
		if (ret == 0)  /* nothing accomplished */
		    break;
	    }
	}
	if (cl_args->comm == COMM_MPICH_PSM) {
	    int one = 1;
	    for (;;) {
		ret = service_psm_startup(one);
		one = 0;  /* only report the new task that first time */
		if (ret < 0) {
		    ret = 1;
		    goto out;
		}
		if (ret == 0)  /* nothing accomplished */
		    break;
	    }
	}
	if (cl_args->tview && cl_args->comm == COMM_MPICH2_PMI)
	    tv_accept_one(i);
    }

    if (cl_args->tview && cl_args->comm == COMM_MPICH2_PMI)
       tv_complete();

    /* don't need these anymore */
    free(nargv[0]);
    free(nargv[1]);
    growstr_free(g);

    if (cl_args->comm == COMM_MPICH_PSM)
	free(psm_uuid);
    if (ret)
	goto out;

    /*
     * Wait for spawn events and submit obit requests.
     */
    while (numtasks_waiting_start) {
	ret = wait_task_start();
	if (ret)
	    goto out;
    }

    debug(1, "All %d task%s (spawn %d) started", task_end - task_start,
          task_end - task_start > 1 ? "s": "", spawn);

    /*
     * Finalize mpi-specific startup protocal, e.g. wait for all tasks to
     * checkin, perform barrier, etc.
     */
    if (cl_args->comm == COMM_MPICH_GM)
	ret = read_gm_startup_ports();

    if (cl_args->comm == COMM_MPICH_IB)
	ret = read_ib_startup_ports();

    if (cl_args->comm == COMM_MPICH_PSM)
	ret = read_psm_startup_ports();

    if (cl_args->comm == COMM_MPICH_RAI)
	ret = read_rai_startup_ports();

    if (ret == 0)
	startup_complete = 1;

  out:
    return ret;
}

/*
 * Poll until a START event happens, doesn't matter which task, and
 * post an obit for it immediately, hoping that we won't lose the exit
 * status if it dies quickly.
 *
 * Return 1 if something died while we were waiting.
 */
static int
wait_task_start(void)
{
    int ret = 0;
    evts_t *ep;
    int numtasks_waiting_start_entry = numtasks_waiting_start;
    int numspawned_entry = numspawned;

    for (;;) {
	/*
	 * Poke startup too, like in the main spawn loop.
	 */
	if (cl_args->comm == COMM_MPICH_IB) {
	    for (;;) {
		ret = service_ib_startup(0);
		if (ret < 0) {
		    ret = 1;
		    break;
		}
		if (ret == 0)  /* nothing accomplished */
		    break;
	    }
	}
	if (cl_args->comm == COMM_MPICH_GM) {
	    for (;;) {
		ret = service_gm_startup(0);
		if (ret < 0) {
		    ret = 1;
		    break;
		}
		if (ret == 0)  /* nothing accomplished */
		    break;
	    }
	}

	ep = poll_event();
	if (ep)
	    dispatch_event(ep);
	else
	    usleep(200000);

	if (ret || numtasks_waiting_start_entry != numtasks_waiting_start)
	    break;
    }

    if (numspawned_entry != numspawned)
	ret = 1;
    return ret;
}

