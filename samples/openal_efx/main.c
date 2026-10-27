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
 *  6c  AL_EFFECT_ECHO (library kernel): the two taps and the spread, then feedback
 *  6d  air absorption and cone outer GAINHF: the 5 kHz level
 *  6e  a stereo source at a distance: AL_AUTO_SOFT (no attenuation), AL_TRUE
 *  6f  ALC_SOFT_pause_device: silence while paused, the source still playing
 *  6g  AL_EFFECT_CHORUS and AL_EFFECT_FLANGER (library kernel): fixed taps, feedback, the LFO's load
 *  6h  same-frame starts and timed parameters: two streams, one per ear, started
 *      by alSourcePlayv (and by two alSourcePlay for comparison); a timed echo
 *      wet gain (alXboxEffectSlotAtSample) against a marker click: how many
 *      samples after the marker the change lands
 *  6i  the on-chip effects (phase9-07 kernels), the dry path muted, read from
 *      the final mix by Goertzel, PASS / FAIL against the host simulator
 *      (expected_6i.py, tolerances in the log): ring modulator (1 kHz x 300 Hz:
 *      700 / 1300 Hz up, 1 kHz gone), distortion (harmonics of a sine against
 *      the dry voice's), equalizer (each band +12 dB against flat at 100 Hz,
 *      500 Hz, 3 kHz, 12 kHz), compressor (loud and quiet, on against off),
 *      autowah (a burst: where the peak sits, loud and quiet)
 *  6j  four slots at once (reverb, equalizer, distortion, autowah): GP load
 *      peak, each slot heard on its own bins, no GP restart, and a second
 *      reverb past the budget refused with AL_INVALID_OPERATION
 *   7  listening: an HRTF source circling, a panned one and a stereo one, all
 *      sending to the slot; the effect changes every 15 s (reverb, echo,
 *      chorus, flanger, ring modulator, distortion, equalizer, compressor,
 *      autowah); output health logged every minute, 5 minutes, then XReboot()
 *
 * First on hardware here (6i, 6j, 7): rnd in the ring modulator's,
 * distortion's and equalizer's biquads and in the autowah; mpyr and macr in
 * the compressor (1/env) and the autowah (its coefficients). The kernels that
 * ran on a console before (reverb, echo, loop) use neither, and the echo and
 * chorus library kernels (6c, 6g) neither; the simulator rounds them
 * convergently, the mode the GP starts in.
 *
 * GP load (AL_XBOX_GP_LOAD / _PEAK) is logged with each effect.
 * Console safety: every log line is written through to E:\openal_efx.txt as
 * it is logged (FILE_FLAG_WRITE_THROUGH, so a reset keeps it), each section
 * starts with a "step:" line, and a watchdog thread (watchdog.h) resets the
 * console through the SMC after 20 s without a log line or a listening-loop
 * pass, logging the last step.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <hal/xbox.h>
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

#define WATCHDOG_LOG LOGFILE
#include "watchdog.h"

/* ---------------------------------------------------------------- log */

static char s_log[48 * 1024];   /* a copy in RAM, for a debugger (xemu) */
static size_t s_log_len;
static int s_log_file;          /* 0 not opened yet, 1 written, -1 no E: */
static char s_last_line[96];    /* the watchdog names it */

static void mount_e(void) {
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
}

/* Appends one line to E:\openal_efx.txt, written through, so a reset or a power-off keeps every line
 * already logged (the first line starts the file afresh) */
static void log_file(const char *line, int n) {
    HANDLE h;
    DWORD written;
    if (s_log_file < 0) return;
    h = CreateFileA(LOGFILE, GENERIC_WRITE, 0, NULL, s_log_file ? OPEN_ALWAYS : CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (!s_log_file) s_log_file = -1;
        return;
    }
    s_log_file = 1;
    SetFilePointer(h, 0, NULL, FILE_END);
    WriteFile(h, line, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

/* Screen, RAM copy, E: (written through), and a watchdog heartbeat naming the line */
static void logf_(const char *fmt, ...) {
    char line[320];
    va_list ap;
    int n;
    size_t k;
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
    log_file(line, n);
    for (k = 0; k + 1 < sizeof(s_last_line) && line[k] && line[k] != '\n'; k++) s_last_line[k] = line[k];
    s_last_line[k] = 0;
    watchdog_kick(s_last_line);
}

static void step(const char *what) {
    logf_("step: %s\n", what);
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

    logf_("-- 3 live parameters: delay 2400, wet 0.5 (re-attach, same image)\n");
    r0 = apu_gp_debug_code_restarts();
    echo_params(2400, 0.0f, 0.5f);
    attach(s_echo);
    logf_("   %s; mailbox acknowledged after %lu frames\n",
          apu_gp_debug_code_restarts() == r0 ? "no reload" : "RELOADED", (unsigned long)apu_gp_debug_mailbox_frames(BAR0));
    run_for(100);
    n = capture(800, play_a);
    log_peaks("delay 2400", n);

    logf_("-- 4 feedback 0.5, delay 4800 (expected +4800, +9600, +14400, 6 dB apart)\n");
    echo_params(4800, 0.5f, 1.0f);
    attach(s_echo);
    run_for(100);
    n = capture(1300, play_a);
    log_peaks("feedback", n);

    logf_("-- 5 two sources into one slot: A on send 0, B on send 1 250 ms later, delay 1000\n");
    echo_params(1000, 0.0f, 1.0f);
    attach(s_echo);
    send_to(s_src_b, 1, s_slot);
    run_for(500);
    n = capture(1000, play_a_then_b);
    log_peaks("A + B", n);
}

static void phase_back(void) {
    uint32_t r0 = apu_gp_debug_code_restarts();
    logf_("-- 6 back to the reverb\n");
    attach(s_reverb);
    run_for(100);
    logf_("   AL error 0x%x; %s; AL_XBOX_REVERB_ACTIVE %d\n", (unsigned)alGetError(),
          apu_gp_debug_code_restarts() == r0 ? "code through the P window" : "GP RESTARTED", status(AL_XBOX_REVERB_ACTIVE));
    log_load("reverb again");
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
}

/* ---------------------------------------------------------------- 6c-6f: phase 9 */

static ALCdevice *s_dev;

static void phase_efx_echo(void) {
    ALuint fx = 0;
    uint32_t r0 = apu_gp_debug_code_restarts(), n;
    logf_("-- 6c AL_EFFECT_ECHO (library kernel): DELAY 0.05, LRDELAY 0.05, SPREAD -1, no feedback\n");
    logf_("   (expected: L +2400 only, R +4800 only, against the centred dry click)\n");
    p_alGenEffects(1, &fx);
    p_alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    p_alEffectf(fx, AL_ECHO_DELAY, 0.05f);
    p_alEffectf(fx, AL_ECHO_LRDELAY, 0.05f);
    p_alEffectf(fx, AL_ECHO_FEEDBACK, 0.0f);
    p_alEffectf(fx, AL_ECHO_DAMPING, 0.0f);
    p_alEffectf(fx, AL_ECHO_SPREAD, -1.0f);
    attach(fx);
    run_for(100);
    logf_("   attach: AL error 0x%x; %s\n", (unsigned)alGetError(),
          apu_gp_debug_code_restarts() == r0 ? "code through the P window" : "GP RESTARTED");
    log_load("efx echo");
    n = capture(800, play_a);
    log_peaks("efx echo", n);
    logf_("   FEEDBACK 0.5, DAMPING 0.5, SPREAD 0 (expected: both ears +2400, +4800 at 0 dB, then one every 2400, each darker)\n");
    p_alEffectf(fx, AL_ECHO_FEEDBACK, 0.5f);
    p_alEffectf(fx, AL_ECHO_DAMPING, 0.5f);
    p_alEffectf(fx, AL_ECHO_SPREAD, 0.0f);
    attach(fx);
    run_for(100);
    n = capture(1300, play_a);
    log_peaks("efx echo fb", n);
    attach(s_reverb);
    p_alDeleteEffects(1, &fx);
    run_for(100);
}

static void log_tones(const char *what, double r500, double r5k, ALuint src, const char *expect) {
    double p500, p5k;
    (void)src;
    measure_tones(&p500, &p5k);
    logf_("   %s: 5 kHz %s dB, 500 Hz %s dB (%s)\n", what, dbs(db10(sqrt(p5k / r5k))),
          dbs(db10(sqrt(p500 / r500))), expect);
}

static void phase_hf_auto(void) {
    ALuint b = make_tones(), src;
    double r500, r5k;
    logf_("-- 6d air absorption and cone outer GAINHF on the voice lowpass (21 units ahead, reference 1)\n");
    src = make_source(b);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -21.0f);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    measure_tones(&r500, &r5k);
    alSourcef(src, AL_AIR_ABSORPTION_FACTOR, 10.0f);
    log_tones("AIR_ABSORPTION_FACTOR 10 (20 m)", r500, r5k, src, "expected 5 kHz -10.0");
    alSourcef(src, AL_AIR_ABSORPTION_FACTOR, 0.0f);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, -1.0f);   /* pointing away from the listener */
    alSourcef(src, AL_CONE_INNER_ANGLE, 30.0f);
    alSourcef(src, AL_CONE_OUTER_ANGLE, 90.0f);
    alSourcef(src, AL_CONE_OUTER_GAIN, 1.0f);
    alSourcef(src, AL_CONE_OUTER_GAINHF, 0.25f);
    log_tones("CONE_OUTER_GAINHF 0.25, outside the cone", r500, r5k, src, "expected 5 kHz -12.0");
    alSourcei(src, AL_DIRECT_FILTER_GAINHF_AUTO, AL_FALSE);
    log_tones("the same, GAINHF_AUTO off", r500, r5k, src, "expected 0.0 both");
    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &b);
    logf_("   AL error 0x%x\n", (unsigned)alGetError());
}

static void phase_spatialize(void) {
    int16_t *pcm = malloc(2u * TONE_FRAMES * sizeof(int16_t));
    ALuint b = 0, src;
    double r, p;
    uint32_t i, n;
    logf_("-- 6e stereo and AL_SOFT_source_spatialize: a stereo 500 Hz tone 10 units ahead\n");
    for (i = 0; i < TONE_FRAMES; i++) {
        pcm[2u * i] = pcm[2u * i + 1u] = (int16_t)(8000.0f * sinf(TWO_PI * 500.0f * (float)i / 48000.0f));
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_STEREO16, pcm, (ALsizei)(2u * TONE_FRAMES * sizeof(int16_t)), 48000);
    free(pcm);
    src = make_source(b);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    run_for(150);
    n = capture(400, NULL);
    r = tone_power(n, 500.0);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -10.0f);
    run_for(150);
    n = capture(400, NULL);
    p = tone_power(n, 500.0);
    logf_("   AL_AUTO_SOFT at 10 units: %s dB (expected 0.0: stereo is not spatialized)\n", dbs(db10(sqrt(p / r))));
    alSourcei(src, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
    run_for(150);
    n = capture(400, NULL);
    p = tone_power(n, 500.0);
    logf_("   AL_TRUE at 10 units: %s dB (expected -20.0)\n", dbs(db10(sqrt(p / r))));

    logf_("-- 6f ALC_SOFT_pause_device\n");
    alSourcei(src, AL_SOURCE_SPATIALIZE_SOFT, AL_AUTO_SOFT);
    alcDevicePauseSOFT(s_dev);
    run_for(150);
    n = capture(400, NULL);
    p = tone_power(n, 500.0);
    {
        ALint st = 0;
        alGetSourcei(src, AL_SOURCE_STATE, &st);
        logf_("   paused: %s dB (expected silence), source %s\n", dbs(db10(sqrt(p / r))),
              st == AL_PLAYING ? "AL_PLAYING" : "NOT PLAYING");
    }
    alcDeviceResumeSOFT(s_dev);
    run_for(150);
    n = capture(400, NULL);
    p = tone_power(n, 500.0);
    logf_("   resumed: %s dB (expected 0.0); ALC error 0x%x, AL error 0x%x\n", dbs(db10(sqrt(p / r))),
          (unsigned)alcGetError(s_dev), (unsigned)alGetError());
    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &b);
}

static void phase_efx_chorus(void) {
    ALuint fx = 0;
    uint32_t r0 = apu_gp_debug_code_restarts(), n;
    logf_("-- 6g AL_EFFECT_CHORUS (library kernel): DELAY 0.01, DEPTH 0 (a fixed tap), no feedback\n");
    logf_("   (expected: both ears +480 at +3.0 dB against the centred dry click)\n");
    p_alGenEffects(1, &fx);
    p_alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    p_alEffectf(fx, AL_CHORUS_DELAY, 0.01f);
    p_alEffectf(fx, AL_CHORUS_DEPTH, 0.0f);
    p_alEffectf(fx, AL_CHORUS_FEEDBACK, 0.0f);
    attach(fx);
    run_for(100);
    logf_("   attach: AL error 0x%x; %s\n", (unsigned)alGetError(),
          apu_gp_debug_code_restarts() == r0 ? "code through the P window" : "GP RESTARTED");
    n = capture(800, play_a);
    log_peaks("chorus", n);
    logf_("   AL_EFFECT_FLANGER: DELAY 0.004, DEPTH 0, FEEDBACK 0.5 (expected: +192 at +3.0, then every 192, 6 dB apart)\n");
    p_alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_FLANGER);
    p_alEffectf(fx, AL_FLANGER_DELAY, 0.004f);
    p_alEffectf(fx, AL_FLANGER_DEPTH, 0.0f);
    p_alEffectf(fx, AL_FLANGER_FEEDBACK, 0.5f);
    attach(fx);
    run_for(100);
    n = capture(800, play_a);
    log_peaks("flanger", n);
    logf_("   the chorus's defaults (triangle, 1.1 Hz, DEPTH 0.1, 16 ms, FEEDBACK 0.25), then sine:\n");
    p_alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    attach(fx);
    run_for(100);
    log_load("chorus triangle");
    p_alEffecti(fx, AL_CHORUS_WAVEFORM, AL_CHORUS_WAVEFORM_SINUSOID);
    attach(fx);
    run_for(100);
    log_load("chorus sine");
    logf_("   AL error 0x%x\n", (unsigned)alGetError());
    attach(s_reverb);
    p_alDeleteEffects(1, &fx);
}

/* ---------------------------------------------------------------- 6h: same frame, timed */

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
    logf_("-- 6h same-frame starts: two streams with one click each, left and right (expected: L = R)\n");
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

    logf_("-- 6h timed parameters: echo delay 1000, wet 0 -> 1 at stream frame %u (alXboxEffectSlotAtSample)\n",
          (unsigned)SYNC_MARK);
    logf_("   (a marker click at that frame on L, peak 1 later; a click every %u frames on R's stream, only through\n"
          "    the echo, whose echoes fall 8 after a multiple of 16; the wet gain scales the echo's output, so the\n"
          "    first echo shows where the change lands: +7 after the marker; -25 is a frame early, +39 a frame late)\n",
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
        if (find_peaks(n, 1, pr, 1)) {
            uint32_t k, mark = 0;
            int top = 0;
            for (k = 0; k < n; k++) {                   /* the marker: the left ear's loudest sample */
                int a = abs(s_cap[2u * k]);
                if (a > top) {
                    top = a;
                    mark = k;
                }
            }
            logf_("   marker at %lu, first echo at %lu: %+ld (expected +7)\n", (unsigned long)mark,
                  (unsigned long)pr[0].pos, (long)pr[0].pos - (long)mark);
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
}

/* ---------------------------------------------------------------- 6i-6j: on-chip effects, several slots */

/* Every expectation below comes from samples/openal_efx/expected_6i.py: the library's kernel run in
 * the host simulator (lib/openal/tools/dsp56sim.py; within 3.3 LSB of 16 bits of OpenAL Soft's
 * algorithm in double precision, tests/test_gp_onchip*.py) on this sample's sound, measured the same
 * way. Levels are quoted against the dry, centre-panned ear (0.7071 of the voice), which the sample
 * measures first. Tolerances: 0.5 dB for the linear checks (the VP's volume steps, the codec path);
 * the checks that depend on the level at the kernel (distortion, compressor, autowah) take the
 * simulator's spread for an input 0.5 dB louder or quieter, plus 0.5 dB (the autowah's peak: 6 %
 * spread, 12 % allowed). xemu: its GP runs late and the captured mix stays silent, so none of
 * these levels can be checked there; only the flow, the attaches and the budget refusal can. */

#define DRY_PCM    8000.0
#define EQ_GAIN12  3.981f            /* +12 dB */

static const double k_rm_side = -3.0, k_rm_tol = 0.5, k_rm_carrier_max = -40.0;   /* the carrier: -111 */
static const double k_di_h1 = 7.7, k_di_h3 = -6.6, k_di_h5 = -12.2, k_di_tol = 0.6, k_di_over_dry = 30.0;
static const double k_eq_flat = 3.0, k_eq_tol = 0.5;
static const double k_eq_gain[4][4] = {                  /* band +12 dB: 100 Hz, 500 Hz, 3 kHz, 12 kHz against flat */
    { 10.3, 1.1, 0.0, 0.0 },        /* low shelf, cutoff 200 Hz */
    { 0.3, 12.0, 0.2, 0.0 },        /* mid1, 500 Hz */
    { 0.0, 0.2, 12.0, 0.4 },        /* mid2, 3 kHz */
    { 0.0, 0.0, 1.6, 10.9 },        /* high shelf, cutoff 6 kHz */
};
static const double k_co_loud = 2.3, k_co_quiet = 6.0, k_co_on = 16.0, k_co_off = 19.7;
static const double k_co_tol = 0.5, k_co_tol_level = 1.0;   /* the loud level: 1.8 .. 2.8 for +-0.5 dB in */
static const double k_aw_loud = 2312.0, k_aw_quiet = 499.0, k_aw_ratio = 4.78;   /* 2192..2451, 482..506, 4.70..5.01 */
static const double k_aw_tol_hz = 0.12, k_aw_tol_ratio = 0.45;                  /* a share of the frequency; the ratio */

static LPALGENFILTERS p_alGenFilters;
static LPALFILTERI p_alFilteri;
static LPALFILTERF p_alFilterf;
static LPALAUXILIARYEFFECTSLOTF p_alAuxiliaryEffectSlotf;
static LPALGETAUXILIARYEFFECTSLOTI p_alGetAuxiliaryEffectSloti;
static LPALDELETEAUXILIARYEFFECTSLOTS p_alDeleteAuxiliaryEffectSlots;

static ALuint s_mute;               /* AL_FILTER_LOWPASS with GAIN 0: the dry path off, the sends untouched */
static ALuint s_b1k;                /* 1 kHz, pcm 8000 */
static double s_dry_unit = 0.70711; /* the dry ear's amplitude for a voice amplitude of 1 */
static double s_dry_h3 = 0.0;       /* the dry voice's own third harmonic against its fundamental, dB */
static int s_checks, s_fails;

static int tenths(double db) {
    return (int)floor(db * 10.0 + 0.5);
}

static double dbd(double ratio) {
    return ratio > 0.0 ? 20.0 * log10(ratio) : -999.0;
}

static void verdict(bool ok) {
    s_checks++;
    if (!ok) s_fails++;
}

/* "what: X dB (expected E +- T) PASS" */
static void check_db(const char *what, double got, double expect, double tol) {
    bool ok = fabs(got - expect) <= tol;
    verdict(ok);
    logf_("   %s: %s dB (expected %s +- %s) %s\n", what, dbs(tenths(got)), dbs(tenths(expect)), dbs(tenths(tol)),
          ok ? "PASS" : "FAIL");
}

/* "what: X dB (expected at most / at least L) PASS" */
static void check_bound(const char *what, double got, double limit, bool at_most) {
    bool ok = at_most ? got <= limit : got >= limit;
    verdict(ok);
    logf_("   %s: %s dB (expected at %s %s) %s\n", what, dbs(tenths(got)), at_most ? "most" : "least",
          dbs(tenths(limit)), ok ? "PASS" : "FAIL");
}

static void check_hz(const char *what, double got, double expect, double tol) {
    bool ok = fabs(got - expect) <= tol;
    verdict(ok);
    logf_("   %s: %ld Hz (expected %ld +- %ld) %s\n", what, (long)floor(got + 0.5), (long)floor(expect + 0.5),
          (long)floor(tol + 0.5), ok ? "PASS" : "FAIL");
}

/* The left channel's amplitude at `hz` over [from, from + count): Goertzel over 960-sample blocks */
static double amp_at(uint32_t from, uint32_t count, double hz) {
    double c = 2.0 * cos(6.283185307179586 * hz / 48000.0), acc = 0.0;
    uint32_t blk, i, blocks = count / 960u;
    for (blk = 0; blk < blocks; blk++) {
        const int16_t *p = s_cap + 2u * (from + blk * 960u);
        double s1 = 0.0, s2 = 0.0;
        for (i = 0; i < 960u; i++) {
            double s = p[2u * i] + c * s1 - s2;
            s2 = s1;
            s1 = s;
        }
        acc += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return blocks ? 2.0 * sqrt(acc / blocks) / 960.0 : 0.0;
}

/* 1 s of sines at `hz` (multiples of 50 Hz: whole cycles in each block and in the loop), `amp` each */
static ALuint make_sines(const double *hz, int count, double amp) {
    int16_t *pcm = malloc(TONE_FRAMES * sizeof(int16_t));
    ALuint b = 0;
    uint32_t i;
    int k;
    for (i = 0; i < TONE_FRAMES; i++) {
        double v = 0.0;
        for (k = 0; k < count; k++) v += amp * sin(6.283185307179586 * hz[k] * (double)i / 48000.0);
        pcm[i] = (int16_t)floor(v + 0.5);
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)(TONE_FRAMES * sizeof(int16_t)), 48000);
    free(pcm);
    return b;
}

/* A looping voice heard only through its send 0 into the slot */
static ALuint wet_source(ALuint buf, ALuint slot) {
    ALuint s = make_source(buf);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcei(s, AL_DIRECT_FILTER, (ALint)s_mute);
    send_to(s, 0, slot);
    return s;
}

static void drop_source(ALuint s) {
    alSourceStop(s);
    alDeleteSources(1, &s);
}

static uint32_t settle_capture(DWORD settle, DWORD ms) {
    run_for(settle);
    return capture(ms, NULL);
}

/* The GP frames the loop counted since `c0` (X:CNT, 24 bits): about 150 in 100 ms while it runs. A
 * kernel switch that leaves the GP executing anything else stops the count (xemu aborted on an
 * invalid opcode right after a switch in some runs) */
static uint32_t gp_frames_since(uint32_t c0) {
    return (gpx(GP_LOOP_CNT) - c0) & 0xFFFFFFu;
}

static void attach_fx(ALuint slot, ALuint fx, const char *name) {
    uint32_t r0 = apu_gp_debug_code_restarts(), c0;
    ALenum e;
    p_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)fx);
    e = alGetError();
    c0 = gpx(GP_LOOP_CNT);
    run_for(100);
    logf_("   %s on slot %u: AL error 0x%x, %s, GP frames +%lu in 100 ms\n", name, (unsigned)slot, (unsigned)e,
          apu_gp_debug_code_restarts() == r0 ? "code through the P window" : "GP RESTARTED",
          (unsigned long)gp_frames_since(c0));
}

static ALuint new_effect(ALenum type) {
    ALuint fx = 0;
    p_alGenEffects(1, &fx);
    p_alEffecti(fx, AL_EFFECT_TYPE, type);
    return fx;
}

static bool onchip_init(void) {
    static const double hz[1] = { 1000.0 };
    p_alGenFilters = (LPALGENFILTERS)alGetProcAddress("alGenFilters");
    p_alFilteri = (LPALFILTERI)alGetProcAddress("alFilteri");
    p_alFilterf = (LPALFILTERF)alGetProcAddress("alFilterf");
    p_alAuxiliaryEffectSlotf = (LPALAUXILIARYEFFECTSLOTF)alGetProcAddress("alAuxiliaryEffectSlotf");
    p_alGetAuxiliaryEffectSloti = (LPALGETAUXILIARYEFFECTSLOTI)alGetProcAddress("alGetAuxiliaryEffectSloti");
    p_alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
    if (!p_alGenFilters || !p_alFilteri || !p_alFilterf || !p_alAuxiliaryEffectSlotf || !p_alGetAuxiliaryEffectSloti ||
        !p_alDeleteAuxiliaryEffectSlots) {
        return false;
    }
    p_alGenFilters(1, &s_mute);
    p_alFilteri(s_mute, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    p_alFilterf(s_mute, AL_LOWPASS_GAIN, 0.0f);
    s_b1k = make_sines(hz, 1, DRY_PCM);
    return true;
}

static void phase_onchip_ref(void) {
    ALuint src = make_source(s_b1k);
    uint32_t n;
    double a1;
    logf_("-- 6i on-chip effects (phase9-07 kernels): dry reference, 1 kHz pcm 8000, no send\n");
    logf_("   first on hardware here: rnd (the ring modulator's, distortion's and equalizer's biquads, the autowah),\n"
          "   mpyr / macr (the compressor's 1/env, the autowah's coefficients); the simulator rounds them\n"
          "   convergently, the mode the GP starts in\n");
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    n = settle_capture(300, 400);
    a1 = amp_at(0, n, 1000.0);
    s_dry_h3 = dbd(amp_at(0, n, 3000.0) / (a1 > 0.0 ? a1 : 1.0));
    logf_("   dry ear %ld (expected 0.7071 x 8000 = 5657), its 3 kHz %s dB against 1 kHz\n", (long)floor(a1 + 0.5),
          dbs(tenths(s_dry_h3)));
    if (a1 > 1000.0) {
        s_dry_unit = a1 / DRY_PCM;
    } else {
        logf_("   (no dry level: wet levels quoted against the nominal 0.7071)\n");
    }
    drop_source(src);
}

static void phase_ringmod(void) {
    ALuint fx = new_effect(AL_EFFECT_RING_MODULATOR), src;
    uint32_t n;
    double ref = s_dry_unit * DRY_PCM;
    logf_("-- 6i ring modulator: 1 kHz pcm 8000 x a 300 Hz sine, HIGHPASS_CUTOFF 0, dry path muted\n");
    p_alEffectf(fx, AL_RING_MODULATOR_FREQUENCY, 300.0f);
    p_alEffectf(fx, AL_RING_MODULATOR_HIGHPASS_CUTOFF, 0.0f);
    p_alEffecti(fx, AL_RING_MODULATOR_WAVEFORM, AL_RING_MODULATOR_SINUSOID);
    attach_fx(s_slot, fx, "ring modulator");
    src = wet_source(s_b1k, s_slot);
    alSourcePlay(src);
    log_load("ring modulator");
    n = settle_capture(300, 400);
    check_db("700 Hz against the dry ear", dbd(amp_at(0, n, 700.0) / ref), k_rm_side, k_rm_tol);
    check_db("1300 Hz against the dry ear", dbd(amp_at(0, n, 1300.0) / ref), k_rm_side, k_rm_tol);
    {
        double side = amp_at(0, n, 700.0);   /* no sideband: 0 dB, a FAIL, not a silent PASS */
        check_bound("1 kHz (the carrier) against 700 Hz", side > 1.0 ? dbd(amp_at(0, n, 1000.0) / side) : 0.0,
                    k_rm_carrier_max, true);
    }
    drop_source(src);
    p_alDeleteEffects(1, &fx);
}

static void phase_distortion(void) {
    ALuint fx = new_effect(AL_EFFECT_DISTORTION), src;
    uint32_t n;
    double h1, h3, h5;
    logf_("-- 6i distortion: 1 kHz pcm 8000, EDGE 0.5, GAIN 0.5, LOWPASS 8 kHz, EQCENTER 3 kHz, EQBANDWIDTH 6 kHz\n");
    p_alEffectf(fx, AL_DISTORTION_EDGE, 0.5f);
    p_alEffectf(fx, AL_DISTORTION_GAIN, 0.5f);
    p_alEffectf(fx, AL_DISTORTION_LOWPASS_CUTOFF, 8000.0f);
    p_alEffectf(fx, AL_DISTORTION_EQCENTER, 3000.0f);
    p_alEffectf(fx, AL_DISTORTION_EQBANDWIDTH, 6000.0f);
    attach_fx(s_slot, fx, "distortion");
    src = wet_source(s_b1k, s_slot);
    alSourcePlay(src);
    log_load("distortion");
    n = settle_capture(300, 400);
    h1 = amp_at(0, n, 1000.0);
    h3 = dbd(amp_at(0, n, 3000.0) / (h1 > 0.0 ? h1 : 1.0));
    h5 = dbd(amp_at(0, n, 5000.0) / (h1 > 0.0 ? h1 : 1.0));
    check_db("1 kHz against the dry ear", dbd(h1 / (s_dry_unit * DRY_PCM)), k_di_h1, k_di_tol);
    check_db("3 kHz against 1 kHz", h3, k_di_h3, k_di_tol);
    check_db("5 kHz against 1 kHz", h5, k_di_h5, k_di_tol);
    check_bound("3 kHz against 1 kHz, above the dry voice's", h3 - s_dry_h3, k_di_over_dry, false);
    drop_source(src);
    p_alDeleteEffects(1, &fx);
}

static void phase_equalizer(void) {
    static const double hz[4] = { 100.0, 500.0, 3000.0, 12000.0 };
    static const ALenum band[4] = { AL_EQUALIZER_LOW_GAIN, AL_EQUALIZER_MID1_GAIN, AL_EQUALIZER_MID2_GAIN,
                                    AL_EQUALIZER_HIGH_GAIN };
    static const char *const name[4] = { "low (cutoff 200 Hz)", "mid1 (500 Hz)", "mid2 (3 kHz)", "high (cutoff 6 kHz)" };
    ALuint fx = new_effect(AL_EFFECT_EQUALIZER), b = make_sines(hz, 4, 3000.0), src;
    double flat[4];
    uint32_t n;
    int i, k;
    logf_("-- 6i equalizer: 100 Hz + 500 Hz + 3 kHz + 12 kHz, pcm 3000 each; each band at +12 dB against all at 0 dB\n");
    attach_fx(s_slot, fx, "equalizer (flat)");
    src = wet_source(b, s_slot);
    alSourcePlay(src);
    n = settle_capture(300, 400);
    for (k = 0; k < 4; k++) {
        char what[48];
        flat[k] = amp_at(0, n, hz[k]);
        snprintf(what, sizeof(what), "flat %ld Hz against the dry ear", (long)hz[k]);
        check_db(what, dbd(flat[k] / (s_dry_unit * 3000.0)), k_eq_flat, k_eq_tol);
        if (flat[k] <= 0.0) flat[k] = 1.0;
    }
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) p_alEffectf(fx, band[k], k == i ? EQ_GAIN12 : 1.0f);
        attach_fx(s_slot, fx, name[i]);
        if (i == 3) log_load("equalizer, one band");
        n = settle_capture(300, 400);
        for (k = 0; k < 4; k++) {
            char what[64];
            snprintf(what, sizeof(what), "%s +12: %ld Hz against flat", i == 0 ? "low" : i == 1 ? "mid1" : i == 2 ? "mid2" : "high",
                     (long)hz[k]);
            check_db(what, dbd(amp_at(0, n, hz[k]) / flat[k]), k_eq_gain[i][k], k_eq_tol);
        }
    }
    drop_source(src);
    alDeleteBuffers(1, &b);
    p_alDeleteEffects(1, &fx);
}

static void phase_compressor(void) {
    static const double hz[1] = { 1000.0 };
    ALuint fx = new_effect(AL_EFFECT_COMPRESSOR), bl = make_sines(hz, 1, 29000.0), bq = make_sines(hz, 1, 3000.0);
    ALuint loud, quiet;
    double lon, loff, qon, qoff;
    logf_("-- 6i compressor: 1 kHz at pcm 29000 (loud) and 3000 (quiet), ONOFF 1 against 0, AL_EFFECTSLOT_GAIN 0.5\n");
    p_alAuxiliaryEffectSlotf(s_slot, AL_EFFECTSLOT_GAIN, 0.5f);   /* the output's headroom: the gain is up to 2 */
    p_alEffecti(fx, AL_COMPRESSOR_ONOFF, AL_TRUE);
    attach_fx(s_slot, fx, "compressor");
    loud = wet_source(bl, s_slot);
    quiet = wet_source(bq, s_slot);
    alSourcePlay(loud);
    log_load("compressor");
    lon = amp_at(0, settle_capture(600, 400), 1000.0);
    p_alEffecti(fx, AL_COMPRESSOR_ONOFF, AL_FALSE);
    attach_fx(s_slot, fx, "compressor off");
    loff = amp_at(0, settle_capture(600, 400), 1000.0);
    alSourceStop(loud);
    alSourcePlay(quiet);
    qoff = amp_at(0, settle_capture(600, 400), 1000.0);
    p_alEffecti(fx, AL_COMPRESSOR_ONOFF, AL_TRUE);
    attach_fx(s_slot, fx, "compressor on");
    qon = amp_at(0, settle_capture(600, 400), 1000.0);
    if (loff <= 0.0) loff = 1.0;
    if (qoff <= 0.0) qoff = 1.0;
    if (qon <= 0.0) qon = 1.0;
    check_db("loud, on against off", dbd(lon / loff), k_co_loud, k_co_tol_level);
    check_db("quiet, on against off", dbd(qon / qoff), k_co_quiet, k_co_tol);
    check_db("loud against quiet, on", dbd(lon / qon), k_co_on, k_co_tol_level);
    check_db("loud against quiet, off (the input's)", dbd(loff / qoff), k_co_off, k_co_tol);
    drop_source(loud);
    drop_source(quiet);
    alDeleteBuffers(1, &bl);
    alDeleteBuffers(1, &bq);
    p_alAuxiliaryEffectSlotf(s_slot, AL_EFFECTSLOT_GAIN, 1.0f);
    p_alDeleteEffects(1, &fx);
}

#define AW_HARMONICS 48
#define AW_SEG       28800u     /* 0.6 s loud, 0.6 s quiet */

/* The autowah's input: 100 Hz .. 4.8 kHz every 100 Hz, Schroeder's phases, `loud` then `quiet` a harmonic */
static ALuint make_comb(double loud, double quiet) {
    int16_t *pcm = malloc(2u * AW_SEG * sizeof(int16_t));
    ALuint b = 0;
    uint32_t i;
    int k;
    for (i = 0; i < AW_SEG; i++) {
        double v = 0.0;
        for (k = 1; k <= AW_HARMONICS; k++) {
            v += sin(6.283185307179586 * 100.0 * k * (double)i / 48000.0 + 3.141592653589793 * k * k / AW_HARMONICS);
        }
        pcm[i] = (int16_t)floor(loud * v + 0.5);
        pcm[AW_SEG + i] = (int16_t)floor(quiet * v + 0.5);
    }
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)(2u * AW_SEG * sizeof(int16_t)), 48000);
    free(pcm);
    return b;
}

/* Where the peak sits: the loudest harmonic, refined by a parabola through its and its neighbours' dB */
static double peak_hz(uint32_t from, uint32_t count) {
    double a[AW_HARMONICS], d = 0.0;
    int k, top = 0;
    for (k = 0; k < AW_HARMONICS; k++) {
        a[k] = amp_at(from, count, 100.0 * (k + 1));
        if (a[k] > a[top]) top = k;
    }
    if (top > 0 && top < AW_HARMONICS - 1 && a[top - 1] > 0.0 && a[top + 1] > 0.0) {
        double l = dbd(a[top - 1]), m = dbd(a[top]), r = dbd(a[top + 1]);
        if (l - 2.0 * m + r != 0.0) d = 0.5 * (l - r) / (l - 2.0 * m + r);
    }
    return 100.0 * (top + 1 + d);
}

static ALuint s_play_src;

static void play_s(void) {
    alSourcePlay(s_play_src);
}

static void phase_autowah(void) {
    ALuint fx = new_effect(AL_EFFECT_AUTOWAH), b = make_comb(700.0, 140.0);
    uint32_t n, onset = 0;
    double fl, fq;
    logf_("-- 6i autowah: harmonics every 100 Hz to 4.8 kHz, pcm 700 each for 0.6 s then 140 for 0.6 s (a burst);\n"
          "   ATTACK / RELEASE 0.06, RESONANCE 100, PEAK_GAIN 0.00003: the peak follows the envelope, 20 Hz + 2480 env\n");
    p_alEffectf(fx, AL_AUTOWAH_ATTACK_TIME, 0.06f);
    p_alEffectf(fx, AL_AUTOWAH_RELEASE_TIME, 0.06f);
    p_alEffectf(fx, AL_AUTOWAH_RESONANCE, 100.0f);
    p_alEffectf(fx, AL_AUTOWAH_PEAK_GAIN, 0.00003f);
    attach_fx(s_slot, fx, "autowah");
    s_play_src = wet_source(b, s_slot);
    run_for(300);
    n = capture(1400, play_s);
    while (onset < n && abs(s_cap[2u * onset]) < 200) onset++;
    if (onset + AW_SEG + 26400u > n) {
        verdict(false);
        logf_("   onset at %lu of %lu frames: too late to measure FAIL\n", (unsigned long)onset, (unsigned long)n);
    } else {
        fl = peak_hz(onset + 12000u, 14400u);              /* 0.25 .. 0.55 s: the loud part */
        fq = peak_hz(onset + 40800u, 14400u);              /* 0.85 .. 1.15 s: the quiet part */
        logf_("   onset at frame %lu of the capture\n", (unsigned long)onset);
        check_hz("peak in the loud part", fl, k_aw_loud, k_aw_tol_hz * k_aw_loud);
        check_hz("peak in the quiet part", fq, k_aw_quiet, k_aw_tol_hz * k_aw_quiet);
        {
            double r = (fq > 20.0) ? (fl - 20.0) / (fq - 20.0) : 0.0;
            bool ok = fabs(r - k_aw_ratio) <= k_aw_tol_ratio;
            verdict(ok);
            logf_("   (peak - 20 Hz) loud against quiet: %ld.%02ld (expected %ld.%02ld +- %ld.%02ld; the input's 5) %s\n",
                  (long)floor(r), (long)floor(r * 100.0 + 0.5) % 100, (long)floor(k_aw_ratio),
                  (long)floor(k_aw_ratio * 100.0 + 0.5) % 100, (long)floor(k_aw_tol_ratio),
                  (long)floor(k_aw_tol_ratio * 100.0 + 0.5) % 100, ok ? "PASS" : "FAIL");
        }
    }
    log_load("autowah");
    drop_source(s_play_src);
    alDeleteBuffers(1, &b);
    p_alDeleteEffects(1, &fx);
    attach(s_reverb);
    run_for(100);
    logf_("   6i: %d of %d checks passed; AL error 0x%x\n", s_checks - s_fails, s_checks, (unsigned)alGetError());
}

/* ---------------------------------------------------------------- 6j: four slots at once */

static void phase_multislot(void) {
    static const char *const name[4] = { "reverb", "equalizer", "distortion", "autowah" };
    ALuint slot[4], fx[4], a, b;
    uint32_t r0, n, k;
    int c0 = s_checks, f0 = s_fails, last, peak;
    apu_ac97_stats_t s0, s1;
    double floor_db;
    ALint cur = -1;
    ALenum e;

    logf_("-- 6j four slots at once: reverb, equalizer (4 bands at +6 dB), distortion, autowah\n");
    logf_("   (simulator: about 27,500 instructions a frame, about 87 %% of a console frame at the reverb's 34 %%\n"
          "    for 10,800; budgets in lib/openal/docs/GP_KERNELS.md 1 and 10)\n");
    slot[0] = s_slot;
    p_alGenAuxiliaryEffectSlots(3, &slot[1]);
    fx[0] = s_reverb;
    fx[1] = new_effect(AL_EFFECT_EQUALIZER);
    p_alEffectf(fx[1], AL_EQUALIZER_LOW_GAIN, 2.0f);
    p_alEffectf(fx[1], AL_EQUALIZER_MID1_GAIN, 2.0f);
    p_alEffectf(fx[1], AL_EQUALIZER_MID2_GAIN, 2.0f);
    p_alEffectf(fx[1], AL_EQUALIZER_HIGH_GAIN, 2.0f);
    fx[2] = new_effect(AL_EFFECT_DISTORTION);
    p_alEffectf(fx[2], AL_DISTORTION_EDGE, 0.5f);
    p_alEffectf(fx[2], AL_DISTORTION_GAIN, 0.5f);
    p_alEffectf(fx[2], AL_DISTORTION_EQCENTER, 3000.0f);
    p_alEffectf(fx[2], AL_DISTORTION_EQBANDWIDTH, 6000.0f);
    fx[3] = new_effect(AL_EFFECT_AUTOWAH);
    p_alEffectf(fx[3], AL_AUTOWAH_RESONANCE, 100.0f);
    p_alEffectf(fx[3], AL_AUTOWAH_PEAK_GAIN, 0.00003f);
    logf_("   slots %u %u %u %u, AL error 0x%x\n", (unsigned)slot[0], (unsigned)slot[1], (unsigned)slot[2],
          (unsigned)slot[3], (unsigned)alGetError());
    r0 = apu_gp_debug_code_restarts();
    for (k = 0; k < 4; k++) {
        p_alAuxiliaryEffectSloti(slot[k], AL_EFFECTSLOT_EFFECT, (ALint)fx[k]);
        e = alGetError();
        verdict(e == AL_NO_ERROR);
        logf_("   %s on slot %u: AL error 0x%x %s\n", name[k], (unsigned)slot[k], (unsigned)e,
              e == AL_NO_ERROR ? "PASS" : "FAIL");
        run_for(100);
    }

    /* every kernel busy: A into slots 1 and 2, B into 3 and 4, dry muted */
    a = wet_source(s_b1k, slot[0]);
    send_to(a, 1, slot[1]);
    b = wet_source(s_b1k, slot[2]);
    send_to(b, 1, slot[3]);
    alSourcePlay(a);
    alSourcePlay(b);
    run_for(300);
    apu_ac97_get_stats(&s0);
    status(AL_XBOX_GP_LOAD_PEAK);
    run_for(2000);
    last = status(AL_XBOX_GP_LOAD);
    peak = status(AL_XBOX_GP_LOAD_PEAK);
    apu_ac97_get_stats(&s1);
    verdict(peak < 100 && s1.padded == s0.padded && s1.underruns == s0.underruns);
    logf_("   all four busy for 2 s: GP load %d %%, peak %d %% (expected below 100, console about 87); AC97 padded +%lu, "
          "underruns +%lu %s\n", last, peak, (unsigned long)(s1.padded - s0.padded),
          (unsigned long)(s1.underruns - s0.underruns),
          peak < 100 && s1.padded == s0.padded && s1.underruns == s0.underruns ? "PASS" : "FAIL");
    drop_source(b);

    /* each slot alone on A's send 0, then no send at all: the floor, once the
     * reverb's tail has died away (300 ms left it at -22 dB on the console) */
    send_to(a, 1, AL_EFFECTSLOT_NULL);
    send_to(a, 0, AL_EFFECTSLOT_NULL);
    n = settle_capture(3000, 400);
    floor_db = dbd(amp_at(0, n, 1000.0) / (s_dry_unit * DRY_PCM));
    logf_("   no send: 1 kHz %s dB against the dry ear (the floor)\n", dbs(tenths(floor_db)));
    for (k = 0; k < 4; k++) {
        double h1, h3, l1;
        bool ok, harm_ok;
        send_to(a, 0, slot[k]);
        n = settle_capture(300, 400);
        h1 = amp_at(0, n, 1000.0);
        h3 = dbd(amp_at(0, n, 3000.0) / (h1 > 0.0 ? h1 : 1.0));
        l1 = dbd(h1 / (s_dry_unit * DRY_PCM));
        ok = l1 >= -30.0 && l1 >= floor_db + 20.0;
        harm_ok = k == 2 ? h3 >= -30.0 : h3 <= -40.0;
        verdict(ok && harm_ok);
        logf_("   slot %u (%s) alone: 1 kHz %s dB against the dry ear (expected at least -30 and 20 over the floor), "
              "3 kHz %s dB against 1 kHz (expected %s) %s\n", (unsigned)slot[k], name[k], dbs(tenths(l1)),
              dbs(tenths(h3)), k == 2 ? "at least -30: the distortion's" : "at most -40: no other kernel distorts",
              ok && harm_ok ? "PASS" : "FAIL");
    }
    drop_source(a);

    /* one more past the budget: a second reverb (X: 2 x $8C0 words of $B20) where the autowah is */
    p_alAuxiliaryEffectSloti(slot[3], AL_EFFECTSLOT_EFFECT, (ALint)s_reverb);
    e = alGetError();
    p_alGetAuxiliaryEffectSloti(slot[3], AL_EFFECTSLOT_EFFECT, &cur);
    verdict(e == AL_INVALID_OPERATION && cur == (ALint)fx[3]);
    logf_("   a second reverb on slot %u: AL error 0x%x (expected 0x%x AL_INVALID_OPERATION), the slot still holds %s %s\n",
          (unsigned)slot[3], (unsigned)e, (unsigned)AL_INVALID_OPERATION, cur == (ALint)fx[3] ? "the autowah" : "SOMETHING ELSE",
          e == AL_INVALID_OPERATION && cur == (ALint)fx[3] ? "PASS" : "FAIL");
    verdict(apu_gp_debug_code_restarts() == r0);
    logf_("   GP restarts during 6j: %lu (expected 0) %s\n", (unsigned long)(apu_gp_debug_code_restarts() - r0),
          apu_gp_debug_code_restarts() == r0 ? "PASS" : "FAIL");

    for (k = 1; k < 4; k++) {
        p_alAuxiliaryEffectSloti(slot[k], AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
        p_alDeleteEffects(1, &fx[k]);
    }
    p_alDeleteAuxiliaryEffectSlots(3, &slot[1]);
    run_for(100);
    logf_("   6j: %d of %d checks passed; AL error 0x%x\n", (s_checks - c0) - (s_fails - f0), s_checks - c0,
          (unsigned)alGetError());
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

enum { FX_REVERB, FX_ECHO, FX_LIB };

typedef struct {
    const char *name;
    int kind;
    ALenum type;           /* FX_LIB: the EFX effect type (a library kernel) */
    float a, b, c;         /* reverb: decay, gain, -; echo: delay (s), feedback, wet */
} setting_t;

static const setting_t k_settings[] = {
    { "reverb, hall (2.5 s)", FX_REVERB, 0, 2.5f, 0.6f, 0 },
    { "echo 300 ms, feedback 0.45", FX_ECHO, 0, 0.300f, 0.45f, 0.5f },
    { "echo 150 ms, feedback 0.3 (parameters only)", FX_ECHO, 0, 0.150f, 0.30f, 0.5f },
    { "chorus (the EFX defaults)", FX_LIB, AL_EFFECT_CHORUS, 0, 0, 0 },
    { "ring modulator, 440 Hz sine", FX_LIB, AL_EFFECT_RING_MODULATOR, 0, 0, 0 },
    { "reverb, room (0.7 s)", FX_REVERB, 0, 0.7f, 0.4f, 0 },
    { "distortion, EDGE 0.4, GAIN 0.2", FX_LIB, AL_EFFECT_DISTORTION, 0, 0, 0 },
    { "flanger (the EFX defaults)", FX_LIB, AL_EFFECT_FLANGER, 0, 0, 0 },
    { "equalizer: low +6 dB, mid1 -6 dB, high +6 dB", FX_LIB, AL_EFFECT_EQUALIZER, 0, 0, 0 },
    { "compressor", FX_LIB, AL_EFFECT_COMPRESSOR, 0, 0, 0 },
    { "autowah, RESONANCE 10", FX_LIB, AL_EFFECT_AUTOWAH, 0, 0, 0 },
};

static ALuint s_lfx;   /* the rotation's library effect */

static void apply(const setting_t *s) {
    if (s->kind == FX_ECHO) {
        echo_params((int)(s->a * 48000.0f), s->b, s->c);
        attach(s_echo);
    } else if (s->kind == FX_REVERB) {
        p_alEffectf(s_reverb, AL_REVERB_DECAY_TIME, s->a);
        p_alEffectf(s_reverb, AL_REVERB_GAIN, s->b);
        attach(s_reverb);
    } else {
        p_alEffecti(s_lfx, AL_EFFECT_TYPE, s->type);   /* the type's defaults again */
        switch (s->type) {
        case AL_EFFECT_DISTORTION:
            p_alEffectf(s_lfx, AL_DISTORTION_EDGE, 0.4f);
            p_alEffectf(s_lfx, AL_DISTORTION_GAIN, 0.2f);
            break;
        case AL_EFFECT_EQUALIZER:
            p_alEffectf(s_lfx, AL_EQUALIZER_LOW_GAIN, 2.0f);
            p_alEffectf(s_lfx, AL_EQUALIZER_MID1_GAIN, 0.5f);
            p_alEffectf(s_lfx, AL_EQUALIZER_HIGH_GAIN, 2.0f);
            break;
        case AL_EFFECT_AUTOWAH:
            p_alEffectf(s_lfx, AL_AUTOWAH_RESONANCE, 10.0f);
            break;
        default:
            break;
        }
        attach(s_lfx);
    }
}

static void phase_listen(void) {
    ALuint hrtf_src, pan_src, st_src, b1, b2, b3;
    DWORD t0, next_switch, next_log;
    uint32_t gp_cnt = gpx(GP_LOOP_CNT);
    uint32_t k = 0, minute = 0, r0 = apu_gp_debug_code_restarts(), peak_load = 0;
    ALXBOXSTATS s0;

    logf_("-- 7 listening, 5 min: HRTF source circling, a panned one, a stereo one; the effect changes every 15 s\n"
          "   (%u settings in turn), then XReboot()\n", (unsigned)(sizeof(k_settings) / sizeof(k_settings[0])));
    alXboxOutputCallback(NULL, NULL);    /* back to zero-copy output */
    p_alGenEffects(1, &s_lfx);
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
            logf_("   %lu s: %s, AL error 0x%x; GP frames +%lu since the last change\n", (unsigned long)(t / 1000u),
                  s->name, (unsigned)alGetError(), (unsigned long)gp_frames_since(gp_cnt));
            gp_cnt = gpx(GP_LOOP_CNT);
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
            next_log += 60000u;
            if (minute == 5u) {
                logf_("== done: 5 min, GP load peak %lu %%; 6i / 6j %d of %d checks passed; XReboot\n",
                      (unsigned long)peak_load, s_checks - s_fails, s_checks);
                Sleep(500);
                XReboot();
            }
        }
        watchdog_kick(NULL);
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
    mount_e();
#ifndef NO_WATCHDOG
    watchdog_start();
#endif
    logf_("openal_efx: ALC_EXT_EFX, AL_XBOX_gp_program and the on-chip effects (phase9-08)\n");
    logf_("log: %s; watchdog %d s\n", s_log_file > 0 ? "E:\\openal_efx.txt, written through line by line"
                                                   : "NO E: (screen and RAM copy only)", WATCHDOG_TIMEOUT_MS / 1000);
    step("alcOpenDevice");
    dev = alcOpenDevice(NULL);
    if (!dev) {
        logf_("alcOpenDevice failed; XReboot in 5 s\n");
        Sleep(5000);
        XReboot();
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
    s_dev = dev;
    alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
    logf_("ALC_EXT_EFX %s, %d sends; entry points %s; GP program %s\n",
          alcIsExtensionPresent(dev, ALC_EXT_EFX_NAME) ? "present" : "MISSING", (int)sends,
          load_efx() && onchip_init() ? "all found" : "MISSING", apu_gp_program_loaded() ? "loaded" : "NOT LOADED");
    if (!p_alXboxEffectProgram || !s_mute) {
        logf_("XReboot in 5 s\n");
        Sleep(5000);
        XReboot();
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

    step("1 EFX reverb");
    phase_reverb();
    step("2-5 echo kernel");
    phase_echo();
    step("6 back to the reverb");
    phase_back();
    step("6b lowpass");
    phase_lowpass();
    step("6c EFX echo");
    phase_efx_echo();
    step("6d air absorption, cone GAINHF");
    phase_hf_auto();
    step("6e/6f stereo spatialize, pause");
    phase_spatialize();
    step("6g chorus, flanger");
    phase_efx_chorus();
    step("6h same-frame starts, timed parameters");
    phase_sync();
    step("6i dry reference");
    phase_onchip_ref();
    step("6i ring modulator");
    phase_ringmod();
    step("6i distortion");
    phase_distortion();
    step("6i equalizer");
    phase_equalizer();
    step("6i compressor");
    phase_compressor();
    step("6i autowah");
    phase_autowah();
    step("6j four slots");
    phase_multislot();
    logf_("== 6i / 6j: %d of %d checks passed\n", s_checks - s_fails, s_checks);
    step("7 listening");
    phase_listen();
    XReboot();
    return 0;
}
