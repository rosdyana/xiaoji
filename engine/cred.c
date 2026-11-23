/* Process credentials: the ids, the supplementary groups and the capability sets, and the calls that
 * change them. The rules follow the Linux 6.12 sources the builder VM has: kernel/sys.c (the set*id
 * calls), kernel/groups.c, kernel/capability.c (capget, capset) and security/commoncap.c (the
 * capability juggling of a uid change, the exec transform, PR_CAPBSET_* and PR_CAP_AMBIENT).
 * A privileged check is the effective set holding CAP_SETUID, CAP_SETGID or CAP_SETPCAP (ns_capable
 * in the kernel), not the uid. The ids are not used for file access checks: stat reports uid and gid
 * 0 for every file, and no permission is enforced (docs/boot.md, B6). */
#include "cred.h"

#include <string.h>

#include "linux.h"
#include "mem.h"
#include "proc.h"

#define ID_KEEP 0xFFFFFFFFu /* -1: leave this id as it is */

#define CAP_V1 0x19980330u
#define CAP_V2 0x20071026u
#define CAP_V3 0x20080522u

/* security/commoncap.c: the capabilities that follow fsuid 0 (CAP_FS_SET, with CAP_LINUX_IMMUTABLE). */
#define CAP_BIT(n) (1ull << (n))
#define CAP_FS_SET (CAP_BIT(LCAP_CHOWN) | CAP_BIT(LCAP_MKNOD) | CAP_BIT(LCAP_DAC_OVERRIDE) |                 \
                    CAP_BIT(LCAP_DAC_READ_SEARCH) | CAP_BIT(LCAP_FOWNER) | CAP_BIT(LCAP_FSETID) |             \
                    CAP_BIT(LCAP_MAC_OVERRIDE) | CAP_BIT(LCAP_LINUX_IMMUTABLE))

static int cap_on(uint64_t set, int cap)
{
    return (int)((set >> cap) & 1u);
}

int cred_has_cap(const proc_t *p, int cap)
{
    return cap_on(p->cred.cap_eff, cap);
}

static int held(const proc_cred_t *c, uint32_t id, int with_saved)
{
    return id == c->ruid || id == c->euid || (with_saved && id == c->suid);
}

static int held_g(const proc_cred_t *c, uint32_t id)
{
    return id == c->rgid || id == c->egid || id == c->sgid;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/* commoncap.c, cap_emulate_setxuid: the capabilities follow a uid change (setreuid, setuid, setresuid).
 * keepcaps is the process's SECBIT_KEEP_CAPS. */
static void emulate_setxuid(proc_cred_t *n, const proc_cred_t *o, int keepcaps)
{
    if ((o->ruid == 0 || o->euid == 0 || o->suid == 0) && n->ruid != 0 && n->euid != 0 && n->suid != 0) {
        if (!keepcaps) {
            n->cap_perm = 0;
            n->cap_eff = 0;
        }
        n->cap_amb = 0;
    }
    if (o->euid == 0 && n->euid != 0)
        n->cap_eff = 0;
    if (o->euid != 0 && n->euid == 0)
        n->cap_eff = n->cap_perm;
}

/* commoncap.c, cap_task_fix_setuid for LSM_SETID_FS: the fsuid changes 0 and non-zero. */
static void fix_setfsuid(proc_cred_t *n, const proc_cred_t *o)
{
    if (o->fsuid == 0 && n->fsuid != 0)
        n->cap_eff &= ~CAP_FS_SET;
    if (o->fsuid != 0 && n->fsuid == 0)
        n->cap_eff |= n->cap_perm & CAP_FS_SET;
}

/* kernel/sys.c, setresuid(ruid, euid, suid). */
static int64_t set_resuid(proc_t *p, uint32_t r, uint32_t e, uint32_t s)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    if (!cap_on(o.cap_eff, LCAP_SETUID)) {
        if ((r != ID_KEEP && !held(&o, r, 1)) || (e != ID_KEEP && !held(&o, e, 1)) ||
            (s != ID_KEEP && !held(&o, s, 1)))
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->ruid = r;
    if (e != ID_KEEP)
        c->euid = e;
    if (s != ID_KEEP)
        c->suid = s;
    c->fsuid = c->euid;
    emulate_setxuid(c, &o, p->keepcaps);
    return 0;
}

/* kernel/sys.c, setresgid(rgid, egid, sgid). */
static int64_t set_resgid(proc_t *p, uint32_t r, uint32_t e, uint32_t s)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    if (!cap_on(o.cap_eff, LCAP_SETGID)) {
        if ((r != ID_KEEP && !held_g(&o, r)) || (e != ID_KEEP && !held_g(&o, e)) ||
            (s != ID_KEEP && !held_g(&o, s)))
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->rgid = r;
    if (e != ID_KEEP)
        c->egid = e;
    if (s != ID_KEEP)
        c->sgid = s;
    c->fsgid = c->egid;
    return 0;
}

/* kernel/sys.c, setreuid(ruid, euid). */
static int64_t set_reuid(proc_t *p, uint32_t r, uint32_t e)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    if (!cap_on(o.cap_eff, LCAP_SETUID)) {
        if (r != ID_KEEP && r != o.ruid && r != o.euid)
            return -LE_PERM;
        if (e != ID_KEEP && e != o.ruid && e != o.euid && e != o.suid)
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->ruid = r;
    if (e != ID_KEEP)
        c->euid = e;
    if (r != ID_KEEP || (e != ID_KEEP && e != o.ruid))
        c->suid = c->euid;
    c->fsuid = c->euid;
    emulate_setxuid(c, &o, p->keepcaps);
    return 0;
}

/* kernel/sys.c, setregid(rgid, egid). */
static int64_t set_regid(proc_t *p, uint32_t r, uint32_t e)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    if (!cap_on(o.cap_eff, LCAP_SETGID)) {
        if (r != ID_KEEP && r != o.rgid && r != o.egid)
            return -LE_PERM;
        if (e != ID_KEEP && e != o.rgid && e != o.egid && e != o.sgid)
            return -LE_PERM;
    }
    if (r != ID_KEEP)
        c->rgid = r;
    if (e != ID_KEEP)
        c->egid = e;
    if (r != ID_KEEP || (e != ID_KEEP && e != o.rgid))
        c->sgid = c->egid;
    c->fsgid = c->egid;
    return 0;
}

/* kernel/sys.c, setuid(uid). */
static int64_t set_uid(proc_t *p, uint32_t u)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    if (cap_on(o.cap_eff, LCAP_SETUID))
        c->ruid = c->suid = u;
    else if (u != o.ruid && u != o.suid)
        return -LE_PERM;
    c->euid = c->fsuid = u;
    emulate_setxuid(c, &o, p->keepcaps);
    return 0;
}

/* kernel/sys.c, setgid(gid). */
static int64_t set_gid(proc_t *p, uint32_t g)
{
    proc_cred_t *c = &p->cred;
    if (cap_on(c->cap_eff, LCAP_SETGID))
        c->rgid = c->egid = c->sgid = g;
    else if (g != c->rgid && g != c->sgid)
        return -LE_PERM;
    c->egid = c->fsgid = g;
    return 0;
}

/* kernel/sys.c, setfsuid(uid): returns the previous fsuid; changed only for an id the process holds. */
static int64_t set_fsuid(proc_t *p, uint32_t u)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    uint32_t old = c->fsuid;
    if (u == o.ruid || u == o.euid || u == o.suid || u == o.fsuid || cap_on(o.cap_eff, LCAP_SETUID)) {
        if (u != o.fsuid) {
            c->fsuid = u;
            fix_setfsuid(c, &o);
        }
    }
    return old;
}

/* kernel/sys.c, setfsgid(gid): returns the previous fsgid. */
static int64_t set_fsgid(proc_t *p, uint32_t g)
{
    proc_cred_t *c = &p->cred;
    uint32_t old = c->fsgid;
    if (g == c->rgid || g == c->egid || g == c->sgid || g == c->fsgid || cap_on(c->cap_eff, LCAP_SETGID)) {
        if (g != c->fsgid)
            c->fsgid = g;
    }
    return old;
}

/* capability(7) / kernel/capability.c, cap_validate_magic: the header version selects the data size
 * (1 or 2 structs). An unknown version is answered with the preferred one, and EINVAL. */
static int64_t cap_header(uint64_t hdr_va, uint32_t *tocopy)
{
    uint8_t h[4];
    if (mem_copy_out(h, hdr_va, sizeof h) < 0)
        return -LE_FAULT;
    uint32_t v = le32(h);
    if (v == CAP_V1) {
        *tocopy = 1;
        return 0;
    }
    if (v == CAP_V2 || v == CAP_V3) {
        *tocopy = 2;
        return 0;
    }
    put32(h, CAP_V3);
    return mem_copy_in(hdr_va, h, sizeof h) < 0 ? -LE_FAULT : -LE_INVAL;
}

/* kernel/capability.c, capget(header, data). The data is tocopy structs of {effective, permitted,
 * inheritable} (32 bits each), low word first. */
static int64_t cap_get(uint64_t hdr_va, uint64_t data_va)
{
    uint32_t tocopy = 0;
    int64_t ret = cap_header(hdr_va, &tocopy);
    if (data_va == 0 || ret != 0)
        return (data_va == 0 && ret == -LE_INVAL) ? 0 : ret;
    uint8_t hb[8];
    if (mem_copy_out(hb, hdr_va, sizeof hb) < 0)
        return -LE_FAULT;
    int32_t pid = (int32_t)le32(hb + 4);
    if (pid < 0)
        return -LE_INVAL;
    const proc_cred_t *t;
    if (pid == 0 || pid == proc_cur()->pid) {
        t = &proc_cur()->cred;
    } else {
        proc_t *q = proc_find(pid);
        if (!q)
            return -LE_SRCH;
        t = &q->cred;
    }
    uint64_t sets[3] = {t->cap_eff, t->cap_perm, t->cap_inh};
    uint8_t d[24];
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++)
            put32(d + 4 * (3 * i + j), (uint32_t)(sets[j] >> (32 * i)));
    }
    return mem_copy_in(data_va, d, (size_t)tocopy * 12) < 0 ? -LE_FAULT : 0;
}

/* security/commoncap.c, cap_capset: the new effective, permitted and inheritable sets, if they are allowed. */
static int64_t cap_apply(proc_t *p, uint64_t eff, uint64_t perm, uint64_t inh)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    /* Without CAP_SETPCAP the new inheritable set stays within the old inheritable and permitted sets. */
    if (!cap_on(o.cap_eff, LCAP_SETPCAP) && (inh & ~(o.cap_inh | o.cap_perm)) != 0)
        return -LE_PERM;
    if ((inh & ~(o.cap_inh | p->capbset)) != 0)
        return -LE_PERM;
    if ((perm & ~o.cap_perm) != 0)
        return -LE_PERM;
    if ((eff & ~perm) != 0)
        return -LE_PERM;
    c->cap_eff = eff;
    c->cap_perm = perm;
    c->cap_inh = inh;
    c->cap_amb &= perm & inh;
    return 0;
}

/* kernel/capability.c, capset(header, data), with security/commoncap.c, cap_capset. */
static int64_t cap_set(proc_t *p, uint64_t hdr_va, uint64_t data_va)
{
    uint32_t tocopy = 0;
    int64_t ret = cap_header(hdr_va, &tocopy);
    if (ret != 0)
        return ret;
    uint8_t hb[8];
    if (mem_copy_out(hb, hdr_va, sizeof hb) < 0)
        return -LE_FAULT;
    int32_t pid = (int32_t)le32(hb + 4);
    if (pid != 0 && pid != p->pid)
        return -LE_PERM;
    uint8_t d[24] = {0};
    if (mem_copy_out(d, data_va, (size_t)tocopy * 12) < 0)
        return -LE_FAULT;
    uint64_t eff = le32(d) | (uint64_t)le32(d + 12) << 32;
    uint64_t perm = le32(d + 4) | (uint64_t)le32(d + 16) << 32;
    uint64_t inh = le32(d + 8) | (uint64_t)le32(d + 20) << 32;

    return cap_apply(p, eff, perm, inh);
}

/* The exec transform (commoncap.c, cap_bprm_creds_from_file) for a program with no file capabilities
 * and no set-id bits. Only the root part is kept: a root process (real or effective uid 0) gets
 * permitted = bounding | inheritable, and effective when the effective uid is 0. The rest of the
 * permitted set is the ambient set. */
void cred_exec(proc_t *p)
{
    proc_cred_t o = p->cred;
    proc_cred_t *c = &p->cred;
    int effective = 0;
    c->cap_perm = 0; /* get_file_caps clears it, and no file capabilities are added */
    if (c->euid == 0 || c->ruid == 0) {
        c->cap_perm = p->capbset | o.cap_inh;
        if (c->euid == 0)
            effective = 1;
    }
    /* Set-id: the effective ids differ from the real ones (no set-id file is modelled). The downgrade
     * runs when the program is set-id or gains permitted caps, with no tracer. */
    int is_setid = c->euid != o.ruid || c->egid != o.rgid;
    uint64_t gained = c->cap_perm & ~o.cap_perm;
    if (is_setid || gained != 0) {
        if (!cap_on(o.cap_eff, LCAP_SETUID) || p->no_new_privs) {
            c->euid = c->ruid;
            c->egid = c->rgid;
        }
        c->cap_perm &= o.cap_perm;
    }
    c->suid = c->fsuid = c->euid;
    c->sgid = c->fsgid = c->egid;
    if (is_setid)
        c->cap_amb = 0; /* file caps or set-id cancel the ambient set */
    c->cap_perm |= c->cap_amb;
    c->cap_eff = effective ? c->cap_perm : c->cap_amb;
    p->keepcaps = 0;
}

/* init's SetCapsForExec (capabilities.cpp): the set is kept in the permitted and inheritable sets, the
 * bounding set is cut to it, and it is raised as the ambient set, so that it survives execve. */
static int64_t set_caps_for_exec(proc_t *p, uint64_t keep)
{
    int64_t rc = cap_apply(p, CAP_BIT(LCAP_SETPCAP), keep | CAP_BIT(LCAP_SETPCAP), keep);
    if (rc)
        return rc;
    for (int cap = 0; cap <= PROC_CAP_LAST; cap++) {
        if (!((keep >> cap) & 1) && (rc = cred_capbset_drop(p, (uint64_t)cap)) != 0)
            return rc;
    }
    rc = cap_apply(p, 0, keep, keep);
    if (rc)
        return rc;
    for (int cap = 0; cap <= PROC_CAP_LAST; cap++) {
        if (((keep >> cap) & 1) && (rc = cred_ambient(p, LPR_CAP_AMBIENT_RAISE, (uint64_t)cap, 0, 0)) != 0)
            return rc;
    }
    return 0;
}

int64_t cred_apply_service(proc_t *p, uint32_t uid, uint32_t gid, const uint32_t *supp, int nsupp, int has_caps,
                           uint64_t caps)
{
    proc_cred_t *c = &p->cred;
    int64_t rc;
    if (nsupp < 0 || nsupp > CRED_MAX_GROUPS)
        return -LE_INVAL;
    /* SetProcessAttributesAndCaps: keep-caps across the uid change, then the gid, the groups and the uid. */
    if (has_caps && uid != 0)
        p->keepcaps = 1;
    if (gid != 0 && (rc = set_gid(p, gid)) != 0)
        return rc;
    if (!cap_on(c->cap_eff, LCAP_SETGID))
        return -LE_PERM;
    memcpy(c->groups, supp, (size_t)nsupp * sizeof c->groups[0]);
    c->ngroups = nsupp;
    if (uid != 0 && (rc = set_uid(p, uid)) != 0)
        return rc;
    if (has_caps)
        return set_caps_for_exec(p, caps);
    if (uid != 0) /* DropInheritableCaps */
        return cap_apply(p, c->cap_eff, c->cap_perm, 0);
    return 0;
}

int64_t cred_capbset_read(const proc_t *p, uint64_t cap)
{
    if (cap > PROC_CAP_LAST)
        return -LE_INVAL;
    return (int64_t)cap_on(p->capbset, (int)cap);
}

int64_t cred_capbset_drop(proc_t *p, uint64_t cap)
{
    if (!cred_has_cap(p, LCAP_SETPCAP))
        return -LE_PERM;
    if (cap > PROC_CAP_LAST)
        return -LE_INVAL;
    p->capbset &= ~CAP_BIT(cap);
    return 0;
}

int64_t cred_ambient(proc_t *p, uint64_t op, uint64_t cap, uint64_t a4, uint64_t a5)
{
    proc_cred_t *c = &p->cred;
    if (op == LPR_CAP_AMBIENT_CLEAR_ALL) {
        if (cap | a4 | a5)
            return -LE_INVAL;
        c->cap_amb = 0;
        return 0;
    }
    if (cap > PROC_CAP_LAST || a4 || a5)
        return -LE_INVAL;
    if (op == LPR_CAP_AMBIENT_IS_SET)
        return cap_on(c->cap_amb, (int)cap);
    if (op != LPR_CAP_AMBIENT_RAISE && op != LPR_CAP_AMBIENT_LOWER)
        return -LE_INVAL;
    if (op == LPR_CAP_AMBIENT_RAISE) {
        if (!cap_on(c->cap_perm, (int)cap) || !cap_on(c->cap_inh, (int)cap))
            return -LE_PERM;
        c->cap_amb |= CAP_BIT(cap);
    } else {
        c->cap_amb &= ~CAP_BIT(cap);
    }
    return 0;
}

static void put_u32s(uint8_t *b, const uint32_t *v, int n)
{
    for (int i = 0; i < n; i++)
        put32(b + 4 * i, v[i]);
}

int64_t cred_syscall(uint64_t nr, const uint64_t a[6])
{
    proc_t *p = proc_cur();
    proc_cred_t *c = &p->cred;
    uint32_t x0 = (uint32_t)a[0], x1 = (uint32_t)a[1], x2 = (uint32_t)a[2];
    switch (nr) {
    case 90: /* capget(header, data) */
        return cap_get(a[0], a[1]);
    case 91: /* capset(header, data) */
        return cap_set(p, a[0], a[1]);
    case 143: /* setregid(rgid, egid) */
        return set_regid(p, x0, x1);
    case 144: /* setgid(gid) */
        return set_gid(p, x0);
    case 145: /* setreuid(ruid, euid) */
        return set_reuid(p, x0, x1);
    case 146: /* setuid(uid) */
        return set_uid(p, x0);
    case 147: /* setresuid(ruid, euid, suid) */
        return set_resuid(p, x0, x1, x2);
    case 148: /* getresuid(&ruid, &euid, &suid) */
    {
        uint8_t b[12];
        put_u32s(b, (const uint32_t[]){c->ruid, c->euid, c->suid}, 3);
        return mem_copy_in(a[0], b, 4) < 0 || mem_copy_in(a[1], b + 4, 4) < 0 ||
                       mem_copy_in(a[2], b + 8, 4) < 0
                   ? -LE_FAULT
                   : 0;
    }
    case 149: /* setresgid(rgid, egid, sgid) */
        return set_resgid(p, x0, x1, x2);
    case 150: /* getresgid(&rgid, &egid, &sgid) */
    {
        uint8_t b[12];
        put_u32s(b, (const uint32_t[]){c->rgid, c->egid, c->sgid}, 3);
        return mem_copy_in(a[0], b, 4) < 0 || mem_copy_in(a[1], b + 4, 4) < 0 ||
                       mem_copy_in(a[2], b + 8, 4) < 0
                   ? -LE_FAULT
                   : 0;
    }
    case 151: /* setfsuid(uid) */
        return set_fsuid(p, x0);
    case 152: /* setfsgid(gid) */
        return set_fsgid(p, x0);
    case 158: /* getgroups(size, list) */
    {
        if (x0 == 0)
            return c->ngroups;
        if (x0 < (uint32_t)c->ngroups)
            return -LE_INVAL;
        uint8_t b[4 * CRED_MAX_GROUPS];
        put_u32s(b, c->groups, c->ngroups);
        if (c->ngroups && mem_copy_in(a[1], b, (size_t)c->ngroups * 4) < 0)
            return -LE_FAULT;
        return c->ngroups;
    }
    case 159: /* setgroups(size, list): needs CAP_SETGID; more than CRED_MAX_GROUPS is refused */
    {
        if (x0 > CRED_MAX_GROUPS)
            return -LE_INVAL;
        if (!cap_on(c->cap_eff, LCAP_SETGID))
            return -LE_PERM;
        uint8_t b[4 * CRED_MAX_GROUPS];
        if (x0 && mem_copy_out(b, a[1], (size_t)x0 * 4) < 0)
            return -LE_FAULT;
        for (uint32_t i = 0; i < x0; i++)
            c->groups[i] = le32(b + 4 * i);
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
