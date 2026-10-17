#pragma once

#include "MeshTypes.h"

namespace Baker {

class MikkTSpaceBridge {
public:
    // Calculates MikkTSpace tangent vectors for the provided LowPolyMesh
    static bool CalculateTangents(LowPolyMesh& mesh);
};

} // namespace Baker
