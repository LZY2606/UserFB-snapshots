; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:21:05
; Input file: .task-build/wf_l_bra.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        lda     #$05
        adc     #$01
        bcc     L8008
        sep     #$20
L8008:  rep     #$20
        .a16
        rts

