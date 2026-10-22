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

    // Walks the topological edge loop across quad/polygon topology (Maya style double-click edge loop)
    static std::vector<std::pair<uint32_t, uint32_t>> GetEdgeLoop(const LowPolyMesh& mesh, uint32_t v0, uint32_t v1);

    // Selects an edge loop and updates mesh.selectedEdges
    static void SelectEdgeLoop(LowPolyMesh& mesh, uint32_t v0, uint32_t v1, bool addToSelection = false);

    // Cuts all currently selected edges into UV seams
    static void CutSelectedEdges(LowPolyMesh& mesh);

    // Sews (stitches) all currently selected edges, removing them from UV seams
    static void SewSelectedEdges(LowPolyMesh& mesh);

    // Clears the current edge selection
    static void ClearSelection(LowPolyMesh& mesh);

    // Selects all currently marked UV cut seams
    static void SelectAllSeams(LowPolyMesh& mesh);

    // Segment mesh triangles into connected islands bounded by seams
    static std::vector<std::vector<int>> SegmentIslands(const LowPolyMesh& mesh);

    // Regenerate seamLineIndices in LowPolyMesh for 3D/2D viewport GPU line rendering
    static void GenerateSeamLineIndices(LowPolyMesh& mesh);
};

} // namespace Baker
