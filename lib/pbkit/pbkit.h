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

int pb_busy(void);

#ifdef __cplusplus
}
#endif

#endif
