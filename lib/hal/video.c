// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2005 Robin Mulloy
// SPDX-FileCopyrightText: 2006 Tom Burns
// SPDX-FileCopyrightText: 2006 Guillaume Lamonoca
// SPDX-FileCopyrightText: 2006 Richard Osborne
// SPDX-FileCopyrightText: 2017-2020 Jannik Vogel
// SPDX-FileCopyrightText: 2019 Lucas Jansson
// SPDX-FileCopyrightText: 2021 Kosmas Raptis
// SPDX-FileCopyrightText: 2021 GXTX
// SPDX-FileCopyrightText: 2021 Stefan Schmidt
// SPDX-FileCopyrightText: 2022 Ryan Wendland

#include <xboxkrnl/xboxkrnl.h>
#include <hal/xbox.h>
#include <hal/video.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>


// Registers
#define PCIO_CRTC_STATUS			0x006013DA

#define PCRTC_START				0x00600800
#define PCRTC_INTR				0x00600100
#define PCRTC_INTR_VBLANK_RESET			0x00000001

#define PCRTC_INTR_EN				0x00600140
#define PCRTC_INTR_EN_VBLANK_DISABLED		0x00000000
#define PCRTC_INTR_EN_VBLANK_ENABLED		0x00000001

#define NV_PMC_INTR_EN_0 0x00000140
#define NV_PMC_INTR_EN_0_INTA_DISABLED 0x00000000
#define NV_PMC_INTR_EN_0_INTA_HARDWARE 0x00000001
#define NV_PMC_INTR_EN_0_INTA_SOFTWARE 0x00000002

#define NV_PRMCIO_CRX__COLOR			0x006013D4
#define NV_PRMCIO_CR__COLOR			0x006013D5
#define NV_CIO_CR_OFFSET_INDEX			0x13 // pitch / 8, bits 7:0
#define NV_CIO_CRE_RPC0_INDEX			0x19 // bits 7:5 = pitch / 8, bits 10:8
#define NV_CIO_CRE_LSR_INDEX			0x25 // bit 5 = pitch / 8, bit 11

// AV mode word, output field (bits 31:30)
#define AV_MODE_OUT_MASK			0xC0000000
#define AV_MODE_OUT_HDTV			0x80000000
#define AV_MODE_OUT_VGA				0xC0000000
#define AV_MODE_TV_MASK				0x0000007F

#define XVIDEO_FLAGS_ALL (XVIDEO_FLAG_WIDESCREEN | XVIDEO_FLAG_INTERLACED | XVIDEO_FLAG_PROGRESSIVE | \
                          XVIDEO_FLAG_FIELD | XVIDEO_FLAG_10X11PIXELASPECTRATIO)

#define NV_USER_DAC_WRITE_MODE_ADDRESS 0x006813C8
#define NV_USER_DAC_PALETTE_DATA 0x006813C9

// Defines for video regions
#define VIDEO_REGION_NTSCM			0x00000100
#define VIDEO_REGION_NTSCJ			0x00000200
#define VIDEO_REGION_PAL			0x00000300

// Defines for pixel formats
#define VIDEO_A1R5G5B5				0x00000010
#define VIDEO_R5G6B5				0x00000011
#define VIDEO_A8R8G8B8				0x00000012

static unsigned char*	framebufferMemory = NULL;
static unsigned char*	_fb = NULL;
static DWORD			dwEncoderSettings 	= 0;
static VIDEO_MODE		vmCurrent = { 0, 0, 0, 0 };
static int			flickerLevel		= 5;
static BOOL			flickerSet		= FALSE;
static BOOL			softenFilter		= FALSE;
static BOOL			softenSet		= FALSE;
static GAMMA_RAMP_ENTRY	gammaRampEntries[256];
static DWORD			dwCurrentAvMode		= 0;
static DWORD			dwCurrentFlags		= 0;

static KINTERRUPT InterruptObject;
static KDPC DPCObject;
static HANDLE VBlankEvent;
static BOOL IsrRegistered = FALSE;

#define VBL_IRQ 3

typedef struct _VIDEO_MODE_SETTING
{
	DWORD dwMode;
	int width;
	int height;
	int refresh;
	DWORD dwStandard;
	DWORD dwFlags;
} VIDEO_MODE_SETTING;

static VIDEO_MODE_SETTING vidModes[] =
{
 {0x44030307,640,480,50,VIDEO_REGION_PAL,AV_PACK_STANDARD}, //640x480 PAL 50Hz
 {0x44040408,720,480,50,VIDEO_REGION_PAL,AV_PACK_STANDARD}, //720x480 PAL 50Hz
 {0x0401010B,640,480,60,VIDEO_REGION_PAL,AV_PACK_STANDARD}, //640x480 PAL 60Hz
 {0x0402020C,720,480,60,VIDEO_REGION_PAL,AV_PACK_STANDARD}, //720x480 PAL 60Hz
 {0x04010101,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_STANDARD}, //640x480 NTSCM 60Hz
 {0x04020202,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_STANDARD}, //720x480 NTSCM 60Hz
 {0x04010103,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_STANDARD}, //640x480 NTSCJ 60Hz
 {0x04020204,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_STANDARD}, //720x480 NTSCJ 60Hz

 {0x44030307,640,480,50,VIDEO_REGION_PAL,AV_PACK_RFU}, //640x480 PAL 50Hz
 {0x44040408,720,480,50,VIDEO_REGION_PAL,AV_PACK_RFU}, //720x480 PAL 50Hz
 {0x0401010B,640,480,60,VIDEO_REGION_PAL,AV_PACK_RFU}, //640x480 PAL 60Hz
 {0x0402020C,720,480,60,VIDEO_REGION_PAL,AV_PACK_RFU}, //720x480 PAL 60Hz
 {0x04010101,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_RFU}, //640x480 NTSCM 60Hz
 {0x04020202,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_RFU}, //720x480 NTSCM 60Hz
 {0x04010103,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_RFU}, //640x480 NTSCJ 60Hz
 {0x04020204,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_RFU}, //720x480 NTSCJ 60Hz

 {0x60030307,640,480,50,VIDEO_REGION_PAL,AV_PACK_SCART}, //640x480 PAL 50Hz RGB
 {0x60040408,720,480,50,VIDEO_REGION_PAL,AV_PACK_SCART}, //720x480 PAL 50Hz RGB
 {0x2001010B,640,480,60,VIDEO_REGION_PAL,AV_PACK_SCART}, //640x480 PAL 60Hz RGB
 {0x2002020C,720,480,60,VIDEO_REGION_PAL,AV_PACK_SCART}, //720x480 PAL 60Hz RGB
 {0x20010101,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_SCART}, //640x480 NTSCM 60Hz RGB
 {0x20020202,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_SCART}, //720x480 NTSCM 60Hz RGB
 {0x20010103,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_SCART}, //640x480 NTSCJ 60Hz RGB
 {0x20020204,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_SCART}, //720x480 NTSCJ 60Hz RGB

 {0x48030314,640,480,50,VIDEO_REGION_PAL,AV_PACK_HDTV}, //640x480i PAL 50Hz
 {0x48040415,720,480,50,VIDEO_REGION_PAL,AV_PACK_HDTV}, //720x480i PAL 50Hz
 {0x08010119,640,480,60,VIDEO_REGION_PAL,AV_PACK_HDTV}, //640x480i PAL 60Hz
 {0x0802021a,720,480,60,VIDEO_REGION_PAL,AV_PACK_HDTV}, //720x480i PAL 60Hz
 {0x0801010d,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //640x480i NTSCM 60Hz
 {0x0802020e,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //720x480i NTSCM 60Hz
 {0x88070701,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //640x480p NTSCM 60Hz
 {0x88080801,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //720x480p NTSCM 60Hz
 {0x880B0A02,1280,720,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //1280x720p NTSCM 60Hz
 {0x880E0C03,1920,1080,60,VIDEO_REGION_NTSCM,AV_PACK_HDTV}, //1920x1080i NTSCM 60Hz
 {0x0801010d,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //640x480i NTSCJ 60Hz
 {0x0802020e,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //720x480i NTSCJ 60Hz
 {0x88070701,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //640x480p NTSCJ 60Hz
 {0x88080801,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //720x480p NTSCJ 60Hz
 {0x880B0A02,1280,720,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //1280x720p NTSCJ 60Hz
 {0x880E0C03,1920,1080,60,VIDEO_REGION_NTSCJ,AV_PACK_HDTV}, //1920x1080i NTSCJ 60Hz

 {0xC0060601,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_VGA}, //640x480 SVGA 60Hz
 {0xC0030303,800,600,60,VIDEO_REGION_NTSCM,AV_PACK_VGA}, //800x600 SVGA 60Hz
 {0xC0040404,1024,768,60,VIDEO_REGION_NTSCM,AV_PACK_VGA}, //1024x768 SVGA 60Hz

 {0x44030307,640,480,50,VIDEO_REGION_PAL,AV_PACK_SVIDEO}, //640x480 PAL 50Hz
 {0x44040408,720,480,50,VIDEO_REGION_PAL,AV_PACK_SVIDEO}, //720x480 PAL 50Hz
 {0x0401010B,640,480,60,VIDEO_REGION_PAL,AV_PACK_SVIDEO}, //640x480 PAL 60Hz
 {0x0402020C,720,480,60,VIDEO_REGION_PAL,AV_PACK_SVIDEO}, //720x480 PAL 60Hz
 {0x04010101,640,480,60,VIDEO_REGION_NTSCM,AV_PACK_SVIDEO}, //640x480 NTSCM 60Hz
 {0x04020202,720,480,60,VIDEO_REGION_NTSCM,AV_PACK_SVIDEO}, //720x480 NTSCM 60Hz
 {0x04010103,640,480,60,VIDEO_REGION_NTSCJ,AV_PACK_SVIDEO}, //640x480 NTSCJ 60Hz
 {0x04020204,720,480,60,VIDEO_REGION_NTSCJ,AV_PACK_SVIDEO}, //720x480 NTSCJ 60Hz
};

static int iVidModes = sizeof(vidModes) / sizeof(VIDEO_MODE_SETTING);

/* AV mode output bits 31:30: 0 / 1 SDTV (interlaced), 2 HDTV (interlaced only
   for TV table index 3, 1080i), 3 VGA (progressive) */
static BOOL AvModeIsInterlaced(DWORD dwMode)
{
	DWORD out = dwMode & AV_MODE_OUT_MASK;

	if (out == AV_MODE_OUT_VGA)
		return FALSE;
	if (out == AV_MODE_OUT_HDTV)
		return (dwMode & AV_MODE_TV_MASK) == 3;
	return TRUE;
}

static void __stdcall DPC(PKDPC Dpc,
PVOID DeferredContext,
PVOID SystemArgument1,
PVOID SystemArgument2)
{
	/* Wake up waiting threads */
	NtPulseEvent(VBlankEvent, NULL);
	return;
}

static BOOLEAN __stdcall ISR(PKINTERRUPT Interrupt, PVOID ServiceContext)
{
	if (VIDEOREG(NV_PMC_INTR_EN_0))
	{
		/* Reset interrupt */
		VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
		/* Call our Dpc */
		KeInsertQueueDpc(&DPCObject,NULL,NULL);
		return TRUE;
	}
	return FALSE;
}

static int InstallVBLInterrupt(void)
{
	int r;

	/* Disable all interrupts */
	VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_DISABLED;
	VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_DISABLED;
	VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
	
	/* Setup our ISR */
	KIRQL irql;
	ULONG vector;
	unsigned char value[4];

	vector = HalGetInterruptVector(VBL_IRQ, &irql);

	NtCreateEvent(&VBlankEvent, NULL, NotificationEvent, FALSE);
	
	KeInitializeDpc(&DPCObject,&DPC,NULL);

	KeInitializeInterrupt(&InterruptObject,
	&ISR,
	NULL,
	vector,
	irql,
	LevelSensitive,
	TRUE);

	r=KeConnectInterrupt(&InterruptObject);
	
	/* Enable interrupts, but leave vblank interrupts disabled */
	VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_HARDWARE;

	return r;
}

static void UninstallVBLInterrupt(void)
{
	VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_DISABLED;
	VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_DISABLED;
	VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
	KeDisconnectInterrupt(&InterruptObject);
	NtClose(VBlankEvent);
}

DWORD XVideoGetEncoderSettings(void)
{
	if(dwEncoderSettings == 0)
	{
		AvSendTVEncoderOption((PVOID)VIDEO_BASE, VIDEO_ENC_GET_SETTINGS, 0, (ULONG*)&dwEncoderSettings);
	}

	return dwEncoderSettings;
}

BOOLEAN XVideoListModes(VIDEO_MODE *vm, int bpp, int refresh, void **p)
{
	int i;
	int position = (int) *p;
	if (position >= iVidModes) {
		return FALSE;
	}
	DWORD dwEnc = XVideoGetEncoderSettings();
	
	DWORD dwAdapter = dwEnc & VIDEO_ADAPTER_MASK;
	DWORD dwStandard = dwEnc & VIDEO_STANDARD_MASK;

	bool is_pal = (dwStandard == VIDEO_REGION_PAL);

	bool allow_480p = dwEnc & VIDEO_MODE_480P;
	bool allow_720p = dwEnc & VIDEO_MODE_720P;
	bool allow_1080i = dwEnc & VIDEO_MODE_1080I;

	VIDEO_MODE_SETTING *pVidMode;
	if (bpp == 0)
	{
		bpp = 32;
	}
	if (refresh == 0)
	{
		if(is_pal)
		{
			refresh = (dwEnc & VIDEO_60Hz) ? 60 : 50;
		} else {
			refresh = 60;
		}
	}
	for(; position < iVidModes; position++)
	{
		pVidMode = &vidModes[position];

		if((pVidMode->dwFlags & VIDEO_ADAPTER_MASK) != dwAdapter)
			continue;

		if(pVidMode->dwStandard != dwStandard)
			continue;

		if(pVidMode->refresh != refresh)
			continue;

		if(!is_pal && (dwAdapter == AV_PACK_HDTV))
		{
			bool is_hd = pVidMode->dwMode & 0x80000000;

			/* If 480p support is enabled, reject 480i modes
			   (forces a 480p mode to be picked) */
			if(allow_480p && (pVidMode->height == 480 && !is_hd))
				continue;

			/* If 480p support is disabled, reject those modes */
			if(!allow_480p && (pVidMode->height == 480 && is_hd))
				continue;

			/* If 720p support is disabled, reject those modes */
			if(!allow_720p && (pVidMode->height == 720 && is_hd))
				continue;

			/* If 1080i support is disabled, reject those modes */
			if(!allow_1080i && (pVidMode->height == 1080 && is_hd))
				continue;
		}
		break;
	}
	
	*p = (void *)(position + 1);
	
	if(position >= iVidModes) // No compatible mode found
	{
		return FALSE;
	}
	else
	{
		vm->width = pVidMode->width;
		vm->height = pVidMode->height;
		vm->bpp = bpp;
		vm->refresh = refresh;
		return TRUE;
	}
}

unsigned char* XVideoGetFB(void)
{
	return _fb;
}

void XVideoSetFB(unsigned char *fb)
{
	assert(((unsigned int)fb & ~0x7FFFFFFF) == 0);
	_fb = fb;
	VIDEOREG(PCRTC_START) = (unsigned int)_fb & 0x7FFFFFFF;
}

VIDEO_MODE XVideoGetMode(void)
{
	return vmCurrent;
}

void XVideoFlushFB(void)
{
	asm __volatile__("sfence");
}

void XVideoInit(DWORD dwMode, int width, int height, int bpp)
{
	int i;
	GAMMA_RAMP_ENTRY defaultGammaRampEntries[256];
	ULONG Step = 0;
	DWORD dwFormat = 0;
	int bytesPerPixel = (bpp+7)/8;
	int pitch = width * bytesPerPixel;
	int screenSize = pitch * height;

	switch(bpp) {
	case 15:
		dwFormat = VIDEO_A1R5G5B5;
		break;
	case 16:
		dwFormat = VIDEO_R5G6B5;
		break;
	case 32:
		dwFormat = VIDEO_A8R8G8B8;
		break;
	default:
		assert(false);
		break;
	}

	XVideoSetVideoEnable(FALSE);

	PVOID previousFB = AvGetSavedDataAddress();
	if (previousFB != NULL) {
		SIZE_T previousFBSize = MmQueryAllocationSize(previousFB);
		MmPersistContiguousMemory(previousFB, previousFBSize, FALSE);
		MmFreeContiguousMemory(previousFB);
		AvSetSavedDataAddress(NULL);
	}

	if (framebufferMemory != NULL) {
		MmFreeContiguousMemory(framebufferMemory);
	}
	framebufferMemory = MmAllocateContiguousMemoryEx(screenSize,
	                                                 0x00000000, 0x7FFFFFFF,
	                                                 0x1000,
	                                                 PAGE_READWRITE |
	                                                 PAGE_WRITECOMBINE);
	assert(framebufferMemory != NULL);
	memset(framebufferMemory, 0x00, screenSize);
	asm __volatile__("sfence");

	do
	{
		Step = AvSetDisplayMode((PVOID)VIDEO_BASE, Step, 
			dwMode, dwFormat, pitch, (unsigned int)framebufferMemory & 0x7FFFFFFF);
	} while(Step);

	/* Store the framebuffer that we set; normally this is done in XVideoSetFB.
	   However, we're using the kernel's AvSetDisplayMode here instead, so we
	   set it here, too. */
	_fb = framebufferMemory;

	XVideoSetFlickerFilter(5);
	XVideoSetSoftenFilter(FALSE);

	for(i = 0; i < 256; i++) {
		defaultGammaRampEntries[i].red = i;
		defaultGammaRampEntries[i].green = i;
		defaultGammaRampEntries[i].blue = i;
	}
	XVideoSetGammaRamp(0, defaultGammaRampEntries, 256);

	XVideoSetVideoEnable(TRUE);
}


BOOL XVideoSetMode(int width, int height, int bpp, int refresh)
{
	VIDEO_MODE vm;
	void *p = NULL;

	while(XVideoListModes(&vm, bpp, refresh, &p))
	{
		if(vm.width != width || vm.height != height)
			continue;
		
		DWORD dwMode = vidModes[(int)p-1].dwMode;

		XVideoInit(dwMode, vm.width, vm.height, vm.bpp);

		vmCurrent.width = vm.width;
		vmCurrent.height = vm.height;
		vmCurrent.bpp = vm.bpp;
		vmCurrent.refresh = vm.refresh;
		dwCurrentAvMode = dwMode;
		dwCurrentFlags = AvModeIsInterlaced(dwMode) ? XVIDEO_FLAG_INTERLACED : XVIDEO_FLAG_PROGRESSIVE;
		return TRUE;
	}

	memset(&vmCurrent, 0x00, sizeof(VIDEO_MODE));
	dwCurrentAvMode = 0;
	dwCurrentFlags = 0;
	return FALSE;
}

/* 640x480 modes and their 10:11 pixel aspect ratio (FPAR) variant with the same
   bits 31:24: register / CRTC / TV table indices (bits 23:0) of the kernel's
   AV_MODE_640x480_FPAR_* modes (Cxbx-Reloaded, EmuKrnlAvModes.h) */
static const DWORD fparModes[][2] =
{
	{0x010101, 0x0F0D0F}, // NTSC-M
	{0x010103, 0x0F0D10}, // NTSC-J
	{0x01010B, 0x0F0D11}, // PAL-60
	{0x01010D, 0x0F0D12}, // NTSC YPrPb (480i)
	{0x030307, 0x100E13}, // PAL-I 50 Hz
	{0x030314, 0x100E18}, // PAL-I YPrPb
	{0x010119, 0x0F0D1B}, // PAL-60 YPrPb
	{0x070701, 0x110F01}, // 480p (HDTV output only, VGA is excluded)
};

BOOL XVideoSetModeEx(int width, int height, int bpp, int refresh, DWORD flags)
{
	DWORD scan = flags & (XVIDEO_FLAG_INTERLACED | XVIDEO_FLAG_PROGRESSIVE);
	DWORD dwMode, out;
	int i, index = -1;

	if (flags == 0)
		return XVideoSetMode(width, height, bpp, refresh);

	if ((flags & ~XVIDEO_FLAGS_ALL) || (flags & XVIDEO_FLAG_FIELD) ||
	    (scan == (XVIDEO_FLAG_INTERLACED | XVIDEO_FLAG_PROGRESSIVE)))
		return FALSE;

	if (bpp == 0)
		bpp = 32;

	if (scan == 0)
	{
		/* the mode XVideoSetMode would pick */
		VIDEO_MODE vm;
		void *p = NULL;
		while (XVideoListModes(&vm, bpp, refresh, &p))
		{
			if (vm.width == width && vm.height == height)
			{
				index = (int)p - 1;
				refresh = vm.refresh;
				break;
			}
		}
	}
	else
	{
		DWORD dwEnc = XVideoGetEncoderSettings();
		DWORD dwAdapter = dwEnc & VIDEO_ADAPTER_MASK;
		DWORD dwStandard = dwEnc & VIDEO_STANDARD_MASK;

		if (refresh == 0)
		{
			if (dwStandard == VIDEO_REGION_PAL)
				refresh = (dwEnc & VIDEO_60Hz) ? 60 : 50;
			else
				refresh = 60;
		}
		for (i = 0; i < iVidModes; i++)
		{
			VIDEO_MODE_SETTING *m = &vidModes[i];
			BOOL interlaced = AvModeIsInterlaced(m->dwMode);

			if ((m->dwFlags & VIDEO_ADAPTER_MASK) != dwAdapter || m->dwStandard != dwStandard ||
			    m->refresh != refresh || m->width != width || m->height != height)
				continue;
			if ((scan == XVIDEO_FLAG_INTERLACED) != interlaced)
				continue;
			/* the HDTV modes need their dashboard setting */
			if ((m->dwMode & AV_MODE_OUT_MASK) == AV_MODE_OUT_HDTV)
			{
				if (m->height == 480 && !(dwEnc & VIDEO_MODE_480P))
					continue;
				if (m->height == 720 && !(dwEnc & VIDEO_MODE_720P))
					continue;
				if (m->height == 1080 && !(dwEnc & VIDEO_MODE_1080I))
					continue;
			}
			index = i;
			break;
		}
	}
	if (index < 0)
		return FALSE;

	dwMode = vidModes[index].dwMode;
	out = dwMode & AV_MODE_OUT_MASK;

	if (flags & XVIDEO_FLAG_10X11PIXELASPECTRATIO)
	{
		DWORD fpar = 0;
		if (width != 640 || height != 480 || out == AV_MODE_OUT_VGA)
			return FALSE;
		for (i = 0; i < (int)(sizeof(fparModes) / sizeof(fparModes[0])); i++)
		{
			if ((dwMode & 0x00FFFFFF) == fparModes[i][0])
			{
				fpar = fparModes[i][1];
				break;
			}
		}
		if (fpar == 0)
			return FALSE;
		dwMode = (dwMode & 0xFF000000) | fpar;
	}

	/* SDTV (and 480i YPrPb) modes have a 16:9 variant with the WSS bit, the HDTV
	   and VGA modes do not (AV_MODE_*_16x9, Cxbx-Reloaded EmuKrnlAvModes.h) */
	if ((flags & XVIDEO_FLAG_WIDESCREEN) && out != AV_MODE_OUT_HDTV && out != AV_MODE_OUT_VGA)
		dwMode |= XVIDEO_AV_MODE_WSS;

	XVideoInit(dwMode, width, height, bpp);

	vmCurrent.width = width;
	vmCurrent.height = height;
	vmCurrent.bpp = bpp;
	vmCurrent.refresh = refresh;
	dwCurrentAvMode = dwMode;
	dwCurrentFlags = (flags & (XVIDEO_FLAG_WIDESCREEN | XVIDEO_FLAG_10X11PIXELASPECTRATIO)) |
	                 (AvModeIsInterlaced(dwMode) ? XVIDEO_FLAG_INTERLACED : XVIDEO_FLAG_PROGRESSIVE);
	return TRUE;
}

DWORD XVideoGetModeFlags(void)
{
	return dwCurrentFlags;
}

DWORD XVideoGetAvMode(void)
{
	return dwCurrentAvMode;
}

DWORD XVideoGetVideoFlags(void)
{
	ULONG type = 0, value = 0, length = 0;

	if (!NT_SUCCESS(ExQueryNonVolatileSetting(XC_VIDEO, &type, &value, sizeof(value), &length)) ||
	    length != sizeof(value))
		return 0;
	return value;
}


void XVideoSetVideoEnable(BOOL enable)
{
	AvSendTVEncoderOption((PVOID)VIDEO_BASE, VIDEO_ENC_VIDEOENABLE, enable ? FALSE : TRUE, NULL);
}


void XVideoSetFlickerFilter(int level)
{
	if(!flickerSet || (level & 0x07) != flickerLevel)
	{
		AvSendTVEncoderOption((PVOID)VIDEO_BASE, VIDEO_ENC_FLICKERFILTER, (level & 0x07), NULL);

		flickerSet = TRUE;
		flickerLevel = level & 0x07;
	}
}

void XVideoSetSoftenFilter(BOOL enable)
{
	if(!softenSet || softenFilter != enable)
	{
		AvSendTVEncoderOption((PVOID)VIDEO_BASE, VIDEO_ENC_SOFTEN_FILTER, enable, NULL);

		softenSet = TRUE;
		softenFilter = enable;
	}
}

void XVideoSetGammaRamp(unsigned int offset, const GAMMA_RAMP_ENTRY *entries, unsigned int count)
{
	int i;

	assert((offset + count) <= 256);
	memcpy(&gammaRampEntries[offset], entries, count * sizeof(GAMMA_RAMP_ENTRY));

	/* Partial setting of `count` entries at `offset` would be nice, but
	   NV2A doesn't seem to support this: Changing the write address will
	   sometimes mess up the existing table.
	   To avoid this, we update it all from a cached state. */
	VIDEOREG8(NV_USER_DAC_WRITE_MODE_ADDRESS) = 0;
	for(i = 0; i < 256; i++) {
		VIDEOREG8(NV_USER_DAC_PALETTE_DATA) = gammaRampEntries[i].red;
		VIDEOREG8(NV_USER_DAC_PALETTE_DATA) = gammaRampEntries[i].green;
		VIDEOREG8(NV_USER_DAC_PALETTE_DATA) = gammaRampEntries[i].blue;
	}
}


void XVideoWaitForVBlank()
{
	if (! IsrRegistered) {
		if (InstallVBLInterrupt())
			IsrRegistered = TRUE;
		else
			return; //Prevents deadlock in case user code hooks IRQ3 first
	}

	/* Enable vblank interrupt */
	VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
	VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_ENABLED;
	
	/* Wait for vblank */
	NtWaitForSingleObject(VBlankEvent, FALSE, NULL);
	
	/* Disable vblank interrupt */
	VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_DISABLED;
	VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
}

unsigned char* XVideoGetVideoBase()
{
	return (unsigned char *)VIDEO_BASE;
}

int XVideoVideoMemorySize()
{
	// TODO Is this always the case?
	// 4MB
	return 1024 * 1024 * 4;
}
