/* Stress test for the IFA reordering rules.  Every phase folds its results
 * into a checksum that is printed, so any illegal reordering (register
 * hazard, memory order, barrier, FP flags, control flow) changes the output.
 * Nothing here depends on timing (no rdcycle/rdtime). */
#include <fenv.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 512

static uint64_t sum;
static volatile uint64_t vmem[N];
static uint64_t arr[N];
static double darr[N];
static jmp_buf jb;

static void
mix(uint64_t v)
{
    sum = (sum ^ v) * 0x100000001b3ULL;
}

/* Independent chains interleaved with a load-use dependency: the case the
 * DAU is meant to reorder. */
static uint64_t __attribute__((noinline))
chains(const uint64_t *p, int n)
{
    uint64_t a = 1, b = 2, c = 3, d = 4;
    for (int i = 0; i < n; i++) {
        uint64_t x = p[i];
        a = a * 3 + x;
        b ^= b << 7;
        c += c >> 3;
        d = d * 5 + (x >> 2);
    }
    return a ^ b ^ c ^ d;
}

/* Store/load through aliasing pointers (memory order). */
static uint64_t __attribute__((noinline))
alias(uint64_t *p, uint64_t *q, int n)
{
    uint64_t s = 0;
    for (int i = 0; i < n; i++) {
        p[i & 63] = i * 7;
        s += q[(i * 5) & 63];
        q[(i * 3) & 63] += s;
    }
    return s;
}

/* WAR / WAW on the same architectural registers. */
static uint64_t __attribute__((noinline))
war_waw(int n)
{
    uint64_t r = 0;
    for (int i = 0; i < n; i++) {
        uint64_t t = arr[i % N];
        uint64_t u = t + i;
        t = u * 3;
        u = t ^ r;
        r = u + arr[(i * 13) % N];
    }
    return r;
}

static uint64_t f1(uint64_t x) { return x * 31 + 7; }
static uint64_t f2(uint64_t x) { return (x >> 3) ^ x; }
static uint64_t f3(uint64_t x) { return x + (x << 5); }
static uint64_t (*const fns[3])(uint64_t) = { f1, f2, f3 };

static uint64_t __attribute__((noinline))
indirect(int n)
{
    uint64_t x = 5;
    for (int i = 0; i < n; i++)
        x = fns[(x ^ i) % 3](x) + i;
    return x;
}

static uint64_t
fib(int n)
{
    return n < 2 ? (uint64_t)n : fib(n - 1) + fib(n - 2);
}

static void __attribute__((noinline))
jumper(int depth)
{
    if (depth == 0)
        longjmp(jb, 42);
    mix(depth);
    jumper(depth - 1);
}

int
main(void)
{
    uint64_t q[64] = { 0 };

    for (int i = 0; i < N; i++) {
        arr[i] = (uint64_t)i * 2654435761u;
        darr[i] = i * 0.37 + 1.0;
    }

    /* 1. Integer chains, mul/div. */
    for (int k = 0; k < 20; k++)
        mix(chains(arr, N));
    for (int i = 1; i < 2000; i++)
        mix(arr[i % N] / (uint64_t)i + arr[(i * 7) % N] % (uint64_t)(i + 3));

    /* 2. Memory ordering with aliasing. */
    for (int k = 0; k < 20; k++)
        mix(alias(q, q, 300));
    for (int i = 0; i < N; i++) {
        vmem[i] = i;
        vmem[(i + 1) % N] = vmem[i] + 1;
        mix(vmem[(i + 1) % N]);
    }

    /* 3. Register WAR/WAW. */
    mix(war_waw(5000));

    /* 4. Atomics, LR/SC and fences. */
    uint64_t atom = 0;
    for (int i = 0; i < 1000; i++) {
        __atomic_fetch_add(&atom, i, __ATOMIC_SEQ_CST);
        uint64_t old = atom;
        __sync_bool_compare_and_swap(&atom, old, old ^ (uint64_t)i);
        __sync_synchronize();
        mix(atom);
    }

    /* 5. FP arithmetic and the sticky fflags CSR. */
    double acc = 0.0;
    feclearexcept(FE_ALL_EXCEPT);
    for (int i = 0; i < N; i++) {
        acc += darr[i] / (darr[(i * 3) % N] + 0.5);
        acc *= 0.999;
        if ((i & 63) == 0) {
            int ex = fetestexcept(FE_INEXACT | FE_DIVBYZERO | FE_OVERFLOW);
            mix((uint64_t)ex);
            feclearexcept(FE_ALL_EXCEPT);
        }
    }
    volatile double zero = 0.0;
    acc += 1.0 / zero > 1e308 ? 1.0 : 0.0;
    mix((uint64_t)fetestexcept(FE_DIVBYZERO));
    mix((uint64_t)(acc * 1000.0));

    /* 6. Indirect calls, recursion, setjmp/longjmp. */
    mix(indirect(5000));
    mix(fib(18));
    int r = setjmp(jb);
    if (r == 0)
        jumper(50);
    mix((uint64_t)r);

    /* 7. Syscalls interleaved with computation. */
    char buf[64];
    for (int i = 0; i < 8; i++) {
        int len = snprintf(buf, sizeof(buf), "phase7 %d %llu\n", i,
                           (unsigned long long)(sum % 100000));
        write(1, buf, len);
        mix(len);
    }
    void *m = malloc(4096);
    memset(m, 0x5a, 4096);
    mix(((unsigned char *)m)[1234]);
    free(m);

    printf("ifa_stress checksum=%016llx\n", (unsigned long long)sum);
    return 0;
}
