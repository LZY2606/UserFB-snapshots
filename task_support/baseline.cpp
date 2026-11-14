#include "fixtures.h"
#include "egModules.h"
#include "egScene.h"
#include "egModel.h"
#include "egCom.h"
#include <iostream>
using namespace Horde3D;
static bool near(float a,float b) {return std::isfinite(a)&&std::abs(a-b)<3e-4f;}
#define CHECK(x) do { if(!(x)) { std::cerr<<"BASELINE_FAIL line="<<__LINE__<<" "<<#x<<"\n"; return 1; } ++checks; }while(0)
static int exercise() {
    int checks=0;
    CHECK(h3dInit(static_cast<H3DRenderDevice::List>(256)));
    h3dSetOption(H3DOptions::FastAnimation,0);
    H3DRes a=motion_fixture::load(H3DResTypes::Animation,"synthetic.anim",motion_fixture::animation());
    H3DRes g=motion_fixture::load(H3DResTypes::Geometry,"synthetic.geo",motion_fixture::geometry());
    CHECK(a!=0&&g!=0);
    H3DNode parent=h3dAddGroupNode(H3DRootNode,"placement");
    h3dSetNodeTransform(parent,7,0,-3,0,30,0,1,1,1);
    H3DNode model=h3dAddModelNode(parent,"character",g);
    h3dSetNodeTransform(model,2,0,1,0,0,0,1,1,1);
    h3dSetNodeParamI(model,H3DModel::SWSkinningI,1);
    H3DNode root=h3dAddJointNode(model,"root",0);
    H3DNode limb=h3dAddJointNode(root,"limb",1);
    H3DNode constant=h3dAddJointNode(limb,"constant",2);
    H3DNode rest=h3dAddJointNode(root,"rest",3);
    h3dSetNodeTransform(rest,0,0,2,0,0,0,1,1,1);
    H3DRes material=h3dAddResource(H3DResTypes::Material,"unrendered.material",0);
    CHECK(h3dAddMeshNode(model,"triangle",material,H3DMeshPrimType::TriangleList,0,3,0,2)!=0);
    CHECK(model&&root&&limb&&constant&&rest);
    h3dSetupModelAnimStage(model,0,a,0,"",false);
    h3dSetModelMorpher(model,"lift",.25f);
    auto *m=static_cast<ModelNode*>(Modules::sceneMan().resolveNodeHandle(model));
    const float xyz[3][3]={{2,2,0},{1,3,0},{1,2,1}};
    for(float frame: {0.f,.5f,1.5f}) {
        Modules::sceneMan().updateNodes();
        h3dSetModelAnimParams(model,0,frame,1);
        h3dUpdateModel(model,H3DModelUpdateFlags::Animation|H3DModelUpdateFlags::Geometry);
        const float *rmat=nullptr; h3dGetNodeTransMats(limb,&rmat,nullptr);
        CHECK(near(rmat[13],2+2*frame));
        h3dGetNodeTransMats(constant,&rmat,nullptr); CHECK(near(rmat[14],1));
        h3dGetNodeTransMats(rest,&rmat,nullptr); CHECK(near(rmat[14],2));
        auto *geo=m->getGeometryResource();
        const float yaw=frame*1.5707963267948966f;
        const float cp=std::cos(.5235987755982988f),sp=std::sin(.5235987755982988f);
        for(int v=0;v<3;++v) {
            float x=xyz[v][0]-1,z=xyz[v][2];
            float localx=1+2*frame+std::cos(yaw)*x+std::sin(yaw)*z;
            float localz=-std::sin(yaw)*x+std::cos(yaw)*z;
            float y=xyz[v][1]+(127.f/255.f)*2*frame+(v==0?.25f:0);
            Vec3f actual=geo->getVertPosData()[v];
            CHECK(near(actual.x,localx)&&near(actual.y,y)&&near(actual.z,localz));
            Vec3f world=m->getAbsTrans()*actual;
            if(!near(world.x,7+cp*(localx+2)+sp*(localz+1))) std::cerr<<"WORLD frame="<<frame<<" v="<<v<<" got="<<world.x<<","<<world.y<<","<<world.z<<" expected="<<7+cp*(localx+2)+sp*(localz+1)<<","<<y<<","<<-3-sp*(localx+2)+cp*(localz+1)<<" modeltx="<<m->getAbsTrans().x[12]<<"\n";
            CHECK(near(world.x,7+cp*(localx+2)+sp*(localz+1)));
            CHECK(near(world.y,y)&&near(world.z,-3-sp*(localx+2)+cp*(localz+1)));
        }
    }
    CHECK(!h3dGetError());
    h3dRelease();
    std::cout<<"HORDE_MOTION_BASELINE_OK checks="<<checks<<"\n";
    return 0;
}
int main(){return exercise();}
