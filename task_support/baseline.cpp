#include "fixture.h"
#include "pelib/ResourceDirectory.h"
#include <iostream>
int main(){auto b=resource_image();std::istringstream in(image_string(b));auto h=read_header(in);PeLib::ResourceDirectoryT<32>d;
 if(d.read(in,h)!=PeLib::ERROR_NONE)return 1;
 auto root=d.getRoot();if(root->getNumberOfChildren()!=1)return 2;
 auto type=root->getChild(0)->getNode();auto rn=dynamic_cast<PeLib::ResourceNode*>(type);if(!rn||rn->getNumberOfChildren()!=1)return 3;
 auto name=dynamic_cast<PeLib::ResourceNode*>(rn->getChild(0)->getNode());if(!name)return 4;
 auto leaf=dynamic_cast<PeLib::ResourceLeaf*>(name->getChild(0)->getNode());if(!leaf)return 5;
 auto data=leaf->getData();if(std::string(data.begin(),data.end())!="RSRC")return 6;
 std::cout<<"BASELINE_OK resource=RSRC root_children=1 leaf_rva="<<std::hex<<leaf->getElementRva()<<" occupied="<<d.getOccupiedAddresses().size()<<"\n";
}
