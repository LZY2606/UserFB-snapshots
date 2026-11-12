#if defined(__APPLE__)
#include <machine/endian.h>
#ifndef BYTE_ORDER
#define BYTE_ORDER __BYTE_ORDER__
#define BIG_ENDIAN __ORDER_BIG_ENDIAN__
#endif
#endif
/* Upstream uses an older spelling for the AArch64 two-table byte lookup. */
#if defined(__aarch64__)
#include <arm_neon.h>
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
