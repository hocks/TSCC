/*
 * redir-helper.c - connect sockets back to mpiexec for PBSPro environments
 * that do not pay attention to MPIEXEC_STD*.
 *
 * $Id$
 *
 * Copyright (C) 2006 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <stdarg.h>

static const char *progname;

/*
 * Strip dir part.
 */
static const char *set_progname(const char *argv0)
{
    const char *cp, *pname;

    for (cp=pname=argv0; *cp; cp++)
	if (*cp == '/')
	    pname = cp+1;
    return pname;
}

/*
 * Error, fatal.
 */
static void error(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ".\n");
    exit(1);
}

/*
 * Error, fatal, with the errno message.
 */
static void error_errno(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s.\n", strerror(errno));
    exit(1);
}

int main(int argc, char *argv[])
{
    const struct {
	int fd;
	const char *env;
    } fds[] = {
	{ 0, "MPIEXEC_STDIN_PORT" },
	{ 1, "MPIEXEC_STDOUT_PORT" },
	{ 2, "MPIEXEC_STDERR_PORT" },
    };
    const char *const host_env = "MPIEXEC_HOST";
    char **nargv;
    int i, j, s, port;
    const char *env, *host_name;
    char *cq;
    struct sockaddr_in sin;
    socklen_t len = sizeof(sin);
    struct hostent *hp = NULL;

    progname = set_progname(argv[0]);

    /*
     * Look up the host first to check for errant usage.
     */
    host_name = getenv(host_env);
    if (!host_name)
	error("no environment variable \"%s\".\n"
	      "This program is not usually run by hand", host_env);
    hp = gethostbyname(host_name);
    if (!hp)
	error("could not resolve \"%s\"", host_name);

    if (argc < 2)
	error("need at least one program argument");

    /*
     * If the env var is found, connect the appropriate fd to it.
     */
    for (i=0; i<3; i++) {
	env = getenv(fds[i].env);
	if (!env)
	    continue;
	port = strtoul(env, &cq, 10);
	if (cq == env || *cq != '\0')
	    error("non-numeric port in environment variable \"%s\"",
	          fds[i].env);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (!s)
	    error_errno("socket");
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = hp->h_addrtype;
	memcpy(&sin.sin_addr, hp->h_addr_list[0], hp->h_length);
	sin.sin_port = htons(port);
	for (j=0; j<3; j++) {
	    if (connect(s, (struct sockaddr *) &sin, len) == 0)
		break;
	    if (errno == EINTR || errno == EADDRINUSE || errno == ETIMEDOUT
	     || errno == ECONNREFUSED) {
		sleep(1);
		if (j < 2)
		    continue;
	    }
	    error("connect %s to %s port %d", fds[i].env, host_name, port);
	}
	(void) close(i);
	if (dup2(s, i) < 0)
	    error_errno("dup2");
    }

    /*
     * Exec the real code.  The first element better be a real path.  We
     * chop off the directory part to get argv[0] for the execed code.
     */
    nargv = malloc(argc * sizeof(*nargv));
    if (!nargv)
    	error("malloc");
    nargv[0] = strdup(set_progname(argv[1]));
    for (i=2; i<argc; i++)
	nargv[i-1] = argv[i];
    nargv[argc-1] = NULL;
    execvp(argv[1], nargv);
    error("execvp");
    return 1;
}

