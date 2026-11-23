/* inotify (B9): instances and their watches. No event is ever queued (inotify.c). */
#ifndef XIAOJI_INOTIFY_H
#define XIAOJI_INOTIFY_H

#include <stdint.h>

void *ino_new(void);
void ino_free(void *o);
/* inotify_add_watch on the instance, for the canonical guest path: its watch descriptor (>= 1), or -1 when the
 * instance has no room. A watch of the same path is returned again, with the new mask. */
int ino_add(void *o, const char *guest_path, uint32_t mask);
/* inotify_rm_watch: 0, or -1 for no such watch. */
int ino_rm(void *o, int wd);

#endif
