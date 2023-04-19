#pragma once

#include "cal3d/global.h"

#include <string>
#include <vector>

CAL3D_PTR(CalCoreSubmesh);

class CAL3D_API CalCompactSubmeshData {
public:
    std::string jsonData;
    std::string binaryData;
};

class CAL3D_API CalCompactMeshWriter {
public:
    static CalCompactSubmeshData saveCompactCoreSubmesh(const boost::shared_ptr<CalCoreSubmesh>&, unsigned int influenceLimit=4, float influenceMargin=0.001);
};
