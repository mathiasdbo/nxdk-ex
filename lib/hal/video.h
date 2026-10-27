// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2005 Robin Mulloy
// SPDX-FileCopyrightText: 2006 Tom Burns
// SPDX-FileCopyrightText: 2006 Richard Osborne
// SPDX-FileCopyrightText: 2017-2021 Stefan Schmidt
// SPDX-FileCopyrightText: 2019 Lucas Jansson
// SPDX-FileCopyrightText: 2020 Jannik Vogel
// SPDX-FileCopyrightText: 2022 Ryan Wendland

#ifndef HAL_VIDEO_H
#define HAL_VIDEO_H

#include <xboxkrnl/xboxkrnl.h>

#if defined(__cplusplus)
extern "C"
{
#endif

// Defines for frame buffer
#define VIDEO_BASE				0xFD000000

// Hardware access macros
#define VIDEOREG(x) (*(volatile unsigned int*)(VIDEO_BASE + (x)))
#define VIDEOREG16(x) (*(volatile unsigned short*)(VIDEO_BASE + (x)))
#define VIDEOREG8(x) (*(volatile unsigned char*)(VIDEO_BASE + (x)))

// Defines for video encoder options
#define VIDEO_ENC_GET_SETTINGS      6
#define VIDEO_ENC_VIDEOENABLE       9
#define VIDEO_ENC_FLICKERFILTER     11
#define VIDEO_ENC_SOFTEN_FILTER     14

// Flags for video settings
#define VIDEO_MODE_720P   0x20000
#define VIDEO_MODE_1080I  0x40000
#define VIDEO_MODE_480P   0x80000
#define VIDEO_60Hz   0x400000
#define VIDEO_50Hz   0x800000
#define VIDEO_WIDESCREEN  0x010000
#define VIDEO_LETTERBOX   0x100000

#define VIDEO_ADAPTER_MASK  0x000000FF
#define VIDEO_STANDARD_MASK 0x0000FF00

// Flags for XVideoSetModeEx (same values as the XDK's D3DPRESENTFLAG_*)
#define XVIDEO_FLAG_WIDESCREEN             0x00000010 // 16:9: SDTV modes get the encoder's WSS bit
#define XVIDEO_FLAG_INTERLACED             0x00000020 // 480i (HDTV pack), 1080i
#define XVIDEO_FLAG_PROGRESSIVE            0x00000040 // 480p / 720p (HDTV pack, enabled in the dashboard), VGA
#define XVIDEO_FLAG_FIELD                  0x00000080 // field rendering: not supported, XVideoSetModeEx fails
#define XVIDEO_FLAG_10X11PIXELASPECTRATIO  0x00000100 // 640x480 only: the kernel's FPAR modes

// AV mode word bits (the kernel's AvSetDisplayMode mode, AV_OPTION_QUERY_MODE)
#define XVIDEO_AV_MODE_OUT_MASK            0xC0000000 // 0, 1 SDTV; 2 HDTV; 3 VGA
#define XVIDEO_AV_MODE_SCART               0x20000000 // RGB
#define XVIDEO_AV_MODE_WSS                 0x10000000 // 16:9 wide screen signalling

//Defines for refresh rates
#define REFRESH_50HZ                50
#define REFRESH_60HZ                60
#define REFRESH_DEFAULT             0

// Structure to hold details about a video mode;
typedef struct _VIDEO_MODE
{
	int		width;
	int		height;
	int		bpp;
	int		refresh;
} VIDEO_MODE;

typedef struct _GAMMA_RAMP_ENTRY
{
	unsigned char red;
	unsigned char green;
	unsigned char blue;
} GAMMA_RAMP_ENTRY;


DWORD XVideoGetEncoderSettings(void);
unsigned char* XVideoGetFB(void);
void XVideoSetFB(unsigned char *fb);
VIDEO_MODE XVideoGetMode(void);
void XVideoFlushFB(void);

void XVideoSetFlickerFilter(int level);
BOOL XVideoSetMode(int width, int height, int bpp, int refresh);

/*
Like XVideoSetMode, with XVIDEO_FLAG_* flags. 0 = XVideoSetMode.
INTERLACED / PROGRESSIVE select the scan type where the AV pack offers both
(HDTV pack, 480 lines); a scan type the pack or the dashboard settings do not
offer fails (SDTV packs are interlaced only, VGA progressive only, 720p is
progressive, 1080i interlaced, 480p / 720p / 1080i need the dashboard flag).
WIDESCREEN sets the encoder's wide screen signalling bit in SDTV modes (the
game renders anamorphic 16:9); HDTV and VGA modes are unchanged by it.
10X11PIXELASPECTRATIO selects the kernel's 640x480 FPAR mode (other sizes fail).
FIELD fails: field rendering is not supported.
Returns FALSE when no mode matches (XVideoSetMode's rules for flags 0; with
flags, the current mode is kept).
*/
BOOL XVideoSetModeEx(int width, int height, int bpp, int refresh, DWORD flags);

// XVIDEO_FLAG_* of the current mode as set: the requested flags plus the scan
// type of the mode (INTERLACED or PROGRESSIVE); 0 before the first mode set
DWORD XVideoGetModeFlags(void);

// The kernel AV mode word of the current mode (0 before the first mode set)
DWORD XVideoGetAvMode(void);

// The dashboard video settings (EEPROM XC_VIDEO): VIDEO_WIDESCREEN,
// VIDEO_LETTERBOX, VIDEO_MODE_480P / 720P / 1080I, VIDEO_60Hz (PAL 60)
DWORD XVideoGetVideoFlags(void);

/*
Keeps the picture on screen across XLaunchXBE: copies the frame buffer the
CRTC is showing into persisted contiguous memory, points the CRTC at the copy
and hands it to the kernel (AvSetSavedDataAddress). The next title's first
XVideoSetMode / XVideoSetModeEx frees it; before that, AvGetSavedDataAddress()
returns it (the kernel may move it at the reboot: use that address, and its
second dword is overwritten by the kernel). With pbkit, call it once the last
frame is on screen (no frame queued), then pb_kill() and XLaunchXBE.
Returns FALSE when no mode is set or the allocation fails.
*/
BOOL XVideoPersistDisplay(void);

void XVideoSetSoftenFilter(BOOL enable);
void XVideoSetVideoEnable(BOOL enable);
void XVideoSetGammaRamp(unsigned int offset, const GAMMA_RAMP_ENTRY *entries, unsigned int count);

/*
Lists the video modes available at the given colour depth and refresh rate.
On the first call the argument p must be a pointer to a void pointer
initialized to NULL. This is used internally to maintain context between
successive calls. If a matching video mode is found then XVideoListModes will
return TRUE and the video mode details will be written to the VIDEO_MODE vm
structure. If no matching video mode is found the function will return FALSE.
Repeatedly calling the function allows all matching video modes to be read out.
If a value of 0 is provided for the refresh rate the current refresh rate is used.
If a value of 0 is provided for the bpp a default value of 32bpp is used.
*/
BOOLEAN XVideoListModes(VIDEO_MODE *vm, int bpp, int refresh, void **p);

void XVideoWaitForVBlank(void);
unsigned char* XVideoGetVideoBase(void);
int XVideoVideoMemorySize(void);

#ifdef __cplusplus
}
#endif

#endif
