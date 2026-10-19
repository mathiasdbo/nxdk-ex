#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "apu_gp.h"
#include "apu_vp.h"
#include "apu_mem.h"
#include "mcpx_apu_regs.h"
#include "../dsp/gp_copy.h"   /* dsp/gp_copy.asm through tools/dsp56asm.py */
#include "apu_gp_loop.h"        /* dsp/gp_loop.asm */
#include "apu_gp_kfreeverb.h"   /* dsp/gp_kfreeverb.asm */
#include "apu_reverb.h"
#include "../dsp/gp_kecho.h"     /* dsp/gp_kecho.asm (--image): the example custom kernel */
#include "gp_kefx_chorus.h"
#include "gp_kefx_echo.h"

#define DEFAULT_SCRATCH APU_GP_SCRATCH_DEFAULT

/*
 * GP stage (apu_gp.c) against a plain-memory BAR0: the bootstrap image and
 * DMA descriptor it writes, the FIFO setup, and the reader of the output ring
 * (the test plays the DSP's part by writing samples and advancing GPOFCUR0).
 */

#define BAR0_SIZE 0x80000u

static uint8_t *s_bar0;
static apu_reverb_word_t s_words[APU_REVERB_MAX_WORDS];
static uint32_t s_code[APU_GP_KERNEL_MAX_WORDS];
static uint32_t reg(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }
#define XR(n) reg(MCPX_APU_GP_XMEM + 4u * (n))
#define PR(n) reg(MCPX_APU_GP_PMEM + 4u * (n))
static void set_reg(uint32_t off, uint32_t v) { *(volatile uint32_t *)(s_bar0 + off) = v; }

/* What the DMA would do for n frames: interleaved L, R int16 at GPOFCUR0 */
static void dsp_writes(uint32_t n, int16_t first) {
    uint8_t *ring = apu_gp_debug_fifo();
    uint32_t cur = reg(MCPX_APU_GPOFCUR0), i;
    for (i = 0; i < n; i++) {
        int16_t *f = (int16_t *)(ring + cur);
        f[0] = (int16_t)(first + (int16_t)i);
        f[1] = (int16_t)-(first + (int16_t)i);
        cur = (cur + 4u) % APU_GP_FIFO_BYTES;
    }
    set_reg(MCPX_APU_GPOFCUR0, cur);
}

int main(void) {
    uintptr_t base;
    const uint32_t *scratch;
    int16_t out[2 * 4096];
    apu_gp_stats_t st;
    uint32_t i, n;

    printf("=== MCPX APU GP Stage (route B) Test ===\n");
    s_bar0 = (uint8_t *)calloc(1, BAR0_SIZE);
    assert(s_bar0);
    base = (uintptr_t)s_bar0;
    assert(apu_mem_init(0) == 0);

    printf("[1] Program encodings...\n");
    /* movep #imm,x:pp = 0000100 0 1 1 110100 1 0 pppppp, then the immediate;
     * jclr #n,x:pp,xxxx = 0000 1010 10pp pppp 1S0b bbbb, then the target */
    assert(apu_gp_program[0] == (0x08F480u | 0x04u) && apu_gp_program[1] == 1);   /* frame complete first */
    assert(apu_gp_program[2] == (0x0A8080u | (0x05u << 8) | 1u) && apu_gp_program[3] == 2);   /* wait: jclr to itself */
    assert(apu_gp_program[4] == (0x08F480u | 0x05u) && apu_gp_program[5] == 2);   /* acknowledge START_FRAME */
    assert(apu_gp_program[6] == (0x08F480u | 0x14u) && apu_gp_program[7] == 0);   /* NEXT_BLOCK = 0 */
    assert(apu_gp_program[8] == (0x08F480u | 0x16u) && apu_gp_program[9] == 1);   /* CONTROL = start */
    assert(apu_gp_program[10] == 0x000008u);                                       /* inc a */
    assert(apu_gp_program[11] == 0x501000u);                                       /* move a0,x:$10 (X:aa parallel move) */
    assert(apu_gp_program[12] == 0x0C0000u);                                       /* jmp $0000 */
    printf("    -> complete / wait / acknowledge / DMA / count / jmp, as run on hardware [PASS]\n");

    printf("[2] Bring-up through apu_vp_init()...\n");
    apu_vp_debug_set_flags(APU_VP_DBG_AC97_COPY);   /* the copy path's 64 KiB ring (zero-copy: test_ac97_zero_copy.c) */
    assert(apu_vp_init(base) == 0);
    assert(apu_gp_ready());
    assert(reg(MCPX_APU_GPRST) == 3u);                          /* out of reset: bootstrap ran */
    assert(reg(MCPX_APU_GPSADDR) != 0 && reg(MCPX_APU_GPSMAXSGE) == DEFAULT_SCRATCH / 4096u - 1u);   /* image + delay lines */
    assert(reg(MCPX_APU_GPFADDR) != 0 && reg(MCPX_APU_GPFMAXSGE) == APU_GP_FIFO_BYTES / 4096u - 1u);
    assert(reg(MCPX_APU_GPOFBASE0) == 0 && reg(MCPX_APU_GPOFEND0) == APU_GP_FIFO_BYTES);
    assert(reg(MCPX_APU_GPOFCUR0) < reg(MCPX_APU_GPOFEND0));
    scratch = apu_gp_debug_scratch();
    /* The default program: the frame loop at P:0, NOP elsewhere (the slots load the kernels) */
    for (i = 0; i < GP_LOOP_WORDS; i++) {
        assert(scratch[i] == gp_loop_words[i]);
    }
    for (i = GP_LOOP_WORDS; i < 0x800u; i++) {
        assert(scratch[i] == 0);
    }
    assert(GP_LOOP_WORDS <= GP_LOOP_KERNEL && GP_LOOP_TRAMP3 == GP_LOOP_TRAMP0 + 3u);
    assert(gp_loop_words[GP_LOOP_TRAMP0] == (0x0C0000u | GP_LOOP_KNONE) && gp_loop_words[GP_LOOP_KNONE] == 0x00000Cu);
    assert(reg(MCPX_APU_GP_XMEM + 0x40u) == 0 && apu_gp_frames(base) == 0);   /* frame counter cleared */
    set_reg(MCPX_APU_GP_XMEM + 0x40u, 0x1000123u);
    assert(apu_gp_frames(base) == 0x000123u);                    /* 24-bit DSP word */
    set_reg(MCPX_APU_GP_XMEM + 0x40u, 0);
    /* Descriptor at X:0: EOL, interleave|to-memory|FIFO0|16-bit, 32x2, X:$1400 */
    assert(reg(MCPX_APU_GP_XMEM + 0) == 0x4000u);
    assert(reg(MCPX_APU_GP_XMEM + 4) == (1u | 2u | (1u << 10) | (32u << 14)));   /* channel stride: one bin */
    assert(reg(MCPX_APU_GP_XMEM + 8) == ((32u << 4) | 1u));
    assert(reg(MCPX_APU_GP_XMEM + 12) == 0x1400u);
    assert(reg(MCPX_APU_SECTL) == MCPX_APU_SECTL_RUN);                          /* frames started after the GP */
    printf("    -> image in scratch, descriptor in X, FIFO ring set, GP released [PASS]\n");

    printf("[3] Reading the output ring...\n");
    assert(apu_gp_fifo_available(base) == 0);
    dsp_writes(96, 100);                                        /* three APU frames */
    assert(apu_gp_fifo_available(base) == 96);
    n = apu_gp_fifo_read(base, out, 64);
    assert(n == 64 && out[0] == 100 && out[1] == -100 && out[126] == 163 && out[127] == -163);
    n = apu_gp_fifo_read(base, out, 1000);
    assert(n == 32 && out[0] == 164);
    assert(apu_gp_fifo_read(base, out, 10) == 0);
    /* Wrap around the end of the ring */
    for (i = 0; i < (APU_GP_FIFO_BYTES / 4u) / 1024u + 1u; i++) {
        dsp_writes(1024, (int16_t)i);
        n = apu_gp_fifo_read(base, out, 4096);
        assert(n == 1024 && out[0] == (int16_t)i && out[2 * 1023] == (int16_t)(i + 1023));
    }
    printf("    -> frames come out in order, interleaved, across the ring end [PASS]\n");

    printf("[4] Reader falling a ring behind...\n");
    dsp_writes(APU_GP_FIFO_BYTES / 4u - 64u, 0);                /* almost a full ring unread */
    n = apu_gp_fifo_read(base, out, 4096);
    apu_gp_get_stats(base, &st);
    assert(st.resyncs == 1);
    assert(apu_gp_fifo_available(base) == APU_GP_FIFO_BYTES / 8u - n);   /* jumped to half a ring behind */
    printf("    -> resynchronised half a ring behind the writer [PASS]\n");

    printf("[5] Discarding the backlog...\n");
    dsp_writes(500, 7);
    assert(apu_gp_fifo_available(base) >= 500u);
    apu_gp_fifo_discard(base);
    assert(apu_gp_fifo_available(base) == 0);
    dsp_writes(3, 40);
    assert(apu_gp_fifo_read(base, out, 10) == 3 && out[0] == 40);
    printf("    -> reader jumps to the DSP's write position [PASS]\n");

    apu_vp_deinit(base);
    assert(!apu_gp_ready() && reg(MCPX_APU_GPRST) == 0);

    printf("[6] Assembler output and other program images...\n");
    assert(GP_COPY_WORDS == APU_GP_PROGRAM_WORDS);
    for (i = 0; i < APU_GP_PROGRAM_WORDS; i++) {
        assert(gp_copy_words[i] == apu_gp_program[i]);
    }
    {
        static const uint32_t image[5] = { 0x08F484u, 0x000001u, 0x000000u, 0x0C0000u, 0x123456u };
        const uint32_t *scratch;
        set_reg(MCPX_APU_GP_XMEM + 4u * 0x11u, 5u);
        set_reg(MCPX_APU_GP_XMEM + 4u * 0x3Fu, 5u);
        set_reg(MCPX_APU_GP_XMEM + 4u * 0x40u, 5u);
        apu_gp_debug_set_program(image, 5);
        apu_gp_debug_set_scratch_bytes(0x47001u);                    /* rounded up to 0x48000 */
        assert(apu_vp_init(base) == 0);
        assert(apu_gp_scratch_bytes() == 0x48000u && reg(MCPX_APU_GPSMAXSGE) == 0x47u);
        scratch = apu_gp_debug_scratch();
        for (i = 0; i < 5u; i++) {
            assert(scratch[i] == image[i]);
        }
        assert(scratch[5] == 0 && scratch[0x47FFFu / 4u] == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * 0x11u) == 0 && reg(MCPX_APU_GP_XMEM + 4u * 0x3Fu) == 0);   /* parameters cleared */
        assert(reg(MCPX_APU_GP_XMEM + 4u * 0x40u) == 5u);
        apu_vp_deinit(base);
        apu_gp_debug_set_program(NULL, 0);
        apu_gp_debug_set_scratch_bytes(0);
        assert(apu_vp_init(base) == 0);
        assert(apu_gp_scratch_bytes() == DEFAULT_SCRATCH && apu_gp_debug_scratch()[0] == gp_loop_words[0]);
        apu_vp_deinit(base);
    }
    printf("    -> the assembled copy program matches; custom image and scratch size load [PASS]\n");

    printf("[7] Reverb control (a slot)...\n");
    {
        apu_reverb_params_t p;
        static const uint32_t image[3] = { 0x08F484u, 0x000001u, 0x0C0000u };
        uint32_t w0, px, xx, yy, sc;
        const apu_gp_kernel_t *rk = apu_gp_reverb_kernel();
        assert(rk && rk->abi == 2u && rk->words == GP_KFREEVERB_WORDS && rk->x_words == GP_KFREEVERB_K_XWORDS);
        assert(rk->scratch_bytes == apu_reverb_scratch_bytes() && rk->descriptors == 2u * APU_REVERB_LINES);
        assert(apu_vp_init(base) == 0);
        assert(!apu_gp_reverb_on() && XR(GP_LOOP_FLAGS) == 0);
        assert(apu_reverb_preset(3, &p) == 0);                       /* hall */
        assert(apu_gp_reverb_set(base, 1, &p, true) == 0 && apu_gp_reverb_on());
        /* first fit into an empty GP: the bottom of each space */
        assert(apu_gp_debug_slot(1, &px, &xx, &yy, &sc));
        assert(px == GP_LOOP_KERNEL && xx == GP_LOOP_KX_BASE && yy == 0 && sc == APU_REVERB_SCRATCH_BASE);
        assert(XR(GP_LOOP_FLAGS) == (2u | 0x20u));                  /* slot 1 and the output lowpass */
        assert(XR(GP_LOOP_SLOTX + 1u) == xx && PR(GP_LOOP_TRAMP1) == (0x0C0000u | px));   /* slot table, trampoline */
        assert(apu_gp_debug_scratch()[GP_LOOP_TRAMP1] == (0x0C0000u | px));               /* mirrored for a restart */
        apu_gp_kernel_relocate(rk, px, xx, yy, s_code);
        for (i = 0; i < rk->words; i++) {
            assert(PR(px + i) == s_code[i] && apu_gp_debug_scratch()[px + i] == s_code[i]);
        }
        assert(XR(xx + GP_KFREEVERB_KIN) == 0x1400u + 32u * 4u);    /* slot 1: bins 4/5 */
        assert(XR(xx + GP_KFREEVERB_RCHAIN) == xx + GP_KFREEVERB_RCHAIN + 16u);   /* chains written */
        assert(XR(xx + 3u) == xx + GP_KFREEVERB_RCHAIN && XR(xx + 4u) == xx + GP_KFREEVERB_WCHAIN);
        assert(XR(xx + GP_KFREEVERB_WCHAIN + 16u * (APU_REVERB_LINES - 1u)) == 0x4000u);   /* writes end at EOL */
        w0 = XR(xx + GP_KFREEVERB_WCHAIN + 4u);
        assert(w0 == 4u * 1200u);                                    /* 25 ms pre-delay */
        p.gain = 0.25f;                                              /* gains only: through the mailbox */
        set_reg(MCPX_APU_GP_XMEM + 4u * (xx + GP_KFREEVERB_WCHAIN + 4u), 0x777u);
        assert(apu_gp_reverb_set(base, 1, &p, true) == 0);
        assert(XR(xx + GP_KFREEVERB_WET) == 0x200000u);
        assert(XR(xx + GP_KFREEVERB_WCHAIN + 4u) == 0x777u);         /* layout untouched */
        assert(XR(GP_LOOP_MB_SEQ) == 1u && XR(GP_LOOP_MB_ACK) == 1u);
        assert(XR(GP_LOOP_MB_N) == apu_reverb_coefficients(&p, xx, s_words));
        assert(XR(GP_LOOP_MBOX + 2u * 2u) == xx + GP_KFREEVERB_WET);   /* pair 2: the wet gain */
        assert(XR(GP_LOOP_MBOX + 2u * 2u + 1u) == 0x200000u);
        p.pre_delay = 0.05f;                                         /* new pre-delay: laid out again, in place */
        assert(apu_gp_reverb_set(base, 1, &p, true) == 0);
        assert(XR(xx + GP_KFREEVERB_WCHAIN + 4u) == 4u * 2400u && XR(GP_LOOP_FLAGS) == (2u | 0x20u));
        assert(apu_gp_kernel_set(base, 1, NULL, NULL, false) == 0 && apu_gp_reverb_on());   /* not a kernel: stays */
        assert(apu_gp_reverb_set(base, 0, &p, false) == 0 && apu_gp_reverb_on());          /* another slot */
        assert(apu_gp_reverb_set(base, 1, &p, false) == 0 && !apu_gp_reverb_on());
        assert(XR(GP_LOOP_FLAGS) == 0 && !apu_gp_debug_slot(1, NULL, NULL, NULL, NULL));
        assert(apu_gp_reverb_set(base, APU_GP_SLOTS, &p, true) == -1);
        assert(apu_gp_mailbox(base, s_words, GP_LOOP_MB_MAX + 1u) == -1);   /* more than the mailbox holds */
        apu_vp_deinit(base);
        apu_gp_debug_set_program(image, 3);                          /* another program: no reverb */
        assert(apu_vp_init(base) == 0);
        assert(apu_gp_reverb_set(base, 0, &p, true) == -1 && !apu_gp_reverb_on());
        assert(apu_gp_mailbox(base, s_words, 1u) == -1);              /* no mailbox in another program */
        apu_vp_deinit(base);
        apu_gp_debug_set_program(NULL, 0);
    }
    printf("    -> placed first fit, relocated through the P window, trampoline, slot table, live gains, "
           "a new pre-delay re-lays the lines, off; refused for another program [PASS]\n");

    printf("[8] Custom kernels in slots (AL_XBOX_gp_program, phase9-04)...\n");
    {
        apu_gp_kernel_t k, e, legacy;
        apu_reverb_params_t p;
        uint32_t params[APU_GP_KERNEL_PARAMS], last, peak, px, xx, sc, ex, ep;
        static uint32_t bad[GP_KECHO_IMAGE_WORDS];
        /* an ABI 1 image: magic, 1, scratch, descriptors, code for P:$100 (rts) */
        static const uint32_t abi1[5] = { APU_GP_IMAGE_MAGIC, 1u, 0x1000u, 2u, 0x00000Cu };
        memset(params, 0, sizeof(params));
        params[0] = 100u;
        params[2] = 0x400000u;
        /* the image checks (before a device: against the scratch apu_gp_init will allocate) */
        assert(apu_gp_kernel_scratch_max() == APU_GP_SCRATCH_DEFAULT - APU_REVERB_SCRATCH_BASE);
        assert(apu_gp_kernel_parse(gp_kecho_image, GP_KECHO_IMAGE_WORDS, &k) == 0 && k.abi == 2u);
        assert(k.relocs == gp_kecho_image + APU_GP_IMAGE_HEADER2 && k.reloc_count == gp_kecho_image[6]);
        assert(k.code == gp_kecho_image + APU_GP_IMAGE_HEADER2 + k.reloc_count && k.words == GP_KECHO_WORDS);
        assert(k.scratch_bytes == GP_KECHO_K_SCRATCH_BYTES && k.descriptors == GP_KECHO_K_DESCRIPTORS);
        assert(k.x_words == GP_KECHO_K_XWORDS && k.y_words == GP_KECHO_K_YWORDS);
        memcpy(bad, gp_kecho_image, sizeof(bad));
        bad[0] ^= 1u;                                                 /* magic */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[0] = APU_GP_IMAGE_MAGIC; bad[1] = 3u;                     /* ABI */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[1] = APU_GP_IMAGE_ABI; bad[2] = apu_gp_kernel_scratch_max() + 1u;   /* scratch */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[2] = 0; bad[3] = APU_GP_KERNEL_MAX_DESCRIPTORS + 1u;      /* descriptors */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[3] = 0; bad[4] = APU_GP_KERNEL_MIN_XWORDS - 1u;           /* X block smaller than the ABI words */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[4] = APU_GP_KERNEL_MAX_XWORDS + 1u;
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[4] = 0x100u; bad[5] = APU_GP_KERNEL_MAX_YWORDS + 1u;      /* Y */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[5] = 0; bad[6] = GP_KECHO_IMAGE_WORDS;                    /* more relocs than words */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[6] = gp_kecho_image[6]; bad[7] = GP_KECHO_WORDS;          /* a reloc beyond the code */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[7] = 0x4000u;                                             /* a reloc word beyond 14 bits */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[7] = gp_kecho_image[7]; bad[GP_KECHO_IMAGE_WORDS - 1u] = 0x1000000u;   /* not a 24-bit word */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        assert(apu_gp_kernel_parse(gp_kecho_image, APU_GP_IMAGE_HEADER2 + gp_kecho_image[6], &k) == -1);   /* no code */
        assert(apu_gp_kernel_parse(abi1, 5u, &legacy) == 0 && legacy.abi == 1u && legacy.code == abi1 + 4);
        assert(legacy.words == 1u && legacy.reloc_count == 0u);
        assert(apu_gp_kernel_parse(abi1, APU_GP_IMAGE_HEADER, &legacy) == -1);
        assert(apu_gp_kernel_parse(gp_kefx_echo_image, GP_KEFX_ECHO_IMAGE_WORDS, &e) == 0);
        assert(apu_gp_kernel_parse(gp_kecho_image, GP_KECHO_IMAGE_WORDS, &k) == 0);
        assert(apu_gp_kernel_set(base, 0, &k, params, true) == -1);  /* no device */

        assert(apu_vp_init(base) == 0);
        /* the loop's mix descriptor and idle meter are set up for every kernel */
        assert(XR(GP_LOOP_FIFO2 + 3u) == GP_LOOP_OUTL && XR(GP_LOOP_FIFO2) == 0x4000u);
        assert(XR(GP_LOOP_IDLMIN) == 0x7FFFFFu);
        apu_gp_load(base, &last, &peak);
        assert(last == 0 && peak == 0);                               /* no meter on the host */
        /* the reverb in slot 0, the example echo beside it in slot 2 */
        assert(apu_reverb_preset(3, &p) == 0 && apu_gp_reverb_set(base, 0, &p, true) == 0);
        memset((uint8_t *)(uintptr_t)apu_gp_debug_scratch() + APU_REVERB_SCRATCH_BASE + GP_KFREEVERB_K_SCRATCH_BYTES,
               0x5A, 64);                                            /* where the echo's scratch will be */
        set_reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_KX_BASE + GP_KFREEVERB_K_XWORDS + 0x30u), 0x123456u);
        assert(apu_gp_kernel_set(base, 2, &k, params, true) == 0);
        assert(apu_gp_kernel_on(2) && apu_gp_kernel_on(APU_GP_SLOTS) && !apu_gp_kernel_on(0) && apu_gp_reverb_on());
        assert(apu_gp_debug_slot(2, &px, &xx, NULL, &sc));
        assert(px == GP_LOOP_KERNEL + GP_KFREEVERB_WORDS && xx == GP_LOOP_KX_BASE + GP_KFREEVERB_K_XWORDS);
        assert(sc == APU_REVERB_SCRATCH_BASE + GP_KFREEVERB_K_SCRATCH_BYTES);
        apu_gp_kernel_relocate(&k, px, xx, 0, s_code);
        for (i = 0; i < GP_KECHO_WORDS; i++) {                        /* P window and bootstrap image */
            assert(PR(px + i) == s_code[i] && apu_gp_debug_scratch()[px + i] == s_code[i]);
        }
        assert(PR(GP_LOOP_TRAMP2) == (0x0C0000u | px) && XR(GP_LOOP_SLOTX + 2u) == xx);
        assert(XR(xx + 0x30u) == 0);                                  /* its X block, cleared */
        assert(XR(xx) == 0x1400u + 32u * 6u);                         /* slot 2: bins 6/7 */
        assert(XR(xx + 1u) == sc && XR(xx + 2u) == GP_KECHO_K_SCRATCH_BYTES);
        assert(XR(xx + 3u) == 0 && XR(xx + 4u) == 0);                 /* no chains yet */
        assert(XR(xx + APU_GP_KERNEL_PARAM0) == 100u && XR(xx + APU_GP_KERNEL_PARAM0 + 2u) == 0x400000u);
        assert(XR(GP_LOOP_FLAGS) == (1u | 4u | 0x20u));              /* slots 0 and 2, the reverb's lowpass */
        assert(((const uint8_t *)apu_gp_debug_scratch())[sc + 63] == 0);   /* scratch zeroed */
        assert(XR(GP_LOOP_KX_BASE + 3u) == GP_LOOP_KX_BASE + GP_KFREEVERB_RCHAIN);   /* the reverb untouched */
        assert(apu_gp_slot_holds(2, &k) && !apu_gp_slot_holds(2, &e) && apu_gp_slot_holds(0, apu_gp_reverb_kernel()));
        assert(apu_gp_slot_holds(1, NULL) && !apu_gp_slot_holds(1, &k));
        /* new parameters, same code: the changed ones through the mailbox, at the slot's KPARAM */
        set_reg(MCPX_APU_GP_XMEM + 4u * (xx + 3u), xx + 0x30u);       /* as the kernel would */
        params[0] = 200u;
        params[31] = 0x7FFFFFu;
        assert(apu_gp_kernel_set(base, 2, &k, params, true) == 0);
        assert(XR(GP_LOOP_MB_N) == 2u);
        assert(XR(GP_LOOP_MBOX) == xx + APU_GP_KERNEL_PARAM0 && XR(GP_LOOP_MBOX + 1u) == 200u);
        assert(XR(GP_LOOP_MBOX + 2u) == xx + APU_GP_KERNEL_PARAM0 + 31u);
        assert(XR(xx + APU_GP_KERNEL_PARAM0 + 31u) == 0x7FFFFFu);
        assert(XR(xx + 3u) == xx + 0x30u);                            /* the kernel kept running */
        /* the reverb stops; the EFX echo in slot 3 takes the first hole that fits each space */
        assert(apu_gp_reverb_set(base, 0, &p, false) == 0 && !apu_gp_reverb_on());
        assert(XR(GP_LOOP_FLAGS) == 4u);                              /* the lowpass went with it */
        assert(apu_gp_kernel_set(base, 3, &e, params, true) == 0);
        assert(apu_gp_debug_slot(3, &ep, &ex, NULL, &sc));
        assert(ex == GP_LOOP_KX_BASE && sc == APU_REVERB_SCRATCH_BASE);    /* the reverb's X and scratch */
        assert(ep == px + GP_KECHO_WORDS);                            /* 169 words: not in the reverb's 101 */
        assert(XR(GP_LOOP_FLAGS) == (4u | 8u) && XR(ex) == 0x1400u + 32u * 8u);   /* slot 3: bins 8/9 */
        {
            /* the live first fit: the reverb's X ($8C0) fits the sums but no hole between the two echoes */
            const apu_gp_kernel_t *set[APU_GP_SLOTS] = { NULL, NULL, NULL, NULL };
            set[0] = apu_gp_reverb_kernel();
            set[2] = &k;
            set[3] = &e;
            assert(!apu_gp_kernels_fit(set));
            set[2] = NULL;                                            /* slot 2 emptied: its block joins the hole */
            assert(apu_gp_kernels_fit(set));
        }
        /* an ABI 1 kernel only alone */
        assert(apu_gp_kernel_set(base, 0, &legacy, params, true) == -1 && !apu_gp_kernel_on(0));
        assert(apu_gp_kernel_set(base, 2, NULL, NULL, false) == 0 && apu_gp_kernel_set(base, 3, NULL, NULL, false) == 0);
        assert(XR(GP_LOOP_FLAGS) == 0 && !apu_gp_kernel_on(APU_GP_SLOTS));
        assert(apu_gp_kernel_set(base, 1, &legacy, params, true) == 0 && apu_gp_kernel_on(1));
        assert(XR(GP_LOOP_FLAGS) == 0x10u && PR(GP_LOOP_KERNEL) == 0x00000Cu);
        assert(XR(GP_LOOP_KIN) == 0x1400u + 32u * 4u && XR(GP_LOOP_KSCR) == APU_REVERB_SCRATCH_BASE);
        assert(XR(GP_LOOP_KPARAM) == 200u && XR(GP_LOOP_K_RHEAD) == 0 && XR(GP_LOOP_K_WHEAD) == GP_LOOP_FIFO2);
        assert(apu_gp_kernel_set(base, 0, &k, params, true) == -1);   /* nothing beside it */
        params[1] = 7u;
        assert(apu_gp_kernel_set(base, 1, &legacy, params, true) == 0 && XR(GP_LOOP_MBOX) == GP_LOOP_KPARAM + 1u);
        assert(apu_gp_kernel_set(base, 1, NULL, NULL, false) == 0 && XR(GP_LOOP_FLAGS) == 0);
        assert(apu_gp_debug_code_restarts() == 0);
        apu_vp_deinit(base);
    }
    printf("    -> image checks (ABI 1 and 2), load beside the reverb (P window, relocated, trampoline, ABI words "
           "at the slot's X block, its bins), live parameters at its KPARAM, first fit reuses a freed hole, "
           "ABI 1 only alone [PASS]\n");

    printf("[9] Budgets and relocation...\n");
    {
        apu_gp_kernel_t echo, chorus, kecho, legacy, big;
        const apu_gp_kernel_t *rv = apu_gp_reverb_kernel();
        const apu_gp_kernel_t *set[APU_GP_SLOTS];
        static const uint32_t abi1[5] = { APU_GP_IMAGE_MAGIC, 1u, 0u, 0u, 0x00000Cu };
        /* every kind of reloc: P word, 12-bit P field, X word, Y word (and a word left alone) */
        static const uint32_t img[APU_GP_IMAGE_HEADER2 + 4u + 6u] = {
            APU_GP_IMAGE_MAGIC, 2u, 0u, 0u, 0x30u, 0x10u, 4u,
            0x0001u, 0x1002u, 0x2004u, 0x3005u,
            0x60F400u, 0x000003u, 0x0C0005u, 0x56F000u, 0x000028u, 0x000002u,
        };
        uint32_t out[6];
        assert(apu_gp_kernel_parse(gp_kefx_echo_image, GP_KEFX_ECHO_IMAGE_WORDS, &echo) == 0);
        assert(apu_gp_kernel_parse(gp_kefx_chorus_image, GP_KEFX_CHORUS_IMAGE_WORDS, &chorus) == 0);
        assert(apu_gp_kernel_parse(gp_kecho_image, GP_KECHO_IMAGE_WORDS, &kecho) == 0);
        assert(apu_gp_kernel_parse(abi1, 5u, &legacy) == 0);
        assert(apu_gp_kernel_parse(img, sizeof(img) / sizeof(img[0]), &big) == 0);
        apu_gp_kernel_relocate(&big, 0x300u, 0x500u, 0x40u, out);
        assert(out[0] == 0x60F400u && out[1] == 0x303u && out[2] == 0x0C0305u);   /* P word, short jmp */
        assert(out[3] == 0x56F000u && out[4] == 0x528u && out[5] == 0x42u);       /* X word, Y word */
        set[0] = rv; set[1] = &echo; set[2] = &chorus; set[3] = NULL;
        assert(apu_gp_kernels_fit(set));                              /* reverb + echo + chorus */
        set[3] = &kecho;
        assert(apu_gp_kernels_fit(set));                              /* and the example echo */
        set[3] = rv;
        assert(!apu_gp_kernels_fit(set));                             /* two reverbs: X (2 x $8C0) */
        set[3] = &chorus;
        assert(!apu_gp_kernels_fit(set));                             /* two choruses: Y (2 x 2048) */
        set[2] = &echo; set[3] = &echo;
        assert(!apu_gp_kernels_fit(set));                             /* scratch: 180 + 3 x 128 KiB */
        set[0] = NULL;
        assert(apu_gp_kernels_fit(set));
        big.descriptors = 51u;
        set[0] = rv; set[1] = &big; set[2] = set[3] = NULL;
        assert(!apu_gp_kernels_fit(set));                             /* descriptors: 50 + 51 */
        set[1] = &legacy;
        assert(!apu_gp_kernels_fit(set));                             /* ABI 1 with another */
        set[0] = NULL;
        assert(apu_gp_kernels_fit(set));                              /* ABI 1 alone */
    }
    printf("    -> the four reloc kinds; reverb + echo + chorus fit, two reverbs (X), two choruses (Y), "
           "scratch, descriptors and ABI 1 beside another do not [PASS]\n");
    apu_mem_shutdown();
    free(s_bar0);
    printf("=== All GP Stage Tests Passed Successfully! ===\n");
    return 0;
}
