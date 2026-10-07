/*
 * SMC AV pack decode test (apu_av_pack_from_smc and the decisions built on it).
 *
 * The Xbox SMC returns RAW pack codes from register 0x04 that differ from the
 * library's APU_AV_PACK_* enum (which mirrors the kernel AV_PACK_* enum). The
 * raw values below are taken from xemu hw/xbox/smbus_xbox_smc.c:
 *   - register map SMC_REG_AVPACK / SMC_REG_AVPACK_*  lines 66-73
 *       SCART 0x00, HDTV 0x01, VGA 0x02, RFU 0x03, SVIDEO 0x04,
 *       COMPOSITE 0x06, NONE 0x07
 *   - register read (one byte)                         smc_receive_byte, line 185
 *   - SMC slave at 7-bit address 0x10 (8-bit 0x20)     hw/xbox/xbox.c:300
 * See lib/openal/docs/XEMU_VERIFICATION.md.
 *
 * The target path of apu_query_smbus_av_pack() (HalReadSMBusValue) cannot run on
 * the host; it is covered by check_target_syntax.sh at compile level and by this
 * decode function at logic level. Mock av_pack values are already decoded.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "apu_eeprom.h"

typedef struct {
    uint32_t raw;           /* value read from SMC register 0x04 */
    uint32_t decoded;       /* expected APU_AV_PACK_* */
    bool     optical;       /* expected apu_is_optical_pack_connected(decoded) */
    const char *name;
} decode_case_t;

static const decode_case_t k_cases[] = {
    { 0x00, APU_AV_PACK_SCART,    true,  "SCART"      },
    { 0x01, APU_AV_PACK_HDTV,     true,  "HDTV"       },
    { 0x02, APU_AV_PACK_VGA,      false, "VGA"        },
    { 0x03, APU_AV_PACK_RFU,      false, "RFU"        },
    { 0x04, APU_AV_PACK_SVIDEO,   true,  "S-Video"    },
    { 0x05, APU_AV_PACK_NONE,     false, "undefined 5" },
    { 0x06, APU_AV_PACK_STANDARD, false, "composite"  },
    { 0x07, APU_AV_PACK_NONE,     false, "none"       },
};

int main(void) {
    size_t i;
    uint32_t raw;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== SMC AV Pack Raw Code Decode Unit Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Constants match xemu's SMC register map and the kernel enum
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing raw SMC constants and library enum values...\n");
    assert(APU_SMC_AV_PACK_SCART == 0x00u);
    assert(APU_SMC_AV_PACK_HDTV == 0x01u);
    assert(APU_SMC_AV_PACK_VGA == 0x02u);
    assert(APU_SMC_AV_PACK_RFU == 0x03u);
    assert(APU_SMC_AV_PACK_SVIDEO == 0x04u);
    assert(APU_SMC_AV_PACK_COMPOSITE == 0x06u);
    assert(APU_SMC_AV_PACK_NONE == 0x07u);

    assert(APU_AV_PACK_NONE == 0u);
    assert(APU_AV_PACK_STANDARD == 1u);
    assert(APU_AV_PACK_RFU == 2u);
    assert(APU_AV_PACK_SCART == 3u);
    assert(APU_AV_PACK_HDTV == 4u);
    assert(APU_AV_PACK_VGA == 5u);
    assert(APU_AV_PACK_SVIDEO == 6u);
    printf("    -> Raw codes and decoded identifiers are distinct numbering schemes [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: Raw codes 0..7 decode to the library enum
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing decode of raw SMC codes 0..7...\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        uint32_t got = apu_av_pack_from_smc(k_cases[i].raw);
        if (got != k_cases[i].decoded) {
            fprintf(stderr, "FAIL: raw 0x%02X (%s) decoded to %u, expected %u\n",
                    (unsigned)k_cases[i].raw, k_cases[i].name,
                    (unsigned)got, (unsigned)k_cases[i].decoded);
            return 1;
        }
        assert(apu_is_optical_pack_connected(got) == k_cases[i].optical);
    }
    printf("    -> SCART/HDTV/VGA/RFU/S-Video/composite/none decode correctly [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: Unknown values and the low-8-bit policy
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing unknown raw values and byte masking...\n");
    /* Codes the SMC never reports decode to "no pack" (not optical) */
    assert(apu_av_pack_from_smc(0x05u) == APU_AV_PACK_NONE);
    assert(apu_av_pack_from_smc(0x08u) == APU_AV_PACK_NONE);
    assert(apu_av_pack_from_smc(0x10u) == APU_AV_PACK_NONE);
    assert(apu_av_pack_from_smc(0x80u) == APU_AV_PACK_NONE);
    assert(apu_av_pack_from_smc(0xFFu) == APU_AV_PACK_NONE);
    /*
     * The SMC register is one byte, so only the low 8 bits are meaningful:
     * 0x106 decodes like 0x06 (composite), 0x101 like HDTV, 0x100 like SCART.
     * (HalReadSMBusValue with ReadWordValue=FALSE never fills the upper bits.)
     */
    assert(apu_av_pack_from_smc(0x106u) == APU_AV_PACK_STANDARD);
    assert(apu_av_pack_from_smc(0x101u) == APU_AV_PACK_HDTV);
    assert(apu_av_pack_from_smc(0x100u) == APU_AV_PACK_SCART);
    assert(apu_av_pack_from_smc(0xFFFFFF00u) == APU_AV_PACK_SCART);
    assert(apu_av_pack_from_smc(0xFFFFFF05u) == APU_AV_PACK_NONE);
    assert(apu_av_pack_from_smc(0xFFFFFFFFu) == APU_AV_PACK_NONE);
    assert(!apu_is_optical_pack_connected(apu_av_pack_from_smc(0x08u)));
    assert(!apu_is_optical_pack_connected(apu_av_pack_from_smc(0xFFu)));

    /* Exhaustive over one byte: the result is always a valid enum, exactly the six
     * defined pack codes decode to a real pack, and upper bits never change it. */
    {
        unsigned non_none = 0;
        for (raw = 0; raw <= 0xFFu; raw++) {
            uint32_t d = apu_av_pack_from_smc(raw);
            assert(d <= APU_AV_PACK_SVIDEO);
            if (d != APU_AV_PACK_NONE) {
                non_none++;
            }
            assert(apu_av_pack_from_smc(raw | 0x100u) == d);
            assert(apu_av_pack_from_smc(raw | 0xABCD0000u) == d);
        }
        /* SCART, HDTV, VGA, RFU, S-Video, composite (raw 0x07 decodes to NONE) */
        assert(non_none == 6);
    }
    printf("    -> Unknown codes -> NONE, only the low byte is decoded [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: Optical and topology decisions driven from raw codes via the mock
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing optical/topology decisions for every raw code...\n");
    for (i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        uint32_t decoded = apu_av_pack_from_smc(k_cases[i].raw);

        /* AC-3 enabled: 5.1 only on optical packs */
        apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, decoded);
        assert(apu_query_smbus_av_pack() == decoded);
        assert(apu_is_optical_pack_connected(apu_query_smbus_av_pack()) == k_cases[i].optical);
        assert(apu_detect_audio_topology() ==
               (k_cases[i].optical ? APU_TOPOLOGY_SURROUND_51 : APU_TOPOLOGY_STEREO_20));

        /* AC-3 disabled: always stereo */
        apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, decoded);
        assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    }

    /*
     * Regressions for the raw-equals-enum confusion: feeding raw SMC codes straight
     * into the enum comparison gave composite (0x06) as optical S-Video, HDTV (0x01)
     * as a plain pack and SCART (0x00) as "none".
     */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, apu_av_pack_from_smc(0x06u));
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, apu_av_pack_from_smc(0x01u));
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, apu_av_pack_from_smc(0x00u));
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);
    /* xemu's default pack is HDTV (raw 0x01) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, apu_av_pack_from_smc(APU_SMC_AV_PACK_HDTV));
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);
    printf("    -> Optical packs (SCART/HDTV/S-Video) enable 5.1 with AC-3, all others stay stereo [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: Mock semantics stay "already decoded"
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing that mock AV pack values are not decoded again...\n");
    /* 0x01 as a mock value is APU_AV_PACK_STANDARD; decoding it as raw would give HDTV (4) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, 0x01u);
    assert(apu_query_smbus_av_pack() == APU_AV_PACK_STANDARD);
    assert(apu_av_pack_from_smc(0x01u) == APU_AV_PACK_HDTV);
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_SVIDEO);
    assert(apu_query_smbus_av_pack() == APU_AV_PACK_SVIDEO);
    printf("    -> Mock returns the configured identifier verbatim [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 6: Unmocked host fallback is unchanged
     * ------------------------------------------------------------------------
     */
    printf("[6] Testing unmocked host fallback...\n");
    apu_eeprom_clear_mock();
    assert(apu_query_smbus_av_pack() == APU_AV_PACK_STANDARD);
    assert(!apu_is_optical_pack_connected(apu_query_smbus_av_pack()));
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    printf("    -> Host fallback is the standard (non-optical) pack [PASS]\n");

    printf("=== All SMC AV Pack Decode Tests Passed Successfully! ===\n");
    return 0;
}
