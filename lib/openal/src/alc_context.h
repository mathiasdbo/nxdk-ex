#ifndef ALC_CONTEXT_H
#define ALC_CONTEXT_H

#include <AL/alc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "apu_eeprom.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALC_XBOX_TOPOLOGY
#define ALC_XBOX_TOPOLOGY               0x1014
#endif

/*
 * ============================================================================
 * Internal ALC Device & Context Structures
 * ============================================================================
 */

/**
 * Internal OpenAL 1.1 Device Object.
 */
struct ALCdevice_struct {
    bool is_open;                       /**< True if hardware device is active */
    ALCenum last_error;                 /**< Last device-level error code */
    char name[64];                      /**< Device specifier string */
    void *voice_table_virt;             /**< 4KB-aligned 8KB Voice Context Array */
    uint32_t voice_table_phys;          /**< 32-bit physical RAM address of voice array */
    uint32_t context_count;             /**< Number of active contexts created on device */
    ALCcontext *active_context;         /**< Pointer to currently active context (if any) */
    uintptr_t apu_base;                 /**< MMIO base address (NV_PAPU_BASE, null-backend RAM or mock) */
    APU_AUDIO_TOPOLOGY topology;        /**< Active audio output topology (Stereo 2.0 or 5.1) */
};

/**
 * Internal OpenAL 1.1 Context Object.
 */
struct ALCcontext_struct {
    bool is_valid;                      /**< True if context is active and valid */
    ALCdevice *device;                  /**< Parent ALC device handle */
    ALCint frequency;                   /**< Mixing frequency (default: 48000 Hz) */
    ALCint refresh;                     /**< Refresh rate in Hz (default: 60 Hz) */
    ALCint sync;                        /**< Synchronous context flag (ALC_FALSE) */
    ALCint mono_sources;                /**< Mono source hints (default: 64) */
    ALCint stereo_sources;              /**< Stereo source hints (default: 32) */
};

/*
 * ============================================================================
 * Testing & Hardware Mock Configuration API
 * ============================================================================
 */

/**
 * Override the APU base MMIO address used by the ALC device subsystem.
 * Enables host testing with mock MMIO buffers without hardware faults.
 *
 * With the default base (NV_PAPU_BASE) alcOpenDevice() runs on a zeroed
 * BAR0-sized RAM region (null backend) instead of the real registers, unless
 * the library is built with -DOPENAL_APU_REAL_MMIO. See alext.h
 * (AL_XBOX_BACKEND) and lib/openal/docs/XEMU_VERIFICATION.md.
 *
 * @param base Base MMIO address, or 0 to restore default NV_PAPU_BASE.
 */
void alc_set_apu_base(uintptr_t base);

/**
 * Retrieve the active APU base MMIO address.
 *
 * While a device is open this is the base the hardware session actually
 * uses (for the null backend: the RAM region); otherwise the configured base.
 */
uintptr_t alc_get_apu_base(void);

#ifdef __cplusplus
}
#endif

#endif /* ALC_CONTEXT_H */
