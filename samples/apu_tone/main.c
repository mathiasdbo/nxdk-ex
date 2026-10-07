#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <xboxkrnl/xboxkrnl.h>

#include "apu_hardware.h"
#include "apu_mem.h"
#include "apu_gp_ucode.h"
#include "apu_voice.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef NVAPU_MAXRAM
#define NVAPU_MAXRAM 0x03FFAFFF
#endif

/*
 * ============================================================================
 * Audio Stream Constants: 48 kHz Stereo 16-bit PCM Sine Wave
 * ============================================================================
 * 4800 stereo frames at 48000 Hz = 0.1s (100 ms).
 * A 480 Hz tone over 0.1s completes exactly 48 integer cycles:
 *   480 Hz * 0.1 s = 48 cycles.
 * When looped back from sample frame 4799 to frame 0, phase continuity is
 * perfectly preserved (sin(0) = 0), resulting in zero audible pops or clicks.
 * ============================================================================
 */
#define SAMPLE_RATE_HZ          48000u
#define SINE_FREQUENCY_HZ       480u
#define NUM_STEREO_FRAMES       4800u
#define NUM_AUDIO_CHANNELS      2u
#define BYTES_PER_SAMPLE        ((uint32_t)sizeof(int16_t))
#define PCM_BUFFER_SIZE_BYTES   (NUM_STEREO_FRAMES * NUM_AUDIO_CHANNELS * BYTES_PER_SAMPLE) /* 19200 bytes */

#define VOICE_ARRAY_SIZE_BYTES  NV_PAPU_VOICE_ARRAY_SIZE_3D /* 8192 bytes (64 * 128B) */
#define VOICE_ARRAY_ALIGN_BYTES NV_PAPU_VOICE_ARRAY_ALIGN   /* 4096 bytes (4KB page aligned) */

#define PRD_TABLE_SIZE_BYTES    4096u                       /* 4096 bytes buffer */
#define PRD_TABLE_ALIGN_BYTES   8u                          /* 8-byte aligned */

int main(void)
{
    /* Initialize video mode for debug console output */
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("====================================================\n");
    debugPrint("  MCPX APU Bare-Metal Voice 0 Audio Playback POC\n");
    debugPrint("====================================================\n\n");

#ifndef OPENAL_APU_REAL_MMIO
    /*
     * Gate: this POC programs the library's own APU register model directly
     * into BAR0, and that model is not xemu-conformant (the GP microcode
     * upload trips an assert and aborts xemu). Do not touch any APU MMIO
     * unless the build explicitly opts in. See lib/openal/docs/XEMU_VERIFICATION.md.
     */
    debugPrint("This POC drives the raw MCPX APU register model, which is\n");
    debugPrint("NOT xemu-conformant: running it can disturb or abort xemu\n");
    debugPrint("(see lib/openal/docs/XEMU_VERIFICATION.md).\n\n");
    debugPrint("Nothing was run and no APU register was touched.\n");
    debugPrint("To run it anyway add -DOPENAL_APU_REAL_MMIO to CFLAGS in the\n");
    debugPrint("environment (CFLAGS=-DOPENAL_APU_REAL_MMIO make); a CFLAGS=... on\n");
    debugPrint("the make command line would drop the Makefile include paths.\n");
    Sleep(10000); /* keep the message on screen */
    return 0;
#endif

    /* Verify hardware data structure contract sizes */
    assert(sizeof(NVAPU_VOICE_CONTEXT_3D) == 128);
    assert(sizeof(NVAPU_PRD_ENTRY) == 8);

    /*
     * ------------------------------------------------------------------------
     * Step 1: Allocate Contiguous Physical Memory via MmAllocateContiguousMemoryEx
     * ------------------------------------------------------------------------
     */
    debugPrint("[1/8] Allocating physically contiguous buffers...\n");

    /* 1.1: 8192-byte Voice Context Array (4096-byte aligned) */
    void *voice_array_virt = MmAllocateContiguousMemoryEx(
        VOICE_ARRAY_SIZE_BYTES,
        0,
        NVAPU_MAXRAM,
        VOICE_ARRAY_ALIGN_BYTES,
        PAGE_READWRITE | PAGE_WRITECOMBINE
    );
    if (!voice_array_virt) {
        debugPrint("FATAL: Failed to allocate Voice Context Array (8192 B)!\n");
        return 1;
    }
    memset(voice_array_virt, 0, VOICE_ARRAY_SIZE_BYTES);
    uint32_t voice_array_phys = (uint32_t)MmGetPhysicalAddress(voice_array_virt);
    assert(((uintptr_t)voice_array_virt & (VOICE_ARRAY_ALIGN_BYTES - 1)) == 0);
    assert((voice_array_phys & (VOICE_ARRAY_ALIGN_BYTES - 1)) == 0);

    /* 1.2: 4096-byte PRD Table buffer (8-byte aligned) */
    void *prd_table_virt = MmAllocateContiguousMemoryEx(
        PRD_TABLE_SIZE_BYTES,
        0,
        NVAPU_MAXRAM,
        PRD_TABLE_ALIGN_BYTES,
        PAGE_READWRITE | PAGE_WRITECOMBINE
    );
    if (!prd_table_virt) {
        debugPrint("FATAL: Failed to allocate PRD Table buffer (4096 B)!\n");
        return 1;
    }
    memset(prd_table_virt, 0, PRD_TABLE_SIZE_BYTES);
    uint32_t prd_table_phys = (uint32_t)MmGetPhysicalAddress(prd_table_virt);
    assert(((uintptr_t)prd_table_virt & (PRD_TABLE_ALIGN_BYTES - 1)) == 0);
    assert((prd_table_phys & (PRD_TABLE_ALIGN_BYTES - 1)) == 0);

    /* 1.3: 19200-byte PCM audio buffer (8-byte aligned) */
    void *pcm_buffer_virt = MmAllocateContiguousMemoryEx(
        PCM_BUFFER_SIZE_BYTES,
        0,
        NVAPU_MAXRAM,
        8,
        PAGE_READWRITE | PAGE_WRITECOMBINE
    );
    if (!pcm_buffer_virt) {
        debugPrint("FATAL: Failed to allocate PCM Audio Buffer (19200 B)!\n");
        return 1;
    }
    memset(pcm_buffer_virt, 0, PCM_BUFFER_SIZE_BYTES);
    uint32_t pcm_buffer_phys = (uint32_t)MmGetPhysicalAddress(pcm_buffer_virt);
    assert(((uintptr_t)pcm_buffer_virt & 7) == 0);
    assert((pcm_buffer_phys & 7) == 0);

    debugPrint("      Voice Array: Virt=0x%p Phys=0x%08lX (4KB aligned)\n",
               voice_array_virt, (unsigned long)voice_array_phys);
    debugPrint("      PRD Table:   Virt=0x%p Phys=0x%08lX (8B aligned)\n",
               prd_table_virt, (unsigned long)prd_table_phys);
    debugPrint("      PCM Buffer:  Virt=0x%p Phys=0x%08lX (19200 B)\n\n",
               pcm_buffer_virt, (unsigned long)pcm_buffer_phys);

    /*
     * ------------------------------------------------------------------------
     * Step 2: Fill PCM Buffer with 480 Hz Stereo Sine Wave
     * ------------------------------------------------------------------------
     */
    debugPrint("[2/8] Synthesizing 480 Hz stereo sine wave (48 full cycles)...\n");
    int16_t *pcm_samples = (int16_t *)pcm_buffer_virt;
    for (uint32_t i = 0; i < NUM_STEREO_FRAMES; ++i) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double rad = 2.0 * M_PI * (double)SINE_FREQUENCY_HZ * t;
        int16_t val = (int16_t)(sin(rad) * 32767.0);

        pcm_samples[i * 2 + 0] = val; /* Front Left */
        pcm_samples[i * 2 + 1] = val; /* Front Right */
    }

    /*
     * ------------------------------------------------------------------------
     * Step 3: Build Physical Region Descriptor (PRD) Table
     * ------------------------------------------------------------------------
     */
    debugPrint("[3/8] Building PRD table entry with EOT flag...\n");
    NVAPU_PRD_ENTRY *prd_table = (NVAPU_PRD_ENTRY *)prd_table_virt;
    size_t prd_count = 0;
    int prd_err = apu_prd_build(pcm_buffer_phys, PCM_BUFFER_SIZE_BYTES, prd_table, 1, &prd_count);
    if (prd_err != 0) {
        debugPrint("FATAL: apu_prd_build failed with code %d!\n", prd_err);
        return 1;
    }
    debugPrint("      PRD [0]: Phys=0x%08lX, Size=%lu B, EOT=%s\n",
               (unsigned long)prd_table[0].physical_address,
               (unsigned long)(prd_table[0].control & 0xFFFFu),
               (prd_table[0].control & NV_PAPU_PRD_EOT) ? "YES" : "NO");

    /*
     * ------------------------------------------------------------------------
     * Step 4: Configure Output Processor (EP) for Stereo
     * ------------------------------------------------------------------------
     */
    debugPrint("[4/8] Configuring Output Processor (EP) stereo FIFO routing...\n");
    apu_write32(NV_PAPU_BASE, NV_PAPU_EP_FIFO_CONFIG, NV_PAPU_EP_FIFO_CONFIG_STEREO);
    apu_write32(NV_PAPU_BASE, NV_PAPU_EP_FIFO_ROUTE, NV_PAPU_EP_ROUTE_DEFAULT);
    apu_write32(NV_PAPU_BASE, NV_PAPU_EP_CONTROL, NV_PAPU_EP_CONTROL_ENABLE);

    /*
     * ------------------------------------------------------------------------
     * Step 5: Upload GP Microcode and Start Global Processor DSP
     * ------------------------------------------------------------------------
     */
    debugPrint("[5/8] Uploading GP microcode (5.1 passthrough) & starting DSP...\n");
    int ucode_err = apu_gp_load_microcode(NV_PAPU_BASE, gp_passthrough_51_bin, GP_PASSTHROUGH_51_SIZE);
    if (ucode_err != 0) {
        debugPrint("FATAL: apu_gp_load_microcode failed with code %d!\n", ucode_err);
        return 1;
    }
    apu_gp_start(NV_PAPU_BASE);
    debugPrint("      GP DSP status: %s\n", apu_gp_is_running(NV_PAPU_BASE) ? "RUNNING" : "HALTED");

    /*
     * ------------------------------------------------------------------------
     * Step 6: Initialize Voice Processor (VP) Subsystem
     * ------------------------------------------------------------------------
     */
    debugPrint("[6/8] Initializing Voice Processor subsystem (64 3D voice mode)...\n");
    int vp_err = apu_voice_subsystem_init(NV_PAPU_BASE, voice_array_virt, voice_array_phys);
    if (vp_err != 0) {
        debugPrint("FATAL: apu_voice_subsystem_init failed with code %d!\n", vp_err);
        return 1;
    }

    /*
     * ------------------------------------------------------------------------
     * Step 7: Prepare Voice Context 0 per Hardware Contract
     * ------------------------------------------------------------------------
     */
    debugPrint("[7/8] Configuring Voice Context 0 parameters...\n");
    NVAPU_VOICE_CONTEXT_3D voice0;
    apu_voice_context_reset(&voice0);

    voice0.format = NVAPU_VOICE_FORMAT_PCM16;           /* 0 = 16-bit signed PCM */
    voice0.channels = NVAPU_VOICE_CHANNELS_STEREO;       /* 1 = stereo */
    voice0.loop_mode = NVAPU_VOICE_LOOP_ON;             /* 1 = looping */
    voice0.mode_3d = 0;                                 /* 0 = 2D direct */
    voice0.active = 0;                                  /* 0 = inactive before trigger */
    voice0.int_on_eot = 0;
    voice0.pitch_step = APU_PITCH_STEP_UNITY;           /* 0x00010000 = 1.0x pitch at 48 kHz */
    voice0.prd_table_phys = prd_table_phys;             /* PRD table physical address */
    voice0.current_prd_index = 0;
    voice0.sample_pos_frac = 0;
    voice0.loop_start_offset = 0;
    voice0.loop_end_offset = PCM_BUFFER_SIZE_BYTES;     /* 19200 bytes */
    voice0.master_vol_left = 0xFFFF;                    /* Maximum gain left (0xFFFF) */
    voice0.master_vol_right = 0xFFFF;                   /* Maximum gain right (0xFFFF) */
    voice0.mixbin_routing_mask = 0x00000003;            /* MixBins 0 and 1 (FL and FR) */
    voice0.mixbin_gain[0] = 0xFF;                       /* FL unity gain (0xFF) */
    voice0.mixbin_gain[1] = 0xFF;                       /* FR unity gain (0xFF) */

    int setup_err = apu_voice_setup(0, &voice0);
    if (setup_err != 0) {
        debugPrint("FATAL: apu_voice_setup(0) failed with code %d!\n", setup_err);
        return 1;
    }

    /*
     * ------------------------------------------------------------------------
     * Step 8: Trigger Voice 0 Playback
     * ------------------------------------------------------------------------
     */
    debugPrint("[8/8] Triggering Voice 0 playback (NV_PAPU_VP_ACTIVE_0 bit 0)...\n\n");
    int trig_err = apu_voice_trigger(NV_PAPU_BASE, 0);
    if (trig_err != 0) {
        debugPrint("FATAL: apu_voice_trigger(0) failed with code %d!\n", trig_err);
        return 1;
    }

    debugPrint("=== Hardware Playback Active ===\n");
    debugPrint("Tone: 480 Hz Sine | Rate: 48 kHz | Channels: Stereo\n\n");

    /*
     * ------------------------------------------------------------------------
     * Diagnostics Monitor Loop
     * ------------------------------------------------------------------------
     */
    uint32_t seconds = 0;
    while (1) {
        uint32_t active0 = apu_read32(NV_PAPU_BASE, NV_PAPU_VP_ACTIVE_0);
        bool v0_active = apu_voice_is_active(NV_PAPU_BASE, 0);
        bool gp_running = apu_gp_is_running(NV_PAPU_BASE);

        debugPrint("[Elapsed: %3lu s] Voice 0: %s | ActiveMask: 0x%08lX | GP DSP: %s\n",
                   (unsigned long)seconds++,
                   v0_active ? "RUNNING" : "STOPPED",
                   (unsigned long)active0,
                   gp_running ? "RUNNING" : "HALTED");

        Sleep(1000);
    }

    return 0;
}
