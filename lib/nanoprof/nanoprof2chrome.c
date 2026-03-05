/*
 * nanoprof2chrome.c — Convert nanoprof binary to Chrome about://tracing JSON
 * Ported from nanoprof2chrome.d (Boost License 1.0)
 *
 * Usage: nanoprof2chrome < input.nanoprof > output.json
 *   or:  nanoprof2chrome input.nanoprof > output.json
 *
 * Then open about://tracing in Chrome/Chromium and load the JSON.
 *
 * Note: nanoprof measures time in nanoseconds, but Chrome tracing uses
 * microseconds. We represent a nanosecond as a microsecond in output so
 * the viewer shows full resolution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

/* TCC/msvcrt on Windows doesn't support %llu — use %I64u instead */
#if defined(_WIN32) && (defined(__TINYC__) || !defined(__MINGW64__))
#define FMT_U64 "%I64u"
#else
#define FMT_U64 "%llu"
#endif

/* ── nanoprof constants ─────────────────────────────────────────────── */

#define NANOPROF_TIMEPOINT_ID_MAX_USER  0x3BFF
#define NANOPROF_TIMEPOINT_ID_MIN_END   (NANOPROF_TIMEPOINT_ID_MAX_USER + 1)
#define NANOPROF_TIMEPOINT_ID_MAX_END   (NANOPROF_TIMEPOINT_ID_MIN_END + 255)

#define MAX_IDS        (1 << 14)
#define READ_BUF_SIZE  (64 * 1024)
#define MAX_NAME_LEN   256

/* ── Event types ────────────────────────────────────────────────────── */

enum event_type {
    EVT_TIMEPOINT,
    EVT_HIGHPREC,
    EVT_DATA,
    EVT_IDREGISTER,
    EVT_PADDING,
    EVT_UNKNOWN,
    EVT_EOF
};

/* ── Registered timepoint ID ────────────────────────────────────────── */

typedef struct {
    char    name[MAX_NAME_LEN];
    int8_t  level;
    int     registered;
} registered_id;

/* ── Timepoint event ────────────────────────────────────────────────── */

typedef struct {
    uint64_t time_delta;
    uint32_t id;
    char     name[MAX_NAME_LEN];
    int8_t   level;
    uint64_t nsecs;
} timepoint_event;

static int tp_is_end(const timepoint_event *tp)
{
    return tp->id >= NANOPROF_TIMEPOINT_ID_MIN_END
        && tp->id <= NANOPROF_TIMEPOINT_ID_MAX_END;
}

/* ── Buffered binary reader ─────────────────────────────────────────── */

typedef struct {
    uint8_t        *buf;
    size_t          buf_cap;
    size_t          buf_len;
    size_t          offset;
    int             eof;
    FILE           *fp;
    registered_id  *ids;   /* heap-allocated, MAX_IDS entries */
} reader;

static void reader_init(reader *r, FILE *fp)
{
    memset(r, 0, sizeof(*r));
    r->buf_cap = READ_BUF_SIZE * 4;
    r->buf     = (uint8_t *)malloc(r->buf_cap);
    r->ids     = (registered_id *)calloc(MAX_IDS, sizeof(registered_id));
    r->fp      = fp;
}

static void reader_free(reader *r)
{
    free(r->buf);
    free(r->ids);
}

static void reader_fill(reader *r)
{
    size_t remaining, n;
    if (r->eof) return;

    remaining = r->buf_len - r->offset;
    if (r->offset > 0 && remaining > 0)
        memmove(r->buf, r->buf + r->offset, remaining);
    r->buf_len = remaining;
    r->offset  = 0;

    /* Grow buffer if needed */
    if (r->buf_len + READ_BUF_SIZE > r->buf_cap) {
        r->buf_cap = r->buf_cap * 2;
        r->buf = (uint8_t *)realloc(r->buf, r->buf_cap);
    }

    n = fread(r->buf + r->buf_len, 1, READ_BUF_SIZE, r->fp);
    if (n == 0) r->eof = 1;
    r->buf_len += n;
}

static uint8_t *reader_peek(reader *r, size_t size)
{
    while (!r->eof && r->offset + size > r->buf_len)
        reader_fill(r);
    if (r->offset + size <= r->buf_len)
        return r->buf + r->offset;
    return NULL;
}

static uint8_t *reader_get(reader *r, size_t size)
{
    uint8_t *p = reader_peek(r, size);
    if (p) r->offset += size;
    return p;
}

/*
 * Read bytes until terminator, return pointer and length (excluding terminator).
 * Also consumes the terminator byte itself.
 */
static uint8_t *reader_get_until(reader *r, uint8_t terminator, size_t *out_len)
{
    size_t search_start = r->offset;

    for (;;) {
        size_t i;
        for (i = search_start; i < r->buf_len; i++) {
            if (r->buf[i] == terminator) {
                uint8_t *p = r->buf + r->offset;
                *out_len   = i - r->offset;
                r->offset  = i + 1; /* skip terminator */
                return p;
            }
        }
        search_start = r->buf_len;
        if (r->eof) return NULL;
        reader_fill(r);
    }
}

/* ── Event type detection ───────────────────────────────────────────── */

/*
 * Prefixes (from MSB):
 *   0b0_______  → Timepoint (most common)
 *   0b1010____  → Data
 *   0b11111101  → HighPrec
 *   0b11111110  → IDRegister
 *   0b11111111  → Padding
 */
static enum event_type reader_next_type(reader *r)
{
    uint8_t *p = reader_peek(r, 1);
    if (!p) return EVT_EOF;

    if ((p[0] >> 7) == 0)    return EVT_TIMEPOINT;
    if ((p[0] >> 4) == 0x0A) return EVT_DATA;
    switch (p[0]) {
        case 0xFD: return EVT_HIGHPREC;
        case 0xFE: return EVT_IDREGISTER;
        case 0xFF: return EVT_PADDING;
        default:   return EVT_UNKNOWN;
    }
}

/* ── Event readers ──────────────────────────────────────────────────── */

/*
 * Timepoint encoding (variable-length):
 *   2B: [7-bit ID] [0 + 7-bit delta]           — ID < 128, delta < 128
 *   4B: [7-bit hi] [1 + 7-bit lo] [0 + 15-bit delta] — 14-bit ID
 *   8B: [7-bit hi] [1 + 7-bit lo] [1 + 47-bit delta] — 14-bit ID, large delta
 */
static int read_timepoint(reader *r, timepoint_event *out)
{
    uint8_t *b2 = reader_get(r, 2);
    if (!b2) return 0;

    if ((b2[1] & 0x80) == 0) {
        /* 2-byte format */
        out->id         = b2[0];
        out->time_delta = b2[1];
    } else {
        /* 4B or 8B — compose 14-bit ID */
        uint8_t *peek1;
        out->id = ((uint32_t)b2[0] << 7) | (b2[1] & 0x7F);

        peek1 = reader_peek(r, 1);
        if (!peek1) return 0;

        if ((peek1[0] & 0x80) == 0) {
            /* 4-byte format: 2B time delta */
            uint8_t *bt = reader_get(r, 2);
            if (!bt) return 0;
            out->time_delta = ((uint64_t)bt[0] << 8) | bt[1];
        } else {
            /* 8-byte format: 6B time delta */
            uint8_t *bt = reader_get(r, 6);
            if (!bt) return 0;
            out->time_delta =
                (((uint64_t)(bt[0] & 0x7F)) << 40) |
                ((uint64_t)bt[1] << 32) |
                ((uint64_t)bt[2] << 24) |
                ((uint64_t)bt[3] << 16) |
                ((uint64_t)bt[4] << 8)  |
                 (uint64_t)bt[5];
        }
    }

    /* Look up registered name and level */
    if (out->id < MAX_IDS && r->ids[out->id].registered) {
        memcpy(out->name, r->ids[out->id].name, MAX_NAME_LEN);
        out->level = r->ids[out->id].level;
    } else {
        strcpy(out->name, "Unregistered");
        out->level = -127;
    }

    return 1;
}

static int read_highprec(reader *r, uint64_t *nsecs)
{
    uint8_t *b = reader_get(r, 8);
    if (!b) return 0;
    /* byte[0] is 0xFD marker, bytes[1..7] are 56-bit nanosecond timestamp */
    *nsecs =
        ((uint64_t)b[1] << 48) |
        ((uint64_t)b[2] << 40) | ((uint64_t)b[3] << 32) |
        ((uint64_t)b[4] << 24) | ((uint64_t)b[5] << 16) |
        ((uint64_t)b[6] << 8)  |  (uint64_t)b[7];
    return 1;
}

/* Read and discard a data event (we don't emit these to Chrome format). */
static int read_data(reader *r)
{
    uint8_t *hdr = reader_get(r, 3);
    uint8_t type;
    if (!hdr) return 0;

    type = hdr[0] & 0x0F;
    switch (type) {
        case 0x0: return 0; /* unknown */
        case 0x1: return reader_get(r, 1) != NULL; /* u8  */
        case 0x2: return reader_get(r, 2) != NULL; /* u16 */
        case 0x3: return reader_get(r, 4) != NULL; /* u32 */
        case 0x4: return reader_get(r, 8) != NULL; /* u64 */
        case 0x5: return reader_get(r, 4) != NULL; /* f32 */
        case 0x6: return reader_get(r, 8) != NULL; /* f64 */
        case 0xF: { /* string — null terminated */
            size_t len;
            return reader_get_until(r, 0, &len) != NULL;
        }
        default: return 0;
    }
}

static int read_id_register(reader *r)
{
    uint8_t *b;
    int8_t   level;
    uint8_t  id_type;
    uint32_t id;
    size_t   name_len, copy_len;
    uint8_t *name;

    b = reader_get(r, 5);
    if (!b) return 0;

    level   = (int8_t)b[1];
    id_type = b[2];
    id      = (((uint32_t)(b[3] & 0x3F)) << 8) | b[4];

    /* Read null-terminated name string */
    name = reader_get_until(r, 0, &name_len);
    if (!name) return 0;

    /* Only register timepoint IDs (type 0x01) */
    if (id_type == 0x01 && id < MAX_IDS) {
        copy_len = name_len < MAX_NAME_LEN - 1 ? name_len : MAX_NAME_LEN - 1;
        memcpy(r->ids[id].name, name, copy_len);
        r->ids[id].name[copy_len] = '\0';
        r->ids[id].level      = level;
        r->ids[id].registered = 1;
    }

    return 1;
}

/* ── Dynamic timepoint array ────────────────────────────────────────── */

typedef struct {
    timepoint_event *data;
    size_t           len;
    size_t           cap;
} tp_array;

static void tp_push(tp_array *a, const timepoint_event *tp)
{
    if (a->len >= a->cap) {
        a->cap = a->cap ? a->cap * 2 : 64;
        a->data = (timepoint_event *)realloc(a->data,
                    a->cap * sizeof(timepoint_event));
    }
    a->data[a->len++] = *tp;
}

/* ── Output ─────────────────────────────────────────────────────────── */

static int      first_scope       = 1;
static uint64_t program_start_ns  = 0;

static void write_scope(const timepoint_event *start, uint64_t nsecs_end)
{
    if (!first_scope) printf(",\n");
    first_scope = 0;

    /* Chrome tracing uses microseconds; nanoprof stores nanoseconds */
    printf("{\"cat\":\"nanoprof\",\"pid\":\"Anitra\",\"tid\":\"main\","
           "\"ph\":\"X\",\"name\":\"%s\","
           "\"ts\":" FMT_U64 ",\"dur\":" FMT_U64 "}",
           start->name,
           (unsigned long long)((start->nsecs - program_start_ns) / 1000),
           (unsigned long long)((nsecs_end - start->nsecs) / 1000));
}

/*
 * Try to close a scope: find the first timepoint at same or lower level
 * in the candidates array. Write the scope if found.
 */
static int close_scope(const timepoint_event *start,
                       const timepoint_event *candidates, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (start->level == -128) {
            /* Level -128 scopes can only end at level -128 */
            if (candidates[i].level == -128) {
                write_scope(start, candidates[i].nsecs);
                return 1;
            }
        } else {
            if (candidates[i].level != -128
                && candidates[i].level <= start->level) {
                write_scope(start, candidates[i].nsecs);
                return 1;
            }
        }
    }
    return 0;
}

/*
 * Handle a HighPrec event: compute exact nanosecond timestamps for all
 * buffered timepoints using interpolation between the two HighPrec
 * timestamps, then close any scopes we can.
 */
static void handle_highprec(uint64_t new_hp, uint64_t *prev_hp, int *have_prev,
                            tp_array *tp_buf, tp_array *pending)
{
    size_t i, dst;
    uint64_t total_psecs, total_delta, time_unit_psecs, cumulative_psecs;

    if (!*have_prev) {
        program_start_ns = new_hp;
        *prev_hp   = new_hp;
        *have_prev = 1;
        tp_buf->len = 0;
        return;
    }

    if (tp_buf->len == 0) {
        *prev_hp = new_hp;
        return;
    }

    /* Compute time in picoseconds between the two HighPrec events */
    total_psecs = 1000ULL * (new_hp - *prev_hp);

    /* Sum all time deltas in the buffer */
    total_delta = 0;
    for (i = 0; i < tp_buf->len; i++)
        total_delta += tp_buf->data[i].time_delta;

    if (total_delta > 0) {
        time_unit_psecs  = total_psecs / total_delta;
        cumulative_psecs = 0;

        for (i = 0; i < tp_buf->len; i++) {
            cumulative_psecs += tp_buf->data[i].time_delta * time_unit_psecs;
            tp_buf->data[i].nsecs = *prev_hp + (cumulative_psecs / 1000);
        }
    }

    /* Try to close previously-pending scopes against new timepoints */
    dst = 0;
    for (i = 0; i < pending->len; i++) {
        if (!close_scope(&pending->data[i], tp_buf->data, tp_buf->len))
            pending->data[dst++] = pending->data[i];
    }
    pending->len = dst;

    /* Try to close scopes started by timepoints in this buffer */
    for (i = 0; i < tp_buf->len; i++) {
        if (!tp_is_end(&tp_buf->data[i])) {
            if (!close_scope(&tp_buf->data[i],
                             tp_buf->data + i + 1,
                             tp_buf->len - i - 1)) {
                tp_push(pending, &tp_buf->data[i]);
            }
        }
    }

    *prev_hp    = new_hp;
    tp_buf->len = 0;
}

/* ── main ───────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    reader   rd;
    tp_array tp_buf      = {0};
    tp_array pending     = {0};
    int      have_prev   = 0;
    uint64_t prev_hp     = 0;
    uint64_t pending_hp  = 0;
    int      have_pend_hp = 0;
    FILE    *input;
    size_t   i;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif

    /* Accept filename argument or read from stdin */
    if (argc > 1 && strcmp(argv[1], "--help") != 0 && strcmp(argv[1], "-h") != 0) {
        input = fopen(argv[1], "rb");
        if (!input) {
            fprintf(stderr, "Cannot open %s\n", argv[1]);
            return 1;
        }
    } else if (argc > 1) {
        fprintf(stderr,
            "Usage: nanoprof2chrome [INPUT] > OUTPUT.json\n"
            "  Reads nanoprof binary from INPUT (or stdin) and writes\n"
            "  Chrome about://tracing JSON to stdout.\n");
        return 0;
    } else {
        input = stdin;
    }

    reader_init(&rd, input);

    printf("[\n");

    for (;;) {
        enum event_type evt = reader_next_type(&rd);
        if (evt == EVT_EOF) break;

        switch (evt) {
        case EVT_TIMEPOINT: {
            timepoint_event tp;
            memset(&tp, 0, sizeof(tp));
            if (!read_timepoint(&rd, &tp)) {
                fprintf(stderr, "ERROR: Truncated Timepoint event\n");
                goto done;
            }
            tp_push(&tp_buf, &tp);

            if (have_pend_hp) {
                handle_highprec(pending_hp, &prev_hp, &have_prev,
                                &tp_buf, &pending);
                have_pend_hp = 0;
            }
            break;
        }
        case EVT_HIGHPREC: {
            uint64_t nsecs;
            if (!read_highprec(&rd, &nsecs)) {
                fprintf(stderr, "ERROR: Truncated HighPrec event\n");
                goto done;
            }
            pending_hp   = nsecs;
            have_pend_hp = 1;
            break;
        }
        case EVT_DATA:
            if (!read_data(&rd)) {
                fprintf(stderr, "ERROR: Truncated Data event\n");
                goto done;
            }
            break;
        case EVT_IDREGISTER:
            if (!read_id_register(&rd)) {
                fprintf(stderr, "ERROR: Truncated IDRegister event\n");
                goto done;
            }
            break;
        case EVT_PADDING:
            reader_get(&rd, 1);
            break;
        case EVT_UNKNOWN:
            fprintf(stderr, "ERROR: Unknown event type 0x%02x\n",
                    *reader_peek(&rd, 1));
            goto done;
        default:
            break;
        }
    }

    /* Close remaining open scopes against the last known timestamp */
    if (have_prev) {
        for (i = 0; i < pending.len; i++)
            write_scope(&pending.data[i], prev_hp);
    }

    /* ── Merge boot_threads.json if it sits next to the input file ──── */
    if (argc > 1) {
        /* Build path: replace filename with boot_threads.json */
        const char *inp = argv[1];
        const char *sep = inp;
        const char *p;
        char threads_path[1024];
        FILE *tf;
        for (p = inp; *p; p++) {
            if (*p == '/' || *p == '\\') sep = p + 1;
        }
        if (sep > inp) {
            size_t dir_len = (size_t)(sep - inp);
            if (dir_len + 20 < sizeof(threads_path)) {
                memcpy(threads_path, inp, dir_len);
                strcpy(threads_path + dir_len, "boot_threads.json");
            } else {
                threads_path[0] = '\0';
            }
        } else {
            strcpy(threads_path, "boot_threads.json");
        }

        tf = fopen(threads_path, "r");
        if (tf) {
            /* Read the entire file and splice events into our JSON array.
               boot_threads.json is a JSON array: [ {event}, ... ]
               We strip the outer [ ] and emit each event with a leading comma. */
            char buf[4096];
            size_t n;
            int inside = 0;  /* have we seen the opening '[' ? */
            while ((n = fread(buf, 1, sizeof(buf), tf)) > 0) {
                size_t j;
                for (j = 0; j < n; j++) {
                    char c = buf[j];
                    if (!inside) {
                        if (c == '[') { inside = 1; printf(",\n"); }
                        continue;
                    }
                    /* Stop at closing ']' */
                    if (c == ']') goto threads_done;
                    putchar(c);
                }
            }
        threads_done:
            fclose(tf);
            fprintf(stderr, "Merged thread traces from %s\n", threads_path);
        }
    }

done:
    printf("\n]\n");

    free(tp_buf.data);
    free(pending.data);
    reader_free(&rd);
    if (input != stdin) fclose(input);

    return 0;
}
