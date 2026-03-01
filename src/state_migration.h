#ifndef STATE_MIGRATION_H
#define STATE_MIGRATION_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "arena.h"

/* ── Field descriptor ──────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    uint32_t    offset;
    uint32_t    size;
} mig_field;

/* ── Stored migration header — lives in arena, persists across reloads ── */

#define MIG_MAX_FIELDS 256
#define MIG_NAME_MAX   64

typedef struct mig_header {
    uint64_t layout_hash;
    uint32_t struct_size;
    uint32_t field_count;
    uint32_t field_offsets[MIG_MAX_FIELDS];
    uint32_t field_sizes[MIG_MAX_FIELDS];
    char     field_names[MIG_MAX_FIELDS][MIG_NAME_MAX];
} mig_header;

/* ── FNV-1a hash for layout fingerprinting ─────────────────────────────── */

static inline uint64_t mig_compute_hash(const mig_field *fields, uint32_t count) {
    uint64_t h = 14695981039346656037ULL;
    uint32_t i;
    for (i = 0; i < count; i++) {
        const char *p = fields[i].name;
        while (*p) { h ^= (uint64_t)(unsigned char)*p++; h *= 1099511628211ULL; }
        h ^= (uint64_t)fields[i].offset; h *= 1099511628211ULL;
        h ^= (uint64_t)fields[i].size;   h *= 1099511628211ULL;
    }
    return h;
}

/* ── Store current field table into arena ──────────────────────────────── */

static inline mig_header *mig_store(arena *a, const mig_field *fields,
                                     uint32_t field_count, uint32_t struct_size,
                                     uint64_t layout_hash, const char *tag) {
    uint32_t i;
    mig_header *hdr = (mig_header *)arena_alloc(a, (uint32_t)sizeof(mig_header), 8, tag);
    if (!hdr) return NULL;
    hdr->layout_hash = layout_hash;
    hdr->struct_size = struct_size;
    hdr->field_count = field_count < MIG_MAX_FIELDS ? field_count : MIG_MAX_FIELDS;
    for (i = 0; i < hdr->field_count; i++) {
        hdr->field_offsets[i] = fields[i].offset;
        hdr->field_sizes[i]  = fields[i].size;
        strncpy(hdr->field_names[i], fields[i].name, MIG_NAME_MAX - 1);
        hdr->field_names[i][MIG_NAME_MAX - 1] = '\0';
    }
    return hdr;
}

/* ── Migrate a single struct by field name ─────────────────────────────── */

static inline int mig_migrate_struct(void *dst, const mig_field *new_fields,
                                      uint32_t new_count, uint32_t new_size,
                                      const void *old_data, const mig_header *old_hdr) {
    uint32_t i, j;
    int migrated = 0;
    for (i = 0; i < new_count; i++) {
        for (j = 0; j < old_hdr->field_count; j++) {
            if (new_fields[i].size == old_hdr->field_sizes[j] &&
                strcmp(new_fields[i].name, old_hdr->field_names[j]) == 0) {
                memcpy((uint8_t *)dst + new_fields[i].offset,
                       (const uint8_t *)old_data + old_hdr->field_offsets[j],
                       new_fields[i].size);
                migrated++;
                break;
            }
        }
    }
    return migrated;
}

/* ── Migrate a component array by field name ───────────────────────────── */

static inline int mig_migrate_array(void *new_arr, const mig_field *new_fields,
                                     uint32_t new_field_count, uint32_t new_elem_size,
                                     const void *old_arr, const mig_header *old_hdr,
                                     int elem_count) {
    int e, migrated = 0;
    uint32_t i, j;
    for (e = 0; e < elem_count; e++) {
        const uint8_t *old_elem = (const uint8_t *)old_arr + (uint32_t)e * old_hdr->struct_size;
        uint8_t *new_elem = (uint8_t *)new_arr + (uint32_t)e * new_elem_size;
        for (i = 0; i < new_field_count; i++) {
            for (j = 0; j < old_hdr->field_count; j++) {
                if (new_fields[i].size == old_hdr->field_sizes[j] &&
                    strcmp(new_fields[i].name, old_hdr->field_names[j]) == 0) {
                    memcpy(new_elem + new_fields[i].offset,
                           old_elem + old_hdr->field_offsets[j],
                           new_fields[i].size);
                    migrated++;
                    break;
                }
            }
        }
    }
    return migrated;
}

#endif /* STATE_MIGRATION_H */
