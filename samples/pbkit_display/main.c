/*
 * pbkit display test (D3D8 gap, batch 5): four configurations,
 *   0 mode flags: XVideoSetModeEx with each XVIDEO_FLAG_* combination against
 *     the AV pack and the dashboard settings (XVideoGetVideoFlags): success or
 *     refusal as expected, the AV mode the kernel reports (AV_OPTION_QUERY_MODE)
 *     equal to XVideoGetAvMode, scan type, WSS bit, FPAR mode; then the display
 *     field status over consecutive VBlanks in the widescreen mode,
 *   1 overlay: a 160x120 YUY2 color bar surface scaled 2x by NV_PVIDEO into a
 *     magenta color keyed rectangle of a 3D scene (a static green quad and a
 *     moving yellow bar in the rectangle hide it); invalid descriptions are
 *     rejected, the registers read back as programmed, updates are applied
 *     one VBlank later, double buffered surfaces are swapped 20 times, the
 *     frame buffer under the overlay still holds the key color (the overlay is
 *     not in it: check it on screen),
 *   2 TV filters: flicker filter 0..5 and the soften filter on a pattern of
 *     one pixel lines (only visible on a TV); with a Conexant encoder the
 *     registers the kernel changed are recorded (SMBus readback),
 *   3 persist display: draws four colored quadrants, XVideoPersistDisplay,
 *     pb_kill and XLaunchXBEEx of its own XBE with launch data; the second run
 *     checks that AvGetSavedDataAddress is set and that the buffer it points to
 *     (the kernel may move it) holds the same pixels (checksum without dword 1,
 *     which the kernel overwrites), records where the CRTC points at boot,
 *     shows the saved buffer for HOLD_MS, then XVideoSetMode must free it.
 * Results are shown with pb_print, sent to DbgPrint and kept in a magic tagged
 * block (g_results, "PBDISPRS"), one summary row per configuration.
 *
 * Without CONFIG the sample starts with configuration 3 (it relaunches itself)
 * and then cycles 0, 1, 2. make CONFIG=n runs configuration n only.
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

#define NUM_CONFIGS   4
#define CYCLE_MS      12000 /* time per configuration when cycling */
#define HOLD_MS       25000 /* persisted display shown by the second run before XVideoSetMode */
#define FIELD_SAMPLES 16
#define MAX_QUADS     64

/* AvSendTVEncoderOption options (Cxbx-Reloaded src/core/kernel/common/xbox.h) */
#define AV_OPTION_QUERY_MODE   13
#define AV_QUERY_ENCODER_TYPE  16

/* SMBus write addresses of the video encoders (xboxdevwiki "SMBus"; xemu emulates the Conexant) */
#define SMB_CONEXANT 0x8A
#define SMB_FOCUS    0xD4
#define SMB_XCALIBUR 0xE0

/* PVIDEO registers read back (offsets from nouveau dispnv04/nvreg.h) */
#define PVIDEO_BUFFER     0x00008700
#define PVIDEO_STOP       0x00008704
#define PVIDEO_BASE0      0x00008900
#define PVIDEO_LIMIT0     0x00008908
#define PVIDEO_OFFSET0    0x00008920
#define PVIDEO_SIZE_IN0   0x00008928
#define PVIDEO_POINT_IN0  0x00008930
#define PVIDEO_DS_DX0     0x00008938
#define PVIDEO_DT_DY0     0x00008940
#define PVIDEO_POINT_OUT0 0x00008948
#define PVIDEO_SIZE_OUT0  0x00008950
#define PVIDEO_FORMAT0    0x00008958
#define PVIDEO_COLOR_KEY  0x00008B00
#define PCRTC_START       0x00600800

/* overlay layout */
#define OV_W        160
#define OV_H        120
#define OV_PITCH    320
#define OV_DST_X    160
#define OV_DST_Y    216
#define OV_DST_W    320
#define OV_DST_H    240
#define OV_KEY      0xFF00FF
#define OV_SWAPS    20

#define COLOR_CLEAR 0x203040

static const char *config_names[NUM_CONFIGS] = {"0 mode flags", "1 overlay YUY2 2x key", "2 TV filters",
                                                "3 persist display"};

typedef struct {
    float pos[4];
    float color[3];
} __attribute__((packed)) Vertex;

/* mode flag cases */
typedef struct {
    uint32_t flags;
    int width, height;
} mf_case_t;

static const mf_case_t mf_cases[] = {
    {0, 640, 480},
    {XVIDEO_FLAG_INTERLACED, 640, 480},
    {XVIDEO_FLAG_PROGRESSIVE, 640, 480},
    {XVIDEO_FLAG_WIDESCREEN, 640, 480},
    {XVIDEO_FLAG_INTERLACED | XVIDEO_FLAG_WIDESCREEN, 640, 480},
    {XVIDEO_FLAG_10X11PIXELASPECTRATIO, 640, 480},
    {XVIDEO_FLAG_10X11PIXELASPECTRATIO | XVIDEO_FLAG_WIDESCREEN | XVIDEO_FLAG_INTERLACED, 640, 480},
    {XVIDEO_FLAG_FIELD, 640, 480},
    {XVIDEO_FLAG_10X11PIXELASPECTRATIO, 720, 480},
    {XVIDEO_FLAG_INTERLACED | XVIDEO_FLAG_PROGRESSIVE, 640, 480},
    {0x200, 640, 480},
};
#define NUM_MF (sizeof(mf_cases) / sizeof(mf_cases[0]))

typedef struct {
    uint32_t flags, ok, expect_ok, fails;
    uint32_t av_kernel, av_hal, mode_flags;
} mf_result_t;

#define TV_STEPS 8
#define TV_DIFFS 6
typedef struct {
    uint32_t setting;  /* 0x00..0x05 flicker level, 0x101 soften on, 0x100 soften off */
    uint32_t changed;  /* encoder registers that changed */
    uint8_t reg[TV_DIFFS], val[TV_DIFFS];
} tv_step_t;

typedef struct {
    uint32_t done, pass, fails;
} summary_t;

typedef struct {
    char magic[8];
    uint32_t config;
    uint32_t state; /* 0 init, 1 running, 2 results valid, 5 holding the persisted display */
    uint32_t pass, fails;
    uint32_t encoder_settings, video_flags, av_pack;
    /* 0: mode flags */
    mf_result_t mf[NUM_MF];
    uint32_t display_av_mode, display_flags;
    int32_t field[FIELD_SAMPLES];
    uint32_t field_vbl[FIELD_SAMPLES];
    /* 1: overlay */
    uint32_t ov_reject;           /* bit per invalid description rejected (expect 0x3F) */
    uint32_t ov_regs[13];         /* BUFFER STOP BASE LIMIT OFFSET SIZE_IN POINT_IN DS_DX DT_DY POINT_OUT SIZE_OUT FORMAT COLOR_KEY */
    uint32_t ov_expect[13];
    uint32_t ov_phys[2];
    uint32_t ov_swaps, ov_lat_min, ov_lat_max, ov_offset_fails;
    uint32_t ov_fb_key_pixel;     /* frame buffer at the overlay center */
    uint32_t ov_buffer_after_disable;
    /* 2: TV filters */
    uint32_t enc_probe;           /* bit 0 Conexant, 1 Focus, 2 Xcalibur answered on SMBus */
    uint32_t enc_type;            /* AV_QUERY_ENCODER_TYPE */
    tv_step_t tv[TV_STEPS];
    /* 3: persist display */
    uint32_t persist_run;         /* 2 = second run (after XLaunchXBEEx) */
    uint32_t saved_addr, launch_saved_addr, saved_phys, launch_saved_phys, crtc_start, persist_pitch;
    uint32_t checksum, launch_checksum;
    uint32_t pixels[5], launch_pixels[5], crtc_pixels[5];
    uint32_t saved_head[4];       /* first dwords of the saved buffer in the second run */
    uint32_t saved_after_setmode;
    uint32_t persist_first_fails;
    summary_t summary[NUM_CONFIGS];
} results_t;

static volatile results_t g_results __attribute__((aligned(16))) = {"PBDISPRS"};

/* launch data of the persist test */
typedef struct {
    char magic[8];
    uint32_t saved_addr, saved_phys, size, pitch, checksum;
    uint32_t pixels[5];
    uint32_t first_fails;
} launch_t;
static const char launch_magic[8] = {'P', 'B', 'D', 'I', 'S', 'P', 'L', 'D'};

static const int px_x[5] = {160, 480, 160, 480, 320};
static const int px_y[5] = {120, 120, 360, 360, 240};
static const uint32_t px_color[5] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00, 0xFFFFFF};

static Vertex *alloc_vertices;
static int num_quads;
static int status_lines;
static char status[16][64];

static void init_shader (void);
static void set_attrib_pointer (unsigned int index, unsigned int format, unsigned int size, unsigned int stride,
                                const void *data);

static void log_line (const char *fmt, ...)
{
    va_list ap;
    if (status_lines >= 16) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(status[status_lines], sizeof(status[0]), fmt, ap);
    va_end(ap);
    DbgPrint("pbkit_display: %s\n", status[status_lines]);
    status_lines++;
}

/* ---- drawing ---- */

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

/* Draws a solid quad; the vertices stay untouched until the frame is on screen
 * (double buffering + pb_wait_for_flip before the next frame) */
static void quad (float x0, float y0, float x1, float y1, uint32_t color)
{
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    Vertex *v;
    uint32_t *p;
    int i;

    if (num_quads >= MAX_QUADS) {
        return;
    }
    v = &alloc_vertices[num_quads * 4];
    for (i = 0; i < 4; i++) {
        v[i].pos[0] = xy[i][0];
        v[i].pos[1] = xy[i][1];
        v[i].pos[2] = 0.0f;
        v[i].pos[3] = 1.0f;
        v[i].color[0] = ((color >> 16) & 0xFF) / 255.0f;
        v[i].color[1] = ((color >> 8) & 0xFF) / 255.0f;
        v[i].color[2] = (color & 0xFF) / 255.0f;
    }
    p = pb_begin();
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_QUADS);
    p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
                 MASK(NV097_DRAW_ARRAYS_COUNT, 3) | MASK(NV097_DRAW_ARRAYS_START_INDEX, num_quads * 4));
    p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
    pb_end(p);
    num_quads++;
}

static void begin_frame (uint32_t clear)
{
    pb_wait_for_flip();
    pb_reset();
    pb_target_back_buffer();
    pb_erase_depth_stencil_buffer(0, 0, WIDTH, HEIGHT);
    pb_fill(0, 0, WIDTH, HEIGHT, clear);
    pb_erase_text_screen();
    setup_draw_state();
    num_quads = 0;
}

static void end_frame (int lines)
{
    int i;
    for (i = 0; i < status_lines && i < lines; i++) {
        pb_print("%s\n", status[i]);
    }
    pb_draw_text_screen();
    while (pb_finished()) {
    }
}

static int start_pbkit (void)
{
    int status_code;

    pb_set_color_format(NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8, false);
    pb_set_depth_format(NV097_SET_SURFACE_FORMAT_ZETA_Z24S8);
    pb_back_buffers(2);
    if ((status_code = pb_init())) {
        DbgPrint("pbkit_display: pb_init error %d\n", status_code);
        return -1;
    }
    pb_show_front_screen();
    init_shader();
    alloc_vertices = MmAllocateContiguousMemoryEx(MAX_QUADS * 4 * sizeof(Vertex), 0, 0x3ffb000, 0,
                                                  PAGE_READWRITE | PAGE_WRITECOMBINE);
    return 0;
}

static void stop_pbkit (void)
{
    while (pb_busy()) {
    }
    MmFreeContiguousMemory(alloc_vertices);
    pb_kill();
}

static uint32_t query_av_mode (void)
{
    ULONG mode = 0;
    AvSendTVEncoderOption((PVOID)VIDEO_BASE, AV_OPTION_QUERY_MODE, 0, &mode);
    return mode;
}

static bool av_mode_interlaced (uint32_t mode)
{
    uint32_t out = mode & XVIDEO_AV_MODE_OUT_MASK;
    if (out == 0xC0000000) return false;
    if (out == 0x80000000) return (mode & 0x7F) == 3;
    return true;
}

static void finish_config (int index, uint32_t fails)
{
    g_results.fails = fails;
    g_results.pass = fails ? 0 : 1;
    g_results.state = 2;
    g_results.summary[index].done = 1;
    g_results.summary[index].pass = g_results.pass;
    g_results.summary[index].fails = fails;
}

/* ---- 0: mode flags ---- */

static void run_mode_flags (void)
{
    uint32_t enc = XVideoGetEncoderSettings();
    uint32_t pack = enc & VIDEO_ADAPTER_MASK;
    bool pal = (enc & VIDEO_STANDARD_MASK) == 0x300;
    bool vga = pack == AV_PACK_VGA;
    bool p480 = (pack == AV_PACK_HDTV) && !pal && (enc & VIDEO_MODE_480P);
    uint32_t fails = 0, t0, frame;
    unsigned int i;

    g_results.state = 1;
    for (i = 0; i < NUM_MF; i++) {
        const mf_case_t *c = &mf_cases[i];
        volatile mf_result_t *r = &g_results.mf[i];
        uint32_t before = XVideoGetAvMode();
        uint32_t f = c->flags;
        bool expect;

        if (f & (XVIDEO_FLAG_FIELD | 0x200) || (f & XVIDEO_FLAG_INTERLACED && f & XVIDEO_FLAG_PROGRESSIVE)) {
            expect = false;
        } else if ((f & XVIDEO_FLAG_10X11PIXELASPECTRATIO) && (c->width != 640 || vga)) {
            expect = false;
        } else if (f & XVIDEO_FLAG_INTERLACED) {
            expect = !vga;
        } else if (f & XVIDEO_FLAG_PROGRESSIVE) {
            expect = vga || p480;
        } else {
            expect = true;
        }
        r->flags = f;
        r->expect_ok = expect;
        r->ok = XVideoSetModeEx(c->width, c->height, 32, REFRESH_DEFAULT, f) ? 1 : 0;
        r->av_hal = XVideoGetAvMode();
        r->av_kernel = query_av_mode();
        r->mode_flags = XVideoGetModeFlags();
        r->fails = 0;
        if (r->ok != (uint32_t)expect) r->fails |= 0x01;
        if (r->ok) {
            bool il = av_mode_interlaced(r->av_kernel);
            bool sdtv = (r->av_kernel & XVIDEO_AV_MODE_OUT_MASK) < 0x80000000;
            if (r->av_kernel != r->av_hal) r->fails |= 0x02;
            if ((f & XVIDEO_FLAG_INTERLACED) && !il) r->fails |= 0x04;
            if ((f & XVIDEO_FLAG_PROGRESSIVE) && il) r->fails |= 0x04;
            if (!!(r->mode_flags & XVIDEO_FLAG_INTERLACED) != il) r->fails |= 0x04;
            if ((f & XVIDEO_FLAG_WIDESCREEN) && sdtv && !(r->av_kernel & XVIDEO_AV_MODE_WSS)) r->fails |= 0x08;
            if (!(f & XVIDEO_FLAG_WIDESCREEN) && (r->av_kernel & XVIDEO_AV_MODE_WSS)) r->fails |= 0x08;
            if (f & XVIDEO_FLAG_10X11PIXELASPECTRATIO) {
                uint32_t crtc = (r->av_kernel >> 8) & 0xFF;
                if (crtc < 0x0D || crtc > 0x0F) r->fails |= 0x10;
            }
        } else if (XVideoGetAvMode() != before) {
            r->fails |= 0x20; /* a refused mode must not change the current one */
        }
        fails |= r->fails;
    }

    /* the display: widescreen (the scan type XVideoSetMode picks) */
    if (!XVideoSetModeEx(WIDTH, HEIGHT, 32, REFRESH_DEFAULT, XVIDEO_FLAG_WIDESCREEN)) {
        XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT);
        fails |= 0x40;
    }
    g_results.display_av_mode = query_av_mode();
    g_results.display_flags = XVideoGetModeFlags();
    if (start_pbkit()) {
        return;
    }
    {
        bool il = av_mode_interlaced(g_results.display_av_mode), alternating = true, unknown = true;
        DWORD vbl;
        int field;
        for (i = 0; i < FIELD_SAMPLES; i++) {
            pb_wait_for_vbl();
            pb_get_display_field_status(&vbl, &field);
            g_results.field[i] = field;
            g_results.field_vbl[i] = vbl;
            if (field != -1) unknown = false;
            if (i && field != -1 && g_results.field[i - 1] != -1 &&
                (uint32_t)(field ^ g_results.field[i - 1]) != ((vbl - g_results.field_vbl[i - 1]) & 1))
                alternating = false;
        }
        if (il ? (unknown || !alternating) : !unknown) fails |= 0x80;
    }
    finish_config(0, fails);

    status_lines = 0;
    log_line("0 mode flags: pack %u enc %08X dash %08X", pack, enc, g_results.video_flags);
    for (i = 0; i < NUM_MF; i++) {
        volatile mf_result_t *r = &g_results.mf[i];
        log_line("%03X %dx%d %s av %08X fl %03X %s", r->flags, mf_cases[i].width, mf_cases[i].height,
                 r->ok ? "set" : "refused", r->av_kernel, r->mode_flags, r->fails ? "FAIL" : "ok");
    }
    {
        char seq[FIELD_SAMPLES + 1];
        for (i = 0; i < FIELD_SAMPLES; i++) seq[i] = g_results.field[i] < 0 ? '-' : (char)('0' + g_results.field[i]);
        seq[FIELD_SAMPLES] = 0;
        log_line("shown av %08X flags %03X field %s", g_results.display_av_mode, g_results.display_flags, seq);
    }
    log_line("results %08X RESULT: %s (fails 0x%02X)", (uint32_t)&g_results, fails ? "FAIL" : "PASS", fails);

    t0 = KeTickCount;
    for (frame = 0;; frame++) {
        float x = (float)((frame * 6) % (WIDTH - 40));
#ifndef CONFIG
        if ((uint32_t)(KeTickCount - t0) >= CYCLE_MS) break;
#endif
        begin_frame(COLOR_CLEAR);
        quad(x, 440.0f, x + 40.0f, 470.0f, 0xFFC000);
        end_frame(16);
    }
    stop_pbkit();
}

/* ---- 1: overlay ---- */

/* 100 % color bars, BT.601 studio range Y Cb Cr: white yellow cyan green magenta red blue black */
static const uint8_t bars_ycbcr[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                         {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};

/* YUY2 (UYVY with OV_UYVY) color bars; marker: a white block moving along the bottom rows (shows the swaps) */
static void fill_overlay (uint8_t *s, int order, int marker)
{
    int x, y;
    for (y = 0; y < OV_H; y++) {
        uint8_t *line = s + y * OV_PITCH;
        for (x = 0; x < OV_W; x += 2) {
            int bar = x / (OV_W / 8);
            const uint8_t *c;
            if (order) bar = 7 - bar;
            c = bars_ycbcr[bar];
            if (y >= OV_H - 16 && x >= marker && x < marker + 16) c = bars_ycbcr[0];
            else if (y >= OV_H - 16) c = bars_ycbcr[7];
#ifdef OV_UYVY
            line[x * 2 + 0] = c[1];
            line[x * 2 + 1] = c[0];
            line[x * 2 + 2] = c[2];
            line[x * 2 + 3] = c[0];
#else
            line[x * 2 + 0] = c[0];
            line[x * 2 + 1] = c[1];
            line[x * 2 + 2] = c[0];
            line[x * 2 + 3] = c[2];
#endif
        }
    }
    __asm__ __volatile__("sfence");
}

static void read_overlay_regs (volatile uint32_t *r)
{
    static const uint32_t a[13] = {PVIDEO_BUFFER,  PVIDEO_STOP,     PVIDEO_BASE0,     PVIDEO_LIMIT0,   PVIDEO_OFFSET0,
                                   PVIDEO_SIZE_IN0, PVIDEO_POINT_IN0, PVIDEO_DS_DX0,   PVIDEO_DT_DY0,   PVIDEO_POINT_OUT0,
                                   PVIDEO_SIZE_OUT0, PVIDEO_FORMAT0, PVIDEO_COLOR_KEY};
    int i;
    for (i = 0; i < 13; i++) r[i] = VIDEOREG(a[i]);
}

static void draw_overlay_scene (uint32_t frame)
{
    float x = (float)(OV_DST_X + (frame * 3) % (OV_DST_W - 24));
    begin_frame(COLOR_CLEAR);
    quad(OV_DST_X, OV_DST_Y, OV_DST_X + OV_DST_W, OV_DST_Y + OV_DST_H, OV_KEY);
    quad(400.0f, 380.0f, 520.0f, 470.0f, 0x00C000);   /* hides the overlay's bottom right corner */
    quad(x, OV_DST_Y - 10.0f, x + 24.0f, OV_DST_Y + 120.0f, 0xFFFF00); /* moving bar over the top half */
    quad(20.0f, 440.0f, 140.0f, 470.0f, 0x808080);
}

static uint32_t wait_overlay_applied (uint32_t vbl_at_update)
{
    uint32_t t0 = KeTickCount;
    while (pb_overlay_update_pending() && (uint32_t)(KeTickCount - t0) < 200) {
    }
    if (pb_overlay_update_pending()) return 0xFFFFFFFF;
    return pb_overlay_last_vbl() - vbl_at_update;
}

static void run_overlay (void)
{
    uint8_t *surf[2];
    pb_overlay ov, bad;
    uint32_t fails = 0, lat, vbl, frame, t0, cur = 0, swap_frame;
    int i;

    g_results.state = 1;
    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT) || start_pbkit()) {
        return;
    }
    for (i = 0; i < 2; i++) {
        surf[i] = MmAllocateContiguousMemoryEx(OV_PITCH * OV_H, 0, 0x03FFAFFF, 0x1000, PAGE_READWRITE | PAGE_WRITECOMBINE);
        g_results.ov_phys[i] = (uint32_t)MmGetPhysicalAddress(surf[i]);
    }
    fill_overlay(surf[0], 0, 0);

    memset(&ov, 0, sizeof(ov));
    ov.surface = surf[0];
    ov.pitch = OV_PITCH;
#ifdef OV_UYVY
    ov.format = PB_OVERLAY_UYVY; /* xemu: asserts (YUY2 only) */
#else
    ov.format = PB_OVERLAY_YUY2;
#endif
    ov.src_width = OV_W;
    ov.src_height = OV_H;
    ov.dst_x = OV_DST_X;
    ov.dst_y = OV_DST_Y;
    ov.dst_width = OV_DST_W;
    ov.dst_height = OV_DST_H;
    ov.color_key_enable = true;
    ov.color_key = OV_KEY;

    /* invalid descriptions: nothing queued */
    g_results.ov_reject = 0;
    if (pb_overlay_update(NULL) == -1) g_results.ov_reject |= 0x01;
    bad = ov; bad.pitch = 100;
    if (pb_overlay_update(&bad) == -1) g_results.ov_reject |= 0x02;
    bad = ov; bad.src_width = 161;
    if (pb_overlay_update(&bad) == -1) g_results.ov_reject |= 0x04;
    bad = ov; bad.dst_width = 16; bad.dst_height = 12; /* 10x down */
    if (pb_overlay_update(&bad) == -1) g_results.ov_reject |= 0x08;
    bad = ov; bad.surface = surf[0] + 4;
    if (pb_overlay_update(&bad) == -1) g_results.ov_reject |= 0x10;
    bad = ov; bad.format = 7;
    if (pb_overlay_update(&bad) == -1) g_results.ov_reject |= 0x20;
    if (g_results.ov_reject != 0x3F || pb_overlay_update_pending()) fails |= 0x01;

    /* first frame of the scene, then the overlay */
    draw_overlay_scene(0);
    end_frame(7);
    vbl = pb_get_vbl_counter();
    if (pb_overlay_update(&ov) != 0) fails |= 0x02;
    lat = wait_overlay_applied(vbl);
    g_results.ov_lat_min = g_results.ov_lat_max = lat;
    if (lat < 1 || lat > 2) fails |= 0x04;

    read_overlay_regs(g_results.ov_regs);
    g_results.ov_expect[0] = 1;
    g_results.ov_expect[1] = 0;
    g_results.ov_expect[2] = 0;
    g_results.ov_expect[3] = 0x03FFFFFF;
    g_results.ov_expect[4] = g_results.ov_phys[0];
    g_results.ov_expect[5] = (OV_H << 16) | OV_W;
    g_results.ov_expect[6] = 0;
    g_results.ov_expect[7] = (OV_W << 20) / OV_DST_W; /* 0x80000: 2x */
    g_results.ov_expect[8] = (OV_H << 20) / OV_DST_H;
    g_results.ov_expect[9] = (OV_DST_Y << 16) | OV_DST_X;
    g_results.ov_expect[10] = (OV_DST_H << 16) | OV_DST_W;
    g_results.ov_expect[11] = OV_PITCH | (ov.format == PB_OVERLAY_YUY2 ? 0x00010000 : 0) | 0x00100000; /* color key */
    g_results.ov_expect[12] = OV_KEY;
    for (i = 0; i < 13; i++) {
        if (i == 0) {
            if (!(g_results.ov_regs[0] & 1)) fails |= 0x08; /* buffer 0 in use */
        } else if (i == 1) {
            /* STOP: xemu reads 0, the hardware value is unknown: recorded only */
        } else if (g_results.ov_regs[i] != g_results.ov_expect[i]) {
            fails |= 0x08;
        }
    }

    /* the frame buffer under the overlay keeps the key color (the overlay is added by the scan-out) */
    draw_overlay_scene(1);
    {
        DWORD f = pb_insert_fence();
        uint8_t *fb = (uint8_t *)pb_back_buffer();
        pb_block_on_fence(f);
        g_results.ov_fb_key_pixel = *(volatile uint32_t *)(fb + (OV_DST_Y + 200) * pb_back_buffer_pitch() +
                                                          (OV_DST_X + 40) * 4) & 0xFFFFFF;
        if (g_results.ov_fb_key_pixel != OV_KEY) fails |= 0x10;
    }
    end_frame(7);

    status_lines = 0;
    log_line("1 overlay: YUY2 %ux%u -> %ux%u at %u,%u key %06X", OV_W, OV_H, OV_DST_W, OV_DST_H, OV_DST_X, OV_DST_Y,
             OV_KEY);
    log_line("reject %02X latency %u vbl regs %s fb %06X", g_results.ov_reject, lat, (fails & 0x08) ? "FAIL" : "ok",
             g_results.ov_fb_key_pixel);
    log_line("offset %08X ds/dx %05X fmt %08X", g_results.ov_regs[4], g_results.ov_regs[7], g_results.ov_regs[11]);
    log_line("swapping surfaces...");

    /* double buffered updates: write the hidden surface, update, the old one is free once applied */
    t0 = KeTickCount;
    swap_frame = 0;
    for (frame = 2;; frame++) {
        draw_overlay_scene(frame);
        end_frame(7);
        if (frame - swap_frame >= 15 && (g_results.ov_swaps < OV_SWAPS || (frame & 63) == 0)) {
            int next = cur ^ 1;
            fill_overlay(surf[next], next, (int)((frame / 15) * 16) % (OV_W - 16));
            ov.surface = surf[next];
            vbl = pb_get_vbl_counter();
            pb_overlay_update(&ov);
            lat = wait_overlay_applied(vbl);
            cur = next;
            swap_frame = frame;
            if (g_results.ov_swaps < OV_SWAPS) {
                g_results.ov_swaps++;
                if (lat < g_results.ov_lat_min) g_results.ov_lat_min = lat;
                if (lat > g_results.ov_lat_max) g_results.ov_lat_max = lat;
                if (VIDEOREG(PVIDEO_OFFSET0) != g_results.ov_phys[cur]) g_results.ov_offset_fails++;
                if (g_results.ov_swaps == OV_SWAPS) {
                    if (g_results.ov_lat_min < 1 || g_results.ov_lat_max > 2) fails |= 0x04;
                    if (g_results.ov_offset_fails) fails |= 0x20;
                    finish_config(1, fails);
                    status_lines = 3;
                    log_line("swaps %u latency %u..%u vbl offset fails %u", g_results.ov_swaps, g_results.ov_lat_min,
                             g_results.ov_lat_max, g_results.ov_offset_fails);
                    log_line("results %08X", (uint32_t)&g_results);
                    log_line("RESULT: %s (fails 0x%02X)", fails ? "FAIL" : "PASS", fails);
                }
            }
        }
#ifndef CONFIG
        if ((uint32_t)(KeTickCount - t0) >= CYCLE_MS && g_results.ov_swaps >= OV_SWAPS) break;
#endif
    }
    pb_overlay_disable();
    wait_overlay_applied(pb_get_vbl_counter());
    g_results.ov_buffer_after_disable = VIDEOREG(PVIDEO_BUFFER);
    stop_pbkit();
    MmFreeContiguousMemory(surf[0]);
    MmFreeContiguousMemory(surf[1]);
}

/* ---- 2: TV filters ---- */

static void snapshot_encoder (uint8_t *regs)
{
    ULONG v;
    int i;
    for (i = 0; i < 256; i++) {
        v = 0;
        HalReadSMBusValue(SMB_CONEXANT, (UCHAR)i, FALSE, &v);
        regs[i] = (uint8_t)v;
    }
}

static void draw_filter_pattern (uint32_t frame)
{
    int y;
    begin_frame(0x000000);
    /* one pixel lines: flicker on an interlaced TV without the flicker filter */
    for (y = 344; y < 472; y += 2) {
        pb_fill(40, y, 260, 1, 0xFFFFFF);
    }
    /* sharp vertical edges and a checkerboard: softened by the soften (luma) filter */
    for (y = 344; y < 472; y += 8) {
        int x;
        for (x = 340; x < 600; x += 16) {
            pb_fill(x + ((y / 8) & 1) * 8, y, 8, 8, 0xFFFFFF);
        }
    }
    quad((float)((frame * 4) % 600), 330.0f, (float)((frame * 4) % 600) + 40.0f, 338.0f, 0xFFC000);
}

static void run_tv_filters (void)
{
    static uint8_t before[256], after[256];
    static const uint32_t steps[TV_STEPS] = {0, 1, 2, 3, 4, 5, 0x101, 0x100};
    uint32_t fails = 0, frame = 0, t0, i, s;
    ULONG v;

    g_results.state = 1;
    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT) || start_pbkit()) {
        return;
    }
    g_results.enc_probe = 0;
    if (HalReadSMBusValue(SMB_CONEXANT, 0, FALSE, &v) == 0) g_results.enc_probe |= 1;
    if (HalReadSMBusValue(SMB_FOCUS, 0, FALSE, &v) == 0) g_results.enc_probe |= 2;
    if (HalReadSMBusValue(SMB_XCALIBUR, 0, FALSE, &v) == 0) g_results.enc_probe |= 4;
    v = 0xFFFFFFFF;
    AvSendTVEncoderOption((PVOID)VIDEO_BASE, AV_QUERY_ENCODER_TYPE, 0, &v);
    g_results.enc_type = v;

    status_lines = 0;
    log_line("2 TV filters: encoder probe %X type %08X", g_results.enc_probe, g_results.enc_type);
    log_line("av mode %08X (on a TV: lines left, checker right)", query_av_mode());
    for (s = 0; s < TV_STEPS; s++) {
        volatile tv_step_t *st = &g_results.tv[s];
        if (g_results.enc_probe & 1) snapshot_encoder(before);
        if (steps[s] < 0x100) {
            XVideoSetFlickerFilter((int)steps[s]);
        } else {
            XVideoSetSoftenFilter(steps[s] & 1);
        }
        st->setting = steps[s];
        st->changed = 0;
        if (g_results.enc_probe & 1) {
            snapshot_encoder(after);
            for (i = 0; i < 256; i++) {
                if (before[i] != after[i]) {
                    if (st->changed < TV_DIFFS) {
                        st->reg[st->changed] = (uint8_t)i;
                        st->val[st->changed] = after[i];
                    }
                    st->changed++;
                }
            }
        }
        if (steps[s] < 0x100) {
            log_line("flicker %u: %u regs %02X=%02X %02X=%02X", steps[s], st->changed, st->reg[0], st->val[0],
                     st->reg[1], st->val[1]);
        } else {
            log_line("soften %s: %u regs %02X=%02X %02X=%02X", (steps[s] & 1) ? "on" : "off", st->changed,
                     st->reg[0], st->val[0], st->reg[1], st->val[1]);
        }
        t0 = KeTickCount;
        while ((uint32_t)(KeTickCount - t0) < 1500) {
            draw_filter_pattern(frame++);
            end_frame(16);
        }
    }
    finish_config(2, fails);
    log_line("results %08X", (uint32_t)&g_results);
    log_line("RESULT: PASS (the filters are visible on a TV only)");

    t0 = KeTickCount;
    for (;;) {
#ifndef CONFIG
        if ((uint32_t)(KeTickCount - t0) >= CYCLE_MS / 2) break;
#endif
        draw_filter_pattern(frame++);
        end_frame(16);
    }
    XVideoSetFlickerFilter(5);
    XVideoSetSoftenFilter(FALSE);
    stop_pbkit();
}

/* ---- 3: persist display ---- */

static uint32_t checksum_phys (uint32_t phys, uint32_t size)
{
    uint32_t sum = 0, i, done;
    for (done = 0; done < size; done += 0x10000) {
        uint32_t n = (size - done) < 0x10000 ? (size - done) : 0x10000;
        volatile uint32_t *p = MmMapIoSpace(phys + done, n, PAGE_READWRITE | PAGE_NOCACHE);
        if (p == NULL) return 0;
        for (i = 0; i < n / 4; i++) {
            /* dword 1 skipped: the kernel writes it on the reboot (see run_persist_receive) */
            if (done || i != 1) sum = sum * 31 + p[i];
        }
        MmUnmapIoSpace((PVOID)p, n);
    }
    return sum;
}

static void pixels_phys (uint32_t phys, uint32_t pitch, volatile uint32_t *out)
{
    int i;
    for (i = 0; i < 5; i++) {
        volatile uint32_t *p = MmMapIoSpace(phys + px_y[i] * pitch + px_x[i] * 4, 4, PAGE_READWRITE | PAGE_NOCACHE);
        out[i] = p ? (*p & 0xFFFFFF) : 0xDEADBEEF;
        if (p) MmUnmapIoSpace((PVOID)p, 4);
    }
}

/* first run: draw, persist, relaunch */
static void run_persist_send (void)
{
    static launch_t ld;
    static char path[512];
    uint32_t t0, i, fails = 0;
    PVOID saved;

    g_results.state = 1;
    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT) || start_pbkit()) {
        return;
    }
    status_lines = 0;
    log_line("3 persist display: relaunching in 3 s");
    t0 = KeTickCount;
    while ((uint32_t)(KeTickCount - t0) < 1000) {
        begin_frame(0x000000);
        quad(0, 0, 320, 240, px_color[0]);
        quad(320, 0, 640, 240, px_color[1]);
        quad(0, 240, 320, 480, px_color[2]);
        quad(320, 240, 640, 480, px_color[3]);
        quad(280, 200, 360, 280, px_color[4]);
        end_frame(1);
    }
    /* the last frame on screen, nothing queued */
    while (pb_busy()) {
    }
    for (i = 0; i < 4; i++) pb_wait_for_vbl();

    memset(&ld, 0, sizeof(ld));
    memcpy(ld.magic, launch_magic, 8);
    if (!XVideoPersistDisplay()) fails |= 0x01;
    saved = AvGetSavedDataAddress();
    ld.saved_addr = (uint32_t)saved;
    ld.saved_phys = saved ? (uint32_t)MmGetPhysicalAddress(saved) : 0;
    ld.size = saved ? MmQueryAllocationSize(saved) : 0;
    ld.pitch = 2560;
    if ((VIDEOREG(PCRTC_START) & 0x03FFFFFF) != ld.saved_phys) fails |= 0x02;
    if (ld.saved_phys) {
        volatile uint32_t *p = saved;
        for (i = 0; i < WIDTH * HEIGHT; i++) {
            if (i != 1) ld.checksum = ld.checksum * 31 + p[i];
        }
        pixels_phys(ld.saved_phys, ld.pitch, ld.pixels);
        for (i = 0; i < 5; i++) {
            if (ld.pixels[i] != px_color[i]) fails |= 0x04;
        }
    }
    ld.first_fails = fails;
    DbgPrint("pbkit_display: persisted %08X phys %08X size %u checksum %08X fails %X\n", ld.saved_addr,
             ld.saved_phys, ld.size, ld.checksum, fails);
    g_results.state = 3; /* the persisted copy on screen */
    g_results.saved_addr = ld.saved_addr;
    g_results.saved_phys = ld.saved_phys;
    g_results.checksum = ld.checksum;
    for (i = 0; i < 12 * 60; i++) pb_wait_for_vbl();
    stop_pbkit();

    /* relaunch this XBE (its own path, e.g. \Device\CdRom0\default.xbe) */
    memcpy(path, XeImageFileName->Buffer, XeImageFileName->Length);
    path[XeImageFileName->Length] = 0;
    XLaunchXBEEx(path, &ld);
    DbgPrint("pbkit_display: XLaunchXBEEx(%s) returned\n", path);
}

/* second run */
static void run_persist_receive (const launch_t *ld)
{
    uint32_t fails = ld->first_fails, i, t0;
    PVOID saved = AvGetSavedDataAddress();

    g_results.persist_run = 2;
    g_results.state = 5;
    g_results.persist_first_fails = ld->first_fails;
    g_results.saved_addr = (uint32_t)saved;
    g_results.launch_saved_addr = ld->saved_addr;
    g_results.saved_phys = saved ? (uint32_t)MmGetPhysicalAddress(saved) : 0;
    g_results.launch_saved_phys = ld->saved_phys;
    g_results.crtc_start = VIDEOREG(PCRTC_START) & 0x03FFFFFF;
    g_results.persist_pitch = ld->pitch;
    g_results.launch_checksum = ld->checksum;
    for (i = 0; i < 5; i++) g_results.launch_pixels[i] = ld->pixels[i];
    /* the kernel may move the persisted pages at boot: the address can differ, the contents not */
    if (saved == NULL) fails |= 0x08;
    if (g_results.saved_phys) {
        g_results.checksum = checksum_phys(g_results.saved_phys, WIDTH * 4 * HEIGHT);
        pixels_phys(g_results.saved_phys, ld->pitch, g_results.pixels);
    }
    pixels_phys(g_results.crtc_start, ld->pitch, g_results.crtc_pixels);
    if (g_results.saved_phys) {
        volatile uint32_t *h = MmMapIoSpace(g_results.saved_phys, 16, PAGE_READWRITE | PAGE_NOCACHE);
        if (h) {
            for (i = 0; i < 4; i++) g_results.saved_head[i] = h[i];
            MmUnmapIoSpace((PVOID)h, 16);
        }
    }
    /* the CRTC does not stay on the saved buffer across the reboot (xemu): show it again until
     * the first mode set */
    if (g_results.saved_phys) VIDEOREG(PCRTC_START) = g_results.saved_phys;
    if (g_results.checksum != ld->checksum) fails |= 0x20;
    for (i = 0; i < 5; i++) {
        if (g_results.pixels[i] != ld->pixels[i]) fails |= 0x20;
    }
    DbgPrint("pbkit_display: second run, saved %08X crtc %08X checksum %08X/%08X, holding %u ms\n",
             (uint32_t)saved, g_results.crtc_start, g_results.checksum, ld->checksum, HOLD_MS);

    /* the persisted picture stays on screen until the first mode set */
    t0 = KeTickCount;
    while ((uint32_t)(KeTickCount - t0) < HOLD_MS) {
        Sleep(100);
    }
    g_results.state = 1;
    if (!XVideoSetMode(WIDTH, HEIGHT, 32, REFRESH_DEFAULT)) fails |= 0x80;
    g_results.saved_after_setmode = (uint32_t)AvGetSavedDataAddress();
    if (g_results.saved_after_setmode != 0) fails |= 0x40;
    finish_config(3, fails);

    if (start_pbkit()) {
        return;
    }
    status_lines = 0;
    log_line("3 persist display, second run (first fails %X)", ld->first_fails);
    log_line("saved %08X phys %08X (sent %08X %08X)", g_results.saved_addr, g_results.saved_phys, ld->saved_addr,
             ld->saved_phys);
    log_line("crtc at boot %08X checksum %08X/%08X", g_results.crtc_start, g_results.checksum, ld->checksum);
    log_line("saved dwords %08X %08X %08X %08X", g_results.saved_head[0], g_results.saved_head[1],
             g_results.saved_head[2], g_results.saved_head[3]);
    log_line("pixels %06X %06X %06X %06X %06X", g_results.pixels[0], g_results.pixels[1], g_results.pixels[2],
             g_results.pixels[3], g_results.pixels[4]);
    log_line("after XVideoSetMode saved %08X", g_results.saved_after_setmode);
    log_line("results %08X", (uint32_t)&g_results);
    log_line("RESULT: %s (fails 0x%02X)", fails ? "FAIL" : "PASS", fails);
    t0 = KeTickCount;
    for (i = 0;; i++) {
#ifndef CONFIG
        if ((uint32_t)(KeTickCount - t0) >= CYCLE_MS / 2) break;
#endif
        begin_frame(COLOR_CLEAR);
        quad((float)((i * 6) % 600), 440.0f, (float)((i * 6) % 600) + 40.0f, 470.0f, 0xFFC000);
        end_frame(16);
    }
    stop_pbkit();
}

static void run_config (int index)
{
    status_lines = 0;
    memset((void *)&g_results.config, 0, offsetof(results_t, summary) - offsetof(results_t, config));
    g_results.config = index;
    g_results.encoder_settings = XVideoGetEncoderSettings();
    g_results.video_flags = XVideoGetVideoFlags();
    g_results.av_pack = g_results.encoder_settings & VIDEO_ADAPTER_MASK;
    switch (index) {
    case 0: run_mode_flags(); break;
    case 1: run_overlay(); break;
    case 2: run_tv_filters(); break;
    case 3: run_persist_send(); break;
    }
}

int main (void)
{
    unsigned long type;
    const unsigned char *data;
    const launch_t *ld = NULL;

    if (XGetLaunchInfo(&type, &data) == 0 && type == LDT_TITLE && !memcmp(data, launch_magic, 8)) {
        ld = (const launch_t *)data;
    }
    (void)config_names;
#ifdef CONFIG
    if (CONFIG == 3 && ld) {
        g_results.config = 3;
        g_results.encoder_settings = XVideoGetEncoderSettings();
        g_results.video_flags = XVideoGetVideoFlags();
        run_persist_receive(ld);
        while (1) {
            Sleep(1000);
        }
    }
    run_config(CONFIG);
    while (1) {
        Sleep(1000);
    }
#else
    if (ld) {
        g_results.config = 3;
        g_results.encoder_settings = XVideoGetEncoderSettings();
        g_results.video_flags = XVideoGetVideoFlags();
        run_persist_receive(ld);
    } else {
        run_config(3); /* relaunches */
    }
    while (1) {
        int i;
        for (i = 0; i < 3; i++) {
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
