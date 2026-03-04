/*
 * Anitra Collaborative Editing Relay Server
 *
 * Single-threaded select()-based TCP server.
 * Maintains authoritative scene state and relays operations between editors.
 *
 * Usage: collab_server [--port PORT] [--project PATH]
 *   --port PORT       Listen port (default: 7777)
 *   --project PATH    TOML project file to load initial state from
 */

#include "collab_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#ifdef _WIN32
  #pragma comment(lib, "ws2_32.lib")
  #define server_close(s) closesocket(s)
#else
  #include <errno.h>
  #include <sys/select.h>
  #define server_close(s) close(s)
#endif

static collab_server g_server;

/* ── Signal handler ── */

static void signal_handler(int sig) {
    (void)sig;
    g_server.running = 0;
}

/* ── Platform init ── */

static int server_platform_init(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);
    return 0;
#endif
}

static void server_platform_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

/* ── Utility: send all bytes ── */

static int server_send_all(server_socket_t sock, const uint8_t *data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(sock, (const char *)(data + sent), len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return 0;
}

/* ── Send message to a single client ── */

static int server_send_msg(server_socket_t sock, collab_msg_type type,
                           const void *payload, int payload_len) {
    uint8_t header[COLLAB_MSG_HEADER_SIZE];
    uint32_t total = COLLAB_MSG_HEADER_SIZE + payload_len;
    collab_write_header(header, total, type);
    if (server_send_all(sock, header, COLLAB_MSG_HEADER_SIZE) != 0) return -1;
    if (payload_len > 0 && server_send_all(sock, payload, payload_len) != 0) return -1;
    return 0;
}

/* ── Broadcast message to all clients except sender ── */

static void server_broadcast_except(collab_server *s, uint32_t except_id,
                                    collab_msg_type type,
                                    const void *payload, int payload_len) {
    int i;
    for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
        if (s->clients[i].connected && s->clients[i].client_id != except_id) {
            server_send_msg(s->clients[i].socket, type, payload, payload_len);
        }
    }
}

/* ── Build snapshot from server state (using a temporary game_state-like buffer) ──
   We build a minimal game_state structure on the stack to reuse collab_snapshot_serialize. */

static int server_build_snapshot(collab_server *s, uint8_t *buf, int buf_size) {
    /* We manually serialize since we don't have a full game_state.
       Format must match collab_snapshot_serialize/deserialize. */
    uint8_t *p = buf;
    int32_t val;

    if (buf_size < collab_snapshot_max_size()) return -1;

    /* Entity names */
    val = s->entity_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->entity_names, s->entity_count * 64); p += s->entity_count * 64;

    /* Transforms */
    val = s->transform_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->transforms, s->transform_count * (int)sizeof(transform_component));
    p += s->transform_count * (int)sizeof(transform_component);

    /* Rotations */
    val = s->rotation_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->rotations, s->rotation_count * (int)sizeof(rotation_component));
    p += s->rotation_count * (int)sizeof(rotation_component);

    /* Scales */
    val = s->scale_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->scales, s->scale_count * (int)sizeof(scale_component));
    p += s->scale_count * (int)sizeof(scale_component);

    /* Capsule colliders */
    val = s->capsule_collider_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->capsule_colliders, s->capsule_collider_count * (int)sizeof(capsule_collider_component));
    p += s->capsule_collider_count * (int)sizeof(capsule_collider_component);

    /* Parent transforms */
    val = s->parent_transform_count;
    memcpy(p, &val, 4); p += 4;
    memcpy(p, s->parent_transforms, s->parent_transform_count * (int)sizeof(parent_transform_component));
    p += s->parent_transform_count * (int)sizeof(parent_transform_component);

    /* Index tables */
    memcpy(p, s->transform_index, COLLAB_COMP_MAX * 4); p += COLLAB_COMP_MAX * 4;
    memcpy(p, s->rotation_index, COLLAB_COMP_MAX * 4); p += COLLAB_COMP_MAX * 4;
    memcpy(p, s->scale_index, COLLAB_COMP_MAX * 4); p += COLLAB_COMP_MAX * 4;
    memcpy(p, s->capsule_collider_index, COLLAB_COMP_MAX * 4); p += COLLAB_COMP_MAX * 4;
    memcpy(p, s->parent_transform_index, COLLAB_COMP_MAX * 4); p += COLLAB_COMP_MAX * 4;

    return (int)(p - buf);
}

/* ── Apply operation to server's authoritative state ── */

static void server_apply_op(collab_server *s, const collab_op *op) {
    int eidx = op->entity_index;
    int cidx;
    if (eidx < 0 || eidx >= COLLAB_COMP_MAX) return;

    switch ((collab_op_type)op->type) {
    case OP_SET_TRANSFORM: {
        op_vec3_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = s->transform_index[eidx];
        if (cidx >= 0) {
            s->transforms[cidx].position.x = p.x;
            s->transforms[cidx].position.y = p.y;
            s->transforms[cidx].position.z = p.z;
        }
        break;
    }
    case OP_SET_ROTATION: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = s->rotation_index[eidx];
        if (cidx >= 0)
            s->rotations[cidx].rotation_y_deg = p.value;
        break;
    }
    case OP_SET_SCALE: {
        op_vec3_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = s->scale_index[eidx];
        if (cidx >= 0) {
            s->scales[cidx].scale.x = p.x;
            s->scales[cidx].scale.y = p.y;
            s->scales[cidx].scale.z = p.z;
        }
        break;
    }
    case OP_SET_CAPSULE_RADIUS: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = s->capsule_collider_index[eidx];
        if (cidx >= 0)
            s->capsule_colliders[cidx].radius = p.value;
        break;
    }
    case OP_SET_CAPSULE_HEIGHT: {
        op_float_payload p;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        cidx = s->capsule_collider_index[eidx];
        if (cidx >= 0)
            s->capsule_colliders[cidx].half_height = p.value;
        break;
    }
    case OP_SET_PARENT: {
        op_parent_payload p;
        int i, found = 0;
        if (op->payload_len < sizeof(p)) return;
        memcpy(&p, op->payload, sizeof(p));
        for (i = 0; i < s->parent_transform_count; i++) {
            if (s->parent_transforms[i].entity_index == eidx) {
                if (p.parent_entity < 0) {
                    int j;
                    for (j = i; j < s->parent_transform_count - 1; j++)
                        s->parent_transforms[j] = s->parent_transforms[j + 1];
                    s->parent_transform_count--;
                    s->parent_transform_index[eidx] = -1;
                } else {
                    s->parent_transforms[i].parent_entity_index = p.parent_entity;
                }
                found = 1;
                break;
            }
        }
        if (!found && p.parent_entity >= 0 && s->parent_transform_count < COLLAB_COMP_MAX) {
            int idx = s->parent_transform_count;
            s->parent_transforms[idx].entity_index = eidx;
            s->parent_transforms[idx].parent_entity_index = p.parent_entity;
            s->parent_transform_index[eidx] = idx;
            s->parent_transform_count++;
        }
        break;
    }
    default:
        break;
    }
}

/* ── Load initial state from project TOML ── */

static int server_load_project(collab_server *s, const char *project_path) {
    project_data proj;
    int i;

    memset(&proj, 0, sizeof(proj));
    if (project_load(project_path, &proj) != 0) {
        fprintf(stderr, "Failed to load project: %s\n", project_path);
        return -1;
    }

    /* Entity names */
    s->entity_count = proj.scene_entity_count;
    memcpy(s->entity_names, proj.scene_entity_names, sizeof(s->entity_names));

    /* Initialize all index tables to -1 */
    for (i = 0; i < COLLAB_COMP_MAX; i++) {
        s->transform_index[i] = -1;
        s->rotation_index[i] = -1;
        s->scale_index[i] = -1;
        s->capsule_collider_index[i] = -1;
        s->parent_transform_index[i] = -1;
    }

    /* Transforms */
    s->transform_count = proj.transform_count;
    for (i = 0; i < proj.transform_count; i++) {
        s->transforms[i].entity_index = proj.transforms[i].entity;
        s->transforms[i].position.x = proj.transforms[i].position[0];
        s->transforms[i].position.y = proj.transforms[i].position[1];
        s->transforms[i].position.z = proj.transforms[i].position[2];
        s->transform_index[proj.transforms[i].entity] = i;
    }

    /* Rotations */
    s->rotation_count = proj.rotation_count;
    for (i = 0; i < proj.rotation_count; i++) {
        s->rotations[i].entity_index = proj.rotations[i].entity;
        s->rotations[i].rotation_y_deg = proj.rotations[i].y;
        s->rotation_index[proj.rotations[i].entity] = i;
    }

    /* Scales */
    s->scale_count = proj.scale_count;
    for (i = 0; i < proj.scale_count; i++) {
        s->scales[i].entity_index = proj.scales[i].entity;
        s->scales[i].scale.x = proj.scales[i].value[0];
        s->scales[i].scale.y = proj.scales[i].value[1];
        s->scales[i].scale.z = proj.scales[i].value[2];
        s->scale_index[proj.scales[i].entity] = i;
    }

    /* Capsule colliders */
    s->capsule_collider_count = proj.capsule_collider_count;
    for (i = 0; i < proj.capsule_collider_count; i++) {
        s->capsule_colliders[i].entity_index = proj.capsule_colliders[i].entity;
        s->capsule_colliders[i].radius = proj.capsule_colliders[i].radius;
        s->capsule_colliders[i].half_height = proj.capsule_colliders[i].half_height;
        s->capsule_collider_index[proj.capsule_colliders[i].entity] = i;
    }

    /* Parent transforms */
    s->parent_transform_count = proj.parent_transform_count;
    for (i = 0; i < proj.parent_transform_count; i++) {
        s->parent_transforms[i].entity_index = proj.parent_transforms[i].entity;
        s->parent_transforms[i].parent_entity_index = proj.parent_transforms[i].parent;
        s->parent_transform_index[proj.parent_transforms[i].entity] = i;
    }

    printf("Loaded project: %s (%d entities)\n", proj.name, s->entity_count);
    return 0;
}

/* ── Handle new client connection ── */

static void server_accept_client(collab_server *s) {
    struct sockaddr_in addr;
    int addrlen = sizeof(addr);
    server_socket_t sock;
    int slot = -1;
    int i;
    msg_welcome_payload welcome;
    uint8_t *snap_buf;
    int snap_len;

    sock = accept(s->listen_socket, (struct sockaddr *)&addr, &addrlen);
    if (sock == SERVER_INVALID_SOCKET) return;

    /* Find free slot */
    for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
        if (!s->clients[i].connected) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Server full */
        printf("Rejected connection: server full\n");
        server_close(sock);
        return;
    }

    /* Read the MSG_JOIN (blocking, should arrive immediately) */
    {
        uint8_t join_buf[COLLAB_MSG_HEADER_SIZE + sizeof(msg_join_payload)];
        int total = 0;
        while (total < (int)sizeof(join_buf)) {
            int n = recv(sock, (char *)(join_buf + total), (int)sizeof(join_buf) - total, 0);
            if (n <= 0) { server_close(sock); return; }
            total += n;
        }
        /* Parse join (we just need the name) */
        {
            msg_join_payload join;
            memcpy(&join, join_buf + COLLAB_MSG_HEADER_SIZE, sizeof(join));
            strncpy(s->clients[slot].name, join.name, sizeof(s->clients[slot].name) - 1);
        }
    }

    s->clients[slot].socket = sock;
    s->clients[slot].client_id = s->next_client_id++;
    s->clients[slot].connected = 1;
    s->clients[slot].recv_len = 0;
    s->client_count++;

    printf("Client %u (%s) connected from %s [slot %d]\n",
           s->clients[slot].client_id, s->clients[slot].name,
           inet_ntoa(addr.sin_addr), slot);

    /* Send MSG_WELCOME */
    welcome.client_id = s->clients[slot].client_id;
    welcome.color = PEER_COLORS[slot % COLLAB_MAX_CLIENTS];
    welcome.current_seq = s->next_seq;
    server_send_msg(sock, MSG_WELCOME, &welcome, sizeof(welcome));

    /* Send MSG_SNAPSHOT */
    snap_buf = (uint8_t *)malloc(collab_snapshot_max_size());
    if (snap_buf) {
        snap_len = server_build_snapshot(s, snap_buf, collab_snapshot_max_size());
        if (snap_len > 0) {
            server_send_msg(sock, MSG_SNAPSHOT, snap_buf, snap_len);
        }
        free(snap_buf);
    }

    /* Set non-blocking for subsequent I/O */
#ifdef _WIN32
    {
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
    }
#else
    {
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    }
#endif
}

/* ── Handle client disconnect ── */

static void server_disconnect_client(collab_server *s, int slot) {
    msg_leave_payload leave;
    printf("Client %u (%s) disconnected [slot %d]\n",
           s->clients[slot].client_id, s->clients[slot].name, slot);

    server_close(s->clients[slot].socket);
    leave.client_id = s->clients[slot].client_id;

    s->clients[slot].connected = 0;
    s->clients[slot].socket = SERVER_INVALID_SOCKET;
    s->clients[slot].recv_len = 0;
    s->client_count--;

    /* Notify remaining clients */
    server_broadcast_except(s, leave.client_id, MSG_LEAVE, &leave, sizeof(leave));
}

/* ── Process messages from a client ── */

static void server_process_client(collab_server *s, int slot) {
    server_client *c = &s->clients[slot];

    while (c->recv_len >= COLLAB_MSG_HEADER_SIZE) {
        uint32_t total_len;
        collab_msg_type msg_type;
        int payload_len;

        if (collab_read_header(c->recv_buf, c->recv_len, &total_len, &msg_type) != 0)
            break;
        if (total_len < COLLAB_MSG_HEADER_SIZE || total_len > 65536) {
            server_disconnect_client(s, slot);
            return;
        }
        if (c->recv_len < total_len) break;

        payload_len = (int)(total_len - COLLAB_MSG_HEADER_SIZE);

        switch (msg_type) {
        case MSG_OP: {
            collab_op op;
            if (collab_op_from_msg(&op, c->recv_buf + COLLAB_MSG_HEADER_SIZE, payload_len) == 0) {
                /* Assign server sequence */
                op.seq = s->next_seq++;
                op.client_id = c->client_id;

                /* Apply to authoritative state */
                server_apply_op(s, &op);

                /* Broadcast to all OTHER clients */
                {
                    uint8_t broadcast_buf[512];
                    int bytes = collab_op_to_msg(&op, broadcast_buf, sizeof(broadcast_buf));
                    if (bytes > 0) {
                        int j;
                        for (j = 0; j < COLLAB_MAX_CLIENTS; j++) {
                            if (s->clients[j].connected && j != slot) {
                                server_send_all(s->clients[j].socket,
                                                broadcast_buf, bytes);
                            }
                        }
                    }
                }

                /* Send ACK to originator */
                {
                    msg_ack_payload ack;
                    ack.server_seq = op.seq;
                    server_send_msg(c->socket, MSG_ACK, &ack, sizeof(ack));
                }
            }
            break;
        }
        case MSG_CURSOR: {
            /* Relay cursor to others */
            server_broadcast_except(s, c->client_id, MSG_CURSOR,
                                    c->recv_buf + COLLAB_MSG_HEADER_SIZE, payload_len);
            break;
        }
        default:
            break;
        }

        /* Shift buffer */
        c->recv_len -= total_len;
        if (c->recv_len > 0)
            memmove(c->recv_buf, c->recv_buf + total_len, c->recv_len);
    }
}

/* ── Main server loop ── */

static void server_run(collab_server *s) {
    printf("Listening on port %d...\n", s->port);

    while (s->running) {
        fd_set read_fds;
        int max_fd;
        struct timeval tv;
        int i, ret;

        FD_ZERO(&read_fds);
        FD_SET(s->listen_socket, &read_fds);
        max_fd = (int)s->listen_socket;

        for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
            if (s->clients[i].connected) {
                FD_SET(s->clients[i].socket, &read_fds);
                if ((int)s->clients[i].socket > max_fd)
                    max_fd = (int)s->clients[i].socket;
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 50000; /* 50ms timeout — responsive but low CPU */

        ret = select(max_fd + 1, &read_fds, NULL, NULL, &tv);
        if (ret < 0) {
            /* Interrupted by signal */
            break;
        }
        if (ret == 0) continue;

        /* Check for new connections */
        if (FD_ISSET(s->listen_socket, &read_fds)) {
            server_accept_client(s);
        }

        /* Check each client for data */
        for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
            if (!s->clients[i].connected) continue;
            if (!FD_ISSET(s->clients[i].socket, &read_fds)) continue;

            {
                int bytes = recv(s->clients[i].socket,
                                 (char *)(s->clients[i].recv_buf + s->clients[i].recv_len),
                                 (int)(sizeof(s->clients[i].recv_buf) - s->clients[i].recv_len), 0);

                if (bytes > 0) {
                    s->clients[i].recv_len += bytes;
                    server_process_client(s, i);
                } else if (bytes == 0) {
                    /* Clean disconnect */
                    server_disconnect_client(s, i);
                } else {
#ifdef _WIN32
                    if (WSAGetLastError() != WSAEWOULDBLOCK)
#else
                    if (errno != EAGAIN && errno != EWOULDBLOCK)
#endif
                    {
                        server_disconnect_client(s, i);
                    }
                }
            }
        }
    }
}

/* ── Main ── */

int main(int argc, char **argv) {
    struct sockaddr_in addr;
    int opt = 1;
    uint16_t port = 7777;
    const char *project_path = NULL;
    int i;

    /* Parse args */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_path = argv[++i];
        }
    }

    /* Init */
    memset(&g_server, 0, sizeof(g_server));
    g_server.running = 1;
    g_server.port = port;
    g_server.next_seq = 1;
    g_server.next_client_id = 0;

    /* Init index tables to -1 */
    for (i = 0; i < COLLAB_COMP_MAX; i++) {
        g_server.transform_index[i] = -1;
        g_server.rotation_index[i] = -1;
        g_server.scale_index[i] = -1;
        g_server.capsule_collider_index[i] = -1;
        g_server.parent_transform_index[i] = -1;
    }

    for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
        g_server.clients[i].socket = SERVER_INVALID_SOCKET;
        g_server.clients[i].connected = 0;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (server_platform_init() != 0) {
        fprintf(stderr, "Failed to initialize sockets\n");
        return 1;
    }

    /* Load project if specified */
    if (project_path) {
        if (server_load_project(&g_server, project_path) != 0) {
            fprintf(stderr, "Warning: continuing with empty scene\n");
        }
    }

    /* Create listen socket */
    g_server.listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server.listen_socket == SERVER_INVALID_SOCKET) {
        fprintf(stderr, "Failed to create listen socket\n");
        server_platform_cleanup();
        return 1;
    }

    setsockopt(g_server.listen_socket, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_server.listen_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "Failed to bind to port %d\n", port);
        server_close(g_server.listen_socket);
        server_platform_cleanup();
        return 1;
    }

    if (listen(g_server.listen_socket, 4) != 0) {
        fprintf(stderr, "Failed to listen\n");
        server_close(g_server.listen_socket);
        server_platform_cleanup();
        return 1;
    }

    printf("Anitra Collab Server v1.0\n");
    printf("  Port:    %d\n", port);
    printf("  Project: %s\n", project_path ? project_path : "(empty scene)");
    printf("  Max:     %d clients\n", COLLAB_MAX_CLIENTS);
    printf("Press Ctrl+C to stop.\n\n");

    /* Run */
    server_run(&g_server);

    /* Cleanup */
    printf("\nShutting down...\n");
    for (i = 0; i < COLLAB_MAX_CLIENTS; i++) {
        if (g_server.clients[i].connected)
            server_close(g_server.clients[i].socket);
    }
    server_close(g_server.listen_socket);
    server_platform_cleanup();

    return 0;
}
