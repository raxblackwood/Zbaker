#include "UVCutter.h"
#include <queue>
#include <map>
#include <cmath>

namespace Baker {

namespace {

// Helper to quantize position for geometric topological edge welding
struct PosKey {
    int64_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
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

// Edge structure connecting two geometric vertices
struct GeomEdge {
    uint32_t g0 = 0;
    uint32_t g1 = 0;
    uint32_t origV0 = 0;
    uint32_t origV1 = 0;
    std::vector<int> triIndices;
};

std::pair<uint32_t, uint32_t> MakeEdgeKey(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

} // namespace

// Build map from vertex index to unique geometric vertex ID
std::vector<uint32_t> UVCutter::BuildGeomVertexMap(const LowPolyMesh& mesh) {
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

void UVCutter::MarkSeamsByAngle(LowPolyMesh& mesh, float angleThresholdDeg) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    auto geomMap = BuildGeomVertexMap(mesh);

    // Compute geometric face normals for each triangle
    std::vector<Vec3> faceNormals(mesh.triangles.size());
    for (size_t i = 0; i < mesh.triangles.size(); ++i) {
        const auto& tri = mesh.triangles[i];
        const Vec3& p0 = mesh.vertices[tri.indices[0]].position;
        const Vec3& p1 = mesh.vertices[tri.indices[1]].position;
        const Vec3& p2 = mesh.vertices[tri.indices[2]].position;
        Vec3 n = (p1 - p0).Cross(p2 - p0);
        float len = n.Length();
        faceNormals[i] = (len > 1e-8f) ? (n * (1.0f / len)) : Vec3(0, 1, 0);
    }

    // Build geometric edge to triangles map
    std::map<std::pair<uint32_t, uint32_t>, GeomEdge> edgeMap;
    for (int tIdx = 0; tIdx < static_cast<int>(mesh.triangles.size()); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t vA = tri.indices[e];
            uint32_t vB = tri.indices[(e + 1) % 3];
            uint32_t gA = geomMap[vA];
            uint32_t gB = geomMap[vB];
            auto key = MakeEdgeKey(gA, gB);

            auto& edge = edgeMap[key];
            edge.g0 = key.first;
            edge.g1 = key.second;
            edge.origV0 = vA;
            edge.origV1 = vB;
            edge.triIndices.push_back(tIdx);
        }
    }

    // Mark seams where dihedral angle exceeds threshold or where edge is open boundary
    float minCos = std::cos(angleThresholdDeg * 3.14159265f / 180.0f);
    for (const auto& kv : edgeMap) {
        const auto& edge = kv.second;
        if (edge.triIndices.size() == 1) {
            // Open mesh boundary: always a seam
            mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
        } else if (edge.triIndices.size() == 2) {
            int tA = edge.triIndices[0];
            int tB = edge.triIndices[1];
            float dotN = faceNormals[tA].Dot(faceNormals[tB]);
            if (dotN < minCos) {
                mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
            }
        }
    }

    GenerateSeamLineIndices(mesh);
}

void UVCutter::MarkSeamsByPolygroup(LowPolyMesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    auto geomMap = BuildGeomVertexMap(mesh);
    std::map<std::pair<uint32_t, uint32_t>, GeomEdge> edgeMap;

    for (int tIdx = 0; tIdx < static_cast<int>(mesh.triangles.size()); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t vA = tri.indices[e];
            uint32_t vB = tri.indices[(e + 1) % 3];
            uint32_t gA = geomMap[vA];
            uint32_t gB = geomMap[vB];
            auto key = MakeEdgeKey(gA, gB);

            auto& edge = edgeMap[key];
            edge.g0 = key.first;
            edge.g1 = key.second;
            edge.origV0 = vA;
            edge.origV1 = vB;
            edge.triIndices.push_back(tIdx);
        }
    }

    for (const auto& kv : edgeMap) {
        const auto& edge = kv.second;
        if (edge.triIndices.size() >= 2) {
            int tA = edge.triIndices[0];
            int tB = edge.triIndices[1];
            if (mesh.triangles[tA].meshIndex != mesh.triangles[tB].meshIndex) {
                mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
            }
        } else if (edge.triIndices.size() == 1) {
            mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
        }
    }

    GenerateSeamLineIndices(mesh);
}

void UVCutter::MarkSeamsByUVBorders(LowPolyMesh& mesh) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    auto geomMap = BuildGeomVertexMap(mesh);
    std::map<std::pair<uint32_t, uint32_t>, GeomEdge> edgeMap;

    for (int tIdx = 0; tIdx < static_cast<int>(mesh.triangles.size()); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t vA = tri.indices[e];
            uint32_t vB = tri.indices[(e + 1) % 3];
            uint32_t gA = geomMap[vA];
            uint32_t gB = geomMap[vB];
            auto key = MakeEdgeKey(gA, gB);

            auto& edge = edgeMap[key];
            edge.g0 = key.first;
            edge.g1 = key.second;
            edge.origV0 = vA;
            edge.origV1 = vB;
            edge.triIndices.push_back(tIdx);
        }
    }

    for (const auto& kv : edgeMap) {
        const auto& edge = kv.second;
        if (edge.triIndices.size() == 1) {
            mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
        } else if (edge.triIndices.size() == 2) {
            // Check if UV coordinates match across triangles
            int t0 = edge.triIndices[0];
            int t1 = edge.triIndices[1];

            // Find vertices in each triangle corresponding to g0 and g1
            const auto& tri0 = mesh.triangles[t0];
            const auto& tri1 = mesh.triangles[t1];

            uint32_t v0_t0 = 0, v1_t0 = 0;
            uint32_t v0_t1 = 0, v1_t1 = 0;

            for (int e = 0; e < 3; ++e) {
                if (geomMap[tri0.indices[e]] == edge.g0) v0_t0 = tri0.indices[e];
                if (geomMap[tri0.indices[e]] == edge.g1) v1_t0 = tri0.indices[e];
                if (geomMap[tri1.indices[e]] == edge.g0) v0_t1 = tri1.indices[e];
                if (geomMap[tri1.indices[e]] == edge.g1) v1_t1 = tri1.indices[e];
            }

            float du0 = std::abs(mesh.vertices[v0_t0].uv.u - mesh.vertices[v0_t1].uv.u);
            float dv0 = std::abs(mesh.vertices[v0_t0].uv.v - mesh.vertices[v0_t1].uv.v);
            float du1 = std::abs(mesh.vertices[v1_t0].uv.u - mesh.vertices[v1_t1].uv.u);
            float dv1 = std::abs(mesh.vertices[v1_t0].uv.v - mesh.vertices[v1_t1].uv.v);

            if (du0 > 1e-4f || dv0 > 1e-4f || du1 > 1e-4f || dv1 > 1e-4f) {
                mesh.cutSeams.insert(MakeEdgeKey(edge.origV0, edge.origV1));
            }
        }
    }

    GenerateSeamLineIndices(mesh);
}

void UVCutter::ClearSeams(LowPolyMesh& mesh) {
    mesh.cutSeams.clear();
    mesh.seamLineIndices.clear();
}

void UVCutter::ToggleEdgeSeam(LowPolyMesh& mesh, uint32_t v0, uint32_t v1) {
    auto key = MakeEdgeKey(v0, v1);
    if (mesh.cutSeams.find(key) != mesh.cutSeams.end()) {
        mesh.cutSeams.erase(key);
    } else {
        mesh.cutSeams.insert(key);
    }
    GenerateSeamLineIndices(mesh);
}

std::vector<std::pair<uint32_t, uint32_t>> UVCutter::GetEdgeLoop(const LowPolyMesh& mesh, uint32_t v0, uint32_t v1) {
    std::vector<std::pair<uint32_t, uint32_t>> loopResult;
    if (mesh.vertices.empty()) return loopResult;

    auto geomMap = BuildGeomVertexMap(mesh);
    if (v0 >= geomMap.size() || v1 >= geomMap.size()) return loopResult;

    uint32_t g0 = geomMap[v0];
    uint32_t g1 = geomMap[v1];
    if (g0 == g1) return loopResult;

    auto initialKey = MakeEdgeKey(g0, g1);
    loopResult.push_back(MakeEdgeKey(v0, v1));

    // Map geometric vertex to original vertex representative & 3D position
    std::vector<Vec3> geomPos(geomMap.size());
    std::vector<uint32_t> geomToOrig(geomMap.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        uint32_t g = geomMap[i];
        geomPos[g] = mesh.vertices[i].position;
        geomToOrig[g] = static_cast<uint32_t>(i);
    }

    // Build geometric polygons:
    struct PolyGeom {
        std::vector<uint32_t> geomVerts;
        std::vector<uint32_t> origVerts;
    };
    std::vector<PolyGeom> polys;

    if (!mesh.polygonFaces.empty()) {
        polys.reserve(mesh.polygonFaces.size());
        for (const auto& face : mesh.polygonFaces) {
            PolyGeom pg;
            pg.origVerts = face;
            pg.geomVerts.resize(face.size());
            for (size_t i = 0; i < face.size(); ++i) {
                pg.geomVerts[i] = geomMap[face[i]];
            }
            polys.push_back(std::move(pg));
        }
    } else {
        polys.reserve(mesh.triangles.size());
        for (const auto& tri : mesh.triangles) {
            PolyGeom pg;
            pg.origVerts = { tri.indices[0], tri.indices[1], tri.indices[2] };
            pg.geomVerts = { geomMap[tri.indices[0]], geomMap[tri.indices[1]], geomMap[tri.indices[2]] };
            polys.push_back(std::move(pg));
        }
    }

    // Map geometric edge to incident polygons
    struct EdgeUsage {
        uint32_t polyIdx;
        uint32_t edgeIdxInPoly;
    };
    std::map<std::pair<uint32_t, uint32_t>, std::vector<EdgeUsage>> edgeToPolys;
    // Map geometric vertex to incident geometric edges (and orig vertex info)
    std::map<uint32_t, std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>>> vertexIncidentEdges;

    for (uint32_t pIdx = 0; pIdx < static_cast<uint32_t>(polys.size()); ++pIdx) {
        const auto& pg = polys[pIdx];
        size_t n = pg.geomVerts.size();
        for (size_t e = 0; e < n; ++e) {
            uint32_t ga = pg.geomVerts[e];
            uint32_t gb = pg.geomVerts[(e + 1) % n];
            auto ek = MakeEdgeKey(ga, gb);
            edgeToPolys[ek].push_back({ pIdx, static_cast<uint32_t>(e) });

            uint32_t oa = pg.origVerts[e];
            uint32_t ob = pg.origVerts[(e + 1) % n];
            vertexIncidentEdges[ga].push_back({ gb, MakeEdgeKey(oa, ob) });
            vertexIncidentEdges[gb].push_back({ ga, MakeEdgeKey(oa, ob) });
        }
    }

    std::set<std::pair<uint32_t, uint32_t>> visitedGeomEdges;
    visitedGeomEdges.insert(initialKey);

    // Maya Edge Loop Walking:
    // Walks end-to-end through vertices (collinear / opposite across vertex star)
    // Dir 0: walk forward from g1 entering next vertices
    // Dir 1: walk backward from g0 entering previous vertices
    for (int dir = 0; dir < 2; ++dir) {
        uint32_t prevV = (dir == 0) ? g0 : g1;
        uint32_t currV = (dir == 0) ? g1 : g0;

        while (true) {
            Vec3 dirIn = (geomPos[currV] - geomPos[prevV]);
            float lenIn = dirIn.Length();
            if (lenIn < 1e-7f) break;
            dirIn = dirIn * (1.0f / lenIn);

            // Find all outgoing edges from currV
            auto itEdges = vertexIncidentEdges.find(currV);
            if (itEdges == vertexIncidentEdges.end() || itEdges->second.size() < 2) {
                break; // Boundary reached
            }

            // In quad topology with regular valence-4 vertex:
            // Look for topological opposite edge across adjacent quad faces
            uint32_t bestNextV = currV;
            std::pair<uint32_t, uint32_t> bestOrigEdge;
            float maxDot = -1.0f;

            // Check incident polygons to find quad-opposite continuation
            auto inEdgeKey = MakeEdgeKey(prevV, currV);
            auto itInPolys = edgeToPolys.find(inEdgeKey);
            bool foundTopological = false;

            if (itInPolys != edgeToPolys.end() && itInPolys->second.size() == 2) {
                // Two quads share the incoming edge
                uint32_t pA = itInPolys->second[0].polyIdx;
                uint32_t pB = itInPolys->second[1].polyIdx;
                if (polys[pA].geomVerts.size() == 4 && polys[pB].geomVerts.size() == 4) {
                    // Find lateral vertex in quad A
                    const auto& qA = polys[pA].geomVerts;
                    uint32_t latA = currV;
                    for (int k = 0; k < 4; ++k) {
                        if (qA[k] == currV) {
                            uint32_t n1 = qA[(k + 1) % 4];
                            uint32_t n2 = qA[(k + 3) % 4];
                            latA = (n1 == prevV) ? n2 : n1;
                            break;
                        }
                    }

                    // Find other quad sharing lateral edge (currV, latA)
                    auto latKey = MakeEdgeKey(currV, latA);
                    auto itLat = edgeToPolys.find(latKey);
                    if (itLat != edgeToPolys.end() && itLat->second.size() == 2) {
                        uint32_t pNext = (itLat->second[0].polyIdx == pA) ? itLat->second[1].polyIdx : itLat->second[0].polyIdx;
                        if (polys[pNext].geomVerts.size() == 4) {
                            const auto& qNext = polys[pNext].geomVerts;
                            for (int k = 0; k < 4; ++k) {
                                if (qNext[k] == currV) {
                                    uint32_t n1 = qNext[(k + 1) % 4];
                                    uint32_t n2 = qNext[(k + 3) % 4];
                                    uint32_t cand = (n1 == latA) ? n2 : n1;
                                    Vec3 dirOut = (geomPos[cand] - geomPos[currV]);
                                    float lenOut = dirOut.Length();
                                    if (lenOut > 1e-7f) {
                                        float d = dirIn.Dot(dirOut * (1.0f / lenOut));
                                        if (d > 0.35f) { // Collinear continuation
                                            bestNextV = cand;
                                            bestOrigEdge = MakeEdgeKey(geomToOrig[currV], geomToOrig[cand]);
                                            foundTopological = true;
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            // Fallback: If not found via strict quad topology, select most collinear outgoing edge
            if (!foundTopological) {
                for (const auto& candidate : itEdges->second) {
                    uint32_t nextV = candidate.first;
                    if (nextV == prevV) continue;

                    Vec3 dirOut = (geomPos[nextV] - geomPos[currV]);
                    float lenOut = dirOut.Length();
                    if (lenOut < 1e-7f) continue;
                    float dotVal = dirIn.Dot(dirOut * (1.0f / lenOut));
                    if (dotVal > maxDot && dotVal > 0.45f) { // Clear collinear direction
                        maxDot = dotVal;
                        bestNextV = nextV;
                        bestOrigEdge = candidate.second;
                    }
                }
            }

            if (bestNextV == currV) {
                // No valid continuation
                break;
            }

            auto nextKey = MakeEdgeKey(currV, bestNextV);
            if (visitedGeomEdges.find(nextKey) != visitedGeomEdges.end()) {
                // Loop closed
                break;
            }
            visitedGeomEdges.insert(nextKey);
            loopResult.push_back(bestOrigEdge);

            prevV = currV;
            currV = bestNextV;
        }
    }

    return loopResult;
}

std::vector<std::pair<uint32_t, uint32_t>> UVCutter::GetEdgeRing(const LowPolyMesh& mesh, uint32_t v0, uint32_t v1) {
    std::vector<std::pair<uint32_t, uint32_t>> ringResult;
    if (mesh.vertices.empty()) return ringResult;

    auto geomMap = BuildGeomVertexMap(mesh);
    if (v0 >= geomMap.size() || v1 >= geomMap.size()) return ringResult;

    uint32_t g0 = geomMap[v0];
    uint32_t g1 = geomMap[v1];
    if (g0 == g1) return ringResult;

    auto initialKey = MakeEdgeKey(g0, g1);
    ringResult.push_back(MakeEdgeKey(v0, v1));

    // Build geometric polygons:
    struct PolyGeom {
        std::vector<uint32_t> geomVerts;
        std::vector<uint32_t> origVerts;
    };
    std::vector<PolyGeom> polys;

    if (!mesh.polygonFaces.empty()) {
        polys.reserve(mesh.polygonFaces.size());
        for (const auto& face : mesh.polygonFaces) {
            PolyGeom pg;
            pg.origVerts = face;
            pg.geomVerts.resize(face.size());
            for (size_t i = 0; i < face.size(); ++i) {
                pg.geomVerts[i] = geomMap[face[i]];
            }
            polys.push_back(std::move(pg));
        }
    } else {
        polys.reserve(mesh.triangles.size());
        for (const auto& tri : mesh.triangles) {
            PolyGeom pg;
            pg.origVerts = { tri.indices[0], tri.indices[1], tri.indices[2] };
            pg.geomVerts = { geomMap[tri.indices[0]], geomMap[tri.indices[1]], geomMap[tri.indices[2]] };
            polys.push_back(std::move(pg));
        }
    }

    struct EdgeUsage {
        uint32_t polyIdx;
        uint32_t edgeIdxInPoly;
    };
    std::map<std::pair<uint32_t, uint32_t>, std::vector<EdgeUsage>> edgeToPolys;

    for (uint32_t pIdx = 0; pIdx < static_cast<uint32_t>(polys.size()); ++pIdx) {
        const auto& pg = polys[pIdx];
        size_t n = pg.geomVerts.size();
        for (size_t e = 0; e < n; ++e) {
            auto ek = MakeEdgeKey(pg.geomVerts[e], pg.geomVerts[(e + 1) % n]);
            edgeToPolys[ek].push_back({ pIdx, static_cast<uint32_t>(e) });
        }
    }

    auto startIt = edgeToPolys.find(initialKey);
    if (startIt == edgeToPolys.end()) return ringResult;

    std::set<std::pair<uint32_t, uint32_t>> visitedGeomEdges;
    visitedGeomEdges.insert(initialKey);

    // Walk across quad faces (stepping to opposite parallel edge in quad)
    const auto& startUsages = startIt->second;
    for (size_t dir = 0; dir < startUsages.size() && dir < 2; ++dir) {
        uint32_t curPolyIdx = startUsages[dir].polyIdx;
        uint32_t curEdgeIdx = startUsages[dir].edgeIdxInPoly;

        while (true) {
            const auto& pg = polys[curPolyIdx];
            size_t n = pg.geomVerts.size();
            if (n < 4) {
                break;
            }

            // In quad (n == 4), opposite parallel edge is (curEdgeIdx + 2) % 4
            uint32_t oppEdgeIdx = (curEdgeIdx + 2) % 4;
            uint32_t oppG0 = pg.geomVerts[oppEdgeIdx];
            uint32_t oppG1 = pg.geomVerts[(oppEdgeIdx + 1) % 4];
            auto oppKey = MakeEdgeKey(oppG0, oppG1);

            if (visitedGeomEdges.find(oppKey) != visitedGeomEdges.end()) {
                break;
            }
            visitedGeomEdges.insert(oppKey);

            uint32_t oppV0 = pg.origVerts[oppEdgeIdx];
            uint32_t oppV1 = pg.origVerts[(oppEdgeIdx + 1) % 4];
            ringResult.push_back(MakeEdgeKey(oppV0, oppV1));

            auto itOpp = edgeToPolys.find(oppKey);
            if (itOpp == edgeToPolys.end() || itOpp->second.size() < 2) {
                break;
            }

            uint32_t nextPolyIdx = (itOpp->second[0].polyIdx == curPolyIdx) ? itOpp->second[1].polyIdx : itOpp->second[0].polyIdx;
            uint32_t nextEdgeIdx = (itOpp->second[0].polyIdx == curPolyIdx) ? itOpp->second[1].edgeIdxInPoly : itOpp->second[0].edgeIdxInPoly;

            curPolyIdx = nextPolyIdx;
            curEdgeIdx = nextEdgeIdx;
        }
    }

    return ringResult;
}

void UVCutter::SelectEdgeRing(LowPolyMesh& mesh, uint32_t v0, uint32_t v1, bool addToSelection) {
    if (!addToSelection) {
        mesh.selectedEdges.clear();
    }
    auto ring = GetEdgeRing(mesh, v0, v1);
    for (const auto& edge : ring) {
        mesh.selectedEdges.insert(edge);
    }
    mesh.GenerateSelectedEdgeLines();
}

std::vector<std::pair<uint32_t, uint32_t>> UVCutter::GetShortestPath(const LowPolyMesh& mesh,
                                                                    uint32_t startV0, uint32_t startV1,
                                                                    uint32_t endV0, uint32_t endV1) {
    std::vector<std::pair<uint32_t, uint32_t>> pathResult;
    if (mesh.vertices.empty()) return pathResult;

    auto geomMap = BuildGeomVertexMap(mesh);
    if (startV0 >= geomMap.size() || startV1 >= geomMap.size() ||
        endV0 >= geomMap.size() || endV1 >= geomMap.size()) {
        return pathResult;
    }

    uint32_t gStart0 = geomMap[startV0];
    uint32_t gStart1 = geomMap[startV1];
    uint32_t gEnd0 = geomMap[endV0];
    uint32_t gEnd1 = geomMap[endV1];

    auto startKey = MakeEdgeKey(startV0, startV1);
    auto endKey = MakeEdgeKey(endV0, endV1);
    if (MakeEdgeKey(gStart0, gStart1) == MakeEdgeKey(gEnd0, gEnd1)) {
        pathResult.push_back(startKey);
        return pathResult;
    }

    // Build adjacency graph of geometric vertices
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

    // Dijkstra shortest path from {gStart0, gStart1} to {gEnd0, gEnd1}
    struct NodeDist {
        float dist;
        uint32_t node;
        bool operator>(const NodeDist& o) const { return dist > o.dist; }
    };
    std::priority_queue<NodeDist, std::vector<NodeDist>, std::greater<NodeDist>> pq;
    std::unordered_map<uint32_t, float> distMap;
    std::unordered_map<uint32_t, std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> parentMap; // node -> (parentGeom, origEdge)

    distMap[gStart0] = 0.0f;
    distMap[gStart1] = 0.0f;
    pq.push({ 0.0f, gStart0 });
    pq.push({ 0.0f, gStart1 });

    uint32_t reachedTarget = UINT32_MAX;

    while (!pq.empty()) {
        auto top = pq.top();
        pq.pop();
        uint32_t u = top.node;
        float d = top.dist;

        if (d > distMap[u]) continue;

        if (u == gEnd0 || u == gEnd1) {
            reachedTarget = u;
            break;
        }

        auto it = adj.find(u);
        if (it == adj.end()) continue;

        for (const auto& edge : it->second) {
            uint32_t v = edge.toGeom;
            float newDist = d + edge.weight;
            auto itDist = distMap.find(v);
            if (itDist == distMap.end() || newDist < itDist->second) {
                distMap[v] = newDist;
                parentMap[v] = { u, edge.origEdge };
                pq.push({ newDist, v });
            }
        }
    }

    if (reachedTarget != UINT32_MAX) {
        // Backtrack path
        uint32_t curr = reachedTarget;
        while (curr != gStart0 && curr != gStart1) {
            auto itP = parentMap.find(curr);
            if (itP == parentMap.end()) break;
            pathResult.push_back(itP->second.second);
            curr = itP->second.first;
        }
        pathResult.push_back(startKey);
        pathResult.push_back(endKey);
    }

    return pathResult;
}

void UVCutter::SelectShortestPath(LowPolyMesh& mesh,
                                  uint32_t startV0, uint32_t startV1,
                                  uint32_t endV0, uint32_t endV1,
                                  bool addToSelection) {
    if (!addToSelection) {
        mesh.selectedEdges.clear();
    }
    auto path = GetShortestPath(mesh, startV0, startV1, endV0, endV1);
    for (const auto& edge : path) {
        mesh.selectedEdges.insert(edge);
    }
    mesh.GenerateSelectedEdgeLines();
}

void UVCutter::CameraProjectUVs(LowPolyMesh& mesh, const DirectX::XMMATRIX& viewProjMatrix) {
    if (mesh.vertices.empty()) return;

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        const Vec3& p = mesh.vertices[i].position;
        DirectX::XMVECTOR pW = DirectX::XMVectorSet(p.x, p.y, p.z, 1.0f);
        DirectX::XMVECTOR pClip = DirectX::XMVector3TransformCoord(pW, viewProjMatrix);

        float ndcX = DirectX::XMVectorGetX(pClip);
        float ndcY = DirectX::XMVectorGetY(pClip);

        float u = std::clamp((ndcX + 1.0f) * 0.5f, 0.0f, 1.0f);
        float v = std::clamp((ndcY + 1.0f) * 0.5f, 0.0f, 1.0f);

        mesh.vertices[i].uv.u = u;
        mesh.vertices[i].uv.v = v;
    }
}

void UVCutter::SelectEdgeLoop(LowPolyMesh& mesh, uint32_t v0, uint32_t v1, bool addToSelection) {
    if (!addToSelection) {
        mesh.selectedEdges.clear();
    }
    auto loop = GetEdgeLoop(mesh, v0, v1);
    for (const auto& edge : loop) {
        mesh.selectedEdges.insert(edge);
    }
    mesh.GenerateSelectedEdgeLines();
}

void UVCutter::CutSelectedEdges(LowPolyMesh& mesh) {
    if (mesh.selectedEdges.empty()) return;
    for (const auto& edge : mesh.selectedEdges) {
        mesh.cutSeams.insert(edge);
    }
    GenerateSeamLineIndices(mesh);
}

void UVCutter::SewSelectedEdges(LowPolyMesh& mesh) {
    if (mesh.selectedEdges.empty()) return;
    auto geomMap = BuildGeomVertexMap(mesh);
    std::set<std::pair<uint32_t, uint32_t>> geomSelected;
    for (const auto& edge : mesh.selectedEdges) {
        if (edge.first < geomMap.size() && edge.second < geomMap.size()) {
            geomSelected.insert(MakeEdgeKey(geomMap[edge.first], geomMap[edge.second]));
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> seamsToRemove;
    for (const auto& seam : mesh.cutSeams) {
        if (seam.first < geomMap.size() && seam.second < geomMap.size()) {
            auto gKey = MakeEdgeKey(geomMap[seam.first], geomMap[seam.second]);
            if (geomSelected.find(gKey) != geomSelected.end()) {
                seamsToRemove.push_back(seam);
            }
        }
    }
    for (const auto& s : seamsToRemove) {
        mesh.cutSeams.erase(s);
    }
    GenerateSeamLineIndices(mesh);
}

void UVCutter::ClearSelection(LowPolyMesh& mesh) {
    mesh.selectedEdges.clear();
    mesh.selectedEdgeLineIndices.clear();
}

void UVCutter::SelectAllSeams(LowPolyMesh& mesh) {
    mesh.selectedEdges = mesh.cutSeams;
    mesh.GenerateSelectedEdgeLines();
}

std::vector<std::vector<int>> UVCutter::SegmentIslands(const LowPolyMesh& mesh) {
    std::vector<std::vector<int>> islands;
    if (mesh.triangles.empty()) return islands;

    auto geomMap = BuildGeomVertexMap(mesh);

    // Build geometric edge to triangle map
    std::map<std::pair<uint32_t, uint32_t>, std::vector<int>> edgeToTris;
    for (int tIdx = 0; tIdx < static_cast<int>(mesh.triangles.size()); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t gA = geomMap[tri.indices[e]];
            uint32_t gB = geomMap[tri.indices[(e + 1) % 3]];
            edgeToTris[MakeEdgeKey(gA, gB)].push_back(tIdx);
        }
    }

    // Build geometric seam set
    std::set<std::pair<uint32_t, uint32_t>> geomSeams;
    for (const auto& s : mesh.cutSeams) {
        if (s.first < mesh.vertices.size() && s.second < mesh.vertices.size()) {
            geomSeams.insert(MakeEdgeKey(geomMap[s.first], geomMap[s.second]));
        }
    }

    // Build triangle adjacency graph: two triangles are adjacent if they share a geometric edge that is NOT a seam
    std::vector<std::vector<int>> triAdjacency(mesh.triangles.size());
    for (const auto& kv : edgeToTris) {
        const auto& key = kv.first;
        const auto& tris = kv.second;
        if (geomSeams.find(key) != geomSeams.end()) {
            continue; // Seam cuts adjacency
        }
        if (tris.size() == 2) {
            triAdjacency[tris[0]].push_back(tris[1]);
            triAdjacency[tris[1]].push_back(tris[0]);
        }
    }

    // BFS to extract connected components (islands)
    std::vector<bool> visited(mesh.triangles.size(), false);
    for (size_t i = 0; i < mesh.triangles.size(); ++i) {
        if (visited[i]) continue;

        std::vector<int> islandTris;
        std::queue<int> q;
        q.push(static_cast<int>(i));
        visited[i] = true;

        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            islandTris.push_back(cur);

            for (int neighbor : triAdjacency[cur]) {
                if (!visited[neighbor]) {
                    visited[neighbor] = true;
                    q.push(neighbor);
                }
            }
        }
        islands.push_back(islandTris);
    }

    return islands;
}

void UVCutter::GenerateSmartSeams(LowPolyMesh& mesh, float angleThresholdDeg) {
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    auto geomMap = BuildGeomVertexMap(mesh);
    size_t numTris = mesh.triangles.size();

    // 1. Compute geometric face normals for each triangle
    std::vector<Vec3> faceNormals(numTris);
    for (size_t i = 0; i < numTris; ++i) {
        const auto& tri = mesh.triangles[i];
        const Vec3& p0 = mesh.vertices[tri.indices[0]].position;
        const Vec3& p1 = mesh.vertices[tri.indices[1]].position;
        const Vec3& p2 = mesh.vertices[tri.indices[2]].position;
        Vec3 n = (p1 - p0).Cross(p2 - p0);
        float len = n.Length();
        faceNormals[i] = (len > 1e-8f) ? (n * (1.0f / len)) : Vec3(0, 1, 0);
    }

    // 2. Build geometric edge to triangles adjacency
    std::map<std::pair<uint32_t, uint32_t>, std::vector<int>> edgeToTris;
    for (int tIdx = 0; tIdx < static_cast<int>(numTris); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t gA = geomMap[tri.indices[e]];
            uint32_t gB = geomMap[tri.indices[(e + 1) % 3]];
            edgeToTris[MakeEdgeKey(gA, gB)].push_back(tIdx);
        }
    }

    // 3. Build triangle adjacency graph
    std::vector<std::vector<int>> triNeighbors(numTris);
    for (const auto& kv : edgeToTris) {
        if (kv.second.size() == 2) {
            triNeighbors[kv.second[0]].push_back(kv.second[1]);
            triNeighbors[kv.second[1]].push_back(kv.second[0]);
        }
    }

    // 4. Cluster triangles into developable charts using normal similarity (Smart UV Charts)
    float minCos = std::cos(angleThresholdDeg * 3.14159265f / 180.0f);
    std::vector<int> chartId(numTris, -1);
    int nextChart = 0;

    for (size_t i = 0; i < numTris; ++i) {
        if (chartId[i] >= 0) continue;

        int cId = nextChart++;
        chartId[i] = cId;
        Vec3 refNormal = faceNormals[i];

        std::queue<int> q;
        q.push(static_cast<int>(i));

        while (!q.empty()) {
            int cur = q.front();
            q.pop();

            for (int neighbor : triNeighbors[cur]) {
                if (chartId[neighbor] >= 0) continue;

                float dotRef = faceNormals[neighbor].Dot(refNormal);
                float dotAdj = faceNormals[neighbor].Dot(faceNormals[cur]);
                if (dotRef >= minCos && dotAdj >= minCos) {
                    chartId[neighbor] = cId;
                    q.push(neighbor);
                }
            }
        }
    }

    // 5. Mark seams strictly along chart boundaries and open mesh borders
    mesh.cutSeams.clear();
    for (int tIdx = 0; tIdx < static_cast<int>(numTris); ++tIdx) {
        const auto& tri = mesh.triangles[tIdx];
        for (int e = 0; e < 3; ++e) {
            uint32_t vA = tri.indices[e];
            uint32_t vB = tri.indices[(e + 1) % 3];
            uint32_t gA = geomMap[vA];
            uint32_t gB = geomMap[vB];
            auto key = MakeEdgeKey(gA, gB);
            const auto& adjTris = edgeToTris[key];
            if (adjTris.size() == 1) {
                mesh.cutSeams.insert(MakeEdgeKey(vA, vB));
            } else if (adjTris.size() == 2) {
                int t0 = adjTris[0];
                int t1 = adjTris[1];
                if (chartId[t0] != chartId[t1]) {
                    mesh.cutSeams.insert(MakeEdgeKey(vA, vB));
                }
            }
        }
    }

    GenerateSeamLineIndices(mesh);
}

void UVCutter::SplitMeshAlongSeams(LowPolyMesh& mesh, const std::vector<std::vector<int>>& islandsTris) {
    if (mesh.vertices.empty() || islandsTris.size() <= 1) return;

    size_t origVertCount = mesh.vertices.size();
    std::vector<int> vertexOwnerIsland(origVertCount, -1);
    std::map<std::pair<int, uint32_t>, uint32_t> dupMap;

    for (size_t islIdx = 0; islIdx < islandsTris.size(); ++islIdx) {
        for (int tIdx : islandsTris[islIdx]) {
            if (tIdx < 0 || tIdx >= static_cast<int>(mesh.triangles.size())) continue;
            auto& tri = mesh.triangles[tIdx];
            for (int k = 0; k < 3; ++k) {
                uint32_t v = tri.indices[k];
                if (v >= origVertCount) continue;

                if (vertexOwnerIsland[v] == -1) {
                    vertexOwnerIsland[v] = static_cast<int>(islIdx);
                } else if (vertexOwnerIsland[v] != static_cast<int>(islIdx)) {
                    auto key = std::make_pair(static_cast<int>(islIdx), v);
                    auto it = dupMap.find(key);
                    if (it != dupMap.end()) {
                        tri.indices[k] = it->second;
                    } else {
                        uint32_t newVIdx = static_cast<uint32_t>(mesh.vertices.size());
                        mesh.vertices.push_back(mesh.vertices[v]);
                        if (!mesh.cagePositions.empty() && v < mesh.cagePositions.size()) {
                            mesh.cagePositions.push_back(mesh.cagePositions[v]);
                        }
                        if (!mesh.cageNormals.empty() && v < mesh.cageNormals.size()) {
                            mesh.cageNormals.push_back(mesh.cageNormals[v]);
                        }
                        if (!mesh.cageOffsets.empty() && v < mesh.cageOffsets.size()) {
                            mesh.cageOffsets.push_back(mesh.cageOffsets[v]);
                        }
                        if (!mesh.vertexSkew.empty() && v < mesh.vertexSkew.size()) {
                            mesh.vertexSkew.push_back(mesh.vertexSkew[v]);
                        }
                        dupMap[key] = newVIdx;
                        tri.indices[k] = newVIdx;
                    }
                }
            }
        }
    }

    GenerateSeamLineIndices(mesh);
    mesh.GenerateSelectedEdgeLines();
}

void UVCutter::GenerateSeamLineIndices(LowPolyMesh& mesh) {
    mesh.seamLineIndices.clear();
    mesh.seamLineIndices.reserve(mesh.cutSeams.size() * 2);

    for (const auto& seam : mesh.cutSeams) {
        mesh.seamLineIndices.push_back(seam.first);
        mesh.seamLineIndices.push_back(seam.second);
    }
}

} // namespace Baker
