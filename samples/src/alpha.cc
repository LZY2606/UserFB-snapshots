// Author-created fixture; BSD-3-Clause, see ../../LICENSE.
extern "C" void observe(int*);
extern "C" __attribute__((noinline)) int recover_alpha(int input) {
  int moving = input + 3;
  int stable = input * 7;
  asm volatile("" : "+r"(moving));
  if (input & 1) { int moving = input + 11; observe(&moving); stable += moving; }
  observe(&moving);
  return moving + stable;
}
