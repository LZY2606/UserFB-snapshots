# Select one precise, compilable semantic change; this is not production code.
BEGIN { mode=ARGV[1];ARGV[1]="";hit=0; in_exception=0; neg=0 }
/if \(\*values == 0x1000000\)/ { in_exception=1 }
/else if \(wps->float_flags \& FLOAT_NEG_ZEROS\)/ { neg=1 }
{
 if(mode=="negative_zero" && neg && /set_sign \(outval, getbit/){$0="                    set_sign (outval, getbit (&wps->wvxbits) & 0);";hit++;neg=0}
 if(mode=="nan" && in_exception && /set_mantissa \(outval, temp\)/){sub(/set_mantissa \(outval, temp\)/,"set_mantissa (outval, 0)");hit++;in_exception=0}
 print
}
END { if(hit!=1)exit 7 }
