#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"

#define MOCK_MMIO_SIZE 0x30000

/* void * -> function pointer without a cast that -std=c99 -pedantic rejects */
typedef void (*ANY_FN)(void);

static ANY_FN ptr_to_fn(void *p) {
    ANY_FN fn = NULL;
    memcpy(&fn, &p, sizeof(fn));
    return fn;
}

/* True if name is a whole space-separated token of list */
static int token_in_list(const char *list, const char *name) {
    size_t n = strlen(name);
    const char *p = list;
    while (*p) {
        size_t len = strcspn(p, " ");
        if (len == n && strncmp(p, name, n) == 0) {
            return 1;
        }
        p += len;
        while (*p == ' ') {
            p++;
        }
    }
    return 0;
}

int main(void) {
    printf("====================================================\n");
    printf(" OpenAL Static Library Build & Link Verification\n");
    printf("====================================================\n");

    /* 1. Setup Mock APU MMIO for Host Execution */
    printf("[1] Initializing mock APU hardware environment...\n");
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    /* 2. ALC Device Management & Information Queries */
    printf("[2] Verifying ALC device APIs...\n");
    ALCdevice *dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    assert(alcGetError(dev) == ALC_NO_ERROR);

    const ALCchar *dev_name = alcGetString(dev, ALC_DEVICE_SPECIFIER);
    assert(dev_name != NULL);

    ALCint major = 0, minor = 0;
    alcGetIntegerv(dev, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(dev, ALC_MINOR_VERSION, 1, &minor);
    assert(major == 1 && minor == 1);

    ALCboolean alc_ext_pres = alcIsExtensionPresent(dev, "ALC_ENUMERATION_EXT");
    (void)alc_ext_pres;
    void *alc_proc = alcGetProcAddress(dev, "alXboxGetHardwareStatus");
    assert(alc_proc != NULL);
    ALCenum alc_enum_val = alcGetEnumValue(dev, "ALC_FREQUENCY");
    (void)alc_enum_val;

    /* 3. ALC Context Management */
    printf("[3] Verifying ALC context lifecycle APIs...\n");
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    assert(ctx != NULL);
    assert(alcMakeContextCurrent(ctx) == ALC_TRUE);
    assert(alcGetCurrentContext() == ctx);
    assert(alcGetContextsDevice(ctx) == dev);

    alcProcessContext(ctx);
    alcSuspendContext(ctx);

    /* 4. AL Global State & Query APIs */
    printf("[4] Verifying AL core state and query APIs...\n");
    alEnable(0);
    alDisable(0);
    assert(alIsEnabled(0) == AL_FALSE);

    const ALchar *vendor = alGetString(AL_VENDOR);
    const ALchar *version = alGetString(AL_VERSION);
    const ALchar *renderer = alGetString(AL_RENDERER);
    const ALchar *extensions = alGetString(AL_EXTENSIONS);
    assert(vendor != NULL && version != NULL && renderer != NULL && extensions != NULL);
    /* Renderer keeps the "MCPX APU" prefix and names the (mock => null) backend */
    assert(strncmp(renderer, "MCPX APU", 8) == 0);
    assert(strstr(renderer, "null") != NULL);

    ALboolean b_val = alGetBoolean(AL_DOPPLER_FACTOR);
    ALboolean b_vals[1] = {0};
    alGetBooleanv(AL_DOPPLER_FACTOR, b_vals);
    (void)b_val;

    ALint i_val = alGetInteger(AL_DISTANCE_MODEL);
    ALint i_vals[1] = {0};
    alGetIntegerv(AL_DISTANCE_MODEL, i_vals);
    (void)i_val;

    ALfloat f_val = alGetFloat(AL_SPEED_OF_SOUND);
    ALfloat f_vals[1] = {0.0f};
    alGetFloatv(AL_SPEED_OF_SOUND, f_vals);
    (void)f_val;

    ALdouble d_val = alGetDouble(AL_SPEED_OF_SOUND);
    ALdouble d_vals[1] = {0.0};
    alGetDoublev(AL_SPEED_OF_SOUND, d_vals);
    (void)d_val;

    ALenum err = alGetError();
    assert(err == AL_INVALID_ENUM);
    assert(alGetError() == AL_NO_ERROR);

    /*
     * Extension honesty: every advertised extension must be functional, and
     * alIsExtensionPresent must agree with the alGetString list.
     */
    {
        const char *p = extensions;
        char name[64];
        int advertised = 0;
        while (*p) {
            size_t len = strcspn(p, " ");
            if (len == 0) {
                p++;
                continue;
            }
            assert(len < sizeof(name));
            memcpy(name, p, len);
            name[len] = '\0';
            assert(alIsExtensionPresent(name) == AL_TRUE);

            if (strcmp(name, "AL_XBOX_hardware_status") == 0) {
                LPALXBOXGETHARDWARESTATUS get_status =
                    (LPALXBOXGETHARDWARESTATUS)ptr_to_fn(alGetProcAddress("alXboxGetHardwareStatus"));
                ALint vc = 0;
                assert(get_status != NULL);
                get_status(AL_XBOX_HW_VOICE_COUNT, &vc);
                assert(vc == 64);
            } else if (strcmp(name, "AL_XBOX_update") == 0) {
                LPALXBOXUPDATEVOICES update =
                    (LPALXBOXUPDATEVOICES)ptr_to_fn(alGetProcAddress("alXboxUpdateVoices"));
                assert(update != NULL);
                update();
                assert(alGetError() == AL_NO_ERROR);
            } else if (strcmp(name, "AL_EXT_MCFORMATS") == 0) {
                /* checked below: formats must be accepted if (and only if) advertised */
            } else {
                printf("advertised extension without a functional check: %s\n", name);
                assert(0);
            }
            advertised++;
            p += len;
        }
        assert(advertised >= 1);

        /* AL_EXT_MCFORMATS <=> alBufferData accepts the multichannel formats */
        {
            ALuint mcbuf = 0;
            int16_t mcdata[64] = {0};
            ALboolean mc_adv = alIsExtensionPresent("AL_EXT_MCFORMATS");
            assert((mc_adv == AL_TRUE) == (token_in_list(extensions, "AL_EXT_MCFORMATS") != 0));
            alGenBuffers(1, &mcbuf);
            assert(mcbuf != 0);
            while (alGetError() != AL_NO_ERROR) {}
            alBufferData(mcbuf, AL_FORMAT_QUAD16, mcdata, (ALsizei)sizeof(mcdata), 48000);
            assert((alGetError() == AL_NO_ERROR) == (mc_adv == AL_TRUE));
            alBufferData(mcbuf, AL_FORMAT_51CHN16, mcdata, (ALsizei)sizeof(mcdata), 48000);
            assert((alGetError() == AL_NO_ERROR) == (mc_adv == AL_TRUE));
            alDeleteBuffers(1, &mcbuf);
            while (alGetError() != AL_NO_ERROR) {}
        }
        assert(alIsExtensionPresent("AL_NONEXISTENT_EXT") == AL_FALSE);
    }
    void *al_proc = alGetProcAddress("alXboxGetHardwareStatus");
    assert(al_proc != NULL);
    assert(alGetProcAddress("alXboxUpdateVoices") != NULL);
    assert(alcGetProcAddress(dev, "alXboxUpdateVoices") != NULL);
    ALenum al_enum_val = alGetEnumValue("AL_FORMAT_51CHN16");
    (void)al_enum_val;

    /* 5. Listener APIs */
    printf("[5] Verifying AL Listener APIs...\n");
    alListenerf(AL_GAIN, 0.75f);
    ALfloat gain_query = 0.0f;
    alGetListenerf(AL_GAIN, &gain_query);
    assert(gain_query == 0.75f);

    alListener3f(AL_POSITION, 1.0f, 2.0f, 3.0f);
    ALfloat px, py, pz;
    alGetListener3f(AL_POSITION, &px, &py, &pz);
    assert(px == 1.0f && py == 2.0f && pz == 3.0f);

    ALfloat ori[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori);
    ALfloat ori_query[6] = {0};
    alGetListenerfv(AL_ORIENTATION, ori_query);
    assert(ori_query[2] == -1.0f);

    alListeneri(AL_GAIN, 1);
    alListener3i(AL_POSITION, 0, 0, 0);
    ALint li_arr[3] = {0, 0, 0};
    alListeneriv(AL_POSITION, li_arr);

    ALint l_int_val = 0;
    alGetListeneri(AL_GAIN, &l_int_val);
    ALint l_3i_a, l_3i_b, l_3i_c;
    alGetListener3i(AL_POSITION, &l_3i_a, &l_3i_b, &l_3i_c);
    ALint l_iv_arr[3] = {0};
    alGetListeneriv(AL_POSITION, l_iv_arr);

    /* 6. Buffer APIs */
    printf("[6] Verifying AL Buffer APIs...\n");
    ALuint buf = 0;
    alGenBuffers(1, &buf);
    assert(buf != 0);
    assert(alIsBuffer(buf) == AL_TRUE);

    int16_t pcm_data[32] = {0};
    alBufferData(buf, AL_FORMAT_MONO16, pcm_data, sizeof(pcm_data), 48000);

    ALint b_freq = 0, b_size = 0, b_bits = 0, b_channels = 0;
    alGetBufferi(buf, AL_FREQUENCY, &b_freq);
    alGetBufferi(buf, AL_SIZE, &b_size);
    alGetBufferi(buf, AL_BITS, &b_bits);
    alGetBufferi(buf, AL_CHANNELS, &b_channels);
    assert(b_freq == 48000);
    assert(b_size == (ALint)sizeof(pcm_data));
    assert(b_bits == 16);
    assert(b_channels == 1);

    alBufferf(buf, 0, 0.0f);
    alBuffer3f(buf, 0, 0.0f, 0.0f, 0.0f);
    ALfloat b_fv[1] = {0.0f};
    alBufferfv(buf, 0, b_fv);
    alBufferi(buf, 0, 0);
    alBuffer3i(buf, 0, 0, 0, 0);
    ALint b_iv[1] = {0};
    alBufferiv(buf, 0, b_iv);

    ALfloat b_f_out = 0.0f;
    alGetBufferf(buf, 0, &b_f_out);
    ALfloat b_3f_a, b_3f_b, b_3f_c;
    alGetBuffer3f(buf, 0, &b_3f_a, &b_3f_b, &b_3f_c);
    alGetBufferfv(buf, 0, b_fv);
    ALint b_3i_a, b_3i_b, b_3i_c;
    alGetBuffer3i(buf, 0, &b_3i_a, &b_3i_b, &b_3i_c);
    alGetBufferiv(buf, 0, b_iv);

    /* 7. Source APIs */
    printf("[7] Verifying AL Source APIs...\n");
    ALuint src = 0;
    alGenSources(1, &src);
    assert(src != 0);
    assert(alIsSource(src) == AL_TRUE);

    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcef(src, AL_PITCH, 1.0f);
    alSourcef(src, AL_GAIN, 0.8f);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 5.0f);
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    ALfloat s_gain = 0.0f;
    alGetSourcef(src, AL_GAIN, &s_gain);
    assert(s_gain == 0.8f);

    ALfloat s_x, s_y, s_z;
    alGetSource3f(src, AL_POSITION, &s_x, &s_y, &s_z);
    assert(s_x == 0.0f && s_y == 0.0f && s_z == 5.0f);

    ALfloat s_fv[3] = {0.0f, 0.0f, 0.0f};
    alSourcefv(src, AL_VELOCITY, s_fv);
    alGetSourcefv(src, AL_VELOCITY, s_fv);

    alSource3i(src, AL_POSITION, 0, 0, 0);
    ALint s_iv[1] = {0};
    alSourceiv(src, AL_LOOPING, s_iv);
    ALint s_int_val = 0;
    alGetSourcei(src, AL_LOOPING, &s_int_val);
    ALint s_3i_a, s_3i_b, s_3i_c;
    alGetSource3i(src, AL_POSITION, &s_3i_a, &s_3i_b, &s_3i_c);
    alGetSourceiv(src, AL_LOOPING, s_iv);

    alSourcePlay(src);
    alSourcePause(src);
    alSourceRewind(src);
    alSourceStop(src);

    ALuint src_array[1] = {src};
    alSourcePlayv(1, src_array);
    alSourcePausev(1, src_array);
    alSourceRewindv(1, src_array);
    alSourceStopv(1, src_array);

    ALuint queued_buf[1] = {buf};
    alSourceQueueBuffers(src, 1, queued_buf);
    ALuint unqueued_buf[1] = {0};
    alSourceUnqueueBuffers(src, 1, unqueued_buf);

    /* 8. Global Distance & Doppler Models */
    printf("[8] Verifying Distance & Doppler APIs...\n");
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    alDopplerFactor(1.5f);
    alDopplerVelocity(343.3f);
    alSpeedOfSound(343.3f);

    /* 9. Xbox Hardware Status Extension */
    printf("[9] Verifying AL_XBOX_hardware_status Extension...\n");
    ALint hw_voices = 0;
    alXboxGetHardwareStatus(AL_XBOX_HW_VOICE_COUNT, &hw_voices);
    assert(hw_voices == 64);

    ALint av_pack = 0;
    alXboxGetHardwareStatus(AL_XBOX_AV_PACK_TYPE, &av_pack);
    (void)av_pack;

    ALint dolby_active = 0;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dolby_active);
    (void)dolby_active;

    ALint vp_base = 0;
    alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &vp_base);
    assert(vp_base != 0);

    /* A mock base is reported as the null/software-model backend */
    ALint backend = -1;
    alXboxGetHardwareStatus(AL_XBOX_BACKEND, &backend);
    assert(backend == AL_XBOX_BACKEND_NULL);

    /* 10. Capture Functions (Stub verification) */
    printf("[10] Verifying ALC capture function stubs...\n");
    ALCdevice *cap_dev = alcCaptureOpenDevice(NULL, 48000, AL_FORMAT_MONO16, 1024);
    assert(cap_dev == NULL);
    assert(alcCaptureCloseDevice(NULL) == ALC_FALSE);
    alcCaptureStart(NULL);
    alcCaptureStop(NULL);
    alcCaptureSamples(NULL, NULL, 0);

    /* 11. Cleanup and Teardown */
    printf("[11] Cleaning up sources, buffers, context, and device...\n");
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(mock_mmio);

    printf("\n>>> ALL PUBLIC OPENAL 1.1 + XBOX EXTENSION SYMBOLS RESOLVED AND VERIFIED! <<<\n");
    return 0;
}
