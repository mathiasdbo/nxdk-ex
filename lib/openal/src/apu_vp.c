#include "apu_vp.h"
#include "apu_mem.h"
#include "apu_gp.h"
#include "apu_ac97.h"
#include "apu_platform.h"
#include "mcpx_apu_regs.h"
#include <math.h>
#include <string.h>

/*
 * ============================================================================
 * State
 * ============================================================================
 */
#define APU_VP_NOTIFY_BYTES (16u * (2u + 4u * APU_VP_MAX_HANDLES))
#define APU_VP_MAX_MAPS     256u

typedef struct {
    uint32_t page_phys;     /* first physical page mapped */
    uint32_t pages;
    uint32_t entry;         /* first SGE index */
} apu_vp_map_t;

static uint8_t *s_voices;   static uint32_t s_voices_phys;
static uint32_t *s_sge;     static uint32_t s_sge_phys;
static uint8_t *s_ssl;      static uint32_t s_ssl_phys;
static uint8_t *s_notify;   static uint32_t s_notify_phys;
static uint32_t s_sge_used[APU_VP_SGE_ENTRIES / 32u];
static apu_vp_map_t s_maps[APU_VP_MAX_MAPS];
static uint32_t s_map_count;
static bool s_ready;
static bool s_fe_ok;                            /* front-end methods execute (xemu, not hardware) */
static uint8_t s_list[APU_VP_MAX_HANDLES];      /* list each handle is linked on, LIST_NONE if none */
static uint8_t s_paused[APU_VP_MAX_HANDLES];    /* taken off its list by apu_vp_voice_pause() */
static apu_vp_fe_stats_t s_fe_stats;
static uint32_t s_soft_end[APU_VP_MAX_HANDLES];  /* data frames of a one-shot looping on its silent tail, 0 = none */
static uint32_t s_retired_at[APU_VP_MAX_HANDLES]; /* XGSCNT when the handle left its list */
static uint8_t s_retired[APU_VP_MAX_HANDLES];     /* has left a list since init */
static uint32_t s_alloc_next;                      /* round-robin start for apu_vp_alloc_handle() */

/* XGSCNT samples a handle stays unused after leaving its list: more than one
 * 32-sample frame, so the VP has finished any frame that was processing it */
#define APU_VP_QUARANTINE_SAMPLES 64u

static inline void reg_wr(uintptr_t bar0, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(bar0 + off) = v;
}

static inline uint32_t reg_rd(uintptr_t bar0, uint32_t off) {
    return *(volatile uint32_t *)(bar0 + off);
}

static inline void method(uintptr_t bar0, uint32_t m, uint32_t v) {
    reg_wr(bar0, MCPX_APU_METHOD_BASE + m, v);
}

static inline volatile uint32_t *voice_dw(uint32_t h, uint32_t off) {
    return (volatile uint32_t *)(s_voices + h * MCPX_VOICE_SIZE + off);
}

/* The pool is write-combining: drain the CPU's WC buffers so a record is in
 * RAM before the list head (an uncached register write) exposes it to the VP.
 * A locked instruction flushes them on every x86. */
static inline void wc_flush(void) {
#if defined(OPENAL_TARGET_XBOX) && (defined(__i386__) || defined(_M_IX86))
    __asm__ __volatile__("lock; addl $0,(%%esp)" ::: "memory");
#endif
}

/*
 * ============================================================================
 * Encodings
 * ============================================================================
 */

int16_t apu_vp_pitch_from_ratio(float ratio) {
    float p;
    if (!(ratio > 0.0f)) {
        return -32768;
    }
    /* log2f is an unimplemented stub (assert) in nxdk's pdclib */
    p = 4096.0f * logf(ratio) * 1.44269504f;
    if (p > 32767.0f) return 32767;
    if (p < -32768.0f) return -32768;
    return (int16_t)(p < 0.0f ? p - 0.5f : p + 0.5f);
}

uint16_t apu_vp_atten_from_gain(float gain) {
    float a;
    if (!(gain > 6.3e-4f)) {
        return MCPX_VOL_MUTE;
    }
    if (gain >= 1.0f) {
        return MCPX_VOL_UNITY;   /* the attenuation cannot amplify */
    }
    a = -1280.0f * log10f(gain) + 0.5f;
    return (a > (float)0xFFE) ? 0xFFEu : (uint16_t)a;
}

void apu_vp_pack_volumes(const uint16_t v[APU_VP_OUTPUTS], uint32_t *vola, uint32_t *volb, uint32_t *volc) {
    uint32_t v6 = v[6] & 0xFFFu, v7 = v[7] & 0xFFFu;
    *vola = ((uint32_t)(v[0] & 0xFFFu) << 4) | ((uint32_t)(v[1] & 0xFFFu) << 20) |
            (v6 & 0xFu) | ((v7 & 0xFu) << 16);
    *volb = ((uint32_t)(v[2] & 0xFFFu) << 4) | ((uint32_t)(v[3] & 0xFFFu) << 20) |
            ((v6 >> 4) & 0xFu) | (((v7 >> 4) & 0xFu) << 16);
    *volc = ((uint32_t)(v[4] & 0xFFFu) << 4) | ((uint32_t)(v[5] & 0xFFFu) << 20) |
            ((v6 >> 8) & 0xFu) | (((v7 >> 8) & 0xFu) << 16);
}

void apu_vp_pack_bins(const uint8_t b[APU_VP_OUTPUTS], uint32_t *vbin, uint32_t *fmt_bins) {
    *vbin = ((uint32_t)(b[0] & 0x1Fu)) | ((uint32_t)(b[1] & 0x1Fu) << 5) |
            ((uint32_t)(b[2] & 0x1Fu) << 10) | ((uint32_t)(b[3] & 0x1Fu) << 16) |
            ((uint32_t)(b[4] & 0x1Fu) << 21) | ((uint32_t)(b[5] & 0x1Fu) << 26);
    *fmt_bins = ((uint32_t)(b[6] & 0x1Fu)) | ((uint32_t)(b[7] & 0x1Fu) << 5);
}

uint32_t apu_vp_format(int channels, int bits, bool loop) {
    /* 16-bit: S16 in a 2-byte container; 8-bit: U8 in a 1-byte container (both 0) */
    uint32_t fmt = (bits == 16) ? (MCPX_FMT_SAMPLE_S16 | MCPX_FMT_CONTAINER_B16) : 0u;
    if (channels == 2) {
        /* Interleaved stereo needs SAMPLES_PER_BLOCK = 1 (stride of two containers) */
        fmt |= MCPX_FMT_STEREO | (1u << MCPX_FMT_SPB_SHIFT);
    }
    if (loop) {
        fmt |= MCPX_FMT_LOOP;
    }
    return fmt;
}

/*
 * ============================================================================
 * SGE mapping: buffers are mapped page by page into the 16 MiB linear space
 * that BA addresses; mappings are cached and reused
 * ============================================================================
 */

static bool sge_used(uint32_t e) {
    return (s_sge_used[e >> 5] >> (e & 31u)) & 1u;
}

static uint32_t s_map_guard;   /* extra pages mapped behind every buffer (apu_vp_debug_set_map_guard) */
static uint32_t s_dbg;         /* APU_VP_DBG_* switches (apu_vp_debug_set_flags) */

void apu_vp_debug_set_flags(uint32_t flags) {
    s_dbg = flags;
}

void apu_vp_debug_set_map_guard(uint32_t pages) {
    s_map_guard = pages;
}

static int map_buffer(uint32_t phys, uint32_t bytes, uint32_t *ba) {
    uint32_t page0 = phys & ~0xFFFu;
    uint32_t pages = ((phys + bytes - 1u) >> 12) - (phys >> 12) + 1u + s_map_guard;
    uint32_t i, start, run;

    for (i = 0; i < s_map_count; i++) {
        const apu_vp_map_t *m = &s_maps[i];
        if (page0 >= m->page_phys && page0 + pages * 4096u <= m->page_phys + m->pages * 4096u) {
            *ba = (m->entry << 12) + (phys - m->page_phys);
            return 0;
        }
    }
    if (s_map_count >= APU_VP_MAX_MAPS) {
        return -1;
    }

    /* First fit run of free entries */
    for (start = 0, run = 0, i = 0; i < APU_VP_SGE_ENTRIES; i++) {
        if (sge_used(i)) {
            run = 0;
            start = i + 1u;
            continue;
        }
        if (++run == pages) {
            break;
        }
    }
    if (run < pages) {
        return -1;
    }
    for (i = 0; i < pages; i++) {
        s_sge[2u * (start + i)] = page0 + i * 4096u;
        s_sge[2u * (start + i) + 1u] = 0;
        s_sge_used[(start + i) >> 5] |= 1u << ((start + i) & 31u);
    }
    s_maps[s_map_count].page_phys = page0;
    s_maps[s_map_count].pages = pages;
    s_maps[s_map_count].entry = start;
    s_map_count++;
    *ba = (start << 12) + (phys & 0xFFFu);
    return 0;
}


/*
 * ============================================================================
 * Front end. On a real console the PIO queue executes one method and then
 * stalls (apu_probe rounds 1-15), so voices are never driven by methods: the
 * CPU writes the voice records and list heads itself, which the VP follows
 * (round 16). Methods are only used where xemu needs them and its front end
 * works: the HRTF table and the per-voice HRTF latch.
 * ============================================================================
 */

static void fe_spin(uintptr_t bar0, uint32_t reads) {
    while (reads--) {
        (void)reg_rd(bar0, MCPX_APU_FECTL);   /* ~1 us per uncached read on hardware */
    }
}

/* Two harmless methods: a working front end consumes both. On hardware the
 * second stays queued; it is stepped out (each change of the method mode to
 * HALTED consumes one entry) so nothing is left behind. */
static bool fe_probe(uintptr_t bar0) {
    uint32_t kicks;

    method(bar0, MCPX_METH_SET_CURRENT_VOICE, 0);
    method(bar0, MCPX_METH_SET_CURRENT_VOICE, 0);
    fe_spin(bar0, 2000u);
    if ((reg_rd(bar0, MCPX_APU_FEPIOQ) & MCPX_APU_FEPIOQ_PENDING) == 0) {
        return true;
    }
    for (kicks = 0; kicks < 64u && (reg_rd(bar0, MCPX_APU_FEPIOQ) & MCPX_APU_FEPIOQ_PENDING); kicks++) {
        reg_wr(bar0, MCPX_APU_FECTL, 0xE0u);
        reg_wr(bar0, MCPX_APU_FECTL, 0x80u);
    }
    reg_wr(bar0, MCPX_APU_FECTL, 0);
    return false;
}

/*
 * ============================================================================
 * Voice lists. The library is the only writer: s_list[] says which list a
 * handle is on, the records' next fields and the head registers hold the
 * chain the VP walks every frame.
 * ============================================================================
 */
#define LIST_NONE 0xFFu

static const uint32_t s_list_heads[3] = { MCPX_APU_TVL2D, MCPX_APU_TVL3D, MCPX_APU_TVLMP };

/* Ring of the last list operations, for freeze diagnostics (apu_vp_debug_trace()) */
#define TRACE_LEN 256u
static apu_vp_trace_t s_trace[TRACE_LEN];
static uint32_t s_trace_n;
static bool s_trace_stopped;               /* frozen at the first frame stall */
static uint32_t s_stall_gp, s_stall_xgs;   /* last GP frame count and the XGSCNT it was seen at */

static void trace(uintptr_t bar0, char op, uint32_t h, uint32_t a) {
    apu_vp_trace_t *e;
    if (s_trace_stopped) {
        return;
    }
    e = &s_trace[s_trace_n++ % TRACE_LEN];
    e->op = op;
    e->h = (uint16_t)h;
    e->a = a;
    e->t = reg_rd(bar0, MCPX_APU_XGSCNT);
}

uint32_t apu_vp_debug_trace(apu_vp_trace_t *out, uint32_t max) {
    uint32_t n = (s_trace_n < TRACE_LEN) ? s_trace_n : TRACE_LEN, i;
    if (n > max) n = max;
    for (i = 0; i < n; i++) {
        out[i] = s_trace[(s_trace_n - n + i) % TRACE_LEN];
    }
    return n;
}

static void link_voice(uintptr_t bar0, uint32_t h, uint8_t list) {
    volatile uint32_t *pl = voice_dw(h, MCPX_VOICE_TAR_PITCH_LINK);
    uint32_t head = reg_rd(bar0, s_list_heads[list]) & 0xFFFFu;
    *pl = (*pl & 0xFFFF0000u) | head;
    wc_flush();
    reg_wr(bar0, s_list_heads[list], h);
    s_list[h] = list;
    trace(bar0, 'L', h, head);   /* a = the old head, now our next */
}

/* Unlink by editing the predecessor's next field (bits 15:0) or the list head */
static void unlink_voice(uintptr_t bar0, uint32_t h) {
    uint32_t next = *voice_dw(h, MCPX_VOICE_TAR_PITCH_LINK) & 0xFFFFu;
    uint32_t cur, guard = 0;
    uint8_t l = s_list[h];

    if (l == LIST_NONE) {
        return;
    }
    s_list[h] = LIST_NONE;
    /* The VP may still be processing this record in the current frame: keep
     * the handle out of apu_vp_alloc_handle() for a while */
    s_retired_at[h] = reg_rd(bar0, MCPX_APU_XGSCNT);
    s_retired[h] = 1;
    cur = reg_rd(bar0, s_list_heads[l]) & 0xFFFFu;
    if (cur == h) {
        reg_wr(bar0, s_list_heads[l], next);
        trace(bar0, 'H', h, next);   /* unlinked at the head; a = new head */
        return;
    }
    while (cur != MCPX_APU_LIST_END && cur < APU_VP_MAX_HANDLES && guard++ < APU_VP_MAX_HANDLES) {
        volatile uint32_t *link = voice_dw(cur, MCPX_VOICE_TAR_PITCH_LINK);
        uint32_t n = *link & 0xFFFFu;
        if (n == h) {
            *link = (*link & 0xFFFF0000u) | next;
            wc_flush();
            trace(bar0, 'U', h, (cur << 16) | next);   /* a = predecessor << 16 | next */
            return;
        }
        cur = n;
    }
    trace(bar0, 'X', h, (uint32_t)l << 16 | guard);   /* not found in its list */
}

static bool record_active(uint32_t h) {
    return (*voice_dw(h, MCPX_VOICE_PAR_STATE) & MCPX_PAR_STATE_ACTIVE_VOICE) != 0;
}

static bool soft_ended(uint32_t h) {
    return s_soft_end[h] != 0 && (*voice_dw(h, MCPX_VOICE_PAR_OFFSET) & 0xFFFFFFu) >= s_soft_end[h];
}

uint32_t apu_vp_service(uintptr_t bar0) {
    uint32_t served = 0, h;

    if (!s_ready) {
        return 0;
    }
    /* xemu traps on an inactive voice still in a list (FETFORCE1 bit 15) and
     * halts the frames until released; only the last idle handle of a frame is
     * reported, so loop while trapped */
    while (s_fe_ok && served < 8u && (reg_rd(bar0, MCPX_APU_FECTL) & MCPX_APU_FECTL_FEMETHMODE)) {
        h = reg_rd(bar0, MCPX_APU_FEDECPARAM) & 0xFFFFu;
        if (h < APU_VP_MAX_HANDLES && !record_active(h)) {
            unlink_voice(bar0, h);
        }
        reg_wr(bar0, MCPX_APU_FECTL, 0);
        reg_wr(bar0, MCPX_APU_ISTS, MCPX_APU_ISTS_FETINTSTS);
        served++;
    }
    /* One-shots that ended: the VP cleared ACTIVE_VOICE, or (real console) CBO
     * passed the data into the silent tail; take them off their list */
    for (h = 0; h < APU_VP_MAX_HANDLES && !(s_dbg & APU_VP_DBG_NO_END_SCAN); h++) {
        if (s_list[h] == LIST_NONE) {
            continue;
        }
        if (soft_ended(h)) {
            /* Only unlink: a state write could land while the VP processes the
             * record in this frame (apu_vp_voice_active() ignores unlisted voices) */
            trace(bar0, 'E', h, *voice_dw(h, MCPX_VOICE_PAR_OFFSET) & 0xFFFFFFu);   /* a = CBO */
            unlink_voice(bar0, h);
        } else if (!record_active(h)) {
            unlink_voice(bar0, h);
        }
    }
    /* Real console: a front-end message (FECTL bit 15, seen with FEDECMETH
     * 0x8008 in apu_probe round 2/12) halts frames; record it and try to
     * clear it by writing the bit back */
    if (!s_fe_ok && !(s_dbg & APU_VP_DBG_NO_FE_CHECK)) {
        uint32_t fectl = reg_rd(bar0, MCPX_APU_FECTL);
        if (fectl & 0x8000u) {
            s_fe_stats.events++;
            s_fe_stats.last_meth = reg_rd(bar0, MCPX_APU_FEDECMETH);
            s_fe_stats.last_param = reg_rd(bar0, MCPX_APU_FEDECPARAM);
            reg_wr(bar0, MCPX_APU_FECTL, fectl & ~MCPX_APU_FECTL_FEMETHMODE);
            reg_wr(bar0, MCPX_APU_ISTS, 0xFFFFFFFFu);
        }
    }
    /* Stall detector: the GP counts 1500 frames a second; if it has not moved
     * while XGSCNT advanced by several frames, keep the trace as it is */
    if (!s_fe_ok && !s_trace_stopped && !(s_dbg & APU_VP_DBG_NO_STALL_DETECT)) {
        uint32_t gp = apu_gp_frames(bar0), xgs = reg_rd(bar0, MCPX_APU_XGSCNT);
        if (gp != s_stall_gp) {
            s_stall_gp = gp;
            s_stall_xgs = xgs;
        } else if (xgs - s_stall_xgs > 512u) {
            trace(bar0, 'Z', 0, s_stall_xgs);   /* a = XGSCNT of the last GP frame */
            s_trace_stopped = true;
        }
    }
    apu_ac97_pump(bar0);
    return served;
}

/*
 * ============================================================================
 * Lifecycle
 * ============================================================================
 */

static void free_tables(void) {
    if (s_voices) apu_mem_free_phys(s_voices);
    if (s_sge) apu_mem_free_phys(s_sge);
    if (s_ssl) apu_mem_free_phys(s_ssl);
    if (s_notify) apu_mem_free_phys(s_notify);
    s_voices = NULL;
    s_sge = NULL;
    s_ssl = NULL;
    s_notify = NULL;
}

void apu_vp_load_hrtf(uintptr_t bar0, const apu_hrtf_entry_t table[APU_HRTF_ENTRIES]) {
    uint32_t e, k;
    if (!s_fe_ok) {
        return;   /* the table can only be loaded through front-end methods */
    }
    for (e = 0; e < APU_HRTF_ENTRIES; e++) {
        const apu_hrtf_entry_t *t = &table[e];
        method(bar0, MCPX_METH_SET_CURRENT_HRTF_ENTRY, e);
        for (k = 0; k < 15u; k++) {
            method(bar0, MCPX_METH_SET_HRIR + 4u * k,
                   (uint32_t)(uint8_t)t->left[2u * k] | ((uint32_t)(uint8_t)t->right[2u * k] << 8) |
                   ((uint32_t)(uint8_t)t->left[2u * k + 1u] << 16) |
                   ((uint32_t)(uint8_t)t->right[2u * k + 1u] << 24));
        }
        method(bar0, MCPX_METH_SET_HRIR_X,
               (uint32_t)(uint8_t)t->left[30] | ((uint32_t)(uint8_t)t->right[30] << 8) |
               ((uint32_t)(uint16_t)t->itd << 16));
    }
}

int apu_vp_init(uintptr_t bar0) {
    if (s_ready) {
        return 0;
    }
    s_voices = (uint8_t *)apu_mem_alloc_table(APU_VP_MAX_HANDLES * MCPX_VOICE_SIZE, &s_voices_phys);
    s_sge = (uint32_t *)apu_mem_alloc_table(APU_VP_SGE_ENTRIES * 8u, &s_sge_phys);
    s_ssl = (uint8_t *)apu_mem_alloc_table(4096, &s_ssl_phys);
    s_notify = (uint8_t *)apu_mem_alloc_table(APU_VP_NOTIFY_BYTES, &s_notify_phys);
    if (!s_voices || !s_sge || !s_ssl || !s_notify) {
        free_tables();
        return -1;
    }
    memset(s_voices, 0, APU_VP_MAX_HANDLES * MCPX_VOICE_SIZE);
    memset(s_sge, 0, APU_VP_SGE_ENTRIES * 8u);
    memset(s_soft_end, 0, sizeof(s_soft_end));
    memset(s_retired, 0, sizeof(s_retired));
    s_alloc_next = 0;
    memset(s_notify, 0, APU_VP_NOTIFY_BYTES);
    memset(s_sge_used, 0, sizeof(s_sge_used));
    memset(s_list, LIST_NONE, sizeof(s_list));
    memset(s_paused, 0, sizeof(s_paused));
    s_map_count = 0;

    /* Quiesce (bring-up step 6) */
    reg_wr(bar0, MCPX_APU_SECTL, 0);
    reg_wr(bar0, MCPX_APU_FECTL, 0);
    reg_wr(bar0, MCPX_APU_GPRST, 0);
    reg_wr(bar0, MCPX_APU_EPRST, 0);
    reg_wr(bar0, MCPX_APU_IEN, 0);
    reg_wr(bar0, MCPX_APU_ISTS, 0xFFFFFFFFu);

    /* Table bases (7), empty lists (9), idle-voice trap (10), notifiers (11) */
    reg_wr(bar0, MCPX_APU_VPVADDR, s_voices_phys);
    reg_wr(bar0, MCPX_APU_VPSGEADDR, s_sge_phys);
    reg_wr(bar0, MCPX_APU_VPSSLADDR, s_ssl_phys);
    reg_wr(bar0, MCPX_APU_TVL2D, MCPX_APU_LIST_END);
    reg_wr(bar0, MCPX_APU_TVL3D, MCPX_APU_LIST_END);
    reg_wr(bar0, MCPX_APU_TVLMP, MCPX_APU_LIST_END);
    reg_wr(bar0, MCPX_APU_FETFORCE1,
           (s_dbg & APU_VP_DBG_NO_FE_PROBE) ? 0u : MCPX_APU_FETFORCE1_SE2FE_IDLE_VOICE);
    reg_wr(bar0, MCPX_APU_FENADDR, s_notify_phys);

    s_fe_ok = (s_dbg & APU_VP_DBG_NO_FE_PROBE) ? false : fe_probe(bar0);
    s_trace_n = 0;
    s_trace_stopped = false;
    s_stall_gp = 0;
    if (!s_fe_ok) {
        /* Real console: with bit 15 set, an ended voice still in a list stops
         * frame processing until software services the front end, which the
         * stalled method queue cannot do (openal_vp froze 24.5 s in, when its
         * first one-shot ended). The driver unlinks ended voices itself. */
        reg_wr(bar0, MCPX_APU_FETFORCE1, 0);
    }
    memset(&s_fe_stats, 0, sizeof(s_fe_stats));

    /* HRTF (step 13), front end only: for handles below 64, outputs 0-3 go to
     * these four submix bins: front left/right (0, 1) and surround left/right
     * (2, 3) in this library's bin order. Then load the spherical-head table. */
    if (s_fe_ok) {
        static apu_hrtf_entry_t table[APU_HRTF_ENTRIES];
        method(bar0, MCPX_METH_SET_HRTF_HEADROOM, 0);
        method(bar0, MCPX_METH_SET_HRTF_SUBMIXES, 0u | (1u << 8) | (2u << 16) | (3u << 24));
        apu_hrtf_build_table(table);
        apu_vp_load_hrtf(bar0, table);
    }

    /* GP program that forwards bins 0/1 to output FIFO 0 (steps 8, 14, 15) */
    if (apu_gp_init(bar0) != 0) {
        free_tables();
        return -1;
    }

    /* Start frame generation (17) */
    reg_wr(bar0, MCPX_APU_SECTL, MCPX_APU_SECTL_RUN);
    s_ready = true;

    /* Real console: nothing else turns the GP output into sound */
    apu_ac97_set_thread(!(s_dbg & APU_VP_DBG_NO_AC97_THREAD));
    if (!s_fe_ok && !(s_dbg & APU_VP_DBG_NO_AC97) && apu_ac97_start(bar0) != 0) {
        apu_vp_deinit(bar0);
        return -1;
    }
    return 0;
}

void apu_vp_deinit(uintptr_t bar0) {
    uint32_t h;

    if (!s_ready) {
        return;
    }
    apu_ac97_stop();
    for (h = 0; h < APU_VP_MAX_HANDLES; h++) {
        unlink_voice(bar0, h);
        *voice_dw(h, MCPX_VOICE_PAR_STATE) = 0;
        s_paused[h] = 0;
    }
    apu_vp_service(bar0);   /* release a pending xemu trap */
    reg_wr(bar0, MCPX_APU_SECTL, 0);
    reg_wr(bar0, MCPX_APU_IEN, 0);
    reg_wr(bar0, MCPX_APU_GPRST, 0);
    apu_gp_deinit(bar0);
    reg_wr(bar0, MCPX_APU_EPRST, 0);
    reg_wr(bar0, MCPX_APU_ISTS, 0xFFFFFFFFu);
    s_ready = false;
    free_tables();
}

bool apu_vp_ready(void) {
    return s_ready;
}

void apu_vp_get_fe_stats(apu_vp_fe_stats_t *out) {
    if (out) {
        *out = s_fe_stats;
    }
}

bool apu_vp_hrtf_available(void) {
    return s_ready && s_fe_ok;
}

/*
 * ============================================================================
 * Voices
 * ============================================================================
 */

/* xemu: TAR_HRTF also latches the entry's filter into its HRTF stage */
static void latch_hrtf(uintptr_t bar0, uint32_t h, uint32_t entry) {
    if (s_fe_ok && h < 64u) {
        method(bar0, MCPX_METH_SET_CURRENT_VOICE, h);
        method(bar0, MCPX_METH_TAR_HRTF, entry);
    }
}

int apu_vp_voice_start(uintptr_t bar0, uint32_t h, const apu_vp_voice_params_t *p) {
    uint32_t frame_bytes, frames, tail_frames, ba, vbin, fmt_bins, vola, volb, volc, hrtf, i;
    bool soft_end;

    if (!s_ready || !p || h >= (s_fe_ok ? APU_VP_MAX_HANDLES : APU_VP_HW_HANDLES) || p->bytes == 0 ||
        (p->channels != 1 && p->channels != 2) || (p->bits != 8 && p->bits != 16)) {
        return -1;
    }
    frame_bytes = (uint32_t)(p->bits / 8) * (uint32_t)p->channels;
    frames = p->bytes / frame_bytes;
    tail_frames = p->tail_bytes / frame_bytes;
    if (frames == 0 || frames + tail_frames > 0xFFFFFFu) {
        return -1;
    }
    /* Real console: a one-shot must not reach its end (that stops every
     * frame), so it loops over the silent tail and the driver ends it */
    soft_end = !s_fe_ok && !p->loop && tail_frames > 0;
    if (map_buffer(p->phys, p->bytes + (soft_end ? tail_frames * frame_bytes : 0u), &ba) != 0) {
        return -3;
    }

    /* Restarting a handle: take it out of its list first, the VP must never
     * see a half-written record */
    unlink_voice(bar0, h);
    s_paused[h] = 0;
    *voice_dw(h, MCPX_VOICE_PAR_STATE) = 0;

    apu_vp_pack_bins(p->bins, &vbin, &fmt_bins);
    apu_vp_pack_volumes(p->vols, &vola, &volb, &volc);
    /* Handles below 64 run the HRTF stage; a non-null entry must be latched or the voice is silent */
    hrtf = (h < 64u && s_fe_ok && p->hrtf_entry >= 0 && p->hrtf_entry < APU_HRTF_ENTRIES) ? (uint32_t)p->hrtf_entry
                                                                                          : MCPX_HRTF_NONE;

    /* What the configuration methods and VOICE_ON leave in the record (xemu vp.c) */
    for (i = 0; i < MCPX_VOICE_SIZE; i += 4u) {
        *voice_dw(h, i) = 0;
    }
    *voice_dw(h, MCPX_VOICE_CFG_VBIN) = vbin;
    *voice_dw(h, MCPX_VOICE_CFG_FMT) = apu_vp_format(p->channels, p->bits, p->loop || soft_end) | fmt_bins;
    *voice_dw(h, MCPX_VOICE_CFG_HRTF_TARGET) = hrtf;   /* ENV0..MISC stay 0: EF_PITCHSCALE must be 0 */
    *voice_dw(h, MCPX_VOICE_CUR_PSL_START) = ba;
    *voice_dw(h, MCPX_VOICE_CUR_PSH_SAMPLE) = soft_end ? frames : 0u;   /* LBO */
    *voice_dw(h, MCPX_VOICE_PAR_OFFSET) = 0;                          /* CBO */
    *voice_dw(h, MCPX_VOICE_PAR_NEXT) = frames - 1u + (soft_end ? tail_frames : 0u);   /* EBO */
    s_soft_end[h] = soft_end ? frames : 0u;
    *voice_dw(h, MCPX_VOICE_TAR_VOLA) = vola;
    *voice_dw(h, MCPX_VOICE_TAR_VOLB) = volb;
    *voice_dw(h, MCPX_VOICE_TAR_VOLC) = volc;
    *voice_dw(h, MCPX_VOICE_TAR_PITCH_LINK) = ((uint32_t)(uint16_t)p->pitch << 16) | MCPX_APU_LIST_END;
    if (hrtf != MCPX_HRTF_NONE) {
        latch_hrtf(bar0, h, hrtf);
    }
    *voice_dw(h, MCPX_VOICE_PAR_STATE) = MCPX_PAR_STATE_ACTIVE_VOICE;
    trace(bar0, 'S', h, (frames & 0xFFFFFFu) | (soft_end ? 0x80000000u : 0u));   /* a = data frames, bit 31 = silent-tail loop */
    link_voice(bar0, h, 0);   /* top of the 2D list */
    return 0;
}

void apu_vp_voice_update(uintptr_t bar0, uint32_t h, int16_t pitch, const uint16_t vols[APU_VP_OUTPUTS],
                         int hrtf_entry) {
    volatile uint32_t *pl;
    uint32_t vola, volb, volc;

    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return;
    }
    apu_vp_pack_volumes(vols, &vola, &volb, &volc);
    /* Targets only: the VP ramps the current values toward them and never
     * writes these fields back. The next field is the library's. */
    *voice_dw(h, MCPX_VOICE_TAR_VOLA) = vola;
    *voice_dw(h, MCPX_VOICE_TAR_VOLB) = volb;
    *voice_dw(h, MCPX_VOICE_TAR_VOLC) = volc;
    pl = voice_dw(h, MCPX_VOICE_TAR_PITCH_LINK);
    *pl = ((uint32_t)(uint16_t)pitch << 16) | (*pl & 0xFFFFu);
    wc_flush();
    if (h < 64u && s_fe_ok && hrtf_entry >= 0 && hrtf_entry < APU_HRTF_ENTRIES) {
        *voice_dw(h, MCPX_VOICE_CFG_HRTF_TARGET) = (uint32_t)hrtf_entry;
        latch_hrtf(bar0, h, (uint32_t)hrtf_entry);
    }
    if (s_fe_ok) {
        apu_vp_service(bar0);   /* xemu: release an idle-voice trap promptly */
    }
}

/* Pause = out of the list (the VP leaves the record, and CBO, alone); resume = back on top */
void apu_vp_voice_pause(uintptr_t bar0, uint32_t h, bool pause) {
    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return;
    }
    if (pause) {
        if (s_list[h] != LIST_NONE) {
            unlink_voice(bar0, h);
            s_paused[h] = 1;
        }
    } else if (s_paused[h]) {
        s_paused[h] = 0;
        if (record_active(h)) {
            link_voice(bar0, h, 0);
        }
    }
}

void apu_vp_voice_off(uintptr_t bar0, uint32_t h) {
    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return;
    }
    unlink_voice(bar0, h);
    s_paused[h] = 0;
    if (s_fe_ok) {
        /* xemu: VOICE_OFF semantics. On hardware the record is left alone: the
         * VP may be processing it in this frame (unlisted voices are inactive) */
        *voice_dw(h, MCPX_VOICE_PAR_STATE) = 0;
    }
    apu_vp_service(bar0);
}

uint32_t apu_vp_alloc_handle(uintptr_t bar0) {
    uint32_t now, k, h;
    uint32_t span;

    if (!s_ready) {
        return MCPX_APU_LIST_END;
    }
    span = (s_fe_ok ? APU_VP_MAX_HANDLES : APU_VP_HW_HANDLES) - APU_VP_HANDLE_BASE;
    now = reg_rd(bar0, MCPX_APU_XGSCNT);
    for (k = 0; k < span; k++) {
        h = APU_VP_HANDLE_BASE + (s_alloc_next + k) % span;
        if (s_list[h] != LIST_NONE || s_paused[h]) {
            continue;
        }
        if (s_retired[h] && now - s_retired_at[h] < APU_VP_QUARANTINE_SAMPLES) {
            continue;
        }
        s_alloc_next = (h - APU_VP_HANDLE_BASE + 1u) % span;
        return h;
    }
    return MCPX_APU_LIST_END;
}

bool apu_vp_voice_active(uint32_t h) {
    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return false;
    }
    /* A frame in flight may write ACTIVE back after an unlink: only listed or
     * paused voices count */
    return (s_list[h] != LIST_NONE || s_paused[h]) && record_active(h) && !soft_ended(h);
}

uint32_t apu_vp_voice_position(uint32_t h) {
    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return 0;
    }
    return *voice_dw(h, MCPX_VOICE_PAR_OFFSET) & 0xFFFFFFu;
}

uint8_t *apu_vp_debug_voice_record(uint32_t h) {
    if (!s_ready || h >= APU_VP_MAX_HANDLES) {
        return NULL;
    }
    return s_voices + h * MCPX_VOICE_SIZE;
}

const uint32_t *apu_vp_debug_sge(void) {
    return s_sge;
}
