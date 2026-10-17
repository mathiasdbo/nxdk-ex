/*
 * alBufferData keeps a buffer's data in cached CPU memory; the APU copy is made
 * only when a source takes the buffer as its static buffer. A stream copies
 * queued buffers into its ring from the CPU copy (on a console the pool is
 * write-combined, and reading it back was the stream's whole cost), and a
 * buffer re-filled with no more data than it held keeps its block.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "al_source.h"
#include "al_buffer.h"
#include "al_stream.h"
#include "apu_voice.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x30000
#define FRAMES 2048

static int16_t s_pcm[FRAMES * 2];

static void fill(int16_t v, int frames) {
    int i;
    for (i = 0; i < frames * 2; i++) s_pcm[i] = (int16_t)(v + i);
}

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint src, stat, b, q[2];
    ALbuffer *bb;
    void *block;
    uint32_t held0;
    ALint v;
    int i;

    printf("=== alBufferData: CPU copy, APU copy on demand ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    alGenSources(1, &src);
    alGenSources(1, &stat);
    alGenBuffers(1, &b);
    alGenBuffers(2, q);
    assert(alGetError() == AL_NO_ERROR);
    held0 = al_buffer_held_bytes();

    printf("[1] alBufferData keeps the data in CPU memory, no APU copy...\n");
    fill(100, FRAMES);
    alBufferData(b, AL_FORMAT_STEREO16, s_pcm, FRAMES * 4, 48000);
    assert(alGetError() == AL_NO_ERROR);
    bb = al_buffer_get(b);
    assert(bb->cpu_data != NULL && bb->data_virt == bb->cpu_data);
    assert(bb->data_phys == 0 && bb->prd_table_virt == NULL && bb->prd_count == 0);
    assert(bb->cpu_cap == FRAMES * 4u && bb->held_bytes == FRAMES * 4u);
    assert(al_buffer_held_bytes() == held0 + FRAMES * 4u);
    assert(memcmp(bb->data_virt, s_pcm, FRAMES * 4) == 0);
    alGetBufferi(b, AL_SIZE, &v);
    assert(v == FRAMES * 4);
    printf("    -> [PASS]\n");

    printf("[2] a refill that fits keeps the block; a bigger one replaces it...\n");
    block = bb->cpu_data;
    for (i = 1; i <= 8; i++) {
        int frames = FRAMES / i;
        fill((int16_t)(200 + i), frames);
        alBufferData(b, AL_FORMAT_STEREO16, s_pcm, frames * 4, 48000);
        assert(alGetError() == AL_NO_ERROR);
        assert(bb->cpu_data == block && bb->cpu_cap == FRAMES * 4u);
        assert(bb->size == frames * 4 && memcmp(bb->data_virt, s_pcm, (size_t)frames * 4) == 0);
        assert(al_buffer_held_bytes() == held0 + FRAMES * 4u);
    }
    alBufferData(b, AL_FORMAT_MONO16, s_pcm, 16, 22050);   /* a format change keeps it too */
    assert(alGetError() == AL_NO_ERROR && bb->cpu_data == block && bb->channels == 1 && bb->frequency == 22050);
    fill(300, FRAMES);
    alBufferData(b, AL_FORMAT_STEREO16, s_pcm, FRAMES * 4, 48000);
    assert(alGetError() == AL_NO_ERROR && bb->cpu_cap == FRAMES * 4u);
    {
        static int16_t big[FRAMES * 4];
        memset(big, 0x11, sizeof(big));
        alBufferData(b, AL_FORMAT_STEREO16, big, (ALsizei)sizeof(big), 48000);
        assert(alGetError() == AL_NO_ERROR);
        assert(bb->cpu_cap == sizeof(big) && memcmp(bb->data_virt, big, sizeof(big)) == 0);
        assert(al_buffer_held_bytes() == held0 + sizeof(big));
    }
    printf("    -> [PASS]\n");

    printf("[3] a static source makes the APU copy, with its tail and table...\n");
    fill(400, FRAMES);
    alBufferData(b, AL_FORMAT_STEREO16, s_pcm, FRAMES * 4, 48000);
    assert(alGetError() == AL_NO_ERROR);
    alSourcei(stat, AL_BUFFER, (ALint)b);
    assert(alGetError() == AL_NO_ERROR);
    assert(bb->cpu_data == NULL && bb->cpu_cap == 0);
    assert(bb->data_phys != 0 && bb->prd_table_virt != NULL && bb->prd_count == (uint32_t)apu_prd_calculate_count(FRAMES * 4));
    assert(memcmp(bb->data_virt, s_pcm, FRAMES * 4) == 0);
    assert(bb->held_bytes > FRAMES * 4u && al_buffer_held_bytes() == held0 + bb->held_bytes);
    alSourcePlay(stat);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(stat, AL_SOURCE_STATE, &v);
    assert(v == AL_PLAYING);
    alBufferData(b, AL_FORMAT_STEREO16, s_pcm, 64, 48000);   /* still attached */
    assert(alGetError() == AL_INVALID_OPERATION);
    alSourceStop(stat);
    alSourcei(stat, AL_BUFFER, (ALint)b);   /* again: nothing to do */
    assert(alGetError() == AL_NO_ERROR && bb->cpu_data == NULL);
    alSourcei(stat, AL_BUFFER, 0);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> [PASS]\n");

    printf("[4] refilled after it, the buffer goes back to a CPU copy...\n");
    fill(500, FRAMES / 2);
    alBufferData(b, AL_FORMAT_STEREO16, s_pcm, FRAMES * 2, 48000);
    assert(alGetError() == AL_NO_ERROR);
    assert(bb->cpu_data != NULL && bb->data_phys == 0 && bb->prd_table_virt == NULL);
    assert(al_buffer_held_bytes() == held0 + FRAMES * 2u);
    printf("    -> [PASS]\n");

    printf("[5] queued buffers stay CPU copies; the ring gets their data...\n");
    for (i = 0; i < 2; i++) {
        fill((int16_t)(1000 * (i + 1)), FRAMES);
        alBufferData(q[i], AL_FORMAT_STEREO16, s_pcm, FRAMES * 4, 48000);
        assert(alGetError() == AL_NO_ERROR);
    }
    alSourceQueueBuffers(src, 2, q);
    assert(alGetError() == AL_NO_ERROR);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);
    for (i = 0; i < 2; i++) {
        assert(al_buffer_get(q[i])->cpu_data != NULL && al_buffer_get(q[i])->data_phys == 0);
    }
    {
        const ALbuffer *rb = al_source_play_buffer(al_source_get(src));
        const int16_t *ring = (const int16_t *)rb->data_virt;
        fill(1000, FRAMES);
        assert(rb != NULL && rb->data_phys != 0);
        assert(memcmp(ring, s_pcm, 64 * 4) == 0);   /* the first buffer's head */
    }
    alSourceStop(src);
    alSourcei(src, AL_BUFFER, 0);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> [PASS]\n");

    printf("[6] deleting returns every byte...\n");
    alDeleteSources(1, &src);
    alDeleteSources(1, &stat);
    alDeleteBuffers(1, &b);
    alDeleteBuffers(2, q);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_buffer_held_bytes() == held0);
    printf("    -> [PASS]\n");

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    assert(apu_mem_pool_size() == 0);

    printf("[7] alXboxSetPoolSize sizes the pool of the next open...\n");
    alXboxSetPoolSize(60000);   /* rounded up to 4 KiB */
    dev = alcOpenDevice(NULL);
    assert(dev && apu_mem_pool_size() == 61440u);
    {
        /* more than the pool holds still allocates (its own block) */
        uint32_t phys = 0;
        void *p = apu_mem_alloc_phys(128u * 1024u, 8, &phys);
        assert(p != NULL && phys != 0);
        apu_mem_free_phys(p);
    }
    alcCloseDevice(dev);
    alXboxSetPoolSize(0);
    dev = alcOpenDevice(NULL);
    assert(dev && apu_mem_pool_size() == APU_MEM_DEFAULT_POOL_SIZE);
    alcCloseDevice(dev);
    printf("    -> [PASS]\n");
    free(mock);
    printf("=== All alBufferData CPU-copy Tests Passed! ===\n");
    return 0;
}
