/* Process credentials: the uid and gid the guest sees, and the calls that change them
 * (setuid and its family, setgroups). The rules follow kernel/sys.c: a process with effective
 * uid 0 is privileged (it has the full capability set that capget reports); an unprivileged
 * process may only set ids it already holds. The ids are not used for access checks: stat
 * reports uid and gid 0 for every file, and no permission is enforced (see docs/boot.md). */
#include "cred.h"

#include <string.h>

#include "linux.h"
#include "mem.h"
#include "proc.h"

#define ID_KEEP 0xFFFFFFFFu /* -1: leave this id as it is */

static int privileged(const proc_cred_t *c)
{
    return c->euid == 0;
}

static int held(const proc_cred_t *c, uint32_t id, int with_saved)
{
    return id == c->ruid || id == c->euid || (with_saved && id == c->suid);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int64_t setres(proc_cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
    if (!privileged(c)) {
        if ((r != ID_KEEP && !held(c, r, 1)) || (e != ID_KEEP && !held(c, e, 1)) ||
            (s != ID_KEEP && !held(c, s, 1)))
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->ruid = r;
    if (e != ID_KEEP) {
        c->euid = e;
        c->fsuid = e;
    }
    if (s != ID_KEEP)
        c->suid = s;
    return 0;
}

static int64_t setresg(proc_cred_t *c, uint32_t r, uint32_t e, uint32_t s)
{
    if (!privileged(c)) {
        if ((r != ID_KEEP && !(r == c->rgid || r == c->egid || r == c->sgid)) ||
            (e != ID_KEEP && !(e == c->rgid || e == c->egid || e == c->sgid)) ||
            (s != ID_KEEP && !(s == c->rgid || s == c->egid || s == c->sgid)))
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->rgid = r;
    if (e != ID_KEEP) {
        c->egid = e;
        c->fsgid = e;
    }
    if (s != ID_KEEP)
        c->sgid = s;
    return 0;
}

int64_t cred_syscall(uint64_t nr, const uint64_t a[6])
{
    proc_t *p = proc_cur();
    proc_cred_t *c = &p->cred;
    uint32_t x0 = (uint32_t)a[0], x1 = (uint32_t)a[1], x2 = (uint32_t)a[2];
    switch (nr) {
    case 143: /* setregid(rgid, egid) */
    {
        if (!privileged(c)) {
            if ((x0 != ID_KEEP && x0 != c->rgid && x0 != c->egid) ||
                (x1 != ID_KEEP && x1 != c->rgid && x1 != c->egid && x1 != c->sgid))
                return -LE_PERM;
        }
        uint32_t old_r = c->rgid;
        if (x0 != ID_KEEP)
            c->rgid = x0;
        if (x1 != ID_KEEP) {
            c->egid = x1;
            c->fsgid = x1;
        }
        if (x0 != ID_KEEP || (x1 != ID_KEEP && x1 != old_r))
            c->sgid = c->egid;
        return 0;
    }
    case 144: /* setgid(gid) */
        if (privileged(c)) {
            c->rgid = c->egid = c->sgid = c->fsgid = x0;
            return 0;
        }
        if (x0 != c->rgid && x0 != c->sgid)
            return -LE_PERM;
        c->egid = c->fsgid = x0;
        return 0;
    case 145: /* setreuid(ruid, euid) */
    {
        if (!privileged(c)) {
            if ((x0 != ID_KEEP && x0 != c->ruid && x0 != c->euid) ||
                (x1 != ID_KEEP && x1 != c->ruid && x1 != c->euid && x1 != c->suid))
                return -LE_PERM;
        }
        uint32_t old_r = c->ruid;
        if (x0 != ID_KEEP)
            c->ruid = x0;
        if (x1 != ID_KEEP) {
            c->euid = x1;
            c->fsuid = x1;
        }
        if (x0 != ID_KEEP || (x1 != ID_KEEP && x1 != old_r))
            c->suid = c->euid;
        return 0;
    }
    case 146: /* setuid(uid) */
        if (privileged(c)) {
            c->ruid = c->euid = c->suid = c->fsuid = x0;
            return 0;
        }
        if (x0 != c->ruid && x0 != c->suid)
            return -LE_PERM;
        c->euid = c->fsuid = x0;
        return 0;
    case 147: /* setresuid(ruid, euid, suid) */
        return setres(c, x0, x1, x2);
    case 148: /* getresuid(&ruid, &euid, &suid) */
    {
        uint8_t b[12];
        put32(b, c->ruid);
        put32(b + 4, c->euid);
        put32(b + 8, c->suid);
        return mem_copy_in(a[0], b, 4) < 0 || mem_copy_in(a[1], b + 4, 4) < 0 ||
                       mem_copy_in(a[2], b + 8, 4) < 0
                   ? -LE_FAULT
                   : 0;
    }
    case 149: /* setresgid(rgid, egid, sgid) */
        return setresg(c, x0, x1, x2);
    case 150: /* getresgid(&rgid, &egid, &sgid) */
    {
        uint8_t b[12];
        put32(b, c->rgid);
        put32(b + 4, c->egid);
        put32(b + 8, c->sgid);
        return mem_copy_in(a[0], b, 4) < 0 || mem_copy_in(a[1], b + 4, 4) < 0 ||
                       mem_copy_in(a[2], b + 8, 4) < 0
                   ? -LE_FAULT
                   : 0;
    }
    case 151: /* setfsuid(uid): returns the previous value; changed only for a held id */
    {
        uint32_t old = c->fsuid;
        if (privileged(c) || held(c, x0, 1) || x0 == c->fsuid)
            c->fsuid = x0;
        return old;
    }
    case 152: /* setfsgid(gid) */
    {
        uint32_t old = c->fsgid;
        if (privileged(c) || x0 == c->rgid || x0 == c->egid || x0 == c->sgid || x0 == c->fsgid)
            c->fsgid = x0;
        return old;
    }
    case 158: /* getgroups(size, list) */
    {
        if (x0 == 0)
            return c->ngroups;
        if (x0 < (uint32_t)c->ngroups)
            return -LE_INVAL;
        uint8_t b[4 * CRED_MAX_GROUPS];
        for (int i = 0; i < c->ngroups; i++)
            put32(b + 4 * i, c->groups[i]);
        if (c->ngroups && mem_copy_in(a[1], b, (size_t)c->ngroups * 4) < 0)
            return -LE_FAULT;
        return c->ngroups;
    }
    case 159: /* setgroups(size, list): privileged only */
    {
        if (!privileged(c))
            return -LE_PERM;
        if (x0 > CRED_MAX_GROUPS)
            return -LE_INVAL;
        uint8_t b[4 * CRED_MAX_GROUPS];
        if (x0 && mem_copy_out(b, a[1], (size_t)x0 * 4) < 0)
            return -LE_FAULT;
        for (uint32_t i = 0; i < x0; i++)
            c->groups[i] = (uint32_t)b[4 * i] | (uint32_t)b[4 * i + 1] << 8 |
                           (uint32_t)b[4 * i + 2] << 16 | (uint32_t)b[4 * i + 3] << 24;
        c->ngroups = (int)x0;
        return 0;
    }
    case 174: return c->ruid;
    case 175: return c->euid;
    case 176: return c->rgid;
    case 177: return c->egid;
    default:
        return -LE_NOSYS;
    }
}
