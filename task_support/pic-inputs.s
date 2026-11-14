.set noreorder
.text
.globl fixture_start
fixture_start:
lui $gp,0x1234
addiu $gp,$gp,-16
addu $gp,$gp,$t9
lw $t9,-32752($gp)
jalr $ra,$t9
addiu $a0,$zero,7
addu $s0,$gp,$zero
lw $t9,16($gp)
