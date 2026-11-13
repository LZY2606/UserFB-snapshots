#include "spiroentrypoints.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
 spiro_cp p[]={{0,0,'{'},{50,80,'c'},{100,0,'}'}};
 ls_bezctx* b=new_ls_bezctx(128,SPIRO_INTERNAL_BEZCTX); assert(b);
 int ok=SpiroCPsToBezier2(p,3,SPIRO_INTERNAL_BEZCTX|SPIRO_INCLUDE_LAST_KNOT,0,&b->base);
 assert(ok==1&&b->l>3);int curves=0;
 for(int i=0;i<b->l;i++)if(b->cd[i].ty=='c'){assert(std::isfinite(b->cd[i].x3));curves++;}
 assert(curves>0);std::cout<<"BASE_SPIRO solver=PASS cubic_count="<<curves<<"\n";
 free_ls_bezctx(b);
}
