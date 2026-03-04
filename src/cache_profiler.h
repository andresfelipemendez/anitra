/*
 * cache_profiler.h — Hardware cache miss profiler (header-only, stb-style)
 *
 * Usage:
 *   In exactly one .c file (externals_runtime.c):
 *     #define CACHE_PROF_IMPL
 *     #include "cache_profiler.h"
 *
 *   Everywhere else:
 *     #include "cache_profiler.h"   // struct typedefs + function declarations only
 *
 * macOS: Uses kpc framework via dlopen/dlsym for hardware performance counters.
 * Windows: Uses AMD Zen PMC via NtSystemDebugControl MSR access (admin required).
 *          Falls back to QueryPerformanceCounter if PMC init fails.
 * Other platforms: Stub implementations, cache_prof_available() returns 0.
 */

#ifndef CACHE_PROFILER_H
#define CACHE_PROFILER_H

#include <stdint.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define CACHE_PROF_MAX_ZONES  128
#define CACHE_PROF_MAX_STACK  32
#define CACHE_PROF_MAX_NAME_LEN 64

/* kpc constants */
#define KPC_CLASS_FIXED          (1u << 0)
#define KPC_CLASS_CONFIGURABLE   (1u << 1)
#define KPC_MAX_COUNTERS         16

/* ── Data Structures ────────────────────────────────────────────────────── */

typedef struct {
    const char *names[CACHE_PROF_MAX_ZONES];
    uint64_t    l1d_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    l2_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    l1i_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    branch_miss[CACHE_PROF_MAX_ZONES];
    uint64_t    instructions[CACHE_PROF_MAX_ZONES];
    uint64_t    cycles[CACHE_PROF_MAX_ZONES];
    uint16_t    count;
} cache_prof_frame;

typedef struct {
    uint64_t counters[KPC_MAX_COUNTERS];
} cache_prof_snapshot;

/* ── API Declarations ───────────────────────────────────────────────────── */

void              cache_prof_init(void);
void              cache_prof_shutdown(void);
void              cache_zone_begin(const char *name);
void              cache_zone_end(void);
void              cache_prof_frame_reset(void);
int               cache_prof_available(void);
int               cache_prof_is_hardware_backend(void);
const char       *cache_prof_status_message(void);
cache_prof_frame *cache_prof_get_frame(void);
void              cache_prof_sort_zones(void);

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef CACHE_PROF_IMPL

#include <stdio.h>

/* ── Platform detection ─────────────────────────────────────────────────── */

#if defined(__APPLE__)
#include <dlfcn.h>
#define CACHE_PROF_MACOS   1
#define CACHE_PROF_WINDOWS 0
#elif defined(_WIN32)
#include <windows.h>
#define CACHE_PROF_MACOS   0
#define CACHE_PROF_WINDOWS 1
#else
#define CACHE_PROF_MACOS   0
#define CACHE_PROF_WINDOWS 0
#endif

/* ── kpc function pointer types (macOS) ─────────────────────────────────── */

#if CACHE_PROF_MACOS

typedef int (*kpc_set_counting_fn)(uint32_t classes);
typedef int (*kpc_set_thread_counting_fn)(uint32_t classes);
typedef int (*kpc_set_config_fn)(uint32_t classes, uint64_t *config);
typedef int (*kpc_get_thread_counters_fn)(int tid, uint32_t count, uint64_t *counters);
typedef uint32_t (*kpc_get_counter_count_fn)(uint32_t classes);
typedef int (*kpc_force_all_ctrs_set_fn)(int val);

/* Apple M1 configurable PMC event IDs */
#define CACHE_PROF_EVT_L1D_MISS    0x04
#define CACHE_PROF_EVT_L1I_MISS    0xa6
#define CACHE_PROF_EVT_BRANCH_MISS 0xcb

#endif /* CACHE_PROF_MACOS */

/* ── AMD Zen PMC types and constants (Windows) ──────────────────────────── */

#if CACHE_PROF_WINDOWS

/* AMD performance monitoring counter MSR addresses.
   Zen 3 has 6 core PMCs: PERF_CTLn at base+2*n, PERF_CTRn at base+2*n+1. */
#define AMD_PERF_CTL_BASE  0xC0010200u
#define AMD_PERF_CTR_BASE  0xC0010201u
#define AMD_PMC_COUNT      5

/* PERF_CTL encoding: Event[7:0] | UnitMask[15:8] | USR[16] | OS[17] | EN[22] */
#define AMD_PERF_CTL_ENC(evt, umask) \
    ((uint64_t)(evt) | ((uint64_t)(umask) << 8) | (3ULL << 16) | (1ULL << 22))

/* Zen 3 event configurations */
#define AMD_EVT_L1D_REFILL    AMD_PERF_CTL_ENC(0x44, 0xFF)  /* L1D cache refills from all sources */
#define AMD_EVT_L2_MISS       AMD_PERF_CTL_ENC(0x64, 0x09)  /* Core to L2: miss */
#define AMD_EVT_BRANCH_MISP   AMD_PERF_CTL_ENC(0xC3, 0x00)  /* Retired branch mispredictions */
#define AMD_EVT_INSTR_RETIRED AMD_PERF_CTL_ENC(0xC0, 0x00)  /* Instructions retired */
#define AMD_EVT_CPU_CYCLES    AMD_PERF_CTL_ENC(0x76, 0x00)  /* CPU clocks not halted */

/* Indices into snapshot.counters[] — must match event config order */
#define CPIDX_L1D    0
#define CPIDX_L2     1
#define CPIDX_BRANCH 2
#define CPIDX_INSTR  3
#define CPIDX_CYCLES 4

/* NtSystemDebugControl command codes */
#define SYSDBG_CMD_READ_MSR  9
#define SYSDBG_CMD_WRITE_MSR 10

/* MSR I/O buffer (matches Windows kernel SYSDBG_MSR struct layout) */
typedef struct {
    unsigned long      Msr;
    unsigned long long Data;
} cache_prof__msr_io;

typedef long (__stdcall *cache_prof__NtSysDbgCtrl_fn)(
    int Command, void *InBuf, unsigned long InLen,
    void *OutBuf, unsigned long OutLen, unsigned long *RetLen);

/* Token privilege types (manual layout for TCC compatibility) */
typedef struct { unsigned long LowPart; long HighPart; } cache_prof__luid;
typedef struct { cache_prof__luid Luid; unsigned long Attributes; } cache_prof__luid_attr;
typedef struct { unsigned long Count; cache_prof__luid_attr Privileges[1]; } cache_prof__token_privs;

typedef int (__stdcall *fn_cprof_OpenProcessToken)(void*, unsigned long, void**);
typedef int (__stdcall *fn_cprof_LookupPrivilegeValueA)(const char*, const char*, cache_prof__luid*);
typedef int (__stdcall *fn_cprof_AdjustTokenPrivileges)(void*, int, cache_prof__token_privs*,
                                                        unsigned long, void*, unsigned long*);

#endif /* CACHE_PROF_WINDOWS */

/* ── Global state ───────────────────────────────────────────────────────── */

static int                cache_prof__available = 0;
static cache_prof_frame   cache_prof__frame;
static char               cache_prof__zone_names[CACHE_PROF_MAX_ZONES][CACHE_PROF_MAX_NAME_LEN];
static char               cache_prof__status_message[192];

/* Zone nesting stack */
static const char        *cache_prof__stack_names[CACHE_PROF_MAX_STACK];
static cache_prof_snapshot cache_prof__stack_snaps[CACHE_PROF_MAX_STACK];
static int                cache_prof__stack_depth = 0;

#if CACHE_PROF_MACOS

static void *cache_prof__kperf_lib = NULL;

static kpc_set_counting_fn         cache_prof__kpc_set_counting = NULL;
static kpc_set_thread_counting_fn  cache_prof__kpc_set_thread_counting = NULL;
static kpc_set_config_fn           cache_prof__kpc_set_config = NULL;
static kpc_get_thread_counters_fn  cache_prof__kpc_get_thread_counters = NULL;
static kpc_get_counter_count_fn    cache_prof__kpc_get_counter_count = NULL;
static kpc_force_all_ctrs_set_fn   cache_prof__kpc_force_all_ctrs_set = NULL;

static uint32_t cache_prof__num_counters = 0;

/* Indices into the counter array.
   Fixed counters come first, then configurables.
   On Apple Silicon, fixed counters 0 = instructions, 1 = cycles.
   Configurable counters start at index = num_fixed. */
static uint32_t cache_prof__idx_instructions = 0;
static uint32_t cache_prof__idx_cycles       = 1;
static uint32_t cache_prof__idx_l1d_miss     = 0;
static uint32_t cache_prof__idx_l1i_miss     = 0;
static uint32_t cache_prof__idx_branch_miss  = 0;

#endif /* CACHE_PROF_MACOS */

#if CACHE_PROF_WINDOWS
static cache_prof__NtSysDbgCtrl_fn cache_prof__ntdbg = NULL;
static int                         cache_prof__pmc_hw = 0;  /* 1 = AMD PMC active */
static LARGE_INTEGER               cache_prof__qpc_freq;
static unsigned long               cache_prof__ctr_msr[AMD_PMC_COUNT]; /* precomputed counter MSR addrs */
#endif /* CACHE_PROF_WINDOWS */

/* ── MSR helpers (Windows) ──────────────────────────────────────────────── */

#if CACHE_PROF_WINDOWS

static int cache_prof__read_msr(unsigned long addr, unsigned long long *value) {
    cache_prof__msr_io io;
    long st;
    io.Msr = addr;
    io.Data = 0;
    st = cache_prof__ntdbg(SYSDBG_CMD_READ_MSR, &io, sizeof(io),
                           &io, sizeof(io), NULL);
    if (st < 0) return 0;
    *value = io.Data;
    return 1;
}

static int cache_prof__write_msr(unsigned long addr, unsigned long long value) {
    cache_prof__msr_io io;
    long st;
    io.Msr = addr;
    io.Data = value;
    st = cache_prof__ntdbg(SYSDBG_CMD_WRITE_MSR, &io, sizeof(io),
                           NULL, 0, NULL);
    return (st >= 0) ? 1 : 0;
}

static int cache_prof__enable_debug_privilege(void) {
    HMODULE adv;
    fn_cprof_OpenProcessToken pOpenTok;
    fn_cprof_LookupPrivilegeValueA pLookup;
    fn_cprof_AdjustTokenPrivileges pAdjust;
    void *token;
    cache_prof__luid luid;
    cache_prof__token_privs tp;
    int ok;

    adv = LoadLibraryA("advapi32.dll");
    if (!adv) return 0;

    pOpenTok = (fn_cprof_OpenProcessToken)GetProcAddress(adv, "OpenProcessToken");
    pLookup  = (fn_cprof_LookupPrivilegeValueA)GetProcAddress(adv, "LookupPrivilegeValueA");
    pAdjust  = (fn_cprof_AdjustTokenPrivileges)GetProcAddress(adv, "AdjustTokenPrivileges");
    if (!pOpenTok || !pLookup || !pAdjust) { FreeLibrary(adv); return 0; }

    token = NULL;
    /* TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY */
    if (!pOpenTok(GetCurrentProcess(), 0x0028, &token)) {
        FreeLibrary(adv);
        return 0;
    }

    luid.LowPart = 0; luid.HighPart = 0;
    if (!pLookup(NULL, "SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        FreeLibrary(adv);
        return 0;
    }

    tp.Count = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = 2; /* SE_PRIVILEGE_ENABLED */

    ok = pAdjust(token, 0, &tp, 0, NULL, NULL);
    CloseHandle(token);
    FreeLibrary(adv);
    return ok;
}

static void cache_prof__configure_pmc_on_current_core(void) {
    static const uint64_t events[AMD_PMC_COUNT] = {
        AMD_EVT_L1D_REFILL, AMD_EVT_L2_MISS, AMD_EVT_BRANCH_MISP,
        AMD_EVT_INSTR_RETIRED, AMD_EVT_CPU_CYCLES
    };
    int i;
    for (i = 0; i < AMD_PMC_COUNT; i++) {
        unsigned long ctl = AMD_PERF_CTL_BASE + 2 * (unsigned long)i;
        unsigned long ctr = AMD_PERF_CTR_BASE + 2 * (unsigned long)i;
        cache_prof__write_msr(ctl, 0);          /* disable */
        cache_prof__write_msr(ctr, 0);          /* reset count */
        cache_prof__write_msr(ctl, events[i]);  /* enable with event */
    }
}

static int cache_prof__try_amd_pmc_init(void) {
    HMODULE ntdll;
    unsigned long long tsc_val;
    SYSTEM_INFO si;
    unsigned long nproc, i;
    unsigned long long test_val;
    DWORD_PTR prev_mask;

    ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return 0;

    cache_prof__ntdbg = (cache_prof__NtSysDbgCtrl_fn)
        GetProcAddress(ntdll, "NtSystemDebugControl");
    if (!cache_prof__ntdbg) return 0;

    if (!cache_prof__enable_debug_privilege()) {
        fprintf(stderr, "[cache_profiler] SeDebugPrivilege denied — "
                        "run as administrator for hardware PMCs\n");
        return 0;
    }

    /* Test MSR read with TSC (MSR 0x10 — always readable on x86) */
    tsc_val = 0;
    if (!cache_prof__read_msr(0x10, &tsc_val)) {
        fprintf(stderr, "[cache_profiler] NtSystemDebugControl MSR read "
                        "blocked (HVCI/VBS likely enabled)\n");
        return 0;
    }

    /* Configure PMCs on all logical processors */
    GetSystemInfo(&si);
    nproc = si.dwNumberOfProcessors;

    /* Save current thread affinity by setting to core 0, getting previous */
    prev_mask = SetThreadAffinityMask(GetCurrentThread(), 1);
    if (!prev_mask) prev_mask = ~(DWORD_PTR)0;

    for (i = 0; i < nproc && i < 64; i++) {
        DWORD_PTR core_mask = (DWORD_PTR)1 << i;
        if (SetThreadAffinityMask(GetCurrentThread(), core_mask)) {
            Sleep(0); /* yield to get scheduled on target core */
            cache_prof__configure_pmc_on_current_core();
        }
    }

    /* Restore original affinity */
    SetThreadAffinityMask(GetCurrentThread(), prev_mask);
    Sleep(0);

    /* Verify we can read a PMC counter */
    test_val = 0;
    if (!cache_prof__read_msr(cache_prof__ctr_msr[0], &test_val)) {
        fprintf(stderr, "[cache_profiler] PMC counter read verify failed\n");
        return 0;
    }

    return 1;
}

#endif /* CACHE_PROF_WINDOWS */

/* ── Snapshot helper ────────────────────────────────────────────────────── */

static void cache_prof__take_snapshot(cache_prof_snapshot *snap) {
    memset(snap, 0, sizeof(*snap));
#if CACHE_PROF_MACOS
    if (cache_prof__available && cache_prof__kpc_get_thread_counters) {
        cache_prof__kpc_get_thread_counters(0, cache_prof__num_counters,
                                            snap->counters);
    }
#elif CACHE_PROF_WINDOWS
    if (cache_prof__pmc_hw && cache_prof__ntdbg) {
        int i;
        for (i = 0; i < AMD_PMC_COUNT; i++) {
            cache_prof__read_msr(cache_prof__ctr_msr[i], &snap->counters[i]);
        }
    } else if (cache_prof__available) {
        LARGE_INTEGER ctr;
        QueryPerformanceCounter(&ctr);
        snap->counters[CPIDX_CYCLES] = (uint64_t)ctr.QuadPart;
    }
#endif
}

static void cache_prof__set_status(const char *msg) {
    size_t i = 0;
    if (!msg) msg = "";
    while (msg[i] && i + 1 < sizeof(cache_prof__status_message)) {
        cache_prof__status_message[i] = msg[i];
        i++;
    }
    cache_prof__status_message[i] = '\0';
}

/* ── API function bodies ────────────────────────────────────────────────── */

void cache_prof_init(void) {
    memset(&cache_prof__frame, 0, sizeof(cache_prof__frame));
    cache_prof__stack_depth = 0;
    cache_prof__available = 0;
    cache_prof__set_status("Cache PMU profiler not initialized");

#if CACHE_PROF_MACOS
    /* Try to load the kperf dynamic library */
    cache_prof__kperf_lib = dlopen(
        "/System/Library/PrivateFrameworks/kperf.framework/kperf", RTLD_LAZY);
    if (!cache_prof__kperf_lib) {
        /* Fallback: try libkperf.dylib directly */
        cache_prof__kperf_lib = dlopen("libkperf.dylib", RTLD_LAZY);
    }
    if (!cache_prof__kperf_lib) {
        fprintf(stderr, "[cache_profiler] dlopen kperf failed: %s\n", dlerror());
        cache_prof__set_status("kperf framework not loadable on this macOS version");
        return;
    }

    /* Resolve function pointers */
    cache_prof__kpc_set_counting =
        (kpc_set_counting_fn)dlsym(cache_prof__kperf_lib, "kpc_set_counting");
    cache_prof__kpc_set_thread_counting =
        (kpc_set_thread_counting_fn)dlsym(cache_prof__kperf_lib, "kpc_set_thread_counting");
    cache_prof__kpc_set_config =
        (kpc_set_config_fn)dlsym(cache_prof__kperf_lib, "kpc_set_config");
    cache_prof__kpc_get_thread_counters =
        (kpc_get_thread_counters_fn)dlsym(cache_prof__kperf_lib,
                                          "kpc_get_thread_counters");
    cache_prof__kpc_get_counter_count =
        (kpc_get_counter_count_fn)dlsym(cache_prof__kperf_lib,
                                        "kpc_get_counter_count");
    cache_prof__kpc_force_all_ctrs_set =
        (kpc_force_all_ctrs_set_fn)dlsym(cache_prof__kperf_lib, "kpc_force_all_ctrs_set");

    if (!cache_prof__kpc_set_counting ||
        !cache_prof__kpc_set_config ||
        !cache_prof__kpc_get_thread_counters ||
        !cache_prof__kpc_get_counter_count) {
        fprintf(stderr, "[cache_profiler] dlsym failed for one or more kpc "
                        "functions\n");
        dlclose(cache_prof__kperf_lib);
        cache_prof__kperf_lib = NULL;
        cache_prof__set_status("kpc symbols missing (API changed)");
        return;
    }

    /* Query counter count */
    uint32_t classes = KPC_CLASS_FIXED | KPC_CLASS_CONFIGURABLE;
    cache_prof__num_counters = cache_prof__kpc_get_counter_count(classes);
    if (cache_prof__num_counters == 0 ||
        cache_prof__num_counters > KPC_MAX_COUNTERS) {
        fprintf(stderr, "[cache_profiler] unexpected counter count: %u\n",
                cache_prof__num_counters);
        dlclose(cache_prof__kperf_lib);
        cache_prof__kperf_lib = NULL;
        cache_prof__set_status("kpc counter topology unsupported");
        return;
    }

    /* Determine fixed counter count to find configurable counter base */
    uint32_t num_fixed = cache_prof__kpc_get_counter_count(KPC_CLASS_FIXED);

    /* Fixed counters: instructions (0), cycles (1) on Apple Silicon */
    cache_prof__idx_instructions = 0;
    cache_prof__idx_cycles       = 1;

    /* Configurable counters start after fixed counters.
       We configure 3 configurable events: L1D miss, L1I miss, branch miss.
       Note: Event IDs are Apple M1 specific. M2/M3/M4 may differ. */
    cache_prof__idx_l1d_miss    = num_fixed + 0;
    cache_prof__idx_l1i_miss    = num_fixed + 1;
    cache_prof__idx_branch_miss = num_fixed + 2;

    /* Configure the PMC events */
    uint32_t num_configurable =
        cache_prof__kpc_get_counter_count(KPC_CLASS_CONFIGURABLE);

    uint64_t config[KPC_MAX_COUNTERS];
    memset(config, 0, sizeof(config));

    if (num_configurable >= 3) {
        config[0] = CACHE_PROF_EVT_L1D_MISS;
        config[1] = CACHE_PROF_EVT_L1I_MISS;
        config[2] = CACHE_PROF_EVT_BRANCH_MISS;
    }

    if (cache_prof__kpc_force_all_ctrs_set) {
        int force_ret = cache_prof__kpc_force_all_ctrs_set(1);
        if (force_ret != 0) {
            fprintf(stderr, "[cache_profiler] kpc_force_all_ctrs_set failed: %d\n", force_ret);
        }
    }

    int ret = cache_prof__kpc_set_config(KPC_CLASS_CONFIGURABLE, config);
    if (ret != 0) {
        fprintf(stderr, "[cache_profiler] kpc_set_config failed: %d\n", ret);
        dlclose(cache_prof__kperf_lib);
        cache_prof__kperf_lib = NULL;
        cache_prof__set_status("kpc_set_config denied (requires privileged PMU access)");
        return;
    }

    /* Enable counting */
    ret = cache_prof__kpc_set_counting(classes);
    if (ret != 0 && cache_prof__kpc_set_thread_counting) {
        ret = cache_prof__kpc_set_thread_counting(classes);
    }
    if (ret != 0) {
        fprintf(stderr, "[cache_profiler] kpc_set_counting failed: %d\n", ret);
        dlclose(cache_prof__kperf_lib);
        cache_prof__kperf_lib = NULL;
        cache_prof__set_status("kpc_set_counting denied (requires privileged PMU access)");
        return;
    }

    cache_prof__available = 1;
    cache_prof__set_status("Hardware counters active (kpc)");
    fprintf(stderr, "[cache_profiler] initialized: %u counters (%u fixed, "
                    "%u configurable)\n",
            cache_prof__num_counters, num_fixed, num_configurable);

#endif /* CACHE_PROF_MACOS */

#if CACHE_PROF_WINDOWS
    QueryPerformanceFrequency(&cache_prof__qpc_freq);

    /* Precompute counter MSR addresses */
    {
        int i;
        for (i = 0; i < AMD_PMC_COUNT; i++)
            cache_prof__ctr_msr[i] = AMD_PERF_CTR_BASE + 2 * (unsigned long)i;
    }

    /* Try AMD hardware PMC init (requires admin + no HVCI) */
    if (cache_prof__try_amd_pmc_init()) {
        cache_prof__pmc_hw = 1;
        cache_prof__available = 1;
        cache_prof__set_status("AMD Zen PMC hardware counters active");
        fprintf(stderr, "[cache_profiler] AMD PMC backend: L1D/L2/Branch/Instr/Cycles\n");
    } else {
        cache_prof__pmc_hw = 0;
        cache_prof__available = 1;
        cache_prof__set_status("QPC cycle counter active (no hardware PMCs)");
        fprintf(stderr, "[cache_profiler] Windows QPC fallback (freq=%llu)\n",
                (unsigned long long)cache_prof__qpc_freq.QuadPart);
    }
#endif

#if !CACHE_PROF_MACOS && !CACHE_PROF_WINDOWS
    cache_prof__set_status("No hardware cache-counter backend for this platform");
#endif
}

void cache_prof_shutdown(void) {
#if CACHE_PROF_MACOS
    if (cache_prof__kperf_lib) {
        dlclose(cache_prof__kperf_lib);
        cache_prof__kperf_lib = NULL;
    }
    cache_prof__kpc_set_counting = NULL;
    cache_prof__kpc_set_thread_counting = NULL;
    cache_prof__kpc_set_config = NULL;
    cache_prof__kpc_get_thread_counters = NULL;
    cache_prof__kpc_get_counter_count = NULL;
    cache_prof__kpc_force_all_ctrs_set = NULL;
#endif
#if CACHE_PROF_WINDOWS
    /* Disable PMCs on all cores if we configured them */
    if (cache_prof__pmc_hw && cache_prof__ntdbg) {
        SYSTEM_INFO si;
        unsigned long i;
        DWORD_PTR prev_mask;
        GetSystemInfo(&si);
        prev_mask = SetThreadAffinityMask(GetCurrentThread(), 1);
        if (!prev_mask) prev_mask = ~(DWORD_PTR)0;
        for (i = 0; i < si.dwNumberOfProcessors && i < 64; i++) {
            DWORD_PTR core_mask = (DWORD_PTR)1 << i;
            if (SetThreadAffinityMask(GetCurrentThread(), core_mask)) {
                int j;
                Sleep(0);
                for (j = 0; j < AMD_PMC_COUNT; j++)
                    cache_prof__write_msr(AMD_PERF_CTL_BASE + 2 * (unsigned long)j, 0);
            }
        }
        SetThreadAffinityMask(GetCurrentThread(), prev_mask);
    }
    cache_prof__pmc_hw = 0;
    cache_prof__ntdbg = NULL;
#endif
    cache_prof__available = 0;
    cache_prof__stack_depth = 0;
    cache_prof__set_status("Cache PMU profiler stopped");
}

void cache_zone_begin(const char *name) {
    if (!cache_prof__available) return;
    if (cache_prof__stack_depth >= CACHE_PROF_MAX_STACK) return;

    int d = cache_prof__stack_depth;
    cache_prof__stack_names[d] = name;
    cache_prof__take_snapshot(&cache_prof__stack_snaps[d]);
    cache_prof__stack_depth = d + 1;
}

void cache_zone_end(void) {
    if (!cache_prof__available) return;
    if (cache_prof__stack_depth <= 0) return;

    cache_prof__stack_depth--;
    int d = cache_prof__stack_depth;

    /* Take an "after" snapshot */
    cache_prof_snapshot after;
    cache_prof__take_snapshot(&after);

    /* Compute deltas */
    cache_prof_snapshot *before = &cache_prof__stack_snaps[d];

    uint64_t l1d_miss    = 0;
    uint64_t l2_miss     = 0;
    uint64_t l1i_miss    = 0;
    uint64_t branch_miss = 0;
    uint64_t instructions = 0;
    uint64_t cycles       = 0;

#if CACHE_PROF_MACOS
    l1d_miss    = after.counters[cache_prof__idx_l1d_miss]    -
                  before->counters[cache_prof__idx_l1d_miss];
    l1i_miss    = after.counters[cache_prof__idx_l1i_miss]    -
                  before->counters[cache_prof__idx_l1i_miss];
    branch_miss = after.counters[cache_prof__idx_branch_miss] -
                  before->counters[cache_prof__idx_branch_miss];
    instructions = after.counters[cache_prof__idx_instructions] -
                   before->counters[cache_prof__idx_instructions];
    cycles       = after.counters[cache_prof__idx_cycles]      -
                   before->counters[cache_prof__idx_cycles];
#elif CACHE_PROF_WINDOWS
    if (cache_prof__pmc_hw) {
        l1d_miss     = after.counters[CPIDX_L1D]    - before->counters[CPIDX_L1D];
        l2_miss      = after.counters[CPIDX_L2]     - before->counters[CPIDX_L2];
        branch_miss  = after.counters[CPIDX_BRANCH] - before->counters[CPIDX_BRANCH];
        instructions = after.counters[CPIDX_INSTR]  - before->counters[CPIDX_INSTR];
        cycles       = after.counters[CPIDX_CYCLES] - before->counters[CPIDX_CYCLES];
    } else {
        cycles = after.counters[CPIDX_CYCLES] - before->counters[CPIDX_CYCLES];
    }
#endif

    /* Append to frame SoA arrays */
    if (cache_prof__frame.count < CACHE_PROF_MAX_ZONES) {
        uint16_t idx = cache_prof__frame.count;
        const char *src_name = cache_prof__stack_names[d];
        size_t n = 0;
        if (!src_name) src_name = "(null)";
        while (src_name[n] && n + 1 < CACHE_PROF_MAX_NAME_LEN) {
            cache_prof__zone_names[idx][n] = src_name[n];
            n++;
        }
        cache_prof__zone_names[idx][n] = '\0';

        /* Keep a stable name pointer across engine hot reloads. */
        cache_prof__frame.names[idx]        = cache_prof__zone_names[idx];
        cache_prof__frame.l1d_miss[idx]     = l1d_miss;
        cache_prof__frame.l2_miss[idx]      = l2_miss;
        cache_prof__frame.l1i_miss[idx]     = l1i_miss;
        cache_prof__frame.branch_miss[idx]  = branch_miss;
        cache_prof__frame.instructions[idx] = instructions;
        cache_prof__frame.cycles[idx]       = cycles;
        cache_prof__frame.count = idx + 1;
    }
}

void cache_prof_frame_reset(void) {
    cache_prof__frame.count = 0;
    cache_prof__stack_depth = 0;
}

int cache_prof_available(void) {
    return cache_prof__available;
}

int cache_prof_is_hardware_backend(void) {
#if CACHE_PROF_MACOS
    return cache_prof__available;
#elif CACHE_PROF_WINDOWS
    return cache_prof__pmc_hw;
#else
    return 0;
#endif
}

const char *cache_prof_status_message(void) {
    return cache_prof__status_message;
}

cache_prof_frame *cache_prof_get_frame(void) {
    return &cache_prof__frame;
}

void cache_prof_sort_zones(void) {
    /* Insertion sort by l1d_miss descending */
    uint16_t n = cache_prof__frame.count;
    uint16_t i, j;

    for (i = 1; i < n; i++) {
        /* Save element i */
        const char *t_name   = cache_prof__frame.names[i];
        uint64_t t_l1d       = cache_prof__frame.l1d_miss[i];
        uint64_t t_l2        = cache_prof__frame.l2_miss[i];
        uint64_t t_l1i       = cache_prof__frame.l1i_miss[i];
        uint64_t t_branch    = cache_prof__frame.branch_miss[i];
        uint64_t t_instr     = cache_prof__frame.instructions[i];
        uint64_t t_cyc       = cache_prof__frame.cycles[i];

        j = i;
        while (j > 0 && cache_prof__frame.l1d_miss[j - 1] < t_l1d) {
            /* Shift element j-1 to j */
            cache_prof__frame.names[j]        = cache_prof__frame.names[j - 1];
            cache_prof__frame.l1d_miss[j]     = cache_prof__frame.l1d_miss[j - 1];
            cache_prof__frame.l2_miss[j]      = cache_prof__frame.l2_miss[j - 1];
            cache_prof__frame.l1i_miss[j]     = cache_prof__frame.l1i_miss[j - 1];
            cache_prof__frame.branch_miss[j]  = cache_prof__frame.branch_miss[j - 1];
            cache_prof__frame.instructions[j] = cache_prof__frame.instructions[j - 1];
            cache_prof__frame.cycles[j]       = cache_prof__frame.cycles[j - 1];
            j--;
        }

        /* Insert saved element at position j */
        cache_prof__frame.names[j]        = t_name;
        cache_prof__frame.l1d_miss[j]     = t_l1d;
        cache_prof__frame.l2_miss[j]      = t_l2;
        cache_prof__frame.l1i_miss[j]     = t_l1i;
        cache_prof__frame.branch_miss[j]  = t_branch;
        cache_prof__frame.instructions[j] = t_instr;
        cache_prof__frame.cycles[j]       = t_cyc;
    }
}

#endif /* CACHE_PROF_IMPL */

#endif /* CACHE_PROFILER_H */
