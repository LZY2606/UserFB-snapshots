; da65 V2.19 - Git b166995d0
; Created:    2026-11-14 12:07:39
; Input file: testwrk/dasm/test65816.bin
; Page:       1


        .setcpu "65816"

L1234           := $1234
L123456         := $123456
LFEDCBA         := $FEDCBA
        .a8
        .i8
        adc     ($10,x)
        adc     $32,s
        adc     $10
        adc     [$10]
        adc     #$54
        adc     $9876
        adc     $FEDBCA
        adc     ($10),y
        adc     ($10)
        adc     ($32,s),y
        adc     $10,x
        adc     [$10],y
        adc     $9876,y
        adc     $9876,x
        adc     LFEDCBA,x
        sbc     ($10,x)
        sbc     $32,s
        sbc     $10
        sbc     [$10]
        sbc     #$54
        sbc     $9876
        sbc     $FEDBCA
        sbc     ($10),y
        sbc     ($10)
        sbc     ($32,s),y
        sbc     $10,x
        sbc     [$10],y
        sbc     $9876,y
        sbc     $9876,x
        sbc     LFEDCBA,x
        cmp     ($10,x)
        cmp     $32,s
        cmp     $10
        cmp     [$10]
        cmp     #$54
        cmp     $9876
        cmp     $FEDBCA
        cmp     ($10),y
        cmp     ($10)
        cmp     ($32,s),y
        cmp     $10,x
        cmp     [$10],y
        cmp     $9876,y
        cmp     $9876,x
        cmp     LFEDCBA,x
        cpx     #$54
        cpx     $10
        cpx     $9876
        cpy     #$54
        cpy     $10
        cpy     $9876
        dec     a
        dec     $10
        dec     $9876
        dec     $10,x
        dec     $9876,x
        dex
        dey
        inc     a
        inc     $10
        inc     $9876
        inc     $10,x
        inc     $9876,x
        inx
        iny
        and     ($10,x)
        and     $32,s
        and     $10
        and     [$10]
        and     #$54
        and     $9876
        and     $FEDBCA
        and     ($10),y
        and     ($10)
        and     ($32,s),y
        and     $10,x
        and     [$10],y
        and     $9876,y
        and     $9876,x
        and     LFEDCBA,x
        eor     ($10,x)
        eor     $32,s
        eor     $10
        eor     [$10]
        eor     #$54
        eor     $9876
        eor     $FEDBCA
        eor     ($10),y
        eor     ($10)
        eor     ($32,s),y
        eor     $10,x
        eor     [$10],y
        eor     $9876,y
        eor     $9876,x
        eor     LFEDCBA,x
        ora     ($10,x)
        ora     $32,s
        ora     $10
        ora     [$10]
        ora     #$54
        ora     $9876
        ora     $FEDBCA
        ora     ($10),y
        ora     ($10)
        ora     ($32,s),y
        ora     $10,x
        ora     [$10],y
        ora     $9876,y
        ora     $9876,x
        ora     LFEDCBA,x
        bit     $10
        bit     $9876
        bit     $10,x
        bit     $9876,x
        bit     #$54
        trb     $10
        trb     $9876
        tsb     $10
        tsb     $9876
        asl     $10
        asl     a
        asl     $9876
        asl     $10,x
        asl     $9876,x
        lsr     $10
        lsr     a
        lsr     $9876
        lsr     $10,x
        lsr     $9876,x
        rol     $10
        rol     a
        rol     $9876
        rol     $10,x
        rol     $9876,x
        ror     $10
        ror     a
        ror     $9876
        ror     $10,x
        ror     $9876,x
L8148:  bcc     L8148
        bcs     L8148
        beq     L8148
        bmi     L8148
        bne     L8148
        bpl     L8148
        bra     L8148
        bvc     L8148
        bvs     L8148
        brl     L8148
        jmp     L1234

        jml     LFEDCBA
        jmp     (L1234)

        jmp     (L1234,x)
        jml     (L1234)
        jml     (L1234)
        jsl     L123456
        jsr     L1234
        jsr     (L1234,x)
        rtl
        rts

        brk
        rti

        clc
        cld
        cli
        clv
        sec
        sed
        sei
        rep     #$12
        sep     #$12
        lda     ($10,x)
        lda     $32,s
        lda     $10
        lda     [$10]
        lda     #$54
        lda     $9876
        lda     $FEDBCA
        lda     ($10),y
        lda     ($10)
        lda     ($32,s),y
        lda     $10,x
        lda     [$10],y
        lda     $9876,y
        lda     $9876,x
        lda     LFEDCBA,x
        ldx     #$54
        ldx     $10
        ldx     $9876
        ldx     $10,y
        ldx     $9876,y
        ldy     #$54
        ldy     $10
        ldy     $9876
        ldy     $10,x
        ldy     $9876,x
        sta     ($10,x)
        sta     $32,s
        sta     $10
        sta     [$10]
        sta     $9876
        sta     $FEDBCA
        sta     ($10),y
        sta     ($10)
        sta     ($32,s),y
        sta     $10,x
        sta     [$10],y
        sta     $9876,y
        sta     $9876,x
        sta     LFEDCBA,x
        stx     $10
        stx     $9876
        stx     $10,y
        sty     $10
        sty     $9876
        sty     $10,x
        stz     $10
        stz     $10,x
        stz     $9876
        stz     $9876,x
        mvn     #$12, #$34
        mvp     #$12, #$34
        nop
        pea     L1234
        pei     ($12)
        per     L8148
        pha
        phx
        phy
        pla
        plx
        ply
        phb
        phd
        phk
        php
        plb
        pld
        plp
        stp
        wai
        tax
        tay
        tsx
        txa
        txs
        txy
        tya
        tyx
        tcd
        tcs
        tdc
        .a16
        .i16
        tsc
        xba
        xce
        adc     #$5432
        sbc     #$5432
        cmp     #$5432
        cpx     #$5432
        cpy     #$5432
        and     #$5432
        eor     #$5432
        ora     #$5432
        bit     #$5432
        lda     #$5432
        ldx     #$5432
        ldy     #$5432
        rts

        jsl     LFEDCBA
        jml     LFEDCBA
        rtl
