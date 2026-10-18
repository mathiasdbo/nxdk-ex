# ALC_EXT_EFX on the APU: implementation plan

Branch `feat/openal-efx`. Workflow phase 8 (`.agents/workflow/state.json`). Status: phase 1 done (phase8-00..05);
the reverb, the custom echo kernel and the sends confirmed on a console (2.1).

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
| 16-bit gain per output | 8 outputs, each a 5-bit bin and a 12-bit attenuation (1/64 dB). Rewriting `VBIN` / `CFG_FMT` bins on a playing voice takes effect at once (2.1) |
| 256 hardware voices | 256 records, 120 used (64 with HRTF) |
| GP runs I3DL2 / chorus natively | GP runs only what we load: the copy program or `gp_reverb.asm` (Freeverb, 165 words) |
| Scratch page table at `0xFE802000` | `GPSADDR` + SGE table, already working (`apu_gp.c`) |
| Per-voice SVF "for free" | A per-voice resonant 2-pole lowpass (`FMODE` 1), live; calibration open (2.1) |
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

### 2.1 Console results (phase8-00, 2026-10-17)

`samples/apu_efx_probe`, log `E:\apu_efx_probe.txt`; RMS on a 5657 tone, frame rate 1493-1505/s throughout.

| Probe | Result |
|---|---|
| Send bins | Outputs 4/5 reach any bin pair 2/3 .. 8/9 at full level (5656) on plain, HRTF and stereo voices, nothing elsewhere; stereo left only on the even bin, right only on the odd. The send routing of 3.2 holds |
| Bin 30 | HRTF spare slots land on bin 30 (RMS 5295 unless muted); bin 31 stays clean. No kernel may read bin 30. The probe's `0x20AC` rewrite changed nothing visible (the register's role stays open) |
| Live re-route | `CFG_VBIN` and the `CFG_FMT` bin bits move a playing voice's outputs at once. Dry bins: no click (largest step 1048 against the tone's 1044). The moved send starts abruptly (step 3062): a live move needs a volume ramp. Not needed by 3.2 (only volumes change) |
| VP filter | Live writes work. `FMODE` 0 and `FMODE` 2 with Q 0 are flat (within the +-3 dB of the band estimate). **`FMODE` 1: resonant 2-pole lowpass on `TAR_FCA`**: cutoff s4.12 in octaves (-0x1000 = one octave down, 0 about 8 kHz), Q (31:16) is damping (0: +25..33 dB peak, 0x4000: about +6 dB, about -12 dB/octave above). `FMODE` 2 with Q 0x4000: a boost around the cutoff (+7..+14 dB). `FMODE` 3 looks like 1 and 2 in series. FCA and FCB were written together; which mode uses which is open |
| Mailbox / GP | (phase8-02 on the real loop) `apu_gp_reverb`: decay -29.8 / -9.9 dB at 0.5 s for T60 1 / 3 s (expected -30 / -10), waits max 810 polls, 0 timeouts, 0 underruns over 64 s: identical to the monolithic program |
| Filter calibration (2026-10-18) | `samples/apu_filter_probe`, 25 tones 50 Hz-20 kHz read by Goertzel against the same voice unfiltered. `FMODE` 1 reads only `TAR_FCA`. Damping (31:16): 0x2000 peaks +11.5 dB at the cutoff, 0x8000 +0.9, **0xB505 is flat (Butterworth: -3.3 dB at the cutoff, -11.6 an octave up)**, 0xFFFF droops. Cutoff (15:0) 8 kHz x 2^(c / 0x1000) for c < 0: -3 dB at 1 kHz / 500 / 250 / 125 / 62 Hz for -0x3000 .. -0x7000; **c = 0 and every positive value: no filtering**; from -0x800 to -1 the 5 kHz loss shrinks smoothly (-2.6 .. 0 dB), shallower near the top than an analog Butterworth (so the library maps by the measured 5 kHz levels). Stereo voices: the same. HRTF voices: the same with `TAR_FCB` = 0 (with FCB set too they filter about an octave lower). A live cutoff change makes no step. `FMODE` 2 is a peaking boost on `TAR_FCB` (+2.8 dB at Q 0xB505, +9.6 at 0x4000, flat at 0xFFFF), not a highpass; `FMODE` 3 is 1 and 2 in series |
| EFX sample (phase8-05, 2026-10-18) | `samples/openal_efx`, all through the AL API. EFX reverb T60 1.5 s: -19.9 dB over 0.5 s (expected -20); send off: -41 dB. Echo kernel: loaded through the P window (no GP restart in any load of the run); dry -> echo exactly +1000 samples on both ears at -3.0 dB (wet 1, dry centre-panned), wet 0.5 gives -9.1; delay 2400 by re-attach: no reload, mailbox acknowledged after 1 frame, echo exactly +2400; feedback 0.5: +4800 / +9600 / +14400 / +19200 at -3.0 / -9.1 / -15.1 / -21.1 dB; sends 0 and 1 into one slot both echo +1000. GP load: reverb 34 %, echo 4 %. 5 min with the effect switching every 15 s (HRTF, panned and stereo sources): 0 underruns / padded / slips, zero-copy lead 1016..1056 |
| Lowpass on the console (openal_efx step 6b, 2026-10-18) | 500 Hz + 5 kHz through `AL_FILTER_LOWPASS`, changed live while the voice plays. 5 kHz level for `GAINHF` 0.9 / 0.7 / 0.5 / 0.25 / 0.1 / 0.03: -0.9 / -2.9 / -5.8 / -11.9 / -20.0 / -30.4 dB (20 log10 `GAINHF`: -0.9 / -3.1 / -6.0 / -12.0 / -20.0 / -30.5); 500 Hz 0.0 .. -0.7 dB. `GAIN` 0.5: -6.0 dB on both tones. Back to `AL_FILTER_NULL`: 0.0 dB. An HRTF voice gives the same numbers to 0.1 dB. 5 min after it: 0 underruns / padded / slips, GP load peak 35 % |

Toolchain note: nxdk's pdclib `lrint*()` is an `assert(0)` stub; round by hand.

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

- A send is two voice outputs: send 0 = outputs 4/5, send 1 = outputs 6/7. Stereo voices put left on the even
  output, right on the odd one; mono voices feed both. Each output carries half the send's gain, so L + R, which
  mono-input kernels (the reverb) read, is the send; stereo-input kernels (Half-Life) read the pair.
  **Since phase 9:** a stereo source into a custom kernel (`AL_EFFECT_XBOX_GP_PROGRAM`) sends each channel at
  the whole send gain instead, so send 1 is attenuation 0 and the kernel reads the samples unscaled (the Xash
  port asked for it: 0.5 is not exact in 1/64 dB steps, so a doubled half cannot be bit-exact).
- **As built (phase8-03):** with one effect per device every send goes to the kernel's input pair, **bins 2/3**,
  whatever its slot; a send whose slot does not hold the active effect is muted. Moving a send or an effect
  between slots therefore only changes volumes, which a playing voice takes live, so nothing waits for the
  live-`VBIN` probe. The per-slot pairs (slot k on bins 2+2k / 3+2k) come with several kernels.
- Send volume = source gain x distance attenuation with `AL_ROOM_ROLLOFF_FACTOR` (and the effect's room
  rolloff, as the spec adds them) x send-filter gain. `AL_AUXILIARY_SEND_FILTER_GAIN_AUTO` follows the spec;
  the HF parts wait for filters. **Phase 9:** `AL_AIR_ABSORPTION_FACTOR` and `AL_CONE_OUTER_GAINHF` (with
  `AL_DIRECT_FILTER_GAINHF_AUTO`) multiply into the voice lowpass's `GAINHF`; the send HF auto flag stays stored
  (one filter per voice). `AL_METERS_PER_UNIT` on the listener scales the air absorption's distances.
- `AL_EFFECTSLOT_GAIN` is the reverb's wet gain on the GP (one word, live); for a custom kernel it scales the
  sends into it (the kernel has no word for it). `AL_EFFECTSLOT_AUXILIARY_SEND_AUTO`
  is stored and honoured with the source auto flags.
- Volume changes are live (as `AL_XBOX_REVERB_SEND` is). `AL_XBOX_REVERB` on: send 0 is
  `AL_XBOX_REVERB_SEND` x the dry gain (the old level exactly) and EFX sends are silent; off: the EFX sends to
  the active slot. Sources without a hardware voice just store everything.

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
| P:$000-$0FF | the loop (116 words) |
| P:$100-$7FF | the kernel (the reverb: 101 words); entry at $100, returns with `rts` |
| X:$00 / X:$08 | copy descriptor (bins 0/1) / mix descriptor (OUT L/R), both to FIFO 0 (both written by `apu_gp_init`) |
| X:$07 / X:$0F | IDLE (START_FRAME polls before this frame) / IDLMIN (the smallest since the CPU wrote $7FFFFF): the load meter |
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
- `alXboxGetHardwareStatus(AL_XBOX_GP_LOAD / AL_XBOX_GP_LOAD_PEAK)`: the share of the frame the effect takes,
  in percent (no cycle counter is known on the GP, so the loop counts its START_FRAME polls and the library
  compares them with a frame without the kernel).

As built (phase8-04):
- **Image** (`tools/dsp56asm.py --image`, symbols `K_SCRATCH_BYTES` / `K_DESCRIPTORS` in the source): magic
  `$47504B` ('GPK'), ABI 1, scratch bytes, descriptors a frame, then the code for P:$100. Checks: code 1..$700
  words, each 24-bit, descriptors <= 60, scratch <= the device's space above $2000 (the reverb's size, about
  180 KiB; before a device, what the next one will allocate). `alXboxEffectProgram` keeps a reference-counted
  copy that slots share (EFX copy semantics: deleting the effect leaves the slot's copy running).
- **ABI for custom kernels** (`dsp/gp_loop.asm`): before the first frame the library stops the kernel for two
  frames, writes the code to P:$100 through the P window and reads it back (if the window did not take it,
  it restarts the GP from the bootstrap image, which holds the code too; `apu_gp_debug_code_restarts`
  counts it), zeroes X:$E0..$BFF and the declared scratch, then writes KIN $E0 (= $1440, bins 2/3), KSCR
  $E1 (= $2000), KSCRN $E2, KPARAM0..31 $E8..$107, K_RHEAD = 0, K_WHEAD = FIFO2, and switches the kernel on.
  The kernel sets up its descriptors and chain heads itself on its first call; its X starts at KOWN $108;
  Y is its own and not initialised.
- **Parameters**: `alEffecti` stores -0x800000..0xFFFFFF (read back sign-extended), `alEffectf` -1..1 as Q23.
  Attaching the effect again with the same image sends only the changed words, in one mailbox post (one
  frame); a different image reloads the kernel. Switching back to a reverb puts its code back the same way.
- **Guide** for kernel authors: `docs/GP_KERNELS.md`; the game's side in `ENGINE_INTEGRATION.md` 3.12-3.13.
- **Example**: `dsp/gp_kecho.asm` (154 words), a stereo echo: delay 32..16384 samples (param 0, raw),
  feedback and wet (params 1/2, Q23), two 64 KiB lines, 4 descriptors. It sets the next frame's read offsets
  from the write offset every frame, so a new delay applies on the next frame. `tests/test_gp_programs.py` [5]
  runs it in the simulator: the echo lands exactly DELAY samples late on each ear, feedback repeats it, a
  delay through the mailbox applies from the next frame, and the clamp holds (about 1600 instructions a frame).

### 3.7 Filters

`alGenFilters` and the object API work. `AL_FILTER_LOWPASS` is built (after Phase 1); `HIGHPASS` and
`BANDPASS` give `AL_INVALID_VALUE` (no mode of the voice filter is a highpass, 2.1).

As built:
- `AL_LOWPASS_GAIN` / `GAINHF` (0..1, default 1) on the filter object; sources keep a copy (`AL_DIRECT_FILTER`,
  `AL_AUXILIARY_SEND_FILTER`), as the extension says.
- `GAIN` scales the dry volume (direct filter) or the send (send filter).
- The direct filter's `GAINHF` is the voice filter: `FMODE` 1, damping 0xB505, the cutoff interpolated from
  the measured level at 5 kHz per cutoff (`apu_vp_lowpass`, `src/apu_vp.c`), FCB 0. Written at voice start
  and live when it changes (no click). Within 0.05 dB of 1: no filter; the lowest cutoff is -0x7000 (-76 dB
  at 5 kHz).
- The voice filter acts on all outputs, so it also shapes the sends; a send filter's `GAINHF` is not applied.
- Tests: `tests/test_efx_filter.c`; console: `samples/apu_filter_probe` (calibration), `openal_efx` step 6b.

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
| The VP filter is unusable | probed usable (2.1); until the follow-up, filters stay `AL_INVALID_VALUE` and muffling goes via `AL_XBOX_REVERB_OUTPUT_LOWPASS` or the game's kernel |
| A custom kernel overruns the frame | `padded` / `slips` in `AL_XBOX_stats` and `AL_XBOX_GP_LOAD_PEAK` show it; nothing guards it on the DSP |
| Games expect several slots (EAX-style per-room) | one active effect is reported by `AL_INVALID_OPERATION`; a later phase links several light kernels into one image |
