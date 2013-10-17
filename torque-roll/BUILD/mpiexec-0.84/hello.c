/*
 * hello.c - test mpi program
 *
 * $Id: hello.c 326 2006-01-24 21:35:26Z pw $
 *
 * Copyright (C) 2000-6 Pete Wyckoff <pw@osc.edu>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#ifdef __GNUC__
#  define ATTR_UNUSED   __attribute__ ((unused))
#else
#  define ATTR_UNUSED
#endif

static char hostname[1024];

/*
 * "Fake" the MPI calls to test comm=none.
 */
#if 0
const int MPI_COMM_WORLD = 0;
static void MPI_Init(int *x, char ***y) { }
static void MPI_Comm_size(int n, int *np) { *np = 1; }
static void MPI_Comm_rank(int n, int *np) { *np = 0; }
static void MPI_Finalize(void) { }
#else
#include "mpi.h"
#endif

static void
handle(int sig ATTR_UNUSED)
{
    const char *opts = getenv("GMPI_OPTS");
    unsigned long *ul = 0;

    printf("hello: %s %s MPI_Init did not finish\n", hostname,
      opts ? opts : "");
    sleep(2);
    *ul = 0;
    exit(1);
}

int
main(int argc, char *argv[])
{
    int myid, numproc;
    int readlines = 0, doze = 0, segv = -1, segvearly = 0;
    int abort = -1, mixup = 0;
    int *exitval = 0, numexitval = 0;
    struct sigaction act;
    char buf[1024];
    FILE *fp = stdout;
    int i;

    for (i=1; i<argc; i++) {
	if (!strcmp(argv[i], "-l"))
	    readlines = 1;
	else if (!strcmp(argv[i], "-sleep"))
	    doze = 1;
	else if (!strcmp(argv[i], "-stderr")) {
	    fp = stderr;
	    setlinebuf(fp);  /* else output will mix up and confuse tester */
	} else if (!strcmp(argv[i], "-segv")) {
	    segv = 0;
	    if (i+1 < argc) {
		char *cq;
		segv = strtol(argv[i+1], &cq, 10);
		if (cq != argv[i+1])  /* got digits, consume argument */
		    ++i;
	    }
	} else if (!strcmp(argv[i], "-segvearly"))
	    segvearly = 1;
	else if (!strcmp(argv[i], "-abort")) {
	    abort = 0;
	    if (i+1 < argc) {
		char *cq;
		abort = strtol(argv[i+1], &cq, 10);
		if (cq != argv[i+1])  /* got digits, consume argument */
		    ++i;
	    }
	} else if (!strcmp(argv[i], "-mixup"))
	    mixup = 1;
	else if (!strcmp(argv[i], "-exitval")) {
	    numexitval = argc - (i+1);
	    exitval = malloc(numexitval * sizeof(*exitval));
	    memset(exitval, 0, numexitval * sizeof(*exitval));
	    numexitval = 0;
	    while (i+1 < argc) {
		char *cq;
		exitval[numexitval] = strtol(argv[i+1], &cq, 10);
		if (cq == argv[i+1])  /* did not parse, give up on these */
		    break;
		++numexitval;
		++i;
	    }
	}
    }

    if (segvearly) {
	/* segv "early" before MPI initialization */
	unsigned long *ul = 0;
	*ul = 0;
    }

    /* MPI initialization */
    gethostname(hostname, sizeof(hostname));
    sigemptyset(&act.sa_mask);
    sigaddset(&act.sa_mask, SIGALRM);
    act.sa_flags = 0;
    act.sa_handler = handle;
    sigaction(SIGALRM, &act, 0);
    /* printf("to mpi init\n"); fflush(stdout); */
    /* sleep(30); */
    alarm(4 * 60 * 60);  /* 4 hours */
    MPI_Init(&argc, &argv);
    alarm(0);
    MPI_Comm_size(MPI_COMM_WORLD, &numproc);
    MPI_Comm_rank(MPI_COMM_WORLD, &myid);

#if 1
    /* Ensure fully connected.  This is required for mpich1/p4 otherwise
     * the MPI_Finalize will hang on some nodes oddly. */
    {
	int i, j;
	char buf[4];
	MPI_Status st;

	for (i=0; i<numproc; i++) {
	    for (j=0; j<numproc; j++) {
		if (i == j) continue;
		if (myid == i)
		    MPI_Send(buf, 4, MPI_BYTE, j, i * numproc + j,
		      MPI_COMM_WORLD);
		if (myid == j)
		    MPI_Recv(buf, 4, MPI_BYTE, i, i * numproc + j,
		      MPI_COMM_WORLD, &st);
	    }
	}
    }
#endif

    fprintf(fp, "hello from %d/%d hostname %s pid %d with %d args:",
      myid, numproc, hostname, getpid(), argc-1);
    while (++argv, --argc)
	fprintf(fp, " %s", *argv);
    fprintf(fp, "\n");
    fflush(fp);

#if 0
    /* keep things synchronized */
    { 
    int sleep_time = 2 + myid * 2;
    sleep(sleep_time);
    fprintf(fp, "hello from %d/%d name %s sleep %2d done.\n", myid, numproc,
      hostname, sleep_time);
    fflush(fp);
    }
#endif

#if 0
    /* display file descriptors */
    {
	int i;
	char s[1024];
	for (i=0; i<9; i++) {
	    sprintf(s, "/proc/%d/fd/%d", getpid(), i);
	    if (readlink(s, buf, sizeof(buf)) >= 0)
		printf("hello %d %s -> %s\n", myid, s, buf);
	}
    }
#endif

    if (mixup) {
	const char *mixstr = "The rain in spain falls mainly in the plain.\n";
	int len = strlen(mixstr);
	int pos = 0, top;
	srand48(time(0) + myid);
	for (i=0; i<40; i++) {
	    do {
		top = (int)(drand48() * len / 3);
		top += pos;
		top %= len;
	    } while (pos == top);
#if 0
	    /* add text of who says what */
	    putchar('<');
	    if (myid < 10)
		putchar(myid + '0');
	    else if (myid < 10 + 26)
		putchar(myid - 10 + 'A');
	    else
		putchar('|');
#endif
	    while (pos != top) {
		putchar(mixstr[pos]);
		if (++pos == len)
		    pos = 0;
	    }
	    usleep((int)(1000000. * drand48()));
	}
	while (pos != 0) {
	    putchar(mixstr[pos]);
	    if (++pos == len)
		pos = 0;
	}
    }

    if (readlines)
	while (fgets(buf, sizeof(buf), stdin))
	    fprintf(fp, "hello %d got line: %s", myid, buf);

    if (abort == myid) {
	sleep(2);
	MPI_Abort(MPI_COMM_WORLD, 4269);
    }

    if (doze) {
	/* close everything and wait awhile */
	(void) close(0);
	(void) close(1);
	(void) close(2);
	sleep(6);
    }

    if (segv == myid) {
	unsigned long *ul = 0;
	sleep(2);
	*ul = 0;
    }

    MPI_Finalize();
    if (myid < numexitval)
	return exitval[myid];
    else
	return 0;
}

