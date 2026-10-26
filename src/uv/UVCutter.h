#pragma once

#include "UVTypes.h"
#include <DirectXMath.h>
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

    // Build map from vertex index to unique geometric vertex ID (welding identical 3D positions across UV seams)
    static std::vector<uint32_t> BuildGeomVertexMap(const LowPolyMesh& mesh);

    // Interactively toggle a seam on an edge between vertex v0 and v1
    static void ToggleEdgeSeam(LowPolyMesh& mesh, uint32_t v0, uint32_t v1);

    // Walks the topological edge loop connecting end-to-end through vertices (Maya style double-click loop)
    static std::vector<std::pair<uint32_t, uint32_t>> GetEdgeLoop(const LowPolyMesh& mesh, uint32_t v0, uint32_t v1);

    // Selects an edge loop and updates mesh.selectedEdges
    static void SelectEdgeLoop(LowPolyMesh& mesh, uint32_t v0, uint32_t v1, bool addToSelection = false);

    // Walks parallel opposite edges across quad faces (Edge Ring)
    static std::vector<std::pair<uint32_t, uint32_t>> GetEdgeRing(const LowPolyMesh& mesh, uint32_t v0, uint32_t v1);

    // Selects an edge ring and updates mesh.selectedEdges
    static void SelectEdgeRing(LowPolyMesh& mesh, uint32_t v0, uint32_t v1, bool addToSelection = false);

    // Finds the shortest topological edge path connecting start edge to end edge (Maya Shift+Click shortest path)
    static std::vector<std::pair<uint32_t, uint32_t>> GetShortestPath(const LowPolyMesh& mesh,
                                                                      uint32_t startV0, uint32_t startV1,
                                                                      uint32_t endV0, uint32_t endV1);

    // Selects the shortest path of edges between two edges (Maya Shift+Click between edges)
    static void SelectShortestPath(LowPolyMesh& mesh,
                                   uint32_t startV0, uint32_t startV1,
                                   uint32_t endV0, uint32_t endV1,
                                   bool addToSelection = true);

    // Projects vertex positions from camera view-projection matrix to UV coordinates [0, 1]
    static void CameraProjectUVs(LowPolyMesh& mesh,
                                 const DirectX::XMMATRIX& viewProjMatrix);

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

    // Generate clean closed UV seams based on normal clustering charts (Smart UV Charts)
    static void GenerateSmartSeams(LowPolyMesh& mesh, float angleThresholdDeg);

    // Splits and duplicates mesh vertices along UV cut seams so each island has independent UV coordinates
    static void SplitMeshAlongSeams(LowPolyMesh& mesh, const std::vector<std::vector<int>>& islandsTris);

    // Regenerate seamLineIndices in LowPolyMesh for 3D/2D viewport GPU line rendering
    static void GenerateSeamLineIndices(LowPolyMesh& mesh);
};

} // namespace Baker
