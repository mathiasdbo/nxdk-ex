#include "wav_loader.h"
#include <string.h>

#define FOURCC(a, b, c, d) \
    ((uint32_t)(uint8_t)(a) | ((uint32_t)(uint8_t)(b) << 8) | \
     ((uint32_t)(uint8_t)(c) << 16) | ((uint32_t)(uint8_t)(d) << 24))

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
}

int wav_parse(const void *data, size_t size, wav_info_t *out_info) {
    if (!data || !out_info || size < 44) {
        return -1;
    }

    const uint8_t *bytes = (const uint8_t *)data;

    /* Check RIFF header */
    uint32_t riff_magic = read_u32_le(bytes);
    if (riff_magic != FOURCC('R', 'I', 'F', 'F')) {
        return -2;
    }

    uint32_t wave_magic = read_u32_le(bytes + 8);
    if (wave_magic != FOURCC('W', 'A', 'V', 'E')) {
        return -2;
    }

    bool has_fmt = false;
    bool has_data = false;
    size_t offset = 12;

    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    const void *pcm_ptr = NULL;
    uint32_t pcm_bytes = 0;

    while (offset + 8 <= size) {
        uint32_t chunk_id = read_u32_le(bytes + offset);
        uint32_t chunk_size = read_u32_le(bytes + offset + 4);
        size_t chunk_data_offset = offset + 8;

        if (chunk_data_offset + chunk_size > size) {
            /* Truncated chunk */
            break;
        }

        if (chunk_id == FOURCC('f', 'm', 't', ' ')) {
            if (chunk_size >= 16) {
                audio_format = read_u16_le(bytes + chunk_data_offset);
                num_channels = read_u16_le(bytes + chunk_data_offset + 2);
                sample_rate = read_u32_le(bytes + chunk_data_offset + 4);
                bits_per_sample = read_u16_le(bytes + chunk_data_offset + 14);
                has_fmt = true;
            }
        } else if (chunk_id == FOURCC('d', 'a', 't', 'a')) {
            pcm_ptr = bytes + chunk_data_offset;
            pcm_bytes = chunk_size;
            has_data = true;
        }

        /* Chunks are padded to word boundaries (2 bytes) */
        size_t padded_size = chunk_size + (chunk_size & 1);
        offset = chunk_data_offset + padded_size;
    }

    if (!has_fmt) {
        return -3;
    }

    /* Enforce 16-bit uncompressed PCM */
    if (audio_format != 1 || bits_per_sample != 16 || (num_channels != 1 && num_channels != 2)) {
        return -4;
    }

    if (!has_data || !pcm_ptr || pcm_bytes == 0) {
        return -5;
    }

    out_info->pcm_data = pcm_ptr;
    out_info->pcm_size = (ALsizei)pcm_bytes;
    out_info->sample_rate = (ALsizei)sample_rate;
    out_info->channels = (ALsizei)num_channels;
    out_info->bits_per_sample = (ALsizei)bits_per_sample;
    out_info->format = (num_channels == 1) ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;

    return 0;
}

int wav_load_to_buffer(ALuint buffer, const void *data, size_t size) {
    wav_info_t info;
    int err = wav_parse(data, size, &info);
    if (err != 0) {
        return err;
    }

    /* Clear any preexisting error state */
    while (alGetError() != AL_NO_ERROR) {}

    alBufferData(buffer, info.format, info.pcm_data, info.pcm_size, info.sample_rate);
    if (alGetError() != AL_NO_ERROR) {
        return -6;
    }

    return 0;
}
