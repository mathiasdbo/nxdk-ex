#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include "al_buffer.h"
#include "apu_mem.h"

int main(void) {
    printf("=== OpenAL Buffer & PRD Chaining Host Test ===\n");

    /* Initialize memory and buffer subsystems */
    int mem_init_res = apu_mem_init(0);
    assert(mem_init_res == 0);
    al_buffer_init_subsystem();

    assert(alGetError() == AL_NO_ERROR);

    /* Test 1: alGenBuffers & alIsBuffer */
    printf("[1] Testing alGenBuffers and alIsBuffer...\n");
    ALuint bufs[4] = {0};
    alGenBuffers(4, bufs);
    assert(alGetError() == AL_NO_ERROR);
    assert(bufs[0] == 1);
    assert(bufs[1] == 2);
    assert(bufs[2] == 3);
    assert(bufs[3] == 4);

    for (int i = 0; i < 4; i++) {
        assert(alIsBuffer(bufs[i]) == AL_TRUE);
    }
    assert(alIsBuffer(0) == AL_FALSE);
    assert(alIsBuffer(999) == AL_FALSE);

    /* Test 2: Invalid generation / deletion params */
    printf("[2] Testing invalid parameters...\n");
    alGenBuffers(-1, bufs);
    assert(alGetError() == AL_INVALID_VALUE);

    alDeleteBuffers(-1, bufs);
    assert(alGetError() == AL_INVALID_VALUE);

    ALuint invalid_id = 150;
    alDeleteBuffers(1, &invalid_id);
    assert(alGetError() == AL_INVALID_NAME);

    /* Deleting 0 should be ignored per spec */
    ALuint zero_id = 0;
    alDeleteBuffers(1, &zero_id);
    assert(alGetError() == AL_NO_ERROR);

    /* Test 3: alBufferData formats and attribute queries */
    printf("[3] Testing alBufferData formats & queries...\n");
    uint8_t mono8_data[100];
    memset(mono8_data, 0x80, sizeof(mono8_data));
    alBufferData(bufs[0], AL_FORMAT_MONO8, mono8_data, sizeof(mono8_data), 22050);
    assert(alGetError() == AL_NO_ERROR);

    ALint freq = 0, bits = 0, channels = 0, size = 0;
    alGetBufferi(bufs[0], AL_FREQUENCY, &freq);
    alGetBufferi(bufs[0], AL_BITS, &bits);
    alGetBufferi(bufs[0], AL_CHANNELS, &channels);
    alGetBufferi(bufs[0], AL_SIZE, &size);
    assert(freq == 22050);
    assert(bits == 8);
    assert(channels == 1);
    assert(size == 100);

    ALfloat f_freq = 0.0f;
    alGetBufferf(bufs[0], AL_FREQUENCY, &f_freq);
    assert(f_freq == 22050.0f);

    /* Test STEREO16 format */
    int16_t stereo16_data[256 * 2];
    memset(stereo16_data, 0, sizeof(stereo16_data));
    alBufferData(bufs[1], AL_FORMAT_STEREO16, stereo16_data, sizeof(stereo16_data), 48000);
    assert(alGetError() == AL_NO_ERROR);

    alGetBufferi(bufs[1], AL_FREQUENCY, &freq);
    alGetBufferi(bufs[1], AL_BITS, &bits);
    alGetBufferi(bufs[1], AL_CHANNELS, &channels);
    alGetBufferi(bufs[1], AL_SIZE, &size);
    assert(freq == 48000);
    assert(bits == 16);
    assert(channels == 2);
    assert(size == (ALsizei)sizeof(stereo16_data));

    /* Test 4: Format validation errors */
    printf("[4] Testing validation error semantics...\n");
    /* Invalid format enum */
    alBufferData(bufs[2], 0x9999, mono8_data, 100, 44100);
    assert(alGetError() == AL_INVALID_ENUM);

    /* Misaligned size for stereo16 (frame size is 4, 3 is not aligned) */
    alBufferData(bufs[2], AL_FORMAT_STEREO16, stereo16_data, 3, 44100);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Negative or zero size */
    alBufferData(bufs[2], AL_FORMAT_MONO8, mono8_data, 0, 44100);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Zero frequency */
    alBufferData(bufs[2], AL_FORMAT_MONO8, mono8_data, 100, 0);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Null data pointer */
    alBufferData(bufs[2], AL_FORMAT_MONO8, NULL, 100, 44100);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Test 5: 128KB Buffer PRD Chaining & EOT verification */
    printf("[5] Testing 128KB buffer PRD chaining (3 chunks)...\n");
    const ALsizei buf_128kb_size = 128 * 1024; /* 131072 bytes */
    uint8_t *large_pcm = (uint8_t *)malloc(buf_128kb_size);
    assert(large_pcm != NULL);
    for (int i = 0; i < buf_128kb_size; i++) {
        large_pcm[i] = (uint8_t)(i & 0xFF);
    }

    alBufferData(bufs[2], AL_FORMAT_MONO8, large_pcm, buf_128kb_size, 48000);
    assert(alGetError() == AL_NO_ERROR);

    ALbuffer *b2 = al_buffer_get(bufs[2]);
    assert(b2 != NULL);
    assert(b2->size == buf_128kb_size);
    assert(b2->prd_count == 3);
    assert(b2->prd_table_virt != NULL);
    assert(b2->data_virt != NULL);
    assert(memcmp(b2->data_virt, large_pcm, buf_128kb_size) == 0);

    /* The chunk limit is 0xF000 (a multiple of 4096), not the odd 65535 */
    assert(NV_PAPU_PRD_MAX_CHUNK == 61440u);

    /* PRD 0: size 61440, no EOT */
    NVAPU_PRD_ENTRY *prd0 = &b2->prd_table_virt[0];
    assert((prd0->control & 0xFFFF) == 61440);
    assert((prd0->control & NV_PAPU_PRD_EOT) == 0);
    assert(prd0->physical_address == b2->data_phys);

    /* PRD 1: size 61440, no EOT, starts on a 4096 boundary relative to PRD 0 */
    NVAPU_PRD_ENTRY *prd1 = &b2->prd_table_virt[1];
    assert((prd1->control & 0xFFFF) == 61440);
    assert((prd1->control & NV_PAPU_PRD_EOT) == 0);
    assert(prd1->physical_address == b2->data_phys + 61440);

    /* PRD 2: size 8192 (131072 - 61440*2), WITH EOT */
    NVAPU_PRD_ENTRY *prd2 = &b2->prd_table_virt[2];
    assert((prd2->control & 0xFFFF) == 8192);
    assert((prd2->control & NV_PAPU_PRD_EOT) != 0);
    assert(prd2->physical_address == b2->data_phys + 122880);

    printf("    -> PRD 0: Phys 0x%08X, Size %u, EOT: %s\n",
           prd0->physical_address, prd0->control & 0xFFFF,
           (prd0->control & NV_PAPU_PRD_EOT) ? "YES" : "NO");
    printf("    -> PRD 1: Phys 0x%08X, Size %u, EOT: %s\n",
           prd1->physical_address, prd1->control & 0xFFFF,
           (prd1->control & NV_PAPU_PRD_EOT) ? "YES" : "NO");
    printf("    -> PRD 2: Phys 0x%08X, Size %u, EOT: %s\n",
           prd2->physical_address, prd2->control & 0xFFFF,
           (prd2->control & NV_PAPU_PRD_EOT) ? "YES" : "NO");

    free(large_pcm);

    /*
     * Test 5b: PRD chunk invariants for every PCM frame size and many buffer sizes.
     * Every chunk except the last is a multiple of 4096 (whole SGE pages), every
     * chunk start is frame aligned (no 16-bit sample is split across chunks), the
     * chunk sizes sum to the buffer size and only the last chunk carries EOT.
     */
    printf("[5b] Testing PRD chunk alignment invariants (mono8/mono16/stereo16)...\n");
    {
        static const struct { ALenum fmt; size_t frame; const char *name; } fmts[] = {
            { AL_FORMAT_MONO8,    1, "mono8"    },
            { AL_FORMAT_MONO16,   2, "mono16"   },
            { AL_FORMAT_STEREO16, 4, "stereo16" },
        };
        static const size_t bases[] = { 0x01000000u, 0x01000008u, 0x01003000u, 0x0A000010u };
        const size_t M = NV_PAPU_PRD_MAX_CHUNK;
        size_t sizes[64];
        size_t nsizes = 0;
        size_t i, f, b;
        size_t n;
        NVAPU_PRD_ENTRY *table;
        static uint8_t pcm_scratch[3 * 61440 + 4096];

        assert(M % 4096u == 0);
        assert(M % 4u == 0);

        /* Boundary sizes around one, two and three chunks plus the old odd limit */
        for (n = 1; n <= 3; n++) {
            sizes[nsizes++] = n * M - 4;
            sizes[nsizes++] = n * M;
            sizes[nsizes++] = n * M + 4;
        }
        sizes[nsizes++] = 4;
        sizes[nsizes++] = 4096;
        sizes[nsizes++] = 4100;
        sizes[nsizes++] = 65532;   /* largest multiple of 4 that is <= 65535 */
        sizes[nsizes++] = 65536;
        sizes[nsizes++] = 65540;
        sizes[nsizes++] = 131072;
        sizes[nsizes++] = 131076;
        assert(nsizes <= sizeof(sizes) / sizeof(sizes[0]));

        /* Direct sweep of apu_prd_build with synthetic frame-aligned bases */
        table = (NVAPU_PRD_ENTRY *)malloc(64 * sizeof(NVAPU_PRD_ENTRY));
        assert(table != NULL);
        for (f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
            for (b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
                for (i = 0; i < nsizes + 200; i++) {
                    size_t size = (i < nsizes) ? sizes[i] : (size_t)(i - nsizes + 1) * 1999u;
                    size_t count = 0, k, sum = 0;
                    size_t want_count;

                    size -= size % fmts[f].frame;
                    if (size == 0) {
                        continue;
                    }
                    want_count = (size + M - 1) / M;
                    assert(apu_prd_calculate_count(size) == want_count);
                    assert(want_count <= 64);

                    assert(apu_prd_build((uint32_t)bases[b], size, table, 64, &count) == 0);
                    assert(count == want_count);

                    for (k = 0; k < count; k++) {
                        size_t chunk = table[k].control & 0xFFFFu;
                        uint32_t start = table[k].physical_address;

                        /* Only bits [15:0] and EOT are used */
                        assert((table[k].control & ~(0xFFFFu | NV_PAPU_PRD_EOT)) == 0);
                        assert(chunk >= 1 && chunk <= M);
                        /* Chunk starts follow the buffer contiguously and stay frame aligned */
                        assert(start == (uint32_t)(bases[b] + k * M));
                        assert(start % fmts[f].frame == 0);
                        if (k + 1 < count) {
                            assert(chunk == M);
                            assert(chunk % 4096u == 0);
                            assert((table[k].control & NV_PAPU_PRD_EOT) == 0);
                        } else {
                            assert(chunk == size - (count - 1) * M);
                            assert(chunk % fmts[f].frame == 0);
                            assert((table[k].control & NV_PAPU_PRD_EOT) != 0);
                        }
                        sum += chunk;
                    }
                    assert(sum == size);

                    /* A table one entry too small is rejected and reports no entries */
                    if (count > 1) {
                        size_t bad = 123;
                        assert(apu_prd_build((uint32_t)bases[b], size, table, count - 1, &bad) == -2);
                        assert(bad == 0);
                    }
                }
            }
        }
        free(table);

        /* End to end through alBufferData: tables built from the real pool */
        for (f = 0; f < sizeof(fmts) / sizeof(fmts[0]); f++) {
            for (i = 0; i < nsizes; i++) {
                size_t size = sizes[i] - sizes[i] % fmts[f].frame;
                ALuint name = 0;
                ALbuffer *bb;
                size_t k, sum = 0;

                if (size == 0 || size > sizeof(pcm_scratch)) {
                    continue;
                }
                memset(pcm_scratch, 0x5A, size);
                alGenBuffers(1, &name);
                assert(alGetError() == AL_NO_ERROR);
                alBufferData(name, fmts[f].fmt, pcm_scratch, (ALsizei)size, 44100);
                assert(alGetError() == AL_NO_ERROR);

                bb = al_buffer_get(name);
                assert(bb != NULL);
                assert(bb->prd_count == (uint32_t)apu_prd_calculate_count(size));
                assert(bb->data_phys % fmts[f].frame == 0);
                for (k = 0; k < bb->prd_count; k++) {
                    const NVAPU_PRD_ENTRY *e = &bb->prd_table_virt[k];
                    size_t chunk = e->control & 0xFFFFu;

                    assert(e->physical_address % fmts[f].frame == 0);
                    assert(e->physical_address ==
                           apu_mem_get_physical_address((const uint8_t *)bb->data_virt + k * M));
                    if (k + 1 < bb->prd_count) {
                        assert(chunk % 4096u == 0);
                        assert((e->control & NV_PAPU_PRD_EOT) == 0);
                    } else {
                        assert((e->control & NV_PAPU_PRD_EOT) != 0);
                    }
                    sum += chunk;
                }
                assert(sum == size);

                alDeleteBuffers(1, &name);
                assert(alGetError() == AL_NO_ERROR);
            }
            printf("    -> %s: chunk invariants hold for %u sizes [PASS]\n",
                   fmts[f].name, (unsigned)nsizes);
        }
    }

    /* Test 6: Re-specifying buffer data */
    printf("[6] Testing buffer data replacement...\n");
    uint8_t small_pcm[16] = {0};
    alBufferData(bufs[2], AL_FORMAT_MONO8, small_pcm, 16, 44100);
    assert(alGetError() == AL_NO_ERROR);
    assert(b2->size == 16);
    assert(b2->prd_count == 1);
    assert((b2->prd_table_virt[0].control & NV_PAPU_PRD_EOT) != 0);
    assert((b2->prd_table_virt[0].control & 0xFFFF) == 16);

    /* Test 7: Reference counting & AL_INVALID_OPERATION enforcement */
    printf("[7] Testing reference counting & source attachment protection...\n");
    al_buffer_retain(b2);
    assert(b2->ref_count == 1);

    /* Cannot modify buffer while attached (ref_count > 0) */
    alBufferData(bufs[2], AL_FORMAT_MONO8, small_pcm, 16, 44100);
    assert(alGetError() == AL_INVALID_OPERATION);

    /* Cannot delete buffer while attached (ref_count > 0) */
    alDeleteBuffers(1, &bufs[2]);
    assert(alGetError() == AL_INVALID_OPERATION);
    assert(alIsBuffer(bufs[2]) == AL_TRUE);

    /* Release buffer */
    al_buffer_release(b2);
    assert(b2->ref_count == 0);

    /* Now deletion must succeed */
    alDeleteBuffers(1, &bufs[2]);
    assert(alGetError() == AL_NO_ERROR);
    assert(alIsBuffer(bufs[2]) == AL_FALSE);

    /* Delete remaining buffers */
    ALuint remaining[3] = {bufs[0], bufs[1], bufs[3]};
    alDeleteBuffers(3, remaining);
    assert(alGetError() == AL_NO_ERROR);
    for (int i = 0; i < 3; i++) {
        assert(alIsBuffer(remaining[i]) == AL_FALSE);
    }

    /* Test 8: Pool exhaustion at 256 buffers */
    printf("[8] Testing 256 buffer pool limit...\n");
    ALuint all_bufs[256];
    alGenBuffers(256, all_bufs);
    assert(alGetError() == AL_NO_ERROR);
    for (int i = 0; i < 256; i++) {
        assert(all_bufs[i] == (ALuint)(i + 1));
        assert(alIsBuffer(all_bufs[i]) == AL_TRUE);
    }

    /* 257th buffer should fail with AL_OUT_OF_MEMORY */
    ALuint overflow_buf = 0;
    alGenBuffers(1, &overflow_buf);
    assert(alGetError() == AL_OUT_OF_MEMORY);
    assert(overflow_buf == 0);

    /* Clean up all 256 buffers */
    alDeleteBuffers(256, all_bufs);
    assert(alGetError() == AL_NO_ERROR);

    /* Subsystem cleanup */
    al_buffer_cleanup_subsystem();
    apu_mem_shutdown();

    printf("=== All Buffer & PRD Chaining Tests Passed Successfully! ===\n");
    return 0;
}
