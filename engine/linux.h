/* Linux arm64 ABI constants used by the engine. Values were checked against the
 * builder VM's arm64 glibc headers (kernel uapi). Host macros are never used
 * for guest-visible numbers: host.c translates between the two. */
#ifndef XIAOJI_LINUX_H
#define XIAOJI_LINUX_H

/* errno values, returned to the guest as -LE_*. */
#define LE_PERM 1
#define LE_NOENT 2
#define LE_SRCH 3
#define LE_INTR 4
#define LE_IO 5
#define LE_NXIO 6
#define LE_2BIG 7
#define LE_NOEXEC 8
#define LE_BADF 9
#define LE_CHILD 10
#define LE_AGAIN 11
#define LE_NOMEM 12
#define LE_ACCES 13
#define LE_FAULT 14
#define LE_BUSY 16
#define LE_EXIST 17
#define LE_XDEV 18
#define LE_NODEV 19
#define LE_NOTDIR 20
#define LE_ISDIR 21
#define LE_INVAL 22
#define LE_NFILE 23
#define LE_MFILE 24
#define LE_NOTTY 25
#define LE_FBIG 27
#define LE_NOSPC 28
#define LE_SPIPE 29
#define LE_ROFS 30
#define LE_PIPE 32
#define LE_RANGE 34
#define LE_NAMETOOLONG 36
#define LE_NOSYS 38
#define LE_NOTEMPTY 39
#define LE_LOOP 40
#define LE_OVERFLOW 75
#define LE_NOTSUP 95
#define LE_NODATA 61
#define LE_TIMEDOUT 110
#define LE_NOTSOCK 88
#define LE_MSGSIZE 90
#define LE_PROTONOSUPPORT 93
#define LE_AFNOSUPPORT 97
#define LE_ISCONN 106
#define LE_NOTCONN 107
#define LE_CONNREFUSED 111
#define LE_DOM 33
#define LE_DESTADDRREQ 89
#define LE_NOPROTOOPT 92
#define LE_ADDRINUSE 98
#define LE_ADDRNOTAVAIL 99

/* open(2) flags as the guest passes them. */
#define LO_RDONLY 00
#define LO_WRONLY 01
#define LO_RDWR 02
#define LO_ACCMODE 03
#define LO_CREAT 0100
#define LO_EXCL 0200
#define LO_NOCTTY 0400
#define LO_TRUNC 01000
#define LO_APPEND 02000
#define LO_NONBLOCK 04000
#define LO_DSYNC 010000
#define LO_DIRECTORY 040000
#define LO_NOFOLLOW 0100000
#define LO_DIRECT 0200000
#define LO_LARGEFILE 0400000
#define LO_NOATIME 01000000
#define LO_CLOEXEC 02000000
#define LO_SYNC 04010000
#define LO_PATH 010000000

/* *at() directory fd and flags. */
#define LAT_FDCWD (-100)
#define LAT_SYMLINK_NOFOLLOW 0x100
#define LAT_REMOVEDIR 0x200
#define LAT_SYMLINK_FOLLOW 0x400
#define LAT_EACCESS 0x200
#define LAT_EMPTY_PATH 0x1000

/* fcntl(2) commands and fd flags. */
#define LF_DUPFD 0
#define LF_GETFD 1
#define LF_SETFD 2
#define LF_GETFL 3
#define LF_SETFL 4
#define LF_DUPFD_CLOEXEC 1030
#define LFD_CLOEXEC 1

/* st_mode file types. */
#define LS_IFMT 0170000
#define LS_IFSOCK 0140000
#define LS_IFLNK 0120000
#define LS_IFREG 0100000
#define LS_IFBLK 060000
#define LS_IFDIR 040000
#define LS_IFCHR 020000
#define LS_IFIFO 010000

/* getdents64 d_type values. */
#define LDT_UNKNOWN 0
#define LDT_FIFO 1
#define LDT_CHR 2
#define LDT_DIR 4
#define LDT_BLK 6
#define LDT_REG 8
#define LDT_LNK 10
#define LDT_SOCK 12

/* access(2) modes. */
#define LR_OK 4
#define LW_OK 2
#define LX_OK 1

/* mmap and mremap. */
#define LPROT_READ 1
#define LPROT_WRITE 2
#define LPROT_EXEC 4
#define LMAP_SHARED 0x01
#define LMAP_PRIVATE 0x02
#define LMAP_FIXED 0x10
#define LMAP_ANONYMOUS 0x20
#define LMREMAP_MAYMOVE 1
#define LMREMAP_FIXED 2
#define LMADV_DONTNEED 4

/* futex(2) ops, after masking off FUTEX_PRIVATE_FLAG and the clock flag. */
#define LFUTEX_WAIT 0
#define LFUTEX_WAKE 1
#define LFUTEX_REQUEUE 3
#define LFUTEX_CMP_REQUEUE 4
#define LFUTEX_WAIT_BITSET 9
#define LFUTEX_WAKE_BITSET 10
#define LFUTEX_OP_MASK 0x7F
#define LFUTEX_CLOCK_REALTIME 256

#define LCLOCK_REALTIME 0
#define LCLOCK_MONOTONIC 1
#define LTIMER_ABSTIME 1

/* Capability numbers (include/uapi/linux/capability.h). */
#define LCAP_CHOWN 0
#define LCAP_DAC_OVERRIDE 1
#define LCAP_DAC_READ_SEARCH 2
#define LCAP_FOWNER 3
#define LCAP_FSETID 4
#define LCAP_SETGID 6
#define LCAP_SETUID 7
#define LCAP_SETPCAP 8
#define LCAP_LINUX_IMMUTABLE 9
#define LCAP_SYS_ADMIN 21
#define LCAP_SYS_RESOURCE 24
#define LCAP_MKNOD 27
#define LCAP_MAC_OVERRIDE 32

/* prctl(2) options. */
#define LPR_SET_PDEATHSIG 1
#define LPR_GET_PDEATHSIG 2
#define LPR_GET_DUMPABLE 3
#define LPR_SET_DUMPABLE 4
#define LPR_GET_KEEPCAPS 7
#define LPR_SET_KEEPCAPS 8
#define LPR_SET_NAME 15
#define LPR_GET_NAME 16
#define LPR_CAPBSET_READ 23
#define LPR_CAPBSET_DROP 24
#define LPR_GET_SECUREBITS 27
#define LPR_CAP_AMBIENT 47
#define LPR_CAP_AMBIENT_IS_SET 1
#define LPR_CAP_AMBIENT_RAISE 2
#define LPR_CAP_AMBIENT_LOWER 3
#define LPR_CAP_AMBIENT_CLEAR_ALL 4
#define LPR_SET_SECUREBITS 28
#define LPR_SET_TIMERSLACK 29
#define LPR_GET_TIMERSLACK 30
#define LPR_SET_CHILD_SUBREAPER 36
#define LPR_GET_CHILD_SUBREAPER 37
#define LPR_SET_NO_NEW_PRIVS 38
#define LPR_GET_NO_NEW_PRIVS 39
#define LPR_SET_TAGGED_ADDR_CTRL 55
#define LPR_GET_TAGGED_ADDR_CTRL 56
#define LPR_SET_PTRACER 0x59616d61
#define LPR_SET_VMA 0x53564d41

#endif
