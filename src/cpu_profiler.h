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
    uint16_t    parent[CPU_PROF_MAX_ZONES]; /* CPU_PROF_INVALID_PARENT for roots */
    uint16_t    count;
} cpu_prof_frame;

/* ── API Declarations ───────────────────────────────────────────────────── */

void            cpu_prof_init(void);
void            cpu_prof_shutdown(void);
void            cpu_zone_begin(const char *name);
void            cpu_zone_end(void);
void            cpu_prof_frame_end(void);
void            cpu_prof_clear_current_frame(void);
cpu_prof_frame *cpu_prof_get_frame(void);
cpu_prof_frame *cpu_prof_get_frame_at_offset(uint16_t frames_back);
uint64_t        cpu_prof_get_frame_id_at_offset(uint16_t frames_back);
int             cpu_prof_get_history_count(void);
void            cpu_prof_sort_zones(void);

#define CPU_PROF_INVALID_PARENT 0xFFFFu

/* ── Implementation ─────────────────────────────────────────────────────── */

#ifdef CPU_PROF_IMPL

/* ── SQLite ─────────────────────────────────────────────────────────────── */

#include "sqlite3.h"
#include <stdio.h>

#define CPU_PROF_BATCH_SIZE 60   /* flush every ~1s at 60fps */

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

/* SQLite persistence state */
static sqlite3      *cpu_prof__db = NULL;
static sqlite3_stmt *cpu_prof__insert_stmt = NULL;
static uint64_t      cpu_prof__frame_id = 0;
static uint32_t      cpu_prof__batch_counter = 0;

/* ── SQLite helpers ─────────────────────────────────────────────────────── */

static void cpu_prof__db_init(void) {
    int rc = sqlite3_open("build/profile.db", &cpu_prof__db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[cpu_profiler] sqlite3_open failed: %s\n",
                cpu_prof__db ? sqlite3_errmsg(cpu_prof__db) : "unknown");
        if (cpu_prof__db) {
            sqlite3_close(cpu_prof__db);
        }
        cpu_prof__db = NULL;
        return;
    }

    /* WAL mode for better concurrent read/write */
    sqlite3_exec(cpu_prof__db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(cpu_prof__db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    const char *create_sql =
        "CREATE TABLE IF NOT EXISTS zones ("
        "  frame_id  INTEGER NOT NULL,"
        "  name      TEXT    NOT NULL,"
        "  start_ns  INTEGER NOT NULL,"
        "  duration_ns INTEGER NOT NULL,"
        "  depth     INTEGER NOT NULL,"
        "  parent_idx INTEGER NOT NULL DEFAULT -1"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_zones_frame ON zones(frame_id);";

    char *err = NULL;
    rc = sqlite3_exec(cpu_prof__db, create_sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[cpu_profiler] create table failed: %s\n",
                err ? err : "unknown");
        if (err) sqlite3_free(err);
        sqlite3_close(cpu_prof__db);
        cpu_prof__db = NULL;
        return;
    }

    /* Find the max frame_id so we continue from where we left off */
    sqlite3_stmt *q = NULL;
    cpu_prof__frame_id = 0;
    rc = sqlite3_prepare_v2(cpu_prof__db,
        "SELECT COALESCE(MAX(frame_id), 0) FROM zones;", -1, &q, NULL);
    if (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
        cpu_prof__frame_id = (uint64_t)sqlite3_column_int64(q, 0);
    }
    if (q) sqlite3_finalize(q);

    /* Backfill old DB schema created before parent_idx existed. */
    sqlite3_exec(cpu_prof__db,
                 "ALTER TABLE zones ADD COLUMN parent_idx INTEGER NOT NULL DEFAULT -1;",
                 NULL, NULL, NULL);

    cpu_prof__current_frame_id = cpu_prof__frame_id + 1;
    if (cpu_prof__current_frame_id == 0) cpu_prof__current_frame_id = 1;

    rc = sqlite3_prepare_v2(cpu_prof__db,
        "INSERT INTO zones (frame_id, name, start_ns, duration_ns, depth, parent_idx) "
        "VALUES (?, ?, ?, ?, ?, ?);", -1, &cpu_prof__insert_stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[cpu_profiler] prepare insert failed: %s\n",
                sqlite3_errmsg(cpu_prof__db));
        sqlite3_close(cpu_prof__db);
        cpu_prof__db = NULL;
        return;
    }

    fprintf(stderr, "[cpu_profiler] SQLite initialized (frame_id=%llu)\n",
            (unsigned long long)cpu_prof__frame_id);
}

static void cpu_prof__db_flush(void) {
    if (!cpu_prof__db || !cpu_prof__insert_stmt) return;

    sqlite3_exec(cpu_prof__db, "BEGIN;", NULL, NULL, NULL);

    /* Walk the ring buffer for the last batch_counter frames */
    uint32_t frames_to_flush = cpu_prof__batch_counter;
    if (frames_to_flush > cpu_prof__frames_recorded) frames_to_flush = cpu_prof__frames_recorded;

    for (uint32_t f = 0; f < frames_to_flush; f++) {
        /* Walk backwards from most recently completed frame slot. */
        int idx = (int)cpu_prof__write_idx - (int)f;
        if (idx < 0) idx += CPU_PROF_MAX_FRAMES;

        cpu_prof_frame *frame = &cpu_prof__frames[idx];
        uint64_t fid = cpu_prof__slot_frame_id[idx];
        if (fid == 0) continue;

        for (uint16_t z = 0; z < frame->count; z++) {
            sqlite3_reset(cpu_prof__insert_stmt);
            sqlite3_bind_int64(cpu_prof__insert_stmt, 1, (sqlite3_int64)fid);
            sqlite3_bind_text(cpu_prof__insert_stmt, 2,
                              frame->names[z] ? frame->names[z] : "",
                              -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(cpu_prof__insert_stmt, 3,
                               (sqlite3_int64)frame->start_ns[z]);
            sqlite3_bind_int64(cpu_prof__insert_stmt, 4,
                               (sqlite3_int64)frame->duration_ns[z]);
            sqlite3_bind_int(cpu_prof__insert_stmt, 5, frame->depth[z]);
            sqlite3_bind_int(cpu_prof__insert_stmt, 6,
                             frame->parent[z] == CPU_PROF_INVALID_PARENT
                                 ? -1
                                 : (int)frame->parent[z]);
            sqlite3_step(cpu_prof__insert_stmt);
        }
    }

    sqlite3_exec(cpu_prof__db, "COMMIT;", NULL, NULL, NULL);
}

static void cpu_prof__db_shutdown(void) {
    if (!cpu_prof__db) return;

    /* Final flush of any remaining frames */
    if (cpu_prof__batch_counter > 0) {
        cpu_prof__db_flush();
        cpu_prof__batch_counter = 0;
    }

    if (cpu_prof__insert_stmt) {
        sqlite3_finalize(cpu_prof__insert_stmt);
        cpu_prof__insert_stmt = NULL;
    }
    sqlite3_close(cpu_prof__db);
    cpu_prof__db = NULL;
}

/* ── API function bodies ────────────────────────────────────────────────── */

void cpu_prof_init(void) {
    memset(cpu_prof__frames, 0, sizeof(cpu_prof__frames));
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
    cpu_prof__batch_counter = 0;
    cpu_prof__db_init();
}

void cpu_prof_shutdown(void) {
    cpu_prof__db_shutdown();
    cpu_prof__write_idx = 0;
    cpu_prof__frames_recorded = 0;
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
    memset(cpu_prof__slot_frame_id, 0, sizeof(cpu_prof__slot_frame_id));
}

void cpu_zone_begin(const char *name) {
    if (cpu_prof__stack_depth >= CPU_PROF_MAX_STACK) return;

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

void cpu_zone_end(void) {
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
}

void cpu_prof_frame_end(void) {
    /* Mark current slot as completed with its stable frame id. */
    cpu_prof__slot_frame_id[cpu_prof__write_idx] = cpu_prof__current_frame_id;
    cpu_prof__frame_id = cpu_prof__current_frame_id;
    if (cpu_prof__frames_recorded < CPU_PROF_MAX_FRAMES) cpu_prof__frames_recorded++;

    cpu_prof__batch_counter++;

    /* Batch-flush to SQLite every CPU_PROF_BATCH_SIZE frames */
    if (cpu_prof__batch_counter >= CPU_PROF_BATCH_SIZE) {
        cpu_prof__db_flush();
        cpu_prof__batch_counter = 0;
    }

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

void cpu_prof_clear_current_frame(void) {
    cpu_prof__frames[cpu_prof__write_idx].count = 0;
    cpu_prof__stack_depth = 0;
    memset(cpu_prof__stack_zone_idx, 0xFF, sizeof(cpu_prof__stack_zone_idx));
}

cpu_prof_frame *cpu_prof_get_frame(void) {
    return &cpu_prof__frames[cpu_prof__write_idx];
}

cpu_prof_frame *cpu_prof_get_frame_at_offset(uint16_t frames_back) {
    int idx;
    if (frames_back >= cpu_prof__frames_recorded) return NULL;
    idx = (int)cpu_prof__write_idx - (int)frames_back;
    if (idx < 0) idx += CPU_PROF_MAX_FRAMES;
    if (cpu_prof__slot_frame_id[idx] == 0) return NULL;
    return &cpu_prof__frames[idx];
}

uint64_t cpu_prof_get_frame_id_at_offset(uint16_t frames_back) {
    int idx;
    if (frames_back >= cpu_prof__frames_recorded) return 0;
    idx = (int)cpu_prof__write_idx - (int)frames_back;
    if (idx < 0) idx += CPU_PROF_MAX_FRAMES;
    return cpu_prof__slot_frame_id[idx];
}

int cpu_prof_get_history_count(void) {
    return (int)cpu_prof__frames_recorded;
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
