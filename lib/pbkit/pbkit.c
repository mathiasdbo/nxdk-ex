// clang-format off

//pbKit core functions

// SPDX-License-Identifier: MIT

// SPDX-FileCopyrightText: 2007 Guillaume Lamonoca
// SPDX-FileCopyrightText: 2017 espes
// SPDX-FileCopyrightText: 2017-2020 Jannik Vogel
// SPDX-FileCopyrightText: 2018-2022 Stefan Schmidt
// SPDX-FileCopyrightText: 2019 Lucas Jansson
// SPDX-FileCopyrightText: 2021 Erik Abair

//#define DBG
//#define LOG

#include <hal/video.h>
#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>
#include <hal/debug.h>
#include <stdbool.h>
#include <assert.h>
#include <winapi/synchapi.h>

#include "pbkit.h"
#include "outer.h"
#include "nv_objects.h"  //shared with renouveau files
#include "nv20_shader.h" //(search "nouveau" on wiki)


#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/*
 * pb_release_debug_screen(): the scan-out buffer XVideoSetMode allocated
 * (XVideoGetFB(), pbkit's debug screen) given back. From then on pbkit never
 * shows it (pb_show_debug_screen, pb_kill's restore of the old scan-out) and
 * its reports go to the kernel debugger (DbgPrint) instead of being drawn
 * into it: every debugPrint below this point is pb_debug_print.
 */
static  int         pb_DebugScreenGone;

static void pb_debug_print(const char *format, ...)
{
    char buf[512];
    va_list args;

    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (pb_DebugScreenGone) DbgPrint("%s", buf);
    else debugPrint("%s", buf);
}
#define debugPrint pb_debug_print

//a macro used to build up a valid method
#define EncodeMethod(subchannel,command,nparam) ((nparam<<18)+(subchannel<<13)+command)



#define INSTANCE_MEM_MAXSIZE                0x5000  //20Kb

#define ADDR_SYSMEM                 1
#define ADDR_FBMEM                  2
#define ADDR_AGPMEM                 3

#define GPU_IRQ                     3

#define XTAL_16MHZ                  16.6667f
#define DW_XTAL_16MHZ                   16666666

#define MAX_EXTRA_BUFFERS               8

#define NONE                        -1

#define TICKSTIMEOUT                    100 //if Dma doesn't react in that time, send a warning

#define PB_SETOUTER                 0xB2A
#define PB_SETNOISE                 0xBAA
#define PB_FINISHED                 0xFAB

unsigned int pb_ColorFmt = NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8;
static unsigned int pb_DepthFmt = NV097_SET_SURFACE_FORMAT_ZETA_Z24S8;
static  int         pb_ColorBpp=32;     //bits per pixel of pb_ColorFmt (computed by pb_init)
static  int         pb_DepthBpp=32;     //bits per pixel of pb_DepthFmt (computed by pb_init)
static  int         pb_DepthFloat=0;    //NV097_SET_CONTROL0_Z_FORMAT: 0=fixed point depth, 1=floating point depth
static  int         pb_WBuffer=1;       //NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE: 1=depth is w (W-buffering), 0=depth is z
                                        //(1 is what pbkit has always pushed in set_draw_buffer, see pb_control0)
static  int         pb_Dither=-1;       //NV097_SET_DITHER_ENABLE: -1=auto (on for 16 bits color formats), 0=off, 1=on

static  int         pb_running=0;

static  DWORD           pb_vbl_counter=0;

//Batch 3 (sync): VBlank/swap callbacks, presentation interval, fences
static  void            (*volatile pb_VBlankCallback)(DWORD vbl_count)=NULL;

//overlay registers queued by pb_overlay_update / pb_overlay_disable, programmed at the VBlank
#define PB_OVL_REGS 11
static  const DWORD     pb_OverlayRegAddr[PB_OVL_REGS]={
    NV_PVIDEO_BASE_0,NV_PVIDEO_LIMIT_0,NV_PVIDEO_OFFSET_0,NV_PVIDEO_SIZE_IN_0,NV_PVIDEO_POINT_IN_0,
    NV_PVIDEO_DS_DX_0,NV_PVIDEO_DT_DY_0,NV_PVIDEO_POINT_OUT_0,NV_PVIDEO_SIZE_OUT_0,NV_PVIDEO_FORMAT_0,
    NV_PVIDEO_COLOR_KEY };
static  volatile DWORD  pb_OverlayReg[PB_OVL_REGS];
static  volatile int    pb_OverlayPending=0;    //0 none, 1 update, 2 disable
static  volatile DWORD  pb_OverlayLastVBL=0;
static  void            (*volatile pb_SwapCallback)(DWORD frame, DWORD vbl_count)=NULL;
static  volatile int    pb_PresentInterval=PB_PRESENT_INTERVAL_ONE;
static  volatile DWORD  pb_LastFlipVBL;     //pb_vbl_counter when a frame was last put on screen
static  volatile int    pb_VBLFlippedSlot=-1;   //intervals other than ONE: slot the ISR flipped to at this VBlank
static  DWORD           pb_FenceLast;       //last value pb_insert_fence handed out
static  volatile DWORD  *pb_FenceMem=NULL;  //GPU written fence value (semaphore DMA context, pb_DmaBuffer8)
#define PB_FENCE_START  0xFFFFFF00u         //first fences wrap past 0 early: wrap bugs show up at once

#ifdef DBG
static int          pb_trace_mode=1;
#else
static int          pb_trace_mode=0;
#endif
                //if set, we wait after each block sending (pb_end)
                //so we are sure GPU received all the data (slower)
                //and that any GPU error comes from last block sent.

static  int         pb_disable_gpu=0;
                //if set, prevents GPU from delaying CPU when FIFO is
                //full (allows to see how fast CPU code is fast alone)

static  KINTERRUPT      pb_InterruptObject;
static  KDPC            pb_DPCObject;

static  HANDLE          pb_VBlankEvent;

static  DWORD           pb_OldMCEnable;
static  DWORD           pb_OldMCInterrupt;
static  DWORD           pb_OldFBConfig0;
static  DWORD           pb_OldFBConfig1;
static  DWORD           pb_OldVideoStart;

static  DWORD           *pb_DmaBuffer8; //points at 32 contiguous bytes (Dma Channel ID 8 buffer)
static  DWORD           *pb_DmaBuffer2; //points at 32 contiguous bytes (Dma Channel ID 2 buffer)
static  DWORD           *pb_DmaBuffer7; //points at 32 contiguous bytes (Dma Channel ID 7 buffer)

static  DWORD           pb_Size=PBKIT_PUSHBUFFER_SIZE;//push buffer size, must be >64Kb and a power of 2
static  uint32_t        *pb_Head;   //points at push buffer head
static  uint32_t        *pb_Tail;   //points at push buffer tail
static  uint32_t        *pb_Put=NULL;   //where next command+params are to be written
static  uint32_t        *pb_Kicked=NULL;    //pb_Put as last handed to the Dma engine (pb_end_deferred)

static  float           pb_CpuFrequency;

static  DWORD           pb_GpuInstMem;

static  DWORD           pb_PushBase;
static  DWORD           pb_PushLimit;

static  DWORD           pb_FifoHTAddr;
static  DWORD           pb_FifoFCAddr;
static  DWORD           pb_FifoU1Addr;

static  DWORD           pb_3DGrCtxInst[2]={0,0};//Adress of the two 3D graphic contexts (addr=inst<<4+NV_PRAMIN)
static  DWORD           pb_GrCtxTableInst;  //Adress of the table that points at the two graphic contexts
static  DWORD           pb_GrCtxInst[2];    //Adress of the two graphic contexts (addr=inst<<4+NV_PRAMIN)
static  int         pb_GrCtxID;     //Current context ID : 0,1 or NONE

static  DWORD           pb_FifoBigInst;     //graphic contexts are stored there, and much more (addr=inst<<4+NV_PRAMIN)

static  DWORD           pb_FreeInst;        //next free space in PRAMIN area (addr=inst<<4+NV_PRAMIN)

static  int         pb_BackBufferNxt=0;
static  int         pb_BackBufferNxtVBL=0;
static  int         pb_BackBufferbReady[5]={0,0,0,0,0}; //5: pb_init clears five
static  int         pb_BackBuffersWanted=2;     //pb_back_buffers(): 2 triple buffering, 1 double
static  volatile DWORD  pb_FramesFinished;      //pb_finished() calls since pb_init
static  volatile DWORD  pb_FramesShown;         //frames the VBlank handler has put on screen
static  int         pb_BackBufferIndex[3];

static  DWORD           pb_FifoChannelsReady=0;
static  DWORD           pb_FifoChannelsMode=NV_PFIFO_MODE_ALL_PIO;
static  DWORD           pb_FifoChannelID=0;

static  DWORD           pb_PutRunSize=0;
static  DWORD           pb_GetRunSize;

static  DWORD           pb_FrameBuffersCount;
static  DWORD           pb_FrameBuffersWidth;
static  DWORD           pb_FrameBuffersHeight;
static  DWORD           pb_FrameBuffersAddr;
static  DWORD           pb_FrameBuffersPitch;
static  DWORD           pb_FBAddr[3];       //frame buffers addresses
static  DWORD           pb_FBSize;      //size of 1 buffer
static  DWORD           pb_FBGlobalSize;    //size of all buffers
static  DWORD           pb_FBVFlag;
static  DWORD           pb_GPUFrameBuffersFormat;//encoded format for GPU
static  DWORD           pb_EXAddr[8];       //extra buffers addresses
static  DWORD           pb_ExtraBuffersCount=0;

static  DWORD           pb_DepthStencilAddr;
static  DWORD           pb_DepthStencilPitch;
static int          pb_DepthStencilLast;
static  int         pb_FBTiled;     //frame buffers in tile #0 (surface pitch is a tile pitch)
static  DWORD           pb_AppFBAddr[3];    //pb_set_frame_buffers (application owned, 0: pbkit allocates)
static  int         pb_AppFBCount=0;
static  DWORD           pb_AppFBPitch;
static  DWORD           pb_AppDSAddr=0;     //pb_set_depth_stencil_buffer (application owned)
static  DWORD           pb_AppDSPitch;
static  int         pb_DSTiled;     //depth stencil buffer in tile #1
static  DWORD           pb_DSAddr;      //depth stencil address
static  DWORD           pb_DSSize;      //size of depth stencil buffer
static  DWORD           pb_GPUDepthStencilFormat;//encoded format for GPU

static  int         pb_front_index;
static  int         pb_back_index;

static  DWORD           pb_Viewport_x;
static  DWORD           pb_Viewport_y;
static  DWORD           pb_Viewport_width;
static  DWORD           pb_Viewport_height;
static  DWORD           pb_Viewport_zmin;
static  DWORD           pb_Viewport_zmax;
static  float           pb_Viewport_zminf;  //exact copies of zmin/zmax (needed to re-push the
static  float           pb_Viewport_zmaxf;  //viewport when the depth scale changes)

static  float           pb_XScale;
static  float           pb_YScale;
static  float           pb_ZScale;
static  float           pb_GlobalScale;
static  float           pb_Bias;

static  int         pb_debug_screen_active;

static  DWORD           pb_DmaChID9Inst;
static  DWORD           pb_DmaChID10Inst;
static  DWORD           pb_DmaChID11Inst;

static volatile DWORD  *pb_DmaUserAddr;

static  DWORD           pb_PushIndex;
static  DWORD           *pb_PushStart;
static  DWORD           *pb_PushNext;

static int          pb_BeginEndPair=0;

static float            pb_FixedPipelineConstants[12]={
                    0.0f,   0.5f,   1.0f,   2.0f,
                    -1.0f,  0.0f,   1.0f,   2.0f,
                    0.0f,   0.0f,   -1.0f,  0.0f    };

static  float           pb_IdentityMatrix[16]={
                    1.0f,   0.0f,   0.0f,   0.0f,
                    0.0f,   1.0f,   0.0f,   0.0f,
                    0.0f,   0.0f,   1.0f,   0.0f,
                    0.0f,   0.0f,   0.0f,   1.0f    };

static  DWORD           pb_TilePitches[16]={
                    0x0200,0x0400,0x0600,0x0800,
                    0x0A00,0x0C00,0x0E00,0x1000,
                    0x1400,0x1800,0x1C00,0x2800,
                    0x3000,0x3800,0x5000,0x7000 };

static  float           pb_BiasTable[7]={
                    0.0f,
                    0.585f,
                    1.0f,
                    1.322f,
                    1.585f,
                    1.907f,
                    2.0f    };

static HAL_SHUTDOWN_REGISTRATION pb_shutdown_registration;

//1 if pitch is one of the 16 tile pitches
static int pb_is_tile_pitch(DWORD pitch)
{
    int i;
    for(i=0;i<16;i++) if (pb_TilePitches[i]==pitch) return 1;
    return 0;
}

//Checks an application supplied buffer (pb_set_frame_buffers, pb_set_depth_stencil_buffer):
//64 bytes aligned, physically contiguous, physical address = addr & 0x03FFFFFF (what pbkit gives
//the GPU, true for MmAllocateContiguousMemoryEx memory) and below 0x03FFB000.
static int pb_app_buffer_ok(DWORD addr, DWORD size)
{
    ULONG_PTR   phys;
    DWORD       offset;

    if ((addr==0)||(addr&0x3F))
    {
        debugPrint("pb_init: application buffer %08x: not 64 bytes aligned\n",(unsigned int)addr);
        return 0;
    }
    phys=MmGetPhysicalAddress((PVOID)addr);
    if ((phys!=(addr&0x03FFFFFF))||(phys+size>0x03FFB000))
    {
        debugPrint("pb_init: application buffer %08x: not contiguous memory below 0x03FFB000\n",(unsigned int)addr);
        return 0;
    }
    for(offset=0x1000-(addr&0xFFF);offset<size;offset+=0x1000)
        if (MmGetPhysicalAddress((PVOID)(addr+offset))!=phys+offset)
        {
            debugPrint("pb_init: application buffer %08x: not physically contiguous\n",(unsigned int)addr);
            return 0;
        }
    return 1;
}

//smallest tile pitch greater or equal to pitch, 0 if pitch is larger than all of them
static DWORD pb_tile_pitch_at_least(DWORD pitch)
{
    int i;
    for(i=0;i<16;i++) if (pb_TilePitches[i]>=pitch) return pb_TilePitches[i];
    return 0;
}

//forward references
static void pb_load_gr_ctx(int ctx_id);
static NTAPI VOID pb_shutdown_notification_routine (PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration);




//private functions

//The frame in slot pb_BackBufferNxtVBL is on screen (PCRTC_START was written): free its slot
static void pb_retire_shown(void)
{
    int next=pb_BackBufferNxtVBL;

    pb_BackBufferbReady[next]=0;

    VIDEOREG(NV_PGRAPH_INCREMENT)|=NV_PGRAPH_INCREMENT_READ_3D_TRIGGER;

    pb_FramesShown++;
    pb_LastFlipVBL=pb_vbl_counter;

    //rotate next back buffer
    pb_BackBufferNxtVBL=(next+1)%pb_FrameBuffersCount;
}

//PB_PRESENT_INTERVAL_IMMEDIATE / ONE_OR_IMMEDIATE: show the finished frames now (from the
//PB_FINISHED subprogram, i.e. the DPC), without waiting for the VBlank. Tearing is possible.
static void pb_flip_now(void)
{
    void    (*swap_cb)(DWORD,DWORD);
    int     next;

    while (pb_BackBufferbReady[next=pb_BackBufferNxtVBL]==1)
    {
        if (pb_debug_screen_active==0) VIDEOREG(PCRTC_START)=pb_FBAddr[pb_BackBufferIndex[next]]&0x03FFFFFF;
        pb_retire_shown();
        swap_cb=pb_SwapCallback;
        if (swap_cb) swap_cb(pb_FramesShown,pb_vbl_counter);
    }
}

//programs the queued overlay update (called at the VBlank, or directly without pbkit's interrupt)
static void pb_overlay_apply(DWORD vbl)
{
    int i;
    int what=pb_OverlayPending;

    if (what==0) return;
    if (what==1)
    {
        for(i=0;i<PB_OVL_REGS;i++) VIDEOREG(pb_OverlayRegAddr[i])=pb_OverlayReg[i];
        VIDEOREG(NV_PVIDEO_STOP)=0;
        VIDEOREG(NV_PVIDEO_BUFFER)=NV_PVIDEO_BUFFER_0_USE;
    }
    else VIDEOREG(NV_PVIDEO_STOP)=NV_PVIDEO_STOP_OVERLAY;
    pb_OverlayLastVBL=vbl;
    pb_OverlayPending=0;
}

static void pb_vbl_handler(void)
{
    BYTE        old_color_addr; //important index to preserve if we are called from Dpc or Isr

    int     next;
    int     shown;
    void    (*vbl_cb)(DWORD);
    void    (*swap_cb)(DWORD,DWORD);

    old_color_addr=VIDEOREG8(NV_PRMCIO_CRX__COLOR);

    pb_vbl_counter++;

    //Index of next back buffer to show up (0-4)
    next=pb_BackBufferNxtVBL;
    shown=0;

    //Is the next back buffer to show up is ready?
    //(interval ONE: the ISR showed it; other intervals: only if the ISR chose to)
    if (pb_BackBufferbReady[next]==1)
    if ((pb_PresentInterval==PB_PRESENT_INTERVAL_ONE)||(pb_VBLFlippedSlot==next))
    {
        //screen swapping has been done already, theoretically, in ISR
        pb_retire_shown();
        shown=1;
    }
    pb_VBLFlippedSlot=-1;

    do
    {
        VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
    }while(VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING);

    NtPulseEvent(pb_VBlankEvent, NULL);

    //user callbacks: must be brief, no pbkit calls, save/restore the FPU state if they use it
    vbl_cb=pb_VBlankCallback;
    if (vbl_cb) vbl_cb(pb_vbl_counter);
    swap_cb=pb_SwapCallback;
    if (shown && swap_cb) swap_cb(pb_FramesShown,pb_vbl_counter);

    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=old_color_addr; //restore color index
}







static void pb_cache_flush(void)
{
    __asm__ __volatile__ ("sfence");
    //assembler instruction "sfence" : waits end of previous instructions

    VIDEOREG(NV_PFB_WC_CACHE)|=NV_PFB_WC_CACHE_FLUSH_TRIGGER;
    while(VIDEOREG(NV_PFB_WC_CACHE)&NV_PFB_WC_CACHE_FLUSH_IN_PROGRESS) {};
}




static void pb_subprog(DWORD subprogID, DWORD paramA, DWORD paramB)
{
    //inner registers 0x1D8C & 0x1D90 match 2 outer registers :
    //[0x1D8C]=[NV20_TCL_PRIMITIVE_3D_PARAMETER_A]=VIDEOREG(NV_PGRAPH_PARAMETER_A)=[0xFD401A88]
    //[0x1D90]=[NV20_TCL_PRIMITIVE_3D_PARAMETER_B]=VIDEOREG(NV_PGRAPH_PARAMETER_B)=[0xFD40186C]
    //so they can be used by a push buffer sequence to set parameters
    //before triggering a subprogram by the command 0x0100 which will
    //throw an interrupt and have CPU execute its code right here.

    //Here just test the subprogID value and execute your own subprogram
    //associated code (avoid using subprogID=0, it seems to be reserved)

    int         next;

    switch(subprogID)
    {
        case PB_SETOUTER: //sets an outer register
            VIDEOREG(paramA)=paramB;
            break;

        case PB_SETNOISE: //Dxt1NoiseEnable: copy paramA in NV_PGRAPH_RDI(sel 0xE0 adr 0x50 & sel 0xDF adr 0x08)
            VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0xE0<<16)&NV_PGRAPH_RDI_INDEX_SELECT)|((0x50)&NV_PGRAPH_RDI_INDEX_ADDRESS);
            VIDEOREG(NV_PGRAPH_RDI_DATA)=paramA;
            VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0xDF<<16)&NV_PGRAPH_RDI_INDEX_SELECT)|((0x08)&NV_PGRAPH_RDI_INDEX_ADDRESS);
            VIDEOREG(NV_PGRAPH_RDI_DATA)=paramA;
            break;

        case PB_FINISHED: //warns that all drawing has been finished for the frame
            next=pb_BackBufferNxt;
            pb_BackBufferIndex[next]=paramA;
            pb_BackBufferbReady[next]=1;
            pb_BackBufferNxt=(next+1)%pb_FrameBuffersCount;
            if (pb_PresentInterval==PB_PRESENT_INTERVAL_IMMEDIATE) pb_flip_now();
            //ONE_OR_IMMEDIATE: the frame missed the VBlank when one passed with nothing new to show
            else if ((pb_PresentInterval==PB_PRESENT_INTERVAL_ONE_OR_IMMEDIATE)&&
                     (next==pb_BackBufferNxtVBL)&&(pb_vbl_counter!=pb_LastFlipVBL)) pb_flip_now();
            break;

        default:
            debugPrint( "Unknown subProgID %lu has been detected by DPC (A=%lx B=%lx).\n",
                    subprogID,
                    paramA,
                    paramB  );
            break;
    }
}



static DWORD pb_gr_handler(void)
{
    DWORD           status;

    DWORD           trapped_address;
    int         trapped_ctx_id;

    DWORD           nsource;

    DWORD           GrClass;

    DWORD           DataLow;

    int         i;

    DWORD           *p;

    VIDEOREG(NV_PGRAPH_FIFO)=NV_PGRAPH_FIFO_ACCESS_DISABLE;

    status=VIDEOREG(NV_PGRAPH_INTR);
    trapped_address=VIDEOREG(NV_PGRAPH_TRAPPED_ADDR);
    nsource=VIDEOREG(NV_PGRAPH_NSOURCE);

    trapped_ctx_id=(trapped_address&NV_PGRAPH_TRAPPED_ADDR_CHID)>>20;
    trapped_address&=NV_PGRAPH_TRAPPED_ADDR_MTHD;

    if (status&NV_PGRAPH_INTR_CONTEXT_SWITCH_PENDING)
    {
        VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_CONTEXT_SWITCH_RESET;

        while(VIDEOREG(NV_PGRAPH_STATUS));

        pb_load_gr_ctx(trapped_ctx_id);
    }

    if (status&NV_PGRAPH_INTR_MISSING_HW_PENDING)
    {
        VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_MISSING_HW_RESET;
    }

    if (    (status&NV_PGRAPH_INTR_NOTIFY_PENDING)||
        (status&NV_PGRAPH_INTR_ERROR_PENDING)   )
    {
        if (nsource&NV_PGRAPH_NSOURCE_ILLEGAL_MTHD_PENDING)
        {
            if (status&NV_PGRAPH_INTR_NOTIFY_PENDING)
                VIDEOREG(NV_PGRAPH_INTR)=   NV_PGRAPH_INTR_NOTIFY_RESET|
                                NV_PGRAPH_INTR_ERROR_RESET|
                                NV_PGRAPH_INTR_SINGLE_STEP_RESET|
                                NV_PGRAPH_INTR_MORE_RESET;
            else
                VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_ERROR_RESET;
        }
    }

    status=VIDEOREG(NV_PGRAPH_INTR);

    if (status)
    {
        VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_CONTEXT_SWITCH_RESET;

        if (    (status!=NV_PGRAPH_INTR_CONTEXT_SWITCH_PENDING)&&
            (status!=NV_PGRAPH_INTR_SINGLE_STEP_PENDING)    )
        {
            if (status&NV_PGRAPH_INTR_MISSING_HW_PENDING)
            {
                while(VIDEOREG(NV_PGRAPH_STATUS)) {};
            }

            if (nsource)
            {
                if (    (status&NV_PGRAPH_INTR_NOTIFY_PENDING)||
                    (status&NV_PGRAPH_INTR_ERROR_PENDING)   )
                {
                    GrClass=VIDEOREG(NV_PGRAPH_CTX_SWITCH1)&NV_PGRAPH_CTX_SWITCH1_GRCLASS;
                    DataLow=VIDEOREG(NV_PGRAPH_TRAPPED_DATA_LOW);   //&NV_PGRAPH_TRAPPED_DATA_LOW_VALUE

                    if ((nsource&NV_PGRAPH_NSOURCE_ILLEGAL_MTHD_PENDING)==0)
                    {
                        if (trapped_address==0x0100)
                        {
                            //The following line may be a bad idea. But without it, interrupt fires permanently...
                            VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_ERROR_RESET;
                            //calls subprogram
                            pb_subprog(DataLow,VIDEOREG(NV_PGRAPH_PARAMETER_A),VIDEOREG(NV_PGRAPH_PARAMETER_B));
                        }
                        else
                        {
                            pb_show_debug_screen();

                            debugPrint("\n");
                            if (nsource&NV_PGRAPH_NSOURCE_DATA_ERROR_PENDING) debugPrint("GPU Error : invalid data error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_PROTECTION_ERROR_PENDING) debugPrint("GPU Error : protection error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_RANGE_EXCEPTION_PENDING) debugPrint("GPU Error : range exception error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_LIMIT_COLOR_PENDING)
                            {
                                uint32_t limit_details = VIDEOREG(0x00400800);
                                debugPrint("GPU Error : color buffer limit error! 0x%X\n", limit_details);
                            }
                            if (nsource&NV_PGRAPH_NSOURCE_LIMIT_ZETA_PENDING)
                            {
                                uint32_t limit_details = VIDEOREG(0x00400800);
                                debugPrint("GPU Error : zeta buffer limit error! 0x%X\n", limit_details);
                            }
                            if (nsource&NV_PGRAPH_NSOURCE_DMA_R_PROTECTION_PENDING) debugPrint("GPU Error : dma read protection error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_DMA_W_PROTECTION_PENDING) debugPrint("GPU Error : dma write protection error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_FORMAT_EXCEPTION_PENDING) debugPrint("GPU Error : format exception error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_PATCH_EXCEPTION_PENDING) debugPrint("GPU Error : patch exception error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_STATE_INVALID_PENDING) debugPrint("GPU Error : object state invalid error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_DOUBLE_NOTIFY_PENDING) debugPrint("GPU Error : double notify error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_NOTIFY_IN_USE_PENDING) debugPrint("GPU Error : notify in use error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_METHOD_CNT_PENDING) debugPrint("GPU Error : method count error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_BFR_NOTIFICATION_PENDING) debugPrint("GPU Error : buffer notification error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_DMA_VTX_PROTECTION_PENDING) debugPrint("GPU Error : DMA vertex protection error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_IDX_INLINE_REUSE_PENDING) debugPrint("Graphics index inline reuse error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_INVALID_OPERATION_PENDING) debugPrint("GPU Error : invalid operation error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_FD_INVALID_OPERATION_PENDING) debugPrint("GPU Error : FD invalid operation error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_TEX_A_PROTECTION_PENDING) debugPrint("GPU Error : texture A protection error!\n");
                            if (nsource&NV_PGRAPH_NSOURCE_TEX_B_PROTECTION_PENDING) debugPrint("GPU Error : texture B protection error!\n");

                            debugPrint( "Error binary flags : %08lx\n"
                                    "Channel ID : %d (0=3D)\n"
                                    "Channel class : %lx\n"
                                    "Push buffer inner register target : %04lx\n"
                                    "Push buffer data (lo) or instance : %08lx\n"
                                    "Push buffer data (hi) or instance : %08x\n"
                                    "Multi-purpose register A [0x1D8C] : %08x\n"
                                    "Multi-purpose register B [0x1D90] : %08x\n\n",
                                    nsource,
                                    trapped_ctx_id,
                                    GrClass,
                                    trapped_address,
                                    DataLow,
                                    VIDEOREG(NV_PGRAPH_TRAPPED_DATA_HIGH),
                                    VIDEOREG(NV_PGRAPH_PARAMETER_A),
                                    VIDEOREG(NV_PGRAPH_PARAMETER_B)     );

                            if (pb_trace_mode==0) debugPrint("Report is accurate only if pb_trace_mode=1 (slower)\n");

                            debugPrint("System halted\n");

                            //calling XReboot() from here doesn't work well.

                            while(1) {
                              Sleep(2000);
                            };
                        }
                    }
                }
            }

            if (status&NV_PGRAPH_INTR_BUFFER_NOTIFY_PENDING)
            {
                while (VIDEOREG(NV_PGRAPH_STATUS)) {};
            }
        }
    }

    VIDEOREG(NV_PGRAPH_FIFO)=NV_PGRAPH_FIFO_ACCESS_ENABLE;

    return VIDEOREG(NV_PGRAPH_INTR);
}


void pb_wait_until_gr_not_busy(void)
{
    DWORD       status;

    while(VIDEOREG(NV_PGRAPH_STATUS)!=NV_PGRAPH_STATUS_NOT_BUSY)
    {
        status=VIDEOREG(NV_PMC_INTR_0);
        if (status&NV_PMC_INTR_0_PGRAPH_PENDING) pb_gr_handler();
        if (status&NV_PMC_INTR_0_PCRTC_PENDING) pb_vbl_handler();
    }
}




static void pb_load_gr_ctx(int ctx_id)
{
    DWORD           old_fifo_access;
    DWORD           dummy;
    int         i;

    if (VIDEOREG(NV_PGRAPH_INTR)!=NV_PGRAPH_INTR_NOT_PENDING) pb_gr_handler();

    old_fifo_access=VIDEOREG(NV_PGRAPH_FIFO);
    VIDEOREG(NV_PGRAPH_FIFO)=NV_PGRAPH_FIFO_ACCESS_DISABLE;

    pb_wait_until_gr_not_busy();

    if ((ctx_id!=pb_GrCtxID)&&(ctx_id!=NONE))
    {
        VIDEOREG(NV_PGRAPH_CHANNEL_CTX_POINTER)=pb_GrCtxInst[ctx_id]&NV_PGRAPH_CHANNEL_CTX_POINTER_INST;
        VIDEOREG(NV_PGRAPH_CHANNEL_CTX_STATUS)=NV_PGRAPH_CHANNEL_CTX_STATUS_UNLOADED;

        pb_wait_until_gr_not_busy();

        VIDEOREG(NV_PGRAPH_CTX_CONTROL)=NV_PGRAPH_CTX_CONTROL_DEVICE_ENABLED;
    }

    pb_GrCtxID=ctx_id;

    if (ctx_id==NONE)
    {
        VIDEOREG(NV_PGRAPH_CTX_CONTROL)=NV_PGRAPH_CTX_CONTROL_DEVICE_ENABLED|NV_PGRAPH_CTX_CONTROL_TIME_NOT_EXPIRED;
        VIDEOREG(NV_PGRAPH_FFINTFC_ST2)=NV_PGRAPH_FFINTFC_ST2_CHID_STATUS_VALID;

        VIDEOREG(NV_PGRAPH_FIFO)=old_fifo_access|NV_PGRAPH_FIFO_ACCESS_ENABLE;
    }
    else
    {
        if (pb_3DGrCtxInst[ctx_id])
        {
            VIDEOREG(NV_PGRAPH_DEBUG_0) =   NV_PGRAPH_DEBUG_0_IDX_STATE_RESET|
                            NV_PGRAPH_DEBUG_0_VTX_STATE_RESET|
                            NV_PGRAPH_DEBUG_0_CAS_STATE_RESET;
            dummy=VIDEOREG(NV_PGRAPH_DEBUG_0);
            VIDEOREG(NV_PGRAPH_DEBUG_0)=NV_PGRAPH_DEBUG_0_NO_RESET;
            dummy=VIDEOREG(NV_PGRAPH_DEBUG_0);

            VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0x00)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0x3D<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
            for(i=0;i<15;i++) VIDEOREG(NV_PGRAPH_RDI_DATA)=0;
        }

        VIDEOREG(NV_PGRAPH_DEBUG_1)|=NV_PGRAPH_DEBUG_1_CACHE_INVALIDATE;

        VIDEOREG(NV_PGRAPH_CTX_USER)=(ctx_id<<24)&NV_PGRAPH_CTX_USER_CHID;
        VIDEOREG(NV_PGRAPH_CHANNEL_CTX_POINTER)=pb_GrCtxInst[ctx_id]&NV_PGRAPH_CHANNEL_CTX_POINTER_INST;
        VIDEOREG(NV_PGRAPH_CHANNEL_CTX_STATUS)=NV_PGRAPH_CHANNEL_CTX_STATUS_LOADED;

        pb_wait_until_gr_not_busy();

        VIDEOREG(NV_PGRAPH_CTX_USER)=(VIDEOREG(NV_PGRAPH_CTX_USER)&~NV_PGRAPH_CTX_USER_CHID)|((ctx_id<<24)&NV_PGRAPH_CTX_USER_CHID);

        VIDEOREG(NV_PGRAPH_CTX_CONTROL) =   NV_PGRAPH_CTX_CONTROL_TIME_NOT_EXPIRED|
                            NV_PGRAPH_CTX_CONTROL_CHID_VALID|
                            NV_PGRAPH_CTX_CONTROL_DEVICE_ENABLED;

        VIDEOREG(NV_PGRAPH_FFINTFC_ST2)&=(NV_PGRAPH_FFINTFC_ST2_CHSWITCH_CLEAR&NV_PGRAPH_FFINTFC_ST2_FIFOHOLD_CLEAR);
    }
}



static DWORD pb_fifo_handler(void)
{
    DWORD       i;
    DWORD       status;
    DWORD       pull;
    DWORD       get_address;
    int     skip_waiting;

    skip_waiting=0;

    status=VIDEOREG(NV_PFIFO_INTR_0);

    if (status&NV_PFIFO_INTR_0_SEMAPHORE_PENDING)
    {
        VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_SEMAPHORE_RESET;
    }

    if (status&NV_PFIFO_INTR_0_ACQUIRE_TIMEOUT_PENDING)
    {
        VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_ACQUIRE_TIMEOUT_RESET;
    }

    status=VIDEOREG(NV_PFIFO_INTR_0);

    if (status&NV_PFIFO_INTR_0_CACHE_ERROR_PENDING)
    {
        pull=VIDEOREG(NV_PFIFO_CACHE1_PULL0);
        get_address=VIDEOREG(NV_PFIFO_CACHE1_GET);  //&NV_PFIFO_CACHE1_GET_ADDRESS (0x3FC)
        get_address>>=2;
        VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;
        VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;
        VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_CACHE_ERROR_RESET;

        for(i=0;i<65535;i++)
        {
            if ((pull&NV_PFIFO_CACHE1_PULL0_HASH_STATE_BUSY)==0) break;
            pull=VIDEOREG(NV_PFIFO_CACHE1_PULL0);
        }

        if (    (pull&NV_PFIFO_CACHE1_PULL0_DEVICE_SOFTWARE)||
            (pull&NV_PFIFO_CACHE1_PULL0_HASH_FAILED)    )
        {
            VIDEOREG(NV_PFIFO_CACHE1_GET)=((get_address+1)<<2)&NV_PFIFO_CACHE1_GET_ADDRESS;
        }

        VIDEOREG(NV_PFIFO_CACHE1_HASH)=0;   //&NV_PFIFO_CACHE1_HASH_INSTANCE
        VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_ENABLE;
        VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_REASSIGN_ENABLED;
    }

    if (status&NV_PFIFO_INTR_0_DMA_PUSHER_PENDING)
    {
        pb_show_debug_screen();
        debugPrint("Software Put=%08lx\n",(DWORD)pb_Put);
        debugPrint("Hardware Put=%08x\n",VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT));
        debugPrint("Hardware Get=%08x\n",VIDEOREG(NV_PFIFO_CACHE1_DMA_GET));
        debugPrint("Dma push buffer engine encountered invalid data at these addresses.\n");

        VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_DMA_PUSHER_RESET;
        VIDEOREG(NV_PFIFO_CACHE1_DMA_STATE)=NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT_0;

        if (VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT)!=VIDEOREG(NV_PFIFO_CACHE1_DMA_GET))
            VIDEOREG(NV_PFIFO_CACHE1_DMA_GET)+=(1<<2);
    }

    if (status&NV_PFIFO_INTR_0_DMA_PT_PENDING)
    {
        VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_DMA_PT_RESET;
    }

    if (VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)&NV_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY)
    {
        if ((VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY)==0)
        do
        {
            if (VIDEOREG(NV_PFIFO_INTR_0)==NV_PFIFO_INTR_0_NOT_PENDING)
            {
                if (VIDEOREG(NV_PGRAPH_INTR)) pb_fifo_handler();

                if (VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING) pb_vbl_handler();

                if ((VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY)==0)
                    continue; //jump to loop start
            }

            if ((VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY)==0)
            {
                skip_waiting=1;
                break;
            }

        }while(VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY);

        if (skip_waiting==0)
        {
            //wait
            while(VIDEOREG8(NV_PFIFO_CACHES)&NV_PFIFO_CACHES_DMA_SUSPEND_BUSY);
            VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)&=NV_PFIFO_CACHE1_DMA_PUSH_STATUS_RUNNING;
        }
    }

    if (VIDEOREG(NV_PFIFO_INTR_0)==NV_PFIFO_INTR_0_NOT_PENDING)
    {
        VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_ENABLE;
        VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_REASSIGN_ENABLED;
    }

    return VIDEOREG(NV_PFIFO_INTR_0)|(VIDEOREG(NV_PFIFO_DEBUG_0)&NV_PFIFO_DEBUG_0_CACHE_ERROR0_PENDING);
}


static void pb_set_fifo_channel(int channel)
{
    DWORD       old_caches,old_push,old_pull,old_channel;

    DWORD       *p;

    DWORD       pending_flags;

    old_caches=VIDEOREG(NV_PFIFO_CACHES);
    old_push=VIDEOREG(NV_PFIFO_CACHE1_PUSH0);
    old_pull=VIDEOREG(NV_PFIFO_CACHE1_PULL0);
    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;

    old_channel=VIDEOREG(NV_PFIFO_CACHE1_PUSH1)&NV_PFIFO_CACHE1_PUSH1_CHID;

    //backup old channel details into PRAMIN area
    p=(DWORD *)(VIDEO_BASE+pb_FifoFCAddr+old_channel*64);
    *(p+0)=VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT);   //&NV_PFIFO_CACHE1_DMA_PUT_OFFSET
    *(p+1)=VIDEOREG(NV_PFIFO_CACHE1_DMA_GET);   //&NV_PFIFO_CACHE1_DMA_GET_OFFSET
    *(p+2)=VIDEOREG(NV_PFIFO_CACHE1_REF);       //&NV_PFIFO_CACHE1_REF_CNT
    *(p+3)=VIDEOREG(NV_PFIFO_CACHE1_DMA_INSTANCE);  //&NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS
    *(p+4)=VIDEOREG(NV_PFIFO_CACHE1_DMA_STATE);
    *(p+5)=VIDEOREG(NV_PFIFO_CACHE1_DMA_FETCH);
    *(p+6)=VIDEOREG(NV_PFIFO_CACHE1_ENGINE);
    *(p+7)=VIDEOREG(NV_PFIFO_CACHE1_PULL1);
    *(p+8)=VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_2); //&NV_PFIFO_CACHE1_ACQUIRE_2_VALUE
    *(p+9)=VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_1); //&NV_PFIFO_CACHE1_ACQUIRE_1_TIMESTAMP
    *(p+10)=VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_0);    //&NV_PFIFO_CACHE1_ACQUIRE_0_TIMEOUT
    *(p+11)=VIDEOREG(NV_PFIFO_CACHE1_SEMAPHORE);
    *(p+12)=VIDEOREG(NV_PFIFO_CACHE1_DMA_SUBROUTINE);

    if (VIDEOREG(NV_PFIFO_CACHE1_PUSH1)&NV_PFIFO_CACHE1_PUSH1_MODE_DMA)
    {
        pending_flags=VIDEOREG(NV_PFIFO_DMA);
        pending_flags&=~(1<<old_channel);
        if (VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT)!=VIDEOREG(NV_PFIFO_CACHE1_DMA_GET))
            pending_flags|=(1<<old_channel);
        VIDEOREG(NV_PFIFO_DMA)=pending_flags;
    }

    //let's switch from old_channel to channel

    VIDEOREG(NV_PFIFO_CACHE1_PUSH1)=channel&NV_PFIFO_CACHE1_PUSH1_CHID;

    if (channel!=1)
    if (pb_FifoChannelsMode&(1<<channel)) //Channel mode was DMA?
        VIDEOREG(NV_PFIFO_CACHE1_PUSH1)|=NV_PFIFO_CACHE1_PUSH1_MODE_DMA;

    //restore channel details from VRAM
    p=(DWORD *)(VIDEO_BASE+pb_FifoFCAddr+channel*64);
    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT)=*(p+0);   //&NV_PFIFO_CACHE1_DMA_PUT_OFFSET
    VIDEOREG(NV_PFIFO_CACHE1_DMA_GET)=*(p+1);   //&NV_PFIFO_CACHE1_DMA_GET_OFFSET
    VIDEOREG(NV_PFIFO_CACHE1_REF)=*(p+2);       //&NV_PFIFO_CACHE1_REF_CNT
    VIDEOREG(NV_PFIFO_CACHE1_DMA_INSTANCE)=*(p+3);  //&NV_PFIFO_CACHE1_DMA_INSTANCE_ADDRESS
    VIDEOREG(NV_PFIFO_CACHE1_DMA_STATE)=*(p+4);
    VIDEOREG(NV_PFIFO_CACHE1_DMA_FETCH)=*(p+5);
    VIDEOREG(NV_PFIFO_CACHE1_ENGINE)=*(p+6);
    VIDEOREG(NV_PFIFO_CACHE1_PULL1)=*(p+7);
    VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_2)=*(p+8); //&NV_PFIFO_CACHE1_ACQUIRE_2_VALUE
    VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_1)=*(p+9); //&NV_PFIFO_CACHE1_ACQUIRE_1_TIMESTAMP
    VIDEOREG(NV_PFIFO_CACHE1_ACQUIRE_0)=*(p+10);    //&NV_PFIFO_CACHE1_ACQUIRE_0_TIMEOUT
    VIDEOREG(NV_PFIFO_CACHE1_SEMAPHORE)=*(p+11);
    VIDEOREG(NV_PFIFO_CACHE1_DMA_SUBROUTINE)=*(p+12);

    if (channel!=1)
    if (pb_FifoChannelsMode&(1<<channel)) //Channel mode was DMA?
        VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=NV_PFIFO_CACHE1_DMA_PUSH_ACCESS_ENABLE;

    VIDEOREG(NV_PFIFO_TIMESLICE)=NV_PFIFO_TIMESLICE_TIMER_EXPIRED;

    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=old_pull;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=old_push;
    VIDEOREG(NV_PFIFO_CACHES)=old_caches;
}





static void __stdcall DPC(PKDPC Dpc, PVOID DeferredContext, PVOID SystemArgument1, PVOID SystemArgument2)
{
    //Deferred Procedure Call (delayed treatment, triggered by ISR)
    //DPCs avoid crashes inside non reentrant user callbacks called by nested ISRs.
    //CAUTION : if you use fpu in DPC you have to save & restore yourself fpu state!!!
    //(fpu=floating point unit, i.e the coprocessor executing floating point opcodes)

    DWORD           more;
    DWORD           status;

    do
    {
        more=0;
        status=VIDEOREG(NV_PMC_INTR_0);

        if (status&NV_PMC_INTR_0_PTIMER_PENDING)
        {
            VIDEOREG(NV_PTIMER_INTR_0)=NV_PTIMER_INTR_0_ALARM_RESET;
            more=VIDEOREG(NV_PTIMER_INTR_0);
        }

        if (status&NV_PMC_INTR_0_PCRTC_PENDING) pb_vbl_handler();

        if (status&NV_PMC_INTR_0_PGRAPH_PENDING) more|=pb_gr_handler();

        if (    (VIDEOREG8(NV_PFIFO_DEBUG_0)&NV_PFIFO_DEBUG_0_CACHE_ERROR0_PENDING)||
            (status&NV_PMC_INTR_0_PFIFO_PENDING)    ) more|=pb_fifo_handler();

        if (    (VIDEOREG8(NV_PVIDEO_INTR)&NV_PVIDEO_INTR_BUFFER_0_PENDING)||
            (status&NV_PMC_INTR_0_PVIDEO_PENDING)   ) VIDEOREG(NV_PVIDEO_INTR)=NV_PVIDEO_INTR_BUFFER_0_RESET;
    }while(more);

    VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_HARDWARE;

    return;
}

static BOOLEAN __stdcall ISR(PKINTERRUPT Interrupt, PVOID ServiceContext)
{
    //Interruption Service Routine (triggered by interrupt signal IRQ3)
    int         next;

    if (pb_running==0) return FALSE;

    //really, not for us at all
    if (VIDEOREG(NV_PMC_INTR_0)==NV_PMC_INTR_0_NOT_PENDING) return FALSE;

    //is it the VBlank event? (if so, proceed screen swapping immediately & in DPC)
    if (VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING)
    {
        //queued overlay update (pb_vbl_counter+1 is the VBlank being handled)
        if (pb_OverlayPending) pb_overlay_apply(pb_vbl_counter+1);

        //Need to show next back buffer to show up? (do it now, it's urgent)
        if (pb_PresentInterval==PB_PRESENT_INTERVAL_ONE)
        {
            if (pb_debug_screen_active==0)
            if (pb_BackBufferbReady[pb_BackBufferNxtVBL]==1) VIDEOREG(PCRTC_START)=pb_FBAddr[pb_BackBufferIndex[pb_BackBufferNxtVBL]]&0x03FFFFFF;
        }
        else if (pb_BackBufferbReady[pb_BackBufferNxtVBL]==1)
        {
            //TWO: at most every second VBlank (pb_vbl_counter+1 is the VBlank being handled)
            if ((pb_PresentInterval!=PB_PRESENT_INTERVAL_TWO)||((DWORD)(pb_vbl_counter+1-pb_LastFlipVBL)>=2))
            {
                if (pb_debug_screen_active==0) VIDEOREG(PCRTC_START)=pb_FBAddr[pb_BackBufferIndex[pb_BackBufferNxtVBL]]&0x03FFFFFF;
                pb_VBLFlippedSlot=pb_BackBufferNxtVBL;
            }
        }
    }

    VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_DISABLED;

    //handle longer & non urgent stuff later with the Dpc
    KeInsertQueueDpc(&pb_DPCObject,NULL,NULL);

    return TRUE;
}



static int pb_install_gpu_interrupt(void)
{
    int r;
    KIRQL irql;
    ULONG vector;

    vector = HalGetInterruptVector(GPU_IRQ, &irql);

    KeInitializeDpc(&pb_DPCObject,&DPC,NULL);

    KeInitializeInterrupt(&pb_InterruptObject,
                &ISR,
                NULL,
                vector,
                irql,
                LevelSensitive,
                TRUE);

    r=KeConnectInterrupt(&pb_InterruptObject);

    return r;
}

static void pb_uninstall_gpu_interrupt(void)
{
    KeDisconnectInterrupt(&pb_InterruptObject);
}



DWORD pb_wait_until_tiles_not_busy(void)
{
    DWORD old_dma_push;

    while (((VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY)==0)||
        ((VIDEOREG8(NV_PFIFO_RUNOUT_STATUS)&NV_PFIFO_RUNOUT_STATUS_LOW_MARK_EMPTY)==0)||
        ((VIDEOREG8(NV_PFIFO_CACHE1_DMA_PUSH)&NV_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY)!=0)  )
    {
        pb_fifo_handler();
        if (VIDEOREG(NV_PGRAPH_INTR)!=NV_PGRAPH_INTR_NOT_PENDING) pb_gr_handler();
        if (VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING) pb_vbl_handler();
    }

    old_dma_push=VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH);
    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=NV_PFIFO_CACHE1_DMA_PUSH_ACCESS_DISABLE;
    while((VIDEOREG8(NV_PFIFO_CACHE1_DMA_PUSH)&NV_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY)!=0);

    return old_dma_push;
}


void pb_release_tile(int index,int clear_offset)
{
    volatile DWORD *pTile;
    volatile DWORD *pZcomp;
    volatile DWORD *p;

    DWORD       addr;
    DWORD       data;

    DWORD       old_dma_push;

    old_dma_push=pb_wait_until_tiles_not_busy();

    //points tile in NV_PFB
    pTile=(DWORD *)(VIDEO_BASE+NV_PFB_TILE+index*16);

    //points tile in NV_PGRAPH
    p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_TILE_XBOX+index*16);

    //points tile in NV_PGRAPH_RDI(0x10)
    addr=((index*4+0x10)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    data=0;

    do
    {
        pb_wait_until_gr_not_busy();

        *(pTile+0)=0;
        *(p+0)=0;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr; VIDEOREG(NV_PGRAPH_RDI_DATA)=data;
    }while(*(pTile+0)!=*(p+0));

    //points tile Zcomp in NV_PFB
    pZcomp=(DWORD *)(VIDEO_BASE+NV_PFB_ZCOMP+index*4);

    //points tile Zcomp in NV_PGRAPH
    p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_ZCOMP_XBOX+index*4);

    //points tile Zcomp in NV_PGRAPH_RDI(0x90)
    addr=((index*4+0x90)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    data=0;

    *(pZcomp+0)=0;
    *(p+0)=0;
    VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr; VIDEOREG(NV_PGRAPH_RDI_DATA)=data;

    if (clear_offset)
    {
        VIDEOREG(NV_PFB_ZCOMP_OFFSET)=0;
        VIDEOREG(NV_PGRAPH_ZCOMP_OFFSET_XBOX)=0;
    }

    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=old_dma_push;
}



void pb_assign_tile(    int tile_index,
                DWORD   tile_addr,
                DWORD   tile_size,
                DWORD   tile_pitch,
                DWORD   tile_z_start_tag,
                DWORD   tile_z_offset,
                DWORD   tile_flags  )
{
    DWORD               old_dma_push;

    DWORD               addr10;
    DWORD               addr30;
    DWORD               addr50;
    DWORD               addr90;

    DWORD               tile_tail;

    DWORD               *pTile;
    DWORD               *pZcomp;
    DWORD               *p;

    DWORD               EncodedZStartTag;
    DWORD               EncodedZOffset;

    assert((tile_index>=0)&&(tile_index<8)); //NV2A has 8 tile regions (NV_PFB_TILE(0..7))
#ifdef DBG
    if ((tile_addr&0x3fff)||(tile_size&0x3fff)) debugPrint("pb_assign_tile: addr & size not well aligned\n");
#endif
    old_dma_push=pb_wait_until_tiles_not_busy();

    //points at tile in NV_PGRAPH_RDI(0x10(Addr),0x30(Tail) & 0x50(Pitch))
    addr10=((tile_index*4+0x10)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    addr30=((tile_index*4+0x30)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    addr50=((tile_index*4+0x50)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);

    tile_tail=tile_addr+tile_size-1;

    //points tile in NV_PFB
    pTile=(DWORD *)(VIDEO_BASE+NV_PFB_TILE+tile_index*16);

    //points tile in NV_PGRAPH
    p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_TILE_XBOX+tile_index*16);


    do
    {
        pb_wait_until_gr_not_busy();

        *(pTile+0)=tile_addr|2|(tile_flags&1);
        *(p+0)=tile_addr|2|(tile_flags&1);
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr10; VIDEOREG(NV_PGRAPH_RDI_DATA)=tile_addr|2|(tile_flags&1);

        *(pTile+1)=tile_tail;
        *(p+1)=tile_tail;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr30; VIDEOREG(NV_PGRAPH_RDI_DATA)=tile_tail;

        *(pTile+2)=tile_pitch;
        *(p+2)=tile_pitch;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr50; VIDEOREG(NV_PGRAPH_RDI_DATA)=tile_pitch;
    }
    while   (   (*(pTile+0)!=*(p+0))||
            (((*(pTile+1))&0xFFFFC000)!=((*(p+1))&0xFFFFC000))||
            (*(pTile+2)!=*(p+2))    );

    if (tile_flags&0x80000000)  //Tag in use?
    {
        EncodedZStartTag=(tile_z_start_tag>>2)|0x80000000;

        if (tile_flags&0x04000000) EncodedZStartTag|=0x04000000;

        //points tile Zcomp in NV_PFB
        pZcomp=(DWORD *)(VIDEO_BASE+NV_PFB_ZCOMP+tile_index*4);

        //points tile Zcomp in NV_PGRAPH
        p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_ZCOMP_XBOX+tile_index*4);

        //points tile Zcomp in NV_PGRAPH_RDI(0x90)
        addr90=((tile_index*4+0x90)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);

        do
        {
            pb_wait_until_gr_not_busy();

            *(pZcomp+0)=EncodedZStartTag;
            *(p+0)=EncodedZStartTag;
            VIDEOREG(NV_PGRAPH_RDI_INDEX)=addr90; VIDEOREG(NV_PGRAPH_RDI_DATA)=EncodedZStartTag;
        }while (*(pZcomp+0)!=*(p+0));

        if (tile_z_offset)
        {
            EncodedZOffset=tile_z_offset|tile_index|0x80000000;

            do
            {
                pb_wait_until_gr_not_busy();

                VIDEOREG(NV_PFB_ZCOMP_OFFSET)=EncodedZOffset;
                VIDEOREG(NV_PGRAPH_ZCOMP_OFFSET_XBOX)=EncodedZOffset;
            }while(VIDEOREG(NV_PFB_ZCOMP_OFFSET)!=VIDEOREG(NV_PGRAPH_ZCOMP_OFFSET_XBOX));
        }
    }

    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=old_dma_push;
}



static void pb_prepare_tiles(void)
{
    DWORD           *pTile;
    DWORD           *pTlimit;
    DWORD           *pTsize;
    DWORD           *pZcomp;

    DWORD           Tile;
    DWORD           Tlimit;
    DWORD           Tsize;
    DWORD           Zcomp;
    DWORD           Zcomp_offset;
    DWORD           Config0;
    DWORD           Config1;

    DWORD           *p;

    int         i;

    p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_TILE_XBOX);
    pTlimit=(DWORD *)(VIDEO_BASE+NV_PFB_TLIMIT);
    pTsize=(DWORD *)(VIDEO_BASE+NV_PFB_TSIZE);
    pTile=(DWORD *)(VIDEO_BASE+NV_PFB_TILE);

    //Copy 8 Tiles details from NV_PFB to NV_PGRAPH and to NV_PGRAPH_RDI(0x10)
    for(i=0x10;i<0x30;i+=4)
    {
        Tile=*(pTile+0);
        *(p+0)=Tile;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=((i+0x00)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
        VIDEOREG(NV_PGRAPH_RDI_DATA)=Tile;

        Tlimit=*(pTlimit+0);
        *(p+1)=Tlimit;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=((i+0x20)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
        VIDEOREG(NV_PGRAPH_RDI_DATA)=Tlimit;

        Tsize=*(pTsize+0);
        *(p+2)=Tsize;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=((i+0x40)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
        VIDEOREG(NV_PGRAPH_RDI_DATA)=Tsize;

        p+=4; //move 16 bytes forward
        pTile+=4;
        pTlimit+=4;
        pTsize+=4;
    }

    p=(DWORD *)(VIDEO_BASE+NV_PGRAPH_ZCOMP_XBOX);
    pZcomp=(DWORD *)(VIDEO_BASE+NV_PFB_ZCOMP);

    //Copy 8 Tiles Zcomp from NV_PFB to NV_PGRAPH and to NV_PGRAPH_RDI(0x90)
    for(i=0x90;i<0x110;i+=4)
    {
        Zcomp=*(pZcomp+0);
        *(p+0)=Zcomp;
        VIDEOREG(NV_PGRAPH_RDI_INDEX)=((i+0x00)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
        VIDEOREG(NV_PGRAPH_RDI_DATA)=Tsize;

        p++; //move 4 bytes forward
        pZcomp++;
    }

    //Copy 3 parameters from NV_PFB to NV_PGRAPH and to NV_PGRAPH_RDI(sel 0xEA : 0xC, 0 & 4)

    Zcomp_offset=VIDEOREG(NV_PFB_ZCOMP_OFFSET);
    VIDEOREG(NV_PGRAPH_ZCOMP_OFFSET_XBOX)=Zcomp_offset;
    VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0x0C)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    VIDEOREG(NV_PGRAPH_RDI_DATA)=Zcomp_offset;

    Config0=VIDEOREG(NV_PFB_CFG0);
    VIDEOREG(NV_PGRAPH_CFG0_XBOX)=Config0;
    VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0x00)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    VIDEOREG(NV_PGRAPH_RDI_DATA)=Config0;

    Config1=VIDEOREG(NV_PFB_CFG1);
    VIDEOREG(NV_PGRAPH_CFG1_XBOX)=Config1;
    VIDEOREG(NV_PGRAPH_RDI_INDEX)=((0x04)&NV_PGRAPH_RDI_INDEX_ADDRESS)|((0xEA<<16)&NV_PGRAPH_RDI_INDEX_SELECT);
    VIDEOREG(NV_PGRAPH_RDI_DATA)=Config1;
}



void pb_create_dma_ctx(DWORD ChannelID,
                       DWORD Class,
                       DWORD Base,
                       DWORD Limit,
                       struct s_CtxDma *pDmaObject)
{
    DWORD           Addr;
    DWORD           AddrSpace;
    DWORD           Inst;
    DWORD           dma_flags;

    Addr=0;
    AddrSpace=0;

    if ((Base&0xF0000000)!=0x80000000)
    {
        Addr=Base;
        AddrSpace=ADDR_FBMEM;
    }
    else
    {
        Addr=Base&0x03FFFFFF;
        AddrSpace=ADDR_SYSMEM;
    }

    Inst=pb_FreeInst; pb_FreeInst+=1; //reserve 1 block (16 bytes)

    dma_flags=Class;
    dma_flags|=0x00003000;
    if (AddrSpace==ADDR_AGPMEM) dma_flags|=0x00030000;
    if (AddrSpace==ADDR_SYSMEM) dma_flags|=0x00020000;
    dma_flags|=0x00008000;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x08)=Addr|3;  //0x00000003|Addr
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x0C)=Addr|3;  //0x00000003|Addr
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x00)=dma_flags;   //0x???sB0cl ???=Addr&0xFFF
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x04)=Limit;   //0x03FFAFFF (MAXRAM)

    memset(pDmaObject,0,sizeof(struct s_CtxDma));

    pDmaObject->ChannelID=ChannelID;
    pDmaObject->Inst=Inst;
    pDmaObject->Class=Class;
    pDmaObject->isGr=0;
}




void pb_bind_channel(struct s_CtxDma *pCtxDmaObject)
{
    DWORD       entry;
    DWORD       *p;

    //entry in hash table
    entry=(((pCtxDmaObject->ChannelID>>11)^pCtxDmaObject->ChannelID)>>11)^pCtxDmaObject->ChannelID;

    //entry*8 max valid value is 0x1000

    //points at entry in hash table (table element size is 8 bytes = 2 dwords)
    p=(DWORD *)(VIDEO_BASE+pb_FifoHTAddr+entry*8);

    *(p+0)= pCtxDmaObject->ChannelID;
    *(p+1)= (0x80000000)|
        (pb_FifoChannelID<<24)|
        (pCtxDmaObject->isGr<<16)|
        (pCtxDmaObject->Inst&0xFFFF);
}



static void pb_3D_init(void)
{
    DWORD           Inst;

    int             channel;

    int         i;

    DWORD           offset;

    DWORD           offset_cmn;

    DWORD           offset_pipe;

    DWORD           offset_4dwords;

    DWORD           offset_20dwords;

    //Initialization of 3 big structures in PRAMIN area
    //At offset 0x0000 size=0x231C bytes=0x1A9C+0x0880
    //At offset 0x231C size=0x0C00 bytes
    //At offset 0x2F1C size=0x0784 bytes
    //Padding 4 dwords (at offset 0x36A0 size=0x0010 bytes?)

    channel=pb_FifoChannelID;

    Inst=pb_GrCtxInst[channel];

    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x000)|=1;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x33C)=0xFFFF0000;
    for(i=0x340;i<=0x39C;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x3A0)=0x0FFF0000;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x3A4)=0x0FFF0000;
    for(i=0x3A8;i<=0x478;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x47C)=0x00000101;
    for(i=0x480;i<=0x48C;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x490)=0x00000111;
    for(i=0x494;i<=0x4A4;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x4A8)=0x44400000;
    for(i=0x4AC;i<=0x4D0;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    for(i=0x4D4;i<=0x4E0;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x00030303;
    for(i=0x4E4;i<=0x4F0;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    for(i=0x4F4;i<=0x500;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x00080000;
    for(i=0x504;i<=0x508;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    for(i=0x50C;i<=0x518;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x01012000;
    for(i=0x51C;i<=0x528;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x000105B8;
    for(i=0x52C;i<=0x538;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x00080008;
    for(i=0x53C;i<=0x558;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    for(i=0x55C;i<=0x578;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x07FF0000; //8 dwords
    for(i=0x57C;i<=0x598;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0x07FF0000; //8 dwords
    for(i=0x59C;i<=0x5A0;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x5A4)=0x4B7FFFFF;
    for(i=0x5A8;i<=0x5F8;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x5FC)=0x00000001;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x600)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x604)=0x00004000;
    for(i=0x608;i<=0x60C;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x610)=0x00000001;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x614)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x618)=0x00040000;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+0x61C)=0x00010000;
    for(i=0x620;i<=0x628;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;
    for(i=0x62C;i<=0x6B4;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //35 dwords
    for(i=0x6B8;i<=0x728;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //26 dwords
    for(i=0x72C;i<=0x79C;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //26 dwords
    for(i=0x7A0;i<=0x810;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //26 dwords
    for(i=0x814;i<=0x818;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //2 dwords
    for(i=0x81C;i<=0xA18;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //128 dwords
    for(i=0xA1C;i<=0xC18;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //128 dwords
    for(i=0xC1C;i<=0xE18;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;   //128 dwords
    for(i=0xE1C;i<=0x1018;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0;  //128 dwords
    for(i=0x101C;i<=0x1318;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0; //192 dwords
    for(i=0x131C;i<=0x1A98;i+=4) VIDEOREG(NV_PRAMIN+(Inst<<4)+i)=0; //224 dwords

    offset=0x1A9C/4;    //number of dwords initialized so far = 0x6A7

    for(i=0;i<0x88;i++) //136 blocks (unit=16 bytes=4 dwords)
    {
        VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*16+0x00)=0x10700FF9;
        VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*16+0x04)=0x0436086C;
        VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*16+0x08)=0x000C001B;
        VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*16+0x0C)=0;
        offset+=4;
    }

    offset_cmn=offset;  //0x231C/4

    for(i=0;i<0x300;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//768 dwords
    offset+=0x300;  //0xC00 bytes

    offset_pipe=offset; //0x2F1C/4

    for(i=0;i<0x68;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//104 dwords
    offset+=0x68;
    for(i=0;i<0xD0;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//208 dwords
    offset+=0xD0;
    offset_4dwords=offset;

    for(i=0;i<0x04;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//004 dwords
    offset+=0x04;
    offset_20dwords=offset;
    for(i=0;i<0x14;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//020 dwords
    offset+=0x14;
    for(i=0;i<0x0F;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//015 dwords
    offset+=0x0F;

    for(i=0;i<0x0E;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//014 dwords
    offset+=0x0E;
    for(i=0;i<0x44;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//068 dwords
    offset+=0x44;
    for(i=0;i<0x20;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//032 dwords
    offset+=0x20;
    for(i=0;i<0x0F;i++) VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4+i*4)=0;//015 dwords
    offset+=0x0F;

    //total: +0x1E0
    //theoretically, offset=0x369C/4=0xDA7

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset*4)=0;
    offset++;

    //total: +0x1E1
    //theoretically, offset=0x36A0/4=0xDA8

    //Padding : 4 dwords?

    //total: +0x1E5
    //theoretically, offset=0x36B0/4=0xDAC

#ifdef DBG
    if (offset+4!=0x36B0/4) debugPrint("pb_3D_init: bad final value for offset\n");
#endif
    //floating point post-initializations in cmn structure

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x380)=0x3F800000; //1.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x384)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x388)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x38C)=0x00000000; //0.0f

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3C0)=0x40000000; //2.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3C4)=0x3F800000; //1.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3C8)=0x3F000000; //0.5f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3CC)=0x00000000; //0.0f

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3D0)=0x40000000; //2.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3D4)=0x3F800000; //1.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3D8)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3DC)=0xBF800000; //-1.0f

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3E0)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3E4)=0xBF800000; //-1.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3E8)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3EC)=0x00000000; //0.0f

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x390)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x394)=0x3F800000; //1.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x398)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x39C)=0x00000000; //0.0f

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3F0)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3F4)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3F8)=0x00000000; //0.0f
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_cmn*4+0x3FC)=0x00000000; //0.0f

    //post-initializations in pipe structure

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x160)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x164)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x168)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x16C)=0;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x100)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x104)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x108)=0x000FE000;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x10C)=0;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x110)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x114)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x118)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x11C)=0;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x130)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x134)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x138)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x13C)=0;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x180)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x184)=0x000003F8;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x188)=0;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_pipe*4+0x18C)=0;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_4dwords*4)=0x002FE000;

    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x010)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x014)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x018)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x01C)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x020)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x024)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x028)=0x001C527C;
    VIDEOREG(NV_PRAMIN+(Inst<<4)+offset_20dwords*4+0x02C)=0x001C527C;

#ifdef DBG
    //at this point pb_GrCtxID and pb_FifoChannelID must be different
    //debugPrint("pb_3D_init: gr=%d fifo=%d\n",pb_GrCtxID,pb_FifoChannelID);
#endif
}

static inline void pb_create_gr_instance(int ChannelID,
                                         int Class,
                                         DWORD instance,
                                         DWORD flags,
                                         DWORD flags3D,
                                         struct s_CtxDma *pGrObject)
{
    DWORD offset = instance << 4;
    VIDEOREG(NV_PRAMIN + offset + 0x00) = flags;
    VIDEOREG(NV_PRAMIN + offset + 0x04) = flags3D;
    VIDEOREG(NV_PRAMIN + offset + 0x08) = 0;
    VIDEOREG(NV_PRAMIN + offset + 0x0C) = 0;

    memset(pGrObject,0,sizeof(struct s_CtxDma));

    pGrObject->ChannelID = ChannelID;
    pGrObject->Class = Class;
    pGrObject->isGr = 1;
    pGrObject->Inst = instance;
}

void pb_create_gr_ctx(int ChannelID,
                      int Class,
                      struct s_CtxDma *pGrObject)
{
    DWORD           flags;
    DWORD           flags3D;
    int             size;
    DWORD           Inst;

    flags=Class&0x000000FF;
    flags3D=0;

    switch (Class)
    {
    case GR_CLASS_97:
        size=0x330; //816 bytes
        flags3D=0x00000A00;
        break;
    case GR_CLASS_39:
        size=16;        //16 bytes
        flags|=0x01000000;
        break;
    case GR_CLASS_12:
    case GR_CLASS_19:
    case GR_CLASS_30:
    case GR_CLASS_62:
    case GR_CLASS_72:
    case GR_CLASS_9F:
        size=16;        //16 bytes
        break;
    default:
        //"CreateGrObject invalid class number"
        size=Class;
        break;
    }

    Inst=pb_FreeInst;
    pb_FreeInst+=(size>>4);

    if (flags3D)
    {
        pb_3DGrCtxInst[pb_FifoChannelID]=Inst;
        pb_3D_init();
    }

    pb_create_gr_instance(ChannelID, Class, Inst, flags, flags3D, pGrObject);
}


static void pb_start(void)
{
    if (pb_disable_gpu==0) //do we really want to send data to GPU?
    {
        //asks push buffer Dma engine to detect incoming Dma data (written at pb_Put)

        pb_cache_flush();
        *(pb_DmaUserAddr+0x40/4)=((DWORD)pb_Put)&0x03FFFFFF;
        //from now any write will be detected
        pb_Kicked=pb_Put;

#ifdef DBG
        if ((*(pb_DmaUserAddr+0x44/4))>0x04000000)
        {
            debugPrint("pb_start: wrong GetAddr\n");
            return;
        }
#endif
    }
}




static void pb_jump_to_head(void)
{
    //Have Dma engine pointer point at push buffer head again.
    //(so we don't run into the tail of push buffer)
    //The best method would be to call this once per frame since it costs time.
    //Of course, avoid writing more data than push buffer size in 1 frame time.
    //If it happens you will get a message suggesting to call pb_reset more often
    //or to enlarge push buffer (with pb_size, before calling pb_init).
    //Default size is 512Kb (128*1024 dwords)

    uint32_t        *pGetAddr;

    DWORD           TimeStampTicks;

#ifdef DBG
    if (pb_BeginEndPair)
    {
        debugPrint("pb_reset musn't be called inside a begin-end block.\n");
        return;
    }
#endif

    //The Dma engine must have started on what was pushed since the last jump before
    //PUT goes back to the head: while GET is still at the head, PUT = head reads as an
    //empty push buffer and all of it (a whole frame, its PB_FINISHED included) is skipped.
    //(A fast frame loop can get here before the engine has fetched anything: seen on xemu.)
    if (pb_Put!=pb_Head)
    {
        pb_kick();
        TimeStampTicks=KeTickCount;
        while ((((*(pb_DmaUserAddr+0x44/4))|0x80000000)==(DWORD)pb_Head)&&(KeTickCount-TimeStampTicks<=TICKSTIMEOUT)) {};
    }

    //writes a jump command
    //forces GPU to jump at push buffer head address at next fetch
    *(pb_Put+0)=1+(((DWORD)pb_Head)&0x0FFFFFFF);
    pb_Put=pb_Head;
    pb_start();

    TimeStampTicks=KeTickCount;

    //wait for arrival of Gpu Get to push buffer head
    do
    {
        if ((*(pb_DmaUserAddr+0x44/4))>0x04000000)
        {
#ifdef DBG
            debugPrint("pb_reset: bad getaddr\n");
#endif
            return;
        }


        if (KeTickCount-TimeStampTicks>TICKSTIMEOUT)
        {
            debugPrint("pb_reset: too long\n");
            break;
        }

        //converts physical address into virtual address
        pGetAddr=(uint32_t *)((*(pb_DmaUserAddr+0x44/4))|0x80000000);
    }while (pGetAddr!=pb_Head);

}








//public functions

int pb_busy(void)
{
    DWORD           PutAddr;
    DWORD           GetAddr;

    pb_kick(); //blocks left by pb_end_deferred: GET never reaches a Put the Dma engine was not given

    GetAddr=*(pb_DmaUserAddr+0x44/4);
#ifdef DBG
    if (GetAddr>0x04000000)
    {
        debugPrint("pb_busy: wrong GetAddr\n");
        return 0;
    }
#endif
    PutAddr=(DWORD)pb_Put;

    if ((GetAddr^PutAddr)&0x0FFFFFFF) return 1; //means different addresses

    if (VIDEOREG(NV_PGRAPH_STATUS)) return 1;

    return 0;
}

DWORD pb_back_buffer_width(void)
{
    return  pb_FrameBuffersWidth;
}

DWORD pb_back_buffer_height(void)
{
    return pb_FrameBuffersHeight;
}

DWORD pb_back_buffer_pitch(void)
{
    return pb_FrameBuffersPitch;
}

DWORD *pb_back_buffer(void)
{
    return (DWORD *)pb_FBAddr[pb_back_index];
}

DWORD *pb_extra_buffer(int buffer_index)
{
    if (buffer_index>=pb_ExtraBuffersCount)
    {
        debugPrint("pb_extra_buffer: buffer index out of range\n");
        return NULL;
    }

    return (DWORD *)pb_EXAddr[buffer_index];
}


//NV097_SET_CONTROL0 (0x290, aka NV20_TCL_PRIMITIVE_3D_W_YUV_FPZ_FLAGS) value.
//bit 0  (0x00000001) STENCIL_WRITE_ENABLE (only meaningful with Z24S8, Z16 has no stencil plane)
//bit 12 (0x00001000) Z_FORMAT: 0=fixed point depth, 1=floating point depth
//bit 16 (0x00010000) Z_PERSPECTIVE_ENABLE: depth is the interpolated w (W-buffering) instead of z
//bit 20 (0x00100000) TEXTURE_PERSPECTIVE_ENABLE
//bit 28 (0x10000000) COLOR_SPACE_CONVERT (YUV), not used
//pbkit has always pushed 0x00110001 here (stencil write, W and perspective texturing on),
//so with the default settings (Z24S8, fixed point, W-buffer on) the value is unchanged.
static DWORD pb_control0(void)
{
    DWORD   control0;

    control0=NV097_SET_CONTROL0_TEXTURE_PERSPECTIVE_ENABLE;
    if (pb_DepthFmt==NV097_SET_SURFACE_FORMAT_ZETA_Z24S8) control0|=NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE;
    if (pb_DepthFloat) control0|=NV097_SET_CONTROL0_Z_FORMAT_FLOAT;
    if (pb_WBuffer) control0|=NV097_SET_CONTROL0_Z_PERSPECTIVE_ENABLE;

    return control0;
}

//Largest depth value of the current depth format, i.e. the value zmax=1.0f maps to
//in pb_set_viewport (VIEWPORT_PX_DIV2 z scale and DEPTH_RANGE/NV097_SET_CLIP_MAX).
//Fixed point: full scale of the integer (0xFFFFFF for Z24S8, 0xFFFF for Z16).
//Floating point (NV097_SET_CONTROL0_Z_FORMAT=1): the values xemu uses as the top of
//its depth clip range (hw/xbox/nv2a/pgraph/glsl/common.c, util.h):
// - Z16   : 511.9375 = largest value of the 16 bits float (raw 0xFFFF, (raw<<11)+0x3C000000 as IEEE754)
// - Z24S8 : 1.0E30, xemu's f24_max (the 24 bits float is raw<<7 as IEEE754, 3.4E38 max)
//These two have not been verified on a console yet.
static float pb_depth_scale_for(unsigned int fmt,int is_float)
{
    if (fmt==NV097_SET_SURFACE_FORMAT_ZETA_Z16) return is_float?511.9375f:(float)0xFFFF;
    return is_float?1.0E30f:(float)0xFFFFFF;
}

//Raw value written in the depth plane by pb_erase_depth_stencil_buffer: zmax of the viewport
//range, encoded in the current depth format (stencil, when present, is cleared to 0).
static DWORD pb_depth_clear_value(void)
{
    DWORD   bits;

    if (pb_DepthFloat)
    {
        *((float *)&bits)=pb_ZScale;
        if (pb_DepthFmt==NV097_SET_SURFACE_FORMAT_ZETA_Z16) return (bits-0x3C000000)>>11; //0xFFFF
        return (bits>>7)<<8; //f24(1.0E30)=0xE293E5, stencil=0
    }

    if (pb_DepthFmt==NV097_SET_SURFACE_FORMAT_ZETA_Z16) return 0xFFFF;
    return 0xFFFFFF00; //(depth<<8)|stencil
}

//Render target the GPU draws into (set_draw_target): pb_set_viewport clamps to its size
static  DWORD           pb_TargetWidth;
static  DWORD           pb_TargetHeight;
static  DWORD           pb_DepthStencilLastAddr;    //zeta address and DMA limit channel 10 points at
static  DWORD           pb_DepthStencilLastLimit;
static  int             pb_WideClip=0;              //window clip widened for a render target larger than the screen

//Multisampling (pb_set_multisample)
static  int             pb_MSMode=PB_MS_NONE;       //NV097_SET_SURFACE_FORMAT_ANTI_ALIASING value
static  DWORD           pb_MSScaleX=1;              //sample buffer size = scan-out size * scale
static  DWORD           pb_MSScaleY=1;
static  DWORD           pb_MSAddr;                  //sample (multisampled) color buffer, 0 without multisampling
static  DWORD           pb_MSPitch;
static  DWORD           pb_MSSize;
static  int             pb_MSTiled;

//Points the GPU color (and zeta) surface at a buffer. All pbkit render targets go through here:
//the rotating back buffers, extra buffers, the multisample buffer and pb_set_render_target.
//- buffer_addr/zeta_addr: physical addresses (zeta_addr 0: no depth stencil buffer)
//- width/height: surface clip (1x size, the GPU scales it for an anti-aliased surface)
//- color_size/zeta_size: bytes the DMA contexts cover
//- format: NV097_SET_SURFACE_FORMAT value (type, anti-aliasing, log2 size when swizzled)
//The zeta DMA context and the depth/stencil test enables are only pushed when the depth
//buffer changes (appears, disappears or moves), so the default back buffer path pushes
//exactly what it always did.
static void set_draw_target(DWORD buffer_addr, DWORD width, DWORD height, DWORD pitch, DWORD color_size,
                            DWORD zeta_addr, DWORD zeta_pitch, DWORD zeta_size, DWORD format)
{
    uint32_t        *p;

    DWORD           pitch_depth_stencil;

    DWORD           dma_flags;
    DWORD           dma_addr;
    DWORD           dma_limit;

    int         flag;
    int         depth_stencil;

    pitch_depth_stencil=zeta_pitch;

    //DMA channel 9 is used by GPU in order to render pixels
    dma_addr=buffer_addr;
    dma_limit=color_size-1; //(last byte)
    dma_flags=DMA_CLASS_3D|0x0000B000;
    dma_addr|=3;

    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID9Inst<<4)+0x08,dma_addr); //set params addr,data
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER); //calls subprogID PB_SETOUTER: does VIDEOREG(addr)=data
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID9Inst<<4)+0x0C,dma_addr);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID9Inst<<4)+0x00,dma_flags);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID9Inst<<4)+0x04,dma_limit);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT3,9);
    pb_end(p);

    //DMA channel 11 is used by GPU in order to bitblt images
    dma_addr=buffer_addr;
    dma_limit=color_size-1; //(last byte)
    dma_flags=DMA_CLASS_3D|0x0000B000;
    dma_addr|=3;

    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID11Inst<<4)+0x08,dma_addr); //set params addr,data
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER); //calls subprogID PB_SETOUTER: does VIDEOREG(addr)=data
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID11Inst<<4)+0x0C,dma_addr);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID11Inst<<4)+0x00,dma_flags);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID11Inst<<4)+0x04,dma_limit);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
    p=pb_push1_to(SUBCH_4,p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT2,11);
    pb_end(p);

    depth_stencil=zeta_addr?1:0; //0: no depth stencil buffer (PB_DEPTH_NONE, render target without one)

    if (pb_DepthStencilLast != depth_stencil) // depth stencil buffer appears or disappears?
    {
        //DMA channel 10 is used by GPU in order to render depth stencil
        if (depth_stencil)
        {
            dma_addr=zeta_addr;
            dma_limit=zeta_size-1; //(last byte)
            dma_flags=DMA_CLASS_3D|0x0000B000;
            dma_addr|=3;
            flag=1;
        }
        else
        {
            dma_addr=0;
            dma_limit=0;
            dma_flags=DMA_CLASS_3D|0x0000B000;
            dma_addr|=3;
            flag=0;
            pitch_depth_stencil=pitch;
        }

        p=pb_begin();
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x08,dma_addr); //set params addr,data
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER); //calls subprogID PB_SETOUTER: does VIDEOREG(addr)=data
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x0C,dma_addr);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x00,dma_flags);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x04,dma_limit);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT4,10);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DEPTH_TEST_ENABLE,flag); //ZEnable=TRUE or FALSE (But don't use W, see below)
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_ENABLE,(flag&&(pb_DepthFmt==NV097_SET_SURFACE_FORMAT_ZETA_Z24S8))?1:0); //StencilEnable=TRUE (Z24S8 only)
        if (!flag) p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DEPTH_WRITE_ENABLE,0); //no depth buffer: ZWriteEnable=FALSE too
        else if (pb_DepthStencilLast==0) p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DEPTH_WRITE_ENABLE,1); //back from a target without one
        pb_end(p);

        pb_DepthStencilLast=depth_stencil;
        pb_DepthStencilLastAddr=zeta_addr;
        pb_DepthStencilLastLimit=depth_stencil?(zeta_size-1):0;
    }
    else if (depth_stencil && ((zeta_addr!=pb_DepthStencilLastAddr)||(zeta_size-1!=pb_DepthStencilLastLimit)))
    {
        //another depth stencil buffer (a render target with its own one): move DMA channel 10 only
        dma_addr=zeta_addr|3;
        dma_limit=zeta_size-1;
        dma_flags=DMA_CLASS_3D|0x0000B000;

        p=pb_begin();
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x08,dma_addr);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x0C,dma_addr);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x00,dma_flags);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PRAMIN+(pb_DmaChID10Inst<<4)+0x04,dma_limit);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER);
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT4,10);
        pb_end(p);

        pb_DepthStencilLastAddr=zeta_addr;
        pb_DepthStencilLastLimit=zeta_size-1;
    }

    p=pb_begin();
    p=pb_push3(p,NV20_TCL_PRIMITIVE_3D_BUFFER_PITCH,(pitch_depth_stencil<<16)|(pitch&0xFFFF),0,0);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_HORIZ,width<<16,height<<16);
    //NV097_SET_CONTROL0, see pb_control0 (0x00110001 with the default settings:
    //stencil write (0x00000001), W (0x00010000), perspective texturing (0x00100000),
    //no YUV (0x10000000), no floating point depth (0x00001000))
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_W_YUV_FPZ_FLAGS,pb_control0());
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BUFFER_FORMAT,format);
    pb_end(p);

    //pb_init's window clip (scissor) covers the screen: widen it for a larger render target,
    //narrow it back afterwards (only then: the default path pushes nothing here)
    flag=((width>pb_FrameBuffersWidth)||(height>pb_FrameBuffersHeight))?1:0;
    if (flag!=pb_WideClip)
    {
        int i;
        DWORD w=flag?4096:pb_FrameBuffersWidth;
        DWORD h=flag?4096:pb_FrameBuffersHeight;
        p=pb_begin();
        for(i=0;i<8;i++)
        {
            p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_HORIZ(i),0|((w-1)<<16));
            p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_VERT(i),0|((h-1)<<16));
        }
        pb_end(p);
        pb_WideClip=flag;
    }

    pb_TargetWidth=width;
    pb_TargetHeight=height;
}

//The rotating back buffers, extra buffers (scan-out size, pbkit's depth stencil buffer)
static void set_draw_buffer(DWORD buffer_addr)
{
    set_draw_target(buffer_addr,pb_FrameBuffersWidth,pb_FrameBuffersHeight,pb_FrameBuffersPitch,
                    pb_FrameBuffersHeight*pb_FrameBuffersPitch,
                    pb_DSAddr&0x03FFFFFF,pb_DepthStencilPitch,pb_FrameBuffersHeight*pb_DepthStencilPitch,
                    pb_GPUFrameBuffersFormat|pb_FBVFlag);
}

//Multisampling: the scene is drawn into the sample buffer (and the scaled depth stencil
//buffer); pb_finished resolves it into the rotating back buffer (scan-out size)
static void set_draw_multisample_buffer(void)
{
    set_draw_target(pb_MSAddr&0x03FFFFFF,pb_FrameBuffersWidth,pb_FrameBuffersHeight,pb_MSPitch,pb_MSSize,
                    pb_DSAddr&0x03FFFFFF,pb_DepthStencilPitch,pb_FrameBuffersHeight*pb_MSScaleY*pb_DepthStencilPitch,
                    pb_GPUFrameBuffersFormat|pb_FBVFlag|
                    (pb_MSMode<<12)); //NV097_SET_SURFACE_FORMAT_ANTI_ALIASING
}

void pb_target_back_buffer(void)
{
    if (pb_MSAddr)
    {
        set_draw_multisample_buffer();
        return;
    }
    set_draw_buffer(pb_FBAddr[pb_back_index]&0x03FFFFFF);
}

void pb_target_extra_buffer(int buffer_index)
{
    if (buffer_index>=pb_ExtraBuffersCount)
    {
        debugPrint("pb_target_extra_buffer: buffer index out of range\n");
        return;
    }

    if (pb_MSAddr)
    {
        //multisampling: extra buffers stay 1x, the (scaled) depth stencil buffer can't go with them
        set_draw_target(pb_EXAddr[buffer_index]&0x03FFFFFF,pb_FrameBuffersWidth,pb_FrameBuffersHeight,
                        pb_FrameBuffersPitch,pb_FrameBuffersHeight*pb_FrameBuffersPitch,0,0,0,
                        pb_GPUFrameBuffersFormat|pb_FBVFlag);
        return;
    }
    set_draw_buffer(pb_EXAddr[buffer_index]&0x03FFFFFF);
}

//Byte offset of texel (x,y) in a swizzled (Morton order) w x h surface or texture, w and h
//powers of two: x and y bits interleave (x in bit 0) while both have bits left, then the
//remaining bits of the larger one follow.
DWORD pb_swizzle_offset(DWORD x, DWORD y, DWORD w, DWORD h, DWORD bytes_per_pixel)
{
    DWORD   offset=0;
    DWORD   bit=1;
    DWORD   mx=1;
    DWORD   my=1;

    while ((mx<w)||(my<h))
    {
        if (mx<w)
        {
            if (x&mx) offset|=bit;
            bit<<=1;
            mx<<=1;
        }
        if (my<h)
        {
            if (y&my) offset|=bit;
            bit<<=1;
            my<<=1;
        }
    }
    return offset*bytes_per_pixel;
}

//bytes per pixel of a NV097_SET_SURFACE_FORMAT_COLOR_* format, 0 if unknown
static int pb_color_bytes(unsigned int fmt)
{
    switch(fmt)
    {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
            return 1;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
            return 2;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
            return 4;
    }
    return 0;
}

static DWORD pb_log2(DWORD v)
{
    DWORD n=0;
    while ((1u<<n)<v) n++;
    return n;
}

//1 if [addr, addr+size) is mapped 1:1 below 0x03FFB000 (first byte checked: the buffer must be
//contiguous memory, MmAllocateContiguousMemoryEx)
static int pb_target_memory_ok(DWORD addr, DWORD size)
{
    return (MmGetPhysicalAddress((PVOID)addr)==(addr&0x03FFFFFF))&&((addr&0x03FFFFFF)+size<=0x03FFB000);
}

int pb_set_render_target(const pb_render_target *rt)
{
    DWORD           addr,zeta,pitch,zeta_pitch,size,zeta_size,format,zeta_fmt;
    int             bytes,zeta_bytes;

    if (!rt)
    {
        pb_target_back_buffer();
        return 0;
    }

    bytes=pb_color_bytes(rt->color_format);
    //a render target's depth buffer has pbkit's depth format (Z24S8 with PB_DEPTH_NONE)
    zeta_fmt=(pb_DepthFmt==PB_DEPTH_NONE)?NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:pb_DepthFmt;
    zeta_bytes=(zeta_fmt==NV097_SET_SURFACE_FORMAT_ZETA_Z16)?2:4;
    addr=(DWORD)rt->addr;
    zeta=(DWORD)rt->depth_addr;

    if ((bytes==0)||(addr==0)||(addr&0x3F)||(zeta&0x3F)||(rt->width==0)||(rt->height==0)||
        (rt->width>4096)||(rt->height>4096))
    {
        debugPrint("pb_set_render_target: invalid format, address or size\n");
        return -1;
    }

    if (rt->swizzled)
    {
        //power of two sizes, Morton order: the GPU takes log2 width/height, no pitch
        if ((rt->width&(rt->width-1))||(rt->height&(rt->height-1)))
        {
            debugPrint("pb_set_render_target: a swizzled target needs power of two sizes\n");
            return -1;
        }
        pitch=rt->width*bytes;
        zeta_pitch=rt->width*zeta_bytes;
        format=(NV097_SET_SURFACE_FORMAT_TYPE_SWIZZLE<<8)|(pb_log2(rt->width)<<16)|(pb_log2(rt->height)<<24);
    }
    else
    {
        pitch=rt->pitch;
        zeta_pitch=rt->depth_pitch;
        if ((pitch<rt->width*bytes)||(pitch&0x3F)||(pitch>0xFFC0)||
            (zeta&&((zeta_pitch<rt->width*zeta_bytes)||(zeta_pitch&0x3F)||(zeta_pitch>0xFFC0))))
        {
            debugPrint("pb_set_render_target: pitches must be >= width*Bpp and multiples of 64\n");
            return -1;
        }
        format=NV097_SET_SURFACE_FORMAT_TYPE_PITCH<<8;
    }
    format|=(zeta_fmt<<4)|rt->color_format;
    size=pitch*rt->height;
    zeta_size=zeta?(zeta_pitch*rt->height):0;

    if (!pb_target_memory_ok(addr,size)||(zeta&&!pb_target_memory_ok(zeta,zeta_size)))
    {
        debugPrint("pb_set_render_target: buffers must be contiguous memory below 0x03FFB000\n");
        return -1;
    }

    set_draw_target(addr&0x03FFFFFF,rt->width,rt->height,pitch,size,
                    zeta&0x03FFFFFF,zeta?zeta_pitch:0,zeta_size,format);
    return 0;
}

void pb_set_multisample(int mode)
{
    assert(pb_running==0); //the sample buffer is allocated by pb_init
    assert((mode==PB_MS_NONE)||(mode==PB_MS_2X)||(mode==PB_MS_4X));
    pb_MSMode=mode;
}

int pb_get_multisample(void)
{
    return pb_MSMode;
}

DWORD *pb_multisample_buffer(void)
{
    return (DWORD *)pb_MSAddr;
}

DWORD pb_multisample_pitch(void)
{
    return pb_MSAddr?pb_MSPitch:0;
}

int pb_overlay_update(const pb_overlay *ov)
{
    DWORD phys,fmt,end;

    if (ov==NULL||ov->surface==NULL) return -1;
    if ((ov->format!=PB_OVERLAY_UYVY)&&(ov->format!=PB_OVERLAY_YUY2)) return -1;
    if ((ov->pitch==0)||(ov->pitch&63)||(ov->pitch>NV_PVIDEO_FORMAT_PITCH)) return -1;
    if ((ov->src_width==0)||(ov->src_height==0)||(ov->dst_width==0)||(ov->dst_height==0)) return -1;
    if ((ov->src_x&1)||(ov->src_width&1)||(ov->src_x+ov->src_width>2046)||(ov->src_y+ov->src_height>2047)) return -1;
    if ((ov->dst_x>4095)||(ov->dst_y>4095)||(ov->dst_width>4095)||(ov->dst_height>4095)) return -1;
    if ((ov->dst_width*8<ov->src_width)||(ov->dst_height*8<ov->src_height)) return -1;
    if ((ov->src_x+ov->src_width)*2>ov->pitch) return -1;
    phys=(DWORD)MmGetPhysicalAddress((PVOID)ov->surface);
    end=phys+ov->pitch*(ov->src_y+ov->src_height);
    if ((phys&63)||(end>0x04000000)) return -1;

    fmt=ov->pitch;
    if (ov->format==PB_OVERLAY_YUY2) fmt|=NV_PVIDEO_FORMAT_COLOR_LE_CR8YB8CB8YA8;
    if (ov->color_key_enable) fmt|=NV_PVIDEO_FORMAT_DISPLAY_COLOR_KEY;
    if (ov->bt709) fmt|=NV_PVIDEO_FORMAT_MATRIX_ITURBT709;

    pb_OverlayPending=0;    //the VBlank interrupt leaves the registers alone while they change
    __asm__ __volatile__("" ::: "memory");
    pb_OverlayReg[0]=0;                                                 //BASE
    pb_OverlayReg[1]=0x03FFFFFF;                                        //LIMIT (64 Mb)
    pb_OverlayReg[2]=phys;                                              //OFFSET
    pb_OverlayReg[3]=((ov->src_y+ov->src_height)<<16)|(ov->src_x+ov->src_width); //SIZE_IN
    pb_OverlayReg[4]=(ov->src_y<<20)|(ov->src_x<<4);                    //POINT_IN (12.4 fixed point)
    pb_OverlayReg[5]=(ov->src_width<<20)/ov->dst_width;                 //DS_DX
    pb_OverlayReg[6]=(ov->src_height<<20)/ov->dst_height;               //DT_DY
    pb_OverlayReg[7]=(ov->dst_y<<16)|ov->dst_x;                         //POINT_OUT
    pb_OverlayReg[8]=(ov->dst_height<<16)|ov->dst_width;                //SIZE_OUT
    pb_OverlayReg[9]=fmt;                                               //FORMAT
    pb_OverlayReg[10]=ov->color_key&0xFFFFFF;                           //COLOR_KEY
    __asm__ __volatile__("" ::: "memory");
    pb_OverlayPending=1;

    if (!pb_running) pb_overlay_apply(pb_vbl_counter);
    return 0;
}

void pb_overlay_disable(void)
{
    pb_OverlayPending=2;
    if (!pb_running) pb_overlay_apply(pb_vbl_counter);
}

int pb_overlay_update_pending(void)
{
    return pb_OverlayPending!=0;
}

DWORD pb_overlay_last_vbl(void)
{
    return pb_OverlayLastVBL;
}

DWORD pb_get_vbl_counter(void)
{
    return pb_vbl_counter; //allows caller to know if a frame has been missed
}

void pb_set_vblank_callback(void (*cb)(DWORD vbl_count))
{
    pb_VBlankCallback=cb;
}

void pb_set_swap_callback(void (*cb)(DWORD frame, DWORD vbl_count))
{
    pb_SwapCallback=cb;
}

void pb_set_presentation_interval(int interval)
{
    if ((interval<PB_PRESENT_INTERVAL_IMMEDIATE)||(interval>PB_PRESENT_INTERVAL_ONE_OR_IMMEDIATE))
    {
        debugPrint("pb_set_presentation_interval: invalid interval %d\n",interval);
        return;
    }
    pb_PresentInterval=interval;
}

int pb_get_presentation_interval(void)
{
    return pb_PresentInterval;
}

/*
 * Display field status. The NV2A CRTC has no field register xemu emulates
 * (NV_PCRTC_RASTER is a counter there) and in the SDTV modes the TV encoder,
 * not the CRTC, interlaces: the NV2A scans out a whole 480 line frame per
 * VBlank and each VBlank is one TV field. So:
 * - interlaced or not comes from the AV mode the kernel set
 *   (AvSendTVEncoderOption AV_OPTION_QUERY_MODE): output mode bits 31:30
 *   0 (480 line SDTV) and 1 (525 line SDTV) are interlaced, 2 (HDTV) only
 *   with TV table index 3 (1080i), 3 (VGA) never;
 * - the field alternates with the VBlank counter, its absolute parity is
 *   anchored once per pb_init on the kernel's AV_OPTION_GUESS_FIELD (non-zero
 *   = odd field).
 * The two kernel calls are made on the first call after pb_init (call it at
 * PASSIVE_LEVEL once, e.g. after pb_init, not from a callback first).
 */
#define PB_AV_OPTION_QUERY_MODE     13
#define PB_AV_OPTION_GUESS_FIELD    15
static int pb_FieldPhase=-2;    //-2: not queried yet, -1: progressive/unknown, 0/1: field at even VBlank counts

void pb_get_display_field_status(DWORD *vbl_count, int *field)
{
    DWORD   vbl;
    ULONG   mode,guess,out;
    int     tries;

    if (pb_FieldPhase==-2)
    {
        mode=0;
        AvSendTVEncoderOption((PVOID)VIDEO_BASE,PB_AV_OPTION_QUERY_MODE,0,&mode);
        out=mode&0xC0000000;
        if ((mode==0)||(out==0xC0000000)||((out==0x80000000)&&((mode&0xFF)!=3)))
            pb_FieldPhase=-1;
        else
        {
            for(tries=0;tries<4;tries++)
            {
                vbl=pb_vbl_counter;
                guess=0;
                AvSendTVEncoderOption((PVOID)VIDEO_BASE,PB_AV_OPTION_GUESS_FIELD,0,&guess);
                if (vbl==pb_vbl_counter) break; //no VBlank in between
            }
            pb_FieldPhase=((guess!=0)?1:0)^(int)(vbl&1);
        }
    }
    vbl=pb_vbl_counter;
    if (vbl_count) *vbl_count=vbl;
    if (field) *field=(pb_FieldPhase<0)?-1:(int)((vbl&1)^(DWORD)pb_FieldPhase);
}

/*
 * Fences: the 3D semaphore DMA context (8, set up by pb_init) points at
 * pb_DmaBuffer8. NV097_BACK_END_WRITE_SEMAPHORE_RELEASE makes the GPU write
 * the fence value there once the rendering pushed before it has passed the
 * back end; the CPU compares it with the fence (signed 32-bit difference, so
 * the counter may wrap; pb_init starts it just below the wrap).
 */
DWORD pb_insert_fence(void)
{
    uint32_t        *p;
    DWORD           fence;

    fence=pb_FenceLast+1;
    p=pb_begin();
    p=pb_push2(p,NV097_SET_SEMAPHORE_OFFSET,0,fence); //+NV097_BACK_END_WRITE_SEMAPHORE_RELEASE
    pb_end_deferred(p);
    pb_FenceLast=fence;
    return fence;
}

bool pb_is_fence_pending(DWORD fence)
{
    if (pb_FenceMem==NULL) return false;
    if ((LONG)(fence-pb_FenceLast)>0) return false;     //never inserted
    if ((LONG)(fence-*pb_FenceMem)<=0) return false;    //the GPU has written this value or a later one
    pb_kick(); //the fence may still be in blocks left by pb_end_deferred
    return true;
}

void pb_block_on_fence(DWORD fence)
{
    while (pb_is_fence_pending(fence)) __asm__ __volatile__ ("pause");
}


void pb_release_debug_screen(void)
{
    if (pb_DebugScreenGone || !pb_running || !XVideoGetFB()) return;
    pb_DebugScreenGone=1;
    MmFreeContiguousMemory(XVideoGetFB());
}

void pb_back_buffers(int n)
{
    pb_BackBuffersWanted=(n==1)?1:2;
}

void pb_set_frame_buffers(DWORD *addrs[], int count, DWORD pitch)
{
    int i;

    assert(pb_running==0); //used by pb_init
    if ((addrs==NULL)||(count<=0))
    {
        pb_AppFBCount=0; //back to buffers allocated by pbkit
        return;
    }
    assert(count<=3);
    for(i=0;i<count;i++) pb_AppFBAddr[i]=(DWORD)addrs[i];
    pb_AppFBCount=count;
    pb_AppFBPitch=pitch;
}

void pb_set_depth_stencil_buffer(DWORD *addr, DWORD pitch)
{
    assert(pb_running==0); //used by pb_init
    pb_AppDSAddr=(DWORD)addr; //NULL: back to a buffer allocated by pbkit
    pb_AppDSPitch=pitch;
}

/*
 * Double buffering (pb_back_buffers(1)): after pb_finished() the next back
 * buffer is the one on screen until the VBlank handler shows the frame just
 * finished. Nothing makes the GPU wait for that, so the caller waits here,
 * on the CPU, before it pushes the next frame's drawing: until every frame
 * pb_finished() queued is on screen. A no-op with triple buffering, where
 * the next back buffer is never the one shown.
 */
void pb_wait_for_flip(void)
{
    LARGE_INTEGER timeout;

    pb_kick(); //the frame waited on may still be in blocks left by pb_end_deferred
    if (pb_FrameBuffersCount>2) return;
    timeout.QuadPart=-10000;    //1 ms: the VBlank event is pulsed, a pulse between the test and the wait is not lost for a whole frame
    while (pb_FramesShown!=pb_FramesFinished)
        NtWaitForSingleObject(pb_VBlankEvent, FALSE, &timeout);
}

DWORD pb_wait_for_vbl(void)
{
    pb_kick(); //no work left unsent while the CPU sleeps
    NtWaitForSingleObject(pb_VBlankEvent, FALSE, NULL);
    return pb_vbl_counter; //allows caller to know if a frame has been missed
}




void pb_extra_buffers(int n)
{
    if (n>MAX_EXTRA_BUFFERS)
        debugPrint("Too many extra buffers\n");
    else
        pb_ExtraBuffersCount=n;
}

void pb_size(DWORD size)
{
    if (pb_running)
        debugPrint("Can't set size while push buffer Dma engine is running.\n");
    else
    {
        if (size<64*1024)
            debugPrint("Push buffer size must be equal or larger than 64Kb.\n");
        else
        if ((size-1)&size)
            debugPrint("Push buffer size must be a power of 2.\n");
        else
        pb_Size=size;
    }
}


void pb_reset(void)
{
    pb_jump_to_head();
}


uint32_t *pb_begin(void)
{
#ifdef DBG
    if (pb_Put>=pb_Tail) debugPrint("ERROR! Push buffer overflow! Use pb_reset more often or enlarge push buffer!\n");

    if (pb_BeginEndPair==1) debugPrint("pb_begin without a pb_end earlier\n");
    pb_BeginEndPair=1;
    pb_PushIndex=0;
    pb_PushNext=pb_Put;
    pb_PushStart=pb_Put;
#endif
    return pb_Put;
}

#ifdef LOG
static FILE *fd;
static int logging=0;


void pb_start_log(void)
{
    if (logging) return;

    logging=1;

    fd=fopen("pbkit_record.txt","w");
}

void pb_stop_log(void)
{
    if (logging==0) return;

    logging=0;
    fclose(fd);
}
#endif


void pb_end(uint32_t *pEnd)
{
    DWORD           TimeStamp1;
    DWORD           TimeStamp2;

    int         i;

#ifdef LOG
    uint32_t    *p;
    int         n;

    if (logging)
    {
        p=pb_PushStart;
        while (p!=pEnd)
        {
            n=(*p>>18)&0x7FF;
            fprintf(fd,"0x%08x, ",*(p++));
            for(i=0;i<n;i++) fprintf(fd,"0x%x, ",*(p++));
            fprintf(fd,"\n");
        }

    }
#endif

#ifdef DBG
    if (pEnd!=pb_PushNext)
    {
        debugPrint("pb_end: input pointer invalid or not following previous write addresses\n");
        assert(false);
    }
    if (pb_BeginEndPair==0)
    {
        debugPrint("pb_end without a pb_begin\n");
        assert(false);
    }
    pb_BeginEndPair=0;
#endif

    pb_Put=pEnd;

    pb_start(); //start (or continue) reading and sending data to GPU

    if (pb_trace_mode) //do we want to wait until block data has been sent (for debugging GPU errors)?
    {

        TimeStamp1=KeTickCount;

        //wait until all begin-end block has been sent to GPU
        while(pb_busy())
        {
            TimeStamp2=KeTickCount;
            if (TimeStamp2-TimeStamp1>TICKSTIMEOUT)
            {
                debugPrint("pb_end: Busy for too long (%lu) (%08x)\n",
                    ((DWORD)(pb_Put)-(DWORD)(pb_Head)),
                    VIDEOREG(NV_PFIFO_CACHE1_DMA_GET)
                    );
                break;
            }
        }
    }
}


/*
 * pb_end without the kick: the block is queued (pb_Put moves on) but the Dma
 * engine is told only once PB_DEFER_DWORDS have built up, at the next pb_end
 * or pb_kick, or before pbkit waits on the GPU (pb_busy, pb_wait_for_flip,
 * pb_wait_for_vbl, pb_reset, pb_finished). Each kick is a write-combining
 * flush, a read-modify-write and spin on NV_PFB_WC_CACHE and the PUT write
 * (pb_start): for a caller ending hundreds of small blocks a frame, most of
 * their cost. A caller that waits on the GPU by other means (polling memory
 * the GPU writes) calls pb_kick first.
 */
#define PB_DEFER_DWORDS 1024u

void pb_end_deferred(uint32_t *pEnd)
{
#ifdef DBG
    if (pEnd!=pb_PushNext)
    {
        debugPrint("pb_end_deferred: input pointer invalid or not following previous write addresses\n");
        assert(false);
    }
    if (pb_BeginEndPair==0)
    {
        debugPrint("pb_end_deferred without a pb_begin\n");
        assert(false);
    }
    pb_BeginEndPair=0;
#endif

    pb_Put=pEnd;
    if (pb_trace_mode || !pb_Kicked || pb_Put<pb_Kicked || (DWORD)(pb_Put-pb_Kicked)>=PB_DEFER_DWORDS)
        pb_start();
}

void pb_kick(void)
{
    if (pb_Put && pb_Put!=pb_Kicked)
        pb_start();
}

void pb_push_to(DWORD subchannel, uint32_t *p, DWORD command, DWORD nparam)
{
#ifdef DBG
    if (p!=pb_PushNext)
    {
        debugPrint("pb_push_to: new write address invalid or not following previous write addresses\n");
        assert(false);
    }
    if (pb_BeginEndPair==0)
    {
        debugPrint("pb_push_to: missing pb_begin earlier\n");
        assert(false);
    }
    pb_PushIndex += 1 + nparam;
    pb_PushNext += 1 + nparam;
    if (pb_PushIndex>128)
    {
        debugPrint("pb_push_to: begin-end block musn't exceed 128 dwords\n");
        assert(false);
    }
#endif

    *(p+0)=EncodeMethod(subchannel,command,nparam);
}

void pb_show_front_screen(void)
{
    VIDEOREG(PCRTC_START)=pb_FBAddr[pb_front_index]&0x03FFFFFF;
    pb_debug_screen_active=0;
}

void pb_show_debug_screen(void)
{
    if (pb_DebugScreenGone) return; //given back (pb_release_debug_screen): keep showing the scene
    VIDEOREG(PCRTC_START)=((DWORD)XVideoGetFB())&0x0FFFFFFF;
    pb_debug_screen_active=1;
}

void pb_show_depth_screen(void)
{
    if (!pb_DSAddr) return; //no depth stencil buffer (PB_DEPTH_NONE)
    VIDEOREG(PCRTC_START)=pb_DSAddr&0x0FFFFFFF;
    pb_debug_screen_active=1;
}







void pb_set_viewport(int dwx,int dwy,int width,int height,float zmin,float zmax)
{
    uint32_t        *p;
    DWORD           dwzminscaled;
    DWORD           dwzmaxscaled;
    float           x,y,w,h;

    if (dwx<0) dwx=0;
    if (dwy<0) dwy=0;
    //clamped to the current render target (the screen before pb_init has set one)
    if (dwx+width>(pb_TargetWidth?pb_TargetWidth:pb_FrameBuffersWidth)) width=(pb_TargetWidth?pb_TargetWidth:pb_FrameBuffersWidth)-dwx;
    if (dwy+height>(pb_TargetHeight?pb_TargetHeight:pb_FrameBuffersHeight)) height=(pb_TargetHeight?pb_TargetHeight:pb_FrameBuffersHeight)-dwy;

    pb_Viewport_x=dwx;
    pb_Viewport_y=dwy;
    pb_Viewport_width=width;
    pb_Viewport_height=height;
    pb_Viewport_zmin=zmin;
    pb_Viewport_zmax=zmax;
    pb_Viewport_zminf=zmin;
    pb_Viewport_zmaxf=zmax;

    x=0.53125f+(float)dwx;
    y=0.53125f+(float)dwy;
    w=0.5f*((float)pb_Viewport_width);
    h=-0.5f*((float)pb_Viewport_height);
    *((float *)&dwzminscaled)=zmin*pb_ZScale;
    *((float *)&dwzmaxscaled)=zmax*pb_ZScale;
/*
    p=pb_begin();
    p=pb_push4f(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_OX,x+0.53125f,y+0.53125f,0.0f,0.0f);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_DEPTH_RANGE_NEAR,dwzminscaled,dwzmaxscaled);
    pb_end(p);
*/
    p=pb_begin();
    p=pb_push4f(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_OX,x+w,y-h,zmin*pb_ZScale,0.0f);
    p=pb_push4f(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_PX_DIV2,w,h,(zmax-zmin)*pb_ZScale,0.0f);
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_DEPTH_RANGE_NEAR,dwzminscaled,dwzmaxscaled);
    pb_end(p);
}




//ALWAYS use this at beginning of frame or you may lose one third of performance because
//automatic compression algorithm for tile #1 can't afford any garbage left behind...
//Also, try to draw from closest distance to farest distance to help algorithm
//Depth is set to max (see pb_depth_clear_value) and stencil, if any, is set to 0.
//Z24S8: (0xFFFFFF<<8)|0, clears depth & stencil. Z16: 0xFFFF, clears depth only.
//Implies that depth test function is set to "less or equal"
void pb_erase_depth_stencil_buffer(int x, int y, int w, int h)
{
    uint32_t    *p;

    int     x1,y1,x2,y2;

    if (!pb_DSAddr) return; //no depth stencil buffer (PB_DEPTH_NONE)

    x1=x;
    y1=y;
    x2=x+w;
    y2=y+h;

    p=pb_begin();
    pb_push(p++,NV20_TCL_PRIMITIVE_3D_CLEAR_VALUE_HORIZ,2);     //sets rectangle coordinates
    *(p++)=((x2-1)<<16)|x1;
    *(p++)=((y2-1)<<16)|y1;
    pb_push(p++,NV20_TCL_PRIMITIVE_3D_CLEAR_VALUE_DEPTH,3);     //sets data used to fill in rectangle
    *(p++)=pb_depth_clear_value();  //(depth<<8)|stencil for Z24S8, depth for Z16
    *(p++)=0;           //color
    if (pb_DepthFmt==NV097_SET_SURFACE_FORMAT_ZETA_Z24S8)
        *(p++)=NV097_CLEAR_SURFACE_Z|NV097_CLEAR_SURFACE_STENCIL; //0x03: triggers the HW rectangle fill (only on D&S)
    else
        *(p++)=NV097_CLEAR_SURFACE_Z;   //0x01: no stencil plane in Z16
    pb_end(p);
}




//Multisample resolve (pb_finished with pb_set_multisample(PB_MS_2X/PB_MS_4X)).
//The sample buffer holds each pixel's samples side by side: 2 horizontally (2X,
//NV097_SET_SURFACE_FORMAT_ANTI_ALIASING_CENTER_CORNER_2), 2x2 (4X, SQUARE_OFFSET_4).
//It is drawn as a linear texture (texel coordinates) on a screen sized quad into the
//rotating back buffer: pixel center x+0.5 lands on texel coordinate 2x+1 (and 2y+1 for 4X),
//the corner shared by the pixel's samples, where the bilinear filter weighs them equally,
//i.e. a box filter over the samples.
//Vertex program: MOV oT0,v9 / MOV oPos,v0 (end), in the last two of the 136 slots.
#define PB_RESOLVE_PROGRAM_SLOT 134
static const DWORD pb_ResolveProgram[8]={
    0x00000000,0x0020121b,0x0836106c,0x2070f848,
    0x00000000,0x0020001b,0x0836106c,0x2070f801 };

//linear texture format matching the color surface format
static DWORD pb_resolve_texture_format(void)
{
    switch(pb_ColorFmt)
    {
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
            return NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X1R5G5B5;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
            return NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_R5G6B5;
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
        case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
            return NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_X8R8G8B8;
        default:
            return NV097_SET_TEXTURE_FORMAT_COLOR_LU_IMAGE_A8R8G8B8;
    }
}

static void pb_resolve_multisample(void)
{
    static const float  corners[4][2]={{0.0f,0.0f},{1.0f,0.0f},{1.0f,1.0f},{0.0f,1.0f}}; //clockwise
    uint32_t            *p;
    float               w,h,tw,th;
    int                 i;

    w=(float)pb_FrameBuffersWidth;
    h=(float)pb_FrameBuffersHeight;
    tw=w*(float)pb_MSScaleX;
    th=h*(float)pb_MSScaleY;

    //the scene must be in memory before the texture unit reads it
    p=pb_begin();
    p=pb_push1(p,NV097_WAIT_FOR_IDLE,0);
    pb_end(p);

    //destination: the rotating back buffer (scan-out size), no anti-aliasing, no depth buffer
    //(depth and stencil tests go off, see set_draw_target)
    set_draw_target(pb_FBAddr[pb_back_index]&0x03FFFFFF,pb_FrameBuffersWidth,pb_FrameBuffersHeight,
                    pb_FrameBuffersPitch,pb_FrameBuffersHeight*pb_FrameBuffersPitch,0,0,0,
                    pb_GPUFrameBuffersFormat|pb_FBVFlag);

    p=pb_begin();
    //vertex program
    p=pb_push1(p,NV097_SET_TRANSFORM_PROGRAM_LOAD,PB_RESOLVE_PROGRAM_SLOT);
    pb_push(p++,NV097_SET_TRANSFORM_PROGRAM,8);
    for(i=0;i<8;i++) *(p++)=pb_ResolveProgram[i];
    p=pb_push1(p,NV097_SET_TRANSFORM_PROGRAM_START,PB_RESOLVE_PROGRAM_SLOT);
    p=pb_push1(p,NV097_SET_TRANSFORM_EXECUTION_MODE,
               NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM|(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV<<2));
    p=pb_push1(p,NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN,0);
    //texture stage 0: the sample buffer, linear (texel coordinates), bilinear, clamped
    p=pb_push2(p,NV097_SET_TEXTURE_OFFSET,pb_MSAddr&0x03FFFFFF,
               2|                                       //CONTEXT_DMA B (channel 3, all memory)
               (NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR<<3)|
               (2<<4)|                                  //DIMENSIONALITY 2
               (pb_resolve_texture_format()<<8)|
               (1<<16));                                //MIPMAP_LEVELS 1
    p=pb_push1(p,NV097_SET_TEXTURE_ADDRESS,0x00030303); //clamp to edge (u, v, p)
    p=pb_push1(p,NV097_SET_TEXTURE_CONTROL0,NV097_SET_TEXTURE_CONTROL0_ENABLE|NV097_SET_TEXTURE_CONTROL0_MAX_LOD_CLAMP);
    p=pb_push1(p,NV097_SET_TEXTURE_CONTROL1,pb_MSPitch<<16);
    p=pb_push1(p,NV097_SET_TEXTURE_FILTER,0x02022000);  //MIN, MAG linear
    p=pb_push1(p,NV097_SET_TEXTURE_IMAGE_RECT,(((DWORD)tw)<<16)|((DWORD)th));
    for(i=1;i<4;i++) p=pb_push1(p,NV097_SET_TEXTURE_CONTROL0+i*0x40,0);
    p=pb_push1(p,NV097_SET_SHADER_OTHER_STAGE_INPUT,0);
    p=pb_push1(p,NV097_SET_SHADER_STAGE_PROGRAM,NV097_SET_SHADER_STAGE_PROGRAM_STAGE0_2D_PROJECTIVE); //stages 1-3: none
    //combiners: one (unused) general stage, final combiner output = texture 0 (rgb and alpha)
    p=pb_push1(p,NV097_SET_COMBINER_COLOR_ICW,0);
    p=pb_push1(p,NV097_SET_COMBINER_COLOR_OCW,0);
    p=pb_push1(p,NV097_SET_COMBINER_ALPHA_ICW,0);
    p=pb_push1(p,NV097_SET_COMBINER_ALPHA_OCW,0);
    p=pb_push1(p,NV097_SET_COMBINER_CONTROL,NV097_SET_COMBINER_CONTROL_ITERATION_COUNT_ONE);
    p=pb_push1(p,NV097_SET_COMBINER_SPECULAR_FOG_CW0,0x00000008);  //A=B=C=0, D=texture 0: out=D
    p=pb_push1(p,NV097_SET_COMBINER_SPECULAR_FOG_CW1,0x00001800);  //G=texture 0 alpha
    //plain copy: no blending, alpha test, culling, logic op; all channels written, filled polygons
    p=pb_push1(p,NV097_SET_BLEND_ENABLE,0);
    p=pb_push1(p,NV097_SET_ALPHA_TEST_ENABLE,0);
    p=pb_push1(p,NV097_SET_CULL_FACE_ENABLE,0);
    p=pb_push1(p,NV097_SET_LOGIC_OP_ENABLE,0);
    p=pb_push1(p,NV097_SET_COLOR_MASK,0x01010101);
    p=pb_push2(p,NV097_SET_FRONT_POLYGON_MODE,NV097_SET_FRONT_POLYGON_MODE_V_FILL,NV097_SET_FRONT_POLYGON_MODE_V_FILL);
    pb_end(p);

    //the quad: texture coordinates (attribute 9) first, the position (attribute 0) launches the vertex
    p=pb_begin();
    p=pb_push1(p,NV097_SET_BEGIN_END,NV097_SET_BEGIN_END_OP_QUADS);
    for(i=0;i<4;i++)
    {
        p=pb_push4f(p,NV097_SET_VERTEX_DATA4F_M+9*16,corners[i][0]*tw,corners[i][1]*th,0.0f,1.0f);
        p=pb_push4f(p,NV097_SET_VERTEX_DATA4F_M+0*16,corners[i][0]*w,corners[i][1]*h,0.0f,1.0f);
    }
    p=pb_push1(p,NV097_SET_BEGIN_END,NV097_SET_BEGIN_END_OP_END);
    pb_end(p);

    //back to pbkit's defaults where pb_init sets one (texture stage 0 off, culling on);
    //transform program start 0. Everything else above stays as the resolve left it.
    p=pb_begin();
    p=pb_push1(p,NV097_SET_TEXTURE_CONTROL0,0);
    p=pb_push1(p,NV097_SET_CULL_FACE_ENABLE,1);
    p=pb_push1(p,NV097_SET_TRANSFORM_PROGRAM_START,0);
    pb_end(p);
}

//returns 1 if we have to retry later (means no free buffer, draw more details next time)
int pb_finished(void)
{
    uint32_t        *p;

    if (pb_BackBufferbReady[pb_BackBufferNxt]) return 1; //table is full, retry later

    //multisampling: resolve the sample buffer into the back buffer first
    if (pb_MSAddr) pb_resolve_multisample();

    //insert in push buffer the commands to trigger screen swapping at next VBlank
    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_ASK_FOR_IDLE,0); //ask for idle
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_NOP,0); //wait for idle
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0); //wait/makespace (obtains null status)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,pb_back_index); //set param=back buffer index to show up
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_FINISHED); //subprogID PB_FINISHED: gets frame ready to show up soon
//  p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STALL_PIPELINE,0); //stall gpu pipeline (not sure it's needed in triple buffering technic)
    pb_end(p);

    //insert in push buffer the commands to trigger selection of next back buffer
    //(because previous ones may not have finished yet, so need to use 0x0100 call)
    pb_FramesFinished++;
    pb_back_index=(pb_back_index+1)%pb_FrameBuffersCount;
    pb_target_back_buffer();

    return 0;
}




void pb_kill(void)
{
    void            *pSavedData;
    int         i;
    DWORD           old_caches,old_push,old_pull;
    DWORD           *p;

    DWORD           TimeStampTicks;

    int         counter;

    HalRegisterShutdownNotification(&pb_shutdown_registration, FALSE);

#ifdef DBG
//  debugPrint("Waiting until Dma is not busy\n");
#endif
    if (pb_Put)
    {
        pb_start();
        pb_wait_until_gr_not_busy();

        *(pb_Put)=(((DWORD)pb_Head)&0x0FFFFFFF)+1; //writes a jump to push buffer head
        pb_Put=pb_Head;
        pb_start();

        TimeStampTicks=KeTickCount;

        while(1)
        {
            if ((*(pb_DmaUserAddr+0x44/4))>0x04000000)
            {
                debugPrint("pb_kill: Bad get addr\n");
                break;
            }

            //did GetAddr reach push buffer head as planned?
            if (((*(pb_DmaUserAddr+0x44/4))&0x0FFFFFFF)==(((DWORD)pb_Head)&0x0FFFFFFF)) break;

            if (KeTickCount-TimeStampTicks>TICKSTIMEOUT)
            {
                debugPrint("pb_kill: Dma busy for too long\n");
                break;
            }
        }
    }
#ifdef DBG
//  if (KeTickCount-TimeStampTicks<=TICKSTIMEOUT) debugPrint("Dma not busy. All is ok.\n");
#endif

    //wait until screen swapping is finished (if one is on its way)
    while(pb_BackBufferbReady[pb_BackBufferNxt]);

    pb_running=0;
    pb_uninstall_gpu_interrupt();

    //stop the overlay now (its surface may be freed after pb_kill)
    pb_OverlayPending=2;
    pb_overlay_apply(pb_vbl_counter);
    KeRemoveQueueDpc(&pb_DPCObject);

    if (pb_ExtraBuffersCount) MmFreeContiguousMemory((PVOID)pb_EXAddr[0]);
    if (pb_DepthStencilAddr) MmFreeContiguousMemory((PVOID)pb_DepthStencilAddr);
    if (pb_FrameBuffersAddr) MmFreeContiguousMemory((PVOID)pb_FrameBuffersAddr);
    if (pb_MSAddr) MmFreeContiguousMemory((PVOID)pb_MSAddr);
    pb_MSAddr=0;
    pb_DepthStencilAddr=0;
    pb_FrameBuffersAddr=0;
    pb_DSAddr=0;

    pb_FenceMem=NULL;
    if (pb_DmaBuffer8) MmFreeContiguousMemory(pb_DmaBuffer8);
    if (pb_DmaBuffer2) MmFreeContiguousMemory(pb_DmaBuffer2);
    if (pb_DmaBuffer7) MmFreeContiguousMemory(pb_DmaBuffer7);

    if (pb_Head) MmFreeContiguousMemory(pb_Head);


    //eventually restore a previously saved video mode

    pSavedData=AvGetSavedDataAddress();
    if (pSavedData==0) AvSendTVEncoderOption((PVOID)VIDEO_BASE,VIDEO_ENC_VIDEOENABLE,1,NULL);


    //restore system completely

    for(i=0;i<8;i++) pb_release_tile(i,1);

    VIDEOREG(NV_PFIFO_DMA_TIMESLICE)=NV_PFIFO_DMA_TIMESLICE_ALL_DISABLE;

    while ( ((VIDEOREG8(NV_PFIFO_CACHE1_STATUS)&NV_PFIFO_CACHE1_STATUS_LOW_MARK_EMPTY)==0)||
        ((VIDEOREG8(NV_PFIFO_RUNOUT_STATUS)&NV_PFIFO_RUNOUT_STATUS_LOW_MARK_EMPTY)==0)||
        ((VIDEOREG8(NV_PFIFO_CACHE1_DMA_PUSH)&NV_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY)!=0)  )
    {
        pb_fifo_handler();
        if (VIDEOREG(NV_PGRAPH_INTR)!=NV_PGRAPH_INTR_NOT_PENDING) pb_gr_handler();
        if (VIDEOREG(NV_PMC_INTR_0)&NV_PMC_INTR_0_PCRTC_PENDING) pb_vbl_handler();
    }

    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=NV_PFIFO_CACHE1_DMA_PUSH_ACCESS_DISABLE;
    while((VIDEOREG8(NV_PFIFO_CACHE1_DMA_PUSH)&NV_PFIFO_CACHE1_DMA_PUSH_STATE_BUSY)!=0);

    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE0_PUSH0)=NV_PFIFO_CACHE0_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE0_PULL0)=NV_PFIFO_CACHE0_PULL0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;

    pb_set_fifo_channel(1);

    VIDEOREG(NV_PFIFO_CACHE1_PUT)=0;
    VIDEOREG(NV_PFIFO_CACHE1_GET)=0;

    old_caches=VIDEOREG(NV_PFIFO_CACHES);
    old_push=VIDEOREG(NV_PFIFO_CACHE1_PUSH0);
    old_pull=VIDEOREG(NV_PFIFO_CACHE1_PULL0);
    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;

    //Neutralize DMA (for channels 0 and 1)
    for(i=0;i<2;i++)
    {
        if (pb_FifoChannelsReady) //any active channel?
        {
            p=(DWORD *)(VIDEO_BASE+pb_FifoFCAddr+i*64);
            *(p+1)=*(p+0);  //DMA_GET=DMA_PUT
            *(p+4)=0;   //DMA_STATE=0
        }
    }

    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=old_pull;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=old_push;
    VIDEOREG(NV_PFIFO_CACHES)=old_caches;

    VIDEOREG(NV_PFIFO_DMA)=NV_PFIFO_DMA_NOT_PENDING;
    VIDEOREG(NV_PFIFO_INTR_EN_0)=NV_PFIFO_INTR_EN_0_ALL_DISABLE;

    pb_load_gr_ctx(NONE);

    //restore most essential outer registers
    VIDEOREG(NV_PFB_CFG0)=pb_OldFBConfig0;
    VIDEOREG(NV_PFB_CFG1)=pb_OldFBConfig1;
    VIDEOREG(NV_PMC_ENABLE)=pb_OldMCEnable;
    VIDEOREG(NV_PMC_INTR_EN_0)=pb_OldMCInterrupt;
    //a persisted display (XVideoPersistDisplay) stays on screen across XLaunchXBE
    if (pSavedData) VIDEOREG(PCRTC_START)=(DWORD)MmGetPhysicalAddress(pSavedData)&0x03FFFFFF;
    //not back to a debug screen that was given back: the front buffer stays allocated and shown
    else if (!pb_DebugScreenGone) VIDEOREG(PCRTC_START)=pb_OldVideoStart;

    NtClose(pb_VBlankEvent);
}


void pb_set_color_format(unsigned int fmt, bool swizzled)
{
    pb_ColorFmt = fmt;
    assert(swizzled == false);
}

void pb_set_depth_format(unsigned int fmt)
{
    assert(pb_running == 0); //the depth stencil buffer is allocated by pb_init
    assert((fmt == PB_DEPTH_NONE) || (fmt == NV097_SET_SURFACE_FORMAT_ZETA_Z16) || (fmt == NV097_SET_SURFACE_FORMAT_ZETA_Z24S8));
    pb_DepthFmt = fmt;
}

unsigned int pb_get_depth_format(void)
{
    return pb_DepthFmt;
}

//Recomputes pb_ZScale and, when the Dma engine is running, re-pushes everything that
//depends on it or on NV097_SET_CONTROL0 (viewport z range, depth clip range, CONTROL0).
static void pb_update_depth_state(void)
{
    uint32_t    *p;

    pb_ZScale = pb_depth_scale_for(pb_DepthFmt, pb_DepthFloat);

    if (pb_running == 0) return;

    p = pb_begin();
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_W_YUV_FPZ_FLAGS, pb_control0());
    pb_end(p);
    pb_set_viewport(pb_Viewport_x, pb_Viewport_y, pb_Viewport_width, pb_Viewport_height, pb_Viewport_zminf, pb_Viewport_zmaxf);
}

void pb_set_depth_float(bool enable)
{
    pb_DepthFloat = enable ? 1 : 0;
    pb_update_depth_state();
}

bool pb_get_depth_float(void)
{
    return pb_DepthFloat != 0;
}

void pb_set_w_buffer(bool enable)
{
    pb_WBuffer = enable ? 1 : 0;
    pb_update_depth_state();
}

bool pb_get_w_buffer(void)
{
    return pb_WBuffer != 0;
}

float pb_depth_scale(void)
{
    return pb_ZScale;
}

//Dither defaults to on with a 16 bits color format and off with a 32 bits one.
void pb_set_dither(bool enable)
{
    uint32_t    *p;

    pb_Dither = enable ? 1 : 0;

    if (pb_running == 0) return;

    p = pb_begin();
    p = pb_push1(p, NV20_TCL_PRIMITIVE_3D_DITHER_ENABLE, pb_Dither);
    pb_end(p);
}

bool pb_get_dither(void)
{
    if (pb_Dither < 0) return pb_ColorBpp == 16;
    return pb_Dither != 0;
}

DWORD *pb_depth_stencil_buffer(void)
{
    return (DWORD *)pb_DSAddr;
}

DWORD pb_depth_stencil_pitch(void)
{
    return pb_DepthStencilPitch;
}

bool pb_frame_buffers_tiled(void)
{
    return pb_FBTiled != 0;
}

bool pb_depth_stencil_tiled(void)
{
    return pb_DSTiled != 0;
}

/**
 * Helper function to calculate numerator and denominator for PTIMER
 *
 * It takes nvclk as input and scales the 31.25MHz/nvclk fraction down into
 * the 16 bit value range of PTIMER parameters. It's not a perfect approach,
 * but more accurate than the hardcoded values of the XDK, while properly
 * scaling with overclocking.
 *
 * PTIMER is just a wall clock used for timestamps anyway.
 */
static inline void pb_calc_ptimer_param (uint32_t nvclk, uint32_t *denominator, uint32_t *numerator)
{
    uint32_t num = nvclk;
    uint32_t den = 31250000;

    const uint32_t max_val = 1 << 16;
    const uint32_t scale_num = (num + max_val - 1) / max_val;
    const uint32_t scale_den = (den + max_val - 1) / max_val;
    const uint32_t scale = (scale_num > scale_den) ? scale_num : scale_den;

    if (scale > 1) {
        num = (num + scale - 1) / scale;
        den = (den + scale - 1) / scale;
    }

    *numerator = num;
    *denominator = den;
}

int pb_init(void)
{
    DWORD           old;

    BYTE            old_color_31;
    BYTE            old_color_82;

    uint32_t        baseaddr,baseaddr2;

    int         i;

    uint32_t        *p;

    struct s_CtxDma sDmaObject2;
    struct s_CtxDma sDmaObject3;
    struct s_CtxDma sDmaObject4;
    struct s_CtxDma sDmaObject5;
    struct s_CtxDma sDmaObject6;
    struct s_CtxDma sDmaObject7;
    struct s_CtxDma sDmaObject8;
    struct s_CtxDma sDmaObject9;
    struct s_CtxDma sDmaObject10;
    struct s_CtxDma sDmaObject11;
    struct s_CtxDma sDmaObject12;

    struct s_CtxDma sGrObject13;
    struct s_CtxDma sGrObject14;
    struct s_CtxDma sGrObject16;
    struct s_CtxDma sGrObject17;

    DWORD           UserAddr;

    DWORD           TimeStamp1;
    DWORD           TimeStamp2;
    DWORD           GetAddr;
    DWORD           PutAddr;
                        //Dma channel properties
    int             dma_trig=128;   //min 8     max 256
    int         dma_size=128;   //min 32    max 256
    int         dma_max_reqs=8; //min 0     max 15

    DWORD           dummy;

    DWORD           channel;

    DWORD           *pGrCtxTable;

    VIDEO_MODE      vm;

    DWORD           format;

    DWORD           BackBufferCount;
    DWORD           BackBufferFormat;
    DWORD           DepthStencilFormat;

    DWORD           Width;
    DWORD           Height;

    DWORD           FrameBufferCount;

    DWORD           HScale;
    DWORD           VScale;

    DWORD           HSize;
    DWORD           VSize;

    DWORD           Pitch;
    DWORD           TilePitch;

    DWORD           Addr;
    DWORD           Size;

    DWORD           FBAddr;
    DWORD           FBSize;

    DWORD           DSAddr;
    DWORD           DSSize;

    DWORD           EXAddr;
    DWORD           EXSize;

    int         n;

    DWORD           value;

    if (pb_running) return -8;

    //reset global vars (except pb_Size)

    pb_3DGrCtxInst[0]=0;
    pb_3DGrCtxInst[1]=0;

    pb_FifoChannelsReady=0;
    pb_FifoChannelsMode=NV_PFIFO_MODE_ALL_PIO;
    pb_FifoChannelID=0;

    PB_GAMMA_RAMP gammaRamp;
    for (i = 0; i < 256; i++) {
        gammaRamp.red[i] = i;
        gammaRamp.green[i] = i;
        gammaRamp.blue[i] = i;
    }
    pb_set_gamma_ramp(&gammaRamp);

    pb_BackBufferNxt=0;
    for(i=0;i<5;i++) pb_BackBufferbReady[i]=0;

    pb_Put=NULL;

    pb_PutRunSize=0;

    pb_FrameBuffersAddr=0;


    //DMA context 8 is the 3D semaphore context (NV097_SET_CONTEXT_DMA_SEMAPHORE): the GPU writes
    //fence values at offset 0. One uncached page: the CPU polls what the GPU writes.
    pb_DmaBuffer8 = MmAllocateContiguousMemoryEx(PAGE_SIZE, 0, MAXRAM, PAGE_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    pb_DmaBuffer2 = MmAllocateContiguousMemoryEx(32, 0, MAXRAM, 0, PAGE_READWRITE);
    pb_DmaBuffer7 = MmAllocateContiguousMemoryEx(32, 0, MAXRAM, 0, PAGE_READWRITE);
    if ((pb_DmaBuffer8==NULL)||(pb_DmaBuffer2==NULL)||(pb_DmaBuffer7==NULL)) return -2;
    memset(pb_DmaBuffer8,0,32);
    pb_FenceLast=PB_FENCE_START;
    pb_DmaBuffer8[0]=PB_FENCE_START;    //no fence pending
    pb_FenceMem=(volatile DWORD *)pb_DmaBuffer8;
    pb_VBLFlippedSlot=-1;
    pb_LastFlipVBL=pb_vbl_counter;
    pb_FieldPhase=-2;   //video mode may have changed: query it again
    memset(pb_DmaBuffer2,0,32);
    memset(pb_DmaBuffer7,0,32);

    pb_Head = MmAllocateContiguousMemoryEx(pb_Size+8*1024, 0, MAXRAM, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (pb_Head==NULL) return -3;

    memset(pb_Head,0,pb_Size+8*1024);

    pb_Tail=pb_Head+pb_Size/4;

    pb_Put=pb_Head;

    pb_BackBufferNxt=0;     //increments when we finish drawing a frame
    pb_BackBufferbReady[0]=0;
    pb_BackBufferbReady[1]=0;
    pb_BackBufferbReady[2]=0;

    pb_BackBufferNxtVBL=0;      //increments when VBlank event fires

    //initialize push buffer DMA engine
    //DMA=Direct Memory Access (means CPU is not involved in the data transfert)

    NtCreateEvent(&pb_VBlankEvent, NULL, NotificationEvent, FALSE);

    VIDEOREG(NV_PBUS_PCI_NV_1)|=NV_PBUS_PCI_NV_1_BUS_MASTER_ENABLED;
    VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_DISABLED;
    VIDEOREG(NV_PTIMER_INTR_EN_0)=NV_PTIMER_INTR_EN_0_ALARM_DISABLED;

    if (pb_install_gpu_interrupt()==0)
    {
        pb_FenceMem=NULL;
        if (pb_DmaBuffer8) MmFreeContiguousMemory(pb_DmaBuffer8);
        if (pb_DmaBuffer2) MmFreeContiguousMemory(pb_DmaBuffer2);
        if (pb_DmaBuffer7) MmFreeContiguousMemory(pb_DmaBuffer7);
        if (pb_Head) MmFreeContiguousMemory(pb_Head);
        NtClose(pb_VBlankEvent);
        return -4; //OpenXDK probably hooked IRQ3 already
    }

    //backup of the most essential outer registers (pb_kill will restore them)
    pb_OldMCEnable=VIDEOREG(NV_PMC_ENABLE);
    pb_OldMCInterrupt=VIDEOREG(NV_PMC_INTR_EN_0);
    pb_OldFBConfig0=VIDEOREG(NV_PFB_CFG0);
    pb_OldFBConfig1=VIDEOREG(NV_PFB_CFG1);
    pb_OldVideoStart=((DWORD)XVideoGetFB())&0x03FFFFFF;

    VIDEOREG(NV_PBUS_PCI_NV_12)=NV_PBUS_PCI_NV_12_ROM_DECODE_DISABLED;
    VIDEOREG(NV_PBUS_PCI_NV_3)=NV_PBUS_PCI_NV_3_LATENCY_TIMER_248_CLOCKS;

    VIDEOREG(NV_PMC_ENABLE)=NV_PMC_ENABLE_ALL_ENABLE;
    VIDEOREG(NV_PMC_INTR_EN_0)=NV_PMC_INTR_EN_0_INTA_HARDWARE;

    // Configure PTIMER to 31.25MHz
    {
        uint32_t mdiv = (VIDEOREG(NV_PRAMDAC_NVPLL_COEFF) & NV_PRAMDAC_NVPLL_COEFF_MDIV);
        uint32_t ndiv = (VIDEOREG(NV_PRAMDAC_NVPLL_COEFF) & NV_PRAMDAC_NVPLL_COEFF_NDIV) >> 8;
        uint32_t pdiv = (VIDEOREG(NV_PRAMDAC_NVPLL_COEFF) & NV_PRAMDAC_NVPLL_COEFF_PDIV) >> 16;

        if (!mdiv) {
            pb_kill();
            return -5; //invalid GPU internal PLL (Phase Locked Loop=GPU freq generator)
        }

        uint32_t num, den;
        pb_calc_ptimer_param(((DW_XTAL_16MHZ * ndiv) / (1 << pdiv)) / mdiv, &den, &num);
        VIDEOREG(NV_PTIMER_NUMERATOR) = num - 1;
        VIDEOREG(NV_PTIMER_DENOMINATOR) = den - 1;
        VIDEOREG(NV_PTIMER_ALARM_0) = 0xFFFFFFFF;
    }


    //The Gpu instance memory is a special place in PRAMIN area (VRAM attached to RAM?)
    //Essential Gpu data will be stored there, for, I guess, top speed access.

    if ((VIDEOREG(NV_PFB_CFG0)&NV_PFB_CFG0_PART_3)!=3)
    {
        pb_kill();
        return -6;
    }

    pb_GpuInstMem=(DWORD)MmClaimGpuInstanceMemory(0xFFFFFFFF,&baseaddr);
        //returns 0x83FF0000            //0x10000
        //physical_memory(0x83FF0000)=0x03FF0000

    if (pb_GpuInstMem==0)
    {
        pb_kill();
        return -7;
    }

    pb_GpuInstMem-=INSTANCE_MEM_MAXSIZE;    //-0x5000=-20480=-20Kb
        // =0x83FEB000

    //a hash table
    pb_FifoHTAddr=baseaddr+NV_PRAMIN;   //0x10000+NV_PRAMIN(0x700000)

    VIDEOREG(NV_PFIFO_RAMHT)=((baseaddr>>8)&NV_PFIFO_RAMHT_BASE_ADDRESS)|NV_PFIFO_RAMHT_SEARCH_128;
            //      =NV_PFIFO_RAMHT_BASE_ADDRESS_10000

    //FC (size 0x80)
    pb_FifoFCAddr=baseaddr+NV_PRAMIN+0x1000;//=0x11000+NV_PRAMIN

    //U1 (size 0x20) Unknown1
    pb_FifoU1Addr=baseaddr+NV_PRAMIN+0x1080;//=0x11080+NV_PRAMIN

    //FC (dwFifoFCAddr, but 128 bytes aligned, with flag 0x200)
    baseaddr2=((pb_FifoFCAddr+0x80)&0x1FC00)|0x200; //0x11200

    VIDEOREG(NV_PFIFO_RAMFC)=baseaddr2<<7|((pb_FifoFCAddr>>8)&NV_PFIFO_RAMFC_BASE_ADDRESS);
                    //   |NV_PFIFO_RAMFC_BASE_ADDRESS_11000
    //=0x00890110 (theoretical value)
    //=0x008A0110 (current value read under openxdk : |0x400 instead of |0x200)

    pb_FreeInst=(pb_FifoU1Addr-NV_PRAMIN+0x20)>>4;
            //     =0x110A (unit=16 bytes block)

    VIDEOREG(NV_PFB_NVM)=VIDEOREG(NV_PFB_NVM)&NV_PFB_NVM_MODE_DISABLE;

    //zeroes whole GPU instance memory
    for(i=0;i<INSTANCE_MEM_MAXSIZE;i+=4) VIDEOREG(NV_PRAMIN+baseaddr+i)=0;

    //reserve 8 blocks (128 bytes) for GrCtxTable
    //(2 first dwords will point at the 2 graphic contexts for the 2 channels)
    pb_GrCtxTableInst=pb_FreeInst; pb_FreeInst+=8;






    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=31; old_color_31=VIDEOREG8(NV_PRMCIO_CR__COLOR);
    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=31; VIDEOREG8(NV_PRMCIO_CR__COLOR)=87;

    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=82; old_color_82=VIDEOREG8(NV_PRMCIO_CR__COLOR);
    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=82; VIDEOREG8(NV_PRMCIO_CR__COLOR)=old_color_82+4;

    VIDEOREG(NV_PVIDEO_DEBUG_2)=(VIDEOREG(NV_PVIDEO_DEBUG_2)&NV_PVIDEO_DEBUG_2_BURST1_CLEAR)|NV_PVIDEO_DEBUG_2_BURST1_INIT;
    VIDEOREG(NV_PVIDEO_DEBUG_2)=(VIDEOREG(NV_PVIDEO_DEBUG_2)&NV_PVIDEO_DEBUG_2_BURST2_CLEAR)|NV_PVIDEO_DEBUG_2_BURST2_INIT;

    VIDEOREG(NV_PVIDEO_DEBUG_3)=(VIDEOREG(NV_PVIDEO_DEBUG_3)&NV_PVIDEO_DEBUG_3_WATER_MARK1_CLEAR)|NV_PVIDEO_DEBUG_3_WATER_MARK1_INIT;
    VIDEOREG(NV_PVIDEO_DEBUG_3)=(VIDEOREG(NV_PVIDEO_DEBUG_3)&NV_PVIDEO_DEBUG_3_WATER_MARK2_CLEAR)|NV_PVIDEO_DEBUG_3_WATER_MARK2_INIT;

    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=32; VIDEOREG8(NV_PRMCIO_CR__COLOR)=41;
    VIDEOREG8(NV_PRMCIO_CRX__COLOR)=27; VIDEOREG8(NV_PRMCIO_CR__COLOR)=5;

    if (old_color_31==0)
    {
        VIDEOREG8(NV_PRMCIO_CRX__COLOR)=31; VIDEOREG8(NV_PRMCIO_CR__COLOR)=153;
    }

    VIDEOREG(NV_PCRTC_CONFIG)=(VIDEOREG(NV_PCRTC_CONFIG)&~NV_PCRTC_CONFIG_START_ADDRESS)|NV_PCRTC_CONFIG_START_ADDRESS_HSYNC;
    //3 replaced with 2=(3&~7)|2




    VIDEOREG(NV_PVIDEO_LUMINANCE_0)=NV_PVIDEO_LUMINANCE_CONTRAST_UNITY|NV_PVIDEO_LUMINANCE_BRIGHTNESS_UNITY;
    VIDEOREG(NV_PVIDEO_LUMINANCE_1)=NV_PVIDEO_LUMINANCE_CONTRAST_UNITY|NV_PVIDEO_LUMINANCE_BRIGHTNESS_UNITY;

    VIDEOREG(NV_PVIDEO_CHROMINANCE_0)=NV_PVIDEO_CHROMINANCE_SAT_COS_UNITY|NV_PVIDEO_CHROMINANCE_SAT_SIN_UNITY;
    VIDEOREG(NV_PVIDEO_CHROMINANCE_1)=NV_PVIDEO_CHROMINANCE_SAT_COS_UNITY|NV_PVIDEO_CHROMINANCE_SAT_SIN_UNITY;

//maybe let's preserve previous setting
//VIDEOREG(NV_PVIDEO_OFFSET_0)=NV_PVIDEO_OFFSET_VALUE_ZERO;
//VIDEOREG(NV_PVIDEO_OFFSET_1)=NV_PVIDEO_OFFSET_VALUE_ZERO;

    VIDEOREG(NV_PVIDEO_SIZE_IN_0)=NV_PVIDEO_SIZE_IN_UNKNOWN_WIDTH|NV_PVIDEO_SIZE_IN_UNKNOWN_HEIGHT;
    VIDEOREG(NV_PVIDEO_SIZE_IN_1)=NV_PVIDEO_SIZE_IN_UNKNOWN_WIDTH|NV_PVIDEO_SIZE_IN_UNKNOWN_HEIGHT;

    VIDEOREG(NV_PVIDEO_POINT_IN_0)=NV_PVIDEO_POINT_IN_S_ORIGIN|NV_PVIDEO_POINT_IN_T_ORIGIN;
    VIDEOREG(NV_PVIDEO_POINT_IN_1)=NV_PVIDEO_POINT_IN_S_ORIGIN|NV_PVIDEO_POINT_IN_T_ORIGIN;

    VIDEOREG(NV_PVIDEO_DS_DX_0)=NV_PVIDEO_DS_DX_RATIO_UNITY;
    VIDEOREG(NV_PVIDEO_DS_DX_1)=NV_PVIDEO_DS_DX_RATIO_UNITY;

    VIDEOREG(NV_PVIDEO_DT_DY_0)=NV_PVIDEO_DT_DY_RATIO_UNITY;
    VIDEOREG(NV_PVIDEO_DT_DY_1)=NV_PVIDEO_DT_DY_RATIO_UNITY;


    pb_GrCtxID=NONE;



    VIDEOREG(NV_PGRAPH_CHANNEL_CTX_TABLE)=pb_GrCtxTableInst&NV_PGRAPH_CHANNEL_CTX_TABLE_INST;

    p=(uint32_t *)(VIDEO_BASE+NV_PRAMIN+(pb_GrCtxTableInst<<4));
    *(p+0)=0; //we don't point at the 2 graphic contexts yet
    *(p+1)=0;




    VIDEOREG(NV_PFIFO_CACHE1_PUT)=0;
    VIDEOREG(NV_PFIFO_CACHE1_GET)=0;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT)=0;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_GET)=0;
    VIDEOREG(NV_PFIFO_CACHE0_HASH)=0;
    VIDEOREG(NV_PFIFO_CACHE1_HASH)=0;
    VIDEOREG(NV_PFIFO_MODE)=NV_PFIFO_MODE_ALL_PIO;
    VIDEOREG(NV_PFIFO_DMA)=NV_PFIFO_DMA_NOT_PENDING;
    VIDEOREG(NV_PFIFO_SIZE)=0;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_STATE)=0;
    VIDEOREG(NV_PFIFO_RUNOUT_PUT)=0;
    VIDEOREG(NV_PFIFO_RUNOUT_GET)=0;


    pb_running=1;


    old=VIDEOREG(NV_PBUS_PCI_NV_19);
    VIDEOREG(NV_PBUS_PCI_NV_19)=old&NV_PBUS_PCI_NV_19_AGP_COMMAND_SBA_ENABLE_OFF&NV_PBUS_PCI_NV_19_AGP_COMMAND_AGP_ENABLE_OFF;
    VIDEOREG(NV_PBUS_PCI_NV_19)=old;

    VIDEOREG(PCRTC_INTR)=PCRTC_INTR_VBLANK_RESET;
    VIDEOREG(PCRTC_INTR_EN)=PCRTC_INTR_EN_VBLANK_ENABLED;

//VIDEOREG(NV_PTIMER_TIME_0)=0;
//VIDEOREG(NV_PTIMER_TIME_1)=ticks; //time & date in ticks (nasty calculation, let's skip it for now)

    VIDEOREG(NV_PGRAPH_FIFO)=NV_PGRAPH_FIFO_ACCESS_DISABLE;



    VIDEOREG(NV_PMC_ENABLE)=VIDEOREG(NV_PMC_ENABLE)&NV_PMC_ENABLE_PGRAPH_DISABLED;
    VIDEOREG(NV_PMC_ENABLE)=VIDEOREG(NV_PMC_ENABLE)|NV_PMC_ENABLE_PGRAPH_ENABLED;

    VIDEOREG(NV_PGRAPH_DEBUG_0) =   NV_PGRAPH_DEBUG_0_NO_RESET;

    VIDEOREG(NV_PGRAPH_DEBUG_1) =   NV_PGRAPH_DEBUG_1_VTX_PTE_ENABLED|
                    NV_PGRAPH_DEBUG_1_VTX_CACHE_ENABLED|
                    NV_PGRAPH_DEBUG_1_VTX_FILE_ENABLED|
                    NV_PGRAPH_DEBUG_1_DRAWDIR_Y_INCR|
                    NV_PGRAPH_DEBUG_1_INSTANCE_ENABLED|
                    NV_PGRAPH_DEBUG_1_CTX_ENABLED;

    VIDEOREG(NV_PGRAPH_DEBUG_7) =   NV_PGRAPH_DEBUG_7_UNKNOWN_OPTIONS;

    VIDEOREG(NV_PGRAPH_DEBUG_3) =   NV_PGRAPH_DEBUG_3_FLUSHING_ENABLED|
                    NV_PGRAPH_DEBUG_3_SYNC_TO_CRTC_ENABLED|
                    NV_PGRAPH_DEBUG_3_FAST_DATA_STRTCH_ENABLED|
                    NV_PGRAPH_DEBUG_3_FAST_3D_SHADOW_DATA_ENABLED|
                    NV_PGRAPH_DEBUG_3_FAST_DMA_READ_ENABLED|
                    NV_PGRAPH_DEBUG_3_IDLE_FILTER_ENABLED|
                    NV_PGRAPH_DEBUG_3_SINGLE_CYCLE_LOAD_ENABLED|
                    NV_PGRAPH_DEBUG_3_BILINEAR_3D_ENABLED|
                    NV_PGRAPH_DEBUG_3_VOLATILE_RESET_ENABLED|
                    NV_PGRAPH_DEBUG_3_DATA_CHECK_ENABLED|
                    NV_PGRAPH_DEBUG_3_FORMAT_CHECK_ENABLED|
                    NV_PGRAPH_DEBUG_3_DMA_CHECK_ENABLED|
                    NV_PGRAPH_DEBUG_3_STATE_CHECK_ENABLED|
                    NV_PGRAPH_DEBUG_3_IMAGE_64BIT_ENABLED|
                    NV_PGRAPH_DEBUG_3_XFMODE_COALESCE_ENABLED|
                    NV_PGRAPH_DEBUG_3_CTX_METHODS_ENABLED|
                    NV_PGRAPH_DEBUG_3_OP_METHODS_ENABLED|
                    NV_PGRAPH_DEBUG_3_IGNORE_PATCHVALID_ENABLED;

    VIDEOREG(NV_PGRAPH_DEBUG_4) = NV_PGRAPH_DEBUG_4_ALL_DISABLE;

    VIDEOREG(NV_PGRAPH_DEBUG_5) = NV_PGRAPH_DEBUG_5_ZCULL_SPARE2_ENABLED;

    if (VIDEOREG(NV_PBUS_ROM_VERSION)&NV_PBUS_ROM_VERSION_MASK)
        VIDEOREG(NV_PGRAPH_UNKNOWN_400B80)=0x45EAD10F;
    else
        VIDEOREG(NV_PGRAPH_UNKNOWN_400B80)=0x45EAD10E;
    VIDEOREG(NV_PGRAPH_UNKNOWN_400B84)=0;
    VIDEOREG(NV_PGRAPH_UNKNOWN_400B88)=0;

    VIDEOREG(NV_PGRAPH_UNKNOWN_400098)=0x78;
    VIDEOREG(NV_PGRAPH_UNKNOWN_40009C)=0x40;

    VIDEOREG(NV_PGRAPH_CHANNEL_CTX_TABLE)=pb_GrCtxTableInst&NV_PGRAPH_CHANNEL_CTX_TABLE_INST;

    pb_wait_until_gr_not_busy();

    pb_prepare_tiles();

    VIDEOREG(NV_PGRAPH_CTX_SWITCH1)=NV_PGRAPH_CTX_SWITCH1_ALL_DISABLE;
    VIDEOREG(NV_PGRAPH_CTX_SWITCH2)=NV_PGRAPH_CTX_SWITCH2_ALL_DISABLE;
    VIDEOREG(NV_PGRAPH_CTX_SWITCH3)=NV_PGRAPH_CTX_SWITCH3_ALL_DISABLE;
    VIDEOREG(NV_PGRAPH_CTX_SWITCH4)=NV_PGRAPH_CTX_SWITCH4_ALL_DISABLE;

    VIDEOREG(NV_PGRAPH_CTX_CONTROL)=NV_PGRAPH_CTX_CONTROL_DEVICE_ENABLED;

    VIDEOREG(NV_PGRAPH_FFINTFC_ST2)=NV_PGRAPH_FFINTFC_ST2_CHID_STATUS_VALID;

    pb_load_gr_ctx(pb_GrCtxID);


    VIDEOREG(NV_PGRAPH_INTR)=NV_PGRAPH_INTR_ALL_ENABLE;
    VIDEOREG(NV_PGRAPH_INTR_EN)=NV_PGRAPH_INTR_EN_ALL_ENABLE;


    VIDEOREG(NV_PFIFO_CACHE1_DMA_FETCH) =   NV_PFIFO_CACHE1_DMA_FETCH_TRIG_128_BYTES|
                        NV_PFIFO_CACHE1_DMA_FETCH_SIZE_32_BYTES|
                        NV_PFIFO_CACHE1_DMA_FETCH_MAX_REQS_15;

    VIDEOREG(NV_PFIFO_DMA_TIMESLICE) =  NV_PFIFO_DMA_TIMESLICE_SELECT_128K|
                        NV_PFIFO_DMA_TIMESLICE_TIMEOUT_ENABLED;

    VIDEOREG(NV_PFIFO_DELAY_0)=255&NV_PFIFO_DELAY_0_WAIT_RETRY;
    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE0_PUSH0)=NV_PFIFO_CACHE0_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE0_PULL0)=NV_PFIFO_CACHE0_PULL0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=NV_PFIFO_CACHE1_DMA_PUSH_ACCESS_DISABLE;

    pb_set_fifo_channel(1);

    VIDEOREG(NV_PFIFO_CACHE1_PUT)=0;    //&NV_PFIFO_CACHE1_PUT_ADDRESS
    VIDEOREG(NV_PFIFO_CACHE1_GET)=0;    //&NV_PFIFO_CACHE1_GET_ADDRESS

    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_ENABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_ENABLE;

    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_REASSIGN_ENABLED;
    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;


    VIDEOREG(NV_PFIFO_INTR_0)=NV_PFIFO_INTR_0_ALL_RESET;
    VIDEOREG(NV_PFIFO_INTR_EN_0)=NV_PFIFO_INTR_EN_0_ALL_ENABLE;;


    //calculate number of CPU cycles per second
    HalReadWritePCISpace(0,0x60,0x6C,&value,4,FALSE);
        //BusNumber,SlotNumber,RegisterNumber,pBuffer,Length,bWritePCISpace
    if (value&0xFF)
        pb_CpuFrequency=5.5f*((float)((value>>8)&0xFF))*(XTAL_16MHZ/((float)(value&0xFF)));
    else
        pb_CpuFrequency=733.33f; //Mhz, theoretically


    pb_create_dma_ctx(3,DMA_CLASS_3D,0,MAXRAM,&sDmaObject3);
    pb_create_dma_ctx(5,DMA_CLASS_2,0,MAXRAM,&sDmaObject5);
    pb_create_dma_ctx(4,DMA_CLASS_3,0,MAXRAM,&sDmaObject4);

    pb_create_dma_ctx(9,DMA_CLASS_3D,0,MAXRAM,&sDmaObject9);
    pb_create_dma_ctx(10,DMA_CLASS_3D,0,MAXRAM,&sDmaObject10);
    pb_create_dma_ctx(11,DMA_CLASS_3D,0,MAXRAM,&sDmaObject11);
    pb_DmaChID9Inst=sDmaObject9.Inst;
    pb_DmaChID10Inst=sDmaObject10.Inst;
    pb_DmaChID11Inst=sDmaObject11.Inst;

    pb_create_dma_ctx(2,DMA_CLASS_3,(DWORD)pb_DmaBuffer2,0x1F,&sDmaObject2);
    pb_create_dma_ctx(7,DMA_CLASS_3D,(DWORD)pb_DmaBuffer7,0x1F,&sDmaObject7);
    //this one is damn important. memory address 0x80000000 acts as a trigger.
    pb_create_dma_ctx(12,DMA_CLASS_3D,0x80000000,0x10000000,&sDmaObject12);
    pb_create_dma_ctx(8,DMA_CLASS_3D,(DWORD)pb_DmaBuffer8,0x20,&sDmaObject8);
    pb_create_dma_ctx(6,DMA_CLASS_2,0,MAXRAM,&sDmaObject6);

    //we initialized channel 0 first, that will match graphic context 0
    pb_FifoChannelID=0;
    pb_FifoChannelsMode=NV_PFIFO_MODE_ALL_PIO;

    pb_FifoBigInst=pb_FreeInst; pb_FreeInst+=0x37F; //895 blocks=14320 bytes=0x37F0 bytes

    dummy=VIDEOREG(NV_PFIFO_CACHES);

    channel=pb_FifoChannelID;

    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_ALL_DISABLE;

    //zeroes 0x37F0 bytes (0xDFC/4=0x37F blocks, 4 dwords in 1 block)
    for(i=0;i<0xDFC;i++) VIDEOREG(NV_PRAMIN+(pb_FifoBigInst<<4)+i*4)=0;

    //here we go, we initialize first graphic context pointer
    pGrCtxTable=(DWORD *)(VIDEO_BASE+NV_PRAMIN+(pb_GrCtxTableInst<<4));
    *(pGrCtxTable+channel)=pb_FifoBigInst;
    pb_GrCtxInst[channel]=pb_FifoBigInst;

    //points at channel details in PRAMIN area
    p=(uint32_t *)(VIDEO_BASE+pb_FifoFCAddr+channel*64);

    //zeroes details
    for(i=0;i<16;i++) *(p+i)=0;

    //set dma instance, future value for VIDEOREG(NV_PFIFO_CACHE1_DMA_INSTANCE)
    *(p+3)=sDmaObject6.Inst;

    //encode trig & size
    dma_trig=(dma_trig>>3)-1;
    dma_size=(dma_size>>5)-1;

    //set dma fetch, future value for VIDEOREG(NV_PFIFO_CACHE1_DMA_FETCH)
    *(p+5)= ((dma_trig<<3)&NV_PFIFO_CACHE1_DMA_FETCH_TRIG)|
        ((dma_size<<13)&NV_PFIFO_CACHE1_DMA_FETCH_SIZE)|
        ((dma_max_reqs<<16)&NV_PFIFO_CACHE1_DMA_FETCH_MAX_REQS);

    pb_FifoChannelsMode|=(1<<channel);
    VIDEOREG(NV_PFIFO_MODE)=pb_FifoChannelsMode;

    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_DISABLE;

    VIDEOREG(NV_PFIFO_CACHE1_PUSH1)=channel&NV_PFIFO_CACHE1_PUSH1_CHID;

    if (pb_FifoChannelsMode&(1<<channel)) VIDEOREG(NV_PFIFO_CACHE1_PUSH1)|=NV_PFIFO_CACHE1_PUSH1_MODE_DMA;

    VIDEOREG(NV_PFIFO_CACHE1_DMA_PUT)=0;    //&NV_PFIFO_CACHE1_DMA_PUT_OFFSET
    VIDEOREG(NV_PFIFO_CACHE1_DMA_GET)=0;    //&NV_PFIFO_CACHE1_DMA_GET_OFFSET
    VIDEOREG(NV_PFIFO_CACHE1_DMA_INSTANCE)=*(p+3);
    VIDEOREG(NV_PFIFO_CACHE1_DMA_CTL)=NV_PFIFO_CACHE1_DMA_CTL_ALL_DISABLE;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_STATE)=NV_PFIFO_CACHE1_DMA_STATE_METHOD_COUNT_0;
    VIDEOREG(NV_PFIFO_CACHE1_ENGINE)=NV_PFIFO_CACHE1_ENGINE_ALL_SW;
    VIDEOREG(NV_PFIFO_CACHE1_DMA_FETCH)=*(p+5);

    if (pb_FifoChannelsMode&(1<<channel)) VIDEOREG(NV_PFIFO_CACHE1_DMA_PUSH)=NV_PFIFO_CACHE1_DMA_PUSH_ACCESS_ENABLE;

    VIDEOREG(NV_PFIFO_CACHE1_PULL0)=NV_PFIFO_CACHE1_PULL0_ACCESS_ENABLE;
    VIDEOREG(NV_PFIFO_CACHE1_PUSH0)=NV_PFIFO_CACHE1_PUSH0_ACCESS_ENABLE;
    VIDEOREG(NV_PFIFO_CACHES)=NV_PFIFO_CACHES_REASSIGN_ENABLED;

    pb_FifoChannelsReady|=(1<<channel);


    UserAddr=VIDEO_BASE+NV_USER+(pb_FifoChannelID<<16);

    pb_bind_channel(&sDmaObject6);
    pb_bind_channel(&sDmaObject12);
    pb_bind_channel(&sDmaObject2);
    pb_bind_channel(&sDmaObject7);
    pb_bind_channel(&sDmaObject4);
    pb_bind_channel(&sDmaObject5);
    pb_bind_channel(&sDmaObject3);
    pb_bind_channel(&sDmaObject9);
    pb_bind_channel(&sDmaObject10);
    pb_bind_channel(&sDmaObject11);
    pb_bind_channel(&sDmaObject8);

    //These objects match the GPU sub channels (3D, 2, 3, 4, in that order)
    pb_create_gr_ctx(13,GR_CLASS_97,&sGrObject13);
    pb_create_gr_ctx(14,GR_CLASS_39,&sGrObject14);
    pb_create_gr_ctx(16,GR_CLASS_9F,&sGrObject16);
    pb_create_gr_ctx(17,GR_CLASS_62,&sGrObject17);
    pb_bind_channel(&sGrObject13);
    pb_bind_channel(&sGrObject14);
    pb_bind_channel(&sGrObject16);
    pb_bind_channel(&sGrObject17);

    pb_DmaUserAddr=(DWORD *)UserAddr;   //VIDEOBASE+NV_USER+(0<<16)

    pb_PushBase=(DWORD)pb_Head;
    pb_PushLimit=(DWORD)pb_Tail;

    //This is the magic part of the whole push buffer DMA engine thing...
    //Both these instructions are necessary, remove one, then no dma engine!
    *((DWORD *)0x80000000)=(((DWORD)pb_Head)&0x0FFFFFFF)+1;
    __asm__ __volatile__ ("wbinvd");
    //assembler instruction wbinvd : write back and invalidate cache

    pb_start(); //start checking if new data has been written and send it to GPU
    //(nothing will be sent, since we sent nothing yet)

    TimeStamp1=KeTickCount;

#ifdef DBG
//  debugPrint("Waiting undil DMA is ready\n");
#endif
    //wait until DMA is ready
    while(1)
    {
        GetAddr=*(pb_DmaUserAddr+0x44/4);

        if (GetAddr>0x04000000)
        {
            debugPrint("pb_init: Bad getaddr\n");
            pb_kill();
            return -9;
        }

        PutAddr=((DWORD)pb_Put);

        if (((GetAddr^PutAddr)&0x0FFFFFFF)==0) break; //means same addresses (Dma is ready)

        TimeStamp2=KeTickCount;

        if (TimeStamp2-TimeStamp1>TICKSTIMEOUT)
        {
            debugPrint("pb_init: Dma didn't get ready in time\n");
            pb_kill();
            return -10;
        }
    }
#ifdef DBG
//  debugPrint("Dma is ready!!!\n");
#endif

    *((DWORD *)0x80000000)=0xFFFFFFFF;

    //Let's start initializing inner GPU registers!!!

    //These commands assign DMA channels to push buffer subchannels
    //and associate some specific GPU parts to specific Dma channels
    p=pb_begin();
    p=pb_push1_to(SUBCH_2,p,NV20_TCL_PRIMITIVE_SET_MAIN_OBJECT,14);
    p=pb_push1_to(SUBCH_3,p,NV20_TCL_PRIMITIVE_SET_MAIN_OBJECT,16);
    p=pb_push1_to(SUBCH_4,p,NV20_TCL_PRIMITIVE_SET_MAIN_OBJECT,17);
    p=pb_push1_to(SUBCH_3D,p,NV20_TCL_PRIMITIVE_SET_MAIN_OBJECT,13);
    p=pb_push1_to(SUBCH_2,p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT0,7);
    p=pb_push1_to(SUBCH_3,p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT5,17);
    p=pb_push1_to(SUBCH_3,p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT_UNKNOWN,3);
    p=pb_push2_to(SUBCH_4,p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT1,3,11);
    pb_end(p); //calls pb_start() which will trigger the reading and sending to GPU (asynchronous, no waiting)

    //setup needed for color computations
    p=pb_begin();
    pb_push(p++,NV20_TCL_PRIMITIVE_3D_SET_OBJECT0,3);
    *(p++)=2;
    *(p++)=3;
    *(p++)=3;
    pb_push(p++,NV20_TCL_PRIMITIVE_3D_SET_OBJECT2A,6);
    *(p++)=4;
    *(p++)=9;
    *(p++)=10;
    *(p++)=3;
    *(p++)=3;
    *(p++)=8;
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SET_OBJECT8,12);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_ACTIVATE_COLORS,0);
    pb_end(p);

    p=pb_begin();
    p=pb_push1(p,NV097_SET_FLAT_SHADE_OP,1); //FIRST_VTX
    p=pb_push4f(p,NV097_SET_EYE_POSITION,0.0f,0.0f,0.0f,1.0f);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_EDGE_FLAG,1);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_TX_SHADER_OP,0x00000000);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_TX_SHADER_PREVIOUS,0x00210000); //(PSTextureInput) What previous stage is used at each stage
    p=pb_push1(p,NV097_SET_COMPRESS_ZBUFFER_EN,0); //
    p=pb_push1(p,NV097_SET_SHADOW_ZSLOPE_THRESHOLD,0x7F800000);
    p=pb_push1(p,NV097_SET_ZMIN_MAX_CONTROL,1); //CULL_NEAR_FAR_EN_TRUE | ZCLAMP_EN_CULL | CULL_IGNORE_W_FALSE
    pb_end(p);

    p=pb_begin();
    p=pb_push_transposed_matrix(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_A(0),pb_IdentityMatrix);
    p=pb_push_transposed_matrix(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_A(4),pb_IdentityMatrix);
    p=pb_push_transposed_matrix(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_A(8),pb_IdentityMatrix);
    p=pb_push_transposed_matrix(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_A(12),pb_IdentityMatrix);
/*  p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_ENABLE(0),0x2202);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_ENABLE(1),0x2202);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_ENABLE(2),0x2202);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CLIP_PLANE_ENABLE(3),0x2202);
*/  p=pb_push4f(p,NV097_SET_FOG_PLANE,0.0f,0.0f,1.0f,0.0f);
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_ID,0x0000003C); //set shader constants cursor at C-36
    pb_push(p++,NV20_TCL_PRIMITIVE_3D_VP_UPLOAD_CONST_X,12);        //loads C-36, C-35 & C-34
    memcpy(p,pb_FixedPipelineConstants,12*4); p+=12; //used by common xbox shaders, but I doubt we will use them.
    //(also usually C-37 is screen center offset Decals vector & c-38 is Scales vector)
    pb_end(p);

    //Frame buffers creation
    //So far, tested only with 640*480 32 bits (default openxdk res)
    //Even if it's a waste of memory, for now, we will leave the openxdk (& SDL)
    //default frame buffer untouched. debugPrint (& SDL) will still target it.
    //We will provide functions pb_show_debug_screen() and pb_show_front_screen()
    //in order to let user (developper) toggle between screens at will.

    pb_FrameBuffersAddr=0;
    pb_DepthStencilAddr=0;
    pb_DepthStencilLast=-2;
    pb_DepthStencilLastAddr=0;
    pb_DepthStencilLastLimit=0;
    pb_WideClip=0;
    pb_TargetWidth=0;
    pb_TargetHeight=0;
    pb_MSAddr=0;
    pb_MSTiled=0;

    vm=XVideoGetMode();

    int ColorBpp = 0;
    switch(pb_ColorFmt) {
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_Z1R5G5B5:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1R5G5B5_O1R5G5B5:
        assert(vm.bpp == 15);
        // The effective width, due to padding, is still 16 bpp
        ColorBpp = 16;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_R5G6B5:
        assert(vm.bpp == 16);
        ColorBpp = 16;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_Z8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X8R8G8B8_O8R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_Z1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_X1A7R8G8B8_O1A7R8G8B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_A8R8G8B8:
        assert(vm.bpp == 32);
        ColorBpp = 32;
        break;
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_B8:
    case NV097_SET_SURFACE_FORMAT_COLOR_LE_G8B8:
        // These formats aren't suitable for display framebuffers
        assert(false);
        break;
    default:
        assert(false);
        break;
    }

    pb_ColorBpp = ColorBpp;

    int DepthBpp = 0;
    switch(pb_DepthFmt) {
    case PB_DEPTH_NONE:
        DepthBpp = 0;   //no depth stencil buffer
        break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z16:
        DepthBpp = 16;
        break;
    case NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:
        DepthBpp = 32;
        break;
    default:
        assert(false);
        break;
    }
    pb_DepthBpp = DepthBpp;
    pb_ZScale = pb_depth_scale_for(pb_DepthFmt, pb_DepthFloat); //0xFFFFFF for Z24S8

    // Activate pitched surface with chosen format. Without a depth buffer the zeta
    // format field keeps Z24S8 (0 is not a valid zeta format); the zeta surface is
    // never used because depth and stencil tests stay off (see set_draw_buffer).
    pb_GPUFrameBuffersFormat = 0x100 |
        (((pb_DepthFmt==PB_DEPTH_NONE)?NV097_SET_SURFACE_FORMAT_ZETA_Z24S8:pb_DepthFmt) << 4) | pb_ColorFmt;

    Width=vm.width;
    Height=vm.height;

    BackBufferCount=pb_BackBuffersWanted;   //2: triple buffering technic!
                        //allows dynamic details adjustment
                        //1: double buffering, pb_wait_for_flip() before each frame
    pb_FramesFinished=0;
    pb_FramesShown=0;

    pb_FrameBuffersCount=BackBufferCount+1; //front buffer + back buffers
    pb_FrameBuffersWidth=Width;
    pb_FrameBuffersHeight=Height;

    HScale=1;
    VScale=1;

    HSize=HScale*Width; //Total width
    VSize=VScale*Height;    //Total height

    //Multisampling: the sample buffer and the depth stencil buffer hold 2 (2X) or 2x2 (4X)
    //samples per pixel; the frame buffers (scan-out) keep the screen size
    pb_MSScaleX=(pb_MSMode!=PB_MS_NONE)?2:1;
    pb_MSScaleY=(pb_MSMode==PB_MS_4X)?2:1;

    //Front and back buffers (tile #0)

    FrameBufferCount=BackBufferCount+1;

    //pitch is the gap between start of a pixel line and start of next pixel line
    //(not necessarily the size of a pixel line, because of hardware optimization)

    Pitch=(((ColorBpp*HSize)>>3)+0x3F)&0xFFFFFFC0; //64 units aligned
    pb_FrameBuffersPitch=Pitch;

    //Tile pitch rule: a tiled buffer's surface pitch must be its tile pitch.
    //The CRTC scans the front buffer out with the pitch XVideoSetMode gave
    //AvSetDisplayMode (width*Bpp), and pb_show_debug_screen shares that pitch,
    //so the surface pitch of the frame buffers can't be raised to the next tile
    //pitch. When width*Bpp is not a tile pitch itself (640 wide 16 bits: 1280,
    //tile pitch would be 1536) the frame buffers are simply not tiled.
    //640 wide 32 bits (2560=0xA00) is a tile pitch: tiled as it always was.
    pb_FBTiled=pb_is_tile_pitch(Pitch);

    Size=Pitch*VSize;

    //verify 64 bytes alignment for size of a frame buffer
    if (Size&(64-1)) debugPrint("pb_init: FBSize is not well aligned.\n");

    pb_FBSize=Size;

    //multiply size by number of physical frame buffers in order to obtain global size
    FBSize=Size*FrameBufferCount;

    if (pb_AppFBCount)
    {
        //Application supplied frame buffers (pb_set_frame_buffers): used as given, never freed.
        //Their pitch must be the scan-out pitch (see the tile pitch rule above).
        if ((pb_AppFBCount!=FrameBufferCount)||(pb_AppFBPitch&&(pb_AppFBPitch!=Pitch)))
        {
            debugPrint("pb_init: pb_set_frame_buffers needs %d buffers of pitch %d\n",(int)FrameBufferCount,(int)Pitch);
            pb_kill();
            return -12;
        }
        for(i=0;i<FrameBufferCount;i++)
        {
            if (!pb_app_buffer_ok(pb_AppFBAddr[i],Size))
            {
                pb_kill();
                return -12;
            }
            pb_FBAddr[i]=pb_AppFBAddr[i];
            //one tile region for all of them: only if they follow each other
            if (pb_AppFBAddr[i]!=pb_AppFBAddr[0]+i*Size) pb_FBTiled=0;
        }
        //tile region start and size are 16 Kb granular: tile only what the buffers cover exactly
        if ((pb_AppFBAddr[0]&0x3FFF)||(FBSize&0x3FFF)) pb_FBTiled=0;
        pb_FBGlobalSize=FBSize;
        pb_FrameBuffersAddr=0;  //not ours: pb_kill doesn't free them
    }
    else
    {
        //Huge alignment enforcement (16 Kb aligned!) for the global size
        FBSize=(FBSize+0x3FFF)&0xFFFFC000;

        FBAddr = (DWORD)MmAllocateContiguousMemoryEx(FBSize, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);

        pb_FBGlobalSize=FBSize;

        pb_FrameBuffersAddr=FBAddr;
        if (!FBAddr)
        {
            pb_kill();
            return -11;
        }

        for(i=0;i<FrameBufferCount;i++)
        {
            pb_FBAddr[i]=FBAddr;
            FBAddr+=Size;
        }
    }

    //8 separate memory contiguous memory zones can be assigned to 8 GPU 'tiles'
    //simultaneously. GPU will apply automatic optimizations or caching on tiles.
    //The most important one is the automatic compression of data (by chunk of
    //16 dwords) in the depth stencil buffer. This buffer reading and writing
    //consumes most of the GPU time. By replacing the 16 dwords by a few dwords
    //(2 or 4), potential performance gain is about one third of frame time (60fps).
    //It is necessary to clear depth stencil buffer entirely at beginning of
    //each frame and draw things from closest depth to farest depth in order to
    //take full benefit of this very important feature. All fast games use it.
    //Compression is calculated by picking up central value of 4x4 block and
    //coding global x & y variation, plus all needed adjustments necessary to
    //fully recover original values. Compression is aborted if the 16 dwords have
    //very different values (will occur at the edges of projected triangles).

    if (pb_FBTiled)
        pb_assign_tile( 0,              //int   tile_index,
                pb_FBAddr[0]&0x03FFFFFF,    //DWORD tile_addr,
                FBSize,             //DWORD tile_size,
                Pitch,              //DWORD tile_pitch (= surface pitch)
                0,              //DWORD tile_z_start_tag,
                0,              //DWORD tile_z_offset,
                0               //DWORD tile_flags
                );


    pb_DSAddr=0;            //stays 0 (NULL) without a depth stencil buffer
    pb_DSTiled=0;
    pb_DSSize=0;
    pb_DepthStencilPitch=0;

    if (DepthBpp)
    {
        //Depth stencil buffer (tile #1)

        //pitch is the gap between start of a pixel line and start of next pixel line
        //(not necessarily the size of a pixel line, because of hardware optimization)

        Pitch=(((DepthBpp*HSize*pb_MSScaleX)>>3)+0x3F)&0xFFFFFFC0; //64 units aligned

        if (pb_AppDSAddr)
        {
            //Application supplied depth stencil buffer (pb_set_depth_stencil_buffer):
            //any 64 bytes multiple pitch >= width*Bpp, tiled only when it can be a tile region
            if ((pb_AppDSPitch<Pitch)||(pb_AppDSPitch&0x3F))
            {
                debugPrint("pb_init: depth stencil pitch must be >= %d and a multiple of 64\n",(int)Pitch);
                pb_kill();
                return -13;
            }
            Pitch=pb_AppDSPitch;
            Size=Pitch*VSize*pb_MSScaleY;
            if (!pb_app_buffer_ok(pb_AppDSAddr,Size))
            {
                pb_kill();
                return -13;
            }
            pb_DSTiled=pb_is_tile_pitch(Pitch)&&!(pb_AppDSAddr&0x3FFF)&&!(Size&0x3FFF);
            pb_DepthStencilPitch=Pitch;
            pb_DSSize=Size;
            DSSize=Size;
            pb_DepthStencilAddr=0;  //not ours: pb_kill doesn't free it
            pb_DSAddr=pb_AppDSAddr;
        }
        else
        {
            //Tile pitch rule: the depth buffer is never scanned out (pb_show_depth_screen
            //is a debug view), so its surface pitch is raised to the next tile pitch
            //(640 wide Z16: 1280 -> 1536; Z24S8: 2560, already a tile pitch).
            TilePitch=pb_tile_pitch_at_least(Pitch);
            if (TilePitch) Pitch=TilePitch;
            pb_DSTiled=(TilePitch!=0);
            pb_DepthStencilPitch=Pitch;

            Size=Pitch*VSize*pb_MSScaleY;

            //verify 64 bytes alignment for size of a frame buffer
            if (Size&(64-1)) debugPrint("pb_init: DSSize is not well aligned.\n");

            pb_DSSize=Size;

            // We use one shared depthbuffer
            DSSize=Size;

            //Huge alignment enforcement (16 Kb aligned!) for the global size
            DSSize=(DSSize+0x3FFF)&0xFFFFC000;

            DSAddr = (DWORD)MmAllocateContiguousMemoryEx(DSSize, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);

            pb_DepthStencilAddr=DSAddr;
            if (!DSAddr)
            {
                pb_kill();
                return -11;
            }

            pb_DSAddr=DSAddr;
        }

        if (pb_DSTiled)
            pb_assign_tile( 1,              //int   tile_index,
                pb_DSAddr&0x03FFFFFF,       //DWORD tile_addr,
                DSSize,             //DWORD tile_size,
                Pitch,              //DWORD tile_pitch (= surface pitch)
                0,              //DWORD tile_z_start_tag,
                0,              //DWORD tile_z_offset,
                (DepthBpp==32)?     //DWORD tile_flags: compressed depth buffer,
                (PB_TILE_FLAGS_ZCOMPRESS|PB_TILE_FLAGS_Z32BITS|PB_TILE_FLAGS_ZBUFFER): //0x84000001 for Z24S8
                (PB_TILE_FLAGS_ZCOMPRESS|PB_TILE_FLAGS_ZBUFFER)     //0x80000001 for Z16
                );
    }
    else if (pb_AppDSAddr)
    {
        debugPrint("pb_init: pb_set_depth_stencil_buffer with PB_DEPTH_NONE\n");
        pb_kill();
        return -13;
    }


    if (pb_ExtraBuffersCount)
    {
        //Extra back buffers (tile #2)

        //pitch is the gap between start of a pixel line and start of next pixel line
        //(not necessarily the size of a pixel line, because of hardware optimization)

        Pitch=(((ColorBpp*HSize)>>3)+0x3F)&0xFFFFFFC0; //64 units aligned
        //same surface pitch as the frame buffers (set_draw_buffer), tiled only if it is a tile pitch
        Size=Pitch*VSize;

        //verify 64 bytes alignment for size of a frame buffer
        if (Size&(64-1)) debugPrint("pb_init: EXSize is not well aligned.\n");

        //multiply size by number of physical frame buffers in order to obtain global size
        EXSize=Size*pb_ExtraBuffersCount;

        //Huge alignment enforcement (16 Kb aligned!) for the global size
        EXSize=(EXSize+0x3FFF)&0xFFFFC000;

        EXAddr = (DWORD)MmAllocateContiguousMemoryEx(EXSize, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!EXAddr)
        {
            pb_kill();
            return -11;
        }

        for(i=0;i<pb_ExtraBuffersCount;i++)
        {
            pb_EXAddr[i]=EXAddr;
            EXAddr+=Size;
        }

        if (pb_is_tile_pitch(Pitch))
            pb_assign_tile( 2,              //int   tile_index,
                pb_EXAddr[0]&0x03FFFFFF,    //DWORD tile_addr,
                EXSize,             //DWORD tile_size,
                Pitch,              //DWORD tile_pitch,
                0,              //DWORD tile_z_start_tag,
                0,              //DWORD tile_z_offset,
                0               //DWORD tile_flags
            );

    }




    if (pb_MSMode!=PB_MS_NONE)
    {
        //Multisample (sample) color buffer (tile #3): screen size * pb_MSScaleX/Y, drawn into
        //instead of the back buffer and resolved into it by pb_finished. It is never scanned
        //out, so its surface pitch is raised to the next tile pitch (tile pitch rule; 640 wide:
        //5120 for 32 bits, 2560 for 16 bits, both tile pitches already).
        Pitch=(((ColorBpp*HSize*pb_MSScaleX)>>3)+0x3F)&0xFFFFFFC0; //64 units aligned
        TilePitch=pb_tile_pitch_at_least(Pitch);
        if (TilePitch) Pitch=TilePitch;
        Size=Pitch*VSize*pb_MSScaleY;
        pb_MSPitch=Pitch;
        pb_MSSize=Size;

        Size=(Size+0x3FFF)&0xFFFFC000; //16 Kb granular (tile region)
        pb_MSAddr=(DWORD)MmAllocateContiguousMemoryEx(Size, 0, 0x03FFB000, 0x4000, PAGE_READWRITE | PAGE_WRITECOMBINE);
        if (!pb_MSAddr)
        {
            pb_kill();
            return -11;
        }

        pb_MSTiled=(TilePitch!=0);
        if (pb_MSTiled)
            pb_assign_tile( 3,              //int   tile_index,
                pb_MSAddr&0x03FFFFFF,       //DWORD tile_addr,
                Size,               //DWORD tile_size,
                Pitch,              //DWORD tile_pitch (= surface pitch)
                0,              //DWORD tile_z_start_tag,
                0,              //DWORD tile_z_offset,
                0               //DWORD tile_flags
            );
    }

    pb_FBVFlag=0x0000; //Quincunx & Gaussian need special flags. We don't, for now.
    pb_XScale=(float)HScale;
    pb_YScale=(float)VScale;
    if (pb_YScale<pb_XScale) pb_GlobalScale=pb_YScale; else pb_GlobalScale=pb_XScale;

    i=(DWORD)(2.0f*(pb_GlobalScale)+0.5f);
    switch(i)
    {
        case 0:
            pb_Bias=-8.0f;
            break;
        case 1:
            pb_Bias=0.53125f;
            break;
        case 2: //0.0f
        case 3: //0.585f
        case 4: //1.0f
        case 5: //1.322f
        case 6: //1.585f
        case 7: //1.907f
        case 8: //2.0f
            pb_Bias=pb_BiasTable[i-2];
            break;
    }

    p=pb_begin();
    n=pb_FrameBuffersCount; //(BackBufferCount+1)
    p=pb_push3(p,NV20_TCL_PRIMITIVE_3D_MAIN_TILES_INDICES,0,1,n);
    pb_end(p);

    //set area where GPU is allowed to draw pixels
    pb_set_viewport(0,0,vm.width*HScale,vm.height*VScale,0.0f,1.0f);

    //set vertex shader type
    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SHADER_TYPE,SHADER_TYPE_INTERNAL);
    pb_end(p);

    //no scissors (accept pixels in 8 rectangles covering all screen)
    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_MODE,0);   //accept pixels inside scissor rectangles union (1=reject)
    for(i=0;i<8;i++)
    {
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_HORIZ(i),0|((vm.width*HScale-1)<<16));
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VIEWPORT_CLIP_VERT(i),0|((vm.height*VScale-1)<<16));
    }
    pb_end(p);

    //funcs: never(0x200), less(0x201), equal(0x202), less or equal(0x203)
    //greater(0x204), not equal(0x205), greater or equal(0x206), always(0x207)

    //various intial settings (simple states)
    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DEPTH_FUNC,0x203); //Depth comparison function="less or equal"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_ALPHA_FUNC_FUNC,0x207); //Alpha comparison function="always"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BLEND_FUNC_ENABLE,0); //AlphaBlendEnable=FALSE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_ALPHA_FUNC_ENABLE,0); //AlphaTestEnable=FALSE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_ALPHA_FUNC_REF,0); //AlphaRef=0
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BLEND_FUNC_SRC,1); //SrcBlend=(1,1,1,1)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BLEND_FUNC_DST,0); //DstBlend=(0,0,0,0)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DEPTH_WRITE_ENABLE,1); //ZWriteEnable=TRUE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_DITHER_ENABLE,pb_get_dither()?1:0); //DitherEnable=FALSE (32 bits) / TRUE (16 bits) unless pb_set_dither was called
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SHADE_MODEL,0x1D01); //ShadeMode="gouraud"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_COLOR_MASK,0x01010101); // ColorWriteEnable=abgr
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_OP_ZFAIL,0x1E00); //StencilZFail="keep"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_OP_ZPASS,0x1E00); //StencilPass="keep"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_FUNC_FUNC,0x207); // Stencil comparison function="always"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_FUNC_REF,0); //StencilRef=0
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_FUNC_MASK,0xFFFFFFFF); //StencilMask=0xFFFFFFFF
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_MASK,0xFFFFFFFF); //StencilWriteMask=0xFFFFFFFF
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BLEND_EQUATION,0x8006); //Blend operator="add"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_BLEND_COLOR,0); //BlendColor=0x000000
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SWATHWIDTH,4); //SwathWidth=128
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_POLYGON_OFFSET_FACTOR,0); //PolygonOffZSlopeScale=0.0f (because ZBias=0.0f)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_POLYGON_OFFSET_UNITS,0); //PolygonOffZOffset=0.0f (because ZBias=0.0f)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_POLYGON_OFFSET_POINT_ENABLE,0); //PtOffEnable=FALSE (because ZBias=0.0f)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_POLYGON_OFFSET_LINE_ENABLE,0); //WireFrameOffEnable=FALSE (because ZBias=0.0f)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_POLYGON_OFFSET_FILL_ENABLE,0); //SolidOffEnable=FALSE (because ZBias=0.0f)
    pb_end(p);

    //various intial settings (complex states)
    p=pb_begin();
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_VERTEX_BLEND_ENABLE,0); //VertexBlend="disable"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FOG_ENABLE,0); //FogEnable=FALSE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FOG_COLOR,0); //FogColor=0x000000
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_POLYGON_MODE_FRONT,0x1B02,0x1B02); //FillMode="solid" BackFillMode="point"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_NORMALIZE_ENABLE,0); //NormalizeNormals=FALSE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_STENCIL_OP_FAIL,0x1E00); //StencilFail="keep"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FRONT_FACE,0x900); //FrontFace="clockwise"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CULL_FACE_ENABLE,1);//CullModeEnable=TRUE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CULL_FACE,0x405); //CullMode="FrontFace opposite" (counterclockwise)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_COLOR_LOGIC_OP_ENABLE,0); //Logic operator="none"
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_LINE_SMOOTH_ENABLE,0,0); //EdgeAntiAlias=0
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_MULTISAMPLE,0xFFFF0001); //MultiSampleAntiAliasing=TRUE & MultiSampleMask=0xFFFF
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_SHADOW_FUNC_FUNC,0); //Shadow comparison function="never"
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_LINE_WIDTH,(DWORD)(1.0f*8.0f*pb_GlobalScale+0.5f)); //LineWidth=1.0f =>8 (0-511)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0); //prepare subprogram call (wait/makespace, will obtain null status)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,1); //set parameter for subprogram (TRUE)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETNOISE); //call subprogID PB_SETNOISE: Dxt1NoiseEnable=TRUE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_CULL_ENABLE,3); //bit0:OcclusionCullEnable=TRUE & bit1:StencilCullEnable=TRUE
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_WAIT_MAKESPACE,0); //prepare subprogram call (wait/makespace, will obtain null status)
    p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PGRAPH_DEBUG_5,NV_PGRAPH_DEBUG_5_ZCULL_SPARE2_ENABLED); //set parameters A & B: DoNotCullUncompressed=FALSE (|8 otherwise)
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER); //calls subprogID PB_SETOUTER: does VIDEOREG(ParamA)=ParamB
    if (VIDEOREG(NV_PBUS_ROM_VERSION)&NV_PBUS_ROM_VERSION_MASK)
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PGRAPH_UNKNOWN_400B80,(0x45EAD10F&~0x18100000)); //RopZCmpAlwaysRead=FALSE (bit27) & RopZRead=FALSE (bit20)
    else
        p=pb_push2(p,NV20_TCL_PRIMITIVE_3D_PARAMETER_A,NV_PGRAPH_UNKNOWN_400B80,(0x45EAD10E&~0x18100000));
    p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_FIRE_INTERRUPT,PB_SETOUTER); //calls subprogID PB_SETOUTER: does VIDEOREG(ParamA)=ParamB
    pb_end(p);

    //various intial settings (texture stages states)
    p=pb_begin();
    for(i=0;i<4;i++)
    {
        p=pb_push1(p,NV20_TCL_PRIMITIVE_3D_TX_ENABLE(i),0); //texture enable flags
    }
    p=pb_push1(p,0x1b68,0); //texture stage 1 BumpEnvMat00=0.0f (stage +1 because no pixel shader used yet)
    p=pb_push1(p,0x1b6c,0); //texture stage 1 BumpEnvMat01=0.0f
    p=pb_push1(p,0x1b70,0);//texture stage 1 BumpEnvMat11=0.0f
    p=pb_push1(p,0x1b74,0); //texture stage 1 BumpEnvMat10=0.0f
    p=pb_push1(p,0x1b78,0); //texture stage 1 BumpEnvMatLightScale=0.0f
    p=pb_push1(p,0x1b7c,0); //texture stage 1 BumpEnvMatLightOffset=0.0f
    p=pb_push3(p,0x03c0,0,0,0); //texture stages 0 TexCoordIndex="passthru"
    p=pb_push1(p,0x1b24,0); //texture stage 0 BorderColor=0x000000
    p=pb_push1(p,0x0ae0,0); //texture stage 0 ColorKeyColor=0x000000
    p=pb_push1(p,0x1ba8,0); //texture stage 2 BumpEnvMat00=0.0f (stage +1 because no pixel shader used yet)
    p=pb_push1(p,0x1bac,0); //texture stage 2 BumpEnvMat01=0.0f
    p=pb_push1(p,0x1bb0,0);//texture stage 2 BumpEnvMat11=0.0f
    p=pb_push1(p,0x1bb4,0); //texture stage 2 BumpEnvMat10=0.0f
    p=pb_push1(p,0x1bb8,0); //texture stage 2 BumpEnvMatLightScale=0.0f
    p=pb_push1(p,0x1bbc,0); //texture stage 2 BumpEnvMatLightOffset=0.0f
    p=pb_push3(p,0x03d0,0,0,0); //texture stages 1 TexCoordIndex="passthru"
    p=pb_push1(p,0x1b64,0); //texture stage 1 BorderColor=0x000000
    p=pb_push1(p,0x0ae4,0); //texture stage 1 ColorKeyColor=0x000000
    p=pb_push1(p,0x1be8,0); //texture stage 3 BumpEnvMat00=0.0f (stage +1 because no pixel shader used yet)
    p=pb_push1(p,0x1bec,0); //texture stage 3 BumpEnvMat01=0.0f
    p=pb_push1(p,0x1bf0,0);//texture stage 3 BumpEnvMat11=0.0f
    p=pb_push1(p,0x1bf4,0); //texture stage 3 BumpEnvMat10=0.0f
    p=pb_push1(p,0x1bf8,0); //texture stage 3 BumpEnvMatLightScale=0.0f
    p=pb_push1(p,0x1bfc,0); //texture stage 3 BumpEnvMatLightOffset=0.0f
    p=pb_push3(p,0x03e0,0,0,0); //texture stages 2 TexCoordIndex="passthru"
    p=pb_push1(p,0x1ba4,0); //texture stage 2 BorderColor=0x000000
    p=pb_push1(p,0x0ae8,0); //texture stage 2 ColorKeyColor=0x000000
    p=pb_push3(p,0x03f0,0,0,0); //texture stages 3 TexCoordIndex="passthru"
    p=pb_push1(p,0x1be4,0); //texture stage 3 BorderColor=0x000000
    p=pb_push1(p,0x0aec,0); //texture stage 3 ColorKeyColor=0x000000
    pb_end(p);

    memset((DWORD *)pb_FBAddr[0],0,pb_FBSize);
    if (pb_DSAddr) memset((DWORD *)pb_DSAddr,0,pb_DSSize);
    if (pb_MSAddr) memset((DWORD *)pb_MSAddr,0,pb_MSSize);

    pb_back_index=1;        //frame buffer #1 is the back buffer for now
    pb_target_back_buffer();    //tells GPU what is the frame buffer target

    pb_front_index=0;       //frame buffer #0 is the front buffer for now
    pb_show_front_screen();     //show it

    pb_shutdown_registration.NotificationRoutine = pb_shutdown_notification_routine;
    pb_shutdown_registration.Priority = 0;
    HalRegisterShutdownNotification(&pb_shutdown_registration, TRUE);

    return 0;
}

static NTAPI VOID pb_shutdown_notification_routine (PHAL_SHUTDOWN_REGISTRATION ShutdownRegistration)
{
	pb_kill();
}
