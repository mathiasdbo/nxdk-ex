#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "apu_hardware.h"
#include "apu_eeprom.h"
#include "apu_ep.h"

#define MOCK_MMIO_SIZE 0x30000 /* 192 KB for BAR0 registers + PRAM @ 0x20000 */

/* Function pointer -> void * without a cast that -std=c99 -pedantic rejects */
static void *fn_to_ptr(void (*fn)(void)) {
    void *p = NULL;
    memcpy(&p, &fn, sizeof(p));
    return p;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Xbox Output Processor (EP) & Dolby Digital DSE Unit Test ===\n");

    /* Allocate simulated APU BAR0 MMIO space */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    uintptr_t apu_base = (uintptr_t)mock_mmio;

    /*
     * ------------------------------------------------------------------------
     * Test 1: Stereo 2.0 Output Processor Initialization
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing Stereo 2.0 EP FIFO and Control initialization...\n");
    memset(mock_mmio, 0, MOCK_MMIO_SIZE);

    int res_stereo = apu_ep_subsystem_init(apu_base, APU_TOPOLOGY_STEREO_20);
    assert(res_stereo == 0);

    uint32_t fifo_cfg = apu_ep_get_fifo_config(apu_base);
    uint32_t fifo_route = apu_ep_get_fifo_route(apu_base);
    uint32_t ep_ctrl = apu_read32(apu_base, NV_PAPU_EP_CONTROL);

    assert(fifo_cfg == NV_PAPU_EP_FIFO_CONFIG_STEREO);
    assert(fifo_cfg == 0x03u);
    assert(fifo_route == NV_PAPU_EP_ROUTE_DEFAULT);
    assert(fifo_route == 0x00543210u);
    assert(ep_ctrl == NV_PAPU_EP_CONTROL_ENABLE);
    assert(ep_ctrl == 0x01u);
    assert((ep_ctrl & NV_PAPU_EP_CONTROL_DSE_ENABLE) == 0);
    assert(!apu_is_dolby_digital_active(apu_base));
    printf("    -> Stereo 2.0 EP configured: FIFO=0x%02X, ROUTE=0x%08X, CTRL=0x%02X [PASS]\n",
           fifo_cfg, fifo_route, ep_ctrl);

    /*
     * ------------------------------------------------------------------------
     * Test 2: Surround 5.1 Output Processor & Dolby Digital DSE Initialization
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing Surround 5.1 EP FIFO, Route, and DSE initialization...\n");
    memset(mock_mmio, 0, MOCK_MMIO_SIZE);

    int res_51 = apu_ep_subsystem_init(apu_base, APU_TOPOLOGY_SURROUND_51);
    assert(res_51 == 0);

    fifo_cfg = apu_ep_get_fifo_config(apu_base);
    fifo_route = apu_ep_get_fifo_route(apu_base);
    ep_ctrl = apu_read32(apu_base, NV_PAPU_EP_CONTROL);

    assert(fifo_cfg == NV_PAPU_EP_FIFO_CONFIG_SURROUND);
    assert(fifo_cfg == 0x3Fu);
    assert(fifo_route == NV_PAPU_EP_ROUTE_DEFAULT);
    assert(fifo_route == 0x00543210u);
    assert((ep_ctrl & NV_PAPU_EP_CONTROL_ENABLE) != 0);
    assert((ep_ctrl & NV_PAPU_EP_CONTROL_DSE_ENABLE) != 0);
    assert(ep_ctrl == (NV_PAPU_EP_CONTROL_ENABLE | NV_PAPU_EP_CONTROL_DSE_ENABLE));
    assert(ep_ctrl == 0x03u);
    assert(apu_is_dolby_digital_active(apu_base));
    printf("    -> Surround 5.1 EP configured: FIFO=0x%02X, ROUTE=0x%08X, CTRL=0x%02X [PASS]\n",
           fifo_cfg, fifo_route, ep_ctrl);

    /*
     * ------------------------------------------------------------------------
     * Test 3: EP Subsystem Teardown & Invalid Inputs
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing EP subsystem deinitialization and error handling...\n");
    apu_ep_subsystem_deinit(apu_base);
    ep_ctrl = apu_read32(apu_base, NV_PAPU_EP_CONTROL);
    assert(ep_ctrl == 0u);
    assert(!apu_is_dolby_digital_active(apu_base));

    int res_invalid = apu_ep_subsystem_init(apu_base, (APU_AUDIO_TOPOLOGY)99);
    assert(res_invalid < 0);
    printf("    -> Deinitialization cleared control register, invalid topology rejected [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: ALC Device Lifecycle Integration with Dynamic Topology
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing ALC device open/close integration with EP subsystem...\n");
    alc_set_apu_base(apu_base);

    /* Case A: Stereo default topology */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);
    ALCdevice *dev_stereo = alcOpenDevice(NULL);
    assert(dev_stereo != NULL);
    assert(dev_stereo->topology == APU_TOPOLOGY_STEREO_20);
    assert(apu_ep_get_fifo_config(apu_base) == NV_PAPU_EP_FIFO_CONFIG_STEREO);
    assert(apu_read32(apu_base, NV_PAPU_EP_CONTROL) == NV_PAPU_EP_CONTROL_ENABLE);
    assert(!apu_is_dolby_digital_active(apu_base));
    assert(alcCloseDevice(dev_stereo) == ALC_TRUE);
    assert(apu_read32(apu_base, NV_PAPU_EP_CONTROL) == 0u);

    /* Case B: Surround 5.1 topology (HDTV Pack + AC-3 enabled in EEPROM) */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);
    ALCdevice *dev_51 = alcOpenDevice(NULL);
    assert(dev_51 != NULL);
    assert(dev_51->topology == APU_TOPOLOGY_SURROUND_51);
    assert(apu_ep_get_fifo_config(apu_base) == NV_PAPU_EP_FIFO_CONFIG_SURROUND);
    assert(apu_read32(apu_base, NV_PAPU_EP_CONTROL) == (NV_PAPU_EP_CONTROL_ENABLE | NV_PAPU_EP_CONTROL_DSE_ENABLE));
    assert(apu_is_dolby_digital_active(apu_base));
    assert(alcCloseDevice(dev_51) == ALC_TRUE);
    assert(apu_read32(apu_base, NV_PAPU_EP_CONTROL) == 0u);
    printf("    -> ALC device lifecycle dynamically configures EP & DSE correctly [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: OpenAL Extension Query API (alXboxGetHardwareStatus)
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing alXboxGetHardwareStatus queries for all tokens...\n");

    /* 5.1 Voice Count Token */
    ALint val = 0;
    alXboxGetHardwareStatus(AL_XBOX_HW_VOICE_COUNT, &val);
    assert(val == 64);
    assert(alGetError() == AL_NO_ERROR);

    /* 5.2 AV Pack Type Token across all supported pack types */
    struct {
        uint32_t eeprom_pack;
        ALint expected_token;
    } pack_mappings[] = {
        { APU_AV_PACK_SCART,    AL_XBOX_AV_PACK_SCART },
        { APU_AV_PACK_HDTV,     AL_XBOX_AV_PACK_HDTV },
        { APU_AV_PACK_VGA,      AL_XBOX_AV_PACK_VGA },
        { APU_AV_PACK_RFU,      AL_XBOX_AV_PACK_RFU },
        { APU_AV_PACK_SVIDEO,   AL_XBOX_AV_PACK_SVIDEO },
        { APU_AV_PACK_STANDARD, AL_XBOX_AV_PACK_COMPOSITE },
        { APU_AV_PACK_NONE,     AL_XBOX_AV_PACK_NONE }
    };
    for (size_t i = 0; i < sizeof(pack_mappings) / sizeof(pack_mappings[0]); ++i) {
        apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, pack_mappings[i].eeprom_pack);
        val = -1;
        alXboxGetHardwareStatus(AL_XBOX_AV_PACK_TYPE, &val);
        assert(val == pack_mappings[i].expected_token);
        assert(alGetError() == AL_NO_ERROR);
    }

    /* AL tokens are library-private values equal to the raw SMC register 0x04 codes */
    assert(AL_XBOX_AV_PACK_SCART == 0x00);
    assert(AL_XBOX_AV_PACK_HDTV == 0x01);
    assert(AL_XBOX_AV_PACK_VGA == 0x02);
    assert(AL_XBOX_AV_PACK_RFU == 0x03);
    assert(AL_XBOX_AV_PACK_SVIDEO == 0x04);
    assert(AL_XBOX_AV_PACK_COMPOSITE == 0x06);
    assert(AL_XBOX_AV_PACK_NONE == 0x07);

    /* 5.3 Dolby Digital Active Token */
    /* In Stereo mode */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);
    dev_stereo = alcOpenDevice(NULL);
    assert(dev_stereo != NULL);
    val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &val);
    assert(val == 0);
    assert(alGetError() == AL_NO_ERROR);
    alcCloseDevice(dev_stereo);

    /* In Surround 5.1 mode */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);
    dev_51 = alcOpenDevice(NULL);
    assert(dev_51 != NULL);
    val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &val);
    assert(val == 1);
    assert(alGetError() == AL_NO_ERROR);

    /* 5.4 VP Context Base Physical Address Token */
    val = 0;
    alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &val);
    assert((uint32_t)val == dev_51->voice_table_phys);
    assert(val != 0);
    assert(alGetError() == AL_NO_ERROR);

    alcCloseDevice(dev_51);

    /* With no device open both cached queries report 0 (no MMIO is read) */
    val = -1;
    alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &val);
    assert(val == 0);
    val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &val);
    assert(val == 0);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> All 4 hardware query tokens returned correct physical parameters [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 6: Error Handling & Extension Function Pointer Retrieval
     * ------------------------------------------------------------------------
     */
    printf("[6] Testing error reporting paths and extension proc address...\n");

    /* Clear any prior errors */
    while (alGetError() != AL_NO_ERROR) {}

    /* Null pointer argument -> AL_INVALID_VALUE */
    alXboxGetHardwareStatus(AL_XBOX_HW_VOICE_COUNT, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Invalid query token -> AL_INVALID_ENUM */
    val = 0;
    alXboxGetHardwareStatus(0x9999, &val);
    assert(alGetError() == AL_INVALID_ENUM);

    /* Extension presence query */
    assert(alIsExtensionPresent("AL_XBOX_hardware_status") == AL_TRUE);
    assert(alIsExtensionPresent("AL_XBOX_update") == AL_TRUE);
    /* Not advertised: alBufferData rejects the multichannel formats */
    assert(alIsExtensionPresent("AL_EXT_MCFORMATS") == AL_FALSE);
    assert(alIsExtensionPresent("AL_NONEXISTENT_EXT") == AL_FALSE);
    assert(alIsExtensionPresent(NULL) == AL_FALSE);

    /* Proc address query */
    void *proc_al = alGetProcAddress("alXboxGetHardwareStatus");
    assert(proc_al == fn_to_ptr((void (*)(void))alXboxGetHardwareStatus));
    void *proc_alc = alcGetProcAddress(NULL, "alXboxGetHardwareStatus");
    assert(proc_alc == fn_to_ptr((void (*)(void))alXboxGetHardwareStatus));
    assert(alGetProcAddress("alXboxUpdateVoices") == fn_to_ptr((void (*)(void))alXboxUpdateVoices));
    assert(alcGetProcAddress(NULL, "alXboxUpdateVoices") == fn_to_ptr((void (*)(void))alXboxUpdateVoices));
    assert(alGetProcAddress("nonExistentFunction") == NULL);
    assert(alGetProcAddress(NULL) == NULL);

    /* Extensions string query */
    const ALchar *ext_str = alGetString(AL_EXTENSIONS);
    assert(ext_str != NULL);
    assert(strstr(ext_str, "AL_XBOX_hardware_status") != NULL);
    assert(strstr(ext_str, "AL_EXT_MCFORMATS") == NULL);
    printf("    -> Error checks, proc address queries, and extension string verified [PASS]\n");

    /* Teardown */
    free(mock_mmio);
    apu_eeprom_clear_mock();

    printf("=== All Output Processor & Dolby Digital DSE Tests Passed Successfully! ===\n");
    return 0;
}
