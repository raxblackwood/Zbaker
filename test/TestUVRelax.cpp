// Self-check for UVUnwrapper::RelaxUVs / MeasureDistortion / SnapIslandsToPixels.
// Build: cl /std:c++17 /EHsc /Isrc/baker /Isrc/uv test/TestUVRelax.cpp src/uv/UVUnwrapper.cpp
#include "UVUnwrapper.h"
#include <cstdio>
#include <cmath>

using namespace Baker;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_fail; } } while (0)

// n x n vertex flat grid of unit quads (2 triangles each) with a sheared, non-uniformly scaled UV
static LowPolyMesh MakeGrid(int n) {
    LowPolyMesh m;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            Vertex v{};
            v.position = Vec3((float)x, 0.0f, (float)y);
            v.normal = Vec3(0, 1, 0);
            const bool inner = x > 0 && y > 0 && x < n - 1 && y < n - 1;
            const float jitter = inner ? 0.18f * std::sin(1.7f * x + 2.3f * y) : 0.0f; // interior wobble
            v.uv = Vec2(0.05f * (x + 0.8f * y + jitter), 0.01f * (y + jitter));        // sheared + squashed
            m.vertices.push_back(v);
        }
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            LowPolyTriangle t1{}; t1.indices[0] = a; t1.indices[1] = c; t1.indices[2] = b; m.triangles.push_back(t1);
            LowPolyTriangle t2{}; t2.indices[0] = b; t2.indices[1] = c; t2.indices[2] = d; m.triangles.push_back(t2);
        }
    return m;
}

static bool NoFolds(const LowPolyMesh& m, float sign) {
    for (const auto& t : m.triangles) {
        const auto& a = m.vertices[t.indices[0]].uv; const auto& b = m.vertices[t.indices[1]].uv; const auto& c = m.vertices[t.indices[2]].uv;
        const float det = (b.u - a.u) * (c.v - a.v) - (c.u - a.u) * (b.v - a.v);
        if (det * sign <= 0.0f) return false;
    }
    return true;
}

int main() {
    LowPolyMesh m = MakeGrid(6);
    const float sign = ((m.vertices[m.triangles[0].indices[1]].uv.u - m.vertices[m.triangles[0].indices[0]].uv.u) *
                        (m.vertices[m.triangles[0].indices[2]].uv.v - m.vertices[m.triangles[0].indices[0]].uv.v) -
                        (m.vertices[m.triangles[0].indices[2]].uv.u - m.vertices[m.triangles[0].indices[0]].uv.u) *
                        (m.vertices[m.triangles[0].indices[1]].uv.v - m.vertices[m.triangles[0].indices[0]].uv.v)) < 0 ? -1.0f : 1.0f;
    CHECK(NoFolds(m, sign));

    const float before = UVUnwrapper::MeasureDistortion(m);
    CHECK(before > 1.5f);
    const float after = UVUnwrapper::RelaxUVs(m, 60);
    std::printf("distortion %.3f -> %.3f\n", before, after);
    CHECK(after < before);
    CHECK(after >= 1.0f - 1e-3f);   // can never beat isometric
    CHECK(NoFolds(m, sign));        // relax must stay flip-free

    // A mesh with all-zero UVs is reported as having nothing to relax
    LowPolyMesh z = MakeGrid(3);
    for (auto& v : z.vertices) v.uv = Vec2(0, 0);
    CHECK(UVUnwrapper::MeasureDistortion(z) == 0.0f);

    // Snap lands the island's min corner on a texel boundary and keeps its shape
    const float w0 = m.vertices[1].uv.u - m.vertices[0].uv.u;
    UVUnwrapper::SnapIslandsToPixels(m, 256);
    float minU = 1e9f, minV = 1e9f;
    for (const auto& v : m.vertices) { minU = std::fmin(minU, v.uv.u); minV = std::fmin(minV, v.uv.v); }
    CHECK(std::fabs(minU * 256.0f - std::round(minU * 256.0f)) < 1e-3f);
    CHECK(std::fabs(minV * 256.0f - std::round(minV * 256.0f)) < 1e-3f);
    CHECK(std::fabs((m.vertices[1].uv.u - m.vertices[0].uv.u) - w0) < 1e-5f);

    std::printf(g_fail ? "FAILED\n" : "OK\n");
    return g_fail ? 1 : 0;
}
