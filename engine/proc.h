/* Guest processes. A process owns its address space, fd table and cwd, signal
 * handlers, ids, brk, umask, exit status, and its place in the parent and child
 * lists. One current process is kept; the scheduler switches it with proc_switch
 * when it runs a thread of another process. Only .c files include this header. */
#ifndef XIAOJI_PROC_H
#define XIAOJI_PROC_H

#include <stdint.h>

typedef struct mm mm_t;         /* mem.c: page table and region list */
typedef struct fdtab fdtab_t;   /* vfs.c: fd table, cwd, program name and argv */
typedef struct sigtab sigtab_t; /* signals.c: handler table and the return trampoline */

typedef struct proc {
    int pid, ppid, pgid, sid;
    mm_t *mm;
    fdtab_t *fds;
    sigtab_t *sig;
    uint64_t brk_base, brk_cur;
    uint32_t umask;
    int dumpable;
    int exited;
    int exit_code;
    int nthreads;
    struct proc *parent, *first_child, *next_sibling;
} proc_t;

/* Creates the initial process (pid 1000), installs its state and makes it current.
 * Call once, before anything else in the engine uses memory, files or signals. */
proc_t *proc_init(void);

proc_t *proc_cur(void);

/* Installs p's address space, fd table and signal table, and makes p current.
 * The TLBs and translation caches are flushed by the address-space switch. */
void proc_switch(proc_t *p);

#endif
