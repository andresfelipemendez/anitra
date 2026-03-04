/*
 * rmt_tcc_compat.c — Interlocked intrinsics for TCC on Windows x86-64
 *
 * TCC declares _InterlockedCompareExchange etc. in its headers (via macro
 * expansion from winnt.h/winbase.h) but does not provide implementations.
 * MSVC supplies them as compiler intrinsics; we implement them here using
 * x86 inline assembly so TCC can link Remotery.
 *
 * Compile with: tcc -c rmt_tcc_compat.c
 * Or include directly in the TCC command line alongside Remotery.c
 */

#ifdef __TINYC__

/* Avoid pulling in the full Windows headers — just use the raw types. */

long _InterlockedCompareExchange(long volatile *dest, long xchg, long cmp) {
    long prev;
    __asm__ volatile("lock cmpxchgl %2, %1"
        : "=a"(prev), "+m"(*dest)
        : "r"(xchg), "0"(cmp)
        : "memory");
    return prev;
}

long long _InterlockedCompareExchange64(long long volatile *dest,
                                        long long xchg, long long cmp) {
    long long prev;
    __asm__ volatile("lock cmpxchgq %2, %1"
        : "=a"(prev), "+m"(*dest)
        : "r"(xchg), "0"(cmp)
        : "memory");
    return prev;
}

long _InterlockedExchangeAdd(long volatile *dest, long add) {
    long prev;
    __asm__ volatile("lock xaddl %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(add)
        : "memory");
    return prev;
}

long long _InterlockedExchangeAdd64(long long volatile *dest, long long add) {
    long long prev;
    __asm__ volatile("lock xaddq %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(add)
        : "memory");
    return prev;
}

long _InterlockedExchange(long volatile *dest, long val) {
    long prev;
    __asm__ volatile("xchgl %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(val)
        : "memory");
    return prev;
}

long long _InterlockedExchange64(long long volatile *dest, long long val) {
    long long prev;
    __asm__ volatile("xchgq %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(val)
        : "memory");
    return prev;
}

void *_InterlockedCompareExchangePointer(void *volatile *dest,
                                         void *xchg, void *cmp) {
    void *prev;
    __asm__ volatile("lock cmpxchgq %2, %1"
        : "=a"(prev), "+m"(*dest)
        : "r"(xchg), "0"(cmp)
        : "memory");
    return prev;
}

long _InterlockedIncrement(long volatile *dest) {
    long one = 1;
    long prev;
    __asm__ volatile("lock xaddl %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(one)
        : "memory");
    return prev + 1;
}

long _InterlockedDecrement(long volatile *dest) {
    long neg_one = -1;
    long prev;
    __asm__ volatile("lock xaddl %0, %1"
        : "=r"(prev), "+m"(*dest)
        : "0"(neg_one)
        : "memory");
    return prev - 1;
}

#endif /* __TINYC__ */
