#include "UIManager.h"
#include "MeshLoader.h"
#include "../baker/ExrWriter.h"

#include <imgui.h>
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <sstream>
#include <cmath>

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
    std::string resultPath;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool coInitialized = SUCCEEDED(hr);

    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        if (SUCCEEDED(pFileOpen->GetOptions(&dwOptions))) {
            pFileOpen->SetOptions(dwOptions | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        pFileOpen->SetTitle(L"Select Output Folder for Baked Maps");

        hr = pFileOpen->Show(owner);
        if (SUCCEEDED(hr)) {
            IShellItem* pItem = nullptr;
            hr = pFileOpen->GetResult(&pItem);
            if (SUCCEEDED(hr)) {
                PWSTR pszFilePath = nullptr;
                hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);
                if (SUCCEEDED(hr) && pszFilePath) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 0) {
                        resultPath.resize(len - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pszFilePath, -1, &resultPath[0], len, nullptr, nullptr);
                    }
                    CoTaskMemFree(pszFilePath);
                }
                pItem->Release();
            }
        }
        pFileOpen->Release();
    }

    if (coInitialized) {
        CoUninitialize();
    }
    return resultPath;
}

UIManager::UIManager() {
    m_settings.resolution = 2048;
    m_settings.cageDistance = 0.05f;
    m_settings.maxRayDistance = 0.2f;
    m_settings.seamPadding = 16;
    m_settings.discardBackfaceHits = true;
    m_settings.angleThreshold = 90.0f;
    m_settings.useBidirectionalRays = true;
    m_settings.isolateMeshParts = false;
    m_settings.bakeDisplacement = true;
    m_settings.bakeNormal = true;
    m_settings.outputDir = "./";
    m_settings.baseFilename = "Bake";

    strncpy_s(m_outDirBuf, m_settings.outputDir.c_str(), sizeof(m_outDirBuf) - 1);
    strncpy_s(m_baseNameBuf, m_settings.baseFilename.c_str(), sizeof(m_baseNameBuf) - 1);
}

UIManager::~UIManager() {
    m_engine.Release();
}

void UIManager::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_logEntries.push_back("[System] ZBrush UDIM Baker initialized.");
    if (device && context) {
        if (m_viewportRenderer.Initialize(device, context)) {
            m_logEntries.push_back("[Viewport] Interactive 3D Viewport initialized (Direct3D 11).");
        } else {
            m_logEntries.push_back("[Error] Failed to initialize 3D Viewport.");
        }
    }
}

void UIManager::CheckScaleAndAutoCalibrate() {
    if (!m_hasLowPoly || !m_hasHighPoly) return;

    float lpDiag = m_lowPolyMesh.GetBoundingBoxDiagonal();
    float hpDiag = m_highPolyMesh.GetBoundingBoxDiagonal();

    if (lpDiag > 1e-4f && hpDiag > 1e-4f) {
        m_detectedScaleRatio = hpDiag / lpDiag;

        // Auto-match scale ONLY if explicitly requested by user
        if (m_autoScaleLowPoly) {
            m_settings.lowPolyScale = m_detectedScaleRatio;
            m_settings.lowPolyScaleVec = { m_detectedScaleRatio, m_detectedScaleRatio, m_detectedScaleRatio };
        } else {
            m_settings.lowPolyScale = 1.0f;
            m_settings.lowPolyScaleVec = { 1.0f, 1.0f, 1.0f };
        }

        // Auto-calibrate cage offset and ray distance proportional to mesh bounding box
        m_settings.cageDistance = hpDiag * 0.015f;     // ~1.5% of diagonal
        m_settings.maxRayDistance = hpDiag * 0.08f;   // ~8.0% of diagonal

        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
        m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
        m_viewportRenderer.FrameAll();

        std::ostringstream oss;
        oss << "[Mesh Calibration] Low-Poly Scale: " << m_settings.lowPolyScale << "x"
            << ", Cage Offset: " << m_settings.cageDistance 
            << ", Max Ray: " << m_settings.maxRayDistance
            << " (Diag HP: " << hpDiag << ", LP: " << lpDiag << ")";
        m_logEntries.push_back(oss.str());
    }
}

bool UIManager::IsUDIMSelected(int tile) const {
    if (m_selectedUDIMs.empty()) return true;
    return std::find(m_selectedUDIMs.begin(), m_selectedUDIMs.end(), tile) != m_selectedUDIMs.end();
}

void UIManager::ToggleUDIM(int tile) {
    auto it = std::find(m_selectedUDIMs.begin(), m_selectedUDIMs.end(), tile);
    if (it != m_selectedUDIMs.end()) {
        m_selectedUDIMs.erase(it);
    } else {
        m_selectedUDIMs.push_back(tile);
    }
}

void UIManager::SelectAllUDIMs() {
    m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;
}

void UIManager::SelectOnlyUDIM(int tile) {
    m_selectedUDIMs.clear();
    m_selectedUDIMs.push_back(tile);
}

void UIManager::SetCommandLineHighPoly(const std::string& path) {
    if (!path.empty()) {
        m_highPolyPath = path;
        m_logEntries.push_back("[ZBrush] Ingested High-Poly path: " + path);
        std::string err;
        if (MeshLoader::LoadHighPolyMesh(path, m_highPolyMesh, err)) {
            m_hasHighPoly = true;
            m_logEntries.push_back("[Mesh] Successfully loaded High-Poly (" +
                std::to_string(m_highPolyMesh.positions.size()) + " verts, " +
                std::to_string(m_highPolyMesh.indices.size() / 3) + " tris).");

            m_viewportRenderer.SetHighPolyMesh(m_highPolyMesh);
            m_viewportRenderer.FrameAll();

            std::string bvhErr;
            if (m_engine.BuildHighPolyBVH(m_highPolyMesh, bvhErr)) {
                m_logEntries.push_back("[Embree] High-Poly BVH accelerated structure built.");
            } else {
                m_logEntries.push_back("[Error] Embree BVH build failed: " + bvhErr);
            }

            CheckScaleAndAutoCalibrate();
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
        if (MeshLoader::LoadLowPolyMesh(path, m_lowPolyMesh, err)) {
            m_hasLowPoly = true;
            ResetTransforms();
            m_logEntries.push_back("[Mesh] Loaded Low-Poly (" +
                std::to_string(m_lowPolyMesh.vertices.size()) + " verts, " +
                std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIMs).");

            // Select all detected tiles by default
            m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;

            // Auto-calibrate initial cage and ray distance from Low-Poly bounding box
            float lpDiag = m_lowPolyMesh.GetBoundingBoxDiagonal();
            if (lpDiag > 1e-4f) {
                m_settings.cageDistance = lpDiag * 0.015f;
                m_settings.maxRayDistance = lpDiag * 0.08f;
            }

            m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
            m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
            m_viewportRenderer.FrameAll();

            // Auto-configure output folder and base filename from low-poly file
            std::filesystem::path lp(path);
            if (lp.has_parent_path()) {
                m_settings.outputDir = lp.parent_path().string();
                strncpy_s(m_outDirBuf, m_settings.outputDir.c_str(), sizeof(m_outDirBuf) - 1);
            }
            if (lp.has_stem()) {
                m_settings.baseFilename = lp.stem().string();
                strncpy_s(m_baseNameBuf, m_settings.baseFilename.c_str(), sizeof(m_baseNameBuf) - 1);
            }

            CheckScaleAndAutoCalibrate();
        } else {
            m_logEntries.push_back("[Error] Failed loading Low-Poly: " + err);
        }
    }
}

void UIManager::AutoDetectZBrushCache(bool force) {
    std::string cacheLow = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    std::string cacheHigh = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj";

    if (force || !m_hasLowPoly) {
        DWORD attrLow = GetFileAttributesA(cacheLow.c_str());
        if (attrLow != INVALID_FILE_ATTRIBUTES && !(attrLow & FILE_ATTRIBUTE_DIRECTORY)) {
            SetCommandLineLowPoly(cacheLow);
        }
    }

    if (force || !m_hasHighPoly) {
        DWORD attrHigh = GetFileAttributesA(cacheHigh.c_str());
        if (attrHigh != INVALID_FILE_ATTRIBUTES && !(attrHigh & FILE_ATTRIBUTE_DIRECTORY)) {
            SetCommandLineHighPoly(cacheHigh);
        }
    }
}

void UIManager::RenderUI() {
    // 0. Synchronize shell state
    if (m_isBaking) {
        m_shellState.statusText = m_bakeStatus.c_str();
        m_shellState.statusColor = BakerUI::Col::Accent();
        m_shellState.bakeProgress = m_bakeProgress;
    } else if (m_bakeStatus.rfind("Failed", 0) == 0) {
        m_shellState.statusText = m_bakeStatus.c_str();
        m_shellState.statusColor = BakerUI::Col::Err();
        m_shellState.bakeProgress = -1.0f;
    } else {
        if (m_hasLowPoly && m_hasHighPoly) {
            m_shellState.statusText = "Ready to bake";
            m_shellState.statusColor = BakerUI::Col::Ok();
        } else {
            m_shellState.statusText = "Awaiting meshes...";
            m_shellState.statusColor = BakerUI::Col::Warn();
        }
        m_shellState.bakeProgress = -1.0f;
    }

    // Begin Host (Main Menu Bar, Top Toolbar, DockSpace, Status Bar)
    BakerUI::BeginFrameHost(m_shellState);

    // Handle menu triggers
    if (m_shellState.openLowPoly) {
        m_shellState.openLowPoly = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0All Files (*.*)\0*.*\0");
        if (!path.empty()) SetCommandLineLowPoly(path);
    }
    if (m_shellState.openHighPoly) {
        m_shellState.openHighPoly = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx;*.bin)\0*.obj;*.fbx;*.bin\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0Binary Mesh (*.bin)\0*.bin\0All Files (*.*)\0*.*\0");
        if (!path.empty()) SetCommandLineHighPoly(path);
    }
    if (m_shellState.openCage) {
        m_shellState.openCage = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0All Files (*.*)\0*.*\0");
        if (!path.empty()) {
            std::string err;
            if (MeshLoader::LoadCageMesh(path, m_lowPolyMesh, err)) {
                m_hasExplicitCage = true;
                m_cagePath = path;
                m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
                m_logEntries.push_back("[Cage] Loaded explicit cage: " + path);
            } else {
                m_logEntries.push_back("[Error] " + err);
            }
        }
    }
    if (m_shellState.exitApp) {
        PostQuitMessage(0);
    }

    // 0. Scene Outliner
    if (m_shellState.showOutliner) {
        RenderOutlinerPanel();
    }

    // 1. Center / Main Stage: Viewport (3D / 2D / Split)
    RenderViewportPanel();

    // 2. Left Column: Mesh Ingestion & Transforms
    RenderMeshPanel();

    // 3. Right Column: Bake Settings
    RenderSettingsPanel();

    // 4. Bottom Tab: UDIM Canvas
    if (m_shellState.showUdim) {
        RenderUDIMCanvasPanel();
    }

    // 5. Bottom Tab: Output Log
    if (m_shellState.showLog) {
        RenderBakeProgressPanel();
    }

    BakerUI::EndFrameHost();
}

void UIManager::RenderOutlinerPanel() {
    ImGui::Begin("Scene Outliner###outliner", &m_shellState.showOutliner);

    BakerUI::SectionLabel("SCENE HIERARCHY");

    // 1. High-Poly Sculpt Node
    bool hpOpen = ImGui::TreeNodeEx("##hp_node", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "High-Poly Sculpt");
    ImGui::SameLine();
    BakerUI::ToggleChip("View##hp_vis", &m_viewportRenderer.showHighPoly);

    if (hpOpen) {
        ImGui::Indent(18.0f);
        if (m_hasHighPoly) {
            std::string hpName = std::filesystem::path(m_highPolyPath).stem().string();
            if (hpName.empty()) hpName = "ZBrush_Sculpt";
            ImGui::TextDisabled("Source: %s", hpName.c_str());
            ImGui::Text("Verts: %zu | Tris: %zu", m_highPolyMesh.positions.size(), m_highPolyMesh.indices.size() / 3);

            // Polypaint badge
            if (!m_highPolyMesh.colors.empty()) {
                BakerUI::StatusPill("Polypaint: Yes", BakerUI::Col::Ok());
            } else {
                BakerUI::StatusPill("Polypaint: None", BakerUI::Col::TextDim());
            }
            ImGui::SameLine();
            // Polygroups badge
            if (!m_highPolyMesh.polygroupPerFace.empty()) {
                BakerUI::StatusPill("Polygroups: Yes", BakerUI::Col::Ok());
            } else {
                BakerUI::StatusPill("Polygroups: None", BakerUI::Col::TextDim());
            }

            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("Opacity##hp_op", &m_viewportRenderer.highPolyOpacity, 0.05f, 1.0f, "%.2f");
        } else {
            ImGui::TextDisabled("No High-Poly Mesh loaded.");
        }
        ImGui::Unindent(18.0f);
        ImGui::TreePop();
    }

    ImGui::Spacing();

    // 2. Low-Poly Target Node
    bool lpOpen = ImGui::TreeNodeEx("##lp_node", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "Low-Poly Target (UDIM)");
    ImGui::SameLine();
    BakerUI::ToggleChip("View##lp_vis", &m_viewportRenderer.showLowPoly);
    ImGui::SameLine();
    BakerUI::ToggleChip("Wire##lp_wire", &m_viewportRenderer.showLowPolyWireframe);

    if (lpOpen) {
        ImGui::Indent(18.0f);
        if (m_hasLowPoly) {
            std::string lpName = std::filesystem::path(m_lowPolyPath).stem().string();
            if (lpName.empty()) lpName = "LowPoly_UDIM";
            ImGui::TextDisabled("Source: %s", lpName.c_str());
            ImGui::Text("Verts: %zu | Tris: %zu", m_lowPolyMesh.vertices.size(), m_lowPolyMesh.triangles.size());
            ImGui::Text("UDIM Tiles: %zu", m_lowPolyMesh.detectedUDIMs.size());

            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("Opacity##lp_op", &m_viewportRenderer.lowPolyOpacity, 0.05f, 1.0f, "%.2f");
        } else {
            ImGui::TextDisabled("No Low-Poly Mesh loaded.");
        }
        ImGui::Unindent(18.0f);
        ImGui::TreePop();
    }

    ImGui::Spacing();

    // 3. Cage Shell Node
    bool cageOpen = ImGui::TreeNodeEx("##cage_node", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth, "Cage Shell");
    ImGui::SameLine();
    BakerUI::ToggleChip("View##cage_vis", &m_viewportRenderer.showCage);

    if (cageOpen) {
        ImGui::Indent(18.0f);
        if (m_hasExplicitCage) {
            ImGui::TextDisabled("Type: Explicit File");
        } else {
            ImGui::TextDisabled("Type: Vertex Normal Extrusion");
            ImGui::Text("Distance: %.4f", m_settings.cageDistance);
        }
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("Opacity##cage_op", &m_viewportRenderer.cageOpacity, 0.05f, 1.0f, "%.2f");
        ImGui::Unindent(18.0f);
        ImGui::TreePop();
    }

    ImGui::End();
}

void UIManager::Render2DUVCanvas(const ImVec2& origin, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Background fill
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(BakerUI::Col::Bg()));

    // Clip to canvas rectangle
    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Pan and Zoom interaction
    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = (io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + size.x &&
                      io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + size.y);

    if (isHovered) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
            (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            m_uvPan.x += io.MouseDelta.x;
            m_uvPan.y += io.MouseDelta.y;
        }

        if (io.MouseWheel != 0.0f) {
            float prevZoom = m_uvZoom;
            m_uvZoom *= std::pow(1.18f, io.MouseWheel);
            m_uvZoom = std::clamp(m_uvZoom, 0.10f, 30.0f);

            // Zoom toward mouse pointer
            float mx = io.MousePos.x - origin.x;
            float my = io.MousePos.y - origin.y;
            m_uvPan.x = mx - (mx - m_uvPan.x) * (m_uvZoom / prevZoom);
            m_uvPan.y = my - (my - m_uvPan.y) * (m_uvZoom / prevZoom);
        }
    }

    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float currentTileSize = baseTileSize * m_uvZoom;

    float initialOffsetX = (size.x - currentTileSize) * 0.5f;
    float initialOffsetY = (size.y - currentTileSize) * 0.5f;

    float originX = origin.x + initialOffsetX + m_uvPan.x;
    float originY = origin.y + initialOffsetY + m_uvPan.y;

    // Determine UDIM grid bounds
    int maxUdimTile = 1001;
    for (int t : m_lowPolyMesh.detectedUDIMs) {
        if (t > maxUdimTile) maxUdimTile = t;
    }
    int maxCols = 10;
    int maxRows = ((maxUdimTile - 1001) / 10) + 1;
    if (maxRows < 1) maxRows = 1;

    // Draw UDIM tile borders & subgrids
    for (int r = 0; r < maxRows; ++r) {
        for (int c = 0; c < maxCols; ++c) {
            int tileId = 1001 + c + r * 10;
            float tX0 = originX + c * currentTileSize;
            float tY0 = originY + (maxRows - 1 - r) * currentTileSize;
            float tX1 = tX0 + currentTileSize;
            float tY1 = tY0 + currentTileSize;

            if (tX1 < origin.x || tX0 > origin.x + size.x || tY1 < origin.y || tY0 > origin.y + size.y) {
                continue;
            }

            bool isDetected = false;
            for (int dt : m_lowPolyMesh.detectedUDIMs) {
                if (dt == tileId) { isDetected = true; break; }
            }

            bool isSelected = isDetected && IsUDIMSelected(tileId);

            // Handle tile click to toggle bake inclusion
            if (isDetected && isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (io.MousePos.x >= tX0 && io.MousePos.x <= tX1 && io.MousePos.y >= tY0 && io.MousePos.y <= tY1) {
                    ToggleUDIM(tileId);
                    isSelected = IsUDIMSelected(tileId);
                }
            }

            ImU32 tileBg = isDetected ? (isSelected ? ImGui::GetColorU32(BakerUI::Col::Panel()) : ImGui::GetColorU32(BakerUI::Col::Bg())) : ImGui::GetColorU32(BakerUI::Col::Bg());
            drawList->AddRectFilled(ImVec2(tX0, tY0), ImVec2(tX1, tY1), tileBg);
            ImU32 borderCol = isSelected ? ImGui::GetColorU32(BakerUI::Col::Accent()) : ImGui::GetColorU32(BakerUI::Col::Border());
            float borderThick = isSelected ? 2.0f : 1.0f;
            drawList->AddRect(ImVec2(tX0, tY0), ImVec2(tX1, tY1), borderCol, 0, 0, borderThick);

            // Subgrid lines
            if (currentTileSize > 140.0f) {
                ImU32 subLineCol = ImGui::GetColorU32(ImVec4(0.20f, 0.24f, 0.30f, 0.35f));
                for (int g = 1; g < 4; ++g) {
                    float fx = tX0 + (g / 4.0f) * currentTileSize;
                    float fy = tY0 + (g / 4.0f) * currentTileSize;
                    drawList->AddLine(ImVec2(fx, tY0), ImVec2(fx, tY1), subLineCol);
                    drawList->AddLine(ImVec2(tX0, fy), ImVec2(tX1, fy), subLineCol);
                }
            }

            char tileLabel[32];
            snprintf(tileLabel, sizeof(tileLabel), "%d", tileId);
            ImU32 labelCol = isSelected ? ImGui::GetColorU32(BakerUI::Col::Accent()) : ImGui::GetColorU32(BakerUI::Col::TextDim());
            drawList->AddText(ImVec2(tX0 + 8, tY0 + 6), labelCol, tileLabel);

            if (isDetected) {
                if (isSelected) {
                    drawList->AddText(ImVec2(tX0 + 8, tY0 + 24), ImGui::GetColorU32(BakerUI::Col::Ok()), "[✓ Bake]");
                } else {
                    drawList->AddText(ImVec2(tX0 + 8, tY0 + 24), ImGui::GetColorU32(BakerUI::Col::Warn()), "[Excluded]");
                }
            }
        }
    }

    // Draw Low-Poly UV Wireframe
    if (m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        ImU32 wireColor = ImGui::GetColorU32(ImVec4(0.92f, 0.94f, 0.98f, 0.65f));

        for (const auto& tri : m_lowPolyMesh.triangles) {
            const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
            const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
            const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];

            float p0x = originX + v0.uv.u * currentTileSize;
            float p0y = originY + (maxRows - v0.uv.v) * currentTileSize;

            float p1x = originX + v1.uv.u * currentTileSize;
            float p1y = originY + (maxRows - v1.uv.v) * currentTileSize;

            float p2x = originX + v2.uv.u * currentTileSize;
            float p2y = originY + (maxRows - v2.uv.v) * currentTileSize;

            float minX = (std::min)({ p0x, p1x, p2x });
            float maxX = (std::max)({ p0x, p1x, p2x });
            float minY = (std::min)({ p0y, p1y, p2y });
            float maxY = (std::max)({ p0y, p1y, p2y });

            if (maxX < origin.x || minX > origin.x + size.x || maxY < origin.y || minY > origin.y + size.y) {
                continue;
            }

            drawList->AddLine(ImVec2(p0x, p0y), ImVec2(p1x, p1y), wireColor);
            drawList->AddLine(ImVec2(p1x, p1y), ImVec2(p2x, p2y), wireColor);
            drawList->AddLine(ImVec2(p2x, p2y), ImVec2(p0x, p0y), wireColor);
        }
    }

    drawList->PopClipRect();

    // Bottom HUD info
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, origin.y + size.y - 32));
    ImGui::BeginGroup();
    char hudBuf[64];
    snprintf(hudBuf, sizeof(hudBuf), "Zoom: %.1fx | MMB/RMB: Pan | Scroll: Zoom", m_uvZoom);
    ImGui::TextDisabled("%s", hudBuf);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset UV View")) {
        m_uvZoom = 1.0f;
        m_uvPan = ImVec2(0.0f, 0.0f);
    }
    ImGui::EndGroup();
}

void UIManager::RenderViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport###viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x < 50 || avail.y < 50) {
        ImGui::End();
        return;
    }

    if (m_viewportMode == ViewportMode::View3D) {
        m_viewportRenderer.Resize((int)avail.x, (int)avail.y);
        m_viewportRenderer.Render();

        ID3D11ShaderResourceView* srv = m_viewportRenderer.GetViewportSRV();
        if (srv) {
            ImGui::Image((ImTextureID)srv, avail);

            if (ImGui::IsItemHovered()) {
                ImGuiIO& io = ImGui::GetIO();
                int button = 0;
                if (io.KeyAlt && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
                    button = 3;
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    button = 1;
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    button = 2;
                }

                if (button != 0 || io.MouseWheel != 0.0f) {
                    m_viewportRenderer.HandleMouseInput(io.MouseDelta.x, io.MouseDelta.y, button, io.MouseWheel);
                }

                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    m_viewportRenderer.FrameAll();
                }
            }
        }
    } else if (m_viewportMode == ViewportMode::View2D) {
        Render2DUVCanvas(origin, avail);
    } else if (m_viewportMode == ViewportMode::SplitBoth) {
        float halfW = std::floor(avail.x * 0.5f);
        m_viewportRenderer.Resize((int)halfW, (int)avail.y);
        m_viewportRenderer.Render();

        ID3D11ShaderResourceView* srv = m_viewportRenderer.GetViewportSRV();
        if (srv) {
            ImGui::Image((ImTextureID)srv, ImVec2(halfW, avail.y));

            if (ImGui::IsItemHovered()) {
                ImGuiIO& io = ImGui::GetIO();
                int button = 0;
                if (io.KeyAlt && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
                    button = 3;
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                    button = 1;
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                    button = 2;
                }

                if (button != 0 || io.MouseWheel != 0.0f) {
                    m_viewportRenderer.HandleMouseInput(io.MouseDelta.x, io.MouseDelta.y, button, io.MouseWheel);
                }

                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    m_viewportRenderer.FrameAll();
                }
            }
        }

        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(origin.x + halfW, origin.y),
            ImVec2(origin.x + halfW, origin.y + avail.y),
            ImGui::GetColorU32(BakerUI::Col::Border()),
            2.0f
        );

        ImVec2 uvOrigin(origin.x + halfW, origin.y);
        ImVec2 uvSize(avail.x - halfW, avail.y);
        Render2DUVCanvas(uvOrigin, uvSize);
    }

    // Floating overlay (top-left) - 3D controls
    if (m_viewportMode != ViewportMode::View2D) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, origin.y + 12));
        ImGui::BeginGroup();
        BakerUI::ToggleChip("High", &m_viewportRenderer.showHighPoly); ImGui::SameLine();
        BakerUI::ToggleChip("Low", &m_viewportRenderer.showLowPoly); ImGui::SameLine();
        BakerUI::ToggleChip("Wire", &m_viewportRenderer.showLowPolyWireframe); ImGui::SameLine();
        BakerUI::ToggleChip("Cage", &m_viewportRenderer.showCage); ImGui::SameLine();
        BakerUI::ToggleChip("Grid", &m_viewportRenderer.showGrid); ImGui::SameLine();

        ImGui::SetNextItemWidth(100);
        const char* shadingModes[] = { "Clay", "Wax Red", "Normals", "Flat" };
        int currentShading = (int)m_viewportRenderer.highPolyShading;
        if (ImGui::Combo("##Shading", &currentShading, shadingModes, IM_ARRAYSIZE(shadingModes))) {
            m_viewportRenderer.highPolyShading = (ShadingMode)currentShading;
        }
        ImGui::EndGroup();
    }

    // Floating mode toggles (top-right)
    float rightBtnGroupW = 380.0f;
    if (avail.x > rightBtnGroupW + 40.0f) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + avail.x - rightBtnGroupW - 12, origin.y + 12));
        ImGui::BeginGroup();

        bool is3D = (m_viewportMode == ViewportMode::View3D);
        bool is2D = (m_viewportMode == ViewportMode::View2D);
        bool isSplit = (m_viewportMode == ViewportMode::SplitBoth);

        if (BakerUI::ToggleChip("3D View", &is3D) && is3D) {
            m_viewportMode = ViewportMode::View3D;
        }
        ImGui::SameLine();
        if (BakerUI::ToggleChip("2D UV", &is2D) && is2D) {
            m_viewportMode = ViewportMode::View2D;
        }
        ImGui::SameLine();
        if (BakerUI::ToggleChip("Split 3D | 2D", &isSplit) && isSplit) {
            m_viewportMode = ViewportMode::SplitBoth;
        }
        ImGui::SameLine();

        if (BakerUI::PrimaryButton("Focus (F)", 78, 26)) {
            m_viewportRenderer.FrameAll();
            m_uvZoom = 1.0f;
            m_uvPan = ImVec2(0.0f, 0.0f);
        }

        ImGui::EndGroup();
    }

    ImGui::End();
}

void UIManager::SyncTransforms() {
    m_lowPolyMesh.translation = m_settings.lowPolyTranslation;
    m_lowPolyMesh.rotation = m_settings.lowPolyRotation;
    m_lowPolyMesh.scale = m_settings.lowPolyScaleVec;
    m_viewportRenderer.SetLowPolyTransform(
        DirectX::XMFLOAT3(m_settings.lowPolyTranslation.x, m_settings.lowPolyTranslation.y, m_settings.lowPolyTranslation.z),
        DirectX::XMFLOAT3(m_settings.lowPolyRotation.x, m_settings.lowPolyRotation.y, m_settings.lowPolyRotation.z),
        DirectX::XMFLOAT3(m_settings.lowPolyScaleVec.x, m_settings.lowPolyScaleVec.y, m_settings.lowPolyScaleVec.z)
    );
}

void UIManager::AutoAlignMeshCenters() {
    if (!m_hasLowPoly || !m_hasHighPoly) return;
    DirectX::XMFLOAT3 hpCenter = m_viewportRenderer.GetHighPolyCenter();
    DirectX::XMFLOAT3 lpCenter = m_viewportRenderer.GetLowPolyCenter();
    m_settings.lowPolyTranslation.x = hpCenter.x - lpCenter.x * m_settings.lowPolyScaleVec.x;
    m_settings.lowPolyTranslation.y = hpCenter.y - lpCenter.y * m_settings.lowPolyScaleVec.y;
    m_settings.lowPolyTranslation.z = hpCenter.z - lpCenter.z * m_settings.lowPolyScaleVec.z;
    SyncTransforms();
    m_logEntries.push_back("[Align] Auto-aligned Low-Poly center to High-Poly center.");
}

void UIManager::AutoFitMeshScale() {
    if (!m_hasLowPoly || !m_hasHighPoly) return;
    float lpDiag = m_lowPolyMesh.GetBoundingBoxDiagonal();
    float hpDiag = m_highPolyMesh.GetBoundingBoxDiagonal();
    if (lpDiag > 1e-4f) {
        float s = hpDiag / lpDiag;
        m_settings.lowPolyScaleVec = { s, s, s };
        m_settings.lowPolyScale = s;
        SyncTransforms();
        m_logEntries.push_back("[Scale] Auto-fit Low-Poly scale to High-Poly bounding diagonal (" + std::to_string(s) + "x).");
    }
}

void UIManager::ResetTransforms() {
    m_settings.lowPolyTranslation = { 0.0f, 0.0f, 0.0f };
    m_settings.lowPolyRotation = { 0.0f, 0.0f, 0.0f };
    m_settings.lowPolyScaleVec = { 1.0f, 1.0f, 1.0f };
    m_settings.lowPolyScale = 1.0f;
    SyncTransforms();
    m_logEntries.push_back("[Transform] Low-Poly transform reset to default.");
}

void UIManager::RenderMeshPanel() {
    ImGui::Begin("Mesh Ingestion###ingest", nullptr);

    BakerUI::SectionLabel("HIGH-POLY SOURCE");
    bool openHP = BakerUI::BeginCard("##hp");
    if (openHP) {
        if (BakerUI::PrimaryButton("Sync from ZBrush Cache")) {
            AutoDetectZBrushCache(true);
        }
        if (BakerUI::SecondaryButton("Browse OBJ / FBX / BIN...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx;*.bin)\0*.obj;*.fbx;*.bin\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0Binary Mesh (*.bin)\0*.bin\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetCommandLineHighPoly(path);
            }
        }
        if (m_hasHighPoly) {
            ImGui::Spacing();
            if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
            ImGui::TextDisabled("%s", m_highPolyPath.c_str());
            ImGui::Text("Positions %zu", m_highPolyMesh.positions.size());
            ImGui::Text("Triangles %zu", m_highPolyMesh.indices.size() / 3);
            ImGui::Text("Diag      %.4f", m_highPolyMesh.GetBoundingBoxDiagonal());
            if (BakerUI::gFontMono) ImGui::PopFont();
            ImGui::Spacing();
            BakerUI::StatusPill("BVH ready (Embree 3)", BakerUI::Col::Ok());
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No high-poly loaded", BakerUI::Col::TextDim());
        }
    }
    BakerUI::EndCard();

    BakerUI::SectionLabel("LOW-POLY TARGET (UDIM)");
    bool openLP = BakerUI::BeginCard("##lp");
    if (openLP) {
        if (BakerUI::SecondaryButton("Browse Low-Poly OBJ / FBX...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetCommandLineLowPoly(path);
            }
        }
        if (m_hasLowPoly) {
            ImGui::Spacing();
            if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
            ImGui::TextDisabled("%s", m_lowPolyPath.c_str());
            ImGui::Text("Vertices  %zu", m_lowPolyMesh.vertices.size());
            ImGui::Text("Triangles %zu", m_lowPolyMesh.triangles.size());
            ImGui::Text("Diag      %.4f", m_lowPolyMesh.GetBoundingBoxDiagonal());
            ImGui::Text("UDIMs     %zu tiles", m_lowPolyMesh.detectedUDIMs.size());
            if (BakerUI::gFontMono) ImGui::PopFont();
            ImGui::Spacing();
            BakerUI::StatusPill("Low-poly loaded", BakerUI::Col::Ok());
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No low-poly loaded", BakerUI::Col::Warn());
        }
    }
    BakerUI::EndCard();

    BakerUI::SectionLabel("EXPLICIT CAGE (OPTIONAL)");
    bool openCage = BakerUI::BeginCard("##cage");
    if (openCage) {
        if (BakerUI::SecondaryButton("Browse Cage OBJ / FBX...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                std::string err;
                if (MeshLoader::LoadCageMesh(path, m_lowPolyMesh, err)) {
                    m_hasExplicitCage = true;
                    m_cagePath = path;
                    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
                    m_logEntries.push_back("[Cage] Loaded explicit cage: " + path);
                } else {
                    m_logEntries.push_back("[Error] " + err);
                }
            }
        }
        ImGui::Spacing();
        if (m_hasExplicitCage) {
            BakerUI::StatusPill("Explicit Cage Active", BakerUI::Col::Ok());
        } else {
            BakerUI::StatusPill("Auto normal extrusion cage", BakerUI::Col::TextDim());
        }
    }
    BakerUI::EndCard();

    if (m_hasLowPoly) {
        BakerUI::SectionLabel("TRANSFORM & ALIGNMENT");
        bool openTrans = BakerUI::BeginCard("##transform");
        if (openTrans) {
            bool changed = false;

            ImGui::TextDisabled("Position (XYZ):");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat3("##pos", &m_settings.lowPolyTranslation.x, 0.01f, -10000.0f, 10000.0f, "%.3f")) {
                changed = true;
            }

            ImGui::TextDisabled("Rotation (XYZ deg):");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat3("##rot", &m_settings.lowPolyRotation.x, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
                changed = true;
            }

            ImGui::TextDisabled("Scale (XYZ):");
            ImGui::Checkbox("Uniform Scale", &m_uniformScale);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (m_uniformScale) {
                float s = m_settings.lowPolyScaleVec.x;
                if (ImGui::DragFloat("##scale_u", &s, 0.005f, 0.0001f, 10000.0f, "%.4f")) {
                    m_settings.lowPolyScaleVec = { s, s, s };
                    m_settings.lowPolyScale = s;
                    changed = true;
                }
            } else {
                if (ImGui::DragFloat3("##scale_xyz", &m_settings.lowPolyScaleVec.x, 0.005f, 0.0001f, 10000.0f, "%.4f")) {
                    changed = true;
                }
            }

            if (changed) {
                SyncTransforms();
            }

            ImGui::Spacing();
            if (m_hasHighPoly) {
                if (BakerUI::PrimaryButton("Auto-Align Centers")) {
                    AutoAlignMeshCenters();
                }
                if (BakerUI::SecondaryButton("Auto-Fit Scale")) {
                    AutoFitMeshScale();
                }
            }

            if (BakerUI::SecondaryButton("Reset Transform")) {
                ResetTransforms();
            }
        }
        BakerUI::EndCard();
    }

    if (m_hasLowPoly && m_hasHighPoly) {
        BakerUI::SectionLabel("SCALE SYNCHRONIZATION");
        bool openScale = BakerUI::BeginCard("##scale");
        if (openScale) {
            ImGui::Text("Scale Ratio: %.4fx", m_detectedScaleRatio);
            if (ImGui::Checkbox("Auto-Match Low-Poly Scale", &m_autoScaleLowPoly)) {
                CheckScaleAndAutoCalibrate();
            }
            if (BakerUI::SecondaryButton("Recalibrate Scale & Bounds")) {
                CheckScaleAndAutoCalibrate();
            }
        }
        BakerUI::EndCard();
    }

    ImGui::End();
}

void UIManager::RenderSettingsPanel() {
    ImGui::Begin("Bake Parameters###params", nullptr);

    static const char* kRes[] = { "1024", "2048", "4096", "8192" };
    static int currentResIdx = 1; // 2048

    static const char* kAA[] = { "1x (Draft)", "2x2 (4 Samples - Prod)", "4x4 (16 Samples - Max)" };
    static int currentAAIdx = 1; // 2x2

    float maxSliderDist = (m_hasHighPoly ? m_highPolyMesh.GetBoundingBoxDiagonal() * 0.5f : 10.0f);
    if (maxSliderDist < 1.0f) maxSliderDist = 1.0f;

    BakerUI::SectionLabel("PROJECTION");
    if (BakerUI::BeginProps("##proj")) {
        if (BakerUI::PropCombo("Resolution", &currentResIdx, kRes, 4)) {
            int resMap[] = { 1024, 2048, 4096, 8192 };
            m_settings.resolution = resMap[currentResIdx];
        }
        if (BakerUI::PropCombo("Antialiasing", &currentAAIdx, kAA, 3)) {
            int aaMap[] = { 1, 2, 4 };
            m_settings.antiAliasing = aaMap[currentAAIdx];
        }
        BakerUI::PropFloat("Skew factor", &m_settings.skewFactor, 0.01f, 0.0f, 1.0f, "%.2f", "Marmoset Skew: 0=Smooth Normals, 1=Flat Face Normals");
        if (!m_hasExplicitCage) {
            if (BakerUI::PropFloat("Cage offset", &m_settings.cageDistance, 0.002f, 0.0001f, maxSliderDist, "%.4f", "Push distance along vertex normal")) {
                m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
            }
        }
        BakerUI::PropFloat("Max ray length", &m_settings.maxRayDistance, 0.005f, 0.001f, maxSliderDist, "%.4f");
        BakerUI::PropInt("Padding (px)", &m_settings.seamPadding, 0, 64, "JFA nearest-texel dilation");
        BakerUI::PropCheck("Discard Backface", &m_settings.discardBackfaceHits, "Prevents inverted border artifacts");
        if (m_settings.discardBackfaceHits) {
            BakerUI::PropFloat("Angle Threshold", &m_settings.angleThreshold, 0.5f, 30.0f, 90.0f, "%.1f deg");
        }
        BakerUI::PropCheck("Bidirectional Rays", &m_settings.useBidirectionalRays, "Marmoset/xNormal dual-direction rays to catch crevices and protrusions");
        BakerUI::PropCheck("Isolate Mesh Parts", &m_settings.isolateMeshParts, "Prevent ray bleeding between adjacent stones/subtools");
        BakerUI::EndProps();
    }

    if (m_hasLowPoly && !m_lowPolyMesh.detectedUDIMs.empty()) {
        std::string udimSummary;
        for (size_t i = 0; i < m_selectedUDIMs.size(); ++i) {
            if (i > 0) udimSummary += ", ";
            udimSummary += std::to_string(m_selectedUDIMs[i]);
        }
        if (udimSummary.empty()) udimSummary = "None (0 selected)";
        ImGui::TextDisabled("Target UDIMs: %s (%zu/%zu)", udimSummary.c_str(), m_selectedUDIMs.size(), m_lowPolyMesh.detectedUDIMs.size());
        ImGui::SameLine();
        if (ImGui::SmallButton("UDIM Mgr")) {
            m_shellState.showUdim = true;
        }
        ImGui::Spacing();
    }

    BakerUI::SectionLabel("OUTPUT MAPS (MARMOSET SUITE)");
    if (BakerUI::BeginProps("##maps")) {
        BakerUI::PropCheck("Tangent Normal (MikkT)", &m_settings.bakeNormal, "World-standard tangent space normal map");
        BakerUI::PropCheck("World / Object Normal", &m_settings.bakeWorldNormal, "Raw XYZ world surface vectors");
        BakerUI::PropCheck("Displacement / Height (32f)", &m_settings.bakeDisplacement, "Vector projection height difference");
        BakerUI::PropCheck("Ambient Occlusion (AO)", &m_settings.bakeAmbientOcclusion, "Cosine-weighted diffuse contact shadows");
        if (m_settings.bakeAmbientOcclusion) {
            BakerUI::PropInt("AO Samples", &m_settings.aoSamples, 4, 64);
            BakerUI::PropFloat("AO Radius", &m_settings.aoMaxDistance, 0.005f, 0.001f, maxSliderDist, "%.4f");
        }
        BakerUI::PropCheck("Cavity Map", &m_settings.bakeCavity, "High-frequency micro-crevices, pores, wrinkles");
        BakerUI::PropCheck("Curvature Map", &m_settings.bakeCurvature, "Convex ridges (white) & concave crevices (black)");
        if (m_settings.bakeCavity || m_settings.bakeCurvature) {
            BakerUI::PropFloat("Cavity Contrast", &m_settings.cavityStrength, 0.1f, 0.1f, 10.0f, "%.1fx");
        }
        BakerUI::PropCheck("Thickness / SSS Map", &m_settings.bakeThickness, "Subsurface scattering inward ray thickness");
        if (m_settings.bakeThickness) {
            BakerUI::PropFloat("Thickness Depth", &m_settings.thicknessMaxDistance, 0.005f, 0.001f, maxSliderDist, "%.4f");
        }
        BakerUI::PropCheck("World Position Map", &m_settings.bakePosition, "World XYZ coordinate gradient map (32f EXR)");
        BakerUI::PropCheck("Material ID (Polygroups)", &m_settings.bakeMaterialID, "Distinct color per ZBrush polygroup ID");
        BakerUI::PropCheck("Base Color (Polypaint)", &m_settings.bakeBaseColor, "ZBrush vertex colors baked directly to UDIM texture");
        BakerUI::PropCheck("Diagnostic map", &m_settings.bakeDiagnosticMap, "Missed rays and ray coverage analysis");
        BakerUI::PropCheck("Flip Normal Y (DX/GL)", &m_settings.flipYNormal);
        BakerUI::PropCheck("Compact BVH", &m_settings.useCompactBVH, "For dense 50M-100M poly sculpts");
        BakerUI::EndProps();
    }

    BakerUI::SectionLabel("DESTINATION & FORMATS");
    bool openDest = BakerUI::BeginCard("##dest_card");
    if (openDest) {
        ImGui::TextDisabled("Output Folder:");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
        ImGui::InputText("##dir", m_outDirBuf, sizeof(m_outDirBuf));
        m_settings.outputDir = m_outDirBuf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(80, 0))) {
            std::string folder = ChooseFolderDialog();
            if (!folder.empty()) {
                m_settings.outputDir = folder;
                strncpy_s(m_outDirBuf, folder.c_str(), sizeof(m_outDirBuf) - 1);
            }
        }

        ImGui::Spacing();
        ImGui::TextDisabled("Base Filename:");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputText("##basename", m_baseNameBuf, sizeof(m_baseNameBuf));
        m_settings.baseFilename = m_baseNameBuf;

        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Open Output Folder in Explorer")) {
            ShellExecuteA(NULL, "open", m_settings.outputDir.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }

        ImGui::Spacing();
        if (BakerUI::BeginProps("##fmt_props")) {
            BakerUI::PropCheck("Export 8-Bit PNG", &m_settings.exportPNG, "For instant preview & game engines");
            BakerUI::PropCheck("Export Float EXR", &m_settings.exportEXR, "16-bit half / 32-bit float for production renderers");
            BakerUI::EndProps();
        }
    }
    BakerUI::EndCard();

    // Pin BakeButton at the bottom of the panel
    float btnH = 44.f * (ImGui::GetFontSize() / 15.f) + ImGui::GetStyle().WindowPadding.y;
    float remain = ImGui::GetContentRegionAvail().y - btnH;
    if (remain > 0) ImGui::Dummy(ImVec2(0, remain));

    bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking;
    if (!canBake && !m_isBaking) {
        ImGui::BeginDisabled();
    }

    float bakeProg = m_isBaking ? m_bakeProgress : -1.0f;
    if (BakerUI::BakeButton(bakeProg, "START HARDWARE-ACCELERATED BAKE")) {
        if (canBake) {
            TriggerBake();
        }
    }

    if (!canBake && !m_isBaking) {
        ImGui::EndDisabled();
    }

    ImGui::End();
}

void UIManager::RenderUDIMCanvasPanel() {
    ImGui::Begin("UDIM Layout###udim", &m_shellState.showUdim);

    if (!m_hasLowPoly || m_lowPolyMesh.detectedUDIMs.empty()) {
        ImGui::TextDisabled("Load a low-poly mesh to inspect and manage UDIM tiles.");
        ImGui::End();
        return;
    }

    BakerUI::SectionLabel("TILE SELECTION FOR BAKE");
    ImGui::Text("Detected UDIM Tiles (%zu total):", m_lowPolyMesh.detectedUDIMs.size());

    ImGui::Spacing();
    if (ImGui::Button("Select All Tiles")) {
        SelectAllUDIMs();
    }
    ImGui::SameLine();
    if (ImGui::Button("Select Only 1001")) {
        SelectOnlyUDIM(1001);
    }
    ImGui::SameLine();
    if (ImGui::Button("Deselect All")) {
        m_selectedUDIMs.clear();
    }

    ImGui::Spacing();
    bool openCards = BakerUI::BeginCard("##udim_cards");
    if (openCards) {
        for (int tile : m_lowPolyMesh.detectedUDIMs) {
            bool sel = IsUDIMSelected(tile);
            char chkLabel[64];
            snprintf(chkLabel, sizeof(chkLabel), "Bake Tile %d##chk_%d", tile, tile);
            if (ImGui::Checkbox(chkLabel, &sel)) {
                ToggleUDIM(tile);
            }
            ImGui::SameLine();
            if (sel) {
                BakerUI::StatusPill("ACTIVE", BakerUI::Col::Ok());
            } else {
                BakerUI::StatusPill("SKIPPED", BakerUI::Col::Warn());
            }
        }
    }
    BakerUI::EndCard();

    ImGui::Spacing();
    BakerUI::SectionLabel("UDIM GRID PREVIEW");

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

        bool isSel = IsUDIMSelected(tile);
        ImU32 bgCol = isSel ? ImGui::GetColorU32(BakerUI::Col::Panel()) : ImGui::GetColorU32(BakerUI::Col::Bg());
        ImU32 borderCol = isSel ? ImGui::GetColorU32(BakerUI::Col::Accent()) : ImGui::GetColorU32(BakerUI::Col::Border());

        drawList->AddRectFilled(p0, p1, bgCol, 6.0f);
        drawList->AddRect(p0, p1, borderCol, 6.0f, 0, isSel ? 2.0f : 1.0f);

        char tileLabel[16];
        snprintf(tileLabel, sizeof(tileLabel), "%d", tile);
        ImVec2 textSz = ImGui::CalcTextSize(tileLabel);
        ImVec2 textPos(p0.x + (tileSize - textSz.x) * 0.5f, p0.y + (tileSize - textSz.y) * 0.5f - 8);
        drawList->AddText(textPos, isSel ? ImGui::GetColorU32(BakerUI::Col::AccentHi()) : ImGui::GetColorU32(BakerUI::Col::TextDim()), tileLabel);

        const char* statusTxt = isSel ? "BAKE" : "SKIP";
        ImVec2 sSz = ImGui::CalcTextSize(statusTxt);
        ImVec2 sPos(p0.x + (tileSize - sSz.x) * 0.5f, p0.y + (tileSize - sSz.y) * 0.5f + 10);
        drawList->AddText(sPos, isSel ? ImGui::GetColorU32(BakerUI::Col::Ok()) : ImGui::GetColorU32(BakerUI::Col::TextDim()), statusTxt);
    }

    ImGui::Dummy(ImVec2(10 * (tileSize + tilePadding), 4 * (tileSize + tilePadding)));
    ImGui::End();
}

void UIManager::RenderBakeProgressPanel() {
    ImGui::Begin("Output Log###log", &m_shellState.showLog);

    if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
    for (const auto& log : m_logEntries) {
        if (log.rfind("[Error]", 0) == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Col::Err());
            ImGui::TextUnformatted(log.c_str());
            ImGui::PopStyleColor();
        } else if (log.rfind("[Bake]", 0) == 0 || log.rfind("[EXR]", 0) == 0 || log.rfind("[PNG]", 0) == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Col::AccentHi());
            ImGui::TextUnformatted(log.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(log.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    if (BakerUI::gFontMono) ImGui::PopFont();

    ImGui::End();
}

void UIManager::TriggerBake() {
    m_isBaking = true;
    m_bakeProgress = 0.0f;
    m_bakeStatus = "Starting bake...";
    m_settings.targetUDIMs = m_selectedUDIMs;

    size_t targetCount = m_selectedUDIMs.empty() ? m_lowPolyMesh.detectedUDIMs.size() : m_selectedUDIMs.size();
    m_logEntries.push_back("[Bake] Initiating bake session across " +
        std::to_string(targetCount) + " selected UDIM tile(s)...");

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
            oss << "[Bake] Ray-tracing finished in " << elapsedSec << "s. Writing output files...";
            m_logEntries.push_back(oss.str());

            // Prepare safe output folder
            std::filesystem::path outFolder(m_settings.outputDir);
            if (m_settings.outputDir.empty() || m_settings.outputDir == "./" || m_settings.outputDir == ".\\") {
                outFolder = std::filesystem::current_path();
            }
            std::error_code ec;
            std::filesystem::create_directories(outFolder, ec);

            for (const auto& res : results) {
                std::string udimStr = std::to_string(res.udimTile);

                // 1. EXR Formats (16-bit half / 32-bit float)
                if (m_settings.exportEXR) {
                    if (m_settings.bakeDisplacement && !res.displacementMap.empty()) {
                        std::string dispPath = (outFolder / (m_settings.baseFilename + "_Disp." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(dispPath, res.displacementMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 32-bit displacement: " + dispPath);
                        } else {
                            m_logEntries.push_back("[Error] Failed writing " + dispPath + ": " + exrErr);
                        }
                    }

                    if (m_settings.bakeNormal && !res.normalMapR.empty()) {
                        std::string normPath = (outFolder / (m_settings.baseFilename + "_Normal." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(normPath, res.normalMapR.data(), res.normalMapG.data(), res.normalMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit normal: " + normPath);
                        }
                    }

                    if (m_settings.bakeWorldNormal && !res.worldNormalR.empty()) {
                        std::string wnormPath = (outFolder / (m_settings.baseFilename + "_WorldNormal." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(wnormPath, res.worldNormalR.data(), res.worldNormalG.data(), res.worldNormalB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit world normal: " + wnormPath);
                        }
                    }

                    if (m_settings.bakeAmbientOcclusion && !res.aoMap.empty()) {
                        std::string aoPath = (outFolder / (m_settings.baseFilename + "_AO." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(aoPath, res.aoMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit AO: " + aoPath);
                        }
                    }

                    if (m_settings.bakeCavity && !res.cavityMap.empty()) {
                        std::string cavPath = (outFolder / (m_settings.baseFilename + "_Cavity." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(cavPath, res.cavityMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit cavity: " + cavPath);
                        }
                    }

                    if (m_settings.bakeCurvature && !res.curvatureMap.empty()) {
                        std::string curvPath = (outFolder / (m_settings.baseFilename + "_Curvature." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(curvPath, res.curvatureMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit curvature: " + curvPath);
                        }
                    }

                    if (m_settings.bakeThickness && !res.thicknessMap.empty()) {
                        std::string thickPath = (outFolder / (m_settings.baseFilename + "_Thickness." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(thickPath, res.thicknessMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit thickness: " + thickPath);
                        }
                    }

                    if (m_settings.bakePosition && !res.positionMapR.empty()) {
                        std::string posPath = (outFolder / (m_settings.baseFilename + "_Position." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(posPath, res.positionMapR.data(), res.positionMapG.data(), res.positionMapB.data(), res.width, res.height, false, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 32-bit world position: " + posPath);
                        }
                    }

                    if (m_settings.bakeMaterialID && !res.materialIdMapR.empty()) {
                        std::string matPath = (outFolder / (m_settings.baseFilename + "_MaterialID." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(matPath, res.materialIdMapR.data(), res.materialIdMapG.data(), res.materialIdMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit material ID: " + matPath);
                        }
                    }

                    if (m_settings.bakeBaseColor && !res.baseColorMapR.empty()) {
                        std::string colPath = (outFolder / (m_settings.baseFilename + "_BaseColor." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(colPath, res.baseColorMapR.data(), res.baseColorMapG.data(), res.baseColorMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit base color: " + colPath);
                        }
                    }

                    if (m_settings.bakeDiagnosticMap && !res.diagnosticMapR.empty()) {
                        std::string diagPath = (outFolder / (m_settings.baseFilename + "_Diag." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(diagPath, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved diagnostic map: " + diagPath);
                        }
                    }
                }

                // 2. PNG Formats (8-bit per channel - Instant Preview & Game Engines)
                if (m_settings.exportPNG) {
                    if (m_settings.bakeNormal && !res.normalMapR.empty()) {
                        std::string normPngPath = (outFolder / (m_settings.baseFilename + "_Normal." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(normPngPath, res.normalMapR.data(), res.normalMapG.data(), res.normalMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit normal: " + normPngPath);
                        }
                    }

                    if (m_settings.bakeWorldNormal && !res.worldNormalR.empty()) {
                        std::string wnormPngPath = (outFolder / (m_settings.baseFilename + "_WorldNormal." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(wnormPngPath, res.worldNormalR.data(), res.worldNormalG.data(), res.worldNormalB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit world normal: " + wnormPngPath);
                        }
                    }

                    if (m_settings.bakeAmbientOcclusion && !res.aoMap.empty()) {
                        std::string aoPngPath = (outFolder / (m_settings.baseFilename + "_AO." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(aoPngPath, res.aoMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit AO map: " + aoPngPath);
                        }
                    }

                    if (m_settings.bakeCavity && !res.cavityMap.empty()) {
                        std::string cavPngPath = (outFolder / (m_settings.baseFilename + "_Cavity." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(cavPngPath, res.cavityMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Cavity map: " + cavPngPath);
                        }
                    }

                    if (m_settings.bakeCurvature && !res.curvatureMap.empty()) {
                        std::string curvPngPath = (outFolder / (m_settings.baseFilename + "_Curvature." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(curvPngPath, res.curvatureMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Curvature map: " + curvPngPath);
                        }
                    }

                    if (m_settings.bakeThickness && !res.thicknessMap.empty()) {
                        std::string thickPngPath = (outFolder / (m_settings.baseFilename + "_Thickness." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(thickPngPath, res.thicknessMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit thickness map: " + thickPngPath);
                        }
                    }

                    if (m_settings.bakeMaterialID && !res.materialIdMapR.empty()) {
                        std::string matPngPath = (outFolder / (m_settings.baseFilename + "_MaterialID." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(matPngPath, res.materialIdMapR.data(), res.materialIdMapG.data(), res.materialIdMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Material ID: " + matPngPath);
                        }
                    }

                    if (m_settings.bakeBaseColor && !res.baseColorMapR.empty()) {
                        std::string colPngPath = (outFolder / (m_settings.baseFilename + "_BaseColor." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(colPngPath, res.baseColorMapR.data(), res.baseColorMapG.data(), res.baseColorMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Base Color: " + colPngPath);
                        }
                    }

                    if (m_settings.bakeDiagnosticMap && !res.diagnosticMapR.empty()) {
                        std::string diagPngPath = (outFolder / (m_settings.baseFilename + "_Diag." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(diagPngPath, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved diagnostic map: " + diagPngPath);
                        }
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
