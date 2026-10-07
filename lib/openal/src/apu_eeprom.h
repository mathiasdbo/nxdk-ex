#ifndef APU_EEPROM_H
#define APU_EEPROM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================================
 * Xbox Hardware AV Pack Identifiers (PIC SMBus Slave 0x20, Command 0x04)
 * ============================================================================
 * The Xbox System Management Controller (SMC PIC) samples the AV pack ID
 * pins at boot and runtime.
 * Optical-capable packs feature an integrated TOSLink port:
 * - APU_AV_PACK_SCART  (Advanced SCART AV Pack - Europe)
 * - APU_AV_PACK_HDTV   (High Definition AV Pack - Component YPbPr + Optical)
 * - APU_AV_PACK_SVIDEO (Advanced AV Pack - S-Video + Composite + Optical)
 * ============================================================================
 */
#define APU_AV_PACK_NONE        0x00u
#define APU_AV_PACK_STANDARD    0x01u
#define APU_AV_PACK_RFU         0x02u
#define APU_AV_PACK_SCART       0x03u
#define APU_AV_PACK_HDTV        0x04u
#define APU_AV_PACK_VGA         0x05u
#define APU_AV_PACK_SVIDEO      0x06u

/*
 * ============================================================================
 * Xbox EEPROM Non-Volatile Audio Settings Flags
 * ============================================================================
 * Queried via ExQueryNonVolatileSetting(XC_AUDIO, ...).
 * Configured by user in MS Dashboard / Audio Settings.
 * ============================================================================
 */
#ifndef XC_AUDIO
#define XC_AUDIO                0x0009u
#endif

#ifndef XC_AUDIO_FLAGS_MONO
#define XC_AUDIO_FLAGS_MONO         0x00000000u
#endif

#ifndef XC_AUDIO_FLAGS_STEREO
#define XC_AUDIO_FLAGS_STEREO       0x00000001u
#endif

#ifndef XC_AUDIO_FLAGS_ENABLE_AC3
#define XC_AUDIO_FLAGS_ENABLE_AC3   0x00010000u
#endif

#ifndef XC_AUDIO_FLAGS_ENABLE_DTS
#define XC_AUDIO_FLAGS_ENABLE_DTS   0x00020000u
#endif

#ifndef XC_AUDIO_FLAGS_SURROUND
#define XC_AUDIO_FLAGS_SURROUND     0x00040000u
#endif

/*
 * ============================================================================
 * Audio Output Topology
 * ============================================================================
 * APU_TOPOLOGY_STEREO_20:
 *   Downmix 6 discrete mixbins to 2 output FIFO channels (FL, FR) via GP DSP
 *   microcode and route to AC'97 analog codec.
 *
 * APU_TOPOLOGY_SURROUND_51:
 *   Transfer 6 discrete mixbins directly to 6 output FIFO channels (FL, FR,
 *   SL, SR, C, LFE) and feed the Dolby Digital Interactive (DSE) realtime
 *   encoder for TOSLink optical S/PDIF output.
 * ============================================================================
 */
typedef enum APU_AUDIO_TOPOLOGY {
    APU_TOPOLOGY_STEREO_20 = 0,
    APU_TOPOLOGY_SURROUND_51 = 1
} APU_AUDIO_TOPOLOGY;

/*
 * ============================================================================
 * Query and Topology Detection APIs
 * ============================================================================
 */

/**
 * Query user-configured non-volatile audio flags from Xbox EEPROM.
 *
 * On Xbox hardware, invokes ExQueryNonVolatileSetting(XC_AUDIO).
 * If uninitialized or failure occurs, falls back to XC_AUDIO_FLAGS_STEREO.
 *
 * @return Bitmask of XC_AUDIO_FLAGS_* settings.
 */
uint32_t apu_query_eeprom_audio_flags(void);

/**
 * Query connected AV Pack type from the System Management Controller (PIC).
 *
 * On Xbox hardware, reads register 0x04 from SMBus PIC slave 0x20 via
 * HalReadSMBusValue. Falls back to APU_AV_PACK_STANDARD on failure.
 *
 * @return AV Pack identifier (APU_AV_PACK_*).
 */
uint32_t apu_query_smbus_av_pack(void);

/**
 * Determine if the specified AV Pack type has TOSLink optical digital output.
 *
 * Optical packs include HDTV, Advanced S-Video, and Advanced SCART.
 *
 * @param av_pack AV pack identifier (APU_AV_PACK_*).
 * @return true if AV pack supports optical S/PDIF output, false otherwise.
 */
bool apu_is_optical_pack_connected(uint32_t av_pack);

/**
 * Automatically resolve target audio topology based on EEPROM audio flags
 * and connected AV pack capabilities.
 *
 * Returns APU_TOPOLOGY_SURROUND_51 only if an optical AV pack is plugged in
 * AND the user enabled Dolby Digital (XC_AUDIO_FLAGS_ENABLE_AC3) in dashboard.
 * Otherwise, resolves to APU_TOPOLOGY_STEREO_20.
 *
 * @return Resolved topology (APU_TOPOLOGY_STEREO_20 or APU_TOPOLOGY_SURROUND_51).
 */
APU_AUDIO_TOPOLOGY apu_detect_audio_topology(void);

/*
 * ============================================================================
 * Mock Configuration APIs for Unit Testing
 * ============================================================================
 */

/**
 * Override EEPROM flags and SMBus AV Pack readings for host unit testing.
 *
 * @param enable If true, mock values are returned by query functions.
 * @param audio_flags Simulated EEPROM flags bitmask.
 * @param av_pack Simulated SMBus AV pack identifier.
 */
void apu_eeprom_set_mock(bool enable, uint32_t audio_flags, uint32_t av_pack);

/**
 * Clear mock state and return to hardware/default query mode.
 */
void apu_eeprom_clear_mock(void);

#ifdef __cplusplus
}
#endif

#endif /* APU_EEPROM_H */
