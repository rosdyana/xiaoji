/* The deadlock check's hang case (B4d): one thread waits on a futex word with no timeout, and nothing
 * else in the guest can wake it. Under init, the engine must report the hang (tests/init/idle-hang.rc),
 * and this program never exits: the run ends at --init-timeout. */
#include <stdint.h>
#include "sys.h"

#define SYS_FUTEX 98
#define FUTEX_WAIT 0

static volatile uint32_t word = 0;

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    put("futex-hang: waiting forever\n");
    sc6(SYS_FUTEX, (i64)(uintptr_t)&word, FUTEX_WAIT, 0, 0, 0, 0);
    put("futex-hang: woke\n");
    return 0;
}
