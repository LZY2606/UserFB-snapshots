#ifndef TASK_PLATFORM_H
#define TASK_PLATFORM_H
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BLARGG_LITTLE_ENDIAN 1
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BLARGG_BIG_ENDIAN 1
#else
#error Unsupported compiler byte order
#endif
#endif
