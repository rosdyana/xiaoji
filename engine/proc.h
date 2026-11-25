#ifndef XIAOJI_PROC_H
#define XIAOJI_PROC_H

#include <stdint.h>

#include "cgroupfs.h"
#include "mem.h"
#include "signals.h"
#include "vfs.h"

/* A guest process: its address space, fd table, signal handlers, ids and family.
 * Its threads are in the scheduler. A process ends when its last thread goes (or
 * on exit_group, or a fatal signal). It is then a zombie until its parent reaps it;
 * its address space and fd table are released at once (pipes see end of file). */
#define CRED_MAX_GROUPS 32

/* A process's credentials (cred.c): the real, effective, saved and filesystem ids, the supplementary
 * groups, and the capability sets (bit n is capability n; the bounding set is proc_t.capbset). A
 * fork copies them. A new process (init or the initial one) is root with the full sets. */
typedef struct {
    uint32_t ruid, euid, suid, fsuid, rgid, egid, sgid, fsgid;
    uint32_t groups[CRED_MAX_GROUPS];
    int ngroups;
    uint64_t cap_eff, cap_perm, cap_inh, cap_amb; /* effective, permitted, inheritable, ambient */
} proc_cred_t;

#define PROC_CAP_LAST 40 /* CAP_LAST_CAP of the oracle kernel (CAP_CHECKPOINT_RESTORE) */
#define PROC_CAPS_ALL ((1ull << (PROC_CAP_LAST + 1)) - 1)
#define PROC_RLIM_N 16   /* RLIM_NLIMITS on Linux arm64 */
#define PROC_ATTR_MAX 128 /* the SELinux context of a process (attr/current) */

typedef struct proc {
    int pid, ppid, pgid, sid;
    mm_t *mm;
    fdtab_t *fds;
    sigtab_t *sig;
    uint64_t brk_base, brk_cur;
    uint32_t umask;
    proc_cred_t cred;
    int dumpable;
    /* prctl state (syscall.c): inherited by a fork, as Linux does. pdeathsig is recorded only
     * (no signal is sent when the parent exits) and is not inherited. */
    int keepcaps, no_new_privs, securebits, subreaper, timerslack, pdeathsig;
    uint64_t capbset; /* the bounding set: bit n is capability n, for n up to PROC_CAP_LAST */
    int exited;      /* ended: its threads are going away */
    int exit_code;   /* the exit status (exit_group's code, or 128+sig for a fatal signal) */
    int term_sig;    /* the signal that ended it, or 0 */
    int zombie;      /* ended and not reaped: wait4 and waitid report it */
    int vfork;       /* made by vfork: shares its parent's address space */
    int nthreads;
    /* Cgroup membership (cgroupfs.c, B11): the directory in each mount, 0 for the mount root. */
    int cg[CG_MAX_MOUNTS];
    int oom_score_adj; /* /proc/<pid>/oom_score_adj: -1000 to 1000 (procfs.c); a fork copies it */
    char dbg_comm[16]; /* the name of the last thread that ended (XIAOJI_DEBUG_FAULTS exit lines) */
    uint64_t rlim[PROC_RLIM_N][2]; /* soft and hard limit per resource (prlimit64, setrlimit) */
    char attr_current[PROC_ATTR_MAX]; /* attr/current: the context, which setcon writes */
    char attr_create[4][PROC_ATTR_MAX]; /* attr/fscreate, sockcreate, keycreate, exec (procfs.c, B16); empty: none */
    /* Accounting for getrusage. Instructions and syscalls are this process's own;
     * the cu_ fields are the totals of its reaped children (and their children). */
    uint64_t dead_icount; /* instructions of its threads already reaped */
    uint64_t load_icount; /* its instructions at the last --load-every report (sched.c) */
    uint64_t nsys;        /* syscalls made */
    uint64_t maxrss_kib;  /* peak resident set, as recorded at exit or getrusage */
    uint64_t cu_icount, cu_nsys, cu_maxrss_kib;
    struct proc *parent, *first_child, *next_sibling;
    struct proc *all_next; /* every process made since start, for lookup by pid */
} proc_t;

proc_t *proc_init(void);

proc_t *proc_cur(void);

void proc_switch(proc_t *p);
/* As proc_switch, even when p is current: installs its tables again and flushes the TLBs (after a restore). */
void proc_switch_force(proc_t *p);

/* A child of cur (the running process) with a new pid. vfork shares cur's address
 * space; otherwise the address space is copied copy-on-write. Its fd table shares the
 * parent's descriptions, and its signal handlers are copied. NULL if out of memory. */
proc_t *proc_fork(proc_t *parent, int pid, int vfork);

/* The process with pid, zombies included; NULL if there is none. */
proc_t *proc_find(int pid);

/* Releases what an ended process holds: its fd table (pipes see end of file), its
 * address space (shared by a vfork parent until that ends), and its signal table. */
void proc_release(proc_t *p);

/* The processes made so far, in no particular order (zombies included). */
proc_t *proc_first(void);
proc_t *proc_next(proc_t *p);
/* 1 for the initial process (its exit ends the engine). */
int proc_is_initial(const proc_t *p);

/* Moves the children of p to init (orphans). */
void proc_reparent_children(proc_t *p, proc_t *init);

/* Frees a reaped zombie. Its usage (and its children's) is added to its parent's
 * cu_ totals first. */
void proc_free(proc_t *p);

#endif
