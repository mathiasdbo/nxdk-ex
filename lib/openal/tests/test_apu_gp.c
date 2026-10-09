#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "apu_gp.h"
#include "apu_vp.h"
#include "apu_mem.h"
#include "mcpx_apu_regs.h"

/*
 * GP stage (apu_gp.c) against a plain-memory BAR0: the bootstrap image and
 * DMA descriptor it writes, the FIFO setup, and the reader of the output ring
 * (the test plays the DSP's part by writing samples and advancing GPOFCUR0).
 */

#define BAR0_SIZE 0x80000u

static uint8_t *s_bar0;
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
    assert(apu_vp_init(base) == 0);
    assert(apu_gp_ready());
    assert(reg(MCPX_APU_GPRST) == 3u);                          /* out of reset: bootstrap ran */
    assert(reg(MCPX_APU_GPSADDR) != 0 && reg(MCPX_APU_GPSMAXSGE) == 1u);   /* 8 KiB image = 2 pages */
    assert(reg(MCPX_APU_GPFADDR) != 0 && reg(MCPX_APU_GPFMAXSGE) == APU_GP_FIFO_BYTES / 4096u - 1u);
    assert(reg(MCPX_APU_GPOFBASE0) == 0 && reg(MCPX_APU_GPOFEND0) == APU_GP_FIFO_BYTES);
    assert(reg(MCPX_APU_GPOFCUR0) < reg(MCPX_APU_GPOFEND0));
    scratch = apu_gp_debug_scratch();
    for (i = 0; i < APU_GP_PROGRAM_WORDS; i++) {
        assert(scratch[i] == apu_gp_program[i]);
    }
    assert(scratch[APU_GP_PROGRAM_WORDS] == 0 && scratch[0x7FF] == 0);   /* rest of P:0..$7FF is NOP */
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
    apu_mem_shutdown();
    free(s_bar0);
    printf("=== All GP Stage Tests Passed Successfully! ===\n");
    return 0;
}
