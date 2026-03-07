#include "collab_client.h"
#include "game.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #define COLLAB_INVALID_SOCKET -1
  #define COLLAB_SOCK(s) ((SOCKET)(s))
  #define collab_closesocket(s) closesocket(COLLAB_SOCK(s))
  #define collab_wouldblock() (WSAGetLastError() == WSAEWOULDBLOCK)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  #define COLLAB_INVALID_SOCKET -1
  #define COLLAB_SOCK(s) (s)
  #define collab_closesocket(s) close(s)
  #define collab_wouldblock() (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

/* ── Init ── */

void collab_init(collab_state *cs) {
    memset(cs, 0, sizeof(collab_state));
    cs->socket = COLLAB_INVALID_SOCKET;
    cs->connected = 0;
    cs->connecting = 0;
    cs->snapshot_received = 0;
    cs->pending_count = 0;
    {
        int i;
        for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
            cs->peers[i].active = 0;
            cs->peers[i].selected_entity = -1;
        }
    }
    snprintf(cs->status_msg, sizeof(cs->status_msg), "Disconnected");
}

/* ── Platform socket setup ── */

static int collab_socket_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static int collab_set_nonblocking(int sock) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(COLLAB_SOCK(sock), FIONBIO, &mode) == 0 ? 0 : -1;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

/* ── Connect ── */

int collab_connect(collab_state *cs, const char *host, uint16_t port) {
    struct sockaddr_in addr;
    int sock;
    msg_join_payload join;

    if (cs->connected || cs->connecting) {
        collab_disconnect(cs);
    }

    collab_socket_init();

    sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        snprintf(cs->status_msg, sizeof(cs->status_msg), "Failed to create socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    /* Resolve host (handles both IP strings and hostnames) */
    {
        struct addrinfo hints, *res;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
            collab_closesocket(sock);
            snprintf(cs->status_msg, sizeof(cs->status_msg), "Cannot resolve host: %s", host);
            return -1;
        }
        addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    /* Blocking connect (brief — server should accept quickly) */
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        collab_closesocket(sock);
        snprintf(cs->status_msg, sizeof(cs->status_msg), "Cannot connect to %s:%d", host, port);
        return -1;
    }

    /* Switch to non-blocking for recv/send during game loop */
    collab_set_nonblocking(sock);

    cs->socket = sock;
    cs->connecting = 1;
    cs->connected = 0;
    cs->recv_len = 0;
    cs->send_raw_len = 0;
    cs->send_queue_count = 0;
    cs->local_seq = 0;
    cs->snapshot_received = 0;
    strncpy(cs->server_host, host, sizeof(cs->server_host) - 1);
    cs->server_port = port;

    snprintf(cs->status_msg, sizeof(cs->status_msg), "Connecting to %s:%d...", host, port);

    /* Send MSG_JOIN */
    {
        uint8_t buf[COLLAB_MSG_HEADER_SIZE + sizeof(msg_join_payload)];
        int total = COLLAB_MSG_HEADER_SIZE + (int)sizeof(msg_join_payload);
        memset(&join, 0, sizeof(join));
        snprintf(join.name, sizeof(join.name), "Editor");
        collab_write_header(buf, (uint32_t)total, MSG_JOIN);
        memcpy(buf + COLLAB_MSG_HEADER_SIZE, &join, sizeof(join));
        send(COLLAB_SOCK(cs->socket), (const char *)buf, total, 0);
    }

    return 0;
}

/* ── Disconnect ── */

void collab_disconnect(collab_state *cs) {
    if (cs->socket != COLLAB_INVALID_SOCKET) {
        collab_closesocket(cs->socket);
        cs->socket = COLLAB_INVALID_SOCKET;
    }
    cs->connected = 0;
    cs->connecting = 0;
    cs->snapshot_received = 0;
    cs->recv_len = 0;
    cs->send_raw_len = 0;
    cs->send_queue_count = 0;
    cs->pending_count = 0;
    {
        int i;
        for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
            cs->peers[i].active = 0;
            cs->peers[i].selected_entity = -1;
        }
    }
    snprintf(cs->status_msg, sizeof(cs->status_msg), "Disconnected");
}

/* ── Emit Operation (coalesced send queue) ── */

void collab_emit_op(collab_state *cs, collab_op_type type,
                    int entity_index, const void *payload, uint16_t payload_len) {
    int i;
    collab_op *slot;

    if (!cs->connected || payload_len > COLLAB_OP_MAX_PAYLOAD) return;

    /* Coalesce: overwrite existing entry with same (entity, type) */
    for (i = 0; i < cs->send_queue_count; i++) {
        if (cs->send_queue[i].entity_index == entity_index &&
            (collab_op_type)cs->send_queue[i].type == type) {
            memcpy(cs->send_queue[i].payload, payload, payload_len);
            cs->send_queue[i].payload_len = payload_len;
            return;
        }
    }

    /* Add new entry */
    if (cs->send_queue_count >= COLLAB_SEND_QUEUE_MAX) return; /* queue full, drop */

    slot = &cs->send_queue[cs->send_queue_count++];
    slot->seq = 0; /* server will assign */
    slot->client_id = cs->client_id;
    slot->type = (uint32_t)type;
    slot->entity_index = entity_index;
    slot->payload_len = payload_len;
    memcpy(slot->payload, payload, payload_len);
}

/* ── Send cursor/selection ── */

void collab_send_cursor(collab_state *cs, int selected_entity) {
    uint8_t buf[COLLAB_MSG_HEADER_SIZE + sizeof(msg_cursor_payload)];
    int total;
    msg_cursor_payload cur;

    if (!cs->connected) return;

    cur.client_id = cs->client_id;
    cur.selected_entity = selected_entity;
    total = COLLAB_MSG_HEADER_SIZE + (int)sizeof(msg_cursor_payload);
    collab_write_header(buf, (uint32_t)total, MSG_CURSOR);
    memcpy(buf + COLLAB_MSG_HEADER_SIZE, &cur, sizeof(cur));
    send(COLLAB_SOCK(cs->socket), (const char *)buf, total, 0);
}

/* ── Flush send queue ── */

static void collab_flush_queue(collab_state *cs) {
    int i;
    uint8_t msg_buf[512];

    for (i = 0; i < cs->send_queue_count; i++) {
        int bytes = collab_op_to_msg(&cs->send_queue[i], msg_buf, sizeof(msg_buf));
        if (bytes > 0) {
            send(COLLAB_SOCK(cs->socket), (const char *)msg_buf, bytes, 0);
        }
        /* OT: track sent ops as pending until ACKed */
        if (cs->pending_count < COLLAB_PENDING_MAX) {
            cs->pending_ops[cs->pending_count++] = cs->send_queue[i];
        }
    }
    cs->send_queue_count = 0;
}

/* ── Process incoming messages ── */

static void collab_process_message(collab_state *cs, collab_msg_type type,
                                   const uint8_t *payload, int payload_len,
                                   struct game_state *gs) {
    switch (type) {
    case MSG_WELCOME: {
        msg_welcome_payload w;
        if (payload_len < (int)sizeof(w)) break;
        memcpy(&w, payload, sizeof(w));
        cs->client_id = w.client_id;
        cs->my_color = w.color;
        cs->connecting = 0;
        cs->connected = 1;
        snprintf(cs->status_msg, sizeof(cs->status_msg),
                 "Connected (user %u)", w.client_id);
        break;
    }
    case MSG_SNAPSHOT: {
        if (collab_snapshot_deserialize(gs, payload, payload_len) == 0) {
            cs->snapshot_received = 1;
            cs->pending_count = 0;
            cs->send_queue_count = 0;
            snprintf(cs->status_msg, sizeof(cs->status_msg),
                     "Connected - synced (%d entities)", gs->scene_entity_count);
        }
        break;
    }
    case MSG_OP: {
        collab_op op;
        if (collab_op_from_msg(&op, payload, payload_len) == 0) {
            if (op.client_id != cs->client_id) {
                /* OT: transform remote op against all pending local ops.
                   If it conflicts with any pending op (same entity+type),
                   skip it — our pending op will take effect when ACKed. */
                int i;
                for (i = 0; i < cs->pending_count; i++) {
                    if (collab_op_transform(&op, &cs->pending_ops[i]))
                        break;
                }
                if (op.type != OP_NOOP) {
                    collab_op_apply(&op, gs);
                }
            }
        }
        break;
    }
    case MSG_ACK: {
        /* OT: pop the oldest pending op (FIFO — TCP preserves order) */
        if (cs->pending_count > 0) {
            cs->pending_count--;
            if (cs->pending_count > 0) {
                memmove(&cs->pending_ops[0], &cs->pending_ops[1],
                        (size_t)cs->pending_count * sizeof(collab_op));
            }
        }
        break;
    }
    case MSG_CURSOR: {
        msg_cursor_payload cur;
        if (payload_len < (int)sizeof(cur)) break;
        memcpy(&cur, payload, sizeof(cur));
        if (cur.client_id < COLLAB_MAX_CLIENTS && cur.client_id != cs->client_id) {
            cs->peers[cur.client_id].active = 1;
            cs->peers[cur.client_id].client_id = cur.client_id;
            cs->peers[cur.client_id].selected_entity = cur.selected_entity;
        }
        break;
    }
    case MSG_LEAVE: {
        msg_leave_payload lv;
        if (payload_len < (int)sizeof(lv)) break;
        memcpy(&lv, payload, sizeof(lv));
        if (lv.client_id < COLLAB_MAX_CLIENTS) {
            cs->peers[lv.client_id].active = 0;
            cs->peers[lv.client_id].selected_entity = -1;
        }
        break;
    }
    default:
        break;
    }
}

/* ── Per-frame update ── */

void collab_update(collab_state *cs, struct game_state *gs) {
    int bytes;

    if (cs->socket == COLLAB_INVALID_SOCKET) return;

    /* Non-blocking recv */
    bytes = recv(COLLAB_SOCK(cs->socket),
                 (char *)(cs->recv_buf + cs->recv_len),
                 COLLAB_RECV_BUF_SIZE - cs->recv_len, 0);

    if (bytes > 0) {
        cs->recv_len += (uint32_t)bytes;
    } else if (bytes == 0) {
        /* Server closed connection */
        collab_disconnect(cs);
        snprintf(cs->status_msg, sizeof(cs->status_msg), "Server disconnected");
        return;
    } else {
        /* WOULDBLOCK is normal for non-blocking socket, anything else is error */
        if (!collab_wouldblock()) {
            collab_disconnect(cs);
            snprintf(cs->status_msg, sizeof(cs->status_msg), "Connection lost");
            return;
        }
    }

    /* Parse complete messages from recv buffer */
    while (cs->recv_len >= COLLAB_MSG_HEADER_SIZE) {
        uint32_t total_len;
        collab_msg_type msg_type;
        int payload_len;

        if (collab_read_header(cs->recv_buf, cs->recv_len, &total_len, &msg_type) != 0)
            break;

        /* Sanity check */
        if (total_len < COLLAB_MSG_HEADER_SIZE || total_len > COLLAB_RECV_BUF_SIZE) {
            collab_disconnect(cs);
            snprintf(cs->status_msg, sizeof(cs->status_msg), "Protocol error");
            return;
        }

        /* Wait for complete message */
        if (cs->recv_len < total_len) break;

        payload_len = (int)(total_len - COLLAB_MSG_HEADER_SIZE);
        collab_process_message(cs, msg_type,
                               cs->recv_buf + COLLAB_MSG_HEADER_SIZE,
                               payload_len, gs);

        /* Shift buffer */
        cs->recv_len -= total_len;
        if (cs->recv_len > 0)
            memmove(cs->recv_buf, cs->recv_buf + total_len, cs->recv_len);
    }

    /* Flush send queue */
    if (cs->connected) {
        collab_flush_queue(cs);
    }
}
