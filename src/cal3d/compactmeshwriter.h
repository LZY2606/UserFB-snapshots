#pragma once

#include "cal3d/global.h"

#include <string>
#include <vector>

CAL3D_PTR(CalCoreSubmesh);

class CAL3D_API CalCompactMeshWriter {
public:
    static std::pair<std::string, std::string> saveCompactCoreSubmesh(const CalCoreSubmeshPtr&, unsigned int influenceLimit=4, float influenceMargin=0.001);
};
