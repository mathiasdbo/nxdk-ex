#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "apu_spatial.h"
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "alc_context.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x40000

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== OpenAL 1.1 Woodworth ITD & Buffer Management Host Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Woodworth Delay Calculation Math & Clamping
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing Woodworth Delay Formula & Clamping (x = 0, +/-0.5, +/-1.0, +/-2.0)...\n");

    /* 1.1: Center (x = 0.0) -> Delay = 0 */
    uint16_t d_zero = apu_calc_itd_delay_samples(0.0f);
    assert(d_zero == 0);

    /* 1.2: Halfway right (x = 0.5) -> round(31 * (0.55*0.5 + 0.45*0.125)) = round(10.26875) = 10 */
    uint16_t d_half_pos = apu_calc_itd_delay_samples(0.5f);
    assert(d_half_pos == 10);

    /* 1.3: Halfway left (x = -0.5) -> magnitude is 0.5 -> Delay = 10 */
    uint16_t d_half_neg = apu_calc_itd_delay_samples(-0.5f);
    assert(d_half_neg == 10);

    /* 1.4: Full right (x = 1.0) -> round(31 * (0.55*1.0 + 0.45*1.0)) = round(31.0) = 31 */
    uint16_t d_one_pos = apu_calc_itd_delay_samples(1.0f);
    assert(d_one_pos == 31);

    /* 1.5: Full left (x = -1.0) -> magnitude is 1.0 -> Delay = 31 */
    uint16_t d_one_neg = apu_calc_itd_delay_samples(-1.0f);
    assert(d_one_neg == 31);

    /* 1.6: Clamping: |x| > 1.0 clamped to 1.0 -> Delay = 31 */
    uint16_t d_two_pos = apu_calc_itd_delay_samples(2.0f);
    assert(d_two_pos == 31);

    uint16_t d_two_neg = apu_calc_itd_delay_samples(-2.0f);
    assert(d_two_neg == 31);

    uint16_t d_huge = apu_calc_itd_delay_samples(100.0f);
    assert(d_huge == 31);

    uint16_t d_huge_neg = apu_calc_itd_delay_samples(-100.0f);
    assert(d_huge_neg == 31);

    /* 1.7: Maximum delay limit guard: must not exceed hardware limit of 64 */
    assert(d_huge <= 64);

    /* 1.8: Monotonicity check: delay non-decreasing as |x| grows from 0 to 1 */
    uint16_t prev_delay = 0;
    for (int step = 0; step <= 100; step++) {
        float x = (float)step / 100.0f;
        uint16_t d = apu_calc_itd_delay_samples(x);
        assert(d >= prev_delay);
        assert(d <= 31);
        prev_delay = d;
    }

    printf("    -> Woodworth delay curve: x=0: %u, x=0.5: %u, x=1.0: %u, x=2.0 (clamp): %u [PASS]\n",
           d_zero, d_half_pos, d_one_pos, d_two_pos);

    /*
     * ------------------------------------------------------------------------
     * Test 2: ITD Tap Assignment (Left vs Right Ear)
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing ITD Tap Assignment (Right x > 0, Left x < 0, Center x = 0)...\n");

    uint16_t tap_left = 999;
    uint16_t tap_right = 999;

    /* 2.1: Center -> Both taps 0 */
    apu_calc_itd_taps(0.0f, &tap_left, &tap_right);
    assert(tap_left == 0);
    assert(tap_right == 0);

    /* 2.2: Right (x = +0.5) -> Left ear delayed by 10, Right ear 0 */
    apu_calc_itd_taps(0.5f, &tap_left, &tap_right);
    assert(tap_left == 10);
    assert(tap_right == 0);

    /* 2.3: Left (x = -0.5) -> Left ear 0, Right ear delayed by 10 */
    apu_calc_itd_taps(-0.5f, &tap_left, &tap_right);
    assert(tap_left == 0);
    assert(tap_right == 10);

    /* 2.4: Full right (x = +1.0) -> Left ear 31, Right ear 0 */
    apu_calc_itd_taps(1.0f, &tap_left, &tap_right);
    assert(tap_left == 31);
    assert(tap_right == 0);

    /* 2.5: Full left (x = -1.0) -> Left ear 0, Right ear 31 */
    apu_calc_itd_taps(-1.0f, &tap_left, &tap_right);
    assert(tap_left == 0);
    assert(tap_right == 31);

    /* 2.6: Clamping: Extreme right (x = +2.5) -> Left ear 31, Right ear 0 */
    apu_calc_itd_taps(2.5f, &tap_left, &tap_right);
    assert(tap_left == 31);
    assert(tap_right == 0);

    /* 2.7: Clamping: Extreme left (x = -2.5) -> Left ear 0, Right ear 31 */
    apu_calc_itd_taps(-2.5f, &tap_left, &tap_right);
    assert(tap_left == 0);
    assert(tap_right == 31);

    /* 2.8: NULL pointer safety */
    apu_calc_itd_taps(0.5f, NULL, &tap_right);
    apu_calc_itd_taps(0.5f, &tap_left, NULL);
    apu_calc_itd_taps(0.5f, NULL, NULL);

    printf("    -> Tap assignment: Center(L=%u, R=%u), Right(L=%u, R=0), Left(L=0, R=%u) [PASS]\n",
           0, 0, 31, 31);

    /*
     * ------------------------------------------------------------------------
     * Test 3: ITD Circular Buffer Management Subsystem
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing ITD Circular Buffer Pool (16KB, 128-byte aligned, per-voice indexing)...\n");

    /* Subsystem constants */
    assert(NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE == 256u);
    assert(NV_PAPU_ITD_BUFFER_ALIGN == 128u);
    assert(NV_PAPU_ITD_POOL_SIZE == 16384u);

    int mem_res = apu_mem_init(0);
    assert(mem_res == 0);

    /* Initialize ITD subsystem */
    int itd_init_res = apu_itd_subsystem_init();
    assert(itd_init_res == 0);

    /* Idempotent initialization */
    assert(apu_itd_subsystem_init() == 0);

    /* Voice 0 buffer queries */
    uint32_t phys_0 = apu_itd_get_voice_buffer_phys(0);
    void *virt_0 = apu_itd_get_voice_buffer_virt(0);
    assert(phys_0 != 0);
    assert(virt_0 != NULL);

    /* 128-byte hardware alignment verification */
    assert((phys_0 & (NV_PAPU_ITD_BUFFER_ALIGN - 1)) == 0);
    assert(((uintptr_t)virt_0 & (NV_PAPU_ITD_BUFFER_ALIGN - 1)) == 0);

    /* Check zero initialization */
    const uint8_t *byte_ptr = (const uint8_t *)virt_0;
    for (size_t b = 0; b < NV_PAPU_ITD_POOL_SIZE; b++) {
        assert(byte_ptr[b] == 0);
    }

    /* Verify all 64 voices: offsets, alignment, non-overlapping addresses */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        uint32_t phys_v = apu_itd_get_voice_buffer_phys(v);
        void *virt_v = apu_itd_get_voice_buffer_virt(v);

        assert(phys_v != 0);
        assert(virt_v != NULL);

        /* Physical address must be phys_0 + v * 256 */
        assert(phys_v == phys_0 + (v * NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE));

        /* Virtual address must be virt_0 + v * 256 */
        assert((uintptr_t)virt_v == (uintptr_t)virt_0 + (v * NV_PAPU_ITD_BUFFER_SIZE_PER_VOICE));

        /* Every individual buffer must be 128-byte aligned */
        assert((phys_v & (NV_PAPU_ITD_BUFFER_ALIGN - 1)) == 0);
        assert(((uintptr_t)virt_v & (NV_PAPU_ITD_BUFFER_ALIGN - 1)) == 0);

        /* Write a distinctive pattern and verify isolation */
        uint16_t *samples = (uint16_t *)virt_v;
        samples[0] = (uint16_t)(0xA000 + v);
        samples[127] = (uint16_t)(0xB000 + v);
    }

    /* Verify sample patterns remained intact without neighbor corruption */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        uint16_t *samples = (uint16_t *)apu_itd_get_voice_buffer_virt(v);
        assert(samples[0] == (uint16_t)(0xA000 + v));
        assert(samples[127] == (uint16_t)(0xB000 + v));
    }

    /* Out-of-bounds index bounds checking */
    assert(apu_itd_get_voice_buffer_phys(64) == 0);
    assert(apu_itd_get_voice_buffer_phys(128) == 0);
    assert(apu_itd_get_voice_buffer_virt(64) == NULL);
    assert(apu_itd_get_voice_buffer_virt(128) == NULL);

    /* Deinitialization */
    apu_itd_subsystem_deinit();
    assert(apu_itd_get_voice_buffer_phys(0) == 0);
    assert(apu_itd_get_voice_buffer_virt(0) == NULL);

    /* Reinitialization after deinit */
    assert(apu_itd_subsystem_init() == 0);
    assert(apu_itd_get_voice_buffer_phys(0) != 0);

    printf("    -> ITD Buffer pool: 64 voices, 256B each, 128B aligned, pattern isolation verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: Voice Context DWORD 16 & DWORD 17 Integration
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing Hardware Voice Context DWORD 16 & 17 Updates (Playback & Real-Time)...\n");

    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);

    NVAPU_VOICE_CONTEXT_3D *voice_array = (NVAPU_VOICE_CONTEXT_3D *)calloc(
        NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D)
    );
    assert(voice_array != NULL);

    int vp_res = apu_voice_subsystem_init((uintptr_t)mock_mmio, voice_array, 0x10000);
    assert(vp_res == 0);

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    /* Create 48kHz mono 16-bit buffer */
    ALuint mono_buf = 0;
    alGenBuffers(1, &mono_buf);
    assert(alGetError() == AL_NO_ERROR);
    int16_t pcm_samples[256] = {0};
    alBufferData(mono_buf, AL_FORMAT_MONO16, pcm_samples, sizeof(pcm_samples), 48000);
    assert(alGetError() == AL_NO_ERROR);

    /* Create mono source */
    ALuint src = 0;
    alGenSources(1, &src);
    assert(alGetError() == AL_NO_ERROR);
    alSourcei(src, AL_BUFFER, (ALint)mono_buf);
    assert(alGetError() == AL_NO_ERROR);

    /*
     * Scenario 4.1: Source placed to the right at (10, 0, 0).
     * Listener at (0, 0, 0) facing -Z (default).
     * Listener local frame: Right = +X, Up = +Y, Forward = -Z.
     * local_pos = (10, 0, 0), distance = 10.0.
     * x_rel = 10.0 / 10.0 = +1.0.
     * ITD: Left delayed by 31 samples, Right delayed by 0 samples.
     */
    alSource3f(src, AL_POSITION, 10.0f, 0.0f, 0.0f);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s = al_source_get(src);
    assert(s != NULL && s->hw_voice_idx >= 0);
    uint32_t v_idx = (uint32_t)s->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *vctx = &voice_array[v_idx];

    /* Verify 3D mode enabled for mono source */
    assert(vctx->mode_3d == 1);

    /* Verify DWORD 16: itd_delay_left = 31, itd_delay_right = 0 */
    assert(vctx->itd_delay_left == 31);
    assert(vctx->itd_delay_right == 0);

    /* Inspect raw DWORD 16 memory representation */
    uint32_t *vctx_dwords = (uint32_t *)vctx;
    uint32_t dw16 = vctx_dwords[16];
    uint32_t expected_dw16 = ((uint32_t)31) | (((uint32_t)0) << 16);
    assert(dw16 == expected_dw16);

    /* Verify DWORD 17: physical circular buffer address */
    uint32_t expected_phys_buf = apu_itd_get_voice_buffer_phys(v_idx);
    assert(vctx->itd_delay_buffer == expected_phys_buf);
    assert(vctx_dwords[17] == expected_phys_buf);
    assert((vctx_dwords[17] & 127) == 0);

    printf("    -> Source at Right (+X): mode_3d=1, Delay L=%u, R=%u, BufferPhys=0x%08X [PASS]\n",
           vctx->itd_delay_left, vctx->itd_delay_right, vctx->itd_delay_buffer);

    /*
     * Scenario 4.2: Real-time source repositioning to the left at (-10, 0, 0).
     * local_pos = (-10, 0, 0), distance = 10.0.
     * x_rel = -10.0 / 10.0 = -1.0.
     * ITD: Left delayed by 0, Right delayed by 31 samples.
     */
    alSource3f(src, AL_POSITION, -10.0f, 0.0f, 0.0f);
    assert(alGetError() == AL_NO_ERROR);

    assert(vctx->itd_delay_left == 0);
    assert(vctx->itd_delay_right == 31);
    dw16 = vctx_dwords[16];
    expected_dw16 = ((uint32_t)0) | (((uint32_t)31) << 16);
    assert(dw16 == expected_dw16);
    assert(vctx->itd_delay_buffer == expected_phys_buf);

    printf("    -> Real-time Move to Left (-X): Delay L=%u, R=%u [PASS]\n",
           vctx->itd_delay_left, vctx->itd_delay_right);

    /*
     * Scenario 4.3: Real-time source position directly in front at (0, 0, -10).
     * local_pos = (0, 0, 10) in local front.
     * x_rel = 0.0 / 10.0 = 0.0.
     * ITD: Left delayed by 0, Right delayed by 0.
     */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -10.0f);
    assert(alGetError() == AL_NO_ERROR);

    assert(vctx->itd_delay_left == 0);
    assert(vctx->itd_delay_right == 0);
    assert(vctx_dwords[16] == 0u);

    printf("    -> Real-time Move to Center (-Z): Delay L=%u, R=%u [PASS]\n",
           vctx->itd_delay_left, vctx->itd_delay_right);

    /*
     * Scenario 4.4: Real-time Listener Orientation Change.
     * Source stays at (0, 0, -10).
     * Rotate listener 90 degrees to face East (+X):
     * Forward = (1, 0, 0), Up = (0, 1, 0).
     * Right basis vector = Forward x Up = (0, 0, 1).
     * Source at (0, 0, -10) relative to listener at (0, 0, 0):
     * Local Right offset = Source_pos . Right = (0, 0, -10) . (0, 0, 1) = -10.0!
     * Now source is directly to the listener's LEFT!
     * x_rel = -10.0 / 10.0 = -1.0 -> Delay Left = 0, Delay Right = 31!
     */
    float orient_east[6] = {1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, orient_east);
    assert(alGetError() == AL_NO_ERROR);

    assert(vctx->itd_delay_left == 0);
    assert(vctx->itd_delay_right == 31);

    printf("    -> Real-time Listener Turn East (+X): Source becomes Left, Delay L=%u, R=%u [PASS]\n",
           vctx->itd_delay_left, vctx->itd_delay_right);

    /*
     * Scenario 4.5: Stereo buffer bypasses 3D ITD processing.
     */
    ALuint stereo_buf = 0;
    alGenBuffers(1, &stereo_buf);
    int16_t stereo_pcm[512] = {0};
    alBufferData(stereo_buf, AL_FORMAT_STEREO16, stereo_pcm, sizeof(stereo_pcm), 48000);

    ALuint stereo_src = 0;
    alGenSources(1, &stereo_src);
    alSourcei(stereo_src, AL_BUFFER, (ALint)stereo_buf);
    alSource3f(stereo_src, AL_POSITION, 10.0f, 0.0f, 0.0f);
    alSourcePlay(stereo_src);

    ALsource *st_src = al_source_get(stereo_src);
    assert(st_src != NULL && st_src->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *st_vctx = &voice_array[st_src->hw_voice_idx];
    assert(st_vctx->mode_3d == 0);
    assert(st_vctx->itd_delay_left == 0);
    assert(st_vctx->itd_delay_right == 0);
    assert(st_vctx->itd_delay_buffer == 0);

    printf("    -> Stereo source bypass: mode_3d=0, ITD taps=0, Buffer=0 [PASS]\n");

    /* Cleanup */
    alSourceStop(src);
    alSourceStop(stereo_src);
    alDeleteSources(1, &src);
    alDeleteSources(1, &stereo_src);
    alDeleteBuffers(1, &mono_buf);
    alDeleteBuffers(1, &stereo_buf);
    al_listener_reset();
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    apu_itd_subsystem_deinit();
    free(voice_array);
    free(mock_mmio);
    apu_mem_shutdown();

    /*
     * ------------------------------------------------------------------------
     * Test 5: End-to-End ALC Device Integration Lifecycle
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing End-to-End ALC Device Lifecycle with ITD Subsystem...\n");

    uint8_t *mock_mmio2 = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio2 != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio2);

    ALCdevice *dev = alcOpenDevice(NULL);
    assert(dev != NULL);

    /* Verify ITD subsystem was initialized by alcOpenDevice */
    uint32_t dev_itd_phys = apu_itd_get_voice_buffer_phys(0);
    assert(dev_itd_phys != 0);
    assert((dev_itd_phys & 127) == 0);

    ALCcontext *ctx = alcCreateContext(dev, NULL);
    assert(ctx != NULL);
    alcMakeContextCurrent(ctx);

    /* Play a mono 3D source through standard ALC context */
    ALuint e2e_buf = 0;
    alGenBuffers(1, &e2e_buf);
    int16_t e2e_pcm[256] = {0};
    alBufferData(e2e_buf, AL_FORMAT_MONO16, e2e_pcm, sizeof(e2e_pcm), 48000);

    ALuint e2e_src = 0;
    alGenSources(1, &e2e_src);
    alSourcei(e2e_src, AL_BUFFER, (ALint)e2e_buf);
    alSource3f(e2e_src, AL_POSITION, -10.0f, 0.0f, 0.0f);
    alSourcePlay(e2e_src);

    ALsource *e2e_s = al_source_get(e2e_src);
    assert(e2e_s != NULL && e2e_s->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *e2e_vctx = apu_voice_get_context((uint32_t)e2e_s->hw_voice_idx);
    assert(e2e_vctx != NULL);
    assert(e2e_vctx->mode_3d == 1);
    assert(e2e_vctx->itd_delay_left == 0);
    assert(e2e_vctx->itd_delay_right == 31);
    assert(e2e_vctx->itd_delay_buffer == apu_itd_get_voice_buffer_phys((uint32_t)e2e_s->hw_voice_idx));

    /* Teardown ALC */
    alSourceStop(e2e_src);
    alDeleteSources(1, &e2e_src);
    alDeleteBuffers(1, &e2e_buf);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    ALCboolean close_ok = alcCloseDevice(dev);
    assert(close_ok == ALC_TRUE);

    /* Verify ITD subsystem deinitialized on device close */
    assert(apu_itd_get_voice_buffer_phys(0) == 0);

    free(mock_mmio2);

    printf("    -> ALC device Open/Close lifecycle cleanly manages ITD buffer pool [PASS]\n");

    printf("=== All Woodworth ITD & Buffer Management Tests Passed Successfully! ===\n");
    return 0;
}
