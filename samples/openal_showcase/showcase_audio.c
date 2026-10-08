#include "showcase_audio.h"
#include <AL/al.h>
#include <AL/alext.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "al_source.h"
#include "al_buffer.h"

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <hal/audio.h>
#  include <windows.h>
#  include <xboxkrnl/xboxkrnl.h>
#  define HAS_AC97 1
#endif

/*
 * ============================================================================
 * Per-source mixer state (indexed by OpenAL source id - 1)
 * ============================================================================
 */
#define MIX_MAX_VOICES 32      /* loudest sources mixed per chunk */
#define ITD_MAX        64      /* library ITD taps are 0..64 samples */
#define CROSSFEED      0.35f   /* far-ear level in the stereo fold */

typedef struct {
    uint32_t play_seq;         /* alSourcePlay() count seen; a change restarts the voice */
    double pos;                /* read position in buffer frames */
    bool done;                 /* one-shot reached its end */
    float gain_l, gain_r;      /* gains applied at the end of the last chunk (ramp start) */
    float z1, z2;              /* biquad state (direct form II transposed) */
    float hist[ITD_MAX + 2];   /* last filtered mono samples, for the ITD taps */
    int hist_pos;
} mix_voice_t;

static mix_voice_t s_voices[AL_MAX_SOURCES];
static uint32_t s_last_voices;     /* sources mixed in the last chunk */

/*
 * Buffer data lives in write-combined memory (it is meant for the APU), which
 * is slow for the CPU to read. Each buffer is copied once into cached memory.
 */
typedef struct {
    ALuint id;
    const void *src_data;      /* data_virt the copy was made from */
    ALsizei size;
    int16_t *pcm;              /* 16-bit samples, interleaved when stereo */
    uint32_t frames;
} pcm_cache_t;

static pcm_cache_t s_cache[64];

static const int16_t *cached_pcm(const ALbuffer *b, uint32_t *frames) {
    pcm_cache_t *slot = NULL;
    uint32_t samples, i;
    int k;

    if (!b || !b->data_virt || b->size <= 0 || b->channels < 1 || b->channels > 2) {
        return NULL;
    }
    for (k = 0; k < (int)(sizeof(s_cache) / sizeof(s_cache[0])); k++) {
        if (s_cache[k].pcm && s_cache[k].id == b->id) {
            if (s_cache[k].src_data == b->data_virt && s_cache[k].size == b->size) {
                *frames = s_cache[k].frames;
                return s_cache[k].pcm;
            }
            /* Buffer refilled: drop the stale copy */
            free(s_cache[k].pcm);
            s_cache[k].pcm = NULL;
        }
        if (!slot && !s_cache[k].pcm) {
            slot = &s_cache[k];
        }
    }
    if (!slot) {
        return NULL;
    }

    samples = (b->bits == 8) ? (uint32_t)b->size : (uint32_t)b->size / 2u;
    slot->pcm = (int16_t *)malloc(samples * sizeof(int16_t));
    if (!slot->pcm) {
        return NULL;
    }
    if (b->bits == 8) {
        const uint8_t *s = (const uint8_t *)b->data_virt;   /* unsigned 8-bit */
        for (i = 0; i < samples; i++) {
            slot->pcm[i] = (int16_t)(((int)s[i] - 128) * 256);
        }
    } else {
        memcpy(slot->pcm, b->data_virt, samples * sizeof(int16_t));
    }
    slot->id = b->id;
    slot->src_data = b->data_virt;
    slot->size = b->size;
    slot->frames = samples / (uint32_t)b->channels;
    *frames = slot->frames;
    return slot->pcm;
}

static void cache_free_all(void) {
    int k;
    for (k = 0; k < (int)(sizeof(s_cache) / sizeof(s_cache[0])); k++) {
        free(s_cache[k].pcm);
        s_cache[k].pcm = NULL;
    }
}

static bool null_backend(void) {
    ALint v = -1;
    alXboxGetHardwareStatus(AL_XBOX_BACKEND, &v);
    return v == AL_XBOX_BACKEND_NULL;
}

/*
 * ============================================================================
 * Bus arithmetic
 *
 * Each source is rendered to 16-bit stereo pairs (L, R) before gain; gains
 * are Q14 (1.0 = 16384). Accumulating the sources into the 32-bit stereo bus
 * and the final saturating conversion to 16-bit run as MMX on the Xbox's
 * Pentium III. They are inline assembly because clang now implements the
 * MMX intrinsics (mmintrin.h) with SSE2, which the Pentium III lacks. The
 * scalar versions compute bit-identical results (SHOWCASE_AUDIO_SCALAR
 * forces them).
 * ============================================================================
 */
#define GAIN_ONE      16384
#define GAIN_SHIFT    14
#define RAMP_SEGMENTS 8        /* gain steps per chunk (64 frames each) */

/* nxdk's clang targets i386-pc-win32 (MSVC mode): no __GNUC__, but GNU asm works */
#if (defined(__i386__) || defined(_M_IX86) || defined(__x86_64__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(SHOWCASE_AUDIO_SCALAR)
#  define BUS_MMX 1
#endif

static int16_t gain_q14(float g) {
    float q = g * (float)GAIN_ONE;
    if (!(q > 0.0f)) return 0;
    if (q > 32767.0f) return 32767;
    return (int16_t)(q + 0.5f);
}

/* acc[2i] += (vb[2i] * gl) >> 14, acc[2i+1] += (vb[2i+1] * gr) >> 14; frames is even */
static void bus_accumulate(int32_t *acc, const int16_t *vb, uint32_t frames, int16_t gl, int16_t gr) {
#if defined(BUS_MMX)
    uint32_t gains = (uint32_t)(uint16_t)gl | ((uint32_t)(uint16_t)gr << 16);
    uint32_t pairs = frames / 2u;
    if (pairs == 0) {
        return;
    }
    __asm__ __volatile__(
        "movd       %[g], %%mm7\n\t"
        "punpckldq  %%mm7, %%mm7\n\t"      /* mm7 = gl gr gl gr */
        "1:\n\t"
        "movq       (%[s]), %%mm0\n\t"     /* L0 R0 L1 R1 */
        "movq       %%mm0, %%mm1\n\t"
        "pmullw     %%mm7, %%mm0\n\t"      /* low halves of the 32-bit products */
        "pmulhw     %%mm7, %%mm1\n\t"      /* high halves */
        "movq       %%mm0, %%mm2\n\t"
        "punpcklwd  %%mm1, %%mm0\n\t"      /* L0*gl, R0*gr */
        "punpckhwd  %%mm1, %%mm2\n\t"      /* L1*gl, R1*gr */
        "psrad      $14, %%mm0\n\t"
        "psrad      $14, %%mm2\n\t"
        "paddd      (%[a]), %%mm0\n\t"
        "paddd      8(%[a]), %%mm2\n\t"
        "movq       %%mm0, (%[a])\n\t"
        "movq       %%mm2, 8(%[a])\n\t"
        "add        $8, %[s]\n\t"
        "add        $16, %[a]\n\t"
        "dec        %[n]\n\t"
        "jnz        1b\n\t"
        "emms\n\t"                         /* hand the FPU back */
        : [s] "+r"(vb), [a] "+r"(acc), [n] "+r"(pairs)
        : [g] "r"(gains)
        : "mm0", "mm1", "mm2", "mm7", "memory", "cc");
#else
    uint32_t i;
    for (i = 0; i < frames; i++) {
        acc[2u * i]      += ((int32_t)vb[2u * i] * gl) >> GAIN_SHIFT;
        acc[2u * i + 1u] += ((int32_t)vb[2u * i + 1u] * gr) >> GAIN_SHIFT;
    }
#endif
}

/* out = saturate16(acc); samples is a multiple of 4 */
static void bus_to_pcm(int16_t *out, const int32_t *acc, uint32_t samples) {
#if defined(BUS_MMX)
    uint32_t quads = samples / 4u;
    if (quads == 0) {
        return;
    }
    __asm__ __volatile__(
        "1:\n\t"
        "movq       (%[a]), %%mm0\n\t"
        "packssdw   8(%[a]), %%mm0\n\t"    /* 4 x int32 -> 4 x int16, saturating */
        "movq       %%mm0, (%[o])\n\t"
        "add        $16, %[a]\n\t"
        "add        $8, %[o]\n\t"
        "dec        %[n]\n\t"
        "jnz        1b\n\t"
        "emms\n\t"
        : [a] "+r"(acc), [o] "+r"(out), [n] "+r"(quads)
        :
        : "mm0", "memory", "cc");
#else
    uint32_t i;
    for (i = 0; i < samples; i++) {
        int32_t s = acc[i];
        out[i] = (int16_t)(s > 32767 ? 32767 : (s < -32768 ? -32768 : s));
    }
#endif
}

/*
 * ============================================================================
 * Per-source rendering (scalar: resampling, the elevation biquad and the ITD
 * delay line are recursive per-sample work)
 * ============================================================================
 */

typedef struct {
    ALsource *src;
    float loudness;
} mix_pick_t;

static int pick_cmp(const void *a, const void *b) {
    float la = ((const mix_pick_t *)a)->loudness, lb = ((const mix_pick_t *)b)->loudness;
    return (la < lb) - (la > lb);
}

/* Linear interpolation of channel ch at fractional frame position p */
static float sample_at(const int16_t *pcm, uint32_t frames, int channels, int ch, double p, bool loop) {
    uint32_t i0, i1;
    float t;
    if (p < 0.0) {
        return 0.0f;
    }
    i0 = (uint32_t)p;
    t = (float)(p - (double)i0);
    if (i0 >= frames) {
        return 0.0f;
    }
    i1 = i0 + 1u;
    if (i1 >= frames) {
        i1 = loop ? 0u : i0;
    }
    return ((float)pcm[i0 * (uint32_t)channels + (uint32_t)ch] * (1.0f - t) +
            (float)pcm[i1 * (uint32_t)channels + (uint32_t)ch] * t) * (1.0f / 32768.0f);
}

static int16_t to_s16(float x) {
    float s = x * 32767.0f;
    if (s > 32767.0f) return 32767;
    if (s < -32768.0f) return -32768;
    return (int16_t)s;
}

/* Render `frames` frames of one source into vb (pre-gain stereo pairs); returns its target gains */
static bool render_source(ALsource *src, mix_voice_t *v, int16_t *vb, uint32_t frames,
                          float *target_l, float *target_r) {
    const ALbuffer *b = src->buffer;
    uint32_t len = 0, n;
    const int16_t *pcm = cached_pcm(b, &len);
    AL_SPATIAL_CALC calc;
    float step;
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    int itd_l = 0, itd_r = 0;
    bool loop = (src->looping == AL_TRUE);

    if (!pcm || len == 0) {
        return false;
    }

    al_source_calc_spatial(src, &calc);
    step = (float)b->frequency / (float)SHOWCASE_AUDIO_RATE * src->pitch;

    if (b->channels == 2) {
        /* 2D stereo buffer: direct to left/right (MixBins FL/FR in the library) */
        *target_l = *target_r = calc.effective_gain;
    } else {
        /* 3D mono: library pan gains folded to stereo, Doppler, ITD and elevation filter */
        const APU_PAN_GAINS_51 *g = &calc.pan_gains_51;
        float fold_l = g->fl + g->sl + 0.7071f * g->c + 0.5f * g->lfe;
        float fold_r = g->fr + g->sr + 0.7071f * g->c + 0.5f * g->lfe;
        /* Crossfeed: a hard-panned source still reaches the far ear, quieter
         * (and, through the ITD taps below, later), as it does on headphones */
        *target_l = (fold_l + CROSSFEED * fold_r) * calc.effective_gain;
        *target_r = (fold_r + CROSSFEED * fold_l) * calc.effective_gain;
        step *= calc.doppler_pitch;
        itd_l = calc.itd_delay_left > ITD_MAX ? ITD_MAX : calc.itd_delay_left;
        itd_r = calc.itd_delay_right > ITD_MAX ? ITD_MAX : calc.itd_delay_right;
        b0 = (float)calc.hrtf_coeffs.b0 / 16384.0f;
        b1 = (float)calc.hrtf_coeffs.b1 / 16384.0f;
        b2 = (float)calc.hrtf_coeffs.b2 / 16384.0f;
        a1 = (float)calc.hrtf_coeffs.a1 / 16384.0f;
        a2 = (float)calc.hrtf_coeffs.a2 / 16384.0f;
    }
    if (!(step > 0.05f)) step = 0.05f;
    if (step > 4.0f) step = 4.0f;

    for (n = 0; n < frames; n++) {
        if (v->pos >= (double)len) {
            if (loop) {
                v->pos = fmod(v->pos, (double)len);
            } else {
                v->done = true;
            }
        }

        if (b->channels == 2) {
            vb[2u * n]      = v->done ? 0 : to_s16(sample_at(pcm, len, 2, 0, v->pos, loop));
            vb[2u * n + 1u] = v->done ? 0 : to_s16(sample_at(pcm, len, 2, 1, v->pos, loop));
        } else {
            float x = v->done ? 0.0f : sample_at(pcm, len, 1, 0, v->pos, loop);
            float y = b0 * x + v->z1;
            int hl, hr;
            v->z1 = b1 * x - a1 * y + v->z2;
            v->z2 = b2 * x - a2 * y;
            if (!isfinite(v->z1) || !isfinite(v->z2)) {
                v->z1 = v->z2 = 0.0f;
                y = x;
            }
            v->hist_pos = (v->hist_pos + 1) % (ITD_MAX + 2);
            v->hist[v->hist_pos] = y;
            /* Each ear hears the filtered signal delayed by its ITD tap */
            hl = (v->hist_pos - itd_l + (ITD_MAX + 2)) % (ITD_MAX + 2);
            hr = (v->hist_pos - itd_r + (ITD_MAX + 2)) % (ITD_MAX + 2);
            vb[2u * n]      = to_s16(v->hist[hl]);
            vb[2u * n + 1u] = to_s16(v->hist[hr]);
        }
        if (!v->done) {
            v->pos += (double)step;
        }
    }
    return true;
}

static void mix_source(ALsource *src, mix_voice_t *v, int32_t *acc, uint32_t frames) {
    static int16_t vb[2 * SHOWCASE_AUDIO_CHUNK_FRAMES];
    float target_l = 0.0f, target_r = 0.0f;
    uint32_t seg_frames = frames / RAMP_SEGMENTS;
    uint32_t s;

    if (!render_source(src, v, vb, frames, &target_l, &target_r)) {
        return;
    }

    /* Mix headroom, then ramp the gains in steps across the chunk so a moving
     * source never clicks */
    target_l *= 0.6f;
    target_r *= 0.6f;
    if (seg_frames < 2u || (seg_frames & 1u) || seg_frames * RAMP_SEGMENTS != frames) {
        bus_accumulate(acc, vb, frames & ~1u, gain_q14(target_l), gain_q14(target_r));
    } else {
        for (s = 0; s < RAMP_SEGMENTS; s++) {
            float t = (float)(s + 1u) / (float)RAMP_SEGMENTS;
            int16_t gl = gain_q14(v->gain_l + (target_l - v->gain_l) * t);
            int16_t gr = gain_q14(v->gain_r + (target_r - v->gain_r) * t);
            bus_accumulate(acc + 2u * s * seg_frames, vb + 2u * s * seg_frames, seg_frames, gl, gr);
        }
    }
    v->gain_l = target_l;
    v->gain_r = target_r;

    /* One-shot finished: clear its ACTIVE bit in the null backend's register
     * stand-in, like the voice processor would, so the library reaps it */
    if (v->done && src->hw_voice_idx >= 0) {
        apu_voice_stop(al_source_get_apu_base(), (uint32_t)src->hw_voice_idx);
    }
}

void showcase_audio_mix(int16_t *out, uint32_t frames) {
    /* 8-byte alignment for the MMX loads and stores */
    static int32_t acc[2 * SHOWCASE_AUDIO_CHUNK_FRAMES] __attribute__((aligned(8)));
    mix_pick_t picks[AL_MAX_SOURCES];
    int npick = 0, i;
    uint32_t done = 0;

    /* Choose the sources to mix: playing, holding a voice slot (a virtualized
     * source is silent, as on the hardware), loudest first */
    for (i = 0; i < AL_MAX_SOURCES; i++) {
        ALsource *src = al_source_get((ALuint)(i + 1));
        mix_voice_t *v = &s_voices[i];
        if (!src) {
            continue;
        }
        if (src->play_seq != v->play_seq) {
            /* alSourcePlay() (re)started the source: from the beginning, fading in */
            memset(v, 0, sizeof(*v));
            v->play_seq = src->play_seq;
        }
        if (src->state != AL_PLAYING || !src->buffer || src->hw_voice_idx < 0 || v->done) {
            v->gain_l = v->gain_r = 0.0f;
            continue;
        }
        picks[npick].src = src;
        picks[npick].loudness = src->gain;
        if (src->buffer->channels == 1) {
            AL_SPATIAL_CALC calc;
            al_source_calc_spatial(src, &calc);
            picks[npick].loudness = calc.effective_gain;
        }
        npick++;
    }
    qsort(picks, (size_t)npick, sizeof(picks[0]), pick_cmp);
    if (npick > MIX_MAX_VOICES) {
        npick = MIX_MAX_VOICES;
    }
    s_last_voices = (uint32_t)npick;

    while (done < frames) {
        uint32_t n = frames - done;
        if (n > SHOWCASE_AUDIO_CHUNK_FRAMES) {
            n = SHOWCASE_AUDIO_CHUNK_FRAMES;
        }
        memset(acc, 0, sizeof(int32_t) * 2u * n);
        for (i = 0; i < npick; i++) {
            mix_source(picks[i].src, &s_voices[picks[i].src->id - 1], acc, n);
        }
        bus_to_pcm(out + 2u * done, acc, 2u * n);
        done += n;
    }
}

/*
 * ============================================================================
 * AC97 output, fed from the main loop
 *
 * nxdk's XAudio driver refills through a DPC callback that it schedules from
 * its interrupt bookkeeping. If the output ever drains, that bookkeeping
 * skips the completion and the DMA halts at the last valid descriptor; no
 * interrupt follows, the callback never runs again and the sound stops for
 * good after the first second or so. So no callback is used: every main-loop
 * frame reads the codec's current (CIV) and last valid (LVI) descriptor
 * indices, mixes straight into the free DMA slots until OUT_AHEAD chunks
 * (~85 ms) are queued, and restarts the DMA if it halted. Nothing runs at
 * DPC level (which also keeps MMX and the FPU out of it).
 * ============================================================================
 */
#define OUT_AHEAD     8u       /* chunks queued ahead of the one playing (~85 ms) */
#define OUT_SLOTS     12u      /* DMA buffers: OUT_AHEAD queued + 1 playing + spare */
#define CHUNK_BYTES   (SHOWCASE_AUDIO_CHUNK_FRAMES * 4u)

#define AC97_PO_CIV   0x114u   /* PCM out: current index value */
#define AC97_PO_LVI   0x115u   /* PCM out: last valid index */
#define AC97_PO_SR    0x116u   /* PCM out: status */
#define AC97_SR_DCH   0x01u    /* DMA controller halted */

static bool s_active = false;

#if defined(HAS_AC97)
static uint8_t *s_out[OUT_SLOTS];
static uint32_t s_out_next = 0;
static uint32_t s_underruns = 0;     /* times the DMA was found halted (output ran dry) */

static volatile uint8_t *ac97_regs(void) {
    extern AC97_DEVICE ac97Device;   /* nxdk hal/audio.c */
    return (volatile uint8_t *)ac97Device.mmio;
}

static void queue_one_chunk(void) {
    int16_t *dst = (int16_t *)s_out[s_out_next % OUT_SLOTS];
    showcase_audio_mix(dst, SHOWCASE_AUDIO_CHUNK_FRAMES);
    XAudioProvideSamples((unsigned char *)dst, (unsigned short)CHUNK_BYTES, 0);
    s_out_next++;
}

static void feed_ac97(void) {
    volatile uint8_t *pb = ac97_regs();
    bool halted = (pb[AC97_PO_SR] & AC97_SR_DCH) != 0;
    unsigned guard = 0;

    while (guard++ < OUT_AHEAD + 1u) {
        unsigned civ = pb[AC97_PO_CIV];
        unsigned lvi = pb[AC97_PO_LVI];
        if (((lvi - civ) & 31u) >= OUT_AHEAD) {
            break;
        }
        queue_one_chunk();
    }
    if (halted) {
        /* It ran out of queued buffers (the main loop stalled): resume */
        s_underruns++;
        XAudioPlay();
    }
}
#endif

int showcase_audio_init(void) {
    memset(s_voices, 0, sizeof(s_voices));
    s_active = false;

#if defined(HAS_AC97)
    {
        uint32_t i;
        for (i = 0; i < OUT_SLOTS; i++) {
            if (!s_out[i]) {
                s_out[i] = (uint8_t *)MmAllocateContiguousMemoryEx(CHUNK_BYTES, 0, 0x03FFAFFF, 0,
                                                                   PAGE_READWRITE | PAGE_WRITECOMBINE);
                if (!s_out[i]) {
                    return -1;
                }
            }
        }
        s_out_next = 0;
        s_underruns = 0;

        /* No callback: the main loop keeps the descriptor list filled */
        XAudioInit(16, 2, NULL, NULL);
        for (i = 0; i < OUT_AHEAD; i++) {
            queue_one_chunk();
        }
        XAudioPlay();
    }
#endif

    s_active = true;
    return 0;
}

void showcase_audio_update(void) {
#if defined(HAS_AC97)
    /* Only the null backend needs the software mix */
    if (!s_active || !null_backend()) {
        return;
    }
    feed_ac97();
#endif
}

void showcase_audio_shutdown(void) {
#if defined(HAS_AC97)
    if (s_active) {
        XAudioPause();
    }
#endif
    s_active = false;
    cache_free_all();
}

void showcase_audio_get_stats(uint32_t *voices, uint32_t *underruns) {
    if (voices) {
        *voices = s_last_voices;
    }
    if (underruns) {
#if defined(HAS_AC97)
        *underruns = s_underruns;
#else
        *underruns = 0;
#endif
    }
}

bool showcase_audio_active(void) {
    return s_active && null_backend();
}
