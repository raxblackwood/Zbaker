#include "UVPacker.h"
#include <algorithm>
#include <cmath>
#include <set>

namespace Baker {

namespace {

// Helper representing a free rectangular bin within a UDIM tile
struct BinRect {
    float minU = 0.0f;
    float minV = 0.0f;
    float maxU = 0.0f;
    float maxV = 0.0f;
    int udimTile = 1001;

    float Width() const { return std::max(0.0f, maxU - minU); }
    float Height() const { return std::max(0.0f, maxV - minV); }

    bool Contains(const BinRect& o) const {
        return o.minU >= minU - 1e-6f && o.maxU <= maxU + 1e-6f &&
               o.minV >= minV - 1e-6f && o.maxV <= maxV + 1e-6f &&
               o.udimTile == udimTile;
    }
};

void GetUDIMTileOffsets(int tile, float& outU, float& outV) {
    int rel = tile - 1001;
    outU = static_cast<float>(rel % 10);
    outV = static_cast<float>(rel / 10);
}

} // namespace

PackStats UVPacker::PackIslands(std::vector<UVIsland>& islands, const PackSettings& settings) {
    PackStats stats;
    if (islands.empty()) return stats;

    float res = static_cast<float>((settings.resolution > 64) ? settings.resolution : 2048);
    float marginUV = static_cast<float>(settings.marginPixels) / res;
    float padUV = static_cast<float>(settings.paddingPixels) / res;

    // 1. Equalize / Normalize Texel Density across all islands
    float sum3DArea = 0.0f;
    float sumUVArea = 0.0f;
    for (const auto& isl : islands) {
        sum3DArea += isl.area3D;
        sumUVArea += isl.areaUV;
    }

    if (settings.normalizeTexelDensity && sum3DArea > 1e-7f && sumUVArea > 1e-7f) {
        float globalTD = sumUVArea / sum3DArea;
        for (auto& isl : islands) {
            if (isl.area3D > 1e-7f && isl.areaUV > 1e-7f) {
                float targetScale = std::sqrt((isl.area3D * globalTD) / isl.areaUV);
                isl.Scale(targetScale);
            }
        }
    }

    // 2. Rotation Optimization (minimize bounding box area)
    for (auto& isl : islands) {
        if (settings.rotation == PackSettings::RotationMode::None) {
            continue;
        }

        std::vector<float> testAngles;
        if (settings.rotation == PackSettings::RotationMode::Steps90) {
            testAngles = { 0.0f, 1.5707963f, 3.1415926f, 4.7123889f };
        } else if (settings.rotation == PackSettings::RotationMode::Steps45) {
            for (int k = 0; k < 8; ++k) {
                testAngles.push_back(k * 0.78539816f);
            }
        } else if (settings.rotation == PackSettings::RotationMode::Free) {
            for (int k = 0; k < 24; ++k) {
                testAngles.push_back(k * (6.2831853f / 24.0f));
            }
        }

        float bestAngle = 0.0f;
        float minBBoxArea = 1e30f;
        UVIsland temp = isl;

        for (float ang : testAngles) {
            temp = isl;
            temp.Rotate(ang);
            float area = temp.bounds.Width() * temp.bounds.Height();
            if (area < minBBoxArea) {
                minBBoxArea = area;
                bestAngle = ang;
            }
        }

        if (std::abs(bestAngle) > 1e-5f) {
            isl.Rotate(bestAngle);
        }
    }

    // 3. Multi-tile / Single-tile MaxRects BSSF 2D Bin Packing
    // Grouping by polygroup if requested
    if (settings.packByPolygroup) {
        std::sort(islands.begin(), islands.end(), [](const UVIsland& a, const UVIsland& b) {
            if (a.polygroup != b.polygroup) return a.polygroup < b.polygroup;
            return (a.bounds.Width() * a.bounds.Height()) > (b.bounds.Width() * b.bounds.Height());
        });
    } else {
        // Sort descending by maximum bounding dimension (heuristic: largest islands first)
        std::sort(islands.begin(), islands.end(), [](const UVIsland& a, const UVIsland& b) {
            float maxDimA = (std::max)(a.bounds.Width(), a.bounds.Height());
            float maxDimB = (std::max)(b.bounds.Width(), b.bounds.Height());
            if (std::abs(maxDimA - maxDimB) > 1e-6f) return maxDimA > maxDimB;
            return (a.bounds.Width() * a.bounds.Height()) > (b.bounds.Width() * b.bounds.Height());
        });
    }

    // Packing loop with scale scaling if single tile overflow occurs
    float globalScale = 1.0f;
    bool packedSuccessfully = false;
    std::set<int> usedTilesSet;

    for (int packAttempt = 0; packAttempt < 15; ++packAttempt) {
        usedTilesSet.clear();
        int currentTile = 1001;
        usedTilesSet.insert(currentTile);

        float uOff, vOff;
        GetUDIMTileOffsets(currentTile, uOff, vOff);

        std::vector<BinRect> freeRects;
        freeRects.push_back({
            uOff + marginUV,
            vOff + marginUV,
            uOff + 1.0f - marginUV,
            vOff + 1.0f - marginUV,
            currentTile
        });

        bool allFit = true;

        for (size_t i = 0; i < islands.size(); ++i) {
            auto& isl = islands[i];
            float w = isl.bounds.Width() * globalScale + padUV;
            float h = isl.bounds.Height() * globalScale + padUV;

            // Find best free rectangle across active tiles
            int bestRectIdx = -1;
            float bestShortSideFit = 1e30f;

            for (size_t r = 0; r < freeRects.size(); ++r) {
                const auto& fr = freeRects[r];
                if (fr.Width() >= w && fr.Height() >= h) {
                    float leftoverHoriz = fr.Width() - w;
                    float leftoverVert = fr.Height() - h;
                    float shortSide = (std::min)(leftoverHoriz, leftoverVert);
                    if (shortSide < bestShortSideFit) {
                        bestShortSideFit = shortSide;
                        bestRectIdx = static_cast<int>(r);
                    }
                }
            }

            // If it doesn't fit in current free rects and multiUDIM is enabled
            if (bestRectIdx < 0 && settings.multiUDIM && static_cast<int>(usedTilesSet.size()) < settings.maxUDIMCount) {
                currentTile++;
                usedTilesSet.insert(currentTile);
                GetUDIMTileOffsets(currentTile, uOff, vOff);
                freeRects.push_back({
                    uOff + marginUV,
                    vOff + marginUV,
                    uOff + 1.0f - marginUV,
                    vOff + 1.0f - marginUV,
                    currentTile
                });
                bestRectIdx = static_cast<int>(freeRects.size()) - 1;
            }

            if (bestRectIdx < 0) {
                allFit = false;
                break;
            }

            // Place island in bestRect
            BinRect targetRect = freeRects[bestRectIdx];
            float placeU = targetRect.minU;
            float placeV = targetRect.minV;

            BinRect placedRect{
                placeU,
                placeV,
                placeU + w,
                placeV + h,
                targetRect.udimTile
            };

            // MaxRects splitting of overlapping free rectangles
            std::vector<BinRect> nextFreeRects;
            for (const auto& fr : freeRects) {
                if (fr.udimTile != placedRect.udimTile ||
                    placedRect.minU >= fr.maxU || placedRect.maxU <= fr.minU ||
                    placedRect.minV >= fr.maxV || placedRect.maxV <= fr.minV) {
                    nextFreeRects.push_back(fr);
                    continue;
                }

                // Split into up to 4 sub-rectangles
                if (placedRect.minU > fr.minU) {
                    nextFreeRects.push_back({ fr.minU, fr.minV, placedRect.minU, fr.maxV, fr.udimTile });
                }
                if (placedRect.maxU < fr.maxU) {
                    nextFreeRects.push_back({ placedRect.maxU, fr.minV, fr.maxU, fr.maxV, fr.udimTile });
                }
                if (placedRect.minV > fr.minV) {
                    nextFreeRects.push_back({ fr.minU, fr.minV, fr.maxU, placedRect.minV, fr.udimTile });
                }
                if (placedRect.maxV < fr.maxV) {
                    nextFreeRects.push_back({ fr.minU, placedRect.maxV, fr.maxU, fr.maxV, fr.udimTile });
                }
            }

            // Prune contained / redundant rectangles
            freeRects.clear();
            for (size_t a = 0; a < nextFreeRects.size(); ++a) {
                bool isContained = false;
                for (size_t b = 0; b < nextFreeRects.size(); ++b) {
                    if (a != b && nextFreeRects[b].Contains(nextFreeRects[a])) {
                        isContained = true;
                        break;
                    }
                }
                if (!isContained && nextFreeRects[a].Width() > 1e-5f && nextFreeRects[a].Height() > 1e-5f) {
                    freeRects.push_back(nextFreeRects[a]);
                }
            }

            // Record target position
            isl.targetUDIM = targetRect.udimTile;
            float du = placeU - isl.bounds.minU;
            float dv = placeV - isl.bounds.minV;
            isl.Translate(du, dv);
        }

        if (allFit) {
            packedSuccessfully = true;
            break;
        }

        // Scale down and retry
        globalScale *= 0.88f;
    }

    if (globalScale < 0.999f) {
        for (auto& isl : islands) {
            isl.Scale(globalScale);
        }
    }

    // 4. Calculate final statistics
    float totalPackedUVArea = 0.0f;
    for (const auto& isl : islands) {
        totalPackedUVArea += isl.bounds.Width() * isl.bounds.Height();
    }

    stats.packedIslands = static_cast<int>(islands.size());
    stats.udimTilesUsed = (std::max)(1, static_cast<int>(usedTilesSet.size()));
    stats.packingEfficiency = std::clamp(totalPackedUVArea / static_cast<float>(stats.udimTilesUsed), 0.0f, 1.0f);
    stats.averageTexelDensity = (sum3DArea > 1e-7f) ? (totalPackedUVArea / sum3DArea) : 1.0f;

    return stats;
}

void UVPacker::ApplyToMesh(LowPolyMesh& mesh, const std::vector<UVIsland>& islands) {
    if (mesh.vertices.empty()) return;

    std::set<int> uniqueUDIMs;

    for (const auto& isl : islands) {
        uniqueUDIMs.insert(isl.targetUDIM);

        // Assign UDIM tile to all triangles in this island
        for (int tIdx : isl.triangleIndices) {
            if (tIdx >= 0 && tIdx < static_cast<int>(mesh.triangles.size())) {
                mesh.triangles[tIdx].udimTile = isl.targetUDIM;
            }
        }

        // Assign local UV coordinates back to mesh vertices
        for (size_t i = 0; i < isl.vertexIndices.size(); ++i) {
            uint32_t vIdx = isl.vertexIndices[i];
            if (vIdx < mesh.vertices.size() && i < isl.uvs.size()) {
                mesh.vertices[vIdx].uv.u = isl.uvs[i].u;
                mesh.vertices[vIdx].uv.v = isl.uvs[i].v;
            }
        }
    }

    mesh.detectedUDIMs.assign(uniqueUDIMs.begin(), uniqueUDIMs.end());
}

} // namespace Baker
