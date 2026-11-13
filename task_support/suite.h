#ifndef NES_DAC_TASK_SUITE_H
#define NES_DAC_TASK_SUITE_H
namespace nes_task { void check(bool ok, const char* expr, const char* file, int line); }
#define TARGET_CHECK(x) ::nes_task::check(bool(x),#x,__FILE__,__LINE__)
void target_dac();
void target_timeline();
void target_nsf();
#endif
