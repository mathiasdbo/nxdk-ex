#ifndef APU_GP_UCODE_H
#define APU_GP_UCODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "apu_hardware.h"

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

#ifdef __cplusplus
}
#endif

#endif /* APU_GP_UCODE_H */
