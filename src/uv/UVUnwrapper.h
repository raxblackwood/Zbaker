#pragma once

#include "UVTypes.h"
#include <vector>

namespace Baker {

class UVUnwrapper {
public:
    // Unwraps all islands extracted from LowPolyMesh, updating their 2D UV charts
    static std::vector<UVIsland> UnwrapMesh(LowPolyMesh& mesh, const std::vector<std::vector<int>>& islandTris);

    // Unwraps a single island given its triangle list
    static UVIsland UnwrapIsland(const LowPolyMesh& mesh, const std::vector<int>& triIndices, int islandId);
};

} // namespace Baker
