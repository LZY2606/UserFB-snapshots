/^LC3_HOT static int get_quantized/ { in_quant=1 }
/^LC3_HOT static void put_residual/ { in_quant=0 }
/^LC3_HOT static void get_lsb/ { in_lsb=1 }
/^static int estimate_noise/ { in_lsb=0 }
{
 if(mode=="context" && in_quant && /state = \(state << 4\) \+/) { sub(/\(state << 4\) \+ /, ""); count++ }
 if(mode=="lsb-sign" && in_lsb && /x\[i(\+1)?\] = lc3_get_bit\(bits\) \? -1 : 1;/) { sub(/\? -1 : 1/, "? 1 : 1"); count++ }
 print
}
END { if(count != (mode=="context" ? 1 : 2))exit 9 }
