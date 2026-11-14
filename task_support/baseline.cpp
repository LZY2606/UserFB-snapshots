// Octahedron and decimate checks adapted from an earlier public scaffold.
// Extended only with existing 5D qslim to document the starting capability.
#include <igl/decimate.h>
#include <igl/qslim.h>
#include <cassert>
#include <iostream>
int main() {
  Eigen::MatrixXd v(6,3); v<<1,0,0,-1,0,0,0,1,0,0,-1,0,0,0,1,0,0,-1;
  Eigen::MatrixXi f(8,3); f<<0,2,4,2,1,4,1,3,4,3,0,4,2,0,5,1,2,5,3,1,5,0,3,5;
  Eigen::MatrixXd u; Eigen::MatrixXi g; Eigen::VectorXi j,i;
  bool ok=igl::decimate(v,f,6,false,u,g,j,i);
  assert(ok && g.rows()==6 && u.rows()==5 && j.size()==6 && i.size()==5);
  assert(g.minCoeff()>=0 && g.maxCoeff()<u.rows() && u.allFinite());
  for(int k=0;k<j.size();++k) assert(j(k)>=0 && j(k)<8);
  std::cout<<"LIBIGL_DECIMATE_OK vertices=5 faces=6\n";
  ok=igl::qslim(v,f,6,false,u,g,j,i);
  assert(ok && u.cols()==3 && g.rows()==6 && u.allFinite());
  Eigen::MatrixXd a(6,5);a.leftCols(3)=v;
  a.col(3)<<0.1,0.7,0.2,0.3,0.9,0.6;
  a.col(4)<<0.4,0.2,0.9,0.3,0.8,0.1;
  ok=igl::qslim(a,f,6,false,u,g,j,i);
  assert(ok && u.cols()==5 && u.allFinite() && g.rows()==6);
  assert(g.minCoeff()>=0 && g.maxCoeff()<u.rows());
  for(int k=0;k<i.size();++k) assert(i(k)>=0 && i(k)<6);
  for(int k=0;k<j.size();++k) assert(j(k)>=0 && j(k)<8);
  std::cout<<"LIBIGL_QSLIM_CURRENT_OK xyz=3 augmented=5 faces=6\n";
}
