/*
 * util.c - general utilities
 *
 * $Id: util.c 419 2008-03-25 18:30:18Z pw $
 *
 * Copyright (C) 2000-8 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <pbs_error.h>  /* pbse_to_txt found in pbs liblog.a */
#include "mpiexec.h"

/* set to 1 to add a timestamp to each debug message */
#define DEBUG_TIMESTAMP 0

/*
 * Set the program name, first statement of code usually.
 */
void
set_progname(int argc ATTR_UNUSED, const char *const argv[])
{
    const char *cp, *eptr;

    for (cp=progname=argv[0]; *cp; cp++)
	if (*cp == '/')
	    progname = cp+1;
    /* figure out directory from whence mpiexec came, if invoked as such */
    progname_dir = NULL;
    eptr = progname;
    if (eptr != argv[0]) {
	--eptr;
	if (*eptr == '/') {  /* require and keep the directory slash */
	    int len = eptr - argv[0] + 1;
	    progname_dir = Malloc(len + 1);
	    strncpy(progname_dir, argv[0], len);
	    progname_dir[len] = '\0';
	}
    }
}

/*
 * Convert tm error numbers to strings.
 */
static const char *
tm_errno_to_str(int err)
{
    const struct {
	int err;
	const char *str;
    } tm_errs[] = {
	{ TM_ESYSTEM, "system error" },
	{ TM_ENOEVENT, "no event" },
	{ TM_ENOTCONNECTED, "not connected" },
	{ TM_EUNKNOWNCMD, "unknown command" },
	{ TM_ENOTIMPLEMENTED, "not implemented" },
	{ TM_EBADENVIRONMENT, "bad environment" },
	{ TM_ENOTFOUND, "not found" },
	{ TM_BADINIT, "bad init" },  /* not my typo */
    };
    const int num_tm_errs = sizeof(tm_errs) / sizeof(tm_errs[0]);
    int i;

    for (i=0; i<num_tm_errs; i++)
	if (err == tm_errs[i].err)
	    return tm_errs[i].str;
    return NULL;
}

static void
error_tm_print(int err)
{
    const char *s;

    fprintf(stderr, ": tm: ");
    s = tm_errno_to_str(err);
    if (s)
	fprintf(stderr, "%s.\n", s);
    else
	fprintf(stderr, "unknown error %d.\n", err);
}

/*
 * Debug message.
 */
void
debug(int level, const char *fmt, ...)
{
    va_list ap;

    if (cl_args->verbose >= level) {
	fprintf(stderr, "%s: ", progname);
	if (DEBUG_TIMESTAMP) {
	    /* add a timestamp to debug messages */
	    struct timeval tv;
	    char buf[20];
	    time_t timet;

	    gettimeofday(&tv, NULL);
	    timet = tv.tv_sec;  /* mac time_t different from __time_t */
	    strftime(buf, 10, "[%H:%M:%S", localtime(&timet));
	    sprintf(buf+9, ".%06ld] ", (long) tv.tv_usec);  /* cast for mac */
	    fputs(buf, stderr);
	}
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, ".\n");
    }
}

/*
 * Warning, non-fatal.
 */
void
warning(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Warning: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ".\n");
}

/*
 * Warning, non-fatal, with the PBS tm errno message.
 */
void
warning_tm(int err, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Warning: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    error_tm_print(err);
}

/*
 * Error, fatal.
 */
void
error(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ".\n");
    try_kill_stdio();  /* never hurts */
    exit(1);
}

/*
 * Error, fatal, with the errno message.
 */
void
error_errno(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, ": %s.\n", strerror(errno));
    try_kill_stdio();
    exit(1);
}

static void
error_pbs_print(int err)
{
    const char *s;

    /*
     * Error message for PBSE_SYSTEM is weird:  " Sytem error: ", so
     * replace it.
     */
    if (err == PBSE_SYSTEM)
	fprintf(stderr, ": System error.\n");
    else if ((s = pbse_to_txt(err)))
	fprintf(stderr, ": %s.\n", s);
    else if (pbs_errno < PBSE_)
	/* try standard error numbers, which it sometimes will return */
	fprintf(stderr, ": %s.\n", strerror(pbs_errno));
    else
	fprintf(stderr, ": Unknown PBS error %d.\n", err);
}

/*
 * Error, fatal, with the pbs_errno message.
 */
void
error_pbs(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    error_pbs_print(pbs_errno);
    try_kill_stdio();
    exit(1);
}

/*
 * Error, fatal, with the PBS tm errno message.
 */
void
error_tm(int err, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    error_tm_print(err);
    try_kill_stdio();
    exit(1);
}

/*
 * Check through both tm and pbs error numbers looking for a match.
 */
void
error_tm_or_pbs(int err, const char *fmt, ...)
{
    va_list ap;
    const char *s;

    fprintf(stderr, "%s: Error: ", progname);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    s = tm_errno_to_str(err);
    if (s)
	error_tm_print(err);
    else
	error_pbs_print(err);
    try_kill_stdio();
    exit(1);
}

/*
 * Put a string in permanent memory.  strdup with error-checking
 * malloc.
 */
char *
strsave(const char *s)
{
    char *t;

    t = Malloc((strlen(s)+1)*sizeof(char));
    strcpy(t, s);
    return t;
}

/*
 * Error-checking malloc.
 */
void *
Malloc(unsigned int n)
{
    void *x;

    if (n == 0)
	error("%s: asked to allocate zero bytes", __func__);
    x = malloc(n);
    if (!x)
	error("%s: couldn't get %u bytes", __func__, n);
    return x;
}

/*
 * Read until a certain string is found.  Only look for matches appearing
 * somewhere after inptr.  Start reading to the end of the string if inptr
 * != 0.  Always terminates strings with null.
 */
int
read_until(int fd, char *buf, size_t count, const char *until, int inptr)
{
    int cc, ptr;

    if (count == 0) {
	if (*until == 0)
	    return 0;
	else
	    error("%s: size 0 buf no until string found", __func__);
    }
    if (inptr)
	ptr = strlen(buf);
    else {
	ptr = 0;
	buf[ptr] = '\0';
    }
    for (;;) {
	if (strstr(buf+inptr, until))
	    return ptr;
	cc = read(fd, buf + ptr, 1);  /* painfully slow, but easy to code */
	if (cc <= 0)
	    return cc;
	ptr += cc;
	buf[ptr] = '\0';
    }
}

/*
 * Gcc >= "2.96" seem to have this nice addition.  2.95.2 does not.
 */
#ifdef __GNUC__
#  if __GNUC__ >= 3
#    define USE_FORMAT_SIZE_T 1
#  endif
#  if __GNUC__ == 2 && __GNUC_MINOR__ >= 96
#    define USE_FORMAT_SIZE_T 1
#  endif
#endif

#ifndef USE_FORMAT_SIZE_T
#  define USE_FORMAT_SIZE_T 0
#endif

/*
 * Loop over reading until everything arrives.  Error if not.
 */
void
read_full(int fd, void *buf, size_t num)
{
    int cc, offset = 0;
    int total = num;

    while (num > 0) {
	cc = read(fd, (char *)buf + offset, num);
	if (cc < 0) {
	    if (USE_FORMAT_SIZE_T)
		error_errno("%s: read %zu bytes", __func__, num);
	    else
		error_errno("%s: read %lu bytes", __func__,
		  (long unsigned int) num);
	}
	if (cc == 0)
	    error("%s: EOF, only %d of %d bytes", __func__, offset, total);
	num -= cc;
	offset += cc;
    }
}

/*
 * Loop over reading until everything arrives.  Return numbytes read,
 * just like read, or 0 for eof, or negative for some error.  Eventually
 * get rid of the non_ret version and check errors everywhere.
 */
int
read_full_ret(int fd, void *buf, size_t num)
{
    int cc, offset = 0;
    int total = num;

    while (num > 0) {
	cc = read(fd, (char *)buf + offset, num);
	if (cc <= 0)
	    return cc;
	num -= cc;
	offset += cc;
    }
    return total;
}

/*
 * Keep looping until all bytes have been accepted by the kernel.  Return
 * count if all okay, else -1 with errno set.
 */
int
write_full(int fd, const void *buf, size_t count)
{
    int cc, ptr = 0;
    int total = count;

    while (count > 0) {
	cc = write(fd, (const char *)buf + ptr, count);
	if (cc < 0)
	    return cc;
	count -= cc;
	ptr += cc;
    }
    return total;
}

/*
 * Write the length, then the bytes, of a null-terminated string.
 */
int
write_full_string(int fd, const char *s)
{
    int len;
    int ret;

    len = strlen(s);
    ret = write_full(fd, &len, sizeof(len));
    if (ret < 0)
	return ret;
    ret = write_full(fd, s, len+1);
    return ret;
}

/*
 * Read and allocate a string into a given pointer.
 */
int
read_full_string(int fd, char **s)
{
    int len;
    int ret;

    ret = read_full_ret(fd, &len, sizeof(len));
    if (ret < 0)
	return ret;
    *s = Malloc((len+1));
    ret = read_full_ret(fd, *s, len+1);
    return ret;
}

