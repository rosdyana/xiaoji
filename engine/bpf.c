/* bpf(2), the maps-only subset (B15, docs/boot.md), and the synthetic bpffs at /sys/fs/bpf.
 *
 * What is here, with the errnos of Linux 6.12 (kernel/bpf/syscall.c, hashtab.c, arraymap.c, inode.c):
 *  - BPF_MAP_CREATE for HASH, LRU_HASH, PERCPU_HASH, LRU_PERCPU_HASH, ARRAY and PERCPU_ARRAY. LRU_HASH is a
 *    HASH here: a full map gives E2BIG where Linux evicts. The PERCPU types keep one CPU slot: a lookup
 *    copies that slot into the first CPU slot of the caller's buffer, and an update reads the first slot.
 *    Other known map types give EOPNOTSUPP; an unknown map type gives EINVAL.
 *  - BPF_MAP_LOOKUP_ELEM, BPF_MAP_UPDATE_ELEM (BPF_ANY, BPF_NOEXIST, BPF_EXIST; BPF_F_LOCK is EINVAL: the
 *    maps have no spin lock), BPF_MAP_DELETE_ELEM and BPF_MAP_GET_NEXT_KEY. The order of a hash's keys is
 *    the slot order here, not the kernel's hash order.
 *  - BPF_OBJ_PIN and BPF_OBJ_GET on the bpffs. The pinned object is a regular file (S_IFREG) with the mode
 *    (umask applied), owner and group the pin was made with. A get checks the access the file_flags ask for
 *    (BPF_F_RDONLY, BPF_F_WRONLY, else read and write) against the owner, group or other bits, and
 *    CAP_DAC_OVERRIDE bypasses it. A get on a directory is EACCES, as Linux's bpf_inode_type.
 *  - BPF_OBJ_GET_INFO_BY_FD for a map fd (type, id, key and value sizes, max entries, flags, name).
 *  - Every other command: EPERM for the known commands (programs, links, attach, tokens and the iterators:
 *    the engine has no CAP_BPF in this model) and EINVAL for a number that is no command. A caller that
 *    needs a program gets the same errno as a process without the capability.
 *
 * Map fds are VFS_BPF descriptions (vfs.c). Their map is shared engine-wide: fork, dup and SCM_RIGHTS share
 * the description, and a pin holds its own reference. Nothing here runs a program. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpf.h"
#include "cred.h"
#include "linux.h"
#include "mem.h"
#include "proc.h"
#include "vfs.h"

/* uapi values (linux/bpf.h, 6.12). */
#define BPFC_MAP_CREATE 0
#define BPFC_MAP_LOOKUP_ELEM 1
#define BPFC_MAP_UPDATE_ELEM 2
#define BPFC_MAP_DELETE_ELEM 3
#define BPFC_MAP_GET_NEXT_KEY 4
#define BPFC_OBJ_PIN 6
#define BPFC_OBJ_GET 7
#define BPFC_OBJ_GET_INFO_BY_FD 15
#define BPFC_LAST_KNOWN 36 /* BPF_TOKEN_CREATE: every command up to here exists in 6.12 */

#define BPFT_HASH 1
#define BPFT_ARRAY 2
#define BPFT_PERCPU_HASH 5
#define BPFT_PERCPU_ARRAY 6
#define BPFT_LRU_HASH 9
#define BPFT_LRU_PERCPU_HASH 10
#define BPFT_DEVMAP_HASH 25 /* kept as a HASH: the image's tether_dev_map; no interface is checked, no program runs */
#define BPFT_LAST 27 /* BPF_MAP_TYPE_RINGBUF: the last type number in 6.12 */

#define BPFF_NO_PREALLOC 0x1u
#define BPFF_NO_COMMON_LRU 0x2u
#define BPFF_NUMA_NODE 0x4u
#define BPFF_RDONLY 0x8u  /* BPF_F_RDONLY: the map is read-only from userspace */
#define BPFF_WRONLY 0x10u /* BPF_F_WRONLY */
#define BPFF_ZERO_SEED 0x40u
#define BPFF_PATH_FD 0x4000u /* BPF_F_PATH_FD */
#define BPF_LOCK_FLAG 0x4u   /* BPF_F_LOCK */
#define BPF_ANY 0u
#define BPF_NOEXIST 1u
#define BPF_EXIST 2u

#define BPF_OBJ_NAME_LEN 16
#define BPF_MAP_INFO_SIZE 88
#define BPF_MAX_ELEM_BYTES (4u * 1024u * 1024u) /* KMALLOC_MAX_SIZE on 4 KiB pages: E2BIG above */
#define PCPU_MIN_UNIT_SIZE 32768u
#define BPF_STORAGE_MAX ((uint64_t)256 << 20) /* more than this is ENOMEM */
#define BPF_ATTR_MAX 128u
#define BPF_SYSCALL_SIZE_MAX 4096u
#define CAP_DAC_OVERRIDE_BIT 1

#define ACC_READ 4u
#define ACC_WRITE 2u

typedef struct bpf_map {
    int refs;
    uint32_t type, key_size, value_size, max_entries, map_flags, id;
    uint32_t vsz;   /* bytes of one value in storage: value_size, or round8(value_size) for a PERCPU type */
    int percpu;
    char name[BPF_OBJ_NAME_LEN];
    uint8_t *used;  /* hash: one flag per slot */
    uint8_t *kv;    /* hash: slot i at kv + i * (key_size + vsz): the key, then the value */
    uint8_t *arr;   /* array: element i at arr + i * vsz */
    uint32_t count; /* hash: used slots */
} bpf_map_t;

static uint32_t next_map_id = 1;

/* ---------- guest attribute and memory access ---------- */

static uint32_t rd32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* Loads the union bpf_attr of size bytes into buf (BPF_ATTR_MAX bytes, zero past size). known is the end of the
 * command's last field: non-zero bytes between known and the end give EINVAL, and beyond BPF_ATTR_MAX, E2BIG
 * (the kernel's CHECK_ATTR and bpf_check_uarg_tail_zero). */
static int64_t attr_load(uint64_t attr, uint64_t size, uint8_t buf[BPF_ATTR_MAX], uint32_t known)
{
    memset(buf, 0, BPF_ATTR_MAX);
    if (size > BPF_SYSCALL_SIZE_MAX)
        return -LE_2BIG;
    uint32_t have = (uint32_t)size < BPF_ATTR_MAX ? (uint32_t)size : BPF_ATTR_MAX;
    if (have && mem_copy_out(buf, attr, have) < 0)
        return -LE_FAULT;
    for (uint64_t off = known; off < size; off += 64) {
        uint8_t tmp[64];
        size_t n = size - off < 64 ? (size_t)(size - off) : 64;
        if (mem_copy_out(tmp, attr + off, n) < 0)
            return -LE_FAULT;
        for (size_t i = 0; i < n; i++) {
            if (!tmp[i])
                continue;
            return off + i < BPF_ATTR_MAX ? -LE_INVAL : -LE_2BIG;
        }
    }
    return 0;
}

/* Copies the NUL-terminated path at va into out. ENAMETOOLONG past cap. */
static int64_t path_load(uint64_t va, char *out, size_t cap)
{
    int64_t n = mem_strlen(va);
    if (n < 0)
        return -LE_FAULT;
    if ((uint64_t)n >= cap)
        return -LE_NAMETOOLONG;
    if (mem_copy_out(out, va, (size_t)n + 1) < 0)
        return -LE_FAULT;
    return 0;
}

/* ---------- maps ---------- */

static void map_free(bpf_map_t *m)
{
    free(m->used);
    free(m->kv);
    free(m->arr);
    free(m);
}

void bpf_map_unref(void *o)
{
    bpf_map_t *m = o;
    if (m && --m->refs == 0)
        map_free(m);
}

static void map_ref(bpf_map_t *m)
{
    m->refs++;
}

static uint32_t round8(uint32_t v)
{
    return (v + 7u) & ~7u;
}

static int is_hash_type(uint32_t t)
{
    return t == BPFT_HASH || t == BPFT_LRU_HASH || t == BPFT_PERCPU_HASH || t == BPFT_LRU_PERCPU_HASH || t == BPFT_DEVMAP_HASH;
}

static int is_array_type(uint32_t t)
{
    return t == BPFT_ARRAY || t == BPFT_PERCPU_ARRAY;
}

static int is_percpu_type(uint32_t t)
{
    return t == BPFT_PERCPU_HASH || t == BPFT_LRU_PERCPU_HASH || t == BPFT_PERCPU_ARRAY;
}

/* The checks of htab_map_alloc_check and array_map_alloc_check (0 or -LE_*). */
static int64_t map_check(uint32_t type, uint32_t key, uint32_t value, uint32_t max, uint32_t flags)
{
    if (type == 0 || type > BPFT_LAST)
        return -LE_INVAL;
    if (is_hash_type(type)) {
        if (flags & ~(BPFF_NO_PREALLOC | BPFF_NO_COMMON_LRU | BPFF_NUMA_NODE | BPFF_ZERO_SEED | BPFF_RDONLY | BPFF_WRONLY))
            return -LE_INVAL;
        if ((flags & BPFF_RDONLY) && (flags & BPFF_WRONLY))
            return -LE_INVAL;
        if (max == 0 || key == 0 || value == 0)
            return -LE_INVAL;
        if ((uint64_t)key + value >= BPF_MAX_ELEM_BYTES)
            return -LE_2BIG;
        if (is_percpu_type(type) && round8(value) > PCPU_MIN_UNIT_SIZE)
            return -LE_2BIG;
        return 0;
    }
    if (is_array_type(type)) {
        if (flags & ~(BPFF_NUMA_NODE | BPFF_RDONLY | BPFF_WRONLY))
            return -LE_INVAL;
        if ((flags & BPFF_RDONLY) && (flags & BPFF_WRONLY))
            return -LE_INVAL;
        if (key != 4 || max == 0 || value == 0)
            return -LE_INVAL;
        if (value >= BPF_MAX_ELEM_BYTES)
            return -LE_2BIG;
        if (is_percpu_type(type) && round8(value) > PCPU_MIN_UNIT_SIZE)
            return -LE_2BIG;
        return 0;
    }
    return -LE_NOTSUP;
}

/* A new map with one reference (the caller's), or NULL with *err set. */
static bpf_map_t *map_new(uint32_t type, uint32_t key, uint32_t value, uint32_t max, uint32_t flags, const char *name,
                          int64_t *err)
{
    *err = map_check(type, key, value, max, flags);
    if (*err)
        return NULL;
    bpf_map_t *m = calloc(1, sizeof *m);
    if (!m) {
        *err = -LE_NOMEM;
        return NULL;
    }
    m->refs = 1;
    m->type = type;
    m->key_size = key;
    m->value_size = value;
    m->max_entries = max;
    m->map_flags = flags;
    m->percpu = is_percpu_type(type);
    m->vsz = m->percpu ? round8(value) : value;
    memcpy(m->name, name, BPF_OBJ_NAME_LEN - 1);
    m->name[BPF_OBJ_NAME_LEN - 1] = 0;
    if (is_hash_type(type)) {
        uint64_t stride = (uint64_t)key + m->vsz;
        if ((uint64_t)max * stride > BPF_STORAGE_MAX || (uint64_t)max * stride == 0) {
            *err = -LE_NOMEM;
            goto fail;
        }
        m->used = calloc(max, 1);
        m->kv = calloc(max, (size_t)stride);
        if (!m->used || !m->kv) {
            *err = -LE_NOMEM;
            goto fail;
        }
    } else {
        uint64_t total = (uint64_t)max * m->vsz;
        if (total > BPF_STORAGE_MAX || total == 0) {
            *err = -LE_NOMEM;
            goto fail;
        }
        m->arr = calloc(max, m->vsz);
        if (!m->arr) {
            *err = -LE_NOMEM;
            goto fail;
        }
    }
    m->id = next_map_id++;
    return m;
fail:
    map_free(m);
    return NULL;
}

/* The map of a map fd, or NULL with *err set. want is ACC_READ, ACC_WRITE, or both. The fd's access (the
 * open flags) and the map's own BPF_F_RDONLY and BPF_F_WRONLY both limit it. */
static bpf_map_t *fd_map(int fd, uint32_t want, int64_t *err)
{
    vfs_fd *f = vfs_get(fd);
    if (!f) {
        *err = -LE_BADF;
        return NULL;
    }
    if (f->kind != VFS_BPF) {
        *err = -LE_INVAL;
        return NULL;
    }
    bpf_map_t *m = f->obj;
    uint32_t acc = f->lflags & LO_ACCMODE;
    int can_read = acc != LO_WRONLY && !(m->map_flags & BPFF_WRONLY);
    int can_write = acc != LO_RDONLY && !(m->map_flags & BPFF_RDONLY);
    if (((want & ACC_READ) && !can_read) || ((want & ACC_WRITE) && !can_write)) {
        *err = -LE_PERM;
        return NULL;
    }
    return m;
}

/* The hash slot holding key, or -1. */
static int64_t hash_find(const bpf_map_t *m, const uint8_t *key)
{
    uint64_t stride = (uint64_t)m->key_size + m->vsz;
    for (uint32_t i = 0; i < m->max_entries; i++) {
        if (m->used[i] && !memcmp(m->kv + i * stride, key, m->key_size))
            return i;
    }
    return -1;
}

static uint8_t *hash_val(const bpf_map_t *m, uint32_t slot)
{
    uint64_t stride = (uint64_t)m->key_size + m->vsz;
    return m->kv + slot * stride + m->key_size;
}

/* Index of an array key (u32), or -1 when the index is past max_entries. */
static int64_t arr_index(const bpf_map_t *m, const uint8_t *key)
{
    uint32_t i = rd32(key);
    return i < m->max_entries ? (int64_t)i : -1;
}

/* The value of key (lookup), or NULL. */
static uint8_t *map_lookup(bpf_map_t *m, const uint8_t *key)
{
    if (is_hash_type(m->type)) {
        int64_t s = hash_find(m, key);
        return s < 0 ? NULL : hash_val(m, (uint32_t)s);
    }
    int64_t i = arr_index(m, key);
    return i < 0 ? NULL : m->arr + (uint64_t)i * m->vsz;
}

static int64_t map_update(bpf_map_t *m, const uint8_t *key, const uint8_t *val, uint64_t flags)
{
    if ((flags & ~(uint64_t)BPF_LOCK_FLAG) > BPF_EXIST)
        return -LE_INVAL;
    if (flags & BPF_LOCK_FLAG)
        return -LE_INVAL; /* no spin lock in these maps */
    uint32_t fl = (uint32_t)flags;
    if (is_hash_type(m->type)) {
        int64_t s = hash_find(m, key);
        if (s >= 0 && fl == BPF_NOEXIST)
            return -LE_EXIST;
        if (s < 0 && fl == BPF_EXIST)
            return -LE_NOENT;
        uint32_t slot;
        if (s >= 0) {
            slot = (uint32_t)s;
        } else {
            if (m->count >= m->max_entries)
                return -LE_2BIG;
            for (slot = 0; slot < m->max_entries && m->used[slot]; slot++)
                ;
            uint64_t stride = (uint64_t)m->key_size + m->vsz;
            memcpy(m->kv + slot * stride, key, m->key_size);
            memset(hash_val(m, slot), 0, m->vsz);
            m->used[slot] = 1;
            m->count++;
        }
        memcpy(hash_val(m, slot), val, m->value_size);
        return 0;
    }
    int64_t i = arr_index(m, key);
    if (i < 0)
        return -LE_2BIG;
    if (fl == BPF_NOEXIST)
        return -LE_EXIST;
    uint8_t *dst = m->arr + (uint64_t)i * m->vsz;
    memset(dst, 0, m->vsz);
    memcpy(dst, val, m->value_size);
    return 0;
}

static int64_t map_delete(bpf_map_t *m, const uint8_t *key)
{
    if (is_hash_type(m->type)) {
        int64_t s = hash_find(m, key);
        if (s < 0)
            return -LE_NOENT;
        m->used[s] = 0;
        m->count--;
        return 0;
    }
    return -LE_INVAL; /* array elements are never deleted */
}

/* The key after key (NULL: the first). Writes it to next and returns 0, or -LE_NOENT at the end. */
static int64_t map_next_key(bpf_map_t *m, const uint8_t *key, uint8_t *next)
{
    if (is_hash_type(m->type)) {
        uint64_t stride = (uint64_t)m->key_size + m->vsz;
        uint32_t start = 0;
        if (key) {
            int64_t s = hash_find(m, key);
            if (s >= 0)
                start = (uint32_t)s + 1;
        }
        for (uint32_t i = start; i < m->max_entries; i++) {
            if (m->used[i]) {
                memcpy(next, m->kv + i * stride, m->key_size);
                return 0;
            }
        }
        return -LE_NOENT;
    }
    /* array_map_get_next_key: a key past the end, or no key, starts at 0; the last index has no next. */
    uint32_t i = 0;
    if (key) {
        uint32_t k = rd32(key);
        if (k >= m->max_entries)
            i = 0;
        else if (k == m->max_entries - 1)
            return -LE_NOENT;
        else
            i = k + 1;
    }
    memcpy(next, &i, 4);
    return 0;
}

/* ---------- the bpffs: a tree of directories and pinned objects ---------- */

typedef struct {
    char *path;     /* canonical guest path */
    int is_dir;
    uint32_t mode, uid, gid;
    bpf_map_t *map; /* objects: the pinned map (one reference) */
} bpffs_node;

static bpffs_node *nodes;
static size_t nnodes, capnodes;

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static int node_find(const char *g)
{
    bpffs_init(); /* the root exists from the first use, whether or not init ran (a plain engine run has no init) */
    for (size_t i = 0; i < nnodes; i++) {
        if (!strcmp(nodes[i].path, g))
            return (int)i;
    }
    return -1;
}

static int node_add(const char *g, int is_dir, uint32_t mode, uint32_t uid, uint32_t gid, bpf_map_t *m)
{
    if (nnodes == capnodes) {
        size_t cap = capnodes ? capnodes * 2 : 16;
        bpffs_node *n = realloc(nodes, cap * sizeof *nodes);
        if (!n)
            return -LE_NOMEM;
        nodes = n;
        capnodes = cap;
    }
    char *p = dup_str(g);
    if (!p)
        return -LE_NOMEM;
    bpffs_node *nd = &nodes[nnodes++];
    nd->path = p;
    nd->is_dir = is_dir;
    nd->mode = mode;
    nd->uid = uid;
    nd->gid = gid;
    nd->map = m;
    return 0;
}

static void node_remove(int idx)
{
    bpffs_node *nd = &nodes[idx];
    if (nd->map)
        bpf_map_unref(nd->map);
    free(nd->path);
    nodes[idx] = nodes[--nnodes];
}

void bpffs_init(void)
{
    if (nnodes == 0)
        node_add(BPF_FS_ROOT, 1, 01777u, 0, 0, NULL);
}

int bpffs_under(const char *guest)
{
    size_t n = strlen(BPF_FS_ROOT);
    return !strncmp(guest, BPF_FS_ROOT, n) && (guest[n] == 0 || guest[n] == '/');
}

bpffs_kind bpffs_lookup(const char *guest)
{
    int i = node_find(guest);
    if (i < 0)
        return BPFFS_NONE;
    return nodes[i].is_dir ? BPFFS_DIR : BPFFS_OBJ;
}

int bpffs_stat(const char *guest, uint32_t *mode, uint32_t *uid, uint32_t *gid)
{
    int i = node_find(guest);
    if (i < 0)
        return -LE_NOENT;
    *mode = nodes[i].is_dir ? 0040000u | nodes[i].mode : nodes[i].mode; /* S_IFDIR or the S_IFREG of a pin */
    *uid = nodes[i].uid;
    *gid = nodes[i].gid;
    return 0;
}

/* The parent of a canonical guest path, into out (cap bytes). */
static int parent_of(const char *g, char *out, size_t cap)
{
    const char *slash = strrchr(g, '/');
    if (!slash || slash == g)
        return -1;
    size_t n = (size_t)(slash - g);
    if (n + 1 > cap)
        return -1;
    memcpy(out, g, n);
    out[n] = 0;
    return 0;
}

int bpffs_mkdir(const char *guest, uint32_t mode)
{
    if (node_find(guest) >= 0)
        return -LE_EXIST;
    char par[VFS_PATH_MAX];
    if (parent_of(guest, par, sizeof par) < 0)
        return -LE_NOENT;
    int pi = node_find(par);
    if (pi < 0)
        return -LE_NOENT;
    if (!nodes[pi].is_dir)
        return -LE_NOTDIR;
    proc_t *p = proc_cur();
    int rc = node_add(guest, 1, mode & 07777u, p->cred.fsuid, p->cred.fsgid, NULL);
    return rc;
}

int bpffs_unlink(const char *guest, int rmdir)
{
    if (!strcmp(guest, BPF_FS_ROOT))
        return -LE_BUSY; /* the root of the bpffs is a mount point */
    int i = node_find(guest);
    if (i < 0)
        return -LE_NOENT;
    if (nodes[i].is_dir && !rmdir)
        return -LE_ISDIR;
    if (!nodes[i].is_dir && rmdir)
        return -LE_NOTDIR;
    if (rmdir) {
        size_t n = strlen(guest);
        for (size_t k = 0; k < nnodes; k++) {
            if (!strncmp(nodes[k].path, guest, n) && nodes[k].path[n] == '/')
                return -LE_NOTEMPTY;
        }
    }
    node_remove(i);
    return 0;
}

/* Adds a pinned object (regular file, the caller's umask applied to 0600) at guest. The node holds its own
 * reference to m. Returns 0 or -LE_*. */
static int64_t pin_add(const char *guest, bpf_map_t *m)
{
    proc_t *p = proc_cur();
    uint32_t mode = 0100000u | (0600u & ~p->umask);
    int rc = node_add(guest, 0, mode, p->cred.fsuid, p->cred.fsgid, m);
    if (rc)
        return rc;
    map_ref(m);
    return 0;
}

/* Linux inode_permission for the access bits (4 read, 2 write) on a pinned object: the owner, group or other
 * bits; CAP_DAC_OVERRIDE bypasses them. */
static int64_t obj_access(const bpffs_node *nd, uint32_t want)
{
    proc_t *p = proc_cur();
    if (cred_has_cap(p, CAP_DAC_OVERRIDE_BIT))
        return 0;
    uint32_t bits = nd->mode & 7u; /* other */
    if (p->cred.fsuid == nd->uid) {
        bits = (nd->mode >> 6) & 7u;
    } else {
        int in = p->cred.fsgid == nd->gid;
        for (int g = 0; !in && g < p->cred.ngroups; g++)
            in = p->cred.groups[g] == nd->gid;
        if (in)
            bits = (nd->mode >> 3) & 7u;
    }
    return (bits & want) == want ? 0 : -LE_ACCES;
}

/* ---------- the commands ---------- */
static int debug_on(void);


static int64_t sys_map_create(uint64_t attr, uint64_t size)
{
    uint8_t a[BPF_ATTR_MAX];
    int64_t rc = attr_load(attr, size, a, 80);
    if (rc)
        return rc;
    uint32_t type = rd32(a + 0), key = rd32(a + 4), value = rd32(a + 8), max = rd32(a + 12), flags = rd32(a + 16);
    if (rd32(a + 20) != 0)  /* inner_map_fd: maps of maps are not built */
        return -LE_INVAL;
    if (rd64(a + 64) != 0)  /* map_extra */
        return -LE_INVAL;
    char name[BPF_OBJ_NAME_LEN];
    memcpy(name, a + 28, BPF_OBJ_NAME_LEN);
    name[BPF_OBJ_NAME_LEN - 1] = 0;
    int64_t err;
    bpf_map_t *m = map_new(type, key, value, max, flags, name, &err);
    if (!m)
        return err;
    /* vfs_obj_new frees the map when the fd table is full. */
    return vfs_obj_new(VFS_BPF, m, 1, LO_RDWR);
}

/* The element commands: the attribute is map_fd, key (u64), value or next_key (u64), flags (u64). */
static int64_t sys_elem(int cmd, uint64_t attr, uint64_t size)
{
    uint8_t a[BPF_ATTR_MAX];
    int64_t rc = attr_load(attr, size, a, 32);
    if (rc)
        return rc;
    int fd = (int)rd32(a + 0);
    uint64_t kva = rd64(a + 8), vva = rd64(a + 16), flags = rd64(a + 24);
    if (cmd == BPFC_MAP_LOOKUP_ELEM && (flags & ~(uint64_t)BPF_LOCK_FLAG))
        return -LE_INVAL;
    if ((cmd == BPFC_MAP_DELETE_ELEM || cmd == BPFC_MAP_GET_NEXT_KEY) && flags)
        return -LE_INVAL;
    uint32_t want = cmd == BPFC_MAP_LOOKUP_ELEM || cmd == BPFC_MAP_GET_NEXT_KEY ? ACC_READ : ACC_WRITE;
    bpf_map_t *m = fd_map(fd, want, &rc);
    if (!m)
        return rc;
    /* A key of key_size bytes (get_next_key takes NULL for the first element). The next key and the value are
     * the caller's buffers. Each copy is a fault (EFAULT) when the address is bad. */
    uint8_t key[256];
    uint8_t *k = key;
    if (m->key_size > sizeof key) {
        k = malloc(m->key_size);
        if (!k)
            return -LE_NOMEM;
    }
    int64_t out = 0;
    int have_key = kva != 0;
    if (have_key && mem_copy_out(k, kva, m->key_size) < 0) {
        out = -LE_FAULT;
    } else if (!have_key && cmd != BPFC_MAP_GET_NEXT_KEY) {
        out = -LE_FAULT;
    } else if (cmd == BPFC_MAP_LOOKUP_ELEM) {
        uint8_t *v = map_lookup(m, k);
        if (!v)
            out = -LE_NOENT;
        else if (mem_copy_in(vva, v, m->vsz) < 0)
            out = -LE_FAULT;
    } else if (cmd == BPFC_MAP_UPDATE_ELEM) {
        uint8_t val[256];
        uint8_t *v = val;
        if (m->value_size > sizeof val) {
            v = malloc(m->value_size);
            if (!v) {
                out = -LE_NOMEM;
                goto done;
            }
        }
        if (mem_copy_out(v, vva, m->value_size) < 0)
            out = -LE_FAULT;
        else
            out = map_update(m, k, v, flags);
        if (v != val)
            free(v);
    } else if (cmd == BPFC_MAP_DELETE_ELEM) {
        out = map_delete(m, k);
    } else {
        uint8_t nk[256];
        uint8_t *n = nk;
        if (m->key_size > sizeof nk) {
            n = malloc(m->key_size);
            if (!n) {
                out = -LE_NOMEM;
                goto done;
            }
        }
        out = map_next_key(m, have_key ? k : NULL, n);
        if (!out && mem_copy_in(vva, n, m->key_size) < 0)
            out = -LE_FAULT;
        if (n != nk)
            free(n);
    }
done:
    if (k != key)
        free(k);
    return out;
}

static int64_t sys_obj_pin(uint64_t attr, uint64_t size)
{
    uint8_t a[BPF_ATTR_MAX];
    int64_t rc = attr_load(attr, size, a, 20);
    if (rc)
        return rc;
    uint64_t pva = rd64(a + 0);
    uint32_t bpf_fd = rd32(a + 8), flags = rd32(a + 12);
    int32_t path_fd = (int32_t)rd32(a + 16);
    if (flags & ~BPFF_PATH_FD)
        return -LE_INVAL;
    if (!(flags & BPFF_PATH_FD) && path_fd)
        return -LE_INVAL;
    vfs_fd *f = vfs_get((int)bpf_fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_BPF)
        return -LE_INVAL;
    char path[VFS_PATH_MAX];
    rc = path_load(pva, path, sizeof path);
    if (rc)
        return rc;
    vfs_res r;
    rc = vfs_resolve((flags & BPFF_PATH_FD) ? path_fd : LAT_FDCWD, path, 0, &r);
    if (rc)
        return rc;
    if (!bpffs_under(r.guest))
        return -LE_PERM; /* user_path_create: the parent is not a bpf directory */
    if (r.exists || r.is_link)
        return -LE_EXIST;
    char par[VFS_PATH_MAX];
    if (parent_of(r.guest, par, sizeof par) < 0)
        return -LE_NOENT;
    int pi = node_find(par);
    if (pi < 0)
        return -LE_NOENT;
    if (!nodes[pi].is_dir)
        return -LE_NOTDIR;
    return pin_add(r.guest, f->obj);
}

static int64_t sys_obj_get(uint64_t attr, uint64_t size)
{
    uint8_t a[BPF_ATTR_MAX];
    int64_t rc = attr_load(attr, size, a, 20);
    if (rc)
        return rc;
    uint64_t pva = rd64(a + 0);
    uint32_t bpf_fd = rd32(a + 8), flags = rd32(a + 12);
    int32_t path_fd = (int32_t)rd32(a + 16);
    if (bpf_fd != 0)
        return -LE_INVAL;
    if (flags & ~(BPFF_RDONLY | BPFF_WRONLY | BPFF_PATH_FD))
        return -LE_INVAL;
    if (!(flags & BPFF_PATH_FD) && path_fd)
        return -LE_INVAL;
    if ((flags & BPFF_RDONLY) && (flags & BPFF_WRONLY))
        return -LE_INVAL;
    char path[VFS_PATH_MAX];
    rc = path_load(pva, path, sizeof path);
    if (rc)
        return rc;
    if (debug_on())
        fprintf(stderr, "bpf: OBJ_GET %s file_flags %#x\n", path, flags);
    vfs_res r;
    rc = vfs_resolve((flags & BPFF_PATH_FD) ? path_fd : LAT_FDCWD, path, 1, &r);
    if (rc)
        return rc;
    if (!r.exists)
        return -LE_NOENT;
    if (!bpffs_under(r.guest))
        return -LE_ACCES; /* a file that is not a bpf object: bpf_inode_type */
    int i = node_find(r.guest);
    if (i < 0)
        return -LE_NOENT;
    if (nodes[i].is_dir)
        return -LE_ACCES;
    uint32_t want = (flags & BPFF_RDONLY) ? ACC_READ : (flags & BPFF_WRONLY) ? ACC_WRITE : (ACC_READ | ACC_WRITE);
    rc = obj_access(&nodes[i], want);
    if (rc)
        return rc;
    bpf_map_t *m = nodes[i].map;
    if (!m)
        return -LE_ACCES;
    map_ref(m);
    uint32_t lf = (flags & BPFF_RDONLY) ? LO_RDONLY : (flags & BPFF_WRONLY) ? LO_WRONLY : LO_RDWR;
    return vfs_obj_new(VFS_BPF, m, 1, lf);
}

static int64_t sys_obj_info(uint64_t attr, uint64_t size)
{
    uint8_t a[BPF_ATTR_MAX];
    int64_t rc = attr_load(attr, size, a, 16);
    if (rc)
        return rc;
    int fd = (int)rd32(a + 0);
    uint32_t info_len = rd32(a + 4);
    uint64_t iva = rd64(a + 8);
    vfs_fd *f = vfs_get(fd);
    if (!f)
        return -LE_BADF;
    if (f->kind != VFS_BPF)
        return -LE_INVAL;
    bpf_map_t *m = f->obj;
    uint8_t info[BPF_MAP_INFO_SIZE];
    memset(info, 0, sizeof info);
    memcpy(info + 0, &m->type, 4);
    memcpy(info + 4, &m->id, 4);
    memcpy(info + 8, &m->key_size, 4);
    memcpy(info + 12, &m->value_size, 4);
    memcpy(info + 16, &m->max_entries, 4);
    memcpy(info + 20, &m->map_flags, 4);
    memcpy(info + 24, m->name, BPF_OBJ_NAME_LEN);
    uint32_t n = info_len < BPF_MAP_INFO_SIZE ? info_len : BPF_MAP_INFO_SIZE;
    if (n && mem_copy_in(iva, info, n) < 0)
        return -LE_FAULT;
    mem_write32(attr + 4, n);
    return 0;
}

/* XIAOJI_DEBUG_BPF=1: one stderr line per failing command, and one per BPF_OBJ_GET with its path and file_flags
 * (what a caller asks for: the flags decide the access check). */
static int debug_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("XIAOJI_DEBUG_BPF") != NULL;
    return on;
}

static int64_t bpf_dispatch(uint64_t cmd, uint64_t attr, uint64_t size)
{
    int c = (int)cmd;
    switch (c) {
    case BPFC_MAP_CREATE: return sys_map_create(attr, size);
    case BPFC_MAP_LOOKUP_ELEM:
    case BPFC_MAP_UPDATE_ELEM:
    case BPFC_MAP_DELETE_ELEM:
    case BPFC_MAP_GET_NEXT_KEY: return sys_elem(c, attr, size);
    case BPFC_OBJ_PIN: return sys_obj_pin(attr, size);
    case BPFC_OBJ_GET: return sys_obj_get(attr, size);
    case BPFC_OBJ_GET_INFO_BY_FD: return sys_obj_info(attr, size);
    default: break;
    }
    if (c < 0 || c > BPFC_LAST_KNOWN)
        return -LE_INVAL;
    return -LE_PERM; /* a known command this subset does not build: programs, links, attach, tokens */
}

int64_t bpf_syscall(uint64_t cmd, uint64_t attr, uint64_t size)
{
    int64_t r = bpf_dispatch(cmd, attr, size);
    if (debug_on() && r < 0)
        fprintf(stderr, "bpf: cmd %d errno %lld\n", (int)cmd, (long long)-r);
    return r;
}

/* ---------- bpfloader's pre-creation (B15) ----------
 *
 * The loader (system/bpf/bpfloader, libbpf_android/Loader.cpp, 0.19) creates the maps of each BPF object in its
 * directories and pins them, and loads the programs. This engine does the first half only: no program is loaded,
 * so bpf.progs_loaded is not set. The maps are created empty; a reader gets "no data", which is what it gets from
 * a loaded program with nothing to count. For each object (its ELF "maps" section, struct bpf_map_def, 116 bytes):
 *   - the loader's version gates (bpfloader_min_ver, bpfloader_max_ver, min_kver, max_kver) decide whether it is made;
 *   - the map is pinned at /sys/fs/bpf/<prefix>map_<file>_<map> (<file> empty when shared), owned by uid:gid and
 *     mode as the def says (the loader's chown and chmod), in the subdirectory <prefix> made first;
 *   - a map whose pin exists is reused, as the loader reuses it;
 *   - a def with a pin_subdir (domain) is not built here: the image's objects have none.
 * Failures are logged to stderr and skipped (the loader would reboot the device; a boot here goes on). */

/* The loader's directories and pin prefixes (BpfLoader.cpp, 0.19). Only the objects directly in each one. */
static const struct {
    const char *dir;
    const char *prefix;
} bpf_dirs[] = {
    {"/apex/com.android.tethering/etc/bpf/", "tethering/"},
    {"/apex/com.android.tethering/etc/bpf/netd_shared/", "netd_shared/"},
    {"/apex/com.android.tethering/etc/bpf/netd_readonly/", "netd_readonly/"},
    {"/apex/com.android.tethering/etc/bpf/net_shared/", "net_shared/"},
    {"/apex/com.android.tethering/etc/bpf/net_private/", "net_private/"},
    {"/system/etc/bpf/", ""},
    {"/vendor/etc/bpf/", "vendor/"},
};

#define BPFLOADER_VERSION 0x13u /* (0 << 16) | 19, Loader.cpp */
#define BPF_KVERS 0x060100u     /* KVER(6, 1, 0): the release uname reports (syscall.c) */
#define BPF_DEF_SIZE 116u       /* sizeof(struct bpf_map_def), bpf_map_def.h */
#define BPF_DEF_DEFAULT_MAX_VER 0x10000u
#define BPF_DEF_DEFAULT_MAX_KVER 0xffffffffu

static int in_range(uint64_t off, uint64_t len, size_t n)
{
    return off <= n && len <= n - off;
}

static uint16_t rd16(const uint8_t *p)
{
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static int preload_pinned; /* maps pinned by bpf_preload (the summary line counts them) */

/* Creates and pins one map of an object. The pin path is built as Loader.cpp builds it. */
static void preload_map(const char *prefix, const char *fname, const char *mapname, const uint8_t *def, size_t dsz)
{
    uint8_t f[BPF_DEF_SIZE];
    memset(f, 0, sizeof f);
    memcpy(f, def, dsz < BPF_DEF_SIZE ? dsz : BPF_DEF_SIZE);
    uint32_t minv = 0, maxv = BPF_DEF_DEFAULT_MAX_VER, mink = 0, maxk = BPF_DEF_DEFAULT_MAX_KVER;
    if (dsz >= 36)
        minv = rd32(f + 32), maxv = rd32(f + 36);
    if (dsz >= 44)
        mink = rd32(f + 40), maxk = rd32(f + 44);
    if (BPFLOADER_VERSION < minv || BPFLOADER_VERSION >= maxv || BPF_KVERS < mink || BPF_KVERS >= maxk)
        return; /* skipped by the version gates, as the loader logs and skips it */
    if (dsz >= 112 && f[80] != 0) {
        fprintf(stderr, "bpfloader: %s map %s: pin_subdir is not built\n", fname, mapname);
        return;
    }
    uint32_t type = rd32(f + 0), key = rd32(f + 4), value = rd32(f + 8), max = rd32(f + 12), flags = rd32(f + 16);
    uint32_t uid = rd32(f + 20), gid = rd32(f + 24), mode = rd32(f + 28);
    int shared = dsz >= 113 && f[112] != 0;
    char pin[VFS_PATH_MAX];
    snprintf(pin, sizeof pin, "%s%smap_%s_%s", BPF_FS_ROOT "/", prefix, shared ? "" : fname, mapname);
    if (node_find(pin) >= 0)
        return; /* reused, as the loader reuses a pinned map */
    if (prefix[0]) {
        char dir[VFS_PATH_MAX];
        snprintf(dir, sizeof dir, "%s/%.*s", BPF_FS_ROOT, (int)strlen(prefix) - 1, prefix);
        if (node_find(dir) < 0 && node_add(dir, 1, 01777u, 0, 0, NULL) < 0)
            return;
    }
    int64_t err;
    bpf_map_t *m = map_new(type, key, value, max, flags, mapname, &err);
    if (!m) {
        fprintf(stderr, "bpfloader: %s map %s: error %lld\n", fname, mapname, (long long)err);
        return;
    }
    if (node_add(pin, 0, 0100000u | (mode & 07777u), uid, gid, m) < 0) {
        bpf_map_unref(m);
        return;
    }
    preload_pinned++;
    if (debug_on())
        fprintf(stderr, "bpfloader: pinned %s (type %u, key %u, value %u, max %u)\n", pin, type, key, value, max);
}

/* The data of the ELF section called name, or NULL. Reads the header table of an ELF64 LE object (d, n). */
static const uint8_t *find_section(const uint8_t *d, size_t n, const char *name, uint64_t *size)
{
    uint64_t shoff = rd64(d + 0x28);
    uint16_t shentsize = rd16(d + 0x3a), shnum = rd16(d + 0x3c), shstrndx = rd16(d + 0x3e);
    if (shentsize < 64 || shnum == 0 || shstrndx >= shnum || !in_range(shoff, (uint64_t)shentsize * shnum, n))
        return NULL;
    const uint8_t *shstr_h = d + shoff + (uint64_t)shstrndx * shentsize;
    uint64_t shstr_off = rd64(shstr_h + 0x18), shstr_size = rd64(shstr_h + 0x20);
    if (!in_range(shstr_off, shstr_size, n))
        return NULL;
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *h = d + shoff + (uint64_t)i * shentsize;
        uint32_t no = rd32(h);
        if (no >= shstr_size || strcmp((const char *)d + shstr_off + no, name))
            continue;
        uint64_t off = rd64(h + 0x18);
        *size = rd64(h + 0x20);
        if (rd32(h + 4) == 8) /* SHT_NOBITS: no data */
            return NULL;
        return in_range(off, *size, n) ? d + off : NULL;
    }
    return NULL;
}

/* A u32 section (the loader's readSectionUint: the first four bytes, little endian), or def when absent. */
static uint32_t section_u32(const uint8_t *d, size_t n, const char *name, uint32_t def)
{
    uint64_t sz = 0;
    const uint8_t *p = find_section(d, n, name, &sz);
    return p && sz >= 4 ? rd32(p) : def;
}

/* Reads the maps of one object: its "maps" section and the symbols that name the entries. The object's
 * bpfloader_min_ver and bpfloader_max_ver gate the whole object (Loader.cpp, defaults 0 and 0x10000);
 * size_of_bpf_map_def (default 32) is how much of each def is read, as the loader trims it. */
static void preload_object(const char *prefix, const char *fname, const uint8_t *d, size_t n)
{
    if (n < 64 || memcmp(d, "\177ELF", 4) != 0 || d[4] != 2 || d[5] != 1)
        return;
    uint32_t minv = section_u32(d, n, "bpfloader_min_ver", 0), maxv = section_u32(d, n, "bpfloader_max_ver", 0x10000u);
    if (BPFLOADER_VERSION < minv || BPFLOADER_VERSION >= maxv) {
        fprintf(stderr, "bpfloader: %s skipped (bpfloader version gate %#x..%#x)\n", fname, minv, maxv);
        return;
    }
    uint32_t def_size = section_u32(d, n, "size_of_bpf_map_def", 32u);
    if (def_size > BPF_DEF_SIZE)
        def_size = BPF_DEF_SIZE;
    uint64_t maps_size = 0;
    const uint8_t *maps = find_section(d, n, "maps", &maps_size);
    if (!maps)
        return;
    uint64_t shoff = rd64(d + 0x28);
    uint16_t shentsize = rd16(d + 0x3a), shnum = rd16(d + 0x3c);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = d + shoff + (uint64_t)i * shentsize;
        if (rd32(sh + 4) != 2) /* SHT_SYMTAB */
            continue;
        uint64_t sym_off = rd64(sh + 0x18), sym_size = rd64(sh + 0x20);
        uint32_t link = rd32(sh + 0x28);
        if (link >= shnum || !in_range(sym_off, sym_size, n))
            return;
        const uint8_t *strh = d + shoff + (uint64_t)link * shentsize;
        uint64_t str_off = rd64(strh + 0x18), str_size = rd64(strh + 0x20);
        if (!in_range(str_off, str_size, n))
            return;
        /* The index of the "maps" section, to match the symbols against it. */
        uint16_t maps_idx = 0xffffu;
        uint64_t shstr_off = rd64(d + shoff + (uint64_t)rd16(d + 0x3e) * shentsize + 0x18);
        uint64_t shstr_size = rd64(d + shoff + (uint64_t)rd16(d + 0x3e) * shentsize + 0x20);
        for (uint16_t k = 0; k < shnum && maps_idx == 0xffffu; k++) {
            const uint8_t *hk = d + shoff + (uint64_t)k * shentsize;
            uint32_t no = rd32(hk);
            if (no < shstr_size && !strcmp((const char *)d + shstr_off + no, "maps"))
                maps_idx = k;
        }
        for (uint64_t off = 0; off + 24 <= sym_size; off += 24) {
            const uint8_t *sym = d + sym_off + off;
            if (rd16(sym + 6) != maps_idx)
                continue;
            uint32_t no = rd32(sym);
            uint64_t val = rd64(sym + 8), sz = rd64(sym + 16);
            if (no == 0 || no >= str_size || sz == 0 || val > maps_size || def_size > maps_size - val)
                continue;
            const char *mapname = (const char *)d + str_off + no;
            if (!mapname[0])
                continue;
            preload_map(prefix, fname, mapname, maps + val, def_size);
        }
        return;
    }
}

/* Loads every *.o directly in dir (the loader's directory scan). */
static void preload_dir(const char *dir, const char *prefix)
{
    int dfd = vfs_open(LAT_FDCWD, dir, LO_RDONLY | LO_DIRECTORY, 0);
    if (dfd < 0)
        return;
    size_t cap = 8192, len = 0;
    uint8_t *ents = malloc(cap);
    if (!ents) {
        vfs_close(dfd);
        return;
    }
    for (;;) {
        if (cap - len < 4096) {
            uint8_t *n2 = realloc(ents, cap * 2);
            if (!n2)
                break;
            ents = n2;
            cap *= 2;
        }
        int64_t got = vfs_getdents(dfd, ents + len, cap - len);
        if (got <= 0)
            break;
        len += (size_t)got;
    }
    vfs_close(dfd);
    for (size_t off = 0; off + 19 <= len;) {
        uint16_t reclen = rd16(ents + off + 16);
        if (reclen < 20 || off + reclen > len)
            break;
        const char *name = (const char *)ents + off + 19;
        size_t nl = strlen(name);
        if (nl > 2 && !strcmp(name + nl - 2, ".o")) {
            char path[VFS_PATH_MAX];
            snprintf(path, sizeof path, "%s%s", dir, name);
            int fd = vfs_open(LAT_FDCWD, path, LO_RDONLY, 0);
            if (fd >= 0) {
                size_t bcap = 65536, blen = 0;
                uint8_t *obj = malloc(bcap);
                while (obj) {
                    if (bcap - blen < 4096) {
                        uint8_t *n2 = realloc(obj, bcap * 2);
                        if (!n2) {
                            free(obj);
                            obj = NULL;
                            break;
                        }
                        obj = n2;
                        bcap *= 2;
                    }
                    int64_t r = vfs_read(fd, obj + blen, bcap - blen);
                    if (r <= 0)
                        break;
                    blen += (size_t)r;
                }
                vfs_close(fd);
                if (obj) {
                    char base[VFS_PATH_MAX];
                    snprintf(base, sizeof base, "%.*s", (int)(nl - 2), name);
                    preload_object(prefix, base, obj, blen);
                    free(obj);
                }
            }
        }
        off += reclen;
    }
    free(ents);
}

void bpf_preload(void)
{
    bpffs_init();
    for (size_t i = 0; i < sizeof bpf_dirs / sizeof bpf_dirs[0]; i++)
        preload_dir(bpf_dirs[i].dir, bpf_dirs[i].prefix);
    fprintf(stderr, "bpfloader: %d maps pinned under /sys/fs/bpf\n", preload_pinned);
}
