#pragma once
#include "dragonBones/DragonBonesHeaders.h"
#include <cassert>
using namespace dragonBones;
struct HeadlessProxy: IArmatureProxy {
 Armature* arm=nullptr;
 bool hasDBEventListener(const std::string&) const override{return false;}
 void dispatchDBEvent(const std::string&,EventObject*) override{}
 void addDBEventListener(const std::string&,const std::function<void(EventObject*)>&) override{}
 void removeDBEventListener(const std::string&,const std::function<void(EventObject*)>&) override{}
 void dbInit(Armature* a) override{arm=a;}void dbClear() override{}void dbUpdate() override{}
 void dispose(bool) override{}Armature* getArmature() const override{return arm;}
 Animation* getAnimation() const override{return arm->getAnimation();}
};
