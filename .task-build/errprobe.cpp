#include "rtperrors.h"
#include <cstdio>
int main(){printf("invalidpt=%s\n",RTPGetErrorString(ERR_RTP_PACKBUILD_INVALIDPAYLOADTYPE).c_str());printf("notinit=%s\n",RTPGetErrorString(ERR_RTP_PACKBUILD_NOTINIT).c_str());}
