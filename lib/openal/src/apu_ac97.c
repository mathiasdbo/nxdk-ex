#include "apu_ac97.h"
#include "apu_gp.h"
#include "apu_vp.h"
#include "apu_platform.h"
#include <string.h>

static bool s_active;
static bool s_zc;                 /* running in zero-copy mode */
static volatile bool s_zc_want = true;   /* apu_ac97_set_zero_copy() */
static apu_ac97_stats_t s_stats;

static void (*volatile s_hook)(void);
static volatile apu_ac97_output_fn s_out_fn;
static void *volatile s_out_user;

static void mode_check(void);

void apu_ac97_set_output_hook(apu_ac97_output_fn fn, void *user) {
    s_out_fn = NULL;   /* never call the new function with the old argument */
    s_out_user = user;
    s_out_fn = fn;
    mode_check();      /* the hook needs the copy mode */
}

void apu_ac97_set_zero_copy(bool on) {
    s_zc_want = on;
}

void apu_ac97_set_thread_hook(void (*fn)(void)) {
    s_hook = fn;
}

/* ------------------------------------------------- zero-copy arithmetic */

uint32_t apu_ac97_zc_first_piece(uint32_t w, uint32_t ring) {
    return ((w % ring + ring - ring / 2u) % ring) / APU_AC97_ZC_PIECE_BYTES;
}

uint32_t apu_ac97_zc_lead(uint32_t w, uint32_t r, uint32_t ring) {
    return (w % ring + ring - r % ring) % ring;
}

int apu_ac97_zc_slip(uint32_t lead, uint32_t ring) {
    if (lead < ring / 4u) {
        return -1;
    }
    if (lead > ring - ring / 4u) {
        return 1;
    }
    return 0;
}

#if defined(OPENAL_TARGET_XBOX)

#include <hal/audio.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define CHUNK_BYTES   (APU_AC97_CHUNK_FRAMES * 4u)
#define SLOTS         (APU_AC97_AHEAD + 4u)   /* queued + playing + spare */

#define AC97_PO_BDBAR 0x110u   /* PCM out: buffer list base */
#define AC97_PO_CIV   0x114u   /* PCM out: current index value */
#define AC97_PO_LVI   0x115u   /* PCM out: last valid index */
#define AC97_PO_SR    0x116u   /* PCM out: status */
#define AC97_PO_PICB  0x118u   /* PCM out: 16-bit samples left in the current buffer */
#define AC97_PO_CR    0x11Bu   /* PCM out: control (bit 0 run, bit 1 reset registers) */
#define AC97_SP_BDBAR 0x170u   /* S/PDIF out: the same registers */
#define AC97_SP_CIV   0x174u
#define AC97_SP_LVI   0x175u
#define AC97_SP_SR    0x176u
#define AC97_SP_CR    0x17Bu
#define AC97_SR_DCH   0x01u    /* DMA controller halted */
#define AC97_CR_RUN   0x01u
#define AC97_CR_RESET 0x02u

#define ZC_ENTRIES    32u
#define ZC_SAMPLES    (APU_AC97_ZC_PIECE_BYTES / 2u)   /* 16-bit samples per piece */

static bool s_use_thread = true;
static uintptr_t s_bar0;

/* Copy mode */
static int16_t *s_slot[SLOTS];   /* AC97 buffers (write-combining) */
static int16_t s_stage[2u * APU_AC97_CHUNK_FRAMES];   /* the block being gathered (cached) */
static uint32_t s_next;     /* slot the next block goes to */
static uint32_t s_fill;     /* frames gathered */

/* Zero-copy mode */
static uint32_t s_zc_ring;      /* ring bytes */
static uint32_t s_zc_pieces;    /* ring / piece */
static uint32_t s_zc_phys;      /* ring physical address */
static uint32_t s_zc_next;      /* piece the next refilled entry gets */
static uint32_t s_zc_tail;      /* entry after LVI: the next one refilled */
static uint32_t s_zc_hold;      /* entries to play before another slip may happen */
static uint8_t s_zc_piece_of[ZC_ENTRIES];

static DWORD s_last_pump;   /* GetTickCount() of the previous pass */
static uint64_t s_tsc0;     /* cycle counter and performance counter at the start */
static LARGE_INTEGER s_qpc0;

static bool s_codec_up;   /* XAudioInit has run */
static void codec_open(void);

/* Pump thread */
static HANDLE s_thread;
static volatile LONG s_quit;

void apu_ac97_set_thread(bool on) {
    s_use_thread = on;
}

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

/* Pass bookkeeping shared by both modes */
static void note_pass(void) {
    DWORD now = GetTickCount();
    if (s_stats.pumps != 0u && now - s_last_pump > s_stats.max_gap_ms) {
        s_stats.max_gap_ms = now - s_last_pump;
    }
    s_last_pump = now;
    s_stats.pumps++;
}

/* ------------------------------------------------------------ copy mode */

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
static void copy_pass(uintptr_t bar0) {
    volatile uint8_t *pb = ac97_regs();
    bool halted = (pb[AC97_PO_SR] & AC97_SR_DCH) != 0;
    uint32_t guard, queued, queued0;

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

static int copy_start(uintptr_t bar0) {
    uint32_t i;
    for (i = 0; i < SLOTS; i++) {
        s_slot[i] = (int16_t *)MmAllocateContiguousMemoryEx(CHUNK_BYTES, 0, 0x03FFAFFF, 0,
                                                            PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!s_slot[i]) {
            free_slots();
            return -1;
        }
        memset(s_slot[i], 0, CHUNK_BYTES);
    }
    codec_open();
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
    return 0;
}

/* ------------------------------------------------------- zero-copy mode */

/* Entry i of both buffer lists plays piece p of the GP ring; no interrupt on completion */
static void zc_set_entry(uint32_t i, uint32_t p) {
    extern AC97_DEVICE ac97Device;
    uint32_t a = s_zc_phys + p * APU_AC97_ZC_PIECE_BYTES;
    ac97Device.pcmOutDescriptor[i].bufferStartAddress = a;
    ac97Device.pcmOutDescriptor[i].bufferLengthInSamples = ZC_SAMPLES;
    ac97Device.pcmOutDescriptor[i].bufferControl = 0;
    ac97Device.pcmSpdifDescriptor[i].bufferStartAddress = a;
    ac97Device.pcmSpdifDescriptor[i].bufferLengthInSamples = ZC_SAMPLES;
    ac97Device.pcmSpdifDescriptor[i].bufferControl = 0;
    s_zc_piece_of[i] = (uint8_t)p;
}

/* Stop one DMA (cr: its control register; its status register is 5 bytes
 * before), wait until it has halted (an ICH must be halted before its
 * registers are reset, and it may still be fetching), then reset it */
static void dma_reset(volatile uint8_t *pb, uint32_t cr) {
    uint32_t guard;
    pb[cr] = 0;
    for (guard = 0; guard < 100000u && !(pb[cr - 5u] & AC97_SR_DCH); guard++) {
    }
    pb[cr] = AC97_CR_RESET;
    for (guard = 0; guard < 100000u && (pb[cr] & AC97_CR_RESET); guard++) {
    }
}

/*
 * Both DMAs reset and pointed at empty buffer lists. The codec cold reset and
 * the interrupt (XAudioInit) are done once: a console that cold-reset the
 * codec again on a mode switch left it silent, its DMA neither moving nor
 * halting, and the next XAudioInit waited for it forever
 */
static void codec_open(void) {
    extern AC97_DEVICE ac97Device;
    volatile uint8_t *pb;
    uint32_t i;
    if (!s_codec_up) {
        XAudioInit(16, 2, NULL, NULL);
        s_codec_up = true;
    }
    pb = ac97_regs();
    dma_reset(pb, AC97_PO_CR);
    dma_reset(pb, AC97_SP_CR);
    pb[AC97_PO_SR] = 0xFFu;
    pb[AC97_SP_SR] = 0xFFu;
    for (i = 0; i < ZC_ENTRIES; i++) {
        ac97Device.pcmOutDescriptor[i].bufferStartAddress = 0;
        ac97Device.pcmOutDescriptor[i].bufferLengthInSamples = 0;
        ac97Device.pcmOutDescriptor[i].bufferControl = 0;
        ac97Device.pcmSpdifDescriptor[i].bufferStartAddress = 0;
        ac97Device.pcmSpdifDescriptor[i].bufferLengthInSamples = 0;
        ac97Device.pcmSpdifDescriptor[i].bufferControl = 0;
    }
    *(volatile uint32_t *)(pb + AC97_PO_BDBAR) = MmGetPhysicalAddress((void *)&ac97Device.pcmOutDescriptor[0]);
    *(volatile uint32_t *)(pb + AC97_SP_BDBAR) = MmGetPhysicalAddress((void *)&ac97Device.pcmSpdifDescriptor[0]);
    ac97Device.nextDescriptor = 0;   /* XAudioProvideSamples starts at entry 0, where the reset CIV is */
}

/*
 * (Re)start both DMAs half a ring behind the GP's write position, with all 32
 * entries queued: the start, and the restart after the codec ran dry
 */
static void zc_begin(uintptr_t bar0) {
    extern AC97_DEVICE ac97Device;
    volatile uint8_t *pb = ac97_regs();
    uint32_t i, first;

    dma_reset(pb, AC97_PO_CR);
    dma_reset(pb, AC97_SP_CR);
    pb[AC97_PO_SR] = 0xFFu;   /* clear the status bits */
    pb[AC97_SP_SR] = 0xFFu;
    *(volatile uint32_t *)(pb + AC97_PO_BDBAR) = MmGetPhysicalAddress((void *)&ac97Device.pcmOutDescriptor[0]);
    *(volatile uint32_t *)(pb + AC97_SP_BDBAR) = MmGetPhysicalAddress((void *)&ac97Device.pcmSpdifDescriptor[0]);

    first = apu_ac97_zc_first_piece(apu_gp_fifo_write_pos(bar0), s_zc_ring);
    for (i = 0; i < ZC_ENTRIES; i++) {
        zc_set_entry(i, (first + i) % s_zc_pieces);
    }
    s_zc_next = (first + ZC_ENTRIES) % s_zc_pieces;
    s_zc_tail = 0;
    s_zc_hold = ZC_ENTRIES;
    pb[AC97_PO_LVI] = ZC_ENTRIES - 1u;
    pb[AC97_SP_LVI] = ZC_ENTRIES - 1u;
    pb[AC97_PO_CR] = AC97_CR_RUN;   /* run, interrupts off */
    pb[AC97_SP_CR] = AC97_CR_RUN;
}

static void zc_pass(uintptr_t bar0) {
    volatile uint8_t *pb = ac97_regs();
    uint32_t civ = 0, picb = 0, w = 0, r, lead, frames, n, i, guard;
    int slip;

    if (pb[AC97_PO_SR] & AC97_SR_DCH) {
        /* The codec reached the last valid entry: the pump was away for
         * longer than the queue (~85 ms). Start again, in phase */
        s_stats.underruns++;
        zc_begin(bar0);
        return;
    }

    /* The codec's read position: CIV and PICB of the same buffer */
    for (guard = 0; guard < 4u; guard++) {
        civ = pb[AC97_PO_CIV] & 31u;
        picb = *(volatile uint16_t *)(pb + AC97_PO_PICB);
        w = apu_gp_fifo_write_pos(bar0);
        if ((pb[AC97_PO_CIV] & 31u) == civ) {
            break;
        }
    }
    if (picb > ZC_SAMPLES) {
        picb = ZC_SAMPLES;
    }
    r = s_zc_piece_of[civ] * APU_AC97_ZC_PIECE_BYTES + (ZC_SAMPLES - picb) * 2u;
    lead = apu_ac97_zc_lead(w, r, s_zc_ring);
    frames = lead / 4u;
    s_stats.lead_last = frames;
    if (frames < s_stats.lead_min) s_stats.lead_min = frames;
    if (frames > s_stats.lead_max) s_stats.lead_max = frames;

    /* Entries played since the last pass: from the tail up to the one before
     * the codec's, or before the S/PDIF DMA's if that one is a little behind
     * (both start together on the same clock). An S/PDIF DMA further away is
     * not running in step: xemu has none, its CIV stays 0, and following it
     * would never refill anything */
    n = (civ - s_zc_tail) & 31u;
    if (!(pb[AC97_SP_SR] & AC97_SR_DCH)) {
        uint32_t sp = pb[AC97_SP_CIV] & 31u;
        if (((civ - sp) & 31u) <= 2u) {
            uint32_t n_sp = (sp - s_zc_tail) & 31u;
            if (n_sp < n) {
                n = n_sp;
            }
        }
    }

    /* Keep the lead in the middle half of the ring. A slip shows in the lead
     * only when the codec reaches the slipped entry, up to a queue later */
    s_zc_hold = (s_zc_hold > n) ? s_zc_hold - n : 0u;
    slip = s_zc_hold ? 0 : apu_ac97_zc_slip(lead, s_zc_ring);
    if (slip) {
        s_zc_next = (s_zc_next + s_zc_pieces + (uint32_t)slip) % s_zc_pieces;
        s_zc_hold = ZC_ENTRIES;
        s_stats.slips++;
    }

    for (i = 0; i < n; i++) {
        zc_set_entry(s_zc_tail, s_zc_next);
        s_zc_next = (s_zc_next + 1u) % s_zc_pieces;
        s_zc_tail = (s_zc_tail + 1u) & 31u;
        s_stats.chunks++;
    }
    if (n) {
        pb[AC97_PO_LVI] = (uint8_t)((s_zc_tail - 1u) & 31u);
        pb[AC97_SP_LVI] = (uint8_t)((s_zc_tail - 1u) & 31u);
    }
}

static int zc_start(uintptr_t bar0) {
    s_zc_ring = apu_gp_fifo_bytes();
    s_zc_pieces = s_zc_ring / APU_AC97_ZC_PIECE_BYTES;
    s_zc_phys = apu_gp_fifo_phys();
    codec_open();
    s_stats.lead_min = 0xFFFFFFFFu;   /* the lead of this zero-copy run */
    s_stats.lead_max = 0;
    s_stats.lead_last = 0;
    zc_begin(bar0);
    return 0;
}

/* --------------------------------------------------------------- common */

/* Zero-copy needs no hook and a ring of whole pieces, at least four, at most 256 */
static bool zc_wanted(void) {
    uint32_t ring = apu_gp_fifo_bytes();
    return s_zc_want && !s_out_fn && ring % APU_AC97_ZC_PIECE_BYTES == 0u &&
           ring / APU_AC97_ZC_PIECE_BYTES >= 4u && ring / APU_AC97_ZC_PIECE_BYTES <= 256u;
}

static void pump_once(uintptr_t bar0) {
    note_pass();
    if (s_zc) {
        zc_pass(bar0);
    } else {
        copy_pass(bar0);
    }
}

/* One pass with its CPU time counted */
static void timed_pass(uintptr_t bar0, void (*hook)(void)) {
    uint64_t t0 = apu_platform_cycles(), dt;
    /* Held voices first, every pass: within half a pass of their frame */
    apu_vp_hold_tick(bar0, APU_AC97_HOLD_WINDOW);
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
        timed_pass(s_bar0, (s_stats.pumps & 1u) ? s_hook : NULL);
        Sleep(APU_AC97_PUMP_MS);
    }
    return 0;
}

static void thread_start(void) {
    s_thread = NULL;
    if (s_use_thread) {
        s_quit = 0;
        s_thread = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
        if (s_thread) {
            /* Above the game: a late pass is an audible gap */
            SetThreadPriority(s_thread, THREAD_PRIORITY_HIGHEST);
        }
    }
    s_stats.threaded = s_thread != NULL;
}

static void thread_stop(void) {
    if (s_thread) {
        s_quit = 1;
        WaitForSingleObject(s_thread, INFINITE);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
    s_stats.threaded = false;
}

static int output_start(uintptr_t bar0) {
    s_zc = zc_wanted();
    s_stats.zero_copy = s_zc;
    return s_zc ? zc_start(bar0) : copy_start(bar0);
}

static void output_stop(void) {
    apu_ac97_halt_dma();   /* the codec reads the GP ring or the copy buffers: stop it before those are freed */
    if (!s_zc) {
        free_slots();
    }
}

int apu_ac97_start(uintptr_t bar0) {
    if (s_active) {
        return 0;
    }
    memset(&s_stats, 0, sizeof(s_stats));
    s_tsc0 = apu_platform_cycles();
    QueryPerformanceCounter(&s_qpc0);
    s_bar0 = bar0;
    if (output_start(bar0) != 0) {
        return -1;
    }
    s_active = true;
    thread_start();
    return 0;
}

/* An output hook came or went: restart the output in the mode it needs (the counters carry on) */
static void mode_check(void) {
    if (!s_active || zc_wanted() == s_zc) {
        return;
    }
    thread_stop();
    output_stop();
    if (output_start(s_bar0) != 0) {
        s_active = false;   /* copy buffers could not be allocated: silent */
        return;
    }
    thread_start();
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
    thread_stop();
    output_stop();
    s_active = false;
    s_zc = false;
    s_stats.zero_copy = false;
}

void apu_ac97_halt_dma(void) {
    volatile uint8_t *pb = ac97_regs();
    if (!pb) {
        return;
    }
    /* Stop both DMAs and reset their registers: they no longer read the old buffers */
    dma_reset(pb, AC97_PO_CR);
    dma_reset(pb, AC97_SP_CR);
}

#else /* host: no codec */

static void mode_check(void) {
}

void apu_ac97_set_thread(bool on) {
    (void)on;
}

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

bool apu_ac97_threaded(void) {
#if defined(OPENAL_TARGET_XBOX)
    return s_active && s_thread != NULL;
#else
    return false;
#endif
}

bool apu_ac97_zero_copy(void) {
    return s_active && s_zc;
}

void apu_ac97_get_stats(apu_ac97_stats_t *out) {
    if (out) {
        *out = s_stats;
        if (out->lead_min == 0xFFFFFFFFu) {
            out->lead_min = 0;   /* no reading yet */
        }
        out->cycle_hz = cycle_hz();
    }
}
