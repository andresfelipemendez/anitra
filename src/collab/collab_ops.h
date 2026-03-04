#ifndef COLLAB_OPS_H
#define COLLAB_OPS_H

#include <stdint.h>
#include <string.h>

/* ── Forward declarations (avoid circular includes with game.h) ── */
struct game_state;

/* ── Protocol Constants ── */

#define COLLAB_MAX_CLIENTS     4
#define COLLAB_OP_MAX_PAYLOAD  256
#define COLLAB_MAX_ENTITY_NAME 64
#define COLLAB_COMP_MAX        512   /* must match PROJECT_COMP_MAX */

/* ── Operation Types ── */

typedef enum {
    OP_SET_TRANSFORM,         /* Vec3 position */
    OP_SET_ROTATION,          /* float rotation_y_deg */
    OP_SET_SCALE,             /* Vec3 scale */
    OP_SET_CAPSULE_RADIUS,    /* float radius */
    OP_SET_CAPSULE_HEIGHT,    /* float half_height */
    OP_SET_PARENT,            /* int parent_entity_index */
    OP_COUNT
} collab_op_type;

/* ── Operation Payloads ── */

typedef struct { float x, y, z; }        op_vec3_payload;
typedef struct { float value; }           op_float_payload;
typedef struct { int32_t parent_entity; } op_parent_payload;

/* ── Operation ── */

typedef struct collab_op {
    uint32_t seq;            /* server-assigned sequence number (0 = unassigned) */
    uint32_t client_id;      /* originating client */
    uint32_t type;           /* collab_op_type */
    int32_t  entity_index;   /* target entity */
    uint16_t payload_len;
    uint8_t  payload[COLLAB_OP_MAX_PAYLOAD];
} collab_op;

/* ── Message Types (wire protocol) ── */

typedef enum {
    MSG_JOIN     = 1,   /* client -> server: request to join */
    MSG_WELCOME  = 2,   /* server -> client: client_id + color assigned */
    MSG_SNAPSHOT = 3,   /* server -> client: full scene state */
    MSG_OP       = 4,   /* bidirectional: a scene operation */
    MSG_ACK      = 5,   /* server -> client: ack with server seq */
    MSG_CURSOR   = 6,   /* bidirectional: selection/presence */
    MSG_LEAVE    = 7    /* server -> clients: a client disconnected */
} collab_msg_type;

/* ── Wire Message Header ── */
/* Every message on the wire:
   [4 bytes: total_len (including these 8 header bytes)]
   [4 bytes: msg_type]
   [N bytes: payload]
*/
#define COLLAB_MSG_HEADER_SIZE 8

/* ── MSG_JOIN payload ── */
typedef struct {
    char name[64];          /* user display name */
} msg_join_payload;

/* ── MSG_WELCOME payload ── */
typedef struct {
    uint32_t client_id;
    uint32_t color;         /* RGB packed: 0xRRGGBB */
    uint32_t current_seq;   /* latest server sequence number */
} msg_welcome_payload;

/* ── MSG_CURSOR payload ── */
typedef struct {
    uint32_t client_id;
    int32_t  selected_entity;  /* -1 = nothing selected */
} msg_cursor_payload;

/* ── MSG_LEAVE payload ── */
typedef struct {
    uint32_t client_id;
} msg_leave_payload;

/* ── MSG_ACK payload ── */
typedef struct {
    uint32_t server_seq;    /* the server-assigned sequence for the acked op */
} msg_ack_payload;

/* ── Inline serialization functions (used by both client and server) ── */

static inline int collab_write_header(uint8_t *buf, uint32_t total_len, collab_msg_type type) {
    uint32_t t = (uint32_t)type;
    memcpy(buf, &total_len, 4);
    memcpy(buf + 4, &t, 4);
    return COLLAB_MSG_HEADER_SIZE;
}

static inline int collab_read_header(const uint8_t *buf, int buf_len,
                                     uint32_t *out_total_len, collab_msg_type *out_type) {
    uint32_t t;
    if (buf_len < COLLAB_MSG_HEADER_SIZE) return -1;
    memcpy(out_total_len, buf, 4);
    memcpy(&t, buf + 4, 4);
    *out_type = (collab_msg_type)t;
    return 0;
}

static inline int collab_op_to_msg(const collab_op *op, uint8_t *buf, int buf_size) {
    /* Layout: [header:8][seq:4][client_id:4][type:4][entity_index:4][payload_len:2][payload:N] */
    int payload_offset = COLLAB_MSG_HEADER_SIZE + 4 + 4 + 4 + 4 + 2;
    int total = payload_offset + op->payload_len;
    if (buf_size < total) return 0;

    collab_write_header(buf, (uint32_t)total, MSG_OP);
    memcpy(buf + 8,  &op->seq, 4);
    memcpy(buf + 12, &op->client_id, 4);
    memcpy(buf + 16, &op->type, 4);
    memcpy(buf + 20, &op->entity_index, 4);
    memcpy(buf + 24, &op->payload_len, 2);
    if (op->payload_len > 0)
        memcpy(buf + 26, op->payload, op->payload_len);
    return total;
}

static inline int collab_op_from_msg(collab_op *op, const uint8_t *payload, int payload_len) {
    int min_size = 4 + 4 + 4 + 4 + 2;
    if (payload_len < min_size) return -1;

    memcpy(&op->seq, payload, 4);
    memcpy(&op->client_id, payload + 4, 4);
    memcpy(&op->type, payload + 8, 4);
    memcpy(&op->entity_index, payload + 12, 4);
    memcpy(&op->payload_len, payload + 16, 2);

    if (op->payload_len > COLLAB_OP_MAX_PAYLOAD) return -1;
    if (payload_len < min_size + op->payload_len) return -1;
    if (op->payload_len > 0)
        memcpy(op->payload, payload + 18, op->payload_len);
    return 0;
}

static inline int collab_snapshot_max_size(void) {
    /* Conservative upper bound — enough for 512 entities with all component types.
       Actual size depends on sizeof(transform_component) etc., but 512KB is safe. */
    return 512 * 1024;
}

/* ── Functions that require game.h (implemented in collab_ops.c) ── */

/* Apply a single operation to game_state. */
void collab_op_apply(const collab_op *op, struct game_state *gs);

/* Serialize the relevant parts of game_state into a snapshot buffer.
   Returns bytes written, or -1 if buf_size is too small. */
int collab_snapshot_serialize(const struct game_state *gs, uint8_t *buf, int buf_size);

/* Deserialize a snapshot and overwrite the corresponding parts of game_state.
   Returns 0 on success, -1 on error. */
int collab_snapshot_deserialize(struct game_state *gs, const uint8_t *buf, int buf_len);

#endif /* COLLAB_OPS_H */
