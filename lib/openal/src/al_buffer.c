#include "al_buffer.h"
#include <AL/alext.h>
#include "apu_vp.h"
#include <string.h>

/*
 * ============================================================================
 * Internal State & Global Buffer Pool
 * ============================================================================
 */
static ALbuffer s_buffers[AL_MAX_BUFFERS];
static bool s_subsystem_initialized = false;
static ALenum s_last_error = AL_NO_ERROR;

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
    }
    s_subsystem_initialized = true;
    s_last_error = AL_NO_ERROR;
}

void al_buffer_cleanup_subsystem(void) {
    for (size_t i = 0; i < AL_MAX_BUFFERS; i++) {
        if (s_buffers[i].in_use) {
            if (s_buffers[i].data_virt) {
                apu_mem_free_phys(s_buffers[i].data_virt);
                s_buffers[i].data_virt = NULL;
                s_buffers[i].data_phys = 0;
            }
            if (s_buffers[i].prd_table_virt) {
                apu_mem_free_phys(s_buffers[i].prd_table_virt);
                s_buffers[i].prd_table_virt = NULL;
                s_buffers[i].prd_table_phys = 0;
            }
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
            if (buf->data_virt) {
                apu_mem_free_phys(buf->data_virt);
                buf->data_virt = NULL;
                buf->data_phys = 0;
            }
            if (buf->prd_table_virt) {
                apu_mem_free_phys(buf->prd_table_virt);
                buf->prd_table_virt = NULL;
                buf->prd_table_phys = 0;
            }
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
        default:
            alSetError(AL_INVALID_ENUM);
            return;
    }

    if (size <= 0 || freq <= 0 || (size % frame_size) != 0 || data == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    size_t prd_count = apu_prd_calculate_count((size_t)size);
    if (prd_count == 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    uint32_t data_phys = 0;
    /* The data is followed by APU_VP_SILENT_TAIL_BYTES of silence that real-console one-shots loop over */
    void *data_virt = apu_mem_alloc_phys((size_t)size + APU_VP_SILENT_TAIL_BYTES, 8, &data_phys);
    if (!data_virt) {
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }

    uint32_t prd_table_phys = 0;
    size_t prd_table_bytes = prd_count * sizeof(NVAPU_PRD_ENTRY);
    NVAPU_PRD_ENTRY *prd_table_virt = (NVAPU_PRD_ENTRY *)apu_mem_alloc_phys(prd_table_bytes, 8, &prd_table_phys);
    if (!prd_table_virt) {
        apu_mem_free_phys(data_virt);
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }

    int prd_res = apu_prd_build(data_phys, (size_t)size, prd_table_virt, prd_count, NULL);
    if (prd_res != 0) {
        apu_mem_free_phys(data_virt);
        apu_mem_free_phys(prd_table_virt);
        alSetError(AL_INVALID_VALUE);
        return;
    }

    memcpy(data_virt, data, (size_t)size);
    memset((uint8_t *)data_virt + size, (bits == 8) ? 0x80 : 0, APU_VP_SILENT_TAIL_BYTES);   /* U8 silence is 0x80 */

    /* Free previously attached PCM data and PRD table if re-specifying buffer data */
    if (buf->data_virt) {
        apu_mem_free_phys(buf->data_virt);
    }
    if (buf->prd_table_virt) {
        apu_mem_free_phys(buf->prd_table_virt);
    }

    buf->data_virt = data_virt;
    buf->data_phys = data_phys;
    buf->prd_table_virt = prd_table_virt;
    buf->prd_table_phys = prd_table_phys;
    buf->prd_count = (uint32_t)prd_count;
    buf->format = format;
    buf->frequency = freq;
    buf->size = size;
    buf->channels = channels;
    buf->bits = bits;
    buf->loop_start = 0;                                   /* AL_SOFT_loop_points: the whole buffer */
    buf->loop_end = size / frame_size;
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
        ALint frames = (buf->channels > 0 && buf->bits > 0) ? buf->size / (buf->channels * (buf->bits / 8)) : 0;
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
