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

    /* PRD 0: size 65535, no EOT */
    NVAPU_PRD_ENTRY *prd0 = &b2->prd_table_virt[0];
    assert((prd0->control & 0xFFFF) == 65535);
    assert((prd0->control & NV_PAPU_PRD_EOT) == 0);
    assert(prd0->physical_address == b2->data_phys);

    /* PRD 1: size 65535, no EOT */
    NVAPU_PRD_ENTRY *prd1 = &b2->prd_table_virt[1];
    assert((prd1->control & 0xFFFF) == 65535);
    assert((prd1->control & NV_PAPU_PRD_EOT) == 0);
    assert(prd1->physical_address == b2->data_phys + 65535);

    /* PRD 2: size 2 (131072 - 65535*2 = 2), WITH EOT */
    NVAPU_PRD_ENTRY *prd2 = &b2->prd_table_virt[2];
    assert((prd2->control & 0xFFFF) == 2);
    assert((prd2->control & NV_PAPU_PRD_EOT) != 0);
    assert(prd2->physical_address == b2->data_phys + 131070);

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
