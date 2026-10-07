#ifndef APU_GP_UCODE_H
#define APU_GP_UCODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "apu_hardware.h"
#include "apu_eeprom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Motorola DSP56300 Global Processor (GP) Microcode Definitions
 * ============================================================================
 * The MCPX APU Global Processor executes 24-bit DSP instructions mapped
 * into 32-bit DWORDs in Program RAM (NV_PAPU_GPDSP_PRAM @ 0xFE820000).
 *
 * Audio frames run synchronously at 48 kHz triggered by hardware frame ticks.
 * - Input:  32 discrete MixBins mapped in X-RAM (X:$0000 .. X:$001F)
 * - Output: 6 EP FIFOs mapped in Y-RAM (Y:$FFB0 .. Y:$FFB5)
 * ============================================================================
 */

#define GP_UCODE_PASSTHROUGH_51_DWORDS   81u
#define GP_UCODE_STEREO_DOWNMIX_DWORDS   86u

#define GP_PASSTHROUGH_51_SIZE           GP_UCODE_PASSTHROUGH_51_DWORDS
#define GP_STEREO_DOWNMIX_SIZE           GP_UCODE_STEREO_DOWNMIX_DWORDS

#if defined(__GNUC__) || defined(__clang__)
#define APU_UCODE_UNUSED __attribute__((unused))
#else
#define APU_UCODE_UNUSED
#endif

/*
 * ----------------------------------------------------------------------------
 * 5.1 Surround Passthrough Microcode (81 DWORDs)
 * ----------------------------------------------------------------------------
 * Routes 6 discrete MixBins directly to 6 Output Processor FIFOs:
 *   X:$0000 -> Y:$FFB0 (Front Left)
 *   X:$0001 -> Y:$FFB1 (Front Right)
 *   X:$0002 -> Y:$FFB2 (Surround Left)
 *   X:$0003 -> Y:$FFB3 (Surround Right)
 *   X:$0004 -> Y:$FFB4 (Center)
 *   X:$0005 -> Y:$FFB5 (LFE / Subwoofer)
 */
static const uint32_t gp_passthrough_51_bin[GP_UCODE_PASSTHROUGH_51_DWORDS] APU_UCODE_UNUSED = {
    /* P:$0000: Reset Vector (2 DWORDs) */
    0x0AF080u, 0x000040u, /* JMP $0040 */

    /* P:$0002..P:$003F: Vector table NOP padding (62 DWORDs) */
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,

    /* P:$0040: Frame Loop Entry (3 DWORDs) */
    0x00000Cu,            /* WAIT (Sleep until 48kHz frame tick) */
    0x5CE000u,            /* MOVE #$0000, r0 (MixBin X-RAM Base) */
    0x5CE1B0u,            /* MOVE #$FFB0, r1 (EP FIFO Y-RAM Base) */

    /* Channel Transfers: discrete X-RAM to Y-RAM (12 DWORDs) */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (FL) */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (FR) */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (SL) */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (SR) */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (C)  */
    0x0E4000u, 0x0E4900u, /* MOVE x:(r0)+, a  -->  MOVE a, y:(r1)+ (LFE)*/

    /* P:$004F: Loop back to WAIT (2 DWORDs) */
    0x0AF080u, 0x000040u  /* JMP $0040 */
};

/*
 * ----------------------------------------------------------------------------
 * Stereo Downmix Microcode (ITU-R BS.775 with Saturation Mode, 86 DWORDs)
 * ----------------------------------------------------------------------------
 * Equations:
 *   L = FL + 0.7071 * C + 0.7071 * SL
 *   R = FR + 0.7071 * C + 0.7071 * SR
 *
 * Fixed-point coefficient:
 *   0.70710678 in Q23 = round(0.70710678 * 8388608) = 0x5A827A
 * Saturation mode enabled via ORI #$001000, SR to prevent clipping wrap.
 */
static const uint32_t gp_stereo_downmix_bin[GP_UCODE_STEREO_DOWNMIX_DWORDS] APU_UCODE_UNUSED = {
    /* P:$0000: Reset Vector (2 DWORDs) */
    0x0AF080u, 0x000040u, /* JMP $0040 */

    /* P:$0002..P:$003F: Vector table NOP padding (62 DWORDs) */
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,
    0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u, 0x000000u,

    /* P:$0040: Initialization (2 DWORDs) */
    0x0000F8u, 0x001000u, /* ORI #$001000, SR (Enable Saturation Mode) */

    /* P:$0042: Frame Loop Entry (5 DWORDs) */
    0x00000Cu,            /* WAIT (Sleep until 48kHz frame tick) */
    0x56F400u, 0x5A827Au, /* MOVE #$5A827A, y0 (-3dB factor in Q23) */
    0x5CE000u,            /* MOVE #$0000, r0 (MixBin base in X-RAM) */
    0x5CE1B0u,            /* MOVE #$FFB0, r1 (EP FIFO base in Y-RAM) */

    /* Center Channel Processing: A = 0.7071 * C, B = 0.7071 * C (3 DWORDs) */
    0x08E040u,            /* MOVE x:(r0+4), x0 (Read Center MixBin 4) */
    0x217000u,            /* MPY x0, y0, a     (A = 0.7071 * Center) */
    0x200018u,            /* MOVE a, b         (B = 0.7071 * Center) */

    /* Front Channel Accumulation: A += FL, B += FR (4 DWORDs) */
    0x0E4000u,            /* MOVE x:(r0)+, x0  (Read FL MixBin 0) */
    0x200008u,            /* ADD x0, a         (A += FL) */
    0x0E4000u,            /* MOVE x:(r0)+, x0  (Read FR MixBin 1) */
    0x200028u,            /* ADD x0, b         (B += FR) */

    /* Surround Channel Accumulation: A += 0.7071 * SL, B += 0.7071 * SR (4 DWORDs) */
    0x0E4000u,            /* MOVE x:(r0)+, x0  (Read SL MixBin 2) */
    0x217800u,            /* MACR x0, y0, a    (A += 0.7071 * SL rounded) */
    0x0E4000u,            /* MOVE x:(r0)+, x0  (Read SR MixBin 3) */
    0x217820u,            /* MACR x0, y0, b    (B += 0.7071 * SR rounded) */

    /* Output Writeback to AC'97 Left/Right (2 DWORDs) */
    0x0E4900u,            /* MOVE a, y:(r1)+   (Write Y:$FFB0 Left) */
    0x0E4D00u,            /* MOVE b, y:(r1)-   (Write Y:$FFB1 Right) */

    /* P:$0054: Loop back to WAIT (2 DWORDs) */
    0x0AF080u, 0x000042u  /* JMP $0042 */
};

/*
 * ----------------------------------------------------------------------------
 * Global Processor Control & Loader Helpers
 * ----------------------------------------------------------------------------
 */

/**
 * Load microcode binary into GP DSP Program RAM (PRAM).
 * Halts the DSP core, writes instructions as 32-bit DWORDs, and returns 0.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param ucode Pointer to microcode DWORD array.
 * @param dword_count Number of DWORDs to upload.
 * @return 0 on success, negative value on invalid parameters.
 */
static inline int apu_gp_load_microcode(uintptr_t apu_base, const uint32_t *ucode, size_t dword_count)
{
    size_t i;

    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }
    if (ucode == NULL) {
        return -1;
    }
    if (dword_count > (NV_PAPU_GPDSP_PRAM_SIZE / sizeof(uint32_t))) {
        return -1;
    }

    /* Halt the GP DSP before modifying PRAM */
    apu_write32(apu_base, NV_PAPU_GPDSP_CONTROL, NV_PAPU_GPDSP_CONTROL_HALT);

    /* Write 24-bit instructions mapped as 32-bit DWORDs into PRAM */
    for (i = 0; i < dword_count; ++i) {
        apu_write32(apu_base, NV_PAPU_GPDSP_PRAM + (uint32_t)(i * sizeof(uint32_t)), ucode[i]);
    }

    return 0;
}

/**
 * Start GP DSP core execution.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return 0 on success.
 */
static inline int apu_gp_start(uintptr_t apu_base)
{
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }
    apu_write32(apu_base, NV_PAPU_GPDSP_CONTROL, NV_PAPU_GPDSP_CONTROL_RUN);
    return 0;
}

/**
 * Stop/halt GP DSP core execution.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return 0 on success.
 */
static inline int apu_gp_stop(uintptr_t apu_base)
{
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }
    apu_write32(apu_base, NV_PAPU_GPDSP_CONTROL, NV_PAPU_GPDSP_CONTROL_HALT);
    return 0;
}

/**
 * Check if the GP DSP core is currently running.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return 1 if running, 0 if halted.
 */
static inline int apu_gp_is_running(uintptr_t apu_base)
{
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }
    return (apu_read32(apu_base, NV_PAPU_GPDSP_STATUS) & NV_PAPU_GPDSP_STATUS_RUNNING) ? 1 : 0;
}

/**
 * Convenience helper to load microcode using default NV_PAPU_BASE.
 *
 * @param ucode Pointer to microcode DWORD array.
 * @param dword_count Number of DWORDs to upload.
 * @return 0 on success, negative value on error.
 */
static inline int apu_load_microcode(const uint32_t *ucode, size_t dword_count)
{
    return apu_gp_load_microcode(NV_PAPU_BASE, ucode, dword_count);
}

/**
 * Load microcode binary and configure Output Processor (EP) FIFO corresponding
 * to target audio topology (Stereo 2.0 or Surround 5.1).
 *
 * - APU_TOPOLOGY_SURROUND_51: loads gp_passthrough_51_bin (81 DWORDs),
 *   sets NV_PAPU_EP_FIFO_CONFIG to 0x3F (NV_PAPU_EP_FIFO_CONFIG_SURROUND).
 * - APU_TOPOLOGY_STEREO_20: loads gp_stereo_downmix_bin (86 DWORDs),
 *   sets NV_PAPU_EP_FIFO_CONFIG to 0x03 (NV_PAPU_EP_FIFO_CONFIG_STEREO).
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param topology Target topology (APU_TOPOLOGY_STEREO_20 or APU_TOPOLOGY_SURROUND_51).
 * @return 0 on success, negative value on error.
 */
static inline int apu_gp_load_topology_microcode(uintptr_t apu_base, APU_AUDIO_TOPOLOGY topology)
{
    if (apu_base == 0) {
        apu_base = NV_PAPU_BASE;
    }

    if (topology == APU_TOPOLOGY_SURROUND_51) {
        int res = apu_gp_load_microcode(apu_base, gp_passthrough_51_bin, GP_PASSTHROUGH_51_SIZE);
        if (res != 0) {
            return res;
        }
        apu_write32(apu_base, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_SURROUND);
        return 0;
    } else if (topology == APU_TOPOLOGY_STEREO_20) {
        int res = apu_gp_load_microcode(apu_base, gp_stereo_downmix_bin, GP_STEREO_DOWNMIX_SIZE);
        if (res != 0) {
            return res;
        }
        apu_write32(apu_base, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_STEREO);
        return 0;
    }

    return -1;
}

/*
 * ----------------------------------------------------------------------------
 * DSP56300 ITU-R BS.775 Reference Arithmetic & Downmix Simulation
 * ----------------------------------------------------------------------------
 */
#define APU_DSP56K_Q23_FACTOR_3DB        0x5A827Au
#define APU_DSP56K_SAMPLE_MAX_24         8388607
#define APU_DSP56K_SAMPLE_MIN_24        (-8388608)

/**
 * 24-bit fixed-point Q23 multiplication with symmetric rounding.
 * Simulates Motorola DSP56300 MACR / MPY scaling.
 *
 * @param val 24-bit signed sample value [-8388608, 8388607].
 * @param coeff 24-bit unsigned/signed Q23 coefficient (e.g. 0x5A827A).
 * @return 24-bit scaled integer.
 */
static inline int32_t apu_dsp56k_q23_mul(int32_t val, int32_t coeff)
{
    int64_t prod = (int64_t)val * (int64_t)coeff;
    int64_t round_const = 1LL << 22;
    int64_t res;
    if (prod >= 0) {
        res = (prod + round_const) >> 23;
    } else {
        res = -((-prod + round_const) >> 23);
    }
    return (int32_t)res;
}

/**
 * Saturate 64-bit value to 24-bit signed range [-8388608, 8388607].
 * Simulates Motorola DSP56300 Saturation Mode enabled via ORI #$001000, SR.
 *
 * @param val 64-bit accumulator value.
 * @return 24-bit saturated integer.
 */
static inline int32_t apu_dsp56k_saturate24(int64_t val)
{
    if (val > (int64_t)APU_DSP56K_SAMPLE_MAX_24) {
        return APU_DSP56K_SAMPLE_MAX_24;
    }
    if (val < (int64_t)APU_DSP56K_SAMPLE_MIN_24) {
        return APU_DSP56K_SAMPLE_MIN_24;
    }
    return (int32_t)val;
}

/**
 * DSP56300 ITU-R BS.775 5.1 downmix reference function for host simulation.
 *
 * Mathematical equations:
 *   Left  = FL + 0.70710678 * Center + 0.70710678 * SL
 *   Right = FR + 0.70710678 * Center + 0.70710678 * SR
 *
 * Coefficients:
 *   Q23 factor 0x5A827A (round(0.70710678 * 8388608)).
 *
 * Channel mapping:
 *   mixbins[0] = Front Left  (FL)
 *   mixbins[1] = Front Right (FR)
 *   mixbins[2] = Surround Left (SL)
 *   mixbins[3] = Surround Right (SR)
 *   mixbins[4] = Center (C)
 *   mixbins[5] = LFE (Subwoofer - bypassed in stereo downmix)
 *
 * @param mixbins Array of 6 discrete 24-bit signed input samples [-8388608, 8388607].
 * @param out_left Destination pointer for saturated 24-bit Left output.
 * @param out_right Destination pointer for saturated 24-bit Right output.
 */
static inline void apu_dsp56k_itur_downmix(const int32_t mixbins[6], int32_t *out_left, int32_t *out_right)
{
    if (mixbins == NULL) {
        if (out_left != NULL) *out_left = 0;
        if (out_right != NULL) *out_right = 0;
        return;
    }

    int32_t fl = mixbins[0];
    int32_t fr = mixbins[1];
    int32_t sl = mixbins[2];
    int32_t sr = mixbins[3];
    int32_t c  = mixbins[4];
    /* mixbins[5] is LFE, bypassed in ITU-R BS.775 stereo downmix */

    int32_t c_scaled  = apu_dsp56k_q23_mul(c, (int32_t)APU_DSP56K_Q23_FACTOR_3DB);
    int32_t sl_scaled = apu_dsp56k_q23_mul(sl, (int32_t)APU_DSP56K_Q23_FACTOR_3DB);
    int32_t sr_scaled = apu_dsp56k_q23_mul(sr, (int32_t)APU_DSP56K_Q23_FACTOR_3DB);

    int64_t left_acc  = (int64_t)fl + (int64_t)c_scaled + (int64_t)sl_scaled;
    int64_t right_acc = (int64_t)fr + (int64_t)c_scaled + (int64_t)sr_scaled;

    if (out_left != NULL) {
        *out_left = apu_dsp56k_saturate24(left_acc);
    }
    if (out_right != NULL) {
        *out_right = apu_dsp56k_saturate24(right_acc);
    }
}

#ifdef __cplusplus
}
#endif

#endif /* APU_GP_UCODE_H */
