#ifndef MCPX_APU_REGS_H
#define MCPX_APU_REGS_H

/*
 * MCPX APU register model, as implemented by xemu (see
 * lib/openal/docs/XEMU_VERIFICATION.md, section 4, "Corrected hardware
 * reference"). This replaces the register layout in apu_hardware.h, which
 * that report found to be wrong. Values marked UNVERIFIABLE there are not
 * known to match real silicon.
 *
 * All accesses are aligned 32-bit.
 */

#include <stdint.h>

/* PCI identity (bus 0, device 5, function 0) */
#define MCPX_APU_PCI_BUS        0u
#define MCPX_APU_PCI_SLOT       5u              /* device | (function << 5) */
#define MCPX_APU_PCI_ID         0x01B010DEu     /* device 0x01B0, vendor 0x10DE */
#define MCPX_APU_BAR0_DEFAULT   0xFE800000u     /* used if PCI config space cannot be read */
#define MCPX_APU_BAR0_SIZE      0x80000u

/* BAR0 regions */
#define MCPX_APU_METHOD_BASE    0x20000u        /* front-end method window: write = run method */
#define MCPX_APU_GPRST          0x3FFFCu        /* GP DSP reset: bit 0 RST, bit 1 DSPRST */
#define MCPX_APU_EPRST          0x5FFFCu        /* EP DSP reset */

/* Main register file */
#define MCPX_APU_ISTS           0x1000u         /* interrupt status, write-1-to-clear */
#define   MCPX_APU_ISTS_FETINTSTS   (1u << 4)   /* front end trapped */
#define   MCPX_APU_ISTS_FENINTSTS   (1u << 5)   /* notifier written */
#define   MCPX_APU_ISTS_FEVINTSTS   (1u << 6)   /* voice ended */
#define MCPX_APU_IEN            0x1004u
#define MCPX_APU_FECTL          0x1100u
#define   MCPX_APU_FECTL_FEMETHMODE 0x000000E0u /* 0 free running, 0x80 halted, 0xE0 trapped */
#define MCPX_APU_FECV           0x1110u
#define MCPX_APU_FEAV           0x1118u
#define MCPX_APU_FENADDR        0x115Cu         /* notifier array, physical */
#define MCPX_APU_FEDECMETH      0x1300u
#define MCPX_APU_FEDECPARAM     0x1304u
#define MCPX_APU_FEPIOQ         0x1340u         /* hardware only: PIO queue put 23:16, get 15:8, pending 7:0
                                                   (not modelled by xemu: reads 0 there) */
#define   MCPX_APU_FEPIOQ_PENDING   0x000000FFu
#define MCPX_APU_FETFORCE1      0x1504u
#define   MCPX_APU_FETFORCE1_SE2FE_IDLE_VOICE (1u << 15)
#define MCPX_APU_SECTL          0x2000u
#define   MCPX_APU_SECTL_XCNTMODE   0x00000018u /* 0 = off; no frames unless non-zero */
/* Frames on: XCNTMODE 1. Measured on a console (samples/apu_probe3, part 2b):
 * every value 0x08..0x0F runs frames at the full 1500/s, so bits 2:0 are not
 * needed. (An early openal_vp build that wrote 0x08 ran no frames, but that
 * build also loaded the GP image from write-combining memory, which ran stale
 * code; the 0x0F used since came from state DirectSound leaves and is no
 * longer used, see the clean-room note in XEMU_VERIFICATION.md 8.1b.) */
#define   MCPX_APU_SECTL_RUN        0x00000008u
#define MCPX_APU_XGSCNT         0x200Cu
#define MCPX_APU_VPVADDR        0x202Cu         /* voice array, physical; voice h at + h * 0x80 */
#define MCPX_APU_VPSGEADDR      0x2030u         /* SGE page table, physical */
#define MCPX_APU_VPSSLADDR      0x2034u         /* stream segment table, physical */
#define MCPX_APU_TVL2D          0x2054u         /* 2D voice list head (0xFFFF = empty) */
#define MCPX_APU_TVL3D          0x2060u
#define MCPX_APU_TVLMP          0x206Cu
#define MCPX_APU_GPSADDR        0x2040u         /* GP scratch SGE table, physical */
#define MCPX_APU_GPFADDR        0x2044u         /* GP FIFO SGE table, physical */
#define MCPX_APU_GPSMAXSGE      0x20D4u         /* highest valid scratch SGE index, inclusive */
#define MCPX_APU_GPFMAXSGE      0x20D8u         /* highest valid FIFO SGE index, inclusive */
#define MCPX_APU_GPOFBASE0      0x3024u         /* GP output FIFO 0: base, end (exclusive), current; */
#define MCPX_APU_GPOFEND0       0x3028u         /*   byte offsets into the GP FIFO linear space */
#define MCPX_APU_GPOFCUR0       0x302Cu
#define MCPX_APU_GP_XMEM        0x30000u        /* GP X memory window: X:n at + 4n */
#define MCPX_APU_GP_PMEM        0x3A000u        /* GP P memory window: P:n at + 4n */
#define MCPX_APU_LIST_END       0xFFFFu

/* Physical alignment of every table whose base goes into a register
 * (VPVADDR, VPSGEADDR, VPSSLADDR, FENADDR, GPSADDR, GPFADDR): the hardware
 * drops bits 13:0 of VPSGEADDR/VPSSLADDR (apu_probe round 1) */
#define APU_TABLE_ALIGN         0x4000u

/* Front-end methods (offsets from MCPX_APU_METHOD_BASE).
 * Never write 0x130 (GET_VOICE_POSITION), 0x180 or 0x18C: xemu asserts. */
#define MCPX_METH_SET_ANTECEDENT_VOICE  0x120u  /* handle 15:0, list 17:16 */
#define   MCPX_ANTECEDENT_LIST_2D_TOP   (1u << 16)
#define   MCPX_ANTECEDENT_LIST_3D_TOP   (2u << 16)
#define MCPX_METH_VOICE_ON              0x124u  /* handle 15:0, EF phase 27:24, EA phase 31:28 */
#define MCPX_METH_VOICE_OFF             0x128u
#define MCPX_METH_VOICE_RELEASE         0x12Cu
#define MCPX_METH_VOICE_PAUSE           0x140u  /* handle 15:0, bit 18 pause */
#define MCPX_METH_SET_CURRENT_HRTF_ENTRY 0x160u /* entry 0..127 for the SET_HRIR methods */
#define MCPX_METH_SET_HRTF_HEADROOM     0x280u  /* bits 2:0 */
#define MCPX_METH_SET_HRTF_SUBMIXES     0x2C0u  /* four 5-bit bins at 4:0, 12:8, 20:16, 28:24 */
#define MCPX_METH_SET_CURRENT_VOICE     0x2F8u
#define MCPX_METH_VOICE_LOCK            0x2FCu
#define MCPX_METH_CFG_VBIN              0x300u
#define MCPX_METH_CFG_FMT               0x304u
#define MCPX_METH_CFG_ENV0              0x308u
#define MCPX_METH_CFG_ENVA              0x30Cu
#define MCPX_METH_CFG_ENV1              0x310u
#define MCPX_METH_CFG_ENVF              0x314u
#define MCPX_METH_CFG_MISC              0x318u
#define MCPX_METH_TAR_HRTF              0x31Cu
#define MCPX_METH_TAR_VOLA              0x360u
#define MCPX_METH_TAR_VOLB              0x364u
#define MCPX_METH_TAR_VOLC              0x368u
#define MCPX_METH_LFO_ENV               0x36Cu
#define MCPX_METH_TAR_FCA               0x374u
#define MCPX_METH_TAR_FCB               0x378u
#define MCPX_METH_TAR_PITCH             0x37Cu  /* bits 31:16, signed 4.12 log2 */
#define MCPX_METH_CFG_BUF_BASE          0x3A0u  /* BA: byte offset in the SGE space */
#define MCPX_METH_CFG_BUF_LBO           0x3A4u  /* loop start, sample frames */
#define MCPX_METH_CFG_BUF_EBO           0x3DCu  /* last frame, inclusive */
#define MCPX_METH_SET_HRIR              0x400u  /* + 4k, k = 0..14: L(2k), R(2k), L(2k+1), R(2k+1) int8 */
#define MCPX_METH_SET_HRIR_X            0x43Cu  /* L30 7:0, R30 15:8, ITD s6.9 31:16 */
#define MCPX_HRTF_NONE                  0xFFFFu

/* NV_PAVS voice record (0x80 bytes). The VP reads it from RAM every frame, so
 * the CPU can build it directly (verified on hardware, apu_probe round 16). */
#define MCPX_VOICE_SIZE         0x80u
#define MCPX_VOICE_CFG_VBIN     0x00u
#define MCPX_VOICE_CFG_FMT      0x04u
#define MCPX_VOICE_CFG_ENV0     0x08u
#define MCPX_VOICE_CFG_ENVA     0x0Cu
#define MCPX_VOICE_CFG_ENV1     0x10u
#define MCPX_VOICE_CFG_ENVF     0x14u
#define MCPX_VOICE_CFG_MISC     0x18u
#define MCPX_VOICE_CFG_HRTF_TARGET 0x1Cu        /* handle 15:0 */
#define MCPX_VOICE_CUR_PSL_START 0x20u          /* BA 23:0 */
#define MCPX_VOICE_CUR_PSH_SAMPLE 0x24u         /* LBO 23:0 */
#define MCPX_VOICE_TAR_VOLA     0x60u
#define MCPX_VOICE_TAR_VOLB     0x64u
#define MCPX_VOICE_TAR_VOLC     0x68u
#define MCPX_VOICE_TAR_LFO_ENV  0x6Cu
#define MCPX_VOICE_TAR_FCA      0x74u
#define MCPX_VOICE_TAR_FCB      0x78u
#define MCPX_VOICE_PAR_STATE    0x54u
#define   MCPX_PAR_STATE_PAUSED       (1u << 18)
#define   MCPX_PAR_STATE_ACTIVE_VOICE (1u << 21)
#define MCPX_VOICE_PAR_OFFSET   0x58u           /* CBO 23:0 */
#define MCPX_VOICE_PAR_NEXT     0x5Cu           /* EBO 23:0 */
#define MCPX_VOICE_TAR_PITCH_LINK 0x7Cu         /* next handle 15:0, pitch 31:16 */

/* CFG_FMT fields */
#define MCPX_FMT_LOOP           (1u << 25)
#define MCPX_FMT_STEREO         (1u << 27)
#define MCPX_FMT_SAMPLE_S16     (1u << 28)
#define MCPX_FMT_CONTAINER_B16  (1u << 30)
#define MCPX_FMT_SPB_SHIFT      16              /* SAMPLES_PER_BLOCK: 1 for stereo PCM16 */

/* Volumes: 12-bit attenuation in 1/64 dB, 0 = unity, 0xFFF = mute */
#define MCPX_VOL_UNITY          0x000u
#define MCPX_VOL_MUTE           0xFFFu

#endif /* MCPX_APU_REGS_H */
