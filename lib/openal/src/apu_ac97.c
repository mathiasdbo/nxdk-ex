#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_platform.h"
#include <string.h>

static bool s_active;
static apu_ac97_stats_t s_stats;

#if defined(OPENAL_TARGET_XBOX)

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#define CHUNK_BYTES   (APU_AC97_CHUNK_FRAMES * 4u)
#define SLOTS         (APU_AC97_AHEAD + 4u)   /* queued + playing + spare */

#define AC97_PO_CIV   0x114u   /* PCM out: current index value */
#define AC97_PO_LVI   0x115u   /* PCM out: last valid index */
#define AC97_PO_SR    0x116u   /* PCM out: status */
#define AC97_SR_DCH   0x01u    /* DMA controller halted */

static int16_t *s_slot[SLOTS];
static uint32_t s_next;     /* slot being filled */
static uint32_t s_fill;     /* frames in it */

static volatile uint8_t *ac97_regs(void) {
    extern AC97_DEVICE ac97Device;   /* nxdk hal/audio.c */
    return (volatile uint8_t *)ac97Device.mmio;
}

static void free_slots(void) {
    uint32_t i;
    for (i = 0; i < SLOTS; i++) {
        if (s_slot[i]) {
            MmFreeContiguousMemory(s_slot[i]);
            s_slot[i] = NULL;
        }
    }
}

int apu_ac97_start(uintptr_t bar0) {
    uint32_t i;

    if (s_active) {
        return 0;
    }
    for (i = 0; i < SLOTS; i++) {
        s_slot[i] = (int16_t *)MmAllocateContiguousMemoryEx(CHUNK_BYTES, 0, 0x03FFAFFF, 0,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_slot[i]) {
            free_slots();
            return -1;
        }
        memset(s_slot[i], 0, CHUNK_BYTES);
    }
    memset(&s_stats, 0, sizeof(s_stats));
    XAudioInit(16, 2, NULL, NULL);
    /* A little silence first so the first pump does not find the DMA halted */
    for (s_next = 0; s_next < 2u; s_next++) {
        XAudioProvideSamples((unsigned char *)s_slot[s_next], (unsigned short)CHUNK_BYTES, 0);
    }
    s_fill = 0;
    apu_gp_fifo_discard(bar0);
    XAudioPlay();
    s_active = true;
    return 0;
}

void apu_ac97_pump(uintptr_t bar0) {
    volatile uint8_t *pb;
    bool halted;
    uint32_t guard;

    if (!s_active) {
        return;
    }
    pb = ac97_regs();
    halted = (pb[AC97_PO_SR] & AC97_SR_DCH) != 0;
    for (guard = 0; guard < SLOTS; guard++) {
        int16_t *dst = s_slot[s_next % SLOTS];
        if (((pb[AC97_PO_LVI] - pb[AC97_PO_CIV]) & 31u) >= APU_AC97_AHEAD && !halted) {
            break;   /* enough queued: leave the rest in the GP ring */
        }
        s_fill += apu_gp_fifo_read(bar0, dst + 2u * s_fill, APU_AC97_CHUNK_FRAMES - s_fill);
        if (s_fill < APU_AC97_CHUNK_FRAMES) {
            break;   /* the GP has not written a whole buffer yet */
        }
        XAudioProvideSamples((unsigned char *)dst, (unsigned short)CHUNK_BYTES, 0);
        s_next++;
        s_fill = 0;
        s_stats.chunks++;
    }
    /* The GP is not delivering (stalled, or the app called too rarely): top
     * the queue up with the partial buffer padded with silence, rather than
     * let the codec run dry and replay old buffers */
    if (((pb[AC97_PO_LVI] - pb[AC97_PO_CIV]) & 31u) < 2u) {
        int16_t *dst = s_slot[s_next % SLOTS];
        memset(dst + 2u * s_fill, 0, (APU_AC97_CHUNK_FRAMES - s_fill) * 4u);
        XAudioProvideSamples((unsigned char *)dst, (unsigned short)CHUNK_BYTES, 0);
        s_next++;
        s_fill = 0;
        s_stats.chunks++;
        s_stats.padded++;
    }
    if (halted) {
        s_stats.underruns++;
        XAudioPlay();
    }
}

void apu_ac97_stop(void) {
    if (!s_active) {
        return;
    }
    XAudioPause();
    s_active = false;
    free_slots();
}

#else /* host: no codec */

int apu_ac97_start(uintptr_t bar0) {
    (void)bar0;
    return 0;
}

void apu_ac97_pump(uintptr_t bar0) {
    (void)bar0;
}

void apu_ac97_stop(void) {
}

#endif

bool apu_ac97_active(void) {
    return s_active;
}

void apu_ac97_get_stats(apu_ac97_stats_t *out) {
    if (out) {
        *out = s_stats;
    }
}
