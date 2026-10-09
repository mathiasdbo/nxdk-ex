/*
 * OpenAL streaming on the real MCPX APU (phase A of
 * lib/openal/docs/XASH3D_INTEGRATION_PLAN.md): buffer queueing (al_stream.c)
 * and the AC97 pump thread (apu_ac97.c).
 *
 * A generated tone is streamed through alSourceQueueBuffers /
 * alSourceUnqueueBuffers in 4 KiB buffers, the way a game streams music or a
 * software mix, through these phases:
 *   1  48 kHz mono 16-bit, 440 Hz
 *   2  22.05 kHz stereo 16-bit: 330 Hz left, 495 Hz right
 *   3  11.025 kHz mono unsigned 8-bit, 660 Hz
 *   4  starvation: the feeder pauses 1 s in every 3; the source stops, then restarts
 *   5  loading screen: a static looping 220 Hz tone plus the stream; every 4 s
 *      the main thread blocks 1.5 s without any OpenAL call (the pump thread
 *      must keep the tone going; the stream plays out, then goes silent)
 *   6  soak: 48 kHz stereo chord, until the console is switched off
 *
 * E:\openal_stream.txt gets one line a second (saved every 5 s).
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "al_source.h"
#include "al_stream.h"
#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_voice.h"

#define BUF_BYTES   4096u
#define NBUF        6u
#define TWO_PI      6.28318531f

/* ---- log ---- */

static char s_log[96 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
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
        memmove(s_log, s_log + sizeof(s_log) / 2u, s_log_len - sizeof(s_log) / 2u);   /* keep the newer half */
        s_log_len -= sizeof(s_log) / 2u;
    }
    memcpy(s_log + s_log_len, line, (size_t)n);
    s_log_len += (size_t)n;
    debugPrint("%s", line);
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\openal_stream.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

/* ---- the streamed tone ---- */

typedef struct {
    ALuint src;
    ALuint buf[NBUF];
    ALenum format;
    ALsizei rate;
    float freq_l, freq_r;   /* freq_r: stereo only */
    float phase_l, phase_r;
    uint32_t chunks;        /* buffers queued */
    uint32_t restarts;      /* alSourcePlay after the source stopped */
    bool started;
} streamer_t;

static uint8_t s_chunk[BUF_BYTES];

static void generate(streamer_t *st) {
    uint32_t i, frames;
    float dl = TWO_PI * st->freq_l / (float)st->rate, dr = TWO_PI * st->freq_r / (float)st->rate;
    switch (st->format) {
    case AL_FORMAT_MONO8:
        for (i = 0; i < BUF_BYTES; i++) {
            s_chunk[i] = (uint8_t)(128 + (int)(40.0f * sinf(st->phase_l)));
            st->phase_l += dl;
        }
        break;
    case AL_FORMAT_STEREO16: {
        int16_t *p = (int16_t *)s_chunk;
        frames = BUF_BYTES / 4u;
        for (i = 0; i < frames; i++) {
            p[2 * i] = (int16_t)(7000.0f * sinf(st->phase_l));
            p[2 * i + 1] = (int16_t)(7000.0f * sinf(st->phase_r));
            st->phase_l += dl;
            st->phase_r += dr;
        }
        break;
    }
    default: {
        int16_t *p = (int16_t *)s_chunk;
        frames = BUF_BYTES / 2u;
        for (i = 0; i < frames; i++) {
            p[i] = (int16_t)(8000.0f * sinf(st->phase_l));
            st->phase_l += dl;
        }
        break;
    }
    }
    if (st->phase_l > 1000.0f) st->phase_l = fmodf(st->phase_l, TWO_PI);
    if (st->phase_r > 1000.0f) st->phase_r = fmodf(st->phase_r, TWO_PI);
}

static void queue_one(streamer_t *st, ALuint b) {
    generate(st);
    alBufferData(b, st->format, s_chunk, (ALsizei)BUF_BYTES, st->rate);
    alSourceQueueBuffers(st->src, 1, &b);
    st->chunks++;
}

static void stream_start(streamer_t *st, ALenum format, ALsizei rate, float fl, float fr) {
    uint32_t i;
    st->format = format;
    st->rate = rate;
    st->freq_l = fl;
    st->freq_r = fr;
    st->phase_l = st->phase_r = 0.0f;
    st->restarts = 0;
    st->started = false;
    for (i = 0; i < NBUF; i++) {
        queue_one(st, st->buf[i]);
    }
    alSourcePlay(st->src);
    st->started = true;
}

static void stream_stop(streamer_t *st) {
    alSourceStop(st->src);
    alSourcei(st->src, AL_BUFFER, 0);   /* releases the queue */
}

/* Refill processed buffers; restart the source if it ran dry */
static void stream_feed(streamer_t *st) {
    ALint processed = 0, state = 0;
    alGetSourcei(st->src, AL_BUFFERS_PROCESSED, &processed);
    while (processed-- > 0) {
        ALuint b;
        alSourceUnqueueBuffers(st->src, 1, &b);
        queue_one(st, b);
    }
    alGetSourcei(st->src, AL_SOURCE_STATE, &state);
    if (state != AL_PLAYING && st->started) {
        alSourcePlay(st->src);
        st->restarts++;
    }
}

/* ---- status ---- */

static void status_line(DWORD t0, int phase, const streamer_t *st, ALuint beacon) {
    ALint state = 0, queued = 0, processed = 0, bstate = 0;
    apu_ac97_stats_t as;
    ALsource *s = al_source_get(st->src);
    alGetSourcei(st->src, AL_SOURCE_STATE, &state);
    alGetSourcei(st->src, AL_BUFFERS_QUEUED, &queued);
    alGetSourcei(st->src, AL_BUFFERS_PROCESSED, &processed);
    if (beacon) {
        alGetSourcei(beacon, AL_SOURCE_STATE, &bstate);
    }
    apu_ac97_get_stats(&as);
    logf_("t %6lu ph %d %s q %ld p %ld chunks %5lu starved %lu restarts %lu | beacon %s | GP %7lu AC97 %6lu und %lu pad %lu "
          "pumps %lu gap %lu ms%s\n",
          (unsigned long)(GetTickCount() - t0), phase,
          state == AL_PLAYING ? "PLAY" : (state == AL_STOPPED ? "STOP" : "----"), (long)queued, (long)processed,
          (unsigned long)st->chunks, (unsigned long)((s && s->stream) ? al_stream_starved(s->stream) : 0u),
          (unsigned long)st->restarts, beacon ? (bstate == AL_PLAYING ? "on" : "off") : "-",
          (unsigned long)apu_gp_frames((uintptr_t)NV_PAPU_BASE), (unsigned long)as.chunks,
          (unsigned long)as.underruns, (unsigned long)as.padded, (unsigned long)as.pumps,
          (unsigned long)as.max_gap_ms, as.threaded ? "" : " (no thread)");
}

int main(void) {
    static const struct {
        ALenum format;
        ALsizei rate;
        float fl, fr;
        uint32_t ms;
        const char *what;
    } phases[] = {
        { AL_FORMAT_MONO16, 48000, 440.0f, 0.0f, 15000, "48 kHz mono16 440 Hz" },
        { AL_FORMAT_STEREO16, 22050, 330.0f, 495.0f, 15000, "22.05 kHz stereo16, 330 Hz L / 495 Hz R" },
        { AL_FORMAT_MONO8, 11025, 660.0f, 0.0f, 15000, "11.025 kHz mono8 660 Hz" },
        { AL_FORMAT_MONO16, 48000, 440.0f, 0.0f, 12000, "starvation: feeder off 1 s in every 3" },
        { AL_FORMAT_MONO16, 48000, 550.0f, 0.0f, 12000, "loading screen: 1.5 s blocks every 4 s, 220 Hz beacon" },
        { AL_FORMAT_STEREO16, 48000, 261.6f, 392.0f, 0, "soak: 48 kHz stereo chord" },
    };
    ALCdevice *dev;
    ALCcontext *ctx;
    streamer_t st;
    ALuint beacon = 0, beacon_buf = 0;
    DWORD t0, t_log, t_save;
    uint32_t k;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("openal_stream: buffer queueing and the AC97 pump thread\n");
    dev = alcOpenDevice(NULL);
    if (!dev) {
        logf_("alcOpenDevice failed\n");
        save_log();
        for (;;) Sleep(1000);
    }
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    logf_("backend: %s\n", apu_voice_hw_backend() ? "MCPX APU (VP + GP + AC97)" : "null (no sound)");

    memset(&st, 0, sizeof(st));
    alGenSources(1, &st.src);
    alSourcei(st.src, AL_SOURCE_RELATIVE, AL_TRUE);   /* centred, no distance attenuation */
    alGenBuffers(NBUF, st.buf);

    t0 = t_log = t_save = GetTickCount();
    for (k = 0; k < sizeof(phases) / sizeof(phases[0]); k++) {
        DWORD tp = GetTickCount(), t_block = tp;
        int phase = (int)k + 1;
        logf_("-- phase %d: %s\n", phase, phases[k].what);
        if (phase == 5) {
            /* a static looping tone: it must not stop while the main thread blocks */
            static int16_t tone[48000];
            uint32_t i;
            for (i = 0; i < 48000u; i++) tone[i] = (int16_t)(5000.0f * sinf(TWO_PI * 220.0f * (float)i / 48000.0f));
            alGenSources(1, &beacon);
            alGenBuffers(1, &beacon_buf);
            alBufferData(beacon_buf, AL_FORMAT_MONO16, tone, (ALsizei)sizeof(tone), 48000);
            alSourcei(beacon, AL_BUFFER, (ALint)beacon_buf);
            alSourcei(beacon, AL_LOOPING, AL_TRUE);
            alSourcei(beacon, AL_SOURCE_RELATIVE, AL_TRUE);
            alSourcePlay(beacon);
        }
        stream_start(&st, phases[k].format, phases[k].rate, phases[k].fl, phases[k].fr);
        while (phases[k].ms == 0 || GetTickCount() - tp < phases[k].ms) {
            DWORD now = GetTickCount();
            bool feed = true;
            if (phase == 4 && ((now - tp) % 3000u) >= 2000u) {
                feed = false;   /* the application is late for 1 s */
            }
            if (feed) {
                stream_feed(&st);
            }
            alXboxUpdateVoices();
            if (phase == 5 && now - t_block >= 4000u) {
                logf_("   (main thread blocked for 1.5 s)\n");
                Sleep(1500);   /* no OpenAL call: the pump thread alone keeps the output going */
                t_block = GetTickCount();
            }
            if (now - t_log >= 1000u) {
                t_log = now;
                status_line(t0, phase, &st, beacon);
            }
            if (now - t_save >= 5000u) {
                t_save = now;
                save_log();
            }
            Sleep(16);
        }
        stream_stop(&st);
        if (beacon) {
            alSourceStop(beacon);
            alSourcei(beacon, AL_BUFFER, 0);
            alDeleteSources(1, &beacon);
            alDeleteBuffers(1, &beacon_buf);
            beacon = 0;
        }
        save_log();
    }
    for (;;) Sleep(1000);
    return 0;
}
