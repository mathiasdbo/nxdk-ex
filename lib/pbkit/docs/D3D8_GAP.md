# pbkit D3D8 gap

Batch 1 (surface formats): depth formats (Z16 / Z24S8, fixed or floating
point), W-buffering, 16 bits color with dithering, and the tile API, for the
`nxdk-scene3d` pbkit. Validated on xemu with `samples/pbkit_formats`.

Batch 2 (memory): no depth buffer, application supplied frame and depth
buffers, the tile pitch rule, and the back buffer count. Validated on xemu with
`samples/pbkit_memory`, see [Batch 2](#batch-2-memory).

Batch 3 (sync): VBlank and swap callbacks, display field status, presentation
intervals, fences, and a `pb_reset` fix. Validated on xemu with
`samples/pbkit_sync`, see [Batch 3](#batch-3-sync).

Batch 4 (quality): render to texture (swizzled and linear render targets) and
multisampling (2x / 4x with a GPU resolve). Validated on xemu with
`samples/pbkit_quality`, see [Batch 4](#batch-4-quality).

Batch 5 (display): present flags (`XVideoSetModeEx`: interlaced / progressive,
widescreen, 10:11 pixel aspect ratio), the dashboard video flags, the TV
filters, a YUY2 / UYVY video overlay on NV_PVIDEO, and a persisted display
across `XLaunchXBE`. Validated on xemu with `samples/pbkit_display`, see
[Batch 5](#batch-5-display).

The points a console must still confirm are listed at the end of each batch.

## API added (all additive, nothing existing changed signature)

`pbkit_framebuffer.h`

| Function | When | What |
| --- | --- | --- |
| `pb_set_depth_format(fmt)` / `pb_get_depth_format()` | before `pb_init` (asserted) | `NV097_SET_SURFACE_FORMAT_ZETA_Z16` or `_Z24S8` (default). Z16 allocates 2 bytes/pixel, no stencil plane. |
| `pb_set_depth_float(bool)` / `pb_get_depth_float()` | any time | `NV097_SET_CONTROL0_Z_FORMAT` (bit 12). Re-pushes CONTROL0 and the viewport z range when the engine is running. Default off. |
| `pb_set_w_buffer(bool)` / `pb_get_w_buffer()` | any time | `NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE` (bit 16): depth = interpolated vertex w. **Default on** (see CONTROL0 below). |
| `pb_depth_scale()` | after `pb_init` | value zmax = 1.0 maps to in `pb_set_viewport` (`pb_ZScale`). |
| `pb_set_dither(bool)` / `pb_get_dither()` | any time | `NV097_SET_DITHER_ENABLE`. Default on for 16 bits color formats, off for 32 bits. |

`pbkit.h`

| Function | What |
| --- | --- |
| `pb_depth_stencil_buffer()` | CPU address of the (shared) depth stencil buffer. Linear for the CPU only with `pb_set_depth_tile_mode(PB_DEPTH_TILE_LINEAR)` or `_NONE` before `pb_init` (the default Z tile is not; see the console run). |
| `pb_depth_stencil_pitch()` | bytes per line of that buffer (same convention as `pb_back_buffer_pitch`). |
| `pb_assign_tile(index, addr, size, pitch, z_start_tag, z_offset, flags)` | now declared (it was already public). Index asserted 0..7. |
| `pb_release_tile(index, clear_offset)` | now public. |
| `PB_TILE_FLAGS_ZBUFFER` 0x1, `PB_TILE_FLAGS_Z32BITS` 0x04000000, `PB_TILE_FLAGS_ZCOMPRESS` 0x80000000 | same values as the XDK `D3DTILE_FLAGS_*`. |

### Depth scale (`pb_ZScale`) and clear value per format

| Format | CONTROL0 Z_FORMAT | `pb_depth_scale()` | `pb_erase_depth_stencil_buffer` raw value | trigger |
| --- | --- | --- | --- | --- |
| Z24S8 | fixed | 0xFFFFFF | 0xFFFFFF00 (depth<<8 \| stencil 0) | Z \| STENCIL (0x03) |
| Z16 | fixed | 0xFFFF | 0xFFFF | Z (0x01) |
| Z16 | float | 511.9375 | 0xFFFF (= f16 of 511.9375) | Z |
| Z24S8 | float | 1.0E30 | 0xE293E500 (= f24 of 1.0E30, stencil 0) | Z \| STENCIL |

The float scales are what xemu uses as the top of its depth clip range
(`hw/xbox/nv2a/pgraph/glsl/common.c`: `f16_max = 511.9375`, `f24_max = 1.0E30`;
`util.h`: f16 raw -> IEEE754 is `(raw << 11) + 0x3C000000`, f24 is `raw << 7`,
so 0xFFFF is the largest 16 bits float and the 24 bits float tops out near
3.4E38). The viewport z range (`VIEWPORT_PX_DIV2` z, `DEPTH_RANGE` /
`NV097_SET_CLIP_MIN/MAX`) is scaled by the same value, so z = 1.0 lands on the
clear value with the `less or equal` depth function pbkit sets. Z24S8 fixed
point output is byte identical to the previous pbkit.

### CONTROL0 correction

The feature list said "CONTROL0 bit 0 = float Z" and the research note said
pbkit never writes `NV097_SET_CONTROL0`. Both are wrong:

- `set_draw_buffer()` has always pushed `NV20_TCL_PRIMITIVE_3D_W_YUV_FPZ_FLAGS`
  (= `NV097_SET_CONTROL0`, 0x290) with **0x00110001** on every
  `pb_target_back_buffer()`: bit 0 STENCIL_WRITE_ENABLE, bit 16
  Z_PERSPECTIVE_ENABLE (W-buffering), bit 20 TEXTURE_PERSPECTIVE_ENABLE.
- bit 12 is Z_FORMAT (1 = float), bit 0 is the stencil write enable.

So W-buffering has been pbkit's default forever; `pb_set_w_buffer(false)` is
what gives a plain Z-buffer. `pb_control0()` now builds the value:
TEXTURE_PERSPECTIVE | (Z24S8 ? STENCIL_WRITE : 0) | (float ? Z_FORMAT : 0) |
(w ? Z_PERSPECTIVE : 0). With the defaults it is still 0x00110001.
`NV20_TCL_PRIMITIVE_3D_STENCIL_ENABLE` is pushed as 1 for Z24S8 (as before)
and 0 for Z16.

### Tiles

Tile #1 (depth) flags: Z24S8 `ZCOMPRESS|Z32BITS|ZBUFFER` = 0x84000001
(unchanged), Z16 `ZCOMPRESS|ZBUFFER` = 0x80000001. The Z32BITS bit goes into
the `NV_PFB_ZCOMP` tag word (`EncodedZStartTag |= 0x04000000`), xemu stores
the register but does not emulate compression, so the Z16 flag is taken from
the XDK `D3DTILE_FLAGS_Z32BITS` definition, not from an emulator check.

Application render targets: allocate with `MmAllocateContiguousMemoryEx`
(16 Kb aligned size and address, below 0x03FFB000, write combined), pick a
pitch from the 16 tile pitches (0x200, 0x400, 0x600, 0x800, 0xA00, 0xC00,
0xE00, 0x1000, 0x1400, 0x1800, 0x1C00, 0x2800, 0x3000, 0x3800, 0x5000,
0x7000), call `pb_assign_tile(3.., addr & 0x03FFFFFF, size, pitch, 0, 0,
flags)` and `pb_release_tile(index, 0)` before freeing. `pb_kill` releases
all 8 tiles. Tiles 0, 1 and 2 are pbkit's.

### 16 bits color

- `pb_init` already sized the color buffers from `pb_ColorFmt`; `XVideoSetMode(640, 480, 16|15, ...)`
  gives the CRTC `VIDEO_R5G6B5` / `VIDEO_A1R5G5B5` with the same 1280 bytes pitch pbkit computes.
- `pb_fill` X1R5G5B5 packing was wrong (red `>> 7`, green `>> 5` instead of
  `>> 9`, `>> 6`); fixed in `pbkit_draw.c`. `pb_print` / `pb_draw_text_screen` go
  through `pb_fill`, so they work in both 16 bits formats (see screenshots).
- `debugPrint` (`lib/hal/debug.c`) handles 15/16/32 bpp already.
- `pb_show_depth_screen` shows the depth buffer through the CRTC color format:
  consistent for Z16 + 16 bits color and Z24S8 + 32 bits color only.

## samples/pbkit_formats

Cycles through 7 configurations with `pb_kill` / `XVideoSetMode` / `pb_init`
(re-init survives on xemu; `make CONFIG=n` builds a single configuration
that holds its frame forever; `make clean` first when changing CONFIG, the
object does not depend on CFLAGS).
z = 0.9, w = 0.3; B blue: z = 0.1, w = 0.7, in depth units, winner drawn
first, loser last), `pb_finished`, then reads color and depth back with the
CPU at 4 pixels (clear, overlap, A only, B only). Results go to `pb_print`,
`DbgPrint` and a `PBFMTRES` tagged block in `.data` (0x32000 in this build).

xemu run (`xemu.exe -s -dvd_path <iso>`, results read over the GDB stub).
Every GDB connection halts the VM until it is resumed, which slows the cycle
down a lot, and a framebuffer read that lands in the middle of a mode switch
can take xemu down: poll rarely, or capture the single configuration builds.

| # | Config | Color readback (clear / overlap / A / B) | Depth readback | Verdict |
| --- | --- | --- | --- | --- |
| 0 | Z24S8 + A8R8G8B8, Z-buffer | 404040 / 0000FF / FF0000 / 0000FF | FFFFFF / 199999 / E66665 / 199999 (exp 0.1 and 0.9 of 0xFFFFFF) | PASS |
| 1 | Z16 + R5G6B5, Z-buffer | 4208 / 001F / F800 / 001F | FFFF / 1999 / E665 / 1999 | PASS |
| 2 | Z16 float + X1R5G5B5, Z-buffer | 2108 / 001F / 7C00 / 001F | 3C00 / 2E66 / 3B33 / 2E66 (xemu writes IEEE half of z/511.9375) | PASS (ordering) |
| 3 | Z24S8 float + A8R8G8B8, Z-buffer | 404040 / 0000FF / FF0000 / 0000FF | FFFFA1 / 199999 / E66666 / 199999 (xemu emulates F24S8 as fixed Z24S8) | PASS (ordering) |
| 4 | Z24S8 + A8R8G8B8, W-buffer (pbkit default CONTROL0) | 404040 / FF0000 / FF0000 / 0000FF | FFFFFF / 4CCCCC / 4CCCCC / B33332 (= w 0.3 / 0.7) | PASS |
| 5 | Z24S8 float + A8R8G8B8, W-buffer | 404040 / 0000FF / FF0000 / 0000FF | FFFFA1 / 000000 / 000000 / 000000 | FAIL on xemu, console only |
| 6 | Z16 float + R5G6B5, W-buffer (`make CONFIG=6` build) | 4208 / F800 / F800 / 001F | 3C00 / 34CC / 34CC / 3999 (xemu: half of w/511.9375) | PASS (ordering) |

Framebuffer captures of every configuration (xemu v0.8.139, 2026-10-26) matched
the readbacks; the 15 bpp one has to be decoded as X1R5G5B5 (the capture tool
assumes R5G6B5 for 16 bits). Run twice, once on the change set and once after
merging master (double buffering, deferred kicks): same values.

Config 5: xemu keeps F24S8 in a fixed point `GL_DEPTH24_STENCIL8` normalized by
1E30 (`pgraph/gl/constants.h` FIXME), and its W path squares w for the slope
term, so w values around 1E29 overflow and realistic w values underflow to 0:
the depth test ties and the last triangle wins. This is the XDK's usual
F24S8 + `D3DZB_USEW` combination and only a console can prove it.

## What xemu cannot prove (console checks)

1. Float depth ranges: `pb_ZScale` 511.9375 (Z16) and 1.0E30 (Z24S8) and the
   raw encodings written by the ROP (`NV097_SET_CONTROL0` bit 12). xemu stores
   its own normalized representation (half float of z/511.9375, fixed Z24S8).
2. F24S8 + W-buffering (config 5).
3. The Z16 compression tag flag (`NV_PFB_ZCOMP` without bit 26) and whether
   the CPU readback of a compressed depth tile is transparent. (The tile pitch
   1536 vs surface pitch 1280 mismatch of 640 wide 16 bits buffers is fixed
   in batch 2, tile pitch rule.)
4. Dithering (`NV097_SET_DITHER_ENABLE`): xemu maps it to `GL_DITHER`, which is
   a no-op for exactly representable colors.
5. `STENCIL_WRITE_ENABLE` = 0 with Z16 (xemu has no stencil attachment then).

### Console run (2026-10-27)

`samples/pbkit_formats` in chain mode (`make CONFIG=n CHAIN=1`, one XBE per
configuration, log in `E:\pbkit_formats.txt`) on a console. Besides the four
readback points the log has the depth along rows 150 and 300 and a GPU probe:
a full screen quad halfway between the two triangles' depths, read back as
colors, which shows what the GPU's depth test holds.

- **The depth test is right in all 7 configurations**, F24S8 + W-buffering
  (config 5, which xemu cannot run) included.
- **With pbkit's default depth tile (a compressed Z tile) the CPU does not see a
  linear image.** Configurations 1, 2, 4, 5 and 6 failed their depth readback
  (the far triangle's own area read the near triangle's value), and the row
  dumps of 0 and 3, which passed, were scrambled too: stripes a few pixels wide,
  whole Z16 rows of one value or of 0. Turning compression off changed nothing;
  the Z flags of the tile region are what the CPU sees through.
- **With a tile region without the Z flags (`PB_DEPTH_TILE_LINEAR`), or none
  (`PB_DEPTH_TILE_NONE`), all 7 pass**, and the rows match the triangles' edges
  to the pixel. The sample now uses `PB_DEPTH_TILE_LINEAR`; pbkit's default stays
  the Z tile, the fast one for rendering. Item 3 above is answered: the readback
  of a Z tile is not transparent, with or without compression.
- Raw float depth on the console (near / far / clear): Z16 float, Z-buffer
  `C998 / FCCB / FFFF`; F24S8, Z-buffer `DF431E / E26B81 / E293E5`; F24S8,
  W-buffer `E0E4AD / E21ABA / E293E5`; Z16 float, W-buffer `E332 / F665 / FFFF`.
  All ordered as the formats require (clear > far > near).
- A depth clear four times the buffer's height (`EXP=GPU_ERROR`) did not raise
  a GPU error.

pbkit's GPU error report halts inside the interrupt DPC, which freezes every
thread. `pb_set_gpu_error_halt(false)` records the error instead
(`pb_get_gpu_error()`), and the sample's watchdog thread (`watchdog.h`) logs it
and resets the console through the SMC, as it does after 10 s without progress.
On the console the hang test reset as expected and logged its last step.

## Batch 2: memory

### API added

`pbkit_framebuffer.h`

| Function | When | What |
| --- | --- | --- |
| `pb_set_depth_format(PB_DEPTH_NONE)` | before `pb_init` | `PB_DEPTH_NONE` = 0: no depth stencil buffer (D3D8 `EnableAutoDepthStencil = FALSE`). |

`pbkit.h`

| Function | When | What |
| --- | --- | --- |
| `pb_set_frame_buffers(DWORD *addrs[], int count, DWORD pitch)` | before `pb_init` | application frame buffers; `NULL`/0 = pbkit allocates again. |
| `pb_set_depth_stencil_buffer(DWORD *addr, DWORD pitch)` | before `pb_init` | application depth stencil buffer; `NULL` = pbkit allocates again. |
| `pb_frame_buffers_tiled()` / `pb_depth_stencil_tiled()` | after `pb_init` | whether tile #0 / #1 was assigned (tile pitch rule, application buffer layout). |
| `pb_back_buffers(1\|2)`, `pb_wait_for_flip()` | already in master (#56) | double / triple buffering; exercised by the sample. |

New `pb_init` errors: -12 invalid application frame buffers, -13 invalid
application depth stencil buffer (or one given with `PB_DEPTH_NONE`). The
reason goes to `debugPrint`. Both come after the GPU setup and call `pb_kill`
like -11, so a later `pb_init` works (the sample checks it).

### No depth buffer (`PB_DEPTH_NONE`)

`pb_init` allocates nothing and assigns no tile #1. `set_draw_buffer` takes
the no-depth branch it always had but never used: DMA 10 (zeta) null (limit
0), `DEPTH_TEST_ENABLE` 0, `STENCIL_ENABLE` 0, and now `DEPTH_WRITE_ENABLE` 0
too; zeta pitch = color pitch. CONTROL0 has no `STENCIL_WRITE_ENABLE`
(`pb_control0` only sets it for Z24S8). `pb_erase_depth_stencil_buffer` and
`pb_show_depth_screen` return at once, `pb_depth_stencil_buffer()` is `NULL`,
`pb_depth_stencil_pitch()` 0. The zeta field of `SET_SURFACE_FORMAT` keeps
Z24S8: 0 is not a zeta format (xemu asserts on it when it binds a zeta surface).

xemu binds the zeta surface for a draw only when the depth or stencil test is
on (`pgraph/gl/draw.c`), and its DMA limit check would fail on the null zeta
DMA otherwise: an application that turns the depth test on in this mode is
wrong on both.

### Application supplied buffers

Allocate them like pbkit does:

```c
DWORD size = 3 * 2560 * 480;   /* pb_back_buffers()+1 frame buffers, one block */
BYTE *fb = MmAllocateContiguousMemoryEx((size + 0x3FFF) & ~0x3FFF, 0, 0x03FFB000, 0x4000,
                                        PAGE_READWRITE | PAGE_WRITECOMBINE);
DWORD *addrs[3] = {(DWORD *)fb, (DWORD *)(fb + 2560 * 480), (DWORD *)(fb + 2 * 2560 * 480)};
pb_set_frame_buffers(addrs, 3, 2560);
pb_set_depth_stencil_buffer(ds, 2560);   /* same kind of allocation, 2560 * 480 bytes */
pb_init();
...
pb_kill();                              /* never frees them */
MmFreeContiguousMemory(fb);
```

`pb_init` checks (fail = -12 / -13):

- count = `pb_back_buffers()` + 1;
- every address 64 bytes aligned, physically contiguous over the buffer, with
  physical address = `addr & 0x03FFFFFF` (pbkit hands the GPU that, true for
  `MmAllocateContiguousMemoryEx` memory) and ending below 0x03FFB000;
- frame buffer pitch = the scan-out pitch pbkit uses itself (width * Bpp
  rounded to 64; 0 means that pitch). **Deviation from the spec** (which said
  pitch >= width * Bpp): the CRTC pitch is set by `XVideoSetMode` /
  `AvSetDisplayMode` and pbkit does not reprogram it, so a wider frame buffer
  would be shown sheared;
- depth stencil pitch: any multiple of 64 >= width * (2 or 4).

Write combining is the application's choice (pbkit can't check it). Tiling is
skipped, not refused, when the buffers can't be a tile region: pitch not a tile
pitch, start not 16 Kb aligned, size not a multiple of 16 Kb (tile regions
have 16 Kb granularity, pbkit never tiles memory past the buffers), or frame
buffers that are not one contiguous block in order. pbkit's own allocations
are unchanged (`pb_FrameBuffersAddr` / `pb_DepthStencilAddr` stay 0 for
application buffers, which is how `pb_kill` knows not to free them).

### Tile pitch rule

A tiled buffer's surface pitch must equal its tile pitch. Before, pbkit used
width * Bpp rounded to 64 as surface pitch and the next `pb_TilePitches` entry
as tile pitch; they only matched for 32 bits color at 640 (2560 = 0xA00).

- **Depth stencil buffer**: surface pitch = tile pitch. It is never scanned
  out, so the padding costs nothing. 640 wide Z16: 1280 -> 1536
  (`pb_depth_stencil_pitch()` reports it; `pb_show_depth_screen` of a Z16
  buffer now looks sheared, it was a debug view).
- **Frame buffers (and extra buffers)**: not tiled when width * Bpp is not a
  tile pitch. The other option, scanning out with the tile pitch, was
  rejected: `XVideoSetMode` passes `pitch = width * Bpp` to
  `AvSetDisplayMode` (`lib/hal/video.c`), pbkit only moves `PCRTC_START`, and
  `pb_show_debug_screen` flips the CRTC to `XVideoGetFB()` which has that
  pitch: a second pitch would mean reprogramming the CRTC on every
  debug/front/depth screen switch, outside pbkit, for a 16 bits mode where
  color tiling buys little. 640 wide 16 bits frame buffers are now untiled
  and packed at 1280 * 480 per buffer.
- 640x480 at 32 bpp is byte identical: same pitches (2560), same sizes, same
  tile #0/#1/#2 parameters, same push buffer contents.

| Mode | Color pitch | Tile #0 | Depth | Depth pitch | Tile #1 |
| --- | --- | --- | --- | --- | --- |
| 640x480 32 bpp | 2560 | yes (0xA00) | Z24S8 | 2560 | yes |
| 640x480 16 bpp | 1280 | no | Z16 | 1536 (was 1280) | yes |
| 640x480 16 bpp | 1280 | no | Z24S8 | 2560 | yes |
| 720x480 32 bpp | 2880 | no | Z24S8 | 3072 (was 2880) | yes |

### samples/pbkit_memory

Six configurations cycled with `pb_kill` / `XVideoSetMode` / `pb_init`
(`make CONFIG=n` for one, `make clean` first). Each draws two overlapping
quads, B (blue, z 0.1) first and A (red, z 0.9) last, plain Z-buffer, then
reads color and depth back with the CPU at 4 pixels (clear, overlap, A only, B
only): the overlap is blue with a depth buffer, red without. It also checks
pitches and tiling, the depth buffer address (`NULL` without one, the
application's when supplied), `pb_show_depth_screen` leaving `PCRTC_START`
alone without a depth buffer, the back buffer rotation over 6 frames, and for
application buffers that `pb_init` rejects 2 buffers (-12) and a depth buffer
at +0x20 (-13) before the valid set-up. Results: `pb_print`, `DbgPrint` and a
`PBMEMRES` tagged block in `.data` (0x32000 in this build).

xemu v0.8.139, 2026-10-26, one `CONFIG=n` ISO per row, then the cycling ISO
(all six PASS, two rounds, so re-init after application buffers and after
`PB_DEPTH_NONE` works):

| # | Config | Pitch fb / ds, tiled fb / ds | Color (clear / overlap / A / B) | Depth | Rotation | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | pbkit buffers A8R8G8B8 Z24S8, triple | 2560 / 2560, 1 / 1 | 404040 / 0000FF / FF0000 / 0000FF | FFFFFF / 199999 / E66665 / 199999 | 3 buffers | PASS |
| 1 | `PB_DEPTH_NONE` A8R8G8B8 | 2560 / 0, 1 / 0 | 404040 / **FF0000** / FF0000 / 0000FF | none, `NULL` | 3 buffers | PASS |
| 2 | app buffers A8R8G8B8 Z24S8 | 2560 / 2560, 1 / 1 | 404040 / 0000FF / FF0000 / 0000FF | FFFFFF / 199999 / E66665 / 199999 | 3, inside the app block; rejects -12 / -13 | PASS |
| 3 | app buffers R5G6B5 Z16 | 1280 / 1536, 0 / 1 | 4208 / 001F / F800 / 001F | FFFF / 1999 / E665 / 1999 | 3, inside the app block; rejects -12 / -13 | PASS |
| 4 | double buffering (`pb_back_buffers(1)`) | 2560 / 2560, 1 / 1 | 404040 / 0000FF / FF0000 / 0000FF | FFFFFF / 199999 / E66665 / 199999 | 2 buffers, alternating | PASS |
| 5 | pbkit buffers R5G6B5 Z16 | 1280 / 1536, 0 / 1 | 4208 / 001F / F800 / 001F | FFFF / 1999 / E665 / 1999 | 3 buffers | PASS |

Framebuffer captures matched the readbacks. `samples/pbkit_formats` config 1
(Z16 + R5G6B5) still passes with the new 1536 depth pitch.

### What xemu cannot prove (console checks)

1. The tile pitch rule itself: xemu ignores `NV_PFB_TILE` (no address
   swizzle, no compression), so tiled vs untiled and the old pitch mismatch
   all look the same there. On a console: 640x480 16 bpp scan-out with
   untiled frame buffers, Z16 at pitch 1536 tiled and compressed, and the CPU
   readback of both.
2. Rendering with a null zeta DMA (`PB_DEPTH_NONE`): xemu never touches the
   zeta surface with the tests off; the real ROP with `DEPTH_WRITE_ENABLE` 0,
   `STENCIL_WRITE_ENABLE` 0 and the occlusion/stencil cull bits pbkit sets
   (`CULL_ENABLE` 3) should not either, but only hardware shows it.
3. Application buffers in tiles: tile #0 over an application block and tile
   #1 compression on an application depth buffer (same as pbkit's own on
   xemu).
4. Physical address checks: xemu maps contiguous memory like the kernel does;
   `MmGetPhysicalAddress` on write combined memory on hardware.

## Batch 3: sync

VBlank and swap callbacks, display field status, presentation intervals and
fences. Validated on xemu with `samples/pbkit_sync`.

### API added

`pbkit.h`

| Function | When | What |
| --- | --- | --- |
| `pb_set_vblank_callback(void (*cb)(DWORD vbl_count))` | any time, `NULL` removes | called once per VBlank from pbkit's DPC, after the flip and the VBlank event pulse. |
| `pb_set_swap_callback(void (*cb)(DWORD frame, DWORD vbl_count))` | any time, `NULL` removes | called once per frame put on screen, from the DPC: after the VBlank callback for VBlank flips, from the `PB_FINISHED` subprogram for immediate flips. `frame` = frames shown since `pb_init` (consecutive). |
| `pb_get_display_field_status(DWORD *vbl_count, int *field)` | after `pb_init` | VBlank counter and the field being scanned out: 0 even, 1 odd, -1 progressive or unknown (D3D `GetDisplayFieldStatus`). |
| `pb_set_presentation_interval(int)` / `pb_get_presentation_interval()` | any time, lasts across `pb_kill`/`pb_init` | `PB_PRESENT_INTERVAL_IMMEDIATE` 0, `_ONE` 1 (default), `_TWO` 2, `_ONE_OR_IMMEDIATE` 3. |
| `DWORD pb_insert_fence(void)` | after `pb_init` | queues a GPU write of the next fence value; does not kick (like `pb_end_deferred`). |
| `bool pb_is_fence_pending(DWORD)` | after `pb_init` | the GPU has not passed the fence yet (kicks the push buffer when it has not). A fence never inserted is not pending. |
| `void pb_block_on_fence(DWORD)` | after `pb_init` | busy waits (`pause`) until the fence has passed. |

Callbacks run at DISPATCH_LEVEL inside the DPC: they must be brief, must not
call pbkit, and must save and restore the FPU state themselves
(`KeSaveFloatingPointState`) if they use floating point. The VBlank handler is
also run from a few pbkit polling loops (tile set-up, `pb_kill`) when they see
the VBlank pending, so a callback can occasionally run in thread context.

### Presentation intervals

`PB_PRESENT_INTERVAL_ONE` is the classic path, unchanged: the ISR writes
`PCRTC_START` at the VBlank when the next queued frame is ready, the DPC
retires it. `pb_finished` pushes the same bytes for every interval; only the
CPU side of the flip changes.

| Interval | Where the flip happens |
| --- | --- |
| ONE | ISR at the VBlank, if the next queued frame is ready (as before). |
| TWO | ISR at the VBlank, only when at least two VBlanks passed since the last flip. |
| IMMEDIATE | `PB_FINISHED` subprogram (DPC), as soon as the GPU has passed the frame's end; `PCRTC_START` is latched at HSYNC (`NV_PCRTC_CONFIG_START_ADDRESS_HSYNC`), so the scan-out switches mid-frame: tearing. Frames still queued (e.g. after switching from ONE) are flipped in order. |
| ONE_OR_IMMEDIATE | ISR at the VBlank like ONE; immediate from the subprogram when the frame is the only one queued and a VBlank has passed since the last flip (the frame missed it). |

For intervals other than ONE the ISR records which slot it flipped
(`pb_VBLFlippedSlot`) and the DPC retires only that slot, so a frame marked
ready between the ISR and the DPC is not retired without having been shown.

### Fences

`pb_init` already binds the 3D semaphore DMA context
(`NV097_SET_CONTEXT_DMA_SEMAPHORE`, method 0x1A4 in the `SET_OBJECT2A` list) to
DMA object 8, `pb_DmaBuffer8`. That buffer is now one uncached, page aligned
page. `pb_insert_fence` pushes `NV097_SET_SEMAPHORE_OFFSET` 0 and
`NV097_BACK_END_WRITE_SEMAPHORE_RELEASE` value (3 dwords): the GPU writes the
value once the rendering pushed before it has passed the back end, without an
interrupt and without stalling. The CPU compares with a signed 32-bit
difference, `(LONG)(fence - written) > 0` = pending, so the counter may wrap;
`pb_init` starts it at 0xFFFFFF00 so the wrap happens within the first 256
fences of every run and wrap bugs show at once.

Why not a `FIRE_INTERRUPT` subprogram like `PB_FINISHED`: that stalls PGRAPH
until the CPU's DPC has handled it, for every fence. xemu emulates the
semaphore release (pgraph `NV097_BACK_END_WRITE_SEMAPHORE_RELEASE`: surface
update, then a 32-bit store at the semaphore DMA object + offset), and the
sample checks it end to end (readback after the fence, wrap).

`pb_insert_fence` ends its block with `pb_end_deferred`; `pb_is_fence_pending`
and `pb_block_on_fence` call `pb_kick` when the fence has not passed, so a
fence behind deferred blocks cannot wait forever.

### Display field status

There is no CRTC field register to read: xemu's `NV_PCRTC_RASTER` is a counter
that increments on every read, and in the SDTV modes the TV encoder
interlaces, not the CRTC (the NV2A scans out a whole frame per VBlank, each
VBlank is one TV field). `pb_get_display_field_status`:

- interlaced or not: the AV mode the kernel set
  (`AvSendTVEncoderOption(AV_OPTION_QUERY_MODE)`, option 13). Output mode bits
  31:30: 0 (480 line SDTV) and 1 (525 line SDTV) are interlaced; 2 (HDTV) only
  with TV table index 3 (1080i); 3 (VGA) never. `field` is -1 otherwise.
- the field alternates with the VBlank counter; its absolute parity is
  anchored once per `pb_init` on `AV_OPTION_GUESS_FIELD` (option 15), non-zero =
  odd.

The two kernel calls happen on the first call after `pb_init`: make that
first call at PASSIVE_LEVEL (not from a callback).

### Push buffer reset fix (`pb_reset`)

The sample found frames lost at high frame rates on xemu (IMMEDIATE, light scene:
61 of 12278 frames never reached the
`PB_FINISHED` subprogram). `pb_jump_to_head` writes the jump at `pb_Put`, sets
PUT to the head and waits for GET to be at the head. When the DMA engine has
not fetched anything since the previous jump, GET is still at the head: the
wait ends at once, and PUT = head = GET reads as an empty push buffer, so the
whole frame (its `PB_FINISHED` included) is skipped and then overwritten.
`pb_jump_to_head` now first kicks and waits (same 100 tick timeout) until GET
has left the head when something was pushed since the last jump. After the
fix: 0 lost frames over 12189 (IMMEDIATE) and on every configuration below.
The push buffer contents are unchanged.

### samples/pbkit_sync

Five configurations cycled with `pb_kill` / `XVideoSetMode` / `pb_init`
(`make CONFIG=n` for one, `make clean` first), 640x480 32 bpp, triple
buffering. Each runs the fence tests and the field status at the default
interval, then sets the callbacks and the interval and measures over 10 s
windows (`KeTickCount`): VBlank callbacks per second against the VBlank
counter, swap callbacks against frames (consecutive frame numbers; after each
window the queue is drained and the totals must be equal), flips per VBlank,
and the share of flips outside the VBlank (a swap callback more than 20000 TSC
cycles after the VBlank callback). "Slow" frames are held back by the CPU to
1.4 refresh periods (23 ms at 60 Hz). Fence tests: pending right after 200
full screen fills, block returns, passed after; a fence behind
`pb_end_deferred` blocks only (block must kick); a CPU written sentinel pixel
is the drawn color once the fence has passed; 300 fences across the wrap
(consecutive values, fences from before the wrap and the first one not
pending, never inserted ones not pending). Results: `pb_print`, `DbgPrint` and
a `PBSYNCRS` tagged block in `.data` (0x33000 in this build) with one summary
row per configuration.

xemu v0.8.139, 2026-10-26, 60 Hz (AV mode 0x0801010D, 480i), one `CONFIG=n`
ISO per row (4 windows each, values of the last), then the cycling ISO:

| # | Config | VBlank cb/s | Swap cb / frames, drained totals | Flips per VBlank | Immediate flips | Frames/s | Verdict |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | ONE, light | 60.05 | 601 / 603, 2416 / 2416 | 1.000 | 0 / 1000 | 60.25 | PASS |
| 1 | TWO, light | 60.03 | 301 / 303, 1216 / 1216 | 0.500 | 0 / 1000 | 30.26 | PASS |
| 2 | IMMEDIATE, light | 71.16 (see below) | 1412 / 1412, 12582 / 12582 | 1.983 | 997 / 1000 | 141.12 | FAIL 0x001 on xemu (VBlank rate only) |
| 3 | ONE, slow frames | 60.56 | 435 / 435, 1744 / 1744 | 0.717 | 0 / 1000 | 43.47 | PASS |
| 4 | ONE_OR_IMMEDIATE, slow frames | 59.97 | 435 / 435, 1744 / 1744 | 0.725 | 1000 / 1000 | 43.47 | PASS |

All rows: VBlank callbacks = VBlank counter increments exactly; field
sequence 0101010101010101 over 16 consecutive VBlanks;
`AV_OPTION_GUESS_FIELD` always 0 on xemu; fences: first 0xFFFFFF01, pending
right after insert (both checks), block 0 ms, deferred + kick ok, readback
00FF00, wrap 0xFFFFFF04..0x0000002F, old and never inserted fences not
pending.

Config 2 fails only the VBlank rate check, an xemu artifact: xemu paces the
VBlank interrupt on host time while the guest clock falls behind under this
load. Measured against the host clock over 30.8 s: 1830 VBlanks (59.4/s) and
26208 guest `KeTickCount` ms (0.85 real time), i.e. 69/s per guest second.
The light configurations measure 60.0/s.

The cycling ISO ran the five configurations and went on into the second
round (re-init after every interval, ONE again after ONE_OR_IMMEDIATE):
summary rows 60.07 / 1.000 / 0, 60.00 / 0.500 / 0, 71.17 / 1.963 / 996
(rate check only), 60.26 / 0.721 / 0, 59.97 / 0.725 / 1000 (VBlank cb/s,
flips per VBlank, immediate per 1000), no fence failure, drained totals equal.
Framebuffer captures showed the report of each configuration (the moving bar
of IMMEDIATE is not torn there: xemu presents whole frames).

### What xemu cannot prove (console checks)

1. The field status: xemu emulates neither a field bit nor the encoder, the
   kernel's `AV_OPTION_GUESS_FIELD` returns 0 there. On a console: the absolute
   parity (does field 0 match the even field on a 480i TV?), behaviour in 480p
   / 720p (-1) and 1080i, and the cost of the two kernel calls.
2. VBlank rates and immediate flips against a real 60 / 50 Hz CRTC, PAL 50 Hz
   (~50 callbacks/s), and the IMMEDIATE VBlank rate check (fails on xemu for
   the reason above).
3. Tearing of IMMEDIATE / ONE_OR_IMMEDIATE flips (`PCRTC_START` latched at
   HSYNC); xemu presents whole frames.
4. Fences on hardware: the back end writing the semaphore only after the ROP
   has written the pixels (xemu flushes the surface first by construction),
   and the CPU seeing the GPU's write in the uncached page.
5. The `pb_reset` fix on hardware (the DMA engine starts within microseconds,
   so the race was rarer there, not impossible).

## Batch 4: quality

Render to texture and multisampling. Validated on xemu with
`samples/pbkit_quality`.

### API added

`pbkit.h`

| Function | When | What |
| --- | --- | --- |
| `int pb_set_render_target(const pb_render_target *rt)` | after `pb_init` | draw into an application buffer until `pb_target_back_buffer`, `pb_target_extra_buffer`, the next `pb_set_render_target` (`NULL` = back buffer) or `pb_finished`. 0, or -1 for an invalid target (nothing changed). |
| `pb_render_target` | | `addr`, `width`, `height`, `color_format` (`NV097_SET_SURFACE_FORMAT_COLOR_LE_*`), `swizzled`, `pitch` (linear), `depth_addr` (`NULL`: none), `depth_pitch` (linear). |
| `DWORD pb_swizzle_offset(x, y, w, h, bytes_per_pixel)` | any time | byte offset of texel (x, y) in a swizzled w x h surface or texture, for CPU access. |
| `pb_set_multisample(int)` / `pb_get_multisample()` | before `pb_init` (asserted) | `PB_MS_NONE` 0 (default), `PB_MS_2X` 1, `PB_MS_4X` 2. |
| `DWORD *pb_multisample_buffer(void)` / `pb_multisample_pitch()` | after `pb_init` | the sample buffer (`NULL` / 0 without multisampling). |

Nothing changes for an application that calls neither `pb_set_render_target`
nor `pb_set_multisample`: same allocations, same tiles, same push buffer bytes
(see "Default path" below).

### Render targets

`set_draw_buffer` became `set_draw_target(color, width, height, pitch, size,
zeta, zeta pitch, zeta size, surface format)`; the back buffers, the extra
buffers, the sample buffer and `pb_set_render_target` all go through it. Like
before it re-bases DMA objects 9 and 11 (color, `PB_SETOUTER` subprogram) and
pushes `SET_SURFACE_PITCH` (color offset 0), `SET_SURFACE_CLIP` and
`SET_SURFACE_FORMAT`.

- **Swizzled**: `NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE` with log2 width / height
  in bits 16-23 / 24-31; width and height powers of two up to 4096; the pitch
  fields are ignored by the hardware, pbkit writes width * Bpp. Sample it as a
  `SZ_*` texture of the same size (normalized coordinates).
- **Linear**: `TYPE_PITCH`, pitch and depth pitch >= width * Bpp, multiples of
  64. Sample it as a `LU_IMAGE_*` texture (texel coordinates). The tile pitch
  rule of batch 2 applies if the application tiles it (`pb_assign_tile`, tile
  #4..#7 are free).
- **Depth buffer**: optional. It uses pbkit's depth format (Z24S8 under
  `PB_DEPTH_NONE`) and the color buffer's layout. DMA object 10 (zeta) is now
  re-based when the depth buffer moves, not only when it appears or
  disappears. A target without one turns the depth and stencil tests off (as
  `PB_DEPTH_NONE` always did); the next target with one turns them back on and
  now also re-enables depth writes (before, only `pb_init` did, which was fine
  while the depth buffer could not come and go).
- The surface clip is the target size; `pb_set_viewport` now clamps to the
  current target instead of the screen. pb_init's window clip (scissor) covers
  the screen, so a target larger than the screen widens it to 4096 and the
  next smaller one narrows it back (only then: the default path pushes
  nothing).
- Checks: format known, address and depth address 64 bytes aligned, mapped 1:1
  below 0x03FFB000 (the first byte: the buffer must be
  `MmAllocateContiguousMemoryEx` memory), sizes, pitches. An invalid target
  returns -1 before anything is pushed.
- **No `pb_fill` / `pb_erase_depth_stencil_buffer` on a swizzled target**: xemu
  asserts (`!(entry.swizzle && pg->clearing)`, `pgraph/gl/surface.c`) and
  exits. Clear it with a quad. The sample does so for both target types.
- Before sampling what the GPU rendered, push `NV097_WAIT_FOR_IDLE`; before the
  CPU reads it, wait for the GPU (a fence or `pb_busy`).

`pb_swizzle_offset` interleaves the bits of x and y (x in bit 0) while both
have bits left, then appends the remaining bits of the larger dimension, the
NV2A / XDK `XGSwizzle` order for non-square sizes.

### Multisampling

`pb_set_multisample(PB_MS_2X)` stores 2 samples per pixel side by side
(`NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2`), `PB_MS_4X` 2x2
(`SQUARE_OFFSET_4`). `pb_init` then allocates:

- a **sample buffer**: width * 2 x height (4X: x height * 2), pitch raised to
  the next tile pitch (640 wide: 5120 for 32 bits, 2560 for 16 bits), tile #3,
  16 Kb granular;
- the **depth stencil buffer at the same scaled size** (tile #1, compressed, as
  before; an application depth buffer must have the scaled pitch and size);
- the frame buffers (scan-out) at the screen size, unchanged.

`pb_target_back_buffer` draws into the sample buffer with the scaled depth
buffer, surface format `| (mode << 12)`. `NV097_SET_ANTI_ALIASING_CONTROL`
(`NV20_TCL_PRIMITIVE_3D_MULTISAMPLE`, 0x1D7C) has always been pushed enabled
with sample mask 0xFFFF by `pb_init`, so nothing changes there.

All coordinates stay in screen pixels: vertices, `pb_set_viewport`, the
surface clip, window clip, `pb_fill` and the depth clear rectangles. The GPU
scales them for an anti-aliased surface. xemu does the same
(`pgraph_apply_anti_aliasing_factor` on clip, scissor and clear rectangles;
`surfaceSize` = surface size / AA factor in the vertex shader), which is why
games' 1x coordinates render correctly there. `pb_back_buffer_width/height/pitch`
and `pb_back_buffer()` keep describing the scan-out buffer.

**Resolve** (`pb_finished`, before the `PB_FINISHED` subprogram): pbkit pushes
`NV097_WAIT_FOR_IDLE`, targets the rotating back buffer (screen size, no
anti-aliasing, no depth buffer), and draws a screen sized quad textured with
the sample buffer as a `LU_IMAGE_*` texture of the scaled size (format
matching the color format), bilinear, clamped. Pixel center x + 0.5 maps to
texel coordinate 2x + 1 (and 2y + 1 for 4X), the corner shared by the pixel's
samples, where bilinear filtering weighs them equally: a box filter. Vertex
program `MOV oT0, v9; MOV oPos, v0` in transform program slots 134-135,
inline vertices (`SET_VERTEX_DATA4F_M` attributes 9 then 0), final combiner
output = texture 0, no blending / alpha test / culling / logic op, color mask
all. Then the flip is queued exactly as before, so every presentation interval
of batch 3 behaves the same (measured below), and `pb_target_back_buffer`
points the GPU at the sample buffer again.

The resolve changes GPU state the application may rely on. Afterwards: texture
stage 0 is disabled (pbkit default), stages 1-3 disabled, culling on (pbkit
default), transform program start 0, execution mode program, depth / stencil
tests and depth writes on; changed and not restored: shader stage program,
combiners, blending, alpha test, logic op, color mask, polygon modes, the
inline values of vertex attributes 0 and 9, transform program slots 134-135.
Set your state again after `pb_finished` (the sample re-pushes its shaders
and vertex arrays every frame).

Left out (optional in the spec):

- Quincunx, gaussian and the 2x horizontal-linear filters, and
  `SetScreenSpaceOffset`. The box filter is the only one. Quincunx / gaussian
  would be extra taps (texture stages 1-3 at offsets, combiner weights) or the
  texture unit's convolution kernel (`NV097_SET_TEXTURE_FILTER` bits 13-15,
  MAG = 4), unverified on xemu and hardware.
- Supersampling (CENTER_1 surface at a scaled size with a scaled viewport, the
  XDK `*_SUPERSAMPLE_*` modes).
- With multisampling, `pb_target_extra_buffer` draws at screen size without
  anti-aliasing and without a depth buffer (the depth buffer is scaled), and
  `pb_show_depth_screen` shows the scaled depth buffer with the scan-out pitch
  (sheared).

### Default path

`set_draw_target` for the back buffer pushes the same blocks as
`set_draw_buffer` did: color DMA 9 and 11, the zeta block only on the first
call after `pb_init` (`pb_DepthStencilLast` -2), with the same methods
(`STENCIL_ENABLE` is now `flag && Z24S8`, identical when there is a depth
buffer and for `PB_DEPTH_NONE`; the added `DEPTH_WRITE_ENABLE 1` is only
pushed on a none -> present transition, which needs a render target), then
pitch / clip / CONTROL0 / format. No window clip block (no larger target), no
resolve, no allocation (`pb_MSMode` = `PB_MS_NONE`, scale 1: the depth pitch
and size expressions multiply by 1). `pb_set_viewport` clamps to the target
size, equal to the screen size. Re-run on xemu with this batch's library:
`samples/pbkit_memory` cycling, all six configurations PASS (fails 0), and
`samples/pbkit_sync` cycling, the batch 3 summary rows again (60.05 / 1.000 /
0, 60.04 / 0.500 / 0, 72.98 / 1.901 / 994 with only the known xemu VBlank
rate failure, 60.56 / 0.717 / 0, 59.97 / 0.725 / 1000; no fence failure).

### samples/pbkit_quality

Five configurations cycled with `pb_kill` / `XVideoSetMode` / `pb_init`
(`make CONFIG=n` for one, delete `main.obj`, `bin/default.xbe` and the ISO
first), 640x480 32 bpp, A8R8G8B8, Z24S8, triple buffering.

- **0 RTT swizzled**, **1 RTT linear + depth**: a 128x128 A8R8G8B8 target
  (swizzled / pitch 512) is cleared with a far quad, gets four colored
  quadrants (z 0.1) and then a magenta square over the center (z 0.9). The CPU
  reads five texels (`pb_swizzle_offset` for config 0): the quadrants, and at
  (60, 60) magenta without a depth buffer, the red quadrant with one (the
  target's own depth buffer hides the square). The target is then sampled
  (`SZ_A8R8G8B8` 2^7 x 2^7 / `LU_IMAGE_A8R8G8B8`, nearest) on a 256x256 quad
  in the back buffer, whose pixels at the same texels are read back.
  Also: `pb_swizzle_offset` spot checks (square and 8x2 / 2x8), an invalid
  target (swizzled, width 100) returns -1.
- **2 MS 2x**, **3 MS 4x**, **4 MS off**: a white triangle on black,
  (300,200) (600.1,200) (300,500.1); its hypotenuse x + y = 800.1 crosses
  pixels (x, 799 - x) 0.1 pixel past their center. Without multisampling
  those 240 pixels (x 330..569) are pure white; with 2x / 4x they must be
  blended (green in 8..247 for >= 90 % of them). Also: pitches (frame buffer
  2560, depth 2560 * scale, sample buffer 5120), a pixel fully inside / outside
  is pure, the raw samples of edge pixel (450, 349) in the sample buffer, and
  flips per VBlank over 2 s for each presentation interval with the resolve
  (ONE 0.9-1.01, TWO 0.45-0.52, IMMEDIATE >= 0.9, ONE_OR_IMMEDIATE 0.9-1.01,
  swap callback count / VBlank counter).

Results: `pb_print`, `DbgPrint` and a `PBQUALRS` tagged block in `.data`
(0x35000 in this build) with one summary row per configuration.

xemu v0.8.139, 2026-10-26, one `CONFIG=n` ISO per row, then the cycling ISO:

| # | Config | Readback | Verdict |
| --- | --- | --- | --- |
| 0 | RTT swizzled | texels FF0000 00FF00 0000FF FFFF00, (60,60) FF00FF; quad pixels identical; swizzle checks ok, reject -1 | PASS |
| 1 | RTT linear + depth | texels FF0000 00FF00 0000FF FFFF00, (60,60) FF0000 (square hidden); quad identical | PASS |
| 2 | MS 2x | pitches 2560 / 5120 / 5120; edge 240 / 240 blended, all 0x808080; samples FFFFFF 000000; flips/VBlank 1.000 0.500 17.45 1.000 | PASS |
| 3 | MS 4x | pitches 2560 / 5120 / 5120; edge 240 / 240 blended, all 0xBFBFBF; samples FFFFFF FFFFFF FFFFFF 000000; flips/VBlank 1.000 0.500 16.75 1.008 | PASS |
| 4 | MS off | pitches 2560 / 2560, no sample buffer; edge 240 / 240 pure FFFFFF; flips/VBlank 1.000 0.500 34.94 1.000 | PASS |

The cycling ISO passed all five and config 0 / 1 again in the second round
(re-init with and without the sample buffer). Framebuffer captures show the
sampled quadrants (the magenta square only in config 0) and the triangle with
a grey (2x) / light grey (4x) / hard (off) edge.

How xemu resolves: it renders an anti-aliased surface at the scaled size
(supersampling: the pixel shader runs per sample) and stores it in guest
memory at that size, two (2x1) or four (2x2) texels per pixel, the layout the
resolve reads. The CPU reads the raw samples there (config 2 / 3). Its sample
positions are the centers of the scaled grid ((x + 0.25, y + 0.5) and
(x + 0.75, y + 0.5) for 2x; the four quarter centers for 4x), hence exactly
1/2 and 3/4 coverage on this edge. IMMEDIATE flips per VBlank are high
because these frames are tiny and xemu presents whole frames; the resolve
halves that rate (34.9 -> 17.4).

### What xemu cannot prove (console checks)

1. The hardware sample positions of CENTER_CORNER_2 and SQUARE_OFFSET_4 (the
   name suggests center + corner for 2x) and multisampling proper (one shade
   per pixel, coverage per sample) instead of xemu's supersampling: the
   sample checks only require a blend, not 1/2 or 3/4.
2. That the hardware scales surface clip, window clip and clear rectangles by
   the AA factor (xemu does; if not, clears and clips cover a quarter / half
   of the sample buffer) and that vertex positions are 1x.
3. The resolve's texel alignment: hardware pixel centers or the rasterizer's
   1/16 subpixel snapping may shift the box filter off the sample corner
   (unequal weights, not a wrong image).
4. Whether `NV097_WAIT_FOR_IDLE` is enough before texturing from a surface the
   GPU just rendered (texture cache), for the resolve and for render targets.
   xemu synchronizes surfaces and textures by construction.
5. `NV097_CLEAR_SURFACE` on a swizzled surface (asserts in xemu; the XDK does
   not allow `Clear` on swizzled targets either, as far as known).
6. A swizzled target with a depth buffer (the zeta surface swizzled the same
   way): not exercised by the sample.
7. Tile #3 for the sample buffer (tiled color surface at pitch 5120) and the
   scaled compressed depth buffer: xemu ignores tiling.

## Batch 5: display

Present flags, dashboard video flags, TV filters, video overlay and persisted
display. Validated on xemu with `samples/pbkit_display`.

### API added

`hal/video.h` (`XVideoSetMode` is unchanged; it now also records the AV mode
and scan type it set)

| Function | What |
| --- | --- |
| `BOOL XVideoSetModeEx(w, h, bpp, refresh, flags)` | `XVideoSetMode` with `XVIDEO_FLAG_*`; flags 0 is `XVideoSetMode`. FALSE when no mode matches (with flags, the current mode is kept). |
| `XVIDEO_FLAG_WIDESCREEN` 0x10, `_INTERLACED` 0x20, `_PROGRESSIVE` 0x40, `_FIELD` 0x80, `_10X11PIXELASPECTRATIO` 0x100 | the values of the XDK `D3DPRESENTFLAG_*`. |
| `DWORD XVideoGetModeFlags(void)` | flags of the current mode: the requested WIDESCREEN / 10X11 plus the scan type of the mode (INTERLACED or PROGRESSIVE). |
| `DWORD XVideoGetAvMode(void)` | the kernel AV mode word of the current mode. |
| `DWORD XVideoGetVideoFlags(void)` | the dashboard settings, EEPROM `XC_VIDEO` (`ExQueryNonVolatileSetting`): `VIDEO_WIDESCREEN` 0x10000, `VIDEO_MODE_720P` 0x20000, `VIDEO_MODE_1080I` 0x40000, `VIDEO_MODE_480P` 0x80000, `VIDEO_LETTERBOX` 0x100000, `VIDEO_60Hz` 0x400000 (PAL 60). 0 when unset or unreadable. |
| `BOOL XVideoPersistDisplay(void)` | keeps the picture across `XLaunchXBE` (see below). |
| `XVIDEO_AV_MODE_OUT_MASK` 0xC0000000, `_SCART` 0x20000000, `_WSS` 0x10000000 | AV mode word fields. |

`pbkit.h`

| Function | What |
| --- | --- |
| `int pb_overlay_update(const pb_overlay *ov)` | checks the description and queues it; pbkit's VBlank interrupt programs PVIDEO buffer 0 at the next VBlank (the last update queued before a VBlank wins). 0, or -1 (nothing queued). |
| `pb_overlay` | `surface`, `pitch`, `format` (`PB_OVERLAY_UYVY` 0 / `PB_OVERLAY_YUY2` 1), `src_x/y/width/height`, `dst_x/y/width/height`, `color_key_enable`, `color_key` (0xRRGGBB), `bt709`. |
| `void pb_overlay_disable(void)` | STOP at the next VBlank (`pb_kill` stops it at once). |
| `int pb_overlay_update_pending(void)` | 1 while a queued update / disable waits for its VBlank (the D3D `GetOverlayUpdateStatus` role). |
| `DWORD pb_overlay_last_vbl(void)` | VBlank count at which the last update / disable was programmed. |

`pb_kill` keeps the CRTC on a persisted display (`AvGetSavedDataAddress()`
set) instead of restoring the `XVideoSetMode` frame buffer.

### Present flags

The AV mode word (`AvSetDisplayMode`, `AV_OPTION_QUERY_MODE`): bits 31:30
output (0 / 1 SDTV, 2 HDTV, 3 VGA), bit 29 SCART (RGB), bit 28 WSS (16:9
wide screen signalling), bits 27:24 DAC disables, bits 23:16 register table,
15:8 CRTC table, 6:0 TV table index. Layout, mode values and the
`AV_FLAGS_*` / `AV_OPTION_*` numbers: Cxbx-Reloaded
`src/core/kernel/exports/EmuKrnlAvModes.h` and
`src/core/kernel/common/xbox.h`, cross-checked with nxdk's own `vidModes`
table (same words).

- INTERLACED / PROGRESSIVE pick from the mode table by scan type: SDTV
  outputs are interlaced, HDTV output is interlaced only for TV index 3
  (1080i), VGA is progressive. With the HDTV pack at 480 lines this selects
  480i YPrPb (0x0801010D NTSC) or 480p (0x88070701). The HDTV modes still need
  their dashboard flag (480p / 720p / 1080i); a scan type the pack cannot
  output fails. No flag: the mode `XVideoSetMode` picks (480p preferred when
  enabled).
- WIDESCREEN ORs bit 28 into SDTV and 480i YPrPb modes, the kernel's
  `AV_MODE_*_16x9` modes. HDTV progressive and VGA modes have no 16:9 variant
  in the kernel table (the display mode table in Cxbx-Reloaded's
  `EmuKrnlAvModes.h` maps widescreen 480p / 720p / 1080i to the same mode
  words): the flag only tells the game to render anamorphic. It is not checked against the dashboard flag
  (`XVideoGetVideoFlags() & VIDEO_WIDESCREEN` is the game's decision, as in the
  XDK).
- 10X11PIXELASPECTRATIO maps a 640x480 mode to the kernel's FPAR mode with the
  same output bits: register / CRTC / TV indices 0F/0D/0F (NTSC-M), 0F/0D/10
  (NTSC-J), 0F/0D/11 (PAL-60), 0F/0D/12 (480i YPrPb), 10/0E/13 (PAL-I),
  10/0E/18 (PAL-I YPrPb), 0F/0D/1B (PAL-60 YPrPb), 11/0F/01 (480p). Other
  sizes and VGA fail. The frame buffer stays 640x480; how the FPAR timings
  look on a TV is console-only.
- FIELD fails. Field rendering (the XDK's 640x240 / 720x240 / 1920x540 modes
  with `AV_FLAGS_FIELD`, same AV mode word as the interlaced mode) needs a
  half height frame buffer scanned out once per field with the field parity
  driving which buffer is shown: a CRTC set up for line doubling or a field
  start offset, which no reachable source documents, plus a flip per VBlank
  matched to `pb_get_display_field_status`. Left out.

### TV filters

No new code: `XVideoSetFlickerFilter(0..5)` (`AV_OPTION_FLICKER_FILTER`, 11)
and `XVideoSetSoftenFilter(BOOL)` (`AV_OPTION_ENABLE_LUMA_FILTER`, 14) pass
the value to the kernel, which programs the encoder over SMBus. The encoder
depends on the board: Conexant CX25871 (SMBus 0x45, write address 0x8A;
1.0-1.3), Focus FS454 (0x6A / 0xD4; 1.4-1.5), Xcalibur (0x70 / 0xE0; 1.6)
(xboxdevwiki "SMBus", "Xbox Hardware Revisions"). The kernel's mapping of the
levels per encoder is not documented in a reachable source; xemu emulates the
Conexant register file only (writes are stored and read back, no filter is
applied, `hw/xbox/smbus_cx25871.c`), so the sample records what the kernel
(4627, the BIOS xemu runs here) writes there:

| Setting | Conexant registers written (value after) |
| --- | --- |
| flicker 0 | 0xC8 = 0xC0 (0x34 stays 0x80) |
| flicker 1 | 0x34 = 0x00, 0xC8 = 0x89 |
| flicker 2 | 0xC8 = 0x92 |
| flicker 3 | 0xC8 = 0x9B |
| flicker 4 | 0xC8 = 0x80 |
| flicker 5 (XVideoInit default) | 0x34 = 0x80 (0xC8 stays 0x80) |
| soften on | 0x96 = 0x16 |
| soften off | 0x96 = 0x06 |

`AV_QUERY_ENCODER_TYPE` (16) returns 0 there (Conexant; the values for Focus
and Xcalibur are unknown). The effect of each level (vertical filter taps,
luma low-pass) and the Focus / Xcalibur programming are console-only.

### Video overlay (NV_PVIDEO)

Registers (offsets in the GPU MMIO, buffer 1 at +4): BUFFER 0x8700 (bit 0
buffer 0 in use, bit 4 buffer 1), STOP 0x8704 (bit 0 stops), BASE 0x8900,
LIMIT 0x8908, LUMINANCE 0x8910, CHROMINANCE 0x8918, OFFSET 0x8920, SIZE_IN
0x8928 (height << 16 | width), POINT_IN 0x8930 (t << 20 | s << 4, 12.4
fixed point), DS_DX 0x8938 / DT_DY 0x8940 (src / dst << 20; 1 << 20 =
unity), POINT_OUT 0x8948 (y << 16 | x), SIZE_OUT 0x8950, FORMAT 0x8958
(pitch bits 12:0, bit 16 color LE_CR8YB8CB8YA8 = YUY2 / 0 = UYVY, bit 20
color key, bit 24 BT.709 matrix), COLOR_KEY 0x8B00. Sources: nouveau
`drivers/gpu/drm/nouveau/dispnv04/nvreg.h` and `overlay.c`
(`nv10_update_plane`, `nv10_disable_plane`), the nouveau X driver
`nv10_xv_ovl.c` (POINT_IN, color key), xemu `hw/xbox/nv2a/nv2a_regs.h`,
`pvideo.c`, `pgraph/gl/display.c`. pbkit's `pb_init` already sets
LUMINANCE / CHROMINANCE to unity and SIZE_IN to 0xFFFFFFFF.

`pb_overlay_update` programs BASE 0, LIMIT 0x03FFFFFF, OFFSET = the
surface's physical address, SIZE_IN = the source rectangle's far corner,
POINT_IN its origin, DS_DX / DT_DY, POINT_OUT, SIZE_OUT, FORMAT, COLOR_KEY,
then STOP 0 and BUFFER = 1, from the VBlank interrupt (ISR, next to the flip).
Checks: 2 bytes per pixel surface in contiguous memory below 64 Mb, 64 byte
aligned, pitch a multiple of 64 (the X driver's rounding) up to 8128, even
source x / width, source within 2046 x 2047 (SIZE_IN fields), destination
fields below 4096, at most 8x downscale (nouveau's `verify_scaling` for
NV1x/NV2x). Only buffer 0 is used: xemu displays buffer 0 only, and the
application double buffers its surfaces instead (write the hidden surface,
update, reuse the old one once `pb_overlay_update_pending()` is 0).

xemu emulates PVIDEO (checked): the overlay is composited when xemu
presents the frame (`pgraph/gl/display.c`), so it shows in the xemu window
but not in the frame buffer, nor in a CRTC capture or a GPU / CPU readback.
Limits there: buffer 0 only, YUY2 only (a UYVY surface aborts xemu:
`Assertion failed: in_color == NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8`,
seen with `make CONFIG=1 OV_UYVY=1`), BT.601 studio range always
(LUMINANCE / CHROMINANCE and the BT.709 bit are ignored), exact color key
compare, SIZE_IN 0xFFFFFFFF hides the overlay, STOP reads 0 and a STOP
write with bit 0 clears BUFFER; registers read back as written. With the
bilinear filter and repeat wrap of its texture, the overlay's first line
and column show a sliver of the opposite edge.

### Persisted display

`XVideoPersistDisplay` copies the frame buffer the CRTC shows
(`NV_PCRTC_START`; pitch from CR13, CR19 bits 7:5 and CR25 bit 5 in 8 byte
units, the way `AvSetDisplayMode` writes it) into a new contiguous
allocation, points the CRTC at the copy, `MmPersistContiguousMemory(TRUE)`,
`AvSetSavedDataAddress(copy)`. A previous persisted copy of the same title is
freed first. The receiver is the existing `XVideoInit`: it frees the saved
buffer on the next title's first mode set. Cxbx-Reloaded documents the same
steps for the XDK's `D3DDevice_PersistDisplay` (`Direct3D9.cpp`). Flow with
pbkit: last frame on screen (nothing queued), `XVideoPersistDisplay()`,
`pb_kill()`, `XLaunchXBE` / `XLaunchXBEEx`.

Observed on xemu (4627 kernel), the sample relaunching its own image:

- the copy (0x83850000, physical 0x03850000) is on screen before the reboot;
- after the reboot `AvGetSavedDataAddress()` returns 0x8003D000 (physical
  0x0003D000, right after the kernel image): the kernel moved the persisted
  pages. The pixels are intact except dword 1, which the kernel overwrote
  with 0xFC7DD000;
- the CRTC then scans out 0x007DD000 (black), not the saved buffer: the
  picture is not kept through the reboot by the kernel. The receiving title
  can show the saved buffer again (the sample points `NV_PCRTC_START` at it
  until its first mode set) and `XVideoSetMode` frees it
  (`AvGetSavedDataAddress()` NULL afterwards).

The overwritten dword and the moved CRTC suggest the kernel reads the saved
block as a structure with a frame buffer address in its second dword (the
XDK's surface header?). Its layout is not documented in a reachable source,
so `XVideoPersistDisplay` hands over the raw frame buffer and the black gap
during the reboot is left as found.

### samples/pbkit_display

Four configurations: `make CONFIG=n` runs one (delete `main.obj`,
`bin/default.xbe` and the ISO first); the cycling ISO starts with 3 (it
relaunches itself) and then cycles 0, 1, 2. 640x480 32 bpp, A8R8G8B8, Z24S8,
double buffering. Results: `pb_print`, `DbgPrint` and a `PBDISPRS` tagged
block in `.data` (0x37010 in this build) with one summary row per
configuration.

- **0 mode flags**: eleven `XVideoSetModeEx` calls (flags 0, I, P, W, I+W,
  10X11, 10X11+W+I, FIELD, 10X11 at 720x480, I+P, unknown 0x200) against the
  pack and dashboard flags: set or refused as expected, kernel AV mode =
  `XVideoGetAvMode()`, scan type, WSS bit with WIDESCREEN only, FPAR CRTC
  index, a refused call keeps the current mode. Then the widescreen mode is
  shown and the field status is sampled over 16 VBlanks.
- **1 overlay**: 160x120 YUY2 100 % color bars (BT.601 studio range) scaled 2x
  to 320x240 at (160, 216) into a magenta (0xFF00FF) keyed rectangle; a static
  green quad and a moving yellow bar inside the rectangle hide it. Six invalid
  descriptions rejected, register readback against the expected values,
  update latency in VBlanks, 20 double buffered swaps (bar order reversed and
  a moving marker), the frame buffer under the overlay still magenta.
- **2 TV filters**: flicker 0..5, soften on / off, 1.5 s each on one pixel
  lines and an 8 pixel checkerboard; SMBus probe of the three encoders,
  `AV_QUERY_ENCODER_TYPE`, Conexant registers changed per step. Pass = the
  calls return (the filters are visible on a TV only).
- **3 persist display**: four colored quadrants and a white center square,
  `XVideoPersistDisplay`, `pb_kill`, `XLaunchXBEEx(<own image path>, data)`;
  the second run checks `AvGetSavedDataAddress()` is set, the pixels at five
  points and a checksum of the buffer it points to (dword 1 excluded) against
  the launch data, records the CRTC at boot, shows the saved buffer for 25 s,
  then `XVideoSetMode` must free it.

xemu v0.8.139, 2026-10-26, HDTV pack, NTSC-M, dashboard video flags 0 (no
480p / 720p / 1080i, no widescreen), one `CONFIG=n` ISO per row, then the
cycling ISO:

| # | Config | xemu result | Verdict |
| --- | --- | --- | --- |
| 0 | mode flags | 0: 0x0801010D (480i YPrPb) I; I: 0x0801010D; P: refused (480p off in the dashboard); W: 0x1801010D; I+W: 0x1801010D; 10X11: 0x080F0D12; 10X11+W+I: 0x180F0D12; FIELD, 10X11 720x480, I+P, 0x200: refused, mode kept. Shown 0x1801010D, field 0101010101010101 | PASS |
| 1 | overlay YUY2 2x key | reject mask 0x3F; latency 1 VBlank (20 swaps: 1..1); BUFFER 1, STOP 0, BASE 0, LIMIT 0x03FFFFFF, OFFSET 0x03972000, SIZE_IN 0x007800A0, POINT_IN 0, DS_DX / DT_DY 0x80000, POINT_OUT 0x00D800A0, SIZE_OUT 0x00F00140, FORMAT 0x00110140, COLOR_KEY 0xFF00FF; frame buffer under it 0xFF00FF. Window capture: bars 2x inside the magenta rectangle, hidden under the green quad and the yellow bar; CRTC capture: magenta only | PASS |
| 2 | TV filters | Conexant only answers on SMBus, encoder type 0, register writes in the table above | PASS (on-screen only) |
| 3 | persist display | first run: copy shown, CRTC = copy; second run: saved 0x8003D000 (sent 0x83850000), pixels FF0000 00FF00 0000FF FFFF00 FFFFFF equal, checksum 0x7D395350 equal, CRTC at boot 0x007DD000 (black), saved buffer shown again (window and CRTC captures: the quadrants), NULL after `XVideoSetMode` | PASS |

The cycling ISO ran 3 (relaunch included), 0, 1, 2 with all four summary
rows PASS and went on into the second round. `UYVY` (`OV_UYVY=1`) stops xemu
with the assertion quoted above. `samples/pbkit_sync` cycling with this
batch's library (the VBlank interrupt now also services the overlay): the
batch 3 summary rows again (60.06 / 1.000 / 0, 59.96 / 0.500 / 0, 68.19 /
1.875 / 994 with only the known xemu VBlank rate failure, 60.56 / 0.717 / 0,
59.98 / 0.723 / 997).

### What xemu cannot prove (console checks)

1. The encoder side of every flag: WSS signalling with WIDESCREEN on a 4:3 /
   16:9 TV, the FPAR modes' 10:11 pixels, 480p through the HDTV pack with the
   dashboard flag set (refused on this xemu EEPROM, so the progressive path ran
   only as a refusal), PAL 50 Hz and SCART RGB variants. xemu emulates no
   encoder; it only stores the mode word.
2. Field rendering (not implemented, see above).
3. The TV filters: flicker levels and soften on a TV, the Conexant register
   meaning, and what the kernel writes on Focus (1.4-1.5) and Xcalibur (1.6)
   boards; `AV_QUERY_ENCODER_TYPE` values there.
4. The overlay on hardware: UYVY, the BT.709 bit, LUMINANCE / CHROMINANCE,
   color key compare in the 32 bit frame buffer, scaling filter quality, the
   8x downscale limit, whether the hardware latches buffer 0's registers at
   the VBlank by itself (pbkit writes them in the VBlank ISR, so tearing is
   avoided either way), BUFFER / STOP readback, overlay bandwidth with 3D
   load, and the overlay in 720p / 1080i modes.
5. The persisted display on retail kernels (other than the 4627 BIOS xemu
   runs here): the relocation, the overwritten dword, and whether the
   picture stays on screen through the reboot when the saved block has the
   layout the XDK uses.
