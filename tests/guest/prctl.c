/* prctl (B4b): the options zygote and system_server use, for a process that is not root (so
 * the native run, made as an ordinary user in the builder VM, gives the same answers). One
 * program, output compared with native. Nothing printed depends on a pid. */
#include <stdint.h>
#include "sys.h"

#define SYS_PRCTL 167

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_GET_DUMPABLE 3
#define PR_SET_DUMPABLE 4
#define PR_GET_KEEPCAPS 7
#define PR_SET_KEEPCAPS 8
#define PR_CAPBSET_READ 23
#define PR_GET_TIMERSLACK 30
#define PR_SET_CHILD_SUBREAPER 36
#define PR_GET_CHILD_SUBREAPER 37
#define PR_SET_NO_NEW_PRIVS 38
#define PR_GET_NO_NEW_PRIVS 39
#define PR_GET_TAGGED_ADDR_CTRL 56
#define PR_SET_PTRACER 0x59616d61

static void line(const char *name, i64 rc)
{
    put(name);
    put(" = ");
    putd(rc);
    put("\n");
}

/* The PR_GET options that write an int to their pointer argument. */
static i64 get_int(int option, const char *name)
{
    int32_t v = -1;
    i64 rc = sc6(SYS_PRCTL, option, (i64)&v, 0, 0, 0, 0);
    put(name);
    put(" = ");
    putd(rc);
    put(" value ");
    putd(v);
    put("\n");
    return rc;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    line("dumpable", sc6(SYS_PRCTL, PR_GET_DUMPABLE, 0, 0, 0, 0, 0));
    line("set dumpable 1", sc6(SYS_PRCTL, PR_SET_DUMPABLE, 1, 0, 0, 0, 0));
    line("set dumpable 2", sc6(SYS_PRCTL, PR_SET_DUMPABLE, 2, 0, 0, 0, 0));
    line("keepcaps", sc6(SYS_PRCTL, PR_GET_KEEPCAPS, 0, 0, 0, 0, 0));
    line("set keepcaps 1", sc6(SYS_PRCTL, PR_SET_KEEPCAPS, 1, 0, 0, 0, 0));
    line("keepcaps after", sc6(SYS_PRCTL, PR_GET_KEEPCAPS, 0, 0, 0, 0, 0));
    line("set keepcaps 2", sc6(SYS_PRCTL, PR_SET_KEEPCAPS, 2, 0, 0, 0, 0));
    line("capbset 0", sc6(SYS_PRCTL, PR_CAPBSET_READ, 0, 0, 0, 0, 0));
    line("capbset 40", sc6(SYS_PRCTL, PR_CAPBSET_READ, 40, 0, 0, 0, 0));
    line("capbset 41", sc6(SYS_PRCTL, PR_CAPBSET_READ, 41, 0, 0, 0, 0));
    line("no_new_privs before", sc6(SYS_PRCTL, PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0, 0));
    line("set no_new_privs", sc6(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0, 0));
    line("no_new_privs after", sc6(SYS_PRCTL, PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0, 0));
    line("set no_new_privs 0", sc6(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 0, 0, 0, 0, 0));
    get_int(PR_GET_PDEATHSIG, "pdeathsig");
    line("timerslack", sc6(SYS_PRCTL, PR_GET_TIMERSLACK, 0, 0, 0, 0, 0));
    get_int(PR_GET_CHILD_SUBREAPER, "subreaper before");
    line("set subreaper", sc6(SYS_PRCTL, PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0, 0));
    get_int(PR_GET_CHILD_SUBREAPER, "subreaper after");
    line("tagged addr", sc6(SYS_PRCTL, PR_GET_TAGGED_ADDR_CTRL, 0, 0, 0, 0, 0));
    line("ptracer (no Yama)", sc6(SYS_PRCTL, PR_SET_PTRACER, 0, 0, 0, 0, 0));
    return 0;
}
