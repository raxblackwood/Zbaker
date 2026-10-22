#include "UVSurfaceCurve.h"
#include "UVCutter.h"
#include <queue>
#include <unordered_map>
#include <map>
#include <set>
#include <cmath>
#include <algorithm>

namespace Baker {

namespace {

struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
};

struct PosKeyHash {
    size_t operator()(const PosKey& k) const noexcept {
        size_t h1 = std::hash<int64_t>{}(k.x);
        size_t h2 = std::hash<int64_t>{}(k.y);
        size_t h3 = std::hash<int64_t>{}(k.z);
        return h1 ^ (h2 * 31 + (h1 >> 2)) ^ (h3 * 1009 + (h2 >> 2));
    }
};

PosKey MakePosKey(const Vec3& p) {
    constexpr double scale = 100000.0;
    return PosKey{
        static_cast<int64_t>(std::floor(static_cast<double>(p.x) * scale + 0.5)),
        static_cast<int64_t>(std::floor(static_cast<double>(p.y) * scale + 0.5)),
        static_cast<int64_t>(std::floor(static_cast<double>(p.z) * scale + 0.5))
    };
}

std::vector<uint32_t> BuildGeomVertexMap(const LowPolyMesh& mesh) {
    std::vector<uint32_t> geomMap(mesh.vertices.size());
    std::unordered_map<PosKey, uint32_t, PosKeyHash> posToGeom;
    posToGeom.reserve(mesh.vertices.size());

    uint32_t nextGeomId = 0;
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        PosKey k = MakePosKey(mesh.vertices[i].position);
        auto it = posToGeom.find(k);
        if (it == posToGeom.end()) {
            posToGeom[k] = nextGeomId;
            geomMap[i] = nextGeomId;
            nextGeomId++;
        } else {
            geomMap[i] = it->second;
        }
    }
    return geomMap;
}

inline std::pair<uint32_t, uint32_t> MakeEdgeKey(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

inline float Dist3D(const Vec3& a, const Vec3& b) {
    return (a - b).Length();
}

} // namespace

void SurfaceCurve::AddPoint(const Vec3& pos, const Vec3& norm, int triIdx) {
    points.emplace_back(pos, norm, triIdx);
    isBaked = false;
}

void SurfaceCurve::AddSegmentPath(const std::vector<Vec3>& segmentSurfacePoints) {
    for (const auto& pt : segmentSurfacePoints) {
        if (!sampledPath.empty()) {
            Vec3 diff = sampledPath.back() - pt;
            if (diff.Dot(diff) < 1e-8f) {
                continue; // Avoid consecutive duplicates
            }
        }
        sampledPath.push_back(pt);
    }
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

void SurfaceCurve::UpdateSampledPath(const LowPolyMesh& mesh,
                                    const DirectX::XMMATRIX* worldMat,
                                    const DirectX::XMMATRIX* viewProjMat,
                                    int vpWidth, int vpHeight) {
    sampledPath.clear();
    if (points.empty()) return;
    if (points.size() == 1) {
        sampledPath.push_back(points[0].position);
        return;
    }

    size_t count = points.size();
    size_t segCount = isClosed ? count : (count - 1);

    bool hasCamera = (worldMat && viewProjMat && vpWidth > 10 && vpHeight > 10);

    for (size_t i = 0; i < segCount; ++i) {
        const Vec3& p0 = points[i].position;
        const Vec3& p1 = points[(i + 1) % count].position;

        if (hasCamera) {
            auto seg = CurveManager::SampleCameraSurfacePath(
                mesh, *worldMat, *viewProjMat, vpWidth, vpHeight, p0, p1, 36
            );
            AddSegmentPath(seg);
        } else {
            // Fallback: 24 surface-interpolated points
            int numSamples = 24;
            for (int s = 0; s < numSamples; ++s) {
                float t = static_cast<float>(s) / static_cast<float>(numSamples);
                Vec3 pInter = p0 * (1.0f - t) + p1 * t;
                Vec3 nInter = (points[i].normal * (1.0f - t) + points[(i + 1) % count].normal * t).Normalized();
                sampledPath.push_back(pInter + nInter * 0.0015f);
            }
        }
    }

    if (!isClosed && !points.empty()) {
        sampledPath.push_back(points.back().position);
    }
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

void CurveManager::UpdateActiveSampledPath(const LowPolyMesh& mesh,
                                          const DirectX::XMMATRIX* worldMat,
                                          const DirectX::XMMATRIX* viewProjMat,
                                          int vpWidth, int vpHeight) {
    SurfaceCurve* c = GetActiveCurve();
    if (c) {
        c->UpdateSampledPath(mesh, worldMat, viewProjMat, vpWidth, vpHeight);
    }
}

std::vector<Vec3> CurveManager::SampleCameraSurfacePath(
    const LowPolyMesh& mesh,
    const DirectX::XMMATRIX& worldMatrix,
    const DirectX::XMMATRIX& viewProjMatrix,
    int vpWidth, int vpHeight,
    const Vec3& startLocal,
    const Vec3& endLocal,
    int numSamples
) {
    std::vector<Vec3> path;
    if (mesh.vertices.empty() || mesh.triangles.empty() || vpWidth <= 0 || vpHeight <= 0) {
        path.push_back(startLocal);
        path.push_back(endLocal);
        return path;
    }

    DirectX::XMMATRIX worldViewProj = DirectX::XMMatrixMultiply(worldMatrix, viewProjMatrix);
    DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(nullptr, viewProjMatrix);
    DirectX::XMMATRIX invWorld = DirectX::XMMatrixInverse(nullptr, worldMatrix);

    DirectX::XMVECTOR p0Local = DirectX::XMVectorSet(startLocal.x, startLocal.y, startLocal.z, 1.0f);
    DirectX::XMVECTOR p1Local = DirectX::XMVectorSet(endLocal.x, endLocal.y, endLocal.z, 1.0f);

    DirectX::XMVECTOR p0Clip = DirectX::XMVector3TransformCoord(p0Local, worldViewProj);
    DirectX::XMVECTOR p1Clip = DirectX::XMVector3TransformCoord(p1Local, worldViewProj);

    float s0X = (DirectX::XMVectorGetX(p0Clip) + 1.0f) * 0.5f * static_cast<float>(vpWidth);
    float s0Y = (1.0f - DirectX::XMVectorGetY(p0Clip)) * 0.5f * static_cast<float>(vpHeight);
    float s1X = (DirectX::XMVectorGetX(p1Clip) + 1.0f) * 0.5f * static_cast<float>(vpWidth);
    float s1Y = (1.0f - DirectX::XMVectorGetY(p1Clip)) * 0.5f * static_cast<float>(vpHeight);

    path.reserve(numSamples + 2);
    path.push_back(startLocal);

    Vec3 lastValidHit = startLocal;

    for (int i = 1; i < numSamples; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(numSamples);
        float sx = s0X * (1.0f - t) + s1X * t;
        float sy = s0Y * (1.0f - t) + s1Y * t;

        float ndcX = (2.0f * sx) / static_cast<float>(vpWidth) - 1.0f;
        float ndcY = 1.0f - (2.0f * sy) / static_cast<float>(vpHeight);

        DirectX::XMVECTOR nearPointNDC = DirectX::XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
        DirectX::XMVECTOR farPointNDC  = DirectX::XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
        DirectX::XMVECTOR nearWorld = DirectX::XMVector3TransformCoord(nearPointNDC, invViewProj);
        DirectX::XMVECTOR farWorld  = DirectX::XMVector3TransformCoord(farPointNDC,  invViewProj);
        DirectX::XMVECTOR dirWorld  = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(farWorld, nearWorld));

        DirectX::XMVECTOR vOrgLocal = DirectX::XMVector3TransformCoord(nearWorld, invWorld);
        DirectX::XMVECTOR vDirLocal = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(dirWorld, invWorld));

        Vec3 localOrg(DirectX::XMVectorGetX(vOrgLocal), DirectX::XMVectorGetY(vOrgLocal), DirectX::XMVectorGetZ(vOrgLocal));
        Vec3 localDir(DirectX::XMVectorGetX(vDirLocal), DirectX::XMVectorGetY(vDirLocal), DirectX::XMVectorGetZ(vDirLocal));

        float hitT = 0.0f;
        Vec3 hitPos, hitNormal;
        int hitTri = -1;

        if (mesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTri)) {
            // Offset slightly along surface normal to cleanly render above polygons
            Vec3 contouredPt = hitPos + hitNormal * 0.0015f;
            path.push_back(contouredPt);
            lastValidHit = contouredPt;
        } else {
            // If grazing edge or occlusion gap, interpolate along 3D contour from last hit
            Vec3 pInter = startLocal * (1.0f - t) + endLocal * t;
            path.push_back(pInter);
        }
    }

    path.push_back(endLocal);
    return path;
}

// 1. Snaps curve directly to closest low-poly mesh edges using geometric Dijkstra shortest surface edge path
bool CurveManager::SnapCurveToMeshEdges(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outSeamEdges) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return false;
    const auto& srcPoints = (!curve.sampledPath.empty()) ? curve.sampledPath : std::vector<Vec3>();
    if (srcPoints.empty() && curve.points.size() < 2) return false;

    // 1. Build map from vertex index to unique geometric vertex ID (spatial welding)
    auto geomMap = BuildGeomVertexMap(mesh);
    std::unordered_map<uint32_t, Vec3> geomPos;
    std::unordered_map<uint32_t, uint32_t> geomToOrig;
    geomPos.reserve(geomMap.size());
    geomToOrig.reserve(geomMap.size());

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        uint32_t g = geomMap[i];
        if (geomPos.find(g) == geomPos.end()) {
            geomPos[g] = mesh.vertices[i].position;
            geomToOrig[g] = static_cast<uint32_t>(i);
        }
    }

    // 2. Build adjacency graph for geometric vertices
    struct EdgeInfo {
        uint32_t toGeom;
        float weight;
        std::pair<uint32_t, uint32_t> origEdge;
    };
    std::unordered_map<uint32_t, std::vector<EdgeInfo>> adj;

    auto AddAdjEdge = [&](uint32_t oa, uint32_t ob) {
        uint32_t ga = geomMap[oa];
        uint32_t gb = geomMap[ob];
        if (ga == gb) return;
        float dist = (mesh.vertices[oa].position - mesh.vertices[ob].position).Length();
        auto origKey = MakeEdgeKey(oa, ob);
        adj[ga].push_back({ gb, dist, origKey });
        adj[gb].push_back({ ga, dist, origKey });
    };

    if (!mesh.polygonFaces.empty()) {
        for (const auto& face : mesh.polygonFaces) {
            size_t n = face.size();
            for (size_t i = 0; i < n; ++i) {
                AddAdjEdge(face[i], face[(i + 1) % n]);
            }
        }
    } else {
        for (const auto& tri : mesh.triangles) {
            AddAdjEdge(tri.indices[0], tri.indices[1]);
            AddAdjEdge(tri.indices[1], tri.indices[2]);
            AddAdjEdge(tri.indices[2], tri.indices[0]);
        }
    }

    // Helper to find closest geometric vertex to a 3D point
    auto FindClosestGeomVert = [&](const Vec3& p) -> uint32_t {
        uint32_t bestG = 0;
        float bestDistSq = 1e30f;
        for (const auto& kv : geomPos) {
            Vec3 diff = kv.second - p;
            float d2 = diff.Dot(diff);
            if (d2 < bestDistSq) {
                bestDistSq = d2;
                bestG = kv.first;
            }
        }
        return bestG;
    };

    // Prepare control trajectory
    std::vector<Vec3> traj;
    if (!curve.sampledPath.empty()) {
        // Subsample dense path to ~60 key waypoints along contour
        size_t step = std::max<size_t>(1, curve.sampledPath.size() / 50);
        for (size_t i = 0; i < curve.sampledPath.size(); i += step) {
            traj.push_back(curve.sampledPath[i]);
        }
        if ((traj.back() - curve.sampledPath.back()).Length() > 1e-4f) {
            traj.push_back(curve.sampledPath.back());
        }
    } else {
        for (const auto& cp : curve.points) {
            traj.push_back(cp.position);
        }
    }

    if (traj.size() < 2) return false;

    // 3. Connect sequential waypoints via geometric Dijkstra shortest path
    size_t count = traj.size();
    size_t segCount = curve.isClosed ? count : (count - 1);

    for (size_t s = 0; s < segCount; ++s) {
        uint32_t gStart = FindClosestGeomVert(traj[s]);
        uint32_t gEnd   = FindClosestGeomVert(traj[(s + 1) % count]);
        if (gStart == gEnd) continue;

        struct NodeDist {
            float dist;
            uint32_t node;
            bool operator>(const NodeDist& o) const { return dist > o.dist; }
        };
        std::priority_queue<NodeDist, std::vector<NodeDist>, std::greater<NodeDist>> pq;
        std::unordered_map<uint32_t, float> distMap;
        std::unordered_map<uint32_t, std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> parentMap;

        distMap[gStart] = 0.0f;
        pq.push({ 0.0f, gStart });

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();
            if (d > distMap[u]) continue;
            if (u == gEnd) break;

            auto it = adj.find(u);
            if (it == adj.end()) continue;

            for (const auto& edge : it->second) {
                float nd = d + edge.weight;
                auto itD = distMap.find(edge.toGeom);
                if (itD == distMap.end() || nd < itD->second) {
                    distMap[edge.toGeom] = nd;
                    parentMap[edge.toGeom] = { u, edge.origEdge };
                    pq.push({ nd, edge.toGeom });
                }
            }
        }

        if (parentMap.find(gEnd) != parentMap.end()) {
            uint32_t curr = gEnd;
            while (curr != gStart && parentMap.find(curr) != parentMap.end()) {
                auto [pNode, origEdge] = parentMap[curr];
                mesh.cutSeams.insert(origEdge);
                outSeamEdges.push_back(origEdge);
                curr = pNode;
            }
        }
    }

    UVCutter::GenerateSeamLineIndices(mesh);
    return !outSeamEdges.empty();
}

// 2. Cuts curve directly into mesh topology by bisecting triangles along curve surface path
bool CurveManager::CutCurveIntoMeshTopology(const SurfaceCurve& curve, LowPolyMesh& mesh, std::vector<std::pair<uint32_t, uint32_t>>& outNewSeamEdges) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return false;
    if (curve.points.size() < 2 && curve.sampledPath.size() < 2) return false;

    // Use dense surface path if available, otherwise control points
    const std::vector<Vec3>& path = (!curve.sampledPath.empty()) ? curve.sampledPath : std::vector<Vec3>();
    std::vector<Vec3> traj;
    if (!path.empty()) {
        size_t step = std::max<size_t>(1, path.size() / 60);
        for (size_t i = 0; i < path.size(); i += step) {
            traj.push_back(path[i]);
        }
        if ((traj.back() - path.back()).Length() > 1e-4f) traj.push_back(path.back());
    } else {
        for (const auto& cp : curve.points) traj.push_back(cp.position);
    }

    if (traj.size() < 2) return false;

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
            if ((pA - pC).Length() < 0.03f * (v.Length() + 0.1f)) {
                outPt = pC;
                outT = tc;
                return true;
            }
        }
        return false;
    };

    size_t count = traj.size();
    size_t segCount = curve.isClosed ? count : (count - 1);

    std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeSplitMap;

    for (size_t s = 0; s < segCount; ++s) {
        const Vec3& A = traj[s];
        const Vec3& B = traj[(s + 1) % count];

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

                // Bisect triangle into two triangles: (v0, newVIdx, v2) and (newVIdx, v1, v2)
                mesh.triangles[t].indices[0] = v0;
                mesh.triangles[t].indices[1] = newVIdx;
                mesh.triangles[t].indices[2] = v2;

                LowPolyTriangle newTri;
                newTri.indices[0] = newVIdx;
                newTri.indices[1] = v1;
                newTri.indices[2] = v2;
                newTri.udimTile = tri.udimTile;
                newTri.meshIndex = tri.meshIndex;
                newTri.groupMask = tri.groupMask;
                mesh.triangles.push_back(newTri);

                auto seamA = MakeEdgeKey(v2, newVIdx);
                mesh.cutSeams.insert(seamA);
                outNewSeamEdges.push_back(seamA);
            }
        }
    }

    // Always ensure connectivity by snapping to nearest edges along contour
    std::vector<std::pair<uint32_t, uint32_t>> snapEdges;
    SnapCurveToMeshEdges(curve, mesh, snapEdges);
    for (const auto& e : snapEdges) {
        outNewSeamEdges.push_back(e);
    }

    UVCutter::GenerateSeamLineIndices(mesh);
    return !outNewSeamEdges.empty();
}

bool CurveManager::BakeCurvesToSeams(CurveManager& mgr, LowPolyMesh& mesh, bool cutIntoTopology, int curveIndex) {
    bool anyBaked = false;
    for (size_t i = 0; i < mgr.curves.size(); ++i) {
        if (curveIndex >= 0 && static_cast<int>(i) != curveIndex) continue;
        auto& curve = mgr.curves[i];
        if (curve.points.size() < 2 && curve.sampledPath.size() < 2) continue;

        std::vector<std::pair<uint32_t, uint32_t>> edges;
        bool success = false;
        if (cutIntoTopology) {
            success = CutCurveIntoMeshTopology(curve, mesh, edges);
        } else {
            success = SnapCurveToMeshEdges(curve, mesh, edges);
        }
        if (success) {
            curve.isBaked = true;
            anyBaked = true;
        }
    }
    return anyBaked;
}

} // namespace Baker
