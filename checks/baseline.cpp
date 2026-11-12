#include "msdfgen.h"
#include "core/ShapeDistanceFinder.h"
#include <cassert>
#include <cmath>
#include <iostream>
int main(){
 using namespace msdfgen;
 Shape s; Contour&c=s.addContour();
 c.addEdge(EdgeHolder(Point2(0,0),Point2(8,0)));
 c.addEdge(EdgeHolder(Point2(8,0),Point2(8,8)));
 c.addEdge(EdgeHolder(Point2(8,8),Point2(0,8)));
 c.addEdge(EdgeHolder(Point2(0,8),Point2(0,0)));
 assert(s.validate());
 SimpleTrueShapeDistanceFinder finder(s); double d=finder.distance(Point2(3,4)); assert(std::abs(std::abs(d)-3)<1e-9);
 edgeColoringSimple(s,3.0);
 Bitmap<float,1> sdf(8,8); Bitmap<float,4> mtsdf(8,8);
 Projection p(Vector2(1,1),Vector2(0,0));
 generateSDF(sdf,s,p,Range(4)); generateMTSDF(mtsdf,s,p,Range(4));
 for(int y=0;y<8;y++)for(int x=0;x<8;x++)assert(std::isfinite(*sdf(x,y))&&std::isfinite(mtsdf(x,y)[3]));
 std::cout<<"BASE_MSDFGEN square_distance=3 sdf+mtsdf=PASS\n";
}
