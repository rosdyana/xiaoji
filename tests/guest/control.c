/* Control flow: recursion, indirect calls, a dense switch (jump table), loops
 * with early exits, a 1000-deep call chain, and a sort of pseudo-random data. */
#include "sys.h"

#define N 1000

static volatile u32 arr[N];

static u64 __attribute__((noinline)) fib(u64 n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

static u64 __attribute__((noinline)) op_add(u64 x) { return x + 0x1111; }
static u64 __attribute__((noinline)) op_mul(u64 x) { return x * 3 + 1; }
static u64 __attribute__((noinline)) op_xor(u64 x) { return x ^ 0xA5A5A5A5ull; }
static u64 __attribute__((noinline)) op_rot(u64 x) { return (x >> 7) | (x << 57); }

static u64 (*const table[4])(u64) = {op_add, op_mul, op_xor, op_rot};

static u64 __attribute__((noinline)) dense(u32 op, u64 x)
{
    switch (op) {
    case 0: return x + 1;
    case 1: return x * 5;
    case 2: return x ^ 0x55;
    case 3: return x - 7;
    case 4: return x << 2;
    case 5: return x >> 3;
    case 6: return x | 0x100;
    case 7: return x & ~0xFFull;
    case 8: return x * 9 + 2;
    case 9: return x ^ (x >> 5);
    case 10: return x + 0x1000;
    case 11: return ~x;
    default: return 0;
    }
}

static u64 __attribute__((noinline)) deep(u64 n)
{
    if (n == 0)
        return 0;
    return deep(n - 1) + n;
}

static u32 xorshift(u32 *s)
{
    u32 x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    u64 f = fib(25);
    put("fib(25)=");
    putu(f);
    put("\n");

    u64 t = 0x0123456789ull;
    for (int i = 0; i < 4; i++) {
        t = table[i](t);
    }
    put("table=");
    putx(t);
    put("\n");

    u64 d = 1;
    for (u32 k = 0; k < 24; k++)
        d = dense(k % 12, d) + k;
    put("dense=");
    putx(d);
    put("\n");

    /* Loop with early exit: first index with a square above 5000. */
    u32 first = 0;
    for (volatile u32 i = 0; i < 100000; i++) { /* volatile: no vectorised search */
        if (i * i > 5000) {
            first = i;
            break;
        }
    }
    put("early_exit=");
    putu(first);
    put("\n");

    put("deep(1000)=");
    putu(deep(1000));
    put("\n");

    u32 seed = 0x2545F491u;
    for (u32 i = 0; i < N; i++)
        arr[i] = xorshift(&seed) % 100000;
    /* Insertion sort. */
    for (u32 i = 1; i < N; i++) {
        u32 v = arr[i];
        u32 j = i;
        while (j > 0 && arr[j - 1] > v) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = v;
    }
    u64 sum = 0;
    for (u32 i = 0; i < N; i++)
        sum = sum * 31 + arr[i];
    put("sorted_first=");
    putu(arr[0]);
    put(" last=");
    putu(arr[N - 1]);
    put("\n");
    put("checksum=");
    putx(sum);
    put("\n");
    return (int)(sum & 0x3F);
}
