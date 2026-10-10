/*
 * Reads, without changing anything, the APU state DirectSound (the dashboard)
 * leaves behind when it launches this program: the VP registers, and the RAM
 * the HRTF target/current address registers (0x2038, 0x203C) and the voice
 * array (VPVADDR) point to. The layout of the HRTF arrays is what the library
 * needs to drive the HRTF stage without front-end methods.
 *
 * Run it straight from the dashboard after a power-on. Log: E:\apu_dsdump.txt
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <nxdk/mount.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "mcpx_apu_regs.h"

#define BAR0 ((volatile uint32_t *)0xFE800000u)

static char s_log[120 * 1024];
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
}

static void save_log(void) {
    FILE *f;
    if (!nxIsDriveMounted('E')) {
        nxMountDrive('E', "\\Device\\Harddisk0\\Partition1\\");
    }
    f = fopen("E:\\apu_dsdump.txt", "wb");
    if (f) {
        fwrite(s_log, 1, s_log_len, f);
        fclose(f);
    }
}

/* Map `bytes` of physical RAM and log every `block`-byte block that is not all zero (up to `max_blocks`) */
static void dump_phys(const char *what, uint32_t phys, uint32_t bytes, uint32_t block, uint32_t max_blocks) {
    volatile uint32_t *m;
    uint32_t b, w, shown = 0, nonzero_blocks = 0;
    logf_("\n== %s at %08lx (%lu bytes, %lu-byte blocks)\n", what, (unsigned long)phys, (unsigned long)bytes,
          (unsigned long)block);
    save_log();   /* in case the mapping hangs */
    if (phys == 0 || (phys & 0xFFFu)) {
        logf_("   not page aligned / zero: skipped\n");
        return;
    }
    m = (volatile uint32_t *)MmMapIoSpace(phys, bytes, PAGE_READWRITE | PAGE_NOCACHE);
    if (!m) {
        logf_("   MmMapIoSpace failed\n");
        return;
    }
    for (b = 0; b < bytes / block; b++) {
        bool any = false;
        for (w = 0; w < block / 4u; w++) {
            if (m[b * (block / 4u) + w]) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }
        nonzero_blocks++;
        if (shown < max_blocks) {
            shown++;
            logf_("   block %3lu (+%05lx):", (unsigned long)b, (unsigned long)(b * block));
            for (w = 0; w < block / 4u; w++) {
                logf_("%s%08lx", (w % 8u) ? " " : "\n     ", (unsigned long)m[b * (block / 4u) + w]);
            }
            logf_("\n");
        }
    }
    logf_("   %lu of %lu blocks not zero\n", (unsigned long)nonzero_blocks, (unsigned long)(bytes / block));
    MmUnmapIoSpace((PVOID)m, bytes);
    save_log();
}

int main(void) {
    uint32_t a, cnt = 0, ht, hc, vv;

    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    debugPrint("apu_dsdump: reading what DirectSound left in the APU...\n");
    logf_("apu_dsdump: APU state left by the dashboard (read only)\n");
    logf_("registers (non-zero) 1100-1160, 1300-1344, 1500-1510, 2000-20fc:");
    {
        static const uint32_t rng[][2] = { { 0x1100, 0x1160 }, { 0x1300, 0x1344 }, { 0x1500, 0x1510 }, { 0x2000, 0x20FC } };
        uint32_t q;
        for (q = 0; q < 4u; q++) {
            for (a = rng[q][0]; a <= rng[q][1]; a += 4u) {
                uint32_t v = BAR0[a / 4u];
                if (v) {
                    logf_("%s%04lx=%08lx", (cnt % 6u) ? " " : "\n   ", (unsigned long)a, (unsigned long)v);
                    cnt++;
                }
            }
        }
    }
    logf_("\n");
    save_log();

    ht = BAR0[0x2038 / 4u];
    hc = BAR0[0x203C / 4u];
    vv = BAR0[MCPX_APU_VPVADDR / 4u];
    /* HRTF target and current arrays: 16 KiB each is generous; 64-byte blocks
     * (one 31-tap stereo FIR + ITD is 64 bytes in the method encoding) */
    dump_phys("HRTF target array (0x2038)", ht, 0x4000u, 64u, 12u);
    dump_phys("HRTF current array (0x203C)", hc, 0x4000u, 64u, 12u);
    /* Voice array: 256 records of 128 bytes; shows how DirectSound fills CFG_HRTF_TARGET */
    dump_phys("voice array (VPVADDR)", vv, 0x8000u, 128u, 16u);

    logf_("\ndone\n");
    save_log();
    debugPrint("done: E:\\apu_dsdump.txt\n");
    for (;;) Sleep(1000);
    return 0;
}
