/*
 * mpiexec.c - gather node settings from PBS, prepare for MPI runtime
 * environment and start tasks through the pbs task manager interface.
 * Attempts to duplicate mpirun  as much as possible, while getting
 * everything correct, and being faster than rsh.
 *
 * $Id: mpiexec.c 420 2008-04-10 21:40:21Z pw $
 *
 * Copyright (C) 2000-8 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#define _GNU_SOURCE  /* hoping to get strsignal() from string.h */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <pwd.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>       /* gethostbyname */
#include "mpiexec.h"

/*
 * Define some globals.
 */
nodes_t *nodes;
tasks_t *tasks;
spawns_t *spawns;
cl_args_t *cl_args;

const char *progname;
char *progname_dir;
char *jobid;
int numnodes;
int numtasks;
int numspawns;
struct passwd *pswd;
struct sockaddr_in myaddr;
char *tvname;

/*
 * Ensure it's executable.  Return true if so.
 */
int
stat_exe(const char *exe, int complain)
{
    struct stat sb;
    int rc;

    debug(3, "%s: testing \"%s\"", __func__, exe);
    rc = stat(exe, &sb);
    if (rc < 0) {
	if (complain)
	    error_errno("%s: %s", __func__, exe);
	return 0;
    }
    if (!S_ISREG(sb.st_mode)) {
	if (complain)
	    error("%s: file %s is not a regular file", __func__, exe);
	return 0;
    }
    if (access(exe, X_OK) < 0) {
	if (complain)
	    error("%s: file %s is not executable", __func__, exe);
	return 0;
    }
    return 1;
}

/*
 * Ensure the executable is in the user's path, or if it's in the cwd,
 * add a "." assuming that's what they meant.  XXX: Security risk?
 * Always returns a new string.
 */
char *
resolve_exe(const char *exe, int argv0_dir)
{
    const char *cp, *cq;
    growstr_t *g;

    /* absolute or relative (non-pathed) location? */
    if (strchr(exe, '/')) {
	stat_exe(exe, 1);
	debug(1, "%s: using absolute path \"%s\"", __func__, exe);
	return strsave(exe);
    }

    g = growstr_init();

    /* if requested, and there was a path-based invocation, lookup in the
     * same directory as argv[0], first */
    if (argv0_dir && progname_dir) {
	growstr_zero(g);
	/* already includes trailing slash */
	growstr_printf(g, "%s%s", progname_dir, exe);
	if (stat_exe(g->s, 0)) {
	    char *ret;
	    debug(1, "%s: found \"%s\" in argv[0] dir", __func__, g->s);
	    ret = strsave(g->s);
	    growstr_free(g);
	    return ret;
	}
    }
    
    /* look in path */
    cp = getenv("PATH");
    if (cp) {
	while (*cp) {
	    cq = strchr(cp, ':');
	    if (!cq)
		cq = cp + strlen(cp);

	    if (cq != cp) {
		growstr_zero(g);
		growstr_append(g, cp);
		g->s[g->len = cq-cp] = 0;  /* just up to, not incl, ':' */
		growstr_printf(g, "/%s", exe);
		if (stat_exe(g->s, 0)) {
		    debug(1, "%s: found \"%s\" in path", __func__, g->s);
		    growstr_free(g);
		    return strsave(exe);
		}
	    }
	    cp = cq;
	    if (*cp)
		++cp;  /* skip : */
	}
    }

    /* look in . */
    if (stat_exe(exe, 0)) {
	char *ret;
	growstr_zero(g);
	growstr_printf(g, "./%s", exe);
	debug(1, "%s: found \"%s\" in current directory", __func__, g->s);
	ret = strsave(g->s);
	growstr_free(g);
	return ret;
    }

    error("%s: executable \"%s\" not found in path or current dir",
      __func__, exe);

    /*NOTREACHED*/
    return 0;
}

/*
 * Convert a unix signal number to a symbolic form.  If the handy function
 * does not exist, just look for a few of the more popular ones.
 */
const char *
parse_signal_number(int sig)
{
#ifdef HAVE_STRSIGNAL
    const char *s = strsignal(sig);
    if (!s)
	s = "unknown";
    return s;
#else
    /* just try to get some of the big ones */
#   if defined(SIGILL)
    if (sig == SIGILL) return "SIGILL";
#   endif
#   if defined(SIGBUS)
    if (sig == SIGBUS) return "SIGBUS";
#   endif
#   if defined(SIGKILL)
    if (sig == SIGKILL) return "SIGKILL";
#   endif
#   if defined(SIGSEGV)
    if (sig == SIGSEGV) return "SIGSEGV";
#   endif
#   if defined(SIGTERM)
    if (sig == SIGTERM) return "SIGTERM";
#   endif
    return "unknown";
#endif
}

static int killall_sig = 0;
static jmp_buf jmp_env;

/*
 * Signal handling.
 */
void
killall(int sig)
{
    static int killall_count = 0;

    debug(1, "%s: caught signal %d (%s)", __func__, sig,
      parse_signal_number(sig));
    ++killall_count;
    killall_sig = sig;
    longjmp(jmp_env, killall_count);
}

/*
 * Enable one signal handler for a list of signals.  Do not defer
 * signal reception while handling these, to let the impatient user
 * interrupt again to really exit.
 */
void
handle_signals(const int *list, int num, void (*handler)(int sig))
{
    const int default_siglist[] = { SIGHUP, SIGINT, SIGTERM };
    struct sigaction act;
    int i, ret;

    if (!list) {
	list = default_siglist;
	num = list_count(default_siglist);
    }
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER;
    act.sa_handler = handler;
    for (i=0; i<num; i++) {
	ret = sigaction(list[i], &act, 0);
	if (ret < 0)
	    error_errno("%s: sigaction %d", __func__, list[i]);
    }
}

/*
 * Just print a little version string.
 */
static void
version(FILE *fp)
{
    if (!strcmp(CONFIGURE_OPTIONS, ""))
	fprintf(fp, "Version %s, no configure options\n", VERSION);
    else
	fprintf(fp, "Version %s, configure options: %s\n", VERSION,
	  CONFIGURE_OPTIONS);
}

/*
 * Environment variable which can specify -comm if no command-line argument
 * is given.
 */
static const char MPIEXEC_COMM_ENV[] = "MPIEXEC_COMM";

/*
 * String version of communication library name.
 */
static const char *
comm_name(int which)
{
    static const struct {
	int num;
	const char *const name;
    } name[] = {
	{ COMM_MPICH_GM, "mpich-gm" },
	{ COMM_MPICH_P4, "mpich-p4" },
	{ COMM_MPICH_IB, "mpich-ib" },
	{ COMM_MPICH_PSM, "mpich-psm" },
	{ COMM_MPICH_RAI, "mpich-rai" },
	{ COMM_MPICH2_PMI, "mpich2-pmi" },
	{ COMM_LAM, "lam" },
	{ COMM_SHMEM, "shmem" },
	{ COMM_EMP, "emp" },
	{ COMM_PORTALS, "portals" },
	{ COMM_NONE, "none" },
    };
    int i;

    for (i=0; i<list_count(name); i++)
	if (name[i].num == which)
	    return name[i].name;
    return "unknown-comm";
}

/*
 * Usage.
 */
static void
usage(void)
{
    fprintf(stderr, "Usage: %s [<args>] <executable> [<exe args>]...\n",
      progname);
    fprintf(stderr, "   or: %s [<args>] -config[=]<file>\n",
      progname);
    fprintf(stderr, "   or: %s [<args>] -server\n",
      progname);
    fprintf(stderr,
      "  -n <numproc> : use only some of the allocated processors\n");
    fprintf(stderr,
      "     Default behavior allocates one process per allocated processor.\n");
    fprintf(stderr,
      "  -verbose : be verbose about mpiexec operation\n");
    fprintf(stderr,
      "  -nostdin : do not listen to stdin, allowing process to go into background\n");
    fprintf(stderr,
      "  -allstdin : send stdin to all processes (default just proc #0)\n");
    fprintf(stderr,
      "  -nostdout : do not redirect stdout/stderr, but let pbs accumulate it\n");
    fprintf(stderr,
      "  -comm (gm|mx|p4|ib|rai|pmi|lam|shmem|emp|portals|none) : choose MPI (default %s)\n",
      comm_name(COMM_DEFAULT));
    fprintf(stderr,
      "    -mpich-p4-[no-]shmem : for MPICH/P4, specify if the library was\n"
      "                           compiled with shared memory support (default %s)\n", HAVE_P4_SHMEM ? "yes" : "no");
    fprintf(stderr,
      "  -pernode : allocate only one process per compute node\n");
    fprintf(stderr,
      "  -npernode <nprocs> : allocate no more than <nprocs> processes per compute node\n");
    fprintf(stderr,
      "  -nolocal : do not run any MPI processes on the local node\n");
    if (HAVE_SED)
	fprintf(stderr,
	  "  -transform-hostname[=]<sed expression> : use alternate names for MPI\n");
    fprintf(stderr,
      "  -transform-hostname-program[=]<executable> : use this script or program\n"
      "                                               to generate alternate names\n");
    fprintf(stderr,
      "  -tv : debug using totalview (ensure it is in your path)\n");
    fprintf(stderr,
      "  -kill : kill other processes if any one process exits\n");
    fprintf(stderr,
      "  -config[=]<file> : use heterogenous node specification file (\"-\" for stdin)\n");
    fprintf(stderr,
      "  -server : do not run any tasks, just serve other concurrent mpiexec clients\n");
    fprintf(stderr,
      "  -version : show version information\n");
    version(stderr);
#if 0  /* No this doesn't work, but if I don't check it in I'll forget where I put it.  */
    fprintf(stderr,
      "  -output <prefix> : send process output to separate files\n");
#endif
    exit(1);
}

static comm_t
parse_comm(const char *const s, const char *const where)
{
    growstr_t *g;

    if (HAVE_COMM_MPICH_GM)
	if (!strcasecmp(s, "gm") || !strcasecmp(s, "mpich-gm")
	 || !strcasecmp(s, "mpich/gm")
	 || !strcasecmp(s, "mx") || !strcasecmp(s, "mpich-mx")
	 || !strcasecmp(s, "mpich/mx"))
	    return COMM_MPICH_GM;
    if (HAVE_COMM_MPICH_P4)
	if (!strcasecmp(s, "p4") || !strcasecmp(s, "mpich-p4")
	 || !strcasecmp(s, "mpich/p4"))
	    return COMM_MPICH_P4;
    if (HAVE_COMM_MPICH_IB)
	if (!strcasecmp(s, "ib") || !strcasecmp(s, "mpich-ib")
	 || !strcasecmp(s, "mpich/ib") || !strcasecmp(s, "mvapich"))
	    return COMM_MPICH_IB;
    if (HAVE_COMM_MPICH_RAI)
	if (!strcasecmp(s, "rai") || !strcasecmp(s, "mpich-rai")
	 || !strcasecmp(s, "mpich/rai"))
	    return COMM_MPICH_RAI;
    if (HAVE_COMM_MPICH2_PMI)
	if (!strcasecmp(s, "mpich2") || !strcasecmp(s, "mpich2-pmi")
	 || !strcasecmp(s, "mpich2/pmi") || !strcasecmp(s, "pmi"))
	    return COMM_MPICH2_PMI;
    if (HAVE_COMM_LAM)
	if (!strcasecmp(s, "lam"))
	    return COMM_LAM;
    if (HAVE_COMM_SHMEM)
	if (!strcasecmp(s, "shmem"))
	    return COMM_SHMEM;
    if (HAVE_COMM_EMP)
	if (!strcasecmp(s, "emp"))
	    return COMM_EMP;
    if (HAVE_COMM_PORTALS)
	if (!strcasecmp(s, "portals"))
	    return COMM_PORTALS;
    if (HAVE_COMM_NONE)
	if (!strcasecmp(s, "none") || !strcasecmp(s, "no"))
	    return COMM_NONE;

    /* complain */
    g = growstr_init();
    growstr_append(g, "%s: unknown MPI library type \"%s\"");
    if (where)
	growstr_append(g, where);
    growstr_append(g, ".\n");
    growstr_append(g, "Available ones:");
    if (HAVE_COMM_MPICH_GM) growstr_printf(g, " %s", comm_name(COMM_MPICH_GM));
    if (HAVE_COMM_MPICH_P4) growstr_printf(g, " %s", comm_name(COMM_MPICH_P4));
    if (HAVE_COMM_MPICH_IB) growstr_printf(g, " %s", comm_name(COMM_MPICH_IB));
    if (HAVE_COMM_MPICH_RAI)
        growstr_printf(g, " %s", comm_name(COMM_MPICH_RAI));
    if (HAVE_COMM_MPICH2_PMI)
        growstr_printf(g, " %s", comm_name(COMM_MPICH2_PMI));
    if (HAVE_COMM_LAM)      growstr_printf(g, " %s", comm_name(COMM_LAM));
    if (HAVE_COMM_SHMEM)    growstr_printf(g, " %s", comm_name(COMM_SHMEM));
    if (HAVE_COMM_EMP)      growstr_printf(g, " %s", comm_name(COMM_EMP));
    if (HAVE_COMM_PORTALS)  growstr_printf(g, " %s", comm_name(COMM_PORTALS));
    if (HAVE_COMM_NONE)     growstr_printf(g, " %s", comm_name(COMM_NONE));
    growstr_printf(g, " (default %s)", comm_name(COMM_DEFAULT));
    error(g->s, __func__, s);

    /*NOTREACHED*/
    return COMM_UNSET;
}

/*
 * For highly flexible argument parsing, allow an option argument
 * to appear in many places.  The following are all equivalent:
 *   --np=3
 *   --np 3
 *   --np3
 * 
 * Note that the argument talked about here is not optional, it is a
 * required argument to an optional command-line option.
 */
static const char *
find_optarg(const char *cp, int *argcp, const char ***const argvp,
  const char *const which)
{
    /* argument could be in this one, or, if not, in the next arg */
    if (*cp) {
	/* optional = */
	if (*cp == '=')
	    ++cp;
    } else {
	if (++*argvp, --*argcp <= 0)
	    error("%s: option -%s requires an argument", __func__, which);
	cp = **argvp;
    }
    return cp;
}

#define MAX(a,b) ((a) > (b) ? (a) : (b))

/*
 * Chop up the mpiexec args, and return argc/argv which is the
 * parallel code to run, with its args only.
 */
static void
parse_args(int *argcp, const char ***argvp)
{
    int argc = *argcp;
    const char **argv = *argvp;
    int len;
    const char *cp, *cq;
    char *cr;

    /*
     * Look for arguments, which must come before exec and args.
     */
    cl_args = Malloc(sizeof(*cl_args));
    memset(cl_args, 0, sizeof(*cl_args));
    cl_args->which_stdin = STDIN_UNSET;
    cl_args->comm = COMM_UNSET;
    cl_args->mpich_p4_shmem = -1;

    while (++argv, --argc > 0) {
	cp = *argv;
	if (*cp++ != '-') break;
	if (*cp == '-')  ++cp;  /* optional second "-" */
	if ((cq = strchr(cp, '=')))  /* maybe optional = */
	    len = cq - cp;
	else
	    len = strlen(cp);
	if (len < 1) {
	    /* exactly -- means end of options, rest is command */
	    if (strcmp(*argv, "--") == 0) {
		++argv, --argc;
		break;
	    }
	    usage();
	}
	if (!strncmp(cp, "nostdout", MAX(6,len)))
	    cl_args->nostdout = 1;
	else if (!strncmp(cp, "nostdin", MAX(6,len))) {
	    if (cl_args->which_stdin == STDIN_ALL)
		error("arguments -nostdin and -allstdin conflict");
	    cl_args->which_stdin = STDIN_NONE;
	} else if (!strncmp(cp, "allstdin", len)) {
	    if (cl_args->which_stdin == STDIN_NONE)
		error("arguments -nostdin and -allstdin conflict");
	    cl_args->which_stdin = STDIN_ALL;
	} else if (!strncmp(cp, "pernode", MAX(1,len)))
	    cl_args->pernode = 1;
	else if (!strncmp(cp, "npernode", MAX(3,len))) {
	    long l;
	    cp += len;
	    cp = find_optarg(cp, &argc, &argv, "npernode");
	    l = strtol(cp, &cr, 10);
	    if (*cr || l <= 0)
		error("argument -npernode requires positive number of processes");
	    cl_args->pernode = l;
	} else if (!strncmp(cp, "nolocal", MAX(3,len)))
	    cl_args->nolocal = 1;
	else if (HAVE_COMM_MPICH_GM && !strncmp(cp, "no-shmem", MAX(2,len)))
	    warning("-no-shmem ignored, use GMPI_SHMEM=0 or MX_DISABLE_SHMEM=1"
	            " environment variable");
	/* keep this after other "n..." items */
	else if (!strncmp(cp, "np", len)) {
	    long l;
	    cp += len;
	    cp = find_optarg(cp, &argc, &argv, "n");
	    l = strtol(cp, &cr, 10);  /* negative value to strtoul is legal! */
	    if (*cr || l <= 0)
		error("argument -n requires positive integral number of nodes");
	    cl_args->numproc = l;
	} else if (!strcmp(cp, "tv") || !strncmp(cp, "totalview", MAX(2,len))) {
	    char *tvenv;
	    cl_args->tview = 1;
	    tvenv = getenv("TOTALVIEW");
	    if (tvenv != NULL) {
		if (access(tvenv, X_OK) == 0)
		    tvname = strdup(tvenv);
		else {
		    warning("%s: TOTALVIEW env variable \"%s\" not executable, "
			    "trying totalview in PATH\n", __func__, tvname);
		    tvenv = NULL;
		}
	    }
	    if (tvenv == NULL)
		tvname = strdup("totalview");
	}
	else if (!strncmp(cp, "config", MAX(3,len))) {
	    cp += MAX(3,len);
	    cl_args->config_file = find_optarg(cp, &argc, &argv, "config");
	} else if (!strncmp(cp, "kill", len))
	    cl_args->kill_others = 1;
	else if (!strncmp(cp, "version", MAX(4,len))) {
	    version(stdout);
	    exit(0);
	/* keep this after other "v..." items; allows old style -v */
	} else if (!strncmp(cp, "verbose", len))
	    ++cl_args->verbose;
	else if (HAVE_SED && !strncmp(cp, "transform-hostname", MAX(2,len))) {
	    cp += MAX(2,len);
	    cl_args->transform_hostname = find_optarg(cp, &argc, &argv,
	      "transform-hostname");
	} else if (HAVE_SED && !strncmp(cp, "gige", len))
	    cl_args->transform_hostname = "s/node/gige/";
	else if (!strncmp(cp, "transform-hostname-program", MAX(20,len))) {
	    cp += MAX(20,len);
	    cl_args->transform_hostname_program = find_optarg(cp, &argc, &argv,
	      "transform-hostname-program");
	} else if (!strncmp(cp, "comm", MAX(3,len))) {
	    if (cl_args->comm != COMM_UNSET)
		error("only choose one communication library");
	    cp += MAX(3,len);
	    cl_args->comm = parse_comm(
	      find_optarg(cp, &argc, &argv, "comm"), 0);
#if 0
	} else if (!strncmp(cp, "output", MAX(1,len))) {
	    cp += MAX(1,len);
	    cl_args->process_output = find_optarg(cp, &argc, &argv, "output");
#endif
	} else if (!strncmp(cp, "mpich-p4-shmem", len) && len == 14)
	    cl_args->mpich_p4_shmem = 1;
	else if (!strncmp(cp, "mpich-p4-no-shmem", len) && len == 17)
	    cl_args->mpich_p4_shmem = 0;
	else if (!strncmp(cp, "server", len))
	    cl_args->server_only = 1;
	else usage();
    }

    /*
     * A bunch of sanity checks.  Not all options are compatible with
     * each other, or the compile-time configuration.
     */
    if (cl_args->server_only) {
	/* many arguments do not make sense here, try to catch lots */
	if (cl_args->which_stdin != STDIN_UNSET
	 || cl_args->nostdout != 0)
	    error("%s: cannot use stdin/stdout arguments with -server",
	      __func__);
	if (cl_args->comm != COMM_UNSET)
	    error("%s: cannot use -comm argument with -server", __func__);
	if (cl_args->pernode)
	    error("%s: cannot use -pernode argument with -server", __func__);
	if (cl_args->nolocal)
	    error("%s: cannot use -nolocal argument with -server", __func__);
	if (cl_args->transform_hostname)
	    error("%s: cannot use -transform_hostname argument with -server",
	      __func__);
	if (cl_args->tview)
	    error("%s: cannot use -totalview argument with -server", __func__);
	if (cl_args->kill_others)
	    error("%s: cannot use -kill argument with -server", __func__);
	if (cl_args->config_file)
	    error("%s: cannot use -config argument with -server", __func__);
    }

    if (cl_args->which_stdin == STDIN_UNSET)
	cl_args->which_stdin = STDIN_ONE;  /* the default, just proc #0 */

    if (cl_args->comm == COMM_UNSET) {
	/*
	 * Accept setting from environment if none given on command line,
	 * else fall to compiled-in default.
	 */
	const char *comm_env = getenv(MPIEXEC_COMM_ENV);
	if (comm_env) {
	    growstr_t *g = growstr_init();
	    growstr_printf(g, "\n  in environment variable \"%s\"",
	      MPIEXEC_COMM_ENV);
	    cl_args->comm = parse_comm(comm_env, g->s);
	    growstr_free(g);
	} else
	    cl_args->comm = COMM_DEFAULT;
    }

    if (cl_args->mpich_p4_shmem == -1) {
	if (cl_args->comm == COMM_MPICH_P4)
	    cl_args->mpich_p4_shmem = HAVE_P4_SHMEM;  /* configure default */
    } else {
	if (cl_args->comm != COMM_MPICH_P4)
	    warning("%s: argument \"-mpich-p4-[no-]shmem\" ignored since\n"
	      "  communication library not MPICH/P4", __func__);
    }

    if (cl_args->config_file && !strcmp(cl_args->config_file, "-"))
	if (cl_args->which_stdin != STDIN_NONE) {
	    warning("reading the config file from stdin forces -nostdin");
	    cl_args->which_stdin = STDIN_NONE;
	}

    if (cl_args->transform_hostname && cl_args->transform_hostname_program)
	error("-transform-hostname and -transform-hostname-program conflict");

#if 0
    if (cl_args->process_output && cl_args->nostdout)
        warning("-output ignored since -nostdout specified");
#endif

    /*
     * Get full path to executable given on command line, resolved using
     * current PATH setting
     */
    if (cl_args->config_file) {
	if (argc != 0)
	    error("%s: extra command-line arguments with -config flag",
	      __func__);
    } else if (cl_args->server_only) {
	if (argc != 0)
	    error("%s: extra command-line arguments with -server flag",
	      __func__);
    } else {
	if (argc < 1)
	    usage();
	argv[0] = resolve_exe(argv[0], 0);
    }
    *argcp = argc;
    *argvp = argv;
}

static void show_exit_statuses(void);

int
main(int argc, const char *argv[])
{
    int i, j, jmp_return, ret;
    struct passwd *pswd_tmp;

    set_progname(argc, argv);
    parse_args(&argc, &argv);
    stdio_notice_streams();

    jobid = getenv("PBS_JOBID");
    if (!jobid)
	error("PBS_JOBID not set in environment.  Code must be run from a\n"
	      "  PBS script, perhaps interactively using \"qsub -I\"");

    /* copy the static passwd struct, since tm_ calls will overwrite it */
    pswd_tmp = getpwuid(getuid());
    if (!pswd_tmp)
	error("%s: no passwd entry for uid %d", __func__, (int) getuid());
    pswd = Malloc(sizeof(*pswd));
    memcpy(pswd, pswd_tmp, sizeof(*pswd));

    /* see if there is a master socket in the case of concurrent mpiexec */
    concurrent_init();

    if (cl_args->server_only && !concurrent_master)
	error("%s: not concurrent master yet -server flag specified", __func__);

    /*
     * Reset signals to a sane state, in case we were spawned weirdly.  Saw
     * a wrapper script that sets SIGCHLD to SIG_IGN, but this won't fly
     * when the popen() in PBSD_authenticate forks and waits.
     */
    {
	const int siglist[] = { SIGHUP, SIGTERM, SIGQUIT, SIGCHLD };
	handle_signals(siglist, list_count(siglist), SIG_DFL);
    }

    /* get taskids from tm, then hostnames from pbs */
    if (concurrent_master)
	get_hosts();
    else
	concurrent_get_nodes();

    if (cl_args->verbose)
	for (i=0; i<numnodes; i++)
	    printf("node %2d: name %s, cpu avail %d\n", i,
	      nodes[i].name, nodes[i].availcpu);

#if 0
    {
	/* debug, cannot strace until after setuid pbs stuff done
	 * in get_hosts(). */
	char s[256];
	sprintf(s, "strace -tt -T -vFf -s 2000 -o o%d -p %d > /dev/null 2>&1 &",
	  getpid(), getpid());
	system(s);
	sleep(1);  /* wait for attach */
    }
#endif

    if (cl_args->server_only) {
	cm_permit_new_clients(1);
	handle_signals(0, 0, killall);
	numtasks = 0;
	numspawns = 0;
	numspawned = 0;
	pipe_with_stdio = -1;
	goto server_only;
    }

    /*
     * Now look at the command-line constraints.
     */
    constrain_nodes();

    /*
     * Finally map the config file requirements onto the available tasks,
     * or for command-line, let them all do the same thing.
     */
    if (cl_args->config_file)
	parse_config();
    else
	argcv_config(argc, argv);

    /* 
     * Identify nodes with multiple identical jobs and squeeze them
     * down since mpich/p4 (and shmem) will use shmem on the same node.
     * This reduces numtasks since we have fewer tasks to spawn.
     */
    tasks_shmem_reduce();

    /*
     * Build the initial spawn group.
     */
    numspawns = 1;
    spawns = Malloc(numspawns * sizeof(*spawns));
    spawns[0].task_start = 0;
    spawns[0].task_end = numtasks;
    /* this indirection is to keep the obit pointers constant while
     * we move around the tasks array */
    spawns[0].obits = Malloc(numtasks * sizeof(*spawns[0].obits));
    spawns[0].ranks2hosts_response = NULL;
    for (i=0; i<numtasks; i++)
	tasks[i].status = &spawns[0].obits[i];

    /*
     * Count the nodes as allocated
     */
    if (concurrent_master) {
	for (i=0; i<numtasks; i++) {
	    nodes[tasks[i].node].cm_availcpu -= tasks[i].num_copies;
	    for (j=0; j<tasks[i].num_copies; j++)
		nodes[tasks[i].node].cm_cpu_free[tasks[i].cpu_index[j]] = 1;
	}
	cm_permit_new_clients(1);
    } else
	concurrent_node_alloc();

    /*
     * Figure out who I am for MPI startup.
     * 
     * Many MPI libs use some out-of-band startup protocol that runs over
     * the same communication fabric that PBS uses, some sort of standard
     * ethernet usually.  Mpiexec serves as the focal point of that protocol
     * and thus will create a listening socket bound to the IP of the
     * hostname used by the other tasks as they try to connect to their
     * startup host.
     *
     * We don't allow for mpiexec to be run from any node other than the
     * mother superior, so this is always nodes[0] that fills this role.
     *
     * Hope that the DNS lookup for our hostname returns only one IP, since
     * we only use the first in the list.
     */
    if (cl_args->comm == COMM_MPICH_GM || cl_args->comm == COMM_MPICH_IB
      || cl_args->comm == COMM_MPICH_P4 || cl_args->comm == COMM_MPICH2_PMI
      || cl_args->comm == COMM_MPICH_RAI) {
	struct hostent *he;

	he = gethostbyname(nodes[0].name);
	if (!he)
	    error("%s: gethostbyname cannot find my name %s", __func__,
	      nodes[0].name);
	if (he->h_length != sizeof(myaddr.sin_addr))
	    error("%s: gethostbyname returns %d-byte addresses, expecting %d",
	      __func__, he->h_length, (int) sizeof(myaddr.sin_addr));
	myaddr.sin_family = (unsigned short) he->h_addrtype;
	memcpy(&myaddr.sin_addr, he->h_addr_list[0], sizeof(myaddr.sin_addr));
    }

    /*
     * Run the tasks and wait for them to finish.  Use of setjmp is
     * to avoid complex shutdown activity in the signal handler.
     */
  server_only:
    jmp_return = setjmp(jmp_env);
    switch (jmp_return) {
        case 0:
	    if (cl_args->server_only) {
		cm_serve_clients();
		concurrent_exit();   /* not reached, wait for ctrl-c path */
	    } else {
		/* return value ignored; it changes cs->exe if okay */
		distribute_executable();
		ret = start_tasks(0);
		if (ret)
		    kill_tasks(SIGTERM);
		wait_tasks();
	    }
	    break;

	/* reached by setjmp return likely */
	case 1:
	    kill_tasks(killall_sig);
	    if (concurrent_master) {
		int num_clients_killed;

		cm_permit_new_clients(0);  /* no new connections */
		num_clients_killed = cm_kill_clients();
		/*
		 * If no clients were connected, hand out exit-status 0 as
		 * the only way a server-only master can exit is with a signal
		 * and it may have been intended.
		 */
		if (num_clients_killed == 0 && cl_args->server_only)
		    jmp_return = 0;
	    }
	    wait_tasks();
	    /* if master, we will end up waiting for client events/tasks */
	    break;

	/* second ctrl-c, don't try to communicate with anything, just die */
	default:
	    handle_signals(0, 0, SIG_DFL);
	    break;
    }

    /* deallocate our tasks */
    if (concurrent_master)
	for (i=0; i<numtasks; i++) {
	    nodes[tasks[i].node].cm_availcpu += tasks[i].num_copies;
	    for (j=0; j<tasks[i].num_copies; j++)
		nodes[tasks[i].node].cm_cpu_free[tasks[i].cpu_index[j]] = 0;
	}

    /*
     * Tell the stdio thread to exit, show status of finished tasks, disconnect
     * from other mpiexecs.  Nothing here waits.
     */
    kill_stdio();
    show_exit_statuses();
    if (concurrent_master)
	cm_serve_clients();
    handle_signals(0, 0, SIG_DFL);
    concurrent_exit();

    free(cl_args);

    if (numtasks)
	return *tasks[0].status;
    else
	return jmp_return;
}

/*
 * Attempt to shrink output if lots of tasks died for the same
 * reason.  Most of the code below just implements some auto-
 * growing arrays.  Could extentd growstr to be generic, but
 * by then might as well switch to C++ and use some templated
 * class library.
 */
static void
show_exit_statuses(void)
{
    struct {
	int status;
	done_how_t done;
	int *tasks;
	int numtasks;
	int maxtasks;
    } *slist = 0;
    int numslist = 0;
    int maxslist = 0;
    int i, j;
    growstr_t *g = growstr_init();

    /* assume okay if we didn't actually get an exit status; and null out
     * the statuses that were never filled */
    for (i=0; i<numtasks; i++) {
	/* if we died, abandoning tasks, perhaps from lots of ctrl-c, do not
	 * report any exit status */
	/* do not report anything if we never got an exit status */
	if (tasks[i].done == DONE_NOT) {
	    debug(2, "%s: task %d was not done", __func__, i);
	    tasks[i].done = DONE_OK;
	    *tasks[i].status = 0;
	}
	if (tasks[i].done == DONE_NO_EXIT_STATUS) {
	    debug(2, "%s: task %d had no exit status", __func__, i);
	    tasks[i].done = DONE_OK;
	    *tasks[i].status = 0;
	}
	/* null out the statuses that were never filled */
	if (tasks[i].done == DONE_NOT_STARTED) {
	    *tasks[i].status = 0;
	}
    }

    for (i=0; i<numtasks; i++) {
	if (*tasks[i].status == 0 && tasks[i].done == DONE_OK)
	    continue;
	for (j=0; j<numslist; j++)
	    if (slist[j].status == *tasks[i].status
	     && slist[j].done == tasks[i].done)
		break;
	if (j == numslist) {
	    if (numslist == maxslist) {
		void *x = slist;
		maxslist += 10;
		slist = Malloc(maxslist * sizeof(*slist));
		if (x) {
		    memcpy(slist, x, numslist * sizeof(*slist));
		    free(x);
		}
	    }
	    slist[j].status = *tasks[i].status;
	    slist[j].done = tasks[i].done;
	    slist[j].tasks = 0;
	    slist[j].numtasks = 0;
	    slist[j].maxtasks = 0;
	    ++numslist;
	}
	if (slist[j].numtasks == slist[j].maxtasks) {
	    void *x = slist[j].tasks;
	    slist[j].maxtasks += 10;
	    slist[j].tasks = Malloc(slist[j].maxtasks
	      * sizeof(*slist[j].tasks));
	    if (x) {
		memcpy(slist[j].tasks, x, slist[j].numtasks
		  * sizeof(*slist[j].tasks));
		free(x);
	    }
	}
	slist[j].tasks[slist[j].numtasks] = i;
	++slist[j].numtasks;
    }

    for (j=0; j<numslist; j++) {
	int inrange, needcomma, needdash;

	growstr_zero(g);
	growstr_printf(g, "task%s ", slist[j].numtasks > 1 ? "s" : "");

	inrange = -1;
	needcomma = 0;
	needdash = 0;
	for (i=0; i<slist[j].numtasks; i++) {
	    if (inrange >= 0) {
		int rangeok = 0;
		if (slist[j].tasks[i] == inrange + 1) {
		    ++inrange;
		    rangeok = 1;
		    needdash = 1;
		    if (i < slist[j].numtasks - 1)
			continue;  /* else fall through and terminate */
		}
		if (needdash) {
		    growstr_printf(g, "-%d", inrange);
		    inrange = -1;
		    needdash = 0;
		}
		if (rangeok == 1)
		    continue;  /* else fall to do the next range */
	    }
	    if (needcomma)
		growstr_append(g, ",");
	    growstr_printf(g, "%d", slist[j].tasks[i]);
	    inrange = slist[j].tasks[i];
	    needcomma = 1;
	}

	/* DONE_NOT cannot happen, hopefully */
	if (slist[j].done == DONE_STARTUP_INCOMPLETE)
	    growstr_append(g, " exited before completing MPI startup");
	else if (slist[j].done == DONE_NOT_STARTED)
	    growstr_printf(g, " %s never spawned due to earlier errors",
		           slist[j].numtasks > 1 ? "were" : "was");
	else if (slist[j].status >= PBS_SIG_OFFSET) {
	    int sig = slist[j].status - PBS_SIG_OFFSET;
	    growstr_printf(g, " died with signal %d (%s)",
	      sig, parse_signal_number(sig));
	} else if (slist[j].status != 0) {
	    growstr_printf(g, " exited with status %d", slist[j].status);
	} else {
	    growstr_printf(g, " exited oddly---report bug: status %d done %d",
	                   slist[j].status, (int) slist[j].done);
	}
	warning(g->s);
    }
    growstr_free(g);
}

