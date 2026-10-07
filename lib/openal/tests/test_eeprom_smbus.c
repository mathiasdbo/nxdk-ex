#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include "apu_eeprom.h"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Xbox EEPROM Audio Flags & SMBus AV Pack Detection Unit Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Optical AV Pack Identification
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing AV Pack optical port detection (apu_is_optical_pack_connected)...\n");
    assert(!apu_is_optical_pack_connected(APU_AV_PACK_NONE));
    assert(!apu_is_optical_pack_connected(APU_AV_PACK_STANDARD));
    assert(!apu_is_optical_pack_connected(APU_AV_PACK_RFU));
    assert(apu_is_optical_pack_connected(APU_AV_PACK_SCART));
    assert(apu_is_optical_pack_connected(APU_AV_PACK_HDTV));
    assert(!apu_is_optical_pack_connected(APU_AV_PACK_VGA));
    assert(apu_is_optical_pack_connected(APU_AV_PACK_SVIDEO));

    /* Out of bounds / invalid AV Pack IDs */
    assert(!apu_is_optical_pack_connected(0x07u));
    assert(!apu_is_optical_pack_connected(0xFFu));
    assert(!apu_is_optical_pack_connected(0x1000u));
    printf("    -> Optical capability correctly identified for all AV packs [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: Host Default Fallback Behavior (Mock Disabled)
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing host fallback behavior when unmocked...\n");
    apu_eeprom_clear_mock();
    uint32_t def_flags = apu_query_eeprom_audio_flags();
    uint32_t def_pack = apu_query_smbus_av_pack();
    APU_AUDIO_TOPOLOGY def_topo = apu_detect_audio_topology();

    assert(def_flags == XC_AUDIO_FLAGS_STEREO);
    assert(def_pack == APU_AV_PACK_STANDARD);
    assert(def_topo == APU_TOPOLOGY_STEREO_20);
    printf("    -> Host fallback resolves to Stereo 2.0 with Standard AV pack [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: EEPROM Flag Query via Mock Interface
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing EEPROM flag variations via mock interface...\n");
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_MONO, APU_AV_PACK_STANDARD);
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_MONO);

    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_STEREO);

    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_ENABLE_AC3);

    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_DTS, APU_AV_PACK_HDTV);
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_ENABLE_DTS);

    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_SURROUND, APU_AV_PACK_STANDARD);
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_SURROUND);

    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3 | XC_AUDIO_FLAGS_ENABLE_DTS, APU_AV_PACK_HDTV);
    assert(apu_query_eeprom_audio_flags() == (XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3 | XC_AUDIO_FLAGS_ENABLE_DTS));
    printf("    -> EEPROM audio flag combinations queried accurately [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: Topology Determination Matrix
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing audio topology resolution decision matrix...\n");

    /* Case A: Standard Composite AV Pack + AC-3 disabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case B: Standard Composite AV Pack + AC-3 enabled -> Stereo 2.0 (No optical connector!) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_STANDARD);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case C: HDTV AV Pack + AC-3 disabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case D: HDTV AV Pack + AC-3 enabled -> Surround 5.1 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);

    /* Case E: Advanced S-Video Pack + AC-3 enabled -> Surround 5.1 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_SVIDEO);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);

    /* Case F: Advanced S-Video Pack + AC-3 disabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_SVIDEO);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case G: Advanced SCART Pack + AC-3 enabled -> Surround 5.1 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_SCART);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);

    /* Case H: Advanced SCART Pack + AC-3 disabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_SCART);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case I: RFU Pack + AC-3 enabled -> Stereo 2.0 (RFU has no optical output) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_RFU);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case J: VGA Box + AC-3 enabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_VGA);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case K: No AV Pack Connected + AC-3 enabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_NONE);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case L: HDTV Pack + DTS enabled without AC-3 -> Stereo 2.0 (APU DSE only encodes AC-3) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_DTS, APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    /* Case M: HDTV Pack + Mono mode with AC-3 enabled -> Surround 5.1 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_MONO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);

    /* Case N: HDTV Pack + Full flags combination (Stereo + AC-3 + DTS + Surround) -> Surround 5.1 */
    apu_eeprom_set_mock(true,
        XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3 | XC_AUDIO_FLAGS_ENABLE_DTS | XC_AUDIO_FLAGS_SURROUND,
        APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);

    printf("    -> Full topology resolution matrix passed [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: Mock Reset Verification
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing mock clear and restoration to host state...\n");
    apu_eeprom_clear_mock();
    assert(apu_query_eeprom_audio_flags() == XC_AUDIO_FLAGS_STEREO);
    assert(apu_query_smbus_av_pack() == APU_AV_PACK_STANDARD);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    printf("    -> Mock clear verified [PASS]\n");

    printf("=== All EEPROM & SMBus AV Pack Detection Tests Passed Successfully! ===\n");
    return 0;
}
