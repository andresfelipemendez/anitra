#include <game.h>
#include <export.h>
#include <math3d.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdbool.h>

/* ── Internal helpers ─────────────────────────────────────────── */

static Vec3 cam_forward(editor_state *e) {
    return VEC3(
        cosf(e->cam_pitch) * sinf(e->cam_yaw),
        sinf(e->cam_pitch),
        cosf(e->cam_pitch) * cosf(e->cam_yaw)
    );
}

static Vec3 world_to_screen(Vec3 pos, Mat4 vp, float w, float h) {
    float cx = vp.m[0]*pos.x + vp.m[4]*pos.y + vp.m[8]*pos.z  + vp.m[12];
    float cy = vp.m[1]*pos.x + vp.m[5]*pos.y + vp.m[9]*pos.z  + vp.m[13];
    float cw = vp.m[3]*pos.x + vp.m[7]*pos.y + vp.m[11]*pos.z + vp.m[15];
    if (cw < 0.001f) return VEC3(-9999, -9999, 0);
    {
        float ndx = cx / cw, ndy = cy / cw;
        return VEC3((ndx * 0.5f + 0.5f) * w, (1.0f - (ndy * 0.5f + 0.5f)) * h, 0);
    }
}

static float pt_seg_dist_sq(Vec3 p, Vec3 a, Vec3 b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    float len_sq = dx*dx + dy*dy;
    float t, qx, qy;
    if (len_sq < 0.001f) { float ex = p.x-a.x, ey = p.y-a.y; return ex*ex+ey*ey; }
    t = ((p.x-a.x)*dx + (p.y-a.y)*dy) / len_sq;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    qx = a.x + t*dx - p.x;
    qy = a.y + t*dy - p.y;
    return qx*qx + qy*qy;
}

static void add_line(editor_state *e, Vec3 a, Vec3 b, float r, float g, float bl) {
    int i;
    if (e->line_count >= EDITOR_MAX_LINES) return;
    i = e->line_count * 2;
    e->lines[i + 0].x = a.x; e->lines[i + 0].y = a.y; e->lines[i + 0].z = a.z;
    e->lines[i + 0].r = r;   e->lines[i + 0].g = g;   e->lines[i + 0].b = bl;
    e->lines[i + 1].x = b.x; e->lines[i + 1].y = b.y; e->lines[i + 1].z = b.z;
    e->lines[i + 1].r = r;   e->lines[i + 1].g = g;   e->lines[i + 1].b = bl;
    e->line_count++;
}

/* ── Line building ─────────────────────────────────────────────── */

static void build_lines(game *g) {
    editor_state *e = &g->editor;
    float gc = 0.3f;
    int i;
    Vec3 axis_dirs[3];
    float colors[3][3];

    e->line_count = 0;

    /* Ground grid: 21x21 on XZ plane at Y=0 */
    for (i = -10; i <= 10; i++) {
        float fi = (float)i;
        add_line(e, VEC3(fi, 0, -10), VEC3(fi, 0, 10), gc, gc, gc);
        add_line(e, VEC3(-10, 0, fi), VEC3(10, 0, fi), gc, gc, gc);
    }
    /* Axis highlights */
    add_line(e, VEC3(-10, 0, 0), VEC3(10, 0, 0), 0.8f, 0.2f, 0.2f);  /* X red */
    add_line(e, VEC3(0, 0, -10), VEC3(0, 0, 10), 0.2f, 0.2f, 0.8f);  /* Z blue */
    add_line(e, VEC3(0, 0, 0),   VEC3(0, 2, 0),  0.2f, 0.8f, 0.2f);  /* Y green */

    /* Game camera frustum gizmo */
    if (g->mesh3d.visible) {
        Vec3 eye    = g->mesh3d.camera_eye;
        Vec3 target = g->mesh3d.camera_target;
        Vec3 cam_up = g->mesh3d.camera_up;
        Vec3 fwd    = vec3_normalize(vec3_sub(target, eye));
        Vec3 right  = vec3_normalize(vec3_cross(fwd, cam_up));
        Vec3 up     = vec3_cross(right, fwd);

        float fov    = 60.0f * 3.14159265f / 180.0f;
        float aspect = (g->width > 0 && g->height > 0)
                       ? (float)g->width / (float)g->height : 4.0f / 3.0f;
        float near_d = 0.3f;
        float vis_d  = 3.0f;

        float nh = tanf(fov * 0.5f) * near_d;
        float nw = nh * aspect;
        float fh = tanf(fov * 0.5f) * vis_d;
        float fw = fh * aspect;

        Vec3 nc = vec3_add(eye, vec3_scale(fwd, near_d));
        Vec3 fc = vec3_add(eye, vec3_scale(fwd, vis_d));

        Vec3 n[4], f[4];
        float cr, cg2, cb;

        n[0] = vec3_add(vec3_add(nc, vec3_scale(right,  nw)), vec3_scale(up,  nh));
        n[1] = vec3_add(vec3_add(nc, vec3_scale(right, -nw)), vec3_scale(up,  nh));
        n[2] = vec3_add(vec3_add(nc, vec3_scale(right, -nw)), vec3_scale(up, -nh));
        n[3] = vec3_add(vec3_add(nc, vec3_scale(right,  nw)), vec3_scale(up, -nh));

        f[0] = vec3_add(vec3_add(fc, vec3_scale(right,  fw)), vec3_scale(up,  fh));
        f[1] = vec3_add(vec3_add(fc, vec3_scale(right, -fw)), vec3_scale(up,  fh));
        f[2] = vec3_add(vec3_add(fc, vec3_scale(right, -fw)), vec3_scale(up, -fh));
        f[3] = vec3_add(vec3_add(fc, vec3_scale(right,  fw)), vec3_scale(up, -fh));

        cr = 1.0f; cg2 = 1.0f; cb = 0.2f; /* yellow */
        for (i = 0; i < 4; i++) add_line(e, eye, f[i], cr, cg2, cb);
        for (i = 0; i < 4; i++) add_line(e, n[i], n[(i+1)%4], cr, cg2, cb);
        for (i = 0; i < 4; i++) add_line(e, f[i], f[(i+1)%4], cr, cg2, cb);
        add_line(e, eye, vec3_add(eye, vec3_scale(up, 0.3f)), 0.2f, 1.0f, 0.2f);

        /* Translate gizmo at game camera eye */
        {
            Vec3 giz = eye;
            float giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
            float giz_len  = giz_dist * 0.08f;
            if (giz_len < 0.1f) giz_len = 0.1f;

            axis_dirs[0] = VEC3(1,0,0); axis_dirs[1] = VEC3(0,1,0); axis_dirs[2] = VEC3(0,0,1);
            colors[0][0] = 1.0f; colors[0][1] = 0.2f; colors[0][2] = 0.2f;
            colors[1][0] = 0.2f; colors[1][1] = 1.0f; colors[1][2] = 0.2f;
            colors[2][0] = 0.2f; colors[2][1] = 0.2f; colors[2][2] = 1.0f;

            for (i = 0; i < 3; i++) {
                int ax = i + 1;
                int hl = (e->gizmo_active == ax || (e->gizmo_active == 0 && e->gizmo_hovered == ax));
                float cr2 = hl ? 1.0f : colors[i][0];
                float cg3 = hl ? 1.0f : colors[i][1];
                float cb2 = hl ? 0.2f : colors[i][2];
                Vec3 tip = vec3_add(giz, vec3_scale(axis_dirs[i], giz_len));
                float ah;
                int a1, a2;
                Vec3 perp1, perp2, back;

                add_line(e, giz, tip, cr2, cg3, cb2);

                /* Arrowhead */
                ah = giz_len * 0.15f;
                a1 = (i + 1) % 3; a2 = (i + 2) % 3;
                perp1 = axis_dirs[a1]; perp2 = axis_dirs[a2];
                back = vec3_sub(tip, vec3_scale(axis_dirs[i], ah));
                add_line(e, tip, vec3_add(back, vec3_scale(perp1,  ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp1, -ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp2,  ah * 0.5f)), cr2, cg3, cb2);
                add_line(e, tip, vec3_add(back, vec3_scale(perp2, -ah * 0.5f)), cr2, cg3, cb2);
            }
        }
    }
}

/* ── Camera input (polling-based) ──────────────────────────────── */

static void update_camera(game *g) {
    editor_state *e = &g->editor;
    const bool *keys;
    float dt, spd, dx, dy;
    Vec3 fwd, right, up;
    SDL_Window *focused;

    if (!e->open || !e->window) return;
    focused = SDL_GetKeyboardFocus();
    if (focused != (SDL_Window *)e->window) return;

    keys = SDL_GetKeyboardState(NULL);
    dt   = g->dt;
    fwd  = cam_forward(e);
    right = vec3_normalize(vec3_cross(fwd, VEC3(0, 1, 0)));
    up    = VEC3(0, 1, 0);
    spd   = e->cam_speed * dt;

    if (keys[SDL_SCANCODE_W]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(fwd,   spd));
    if (keys[SDL_SCANCODE_S]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(fwd,   spd));
    if (keys[SDL_SCANCODE_A]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(right, spd));
    if (keys[SDL_SCANCODE_D]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(right, spd));
    if (keys[SDL_SCANCODE_E]) e->cam_pos = vec3_add(e->cam_pos, vec3_scale(up,    spd));
    if (keys[SDL_SCANCODE_Q]) e->cam_pos = vec3_sub(e->cam_pos, vec3_scale(up,    spd));

    if (e->cam_mouse_look) {
        SDL_GetRelativeMouseState(&dx, &dy);
        e->cam_yaw   -= dx * e->cam_sens;
        e->cam_pitch -= dy * e->cam_sens;
        if (e->cam_pitch >  1.55f) e->cam_pitch =  1.55f;
        if (e->cam_pitch < -1.55f) e->cam_pitch = -1.55f;
    }
}

/* ── Gizmo hover detection (polling-based) ─────────────────────── */

static void update_gizmo_hover(game *g) {
    editor_state *e = &g->editor;
    SDL_Window *mouse_win;
    int ww, wh, i;
    float fw2, fh2, ed_aspect, mx, my;
    Mat4 ed_proj, ed_view, vp;
    Vec3 ed_fwd, giz, mouse, center_s;
    float giz_dist, giz_len, best_dist;
    Vec3 axis_dirs[3];

    if (!e->open || !e->window || !g->mesh3d.visible) {
        e->gizmo_hovered = 0;
        return;
    }
    if (e->gizmo_active != 0) return;

    mouse_win = SDL_GetMouseFocus();
    if (mouse_win != (SDL_Window *)e->window) {
        e->gizmo_hovered = 0;
        return;
    }

    SDL_GetWindowSize((SDL_Window *)e->window, &ww, &wh);
    fw2 = (float)ww; fh2 = (float)wh;
    if (fw2 < 1 || fh2 < 1) return;

    ed_aspect = fw2 / fh2;
    ed_proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, ed_aspect, 0.1f, 200.0f);
    ed_fwd  = cam_forward(e);
    ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
    vp = mat4_mul(ed_proj, ed_view);

    SDL_GetMouseState(&mx, &my);
    mouse = VEC3(mx, my, 0);

    giz = g->mesh3d.camera_eye;
    giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
    giz_len  = giz_dist * 0.08f;
    if (giz_len < 0.1f) giz_len = 0.1f;

    center_s = world_to_screen(giz, vp, fw2, fh2);
    axis_dirs[0] = VEC3(giz_len, 0, 0);
    axis_dirs[1] = VEC3(0, giz_len, 0);
    axis_dirs[2] = VEC3(0, 0, giz_len);

    e->gizmo_hovered = 0;
    best_dist = 12.0f * 12.0f;

    for (i = 0; i < 3; i++) {
        Vec3 tip_s = world_to_screen(vec3_add(giz, axis_dirs[i]), vp, fw2, fh2);
        float d = pt_seg_dist_sq(mouse, center_s, tip_s);
        if (d < best_dist) {
            best_dist = d;
            e->gizmo_hovered = i + 1;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────── */

EXPORT void init_editor(game *g) {
    editor_state *e = &g->editor;
    if (e->initialized) return;

    e->cam_pos    = VEC3(0.0f, 3.0f, 8.0f);
    e->cam_yaw    = 3.14159265f;
    e->cam_pitch  = -0.3f;
    e->cam_speed  = 5.0f;
    e->cam_sens   = 0.003f;
    e->cam_mouse_look = 0;
    e->open = 1;
    e->gizmo_hovered = 0;
    e->gizmo_active  = 0;
    e->line_count = 0;
    e->initialized = 1;
}

EXPORT void update_editor(game *g) {
    if (!g->editor.open) return;
    update_camera(g);
    update_gizmo_hover(g);
    build_lines(g);
}

EXPORT void destroy_editor(game *g) {
    (void)g;
}

EXPORT void editor_handle_event(game *g, void *event_ptr) {
    editor_state *e = &g->editor;
    SDL_Event *ev = (SDL_Event *)event_ptr;
    SDL_Window *evwin;

    if (!e->open || !e->window) return;

    /* Right-click mouse look toggle */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_RIGHT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window) {
            e->cam_mouse_look = 1;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 1);
        }
    }
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_RIGHT) {
        if (e->cam_mouse_look) {
            e->cam_mouse_look = 0;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 0);
        }
    }
    if (ev->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->cam_mouse_look) {
            e->cam_mouse_look = 0;
            SDL_SetWindowRelativeMouseMode((SDL_Window *)e->window, 0);
        }
    }

    /* Gizmo: left-click to start drag */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT) {
        evwin = SDL_GetWindowFromEvent(ev);
        if (evwin == (SDL_Window *)e->window && e->gizmo_hovered != 0) {
            int ww, wh;
            float fw2, fh2, ed_aspect, giz_dist, giz_len, sdx, sdy, slen;
            Mat4 ed_proj, ed_view, vp;
            Vec3 ed_fwd, giz, world_axis, center_s, tip_s;

            e->gizmo_active = e->gizmo_hovered;
            e->gizmo_drag_start_eye    = g->mesh3d.camera_eye;
            e->gizmo_drag_start_target = g->mesh3d.camera_target;
            e->gizmo_drag_accum = 0;

            SDL_GetWindowSize((SDL_Window *)e->window, &ww, &wh);
            fw2 = (float)ww; fh2 = (float)wh;
            ed_aspect = fw2 / fh2;
            ed_proj = mat4_perspective(60.0f * 3.14159265f / 180.0f, ed_aspect, 0.1f, 200.0f);
            ed_fwd  = cam_forward(e);
            ed_view = mat4_look_at(e->cam_pos, vec3_add(e->cam_pos, ed_fwd), VEC3(0, 1, 0));
            vp = mat4_mul(ed_proj, ed_view);

            giz = g->mesh3d.camera_eye;
            giz_dist = vec3_len(vec3_sub(giz, e->cam_pos));
            giz_len  = giz_dist * 0.08f;
            if (giz_len < 0.1f) giz_len = 0.1f;

            world_axis = (e->gizmo_active == 1) ? VEC3(1, 0, 0) :
                         (e->gizmo_active == 2) ? VEC3(0, 1, 0) : VEC3(0, 0, 1);

            center_s = world_to_screen(giz, vp, fw2, fh2);
            tip_s    = world_to_screen(vec3_add(giz, vec3_scale(world_axis, giz_len)), vp, fw2, fh2);
            sdx = tip_s.x - center_s.x;
            sdy = tip_s.y - center_s.y;
            slen = sqrtf(sdx * sdx + sdy * sdy);
            if (slen > 0.001f) {
                e->gizmo_screen_axis = VEC3(sdx / slen, sdy / slen, 0);
                e->gizmo_world_per_pixel = giz_len / slen;
            } else {
                e->gizmo_screen_axis = VEC3(1, 0, 0);
                e->gizmo_world_per_pixel = 0.01f;
            }
        }
    }

    /* Gizmo: mouse motion during drag */
    if (ev->type == SDL_EVENT_MOUSE_MOTION && e->gizmo_active != 0) {
        float dot = ev->motion.xrel * e->gizmo_screen_axis.x
                  + ev->motion.yrel * e->gizmo_screen_axis.y;
        Vec3 world_axis, delta;
        e->gizmo_drag_accum += dot;

        world_axis = (e->gizmo_active == 1) ? VEC3(1, 0, 0) :
                     (e->gizmo_active == 2) ? VEC3(0, 1, 0) : VEC3(0, 0, 1);
        delta = vec3_scale(world_axis, e->gizmo_drag_accum * e->gizmo_world_per_pixel);
        g->mesh3d.camera_eye    = vec3_add(e->gizmo_drag_start_eye, delta);
        g->mesh3d.camera_target = vec3_add(e->gizmo_drag_start_target, delta);
    }

    /* Gizmo: left-button up ends drag */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP &&
        ev->button.button == SDL_BUTTON_LEFT) {
        e->gizmo_active = 0;
    }
}
