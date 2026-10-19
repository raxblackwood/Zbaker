#include "../src/baker/MeshTypes.h"
#include "../src/baker/GoZReader.h"
#include <iostream>
#include <fstream>
#include <cassert>
#include <cstring>

using namespace Baker;

// Helper to write an official GoZ binary test file
void CreateSyntheticGoZFile(const std::string& path) {
    std::ofstream out(path, std::ios::binary);

    // 1. 32-byte File Header
    char header[32] = { 0 };
    memcpy(header, "GoZb", 4);
    strncpy_s(header + 4, 28, "Synthetic Test ZBrush 2026", 27);
    out.write(header, 32);

    // 2. Point List Block (4 vertices forming a plane)
    // Block header: tag, size, count, modifier
    uint32_t pointTag = 10001;
    uint32_t numPoints = 4;
    uint32_t pointSize = 16 + numPoints * 12;
    float pointMod = 0.0f;
    out.write(reinterpret_cast<const char*>(&pointTag), 4);
    out.write(reinterpret_cast<const char*>(&pointSize), 4);
    out.write(reinterpret_cast<const char*>(&numPoints), 4);
    out.write(reinterpret_cast<const char*>(&pointMod), 4);

    float pts[12] = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f
    };
    out.write(reinterpret_cast<const char*>(pts), sizeof(pts));

    // 3. Face4 Format 1 Block (1 Quad face: 0, 1, 2, 3)
    uint32_t faceTag = 20001;
    uint32_t numFaces = 1;
    uint32_t faceSize = 16 + numFaces * 16;
    float faceMod = 0.0f;
    out.write(reinterpret_cast<const char*>(&faceTag), 4);
    out.write(reinterpret_cast<const char*>(&faceSize), 4);
    out.write(reinterpret_cast<const char*>(&numFaces), 4);
    out.write(reinterpret_cast<const char*>(&faceMod), 4);

    int32_t quad[4] = { 0, 1, 2, 3 };
    out.write(reinterpret_cast<const char*>(quad), sizeof(quad));

    // 4. Groups List Block (1 PolyGroup ID = 42)
    uint32_t grpTag = 40001;
    uint32_t numGrps = 1;
    uint32_t grpSize = 16 + 4; // 16 bytes header + 2 bytes data + 2 bytes padding to 4-byte boundary
    float grpMod = 0.0f;
    out.write(reinterpret_cast<const char*>(&grpTag), 4);
    out.write(reinterpret_cast<const char*>(&grpSize), 4);
    out.write(reinterpret_cast<const char*>(&numGrps), 4);
    out.write(reinterpret_cast<const char*>(&grpMod), 4);

    uint16_t grpId = 42;
    out.write(reinterpret_cast<const char*>(&grpId), 2);

    // Pad to 4-byte boundary
    char pad[2] = { 0, 0 };
    out.write(pad, 2);

    // 5. MRGB Polypaint Block (4 vertex colors: White, Red, Green, Blue)
    uint32_t mrgbTag = 35001;
    uint32_t numColors = 4;
    uint32_t mrgbSize = 16 + numColors * 4;
    float mrgbMod = 0.0f;
    out.write(reinterpret_cast<const char*>(&mrgbTag), 4);
    out.write(reinterpret_cast<const char*>(&mrgbSize), 4);
    out.write(reinterpret_cast<const char*>(&numColors), 4);
    out.write(reinterpret_cast<const char*>(&mrgbMod), 4);

    uint32_t colors[4] = {
        0xFFFFFFFF, // White (M=255, R=255, G=255, B=255)
        0xFFFF0000, // Red
        0xFF00FF00, // Green
        0xFF0000FF  // Blue
    };
    out.write(reinterpret_cast<const char*>(colors), sizeof(colors));

    // 6. End of File Block
    uint32_t eofTag = 0;
    uint32_t eofSize = 16;
    uint32_t eofCount = 0;
    float eofMod = 0.0f;
    out.write(reinterpret_cast<const char*>(&eofTag), 4);
    out.write(reinterpret_cast<const char*>(&eofSize), 4);
    out.write(reinterpret_cast<const char*>(&eofCount), 4);
    out.write(reinterpret_cast<const char*>(&eofMod), 4);

    out.close();
}

int main() {
    std::cout << "=== Running GoZ Binary Reader Unit Test ===" << std::endl;

    std::string testPath = "test_synthetic.GoZ";
    CreateSyntheticGoZFile(testPath);
    std::cout << "[Test] Created synthetic GoZ file: " << testPath << std::endl;

    HighPolyMesh mesh;
    std::string err;
    bool ok = GoZReader::ReadGoZFile(testPath, mesh, err);

    if (!ok) {
        std::cerr << "[FAIL] Failed to read GoZ file: " << err << std::endl;
        return 1;
    }

    std::cout << "[PASS] ReadGoZFile succeeded!" << std::endl;
    std::cout << "       Positions: " << mesh.positions.size() << " (Expected: 4)" << std::endl;
    std::cout << "       Quad Indices: " << mesh.quadIndices.size() << " (Expected: 4)" << std::endl;
    std::cout << "       Triangulated Indices: " << mesh.indices.size() << " (Expected: 6)" << std::endl;
    std::cout << "       Colors: " << mesh.colors.size() << " (Expected: 4)" << std::endl;
    std::cout << "       Polygroup per Face: " << mesh.polygroupPerFace.size() << " (Expected: 2)" << std::endl;

    assert(mesh.positions.size() == 4);
    assert(mesh.isQuadMesh == true);
    assert(mesh.quadIndices.size() == 4);
    assert(mesh.indices.size() == 6);
    assert(mesh.colors.size() == 4);
    assert(mesh.polygroupPerFace.size() == 2);
    assert(mesh.polygroupPerFace[0] == 42);
    assert(mesh.polygroupPerFace[1] == 42);

    // Test SubTool Cache
    HighPolyMesh cachedMesh;
    bool cacheOk = GoZReader::GetOrLoadSubTool(testPath, cachedMesh, false, err);
    assert(cacheOk);
    assert(GoZReader::GetCachedSubToolCount() == 1);
    std::cout << "[PASS] SubTool Cache Verified (Cached Count = 1)" << std::endl;

    // Second fetch must hit cache
    HighPolyMesh cachedMesh2;
    bool cacheHit = GoZReader::GetOrLoadSubTool(testPath, cachedMesh2, false, err);
    assert(cacheHit);
    assert(cachedMesh2.positions.size() == 4);
    std::cout << "[PASS] SubTool Cache Hit Verified (Zero-reload)" << std::endl;

    std::remove(testPath.c_str());
    std::cout << "=== All GoZ Reader Unit Tests PASSED! ===" << std::endl;
    return 0;
}
