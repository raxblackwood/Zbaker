#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>

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
        if (len > 1e-7f) {
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
    int udimTile = 1001; // Detected UDIM tile (e.g. 1001, 1002...)
};

// Represents a low-poly mesh configured for UDIM baking
struct LowPolyMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // Always triangulated (size % 3 == 0)
    std::vector<LowPolyTriangle> triangles;
    std::vector<int> detectedUDIMs; // Unique sorted list of UDIM tiles (e.g., 1001, 1002...)
    std::vector<Vec3> cagePositions; // Optional explicit cage vertex positions

    bool HasExplicitCage() const {
        return !cagePositions.empty() && cagePositions.size() == vertices.size();
    }

    void Clear() {
        vertices.clear();
        indices.clear();
        triangles.clear();
        detectedUDIMs.clear();
        cagePositions.clear();
    }
};

// Represents a high-poly SubTool mesh ingested for ray tracing
struct HighPolyMesh {
    std::string name;
    unsigned int maskId = 1; // Ray mask (1, 2, 4, 8, etc.) for Bake Groups
    bool isVisible = true;
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<uint32_t> indices; // Triangulated
    
    void Clear() {
        positions.clear();
        normals.clear();
        indices.clear();
    }
};

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
