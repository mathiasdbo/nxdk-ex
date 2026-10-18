; AL_EFFECT_ECHO on the GP (lib/openal/docs/SOFT_GAP_PLAN.md phase9-05).
; Our own code, from the EFX echo's parameter descriptions; runs inside
; dsp/gp_loop.asm as a library kernel, loaded like a custom kernel.
;
; The slot's two bins summed are the input m (each send arrives half on each).
; One delay line, two taps:
;   t1 = line[t - D1], t2 = line[t - D2]          D2 = DELAY + LRDELAY
;   OUT L += G1L t1 + G2L t2;  OUT R += G1R t1 + G2R t2   (SPREAD, constant power)
;   line[t] = m + FEEDBACK lp(t2)                  lp: one pole, DAMPING
; so the first echo is the input as it came and each repeat is darker.
; Parameters (KPARAM + n), set by the library from the EFX echo:
;   0  D1 in samples, 1  D2 in samples (both clamped to 32 .. MAXDLY)
;   2  feedback, 3  damping pole, 4..7  G1L G1R G2L G2R (all Q23)
; One 32768-sample line (128 KiB) in scratch, one block a frame each way by
; DMA: the two tap reads before the kernel, the line write after it.

K_SCRATCH_BYTES equ $20000      ; image header: scratch the kernel needs
K_DESCRIPTORS   equ 3           ; image header: descriptors a frame (both chains)

KIN     equ     $E0             ; ABI (gp_loop.asm)
KSCR    equ     $E1
KPARAM  equ     $E8
K_RHEAD equ     $19
K_WHEAD equ     $1A
FIFO2   equ     $08
OUTL    equ     $20
OUTR    equ     $40

LINE    equ     $20000          ; bytes in the line
MAXDLY  equ     32704           ; samples (the line less two blocks)
MINDLY  equ     32              ; a tap must lie in blocks already written

INIT    equ     $108            ; 1 once the descriptors are set up
LPST    equ     $109            ; the damping lowpass's state
RD1     equ     $110            ; read descriptors, 7 words each
RD2     equ     $118
WR      equ     $120            ; write descriptor
T1      equ     $140            ; the tap blocks
T2      equ     $160
WB      equ     $180            ; the block written to the line

EOL     equ     $004000
CTL_R   equ     $0049D0         ; memory -> DSP, write-back, circular scratch, 24-bit, step 1
CTL_W   equ     $0049D2         ; DSP -> memory, the same

        org     p:$100
kernel  move    x:INIT,a
        tst     a
        jne     ready

; first call: descriptors and chain heads (read RD1 -> RD2; write WR -> FIFO2)
        move    #RD2,x0
        move    x0,x:RD1
        move    #EOL,x0
        move    x0,x:RD2
        move    #FIFO2,x0
        move    x0,x:WR
        move    #CTL_R,x0
        move    x0,x:RD1+1
        move    x0,x:RD2+1
        move    #CTL_W,x0
        move    x0,x:WR+1
        move    #32,x0
        move    x0,x:RD1+2
        move    x0,x:RD2+2
        move    x0,x:WR+2
        move    #T1,x0
        move    x0,x:RD1+3
        move    #T2,x0
        move    x0,x:RD2+3
        move    #WB,x0
        move    x0,x:WR+3
        move    #LINE-1,x0
        move    x0,x:RD1+6
        move    x0,x:RD2+6
        move    x0,x:WR+6
        move    x:KSCR,x0
        move    x0,x:RD1+5
        move    x0,x:RD2+5
        move    x0,x:WR+5
        move    #RD1,x0
        move    x0,x:K_RHEAD
        move    #WR,x0
        move    x0,x:K_WHEAD
        move    #1,x0
        move    x0,x:INIT

; the taps into OUT L, then OUT R
ready   move    #T1,r0
        move    #T2,r1
        move    #OUTL,r3
        move    x:KPARAM+4,y0           ; G1L
        move    x:KPARAM+6,y1           ; G2L
        jsr     taps
        move    #T1,r0
        move    #T2,r1
        move    #OUTR,r3
        move    x:KPARAM+5,y0           ; G1R
        move    x:KPARAM+7,y1           ; G2R
        jsr     taps

; the line's next block: m + FEEDBACK lp(t2)
        move    x:KIN,r4
        move    x:KIN,a
        add     #32,a
        move    a1,r5
        move    #T2,r1
        move    #WB,r2
        move    x:KPARAM+2,y0           ; feedback
        move    x:KPARAM+3,y1           ; damping pole
        move    x:LPST,b
        do      #32,line_end
        move    x:(r1)+,x1              ; t2
        tfr     b,a
        sub     x1,a                    ; lp - t2
        move    a,x0
        tfr     x1,b
        mac     x0,y1,b                 ; lp = t2 + p (lp - t2)
        move    b,x1                    ; lp (limited)
        move    x:(r4)+,a               ; left bin
        move    x:(r5)+,x0
        add     x0,a                    ; m
        mac     x1,y0,a                 ; m + FEEDBACK lp
        move    a,x:(r2)+
line_end
        move    b,x:LPST

; next frame's tap reads: offset = write offset + 128 - 4 D (mod LINE)
        move    x:KPARAM,a
        jsr     offset
        move    b1,x:RD1+4
        move    x:KPARAM+1,a
        jsr     offset
        move    b1,x:RD2+4
        rts

; a = a delay in samples -> b = the read offset for it (uses x0)
offset  move    #MINDLY,x0
        cmp     x0,a
        jge     o_lo
        tfr     x0,a
o_lo    move    #MAXDLY,x0
        cmp     x0,a
        jle     o_hi
        tfr     x0,a
o_hi    asl     a
        asl     a                       ; 4 D bytes
        move    x:WR+4,b
        move    #128,x0
        add     x0,b
        sub     a,b
        jge     o_ok
        move    #LINE,x0
        add     x0,b
o_ok    rts

; OUT (r3) += y0 x (r0) + y1 x (r1), 32 samples
taps    do      #32,tp_end
        move    x:(r3),a
        move    x:(r0)+,x0
        mac     x0,y0,a
        move    x:(r1)+,x1
        mac     x1,y1,a
        move    a,x:(r3)+
tp_end
        rts
