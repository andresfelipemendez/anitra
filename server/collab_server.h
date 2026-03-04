#ifndef COLLAB_SERVER_H
#define COLLAB_SERVER_H

#include "../src/collab/collab_ops.h"
#include "../src/project.h"

/* ── Component types (must match game.h definitions exactly) ── */
/* Duplicated here to avoid pulling in the full game.h dependency tree.
   If you change these in game.h, update them here too. */

typedef struct { float x, y, z; } Vec3;

typedef struct {
    int entity_index;
    Vec3 position;
} transform_component;

typedef struct {
    int entity_index;
    float rotation_y_deg;
} rotation_component;

typedef struct {
    int entity_index;
    Vec3 scale;
} scale_component;

typedef struct {
    float x, y, w, h;
} rect_compat; /* avoid conflict with rect in game.h */

typedef struct {
    int entity_index;
    float radius;
    float half_height;
    rect_compat aabb;
} capsule_collider_component;

typedef struct {
    int entity_index;
    int parent_entity_index;
} parent_transform_component;

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET server_socket_t;
  #define SERVER_INVALID_SOCKET INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  typedef int server_socket_t;
  #define SERVER_INVALID_SOCKET -1
#endif

/* ── Per-client state ── */

typedef struct {
    server_socket_t socket;
    uint32_t client_id;
    char name[64];
    int  connected;

    /* Receive buffer (accumulates partial reads) */
    uint8_t recv_buf[65536];
    uint32_t recv_len;
} server_client;

/* ── Server state ── */

#define SERVER_MAX_OPS 8192

typedef struct {
    /* Listen socket */
    server_socket_t listen_socket;
    uint16_t port;

    /* Connected clients */
    server_client clients[COLLAB_MAX_CLIENTS];
    int client_count;
    uint32_t next_client_id;

    /* Authoritative scene state — mirrors game_state component arrays */
    /* Entity names */
    char entity_names[COLLAB_COMP_MAX][64];
    int  entity_count;

    /* Component arrays */
    transform_component         transforms[COLLAB_COMP_MAX];
    int transform_count;
    rotation_component          rotations[COLLAB_COMP_MAX];
    int rotation_count;
    scale_component             scales[COLLAB_COMP_MAX];
    int scale_count;
    capsule_collider_component  capsule_colliders[COLLAB_COMP_MAX];
    int capsule_collider_count;
    parent_transform_component  parent_transforms[COLLAB_COMP_MAX];
    int parent_transform_count;

    /* Index tables */
    int transform_index[COLLAB_COMP_MAX];
    int rotation_index[COLLAB_COMP_MAX];
    int scale_index[COLLAB_COMP_MAX];
    int capsule_collider_index[COLLAB_COMP_MAX];
    int parent_transform_index[COLLAB_COMP_MAX];

    /* Operation history */
    uint32_t next_seq;

    /* Per-client colors */
    uint32_t client_colors[COLLAB_MAX_CLIENTS];

    /* Running flag */
    int running;
} collab_server;

/* Predefined peer colors (distinct, visually readable) */
static const uint32_t PEER_COLORS[COLLAB_MAX_CLIENTS] = {
    0x4A90D9,  /* blue */
    0xE67E22,  /* orange */
    0x2ECC71,  /* green */
    0xE74C3C   /* red */
};

#endif /* COLLAB_SERVER_H */
