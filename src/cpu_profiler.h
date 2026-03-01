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

#define CPU_PROF_MAX_ZONES  64
#define CPU_PROF_MAX_FRAMES 300
#define CPU_PROF_MAX_STACK  16

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
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
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
    uint16_t    count;
} cpu_prof_frame;

/* ── API Declarations ───────────────────────────────────────────────────── */

void            cpu_prof_init(void);
void            cpu_prof_shutdown(void);
void            cpu_zone_begin(const char *name);
void            cpu_zone_end(void);
void            cpu_prof_frame_end(void);
cpu_prof_frame *cpu_prof_get_frame(void);
void            cpu_prof_sort_zones(void);

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef CPU_PROF_IMPL

/* ── Global state ───────────────────────────────────────────────────────── */

static cpu_prof_frame cpu_prof__frames[CPU_PROF_MAX_FRAMES];
static uint16_t       cpu_prof__write_idx = 0;

/* Zone nesting stack */
static const char *cpu_prof__stack_names[CPU_PROF_MAX_STACK];
static uint64_t    cpu_prof__stack_start_ns[CPU_PROF_MAX_STACK];
static int         cpu_prof__stack_depth = 0;

/* ── API function bodies ────────────────────────────────────────────────── */

void cpu_prof_init(void) {
    memset(cpu_prof__frames, 0, sizeof(cpu_prof__frames));
    cpu_prof__write_idx = 0;
    cpu_prof__stack_depth = 0;
}

void cpu_prof_shutdown(void) {
    cpu_prof__write_idx = 0;
    cpu_prof__stack_depth = 0;
}

void cpu_zone_begin(const char *name) {
    if (cpu_prof__stack_depth >= CPU_PROF_MAX_STACK) return;

    int d = cpu_prof__stack_depth;
    cpu_prof__stack_names[d] = name;
    cpu_prof__stack_start_ns[d] = cpu_prof_now_ns();
    cpu_prof__stack_depth = d + 1;
}

void cpu_zone_end(void) {
    if (cpu_prof__stack_depth <= 0) return;

    uint64_t end_ns = cpu_prof_now_ns();

    cpu_prof__stack_depth--;
    int d = cpu_prof__stack_depth;

    uint64_t start_ns = cpu_prof__stack_start_ns[d];
    uint64_t duration = end_ns - start_ns;

    /* Append to current frame's SoA arrays */
    cpu_prof_frame *frame = &cpu_prof__frames[cpu_prof__write_idx];
    if (frame->count < CPU_PROF_MAX_ZONES) {
        uint16_t idx = frame->count;
        frame->names[idx]       = cpu_prof__stack_names[d];
        frame->start_ns[idx]    = start_ns;
        frame->duration_ns[idx] = duration;
        frame->depth[idx]       = (uint16_t)d;
        frame->count = idx + 1;
    }
}

void cpu_prof_frame_end(void) {
    /* Advance write index, wrapping around the ring buffer */
    cpu_prof__write_idx = (cpu_prof__write_idx + 1) % CPU_PROF_MAX_FRAMES;

    /* Clear the next frame for fresh recording */
    cpu_prof__frames[cpu_prof__write_idx].count = 0;

    /* Reset zone stack for safety */
    cpu_prof__stack_depth = 0;
}

cpu_prof_frame *cpu_prof_get_frame(void) {
    return &cpu_prof__frames[cpu_prof__write_idx];
}

void cpu_prof_sort_zones(void) {
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

        j = i;
        while (j > 0 && frame->duration_ns[j - 1] < t_duration) {
            /* Shift element j-1 to j */
            frame->names[j]       = frame->names[j - 1];
            frame->start_ns[j]    = frame->start_ns[j - 1];
            frame->duration_ns[j] = frame->duration_ns[j - 1];
            frame->depth[j]       = frame->depth[j - 1];
            j--;
        }

        /* Insert saved element at position j */
        frame->names[j]       = t_name;
        frame->start_ns[j]    = t_start;
        frame->duration_ns[j] = t_duration;
        frame->depth[j]       = t_depth;
    }
}

#endif /* CPU_PROF_IMPL */

#endif /* CPU_PROFILER_H */
