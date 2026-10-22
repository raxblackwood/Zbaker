#pragma once

#include "../baker/MeshTypes.h"
#include <string>

namespace Baker {

class MeshLoader {
public:
    // Unified loader: auto-detects .obj or .fbx, computes MikkTSpace tangents and detects UDIMs
    static bool LoadLowPolyMesh(
        const std::string& filePath,
        LowPolyMesh& outMesh,
        std::string& outError
    );

    // Unified loader: auto-detects .obj or .fbx, validates topology against loaded low-poly
    static bool LoadCageMesh(
        const std::string& filePath,
        LowPolyMesh& lowPoly,
        std::string& outError
    );

    // Unified loader: auto-detects .obj, .fbx, or .bin for BVH acceleration
    static bool LoadHighPolyMesh(
        const std::string& filePath,
        HighPolyMesh& outMesh,
        std::string& outError
    );

    // Specific OBJ loaders
    static bool LoadLowPolyOBJ(
        const std::string& filePath,
        LowPolyMesh& outMesh,
        std::string& outError
    );

    // Exports the low-poly mesh with explicit vn vertex normals matching the baker exactly
    static bool SaveLowPolyOBJWithNormals(
        const std::string& filePath,
        const LowPolyMesh& mesh,
        std::string& outError
    );

    // Deduplicates wireframe lines based on 3D geometric edge positions (eliminates bold/overlapping edges)
    static void DeduplicateWireframeLines(LowPolyMesh& mesh);

    static bool LoadCageOBJ(
        const std::string& filePath,
        LowPolyMesh& lowPoly,
        std::string& outError
    );

    static bool LoadHighPolyOBJ(
        const std::string& filePath,
        HighPolyMesh& outMesh,
        std::string& outError
    );

    // Specific FBX loaders (Autodesk FBX ASCII & Binary via ufbx)
    static bool LoadLowPolyFBX(
        const std::string& filePath,
        LowPolyMesh& outMesh,
        std::string& outError
    );

    static bool LoadCageFBX(
        const std::string& filePath,
        LowPolyMesh& lowPoly,
        std::string& outError
    );

    static bool LoadHighPolyFBX(
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
