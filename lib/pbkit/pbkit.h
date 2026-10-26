// clang-format off

//pbKit header

// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2007 Guillaume Lamonoca
// SPDX-FileCopyrightText: 2017 espes
// SPDX-FileCopyrightText: 2017-2020 Jannik Vogel
// SPDX-FileCopyrightText: 2018-2022 Stefan Schmidt
// SPDX-FileCopyrightText: 2019 Lucas Jansson
// SPDX-FileCopyrightText: 2021 Erik Abair

#ifndef _PBKIT_H_
#define _PBKIT_H_

#if defined(__cplusplus)
extern "C"
{
#endif

#include <xboxkrnl/xboxkrnl.h>
#include <stdint.h>
#if !defined(__cplusplus)
#include <stdbool.h>
#endif

#include "outer.h"
#include "nv_objects.h"
#include "nv_regs.h"
#include "pbkit_gamma.h"
#include "pbkit_dma.h"
#include "pbkit_draw.h"
#include "pbkit_framebuffer.h"
#include "pbkit_print.h"
#include "pbkit_pushbuffer.h"

void    pb_show_front_screen(void); //shows scene (allows VBL synced screen swapping)
void    pb_show_debug_screen(void); //shows debug screen (default openxdk+SDL buffer)
void    pb_show_depth_screen(void); //shows depth screen (depth stencil buffer)

DWORD   pb_get_vbl_counter(void);

//VBlank and swap callbacks (NULL removes them). Both run in pbkit's DPC (DISPATCH_LEVEL), right
//after the VBlank handling (swap: after a frame was put on screen, also from the DPC for
//PB_PRESENT_INTERVAL_IMMEDIATE flips). They must be brief, must not call pbkit and must save and
//restore the FPU state themselves if they use floating point (KeSaveFloatingPointState).
//vbl_count is pb_get_vbl_counter() at that VBlank, frame the number of frames shown since pb_init.
void    pb_set_vblank_callback(void (*cb)(DWORD vbl_count));
void    pb_set_swap_callback(void (*cb)(DWORD frame, DWORD vbl_count));
//Display field status: *vbl_count = VBlank counter, *field = 0 (even) or 1 (odd) field being
//scanned out in an interlaced mode, -1 when progressive or unknown. Either pointer may be NULL.
void    pb_get_display_field_status(DWORD *vbl_count, int *field);

//Presentation interval (D3DPRESENT_INTERVAL_*): when pb_finished() frames are put on screen.
//Can be changed at any time; it lasts across pb_kill/pb_init.
#define PB_PRESENT_INTERVAL_ONE             1   //at the next VBlank (default, the classic pbkit behaviour)
#define PB_PRESENT_INTERVAL_TWO             2   //at a VBlank, at most every second VBlank
#define PB_PRESENT_INTERVAL_IMMEDIATE       0   //as soon as the GPU has finished the frame (tearing)
#define PB_PRESENT_INTERVAL_ONE_OR_IMMEDIATE 3  //at the next VBlank, immediately if the frame missed one
void    pb_set_presentation_interval(int interval);
int     pb_get_presentation_interval(void);

//Fences: pb_insert_fence() queues a GPU write of a new 32-bit value (wrap-safe compare) once
//everything pushed before it has been rendered (NV097_BACK_END_WRITE_SEMAPHORE_RELEASE). Like
//pb_end_deferred it does not kick the push buffer; pb_is_fence_pending/pb_block_on_fence kick it
//when the fence has not passed. A fence that was never inserted is not pending.
DWORD   pb_insert_fence(void);
bool    pb_is_fence_pending(DWORD fence);
void    pb_block_on_fence(DWORD fence);     //busy waits until the GPU has passed the fence
DWORD   pb_wait_for_vbl(void);  //waits for VBL, returns VBL counter value as bonus info
                //you can avoid calling it and try to draw a lot in advance
                //use it if you need strong synchronization with controls

//ALWAYS use this at beginning of frame or you may lose one third of performance because
//automatic compression algorithm for tile #1 can't afford any garbage left behind...
void    pb_erase_depth_stencil_buffer(int x, int y, int w, int h);

void    pb_reset(void); //forces a jump to push buffer head (do it at frame start)
int pb_finished(void);  //prepare screen swapping at VBlank (do it at frame end)
                //if it returns 1 it failed (too early, just wait & retry)
                //that means you can draw more details in your scene

void pb_wait_until_gr_not_busy(void);
DWORD pb_wait_until_tiles_not_busy(void);

void    pb_extra_buffers(int n);//requests additional back buffers (default is 0) (call it before pb_init)
void    pb_back_buffers(int n); //2 (default): triple buffering; 1: double buffering, one frame buffer less (call it before pb_init)
//Application supplied buffers (call before pb_init; pbkit uses them instead of allocating and
//never frees them; pass NULL to go back to pbkit's own buffers). Allocate them with
//  MmAllocateContiguousMemoryEx(size, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE)
//(16 Kb aligned, below 0x03FFB000, write combined: the CPU side mapping is the app's job).
//pb_init checks 64 bytes alignment and physical contiguity and fails with -12 (frame buffers)
//or -13 (depth stencil buffer).
//- frame buffers: count must be pb_back_buffers()+1 (3 by default), pitch the scan-out pitch
//  pbkit uses itself, width*Bpp rounded to 64 (0 = that pitch); the CRTC is not reprogrammed.
//  Tiled (tile #0) when the pitch is a tile pitch and the buffers follow each other in one
//  16 Kb aligned block whose size is a 16 Kb multiple (e.g. one 3*640*480*4 allocation).
//- depth stencil buffer: any pitch >= width*Bpp (2 or 4 bytes) that is a 64 bytes multiple.
//  Tiled and compressed (tile #1) when the pitch is a tile pitch (1536 for 640 wide Z16, 2560
//  for Z24S8), the address 16 Kb aligned and pitch*height a 16 Kb multiple.
void    pb_set_frame_buffers(DWORD *addrs[], int count, DWORD pitch);
void    pb_set_depth_stencil_buffer(DWORD *addr, DWORD pitch);
void    pb_release_debug_screen(void); //after pb_init: frees XVideoSetMode's own frame buffer (the debug screen); pbkit then never shows it and prints its reports with DbgPrint (nxdk's debugPrint must not be used after this either)
void    pb_wait_for_flip(void); //double buffering: waits until the frames pb_finished queued are on screen (call it before drawing the next frame; no-op with triple buffering)
void    pb_size(DWORD size);    //sets push buffer size (default is 512Kb) (call it before pb_init)
int     pb_init(void);      //returns 0 if everything went well (starts Dma engine)
void    pb_kill(void);      //stops Dma engine and releases push buffer

void    pb_target_extra_buffer(int n);  //to have rendering made into a static extra buffer
void    pb_target_back_buffer(void);    //to have rendering made into normal rotating back buffer

//Render to texture: pb_set_render_target points the GPU at an application buffer until the next
//pb_target_back_buffer / pb_target_extra_buffer / pb_set_render_target (NULL: back buffer) or
//pb_finished. Buffers: contiguous memory below 0x03FFB000 (MmAllocateContiguousMemoryEx), 64
//bytes aligned. Returns 0, or -1 (nothing changed) for an invalid target.
//- swizzled: width and height powers of two (<= 4096), Morton order (pb_swizzle_offset), the
//  pitches are not used; sample it as a SZ_* texture of the same size.
//- linear: pitch (and depth_pitch) >= width*Bpp, multiples of 64; sample it as a LU_IMAGE_*
//  texture (texel coordinates). A tiled buffer needs a tile pitch (see pb_assign_tile).
//- depth_addr NULL: no depth buffer, depth and stencil tests go off (back on, with depth writes,
//  when a target with a depth buffer follows). Otherwise it has pbkit's depth format
//  (pb_get_depth_format, Z24S8 with PB_DEPTH_NONE) and the color buffer's layout.
//Coordinates and pb_set_viewport are in target pixels; the window clip is widened to 4096 for
//a target larger than the screen. No pb_fill / pb_erase_depth_stencil_buffer (NV097_CLEAR_SURFACE)
//on a swizzled target (xemu asserts): clear it with a quad. Push NV097_WAIT_FOR_IDLE before
//sampling it, wait for the GPU (fence, pb_busy) before the CPU reads it.
typedef struct {
    DWORD        *addr;          //color buffer
    DWORD        width;
    DWORD        height;
    unsigned int color_format;   //NV097_SET_SURFACE_FORMAT_COLOR_LE_*
    bool         swizzled;
    DWORD        pitch;          //linear only: bytes per line
    DWORD        *depth_addr;    //NULL: no depth buffer
    DWORD        depth_pitch;    //linear only
} pb_render_target;
int     pb_set_render_target(const pb_render_target *rt);
//Byte offset of texel (x, y) in a swizzled w x h surface or texture (powers of two), for CPU access
DWORD   pb_swizzle_offset(DWORD x, DWORD y, DWORD w, DWORD h, DWORD bytes_per_pixel);

//Multisampling (call before pb_init). PB_MS_2X: 2 samples per pixel side by side
//(NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2), PB_MS_4X: 2x2 (SQUARE_OFFSET_4).
//pb_init allocates a sample buffer (width*2 x height, 4X: width*2 x height*2, tile #3) and the
//depth stencil buffer at that size; pb_target_back_buffer draws into the sample buffer and
//pb_finished resolves it on the GPU into the back buffer (box filter: the bilinear average of
//each pixel's samples) before queuing the flip. All coordinates (vertices, viewport, clears,
//pb_fill) stay in screen pixels: the GPU scales them. The frame buffers keep the screen size.
//The resolve changes GPU state: transform program start (slot 134, set back to 0), execution
//mode (program), slots 134-135 of the transform program, texture stages (0 disabled again,
//1-3 disabled), shader stage program, combiners, blending, alpha test, logic op, color mask,
//polygon modes, vertex attributes 0 and 9 (inline values); depth/stencil tests and depth writes
//and culling are back on. Set your own state again after pb_finished.
//pb_target_extra_buffer draws at screen size without depth buffer when multisampling.
#define PB_MS_NONE  0
#define PB_MS_2X    1
#define PB_MS_4X    2
void    pb_set_multisample(int mode);
int     pb_get_multisample(void);
DWORD   *pb_multisample_buffer(void);   //after pb_init: the sample buffer (NULL without multisampling)
DWORD   pb_multisample_pitch(void);

DWORD   *pb_extra_buffer(int n);    //returns a static extra buffer address
DWORD   *pb_back_buffer(void);      //returns normal rotating back buffer address
DWORD   pb_back_buffer_width(void);
DWORD   pb_back_buffer_height(void);
DWORD   pb_back_buffer_pitch(void);
DWORD   *pb_depth_stencil_buffer(void); //returns the (shared) depth stencil buffer address (CPU side)
DWORD   pb_depth_stencil_pitch(void);   //bytes between two lines of the depth stencil buffer
//Tile pitch rule: a tiled buffer's surface pitch is its tile pitch. The depth stencil buffer
//uses the next tile pitch (640 wide Z16: 1536). The frame buffers keep width*Bpp (scan-out
//pitch) and are tiled only when that is a tile pitch (640 wide 32 bits: yes, 16 bits: no).
bool    pb_frame_buffers_tiled(void);   //after pb_init: frame buffers are in tile #0
bool    pb_depth_stencil_tiled(void);   //after pb_init: depth stencil buffer is in tile #1 (compressed)

//GPU tile regions (NV_PFB_TILE 0..7): pb_init uses #0 (frame buffers), #1 (depth stencil)
//and #2 (extra buffers). An application render target can be assigned to a free one;
//tile_addr & tile_size must be 16Kb aligned and tile_pitch one of the 16 listed pitches
//(0x200..0x7000). tile_flags (same values as the XDK D3DTILE_FLAGS_*):
#define PB_TILE_FLAGS_ZBUFFER   0x00000001  //tile holds a depth (stencil) buffer
#define PB_TILE_FLAGS_Z32BITS   0x04000000  //32 bits depth (Z24S8), omit for Z16
#define PB_TILE_FLAGS_ZCOMPRESS 0x80000000  //automatic depth compression (NV_PFB_ZCOMP tag)
void    pb_assign_tile(int tile_index,DWORD tile_addr,DWORD tile_size,DWORD tile_pitch,
                       DWORD tile_z_start_tag,DWORD tile_z_offset,DWORD tile_flags);
void    pb_release_tile(int tile_index,int clear_offset); //clear_offset=1 also resets NV_PFB_ZCOMP_OFFSET

void    pb_set_viewport(int dwx,int dwy,int width,int height,float zmin,float zmax);

//Video overlay (NV_PVIDEO buffer 0): a YUY2 or UYVY surface scaled into a screen rectangle by
//the scan-out, over the frame buffer (where it has the color key, if enabled). Not part of the
//frame buffer: a CPU or GPU readback of the screen does not see it. pb_overlay_update checks
//the description and queues it: pbkit's VBlank interrupt programs it at the next VBlank, the
//last update queued before a VBlank wins. The surface must stay untouched while it is shown:
//double buffer it, write the next one, update, and reuse the previous surface once
//pb_overlay_update_pending() is 0. Needs pb_init (before it / after pb_kill the registers are
//written at once). pb_kill and pb_overlay_disable (at the next VBlank) stop the overlay.
//Surface: contiguous memory below 64 Mb, 64 bytes aligned, pitch a multiple of 64 (<= 8128),
//2 bytes per pixel. Source rectangle: src_x + src_width <= 2046 (even), src_y + src_height <=
//2047; destination: dst_x, dst_y, dst_width, dst_height < 4096, scaling (ds/dx = src/dst,
//20 bits fraction) between 8x down (dst >= src / 8) and any upscale.
#define PB_OVERLAY_UYVY 0   //bytes U Y0 V Y1 (NV_PVIDEO_FORMAT_COLOR_LE_YB8CR8YA8CB8; xemu: not supported, asserts)
#define PB_OVERLAY_YUY2 1   //bytes Y0 U Y1 V (NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8)
typedef struct {
    const void  *surface;
    DWORD       pitch;
    int         format;             //PB_OVERLAY_*
    DWORD       src_x, src_y, src_width, src_height;
    DWORD       dst_x, dst_y, dst_width, dst_height;
    bool        color_key_enable;   //show the overlay only where the frame buffer pixel is color_key
    DWORD       color_key;          //0xRRGGBB
    bool        bt709;              //ITU-R BT.709 matrix instead of BT.601
} pb_overlay;
int     pb_overlay_update(const pb_overlay *ov);   //0, or -1 (nothing queued) for an invalid description
void    pb_overlay_disable(void);
int     pb_overlay_update_pending(void);    //1 while a queued update (or disable) waits for its VBlank
DWORD   pb_overlay_last_vbl(void);          //VBlank count at which the last update / disable was programmed

int pb_busy(void);

#ifdef __cplusplus
}
#endif

#endif
