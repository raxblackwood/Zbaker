#pragma once

#include "../baker/MeshTypes.h"
#include "UVTypes.h"
#include <DirectXMath.h>
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

    // Dense surface-following points contoured to the mesh surface along camera perspective
    std::vector<Vec3> sampledPath;

    void AddPoint(const Vec3& pos, const Vec3& norm, int triIdx);
    void AddSegmentPath(const std::vector<Vec3>& segmentSurfacePoints);
    void MovePoint(size_t index, const Vec3& newPos, const Vec3& newNorm, int newTriIdx);
    void RemovePoint(size_t index);
    void Clear();
    void UpdateSampledPath(const LowPolyMesh& mesh,
                           const DirectX::XMMATRIX* worldMat = nullptr,
                           const DirectX::XMMATRIX* viewProjMat = nullptr,
                           int vpWidth = 0, int vpHeight = 0);
};

class CurveManager {
public:
    std::vector<SurfaceCurve> curves;
    int activeCurveIndex = -1;
    int selectedPointIndex = -1;
    int hoveredPointIndex = -1;
    bool isDrawing = false;
    bool autoCutSeams = true; // Auto-cut seams on mesh as curve points are placed
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
    void UpdateActiveSampledPath(const LowPolyMesh& mesh,
                                 const DirectX::XMMATRIX* worldMat = nullptr,
                                 const DirectX::XMMATRIX* viewProjMat = nullptr,
                                 int vpWidth = 0, int vpHeight = 0);

    // Samples dense 3D points on the mesh surface along the 2D screen line between start and end as seen from camera
    static std::vector<Vec3> SampleCameraSurfacePath(
        const LowPolyMesh& mesh,
        const DirectX::XMMATRIX& worldMatrix,
        const DirectX::XMMATRIX& viewProjMatrix,
        int vpWidth, int vpHeight,
        const Vec3& startLocal,
        const Vec3& endLocal,
        int numSamples = 36
    );

    // 1. Snaps curve directly to closest low-poly mesh edges via geometric Dijkstra shortest surface edge path
    static bool SnapCurveToMeshEdges(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outSeamEdges);

    // 2. Cuts curve directly into mesh topology by bisecting/splitting triangles along curve trajectory
    static bool CutCurveIntoMeshTopology(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outNewSeamEdges);

    // 3. Unified bake function to convert curve(s) into UV seams
    static bool BakeCurvesToSeams(CurveManager& mgr, LowPolyMesh& mesh, bool cutIntoTopology, int curveIndex = -1);
};

} // namespace Baker
