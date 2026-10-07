#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <assert.h>
#include <time.h>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_voice.h"
#include "apu_spatial.h"
#include "apu_eeprom.h"
#include "apu_ep.h"
#include "apu_voice_mgr.h"
#include "al_source.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* XBOX_CPU_HZ, CYCLES_PER_US and FRAME_TIME_US are not used: this host test
 * prints raw host timer ticks (the TSC on x86, clock() elsewhere) and does not
 * convert them to time or CPU load. */
#define XBOX_CPU_HZ             733333333ULL
#define CYCLES_PER_US           (733.333333)
#define FRAME_TIME_US           (16666.6667)
#define MAX_BENCH_SOURCES       64
#define BENCH_FRAMES            50

static int16_t s_pcm_data[4800];

/* TSC ticks on x86 hosts, clock() ticks on any other host (the unit differs). */
static inline uint64_t get_host_ticks(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
#else
    return (uint64_t)clock();
#endif
}

static void setup_test_tone(void) {
    for (size_t i = 0; i < 4800; ++i) {
        double t = (double)i / 48000.0;
        double rad = 2.0 * M_PI * 480.0 * t;
        s_pcm_data[i] = (int16_t)(sin(rad) * 20000.0);
    }
}

int main(void) {
    printf("=== OpenAL Host Timing Test (source update path incl. mock voice-context writes) ===\n");

    setup_test_tone();

    /* Initialize physical memory allocator */
    int mem_init_res = apu_mem_init(0);
    assert(mem_init_res == 0);

    /* Allocate mock MMIO register space */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, 0x10000);
    assert(mock_mmio != NULL);

    /* Allocate mock 3D Voice Context Array (64 voices * 128 bytes = 8192 bytes) */
    NVAPU_VOICE_CONTEXT_3D *voice_array = (NVAPU_VOICE_CONTEXT_3D *)calloc(
        NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D)
    );
    assert(voice_array != NULL);

    int vp_init_res = apu_voice_subsystem_init((uintptr_t)mock_mmio, voice_array, 0x10000);
    assert(vp_init_res == 0);

    /* Initialize OpenAL subsystems */
    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    /* Test 1: Buffer setup and 64 source creation */
    printf("[1] Allocating buffer and 64 hardware voice sources...\n");
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    assert(buffer != 0);
    alBufferData(buffer, AL_FORMAT_MONO16, s_pcm_data, sizeof(s_pcm_data), 48000);
    assert(alGetError() == AL_NO_ERROR);

    ALuint sources[MAX_BENCH_SOURCES];
    alGenSources(MAX_BENCH_SOURCES, sources);
    assert(alGetError() == AL_NO_ERROR);

    for (int i = 0; i < MAX_BENCH_SOURCES; ++i) {
        alSourcei(sources[i], AL_BUFFER, (ALint)buffer);
        alSourcei(sources[i], AL_LOOPING, AL_TRUE);
        alSourcef(sources[i], AL_REFERENCE_DISTANCE, 1.0f);
        alSourcef(sources[i], AL_MAX_DISTANCE, 50.0f);
        alSourcef(sources[i], AL_ROLLOFF_FACTOR, 1.0f);
        alSourcef(sources[i], AL_GAIN, 0.5f);
    }
    printf("    -> 64 sources and 48 kHz mono buffer initialized [PASS]\n");

    /* Test 2: Profiling across voice count tiers: 1, 16, 32, 64 */
    printf("[2] Executing profiling tiers (1, 16, 32, 64 active sources)...\n");
    const uint32_t tiers[] = { 1, 16, 32, 64 };
    const int num_tiers = 4;

    for (int t = 0; t < num_tiers; ++t) {
        uint32_t active_count = tiers[t];

        /* Stop all and play only active_count */
        alSourceStopv(MAX_BENCH_SOURCES, sources);

        for (uint32_t i = 0; i < active_count; ++i) {
            float angle = (float)i * (2.0f * (float)M_PI / (float)active_count);
            float x = 5.0f * sinf(angle);
            float z = -5.0f * cosf(angle);
            alSource3f(sources[i], AL_POSITION, x, 0.0f, z);
            alSourcePlay(sources[i]);
        }

        assert(apu_voice_mgr_get_active_hw_count() == active_count);

        uint64_t total_ticks = 0;
        for (int f = 0; f < BENCH_FRAMES; ++f) {
            float theta = (float)f * 0.05f;

            uint64_t t0 = get_host_ticks();

            for (uint32_t i = 0; i < active_count; ++i) {
                float a = (float)i * (2.0f * (float)M_PI / (float)active_count) + theta;
                alSource3f(sources[i], AL_POSITION, 5.0f * sinf(a), 0.0f, -5.0f * cosf(a));
            }
            al_source_update_frame();

            uint64_t t1 = get_host_ticks();
            total_ticks += (t1 > t0) ? (t1 - t0) : 0;
        }

        double avg_ticks = (double)total_ticks / (double)BENCH_FRAMES;
        printf("    -> Tier %2u Voices: Avg host ticks per frame (x86 TSC, else clock()) = %8.0f | active count = %u [PASS]\n",
               active_count, avg_ticks, apu_voice_mgr_get_active_hw_count());
    }

    /* Test 3: all 64 voices are still active after the last tier. The tick
     * counts above are informational only: this test asserts no timing budget.
     * The timed region is the library's source update path (the spatial math
     * plus the writes of voice contexts into mock memory) and the harness' own
     * sinf/cosf; it is not APU mixing. */
    printf("[3] Verifying full 64 HW voice saturation after profiling...\n");
    uint32_t active_hw = apu_voice_mgr_get_active_hw_count();
    assert(active_hw == 64);
    printf("    -> 64 HW voices still active (no timing threshold is asserted) [PASS]\n");

    /* Teardown */
    printf("[4] Tearing down benchmark sources and buffers...\n");
    alSourceStopv(MAX_BENCH_SOURCES, sources);
    alDeleteSources(MAX_BENCH_SOURCES, sources);
    alDeleteBuffers(1, &buffer);
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    free(voice_array);
    free(mock_mmio);
    apu_mem_shutdown();

    printf("=== Host Timing Test Passed! ===\n");
    return 0;
}
