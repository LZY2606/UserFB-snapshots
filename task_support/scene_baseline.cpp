#include "libs/yocto/yocto_sceneio.h"
#include "libs/yocto/yocto_bvh.h"
#include "libs/yocto/yocto_shape.h"
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace yocto;
static void check(bool b, const char* message) {if(!b) throw std::runtime_error(message);}
int main() {
  scene_data scene;
  subdiv_data cage;
  cage.shape=0; cage.smooth=true;
  cage.positions={{0,0,0},{1,0,0},{2,0,0},{0,1,0},{1,1,0},{2,1,0}};
  cage.quadspos={{0,1,4,3},{1,2,5,4}};
  cage.quadsnorm=cage.quadspos; cage.normals=vector<vec3f>(6,{0,0,1});
  cage.quadstexcoord={{0,1,2,3},{4,5,6,7}};
  cage.texcoords={{0,0},{0,0},{0,0},{0,0},{0.5f,0},{0.5f,0},{0.5f,0},{0.5f,0}};
  cage.displacement=1; cage.displacement_tex=0;
  texture_data texture; texture.width=2;texture.height=1;texture.linear=true;
  texture.pixelsf={{0.2f,0.2f,0.2f,0.2f},{0.6f,0.6f,0.6f,0.6f}};
  scene.textures={texture};scene.texture_names={"height"};
  scene.shapes.resize(1);scene.shape_names={"surface"};
  scene.subdivs={cage};scene.subdiv_names={"cage"};
  scene.materials.resize(1);scene.material_names={"material"};
  scene.instances={{identity3x4f,0,0},{translation_frame(vec3f{3,0,0}),0,0}};
  scene.instance_names={"left","right"};
  tesselate_subdivs(scene);
  auto& shape=scene.shapes[0];check(shape.quads.size()==2 && shape.positions.size()==8,"facevarying split");
  int seam_count=0;
  for(auto p:shape.positions) if(p.x==1){check(std::abs(p.z-0.4f)<1e-6f,"seam average");++seam_count;}
  check(seam_count==4,"seam split vertices");
  check(scene.subdivs[0].positions==cage.positions,"source cage changed");
  auto once=shape.positions;tesselate_subdivs(scene);check(scene.shapes[0].positions==once,"repeat generation");
  std::cout<<"BASELINE displacement before UV split and repeat generation PASS\n";
  auto bvh=make_scene_bvh(scene,false,true);
  auto hit=intersect_scene_bvh(bvh,scene,{{3.25f,0.25f,2},{0,0,-1}});
  check(hit.hit && hit.instance==1,"shared instance hit");
  check(eval_normal(scene,scene.instances[1],hit.element,hit.uv).z>0.9f,"scene normal");
  std::cout<<"BASELINE generated shape shared instances and BVH normal PASS\n";
  auto dirname=std::filesystem::path(".task-build/scene-roundtrip");
  std::filesystem::create_directories(dirname);
  auto filename=(dirname/"scene.json").string();std::string error;
  check(make_scene_directories(filename,scene,error),error.c_str());
  check(save_scene(filename,scene,error,true),error.c_str());
  auto loaded=scene_data{};
  auto roundtrip_ok=load_scene(filename,loaded,error,true);
  std::cout<<"WITNESS current textured subdiv roundtrip rejected="<<!roundtrip_ok<<"\n";
  // Exercise the independent legacy path regardless of whether the new work repairs the reference.
  scene.subdivs[0].displacement_tex=invalidid;
  check(save_scene(filename,scene,error,true),error.c_str());
  loaded={};check(load_scene(filename,loaded,error,true),error.c_str());
  check(loaded.subdivs.size()==1 && loaded.instances.size()==2,"asset counts");
  check(loaded.instances[0].shape==loaded.instances[1].shape,"shared shape reference");
  check(loaded.subdivs[0].positions==cage.positions,"OBJ cage values");
  check(loaded.subdivs[0].quadspos==cage.quadspos,"OBJ cage topology");
  tesselate_subdivs(loaded);
  check(loaded.shapes[0].quads.size()==2,"loaded generation");
  check(loaded.shapes[0].positions.size()==8,"loaded UV seam");
  std::cout<<"BASELINE JSON OBJ texture roundtrip and regenerated mesh PASS\n";
  for(bool cc:{false,true}) {scene.subdivs[0].subdivisions=2;scene.subdivs[0].catmullclark=cc;tesselate_subdivs(scene);check(scene.shapes[0].quads.size()==32,"subdivision count");}
  std::cout<<"BASELINE linear and CatmullClark two levels PASS\n";
}
