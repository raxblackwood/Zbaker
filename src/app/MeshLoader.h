#pragma once

#include "../baker/MeshTypes.h"
#include <string>

namespace Baker {

class MeshLoader {
public:
    // Loads a low-poly OBJ mesh, computes MikkTSpace tangents and detects UDIM tiles
    static bool LoadLowPolyOBJ(
        const std::string& filePath,
        LowPolyMesh& outMesh,
        std::string& outError
    );

    // Loads an explicit cage OBJ mesh, validating topology against the loaded low-poly mesh
    static bool LoadCageOBJ(
        const std::string& filePath,
        LowPolyMesh& lowPoly,
        std::string& outError
    );

    // Loads a high-poly OBJ mesh for BVH acceleration
    static bool LoadHighPolyOBJ(
        const std::string& filePath,
        HighPolyMesh& outMesh,
        std::string& outError
    );

    // Fast binary geometry reader with memory-mapped file (MMF) capability for 50M-100M sculpts
    static bool LoadBinaryGeometry(
        const std::string& binPath,
        HighPolyMesh& outMesh,
        std::string& outError
    );

    // Saves high-poly mesh to fast binary format
    static bool SaveBinaryGeometry(
        const std::string& binPath,
        const HighPolyMesh& mesh,
        std::string& outError
    );

    // Loads a Multi-SubTool manifest file (JSON or line-delimited config)
    static bool LoadSubToolManifest(
        const std::string& manifestPath,
        MultiSubToolManifest& outManifest,
        std::string& outError
    );
};

} // namespace Baker
