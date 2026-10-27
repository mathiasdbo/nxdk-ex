/*
 * pbkit quality test (D3D8 gap, batch 4): render to texture and multisampling.
 * Cycles through configurations with pb_kill/pb_init:
 *   0 RTT swizzled: a 128x128 swizzled A8R8G8B8 render target (no depth buffer)
 *     gets four colored quadrants and a magenta square drawn last over the
 *     center; the CPU reads texels back with pb_swizzle_offset, then the
 *     target is sampled as a SZ_A8R8G8B8 texture on a 256x256 quad in the back
 *     buffer and the quad's pixels are read back.
 *   1 RTT linear: the same with a linear (pitch 512) target and its own depth
 *     buffer: the magenta square is drawn far behind the quadrants and must be
 *     hidden (depth test in the render target); sampled as LU_IMAGE_A8R8G8B8.
 *   2 MS 2x, 3 MS 4x, 4 MS off (reference): a white triangle on black whose
 *     hypotenuse x + y = 800.1 crosses pixels (x, 799 - x) at less than half a
 *     pixel from their center: without multisampling those pixels are pure
 *     white, with 2x/4x the resolve must blend them (2x: 1/2, 4x: 3/4 with
 *     xemu's sample positions). Also checks fully inside/outside pixels, the
 *     raw samples in pb_multisample_buffer(), and that every presentation
 *     interval still flips at the expected rate with the resolve.
 * Results are shown with pb_print, sent to DbgPrint and kept in a magic tagged
 * block (g_results, "PBQUALRS").
 *
 * Build with make CONFIG=n to run a single configuration without pb_kill.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))

#define WIDTH  640
#define HEIGHT 480

#define HOLD_FRAMES   (15 * 60) /* ~15 s per configuration before moving on */
#define RATE_MS       2000      /* presentation interval measurement window */

enum { KIND_RTT_SWIZZLED, KIND_RTT_LINEAR, KIND_MS };

typedef struct {
    const char *name;
    int kind;
    int ms; /* PB_MS_* */
} config_t;

static const config_t configs[] = {
    {"0 RTT swizzled 128x128", KIND_RTT_SWIZZLED, PB_MS_NONE},
    {"1 RTT linear 128x128 + depth", KIND_RTT_LINEAR, PB_MS_NONE},
    {"2 multisample 2x", KIND_MS, PB_MS_2X},
    {"3 multisample 4x", KIND_MS, PB_MS_4X},
    {"4 multisample off (reference)", KIND_MS, PB_MS_NONE},
};
#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))

#define RT_SIZE  128
#define RT_PITCH (RT_SIZE * 4)

#define COLOR_CLEAR    0xFF404040
#define COLOR_RT_CLEAR 0xFF202020
#define COLOR_TL       0xFF0000
#define COLOR_TR       0x00FF00
#define COLOR_BL       0x0000FF
#define COLOR_BR       0xFFFF00
#define COLOR_CENTER   0xFF00FF

/* the render target sampled on a 256x256 quad at (QX, QY): 2 screen pixels per texel */
#define QX 320
#define QY 210

/* texels checked: the four quadrants, the center (magenta without depth, the quadrant with) */
static const int texels[5][2] = {{16, 16}, {112, 16}, {16, 112}, {112, 112}, {60, 60}};

/* multisample triangle (TX, TY) (TX + TL, TY) (TX, TY + TL): hypotenuse x + y = EDGE = 800.1
 * crosses pixels (x, 799 - x) at 0.1 pixel from their center, kept below the report text */
#define TX      300.0f
#define TY      200.0f
#define TL      300.1f
#define EDGE_Y  799
#define EDGE_X0 330
#define EDGE_X1 570

static const uint32_t intervals[4] = {PB_PRESENT_INTERVAL_ONE, PB_PRESENT_INTERVAL_TWO, PB_PRESENT_INTERVAL_IMMEDIATE,
                                      PB_PRESENT_INTERVAL_ONE_OR_IMMEDIATE};
/* flips per VBlank x1000, accepted range per interval (light scene) */
static const uint32_t fpv_min[4] = {900, 450, 900, 900};
static const uint32_t fpv_max[4] = {1010, 520, 1000000, 1010};

typedef struct {
    float pos[4];   /* x, y, z, w */
    float color[3]; /* r, g, b */
    float tex[2];   /* u, v */
} __attribute__((packed)) Vertex;

typedef struct {
    uint32_t done, pass, fails;
} summary_t;

/* Results, findable in memory by its magic tag */
typedef struct {
    char magic[8];
    uint32_t config;
    uint32_t frame;
    uint32_t state; /* 0 = rendering, 1 = results valid */
    uint32_t pass;  /* 1 = all checks passed */
    uint32_t fails; /* bit mask of failed checks (see run_config) */
    uint32_t init_status;
    /* render to texture */
    uint32_t rt_addr, rt_depth;
    int32_t rt_set;              /* pb_set_render_target result */
    int32_t rt_reject;           /* result for an invalid target (non power of two swizzled) */
    uint32_t texel[5], texel_expect[5];
    uint32_t quad[5], quad_expect[5]; /* the sampled quad on screen, same texels */
    uint32_t swz_offset_check;   /* pb_swizzle_offset spot checks passed */
    /* multisampling */
    uint32_t ms_mode, ms_addr, ms_pitch, fb_pitch, ds_pitch;
    uint32_t inside, outside;    /* resolved pixels fully inside / outside the triangle */
    uint32_t edge_pixels, edge_blend, edge_pure, edge_min, edge_max;
    uint32_t edge_sample[8];     /* resolved edge pixel (450, 349) and its raw samples */
    uint32_t fpv[4];             /* flips per VBlank x1000 for ONE, TWO, IMMEDIATE, ONE_OR_IMMEDIATE */
    summary_t summary[NUM_CONFIGS];
} results_t;

static volatile results_t g_results __attribute__((aligned(16))) = {"PBQUALRS"};

static volatile uint32_t cb_swap;

static Vertex *alloc_vertices;
static uint32_t *rt_color, *rt_depth;
static int status_lines;
static char status[20][64];

static void init_shader (void);
static void set_ps_color (void);
static void set_ps_texture (void);
static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data);

static void swap_callback (DWORD frame, DWORD vbl_count)
{
    (void)frame;
    (void)vbl_count;
    cb_swap++;
}

static void log_line (const char *fmt, ...)
{
    va_list ap;
    if (status_lines >= 20) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(status[status_lines], sizeof(status[0]), fmt, ap);
    va_end(ap);
    DbgPrint("pbkit_quality: %s\n", status[status_lines]);
    status_lines++;
}

static void set_vertex (Vertex *v, float x, float y, float z, uint32_t color, float u, float t)
{
    v->pos[0] = x;
    v->pos[1] = y;
    v->pos[2] = z * pb_depth_scale();
    v->pos[3] = 1.0f;
    v->color[0] = ((color >> 16) & 0xFF) / 255.0f;
    v->color[1] = ((color >> 8) & 0xFF) / 255.0f;
    v->color[2] = (color & 0xFF) / 255.0f;
    v->tex[0] = u;
    v->tex[1] = t;
}

/* clockwise quad, texture coordinates u0,v0 .. u1,v1 */
static void set_quad (Vertex *v, float x0, float y0, float x1, float y1, float z, uint32_t color, float u0, float v0,
                      float u1, float v1)
{
    set_vertex(&v[0], x0, y0, z, color, u0, v0);
    set_vertex(&v[1], x1, y0, z, color, u1, v0);
    set_vertex(&v[2], x1, y1, z, color, u1, v1);
    set_vertex(&v[3], x0, y1, z, color, u0, v1);
}

static void draw (unsigned int mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 MASK(NV097_DRAW_ARRAYS_COUNT, (count - 1)) | MASK(NV097_DRAW_ARRAYS_START_INDEX, start));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}

/* Per frame state (the multisample resolve in pb_finished changes it): shaders, vertex arrays */
static void setup_draw_state (void)
{
    uint32_t *p;
    int i;

    init_shader();
    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);
    set_attrib_pointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4, sizeof(Vertex), &alloc_vertices[0].pos[0]);
    set_attrib_pointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof(Vertex), &alloc_vertices[0].color[0]);
    set_attrib_pointer(9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 2, sizeof(Vertex), &alloc_vertices[0].tex[0]);
}

/* Vertices: 0-15 render target quadrants, 16-19 center square, 20-23 sampled quad, 24-26 triangle,
 * 28-31 render target clear */
static void fill_vertices (const config_t *cfg)
{
    const float h = RT_SIZE / 2.0f, s = RT_SIZE;
    /* swizzled textures take normalized coordinates, linear ones texel coordinates */
    const float ts = (cfg->kind == KIND_RTT_SWIZZLED) ? 1.0f : (float)RT_SIZE;

    set_quad(&alloc_vertices[0], 0.0f, 0.0f, h, h, 0.1f, COLOR_TL, 0, 0, 0, 0);
    set_quad(&alloc_vertices[4], h, 0.0f, s, h, 0.1f, COLOR_TR, 0, 0, 0, 0);
    set_quad(&alloc_vertices[8], 0.0f, h, h, s, 0.1f, COLOR_BL, 0, 0, 0, 0);
    set_quad(&alloc_vertices[12], h, h, s, s, 0.1f, COLOR_BR, 0, 0, 0, 0);
    /* drawn last; far: hidden by the quadrants with a depth buffer */
    set_quad(&alloc_vertices[16], 32.0f, 32.0f, 96.0f, 96.0f, 0.9f, COLOR_CENTER, 0, 0, 0, 0);
    set_quad(&alloc_vertices[20], QX, QY, QX + 2 * RT_SIZE, QY + 2 * RT_SIZE, 0.5f, 0xFFFFFF, 0.0f, 0.0f, ts, ts);
    set_vertex(&alloc_vertices[24], TX, TY, 0.5f, 0xFFFFFF, 0, 0); /* clockwise */
    set_vertex(&alloc_vertices[25], TX + TL, TY, 0.5f, 0xFFFFFF, 0, 0);
    set_vertex(&alloc_vertices[26], TX, TY + TL, 0.5f, 0xFFFFFF, 0, 0);
    set_quad(&alloc_vertices[28], 0.0f, 0.0f, s, s, 0.99f, COLOR_RT_CLEAR & 0xFFFFFF, 0, 0, 0, 0);
}

static pb_render_target make_target (const config_t *cfg, bool with_depth)
{
    pb_render_target rt;
    memset(&rt, 0, sizeof(rt));
    rt.addr = (DWORD *)rt_color;
    rt.width = RT_SIZE;
    rt.height = RT_SIZE;
    rt.color_format = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
    rt.swizzled = (cfg->kind == KIND_RTT_SWIZZLED);
    rt.pitch = RT_PITCH;
    rt.depth_addr = with_depth ? (DWORD *)rt_depth : NULL;
    rt.depth_pitch = RT_PITCH;
    return rt;
}

/* Render to texture: draw into the target, then sample it on a quad in the back buffer */
static void draw_rtt (const config_t *cfg)
{
    pb_render_target rt = make_target(cfg, cfg->kind == KIND_RTT_LINEAR);
    uint32_t *p;
    DWORD format;

    g_results.rt_set = pb_set_render_target(&rt);
    if (rt.depth_addr) {
        pb_erase_depth_stencil_buffer(0, 0, RT_SIZE, RT_SIZE); /* the target's own depth buffer */
    }
    /* no pb_fill (NV097_CLEAR_SURFACE) on a swizzled surface: xemu asserts, the target is
     * cleared with a quad (the far plane: depth test passes, the quadrants come nearer) */
    setup_draw_state();
    set_ps_color();
    draw(NV097_SET_BEGIN_END_OP_QUADS, 28, 4);
    draw(NV097_SET_BEGIN_END_OP_QUADS, 0, 20);

    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT);
    pb_fill(0, 0, WIDTH, HEIGHT, COLOR_CLEAR);

    /* the render target must be in memory before the texture unit reads it */
    if (cfg->kind == KIND_RTT_SWIZZLED) {
        format = 2 | (1 << 3) | (2 << 4) | (NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A8R8G8B8 << 8) | (1 << 16) | (7 << 20) |
                 (7 << 24); /* DMA B, border color, 2D, 1 level, 2^7 x 2^7 */
    } else {
        format = 2 | (1 << 3) | (2 << 4) | (NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8 << 8) | (1 << 16);
    }
    p = pb_begin();
    p = pb_push1(p, NV097_WAIT_FOR_IDLE, 0);
    p = pb_push2(p, NV097_SET_TEXTURE_OFFSET, (DWORD)rt_color & 0x03FFFFFF, format);
    p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS, 0x00030303);
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0x4003FFC0);
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL1, RT_PITCH << 16);
    p = pb_push1(p, NV097_SET_TEXTURE_FILTER, 0x01012000); /* nearest: exact texels */
    p = pb_push1(p, NV097_SET_TEXTURE_IMAGE_RECT, (RT_SIZE << 16) | RT_SIZE);
    pb_end(p);
    set_ps_texture();
    draw(NV097_SET_BEGIN_END_OP_QUADS, 20, 4);
    p = pb_begin();
    p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0, 0);
    pb_end(p);
    set_ps_color();
}

static void draw_ms (void)
{
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT);
    pb_fill(0, 0, WIDTH, HEIGHT, 0xFF000000);
    setup_draw_state();
    set_ps_color();
    draw(NV097_SET_BEGIN_END_OP_TRIANGLES, 24, 3);
}

/* One frame; returns the back buffer it was drawn (or resolved) into */
static uint32_t render_frame (const config_t *cfg, bool text)
{
    uint32_t back;
    int i;

    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
    back = (uint32_t)pb_back_buffer();
    if (cfg->kind == KIND_MS) {
        draw_ms();
    } else {
        draw_rtt(cfg);
    }
    if (text) {
        pb_erase_text_screen();
        for (i = 0; i < status_lines; i++) {
            pb_print("%s\n", status[i]);
        }
        pb_draw_text_screen();
    }
    while (pb_busy()) {
    }
    while (pb_finished()) {
    }
    return back;
}

static uint32_t px32 (uint32_t base, uint32_t pitch, int x, int y)
{
    return *(volatile uint32_t *)(base + y * pitch + x * 4) & 0xFFFFFF;
}

static bool close_color (uint32_t a, uint32_t b)
{
    int i;
    for (i = 0; i < 24; i += 8) {
        int ca = (a >> i) & 0xFF, cb = (b >> i) & 0xFF;
        if (ca > cb + 2 || cb > ca + 2) return false;
    }
    return true;
}

static uint32_t check_rtt (const config_t *cfg, uint32_t fb)
{
    static const uint32_t quad_colors[4] = {COLOR_TL, COLOR_TR, COLOR_BL, COLOR_BR};
    uint32_t fails = 0;
    bool depth = (cfg->kind == KIND_RTT_LINEAR);
    pb_render_target bad;
    int i;

    /* pb_swizzle_offset: known Morton offsets (x in bit 0), non-square too */
    g_results.swz_offset_check = (pb_swizzle_offset(1, 0, 128, 128, 1) == 1) &&
                                 (pb_swizzle_offset(0, 1, 128, 128, 1) == 2) &&
                                 (pb_swizzle_offset(3, 3, 128, 128, 4) == 15 * 4) &&
                                 (pb_swizzle_offset(127, 127, 128, 128, 4) == (128 * 128 - 1) * 4) &&
                                 (pb_swizzle_offset(0, 1, 8, 2, 1) == 2) &&       /* 8x2: x0 y0 x1 x2 */
                                 (pb_swizzle_offset(4, 0, 8, 2, 1) == 8) &&
                                 (pb_swizzle_offset(0, 3, 2, 8, 1) == 6);         /* 2x8: x0 y0 y1 y2 */
    if (!g_results.swz_offset_check) fails |= 0x001;

    /* an invalid target is rejected and changes nothing */
    bad = make_target(cfg, false);
    bad.swizzled = true;
    bad.width = 100;
    g_results.rt_reject = pb_set_render_target(&bad);
    pb_target_back_buffer();
    if ((g_results.rt_set != 0) || (g_results.rt_reject != -1)) fails |= 0x002;
    log_line("%s set %d reject %d swz %s", cfg->name, g_results.rt_set, g_results.rt_reject,
             g_results.swz_offset_check ? "ok" : "FAIL");

    for (i = 0; i < 5; i++) {
        int x = texels[i][0], y = texels[i][1];
        uint32_t off = cfg->kind == KIND_RTT_SWIZZLED ? pb_swizzle_offset(x, y, RT_SIZE, RT_SIZE, 4)
                                                      : (uint32_t)(y * RT_PITCH + x * 4);
        uint32_t expect = (i < 4) ? quad_colors[i] : (depth ? COLOR_TL : COLOR_CENTER);
        bool ok_t, ok_q;
        g_results.texel[i] = *(volatile uint32_t *)((uint8_t *)rt_color + off) & 0xFFFFFF;
        g_results.texel_expect[i] = expect;
        g_results.quad[i] = px32(fb, pb_back_buffer_pitch(), QX + 2 * x, QY + 2 * y);
        g_results.quad_expect[i] = expect;
        ok_t = close_color(g_results.texel[i], expect);
        ok_q = close_color(g_results.quad[i], expect);
        if (!ok_t) fails |= 0x010u << i;
        if (!ok_q) fails |= 0x200u << i;
        log_line("texel %3d,%3d %06X quad %06X exp %06X %s", x, y, g_results.texel[i], g_results.quad[i], expect,
                 (ok_t && ok_q) ? "ok" : "FAIL");
    }
    return fails;
}

static bool pure (uint32_t c)
{
    return (c == 0x000000) || (c == 0xFFFFFF);
}

/* Flips per VBlank over RATE_MS for each presentation interval (swap callback count) */
static uint32_t check_intervals (const config_t *cfg)
{
    uint32_t fails = 0;
    int k;

    pb_set_swap_callback(swap_callback);
    for (k = 0; k < 4; k++) {
        uint32_t t0, s0, v0, ds, dv;
        pb_set_presentation_interval(intervals[k]);
        render_frame(cfg, false);
        render_frame(cfg, false);
        t0 = KeTickCount;
        s0 = cb_swap;
        v0 = pb_get_vbl_counter();
        while ((uint32_t)(KeTickCount - t0) < RATE_MS) {
            /* no pb_wait_for_vbl: IMMEDIATE must be able to flip more than once per VBlank */
            pb_reset();
            draw_ms();
            while (pb_busy()) {
            }
            while (pb_finished()) {
            }
        }
        ds = cb_swap - s0;
        dv = pb_get_vbl_counter() - v0;
        g_results.fpv[k] = dv ? (ds * 1000 / dv) : 0;
        if ((g_results.fpv[k] < fpv_min[k]) || (g_results.fpv[k] > fpv_max[k])) fails |= 0x100u << k;
    }
    pb_set_presentation_interval(PB_PRESENT_INTERVAL_ONE);
    pb_set_swap_callback(NULL);
    log_line("flips/VBL x1000 ONE %u TWO %u IMM %u OOI %u %s", g_results.fpv[0], g_results.fpv[1], g_results.fpv[2],
             g_results.fpv[3], (fails & 0xF00) ? "FAIL" : "ok");
    return fails;
}

static uint32_t check_ms (const config_t *cfg, uint32_t fb)
{
    uint32_t fails = 0, pitch = pb_back_buffer_pitch();
    uint32_t ms = (uint32_t)pb_multisample_buffer(), msp = pb_multisample_pitch();
    uint32_t sx = (cfg->ms != PB_MS_NONE) ? 2 : 1, sy = (cfg->ms == PB_MS_4X) ? 2 : 1;
    int x, i;

    g_results.ms_mode = pb_get_multisample();
    g_results.ms_addr = ms;
    g_results.ms_pitch = msp;
    g_results.fb_pitch = pitch;
    g_results.ds_pitch = pb_depth_stencil_pitch();

    /* 1: buffers: sample buffer and depth buffer at the scaled size, frame buffers 1x */
    {
        bool ok = (pitch == WIDTH * 4) && (g_results.ds_pitch == WIDTH * 4 * sx) &&
                  ((cfg->ms == PB_MS_NONE) ? (ms == 0) : (ms != 0 && msp == WIDTH * 4 * sx));
        if (!ok) fails |= 0x001;
        log_line("%s", cfg->name);
        log_line("pitch fb %u ds %u ms %u buf %08X %s", pitch, g_results.ds_pitch, msp, ms, ok ? "ok" : "FAIL");
    }

    /* 2: inside / outside pixels are pure */
    g_results.inside = px32(fb, pitch, 330, 230);
    g_results.outside = px32(fb, pitch, 100, 400);
    if ((g_results.inside != 0xFFFFFF) || (g_results.outside != 0x000000)) fails |= 0x002;

    /* 3: edge pixels (x, 799 - x): blended with multisampling, pure without */
    g_results.edge_pixels = g_results.edge_blend = g_results.edge_pure = 0;
    g_results.edge_min = 0xFF;
    g_results.edge_max = 0;
    for (x = EDGE_X0; x < EDGE_X1; x++) {
        uint32_t c = px32(fb, pitch, x, EDGE_Y - x);
        uint32_t g = (c >> 8) & 0xFF;
        g_results.edge_pixels++;
        if (pure(c)) {
            g_results.edge_pure++;
        } else if (g >= 8 && g <= 247) {
            g_results.edge_blend++;
        }
        if (g < g_results.edge_min) g_results.edge_min = g;
        if (g > g_results.edge_max) g_results.edge_max = g;
    }
    if (cfg->ms == PB_MS_NONE) {
        if (g_results.edge_pure != g_results.edge_pixels) fails |= 0x004;
    } else {
        if (g_results.edge_blend * 10 < g_results.edge_pixels * 9) fails |= 0x004;
    }
    log_line("inside %06X outside %06X %s", g_results.inside, g_results.outside, (fails & 0x002) ? "FAIL" : "ok");
    log_line("edge %u px: blend %u pure %u G %u..%u %s", g_results.edge_pixels, g_results.edge_blend,
             g_results.edge_pure, g_results.edge_min, g_results.edge_max, (fails & 0x004) ? "FAIL" : "ok");

    /* 4: the resolved edge pixel (450, 349) and its raw samples */
    memset((void *)g_results.edge_sample, 0, sizeof(g_results.edge_sample));
    g_results.edge_sample[0] = px32(fb, pitch, 450, 349);
    if (ms) {
        for (i = 0; i < (int)(sx * sy); i++) {
            g_results.edge_sample[1 + i] = px32(ms, msp, 450 * sx + (i % sx), 349 * sy + (i / sx));
        }
    }
    log_line("pixel 450,349 %06X samples %06X %06X %06X %06X", g_results.edge_sample[0], g_results.edge_sample[1],
             g_results.edge_sample[2], g_results.edge_sample[3], g_results.edge_sample[4]);

    /* 5: presentation intervals with the resolve */
    fails |= check_intervals(cfg);
    return fails;
}

static void run_config (int index)
{
    const config_t *cfg = &configs[index];
    uint32_t fb = 0, fails = 0;
    int i, status_code, frame;

    status_lines = 0;
    memset((void *)&g_results.config, 0, offsetof(results_t, summary) - offsetof(results_t, config));
    g_results.config = index;

    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT)) {
        debugPrint("XVideoSetMode failed\n");
        Sleep(2000);
        return;
    }

    pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    pb_set_depth_format(NV097_SET_SURFACE_FORMAT_ZETA_Z24S8);
    pb_set_w_buffer(false); /* plain Z-buffer: depth = z */
    pb_set_multisample(cfg->ms);

    if ((status_code = pb_init())) {
        g_results.init_status = status_code;
        debugPrint("pb_init Error %d\n", status_code);
        Sleep(2000);
        return;
    }
    pb_show_front_screen();

    alloc_vertices = MmAllocateContiguousMemoryEx(32 * sizeof(Vertex), 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    rt_color = MmAllocateContiguousMemoryEx(RT_PITCH * RT_SIZE, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    rt_depth = MmAllocateContiguousMemoryEx(RT_PITCH * RT_SIZE, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);
    g_results.rt_addr = (uint32_t)rt_color;
    g_results.rt_depth = (uint32_t)rt_depth;
    fill_vertices(cfg);

    for (i = 0; i < 6; i++) {
        fb = render_frame(cfg, false);
    }
    /* let the last frame complete */
    for (i = 0; i < 4; i++) {
        pb_wait_for_vbl();
    }
    while (pb_busy()) {
    }

    if (cfg->kind == KIND_MS) {
        fails = check_ms(cfg, fb);
    } else {
        fails = check_rtt(cfg, fb);
    }

    log_line("RESULT: %s (fails 0x%03X)", fails ? "FAIL" : "PASS", fails);
    g_results.fails = fails;
    g_results.pass = fails ? 0 : 1;
    g_results.summary[index].done = 1;
    g_results.summary[index].pass = g_results.pass;
    g_results.summary[index].fails = fails;
    g_results.state = 1;

    /* Hold the frame with the report on screen */
#ifdef CONFIG
    for (frame = 0;; frame++) {
#else
    for (frame = 0; frame < HOLD_FRAMES; frame++) {
#endif
        render_frame(cfg, true);
        g_results.frame = frame;
    }

    pb_kill();
    MmFreeContiguousMemory(alloc_vertices);
    MmFreeContiguousMemory(rt_color);
    MmFreeContiguousMemory(rt_depth);
}

int main (void)
{
#ifdef CONFIG
    run_config(CONFIG);
#else
    while (1) {
        unsigned int i;
        for (i = 0; i < NUM_CONFIGS; i++) {
            run_config(i);
        }
    }
#endif
    return 0;
}

static void init_shader (void)
{
    uint32_t *p;
    int i;

    uint32_t vs_program[] = {
#include "vs.inl"
    };

    p = pb_begin();
    p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
    p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
                 MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE, NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
                     MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
                          NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
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
}

/* vertex color only */
static void set_ps_color (void)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_SHADER_STAGE_PROGRAM, 0); /* no texture stages */
#include "ps.inl"
    pb_end(p);
}

/* texture stage 0 only */
static void set_ps_texture (void)
{
    uint32_t *p = pb_begin();
#include "ps_tex.inl"
    pb_end(p);
}

static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
                 MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format) |
                     MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |
                     MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
    p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4, (uint32_t)data & 0x03ffffff);
    pb_end(p);
}
