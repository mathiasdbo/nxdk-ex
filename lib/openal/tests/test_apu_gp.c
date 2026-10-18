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

/* Scratch space of the default program: its image and the reverb's delay lines */
#define DEFAULT_SCRATCH ((APU_REVERB_SCRATCH_BASE + apu_reverb_scratch_bytes() + 4095u) & ~4095u)

/*
 * GP stage (apu_gp.c) against a plain-memory BAR0: the bootstrap image and
 * DMA descriptor it writes, the FIFO setup, and the reader of the output ring
 * (the test plays the DSP's part by writing samples and advancing GPOFCUR0).
 */

#define BAR0_SIZE 0x80000u

static uint8_t *s_bar0;
static apu_reverb_word_t s_words[APU_REVERB_MAX_WORDS];
static uint32_t reg(uint32_t off) { return *(volatile uint32_t *)(s_bar0 + off); }
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
    /* The default program: the frame loop at P:0, kernel 0 (the reverb) at P:$100, NOP elsewhere */
    for (i = 0; i < GP_LOOP_WORDS; i++) {
        assert(scratch[i] == gp_loop_words[i]);
    }
    for (i = GP_LOOP_WORDS; i < GP_KFREEVERB_ORIGIN; i++) {
        assert(scratch[i] == 0);
    }
    for (i = 0; i < GP_KFREEVERB_WORDS; i++) {
        assert(scratch[GP_KFREEVERB_ORIGIN + i] == gp_kfreeverb_words[i]);
    }
    assert(scratch[GP_KFREEVERB_ORIGIN + GP_KFREEVERB_WORDS] == 0 && scratch[0x7FF] == 0);
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

    printf("[7] Reverb control...\n");
    {
        apu_reverb_params_t p;
        static const uint32_t image[3] = { 0x08F484u, 0x000001u, 0x0C0000u };
        uint32_t w0;
        assert(apu_vp_init(base) == 0);
        assert(!apu_gp_reverb_on() && reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FLAGS) == 0);
        assert(apu_reverb_preset(3, &p) == 0);                       /* hall */
        assert(apu_gp_reverb_set(base, &p, true) == 0 && apu_gp_reverb_on());
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FLAGS) == 1u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_KFREEVERB_RCHAIN) == GP_KFREEVERB_RCHAIN + 16u);   /* chains written */
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_RHEAD) == GP_KFREEVERB_RCHAIN);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_WHEAD) == GP_KFREEVERB_WCHAIN);
        w0 = reg(MCPX_APU_GP_XMEM + 4u * (GP_KFREEVERB_WCHAIN + 4u));
        assert(w0 == 4u * 1200u);                                    /* 25 ms pre-delay */
        p.gain = 0.25f;                                              /* gains only: through the mailbox */
        set_reg(MCPX_APU_GP_XMEM + 4u * (GP_KFREEVERB_WCHAIN + 4u), 0x777u);
        assert(apu_gp_reverb_set(base, &p, true) == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_KFREEVERB_WET) == 0x200000u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_KFREEVERB_WCHAIN + 4u)) == 0x777u);   /* layout untouched */
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_MB_SEQ) == 1u && reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_MB_ACK) == 1u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_MB_N) == apu_reverb_coefficients(&p, s_words));
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_MBOX + 2u * 2u)) == GP_KFREEVERB_WET);   /* pair 2: the wet gain */
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_MBOX + 2u * 2u + 1u)) == 0x200000u);
        p.pre_delay = 0.05f;                                         /* new pre-delay: laid out again */
        assert(apu_gp_reverb_set(base, &p, true) == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_KFREEVERB_WCHAIN + 4u)) == 4u * 2400u);
        assert(apu_gp_reverb_set(base, &p, false) == 0 && !apu_gp_reverb_on());
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FLAGS) == 0);
        assert(apu_gp_mailbox(base, s_words, GP_LOOP_MB_MAX + 1u) == -1);   /* more than the mailbox holds */
        apu_vp_deinit(base);
        apu_gp_debug_set_program(image, 3);                          /* another program: no reverb */
        assert(apu_vp_init(base) == 0);
        assert(apu_gp_reverb_set(base, &p, true) == -1 && !apu_gp_reverb_on());
        assert(apu_gp_mailbox(base, s_words, 1u) == -1);              /* no mailbox in another program */
        apu_vp_deinit(base);
        apu_gp_debug_set_program(NULL, 0);
    }
    printf("    -> on, live gains, a new pre-delay re-lays the lines, off; refused for another program [PASS]\n");

    printf("[8] Custom kernels (AL_XBOX_gp_program)...\n");
    {
        apu_gp_kernel_t k;
        apu_reverb_params_t p;
        uint32_t params[APU_GP_KERNEL_PARAMS], i, last, peak;
        static uint32_t bad[GP_KECHO_IMAGE_WORDS];
        memset(params, 0, sizeof(params));
        params[0] = 100u;
        params[2] = 0x400000u;
        /* the image checks (before a device: against the scratch apu_gp_init will allocate) */
        assert(apu_gp_kernel_scratch_max() == DEFAULT_SCRATCH - APU_REVERB_SCRATCH_BASE);
        assert(apu_gp_kernel_parse(gp_kecho_image, GP_KECHO_IMAGE_WORDS, &k) == 0);
        assert(k.code == gp_kecho_image + APU_GP_IMAGE_HEADER && k.words == GP_KECHO_WORDS);
        assert(k.scratch_bytes == GP_KECHO_K_SCRATCH_BYTES && k.descriptors == GP_KECHO_K_DESCRIPTORS);
        memcpy(bad, gp_kecho_image, sizeof(bad));
        bad[0] ^= 1u;                                                 /* magic */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[0] = APU_GP_IMAGE_MAGIC; bad[1] = 2u;                     /* ABI */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[1] = APU_GP_IMAGE_ABI; bad[2] = apu_gp_kernel_scratch_max() + 1u;   /* scratch */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[2] = 0; bad[3] = APU_GP_KERNEL_MAX_DESCRIPTORS + 1u;      /* descriptors */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        bad[3] = 0; bad[10] = 0x1000000u;                             /* not a 24-bit word */
        assert(apu_gp_kernel_parse(bad, GP_KECHO_IMAGE_WORDS, &k) == -1);
        assert(apu_gp_kernel_parse(gp_kecho_image, APU_GP_IMAGE_HEADER, &k) == -1);   /* no code */
        assert(apu_gp_kernel_parse(gp_kecho_image, APU_GP_IMAGE_HEADER + APU_GP_KERNEL_MAX_WORDS + 1u, &k) == -1);
        assert(apu_gp_kernel_parse(gp_kecho_image, GP_KECHO_IMAGE_WORDS, &k) == 0);
        assert(apu_gp_kernel_set(base, &k, params, true) == -1);     /* no device */

        assert(apu_vp_init(base) == 0);
        /* the loop's mix descriptor and idle meter are set up for every kernel */
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_FIFO2 + 3u)) == GP_LOOP_OUTL);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FIFO2) == 0x4000u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_IDLMIN) == 0x7FFFFFu);
        apu_gp_load(base, &last, &peak);
        assert(last == 0 && peak == 0);                               /* no meter on the host */
        assert(apu_reverb_preset(3, &p) == 0 && apu_gp_reverb_set(base, &p, true) == 0);
        set_reg(MCPX_APU_GP_XMEM + 4u * 0x200u, 0x123456u);           /* the reverb's X, to be cleared */
        memset((uint8_t *)(uintptr_t)apu_gp_debug_scratch() + APU_REVERB_SCRATCH_BASE, 0x5A, 64);
        assert(apu_gp_kernel_set(base, &k, params, true) == 0);
        assert(apu_gp_kernel_on() && !apu_gp_reverb_on());
        for (i = 0; i < GP_KECHO_WORDS; i++) {                        /* P window and bootstrap image */
            assert(reg(MCPX_APU_GP_PMEM + 4u * (GP_LOOP_KERNEL + i)) == gp_kecho_image[APU_GP_IMAGE_HEADER + i]);
            assert(apu_gp_debug_scratch()[GP_LOOP_KERNEL + i] == gp_kecho_image[APU_GP_IMAGE_HEADER + i]);
        }
        assert(reg(MCPX_APU_GP_XMEM + 4u * 0x200u) == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_KIN) == 0x1440u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_KSCR) == APU_REVERB_SCRATCH_BASE);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_KSCRN) == GP_KECHO_K_SCRATCH_BYTES);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_KPARAM) == 100u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_KPARAM + 2u)) == 0x400000u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_RHEAD) == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_WHEAD) == GP_LOOP_FIFO2);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FLAGS) == 1u);
        assert(((const uint8_t *)apu_gp_debug_scratch())[APU_REVERB_SCRATCH_BASE + 63] == 0);   /* scratch zeroed */
        /* new parameters, same code: the changed ones through the mailbox, nothing reloaded */
        set_reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_RHEAD, 0x110u);     /* as the kernel would */
        params[0] = 200u;
        params[31] = 0x7FFFFFu;
        assert(apu_gp_kernel_set(base, &k, params, true) == 0);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_MB_N) == 2u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_MBOX) == GP_LOOP_KPARAM);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_MBOX + 1u)) == 200u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_MBOX + 2u)) == GP_LOOP_KPARAM + 31u);
        assert(reg(MCPX_APU_GP_XMEM + 4u * (GP_LOOP_KPARAM + 31u)) == 0x7FFFFFu);
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_RHEAD) == 0x110u);   /* the kernel kept running */
        /* the reverb again: its code goes back, the custom kernel stops */
        assert(apu_gp_reverb_set(base, &p, true) == 0 && apu_gp_reverb_on() && !apu_gp_kernel_on());
        for (i = 0; i < GP_KFREEVERB_WORDS; i++) {
            assert(reg(MCPX_APU_GP_PMEM + 4u * (GP_LOOP_KERNEL + i)) == gp_kfreeverb_words[i]);
        }
        assert(reg(MCPX_APU_GP_PMEM + 4u * (GP_LOOP_KERNEL + GP_KECHO_WORDS - 1u)) == 0);   /* the rest: NOP */
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_K_RHEAD) == GP_KFREEVERB_RCHAIN);
        assert(apu_gp_kernel_set(base, &k, params, false) == 0 && apu_gp_reverb_on());   /* off: not ours */
        assert(apu_gp_kernel_set(base, &k, params, true) == 0 && apu_gp_kernel_on());
        assert(apu_gp_kernel_set(base, NULL, NULL, false) == 0 && !apu_gp_kernel_on());
        assert(reg(MCPX_APU_GP_XMEM + 4u * GP_LOOP_FLAGS) == 0);
        assert(apu_gp_debug_code_restarts() == 0);
        apu_vp_deinit(base);
    }
    printf("    -> image checks, load (P window, ABI words, scratch), live parameters, back to the reverb [PASS]\n");
    apu_mem_shutdown();
    free(s_bar0);
    printf("=== All GP Stage Tests Passed Successfully! ===\n");
    return 0;
}
