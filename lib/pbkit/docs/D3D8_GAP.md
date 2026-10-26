# pbkit D3D8 gap, batch 1: surface formats

Depth formats (Z16 / Z24S8, fixed or floating point), W-buffering, 16 bits
color with dithering, and the tile API, for the `nxdk-scene3d` pbkit.
Validated on xemu with `samples/pbkit_formats`; the points a console must still
confirm are listed at the end.

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
| `pb_depth_stencil_buffer()` | CPU address of the (shared) depth stencil buffer. |
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
   the CPU readback of a compressed depth tile is transparent; also the tile
   pitch (1536 from the pitch table) vs the surface pitch (1280) pbkit uses
   for 640 wide 16 bits buffers, inherited from the color buffer code.
4. Dithering (`NV097_SET_DITHER_ENABLE`): xemu maps it to `GL_DITHER`, which is
   a no-op for exactly representable colors.
5. `STENCIL_WRITE_ENABLE` = 0 with Z16 (xemu has no stencil attachment then).
