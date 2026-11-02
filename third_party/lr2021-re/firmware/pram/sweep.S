; LR2021 PRAM blob: custom commands through ActivatePram (0x012D) sub-commands.
; ARCv2 EM, linked at CPU 0x101000. Upload with tools/pram.py (WriteRegMem32 to host 0x801000),
; activate with "0x012D 00", call sub-command k with "0x012D k <params>".
;
; How the ROM uses it (docs/pram-patch.md, ROM 0x24FC / 0x10AC4):
;   [0x101000] magic, low byte of [0x101004] = number of entries, entry table at 0x101008.
;   Entry 0 runs once at activation and must return 2. Entry k >= 1 is a sub-command handler:
;     r0 = parameter length, r1 = parameter pointer, r2 = response struct {u32 len; u8 *buf}
;     return 2 = OK, 1 = parameter error, 0 = fail, 3 = data in the response struct.
;   Normal ARC ABI: r0-r12 caller-saved, r13-r25 callee-saved, blink = return address.

        .equ ROM_SET_RF_FREQ, 0xFC00    ; SetRfFrequency (0x0200) handler: r0 = 4, r1 = &be32 Hz
        .equ ROM_RSSI_INST,   0x5C4C    ; instant RSSI, 9 bits: dBm = -value / 2
        ; Results go into PRAM itself, after the code. 0x804000 is the packet engine's DMA target
        ; in LoRa RX, and CPU access to a DMA-owned bank stalls the chip. The host reads PRAM back
        ; at CPU 0x101000+ with ReadRegMem32.
        .equ OUT_BUF,         0x101C80  ; after the code + FFT tables (build.sh checks the image fits)
        .equ OUT_MAX,         448       ; up to 0x102000

        .arc_attribute Tag_ARC_ABI_rf16, 1     ; 16-entry register file, matches fft.c (-mrf16)
        .section .text
        .global _start
_start:
header:
        .word   0x600DB002              ; magic checked by pram_check
        .word   0x0000000B              ; low byte: 11 entries
entries:
        .word   install                 ; 0
        .word   sweep                   ; 1
        .word   hello                   ; 2
        .word   badcmd                  ; 3-6: retired debug entries (setfreq, rssi1, storetest, onepoint)
        .word   badcmd
        .word   badcmd
        .word   badcmd
        .word   fft_cmd                 ; 7 (fft.c: averaged FFT of a raw capture)
        .word   badcmd                  ; 8: retired (capture only, debug)
        .word   capfft_cmd              ; 9 (fft.c: capture + FFT + dB in one command)
        .word   bench_cmd               ; 10 (fft.c: timing benchmark)
        .word   0

; ---------------------------------------------------------------- 0: install
install:
        mov_s   r0, 2
        j_s     [blink]

; ---------------------------------------------------------------- 2: hello
; Returns 6 bytes: 'L' 'R' 'C' '1', parameter length, first parameter byte (or 0).
hello:
        ld_s    r3, [r2, 4]             ; response buffer
        mov_s   r12, 0x4C
        stb_s   r12, [r3, 0]
        mov_s   r12, 0x52
        stb_s   r12, [r3, 1]
        mov_s   r12, 0x43
        stb_s   r12, [r3, 2]
        mov_s   r12, 0x31
        stb_s   r12, [r3, 3]
        stb_s   r0, [r3, 4]
        mov_s   r12, 0
        breq_s  r0, 0, 1f
        ldb_s   r12, [r1, 0]
1:      stb_s   r12, [r3, 5]
        mov_s   r12, 6
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        j_s     [blink]

; ---------------------------------------------------------------- unused slots
badcmd:
        mov_s   r0, 1
        j_s     [blink]

; ---------------------------------------------------------------- 1: sweep
; Params (big-endian): start Hz (4), step Hz (4, signed), n (2), dwell (2), avg (2).
; For each point: retune (radio stays in RX), busy-wait `dwell` x 16 loop iterations, sum `avg`
; instant RSSI readings, store the u16 sum at OUT_BUF + 2*i (PRAM, see OUT_BUF). Returns 2 bytes:
; points done. The radio must already be in RX (SetRx) on the right path (LF below 1 GHz).
;
; The CPU has the reduced 16-entry register file (RF_BUILD 0x4B02): only r0-r3, r10-r15 and
; r26-r31 exist. Touching r16-r25 faults, so the state lives in a stack frame:
        .equ F_FREQ,  0                 ; be32 frequency buffer for the ROM handler
        .equ F_STEP,  4
        .equ F_N,     8                 ; points left
        .equ F_DWELL, 12
        .equ F_AVG,   16
        .equ F_OUT,   20                ; next output address
        .equ F_RESP,  24                ; response struct
        .equ F_DONE,  28
        .equ F_SIZE,  32
sweep:
        enter_s [r13-r15, blink]
        sub     sp, sp, F_SIZE
        st      r2, [sp, F_RESP]
        cmp     r0, 14
        blo     .Lbad
        mov     r15, r1                 ; parameter pointer
        ; start -> r13 (big-endian)
        ldb     r13, [r15, 0]
        ldb     r12, [r15, 1]
        asl     r13, r13, 8
        or      r13, r13, r12
        ldb     r12, [r15, 2]
        asl     r13, r13, 8
        or      r13, r13, r12
        ldb     r12, [r15, 3]
        asl     r13, r13, 8
        or      r13, r13, r12
        ; step
        ldb     r14, [r15, 4]
        ldb     r12, [r15, 5]
        asl     r14, r14, 8
        or      r14, r14, r12
        ldb     r12, [r15, 6]
        asl     r14, r14, 8
        or      r14, r14, r12
        ldb     r12, [r15, 7]
        asl     r14, r14, 8
        or      r14, r14, r12
        st      r14, [sp, F_STEP]
        ; n, dwell, avg (u16)
        ldb     r14, [r15, 8]
        ldb     r12, [r15, 9]
        asl     r14, r14, 8
        or      r14, r14, r12
        st      r14, [sp, F_N]
        cmp     r14, 0
        beq     .Lbad
        cmp     r14, OUT_MAX
        bhi     .Lbad
        ldb     r14, [r15, 10]
        ldb     r12, [r15, 11]
        asl     r14, r14, 8
        or      r14, r14, r12
        asl     r14, r14, 4             ; dwell x 16
        st      r14, [sp, F_DWELL]
        ldb     r14, [r15, 12]
        ldb     r12, [r15, 13]
        asl     r14, r14, 8
        or      r14, r14, r12
        st      r14, [sp, F_AVG]
        cmp     r14, 0
        beq     .Lbad
        mov     r12, OUT_BUF
        st      r12, [sp, F_OUT]
        mov     r12, 0
        st      r12, [sp, F_DONE]
.Lpoint:
        lsr     r12, r13, 24
        stb     r12, [sp, F_FREQ + 0]
        lsr     r12, r13, 16
        stb     r12, [sp, F_FREQ + 1]
        lsr     r12, r13, 8
        stb     r12, [sp, F_FREQ + 2]
        stb     r13, [sp, F_FREQ + 3]
        mov_s   r0, 4
        add     r1, sp, F_FREQ
        mov     r12, ROM_SET_RF_FREQ
        jl      [r12]
        ld      r12, [sp, F_DWELL]
        breq    r12, 0, 3f
2:      sub.f   r12, r12, 1
        bnz     2b
3:      mov     r14, 0                  ; RSSI sum
        ld      r15, [sp, F_AVG]        ; readings left
4:      mov     r12, ROM_RSSI_INST
        jl      [r12]
        add     r14, r14, r0
        sub.f   r15, r15, 1
        bnz     4b
        ld      r12, [sp, F_OUT]
        sth.ab  r14, [r12, 2]
        st      r12, [sp, F_OUT]
        ld      r12, [sp, F_DONE]
        add     r12, r12, 1
        st      r12, [sp, F_DONE]
        ld      r12, [sp, F_STEP]
        add     r13, r13, r12
        ld      r12, [sp, F_N]
        sub.f   r12, r12, 1
        st      r12, [sp, F_N]
        bnz     .Lpoint
        ; response: u16 points done
        ld      r2, [sp, F_RESP]
        ld_s    r3, [r2, 4]
        ld      r12, [sp, F_DONE]
        lsr     r0, r12, 8
        stb_s   r0, [r3, 0]
        stb_s   r12, [r3, 1]
        mov_s   r12, 2
        st_s    r12, [r2, 0]
        mov_s   r0, 3
        b       .Lout
.Lbad:
        mov_s   r0, 1
.Lout:
        add     sp, sp, F_SIZE
        leave_s [r13-r15, blink, pcl]
