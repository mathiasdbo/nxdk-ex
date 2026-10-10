/*
 * Console probe round 3 (log saved after every step, E:\apu_probe3.txt):
 *
 *   1  AL_LOOPING through OpenAL while playing (apu_vp_voice_set_loop):
 *      a looping source released must end; a one-shot set looping must keep playing
 *   2  start-up: 20 deinit/init cycles of the driver; does every start run frames?
 *      2b: which SECTL values run frames (0x08..0x0F)
 *      (twice a start ran 1-3 frames and stopped: openal_engine, apu_vp_stress F3)
 *   3  128 voices: looping voices on handles 0..127; which ones advance? (apu_probe2
 *      C showed handles 0-3 playing as plain voices)
 *   4  reset: freeze the APU with a freshly written record on handle 129 (as the
 *      polyphony bug did), then deinit/init, the PCI command register, and a PCI
 *      power-management D3hot -> D0 cycle (capability at config 0x44)
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <math.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "apu_gp.h"
#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_vp.h"
#include "mcpx_apu_regs.h"

#define BAR0        ((uintptr_t)NV_PAPU_BASE)
#define REG(off)    (*(volatile uint32_t *)(BAR0 + (off)))
#define FRAMES      20000u

static char s_log[64 * 1024];
static size_t s_log_len;

static void logf_(const char *fmt, ...) {
    char line[300];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n > 0 && s_log_len + (size_t)n < sizeof(s_log)) {
        memcpy(s_log + s_log_len, line, (size_t)n);
        s_log_len += (size_t)n;
    }
    debugPrint("%s", line);
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\apu_probe3.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

static int16_t s_tone[FRAMES];
static uint32_t s_pcm_phys;

static uint32_t gp_delta(uint32_t ms) {
    uint32_t g0 = apu_gp_frames(BAR0);
    Sleep(ms);
    return (apu_gp_frames(BAR0) - g0) & 0xFFFFFFu;
}

static void run_for(DWORD ms) {
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        alXboxUpdateVoices();
        Sleep(16);
    }
}

static ALint geti(ALuint s, ALenum p) {
    ALint v = -1;
    alGetSourcei(s, p, &v);
    return v;
}

/* ---- 1: AL_LOOPING while playing ---- */
static void part_looping(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    ALuint b, s;
    ALint lp[2] = { 4800, 14400 };
    int k;
    if (!dev) {
        logf_("1: alcOpenDevice failed\n");
        return;
    }
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, s_tone, (ALsizei)sizeof(s_tone), 48000);   /* 0.42 s */
    alBufferiv(b, AL_LOOP_POINTS_SOFT, lp);
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)b);
    alSourcei(s, AL_SOURCE_RELATIVE, AL_TRUE);

    logf_("-- 1a: looping source, released after 1 s: must end within ~0.2 s\n");
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    run_for(1000);
    logf_("   before release: state %s, offset %ld\n", geti(s, AL_SOURCE_STATE) == AL_PLAYING ? "PLAYING" : "other",
          (long)geti(s, AL_SAMPLE_OFFSET));
    alSourcei(s, AL_LOOPING, AL_FALSE);
    {
        DWORD t0 = GetTickCount();
        while (geti(s, AL_SOURCE_STATE) == AL_PLAYING && GetTickCount() - t0 < 2000u) {
            run_for(16);
        }
        logf_("   %s %lu ms after the release\n", geti(s, AL_SOURCE_STATE) == AL_STOPPED ? "STOPPED" : "STILL PLAYING",
              (unsigned long)(GetTickCount() - t0));
    }
    logf_("-- 1b: one-shot set looping at 0.1 s: must still play after 2 s, inside the loop\n");
    alSourcei(s, AL_LOOPING, AL_FALSE);
    alSourcePlay(s);
    run_for(100);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    for (k = 0; k < 4; k++) {
        run_for(500);
        logf_("   %4d ms: state %s, offset %ld\n", 100 + (k + 1) * 500,
              geti(s, AL_SOURCE_STATE) == AL_PLAYING ? "PLAYING" : "STOPPED", (long)geti(s, AL_SAMPLE_OFFSET));
    }
    logf_("-- 1c: the same, set looping late (offset past the loop end): one pass to the end, then the loop\n");
    alSourceStop(s);
    alSourcei(s, AL_LOOPING, AL_FALSE);
    alSourcei(s, AL_SAMPLE_OFFSET, 15000);
    alSourcePlay(s);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    for (k = 0; k < 4; k++) {
        run_for(300);
        logf_("   %4d ms: state %s, offset %ld\n", (k + 1) * 300,
              geti(s, AL_SOURCE_STATE) == AL_PLAYING ? "PLAYING" : "STOPPED", (long)geti(s, AL_SAMPLE_OFFSET));
    }
    alSourceStop(s);
    alSourcei(s, AL_BUFFER, 0);
    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b);
    logf_("   frames %s\n", gp_delta(20) ? "running" : "STOPPED");
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    save_log();
}

/* ---- 2: start-up reliability ---- */
static void part_startup(void) {
    int k, bad = 0;
    logf_("-- 2: 20 driver deinit/init cycles\n");
    apu_vp_debug_set_flags(APU_VP_DBG_NO_AC97);
    for (k = 0; k < 20; k++) {
        uint32_t fectl0 = REG(MCPX_APU_FECTL), d;
        if (apu_vp_init(BAR0) != 0) {
            logf_("   %2d: apu_vp_init failed\n", k);
            bad++;
            continue;
        }
        d = gp_delta(20);
        if (d < 10u) {
            bad++;
            logf_("   %2d: only %lu frames in 20 ms (GP program %s); before init FECTL %08lx, now FECTL %08lx "
                  "FEDECMETH %08lx SECTL %08lx GPRST %08lx\n",
                  k, (unsigned long)d, apu_gp_program_loaded() ? "loaded" : "NOT LOADED", (unsigned long)fectl0,
                  (unsigned long)REG(MCPX_APU_FECTL), (unsigned long)REG(MCPX_APU_FEDECMETH),
                  (unsigned long)REG(MCPX_APU_SECTL), (unsigned long)REG(MCPX_APU_GPRST));
            save_log();
        }
        if (k < 19) {
            apu_vp_deinit(BAR0);
            Sleep(5);
        }
    }
    logf_("   %d of 20 starts did not run frames\n", bad);
    save_log();
}

/* ---- 2b: which SECTL bits frames need (our own measurement; the library's
 * 0x0F was first seen in the state DirectSound leaves, see the clean-room note) ---- */
static void part_sectl(void) {
    uint32_t v;
    logf_("-- 2b: SECTL 0x08..0x0F: GP frames in 30 ms each\n");
    for (v = 0x08u; v <= 0x0Fu; v++) {
        uint32_t d;
        REG(MCPX_APU_SECTL) = 0;
        Sleep(5);
        REG(MCPX_APU_SECTL) = v;
        d = gp_delta(30);
        logf_("   SECTL %02lx: %lu frames (read back %08lx)\n", (unsigned long)v, (unsigned long)d,
              (unsigned long)REG(MCPX_APU_SECTL));
        save_log();
    }
    REG(MCPX_APU_SECTL) = MCPX_APU_SECTL_RUN;
    Sleep(10);
}

/* ---- 3: voices on handles 0..127 ---- */
static void params(apu_vp_voice_params_t *p) {
    int i;
    memset(p, 0, sizeof(*p));
    p->phys = s_pcm_phys;
    p->bytes = FRAMES * 2u;
    p->channels = 1;
    p->bits = 16;
    p->loop = true;
    for (i = 0; i < APU_VP_OUTPUTS; i++) {
        p->bins[i] = (uint8_t)(i & 1);
        p->vols[i] = 0xFFFu;
    }
    p->vols[0] = p->vols[1] = 0x600u;
    p->hrtf_entry = -1;
    p->tail_bytes = APU_VP_SILENT_TAIL_BYTES;
}

static bool part_voices(void) {
    static uint32_t c0[128];
    apu_vp_voice_params_t p;
    uint32_t h, moved = 0, moved_low = 0, fails = 0;
    logf_("-- 3: looping voices on handles 0..127 at once\n");
    params(&p);
    for (h = 0; h < 128u; h++) {
        p.pitch = (int16_t)((int)(h % 16u) * 64 - 512);
        if (apu_vp_voice_start(BAR0, h, &p) != 0) fails++;
    }
    Sleep(50);
    for (h = 0; h < 128u; h++) c0[h] = apu_vp_voice_position(h);
    Sleep(100);
    for (h = 0; h < 128u; h++) {
        if (apu_vp_voice_position(h) != c0[h]) {
            moved++;
            if (h < 64u) moved_low++;
        }
    }
    logf_("   %lu of 128 advance (%lu of handles 0..63), %lu starts failed; frames %s\n", (unsigned long)moved,
          (unsigned long)moved_low, (unsigned long)fails, gp_delta(20) ? "running" : "STOPPED");
    save_log();
    /* churn on all 128 handles for 5 s: stop and restart random ones */
    {
        DWORD t0 = GetTickCount();
        uint32_t restarts = 0, r = 12345u;
        while (GetTickCount() - t0 < 5000u) {
            r = r * 1664525u + 1013904223u;
            h = (r >> 8) % 128u;
            apu_vp_voice_off(BAR0, h);
            Sleep(1);   /* past the one-frame quarantine */
            apu_vp_voice_start(BAR0, h, &p);
            restarts++;
            if ((restarts & 63u) == 0u) {
                apu_vp_service(BAR0);
                if (!gp_delta(5)) break;
            }
        }
        logf_("   churn: %lu restarts in 5 s, frames %s\n", (unsigned long)restarts, gp_delta(20) ? "running" : "STOPPED");
    }
    for (h = 0; h < 128u; h++) apu_vp_voice_off(BAR0, h);
    save_log();
    return gp_delta(20) != 0u;
}

/* ---- 4: freeze, then reset ---- */
static uint32_t pci_rd(uint32_t off) {
    uint32_t v = 0;
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, off, &v, 4, FALSE);
    return v;
}

static void pci_wr(uint32_t off, uint32_t v) {
    HalReadWritePCISpace(MCPX_APU_PCI_BUS, MCPX_APU_PCI_SLOT, off, &v, 4, TRUE);
}

static bool reinit(const char *tag) {
    uint32_t d;
    apu_vp_deinit(BAR0);
    Sleep(10);
    if (apu_vp_init(BAR0) != 0) {
        logf_("   %s: apu_vp_init failed\n", tag);
        save_log();
        return false;
    }
    d = gp_delta(30);
    logf_("   %s: GP program %s, %lu frames in 30 ms -> %s; FECTL %08lx FEDECMETH %08lx\n", tag,
          apu_gp_program_loaded() ? "loaded" : "NOT LOADED", (unsigned long)d, d > 10u ? "RECOVERED" : "still stopped",
          (unsigned long)REG(MCPX_APU_FECTL), (unsigned long)REG(MCPX_APU_FEDECMETH));
    save_log();
    return d > 10u;
}

static void part_reset(bool frozen) {
    uint32_t cmd, bar0, pmcsr;
    if (!frozen) {
        apu_vp_voice_params_t p;
        volatile uint32_t *r = (volatile uint32_t *)apu_vp_debug_voice_record(129);
        uint32_t w, vbin, fb, vola, volb, volc;
        logf_("-- 4: freeze: a freshly written record on handle 129\n");
        params(&p);
        /* what apu_vp_voice_start() writes (it refuses handle 129 itself); BA of a mapping it made */
        apu_vp_voice_start(BAR0, 70, &p);
        apu_vp_pack_bins(p.bins, &vbin, &fb);
        apu_vp_pack_volumes(p.vols, &vola, &volb, &volc);
        for (w = 0; w < 32u; w++) r[w] = 0;
        r[MCPX_VOICE_CFG_VBIN / 4] = vbin;
        r[MCPX_VOICE_CFG_FMT / 4] = apu_vp_format(1, 16, true) | fb;
        r[MCPX_VOICE_CFG_HRTF_TARGET / 4] = MCPX_HRTF_NONE;
        r[MCPX_VOICE_CUR_PSL_START / 4] = ((volatile uint32_t *)apu_vp_debug_voice_record(70))[MCPX_VOICE_CUR_PSL_START / 4] & 0xFFFFFFu;
        r[MCPX_VOICE_PAR_NEXT / 4] = FRAMES - 1u;
        r[MCPX_VOICE_TAR_VOLA / 4] = vola;
        r[MCPX_VOICE_TAR_VOLB / 4] = volb;
        r[MCPX_VOICE_TAR_VOLC / 4] = volc;
        r[MCPX_VOICE_TAR_PITCH_LINK / 4] = REG(MCPX_APU_TVL2D) & 0xFFFFu;
        r[MCPX_VOICE_PAR_STATE / 4] = MCPX_PAR_STATE_ACTIVE_VOICE;
        REG(MCPX_APU_TVL2D) = 129;
        Sleep(50);
        frozen = gp_delta(30) == 0u;
        logf_("   frames %s; FECTL %08lx FEDECMETH %08lx FEDECPARAM %08lx\n", frozen ? "STOPPED (frozen)" : "running (no freeze)",
              (unsigned long)REG(MCPX_APU_FECTL), (unsigned long)REG(MCPX_APU_FEDECMETH),
              (unsigned long)REG(MCPX_APU_FEDECPARAM));
        save_log();
        if (!frozen) return;
    }
    logf_("-- 4a: deinit + init\n");
    if (reinit("4a")) return;
    logf_("-- 4b: PCI command: memory and bus master off 20 ms\n");
    cmd = pci_rd(0x04);
    pci_wr(0x04, cmd & ~0x6u);
    Sleep(20);
    pci_wr(0x04, cmd);
    if (reinit("4b")) return;
    logf_("-- 4c: PCI power management D3hot -> D0 (PMCSR at 0x48)\n");
    bar0 = pci_rd(0x10);
    cmd = pci_rd(0x04);
    pmcsr = pci_rd(0x48);
    save_log();
    pci_wr(0x48, (pmcsr & ~3u) | 3u);
    Sleep(20);
    logf_("   in D3: PMCSR %08lx, a BAR0 read %08lx\n", (unsigned long)pci_rd(0x48), (unsigned long)REG(MCPX_APU_SECTL));
    pci_wr(0x48, pmcsr & ~3u);
    Sleep(20);
    logf_("   back in D0: PMCSR %08lx BAR0 %08lx (was %08lx) command %08lx (was %08lx) SECTL %08lx\n",
          (unsigned long)pci_rd(0x48), (unsigned long)pci_rd(0x10), (unsigned long)bar0, (unsigned long)pci_rd(0x04),
          (unsigned long)cmd, (unsigned long)REG(MCPX_APU_SECTL));
    pci_wr(0x10, bar0);
    pci_wr(0x04, cmd);
    if (reinit("4c")) return;
    logf_("   no reset brought the frames back\n");
    save_log();
}

int main(void) {
    uint32_t i;
    bool alive;
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    logf_("apu_probe3: AL_LOOPING live, start-up cycles, 128 voices, reset\n");
    for (i = 0; i < FRAMES; i++) s_tone[i] = (int16_t)(5000.0f * sinf(6.2831853f * 440.0f * (float)i / 48000.0f));
    {
        uint32_t ph;
        int16_t *pcm = (int16_t *)apu_mem_alloc_table(FRAMES * 2u + APU_VP_SILENT_TAIL_BYTES, &ph);
        memcpy(pcm, s_tone, sizeof(s_tone));
        s_pcm_phys = ph;
    }
    part_looping();
    part_startup();       /* leaves the driver initialised */
    part_sectl();
    alive = part_voices();
    part_reset(!alive);
    logf_("done\n");
    save_log();
    for (;;) Sleep(1000);
    return 0;
}
