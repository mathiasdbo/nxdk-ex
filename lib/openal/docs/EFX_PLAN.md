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

### 2.2 On-chip effects and several slots (phase9-08, `samples/openal_efx` 6i / 6j)

The sample mutes the dry path (`AL_FILTER_LOWPASS`, `GAIN` 0), sends a voice into the slot and reads the final
mix through `AL_XBOX_output_callback` (Goertzel over 960-sample blocks of the left ear). Expected values come from
the library's kernels in the host simulator on the same sounds (`samples/openal_efx/expected_6i.py`; the kernels
are within 3.3 LSB of 16 bits of OpenAL Soft's algorithms, `GP_KERNELS.md` 10). Levels are against the dry,
centre-panned ear, which the sample measures first (expected 0.7071 x pcm 8000 = 5657). Tolerance: 0.5 dB on the
linear checks; the level-dependent ones add the simulator's spread for an input 0.5 dB louder or quieter.
First on hardware: `rnd` (ring modulator, distortion, equalizer, autowah), `mpyr` / `macr` (compressor, autowah).

**xemu (2026-10-27, xemu 0.8.139):** every capture of the mix is silent (`AC97 padded` on each one, `output_slips`
growing, the GP's frames late), here and in the older sections 1-6h, and `AL_XBOX_GP_LOAD` reads 0: no level, load
or timing check below can be modelled there. What xemu does show: every kernel loads through the P window (0 GP
restarts in 6i, 6j and the 5-minute rotation), all four slots attach (`AL_NO_ERROR`), the second reverb is refused
with `AL_INVALID_OPERATION` and the slot keeps its autowah, the rotation changes effect every 15 s with no AL error,
and the log on E: is written through line by line. Its GP runs at 90-300 frames/s instead of 1500 (`GP frames +N`
in the log). xemu 0.8.139 aborted in five of seven runs with `dsp_cpu.c:419 lookup_opcode_slow: Invalid op code`
(op `$0000E0`, `$0000EA`), each time right after a kernel switch (once in 6d, before any new kernel; in 6i at the
equalizer; in 7 after the autowah and after the echo); the runs without an abort ended with the `XReboot()`. The
cause: `apu_gp.c` `wait_frames()` gave up after 40000 register reads (about 20 ms) and went on as if the two frames
had passed, so on a GP this slow the code was rewritten under a kernel still in its frame. It now waits up to 250 ms
by `KeTickCount` and refuses the load instead: 3 of 3 xemu runs then reached the `XReboot()`, no abort.

**Console (2026-10-27):** 44 of 45 checks passed, the GP ran about 1500 frames a second with every kernel, and
the rotation ran 5 minutes without an underrun, a padded buffer or a slip. The one failure was the test's: 6j's
floor was measured while the reverb's tail still rang (fixed, rerun pending).

| Section | Check | Expected (simulator), tolerance | xemu | Console |
|---|---|---|---|---|
| 6i ring modulator | 1 kHz x 300 Hz sine, `HIGHPASS_CUTOFF` 0: 700 / 1300 Hz against the dry ear | -3.0 / -3.0 dB, +-0.5 | silent | -3.0 / -3.0, PASS |
| | the 1 kHz carrier against 700 Hz | below -40 dB (simulator -111) | silent | -105.7 dB, PASS |
| 6i distortion | 1 kHz pcm 8000, `EDGE` 0.5, `GAIN` 0.5, lowpass 8 kHz, EQ 3 kHz / 6 kHz: H1 against the dry ear; H3, H5 against H1 | +7.7; -6.6, -12.2 dB, +-0.6; H3 30 dB over the dry voice's | silent | +7.6; -6.6, -12.2; H3 92.4 dB over, PASS |
| 6i equalizer | 100 / 500 / 3000 / 12000 Hz, flat against the dry ear | +3.0 dB each, +-0.5 | silent | +3.0 each, PASS |
| | low +12 dB (cutoff 200 Hz) | +10.3 / +1.1 / 0.0 / 0.0, +-0.5 | silent | +10.3 / +1.1 / 0.0 / 0.0, PASS |
| | mid1 +12 dB (500 Hz) | +0.3 / +12.0 / +0.2 / 0.0, +-0.5 | silent | +0.3 / +12.0 / +0.2 / 0.0, PASS |
| | mid2 +12 dB (3 kHz) | 0.0 / +0.2 / +12.0 / +0.4, +-0.5 | silent | 0.0 / +0.2 / +12.0 / +0.4, PASS |
| | high +12 dB (cutoff 6 kHz) | 0.0 / 0.0 / +1.6 / +10.9, +-0.5 | silent | 0.0 / 0.0 / +1.6 / +10.9, PASS |
| 6i compressor | 1 kHz pcm 29000 / 3000, slot gain 0.5: loud on - off, quiet on - off | +2.3 +-1.0, +6.0 +-0.5 | silent | +2.3, +6.0, PASS |
| | loud - quiet, on / off | 16.0 +-1.0 / 19.7 +-0.5 dB | silent | 16.0 / 19.7, PASS |
| 6i autowah | harmonics every 100 Hz, 0.6 s loud then 0.6 s 14 dB quieter, `RESONANCE` 100, `PEAK_GAIN` 0.00003: the peak, loud / quiet | 2312 / 499 Hz, +-12 %; (f - 20) ratio 4.78 +-0.45 | silent | 2312 / 499 Hz; 4.78, PASS |
| 6i GP load | ring modulator, distortion, equalizer (one band), compressor, autowah | simulator 3,500 / 4,500 / 2,400 / 1,800 / 7,200 instructions a frame | 0 % (not modelled) | 0 / 8 / 3 / 3 / 12 % (reverb 34 %, echo 2-3 %, chorus 0 %: the meter reads 0 for the ring modulator and the chorus, to look into) |
| 6j four slots | reverb + equalizer (4 bands) + distortion + autowah attach | `AL_NO_ERROR` each | PASS | PASS |
| | all four busy 2 s: GP load peak, AC97 padded | below 100 % (about 87 % by the reverb's console scale, 27,500 instructions), 0 padded | 0 %, padded +188 (not modelled) | 56 %, peak 57 %; padded +0, underruns +0, PASS |
| | each slot alone: 1 kHz against the dry ear; 3 kHz against 1 kHz | at least -30 dB and 20 over the floor; H3 at least -30 dB on the distortion only | silent | reverb -4.5, equalizer +4.6, distortion +7.6, autowah +3.1 dB; H3 -70.6 / -80.5 / -6.6 / -96.9: PASS but the reverb (FAIL: the floor, measured 300 ms after its sends, was the reverb's own tail at -22.1 dB; the floor now waits 3 s) |
| | a second reverb on slot 4 | `AL_INVALID_OPERATION`, slot unchanged | PASS | PASS |
| | GP restarts | 0 | 0, PASS | 0, PASS |
| 7 rotation | 11 settings (reverb, echo, chorus, ring modulator, distortion, flanger, equalizer, compressor, autowah), 15 s each, 5 min, then `XReboot()` | 0 underruns / padded / slips, no AL error | no AL error, 0 restarts; slips not modelled | 0 underruns / padded / slips each minute, no AL error, 0 restarts, about 1500 GP frames/s, GP load peak 35 %, `XReboot()`, PASS |

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
- **As built (phase9-04):** each slot runs its own GP kernel (EFX slot s in GP slot s - 1), so a reverb, an echo
  and a chorus play at once. An attach gives `AL_INVALID_OPERATION` only when the GP's shared budgets are full:
  the kernels' P (0x700 words), X ($B20), Y (2048), scratch (512 KiB less the bootstrap image) and 60
  descriptors, summed, and while the GP runs, the loader's own first fit (no compaction). A refused attach
  changes nothing. `AL_XBOX_REVERB` stays exclusive with EFX effects; an ABI 1 kernel runs only alone.
- **As built (phase9-05..07):** the echo, the chorus / flanger and the on-chip effects (ring modulator,
  distortion, equalizer, compressor, autowah) are library kernels a slot loads like a custom one
  (`GP_KERNELS.md` 10); the frequency shifter, vocal morpher and pitch shifter stay `AL_INVALID_VALUE`.

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
- **As built (phase9-04):** send n of a source to EFX slot s leaves on outputs 4+2n / 5+2n to GP slot k = s - 1's
  pair, **bins 2+2k / 3+2k** (2/3 .. 8/9, all reachable at full level: 2.1); a send to a slot without an effect
  is muted where it is. The per-channel-full rule of a stereo source into an `AL_EFFECT_XBOX_GP_PROGRAM` slot
  holds per send. A send that moves on a playing voice ramps (`apu_voice.c`): its targets go to mute, after 2
  GP frames (the VP has ramped the current volumes down) `CFG_VBIN` / the `CFG_FMT` bin bits change and the
  targets come back; moving back within the ramp calls it off. A voice not playing takes the bins at its start.
  `AL_XBOX_REVERB` is GP slot 0 on bins 2/3, as before.
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
| X:$60-$DD | mailbox: 63 (address, value) pairs |
| X:$DE (since phase9-04 X:$D5) | mailbox: the frame (X:CNT, mod 2^24) to apply it at; the CPU writes it with every post |
| X:$E0-$BFF, all of Y | the kernel; the reverb: KIN (input pair address) at $E0, coefficients $E1-$E5, tables, chains at $1B0, blocks from $340 to $97F |
| scratch from byte $2000 | the kernel's delay lines |

The kernel's read chain (K_RHEAD, 0 = none) runs before the dry copy; its write chain (K_WHEAD) must end at
the mix descriptor. The reverb uses 50 descriptors of the 60 a kernel may have. `tools/dsp56sim.py` runs
the loop and kernel on the host: `tests/test_gp_programs.py` checks the reverb against apu_reverb.c's
reference word for word (the old program matched it the same way first), and the mailbox and burn. On the
host simulator the loop plus the reverb is about 10800 instructions a frame (the old program 10500).

**Since phase9-04** (one kernel per slot, `GP_KERNELS.md`): the loop is 209 words with four trampolines
(`TRAMPk: jmp entry`, rewritten by the CPU with the slot off); FLAGS bits 0-3 switch slots 0-3, bit 4 an ABI 1
kernel at P:$100 (alone, the old path), bit 5 the output lowpass (the reverb's); the mailbox is 56 pairs (MB_AT at $D5)
($60-$CF), the slot table (each slot's X block) at $D0-$D3. Kernels are relocatable images placed first fit
in P:$100-$7FF, X:$E0-$BFF, Y and scratch from $2000; each has its own chains (both ending at end of list),
run and waited for slot by slot, then the mix descriptor. The reverb's X block is $8C0 words.

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
so existing code is unchanged; `AL_XBOX_REVERB_OUTPUT_LOWPASS` stays an Xbox extra. Since phase9-04 the reverb
kernel is relocatable like the others (`apu_reverb_layout` / `apu_reverb_coefficients` take the slot's X block
and scratch), its output lowpass acts on the whole mix while it runs, and only one reverb fits (X).

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
| Games expect several slots (EAX-style per-room) | done in phase9-04: one relocatable kernel per slot; `AL_INVALID_OPERATION` only when the shared budgets are full |
