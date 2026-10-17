#include "../src/baker/MeshTypes.h"
#include "../src/baker/BakerEngine.h"
#include "../src/baker/ExrWriter.h"
#include "../src/app/MeshLoader.h"
#include <iostream>

using namespace Baker;

int main() {
    std::string err;
    LowPolyMesh lp;
    std::string lpPath = "C:/Users/raman/Downloads/SM_P_GB_Stone_Wall_Old_02_01_Low 1.fbx";
    if (!MeshLoader::LoadLowPolyFBX(lpPath, lp, err)) {
        std::cerr << "LP error: " << err << std::endl;
        return 1;
    }
    float w1_minU = 999, w1_maxU = -999, w1_minV = 999, w1_maxV = -999;
    float w2_minU = 999, w2_maxU = -999, w2_minV = 999, w2_maxV = -999;
    float w3_minU = 999, w3_maxU = -999, w3_minV = 999, w3_maxV = -999;
    int w1_tris = 0, w2_tris = 0, w3_tris = 0;
    for (const auto& tri : lp.triangles) {
        float avgX = (lp.vertices[tri.indices[0]].position.x + lp.vertices[tri.indices[1]].position.x + lp.vertices[tri.indices[2]].position.x) / 3.0f;
        if (avgX < -1.3f) {
            w1_tris++;
            for (int c = 0; c < 3; ++c) {
                float u = lp.vertices[tri.indices[c]].uv.u; float v = lp.vertices[tri.indices[c]].uv.v;
                w1_minU = (std::min)(w1_minU, u); w1_maxU = (std::max)(w1_maxU, u);
                w1_minV = (std::min)(w1_minV, v); w1_maxV = (std::max)(w1_maxV, v);
            }
        } else if (avgX > 1.1f) {
            w3_tris++;
            for (int c = 0; c < 3; ++c) {
                float u = lp.vertices[tri.indices[c]].uv.u; float v = lp.vertices[tri.indices[c]].uv.v;
                w3_minU = (std::min)(w3_minU, u); w3_maxU = (std::max)(w3_maxU, u);
                w3_minV = (std::min)(w3_minV, v); w3_maxV = (std::max)(w3_maxV, v);
            }
        } else {
            w2_tris++;
            for (int c = 0; c < 3; ++c) {
                float u = lp.vertices[tri.indices[c]].uv.u; float v = lp.vertices[tri.indices[c]].uv.v;
                w2_minU = (std::min)(w2_minU, u); w2_maxU = (std::max)(w2_maxU, u);
                w2_minV = (std::min)(w2_minV, v); w2_maxV = (std::max)(w2_maxV, v);
            }
        }
    }
    std::cout << "Wall 1 (" << w1_tris << " tris): U[" << w1_minU << ", " << w1_maxU << "], V[" << w1_minV << ", " << w1_maxV << "]\n";
    std::cout << "Wall 2 (" << w2_tris << " tris): U[" << w2_minU << ", " << w2_maxU << "], V[" << w2_minV << ", " << w2_maxV << "]\n";
    std::cout << "Wall 3 (" << w3_tris << " tris): U[" << w3_minU << ", " << w3_maxU << "], V[" << w3_minV << ", " << w3_maxV << "]\n";
    std::cout << "Detected UDIMs: ";
    for (int u : lp.detectedUDIMs) std::cout << u << " ";
    std::cout << "\n";
    for (int u : lp.detectedUDIMs) {
        int count = 0;
        float minU = 999, maxU = -999, minV = 999, maxV = -999;
        float minX = 999, maxX = -999;
        for (const auto& tri : lp.triangles) {
            if (tri.udimTile == u) {
                count++;
                for (int c = 0; c < 3; ++c) {
                    float uval = lp.vertices[tri.indices[c]].uv.u;
                    float vval = lp.vertices[tri.indices[c]].uv.v;
                    float xval = lp.vertices[tri.indices[c]].position.x;
                    minU = (std::min)(minU, uval); maxU = (std::max)(maxU, uval);
                    minV = (std::min)(minV, vval); maxV = (std::max)(maxV, vval);
                    minX = (std::min)(minX, xval); maxX = (std::max)(maxX, xval);
                }
            }
        }
        std::cout << "UDIM " << u << ": " << count << " tris, U[" << minU << ", " << maxU << "], V[" << minV << ", " << maxV << "], X[" << minX << ", " << maxX << "]\n";
    }
    std::cout << "LP Bounds: [" << lp.GetBoundingBoxMin().x << ", " << lp.GetBoundingBoxMin().y << ", " << lp.GetBoundingBoxMin().z
              << "] to [" << lp.GetBoundingBoxMax().x << ", " << lp.GetBoundingBoxMax().y << ", " << lp.GetBoundingBoxMax().z << "]\n";
    std::cout << "LP Center: [" << lp.GetBoundingBoxCenter().x << ", " << lp.GetBoundingBoxCenter().y << ", " << lp.GetBoundingBoxCenter().z << "], Diag: " << lp.GetBoundingBoxDiagonal() << "\n";

    HighPolyMesh hp;
    std::string hpPath = "C:/Users/raman/Downloads/SM_P_GB_Stone_Wall_Old_02_01_High 1.fbx";
    if (!MeshLoader::LoadHighPolyFBX(hpPath, hp, err)) {
        std::cerr << "HP error: " << err << std::endl;
        return 1;
    }
    std::cout << "HP loaded: " << hp.positions.size() << " verts, " << hp.indices.size() / 3 << " tris\n";
    std::cout << "HP Bounds: [" << hp.GetBoundingBoxMin().x << ", " << hp.GetBoundingBoxMin().y << ", " << hp.GetBoundingBoxMin().z
              << "] to [" << hp.GetBoundingBoxMax().x << ", " << hp.GetBoundingBoxMax().y << ", " << hp.GetBoundingBoxMax().z << "]\n";
    std::cout << "HP Center: [" << hp.GetBoundingBoxCenter().x << ", " << hp.GetBoundingBoxCenter().y << ", " << hp.GetBoundingBoxCenter().z << "], Diag: " << hp.GetBoundingBoxDiagonal() << "\n";

    BakerEngine engine;
    if (!engine.BuildHighPolyBVH(hp, err)) {
        std::cerr << "BVH error: " << err << std::endl;
        return 1;
    }
    std::cout << "BVH ready\n";

    BakeSettings settings;
    settings.resolution = 1024;
    settings.cageDistance = 0.05f;
    settings.maxRayDistance = 0.2f;
    settings.seamPadding = 16;
    settings.bakeDisplacement = true;
    settings.bakeNormal = true;
    settings.bakeAmbientOcclusion = true;
    settings.aoSamples = 16;
    settings.bakeCavity = true;
    settings.bakeCurvature = true;
    settings.cavityStrength = 1.5f;
    settings.bakeDiagnosticMap = true;
    settings.outputDir = "./";
    settings.baseFilename = "UserTest";

    std::vector<BakeResult> results;
    if (!engine.ExecuteBake(lp, settings, results, [](float p, const std::string& s) {
        std::cout << "Progress: " << int(p*100) << "%\r" << std::flush;
    }, err)) {
        std::cerr << "Bake error: " << err << std::endl;
        return 1;
    }

    std::cout << "\nBake finished! Results: " << results.size() << "\n";
    for (size_t i = 0; i < results.size(); ++i) {
        size_t hits = 0;
        for (auto h : results[i].hitMask) if (h) hits++;
        std::cout << "UDIM " << results[i].udimTile << " Hit texels: " << hits << " / " << (1024*1024) << " (" << (hits * 100.0 / (1024*1024)) << "%)\n";

        std::string sTile = std::to_string(results[i].udimTile);
        std::string errStr;
        ExrWriter::SaveRGBPNG("UserTest_Normal." + sTile + ".png", results[i].normalMapR.data(), results[i].normalMapG.data(), results[i].normalMapB.data(), 1024, 1024, errStr);
        ExrWriter::SaveGrayscalePNG("UserTest_AO." + sTile + ".png", results[i].aoMap.data(), 1024, 1024, errStr);
        ExrWriter::SaveGrayscalePNG("UserTest_Cavity." + sTile + ".png", results[i].cavityMap.data(), 1024, 1024, errStr);
        ExrWriter::SaveGrayscalePNG("UserTest_Curvature." + sTile + ".png", results[i].curvatureMap.data(), 1024, 1024, errStr);
        ExrWriter::SaveRGBPNG("UserTest_Diag." + sTile + ".png", results[i].diagnosticMapR.data(), results[i].diagnosticMapG.data(), results[i].diagnosticMapB.data(), 1024, 1024, errStr);
    }
    std::cout << "Saved Normal, AO, Cavity, and Curvature PNGs for all UDIMs!\n";
    return 0;
}