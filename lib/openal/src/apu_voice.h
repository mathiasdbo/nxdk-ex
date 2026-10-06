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
 * The Voice Processor operates in 64 3D voice mode or 256 2D voice mode.
 * In 3D mode, each voice context is 128 bytes (32 DWORDs) and includes:
 *   - Resampler phase & 16.16 pitch step (polyphase resampler)
 *   - PRD table DMA pointers & fractional phase
 *   - EG1 (ADSR) amplitude and EG2 pitch/cutoff envelopes
 *   - ITD sample delay buffer (0..64 samples)
 *   - Q14 fixed-point HRTF biquad filtering (b0, b1, b2, a1, a2)
 *   - MixBin routing masks (MixBins 0..31) and 8-bit linear gain multipliers
 *
 * All 64 3D voice contexts reside contiguously in an 8 KB physical page
 * aligned to 4 KB (NV_PAPU_VOICE_ARRAY_SIZE_3D = 8192 bytes).
 * ============================================================================
 */

/* Voice Format Codes (DWORD 0: bits 0..1) */
#define NVAPU_VOICE_FORMAT_PCM16         0u  /* 16-bit Signed Little-Endian PCM */
#define NVAPU_VOICE_FORMAT_PCM8          1u  /* 8-bit Signed PCM */
#define NVAPU_VOICE_FORMAT_ADPCM         2u  /* Xbox ADPCM 4-bit */

/* Voice Channel Layout (DWORD 0: bit 2) */
#define NVAPU_VOICE_CHANNELS_MONO        0u  /* Mono source (1 channel) */
#define NVAPU_VOICE_CHANNELS_STEREO      1u  /* Stereo source (2 channels) */

/* Voice Looping Mode (DWORD 0: bit 3) */
#define NVAPU_VOICE_LOOP_OFF             0u  /* One-shot playback (stops at EOT) */
#define NVAPU_VOICE_LOOP_ON              1u  /* Looping playback */

/* 16.16 Fixed-Point Pitch Step Constants & Macros */
#define APU_PITCH_STEP_UNITY             0x00010000u /* 1.0x pitch step at 48000 Hz */

/**
 * Calculate 16.16 fixed-point phase pitch step for the Voice Processor.
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
    uint32_t pitch_step;                /* (SampleRate / 48000) * Pitch * 65536 */

    /* DWORD 2-4: DMA Pointers */
    uint32_t prd_table_phys;            /* Physical address of the PRD table */
    uint32_t current_prd_index;         /* Active PRD index */
    uint32_t sample_pos_frac;           /* [0:15] Fractional phase, [16:31] Byte offset */

    /* DWORD 5-6: Loop Offsets */
    uint32_t loop_start_offset;         /* Byte offset for loop restart */
    uint32_t loop_end_offset;           /* Byte offset for loop trigger */

    /* DWORD 7: Direct 2D Master Gain */
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

    /* DWORD 28-31: MixBin Gain Matrix (8-Bit Linear Multipliers) */
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

#ifdef __cplusplus
}
#endif

#endif /* APU_VOICE_H */
