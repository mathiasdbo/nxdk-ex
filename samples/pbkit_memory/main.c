/*
 * pbkit memory test (D3D8 gap, batch 2): cycles through frame/depth buffer
 * memory configurations with pb_kill/pb_init:
 *   - pbkit's own buffers (triple buffering), 32 and 16 bits,
 *   - no depth buffer (PB_DEPTH_NONE),
 *   - application supplied frame and depth buffers (pb_set_frame_buffers,
 *     pb_set_depth_stencil_buffer), 32 bits + Z24S8 and 16 bits + Z16,
 *   - double buffering (pb_back_buffers(1) + pb_wait_for_flip).
 * Each configuration draws two overlapping quads, the near one (B, blue)
 * first and the far one (A, red) last, then reads color and depth back with
 * the CPU: with a depth buffer the overlap is blue, without one it is red.
 * It also checks pitches, tiling (tile pitch rule), the buffer addresses and
 * the back buffer rotation. Results are shown with pb_print, sent to DbgPrint
 * and kept in a magic tagged block (g_results, "PBMEMRES").
 *
 * Build with make CONFIG=n to run a single configuration without pb_kill.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <pbkit/pbkit.h>
#include <stdarg.h>
#include <stdbool.h>
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
#define ROTATE_FRAMES 6         /* frames recorded for the back buffer rotation check */

typedef struct {
    const char *name;
    unsigned int color_fmt;
    int bpp;
    unsigned int depth_fmt; /* PB_DEPTH_NONE, Z16 or Z24S8 */
    bool app_buffers;       /* pb_set_frame_buffers + pb_set_depth_stencil_buffer */
    int back_buffers;       /* pb_back_buffers(): 2 triple, 1 double buffering */
    uint32_t fb_pitch;      /* expected pitches and tiling */
    uint32_t ds_pitch;
    bool fb_tiled;
    bool ds_tiled;
} config_t;

#define A8R8G8B8 NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8
#define R5G6B5   NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5
#define Z24S8    NV097_SET_SURFACE_FORMAT_ZETA_Z24S8
#define Z16      NV097_SET_SURFACE_FORMAT_ZETA_Z16

static const config_t configs[] = {
    {"0 pbkit buffers A8R8G8B8 Z24S8 triple", A8R8G8B8, 32, Z24S8, false, 2, 2560, 2560, true, true},
    {"1 no depth (PB_DEPTH_NONE) A8R8G8B8", A8R8G8B8, 32, PB_DEPTH_NONE, false, 2, 2560, 0, true, false},
    {"2 app buffers A8R8G8B8 Z24S8", A8R8G8B8, 32, Z24S8, true, 2, 2560, 2560, true, true},
    /* 1280 is not a tile pitch: frame buffers untiled; depth uses tile pitch 1536 */
    {"3 app buffers R5G6B5 Z16", R5G6B5, 16, Z16, true, 2, 1280, 1536, false, true},
    {"4 double buffering A8R8G8B8 Z24S8", A8R8G8B8, 32, Z24S8, false, 1, 2560, 2560, true, true},
    {"5 pbkit buffers R5G6B5 Z16", R5G6B5, 16, Z16, false, 2, 1280, 1536, false, true},
};
#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))

/* Quads, screen space, clockwise (= front face for pbkit) */
#define A_Z 0.9f /* far, drawn last */
#define B_Z 0.1f /* near, drawn first */
#define VERTEX_W 1.0f
#define COLOR_A     0xFF0000
#define COLOR_B     0x0000FF
#define COLOR_CLEAR 0x404040

static const float quad_a[4] = {100.0f, 100.0f, 400.0f, 380.0f}; /* x0, y0, x1, y1 */
static const float quad_b[4] = {240.0f, 160.0f, 540.0f, 420.0f};

/* Probe pixels: outside both quads, inside both, inside A only, inside B only */
static const int probes[4][2] = {{40, 40}, {320, 250}, {160, 150}, {480, 400}};
static const char *probe_names[4] = {"clear", "overlap", "A only", "B only"};

typedef struct {
    float pos[4];   /* x, y, z, w */
    float color[3]; /* r, g, b */
} __attribute__((packed)) Vertex;

/* Readback results, findable in memory by its magic tag */
typedef struct {
    char magic[8];
    uint32_t config;
    uint32_t frame;
    uint32_t state; /* 0 = rendering, 1 = results valid */
    uint32_t pass;  /* 1 = all checks passed */
    uint32_t fails; /* bit mask of failed checks (see run_config) */
    uint32_t color[4];        /* raw color read back: clear, overlap, A only, B only */
    uint32_t color_expect[4];
    uint32_t depth[4];        /* raw depth (stencil stripped for Z24S8) */
    uint32_t depth_expect[4];
    uint32_t fb_pitch, ds_pitch, fb_tiled, ds_tiled;
    uint32_t ds_addr;         /* pb_depth_stencil_buffer() */
    uint32_t app_fb, app_ds;  /* application buffers (0 when pbkit allocates) */
    uint32_t rotation[ROTATE_FRAMES]; /* pb_back_buffer() of consecutive frames */
    int32_t reject_count, reject_align; /* pb_init results of the invalid app buffer tests */
} results_t;

static volatile results_t g_results __attribute__((aligned(16))) = {"PBMEMRES"};

static Vertex *alloc_vertices;
static int status_lines;
static char status[16][64];

static void init_shader (void);
static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data);
static void draw_arrays (unsigned int mode, int start, int count);

static void log_line (const char *fmt, ...)
{
    va_list ap;
    if (status_lines >= 16) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(status[status_lines], sizeof(status[0]), fmt, ap);
    va_end(ap);
    DbgPrint("pbkit_memory: %s\n", status[status_lines]);
    status_lines++;
}

static uint32_t pack_color (unsigned int fmt, uint32_t argb)
{
    if (fmt == R5G6B5) {
        return ((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F);
    }
    return argb & 0xFFFFFF;
}

/* Per channel tolerance of 1 (dithering, rounding) */
static bool color_matches (unsigned int fmt, uint32_t got, uint32_t expect)
{
    static const int shifts32[3] = {16, 8, 0}, widths32[3] = {8, 8, 8};
    static const int shifts16[3] = {11, 5, 0}, widths16[3] = {5, 6, 5};
    const int *shifts = (fmt == R5G6B5) ? shifts16 : shifts32;
    const int *widths = (fmt == R5G6B5) ? widths16 : widths32;
    int i;
    for (i = 0; i < 3; i++) {
        int mask = (1 << widths[i]) - 1;
        int a = (got >> shifts[i]) & mask;
        int b = (expect >> shifts[i]) & mask;
        if ((a > b + 1) || (b > a + 1)) {
            return false;
        }
    }
    return true;
}

static uint32_t read_color (const config_t *cfg, const uint8_t *fb, uint32_t pitch, int x, int y)
{
    if (cfg->bpp == 32) {
        return *(const uint32_t *)(fb + y * pitch + x * 4) & 0xFFFFFF;
    }
    return *(const uint16_t *)(fb + y * pitch + x * 2);
}

static uint32_t read_depth (const config_t *cfg, const uint8_t *ds, uint32_t pitch, int x, int y)
{
    if (cfg->depth_fmt == Z16) {
        return *(const uint16_t *)(ds + y * pitch + x * 2);
    }
    return *(const uint32_t *)(ds + y * pitch + x * 4) >> 8;
}

static void fill_quad (Vertex *v, const float r[4], float z, uint32_t color, float scale)
{
    const float xy[4][2] = {{r[0], r[1]}, {r[2], r[1]}, {r[2], r[3]}, {r[0], r[3]}}; /* clockwise */
    int i;
    for (i = 0; i < 4; i++) {
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[2] = z * scale;
        v[i].pos[3] = VERTEX_W;
        v[i].color[0] = ((color >> 16) & 0xFF) / 255.0f;
        v[i].color[1] = ((color >> 8) & 0xFF) / 255.0f;
        v[i].color[2] = (color & 0xFF) / 255.0f;
    }
}

static void draw_scene (void)
{
    uint32_t *p;
    int i;

    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT); /* no-op without a depth buffer */
    pb_fill(0, 0, WIDTH, HEIGHT, COLOR_CLEAR);
    pb_erase_text_screen();

    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    pb_end(p);

    set_attrib_pointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4, sizeof(Vertex), &alloc_vertices[0].pos[0]);
    set_attrib_pointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof(Vertex), &alloc_vertices[0].color[0]);

    /* vertices 0-3: B (near) first, 4-7: A (far) last */
    draw_arrays(NV097_SET_BEGIN_END_OP_QUADS, 0, 8);
}

/* One frame: wait for a free back buffer, draw, optional report text, queue the flip */
static uint32_t render_frame (bool text)
{
    uint32_t back;
    int i;

    pb_wait_for_flip(); /* double buffering: the next back buffer is free once the last frame is shown */
    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
    back = (uint32_t)pb_back_buffer();
    draw_scene();
    if (text) {
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

/* Application buffers: one 16 Kb aligned contiguous block for the frame
 * buffers, one for the depth buffer, write combined, below 0x03FFB000 */
static void *alloc_app_buffer (uint32_t size)
{
    return MmAllocateContiguousMemoryEx((size + 0x3FFF) & ~0x3FFF, 0, 0x03FFB000, 0x4000,
                                        PAGE_READWRITE | PAGE_WRITECOMBINE);
}

static void set_app_buffers (const config_t *cfg, uint8_t *fb_block, uint8_t *ds_block, int count, uint32_t ds_offset)
{
    DWORD *addrs[3];
    uint32_t fb_size = cfg->fb_pitch * HEIGHT;
    int i;
    for (i = 0; i < 3; i++) {
        addrs[i] = (DWORD *)(fb_block + i * fb_size);
    }
    pb_set_frame_buffers(addrs, count, cfg->fb_pitch);
    pb_set_depth_stencil_buffer((DWORD *)(ds_block + ds_offset), cfg->ds_pitch);
}

static void run_config (int index)
{
    const config_t *cfg = &configs[index];
    uint8_t *fb, *ds, *app_fb = NULL, *app_ds = NULL;
    uint32_t fb_pitch, ds_pitch, scale_bits, crtc_before, crtc_after;
    uint32_t fails = 0;
    float scale;
    int i, j, status_code, frame, distinct;
    bool has_depth = (cfg->depth_fmt != PB_DEPTH_NONE);

    status_lines = 0;
    memset((void *)&g_results.config, 0, sizeof(g_results) - sizeof(g_results.magic));
    g_results.config = index;

    if (!XVideoSetMode(WIDTH, HEIGHT, cfg->bpp, REFRESH_DEFAULT)) {
        debugPrint("XVideoSetMode %d bpp failed\n", cfg->bpp);
        Sleep(2000);
        return;
    }

    pb_set_color_format(cfg->color_fmt, false);
    pb_set_depth_format(cfg->depth_fmt);
    pb_set_depth_float(false);
    pb_set_w_buffer(false); /* plain Z-buffer: depth = z */
    pb_back_buffers(cfg->back_buffers);
    pb_set_frame_buffers(NULL, 0, 0);
    pb_set_depth_stencil_buffer(NULL, 0);

    if (cfg->app_buffers) {
        app_fb = alloc_app_buffer(3 * cfg->fb_pitch * HEIGHT);
        app_ds = alloc_app_buffer(cfg->ds_pitch * HEIGHT + 0x40);
        if (!app_fb || !app_ds) {
            debugPrint("application buffer allocation failed\n");
            Sleep(2000);
            return;
        }
        g_results.app_fb = (uint32_t)app_fb;
        g_results.app_ds = (uint32_t)app_ds;

        /* invalid set-ups pb_init must reject: wrong count, misaligned depth buffer */
        set_app_buffers(cfg, app_fb, app_ds, 2, 0);
        g_results.reject_count = pb_init();
        if (g_results.reject_count == 0) {
            pb_kill();
        }
        set_app_buffers(cfg, app_fb, app_ds, 3, 0x20);
        g_results.reject_align = pb_init();
        if (g_results.reject_align == 0) {
            pb_kill();
        }

        set_app_buffers(cfg, app_fb, app_ds, 3, 0);
    }

    if ((status_code = pb_init())) {
        debugPrint("pb_init Error %d\n", status_code);
        Sleep(2000);
        return;
    }
    pb_show_front_screen();

    /* pb_show_depth_screen must not move the scan-out without a depth buffer */
    crtc_before = VIDEOREG(PCRTC_START);
    pb_show_depth_screen();
    crtc_after = VIDEOREG(PCRTC_START);
    pb_show_front_screen();

    scale = pb_depth_scale();
    memcpy(&scale_bits, &scale, 4);

    init_shader();

    alloc_vertices = MmAllocateContiguousMemoryEx(8 * sizeof(Vertex), 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    fill_quad(&alloc_vertices[0], quad_b, B_Z, COLOR_B, scale);
    fill_quad(&alloc_vertices[4], quad_a, A_Z, COLOR_A, scale);

    fb_pitch = pb_back_buffer_pitch();
    ds = (uint8_t *)pb_depth_stencil_buffer();
    ds_pitch = pb_depth_stencil_pitch();
    g_results.fb_pitch = fb_pitch;
    g_results.ds_pitch = ds_pitch;
    g_results.fb_tiled = pb_frame_buffers_tiled();
    g_results.ds_tiled = pb_depth_stencil_tiled();
    g_results.ds_addr = (uint32_t)ds;

    /* Back buffer rotation over a few frames; the last one is read back */
    for (i = 0; i < ROTATE_FRAMES; i++) {
        g_results.rotation[i] = render_frame(false);
    }
    fb = (uint8_t *)g_results.rotation[ROTATE_FRAMES - 1];
    /* let the swap and the rendering fully complete */
    for (i = 0; i < 4; i++) {
        pb_wait_for_vbl();
    }
    while (pb_busy()) {
    }

    log_line("%s", cfg->name);

    /* 1: pitches and tiling (tile pitch rule) */
    {
        bool ok = (fb_pitch == cfg->fb_pitch) && (ds_pitch == cfg->ds_pitch) &&
                  (g_results.fb_tiled == cfg->fb_tiled) && (g_results.ds_tiled == cfg->ds_tiled);
        if (!ok) fails |= 0x001;
        log_line("pitch fb %u ds %u tiled fb %u ds %u %s", fb_pitch, ds_pitch, g_results.fb_tiled, g_results.ds_tiled,
                 ok ? "ok" : "FAIL");
    }

    /* 2: depth buffer address (NULL without one, the app's when supplied), depth screen no-op */
    {
        bool ok = has_depth ? (ds != NULL) : (ds == NULL && crtc_before == crtc_after);
        if (cfg->app_buffers) ok = ok && (ds == app_ds);
        if (!ok) fails |= 0x002;
        log_line("depth buffer %08X%s %s", (uint32_t)ds, has_depth ? "" : " (none, show_depth no-op)",
                 ok ? "ok" : "FAIL");
    }

    /* 3: back buffer rotation: back_buffers+1 distinct buffers, never the same twice in a row,
     * inside the application block when supplied */
    {
        bool ok = true;
        distinct = 0;
        for (i = 0; i < ROTATE_FRAMES; i++) {
            for (j = 0; j < i; j++) {
                if (g_results.rotation[j] == g_results.rotation[i]) break;
            }
            if (j == i) distinct++;
            if (i && (g_results.rotation[i] == g_results.rotation[i - 1])) ok = false;
            if (cfg->app_buffers && ((g_results.rotation[i] - (uint32_t)app_fb) % (cfg->fb_pitch * HEIGHT) ||
                                     g_results.rotation[i] >= (uint32_t)app_fb + 3 * cfg->fb_pitch * HEIGHT ||
                                     g_results.rotation[i] < (uint32_t)app_fb)) {
                ok = false;
            }
        }
        ok = ok && (distinct == cfg->back_buffers + 1);
        if (!ok) fails |= 0x004;
        log_line("rotation %d buffers %08X %08X %s", distinct, g_results.rotation[0], g_results.rotation[1],
                 ok ? "ok" : "FAIL");
    }

    /* 4: invalid application buffers rejected */
    if (cfg->app_buffers) {
        bool ok = (g_results.reject_count == -12) && (g_results.reject_align == -13);
        if (!ok) fails |= 0x008;
        log_line("reject count %d misaligned %d %s", g_results.reject_count, g_results.reject_align,
                 ok ? "ok" : "FAIL");
    }

    /* 5..8 colors, 9..12 depth */
    {
        uint32_t clear_depth = (cfg->depth_fmt == Z16) ? 0xFFFF : 0xFFFFFF;
        uint32_t overlap = has_depth ? COLOR_B : COLOR_A; /* depth test: near B; none: A drawn last */
        uint32_t color_exp[4], depth_exp[4];

        color_exp[0] = pack_color(cfg->color_fmt, COLOR_CLEAR);
        color_exp[1] = pack_color(cfg->color_fmt, overlap);
        color_exp[2] = pack_color(cfg->color_fmt, COLOR_A);
        color_exp[3] = pack_color(cfg->color_fmt, COLOR_B);
        depth_exp[0] = clear_depth;
        depth_exp[1] = (uint32_t)(B_Z * scale);
        depth_exp[2] = (uint32_t)(A_Z * scale);
        depth_exp[3] = (uint32_t)(B_Z * scale);

        for (i = 0; i < 4; i++) {
            bool ok;
            g_results.color[i] = read_color(cfg, fb, fb_pitch, probes[i][0], probes[i][1]);
            g_results.color_expect[i] = color_exp[i];
            ok = color_matches(cfg->color_fmt, g_results.color[i], color_exp[i]);
            if (!ok) fails |= 0x010u << i;
            log_line("color %-7s %06X exp %06X %s", probe_names[i], g_results.color[i], color_exp[i],
                     ok ? "ok" : "FAIL");
        }
        if (has_depth) {
            for (i = 0; i < 4; i++) {
                int32_t diff;
                bool ok;
                g_results.depth[i] = read_depth(cfg, ds, ds_pitch, probes[i][0], probes[i][1]);
                g_results.depth_expect[i] = depth_exp[i];
                diff = (int32_t)g_results.depth[i] - (int32_t)depth_exp[i];
                ok = (i == 0) ? (g_results.depth[i] == depth_exp[i]) : ((diff >= -2) && (diff <= 2));
                if (!ok) fails |= 0x100u << i;
                log_line("depth %-7s %06X exp %06X %s", probe_names[i], g_results.depth[i], depth_exp[i],
                         ok ? "ok" : "FAIL");
            }
        }
    }

    log_line("RESULT: %s (fails 0x%03X)", fails ? "FAIL" : "PASS", fails);
    g_results.fails = fails;
    g_results.pass = fails ? 0 : 1;
    g_results.state = 1;

    /* Hold the frame with the report on screen */
#ifdef CONFIG
    for (frame = 0;; frame++) {
#else
    for (frame = 0; frame < HOLD_FRAMES; frame++) {
#endif
        render_frame(true);
        g_results.frame = frame;
    }

    MmFreeContiguousMemory(alloc_vertices);
    pb_kill();
    /* application buffers are ours: pb_kill left them alone */
    if (app_fb) MmFreeContiguousMemory(app_fb);
    if (app_ds) MmFreeContiguousMemory(app_ds);
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

    p = pb_begin();
#include "ps.inl"
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

static void draw_arrays (unsigned int mode, int start, int count)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, mode);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 MASK(NV097_DRAW_ARRAYS_COUNT, (count - 1)) | MASK(NV097_DRAW_ARRAYS_START_INDEX, start));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
}
