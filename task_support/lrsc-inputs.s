# Benign created RV64 instruction data; never executed.
.option norelax
.text
.option norvc
start:
  lr.d.aq t0, (a0)
  addi a1, a0, 0
.option rvc
  c.addi t0, 1
.option norvc
  sc.d.rl t1, t0, (a1)
  bne t1, zero, start
  addi a2, zero, 1
