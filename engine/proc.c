/* Guest processes: creation of the initial process and switching between processes.
 * The subsystems (mem.c, vfs.c, signals.c) keep one current record each; this file
 * installs a process's records into them. */
#include "proc.h"

#include <stdio.h>
#include <stdlib.h>

#include "mem.h"
#include "signals.h"
#include "vfs.h"

#define INIT_PID 1000 /* the main thread's tid is the process id, as before */
#define INIT_PPID 1
#define INIT_PGID 1000
#define INIT_UMASK 022

static proc_t *cur;

static void die_oom(void)
{
    fputs("xiaoji: out of host memory\n", stderr);
    exit(1);
}

proc_t *proc_init(void)
{
    proc_t *p = calloc(1, sizeof *p);
    if (!p)
        die_oom();
    p->pid = INIT_PID;
    p->ppid = INIT_PPID;
    p->pgid = INIT_PGID;
    p->sid = INIT_PID;
    p->umask = INIT_UMASK;
    p->dumpable = 1; /* SUID_DUMP_USER */
    p->mm = mem_mm_new();
    p->fds = vfs_fdtab_new();
    p->sig = calloc(1, sig_tab_size());
    if (!p->mm || !p->fds || !p->sig)
        die_oom();
    mem_mm_switch(p->mm);
    vfs_fdtab_switch(p->fds);
    sig_tab_switch(p->sig);
    cur = p;
    return p;
}

proc_t *proc_cur(void) { return cur; }

void proc_switch(proc_t *p)
{
    if (p == cur)
        return;
    mem_mm_switch(p->mm);
    vfs_fdtab_switch(p->fds);
    sig_tab_switch(p->sig);
    cur = p;
}
