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
    int seamPadding = 32;             // Dilation pixels (JFA nearest-texel)
    int antiAliasing = 2;             // 1 = 1x (Draft), 2 = 2x2 (4 rays/pixel), 4 = 4x4 (16 rays/pixel)
    bool discardBackfaceHits = true;  // Discard backfaces & grazing hits to prevent inverted border artifacts
    float angleThreshold = 75.0f;     // Max angle (degrees) between LP and HP normal (rejects cross-corner grazing hits)
    bool useBidirectionalRays = true; // Cast both inward from cage and outward from surface (Marmoset/xNormal standard)
    float maxOutwardDistance = 0.2f;  // Maximum distance to search along +normal for protruding sculpt peaks
    bool isolateMeshParts = false;    // Ray mask isolation per mesh/subtool part (prevents bleeding between adjacent stones)
    bool bakeDisplacement = true;     // 32-bit float EXR
    bool bakeNormal = true;           // Tangent-space normal map (MikkTSpace)
    bool bakeWorldNormal = false;     // Object/World-space normal map
    bool bakeAmbientOcclusion = false;// Cosine-weighted ray-traced AO map
    int aoSamples = 16;               // Number of hemisphere rays per hit for AO
    float aoMaxDistance = 0.5f;       // Maximum search distance for AO occlusion
    bool bakeCavity = true;           // Cavity map (micro-crevices, bark fissures, seams)
    bool bakeCurvature = false;       // Curvature map (0.5 = flat, 0.0 = valley, 1.0 = ridge)
    float cavityStrength = 1.5f;      // Cavity contrast / intensity multiplier
    bool bakeThickness = false;       // Translucency / subsurface scattering thickness map
    float thicknessMaxDistance = 0.5f;// Max search depth for inward thickness rays
    bool bakePosition = false;        // Normalized object/world position map (RGB in [0, 1])
    bool bakeMaterialID = true;       // Polygroup / SubTool material ID map
    bool bakeBaseColor = true;        // Polypaint / vertex color map
    bool bakeDiagnosticMap = true;    // Diagnostic map (Miss=Red, Hit=Green, Clamp=Yellow)
    bool exportPNG = true;            // Save 8-bit PNG for instant preview & game engines
    bool exportEXR = true;            // Save 16-bit half / 32-bit float EXR
    bool flipYNormal = false;         // Normal map format: false = OpenGL (+Y) (Blender/Maya/Unity), true = DirectX (-Y) (Unreal/3ds Max)
    bool useCompactBVH = false;       // Compact/low build quality for 50M-100M polys to conserve memory
    unsigned int groupMask = 0xFFFFFFFF; // SubTool ray mask for bake group isolation
    float lowPolyScale = 1.0f;        // Scale multiplier for low-poly (uniform)
    Vec3 lowPolyTranslation = Vec3(0, 0, 0); // Position offset (X, Y, Z)
    Vec3 lowPolyRotation = Vec3(0, 0, 0);    // Rotation (Pitch, Yaw, Roll in degrees)
    Vec3 lowPolyScaleVec = Vec3(1, 1, 1);    // Non-uniform scale (X, Y, Z)
    float skewFactor = 0.0f;          // Marmoset skew correction (0 = smooth normal, 1 = flat triangle normal)
    std::vector<int> targetUDIMs;     // Filtered UDIM tiles to bake (empty = all detected UDIMs)
    std::string outputDir = "./";
    std::string baseFilename = "Bake";
};

struct BakeResult {
    int udimTile = 1001;
    int width = 0;
    int height = 0;
    std::vector<float> displacementMap;  // [width * height] 32-bit float
    std::vector<float> normalMapR;       // [width * height] Tangent Normal
    std::vector<float> normalMapG;       // [width * height]
    std::vector<float> normalMapB;       // [width * height]
    std::vector<float> worldNormalR;     // [width * height] World / Object Space Normal
    std::vector<float> worldNormalG;
    std::vector<float> worldNormalB;
    std::vector<float> aoMap;            // [width * height] Ambient Occlusion
    std::vector<float> cavityMap;        // [width * height] Cavity (0 = crevice, 1 = flat)
    std::vector<float> curvatureMap;     // [width * height] Curvature (0.5 = flat, 0 = valley, 1 = ridge)
    std::vector<float> thicknessMap;     // [width * height] Thickness (0 = deep, 1 = thin)
    std::vector<float> positionMapR;     // [width * height] Normalized World Position
    std::vector<float> positionMapG;
    std::vector<float> positionMapB;
    std::vector<float> materialIdMapR;   // [width * height] Material / Polygroup ID
    std::vector<float> materialIdMapG;
    std::vector<float> materialIdMapB;
    std::vector<float> baseColorMapR;    // [width * height] Polypaint / Vertex Color
    std::vector<float> baseColorMapG;
    std::vector<float> baseColorMapB;
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

    using IntermediateCallback = std::function<void(int tile, int width, int height, const float* r, const float* g, const float* b)>;

    // Bakes all UDIM tiles detected on the low-poly mesh (with explicit cage support & ray masking)
    bool ExecuteBake(
        const LowPolyMesh& lowPoly,
        const BakeSettings& settings,
        std::vector<BakeResult>& outResults,
        std::function<void(float progress, const std::string& status)> progressCallback,
        std::string& outError,
        IntermediateCallback livePreviewCallback = nullptr
    );

    // Jump Flooding Algorithm (JFA) exact nearest-texel dilation across all active bake result buffers
    static void ApplyJumpFloodingDilation(
        BakeResult& res,
        int paddingPixels
    );

    // Legacy overload
    static void ApplyJumpFloodingDilation(
        std::vector<float>& displacement,
        std::vector<float>& normR,
        std::vector<float>& normG,
        std::vector<float>& normB,
        std::vector<float>& ao,
        std::vector<float>& cavity,
        std::vector<float>& curvature,
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
    struct SubToolGeomData {
        std::vector<Vec3> normals;
        std::vector<Vec3> colors;
        std::vector<uint32_t> polygroups;
        std::vector<uint32_t> indices;
        Vec3 bMin = Vec3(0, 0, 0);
        Vec3 bMax = Vec3(0, 0, 0);
    };

    void* m_embreeDevice = nullptr;
    void* m_embreeScene = nullptr;
    std::vector<SubToolGeomData> m_geoms;
    Vec3 m_sceneBMin = Vec3(0, 0, 0);
    Vec3 m_sceneBMax = Vec3(0, 0, 0);
    bool m_bvhReady = false;
};

} // namespace Baker

