#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "al_source.h"
#include "al_buffer.h"
#include "al_listener.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x30000 /* 192 KB for BAR0 registers + PRAM @ 0x20000 */

int main(void) {
    printf("=== OpenAL ALC Device & Context Management Host Test ===\n");

    /* Allocate mock APU MMIO page */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    /* Test 1: Device Open, Hardware Subsystems Setup, and Query */
    printf("[1] Testing alcOpenDevice(NULL) & APU hardware initialization...\n");
    ALCdevice *dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    assert(dev->is_open == true);
    assert(dev->voice_table_virt != NULL);
    assert(dev->voice_table_phys != 0);
    assert(dev->context_count == 0);
    assert(strcmp(dev->name, "MCPX APU 5.1 Surround") == 0);
    assert(alcGetError(dev) == ALC_NO_ERROR);

    /* A mock base is used as given (null/software-model backend, no real MMIO) */
    assert(dev->apu_base == (uintptr_t)mock_mmio);
    assert(alc_get_apu_base() == (uintptr_t)mock_mmio);

    /* Verify APU Output Processor (EP) register programming */
    uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
    uint32_t ep_route = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_ROUTE);
    uint32_t ep_ctrl = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_CONTROL);
    assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_STEREO);
    assert(ep_route == NV_PAPU_EP_ROUTE_DEFAULT);
    assert((ep_ctrl & NV_PAPU_EP_CONTROL_ENABLE) != 0);

    /* Verify APU Global Processor (GP) DSP microcode upload and start */
    uint32_t gp_ctrl = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_CONTROL);
    assert((gp_ctrl & NV_PAPU_GPDSP_CONTROL_RUN) != 0);
    uint32_t pram_first = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM);
    assert(pram_first == 0x0AF080u); /* Reset vector instruction */

    /* Verify APU Voice Processor (VP) table base address and control */
    uint32_t vp_base = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_BASE_ADDR);
    uint32_t vp_ctrl = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_CONTROL);
    assert(vp_base == dev->voice_table_phys);
    assert((vp_ctrl & NV_PAPU_VP_CONTROL_ENABLE) != 0);
    assert((vp_ctrl & NV_PAPU_VP_CONTROL_MODE_3D) != 0);
    printf("    -> Hardware EP/GP/VP and Voice Table verified [PASS]\n");

    /* Test 2: ALC String & Integer Query APIs */
    printf("[2] Testing alcGetString & alcGetIntegerv queries...\n");
    const ALCchar *dev_name = alcGetString(dev, ALC_DEVICE_SPECIFIER);
    assert(dev_name != NULL);
    assert(strcmp(dev_name, "MCPX APU 5.1 Surround") == 0);

    const ALCchar *def_name = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);
    assert(def_name != NULL);
    assert(strcmp(def_name, "MCPX APU 5.1 Surround") == 0);

    const ALCchar *exts = alcGetString(dev, ALC_EXTENSIONS);
    assert(exts != NULL);

    ALCint major = 0, minor = 0;
    alcGetIntegerv(dev, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(dev, ALC_MINOR_VERSION, 1, &minor);
    assert(major == 1 && minor == 1);

    ALCint attr_size = 0;
    alcGetIntegerv(dev, ALC_ATTRIBUTES_SIZE, 1, &attr_size);
    assert(attr_size == 11);

    ALCint attrs[12] = {0};
    alcGetIntegerv(dev, ALC_ALL_ATTRIBUTES, 11, attrs);
    assert(attrs[0] == ALC_FREQUENCY && attrs[1] == 48000);
    assert(attrs[2] == ALC_REFRESH && attrs[3] == 60);
    assert(attrs[4] == ALC_SYNC && attrs[5] == ALC_FALSE);
    assert(attrs[6] == ALC_MONO_SOURCES && attrs[7] == 64);
    assert(attrs[8] == ALC_STEREO_SOURCES && attrs[9] == 32);
    assert(attrs[10] == 0);
    printf("    -> String and integer queries conform to OpenAL 1.1 [PASS]\n");

    /* Test 3: Context Creation & Attribute Parsing */
    printf("[3] Testing alcCreateContext & custom attributes...\n");
    ALCcontext *ctx1 = alcCreateContext(dev, NULL);
    assert(ctx1 != NULL);
    assert(ctx1->device == dev);
    assert(ctx1->frequency == 48000);
    assert(ctx1->mono_sources == 64);
    assert(dev->context_count == 1);

    ALCint custom_attrs[] = {
        ALC_FREQUENCY, 44100,
        ALC_REFRESH, 50,
        ALC_MONO_SOURCES, 32,
        0
    };
    ALCcontext *ctx2 = alcCreateContext(dev, custom_attrs);
    assert(ctx2 != NULL);
    assert(ctx2->frequency == 44100);
    assert(ctx2->refresh == 50);
    assert(ctx2->mono_sources == 32);
    assert(dev->context_count == 2);
    printf("    -> Contexts created and attributes parsed [PASS]\n");

    /* Test 4: Current Context Switching */
    printf("[4] Testing alcMakeContextCurrent & alcGetCurrentContext...\n");
    assert(alcGetCurrentContext() == NULL);

    assert(alcMakeContextCurrent(ctx1) == ALC_TRUE);
    assert(alcGetCurrentContext() == ctx1);
    assert(dev->active_context == ctx1);

    assert(alcMakeContextCurrent(ctx2) == ALC_TRUE);
    assert(alcGetCurrentContext() == ctx2);
    assert(dev->active_context == ctx2);

    assert(alcMakeContextCurrent(NULL) == ALC_TRUE);
    assert(alcGetCurrentContext() == NULL);

    /* Processing a valid context is legal (it also runs the voice frame tick); invalid is an error */
    alcProcessContext(ctx1);
    assert(alcGetError(dev) == ALC_NO_ERROR);
    assert(alcGetError(NULL) == ALC_NO_ERROR);
    alcProcessContext(NULL);
    assert(alcGetError(NULL) == ALC_INVALID_CONTEXT);
    printf("    -> Context switching verified [PASS]\n");

    /* Test 5: Context Destruction Safety */
    printf("[5] Testing alcDestroyContext and active context protection...\n");
    assert(alcMakeContextCurrent(ctx1) == ALC_TRUE);

    /* Attempting to destroy current context must fail */
    alcDestroyContext(ctx1);
    assert(alcGetError(dev) == ALC_INVALID_CONTEXT);
    assert(dev->context_count == 2);

    /* Destroying inactive context succeeds */
    alcDestroyContext(ctx2);
    assert(alcGetError(dev) == ALC_NO_ERROR);
    assert(dev->context_count == 1);

    /* Unbind current context and destroy ctx1 */
    assert(alcMakeContextCurrent(NULL) == ALC_TRUE);
    alcDestroyContext(ctx1);
    assert(alcGetError(dev) == ALC_NO_ERROR);
    assert(dev->context_count == 0);
    printf("    -> Context destruction rules verified [PASS]\n");

    /* Test 6: Device Close Safety and Hardware Teardown */
    printf("[6] Testing alcCloseDevice & hardware deinitialization...\n");
    /* Create a temporary context to verify close rejection */
    ALCcontext *ctx_temp = alcCreateContext(dev, NULL);
    assert(ctx_temp != NULL);
    assert(alcCloseDevice(dev) == ALC_FALSE);
    assert(alcGetError(dev) == ALC_INVALID_VALUE);

    alcDestroyContext(ctx_temp);
    assert(dev->context_count == 0);

    /* Closing device with zero contexts succeeds */
    assert(alcCloseDevice(dev) == ALC_TRUE);
    /* Verify EP was stopped */
    uint32_t ep_ctrl_closed = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_CONTROL);
    assert(ep_ctrl_closed == 0);
    /* Verify VP reset was asserted */
    uint32_t vp_ctrl_closed = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_CONTROL);
    assert((vp_ctrl_closed & NV_PAPU_VP_CONTROL_RESET) != 0);
    printf("    -> Device closed and hardware torn down cleanly [PASS]\n");

    /* Test 7: Full OpenAL 1.1 Playback Pipeline using ALC */
    printf("[7] Testing end-to-end OpenAL playback via ALC device & context...\n");
    dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    ctx1 = alcCreateContext(dev, NULL);
    assert(ctx1 != NULL);
    assert(alcMakeContextCurrent(ctx1) == ALC_TRUE);

    /* Create audio buffer and source */
    ALuint buf = 0;
    alGenBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);

    int16_t test_pcm[128] = {0};
    for (int i = 0; i < 128; i++) {
        test_pcm[i] = (int16_t)(i * 200);
    }
    alBufferData(buf, AL_FORMAT_STEREO16, test_pcm, sizeof(test_pcm), 48000);
    assert(alGetError() == AL_NO_ERROR);

    ALuint src = 0;
    alGenSources(1, &src);
    assert(alGetError() == AL_NO_ERROR);

    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);

    ALint state = 0;
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);

    /* Dynamic pitch and gain modification */
    alSourcef(src, AL_PITCH, 1.5f);
    alSourcef(src, AL_GAIN, 0.5f);
    float p = 0.0f, g = 0.0f;
    alGetSourcef(src, AL_PITCH, &p);
    alGetSourcef(src, AL_GAIN, &g);
    assert(p == 1.5f);
    assert(g == 0.5f);

    /* Teardown */
    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alcMakeContextCurrent(NULL) == ALC_TRUE);
    alcDestroyContext(ctx1);
    assert(alcCloseDevice(dev) == ALC_TRUE);
    printf("    -> End-to-end playback and property control verified [PASS]\n");

    /* Test 8: Invalid Parameters & Error Handling */
    printf("[8] Testing ALC invalid parameters & error handling...\n");
    ALCdevice *bad_dev = alcOpenDevice("NonExistentDevice");
    assert(bad_dev == NULL);
    assert(alcGetError(NULL) == ALC_INVALID_VALUE);

    ALCcontext *bad_ctx = alcCreateContext(NULL, NULL);
    assert(bad_ctx == NULL);
    assert(alcGetError(NULL) == ALC_INVALID_DEVICE);

    assert(alcGetContextsDevice(NULL) == NULL);
    assert(alcGetError(NULL) == ALC_INVALID_CONTEXT);

    assert(alcCloseDevice(NULL) == ALC_FALSE);
    assert(alcGetError(NULL) == ALC_INVALID_DEVICE);
    printf("    -> ALC error handling verified [PASS]\n");

    free(mock_mmio);
    printf("=== All ALC Device & Context Tests Passed Successfully! ===\n");
    return 0;
}
