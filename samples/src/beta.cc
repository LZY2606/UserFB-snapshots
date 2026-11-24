// Author-created fixture; BSD-3-Clause, see ../../LICENSE.
extern "C" void observe(int*);
extern "C" __attribute__((noinline)) int recover_beta(int input) {
  int moving = input - 5;
  asm volatile("" : "+r"(moving));
  observe(&moving);
  return moving * 3;
}
