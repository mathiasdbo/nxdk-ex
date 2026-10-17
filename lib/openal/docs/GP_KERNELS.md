# Writing a GP kernel (AL_XBOX_gp_program)

A GP kernel is your own DSP56300 code that the APU's GP DSP runs on the effect send of every source, every
0.67 ms, as an `ALC_EXT_EFX` effect. It costs no CPU time. The library owns the frame, the output and the
parameter delivery; the kernel only processes one 32-sample block per call.

**Status:** host-tested (assembler, simulator, loader). Not yet run on a console: the `openal_efx` sample
(phase8-05) checks the echo's timing on silicon and whether the code loads without a GP restart (see
"Loading" below). The ABI is version 1 (`AL_XBOX_GP_IMAGE_ABI`).

The example used throughout is `dsp/gp_kecho.asm`, a stereo echo (154 words).

## 1. From the game's side

```c
#include "gp_kecho.h"                 /* generated: gp_kecho_image[], GP_KECHO_IMAGE_WORDS */

ALuint fx, slot;
alGenEffects(1, &fx);
alEffecti(fx, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
alXboxEffectProgram(fx, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
alEffecti(fx, AL_XBOX_GP_PARAM0 + 0, 4800);      /* delay: 100 ms, a raw integer */
alEffectf(fx, AL_XBOX_GP_PARAM0 + 1, 0.4f);      /* feedback, Q23 */
alEffectf(fx, AL_XBOX_GP_PARAM0 + 2, 0.5f);      /* wet, Q23 */

alGenAuxiliaryEffectSlots(1, &slot);
alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)fx);   /* loads and starts it */
alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
```

- **`alXboxEffectProgram(effect, words, count, flags)`**: `effect` must be of type
  `AL_EFFECT_XBOX_GP_PROGRAM` (`AL_INVALID_OPERATION` otherwise). The library checks the image and keeps its own
  copy, so the game's array can be freed afterwards. `count` 0 clears the program. `flags` must be 0.
  - A bad image gives `AL_INVALID_VALUE`: wrong magic or ABI, code outside 1..0x700 words, a word above 24
    bits, more than 60 descriptors, or more scratch than the device has (about 180 KiB).
  - Setting `AL_EFFECT_TYPE` again drops the image and resets the parameters to 0.
- **Parameters** `AL_XBOX_GP_PARAM0 + n`, n < `AL_XBOX_GP_PARAMS` (32): 24-bit words the kernel reads at
  `X:KPARAM + n`.
  - `alEffecti` stores an integer from -0x800000 to 0xFFFFFF; `alGetEffecti` reads it back sign-extended.
  - `alEffectf` stores a fraction from -1 to 1 as Q23. 1.0 saturates to 0x7FFFFF.
  - Out of range: `AL_INVALID_VALUE`, and the old value stays.
- **Changes take effect on attach**, as with every EFX effect: the slot holds a copy. After changing parameters,
  call `alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, fx)` again. With the same image only the changed
  words are sent, all together, at the start of one frame. A different image reloads the kernel (section 5).
- **Deleting the effect** leaves the slot's copy running, as EFX specifies.
- **One effect per device:** the GP runs one program. While a slot holds a kernel, attaching an effect to
  another slot, or `alEnable(AL_XBOX_REVERB)`, gives `AL_INVALID_OPERATION`. Attaching a reverb to the same slot
  replaces the kernel and puts the reverb's code back.
- **`AL_EFFECTSLOT_GAIN` does not reach a custom kernel.** The kernel has no word for it; give it a wet
  parameter (as the echo does).
- **Load:** `alXboxGetHardwareStatus(AL_XBOX_GP_LOAD, &v)` is the share of the last frame the effect took, in
  percent; `AL_XBOX_GP_LOAD_PEAK` the highest since the previous peak query. 100 means frames are late and the
  dry mix stutters. Console only: 0 elsewhere (section 7).

## 2. The frame

Each frame is 32 samples at 48 kHz. The library's loop (`dsp/gp_loop.asm`) does, in order:

1. wait for the APU's frame start (counting how long it waits: the load meter);
2. apply the mailbox: parameter changes, written into `X:KPARAM + n` before your kernel runs;
3. run your **read chain** (`X:K_RHEAD`, 0 for none) and wait for it: DMA from your scratch into X;
4. copy the dry mix (bins 0/1) into OUT L / OUT R;
5. `jsr $100`: **your kernel**;
6. the output lowpass (off for custom kernels);
7. run your **write chain** (`X:K_WHEAD`), which must end at the mix descriptor `FIFO2` ($08): DMA from X to
   your scratch, then OUT L/R to the output.

Because the reads run before the kernel and the writes after it, data the kernel writes to scratch in frame n
can be read back at the earliest in frame n+1.

## 3. Memory and the ABI words

| Where | What | Who sets it |
|---|---|---|
| P:$100..$7FF | your code; entry at $100, return with `rts` | the library, from the image |
| X:KIN ($E0) | X address of the input's left block ($1440, bins 2/3); the right block is 32 words on | library |
| X:KSCR ($E1) | byte offset of your scratch ($2000) | library |
| X:KSCRN ($E2) | the scratch size you declared, in bytes | library |
| X:KPARAM ($E8..$107) | the 32 parameters | library, then the mailbox |
| X:KOWN ($108..$BFF) | your own X memory (descriptors, buffers, state) | you |
| X:K_RHEAD ($19) | first read descriptor, 0 for none | you (the library writes 0) |
| X:K_WHEAD ($1A) | first write descriptor; the chain ends at FIFO2 | you (the library writes FIFO2) |
| X:OUTL ($20) / OUTR ($40) | 32 words each: the dry mix; add your output to them | loop |
| all of Y | yours; **not initialised** | you |
| scratch from KSCR | your delay lines and tables (system RAM through DMA), zeroed | library |

Before the first frame, the library zeroes X:$E0..$BFF and the declared scratch, then writes the ABI words.
Anything else you need (descriptors, chain heads, tables, Y) you set up yourself. The usual pattern is a flag
in your X that is 0 on the first call:

```asm
INIT    equ     $108
        org     p:$100
kernel  move    x:INIT,a
        tst     a
        jne     ready
        ...                     ; build descriptors, set K_RHEAD / K_WHEAD
        move    #1,x0
        move    x0,x:INIT
ready   ...                     ; the per-frame work
        rts
```

**Rules:**
- Use any register; the loop keeps nothing in them across the call. `jsr` and `do` nest on the DSP's
  hardware stack, and the loop's call takes one level of it.
- Do not write X below $E0 except K_RHEAD and K_WHEAD, nor OUT L/R beyond their 32 words.
- Read the parameters every frame (or compare them with a copy). A change arrives between two calls with no
  other notice, and all the words of one change arrive in the same frame.

## 4. Input, output and levels

- **Samples** are 24-bit fractions (Q23) in the low 24 bits of each word.
- **Input:** every EFX send of every source mixes into bins 2/3, whatever slot it names (with one effect per
  device, sends to other slots are muted). Each voice output carries half the send's gain: a mono source
  feeds both bins at half, so L + R is the send; a stereo source puts its left channel on bin 2 and its right
  on bin 3. A mono-input effect reads `L + R`; a stereo one reads the pair.
- **Output:** add to OUT L / OUT R, which already hold the dry mix. The mix descriptor sends the top 16 bits
  of each 24-bit word to the codec. Adding in an accumulator and moving the result back to X saturates at full
  scale, so a loud scene clips: keep headroom in the wet gain.

## 5. DMA and scratch

The GP has 4096 words of X. Anything bigger, such as delay lines, lives in scratch, system RAM the library
maps for the GP, and is moved by the DMA engine. A descriptor is 7 X words:

| Word | Meaning |
|---|---|
| 0 | next descriptor's X address, or `$004000` (end-of-list bit 14) |
| 1 | control (below) |
| 2 | word count |
| 3 | X address of the block |
| 4 | byte offset inside the buffer (advanced by the engine when write-back is on) |
| 5 | byte offset of the buffer in scratch (`KSCR` + your offset) |
| 6 | buffer size in bytes, minus 1 |

Control bits: 0 interleave, 1 direction (1: X to memory), 4 offset write-back, 8:5 buffer (`$E` circular
scratch, `$F` linear scratch), 12:10 format (2: 24-bit), 23:14 channel stride. The echo uses `$0049D0` (read:
memory to X, write-back, circular, 24-bit, stride 1) and `$0049D2` (the same, X to memory). A 24-bit sample
takes 4 bytes of scratch. Memory to X works only for non-interleaved scratch transfers.

- **Budget:** at most 60 descriptors a frame across both chains (declare them in `K_DESCRIPTORS`); the
  library's own copy and mix descriptors are extra. The reverb uses 50.
- **Moving a read position.** With write-back on, the engine moves each offset by the transfer size every
  frame. To change a delay, set the read descriptor's offset (word 4) yourself. The echo sets it every frame
  from the write descriptor's offset:
  `read = write + 128 - 4 * DELAY (mod line size)`. The 128 skips the block being written this frame, so a
  new delay applies from the next frame.
- **Scratch layout** is yours: `KSCR` is where it starts, `KSCRN` its size. The echo keeps two 64 KiB lines,
  left at `KSCR`, right at `KSCR + $10000`.

## 6. Building the image

Two equates at the top of the source declare what the kernel needs; the assembler writes them into the
image header:

```asm
K_SCRATCH_BYTES equ $20000      ; scratch bytes
K_DESCRIPTORS   equ 4           ; descriptors a frame, both chains
```

```bash
python3 lib/openal/tools/dsp56asm.py lib/openal/dsp/gp_kecho.asm -o lib/openal/dsp/gp_kecho.h --image
```

The header holds `NAME_image[NAME_IMAGE_WORDS]`, the image to pass to `alXboxEffectProgram`, plus every equate
as `NAME_<SYMBOL>`. The image layout is: `$47504B` ('GPK'), the ABI version, scratch bytes, descriptors, then
the code for P:$100. Any tool that writes this layout works. `--image` refuses a source without the two
equates or one whose code does not start at `org p:$100`.

`tools/dsp56asm.py` covers the subset of DSP56300 instructions its header lists (moves, `mac`/`mpy`, `do`/
`rep`, jumps and calls, bit tests, `movep`). Encodings follow Motorola's public DSP56300 Family Manual.

## 7. Loading, switching and the load meter

When a slot gets a new image, the library:
1. switches the kernel off for two frames (the dry mix keeps playing);
2. measures how long an empty frame waits (the load meter's reference);
3. writes the code into P memory through the GP's P window and reads it back. If the read-back differs, it
   restarts the GP from its bootstrap image, which also holds the new code. `apu_gp_debug_code_restarts()`
   counts the restarts. Whether the P window accepts writes while the GP runs is not yet known on silicon;
   either way the kernel ends up loaded;
4. zeroes X and scratch, writes the ABI words, and switches the kernel on.

The kernel starts from silence: delay lines are empty after every load. Switching back to the reverb reloads
the reverb's code the same way.

**Load meter.** No cycle counter is known on the GP, so the loop counts how many times it polls for the next
frame start. The library compares that count with an empty frame's: the percentage is the share of the frame
your effect (kernel and its DMA) takes. For scale, the reverb runs about 10,800 instructions a frame in the
host simulator and has no late frames on a console. A kernel that overruns does not crash, but the output gets
late frames: watch `AL_XBOX_GP_LOAD_PEAK`, and `ac97_padded` / `output_slips` in `AL_XBOX_stats`.

## 8. Testing on the host

`tools/dsp56sim.py` runs the loop and a kernel on the PC with the same DMA model, so a kernel can be checked
sample by sample before it reaches a console. `tests/test_gp_programs.py` [5] does it for the echo:

```python
gp = dsp56sim.Gp(LOOP, extra=(ECHO,))        # loop + kernel sources
for i, v in enumerate(COPY_DESC): gp.x[i] = v
gp.run_until_wait()
load_custom(gp, [100, 0, 0x400000], gp.symbols['K_SCRATCH_BYTES'])   # what apu_gp.c does
fifo = gp.frame({2: block_left, 3: block_right})                    # one frame; returns the output
```

Its helpers `load_custom` (the library's load sequence) and `run_echo` (impulses in, output streams out) can be
copied for a new kernel. Parameter changes are tested by writing the mailbox (`MBOX`, `MB_N`, `MB_SEQ`) as the
library does. The simulator knows the instructions the assembler knows; it does not model the GP's timing,
only the instruction count (`gp.frame_steps` for the last frame).

The library's loader is tested with the null backend in `tests/test_apu_gp.c` [8], and the AL entry points in
`tests/test_efx_program.c`.

## 9. Checklist

- `org p:$100`, `rts` at the end, under 0x700 words.
- `K_SCRATCH_BYTES` and `K_DESCRIPTORS` declared and true.
- Descriptors built on the first call; the write chain ends at `FIFO2`; K_RHEAD is 0 if you read nothing.
- Your X from `KOWN`; Y initialised by you.
- Parameters read every frame; delays that change rewrite their read offsets.
- Headroom in the output; `AL_XBOX_GP_LOAD_PEAK` checked on a console with the worst case running.
