#pragma once

#include "MeshTypes.h"
#include <string>
#include <vector>
#include <functional>

namespace Baker {

struct BakeSettings {
    int resolution = 2048;            // 1024, 2048, 4096, 8192
    float cageDistance = 0.05f;       // Normal push distance for ray start (used if no explicit cage)
    float maxRayDistance = 0.2f;      // Max ray length
    int seamPadding = 16;             // Dilation pixels (JFA nearest-texel)
    bool bakeDisplacement = true;     // 32-bit float EXR
    bool bakeNormal = true;           // Tangent-space normal map
    bool bakeDiagnosticMap = true;    // Diagnostic map (Miss/Hit/Clamp)
    bool flipYNormal = false;         // DirectX (-Y) vs OpenGL (+Y)
    bool useCompactBVH = false;       // Compact/low build quality for 50M-100M polys to conserve memory
    unsigned int groupMask = 0xFFFFFFFF; // SubTool ray mask for bake group isolation
    std::string outputDir = "./";
    std::string baseFilename = "Bake";
};

struct BakeResult {
    int udimTile;
    int width;
    int height;
    std::vector<float> displacementMap;  // [width * height] 32-bit float
    std::vector<float> normalMapR;       // [width * height]
    std::vector<float> normalMapG;       // [width * height]
    std::vector<float> normalMapB;       // [width * height]
    std::vector<float> diagnosticMapR;   // [width * height] (Miss=Red, Hit=Green, Clamp=Yellow)
    std::vector<float> diagnosticMapG;   // [width * height]
    std::vector<float> diagnosticMapB;   // [width * height]
    std::vector<uint8_t> hitMask;        // 1 if sampled, 0 if background
};

class BakerEngine {
public:
    BakerEngine();
    ~BakerEngine();

    // Initializes Embree ray tracing scene from a single high-poly mesh
    bool BuildHighPolyBVH(const HighPolyMesh& highPoly, std::string& outError);

    // Initializes Embree ray tracing scene from multiple high-poly SubTools with individual geometry masks
    bool BuildMultiSubToolBVH(
        const std::vector<HighPolyMesh>& subtools,
        bool compactQuality,
        std::string& outError
    );

    // Bakes all UDIM tiles detected on the low-poly mesh (with explicit cage support & ray masking)
    bool ExecuteBake(
        const LowPolyMesh& lowPoly,
        const BakeSettings& settings,
        std::vector<BakeResult>& outResults,
        std::function<void(float progress, const std::string& status)> progressCallback,
        std::string& outError
    );

    // Jump Flooding Algorithm (JFA) exact nearest-texel dilation (no float drift, re-normalized normals)
    static void ApplyJumpFloodingDilation(
        std::vector<float>& displacement,
        std::vector<float>& normR,
        std::vector<float>& normG,
        std::vector<float>& normB,
        const std::vector<uint8_t>& mask,
        int width,
        int height,
        int paddingPixels
    );

    // Backward-compatible seam padding wrapper using JFA
    static void ApplySeamPadding(
        std::vector<float>& channel,
        const std::vector<uint8_t>& mask,
        int width,
        int height,
        int paddingPixels
    );

    void Release();

private:
    void* m_embreeDevice = nullptr;
    void* m_embreeScene = nullptr;
    bool m_bvhReady = false;
};

} // namespace Baker
