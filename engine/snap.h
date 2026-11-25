/* Snapshots of the whole engine (docs/snapshot.md): --snapshot-at writes one at a slice boundary and stops the engine;
 * --restore reads it back into a fresh run of the same engine build, which goes on from there. */
#ifndef XIAOJI_SNAP_H
#define XIAOJI_SNAP_H

#include <stddef.h>
#include <stdint.h>

/* The condition (--snapshot-at): "time:S" (S seconds of engine time from the start), "icount:N" (N guest
 * instructions), "log:TEXT" (the first guest log line that contains TEXT), "prop:NAME=VALUE" (a property set to that
 * value). 0, or -1 for a bad condition. */
int snap_set_trigger(const char *cond);
/* Seconds to wait after the condition holds (--snapshot-after). */
void snap_set_after(uint64_t seconds);
/* The snapshot file (--snapshot-out). */
void snap_set_out(const char *path);

/* The scheduler loop calls this at every slice boundary: when the condition is due, the snapshot is written and the
 * engine exits (status 0, or 1 when the write failed). */
void snap_point(void);
/* The host monotonic time at which the snapshot falls due, when it is a time (the loop's idle sleeps end there). */
int snap_deadline(uint64_t *dl);
/* Hooks: each guest log line (logd), and each property change (props). */
void snap_note_log(const char *msg);
void snap_note_prop(const char *name, size_t nlen, const char *value, size_t vlen);

/* Exit handlers of the engine: the list is in engine memory, so a restored image keeps the handlers of the run that
 * wrote it. main registers snap_run_atexit with the C library once. */
void snap_atexit(void (*fn)(void));
void snap_run_atexit(void);

/* Reads the snapshot at path into memory (it replaces the engine's whole state). 0, or -1 with a message: a failure
 * before the image is read leaves the run as it was; a failure after it must end the run. */
int snap_restore(const char *path);
/* After snap_restore (and after main has applied this run's options): the host-side fixups (the clock, the host
 * descriptors, the writable trees, the caches), then the scheduler loop. Returns the engine's exit status. */
int snap_resume(uint64_t init_timeout_s);
/* 1 in a run that was restored from a snapshot. */
int snap_restored(void);

#endif
