#include "../src/baker/MeshTypes.h"
#include "../src/baker/BakerEngine.h"
#include "../src/baker/ExrWriter.h"
#include "../src/app/MeshLoader.h"
#include <iostream>

using namespace Baker;

int main() {
    std::string err;
    LowPolyMesh lp;
    std::string lpPath = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    if (!MeshLoader::LoadLowPolyOBJ(lpPath, lp, err)) {
        std::cerr << "LP error: " << err << std::endl;
        return 1;
    }
    std::cout << "LP loaded: " << lp.vertices.size() << " verts, " << lp.triangles.size() << " tris\n";

    HighPolyMesh hp;
    std::string hpPath = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj";
    if (!MeshLoader::LoadHighPolyOBJ(hpPath, hp, err)) {
        std::cerr << "HP error: " << err << std::endl;
        return 1;
    }
    std::cout << "HP loaded: " << hp.positions.size() << " verts, " << hp.indices.size() / 3 << " tris\n";

    BakerEngine engine;
    if (!engine.BuildHighPolyBVH(hp, err)) {
        std::cerr << "BVH error: " << err << std::endl;
        return 1;
    }
    std::cout << "BVH ready\n";

    BakeSettings settings;
    settings.resolution = 2048;
    settings.antiAliasing = 1;
    settings.cageDistance = 0.02f;
    settings.maxRayDistance = 0.1f;
    settings.seamPadding = 32;
    settings.angleThreshold = 75.0f;
    settings.discardBackfaceHits = true;
    settings.flipYNormal = false; // OpenGL (+Y)
    settings.bakeNormal = true;
    settings.bakeDiagnosticMap = true;
    settings.targetUDIMs = { 1001 };

    std::vector<BakeResult> results;
    if (!engine.ExecuteBake(lp, settings, results, [](float p, const std::string& s) {
        std::cout << "Progress: " << int(p * 100) << "%\r" << std::flush;
    }, err)) {
        std::cerr << "Bake error: " << err << std::endl;
        return 1;
    }

    std::cout << "\nBake finished!\n";
    if (!results.empty()) {
        std::string errStr;
        std::string outNorm = "C:/Users/raman/Downloads/temp/bb/Verified_Fixed_Normal.1001.png";
        std::string outDiag = "C:/Users/raman/Downloads/temp/bb/Verified_Fixed_Diag.1001.png";
        ExrWriter::SaveRGBPNG(outNorm, results[0].normalMapR.data(), results[0].normalMapG.data(), results[0].normalMapB.data(), 2048, 2048, errStr);
        ExrWriter::SaveRGBPNG(outDiag, results[0].diagnosticMapR.data(), results[0].diagnosticMapG.data(), results[0].diagnosticMapB.data(), 2048, 2048, errStr);
        std::cout << "Saved verified normal map: " << outNorm << "\n";

        std::string lpObjPath = "C:/Users/raman/Downloads/temp/bb/Verified_LowPoly_Baked.obj";
        MeshLoader::SaveLowPolyOBJWithNormals(lpObjPath, lp, errStr);
        std::cout << "Saved low poly with matching normals: " << lpObjPath << "\n";
    }

    return 0;
}