#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <AL/al.h>
#include <AL/alext.h>
#include "al_buffer.h"
#include "apu_mem.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

/*
 * AL_EXT_STATIC_BUFFER (alBufferDataStatic): the page list a static buffer
 * hands the voice (the application's pages, then the library's copy of the last
 * partial page with the silent tail), the VP's mapping of such a list, and the
 * memory accounting. On the host a page's "physical" address is its virtual
 * address (apu_mem_get_physical_address outside the pool).
 */

#define BAR0_SIZE 0x80000u

static uint8_t *s_bar0;

static uint32_t voice_dw(uint32_t h, uint32_t off) {
    return *(uint32_t *)(apu_vp_debug_voice_record(h) + off);
}

/* The data a static buffer of `size` bytes at `data` reads, through its page list */
static void check_pages(ALuint id, const uint8_t *data, uint32_t size, int bits) {
    ALbuffer *b = al_buffer_get(id);
    uintptr_t start = (uintptr_t)data, end = start + size;
    uint32_t engine = (uint32_t)(((end & ~(uintptr_t)0xFFF) - (start & ~(uintptr_t)0xFFF)) >> 12), i;
    const uint8_t *tail;
    assert(b && b->is_static && b->data_virt == data && b->data_phys == 0);
    assert(b->page_offset == (uint32_t)(start & 0xFFFu));
    assert(b->map_key != 0 && b->page_count >= engine + 1u);
    for (i = 0; i < engine; i++) {
        assert(b->pages[i] == apu_mem_get_physical_address((const void *)((start & ~(uintptr_t)0xFFF) + 4096u * i)));
    }
    tail = (const uint8_t *)b->tail_virt;
    assert(b->pages[engine] == apu_mem_get_physical_address(tail));
    /* the last page: the data's last bytes at their offset, then silence */
    {
        uintptr_t from = (start > (end & ~(uintptr_t)0xFFF)) ? start : (end & ~(uintptr_t)0xFFF);
        assert(memcmp(tail + (from & 0xFFFu), (const void *)from, (size_t)(end - from)) == 0);
        for (i = 0; i < APU_VP_SILENT_TAIL_BYTES; i++) {
            assert(tail[(end & 0xFFFu) + i] == ((bits == 8) ? 0x80 : 0));
        }
        assert((end & 0xFFFu) + APU_VP_SILENT_TAIL_BYTES <= (b->page_count - engine) * 4096u);
    }
}

int main(void) {
    uint8_t *mem = (uint8_t *)malloc(5u * 4096u + 64u);
    uint8_t *data;
    uint32_t i;
    ALuint bufs[3];
    ALint held0 = 0, held = 0, stat = 0, v = 0;

    printf("=== AL_EXT_STATIC_BUFFER ===\n");
    assert(mem);
    al_buffer_debug_set_static_flush(false);   /* no cache to write back on the host */
    /* data starting 0x123 into a page and ending inside the fourth page */
    data = (uint8_t *)((((uintptr_t)mem + 4095u) & ~(uintptr_t)4095u) + 0x123u);
    for (i = 0; i < 3u * 4096u + 0x200u; i++) data[i] = (uint8_t)(i * 7u + 3u);

    printf("[1] Page list and the copied last page...\n");
    alGenBuffers(3, bufs);
    alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES, &held0);
    alBufferDataStatic(bufs[0], AL_FORMAT_MONO16, data, 3 * 4096 + 0x200, 11025);
    assert(alGetError() == AL_NO_ERROR);
    check_pages(bufs[0], data, 3u * 4096u + 0x200u, 16);
    assert(al_buffer_get(bufs[0])->page_count == 4u);          /* 3 of the application's pages + 1 copied */
    alGetBufferi(bufs[0], AL_SIZE, &v);
    assert(v == 3 * 4096 + 0x200);
    alGetBufferi(bufs[0], AL_FREQUENCY, &v);
    assert(v == 11025);
    /* a sub-range inside one page (a word's middle): one copied page, nothing else */
    alBufferDataStatic(bufs[1], AL_FORMAT_MONO8, data + 0x40, 0x300, 11025);
    assert(alGetError() == AL_NO_ERROR);
    check_pages(bufs[1], data + 0x40, 0x300u, 8);
    assert(al_buffer_get(bufs[1])->page_count == 1u);
    /* ending exactly on a page boundary: the tail is a fresh page of silence */
    {
        uint32_t len = (uint32_t)(4096u - 0x123u) + 4096u;
        alBufferDataStatic(bufs[2], AL_FORMAT_MONO8, data, (ALsizei)len, 22050);
        assert(alGetError() == AL_NO_ERROR);
        check_pages(bufs[2], data, len, 8);
        assert(al_buffer_get(bufs[2])->page_count == 3u);
    }
    printf("    -> application pages in place, last partial page copied with its silent tail [PASS]\n");

    printf("[2] Memory accounting and errors...\n");
    alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES, &held);
    alXboxGetHardwareStatus(AL_XBOX_STATIC_BUFFER_BYTES, &stat);
    assert(stat == (3 * 4096 + 0x200) + 0x300 + (int)((4096u - 0x123u) + 4096u));
    assert(held - held0 == 3 * 4096 + (4 + 1 + 3) * (int)sizeof(uint32_t));   /* three tail pages and the lists */
    alBufferDataStatic(bufs[0], AL_FORMAT_MONO16, data, 3, 11025);           /* half a frame */
    assert(alGetError() == AL_INVALID_VALUE);
    alBufferDataStatic(bufs[0], AL_FORMAT_MONO16, NULL, 64, 11025);
    assert(alGetError() == AL_INVALID_VALUE);
    alBufferDataStatic(bufs[0], 0x1234, data, 64, 11025);
    assert(alGetError() == AL_INVALID_ENUM);
    alBufferDataStatic(9999, AL_FORMAT_MONO16, data, 64, 11025);
    assert(alGetError() == AL_INVALID_NAME);
    /* a copied buffer replacing a static one gives the static memory back */
    alBufferData(bufs[1], AL_FORMAT_MONO8, data, 0x300, 11025);
    alXboxGetHardwareStatus(AL_XBOX_STATIC_BUFFER_BYTES, &stat);
    assert(stat == (3 * 4096 + 0x200) + (int)((4096u - 0x123u) + 4096u));
    assert(!al_buffer_get(bufs[1])->is_static);
    alDeleteBuffers(3, bufs);
    alXboxGetHardwareStatus(AL_XBOX_BUFFER_BYTES, &held);
    alXboxGetHardwareStatus(AL_XBOX_STATIC_BUFFER_BYTES, &stat);
    assert(held == held0 && stat == 0);
    printf("    -> held and referenced bytes follow the buffers; bad sizes and formats refused [PASS]\n");

    printf("[3] The VP maps a page list...\n");
    {
        uintptr_t base;
        apu_vp_voice_params_t p;
        static const uint32_t list[3] = { 0x01234000u, 0x00777000u, 0x02000000u };   /* not contiguous */
        const uint32_t *sge;
        uint32_t ba, ba2;
        s_bar0 = (uint8_t *)calloc(1, BAR0_SIZE);
        base = (uintptr_t)s_bar0;
        assert(apu_mem_init(0) == 0);
        assert(apu_vp_init(base) == 0);
        memset(&p, 0, sizeof(p));
        p.pages = list;
        p.page_count = 3;
        p.map_key = 41;
        p.phys = 0x2A0;                 /* first byte's offset in list[0] */
        p.bytes = 2u * 4096u;
        p.channels = 1;
        p.bits = 16;
        p.tail_bytes = APU_VP_SILENT_TAIL_BYTES;
        p.hrtf_entry = -1;
        for (i = 0; i < APU_VP_OUTPUTS; i++) p.vols[i] = 0xFFFu;
        assert(apu_vp_voice_start(base, 70, &p) == 0);
        ba = voice_dw(70, MCPX_VOICE_CUR_PSL_START) & 0xFFFFFFu;
        assert((ba & 0xFFFu) == 0x2A0u);
        sge = apu_vp_debug_sge();
        for (i = 0; i < 3u; i++) {
            assert(sge[2u * ((ba >> 12) + i)] == list[i]);   /* one entry per listed page, in order */
        }
        assert(apu_vp_voice_start(base, 71, &p) == 0);        /* the same key: the mapping is reused */
        assert((voice_dw(71, MCPX_VOICE_CUR_PSL_START) & 0xFFFFFFu) == ba);
        p.map_key = 42;                                       /* another list */
        assert(apu_vp_voice_start(base, 72, &p) == 0);
        ba2 = voice_dw(72, MCPX_VOICE_CUR_PSL_START) & 0xFFFFFFu;
        assert((ba2 >> 12) != (ba >> 12));
        p.bytes = 3u * 4096u;                                 /* data + tail beyond the list: refused */
        assert(apu_vp_voice_start(base, 73, &p) != 0);
        p.bytes = 4096u;
        p.map_key = 0;                                        /* a list needs a key */
        assert(apu_vp_voice_start(base, 73, &p) != 0);
        /* Xbox ADPCM: 36-byte blocks of 64 samples per channel; EBO counts samples */
        p.map_key = 43;
        p.bits = 4;
        p.channels = 2;
        p.bytes = 40u * 72u;                                  /* 40 stereo blocks */
        p.loop = true;
        p.tail_bytes = apu_vp_tail_bytes(2, 4);
        assert(p.tail_bytes == 8u * 72u);                     /* eight silent blocks */
        assert(apu_vp_voice_start(base, 74, &p) == 0);
        assert(voice_dw(74, MCPX_VOICE_PAR_NEXT) == 40u * 64u - 1u);
        assert((voice_dw(74, MCPX_VOICE_CFG_FMT) & (3u << 30)) == MCPX_FMT_CONTAINER_ADPCM);
        assert(voice_dw(74, MCPX_VOICE_CFG_FMT) & MCPX_FMT_STEREO);
        assert(apu_vp_bytes_to_frames(36u, 1, 4) == 64u && apu_vp_frames_to_bytes(100u, 1, 4) == 36u);
        assert(apu_vp_frames_to_bytes(100u, 2, 16) == 400u && apu_vp_tail_bytes(1, 8) == APU_VP_SILENT_TAIL_BYTES);
        apu_vp_deinit(base);
        apu_mem_shutdown();
        free(s_bar0);
    }
    printf("    -> one SGE entry per page in order, cached by key, overruns refused [PASS]\n");

    free(mem);
    printf("=== All Static Buffer Tests Passed Successfully! ===\n");
    return 0;
}
