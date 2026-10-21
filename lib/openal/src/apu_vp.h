#ifndef APU_VP_H
#define APU_VP_H

/*
 * MCPX APU Voice Processor driver on the corrected register model
 * (mcpx_apu_regs.h, lib/openal/docs/XEMU_VERIFICATION.md sections 4, 7 and 8.1b).
 *
 * Route A of that document: buffer voices on the Voice Processor. The CPU
 * writes each voice record and the list heads itself (the VP reads them from
 * RAM every frame); front-end methods are not used for voices, because on a
 * real console the method queue stalls after one method. The GP program in
 * apu_gp.c copies mixbins 0/1 to a FIFO ring and, on hardware, apu_ac97.c
 * forwards it to the AC97 (both verified on a retail console). On xemu with
 * real-time DSP processing off, the VP output itself is the audio output.
 *
 * HRTF: on xemu through methods (the table upload and the per-voice latch,
 * handles below 64); on a real console, where methods stall, the driver
 * writes the HRTF target array itself and plays HRTF voices on the 3D list
 * (any handle; apu_hrtf_probe rounds 2-5, see the comment at
 * HRTF_TARGET_BYTES in apu_vp.c).
 *
 * Handles: xemu uses 64..255 for plain voices and 0..63 for HRTF voices; a
 * console uses 0..127 for both (APU_VP_HW_HANDLES).
 *
 * Every function takes the BAR0 base, so the encodings can be checked on the
 * host against plain memory.
 */

#include <stdbool.h>
#include <stdint.h>
#include "apu_hrtf.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APU_VP_HANDLE_BASE   64u
#define APU_VP_MAX_HANDLES   256u
/* On a real console the VP reports every handle above 127 as idle
 * (SE2FE_IDLE_VOICE, FEDECPARAM = handle) the first time it walks it, and
 * frame processing stops for good: samples/apu_vp_stress froze on handle 129
 * every run, with the driver and with raw record writes alike, and ran clean
 * on handles 64..127. Hardware voices are handles below this limit. */
#define APU_VP_HW_HANDLES    128u
/* BA is 24-bit (16 MiB), but on a real console the VP stops all frame
 * processing for good when it fetches through an SGE entry above 2047
 * (apu_probe round 18): 2048 entries,
 * an 8 MiB linear space of 4 KiB pages */
#define APU_VP_SGE_ENTRIES   2048u
#define APU_VP_OUTPUTS       8

/* Zeroed bytes every OpenAL buffer carries after its data (al_buffer.c). On a
 * real console a voice that reaches its end stops all frame processing
 * (openal_vp froze when its first one-shot ended), so one-shots loop over this
 * silence and the driver stops them once CBO passes the data. */
#define APU_VP_SILENT_TAIL_BYTES 256u

/*
 * Xbox ADPCM (bits = 4 in this library): IMA ADPCM in 36-byte blocks per
 * channel (stereo: the two headers, then 4-byte chunks alternating left and
 * right), 64 samples per block: the header sample is the block's first, and
 * the next block's header repeats its last (apu_adpcm_probe round 1: that
 * layout played at the exact rate, a 65-sample layout 65/64 too fast). CBO,
 * EBO and LBO count samples. All-zero blocks decode to silence, so the silent
 * tail of an ADPCM buffer is APU_VP_ADPCM_TAIL_BLOCKS zero blocks.
 */
#define APU_VP_ADPCM_BLOCK_BYTES  36u
#define APU_VP_ADPCM_BLOCK_FRAMES 64u
#define APU_VP_ADPCM_TAIL_BLOCKS  8u

static inline uint32_t apu_vp_bytes_to_frames(uint32_t bytes, int channels, int bits) {
    return (bits == 4) ? bytes / (APU_VP_ADPCM_BLOCK_BYTES * (uint32_t)channels) * APU_VP_ADPCM_BLOCK_FRAMES
                       : bytes / ((uint32_t)(bits / 8) * (uint32_t)channels);
}

/* Bytes before frame `frames` (ADPCM: the start of its block) */
static inline uint32_t apu_vp_frames_to_bytes(uint32_t frames, int channels, int bits) {
    return (bits == 4) ? frames / APU_VP_ADPCM_BLOCK_FRAMES * APU_VP_ADPCM_BLOCK_BYTES * (uint32_t)channels
                       : frames * (uint32_t)(bits / 8) * (uint32_t)channels;
}

/* Silence behind every buffer's data, and the byte it is made of */
static inline uint32_t apu_vp_tail_bytes(int channels, int bits) {
    return (bits == 4) ? APU_VP_ADPCM_TAIL_BLOCKS * APU_VP_ADPCM_BLOCK_BYTES * (uint32_t)channels
                       : APU_VP_SILENT_TAIL_BYTES;
}

static inline uint8_t apu_vp_silence_byte(int bits) {
    return (bits == 8) ? 0x80u : 0u;   /* U8 silence is 0x80; S16 and ADPCM 0 */
}

typedef struct {
    uint32_t phys;          /* physical address of the sample data (contiguous); with `pages`: the offset
                               of the first byte in pages[0] */
    const uint32_t *pages;  /* NULL, or the physical address of each 4 KiB page of the data, in order
                               (data that is not physically contiguous: alBufferDataStatic) */
    uint32_t page_count;
    uint32_t map_key;       /* with `pages`: non-zero, unique per page list (the mapping is cached by it) */
    uint32_t bytes;
    int channels;           /* 1 or 2 (interleaved) */
    int bits;               /* 8 (unsigned), 16 (signed) or 4 (Xbox ADPCM) */
    bool loop;
    int16_t pitch;          /* signed 4.12 log2 of (source rate * pitch / 48000) */
    uint8_t bins[APU_VP_OUTPUTS];   /* mixbin of each output */
    uint16_t vols[APU_VP_OUTPUTS];  /* 12-bit attenuation in 1/64 dB, 0 = unity, 0xFFF = mute */
    int hrtf_entry;         /* HRTF table entry 0..127 for handles below 64, -1 = none */
    uint32_t tail_bytes;    /* zeroed bytes readable after the data (APU_VP_SILENT_TAIL_BYTES), 0 = none */
    uint32_t loop_start;    /* looping voices: frames [loop_start, loop_end) repeat (loop_end 0 = the whole buffer) */
    uint32_t loop_end;
    uint32_t start_frame;   /* first frame played (CBO at start) */
    uint32_t fmode;         /* voice filter: CFG_MISC FMODE (0 = off, 1 = lowpass), apu_vp_lowpass() */
    uint32_t fc;            /* its TAR_FCA / TAR_FCB word: damping (31:16) | cutoff (15:0, s4.12 octaves) */
} apu_vp_voice_params_t;

/**
 * Allocate the voice array, SGE, SSL and notifier tables and run the bring-up
 * (quiesce, table bases, empty lists, idle trap, notifiers, front-end check,
 * HRTF if the front end works, GP stage, start frames, AC97 forwarding on
 * hardware).
 * @return 0 on success, negative on allocation failure.
 */
int apu_vp_init(uintptr_t bar0);

/** Turn every voice off, stop frame generation and free the tables. */
void apu_vp_deinit(uintptr_t bar0);

/**
 * Upload a 128-entry HRTF table (SET_CURRENT_HRTF_ENTRY, SET_HRIR x 15,
 * SET_HRIR_X per entry). apu_vp_init() loads the spherical-head table from
 * apu_hrtf.c; this can replace it. Does nothing without a working front end.
 */
void apu_vp_load_hrtf(uintptr_t bar0, const apu_hrtf_entry_t table[APU_HRTF_ENTRIES]);

/** True between apu_vp_init() and apu_vp_deinit(). */
bool apu_vp_ready(void);

/**
 * True if the HRTF stage is usable: on xemu through front-end methods
 * (handles below 64), on a real console through the HRTF arrays the driver
 * sets up (any handle, 3D list; APU_VP_DBG_NO_HRTF turns it off).
 */
bool apu_vp_hrtf_available(void);

/** True if front-end methods execute (xemu); a real console stalls them (HRTF there goes through the arrays). */
bool apu_vp_front_end_ok(void);

/** True if the handle plays as a console HRTF voice (3D list, CFG_HRTF_TARGET). */
bool apu_vp_voice_is_hrtf(uint32_t h);

/*
 * The VP runs a two-ear 31-tap filter per HRTF voice every frame: 120 of them
 * at once slowed its frames below real time on a console (openal_engine
 * phase 5: the AC97 feed padded 1047 buffers), so the voice layer plays at
 * most APU_VP_HRTF_MAX_VOICES as HRTF voices and pans the rest by volume.
 */
#define APU_VP_HRTF_MAX_VOICES 64u

/** Console HRTF voices playing or paused now. */
uint32_t apu_vp_hrtf_voices(void);

/* Front-end messages seen on a real console (FECTL bit 15) */
typedef struct {
    uint32_t events;
    uint32_t last_meth;     /* FEDECMETH / FEDECPARAM when the last one was seen */
    uint32_t last_param;
} apu_vp_fe_stats_t;

void apu_vp_get_fe_stats(apu_vp_fe_stats_t *out);

/* Last list operations of the driver (freeze diagnostics). op: S start, L link
 * (a = old head), H unlink at the head (a = new head), U unlink (a = predecessor
 * << 16 | next), X unlink did not find the voice, E one-shot ended (a = CBO),
 * Z the GP frame counter stopped (a = XGSCNT of its last frame; nothing is
 * recorded after it, so the entries before Z led up to the stall). */
typedef struct {
    char op;
    uint16_t h;
    uint32_t a;
    uint32_t t;   /* XGSCNT */
} apu_vp_trace_t;

/** Map this many extra pages behind every buffer mapped from now on (diagnostics). */
void apu_vp_debug_set_map_guard(uint32_t pages);

/* Diagnostic switches (apu_vp_debug_set_flags); NO_AC97 takes effect at the next apu_vp_init() */
#define APU_VP_DBG_NO_AC97          1u   /* do not forward the GP FIFO to the AC97 */
#define APU_VP_DBG_NO_FE_CHECK      2u   /* apu_vp_service() does not read FECTL */
#define APU_VP_DBG_NO_STALL_DETECT  4u   /* apu_vp_service() does not read the GP frame counter */
#define APU_VP_DBG_NO_END_SCAN      8u   /* apu_vp_service() leaves one-shots in their silent tail listed */
#define APU_VP_DBG_NO_FE_PROBE     16u   /* apu_vp_init() sends no methods and assumes a real console */
#define APU_VP_DBG_NO_AC97_THREAD  32u   /* pump the AC97 from apu_vp_service(), no thread (next apu_vp_init()) */
#define APU_VP_DBG_HANDLES_HIGH    64u   /* a console allocates handles 64..127 only, as before handles 0..63 were used */
#define APU_VP_DBG_NO_HRTF        128u   /* a console does not set up its HRTF stage (next apu_vp_init()) */
#define APU_VP_DBG_AC97_COPY      256u   /* the CPU copies the GP ring (64 KiB) to the AC97 instead of zero-copy (next apu_vp_init()) */
void apu_vp_debug_set_flags(uint32_t flags);

/** Copy up to `max` of the most recent operations, oldest first. @return count. */
uint32_t apu_vp_debug_trace(apu_vp_trace_t *out, uint32_t max);

/**
 * Write the voice record and link the voice at the top of the 2D list. A
 * handle that is still playing is taken off its list and restarted.
 * @return 0 on success, negative for bad parameters or if the buffer cannot
 *         be mapped.
 */
int apu_vp_voice_start(uintptr_t bar0, uint32_t handle, const apu_vp_voice_params_t *p);

/**
 * Update pitch, the eight output volumes and (for handles below 64, if
 * hrtf_entry >= 0 and HRTF is available) the HRTF entry of a running voice.
 */
/**
 * Move a running voice's eight outputs to `bins` (CFG_VBIN and the CFG_FMT bin
 * bits): it takes effect at once (EFX_PLAN.md 2.1), so mute what moves first.
 */
void apu_vp_voice_set_bins(uintptr_t bar0, uint32_t handle, const uint8_t bins[APU_VP_OUTPUTS]);

/** Set a running voice's filter (FMODE and TAR_FCA/FCB; written only when it changed). */
void apu_vp_voice_set_filter(uintptr_t bar0, uint32_t handle, uint32_t fmode, uint32_t fc);

void apu_vp_voice_update(uintptr_t bar0, uint32_t handle, int16_t pitch, const uint16_t vols[APU_VP_OUTPUTS],
                         int hrtf_entry);

/**
 * Switch looping on or off while the voice plays (AL_LOOPING), by rewriting
 * its loop markers in place. Off: the voice plays on to the end of its data
 * and stops there. On: it loops [loop_start, loop_end) (loop_end 0 = the whole
 * buffer); if it is already past the loop end it first plays to the end of
 * the data. Works on paused voices too.
 * @return 0, or negative if the handle is not playing, has already ended, or
 *         cannot be released (no silent tail).
 */
int apu_vp_voice_set_loop(uintptr_t bar0, uint32_t h, bool loop, uint32_t loop_start, uint32_t loop_end);

/** Pause (true: off its list, position kept) or resume (false: back on top of the 2D list). */
void apu_vp_voice_pause(uintptr_t bar0, uint32_t handle, bool pause);

/**
 * Hold the voice lists' heads back until apu_vp_batch_end(): the voices
 * started or resumed in between join the lists with one register write per
 * list, so they start on the same VP frame (alSourcePlayv). Voices taken off
 * the head of a list leave with it too; others leave at once.
 */
void apu_vp_batch_begin(uintptr_t bar0);
void apu_vp_batch_end(uintptr_t bar0);

/** Stop a voice: off its list (record inactive on xemu; left alone on hardware). */
void apu_vp_voice_off(uintptr_t bar0, uint32_t handle);

/**
 * Held voices: a start or a stop made exact by the output thread.
 *
 * The game thread acts on a timed start or stop only at its updates, once a
 * frame; at 30 fps that is up to 17 ms either side of the frame asked for,
 * which a weapon fired every 100 ms makes audible. With a hold, the game
 * thread starts the voice at the update before its frame, muted (current and
 * target volumes at full attenuation) and at the lowest pitch (1/256: 0.19
 * frames of the sound per ms), and the output thread, every 4 ms, gives it its
 * volumes and pitch at XGSCNT `due` (the VP ramps the volumes up over one
 * 32-sample frame). A mute does the opposite at `due`: the target volumes go to
 * full attenuation, and the game stops the voice at its next update.
 *
 * Volumes and pitch set while a start is held (apu_vp_voice_update) are kept
 * for its release; volumes set once a mute is done are kept and not written.
 * Restarting, pausing or switching off the voice drops its hold, giving it
 * back its volumes and pitch. The thread writes only the target volumes and
 * the pitch half of TAR_PITCH_LINK (16-bit stores: the link half is the game
 * thread's); the two sides take turns on the table (apu_vp_hold_tick).
 *
 * apu_vp_voice_hold() must come right after apu_vp_voice_start(), before
 * apu_vp_batch_end() makes the voice visible to the VP.
 * @return 0, or negative (the table is full, the handle has a hold already, or
 *         the VP is not ready): the caller acts at once instead.
 */
#define APU_VP_HOLD_MAX 64u
int apu_vp_voice_hold(uintptr_t bar0, uint32_t handle, uint32_t due);
int apu_vp_voice_mute_at(uintptr_t bar0, uint32_t handle, uint32_t due);
/** Drop a handle's hold (a stop cancelled): its volumes and pitch back. */
void apu_vp_voice_unhold(uintptr_t bar0, uint32_t handle);
/** A hold or a mute waiting on the handle (not yet released / done). */
bool apu_vp_voice_held(uint32_t handle);

/**
 * The output thread's pass: releases the starts and makes the mutes whose
 * XGSCNT `due` is at most `window` samples away. Skips the pass if the game
 * thread is changing the table. Cheap when nothing is held.
 */
void apu_vp_hold_tick(uintptr_t bar0, uint32_t window);

/** The game thread: releases or makes, at once, holds `late` samples past their due (the thread stalled). */
void apu_vp_hold_overdue(uintptr_t bar0, uint32_t late);

/** A hold or mute can be added (the table is not full). */
bool apu_vp_hold_room(void);

/** XGSCNT: the VP's sample counter (48 kHz). */
uint32_t apu_vp_now(uintptr_t bar0);

typedef struct {
    uint32_t holds;         /* starts held */
    uint32_t mutes;         /* mutes armed */
    uint32_t refused;       /* holds or mutes refused (table full, handle busy) */
    uint32_t done;          /* released or muted by the output thread */
    uint32_t overdue;       /* released or muted by the game thread (apu_vp_hold_overdue) */
    uint32_t skipped;       /* thread passes skipped while the game thread had the table */
    int32_t err_min;        /* XGSCNT - due when done, least and most (samples) */
    int32_t err_max;
    uint64_t err_abs_sum;   /* sum of |XGSCNT - due| over `done` */
    uint32_t now;           /* held or armed at this moment */
} apu_vp_hold_stats_t;

void apu_vp_hold_get_stats(apu_vp_hold_stats_t *out);

/**
 * A plain-voice handle (64..255 on xemu, 64..127 on a real console:
 * APU_VP_HW_HANDLES) that is not playing and has been off its
 * list for more than one APU frame. On a real console the VP must never meet
 * a record being rewritten (a half-written one ends the voice, which stops all
 * frames), so a restarted source gets a fresh handle instead of reusing the
 * one it may still be playing on.
 * @return the handle, or 0xFFFF if none is free.
 */
uint32_t apu_vp_alloc_handle(uintptr_t bar0);

/** Handles apu_vp_alloc_handle() can return: 192 on xemu, 128 on a console (64 with APU_VP_DBG_HANDLES_HIGH). */
uint32_t apu_vp_plain_handles(void);

/** Playing or paused: false once a one-shot ended or the voice was switched off. */
bool apu_vp_voice_active(uint32_t handle);

/** Current sample frame (CBO) of a voice. */
uint32_t apu_vp_voice_position(uint32_t handle);

/**
 * Housekeeping, to call every frame: release xemu's idle-voice trap, unlink
 * one-shots that ended, and on hardware feed the AC97 from the GP FIFO.
 * @return number of traps serviced.
 */
uint32_t apu_vp_service(uintptr_t bar0);

/** TSC cycles spent in apu_vp_service() since apu_vp_init(). */
uint64_t apu_vp_service_cycles(void);

/**
 * Pages of the 2048-page sample space (APU_VP_SGE_ENTRIES) mapped for buffers
 * that playing, paused or just-stopped voices use. Buffers are mapped when a
 * voice starts; a mapping no voice uses is reclaimed when space is needed (or
 * by this call), so freeing and loading buffers never runs the space out.
 */
uint32_t apu_vp_sample_pages_used(uintptr_t bar0);

/**
 * Frame starts apu_vp_init() had to redo since boot because the GP counted no
 * frames after the first one (a console only; 0 is the normal case).
 */
uint32_t apu_vp_start_retries(void);

/** The console HRTF target array (host tests), NULL if not set up. */
const uint32_t *apu_vp_debug_hrtf_target(void);

/** Voice record of a handle and the SGE table (host tests). */
uint8_t *apu_vp_debug_voice_record(uint32_t handle);
const uint32_t *apu_vp_debug_sge(void);

/* Encodings (exposed for tests) */

/** Signed 4.12 log2 pitch for a playback-rate ratio (source frames per 48 kHz frame). */
int16_t apu_vp_pitch_from_ratio(float ratio);

/** 12-bit attenuation (1/64 dB, 0 = unity, 0xFFF = mute) for a linear gain. */
uint16_t apu_vp_atten_from_gain(float gain);

/*
 * Voice filter as a lowpass (EFX_PLAN.md 2.1 / 3.7, apu_filter_probe): FMODE 1
 * is a 2-pole lowpass on TAR_FCA (TAR_FCB must be 0: HRTF voices apply it too).
 * Cutoff in the low half, 8 kHz x 2^(c / 0x1000) for c < 0 (0 and above: no
 * filtering); damping in the high half, APU_VP_LP_DAMPING flat (Butterworth:
 * -3.3 dB at the cutoff). apu_vp_lowpass() picks the cutoff whose measured
 * level at 5 kHz (the EFX reference) is AL_LOWPASS_GAINHF. Returns the FMODE
 * (0: no filter) and writes the FCA word to *fc.
 */
#define APU_VP_LP_DAMPING   0xB505u      /* sqrt(2) if 0x8000 = 1 */
#define APU_VP_LP_ZERO_HZ   8000.0f      /* the cutoff word 0 extrapolated */
#define APU_VP_LP_CUT_MIN   (-0x7000)    /* about 62 Hz, -76 dB at 5 kHz */
uint32_t apu_vp_lowpass(float gain_hf, uint32_t *fc);

/** Pack eight 12-bit output attenuations into TAR_VOLA/B/C. */
void apu_vp_pack_volumes(const uint16_t vols[APU_VP_OUTPUTS], uint32_t *vola, uint32_t *volb, uint32_t *volc);

/** Pack the eight output mixbins into CFG_VBIN (outputs 0-5) and the CFG_FMT bin bits (6, 7). */
void apu_vp_pack_bins(const uint8_t bins[APU_VP_OUTPUTS], uint32_t *vbin, uint32_t *fmt_bins);

/** CFG_FMT for a buffer voice (without the output bins). */
uint32_t apu_vp_format(int channels, int bits, bool loop);

#ifdef __cplusplus
}
#endif

#endif /* APU_VP_H */
