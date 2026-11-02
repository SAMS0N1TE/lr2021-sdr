; CPU timing benchmarks for entry 10 (fft.c bench_cmd).
; (The FFT butterfly stage used to live here in assembly; it is C again now that the patched GCC
; can allocate blink, see fft.c fft_stage.)

        .arc_attribute Tag_ARC_ABI_rf16, 1     ; 16-entry register file, matches fft.c (-mrf16)
        .section .text

; 1,000,000 iterations of a one-NOP zero-overhead loop: 1 cycle each (benchmark, entry 10)
        .global bench_nops
        .align  4                       ; bl targets must be 4-byte aligned
bench_nops:
        mov     lp_count, 1000000
        lp      .Lnops_end
        nop
.Lnops_end:
        j_s     [blink]

; Micro-benchmarks (entry 10, modes 16..22): 1,000,000 iterations of a zero-overhead loop whose
; body is: 16 = nop_s, 17 = 2 x nop_s, 18 = 2 x 32-bit add, 19 = ldh.x from 0x805C00,
; 20 = sth to 0x805C00, 21 = mpyw, 22 = mpy.     void bench_micro(u32 mode);
        .global bench_micro
        .align  4
bench_micro:
        mov     r1, 0x805C00
        mov     r12, 3
        mov     lp_count, 1000000
        breq    r0, 16, .Lm16
        breq    r0, 17, .Lm17
        breq    r0, 18, .Lm18
        breq    r0, 19, .Lm19
        breq    r0, 20, .Lm20
        breq    r0, 21, .Lm21
        breq    r0, 22, .Lm22
        j_s     [blink]
.Lm16:  lp      .Le16
        nop_s
.Le16:  j_s     [blink]
.Lm17:  lp      .Le17
        nop_s
        nop_s
.Le17:  j_s     [blink]
.Lm18:  lp      .Le18
        add     r2, r2, 0x1234
        add     r3, r3, 0x1234
.Le18:  j_s     [blink]
.Lm19:  lp      .Le19
        ldh.x   r2, [r1, 0]
.Le19:  j_s     [blink]
.Lm20:  lp      .Le20
        sth     r12, [r1, 0]
.Le20:  j_s     [blink]
.Lm21:  lp      .Le21
        mpyw    r2, r12, r12
.Le21:  j_s     [blink]
.Lm22:  lp      .Le22
        mpy     r2, r12, r12
.Le22:  j_s     [blink]
