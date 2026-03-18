#ifndef ARENA_H
#define ARENA_H

#include <stdint.h>
#include <string.h>

/* Tracy integration: plot arena usage when Tracy is active */
#if defined(TRACY_ENABLE) && !defined(ARENA_TRACY_PLOT)
#include <tracy/TracyC.h>
#define ARENA_TRACY_PLOT(name, val) TracyCPlotI(name, val)
#endif

#define ARENA_MAX_RECORDS 128

typedef struct arena arena;

typedef struct {
    uint32_t    offset;
    uint32_t    size;
    const char *tag;
    arena      *child;      /* non-null → this record IS a sub-arena */
} arena_record;

struct arena {
    uint8_t     *base;
    uint32_t     capacity;
    uint32_t     used;
    arena_record records[ARENA_MAX_RECORDS];
    uint32_t     record_count;
};

static inline void arena_init(arena *a, void *base, uint32_t capacity) {
    a->base         = (uint8_t *)base;
    a->capacity     = capacity;
    a->used         = 0;
    a->record_count = 0;
}

static inline void *arena_alloc(arena *a, uint32_t size, uint32_t align, const char *tag) {
    uint32_t mask    = align - 1;
    uint32_t aligned = (a->used + mask) & ~mask;
    if (aligned + size > a->capacity) return 0;

    void *ptr = a->base + aligned;
    memset(ptr, 0, size);

    if (a->record_count < ARENA_MAX_RECORDS) {
        arena_record *r = &a->records[a->record_count++];
        r->offset = aligned;
        r->size   = size;
        r->tag    = tag;
        r->child  = 0;
    }

    a->used = aligned + size;
#ifdef ARENA_TRACY_PLOT
    ARENA_TRACY_PLOT("arena used", (int64_t)a->used);
#endif
    return ptr;
}

static inline void arena_reset(arena *a) {
    a->used         = 0;
    a->record_count = 0;
}

/* Allocate a child arena from a parent.
   The child struct lives at the start of the parent allocation,
   followed by 'capacity' bytes of backing memory. */
static inline arena *arena_alloc_subarena(arena *parent, uint32_t capacity,
                                          uint32_t align, const char *tag) {
    uint32_t total = (uint32_t)sizeof(arena) + capacity;
    arena *child = (arena *)arena_alloc(parent, total, align, tag);
    if (!child) return 0;

    /* arena_alloc already zeroed the memory; init the child */
    child->base     = (uint8_t *)child + sizeof(arena);
    child->capacity = capacity;
    /* used and record_count are already 0 from memset */

    /* Link the parent record to this child */
    if (parent->record_count > 0) {
        parent->records[parent->record_count - 1].child = child;
    }

    return child;
}

#endif /* ARENA_H */
