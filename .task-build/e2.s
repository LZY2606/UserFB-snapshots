; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:10:50
; Input file: .task-build/e2.bin
; Page:       1


        .setcpu "65816"

        .a8
        .i8
        jmp     L8005

        .byte   $A9
        .byte   $01
L8005:  rts

        .byte   $A9
        .byte   $02
        .byte   $60
