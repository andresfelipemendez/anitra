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
#endif

#define CPU_PROF_INVALID_PARENT 0xFFFFu

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef CPU_PROF_IMPL

#include <stdio.h>

#ifdef CPU_PROF_USE_REMOTERY
#include "Remotery.h"
static Remotery *cpu_prof__rmt = NULL;
#endif

/* ── Global state ───────────────────────────────────────────────────────── */

static cpu_prof_frame cpu_prof__frames[CPU_PROF_MAX_FRAMES];
static uint16_t       cpu_prof__write_idx = 0;
static char           cpu_prof__zone_names[CPU_PROF_MAX_FRAMES][CPU_PROF_MAX_ZONES][CPU_PROF_MAX_NAME_LEN];
static uint64_t       cpu_prof__slot_frame_id[CPU_PROF_MAX_FRAMES];
static uint16_t       cpu_prof__frames_recorded = 0;
static uint64_t       cpu_prof__current_frame_id = 1;

/* Zone nesting stack */
static int16_t     cpu_prof__stack_zone_idx[CPU_PROF_MAX_STACK];
static int         cpu_prof__stack_depth = 0;
static int         cpu_prof__capture_enabled = 1;

/* ── API function bodies ────────────────────────────────────────────────── */

CPU_PROF_API void cpu_prof_init(void) {
    memset(cpu_prof__frames, 0, sizeof(cpu_prof__frames));
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    cpu_prof__stack_depth = 0;
    cpu_prof__capture_enabled = 1;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));

#ifdef CPU_PROF_USE_REMOTERY
    rmtError err = rmt_CreateGlobalInstance(&cpu_prof__rmt);
    if (err != RMT_ERROR_NONE) {
        fprintf(stderr, "[cpu_profiler] Remotery init failed (%d)\n", (int)err);
        cpu_prof__rmt = NULL;
    }
#endif
}

CPU_PROF_API void cpu_prof_shutdown(void) {
#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) {
        rmt_DestroyGlobalInstance(cpu_prof__rmt);
        cpu_prof__rmt = NULL;
    }
#endif
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
}

CPU_PROF_API void cpu_zone_begin(const char *name) {
    if (!cpu_prof__capture_enabled) return;
    if (cpu_prof__stack_depth >= CPU_PROF_MAX_STACK) return;

#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) _rmt_BeginCPUSample(name, 0, NULL);
#endif

    cpu_prof_frame *frame = &cpu_prof__frames[cpu_prof__write_idx];
    int d = cpu_prof__stack_depth;
    uint64_t start_ns = cpu_prof_now_ns();
    int16_t zone_idx = -1;

    cpu_prof__stack_zone_idx[d] = -1;

    if (frame->count < CPU_PROF_MAX_ZONES) {
        uint16_t idx = frame->count;
        const char *src_name = name;
        char *dst_name = cpu_prof__zone_names[cpu_prof__write_idx][idx];
        size_t n = 0;

        if (!src_name) src_name = "(null)";
        while (src_name[n] && n + 1 < CPU_PROF_MAX_NAME_LEN) {
            dst_name[n] = src_name[n];
            n++;
        }
        dst_name[n] = '\0';

        zone_idx = (int16_t)idx;
        frame->names[idx]       = dst_name;
        frame->start_ns[idx]    = start_ns;
        frame->duration_ns[idx] = 0;
        frame->depth[idx]       = (uint16_t)d;
        frame->parent[idx]      = (d > 0 && cpu_prof__stack_zone_idx[d - 1] >= 0)
                                   ? (uint16_t)cpu_prof__stack_zone_idx[d - 1]
                                   : CPU_PROF_INVALID_PARENT;
        frame->count = idx + 1;
    }

    cpu_prof__stack_zone_idx[d] = zone_idx;
    cpu_prof__stack_depth = d + 1;
}

CPU_PROF_API void cpu_zone_end(void) {
    if (!cpu_prof__capture_enabled) return;
    if (cpu_prof__stack_depth <= 0) return;

    cpu_prof_frame *frame = &cpu_prof__frames[cpu_prof__write_idx];
    uint64_t end_ns = cpu_prof_now_ns();

    cpu_prof__stack_depth--;
    int d = cpu_prof__stack_depth;
    int16_t zone_idx = cpu_prof__stack_zone_idx[d];
    uint64_t start_ns;

    if (zone_idx < 0) return;
    if ((uint16_t)zone_idx >= frame->count) return;

    start_ns = frame->start_ns[(uint16_t)zone_idx];
    frame->duration_ns[(uint16_t)zone_idx] = end_ns - start_ns;

#ifdef CPU_PROF_USE_REMOTERY
    if (cpu_prof__rmt) _rmt_EndCPUSample();
#endif
}

CPU_PROF_API void cpu_prof_frame_end(void) {
    if (!cpu_prof__capture_enabled) return;
    /* Mark current slot as completed with its stable frame id. */
    cpu_prof__slot_frame_id[cpu_prof__write_idx] = cpu_prof__current_frame_id;
    if (cpu_prof__frames_recorded < CPU_PROF_MAX_FRAMES) cpu_prof__frames_recorded++;

    /* Advance write index, wrapping around the ring buffer */
    cpu_prof__write_idx = (cpu_prof__write_idx + 1) % CPU_PROF_MAX_FRAMES;
    cpu_prof__current_frame_id++;
    if (cpu_prof__current_frame_id == 0) cpu_prof__current_frame_id = 1;

    /* Clear the next frame for fresh recording */
    cpu_prof__frames[cpu_prof__write_idx].count = 0;
    cpu_prof__slot_frame_id[cpu_prof__write_idx] = cpu_prof__current_frame_id;

    /* Reset zone stack for safety */
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
}

CPU_PROF_API void cpu_prof_clear_current_frame(void) {
    cpu_prof__frames[cpu_prof__write_idx].count = 0;
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
}

CPU_PROF_API void cpu_prof_set_capture_enabled(int enabled) {
    cpu_prof__capture_enabled = enabled ? 1 : 0;
    if (!cpu_prof__capture_enabled) {
        cpu_prof__stack_depth = 0;
        memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
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
