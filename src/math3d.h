#ifndef MATH3D_H
#define MATH3D_H

#include <math.h>
#include <string.h>

/* ── 2D types for 2D gameplay code ─────────────────────────────── */
typedef struct { float x, y; } vec2;

static inline vec2 vec2_add(vec2 a, vec2 b)      { return (vec2){a.x + b.x, a.y + b.y}; }
static inline vec2 vec2_sub(vec2 a, vec2 b)      { return (vec2){a.x - b.x, a.y - b.y}; }
static inline vec2 vec2_scale(vec2 v, float s)   { return (vec2){v.x * s, v.y * s}; }
static inline float vec2_dot(vec2 a, vec2 b)     { return a.x*b.x + a.y*b.y; }
static inline float vec2_len(vec2 v)             { return sqrtf(vec2_dot(v,v)); }
static inline vec2 vec2_normalize(vec2 v) {
    float l = vec2_len(v);
    return l > 1e-8f ? vec2_scale(v, 1.0f/l) : (vec2){0,0};
}

/* ── 3D types ───────────────────────────────────────────────────── */
typedef struct { float x, y, z; } Vec3;
typedef struct { float x, y, z, w; } Quat;
typedef struct { float m[16]; } Mat4;

#define VEC3(x_, y_, z_)      ((Vec3){(x_), (y_), (z_)})
#define QUAT(x_, y_, z_, w_)  ((Quat){(x_), (y_), (z_), (w_)})

/* ── Vec3 ───────────────────────────────────────────────────────── */

static inline Vec3 vec3(float x, float y, float z) { return VEC3(x, y, z); }
static inline Vec3 vec3_add(Vec3 a, Vec3 b)  { return VEC3(a.x+b.x, a.y+b.y, a.z+b.z); }
static inline Vec3 vec3_sub(Vec3 a, Vec3 b)  { return VEC3(a.x-b.x, a.y-b.y, a.z-b.z); }
static inline Vec3 vec3_scale(Vec3 v, float s) { return VEC3(v.x*s, v.y*s, v.z*s); }
static inline Vec3 vec3_neg(Vec3 v)          { return VEC3(-v.x, -v.y, -v.z); }
static inline float vec3_dot(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline float vec3_len(Vec3 v)         { return sqrtf(vec3_dot(v,v)); }

static inline Vec3 vec3_normalize(Vec3 v) {
    float l = vec3_len(v);
    return l > 1e-8f ? vec3_scale(v, 1.0f/l) : VEC3(0,0,0);
}

static inline Vec3 vec3_cross(Vec3 a, Vec3 b) {
    return VEC3(
        a.y*b.z - a.z*b.y,
        a.z*b.x - a.x*b.z,
        a.x*b.y - a.y*b.x
    );
}

static inline Vec3 vec3_lerp(Vec3 a, Vec3 b, float t) {
    return VEC3(a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.z + (b.z-a.z)*t);
}

/* ── Quat ───────────────────────────────────────────────────────── */

static inline Quat quat_identity(void) { return QUAT(0, 0, 0, 1); }

static inline Quat quat_mul(Quat a, Quat b) {
    return QUAT(
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z
    );
}

static inline Quat quat_conjugate(Quat q) { return QUAT(-q.x, -q.y, -q.z, q.w); }

static inline float quat_dot(Quat a, Quat b) {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}

static inline Quat quat_normalize(Quat q) {
    float l = sqrtf(quat_dot(q, q));
    if (l < 1e-8f) return quat_identity();
    float inv = 1.0f / l;
    return QUAT(q.x*inv, q.y*inv, q.z*inv, q.w*inv);
}

static inline Quat quat_inverse(Quat q) {
    return quat_normalize(quat_conjugate(q));
}

static inline Quat quat_nlerp(Quat a, Quat b, float t) {
    /* ensure shortest path */
    if (quat_dot(a, b) < 0.0f) {
        b = QUAT(-b.x, -b.y, -b.z, -b.w);
    }
    Quat r = {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
        a.w + (b.w - a.w) * t
    };
    return quat_normalize(r);
}

static inline Quat quat_slerp(Quat a, Quat b, float t) {
    float d = quat_dot(a, b);
    if (d < 0.0f) { b = QUAT(-b.x, -b.y, -b.z, -b.w); d = -d; }
    if (d > 0.9995f) return quat_nlerp(a, b, t); /* fallback to nlerp */
    float theta = acosf(d);
    float sin_theta = sinf(theta);
    float wa = sinf((1.0f - t) * theta) / sin_theta;
    float wb = sinf(t * theta) / sin_theta;
    return QUAT(
        wa*a.x + wb*b.x, wa*a.y + wb*b.y,
        wa*a.z + wb*b.z, wa*a.w + wb*b.w
    );
}

static inline Vec3 quat_rotate(Quat q, Vec3 v) {
    Vec3 u = {q.x, q.y, q.z};
    float s = q.w;
    Vec3 t = vec3_scale(vec3_cross(u, v), 2.0f);
    return vec3_add(vec3_add(v, vec3_scale(t, s)), vec3_cross(u, t));
}

/* ── Mat4 (column-major) ────────────────────────────────────────── */
/*  m[col*4 + row]  — matches GLSL layout                          */

static inline Mat4 mat4_identity(void) {
    Mat4 m = {0};
    m.m[0] = m.m[5] = m.m[10] = m.m[15] = 1.0f;
    return m;
}

static inline Mat4 mat4_mul(Mat4 a, Mat4 b) {
    Mat4 r = {0};
    int i, j, k;
    for (j = 0; j < 4; j++)
        for (i = 0; i < 4; i++)
            for (k = 0; k < 4; k++)
                r.m[j*4+i] += a.m[k*4+i] * b.m[j*4+k];
    return r;
}

static inline Mat4 mat4_from_translation(Vec3 t) {
    Mat4 m = mat4_identity();
    m.m[12] = t.x; m.m[13] = t.y; m.m[14] = t.z;
    return m;
}

static inline Mat4 mat4_from_quat(Quat q) {
    Mat4 m = {0};
    float xx = q.x*q.x, yy = q.y*q.y, zz = q.z*q.z;
    float xy = q.x*q.y, xz = q.x*q.z, yz = q.y*q.z;
    float wx = q.w*q.x, wy = q.w*q.y, wz = q.w*q.z;
    m.m[0]  = 1.0f - 2.0f*(yy+zz);  m.m[1]  = 2.0f*(xy+wz);        m.m[2]  = 2.0f*(xz-wy);
    m.m[4]  = 2.0f*(xy-wz);          m.m[5]  = 1.0f - 2.0f*(xx+zz); m.m[6]  = 2.0f*(yz+wx);
    m.m[8]  = 2.0f*(xz+wy);          m.m[9]  = 2.0f*(yz-wx);        m.m[10] = 1.0f - 2.0f*(xx+yy);
    m.m[15] = 1.0f;
    return m;
}

static inline Mat4 mat4_from_scale(Vec3 s) {
    Mat4 m = {0};
    m.m[0] = s.x; m.m[5] = s.y; m.m[10] = s.z; m.m[15] = 1.0f;
    return m;
}

static inline Mat4 mat4_from_trs(Vec3 t, Quat r, Vec3 s) {
    Mat4 mr = mat4_from_quat(r);
    /* apply scale to rotation columns, then set translation */
    mr.m[0] *= s.x; mr.m[1] *= s.x; mr.m[2]  *= s.x;
    mr.m[4] *= s.y; mr.m[5] *= s.y; mr.m[6]  *= s.y;
    mr.m[8] *= s.z; mr.m[9] *= s.z; mr.m[10] *= s.z;
    mr.m[12] = t.x; mr.m[13] = t.y; mr.m[14] = t.z;
    return mr;
}

static inline Mat4 mat4_perspective(float fov_rad, float aspect, float near_z, float far_z) {
    Mat4 m = {0};
    float f = 1.0f / tanf(fov_rad * 0.5f);
    m.m[0]  = f / aspect;
    m.m[5]  = f;
    m.m[10] = far_z / (near_z - far_z);
    m.m[11] = -1.0f;
    m.m[14] = (near_z * far_z) / (near_z - far_z);
    return m;
}

static inline Mat4 mat4_look_at(Vec3 eye, Vec3 target, Vec3 up) {
    Vec3 f = vec3_normalize(vec3_sub(target, eye));
    Vec3 r = vec3_normalize(vec3_cross(f, up));
    Vec3 u = vec3_cross(r, f);
    Mat4 m = mat4_identity();
    m.m[0] = r.x;  m.m[4] = r.y;  m.m[8]  = r.z;
    m.m[1] = u.x;  m.m[5] = u.y;  m.m[9]  = u.z;
    m.m[2] = -f.x; m.m[6] = -f.y; m.m[10] = -f.z;
    m.m[12] = -vec3_dot(r, eye);
    m.m[13] = -vec3_dot(u, eye);
    m.m[14] =  vec3_dot(f, eye);
    return m;
}

/* Load a column-major Mat4 from a flat float[16] (e.g., cgltf output) */
static inline Mat4 mat4_from_floats(const float *f) {
    Mat4 m;
    memcpy(m.m, f, 16 * sizeof(float));
    return m;
}

#endif /* MATH3D_H */
