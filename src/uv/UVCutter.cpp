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

// Build map from vertex index to unique geometric vertex ID
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

void UVCutter::GenerateSeamLineIndices(LowPolyMesh& mesh) {
    mesh.seamLineIndices.clear();
    mesh.seamLineIndices.reserve(mesh.cutSeams.size() * 2);

    for (const auto& seam : mesh.cutSeams) {
        mesh.seamLineIndices.push_back(seam.first);
        mesh.seamLineIndices.push_back(seam.second);
    }
}

} // namespace Baker
