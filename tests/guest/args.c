/* Prints argc and argv, then the AT_PAGESZ and AT_HWCAP values from auxv.
 * AT_HWCAP is masked to its low two bits (FP|ASIMD): the native kernel reports
 * more feature bits than the engine does. */
#include "sys.h"

#define AT_PAGESZ 6
#define AT_HWCAP 16

static u64 find_aux(u64 type, int *found)
{
    char **e = xiaoji_envp;
    while (*e)
        e++;
    u64 *aux = (u64 *)(e + 1);
    for (; aux[0] != 0; aux += 2) {
        if (aux[0] == type) {
            *found = 1;
            return aux[1];
        }
    }
    *found = 0;
    return 0;
}

int main(int argc, char **argv)
{
    put("argc=");
    putu((u64)argc);
    put("\n");
    for (int i = 0; i < argc; i++) {
        put("argv[");
        putu((u64)i);
        put("]=");
        put(argv[i]);
        put("\n");
    }
    int found;
    u64 v = find_aux(AT_PAGESZ, &found);
    put("AT_PAGESZ=");
    if (found)
        putu(v);
    else
        put("missing");
    put("\n");
    v = find_aux(AT_HWCAP, &found);
    put("AT_HWCAP&3=");
    if (found)
        putu(v & 3);
    else
        put("missing");
    put("\n");
    return argc == 3 ? 0 : 1;
}
