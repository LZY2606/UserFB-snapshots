#include <nanospline/BSplinePatch.h>
#include <nanospline/forward_declaration.h>
#include <cmath>
#include <iostream>
int main(){
 using C=nanospline::BSpline<double,3,-1>;
 C c; // Dynamic degree is inferred from knots and control count.
 Eigen::Matrix<double,5,3> cp;
 cp<<0,0,0, 1,0,1, 2,1,0, 3,1,-1, 4,0,0;
 Eigen::VectorXd k(9);k<<-2,-2,-2,-2,0,3,3,3,3;
 c.set_control_points(cp); c.set_knots(k);
 auto b=c.convert_to_Bezier(); auto e=c.elevate_degree();
 double err=0;
 for(int i=0;i<=32;++i){double t=-2+5.0*i/32;err=std::max(err,(c.evaluate(t)-e.evaluate(t)).norm());}
 nanospline::BSplinePatch<double,3,1,1> p;
 Eigen::Matrix<double,4,3> grid;grid<<0,0,0, 0,1,0, 1,0,0, 1,1,1;
 Eigen::Vector4d knots;knots<<0,0,1,1;
 p.set_control_grid(grid);p.set_knots_u(knots);p.set_knots_v(knots);p.initialize();
 auto pos=p.evaluate(.25,.75);auto iso=p.compute_iso_curve_u(.75);
 auto du=p.evaluate_derivative_u(.25,.75); auto dv=p.evaluate_derivative_v(.25,.75);
 if(std::get<0>(b).size()!=2 || err>1e-12 || (pos-iso.evaluate(.25)).norm()>1e-12 || std::abs(pos[2]-.1875)>1e-12 || std::abs(du[2]-.75)>1e-12 || std::abs(dv[2]-.25)>1e-12) return 1;
 std::cout<<"CURVE_PRODUCTION_OK pieces="<<std::get<0>(b).size()<<" elevation_error="<<err<<"\n";
 std::cout<<"PATCH_PRODUCTION_OK z="<<pos[2]<<" du_z="<<du[2]<<" dv_z="<<dv[2]<<"\n";
}
