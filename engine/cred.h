/* Process credentials: the ids, the supplementary groups, the capability sets, and the calls that
 * change them (cred.c). */
#ifndef XIAOJI_CRED_H
#define XIAOJI_CRED_H

#include <stdint.h>

#include "proc.h"

/* The credential syscall nr (capget 90, capset 91, 143 to 152, 158, 159, 174 to 177) with its
 * arguments a. Returns the Linux result (or -errno). */
int64_t cred_syscall(uint64_t nr, const uint64_t a[6]);

/* 1 if the effective set of p holds capability cap. */
int cred_has_cap(const proc_t *p, int cap);

/* prctl(PR_CAPBSET_READ) and prctl(PR_CAPBSET_DROP) for p. */
int64_t cred_capbset_read(const proc_t *p, uint64_t cap);
int64_t cred_capbset_drop(proc_t *p, uint64_t cap);

/* prctl(PR_CAP_AMBIENT, op, cap, a4, a5) for p (the ambient set). */
int64_t cred_ambient(proc_t *p, uint64_t op, uint64_t cap, uint64_t a4, uint64_t a5);

/* init's service start (service.cpp, SetProcessAttributesAndCaps), in the service's process before its
 * program is loaded: keep-caps when the service has capabilities and a non-zero uid, then the gid (if not 0),
 * the supplementary groups, the uid (if not 0), and the capability set (has_caps: caps, the set kept across
 * exec; without it, a non-root service drops its inheritable set). Returns 0 or -errno. */
int64_t cred_apply_service(proc_t *p, uint32_t uid, uint32_t gid, const uint32_t *supp, int nsupp, int has_caps,
                           uint64_t caps);

/* The credential part of execve for p, after its program is in place (no file capabilities and no
 * set-id bits are modelled, see docs/boot.md, B6): clears keep-caps, and recomputes the sets as
 * security/commoncap.c does (cap_bprm_creds_from_file). */
void cred_exec(proc_t *p);

#endif
