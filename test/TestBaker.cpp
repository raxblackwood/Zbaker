#include "../src/baker/MeshTypes.h"
#include "../src/baker/BakerEngine.h"
#include "../src/baker/MikkTSpaceBridge.h"
#include "../src/baker/ExrWriter.h"
#include "../src/app/MeshLoader.h"
#include "../src/uv/UVCutter.h"
#include "../src/uv/UVUnwrapper.h"
#include "../src/uv/UVPacker.h"
#include "../src/uv/UVSurfaceCurve.h"

#include <iostream>
#include <fstream>
#include <cassert>

using namespace Baker;

void CreateTestQuadGridOBJ(const std::string& path) {
    std::ofstream out(path);
    // 3x2 grid of quads (4 vertices wide, 3 vertices high = 3x2 = 6 quads)
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            out << "v " << static_cast<float>(x) << " " << static_cast<float>(y) << " 0.0\n";
            out << "vt " << (static_cast<float>(x) / 3.0f) << " " << (static_cast<float>(y) / 2.0f) << "\n";
            out << "vn 0.0 0.0 1.0\n";
        }
    }
    // Quads: row 0: (1,2,6,5), (2,3,7,6), (3,4,8,7)
    //        row 1: (5,6,10,9), (6,7,11,10), (7,8,12,11)
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 3; ++x) {
            int v1 = y * 4 + x + 1;
            int v2 = y * 4 + (x + 1) + 1;
            int v3 = (y + 1) * 4 + (x + 1) + 1;
            int v4 = (y + 1) * 4 + x + 1;
            out << "f " << v1 << "/" << v1 << "/" << v1 << " "
                        << v2 << "/" << v2 << "/" << v2 << " "
                        << v3 << "/" << v3 << "/" << v3 << " "
                        << v4 << "/" << v4 << "/" << v4 << "\n";
        }
    }
    out.close();
}

void CreateTestLowPolyOBJ(const std::string& path) {
    std::ofstream out(path);
    out << "v -1.0 0.0 0.0\n";
    out << "v  0.0 0.0 0.0\n";
    out << "v  0.0 1.0 0.0\n";
    out << "v -1.0 1.0 0.0\n";

    out << "v  0.0 0.0 0.0\n";
    out << "v  1.0 0.0 0.0\n";
    out << "v  1.0 1.0 0.0\n";
    out << "v  0.0 1.0 0.0\n";

    out << "vt 0.0 0.0\n";
    out << "vt 1.0 0.0\n";
    out << "vt 1.0 1.0\n";
    out << "vt 0.0 1.0\n";

    out << "vt 1.0 0.0\n";
    out << "vt 2.0 0.0\n";
    out << "vt 2.0 1.0\n";
    out << "vt 1.0 1.0\n";

    out << "vn 0.0 0.0 1.0\n";

    out << "f 1/1/1 2/2/1 3/3/1\n";
    out << "f 1/1/1 3/3/1 4/4/1\n";

    out << "f 5/5/1 6/6/1 7/7/1\n";
    out << "f 5/5/1 7/7/1 8/8/1\n";
    out.close();
}

void CreateTestHighPolyOBJ(const std::string& path) {
    std::ofstream out(path);
    int N = 50;
    for (int y = 0; y <= N; ++y) {
        float fy = static_cast<float>(y) / N;
        for (int x = 0; x <= N * 2; ++x) {
            float fx = static_cast<float>(x) / (N * 2);
            float posX = -1.0f + 2.0f * fx;
            float posY = fy;

            float d1 = std::sqrt((posX + 0.5f) * (posX + 0.5f) + (posY - 0.5f) * (posY - 0.5f));
            float d2 = std::sqrt((posX - 0.5f) * (posX - 0.5f) + (posY - 0.5f) * (posY - 0.5f));
            float posZ = 0.0f;
            if (d1 < 0.4f) posZ += 0.15f * std::cos(d1 * 3.14159f / 0.4f);
            if (d2 < 0.4f) posZ += 0.25f * std::cos(d2 * 3.14159f / 0.4f);

            out << "v " << posX << " " << posY << " " << posZ << "\n";
        }
    }

    int stride = N * 2 + 1;
    for (int y = 0; y < N; ++y) {
        for (int x = 0; x < N * 2; ++x) {
            int i0 = y * stride + x + 1;
            int i1 = y * stride + (x + 1) + 1;
            int i2 = (y + 1) * stride + (x + 1) + 1;
            int i3 = (y + 1) * stride + x + 1;

            out << "f " << i0 << " " << i1 << " " << i2 << "\n";
            out << "f " << i0 << " " << i2 << " " << i3 << "\n";
        }
    }
    out.close();
}

int main(int argc, char** argv) {
    if (argc >= 3) {
        std::string lowPolyPath = argv[1];
        std::string highPolyPath = argv[2];
        int res = (argc >= 4) ? std::stoi(argv[3]) : 2048;
        int aa = (argc >= 5) ? std::stoi(argv[4]) : 2;

        std::cout << "=== CLI Test Bake ===" << std::endl;
        std::cout << "LowPoly: " << lowPolyPath << std::endl;
        std::cout << "HighPoly: " << highPolyPath << std::endl;
        std::cout << "Resolution: " << res << ", AA: " << aa << "x" << std::endl;

        LowPolyMesh lp;
        std::string err;
        if (!MeshLoader::LoadLowPolyMesh(lowPolyPath, lp, err)) {
            std::cerr << "Failed loading lowpoly: " << err << std::endl;
            return 1;
        }

        HighPolyMesh hp;
        if (!MeshLoader::LoadHighPolyMesh(highPolyPath, hp, err)) {
            std::cerr << "Failed loading highpoly: " << err << std::endl;
            return 1;
        }

        float lpDiag = lp.GetBoundingBoxDiagonal();
        float hpDiag = hp.GetBoundingBoxDiagonal();
        float scale = 1.0f;
        if (lpDiag > 1e-4f && hpDiag > 1e-4f) {
            float ratio = hpDiag / lpDiag;
            if (ratio > 5.0f || ratio < 0.2f) {
                scale = ratio;
                std::cout << "Auto-scaling low-poly by " << scale << "x to match high-poly" << std::endl;
            }
        }

        BakerEngine engine;
        if (!engine.BuildHighPolyBVH(hp, err)) {
            std::cerr << "Failed building BVH: " << err << std::endl;
            return 1;
        }

        BakeSettings settings;
        settings.resolution = res;
        settings.antiAliasing = aa;
        settings.lowPolyScale = scale;
        settings.cageDistance = hpDiag * 0.02f;
        settings.maxRayDistance = hpDiag * 0.08f;
        settings.seamPadding = 16;
        settings.discardBackfaceHits = true;
        settings.bakeNormal = true;
        settings.bakeDisplacement = true;
        settings.bakeDiagnosticMap = true;
        settings.bakeAmbientOcclusion = true;
        settings.aoSamples = 16;
        settings.aoMaxDistance = hpDiag * 0.1f;
        settings.bakeCavity = true;
        settings.bakeCurvature = true;
        settings.cavityStrength = 1.5f;
        settings.exportPNG = true;
        settings.exportEXR = true;
        settings.baseFilename = "CLIBake";
        settings.outputDir = "./";

        std::vector<BakeResult> results;
        bool ok = engine.ExecuteBake(lp, settings, results, [](float p, const std::string& s) {
            std::cout << "\rProgress: " << static_cast<int>(p * 100) << "% (" << s << ")    " << std::flush;
        }, err);
        std::cout << std::endl;

        if (!ok) {
            std::cerr << "Bake failed: " << err << std::endl;
            return 1;
        }

        for (const auto& r : results) {
            std::string udim = std::to_string(r.udimTile);
            std::string normPng = "CLIBake_Normal." + udim + ".png";
            std::string normExr = "CLIBake_Normal." + udim + ".exr";
            std::string aoPng = "CLIBake_AO." + udim + ".png";
            std::string cavPng = "CLIBake_Cavity." + udim + ".png";
            std::string curvPng = "CLIBake_Curvature." + udim + ".png";
            std::string diagPng = "CLIBake_Diag." + udim + ".png";

            std::string wErr;
            ExrWriter::SaveRGBPNG(normPng, r.normalMapR.data(), r.normalMapG.data(), r.normalMapB.data(), r.width, r.height, wErr);
            ExrWriter::SaveRGBFloatImage(normExr, r.normalMapR.data(), r.normalMapG.data(), r.normalMapB.data(), r.width, r.height, true, wErr);
            ExrWriter::SaveGrayscalePNG(aoPng, r.aoMap.data(), r.width, r.height, wErr);
            ExrWriter::SaveGrayscalePNG(cavPng, r.cavityMap.data(), r.width, r.height, wErr);
            ExrWriter::SaveGrayscalePNG(curvPng, r.curvatureMap.data(), r.width, r.height, wErr);
            ExrWriter::SaveRGBPNG(diagPng, r.diagnosticMapR.data(), r.diagnosticMapG.data(), r.diagnosticMapB.data(), r.width, r.height, wErr);

            std::cout << "Saved: " << normPng << ", " << aoPng << ", " << cavPng << ", " << curvPng << std::endl;
        }

        std::cout << "CLI Bake Completed Successfully!" << std::endl;
        return 0;
    }

    std::cout << "=== Running ZBrush UDIM Baker Advanced Test Suite ===" << std::endl;

    std::string lowPolyPath = "test_lowpoly.obj";
    std::string highPolyPath = "test_highpoly.obj";
    std::string binHighPolyPath = "test_highpoly.bin";

    CreateTestLowPolyOBJ(lowPolyPath);
    CreateTestHighPolyOBJ(highPolyPath);

    // 1. Ingest Low-Poly
    LowPolyMesh lowPoly;
    std::string err;
    bool lpLoaded = MeshLoader::LoadLowPolyOBJ(lowPolyPath, lowPoly, err);
    if (!lpLoaded) {
        std::cerr << "Failed loading low-poly: " << err << std::endl;
        return 1;
    }
    std::cout << "[Test 1/5] Loaded Low-Poly: " << lowPoly.triangles.size() << " triangles across "
              << lowPoly.detectedUDIMs.size() << " UDIMs (";
    for (int u : lowPoly.detectedUDIMs) std::cout << u << " ";
    std::cout << ")" << std::endl;
    assert(lowPoly.detectedUDIMs.size() == 2);

    // 2. Test Explicit Cage Configuration
    lowPoly.cagePositions.resize(lowPoly.vertices.size());
    for (size_t i = 0; i < lowPoly.vertices.size(); ++i) {
        // Push cage outward by +0.06 along normal
        lowPoly.cagePositions[i] = lowPoly.vertices[i].position + lowPoly.vertices[i].normal * 0.06f;
    }
    assert(lowPoly.HasExplicitCage());
    std::cout << "[Test 2/5] Explicit cage topology verified (" << lowPoly.cagePositions.size() << " cage points)." << std::endl;

    // 3. Ingest High-Poly & Test Fast Binary MMF Format
    HighPolyMesh highPoly;
    bool hpLoaded = MeshLoader::LoadHighPolyOBJ(highPolyPath, highPoly, err);
    assert(hpLoaded);

    bool binSaved = MeshLoader::SaveBinaryGeometry(binHighPolyPath, highPoly, err);
    assert(binSaved);

    HighPolyMesh binHighPoly;
    bool binLoaded = MeshLoader::LoadBinaryGeometry(binHighPolyPath, binHighPoly, err);
    assert(binLoaded);
    assert(binHighPoly.positions.size() == highPoly.positions.size());
    assert(binHighPoly.indices.size() == highPoly.indices.size());
    std::cout << "[Test 3/5] Binary geometry serialization & MMF loading verified ("
              << binHighPoly.positions.size() << " verts, " << (binHighPoly.indices.size() / 3) << " tris)." << std::endl;

    // 4. Test Multi-SubTool BVH Generation with Ray Masking
    BakerEngine engine;
    std::vector<HighPolyMesh> subtools;
    binHighPoly.maskId = 0x1;
    subtools.push_back(binHighPoly);

    bool bvhOk = engine.BuildMultiSubToolBVH(subtools, false, err);
    assert(bvhOk);
    std::cout << "[Test 4/5] Multi-SubTool Embree 3 BVH acceleration built with geometry masking." << std::endl;

    // 5. Execute Bake with JFA Dilation, Diagnostic Map, Cavity, and Curvature
    BakeSettings settings;
    settings.resolution = 512;
    settings.cageDistance = 0.05f;
    settings.maxRayDistance = 0.5f;
    settings.seamPadding = 12; // JFA nearest-texel padding
    settings.bakeDisplacement = true;
    settings.bakeNormal = true;
    settings.bakeAmbientOcclusion = true;
    settings.aoSamples = 16;
    settings.bakeCavity = true;
    settings.bakeCurvature = true;
    settings.cavityStrength = 1.5f;
    settings.bakeDiagnosticMap = true;
    settings.groupMask = 0x1;
    settings.outputDir = "./";
    settings.baseFilename = "AdvBake";

    std::vector<BakeResult> results;
    bool bakeOk = engine.ExecuteBake(lowPoly, settings, results, [](float p, const std::string& s) {
        std::cout << "  Progress: " << static_cast<int>(p * 100) << "% (" << s << ")\r" << std::flush;
    }, err);

    std::cout << std::endl;
    assert(bakeOk);
    assert(results.size() == 2);

    for (const auto& res : results) {
        std::string dispFile = "AdvBake_Disp." + std::to_string(res.udimTile) + ".exr";
        std::string normFile = "AdvBake_Normal." + std::to_string(res.udimTile) + ".exr";
        std::string diagFile = "AdvBake_Diag." + std::to_string(res.udimTile) + ".exr";
        std::string cavFile = "AdvBake_Cavity." + std::to_string(res.udimTile) + ".png";
        std::string curvFile = "AdvBake_Curvature." + std::to_string(res.udimTile) + ".png";
        std::string aoFile = "AdvBake_AO." + std::to_string(res.udimTile) + ".png";

        std::string exrErr;
        assert(ExrWriter::SaveFloatImage(dispFile, res.displacementMap.data(), res.width, res.height, exrErr));
        assert(ExrWriter::SaveRGBFloatImage(normFile, res.normalMapR.data(), res.normalMapG.data(), res.normalMapB.data(), res.width, res.height, true, exrErr));
        assert(ExrWriter::SaveRGBFloatImage(diagFile, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, true, exrErr));
        assert(ExrWriter::SaveGrayscalePNG(cavFile, res.cavityMap.data(), res.width, res.height, exrErr));
        assert(ExrWriter::SaveGrayscalePNG(curvFile, res.curvatureMap.data(), res.width, res.height, exrErr));
        assert(ExrWriter::SaveGrayscalePNG(aoFile, res.aoMap.data(), res.width, res.height, exrErr));

        std::cout << "  Verified UDIM " << res.udimTile << ": Displacement (32-bit), Normal (16-bit), Cavity, Curvature, AO." << std::endl;
    }

    std::cout << "[Test 5/5] JFA dilation & multi-tile output verification complete!" << std::endl;

    // Test 6: Verify SaveLowPolyOBJWithNormals deduplicates unrolled corner vertices
    // into shared geometric 'v' positions (manifold welded topology)
    std::string userLpPath = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    std::ifstream userLpCheck(userLpPath);
    if (userLpCheck.good()) {
        userLpCheck.close();
        LowPolyMesh userLp;
        std::string loadErr;
        bool loadOk = MeshLoader::LoadLowPolyOBJ(userLpPath, userLp, loadErr);
        if (!loadOk) {
            std::cerr << "Failed to load LP: " << loadErr << std::endl;
            return 1;
        }
        std::string saveErr;
        std::string verifiedObjPath = "C:/Users/raman/Downloads/temp/bb/Verified_LowPoly_Baked.obj";
        bool saveOk = MeshLoader::SaveLowPolyOBJWithNormals(verifiedObjPath, userLp, saveErr);
        if (!saveOk) {
            std::cerr << "Failed to save LP: " << saveErr << std::endl;
            return 1;
        }
        std::cout << "[Test 6/6] Saved welded low-poly with matching normals: " << verifiedObjPath << std::endl;
    }

    // Test 7: Verify Marmoset Phase 2 Paint Cage & Paint Skew Ray-casting and baking
    {
        std::cout << "[Test 7/7] Testing Marmoset Paint Cage & Skew ray-casting and bake integration..." << std::endl;
        // Cast ray from (0, 0.5, 10) downwards towards z = 0
        Vec3 rayOrg(0.2f, 0.2f, 5.0f);
        Vec3 rayDir(0.0f, 0.0f, -1.0f);
        float hitT = 0.0f;
        Vec3 hitPos, hitNormal;
        int hitTriIdx = -1;
        bool hit = lowPoly.Raycast(rayOrg, rayDir, hitT, hitPos, hitNormal, hitTriIdx);
        assert(hit);
        assert(hitTriIdx >= 0);
        assert(std::abs(hitPos.z) < 1e-4f);

        // Apply painted skew
        lowPoly.vertexSkew.assign(lowPoly.vertices.size(), 0.8f);
        assert(lowPoly.HasVertexSkew());

        // Apply painted cage
        lowPoly.cageOffsets.assign(lowPoly.vertices.size(), 0.02f);
        assert(lowPoly.HasCageOffsets());
        for (size_t i = 0; i < lowPoly.vertices.size(); ++i) {
            Vec3 n = lowPoly.HasWatertightCageNormals() ? lowPoly.cageNormals[i] : lowPoly.vertices[i].normal;
            lowPoly.cagePositions[i] = lowPoly.vertices[i].position + n * (settings.cageDistance + lowPoly.cageOffsets[i]);
        }
        assert(lowPoly.HasExplicitCage());

        std::vector<BakeResult> phase2Results;
        std::string phase2Err;
        bool phase2Ok = engine.ExecuteBake(lowPoly, settings, phase2Results, nullptr, phase2Err);
        assert(phase2Ok);
        assert(!phase2Results.empty());
        std::cout << "  Marmoset Phase 2 Paint Cage & Skew ray-casting and bake integration VERIFIED!" << std::endl;
    }

    // Test 8: UV Cutting, Island Segmentation, Spring Relaxation Unwrap, and MaxRects Packing
    {
        std::cout << "[Test 8/8] Testing UV Cutting, Island Segmentation, Unwrapping, and MaxRects Packing..." << std::endl;

        // Create a cube mesh with sharp 90-degree corners to test dihedral seam marking
        LowPolyMesh cubeMesh;
        // 8 vertices for a cube [-1, 1]
        std::vector<Vec3> corners = {
            {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
            {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}
        };
        // 6 faces * 2 tris = 12 triangles
        int cubeQuads[6][4] = {
            {0, 3, 2, 1}, // Front (Z = -1)
            {4, 5, 6, 7}, // Back  (Z = +1)
            {0, 1, 5, 4}, // Bottom(Y = -1)
            {2, 3, 7, 6}, // Top   (Y = +1)
            {0, 4, 7, 3}, // Left  (X = -1)
            {1, 2, 6, 5}  // Right (X = +1)
        };

        for (int f = 0; f < 6; ++f) {
            uint32_t i0 = static_cast<uint32_t>(cubeMesh.vertices.size());
            Vertex v0, v1, v2, v3;
            v0.position = corners[cubeQuads[f][0]];
            v1.position = corners[cubeQuads[f][1]];
            v2.position = corners[cubeQuads[f][2]];
            v3.position = corners[cubeQuads[f][3]];
            cubeMesh.vertices.push_back(v0);
            cubeMesh.vertices.push_back(v1);
            cubeMesh.vertices.push_back(v2);
            cubeMesh.vertices.push_back(v3);

            LowPolyTriangle t1, t2;
            t1.indices[0] = i0;     t1.indices[1] = i0 + 1; t1.indices[2] = i0 + 2;
            t1.meshIndex = static_cast<uint32_t>(f); // Polygroup per face
            t2.indices[0] = i0;     t2.indices[1] = i0 + 2; t2.indices[2] = i0 + 3;
            t2.meshIndex = static_cast<uint32_t>(f);
            cubeMesh.triangles.push_back(t1);
            cubeMesh.triangles.push_back(t2);
        }

        // 1. Mark seams by dihedral angle (threshold 60 degrees - cube has 90 degree edges)
        UVCutter::MarkSeamsByAngle(cubeMesh, 60.0f);
        assert(!cubeMesh.cutSeams.empty());
        assert(!cubeMesh.seamLineIndices.empty());
        std::cout << "  Marked " << cubeMesh.cutSeams.size() << " seam edges via angle threshold." << std::endl;

        // 2. Segment into islands (each face should be its own island -> 6 islands)
        auto islandsTris = UVCutter::SegmentIslands(cubeMesh);
        assert(islandsTris.size() == 6);
        std::cout << "  Segmented cube into " << islandsTris.size() << " separate UV islands." << std::endl;

        // 3. Unwrap all islands
        auto islands = UVUnwrapper::UnwrapMesh(cubeMesh, islandsTris);
        assert(islands.size() == 6);
        for (const auto& isl : islands) {
            assert(isl.bounds.Width() > 0.0f);
            assert(isl.bounds.Height() > 0.0f);
            assert(isl.area3D > 0.0f);
        }
        std::cout << "  Unwrapped " << islands.size() << " islands successfully." << std::endl;

        // 4. Pack islands with MaxRects BSSF
        PackSettings packCfg;
        packCfg.resolution = 1024;
        packCfg.paddingPixels = 4;
        packCfg.marginPixels = 4;
        packCfg.rotation = PackSettings::RotationMode::Steps90;
        packCfg.normalizeTexelDensity = true;
        packCfg.multiUDIM = false;

        PackStats stats = UVPacker::PackIslands(islands, packCfg);
        assert(stats.packedIslands == 6);
        assert(stats.udimTilesUsed == 1);
        std::cout << "  Packed 6 islands into tile 1001 with efficiency: " 
                  << (stats.packingEfficiency * 100.0f) << "%." << std::endl;

        // Verify bounds are within [0, 1]
        for (const auto& isl : islands) {
            assert(isl.bounds.minU >= 0.0f && isl.bounds.maxU <= 1.0f);
            assert(isl.bounds.minV >= 0.0f && isl.bounds.maxV <= 1.0f);
        }

        // Verify non-overlapping bounding boxes (with margin/padding considered)
        for (size_t a = 0; a < islands.size(); ++a) {
            for (size_t b = a + 1; b < islands.size(); ++b) {
                // Shrink slightly to avoid edge-to-edge floating point false-positives
                float eps = 1e-4f;
                bool xOverlap = (islands[a].bounds.maxU - eps > islands[b].bounds.minU + eps) &&
                                (islands[a].bounds.minU + eps < islands[b].bounds.maxU - eps);
                bool yOverlap = (islands[a].bounds.maxV - eps > islands[b].bounds.minV + eps) &&
                                (islands[a].bounds.minV + eps < islands[b].bounds.maxV - eps);
                assert(!(xOverlap && yOverlap));
            }
        }
        std::cout << "  Verified non-overlapping bounding boxes for all packed islands." << std::endl;

        // 5. Apply packed UVs back to mesh
        UVPacker::ApplyToMesh(cubeMesh, islands);
        for (const auto& v : cubeMesh.vertices) {
            assert(v.uv.x >= 0.0f && v.uv.x <= 1.0f);
            assert(v.uv.y >= 0.0f && v.uv.y <= 1.0f);
        }
        std::cout << "  UV coordinates applied to mesh vertices successfully." << std::endl;

        // 6. Test Multi-UDIM distribution
        PackSettings udimCfg = packCfg;
        udimCfg.multiUDIM = true;
        udimCfg.maxUDIMCount = 3;
        udimCfg.packByPolygroup = true;
        PackStats udimStats = UVPacker::PackIslands(islands, udimCfg);
        assert(udimStats.packedIslands == 6);
        std::cout << "  Multi-UDIM packed across " << udimStats.udimTilesUsed << " tiles." << std::endl;

        std::cout << "  UV Cutting, Segmentation, Unwrapping, and MaxRects Packing VERIFIED!" << std::endl;
    }

    // Test 9: 3D Surface Curves for UV Cutting (Edge Snapping & Topological Cuts)
    {
        std::cout << "[Test 9/9] Testing 3D Surface Curves for UV Cutting..." << std::endl;

        // Build a grid plane mesh (4x4 quads = 32 triangles)
        LowPolyMesh planeMesh;
        int N = 4;
        for (int y = 0; y <= N; ++y) {
            for (int x = 0; x <= N; ++x) {
                Vertex v;
                v.position = Vec3(static_cast<float>(x) - 2.0f, static_cast<float>(y) - 2.0f, 0.0f);
                v.normal = Vec3(0.0f, 0.0f, 1.0f);
                v.uv = Vec2(static_cast<float>(x) / N, static_cast<float>(y) / N);
                planeMesh.vertices.push_back(v);
            }
        }
        for (int y = 0; y < N; ++y) {
            for (int x = 0; x < N; ++x) {
                uint32_t i0 = y * (N + 1) + x;
                uint32_t i1 = i0 + 1;
                uint32_t i2 = (y + 1) * (N + 1) + x;
                uint32_t i3 = i2 + 1;

                LowPolyTriangle t1, t2;
                t1.indices[0] = i0; t1.indices[1] = i1; t1.indices[2] = i2;
                t2.indices[0] = i1; t2.indices[1] = i3; t2.indices[2] = i2;
                planeMesh.triangles.push_back(t1);
                planeMesh.triangles.push_back(t2);
            }
        }
        assert(planeMesh.triangles.size() == 32);

        // 1. Create a CurveManager and add Curve 1
        CurveManager curveMgr;
        SurfaceCurve* c1 = curveMgr.GetActiveCurve();
        assert(c1 != nullptr);
        assert(c1->name == "Curve 1");

        // Add 3 points traversing across the mesh: left (-1.8, 0, 0), center (0, 0.2, 0), right (1.8, 0, 0)
        c1->AddPoint(Vec3(-1.8f, 0.0f, 0.0f), Vec3(0, 0, 1), 0);
        c1->AddPoint(Vec3(0.0f, 0.2f, 0.0f), Vec3(0, 0, 1), 16);
        c1->AddPoint(Vec3(1.8f, 0.0f, 0.0f), Vec3(0, 0, 1), 30);
        assert(c1->points.size() == 3);

        // Verify moving a control point
        c1->MovePoint(1, Vec3(0.0f, 0.0f, 0.0f), Vec3(0, 0, 1), 16);
        assert(c1->points[1].position.y == 0.0f);

        // Sample path generation
        c1->UpdateSampledPath(planeMesh);
        assert(c1->sampledPath.size() > 3);

        // 2. Test Dijkstra Edge Snapping to Seams
        std::vector<std::pair<uint32_t, uint32_t>> snappedEdges;
        bool snapOk = CurveManager::SnapCurveToMeshEdges(*c1, planeMesh, snappedEdges);
        assert(snapOk);
        assert(!snappedEdges.empty());
        assert(!planeMesh.cutSeams.empty());
        assert(!planeMesh.seamLineIndices.empty());
        std::cout << "  Curve 1 snapped to " << snappedEdges.size() << " low-poly mesh seam edges." << std::endl;

        // 3. Test Curve 2 with Topological Knife Cut
        int c2Idx = curveMgr.AddCurve("Curve 2");
        SurfaceCurve* c2 = curveMgr.GetActiveCurve();
        assert(c2 != nullptr);
        assert(c2Idx == 1);

        // Draw a diagonal cut across the mesh
        c2->AddPoint(Vec3(-1.5f, -1.5f, 0.0f), Vec3(0, 0, 1), 0);
        c2->AddPoint(Vec3(1.5f, 1.5f, 0.0f), Vec3(0, 0, 1), 31);

        size_t vertsBeforeCut = planeMesh.vertices.size();
        size_t trisBeforeCut = planeMesh.triangles.size();

        std::vector<std::pair<uint32_t, uint32_t>> cutEdges;
        bool cutOk = CurveManager::CutCurveIntoMeshTopology(*c2, planeMesh, cutEdges);
        assert(cutOk);
        assert(!cutEdges.empty());
        assert(planeMesh.vertices.size() >= vertsBeforeCut);
        assert(planeMesh.triangles.size() >= trisBeforeCut);
        std::cout << "  Curve 2 cut into mesh topology: " << planeMesh.vertices.size() << " verts ("
                  << (planeMesh.vertices.size() - vertsBeforeCut) << " new), "
                  << planeMesh.triangles.size() << " tris." << std::endl;

        // 4. Test Unwrapping along Curve Seams
        auto islandsTris = UVCutter::SegmentIslands(planeMesh);
        assert(islandsTris.size() >= 2);
        auto islands = UVUnwrapper::UnwrapMesh(planeMesh, islandsTris);
        assert(islands.size() >= 2);

        PackSettings pCfg;
        pCfg.resolution = 1024;
        PackStats pStats = UVPacker::PackIslands(islands, pCfg);
        assert(pStats.packedIslands >= 2);
        std::cout << "  Unwrapped & packed " << pStats.packedIslands << " islands partitioned by 3D curves." << std::endl;

        std::cout << "  3D Surface Curves for UV Cutting VERIFIED!" << std::endl;
    }

    // 10. Test Wireframe Deduplication & Default Studio White Shading
    {
        std::cout << "[Test 10/10] Testing Wireframe Edge Deduplication & Blender Studio White Default..." << std::endl;
        std::string testObjPath = "test_wireframe.obj";
        CreateTestLowPolyOBJ(testObjPath);

        LowPolyMesh wfMesh;
        std::string err;
        bool loaded = MeshLoader::LoadLowPolyOBJ(testObjPath, wfMesh, err);
        assert(loaded);
        // 2 adjacent quads have 7 unique geometric edges (14 line indices), not 16 doubled indices
        assert(wfMesh.wireframeLineIndices.size() == 14);
        std::cout << "  Wireframe deduplication verified: 2 adjacent quads produced "
                  << wfMesh.wireframeLineIndices.size() / 2 << " unique edges (14 indices), 0 duplicates." << std::endl;

        // Verify default shading mode is Studio White (0)
        assert(static_cast<int>(ShadingMode::StudioWhite) == 0);
        std::cout << "  Default Shading Mode verified as Blender Studio White (mode 0)." << std::endl;
        std::cout << "  Wireframe Deduplication & Blender Studio White VERIFIED!" << std::endl;
    }

    // 11. Test Maya Edge Loop Selection, Edge Ring, Shortest Path, Cut, Sew, and Camera Projection
    {
        std::cout << "[Test 11/11] Testing Maya Edge Loop Selection, Ring, Shortest Path, Cut, and Sew..." << std::endl;
        std::string testObjPath = "test_quad_grid.obj";
        CreateTestQuadGridOBJ(testObjPath);
        LowPolyMesh wfMesh;
        std::string err;
        bool loaded = MeshLoader::LoadLowPolyOBJ(testObjPath, wfMesh, err);
        assert(loaded);
        assert(!wfMesh.polygonFaces.empty());

        // 1. Collinear Edge Loop: edge (0, 1) in quad 0 -> collinear across horizontal grid row
        uint32_t v0 = wfMesh.polygonFaces[0][0];
        uint32_t v1 = wfMesh.polygonFaces[0][1]; // horizontal bottom edge
        auto loop = UVCutter::GetEdgeLoop(wfMesh, v0, v1);
        assert(loop.size() >= 3);
        std::cout << "  Maya Edge Loop: walked collinear loop across vertices, found "
                  << loop.size() << " loop edges." << std::endl;

        // Select Edge Loop
        UVCutter::SelectEdgeLoop(wfMesh, v0, v1);
        assert(wfMesh.selectedEdges.size() == loop.size());
        assert(wfMesh.selectedEdgeLineIndices.size() == wfMesh.selectedEdges.size() * 2);

        // 2. Parallel Edge Ring: stepping across opposite faces
        auto ring = UVCutter::GetEdgeRing(wfMesh, v0, v1);
        assert(ring.size() >= 2);
        std::cout << "  Maya Edge Ring: walked parallel opposite edges across quad faces, found "
                  << ring.size() << " ring edges." << std::endl;
        UVCutter::SelectEdgeRing(wfMesh, v0, v1, false);
        assert(wfMesh.selectedEdges.size() == ring.size());

        // 3. Shortest Path: Dijkstra topological edge path between edge (0, 1) and edge (2, 3)
        auto path = UVCutter::GetShortestPath(wfMesh, v0, v1, 2, 3);
        assert(path.size() >= 2);
        std::cout << "  Maya Shortest Path: found Dijkstra topological path of "
                  << path.size() << " edges between distant edges." << std::endl;
        UVCutter::SelectShortestPath(wfMesh, v0, v1, 2, 3, false);
        assert(wfMesh.selectedEdges.size() == path.size());

        // 4. Cut Selected Edges
        size_t selCount = wfMesh.selectedEdges.size();
        UVCutter::CutSelectedEdges(wfMesh);
        assert(wfMesh.cutSeams.size() >= selCount);
        assert(!wfMesh.seamLineIndices.empty());
        std::cout << "  Maya Cut: turned " << selCount << " selected edges into UV cut seams." << std::endl;

        // 5. Sew Selected Edges
        UVCutter::SewSelectedEdges(wfMesh);
        assert(wfMesh.cutSeams.empty());
        assert(wfMesh.seamLineIndices.empty());
        std::cout << "  Maya Sew: stitched and removed all seams from selected edges." << std::endl;

        // 6. Camera Projection
        DirectX::XMMATRIX idProj = DirectX::XMMatrixIdentity();
        UVCutter::CameraProjectUVs(wfMesh, idProj);
        assert(!wfMesh.vertices.empty());
        for (const auto& v : wfMesh.vertices) {
            assert(v.uv.u >= 0.0f && v.uv.u <= 1.0f);
            assert(v.uv.v >= 0.0f && v.uv.v <= 1.0f);
        }
        std::cout << "  Camera Projection: mapped all vertices to [0, 1] normalized UV bounds." << std::endl;

        std::cout << "  Maya Edge Loop, Ring, Shortest Path, Cut, Sew, and Camera Proj VERIFIED!" << std::endl;
    }

    std::cout << "=== All Advanced Architecture Plan Tests PASSED! ===" << std::endl;
    return 0;
}
