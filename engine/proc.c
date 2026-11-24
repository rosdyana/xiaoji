#include "proc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filecache.h"
#include "mem.h"
#include "selinuxfs.h"
#include "signals.h"
#include "vfs.h"

#define INIT_PID 1000 /* the main thread's tid is the process id, as before */
#define INIT_PPID 1
#define INIT_PGID 1000
#define INIT_UMASK 022

static proc_t *cur;
static proc_t *all_procs; /* every process made, newest first */

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
    p->dumpable = 1;       /* SUID_DUMP_USER, as Linux starts a process */
    p->timerslack = 50000; /* the default timer slack, in ns */
    for (int r = 0; r < PROC_RLIM_N; r++)
        p->rlim[r][0] = p->rlim[r][1] = ~0ull; /* RLIM_INFINITY */
    p->rlim[3][0] = 8ull << 20; /* RLIMIT_STACK: the soft limit is 8 MiB, as before */
    strcpy(p->attr_current, SELINUX_CTX);
    p->capbset = PROC_CAPS_ALL;
    p->cred.cap_eff = p->cred.cap_perm = PROC_CAPS_ALL; /* root: full effective and permitted sets */
    p->mm = mem_mm_new();
    p->fds = vfs_fdtab_new();
    p->sig = calloc(1, sig_tab_size());
    if (!p->mm || !p->fds || !p->sig)
        die_oom();
    mem_mm_switch(p->mm);
    vfs_fdtab_switch(p->fds);
    sig_tab_switch(p->sig);
    cur = p;
    p->all_next = all_procs;
    all_procs = p;
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

static void link_child(proc_t *parent, proc_t *c)
{
    c->parent = parent;
    c->next_sibling = parent->first_child;
    parent->first_child = c;
}

static void unlink_child(proc_t *c)
{
    proc_t *par = c->parent;
    if (!par)
        return;
    proc_t **pp = &par->first_child;
    while (*pp && *pp != c)
        pp = &(*pp)->next_sibling;
    if (*pp)
        *pp = c->next_sibling;
    c->parent = NULL;
    c->next_sibling = NULL;
}

proc_t *proc_fork(proc_t *parent, int pid, int vfork)
{
    proc_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->pid = pid;
    c->ppid = parent->pid;
    c->pgid = parent->pgid;
    c->sid = parent->sid;
    c->umask = parent->umask;
    c->cred = parent->cred;
    memcpy(c->cg, parent->cg, sizeof c->cg); /* a child starts in its parent's cgroups */
    memcpy(c->rlim, parent->rlim, sizeof c->rlim);
    memcpy(c->attr_current, parent->attr_current, sizeof c->attr_current);
    memcpy(c->attr_create, parent->attr_create, sizeof c->attr_create);
    c->dumpable = parent->dumpable;
    c->keepcaps = parent->keepcaps;
    c->no_new_privs = parent->no_new_privs;
    c->securebits = parent->securebits;
    c->subreaper = parent->subreaper;
    c->timerslack = parent->timerslack;
    c->oom_score_adj = parent->oom_score_adj;
    c->capbset = parent->capbset;
    c->brk_base = parent->brk_base;
    c->brk_cur = parent->brk_cur;
    c->vfork = vfork != 0;
    if (vfork) {
        c->mm = parent->mm;
        mem_mm_ref(c->mm);
    } else {
        c->mm = mem_mm_fork(parent->mm);
    }
    c->fds = vfs_fdtab_fork(parent->fds);
    c->sig = calloc(1, sig_tab_size());
    if (!c->mm || !c->fds || !c->sig) {
        if (c->mm)
            mem_mm_unref(c->mm);
        if (c->fds)
            vfs_fdtab_release(c->fds);
        free(c->sig);
        free(c);
        return NULL;
    }
    sig_tab_copy(c->sig, parent->sig);
    link_child(parent, c);
    c->all_next = all_procs;
    all_procs = c;
    return c;
}

proc_t *proc_first(void) { return all_procs; }

proc_t *proc_next(proc_t *p) { return p->all_next; }

int proc_is_initial(const proc_t *p) { return p->pid == INIT_PID; }

proc_t *proc_find(int pid)
{
    for (proc_t *p = all_procs; p; p = p->all_next)
        if (p->pid == pid)
            return p;
    return NULL;
}

void proc_release(proc_t *p)
{
    cgroupfs_proc_exit(p);
    fc_flush_all(); /* a process's stores to shared file views reach the files when it exits */
    if (p->mm) {
        uint64_t rss = mem_mm_rss_kib(p->mm);
        if (rss > p->maxrss_kib)
            p->maxrss_kib = rss;
    }
    if (p->fds) {
        vfs_fdtab_release(p->fds);
        p->fds = NULL;
    }
    if (p->mm) {
        mem_mm_unref(p->mm);
        p->mm = NULL;
    }
    free(p->sig);
    p->sig = NULL;
}

void proc_reparent_children(proc_t *p, proc_t *init)
{
    while (p->first_child) {
        proc_t *c = p->first_child;
        unlink_child(c);
        c->ppid = init->pid;
        link_child(init, c);
    }
}

void proc_free(proc_t *p)
{
    proc_t *par = p->parent;
    if (par) {
        par->cu_icount += p->dead_icount + p->cu_icount;
        par->cu_nsys += p->nsys + p->cu_nsys;
        uint64_t rss = p->maxrss_kib > p->cu_maxrss_kib ? p->maxrss_kib : p->cu_maxrss_kib;
        if (rss > par->cu_maxrss_kib)
            par->cu_maxrss_kib = rss;
    }
    unlink_child(p);
    proc_t **pp = &all_procs;
    while (*pp && *pp != p)
        pp = &(*pp)->all_next;
    if (*pp)
        *pp = p->all_next;
    free(p);
}
