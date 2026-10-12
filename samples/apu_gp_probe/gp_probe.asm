; GP probe program (samples/apu_gp_probe, reverb plan phases 0 and 2).
; The copy program's frame loop plus optional measurements, switched by the
; flag word at X:$11 that the CPU writes through the X window:
;   bit 0  after starting the DMA chain, wait for its end-of-list flag
;          ($FFFFC5 bit 7), at most X:$17 polls; log the poll counts
;   bit 1  burn X:$12 cycles per frame (rep of nop)
;   bit 2  read X:(X:$13) and Y:(X:$13) with the core into X:$14 / X:$1A
;   bit 3  echo (phase 2): chain A reads the delay line into DIN, then
;            out L/R = bin 0/1 + wet * line,  line in = bin 2 + fb * line
;          into OUTL/OUTR and LOUT; chain B writes LOUT to the line and
;          sends OUTL/OUTR to the FIFO
; Chain A starts at X:(HEADA), chain B at X:(HEADB) (both 0 = the FIFO
; descriptor at X:0). Every frame the frame counter is stamped into X:$300.

CNT     equ     $10             ; frame counter (apu_gp_frames)
FLAGS   equ     $11
BURN    equ     $12
RADDR   equ     $13
RX      equ     $14
SPINS   equ     $15             ; polls until end-of-list, last wait
SPMAX   equ     $16             ; largest SPINS seen
SPLIM   equ     $17             ; poll limit
TMOUT   equ     $18             ; waits that hit the limit
RAWC5   equ     $19             ; $FFFFC5 before the clear, last frame
RY      equ     $1A
DMACTL  equ     $1B             ; DMA CONTROL after the wait, last frame
HEADA   equ     $1C
HEADB   equ     $1D
WET     equ     $1E             ; Q23
FB      equ     $1F             ; Q23
OUTL    equ     $200            ; staging: L at $200, R at $220 (FIFO descriptor stride 32)
OUTR    equ     $220
DIN     equ     $240            ; 32 words read from the line
LOUT    equ     $260            ; 32 words written to the line
STAMP   equ     $300
BIN0    equ     $1400
BIN1    equ     $1420
BIN2    equ     $1440

        org     p:0
frame   movep   #1,x:$FFFFC4            ; frame complete
wait    jclr    #1,x:$FFFFC5,wait       ; wait for START_FRAME
        movep   #2,x:$FFFFC5            ; acknowledge it
        move    x:CNT,x0
        move    x0,x:STAMP
        movep   x:$FFFFC5,x:RAWC5
        movep   #$80,x:$FFFFC5          ; clear end-of-list
        movep   x:HEADA,x:$FFFFD4       ; chain A
        movep   #1,x:$FFFFD6            ; start
        jset    #3,x:FLAGS,echo
        jclr    #0,x:FLAGS,nowait
        jsr     waiteol
nowait  jclr    #1,x:FLAGS,noburn
        rep     x:BURN
        nop
noburn  jclr    #2,x:FLAGS,count
        move    x:RADDR,r0
        nop
        move    x:(r0),b
        move    b1,x:RX
        move    y:(r0),b
        move    b1,x:RY
        jmp     count

echo    jsr     waiteol                 ; the line block is in DIN
        move    #BIN0,r0
        move    #BIN1,r1
        move    #BIN2,r2
        move    #DIN,r3
        move    #LOUT,r4
        move    #OUTL,r5
        move    #OUTR,r6
        move    x:WET,x1
        move    x:FB,y1
        do      #32,eloop
        move    x:(r3)+,x0              ; line sample
        move    x:(r0)+,a               ; dry left
        mac     x0,x1,a
        move    a,x:(r5)+
        move    x:(r1)+,a               ; dry right
        mac     x0,x1,a
        move    a,x:(r6)+
        move    x:(r2)+,a               ; send
        mac     x0,y1,a
        move    a,x:(r4)+
eloop
        movep   #$80,x:$FFFFC5
        movep   x:HEADB,x:$FFFFD4       ; chain B
        movep   #1,x:$FFFFD6
        jsr     waiteol

count   move    x:CNT,b
        add     #1,b
        move    b1,x:CNT
        jmp     frame

; Wait for the chain's end-of-list flag, at most X:SPLIM polls (uses b, x0)
waiteol clr     b
        move    x:SPLIM,x0
spin    jset    #7,x:$FFFFC5,done
        add     #1,b
        cmp     x0,b
        jlt     spin
        move    b1,x:SPINS              ; limit reached
        move    x:TMOUT,b
        add     #1,b
        move    b1,x:TMOUT
        jmp     after
done    move    b1,x:SPINS
        move    x:SPMAX,x0
        cmp     x0,b
        jle     after
        move    b1,x:SPMAX
after   movep   x:$FFFFD6,x:DMACTL
        rts
