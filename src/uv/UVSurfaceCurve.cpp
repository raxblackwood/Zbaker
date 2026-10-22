#include "UVSurfaceCurve.h"
#include "UVCutter.h"
#include <queue>
#include <unordered_map>
#include <map>
#include <cmath>
#include <algorithm>

namespace Baker {

namespace {

inline std::pair<uint32_t, uint32_t> MakeEdgeKey(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

// Distance between two 3D vectors
inline float Dist3D(const Vec3& a, const Vec3& b) {
    return (a - b).Length();
}

} // namespace

void SurfaceCurve::AddPoint(const Vec3& pos, const Vec3& norm, int triIdx) {
    points.emplace_back(pos, norm, triIdx);
    isBaked = false;
}

void SurfaceCurve::MovePoint(size_t index, const Vec3& newPos, const Vec3& newNorm, int newTriIdx) {
    if (index < points.size()) {
        points[index].position = newPos;
        points[index].normal = newNorm;
        if (newTriIdx >= 0) points[index].triIndex = newTriIdx;
        isBaked = false;
    }
}

void SurfaceCurve::RemovePoint(size_t index) {
    if (index < points.size()) {
        points.erase(points.begin() + index);
        isBaked = false;
    }
}

void SurfaceCurve::Clear() {
    points.clear();
    sampledPath.clear();
    isBaked = false;
}

void SurfaceCurve::UpdateSampledPath(const LowPolyMesh& mesh) {
    sampledPath.clear();
    if (points.empty()) return;
    if (points.size() == 1) {
        sampledPath.push_back(points[0].position);
        return;
    }

    size_t count = points.size();
    size_t segCount = isClosed ? count : (count - 1);

    for (size_t i = 0; i < segCount; ++i) {
        const Vec3& p0 = points[i].position;
        const Vec3& p1 = points[(i + 1) % count].position;

        // Sample 12 intermediate points per segment with slight surface offset
        int numSamples = 12;
        for (int s = 0; s < numSamples; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(numSamples);
            Vec3 pInter = p0 * (1.0f - t) + p1 * t;

            // Optional: elevate slightly above surface along normal to avoid Z-fighting in 3D view
            Vec3 nInter = (points[i].normal * (1.0f - t) + points[(i + 1) % count].normal * t).Normalized();
            sampledPath.push_back(pInter + nInter * 0.001f);
        }
    }
    sampledPath.push_back(points[segCount % count].position);
}

CurveManager::CurveManager() {
    AddCurve("Curve 1");
}

int CurveManager::AddCurve(const std::string& name) {
    SurfaceCurve curve;
    curve.id = nextCurveId++;
    curve.name = name.empty() ? ("Curve " + std::to_string(curve.id)) : name;
    curves.push_back(curve);
    activeCurveIndex = static_cast<int>(curves.size()) - 1;
    selectedPointIndex = -1;
    hoveredPointIndex = -1;
    return activeCurveIndex;
}

void CurveManager::RemoveCurve(int index) {
    if (index >= 0 && index < static_cast<int>(curves.size())) {
        curves.erase(curves.begin() + index);
        if (activeCurveIndex >= static_cast<int>(curves.size())) {
            activeCurveIndex = static_cast<int>(curves.size()) - 1;
        }
        selectedPointIndex = -1;
        hoveredPointIndex = -1;
    }
    if (curves.empty()) {
        AddCurve("Curve 1");
    }
}

void CurveManager::ClearAll() {
    curves.clear();
    activeCurveIndex = -1;
    selectedPointIndex = -1;
    hoveredPointIndex = -1;
    isDrawing = false;
    AddCurve("Curve 1");
}

SurfaceCurve* CurveManager::GetActiveCurve() {
    if (activeCurveIndex >= 0 && activeCurveIndex < static_cast<int>(curves.size())) {
        return &curves[activeCurveIndex];
    }
    return nullptr;
}

const SurfaceCurve* CurveManager::GetActiveCurve() const {
    if (activeCurveIndex >= 0 && activeCurveIndex < static_cast<int>(curves.size())) {
        return &curves[activeCurveIndex];
    }
    return nullptr;
}

void CurveManager::AddPointToActive(const Vec3& pos, const Vec3& normal, int triIdx) {
    SurfaceCurve* c = GetActiveCurve();
    if (!c) {
        AddCurve();
        c = GetActiveCurve();
    }
    if (c) {
        c->AddPoint(pos, normal, triIdx);
        selectedPointIndex = static_cast<int>(c->points.size()) - 1;
    }
}

void CurveManager::MoveActivePoint(int pointIdx, const Vec3& newPos, const Vec3& newNormal, int newTriIdx) {
    SurfaceCurve* c = GetActiveCurve();
    if (c && pointIdx >= 0 && pointIdx < static_cast<int>(c->points.size())) {
        c->MovePoint(pointIdx, newPos, newNormal, newTriIdx);
    }
}

void CurveManager::RemoveActivePoint(int pointIdx) {
    SurfaceCurve* c = GetActiveCurve();
    if (c && pointIdx >= 0 && pointIdx < static_cast<int>(c->points.size())) {
        c->RemovePoint(pointIdx);
        if (selectedPointIndex >= static_cast<int>(c->points.size())) {
            selectedPointIndex = static_cast<int>(c->points.size()) - 1;
        }
    }
}

void CurveManager::UpdateActiveSampledPath(const LowPolyMesh& mesh) {
    SurfaceCurve* c = GetActiveCurve();
    if (c) {
        c->UpdateSampledPath(mesh);
    }
}

// 1. Snaps curve directly to closest low-poly mesh edges using Dijkstra shortest surface edge path
bool CurveManager::SnapCurveToMeshEdges(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outSeamEdges) {
    if (curve.points.size() < 2 || mesh.vertices.empty() || mesh.triangles.empty()) return false;

    // 1. Build adjacency graph for mesh vertices based on triangle edges
    size_t vertCount = mesh.vertices.size();
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(vertCount);

    for (const auto& tri : mesh.triangles) {
        for (int k = 0; k < 3; ++k) {
            uint32_t u = tri.indices[k];
            uint32_t v = tri.indices[(k + 1) % 3];
            float w = Dist3D(mesh.vertices[u].position, mesh.vertices[v].position);
            adj[u].emplace_back(v, w);
            adj[v].emplace_back(u, w);
        }
    }

    // 2. Find closest mesh vertex for each curve control point
    std::vector<uint32_t> closestVerts(curve.points.size());
    for (size_t i = 0; i < curve.points.size(); ++i) {
        const auto& cp = curve.points[i];
        uint32_t bestV = 0;
        float bestDist = 1e30f;

        // If CP has a valid triangle hit, first check the 3 vertices of that triangle
        if (cp.triIndex >= 0 && cp.triIndex < static_cast<int>(mesh.triangles.size())) {
            const auto& tri = mesh.triangles[cp.triIndex];
            for (int k = 0; k < 3; ++k) {
                float d = Dist3D(mesh.vertices[tri.indices[k]].position, cp.position);
                if (d < bestDist) {
                    bestDist = d;
                    bestV = tri.indices[k];
                }
            }
        } else {
            // Full search
            for (size_t v = 0; v < vertCount; ++v) {
                float d = Dist3D(mesh.vertices[v].position, cp.position);
                if (d < bestDist) {
                    bestDist = d;
                    bestV = static_cast<uint32_t>(v);
                }
            }
        }
        closestVerts[i] = bestV;
    }

    // 3. Run Dijkstra for each sequential pair of vertices
    size_t count = curve.points.size();
    size_t segCount = curve.isClosed ? count : (count - 1);

    for (size_t i = 0; i < segCount; ++i) {
        uint32_t startV = closestVerts[i];
        uint32_t endV = closestVerts[(i + 1) % count];
        if (startV == endV) continue;

        // Dijkstra shortest path
        std::vector<float> dist(vertCount, 1e30f);
        std::vector<int> prev(vertCount, -1);
        using QNode = std::pair<float, uint32_t>;
        std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> pq;

        dist[startV] = 0.0f;
        pq.emplace(0.0f, startV);

        while (!pq.empty()) {
            auto [curDist, u] = pq.top();
            pq.pop();

            if (u == endV) break;
            if (curDist > dist[u]) continue;

            for (const auto& [v, weight] : adj[u]) {
                float nextD = curDist + weight;
                if (nextD < dist[v]) {
                    dist[v] = nextD;
                    prev[v] = static_cast<int>(u);
                    pq.emplace(nextD, v);
                }
            }
        }

        // Trace path back from endV to startV
        if (prev[endV] != -1) {
            uint32_t curr = endV;
            while (curr != startV && prev[curr] != -1) {
                uint32_t parent = static_cast<uint32_t>(prev[curr]);
                auto edgeKey = MakeEdgeKey(curr, parent);
                mesh.cutSeams.insert(edgeKey);
                outSeamEdges.push_back(edgeKey);
                curr = parent;
            }
        }
    }

    UVCutter::GenerateSeamLineIndices(mesh);
    return !outSeamEdges.empty();
}

// 2. Cuts curve directly into mesh topology by bisecting/splitting triangles along curve trajectory
bool CurveManager::CutCurveIntoMeshTopology(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outNewSeamEdges) {
    if (curve.points.size() < 2 || mesh.vertices.empty() || mesh.triangles.empty()) return false;

    // Helper to find intersection between 3D segment AB and edge CD on triangle plane
    auto SegmentIntersect = [](const Vec3& A, const Vec3& B, const Vec3& C, const Vec3& D, Vec3& outPt, float& outT) -> bool {
        Vec3 u = B - A;
        Vec3 v = D - C;
        Vec3 w = A - C;
        float a = u.Dot(u);
        float b = u.Dot(v);
        float c = v.Dot(v);
        float d = u.Dot(w);
        float e = v.Dot(w);
        float Dval = a * c - b * b;

        if (std::abs(Dval) < 1e-7f) return false;

        float sN = (b * e - c * d);
        float tN = (a * e - b * d);

        float sc = sN / Dval;
        float tc = tN / Dval;

        if (sc >= -0.01f && sc <= 1.01f && tc >= 0.0f && tc <= 1.0f) {
            sc = std::clamp(sc, 0.0f, 1.0f);
            tc = std::clamp(tc, 0.0f, 1.0f);
            Vec3 pA = A + u * sc;
            Vec3 pC = C + v * tc;
            if ((pA - pC).Length() < 0.02f * (v.Length() + 0.1f)) {
                outPt = pC;
                outT = tc;
                return true;
            }
        }
        return false;
    };

    size_t count = curve.points.size();
    size_t segCount = curve.isClosed ? count : (count - 1);

    // Track original edge splits to avoid duplicate vertex insertion
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeSplitMap;

    for (size_t s = 0; s < segCount; ++s) {
        const Vec3& A = curve.points[s].position;
        const Vec3& B = curve.points[(s + 1) % count].position;

        // Traverse triangles and test edges for cut intersection
        size_t initialTriCount = mesh.triangles.size();
        for (size_t t = 0; t < initialTriCount; ++t) {
            auto tri = mesh.triangles[t];
            int splitEdge = -1;
            Vec3 interPos;
            float interT = 0.0f;

            for (int e = 0; e < 3; ++e) {
                uint32_t v0 = tri.indices[e];
                uint32_t v1 = tri.indices[(e + 1) % 3];
                Vec3 C = mesh.vertices[v0].position;
                Vec3 D = mesh.vertices[v1].position;

                Vec3 hitPt;
                float tVal = 0.0f;
                if (SegmentIntersect(A, B, C, D, hitPt, tVal)) {
                    // Ignore if extremely close to existing endpoints
                    if (tVal > 0.05f && tVal < 0.95f) {
                        splitEdge = e;
                        interPos = hitPt;
                        interT = tVal;
                        break;
                    }
                }
            }

            if (splitEdge >= 0) {
                uint32_t v0 = tri.indices[splitEdge];
                uint32_t v1 = tri.indices[(splitEdge + 1) % 3];
                uint32_t v2 = tri.indices[(splitEdge + 2) % 3];
                auto edgeKey = MakeEdgeKey(v0, v1);

                uint32_t newVIdx = 0;
                auto it = edgeSplitMap.find(edgeKey);
                if (it != edgeSplitMap.end()) {
                    newVIdx = it->second;
                } else {
                    // Create new vertex interpolated along edge (v0, v1)
                    Vertex newVert;
                    newVert.position = interPos;
                    newVert.normal = (mesh.vertices[v0].normal * (1.0f - interT) + mesh.vertices[v1].normal * interT).Normalized();
                    newVert.uv.u = mesh.vertices[v0].uv.u * (1.0f - interT) + mesh.vertices[v1].uv.u * interT;
                    newVert.uv.v = mesh.vertices[v0].uv.v * (1.0f - interT) + mesh.vertices[v1].uv.v * interT;
                    newVert.tangent = mesh.vertices[v0].tangent;

                    newVIdx = static_cast<uint32_t>(mesh.vertices.size());
                    mesh.vertices.push_back(newVert);
                    edgeSplitMap[edgeKey] = newVIdx;
                }

                // Subdivide triangle t into two triangles: (v0, newV, v2) and (newV, v1, v2)
                LowPolyTriangle triA = tri;
                triA.indices[0] = v0;
                triA.indices[1] = newVIdx;
                triA.indices[2] = v2;

                LowPolyTriangle triB = tri;
                triB.indices[0] = newVIdx;
                triB.indices[1] = v1;
                triB.indices[2] = v2;

                mesh.triangles[t] = triA;
                mesh.triangles.push_back(triB);

                // Add newly created cut edge (newVIdx, v2) to seams
                auto newSeam = MakeEdgeKey(newVIdx, v2);
                mesh.cutSeams.insert(newSeam);
                outNewSeamEdges.push_back(newSeam);
            }
        }
    }

    // Fall back to edge snapping for any endpoints to ensure continuous seam loop
    SnapCurveToMeshEdges(curve, mesh, outNewSeamEdges);

    UVCutter::GenerateSeamLineIndices(mesh);
    return true;
}

// 3. Unified bake function to convert curve(s) into UV seams
bool CurveManager::BakeCurvesToSeams(CurveManager& mgr, LowPolyMesh& mesh, bool cutIntoTopology, int curveIndex) {
    if (mesh.vertices.empty()) return false;

    std::vector<std::pair<uint32_t, uint32_t>> seamEdges;

    if (curveIndex >= 0 && curveIndex < static_cast<int>(mgr.curves.size())) {
        SurfaceCurve& c = mgr.curves[curveIndex];
        if (cutIntoTopology) {
            CutCurveIntoMeshTopology(c, mesh, seamEdges);
        } else {
            SnapCurveToMeshEdges(c, mesh, seamEdges);
        }
        c.isBaked = true;
    } else {
        // Bake all unbaked curves
        for (auto& c : mgr.curves) {
            if (c.points.size() < 2) continue;
            if (cutIntoTopology) {
                CutCurveIntoMeshTopology(c, mesh, seamEdges);
            } else {
                SnapCurveToMeshEdges(c, mesh, seamEdges);
            }
            c.isBaked = true;
        }
    }

    UVCutter::GenerateSeamLineIndices(mesh);
    return !seamEdges.empty();
}

} // namespace Baker
