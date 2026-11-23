/* inotify (B9): the instances and their watches.
 *
 * The engine serves inotify for the consumers that set it up at start-up: EventHub watches /dev and registers its
 * input directory (inotify_add_watch must succeed, or EventHub aborts). No event is ever queued. The engine has no
 * device hot-plug, and the guest's own changes to the file system are not reported, so a read gives EAGAIN and the
 * fd is never readable (a blocking read does not wait either: docs/boot.md B9, gaps). A watch is numbered as Linux
 * numbers it (from 1, per instance), and removing it is the only other state.
 */
#include "inotify.h"

#include <stdlib.h>
#include <string.h>

#define INO_MAX_WATCH 64

typedef struct {
    int wd;
    uint32_t mask;
    char *path;
} ino_watch;

typedef struct {
    int n;
    int next_wd;
    ino_watch w[INO_MAX_WATCH];
} ino_t;

void *ino_new(void)
{
    ino_t *i = calloc(1, sizeof *i);
    if (i)
        i->next_wd = 1;
    return i;
}

void ino_free(void *o)
{
    ino_t *i = o;
    if (!i)
        return;
    for (int k = 0; k < i->n; k++)
        free(i->w[k].path);
    free(i);
}

int ino_add(void *o, const char *path, uint32_t mask)
{
    ino_t *i = o;
    for (int k = 0; k < i->n; k++)
        if (!strcmp(i->w[k].path, path)) {
            i->w[k].mask = mask;
            return i->w[k].wd;
        }
    if (i->n == INO_MAX_WATCH)
        return -1;
    size_t len = strlen(path) + 1;
    char *p = malloc(len); /* strdup is not in C11 */
    if (!p)
        return -1;
    memcpy(p, path, len);
    ino_watch *w = &i->w[i->n++];
    w->wd = i->next_wd++;
    w->mask = mask;
    w->path = p;
    return w->wd;
}

int ino_rm(void *o, int wd)
{
    ino_t *i = o;
    for (int k = 0; k < i->n; k++)
        if (i->w[k].wd == wd) {
            free(i->w[k].path);
            i->w[k] = i->w[--i->n];
            return 0;
        }
    return -1;
}
