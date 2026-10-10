#ifndef APU_VOICE_H
#define APU_VOICE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "apu_hardware.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox MCPX APU Voice Processor (VP) Voice Context & Management Definitions
 * ============================================================================
 * WARNING: NVAPU_VOICE_CONTEXT_3D below is a SOFTWARE MODEL. It is NOT the
 * NV_PAVS voice record that the VP reads. In xemu (478b4f4) a voice is 0x80
 * bytes at VPVADDR + handle * 0x80, with DWORD 0 = CFG_VBIN, DWORD 1 =
 * CFG_FMT, 0x54 = PAR_STATE (ACTIVE_VOICE is bit 21), 0x58 = current buffer
 * offset (CBO) and 0x7C = next-voice handle | pitch. Only the size (128 bytes)
 * coincides. Voices are programmed with front-end methods, not by writing
 * this structure. Differences that matter (details and xemu source
 * references: lib/openal/docs/XEMU_VERIFICATION.md):
 *  - Pitch: hardware TAR_PITCH is a signed 16-bit log2 value in 4.12 fixed
 *    point (bits 31:16 of the method argument), p = round(4096 *
 *    log2(src_rate / 48000)). The 16.16 linear step used here is a model
 *    value only.
 *  - Volume: hardware volumes are 12-bit ATTENUATIONS in 1/64 dB, where 0 is
 *    unity (0 dB) and 0xFFF is mute. The linear gains used here (0 = silence)
 *    have the OPPOSITE polarity: a zero-filled voice record mixes all eight
 *    outputs at 0 dB into bin 0. (For handles < 64 only outputs 0-3 are
 *    redirected to the four HRTF submix bins; outputs 4-7 still use the
 *    record's bins, i.e. bin 0. With HRTF enabled, a zero HRTF handle on such
 *    a voice is not the null handle (null is 0xFFFF), so the voice is silent
 *    unless a table entry was latched.)
 *  - Routing: 8 (5-bit mixbin, 12-bit attenuation) outputs per voice, not a
 *    mixbin bitmask plus 16 gains.
 *  - 3D: HRTF is a 31-tap FIR plus one ITD (+/- 42 samples) taken from a
 *    128-entry table loaded by methods, for handles < 64 only. There is no
 *    Q14 biquad or RAM ITD buffer in the voice record.
 *  - Buffers: a global SGE page table (VPSGEADDR) and sample-unit offsets
 *    (CBO/LBO/EBO), not per-buffer PRD lists with byte offsets.
 *  - 256 voice handles coexist (no 64-3D-OR-256-2D mode); a voice runs only
 *    after VOICE_ON has linked it into a voice list.
 *
 * Model summary (this file):
 * Each voice context is 128 bytes (32 DWORDs) and holds:
 *   - 16.16 pitch step
 *   - PRD table DMA pointers & position
 *   - EG1 (ADSR) amplitude and EG2 pitch/cutoff envelope placeholders
 *   - ITD delay values (field range 0..64 samples; the Woodworth formula in
 *     apu_spatial.c never exceeds 31) and a delay-buffer pointer
 *   - Q14 HRTF biquad coefficients (b0, b1, b2, a1, a2)
 *   - MixBin routing mask (MixBins 0..31) and 8-bit linear gain multipliers
 *
 * All 64 voice contexts reside contiguously in an 8 KB physical page aligned
 * to 4 KB (NV_PAPU_VOICE_ARRAY_SIZE_3D = 8192 bytes); the VP can address up
 * to 256 handles (32 KB).
 * ============================================================================
 */

/* Voice Format Codes (DWORD 0: bits 0..1) */
#define NVAPU_VOICE_FORMAT_PCM16         0u  /* 16-bit Signed Little-Endian PCM */
/* 8-bit PCM is UNSIGNED (offset binary, 0x80 = silence): the hardware sample
 * size is U8, and OpenAL AL_FORMAT_*8 data is unsigned as well, so it is
 * copied unchanged and needs no conversion. */
#define NVAPU_VOICE_FORMAT_PCM8          1u  /* 8-bit Unsigned PCM */
#define NVAPU_VOICE_FORMAT_ADPCM         2u  /* Xbox ADPCM 4-bit */

/* Voice Channel Layout (DWORD 0: bit 2) */
#define NVAPU_VOICE_CHANNELS_MONO        0u  /* Mono source (1 channel) */
#define NVAPU_VOICE_CHANNELS_STEREO      1u  /* Stereo source (2 channels) */

/* Voice Looping Mode (DWORD 0: bit 3) */
#define NVAPU_VOICE_LOOP_OFF             0u  /* One-shot playback (stops at EOT) */
#define NVAPU_VOICE_LOOP_ON              1u  /* Looping playback */

/* 16.16 Fixed-Point Pitch Step Constants & Macros
 * (software-model value; hardware TAR_PITCH is a signed 4.12 log2 field,
 * p = round(4096 * log2(rate / 48000)), where unity is 0, not 0x10000) */
#define APU_PITCH_STEP_UNITY             0x00010000u /* 1.0x pitch step at 48000 Hz */

/**
 * Calculate 16.16 fixed-point phase pitch step for the Voice Processor.
 * Linear model value, NOT the hardware encoding (see the banner above).
 *
 * Formula: ((SampleRate * (pitch_multiplier * 65536 + 0.5)) / 48000)
 *
 * @param sr Source sample rate in Hz (e.g. 44100, 48000, 22050).
 * @param pitch_mult Floating-point pitch scale factor (e.g. 1.0f).
 * @return 32-bit unsigned 16.16 fixed-point pitch step.
 */
#define APU_CALC_PITCH_STEP(sr, pitch_mult) \
    ((uint32_t)((((uint64_t)(sr) * (uint32_t)((pitch_mult) * 65536.0f + 0.5f))) / 48000u))

/*
 * ----------------------------------------------------------------------------
 * 3D Voice Context Layout (128 Bytes / 32 DWORDs)
 * ----------------------------------------------------------------------------
 */
#if defined(_MSC_VER) && !defined(__clang__)
#pragma pack(push, 1)
#endif

typedef struct
#if defined(__GNUC__) || defined(__clang__)
__attribute__((packed, aligned(128)))
#elif defined(_MSC_VER)
__declspec(align(128))
#endif
NVAPU_VOICE_CONTEXT_3D {
    /* DWORD 0: Voice Control & Format (32 bits) */
    uint32_t format              : 2;   /* 00 = 16-bit Signed PCM, 01 = 8-bit, 10 = ADPCM */
    uint32_t channels            : 1;   /* 0 = Mono, 1 = Stereo */
    uint32_t loop_mode           : 1;   /* 0 = One-shot, 1 = Looping */
    uint32_t mode_3d             : 1;   /* 1 = Full 3D HRTF/ITD processing enabled */
    uint32_t active              : 1;   /* 1 = Active rendering */
    uint32_t int_on_eot          : 1;   /* Trigger interrupt when hitting EOT flag */
    uint32_t reserved0           : 25;

    /* DWORD 1: Resampler Phase Pitch Step (16.16 fixed-point) */
    uint32_t pitch_step;                /* (SampleRate / 48000) * Pitch * 65536
                                           (linear model; hardware TAR_PITCH is signed 4.12 log2) */

    /* DWORD 2-4: DMA Pointers */
    uint32_t prd_table_phys;            /* Physical address of the PRD table */
    uint32_t current_prd_index;         /* Active PRD index */
    uint32_t sample_pos_frac;           /* [0:15] Fractional phase, [16:31] Byte offset */

    /* DWORD 5-6: Loop Offsets */
    uint32_t loop_start_offset;         /* Byte offset for loop restart */
    uint32_t loop_end_offset;           /* Byte offset for loop trigger */

    /* DWORD 7: Direct 2D Master Gain
     * Linear model gains, 0 = silence. Hardware volumes are 12-bit
     * attenuations (1/64 dB) with 0 = unity and 0xFFF = mute. */
    uint16_t master_vol_left;           /* Linear gain Left  (0 - 0xFFFF) */
    uint16_t master_vol_right;          /* Linear gain Right (0 - 0xFFFF) */

    /* DWORD 8-11: Amplitude Envelope (EG1 - ADSR) */
    uint32_t eg1_attack;
    uint32_t eg1_decay;
    uint32_t eg1_sustain;
    uint32_t eg1_release;

    /* DWORD 12-15: Pitch / Filter Modulation (EG2) */
    uint32_t eg2_modulation;
    uint32_t eg2_rate;
    uint32_t filter_cutoff;
    uint32_t filter_resonance;

    /* DWORD 16-19: ITD Spatial Delay Engine */
    uint16_t itd_delay_left;            /* Left ear tap delay in samples (0 to 64) */
    uint16_t itd_delay_right;           /* Right ear tap delay in samples (0 to 64) */
    uint32_t itd_delay_buffer;          /* Physical address of circular ITD buffer */
    uint32_t itd_feedback_left;
    uint32_t itd_feedback_right;

    /* DWORD 20-23: HRTF Biquad Coefficients (Q14 Fixed-Point) */
    int16_t  hrtf_b0;                   /* Feedforward tap 0 */
    int16_t  hrtf_b1;                   /* Feedforward tap 1 */
    int16_t  hrtf_b2;                   /* Feedforward tap 2 */
    int16_t  hrtf_a1;                   /* Feedback tap 1 */
    int16_t  hrtf_a2;                   /* Feedback tap 2 */
    int16_t  reserved1[3];

    /* DWORD 24-27: MixBin Routing */
    uint32_t mixbin_routing_mask;       /* Bitmask (0x0000003F enables MixBins 0..5) */
    uint32_t reserved2[3];

    /* DWORD 28-31: MixBin Gain Matrix (8-Bit Linear Multipliers)
     * Linear model gains, 0 = silence (hardware polarity is inverted, see
     * the banner). The FL..LFE bin order is this library's convention. */
    uint8_t  mixbin_gain[16];           /* [0]=FL, [1]=FR, [2]=SL, [3]=SR, [4]=C, [5]=LFE */
}
#if defined(_MSC_VER) && !defined(__clang__)
NVAPU_VOICE_CONTEXT_3D;
#pragma pack(pop)
#else
NVAPU_VOICE_CONTEXT_3D;
#endif

/*
 * ----------------------------------------------------------------------------
 * Voice Subsystem Management API
 * ----------------------------------------------------------------------------
 */

/**
 * Initialize the Voice Processor subsystem and register the voice context array.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param context_array_virt Virtual memory pointer to the 8 KB aligned voice array.
 * @param context_array_phys Physical memory address of the 8 KB aligned voice array.
 * @return 0 on success, negative value on error.
 */
int apu_voice_subsystem_init(uintptr_t apu_base, void *context_array_virt, uint32_t context_array_phys);

/**
 * Shut down the Voice Processor subsystem, halting active voices.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 */
void apu_voice_subsystem_deinit(uintptr_t apu_base);

/**
 * Reset a 3D voice context to default clean state.
 *
 * @param ctx Pointer to voice context structure.
 */
void apu_voice_context_reset(NVAPU_VOICE_CONTEXT_3D *ctx);

/**
 * Configure voice parameters in the hardware context array.
 *
 * @param index Voice slot index (0..63).
 * @param ctx Pointer to configured 3D voice context.
 * @return 0 on success, negative value on out-of-bounds index or uninitialized array.
 */
int apu_voice_setup(uint32_t index, const NVAPU_VOICE_CONTEXT_3D *ctx);

/**
 * Retrieve a pointer to a voice context in the managed context array.
 *
 * @param index Voice slot index (0..63).
 * @return Pointer to context, or NULL if invalid index or not initialized.
 */
NVAPU_VOICE_CONTEXT_3D *apu_voice_get_context(uint32_t index);

/**
 * Trigger voice playback by setting its active bit in NV_PAPU_VP_ACTIVE_0/1.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param index Voice slot index (0..63).
 * @return 0 on success, negative value on error.
 */
int apu_voice_trigger(uintptr_t apu_base, uint32_t index);

/**
 * Stop voice playback by clearing its active bit in NV_PAPU_VP_ACTIVE_0/1.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param index Voice slot index (0..63).
 * @return 0 on success, negative value on error.
 */
int apu_voice_stop(uintptr_t apu_base, uint32_t index);

/**
 * Pause or resume voice playback via NV_PAPU_VP_PAUSE_0/1.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param index Voice slot index (0..63).
 * @param pause 1 to pause, 0 to resume.
 * @return 0 on success, negative value on error.
 */
int apu_voice_pause(uintptr_t apu_base, uint32_t index, int pause);

/**
 * Check if a voice is currently marked active in hardware.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param index Voice slot index (0..63).
 * @return 1 if active, 0 if inactive or invalid index.
 */
int apu_voice_is_active(uintptr_t apu_base, uint32_t index);

/**
 * Number of apu_voice_stop() calls on valid voice indices since program start
 * (wraps at 2^32). Regression-test aid: lets a test observe that a voice was
 * halted before its context was rewritten.
 */
uint32_t apu_voice_debug_stop_count(void);

/**
 * Record the sample buffer a voice slot will play (needed by the hardware
 * Voice Processor backend, which addresses the buffer itself).
 */
void apu_voice_bind_buffer(uint32_t index, uint32_t phys, uint32_t bytes, int channels, int bits);

/**
 * After apu_voice_bind_buffer(): the loop region of a looping voice
 * ([loop_start, loop_end) in frames, loop_end 0 = the whole buffer) and the
 * frame the next trigger starts at (hardware backend; the software model's
 * position starts there too).
 */
void apu_voice_bind_loop(uint32_t index, uint32_t loop_start, uint32_t loop_end, uint32_t start_frame);

/**
 * Direct mode (AL_XBOX_DIRECT_GAINS): the slot's left/right output gains
 * (times the context's master volume) replace the pan; a mono slot plays as a
 * plain voice, without HRTF. Takes effect at the next trigger or commit.
 */
void apu_voice_set_direct(uint32_t index, bool on, float left, float right);

/**
 * Switch looping of a running voice (AL_LOOPING while playing or paused): the
 * hardware backend rewrites its loop markers in place (apu_vp_voice_set_loop);
 * the software model updates the context's loop mode.
 */
void apu_voice_set_looping(uintptr_t apu_base, uint32_t index, bool loop);

/**
 * Record a voice slot's source position in the listener frame (+x right,
 * +y up, +z behind); the hardware backend picks the HRTF entry from it.
 */
void apu_voice_set_direction(uint32_t index, const float local_pos[3]);

/**
 * Push a running voice's pitch and output volumes from its context to the
 * hardware (no-op on the software model).
 */
void apu_voice_commit(uintptr_t apu_base, uint32_t index);

/**
 * Service the Voice Processor's idle-voice trap (no-op on the software
 * model). Must run regularly on hardware: an unserviced trap halts the VP.
 */
void apu_voice_service(uintptr_t apu_base);

/**
 * Voice Processor handle a slot last started (hardware backend): the slot
 * number for mono (HRTF) voices, APU_VP_HANDLE_BASE + slot otherwise.
 */
uint32_t apu_voice_hw_handle(uint32_t index);

/**
 * True when the voice calls drive the hardware Voice Processor.
 */
bool apu_voice_hw_backend(void);

/*
 * Voice slots (the library's hardware voices, what the voice manager hands to
 * sources). The software model and xemu have 64 (on xemu, mono slot n plays
 * on HRTF handle n). A real console has 120: its VP plays every handle 0..127
 * as a plain voice (apu_probe3: all 128 advance, 2389 restarts in 5 s); 8
 * handles stay spare so a restart always finds one past the one-frame quarantine.
 */
#define APU_VOICE_MAX_SLOTS 128u
#define APU_VOICE_HW_SLOTS  120u

/** Voice slots of the active backend: APU_VOICE_HW_SLOTS on a console, 64 otherwise. */
uint32_t apu_voice_slot_count(void);

/**
 * Sample frame a voice slot is playing in its buffer: the VP's CBO on the
 * hardware backend; on the software model, the value last set with
 * apu_voice_debug_set_position() (0 after a trigger), since nothing plays there.
 */
uint32_t apu_voice_get_position(uint32_t index);

/** Software model: set a slot's play position (host tests stand in for the VP). */
void apu_voice_debug_set_position(uint32_t index, uint32_t frames);

#ifdef __cplusplus
}
#endif

#endif /* APU_VOICE_H */
