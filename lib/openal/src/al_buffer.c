#include "al_buffer.h"
#include <AL/alext.h>
#include "apu_platform.h"
#include "apu_vp.h"
#include <stdlib.h>
#include <string.h>

/*
 * ============================================================================
 * Internal State & Global Buffer Pool
 * ============================================================================
 */
static ALbuffer s_buffers[AL_MAX_BUFFERS];
static bool s_subsystem_initialized = false;
static ALenum s_last_error = AL_NO_ERROR;
static uint32_t s_held_bytes;      /* library memory held for buffer data */
static uint32_t s_static_bytes;    /* application memory static buffers point at */
static uint32_t s_next_map_key = 1;
static bool s_static_flush = true;

/* Free whatever data a buffer holds (its own copy, or a static buffer's page
 * list and tail copy) and its PRD table */
static void release_data(ALbuffer *buf) {
    if (buf->is_static) {
        free(buf->pages);
        if (buf->tail_virt) {
            apu_mem_free_phys(buf->tail_virt);
        }
        s_static_bytes -= (uint32_t)buf->size;
    } else if (buf->cpu_data) {
        free(buf->cpu_data);   /* data_virt points at it */
    } else if (buf->data_virt) {
        apu_mem_free_phys(buf->data_virt);
    }
    buf->cpu_data = NULL;
    buf->cpu_cap = 0;
    if (buf->prd_table_virt) {
        apu_mem_free_phys(buf->prd_table_virt);
    }
    s_held_bytes -= buf->held_bytes;
    buf->data_virt = NULL;
    buf->data_phys = 0;
    buf->prd_table_virt = NULL;
    buf->prd_table_phys = 0;
    buf->prd_count = 0;
    buf->is_static = false;
    buf->pages = NULL;
    buf->page_count = 0;
    buf->page_offset = 0;
    buf->tail_virt = NULL;
    buf->map_key = 0;
    buf->held_bytes = 0;
}

uint32_t al_buffer_held_bytes(void) {
    return s_held_bytes;
}

uint32_t al_buffer_static_bytes(void) {
    return s_static_bytes;
}

void al_buffer_debug_set_static_flush(bool on) {
    s_static_flush = on;
}

/*
 * ============================================================================
 * Error Reporting Implementation
 * ============================================================================
 */
void alSetError(ALenum error) {
    if (s_last_error == AL_NO_ERROR && error != AL_NO_ERROR) {
        s_last_error = error;
    }
}

void al_set_error(ALenum error) {
    alSetError(error);
}

AL_API ALenum AL_APIENTRY alGetError(void) {
    ALenum err = s_last_error;
    s_last_error = AL_NO_ERROR;
    return err;
}

/*
 * ============================================================================
 * Buffer Subsystem Helpers
 * ============================================================================
 */
void al_buffer_init_subsystem(void) {
    for (size_t i = 0; i < AL_MAX_BUFFERS; i++) {
        s_buffers[i].id = (ALuint)(i + 1);
        s_buffers[i].in_use = false;
        s_buffers[i].format = 0;
        s_buffers[i].frequency = 0;
        s_buffers[i].size = 0;
        s_buffers[i].channels = 0;
        s_buffers[i].bits = 0;
        s_buffers[i].data_virt = NULL;
        s_buffers[i].data_phys = 0;
        s_buffers[i].prd_table_virt = NULL;
        s_buffers[i].prd_table_phys = 0;
        s_buffers[i].prd_count = 0;
        s_buffers[i].ref_count = 0;
        s_buffers[i].is_static = false;
        s_buffers[i].pages = NULL;
        s_buffers[i].page_count = 0;
        s_buffers[i].page_offset = 0;
        s_buffers[i].tail_virt = NULL;
        s_buffers[i].map_key = 0;
        s_buffers[i].held_bytes = 0;
        s_buffers[i].cpu_data = NULL;
        s_buffers[i].cpu_cap = 0;
    }
    s_held_bytes = 0;
    s_static_bytes = 0;
    s_subsystem_initialized = true;
    s_last_error = AL_NO_ERROR;
}

void al_buffer_cleanup_subsystem(void) {
    for (size_t i = 0; i < AL_MAX_BUFFERS; i++) {
        if (s_buffers[i].in_use) {
            release_data(&s_buffers[i]);
            s_buffers[i].in_use = false;
            s_buffers[i].ref_count = 0;
            s_buffers[i].loop_start = 0;
            s_buffers[i].loop_end = 0;
        }
    }
    s_subsystem_initialized = false;
    s_last_error = AL_NO_ERROR;
}

static void ensure_subsystem_initialized(void) {
    if (!s_subsystem_initialized) {
        al_buffer_init_subsystem();
    }
}

ALbuffer *al_buffer_get(ALuint id) {
    ensure_subsystem_initialized();
    if (id < 1 || id > AL_MAX_BUFFERS) {
        return NULL;
    }
    ALbuffer *buf = &s_buffers[id - 1];
    if (!buf->in_use) {
        return NULL;
    }
    return buf;
}

void al_buffer_retain(ALbuffer *buf) {
    if (buf) {
        buf->ref_count++;
    }
}

void al_buffer_release(ALbuffer *buf) {
    if (buf && buf->ref_count > 0) {
        buf->ref_count--;
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Buffer Lifecycle APIs
 * ============================================================================
 */
AL_API void AL_APIENTRY alGenBuffers(ALsizei n, ALuint *buffers) {
    ensure_subsystem_initialized();

    if (n < 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (n == 0) {
        return;
    }
    if (!buffers) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /* Verify sufficient free slots exist in pool */
    ALsizei available = 0;
    for (size_t i = 0; i < AL_MAX_BUFFERS; i++) {
        if (!s_buffers[i].in_use) {
            available++;
        }
    }

    if (available < n) {
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }

    /* Allocate buffer slots sequentially */
    ALsizei allocated = 0;
    for (size_t i = 0; i < AL_MAX_BUFFERS && allocated < n; i++) {
        if (!s_buffers[i].in_use) {
            s_buffers[i].id = (ALuint)(i + 1);
            s_buffers[i].in_use = true;
            s_buffers[i].format = 0;
            s_buffers[i].frequency = 0;
            s_buffers[i].size = 0;
            s_buffers[i].channels = 0;
            s_buffers[i].bits = 0;
            s_buffers[i].data_virt = NULL;
            s_buffers[i].data_phys = 0;
            s_buffers[i].prd_table_virt = NULL;
            s_buffers[i].prd_table_phys = 0;
            s_buffers[i].prd_count = 0;
            s_buffers[i].ref_count = 0;
            s_buffers[i].loop_start = 0;
            s_buffers[i].loop_end = 0;

            buffers[allocated++] = s_buffers[i].id;
        }
    }
}

AL_API void AL_APIENTRY alDeleteBuffers(ALsizei n, const ALuint *buffers) {
    ensure_subsystem_initialized();

    if (n < 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (n == 0) {
        return;
    }
    if (!buffers) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /* Validation phase: all IDs must be valid (or 0) and ref_count == 0 */
    for (ALsizei i = 0; i < n; i++) {
        ALuint id = buffers[i];
        if (id == 0) {
            continue; /* OpenAL ignores 0 without error */
        }
        if (id > AL_MAX_BUFFERS || !s_buffers[id - 1].in_use) {
            alSetError(AL_INVALID_NAME);
            return;
        }
        if (s_buffers[id - 1].ref_count > 0) {
            alSetError(AL_INVALID_OPERATION);
            return;
        }
    }

    /* Deletion phase */
    for (ALsizei i = 0; i < n; i++) {
        ALuint id = buffers[i];
        if (id == 0) {
            continue;
        }
        ALbuffer *buf = &s_buffers[id - 1];
        if (buf->in_use) {
            release_data(buf);
            buf->in_use = false;
            buf->format = 0;
            buf->frequency = 0;
            buf->size = 0;
            buf->channels = 0;
            buf->bits = 0;
            buf->prd_count = 0;
            buf->ref_count = 0;
        }
    }
}

AL_API ALboolean AL_APIENTRY alIsBuffer(ALuint buffer) {
    ensure_subsystem_initialized();

    if (buffer < 1 || buffer > AL_MAX_BUFFERS) {
        return AL_FALSE;
    }
    return s_buffers[buffer - 1].in_use ? AL_TRUE : AL_FALSE;
}

/* AL_EXT_FLOAT32: -1 .. 1 floats to 16-bit, clipped, rounded to nearest (NaN: 0) */
static int16_t float_to_s16(float f) {
    float v;
    if (!(f == f)) return 0;
    v = f * 32767.0f;
    if (v >= 32767.0f) return 32767;
    if (v <= -32768.0f) return -32768;
    return (int16_t)(v >= 0.0f ? v + 0.5f : v - 0.5f);
}

AL_API void AL_APIENTRY alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq) {
    ensure_subsystem_initialized();

    ALbuffer *buf = al_buffer_get(buffer);
    if (!buf) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (buf->ref_count > 0) {
        alSetError(AL_INVALID_OPERATION);
        return;
    }

    if (format == AL_FORMAT_MONO_FLOAT32 || format == AL_FORMAT_STEREO_FLOAT32) {
        ALsizei frame = (format == AL_FORMAT_STEREO_FLOAT32) ? 8 : 4, n, i;
        const float *in = (const float *)data;
        int16_t *pcm;
        if (size <= 0 || freq <= 0 || (size % frame) != 0 || data == NULL) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
        n = size / 4;
        pcm = (int16_t *)malloc((size_t)n * sizeof(int16_t));
        if (!pcm) {
            alSetError(AL_OUT_OF_MEMORY);
            return;
        }
        for (i = 0; i < n; i++) {
            pcm[i] = float_to_s16(in[i]);
        }
        alBufferData(buffer, (format == AL_FORMAT_STEREO_FLOAT32) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, pcm,
                     n * (ALsizei)sizeof(int16_t), freq);
        free(pcm);
        return;
    }

    ALint channels;
    ALint bits;
    ALsizei frame_size;

    switch (format) {
        case AL_FORMAT_MONO8:
            channels = 1;
            bits = 8;
            frame_size = 1;
            break;
        case AL_FORMAT_MONO16:
            channels = 1;
            bits = 16;
            frame_size = 2;
            break;
        case AL_FORMAT_STEREO8:
            channels = 2;
            bits = 8;
            frame_size = 2;
            break;
        case AL_FORMAT_STEREO16:
            channels = 2;
            bits = 16;
            frame_size = 4;
            break;
        case AL_FORMAT_MONO_XBOX_ADPCM:     /* sizes in whole 36-byte blocks */
            channels = 1;
            bits = 4;
            frame_size = (ALsizei)APU_VP_ADPCM_BLOCK_BYTES;
            break;
        case AL_FORMAT_STEREO_XBOX_ADPCM:
            channels = 2;
            bits = 4;
            frame_size = (ALsizei)(2u * APU_VP_ADPCM_BLOCK_BYTES);
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            return;
    }

    if (size <= 0 || freq <= 0 || (size % frame_size) != 0 || data == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    if (apu_prd_calculate_count((size_t)size) == 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /*
     * The data goes to cached CPU memory, not to the APU pool: the pool is
     * write-combined, and a stream reads its buffers back to copy them into
     * its ring (al_stream.c fill), which from write-combined memory is an
     * uncached read of every byte - on a console about 16 MB/s, 0.2 ms a frame
     * for one 48 kHz stereo stream. The APU copy is made when a voice needs it
     * (al_buffer_make_apu_ready). A buffer re-filled with no more data than
     * it held keeps its block, so a stream's refills allocate nothing.
     */
    void *cpu;
    uint32_t cap;
    if (buf->cpu_data != NULL && buf->cpu_cap >= (uint32_t)size) {
        cpu = buf->cpu_data;
        cap = buf->cpu_cap;
        buf->cpu_data = NULL;   /* kept from release_data below */
        buf->data_virt = NULL;
        s_held_bytes -= buf->held_bytes;
        buf->held_bytes = 0;
    } else {
        cpu = malloc((size_t)size);
        if (!cpu) {
            alSetError(AL_OUT_OF_MEMORY);
            return;
        }
        cap = (uint32_t)size;
    }
    memcpy(cpu, data, (size_t)size);

    /* Free previously attached PCM data and PRD table if re-specifying buffer data */
    release_data(buf);

    buf->cpu_data = cpu;
    buf->cpu_cap = cap;
    buf->held_bytes = cap;
    s_held_bytes += buf->held_bytes;
    buf->data_virt = cpu;
    buf->format = format;
    buf->frequency = freq;
    buf->size = size;
    buf->channels = channels;
    buf->bits = bits;
    buf->loop_start = 0;                                   /* AL_SOFT_loop_points: the whole buffer */
    buf->loop_end = (ALint)apu_vp_bytes_to_frames((uint32_t)size, channels, bits);
}

ALenum al_buffer_make_apu_ready(ALbuffer *buf) {
    if (!buf || buf->is_static || buf->cpu_data == NULL) {
        return AL_NO_ERROR;
    }

    size_t size = (size_t)buf->size;
    size_t prd_count = apu_prd_calculate_count(size);
    uint32_t data_phys = 0;
    /* The data is followed by silence that real-console one-shots loop over */
    uint32_t tail_bytes = apu_vp_tail_bytes(buf->channels, buf->bits);
    void *data_virt = apu_mem_alloc_phys(size + tail_bytes, 8, &data_phys);
    if (!data_virt) {
        return AL_OUT_OF_MEMORY;
    }

    uint32_t prd_table_phys = 0;
    size_t prd_table_bytes = prd_count * sizeof(NVAPU_PRD_ENTRY);
    NVAPU_PRD_ENTRY *prd_table_virt = (NVAPU_PRD_ENTRY *)apu_mem_alloc_phys(prd_table_bytes, 8, &prd_table_phys);
    if (!prd_table_virt) {
        apu_mem_free_phys(data_virt);
        return AL_OUT_OF_MEMORY;
    }

    if (apu_prd_build(data_phys, size, prd_table_virt, prd_count, NULL) != 0) {
        apu_mem_free_phys(data_virt);
        apu_mem_free_phys(prd_table_virt);
        return AL_INVALID_VALUE;
    }

    memcpy(data_virt, buf->cpu_data, size);
    memset((uint8_t *)data_virt + size, apu_vp_silence_byte(buf->bits), tail_bytes);

    free(buf->cpu_data);
    buf->cpu_data = NULL;
    buf->cpu_cap = 0;
    s_held_bytes -= buf->held_bytes;
    buf->held_bytes = (uint32_t)(size + tail_bytes + prd_table_bytes);
    s_held_bytes += buf->held_bytes;
    buf->data_virt = data_virt;
    buf->data_phys = data_phys;
    buf->prd_table_virt = prd_table_virt;
    buf->prd_table_phys = prd_table_phys;
    buf->prd_count = (uint32_t)prd_count;
    return AL_NO_ERROR;
}

/* Write the CPU cache back to memory: the APU reads RAM directly */
static void flush_cpu_cache(void) {
#if defined(OPENAL_TARGET_XBOX)
    __asm__ __volatile__("wbinvd" ::: "memory");
#endif
}

/*
 * AL_EXT_STATIC_BUFFER: the buffer plays the application's memory in place.
 * The APU maps it page by page (each 4 KiB page's physical address), so it need
 * not be physically contiguous. Only the last partial page is copied, into the
 * library's own page(s), followed by the silent tail a console one-shot needs.
 */
AL_API void AL_APIENTRY alBufferDataStatic(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size,
                                           ALsizei freq) {
    ALbuffer *buf;
    ALint channels, bits;
    ALsizei frame_size;
    uintptr_t start, end, last_page;
    uint32_t engine_pages, tail_bytes, tail_pages, count, i, tail_phys = 0;
    uint32_t *pages;
    uint8_t *tail;

    ensure_subsystem_initialized();
    buf = al_buffer_get(buffer);
    if (!buf) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (buf->ref_count > 0) {
        alSetError(AL_INVALID_OPERATION);
        return;
    }
    switch (format) {
        case AL_FORMAT_MONO8:    channels = 1; bits = 8;  frame_size = 1; break;
        case AL_FORMAT_MONO16:   channels = 1; bits = 16; frame_size = 2; break;
        case AL_FORMAT_STEREO8:  channels = 2; bits = 8;  frame_size = 2; break;
        case AL_FORMAT_STEREO16: channels = 2; bits = 16; frame_size = 4; break;
        case AL_FORMAT_MONO_XBOX_ADPCM:   channels = 1; bits = 4; frame_size = (ALsizei)APU_VP_ADPCM_BLOCK_BYTES; break;
        case AL_FORMAT_STEREO_XBOX_ADPCM: channels = 2; bits = 4; frame_size = (ALsizei)(2u * APU_VP_ADPCM_BLOCK_BYTES); break;
        default:
            alSetError(AL_INVALID_ENUM);
            return;
    }
    if (size <= 0 || freq <= 0 || (size % frame_size) != 0 || data == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /* The application's whole pages; the page holding the end (partial, or a
     * fresh one when the data ends on a page boundary) becomes the library's */
    start = (uintptr_t)data;
    end = start + (uintptr_t)size;
    last_page = end & ~(uintptr_t)0xFFF;
    engine_pages = (uint32_t)((last_page - (start & ~(uintptr_t)0xFFF)) >> 12);
    tail_bytes = (uint32_t)(end & 0xFFFu) + apu_vp_tail_bytes(channels, bits);
    tail_pages = (tail_bytes + 4095u) / 4096u;
    count = engine_pages + tail_pages;

    pages = (uint32_t *)malloc(count * sizeof(uint32_t));
    tail = (uint8_t *)apu_mem_alloc_phys(tail_pages * 4096u, 4096, &tail_phys);
    if (!pages || !tail) {
        free(pages);
        if (tail) apu_mem_free_phys(tail);
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }
    for (i = 0; i < engine_pages; i++) {
        uint32_t phys = apu_mem_get_physical_address((const void *)((start & ~(uintptr_t)0xFFF) + 4096u * i));
        if (phys == 0) {
            free(pages);
            apu_mem_free_phys(tail);
            alSetError(AL_INVALID_VALUE);   /* not RAM the APU can read */
            return;
        }
        pages[i] = phys & ~0xFFFu;
    }
    for (i = 0; i < tail_pages; i++) {
        pages[engine_pages + i] = tail_phys + 4096u * i;
    }
    /* The tail page holds the data's last bytes at their page offset, then silence */
    memset(tail, 0, tail_pages * 4096u);
    {
        uintptr_t from = (start > last_page) ? start : last_page;
        memcpy(tail + (from & 0xFFFu), (const void *)from, (size_t)(end - from));
    }
    memset(tail + (end & 0xFFFu), apu_vp_silence_byte(bits), apu_vp_tail_bytes(channels, bits));
    if (s_static_flush) {
        flush_cpu_cache();   /* the application's last writes reach RAM before the APU reads it */
    }

    release_data(buf);
    buf->is_static = true;
    buf->data_virt = (void *)(uintptr_t)data;
    buf->data_phys = 0;
    buf->pages = pages;
    buf->page_count = count;
    buf->page_offset = (uint32_t)(start & 0xFFFu);
    buf->tail_virt = tail;
    buf->map_key = s_next_map_key++;
    if (s_next_map_key == 0x80000000u) {
        s_next_map_key = 1;   /* keys are 31-bit (apu_vp.c makes them odd ids) */
    }
    buf->held_bytes = tail_pages * 4096u + count * (uint32_t)sizeof(uint32_t);
    s_held_bytes += buf->held_bytes;
    s_static_bytes += (uint32_t)size;
    buf->format = format;
    buf->frequency = freq;
    buf->size = size;
    buf->channels = channels;
    buf->bits = bits;
    buf->loop_start = 0;
    buf->loop_end = (ALint)apu_vp_bytes_to_frames((uint32_t)size, channels, bits);
}

AL_API void AL_APIENTRY alGetBufferi(ALuint buffer, ALenum param, ALint *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALbuffer *buf = al_buffer_get(buffer);
    if (!buf) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_FREQUENCY:
            *value = buf->frequency;
            break;
        case AL_BITS:
            *value = buf->bits;
            break;
        case AL_CHANNELS:
            *value = buf->channels;
            break;
        case AL_SIZE:
            *value = buf->size;
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetBufferf(ALuint buffer, ALenum param, ALfloat *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALbuffer *buf = al_buffer_get(buffer);
    if (!buf) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_FREQUENCY:
            *value = (ALfloat)buf->frequency;
            break;
        case AL_BITS:
            *value = (ALfloat)buf->bits;
            break;
        case AL_CHANNELS:
            *value = (ALfloat)buf->channels;
            break;
        case AL_SIZE:
            *value = (ALfloat)buf->size;
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetBufferiv(ALuint buffer, ALenum param, ALint *values) {
    if (param == AL_LOOP_POINTS_SOFT) {
        ALbuffer *buf;
        if (!values) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
        buf = al_buffer_get(buffer);
        if (!buf) {
            alSetError(AL_INVALID_NAME);
            return;
        }
        values[0] = buf->loop_start;
        values[1] = buf->loop_end;
        return;
    }
    alGetBufferi(buffer, param, values);
}

AL_API void AL_APIENTRY alGetBufferfv(ALuint buffer, ALenum param, ALfloat *values) {
    alGetBufferf(buffer, param, values);
}

/*
 * ============================================================================
 * Read-Only Buffer Parameter Setters (Generate AL_INVALID_ENUM per OpenAL 1.1)
 * ============================================================================
 */
AL_API void AL_APIENTRY alBufferf(ALuint buffer, ALenum param, ALfloat value) {
    (void)value;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alBuffer3f(ALuint buffer, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) {
    (void)v1; (void)v2; (void)v3;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alBufferfv(ALuint buffer, ALenum param, const ALfloat *values) {
    (void)values;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alBufferi(ALuint buffer, ALenum param, ALint value) {
    (void)value;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alBuffer3i(ALuint buffer, ALenum param, ALint v1, ALint v2, ALint v3) {
    (void)v1; (void)v2; (void)v3;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alBufferiv(ALuint buffer, ALenum param, const ALint *values) {
    ALbuffer *buf = al_buffer_get(buffer);
    if (!buf) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param != AL_LOOP_POINTS_SOFT) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    /* AL_SOFT_loop_points: not while a source uses the buffer */
    if (buf->ref_count > 0) {
        alSetError(AL_INVALID_OPERATION);
        return;
    }
    {
        ALint frames = (buf->channels > 0 && buf->bits > 0)
                           ? (ALint)apu_vp_bytes_to_frames((uint32_t)buf->size, buf->channels, buf->bits) : 0;
        if (values[0] < 0 || values[0] >= values[1] || values[1] > frames) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
    }
    buf->loop_start = values[0];
    buf->loop_end = values[1];
}

AL_API void AL_APIENTRY alGetBuffer3f(ALuint buffer, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3) {
    (void)v1; (void)v2; (void)v3;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}

AL_API void AL_APIENTRY alGetBuffer3i(ALuint buffer, ALenum param, ALint *v1, ALint *v2, ALint *v3) {
    (void)v1; (void)v2; (void)v3;
    if (!alIsBuffer(buffer)) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    (void)param;
    alSetError(AL_INVALID_ENUM);
}
