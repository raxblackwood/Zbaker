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

namespace {

// Rest triangle in its own plane: r1 = (a, 0), r2 = (b, c). detR = a*c.
struct RestTri { float a, b, c; float area; bool ok; };

struct RelaxCtx {
    const LowPolyMesh& mesh;
    std::vector<RestTri> rest;
    std::vector<int> comp;              // component id per vertex
    std::vector<float> k;               // per-component UV->3D scale (sqrt(A3D / AUV))
    std::vector<float> sign;            // per-component orientation (+1 or -1)
    std::vector<std::vector<int>> ring; // triangles around each vertex
    explicit RelaxCtx(LowPolyMesh& m) : mesh(m) {}
    explicit RelaxCtx(const LowPolyMesh& m) : mesh(m) {}

    // Area-weighted symmetric Dirichlet energy of one triangle (4 * area at isometry).
    float TriEnergy(int t) const {
        const RestTri& r = rest[t];
        if (!r.ok) return 0.0f;
        const auto& tri = mesh.triangles[t];
        const auto& q0 = mesh.vertices[tri.indices[0]].uv;
        const auto& q1 = mesh.vertices[tri.indices[1]].uv;
        const auto& q2 = mesh.vertices[tri.indices[2]].uv;
        const int ci = comp[tri.indices[0]];
        const float kk = k[ci];
        const float e1u = q1.u - q0.u, e1v = q1.v - q0.v, e2u = q2.u - q0.u, e2v = q2.v - q0.v;
        const float detR = r.a * r.c;
        // J = Q * R^-1 with R^-1 = 1/detR * [[c, -b], [0, a]]
        const float j00 = kk * (e1u * r.c) / detR;
        const float j10 = kk * (e1v * r.c) / detR;
        const float j01 = kk * (-e1u * r.b + e2u * r.a) / detR;
        const float j11 = kk * (-e1v * r.b + e2v * r.a) / detR;
        const float det = (j00 * j11 - j01 * j10) * sign[ci];
        if (det < 1e-6f) return 1e30f; // folded or degenerate: forbidden
        const float f2 = j00 * j00 + j01 * j01 + j10 * j10 + j11 * j11;
        return r.area * f2 * (1.0f + 1.0f / (det * det));
    }

    float RingEnergy(int v) const {
        float e = 0.0f;
        for (int t : ring[v]) e += TriEnergy(t);
        return e;
    }
};

bool BuildCtx(RelaxCtx& c) {
    const LowPolyMesh& m = c.mesh;
    const size_t nV = m.vertices.size(), nT = m.triangles.size();
    if (nV == 0 || nT == 0) return false;

    // Components via union-find on shared vertex indices (the mesh is seam-split, so one per island)
    std::vector<int> parent(nV);
    for (size_t i = 0; i < nV; ++i) parent[i] = static_cast<int>(i);
    auto find = [&](int x) { while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    c.rest.resize(nT);
    c.ring.assign(nV, {});
    for (size_t t = 0; t < nT; ++t) {
        const auto& tri = m.triangles[t];
        parent[find(tri.indices[1])] = find(tri.indices[0]);
        parent[find(tri.indices[2])] = find(tri.indices[0]);
        for (int j = 0; j < 3; ++j) c.ring[tri.indices[j]].push_back(static_cast<int>(t));

        const Vec3& p0 = m.vertices[tri.indices[0]].position;
        const Vec3 e1 = m.vertices[tri.indices[1]].position - p0;
        const Vec3 e2 = m.vertices[tri.indices[2]].position - p0;
        const float a = e1.Length();
        RestTri r{ 0, 0, 0, 0, false };
        if (a > 1e-9f) {
            r.a = a;
            r.b = e1.Dot(e2) / a;
            r.c = e1.Cross(e2).Length() / a;
            r.area = 0.5f * a * r.c;
            r.ok = r.c > 1e-9f;
        }
        c.rest[t] = r;
    }
    c.comp.assign(nV, -1);
    std::unordered_map<int, int> ids;
    for (size_t i = 0; i < nV; ++i) {
        const int root = find(static_cast<int>(i));
        auto it = ids.find(root);
        if (it == ids.end()) it = ids.emplace(root, static_cast<int>(ids.size())).first;
        c.comp[i] = it->second;
    }
    const size_t nC = ids.size();
    std::vector<double> a3(nC, 0.0), aUV(nC, 0.0), aSigned(nC, 0.0);
    for (size_t t = 0; t < nT; ++t) {
        if (!c.rest[t].ok) continue;
        const auto& tri = m.triangles[t];
        const auto& v0 = m.vertices[tri.indices[0]]; const auto& v1 = m.vertices[tri.indices[1]]; const auto& v2 = m.vertices[tri.indices[2]];
        const double s = 0.5 * ((v1.uv.u - v0.uv.u) * (v2.uv.v - v0.uv.v) - (v2.uv.u - v0.uv.u) * (v1.uv.v - v0.uv.v));
        const int ci = c.comp[tri.indices[0]];
        a3[ci] += c.rest[t].area; aUV[ci] += std::abs(s); aSigned[ci] += s;
    }
    c.k.assign(nC, 0.0f);
    c.sign.assign(nC, 1.0f);
    bool any = false;
    for (size_t i = 0; i < nC; ++i) {
        if (a3[i] > 1e-12 && aUV[i] > 1e-12) { c.k[i] = static_cast<float>(std::sqrt(a3[i] / aUV[i])); any = true; }
        c.sign[i] = aSigned[i] < 0.0 ? -1.0f : 1.0f; // mirrored islands stay mirrored
    }
    return any;
}

float MeanDistortion(const RelaxCtx& c) {
    double e = 0.0, w = 0.0;
    for (size_t t = 0; t < c.rest.size(); ++t) {
        if (!c.rest[t].ok) continue;
        const float te = c.TriEnergy(static_cast<int>(t));
        if (te >= 1e29f) continue; // folded triangles are excluded from the mean
        e += te; w += 4.0 * c.rest[t].area;
    }
    return w > 0.0 ? static_cast<float>(e / w) : 0.0f;
}

} // namespace

float UVUnwrapper::MeasureDistortion(const LowPolyMesh& mesh) {
    RelaxCtx c(mesh);
    return BuildCtx(c) ? MeanDistortion(c) : 0.0f;
}

float UVUnwrapper::RelaxUVs(LowPolyMesh& mesh, int iterations) {
    RelaxCtx c(mesh);
    if (!BuildCtx(c)) return 0.0f;

    for (int iter = 0; iter < iterations; ++iter) {
        for (size_t vi = 0; vi < mesh.vertices.size(); ++vi) {
            const int v = static_cast<int>(vi);
            if (c.ring[v].empty() || c.k[c.comp[v]] <= 0.0f) continue;
            Vertex& vert = mesh.vertices[v];

            // Local UV scale: mean length of the ring edges touching this vertex
            float edgeLen = 0.0f; int n = 0;
            for (int t : c.ring[v]) {
                for (int j = 0; j < 3; ++j) {
                    const auto& o = mesh.vertices[mesh.triangles[t].indices[j]];
                    const float d = std::hypot(o.uv.u - vert.uv.u, o.uv.v - vert.uv.v);
                    if (d > 0.0f) { edgeLen += d; ++n; }
                }
            }
            if (n == 0) continue;
            edgeLen /= n;
            if (edgeLen < 1e-7f) continue;

            const float h = edgeLen * 1e-3f;
            const float u0 = vert.uv.u, v0 = vert.uv.v;
            const float e0 = c.RingEnergy(v);
            if (e0 >= 1e29f) continue; // already folded here: leave it alone
            vert.uv.u = u0 + h; const float eUp = c.RingEnergy(v);
            vert.uv.u = u0 - h; const float eUm = c.RingEnergy(v);
            vert.uv.u = u0;
            vert.uv.v = v0 + h; const float eVp = c.RingEnergy(v);
            vert.uv.v = v0 - h; const float eVm = c.RingEnergy(v);
            vert.uv.v = v0;
            if (eUp >= 1e29f || eUm >= 1e29f || eVp >= 1e29f || eVm >= 1e29f) continue; // too close to a fold
            const float gu = (eUp - eUm) / (2.0f * h), gv = (eVp - eVm) / (2.0f * h);
            const float gl = std::hypot(gu, gv);
            if (gl < 1e-9f) continue;

            float step = edgeLen * 0.3f;
            for (int ls = 0; ls < 8; ++ls, step *= 0.5f) {
                vert.uv.u = u0 - gu / gl * step;
                vert.uv.v = v0 - gv / gl * step;
                if (c.RingEnergy(v) < e0) break; // accepted (also rejects folds: they cost 1e30)
                vert.uv.u = u0; vert.uv.v = v0;  // no improving step yet
            }
        }
    }
    return MeanDistortion(c);
}

void UVUnwrapper::SnapIslandsToPixels(LowPolyMesh& mesh, int resolution) {
    if (resolution <= 0) return;
    RelaxCtx c(mesh);
    BuildCtx(c); // only the component ids and rings are needed
    if (c.comp.size() != mesh.vertices.size()) return;
    int nC = 0;
    for (int id : c.comp) nC = (std::max)(nC, id + 1);
    std::vector<float> minU(nC, 1e30f), minV(nC, 1e30f);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (c.ring[i].empty()) continue;
        minU[c.comp[i]] = (std::min)(minU[c.comp[i]], mesh.vertices[i].uv.u);
        minV[c.comp[i]] = (std::min)(minV[c.comp[i]], mesh.vertices[i].uv.v);
    }
    const float r = static_cast<float>(resolution);
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (c.ring[i].empty()) continue;
        const int ci = c.comp[i];
        mesh.vertices[i].uv.u += std::round(minU[ci] * r) / r - minU[ci];
        mesh.vertices[i].uv.v += std::round(minV[ci] * r) / r - minV[ci];
    }
}

} // namespace Baker
