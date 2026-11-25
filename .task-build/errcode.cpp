#include <cstdio>
#include "error_code.h"
using namespace WelsDec;
int main(){ printf("i4=%d i16=%d chroma=%d cabac_unexp=%d no_bs=%d\n",
 ERR_INFO_INVALID_I4x4_PRED_MODE, ERR_INFO_INVALID_I16x16_PRED_MODE,
 ERR_INFO_INVALID_I_CHROMA_PRED_MODE, ERR_CABAC_UNEXPECTED_VALUE, ERR_CABAC_NO_BS_TO_READ); }
