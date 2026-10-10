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
 * HRTF needs methods (the table upload and xemu's per-voice latch), so it is
 * only available where the front end works (apu_vp_hrtf_available()).
 *
 * Library voice slot n (0..63) is VP handle APU_VP_HANDLE_BASE + n: handles of
 * 64 and above are plain voices that skip the HRTF stage.
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
 * (apu_probe round 18; the dashboard leaves 0x2018 = 0x7FF): 2048 entries,
 * an 8 MiB linear space of 4 KiB pages */
#define APU_VP_SGE_ENTRIES   2048u
#define APU_VP_OUTPUTS       8

/* Zeroed bytes every OpenAL buffer carries after its data (al_buffer.c). On a
 * real console a voice that reaches its end stops all frame processing
 * (openal_vp froze when its first one-shot ended), so one-shots loop over this
 * silence and the driver stops them once CBO passes the data. */
#define APU_VP_SILENT_TAIL_BYTES 256u

typedef struct {
    uint32_t phys;          /* physical address of the sample data (contiguous) */
    uint32_t bytes;
    int channels;           /* 1 or 2 (interleaved) */
    int bits;               /* 8 (unsigned) or 16 (signed) */
    bool loop;
    int16_t pitch;          /* signed 4.12 log2 of (source rate * pitch / 48000) */
    uint8_t bins[APU_VP_OUTPUTS];   /* mixbin of each output */
    uint16_t vols[APU_VP_OUTPUTS];  /* 12-bit attenuation in 1/64 dB, 0 = unity, 0xFFF = mute */
    int hrtf_entry;         /* HRTF table entry 0..127 for handles below 64, -1 = none */
    uint32_t tail_bytes;    /* zeroed bytes readable after the data (APU_VP_SILENT_TAIL_BYTES), 0 = none */
    uint32_t loop_start;    /* looping voices: frames [loop_start, loop_end) repeat (loop_end 0 = the whole buffer) */
    uint32_t loop_end;
    uint32_t start_frame;   /* first frame played (CBO at start) */
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
 * True if front-end methods execute, so the HRTF stage (handles below 64) is
 * usable: on xemu, not on a real console.
 */
bool apu_vp_hrtf_available(void);

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
void apu_vp_voice_update(uintptr_t bar0, uint32_t handle, int16_t pitch, const uint16_t vols[APU_VP_OUTPUTS],
                         int hrtf_entry);

/** Pause (true: off its list, position kept) or resume (false: back on top of the 2D list). */
void apu_vp_voice_pause(uintptr_t bar0, uint32_t handle, bool pause);

/** Stop a voice: off its list (record inactive on xemu; left alone on hardware). */
void apu_vp_voice_off(uintptr_t bar0, uint32_t handle);

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

/**
 * Pages of the 2048-page sample space (APU_VP_SGE_ENTRIES) mapped for buffers
 * that playing, paused or just-stopped voices use. Buffers are mapped when a
 * voice starts; a mapping no voice uses is reclaimed when space is needed (or
 * by this call), so freeing and loading buffers never runs the space out.
 */
uint32_t apu_vp_sample_pages_used(uintptr_t bar0);

/** Voice record of a handle and the SGE table (host tests). */
uint8_t *apu_vp_debug_voice_record(uint32_t handle);
const uint32_t *apu_vp_debug_sge(void);

/* Encodings (exposed for tests) */

/** Signed 4.12 log2 pitch for a playback-rate ratio (source frames per 48 kHz frame). */
int16_t apu_vp_pitch_from_ratio(float ratio);

/** 12-bit attenuation (1/64 dB, 0 = unity, 0xFFF = mute) for a linear gain. */
uint16_t apu_vp_atten_from_gain(float gain);

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
