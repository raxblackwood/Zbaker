#pragma once

#include "../baker/MeshTypes.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace Baker {

struct UVPoint {
    float u = 0.0f;
    float v = 0.0f;

    UVPoint() = default;
    UVPoint(float _u, float _v) : u(_u), v(_v) {}

    UVPoint operator+(const UVPoint& o) const { return UVPoint(u + o.u, v + o.v); }
    UVPoint operator-(const UVPoint& o) const { return UVPoint(u - o.u, v - o.v); }
    UVPoint operator*(float s) const { return UVPoint(u * s, v * s); }

    float LengthSq() const { return u * u + v * v; }
    float Length() const { return std::sqrt(LengthSq()); }
};

struct UVRect {
    float minU = 0.0f;
    float minV = 0.0f;
    float maxU = 0.0f;
    float maxV = 0.0f;

    UVRect() = default;
    UVRect(float _minU, float _minV, float _maxU, float _maxV)
        : minU(_minU), minV(_minV), maxU(_maxU), maxV(_maxV) {}

    float Width() const { return std::max(0.0f, maxU - minU); }
    float Height() const { return std::max(0.0f, maxV - minV); }
    float Area() const { return Width() * Height(); }

    bool Contains(const UVPoint& p) const {
        return p.u >= minU && p.u <= maxU && p.v >= minV && p.v <= maxV;
    }

    bool Intersects(const UVRect& o) const {
        return !(maxU <= o.minU || minU >= o.maxU || maxV <= o.minV || minV >= o.maxV);
    }
};

struct UVIsland {
    int islandId = 0;
    int polygroup = 0;
    int targetUDIM = 1001;

    // Indices of triangles in LowPolyMesh::triangles that belong to this island
    std::vector<int> triangleIndices;

    // Unique vertex indices referenced by this island
    std::vector<uint32_t> vertexIndices;

    // Local 2D UV positions corresponding to LowPolyMesh::vertices[vertexIndices[i]]
    // (Keyed by vertex index or per-triangle loop)
    std::vector<UVPoint> uvs;

    UVRect bounds;
    float area3D = 0.0f;
    float areaUV = 0.0f;
    float rotationRad = 0.0f;

    void ComputeBounds() {
        if (uvs.empty()) {
            bounds = UVRect(0, 0, 0, 0);
            return;
        }
        bounds.minU = uvs[0].u;
        bounds.maxU = uvs[0].u;
        bounds.minV = uvs[0].v;
        bounds.maxV = uvs[0].v;

        for (size_t i = 1; i < uvs.size(); ++i) {
            bounds.minU = (std::min)(bounds.minU, uvs[i].u);
            bounds.maxU = (std::max)(bounds.maxU, uvs[i].u);
            bounds.minV = (std::min)(bounds.minV, uvs[i].v);
            bounds.maxV = (std::max)(bounds.maxV, uvs[i].v);
        }
    }

    void Translate(float du, float dv) {
        for (auto& p : uvs) {
            p.u += du;
            p.v += dv;
        }
        bounds.minU += du;
        bounds.maxU += du;
        bounds.minV += dv;
        bounds.maxV += dv;
    }

    void Scale(float s) {
        if (s <= 1e-7f) return;
        UVPoint center((bounds.minU + bounds.maxU) * 0.5f, (bounds.minV + bounds.maxV) * 0.5f);
        for (auto& p : uvs) {
            p.u = center.u + (p.u - center.u) * s;
            p.v = center.v + (p.v - center.v) * s;
        }
        ComputeBounds();
    }

    void Rotate(float rad) {
        if (std::abs(rad) < 1e-6f) return;
        float cosA = std::cos(rad);
        float sinA = std::sin(rad);
        UVPoint center((bounds.minU + bounds.maxU) * 0.5f, (bounds.minV + bounds.maxV) * 0.5f);

        for (auto& p : uvs) {
            float du = p.u - center.u;
            float dv = p.v - center.v;
            p.u = center.u + (du * cosA - dv * sinA);
            p.v = center.v + (du * sinA + dv * cosA);
        }
        rotationRad += rad;
        ComputeBounds();
    }
};

} // namespace Baker
