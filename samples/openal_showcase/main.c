#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <windows.h>
#  include <hal/debug.h>
#else
#  include <windows.h>
#endif

#include <AL/al.h>
#include <AL/alext.h>

#include "showcase_input.h"
#include "showcase_ui.h"
#include "showcase_modes.h"
#include "showcase_scene.h"
#include "showcase_audio.h"
#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_voice.h"
#include "apu_vp.h"
#include "al_source.h"
#include "mcpx_apu_regs.h"

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <stdarg.h>
#  include <string.h>
#  include <nxdk/mount.h>
#  define SHOWCASE_LOG 1
#endif

#ifdef SHOWCASE_LOG
/*
 * E:\openal_showcase.txt: one status line a second, written out every 5 s
 * (and at the first APU stall), so a run on a real console can be read back
 * over FTP. The newest lines are kept when the buffer fills.
 */
static char s_log[48 * 1024];
static size_t s_log_len;

static void log_line(const char *fmt, ...) {
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    if (s_log_len + (size_t)n > sizeof(s_log)) {
        /* drop the older half */
        memmove(s_log, s_log + sizeof(s_log) / 2u, s_log_len - sizeof(s_log) / 2u);
        s_log_len -= sizeof(s_log) / 2u;
    }
    memcpy(s_log + s_log_len, line, (size_t)n);
    s_log_len += (size_t)n;
}

static void log_save(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\openal_showcase.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

/* Called once a frame; logs every second */
static void log_tick(const showcase_app_t *app, const showcase_perf_t *perf, bool apu_ok) {
    static DWORD t0, t_last, t_saved;
    static uint32_t gp_last;
    static bool stalled;
    DWORD now = GetTickCount();

    if (t0 == 0) {
        t0 = t_last = t_saved = now;
        gp_last = apu_ok ? apu_gp_frames((uintptr_t)NV_PAPU_BASE) : 0;
        log_line("openal_showcase: audio %s, APU frame-start retries %lu\n", apu_ok ? "APU (VP+GP)" : "CPU mixer / silent",
                 (unsigned long)apu_vp_start_retries());
        return;
    }
    if (now - t_last < 1000u) {
        return;
    }
    t_last = now;
    if (apu_ok) {
        apu_ac97_stats_t as;
        uint32_t gp = apu_gp_frames((uintptr_t)NV_PAPU_BASE);
        apu_ac97_get_stats(&as);
        /* nxdk's printf has no %f: times in 1/100 ms, fps in 1/10 */
        log_line("t %6lu ms mode %d fps %ld/10 audio %ld/100 ms scene %ld/100 ms voices %2u GP %7lu AC97 %6lu und %lu pad %lu\n",
                 (unsigned long)(now - t0), app->current_mode, (long)(perf->fps * 10.0f),
                 (long)(perf->audio_ms * 100.0f), (long)(perf->scene_ms * 100.0f), (unsigned)perf->voices,
                 (unsigned long)gp, (unsigned long)as.chunks, (unsigned long)as.underruns, (unsigned long)as.padded);
        {   /* the orbit source (mode 1, helicopter loop): its voice as the VP sees it */
            ALsource *o = al_source_get(app->src_orbit);
            ALint st = 0, off = 0;
            alGetSourcei(app->src_orbit, AL_SOURCE_STATE, &st);
            alGetSourcei(app->src_orbit, AL_SAMPLE_OFFSET, &off);
            if (o && o->hw_voice_idx >= 0) {
                uint32_t h = apu_voice_hw_handle((uint32_t)o->hw_voice_idx);
                const volatile uint32_t *r = (h < APU_VP_MAX_HANDLES)
                                                 ? (const volatile uint32_t *)apu_vp_debug_voice_record(h)
                                                 : NULL;
                log_line("   orbit: state %x offset %ld slot %d handle %lu", (unsigned)st, (long)off, o->hw_voice_idx,
                         (unsigned long)h);
                if (r) {
                    log_line(" FMT %08lx LBO %08lx CBO %08lx EBO %08lx pitch %04lx vol %08lx state %08lx",
                             (unsigned long)r[MCPX_VOICE_CFG_FMT / 4], (unsigned long)r[MCPX_VOICE_CUR_PSH_SAMPLE / 4],
                             (unsigned long)r[MCPX_VOICE_PAR_OFFSET / 4], (unsigned long)r[MCPX_VOICE_PAR_NEXT / 4],
                             (unsigned long)(r[MCPX_VOICE_TAR_PITCH_LINK / 4] >> 16), (unsigned long)r[MCPX_VOICE_TAR_VOLA / 4],
                             (unsigned long)r[MCPX_VOICE_PAR_STATE / 4]);
                }
                log_line("\n");
            } else {
                log_line("   orbit: state %x offset %ld, no voice\n", (unsigned)st, (long)off);
            }
        }
        if (gp == gp_last && !stalled) {
            /* Frames stopped: keep the driver's last list operations */
            static apu_vp_trace_t tr[64];
            uint32_t n = apu_vp_debug_trace(tr, 64u), i;
            stalled = true;
            log_line("APU STALLED: GP frame counter stopped; last voice operations:\n");
            for (i = 0; i < n; i++) {
                log_line("  %c %3u %08lx %08lx\n", tr[i].op, (unsigned)tr[i].h, (unsigned long)tr[i].a,
                         (unsigned long)tr[i].t);
            }
            log_save();
            t_saved = now;
        }
        gp_last = gp;
    } else {
        log_line("t %6lu ms mode %d fps %ld/10 audio %ld/100 ms scene %ld/100 ms voices %2u und %u\n",
                 (unsigned long)(now - t0), app->current_mode, (long)(perf->fps * 10.0f), (long)(perf->audio_ms * 100.0f),
                 (long)(perf->scene_ms * 100.0f), (unsigned)perf->voices, (unsigned)perf->underruns);
    }
    if (now - t_saved >= 5000u) {
        log_save();
        t_saved = now;
    }
}
#endif

/* Sources the APU is playing (VP voices of the library's slots) */
static uint32_t apu_active_voices(void) {
    uint32_t i, n = 0;
    for (i = 0; i < apu_voice_slot_count(); i++) {
        if (apu_vp_voice_active(apu_voice_hw_handle(i))) {
            n++;
        }
    }
    return n;
}

int main(void) {
    /* 1. Initialize UI Framebuffer & Video Mode */
    if (showcase_ui_init() != 0) {
        return 1;
    }

    /* 2. NV2A 3D scene and HUD. Without it the text dashboard is shown instead. */
    bool scene_ok = (showcase_scene_init() == 0);
    showcase_ui_set_text_overlay(!scene_ok);

    /* 3. Initialize Gamepad / Auto-Tour Input */
    if (showcase_input_init() != 0) {
        showcase_scene_shutdown();
        showcase_ui_shutdown();
        return 2;
    }

    /* 4. Initialize OpenAL APU Audio & Asset Subsystem */
    static showcase_app_t app;
#ifdef SHOWCASE_HIGH_HANDLES
    /* A/B diagnostic: VP handles 64..127 only, as before handles 0..63 were used */
    apu_vp_debug_set_flags(APU_VP_DBG_HANDLES_HIGH);
#endif
    if (showcase_app_init(&app) != 0) {
        showcase_input_shutdown();
        showcase_scene_shutdown();
        showcase_ui_shutdown();
        return 3;
    }

    /* 5. Sound output. Built with SHOWCASE_BACKEND=apu (the default) on a real
     *    console, alcOpenDevice() brought up the MCPX APU: the VP plays the
     *    sources and the library forwards the GP output to the AC97. Otherwise
     *    (null backend) the software mixer plays them. */
    bool apu_ok = apu_voice_hw_backend();
    bool audio_ok = !apu_ok && (showcase_audio_init() == 0);
    showcase_scene_set_audio_output(apu_ok ? SHOWCASE_AUDIO_APU
                                           : ((audio_ok && showcase_audio_active()) ? SHOWCASE_AUDIO_CPU
                                                                                    : SHOWCASE_AUDIO_SILENT));

    showcase_input_t input;
    bool running = true;

    /* Frame timing for the HUD (QueryPerformanceCounter ticks) */
    LARGE_INTEGER freq, t_frame, t_prev, t_a, t_b;
    showcase_perf_t perf = { 0.0f, 0.0f, 0.0f, 0, 0 };
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_prev);

    /* 6. Main loop, one iteration per video frame */
    while (running) {
        showcase_ui_begin_frame();
        showcase_input_poll(&input);

        /* Exit combo: Back + Start */
        if (input.btn_back && input.btn_start) {
            running = false;
        }

        showcase_app_update(&app, &input);

        /* Library frame tick: reaps finished voices, promotes waiting sources
         * and, on the APU, feeds the AC97 from the GP output */
        QueryPerformanceCounter(&t_a);
        alXboxUpdateVoices();
        if (audio_ok) {
            showcase_audio_update();
        }
        QueryPerformanceCounter(&t_b);
        /* Smoothed: one mixed chunk is 10.7 ms of audio, so per-frame cost jitters */
        perf.audio_ms += (1000.0f * (float)(t_b.QuadPart - t_a.QuadPart) / (float)freq.QuadPart - perf.audio_ms) * 0.1f;
        if (audio_ok) {
            showcase_audio_get_stats(&perf.voices, &perf.underruns);
        } else if (apu_ok) {
            apu_ac97_stats_t as;
            apu_ac97_get_stats(&as);
            perf.voices = apu_active_voices();
            perf.underruns = as.underruns + as.padded;   /* both are audible gaps */
        }
#ifdef SHOWCASE_LOG
        log_tick(&app, &perf, apu_ok);
#endif

        if (scene_ok) {
            QueryPerformanceCounter(&t_a);
            showcase_scene_set_perf(&perf);
            showcase_scene_build(&app, &input);
            showcase_scene_submit();
            QueryPerformanceCounter(&t_b);
            perf.scene_ms += (1000.0f * (float)(t_b.QuadPart - t_a.QuadPart) / (float)freq.QuadPart - perf.scene_ms) * 0.1f;
        }
        showcase_app_render_ui(&app);
        showcase_ui_end_frame();

        /* The 3D frame is paced by the vertical blank (pb_wait_for_vbl in
         * showcase_ui_begin_frame); only the text fallback needs a sleep */
        if (!scene_ok) {
            Sleep(20);
        }

        QueryPerformanceCounter(&t_frame);
        if (t_frame.QuadPart > t_prev.QuadPart) {
            float frame_s = (float)(t_frame.QuadPart - t_prev.QuadPart) / (float)freq.QuadPart;
            float fps = 1.0f / frame_s;
            /* Motion uses the real frame time, clamped against stalls */
            app.dt = (frame_s < 0.005f) ? 0.005f : ((frame_s > 0.05f) ? 0.05f : frame_s);
            perf.fps = (perf.fps <= 0.0f) ? fps : perf.fps + (fps - perf.fps) * 0.1f;
        }
        t_prev = t_frame;
    }

#ifdef SHOWCASE_LOG
    log_line("exit\n");
    log_save();
#endif

    /* 7. Clean Teardown */
    showcase_audio_shutdown();
    showcase_app_shutdown(&app);
    showcase_input_shutdown();
    showcase_scene_shutdown();
    showcase_ui_shutdown();

    return 0;
}
