; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 11:44:41
; Input file: .task-build/state.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        rep     #$20
        .a16
        lda     #$1234
        sep     #$20
        .a8
        lda     #$56
        rts

