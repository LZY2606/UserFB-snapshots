#include "tinysplinecxx.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace tinyspline;
int main(){
 BSpline rail(3,3,2,BSpline::Clamped);
 rail.setControlPoints({1,0,0, 1,1,0, 0,1,0});
 auto p=rail.eval(.5).result();
 assert(std::fabs(p[0]-.75)<1e-12 && std::fabs(p[1]-.75)<1e-12);
 auto refined=rail.insertKnot(.37,1);
 auto q=refined.eval(.5).result();
 assert(std::fabs(q[0]-p[0])<1e-12 && std::fabs(q[1]-p[1])<1e-12);
 auto restored=BSpline::parseJson(rail.toJson());
 auto r=restored.eval(.5).result(); assert(std::fabs(r[0]-p[0])<1e-12);
 auto frames=rail.computeRMF({0,.2,.5,.8,1});
 auto tangent=frames.at(2).tangent();
 assert(std::isfinite(tangent.x()) && std::isfinite(tangent.y()));
 std::cout<<"BASELINE_POLYNOMIAL midpoint="<<p[0]<<","<<p[1]<<" radius="<<std::hypot(p[0],p[1])<<"\n";
 std::cout<<"BASELINE_OK ordinary_edit_json_rmf\n";
}
