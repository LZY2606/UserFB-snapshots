#include "Solver.h"
#include "Constraint.h"
#include "Force.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
using namespace ShapeOp;
void check(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main(){
 Matrix3X p(3,2);p<<0,2,0,0,0,0;
 Solver s;s.setPoints(p);
 s.addConstraint(std::make_shared<EdgeStrainConstraint>(std::vector<int>{0,1},4,p));
 s.addConstraint(std::make_shared<ClosenessConstraint>(std::vector<int>{0},100,p));
 check(s.initialize(),"initialize static");
 p(0,1)=4;s.setPoints(p);check(s.solve(20),"solve static");
 check(std::abs(s.getPoints()(0,0))<1e-9,"anchor");
 check(std::abs(s.getPoints()(0,1)-2)<1e-9,"edge projection");
 Matrix3X d(3,1);d<<1,1,0;
 Solver dyn;dyn.setPoints(d);
 dyn.addConstraint(std::make_shared<ClosenessConstraint>(std::vector<int>{0},4,d));
 dyn.addForces(std::make_shared<GravityForce>(Vector3(0,-8,0)));
 check(dyn.initialize(true,2,1,.25),"initialize dynamic");
 check(dyn.solve(3),"solve dynamic");
 check(std::abs(dyn.getPoints()(1,0)-5.0/9)<1e-9,"coupled momentum and projection");
 check(std::abs(dyn.getPoints()(0,0)-1)<1e-9,"x remains");
 std::cout<<"SHAPEOP_CURRENT_OK edge=2 dynamicY=0.5555555556\n";
}
