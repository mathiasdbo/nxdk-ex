// pbKit framebuffer management functions

// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2007 Guillaume Lamonoca
// SPDX-FileCopyrightText: 2017 espes
// SPDX-FileCopyrightText: 2017-2020 Jannik Vogel
// SPDX-FileCopyrightText: 2018-2022 Stefan Schmidt
// SPDX-FileCopyrightText: 2019 Lucas Jansson
// SPDX-FileCopyrightText: 2021-2025 Erik Abair

#ifndef PBKIT_FRAMEBUFFER_H
#define PBKIT_FRAMEBUFFER_H

#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif

// Sets the framebuffer's color surface format (call it before pb_init).
void pb_set_color_format (unsigned int fmt, bool swizzled);

// No depth stencil buffer at all (pb_set_depth_format(PB_DEPTH_NONE)): pb_init
// allocates nothing and assigns no tile #1, depth and stencil tests are off,
// pb_erase_depth_stencil_buffer and pb_show_depth_screen do nothing and
// pb_depth_stencil_buffer() returns NULL. Do not enable the depth or stencil
// test yourself in that mode (the GPU has no depth surface to use).
#define PB_DEPTH_NONE 0

// Sets the depth stencil surface format, NV097_SET_SURFACE_FORMAT_ZETA_Z16,
// NV097_SET_SURFACE_FORMAT_ZETA_Z24S8 (default) or PB_DEPTH_NONE. Call it
// before pb_init, the buffer is allocated there (Z16: 2 bytes/pixel, no
// stencil plane).
void pb_set_depth_format (unsigned int fmt);
unsigned int pb_get_depth_format (void);

// Floating point depth values (NV097_SET_CONTROL0_Z_FORMAT), default off. May
// be called at any time; pb_init pushes it and a later call re-pushes CONTROL0
// and the viewport z range. See pb_depth_scale for the z range per format.
void pb_set_depth_float (bool enable);
bool pb_get_depth_float (void);

// W-buffering (NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE): the depth written and
// tested is the interpolated vertex w instead of z. pbkit has always enabled
// it, so the default is on; pass false for a plain Z-buffer. Any time.
void pb_set_w_buffer (bool enable);
bool pb_get_w_buffer (void);

// Tile region of the depth stencil buffer, set before pb_init (console checked
// with samples/pbkit_formats, 2026-10-27):
// - PB_DEPTH_TILE_Z (default): a compressed Z tile. Fastest for the GPU, but
//   the CPU sees the GPU's internal Z layout through pb_depth_stencil_buffer(),
//   not a linear image (xemu hides this: it reads back linearly in any mode).
// - PB_DEPTH_TILE_LINEAR: a tile region without the Z flags. The GPU still
//   tiles its accesses and the CPU reads and writes the buffer linearly.
// - PB_DEPTH_TILE_NONE: no tile region. Linear for the CPU too.
// The depth test itself is right in every mode.
#define PB_DEPTH_TILE_Z      0
#define PB_DEPTH_TILE_LINEAR 1
#define PB_DEPTH_TILE_NONE   2
void pb_set_depth_tile_mode (int mode);
int pb_get_depth_tile_mode (void);

// The depth value 1.0f maps to in pb_set_viewport: 0xFFFFFF (Z24S8), 0xFFFF
// (Z16), 1.0E30 (Z24S8 float), 511.9375 (Z16 float). Valid after pb_init.
float pb_depth_scale (void);

// Color dithering (NV097_SET_DITHER_ENABLE). Default: on for the 16 bits color
// formats, off for the 32 bits ones. Any time.
void pb_set_dither (bool enable);
bool pb_get_dither (void);

#if defined(__cplusplus)
}
#endif

#endif // PBKIT_FRAMEBUFFER_H
