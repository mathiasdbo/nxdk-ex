#include "showcase_scene.h"
#include <AL/al.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "showcase_fmt.h"
#include "al_source.h"
#include "apu_voice_mgr.h"

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <hal/video.h>
#  include <pbkit/pbkit.h>
#  include <windows.h>
#  include <xboxkrnl/xboxkrnl.h>
#  include <strings.h>
#  define HAS_PBKIT 1
#  define MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ============================================================================
 * Font: the public-domain font8x8 table shipped with nxdk's debug console
 * (256 glyphs, 8 bytes each, bit 0 = leftmost pixel)
 * ============================================================================
 */
static const unsigned char s_font[] = {
#include "../../lib/hal/font_terminal.h"
};
#undef FONT_WIDTH
#undef FONT_HEIGHT
#undef FONT_VMIRROR

/*
 * ============================================================================
 * Frame buffer of vertices & batches
 * ============================================================================
 */
#define SCENE_MAX_VERTICES 120000u
#define SCENE_MAX_BATCHES  1024u

#define PRIM_LINES 0
#define PRIM_TRIS  1
#define PRIM_QUADS 2

typedef struct {
    int prim;
    uint32_t first;
    uint32_t count;
} scene_batch_t;

static showcase_scene_vertex_t *s_verts = NULL;
static uint32_t s_vert_count = 0;
static scene_batch_t s_batches[SCENE_MAX_BATCHES];
static uint32_t s_batch_count = 0;
static bool s_overflow = false;
static uint32_t s_frame = 0;

/* Camera of the frame being built */
typedef struct {
    float eye[3];
    float right[3];
    float up[3];
    float fwd[3];
    float fx;
    float fy;
} scene_camera_t;

static scene_camera_t s_cam;
static float s_cam_back = 9.0f;
static float s_cam_height = 5.5f;
static bool s_audio_output = false;   /* the software mixer is playing the sources */
static showcase_perf_t s_perf;
static bool s_perf_valid = false;

#define CAM_NEAR 0.2f
#define FLOOR_Y  (-1.6f)

/* Palette (0xAARRGGBB) */
#define COL_PANEL      0xB0101A26u
#define COL_PANEL_EDGE 0xFF2E4A66u
#define COL_TEXT       0xFFE6EDF3u
#define COL_DIM        0xFF8B9BB0u
#define COL_ACCENT     0xFF4FC3F7u
#define COL_WARN       0xFFFFB74Du
#define COL_GOOD       0xFF66BB6Au
#define COL_LEFT       0xFF42A5F5u /* left ear / left channel */
#define COL_RIGHT      0xFFEF5350u /* right ear / right channel */
#define COL_GRID       0x5539506Bu
#define COL_GRID_AXIS  0x9946627Fu

#if defined(HAS_PBKIT)
static float s_viewport[4][4];
#endif

/*
 * ============================================================================
 * Small math helpers
 * ============================================================================
 */
static float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void sub3(float out[3], const float a[3], const float b[3]) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void cross3(float out[3], const float a[3], const float b[3]) {
    float r[3];
    r[0] = a[1] * b[2] - a[2] * b[1];
    r[1] = a[2] * b[0] - a[0] * b[2];
    r[2] = a[0] * b[1] - a[1] * b[0];
    memcpy(out, r, sizeof(r));
}

static void norm3(float v[3]) {
    float len = sqrtf(dot3(v, v));
    if (len > 1e-6f) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

static void set3(float out[3], float x, float y, float z) {
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

/* Colour helpers */
static uint32_t col_alpha(uint32_t c, float a) {
    uint32_t alpha = (uint32_t)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (c & 0x00FFFFFFu) | (alpha << 24);
}

static uint32_t col_scale(uint32_t c, float k) {
    uint32_t r = (uint32_t)clampf((float)((c >> 16) & 0xFF) * k, 0.0f, 255.0f);
    uint32_t g = (uint32_t)clampf((float)((c >> 8) & 0xFF) * k, 0.0f, 255.0f);
    uint32_t b = (uint32_t)clampf((float)(c & 0xFF) * k, 0.0f, 255.0f);
    return (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
}

static uint32_t col_lerp(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    int shift;
    t = clampf(t, 0.0f, 1.0f);
    for (shift = 0; shift < 32; shift += 8) {
        float ca = (float)((a >> shift) & 0xFF);
        float cb = (float)((b >> shift) & 0xFF);
        out |= ((uint32_t)(ca + (cb - ca) * t + 0.5f) & 0xFFu) << shift;
    }
    return out;
}

/*
 * ============================================================================
 * Primitive emission
 * ============================================================================
 */

/* Reserve n vertices of a primitive type; NULL when the buffer is full */
static showcase_scene_vertex_t *emit(int prim, uint32_t n) {
    scene_batch_t *b;
    showcase_scene_vertex_t *v;

    if (!s_verts || s_vert_count + n > SCENE_MAX_VERTICES) {
        s_overflow = true;
        return NULL;
    }

    b = (s_batch_count > 0) ? &s_batches[s_batch_count - 1] : NULL;
    if (!b || b->prim != prim) {
        if (s_batch_count >= SCENE_MAX_BATCHES) {
            s_overflow = true;
            return NULL;
        }
        b = &s_batches[s_batch_count++];
        b->prim = prim;
        b->first = s_vert_count;
        b->count = 0;
    }

    v = &s_verts[s_vert_count];
    s_vert_count += n;
    b->count += n;
    return v;
}

/* Screen pixel -> vertex (NDC; depth is unused, everything is drawn in order) */
static void put_px(showcase_scene_vertex_t *v, float px, float py, uint32_t color) {
    v->x = px / (SHOWCASE_SCENE_WIDTH * 0.5f) - 1.0f;
    v->y = 1.0f - py / (SHOWCASE_SCENE_HEIGHT * 0.5f);
    v->z = 0.5f;
    v->color = color;
}

static void rect_px(float x, float y, float w, float h, uint32_t color) {
    showcase_scene_vertex_t *v = emit(PRIM_QUADS, 4);
    if (!v) {
        return;
    }
    put_px(&v[0], x, y, color);
    put_px(&v[1], x + w, y, color);
    put_px(&v[2], x + w, y + h, color);
    put_px(&v[3], x, y + h, color);
}

static void frame_px(float x, float y, float w, float h, uint32_t color) {
    rect_px(x, y, w, 1.0f, color);
    rect_px(x, y + h - 1.0f, w, 1.0f, color);
    rect_px(x, y, 1.0f, h, color);
    rect_px(x + w - 1.0f, y, 1.0f, h, color);
}

static void panel_px(float x, float y, float w, float h) {
    rect_px(x, y, w, h, COL_PANEL);
    frame_px(x, y, w, h, COL_PANEL_EDGE);
}

static void line_px(float x0, float y0, float x1, float y1, uint32_t c0, uint32_t c1) {
    showcase_scene_vertex_t *v = emit(PRIM_LINES, 2);
    if (!v) {
        return;
    }
    put_px(&v[0], x0, y0, c0);
    put_px(&v[1], x1, y1, c1);
}

static void tri_px(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t color) {
    showcase_scene_vertex_t *v = emit(PRIM_TRIS, 3);
    if (!v) {
        return;
    }
    put_px(&v[0], x0, y0, color);
    put_px(&v[1], x1, y1, color);
    put_px(&v[2], x2, y2, color);
}

static void disc_px(float cx, float cy, float r, uint32_t color) {
    const int seg = 14;
    int i;
    for (i = 0; i < seg; i++) {
        float a0 = (float)i * 2.0f * (float)M_PI / (float)seg;
        float a1 = (float)(i + 1) * 2.0f * (float)M_PI / (float)seg;
        tri_px(cx, cy, cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), color);
    }
}

static void ring_px(float cx, float cy, float r, uint32_t color) {
    const int seg = 24;
    int i;
    for (i = 0; i < seg; i++) {
        float a0 = (float)i * 2.0f * (float)M_PI / (float)seg;
        float a1 = (float)(i + 1) * 2.0f * (float)M_PI / (float)seg;
        line_px(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), color, color);
    }
}

/* Draw text with the 8x8 font; returns the width in pixels */
static float text_px(float x, float y, int scale, uint32_t color, const char *s) {
    float s_f = (float)scale;
    float pen = x;

    for (; *s; s++) {
        const unsigned char *glyph = &s_font[(unsigned char)*s * 8u];
        int row;
        for (row = 0; row < 8; row++) {
            unsigned bits = glyph[row];
            int col = 0;
            while (col < 8) {
                int start;
                if (!(bits & (1u << col))) {
                    col++;
                    continue;
                }
                start = col;
                while (col < 8 && (bits & (1u << col))) {
                    col++;
                }
                rect_px(pen + (float)start * s_f, y + (float)row * s_f,
                        (float)(col - start) * s_f, s_f, color);
            }
        }
        pen += 8.0f * s_f;
    }
    return pen - x;
}

static float textf_px(float x, float y, int scale, uint32_t color, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

#include <stdarg.h>
static float textf_px(float x, float y, int scale, uint32_t color, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text_px(x, y, scale, color, buf);
}

/* Horizontal bar with a frame, value in [0, 1] */
static void bar_px(float x, float y, float w, float h, float value, uint32_t color) {
    rect_px(x, y, w, h, 0x80000000u);
    rect_px(x, y, w * clampf(value, 0.0f, 1.0f), h, color);
    frame_px(x, y, w, h, COL_PANEL_EDGE);
}

/*
 * ============================================================================
 * Camera & 3D primitives
 * ============================================================================
 */

static void camera_setup(const float eye[3], const float target[3]) {
    const float world_up[3] = { 0.0f, 1.0f, 0.0f };
    const float fov_y = 66.0f * (float)M_PI / 180.0f;

    memcpy(s_cam.eye, eye, sizeof(s_cam.eye));
    sub3(s_cam.fwd, target, eye);
    norm3(s_cam.fwd);
    cross3(s_cam.right, s_cam.fwd, world_up);
    norm3(s_cam.right);
    cross3(s_cam.up, s_cam.right, s_cam.fwd);

    s_cam.fy = 1.0f / tanf(fov_y * 0.5f);
    s_cam.fx = s_cam.fy * ((float)SHOWCASE_SCENE_HEIGHT / (float)SHOWCASE_SCENE_WIDTH);
}

/* World -> camera space (x right, y up, z depth along the view direction) */
static void to_view(const float p[3], float out[3]) {
    float d[3];
    sub3(d, p, s_cam.eye);
    out[0] = dot3(d, s_cam.right);
    out[1] = dot3(d, s_cam.up);
    out[2] = dot3(d, s_cam.fwd);
}

/* Camera space -> screen pixels (z must be >= CAM_NEAR) */
static void view_to_px(const float v[3], float *px, float *py) {
    float nx = s_cam.fx * v[0] / v[2];
    float ny = s_cam.fy * v[1] / v[2];
    /* Off-center projection: the view is centered between the radar and the readouts */
    *px = SHOWCASE_SCENE_VIEW_CX + nx * (SHOWCASE_SCENE_WIDTH * 0.5f);
    *py = (1.0f - ny) * (SHOWCASE_SCENE_HEIGHT * 0.5f);
}

bool showcase_scene_project(const float world[3], float *out_px, float *out_py) {
    float v[3];
    to_view(world, v);
    if (v[2] < CAM_NEAR) {
        return false;
    }
    view_to_px(v, out_px, out_py);
    return true;
}

/* Liang-Barsky clip of a screen segment against a slightly enlarged screen */
static bool clip_screen(float *x0, float *y0, float *x1, float *y1) {
    const float xmin = -8.0f, ymin = -8.0f;
    const float xmax = SHOWCASE_SCENE_WIDTH + 8.0f, ymax = SHOWCASE_SCENE_HEIGHT + 8.0f;
    float dx = *x1 - *x0, dy = *y1 - *y0;
    float t0 = 0.0f, t1 = 1.0f;
    float p[4], q[4];
    int i;

    p[0] = -dx; q[0] = *x0 - xmin;
    p[1] = dx;  q[1] = xmax - *x0;
    p[2] = -dy; q[2] = *y0 - ymin;
    p[3] = dy;  q[3] = ymax - *y0;

    for (i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) {
                return false;
            }
        } else {
            float t = q[i] / p[i];
            if (p[i] < 0.0f) {
                if (t > t1) return false;
                if (t > t0) t0 = t;
            } else {
                if (t < t0) return false;
                if (t < t1) t1 = t;
            }
        }
    }

    {
        float nx0 = *x0 + t0 * dx, ny0 = *y0 + t0 * dy;
        float nx1 = *x0 + t1 * dx, ny1 = *y0 + t1 * dy;
        *x0 = nx0; *y0 = ny0; *x1 = nx1; *y1 = ny1;
    }
    return true;
}

static void line3d(const float a[3], const float b[3], uint32_t ca, uint32_t cb) {
    float va[3], vb[3];
    float x0, y0, x1, y1;

    to_view(a, va);
    to_view(b, vb);

    /* Near-plane clip */
    if (va[2] < CAM_NEAR && vb[2] < CAM_NEAR) {
        return;
    }
    if (va[2] < CAM_NEAR || vb[2] < CAM_NEAR) {
        float t = (CAM_NEAR - va[2]) / (vb[2] - va[2]);
        float m[3];
        uint32_t cm = col_lerp(ca, cb, t);
        m[0] = va[0] + (vb[0] - va[0]) * t;
        m[1] = va[1] + (vb[1] - va[1]) * t;
        m[2] = CAM_NEAR;
        if (va[2] < CAM_NEAR) {
            memcpy(va, m, sizeof(m));
            ca = cm;
        } else {
            memcpy(vb, m, sizeof(m));
            cb = cm;
        }
    }

    view_to_px(va, &x0, &y0);
    view_to_px(vb, &x1, &y1);
    if (clip_screen(&x0, &y0, &x1, &y1)) {
        line_px(x0, y0, x1, y1, ca, cb);
    }
}

/* Horizontal circle (y = const) */
static void circle3d(const float c[3], float r, uint32_t color, int seg) {
    int i;
    for (i = 0; i < seg; i++) {
        float a0 = (float)i * 2.0f * (float)M_PI / (float)seg;
        float a1 = (float)(i + 1) * 2.0f * (float)M_PI / (float)seg;
        float p0[3], p1[3];
        set3(p0, c[0] + r * cosf(a0), c[1], c[2] + r * sinf(a0));
        set3(p1, c[0] + r * cosf(a1), c[1], c[2] + r * sinf(a1));
        line3d(p0, p1, color, color);
    }
}

/* Text anchored at a world point (centered); skipped when behind the camera */
static void label3d(const float p[3], uint32_t color, const char *s) {
    float px, py;
    if (showcase_scene_project(p, &px, &py)) {
        float w = (float)strlen(s) * 8.0f;
        if (px > -w && px < SHOWCASE_SCENE_WIDTH + w && py > -8.0f && py < SHOWCASE_SCENE_HEIGHT + 8.0f) {
            text_px(px - w * 0.5f, py - 4.0f, 1, color, s);
        }
    }
}

/*
 * Solid objects are collected first, then drawn far to near (painter's
 * algorithm). Faces facing away from the camera are skipped, so a convex
 * object never overdraws itself.
 */
#define SOLID_BOX 0
#define SOLID_OCTA 1
#define SCENE_MAX_SOLIDS 192

typedef struct {
    int kind;
    float pos[3];
    float half[3];
    float yaw;
    uint32_t color;
    float depth;
} scene_solid_t;

static scene_solid_t s_solids[SCENE_MAX_SOLIDS];
static int s_solid_count = 0;

static void add_solid(int kind, const float pos[3], float hx, float hy, float hz, float yaw, uint32_t color) {
    scene_solid_t *s;
    if (s_solid_count >= SCENE_MAX_SOLIDS) {
        return;
    }
    s = &s_solids[s_solid_count++];
    s->kind = kind;
    memcpy(s->pos, pos, sizeof(s->pos));
    set3(s->half, hx, hy, hz);
    s->yaw = yaw;
    s->color = color;
    s->depth = 0.0f;
}

static int solid_cmp(const void *a, const void *b) {
    const scene_solid_t *sa = (const scene_solid_t *)a;
    const scene_solid_t *sb = (const scene_solid_t *)b;
    if (sa->depth > sb->depth) return -1;
    if (sa->depth < sb->depth) return 1;
    return 0;
}

/* One flat-shaded convex face (3 or 4 corners); center = object center */
static void face3d(const float *const corners[], int n, const float center[3], uint32_t color) {
    static const float light[3] = { 0.37f, 0.84f, 0.40f };
    float fc[3] = { 0.0f, 0.0f, 0.0f };
    float normal[3], to_eye[3];
    float px[4], py[4];
    float shade;
    int i;
    showcase_scene_vertex_t *v;

    for (i = 0; i < n; i++) {
        fc[0] += corners[i][0] / (float)n;
        fc[1] += corners[i][1] / (float)n;
        fc[2] += corners[i][2] / (float)n;
    }
    sub3(normal, fc, center);
    norm3(normal);
    sub3(to_eye, s_cam.eye, fc);
    if (dot3(normal, to_eye) <= 0.0f) {
        return; /* back face */
    }

    for (i = 0; i < n; i++) {
        float vv[3];
        to_view(corners[i], vv);
        if (vv[2] < CAM_NEAR) {
            return;
        }
        view_to_px(vv, &px[i], &py[i]);
        if (px[i] < -2000.0f || px[i] > 2640.0f || py[i] < -2000.0f || py[i] > 2480.0f) {
            return; /* stay inside the rasteriser's guard band */
        }
    }

    shade = 0.45f + 0.55f * clampf(dot3(normal, light), 0.0f, 1.0f);
    color = col_scale(color, shade);

    v = emit(PRIM_TRIS, (uint32_t)(n - 2) * 3u);
    if (!v) {
        return;
    }
    for (i = 1; i + 1 < n; i++) {
        put_px(v++, px[0], py[0], color);
        put_px(v++, px[i], py[i], color);
        put_px(v++, px[i + 1], py[i + 1], color);
    }
}

static void draw_box(const scene_solid_t *s) {
    float c[8][3];
    float cy = cosf(s->yaw), sy = sinf(s->yaw);
    int i;
    /* Faces as corner indices: -x, +x, -y, +y, -z, +z */
    static const int faces[6][4] = {
        { 0, 2, 6, 4 }, { 1, 5, 7, 3 },
        { 0, 4, 5, 1 }, { 2, 3, 7, 6 },
        { 0, 1, 3, 2 }, { 4, 6, 7, 5 }
    };

    for (i = 0; i < 8; i++) {
        float lx = (i & 1) ? s->half[0] : -s->half[0];
        float ly = (i & 2) ? s->half[1] : -s->half[1];
        float lz = (i & 4) ? s->half[2] : -s->half[2];
        /* Yaw about +y, matching the listener's forward (sin yaw, 0, -cos yaw) */
        c[i][0] = s->pos[0] + lx * cy - lz * sy;
        c[i][1] = s->pos[1] + ly;
        c[i][2] = s->pos[2] + lx * sy + lz * cy;
    }
    for (i = 0; i < 6; i++) {
        const float *corners[4];
        corners[0] = c[faces[i][0]];
        corners[1] = c[faces[i][1]];
        corners[2] = c[faces[i][2]];
        corners[3] = c[faces[i][3]];
        face3d(corners, 4, s->pos, s->color);
    }
}

static void draw_octa(const scene_solid_t *s) {
    float p[6][3];
    int i;
    static const int faces[8][3] = {
        { 0, 2, 4 }, { 2, 1, 4 }, { 1, 3, 4 }, { 3, 0, 4 },
        { 2, 0, 5 }, { 1, 2, 5 }, { 3, 1, 5 }, { 0, 3, 5 }
    };

    set3(p[0], s->pos[0] + s->half[0], s->pos[1], s->pos[2]);
    set3(p[1], s->pos[0] - s->half[0], s->pos[1], s->pos[2]);
    set3(p[2], s->pos[0], s->pos[1], s->pos[2] + s->half[2]);
    set3(p[3], s->pos[0], s->pos[1], s->pos[2] - s->half[2]);
    set3(p[4], s->pos[0], s->pos[1] + s->half[1], s->pos[2]);
    set3(p[5], s->pos[0], s->pos[1] - s->half[1], s->pos[2]);

    for (i = 0; i < 8; i++) {
        const float *corners[3];
        corners[0] = p[faces[i][0]];
        corners[1] = p[faces[i][1]];
        corners[2] = p[faces[i][2]];
        face3d(corners, 3, s->pos, s->color);
    }
}

static void flush_solids(void) {
    int i;
    for (i = 0; i < s_solid_count; i++) {
        float v[3];
        to_view(s_solids[i].pos, v);
        s_solids[i].depth = v[2];
    }
    qsort(s_solids, (size_t)s_solid_count, sizeof(s_solids[0]), solid_cmp);
    for (i = 0; i < s_solid_count; i++) {
        if (s_solids[i].kind == SOLID_BOX) {
            draw_box(&s_solids[i]);
        } else {
            draw_octa(&s_solids[i]);
        }
    }
    s_solid_count = 0;
}

/*
 * ============================================================================
 * Scene content
 * ============================================================================
 */

typedef struct {
    ALuint id;           /* 0: no focus source in this mode */
    const char *name;
    uint32_t color;
    bool active;         /* playing */
    AL_SPATIAL_CALC calc;
    float pos[3];
    int hw_voice;
} scene_focus_t;

/* Listener position of the frame being built (app->listener_pos) */
static float s_listener_pos[3] = { 0.0f, 0.0f, 0.0f };

/* Speaker layout of mode 3 (same positions as apply_channel_51 in showcase_modes.c) */
static const float s_speaker_pos[6][3] = {
    { -2.887f, 0.0f, -5.0f },  /* FL */
    {  0.0f,   0.0f, -5.0f },  /* C */
    {  2.887f, 0.0f, -5.0f },  /* FR */
    {  4.698f, 0.0f,  1.710f },/* SR */
    { -4.698f, 0.0f,  1.710f },/* SL */
    {  1.6f,  -1.1f, -4.2f }   /* LFE (non-directional, drawn near the front) */
};
static const char *const s_speaker_names[6] = { "FL", "C", "FR", "SR", "SL", "LFE" };

static bool source_snapshot(ALuint id, scene_focus_t *f) {
    ALsource *src = al_source_get(id);
    if (!src) {
        return false;
    }
    f->id = id;
    f->active = (src->state == AL_PLAYING);
    f->hw_voice = src->hw_voice_idx;
    memcpy(f->pos, src->position, sizeof(f->pos));
    al_source_calc_spatial(src, &f->calc);
    return true;
}

static void pick_focus(const showcase_app_t *app, scene_focus_t *f) {
    memset(f, 0, sizeof(*f));
    f->hw_voice = -1;

    switch (app->current_mode) {
        case MODE_ORBIT_3D:
        case MODE_BGM_2D_CONCURRENT:
            f->name = "HELICOPTER";
            f->color = 0xFF26C6DAu;
            source_snapshot(app->src_orbit, f);
            break;
        case MODE_DOPPLER_FLYBY:
            f->name = "DOPPLER SIREN";
            f->color = 0xFFFFCA28u;
            source_snapshot(app->src_doppler, f);
            break;
        case MODE_SURROUND_51:
            if (app->surround_channel_idx == 5) {
                f->name = "EXPLOSION (LFE)";
                f->color = 0xFFAB47BCu;
                source_snapshot(app->src_lfe_51, f);
            } else {
                f->name = "SPEAKER TEST";
                f->color = 0xFF9CCC65u;
                source_snapshot(app->src_sat_51, f);
            }
            break;
        default:
            break;
    }
}

static void draw_grid(void) {
    const float ext = 24.0f;
    float g;
    for (g = -ext; g <= ext + 0.01f; g += 2.0f) {
        float a[3], b[3];
        uint32_t c = (fabsf(g) < 0.01f) ? COL_GRID_AXIS : COL_GRID;
        set3(a, g, FLOOR_Y, -ext);
        set3(b, g, FLOOR_Y, ext);
        line3d(a, b, c, c);
        set3(a, -ext, FLOOR_Y, g);
        set3(b, ext, FLOOR_Y, g);
        line3d(a, b, c, c);
    }
}

static void listener_vectors(const showcase_app_t *app, float fwd[3], float flat_fwd[3], float flat_right[3]) {
    float cy = cosf(app->camera_yaw), sy = sinf(app->camera_yaw);
    float cp = cosf(app->camera_pitch), sp = sinf(app->camera_pitch);
    set3(fwd, sy * cp, sp, -cy * cp);
    set3(flat_fwd, sy, 0.0f, -cy);
    set3(flat_right, cy, 0.0f, sy);
}

static void draw_listener(const showcase_app_t *app) {
    float fwd[3], ffwd[3], fright[3], p[3];
    const float *L = s_listener_pos;
    float yaw = app->camera_yaw;

    listener_vectors(app, fwd, ffwd, fright);

    add_solid(SOLID_BOX, s_listener_pos, 0.32f, 0.38f, 0.32f, yaw, 0xFFCFD8DCu);
    /* Ears: left blue, right red (same colours as the ITD meters) */
    set3(p, L[0] - fright[0] * 0.42f, L[1], L[2] - fright[2] * 0.42f);
    add_solid(SOLID_BOX, p, 0.10f, 0.16f, 0.12f, yaw, COL_LEFT);
    set3(p, L[0] + fright[0] * 0.42f, L[1], L[2] + fright[2] * 0.42f);
    add_solid(SOLID_BOX, p, 0.10f, 0.16f, 0.12f, yaw, COL_RIGHT);
    /* Nose follows the full look direction (yaw and pitch) */
    set3(p, L[0] + fwd[0] * 0.48f, L[1] + fwd[1] * 0.48f, L[2] + fwd[2] * 0.48f);
    add_solid(SOLID_OCTA, p, 0.11f, 0.11f, 0.11f, 0.0f, COL_ACCENT);

    /* Look direction ray and its shadow */
    {
        float tip[3], fa[3], fb[3];
        set3(tip, L[0] + fwd[0] * 3.0f, L[1] + fwd[1] * 3.0f, L[2] + fwd[2] * 3.0f);
        line3d(s_listener_pos, tip, col_alpha(COL_ACCENT, 0.9f), col_alpha(COL_ACCENT, 0.0f));
        set3(fa, L[0], FLOOR_Y, L[2]);
        set3(fb, L[0] + ffwd[0] * 3.0f, FLOOR_Y, L[2] + ffwd[2] * 3.0f);
        line3d(fa, fb, col_alpha(COL_ACCENT, 0.5f), col_alpha(COL_ACCENT, 0.0f));
    }
}

/* Source marker: drop line to the floor, emission ripples and the sound ray to the listener */
static void draw_emitter(const float pos[3], uint32_t color, float gain, bool active, float size) {
    float floor_pt[3];
    float r = size * (0.7f + 0.6f * clampf(gain, 0.0f, 1.0f));

    set3(floor_pt, pos[0], FLOOR_Y, pos[2]);
    line3d(pos, floor_pt, col_alpha(color, 0.5f), col_alpha(color, 0.15f));
    circle3d(floor_pt, 0.25f, col_alpha(color, 0.5f), 10);

    if (active) {
        int k;
        float phase = (float)s_frame * 0.02f;
        for (k = 0; k < 3; k++) {
            float t = fmodf(phase + (float)k / 3.0f, 1.0f);
            circle3d(pos, r + t * 1.8f, col_alpha(color, (1.0f - t) * 0.8f * clampf(gain * 1.5f + 0.2f, 0.0f, 1.0f)), 20);
        }
        line3d(pos, s_listener_pos, col_alpha(color, 0.15f + 0.75f * clampf(gain, 0.0f, 1.0f)), col_alpha(color, 0.05f));
        add_solid(SOLID_OCTA, pos, r, r * 1.3f, r, 0.0f, color);
    } else {
        add_solid(SOLID_OCTA, pos, r, r * 1.3f, r, 0.0f, col_scale(color, 0.4f));
    }
}

static void draw_distance_rings(const scene_focus_t *f) {
    ALsource *src;
    float c[3], p[3];
    char buf[24];

    if (!f->id || !(src = al_source_get(f->id))) {
        return;
    }
    set3(c, s_listener_pos[0], FLOOR_Y, s_listener_pos[2]);
    if (src->reference_distance > 0.0f && src->reference_distance < 60.0f) {
        circle3d(c, src->reference_distance, col_alpha(COL_GOOD, 0.6f), 48);
        set3(p, c[0] + src->reference_distance * 0.7071f, FLOOR_Y, c[2] + src->reference_distance * 0.7071f);
        snprintf(buf, sizeof(buf), "REF %sm", showcase_fx(src->reference_distance, 0, false, 0));
        label3d(p, col_alpha(COL_GOOD, 0.9f), buf);
    }
    if (src->max_distance > 0.0f && src->max_distance < 60.0f) {
        circle3d(c, src->max_distance, col_alpha(COL_WARN, 0.45f), 64);
        set3(p, c[0] + src->max_distance * 0.7071f, FLOOR_Y, c[2] + src->max_distance * 0.7071f);
        snprintf(buf, sizeof(buf), "MAX %sm", showcase_fx(src->max_distance, 0, false, 0));
        label3d(p, col_alpha(COL_WARN, 0.9f), buf);
    }
}

/* Doppler colour: approaching (pitch > 1) shifts to blue, receding to red */
static uint32_t doppler_color(float pitch) {
    float t = clampf((pitch - 1.0f) * 6.0f, -1.0f, 1.0f);
    if (t >= 0.0f) {
        return col_lerp(0xFFFFFFFFu, 0xFF42A5F5u, t);
    }
    return col_lerp(0xFFFFFFFFu, 0xFFEF5350u, -t);
}

static void draw_mode_world(const showcase_app_t *app, const scene_focus_t *f) {
    switch (app->current_mode) {
        case MODE_ORBIT_3D:
        case MODE_BGM_2D_CONCURRENT: {
            /* Orbit path (mode 1 bobs in height, mode 5 stays flat at 5 m) */
            const int seg = 72;
            float radius = (app->current_mode == MODE_ORBIT_3D) ? app->orbit_radius : 5.0f;
            int i;
            for (i = 0; i < seg; i++) {
                float a0 = (float)i * 2.0f * (float)M_PI / (float)seg;
                float a1 = (float)(i + 1) * 2.0f * (float)M_PI / (float)seg;
                float p0[3], p1[3];
                float y0 = (app->current_mode == MODE_ORBIT_3D) ? 1.2f * sinf(2.0f * a0) : 0.0f;
                float y1 = (app->current_mode == MODE_ORBIT_3D) ? 1.2f * sinf(2.0f * a1) : 0.0f;
                set3(p0, radius * sinf(a0), y0, -radius * cosf(a0));
                set3(p1, radius * sinf(a1), y1, -radius * cosf(a1));
                line3d(p0, p1, col_alpha(f->color, 0.35f), col_alpha(f->color, 0.35f));
            }
            draw_emitter(f->pos, f->color, f->calc.effective_gain, f->active, 0.32f);
            break;
        }

        case MODE_DOPPLER_FLYBY: {
            float a[3], b[3];
            set3(a, -35.0f, 0.5f, -1.2f);
            set3(b, 35.0f, 0.5f, -1.2f);
            line3d(a, b, col_alpha(f->color, 0.3f), col_alpha(f->color, 0.3f));
            add_solid(SOLID_BOX, a, 0.3f, 0.3f, 0.3f, 0.0f, 0xFF607D8Bu);
            if (app->doppler_flying) {
                uint32_t c = doppler_color(f->calc.doppler_pitch);
                float tip[3];
                draw_emitter(f->pos, c, f->calc.effective_gain, true, 0.35f);
                set3(tip, f->pos[0] + 3.0f, f->pos[1], f->pos[2]);
                line3d(f->pos, tip, col_alpha(c, 0.9f), col_alpha(c, 0.1f));
            } else {
                label3d(a, COL_WARN, "LAUNCHER");
            }
            break;
        }

        case MODE_SURROUND_51: {
            int i;
            for (i = 0; i < 6; i++) {
                bool on = (i == app->surround_channel_idx);
                float top[3];
                uint32_t c = on ? f->color : 0xFF546E7Au;
                if (i == 5) {
                    add_solid(SOLID_BOX, s_speaker_pos[i], 0.45f, 0.45f, 0.45f, 0.0f, c);
                } else {
                    /* Cabinets face the listener */
                    float yaw = atan2f(-s_speaker_pos[i][0], s_speaker_pos[i][2]);
                    add_solid(SOLID_BOX, s_speaker_pos[i], 0.28f, 0.45f, 0.22f, yaw, c);
                }
                set3(top, s_speaker_pos[i][0], s_speaker_pos[i][1] + 0.85f, s_speaker_pos[i][2]);
                label3d(top, on ? COL_TEXT : COL_DIM, s_speaker_names[i]);
            }
            if (app->surround_channel_idx < 5) {
                draw_emitter(f->pos, f->color, f->calc.effective_gain, f->active, 0.18f);
            } else {
                /* LFE has no direction: pulse rings around the subwoofer */
                float phase = fmodf((float)s_frame * 0.03f, 1.0f);
                float fl[3];
                set3(fl, s_speaker_pos[5][0], FLOOR_Y, s_speaker_pos[5][2]);
                circle3d(fl, 0.6f + phase * 3.0f, col_alpha(f->color, 1.0f - phase), 32);
            }
            break;
        }

        case MODE_POLYPHONY_STRESS: {
            int i;
            for (i = 0; i < MAX_STRESS_SOURCES; i++) {
                ALsource *src;
                uint32_t c;
                if (!app->stress_active[i] || !(src = al_source_get(app->src_stress[i]))) {
                    continue;
                }
                if (src->state != AL_PLAYING) {
                    continue;
                }
                c = (src->hw_voice_idx >= 0) ? COL_GOOD : COL_WARN;
                add_solid(SOLID_BOX, src->position, 0.16f, 0.16f, 0.16f, 0.0f, c);
            }
            break;
        }

        default:
            break;
    }
}

/*
 * ============================================================================
 * HUD
 * ============================================================================
 */

static void hud_top_bar(const showcase_app_t *app, const showcase_input_t *input) {
    char right[64];
    float w;

    panel_px(0.0f, 0.0f, (float)SHOWCASE_SCENE_WIDTH, 52.0f);
    text_px(32.0f, 10.0f, 1, COL_ACCENT, "NXDK OPENAL SHOWCASE");
    textf_px(32.0f + 21.0f * 8.0f, 10.0f, 1, COL_DIM, "MODE %d/%d", app->current_mode, SHOWCASE_NUM_MODES);
    if (app->walk_mode) {
        /* Walk mode badge with the listener's ground position */
        textf_px(32.0f + 32.0f * 8.0f, 10.0f, 1, COL_GOOD, "WALK x %s z %s",
                 showcase_fx(app->listener_pos[0], 1, true, 0), showcase_fx(app->listener_pos[2], 1, true, 0));
    }
    text_px(32.0f, 24.0f, 2, COL_TEXT, showcase_app_mode_title(app->current_mode));

    snprintf(right, sizeof(right), "%s", (input && input->controller_connected) ? "GAMEPAD" : "AUTO-TOUR");
    w = (float)strlen(right) * 8.0f;
    text_px((float)SHOWCASE_SCENE_WIDTH - 32.0f - w, 10.0f, 1,
            (input && input->controller_connected) ? COL_GOOD : COL_WARN, right);

    /* Short enough to clear the longest mode title */
    snprintf(right, sizeof(right), "%s", (app->topology == 1) ? "5.1" : "STEREO");
    w = (float)strlen(right) * 8.0f;
    text_px((float)SHOWCASE_SCENE_WIDTH - 32.0f - w, 36.0f, 1, COL_DIM, right);
}

/* Top-down radar: listener at the center, facing up */
static void hud_radar(const showcase_app_t *app, const scene_focus_t *f) {
    const float x = 32.0f, y = 62.0f, size = 132.0f;
    const float cx = x + size * 0.5f, cy = y + size * 0.5f + 4.0f;
    const float rad = size * 0.5f - 10.0f;
    float range = 16.0f;
    float fwd[3], ffwd[3], fright[3];
    int i;

    if (app->current_mode == MODE_DOPPLER_FLYBY) {
        range = 36.0f;
    } else if (app->current_mode == MODE_POLYPHONY_STRESS) {
        range = 14.0f;
    }

    listener_vectors(app, fwd, ffwd, fright);
    panel_px(x, y, size, size + 8.0f);
    text_px(x + 6.0f, y + 4.0f, 1, COL_DIM, "RADAR");
    textf_px(x + size - 6.0f - 4.0f * 8.0f, y + 4.0f, 1, COL_DIM, "%sm", showcase_fx(range, 0, false, 3));

    ring_px(cx, cy, rad, col_alpha(COL_PANEL_EDGE, 1.0f));
    ring_px(cx, cy, rad * 0.5f, col_alpha(COL_PANEL_EDGE, 0.7f));
    line_px(cx, cy - rad, cx, cy + rad, col_alpha(COL_PANEL_EDGE, 0.6f), col_alpha(COL_PANEL_EDGE, 0.6f));
    line_px(cx - rad, cy, cx + rad, cy, col_alpha(COL_PANEL_EDGE, 0.6f), col_alpha(COL_PANEL_EDGE, 0.6f));

    /* Listener: arrow up = facing; ears on each side */
    tri_px(cx, cy - 7.0f, cx - 5.0f, cy + 5.0f, cx + 5.0f, cy + 5.0f, COL_ACCENT);
    rect_px(cx - 8.0f, cy - 1.0f, 2.0f, 4.0f, COL_LEFT);
    rect_px(cx + 6.0f, cy - 1.0f, 2.0f, 4.0f, COL_RIGHT);

#define RADAR_DOT(P, R, C) do { \
        float rel_[3], lr_, lf_; \
        sub3(rel_, (P), s_listener_pos); \
        lr_ = dot3(rel_, fright); \
        lf_ = dot3(rel_, ffwd); \
        float d_ = sqrtf(lr_ * lr_ + lf_ * lf_); \
        float k_ = rad / range; \
        if (d_ > range) { k_ *= range / d_; } \
        disc_px(cx + lr_ * k_, cy - lf_ * k_, (R), (C)); \
    } while (0)

    switch (app->current_mode) {
        case MODE_SURROUND_51:
            for (i = 0; i < 5; i++) {
                RADAR_DOT(s_speaker_pos[i], (i == app->surround_channel_idx) ? 4.0f : 2.5f,
                          (i == app->surround_channel_idx) ? f->color : COL_DIM);
            }
            break;
        case MODE_POLYPHONY_STRESS:
            for (i = 0; i < MAX_STRESS_SOURCES; i++) {
                ALsource *src;
                if (!app->stress_active[i] || !(src = al_source_get(app->src_stress[i])) || src->state != AL_PLAYING) {
                    continue;
                }
                RADAR_DOT(src->position, 1.5f, (src->hw_voice_idx >= 0) ? COL_GOOD : COL_WARN);
            }
            break;
        default:
            if (f->id && (f->active || app->current_mode != MODE_DOPPLER_FLYBY)) {
                RADAR_DOT(f->pos, 4.0f, f->active ? f->color : col_scale(f->color, 0.5f));
            }
            break;
    }
#undef RADAR_DOT
}

/* Spatial model readouts of the focus source, or the voice slot map in mode 4 */
static void hud_spatial_panel(const showcase_app_t *app, const scene_focus_t *f) {
    const float x = 428.0f, y = 62.0f, w = 180.0f;
    float ty = y + 6.0f;

    if (app->current_mode == MODE_POLYPHONY_STRESS) {
        uint32_t active_hw = apu_voice_mgr_get_active_hw_count();
        uint32_t standby = apu_voice_mgr_get_virtual_standby_count();
        int v;

        panel_px(x, y, w, 176.0f);
        text_px(x + 8.0f, ty, 1, COL_ACCENT, "VOICE SLOTS (64)");
        ty += 16.0f;
        for (v = 0; v < 64; v++) {
            int owner = apu_voice_mgr_get_owner((uint32_t)v);
            float cx = x + 8.0f + (float)(v % 8) * 20.0f;
            float cy = ty + (float)(v / 8) * 12.0f;
            rect_px(cx, cy, 17.0f, 9.0f, (owner > 0) ? COL_GOOD : 0xFF263238u);
        }
        ty += 8.0f * 12.0f + 6.0f;
        rect_px(x + 8.0f, ty, 8.0f, 8.0f, COL_GOOD);
        textf_px(x + 20.0f, ty, 1, COL_TEXT, "HARDWARE  %2u", (unsigned)active_hw);
        ty += 12.0f;
        rect_px(x + 8.0f, ty, 8.0f, 8.0f, COL_WARN);
        textf_px(x + 20.0f, ty, 1, COL_TEXT, "VIRTUAL   %2u", (unsigned)standby);
        ty += 14.0f;
        text_px(x + 8.0f, ty, 1, COL_DIM, "Steal: lowest priority");
        return;
    }

    if (!f->id) {
        return;
    }

    panel_px(x, y, w, 262.0f);
    text_px(x + 8.0f, ty, 1, COL_ACCENT, "SPATIAL MODEL");
    ty += 14.0f;
    rect_px(x + 8.0f, ty, 8.0f, 8.0f, f->color);
    text_px(x + 20.0f, ty, 1, COL_TEXT, f->name);
    ty += 12.0f;
    text_px(x + 20.0f, ty, 1, f->active ? COL_GOOD : COL_DIM,
            !f->active ? "IDLE" : (f->hw_voice >= 0 ? "PLAYING  HW VOICE" : "PLAYING  VIRTUAL"));
    if (f->active && f->hw_voice >= 0) {
        textf_px(x + 20.0f + 18.0f * 8.0f, ty, 1, COL_GOOD, "%d", f->hw_voice);
    }
    ty += 16.0f;

    {
        /* Listener frame of al_listener_world_to_local(): +x right, +y up, +z BEHIND */
        const float *lp = f->calc.local_pos;
        float az = atan2f(lp[0], -lp[2]) * 180.0f / (float)M_PI;
        float el = atan2f(lp[1], sqrtf(lp[0] * lp[0] + lp[2] * lp[2])) * 180.0f / (float)M_PI;
        textf_px(x + 8.0f, ty, 1, COL_TEXT, "DIST %s m", showcase_fx(f->calc.distance, 2, false, 6));
        ty += 12.0f;
        textf_px(x + 8.0f, ty, 1, COL_TEXT, "AZ %s\xB0  EL %s\xB0", showcase_fx(az, 0, true, 4), showcase_fx(el, 0, true, 3));
        ty += 16.0f;
    }

    text_px(x + 8.0f, ty, 1, COL_DIM, "GAIN");
    bar_px(x + 48.0f, ty, 84.0f, 8.0f, f->calc.effective_gain, COL_ACCENT);
    textf_px(x + 138.0f, ty, 1, COL_TEXT, "%s", showcase_fx(f->calc.effective_gain, 2, false, 0));
    ty += 14.0f;

    text_px(x + 8.0f, ty, 1, COL_DIM, "DOPP");
    {
        /* Centered at 1.0: right = higher pitch (approaching) */
        float d = clampf((f->calc.doppler_pitch - 1.0f) * 4.0f, -1.0f, 1.0f);
        float bx = x + 48.0f, bw = 84.0f, mid = bx + bw * 0.5f;
        rect_px(bx, ty, bw, 8.0f, 0x80000000u);
        if (d >= 0.0f) {
            rect_px(mid, ty, bw * 0.5f * d, 8.0f, 0xFF42A5F5u);
        } else {
            rect_px(mid + bw * 0.5f * d, ty, -bw * 0.5f * d, 8.0f, 0xFFEF5350u);
        }
        rect_px(mid, ty - 2.0f, 1.0f, 12.0f, COL_TEXT);
        frame_px(bx, ty, bw, 8.0f, COL_PANEL_EDGE);
        textf_px(x + 138.0f, ty, 1, COL_TEXT, "x%s", showcase_fx(f->calc.doppler_pitch, 2, false, 0));
    }
    ty += 14.0f;

    text_px(x + 8.0f, ty, 1, COL_DIM, "ITD");
    text_px(x + 40.0f, ty, 1, COL_LEFT, "L");
    bar_px(x + 50.0f, ty, 82.0f, 8.0f, (float)f->calc.itd_delay_left / 32.0f, COL_LEFT);
    textf_px(x + 138.0f, ty, 1, COL_TEXT, "%2u", (unsigned)f->calc.itd_delay_left);
    ty += 12.0f;
    text_px(x + 40.0f, ty, 1, COL_RIGHT, "R");
    bar_px(x + 50.0f, ty, 82.0f, 8.0f, (float)f->calc.itd_delay_right / 32.0f, COL_RIGHT);
    textf_px(x + 138.0f, ty, 1, COL_TEXT, "%2u", (unsigned)f->calc.itd_delay_right);
    ty += 10.0f;
    text_px(x + 40.0f, ty, 1, COL_DIM, "samples @48k");
    ty += 16.0f;

    /* MixBins in speaker order: FL C FR SL SR LFE (MixBin 0 4 1 2 3 5) */
    text_px(x + 8.0f, ty, 1, COL_DIM, "MIXBINS (5.1 PAN)");
    ty += 12.0f;
    {
        static const int bins[6] = { 0, 4, 1, 2, 3, 5 };
        static const char *const names[6] = { "FL", "C", "FR", "SL", "SR", "LFE" };
        const float bh = 40.0f;
        int i;
        for (i = 0; i < 6; i++) {
            float bx = x + 12.0f + (float)i * 27.0f;
            float v = (float)f->calc.mixbin_gain[bins[i]] / 255.0f;
            uint32_t c = (bins[i] == 0 || bins[i] == 2) ? COL_LEFT :
                         (bins[i] == 1 || bins[i] == 3) ? COL_RIGHT :
                         (bins[i] == 5) ? 0xFFAB47BCu : COL_TEXT;
            rect_px(bx, ty, 16.0f, bh, 0x80000000u);
            rect_px(bx, ty + bh * (1.0f - v), 16.0f, bh * v, c);
            frame_px(bx, ty, 16.0f, bh, COL_PANEL_EDGE);
            text_px(bx + 8.0f - (float)strlen(names[i]) * 4.0f, ty + bh + 3.0f, 1, COL_DIM, names[i]);
        }
    }
}

/* Mode status line above the legend */
static void hud_status(const showcase_app_t *app, const scene_focus_t *f) {
    char line[96];

    switch (app->current_mode) {
        case MODE_ORBIT_3D:
            snprintf(line, sizeof(line), "ORBIT  radius %s m   speed %s rad/s",
                     showcase_fx(app->orbit_radius, 1, false, 0), showcase_fx(app->orbit_speed, 2, true, 0));
            break;
        case MODE_DOPPLER_FLYBY:
            if (app->doppler_flying) {
                snprintf(line, sizeof(line), "PROJECTILE  x %s m   42 m/s   pitch x%s",
                         showcase_fx(app->doppler_pos_x, 1, true, 5), showcase_fx(f->calc.doppler_pitch, 3, false, 0));
            } else {
                snprintf(line, sizeof(line), "PROJECTILE READY - launch it to hear the pitch bend");
            }
            break;
        case MODE_SURROUND_51: {
            static const char *const names[6] = {
                "FRONT LEFT (MixBin 0)", "CENTER (MixBin 4)", "FRONT RIGHT (MixBin 1)",
                "SURROUND RIGHT (MixBin 3)", "SURROUND LEFT (MixBin 2)", "SUBWOOFER LFE (MixBin 5)"
            };
            snprintf(line, sizeof(line), "SPEAKER %d/6  %s", app->surround_channel_idx + 1,
                     names[app->surround_channel_idx % 6]);
            break;
        }
        case MODE_POLYPHONY_STRESS:
            snprintf(line, sizeof(line), "%u sources in hardware slots, %u waiting virtual",
                     (unsigned)apu_voice_mgr_get_active_hw_count(),
                     (unsigned)apu_voice_mgr_get_virtual_standby_count());
            break;
        case MODE_BGM_2D_CONCURRENT:
            snprintf(line, sizeof(line), "2D MUSIC %s (direct FL/FR)  +  3D HELICOPTER at 5 m",
                     app->bgm_playing ? "PLAYING" : "PAUSED");
            break;
        default:
            line[0] = '\0';
            break;
    }

    panel_px(0.0f, 334.0f, (float)SHOWCASE_SCENE_WIDTH, 30.0f);
    text_px(32.0f, 340.0f, 1, COL_TEXT, line);
    {
        /* Where the sound comes from, then frame timing and audio health */
        float x = 32.0f + text_px(32.0f, 352.0f, 1, COL_DIM, s_audio_output
                                  ? "CPU (MMX) mix to AC97 |" : "Model only, silent |");
        if (s_perf_valid) {
            char perf[80];
            snprintf(perf, sizeof(perf), " %s fps  scene %s ms  mix %s ms  %u src  %u xrun",
                     showcase_fx(s_perf.fps, 0, false, 0), showcase_fx(s_perf.scene_ms, 1, false, 0),
                     showcase_fx(s_perf.audio_ms, 1, false, 0), (unsigned)s_perf.voices,
                     (unsigned)s_perf.underruns);
            text_px(x, 352.0f, 1, s_perf.underruns ? COL_WARN : COL_DIM, perf);
        }
    }
}

/* Legend icons; each returns the width it used */
static float icon_letter(float x, float cy, uint32_t fill, uint32_t ink, const char *letter) {
    disc_px(x + 8.0f, cy, 8.0f, fill);
    text_px(x + 8.0f - (float)strlen(letter) * 4.0f, cy - 4.0f, 1, ink, letter);
    return 16.0f;
}

static float icon_pill(float x, float cy, const char *label) {
    float w = (float)strlen(label) * 8.0f + 8.0f;
    rect_px(x, cy - 7.0f, w, 14.0f, 0xFF455A64u);
    frame_px(x, cy - 7.0f, w, 14.0f, 0xFF90A4AEu);
    text_px(x + 4.0f, cy - 4.0f, 1, COL_TEXT, label);
    return w;
}

static float icon_dpad(float x, float cy, bool horiz, bool vert, int only /* -1 left, +1 right, 0 both */) {
    const float c = x + 8.0f;
    rect_px(c - 2.5f, cy - 8.0f, 5.0f, 16.0f, 0xFF546E7Au);
    rect_px(c - 8.0f, cy - 2.5f, 16.0f, 5.0f, 0xFF546E7Au);
    if (vert) {
        rect_px(c - 2.5f, cy - 8.0f, 5.0f, 5.0f, COL_TEXT);
        rect_px(c - 2.5f, cy + 3.0f, 5.0f, 5.0f, COL_TEXT);
    }
    if (horiz) {
        if (only <= 0) {
            rect_px(c - 8.0f, cy - 2.5f, 5.0f, 5.0f, COL_TEXT);
        }
        if (only >= 0) {
            rect_px(c + 3.0f, cy - 2.5f, 5.0f, 5.0f, COL_TEXT);
        }
    }
    return 16.0f;
}

static float icon_stick(float x, float cy, const char *letter) {
    disc_px(x + 8.0f, cy, 8.0f, 0xFF37474Fu);
    ring_px(x + 8.0f, cy, 8.0f, 0xFFB0BEC5u);
    text_px(x + 4.0f, cy - 4.0f, 1, COL_TEXT, letter);
    return 16.0f;
}

static float icon_trigger(float x, float cy, const char *label) {
    rect_px(x, cy - 7.0f, 20.0f, 14.0f, 0xFF37474Fu);
    rect_px(x + 2.0f, cy - 9.0f, 16.0f, 2.0f, 0xFFB0BEC5u);
    text_px(x + 2.0f, cy - 4.0f, 1, COL_TEXT, label);
    return 20.0f;
}

static float draw_button_icon(showcase_button_t b, float x, float cy) {
    switch (b) {
        case SHOWCASE_BTN_A:     return icon_letter(x, cy, 0xFF43A047u, 0xFF0B1A0Bu, "A");
        case SHOWCASE_BTN_B:     return icon_letter(x, cy, 0xFFE53935u, 0xFF1A0B0Bu, "B");
        case SHOWCASE_BTN_X:     return icon_letter(x, cy, 0xFF1E88E5u, 0xFF0B0F1Au, "X");
        case SHOWCASE_BTN_Y:     return icon_letter(x, cy, 0xFFFDD835u, 0xFF1A170Bu, "Y");
        case SHOWCASE_BTN_WHITE: return icon_letter(x, cy, 0xFFF5F5F5u, 0xFF202020u, "");
        case SHOWCASE_BTN_BLACK: {
            float w = icon_letter(x, cy, 0xFF111111u, 0xFFE0E0E0u, "");
            ring_px(x + 8.0f, cy, 8.0f, 0xFF9E9E9Eu);
            return w;
        }
        case SHOWCASE_BTN_START: return icon_pill(x, cy, "START");
        case SHOWCASE_BTN_BACK:  return icon_pill(x, cy, "BACK");
        case SHOWCASE_BTN_DPAD_LEFT_RIGHT: return icon_dpad(x, cy, true, false, 0);
        case SHOWCASE_BTN_DPAD_UP_DOWN:    return icon_dpad(x, cy, false, true, 0);
        case SHOWCASE_BTN_DPAD_LEFT:       return icon_dpad(x, cy, true, false, -1);
        case SHOWCASE_BTN_DPAD_RIGHT:      return icon_dpad(x, cy, true, false, 1);
        case SHOWCASE_BTN_LSTICK:  return icon_stick(x, cy, "L");
        case SHOWCASE_BTN_RSTICK:  return icon_stick(x, cy, "R");
        case SHOWCASE_BTN_LTRIGGER: return icon_trigger(x, cy, "LT");
        case SHOWCASE_BTN_RTRIGGER: return icon_trigger(x, cy, "RT");
        default: return 0.0f;
    }
}

static void hud_legend(const showcase_app_t *app, const showcase_input_t *input) {
    showcase_legend_entry_t entries[SHOWCASE_LEGEND_MAX_ENTRIES];
    const float x = 24.0f, y = 368.0f, w = 592.0f, h = 92.0f;
    const float row_h = 17.0f;
    int n = showcase_app_get_legend(app, entries, SHOWCASE_LEGEND_MAX_ENTRIES);
    int mode_rows = 0, global_rows = 0;
    int i;

    panel_px(x, y, w, h);
    text_px(x + 8.0f, y + 6.0f, 1, COL_ACCENT, "CONTROLS");
    if (input && !input->controller_connected) {
        text_px(x + 88.0f, y + 6.0f, 1, COL_WARN, "no gamepad: auto-tour changes mode every few s");
    }

    /* Mode and listener entries in the left column, global ones in the right */
    for (i = 0; i < n; i++) {
        bool global = entries[i].global;
        float col_x = global ? (x + w * 0.5f + 4.0f) : (x + 8.0f);
        float cy = y + 28.0f + (float)(global ? global_rows++ : mode_rows++) * row_h;
        float pen = col_x;
        int b;

        for (b = 0; b < SHOWCASE_LEGEND_MAX_BUTTONS; b++) {
            if (entries[i].buttons[b] == SHOWCASE_BTN_NONE) {
                continue;
            }
            if (b > 0) {
                text_px(pen + 2.0f, cy - 4.0f, 1, COL_DIM, entries[i].combo ? "+" : "/");
                pen += 12.0f;
            }
            pen += draw_button_icon(entries[i].buttons[b], pen, cy);
        }
        pen = (pen + 10.0f > col_x + 72.0f) ? (pen + 10.0f) : (col_x + 72.0f);
        if (entries[i].hold) {
            pen += text_px(pen, cy - 4.0f, 1, COL_WARN, "HOLD ");
        }
        text_px(pen, cy - 4.0f, 1, COL_TEXT, entries[i].action);
    }
}

/*
 * ============================================================================
 * Public API
 * ============================================================================
 */

int showcase_scene_init(void) {
    s_vert_count = 0;
    s_batch_count = 0;
    s_overflow = false;
    s_frame = 0;
    s_solid_count = 0;

    if (s_verts == NULL) {
#if defined(HAS_PBKIT)
        s_verts = (showcase_scene_vertex_t *)MmAllocateContiguousMemoryEx(
            SCENE_MAX_VERTICES * sizeof(showcase_scene_vertex_t), 0, 0x3ffb000, 0,
            PAGE_READWRITE | PAGE_WRITECOMBINE);
#else
        s_verts = (showcase_scene_vertex_t *)malloc(SCENE_MAX_VERTICES * sizeof(showcase_scene_vertex_t));
#endif
        if (s_verts == NULL) {
            return -1;
        }
    }

#if defined(HAS_PBKIT)
    {
        /* Vertex shader: positions arrive in NDC, the constant maps them to the screen */
        static const uint32_t vs_program[] = {
#include "vs.inl"
        };
        uint32_t *p;
        unsigned int i;

        memset(s_viewport, 0, sizeof(s_viewport));
        s_viewport[0][0] = SHOWCASE_SCENE_WIDTH / 2.0f;
        s_viewport[1][1] = SHOWCASE_SCENE_HEIGHT / -2.0f;
        s_viewport[2][2] = 65536.0f;
        s_viewport[3][3] = 1.0f;
        s_viewport[3][0] = SHOWCASE_SCENE_WIDTH / 2.0f;
        s_viewport[3][1] = SHOWCASE_SCENE_HEIGHT / 2.0f;

        p = pb_begin();
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
        p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                     MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                     MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
        pb_end(p);

        p = pb_begin();
        p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
        pb_end(p);

        for (i = 0; i < sizeof(vs_program) / 16; i++) {
            p = pb_begin();
            pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
            memcpy(p, &vs_program[i * 4], 4 * 4);
            p += 4;
            pb_end(p);
        }

        p = pb_begin();
#include "ps.inl"
        pb_end(p);
    }
#endif

    return 0;
}

void showcase_scene_build(const showcase_app_t *app, const showcase_input_t *input) {
    scene_focus_t focus;
    float fwd[3], ffwd[3], fright[3];
    float eye[3], target[3];
    float want_back = 9.0f, want_height = 5.5f;

    s_vert_count = 0;
    s_batch_count = 0;
    s_overflow = false;
    s_solid_count = 0;
    s_frame++;

    if (!app || !s_verts) {
        return;
    }

    /* Chase camera behind the listener: what is in front of the listener is
     * in front of the camera, so left/right on screen match left/right ears */
    switch (app->current_mode) {
        case MODE_ORBIT_3D:
            want_back = clampf(app->orbit_radius * 1.1f + 5.0f, 7.0f, 22.0f);
            want_height = want_back * 0.6f;
            break;
        case MODE_DOPPLER_FLYBY:
            want_back = 11.0f;
            want_height = 5.0f;
            break;
        case MODE_SURROUND_51:
            want_back = 11.0f;
            want_height = 10.0f;
            break;
        case MODE_POLYPHONY_STRESS:
            want_back = 15.0f;
            want_height = 13.0f;
            break;
        default:
            break;
    }
    s_cam_back += (want_back - s_cam_back) * 0.08f;
    s_cam_height += (want_height - s_cam_height) * 0.08f;

    listener_vectors(app, fwd, ffwd, fright);
    memcpy(s_listener_pos, app->listener_pos, sizeof(s_listener_pos));
    set3(eye, s_listener_pos[0] - ffwd[0] * s_cam_back, s_listener_pos[1] + s_cam_height,
         s_listener_pos[2] - ffwd[2] * s_cam_back);
    set3(target, s_listener_pos[0] + ffwd[0] * 3.0f, s_listener_pos[1] - 0.8f, s_listener_pos[2] + ffwd[2] * 3.0f);
    camera_setup(eye, target);

    pick_focus(app, &focus);

    /* World, back to front: floor, guides, then solids sorted by depth */
    draw_grid();
    if (app->current_mode != MODE_POLYPHONY_STRESS && app->current_mode != MODE_SURROUND_51) {
        draw_distance_rings(&focus);
    }
    draw_mode_world(app, &focus);
    draw_listener(app);
    flush_solids();

    /* HUD */
    hud_top_bar(app, input);
    hud_radar(app, &focus);
    hud_spatial_panel(app, &focus);
    hud_status(app, &focus);
    hud_legend(app, input);
}

void showcase_scene_submit(void) {
#if defined(HAS_PBKIT)
    uint32_t *p;
    uint32_t b;
    int i;

    if (!s_verts || s_vert_count == 0) {
        return;
    }

    p = pb_begin();
    /* Everything is drawn in order: no depth test, no culling; alpha blending on */
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
    p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
    p = pb_push1(p, NV097_SET_BLEND_EQUATION, NV097_SET_BLEND_EQUATION_V_FUNC_ADD);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR, NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
    p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR, NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA);
    p = pb_push1(p, NV097_SET_LINE_WIDTH, 1 << 3);
    /* Viewport matrix in c[0] (constant slot 96, see vs.inl) */
    p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);
    pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
    memcpy(p, s_viewport, 16 * 4);
    p += 16;
    pb_end(p);

    /* Attribute 0: float3 position, attribute 3: D3D colour; all others off */
    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);

    p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 0 * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 3) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(showcase_scene_vertex_t)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 0 * 4, (uint32_t)&s_verts[0].x & 0x03ffffff);
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + 3 * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_D3D) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, 4) |
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, sizeof(showcase_scene_vertex_t)));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + 3 * 4, (uint32_t)&s_verts[0].color & 0x03ffffff);
    pb_end(p);

    for (b = 0; b < s_batch_count; b++) {
        /* DRAW_ARRAYS takes at most 256 vertices per word; 252 keeps lines,
         * triangles and quads whole across words */
        const uint32_t chunk = 252;
        uint32_t op = (s_batches[b].prim == PRIM_LINES) ? NV097_SET_BEGIN_END_OP_LINES :
                      (s_batches[b].prim == PRIM_TRIS) ? NV097_SET_BEGIN_END_OP_TRIANGLES :
                      NV097_SET_BEGIN_END_OP_QUADS;
        uint32_t start = s_batches[b].first;
        uint32_t left = s_batches[b].count;

        p = pb_begin();
        p = pb_push1(p, NV097_SET_BEGIN_END, op);
        pb_end(p);

        while (left > 0) {
            uint32_t words = 0;
            uint32_t *hdr;

            p = pb_begin();
            hdr = p++;
            while (left > 0 && words < 64) {
                uint32_t n = (left > chunk) ? chunk : left;
                *(p++) = MASK(NV097_DRAW_ARRAYS_COUNT, n - 1) | MASK(NV097_DRAW_ARRAYS_START_INDEX, start);
                start += n;
                left -= n;
                words++;
            }
            /* bit 30: every word goes to the same method (NV097_DRAW_ARRAYS) */
            pb_push(hdr, 0x40000000 | NV097_DRAW_ARRAYS, words);
            pb_end(p);
        }

        p = pb_begin();
        p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
        pb_end(p);
    }
#endif
}

void showcase_scene_set_perf(const showcase_perf_t *perf) {
    if (perf) {
        s_perf = *perf;
        s_perf_valid = true;
    }
}

void showcase_scene_set_audio_output(bool playing) {
    s_audio_output = playing;
}

void showcase_scene_shutdown(void) {
    if (s_verts) {
#if defined(HAS_PBKIT)
        MmFreeContiguousMemory(s_verts);
#else
        free(s_verts);
#endif
        s_verts = NULL;
    }
    s_vert_count = 0;
    s_batch_count = 0;
}

void showcase_scene_get_stats(showcase_scene_stats_t *out) {
    if (!out) {
        return;
    }
    out->vertices = s_verts;
    out->vertex_count = s_vert_count;
    out->vertex_capacity = SCENE_MAX_VERTICES;
    out->batch_count = s_batch_count;
    out->overflow = s_overflow;
}
