#include "agg_path_storage.h"
#include "agg_conv_curve.h"
#include "agg_conv_transform.h"
#include "agg_trans_perspective.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main() {
 agg::path_storage path; path.move_to(1,1); path.curve4(1,8,8,8,8,1); path.close_polygon();
 agg::conv_curve<agg::path_storage> flat(path); flat.approximation_scale(2);
 agg::trans_perspective m(1,0,.02,0,1,.01,0,0,1);
 agg::conv_transform<decltype(flat),agg::trans_perspective> output(flat,m);
 output.rewind(0); double x,y; unsigned cmd,n=0; bool closed=false;
 while(!agg::is_stop(cmd=output.vertex(&x,&y))) {
  if(agg::is_vertex(cmd)) { assert(std::isfinite(x)&&std::isfinite(y)); ++n; }
  if(agg::is_end_poly(cmd)) closed=agg::is_close(cmd);
 }
 assert(n>8 && closed);
 std::cout<<"BASE_AGG_PATH curve_vertices="<<n<<" closed=1\n";
}
