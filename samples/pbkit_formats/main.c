/*
 * pbkit surface format test: cycles through depth/color buffer configurations
 * (Z24S8 / Z16, fixed / floating point depth, Z / W-buffering, 32 / 16 bits
 * color), draws two overlapping triangles per configuration and reads the color
 * and depth buffers back with the CPU to check the clear value, the depth test
 * winner and the 16 bits color packing. Results are shown with pb_print,
 * sent to DbgPrint and kept in a magic tagged block (g_results) that a debugger
 * can find in memory.
 *
 * Build with make CONFIG=n to run a single configuration without pb_kill.
 *
 * On a console the same lines go to E:\pbkit_formats.txt during the first
 * pass (a START line before each pb_init, so a freeze names its config); after
 * LOG_CYCLES passes the cycling build reboots to the dashboard so the file can
 * be fetched over FTP.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
#include <nxdk/mount.h>
#include <pbkit/pbkit.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "watchdog.h"

#define MASK(mask, val) (((val) << (ffs(mask) - 1)) & (mask))

#define WIDTH  640
#define HEIGHT 480

#define HOLD_FRAMES (15 * 60) /* ~15 s per configuration before moving on */

typedef struct {
    const char *name;
    unsigned int color_fmt;
    int bpp;
    unsigned int depth_fmt;
    bool depth_float;
    bool w_buffer;
    const char *note; /* shown on screen, e.g. known emulator limitation */
} config_t;

static const config_t configs[] = {
    {"0 Z24S8 A8R8G8B8 Z-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 32, NV097_SET_SURFACE_FORMAT_ZETA_Z24S8,
     false, false, NULL},
    {"1 Z16 R5G6B5 Z-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, 16, NV097_SET_SURFACE_FORMAT_ZETA_Z16, false,
     false, NULL},
    {"2 Z16 float X1R5G5B5 Z-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5, 15,
     NV097_SET_SURFACE_FORMAT_ZETA_Z16, true, false, NULL},
    {"3 Z24S8 float A8R8G8B8 Z-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 32,
     NV097_SET_SURFACE_FORMAT_ZETA_Z24S8, true, false, NULL},
    {"4 Z24S8 A8R8G8B8 W-buffer (pbkit default)", NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 32,
     NV097_SET_SURFACE_FORMAT_ZETA_Z24S8, false, true, NULL},
    /* xemu emulates F24S8 with a fixed point Z24S8 texture normalized by 1E30:
     * w values of a few hundred units underflow to 0 and w ~1E29 overflow its
     * w*w slope term, so this one can only pass on a console. */
    {"5 Z24S8 float A8R8G8B8 W-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, 32,
     NV097_SET_SURFACE_FORMAT_ZETA_Z24S8, true, true, "(console only: xemu stores F24S8 as fixed Z24S8)"},
    {"6 Z16 float R5G6B5 W-buffer", NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5, 16, NV097_SET_SURFACE_FORMAT_ZETA_Z16,
     true, true, NULL},
};
#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))

/* Two overlapping triangles, screen space, clockwise (= front face for pbkit).
 * A (red):  z = 0.9, w = 0.3 of the depth range
 * B (blue): z = 0.1, w = 0.7 of the depth range
 * so the Z-buffer winner of the overlap is B and the W-buffer winner is A. */
#define A_Z 0.9f
#define A_W 0.3f
#define B_Z 0.1f
#define B_W 0.7f
#define COLOR_A     0xFF0000
#define COLOR_B     0x0000FF
#define COLOR_CLEAR 0x404040

static const float tri_a[3][2] = {{100.0f, 100.0f}, {420.0f, 100.0f}, {260.0f, 400.0f}};
static const float tri_b[3][2] = {{220.0f, 100.0f}, {540.0f, 100.0f}, {380.0f, 400.0f}};

/* Probe pixels: outside both triangles, inside both, inside A only, inside B only */
#define PX_CLEAR_X 40
#define PX_CLEAR_Y 40
#define PX_BOTH_X  320
#define PX_BOTH_Y  200
#define PX_A_X     160
#define PX_A_Y     150
#define PX_B_X     480
#define PX_B_Y     150

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
    uint32_t fails; /* bit mask of failed checks (see check_t order) */
    uint32_t color[4];        /* raw color read back: clear, both, A only, B only */
    uint32_t color_expect[4]; /* expected (same order) */
    uint32_t depth[4];        /* raw depth read back (stencil stripped for Z24S8) */
    uint32_t depth_expect[4]; /* expected depth (fixed point formats only) */
    uint32_t depth_scale_bits;
} results_t;

static volatile results_t g_results __attribute__((aligned(16))) = {"PBFMTRES"};

static Vertex *alloc_vertices;
static int status_lines;
static char status[12][64];

#define LOGFILE    "E:\\pbkit_formats.txt"
#define LOG_CYCLES 2 /* cycling build: passes over configs[] before XReboot */
static int g_cycle;        /* passes over configs[] completed */
static bool g_log_started; /* the first write truncates the file */

static void mount_e (void)
{
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
}

static char g_last_step[96]; /* the watchdog names the last line logged */

/* Appends one line to E:\pbkit_formats.txt during the first pass (on xemu
 * without an E: partition CreateFile just fails) and bumps the watchdog */
static void log_file (const char *line)
{
    HANDLE h;
    DWORD written;

    strncpy(g_last_step, line, sizeof(g_last_step) - 1);
    watchdog_kick(g_last_step);
#ifdef NO_FILE_LOG
    return; /* experiment: no disk writes */
#endif
    if (g_cycle > 0) {
        return;
    }
    mount_e();
    /* write-through, so a reset or a power-off keeps every line already logged */
    h = CreateFileA(LOGFILE, GENERIC_WRITE, 0, NULL, g_log_started ? OPEN_ALWAYS : CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    g_log_started = true;
    if (h != INVALID_HANDLE_VALUE) {
        SetFilePointer(h, 0, NULL, FILE_END);
        WriteFile(h, line, (DWORD)strlen(line), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
}

static void init_shader (void);
static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data);
static void draw_arrays (unsigned int mode, int start, int count);

static void log_line (const char *fmt, ...)
{
    va_list ap;
    if (status_lines >= 12) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(status[status_lines], sizeof(status[0]), fmt, ap);
    va_end(ap);
    DbgPrint("pbkit_formats: %s\n", status[status_lines]);
    log_file(status[status_lines]);
    status_lines++;
}

/* Packs an X8R8G8B8 color the way the GPU stores it in the given color format */
static uint32_t pack_color (unsigned int fmt, uint32_t argb)
{
    switch (fmt) {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
            return ((argb >> 9) & 0x7C00) | ((argb >> 6) & 0x03E0) | ((argb >> 3) & 0x001F);
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            return ((argb >> 8) & 0xF800) | ((argb >> 5) & 0x07E0) | ((argb >> 3) & 0x001F);
        default:
            return argb & 0xFFFFFF;
    }
}

/* Per channel tolerance of 1 (dithering, rounding) */
static bool color_matches (unsigned int fmt, uint32_t got, uint32_t expect)
{
    int shifts[3], widths[3], i;
    switch (fmt) {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
            shifts[0] = 10; shifts[1] = 5; shifts[2] = 0;
            widths[0] = widths[1] = widths[2] = 5;
            break;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            shifts[0] = 11; shifts[1] = 5; shifts[2] = 0;
            widths[0] = 5; widths[1] = 6; widths[2] = 5;
            break;
        default:
            shifts[0] = 16; shifts[1] = 8; shifts[2] = 0;
            widths[0] = widths[1] = widths[2] = 8;
            break;
    }
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
    if (cfg->bpp == 15) { /* X1R5G5B5: ignore the unused top bit */
        return *(const uint16_t *)(fb + y * pitch + x * 2) & 0x7FFF;
    }
    return *(const uint16_t *)(fb + y * pitch + x * 2);
}

static uint32_t read_depth (const config_t *cfg, const uint8_t *ds, uint32_t pitch, int x, int y)
{
    if (cfg->depth_fmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16) {
        return *(const uint16_t *)(ds + y * pitch + x * 2);
    }
    return *(const uint32_t *)(ds + y * pitch + x * 4) >> 8;
}

static void fill_triangle (Vertex *v, const float xy[3][2], float z, float w, uint32_t color, float scale)
{
    int i;
    for (i = 0; i < 3; i++) {
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[2] = z * scale;
        v[i].pos[3] = w * scale;
        v[i].color[0] = ((color >> 16) & 0xFF) / 255.0f;
        v[i].color[1] = ((color >> 8) & 0xFF) / 255.0f;
        v[i].color[2] = (color & 0xFF) / 255.0f;
    }
}

static void draw_scene (const config_t *cfg)
{
    uint32_t *p;
    int i;

    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT);
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

    /* The expected winner is drawn first, the loser last: only the depth test
     * can give the right result. Vertices 0-2: winner, 3-5: loser. */
    draw_arrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0, 6);
}

static void run_config (int index)
{
    const config_t *cfg = &configs[index];
    uint8_t *fb, *ds;
    uint32_t fb_pitch, ds_pitch, scale_bits;
    float scale;
    int i, status_code, frame;
    bool pass = true;
    uint32_t fails = 0;
    bool w = cfg->w_buffer;

    status_lines = 0;
    {
        char line[96];
        snprintf(line, sizeof(line), "START config %d (%s) cycle %d", index, cfg->name, g_cycle);
        log_file(line);
    }
    g_results.config = index;
    g_results.frame = 0;
    g_results.state = 0;
    g_results.pass = 0;
    g_results.fails = 0;

    if (!XVideoSetMode(WIDTH, HEIGHT, cfg->bpp, REFRESH_DEFAULT)) {
        debugPrint("XVideoSetMode %d bpp failed\n", cfg->bpp);
        log_file("XVideoSetMode failed");
        Sleep(2000);
        return;
    }
    log_file("step: XVideoSetMode ok");

    pb_set_color_format(cfg->color_fmt, false);
    pb_set_depth_format(cfg->depth_fmt);
    pb_set_depth_float(cfg->depth_float);
    pb_set_w_buffer(cfg->w_buffer);

    if ((status_code = pb_init())) {
        debugPrint("pb_init Error %d\n", status_code);
        log_file("pb_init failed");
        Sleep(2000);
        return;
    }
    log_file("step: pb_init ok");
#ifdef GPU_ERROR
    /* experiment: a depth clear four times the buffer's height, past its DMA
     * limit; on a console this raises a zeta limit error for the watchdog */
    log_file("EXP GPU_ERROR: oversized depth clear");
    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT * 4);
    while (pb_busy()) {
    }
    log_file("EXP GPU_ERROR: clear done without an error");
#endif
#ifdef HANG
    /* experiment: a hang at passive level, for the watchdog */
    log_file("EXP HANG: spinning");
    for (;;) {
        __asm__ volatile("");
    }
#endif
    pb_show_front_screen();
    log_file("step: pb_show_front_screen ok");

    scale = pb_depth_scale();
    memcpy(&scale_bits, &scale, 4);
    g_results.depth_scale_bits = scale_bits;

    init_shader();
    log_file("step: init_shader ok");

    alloc_vertices = MmAllocateContiguousMemoryEx(6 * sizeof(Vertex), 0, 0x3ffb000, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (w) {
        fill_triangle(&alloc_vertices[0], tri_a, A_Z, A_W, COLOR_A, scale);
        fill_triangle(&alloc_vertices[3], tri_b, B_Z, B_W, COLOR_B, scale);
    } else {
        fill_triangle(&alloc_vertices[0], tri_b, B_Z, B_W, COLOR_B, scale);
        fill_triangle(&alloc_vertices[3], tri_a, A_Z, A_W, COLOR_A, scale);
    }

    /* Frame 1: render and read back */
    pb_wait_for_vbl();
    pb_reset();
    pb_target_back_buffer();
    fb = (uint8_t *)pb_back_buffer();
    fb_pitch = pb_back_buffer_pitch();
    ds = (uint8_t *)pb_depth_stencil_buffer();
    ds_pitch = pb_depth_stencil_pitch();
    log_file("step: target back buffer ok");

    draw_scene(cfg);
    log_file("step: draw_scene submitted");
    while (pb_busy()) {
    }
    log_file("step: pb_busy idle");
    while (pb_finished()) {
    }
    log_file("step: pb_finished ok");
    /* let the swap and the rendering fully complete */
    for (i = 0; i < 4; i++) {
        pb_wait_for_vbl();
    }
    while (pb_busy()) {
    }
    log_file("step: idle after 4 vbl");

    /* Expectations */
    {
        const int px[4][2] = {{PX_CLEAR_X, PX_CLEAR_Y}, {PX_BOTH_X, PX_BOTH_Y}, {PX_A_X, PX_A_Y}, {PX_B_X, PX_B_Y}};
        float depth_a = w ? A_W : A_Z, depth_b = w ? B_W : B_Z;
        uint32_t color_exp[4], depth_exp[4];
        uint32_t clear_depth = (cfg->depth_fmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16) ? 0xFFFF : 0xFFFFFF;
        bool both_is_a = w; /* overlap winner */
        static const char *names[4] = {"clear", "overlap", "A only", "B only"};

        color_exp[0] = pack_color(cfg->color_fmt, COLOR_CLEAR);
        color_exp[1] = pack_color(cfg->color_fmt, both_is_a ? COLOR_A : COLOR_B);
        color_exp[2] = pack_color(cfg->color_fmt, COLOR_A);
        color_exp[3] = pack_color(cfg->color_fmt, COLOR_B);
        depth_exp[0] = clear_depth;
        depth_exp[1] = (uint32_t)((both_is_a ? depth_a : depth_b) * scale);
        depth_exp[2] = (uint32_t)(depth_a * scale);
        depth_exp[3] = (uint32_t)(depth_b * scale);

        for (i = 0; i < 4; i++) {
            char line[64];
            g_results.color[i] = read_color(cfg, fb, fb_pitch, px[i][0], px[i][1]);
            snprintf(line, sizeof(line), "step: color read %d = %06lX", i, (unsigned long)g_results.color[i]);
            log_file(line);
#ifdef NO_DEPTH_READ
            g_results.depth[i] = depth_exp[i]; /* experiment: no CPU read of the depth tile */
#else
            g_results.depth[i] = read_depth(cfg, ds, ds_pitch, px[i][0], px[i][1]);
#endif
            snprintf(line, sizeof(line), "step: depth read %d = %06lX", i, (unsigned long)g_results.depth[i]);
            log_file(line);
            g_results.color_expect[i] = color_exp[i];
            g_results.depth_expect[i] = depth_exp[i];
        }

        log_line("%s", cfg->name);
        if (cfg->note) {
            log_line("%s", cfg->note);
        }
        log_line("scale 0x%08X pitch fb %u ds %u", scale_bits, fb_pitch, ds_pitch); /* float bits: %g is not supported */

        /* colors: clear value, depth test winner, 16 bits packing */
        for (i = 0; i < 4; i++) {
            bool ok = color_matches(cfg->color_fmt, g_results.color[i], color_exp[i]);
            if (!ok) {
                fails |= 1u << i;
            }
            log_line("color %-7s %06X exp %06X %s", names[i], g_results.color[i], color_exp[i], ok ? "ok" : "FAIL");
        }

        if (!cfg->depth_float) {
            /* fixed point: exact clear value, interpolated z/w within 2 LSB */
            for (i = 0; i < 4; i++) {
                int32_t diff = (int32_t)g_results.depth[i] - (int32_t)depth_exp[i];
                bool ok = (i == 0) ? (g_results.depth[i] == depth_exp[i]) : ((diff >= -2) && (diff <= 2));
                if (!ok) {
                    fails |= 16u << i;
                }
                log_line("depth %-7s %06X exp %06X %s", names[i], g_results.depth[i], depth_exp[i], ok ? "ok" : "FAIL");
            }
        } else {
            /* floating point: the raw encoding is emulator/hardware specific,
             * check the ordering (monotonic encodings): clear > far > near */
            uint32_t near_v = g_results.depth[both_is_a ? 2 : 3];
            uint32_t far_v = g_results.depth[both_is_a ? 3 : 2];
            bool ok0 = g_results.depth[0] > far_v;
            bool ok1 = g_results.depth[1] == near_v;
            bool ok2 = far_v > near_v;
            if (!ok0) fails |= 16u;
            if (!ok1) fails |= 32u;
            if (!ok2) fails |= 64u;
            log_line("depth raw clear %06X both %06X A %06X B %06X", g_results.depth[0], g_results.depth[1],
                     g_results.depth[2], g_results.depth[3]);
            log_line("float order clear>far %s both==near %s far>near %s", ok0 ? "ok" : "FAIL", ok1 ? "ok" : "FAIL",
                     ok2 ? "ok" : "FAIL");
        }
    }

    pass = (fails == 0);
    log_line("RESULT: %s (fails 0x%02X)", pass ? "PASS" : "FAIL", fails);
    g_results.fails = fails;
    g_results.pass = pass ? 1 : 0;
    g_results.state = 1;

    /* Hold the frame with the report on screen */
#if defined(CONFIG) && !defined(CHAIN)
    for (frame = 0;; frame++) {
#else
    for (frame = 0; frame < HOLD_FRAMES; frame++) {
#endif
        pb_wait_for_vbl();
        pb_reset();
        pb_target_back_buffer();
        draw_scene(cfg);
        for (i = 0; i < status_lines; i++) {
            pb_print("%s\n", status[i]);
        }
        pb_draw_text_screen();
        if (frame == 0) {
            log_file("hold frame 0 submitted");
        }
        while (pb_busy()) {
        }
        while (pb_finished()) {
        }
        g_results.frame = frame;
        watchdog_kick(NULL);
        if (frame < 3 || frame == HOLD_FRAMES - 1) {
            char line[48];
            snprintf(line, sizeof(line), "hold frame %d ok", frame);
            log_file(line);
        }
#ifdef NO_FILE_LOG
        if (0) {
#else
        if ((frame % 10) == 0 && g_cycle == 0) {
#endif
            /* E:\pbkit_progress.txt names the last frame that completed, for a hang */
            FILE *pf;
            mount_e();
            pf = fopen("E:\\pbkit_progress.txt", "wb");
            if (pf) {
                fprintf(pf, "config %d frame %d\r\n", index, frame);
                fclose(pf);
            }
        }
    }
    log_file("hold done");

#ifndef CHAIN
    MmFreeContiguousMemory(alloc_vertices);
    pb_kill();
#endif
}

int main (void)
{
#ifndef NO_WATCHDOG
    mount_e();
    watchdog_start();
#endif
    {
        char line[160];
#if defined(CONFIG) && defined(CHAIN)
        g_log_started = (CONFIG != 0); /* cfg1..6 append */
#endif
        watchdog_sector0("at start", line, sizeof(line));
        log_file(line);
    }
#ifdef CONFIG
#ifdef CHAIN
    /* One XBE per configuration: cfg0 (default.xbe) truncates the log, the
     * others append; after the hold the next XBE is launched through the
     * kernel instead of pb_kill/pb_init, the last one reboots. */
    run_config(CONFIG);
    if (CONFIG + 1 < (int)NUM_CONFIGS) {
        char path[64];
        snprintf(path, sizeof(path), "E:\\Games\\pbkit Formats\\cfg%d.xbe", CONFIG + 1);
        {
            char line[96];
            snprintf(line, sizeof(line), "NEXT %s", path);
            log_file(line);
        }
        XLaunchXBE(path);
        log_file("XLaunchXBE returned");
    } else {
        char line[160];
        watchdog_sector0("before reboot", line, sizeof(line));
        log_file(line);
        log_file("DONE: rebooting to the dashboard");
        XReboot();
    }
    while (1) {
    }
#else
    run_config(CONFIG);
#endif
#else
    while (1) {
        unsigned int i;
        for (i = 0; i < NUM_CONFIGS; i++) {
            run_config(i);
        }
        g_cycle++;
        if (g_cycle == LOG_CYCLES) {
            log_file("DONE: rebooting to the dashboard");
            XReboot();
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
