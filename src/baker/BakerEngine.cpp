#include "BakerEngine.h"
#include <embree3/rtcore.h>

#include <cmath>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>

namespace Baker {

BakerEngine::BakerEngine() = default;

BakerEngine::~BakerEngine() {
    Release();
}

void BakerEngine::Release() {
    if (m_embreeScene) {
        rtcReleaseScene(static_cast<RTCScene>(m_embreeScene));
        m_embreeScene = nullptr;
    }
    if (m_embreeDevice) {
        rtcReleaseDevice(static_cast<RTCDevice>(m_embreeDevice));
        m_embreeDevice = nullptr;
    }
    m_geoms.clear();
    m_bvhReady = false;
}

bool BakerEngine::BuildHighPolyBVH(const HighPolyMesh& highPoly, std::string& outError) {
    std::vector<HighPolyMesh> list;
    list.push_back(highPoly);
    return BuildMultiSubToolBVH(list, false, outError);
}

// Backface intersection filter for Embree 3
static void BackfaceFilter(const struct RTCFilterFunctionNArguments* args) {
    int* valid = args->valid;
    unsigned int N = args->N;
    for (unsigned int i = 0; i < N; ++i) {
        if (valid[i] != -1) continue;
        RTCRay ray = rtcGetRayFromRayN(args->ray, N, i);
        RTCHit hit = rtcGetHitFromHitN(args->hit, N, i);
        float dot = ray.dir_x * hit.Ng_x + ray.dir_y * hit.Ng_y + ray.dir_z * hit.Ng_z;
        if (dot >= 0.0f) {
            valid[i] = 0; // reject backface hit, let Embree continue search
        }
    }
}

bool BakerEngine::BuildMultiSubToolBVH(
    const std::vector<HighPolyMesh>& subtools,
    bool compactQuality,
    std::string& outError
) {
    Release();

    if (subtools.empty()) {
        outError = "No SubTools provided for BVH generation";
        return false;
    }

    RTCDevice device = rtcNewDevice(nullptr);
    if (!device) {
        outError = "Failed to initialize Embree device";
        return false;
    }
    m_embreeDevice = device;

    RTCScene scene = rtcNewScene(device);
    if (!scene) {
        outError = "Failed to create Embree scene";
        return false;
    }
    m_embreeScene = scene;

    if (compactQuality) {
        rtcSetSceneBuildQuality(scene, RTC_BUILD_QUALITY_LOW);
        rtcSetSceneFlags(scene, RTC_SCENE_FLAG_COMPACT);
    }

    size_t attachedCount = 0;

    m_sceneBMin = Vec3(1e9f, 1e9f, 1e9f);
    m_sceneBMax = Vec3(-1e9f, -1e9f, -1e9f);

    for (const auto& subtool : subtools) {
        if (!subtool.isVisible || subtool.positions.empty() || subtool.indices.empty()) {
            continue;
        }

        RTCGeometry geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_TRIANGLE);
        if (compactQuality) {
            rtcSetGeometryBuildQuality(geom, RTC_BUILD_QUALITY_LOW);
        }

        size_t numVerts = subtool.positions.size();
        size_t numTris = subtool.indices.size() / 3;

        float* vb = static_cast<float*>(rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_VERTEX, 0, RTC_FORMAT_FLOAT3, 3 * sizeof(float), numVerts
        ));

        for (size_t i = 0; i < numVerts; ++i) {
            vb[3 * i + 0] = subtool.positions[i].x;
            vb[3 * i + 1] = subtool.positions[i].y;
            vb[3 * i + 2] = subtool.positions[i].z;
        }

        unsigned int* ib = static_cast<unsigned int*>(rtcSetNewGeometryBuffer(
            geom, RTC_BUFFER_TYPE_INDEX, 0, RTC_FORMAT_UINT3, 3 * sizeof(unsigned int), numTris
        ));

        for (size_t i = 0; i < subtool.indices.size(); ++i) {
            ib[i] = subtool.indices[i];
        }

        // Set SubTool group ray mask
        rtcSetGeometryMask(geom, subtool.maskId);

        rtcCommitGeometry(geom);
        unsigned int geomID = rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);

        // Store geometry normals, colors, and polygroup IDs
        if (geomID >= m_geoms.size()) {
            m_geoms.resize(geomID + 1);
        }

        m_geoms[geomID].indices = subtool.indices;
        m_geoms[geomID].colors = subtool.colors;
        m_geoms[geomID].polygroups = subtool.polygroupPerFace;
        Vec3 smMin = subtool.GetBoundingBoxMin();
        Vec3 smMax = subtool.GetBoundingBoxMax();
        m_geoms[geomID].bMin = smMin;
        m_geoms[geomID].bMax = smMax;

        m_sceneBMin.x = (std::min)(m_sceneBMin.x, smMin.x);
        m_sceneBMin.y = (std::min)(m_sceneBMin.y, smMin.y);
        m_sceneBMin.z = (std::min)(m_sceneBMin.z, smMin.z);
        m_sceneBMax.x = (std::max)(m_sceneBMax.x, smMax.x);
        m_sceneBMax.y = (std::max)(m_sceneBMax.y, smMax.y);
        m_sceneBMax.z = (std::max)(m_sceneBMax.z, smMax.z);

        if (subtool.normals.size() == subtool.positions.size()) {
            m_geoms[geomID].normals = subtool.normals;
        } else {
            // Compute area-weighted smooth vertex normals
            std::vector<Vec3> smooth(numVerts, Vec3(0, 0, 0));
            for (size_t i = 0; i < subtool.indices.size(); i += 3) {
                uint32_t i0 = subtool.indices[i + 0];
                uint32_t i1 = subtool.indices[i + 1];
                uint32_t i2 = subtool.indices[i + 2];

                Vec3 e1 = subtool.positions[i1] - subtool.positions[i0];
                Vec3 e2 = subtool.positions[i2] - subtool.positions[i0];
                Vec3 fn = e1.Cross(e2);

                smooth[i0] = smooth[i0] + fn;
                smooth[i1] = smooth[i1] + fn;
                smooth[i2] = smooth[i2] + fn;
            }
            for (size_t i = 0; i < numVerts; ++i) {
                float len = smooth[i].Length();
                if (len > 1e-20f) {
                    smooth[i] = smooth[i] * (1.0f / len);
                } else {
                    smooth[i] = Vec3(0, 1, 0);
                }
            }
            m_geoms[geomID].normals = std::move(smooth);
        }

        attachedCount++;
    }

    if (attachedCount == 0) {
        outError = "All provided SubTools are hidden or contain no geometry";
        return false;
    }

    rtcCommitScene(scene);
    m_bvhReady = true;
    return true;
}

struct SeedPoint {
    int16_t x;
    int16_t y;
};

void BakerEngine::ApplyJumpFloodingDilation(
    BakeResult& res,
    int paddingPixels
) {
    int width = res.width;
    int height = res.height;
    const auto& mask = res.hitMask;

    if (paddingPixels <= 0 || width <= 0 || height <= 0 || mask.empty()) return;

    size_t totalPixels = static_cast<size_t>(width) * height;
    std::vector<SeedPoint> ping(totalPixels, SeedPoint{ -1, -1 });
    std::vector<SeedPoint> pong(totalPixels, SeedPoint{ -1, -1 });

    // 1. Seed initialization: pixels inside the UV mask point to themselves
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            if (mask[idx]) {
                ping[idx] = SeedPoint{ static_cast<int16_t>(x), static_cast<int16_t>(y) };
            }
        }
    }

    // 2. Jump Flooding Passes
    int initialStep = std::max(width, height) / 2;
    std::vector<int> stepSizes;
    for (int step = initialStep; step >= 1; step /= 2) {
        stepSizes.push_back(step);
    }
    stepSizes.push_back(1);

    bool usePing = true;
    for (int step : stepSizes) {
        const auto& srcGrid = usePing ? ping : pong;
        auto& dstGrid = usePing ? pong : ping;

        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                int currIdx = y * width + x;
                SeedPoint bestSeed = srcGrid[currIdx];
                int bestDistSq = 0x7FFFFFFF;

                if (bestSeed.x >= 0) {
                    int dx = x - bestSeed.x;
                    int dy = y - bestSeed.y;
                    bestDistSq = dx * dx + dy * dy;
                }

                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = y + dy * step;
                    if (ny < 0 || ny >= height) continue;

                    for (int dx = -1; dx <= 1; ++dx) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = x + dx * step;
                        if (nx < 0 || nx >= width) continue;

                        int nIdx = ny * width + nx;
                        SeedPoint nSeed = srcGrid[nIdx];
                        if (nSeed.x >= 0) {
                            int dX = x - nSeed.x;
                            int dY = y - nSeed.y;
                            int candDistSq = dX * dX + dY * dY;
                            if (candDistSq < bestDistSq) {
                                bestDistSq = candDistSq;
                                bestSeed = nSeed;
                            }
                        }
                    }
                }

                dstGrid[currIdx] = bestSeed;
            }
        }

        usePing = !usePing;
    }

    const auto& finalGrid = usePing ? ping : pong;
    int maxDistSq = paddingPixels * paddingPixels;

    // 3. Final Dilation & Renormalization Pass for all map channels
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            if (mask[idx]) continue; // Already valid

            SeedPoint seed = finalGrid[idx];
            if (seed.x >= 0) {
                int dx = x - seed.x;
                int dy = y - seed.y;
                int distSq = dx * dx + dy * dy;

                if (distSq <= maxDistSq) {
                    int srcIdx = seed.y * width + seed.x;

                    if (!res.displacementMap.empty()) res.displacementMap[idx] = res.displacementMap[srcIdx];
                    if (!res.aoMap.empty()) res.aoMap[idx] = res.aoMap[srcIdx];
                    if (!res.cavityMap.empty()) res.cavityMap[idx] = res.cavityMap[srcIdx];
                    if (!res.curvatureMap.empty()) res.curvatureMap[idx] = res.curvatureMap[srcIdx];
                    if (!res.thicknessMap.empty()) res.thicknessMap[idx] = res.thicknessMap[srcIdx];

                    if (!res.positionMapR.empty()) {
                        res.positionMapR[idx] = res.positionMapR[srcIdx];
                        res.positionMapG[idx] = res.positionMapG[srcIdx];
                        res.positionMapB[idx] = res.positionMapB[srcIdx];
                    }
                    if (!res.materialIdMapR.empty()) {
                        res.materialIdMapR[idx] = res.materialIdMapR[srcIdx];
                        res.materialIdMapG[idx] = res.materialIdMapG[srcIdx];
                        res.materialIdMapB[idx] = res.materialIdMapB[srcIdx];
                    }
                    if (!res.baseColorMapR.empty()) {
                        res.baseColorMapR[idx] = res.baseColorMapR[srcIdx];
                        res.baseColorMapG[idx] = res.baseColorMapG[srcIdx];
                        res.baseColorMapB[idx] = res.baseColorMapB[srcIdx];
                    }

                    // Renormalize tangent normal
                    if (!res.normalMapR.empty() && !res.normalMapG.empty() && !res.normalMapB.empty()) {
                        float nx = res.normalMapR[srcIdx] * 2.0f - 1.0f;
                        float ny = res.normalMapG[srcIdx] * 2.0f - 1.0f;
                        float nz = res.normalMapB[srcIdx] * 2.0f - 1.0f;
                        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                        res.normalMapR[idx] = 0.5f * nx + 0.5f;
                        res.normalMapG[idx] = 0.5f * ny + 0.5f;
                        res.normalMapB[idx] = 0.5f * nz + 0.5f;
                    }

                    // Renormalize world normal
                    if (!res.worldNormalR.empty() && !res.worldNormalG.empty() && !res.worldNormalB.empty()) {
                        float nx = res.worldNormalR[srcIdx] * 2.0f - 1.0f;
                        float ny = res.worldNormalG[srcIdx] * 2.0f - 1.0f;
                        float nz = res.worldNormalB[srcIdx] * 2.0f - 1.0f;
                        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
                        res.worldNormalR[idx] = 0.5f * nx + 0.5f;
                        res.worldNormalG[idx] = 0.5f * ny + 0.5f;
                        res.worldNormalB[idx] = 0.5f * nz + 0.5f;
                    }
                }
            }
        }
    }
}

void BakerEngine::ApplyJumpFloodingDilation(
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
) {
    BakeResult dummy;
    dummy.width = width;
    dummy.height = height;
    dummy.displacementMap = displacement;
    dummy.normalMapR = normR;
    dummy.normalMapG = normG;
    dummy.normalMapB = normB;
    dummy.aoMap = ao;
    dummy.cavityMap = cavity;
    dummy.curvatureMap = curvature;
    dummy.hitMask = mask;
    ApplyJumpFloodingDilation(dummy, paddingPixels);
    displacement = std::move(dummy.displacementMap);
    normR = std::move(dummy.normalMapR);
    normG = std::move(dummy.normalMapG);
    normB = std::move(dummy.normalMapB);
    ao = std::move(dummy.aoMap);
    cavity = std::move(dummy.cavityMap);
    curvature = std::move(dummy.curvatureMap);
}

void BakerEngine::ApplySeamPadding(
    std::vector<float>& channel,
    const std::vector<uint8_t>& mask,
    int width,
    int height,
    int paddingPixels
) {
    std::vector<float> empty;
    ApplyJumpFloodingDilation(channel, empty, empty, empty, empty, empty, empty, mask, width, height, paddingPixels);
}

struct SubpixelOffset {
    float dx;
    float dy;
};

bool BakerEngine::ExecuteBake(
    const LowPolyMesh& lowPoly,
    const BakeSettings& settings,
    std::vector<BakeResult>& outResults,
    std::function<void(float progress, const std::string& status)> progressCallback,
    std::string& outError
) {
    if (!m_bvhReady || !m_embreeScene) {
        outError = "High-poly BVH is not initialized";
        return false;
    }

    if (lowPoly.triangles.empty() || lowPoly.detectedUDIMs.empty()) {
        outError = "Low-poly mesh has no triangles or UDIMs";
        return false;
    }

    outResults.clear();
    int W = settings.resolution;
    int H = settings.resolution;
    std::vector<int> activeTiles;
    if (!settings.targetUDIMs.empty()) {
        for (int t : lowPoly.detectedUDIMs) {
            if (std::find(settings.targetUDIMs.begin(), settings.targetUDIMs.end(), t) != settings.targetUDIMs.end()) {
                activeTiles.push_back(t);
            }
        }
    }
    if (activeTiles.empty()) {
        activeTiles = lowPoly.detectedUDIMs;
    }
    size_t totalTiles = activeTiles.size();
    bool hasExplicitCage = lowPoly.HasExplicitCage();

    // Configure subpixel supersampling (AA)
    std::vector<SubpixelOffset> subpixelOffsets;
    if (settings.antiAliasing == 4) {
        // 4x4 Grid = 16 rays/pixel (Ultra High Quality)
        subpixelOffsets.resize(16);
        for (int sy = 0; sy < 4; ++sy) {
            for (int sx = 0; sx < 4; ++sx) {
                subpixelOffsets[sy * 4 + sx] = {
                    (static_cast<float>(sx) + 0.5f) / 4.0f - 0.5f,
                    (static_cast<float>(sy) + 0.5f) / 4.0f - 0.5f
                };
            }
        }
    } else if (settings.antiAliasing == 2) {
        // 2x2 Grid = 4 rays/pixel (Production Standard)
        subpixelOffsets = {
            { -0.25f, -0.25f },
            {  0.25f, -0.25f },
            { -0.25f,  0.25f },
            {  0.25f,  0.25f }
        };
    } else {
        // 1x = 1 ray/pixel (Draft / Fast)
        subpixelOffsets = { { 0.0f, 0.0f } };
    }

    RTCScene scene = static_cast<RTCScene>(m_embreeScene);
    unsigned int numThreads = (std::max)(1u, (std::min)(64u, std::thread::hardware_concurrency()));

    for (size_t tileIdx = 0; tileIdx < totalTiles; ++tileIdx) {
        int tile = activeTiles[tileIdx];

        if (progressCallback) {
            float baseProg = static_cast<float>(tileIdx) / static_cast<float>(totalTiles);
            std::string modeDesc = " [AA " + std::to_string(subpixelOffsets.size()) + "x, " +
                (hasExplicitCage ? "Explicit Cage" : "Auto Cage") + "]";
            progressCallback(baseProg, "Baking UDIM Tile " + std::to_string(tile) + modeDesc + "...");
        }

        BakeResult res;
        res.udimTile = tile;
        res.width = W;
        res.height = H;
        res.displacementMap.resize(W * H, 0.0f);
        res.normalMapR.resize(W * H, 0.5f);
        res.normalMapG.resize(W * H, 0.5f);
        res.normalMapB.resize(W * H, 1.0f);
        res.worldNormalR.resize(W * H, 0.5f);
        res.worldNormalG.resize(W * H, 0.5f);
        res.worldNormalB.resize(W * H, 1.0f);
        res.aoMap.resize(W * H, 1.0f);
        res.cavityMap.resize(W * H, 1.0f);
        res.curvatureMap.resize(W * H, 0.5f);
        res.thicknessMap.resize(W * H, 0.0f);
        res.positionMapR.resize(W * H, 0.0f);
        res.positionMapG.resize(W * H, 0.0f);
        res.positionMapB.resize(W * H, 0.0f);
        res.materialIdMapR.resize(W * H, 0.5f);
        res.materialIdMapG.resize(W * H, 0.5f);
        res.materialIdMapB.resize(W * H, 0.5f);
        res.baseColorMapR.resize(W * H, 1.0f);
        res.baseColorMapG.resize(W * H, 1.0f);
        res.baseColorMapB.resize(W * H, 1.0f);
        res.diagnosticMapR.resize(W * H, 0.0f);
        res.diagnosticMapG.resize(W * H, 0.0f);
        res.diagnosticMapB.resize(W * H, 0.0f);
        res.hitMask.resize(W * H, 0);

        int tileUOffset = (tile - 1001) % 10;
        int tileVOffset = (tile - 1001) / 10;

        // Collect triangles belonging to this UDIM tile
        std::vector<size_t> tileTriangles;
        for (size_t t = 0; t < lowPoly.triangles.size(); ++t) {
            if (lowPoly.triangles[t].udimTile == tile) {
                tileTriangles.push_back(t);
            }
        }

        // Bin triangles into scanlines for multi-threaded rasterization
        std::vector<std::vector<uint32_t>> rowTriangles(H);
        for (size_t t = 0; t < tileTriangles.size(); ++t) {
            size_t triIdx = tileTriangles[t];
            const auto& tri = lowPoly.triangles[triIdx];
            const Vertex& v0 = lowPoly.vertices[tri.indices[0]];
            const Vertex& v1 = lowPoly.vertices[tri.indices[1]];
            const Vertex& v2 = lowPoly.vertices[tri.indices[2]];

            float v0_val = v0.uv.v - static_cast<float>(tileVOffset);
            float v1_val = v1.uv.v - static_cast<float>(tileVOffset);
            float v2_val = v2.uv.v - static_cast<float>(tileVOffset);

            float minV = (std::min)({ v0_val, v1_val, v2_val });
            float maxV = (std::max)({ v0_val, v1_val, v2_val });

            int minY = (std::max)(0, static_cast<int>(std::floor(minV * H)));
            int maxY = (std::min)(H - 1, static_cast<int>(std::ceil(maxV * H)));

            for (int y = minY; y <= maxY; ++y) {
                rowTriangles[y].push_back(static_cast<uint32_t>(triIdx));
            }
        }

        // Per-subpixel sample hit for closest-distance depth testing (handles overlapping UVs / duplicate pieces)
        struct SubpixelHit {
            bool hasHit = false;
            float hitDistance = 1e30f;
            Vec3 norm = Vec3(0, 0, 0);
            Vec3 worldNorm = Vec3(0, 0, 0);
            Vec3 pos = Vec3(0, 0, 0);
            Vec3 baseColor = Vec3(1, 1, 1);
            Vec3 matId = Vec3(0.5f, 0.5f, 0.5f);
            float disp = 0.0f;
            float aoVal = 1.0f;
            float thickVal = 0.0f;
        };

        struct PixelAccum {
            SubpixelHit samples[16];
            bool covered = false;
        };

        std::atomic<int> nextRow(0);
        std::atomic<int> completedRows(0);
        std::vector<std::thread> workers;

        Vec3 lpTrans = settings.lowPolyTranslation;
        Vec3 lpRot = settings.lowPolyRotation;
        float baseScale = settings.lowPolyScale > 1e-6f ? settings.lowPolyScale : 1.0f;
        Vec3 lpScaleVec = Vec3(settings.lowPolyScaleVec.x * baseScale,
                               settings.lowPolyScaleVec.y * baseScale,
                               settings.lowPolyScaleVec.z * baseScale);

        for (unsigned int tid = 0; tid < numThreads; ++tid) {
            workers.emplace_back([&, tileUOffset, tileVOffset, lpTrans, lpRot, lpScaleVec]() {
                RTCIntersectContext context;
                rtcInitIntersectContext(&context);
                if (settings.discardBackfaceHits) {
                    context.filter = BackfaceFilter;
                }

                std::vector<PixelAccum> rowAccum(W);

                while (true) {
                    int y = nextRow.fetch_add(1);
                    if (y >= H) break;

                    const auto& trisInRow = rowTriangles[y];
                    if (trisInRow.empty()) {
                        completedRows.fetch_add(1);
                        continue;
                    }

                    // Reset row accumulators
                    for (int x = 0; x < W; ++x) {
                        rowAccum[x] = PixelAccum{};
                    }

                    for (uint32_t triIdx : trisInRow) {
                        const auto& tri = lowPoly.triangles[triIdx];
                        const Vertex& v0 = lowPoly.vertices[tri.indices[0]];
                        const Vertex& v1 = lowPoly.vertices[tri.indices[1]];
                        const Vertex& v2 = lowPoly.vertices[tri.indices[2]];

                        float u0 = v0.uv.u - static_cast<float>(tileUOffset);
                        float v0_val = v0.uv.v - static_cast<float>(tileVOffset);

                        float u1 = v1.uv.u - static_cast<float>(tileUOffset);
                        float v1_val = v1.uv.v - static_cast<float>(tileVOffset);

                        float u2 = v2.uv.u - static_cast<float>(tileUOffset);
                        float v2_val = v2.uv.v - static_cast<float>(tileVOffset);

                        float minU = (std::min)({ u0, u1, u2 });
                        float maxU = (std::max)({ u0, u1, u2 });

                        int minX = (std::max)(0, static_cast<int>(std::floor(minU * W)));
                        int maxX = (std::min)(W - 1, static_cast<int>(std::ceil(maxU * W)));

                        float denom = (v1_val - v2_val) * (u0 - u2) + (u2 - u1) * (v0_val - v2_val);
                        if (std::abs(denom) < 1e-9f) continue;
                        float invDenom = 1.0f / denom;

                        for (int x = minX; x <= maxX; ++x) {
                            for (size_t sIdx = 0; sIdx < subpixelOffsets.size(); ++sIdx) {
                                const auto& offset = subpixelOffsets[sIdx];
                                float uPix = (static_cast<float>(x) + 0.5f + offset.dx) / static_cast<float>(W);
                                float vPix = (static_cast<float>(y) + 0.5f + offset.dy) / static_cast<float>(H);

                                float w0 = ((v1_val - v2_val) * (uPix - u2) + (u2 - u1) * (vPix - v2_val)) * invDenom;
                                float w1 = ((v2_val - v0_val) * (uPix - u2) + (u0 - u2) * (vPix - v2_val)) * invDenom;
                                float w2 = 1.0f - w0 - w1;

                                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;

                                w0 = (std::max)(0.0f, w0);
                                w1 = (std::max)(0.0f, w1);
                                w2 = (std::max)(0.0f, w2);
                                float sumW = w0 + w1 + w2;
                                if (sumW > 1e-7f) {
                                    w0 /= sumW; w1 /= sumW; w2 /= sumW;
                                }

                                rowAccum[x].covered = true;

                                Vec3 rawPos = v0.position * w0 + v1.position * w1 + v2.position * w2;
                                Vec3 rawNorm = (v0.normal * w0 + v1.normal * w1 + v2.normal * w2).Normalized();
                                Vec3 rawTan = (Vec3(v0.tangent.x, v0.tangent.y, v0.tangent.z) * w0 +
                                               Vec3(v1.tangent.x, v1.tangent.y, v1.tangent.z) * w1 +
                                               Vec3(v2.tangent.x, v2.tangent.y, v2.tangent.z) * w2).Normalized();

                                Vec3 pos = TransformPoint(rawPos, lpTrans, lpRot, lpScaleVec);
                                Vec3 norm = TransformDirection(rawNorm, lpRot, lpScaleVec);
                                Vec3 tan3 = TransformDirection(rawTan, lpRot, lpScaleVec);

                                float tanSign = v0.tangent.w;

                                // Gram-Schmidt orthonormalize MikkTSpace frame
                                Vec3 n = norm.Normalized();
                                Vec3 t = (tan3 - n * n.Dot(tan3)).Normalized();
                                Vec3 b = (n.Cross(t) * tanSign).Normalized();

                                // Marmoset-style skew correction: blend smooth normal with flat face normal
                                Vec3 rawFaceNorm = (v1.position - v0.position).Cross(v2.position - v0.position).Normalized();
                                Vec3 geomFaceNormal = TransformDirection(rawFaceNorm, lpRot, lpScaleVec);
                                float skew = std::clamp(settings.skewFactor, 0.0f, 1.0f);
                                Vec3 rayNormal = (n * (1.0f - skew) + geomFaceNormal * skew).Normalized();

                                // Watertight cage normal across UV seams and hard edges
                                Vec3 cageNormal = rayNormal;
                                if (lowPoly.HasWatertightCageNormals()) {
                                    Vec3 rawCageNorm = (lowPoly.cageNormals[tri.indices[0]] * w0 +
                                                        lowPoly.cageNormals[tri.indices[1]] * w1 +
                                                        lowPoly.cageNormals[tri.indices[2]] * w2).Normalized();
                                    Vec3 transCageNorm = TransformDirection(rawCageNorm, lpRot, lpScaleVec).Normalized();
                                    cageNormal = (transCageNorm * (1.0f - skew) + geomFaceNormal * skew).Normalized();
                                }

                                Vec3 rayOrg;
                                Vec3 rayDir;
                                float maxDist = 0.0f;

                                if (hasExplicitCage) {
                                    const Vec3& c0 = lowPoly.cagePositions[tri.indices[0]];
                                    const Vec3& c1 = lowPoly.cagePositions[tri.indices[1]];
                                    const Vec3& c2 = lowPoly.cagePositions[tri.indices[2]];
                                    Vec3 rawCage = c0 * w0 + c1 * w1 + c2 * w2;
                                    Vec3 cagePos = TransformPoint(rawCage, lpTrans, lpRot, lpScaleVec);

                                    Vec3 cageToLow = pos - cagePos;
                                    float cageDist = cageToLow.Length();
                                    rayOrg = cagePos;

                                    if (cageDist > 1e-6f) {
                                        rayDir = cageToLow * (1.0f / cageDist);
                                    } else {
                                        rayDir = cageNormal * -1.0f;
                                    }
                                    maxDist = cageDist + settings.maxRayDistance;
                                } else {
                                    rayOrg = pos + cageNormal * settings.cageDistance;
                                    rayDir = cageNormal * -1.0f;
                                    maxDist = settings.cageDistance + settings.maxRayDistance;
                                }

                                unsigned int rayMask = settings.isolateMeshParts ? tri.groupMask : settings.groupMask;

                                // --- 1. Primary Inward Ray (from cage envelope inward) ---
                                RTCRayHit rayhitIn;
                                rayhitIn.ray.org_x = rayOrg.x;
                                rayhitIn.ray.org_y = rayOrg.y;
                                rayhitIn.ray.org_z = rayOrg.z;
                                rayhitIn.ray.dir_x = rayDir.x;
                                rayhitIn.ray.dir_y = rayDir.y;
                                rayhitIn.ray.dir_z = rayDir.z;
                                rayhitIn.ray.tnear = 0.0f;
                                rayhitIn.ray.tfar = maxDist;
                                rayhitIn.ray.mask = rayMask;
                                rayhitIn.ray.flags = 0;
                                rayhitIn.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                                rayhitIn.hit.primID = RTC_INVALID_GEOMETRY_ID;

                                rtcIntersect1(scene, &context, &rayhitIn);

                                bool hasHitIn = false;
                                Vec3 hitPosIn, hitNormIn;
                                float dispIn = 0.0f;
                                unsigned int gidIn = 0, pidIn = 0;
                                float hitUIn = 0.0f, hitVIn = 0.0f, hitWIn = 0.0f;

                                float minCos = std::cos(std::clamp(settings.angleThreshold, 10.0f, 90.0f) * 3.14159265f / 180.0f);

                                if (rayhitIn.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                    gidIn = rayhitIn.hit.geomID;
                                    pidIn = rayhitIn.hit.primID;
                                    hitUIn = rayhitIn.hit.u;
                                    hitVIn = rayhitIn.hit.v;
                                    hitWIn = 1.0f - hitUIn - hitVIn;

                                    if (gidIn < m_geoms.size() && !m_geoms[gidIn].normals.empty() && !m_geoms[gidIn].indices.empty()) {
                                        const auto& g = m_geoms[gidIn];
                                        uint32_t i0 = g.indices[pidIn * 3 + 0];
                                        uint32_t i1 = g.indices[pidIn * 3 + 1];
                                        uint32_t i2 = g.indices[pidIn * 3 + 2];
                                        hitNormIn = (g.normals[i0] * hitWIn + g.normals[i1] * hitUIn + g.normals[i2] * hitVIn).Normalized();
                                        if (hitNormIn.Length() < 1e-4f) {
                                            hitNormIn = Vec3(rayhitIn.hit.Ng_x, rayhitIn.hit.Ng_y, rayhitIn.hit.Ng_z).Normalized();
                                        }
                                    } else {
                                        hitNormIn = Vec3(rayhitIn.hit.Ng_x, rayhitIn.hit.Ng_y, rayhitIn.hit.Ng_z).Normalized();
                                    }

                                    Vec3 geomNg = Vec3(rayhitIn.hit.Ng_x, rayhitIn.hit.Ng_y, rayhitIn.hit.Ng_z);
                                    if (hitNormIn.Dot(geomNg) < 0.0f) {
                                        hitNormIn = hitNormIn * -1.0f;
                                    }

                                    bool valid = true;
                                    if (settings.discardBackfaceHits) {
                                        bool isBackface = (hitNormIn.Dot(rayDir) >= -1e-4f);
                                        bool exceedsAngle = (settings.angleThreshold < 89.9f) && (hitNormIn.Dot(n) < minCos);
                                        if (isBackface || exceedsAngle) {
                                            valid = false;
                                        }
                                    }
                                    if (valid) {
                                        hitPosIn = rayOrg + rayDir * rayhitIn.ray.tfar;
                                        dispIn = (hitPosIn - pos).Dot(n);
                                        hasHitIn = true;
                                    }
                                }

                                // --- 2. Fallback Outward Ray (catches tall protruding sculpt peaks beyond cage) ---
                                bool hasHitOut = false;
                                Vec3 hitPosOut, hitNormOut;
                                float dispOut = 0.0f;
                                unsigned int gidOut = 0, pidOut = 0;
                                float hitUOut = 0.0f, hitVOut = 0.0f, hitWOut = 0.0f;

                                if (settings.useBidirectionalRays && (!hasHitIn || dispIn < 0.0f)) {
                                    Vec3 rayOrgOut = pos + cageNormal * 0.0001f;
                                    Vec3 rayDirOut = cageNormal;
                                    float maxDistOut = (settings.maxOutwardDistance > 1e-6f ? settings.maxOutwardDistance : settings.maxRayDistance);

                                    RTCRayHit rayhitOut;
                                    rayhitOut.ray.org_x = rayOrgOut.x;
                                    rayhitOut.ray.org_y = rayOrgOut.y;
                                    rayhitOut.ray.org_z = rayOrgOut.z;
                                    rayhitOut.ray.dir_x = rayDirOut.x;
                                    rayhitOut.ray.dir_y = rayDirOut.y;
                                    rayhitOut.ray.dir_z = rayDirOut.z;
                                    rayhitOut.ray.tnear = 0.0f;
                                    rayhitOut.ray.tfar = maxDistOut;
                                    rayhitOut.ray.mask = rayMask;
                                    rayhitOut.ray.flags = 0;
                                    rayhitOut.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                                    rayhitOut.hit.primID = RTC_INVALID_GEOMETRY_ID;

                                    rtcIntersect1(scene, &context, &rayhitOut);

                                    if (rayhitOut.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                                        gidOut = rayhitOut.hit.geomID;
                                        pidOut = rayhitOut.hit.primID;
                                        hitUOut = rayhitOut.hit.u;
                                        hitVOut = rayhitOut.hit.v;
                                        hitWOut = 1.0f - hitUOut - hitVOut;

                                        if (gidOut < m_geoms.size() && !m_geoms[gidOut].normals.empty() && !m_geoms[gidOut].indices.empty()) {
                                            const auto& g = m_geoms[gidOut];
                                            uint32_t i0 = g.indices[pidOut * 3 + 0];
                                            uint32_t i1 = g.indices[pidOut * 3 + 1];
                                            uint32_t i2 = g.indices[pidOut * 3 + 2];
                                            hitNormOut = (g.normals[i0] * hitWOut + g.normals[i1] * hitUOut + g.normals[i2] * hitVOut).Normalized();
                                            if (hitNormOut.Length() < 1e-4f) {
                                                hitNormOut = Vec3(rayhitOut.hit.Ng_x, rayhitOut.hit.Ng_y, rayhitOut.hit.Ng_z).Normalized();
                                            }
                                        } else {
                                            hitNormOut = Vec3(rayhitOut.hit.Ng_x, rayhitOut.hit.Ng_y, rayhitOut.hit.Ng_z).Normalized();
                                        }

                                        Vec3 geomNg = Vec3(rayhitOut.hit.Ng_x, rayhitOut.hit.Ng_y, rayhitOut.hit.Ng_z);
                                        if (hitNormOut.Dot(geomNg) < 0.0f) {
                                            hitNormOut = hitNormOut * -1.0f;
                                        }

                                        bool valid = true;
                                        if (settings.discardBackfaceHits) {
                                            bool isBackface = (hitNormOut.Dot(rayDirOut) >= -1e-4f);
                                            bool exceedsAngle = (settings.angleThreshold < 89.9f) && (hitNormOut.Dot(n) < minCos);
                                            if (isBackface || exceedsAngle) {
                                                valid = false;
                                            }
                                        }
                                        if (valid) {
                                            hitPosOut = rayOrgOut + rayDirOut * rayhitOut.ray.tfar;
                                            dispOut = (hitPosOut - pos).Dot(n);
                                            hasHitOut = true;
                                        }
                                    }
                                }

                                // --- 3. Marmoset Closest-Hit Arbitration ---
                                bool useInward = false;
                                if (hasHitIn && hasHitOut) {
                                    useInward = (std::abs(dispIn) <= std::abs(dispOut));
                                } else if (hasHitIn) {
                                    useInward = true;
                                } else if (hasHitOut) {
                                    useInward = false;
                                } else {
                                    continue;
                                }

                                Vec3 hitPos = useInward ? hitPosIn : hitPosOut;
                                Vec3 hitNorm = useInward ? hitNormIn : hitNormOut;
                                float disp = useInward ? dispIn : dispOut;
                                unsigned int gid = useInward ? gidIn : gidOut;
                                unsigned int pid = useInward ? pidIn : pidOut;
                                float hitU = useInward ? hitUIn : hitUOut;
                                float hitV = useInward ? hitVIn : hitVOut;
                                float hitW = useInward ? hitWIn : hitWOut;

                                uint32_t i0 = 0, i1 = 0, i2 = 0;
                                if (gid < m_geoms.size() && !m_geoms[gid].indices.empty()) {
                                    const auto& g = m_geoms[gid];
                                    i0 = g.indices[pid * 3 + 0];
                                    i1 = g.indices[pid * 3 + 1];
                                    i2 = g.indices[pid * 3 + 2];
                                }

                                float nTx = hitNorm.Dot(t);
                                float nTy = hitNorm.Dot(b);
                                if (settings.flipYNormal) nTy = -nTy;
                                float nTz = hitNorm.Dot(n);

                                Vec3 tsNorm = Vec3(nTx, nTy, nTz).Normalized();

                                // Ambient Occlusion
                                float aoVal = 1.0f;
                                if (settings.bakeAmbientOcclusion && settings.aoSamples > 0) {
                                    int occludedCount = 0;
                                    Vec3 aoUp = (std::abs(hitNorm.z) < 0.99f) ? Vec3(0, 0, 1) : Vec3(1, 0, 0);
                                    Vec3 aoTangent = hitNorm.Cross(aoUp).Normalized();
                                    Vec3 aoBitangent = hitNorm.Cross(aoTangent);

                                    for (int a = 0; a < settings.aoSamples; ++a) {
                                        float u1 = (static_cast<float>(a) + 0.5f) / static_cast<float>(settings.aoSamples);
                                        float u2 = std::fmod(u1 * 7.618f + 0.123f, 1.0f);
                                        float phi = 6.2831853f * u1;
                                        float r = std::sqrt(u2);
                                        float lx = r * std::cos(phi);
                                        float ly = r * std::sin(phi);
                                        float lz = std::sqrt((std::max)(0.0f, 1.0f - u2));

                                        Vec3 aoDir = (aoTangent * lx + aoBitangent * ly + hitNorm * lz).Normalized();

                                        RTCRay aoRay;
                                        aoRay.org_x = hitPos.x + hitNorm.x * 0.001f;
                                        aoRay.org_y = hitPos.y + hitNorm.y * 0.001f;
                                        aoRay.org_z = hitPos.z + hitNorm.z * 0.001f;
                                        aoRay.dir_x = aoDir.x;
                                        aoRay.dir_y = aoDir.y;
                                        aoRay.dir_z = aoDir.z;
                                        aoRay.tnear = 0.001f;
                                        aoRay.tfar = settings.aoMaxDistance;
                                        aoRay.mask = rayMask;
                                        aoRay.flags = 0;

                                        rtcOccluded1(scene, &context, &aoRay);
                                        if (aoRay.tfar < 0.0f) {
                                            occludedCount++;
                                        }
                                    }
                                    aoVal = 1.0f - static_cast<float>(occludedCount) / static_cast<float>(settings.aoSamples);
                                }

                                // Thickness Map (translucency / SSS)
                                float thickVal = 0.0f;
                                if (settings.bakeThickness && settings.thicknessMaxDistance > 1e-4f) {
                                    RTCRayHit thickRay;
                                    thickRay.ray.org_x = hitPos.x - hitNorm.x * 0.001f;
                                    thickRay.ray.org_y = hitPos.y - hitNorm.y * 0.001f;
                                    thickRay.ray.org_z = hitPos.z - hitNorm.z * 0.001f;
                                    thickRay.ray.dir_x = -hitNorm.x;
                                    thickRay.ray.dir_y = -hitNorm.y;
                                    thickRay.ray.dir_z = -hitNorm.z;
                                    thickRay.ray.tnear = 0.001f;
                                    thickRay.ray.tfar = settings.thicknessMaxDistance;
                                    thickRay.ray.mask = rayMask;
                                    thickRay.ray.flags = 0;
                                    thickRay.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                                    thickRay.hit.primID = RTC_INVALID_GEOMETRY_ID;

                                    rtcIntersect1(scene, &context, &thickRay);
                                    float d = (thickRay.hit.geomID != RTC_INVALID_GEOMETRY_ID) ? thickRay.ray.tfar : settings.thicknessMaxDistance;
                                    thickVal = 1.0f - std::clamp(d / settings.thicknessMaxDistance, 0.0f, 1.0f);
                                }

                                    // Base Color (Polypaint)
                                    Vec3 hitColor(1.0f, 1.0f, 1.0f);
                                    if (gid < m_geoms.size() && !m_geoms[gid].colors.empty()) {
                                        const auto& cols = m_geoms[gid].colors;
                                        if (i0 < cols.size() && i1 < cols.size() && i2 < cols.size()) {
                                            hitColor = cols[i0] * hitW + cols[i1] * hitU + cols[i2] * hitV;
                                        }
                                    }

                                    // Material ID (Polygroups)
                                    uint32_t pgId = 1;
                                    if (gid < m_geoms.size() && !m_geoms[gid].polygroups.empty() && pid < m_geoms[gid].polygroups.size()) {
                                        pgId = m_geoms[gid].polygroups[pid];
                                    } else {
                                        pgId = gid + 1;
                                    }
                                    float hue = std::fmod(pgId * 0.61803398875f, 1.0f);
                                    Vec3 idCol = HSVtoRGB(hue, 0.85f, 0.95f);

                                    float hitDist = std::abs(disp);
                                    if (!rowAccum[x].samples[sIdx].hasHit || hitDist < rowAccum[x].samples[sIdx].hitDistance) {
                                        auto& s = rowAccum[x].samples[sIdx];
                                        s.hasHit = true;
                                        s.hitDistance = hitDist;
                                        s.norm = tsNorm;
                                        s.worldNorm = hitNorm;
                                        s.pos = hitPos;
                                        s.baseColor = hitColor;
                                        s.matId = idCol;
                                        s.disp = disp;
                                        s.aoVal = aoVal;
                                        s.thickVal = thickVal;
                                    }
                                }
                            }
                        }

                        // Flush row pixels into result buffers
                    for (int x = 0; x < W; ++x) {
                        int pixelIdx = y * W + x;
                        const auto& p = rowAccum[x];

                        Vec3 accumNorm(0, 0, 0);
                        Vec3 accumWorldNorm(0, 0, 0);
                        Vec3 accumPos(0, 0, 0);
                        Vec3 accumBaseColor(0, 0, 0);
                        Vec3 accumMatId(0, 0, 0);
                        float accumDisp = 0.0f;
                        float accumAO = 0.0f;
                        float accumThick = 0.0f;
                        int hitCount = 0;

                        for (size_t sIdx = 0; sIdx < subpixelOffsets.size(); ++sIdx) {
                            if (p.samples[sIdx].hasHit) {
                                hitCount++;
                                accumNorm = accumNorm + p.samples[sIdx].norm;
                                accumWorldNorm = accumWorldNorm + p.samples[sIdx].worldNorm;
                                accumPos = accumPos + p.samples[sIdx].pos;
                                accumBaseColor = accumBaseColor + p.samples[sIdx].baseColor;
                                accumMatId = accumMatId + p.samples[sIdx].matId;
                                accumDisp += p.samples[sIdx].disp;
                                accumAO += p.samples[sIdx].aoVal;
                                accumThick += p.samples[sIdx].thickVal;
                            }
                        }

                        if (hitCount > 0) {
                            float invHit = 1.0f / static_cast<float>(hitCount);
                            Vec3 avgNorm = accumNorm.Normalized();
                            Vec3 avgWorldNorm = accumWorldNorm.Normalized();
                            Vec3 avgPos = accumPos * invHit;
                            Vec3 avgBaseCol = accumBaseColor * invHit;
                            Vec3 avgMatId = accumMatId * invHit;
                            float avgDisp = accumDisp * invHit;
                            float avgAO = accumAO * invHit;
                            float avgThick = accumThick * invHit;

                            res.displacementMap[pixelIdx] = avgDisp;
                            res.normalMapR[pixelIdx] = 0.5f * avgNorm.x + 0.5f;
                            res.normalMapG[pixelIdx] = 0.5f * avgNorm.y + 0.5f;
                            res.normalMapB[pixelIdx] = 0.5f * avgNorm.z + 0.5f;

                            res.worldNormalR[pixelIdx] = 0.5f * avgWorldNorm.x + 0.5f;
                            res.worldNormalG[pixelIdx] = 0.5f * avgWorldNorm.y + 0.5f;
                            res.worldNormalB[pixelIdx] = 0.5f * avgWorldNorm.z + 0.5f;

                            res.aoMap[pixelIdx] = avgAO;
                            res.thicknessMap[pixelIdx] = avgThick;

                            Vec3 span = m_sceneBMax - m_sceneBMin;
                            res.positionMapR[pixelIdx] = std::clamp((avgPos.x - m_sceneBMin.x) / (span.x > 1e-4f ? span.x : 1.0f), 0.0f, 1.0f);
                            res.positionMapG[pixelIdx] = std::clamp((avgPos.y - m_sceneBMin.y) / (span.y > 1e-4f ? span.y : 1.0f), 0.0f, 1.0f);
                            res.positionMapB[pixelIdx] = std::clamp((avgPos.z - m_sceneBMin.z) / (span.z > 1e-4f ? span.z : 1.0f), 0.0f, 1.0f);

                            res.baseColorMapR[pixelIdx] = std::clamp(avgBaseCol.x, 0.0f, 1.0f);
                            res.baseColorMapG[pixelIdx] = std::clamp(avgBaseCol.y, 0.0f, 1.0f);
                            res.baseColorMapB[pixelIdx] = std::clamp(avgBaseCol.z, 0.0f, 1.0f);

                            res.materialIdMapR[pixelIdx] = std::clamp(avgMatId.x, 0.0f, 1.0f);
                            res.materialIdMapG[pixelIdx] = std::clamp(avgMatId.y, 0.0f, 1.0f);
                            res.materialIdMapB[pixelIdx] = std::clamp(avgMatId.z, 0.0f, 1.0f);

                            res.diagnosticMapR[pixelIdx] = 0.0f;
                            res.diagnosticMapG[pixelIdx] = 1.0f; // Clean Green Hit
                            res.diagnosticMapB[pixelIdx] = 0.0f;
                            res.hitMask[pixelIdx] = 1;
                        } else if (p.covered) {
                            res.diagnosticMapR[pixelIdx] = 1.0f; // Missed
                            res.diagnosticMapG[pixelIdx] = 0.0f;
                            res.diagnosticMapB[pixelIdx] = 0.0f;
                        }
                    }

                    completedRows.fetch_add(1);
                }
            });
        }

        for (auto& w : workers) {
            w.join();
        }

        // Compute Cavity and Curvature maps from multi-scale tangent normal divergence & AO modulation
        if (settings.bakeCavity || settings.bakeCurvature) {
            float strength = std::clamp(settings.cavityStrength, 0.1f, 10.0f);

            std::atomic<int> nextCavRow(0);
            std::vector<std::thread> cavWorkers;
            for (unsigned int th = 0; th < numThreads; ++th) {
                cavWorkers.emplace_back([&]() {
                    while (true) {
                        int y = nextCavRow.fetch_add(1);
                        if (y >= H) break;

                        for (int x = 0; x < W; ++x) {
                            int pidx = y * W + x;
                            if (!res.hitMask[pidx]) continue;

                            auto sampleTSNorm = [&](int sx, int sy) -> std::pair<float, float> {
                                sx = std::clamp(sx, 0, W - 1);
                                sy = std::clamp(sy, 0, H - 1);
                                int sIdx = sy * W + sx;
                                if (!res.hitMask[sIdx]) {
                                    sIdx = pidx;
                                }
                                float nx = res.normalMapR[sIdx] * 2.0f - 1.0f;
                                float ny = res.normalMapG[sIdx] * 2.0f - 1.0f;
                                return { nx, ny };
                            };

                            // Scale 1: 1px radius - fine pores, scratches, sharp creases
                            auto nR1 = sampleTSNorm(x + 1, y);
                            auto nL1 = sampleTSNorm(x - 1, y);
                            auto nU1 = sampleTSNorm(x, y + 1);
                            auto nD1 = sampleTSNorm(x, y - 1);
                            float div1 = ((nR1.first - nL1.first) + (nU1.second - nD1.second)) * 0.5f;

                            // Scale 2: 3px radius - structural cracks, bark fissures, bevel valleys
                            auto nR3 = sampleTSNorm(x + 3, y);
                            auto nL3 = sampleTSNorm(x - 3, y);
                            auto nU3 = sampleTSNorm(x, y + 3);
                            auto nD3 = sampleTSNorm(x, y - 3);
                            float div2 = ((nR3.first - nL3.first) + (nU3.second - nD3.second)) / 6.0f;

                            // Scale 3: 5px radius - broader valleys and ridges
                            auto nR5 = sampleTSNorm(x + 5, y);
                            auto nL5 = sampleTSNorm(x - 5, y);
                            auto nU5 = sampleTSNorm(x, y + 5);
                            auto nD5 = sampleTSNorm(x, y - 5);
                            float div3 = ((nR5.first - nL5.first) + (nU5.second - nD5.second)) / 10.0f;

                            float totalDiv = div1 * 0.50f + div2 * 0.35f + div3 * 0.15f;

                            // Cavity calculation: flat/peaks are 1.0, crevices/valleys (negative divergence) are dark
                            if (settings.bakeCavity) {
                                float concavity = (std::max)(0.0f, -totalDiv);
                                float cav = std::clamp(1.0f - concavity * 3.5f * strength, 0.0f, 1.0f);
                                if (settings.bakeAmbientOcclusion) {
                                    float aoVal = res.aoMap[pidx];
                                    cav *= (0.35f + 0.65f * std::sqrt(aoVal));
                                }
                                res.cavityMap[pidx] = cav;
                            }

                            // Curvature calculation: 0.5 neutral, <0.5 concave (valleys), >0.5 convex (ridges)
                            if (settings.bakeCurvature) {
                                float curv = std::clamp(0.5f + totalDiv * 1.5f * strength, 0.0f, 1.0f);
                                res.curvatureMap[pidx] = curv;
                            }
                        }
                    }
                });
            }
            for (auto& w : cavWorkers) {
                w.join();
            }
        }

        // Apply Jump Flooding Algorithm nearest-texel dilation across tile boundaries
        if (settings.seamPadding > 0) {
            ApplyJumpFloodingDilation(res, settings.seamPadding);
        }

        outResults.push_back(std::move(res));
    }

    if (progressCallback) {
        progressCallback(1.0f, "Baking complete!");
    }

    return true;
}

} // namespace Baker
