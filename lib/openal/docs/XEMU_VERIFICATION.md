# xemu verification of the nxdk-ex OpenAL / MCPX APU layer

| | |
|---|---|
| Subject (audited) | `lib/openal` at commit `639dcdd` (nxdk-ex master), called "the base" below |
| This branch | the base plus the code, documentation and test-infrastructure fixes listed in section 2.4 |
| Reference | xemu at commit `478b4f4` (`hw/xbox/mcpx/apu/**` and related files) |
| Method | static reading of the xemu source; xemu itself was never built or run (the repository measurements in 1.2 are host runs of this repository's tests) |
| Written | 2026-10-07 |
| Companion | `lib/openal/README.md` (what the library is and is not) |

xemu references are written `path:lines` relative to the xemu repository root, for example
`hw/xbox/mcpx/apu/vp/vp.c:1387-1412`, always at commit 478b4f4. Repository references are
`path:lines` at commit 639dcdd unless a row says otherwise; line numbers drift as the tree changes.

**Two statuses are kept apart.** *Status at base 639dcdd* is what the audit found: sections 1 to 3, the
"consequence" text in section 5 and every measurement quoted here describe the base, and the verdict tables
are kept as the audit record; rows that were corrected or annotated afterwards say so (base versus branch). *Status in this branch* is given in section 2.4 (what this
series fixed), section 2.5 (the status of each finding: fixed, mitigated by the default null backend, or still
open) and the status column of section 5. Section 4 (the corrected hardware reference) and sections 7 and 8
(porting plan, roadmap) describe xemu and the way forward, and section 6 lists open questions; none of them
depends on the library's state.

Contents: 1 Purpose, method and caveats - 2 Executive summary (2.4 What this series fixed, 2.5 Status of the
findings) - 3 Verdict tables per cluster (audit result at 639dcdd) - 4 Corrected hardware reference -
5 What was wrong in the library, by severity - 6 UNVERIFIABLE items and assumptions - 7 Porting plan -
8 Roadmap - 9 Appendix: corrections to the initial review.

---

## 1. Purpose, method and caveats

### 1.1 Purpose

`lib/openal` presents itself as a hardware-accelerated OpenAL 1.1 implementation for the MCPX audio
processing unit (APU). xemu is the reference emulator for that APU. This document records how the
library's register map, voice model, DSP microcode and README claims compare with the APU as xemu
implements it, so that maintainers can decide what to keep, fix or remove. It also serves as a
corrected hardware reference (section 4) and as a bring-up plan (section 7).

### 1.2 Method

* **Source reading.** The xemu files read for this document are `hw/xbox/mcpx/apu/` (`apu.c`,
  `apu_regs.h`, `apu_int.h`, `monitor.c`, `vp/vp.c`, `vp/vp.h`, `vp/hrtf.h`, `vp/adpcm.h`,
  `fpconv.h`, `dsp/gp_ep.c`, `dsp/dsp.c`, `dsp/dsp_c.c`, `dsp/dsp_dma.c`,
  `dsp/interp/dsp_cpu.c`, `dsp/interp/dsp_emu.c.inc`, `dsp/interp/dsp_cpu_regs.h`), `hw/xbox/mcpx/aci.c`,
  `hw/audio/ac97.c`, `hw/audio/ac97_int.h`, `hw/xbox/xbox.c`, `hw/xbox/xbox_pci.c`,
  `hw/xbox/smbus_xbox_smc.c`, `hw/xbox/amd_smbus.c`, `hw/xbox/eeprom_generation.c/.h`,
  `config_spec.yml`, `include/qemu/osdep.h` and `ui/xui/debug.cc`, `ui/xui/main-menu.cc`.
* **Opcode decoding.** Words from `lib/openal/src/apu_gp_ucode.h` were matched against the 24-bit
  template table in `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c:152-340` with a throwaway script that parses
  that table and applies the same first-match rule as `lookup_opcode_slow` (`dsp_cpu.c:409-421`).
  Words at or above 0x100000 go through the parallel-move path (`dsp_cpu.c:622-637`), whose bit fields
  were read from `emu_pm_2`, `emu_pm_2_2` and `emu_pm_5` in `dsp_emu.c.inc`. No xemu code was run.
* **Two earlier passes.** Eight per-cluster verification passes were each followed by a skeptical
  audit. This document does not adopt their conclusions on trust: every row in section 3 was
  re-derived by re-reading the cited xemu lines, and several earlier statements changed as a result
  (section 1.5).
* **Repository measurements** (line coverage, API reference counts) were reproduced at 639dcdd:
  `gcc --coverage -O0` over `lib/openal/src` with all host tests gives 2,088 of 2,706 lines (77.2 %) at
  639dcdd; 94 public AL/ALC/ALX functions are declared at 639dcdd, none is unreferenced by tests, and 40 are
  referenced only by the link/smoke test `tests/test_lib_build.c` (at 639dcdd). The same method on this branch
  gives 2,210 of 2,768 lines (79.8 %) and 95 declared functions (`alXboxUpdateVoices` was added). Wherever
  this document quotes 94 functions, 2,706 lines or 77.2 %, those are the base figures.
* **Licensing.** xemu is GPL-2.0. This document states facts (offsets, bit positions, behaviours) and
  paraphrases everything; no xemu source text is copied.

### 1.3 How to read the verdicts

A verdict describes the **repository's claim or assumption** (the subject of the row) against xemu.
Earlier passes mixed polarities ("refuted" sometimes meant "the reviewer's criticism was refuted, the
repo is fine"); that is normalised here.

| Verdict | Meaning |
|---|---|
| CORRECT | xemu agrees with the repository |
| WRONG | xemu implements something incompatible, or contradicts the claim |
| PARTIAL | part of the claim agrees, part does not (the row says which) |
| NOT IN XEMU | xemu has no such feature, so the claim can be neither exercised nor validated there |
| UNVERIFIABLE | xemu is silent or says it is guessing; nothing else here can settle it |

Confidence (high / medium / low) is how sure the author is about the *reading of the source*, not
about real silicon.

### 1.4 Caveats

* **xemu is a reverse-engineered model**, not hardware documentation. It carries FIXMEs on exactly the
  points that matter here, for example the current-buffer-offset reset in VOICE_ON
  (`vp/vp.c:216`), the SGE table size and sharing (`vp/vp.c:443-444`, `476-477`), ADPCM samples per
  block, 64 versus 65 (`apu_regs.h:336`), and the HRTF parameter smoothing (`vp/hrtf.h:85`). Claims
  about real silicon are marked UNVERIFIABLE wherever xemu is silent.
* **Asserts are always live.** `include/qemu/osdep.h:311-316` rejects builds with `NDEBUG` or
  `G_DISABLE_ASSERT`. An xemu assertion aborts the whole emulator, not just the guest.
* **DSP backend.** Only the in-tree C interpreter was read. The optional JIT backend
  (`audio.use_dsp_jit`, default false, `config_spec.yml:317-319`) lives in an external library that is
  not part of the checkout; its behaviour (for example for WAIT/STOP) is UNVERIFIABLE here.
* **Defaults.** `audio.use_dsp` has no explicit default in `config_spec.yml:316`; it is presumably
  false (inference; the config generator is not in the checkout). `audio.hrtf` defaults to true
  (`config_spec.yml:320-322`).
* **Implementation quirks are not hardware evidence.** Examples: a voice that ends mid-frame loses that
  whole frame (`vp/vp.c:1356-1360`); the end-of-buffer test `cbo >= ebo` (`vp/vp.c:1093`) can skip the
  last sample when a 32-sample request ends exactly on it; `NEW_VOICE` is never set; the per-voice
  headroom field is never read (`apu_regs.h:231`).
* **Static analysis only.** Consequences such as "xemu aborts" are derived from the code path; they were
  not observed.

### 1.5 Corrections made while writing this document

The following statements in the earlier review output were wrong or overstated and are corrected here:

1. **Pitch constant.** For a 44.1 kHz source the TAR_PITCH value is `-501` (exact `-500.76`), i.e.
   argument `0xFE0B0000`. An earlier step list gave `-493` / `0xFE130000`, which decodes to roughly
   44,158 Hz. Other values: 22.05 kHz `-4597` (`0xEE0B0000`), 32 kHz `-2396` (`0xF6A40000`),
   8 kHz `-10588` (`0xD6A40000`), 24 kHz `-4096` (`0xF0000000`), 96 kHz `+4096` (`0x10000000`).
2. **TVL/CVL/NVL are not volumes.** `NV_PAPU_TVL2D/CVL2D/NVL2D`, `TVL3D/CVL3D/NVL3D`,
   `TVLMP/CVLMP/NVLMP` (0x2054-0x2074) are the top / current / next pointers of the three voice lists
   (`vp/vp.c:25-31`, `1809-1841`). xemu has no volume ramp (section 3, C4.4).
3. **DSP DMA format 0.** Format 0 ("8 bit") is accepted by the format switch but asserts at transfer
   time in every path (`dsp/dsp_dma.c:169-172`, `237-238`, `253-254`, `301-303`); format 3 asserts at
   decode (`dsp_dma.c:182-185`). Only formats 1 (16 bit), 2 and 6 (24 bit) are usable.
4. **ITD buffer sizing was overstated.** The repository header's 256 bytes per voice equals two ears x
   64 samples x 2 bytes, which is self-consistent. The real defect is smaller: the Woodworth formula
   never exceeds 31 samples, so the clamp at 64 is unreachable, and neither number matches xemu's
   +/-42 samples (`vp/hrtf.h:31`, `62-63`).
5. **"XC_AUDIO macro redefined" is a warning, not an error.** It is `-Wmacro-redefined`: at 639dcdd the
   fallback in `apu_eeprom.h` (`0x0009u`) was spelled differently from the kernel header's `0x0009`
   (`lib/xboxkrnl/xboxkrnl.h:4228`). It becomes an error only under `-Werror`. At 639dcdd the nxdk compile path
   (`bin/nxdk-cc:20`) passed only the unrelated `-Wno-builtin-macro-redefined`, and the only `-Werror` in the
   tree was the standalone host build, where the guarded code is not compiled at all. This branch fixes the
   redefinition (identical spelling, kernel header included first) and adds `tests/check_target_syntax.sh`,
   which type-checks the target-guarded code with `-Wall -Wextra -Werror` (default flags at line 127).
6. **GPRST fix hint.** "Release GPRST first and write P afterwards" does not avoid the bootstrap: the
   transition to both reset bits set always runs the scratch-memory bootstrap
   (`dsp/gp_ep.c:251-260`), which asserts without a valid scratch SGE table. Working orders are in
   section 7, step 14.
7. **0x3000/0x3004/0x3010 and the GP FIFO block.** The GP FIFO registers start at 0x3024
   (`apu_regs.h:80`); 0x3000-0x3023 are undefined plain-storage cells.
8. **"The voice context holds no filter coefficients"** is true only for HRTF/biquad coefficients.
   `TAR_FCA/TAR_FCB` (voice offsets 0x74/0x78) hold the state-variable filter cutoff and Q
   (`apu_regs.h:313-316`, `vp/vp.c:1441-1448`).
9. **ISTS is write-1-to-clear**, not plain storage (`apu.c:74-79`); only IEN and the unnamed offsets are
   plain stores.
10. **Memory-based statements dropped.** Byte-compatibility of OpenAL IMA4 with Xbox ADPCM, a
    DirectSound mixbin order, the XC_AUDIO flag encoding and the set of AV packs that carry a TOSLink
    connector are not evidenced by xemu or by this repository and are listed as UNVERIFIABLE (section 6).
    A "kernel AV-pack numbering" *for the raw SMC register* is refuted by xemu (the SMC returns different
    codes, C6.2). The *kernel's own* `AV_PACK_*` numbering is defined in-tree
    (`lib/xboxkrnl/xboxkrnl.h:58-65`, used by `lib/hal/video.c:78-136,227,254` through
    `XVideoGetEncoderSettings()`); "kernel-style enum" in this document means that enum. A kernel-numbered
    source for the pack therefore already exists in-tree (`XVideoGetEncoderSettings()` masked with
    `VIDEO_ADAPTER_MASK`, as `lib/hal/video.c:227` does); this branch instead decodes the raw SMC code.

---

## 2. Executive summary

Sections 2.1 to 2.3 are the audit result at 639dcdd; 2.4 and 2.5 give the status in this branch.

### 2.1 What is right

* **PCI identity.** The APU is PCI bus 0, device 5, function 0, vendor 0x10DE, device 0x01B0, with a
  0x80000-byte memory BAR0 (`hw/xbox/xbox.c:331`, `hw/xbox/mcpx/apu/apu.c:392-393`, `407`, `583-586`).
* **HRTF lives in the voice processor**, not on the GP DSP (`vp/vp.c:1458-1465`), although in a different
  form than the library models it.
* **Sample data paths.** 16-bit PCM is signed little-endian, stereo is interleaved L,R and mono is
  duplicated to both channels (`vp/vp.c:1059-1090`). 8-bit PCM is **unsigned** (`vp/vp.c:1063-1066`,
  `fpconv.h:31-34`), so the library's unchanged copy of OpenAL 8-bit data is correct; only a header
  comment said "signed".
* **DSP word packing.** One 24-bit DSP word per 32-bit register, top byte zero (`dsp/gp_ep.c:335-340`,
  `dsp/interp/dsp_cpu.c:936-961`). `NOP` (0x000000) and the long `JMP` pair (0x0AF080 + address) decode
  as commented.
* **Voice record size.** 128 bytes per voice (`apu_regs.h:220`).
* **Fixed 48 kHz mix domain** with 32-sample frames and a per-voice sample-rate converter
  (`apu_regs.h:333`, `vp/vp.c:1157-1194`).
* **The SMBus AV-pack read** (slave 0x20, register 0x04, byte read) addresses the right register
  (`hw/xbox/smbus_xbox_smc.c:66`, `hw/xbox/xbox.c:300`, `hw/xbox/amd_smbus.c:85-87`), and
  `ExQueryNonVolatileSetting(XC_AUDIO, ...)` is used consistently with the nxdk header.
* **The BS.775 downmix arithmetic** (1.0 front, 0.7071 centre and surround, LFE dropped, Q23 constant
  0x5A827A, 24-bit saturation) is sound as host C and tested; it is not a hardware claim.

### 2.2 What was wrong at 639dcdd

The audit result at the base. The last sentence of each item gives its status in this branch (2.4 and 2.5).

* **Register map.** `apu_hardware.h` is a private model: 0x1000/0x1004 are ISTS/IEN, 0x1010-0x1024 and
  0x2004 are plain-storage cells with no function in xemu, 0x2000 is SECTL (the frame-engine gate),
  `BAR0+0x20000` is the voice front-end method window (not GP program RAM), 0x3000-0x3010 are not EP
  registers, and there is no 64-3D-or-256-2D mode. *Still open; the header banner says so, and the default
  backend never writes these registers to an emulated or real APU.*
* **Voice model.** The 128-byte `NVAPU_VOICE_CONTEXT_3D` is not the NV_PAVS record; voices are started,
  stopped and paused by front-end methods and must be linked into one of three voice lists. *Still open
  (banner in `apu_voice.h`).*
* **Encodings.** Pitch is a signed 4.12 log2 field (not a linear 16.16 step); volumes are 12-bit
  attenuations in 1/64 dB where **0 is unity and 0xFFF is mute** (the library uses linear gains with 0 =
  silence); each voice has exactly eight (mixbin, attenuation) outputs. *Still open.*
* **Buffer addressing.** One global SGE page table; `BA` is a 24-bit **byte** offset into the linear space
  and `CBO`/`LBO`/`EBO` are 24-bit sample-frame indices; not per-buffer PRD lists with byte offsets and an
  EOT bit. *Still open; the PRD chunk-size bug found on the way is fixed.*
* **Voice position and completion.** Position is the sample index CBO in the voice record; completion is
  `ACTIVE_VOICE` in the voice record, a notifier byte at `FENADDR`, ISTS bits and the idle-voice trap.
  The library's `VP_ACTIVE` bitmasks never clear in xemu. *Still open: on the null backend nothing completes
  by itself either (README, "Per-frame update").*
* **GP microcode.** The two listings do not decode as commented (`0x00000C` is RTS, `0x0E4000`-class
  words are conditional jumps, `0x001000` is not an opcode), use a wrong address map (mixbins are at
  `X:0x1400 + 32*bin`, Y RAM has 2048 words) and never end a frame by writing bit 0 to `X:$FFFFC4`.
  *Still open (banner in `apu_gp_ucode.h`); `alcOpenDevice` still uploads them, into a RAM stand-in by
  default.*
* **EP / Dolby.** xemu has no EP-enable, Dolby/DSE, AC-3 or S/PDIF encoder model. The EP is a second
  programmable DSP with 4 output and 2 input FIFOs. *Still open; the README and the showcase text no longer
  claim an encoder.*
* **HRTF form.** 31-tap FIR per ear plus one ITD (+/-42 samples) from a 128-entry table loaded through
  methods, for voice handles below 64 only; not a Q14 biquad plus a RAM delay buffer. *Still open.*
* **AV-pack decoding.** The raw SMC code was compared against a kernel-style enum (the kernel's `AV_PACK_*`
  numbering, see 1.5 item 10); under xemu's default HDTV pack (raw 1) the library saw "standard composite".
  *Fixed: `apu_av_pack_from_smc()` translates the raw code.*
* **Build guards.** At 639dcdd the hardware branches in `apu_eeprom.c` and `apu_mem.c` were guarded by
  `__NXDK__`/`_XBOX`, which `bin/nxdk-cc` never defines (it passes only `-DNXDK`); on nxdk builds they were
  compiled out. *Fixed: `apu_platform.h` selects them for `NXDK` as well.*
* **README claims.** "Zero CPU mixing overhead", "click-free preemption", the "Verified Performance"
  table, "100 % coverage", hardware Dolby Digital encoding and OpenAL 1.1 streaming were not supported
  (section 3, cluster C7). *Fixed: the README was rewritten and the sample and benchmark text corrected.*

### 2.3 What happens at 639dcdd / with -DOPENAL_APU_REAL_MMIO

Derived by reading `alcOpenDevice()` at 639dcdd (`lib/openal/src/alc_context.c:96-181` at that commit)
against the xemu code; not executed. It describes the base library and, apart from step 1, this branch built
with `-DOPENAL_APU_REAL_MMIO`. With this branch's default null backend none of the accesses below reaches an
emulated or real APU: they land in a zeroed RAM stand-in (2.4, fix 5). With the default base `0xFE800000`:

1. Host-side pool and voice-array allocation. At 639dcdd the `__NXDK__`/`_XBOX` guard was never true under
   `nxdk-cc`, so `apu_mem.c` used its host fallback: a `malloc`ed pool with a fake physical base
   `0x01000000` (`apu_mem.c:76-83`), so the "physical" addresses handed to the APU were not physical. In this
   branch the guard is fixed and a target build allocates contiguous kernel memory.
2. `apu_ep_subsystem_init`: three stores to 0x3000/0x3004/0x3010, which are plain storage in xemu
   (`apu.c:92-96`); no effect.
3. `apu_gp_load_microcode`: writes `SECTL = 0` (frames stay off), then 81 (5.1) or 86 (stereo) words to
   `BAR0 + 0x20000 + 4*i`. Those addresses are front-end **methods** (`vp/vp.c:595-660`). Word 72 lands on
   `0x120` (SET_ANTECEDENT_VOICE), word 73 on `0x124` (VOICE_ON), 74 on VOICE_OFF, 75 on VOICE_RELEASE and
   word 76 on `0x130` (GET_VOICE_POSITION).
   * 5.1 image: word 73 is `0x0E4000`, i.e. VOICE_ON with handle `0x4000`, which fails
     `assert(v < 256)` in `is_voice_locked` (`vp/vp.c:150-155`, `186`). xemu aborts.
   * Stereo image: word 72 (`0x217000`) sets the antecedent to "top of the 2D list", word 73 is VOICE_ON for
     handle 24 (which also makes handle 24 the head of the 2D list), word 74 VOICE_OFF for handle `0x4000`,
     word 75 VOICE_RELEASE for handle 8. With `VPVADDR = 0` and `FENADDR = 0` these read and write guest
     physical memory near `0x400` (voice 8), `0xC00` (voice 24), `0x200000` (voice `0x4000`) and
     `0x10002E`-`0x10002F` (its notifier bytes) (`vp/vp.c:33-52`, `100-132`, `274-307`). Word 76 then executes
     `assert(0)` (`vp/vp.c:651-656`). xemu aborts after corrupting guest RAM.
   Which image is chosen depends on `apu_detect_audio_topology()`. At 639dcdd the hardware queries were
   compiled out (build guard), so it was always the stereo one. In this branch the queries are live on the
   target; with xemu's defaults (HDTV pack, an EEPROM whose audio dword xemu leaves zero, so the AC3 flag is
   clear) the stereo image is the expected choice (inference).
4. (not reached) `SECTL = 1` would not start the engine: only bits 4:3 (`XCNTMODE`, mask `0x18`) count
   (`apu.c:270-296`).
5. (not reached) `apu_voice_subsystem_init` writes the voice-array address into `0x1004` (IEN, so all APU
   interrupts end up masked), `0x6` into `0x1000` (ISTS, clears only undefined bits) and zeros into
   `0x1010-0x1024` (plain storage, no function) (`apu.c:26-44`, `74-79`, `92-96`). `VPVADDR` stays 0.

At 639dcdd no library voice code was reachable on xemu: the stock library took this path, and so did the
shipped samples that call `alcOpenDevice()` with the default base or `apu_gp_load_microcode()`
(`samples/apu_tone`, `openal_2d`, `openal_51_test`, `openal_benchmark`, `audio_3d_spatial`). In this branch
the same holds for builds with `-DOPENAL_APU_REAL_MMIO`; by default the samples run on the RAM stand-in, and
`samples/apu_tone` refuses to run without the define. The host tests cannot detect any of this: they drive
the library against a zero-initialised memory block that acts as a mock register file.

Even with the upload fixed, the first frame would trap or abort: the three voice-list heads reset to 0
(voice 0) instead of the 0xFFFF terminator, an inactive listed voice raises `SE2FE_IDLE_VOICE`, which
asserts unless `FETFORCE1` bit 15 is set (`vp/vp.c:559-570`, `1809-1841`), and `FENADDR = 0` makes every
voice-end notifier write into low guest RAM (`vp/vp.c:33-52`).

### 2.4 What this series fixed

The commits `d3002ab`, `3b6d4de`, `6ecefde` and `3536292` on top of 639dcdd, plus the documentation and
test-infrastructure change this file belongs to. None of it makes the register layer xemu-conformant; 2.5
lists what is still open. Fixes marked *library-internal* were found while auditing and are not xemu findings.

| # | Fix | Where | Findings |
|---|---|---|---|
| 1 | **Target guard.** `apu_platform.h` defines `OPENAL_TARGET_XBOX` for `NXDK`, `__NXDK__` or `_XBOX`; `apu_mem.c` and `apu_eeprom.c` select the kernel paths (contiguous memory, SMBus, EEPROM) with it, so real nxdk builds no longer take the host fallbacks. The showcase's `main.c` and `showcase_ui.c` had the same dead guard and were fixed too. The XC_AUDIO fallback now has the kernel header's spelling. `tests/check_target_syntax.sh` type-checks the target branches against the real nxdk headers (they are never compiled by the host tests; not run on hardware or xemu) | `apu_platform.h`, `apu_mem.c`, `apu_eeprom.c`, `apu_eeprom.h`, `samples/openal_showcase` | C6.A1, C6.A2 |
| 2 | **PRD chunk size** 65535 -> 0xF000 bytes: an odd chunk size started every later chunk at an odd address and split 16-bit samples. *Library-internal* | `apu_mem.h` | consequence column of section 5, buffer row |
| 3 | **Allocator.** `apu_mem_alloc_phys` wrote a chunk header outside the free chunk for alignments of 64 or more (heap corruption, endless free-list walk); the fit is now checked first, the 32-bit size mask is fixed and the host fallback honours the alignment. The ITD pool (128) and the voice table (4096) used it. *Library-internal* | `apu_mem.c` | - |
| 4 | **AV-pack decode.** `apu_av_pack_from_smc()` translates the raw SMC codes (SCART 0, HDTV 1, VGA 2, RFU 3, S-Video 4, composite 6, none 7) to the library enum on the target path (the value is masked to its low byte); `AL_XBOX_AV_PACK_COMPOSITE` 0x05 -> 0x06 so the tokens equal the raw SMC codes; `XC_AUDIO_FLAGS_SURROUND` 0x40000 -> 0x2 (XDK convention, still UNVERIFIABLE) | `apu_eeprom.c`, `apu_eeprom.h`, `alext.h` | C6.2, C6.2b, C6.A3, C6.A4, C5.A5 |
| 5 | **Null backend by default, real MMIO as an opt-in.** Without `-DOPENAL_APU_REAL_MMIO` the device runs on a zeroed BAR0-sized RAM stand-in and no real APU register is touched; `AL_XBOX_BACKEND` and `alGetString(AL_RENDERER)` report the backend; `samples/apu_tone` is gated by the same define. `AL_XBOX_VP_BASE_PHYS` and `AL_XBOX_DOLBY_DIGITAL_ACTIVE` answer from cached software state instead of reading registers back | `alc_context.c`, `alext.h`, `samples/*` | C1.3c, C1.A1, C1.A6, C1.A7, C1.A8, C1.A9, C8.2 |
| 6 | **Extension string.** `AL_EXT_MCFORMATS` is no longer advertised (`alBufferData` rejects the formats); the list is `AL_XBOX_hardware_status AL_XBOX_update` and `alIsExtensionPresent` agrees (checked by `test_lib_build`) | `alc_context.c`, `alext.h` | C7.A1 |
| 7 | **Voice tick.** `alXboxUpdateVoices()` (also run by `alcProcessContext`) is the public entry point of the voice-manager tick that reaps finished one-shots and promotes standby sources; before, nothing outside the tests called it | `alc_context.c`, `alext.h` | C1.A2 (partly: see 2.5) |
| 8 | **One spatial computation per update.** `alSource3f(AL_POSITION)` ran the full spatial computation five times (gain, pitch, ITD, HRTF, mixbins); it runs once and applies every field (bit-identical results). The standby-promotion loop computes each candidate's priority once per update. *Library-internal* | `al_source.c`, `apu_voice_mgr.c` | - |
| 9 | **Paused updates.** Paused sources were dropped from every update, so a resume used stale gain, pan and pitch; they are now updated | `al_source.c` | - |
| 10 | **Replay restart.** `alSourcePlay` on a playing source halts the voice first and restarts from the beginning (it used to rewrite the live voice context without halting) | `al_source.c` | C2.A4 (partly: see 2.5) |
| 11 | **Voice-steal ties.** With equal priorities a newcomer now evicts the oldest holder (`play_seq`) instead of never stealing; a silent source never evicts an audible one | `apu_voice_mgr.c` | - |
| 12 | **Test infrastructure.** `run_tests.py` links libm and keeps its build in a temporary directory; `make test` works on the host (it builds the library itself instead of through the `-m32` standalone build, the phony test targets that disabled the implicit rule are gone, and the hard-coded test count is replaced by the real one), builds into `tests/build` and derives its test list from `tests/test_*.c`; `.gitignore`; new tests `test_mem_alloc`, `test_av_pack_decode`, `test_backend_guard`, `test_xbox_update`, `test_source_regressions`, `test_voice_priority` and PRD invariants in `test_buffer_prd` | `lib/openal/tests/` | C7.5, C7.A5 |
| 13 | **Honest claims.** README rewritten (scope, backends, limitations); banners in `apu_hardware.h`, `apu_voice.h` and `apu_gp_ucode.h` (comments only); "click-free" and "0.0 % CPU" text removed from comments and the showcase; showcase and benchmark UI text and READMEs reworded; the benchmark test labels say what is measured; the unused benchmark macros are documented | `README.md`, `src/*.h`, `samples/*`, `tests/test_benchmark.c` | C7.1-C7.4c, C7.A2-C7.A4, C4.4c |

### 2.5 Status of the findings in this branch

Sections 3 and 5 keep the audit result at 639dcdd; this table gives the status of each finding in this
branch. **Every finding not listed under "fixed" or "mitigated" is still open.** In particular the defects
themselves (as opposed to their consequences) are unchanged: the register map, the voice record, the pitch,
volume and routing encodings, buffer addressing and formats, the HRTF form, the GP/EP microcode and the
Dolby/AC-3 claims. "Mitigated" means that by default nothing reaches an emulated or real APU, not that the
register model became correct.

| Status | Findings | What changed |
|---|---|---|
| **Fixed** | C6.A1 | Target guard (fix 1) |
| **Fixed** | C6.A2 | Same spelling as the kernel header, kernel header included first (fix 1) |
| **Fixed** | C6.2, C6.2b, C5.A5, C6.A3 | Raw SMC codes are decoded (`apu_eeprom.c`, `apu_av_pack_from_smc`); the token values equal the raw codes; `test_av_pack_decode` covers the decode. The tests' mocks still inject decoded identifiers by design |
| **Fixed** (partly) | C6.A4 | The raw value is masked to its low byte; on an SMBus failure the library still assumes a standard (composite) pack, by design |
| **Fixed** | C1.A6 | The two status queries answer from cached software state (fix 5) |
| **Fixed** | C7.A1 | Extension string (fix 6) |
| **Fixed** | C7.A2, C7.1, C7.1b, C7.1c, C7.2, C7.2b, C7.3, C7.4a, C7.4b, C7.4c, C4.4c (claim text) | README, comments, benchmark and showcase text (fix 13). The mute-then-halt preemption itself is unchanged: an abrupt cut, and xemu has no volume ramp. Stubs (queueing, offsets, capture) remain and are listed in the README |
| **Fixed** | C7.5, C7.A5 | Test infrastructure (fix 12) |
| **Fixed** (documented) | C7.A3, C7.A4 | The unmeasured numbers are gone from the README; the benchmark's own limits (harness `sinf`/`cosf`, mock voice-context writes, differing frame counts and sleeps) are stated in the README and the test |
| **Fixed** (partly) | C3.4 / buffer row of section 5 | Chunk size 0xF000 so 16-bit samples are not split; the PRD model itself still has no xemu counterpart |
| **Mitigated** by the default null backend | C1.3c, C1.A1, C8.2 | The microcode upload lands in the RAM stand-in, so xemu is not aborted; with `-DOPENAL_APU_REAL_MMIO` it still is |
| **Mitigated** by the default null backend | C1.2b, C1.6, C1.A7, C1.A8 | The init writes, the missing `FENADDR` and the first-frame chain cannot disturb an emulated or real APU by default; the defects remain in the opt-in path |
| **Mitigated** by the default null backend | C1.A9 | The shipped samples open the device on the RAM stand-in; `samples/apu_tone` refuses to run without the define |
| **Open** (partly addressed) | C1.A2, C2.A5, C2.6, C8.8 | `alXboxUpdateVoices()` is public now, but completion is still not observable: on the null backend no voice finishes by itself, and the real registers would not report it either |
| **Open** (partly addressed) | C2.A4 | `alSourcePlay` halts a playing voice before restarting it, but the library still has no voice-list model; a second VOICE_ON on a linked voice would still create a cycle |
| **Open** | everything else | Register map, voice record, encodings, buffers, HRTF form, GP/EP microcode, Dolby: the audit rows stand as written in section 3 |

---
## 3. Verdict tables per cluster

These tables are the audit result at base 639dcdd and are kept as the historical record; the status of each
finding in this branch is in section 2.5. Eight clusters were reviewed (C1 register map, C2 voice control, C3
formats and buffers, C4 spatial mixing, C5 GP/EP DSP, C6 AV pack and EEPROM, C7 README and benchmarks, C8
bring-up). Each table lists the
claims in the original review's numbering (so findings can be traced), restated as *the repository's
claim or assumption*; verdict vocabulary is in section 1.3. A second table per cluster lists additional
findings with a severity. "Repo ref" lines refer to commit 639dcdd (note: `apu_hardware.h`, `apu_voice.h`
and `apu_gp_ucode.h` received long banner comments afterwards, so their line numbers here are those of
639dcdd). "xemu ref" lines are at 478b4f4; `apu/` abbreviates `hw/xbox/mcpx/apu/`.

### C1 - MMIO register map and BAR layout

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C1.1 | PCI identity bus 0 dev 5 fn 0, 10DE:01B0, BAR0 default 0xFE800000 | CORRECT (identity); BAR0 base UNVERIFIABLE | high | `apu_hardware.h:15-17,30-49` | `hw/xbox/xbox.c:331`; `apu/apu.c:392-393,407,583-586`; `hw/xbox/xbox_pci.c:125` (INTA to IRQ 5). The BAR address is assigned by the BIOS; no literal in xemu |
| C1.2 | 0x1000 = VP control, 0x1004 = voice-array base, 0x1010/0x1014 = active masks, 0x1020/0x1024 = pause masks | WRONG | high | `apu_hardware.h:62-67`; `apu_voice.c:39-54,115-207` | `apu/apu_regs.h:27-32` (ISTS, IEN); `apu/apu.c:46-64,66-98` (rest is plain storage); `apu/vp/vp.c:100-132` (voice at VPVADDR+h*0x80, `ACTIVE_VOICE` in the record) |
| C1.2b | The init writes (address to 0x1004, bits to 0x1000, zeros to 0x1010-0x1024) configure the VP | WRONG | high | `apu_voice.c:39,47-54,65-71` | `apu/apu.c:26-44` (IRQ needs IEN bit 0), `74-79` (ISTS is write-1-to-clear), `92-96`, `320-330` (`VPVADDR` stays 0). The aligned address in IEN masks all interrupts |
| C1.3 | 0x2000 = GP run/halt (bit 0), frame IRQ (bit 2); 0x2004 = GP status | WRONG | high | `apu_hardware.h:82-93`; `apu_gp_ucode.h:171-173,188-210` | `apu/apu_regs.h:54-57` (0x2000 = SECTL; only `XCNTMODE` mask 0x18; 0x2004 undefined); `apu/apu.c:80-84,270-296` |
| C1.3b | GP program RAM is at BAR0+0x20000 | WRONG | high | `apu_hardware.h:84`; `apu_gp_ucode.h:157-180` | `apu/apu.c:395-405` (VP window 0x20000, GP 0x30000, EP 0x50000); `apu/dsp/gp_ep.c:290-295,335-340`; `apu_regs.h:112-125` (GP P memory at BAR0+0x3A000) |
| C1.3c | Streaming the microcode to BAR0+0x20000+4i is harmless | WRONG | high | `apu_gp_ucode.h:51-80,94-140,157-180`; `alc_context.c:146-158` | `apu/vp/vp.c:595-660` (methods), `150-155,186` (`assert(v < 256)`), `651-656` (`assert(0)`); `include/qemu/osdep.h:311-316` |
| C1.4 | EP registers EP_CONTROL 0x3000, FIFO_CONFIG 0x3004, FIFO_ROUTE 0x3010 | WRONG | high | `apu_hardware.h:98-108`; `apu_ep.c:3-54` | `apu/apu_regs.h:79-110` (GP FIFO registers from 0x3024; EP FIFO from 0x4024), `122-125`; `apu/apu.c:92-96`; `apu/dsp/gp_ep.c:359-441` |
| C1.4b | EP enable bit, Dolby/DSE encoder bit, 4-bit channel-routing nibbles | NOT IN XEMU | high | `apu_hardware.h:105-147`; `apu_ep.c:8-18,31-38` | `apu/dsp/gp_ep.c:185-196,198-249,454-455,482-484` (EP "enable" = both `EPRST` bits set); `hw/xbox/mcpx/aci.c:42-69`; no DSE/AC-3/S-PDIF symbol anywhere under `hw/xbox` |
| C1.5 | VP_CONTROL bit 2 selects 64 3D voices XOR 256 2D voices | WRONG | high | `apu_hardware.h:19-21,51-57,69-77`; `apu_voice.c:41-48` | `apu/apu_regs.h:330-331`; `apu/vp/vp.c:357,1380-1385,1431-1437,1458-1465,1472-1477,1546-1551` (all 256 handles coexist; `v < 64` selects HRTF semantics) |
| C1.6 | The bring-up in `alcOpenDevice` programs what the APU needs | WRONG | medium | `alc_context.c:96-181`; `apu_voice.c:20-58`; `apu_ep.c:3-21` | Registers xemu consumes and the library never writes: `FECTL`, `FETFORCE1`, `FENADDR`, `SECTL.XCNTMODE`, `VPVADDR`, `VPSGEADDR`, `VPSSLADDR`, `GPSADDR/GPFADDR/EPSADDR/EPFADDR`, the four `*MAXSGE`, `TVL2D/3D/MP`, the GP/EP FIFO blocks, `GPRST/EPRST`, every front-end method (`apu_regs.h:27-125,128-214`; `apu/apu.c:270-296`). Real silicon may need more (UNVERIFIABLE) |

Additional findings (C1):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C1.A1 | critical | `alcOpenDevice` bring-up aborts xemu (section 2.3; at 639dcdd, and in this branch with `-DOPENAL_APU_REAL_MMIO`): upload into the method window, `SECTL` never non-OFF, `GPRST/EPRST` never written | `alc_context.c:96-181`; `apu_gp_ucode.h:157-195`; `apu_voice.c:20-58` | `apu/vp/vp.c:150-155,595-660`; `apu/apu.c:270-296` |
| C1.A2 | high | One-shot completion is unobservable: `VP_ACTIVE_0/1` are plain storage that only the library writes, so a finished one-shot stays "playing" forever | `apu_voice_mgr.c:121-127,235-259`; `apu_voice.c:115-207`; `al_source.c:964-972` | `apu/apu.c:46-64,92-96`; `apu/vp/vp.c:118-132` |
| C1.A3 | high | Voice-list heads reset to 0 = voice 0, not empty; terminator is 0xFFFF; library never links voices | `apu_voice.c:20-58` | `apu/apu_regs.h:65-73`; `apu/vp/vp.c:1809-1841` |
| C1.A4 | high | GP microcode memory-map assumptions conflict with xemu's DSP map (see C5) | `apu_gp_ucode.h:14-25,39-140` | `apu/apu_regs.h:322`; `apu/dsp/interp/dsp_cpu.c:897-964` |
| C1.A5 | medium | Voice table: library allocates 8 KiB for handles 0-63; xemu indexes up to 256 handles (32 KiB) with no bounds check; the 4 KiB alignment is not required by xemu (real hardware UNVERIFIABLE) | `apu_hardware.h:55-57`; `alc_context.c:107-119` | `apu/apu_regs.h:58,220,330-331`; `apu/vp/vp.c:100-116` |
| C1.A6 | medium | `AL_XBOX_VP_BASE_PHYS` and `AL_XBOX_DOLBY_DIGITAL_ACTIVE` read back registers the library itself wrote (0x1004, 0x3000), so they only appear to work | `alc_context.c:662-673`; `apu_ep.c:31-38` | `apu/apu.c:55-59,92-96` |
| C1.A7 | medium | `FENADDR` (0x115C) is never programmed; every voice end writes notifier bytes at `FENADDR + 16*(2 + 4*voice + n) + 15` (and the byte before) into low guest RAM, and sets ISTS bits 5 and 6 | `alc_context.c:96-181` | `apu/vp/vp.c:33-52,118-132`; `apu/apu.c:322` |
| C1.A8 | medium | First-frame chain: list heads, an inactive listed voice, `SE2FE_IDLE_VOICE` without `FETFORCE1` bit 15 | `apu_voice.c:20-58` | `apu/vp/vp.c:559-570,1809-1841`; `apu/apu.c:270-296` |
| C1.A9 | medium | The shipped samples took the same abort path at 639dcdd (in this branch only with `-DOPENAL_APU_REAL_MMIO`) | `samples/apu_tone/main.c:166-190,251-253`; `openal_2d`, `openal_51_test`, `openal_benchmark`, `audio_3d_spatial` | `apu/vp/vp.c:150-155,651-656` |
| C1.A10 | low | xemu names the EP "Encode Processor"; the library says "Output Processor routing six FIFOs to AC'97/DSE" | `apu_hardware.h:23-24`; `apu_ep.h:12-27` | `apu/dsp/gp_ep.c:358` |
| C1.A11 | low | `NV_PAPU_GPDSP_PRAM_SIZE` = 2048 words is the bootstrap image size, not the 4096-word P window | `apu_hardware.h:95-96` | `apu/dsp/gp_ep.c:290-295,335-340`; `apu/dsp/dsp_c.c:89-104` |
| C1.A12 | info | GP/EP window accesses must be aligned 32-bit (asserted); the library's volatile 32-bit accessors comply | `apu_hardware.h:153-190` | `apu/dsp/gp_ep.c:267-268,311-312,363-364,401-402` |
| C1.A13 | info | The APU interrupt is level-sensitive INTA on IRQ 5; the library has no interrupt handling | `apu_voice.c:39,48` | `hw/xbox/xbox_pci.c:125`; `apu/apu.c:26-44` |

### C2 - Voice control model

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C2.1 | A 128-byte per-voice context in RAM, started by setting bits in 64-bit ACTIVE/PAUSE registers | PARTIAL (128-byte per-voice record in RAM at a programmed base: right; base register, layout, 64-voice bound, bitmask start/stop: wrong) | high | `apu_voice.h:68-139`; `apu_voice.c:39-54,115-190`; `al_source.c:352-417` | `apu/apu_regs.h:58,219-319`; `apu/vp/vp.c:100-116,164-576` (methods), `595-665`, `1809-1858` |
| C2.2 | 64 3D voices are the whole voice set; 256 sources are virtualised over 64 slots because of a mode switch | WRONG (the 64 cap is self-imposed; xemu runs 256 handles at once, 64 of them HRTF-capable) | high | `apu_hardware.h:19-21,69-77`; `apu_voice_mgr.c:12-28` | `apu/apu_regs.h:330-331`; `apu/vp/vp.c:357,1380-1385,1431-1437,1458-1465,1546-1552,1825` |
| C2.3 | Pause is a bit in a 64-bit `VP_PAUSE` bitmask (0x1020/0x1024) | WRONG | high | `apu_voice.c:51-54,169-190` | `apu/apu_regs.h:145-147,277`; `apu/vp/vp.c:311-315,913-915,1297-1307` (method 0x140, argument bit 18, sets `PAR_STATE.PAUSED`) |
| C2.4 | Playback position = `current_prd_index` + `sample_pos_frac`, read back from the context for preemption | WRONG | high | `apu_voice_mgr.c:150-157`; `al_source.c:366-375`; `apu_voice.h:88-90` | `apu/vp/vp.c:217-218` (VOICE_ON zeroes CBO), `398-413`, `651-656` (GET_VOICE_POSITION asserts), `1012-1013,1054,1093-1119` (CBO is a 24-bit sample index at voice offset 0x58); no fractional phase in the record |
| C2.5 | Envelope fields eg1_*/eg2_*, zero-filled by reset | PARTIAL (two 6-phase envelope generators exist; the library's fields are not they; zero parameters play at unity only if VOICE_ON starts the OFF phase) | high | `apu_voice.h:100-110`; `apu_voice.c:78-94` | `apu/apu_regs.h:138-139,251-264,272-289`; `apu/vp/vp.c:182-273,274-307,679-835,1309-1329` |
| C2.6 | One-shot completion is detected by polling an ACTIVE bit | PARTIAL (the analogue is `PAR_STATE.ACTIVE_VOICE` in the voice's RAM record; completion also raises notifier/ISTS and the idle-voice trap) | high | `apu_voice.c:192-207`; `apu_voice_mgr.c:121-127,238-260` | `apu/apu_regs.h:216,276-289,345-353`; `apu/vp/vp.c:33-52,118-132,559-570,825-829,946-949,1093-1116,1833-1836` |
| C2.7 | The library's voice programming (`al_source_program_hw_voice`) suffices to make a mono PCM16 voice audible | WRONG (xemu needs about a dozen methods plus `SECTL`, `VPVADDR`, `VPSGEADDR`, `FENADDR`, list heads, `FETFORCE1`, an SGE table; section 7) | medium | `al_source.c:352-417`; `apu_voice.c:20-58` | `apu/vp/vp.c:164-413,667-677,837-1121,1288-1482,1809-1841` |

Additional findings (C2):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C2.A1 | high | `VP_CONTROL`/`VP_BASE_ADDR` collide with ISTS/IEN; `VP_ACTIVE/PAUSE` are unused offsets (see C1.2) | `apu_hardware.h:62-67` | `apu/apu_regs.h:27-32` |
| C2.A2 | high | Volume polarity is inverted: 12-bit attenuation in 1/64 dB, 0 = unity, 0xFFF = mute; a zero-filled voice record sends eight 0 dB copies into bin 0 (for handles below 64 see C4.A3 for the non-null zero HRTF handle and C4.A4 for the redirection of outputs 0-3); "zero the gains before stopping" would be full level | `apu_voice.h:96-98,131-132`; `apu_voice_mgr.c:32-34,162-166,211-214`; `al_source.c:258-262,380-395` | `apu/vp/vp.c:94-98,1387-1412,1469-1482`; `apu/apu_regs.h:296-310` |
| C2.A3 | high | `TAR_PITCH` is a signed 4.12 log2 value in bits 31:16, `rate = 1 / 2^((p + pitchscale*32*ef)/4096)`; the library's linear 16.16 step is not representable | `apu_voice.h:44-57,84-85`; `al_source.c:245-256,271,378` | `apu/vp/vp.c:393-397,1316-1320`; `apu/apu_regs.h:182-183,317-319` |
| C2.A4 | high | Voice-list management is mandatory: VOICE_ON pushes onto a list unconditionally, so re-issuing it for a linked voice creates a cycle. The frame walk follows a list for at most 256 steps and enqueues every active voice it meets, so a cycle fills the shared 256-entry work queue, and any further active voice enqueued later in the same frame (from a later list, or a prefix before the cycle) trips the enqueue assert and aborts xemu (inferred from the code). VOICE_OFF and natural completion never unlink; no method removes a voice | `apu_voice.c:115-167`; `apu_voice_mgr.c:121-127,168-188,202-224,238-260` | `apu/vp/vp.c:182-214,308-310,559-570,1647-1656,1823-1839`; `apu/vp/vp.h:73` |
| C2.A5 | high | The `ACTIVE`/`PAUSE` completion path can never fire (a self-echo of the library's own writes); `samples/apu_tone` prints "RUNNING" from its own write | `apu_voice.c:115-140,192-207`; `apu_voice_mgr.c:235-259`; `samples/apu_tone/main.c:249-259`; `tests/test_preemption.c:128` | `apu/apu.c:46-64,92-96` |
| C2.A6 | high | The microcode upload target 0x20000 is the method window (see C1.3c) | `apu_gp_ucode.h:157-180` | `apu/vp/vp.c:595-660` |
| C2.A7 | medium | Routing: eight (5-bit bin, 12-bit attenuation) outputs; for handles < 64 bins 0-3 are forced to the HRTF submix bins; a handle < 64 with a non-null HRTF handle and no loaded HRIR is silent | `apu_voice.h:127-132`; `al_source.c:394-411` | `apu/vp/vp.c:1362-1412,1458-1482,1546-1568`; `apu/vp/hrtf.h:46-49` |
| C2.A8 | medium | Buffers use a flat SGE table with a byte-offset `BA` and sample-index CBO/EBO/LBO; streams use SSL segments | `apu_mem.h:13-53`; `al_buffer.c:285-329` | `apu/vp/vp.c:667-677,837-863,917-985,1012-1062,1093-1119` |
| C2.A9 | medium | Stereo PCM needs `CFG_FMT.SAMPLES_PER_BLOCK = 1` (stride = container bytes x (field + 1)); with 0 the right sample overlaps the next frame's left (inference from the code) | `al_source.c:362-363,396-411`; `apu_voice.h:77-78` | `apu/vp/vp.c:864-866,1008,1052-1085`; `apu/apu_regs.h:232-233,240` |
| C2.A10 | medium | The Q14 biquad, ITD pair and RAM ITD buffer have no counterpart in the voice record (see C4) | `apu_voice.h:112-125` | `apu/vp/vp.h:91-98`; `apu/vp/hrtf.h:35-44` |
| C2.A11 | medium | Mixbin 31 is the multipass bin; routing any output of a normal voice to it can trip `voice_work_schedule` asserts | `apu_voice.h:127-132` | `apu/vp/vp.c:1665-1704` (asserts at 1677-1684); `apu/apu_regs.h:358-359` |
| C2.A12 | low | PCM8 is documented "signed" (wrong comment; data path correct, see C3.1) | `apu_voice.h:33` | `apu/vp/vp.c:1063-1066` |
| C2.A13 | low | No FE method unlinks a voice: the guest edits the predecessor's next-handle field (voice offset 0x7C, bits 15:0) or the list head; the frame walk reads it without a lock | `apu_voice_mgr.c:168-188,202-224` | `apu/vp/vp.c:134-148,176-178,191-214,1823-1839` |
| C2.A14 | info | Quirks: a voice that ends mid-frame loses that whole frame; `NEW_VOICE` is never set; per-voice headroom field is unused; `TAR_*` volumes are applied without ramping | `apu_voice_mgr.c:150-174` | `apu/vp/vp.c:1012-1013,1093-1116,1356-1360`; `apu/apu_regs.h:231,278` |

### C3 - Sample formats, buffer addressing and pitch

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C3.1 | `NVAPU_VOICE_FORMAT_PCM8` is "8-bit Signed PCM"; OpenAL u8 data is copied unchanged | PARTIAL (the comment is WRONG, hardware 8-bit is unsigned; the unchanged copy is CORRECT) | high | `apu_voice.h:33`; `al_buffer.c:255-269,315`; `al_source.c:362` | `apu/apu_regs.h:241-243` (`SAMPLE_SIZE_U8 = 0`); `apu/vp/vp.c:1063-1066`; `apu/fpconv.h:31-34` ((v - 0x80)/128) |
| C3.2 | 16-bit PCM is signed little-endian; mono/stereo is one flag | PARTIAL (semantics CORRECT; the library's 2-bit format codes are not the hardware fields `SAMPLE_SIZE`/`CONTAINER_SIZE`/`STEREO`, and the `SAMPLES_PER_BLOCK` stride rule is missing) | medium | `apu_voice.h:32,37-38,76-77`; `al_buffer.c:260-274,315` | `apu/apu_regs.h:232,240-250`; `apu/vp/vp.c:841-849,1008,1052-1090` |
| C3.3 | `NVAPU_VOICE_FORMAT_ADPCM` is "Xbox ADPCM 4-bit" | PARTIAL (hardware format is 36 bytes per channel and block; the enum is dead: `alBufferData` cannot load it) | high | `apu_voice.h:34`; `al_buffer.c:254-278`; `al_source.c:362` | `apu/vp/adpcm.h:50-140`; `apu/vp/vp.c:889-890,990-1046`; `apu/apu_regs.h:246-250,336` |
| C3.4 | Buffers are PRD tables of 8-byte `{phys, control}` entries, chunks <= 65535 bytes, EOT in bit 31, one table per buffer | WRONG | high | `apu_mem.h:25-53`; `apu_mem.c:330-381`; `al_buffer.c:285-313` | `apu/vp/vp.c:667-677` (8-byte SGE entry per 4 KiB page, only dword 0 used), `957-967` (SSL entry: length in samples in bits 15:0), `1019-1056`; `apu/apu_regs.h:59-60,338-343` |
| C3.5 | Pitch is a linear 16.16 step, clamped 0x1000..0x40000 | WRONG | high | `apu_voice.h:44-57,84-85`; `al_source.c:245-256` | `apu/vp/vp.c:393-397,1316-1321,1181`; `apu/apu_regs.h:182-183,317-319` |
| C3.6 | Looping uses a loop bit plus byte offsets `loop_start_offset`/`loop_end_offset` | PARTIAL (loop bit, loop-start and end exist; they are 24-bit **sample** indices and EBO is the inclusive last sample) | high | `apu_voice.h:40-42,78,92-94`; `al_source.c:359-374,764-770` | `apu/apu_regs.h:186-191,238,293-294`; `apu/vp/vp.c:402-413,854-863,1012-1013,1093-1116` |
| C3.7 | Voices are resampled to a fixed 48 kHz mix in 32-sample frames by a "polyphase resampler" | PARTIAL (48 kHz domain and 32-sample frames CORRECT; the interpolation method is UNVERIFIABLE: xemu uses libsamplerate sinc and says the hardware method is unknown) | medium | `apu_voice.h:19,24,47-57` | `apu/apu_regs.h:333,363`; `apu/vp/vp.c:1123-1194` (comment at 1167-1172) |

Additional findings (C3):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C3.A1 | high | The VP SGE table is global (one `VPSGEADDR`); all voices share one linear address space and each buffer needs a distinct BA range; the per-buffer PRD tables and per-voice `prd_table_phys` have no counterpart | `apu_voice.h:88`; `al_source.c:366`; `al_buffer.c:285-329` | `apu/apu_regs.h:59`; `apu/vp/vp.c:446-448,482-484,667-677,1029-1030,1054-1056` |
| C3.A2 | medium | Buffer-mode PCM translates one SGE address per sample frame (per CBO step; ADPCM: per 32-bit word of a block) and does not follow page crossings inside it (FIXME): BA must be a multiple of the frame stride so no frame straddles a 4 KiB page unless pages are contiguous | `apu_mem.c:362-367`; `al_buffer.c:292` | `apu/vp/vp.c:1048,1050-1085`; ADPCM `1026-1034` |
| C3.A3 | medium | VOICE_ON resets CBO to 0, so a restored position (preemption, `AL_*_OFFSET`) must be written after VOICE_ON; no sub-sample phase is kept in the record | `al_source.c:367-375`; `apu_voice_mgr.c:150-157` | `apu/vp/vp.c:216-218,264,406-409,1118-1119` |
| C3.A4 | low | Tests and a sample lock in the linear pitch encoding and the 0x1000..0x40000 clamp | `tests/test_pitch_gain.c:76-147`; `tests/test_spatial_attenuation.c:428-445`; `samples/apu_tone/main.c:211` | `apu/vp/vp.c:393-397` (only bits 31:16 stored) |
| C3.A5 | low | Hardware CBO/LBO/EBO/BA are 24-bit; `alBufferData` has no frame-count bound | `al_buffer.c:280-283` | `apu/apu_regs.h:185-191,267-270,290-295` |
| C3.A6 | low | Changing `AL_LOOPING` on a live voice is a read-modify-write of the whole `CFG_FMT` dword | `al_source.c:764-769` | `apu/vp/vp.c:328-331` |
| C3.A7 | low | End-of-buffer quirk: loop runs while `cbo <= ebo`, wrap/stop test is `cbo >= ebo`; the sample at EBO can be skipped at a frame boundary | `samples/apu_tone/main.c:215-216` | `apu/vp/vp.c:1012-1013,1093-1116,975` |
| C3.A8 | info | 8-bit slack must be 0x80, not 0; `ALbuffer` stores no frame count; S24/S32, ADPCM and multichannel buffers are unsupported | `apu_mem.c:236,259`; `al_buffer.c:254-278` | `apu/fpconv.h:31-34`; `apu/apu_regs.h:241-250` |

### C4 - Spatialisation and mixing

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C4.1 | Per-voice Q14 biquad, ITD taps 0..64, 256-byte RAM ITD buffer | WRONG (form and storage; the RAM-buffer sub-claim is UNVERIFIABLE) | high | `apu_voice.h:17-24,112-125`; `apu_spatial.c:253-277,368-404`; `al_source.c:206-228,384-395` | `apu/vp/vp.c:316-320,352-368,414-438,1458-1465`; `apu/vp/hrtf.h:29-44,58-142`; `apu/vp/vp.h:91-98` |
| C4.1a | HRTF/ITD is implemented in the voice processor | CORRECT | high | `apu_hardware.h:20-21`; `apu_voice.h:13-24` | `apu/vp/vp.c:1458-1465`; `apu/vp/vp.h:43-49`; no HRTF code under `apu/dsp/` |
| C4.1b | The filter is one Q14 2nd-order biquad, identical for both ears | WRONG (per-ear 31-tap FIR, int8/128 coefficients, L1-normalised) | high | `apu_spatial.h:29-43`; `apu_spatial.c:320-415` | `apu/vp/hrtf.h:30,58-142`; `apu/vp/vp.c:157-162,414-438`; `apu/fpconv.h:26-29` |
| C4.1c | The filter coefficients live in the voice context | WRONG (the record holds only a 16-bit HRTF handle; 0xFFFF = none; coefficients are in a 128-entry device table) | high | `apu_voice.h:112-125`; `al_source.c:299-312` | `apu/apu_regs.h:265-266,355-356`; `apu/vp/vp.c:352-368`; `apu/vp/vp.h:91-98` |
| C4.1d | ITD: two unsigned ear delays 0..64 samples | PARTIAL (a per-3D-voice ITD in 48 kHz samples, one ear delayed at a time, positive delays the left ear: CORRECT; one signed s6.9 value per table entry, fractional, clamped to +/-42, host-side delay line: different) | medium | `apu_spatial.h:14-27,190-213`; `apu_spatial.c:253-312` | `apu/vp/hrtf.h:31-33,58-63,101-142`; `apu/fpconv.h:41-44`; `apu/vp/vp.c:429-438` |
| C4.1e | A 3D voice is routed to six panned mixbins via a routing mask and gains | WRONG (eight outputs; for handles < 64, bins 0-3 are forced to the four HRTF submix bins set by SET_HRTF_SUBMIXES) | high | `apu_voice.h:127-132`; `al_source.c:230-232,384-395` | `apu/vp/vp.c:543-551,1362-1385,1469-1482,1546-1551` |
| C4.2 | Volume = 16-bit linear master gain + 8-bit linear per-mixbin gains | WRONG | high | `apu_voice.h:96-98,127-132`; `al_source.c:258-262,380-395` | `apu/vp/vp.c:94-98,1362-1412,1469-1482`; `apu/apu_regs.h:221-231,296-310` |
| C4.2a | Zero gain means silence | WRONG (0 = 0 dB, 0xFFF = mute) | high | `apu_voice.c:84-94`; `apu_voice_mgr.c:162-165` | `apu/vp/vp.c:94-98,1387-1412` |
| C4.2b | Routing is a 32-bit mixbin mask plus per-bin gains, with no headroom concept | WRONG (exactly 8 outputs per voice; global per-bin 3-bit headroom shift plus a separate HRTF headroom) | high | `apu_voice.h:24,127-132` | `apu/vp/vp.c:543-558,1469-1482`; `apu/vp/vp.h:85-88` |
| C4.3 | MixBin 0..5 = FL, FR, SL, SR, C, LFE | UNVERIFIABLE (xemu attaches no role to bins; its GP monitor taps bins 0/1 as L/R only) | high | `apu_voice.h:132`; `apu_spatial.h:45-56`; `apu_gp_ucode.h:39-50` | `apu/dsp/gp_ep.c:443-452,468-478`; `apu/vp/vp.c:1267-1270` (multipass bin 31) |
| C4.3a | Mixbins are at X:$0000..$001F, one word per bin per tick | WRONG (X:0x1400 + 32*bin, 32 samples each; X:0.. is plain RAM) | high | `apu_gp_ucode.h:14-25,43-80` | `apu/dsp/gp_ep.c:443-452`; `apu/dsp/interp/dsp_cpu.c:897-926,936-964` |
| C4.4 | Volumes ramp toward targets, so "zero then halt" is click-free | WRONG (no ramp: `TAR_*` is applied as a step at the next 32-sample frame; hardware ramping is UNVERIFIABLE) | high | `README.md:8` (old); `apu_voice_mgr.c:150-174,202-224`; `tests/test_preemption.c:58-138` | `apu/vp/vp.c:369-380,795-829,1387-1412,1478-1482` |
| C4.4a | `TVL*/CVL*/NVL*` are volume ramp registers | WRONG (voice-list top/current/next pointers) | high | - | `apu/apu_regs.h:65-73`; `apu/vp/vp.c:25-31,1809-1841` |
| C4.4c | Zeroing volumes then halting a voice is click-free | UNVERIFIABLE on hardware; in xemu the cut is abrupt (the fade path is VOICE_RELEASE, which the library never uses) | medium | `apu_voice_mgr.c:150-174`; `apu_voice.c:142-167` | `apu/vp/vp.c:118-132,274-307,825-829,1478-1481` |
| C4.5 | All voices, stereo/music included, sit in the 64 3D slots; a mode bit switches 3D/2D | WRONG (handles 64-255 are plain voices; non-HRTF voices in slots < 64 get bins 0-3 overridden) | high | `apu_hardware.h:51-57,69-77`; `al_source.c:353,396-411`; `apu_voice_mgr.c:12` | `apu/vp/vp.c:179-214,1380-1385,1431-1437,1472,1546-1551,1809-1841` |
| C4.6 | LFE low-pass / bass management is a hardware stage | NOT IN XEMU (no LFE or bass management in VP, GP or EP; the library itself only applies a gain on bin 5) | high | `README.md:10` (old); `apu_spatial.c:453-465` | `apu/vp/vp.c:1426-1467`; `apu/vp/svf.h`; `apu/dsp/gp_ep.c:443-494` |

Additional findings (C4):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C4.A1 | high | The 128-byte context overlays NV_PAVS with different fields: `master_vol` at 0x1C = `CFG_HRTF_TARGET`; `hrtf_b2/a1` at 0x54 = `PAR_STATE`; `mixbin_routing_mask` at 0x60 = `TAR_VOLA`; `mixbin_gain[12..15]` at 0x7C = next-voice link and pitch | `apu_voice.h:68-139` | `apu/apu_regs.h:219-319` |
| C4.A2 | high | GP microcode words do not decode as commented (section 4.9); ISA, memory map and frame protocol are wrong | `apu_gp_ucode.h:53-139` | `apu/dsp/interp/dsp_cpu.c:152-340,409-421,622`; `dsp_emu.c.inc:5214-5790,7885-7905,7999-8008,8118-8121` |
| C4.A3 | medium | A voice at handle < 64 whose HRTF handle is non-null but whose entry was never latched by SET_VOICE_TAR_HRTF is silent (all-zero coefficients); the library's `master_vol_left` at 0x1C would be read as an HRTF handle | `apu_voice.h:96-98`; `apu_voice.c:84-104`; `al_source.c:380-382` | `apu/vp/vp.c:352-368,1458-1465,1878-1880`; `apu/vp/hrtf.h:46-49` |
| C4.A4 | medium | Placing every source (stereo included) in slots < 64 forces bins 0-3 to the HRTF submix bins | `al_source.c:353,396-411` | `apu/vp/vp.c:1380-1385,1472-1475` |
| C4.A5 | medium | For a mono voice every one of the eight outputs takes ear 0: the right-ear HRTF output is discarded (a possible xemu limitation; do not infer HRTF left/right bin assignment from xemu) | `apu_voice.h:127-132` | `apu/vp/vp.c:1088-1090,1296,1463,1469-1482` |
| C4.A6 | low | HRTF quirks: per-ear L1 normalisation discards level differences; the `break` in `hrtf_filter_set_target_params` skips the second ear when the first ear's L1 sum is exactly 0 or 1 | - | `apu/vp/hrtf.h:58-81` |
| C4.A7 | low | The state-variable filter's normalised cutoff is clamped to [0.003906, 1.0], which does not cover the library's 1-20 kHz elevation cutoffs | `apu_spatial.c:320-355` | `apu/vp/vp.c:1426-1456` |
| C4.A8 | low | ITD sizing: the 256-byte figure is self-consistent; the 64-sample clamp is unreachable (formula peaks at 31); xemu clamps at +/-42 | `apu_spatial.h:18-27`; `apu_spatial.c:253-277` | `apu/vp/hrtf.h:31,62-63` |
| C4.A9 | low | `attack_rate == 0` jumps to full level with a comment that hardware crackled; `VOICE_RELEASE` with release rate 0 cuts within a frame, so a zero-filled envelope does not give a fade | `apu_voice.c:84-94` | `apu/vp/vp.c:274-307,711-716,795-829` |
| C4.A10 | info | 5.1, LFE and Dolby routing cannot be validated in xemu: the monitor is stereo 48 kHz S16 | `README.md:10` (old) | `apu/monitor.c:22-84`; `apu/dsp/gp_ep.c:185-196,227-234`; `apu/apu_regs.h:324-328` |
| C4.A11 | info | HRTF can be disabled by the user (`audio.hrtf`); xemu latches an entry only on SET_VOICE_TAR_HRTF; smoothing is an approximation | `al_source.c:299-312` | `apu/vp/vp.c:352-368,1458-1465`; `apu/vp/hrtf.h:83-99`; `ui/xui/debug.cc:224` |
### C5 - GP and EP DSPs

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C5.1 | GP program is halted via 0x2000, written as dwords at "PRAM" BAR0+0x20000 and started via 0x2000 | WRONG (only the word packing is right) | high | `apu_gp_ucode.h:157-224`; `apu_hardware.h:82-96` | `apu/apu.c:392-405,270-296`; `apu/apu_regs.h:54-57,112-120`; `apu/dsp/gp_ep.c:251-260,263-351` |
| C5.1.1 | `NV_PAPU_GPDSP_PRAM` = 0x20000 | WRONG (VP method window; GP P memory is BAR0+0x3A000) | high | `apu_hardware.h:84`; `apu_gp_ucode.h:176` | `apu/apu.c:395-397`; `apu/dsp/gp_ep.c:335-339`; `apu_regs.h:115` |
| C5.1.2 | `GPDSP_CONTROL`/`GPDSP_STATUS` with RUN/HALT/FRAME_IRQ bits start and stop the GP | WRONG (0x2000 is SECTL; the GP is gated by `GPRST`) | high | `apu_hardware.h:82-93`; `apu_gp_ucode.h:172,193,208,223` | `apu/apu_regs.h:54-57`; `apu/apu.c:80-84,270-296`; `apu/dsp/gp_ep.c:251-260,341-344,454-479` |
| C5.1.3 | A 24-bit instruction is written as one 32-bit dword with the top byte zero | CORRECT | high | `apu_gp_ucode.h:174-177`; `apu_hardware.h:96` | `apu/dsp/gp_ep.c:290-295,335-339`; `apu/dsp/interp/dsp_cpu.c:888-895,936-961`; `apu/dsp/dsp_c.c:94-101` |
| C5.1.4 | GP program RAM is 2048 words (8 KiB) | PARTIAL (2048 is the bootstrap image size; the P window is 4096 words) | medium | `apu_hardware.h:95-96` | `apu/dsp/interp/dsp_cpu_regs.h:113-115`; `apu/dsp/gp_ep.c:290,335`; `apu/dsp/dsp_c.c:94-97` |
| C5.1.5 | (implied) a HALT/RUN register is the whole reset protocol | WRONG (real protocol: `GPRST` bits 0 and 1; the 0-to-both-set edge DMA-bootstraps P:0..0x7FF from scratch memory through `GPSADDR`/`GPSMAXSGE`) | high | `apu_gp_ucode.h:157-224` (no `GPRST` anywhere in `lib/openal/src`) | `apu/apu_regs.h:116-120`; `apu/dsp/gp_ep.c:51-100,251-260,341-344`; `apu/dsp/dsp_c.c:89-104` |
| C5.2 | The two microcode listings execute as commented | WRONG | high | `apu_gp_ucode.h:51-80,94-140` | `apu/dsp/interp/dsp_cpu.c:152-340,409-421,591-637`; `dsp_emu.c.inc:28-38,5092-5132,5214-5790` (per-word table in section 4.9) |
| C5.2.1 | `0x00000C` is WAIT (sleep until the 48 kHz frame tick) | WRONG (RTS; WAIT is `0x000086`, a no-op in the interpreter; an RTS with an empty stack asserts) | high | `apu_gp_ucode.h:66,112` | `dsp_cpu.c:329,339`; `dsp_emu.c.inc:7999-8008,8118-8121`; `dsp_cpu.c:1096-1119` |
| C5.2.2 | `0x0AF080` plus an address word is `JMP`; `0x000000` is `NOP` | CORRECT | high | `apu_gp_ucode.h:53,56-63,79,96,139` | `dsp_cpu.c:252,310,142-150`; `dsp_emu.c.inc:7007-7016` |
| C5.2.3 | `0x5CE000` is `MOVE #$0000,r0`, `0x5CE1B0` is `MOVE #$FFB0,r1` | WRONG (`move y:(r0),a1`; `mpy +y1,y0,a y:(r1),a1`) | high | `apu_gp_ucode.h:67-68,114-115` | `dsp_emu.c.inc:5606-5674,5787-5790,5092-5132` |
| C5.2.4 | `0x0E4000`/`0x0E4900`/`0x0E4D00` are `MOVE x:(r0)+,a` / `MOVE a,y:(r1)+` / `MOVE b,y:(r1)-` | WRONG (`jcc xxx`, condition 4, targets 0x000/0x900/0xD00) | high | `apu_gp_ucode.h:71-76,123-136` | `dsp_cpu.c:245`; `dsp_emu.c.inc:6891-6903` |
| C5.2.5 | `0x56F400 0x5A827A` is `MOVE #$5A827A,y0` | PARTIAL (valid long-immediate move, but into the accumulator A) | high | `apu_gp_ucode.h:113` | `dsp_emu.c.inc:5606-5674` |
| C5.2.6 | `0x08E040` is `MOVE x:(r0+4),x0` | WRONG (`movep p:(r0),x:$ffffc0`; the write to the unmodelled peripheral address is ignored) | high | `apu_gp_ucode.h:118` | `dsp_cpu.c:300`; `dsp_emu.c.inc:7637-7658`; `apu/dsp/dsp.c:75-104` |
| C5.2.7 | `0x217000`/`0x217800`/`0x217820` are `MPY`/`MACR` | WRONG (register-to-register moves with ALU byte 0x00; `0x217820` adds `x` to `a`) | high | `apu_gp_ucode.h:119,130,132` | `dsp_emu.c.inc:5337-5400,5092-5101` |
| C5.2.8 | `0x200018`/`0x200008`/`0x200028` are `MOVE a,b`/`ADD x0,a`/`ADD x0,b` | WRONG (`add a,b`; ALU slot 0x08 is undefined and asserts; `add x,b`) | high | `apu_gp_ucode.h:120,124,126` | `dsp_emu.c.inc:5337-5357,5092-5101,28-38` |
| C5.2.9 | `0x0000F8 0x001000` is `ORI #$001000,SR` (saturation mode) | WRONG (`0x0000F8` is the complete one-word `ori #0,mr`; `0x001000` matches no opcode and asserts; xemu's core has no saturation-mode bit) | high | `apu_gp_ucode.h:92-93,108-109,281-322` | `dsp_cpu.c:315,409-421`; `dsp_emu.c.inc:7885-7905`; `dsp_cpu_regs.h:28-49` |
| C5.3 | The microcode's address-space assumptions match xemu | WRONG | high | `apu_gp_ucode.h:18-24,43-50` | `apu/apu_regs.h:322-336`; `apu/dsp/dsp.c:44-104`; `dsp_cpu.c:897-964` |
| C5.3.1 | The 32 mixbins are at X:$0000..$001F, one sample each | WRONG (X:0x1400 + 32*bin + sample; host window GP+0x5000; the interpreter also aliases X:0x0C00) | high | `apu_gp_ucode.h:21-22,43-50,67,114` | `apu/dsp/gp_ep.c:278-283,323-328,443-452`; `dsp_cpu.c:905-909,946-949`; `apu_regs.h:113,322,333-334` |
| C5.3.2 | GP output goes to six EP FIFOs at Y:$FFB0..$FFB5 | WRONG (Y RAM is 2048 words, so `Y:$FFB0` asserts; peripherals are X:$FFFF80+; output is DMA into GP output FIFOs 0-3 in guest RAM) | high | `apu_gp_ucode.h:23,43-50,68,115,135-136` | `dsp_cpu.c:917-919,954-956`; `dsp_cpu_regs.h:113-121`; `apu/dsp/dsp_dma.c:103-316`; `apu/dsp/gp_ep.c:143-183` |
| C5.3.3 | Frames run at 48 kHz, one sample per mixbin per tick | WRONG (32-sample frames at 1500 Hz; the EP runs every 8th frame, 256 samples) | high | `apu_gp_ucode.h:21,66,112` | `apu/apu_regs.h:333,363`; `apu/apu.c:219-255`; `apu/dsp/gp_ep.c:443-493`; `apu/dsp/dsp.c:106-109` |
| C5.4 | EP_CONTROL (enable bit 0, DSE bit 1), EP_FIFO_CONFIG (0x03/0x3F), EP_FIFO_ROUTE nibbles | WRONG / NOT IN XEMU (see C1.4, C1.4b) | high | `apu_ep.c:3-38`; `apu_hardware.h:101-147` | `apu/apu_regs.h:79-110,122-125`; `apu/dsp/gp_ep.c:358-436,454-455,481-493` |
| C5.4.1 | A hardware AC-3 / Dolby Digital encoder (or S/PDIF data path) exists in the modelled APU | NOT IN XEMU (the AC97 core has S/PDIF-out registers but transfers no data) | high | `apu_ep.h:17-19,26`; `apu_ep.c:11,37` | no hits under `hw/xbox`; `hw/audio/ac97.c:240-241,341-342,366-367,1026-1125`; `hw/audio/ac97_int.h:32` |
| C5.4.2 | On real hardware AC-3 is produced by firmware on the EP DSP | UNVERIFIABLE (xemu only labels the EP "Encode Processor - encoding DSP" and runs whatever the guest uploads) | medium | `apu_ep.h:17-29` | `apu/dsp/gp_ep.c:358-436` |
| C5.4.3 | `EP_FIFO_CONFIG = 0x3F` selects six channels with nibble routing | WRONG (4 output + 2 input FIFOs; one DMA descriptor can carry up to 16 interleaved channels; xemu's monitor consumes only EP output FIFO 0 as stereo int16) | medium | `apu_hardware.h:110-147`; `apu_gp_ucode.h:242-270` | `apu/apu_regs.h:324-328`; `apu/dsp/gp_ep.c:185-196,198-249`; `apu/dsp/dsp_dma.c:160-165,219-243` |
| C5.5 | EP output reaches AC97/S-PDIF through an internal link | WRONG (the APU monitor is an SDL stream; the AC97 ACI is an independent PCI function; they share only guest RAM, and only in the debug "AC97" monitor mode) | high | `apu_ep.h:13-28` | `apu/dsp/gp_ep.c:26-49,185-196,227-234`; `apu/monitor.c:22-84`; `hw/xbox/mcpx/aci.c:42-69` |
| C5.5.1 | `lib/hal/audio.c`'s AC97 DMA ring (PO engine at 0x110.., SO at 0x170.., `GLOB_CNT` 0x12C, `GLOB_STA` 0x130) fits xemu's ACI | CORRECT (register layout matches; S/PDIF out has no data path in xemu) | high | `lib/hal/audio.c:22-33,156,176-205,237-247` | `hw/audio/ac97.c:117-135` (engine register layout), `785` (`GLOB_STA`), `1026-1125`; `hw/audio/ac97_int.h:28-34`; `hw/xbox/mcpx/aci.c:48-67` (BAR2, NAM +0, NABM +0x100) |

Additional findings (C5):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C5.A1 | high | A GP program that never writes bit 0 to `X:$FFFFC4` hangs the APU thread when "real-time DSP" is on (the host loops `dsp_run(1000)` until the halt request while holding the device lock, and guest writes to the GP/EP windows take the same lock); neither repository program writes it | `apu_gp_ucode.h:51-140` | `apu/dsp/gp_ep.c:305-309,457-466,481-492`; `apu/dsp/dsp.c:76-82`; `apu/apu.c:257-303` |
| C5.A2 | high | Bootstrap on the GPRST 0-to-3 edge overwrites P:0..0x7FF from scratch memory and asserts if the scratch SGE table (`GPSADDR`, `GPSMAXSGE`, at least two entries for the 0x2000-byte image) is not set up | `apu_gp_ucode.h:157-195` | `apu/dsp/gp_ep.c:51-100` (assert at 60), `251-260`; `apu/dsp/dsp_c.c:89-104,271` (P memory is pre-filled with 0xCA bytes) |
| C5.A3 | medium | Default xemu configuration (`audio.use_dsp` presumably false) mixes VP voices straight into the SDL monitor and zeroes the mixbins, so GP/EP code is inaudible; GP/EP tests need "Real-time DSP processing" or the debug monitor | `apu_gp_ucode.h:14-25` | `apu/dsp/gp_ep.c:26-49`; `apu/vp/vp.c:1844-1857`; `config_spec.yml:316`; `ui/xui/main-menu.cc:847-850`; `ui/xui/debug.cc:204-208` |
| C5.A4 | medium | FIFO prerequisites: GP output FIFO with base = end = cur = 0 asserts (`cur < end`); EP FIFO with end == base divides by zero; EP output FIFO 0 must receive exactly 1024 bytes per run when the monitor is EP or GP-or-EP; memory-to-DSP DMA from a FIFO asserts; the DMA chain head is NEXT_BLOCK, not START_BLOCK (4.9) | `apu_hardware.h:99-147` | `apu/dsp/gp_ep.c:60,173,191,237-239`; `apu/dsp/dsp_dma.c:279-291` |
| C5.A5 | medium | The AV-pack numbering that gates the Dolby/5.1 topology does not match the SMC (see C6.2) | `apu_eeprom.h:23-29`; `alc_context.c:631-655` | `hw/xbox/smbus_xbox_smc.c:66-73` |
| C5.A6 | low | The frame-start flag at `X:$FFFFC5` bit 1 is sticky: a program that polls it must clear it by writing the bit back; bit 0 (abort) is never set; the halt write at `$FFFFC4` is honoured only when its bit 0 is set | `apu_gp_ucode.h:66,112` | `apu/dsp/dsp.c:34-36,51-56,76-90,106-109` |
| C5.A7 | low | `lib/hal/audio.c` completes its callbacks from S/PDIF-out buffer completions, which xemu never produces; callback-mode users (for example `samples/xaudio`) may stall under xemu (static trace, not executed) | `lib/hal/audio.c:70-121,238,271-277` | `hw/audio/ac97.c:240-241,341-342,1026-1125` |
| C5.A8 | info | The interpreter makes WAIT and STOP no-ops and asserts on stack under/overflow, on undefined ALU slots (0x04, 0x08, 0x0C, 0x15), on words that match no template, and on words whose template has no emulation function (81 of the table's 187 templates; the matcher alone is therefore not a validity check). `exception_debugging` is set at reset; the JIT backend is not in the checkout | `apu_gp_ucode.h:112` | `apu/dsp/interp/dsp_cpu.c:152-340,405,419,628-633,1104-1110`; `dsp_emu.c.inc:28-38,5092-5132,8010-8013,8118-8121`; `apu/dsp/dsp_jit.c:221` |

### C6 - AV pack and EEPROM audio flags

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C6.1 | `HalReadSMBusValue(0x20, 0x04, FALSE, &v)` reads the AV pack (slave 0x20, register 0x04, byte read) | CORRECT (0x20 is the 8-bit form of the 7-bit SMC address 0x10; the kernel adds the read bit, inferred from the AMD756 model) | high | `apu_eeprom.c:42-57`; `apu_eeprom.h:13` | `hw/xbox/xbox.c:300`; `hw/xbox/amd_smbus.c:85-87,107-110`; `hw/xbox/smbus_xbox_smc.c:66,116-125,166-172,185-186` |
| C6.2 | The returned byte is compared against `APU_AV_PACK_*` (NONE 0, STANDARD 1, RFU 2, SCART 3, HDTV 4, VGA 5, SVIDEO 6: the kernel's `AV_PACK_*` enum, `lib/xboxkrnl/xboxkrnl.h:58-65`) | WRONG (the SMC returns SCART 0, HDTV 1, VGA 2, RFU 3, SVIDEO 4, COMPOSITE 6, NONE 7; xemu's default HDTV pack reads 1 = "standard") | high | `apu_eeprom.h:23-29`; `apu_eeprom.c:42-63`; `alc_context.c:631-659` | `hw/xbox/smbus_xbox_smc.c:66-73,185-186,215-241`; `hw/xbox/xbox.c:501`; `config_spec.yml:358-361` |
| C6.2b | `AL_XBOX_AV_PACK_COMPOSITE` is 0x05 | WRONG (0x06 in the SMC; 0x05 is never produced; it equals the index of "composite" in xemu's config enum, a probable origin, speculation) | high | `alext.h:38`; `alc_context.c:631-659` | `hw/xbox/smbus_xbox_smc.c:72`; `config_spec.yml:358-361`; `system/vl.c:3009-3017` |
| C6.3 | Exactly the HDTV, S-Video and SCART packs have a TOSLink output | UNVERIFIABLE (xemu models the pack only as an ID byte) | high | `apu_eeprom.h:15-21,108-116`; `apu_eeprom.c:59-63` | `hw/xbox/smbus_xbox_smc.c:66-73` |
| C6.4 | `XC_AUDIO_FLAGS_*` values (MONO 0, STEREO 1, AC3 0x10000, DTS 0x20000, SURROUND 0x40000 at 639dcdd; this branch uses 0x2 for SURROUND, equally unverified) | UNVERIFIABLE (only this repository defines them; xemu sets no audio flags) | medium | `apu_eeprom.h:33-61` | `hw/xbox/eeprom_generation.c:93-120`; `hw/xbox/eeprom_generation.h:29-43` |
| C6.5 | `ExQueryNonVolatileSetting(XC_AUDIO, ...)` is called correctly | CORRECT (signature and index match the nxdk header and the in-tree timezone code; the kernel is not emulated) | high | `apu_eeprom.c:28-40` | `hw/xbox/smbus_storage.c:154-156` (EEPROM is an SMBus storage device at 0x54 that only supplies bytes) |
| C6.6 | 5.1 digital output is selected when an optical-capable pack AND the AC3 flag are present | PARTIAL (a reasonable heuristic; its inputs are wrong; xemu cannot validate it) | medium | `apu_eeprom.c:65-74`; `apu_eeprom.h:63-80` | `apu/monitor.c:24-28` (stereo output only); `hw/xbox/eeprom_generation.c:112-116` |

Additional findings (C6):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C6.A1 | high | At 639dcdd the hardware branches in `apu_eeprom.c` and `apu_mem.c` were guarded by `defined(__NXDK__) \|\| defined(_XBOX)`; `bin/nxdk-cc` defines only `-DNXDK`, so on nxdk builds both queries returned their host fallbacks (STEREO flags, STANDARD pack) and the contiguous-memory pool was a malloc'ed host pool with a fake physical base. Topology was therefore always STEREO_20. Fixed in this branch (2.4, fix 1) | `apu_eeprom.c:4-6,28-39,47-56`; `apu_mem.c:5,42,63,104,123,249,322`; `bin/nxdk-cc:21` | n/a (build issue) |
| C6.A2 | low | The `XC_AUDIO` fallback `#define` (suffix `u`) is redefined by `xboxkrnl.h`: `-Wmacro-redefined`, a warning (see 1.5 item 5) | `apu_eeprom.h:39-41`; `apu_eeprom.c:1,4-6`; `lib/xboxkrnl/xboxkrnl.h:4228` | n/a |
| C6.A3 | medium | Tests and sample labels feed kernel-style pack values as if they were SMC output, so the suite passes while hardware decoding is wrong | `tests/test_eeprom_smbus.c`; `tests/test_dse_toslink.c:140-160`; `samples/openal_51_test/main.c:126-137` | `hw/xbox/smbus_xbox_smc.c:66-73` |
| C6.A4 | low | On `HalReadSMBusValue` failure the library returns `APU_AV_PACK_STANDARD`; the full ULONG is returned unmasked | `apu_eeprom.c:47-56`; `alc_context.c:653-656` | `hw/xbox/smbus_xbox_smc.c:73` |
| C6.A5 | info | xemu's generated EEPROM populates only the first 44 bytes of the user section plus the language dword at user-section offset 0x2C (EEPROM 0x90); everything else, including wherever the audio flags live on real EEPROMs (xemu does not say), is zero. That 0 means "MONO" is an unproven interpretation | `apu_eeprom.c:28-40` | `hw/xbox/eeprom_generation.c:93-120`; `hw/xbox/eeprom_generation.h:29-43` |
| C6.A6 | info | xemu models no S/PDIF output data, no DSE and no AC-3; its monitor and AC97 outputs are 2-channel; its default pack is HDTV | `apu_ep.c:3-22`; `apu_gp_ucode.h:236-275` | `apu/monitor.c:24-28`; `hw/audio/ac97.c:297`; `hw/xbox/xbox.c:501` |

### C7 - README, benchmark and test-suite claims

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C7.1 | The README's "Verified Performance (Pentium III 733.333 MHz)" numbers (7,780 / 65,450 / 127,060 / 261,800 cycles for 1 / 16 / 32 / 64 voices) were verified | UNVERIFIABLE (no code, constant or log produces them; they appear only as prose; "verified" is unjustified) | high | `README.md:13-17` (old); `samples/openal_benchmark/main.c:177-194,199-207`; `tests/test_benchmark.c:34-42,139-141` | `hw/xbox/xbox.c:457` (QEMU pentium3 CPU model; no TSC-frequency setting); xemu is not cycle-accurate |
| C7.1b | The benchmark measures APU mixing cost | WRONG (it times the library's software spatial math against a calloc'ed mock MMIO block; nothing is mixed) | high | `tests/test_benchmark.c:58-78,127-135`; `al_source.c:646-665,276-325`; `apu_voice_mgr.c:232-290` | `apu/vp/vp.c:1387-1412,1478-1481` |
| C7.1c | The benchmark's pass/fail text and thresholds agree with the README | WRONG (sample prints "< 0.5 %" and "zero software mixing load" unconditionally; the README gives 1.04 % and 2.14 %; the host test asserts only an active-voice count; its time-conversion constants are unused) | high | `samples/openal_benchmark/main.c:165-167,192-193` (old); `README.md:16-17` (old); `tests/test_benchmark.c:26-28,144-148` (old) | - |
| C7.1d | xemu can settle the CPU-cost / "zero CPU mixing" feasibility claim | UNVERIFIABLE (xemu shows the APU model, not the guest CPU, does the mixing; it cannot confirm cycle counts or frame-budget percentages) | medium | `README.md:6,13-17` (old) | `apu/apu.c:219-255`; `apu/vp/vp.c:1585-1645,1809-1858` |
| C7.2 | The README's test count ("17 automated regression test suites") is current | WRONG (stale: the runner discovers every `tests/test_*.c`; `test_benchmark` is a timing harness, `test_wav_loader` tests sample-side parsing) | high | `README.md:20` (old); `tests/run_tests.py:4` (old) | - |
| C7.2b | The tests cover "100 % of the hardware abstraction and OpenAL 1.1 APIs" | WRONG (measured 2,088 of 2,706 lines = 77.2 % in `lib/openal/src` at 639dcdd; 40 of the 94 public functions declared at 639dcdd are referenced only by the link/smoke test; several APIs are stubs) | high | `README.md:20` (old); `tests/test_lib_build.c`; `al_source.c:1207-1251,1421-1435`; `alc_context.c:328-340,512-564,609-612` | - |
| C7.3 | "Zero CPU Mixing Overhead: all voice mixing, polyphase resampling, pitch shifting and panning are processed directly on the APU VP and GP" | UNVERIFIABLE for the VP/GP offload (xemu confirms the *architecture*: voice mixing, resampling, envelopes and HRTF are the VP's job); WRONG as a description of this library (its register layer does not match, panning/ITD/HRTF coefficients are computed on the CPU, its GP programs are invalid) | medium | `README.md:6-11` (old); `apu_hardware.h:57-117`; `al_source.c:122-233` | `apu/vp/vp.c:182-273,308-310,595-660,1809-1858` |
| C7.4a | "Click-free preemption": volumes zeroed before halting, phase and PRD index preserved | UNVERIFIABLE on hardware; WRONG for the stated mechanism under xemu (no ramp; VOICE_ON resets CBO and the filter and resampler state; fractional phase is not a guest-visible field) | medium | `apu_voice_mgr.c:130-175`; `al_source.c:366-375,414-416`; `tests/test_preemption.c:92-93,119-129` | `apu/vp/vp.c:54-62,94-98,216-220,264,274-307,406-409,795-824,1387-1412,1478-1481` |
| C7.4b | "Hardware Dolby Digital DSE real-time bitstream encoding over optical TOSLink" | NOT IN XEMU (the library only writes a bit to a register it defines itself; no AC-3 encoder, S/PDIF framing or optical path exists in the library or in xemu) | high | `README.md:10` (old); `apu_ep.c:3-14,27-34`; `apu_hardware.h:106-109`; `tests/test_dse_toslink.c` | no hits under `hw/xbox`; `apu/dsp/gp_ep.c:358,395-429`; `apu/monitor.c:23-27` |
| C7.4c | "ITU-R BS.775 stereo downmixing with DSP56300 saturation limiting for analog RCA" | PARTIAL (the host C arithmetic is sound and tested against itself; the microcode does not decode as documented, section 4.9) | medium | `apu_gp_ucode.h:82-134,301-359`; `tests/test_stereo_downmix.c:36-219,254-262,283-294,357` | `apu/dsp/interp/dsp_cpu.c:315,329,339,409-421`; `dsp_emu.c.inc:5150-5211` (accumulator-to-register moves always limit to 24 bits) |
| C7.5 | The test suite runs clean on the host (`-std=c99 -pedantic -Wall -Werror`, clang) | PARTIAL at 639dcdd: libraries and tests compile warning-free, but `run_tests.py` never linked libm (reproduced on Linux with clang 18: 21 tests discovered, 3 passed, 18 failed to link on the libm symbols; the passes were `test_buffer_prd`, `test_eeprom_smbus` and `test_scene_engine`) and `make test` could never pass (it stopped at the first test with "not found": phony test targets disabled the implicit rule, and the nested library build forces `-m32`). Both are fixed in this branch | high | `tests/run_tests.py` (old); `tests/Makefile` (old); `lib/openal/Makefile:34-45` | - |

Additional findings (C7):

| ID | Sev. | Finding | Repo ref | xemu ref |
|---|---|---|---|---|
| C7.A1 | medium | `AL_EXT_MCFORMATS` was advertised while `alBufferData` rejects the multichannel formats (`AL_INVALID_ENUM`) | `alc_context.c:580-581,588-597`; `al_buffer.c:254-277`; `alext.h:16-19` | - |
| C7.A2 | medium | The README called the library OpenAL 1.1, but `alSourceQueueBuffers`/`alSourceUnqueueBuffers` return `AL_INVALID_OPERATION`, `AL_BUFFERS_QUEUED/PROCESSED` are faked, `AL_SEC/SAMPLE/BYTE_OFFSET` are defined but unimplemented, capture is stubbed, extension/enum queries return constants | `al_source.c:988-998,1205-1251`; `al.h:83-90`; `alc_context.c:512-530,538-564,609-612` | - |
| C7.A3 | medium | The benchmark numbers look derived rather than measured: 65,450 x 4 = 261,800 exactly, so the 16- and 64-voice tiers have an identical per-voice cost; all four values are multiples of ten; the host test's microsecond/percent constants are unused; the time-and-percent columns could only come from the Xbox sample, which prints on screen and then loops forever | `README.md:14-17` (old); `tests/test_benchmark.c:26-28`; `samples/openal_benchmark/main.c:165-167,199-207` | - |
| C7.A4 | low | The timed region of both benchmarks includes the harness's own `sinf`/`cosf`; the sample also sleeps 16 ms between frames, the host test does not; the sample has 60 frames per tier, the host test 50 | `tests/test_benchmark.c:127-135`; `samples/openal_benchmark/main.c:143-163` | - |
| C7.A5 | low | `*.a` and extension-less test binaries were not git-ignored; `run_tests.py` wrote `libopenal.a` and `.obj` files into the source tree | `tests/run_tests.py` (old); `.gitignore` | - |

### C8 - Minimal correct driver bring-up (claims about the existing library)

| ID | Repository claim / assumption | Verdict | Conf. | Repo ref | xemu ref |
|---|---|---|---|---|---|
| C8.1 | The register map and enable model (VP_CONTROL, VP_ACTIVE, GPDSP_CONTROL RUN, EP_CONTROL ...) start the APU | WRONG (no frame is ever generated) | high | `apu_hardware.h:62-66,82-89,101-108`; `apu_voice.c:39-54`; `apu_gp_ucode.h:172,193` | `apu/apu_regs.h:27-32,54-58`; `apu/apu.c:66-98,270-296` |
| C8.2 | GP microcode can be uploaded to BAR0+0x20000 | WRONG (aborts xemu) | high | `apu_gp_ucode.h:157-180`; `alc_context.c:146-158` | `apu/vp/vp.c:150-155,595-660`; `apu/dsp/gp_ep.c:333-340` |
| C8.3 | A 128-byte bitfield context written by the CPU plus ACTIVE bits runs a voice | WRONG | high | `apu_voice.h:68-139`; `apu_voice.c:96-140`; `al_source.c:350-419` | `apu/apu_regs.h:219-319`; `apu/vp/vp.c:100-116,164-576,1809-1842` |
| C8.4 | PRD tables with <= 65535-byte chunks and an EOT flag address sample data | WRONG | high | `apu_mem.h:29-35,50-53`; `apu_mem.c:330-381` | `apu/vp/vp.c:667-677,398-413,439-454,1050-1115` |
| C8.5 | Linear 16.16 pitch, 8-bit linear gains plus mask routing, 16-bit master volume, "signed" PCM8 | WRONG (pitch, volume and routing); PCM8 data path CORRECT | high | `apu_voice.h:31-34,44-57,96-98,127-132`; `al_source.c:366-405` | `apu/vp/vp.c:393-397,1316-1320,94-98,1362-1412,1469-1482`; `apu/fpconv.h:31-34` |
| C8.6 | GP microcode contract: mixbins at X:$0000, outputs Y:$FFB0, WAIT as frame sync | WRONG | high | `apu_gp_ucode.h:20-25,51-140` | `apu/dsp/gp_ep.c:443-479`; `apu/dsp/dsp.c:44-109`; `dsp_cpu.c:329,339,917-919` |
| C8.7 | EP registers, DSE bit, six-channel routing, GP FIFOs wired to the EP, AC97 fed by the EP | NOT IN XEMU | high | `apu_ep.c:3-21`; `apu_hardware.h:20-24,98-147` | `apu/dsp/gp_ep.c:185-249,359-436,454-494`; `apu/dsp/dsp_dma.c:261-289`; `hw/xbox/mcpx/aci.c:42-69` |
| C8.8 | Voices that finish simply disappear; completion is readable from a hardware-cleared ACTIVE bit | WRONG (a finished voice stays linked; the next frame raises `SE2FE_IDLE_VOICE`, which asserts unless `FETFORCE1` bit 15 is set and otherwise traps the front end until the guest unlinks the voice and clears `FECTL`) | high | `apu_voice_mgr.c:120-126,172`; `al_source.c:960-970` | `apu/vp/vp.c:118-132,559-570,1823-1842`; `apu/apu.c:28-30,270-296` |
| C8.9 | PCI identity bus 0 dev 5 fn 0, 10DE:01B0, 0x80000-byte BAR0 | CORRECT (base address UNVERIFIABLE) | high | `apu_hardware.h:30-49` | `apu/apu.c:392-407,583-584`; `hw/xbox/xbox.c:331`; `include/hw/pci/pci_ids.h:292,297` |
| C8.10 | EP output reaches the AC97 codec through an internal link driven by the ACI | WRONG (the APU thread pushes frames at `EP_FRAME_US` = 5333 us; the ACI has no timer; AC97 playback is pulled by the host audio backend) | high | `apu_ep.h:13-28` | `apu/apu.c:169-217,257-303`; `apu/apu_regs.h:363`; `apu/monitor.c:70-84`; `hw/audio/ac97.c:1122-1125` |

The 25-step bring-up sequence derived from this cluster, with corrections, is in section 7.

---
## 4. Corrected hardware reference

Everything in this section is what xemu 478b4f4 implements, read from the cited lines. Where a statement
is inference it says so. It describes xemu's model of the APU; real silicon may differ where xemu says it
is guessing (section 6). All offsets are byte offsets; all register and method accesses should be aligned
32-bit accesses. Paths are relative to `hw/xbox/mcpx/apu/` unless they start with `hw/` or `include/`.

### 4.1 Device and BAR0 layout

* PCI bus 0, device 5, function 0, 10DE:01B0, revision 177, class multimedia-audio, interrupt pin INTA,
  routed to ISA IRQ 5 (`apu.c:386-407,583-586`; `hw/xbox/xbox_pci.c:125`).
* BAR0 is a 0x80000-byte memory BAR whose address is assigned by the BIOS (not stated in xemu).

| BAR0 offset | Size | Contents |
|---|---|---|
| 0x00000-0x1FFFF | 0x20000 | Main register file: each written byte-address is stored in its own slot and read back, except the special registers in 4.2 (`apu.c:46-98`) |
| 0x20000-0x2FFFF | 0x10000 | Voice-processor / front-end **method window** (4.3): a 32-bit write at `0x20000 + method` runs the method synchronously (`apu.c:395-397`; `vp/vp.c:595-660`) |
| 0x30000-0x3FFFF | 0x10000 | GP DSP window: X memory +0x0000, mixbin buffer +0x5000, Y memory +0x6000, P memory +0xA000, GPRST +0xFFFC (`apu.c:399-401`; `dsp/gp_ep.c:263-356`) |
| 0x40000-0x4FFFF | - | no region; reads return 0, writes are dropped |
| 0x50000-0x5FFFF | 0x10000 | EP DSP window: X +0x0000, Y +0x6000, P +0xA000, EPRST +0xFFFC (`apu.c:403-405`; `dsp/gp_ep.c:359-441`) |
| 0x60000-0x7FFFF | - | no region; reads return 0, writes are dropped |

GP and EP window accesses assert that the access size is 4 and the offset is 4-aligned
(`dsp/gp_ep.c:267-268,311-312,363-364,401-402`). The GP/EP window write handlers take the device lock that the
frame thread holds while it builds a frame (`dsp/gp_ep.c:309`, `apu.c:257-303`).

### 4.2 Main register file (offsets in BAR0)

| Offset | Name | Meaning (xemu 478b4f4) |
|---|---|---|
| 0x1000 | `ISTS` | Interrupt status, **write-1-to-clear**. Bits: 0 GINTSTS (global, derived), 4 FETINTSTS (front-end trapped), 5 FENINTSTS (notify), 6 FEVINTSTS (voice) (`apu_regs.h:27-31`; `apu.c:74-79`) |
| 0x1004 | `IEN` | Interrupt enable. The line asserts when bit 0 is set and `ISTS & ~bit0 & IEN` is non-zero (`apu.c:26-44`). IEN is plain storage and does not itself re-evaluate the line (`apu.c:92-96`). The line is re-evaluated by an ISTS write, even of 0 (`apu.c:74-79`), and by the frame thread whenever it raises its irq flag: on every loop pass while `SECTL.XCNTMODE` is OFF or `FECTL` has any of bits 7:5 set (the loop wakes at least every 5 ms), and after a notifier write or an idle-voice trap (`apu.c:273-294`; `vp/vp.c:51,566`). With frames running, a pending ISTS bit therefore stays un-raised until one of those events |
| 0x1100 | `FECTL` | Front-end control. `FEMETHMODE` mask 0xE0: 0 free-running, 0x80 halted, 0xE0 trapped; `FETRAPREASON` mask 0xF00 (0xF00 = requested). Any of bits 7:5 stops frame generation (`apu_regs.h:33-39`; `apu.c:270-296`) |
| 0x1110 | `FECV` | Current voice handle (set by SET_CURRENT_VOICE) |
| 0x1118 | `FEAV` | Antecedent voice: `VALUE` bits 15:0, `LST` bits 17:16 (set by SET_ANTECEDENT_VOICE; used by VOICE_ON) |
| 0x115C | `FENADDR` | Physical base of the notifier array (`vp/vp.c:33-52`) |
| 0x1300 / 0x1304 | `FEDECMETH` / `FEDECPARAM` | Last method and argument; **every** method overwrites them (`vp/vp.c:172-173`) |
| 0x1324 / 0x1334 | `FEMEMADDR` / `FEMEMDATA` | A write to FEMEMDATA also stores the value at the physical address held in FEMEMADDR (`apu.c:85-91`). The source comment suggests software uses this when notifies complete; the purpose is unverified, and `apu.c:85-91` is the only use of either register in the xemu tree, so there are no fence or ordering semantics |
| 0x1500 / 0x1504 | `FETFORCE0` / `FETFORCE1` | FETFORCE0 is unused. FETFORCE1 bit 15 (`SE2FE_IDLE_VOICE`) must be set or an inactive listed voice asserts (`vp/vp.c:559-570`) |
| 0x2000 | `SECTL` | `XCNTMODE` mask 0x18: 0 = OFF; **no frame is generated unless it is non-zero**. xemu defines only OFF; the meaning of the other encodings is unknown (`apu_regs.h:54-56`; `apu.c:270-296`) |
| 0x200C | `XGSCNT` | Read-only: `ep_frame_div` x 32, i.e. the frames processed since the last EPRST write times 32 (`apu.c:52-54,251`); every EPRST write clears it (`dsp/gp_ep.c:428`), device reset does not (`apu.c:320-330`) |
| 0x202C | `VPVADDR` | Physical base of the voice array: voice `h` is at `VPVADDR + h * 0x80` (`vp/vp.c:100-116`) |
| 0x2030 | `VPSGEADDR` | Physical base of the VP SGE (page) table (4.5) |
| 0x2034 | `VPSSLADDR` | Physical base of the SSL segment table (stream voices) |
| 0x2040 / 0x2044 | `GPSADDR` / `GPFADDR` | GP scratch / GP FIFO SGE table bases |
| 0x2048 / 0x204C | `EPSADDR` / `EPFADDR` | EP scratch / EP FIFO SGE table bases |
| 0x2054 / 0x2058 / 0x205C | `TVL2D` / `CVL2D` / `NVL2D` | 2D voice list: top (head), current, next. Reset value 0 reads as voice 0; the terminator is 0xFFFF |
| 0x2060 / 0x2064 / 0x2068 | `TVL3D` / `CVL3D` / `NVL3D` | 3D voice list |
| 0x206C / 0x2070 / 0x2074 | `TVLMP` / `CVLMP` / `NVLMP` | multipass voice list |
| 0x20D4 / 0x20D8 | `GPSMAXSGE` / `GPFMAXSGE` | highest valid SGE index, inclusive, for GP scratch / FIFO (`dsp/gp_ep.c:60`) |
| 0x20DC / 0x20E0 | `EPSMAXSGE` / `EPFMAXSGE` | the same for the EP |
| 0x3024 + 0x10*n (n = 0..3) | `GPOFBASEn` / `+4 END` / `+8 CUR` | GP output FIFO n: base, end (exclusive), current; low 24 bits are used as a byte offset into the GP FIFO linear space |
| 0x3064 + 0x10*n (n = 0..1) | `GPIFBASEn` / `END` / `CUR` | GP input FIFOs (registers exist; the DMA path is not implemented) |
| 0x4024 + 0x10*n (n = 0..3) | `EPOFBASEn` / `END` / `CUR` | EP output FIFOs |
| 0x4064 + 0x10*n (n = 0..1) | `EPIFBASEn` / `END` / `CUR` | EP input FIFOs |

`GPRST` (BAR0 + 0x3FFFC) and `EPRST` (BAR0 + 0x5FFFC): bit 0 `RST`, bit 1 `DSPRST`, bit 2 `NMI`,
bit 3 `ABORT` (the last two are defined but unimplemented). The DSP runs only while **both** bits 0 and 1 are
set. Writing a value with either bit clear resets the DSP core: PC = 0, registers and stack are reset and the
core's own peripheral array is zeroed; X/Y/P memory is kept. The state that the peripheral accessors actually
use, the sticky `$FFFFC5` flag word and the DMA registers (NEXT_BLOCK, START_BLOCK, CONTROL, the end-of-list
flag), lives outside the core and is not touched by the reset, so a stale start-frame flag or DMA chain head
survives `GPRST = 0` (`dsp/dsp_c.c:58-62`, `dsp/interp/dsp_cpu.c:349-407`, `dsp/dsp.c:44-104`). A transition
from "not both set" to "both set" runs the **bootstrap**: `P:0..0x7FF` (0x2000 bytes) are overwritten from GP
(EP) scratch memory at offset 0 through the scratch SGE table (`dsp/gp_ep.c:251-260,341-344,425-429`;
`dsp/dsp_c.c:89-104`). An EPRST write also clears the frame divider (`dsp/gp_ep.c:428`). Device reset clears
only the main register file and the VP state; the GP/EP register files and DSP memories are **not** cleared
(`apu.c:320-330`).

### 4.3 Front-end PIO method table

Method offsets are relative to `BAR0 + 0x20000`. The write value is the argument. Unlisted offsets are
silently ignored (`vp/vp.c:595-660`). A read of `0x010` (`PIO_FREE`) always returns 0x80 ("queue empty");
every other read returns 0 (`vp/vp.c:578-593`). The three methods marked **assert** call `assert(0)` and abort
xemu.

| Offset | Method | Argument |
|---|---|---|
| 0x120 | `SET_ANTECEDENT_VOICE` | bits 15:0 handle, bits 17:16 list: 0 inherit (insert after the handle), 1 2D top, 2 3D top, 3 multipass top |
| 0x124 | `VOICE_ON` | bits 15:0 handle, bits 27:24 EF start phase, bits 31:28 EA start phase. Links the voice, clears CBO, resets filters, sets `ACTIVE_VOICE` (`vp/vp.c:182-273`) |
| 0x128 | `VOICE_OFF` | handle; clears `ACTIVE_VOICE`, writes the notifier, sets ISTS bits 5 and 6 |
| 0x12C | `VOICE_RELEASE` | handle; starts both release envelopes |
| 0x130 | `GET_VOICE_POSITION` | **assert** |
| 0x140 | `VOICE_PAUSE` | bits 15:0 handle, bit 18 = 1 pause / 0 resume (`PAR_STATE.PAUSED`) |
| 0x160 | `SET_CURRENT_HRTF_ENTRY` | entry index 0..127 |
| 0x180 | `SET_CONTEXT_DMA_NOTIFY` | **assert** |
| 0x18C | `SET_CURRENT_SSL_CONTEXT_DMA` | **assert** |
| 0x190 | `SET_CURRENT_SSL` | SSL base page: the low 6 bits must be 0 and the value must be below 0x10000 (8192 SSL entries x 8 bytes); both are asserted (`vp/vp.c:520-525`) |
| 0x200 + 4n (n = 0..31) | `SET_SUBMIX_HEADROOM[n]` | bits 2:0 headroom shift for mixbin n |
| 0x280 | `SET_HRTF_HEADROOM` | bits 2:0 |
| 0x2C0 | `SET_HRTF_SUBMIXES` | four 5-bit mixbin numbers at bits 4:0, 12:8, 20:16, 28:24 |
| 0x2F8 | `SET_CURRENT_VOICE` | handle (stored in `FECV`); the voice-config methods below act on it |
| 0x2FC | `VOICE_LOCK` | bit 0 lock / unlock the current voice (a locked queued voice stalls the whole VP frame) |
| 0x300 / 0x304 | `SET_VOICE_CFG_VBIN` / `CFG_FMT` | whole voice dwords 0x00 / 0x04 |
| 0x308 / 0x30C / 0x310 / 0x314 / 0x318 | `CFG_ENV0` / `ENVA` / `ENV1` / `ENVF` / `MISC` | whole voice dwords 0x08 / 0x0C / 0x10 / 0x14 / 0x18 |
| 0x31C | `SET_VOICE_TAR_HRTF` | bits 15:0 HRTF handle (0xFFFF = none); for voices < 64 and a non-null handle it latches that table entry (asserts handle < 128) |
| 0x320 / 0x35C | `SET_VOICE_SSL_A` / `SSL_B` | bits 7:0 segment count, bits 31:8 base page |
| 0x360 / 0x364 / 0x368 | `SET_VOICE_TAR_VOLA` / `VOLB` / `VOLC` | whole voice dwords 0x60 / 0x64 / 0x68 (4.7) |
| 0x36C | `SET_VOICE_LFO_ENV` | whole dword 0x6C |
| 0x374 / 0x378 | `SET_VOICE_TAR_FCA` / `FCB` | whole dwords 0x74 / 0x78 |
| 0x37C | `SET_VOICE_TAR_PITCH` | bits 31:16 pitch (4.6); only that field of dword 0x7C changes |
| 0x3A0 / 0x3A4 / 0x3D8 / 0x3DC | `CFG_BUF_BASE` / `CFG_BUF_LBO` / `BUF_CBO` / `CFG_BUF_EBO` | 24-bit BA / LBO / CBO / EBO; the upper byte of the dword is preserved |
| 0x400 + 4k (k = 0..14) | `SET_HRIR` | four int8 coefficients: left tap 2k (7:0), right tap 2k (15:8), left tap 2k+1 (23:16), right tap 2k+1 (31:24) |
| 0x43C | `SET_HRIR_X` | left tap 30 (7:0), right tap 30 (15:8), ITD as signed s6.9 (31:16) |
| 0x600 + 8n / 0x604 + 8n | SSL segment offset / length | stored at `VPSSLADDR + ssl_base_page*8 + (method - 0x600)`; 64 pairs are intended, but the accepted window is 0x600-0x803 (`vp/vp.c:526-541,639-640`), so a write to 0x800 is also accepted, as a 65th offset word |
| 0x804 / 0x808 | `SET_CURRENT_INBUF_SGE` / `_OFFSET` | handle = SGE index; offset stores `arg & 0xFFFFF000` as dword 0 of `VPSGEADDR + index*8` |
| 0x1000 + 8n / 0x1004 + 8n | `SET_OUTBUF_BA` / `_LEN` (n = 0..3) | accepted and ignored |
| 0x1800 / 0x1808 | `SET_CURRENT_OUTBUF_SGE` / `_OFFSET` | writes into the same `VPSGEADDR` table (xemu itself doubts this, `vp/vp.c:476-481`) |

Voice-config methods are whole-dword writes except the partial-mask ones noted (pitch, HRTF handle,
BA/LBO/CBO/EBO). VOICE_ON is the only method that rewrites a voice's next-voice field, so stale link bits
survive other methods.

### 4.4 The NV_PAVS voice record (0x80 bytes at `VPVADDR + handle * 0x80`)

| Offset | Name | Fields |
|---|---|---|
| 0x00 | `CFG_VBIN` | `V0BIN` 4:0, `V1BIN` 9:5, `V2BIN` 14:10, `V3BIN` 20:16, `V4BIN` 25:21, `V5BIN` 30:26 (5-bit mixbin numbers) |
| 0x04 | `CFG_FMT` | `V6BIN` 4:0, `V7BIN` 9:5, `HEADROOM` 15:13 (unused), `SAMPLES_PER_BLOCK` 20:16 (shares its bits with `MULTIPASS_BIN`), `MULTIPASS` 21, `LINKED` 22, `PERSIST` 23, `DATA_TYPE` 24 (1 = stream), `LOOP` 25, `CLEAR_MIX` 26, `STEREO` 27, `SAMPLE_SIZE` 29:28, `CONTAINER_SIZE` 31:30 |
| 0x08 | `CFG_ENV0` | `EA_ATTACKRATE` 11:0, `EA_DELAYTIME` 23:12, `EF_PITCHSCALE` 31:24 (signed) |
| 0x0C | `CFG_ENVA` | `EA_DECAYRATE` 11:0, `EA_HOLDTIME` 23:12, `EA_SUSTAINLEVEL` 31:24 |
| 0x10 / 0x14 | `CFG_ENV1` / `CFG_ENVF` | the same fields for the filter/pitch envelope (`EF_FCSCALE` 31:24 is not implemented) |
| 0x18 | `CFG_MISC` | `EF_RELEASERATE` 11:0, `FMODE` 17:16 (filter mode) |
| 0x1C | `CFG_HRTF_TARGET` | HRTF handle 15:0 (0xFFFF = none) |
| 0x20 | `CUR_PSL_START` | `BA` 23:0, buffer base (linear byte offset) |
| 0x24 | `CUR_PSH_SAMPLE` | `LBO` 23:0, loop start (sample index) |
| 0x34 | `CUR_ECNT` | `EACOUNT` 15:0, `EFCOUNT` 31:16 (envelope counters) |
| 0x54 | `PAR_STATE` | `PAUSED` 18, `NEW_VOICE` 20, `ACTIVE_VOICE` 21, `EFCUR` 27:24, `EACUR` 31:28 |
| 0x58 | `PAR_OFFSET` | `CBO` 23:0 (current sample index, written back by the VP), `EALVL` 31:24 |
| 0x5C | `PAR_NEXT` | `EBO` 23:0 (inclusive last sample index), `EFLVL` 31:24 |
| 0x60 / 0x64 / 0x68 | `TAR_VOLA` / `VOLB` / `VOLC` | eight 12-bit attenuations (4.7) |
| 0x6C | `TAR_LFO_ENV` | `EA_RELEASERATE` 11:0 |
| 0x74 / 0x78 | `TAR_FCA` / `TAR_FCB` | filter cutoff (15:0, signed 4.12 log2) and Q (31:16), left / right |
| 0x7C | `TAR_PITCH_LINK` | next-voice handle 15:0, pitch 31:16 |
| 0x28, 0x2C, 0x30, 0x38-0x50, 0x70 | - | no field defined in xemu |

(`apu_regs.h:219-319`.) Envelope phases (`EACUR`/`EFCUR`): 0 OFF, 1 DELAY, 2 ATTACK, 3 HOLD, 4 DECAY,
5 SUSTAIN, 6 RELEASE, 7 FORCE_RELEASE (`apu_regs.h:281-288`); anything above 7 asserts
(`vp/vp.c:830-833`). The DELAY, HOLD, DECAY and RELEASE counters are loaded with `rate * 16` and decremented
once per 32-sample frame: `VOICE_ON` loads the delay or hold time for a DELAY or HOLD start phase, the
HOLD-to-DECAY step loads the decay rate and `VOICE_RELEASE` loads the release rate. The ATTACK counter is
different: it is cleared on entry and counts up to `attack_rate * 16`; SUSTAIN forces it to 0
(`vp/vp.c:222-261,274-300,690-794`). With both start phases OFF the envelope output is exactly 1.0 and never
advances; a non-OFF start phase with all-zero parameters decays to silence after about two frames
(`vp/vp.c:686-689,705-794`). Even with the filter envelope OFF the pitch term `EF_PITCHSCALE * 32` is added,
so `CFG_ENV0` must be written as 0 for a plain voice (`vp/vp.c:1316-1320`).

### 4.5 Sample formats and buffer addressing

| Field | Values |
|---|---|
| `SAMPLE_SIZE` | 0 **U8 (unsigned, 0x80 = silence)**, 1 S16 little-endian, 2 S24 (low 24 bits of a 32-bit word), 3 S32 (`apu_regs.h:241-245`; `vp/vp.c:1062-1082`) |
| `CONTAINER_SIZE` | 0 B8 (1 byte), 1 B16 (2 bytes), 2 ADPCM (36-byte block per channel), 3 B32 (4 bytes) (`vp/vp.c:846-849`) |
| `STEREO` | 1 = interleaved L,R containers; mono is duplicated to both channels (`vp/vp.c:1059-1090`) |

**Stride rule (read from the code, so labelled inference about intent).** For PCM the per-frame stride is
`container_bytes * (SAMPLES_PER_BLOCK_field + 1)` and the address of frame `CBO` is `BA + CBO * stride`; the
right channel is read at `+container_bytes`. Interleaved stereo therefore only works with the field set to
1 (stride = two containers); with 0 the right sample of one frame overlaps the left sample of the next.
For ADPCM the block is `36 * (field + 1)` bytes, so stereo ADPCM needs field = 1 (72 bytes)
(`vp/vp.c:864-866,1008,1052-1085,1018`). What real software or hardware programs here is UNVERIFIABLE.

**Buffer-mode voices (`DATA_TYPE` = 0).** The sample address is translated through one global SGE table at
`VPSGEADDR`: entry = `linear >> 12`, an 8-byte entry whose dword 0 is the physical page address and whose
dword 1 is never read; physical address = `dword0 + (linear & 0xFFF)` (`vp/vp.c:667-677`). `BA` is a 24-bit
**byte** offset into the SGE-mapped linear space (the address of sample frame `CBO` is `BA + CBO * stride`,
`vp/vp.c:1054`; for ADPCM `BA` is added to the byte offset of the block, `vp/vp.c:1018,1026`), whereas `CBO`,
`LBO` and `EBO` are 24-bit **sample-frame** indices; `EBO` is the inclusive last frame; the loop bit wraps
`CBO` to `LBO`, otherwise the voice is turned off (`vp/vp.c:1012-1013,1093-1116`). The translation is done
once per sample frame (once per `CBO` step; for ADPCM once per 32-bit word of a block, `vp/vp.c:1026-1034`)
and does not follow a 4 KiB page crossing inside that access (FIXME at `vp/vp.c:1048`), so a sample frame
must not straddle a page unless the pages are contiguous.

**Stream voices (`DATA_TYPE` = 1).** Two segment lists (A and B) per voice, each with a base page and a
count (SSL_A/SSL_B). A segment entry at `VPSSLADDR + (base + index) * 8` holds the physical byte address of
the segment (dword 0, non-zero, used directly, so a segment must be physically contiguous) and, in dword 1,
the length in samples (15:0), container size (17:16), samples-per-block minus one (22:18) and stereo (23);
these must equal the voice's `CFG_FMT` (asserts). Streams cannot loop, and a non-`PERSIST` stream is ended
immediately unless the amplitude envelope is already in release (`vp/vp.c:917-985`). ADPCM stream segments are
read in whole 64-sample blocks: a block that does not fit completely inside floor(length / 64) blocks asserts
(`vp/vp.c:1018-1022`), so ADPCM segment lengths should be multiples of 64 samples (inference about intent).

**ADPCM.** Each 36-byte per-channel block is a 4-byte header (int16 little-endian predictor, which is also
the first output sample; int8 step index 0..88; one zero byte), then 8 chunks of 4 bytes (stereo: the
channel-0 chunk, then the channel-1 chunk); each byte is two 4-bit samples, low nibble first; the standard
89-entry IMA step table with index adjustments -1,-1,-1,-1,2,4,6,8 applies. A block decodes to 65 samples
per channel; xemu advances 64 samples per block, so 64 versus 65 is UNVERIFIABLE (`vp/adpcm.h:50-140`;
`vp/vp.c:1014-1046`; `apu_regs.h:336`). A header with an out-of-range index or a non-zero fourth byte decodes
to nothing.

### 4.6 Pitch

`TAR_PITCH` bits 31:16 hold a signed 16-bit **4.12 log2** value `p`. Per frame the VP computes the converter
ratio `rate = 1 / 2^((p + EF_PITCHSCALE * 32 * ef) / 4096)` (with `ef` the filter-envelope output, 1.0 when
OFF) and passes it to the resampler as output frames per input frame, so the source frames consumed per
48 kHz output frame are `2^(p/4096)` (`vp/vp.c:393-397,1316-1321,1181`). For a source rate `F` and an OpenAL
pitch multiplier `m`:

`p = round(4096 * log2(F * m / 48000))`, written as `(uint32_t)(uint16_t)p << 16`.

| Source rate | `p` | `TAR_PITCH` argument |
|---|---|---|
| 48000 Hz | 0 | `0x00000000` |
| 96000 Hz | +4096 | `0x10000000` |
| 44100 Hz | -501 (exact -500.76) | `0xFE0B0000` |
| 32000 Hz | -2396 | `0xF6A40000` |
| 24000 Hz | -4096 | `0xF0000000` |
| 22050 Hz | -4597 | `0xEE0B0000` |
| 8000 Hz | -10588 | `0xD6A40000` |

Unity is 0, not 0x10000. xemu does not clamp; the field wraps at 16 bits (range about 0.004x to 256x of
48 kHz). The library's linear 16.16 values (0x10000, 0xEB33 for 44.1 kHz) are not representable; written as the
0x37C method argument (only bits 31:16 are stored) they would give `p` of 1 (0x10000) and 0 (0xEB33). Loading
their low 16 bits into the field instead would be equally wrong (0xEB33 would give `p` = -5325, about
19.5 kHz). The sign convention is derived from xemu's code and its debug UI (`ui/xui/debug.cc:146`); whether
DirectSound's signs match silicon is UNVERIFIABLE beyond that.

### 4.7 Volumes and routing

Each voice has exactly **eight outputs**, each a (mixbin number, volume) pair. Bins 0-5 come from
`CFG_VBIN`, bins 6 and 7 from `CFG_FMT`. A volume is a 12-bit **attenuation in 1/64 dB**: the linear factor
is `10^(-vol / 1280)`, **0 is unity (0 dB)**, 0xFFE is -63.97 dB and **0xFFF is mute** (`vp/vp.c:94-98`).
Packing (`apu_regs.h:296-310`; `vp/vp.c:1387-1412`):

| Output | Volume field |
|---|---|
| 0 | `VOLA[15:4]` |
| 1 | `VOLA[31:20]` |
| 2 / 3 | `VOLB[15:4]` / `VOLB[31:20]` |
| 4 / 5 | `VOLC[15:4]` / `VOLC[31:20]` |
| 6 | `(VOLC[3:0] << 8) \| (VOLB[3:0] << 4) \| VOLA[3:0]` |
| 7 | `(VOLC[19:16] << 8) \| (VOLB[19:16] << 4) \| VOLA[19:16]` |

Example: `VOLA = 0x000F000F`, `VOLB = VOLC = 0xFFFFFFFF` keeps outputs 0 and 1 at 0 dB and mutes the other six.
A zero-filled record mixes eight 0 dB copies into bin 0 (for handles of 64 and above, or with `audio.hrtf`
off, or with an HRTF handle of 0xFFFF). For a handle below 64 a zero HRTF handle is not the null handle (null
is 0xFFFF), so with `audio.hrtf` on (the default) the voice runs the HRTF filter with its all-zero, never
latched coefficients and is silent (`vp/vp.c:1458-1465`, `vp/hrtf.h:46-49`, `vp/vp.c:1878-1880`); its
outputs 0-3 also go to the HRTF submix bins instead of the record's bins. Linear gain `g` to attenuation:
`vol = clamp(round(-1280 * log10(g)), 0, 0xFFE)`, and 0xFFF when `g` is below about 6.3e-4. For example
`g = 0.5` gives `0x181` and `g = 0.7071` gives `0x0C1` (0x181 encodes -6.016 dB and 0x0C1 encodes -3.016 dB;
the ideal values are -6.021 dB and -3.010 dB).

Every voice, including those routed to bin 0, contributes through all eight outputs
(`vp/vp.c:1469-1482`). A mono voice feeds its sample to all eight; a stereo voice feeds the left sample to
even outputs and the right sample to odd ones. Each output is divided by `2^headroom`, where the headroom is
the per-destination-bin value set by `SET_SUBMIX_HEADROOM` (0..7, reset 0), or the HRTF headroom for outputs
0-3 of a voice below 64. **For handles below 64, outputs 0-3 are always redirected to the four bins set by
`SET_HRTF_SUBMIXES`** (reset: all bin 0), whatever `V0BIN..V3BIN` say (`vp/vp.c:1380-1385,1472-1475`).
Mixing happens in floating point; the mixbins are converted to 24-bit Q23 (1.0 = 0x800000, saturating at
0x7FFFFF) when handed to the GP (`apu/fpconv.h:56-68`; `dsp/gp_ep.c:446-452`).

Mixbin 31 is the multipass bin. `voice_work_schedule` asserts that a voice either targets bin 31 itself or
follows no voice that left bin 31 dirty, that multipass voices use bin 31 and `CLEAR_MIX`, and that an MP source
voice is ordered consecutively with its consumer. All eight bin fields (including muted ones) count, so a
driver must keep unused slots on a bin below 31 (`vp/vp.c:1525-1583,1658-1705`).

Volume and envelope values are read once per 32-sample frame and applied as a constant for the frame; there
is no ramp, and the volume registers contain no linear gain stage: the only per-voice linear scaling is the
amplitude-envelope level (1.0 while the EA phase is OFF), and the host-side `audio.volume_limit` acts as a
master gain on the SDL stream (`vp/vp.c:1387-1412,1470,1478-1482`; `apu/monitor.c:77-78`). Hardware
ramping toward `TAR_*` values is UNVERIFIABLE. To fade a voice, use `VOICE_RELEASE` with a non-zero release
rate (an exponential, about -60 dB over `rate * 16` frames; the voice switches itself off at the end,
`vp/vp.c:274-307,795-829`); release rate 0 cuts within a frame.

### 4.8 HRTF

* Runs in the voice processor, per voice, for **handles below 64**, when `g_config.audio.hrtf` is on
  (default true) and the voice's `CFG_HRTF_TARGET` is not 0xFFFF; it runs after the optional state-variable
  low-pass and before volume and mixbin accumulation (`vp/vp.c:1426-1465`).
* A 128-entry table (`HRTF_ENTRY_COUNT`), each entry = two 31-tap int8 FIRs (coefficient = value / 128) plus
  one ITD, signed s6.9 (value / 512), clamped to +/-42 samples. A positive ITD delays the left channel by that
  many samples, a negative one the right channel, with linear interpolation of the fraction
  (`vp/hrtf.h:29-44,58-63,101-142`; `apu/fpconv.h:26-29,41-44`). The table is loaded with
  `SET_CURRENT_HRTF_ENTRY`, `SET_HRIR` x 15 and `SET_HRIR_X`, and an entry is latched into a voice only by
  `SET_VOICE_TAR_HRTF` (`vp/vp.c:316-320,352-368,414-438`).
* Each ear's target coefficients are normalised to unit L1 norm, so interaural level cues in the coefficients
  are discarded; current values follow the targets with a per-sample one-pole smoothing (alpha 0.01), which
  xemu marks as not matching hardware (`vp/hrtf.h:58-99`).
* A voice below 64 with a non-null handle whose entry was never latched has all-zero coefficients and is
  silent (`vp/hrtf.h:46-49`; `vp/vp.c:1878-1880`).
* For a mono voice, all eight outputs take ear 0 (`samples[i][b % channels]`); the right-ear output is
  discarded. Whether that is deliberate (for example if DirectSound programs HRTF voices as stereo) is
  unclear (`vp/vp.c:1296,1463,1480`).
* The library's per-voice Q14 biquad, two unsigned ITD taps in 0..64 and a RAM ITD buffer have no
  counterpart.

### 4.9 GP and EP DSPs (DSP56300)

**Memory map (C interpreter).** X RAM 4096 words (`X:0x000-0xFFF`; the mixbin buffer 1024 words appears at
`X:0x1400-0x17FF` and is aliased at `X:0xC00-0xFFF`); Y RAM **2048** words (any larger Y address asserts);
P RAM 4096 words; peripherals at `X:0xFFFF80` and above. All three RAMs start filled with 0xCA bytes, and a P
word with a non-zero top byte asserts when read, so running off the end of a loaded program aborts
(`dsp/interp/dsp_cpu_regs.h:108-121`; `dsp/interp/dsp_cpu.c:888-964`; `dsp/dsp_c.c:271-273`). The host
windows (4.1) show X (GP 0x1000 words, EP 0xC00 words), the mixbin buffer (GP only, `+0x5000`), Y (GP 0x800
words, EP 0x100 words) and P (0x1000 words); the mixbin buffer is rewritten from the VP every frame.

**Frames.** A frame is 32 samples (1500 per second). Each frame the host writes the 32 mixbins (24-bit,
`X:0x1400 + 32*bin + sample`), sets the start-frame flag (bit 1 of `X:$FFFFC5`) and runs the GP until the
program writes bit 0 to `X:$FFFFC4`; with `audio.use_dsp` off (non-realtime) only one 1000-cycle quantum
runs per frame. The EP runs every 8th frame (256 samples, `EP_FRAME_US` = 5333). Writing bit 0 to `$FFFFC4`
is the **only** end-of-frame mechanism; WAIT and STOP are no-ops. In realtime mode a program that never
halts spins the APU thread while it holds the device lock (`dsp/gp_ep.c:443-494`; `dsp/dsp.c:34-36,75-109`;
`dsp/dsp_c.c:69-87`).

**Peripherals implemented:** `$FFFFB3` (reads 0), `$FFFFC4` (halt request), `$FFFFC5` (read: pending flags
plus DMA end-of-list bit 7; write: clear the written bits), `$FFFFD4` NEXT_BLOCK, `$FFFFD5` START_BLOCK,
`$FFFFD6` CONTROL, `$FFFFD7` CONFIGURATION (the DMA engine). Everything else reads as 0xABABA or is ignored
(`dsp/dsp.c:44-104`).

**DMA.** Writing CONTROL action 1 (start) executes the descriptor chain synchronously, beginning at the
pointer in NEXT_BLOCK (`$FFFFD4`, initial value 0, so the first node is read from X:0 unless software writes
NEXT_BLOCK first). START_BLOCK (`$FFFFD5`) is only stored and read back; the engine never uses it
(`dsp/dsp_dma.c:110-143,334,376`). A head pointer with bit 14 set starts nothing; otherwise each pass runs the
node at the current pointer and then follows that node's own next pointer, so the node whose next pointer has
bit 14 set is still executed and ends the chain (and sets the end-of-list flag read at `$FFFFC5` bit 7). The
CONTROL action field accepts 1 start, 2 stop, 3 freeze and 4 unfreeze; the other values (0 NOP, 5 ABORT, 6 and
7) assert (`dsp/dsp_dma.c:349-369`, `dsp/dsp_dma_regs.h:30-36`), and the third read of CONTROL while RUNNING
returns STOPPED (`dsp/dsp_dma.c:323-331`).

A descriptor is 7 DSP words at the node address (below 0x1800 in X; 0x1800-0x1FFF in Y; 0x2800-0x37FF in P):
next pointer (bits 13:0, bit 14 = end of list), control, count, DSP address, scratch offset, scratch base and
size - 1. Control: bit 0 interleave, bit 1 direction (1 = DSP to memory), bits 3:2 and bit 13 must be 0, bit 4 offset
write-back, bits 8:5 buffer (0-3 FIFO out, 0xE circular scratch, 0xF linear scratch), bits 12:10 format.
Usable formats are 1 (16-bit, value >> 8), 2 and 6 (24-bit in a 32-bit word); format 0 decodes as 8-bit but
asserts when data is transferred, other values assert at decode. Memory-to-DSP transfers exist only for
non-interleaved scratch buffers; any FIFO read asserts, so GP output FIFOs cannot be fed to EP input FIFOs
through the DMA engine (`dsp/dsp_dma.c:103-316`). An EP output FIFO 0 transfer must be exactly 1024 bytes
(256 stereo int16) when the monitor point is EP or GP-or-EP (`dsp/gp_ep.c:185-196`).

**Instruction encodings** (24-bit words; all verified against the template table in `dsp_cpu.c:152-340`):

| Instruction | Word(s) | Source |
|---|---|---|
| `NOP` | `0x000000` | `dsp_cpu.c:310` |
| `RTS` | `0x00000C` | `dsp_cpu.c:329` |
| `WAIT` (no-op) / `STOP` (no-op) | `0x000086` / `0x000087` | `dsp_cpu.c:339,330` |
| `JMP xxx` (12-bit) | `0x0C0000 \| addr` | `dsp_cpu.c:253` |
| `JMP` long | `0x0AF080`, then the address word | `dsp_cpu.c:252` |
| `jcc xxx` | `0x0E0000 \| (cc << 12) \| addr` | `dsp_cpu.c:245` |
| `movep #imm,x:$FFFFC4` (end frame; derived from the `movep_23` template and `emu_movep_23`, never executed) | `0x08F484`, then `0x000001` | `dsp_cpu.c:297`; `dsp_emu.c.inc:7664-7701` |
| `move #imm,r0` / `r1` (derived from `emu_pm_5`) | `0x60F400` / `0x61F400`, then the immediate | `dsp_emu.c.inc:5606-5674` |

**Which instructions can run.** The template table has 187 entries (`dsp_cpu.c:152-340`); 106 have an
emulation function and 81 do not (for example `eor #xx`/`#xxxx`, `or #xx`, `lsr #ii`, `lsl S,D`, `lsr S,D`,
`mac`/`mpy S,#n,D`, `asl`/`asr S1,S2,D`, `clb`, `extract`, `insert`, `dmac`, `bsclr`/`bsset`, `trap`, `debug`
and `pflush`). A word that matches no template asserts (`dsp_cpu.c:409-421`), and so does a word whose template
has no emulation function (`dsp_cpu.c:628-633`, `dsp_emu.c.inc:28-38`; `exception_debugging` is set at reset,
`dsp_cpu.c:405`). In the parallel-move ALU table the slots 0x04, 0x08, 0x0C and 0x15 are undefined and assert
the same way (`dsp_emu.c.inc:5092-5132`). So matching a template is not enough: an instruction is usable only if
its template has an emulation function and, for words of 0x100000 and above, its ALU slot is defined. Decoding
happens when the word is executed, not when the image is loaded.

A minimal GP image that only ends each frame would be `0x08F484 0x000001 0x0C0000` (`movep #1,x:$FFFFC4`;
`jmp $0000`); it is derived by hand and was not run. With the EP disabled and the monitor at GP-or-EP,
xemu then plays mixbin 0 as left and mixbin 1 as right (`dsp/gp_ep.c:468-478`).

**How the repository's microcode words actually decode** (`apu_gp_ucode.h`; first-match against the
template table; words of 0x100000 and above by their parallel-move class):

| Word | Comment in the repository | xemu decode |
|---|---|---|
| `0x0AF080` + address | `JMP $addr` | `jmp` long (CORRECT) |
| `0x000000` | NOP | `nop` (CORRECT) |
| `0x00000C` | `WAIT` | `rts` |
| `0x5CE000` | `MOVE #$0000,r0` | class 5: `move y:(r0),a1` |
| `0x5CE1B0` | `MOVE #$FFB0,r1` | class 5: `mpy +y1,y0,a` with `move y:(r1),a1` |
| `0x0E4000`, `0x0E4900`, `0x0E4D00` | `MOVE x:(r0)+,a` / `MOVE a,y:(r1)+` / `MOVE b,y:(r1)-` | `jcc xxx`, condition 4, targets `$000`, `$900`, `$D00` |
| `0x0000F8` | `ORI #$001000,SR` (first word) | `ori #0,mr` (complete one-word instruction) |
| `0x001000` | (second word of the ORI) | matches no opcode: assert |
| `0x56F400` + `0x5A827A` | `MOVE #$5A827A,y0` | `move #$5A827A,a` |
| `0x08E040` | `MOVE x:(r0+4),x0` | `movep p:(r0),x:$FFFFC0` |
| `0x217000` | `MPY x0,y0,a` | class 2 register move `move b2,r0` (ALU byte 0x00) |
| `0x200018` | `MOVE a,b` | `add a,b` |
| `0x200008` | `ADD x0,a` | ALU slot 0x08 is undefined: assert |
| `0x200028` | `ADD x0,b` | `add x,b` |
| `0x217800` | `MACR x0,y0,a` | `move b2,n0` (ALU byte 0x00) |
| `0x217820` | `MACR x0,y0,b` | `add x,a` with `move b2,n0` |

The class-2, class-5 and ALU-slot readings come from `emu_pm_2`, `emu_pm_2_2`, `emu_pm_5` and the ALU table at
`dsp_emu.c.inc:5092-5132`; register names are those of `dsp_cpu_regs.h:51-100`. Neither listing can run:
the passthrough image reaches an `rts` with an empty stack (assert) after the valid reset jump, and the
downmix image executes `0x001000` (assert). Neither ever writes `X:$FFFFC4`.

### 4.10 Output path, monitor points and xemu configuration

* The APU thread builds frames (VP, then GP, then EP) and pushes finished 256-frame stereo int16 buffers
  (1024 bytes) into a 48 kHz S16 stereo SDL stream every 8th frame; it paces itself against the stream's
  queue level, not against the AC97 device (`apu.c:169-255`; `apu/monitor.c:22-84`).
* Where audio is tapped (`monitor.point`, `dsp/gp_ep.c:26-49`; `apu_debug.h:28-34`):

| Point | Selected by | Behaviour |
|---|---|---|
| VP | `audio.use_dsp` off (presumed default) | Voices are summed straight into the monitor buffer (each voice at the maximum of its eight output gains, ignoring bins) and the mixbins are then zeroed: **no GP or EP program is needed** and the DSPs should stay in reset (`vp/vp.c:1484-1522,1844-1857`) |
| GP-or-EP | `audio.use_dsp` on ("Real-time DSP processing", `ui/xui/main-menu.cc:847-850`) | GP and EP run in realtime. If the EP is not enabled the monitor reads `X:0x1400+i` (left) and `X:0x1420+i` (right) from the GP; if it is enabled the EP must DMA exactly 1024 bytes to its output FIFO 0, and xemu then writes silence into that FIFO |
| AC97 / GP / EP | debug UI "Monitor" combo (`ui/xui/debug.cc:204-208`) | AC97: the EP's FIFO 0 samples stay in guest RAM for a guest-programmed AC97 buffer list, and the SDL stream receives only silence (the monitor buffer is pushed every 8th frame without testing the monitor point, and nothing fills it in this mode; `apu/monitor.c:70-84`, `dsp/gp_ep.c:185-196`) |

* The AC97 device (ACI, PCI device 6, 10DE:01B1, BAR2 0x1000 bytes: mixer registers at +0x000-0x0FF, bus
  master at +0x100-0x17F) is independent of the APU: it pulls PCM-out data from guest RAM when the host audio
  backend asks, uses S16 stereo, and has registers but no data path for S/PDIF out (`hw/xbox/mcpx/aci.c:42-69`;
  `hw/audio/ac97.c:117-135,240-241,297,341-342,366-367,785,1026-1125`).
* Other xemu settings that matter: `audio.hrtf` (default true) switches HRTF filtering;
  `audio.use_dsp_jit` (default false) selects the external JIT engine (`config_spec.yml:316-322`);
  `sys.avpack` (default `hdtv`) sets the SMC pack code (`config_spec.yml:358-361`; `hw/xbox/xbox.c:501`).
  xemu defines three APU trace events: `mcpx_apu_method` (emitted by the front-end method handler, so not for
  the three asserting methods or for unlisted offsets), `mcpx_apu_reg_read` and `mcpx_apu_reg_write` (emitted by
  the BAR0 root handler: the main register file 0x00000-0x1FFFF, plus no-op accesses to the unmapped ranges
  0x40000-0x4FFFF and 0x60000-0x7FFFF). GP and EP window accesses, including `GPRST`, `EPRST` and the
  X/Y/P windows, have no APU event; DSP bring-up can use the DSP events `dsp_read_peripheral`,
  `dsp_write_peripheral`, `dsp56k_execute_instruction` and `dsp56k_execute_instruction_disasm`
  (`apu/trace-events`, `apu/dsp/trace-events`; `apu.c:62,71`; `vp/vp.c:168`).

---
## 5. What was wrong in the library at 639dcdd, by severity

"Consequence" is what happens if the library at 639dcdd drives xemu's APU model (derived from the code, not
observed). "Rows" points at the evidence rows in section 3. "Status in this branch" is the current state
(see 2.4 and 2.5): fixed, mitigated by the default null backend (the defect remains, but by default nothing
reaches an emulated or real APU), or open.

| Sev. | Defect | Consequence under xemu (at 639dcdd) | Rows | Status in this branch |
|---|---|---|---|---|
| critical | `alcOpenDevice` bring-up is built on a private register map; the first hardware access that matters is the microcode upload into `BAR0+0x20000` | **xemu aborts** (5.1 image: `assert(v < 256)` for handle 0x4000; stereo image: `assert(0)` at method 0x130 after VOICE_ON/OFF/RELEASE against `VPVADDR = 0` and `FENADDR = 0` scribbled guest RAM). All shipped samples that opened a device took the same path | C1.3c, C1.A1, C8.2 | **Mitigated**: the default null backend sends the upload to a RAM stand-in; still open with `-DOPENAL_APU_REAL_MMIO` |
| critical | The library starts no frame: `SECTL.XCNTMODE` is never non-zero, `GPRST`/`EPRST` are never written | If the upload were fixed, nothing would ever execute: no VP, GP or EP frame, no audio | C1.3, C5.1.5, C8.1 | **Open** (the default backend drives no APU; the opt-in path is unchanged) |
| high | Register collisions: 0x1000/0x1004 are ISTS/IEN, 0x2000 is SECTL, 0x1010-0x1024/0x2004/0x3000-0x3010 are unused storage | The voice-array address lands in IEN (all APU interrupts masked); `VPVADDR` stays 0, so any later voice method touches guest page 0 + handle*0x80; all "readbacks" echo the library's own writes (`AL_XBOX_VP_BASE_PHYS`, `AL_XBOX_DOLBY_DIGITAL_ACTIVE`, `apu_gp_is_running`) | C1.2, C1.2b, C1.A6 | **Mitigated** (the registers are RAM by default). The two status readbacks are **fixed** (cached state); the `apu_gp_is_running` helper still reads a plain-storage cell |
| high | No front-end method / voice-list model: voices are "started" by setting bits and by writing a 128-byte structure | Nothing the library writes starts, stops, pauses or positions a voice. The three list heads reset to 0 (voice 0), so the first frame would walk voice 0, find it inactive and raise `SE2FE_IDLE_VOICE` (assert unless `FETFORCE1` bit 15) | C2.1, C1.A3, C1.A8, C8.3, C8.8 | **Open** (mitigated by default) |
| high | One-shot completion is read from `VP_ACTIVE_0/1`, which are plain storage | One-shot sources never reach `AL_STOPPED`; finished voices hold their slot until `alSourceStop`; `samples/apu_tone` prints "RUNNING" from its own write; the host mock hides this | C1.A2, C2.A5, C2.6 | **Open**, partly addressed: `alXboxUpdateVoices()` is public, but on the null backend a one-shot stays `AL_PLAYING` until `alSourceStop` (README) |
| high | Volume polarity inverted (linear, 0 = silence) and wrong width/shape | On a real NV_PAVS layout zero means 0 dB: "zero the gains before stopping" would produce full-level audio, and a zero-filled voice sends eight 0 dB copies into bin 0 (for handles of 64 and above or with HRTF off; a handle below 64 with a zero HRTF handle is silent, 4.7) | C2.A2, C4.2, C4.2a | **Open** (model values; documented in the headers and README) |
| high | Pitch encoding is a linear 16.16 step | Only bits 31:16 of the method argument are stored: every library value below 0x10000 gives `p = 0` (48 kHz) and 0x10000 gives `p = 1`; sources at other rates play at the wrong speed | C2.A3, C3.5 | **Open** (documented) |
| high | The 128-byte `NVAPU_VOICE_CONTEXT_3D` is not NV_PAVS | A context written as defined would be misread: `master_vol` as HRTF handle, `hrtf_b2/a1` over `PAR_STATE`, `mixbin_gain[12..15]` over the next-voice link and pitch | C4.A1, C2.1 | **Open** (documented in the `apu_voice.h` banner) |
| high | Buffers are PRD chunk lists with byte offsets and an EOT bit | xemu reads a global per-page SGE table, a byte-offset `BA` and sample-frame indices; a PRD-shaped table makes SSL assertions fail or reads wrong pages; chunk sizes of 65535 bytes split 16-bit samples across chunks | C3.4, C3.A1, C8.4 | **Open** for the PRD model; the chunk-size bug is **fixed** (0xF000) |
| high | GP microcode listings are not valid DSP56300 as commented, use a wrong address map and never end a frame | Passthrough: `rts` on an empty stack asserts. Downmix: `0x001000` asserts. A program that never writes `X:$FFFFC4` hangs the APU thread in realtime mode | C5.2-C5.3.3, C5.A1 | **Open**: documented in the `apu_gp_ucode.h` banner; the upload still happens, into the RAM stand-in by default |
| high | The `__NXDK__`/`_XBOX` guards in `apu_eeprom.c`/`apu_mem.c` were never true under `nxdk-cc` | Hardware queries compiled out (always STEREO flags and STANDARD pack, topology always STEREO_20); the "contiguous pool" was a malloc'ed host pool with the fake physical base 0x01000000, so addresses given to the APU were not physical | C6.A1 | **Fixed** (`apu_platform.h`) |
| high | AV-pack codes are compared against the kernel's `AV_PACK_*` enum | With xemu's default HDTV pack (raw 1) the library reads "standard"; raw 6 (composite) reads as S-Video and is wrongly optical-capable; HDTV never reaches 5.1 | C6.2 | **Fixed** (`apu_av_pack_from_smc()`) |
| medium | `FENADDR` is never programmed | Every voice end writes two bytes at `16*(2 + 4*voice + n) + 14/15` from address 0: corrupts low guest RAM | C1.A7 | **Mitigated** by the null backend; open with `-DOPENAL_APU_REAL_MMIO` |
| medium | Routing and 3D placement ignore the eight-output model; all sources use slots below 64 | For handles < 64 bins 0-3 are forced to the HRTF submix bins (all bin 0 at reset); a non-null HRTF handle without a latched entry is silent; stereo/music sources cannot be steered | C2.A7, C4.1e, C4.A4, C4.5 | **Open** |
| medium | Stereo PCM is configured without `SAMPLES_PER_BLOCK = 1` | Right sample of each frame overlaps the next left sample (inference) | C2.A9, C3.2 | **Open** |
| medium | HRTF is modelled as a Q14 biquad plus ITD 0..64 and a RAM buffer | Nothing of it can be expressed in xemu: 31-tap FIRs per ear and one +/-42-sample ITD per table entry must be generated and loaded | C4.1-C4.1d | **Open** (documented) |
| medium | Restarting a source re-issues VOICE_ON; completion/stop never unlinks | A second VOICE_ON on a linked voice makes the list cyclic (the frame walk follows it for up to 256 steps and can overflow the work queue, C2.A4); a finished voice left linked traps the front end every frame | C2.A4, C8.8 | **Open**, partly addressed: a replay now halts the voice first; there is still no list model |
| medium | Position model is `current_prd_index` + `sample_pos_frac` | CBO is a 24-bit sample index; VOICE_ON zeroes it; a preemption resume must write CBO after VOICE_ON; no fractional phase is guest-visible; GET_VOICE_POSITION aborts | C2.4, C3.A3 | **Open** |
| medium | Voice table is 8 KiB for 64 voices and not programmed into `VPVADDR` | Handles >= 64 (plain 2D voices) index past the table; xemu does no bounds check | C1.A5 | **Open** |
| medium | Mixbin 31 is a legal-looking "unused" bin | Routing any of a normal voice's eight outputs to bin 31 can trip multipass scheduling asserts | C2.A11 | **Open** |
| medium | AV-pack tokens: `AL_XBOX_AV_PACK_COMPOSITE` = 0x05; tests and sample labels assume kernel numbering | Wrong token for composite (SMC code 6); suite passes while decoding is wrong | C6.2b, C6.A3 | **Fixed** (composite is 0x06; tokens equal the raw SMC codes; `test_av_pack_decode`) |
| medium | `AL_EXT_MCFORMATS` advertised, multichannel buffers rejected; queueing, offsets and capture are stubs | Applications that trust the extension string or use streaming fail at run time | C7.A1, C7.A2 | **Fixed** for the extension string; the stubs (queueing, offsets, capture) remain and are documented in the README |
| low | ITD range 0..64 samples (formula peaks at 31), "polyphase resampler", loop offsets in bytes, `EBO` as one-past-end | Numbers differ from xemu's model (+/-42 samples, libsamplerate sinc of unknown hardware equivalence, sample-unit inclusive `EBO`) | C4.1d, C3.6, C3.7 | **Open** (the README states 0-31 samples and the xemu limit) |
| low | Tests assert register values in mock memory and the linear pitch/volume encodings | They pass while hardware behaviour is wrong; a switch to the real encodings breaks them (intended) | C2.A5, C3.A4, C6.A3 | **Open** (new tests cover the decode, guards and backend, not xemu behaviour) |
| low | `lib/openal/tests/run_tests.py` and `Makefile` could not run on Linux; `*.a` not ignored | 3 of 21 tests passed via `run_tests.py` (18 failed to link on libm); `make test` could never pass | C7.5, C7.A5 | **Fixed** |

README claims that this analysis does not support (fixed by the README rewrite in this change):

| README claim (old) | Status |
|---|---|
| "Zero CPU Mixing Overhead ... processed directly on the APU VP and GP" | Architecture plausible (xemu does mixing in the VP model), but not demonstrated for this library; panning, ITD and HRTF parameters are computed on the CPU; its register layer does not match |
| "Voice Virtualization ... 256 virtual sources over 64 hardware voice contexts" | Self-imposed limit (xemu runs 256 handles at once) |
| "Click-Free Preemption ... preserving fractional sample playback phase and PRD indices" | No hardware ramp verified; no fractional phase or PRD index exists in xemu's model |
| "Woodworth ITD (0-64 samples) and Q14 Butterworth HRTF biquad" | Model values (the formula never exceeds 31 samples; the README now says 0-31); xemu's VP uses a 31-tap FIR plus +/-42-sample ITD |
| "hardware Dolby Digital DSE real-time bitstream encoding over optical TOSLink" | No such register or encoder in xemu or the library; unverifiable on hardware |
| "ITU-R BS.775 downmixing with DSP56300 saturation limiting" | Host C arithmetic sound; the microcode is invalid |
| "Verified Performance (Pentium III 733.333 MHz)" table | Not produced by any code or log; "verified" is unjustified |
| "17 automated regression test suites covering 100 % of the hardware abstraction and OpenAL 1.1 APIs" | Stale count; measured line coverage 77.2 % at 639dcdd (79.8 % in this branch); 40 of the 94 functions declared at 639dcdd were only smoke-tested |
| "OpenAL 1.1 implementation" | A subset: no buffer queueing/streaming, no `AL_*_OFFSET`, capture stubbed |

---

## 6. UNVERIFIABLE items and assumptions

Nothing below can be settled from xemu. Each is either an open fact about silicon or a library assumption
that must be marked as such wherever it appears.

| Item | Why it cannot be settled | Where it matters |
|---|---|---|
| **BAR0 base `0xFE800000`** (and the AC97 base `0xFEC00000`) | The BIOS assigns BARs; xemu has no APU or ACI base literal under `hw/xbox`, `hw/audio` or `ui/` (`0xFEC00000` occurs elsewhere in the tree only as the unrelated IO-APIC default, `include/hw/intc/ioapic.h:24`). `lib/hal/audio.c:156` also hard-codes the AC97 base | `NV_PAPU_BASE`; consider reading BAR0 from PCI config space (bus 0, device 5, function 0, offset 0x10) |
| **Which AV packs have a TOSLink output** (library assumes HDTV, S-Video, SCART) | xemu models the pack only as an SMC ID byte and a video encoder choice | `apu_is_optical_pack_connected`, topology choice |
| **`XC_AUDIO_FLAGS_*` values** and the meaning of a zero audio dword | Only this repository defines them; xemu's generated EEPROM leaves the audio dword at zero without defining it | `apu_eeprom.h`, `apu_detect_audio_topology` |
| **EEPROM defaults on real consoles** | xemu generates a default EEPROM and populates only 44 bytes of the user section plus a language dword | topology decision |
| **Whether AC-3 / Dolby Digital is produced by firmware on the EP** and how the stock images route GP output to the EP | xemu has no encoder and no stock images; GP output FIFOs are never read back by DMA | any Dolby claim; roadmap |
| **Real-hardware AC-3 / S/PDIF path** | xemu's S/PDIF-out engine accepts registers and transfers nothing | 5.1 / TOSLink features |
| Non-zero `SECTL.XCNTMODE` encodings | xemu defines only OFF and treats any non-zero value as "run" | step 17 uses `0x08` only because it satisfies xemu |
| ADPCM samples per block (64 or 65) | xemu advances 64 and carries a FIXME | ADPCM support |
| Resampler interpolation method | xemu uses libsamplerate sinc and says the real method is unknown | "polyphase resampler" wording |
| `GET_VOICE_POSITION` behaviour; whether silicon has `ACTIVE`/`PAUSE` bitmask registers | xemu asserts / has nothing at those offsets | position and completion design |
| Hardware ramping of `TAR_*` volumes | xemu applies them as a step | any "click-free" claim |
| Roles of mixbins 0..5 (FL, FR, SL, SR, C, LFE) and which HRTF submix bin is left or right | xemu attaches no names; only bins 0/1 are used as L/R by the GP monitor | spatial code, GP images |
| Where ITD history lives; the true ITD limit (xemu clamps at +/-42) | host-side in xemu | HRTF design |
| FIFO base/end/cur units (xemu uses the raw low 24 bits; a FIXME suggests hardware may use a shifted field) | `apu_regs.h:81` | GP/EP FIFO setup |
| `SAMPLES_PER_BLOCK` semantics beyond xemu's stride arithmetic | inferred from vp.c | stereo and ADPCM voices |
| Notifier and interrupt semantics (notify enable, IN_PROGRESS status, `SET_CONTEXT_DMA_NOTIFY`) | partial in xemu, three methods assert | completion handling |
| Which handle ranges stock software uses for 2D versus 3D voices, and whether the list split is a hardware rule or a DirectSound convention | xemu keys behaviour on handle < 64 and treats lists as a convention (open TODO at `vp/vp.c:1493-1498`) | voice allocator |
| Alignment and memory-type requirements for the tables (4 KiB alignment, write-combined memory) | xemu uses plain physical RAM and enforces nothing | `apu_mem` |
| `GPRST` bits 2 and 3 (`NMI`, `ABORT`) and saturation-mode (SM) semantics of the DSP | unimplemented / no such bit in xemu's core | DSP firmware |
| Interpretation of `audio.use_dsp` default | no explicit default in `config_spec.yml:316` | test plans |
| JIT DSP backend behaviour | external library not in the checkout | DSP tests with the JIT |
| DirectSound's actual method ordering and envelope start phases | not in xemu | porting plan ordering is a conservative choice, not an observed sequence |
| OpenAL IMA4 byte-compatibility with Xbox ADPCM | recollections conflict; neither xemu nor this repository decides it | ADPCM via `AL_EXT_IMA4` |

---

## 7. Porting plan

### 7.1 Routes

* **Route A (default xemu configuration, `monitor = VP`).** One 2D voice, no DSP code, no AC97 programming.
  The DSPs stay in reset. Output goes straight to the host stream. This is the minimal first target.
* **Route B (`audio.use_dsp` on, `monitor = GP-or-EP`).** Needs a GP program that ends every frame by writing
  bit 0 to `X:$FFFFC4`; the EP is optional.
* **Route C (debug monitor `AC97`).** Needs GP and EP programs, an EP output FIFO 0 buffer and guest AC97
  descriptors that point at the same pages. Not recommended as a first goal.

### 7.2 Bring-up sequence (25 steps)

Derived from the xemu source for one mono PCM16 one-shot voice; steps 14-16 and 25 apply to routes B and C
only. "Re-derived" values were recomputed while writing this document; the ordering beyond "configure first,
VOICE_ON last" is a conservative choice (DirectSound's ordering is UNVERIFIABLE). Not executed.

1. **Find and map the devices.** APU: PCI 0:5.0, 10DE:01B0; ACI: 0:6.0, 10DE:01B1 (BAR2). Use aligned
   32-bit accesses everywhere (GP/EP windows assert it).
2. **Hook the interrupts (optional).** APU INTA is ISA IRQ 5, ACI is IRQ 6. The line is re-evaluated by an
   ISTS write and by the frame thread when it raises its irq flag (4.2: within about 5 ms while frames are off
   or trapped, otherwise only after a notifier write or an idle-voice trap); follow an IEN write with an ISTS
   write of 0 so a pending bit is not left un-raised.
3. **Pick the route** (7.1). No register reports the host configuration (inference from the sources read).
4. **Allocate zero-filled guest RAM:** voice array `(max_handle + 1) * 0x80` bytes (use handles >= 64 for plain
   voices, so at least 0x2080 bytes; 0x8000 for all 256); VP SGE table (8-byte entries, one per 4 KiB page of
   the linear space; xemu does not bound the index); SSL table only for streams; notifier array
   `16 * (2 + 4 * (max_handle + 1))` bytes (0x4020 for 256 voices); sample pages; routes B/C: GP and EP scratch
   and FIFO SGE tables plus their pages. xemu enforces no alignment and no memory type.
5. **Method rules.** A method is one 32-bit write to `BAR0 + 0x20000 + offset`, executed synchronously. There is
   no queue; `0x010` always reads 0x80. Never write `0x130`, `0x180` or `0x18C`. Every method overwrites
   `FEDECMETH`/`FEDECPARAM`. Lock a voice (`SET_CURRENT_VOICE`, `VOICE_LOCK 1 ... 0`) while editing one that is
   active, and keep the window short: a locked queued voice stalls the whole VP frame. The method handlers
   that take the device lock (`VOICE_LOCK`, `VOICE_ON`, `VOICE_RELEASE`) may block the guest CPU while a frame
   is being built (inference from `apu.c:257-303`).
6. **Quiesce.** `SECTL = 0`, `FECTL = 0`, `GPRST = 0`, `EPRST = 0`, `IEN = 0`, `ISTS = 0xFFFFFFFF`. Device
   reset does not clear the GP/EP register files or DSP memories.
7. **Program the VP table bases:** `VPVADDR`, `VPSGEADDR`, `VPSSLADDR` (even if unused).
8. **Routes B/C:** `GPSADDR`, `GPFADDR`, `EPSADDR`, `EPFADDR` and the four `*MAXSGE` registers (inclusive highest
   index; the bootstrap image spans two pages, so at least 1).
9. **Empty the three voice lists before frames start:** `TVL2D = TVL3D = TVLMP = 0xFFFF`.
10. **Enable the idle-voice trap:** `FETFORCE1 = 0x00008000`. Without it xemu asserts on the first finished
    voice that is still linked.
11. **Point the notifier area at guest RAM:** `FENADDR = phys(notifier array)`. On voice end xemu writes
    byte `FENADDR + 16*(2 + 4*voice + n) + 15` = 0x01 (n = 0 for buffer voices) and the byte before it = 1,
    and sets ISTS bits 5 and 6. A write to `FEMEMDATA` also stores its value at the physical address in `FEMEMADDR` (4.2); xemu's comment suggests software uses this when notifies complete, but the purpose is unverified.
12. **Interrupts (optional; ISTS bits latch regardless of IEN):** `IEN = 0x71` (bit 0 plus bits 4, 5, 6), then
    `ISTS = 0`. FETINTSTS is re-raised while `FECTL & 0xE0` is non-zero, so clear the trap before clearing
    ISTS bit 4.
13. **Optional mixer setup:** `SET_SUBMIX_HEADROOM[bin]`, `SET_HRTF_HEADROOM`, `SET_HRTF_SUBMIXES`, and the HRTF
    table (`SET_CURRENT_HRTF_ENTRY`, `SET_HRIR` x 15, `SET_HRIR_X`) for voices below 64.
14. **Routes B/C - GP.** Either (a) place the image (the first 0x800 words as little-endian 32-bit words,
    top byte 0) in GP scratch space at linear offset 0 behind a valid SGE table, then write `GPRST = 0` and
    `GPRST = 3`; or (b) with `SECTL.XCNTMODE` still OFF (so no frame runs), perform the 0-to-3 transition
    with a valid, even all-NOP, scratch image and write the program through the P window
    (`BAR0 + 0x3A000 + 4*addr`) afterwards (inference from `dsp/gp_ep.c:251-260`, `dsp/dsp_c.c:89-104`,
    `apu.c:270-296`). Writing P before the transition is overwritten for words 0..0x7FF. The program must
    satisfy the contract in section 4.9: read mixbins at `X:0x1400 + 32*bin`, end every frame with
    `movep #1,x:$FFFFC4`, use only DMA formats 1/2/6 (writing the chain head to NEXT_BLOCK, not START_BLOCK), and contain only
    instructions whose template has an emulation function and whose parallel-move ALU slot is defined (4.9). Candidate image
    (derived, never run): `0x08F484, 0x000001, 0x0C0000`.
15. **Routes B/C - GP output FIFOs** (only if the firmware DMAs to FIFO buffers 0-3): program `GPOFBASEn`,
    `GPOFENDn`, `GPOFCURn` before releasing reset, with `BASE <= CUR < END`.
16. **Route C - EP:** image in EP scratch at offset 0, `EPRST = 0` then `3`; EP output FIFO 0 needs
    `BASE < END` and must receive exactly 1024 bytes per run (256 stereo int16).
17. **Start frame generation:** `SECTL = 0x00000008` (any non-zero `XCNTMODE`; only OFF is defined) and keep
    `FECTL.FEMETHMODE` = 0. Do this only after steps 7-12 (and 14-16 for routes B/C).
18. **Map the sample buffer in the SGE table:** one entry per 4 KiB page, `SGE[k] = {phys(page k), 0}` (or via
    methods `0x804`/`0x808`). `BA` is a linear byte offset (`k0 * 4096 + (buffer_phys & 0xFFF)`); keep `BA`
    a multiple of the frame stride so no frame straddles a page.
19. **Configure the voice** (handle `h` in 64..255; values for a mono PCM16 one-shot at 48 kHz, outputs 0 and 1
    at 0 dB, everything else muted, no envelopes, no filter):

    | Method | Value |
    |---|---|
    | `0x2F8` SET_CURRENT_VOICE | `h` |
    | `0x300` CFG_VBIN | `0x00000020` (V0BIN = 0, V1BIN = 1) |
    | `0x304` CFG_FMT | `0x50000000` (S16, B16, mono, buffer, no loop; add `1 << 25` to loop; `SAMPLES_PER_BLOCK` field 0) |
    | `0x308`, `0x30C`, `0x310`, `0x314`, `0x318`, `0x36C` ENV0/ENVA/ENV1/ENVF/MISC/LFO_ENV | `0` (`EF_PITCHSCALE` must be 0) |
    | `0x31C` TAR_HRTF | `0x0000FFFF` (required for handles below 64) |
    | `0x360` / `0x364` / `0x368` TAR_VOLA/B/C | `0x000F000F` / `0xFFFFFFFF` / `0xFFFFFFFF` |
    | `0x374` / `0x378` TAR_FCA/FCB | `0` |
    | `0x37C` TAR_PITCH | `(uint32_t)(uint16_t)p << 16`, `p = round(4096*log2(F/48000))` (44.1 kHz: `0xFE0B0000`) |
    | `0x3A0` CFG_BUF_BASE | `BA` |
    | `0x3A4` CFG_BUF_LBO | loop-start sample (0) |
    | `0x3DC` CFG_BUF_EBO | `N - 1` for `N` samples |

    Write every field explicitly: the full-dword methods overwrite, but stale bits survive in the fields no
    method touches (the next-voice link, `PAUSED`). Set `SAMPLES_PER_BLOCK` = 1 for stereo PCM16.
20. **Link and start:** `0x120 SET_ANTECEDENT_VOICE = 0x00010000` (2D top; always write it first: `FEAV`
    resets to "inherit after voice 0"), then `0x124 VOICE_ON = h` (envelope start phases 0, i.e. `0x40` for
    `h = 64`). Never issue VOICE_ON for a voice that is already linked. `VOICE_OFF 0x128`, `VOICE_RELEASE
    0x12C` (fade) and `VOICE_PAUSE 0x140` (`h | pause << 18`) are the other control methods.
21. **What xemu does per frame (informational).** For each list (2D, 3D, MP): `CVL = TVL`, walk the next links
    (at most 256 steps), call `SE2FE_IDLE_VOICE` for inactive voices, queue active ones; voices are processed on
    worker threads; the GP runs every frame, the EP every 8th; every 8th frame 256 stereo frames go to the
    host stream. The guest never pulls frames.
22. **Detect completion**, fastest first: voice RAM (`CBO` at +0x58 advances; `ACTIVE_VOICE`, +0x54 bit 21, drops
    at the end of a one-shot); the notifier byte; ISTS bits 5/6; then the trap in step 23.
23. **Handle the idle-voice trap** (mandatory once `FETFORCE1` bit 15 is set): on ISTS bit 4 (or by polling
    `FECTL & 0xE0`) read `FEDECPARAM` (the handle; `FEDECMETH` = 0x8000) **before** any other method; re-read
    `PAR_STATE.ACTIVE_VOICE` and unlink only if it is 0 (VOICE_ON links before it sets the flag, so a frame can
    see a voice mid-start; inference); unlink by editing the predecessor's next-handle field (voice +0x7C, bits
    15:0, preserving the pitch bits) or the list head; then `FECTL = 0` and `ISTS = 0x10`. Only the last
    idle handle of a frame is reported; expect further traps. No method removes a voice from a list.
24. **Stop and tear down:** `VOICE_OFF` (or `VOICE_RELEASE`) for each live voice, service the trap so it is
    unlinked, then `SECTL = 0`, `IEN = 0`, `GPRST = 0`, `EPRST = 0`, `ISTS = 0xFFFFFFFF`; free the tables only
    after that.
25. **Route C only - AC97 stage:** `lib/hal/audio.c` already programs the register layout xemu's ACI accepts
    (PO engine at ACI + 0x110, `LVI` 0x115, `SR` 0x116, `CR` 0x11B; buffer descriptor `{addr, ctl|length in
    16-bit samples}` with IOC bit 31); point the descriptors at the pages the EP FIFO 0 maps. xemu does not
    link the EP to AC97; this works only through shared guest RAM and only in the debug `AC97` monitor mode.

### 7.3 xemu configuration notes

* Default (`audio.use_dsp` presumably off): Route A is audible; GP and EP code is not (the mixbins are zeroed
  after the VP stage). Enable "Real-time DSP processing" (`audio.use_dsp`), or use the debug window's Monitor
  selector and Realtime checkboxes, to hear GP/EP output.
* With `audio.use_dsp` off the DSPs get only one 1000-cycle quantum per frame (`dsp/gp_ep.c:463-465`), so
  firmware that needs more than that cannot finish a frame in that mode (whether stock firmware does is
  unknown); with it on, a firmware that never halts hangs the APU thread. Realtime is not tied only to
  `audio.use_dsp`: the debug window has independent GP and EP "Realtime" checkboxes and a Monitor selector
  (`ui/xui/debug.cc:204-222`; `apu/debug.c:46-64`). A word that matches no template, whose template has no
  emulation function, or that uses an undefined ALU slot aborts xemu when the DSP executes it, in realtime and
  non-realtime mode alike (4.9).
* `audio.hrtf` must be on and the voice's HRTF handle non-null for HRTF to run.
* xemu cannot validate 5.1, LFE or Dolby/TOSLink output: its monitor and AC97 outputs are stereo.
* Asserts are always active: a single wrong register write can end the emulator, so bring-up should proceed
  one step at a time with xemu's trace events enabled. `mcpx_apu_method`, `mcpx_apu_reg_read` and
  `mcpx_apu_reg_write` cover the front-end methods (except the three asserting ones) and the BAR0 root handler
  (main register file, plus no-op accesses to the unmapped holes); the GP/EP windows, including the `GPRST`/`EPRST` bootstrap, are not traced by them, so use the
  `dsp_*` events (4.10) for DSP bring-up.

### 7.4 Open questions

1. The stock GP and EP images: what do they do, and how does stock GP hand samples to stock EP given that the
   DMA engine cannot read FIFOs?
2. Non-zero `SECTL.XCNTMODE` encodings and any other setup-engine bits real hardware needs.
3. Whether stock DirectSound points the AC97 buffer list at the EP output FIFO memory.
4. FIFO register units on hardware.
5. `SAMPLES_PER_BLOCK` semantics.
6. Notifier/interrupt semantics that xemu only partly implements.
7. Which handle ranges stock software assigns to 2D and 3D voices.
8. Memory attributes and alignment on real hardware.
9. xemu quirks a driver may observe but must not rely on: the last sample at a frame boundary, the lost final
   frame of a one-shot (pad one-shot buffers with at least 64 samples of trailing silence if the tail matters),
   `VOICE_ON` resetting CBO, a possible cycle-budget growth in the C interpreter after long runs (not
   investigated).
10. JIT-engine behaviour for GP/EP firmware.

---

## 8. Roadmap

### 8.1 The decision the maintainers must make

The library's value proposition is "hardware voices for OpenAL on the Xbox". Two incompatible ways forward
exist, and the choice decides what the next months of work are.

| | **A. Faithful hardware driver** | **B. AC97 + software mixer** (reuse `lib/hal/audio.c`) |
|---|---|---|
| What it is | Rewrite the register layer on the real MCPX model (sections 4 and 7): NV_PAVS records, front-end methods, voice lists and idle-trap service, SGE allocator, notifiers, HRTF table, and GP/EP firmware where needed | Mix on the CPU: resample, pan and sum sources into 48 kHz S16 stereo and feed `XAudioInit/XAudioProvideSamples` (the existing AC97 PCM-out ring, whose register layout matches xemu's ACI) |
| Pros | Real offload of mixing, resampling, envelopes and HRTF; up to 256 voices (64 with HRTF); one model for xemu and, presumably, silicon; the library's name finally matches what it does | Works on xemu and hardware with an existing, small driver; deterministic and fully testable on the host (the library's spatial math becomes audible instead of being written to unused registers); no asserts that can end the emulator; no dependence on unknown DSP images; easy to ship and debug |
| Cons | Large rewrite touching every module; DSP firmware beyond xemu's VP-only route needs images that are not available (stock images unknown, hand-written DSP56300 code is error-prone); several behaviours are UNVERIFIABLE from xemu and need real hardware; mock-MMIO tests cannot catch register mistakes (an xemu-based test path is needed); a wrong write aborts xemu; no way to validate Dolby/5.1/TOSLink in xemu | Costs CPU time (the very thing the old README claimed to avoid; the library's own software spatial math is already measurable with the benchmark); no hardware HRTF, no Dolby/AC-3 unless a software encoder is added; latency and mixing quality are the library's responsibility |
| Risk | High: much of the work is unverifiable from xemu alone | Low |
| What stays valid | PCI identity, 24-bit DSP word packing, `NOP`/`JMP` opcodes, U8 sample handling, host spatial math | The OpenAL API layer, the source/buffer/listener logic, spatial math, voice manager |

Recommendation (a judgment, not a verified fact): do **B** first as the default, shippable backend, because
it is the only route that can be validated end to end today, and pursue **A** as an opt-in experimental backend
(behind `-DOPENAL_APU_REAL_MMIO`, which already isolates the real-MMIO path) staged as below. The two are not
exclusive; the backend selection introduced alongside this document (`AL_XBOX_BACKEND`,
`-DOPENAL_APU_REAL_MMIO`, see `README.md`) separates them. Whatever is chosen, keep the
documentation honest: `README.md` states the current scope, and this document is the reference for anything
hardware-related.

### 8.1b Real-hardware observations (samples/apu_probe, rounds 1-17, one retail console, booted from the dashboard)

Measured on silicon by reading registers back; not inferred from xemu. Each finding names the probe round
(the logs are `E:\apu_probe*.txt` on the console).

* **State left by the dashboard (DirectSound):** `SECTL = 0x00000007`, `FECTL = 0x0007138F`, `IEN = 0xD8`,
  `GPRST = 0`, `EPRST = 1`, `FETFORCE1 = 0x8000`, table bases at 16 KiB-aligned addresses
  (`VPVADDR 0x03680000`, `VPSGEADDR 0x0367C000`, `GPSADDR 0x03648000`, `EPSADDR 0x03630000`, ...). The GP/EP
  memories still hold DirectSound's images after the dashboard hands over (round 4).
* **PIO queue:** method writes go to a 32-entry queue mirrored at `BAR0 + 0x1400-0x14FF` as
  `{0x000A0000 | method, value}` pairs; `0x1340` holds put (bits 23:16), get (15:8) and the pending count (7:0);
  `PIO_FREE` (`0x20010`) counts free entries x 4 (0x80 when empty) (rounds 1-3). DirectSound also uses methods
  `0x104` and `0x10C` (its last two before hand-over: `0x10C = 0`, `0x104 = 2`), which xemu does not implement.
* **The front end executes exactly one queued method and then stops consuming the queue**, whatever the
  method (`0x804`, `0x2F8`, `0x104`), with frames on or off, in every `FECTL` method mode (bits 7:5), with
  `SECTL` stage bits 0-2 in any combination, with the DSPs in reset or running our own frame-ending loop, and
  after sending `0x104 = 0` first (rounds 1-9). No `ISTS` bit is raised. Unresolved; this blocks every VP use
  on hardware. Not explained yet; candidates: the semantics of `0x104`/`0x10C`, a per-method acknowledge
  (trap/interrupt protocol, `FETFORCE1` bit 15), or a GP frame-end handshake that differs from xemu.
* **`SECTL`:** bits 4:3 (`XCNTMODE`) = 1 runs the sample counter `XGSCNT` at ~48 000 per second; 2 and 3 do not
  advance it. Bits 2:0 are set by DirectSound (meaning unknown). With bits 2:0 = 7 and frames on, the front end
  once reported decode `0x8008` with parameter 1 and set `FECTL` bit 15 (round 2).
* **`FECV` does not change on `SET_CURRENT_VOICE`** on hardware; `FEDECMETH`/`FEDECPARAM` do show the method
  the front end decoded (bit 25 of `FEDECMETH` set in method mode `0xE0`) (rounds 3, 5).
* **Table bases are 16 KiB-aligned:** `VPSGEADDR`/`VPSSLADDR` read back with bits 13:0 cleared (round 1);
  `VPVADDR`, `GPSADDR` and `EPSADDR` accepted 16 KiB-aligned values unchanged (rounds 6, 8).
* **DSP loading:** with `RST = 0` the P window reads 0 and ignores writes; with `RST = 1` (core held) P and X are
  readable and writable through the window; the transition to `RST = 3` **runs the bootstrap** and overwrites
  `P:0..` from the scratch SGE table, as in xemu (round 7, where a stale `GPSADDR` reloaded DirectSound's image).
  Loading our image through our own 16 KiB-aligned scratch SGE table works: both DSPs read back
  `08F484 000001 0C0000` after release and while frames run (round 8). The probe holds a DSP in reset if its
  program memory is not ours, so no other code runs.
* **The FE advances on `FECTL` method-mode writes (round 10).** After the one automatic method, writing
  `FECTL` method mode `0xE0` (TRAPPED in xemu's naming) and then `0x80` (HALTED) made the front end consume
  exactly one more queued method. The dashboard leaves the mode at `0x80`. Writing mode `0` (FREE_RUNNING), clearing
  bits 12:8, the xemu trap-service sequence, `ISTS`/`IEN`/`FETFORCE1` writes did not.
* **Stepping rule (round 11):** each write that *changes* the method mode to `0x80` consumes exactly one entry
  (from `0xE0`, `0x00` or any other mode); rewriting `0x80` while already halted, and every other mode value,
  consume none. Stepped this way, 21 voice methods were all consumed and decoded (`FEDECMETH` shows them), but
  the voice record in RAM stayed all zero and `TVL2D` stayed `0xFFFF`: in halted mode the step decodes a method
  for software without executing it. In free-running mode with frames on, nothing is consumed (rounds 10, 11).
* **Free-running FE raises an internal message (round 12; also seen in round 2):** with `XCNTMODE = 1`, mode 0
  and the DSPs loaded, the FE consumed one method, then `FEDECMETH` read `0x(8)008` with parameter 1 and
  `FECTL` bit 15 set, and the queue stopped. xemu's only internal message is `SE2FE_IDLE_VOICE = 0x8000`; this
  looks like another setup-engine-to-front-end message that software must service. Its meaning is unknown.
* **DSP frame order (round 12, explained by xemu PR #3047, which was measured on silicon):** a DSP loop that waits for
  `START_FRAME` before writing `x:$FFFFC4 = 1` counts no frames, because a latched start is only released by a
  frame-complete write. With the loop `movep #1,x:$FFFFC4` / `jclr #1,x:$FFFFC5,*` / `movep #2,x:$FFFFC5`,
  `XCNTMODE = 1` gives **151 GP and 19 EP frames per 100 ms** (1500/s and 1500/8 per second), none with frames off; the DSPs
  run whatever the FE method mode is (round 13). With the DSPs completing frames the `0x8008` message no longer
  appears, but the free-running FE still executes one method (`SET_CURRENT_VOICE 64`) and stalls on the second;
  writing `FECTL` bit 15 does nothing. Round 14 diffs the FE registers around the stall and retries the
  `FECTL` low-nibble and bits 18:16 combinations now that frames really run.
* **Round 14:** bus mastering is on (PCI command `0x0006`). None of the 50 `FECTL` low-nibble x bits 18:16
  combinations in mode 0 moves the queue (bits 18:16 read back 7 whatever is written). **`FEMEMADDR` (`0x1324`)
  advances by 4 on its own**: on the halted -> free-running switch and on each method (`0x03688010`, `...14`, ...
  `...24`, inside DirectSound's notifier area at `FENADDR = 0x03688000`, which we do not own). A `FEMEMDATA`
  write stores the value at `FEMEMADDR` (our word received `0x12345678`) and advances the pointer by 8. The
  notifier block at our `FENADDR` stays zero. Registers `0x120C`, `0x1218` and `0x121C` change all the time (counters). Probes
  must point `FEMEMADDR` at their own memory first (round 15 does).
* **`FEMEMADDR`/`FEMEMDATA` is a CPU memory port with auto-increment, not FE activity (round 15).** With the
  pointer in our own buffer, it stays put through the free-running switch and the methods; it only advances on
  `FEMEMDATA` accesses (a write stores the word and advances by 4; round 14's register snapshot read it, which
  is why it moved there). The FE touches no memory through it. Filling the buffer with 0, 1, 2, `0xFFFFFFFF`,
  `0x40`, `0x80000000` or `0x01000000` does not release the queue. Also: DirectSound's freed notifier page
  is unmapped, so reading it through `0x80000000 | phys` faults (the first round 15 attempt hung there).
* **The VP plays a voice whose record and list head the CPU wrote directly (round 16).** With frames on
  (`XCNTMODE = 1`, DSPs running the complete-then-wait loop), a record built in RAM as xemu's methods would leave
  it (VBIN `0x20`, FMT S16/B16/loop `0x52000000`, HRTF target `0xFFFF`, BA = PCM offset in the SGE space, EBO
  47999, VOLA `0x000F000F`, VOLB/VOLC muted, PITCH_LINK `0x0000FFFF`, `PAR_STATE` = `ACTIVE_VOICE`) plus
  `TVL2D = handle` plays: `CBO` advances by `0x12E0` = 4832 samples per 100 ms (48 kHz at pitch 0), with the
  FE halted or free-running, on the 2D or the 3D list, with or without `NEW_VOICE`. The hardware writes back the
  current volumes (`0x28`-`0x30` = the target volumes), the current physical page at `0x38` (PCM base + page of
  `CBO`), `PAR_STATE = 0x00E00000` (bits 23:21) and `EALVL = 0xFF`. `CVL2D` follows the list head; list-head
  writes stick with frames off. **So the VP works without the FE**: until the FE stall is understood, the
  driver can manage records and list heads from the CPU.
* **End-to-end audio on hardware (round 17, heard on the console).** GP program: `movep #1,x:$FFFFC4` /
  `jclr #1,x:$FFFFC5,*` / `movep #2,x:$FFFFC5` / `movep #0,x:$FFFFD4` / `movep #1,x:$FFFFD6` / `jmp $0`, with the
  descriptor `{0x004000, 0x000403, 0x000201, 0x001400}` at X:0 (interleave, to memory, FIFO 0, 16-bit, 32 x 2,
  from `X:$1400`). FIFO 0 is a 64 KiB ring (`GPFADDR` SGE table of 16 pages, `GPFMAXSGE = 15`, `GPOFBASE0 = 0`,
  `GPOFEND0 = 0x10000`). `GPOFCUR0` advances `0x4B80` bytes per 100 ms (48 kHz x 4 bytes). The VP mixes the
  voice into the mixbins as 24-bit values (`0x1F3200` = 7986 << 8 at `X:$1400`, bin 1 at `X:$1420`). The CPU
  forwarded 191 936 frames in 4 s to the AC97 with no resyncs or underruns. The 2 s capture has peak 8000 on
  both channels and 1760 zero crossings, i.e. the 440 Hz test tone, bit-exact in level.
* **`movep #1,x:$FFFFC4` does not stall the DSPs on hardware (round 10).** A loop that counts iterations
  into `X:$10` around that instruction ran ~1.6 M iterations per 100 ms on both GP and EP, with frames off and
  with `SECTL = 0x0F` alike: the frame handshake in xemu's model is not how silicon paces the DSPs.


**Progress (executed on xemu 0.8.136, "Real-time DSP processing" off).** Not yet run on a real console.

* Stage 1 done: `samples/apu_vp_voice` follows 7.2 steps 1-13 and 17-24 with the register model in
  `src/mcpx_apu_regs.h`. A one-shot, a pitched one-shot and a looped voice switched off with `VOICE_OFF` all
  advance `CBO`, clear `ACTIVE_VOICE`, write the notifier, raise the idle-voice trap and are unlinked; xemu does
  not abort. The trace-event regression recipe is still to do.
* Stage 2 mostly done, stage 3 started: `src/apu_vp.c` is the library's VP driver. With
  `-DOPENAL_APU_REAL_MMIO` and the real BAR0, `alcOpenDevice()` runs it instead of the legacy EP/GP setup, and
  the `apu_voice_*` calls drive VP handles 64-127. It implements the signed 4.12 log2 pitch, 12-bit attenuations,
  eight-output routing, `SAMPLES_PER_BLOCK` = 1 for stereo, unsigned 8-bit, an SGE page allocator over the
  24-bit linear space, locked live updates of pitch and volumes, and idle-trap service with unlinking.
  `samples/openal_vp` runs six OpenAL phases on it (loop, distance and pitch sweeps, orbit, a three-voice
  chord, one-shots that end as `AL_STOPPED`); all pass. `tests/test_apu_vp.c` checks the encodings against
  4.6/4.7 and the register writes on a memory BAR0. Not done: `CBO` save/restore for preempted voices (a
  re-promoted voice restarts from the beginning), notifier- or interrupt-driven completion (the driver polls
  `ACTIVE_VOICE`; applications must call `alXboxUpdateVoices()` so the trap is serviced), and handles 128-255.
* **Ported to the real-console path (after probe round 17), host-tested, not yet run on hardware:**
  `apu_vp.c` no longer drives voices with methods. It writes each record as the methods would leave it, links it
  at the 2D head and keeps a software list table. Pause unlinks the voice (keeping `CBO`), off unlinks it and
  clears `PAR_STATE`, and ended one-shots are unlinked in `apu_vp_service()`. At init two `SET_CURRENT_VOICE`
  methods probe the front end (`FEPIOQ`, `0x1340`, pending byte; xemu reads 0). If it stalls, the leftover
  entry is stepped out and HRTF is disabled, because the table and xemu's per-voice latch need methods. Mono
  sources then play on plain voices panned by volume, with the 5.1 pan folded into bins 0/1. Table bases are
  16 KiB-aligned (`APU_TABLE_ALIGN`), and a locked instruction drains the write-combining pool before a list
  head exposes a record. `apu_gp.c` runs the hardware frame loop (complete, wait, acknowledge, DMA, count at
  `X:$10`) and writes the descriptor after the release. `apu_ac97.c` forwards the GP FIFO to the AC97 from
  `apu_vp_service()` (512-frame buffers, 4 queued) when the front end stalls, i.e. on a real console.
* **First hardware runs of `samples/openal_vp` (2026-10-07):**
  * **`SECTL`:** writing only `0x08` runs no frames at all. Bits 2:0 must stay 7 as in every probe
    (`MCPX_APU_SECTL_RUN = 0x0F`).
  * **GP image:** loaded from the write-combining pool, the GP bootstrap ran stale code (`P:0 = 0x0BF080`).
    APU tables and the GP image now come from `apu_mem_alloc_table()` (uncached, physically 16 KiB-aligned),
    and `apu_gp_init()` reads P memory back and retries.
  * **With both fixes, OpenAL is audible through the APU on the console:** the loop, distance sweep and pitch
    sweep, with 0 AC97 underruns.
  * **Freeze when a one-shot ends:** 24.5 s in, exactly when the first one-shot ping ended, frame processing
    froze (GP frame counter stuck, `XGSCNT` still running) and the codec replayed stale buffers. With
    `FETFORCE1` bit 15 set (as DirectSound leaves it), an ended voice still in a list apparently halts frames
    until the front end is serviced, which the stalled method queue cannot do. On hardware the driver now
    clears `FETFORCE1`, logs any `FECTL` bit-15 message, and the AC97 feed pads with silence when the GP
    falls behind.
  * **`FETFORCE1` was not the cause:** clearing it changed nothing. It froze again at ~36 700 frames, with no
    `FECTL` bit-15 message. A voice that *reaches its end* on hardware stops all frame processing, while
    voices unlinked by the CPU (stopping the chord) do not.
  * **Workaround:** every OpenAL buffer now carries `APU_VP_SILENT_TAIL_BYTES` (256) of silence. On hardware a
    one-shot loops over that tail (LBO = data frames, EBO = end of tail), and `apu_vp_service()` unlinks it once
    `CBO` passes the data, so no voice ever ends by itself.
  * **Verified on the console:** `samples/openal_vp` ran more than two full cycles of its six phases
    (102 268 GP frames, about 68 s) with no freeze. It played the loop, distance and pitch sweeps, orbit,
    three-voice chord and one-shot pings, with 0 AC97 underruns and 4 padded buffers (start-up). HRTF is still
    unavailable there (it needs front-end methods).
* **`samples/openal_showcase` on the APU (branch `feat/showcase-apu-backend`, resolved on hardware):**
  * **Probe round 18: the VP SGE table works only up to entry 2047.** The tone mapped at entries 0 to 1047 came
    out clean (peak 8000, about 87 zero crossings per 100 ms, 0 sample-to-sample glitches). Mapped from entry
    2047 upward, the VP stopped all frame processing for good, and the GP counted no more frames until the
    console restarted. The dashboard leaves `0x2018 = 0x7FF` (next to `0x2010 = 0x800` and `0x2014 = 0x400`);
    writing `0xFFF` does not lift the limit. `APU_VP_SGE_ENTRIES` is now 2048, an 8 MiB linear space.
  * **Rounds 19-21: routing and the right channel.** Each VP output reaches its own mixbin. The GP output DMA
    needs a channel stride: with descriptor control bits 23:14 (`dsp_step`) at 0, a console reads mixbin 0 for
    every channel of an interleaved transfer, so the right speaker was silent. `dsp_step = 32` (one bin) fixes
    it (`apu_gp.c`); xemu ignores the field. Interleaving in a DSP program instead did not work on hardware.
    The helicopter's noise came from the AC97 feed (now primed deeper, padded with silence only when empty).
  * **Rounds 22-27 and `samples/apu_vp_stress`: the polyphony freeze.** Under churn (3 one-shot starts per
    16 ms, the oldest stopped beyond 40) frame processing stopped for good, always at the same point of a given
    start sequence, and nothing short of a reboot (not `apu_vp_deinit`/`apu_vp_init`, not any `FECTL`, `ISTS`,
    `FETFORCE1` or `SECTL` write) brought it back. Ruled out one by one: `FETFORCE1`, buffer length, page offset
    and cache type, live target updates, the AC97 feed, VP write-back of driver fields, middle-of-list unlinks
    (the freeze happened identically with the end scan off), a guard page behind each buffer, `fe_probe()`,
    and `FEMEMADDR`.
  * **Cause: handles above 127.** With `fe_probe()` skipped, the front end showed the message the probe's stuck
    method had hidden: `FECTL` bit 15, `FEDECMETH = 0x8000` (`SE2FE_IDLE_VOICE`), `FEDECPARAM = 129`, the
    newest list head, whose record said ACTIVE. The VP calls every handle above 127 idle the first time it walks
    it and halts. Driver churn on handles 64-127 ran clean (708 starts in 4 s), while round 27's register-level
    replay, run in the same binary, froze on handle 129 just like the driver (its earlier standalone "never
    froze" result was misleading). The driver now caps hardware voices at `APU_VP_HW_HANDLES` (128):
    `apu_vp_alloc_handle()` hands out 64-127 and `apu_vp_voice_start()` refuses higher handles. Verified on the
    console: 40 and 60 concurrent one-shots for 8 s each (1413 starts, no refusals), `samples/openal_vp` for
    90 s+ (4413 starts, 0 underruns) and the showcase's polyphony mode.
  * **Handles 0-63 as plain voices** (no HRTF target) neither froze nor played: their `CBO` never advanced.
    They need the HRTF stage, so a console has 64 voices.
* Stage 4 done in xemu (not audible there, see 4.8 and C4.A5): `src/apu_hrtf.c` computes the 128-entry table
  from a spherical-head model (Brown-Duda head-shadow shelf per ear as a 31-tap int8 FIR, Woodworth ITD in
  s6.9; 32 azimuths x elevations -30/0/30/60; no measured data, no pinna cues). `apu_vp_init()` uploads it with
  `SET_CURRENT_HRTF_ENTRY`/`SET_HRIR` x 15/`SET_HRIR_X` and sets `SET_HRTF_SUBMIXES` to bins 0-3 (front
  L/R, surround L/R). Mono sources now play on handles 0-63 with `SET_VOICE_TAR_HRTF` latched to the entry of
  the source direction, re-latched on every spatial update; outputs 0/1 and 2/3 carry the pan's front and rear
  shares, output 5 the LFE share. Stereo sources stay on plain handles 64-127. `samples/openal_vp` shows the
  latched entry following the orbit, with no xemu abort; `tests/test_apu_hrtf.c` and `tests/test_apu_vp.c`
  cover the table and the upload. Whether real hardware L1-normalises the FIRs like xemu, and how its HRTF
  output reaches the speakers, needs stage 5 and a console.

1. **VP-only mono voice on xemu** (7.2 steps 1-13 and 17-24, route A): prove one 48 kHz PCM16 one-shot voice
   plays and completes without aborting; add a trace-event-based regression recipe.
2. **Encodings:** signed log2 pitch, 12-bit attenuation (0 = unity, 0xFFF = mute), eight-output routing, the
   `SAMPLES_PER_BLOCK` rule, a global SGE page allocator, unsigned 8-bit handling.
3. **Voice manager:** lists, idle-trap service, notifier-based completion, `CBO` save/restore after VOICE_ON,
   handles 64-255 for non-HRTF voices on xemu, 64-127 on a console (8.1b).
4. **HRTF:** generate 31-tap int8 FIRs and s6.9 ITD, load the 128-entry table, latch with `SET_VOICE_TAR_HRTF`.
5. **GP/EP firmware (routes B and C):** assemble and validate against xemu's opcode table, including that
   every instruction's template has an emulation function (4.9); frame contract of 4.9.
6. **Hardware validation:** every item in section 6 that matters, on a real console.

### 8.3 Staged plan for route B

1. Software mixer core (resample to 48 kHz S16 stereo, apply the existing gains/pan/ITD, sum, clip).
2. Backend glue to `XAudioInit`/`XAudioProvideSamples` and the per-frame `alXboxUpdateVoices()` tick.
3. CPU-budget measurement on hardware with the benchmark sample, recorded in a committed results file.
4. Optional extras: HRTF-style filtering in software, software AC-3 if Dolby output is wanted.

### 8.4 Independent of the route

* Replace the host-mock-only confidence with a test that asserts the library only touches registers xemu
  defines (a register-address table), and run the VP-only route under xemu in CI if feasible.
* Keep the real-MMIO path opt-in until route A step 1 passes on xemu.
* Keep `docs/XEMU_VERIFICATION.md` in step with the code: update the verdict rows when behaviour changes.

---

## 9. Appendix: corrections to the initial review

Two claims in the first review of this library turned out to be wrong when checked against xemu.

**(a) 8-bit PCM is unsigned on the hardware, so passing OpenAL data through unchanged is correct.**
The first review said the library mislabels 8-bit PCM and must convert it. In xemu `SAMPLE_SIZE_U8` (value 0)
voices are read with an unsigned byte load and converted as `(v - 0x80) / 128`
(`hw/xbox/mcpx/apu/apu_regs.h:241-243`, `vp/vp.c:1063-1066`, `fpconv.h:31-34`); the debug UI labels the same
value "Unsigned 8b PCM" (`ui/xui/debug.cc:128-133`). OpenAL's 8-bit formats are unsigned as well, so
`alBufferData`'s unchanged copy (`lib/openal/src/al_buffer.c:315`) is right and **no signed conversion should
be added**. Only the comment in `apu_voice.h` ("8-bit Signed PCM") was wrong; it now says unsigned.
One consequence to remember: any padding or slack appended to an 8-bit buffer must be 0x80, because 0x00
decodes as full-scale negative.

**(b) HRTF is implemented in the voice processor, but in a different form than the library models.**
The first review claimed HRTF runs only on the GP DSP and therefore the library's attribution of HRTF to the VP
was wrong. xemu runs HRTF per voice inside the voice processor (`vp/vp.c:1458-1465`, `vp/hrtf.h`), and the GP
emulation contains no HRTF code. The library's attribution to the VP is therefore right. What is wrong is
the form: a 31-tap FIR per ear with int8 coefficients plus one signed s6.9 ITD (clamped to +/-42 samples) from
a 128-entry table loaded through front-end methods, available for voice handles below 64 only, instead of a
Q14 biquad with an ITD pair of 0..64 samples and a RAM delay buffer stored in the voice context
(section 4.8; rows C4.1, C4.1a-C4.1d).

Further corrections made while writing this document are listed in section 1.5.
