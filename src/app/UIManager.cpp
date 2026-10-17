#include "UIManager.h"
#include "MeshLoader.h"
#include "../baker/ExrWriter.h"

#include <imgui.h>
#include <windows.h>
#include <commdlg.h>
#include <thread>
#include <chrono>
#include <sstream>

namespace Baker {

static std::string OpenFileDialog(const wchar_t* filter, HWND owner = nullptr) {
    wchar_t filename[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        int len = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string result(len - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, filename, -1, &result[0], len, nullptr, nullptr);
            return result;
        }
    }
    return "";
}

static std::string ChooseFolderDialog(HWND owner = nullptr) {
    // Simple fallback or standard folder prompt
    wchar_t path[MAX_PATH] = L"./";
    return "./";
}

UIManager::UIManager() {
    m_settings.resolution = 2048;
    m_settings.cageDistance = 0.05f;
    m_settings.maxRayDistance = 0.2f;
    m_settings.seamPadding = 16;
    m_settings.bakeDisplacement = true;
    m_settings.bakeNormal = true;
    m_settings.outputDir = "./";
    m_settings.baseFilename = "Bake";
}

UIManager::~UIManager() {
    m_engine.Release();
}

void UIManager::Initialize() {
    m_logEntries.push_back("[System] ZBrush UDIM Baker initialized.");
}

void UIManager::SetCommandLineHighPoly(const std::string& path) {
    if (!path.empty()) {
        m_highPolyPath = path;
        m_logEntries.push_back("[ZBrush] Ingested High-Poly path: " + path);
        std::string err;
        if (MeshLoader::LoadHighPolyOBJ(path, m_highPolyMesh, err)) {
            m_hasHighPoly = true;
            m_logEntries.push_back("[Mesh] Successfully loaded High-Poly (" +
                std::to_string(m_highPolyMesh.positions.size()) + " verts, " +
                std::to_string(m_highPolyMesh.indices.size() / 3) + " tris).");

            std::string bvhErr;
            if (m_engine.BuildHighPolyBVH(m_highPolyMesh, bvhErr)) {
                m_logEntries.push_back("[Embree] High-Poly BVH accelerated structure built.");
            } else {
                m_logEntries.push_back("[Error] Embree BVH build failed: " + bvhErr);
            }
        } else {
            m_logEntries.push_back("[Error] Failed loading High-Poly: " + err);
        }
    }
}

void UIManager::SetCommandLineLowPoly(const std::string& path) {
    if (!path.empty()) {
        m_lowPolyPath = path;
        m_logEntries.push_back("[ZBrush] Ingested Low-Poly path: " + path);
        std::string err;
        if (MeshLoader::LoadLowPolyOBJ(path, m_lowPolyMesh, err)) {
            m_hasLowPoly = true;
            m_logEntries.push_back("[Mesh] Loaded Low-Poly (" +
                std::to_string(m_lowPolyMesh.vertices.size()) + " verts, " +
                std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIMs).");
        } else {
            m_logEntries.push_back("[Error] Failed loading Low-Poly: " + err);
        }
    }
}

void UIManager::AutoDetectZBrushCache() {
    std::string cacheLow = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    std::string cacheHigh = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj";

    DWORD attrLow = GetFileAttributesA(cacheLow.c_str());
    if (attrLow != INVALID_FILE_ATTRIBUTES && !(attrLow & FILE_ATTRIBUTE_DIRECTORY)) {
        SetCommandLineLowPoly(cacheLow);
    }

    DWORD attrHigh = GetFileAttributesA(cacheHigh.c_str());
    if (attrHigh != INVALID_FILE_ATTRIBUTES && !(attrHigh & FILE_ATTRIBUTE_DIRECTORY)) {
        SetCommandLineHighPoly(cacheHigh);
    }
}

void UIManager::RenderUI() {
    RenderMenuBar();

    ImGui::SetNextWindowPos(ImVec2(10, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380, 500), ImGuiCond_FirstUseEver);
    RenderMeshPanel();

    ImGui::SetNextWindowPos(ImVec2(400, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
    RenderSettingsPanel();

    ImGui::SetNextWindowPos(ImVec2(830, 30), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 500), ImGuiCond_FirstUseEver);
    RenderUDIMCanvasPanel();

    ImGui::SetNextWindowPos(ImVec2(10, 540), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1240, 200), ImGuiCond_FirstUseEver);
    RenderBakeProgressPanel();
}

void UIManager::RenderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Load Low-Poly Mesh (.obj)...")) {
                std::string path = OpenFileDialog(L"Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    m_lowPolyPath = path;
                    std::string err;
                    if (MeshLoader::LoadLowPolyOBJ(path, m_lowPolyMesh, err)) {
                        m_hasLowPoly = true;
                        m_logEntries.push_back("[Mesh] Loaded Low-Poly: " + path + " (" +
                            std::to_string(m_lowPolyMesh.vertices.size()) + " verts, " +
                            std::to_string(m_lowPolyMesh.triangles.size()) + " tris, " +
                            std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIMs).");
                    } else {
                        m_logEntries.push_back("[Error] Failed to load Low-Poly: " + err);
                    }
                }
            }

            if (ImGui::MenuItem("Load Explicit Cage Mesh (.obj)...")) {
                std::string path = OpenFileDialog(L"Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    std::string err;
                    if (MeshLoader::LoadCageOBJ(path, m_lowPolyMesh, err)) {
                        m_hasExplicitCage = true;
                        m_cagePath = path;
                        m_logEntries.push_back("[Cage] Loaded explicit cage: " + path + " (matches " +
                            std::to_string(m_lowPolyMesh.cagePositions.size()) + " vertices).");
                    } else {
                        m_logEntries.push_back("[Error] " + err);
                    }
                }
            }

            if (ImGui::MenuItem("Load High-Poly Sculpt (.obj / .bin)...")) {
                std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.bin)\0*.obj;*.bin\0All Files (*.*)\0*.*\0");
                if (!path.empty()) {
                    SetCommandLineHighPoly(path);
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit")) {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("About UDIM Baker")) {
                m_logEntries.push_back("[About] ZBrush UDIM Baker v1.0. Intel Embree + MikkTSpace + 32-bit EXR.");
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
}

void UIManager::RenderMeshPanel() {
    ImGui::Begin("Mesh Ingestion", nullptr);

    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "ZBrush Live Sync:");
    if (ImGui::Button("Sync / Load from ZBrush Cache", ImVec2(-1, 32))) {
        AutoDetectZBrushCache();
    }
    ImGui::Separator();

    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Target Low-Poly Mesh (with UDIMs):");
    if (ImGui::Button("Browse Low-Poly OBJ...", ImVec2(-1, 28))) {
        std::string path = OpenFileDialog(L"Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            m_lowPolyPath = path;
            std::string err;
            if (MeshLoader::LoadLowPolyOBJ(path, m_lowPolyMesh, err)) {
                m_hasLowPoly = true;
                m_logEntries.push_back("[Mesh] Loaded Low-Poly (" + std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIMs).");
            } else {
                m_logEntries.push_back("[Error] " + err);
            }
        }
    }
    if (m_hasLowPoly) {
        ImGui::Text("File: %s", m_lowPolyPath.c_str());
        ImGui::BulletText("Vertices: %zu", m_lowPolyMesh.vertices.size());
        ImGui::BulletText("Triangles: %zu", m_lowPolyMesh.triangles.size());
        ImGui::BulletText("Detected UDIMs: %zu tiles", m_lowPolyMesh.detectedUDIMs.size());

        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.8f, 0.5f, 1.0f, 1.0f), "Explicit Cage Mesh (Optional):");
        if (ImGui::Button("Browse Explicit Cage OBJ...", ImVec2(-1, 26))) {
            std::string path = OpenFileDialog(L"Wavefront OBJ (*.obj)\0*.obj\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                std::string err;
                if (MeshLoader::LoadCageOBJ(path, m_lowPolyMesh, err)) {
                    m_hasExplicitCage = true;
                    m_cagePath = path;
                    m_logEntries.push_back("[Cage] Loaded explicit cage: " + path + " (topology verified).");
                } else {
                    m_logEntries.push_back("[Error] " + err);
                }
            }
        }
        if (m_hasExplicitCage) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "Explicit Cage Active (%zu verts)", m_lowPolyMesh.cagePositions.size());
        } else {
            ImGui::TextDisabled("Using automatic normal extrusion cage.");
        }
    } else {
        ImGui::TextDisabled("No Low-Poly mesh loaded.");
    }

    ImGui::Separator();

    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "Source High-Poly Mesh (ZBrush Sculpt):");
    if (ImGui::Button("Browse High-Poly OBJ / BIN...", ImVec2(-1, 30))) {
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.bin)\0*.obj;*.bin\0All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            SetCommandLineHighPoly(path);
        }
    }
    if (m_hasHighPoly) {
        ImGui::Text("File: %s", m_highPolyPath.c_str());
        ImGui::BulletText("Positions: %zu", m_highPolyMesh.positions.size());
        ImGui::BulletText("Triangles: %zu", m_highPolyMesh.indices.size() / 3);
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "BVH Acceleration Ready (Embree 3)");
    } else {
        ImGui::TextDisabled("No High-Poly mesh loaded.");
    }

    ImGui::End();
}

void UIManager::RenderSettingsPanel() {
    ImGui::Begin("Bake Parameters", nullptr);

    const char* resolutions[] = { "1024 x 1024", "2048 x 2048", "4096 x 4096", "8192 x 8192" };
    static int currentResIdx = 1; // 2048
    if (ImGui::Combo("Map Resolution", &currentResIdx, resolutions, IM_ARRAYSIZE(resolutions))) {
        int resMap[] = { 1024, 2048, 4096, 8192 };
        m_settings.resolution = resMap[currentResIdx];
    }

    if (!m_hasExplicitCage) {
        ImGui::SliderFloat("Cage Offset", &m_settings.cageDistance, 0.001f, 1.0f, "%.4f");
    } else {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Cage Offset: Explicit Mesh Driven");
    }
    ImGui::SliderFloat("Max Ray Length", &m_settings.maxRayDistance, 0.01f, 5.0f, "%.3f");
    ImGui::SliderInt("Seam Padding / JFA (px)", &m_settings.seamPadding, 0, 64);

    ImGui::Separator();
    ImGui::Checkbox("Displacement Map (32-bit Float EXR)", &m_settings.bakeDisplacement);
    ImGui::Checkbox("Tangent Normal Map (MikkTSpace)", &m_settings.bakeNormal);
    ImGui::Checkbox("Diagnostic Map (Miss/Hit/Clamp EXR)", &m_settings.bakeDiagnosticMap);
    ImGui::Checkbox("Flip Normal Y (DirectX)", &m_settings.flipYNormal);
    ImGui::Checkbox("Compact BVH (RTC_BUILD_QUALITY_LOW) [50M-100M]", &m_settings.useCompactBVH);

    ImGui::Separator();
    static char baseNameBuf[128] = "Sculpt_Bake";
    ImGui::InputText("Base Output Name", baseNameBuf, IM_ARRAYSIZE(baseNameBuf));
    m_settings.baseFilename = baseNameBuf;

    static char outDirBuf[256] = "./";
    ImGui::InputText("Output Folder", outDirBuf, IM_ARRAYSIZE(outDirBuf));
    m_settings.outputDir = outDirBuf;

    ImGui::Separator();
    bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking;
    if (!canBake) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("START HARDWARE-ACCELERATED BAKE", ImVec2(-1, 45))) {
        TriggerBake();
    }
    if (!canBake) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void UIManager::RenderUDIMCanvasPanel() {
    ImGui::Begin("UDIM Canvas & Tile Layout", nullptr);

    if (!m_hasLowPoly || m_lowPolyMesh.detectedUDIMs.empty()) {
        ImGui::TextDisabled("Load a low-poly mesh to inspect detected UDIM tiles.");
        ImGui::End();
        return;
    }

    ImGui::Text("Detected UDIM Tiles (%zu total):", m_lowPolyMesh.detectedUDIMs.size());

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    float tileSize = 60.0f;
    float tilePadding = 10.0f;

    for (size_t i = 0; i < m_lowPolyMesh.detectedUDIMs.size(); ++i) {
        int tile = m_lowPolyMesh.detectedUDIMs[i];
        int col = (tile - 1001) % 10;
        int row = (tile - 1001) / 10;

        ImVec2 p0(canvasPos.x + col * (tileSize + tilePadding), canvasPos.y + row * (tileSize + tilePadding));
        ImVec2 p1(p0.x + tileSize, p0.y + tileSize);

        drawList->AddRectFilled(p0, p1, IM_COL32(45, 60, 80, 255), 4.0f);
        drawList->AddRect(p0, p1, IM_COL32(100, 160, 240, 255), 4.0f, 0, 2.0f);

        char tileLabel[16];
        snprintf(tileLabel, sizeof(tileLabel), "%d", tile);
        ImVec2 textPos(p0.x + 12, p0.y + 20);
        drawList->AddText(textPos, IM_COL32(255, 255, 255, 255), tileLabel);
    }

    ImGui::Dummy(ImVec2(400, 300));
    ImGui::End();
}

void UIManager::RenderBakeProgressPanel() {
    ImGui::Begin("Status & Output Log", nullptr);

    if (m_isBaking) {
        ImGui::ProgressBar(m_bakeProgress, ImVec2(-1, 20), m_bakeStatus.c_str());
    } else {
        ImGui::ProgressBar(m_bakeProgress, ImVec2(-1, 20), "Ready");
    }

    ImGui::BeginChild("LogRegion", ImVec2(0, 0), true);
    for (const auto& log : m_logEntries) {
        ImGui::TextUnformatted(log.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    ImGui::End();
}

void UIManager::TriggerBake() {
    m_isBaking = true;
    m_bakeProgress = 0.0f;
    m_bakeStatus = "Starting bake...";
    m_logEntries.push_back("[Bake] Initiating bake session across " +
        std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIM tiles...");

    std::thread([this]() {
        std::vector<BakeResult> results;
        std::string err;

        auto progressCb = [this](float prog, const std::string& status) {
            this->m_bakeProgress = prog;
            this->m_bakeStatus = status;
        };

        auto startTime = std::chrono::high_resolution_clock::now();

        bool success = m_engine.ExecuteBake(m_lowPolyMesh, m_settings, results, progressCb, err);

        auto endTime = std::chrono::high_resolution_clock::now();
        double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

        if (success) {
            std::ostringstream oss;
            oss << "[Bake] Ray-tracing finished in " << elapsedSec << "s. Writing EXR output files...";
            m_logEntries.push_back(oss.str());

            for (const auto& res : results) {
                // Save displacement EXR: <baseName>_Disp.<UDIM>.exr
                if (m_settings.bakeDisplacement) {
                    std::string dispPath = m_settings.outputDir + "/" + m_settings.baseFilename +
                        "_Disp." + std::to_string(res.udimTile) + ".exr";
                    std::string exrErr;
                    if (ExrWriter::SaveFloatImage(dispPath, res.displacementMap.data(), res.width, res.height, exrErr)) {
                        m_logEntries.push_back("[EXR] Saved 32-bit displacement: " + dispPath);
                    } else {
                        m_logEntries.push_back("[Error] Failed writing " + dispPath + ": " + exrErr);
                    }
                }

                // Save normal map EXR: <baseName>_Normal.<UDIM>.exr
                if (m_settings.bakeNormal) {
                    std::string normPath = m_settings.outputDir + "/" + m_settings.baseFilename +
                        "_Normal." + std::to_string(res.udimTile) + ".exr";
                    std::string exrErr;
                    if (ExrWriter::SaveRGBFloatImage(
                        normPath,
                        res.normalMapR.data(),
                        res.normalMapG.data(),
                        res.normalMapB.data(),
                        res.width,
                        res.height,
                        true, // 16-bit half float for normal
                        exrErr
                    )) {
                        m_logEntries.push_back("[EXR] Saved 16-bit normal: " + normPath);
                    } else {
                        m_logEntries.push_back("[Error] Failed writing " + normPath + ": " + exrErr);
                    }
                }

                // Save diagnostic map EXR: <baseName>_Diag.<UDIM>.exr
                if (m_settings.bakeDiagnosticMap) {
                    std::string diagPath = m_settings.outputDir + "/" + m_settings.baseFilename +
                        "_Diag." + std::to_string(res.udimTile) + ".exr";
                    std::string exrErr;
                    if (ExrWriter::SaveRGBFloatImage(
                        diagPath,
                        res.diagnosticMapR.data(),
                        res.diagnosticMapG.data(),
                        res.diagnosticMapB.data(),
                        res.width,
                        res.height,
                        true,
                        exrErr
                    )) {
                        m_logEntries.push_back("[EXR] Saved diagnostic map: " + diagPath);
                    }
                }
            }

            m_logEntries.push_back("[Bake] All UDIM tiles successfully baked and written!");
            m_bakeStatus = "Complete!";
            m_bakeProgress = 1.0f;
        } else {
            m_logEntries.push_back("[Error] Bake failed: " + err);
            m_bakeStatus = "Failed: " + err;
        }

        m_isBaking = false;
    }).detach();
}

} // namespace Baker
