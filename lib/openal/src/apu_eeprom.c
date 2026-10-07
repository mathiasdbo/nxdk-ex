#include "apu_eeprom.h"
#include <string.h>

#if defined(__NXDK__) || defined(_XBOX)
#include <xboxkrnl/xboxkrnl.h>
#endif

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

#if defined(__NXDK__) || defined(_XBOX)
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

uint32_t apu_query_smbus_av_pack(void) {
    if (s_mock_enabled) {
        return s_mock_av_pack;
    }

#if defined(__NXDK__) || defined(_XBOX)
    ULONG pack_val = 0;
    NTSTATUS status = HalReadSMBusValue(0x20, 0x04, FALSE, &pack_val);
    if (NT_SUCCESS(status)) {
        return (uint32_t)pack_val;
    }
    return APU_AV_PACK_STANDARD;
#else
    return APU_AV_PACK_STANDARD;
#endif
}

bool apu_is_optical_pack_connected(uint32_t av_pack) {
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
