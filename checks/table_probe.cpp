#include <cmath>
#include <cstdio>
namespace data { struct mat33 {double m[9];};
#include "task_data/ltc.inc"
}
int main(){
 if(data::size!=64)return 1;
 for(int i=0;i<4096;i++){
  for(int j=0;j<9;j++)if(!std::isfinite(data::tabM[i].m[j])||!std::isfinite(data::tabMinv[i].m[j]))return 2;
  if(!std::isfinite(data::tabMagnitude[i])||data::tabMagnitude[i]<0)return 3;
 }
 if(data::tabMinv[32].m[0]==data::tabMinv[32+32*64].m[0])return 4;
 puts("LTC_DATA_OK entries=4096 column_major=source_export");return 0;
}
