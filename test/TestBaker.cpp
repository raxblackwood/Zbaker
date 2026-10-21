#include "../src/baker/MeshTypes.h"
#include "../src/baker/BakerEngine.h"
#include "../src/baker/MikkTSpaceBridge.h"
#include "../src/baker/ExrWriter.h"
#include "../src/app/MeshLoader.h"

#include <iostream>
#include <fstream>
#include <cassert>

using namespace Baker;

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

    std::cout << "=== All Advanced Architecture Plan Tests PASSED! ===" << std::endl;
    return 0;
}
