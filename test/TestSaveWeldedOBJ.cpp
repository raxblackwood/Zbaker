#include "../src/baker/MeshTypes.h"
#include "../src/app/MeshLoader.h"
#include <iostream>

using namespace Baker;

int main() {
    std::string err;
    LowPolyMesh lp;
    std::string lpPath = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    if (!MeshLoader::LoadLowPolyOBJ(lpPath, lp, err)) {
        std::cerr << "Failed to load LP: " << err << std::endl;
        return 1;
    }
    std::cout << "Loaded LP: " << lp.vertices.size() << " corner vertices, " << lp.triangles.size() << " triangles.\n";

    std::string outPath = "C:/Users/raman/Downloads/temp/bb/Verified_LowPoly_Baked.obj";
    if (!MeshLoader::SaveLowPolyOBJWithNormals(outPath, lp, err)) {
        std::cerr << "Failed to save LP: " << err << std::endl;
        return 1;
    }
    std::cout << "Successfully saved welded LP to: " << outPath << std::endl;
    return 0;
}
