/*
 * util.h - declarations of general utilities
 *
 * $Id: util.h 395 2006-11-29 15:22:26Z pw $
 *
 * Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#include <stdlib.h>
#ifndef __util_h
#define __util_h

#ifdef __GNUC__
#  define ATTR_PRINTF   __attribute__ ((format(printf, 1, 2)))
#  define ATTR_PRINTF2  __attribute__ ((format(printf, 2, 3)))
#  define ATTR_NORETURN __attribute__ ((noreturn))
#  define ATTR_UNUSED   __attribute__ ((unused))
#  if __GNUC__ > 2
#    define ATTR_MALLOC   __attribute__ ((malloc))
#  else
#    define ATTR_MALLOC
#  endif
#else
#  define ATTR_PRINTF
#  define ATTR_PRINTF2
#  define ATTR_NORETURN
#  define ATTR_UNUSED
#  define ATTR_MALLOC
#endif

/*
 * PGI C, and Intel ia64 ecc are not ISO C9X compliant compilers.
 */
#if defined(__PGI) || defined(__ECC)
#  define __func__ __FILE__
#endif

/* Nor is the native Irix MIPS C compiler.  It also does not
 * understand "inline".  */
#if (defined(__sgi) && defined(__host_mips) && !defined(__GNUC__)) || defined(_CRAYC)
#  define __func__ __FILE__
#  define inline __inline
#endif

#ifndef HAVE_SOCKLEN_T
typedef unsigned int socklen_t;
#endif

extern void set_progname(int argc, const char *const argv[]);
extern void debug(int level, const char *fmt, ...) ATTR_PRINTF2;
extern void warning(const char *fmt, ...) ATTR_PRINTF;
extern void warning_tm(int err, const char *fmt, ...) ATTR_PRINTF2;
extern void error(const char *fmt, ...) ATTR_PRINTF ATTR_NORETURN;
extern void error_errno(const char *fmt, ...) ATTR_PRINTF ATTR_NORETURN;
extern void error_pbs(const char *fmt, ...) ATTR_NORETURN;
extern void error_tm(int err, const char *fmt, ...) ATTR_NORETURN;
extern void error_tm_or_pbs(int err, const char *fmt, ...) ATTR_NORETURN;
extern char *strsave(const char *s);
extern void *Malloc(unsigned int n) ATTR_MALLOC;
extern int read_until(int fd, char *buf, size_t count, const char *until,
  int inptr);
extern void read_full(int fd, void *buf, size_t count);
extern int read_full_ret(int fd, void *buf, size_t count);
extern int write_full(int fd, const void *buf, size_t count);
extern int write_full_string(int fd, const char *s);
extern int read_full_string(int fd, char **s);

#endif  /* __util_h */
