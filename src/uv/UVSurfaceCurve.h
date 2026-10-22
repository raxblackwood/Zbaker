#pragma once

#include "../baker/MeshTypes.h"
#include "UVTypes.h"
#include <vector>
#include <string>
#include <utility>

namespace Baker {

struct CurvePoint {
    Vec3 position;      // Local 3D coordinate on low-poly mesh surface
    Vec3 normal;        // Surface normal at point
    int triIndex = -1;  // Triangle index hit on low-poly mesh
    float u = 0.0f;     // Barycentric u coordinate in triangle
    float v = 0.0f;     // Barycentric v coordinate in triangle

    CurvePoint() = default;
    CurvePoint(const Vec3& p, const Vec3& n, int tri = -1, float _u = 0.0f, float _v = 0.0f)
        : position(p), normal(n), triIndex(tri), u(_u), v(_v) {}
};

struct SurfaceCurve {
    int id = 1;
    std::string name = "Curve 1";
    std::vector<CurvePoint> points;
    bool isClosed = false;
    bool isBaked = false;

    // Interpolated points along mesh surface for smooth visualization
    std::vector<Vec3> sampledPath;

    void AddPoint(const Vec3& pos, const Vec3& norm, int triIdx);
    void MovePoint(size_t index, const Vec3& newPos, const Vec3& newNorm, int newTriIdx);
    void RemovePoint(size_t index);
    void Clear();
    void UpdateSampledPath(const LowPolyMesh& mesh);
};

class CurveManager {
public:
    std::vector<SurfaceCurve> curves;
    int activeCurveIndex = -1;
    int selectedPointIndex = -1;
    int hoveredPointIndex = -1;
    bool isDrawing = false;
    int nextCurveId = 1;

    CurveManager();

    int AddCurve(const std::string& name = "");
    void RemoveCurve(int index);
    void ClearAll();

    SurfaceCurve* GetActiveCurve();
    const SurfaceCurve* GetActiveCurve() const;

    void AddPointToActive(const Vec3& pos, const Vec3& normal, int triIdx);
    void MoveActivePoint(int pointIdx, const Vec3& newPos, const Vec3& newNormal, int newTriIdx);
    void RemoveActivePoint(int pointIdx);
    void UpdateActiveSampledPath(const LowPolyMesh& mesh);

    // 1. Snaps curve directly to closest low-poly mesh edges via Dijkstra shortest surface edge path
    static bool SnapCurveToMeshEdges(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outSeamEdges);

    // 2. Cuts curve directly into mesh topology by bisecting/splitting triangles along curve trajectory
    static bool CutCurveIntoMeshTopology(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outNewSeamEdges);

    // 3. Unified bake function to convert curve(s) into UV seams
    static bool BakeCurvesToSeams(CurveManager& mgr, LowPolyMesh& mesh, bool cutIntoTopology, int curveIndex = -1);
};

} // namespace Baker
