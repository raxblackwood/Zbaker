#include "BakerEngine.h"
#include <embree3/rtcore.h>

#include <cmath>
#include <algorithm>
#include <iostream>
#include <thread>
#include <atomic>

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
    m_bvhReady = false;
}

bool BakerEngine::BuildHighPolyBVH(const HighPolyMesh& highPoly, std::string& outError) {
    std::vector<HighPolyMesh> list;
    list.push_back(highPoly);
    return BuildMultiSubToolBVH(list, false, outError);
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
        rtcAttachGeometry(scene, geom);
        rtcReleaseGeometry(geom);
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
    std::vector<float>& displacement,
    std::vector<float>& normR,
    std::vector<float>& normG,
    std::vector<float>& normB,
    const std::vector<uint8_t>& mask,
    int width,
    int height,
    int paddingPixels
) {
    if (paddingPixels <= 0 || width <= 0 || height <= 0) return;

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

    // 2. Jump Flooding Passes: step sizes halving from max(W, H)/2 down to 1
    int initialStep = std::max(width, height) / 2;
    std::vector<int> stepSizes;
    for (int step = initialStep; step >= 1; step /= 2) {
        stepSizes.push_back(step);
    }
    // JFA+1 pass for improved discrete boundary accuracy
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

                // Sample 8 neighbors at offset 'step'
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

    // 3. Final Dilation & Renormalization Pass
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int idx = y * width + x;
            if (mask[idx]) continue; // Already has original bake value

            SeedPoint seed = finalGrid[idx];
            if (seed.x >= 0) {
                int dx = x - seed.x;
                int dy = y - seed.y;
                int distSq = dx * dx + dy * dy;

                if (distSq <= maxDistSq) {
                    int srcIdx = seed.y * width + seed.x;

                    // Exact nearest copy: zero float drift!
                    if (!displacement.empty()) {
                        displacement[idx] = displacement[srcIdx];
                    }

                    // Renormalize normal map
                    if (!normR.empty() && !normG.empty() && !normB.empty()) {
                        float nx = normR[srcIdx] * 2.0f - 1.0f;
                        float ny = normG[srcIdx] * 2.0f - 1.0f;
                        float nz = normB[srcIdx] * 2.0f - 1.0f;
                        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
                        if (len > 1e-6f) {
                            nx /= len;
                            ny /= len;
                            nz /= len;
                        }
                        normR[idx] = 0.5f * nx + 0.5f;
                        normG[idx] = 0.5f * ny + 0.5f;
                        normB[idx] = 0.5f * nz + 0.5f;
                    }
                }
            }
        }
    }
}

void BakerEngine::ApplySeamPadding(
    std::vector<float>& channel,
    const std::vector<uint8_t>& mask,
    int width,
    int height,
    int paddingPixels
) {
    std::vector<float> empty;
    ApplyJumpFloodingDilation(channel, empty, empty, empty, mask, width, height, paddingPixels);
}

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
    size_t totalTiles = lowPoly.detectedUDIMs.size();
    bool hasExplicitCage = lowPoly.HasExplicitCage();

    for (size_t tileIdx = 0; tileIdx < totalTiles; ++tileIdx) {
        int tile = lowPoly.detectedUDIMs[tileIdx];

        if (progressCallback) {
            float baseProg = static_cast<float>(tileIdx) / static_cast<float>(totalTiles);
            progressCallback(baseProg, "Baking UDIM Tile " + std::to_string(tile) +
                (hasExplicitCage ? " (Explicit Cage)..." : " (Auto Cage)..."));
        }

        BakeResult res;
        res.udimTile = tile;
        res.width = W;
        res.height = H;
        res.displacementMap.resize(W * H, 0.0f);
        res.normalMapR.resize(W * H, 0.5f);
        res.normalMapG.resize(W * H, 0.5f);
        res.normalMapB.resize(W * H, 1.0f);
        res.diagnosticMapR.resize(W * H, 0.0f);
        res.diagnosticMapG.resize(W * H, 0.0f);
        res.diagnosticMapB.resize(W * H, 0.0f);
        res.hitMask.resize(W * H, 0);

        int tileUOffset = (tile - 1001) % 10;
        int tileVOffset = (tile - 1001) / 10;

        RTCScene scene = static_cast<RTCScene>(m_embreeScene);

        std::vector<size_t> tileTriangles;
        for (size_t t = 0; t < lowPoly.triangles.size(); ++t) {
            if (lowPoly.triangles[t].udimTile == tile) {
                tileTriangles.push_back(t);
            }
        }

        for (size_t triIdx : tileTriangles) {
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

            float minU = std::min({ u0, u1, u2 });
            float maxU = std::max({ u0, u1, u2 });
            float minV = std::min({ v0_val, v1_val, v2_val });
            float maxV = std::max({ v0_val, v1_val, v2_val });

            int minX = std::max(0, static_cast<int>(std::floor(minU * W)));
            int maxX = std::min(W - 1, static_cast<int>(std::ceil(maxU * W)));
            int minY = std::max(0, static_cast<int>(std::floor(minV * H)));
            int maxY = std::min(H - 1, static_cast<int>(std::ceil(maxV * H)));

            float denom = (v1_val - v2_val) * (u0 - u2) + (u2 - u1) * (v0_val - v2_val);
            if (std::abs(denom) < 1e-9f) continue;
            float invDenom = 1.0f / denom;

            for (int y = minY; y <= maxY; ++y) {
                float vPix = (static_cast<float>(y) + 0.5f) / static_cast<float>(H);

                for (int x = minX; x <= maxX; ++x) {
                    float uPix = (static_cast<float>(x) + 0.5f) / static_cast<float>(W);

                    float w0 = ((v1_val - v2_val) * (uPix - u2) + (u2 - u1) * (vPix - v2_val)) * invDenom;
                    float w1 = ((v2_val - v0_val) * (uPix - u2) + (u0 - u2) * (vPix - v2_val)) * invDenom;
                    float w2 = 1.0f - w0 - w1;

                    if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;

                    Vec3 pos = v0.position * w0 + v1.position * w1 + v2.position * w2;
                    Vec3 norm = (v0.normal * w0 + v1.normal * w1 + v2.normal * w2).Normalized();

                    Vec3 tan3 = (Vec3(v0.tangent.x, v0.tangent.y, v0.tangent.z) * w0 +
                                 Vec3(v1.tangent.x, v1.tangent.y, v1.tangent.z) * w1 +
                                 Vec3(v2.tangent.x, v2.tangent.y, v2.tangent.z) * w2).Normalized();

                    float tanSign = v0.tangent.w;
                    Vec3 bitan = (norm.Cross(tan3) * tanSign).Normalized();

                    Vec3 rayOrg;
                    Vec3 rayDir;
                    float maxDist = 0.0f;

                    if (hasExplicitCage) {
                        const Vec3& c0 = lowPoly.cagePositions[tri.indices[0]];
                        const Vec3& c1 = lowPoly.cagePositions[tri.indices[1]];
                        const Vec3& c2 = lowPoly.cagePositions[tri.indices[2]];
                        Vec3 cagePos = c0 * w0 + c1 * w1 + c2 * w2;

                        Vec3 cageToLow = pos - cagePos;
                        float cageDist = cageToLow.Length();
                        rayOrg = cagePos;

                        if (cageDist > 1e-6f) {
                            rayDir = cageToLow * (1.0f / cageDist);
                        } else {
                            rayDir = norm * -1.0f;
                        }
                        maxDist = cageDist + settings.maxRayDistance;
                    } else {
                        rayOrg = pos + norm * settings.cageDistance;
                        rayDir = norm * -1.0f;
                        maxDist = settings.cageDistance + settings.maxRayDistance;
                    }

                    RTCIntersectContext context;
                    rtcInitIntersectContext(&context);

                    RTCRayHit rayhit;
                    rayhit.ray.org_x = rayOrg.x;
                    rayhit.ray.org_y = rayOrg.y;
                    rayhit.ray.org_z = rayOrg.z;
                    rayhit.ray.dir_x = rayDir.x;
                    rayhit.ray.dir_y = rayDir.y;
                    rayhit.ray.dir_z = rayDir.z;
                    rayhit.ray.tnear = 0.0f;
                    rayhit.ray.tfar = maxDist;
                    rayhit.ray.mask = settings.groupMask;
                    rayhit.ray.flags = 0;
                    rayhit.hit.geomID = RTC_INVALID_GEOMETRY_ID;
                    rayhit.hit.primID = RTC_INVALID_GEOMETRY_ID;

                    rtcIntersect1(scene, &context, &rayhit);

                    int pixelIdx = y * W + x;

                    if (rayhit.hit.geomID != RTC_INVALID_GEOMETRY_ID) {
                        Vec3 hitPos = rayOrg + rayDir * rayhit.ray.tfar;
                        Vec3 hitNorm = Vec3(rayhit.hit.Ng_x, rayhit.hit.Ng_y, rayhit.hit.Ng_z).Normalized();

                        // 1. Displacement
                        float disp = (hitPos - pos).Dot(norm);
                        res.displacementMap[pixelIdx] = disp;

                        // 2. Tangent-Space Normal
                        float nTx = hitNorm.Dot(tan3);
                        float nTy = hitNorm.Dot(bitan);
                        if (settings.flipYNormal) nTy = -nTy;
                        float nTz = hitNorm.Dot(norm);

                        Vec3 tsNorm = Vec3(nTx, nTy, nTz).Normalized();

                        res.normalMapR[pixelIdx] = 0.5f * tsNorm.x + 0.5f;
                        res.normalMapG[pixelIdx] = 0.5f * tsNorm.y + 0.5f;
                        res.normalMapB[pixelIdx] = 0.5f * tsNorm.z + 0.5f;

                        // 3. Diagnostic Map: Green = Clean hit, Yellow = Extreme angle
                        if (hitNorm.Dot(norm) < 0.0f) {
                            res.diagnosticMapR[pixelIdx] = 1.0f;
                            res.diagnosticMapG[pixelIdx] = 1.0f;
                            res.diagnosticMapB[pixelIdx] = 0.0f; // Yellow (potential self-intersection/fold)
                        } else {
                            res.diagnosticMapR[pixelIdx] = 0.0f;
                            res.diagnosticMapG[pixelIdx] = 1.0f;
                            res.diagnosticMapB[pixelIdx] = 0.0f; // Green
                        }

                        res.hitMask[pixelIdx] = 1;
                    } else {
                        // Diagnostic Map: Red = Miss
                        res.diagnosticMapR[pixelIdx] = 1.0f;
                        res.diagnosticMapG[pixelIdx] = 0.0f;
                        res.diagnosticMapB[pixelIdx] = 0.0f;
                    }
                }
            }
        }

        // Apply Jump Flooding Algorithm nearest-texel dilation across tile boundaries
        if (settings.seamPadding > 0) {
            ApplyJumpFloodingDilation(
                res.displacementMap,
                res.normalMapR,
                res.normalMapG,
                res.normalMapB,
                res.hitMask,
                W,
                H,
                settings.seamPadding
            );
        }

        outResults.push_back(std::move(res));
    }

    if (progressCallback) {
        progressCallback(1.0f, "Baking complete!");
    }

    return true;
}

} // namespace Baker
