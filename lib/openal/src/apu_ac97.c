#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_platform.h"
#include <string.h>

static bool s_active;
static bool s_use_thread = true;
static apu_ac97_stats_t s_stats;

static void (*volatile s_hook)(void);
static volatile apu_ac97_output_fn s_out_fn;
static void *volatile s_out_user;

void apu_ac97_set_output_hook(apu_ac97_output_fn fn, void *user) {
    s_out_fn = NULL;   /* never call the new function with the old argument */
    s_out_user = user;
    s_out_fn = fn;
}

void apu_ac97_set_thread(bool on) {
    s_use_thread = on;
}

void apu_ac97_set_thread_hook(void (*fn)(void)) {
    s_hook = fn;
}

#if defined(OPENAL_TARGET_XBOX)

#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define CHUNK_BYTES   (APU_AC97_CHUNK_FRAMES * 4u)
#define SLOTS         (APU_AC97_AHEAD + 4u)   /* queued + playing + spare */

#define AC97_PO_CIV   0x114u   /* PCM out: current index value */
#define AC97_PO_LVI   0x115u   /* PCM out: last valid index */
#define AC97_PO_SR    0x116u   /* PCM out: status */
#define AC97_SR_DCH   0x01u    /* DMA controller halted */
#define AC97_PO_CR    0x11Bu   /* PCM out: control (bit 0 run, bit 1 reset registers) */

static int16_t *s_slot[SLOTS];   /* AC97 buffers (write-combining) */
static int16_t s_stage[2u * APU_AC97_CHUNK_FRAMES];   /* the block being gathered (cached) */
static uint32_t s_next;     /* slot the next block goes to */
static uint32_t s_fill;     /* frames gathered */
static DWORD s_last_pump;   /* GetTickCount() of the previous pass */
static uint64_t s_tsc0;     /* cycle counter and performance counter at the start */
static LARGE_INTEGER s_qpc0;

/* Pump thread */
static HANDLE s_thread;
static uintptr_t s_thread_bar0;
static volatile LONG s_quit;

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

/* A gathered block: the output hook, then into the next AC97 buffer */
static void queue_block(void) {
    apu_ac97_output_fn fn = s_out_fn;
    int16_t *dst = s_slot[s_next % SLOTS];
    uint64_t t1;
    if (fn) {
        uint64_t t0 = apu_platform_cycles();
        fn(s_stage, APU_AC97_CHUNK_FRAMES, s_out_user);
        s_stats.hook_cycles += apu_platform_cycles() - t0;
    }
    t1 = apu_platform_cycles();
    memcpy(dst, s_stage, CHUNK_BYTES);
    XAudioProvideSamples((unsigned char *)dst, (unsigned short)CHUNK_BYTES, 0);
    s_stats.queue_cycles += apu_platform_cycles() - t1;
    s_next++;
    s_fill = 0;
    s_stats.chunks++;
}

/* One pass: queue what the GP has written, pad if the codec is about to run dry */
static void pump_once(uintptr_t bar0) {
    volatile uint8_t *pb = ac97_regs();
    bool halted = (pb[AC97_PO_SR] & AC97_SR_DCH) != 0;
    DWORD now = GetTickCount();
    uint32_t guard, queued, queued0;

    if (s_stats.pumps != 0u && now - s_last_pump > s_stats.max_gap_ms) {
        s_stats.max_gap_ms = now - s_last_pump;
    }
    s_last_pump = now;
    s_stats.pumps++;

    /* The codec's queue is read once for the fill and counted here as
     * buffers go on it: each register read is a bus transaction. The codec
     * may take one buffer (10.7 ms) off it during the pass, so the count can
     * be one high: the fill then stops with APU_AC97_AHEAD - 1 queued, still
     * more than a 4 ms pass needs */
    queued = (pb[AC97_PO_LVI] - pb[AC97_PO_CIV]) & 31u;
    queued0 = queued;
    for (guard = 0; guard < SLOTS; guard++) {
        uint64_t t0;
        if (queued >= APU_AC97_AHEAD && !halted) {
            break;   /* enough queued: leave the rest in the GP ring */
        }
        t0 = apu_platform_cycles();
        s_fill += apu_gp_fifo_read(bar0, s_stage + 2u * s_fill, APU_AC97_CHUNK_FRAMES - s_fill);
        s_stats.fifo_cycles += apu_platform_cycles() - t0;
        if (s_fill < APU_AC97_CHUNK_FRAMES) {
            break;   /* the GP has not written a whole buffer yet */
        }
        queue_block();
        queued++;
    }
    /* The GP is not delivering (stalled, or the pump ran too rarely) and the
     * codec is on its last buffer: top the queue up with the partial buffer
     * padded with silence, rather than let it run dry and replay old buffers.
     * A buffer queued in this pass is still on the queue; with none, the
     * snapshot may be stale, so the queue is read again */
    if (queued == queued0) {
        queued = (pb[AC97_PO_LVI] - pb[AC97_PO_CIV]) & 31u;
    }
    if (queued == 0u) {
        memset(s_stage + 2u * s_fill, 0, (APU_AC97_CHUNK_FRAMES - s_fill) * 4u);
        queue_block();
        s_stats.padded++;
    }
    if (halted) {
        s_stats.underruns++;
        XAudioPlay();
    }
}

/* One pass with its CPU time counted */
static void timed_pass(uintptr_t bar0, void (*hook)(void)) {
    uint64_t t0 = apu_platform_cycles(), dt;
    pump_once(bar0);
    if (hook) {
        uint64_t t1 = apu_platform_cycles();
        hook();
        s_stats.thread_hook_cycles += apu_platform_cycles() - t1;
    }
    dt = apu_platform_cycles() - t0;
    s_stats.cycles += dt;
    if (dt > s_stats.max_cycles) {
        s_stats.max_cycles = (dt > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)dt;
    }
}

static DWORD WINAPI pump_thread(LPVOID arg) {
    (void)arg;
    while (!s_quit) {
        /* The scrub every second pass (8 ms): it only guards against an
         * application that stops feeding a stream, with half a ring of slack */
        timed_pass(s_thread_bar0, (s_stats.pumps & 1u) ? s_hook : NULL);
        Sleep(APU_AC97_PUMP_MS);
    }
    return 0;
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
    s_tsc0 = apu_platform_cycles();
    QueryPerformanceCounter(&s_qpc0);
    XAudioInit(16, 2, NULL, NULL);
    /* Silence first: this is the lead the GP output keeps over the codec, and
     * it must outlast the longest gap between two pumps (a 60 Hz frame is
     * 16.7 ms without the thread); with too little, every pump found the queue
     * nearly empty */
    for (s_next = 0; s_next < APU_AC97_PRIME; s_next++) {
        XAudioProvideSamples((unsigned char *)s_slot[s_next], (unsigned short)CHUNK_BYTES, 0);
    }
    s_fill = 0;
    apu_gp_fifo_discard(bar0);
    XAudioPlay();
    s_active = true;

    s_thread = NULL;
    if (s_use_thread) {
        s_quit = 0;
        s_thread_bar0 = bar0;
        s_thread = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
        if (s_thread) {
            /* Above the game: a late pass is an audible gap */
            SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);
            s_stats.threaded = true;
        }
    }
    return 0;
}

void apu_ac97_pump(uintptr_t bar0) {
    if (!s_active || s_thread) {
        return;   /* the thread owns the AC97 and the FIFO read position */
    }
    timed_pass(bar0, NULL);
}

static uint32_t cycle_hz(void) {
    LARGE_INTEGER now, freq;
    uint64_t dq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    dq = (uint64_t)(now.QuadPart - s_qpc0.QuadPart);
    if (!s_active || freq.QuadPart <= 0 || dq < (uint64_t)freq.QuadPart / 100u) {
        return 0;
    }
    return (uint32_t)((double)(apu_platform_cycles() - s_tsc0) * (double)freq.QuadPart / (double)dq);
}

void apu_ac97_stop(void) {
    if (!s_active) {
        return;
    }
    if (s_thread) {
        s_quit = 1;
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
        s_stats.threaded = false;
    }
    XAudioPause();
    s_active = false;
    free_slots();
}

void apu_ac97_halt_dma(void) {
    volatile uint8_t *pb = ac97_regs();
    if (!pb) {
        return;
    }
    pb[AC97_PO_CR] = 0;       /* stop the PCM-out DMA */
    pb[AC97_PO_CR] = 0x02u;   /* and reset its registers: it no longer reads the old buffers */
}

#else /* host: no codec */

static uint32_t cycle_hz(void) {
    return 0;
}

void apu_ac97_halt_dma(void) {
}

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
        out->cycle_hz = cycle_hz();
    }
}
