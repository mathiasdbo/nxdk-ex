; Example custom GP kernel: a stereo echo (lib/openal/docs/EFX_PLAN.md 3.6).
; Our own code; runs inside dsp/gp_loop.asm (kernel ABI there), loaded with
; alXboxEffectProgram() from the image tools/dsp56asm.py --image makes.
;
; Per ear: delayed = line[t - DELAY]; OUT += WET * delayed;
;          line[t] = in + FEEDBACK * delayed
; Parameters (AL_XBOX_GP_PARAM0 + n):
;   0  delay in samples, 32 .. 16384 (raw integer; clamped)
;   1  feedback, Q23 (alEffectf -1 .. 1)
;   2  wet gain, Q23
; Two 16384-sample lines (64 KiB each) in the kernel's scratch, moved one
; 32-sample block per frame by DMA: the read descriptors run before the
; kernel, the writes after it. Every frame the kernel sets the read offsets
; for the next frame from the write offset and DELAY, so a new delay applies
; on the next frame.

K_SCRATCH_BYTES equ $20000      ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 4           ; image header: descriptors a frame (both chains)

KIN     equ     $E0             ; ABI (gp_loop.asm)
KSCR    equ     $E1
KPARAM  equ     $E8
K_RHEAD equ     $19
K_WHEAD equ     $1A
FIFO2   equ     $08
OUTL    equ     $20
OUTR    equ     $40

LINE    equ     $10000          ; bytes per line
MAXDLY  equ     16384           ; samples
MINDLY  equ     32

INIT    equ     $108            ; 1 once the descriptors are set up
RDL     equ     $110            ; read descriptors (left, right), 7 words each
RDR     equ     $118
WRL     equ     $120            ; write descriptors
WRR     equ     $128
DLYL    equ     $140            ; the delayed block, per ear
DLYR    equ     $160
WBL     equ     $180            ; the block written back, per ear
WBR     equ     $1A0

EOL     equ     $004000
CTL_R   equ     $0049D0         ; memory -> DSP, write-back, circular scratch, 24-bit, step 1
CTL_W   equ     $0049D2         ; DSP -> memory, the same

        org     p:$100
kernel  move    x:INIT,a
        tst     a
        jne     ready

; first call: descriptors and chain heads (read L -> R; write L -> R -> FIFO2)
        move    #RDR,x0
        move    x0,x:RDL
        move    #EOL,x0
        move    x0,x:RDR
        move    #WRR,x0
        move    x0,x:WRL
        move    #FIFO2,x0
        move    x0,x:WRR
        move    #CTL_R,x0
        move    x0,x:RDL+1
        move    x0,x:RDR+1
        move    #CTL_W,x0
        move    x0,x:WRL+1
        move    x0,x:WRR+1
        move    #32,x0
        move    x0,x:RDL+2
        move    x0,x:RDR+2
        move    x0,x:WRL+2
        move    x0,x:WRR+2
        move    #DLYL,x0
        move    x0,x:RDL+3
        move    #DLYR,x0
        move    x0,x:RDR+3
        move    #WBL,x0
        move    x0,x:WRL+3
        move    #WBR,x0
        move    x0,x:WRR+3
        move    #LINE-1,x0
        move    x0,x:RDL+6
        move    x0,x:RDR+6
        move    x0,x:WRL+6
        move    x0,x:WRR+6
        move    x:KSCR,a                ; left line at KSCR, right at KSCR + LINE
        move    a1,x:RDL+5
        move    a1,x:WRL+5
        move    #LINE,x0
        add     x0,a
        move    a1,x:RDR+5
        move    a1,x:WRR+5
        move    #RDL,x0
        move    x0,x:K_RHEAD
        move    #WRL,x0
        move    x0,x:K_WHEAD
        move    #1,x0
        move    x0,x:INIT

; left: OUT L += WET * delayed; line = in + FEEDBACK * delayed
ready   move    x:KPARAM+2,y0           ; wet
        move    x:KPARAM+1,y1           ; feedback
        move    #DLYL,r0
        move    x:KIN,r1
        move    #WBL,r2
        move    #OUTL,r3
        jsr     ear
        move    #DLYR,r0
        move    x:KIN,a
        add     #32,a
        move    a1,r1
        move    #WBR,r2
        move    #OUTR,r3
        jsr     ear

; next frame's reads: offset = write offset + 128 - 4 DELAY (mod LINE)
        move    x:KPARAM,a              ; DELAY, clamped
        move    #MINDLY,x0
        cmp     x0,a
        jge     dly_lo
        tfr     x0,a
dly_lo  move    #MAXDLY,x0
        cmp     x0,a
        jle     dly_hi
        tfr     x0,a
dly_hi  asl     a
        asl     a                       ; 4 DELAY bytes
        move    x:WRL+4,b
        move    #128,x0
        add     x0,b
        sub     a,b
        jge     off_ok
        move    #LINE,x0
        add     x0,b
off_ok  move    b1,x:RDL+4
        move    b1,x:RDR+4
        rts

; one ear: r0 delayed block, r1 input bin, r2 write-back block, r3 OUT block
ear     do      #32,ear_end
        move    x:(r0)+,x0              ; delayed
        move    x:(r3),a
        mac     x0,y0,a                 ; OUT + WET * delayed
        move    a,x:(r3)+
        move    x:(r1)+,b
        mac     x0,y1,b                 ; in + FEEDBACK * delayed
        move    b,x:(r2)+
ear_end
        rts
