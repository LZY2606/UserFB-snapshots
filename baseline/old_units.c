#define main upstream_test_main
#include "test.c"
#undef main
int main(void){test_format_commands();test_reply_reader();printf("OLD_PARSER_UNITS tests=%d fails=%d\n",tests,fails);return fails||tests==0;}
