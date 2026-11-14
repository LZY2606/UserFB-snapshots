#include <stdio.h>
int _test_func_test_api_play_buffer(void);
int _test_func_test_api_inject_event(void);
int _test_func_test_player_nna_cont(void);
int _test_func_test_api_restart_module(void);
#define RUN(name) do { if (_test_func_##name()!=0) return 1; puts("OLD_TEST_OK " #name); } while(0)
int main(void) {
 RUN(test_api_play_buffer); RUN(test_api_inject_event);
 RUN(test_player_nna_cont); RUN(test_api_restart_module);
 puts("OLD_PLAYBACK_REGRESSIONS_OK count=4"); return 0;
}
