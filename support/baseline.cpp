#define main wfc_upstream_main
#include "main.cpp"
#undef main
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
int main(){
 const Palette pal={{0,0,0,255},{255,255,255,255}};
 PalettedImage sample{4,4,{},pal};for(int y=0;y<4;++y)for(int x=0;x<4;++x)sample.data.push_back((x+y)%2);
 auto patterns=extract_patterns(sample,2,true,1,nullptr);require(patterns.size()==2,"two checker patterns");
 OverlappingModel model(patterns,pal,2,true,6,6,kInvalidHash);
 auto output=create_output(model);auto result=run(&output,model,42,0,nullptr);require(result==Result::kSuccess,"checker solve");
 auto image=image_from_graphics(model.graphics(output),pal);int white=0;
 for(int y=0;y<6;++y)for(int x=0;x<6;++x){auto c=image.get(x,y);white+=c.r==255;require(c==pal[0]||c==pal[1],"palette");require(!(c==image.get((x+1)%6,y)),"horizontal overlap");require(!(c==image.get(x,(y+1)%6)),"vertical overlap");}
 require(white==18,"physical pixel count");std::cout<<"OVERLAP_TORUS_OK patterns=2 pixels=36 white=18\n";
 OverlappingModel planar(patterns,pal,2,false,5,4,kInvalidHash);auto edge=create_output(planar);require(run(&edge,planar,42,0,nullptr)==Result::kSuccess,"planar solve");
 auto graphics=planar.graphics(edge);for(int y=0;y<4;++y)for(int x=0;x<5;++x){auto c=graphics.ref(x,y);require(!c.empty(),"edge coverage");for(auto t:c)require(t==c.front(),"overlap agreement");}
 std::cout<<"PLANAR_EDGE_COVERAGE_OK pixels=20\n";
 OverlappingModel odd(patterns,pal,2,true,5,5,kInvalidHash);auto bad=create_output(odd);require(run(&bad,odd,42,0,nullptr)==Result::kFail,"odd torus contradiction");
 std::cout<<"ODD_TORUS_CONTRADICTION_OK\nWFC_BASELINE_OK\n";
}
