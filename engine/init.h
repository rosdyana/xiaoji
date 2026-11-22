/* The built-in init (B4a), started by --init CONFIG. It makes the sockets of each service,
 * computes BOOTCLASSPATH and the other classpath variables the way derive_classpath does,
 * starts and restarts the services, acts on ctl.* properties, and stops when told to.
 * See init.c and docs/boot.md. */
#ifndef XIAOJI_INIT_H
#define XIAOJI_INIT_H

#include <stddef.h>
#include <stdint.h>

/* Runs init with the given config file (a host path). timeout_s is the time after which
 * init stops (0 for none). Returns the engine's exit status. */
int init_main(const char *config, uint64_t timeout_s);

/* 1 while init runs (the property hooks below act only then). */
int init_active(void);
/* --trace for the services' threads. */
void init_set_trace(int on);

/* A ctl.start, ctl.stop or ctl.restart property (action) for a service (svc). Queued: init
 * acts between slices, never inside the property call. */
void init_ctl_request(const char *action, size_t alen, const char *svc, size_t slen);

/* Any property set (name, value). xiaoji.exit stops init. */
void init_prop_changed(const char *name, size_t nlen, const char *value, size_t vlen);

#endif
