#ifndef TASK_CONFIG_H
#define TASK_CONFIG_H
#define HAVE_LIBPOPT 0
#define HAVE_GCC 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define STDC_HEADERS 1
#if defined(__APPLE__)
#include <machine/endian.h>
#elif defined(__linux__)
#include <endian.h>
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER__
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN __ORDER_LITTLE_ENDIAN__
#endif
#endif
