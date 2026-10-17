# ALC_EXT_EFX on the APU: implementation plan

Branch `feat/openal-efx`. Workflow phase 8 (`.agents/workflow/state.json`). Status: phase8-01 (EFX objects)
and phase8-02 (frame loop, kernel, mailbox) done on the host; phase8-00 (console probe) built, waiting for the
console; phases 03-05 to do.

## 1. Goal

Expose the standard OpenAL Effects Extension (`ALC_EXT_EFX` 1.0) so ports and engines that already use it run
on the APU unchanged, with the mixing on hardware: sources are VP voices, auxiliary effect slots are mixbins,
effects are code on the GP. Runtime CPU cost stays at parameter writes.

**Phase 1 (this plan):** the EFX objects and tokens, sends to slots, `AL_EFFECT_REVERB` / `AL_EFFECT_EAXREVERB`
on the existing GP reverb, a vendor effect type that runs the application's own GP kernel, and the probes that
decide filters and live re-routing. **Phase 2:** Half-Life's room kernel (`s_dsp.c`, bit-exact integers) as a
custom-kernel effect, kept in the Xash tree. **Phase 3:** Xash's APU voices send to that slot (fork item 5b).

**Not in scope.** Several effects running at once, chorus / flanger / echo / distortion and the other EFX
types (`AL_INVALID_VALUE`), filters before the probe passes, 5.1 output, `AL_SOFT_*` additions.

**Clean-room.** `efx.h` is written from the public token list (values are interface). Reverb and kernels are
our own; no `dsstdfx` image, no Creative preset tables (EFX presets stay the application's business).

## 2. What we start from (measured)

The pasted design (`al_nxdk.h` sketch) gets the mapping right; these details differ on a console:

| Sketch | Measured / in the tree |
|---|---|
| Mixbins are 24-bit float | 24-bit fixed point (Q23), 32 samples a frame at `X:$1400 + 32n`; clip at 1.0, no headroom (headroom needs front-end methods) |
| Dry bins go straight to the AC97 | Everything passes the GP: its program reads bins 0/1 and writes the only output, the FIFO ring the AC97 reads (zero-copy). Any program must emit dry + wet |
| Stereo source = 2 linked voices | One record; left on even outputs, right on odd |
| 16-bit gain per output | 8 outputs, each a 5-bit bin and a 12-bit attenuation (1/64 dB). Bins are written at voice start; rewriting `VBIN` on a playing voice is unprobed |
| 256 hardware voices | 256 records, 120 used (64 with HRTF) |
| GP runs I3DL2 / chorus natively | GP runs only what we load: the copy program or `gp_reverb.asm` (Freeverb, 165 words) |
| Scratch page table at `0xFE802000` | `GPSADDR` + SGE table, already working (`apu_gp.c`) |
| Per-voice SVF "for free" | `CFG_MISC.FMODE` / `TAR_FCA` / `TAR_FCB` exist; on a console only "FMODE 1 and 3 change the level" is known |
| `float_to_dsp24`, RT60 maths | Fine for the generic reverb (`apu_reverb_coefficients` does it); a Half-Life kernel needs its own integers |

Also from the tree:
- **Outputs in use.** Panned voices: 0/1 -> bins 0/1. HRTF voices: outputs 0-3 feed the four submix slots
  (0x20AC). Output 4 -> bin 2 is the reverb send today. **Outputs 4-7 are free on every voice kind**, so sends
  use 4-7 (outputs 2/3 are not free on HRTF voices).
- **Bin 31 is the multipass bin** (XEMU_VERIFICATION C2.A11, open). `HRTF_UNUSED_BIN` in `apu_vp.c` is 31
  today; this plan moves it to bin 30.
- **GP budget.** 64000 cycles and 64 DMA descriptors a frame kept full rate, 96000 cycles did not. The reverb
  uses 25 delay lines (about 50 descriptors). There is room for one heavy effect, not two.
- **No mailbox.** Coefficients are single live X-word writes; a pre-delay change rewrites descriptors with the
  wet gain at zero for a frame.

## 3. Design

### 3.1 API objects (`al_efx.c`, new)

- `include/AL/efx.h`: every EFX 1.0 token and function prototype / `LPAL*` typedef.
- Effects, filters and slots: id tables like sources (`alGen*`, `alDelete*`, `alIs*`), parameters stored with
  the spec's defaults and ranges, getters return what was set. `AL_INVALID_VALUE` for unsupported effect
  or filter types and out-of-range values, `AL_INVALID_OPERATION` for deleting a slot a source sends to
  (slots copy effects and sources copy filters, as the extension says, so those can always be deleted).
- `ALC_EXT_EFX` in the device extension string and `alcIsExtensionPresent`; every EFX function through
  `alGetProcAddress`; `ALC_EFX_MAJOR_VERSION` / `MINOR_VERSION` = 1 / 0.
- `ALC_MAX_AUXILIARY_SENDS`: accepted in `alcCreateContext` attributes, clamped to **2**, readable by
  `alcGetIntegerv`.
- Up to **4 slot objects**. Phase 1 runs **one effect per device**: attaching a non-null effect while another
  slot (or `AL_XBOX_REVERB`) has one gives `AL_INVALID_OPERATION`. Slots without an effect are legal and silent.

### 3.2 Slots, bins and sends

- Slot k (0..3) owns the bin pair **2+2k / 3+2k** (bins 2..9). A send is two voice outputs: send 0 = outputs
  4/5, send 1 = outputs 6/7. Stereo voices put left on the even output, right on the odd one; mono voices feed
  both at -3 dB. Mono-input kernels (the reverb) read L + R; stereo-input kernels (Half-Life) read the pair.
- Send volume = source gain x distance attenuation with `AL_ROOM_ROLLOFF_FACTOR` (and the effect's room
  rolloff, as the spec adds them) x send-filter gain. `AL_AUXILIARY_SEND_FILTER_GAIN_AUTO` follows the spec;
  the HF parts (`AL_AIR_ABSORPTION_FACTOR`, `AL_CONE_OUTER_GAINHF`, `*_GAINHF_AUTO`) are stored and wait for
  filters. `AL_METERS_PER_UNIT` on the listener is stored (used by air absorption later).
- `AL_EFFECTSLOT_GAIN` is the kernel's wet gain on the GP (one word, live). `AL_EFFECTSLOT_AUXILIARY_SEND_AUTO`
  is stored and honoured with the source auto flags.
- Volume changes are live (as `AL_XBOX_REVERB_SEND` is today). A **bin change** (a send moved to another slot,
  or a slot's effect detached) on a playing voice depends on probe step 3 (phase8-00): live `VBIN` rewrite if it works,
  otherwise the change applies at the next `alSourcePlay` (documented). Sources without a hardware voice
  just store it.

### 3.3 GP program: frame loop + one kernel

`gp_reverb.asm` was the whole program. It is split into a library-owned **frame loop** and a **kernel**
(done: `dsp/gp_loop.asm`, `dsp/gp_kfreeverb.asm`; the map below):
- Loop (library): wait for the frame start, acknowledge, apply the mailbox, run the input/output DMA chain,
  copy bins 0/1 into the staging pair, `jsr` the kernel (or skip it), write the staging pair to FIFO 0.
- Kernel ABI: fixed P origin and size, its own X/Y window and scratch byte range, the address of its input bin
  pair, the staging pair it adds its output to, a parameter block, and a quota of DMA descriptors appended to
  the loop's chain. Exact addresses are a research item (X is $0..$BFF on chip; today's reverb map in
  GP_REVERB_PLAN 3.4).
- The Freeverb becomes the first kernel; the copy program becomes "loop, no kernel". The output of the split
  reverb must match today's host reference bit for bit.

As built:

| Where | What |
|---|---|
| P:$000-$0FF | the loop (108 words) |
| P:$100-$7FF | the kernel (the reverb: 101 words); entry at $100, returns with `rts` |
| X:$00 / X:$08 | copy descriptor (bins 0/1) / mix descriptor (OUT L/R), both to FIFO 0 |
| X:$10-$1F | CNT, FLAGS (bit 0 kernel on), BURN, SPINS, SPMAX, TMOUT, MB_SEQ, MB_ACK, MB_N, K_RHEAD, K_WHEAD, OUTLP, OUTST[4] |
| X:$20 / X:$40 | OUT L / OUT R (32 words each): the dry bins 0/1, the kernel adds to them, then the output lowpass |
| X:$60-$DF | mailbox: 64 (address, value) pairs |
| X:$E0-$BFF, all of Y | the kernel; the reverb: KIN (input pair address) at $E0, coefficients $E1-$E5, tables, chains at $1B0, blocks from $340 to $97F |
| scratch from byte $2000 | the kernel's delay lines |

The kernel's read chain (K_RHEAD, 0 = none) runs before the dry copy; its write chain (K_WHEAD) must end at
the mix descriptor. The reverb uses 50 descriptors of the 60 a kernel may have. `tools/dsp56sim.py` runs
the loop and kernel on the host: `tests/test_gp_programs.py` checks the reverb against apu_reverb.c's
reference word for word (the old program matched it the same way first), and the mailbox and burn. On the
host simulator the loop plus the reverb is about 10800 instructions a frame (the old program 10500).

### 3.4 Mailbox

The CPU writes up to 64 (address, value) pairs and their count, then a sequence word; at the top of the next
frame the loop applies the pairs when the sequence changed and acknowledges it (MB_ACK). All words of one
change land in the same frame (latency: up to one frame, 0.67 ms, plus the CPU's write;
`apu_gp_debug_mailbox_frames` measures it on a console). The reverb's gain changes use it. A new pre-delay
still restarts the kernel (off for a frame, new layout, empty lines): its descriptors' offsets are written
back by the DMA every frame, so a live change would need the GP to compute the new offset.

### 3.5 `AL_EFFECT_REVERB` / `AL_EFFECT_EAXREVERB` on the Freeverb

An approximation, documented as one. All parameters are stored and read back; the ones the kernel uses:

| EFX parameter | Kernel parameter |
|---|---|
| `DECAY_TIME` (0.1..20 s) | `decay_time` |
| `DECAY_HFRATIO` (0.1..2) | `hf_ratio`, clamped to 1 (no HF boost) |
| `GAIN` x `LATE_REVERB_GAIN` | `gain` (clamped to 1) |
| `REFLECTIONS_DELAY` + `LATE_REVERB_DELAY` | `pre_delay`, clamped to 0.1 s |
| `DIFFUSION` | `diffusion` |
| `GAINHF` | `input_lowpass` (one-pole fit at 5 kHz) |
| `REFLECTIONS_GAIN`, `DENSITY`, `AIR_ABSORPTION_GAINHF`, `DECAY_HFLIMIT`, EAX-only (`GAINLF`, echo, modulation, pans, LF/HF references) | stored, not used |
| `ROOM_ROLLOFF_FACTOR` | used by the sends (3.2) |

`AL_XBOX_reverb` stays: `alEnable(AL_XBOX_REVERB)` runs the same kernel through an internal slot on bins 2/3,
so existing code is unchanged; `AL_XBOX_REVERB_OUTPUT_LOWPASS` stays an Xbox extra.

### 3.6 Custom kernels (`AL_XBOX_gp_program`, vendor)

- `alEffecti(effect, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM)`.
- `alXboxEffectProgram(effect, const ALuint *words, ALsizei count, ALuint flags)` loads a kernel image built
  for the ABI in 3.3 (assembled with `tools/dsp56asm.py` or anything else). The library checks the size,
  scratch and descriptor quotas (`AL_INVALID_VALUE`) and keeps a copy.
- Parameters: `alEffecti(effect, AL_XBOX_GP_PARAM0 + n, v)`, n < 32, raw 24-bit words (integers stay exact;
  `alEffectf` stores `v` as Q23). Delivered through the mailbox on a frame boundary.
- `alXboxGetHardwareStatus(AL_XBOX_GP_KERNEL_CYCLES)`: last frame's kernel cycle count, so a game can see
  its headroom.

### 3.7 Filters

`alGenFilters` and the object API work; `AL_FILTER_NULL` is accepted everywhere. `AL_FILTER_LOWPASS`,
`HIGHPASS`, `BANDPASS` give `AL_INVALID_VALUE` until probe step 4 (phase8-00) shows a usable per-voice filter. If it does, the
direct filter maps to the voice filter (it acts on all outputs, so a send filter whose HF differs from the
direct one gets its gain only, documented); that is a follow-up task, not Phase 1.

## 4. Tasks

Each becomes `specs/task-phase8-0N-*.md` (< 400 tokens) on approval.

| Task | Owner | Content | Exit |
|---|---|---|---|
| **phase8-00-efx-probes** | og-xbox-expert / nvidia-apu-expert | `samples/apu_efx_probe`, console. a) rewrite `VBIN` on a playing voice (plain, HRTF, stereo): does the bin move, any click; b) outputs 4-7 to bins 2..9 on all voice kinds, stereo on even/odd; c) HRTF unused slots on bin 30 instead of 31; d) VP filter: `FMODE` 0-3 x `TAR_FCA/FCB` steps on white noise, 10-band levels from a captured bin pair (the FIFO descriptor pointed at it); mailbox latency and GP headroom are measured in phase8-02 on the real loop | numbers for each, a go/no-go on live re-routing and on filters |
| **phase8-01-efx-core** | openal-expert | `efx.h`, `al_efx.c` objects, ALC attribute and strings, proc addresses, source / listener / slot parameter storage, one-effect rule; null backend | host tests: every token's default, range and error; `alGetProcAddress` for all functions |
| **phase8-02-gp-kernel-loop** | dsp-microcode-specialist | split `gp_reverb.asm` into loop + kernel, mailbox, Freeverb as kernel 0, `AL_XBOX_reverb` on it, bin 31 -> 30 | bit-exact against the host reference; xemu: 0 underruns; console: reverb sample unchanged |
| **phase8-03-slots-sends** | nvidia-apu-expert | slot bins, sends on outputs 4-7, send gains and auto flags, slot gain, live or next-play bin changes per probe a, `AL_EFFECT_REVERB` / `EAXREVERB` mapping | host tests for gains and routing; console: two sources, two sends, one slot |
| **phase8-04-custom-kernel** | dsp-microcode-specialist | `AL_XBOX_gp_program`: loader, checks, params, cycle status; a small example kernel (stereo echo) in the sample | host tests for the checks; console: echo delay exact, params change on a frame |
| **phase8-05-efx-docs-sample** | openal-expert / publisher | `samples/openal_efx` (reverb via EFX, custom kernel, sends), `ENGINE_INTEGRATION.md` 3.12 (EFX recipe, limits, the approximation table), `XEMU_VERIFICATION.md` 8.1b results, README extension list | console run: 0 underruns / padded / slips |

Phase 2 (Half-Life kernel) starts after phase8-04 and lives in the Xash fork; it needs nothing more from the
library than the ABI and the mailbox.

## 5. Testing

- Host: `tests/test_efx_*.c` in `run_tests.py`: objects, errors, defaults, gain maths, reverb mapping, kernel
  image checks, mailbox block layout.
- DSP: the split program against `apu_reverb.c`'s host reference (as today); xemu runs the GP, not S/PDIF.
- Console: probe sample first, then `openal_efx` and the engine / showcase. Rebuild sample objects after
  header changes (rule 01).

## 6. Risks

| Risk | Fallback |
|---|---|
| `VBIN` cannot change on a playing voice | bin changes apply at the next play (documented); the engine sets sends before playing |
| The split loop costs too much with the reverb | keep the reverb monolithic for `AL_EFFECT_REVERB`; custom kernels replace the whole program with a loop template we ship |
| The VP filter is unusable | filters stay `AL_INVALID_VALUE`; muffling via `AL_XBOX_REVERB_OUTPUT_LOWPASS` or the game's kernel |
| A custom kernel overruns the frame | `padded` / `slips` in `AL_XBOX_stats` and `AL_XBOX_GP_KERNEL_CYCLES` show it; nothing guards it on the DSP |
| Games expect several slots (EAX-style per-room) | one active effect is reported by `AL_INVALID_OPERATION`; a later phase links several light kernels into one image |
