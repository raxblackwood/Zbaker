#pragma once

#include "UVTypes.h"
#include <vector>

namespace Baker {

struct PackSettings {
    int resolution = 2048;
    int paddingPixels = 8;
    int marginPixels = 8;

    enum class RotationMode {
        None = 0,
        Steps90 = 1,
        Steps45 = 2,
        Free = 3
    };
    RotationMode rotation = RotationMode::Steps90;

    bool normalizeTexelDensity = true;
    bool multiUDIM = false;
    int maxUDIMCount = 4;
    bool packByPolygroup = false;
};

struct PackStats {
    int packedIslands = 0;
    int udimTilesUsed = 1;
    float packingEfficiency = 0.0f; // [0..1] percentage of UV area to packed tile area
    float averageTexelDensity = 0.0f;
};

class UVPacker {
public:
    // Packs an array of UVIslands into one or more UDIM tiles according to PackSettings
    static PackStats PackIslands(std::vector<UVIsland>& islands, const PackSettings& settings);

    // Applies packed UV island coordinates and UDIM tile assignments directly to LowPolyMesh
    static void ApplyToMesh(LowPolyMesh& mesh, const std::vector<UVIsland>& islands);
};

} // namespace Baker
