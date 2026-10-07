#include "apu_platform.h"
#include <string.h>

/* Kernel header first so apu_eeprom.h's XC_AUDIO guard sees the kernel's definition */
#ifdef OPENAL_TARGET_XBOX
#include <xboxkrnl/xboxkrnl.h>
#endif
#include "apu_eeprom.h"

/*
 * ============================================================================
 * Internal Mock State for Unit Testing
 * ============================================================================
 */
static bool s_mock_enabled = false;
static uint32_t s_mock_flags = 0;
static uint32_t s_mock_av_pack = 0;

/*
 * ============================================================================
 * Hardware Query Functions
 * ============================================================================
 */

uint32_t apu_query_eeprom_audio_flags(void) {
    if (s_mock_enabled) {
        return s_mock_flags;
    }

#ifdef OPENAL_TARGET_XBOX
    ULONG type = 0;
    ULONG flags = 0;
    ULONG res_len = 0;
    NTSTATUS status = ExQueryNonVolatileSetting(XC_AUDIO, &type, &flags, (ULONG)sizeof(flags), &res_len);
    if (NT_SUCCESS(status)) {
        return (uint32_t)flags;
    }
    return XC_AUDIO_FLAGS_STEREO;
#else
    return XC_AUDIO_FLAGS_STEREO;
#endif
}

/*
 * SMC register 0x04 returns raw pack codes: xemu hw/xbox/smbus_xbox_smc.c
 * lines 66-73 (SMC_REG_AVPACK_*), served by smc_receive_byte() at line 185.
 * The SMC sits at 7-bit I2C address 0x10 (hw/xbox/xbox.c:300), i.e. 0x20 as the
 * 8-bit SlaveAddress passed to HalReadSMBusValue. See lib/openal/docs/XEMU_VERIFICATION.md.
 */
uint32_t apu_av_pack_from_smc(uint32_t raw_smc) {
    switch (raw_smc & 0xFFu) {
    case APU_SMC_AV_PACK_SCART:     return APU_AV_PACK_SCART;
    case APU_SMC_AV_PACK_HDTV:      return APU_AV_PACK_HDTV;
    case APU_SMC_AV_PACK_VGA:       return APU_AV_PACK_VGA;
    case APU_SMC_AV_PACK_RFU:       return APU_AV_PACK_RFU;
    case APU_SMC_AV_PACK_SVIDEO:    return APU_AV_PACK_SVIDEO;
    case APU_SMC_AV_PACK_COMPOSITE: return APU_AV_PACK_STANDARD;
    case APU_SMC_AV_PACK_NONE:      return APU_AV_PACK_NONE;
    default:                        return APU_AV_PACK_NONE;
    }
}

uint32_t apu_query_smbus_av_pack(void) {
    if (s_mock_enabled) {
        /* Mock values are already decoded APU_AV_PACK_* identifiers */
        return s_mock_av_pack;
    }

#ifdef OPENAL_TARGET_XBOX
    ULONG pack_val = 0;
    NTSTATUS status = HalReadSMBusValue(0x20, 0x04, FALSE, &pack_val);
    if (NT_SUCCESS(status)) {
        return apu_av_pack_from_smc((uint32_t)pack_val);
    }
    /* SMBus failure: assume a standard (non-optical) pack so topology stays stereo */
    return APU_AV_PACK_STANDARD;
#else
    return APU_AV_PACK_STANDARD;
#endif
}

bool apu_is_optical_pack_connected(uint32_t av_pack) {
    /* Which packs expose TOSLink is an UNVERIFIED assumption (xemu is silent on it) */
    return (av_pack == APU_AV_PACK_HDTV ||
            av_pack == APU_AV_PACK_SVIDEO ||
            av_pack == APU_AV_PACK_SCART);
}

APU_AUDIO_TOPOLOGY apu_detect_audio_topology(void) {
    uint32_t flags = apu_query_eeprom_audio_flags();
    uint32_t pack = apu_query_smbus_av_pack();

    if (apu_is_optical_pack_connected(pack) && ((flags & XC_AUDIO_FLAGS_ENABLE_AC3) != 0)) {
        return APU_TOPOLOGY_SURROUND_51;
    }

    return APU_TOPOLOGY_STEREO_20;
}

/*
 * ============================================================================
 * Mock Configuration APIs
 * ============================================================================
 */

void apu_eeprom_set_mock(bool enable, uint32_t audio_flags, uint32_t av_pack) {
    s_mock_enabled = enable;
    s_mock_flags = audio_flags;
    s_mock_av_pack = av_pack;
}

void apu_eeprom_clear_mock(void) {
    s_mock_enabled = false;
    s_mock_flags = 0;
    s_mock_av_pack = 0;
}
