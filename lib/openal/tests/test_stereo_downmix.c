#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "apu_gp_ucode.h"
#include "apu_eeprom.h"
#include "apu_hardware.h"

#define MOCK_MMIO_SIZE 0x30000 /* 192 KB for BAR0 registers + PRAM @ 0x20000 */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== OpenAL DSP56300 ITU-R BS.775 Stereo Downmix Host Unit Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Validate DSP56300 ITU-R BS.775 Downmix Math Across All 6 Channels
     * ------------------------------------------------------------------------
     * Equations:
     *   Left  = FL + 0.70710678 * Center + 0.70710678 * SL
     *   Right = FR + 0.70710678 * Center + 0.70710678 * SR
     * Factor:
     *   0x5A827A in Q23 (round(0.70710678 * 8388608) = 5931642)
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing DSP56300 ITU-R BS.775 downmix math across all 6 channels...\n");

    /* Case 1a: All channels silent */
    {
        int32_t mixbins[6] = {0, 0, 0, 0, 0, 0};
        int32_t out_l = -1, out_r = -1;
        apu_dsp56k_itur_downmix(mixbins, &out_l, &out_r);
        assert(out_l == 0 && out_r == 0);
    }

    /* Case 1b: Center alone: L = 0.7071 * C, R = 0.7071 * C */
    {
        /* Test with half-scale positive value: 0x400000 (4194304) */
        int32_t c_half = 0x400000;
        int32_t mixbins_c[6] = {0, 0, 0, 0, c_half, 0};
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_c, &out_l, &out_r);
        assert(out_l == 2965821); /* round(4194304 * 0.70710678) */
        assert(out_r == 2965821);
        assert(out_l == out_r);

        /* Verify accuracy against double-precision reference */
        double expected = (double)c_half * 0.70710678;
        assert(fabs((double)out_l - expected) < 1.0);

        /* Test with negative center: -0x400000 */
        int32_t mixbins_c_neg[6] = {0, 0, 0, 0, -c_half, 0};
        apu_dsp56k_itur_downmix(mixbins_c_neg, &out_l, &out_r);
        assert(out_l == -2965821);
        assert(out_r == -2965821);

        /* Test with full scale positive: 0x7FFFFF (8388607) */
        int32_t c_max = 0x7FFFFF;
        int32_t mixbins_c_max[6] = {0, 0, 0, 0, c_max, 0};
        apu_dsp56k_itur_downmix(mixbins_c_max, &out_l, &out_r);
        assert(out_l == 5931641);
        assert(out_r == 5931641);
        assert(fabs((double)out_l - ((double)c_max * 0.70710678)) < 1.0);
    }

    /* Case 1c: FL alone: L = FL, R = 0 */
    {
        int32_t fl_val = 0x2468AC;
        int32_t mixbins_fl[6] = {fl_val, 0, 0, 0, 0, 0};
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_fl, &out_l, &out_r);
        assert(out_l == fl_val);
        assert(out_r == 0);

        /* Negative FL */
        mixbins_fl[0] = -fl_val;
        apu_dsp56k_itur_downmix(mixbins_fl, &out_l, &out_r);
        assert(out_l == -fl_val);
        assert(out_r == 0);
    }

    /* Case 1d: FR alone: L = 0, R = FR */
    {
        int32_t fr_val = 0x3579BD;
        int32_t mixbins_fr[6] = {0, fr_val, 0, 0, 0, 0};
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_fr, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == fr_val);

        /* Negative FR */
        mixbins_fr[1] = -fr_val;
        apu_dsp56k_itur_downmix(mixbins_fr, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == -fr_val);
    }

    /* Case 1e: SL alone: L = 0.7071 * SL, R = 0 */
    {
        int32_t sl_val = 0x400000;
        int32_t mixbins_sl[6] = {0, 0, sl_val, 0, 0, 0};
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_sl, &out_l, &out_r);
        assert(out_l == 2965821);
        assert(out_r == 0);

        /* Negative SL */
        mixbins_sl[2] = -sl_val;
        apu_dsp56k_itur_downmix(mixbins_sl, &out_l, &out_r);
        assert(out_l == -2965821);
        assert(out_r == 0);
    }

    /* Case 1f: SR alone: L = 0, R = 0.7071 * SR */
    {
        int32_t sr_val = 0x400000;
        int32_t mixbins_sr[6] = {0, 0, 0, sr_val, 0, 0};
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_sr, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == 2965821);

        /* Negative SR */
        mixbins_sr[3] = -sr_val;
        apu_dsp56k_itur_downmix(mixbins_sr, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == -2965821);
    }

    /* Case 1g: LFE alone: bypassed / not in stereo downmix */
    {
        int32_t mixbins_lfe[6] = {0, 0, 0, 0, 0, 0x7FFFFF};
        int32_t out_l = -1, out_r = -1;
        apu_dsp56k_itur_downmix(mixbins_lfe, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == 0);

        /* Negative LFE max */
        mixbins_lfe[5] = -0x800000;
        apu_dsp56k_itur_downmix(mixbins_lfe, &out_l, &out_r);
        assert(out_l == 0);
        assert(out_r == 0);
    }
    printf("    -> Discrete channel gain multipliers match ITU-R BS.775 [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: Saturation Mode Verification
     * ------------------------------------------------------------------------
     * DSP56300 Saturation Mode (SR bit 12 set via ORI #$001000, SR):
     * Sum > +1.0 must clamp to 0x7FFFFF (8388607) without wrapping to negative.
     * Sum < -1.0 must clamp to -0x800000 (-8388608) without wrapping to positive.
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing saturation mode verification (clipping without sign wrap)...\n");

    /* Case 2a: Multiple loud positive channels sum to > +1.0 */
    {
        /* FL = 0x7FFFFF, C = 0x7FFFFF, SL = 0x7FFFFF */
        /* FR = 0x7FFFFF, C = 0x7FFFFF, SR = 0x7FFFFF */
        int32_t mixbins_loud_pos[6] = {
            0x7FFFFF, /* FL */
            0x7FFFFF, /* FR */
            0x7FFFFF, /* SL */
            0x7FFFFF, /* SR */
            0x7FFFFF, /* C  */
            0x7FFFFF  /* LFE */
        };
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_loud_pos, &out_l, &out_r);

        /* Must saturate exactly to 24-bit maximum (0x7FFFFF = 8388607) */
        assert(out_l == 0x7FFFFF);
        assert(out_r == 0x7FFFFF);
        assert(out_l > 0 && out_r > 0); /* No integer overflow wrap to negative */
    }

    /* Case 2b: Multiple loud negative channels sum to < -1.0 */
    {
        /* FL = -0x800000, C = -0x800000, SL = -0x800000 */
        /* FR = -0x800000, C = -0x800000, SR = -0x800000 */
        int32_t mixbins_loud_neg[6] = {
            -0x800000, /* FL */
            -0x800000, /* FR */
            -0x800000, /* SL */
            -0x800000, /* SR */
            -0x800000, /* C  */
            -0x800000  /* LFE */
        };
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_loud_neg, &out_l, &out_r);

        /* Must saturate exactly to 24-bit minimum (-0x800000 = -8388608) */
        assert(out_l == -0x800000);
        assert(out_r == -0x800000);
        assert(out_l < 0 && out_r < 0); /* No integer underflow wrap to positive */
    }

    /* Case 2c: Asymmetric saturation (Left over-range, Right within range) */
    {
        int32_t mixbins_asym[6] = {
            0x7FFFFF, /* FL */
            0x100000, /* FR */
            0x7FFFFF, /* SL */
            0,        /* SR */
            0x200000, /* C  */
            0         /* LFE */
        };
        int32_t out_l = 0, out_r = 0;
        apu_dsp56k_itur_downmix(mixbins_asym, &out_l, &out_r);

        /* Left saturates to 0x7FFFFF */
        assert(out_l == 0x7FFFFF);
        /* Right = 0x100000 + 0.7071 * 0x200000 = 1048576 + 1482911 = 2531487 (no saturation) */
        int32_t expected_r = 0x100000 + apu_dsp56k_q23_mul(0x200000, (int32_t)APU_DSP56K_Q23_FACTOR_3DB);
        assert(out_r == expected_r);
        assert(out_r == 2531487);
    }
    printf("    -> 24-bit saturation mode prevents overflow and underflow wrap [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: Microcode Loading & APU MMIO Register Configuration
     * ------------------------------------------------------------------------
     * - APU_TOPOLOGY_SURROUND_51: loads gp_passthrough_51_bin (81 DWORDs),
     *   configures NV_PAPU_EP_FIFO_CONFIG = 0x3F.
     * - APU_TOPOLOGY_STEREO_20: loads gp_stereo_downmix_bin (86 DWORDs),
     *   configures NV_PAPU_EP_FIFO_CONFIG = 0x03.
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing microcode loader with simulated APU MMIO page...\n");
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);

    /* Test 3a: Load Surround 5.1 microcode */
    {
        memset(mock_mmio, 0, MOCK_MMIO_SIZE);
        int res51 = apu_gp_load_topology_microcode((uintptr_t)mock_mmio, APU_TOPOLOGY_SURROUND_51);
        assert(res51 == 0);

        /* Verify EP FIFO configuration register */
        uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
        assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_SURROUND);
        assert(ep_fifo == 0x3Fu);

        /* Verify DSP control register halted during upload */
        uint32_t gp_ctrl = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_CONTROL);
        assert(gp_ctrl == NV_PAPU_GPDSP_CONTROL_HALT);

        /* Verify PRAM contents match gp_passthrough_51_bin */
        for (size_t i = 0; i < GP_PASSTHROUGH_51_SIZE; ++i) {
            uint32_t word = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (uint32_t)(i * 4));
            assert(word == gp_passthrough_51_bin[i]);
        }

        /* Verify 5.1 loop entry point at P:$0040 (offset 64): WAIT instruction */
        uint32_t wait_op = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x40 * 4));
        assert(wait_op == 0x00000Cu);
    }

    /* Test 3b: Load Stereo 2.0 microcode */
    {
        memset(mock_mmio, 0, MOCK_MMIO_SIZE);
        int res20 = apu_gp_load_topology_microcode((uintptr_t)mock_mmio, APU_TOPOLOGY_STEREO_20);
        assert(res20 == 0);

        /* Verify EP FIFO configuration register */
        uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
        assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_STEREO);
        assert(ep_fifo == 0x03u);

        /* Verify DSP control register */
        uint32_t gp_ctrl = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_CONTROL);
        assert(gp_ctrl == NV_PAPU_GPDSP_CONTROL_HALT);

        /* Verify PRAM contents match gp_stereo_downmix_bin */
        for (size_t i = 0; i < GP_STEREO_DOWNMIX_SIZE; ++i) {
            uint32_t word = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (uint32_t)(i * 4));
            assert(word == gp_stereo_downmix_bin[i]);
        }

        /* Verify Stereo initialization at P:$0040: ORI #$001000, SR */
        uint32_t ori_op = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x40 * 4));
        uint32_t ori_imm = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x41 * 4));
        assert(ori_op == 0x0000F8u);
        assert(ori_imm == 0x001000u);

        /* Verify Q23 constant 0x5A827A at P:$0044 */
        uint32_t q23_const = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x44 * 4));
        assert(q23_const == 0x5A827Au);
    }

    /* Test 3c: Invalid topology parameter handling */
    {
        int res_invalid = apu_gp_load_topology_microcode((uintptr_t)mock_mmio, (APU_AUDIO_TOPOLOGY)0x99);
        assert(res_invalid < 0);
    }
    printf("    -> PRAM instruction binary and EP FIFO configuration verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: OpenAL ALC Device Integration & Hardware Topology Detection
     * ------------------------------------------------------------------------
     * - Mock Stereo 2.0  -> alcOpenDevice initializes APU_TOPOLOGY_STEREO_20.
     * - Mock Surround 5.1 -> alcOpenDevice initializes APU_TOPOLOGY_SURROUND_51.
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing ALC device open integration with dynamic topology detection...\n");
    alc_set_apu_base((uintptr_t)mock_mmio);

    /* Case 4a: Mock Stereo 2.0 environment */
    {
        apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);

        ALCdevice *dev_stereo = alcOpenDevice(NULL);
        assert(dev_stereo != NULL);
        assert(dev_stereo->is_open == true);
        assert(dev_stereo->topology == APU_TOPOLOGY_STEREO_20);

        /* Verify hardware EP FIFO configured for Stereo (0x03) */
        uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
        assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_STEREO);

        /* Verify stereo downmix microcode was uploaded */
        uint32_t ori_op = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x40 * 4));
        assert(ori_op == 0x0000F8u);

        /* Query ALC_XBOX_TOPOLOGY */
        ALCint topo_val = -1;
        alcGetIntegerv(dev_stereo, ALC_XBOX_TOPOLOGY, 1, &topo_val);
        assert(topo_val == (ALCint)APU_TOPOLOGY_STEREO_20);

        /* Close device cleanly */
        ALCboolean closed = alcCloseDevice(dev_stereo);
        assert(closed == ALC_TRUE);
    }

    /* Case 4b: Mock Surround 5.1 environment (HDTV Pack + AC-3 enabled) */
    {
        apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO | XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);

        ALCdevice *dev_surround = alcOpenDevice(NULL);
        assert(dev_surround != NULL);
        assert(dev_surround->is_open == true);
        assert(dev_surround->topology == APU_TOPOLOGY_SURROUND_51);

        /* Verify hardware EP FIFO configured for 5.1 Surround (0x3F) */
        uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
        assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_SURROUND);

        /* Verify 5.1 passthrough microcode was uploaded */
        uint32_t wait_op = apu_read32((uintptr_t)mock_mmio, NV_PAPU_GPDSP_PRAM + (0x40 * 4));
        assert(wait_op == 0x00000Cu);

        /* Query ALC_XBOX_TOPOLOGY */
        ALCint topo_val = -1;
        alcGetIntegerv(dev_surround, ALC_XBOX_TOPOLOGY, 1, &topo_val);
        assert(topo_val == (ALCint)APU_TOPOLOGY_SURROUND_51);

        /* Close device cleanly */
        ALCboolean closed = alcCloseDevice(dev_surround);
        assert(closed == ALC_TRUE);
    }

    /* Case 4c: Restore unmocked state (defaults to Stereo 2.0 on host) */
    {
        apu_eeprom_clear_mock();

        ALCdevice *dev_default = alcOpenDevice(NULL);
        assert(dev_default != NULL);
        assert(dev_default->topology == APU_TOPOLOGY_STEREO_20);

        uint32_t ep_fifo = apu_read32((uintptr_t)mock_mmio, NV_PAPU_EP_FIFO_CONFIG);
        assert(ep_fifo == NV_PAPU_EP_FIFO_CONFIG_STEREO);

        ALCboolean closed = alcCloseDevice(dev_default);
        assert(closed == ALC_TRUE);
    }
    printf("    -> alcOpenDevice dynamically resolves and configures target topology [PASS]\n");

    /* Cleanup */
    alc_set_apu_base(0);
    free(mock_mmio);

    printf("=== All DSP56300 ITU-R BS.775 Stereo Downmix Tests Passed Successfully! ===\n");
    return 0;
}
