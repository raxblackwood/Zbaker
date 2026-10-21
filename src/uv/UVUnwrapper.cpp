#include "UVUnwrapper.h"
#include <unordered_map>
#include <map>
#include <cmath>
#include <algorithm>

namespace Baker {

UVIsland UVUnwrapper::UnwrapIsland(const LowPolyMesh& mesh, const std::vector<int>& triIndices, int islandId) {
    UVIsland island;
    island.islandId = islandId;
    island.triangleIndices = triIndices;
    if (triIndices.empty()) return island;

    island.polygroup = static_cast<int>(mesh.triangles[triIndices[0]].meshIndex);

    // 1. Gather all unique vertices used by this island
    std::unordered_map<uint32_t, int> localVertMap;
    std::vector<uint32_t> localVertIndices;
    for (int tIdx : triIndices) {
        const auto& tri = mesh.triangles[tIdx];
        for (int k = 0; k < 3; ++k) {
            uint32_t vIdx = tri.indices[k];
            if (localVertMap.find(vIdx) == localVertMap.end()) {
                localVertMap[vIdx] = static_cast<int>(localVertIndices.size());
                localVertIndices.push_back(vIdx);
            }
        }
    }
    island.vertexIndices = localVertIndices;

    size_t vertCount = localVertIndices.size();
    if (vertCount == 0) return island;

    // 2. Compute 3D area and average weighted normal of the island
    Vec3 avgNormal(0, 0, 0);
    Vec3 centroid(0, 0, 0);
    float total3DArea = 0.0f;

    for (int tIdx : triIndices) {
        const auto& tri = mesh.triangles[tIdx];
        const Vec3& p0 = mesh.vertices[tri.indices[0]].position;
        const Vec3& p1 = mesh.vertices[tri.indices[1]].position;
        const Vec3& p2 = mesh.vertices[tri.indices[2]].position;

        Vec3 cross = (p1 - p0).Cross(p2 - p0);
        float triArea = cross.Length() * 0.5f;
        total3DArea += triArea;

        if (cross.Length() > 1e-8f) {
            avgNormal = avgNormal + cross.Normalized() * triArea;
        }
    }

    island.area3D = total3DArea;
    if (avgNormal.Length() > 1e-6f) {
        avgNormal = avgNormal.Normalized();
    } else {
        avgNormal = Vec3(0, 1, 0);
    }

    for (uint32_t vIdx : localVertIndices) {
        centroid = centroid + mesh.vertices[vIdx].position;
    }
    centroid = centroid * (1.0f / static_cast<float>(vertCount));

    // 3. Orthonormal projection basis (U, V) perpendicular to avgNormal
    Vec3 up(0, 1, 0);
    if (std::abs(avgNormal.y) > 0.95f) {
        up = Vec3(1, 0, 0);
    }
    Vec3 uDir = avgNormal.Cross(up).Normalized();
    Vec3 vDir = avgNormal.Cross(uDir).Normalized();

    // 4. Initial planar projection
    std::vector<UVPoint> uvs(vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        Vec3 rel = mesh.vertices[localVertIndices[i]].position - centroid;
        uvs[i].u = rel.Dot(uDir);
        uvs[i].v = rel.Dot(vDir);
    }

    // 5. Conformal length-preserving spring relaxation to minimize distortion
    // Collect all internal edges in the island
    struct IslandEdge {
        int v0 = 0;
        int v1 = 0;
        float restLen3D = 0.0f;
    };
    std::vector<IslandEdge> edges;
    std::map<std::pair<int, int>, float> edgeLenMap;

    for (int tIdx : triIndices) {
        const auto& tri = mesh.triangles[tIdx];
        for (int k = 0; k < 3; ++k) {
            int lv0 = localVertMap[tri.indices[k]];
            int lv1 = localVertMap[tri.indices[(k + 1) % 3]];
            int minV = (std::min)(lv0, lv1);
            int maxV = (std::max)(lv0, lv1);
            auto key = std::make_pair(minV, maxV);
            if (edgeLenMap.find(key) == edgeLenMap.end()) {
                float len = (mesh.vertices[localVertIndices[minV]].position - mesh.vertices[localVertIndices[maxV]].position).Length();
                edgeLenMap[key] = len;
                edges.push_back({ minV, maxV, len });
            }
        }
    }

    // Spring relaxation iterations
    constexpr int RELAX_ITERS = 25;
    for (int iter = 0; iter < RELAX_ITERS; ++iter) {
        for (const auto& e : edges) {
            float du = uvs[e.v1].u - uvs[e.v0].u;
            float dv = uvs[e.v1].v - uvs[e.v0].v;
            float curLen = std::sqrt(du * du + dv * dv);
            if (curLen < 1e-7f) continue;

            float diff = (curLen - e.restLen3D) / curLen;
            float stepU = du * diff * 0.25f;
            float stepV = dv * diff * 0.25f;

            uvs[e.v0].u += stepU;
            uvs[e.v0].v += stepV;
            uvs[e.v1].u -= stepU;
            uvs[e.v1].v -= stepV;
        }
    }

    // 6. Compute 2D UV Area
    float totalUVArea = 0.0f;
    for (int tIdx : triIndices) {
        const auto& tri = mesh.triangles[tIdx];
        int lv0 = localVertMap[tri.indices[0]];
        int lv1 = localVertMap[tri.indices[1]];
        int lv2 = localVertMap[tri.indices[2]];

        float triUVArea = 0.5f * std::abs(
            uvs[lv0].u * (uvs[lv1].v - uvs[lv2].v) +
            uvs[lv1].u * (uvs[lv2].v - uvs[lv0].v) +
            uvs[lv2].u * (uvs[lv0].v - uvs[lv1].v)
        );
        totalUVArea += triUVArea;
    }
    island.areaUV = (totalUVArea > 1e-8f) ? totalUVArea : 1e-4f;
    island.uvs = uvs;
    island.ComputeBounds();

    return island;
}

std::vector<UVIsland> UVUnwrapper::UnwrapMesh(LowPolyMesh& mesh, const std::vector<std::vector<int>>& islandTris) {
    std::vector<UVIsland> islands;
    islands.reserve(islandTris.size());

    for (size_t i = 0; i < islandTris.size(); ++i) {
        islands.push_back(UnwrapIsland(mesh, islandTris[i], static_cast<int>(i)));
    }
    return islands;
}

} // namespace Baker
