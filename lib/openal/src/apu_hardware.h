#ifndef APU_HARDWARE_H
#define APU_HARDWARE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox MCPX Audio Processing Unit (APU) Hardware Definitions
 * ============================================================================
 * Architecture Overview:
 * - Southbridge PCI device: Bus 0, Device 5, Function 0
 * - Device ID: 0x01B0, Vendor ID: 0x10DE (NVIDIA Corporation)
 * - BAR0 default physical address: 0xFE800000
 *
 * Subsystems:
 * - Voice Processor (VP): 64 3D or 256 2D voices with hardware resampling,
 *   ADSR envelopes, ITD delay buffers, and HRTF biquad filtering.
 * - Global Processor (GP): DSP56300 core mixing 32 MixBins at 48 kHz.
 * - Output Processor (EP): 6-channel FIFO ring routing to AC'97 / Dolby DSE.
 * ============================================================================
 */

/* ----------------------------------------------------------------------------
 * PCI Configuration Identifiers
 * ---------------------------------------------------------------------------- */
#define NV_PAPU_PCI_BUS                 0u
#define NV_PAPU_PCI_DEV                 5u
#define NV_PAPU_PCI_SLOT                5u
#define NV_PAPU_PCI_FUNC                0u
#define NV_PAPU_PCI_VENDOR_ID           0x10DEu
#define NV_PAPU_PCI_DEVICE_ID           0x01B0u

/* Aliases */
#define APU_PCI_BUS                     NV_PAPU_PCI_BUS
#define APU_PCI_DEV                     NV_PAPU_PCI_DEV
#define APU_PCI_FUNC                    NV_PAPU_PCI_FUNC
#define APU_PCI_VENDOR_ID               NV_PAPU_PCI_VENDOR_ID
#define APU_PCI_DEVICE_ID               NV_PAPU_PCI_DEVICE_ID

/* ----------------------------------------------------------------------------
 * Base Addresses and Alignment Constants
 * ---------------------------------------------------------------------------- */
#define NV_PAPU_BASE                    0xFE800000u
#define NV_PAPU_BAR0_DEFAULT            0xFE800000u
#define APU_BAR0_DEFAULT                NV_PAPU_BAR0_DEFAULT

#define NV_PAPU_NUM_3D_VOICES           64u
#define NV_PAPU_NUM_2D_VOICES           256u
#define NV_PAPU_NUM_MIXBINS             32u

#define NV_PAPU_VOICE_CONTEXT_SIZE      128u
#define NV_PAPU_VOICE_ARRAY_ALIGN       4096u
#define NV_PAPU_VOICE_ARRAY_SIZE_3D     (NV_PAPU_NUM_3D_VOICES * NV_PAPU_VOICE_CONTEXT_SIZE) /* 8192 bytes */

/* ----------------------------------------------------------------------------
 * Voice Processor (VP) Registers (0x1000 .. 0x1024)
 * ---------------------------------------------------------------------------- */
#define NV_PAPU_VP_CONTROL              0x00001000u
#define NV_PAPU_VP_BASE_ADDR            0x00001004u
#define NV_PAPU_VP_ACTIVE_0             0x00001010u
#define NV_PAPU_VP_ACTIVE_1             0x00001014u
#define NV_PAPU_VP_PAUSE_0              0x00001020u
#define NV_PAPU_VP_PAUSE_1              0x00001024u

/* NV_PAPU_VP_CONTROL Bitfields */
#define NV_PAPU_VP_CONTROL_RESET        (1u << 0) /* Bit 0: VP Hardware Reset */
#define NV_PAPU_VP_CONTROL_ENABLE       (1u << 1) /* Bit 1: VP Engine Enable */
#define NV_PAPU_VP_CONTROL_MODE_3D      (1u << 2) /* Bit 2: 1 = 64 3D Voices, 0 = 256 2D Voices */
#define NV_PAPU_VP_CONTROL_MODE_2D      (0u << 2)
#define NV_PAPU_VP_CONTROL_MODE_MASK    (1u << 2)

#define NV_PAPU_VP_MODE_64_3D           NV_PAPU_VP_CONTROL_MODE_3D
#define NV_PAPU_VP_MODE_256_2D          NV_PAPU_VP_CONTROL_MODE_2D

/* ----------------------------------------------------------------------------
 * Global Processor (GP DSP) Registers (0x2000 .. 0x20000)
 * ---------------------------------------------------------------------------- */
#define NV_PAPU_GPDSP_CONTROL           0x00002000u
#define NV_PAPU_GPDSP_STATUS            0x00002004u
#define NV_PAPU_GPDSP_PRAM              0x00020000u

/* NV_PAPU_GPDSP_CONTROL Bitfields */
#define NV_PAPU_GPDSP_CONTROL_RUN       (1u << 0) /* Bit 0: DSP Run/Halt (1 = Run, 0 = Halt) */
#define NV_PAPU_GPDSP_CONTROL_HALT      (0u << 0)
#define NV_PAPU_GPDSP_CONTROL_FRAME_IRQ (1u << 2) /* Bit 2: 48 kHz Frame Tick Sync Interrupt Enable */

/* NV_PAPU_GPDSP_STATUS Bitfields */
#define NV_PAPU_GPDSP_STATUS_RUNNING    (1u << 0) /* Bit 0: DSP Execution Status */
#define NV_PAPU_GPDSP_STATUS_FRAME_SYNC (1u << 2) /* Bit 2: Frame Boundary Sync Flag */

/* GP DSP Program RAM capacity */
#define NV_PAPU_GPDSP_PRAM_SIZE         (2048u * 4u) /* 2048 x 24-bit instructions in DWORDs (8KB) */

/* ----------------------------------------------------------------------------
 * Output Processor (EP) Registers (0x3000 .. 0x3010)
 * ---------------------------------------------------------------------------- */
#define NV_PAPU_EP_CONTROL              0x00003000u
#define NV_PAPU_EP_FIFO_CONFIG          0x00003004u
#define NV_PAPU_EP_FIFO_ROUTE           0x00003010u

/* NV_PAPU_EP_CONTROL Bitfields */
#define NV_PAPU_EP_CONTROL_ENABLE       (1u << 0) /* Bit 0: Output Processor Enable */
#define NV_PAPU_EP_CONTROL_DSE_ENABLE   (1u << 1) /* Bit 1: Hardware Dolby Digital DSE Realtime Encoder Enable */
#define NV_PAPU_EP_CONTROL_AC3_ENABLE   NV_PAPU_EP_CONTROL_DSE_ENABLE

/* NV_PAPU_EP_FIFO_CONFIG Channel Masks */
#define NV_PAPU_EP_FIFO_CONFIG_STEREO   0x03u     /* Channels 0-1 (Front Left, Front Right) */
#define NV_PAPU_EP_FIFO_CONFIG_SURROUND 0x3Fu     /* Channels 0-5 (5.1 Surround Sound) */
#define NV_PAPU_EP_FIFO_CONFIG_5_1      0x3Fu

/* NV_PAPU_EP_FIFO_ROUTE Channel Routing Identifiers */
#define NV_PAPU_EP_ROUTE_FL             0x0u      /* Front Left */
#define NV_PAPU_EP_ROUTE_FR             0x1u      /* Front Right */
#define NV_PAPU_EP_ROUTE_SL             0x2u      /* Surround Left */
#define NV_PAPU_EP_ROUTE_SR             0x3u      /* Surround Right */
#define NV_PAPU_EP_ROUTE_CENTER         0x4u      /* Center */
#define NV_PAPU_EP_ROUTE_LFE            0x5u      /* Low-Frequency Effects / Subwoofer */

/* Routing Nibble Helpers: Each channel destination occupies a 4-bit nibble */
#define NV_PAPU_EP_ROUTE_NIBBLE(ch, route) \
    (((uint32_t)(route) & 0x0Fu) << ((uint32_t)(ch) * 4u))

#define NV_PAPU_EP_ROUTE_GET(reg, ch) \
    (((uint32_t)(reg) >> ((uint32_t)(ch) * 4u)) & 0x0Fu)

#define NV_PAPU_EP_ROUTE_SET(reg, ch, route) \
    (((uint32_t)(reg) & ~(0x0Fu << ((uint32_t)(ch) * 4u))) | (((uint32_t)(route) & 0x0Fu) << ((uint32_t)(ch) * 4u)))

#define NV_PAPU_EP_ROUTE_PACK_5_1(fl, fr, sl, sr, c, lfe) \
    ( NV_PAPU_EP_ROUTE_NIBBLE(0, (fl))  | \
      NV_PAPU_EP_ROUTE_NIBBLE(1, (fr))  | \
      NV_PAPU_EP_ROUTE_NIBBLE(2, (sl))  | \
      NV_PAPU_EP_ROUTE_NIBBLE(3, (sr))  | \
      NV_PAPU_EP_ROUTE_NIBBLE(4, (c))   | \
      NV_PAPU_EP_ROUTE_NIBBLE(5, (lfe)) )

#define NV_PAPU_EP_ROUTE_DEFAULT \
    NV_PAPU_EP_ROUTE_PACK_5_1(NV_PAPU_EP_ROUTE_FL, \
                              NV_PAPU_EP_ROUTE_FR, \
                              NV_PAPU_EP_ROUTE_SL, \
                              NV_PAPU_EP_ROUTE_SR, \
                              NV_PAPU_EP_ROUTE_CENTER, \
                              NV_PAPU_EP_ROUTE_LFE)

/* ----------------------------------------------------------------------------
 * MMIO Volatile Access Macros and Helpers
 * ---------------------------------------------------------------------------- */

/**
 * Access a 32-bit APU MMIO register given a base pointer/address and byte offset.
 */
#define APU_REG32(base, offset) (*(volatile uint32_t *)((uintptr_t)(base) + (uintptr_t)(offset)))

/**
 * Direct access to default APU BAR0 physical MMIO register.
 */
#define APU_REG(offset) APU_REG32(NV_PAPU_BASE, (offset))

/**
 * Macro helpers for reading and writing 32-bit MMIO registers.
 */
#define APU_READ32(base, offset) \
    (APU_REG32((base), (offset)))

#define APU_WRITE32(base, offset, val) \
    do { APU_REG32((base), (offset)) = (uint32_t)(val); } while (0)

#if defined(__GNUC__) || defined(__clang__)
#define APU_INLINE static inline __attribute__((unused))
#else
#define APU_INLINE static inline
#endif

/**
 * Type-safe inline helper to read a 32-bit register.
 */
APU_INLINE uint32_t apu_read32(uintptr_t base, uint32_t offset) {
    return APU_REG32(base, offset);
}

/**
 * Type-safe inline helper to write a 32-bit register.
 */
APU_INLINE void apu_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    APU_REG32(base, offset) = value;
}

#ifdef __cplusplus
}
#endif

#endif /* APU_HARDWARE_H */
