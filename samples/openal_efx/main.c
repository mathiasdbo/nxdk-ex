/*
 * ALC_EXT_EFX and AL_XBOX_gp_program on the real MCPX APU (task phase8-05,
 * lib/openal/docs/EFX_PLAN.md). Everything goes through the AL API the way a
 * port uses it: EFX entry points from alGetProcAddress, the final mix read
 * back through AL_XBOX_output_callback. Clean-room: our own sounds and the
 * library's own kernels.
 *
 *   1  EFX reverb: AL_EFFECT_REVERB on a slot, a click on send 0; the tail's
 *      decay against the T60, and no tail with the send off
 *   2  echo kernel (dsp/gp_kecho.asm) through alXboxEffectProgram: was the code
 *      loaded through the P window or did the GP restart; dry click -> echo
 *      distance on both ears (expected: exactly the delay)
 *   3  live parameters: a new delay and wet gain on re-attach (mailbox, no
 *      reload), mailbox latency in frames
 *   4  feedback: echoes at D, 2D, 3D, 6 dB apart
 *   5  two sources on sends 0 and 1 into one slot
 *   6  back to the reverb: its code reloaded, AL_XBOX_REVERB_ACTIVE
 *  6b  AL_FILTER_LOWPASS: 500 Hz + 5 kHz through direct filters, plain and HRTF
 *      voices; the 5 kHz level against GAINHF, GAIN on both, a live change
 *  6c  same-frame starts and timed parameters: two streams, one per ear, started
 *      by alSourcePlayv (and by two alSourcePlay for comparison); a timed echo
 *      wet gain (alXboxEffectSlotAtSample) against a marker click: how many
 *      samples after the marker the change lands
 *   7  listening: an HRTF source circling, a panned one and a stereo one, all
 *      sending to the slot; reverb and echo settings change every 15 s;
 *      output health logged every minute, 5 minutes (then plays on)
 *
 * GP load (AL_XBOX_GP_LOAD / _PEAK) is logged with each effect.
 * E:\openal_efx.txt gets the results (rewritten at every save).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_vp.h"
#include "gp_kecho.h"         /* lib/openal/dsp: the example kernel's image */
#include "mcpx_apu_regs.h"
#include "apu_gp_loop.h"
#include "al_source.h"         /* al_source_debug_timed_frame */

#define BAR0    ((uintptr_t)NV_PAPU_BASE)
#define LOGFILE "E:\\openal_efx.txt"
#define TWO_PI  6.28318531f

/* ---------------------------------------------------------------- log */

static char s_log[32 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    char line[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
    if (s_log_len + (size_t)n < sizeof(s_log)) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
    debugPrint("%s", line);
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen(LOGFILE, "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

/* ---------------------------------------------------------------- EFX entry points, as a port loads them */

static LPALGENEFFECTS p_alGenEffects;
static LPALDELETEEFFECTS p_alDeleteEffects;
static LPALEFFECTI p_alEffecti;
static LPALEFFECTF p_alEffectf;
static LPALGENAUXILIARYEFFECTSLOTS p_alGenAuxiliaryEffectSlots;
static LPALAUXILIARYEFFECTSLOTI p_alAuxiliaryEffectSloti;
static LPALXBOXEFFECTPROGRAM p_alXboxEffectProgram;

static bool load_efx(void) {
    p_alGenEffects = (LPALGENEFFECTS)alGetProcAddress("alGenEffects");
    p_alDeleteEffects = (LPALDELETEEFFECTS)alGetProcAddress("alDeleteEffects");
    p_alEffecti = (LPALEFFECTI)alGetProcAddress("alEffecti");
    p_alEffectf = (LPALEFFECTF)alGetProcAddress("alEffectf");
    p_alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
    p_alAuxiliaryEffectSloti = (LPALAUXILIARYEFFECTSLOTI)alGetProcAddress("alAuxiliaryEffectSloti");
    p_alXboxEffectProgram = (LPALXBOXEFFECTPROGRAM)alGetProcAddress("alXboxEffectProgram");
    return p_alGenEffects && p_alDeleteEffects && p_alEffecti && p_alEffectf && p_alGenAuxiliaryEffectSlots &&
           p_alAuxiliaryEffectSloti && p_alXboxEffectProgram;
}

/* ---------------------------------------------------------------- the library's tick */

static void run_for(DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        alXboxUpdateVoices();
        Sleep(8);
    }
}

static int status(ALenum what) {
    ALint v = -1;
    alXboxGetHardwareStatus(what, &v);
    return (int)v;
}

static void log_load(const char *what) {
    int last, peak;
    run_for(300);
    status(AL_XBOX_GP_LOAD_PEAK);            /* reset the peak */
    run_for(500);
    last = status(AL_XBOX_GP_LOAD);
    peak = status(AL_XBOX_GP_LOAD_PEAK);
    logf_("   %s: GP load %d %%, peak %d %% over 0.5 s\n", what, last, peak);
}

/* ---------------------------------------------------------------- capture of the final mix */

#define CAP_FRAMES 72000u     /* 1.5 s */
static int16_t s_cap[2 * CAP_FRAMES];
static volatile uint32_t s_cap_n;
static volatile int s_cap_on;

static void AL_APIENTRY cap_cb(ALshort *samples, ALsizei frames, ALvoid *user) {
    uint32_t n = s_cap_n, k = (uint32_t)frames;
    (void)user;
    if (!s_cap_on) return;
    if (k > CAP_FRAMES - n) k = CAP_FRAMES - n;
    memcpy(s_cap + 2u * n, samples, 4u * k);
    s_cap_n = n + k;
    if (s_cap_n >= CAP_FRAMES) s_cap_on = 0;
}

/* Capture `ms` of the mix; `start` runs right after the capture begins */
static uint32_t capture(DWORD ms, void (*start)(void)) {
    apu_ac97_stats_t a0, a1;
    s_cap_on = 0;
    s_cap_n = 0;
    apu_ac97_get_stats(&a0);
    s_cap_on = 1;
    if (start) start();
    run_for(ms);
    s_cap_on = 0;
    apu_ac97_get_stats(&a1);
    if (a1.padded != a0.padded || a1.underruns != a0.underruns) {
        logf_("   (capture: %lu padded, %lu underruns: positions may be off)\n",
              (unsigned long)(a1.padded - a0.padded), (unsigned long)(a1.underruns - a0.underruns));
    }
    return s_cap_n;
}

typedef struct {
    uint32_t pos;
    int amp;
} peak_t;

/* Local maxima of |channel| above 1/20 of the loudest (-26 dB), at most `max` */
static uint32_t find_peaks(uint32_t n, int ch, peak_t *out, uint32_t max) {
    uint32_t i, k = 0;
    int top = 0;
    for (i = 0; i < n; i++) {
        int a = abs(s_cap[2u * i + (uint32_t)ch]);
        if (a > top) top = a;
    }
    if (top == 0) return 0;
    for (i = 8; i + 8u < n && k < max; i++) {
        int a = abs(s_cap[2u * i + (uint32_t)ch]), j, is_max = 1;
        if (a * 20 < top) continue;
        for (j = -8; j <= 8 && is_max; j++) {
            int b = abs(s_cap[2u * (uint32_t)((int)i + j) + (uint32_t)ch]);
            if ((j < 0 && b >= a) || (j > 0 && b > a)) is_max = 0;
        }
        if (is_max) {
            out[k].pos = i;
            out[k].amp = a;
            k++;
            i += 8;
        }
    }
    return k;
}

static int db10(double ratio) {   /* tenths of a dB */
    double v = ratio > 0.0 ? 200.0 * log10(ratio) : -9999.0;
    return (int)(v < 0.0 ? v - 0.5 : v + 0.5);
}

/* "-12.3" for tenths of a dB (four buffers, for one printf) */
static const char *dbs(int d) {
    static char buf[4][16];
    static int k;
    char *b = buf[k++ & 3];
    snprintf(b, 16, "%s%d.%d", d < 0 ? "-" : "", abs(d) / 10, abs(d) % 10);
    return b;
}

/* Each peak against the first: "+distance samples (level dB)" */
static void log_peaks(const char *what, uint32_t n) {
    static const char *const ear[2] = { "L", "R" };
    peak_t p[12];
    int ch;
    for (ch = 0; ch < 2; ch++) {
        uint32_t k = find_peaks(n, ch, p, 12), i;
        char rest[160];
        int len = 0;
        if (k == 0) {
            logf_("   %s %s: no sound\n", what, ear[ch]);
            continue;
        }
        rest[0] = 0;
        for (i = 1; i < k && len < (int)sizeof(rest) - 24; i++) {
            int d = db10((double)p[i].amp / (double)p[0].amp);
            len += snprintf(rest + len, sizeof(rest) - (size_t)len, " +%lu (%s dB)",
                            (unsigned long)(p[i].pos - p[0].pos), dbs(d));
        }
        logf_("   %s %s: first at %lu (%d),%s\n", what, ear[ch], (unsigned long)p[0].pos, p[0].amp,
              k > 1 ? rest : " nothing after it");
    }
}

static double energy(uint32_t from, uint32_t to, uint32_t n) {
    double e = 0.0;
    uint32_t i;
    if (to > n) to = n;
    for (i = from; i < to; i++) {
        double l = s_cap[2 * i], r = s_cap[2 * i + 1];
        e += l * l + r * r;
    }
    return e;
}

/* ---------------------------------------------------------------- sounds */

#define CLICK_FRAMES 24000u   /* 0.5 s, the click 100 ms in (a new voice ramps its volume up first) */
#define CLICK_AT     4800u

static ALuint s_click_buf, s_src_a, s_src_b, s_slot;

static ALuint make_click(void) {
    static int16_t pcm[CLICK_FRAMES];
    ALuint b;
    memset(pcm, 0, sizeof(pcm));
    pcm[CLICK_AT] = 16000;
    pcm[CLICK_AT + 1] = 22000;
    pcm[CLICK_AT + 2] = 16000;
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    return b;
}

static ALuint make_source(ALuint buf) {
    ALuint s;
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)buf);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(s, AL_POSITION, 0.0f, 0.0f, 0.0f);
    return s;
}

static void send_to(ALuint src, int send, ALuint slot) {
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, send, AL_FILTER_NULL);
}

static void play_a(void) {
    alSourcePlay(s_src_a);
}

static void play_a_then_b(void) {
    alSourcePlay(s_src_a);
    Sleep(250);
    alSourcePlay(s_src_b);
}

/* ---------------------------------------------------------------- effects */

static ALuint s_reverb, s_echo;

static void attach(ALuint effect) {
    p_alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, (ALint)effect);
}

static void echo_params(int delay, float feedback, float wet) {
    p_alEffecti(s_echo, AL_XBOX_GP_PARAM0 + 0, delay);
    p_alEffectf(s_echo, AL_XBOX_GP_PARAM0 + 1, feedback);
    p_alEffectf(s_echo, AL_XBOX_GP_PARAM0 + 2, wet);
}

static void phase_reverb(void) {
    uint32_t n;
    double e1, e2, e0;
    logf_("-- 1 EFX reverb (AL_EFFECT_REVERB, T60 1.5 s) on send 0\n");
    p_alEffecti(s_reverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    p_alEffectf(s_reverb, AL_REVERB_DECAY_TIME, 1.5f);
    p_alEffectf(s_reverb, AL_REVERB_DECAY_HFRATIO, 1.0f);
    p_alEffectf(s_reverb, AL_REVERB_GAIN, 1.0f);
    p_alEffectf(s_reverb, AL_REVERB_LATE_REVERB_GAIN, 1.0f);
    p_alEffectf(s_reverb, AL_REVERB_GAINHF, 1.0f);
    attach(s_reverb);
    send_to(s_src_a, 0, s_slot);
    logf_("   attach: AL error 0x%x, AL_XBOX_REVERB_ACTIVE %d\n", (unsigned)alGetError(), status(AL_XBOX_REVERB_ACTIVE));
    log_load("reverb");
    run_for(2000);   /* the tail of anything before */
    n = capture(1200, play_a);
    {
        peak_t p[1];
        uint32_t at = find_peaks(n, 0, p, 1) ? p[0].pos : 0;
        e0 = energy(at, at + 2400u, n);                         /* the click and the first 50 ms */
        e1 = energy(at + 7200u, at + 12000u, n);                /* 0.15 .. 0.25 s */
        e2 = energy(at + 31200u, at + 36000u, n);               /* 0.65 .. 0.75 s */
        {
            int d = db10(sqrt(e2 / (e1 > 0.0 ? e1 : 1.0))), t = db10(sqrt(e1 / (e0 > 0.0 ? e0 : 1.0)));
            logf_("   click at %lu; tail at 0.15 s %s dB against the click, 0.5 s later %s dB more "
                  "(expected -20 for T60 1.5 s)\n", (unsigned long)at, dbs(t), dbs(d));
        }
    }
    /* the send off: no tail */
    send_to(s_src_a, 0, AL_EFFECTSLOT_NULL);
    run_for(2500);
    n = capture(1200, play_a);
    {
        peak_t p[1];
        uint32_t at = find_peaks(n, 0, p, 1) ? p[0].pos : 0;
        double t = energy(at + 7200u, at + 36000u, n), c = energy(at, at + 2400u, n);
        int d = db10(sqrt(t / (c > 0.0 ? c : 1.0)));
        logf_("   send off: tail %s dB against the click (expected: nothing)\n", dbs(d));
    }
    send_to(s_src_a, 0, s_slot);
    save_log();
}

static void phase_echo(void) {
    uint32_t r0 = apu_gp_debug_code_restarts(), n;
    apu_ac97_stats_t a0, a1;
    logf_("-- 2 echo kernel (gp_kecho, %u words) through alXboxEffectProgram, delay 1000, wet 1\n",
          (unsigned)GP_KECHO_WORDS);
    p_alEffecti(s_echo, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    p_alXboxEffectProgram(s_echo, gp_kecho_image, (ALsizei)GP_KECHO_IMAGE_WORDS, 0);
    echo_params(1000, 0.0f, 1.0f);
    apu_ac97_get_stats(&a0);
    attach(s_echo);
    run_for(100);
    apu_ac97_get_stats(&a1);
    logf_("   attach: AL error 0x%x; code %s (GP restarts %lu); AC97 padded +%lu around the load; reverb active %d\n",
          (unsigned)alGetError(), apu_gp_debug_code_restarts() == r0 ? "loaded through the P window" : "loaded by a GP RESTART",
          (unsigned long)apu_gp_debug_code_restarts(), (unsigned long)(a1.padded - a0.padded), status(AL_XBOX_REVERB_ACTIVE));
    log_load("echo");
    n = capture(800, play_a);
    log_peaks("delay 1000", n);
    save_log();

    logf_("-- 3 live parameters: delay 2400, wet 0.5 (re-attach, same image)\n");
    r0 = apu_gp_debug_code_restarts();
    echo_params(2400, 0.0f, 0.5f);
    attach(s_echo);
    logf_("   %s; mailbox acknowledged after %lu frames\n",
          apu_gp_debug_code_restarts() == r0 ? "no reload" : "RELOADED", (unsigned long)apu_gp_debug_mailbox_frames(BAR0));
    run_for(100);
    n = capture(800, play_a);
    log_peaks("delay 2400", n);
    save_log();

    logf_("-- 4 feedback 0.5, delay 4800 (expected +4800, +9600, +14400, 6 dB apart)\n");
    echo_params(4800, 0.5f, 1.0f);
    attach(s_echo);
    run_for(100);
    n = capture(1300, play_a);
    log_peaks("feedback", n);
    save_log();

    logf_("-- 5 two sources into one slot: A on send 0, B on send 1 250 ms later, delay 1000\n");
    echo_params(1000, 0.0f, 1.0f);
    attach(s_echo);
    send_to(s_src_b, 1, s_slot);
    run_for(500);
    n = capture(1000, play_a_then_b);
    log_peaks("A + B", n);
    save_log();
}

static void phase_back(void) {
    uint32_t r0 = apu_gp_debug_code_restarts();
    logf_("-- 6 back to the reverb\n");
    attach(s_reverb);
    run_for(100);
    logf_("   AL error 0x%x; %s; AL_XBOX_REVERB_ACTIVE %d\n", (unsigned)alGetError(),
          apu_gp_debug_code_restarts() == r0 ? "code through the P window" : "GP RESTARTED", status(AL_XBOX_REVERB_ACTIVE));
    log_load("reverb again");
    save_log();
}

/* ---------------------------------------------------------------- 6b: lowpass */

#define TONE_FRAMES 48000u

static ALuint make_tones(void) {
    static int16_t pcm[TONE_FRAMES];
    ALuint b;
    uint32_t i;
    for (i = 0; i < TONE_FRAMES; i++) {   /* whole cycles in 960 samples: exact Goertzel bins */
        float t = (float)i / 48000.0f;
        pcm[i] = (int16_t)(5000.0f * sinf(TWO_PI * 500.0f * t) + 5000.0f * sinf(TWO_PI * 5000.0f * t));
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    return b;
}

/* Tone power of the left channel at `hz` over 960-sample blocks */
static double tone_power(uint32_t n, double hz) {
    double c = 2.0 * cos(6.283185307179586 * hz / 48000.0), acc = 0.0;
    uint32_t blk, i, blocks = n / 960u;
    for (blk = 0; blk < blocks; blk++) {
        double s1 = 0.0, s2 = 0.0;
        for (i = 0; i < 960u; i++) {
            double s = s_cap[2u * (blk * 960u + i)] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        acc += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return blocks ? acc / blocks : 0.0;
}

static void measure_tones(double *p500, double *p5k) {
    uint32_t n;
    run_for(150);   /* the voice's volume ramp and a filter change settle */
    n = capture(400, NULL);
    *p500 = tone_power(n, 500.0);
    *p5k = tone_power(n, 5000.0);
}

static void lowpass_on(ALuint src, const char *kind) {
    static const float ghf[] = { 0.9f, 0.7f, 0.5f, 0.25f, 0.1f, 0.03f };
    double r500, r5k, p500, p5k;
    ALuint f = 0;
    uint32_t i;
    LPALGENFILTERS gen = (LPALGENFILTERS)alGetProcAddress("alGenFilters");
    LPALFILTERI fi = (LPALFILTERI)alGetProcAddress("alFilteri");
    LPALFILTERF ff = (LPALFILTERF)alGetProcAddress("alFilterf");
    LPALDELETEFILTERS del = (LPALDELETEFILTERS)alGetProcAddress("alDeleteFilters");
    gen(1, &f);
    fi(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    measure_tones(&r500, &r5k);
    for (i = 0; i < sizeof(ghf) / sizeof(ghf[0]); i++) {
        ff(f, AL_LOWPASS_GAINHF, ghf[i]);
        alSourcei(src, AL_DIRECT_FILTER, (ALint)f);   /* live, while it plays */
        measure_tones(&p500, &p5k);
        logf_("   %s GAINHF %d.%02d: 5 kHz %s dB (expected %s), 500 Hz %s dB\n", kind, (int)ghf[i],
              (int)(ghf[i] * 100.0f + 0.5f) % 100, dbs(db10(sqrt(p5k / r5k))), dbs(db10(ghf[i])),
              dbs(db10(sqrt(p500 / r500))));
    }
    ff(f, AL_LOWPASS_GAINHF, 1.0f);
    ff(f, AL_LOWPASS_GAIN, 0.5f);
    alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
    measure_tones(&p500, &p5k);
    logf_("   %s GAIN 0.5, GAINHF 1: 500 Hz %s dB, 5 kHz %s dB (expected -6.0 both)\n", kind,
          dbs(db10(sqrt(p500 / r500))), dbs(db10(sqrt(p5k / r5k))));
    alSourcei(src, AL_DIRECT_FILTER, AL_FILTER_NULL);
    measure_tones(&p500, &p5k);
    logf_("   %s no filter again: 500 Hz %s dB, 5 kHz %s dB\n", kind, dbs(db10(sqrt(p500 / r500))),
          dbs(db10(sqrt(p5k / r5k))));
    alSourceStop(src);
    del(1, &f);
}

static void phase_lowpass(void) {
    ALuint b = make_tones(), plain, hrtf;
    logf_("-- 6b AL_FILTER_LOWPASS: 500 Hz + 5 kHz, levels against the same voice unfiltered\n");
    plain = make_source(b);
    lowpass_on(plain, "plain");
    alEnable(AL_XBOX_HRTF);
    hrtf = make_source(b);
    alSource3f(hrtf, AL_POSITION, 0.0f, 0.0f, -1.0f);   /* straight ahead */
    logf_("   HRTF %s\n", status(AL_XBOX_HRTF_ACTIVE) ? "active" : "NOT ACTIVE (the HRTF line measures a panned voice)");
    lowpass_on(hrtf, "HRTF ");
    alDisable(AL_XBOX_HRTF);
    alDeleteSources(1, &plain);
    alDeleteSources(1, &hrtf);
    alDeleteBuffers(1, &b);
    logf_("   AL error 0x%x\n", (unsigned)alGetError());
    save_log();
}

/* ---------------------------------------------------------------- 6c: same frame, timed */

#define SYNC_FRAMES 24000u
#define SYNC_MARK   9600u     /* the marker click and the timed change, in the streams' frames */
#define SYNC_STEP   16u       /* the wet stream's clicks */

static ALuint s_sync_src[2];

static ALuint make_stream(ALuint buf, float left, float right) {
    ALuint s;
    ALfloat g[2];
    g[0] = left;
    g[1] = right;
    alGenSources(1, &s);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);
    alSourceQueueBuffers(s, 1, &buf);
    alSourcei(s, AL_XBOX_DIRECT_MODE, AL_TRUE);
    alSourcefv(s, AL_XBOX_DIRECT_GAINS, g);
    return s;
}

static void play_v(void) {
    alSourcePlayv(2, s_sync_src);
}

static void play_each(void) {
    alSourcePlay(s_sync_src[0]);
    alSourcePlay(s_sync_src[1]);
}

static void sync_restart(void) {
    alSourceStopv(2, s_sync_src);
    alSourceRewindv(2, s_sync_src);
}

static ALuint s_timed_fx;
static LPALXBOXEFFECTSLOTATSAMPLE p_alXboxEffectSlotAtSample;

static uint32_t gpx(uint32_t a) {
    return *(volatile uint32_t *)(BAR0 + MCPX_APU_GP_XMEM + 4u * a) & 0xFFFFFFu;
}

static uint32_t s_t_cnt, s_t_at, s_t_seq, s_t_ack, s_t_lib;

static void play_v_timed(void) {
    alSourcePlayv(2, s_sync_src);
    p_alXboxEffectSlotAtSample(s_slot, s_timed_fx, s_sync_src[1], SYNC_MARK);
    s_t_cnt = gpx(GP_LOOP_CNT);
    s_t_at = gpx(GP_LOOP_MB_AT);
    s_t_seq = gpx(GP_LOOP_MB_SEQ);
    s_t_ack = gpx(GP_LOOP_MB_ACK);
    s_t_lib = al_source_debug_timed_frame();
}

static void play_v_only(void) {
    alSourcePlayv(2, s_sync_src);
}

static void log_lr(const char *what, uint32_t n) {
    peak_t pl[4], pr[4];
    uint32_t kl = find_peaks(n, 0, pl, 4), kr = find_peaks(n, 1, pr, 4), i;
    char line[200];
    int len = snprintf(line, sizeof(line), "   %s: L", what);
    for (i = 0; i < kl; i++) len += snprintf(line + len, sizeof(line) - (size_t)len, " %lu(%d)", (unsigned long)pl[i].pos, pl[i].amp);
    len += snprintf(line + len, sizeof(line) - (size_t)len, "; R");
    for (i = 0; i < kr; i++) len += snprintf(line + len, sizeof(line) - (size_t)len, " %lu(%d)", (unsigned long)pr[i].pos, pr[i].amp);
    logf_("%s\n", line);
}

static void phase_sync(void) {
    static int16_t pcm[SYNC_FRAMES], wet[SYNC_FRAMES];
    ALuint b = 0, bw = 0, f = 0;
    LPALGENFILTERS gen_f = (LPALGENFILTERS)alGetProcAddress("alGenFilters");
    LPALFILTERI filt_i = (LPALFILTERI)alGetProcAddress("alFilteri");
    LPALFILTERF filt_f = (LPALFILTERF)alGetProcAddress("alFilterf");
    uint32_t i, n, trial;
    peak_t pl[4], pr[4];

    p_alXboxEffectSlotAtSample = (LPALXBOXEFFECTSLOTATSAMPLE)alGetProcAddress("alXboxEffectSlotAtSample");
    logf_("-- 6c same-frame starts: two streams with one click each, left and right (expected: L = R)\n");
    memset(pcm, 0, sizeof(pcm));
    pcm[SYNC_MARK] = 16000;
    pcm[SYNC_MARK + 1] = 22000;
    pcm[SYNC_MARK + 2] = 16000;
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    s_sync_src[0] = make_stream(b, 1.0f, 0.0f);
    s_sync_src[1] = make_stream(b, 0.0f, 1.0f);
    for (trial = 0; trial < 12; trial++) {
        bool v = trial < 6;
        n = capture(500, v ? play_v : play_each);
        if (find_peaks(n, 0, pl, 1) && find_peaks(n, 1, pr, 1)) {
            logf_("   %s: L at %lu, R at %lu: %ld\n", v ? "alSourcePlayv  " : "2 x alSourcePlay", (unsigned long)pl[0].pos,
                  (unsigned long)pr[0].pos, (long)pr[0].pos - (long)pl[0].pos);
        } else {
            logf_("   %s: no click\n", v ? "alSourcePlayv" : "2 x alSourcePlay");
        }
        sync_restart();
    }
    save_log();

    logf_("-- 6c timed parameters: echo delay 1000, wet 0 -> 1 at stream frame %u (alXboxEffectSlotAtSample)\n",
          (unsigned)SYNC_MARK);
    logf_("   (a marker click at that frame on L, peak 1 later; a click every %u frames on R's stream, only through\n"
          "    the echo, whose echoes fall 8 after a multiple of 16; the wet gain scales the echo's output, so the\n"
          "    first echo shows where the change lands: expected +7 after the marker, + 32 per frame of GP latency;\n"
          "    an R peak on the marker itself is the marker's -64 dB trace: no echo)\n",
          (unsigned)SYNC_STEP);
    memset(wet, 0, sizeof(wet));
    for (i = 0; i + 2u < SYNC_FRAMES; i += SYNC_STEP) {
        wet[i] = 12000;
    }
    alGenBuffers(1, &bw);
    alBufferData(bw, AL_FORMAT_MONO16, wet, (ALsizei)sizeof(wet), 48000);
    alDeleteSources(2, s_sync_src);
    s_sync_src[0] = make_stream(b, 1.0f, 0.0f);     /* the marker, left */
    s_sync_src[1] = make_stream(bw, 0.0f, 0.0f);    /* only into the echo */
    gen_f(1, &f);
    filt_i(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    filt_f(f, AL_LOWPASS_GAIN, 0.0f);
    alSourcei(s_sync_src[1], AL_DIRECT_FILTER, (ALint)f);
    send_to(s_sync_src[1], 0, s_slot);
    p_alGenEffects(1, &s_timed_fx);
    p_alEffecti(s_timed_fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    p_alXboxEffectProgram(s_timed_fx, gp_kecho_image, (ALsizei)GP_KECHO_IMAGE_WORDS, 0);
    p_alEffecti(s_timed_fx, AL_XBOX_GP_PARAM0 + 0, 1000);
    p_alEffectf(s_timed_fx, AL_XBOX_GP_PARAM0 + 2, 1.0f);
    /* control: wet 1 attached at once (no timing): R should click from 1000 */
    attach(s_timed_fx);
    run_for(100);
    n = capture(600, play_v_only);
    log_lr("control, wet 1 at once", n);
    sync_restart();
    for (trial = 0; trial < 6; trial++) {
        echo_params(1000, 0.0f, 0.0f);
        attach(s_echo);
        run_for(100);
        n = capture(600, play_v_timed);
        logf_("   at the call: CNT %lu, MB_AT %lu, SEQ %lu ACK %lu, library frame %lu; after: CNT %lu ACK %lu\n",
              (unsigned long)s_t_cnt, (unsigned long)s_t_at, (unsigned long)s_t_seq, (unsigned long)s_t_ack,
              (unsigned long)s_t_lib, (unsigned long)gpx(GP_LOOP_CNT), (unsigned long)gpx(GP_LOOP_MB_ACK));
        log_lr("timed", n);
        if (find_peaks(n, 0, pl, 1) && find_peaks(n, 1, pr, 1)) {
            logf_("   marker at %lu, first echo at %lu: %+ld\n", (unsigned long)pl[0].pos,
                  (unsigned long)pr[0].pos, (long)pr[0].pos - (long)pl[0].pos);
        } else {
            logf_("   no marker or no echo\n");
        }
        sync_restart();
    }
    logf_("   AL error 0x%x\n", (unsigned)alGetError());
    alDeleteSources(2, s_sync_src);
    p_alDeleteEffects(1, &s_timed_fx);
    echo_params(1000, 0.0f, 1.0f);
    attach(s_reverb);
    run_for(100);
    save_log();
}

/* ---------------------------------------------------------------- 7: listening */

#define LOOP_FRAMES 96000u   /* 2 s */

static ALuint make_mono_loop(uint32_t seed) {
    static int16_t pcm[LOOP_FRAMES];
    ALuint b;
    uint32_t i;
    memset(pcm, 0, sizeof(pcm));
    for (i = 0; i < 9600u; i++) {   /* 200 ms of noise, fading, then silence */
        int v;
        seed = seed * 1664525u + 1013904223u;
        v = (int)((seed >> 16) & 0xFFFFu) - 32768;
        pcm[i] = (int16_t)((float)v * 0.35f * (1.0f - (float)i / 9600.0f));
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    return b;
}

static ALuint make_stereo_loop(void) {
    static int16_t pcm[2 * LOOP_FRAMES];
    ALuint b;
    uint32_t i;
    memset(pcm, 0, sizeof(pcm));
    for (i = 0; i < 24000u; i++) {   /* a 0.5 s plucked fifth: 440 Hz left, 660 Hz right, at 1 s */
        float env = expf(-(float)i / 6000.0f) * 7000.0f, t = (float)i / 48000.0f;
        pcm[2u * (48000u + i)] = (int16_t)(env * sinf(TWO_PI * 440.0f * t));
        pcm[2u * (48000u + i) + 1u] = (int16_t)(env * sinf(TWO_PI * 660.0f * t));
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_STEREO16, pcm, (ALsizei)sizeof(pcm), 48000);
    return b;
}

typedef struct {
    const char *name;
    int echo;              /* 0: reverb */
    float a, b, c;         /* reverb: decay, gain, -; echo: delay (s), feedback, wet */
} setting_t;

static const setting_t k_settings[] = {
    { "reverb, hall (2.5 s)", 0, 2.5f, 0.6f, 0 },
    { "echo 300 ms, feedback 0.45", 1, 0.300f, 0.45f, 0.5f },
    { "echo 150 ms, feedback 0.3 (parameters only)", 1, 0.150f, 0.30f, 0.5f },
    { "reverb, room (0.7 s)", 0, 0.7f, 0.4f, 0 },
};

static void apply(const setting_t *s) {
    if (s->echo) {
        echo_params((int)(s->a * 48000.0f), s->b, s->c);
        attach(s_echo);
    } else {
        p_alEffectf(s_reverb, AL_REVERB_DECAY_TIME, s->a);
        p_alEffectf(s_reverb, AL_REVERB_GAIN, s->b);
        attach(s_reverb);
    }
}

static void phase_listen(void) {
    ALuint hrtf_src, pan_src, st_src, b1, b2, b3;
    DWORD t0, next_switch, next_log;
    uint32_t k = 0, minute = 0, r0 = apu_gp_debug_code_restarts(), peak_load = 0;
    ALXBOXSTATS s0;

    logf_("-- 7 listening, 5 min: HRTF source circling, a panned one, a stereo one; settings change every 15 s\n");
    alXboxOutputCallback(NULL, NULL);    /* back to zero-copy output */
    alEnable(AL_XBOX_HRTF);
    b1 = make_mono_loop(111u);
    b2 = make_mono_loop(999u);
    b3 = make_stereo_loop();
    hrtf_src = make_source(b1);
    pan_src = make_source(b2);
    st_src = make_source(b3);
    alSourcei(hrtf_src, AL_LOOPING, AL_TRUE);
    alSourcei(pan_src, AL_LOOPING, AL_TRUE);
    alSourcei(st_src, AL_LOOPING, AL_TRUE);
    send_to(hrtf_src, 0, s_slot);
    send_to(pan_src, 0, s_slot);
    send_to(st_src, 1, s_slot);
    alSourcef(st_src, AL_GAIN, 0.8f);
    alSourcePlay(hrtf_src);
    run_for(660);                        /* offset the three loops */
    alSourcePlay(pan_src);
    run_for(330);
    alSourcePlay(st_src);

    memset(&s0, 0, sizeof(s0));
    s0.size = sizeof(s0);
    alXboxGetStats(&s0);
    status(AL_XBOX_GP_LOAD_PEAK);
    t0 = GetTickCount();
    next_switch = 0;
    next_log = 60000u;
    for (;;) {
        DWORD t = GetTickCount() - t0;
        float a = (float)t * 0.0006f;                       /* one turn in ~10 s */
        float pan = 0.5f + 0.5f * sinf((float)t * 0.0011f);
        ALfloat g[2];
        alSource3f(hrtf_src, AL_POSITION, 2.0f * sinf(a), 0.0f, -2.0f * cosf(a));
        g[0] = cosf(pan * 1.5707963f);
        g[1] = sinf(pan * 1.5707963f);
        alSourcefv(pan_src, AL_XBOX_DIRECT_GAINS, g);
        if (t >= next_switch) {
            const setting_t *s = &k_settings[k++ % (sizeof(k_settings) / sizeof(k_settings[0]))];
            apply(s);
            debugPrint("effect: %s\n", s->name);
            next_switch += 15000u;
        }
        if (t >= next_log && minute < 5u) {
            ALXBOXSTATS s;
            uint32_t pk = (uint32_t)status(AL_XBOX_GP_LOAD_PEAK);
            if (pk > peak_load) peak_load = pk;
            memset(&s, 0, sizeof(s));
            s.size = sizeof(s);
            alXboxGetStats(&s);
            minute++;
            logf_("   %lu min: underruns %lu padded %lu slips %lu, zero-copy %lu, lead %lu..%lu; GP load peak %lu %%, "
                  "kernel loads by restart %lu; AL error 0x%x\n", (unsigned long)minute,
                  (unsigned long)(s.ac97_underruns - s0.ac97_underruns), (unsigned long)(s.ac97_padded - s0.ac97_padded),
                  (unsigned long)(s.output_slips - s0.output_slips), (unsigned long)s.output_zero_copy,
                  (unsigned long)s.output_lead_min, (unsigned long)s.output_lead_max, (unsigned long)pk,
                  (unsigned long)(apu_gp_debug_code_restarts() - r0), (unsigned)alGetError());
            if (minute == 5u) {
                logf_("== done: 5 min, GP load peak %lu %%; playing on\n", (unsigned long)peak_load);
            }
            save_log();
            next_log += 60000u;
        }
        alXboxUpdateVoices();
        Sleep(16);
    }
}

/* ---------------------------------------------------------------- main */

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;
    ALCint sends = 0, attrs[] = { ALC_MAX_AUXILIARY_SENDS, 2, 0 };

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("openal_efx: ALC_EXT_EFX and AL_XBOX_gp_program\n");
    dev = alcOpenDevice(NULL);
    if (!dev) {
        logf_("alcOpenDevice failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    ctx = alcCreateContext(dev, attrs);
    if (ctx && !apu_ac97_active()) {
        /* xemu: the library left the output to the emulated front end, where bin 2 is a
         * surround bin and the GP effects stay off. Start again the way a console does */
        logf_("AC97 inactive (front end works: xemu?), reopening with APU_VP_DBG_NO_FE_PROBE\n");
        alcDestroyContext(ctx);
        alcCloseDevice(dev);
        apu_vp_debug_set_flags(APU_VP_DBG_NO_FE_PROBE);
        dev = alcOpenDevice(NULL);
        ctx = dev ? alcCreateContext(dev, attrs) : NULL;
    }
    alcMakeContextCurrent(ctx);
    alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
    logf_("ALC_EXT_EFX %s, %d sends; entry points %s; GP program %s\n",
          alcIsExtensionPresent(dev, ALC_EXT_EFX_NAME) ? "present" : "MISSING", (int)sends,
          load_efx() ? "all found" : "MISSING", apu_gp_program_loaded() ? "loaded" : "NOT LOADED");
    save_log();
    if (!p_alXboxEffectProgram) {
        for (;;) Sleep(1000);
    }

    alDisable(AL_XBOX_HRTF);            /* clean clicks: the measurements use panned voices */
    s_click_buf = make_click();
    s_src_a = make_source(s_click_buf);
    s_src_b = make_source(s_click_buf);
    p_alGenEffects(1, &s_reverb);
    p_alGenEffects(1, &s_echo);
    p_alGenAuxiliaryEffectSlots(1, &s_slot);
    alXboxOutputCallback(cap_cb, NULL);  /* copy mode while measuring */
    run_for(500);

    phase_reverb();
    phase_echo();
    phase_back();
    phase_lowpass();
    phase_sync();
    phase_listen();
    return 0;
}
