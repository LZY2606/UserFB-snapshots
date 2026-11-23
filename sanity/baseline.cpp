#include "pmp/algorithms/fairing.h"
#include "pmp/algorithms/laplace.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace pmp;
int main() {
 SurfaceMesh m;
 auto c=m.add_vertex(Point(0.13,0.07,1));
 auto a=m.add_vertex(Point(-1,-1,0)),b=m.add_vertex(Point(1,-1,0));
 auto d=m.add_vertex(Point(1,1,0)),e=m.add_vertex(Point(-1,1,0));
 m.add_triangle(c,a,b);m.add_triangle(c,b,d);
 m.add_triangle(c,d,e);m.add_triangle(c,e,a);
 auto before=m.position(c); auto boundary=m.position(a);
 SparseMatrix L;laplace_matrix(m,L,true);
 if(L.rows()!=5 || (L*Eigen::VectorXd::Ones(5)).norm()>1e-10)
  throw std::runtime_error("rest Laplacian inconsistent");
 fair(m,1);
 if(std::abs(m.position(c)[2])>1e-8 || norm(m.position(a)-boundary)>1e-10)
  throw std::runtime_error("fairing path changed");
 std::cout<<"PMP_FAIR_BASELINE vertices="<<m.n_vertices()<<" faces="<<m.n_faces()
          <<" center_z_before="<<before[2]<<" center_z_after="<<std::abs(m.position(c)[2])<<"\n";
 SparseMatrix A=-L;
 DenseMatrix B=DenseMatrix::Zero(5,3),C;
 coordinates_to_matrix(m,C);C.col(2).setConstant(2.0);
 auto X=cholesky_solve(A,B,[](unsigned int i){return i!=0;},C);
 if((X.col(2).array()-2.0).abs().maxCoeff()>1e-8)
  throw std::runtime_error("Dirichlet solve failed");
 std::cout<<"PMP_DIRICHLET_BASELINE rows="<<X.rows()<<" constrained=4 free=1 z=2\n";
}
