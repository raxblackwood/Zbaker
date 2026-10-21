#pragma once

#include "UVTypes.h"
#include <vector>
#include <set>
#include <unordered_map>
#include <utility>

namespace Baker {

class UVCutter {
public:
    // Mark seams along edges where dihedral angle between triangle face normals exceeds angleThresholdDeg
    static void MarkSeamsByAngle(LowPolyMesh& mesh, float angleThresholdDeg);

    // Mark seams along edges that separate different polygroups or mesh subtool parts
    static void MarkSeamsByPolygroup(LowPolyMesh& mesh);

    // Mark seams along existing UV boundary edges (where UV coordinates split across shared 3D positions)
    static void MarkSeamsByUVBorders(LowPolyMesh& mesh);

    // Clear all marked seams
    static void ClearSeams(LowPolyMesh& mesh);

    // Interactively toggle a seam on an edge between vertex v0 and v1
    static void ToggleEdgeSeam(LowPolyMesh& mesh, uint32_t v0, uint32_t v1);

    // Segment mesh triangles into connected islands bounded by seams
    static std::vector<std::vector<int>> SegmentIslands(const LowPolyMesh& mesh);

    // Regenerate seamLineIndices in LowPolyMesh for 3D/2D viewport GPU line rendering
    static void GenerateSeamLineIndices(LowPolyMesh& mesh);
};

} // namespace Baker
