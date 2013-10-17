/*
 * mpiexec.h - common variables and routines
 *
 * $Id: mpiexec.h 420 2008-04-10 21:40:21Z pw $
 *
 * Copyright (C) 2000-3 Ohio Supercomputer Center.
 * Copyright (C) 2000-7 Pete Wyckoff <pw@osc.edu>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#ifndef __mpiexec_h
#define __mpiexec_h

#include <pbs_ifl.h>
#include <tm.h>
#include <netinet/in.h>  /* sockaddr_in */
#include "config.h"
#include "util.h"
#include "growstr.h"
#include "list.h"

/*
 * Different ways that a task can have finished; or not even been started
 * yet.  Used for nice exit status reporting.
 */
typedef enum {
    DONE_NOT_STARTED = 0,  /* initial state, before spawned */
    DONE_NOT,
    DONE_OK,
    DONE_STARTUP_INCOMPLETE,
    DONE_NO_EXIT_STATUS,
} done_how_t;

/*
 * The config file (or command-line arguments) is parsed into this
 * structure to save node and executable specification.
 */
typedef struct {
    struct list_head list;
    const char *exe;
    const char *args;
    int line;
    int allocated;
} config_spec_t;

/*
 * List of available nodes and number of processes supported on each.  Also
 * remembers what has been allocated on each in the case of concurrent mpiexec
 * runs.
 */
typedef struct {
    char *name;      /* hostname */
    char *mpname;    /* hostname for message-passing communication */
    tm_node_id *ids; /* node ids collected from PBS, one for each virtual CPU */
    int *cpu_ids;    /* CPU ids collected from PBS, perhaps not useful */
    int *cpu_free;   /* 1 if the CPU is free; sum(cpu_free) = availcpu */
    int *cm_cpu_free; /* ditto, just for CM */
    int numcpu;      /* length of ids and cpu_ids arrays */
    int availcpu;    /* free CPUs for a particular client  */
    int cm_availcpu; /* free CPUs overall, at the concurrent master only */
} nodes_t;
extern nodes_t *nodes;

/*
 * Tasks are processes that are spawned onto nodes using the TM API.
 */
typedef struct {
    int node;             /* up pointer for name and mpname */
    int *cpu_index;       /* index into ids-like arrays on node (num_copies) */
    int cpu_index_one;    /* for usual case when num_copies == 1 */
    int num_copies;       /* mpich/p4 or shmem number of processes in task */
    tm_task_id tid;       /* TM task id (after spawned) */
    done_how_t done;      /* true when task has exited or been killed */
    int *status;          /* task exit status location */
    config_spec_t *conf;  /* what to run */
} tasks_t;
extern tasks_t *tasks;

/*
 * A spawn is a set of tasks, contiguous in the tasks[] array, that
 * were all started at the same time with MPI_Spawn or similar.  The initial
 * set of tasks (that were not spawned) are always spawn #0.
 */
typedef struct {
    int task_start;  /* id of the first entry in tasks[] */
    int task_end;    /* one beyond the last task in this spawn */
    int *obits;      /* arrays of ints for TM to put its exit status */
    growstr_t *ranks2hosts_response;  /* for Intel 3.0 PMI */
} spawns_t;
extern spawns_t *spawns;

/*
 * Command-line arguments, conveniently lumped together.  Some of these
 * are only for a certain version, but I don't bother to ifdef here since
 * it would be much less readable.  See the parsing code.
 */
typedef enum {
    STDIN_UNSET = -1,
    STDIN_NONE,
    STDIN_ONE,
    STDIN_ALL,
} which_stdin_t;

typedef enum {
    COMM_UNSET = -1,
    COMM_MPICH_GM,
    COMM_MPICH_P4,
    COMM_MPICH_IB,
    COMM_MPICH_PSM,
    COMM_MPICH_RAI,
    COMM_MPICH2_PMI,
    COMM_LAM,
    COMM_SHMEM,
    COMM_EMP,
    COMM_PORTALS,
    COMM_NONE,
} comm_t;

typedef struct {
    int numproc;                /* -n <numproc> */
    int tview;                  /* -tv */
    int pernode;                /* -pernode and -npernode */
    int nolocal;                /* -nolocal */
    const char *config_file;    /* -config <config_file> */
    which_stdin_t which_stdin;  /* -(no|all)stdin */
    int nostdout;               /* -nostdout */
    int kill_others;            /* -kill-others */
    int verbose;                /* -verbose, increments once per cl arg */
    const char *transform_hostname;  /* -transform-hostname */
    const char *transform_hostname_program;  /* -transform-hostname-program */
    comm_t comm;                /* -comm=(gm|p4|ib|...|none) */
    const char *process_output; /* -output <prefix> */
    int mpich_p4_shmem;         /* -mpich-p4-[no-]shmem */
    int server_only;            /* -server */
} cl_args_t;
extern cl_args_t *cl_args;

/*
 * Types of TM events that can happen.
 */
typedef enum {
    EVT_WILDCARD = -1,
    EVT_START,
    EVT_OBIT,
    EVT_KILL,
} evt_type_t;

/*
 * To describe tasks that are alive in the system, only used on the
 * concurrent master.  In task.c.
 */
typedef struct {
    struct list_head list;
    int tid;
    int client;  /* entry into concurrent clients[] array */
    int task;    /* entry into tasks[] array of client */
    int status;  /* output pointer for tm_obit, written in tm_poll */
} tids_t;
extern struct list_head *tids;

/*
 * To describe events that are outstanding, used on both clients and master.
 * Only for TM-related events, not communication between stdio and parent
 * or from concurrent mpiexec to master.
 */
typedef struct {
    struct list_head list;
    int evt;
    int client;
    int task;
    evt_type_t type;
    int dead;      /* master: if tid died, its events are marked ignorable */
    int obit_evt;  /* client: if START, this is the new evt for the OBIT */
} evts_t;
extern struct list_head *evts;

/*
 * Other global variables
 */
/* mpiexec.c */
extern const char *progname;
extern char *progname_dir;  /* for use in looking up redir-helper */
extern char *jobid;  /* PBS_JOBID from environment */
extern int numnodes;  /* number of distinct nodes given to us by PBS */
extern int numtasks;  /* actual number of processes to spawn, <= nnodes */
extern int numspawns;  /* 1 + number of times MPI_Spawn called */
extern struct passwd *pswd;  /* used for home dir, shell, user name */
extern struct sockaddr_in myaddr;  /* for out-of-band MPI lib startup */
extern char *tvname;  /* name of totalview executable, possibly from env var */

/* concurrent.c */
extern int concurrent_master;  /* if first mpiexec to run in job */

/* start_tasks.c */
extern int numspawned;  /* number of jobs successfully spawned */
extern int numtasks_waiting_start;  /* number spawned but no EVT_START yet */
extern int startup_complete;  /* false until mpi startup completed */

/* task.c */
extern int have_killed;  /* if sent kills to tasks */

/* stdio.c */
extern int pipe_with_stdio;  /* communication channel with stdio listener */

/* gm.c: exported to start_tasks.c to pass to stdio listener */
int gmpi_fd[2];

/* pmi.c: exported to stdio.c */
int pmi_listen_fd, pmi_listen_port, *pmi_fds, *pmi_barrier;

/*
 * Prototypes
 */
/* mpiexec.c */
char *resolve_exe(const char *exe, int argv0_dir);
const char *parse_signal_number(int sig);
void killall(int sig);
void handle_signals(const int *list, int num, void (*handler)(int sig));
int stat_exe(const char *exe, int complain);

/* get_hosts.c */
void get_hosts(void);
void constrain_nodes(void);
void reconnect_to_mom(void);

/* start_tasks.c */
int start_tasks(int spawn);
int do_tm_poll(tm_event_t *evt, const char *caller, int block);

/* task.c */
tids_t *tid_add(int tid, int client, int task);
tids_t *tid_find(int client, int task);
void tid_del(tids_t *tp);
void tid_dump(void);
const char *node_name_from_nid(tm_node_id nid);
tm_event_t kill_tid(tids_t *tp);
void kill_tasks(int signum);
void wait_tasks(void);

/* event.c */
void evt_add(int tid, int client, int task, evt_type_t type);
evts_t *evt_lookup(int evt);
void evt_del(evts_t *ep);
const char *evt_type_string(evt_type_t type);
void evt_dump(void);
void dispatch_event(evts_t *ep);
evts_t *poll_event(void);
evts_t *block_event(void);
int poll_events_until_obit(void);

/* config.c */
config_spec_t *new_config_spec(void);
void allocate_cpu_to_task(int node, tasks_t *task);
void parse_config(void);
void argcv_config(int argc, const char *const argv[]);
void tasks_shmem_reduce(void);
const char *config_get_unique_executable(void);
void config_set_unique_executable(const char *s);

/* stdio.c */
void stdio_fork(int expected_in[3], int abort_fd_in[2], int pmi_fd);
int stdio_port(int n);
void stdio_notice_streams(void);
void stdio_msg_parent_read(void);
void stdio_msg_parent_say_abort_fd(int abort_fd_index);
void stdio_msg_listener_spawn(int rank, int nprocs,
  const char *execname, int numarg, const char *const *args, int numinfo,
  const char *const *infokeys, const char *const *infovals);
int stdio_msg_parent_say_more_tasks(int num, int expected_in[3]);
void kill_stdio(void);
void try_kill_stdio(void);
void kill_stdio_abort_fd(int abort_fd_index);
void poll_set(int fd, fd_set *fds);
void poll_del(int fd, fd_set *fds);
void maybe_exit_stdio(void);  /* for use by device-specific code */

/* concurrent.c */
void concurrent_init(void);
void concurrent_exit(void);
void concurrent_get_nodes(void);
void concurrent_node_alloc(void);
void concurrent_request_spawn(int tasknum, int argc, char **argv,
  char **envp, tm_node_id nid);
void concurrent_request_kill(int tasknum, int signum);
evts_t *concurrent_poll(int block);
void cm_serve_clients(void);
void cm_forward_event(evts_t *ep);
void cm_check_clients(void);
void cm_permit_new_clients(int listen);
int cm_kill_clients(void);

/* p4.c */
int prepare_p4_master_port(void);
int read_p4_master_port(int *port);

/* gm.c */
void prepare_gm_startup_ports(int gmpi_port[2]);
int read_gm_startup_ports(void);
int service_gm_startup(int created_new_task);

/* ib.c */
int prepare_ib_startup_port(int *fd);
int read_ib_startup_ports(void);
int service_ib_startup(int created_new_task);

/* psm.c */
int prepare_psm_startup_port(int *fd);
int read_psm_startup_ports(void);
int service_psm_startup(int created_new_task);

/* rai.c */
int prepare_rai_startup_port(void);
tm_event_t read_rai_startup_ports(void);

/* pmi.c */
void accept_pmi_conn(fd_set *rfs);
void handle_pmi(int rank, fd_set *rfs);
void pmi_send_spawn_result(int rank, int ok);
int prepare_pmi_startup_port(int *pmi_fd);

/* exedist.c */
int distribute_executable(void);

/* spawn.c */
int spawn(int nprocs, char *execname, int numarg, char **args, int numinfo,
          char **infokeys, char **infovals);

/* handy define */
#define list_count(list) ((int)(sizeof(list) / sizeof(list[0])))

/*
 * In PBS mom mach code, scan_for_terminated() interprets
 * the return value from wait for us, whether we like it or not:
 *
 * if (WIFEXITED(statloc))
 *     exiteval = WEXITSTATUS(statloc);
 * else if (WIFSIGNALED(statloc))
 *     exiteval = WTERMSIG(statloc) + 0x100;
 * else
 *     exiteval = 1;
 * 
 * The magic constant below fixes that.  On freebsd including darwin
 * aka mac osx, this number is different.  No good compiler symbol
 * sticks out, and there is no pbs/tm call to find out, so guess
 * using this one.
 *
 * It is used both in wait_tasks() to determine when to kill the rest
 * of the tasks for abnormal exit, and in show_exit_statuses() for nice
 * printing.
 */
#ifdef __MACH__
#define PBS_SIG_OFFSET 10000
#else
#define PBS_SIG_OFFSET 0x100
#endif

#endif  /* __mpiexec_h */
