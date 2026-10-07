#ifndef APU_EP_H
#define APU_EP_H

#include <stdint.h>
#include <stdbool.h>
#include "apu_hardware.h"
#include "apu_eeprom.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox MCPX APU Output Processor (EP) Subsystem Definitions
 * ============================================================================
 * The Output Processor (EP) mixes and routes the 6 GP FIFO channels to:
 * - AC'97 Codec (Stereo Front Left / Front Right analog output)
 * - Dolby Digital DSE Realtime Interactive Encoder (5.1 AC-3 TOSLink optical S/PDIF)
 *
 * NOTE: this register layout is the library's own model and is not
 * xemu-conformant (see lib/openal/docs/XEMU_VERIFICATION.md). By default alcOpenDevice()
 * applies it to a RAM stand-in, not to the real BAR0 (AL_XBOX_BACKEND).
 * alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE) does not read it.
 *
 * Register configuration:
 * - NV_PAPU_EP_FIFO_CONFIG (0x3004): 0x03 (Stereo 2.0) or 0x3F (Surround 5.1)
 * - NV_PAPU_EP_FIFO_ROUTE  (0x3010): 0x00543210 (Default 6-channel routing)
 * - NV_PAPU_EP_CONTROL     (0x3000):
 *     Bit 0 (ENABLE):     1 (Output Processor Enable)
 *     Bit 1 (DSE_ENABLE): 1 (Dolby Digital Realtime AC-3 Encoder Enable)
 * ============================================================================
 */

/**
 * Initialize the Output Processor (EP) subsystem for the specified topology.
 *
 * Configures EP FIFO channels, default routing matrix, and hardware
 * Dolby Digital DSE encoder when 5.1 surround topology is requested.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @param topology Target audio topology (APU_TOPOLOGY_STEREO_20 or APU_TOPOLOGY_SURROUND_51).
 * @return 0 on success, negative value on invalid topology or error.
 */
int apu_ep_subsystem_init(uintptr_t apu_base, APU_AUDIO_TOPOLOGY topology);

/**
 * Shut down the Output Processor (EP) subsystem, silencing output.
 *
 * Clears NV_PAPU_EP_CONTROL to 0.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 */
void apu_ep_subsystem_deinit(uintptr_t apu_base);

/**
 * Determine if Dolby Digital Interactive Realtime Encoding (DSE) is active.
 *
 * Checks if NV_PAPU_EP_CONTROL_DSE_ENABLE bit is set in NV_PAPU_EP_CONTROL.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return true if Dolby Digital AC-3 DSE encoder is active, false otherwise.
 */
bool apu_is_dolby_digital_active(uintptr_t apu_base);

/**
 * Retrieve the current EP FIFO channel configuration mask.
 *
 * Reads NV_PAPU_EP_FIFO_CONFIG from APU MMIO.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return 32-bit FIFO configuration mask (e.g. 0x03 or 0x3F).
 */
uint32_t apu_ep_get_fifo_config(uintptr_t apu_base);

/**
 * Retrieve the current EP FIFO routing matrix configuration.
 *
 * Reads NV_PAPU_EP_FIFO_ROUTE from APU MMIO.
 *
 * @param apu_base Base MMIO address of APU (or 0 for default NV_PAPU_BASE).
 * @return 32-bit FIFO routing matrix value (e.g. 0x00543210).
 */
uint32_t apu_ep_get_fifo_route(uintptr_t apu_base);

#ifdef __cplusplus
}
#endif

#endif /* APU_EP_H */
