#ifndef TASK_UUID_COMPAT_H
#define TASK_UUID_COMPAT_H
typedef unsigned char uuid_t[16];
void uuid_generate(uuid_t out);
#endif
