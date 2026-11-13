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

    // Mean symmetric-Dirichlet distortion of the mesh UVs (area weighted, scale-invariant per island).
    // 1.0 = isometric, larger = more stretch/shear. Returns 0 for a mesh without usable UVs.
    static float MeasureDistortion(const LowPolyMesh& mesh);

    // Lowers that distortion in place with flip-safe per-vertex descent. Islands are the connected
    // triangle sets of the seam-split mesh and each keeps its own scale. Returns distortion after.
    static float RelaxUVs(LowPolyMesh& mesh, int iterations);

    // Translates each island so its min corner lands on a texel boundary of a resolution x resolution map.
    static void SnapIslandsToPixels(LowPolyMesh& mesh, int resolution);
};

} // namespace Baker
