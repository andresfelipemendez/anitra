/*
 * cpu_profiler.h — CPU zone timing profiler (header-only, stb-style)
 *
 * Usage:
 *   In exactly one .c file (externals_runtime.c):
 *     #define CPU_PROF_IMPL
 *     #include "cpu_profiler.h"
 *
 *   Everywhere else:
 *     #include "cpu_profiler.h"   // struct typedefs + function declarations only
 *
 * Records per-frame zone timings with nanosecond precision.
 * Ring buffer stores the last 300 frames (~5 seconds at 60fps).
 * Thread-safe: each thread gets a private buffer, merged at frame_end.
 * Platform-specific high-resolution timing on macOS, Linux, and Windows.
 */

#ifndef CPU_PROFILER_H
#define CPU_PROFILER_H

#include <stdint.h>
#include <string.h>

/* ── Constants ──────────────────────────────────────────────────────────── */

#define CPU_PROF_MAX_ZONES  256
#define CPU_PROF_MAX_FRAMES 300
#define CPU_PROF_MAX_STACK  32
#define CPU_PROF_MAX_NAME_LEN 64
#define CPU_PROF_MAX_THREADS 8

/* ── Platform timing (always available) ─────────────────────────────────── */

#if defined(__APPLE__)
#include <time.h>
#elif defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

static inline uint64_t cpu_prof_now_ns(void) {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#elif defined(_WIN32)
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER ctr;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (uint64_t)((double)ctr.QuadPart / (double)freq.QuadPart * 1e9);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ── Data Structures ────────────────────────────────────────────────────── */

typedef struct {
    const char *names[CPU_PROF_MAX_ZONES];
    uint64_t    start_ns[CPU_PROF_MAX_ZONES];
    uint64_t    duration_ns[CPU_PROF_MAX_ZONES];
    uint16_t    depth[CPU_PROF_MAX_ZONES];
    uint16_t    parent[CPU_PROF_MAX_ZONES]; /* CPU_PROF_INVALID_PARENT for roots */
    uint16_t    count;
} cpu_prof_frame;

/* Per-thread profiler state (private buffer + nesting stack) */
typedef struct {
    cpu_prof_frame frame;
    char        zone_names[CPU_PROF_MAX_ZONES][CPU_PROF_MAX_NAME_LEN];
    int16_t     stack_zone_idx[CPU_PROF_MAX_STACK];
    int         stack_depth;
    const char *thread_name;
    int         active;
} cpu_prof_thread_state;

/* ── DLL export decoration ──────────────────────────────────────────────── */
/* Define CPU_PROF_DLL_EXPORT in the DLL that owns the profiler state
   (externals.dll) so all functions get __declspec(dllexport).
   Other DLLs (editor.dll) link against the .def file and import them. */
#if defined(CPU_PROF_DLL_EXPORT) && defined(_WIN32)
#define CPU_PROF_API __declspec(dllexport)
#else
#define CPU_PROF_API
#endif

/* ── API Declarations ───────────────────────────────────────────────────── */

#if defined(CPU_PROF_USE_FPTRS) && !defined(CPU_PROF_IMPL)
/* Function-pointer mode: DLLs that don't own profiler state (e.g. editor.dll)
   use these static pointers, wired at runtime via cpu_prof_assign_fns().
   Identifiers match the regular API so call sites need no changes. */
static void            (*cpu_prof_init)(void);
static void            (*cpu_prof_shutdown)(void);
static void            (*cpu_zone_begin)(const char *name);
static void            (*cpu_zone_end)(void);
static void            (*cpu_prof_frame_end)(void);
static void            (*cpu_prof_clear_current_frame)(void);
static void            (*cpu_prof_set_capture_enabled)(int enabled);
static int             (*cpu_prof_get_capture_enabled)(void);
static cpu_prof_frame *(*cpu_prof_get_frame)(void);
static cpu_prof_frame *(*cpu_prof_get_frame_at_offset)(uint16_t frames_back);
static uint64_t        (*cpu_prof_get_frame_id_at_offset)(uint16_t frames_back);
static int             (*cpu_prof_get_history_count)(void);
static void            (*cpu_prof_sort_zones)(void);
static int             (*cpu_prof_register_thread)(const char *name);
#else
CPU_PROF_API void            cpu_prof_init(void);
CPU_PROF_API void            cpu_prof_shutdown(void);
CPU_PROF_API void            cpu_zone_begin(const char *name);
CPU_PROF_API void            cpu_zone_end(void);
CPU_PROF_API void            cpu_prof_frame_end(void);
CPU_PROF_API void            cpu_prof_clear_current_frame(void);
CPU_PROF_API void            cpu_prof_set_capture_enabled(int enabled);
CPU_PROF_API int             cpu_prof_get_capture_enabled(void);
CPU_PROF_API cpu_prof_frame *cpu_prof_get_frame(void);
CPU_PROF_API cpu_prof_frame *cpu_prof_get_frame_at_offset(uint16_t frames_back);
CPU_PROF_API uint64_t        cpu_prof_get_frame_id_at_offset(uint16_t frames_back);
CPU_PROF_API int             cpu_prof_get_history_count(void);
CPU_PROF_API void            cpu_prof_sort_zones(void);
CPU_PROF_API int             cpu_prof_register_thread(const char *name);
#endif

#define CPU_PROF_INVALID_PARENT 0xFFFFu

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef CPU_PROF_IMPL

#include <stdio.h>
#include <SDL3/SDL_thread.h>

#ifdef CPU_PROF_USE_REMOTERY
#include "Remotery.h"
static Remotery *cpu_prof__rmt = NULL;
#endif

/* ── Global state ───────────────────────────────────────────────────────── */

/* Shared ring buffer (read by UI, written only during merge at frame_end) */
static cpu_prof_frame cpu_prof__frames[CPU_PROF_MAX_FRAMES];
static uint16_t       cpu_prof__write_idx = 0;
static char           cpu_prof__zone_names[CPU_PROF_MAX_FRAMES][CPU_PROF_MAX_ZONES][CPU_PROF_MAX_NAME_LEN];
static uint64_t       cpu_prof__slot_frame_id[CPU_PROF_MAX_FRAMES];
static uint16_t       cpu_prof__frames_recorded = 0;
static uint64_t       cpu_prof__current_frame_id = 1;
static int            cpu_prof__capture_enabled = 1;

/* Per-thread state (private buffers — zero contention during recording) */
static cpu_prof_thread_state cpu_prof__threads[CPU_PROF_MAX_THREADS];
static int                   cpu_prof__thread_count = 0;
static SDL_TLSID             cpu_prof__tls_id;

/* ── API function bodies ────────────────────────────────────────────────── */

CPU_PROF_API int cpu_prof_register_thread(const char *name) {
    int slot;
    if (cpu_prof__thread_count >= CPU_PROF_MAX_THREADS) return -1;
    slot = cpu_prof__thread_count++;
    {
        cpu_prof_thread_state *ts = &cpu_prof__threads[slot];
        memset(ts, 0, sizeof(*ts));
        ts->thread_name = name;
        ts->active = 1;
        ts->stack_depth = 0;
        ts->frame.count = 0;
        memset(ts->stack_zone_idx, 0xFF, sizeof(ts->stack_zone_idx));
        SDL_SetTLS(&cpu_prof__tls_id, ts, NULL);
    }
    return slot;
}

CPU_PROF_API void cpu_prof_init(void) {
    memset(cpu_prof__frames, 0, sizeof(cpu_prof__frames));
    memset(cpu_prof__threads, 0, sizeof(cpu_prof__threads));
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    cpu_prof__thread_count = 0;
    cpu_prof__capture_enabled = 1;

    /* Register calling thread (main) as slot 0 */
    cpu_prof_register_thread("main");

#ifdef CPU_PROF_USE_REMOTERY
    {
        rmtError err = rmt_CreateGlobalInstance(&cpu_prof__rmt);
        if (err != RMT_ERROR_NONE) {
            fprintf(stderr, "[cpu_profiler] Remotery init failed (%d)\n", (int)err);
            cpu_prof__rmt = NULL;
        }
    }
#endif
}

CPU_PROF_API void cpu_prof_shutdown(void) {
    int t;
#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) {
        rmt_DestroyGlobalInstance(cpu_prof__rmt);
        cpu_prof__rmt = NULL;
    }
#endif
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    for (t = 0; t < CPU_PROF_MAX_THREADS; t++) {
        cpu_prof__threads[t].stack_depth = 0;
        cpu_prof__threads[t].active = 0;
        memset(cpu_prof__threads[t].stack_zone_idx, 0xFF, sizeof(cpu_prof__threads[t].stack_zone_idx));
    }
    cpu_prof__thread_count = 0;
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
}

CPU_PROF_API void cpu_zone_begin(const char *name) {
    cpu_prof_thread_state *ts;
    int d;
    cpu_prof_frame *frame;

    if (!cpu_prof__capture_enabled) return;
    ts = (cpu_prof_thread_state *)SDL_GetTLS(&cpu_prof__tls_id);
    if (!ts) return;
    if (ts->stack_depth >= CPU_PROF_MAX_STACK) return;

#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) _rmt_BeginCPUSample(name, 0, NULL);
#endif

    d = ts->stack_depth;
    frame = &ts->frame;
    ts->stack_zone_idx[d] = -1;

    if (frame->count < CPU_PROF_MAX_ZONES) {
        uint16_t idx = frame->count;
        const char *src_name = name;
        char *dst_name = ts->zone_names[idx];
        size_t n = 0;

        if (!src_name) src_name = "(null)";
        while (src_name[n] && n + 1 < CPU_PROF_MAX_NAME_LEN) {
            dst_name[n] = src_name[n];
            n++;
        }
        dst_name[n] = '\0';

        frame->names[idx]       = dst_name;
        frame->start_ns[idx]    = cpu_prof_now_ns();
        frame->duration_ns[idx] = 0;
        frame->depth[idx]       = (uint16_t)d;
        frame->parent[idx]      = (d > 0 && ts->stack_zone_idx[d - 1] >= 0)
                                   ? (uint16_t)ts->stack_zone_idx[d - 1]
                                   : CPU_PROF_INVALID_PARENT;
        ts->stack_zone_idx[d] = (int16_t)idx;
        frame->count = idx + 1;
    }

    ts->stack_depth = d + 1;
}

CPU_PROF_API void cpu_zone_end(void) {
    cpu_prof_thread_state *ts;
    int d;
    int16_t zone_idx;
    cpu_prof_frame *frame;
    uint64_t end_ns;

    if (!cpu_prof__capture_enabled) return;
    ts = (cpu_prof_thread_state *)SDL_GetTLS(&cpu_prof__tls_id);
    if (!ts || ts->stack_depth <= 0) return;

    ts->stack_depth--;
    d = ts->stack_depth;
    zone_idx = ts->stack_zone_idx[d];

    if (zone_idx < 0) return;
    frame = &ts->frame;
    if ((uint16_t)zone_idx >= frame->count) return;

    end_ns = cpu_prof_now_ns();
    frame->duration_ns[(uint16_t)zone_idx] = end_ns - frame->start_ns[(uint16_t)zone_idx];

#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) _rmt_EndCPUSample();
#endif
}

CPU_PROF_API void cpu_prof_frame_end(void) {
    cpu_prof_frame *dst;
    uint16_t dst_count;
    int t;

    if (!cpu_prof__capture_enabled) return;

    dst = &cpu_prof__frames[cpu_prof__write_idx];
    dst_count = 0;

    /* Merge all thread-local frames into the ring buffer slot */
    for (t = 0; t < cpu_prof__thread_count; t++) {
        cpu_prof_thread_state *ts = &cpu_prof__threads[t];
        cpu_prof_frame *src;
        uint16_t base, i;

        if (!ts->active) continue;
        src = &ts->frame;
        base = dst_count;

        for (i = 0; i < src->count && dst_count < CPU_PROF_MAX_ZONES; i++) {
            uint16_t di = dst_count;
            const char *src_name = src->names[i] ? src->names[i] : "(null)";
            char *dn = cpu_prof__zone_names[cpu_prof__write_idx][di];
            size_t n = 0;
            while (src_name[n] && n + 1 < CPU_PROF_MAX_NAME_LEN) {
                dn[n] = src_name[n]; n++;
            }
            dn[n] = '\0';

            dst->names[di]       = dn;
            dst->start_ns[di]    = src->start_ns[i];
            dst->duration_ns[di] = src->duration_ns[i];
            dst->depth[di]       = src->depth[i];
            dst->parent[di]      = (src->parent[i] == CPU_PROF_INVALID_PARENT)
                                    ? CPU_PROF_INVALID_PARENT
                                    : (uint16_t)(src->parent[i] + base);
            dst_count++;
        }

        /* Reset thread-local frame for next frame */
        src->count = 0;
        ts->stack_depth = 0;
        memset(ts->stack_zone_idx, 0xFF, sizeof(ts->stack_zone_idx));
    }

    dst->count = dst_count;

    /* Commit to ring buffer */
    cpu_prof__slot_frame_id[cpu_prof__write_idx] = cpu_prof__current_frame_id;
    if (cpu_prof__frames_recorded < CPU_PROF_MAX_FRAMES) cpu_prof__frames_recorded++;

    /* Advance write index */
    cpu_prof__write_idx = (cpu_prof__write_idx + 1) % CPU_PROF_MAX_FRAMES;
    cpu_prof__current_frame_id++;
    if (cpu_prof__current_frame_id == 0) cpu_prof__current_frame_id = 1;

    /* Clear the next frame slot */
    cpu_prof__frames[cpu_prof__write_idx].count = 0;
    cpu_prof__slot_frame_id[cpu_prof__write_idx] = cpu_prof__current_frame_id;
}

CPU_PROF_API void cpu_prof_clear_current_frame(void) {
    int t;
    cpu_prof__frames[cpu_prof__write_idx].count = 0;
    for (t = 0; t < cpu_prof__thread_count; t++) {
        cpu_prof__threads[t].frame.count = 0;
        cpu_prof__threads[t].stack_depth = 0;
        memset(cpu_prof__threads[t].stack_zone_idx, 0xFF,
               sizeof(cpu_prof__threads[t].stack_zone_idx));
    }
}

CPU_PROF_API void cpu_prof_set_capture_enabled(int enabled) {
    int t;
    cpu_prof__capture_enabled = enabled ? 1 : 0;
    if (!cpu_prof__capture_enabled) {
        for (t = 0; t < cpu_prof__thread_count; t++) {
            cpu_prof__threads[t].stack_depth = 0;
            memset(cpu_prof__threads[t].stack_zone_idx, 0xFF,
                   sizeof(cpu_prof__threads[t].stack_zone_idx));
        }
    }
}

CPU_PROF_API int cpu_prof_get_capture_enabled(void) {
    return cpu_prof__capture_enabled;
}

CPU_PROF_API cpu_prof_frame *cpu_prof_get_frame(void) {
    return &cpu_prof__frames[cpu_prof__write_idx];
}

CPU_PROF_API cpu_prof_frame *cpu_prof_get_frame_at_offset(uint16_t frames_back) {
    int idx;
    if (frames_back >= cpu_prof__frames_recorded) return NULL;
    idx = (int)cpu_prof__write_idx - (int)frames_back;
    if (idx < 0) idx += CPU_PROF_MAX_FRAMES;
    if (cpu_prof__slot_frame_id[idx] == 0) return NULL;
    return &cpu_prof__frames[idx];
}

CPU_PROF_API uint64_t cpu_prof_get_frame_id_at_offset(uint16_t frames_back) {
    int idx;
    if (frames_back >= cpu_prof__frames_recorded) return 0;
    idx = (int)cpu_prof__write_idx - (int)frames_back;
    if (idx < 0) idx += CPU_PROF_MAX_FRAMES;
    return cpu_prof__slot_frame_id[idx];
}

CPU_PROF_API int cpu_prof_get_history_count(void) {
    return (int)cpu_prof__frames_recorded;
}

CPU_PROF_API void cpu_prof_sort_zones(void) {
    /* Insertion sort by duration_ns descending */
    cpu_prof_frame *frame = &cpu_prof__frames[cpu_prof__write_idx];
    uint16_t n = frame->count;
    uint16_t i, j;

    for (i = 1; i < n; i++) {
        /* Save element i */
        const char *t_name     = frame->names[i];
        uint64_t    t_start    = frame->start_ns[i];
        uint64_t    t_duration = frame->duration_ns[i];
        uint16_t    t_depth    = frame->depth[i];
        uint16_t    t_parent   = frame->parent[i];

        j = i;
        while (j > 0 && frame->duration_ns[j - 1] < t_duration) {
            /* Shift element j-1 to j */
            frame->names[j]       = frame->names[j - 1];
            frame->start_ns[j]    = frame->start_ns[j - 1];
            frame->duration_ns[j] = frame->duration_ns[j - 1];
            frame->depth[j]       = frame->depth[j - 1];
            frame->parent[j]      = frame->parent[j - 1];
            j--;
        }

        /* Insert saved element at position j */
        frame->names[j]       = t_name;
        frame->start_ns[j]    = t_start;
        frame->duration_ns[j] = t_duration;
        frame->depth[j]       = t_depth;
        frame->parent[j]      = t_parent;
    }
}

#endif /* CPU_PROF_IMPL */

#endif /* CPU_PROFILER_H */
