#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <algorithm>
#include <set>
#include <utility>

namespace Baker {

struct Vec2 {
    float u = 0.0f;
    float v = 0.0f;

    Vec2() = default;
    Vec2(float _u, float _v) : u(_u), v(_v) {}
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    Vec3() = default;
    Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }

    float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 Cross(const Vec3& o) const {
        return Vec3(
            y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x
        );
    }
    float Length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 Normalized() const {
        float len = Length();
        if (len > 1e-20f) {
            return *this * (1.0f / len);
        }
        return *this;
    }
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f; // Sign for bitangent in MikkTSpace

    Vec4() = default;
    Vec4(float _x, float _y, float _z, float _w = 1.0f) : x(_x), y(_y), z(_z), w(_w) {}
};

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 tangent;
};

// Represents a triangle on a low-poly mesh
struct LowPolyTriangle {
    uint32_t indices[3];
    int udimTile = 1001;              // Detected UDIM tile (e.g. 1001, 1002...)
    uint32_t meshIndex = 0;           // Source mesh / node / subtool index
    uint32_t groupMask = 0xFFFFFFFF;  // Bitmask for bake group isolation
};

// Represents a low-poly mesh configured for UDIM baking
struct LowPolyMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // Always triangulated (size % 3 == 0)
    std::vector<LowPolyTriangle> triangles;
    std::vector<int> detectedUDIMs; // Unique sorted list of UDIM tiles (e.g., 1001, 1002...)
    std::vector<Vec3> cagePositions; // Optional explicit cage vertex positions
    std::vector<Vec3> cageNormals;   // Watertight averaged normals across UV seams for continuous envelope

    // Original polygon topology preserved from source DCC / ZBrush (Quads & Tris)
    uint32_t quadCount = 0;
    uint32_t triCount = 0;
    uint32_t polyCount = 0;
    std::vector<std::vector<uint32_t>> polygonFaces; // Original polygon faces (quads, n-gons, tris)
    std::vector<uint32_t> wireframeLineIndices; // Perimeter line edges of original faces (NO internal diagonals!)
    std::vector<uint32_t> seamLineIndices;      // Marked UV cut seams for 3D/2D viewport line rendering
    std::set<std::pair<uint32_t, uint32_t>> cutSeams; // Set of edge pairs (vMin, vMax) marked as seams
    std::set<std::pair<uint32_t, uint32_t>> selectedEdges; // Set of edge pairs (vMin, vMax) selected for Cut/Sew
    std::vector<uint32_t> selectedEdgeLineIndices; // Line indices for rendering selected edges

    void GenerateSelectedEdgeLines() {
        selectedEdgeLineIndices.clear();
        selectedEdgeLineIndices.reserve(selectedEdges.size() * 2);
        for (const auto& edge : selectedEdges) {
            selectedEdgeLineIndices.push_back(edge.first);
            selectedEdgeLineIndices.push_back(edge.second);
        }
    }

    Vec3 translation = Vec3(0, 0, 0);
    Vec3 rotation = Vec3(0, 0, 0); // Pitch, Yaw, Roll in degrees
    Vec3 scale = Vec3(1, 1, 1);
    float skewFactor = 0.0f; // Marmoset skew correction (0 = smooth, 1 = flat)
    std::vector<float> cageOffsets; // Optional per-vertex cage offsets
    std::vector<float> vertexSkew;  // Optional per-vertex painted skew correction factor [0..1]

    bool HasExplicitCage() const {
        return !cagePositions.empty() && cagePositions.size() == vertices.size();
    }

    bool HasWatertightCageNormals() const {
        return !cageNormals.empty() && cageNormals.size() == vertices.size();
    }

    bool HasVertexSkew() const {
        return !vertexSkew.empty() && vertexSkew.size() == vertices.size();
    }

    bool HasCageOffsets() const {
        return !cageOffsets.empty() && cageOffsets.size() == vertices.size();
    }

    // Fast Möller-Trumbore ray-mesh intersection (returns true if hit, with hitT, hitPos, hitNormal, hitTriIdx)
    bool Raycast(const Vec3& rayOrg, const Vec3& rayDir, float& hitT, Vec3& hitPos, Vec3& hitNormal, int& hitTriIdx) const {
        hitT = 1e30f;
        hitTriIdx = -1;
        bool hasHit = false;

        for (int i = 0; i < (int)triangles.size(); ++i) {
            const auto& tri = triangles[i];
            const Vec3& v0 = vertices[tri.indices[0]].position;
            const Vec3& v1 = vertices[tri.indices[1]].position;
            const Vec3& v2 = vertices[tri.indices[2]].position;

            Vec3 e1 = v1 - v0;
            Vec3 e2 = v2 - v0;
            Vec3 pvec = rayDir.Cross(e2);
            float det = e1.Dot(pvec);

            if (std::abs(det) < 1e-8f) continue;
            float invDet = 1.0f / det;

            Vec3 tvec = rayOrg - v0;
            float u = tvec.Dot(pvec) * invDet;
            if (u < 0.0f || u > 1.0f) continue;

            Vec3 qvec = tvec.Cross(e1);
            float v = rayDir.Dot(qvec) * invDet;
            if (v < 0.0f || u + v > 1.0f) continue;

            float t = e2.Dot(qvec) * invDet;
            if (t > 1e-4f && t < hitT) {
                hitT = t;
                hitTriIdx = i;
                hasHit = true;
                hitPos = rayOrg + rayDir * t;
                hitNormal = e1.Cross(e2).Normalized();
                if (det < 0.0f) hitNormal = hitNormal * -1.0f;
            }
        }
        return hasHit;
    }

    // Computes angle-aware smooth vertex normals respecting creaseAngleDeg (default 180 deg for standard smooth shading)
    // Preserves sharp edges when specified, or smoothly averages continuous surfaces matching DCC smooth shading
    void ComputeSmoothVertexNormals(float creaseAngleDeg = 180.0f) {
        if (vertices.empty() || triangles.empty()) return;

        float minCos = std::cos(creaseAngleDeg * 3.14159265f / 180.0f);

        struct PosKey {
            int64_t x, y, z;
            bool operator==(const PosKey& o) const {
                return x == o.x && y == o.y && z == o.z;
            }
        };
        struct PosHasher {
            size_t operator()(const PosKey& k) const {
                size_t h = 14695981039346656037ULL;
                h ^= static_cast<size_t>(k.x); h *= 1099511628211ULL;
                h ^= static_cast<size_t>(k.y); h *= 1099511628211ULL;
                h ^= static_cast<size_t>(k.z); h *= 1099511628211ULL;
                return h;
            }
        };

        auto Quantize = [](const Vec3& p) -> PosKey {
            return PosKey{
                static_cast<int64_t>(std::floor(p.x * 100000.0f + 0.5f)),
                static_cast<int64_t>(std::floor(p.y * 100000.0f + 0.5f)),
                static_cast<int64_t>(std::floor(p.z * 100000.0f + 0.5f))
            };
        };

        struct TriFaceInfo {
            Vec3 areaNormal; // Unnormalized area-weighted face normal
            Vec3 unitNormal; // Unit face normal
        };
        std::vector<TriFaceInfo> triFaces(triangles.size());
        std::unordered_map<PosKey, std::vector<uint32_t>, PosHasher> posToTriangles;
        posToTriangles.reserve(vertices.size());

        for (size_t t = 0; t < triangles.size(); ++t) {
            const auto& tri = triangles[t];
            const auto& p0 = vertices[tri.indices[0]].position;
            const auto& p1 = vertices[tri.indices[1]].position;
            const auto& p2 = vertices[tri.indices[2]].position;

            Vec3 fn = (p1 - p0).Cross(p2 - p0);
            triFaces[t].areaNormal = fn;
            triFaces[t].unitNormal = fn.Normalized();

            posToTriangles[Quantize(p0)].push_back(static_cast<uint32_t>(t));
            posToTriangles[Quantize(p1)].push_back(static_cast<uint32_t>(t));
            posToTriangles[Quantize(p2)].push_back(static_cast<uint32_t>(t));
        }

        for (size_t t = 0; t < triangles.size(); ++t) {
            const auto& tri = triangles[t];
            const Vec3& myUnitNorm = triFaces[t].unitNormal;

            for (int c = 0; c < 3; ++c) {
                uint32_t vIdx = tri.indices[c];
                PosKey k = Quantize(vertices[vIdx].position);

                Vec3 accum(0, 0, 0);
                auto it = posToTriangles.find(k);
                if (it != posToTriangles.end()) {
                    for (uint32_t adjTri : it->second) {
                        const Vec3& adjUnitNorm = triFaces[adjTri].unitNormal;
                        if (myUnitNorm.Dot(adjUnitNorm) >= minCos) {
                            accum = accum + triFaces[adjTri].areaNormal;
                        }
                    }
                }

                if (accum.Length() > 1e-20f) {
                    vertices[vIdx].normal = accum.Normalized();
                } else if (myUnitNorm.Length() > 1e-20f) {
                    vertices[vIdx].normal = myUnitNorm;
                }
            }
        }
    }

    void SmoothNormalsIfMissing(float creaseAngleDeg = 180.0f) {
        bool needsSmoothing = false;
        for (const auto& v : vertices) {
            if (v.normal.Length() < 1e-4f) {
                needsSmoothing = true;
                break;
            }
        }
        if (!needsSmoothing && triangles.size() > 1) {
            int flatTriCount = 0;
            for (const auto& tri : triangles) {
                const Vec3& n0 = vertices[tri.indices[0]].normal;
                const Vec3& n1 = vertices[tri.indices[1]].normal;
                const Vec3& n2 = vertices[tri.indices[2]].normal;
                if (std::abs(n0.Dot(n1) - 1.0f) < 1e-4f && std::abs(n0.Dot(n2) - 1.0f) < 1e-4f) {
                    flatTriCount++;
                }
            }
            if (flatTriCount == (int)triangles.size()) {
                needsSmoothing = true;
            }
        }
        if (needsSmoothing) {
            ComputeSmoothVertexNormals(creaseAngleDeg);
        }
    }

    // Computes continuous, watertight cage normals by welding positions across UV seams
    // Respects maxWeldAngleDeg (default 40 deg) to prevent rays from crossing bevels and 90-deg sharp corners
    void ComputeWatertightCageNormals(float maxWeldAngleDeg = 40.0f) {
        cageNormals.assign(vertices.size(), Vec3(0, 0, 0));
        if (vertices.empty() || triangles.empty()) return;

        float minCos = std::cos(maxWeldAngleDeg * 3.14159265f / 180.0f);

        struct PosKey {
            int64_t x, y, z;
            bool operator==(const PosKey& o) const {
                return x == o.x && y == o.y && z == o.z;
            }
        };
        struct PosHasher {
            size_t operator()(const PosKey& k) const {
                size_t h = 14695981039346656037ULL;
                h ^= static_cast<size_t>(k.x); h *= 1099511628211ULL;
                h ^= static_cast<size_t>(k.y); h *= 1099511628211ULL;
                h ^= static_cast<size_t>(k.z); h *= 1099511628211ULL;
                return h;
            }
        };

        auto Quantize = [](const Vec3& p) -> PosKey {
            return PosKey{
                static_cast<int64_t>(std::floor(p.x * 100000.0f + 0.5f)),
                static_cast<int64_t>(std::floor(p.y * 100000.0f + 0.5f)),
                static_cast<int64_t>(std::floor(p.z * 100000.0f + 0.5f))
            };
        };

        struct TriFaceInfo {
            Vec3 areaNormal;
            Vec3 unitNormal;
        };
        std::vector<TriFaceInfo> triFaces(triangles.size());
        std::unordered_map<PosKey, std::vector<uint32_t>, PosHasher> posToTriangles;
        posToTriangles.reserve(vertices.size());

        for (size_t t = 0; t < triangles.size(); ++t) {
            const auto& tri = triangles[t];
            const auto& p0 = vertices[tri.indices[0]].position;
            const auto& p1 = vertices[tri.indices[1]].position;
            const auto& p2 = vertices[tri.indices[2]].position;

            Vec3 fn = (p1 - p0).Cross(p2 - p0);
            triFaces[t].areaNormal = fn;
            triFaces[t].unitNormal = fn.Normalized();

            posToTriangles[Quantize(p0)].push_back(static_cast<uint32_t>(t));
            posToTriangles[Quantize(p1)].push_back(static_cast<uint32_t>(t));
            posToTriangles[Quantize(p2)].push_back(static_cast<uint32_t>(t));
        }

        for (size_t t = 0; t < triangles.size(); ++t) {
            const auto& tri = triangles[t];
            const Vec3& myUnitNorm = triFaces[t].unitNormal;

            for (int c = 0; c < 3; ++c) {
                uint32_t vIdx = tri.indices[c];
                PosKey k = Quantize(vertices[vIdx].position);

                Vec3 accum(0, 0, 0);
                auto it = posToTriangles.find(k);
                if (it != posToTriangles.end()) {
                    for (uint32_t adjTri : it->second) {
                        const Vec3& adjUnitNorm = triFaces[adjTri].unitNormal;
                        if (myUnitNorm.Dot(adjUnitNorm) >= minCos) {
                            accum = accum + triFaces[adjTri].areaNormal;
                        }
                    }
                }

                if (accum.Length() > 1e-20f) {
                    cageNormals[vIdx] = accum.Normalized();
                } else if (vertices[vIdx].normal.Length() > 1e-20f) {
                    cageNormals[vIdx] = vertices[vIdx].normal.Normalized();
                } else {
                    cageNormals[vIdx] = myUnitNorm;
                }
            }
        }
    }

    Vec3 GetBoundingBoxMin() const {
        if (vertices.empty()) return Vec3(0, 0, 0);
        Vec3 minB = vertices[0].position;
        for (const auto& v : vertices) {
            minB.x = (std::min)(minB.x, v.position.x);
            minB.y = (std::min)(minB.y, v.position.y);
            minB.z = (std::min)(minB.z, v.position.z);
        }
        return minB;
    }

    Vec3 GetBoundingBoxMax() const {
        if (vertices.empty()) return Vec3(0, 0, 0);
        Vec3 maxB = vertices[0].position;
        for (const auto& v : vertices) {
            maxB.x = (std::max)(maxB.x, v.position.x);
            maxB.y = (std::max)(maxB.y, v.position.y);
            maxB.z = (std::max)(maxB.z, v.position.z);
        }
        return maxB;
    }

    Vec3 GetBoundingBoxCenter() const {
        return (GetBoundingBoxMin() + GetBoundingBoxMax()) * 0.5f;
    }

    float GetBoundingBoxDiagonal() const {
        return (GetBoundingBoxMax() - GetBoundingBoxMin()).Length();
    }

    void Clear() {
        vertices.clear();
        indices.clear();
        triangles.clear();
        detectedUDIMs.clear();
        cagePositions.clear();
        cageNormals.clear();
        cageOffsets.clear();
        vertexSkew.clear();
        quadCount = 0;
        triCount = 0;
        polyCount = 0;
        polygonFaces.clear();
        wireframeLineIndices.clear();
        seamLineIndices.clear();
        cutSeams.clear();
        selectedEdges.clear();
        selectedEdgeLineIndices.clear();
        translation = Vec3(0, 0, 0);
        rotation = Vec3(0, 0, 0);
        scale = Vec3(1, 1, 1);
        skewFactor = 0.0f;
    }
};

// Represents a high-poly SubTool mesh ingested for ray tracing
struct HighPolyMesh {
    std::string name;
    unsigned int maskId = 1; // Ray mask (1, 2, 4, 8, etc.) for Bake Groups
    bool isVisible = true;
    bool isQuadMesh = false;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> colors; // Polypaint / Vertex Colors (RGB in [0, 1])
    std::vector<uint32_t> polygroupPerFace; // Polygroup ID per triangle face (for Material ID map)
    std::vector<uint32_t> indices; // Triangulated
    std::vector<uint32_t> quadIndices; // 4 indices per quad (for Embree quad geometry)

    Vec3 GetBoundingBoxMin() const {
        if (positions.empty()) return Vec3(0, 0, 0);
        Vec3 minB = positions[0];
        for (const auto& p : positions) {
            minB.x = (std::min)(minB.x, p.x);
            minB.y = (std::min)(minB.y, p.y);
            minB.z = (std::min)(minB.z, p.z);
        }
        return minB;
    }

    Vec3 GetBoundingBoxMax() const {
        if (positions.empty()) return Vec3(0, 0, 0);
        Vec3 maxB = positions[0];
        for (const auto& p : positions) {
            maxB.x = (std::max)(maxB.x, p.x);
            maxB.y = (std::max)(maxB.y, p.y);
            maxB.z = (std::max)(maxB.z, p.z);
        }
        return maxB;
    }

    Vec3 GetBoundingBoxCenter() const {
        return (GetBoundingBoxMin() + GetBoundingBoxMax()) * 0.5f;
    }

    float GetBoundingBoxDiagonal() const {
        return (GetBoundingBoxMax() - GetBoundingBoxMin()).Length();
    }
    
    void Clear() {
        isQuadMesh = false;
        positions.clear();
        normals.clear();
        colors.clear();
        polygroupPerFace.clear();
        indices.clear();
        quadIndices.clear();
    }
};

// Math helpers for transformations & color ID generation
inline Vec3 RotateEuler(const Vec3& v, const Vec3& deg) {
    float radX = deg.x * 3.14159265f / 180.0f;
    float radY = deg.y * 3.14159265f / 180.0f;
    float radZ = deg.z * 3.14159265f / 180.0f;
    // Rotation around X
    float cx = std::cos(radX), sx = std::sin(radX);
    Vec3 p = { v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx };
    // Rotation around Y
    float cy = std::cos(radY), sy = std::sin(radY);
    p = { p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy };
    // Rotation around Z
    float cz = std::cos(radZ), sz = std::sin(radZ);
    return { p.x * cz - p.y * sz, p.x * sz + p.y * cz, p.z };
}

inline Vec3 TransformPoint(const Vec3& p, const Vec3& t, const Vec3& r, const Vec3& s) {
    Vec3 scaled = { p.x * s.x, p.y * s.y, p.z * s.z };
    Vec3 rotated = RotateEuler(scaled, r);
    return rotated + t;
}

inline Vec3 TransformDirection(const Vec3& d, const Vec3& r, const Vec3& s) {
    Vec3 scaled = { d.x / (std::abs(s.x) > 1e-6f ? s.x : 1.0f),
                   d.y / (std::abs(s.y) > 1e-6f ? s.y : 1.0f),
                   d.z / (std::abs(s.z) > 1e-6f ? s.z : 1.0f) };
    return RotateEuler(scaled, r).Normalized();
}

inline Vec3 HSVtoRGB(float h, float s, float v) {
    float c = v * s;
    float x = c * (1.0f - std::abs(std::fmod(h * 6.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;
    int hi = static_cast<int>(std::floor(h * 6.0f)) % 6;
    switch (hi) {
        case 0: r = c; g = x; b = 0; break;
        case 1: r = x; g = c; b = 0; break;
        case 2: r = 0; g = c; b = x; break;
        case 3: r = 0; g = x; b = c; break;
        case 4: r = x; g = 0; b = c; break;
        case 5: r = c; g = 0; b = x; break;
    }
    return Vec3(r + m, g + m, b + m);
}

// Represents a multi-SubTool manifest for production baking
struct BakeGroup {
    std::string name = "DefaultGroup";
    unsigned int groupMask = 0xFFFFFFFF; // Bitmask of SubTools to intersect
    std::vector<int> targetUDIMs;         // Target UDIM tiles for this group
};

struct MultiSubToolManifest {
    std::vector<HighPolyMesh> subtools;
    std::vector<BakeGroup> groups;
};

// UDIM Helper: calculates UDIM tile ID from UV coordinate
// Tile = 1001 + floor(u) + 10 * floor(v)
inline int CalculateUDIMTile(float u, float v) {
    int tileU = static_cast<int>(std::floor(u));
    int tileV = static_cast<int>(std::floor(v));
    if (tileU < 0 || tileU >= 10 || tileV < 0) {
        // Clamp or fallback to 1001 if out-of-range negative
        if (tileU < 0) tileU = 0;
        if (tileV < 0) tileV = 0;
    }
    return 1001 + tileU + (10 * tileV);
}

} // namespace Baker
