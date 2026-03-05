/* ── Stubs for building a headless test_editor.dll ─────────────────────
   Provides empty/safe implementations for symbols that editor.c imports
   from SDL3.dll, externals.dll, and ws2_32.dll at runtime.

   Compiled INTO test_editor.dll alongside editor.c so the DLL has no
   external dependencies — LoadLibrary succeeds without real SDL3 or
   externals present.
   ──────────────────────────────────────────────────────────────────── */

#include <stdint.h>
#include <string.h>

/* ── SDL3 stubs ─────────────────────────────────────────────────────── */

/* We only need the SDL types that appear in editor.c function signatures.
   editor.c includes <SDL3/SDL.h> so the real types are in scope; these
   stubs just need to compile and return harmless defaults. */

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Event  SDL_Event;

/* SDL_GetWindowSize: editor.c calls this to get win size for dock layout */
void SDL_GetWindowSize(SDL_Window *w, int *pw, int *ph) {
    (void)w;
    if (pw) *pw = 1280;
    if (ph) *ph = 720;
}

/* SDL_RaiseWindow: called when docking moves a panel to a window */
void SDL_RaiseWindow(SDL_Window *w) { (void)w; }

/* SDL_GetKeyboardFocus: update_editor checks if editor window is focused */
SDL_Window *SDL_GetKeyboardFocus(void) { return (SDL_Window *)0; }

/* SDL_GetKeyboardState: returns array of key states (all zeros = no keys) */
static uint8_t fake_keys[512];
const uint8_t *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = 512;
    return fake_keys;
}

/* SDL_GetMouseFocus: returns which window has mouse focus */
SDL_Window *SDL_GetMouseFocus(void) { return (SDL_Window *)0; }

/* SDL_GetMouseState: returns mouse position */
float SDL_GetMouseState(float *x, float *y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return 0;
}

/* SDL_GetRelativeMouseState: returns relative mouse movement */
float SDL_GetRelativeMouseState(float *x, float *y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return 0;
}

/* SDL_GetGlobalMouseState: global screen-space mouse position */
float SDL_GetGlobalMouseState(float *x, float *y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return 0;
}

/* SDL_SetWindowRelativeMouseMode: for camera look */
int SDL_SetWindowRelativeMouseMode(SDL_Window *w, int enabled) {
    (void)w; (void)enabled;
    return 0;
}

/* SDL_GetWindowFromEvent: map event → window */
SDL_Window *SDL_GetWindowFromEvent(const SDL_Event *ev) {
    (void)ev;
    return (SDL_Window *)0;
}

/* SDL_GetWindowFromID: map window ID → window */
SDL_Window *SDL_GetWindowFromID(uint32_t id) {
    (void)id;
    return (SDL_Window *)0;
}

/* SDL_GetWindowPosition: window position on screen */
int SDL_GetWindowPosition(SDL_Window *w, int *x, int *y) {
    (void)w;
    if (x) *x = 0;
    if (y) *y = 0;
    return 0;
}

/* SDL_StartTextInput / SDL_StopTextInput */
int SDL_StartTextInput(SDL_Window *w) { (void)w; return 0; }
int SDL_StopTextInput(SDL_Window *w)  { (void)w; return 0; }

/* SDL_GetModState: modifier key state */
typedef uint16_t SDL_Keymod;
SDL_Keymod SDL_GetModState(void) { return 0; }

/* SDL_CreateSystemCursor / SDL_SetCursor */
typedef struct SDL_Cursor SDL_Cursor;
SDL_Cursor *SDL_CreateSystemCursor(int id) { (void)id; return (SDL_Cursor *)0; }
int SDL_SetCursor(SDL_Cursor *c) { (void)c; return 0; }

/* ── Externals stubs (symbols imported from externals.dll) ──────────── */

void cpu_zone_begin(const char *name) { (void)name; }
void cpu_zone_end(void) {}
void cpu_prof_frame_end(void) {}
void cpu_prof_set_capture_enabled(int e) { (void)e; }
int  cpu_prof_get_capture_enabled(void) { return 0; }
int  cpu_prof_get_history_count(void) { return 0; }
uint64_t cpu_prof_get_frame_id_at_offset(int offset) { (void)offset; return 0; }
void *cpu_prof_get_frame_at_offset(int offset) { (void)offset; return 0; }

/* ── Collab stubs (normally from collab_ops.c / collab_client.c) ────── */

/* Forward-declare the opaque types so the signatures match */
struct collab_state;
struct game_state;
typedef int collab_op_type;

void collab_init(struct collab_state *cs) { (void)cs; }
int  collab_connect(struct collab_state *cs, const char *host, uint16_t port)
     { (void)cs; (void)host; (void)port; return -1; }
void collab_disconnect(struct collab_state *cs) { (void)cs; }
void collab_update(struct collab_state *cs, struct game_state *gs)
     { (void)cs; (void)gs; }
void collab_emit_op(struct collab_state *cs, collab_op_type type,
                    int entity_index, const void *payload, uint16_t payload_len)
     { (void)cs; (void)type; (void)entity_index; (void)payload; (void)payload_len; }
void collab_send_cursor(struct collab_state *cs, int entity_index)
     { (void)cs; (void)entity_index; }

/* ── Project stubs (normally from project.c, calls toml parser) ──────
   init_editor only touches project_find_* if gs->project_loaded is set.
   With project_loaded == 0, these are never called. Provide stubs so
   the DLL links cleanly. ──────────────────────────────────────────── */

struct project_data;
struct project_mesh;
struct project_anim;
struct project_box_collider;

int project_load(const char *path, struct project_data *out) {
    (void)path; (void)out;
    return -1;
}

const struct project_mesh *project_find_mesh(const struct project_data *p, int entity) {
    (void)p; (void)entity;
    return 0;
}

const struct project_anim *project_find_anim(const struct project_data *p, int entity) {
    (void)p; (void)entity;
    return 0;
}

const struct project_box_collider *project_find_box_collider(const struct project_data *p, int entity) {
    (void)p; (void)entity;
    return 0;
}

const char *project_find_asset(const struct project_data *p, const char *key, int type) {
    (void)p; (void)key; (void)type;
    return 0;
}
