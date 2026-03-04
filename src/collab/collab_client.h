#ifndef COLLAB_CLIENT_H
#define COLLAB_CLIENT_H

#include "collab_ops.h"

/* ── Client State ── */

#define COLLAB_SEND_QUEUE_MAX  64
#define COLLAB_RECV_BUF_SIZE   65536
#define COLLAB_SEND_BUF_SIZE   65536

/* Peer presence info */
typedef struct collab_peer {
    int      active;
    uint32_t client_id;
    uint32_t color;           /* 0xRRGGBB */
    int32_t  selected_entity; /* -1 = none */
} collab_peer;

typedef struct collab_state {
    /* Socket handle — stored as int so it survives hot-reload.
       On Windows, SOCKET is UINT_PTR but fits in int for our use.
       Value of -1 means disconnected. */
    int socket;

    uint32_t client_id;
    uint32_t my_color;
    int connected;
    int connecting;         /* 1 while waiting for MSG_WELCOME */

    /* Send queue — operations coalesced per (entity, type) */
    collab_op send_queue[COLLAB_SEND_QUEUE_MAX];
    int send_queue_count;

    /* Raw send buffer for partially-sent data */
    uint8_t send_raw[COLLAB_SEND_BUF_SIZE];
    int send_raw_len;

    /* Receive buffer — accumulates partial TCP reads */
    uint8_t recv_buf[COLLAB_RECV_BUF_SIZE];
    uint32_t recv_len;

    /* Server address */
    char server_host[256];
    uint16_t server_port;

    /* Local sequence counter (pre-server assignment) */
    uint32_t local_seq;

    /* Peer presence */
    collab_peer peers[COLLAB_MAX_CLIENTS];

    /* Connection status message for UI */
    char status_msg[128];

    /* Snapshot received flag — set to 1 after initial sync */
    int snapshot_received;
} collab_state;

/* ── API ── */

/* Initialize collab state (zero everything, set socket to -1) */
void collab_init(collab_state *cs);

/* Non-blocking connect to server. Returns 0 on success (connection initiated), -1 on error. */
int collab_connect(collab_state *cs, const char *host, uint16_t port);

/* Disconnect from server. */
void collab_disconnect(collab_state *cs);

/* Per-frame update: non-blocking recv, process messages, flush send queue.
   Must be called from update_editor(). */
void collab_update(collab_state *cs, struct game_state *gs);

/* Queue an operation for sending. Coalesces by (entity_index, type). */
void collab_emit_op(collab_state *cs, collab_op_type type,
                    int entity_index, const void *payload, uint16_t payload_len);

/* Send cursor/selection update to peers. */
void collab_send_cursor(collab_state *cs, int selected_entity);

#endif /* COLLAB_CLIENT_H */
