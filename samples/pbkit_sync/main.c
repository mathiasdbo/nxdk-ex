/*
 * pbkit sync test (D3D8 gap, batch 3): cycles through presentation interval
 * configurations with pb_kill/pb_init and measures, over 10 s windows,
 *   - VBlank callbacks per second (~60 NTSC / 50 PAL) against the VBlank counter,
 *   - swap callbacks against the frames pb_finished queued (consecutive frame numbers),
 *   - flips per VBlank: ONE = 1, TWO = 0.5, IMMEDIATE > 1 with a light scene,
 *   - flips outside the VBlank (immediate flips): none for ONE and TWO, all of
 *     them for IMMEDIATE, most of them for ONE_OR_IMMEDIATE with frames that
 *     take ~1.4 refresh periods (and none for ONE with the same slow frames),
 *   - the display field status over consecutive VBlanks,
 * and once per configuration the fences: pending right after heavy work,
 * pb_block_on_fence returns and the fence has passed, a fence behind blocks
 * left by pb_end_deferred (block must kick), the rendering is in memory once
 * the fence has passed (CPU readback), 300 fences across the 32-bit wrap,
 * fences from before the wrap and never inserted ones are not pending.
 * Results are shown with pb_print, sent to DbgPrint and kept in a magic
 * tagged block (g_results, "PBSYNCRS"), with one summary row per configuration.
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

#define WINDOW_MS      10000 /* measurement window */
#define WINDOWS_CYCLE  2     /* windows per configuration when cycling */
#define FIELD_SAMPLES  16
#define WRAP_FENCES    300
#define IMM_CYCLES     20000 /* a swap callback later than this after the VBlank callback is an immediate flip */

typedef struct {
    const char *name;
    int interval;
    bool slow;          /* frames take ~1.4 refresh periods */
    uint32_t fpv_min;   /* flips per VBlank x1000, accepted range */
    uint32_t fpv_max;
    uint32_t imm_min;   /* immediate flips per 1000 flips, accepted range */
    uint32_t imm_max;
} config_t;

static const config_t configs[] = {
    {"0 ONE light", PB_PRESENT_INTERVAL_ONE, false, 950, 1010, 0, 10},
    {"1 TWO light", PB_PRESENT_INTERVAL_TWO, false, 470, 510, 0, 10},
    {"2 IMMEDIATE light", PB_PRESENT_INTERVAL_IMMEDIATE, false, 1200, 1000000, 950, 1000},
    {"3 ONE slow frames", PB_PRESENT_INTERVAL_ONE, true, 550, 850, 0, 10},
    {"4 ONE_OR_IMMEDIATE slow", PB_PRESENT_INTERVAL_ONE_OR_IMMEDIATE, true, 550, 850, 800, 1000},
};
#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))

#define COLOR_CLEAR 0x202040
#define COLOR_BAR   0xFFC000
#define COLOR_FENCE 0x00FF00
#define COLOR_SENTINEL 0x123456

typedef struct {
    float pos[4];   /* x, y, z, w */
    float color[3]; /* r, g, b */
} __attribute__((packed)) Vertex;

typedef struct {
    uint32_t done;              /* 1 = this configuration has completed windows */
    uint32_t pass, fails;
    uint32_t vbl_per_s_x100;
    uint32_t flips_per_vbl_x1000;
    uint32_t imm_per_1000;
    uint32_t fps_x100;
    uint32_t fence_fails;
} summary_t;

/* Results, findable in memory by its magic tag */
typedef struct {
    char magic[8];
    uint32_t config;
    uint32_t state;   /* 0 = init, 1 = fence tests, 2 = field status, 3 = measuring, 4 = results valid */
    uint32_t pass;    /* 1 = all checks of the last window passed */
    uint32_t fails;   /* bit mask of failed checks (see check_window) */
    uint32_t windows; /* completed measurement windows */
    uint32_t refresh;
    /* last window */
    uint32_t window_ms;
    uint32_t vbl_cb, vbl_counter_delta, swap_cb, frames, imm_flips, swap_gaps;
    uint32_t vbl_per_s_x100, flips_per_vbl_x1000, imm_per_1000, fps_x100;
    /* field status at consecutive VBlanks, and what the kernel reports (probe) */
    uint32_t field_vbl[FIELD_SAMPLES];
    int32_t field[FIELD_SAMPLES];
    uint32_t av_mode;                   /* AvSendTVEncoderOption AV_OPTION_QUERY_MODE */
    uint32_t av_guess[FIELD_SAMPLES];   /* AV_OPTION_GUESS_FIELD */
    /* fences */
    uint32_t fence_fails;
    uint32_t fence_first;            /* first fence value after pb_init */
    uint32_t fence_pending_seen;     /* pending right after insert behind heavy work */
    uint32_t fence_block_ms;         /* pb_block_on_fence time for the heavy work */
    uint32_t fence_deferred_ok;      /* block on a fence behind deferred blocks returned */
    uint32_t fence_pixel;            /* readback after the fence passed */
    uint32_t fence_wrap_from, fence_wrap_to, fence_wrapped;
    uint32_t fence_hw;               /* value the GPU wrote last */
    uint32_t total_frames, total_swaps; /* since the callbacks were set, after draining the queue */
    summary_t summary[NUM_CONFIGS];
} results_t;

static volatile results_t g_results __attribute__((aligned(16))) = {"PBSYNCRS"};

/* Callback counters (written in pbkit's DPC: integer only, no FPU) */
static volatile uint32_t cb_vbl, cb_swap, cb_imm, cb_gaps, cb_last_frame, cb_vbl_tsc;

static Vertex *alloc_vertices;
static int status_lines;
static char status[20][64];

static void init_shader (void);
static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data);

static inline uint32_t rdtsc_lo (void)
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return lo;
}

static void vblank_callback (DWORD vbl_count)
{
    (void)vbl_count;
    cb_vbl++;
    cb_vbl_tsc = rdtsc_lo();
}

static void swap_callback (DWORD frame, DWORD vbl_count)
{
    (void)vbl_count;
    cb_swap++;
    if (cb_last_frame && (frame != cb_last_frame + 1)) cb_gaps++;
    cb_last_frame = frame;
    if ((uint32_t)(rdtsc_lo() - cb_vbl_tsc) > IMM_CYCLES) cb_imm++;
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
    DbgPrint("pbkit_sync: %s\n", status[status_lines]);
    status_lines++;
}

static void set_quad (Vertex *v, float x0, float y0, float x1, float y1, uint32_t color)
{
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}; /* clockwise */
    int i;
    for (i = 0; i < 4; i++) {
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[2] = 0.0f;
        v[i].pos[3] = 1.0f;
        v[i].color[0] = ((color >> 16) & 0xFF) / 255.0f;
        v[i].color[1] = ((color >> 8) & 0xFF) / 255.0f;
        v[i].color[2] = (color & 0xFF) / 255.0f;
    }
}

/* Quad from vertices start..start+3; deferred: pb_end_deferred instead of pb_end */
static void draw_quad (int start, bool deferred)
{
    uint32_t *p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 MASK(NV097_DRAW_ARRAYS_COUNT, 3) | MASK(NV097_DRAW_ARRAYS_START_INDEX, start));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    if (deferred) {
        pb_end_deferred(p);
    } else {
        pb_end(p);
    }
}

static void setup_draw_state (void)
{
    uint32_t *p;
    int i;

    p = pb_begin();
    pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
    for (i = 0; i < 16; i++) {
        *(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
    }
    p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
    pb_end(p);
    set_attrib_pointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 4, sizeof(Vertex), &alloc_vertices[0].pos[0]);
    set_attrib_pointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F, 3, sizeof(Vertex), &alloc_vertices[0].color[0]);
}

static volatile uint32_t frames_finished;

/* One frame: light scene (clear, moving bar, report text); slow: the CPU holds the
 * frame back until slow_ms after its start, so it misses the next VBlank */
static void render_frame (uint32_t frame, uint32_t slow_ms)
{
    uint32_t start = KeTickCount;
    float x = (float)((frame * 8) % (WIDTH - 40));
    int i;

    pb_wait_for_flip();
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT);
    pb_fill(0, 0, WIDTH, HEIGHT, COLOR_CLEAR);
    pb_erase_text_screen();
    set_quad(&alloc_vertices[0], x, 0.0f, x + 40.0f, (float)HEIGHT, COLOR_BAR);
    setup_draw_state();
    draw_quad(0, false);
    for (i = 0; i < status_lines; i++) {
        pb_print("%s\n", status[i]);
    }
    pb_draw_text_screen();
    if (slow_ms) {
        while ((uint32_t)(KeTickCount - start) < slow_ms) {
        }
    }
    while (pb_finished()) {
    }
    frames_finished++;
}

/* Fence tests, run once per configuration; returns a fail mask */
static uint32_t test_fences (void)
{
    uint32_t fails = 0, f, prev, first, t0, i;
    uint32_t pre_wrap = 0;
    uint8_t *fb;
    volatile uint32_t *pixel;

    g_results.state = 1;

    /* 1: heavy work, fence, pending right after, block returns, then passed */
    pb_reset();
    pb_target_back_buffer();
    for (i = 0; i < 200; i++) {
        pb_fill(0, 0, WIDTH, HEIGHT, (i & 1) ? 0x000000 : 0xFFFFFF);
    }
    f = pb_insert_fence();
    first = f;
    g_results.fence_first = f;
    g_results.fence_pending_seen = pb_is_fence_pending(f);
    t0 = KeTickCount;
    pb_block_on_fence(f);
    g_results.fence_block_ms = KeTickCount - t0;
    if (pb_is_fence_pending(f)) fails |= 0x01;
    if (first != 0xFFFFFF01u) fails |= 0x01; /* pb_init starts below the wrap */

    /* 2: fence behind blocks left by pb_end_deferred: pb_block_on_fence must kick */
    pb_reset();
    setup_draw_state();
    set_quad(&alloc_vertices[0], 0.0f, 0.0f, 16.0f, 16.0f, COLOR_BAR);
    for (i = 0; i < 8; i++) {
        draw_quad(0, true);
    }
    f = pb_insert_fence();
    t0 = KeTickCount;
    while (pb_is_fence_pending(f) && (uint32_t)(KeTickCount - t0) < 2000) {
    }
    g_results.fence_deferred_ok = !pb_is_fence_pending(f);
    if (!g_results.fence_deferred_ok) fails |= 0x02;

    /* 3: the rendering before the fence is in memory once it has passed */
    pb_reset();
    pb_target_back_buffer();
    fb = (uint8_t *)pb_back_buffer();
    pixel = (volatile uint32_t *)(fb + 240 * pb_back_buffer_pitch() + 320 * 4);
    *pixel = COLOR_SENTINEL;
    __asm__ __volatile__("sfence");
    pb_fill(0, 0, WIDTH, HEIGHT, COLOR_CLEAR);
    setup_draw_state();
    set_quad(&alloc_vertices[0], 300.0f, 220.0f, 340.0f, 260.0f, COLOR_FENCE);
    draw_quad(0, true);
    f = pb_insert_fence();
    pb_block_on_fence(f);
    g_results.fence_pixel = *pixel & 0xFFFFFF;
    if (g_results.fence_pixel != COLOR_FENCE) fails |= 0x04;

    /* 4: 300 fences across the 32-bit wrap, consecutive values */
    pb_reset();
    setup_draw_state();
    set_quad(&alloc_vertices[0], 0.0f, 0.0f, 8.0f, 8.0f, COLOR_BAR);
    prev = f;
    g_results.fence_wrap_from = prev + 1;
    g_results.fence_wrapped = 0;
    for (i = 0; i < WRAP_FENCES; i++) {
        draw_quad(0, true);
        f = pb_insert_fence();
        if (f != prev + 1) fails |= 0x08;
        if (f < prev) {
            g_results.fence_wrapped = 1;
        }
        if (f == 0xFFFFFFF0u) pre_wrap = f;
        prev = f;
    }
    g_results.fence_wrap_to = f;
    pb_block_on_fence(f);
    if (!g_results.fence_wrapped || !pre_wrap) fails |= 0x08;
    /* 5: fences from before the wrap and the first one have passed (a plain compare
     * would call them pending), never inserted ones are not pending */
    if (pb_is_fence_pending(pre_wrap) || pb_is_fence_pending(first) || pb_is_fence_pending(f)) fails |= 0x10;
    if (pb_is_fence_pending(f + 1) || pb_is_fence_pending(f + 1000)) fails |= 0x20;

    /* 6: pending across the wrap behind heavy work (recorded), then passed */
    for (i = 0; i < 200; i++) {
        pb_fill(0, 0, WIDTH, HEIGHT, (i & 1) ? 0x000000 : 0xFFFFFF);
    }
    f = pb_insert_fence();
    if (pb_is_fence_pending(f)) g_results.fence_pending_seen |= 2;
    pb_block_on_fence(f);
    if (pb_is_fence_pending(f)) fails |= 0x10;

    g_results.fence_hw = f; /* last fence, passed */
    g_results.fence_fails = fails;
    return fails;
}

/* Field status at FIELD_SAMPLES consecutive VBlanks, with the kernel's report (probe) */
static uint32_t test_field_status (void)
{
    ULONG mode = 0, guess;
    DWORD vbl;
    int field, i;
    bool all_unknown = true, alternating = true;

    g_results.state = 2;
    AvSendTVEncoderOption((PVOID)VIDEO_BASE, 13 /* AV_OPTION_QUERY_MODE */, 0, &mode);
    g_results.av_mode = mode;
    for (i = 0; i < FIELD_SAMPLES; i++) {
        pb_wait_for_vbl();
        pb_get_display_field_status(&vbl, &field);
        guess = 0;
        AvSendTVEncoderOption((PVOID)VIDEO_BASE, 15 /* AV_OPTION_GUESS_FIELD */, 0, &guess);
        g_results.field_vbl[i] = vbl;
        g_results.field[i] = field;
        g_results.av_guess[i] = guess;
    }
    for (i = 0; i < FIELD_SAMPLES; i++) {
        if (g_results.field[i] != -1) all_unknown = false;
        if (g_results.field[i] < -1 || g_results.field[i] > 1) alternating = false;
        if (i && g_results.field[i] != -1 && g_results.field[i - 1] != -1 &&
            (uint32_t)(g_results.field[i] ^ g_results.field[i - 1]) !=
                ((g_results.field_vbl[i] - g_results.field_vbl[i - 1]) & 1)) {
            alternating = false;
        }
    }
    {
        /* AV mode bits 31:30: 0/1 SDTV (interlaced), 2 HDTV (interlaced only for TV index 3, 1080i), 3 VGA */
        uint32_t out = mode & 0xC0000000;
        bool interlaced = mode && (out == 0 || out == 0x40000000 || (out == 0x80000000 && (mode & 0xFF) == 3));
        if (interlaced) {
            return (!all_unknown && alternating) ? 0 : 0x020;
        }
        return all_unknown ? 0 : 0x020;
    }
}

/* Checks of one measurement window; returns a fail mask */
static uint32_t check_window (const config_t *cfg)
{
    uint32_t fails = 0;
    uint32_t refresh = g_results.refresh;
    uint32_t d;

    /* 0x001: VBlank callbacks per second within 2% of the refresh rate */
    if (g_results.vbl_per_s_x100 < refresh * 98 || g_results.vbl_per_s_x100 > refresh * 102) fails |= 0x001;
    /* 0x002: one VBlank callback per VBlank counter increment */
    d = g_results.vbl_cb > g_results.vbl_counter_delta ? g_results.vbl_cb - g_results.vbl_counter_delta
                                                       : g_results.vbl_counter_delta - g_results.vbl_cb;
    if (d > 1) fails |= 0x002;
    /* 0x004: one swap callback per frame (exact once the queue is drained), consecutive frame numbers */
    if (g_results.swap_gaps || g_results.total_frames != g_results.total_swaps) fails |= 0x004;
    /* 0x008: flips per VBlank for the interval */
    if (g_results.flips_per_vbl_x1000 < cfg->fpv_min || g_results.flips_per_vbl_x1000 > cfg->fpv_max) fails |= 0x008;
    /* 0x010: flips outside the VBlank */
    if (g_results.imm_per_1000 < cfg->imm_min || g_results.imm_per_1000 > cfg->imm_max) fails |= 0x010;
    return fails;
}

static void report (int index, uint32_t fails)
{
    const config_t *cfg = &configs[index];
    int i;

    status_lines = 0;
    log_line("%s (%u Hz) window %u", cfg->name, g_results.refresh, g_results.windows);
    log_line("vbl cb %u/%u in %u ms: %u.%02u/s %s", g_results.vbl_cb, g_results.vbl_counter_delta,
             g_results.window_ms, g_results.vbl_per_s_x100 / 100, g_results.vbl_per_s_x100 % 100,
             (fails & 0x003) ? "FAIL" : "ok");
    log_line("swap cb %u frames %u gaps %u total %u/%u %s", g_results.swap_cb, g_results.frames, g_results.swap_gaps,
             g_results.total_swaps, g_results.total_frames,
             (fails & 0x004) ? "FAIL" : "ok");
    log_line("flips/vbl %u.%03u (%u..%u) %s", g_results.flips_per_vbl_x1000 / 1000,
             g_results.flips_per_vbl_x1000 % 1000, cfg->fpv_min, cfg->fpv_max, (fails & 0x008) ? "FAIL" : "ok");
    log_line("immediate %u/1000 (%u..%u) fps %u.%02u %s", g_results.imm_per_1000, cfg->imm_min, cfg->imm_max,
             g_results.fps_x100 / 100, g_results.fps_x100 % 100, (fails & 0x010) ? "FAIL" : "ok");
    {
        char seq[FIELD_SAMPLES + 1];
        for (i = 0; i < FIELD_SAMPLES; i++) {
            seq[i] = (g_results.field[i] < 0) ? '-' : (char)('0' + g_results.field[i]);
        }
        seq[FIELD_SAMPLES] = 0;
        log_line("field %s av mode %08X guess %u%u%u%u %s", seq, g_results.av_mode, g_results.av_guess[0],
                 g_results.av_guess[1], g_results.av_guess[2], g_results.av_guess[3],
                 (fails & 0x020) ? "FAIL" : "ok");
    }
    log_line("fence first %08X block %u ms pending %u %s", g_results.fence_first, g_results.fence_block_ms,
             g_results.fence_pending_seen, (g_results.fence_fails & 0x01) ? "FAIL" : "ok");
    log_line("fence deferred+kick %s, pixel %06X %s", g_results.fence_deferred_ok ? "ok" : "FAIL",
             g_results.fence_pixel, (g_results.fence_fails & 0x04) ? "FAIL" : "ok");
    log_line("fence wrap %08X..%08X %s, old/never %s", g_results.fence_wrap_from, g_results.fence_wrap_to,
             (g_results.fence_fails & 0x08) ? "FAIL" : "ok", (g_results.fence_fails & 0x30) ? "FAIL" : "ok");
    log_line("results %08X", (uint32_t)&g_results);
    log_line("RESULT: %s (fails 0x%03X fence 0x%02X)", (fails || g_results.fence_fails) ? "FAIL" : "PASS", fails,
             g_results.fence_fails);
}

static void run_config (int index)
{
    const config_t *cfg = &configs[index];
    uint32_t slow_ms, field_fails, frame, w, t0, fails, i;
    int status_code;

    status_lines = 0;
    memset((void *)&g_results.config, 0, offsetof(results_t, summary) - offsetof(results_t, config));
    g_results.config = index;

    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT)) {
        debugPrint("XVideoSetMode failed\n");
        Sleep(2000);
        return;
    }
    g_results.refresh = XVideoGetMode().refresh;
    slow_ms = 1400 / g_results.refresh; /* ~1.4 refresh periods */

    pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    pb_set_depth_format(NV097_SET_SURFACE_FORMAT_ZETA_Z24S8);
    pb_back_buffers(2);
    pb_set_presentation_interval(PB_PRESENT_INTERVAL_ONE); /* fence and field tests at the default */
    if ((status_code = pb_init())) {
        debugPrint("pb_init Error %d\n", status_code);
        Sleep(2000);
        return;
    }
    pb_show_front_screen();
    init_shader();
    alloc_vertices = MmAllocateContiguousMemoryEx(4 * sizeof(Vertex), 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);

    g_results.fence_fails = test_fences();
    field_fails = test_field_status();

    cb_vbl = cb_swap = cb_imm = cb_gaps = cb_last_frame = 0;
    frames_finished = 0;
    pb_set_vblank_callback(vblank_callback);
    pb_set_swap_callback(swap_callback);
    pb_set_presentation_interval(cfg->interval);
    g_results.state = 3;
    log_line("%s: measuring %u s...", cfg->name, WINDOW_MS / 1000);

    frame = 0;
#ifdef CONFIG
    for (w = 0;; w++) {
#else
    for (w = 0; w < WINDOWS_CYCLE; w++) {
#endif
        uint32_t vbl0, swap0, imm0, frames0, counter0, ms;
        /* settle one frame queue, then snapshot */
        render_frame(frame++, cfg->slow ? slow_ms : 0);
        t0 = KeTickCount;
        vbl0 = cb_vbl;
        swap0 = cb_swap;
        imm0 = cb_imm;
        frames0 = frames_finished;
        counter0 = pb_get_vbl_counter();
        cb_gaps = 0;
        while ((uint32_t)(KeTickCount - t0) < WINDOW_MS) {
            render_frame(frame++, cfg->slow ? slow_ms : 0);
        }
        ms = KeTickCount - t0;
        g_results.window_ms = ms;
        g_results.vbl_cb = cb_vbl - vbl0;
        g_results.vbl_counter_delta = pb_get_vbl_counter() - counter0;
        g_results.swap_cb = cb_swap - swap0;
        g_results.imm_flips = cb_imm - imm0;
        g_results.frames = frames_finished - frames0;
        g_results.swap_gaps = cb_gaps;
        /* drain: every frame pb_finished queued must reach the screen, one swap callback each */
        while (pb_busy()) {
        }
        for (i = 0; i < 8; i++) {
            pb_wait_for_vbl();
        }
        g_results.total_frames = frames_finished;
        g_results.total_swaps = cb_swap;
        g_results.vbl_per_s_x100 = (uint32_t)((uint64_t)g_results.vbl_cb * 100000 / ms);
        g_results.fps_x100 = (uint32_t)((uint64_t)g_results.frames * 100000 / ms);
        g_results.flips_per_vbl_x1000 =
            g_results.vbl_cb ? (uint32_t)((uint64_t)g_results.swap_cb * 1000 / g_results.vbl_cb) : 0;
        g_results.imm_per_1000 = g_results.swap_cb ? (uint32_t)((uint64_t)g_results.imm_flips * 1000 / g_results.swap_cb) : 0;
        g_results.windows = w + 1;

        fails = check_window(cfg) | field_fails;
        g_results.fails = fails;
        g_results.pass = (fails || g_results.fence_fails) ? 0 : 1;
        g_results.state = 4;
        g_results.summary[index].done = 1;
        g_results.summary[index].pass = g_results.pass;
        g_results.summary[index].fails = fails;
        g_results.summary[index].vbl_per_s_x100 = g_results.vbl_per_s_x100;
        g_results.summary[index].flips_per_vbl_x1000 = g_results.flips_per_vbl_x1000;
        g_results.summary[index].imm_per_1000 = g_results.imm_per_1000;
        g_results.summary[index].fps_x100 = g_results.fps_x100;
        g_results.summary[index].fence_fails = g_results.fence_fails;
        report(index, fails);
    }

    pb_set_vblank_callback(NULL);
    pb_set_swap_callback(NULL);
    pb_set_presentation_interval(PB_PRESENT_INTERVAL_ONE);
    MmFreeContiguousMemory(alloc_vertices);
    pb_kill();
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
