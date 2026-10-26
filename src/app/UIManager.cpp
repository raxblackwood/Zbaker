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
#include <fstream>
#include <cmath>

namespace Baker {

namespace {
inline std::pair<uint32_t, uint32_t> MakeEdgeKey(uint32_t a, uint32_t b) {
    return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
}

inline float DistToSegment2D(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float lenSq = dx * dx + dy * dy;
    if (lenSq < 1e-4f) {
        float dpx = p.x - a.x;
        float dpy = p.y - a.y;
        return std::sqrt(dpx * dpx + dpy * dpy);
    }
    float t = std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / lenSq, 0.0f, 1.0f);
    float qx = a.x + t * dx;
    float qy = a.y + t * dy;
    float rx = p.x - qx;
    float ry = p.y - qy;
    return std::sqrt(rx * rx + ry * ry);
}
} // namespace

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
    m_settings.seamPadding = 32;
    m_settings.discardBackfaceHits = true;
    m_settings.angleThreshold = 75.0f;
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
            std::string err = m_viewportRenderer.GetLastError();
            if (!err.empty()) {
                m_logEntries.push_back("[Error] Failed to initialize 3D Viewport: " + err);
            } else {
                m_logEntries.push_back("[Error] Failed to initialize 3D Viewport.");
            }
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
    if (path.empty()) return;
    if (m_isLoadingHighPoly) {
        m_logEntries.push_back("[Warning] A high-poly mesh is already being loaded in background.");
        return;
    }

    m_highPolyPath = path;
    m_isLoadingHighPoly = true;
    m_loadingStatusText = "Ingesting High-Poly Mesh...";
    m_logEntries.push_back("[ZBrush] Ingesting High-Poly in background thread: " + path);

    std::thread([this, path]() {
        std::string err;
        HighPolyMesh tempMesh;
        bool ok = MeshLoader::LoadHighPolyMesh(path, tempMesh, err);

        std::string bvhErr;
        bool bvhOk = false;
        if (ok) {
            bvhOk = m_engine.BuildHighPolyBVH(tempMesh, bvhErr);
        }

        {
            std::lock_guard<std::mutex> lock(m_meshMutex);
            if (ok) {
                m_highPolyMesh = std::move(tempMesh);
                m_hasHighPoly = true;
                m_pendingMeshUpload = true;
                m_logEntries.push_back("[Mesh] Successfully loaded High-Poly (" +
                    std::to_string(m_highPolyMesh.positions.size()) + " verts, " +
                    std::to_string(m_highPolyMesh.indices.size() / 3) + " tris).");
                if (bvhOk) {
                    m_logEntries.push_back("[Embree] High-Poly BVH accelerated structure built.");
                } else {
                    m_logEntries.push_back("[Error] Embree BVH build failed: " + bvhErr);
                }
            } else {
                m_logEntries.push_back("[Error] Failed loading High-Poly: " + err);
            }
            m_isLoadingHighPoly = false;
        }
    }).detach();
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

            m_history.Clear();
            PushHistoryState("Load Low-Poly Mesh");
        } else {
            m_logEntries.push_back("[Error] Failed loading Low-Poly: " + err);
        }
    }
}

void UIManager::ClearLowPoly() {
    m_lowPolyMesh.Clear();
    m_hasLowPoly = false;
    m_lowPolyPath.clear();
    m_selectedUDIMs.clear();
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, 1.0f);
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, 0.0f, 1.0f);
    m_logEntries.push_back("[Scene] Low-Poly mesh cleared.");
}

void UIManager::ClearHighPoly() {
    std::lock_guard<std::mutex> lock(m_meshMutex);
    m_highPolyMesh.Clear();
    m_hasHighPoly = false;
    m_highPolyPath.clear();
    m_pendingMeshUpload = false;
    m_viewportRenderer.SetHighPolyMesh(m_highPolyMesh);
    m_logEntries.push_back("[Scene] High-Poly mesh cleared.");
}

void UIManager::ClearCage() {
    m_hasExplicitCage = false;
    m_cagePath.clear();
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
    m_logEntries.push_back("[Scene] Explicit cage cleared. Using procedural extrusion.");
}

void UIManager::ClearScene() {
    ClearLowPoly();
    ClearHighPoly();
    ClearCage();
    m_viewportRenderer.FrameAll();
    m_logEntries.push_back("[Scene] Scene completely cleared.");
}

void UIManager::AutoDetectZBrushCache(bool force) {
    std::string cacheLow = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
    std::string cacheHigh = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj";

    // Also check GoZ Default project folder for latest .GoZ export
    std::string gozDir = "C:/Users/Public/Pixologic/GoZProjects/Default";
    std::string latestGoz = "";
    if (std::filesystem::exists(gozDir)) {
        std::filesystem::file_time_type latestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(gozDir)) {
            if (entry.is_regular_file()) {
                std::string ext = entry.path().extension().string();
                for (char& c : ext) c = (char)std::tolower(c);
                if (ext == ".goz") {
                    auto wt = entry.last_write_time();
                    if (wt > latestTime) {
                        latestTime = wt;
                        latestGoz = entry.path().string();
                    }
                }
            }
        }
    }

    if (!latestGoz.empty()) {
        cacheHigh = latestGoz;
    }

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

void UIManager::CheckZBrushLiveSync() {
    static int s_checkCounter = 0;
    if (++s_checkCounter % 15 != 0) return;

    std::string triggerPath = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/sync_trigger.txt";
    DWORD attr = GetFileAttributesA(triggerPath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        std::ifstream inFile(triggerPath);
        std::string line;
        if (std::getline(inFile, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
                line.pop_back();
            }
        }
        inFile.close();
        DeleteFileA(triggerPath.c_str());

        m_logEntries.push_back("[ZBrush Sync] Received live sync request from ZBrush: " + line);

        if (!line.empty() && line.find('|') != std::string::npos) {
            size_t pipe = line.find('|');
            std::string high = line.substr(0, pipe);
            std::string low = line.substr(pipe + 1);
            if (!high.empty()) {
                SetCommandLineHighPoly(high);
            }
            if (!low.empty()) {
                SetCommandLineLowPoly(low);
            }
        } else {
            AutoDetectZBrushCache(true);
        }
    }
}

void UIManager::RenderUI() {
    CheckZBrushLiveSync();

    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        // Global Undo / Redo Shortcuts: Ctrl+Z = Undo, Ctrl+Shift+Z or Ctrl+Y = Redo
        if (io.KeyCtrl && !io.KeyShift && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            PerformUndo();
        } else if ((io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) ||
                   (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Y, false))) {
            PerformRedo();
        }
    }

    // 0. Check pending GPU mesh upload from background loader
    if (m_pendingMeshUpload) {
        std::lock_guard<std::mutex> lock(m_meshMutex);
        m_viewportRenderer.SetHighPolyMesh(m_highPolyMesh);
        m_viewportRenderer.FrameAll();
        CheckScaleAndAutoCalibrate();
        m_pendingMeshUpload = false;
    }

    // 0b. Check pending GPU baked normal map upload from bake worker
    if (m_pendingNormalUpload) {
        std::lock_guard<std::mutex> lock(m_normalMutex);
        m_viewportRenderer.SetBakedNormalMap(m_stagedNormalR.data(), m_stagedNormalG.data(), m_stagedNormalB.data(), m_stagedNormalW, m_stagedNormalH);
        m_pendingNormalUpload = false;
    }

    // 0. Synchronize shell state
    if (m_isLoadingHighPoly) {
        m_shellState.statusText = "Ingesting High-Poly Mesh in background...";
        m_shellState.statusColor = BakerUI::Col::Warn();
        m_shellState.bakeProgress = 0.5f; // Pulses status bar activity
    } else if (m_isBaking) {
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
    if (m_shellState.clearScene) {
        m_shellState.clearScene = false;
        ClearScene();
    }
    if (m_shellState.loadZBrushCache) {
        m_shellState.loadZBrushCache = false;
        AutoDetectZBrushCache(true);
    }
    if (m_shellState.exitApp) {
        PostQuitMessage(0);
    }

    // Menu action triggers
    if (m_shellState.triggerBake) {
        m_shellState.triggerBake = false;
        bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking && !m_isLoadingHighPoly;
        if (canBake) TriggerBake();
    }
    if (m_shellState.clearNormalPreview) {
        m_shellState.clearNormalPreview = false;
        m_viewportRenderer.ClearBakedNormalMap();
        m_logEntries.push_back("[Viewport] Cleared baked normal map preview.");
    }
    if (m_shellState.triggerUndo) {
        m_shellState.triggerUndo = false;
        PerformUndo();
    }
    if (m_shellState.triggerRedo) {
        m_shellState.triggerRedo = false;
        PerformRedo();
    }
    if (m_shellState.selectEdgeLoop) {
        m_shellState.selectEdgeLoop = false;
        if (m_hasLastSelectedEdge) {
            UVCutter::SelectEdgeLoop(m_lowPolyMesh, m_lastSelectedEdge.first, m_lastSelectedEdge.second, true);
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Select Edge Loop");
        }
    }
    if (m_shellState.selectEdgeRing) {
        m_shellState.selectEdgeRing = false;
        if (m_hasLastSelectedEdge) {
            UVCutter::SelectEdgeRing(m_lowPolyMesh, m_lastSelectedEdge.first, m_lastSelectedEdge.second, true);
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Select Edge Ring");
        }
    }
    if (m_shellState.triggerAutoSeams) {
        m_shellState.triggerAutoSeams = false;
        MarkSeamsByAngle();
    }
    if (m_shellState.triggerCutSeams) {
        m_shellState.triggerCutSeams = false;
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            UVCutter::CutSelectedEdges(m_lowPolyMesh);
            m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
            m_viewportRenderer.showSeams = true;
            PushHistoryState("Cut Seams");
        }
    }
    if (m_shellState.triggerSewSeams) {
        m_shellState.triggerSewSeams = false;
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            UVCutter::SewSelectedEdges(m_lowPolyMesh);
            m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
            PushHistoryState("Sew Seams");
        }
    }
    if (m_shellState.triggerClearSeams) {
        m_shellState.triggerClearSeams = false;
        ClearSeams();
    }
    if (m_shellState.triggerUnwrap) {
        m_shellState.triggerUnwrap = false;
        UnwrapMesh();
    }
    if (m_shellState.triggerPack) {
        m_shellState.triggerPack = false;
        PackUVs();
    }
    if (m_shellState.exportLowPoly) {
        m_shellState.exportLowPoly = false;
        ExportUnwrappedOBJ();
    }

    bool isUVMode = (m_shellState.activeTab == BakerUI::ShellState::AppTab::UVCutAndPack);

    // Workspace mode transition & focus orchestration
    if (m_shellState.activeTab != m_prevActiveTab) {
        if (isUVMode) {
            // Switched from Baking to UV Cut & Pack
            m_viewportMode = ViewportMode::SplitBoth;
            m_toolMode = ToolMode::CutSeam;
            m_viewportRenderer.showHighPoly = false; // Hide high-poly sculpt to expose low-poly mesh
            m_viewportRenderer.showCage = false;     // Hide cage
            m_viewportRenderer.showLowPoly = true;
            m_viewportRenderer.showLowPolyWireframe = true;
            m_viewportRenderer.showSeams = true;
            ImGui::SetWindowFocus("UV Tool###uvpanel");
            ImGui::SetWindowFocus("UDIM Layout###udim");
            m_logEntries.push_back("[Workspace] Switched to UV Cut & Pack View (Split 3D/2D).");
        } else {
            // Switched from UV Cut & Pack to Baking
            m_viewportMode = ViewportMode::View3D;
            m_toolMode = ToolMode::Navigate;
            m_viewportRenderer.showHighPoly = true;
            m_viewportRenderer.showCage = false;
            m_viewportRenderer.showLowPoly = true;
            ImGui::SetWindowFocus("Bake Parameters###params");
            ImGui::SetWindowFocus("Output Log###log");
            m_logEntries.push_back("[Workspace] Switched to Bake Project View.");
        }
        m_prevActiveTab = m_shellState.activeTab;
    }

    // 0. Scene Outliner (available in both workspaces)
    if (m_shellState.showOutliner) {
        RenderOutlinerPanel();
    }

    // 1. Center / Main Stage: Viewport (3D / 2D / Split)
    RenderViewportPanel();

    // 2. Mode-specific panels (strictly segregated by active workspace)
    if (!isUVMode) {
        // Baking Workspace
        RenderMeshPanel();
        RenderSettingsPanel();
    } else {
        // UV Cut & Pack Workspace
        RenderUVCutAndPackPanel();
    }

    // 3. Bottom Panels
    if (m_shellState.showUdim) {
        RenderUDIMCanvasPanel();
    }
    if (m_shellState.showLog) {
        RenderBakeProgressPanel();
    }

    BakerUI::EndFrameHost();
}

void UIManager::RenderOutlinerPanel() {
    if (!ImGui::Begin("Scene Outliner###outliner", &m_shellState.showOutliner)) {
        ImGui::End();
        return;
    }

    bool hasAnyObject = m_hasHighPoly || m_hasLowPoly || m_isLoadingHighPoly;

    // Header with clear button if meshes exist
    ImGui::BeginGroup();
    BakerUI::SectionLabel("SCENE COLLECTION");
    if (hasAnyObject) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 75.0f);
        if (ImGui::SmallButton("Clear##outliner_clr")) {
            ClearScene();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear all meshes from scene");
    }
    ImGui::EndGroup();

    // Scene Collection Tree Node (Blender style)
    bool collOpen = ImGui::TreeNodeEx("##collection_tree",
                                      ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                      hasAnyObject ? "Collection" : "Collection (Empty)");

    if (collOpen) {
        if (!hasAnyObject) {
            ImGui::Indent(18.0f);
            ImGui::TextDisabled("No objects in scene");
            ImGui::Unindent(18.0f);
        } else {
            // 1. High-Poly Sculpt Node (Only if loaded or loading)
            if (m_hasHighPoly || m_isLoadingHighPoly) {
                std::string hpName = std::filesystem::path(m_highPolyPath).stem().string();
                if (hpName.empty()) hpName = "HighPoly_Sculpt";

                ImGui::PushID("hp_node_hdr");
                bool hpOpen = ImGui::TreeNodeEx("##hp_tree", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "[HP] %s", hpName.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 95.0f);
                BakerUI::ToggleChip("View", &m_viewportRenderer.showHighPoly);
                ImGui::SameLine();
                if (ImGui::SmallButton("X##hp_del")) {
                    ClearHighPoly();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unload high-poly sculpt");
                ImGui::PopID();

                if (hpOpen) {
                    ImGui::Indent(16.0f);
                    if (m_isLoadingHighPoly) {
                        BakerUI::StatusPill("Ingesting & building BVH...", BakerUI::Col::Warn());
                    } else {
                        if (m_highPolyMesh.isQuadMesh && !m_highPolyMesh.quadIndices.empty()) {
                            ImGui::TextDisabled("Verts: %zu  |  Quads: %zu", m_highPolyMesh.positions.size(), m_highPolyMesh.quadIndices.size() / 4);
                        } else {
                            ImGui::TextDisabled("Verts: %zu  |  Tris: %zu", m_highPolyMesh.positions.size(), m_highPolyMesh.indices.size() / 3);
                        }

                        // Badges
                        if (!m_highPolyMesh.colors.empty()) {
                            BakerUI::StatusPill("Polypaint", BakerUI::Col::Ok());
                        } else {
                            BakerUI::StatusPill("No Polypaint", BakerUI::Col::TextDim());
                        }
                        ImGui::SameLine();
                        if (!m_highPolyMesh.polygroupPerFace.empty()) {
                            BakerUI::StatusPill("Polygroups", BakerUI::Col::Ok());
                        } else {
                            BakerUI::StatusPill("No Polygroups", BakerUI::Col::TextDim());
                        }

                        ImGui::SetNextItemWidth(120);
                        ImGui::SliderFloat("Opacity##hp_op", &m_viewportRenderer.highPolyOpacity, 0.05f, 1.0f, "%.2f");

                        ImGui::SetNextItemWidth(120);
                        const char* shadingModes[] = { "Studio White", "Clay", "Wax Red", "Normals", "Flat" };
                        int currentShading = (m_viewportRenderer.highPolyShading == ShadingMode::FlatShaded) ? 4 : (int)m_viewportRenderer.highPolyShading;
                        if (ImGui::Combo("Shading##hp_shade", &currentShading, shadingModes, IM_ARRAYSIZE(shadingModes))) {
                            m_viewportRenderer.highPolyShading = (currentShading == 4) ? ShadingMode::FlatShaded : (ShadingMode)currentShading;
                        }
                    }
                    ImGui::Unindent(16.0f);
                    ImGui::TreePop();
                }
                ImGui::Spacing();
            }

            // 2. Low-Poly Target Node (Only if loaded)
            if (m_hasLowPoly) {
                std::string lpName = std::filesystem::path(m_lowPolyPath).stem().string();
                if (lpName.empty()) lpName = "LowPoly_UDIM";

                ImGui::PushID("lp_node_hdr");
                bool lpOpen = ImGui::TreeNodeEx("##lp_tree", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "[LP] %s", lpName.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 145.0f);
                BakerUI::ToggleChip("View", &m_viewportRenderer.showLowPoly);
                ImGui::SameLine();
                BakerUI::ToggleChip("Wire", &m_viewportRenderer.showLowPolyWireframe);
                ImGui::SameLine();
                if (ImGui::SmallButton("X##lp_del")) {
                    ClearLowPoly();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unload low-poly mesh");
                ImGui::PopID();

                if (lpOpen) {
                    ImGui::Indent(16.0f);
                    if (m_lowPolyMesh.quadCount > 0 && m_lowPolyMesh.triCount == 0) {
                        ImGui::TextDisabled("Verts: %zu  |  Quads: %u", m_lowPolyMesh.vertices.size(), m_lowPolyMesh.quadCount);
                    } else if (m_lowPolyMesh.quadCount > 0) {
                        ImGui::TextDisabled("Verts: %zu  |  Faces: %u (%u Quads, %u Tris)",
                                            m_lowPolyMesh.vertices.size(), m_lowPolyMesh.polyCount,
                                            m_lowPolyMesh.quadCount, m_lowPolyMesh.triCount);
                    } else {
                        ImGui::TextDisabled("Verts: %zu  |  Tris: %zu", m_lowPolyMesh.vertices.size(), m_lowPolyMesh.triangles.size());
                    }
                    ImGui::SetNextItemWidth(120);
                    ImGui::SliderFloat("Opacity##lp_op", &m_viewportRenderer.lowPolyOpacity, 0.05f, 1.0f, "%.2f");

                    // UDIM tiles checklist
                    char udimHeader[64];
                    snprintf(udimHeader, sizeof(udimHeader), "UDIM Tiles (%zu)###lp_udims", m_lowPolyMesh.detectedUDIMs.size());
                    if (ImGui::TreeNodeEx(udimHeader, ImGuiTreeNodeFlags_DefaultOpen)) {
                        if (ImGui::SmallButton("All##sel_all")) SelectAllUDIMs();
                        ImGui::SameLine();
                        if (ImGui::SmallButton("None##sel_none")) m_selectedUDIMs.clear();

                        for (int tile : m_lowPolyMesh.detectedUDIMs) {
                            bool sel = IsUDIMSelected(tile);
                            char tLabel[32];
                            snprintf(tLabel, sizeof(tLabel), "Tile %d", tile);
                            if (ImGui::Checkbox(tLabel, &sel)) {
                                ToggleUDIM(tile);
                            }
                        }
                        ImGui::TreePop();
                    }
                    ImGui::Unindent(16.0f);
                    ImGui::TreePop();
                }
                ImGui::Spacing();

                // 3. Cage Shell Node (Only visible when low-poly is present)
                ImGui::PushID("cage_node_hdr");
                const char* cageLabel = m_hasExplicitCage ? "[Cage] Explicit Cage" : "[Cage] Normal Push Shell";
                bool cageOpen = ImGui::TreeNodeEx("##cage_tree", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                  "%s", cageLabel);
                ImGui::SameLine(ImGui::GetWindowWidth() - (m_hasExplicitCage ? 95.0f : 65.0f));
                BakerUI::ToggleChip("View##cage_v", &m_viewportRenderer.showCage);
                if (m_hasExplicitCage) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X##cage_del")) {
                        ClearCage();
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear explicit cage");
                }
                ImGui::PopID();

                if (cageOpen) {
                    ImGui::Indent(16.0f);
                    if (!m_hasExplicitCage) {
                        ImGui::SetNextItemWidth(120);
                        if (ImGui::DragFloat("Offset##cage_dist", &m_settings.cageDistance, 0.001f, 0.0f, 2.0f, "%.4f")) {
                            m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
                        }
                    } else {
                        std::string cName = std::filesystem::path(m_cagePath).stem().string();
                        ImGui::TextDisabled("File: %s", cName.c_str());
                    }
                    ImGui::SetNextItemWidth(120);
                    ImGui::SliderFloat("Opacity##cage_op", &m_viewportRenderer.cageOpacity, 0.05f, 1.0f, "%.2f");
                    ImGui::Unindent(16.0f);
                    ImGui::TreePop();
                }
            }
        }
        ImGui::TreePop();
    }

    ImGui::End();
}

void UIManager::Render2DUVCanvas(const ImVec2& origin, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Deep dark background matching Blender UV Editor
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(22, 22, 24, 255));

    // Clip to canvas rectangle
    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Pan and Zoom interaction
    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = (io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + size.x &&
                      io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + size.y);

    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float currentTileSize = baseTileSize * m_uvZoom;

    // Helper functions for true UV <-> Screen mapping
    auto UVToScreen = [&](float u, float v) -> ImVec2 {
        return ImVec2(origin.x + m_uvPan.x + u * currentTileSize,
                      origin.y + m_uvPan.y + (1.0f - v) * currentTileSize);
    };

    if (isHovered) {
        // Pan (RMB drag, MMB drag, or Alt + LMB drag)
        if ((!io.KeyShift && !m_markingMenu.active && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
            (io.KeyAlt && ImGui::IsMouseDragging(ImGuiMouseButton_Left))) {
            m_uvPan.x += io.MouseDelta.x;
            m_uvPan.y += io.MouseDelta.y;
        }

        // True Cursor-Anchored Zoom: the exact UV point under the mouse cursor remains frozen at the cursor
        if (io.MouseWheel != 0.0f) {
            float u = (io.MousePos.x - origin.x - m_uvPan.x) / currentTileSize;
            float v = 1.0f - (io.MousePos.y - origin.y - m_uvPan.y) / currentTileSize;

            float zoomFactor = std::pow(1.15f, io.MouseWheel);
            float newZoom = std::clamp(m_uvZoom * zoomFactor, 0.05f, 50.0f);
            float newTileSize = baseTileSize * newZoom;

            m_uvPan.x = io.MousePos.x - origin.x - u * newTileSize;
            m_uvPan.y = io.MousePos.y - origin.y - (1.0f - v) * newTileSize;

            m_uvZoom = newZoom;
            currentTileSize = newTileSize;
        }
    }

    // 1. Determine which tiles to draw:
    // By default, draw the standard unit tile 1001 [0, 1]x[0, 1].
    // If multiple UDIM tiles are detected in the loaded mesh, draw only those detected tiles.
    std::vector<int> tilesToDraw = { 1001 };
    for (int t : m_lowPolyMesh.detectedUDIMs) {
        if (t >= 1001 && t <= 1099) {
            if (std::find(tilesToDraw.begin(), tilesToDraw.end(), t) == tilesToDraw.end()) {
                tilesToDraw.push_back(t);
            }
        }
    }

    // Draw active tile canvases & subgrids (matching Blender UV editor)
    for (int udim : tilesToDraw) {
        int uTile = (udim - 1001) % 10;
        int vTile = (udim - 1001) / 10;

        float x0 = origin.x + m_uvPan.x + uTile * currentTileSize;
        float y0 = origin.y + m_uvPan.y + (1.0f - (vTile + 1)) * currentTileSize;
        float x1 = x0 + currentTileSize;
        float y1 = y0 + currentTileSize;

        if (x1 < origin.x || x0 > origin.x + size.x || y1 < origin.y || y0 > origin.y + size.y) {
            continue;
        }

        // Blender neutral dark tile fill
        drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(28, 28, 31, 255));

        // Live Baked Texture Display (xNormal / Substance style) if present
        if (m_viewportRenderer.GetBakedNormalSRV() && udim == 1001) {
            drawList->AddImage(
                (ImTextureID)m_viewportRenderer.GetBakedNormalSRV(),
                ImVec2(x0, y0),
                ImVec2(x1, y1),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f)
            );
        }

        // Blender 8x8 subgrid lines
        if (currentTileSize > 80.0f) {
            ImU32 subLineCol = IM_COL32(38, 38, 42, 255);
            for (int g = 1; g < 8; ++g) {
                float fx = x0 + (g / 8.0f) * currentTileSize;
                float fy = y0 + (g / 8.0f) * currentTileSize;
                drawList->AddLine(ImVec2(fx, y0), ImVec2(fx, y1), subLineCol, 1.0f);
                drawList->AddLine(ImVec2(x0, fy), ImVec2(x1, fy), subLineCol, 1.0f);
            }
        }

        // Clean subtle 1px border matching Blender
        drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(60, 60, 65, 255), 0.0f, 0, 1.0f);

        // UDIM label in muted grey
        char tileStr[16];
        snprintf(tileStr, sizeof(tileStr), "%d", udim);
        drawList->AddText(ImVec2(x0 + 8.0f, y0 + 6.0f), IM_COL32(110, 110, 118, 255), tileStr);
    }

    // Draw Low-Poly UV Faces and Wireframe (Clean Blender UV Shells: Pure Quad/Polygon Outlines)
    if (m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        ImU32 faceFillCol = IM_COL32(220, 225, 235, 18);   // Subtle ~7% translucent face fill
        ImU32 edgeCol = IM_COL32(100, 105, 115, 200);      // Clean 1px wireframe edge

        // 1. Face fill
        for (const auto& tri : m_lowPolyMesh.triangles) {
            const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
            const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
            const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];

            ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
            ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
            ImVec2 p2(origin.x + m_uvPan.x + v2.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v2.uv.v) * currentTileSize);

            drawList->AddTriangleFilled(p0, p1, p2, faceFillCol);
        }

        // 2. Wireframe edges (Pure polygon outlines matching Blender UV Editor: NO triangulated diagonals!)
        if (!m_lowPolyMesh.polygonFaces.empty()) {
            for (const auto& face : m_lowPolyMesh.polygonFaces) {
                size_t fv = face.size();
                for (size_t v = 0; v < fv; ++v) {
                    uint32_t i0 = face[v];
                    uint32_t i1 = face[(v + 1) % fv];
                    if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                        const auto& v0 = m_lowPolyMesh.vertices[i0];
                        const auto& v1 = m_lowPolyMesh.vertices[i1];
                        ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                        ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
                        drawList->AddLine(p0, p1, edgeCol, 1.0f);
                    }
                }
            }
        } else if (!m_lowPolyMesh.wireframeLineIndices.empty()) {
            for (size_t i = 0; i + 1 < m_lowPolyMesh.wireframeLineIndices.size(); i += 2) {
                uint32_t i0 = m_lowPolyMesh.wireframeLineIndices[i];
                uint32_t i1 = m_lowPolyMesh.wireframeLineIndices[i + 1];
                if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                    const auto& v0 = m_lowPolyMesh.vertices[i0];
                    const auto& v1 = m_lowPolyMesh.vertices[i1];
                    ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                    ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
                    drawList->AddLine(p0, p1, edgeCol, 1.0f);
                }
            }
        } else {
            for (const auto& tri : m_lowPolyMesh.triangles) {
                const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
                const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
                const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];

                ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
                ImVec2 p2(origin.x + m_uvPan.x + v2.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v2.uv.v) * currentTileSize);

                drawList->AddLine(p0, p1, edgeCol, 1.0f);
                drawList->AddLine(p1, p2, edgeCol, 1.0f);
                drawList->AddLine(p2, p0, edgeCol, 1.0f);
            }
        }
    }

    // Draw marked cut seams on 2D UV canvas (Neon Cyan)
    if (m_hasLowPoly && !m_lowPolyMesh.seamLineIndices.empty()) {
        ImU32 seamCol = IM_COL32(0, 240, 255, 230);
        for (size_t i = 0; i + 1 < m_lowPolyMesh.seamLineIndices.size(); i += 2) {
            uint32_t i0 = m_lowPolyMesh.seamLineIndices[i];
            uint32_t i1 = m_lowPolyMesh.seamLineIndices[i + 1];
            if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                const auto& v0 = m_lowPolyMesh.vertices[i0];
                const auto& v1 = m_lowPolyMesh.vertices[i1];
                ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
                drawList->AddLine(p0, p1, seamCol, 2.0f);
            }
        }
    }

    // Draw selected edges on 2D UV canvas (Glowing Golden Yellow)
    if (m_hasLowPoly && !m_lowPolyMesh.selectedEdges.empty()) {
        ImU32 selCol = IM_COL32(255, 215, 20, 255);
        for (const auto& edge : m_lowPolyMesh.selectedEdges) {
            if (edge.first < m_lowPolyMesh.vertices.size() && edge.second < m_lowPolyMesh.vertices.size()) {
                const auto& v0 = m_lowPolyMesh.vertices[edge.first];
                const auto& v1 = m_lowPolyMesh.vertices[edge.second];
                ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
                drawList->AddLine(p0, p1, selCol, 3.5f);
            }
        }
    }

    // 2D Interactive Edge Hover, Click, Double-Click Loop, and Shift+RMB Marking Menu
    if (isHovered && m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        int hoverV0 = -1, hoverV1 = -1;
        float minEdgeDist2D = 16.0f;
        ImVec2 hP0, hP1;

        for (const auto& tri : m_lowPolyMesh.triangles) {
            for (int e = 0; e < 3; ++e) {
                uint32_t ea = tri.indices[e];
                uint32_t eb = tri.indices[(e + 1) % 3];
                const auto& v0 = m_lowPolyMesh.vertices[ea];
                const auto& v1 = m_lowPolyMesh.vertices[eb];
                ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
                ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);

                float dist = DistToSegment2D(io.MousePos, p0, p1);
                if (dist < minEdgeDist2D) {
                    minEdgeDist2D = dist;
                    hoverV0 = static_cast<int>(ea);
                    hoverV1 = static_cast<int>(eb);
                    hP0 = p0;
                    hP1 = p1;
                }
            }
        }

        if (hoverV0 >= 0) {
            drawList->AddLine(hP0, hP1, IM_COL32(0, 255, 255, 255), 3.5f);

            if (io.MouseDoubleClicked[0]) {
                UVCutter::SelectEdgeLoop(m_lowPolyMesh, static_cast<uint32_t>(hoverV0), static_cast<uint32_t>(hoverV1), io.KeyShift);
                m_lastSelectedEdge = MakeEdgeKey(static_cast<uint32_t>(hoverV0), static_cast<uint32_t>(hoverV1));
                m_hasLastSelectedEdge = true;
                m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                PushHistoryState("Select Edge Loop in 2D");
                char msg[128];
                snprintf(msg, sizeof(msg), "Selected edge loop in 2D: %zu edges selected", m_lowPolyMesh.selectedEdges.size());
                m_uvStatusMessage = msg;
            } else if (ImGui::IsMouseClicked(0)) {
                auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(hoverV0), static_cast<uint32_t>(hoverV1));
                if (io.KeyShift && m_hasLastSelectedEdge && !m_lowPolyMesh.selectedEdges.empty() && m_lastSelectedEdge != edgeKey) {
                    // Maya Shift-Click Shortest Path between edges in 2D
                    UVCutter::SelectShortestPath(m_lowPolyMesh, m_lastSelectedEdge.first, m_lastSelectedEdge.second,
                                                 edgeKey.first, edgeKey.second, true);
                    m_lastSelectedEdge = edgeKey;
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    PushHistoryState("Select Shortest Path in 2D");
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Selected path between edges in 2D: %zu edges total", m_lowPolyMesh.selectedEdges.size());
                    m_uvStatusMessage = msg;
                } else if (io.KeyShift) {
                    if (m_lowPolyMesh.selectedEdges.count(edgeKey)) {
                        m_lowPolyMesh.selectedEdges.erase(edgeKey);
                    } else {
                        m_lowPolyMesh.selectedEdges.insert(edgeKey);
                    }
                    m_lastSelectedEdge = edgeKey;
                    m_hasLastSelectedEdge = true;
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    PushHistoryState("Toggle Edge Selection in 2D");
                } else {
                    m_lowPolyMesh.selectedEdges.clear();
                    m_lowPolyMesh.selectedEdges.insert(edgeKey);
                    m_lastSelectedEdge = edgeKey;
                    m_hasLastSelectedEdge = true;
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    PushHistoryState("Select Edge in 2D");
                }
            }

            if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                if (m_lowPolyMesh.selectedEdges.empty()) {
                    auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(hoverV0), static_cast<uint32_t>(hoverV1));
                    m_lowPolyMesh.selectedEdges.insert(edgeKey);
                    m_lastSelectedEdge = edgeKey;
                    m_hasLastSelectedEdge = true;
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    PushHistoryState("Select Edge in 2D");
                }
                m_markingMenu.active = true;
                m_markingMenu.centerPos = io.MousePos;
                m_markingMenu.armedWedge = -1;
            }
        } else {
            if (ImGui::IsMouseClicked(0) && !io.KeyShift && !io.KeyAlt) {
                if (!m_lowPolyMesh.selectedEdges.empty()) {
                    m_lowPolyMesh.selectedEdges.clear();
                    m_hasLastSelectedEdge = false;
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    PushHistoryState("Clear Selection");
                }
            }
            if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                m_markingMenu.active = true;
                m_markingMenu.centerPos = io.MousePos;
                m_markingMenu.armedWedge = -1;
            }
        }
    }

    // 2D UV Canvas bottom status hint text matching reference design
    char hintUV[128];
    snprintf(hintUV, sizeof(hintUV), "Texel density %.2f px/cm  *  padding %d px",
             m_packStats.averageTexelDensity > 0.01f ? m_packStats.averageTexelDensity : 10.24f,
             m_packSettings.paddingPixels);
    drawList->AddText(ImVec2(origin.x + 14.0f, origin.y + size.y - 20.0f), ImGui::GetColorU32(BakerUI::Hex(0x5A6878)), hintUV);

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
    bool open = ImGui::Begin("Viewport###viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x < 50 || avail.y < 50) {
        ImGui::End();
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    float splitterW = 6.0f;
    float leftW = avail.x;
    float rightW = 0.0f;

    if (m_viewportMode == ViewportMode::SplitBoth) {
        leftW = std::floor((avail.x - splitterW) * m_splitRatio);
        if (leftW < 80.0f) leftW = 80.0f;
        rightW = avail.x - leftW - splitterW;
        if (rightW < 80.0f) {
            rightW = 80.0f;
            leftW = avail.x - rightW - splitterW;
        }
    } else if (m_viewportMode == ViewportMode::View2D) {
        leftW = 0.0f;
        rightW = avail.x;
    }

    if (leftW > 0.0f) {
        m_viewportRenderer.Resize((int)leftW, (int)avail.y);

        if (ImGui::IsWindowHovered()) {
            bool in3DZone = (io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + leftW &&
                             io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + avail.y);
            if (in3DZone) {
                int button = 0;
                if (io.KeyAlt) {
                    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) button = 1;       // Maya Alt + LMB: Tumble/Orbit
                    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) button = 2; // Maya Alt + MMB: Track/Pan
                    else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) button = 3;  // Maya Alt + RMB: Dolly/Zoom
                } else {
                    bool isUVActive = (m_toolMode == ToolMode::CutSeam) || (m_shellState.activeTab == BakerUI::ShellState::AppTab::UVCutAndPack);
                    bool suppressNav = m_markingMenu.active || io.KeyShift;
                    if (!suppressNav) {
                        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) button = 2;      // MMB: Track/Pan
                        else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) button = 3; // RMB: Dolly/Zoom
                        else if (m_toolMode == ToolMode::Navigate && !isUVActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) button = 1;  // LMB: Orbit only in pure Navigate mode
                    }
                }

                if (button != 0 || io.MouseWheel != 0.0f) {
                    m_viewportRenderer.HandleMouseInput(io.MouseDelta.x, io.MouseDelta.y, button, io.MouseWheel);
                }

                if (ImGui::IsKeyPressed(ImGuiKey_F)) {
                    m_viewportRenderer.FrameAll();
                }
            }
        }

        m_viewportRenderer.Render();

        ID3D11ShaderResourceView* srv = m_viewportRenderer.GetViewportSRV();
        if (srv) {
            ImGui::Image((ImTextureID)srv, ImVec2(leftW, avail.y));
        }
    }

    // Marmoset-style 3D interactive brush ray-casting
    if ((m_toolMode == ToolMode::PaintCage || m_toolMode == ToolMode::PaintSkew) && !ImGui::GetIO().KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? leftW : avail.x;
        float relX = io.MousePos.x - origin.x;
        float relY = io.MousePos.y - origin.y;

        if (relX >= 0.0f && relX <= maxActiveW && relY >= 0.0f && relY <= avail.y) {
            DirectX::XMFLOAT3 rOrgW, rDirW;
            if (m_viewportRenderer.UnprojectRay(relX, relY, rOrgW, rDirW)) {
                DirectX::XMMATRIX invWorld = DirectX::XMMatrixInverse(nullptr, m_viewportRenderer.GetLowPolyWorldMatrix());
                DirectX::XMVECTOR vOrg = DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&rOrgW), invWorld);
                DirectX::XMVECTOR vDir = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&rDirW), invWorld));
                Vec3 localOrg(DirectX::XMVectorGetX(vOrg), DirectX::XMVectorGetY(vOrg), DirectX::XMVectorGetZ(vOrg));
                Vec3 localDir(DirectX::XMVectorGetX(vDir), DirectX::XMVectorGetY(vDir), DirectX::XMVectorGetZ(vDir));

                float hitT = 0.0f;
                Vec3 hitPos, hitNormal;
                int hitTriIdx = -1;
                if (m_lowPolyMesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTriIdx)) {
                    // Interactive dragging stroke
                    if (io.MouseDown[0]) {
                        bool invert = io.KeyCtrl || m_brushInvert;
                        if (m_toolMode == ToolMode::PaintCage) {
                            EnsureCageInitialized();
                            float baseDist = m_settings.cageDistance;
                            float deltaStep = m_brushStrength * (m_brushRadius * 0.04f);
                            for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
                                float d = (m_lowPolyMesh.vertices[i].position - hitPos).Length();
                                if (d <= m_brushRadius) {
                                    float sNorm = d / m_brushRadius;
                                    float falloff = (1.0f - sNorm * sNorm);
                                    falloff = falloff * falloff;
                                    if (invert) {
                                        m_lowPolyMesh.cageOffsets[i] -= deltaStep * falloff;
                                    } else {
                                        m_lowPolyMesh.cageOffsets[i] += deltaStep * falloff;
                                    }
                                    Vec3 n = m_lowPolyMesh.HasWatertightCageNormals() ? m_lowPolyMesh.cageNormals[i] : m_lowPolyMesh.vertices[i].normal;
                                    m_lowPolyMesh.cagePositions[i] = m_lowPolyMesh.vertices[i].position + n * (baseDist + m_lowPolyMesh.cageOffsets[i]);
                                }
                            }
                            m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, 1.0f);
                        } else if (m_toolMode == ToolMode::PaintSkew) {
                            EnsureSkewInitialized();
                            float deltaStep = m_brushStrength * 0.08f;
                            for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
                                float d = (m_lowPolyMesh.vertices[i].position - hitPos).Length();
                                if (d <= m_brushRadius) {
                                    float sNorm = d / m_brushRadius;
                                    float falloff = (1.0f - sNorm * sNorm);
                                    falloff = falloff * falloff;
                                    if (invert) {
                                        m_lowPolyMesh.vertexSkew[i] = (std::max)(0.0f, m_lowPolyMesh.vertexSkew[i] - deltaStep * falloff);
                                    } else {
                                        m_lowPolyMesh.vertexSkew[i] = (std::min)(1.0f, m_lowPolyMesh.vertexSkew[i] + deltaStep * falloff);
                                    }
                                }
                            }
                            m_viewportRenderer.UpdateSkew(m_lowPolyMesh, 1.0f);
                        }
                    }
                }
            }
        }
    }

    // Interactive Seam Cutting mode
    // Interactive Seam Cutting & Maya Edge Selection mode
    bool isUVCuttingActive = (m_toolMode == ToolMode::CutSeam) || (m_shellState.activeTab == BakerUI::ShellState::AppTab::UVCutAndPack);
    if (isUVCuttingActive && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? leftW : avail.x;
        float relX = io.MousePos.x - origin.x;
        float relY = io.MousePos.y - origin.y;

        if (relX >= 0.0f && relX <= maxActiveW && relY >= 0.0f && relY <= avail.y) {
            DirectX::XMFLOAT3 rOrgW, rDirW;
            if (m_viewportRenderer.UnprojectRay(relX, relY, rOrgW, rDirW)) {
                DirectX::XMMATRIX worldLP = m_viewportRenderer.GetLowPolyWorldMatrix();
                DirectX::XMMATRIX invWorld = DirectX::XMMatrixInverse(nullptr, worldLP);
                DirectX::XMVECTOR vOrg = DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&rOrgW), invWorld);
                DirectX::XMVECTOR vDir = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&rDirW), invWorld));
                Vec3 localOrg(DirectX::XMVectorGetX(vOrg), DirectX::XMVectorGetY(vOrg), DirectX::XMVectorGetZ(vOrg));
                Vec3 localDir(DirectX::XMVectorGetX(vDir), DirectX::XMVectorGetY(vDir), DirectX::XMVectorGetZ(vDir));

                int bestV0 = -1, bestV1 = -1;
                float bestScreenDist = 1e9f;
                ImVec2 bestScreenA(0, 0), bestScreenB(0, 0);

                // Helper to test an edge against the screen cursor
                auto TestEdge = [&](uint32_t ea, uint32_t eb) {
                    if (ea >= m_lowPolyMesh.vertices.size() || eb >= m_lowPolyMesh.vertices.size()) return;
                    Vec3 pA = m_lowPolyMesh.vertices[ea].position;
                    Vec3 pB = m_lowPolyMesh.vertices[eb].position;

                    DirectX::XMVECTOR va = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(pA.x, pA.y, pA.z, 1.0f), worldLP);
                    DirectX::XMVECTOR vb = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(pB.x, pB.y, pB.z, 1.0f), worldLP);
                    DirectX::XMFLOAT3 wA, wB;
                    DirectX::XMStoreFloat3(&wA, va);
                    DirectX::XMStoreFloat3(&wB, vb);

                    float sAX, sAY, sBX, sBY;
                    if (m_viewportRenderer.ProjectToScreen(wA, sAX, sAY) && m_viewportRenderer.ProjectToScreen(wB, sBX, sBY)) {
                        ImVec2 sa(origin.x + sAX, origin.y + sAY);
                        ImVec2 sb(origin.x + sBX, origin.y + sBY);
                        float d = DistToSegment2D(io.MousePos, sa, sb);
                        if (d < bestScreenDist) {
                            bestScreenDist = d;
                            bestV0 = static_cast<int>(ea);
                            bestV1 = static_cast<int>(eb);
                            bestScreenA = sa;
                            bestScreenB = sb;
                        }
                    }
                };

                float hitT = 0.0f;
                Vec3 hitPos, hitNormal;
                int hitTriIdx = -1;
                bool hasHit = m_lowPolyMesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTriIdx);

                if (hasHit && hitTriIdx >= 0) {
                    const auto& tri = m_lowPolyMesh.triangles[hitTriIdx];
                    TestEdge(tri.indices[0], tri.indices[1]);
                    TestEdge(tri.indices[1], tri.indices[2]);
                    TestEdge(tri.indices[2], tri.indices[0]);
                }

                // If ray did not hit a face directly or cursor is closer to another wireframe edge, search wireframe edges within 18px
                if (bestV0 < 0 || bestScreenDist > 18.0f) {
                    for (size_t i = 0; i + 1 < m_lowPolyMesh.wireframeLineIndices.size(); i += 2) {
                        uint32_t ea = m_lowPolyMesh.wireframeLineIndices[i];
                        uint32_t eb = m_lowPolyMesh.wireframeLineIndices[i + 1];
                        TestEdge(ea, eb);
                    }
                }

                // If an edge is found within picking radius (28 pixels on screen)
                if (bestV0 >= 0 && bestScreenDist <= 28.0f) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    // Neon cyan glowing hover line indicator (clean standard cursor)
                    drawList->AddLine(bestScreenA, bestScreenB, IM_COL32(0, 255, 255, 255), 3.5f);

                    // Maya Double-Click: Select Edge Loop
                    if (io.MouseDoubleClicked[0]) {
                        UVCutter::SelectEdgeLoop(m_lowPolyMesh, static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1), io.KeyShift);
                        m_lastSelectedEdge = MakeEdgeKey(static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1));
                        m_hasLastSelectedEdge = true;
                        m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                        PushHistoryState("Select Edge Loop");
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Selected edge loop: %zu edges selected", m_lowPolyMesh.selectedEdges.size());
                        m_uvStatusMessage = msg;
                    }
                    // Maya Single-Click: Select Edge or Shortest Path with Shift
                    else if (ImGui::IsMouseClicked(0)) {
                        auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1));
                        if (io.KeyShift && m_hasLastSelectedEdge && !m_lowPolyMesh.selectedEdges.empty() && m_lastSelectedEdge != edgeKey) {
                            // Maya Shift-Click Shortest Path between previously clicked edge and this edge!
                            UVCutter::SelectShortestPath(m_lowPolyMesh, m_lastSelectedEdge.first, m_lastSelectedEdge.second,
                                                         edgeKey.first, edgeKey.second, true);
                            m_lastSelectedEdge = edgeKey;
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Select Shortest Path");
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Selected path between edges: %zu edges total", m_lowPolyMesh.selectedEdges.size());
                            m_uvStatusMessage = msg;
                        } else if (io.KeyShift) {
                            if (m_lowPolyMesh.selectedEdges.count(edgeKey)) {
                                m_lowPolyMesh.selectedEdges.erase(edgeKey);
                            } else {
                                m_lowPolyMesh.selectedEdges.insert(edgeKey);
                            }
                            m_lastSelectedEdge = edgeKey;
                            m_hasLastSelectedEdge = true;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Toggle Edge Selection");
                        } else {
                            m_lowPolyMesh.selectedEdges.clear();
                            m_lowPolyMesh.selectedEdges.insert(edgeKey);
                            m_lastSelectedEdge = edgeKey;
                            m_hasLastSelectedEdge = true;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Select Edge");
                        }
                    }

                    // Maya Shift + Right-Click: Open Marking Menu
                    if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        if (m_lowPolyMesh.selectedEdges.empty()) {
                            auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1));
                            m_lowPolyMesh.selectedEdges.insert(edgeKey);
                            m_lastSelectedEdge = edgeKey;
                            m_hasLastSelectedEdge = true;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Select Edge");
                        }
                        m_markingMenu.active = true;
                        m_markingMenu.centerPos = io.MousePos;
                        m_markingMenu.armedWedge = -1;
                    }
                } else {
                    // Clicked in empty space without Shift: clear selection
                    if (ImGui::IsMouseClicked(0) && !io.KeyShift && !io.KeyAlt) {
                        if (!m_lowPolyMesh.selectedEdges.empty()) {
                            m_lowPolyMesh.selectedEdges.clear();
                            m_hasLastSelectedEdge = false;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Clear Selection");
                        }
                    }
                }
            }
        }
    }

    // Maya Shift + Right-Click general 3D viewport trigger
    if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !m_markingMenu.active && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? leftW : avail.x;
        float relX = io.MousePos.x - origin.x;
        float relY = io.MousePos.y - origin.y;
        if (relX >= 0.0f && relX <= maxActiveW && relY >= 0.0f && relY <= avail.y) {
            m_markingMenu.active = true;
            m_markingMenu.centerPos = io.MousePos;
            m_markingMenu.armedWedge = -1;
        }
    }

    // Interactive 3D Surface Curve Drawing and Point Movement
    if (m_toolMode == ToolMode::DrawCurve && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? leftW : avail.x;
        float relX = io.MousePos.x - origin.x;
        float relY = io.MousePos.y - origin.y;

        if (relX >= 0.0f && relX <= maxActiveW && relY >= 0.0f && relY <= avail.y) {
            DirectX::XMFLOAT3 rOrgW, rDirW;
            if (m_viewportRenderer.UnprojectRay(relX, relY, rOrgW, rDirW)) {
                DirectX::XMMATRIX worldMat = m_viewportRenderer.GetLowPolyWorldMatrix();
                DirectX::XMMATRIX viewProjMat = m_viewportRenderer.GetViewProjectionMatrix();
                int vpW = m_viewportRenderer.GetWidth();
                int vpH = m_viewportRenderer.GetHeight();

                DirectX::XMMATRIX invWorld = DirectX::XMMatrixInverse(nullptr, worldMat);
                DirectX::XMVECTOR vOrg = DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&rOrgW), invWorld);
                DirectX::XMVECTOR vDir = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&rDirW), invWorld));
                Vec3 localOrg(DirectX::XMVectorGetX(vOrg), DirectX::XMVectorGetY(vOrg), DirectX::XMVectorGetZ(vOrg));
                Vec3 localDir(DirectX::XMVectorGetX(vDir), DirectX::XMVectorGetY(vDir), DirectX::XMVectorGetZ(vDir));

                float hitT = 0.0f;
                Vec3 hitPos, hitNormal;
                int hitTriIdx = -1;
                bool hit = m_lowPolyMesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTriIdx);

                SurfaceCurve* activeCurve = m_curveManager.GetActiveCurve();

                // 1. Detect if mouse cursor is hovering over any existing point handle
                int hoveredPt = -1;
                if (activeCurve) {
                    for (size_t p = 0; p < activeCurve->points.size(); ++p) {
                        float sX, sY;
                        if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(activeCurve->points[p].position.x, activeCurve->points[p].position.y, activeCurve->points[p].position.z), sX, sY)) {
                            float dScreen = std::hypot(io.MousePos.x - (origin.x + sX), io.MousePos.y - (origin.y + sY));
                            if (dScreen <= 16.0f) {
                                hoveredPt = static_cast<int>(p);
                                break;
                            }
                        }
                    }
                }
                m_curveManager.hoveredPointIndex = hoveredPt;

                // 2. Handle Dragging existing point
                if (m_draggingPointIndex >= 0 && io.MouseDown[0]) {
                    m_wasDraggingPoint = true;
                    if (hit && activeCurve) {
                        m_curveManager.MoveActivePoint(m_draggingPointIndex, hitPos, hitNormal, hitTriIdx);
                        m_curveManager.UpdateActiveSampledPath(m_lowPolyMesh, &worldMat, &viewProjMat, vpW, vpH);
                    }
                } else if (io.MouseReleased[0]) {
                    if (m_wasDraggingPoint) {
                        m_wasDraggingPoint = false;
                        PushHistoryState("Move Curve Point");
                    }
                    m_draggingPointIndex = -1;
                }

                // 3. Handle Click to Grab or Add Point on Surface
                if (ImGui::IsMouseClicked(0)) {
                    if (hoveredPt >= 0) {
                        if (io.KeyShift) {
                            m_curveManager.selectedPointEndIndex = hoveredPt;
                        } else {
                            m_draggingPointIndex = hoveredPt;
                            m_curveManager.selectedPointIndex = hoveredPt;
                            m_curveManager.selectedPointEndIndex = -1;
                        }
                    } else if (hit) {
                        m_curveManager.AddPointToActive(hitPos, hitNormal, hitTriIdx);
                        m_curveManager.UpdateActiveSampledPath(m_lowPolyMesh, &worldMat, &viewProjMat, vpW, vpH);
                        PushHistoryState("Add Curve Point");

                        char msg[128];
                        snprintf(msg, sizeof(msg), "Placed curve point %zu on surface. Press Enter to slice mesh topology and start next cut.",
                                 activeCurve ? activeCurve->points.size() : 1);
                        m_uvStatusMessage = msg;
                    }
                }

                // Finish curve and slice mesh topology on Enter or Right Click
                if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsMouseClicked(1)) {
                    m_draggingPointIndex = -1;
                    m_curveManager.selectedPointIndex = -1;
                    if (activeCurve && activeCurve->points.size() >= 2) {
                        std::vector<std::pair<uint32_t, uint32_t>> seamEdges;
                        CurveManager::CutCurveIntoMeshTopology(*activeCurve, m_lowPolyMesh, seamEdges);
                        activeCurve->isBaked = true;
                        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
                        m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
                        m_viewportRenderer.showSeams = true;

                        // Automatically finalize this curve and start a new fresh curve! Tool mode stays active!
                        int newCurveIdx = m_curveManager.AddCurve();
                        PushHistoryState("Slice Mesh Topology");
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Sliced mesh topology along curve (%zu slice edges). Ready for next cut.",
                                 seamEdges.size());
                        m_uvStatusMessage = msg;
                        m_logEntries.push_back(std::string("[Curve Slice] ") + msg);
                    }
                }
            }
        }
    }

    // Render 3D Surface Curves on Viewport
    if (m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        float viewX0 = origin.x;
        float viewY0 = origin.y;
        DirectX::XMMATRIX worldMat = m_viewportRenderer.GetLowPolyWorldMatrix();
        DirectX::XMMATRIX viewProjMat = m_viewportRenderer.GetViewProjectionMatrix();
        int vpW = m_viewportRenderer.GetWidth();
        int vpH = m_viewportRenderer.GetHeight();

        for (size_t cIdx = 0; cIdx < m_curveManager.curves.size(); ++cIdx) {
            const auto& curve = m_curveManager.curves[cIdx];
            if (curve.points.empty()) continue;

            bool isActive = (static_cast<int>(cIdx) == m_curveManager.activeCurveIndex);
            // Golden amber curve matching reference design
            ImU32 curveColor = isActive ? IM_COL32(255, 172, 48, 255) : IM_COL32(78, 226, 182, 190);
            float thickness = isActive ? 3.0f : 2.0f;

            // Draw dense surface-following path contoured over mesh bumps and crevices
            if (curve.sampledPath.size() >= 2) {
                for (size_t s = 0; s + 1 < curve.sampledPath.size(); ++s) {
                    const Vec3& p0 = curve.sampledPath[s];
                    const Vec3& p1 = curve.sampledPath[s + 1];
                    float s0X, s0Y, s1X, s1Y;
                    if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(p0.x, p0.y, p0.z), s0X, s0Y) &&
                        m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(p1.x, p1.y, p1.z), s1X, s1Y)) {
                        drawList->AddLine(ImVec2(viewX0 + s0X, viewY0 + s0Y), ImVec2(viewX0 + s1X, viewY0 + s1Y), curveColor, thickness);
                    }
                }
            } else {
                size_t ptCount = curve.points.size();
                size_t segCount = curve.isClosed ? ptCount : (ptCount - 1);
                for (size_t i = 0; i < segCount; ++i) {
                    const Vec3& p0 = curve.points[i].position;
                    const Vec3& p1 = curve.points[(i + 1) % ptCount].position;
                    float s0X, s0Y, s1X, s1Y;
                    if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(p0.x, p0.y, p0.z), s0X, s0Y) &&
                        m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(p1.x, p1.y, p1.z), s1X, s1Y)) {
                        drawList->AddLine(ImVec2(viewX0 + s0X, viewY0 + s0Y), ImVec2(viewX0 + s1X, viewY0 + s1Y), curveColor, thickness);
                    }
                }
            }

            // Highlight selected segment if any
            if (isActive && m_curveManager.selectedPointIndex >= 0 && curve.points.size() >= 2) {
                int p0 = m_curveManager.selectedPointIndex;
                int p1 = (m_curveManager.selectedPointEndIndex >= 0) ? m_curveManager.selectedPointEndIndex
                                                                      : (p0 + 1 < (int)curve.points.size() ? p0 + 1 : (curve.isClosed ? 0 : p0));
                if (p0 != p1) {
                    size_t sStart = static_cast<size_t>(p0);
                    size_t sEnd = static_cast<size_t>(p1);
                    if (sStart > sEnd && !curve.isClosed) std::swap(sStart, sEnd);

                    for (size_t s = sStart; s < sEnd && s < curve.segmentPaths.size(); ++s) {
                        const auto& seg = curve.segmentPaths[s];
                        for (size_t k = 0; k + 1 < seg.size(); ++k) {
                            float s0X, s0Y, s1X, s1Y;
                            if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(seg[k].x, seg[k].y, seg[k].z), s0X, s0Y) &&
                                m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(seg[k+1].x, seg[k+1].y, seg[k+1].z), s1X, s1Y)) {
                                drawList->AddLine(ImVec2(viewX0 + s0X, viewY0 + s0Y), ImVec2(viewX0 + s1X, viewY0 + s1Y),
                                                  IM_COL32(255, 60, 220, 255), 4.5f);
                            }
                        }
                    }
                }
            }

            // Draw control point handles
            size_t ptCount = curve.points.size();
            for (size_t p = 0; p < ptCount; ++p) {
                const Vec3& pt = curve.points[p].position;
                float sX, sY;
                if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(pt.x, pt.y, pt.z), sX, sY)) {
                    ImVec2 scrP(viewX0 + sX, viewY0 + sY);
                    bool isSelected = isActive && (m_curveManager.selectedPointIndex == static_cast<int>(p) ||
                                                   m_curveManager.selectedPointEndIndex == static_cast<int>(p) ||
                                                   m_draggingPointIndex == static_cast<int>(p));
                    bool isHovered = isActive && (m_curveManager.hoveredPointIndex == static_cast<int>(p));

                    if (isSelected || isHovered) {
                        drawList->AddCircle(scrP, 9.0f, IM_COL32(255, 172, 48, 255), 18, 2.0f);
                    }
                    drawList->AddCircleFilled(scrP, 5.0f, isSelected ? IM_COL32(255, 172, 48, 255) : IM_COL32(245, 248, 252, 255));
                    drawList->AddCircle(scrP, 5.0f, IM_COL32(20, 27, 36, 240), 16, 1.5f);

                    char pLabel[16];
                    snprintf(pLabel, sizeof(pLabel), "P%zu", p);
                    drawList->AddText(ImVec2(scrP.x + 8.0f, scrP.y - 14.0f), isSelected ? IM_COL32(255, 172, 48, 255) : IM_COL32(180, 195, 210, 220), pLabel);
                }
            }

            // If in DrawCurve mode and curve is active, draw camera-angle surface contour preview to cursor
            if (isActive && m_toolMode == ToolMode::DrawCurve && m_draggingPointIndex < 0 && !curve.points.empty()) {
                float relX = io.MousePos.x - origin.x;
                float relY = io.MousePos.y - origin.y;
                float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? leftW : avail.x;
                if (relX >= 0.0f && relX <= maxActiveW && relY >= 0.0f && relY <= avail.y) {
                    DirectX::XMFLOAT3 rOrgW, rDirW;
                    if (m_viewportRenderer.UnprojectRay(relX, relY, rOrgW, rDirW)) {
                        DirectX::XMMATRIX invWorld = DirectX::XMMatrixInverse(nullptr, worldMat);
                        DirectX::XMVECTOR vOrg = DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&rOrgW), invWorld);
                        DirectX::XMVECTOR vDir = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(DirectX::XMLoadFloat3(&rDirW), invWorld));
                        Vec3 localOrg(DirectX::XMVectorGetX(vOrg), DirectX::XMVectorGetY(vOrg), DirectX::XMVectorGetZ(vOrg));
                        Vec3 localDir(DirectX::XMVectorGetX(vDir), DirectX::XMVectorGetY(vDir), DirectX::XMVectorGetZ(vDir));

                        float hitT = 0.0f;
                        Vec3 hitPos, hitNormal;
                        int hitTriIdx = -1;
                        if (m_lowPolyMesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTriIdx)) {
                            const Vec3& lastP = curve.points.back().position;
                            auto previewPath = CurveManager::SampleCameraSurfacePath(
                                m_lowPolyMesh, worldMat, viewProjMat, vpW, vpH, lastP, hitPos, 32
                            );
                            for (size_t s = 0; s + 1 < previewPath.size(); ++s) {
                                float s0X, s0Y, s1X, s1Y;
                                if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(previewPath[s].x, previewPath[s].y, previewPath[s].z), s0X, s0Y) &&
                                    m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(previewPath[s + 1].x, previewPath[s + 1].y, previewPath[s + 1].z), s1X, s1Y)) {
                                    drawList->AddLine(ImVec2(viewX0 + s0X, viewY0 + s0Y), ImVec2(viewX0 + s1X, viewY0 + s1Y), IM_COL32(255, 220, 60, 240), 2.5f);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    bool isUVMode = (m_shellState.activeTab == BakerUI::ShellState::AppTab::UVCutAndPack);

    // Floating overlay (top-left) - 3D controls (context-aware grouping by workspace mode)
    if (m_viewportMode != ViewportMode::View2D) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 12, origin.y + 12));
        ImGui::BeginGroup();

        if (!isUVMode) {
            // === BAKING VIEW CONTROLS ===
            BakerUI::ToggleChip("High", &m_viewportRenderer.showHighPoly); ImGui::SameLine();
            BakerUI::ToggleChip("Low", &m_viewportRenderer.showLowPoly); ImGui::SameLine();
            BakerUI::ToggleChip("Wire", &m_viewportRenderer.showLowPolyWireframe); ImGui::SameLine();
            BakerUI::ToggleChip("Cage", &m_viewportRenderer.showCage); ImGui::SameLine();
            BakerUI::ToggleChip("Grid", &m_viewportRenderer.showGrid); ImGui::SameLine();
            if (m_viewportRenderer.HasBakedNormalMap()) {
                BakerUI::ToggleChip("Baked Normals", &m_viewportRenderer.showBakedNormals); ImGui::SameLine();
            }

            ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();

            bool isNav = (m_toolMode == ToolMode::Navigate);
            if (BakerUI::ToggleChip("Navigate", &isNav)) {
                m_toolMode = ToolMode::Navigate;
            }
            ImGui::SameLine();
            bool isPaintCage = (m_toolMode == ToolMode::PaintCage);
            if (BakerUI::ToggleChip("Paint Cage", &isPaintCage)) {
                m_toolMode = isPaintCage ? ToolMode::PaintCage : ToolMode::Navigate;
                if (m_toolMode == ToolMode::PaintCage) {
                    m_viewportRenderer.showCage = true;
                    EnsureCageInitialized();
                }
            }
            ImGui::SameLine();
            bool isPaintSkew = (m_toolMode == ToolMode::PaintSkew);
            if (BakerUI::ToggleChip("Paint Skew", &isPaintSkew)) {
                m_toolMode = isPaintSkew ? ToolMode::PaintSkew : ToolMode::Navigate;
                if (m_toolMode == ToolMode::PaintSkew) {
                    EnsureSkewInitialized();
                    m_viewportRenderer.showSkew = true;
                } else {
                    m_viewportRenderer.showSkew = false;
                }
            }
        } else {
            // === UV CUT & PACK VIEW CONTROLS ===
            BakerUI::ToggleChip("Low", &m_viewportRenderer.showLowPoly); ImGui::SameLine();
            BakerUI::ToggleChip("Wire", &m_viewportRenderer.showLowPolyWireframe); ImGui::SameLine();
            BakerUI::ToggleChip("Seams", &m_viewportRenderer.showSeams); ImGui::SameLine();
            BakerUI::ToggleChip("Grid", &m_viewportRenderer.showGrid); ImGui::SameLine();

            ImGui::Dummy(ImVec2(6.0f, 0.0f)); ImGui::SameLine();
            bool canUndo = m_history.CanUndo();
            if (!canUndo) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Undo")) {
                PerformUndo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tip = canUndo ? ("Undo: " + m_history.GetUndoDescription() + " (Ctrl+Z)") : "Nothing to undo (Ctrl+Z)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (!canUndo) ImGui::EndDisabled();

            ImGui::SameLine();
            bool canRedo = m_history.CanRedo();
            if (!canRedo) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Redo")) {
                PerformRedo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                std::string tip = canRedo ? ("Redo: " + m_history.GetRedoDescription() + " (Ctrl+Shift+Z)") : "Nothing to redo (Ctrl+Shift+Z)";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            if (!canRedo) ImGui::EndDisabled();

            ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();

            bool isNav = (m_toolMode == ToolMode::Navigate);
            if (BakerUI::ToggleChip("Navigate", &isNav)) {
                m_toolMode = ToolMode::Navigate;
            }
            ImGui::SameLine();
            bool isCutSeam = (m_toolMode == ToolMode::CutSeam);
            if (BakerUI::ToggleChip("Cut Seam", &isCutSeam)) {
                m_toolMode = isCutSeam ? ToolMode::CutSeam : ToolMode::Navigate;
                if (m_toolMode == ToolMode::CutSeam) {
                    m_viewportRenderer.showSeams = true;
                }
            }
            ImGui::SameLine();
            bool isDrawCurve = (m_toolMode == ToolMode::DrawCurve);
            if (BakerUI::ToggleChip("Curve Cut", &isDrawCurve)) {
                m_toolMode = isDrawCurve ? ToolMode::DrawCurve : ToolMode::Navigate;
                if (m_toolMode == ToolMode::DrawCurve) {
                    m_viewportRenderer.showSeams = true;
                }
            }
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(8, 0)); ImGui::SameLine();

        ImGui::SetNextItemWidth(100);
        const char* shadingModes[] = { "Studio White", "Clay", "Wax Red", "Normals", "Flat" };
        int currentShading = (m_viewportRenderer.highPolyShading == ShadingMode::FlatShaded) ? 4 : (int)m_viewportRenderer.highPolyShading;
        if (ImGui::Combo("##Shading", &currentShading, shadingModes, IM_ARRAYSIZE(shadingModes))) {
            m_viewportRenderer.highPolyShading = (currentShading == 4) ? ShadingMode::FlatShaded : (ShadingMode)currentShading;
        }
        ImGui::EndGroup();

        // Floating Marmoset 3D Brush Settings Bar
        if (m_toolMode != ToolMode::Navigate && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 12.0f, origin.y + 44.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 0.96f));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(40.0f / 255.0f, 40.0f / 255.0f, 40.0f / 255.0f, 0.90f));
            float barW = (std::min)(940.0f, avail.x - 24.0f);
            ImGui::BeginChild("##brush_bar_zb", ImVec2(barW, 36.0f), true, ImGuiWindowFlags_NoScrollbar);
            {
                if (m_toolMode == ToolMode::DrawCurve) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.1f, 1.0f), "CAMERA CURVE CUT");
                    ImGui::SameLine();
                    SurfaceCurve* c = m_curveManager.GetActiveCurve();
                    ImGui::TextDisabled("%s (%zu pts)", c ? c->name.c_str() : "Curve", c ? c->points.size() : 0);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.0f, 0.95f, 1.0f, 1.0f), "| Seams: %zu", m_lowPolyMesh.cutSeams.size());
                    ImGui::SameLine();
                    if (ImGui::Button("Snap Seams")) {
                        SnapActiveCurveToSeams();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Cut Topology")) {
                        CutActiveCurveIntoTopology();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("+ New")) {
                        m_curveManager.AddCurve();
                    }
                    ImGui::SameLine();
                    if (c) {
                        ImGui::Checkbox("Loop", &c->isClosed);
                        ImGui::SameLine();
                    }
                    if (c && c->points.size() >= 2) {
                        int ptCountInt = static_cast<int>(c->points.size());
                        int p0 = (m_curveManager.selectedPointIndex >= 0) ? m_curveManager.selectedPointIndex : 0;
                        if (p0 >= ptCountInt) p0 = ptCountInt - 1;
                        int defaultP1 = (p0 + 1 < ptCountInt) ? (p0 + 1) : (c->isClosed ? 0 : p0);
                        int p1 = (m_curveManager.selectedPointEndIndex >= 0) ? m_curveManager.selectedPointEndIndex : defaultP1;
                        if (p1 >= ptCountInt) p1 = ptCountInt - 1;

                        char strBtn[48];
                        snprintf(strBtn, sizeof(strBtn), "Straighten P%d->P%d", p0, p1);
                        if (ImGui::Button(strBtn)) {
                            PushHistoryState("Straighten Curve Segment");
                            c->StraightenSegment(p0, p1, m_lowPolyMesh);
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Straightened segment P%d -> P%d", p0, p1);
                            m_uvStatusMessage = msg;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Straightens the surface path between P%d and P%d", p0, p1);

                        ImGui::SameLine();
                        char smBtn[48];
                        snprintf(smBtn, sizeof(smBtn), "Smooth P%d->P%d", p0, p1);
                        if (ImGui::Button(smBtn)) {
                            PushHistoryState("Smooth Curve Segment");
                            c->SmoothSegment(p0, p1, m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Smoothed segment P%d -> P%d (%d iter)", p0, p1, m_curveManager.smoothIterations);
                            m_uvStatusMessage = msg;
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths out kinks/deviations between P%d and P%d using Laplacian relaxation", p0, p1);

                        ImGui::SameLine();
                        if (ImGui::Button("Smooth All")) {
                            PushHistoryState("Smooth Entire Curve");
                            c->SmoothEntireCurve(m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
                            m_uvStatusMessage = "Smoothed entire curve";
                        }
                        ImGui::SameLine();
                    }
                    if (ImGui::Button("Clear")) {
                        ClearActiveCurve();
                    }
                } else if (m_toolMode == ToolMode::CutSeam) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(ImVec4(0.0f, 0.95f, 1.0f, 1.0f), "MAYA UV CUTTING");
                    ImGui::SameLine();
                    ImGui::TextDisabled("| Shift+RMB: Marking Menu | 2xClick: Loop | Shift+Click: Path Between");
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.12f, 1.0f), "| Selected: %zu", m_lowPolyMesh.selectedEdges.size());
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 1.0f, 1.0f), "| Seams: %zu", m_lowPolyMesh.cutSeams.size());
                } else {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(m_toolMode == ToolMode::PaintCage ? ImVec4(0.3f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                       m_toolMode == ToolMode::PaintCage ? "CAGE BRUSH" : "SKEW BRUSH");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0f);
                    ImGui::SliderFloat("Radius##zb", &m_brushRadius, 0.02f, 10.0f, "%.2f");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(90.0f);
                    ImGui::SliderFloat("Strength##zb", &m_brushStrength, 0.05f, 1.0f, "%.2f");
                    ImGui::SameLine();
                    ImGui::Checkbox("Invert (Ctrl)", &m_brushInvert);
                    ImGui::SameLine();

                    if (m_toolMode == ToolMode::PaintCage) {
                        if (ImGui::Button("Reset Cage")) {
                            ResetPaintedCage();
                        }
                    } else if (m_toolMode == ToolMode::PaintSkew) {
                        BakerUI::ToggleChip("Heatmap", &m_viewportRenderer.showSkew);
                        ImGui::SameLine();
                        if (ImGui::Button("Reset Skew")) {
                            ResetPaintedSkew();
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(2);
        }
    }

    // 3D Viewport bottom status hint text matching reference design
    if (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const char* hint3D = "Alt+LMB orbit  *  Shift+RMB marking menu  *  Enter finalize cut";
        dl->AddText(ImVec2(origin.x + 14.0f, origin.y + avail.y - 20.0f), ImGui::GetColorU32(BakerUI::Hex(0x5A6878)), hint3D);
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

    // 2D UV Canvas overlay & sliding splitter if 2D or Split mode is active
    if (m_viewportMode == ViewportMode::View2D) {
        Render2DUVCanvas(origin, avail);
    } else if (m_viewportMode == ViewportMode::SplitBoth) {
        // Interactive Draggable Splitter Bar
        ImGui::SetCursorScreenPos(ImVec2(origin.x + leftW, origin.y));
        ImGui::InvisibleButton("##split_slider", ImVec2(splitterW, avail.y));
        bool isSplitHovered = ImGui::IsItemHovered();
        bool isSplitActive = ImGui::IsItemActive();
        if (isSplitHovered || isSplitActive) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (isSplitActive) {
            m_splitRatio += io.MouseDelta.x / (avail.x - splitterW);
            m_splitRatio = std::clamp(m_splitRatio, 0.10f, 0.90f);
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 splitBg = isSplitActive ? ImGui::GetColorU32(BakerUI::Hex(0x4CC3A6, 0.35f)) :
                        (isSplitHovered ? ImGui::GetColorU32(BakerUI::Hex(0x4CC3A6, 0.15f)) :
                                           ImGui::GetColorU32(BakerUI::Hex(0x181818)));
        dl->AddRectFilled(ImVec2(origin.x + leftW, origin.y),
                          ImVec2(origin.x + leftW + splitterW, origin.y + avail.y), splitBg);

        ImU32 splitLineCol = isSplitActive ? ImGui::GetColorU32(BakerUI::Col::Accent()) :
                             (isSplitHovered ? ImGui::GetColorU32(BakerUI::Col::AccentHi()) :
                                                ImGui::GetColorU32(BakerUI::Hex(0x282828)));
        dl->AddLine(ImVec2(origin.x + leftW + 3.0f, origin.y),
                    ImVec2(origin.x + leftW + 3.0f, origin.y + avail.y),
                    splitLineCol);

        // Tactile central grip pill
        float midY = origin.y + avail.y * 0.5f;
        ImU32 gripCol = (isSplitHovered || isSplitActive) ? ImGui::GetColorU32(BakerUI::Col::Accent()) :
                                                             ImGui::GetColorU32(BakerUI::Hex(0x505050));
        dl->AddRectFilled(ImVec2(origin.x + leftW + 1.5f, midY - 14.0f),
                          ImVec2(origin.x + leftW + splitterW - 1.5f, midY + 14.0f),
                          gripCol, 1.5f);

        ImVec2 splitPos = ImVec2(origin.x + leftW + splitterW, origin.y);
        ImVec2 splitSize = ImVec2(rightW, avail.y);
        Render2DUVCanvas(splitPos, splitSize);
    }

    // Floating Workspace Action HUD (bottom center of viewport)
    if (!isUVMode) {
        // Baking View: Start Bake & Progress HUD
        int udimCount = m_lowPolyMesh.detectedUDIMs.empty() ? 1 : (int)m_lowPolyMesh.detectedUDIMs.size();
        bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking && !m_isLoadingHighPoly;
        if (BakerUI::FloatingBakeBar(origin, avail, canBake, m_isBaking, m_bakeProgress, m_bakeStatus.c_str(),
                                     udimCount, m_settings.resolution, m_settings.resolution)) {
            if (canBake) {
                TriggerBake();
            }
        }
    } else {
        // UV Cut & Pack View: Quick Unwrap, Pack, and Export HUD
        RenderFloatingUVBar(origin, avail);
    }

    RenderMarkingMenu();

    ImGui::End();
}

void UIManager::RenderFloatingUVBar(const ImVec2& origin, const ImVec2& avail) {
    if (!m_hasLowPoly) return;

    float barW = 460.0f;
    float barH = 38.0f;
    float posX = origin.x + (avail.x - barW) * 0.5f;
    float posY = origin.y + avail.y - barH - 12.0f;

    ImGui::SetCursorScreenPos(ImVec2(posX, posY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(24.0f / 255.0f, 24.0f / 255.0f, 26.0f / 255.0f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(48.0f / 255.0f, 48.0f / 255.0f, 52.0f / 255.0f, 0.85f));

    if (ImGui::BeginChild("##floating_uv_bar", ImVec2(barW, barH), true, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::AlignTextToFramePadding();

        size_t seamCount = m_lowPolyMesh.cutSeams.size();
        char seamStr[32];
        snprintf(seamStr, sizeof(seamStr), "%zu Seams", seamCount);
        BakerUI::StatusPill(seamStr, seamCount > 0 ? BakerUI::Hex(0x4CC3A6) : BakerUI::Hex(0x888888));
        ImGui::SameLine(0, 8.0f);

        char islStr[32];
        snprintf(islStr, sizeof(islStr), "%zu Islands", m_currentIslands.size());
        BakerUI::StatusPill(islStr, m_currentIslands.empty() ? BakerUI::Hex(0x888888) : BakerUI::Hex(0x4E92F9));
        ImGui::SameLine(0, 12.0f);

        if (BakerUI::PrimaryButton("Unwrap (Ctrl+U)", 115, 24)) {
            UnwrapMesh();
        }
        ImGui::SameLine(0, 6.0f);

        if (BakerUI::SecondaryButton("Pack (Ctrl+P)", 105, 24)) {
            PackUVs();
        }
        ImGui::SameLine(0, 6.0f);

        if (BakerUI::SecondaryButton("Export OBJ", 90, 24)) {
            ExportUnwrappedOBJ();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void UIManager::RenderMarkingMenu() {
    if (!m_markingMenu.active) return;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    ImVec2 c = m_markingMenu.centerPos;
    float dx = io.MousePos.x - c.x;
    float dy = io.MousePos.y - c.y;
    float dist = std::sqrt(dx * dx + dy * dy);
    m_markingMenu.dragDist = dist;

    // Determine armed wedge based on angle if dragged > 16 pixels
    int armed = -1;
    if (dist > 16.0f) {
        float angleDeg = std::atan2(dy, dx) * 180.0f / 3.14159265f; // [-180, 180]
        if (angleDeg >= -112.5f && angleDeg < -67.5f) {
            armed = 0; // CUT (North)
        } else if (angleDeg >= 67.5f && angleDeg < 112.5f) {
            armed = 1; // SEW (South)
        } else if (angleDeg >= -22.5f && angleDeg < 22.5f) {
            armed = 2; // LOOP (East)
        } else if (angleDeg >= 157.5f || angleDeg < -157.5f) {
            armed = 3; // UNWRAP (West)
        } else if (angleDeg >= -67.5f && angleDeg < -22.5f) {
            armed = 4; // EDGE RING (NE)
        } else if (angleDeg >= -157.5f && angleDeg < -112.5f) {
            armed = 5; // CAMERA PROJECT (NW)
        } else if (angleDeg >= 22.5f && angleDeg < 67.5f) {
            armed = 6; // AUTO UV (SE)
        } else if (angleDeg >= 112.5f && angleDeg < 157.5f) {
            armed = 7; // CLEAR SEL (SW)
        }
    }
    m_markingMenu.armedWedge = armed;

    // Draw dark radial hub at center
    drawList->AddCircleFilled(c, 24.0f, IM_COL32(18, 20, 24, 235));
    drawList->AddCircle(c, 24.0f, IM_COL32(80, 85, 95, 210), 32, 2.0f);
    drawList->AddText(ImVec2(c.x - 13.0f, c.y - 6.0f), IM_COL32(0, 0, 0, 220), "MAYA");
    drawList->AddText(ImVec2(c.x - 14.0f, c.y - 7.0f), IM_COL32(0, 230, 255, 240), "MAYA");

    // Radial spokes
    auto DrawSpoke = [&](float angleDeg) {
        float rad = angleDeg * 3.14159265f / 180.0f;
        ImVec2 p0(c.x + std::cos(rad) * 26.0f, c.y + std::sin(rad) * 26.0f);
        ImVec2 p1(c.x + std::cos(rad) * 58.0f, c.y + std::sin(rad) * 58.0f);
        drawList->AddLine(p0, p1, IM_COL32(70, 75, 88, 170), 1.5f);
    };
    DrawSpoke(-90.0f);  // N
    DrawSpoke(90.0f);   // S
    DrawSpoke(0.0f);    // E
    DrawSpoke(180.0f);  // W
    DrawSpoke(-45.0f);  // NE
    DrawSpoke(-135.0f); // NW
    DrawSpoke(45.0f);   // SE
    DrawSpoke(135.0f);  // SW

    // Draw active gesture laser line towards cursor
    if (dist > 8.0f) {
        drawList->AddLine(c, io.MousePos, IM_COL32(0, 220, 255, 220), 2.5f);
        drawList->AddCircleFilled(io.MousePos, 4.0f, IM_COL32(0, 255, 255, 255));
    }

    // Helper to render wedge badges
    auto RenderWedgeBadge = [&](int id, const ImVec2& offset, const char* label, const char* sub, ImU32 activeColor) {
        bool isArmed = (armed == id);
        ImVec2 bp(c.x + offset.x, c.y + offset.y);
        ImVec2 textSize = ImGui::CalcTextSize(label);
        float w = (std::max)(textSize.x + 24.0f, 106.0f);
        float h = 32.0f;
        ImVec2 minP(bp.x - w * 0.5f, bp.y - h * 0.5f);
        ImVec2 maxP(bp.x + w * 0.5f, bp.y + h * 0.5f);

        // Check if mouse is hovering badge directly
        if (io.MousePos.x >= minP.x && io.MousePos.x <= maxP.x && io.MousePos.y >= minP.y && io.MousePos.y <= maxP.y) {
            isArmed = true;
            m_markingMenu.armedWedge = id;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                ExecuteMarkingMenuAction(id);
                m_markingMenu.active = false;
                m_markingMenu.armedWedge = -1;
                return;
            }
        }

        ImU32 bgCol = isArmed ? IM_COL32(24, 38, 52, 250) : IM_COL32(20, 22, 28, 230);
        ImU32 borderCol = isArmed ? activeColor : IM_COL32(70, 75, 88, 180);
        ImU32 textCol = isArmed ? activeColor : IM_COL32(230, 235, 245, 255);

        drawList->AddRectFilled(minP, maxP, bgCol, 5.0f);
        drawList->AddRect(minP, maxP, borderCol, 5.0f, 0, isArmed ? 2.5f : 1.0f);

        float textX = bp.x - textSize.x * 0.5f;
        float textY = bp.y - (sub ? 11.0f : 7.0f);
        drawList->AddText(ImVec2(textX + 1.0f, textY + 1.0f), IM_COL32(0, 0, 0, 220), label);
        drawList->AddText(ImVec2(textX, textY), textCol, label);

        if (sub) {
            ImVec2 subSize = ImGui::CalcTextSize(sub);
            float subX = bp.x - subSize.x * 0.5f;
            float subY = bp.y + 2.0f;
            drawList->AddText(ImVec2(subX + 1.0f, subY + 1.0f), IM_COL32(0, 0, 0, 180), sub);
            drawList->AddText(ImVec2(subX, subY), IM_COL32(150, 155, 170, 200), sub);
        }
    };

    RenderWedgeBadge(0, ImVec2(0.0f, -72.0f),   "CUT",            "^ Flick Up",     IM_COL32(0, 230, 255, 255));
    RenderWedgeBadge(1, ImVec2(0.0f, 72.0f),    "SEW",            "v Flick Down",   IM_COL32(255, 170, 20, 255));
    RenderWedgeBadge(2, ImVec2(118.0f, 0.0f),   "EDGE LOOP",      "> Flick Right",  IM_COL32(80, 170, 255, 255));
    RenderWedgeBadge(3, ImVec2(-126.0f, 0.0f),  "UNWRAP & PACK",  "< Flick Left",   IM_COL32(60, 230, 130, 255));
    RenderWedgeBadge(4, ImVec2(98.0f, -54.0f),  "EDGE RING",      "^> Flick NE",    IM_COL32(190, 130, 255, 255));
    RenderWedgeBadge(5, ImVec2(-116.0f, -54.0f), "CAMERA PROJ",   "<^ Flick NW",    IM_COL32(255, 140, 60, 255));
    RenderWedgeBadge(6, ImVec2(98.0f, 54.0f),   "AUTO UV",        "v> Flick SE",    IM_COL32(255, 220, 50, 255));
    RenderWedgeBadge(7, ImVec2(-116.0f, 54.0f), "CLEAR SEL",      "<v Flick SW",    IM_COL32(240, 90, 90, 255));

    // When RMB is released, execute action if armed!
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (m_markingMenu.armedWedge >= 0) {
            ExecuteMarkingMenuAction(m_markingMenu.armedWedge);
        }
        m_markingMenu.active = false;
        m_markingMenu.armedWedge = -1;
    }

    // Dismiss on Escape or LMB click outside
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && dist > 140.0f)) {
        m_markingMenu.active = false;
        m_markingMenu.armedWedge = -1;
    }
}

void UIManager::ExecuteMarkingMenuAction(int wedge) {
    if (!m_hasLowPoly) return;

    if (wedge == 0) { // CUT SEAMS
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            size_t count = m_lowPolyMesh.selectedEdges.size();
            UVCutter::CutSelectedEdges(m_lowPolyMesh);
            m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
            m_viewportRenderer.showSeams = true;
            PushHistoryState("Maya Cut UVs");
            char msg[128];
            snprintf(msg, sizeof(msg), "Cut %zu seam edge(s) via Maya gesture", count);
            m_uvStatusMessage = msg;
            m_logEntries.push_back(std::string("[Maya Cut] ") + msg);
        } else {
            m_uvStatusMessage = "No edges selected to cut (Select an edge or loop first)";
        }
    } else if (wedge == 1) { // SEW / STITCH
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            size_t count = m_lowPolyMesh.selectedEdges.size();
            UVCutter::SewSelectedEdges(m_lowPolyMesh);
            m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
            PushHistoryState("Maya Sew UVs");
            char msg[128];
            snprintf(msg, sizeof(msg), "Sewed %zu edge(s) via Maya gesture", count);
            m_uvStatusMessage = msg;
            m_logEntries.push_back(std::string("[Maya Sew] ") + msg);
        } else {
            m_uvStatusMessage = "No edges selected to sew";
        }
    } else if (wedge == 2) { // SELECT EDGE LOOP
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            auto currentEdges = m_lowPolyMesh.selectedEdges;
            for (const auto& edge : currentEdges) {
                UVCutter::SelectEdgeLoop(m_lowPolyMesh, edge.first, edge.second, true);
            }
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Maya Select Edge Loop");
            char msg[128];
            snprintf(msg, sizeof(msg), "Selected edge loop(s): %zu edges selected", m_lowPolyMesh.selectedEdges.size());
            m_uvStatusMessage = msg;
        } else {
            m_uvStatusMessage = "Click an edge to select an edge loop";
        }
    } else if (wedge == 3) { // UNWRAP & PACK
        auto islandsTris = UVCutter::SegmentIslands(m_lowPolyMesh);
        if (!islandsTris.empty()) {
            m_currentIslands = UVUnwrapper::UnwrapMesh(m_lowPolyMesh, islandsTris);
            m_packStats = UVPacker::PackIslands(m_currentIslands, m_packSettings);
            UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
            m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, 1.0f);
            m_hasUnwrappedIslands = true;
            PushHistoryState("Maya Unwrap & Pack");
            char msg[128];
            snprintf(msg, sizeof(msg), "Unwrapped & packed %zu UV islands (%.1f%% efficiency)",
                     m_currentIslands.size(), m_packStats.packingEfficiency * 100.0f);
            m_uvStatusMessage = msg;
            m_logEntries.push_back(std::string("[UV] ") + msg);
        } else {
            m_uvStatusMessage = "Mesh has no triangles to unwrap";
        }
    } else if (wedge == 4) { // SELECT EDGE RING
        if (!m_lowPolyMesh.selectedEdges.empty()) {
            auto currentEdges = m_lowPolyMesh.selectedEdges;
            for (const auto& edge : currentEdges) {
                UVCutter::SelectEdgeRing(m_lowPolyMesh, edge.first, edge.second, true);
            }
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Maya Select Edge Ring");
            char msg[128];
            snprintf(msg, sizeof(msg), "Selected edge ring(s): %zu edges selected", m_lowPolyMesh.selectedEdges.size());
            m_uvStatusMessage = msg;
        } else {
            m_uvStatusMessage = "Click an edge to select an edge ring";
        }
    } else if (wedge == 5) { // CAMERA PROJECTION
        DirectX::XMMATRIX viewProj = m_viewportRenderer.GetViewProjectionMatrix();
        UVCutter::CameraProjectUVs(m_lowPolyMesh, viewProj);
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, 1.0f);
        PushHistoryState("Maya Camera Projection");
        m_uvStatusMessage = "Projected UVs from current 3D camera view";
        m_logEntries.push_back("[UV] Camera Projection applied from current viewport view.");
    } else if (wedge == 6) { // AUTO UV
        AutoCutUnwrapAndPack();
    } else if (wedge == 7) { // CLEAR SELECTION
        m_lowPolyMesh.selectedEdges.clear();
        m_hasLastSelectedEdge = false;
        m_lowPolyMesh.GenerateSelectedEdgeLines();
        m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
        PushHistoryState("Maya Clear Selection");
        m_uvStatusMessage = "Selection cleared";
    }
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
    if (!ImGui::Begin("Mesh Ingestion###ingest", nullptr)) {
        ImGui::End();
        return;
    }

    // 1. High-Poly Source Card (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("HIGH-POLY SOURCE", "##hp_card_zb", false)) {
        std::string cacheHigh = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj";
        DWORD attrHP = GetFileAttributesA(cacheHigh.c_str());
        std::string syncHighLabel = "Sync from ZBrush Cache";
        if (attrHP != INVALID_FILE_ATTRIBUTES && !(attrHP & FILE_ATTRIBUTE_DIRECTORY)) {
            uintmax_t sz = 0;
            try { sz = std::filesystem::file_size(cacheHigh); } catch (...) {}
            char b[64];
            if (sz > 1024 * 1024 * 1024) {
                snprintf(b, sizeof(b), "Sync Active Sculpt (%.2f GB)", sz / (1024.0 * 1024.0 * 1024.0));
            } else if (sz > 0) {
                snprintf(b, sizeof(b), "Sync Active Sculpt (%.1f MB)", sz / (1024.0 * 1024.0));
            } else {
                snprintf(b, sizeof(b), "Sync from ZBrush Cache");
            }
            syncHighLabel = b;
        }

        if (BakerUI::PrimaryButton(syncHighLabel.c_str())) {
            AutoDetectZBrushCache(true);
        }
        if (BakerUI::SecondaryButton("Browse OBJ / FBX / BIN...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx;*.bin)\0*.obj;*.fbx;*.bin\0Wavefront OBJ (*.obj)\0*.obj\0Autodesk FBX (*.fbx)\0*.fbx\0Binary Mesh (*.bin)\0*.bin\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetCommandLineHighPoly(path);
            }
        }
        if (m_isLoadingHighPoly) {
            ImGui::Spacing();
            BakerUI::StatusPill("Ingesting in background...", BakerUI::Col::Warn());
            ImGui::TextDisabled("Parsing mesh & building BVH tree...");
        } else if (m_hasHighPoly) {
            ImGui::Spacing();
            if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
            ImGui::TextDisabled("%s", m_highPolyPath.c_str());
            ImGui::Text("Positions %zu", m_highPolyMesh.positions.size());
            if (m_highPolyMesh.isQuadMesh && !m_highPolyMesh.quadIndices.empty()) {
                ImGui::Text("Quads     %zu", m_highPolyMesh.quadIndices.size() / 4);
            } else {
                ImGui::Text("Triangles %zu", m_highPolyMesh.indices.size() / 3);
            }
            ImGui::Text("Diag      %.4f", m_highPolyMesh.GetBoundingBoxDiagonal());
            if (BakerUI::gFontMono) ImGui::PopFont();
            ImGui::Spacing();
            BakerUI::StatusPill("BVH ready (Embree 3)", BakerUI::Col::Ok());
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No high-poly loaded", BakerUI::Col::TextDim());
        }
        BakerUI::EndSectionCard();
    }

    // 2. Low-Poly Target Card (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("LOW-POLY TARGET (UDIM)", "##lp_card_zb", false)) {
        std::string cacheLow = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
        DWORD attrLP = GetFileAttributesA(cacheLow.c_str());
        if (attrLP != INVALID_FILE_ATTRIBUTES && !(attrLP & FILE_ATTRIBUTE_DIRECTORY)) {
            uintmax_t sz = 0;
            try { sz = std::filesystem::file_size(cacheLow); } catch (...) {}
            char b[64];
            snprintf(b, sizeof(b), "Sync Active Low-Poly (%.1f MB)", sz / (1024.0 * 1024.0));
            if (BakerUI::PrimaryButton(b)) {
                SetCommandLineLowPoly(cacheLow);
            }
        }
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
            if (m_lowPolyMesh.quadCount > 0 && m_lowPolyMesh.triCount == 0) {
                ImGui::Text("Quads     %u", m_lowPolyMesh.quadCount);
            } else if (m_lowPolyMesh.quadCount > 0) {
                ImGui::Text("Faces     %u (%u Quads, %u Tris)", m_lowPolyMesh.polyCount, m_lowPolyMesh.quadCount, m_lowPolyMesh.triCount);
            } else {
                ImGui::Text("Triangles %zu", m_lowPolyMesh.triangles.size());
            }
            ImGui::Text("Diag      %.4f", m_lowPolyMesh.GetBoundingBoxDiagonal());
            ImGui::Text("UDIMs     %zu tiles", m_lowPolyMesh.detectedUDIMs.size());
            if (BakerUI::gFontMono) ImGui::PopFont();
            ImGui::Spacing();
            BakerUI::StatusPill("Low-poly loaded", BakerUI::Col::Ok());
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No low-poly loaded", BakerUI::Col::Warn());
        }
        BakerUI::EndSectionCard();
    }

    // 3. Explicit Cage (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("EXPLICIT CAGE (OPTIONAL)", "##cage_card_zb", false)) {
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
        BakerUI::EndSectionCard();
    }

    if (m_hasLowPoly) {
        if (BakerUI::BeginSectionCard("TRANSFORM & ALIGNMENT", "##trans_card_zb", false)) {
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
            BakerUI::EndSectionCard();
        }
    }

    if (m_hasLowPoly && m_hasHighPoly) {
        if (BakerUI::BeginSectionCard("SCALE SYNCHRONIZATION", "##scale_card_zb", false)) {
            ImGui::Text("Scale Ratio: %.4fx", m_detectedScaleRatio);
            if (ImGui::Checkbox("Auto-Match Low-Poly Scale", &m_autoScaleLowPoly)) {
                CheckScaleAndAutoCalibrate();
            }
            if (BakerUI::SecondaryButton("Recalibrate Scale & Bounds")) {
                CheckScaleAndAutoCalibrate();
            }
            BakerUI::EndSectionCard();
        }
    }

    ImGui::End();
}

void UIManager::RenderSettingsPanel() {
    if (!ImGui::Begin("Bake Parameters###params", nullptr)) {
        ImGui::End();
        return;
    }

    static const char* kRes[] = { "1024", "2048", "4096", "8192 (8K)", "16384 (16K)" };
    static int currentResIdx = 1; // 2048

    static const char* kAA[] = { "1x (Draft)", "2x2 (4 Samples - Prod)", "4x4 (16 Samples - Max)" };
    static int currentAAIdx = 1; // 2x2

    float maxSliderDist = (m_hasHighPoly ? m_highPolyMesh.GetBoundingBoxDiagonal() * 0.5f : 10.0f);
    if (maxSliderDist < 1.0f) maxSliderDist = 1.0f;

    // 1. Projection & Quality (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("PROJECTION & QUALITY", "##proj_card_zb", false)) {
        if (BakerUI::BeginProps("##proj")) {
            if (BakerUI::PropCombo("Resolution", &currentResIdx, kRes, 5)) {
                int resMap[] = { 1024, 2048, 4096, 8192, 16384 };
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
        BakerUI::EndSectionCard();
    }

    // 2. Output Maps (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("OUTPUT MAPS (MARMOSET SUITE)", "##maps_card_zb", false)) {
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
            static const char* const kNormalFormats[] = { "OpenGL (+Y) (Blender/Maya)", "DirectX (-Y) (Unreal/Max)" };
            int curNormalFmt = m_settings.flipYNormal ? 1 : 0;
            if (BakerUI::PropCombo("Normal Format", &curNormalFmt, kNormalFormats, 2)) {
                m_settings.flipYNormal = (curNormalFmt == 1);
            }
            BakerUI::PropCheck("Compact BVH", &m_settings.useCompactBVH, "For dense 50M-100M poly sculpts");
            BakerUI::EndProps();
        }
        BakerUI::EndSectionCard();
    }

    // 3. Destination & Formats (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("DESTINATION & FORMATS", "##dest_card_zb", false)) {
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
        BakerUI::EndSectionCard();
    }

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

void UIManager::MarkSeamsByAngle() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByAngle(m_lowPolyMesh, m_seamAngleThreshold);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Mark Seams By Angle");
    char msg[128];
    snprintf(msg, sizeof(msg), "Marked %zu seams by angle (%.1f deg)", m_lowPolyMesh.cutSeams.size(), m_seamAngleThreshold);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void UIManager::MarkSeamsByPolygroup() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByPolygroup(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Mark Seams By Polygroup");
    char msg[128];
    snprintf(msg, sizeof(msg), "Marked %zu seams along Polygroup boundaries", m_lowPolyMesh.cutSeams.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void UIManager::MarkSeamsByUVBorders() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByUVBorders(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Mark Seams By UV Borders");
    char msg[128];
    snprintf(msg, sizeof(msg), "Detected %zu seams from existing UV borders", m_lowPolyMesh.cutSeams.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void UIManager::ClearSeams() {
    UVCutter::ClearSeams(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    PushHistoryState("Clear Seams");
    m_uvStatusMessage = "Cleared all seams.";
    m_logEntries.push_back("[UV Cut] Cleared all seams.");
}

void UIManager::UnwrapMesh() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    auto islandsTris = UVCutter::SegmentIslands(m_lowPolyMesh);
    if (islandsTris.empty()) {
        m_uvStatusMessage = "No islands segmented. Mark seams first or unwrap whole mesh.";
        return;
    }
    m_currentIslands = UVUnwrapper::UnwrapMesh(m_lowPolyMesh, islandsTris);
    m_hasUnwrappedIslands = true;
    PushHistoryState("Unwrap Mesh");
    char msg[128];
    snprintf(msg, sizeof(msg), "Unwrapped %zu islands successfully", m_currentIslands.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Unwrap] ") + msg);
}

void UIManager::PackUVs() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    if (!m_hasUnwrappedIslands || m_currentIslands.empty()) {
        UnwrapMesh();
    }
    if (m_currentIslands.empty()) {
        m_uvStatusMessage = "No islands to pack.";
        return;
    }
    m_packStats = UVPacker::PackIslands(m_currentIslands, m_packSettings);
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);

    m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;
    PushHistoryState("Pack UVs");

    char msg[256];
    snprintf(msg, sizeof(msg), "Packed %d islands across %d tile(s). Efficiency: %.1f%%",
        m_packStats.packedIslands, m_packStats.udimTilesUsed, m_packStats.packingEfficiency * 100.0f);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Pack] ") + msg);
}

void UIManager::AutoCutUnwrapAndPack() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    // 1. Mark seams by angle
    UVCutter::MarkSeamsByAngle(m_lowPolyMesh, m_seamAngleThreshold);
    if (m_packSettings.packByPolygroup) {
        UVCutter::MarkSeamsByPolygroup(m_lowPolyMesh);
    }
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;

    // 2. Segment islands
    auto islandsTris = UVCutter::SegmentIslands(m_lowPolyMesh);

    // 3. Conformal unwrap
    m_currentIslands = UVUnwrapper::UnwrapMesh(m_lowPolyMesh, islandsTris);
    m_hasUnwrappedIslands = true;

    // 4. MaxRects BSSF multi-rotation packing
    m_packStats = UVPacker::PackIslands(m_currentIslands, m_packSettings);
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);

    m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;
    PushHistoryState("Auto Cut, Unwrap & Pack");

    char msg[256];
    snprintf(msg, sizeof(msg), "Auto Cut & Pack complete! %d islands packed across %d tile(s) (%.1f%% efficiency)",
        m_packStats.packedIslands, m_packStats.udimTilesUsed, m_packStats.packingEfficiency * 100.0f);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Auto] ") + msg);
}

void UIManager::ExportUnwrappedOBJ() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "No low-poly mesh to export.";
        return;
    }
    std::string outPath = m_lowPolyPath;
    if (outPath.empty()) {
        outPath = "./Unwrapped_LowPoly.obj";
    } else {
        std::filesystem::path p(outPath);
        p.replace_filename(p.stem().string() + "_Unwrapped.obj");
        outPath = p.string();
    }
    std::string err;
    if (MeshLoader::SaveLowPolyOBJWithNormals(outPath, m_lowPolyMesh, err)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Saved unwrapped low-poly OBJ to: %s", outPath.c_str());
        m_uvStatusMessage = msg;
        m_logEntries.push_back(std::string("[Export] ") + msg);
    } else {
        m_uvStatusMessage = "Failed to export OBJ: " + err;
        m_logEntries.push_back("[Error] " + m_uvStatusMessage);
    }
}

void UIManager::SnapActiveCurveToSeams() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    SurfaceCurve* c = m_curveManager.GetActiveCurve();
    if (!c || c->points.size() < 2) {
        m_uvStatusMessage = "Active curve needs at least 2 points.";
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> seamEdges;
    CurveManager::SnapCurveToMeshEdges(*c, m_lowPolyMesh, seamEdges);
    c->isBaked = true;
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Snap Curve To Seams");
    char msg[128];
    snprintf(msg, sizeof(msg), "Snapped %s to %zu seam edges on mesh.", c->name.c_str(), seamEdges.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[Curve Cut] ") + msg);
}

void UIManager::CutActiveCurveIntoTopology() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    SurfaceCurve* c = m_curveManager.GetActiveCurve();
    if (!c || c->points.size() < 2) {
        m_uvStatusMessage = "Active curve needs at least 2 points.";
        return;
    }
    std::vector<std::pair<uint32_t, uint32_t>> seamEdges;
    CurveManager::CutCurveIntoMeshTopology(*c, m_lowPolyMesh, seamEdges);
    c->isBaked = true;
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    m_curveManager.AddCurve();
    PushHistoryState("Cut Curve Into Topology");
    char msg[128];
    snprintf(msg, sizeof(msg), "Cut %s into mesh topology (%zu new seam edges). Ready for next curve.", c->name.c_str(), seamEdges.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[Curve Cut] ") + msg);
}

void UIManager::BakeAllCurvesToSeams(bool cutIntoTopology) {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    bool anyBaked = CurveManager::BakeCurvesToSeams(m_curveManager, m_lowPolyMesh, cutIntoTopology, -1);
    if (cutIntoTopology) {
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    }
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Bake All Curves To Seams");
    char msg[128];
    snprintf(msg, sizeof(msg), "Baked all curves into mesh seams (%s). Total seams: %zu",
        cutIntoTopology ? "topological cut" : "edge snapped", m_lowPolyMesh.cutSeams.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[Curve Cut] ") + msg);
}

void UIManager::ClearActiveCurve() {
    SurfaceCurve* c = m_curveManager.GetActiveCurve();
    if (c) {
        c->Clear();
        PushHistoryState("Clear Curve");
        m_uvStatusMessage = "Cleared points on active curve.";
        m_logEntries.push_back("[Curve Cut] Cleared points on active curve.");
    }
}

void UIManager::ClearAllCurves() {
    m_curveManager.ClearAll();
    PushHistoryState("Clear All Curves");
    m_uvStatusMessage = "Cleared all curves.";
    m_logEntries.push_back("[Curve Cut] Cleared all curves.");
}

void UIManager::CalculateTexelDensity() {
    if (!m_hasLowPoly || m_lowPolyMesh.triangles.empty()) {
        m_calculatedTexelDensity = 0.0f;
        return;
    }
    double total3DArea = 0.0;
    double totalUVArea = 0.0;

    for (const auto& tri : m_lowPolyMesh.triangles) {
        if (tri.indices[0] >= m_lowPolyMesh.vertices.size() ||
            tri.indices[1] >= m_lowPolyMesh.vertices.size() ||
            tri.indices[2] >= m_lowPolyMesh.vertices.size()) continue;

        const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
        const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
        const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];

        // 3D area via cross product
        Vec3 e1 = { v1.position.x - v0.position.x, v1.position.y - v0.position.y, v1.position.z - v0.position.z };
        Vec3 e2 = { v2.position.x - v0.position.x, v2.position.y - v0.position.y, v2.position.z - v0.position.z };
        Vec3 cp = { e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
        float a3d = 0.5f * std::sqrt(cp.x * cp.x + cp.y * cp.y + cp.z * cp.z);
        total3DArea += a3d;

        // 2D UV area via cross product
        float u1 = v1.uv.u - v0.uv.u, v1_ = v1.uv.v - v0.uv.v;
        float u2 = v2.uv.u - v0.uv.u, v2_ = v2.uv.v - v0.uv.v;
        float aUV = 0.5f * std::abs(u1 * v2_ - v1_ * u2);
        totalUVArea += aUV;
    }

    if (total3DArea > 1e-6) {
        float res = (float)m_packSettings.resolution;
        m_calculatedTexelDensity = (float)((std::sqrt(totalUVArea) * res) / std::sqrt(total3DArea));
    } else {
        m_calculatedTexelDensity = 0.0f;
    }
}

void UIManager::SetTargetTexelDensity(float targetTD) {
    if (!m_hasLowPoly) return;
    CalculateTexelDensity();
    if (m_calculatedTexelDensity < 1e-4f || targetTD < 1e-4f) return;

    float scale = targetTD / m_calculatedTexelDensity;
    for (auto& v : m_lowPolyMesh.vertices) {
        float tileU = std::floor(v.uv.u);
        float tileV = std::floor(v.uv.v);
        float localU = v.uv.u - tileU;
        float localV = v.uv.v - tileV;
        v.uv.u = tileU + localU * scale;
        v.uv.v = tileV + localV * scale;
    }
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    CalculateTexelDensity();
    PushHistoryState("Set Texel Density");
    char buf[128];
    snprintf(buf, sizeof(buf), "Texel density set to %.2f px/cm", m_calculatedTexelDensity);
    m_uvStatusMessage = buf;
    m_logEntries.push_back(std::string("[Texel Density] ") + buf);
}

void UIManager::EqualizeTexelDensity() {
    if (!m_hasLowPoly) return;
    CalculateTexelDensity();
    m_packSettings.normalizeTexelDensity = true;
    PackUVs();
    m_uvStatusMessage = "Equalized texel density across all islands.";
    m_logEntries.push_back("[Texel Density] Equalized texel density across all UV shells.");
}

void UIManager::StackIdenticalIslands() {
    if (!m_hasLowPoly) return;
    if (m_currentIslands.empty()) {
        UnwrapMesh();
    }
    if (m_currentIslands.size() < 2) return;

    int stackedCount = 0;
    for (size_t i = 0; i < m_currentIslands.size(); ++i) {
        for (size_t j = i + 1; j < m_currentIslands.size(); ++j) {
            auto& a = m_currentIslands[i];
            auto& b = m_currentIslands[j];
            if (a.triangleIndices.size() == b.triangleIndices.size()) {
                float aW = a.bounds.Width(), aH = a.bounds.Height();
                float bW = b.bounds.Width(), bH = b.bounds.Height();
                if (std::abs(aW - bW) < 0.02f && std::abs(aH - bH) < 0.02f) {
                    float du = a.bounds.minU - b.bounds.minU;
                    float dv = a.bounds.minV - b.bounds.minV;
                    for (auto& uv : b.uvs) {
                        uv.u += du;
                        uv.v += dv;
                    }
                    b.ComputeBounds();
                    stackedCount++;
                }
            }
        }
    }
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Stack Similar Islands");
    char buf[128];
    snprintf(buf, sizeof(buf), "Stacked %d similar island pair(s)", stackedCount);
    m_uvStatusMessage = buf;
    m_logEntries.push_back(std::string("[ZenUV Stacks] ") + buf);
}

void UIManager::SeparateOverlappingIslands() {
    if (!m_hasLowPoly) return;
    if (m_currentIslands.empty()) {
        UnwrapMesh();
    }
    int separatedCount = 0;
    for (size_t i = 0; i < m_currentIslands.size(); ++i) {
        for (size_t j = i + 1; j < m_currentIslands.size(); ++j) {
            auto& a = m_currentIslands[i];
            auto& b = m_currentIslands[j];
            if (std::abs(a.bounds.minU - b.bounds.minU) < 0.001f &&
                std::abs(a.bounds.minV - b.bounds.minV) < 0.001f) {
                for (auto& uv : b.uvs) {
                    uv.u += 1.0f;
                }
                b.ComputeBounds();
                separatedCount++;
            }
        }
    }
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Separate Overlapping Islands");
    char buf[128];
    snprintf(buf, sizeof(buf), "Offset %d overlapping island(s) to tile +1", separatedCount);
    m_uvStatusMessage = buf;
    m_logEntries.push_back(std::string("[UVPackmaster] ") + buf);
}

void UIManager::StraightenIslands() {
    if (!m_hasLowPoly) return;
    if (m_currentIslands.empty()) {
        UnwrapMesh();
    }
    for (auto& isl : m_currentIslands) {
        if (isl.uvs.size() < 4) continue;
        float cu = 0.5f * (isl.bounds.minU + isl.bounds.maxU);
        float cv = 0.5f * (isl.bounds.minV + isl.bounds.maxV);
        float covXX = 0, covXY = 0, covYY = 0;
        for (const auto& uv : isl.uvs) {
            float dx = uv.u - cu;
            float dy = uv.v - cv;
            covXX += dx * dx;
            covXY += dx * dy;
            covYY += dy * dy;
        }
        float angle = 0.5f * std::atan2(2.0f * covXY, covXX - covYY);
        float cosA = std::cos(-angle);
        float sinA = std::sin(-angle);
        for (auto& uv : isl.uvs) {
            float dx = uv.u - cu;
            float dy = uv.v - cv;
            uv.u = cu + dx * cosA - dy * sinA;
            uv.v = cv + dx * sinA + dy * cosA;
        }
        isl.ComputeBounds();
    }
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Straighten Islands");
    m_uvStatusMessage = "Straightened islands to UV axes.";
    m_logEntries.push_back("[ZenUV Reshape] Straightened islands to UV grid axes.");
}

void UIManager::OrientIslandsToWorld() {
    if (!m_hasLowPoly) return;
    if (m_currentIslands.empty()) {
        UnwrapMesh();
    }
    for (auto& isl : m_currentIslands) {
        float cosA = 0.0f, sinA = 1.0f;
        float cu = 0.5f * (isl.bounds.minU + isl.bounds.maxU);
        float cv = 0.5f * (isl.bounds.minV + isl.bounds.maxV);
        for (auto& uv : isl.uvs) {
            float dx = uv.u - cu;
            float dy = uv.v - cv;
            uv.u = cu + dx * cosA - dy * sinA;
            uv.v = cv + dx * sinA + dy * cosA;
        }
        isl.ComputeBounds();
    }
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Orient to World");
    m_uvStatusMessage = "Oriented islands to world coordinates.";
    m_logEntries.push_back("[ZenUV World Orient] Oriented UV islands to 3D world space.");
}

void UIManager::RenderUVCutAndPackPanel() {
    if (!ImGui::Begin("UV Tool###uvpanel", &m_shellState.showUVPanel)) {
        ImGui::End();
        return;
    }

    // 1. Maya UV Workflow Gestures & Status Card (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("MAYA UV CUTTING (GESTURE-DRIVEN)", "##maya_card_zb", false)) {
        ImGui::TextColored(ImVec4(0.0f, 0.95f, 1.0f, 1.0f), "Viewport & 2D Canvas Gestures:");
        ImGui::BulletText("Double-Click Edge: Select Edge Loop (Collinear)");
        ImGui::BulletText("Shift + Click Edge: Select Shortest Path between edges");
        ImGui::BulletText("Shift + Right-Click: Radial Marking Menu");
        ImGui::Indent();
        ImGui::TextDisabled("Wedges: Cut (Up), Sew (Down), Loop (Right), Unwrap (Left)");
        ImGui::TextDisabled("Ring (NE), Camera Proj (NW), Auto UV (SE), Clear (SW)");
        ImGui::Unindent();
        ImGui::BulletText("Click Empty Space: Clear edge selection");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Selected Edges: %zu  |  Marked Seams: %zu", m_lowPolyMesh.selectedEdges.size(), m_lowPolyMesh.cutSeams.size());
        if (!m_uvStatusMessage.empty()) {
            ImGui::Spacing();
            BakerUI::StatusPill(m_uvStatusMessage.c_str(), BakerUI::Col::Accent());
        }
        BakerUI::EndSectionCard();
    }

    // 2b. 3D Surface Curves (Interactive Surface Cutting)
    if (BakerUI::BeginSectionCard("3D SURFACE CURVES", "##curves_card_zb", false)) {
        ImGui::TextWrapped("Click anywhere on the low-poly mesh in 3D to draw cutting curves. Move control points dynamically or bake curves into seams.");
        ImGui::Spacing();

        // Curve selection combo
        std::vector<std::string> curveNames;
        std::vector<const char*> curveNamePtrs;
        for (size_t i = 0; i < m_curveManager.curves.size(); ++i) {
            char nameBuf[128];
            snprintf(nameBuf, sizeof(nameBuf), "%s (%zu pts)%s",
                m_curveManager.curves[i].name.c_str(),
                m_curveManager.curves[i].points.size(),
                m_curveManager.curves[i].isBaked ? " [Baked]" : "");
            curveNames.push_back(nameBuf);
        }
        for (const auto& s : curveNames) curveNamePtrs.push_back(s.c_str());

        int curCurve = m_curveManager.activeCurveIndex;
        if (ImGui::Combo("Select Curve", &curCurve, curveNamePtrs.data(), static_cast<int>(curveNamePtrs.size()))) {
            m_curveManager.activeCurveIndex = curCurve;
        }

        // Add / Delete / Clear Buttons
        if (ImGui::Button("+ New Curve")) {
            m_curveManager.AddCurve();
            m_toolMode = ToolMode::DrawCurve;
            m_viewportRenderer.showSeams = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("- Delete")) {
            m_curveManager.RemoveCurve(m_curveManager.activeCurveIndex);
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear All Curves")) {
            ClearAllCurves();
        }

        ImGui::Spacing();
        bool isCurveMode = (m_toolMode == ToolMode::DrawCurve);
        if (BakerUI::ToggleChip(isCurveMode ? "Drawing Curve in 3D (Click Mesh)" : "Enable 3D Curve Draw Mode", &isCurveMode)) {
            m_toolMode = isCurveMode ? ToolMode::DrawCurve : ToolMode::Navigate;
            if (m_toolMode == ToolMode::DrawCurve) {
                m_viewportRenderer.showSeams = true;
            }
        }

        SurfaceCurve* c = m_curveManager.GetActiveCurve();
        if (c) {
            ImGui::Checkbox("Close Curve (Loop)", &c->isClosed);
            ImGui::SameLine();
            ImGui::TextDisabled("| %zu points", c->points.size());

            // Points table
            if (!c->points.empty() && ImGui::TreeNodeEx("Control Points List", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("##c_pts_zb", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 25.0f);
                    ImGui::TableSetupColumn("Pos (X, Y, Z)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Select", ImGuiTableColumnFlags_WidthFixed, 45.0f);
                    ImGui::TableSetupColumn("Del", ImGuiTableColumnFlags_WidthFixed, 35.0f);
                    ImGui::TableHeadersRow();

                    for (size_t p = 0; p < c->points.size(); ++p) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%zu", p);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%.2f, %.2f, %.2f", c->points[p].position.x, c->points[p].position.y, c->points[p].position.z);

                        ImGui::TableSetColumnIndex(2);
                        char selId[32]; snprintf(selId, sizeof(selId), "Sel##p%zu", p);
                        bool isSel = (m_curveManager.selectedPointIndex == static_cast<int>(p));
                        if (ImGui::SmallButton(selId)) {
                            m_curveManager.selectedPointIndex = static_cast<int>(p);
                        }

                        ImGui::TableSetColumnIndex(3);
                        char delId[32]; snprintf(delId, sizeof(delId), "X##p%zu", p);
                        if (ImGui::SmallButton(delId)) {
                            m_curveManager.RemoveActivePoint(static_cast<int>(p));
                            break;
                        }
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }
        }

        // Edit Curve Controls (Smooth & Straighten between points)
        if (c && c->points.size() >= 2) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            BakerUI::SectionLabel("EDIT ACTIVE CURVE (SMOOTH & STRAIGHTEN)");

            int ptCountInt = static_cast<int>(c->points.size());
            int p0 = (m_curveManager.selectedPointIndex >= 0) ? m_curveManager.selectedPointIndex : 0;
            if (p0 >= ptCountInt) p0 = ptCountInt - 1;
            int defaultP1 = (p0 + 1 < ptCountInt) ? (p0 + 1) : (c->isClosed ? 0 : p0);
            int p1 = (m_curveManager.selectedPointEndIndex >= 0) ? m_curveManager.selectedPointEndIndex : defaultP1;
            if (p1 >= ptCountInt) p1 = ptCountInt - 1;

            if (BakerUI::BeginProps("##curve_edit_props_zb")) {
                BakerUI::PropInt("From Point (P#)", &p0, 0, ptCountInt - 1, "Start control point of segment to edit");
                BakerUI::PropInt("To Point (P#)", &p1, 0, ptCountInt - 1, "End control point of segment to edit");
                BakerUI::PropInt("Smooth Iterations", &m_curveManager.smoothIterations, 1, 50, "Laplacian relaxation passes");
                BakerUI::PropFloat("Smooth Factor", &m_curveManager.smoothFactor, 0.01f, 0.05f, 1.0f, "%.2f", "Smoothing blend weight (0.50 recommended)");
                BakerUI::EndProps();
            }
            m_curveManager.selectedPointIndex = p0;
            m_curveManager.selectedPointEndIndex = p1;

            ImGui::Spacing();
            char straightBtn[64];
            snprintf(straightBtn, sizeof(straightBtn), "STRAIGHTEN SEGMENT (P%d -> P%d)", p0, p1);
            if (BakerUI::SecondaryButton(straightBtn)) {
                PushHistoryState("Straighten Curve Segment");
                c->StraightenSegment(p0, p1, m_lowPolyMesh);
                char msg[128];
                snprintf(msg, sizeof(msg), "Straightened segment between P%d and P%d.", p0, p1);
                m_uvStatusMessage = msg;
                m_logEntries.push_back(std::string("[Curve Edit] ") + msg);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Replaces sampled path between P%d and P%d with a direct straight chord", p0, p1);

            ImGui::Spacing();
            char smoothBtn[64];
            snprintf(smoothBtn, sizeof(smoothBtn), "SMOOTH SEGMENT (P%d -> P%d)", p0, p1);
            if (BakerUI::PrimaryButton(smoothBtn)) {
                PushHistoryState("Smooth Curve Segment");
                c->SmoothSegment(p0, p1, m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
                char msg[128];
                snprintf(msg, sizeof(msg), "Smoothed segment between P%d and P%d (%d iterations).", p0, p1, m_curveManager.smoothIterations);
                m_uvStatusMessage = msg;
                m_logEntries.push_back(std::string("[Curve Edit] ") + msg);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smooths out kinks, bends, and background loops between P%d and P%d using Laplacian relaxation", p0, p1);

            ImGui::Spacing();
            if (ImGui::Button("Smooth Entire Curve")) {
                PushHistoryState("Smooth Entire Curve");
                c->SmoothEntireCurve(m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
                m_uvStatusMessage = "Smoothed entire curve.";
                m_logEntries.push_back("[Curve Edit] Smoothed entire curve.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Straighten All")) {
                PushHistoryState("Straighten Entire Curve");
                c->StraightenEntireCurve(m_lowPolyMesh);
                m_uvStatusMessage = "Straightened all segments on curve.";
                m_logEntries.push_back("[Curve Edit] Straightened all segments on curve.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Recalculate Camera Path")) {
                c->ResetSegmentCustomization(0, c->points.size());
                DirectX::XMMATRIX worldMat = m_viewportRenderer.GetLowPolyWorldMatrix();
                DirectX::XMMATRIX viewProjMat = m_viewportRenderer.GetViewProjectionMatrix();
                int vpW = m_viewportRenderer.GetWidth();
                int vpH = m_viewportRenderer.GetHeight();
                c->UpdateSampledPath(m_lowPolyMesh, &worldMat, &viewProjMat, vpW, vpH);
                m_uvStatusMessage = "Recalculated sampled curve path from camera.";
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Convert / Bake Actions
        if (BakerUI::PrimaryButton("SNAP CURVE TO EDGES & CUT SEAMS")) {
            SnapActiveCurveToSeams();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Traces the curve along low-poly mesh edges using Dijkstra shortest surface paths");

        ImGui::Spacing();
        if (BakerUI::SecondaryButton("CUT CURVE INTO MESH TOPOLOGY")) {
            CutActiveCurveIntoTopology();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Bisects and splits triangle faces directly along the curve trajectory, inserting vertices & seams exactly along the line");

        ImGui::Spacing();
        if (ImGui::Button("Bake All Curves (Snap Edges)")) {
            BakeAllCurvesToSeams(false);
        }
        ImGui::SameLine();
        if (ImGui::Button("Bake All (Cut Topology)")) {
            BakeAllCurvesToSeams(true);
        }
        BakerUI::EndSectionCard();
    }

    // 3. Unwrap Section (Blender Collapsible Section Card, packed by default)
    if (BakerUI::BeginSectionCard("CONFORMAL UNWRAP & AUTO-SEAMS", "##unwrap_card_zb", false)) {
        if (BakerUI::BeginProps("##unwrap_props_zb")) {
            BakerUI::PropFloat("Seam Angle Crease", &m_seamAngleThreshold, 1.0f, 15.0f, 90.0f, "%.1f deg", "Auto mark seams on sharp angle edges");
            BakerUI::PropInt("Spring Iterations", &m_relaxIterations, 5, 200, "Relaxation passes to minimize texture distortion");
            BakerUI::EndProps();
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Unwrap Islands (Conformal + Relax)")) {
            UnwrapMesh();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Auto Seams & Unwrap")) {
            UVCutter::MarkSeamsByAngle(m_lowPolyMesh, m_seamAngleThreshold);
            m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
            m_viewportRenderer.showSeams = true;
            UnwrapMesh();
        }
        ImGui::Spacing();
        if (BakerUI::PrimaryButton("1-Click Auto Cut, Unwrap & Pack")) {
            AutoCutUnwrapAndPack();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Islands: %zu", m_currentIslands.size());
        BakerUI::EndSectionCard();
    }

    // 4. Texel Density Toolkit (ZenUV & UVPackmaster Parity)
    if (BakerUI::BeginSectionCard("TEXEL DENSITY (ZENUV & UVPM)", "##td_card_zb", false)) {
        if (BakerUI::BeginProps("##td_props_zb")) {
            BakerUI::PropFloat("Target TD (px/cm)", &m_targetTexelDensity, 0.5f, 0.1f, 100.0f, "%.2f px/cm", "Desired texel density per world centimetre");
            if (m_calculatedTexelDensity > 0.0f) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("Current TD");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.3f, 0.85f, 0.7f, 1.0f), "%.2f px/cm", m_calculatedTexelDensity);
            }
            BakerUI::EndProps();
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Calculate Current TD")) {
            CalculateTexelDensity();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Apply Target TD")) {
            SetTargetTexelDensity(m_targetTexelDensity);
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Equalize Island Density")) {
            EqualizeTexelDensity();
        }
        BakerUI::EndSectionCard();
    }

    // 5. Island Stacking & Alignment (ZenUV Stacks & Reshape)
    if (BakerUI::BeginSectionCard("STACKING & ALIGNMENT (ZENUV STACKS)", "##stack_card_zb", false)) {
        if (BakerUI::BeginProps("##stack_props_zb")) {
            BakerUI::PropCheck("Lock Overlapping", &m_lockOverlapping, "Treat stacked identical islands as a single unit during pack");
            BakerUI::EndProps();
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Stack Similar Islands")) {
            StackIdenticalIslands();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Separate Overlaps (+1 Tile)")) {
            SeparateOverlappingIslands();
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Straighten Islands (Axes)")) {
            StraightenIslands();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Orient to World Up (+Y)")) {
            OrientIslandsToWorld();
        }
        BakerUI::EndSectionCard();
    }

    // 6. Advanced Packing Section (UVPackmaster 4 Inspired)
    if (BakerUI::BeginSectionCard("ADVANCED PACKING (UVPACKMASTER 4)", "##pack_card_zb", false)) {
        static const char* kResOptions[] = { "512", "1024", "2048", "4096", "8192", "16384" };
        static int resIdx = 2; // 2048
        if (BakerUI::BeginProps("##pack_props_zb")) {
            if (BakerUI::PropCombo("Target Resolution", &resIdx, kResOptions, IM_ARRAYSIZE(kResOptions))) {
                if (resIdx == 0) m_packSettings.resolution = 512;
                else if (resIdx == 1) m_packSettings.resolution = 1024;
                else if (resIdx == 2) m_packSettings.resolution = 2048;
                else if (resIdx == 3) m_packSettings.resolution = 4096;
                else if (resIdx == 4) m_packSettings.resolution = 8192;
                else if (resIdx == 5) m_packSettings.resolution = 16384;
            }

            BakerUI::PropInt("Island Padding (px)", &m_packSettings.paddingPixels, 0, 64, "Spacing between UV islands in pixels");
            BakerUI::PropInt("Tile Margin (px)", &m_packSettings.marginPixels, 0, 64, "Margin from the outer UDIM border in pixels");

            static const char* kRotOptions[] = { "None (0 deg)", "90 deg Steps (4x)", "45 deg Steps (8x)", "Free Rotation (32x)" };
            int rotIdx = (int)m_packSettings.rotation;
            if (BakerUI::PropCombo("Rotation Search", &rotIdx, kRotOptions, IM_ARRAYSIZE(kRotOptions))) {
                m_packSettings.rotation = (PackSettings::RotationMode)rotIdx;
            }

            BakerUI::PropCheck("Normalize Texel Density", &m_packSettings.normalizeTexelDensity, "Equalizes UV area relative to 3D world surface area");
            BakerUI::PropCheck("Multi-UDIM Packing", &m_packSettings.multiUDIM, "Automatically pack across tiles 1001, 1002, 1003...");
            if (m_packSettings.multiUDIM) {
                BakerUI::PropInt("Max UDIM Tiles", &m_packSettings.maxUDIMCount, 1, 10, "Maximum number of UDIM tiles allowed");
            }
            BakerUI::PropCheck("Pack by Polygroup", &m_packSettings.packByPolygroup, "Keep islands of the same polygroup grouped on dedicated tiles");

            BakerUI::EndProps();
        }

        ImGui::Spacing();
        if (BakerUI::PrimaryButton("PACK UVs (MaxRects BSSF)")) {
            PackUVs();
        }
        BakerUI::EndSectionCard();
    }

    // 5. Packing Statistics Card
    if (m_packStats.packedIslands > 0) {
        if (BakerUI::BeginSectionCard("PACKING METRICS", "##stats_card_zb", false)) {
            ImGui::Text("Packed Islands: %d", m_packStats.packedIslands);
            ImGui::Text("UDIM Tiles Used: %d", m_packStats.udimTilesUsed);
            ImGui::Text("Packing Efficiency: %.1f%%", m_packStats.packingEfficiency * 100.0f);
            float udimProg = m_packStats.packingEfficiency;
            ImGui::ProgressBar(udimProg, ImVec2(-FLT_MIN, 0.0f));
            BakerUI::EndSectionCard();
        }
    }

    // 6. Export Card
    if (BakerUI::BeginSectionCard("EXPORT MESH", "##exp_card_zb", false)) {
        if (BakerUI::SecondaryButton("Export Low-Poly OBJ (with New UVs)...")) {
            ExportUnwrappedOBJ();
        }
        BakerUI::EndSectionCard();
    }

    ImGui::End();
}

void UIManager::RenderUDIMCanvasPanel() {
    if (!ImGui::Begin("UDIM Layout###udim", &m_shellState.showUdim)) {
        ImGui::End();
        return;
    }

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
    if (!ImGui::Begin("Output Log###log", &m_shellState.showLog)) {
        ImGui::End();
        return;
    }

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

        auto livePreviewCb = [this](int tile, int width, int height, const float* r, const float* g, const float* b) {
            std::lock_guard<std::mutex> lock(this->m_normalMutex);
            size_t count = static_cast<size_t>(width) * height;
            if (this->m_stagedNormalR.size() != count) {
                this->m_stagedNormalR.resize(count);
                this->m_stagedNormalG.resize(count);
                this->m_stagedNormalB.resize(count);
            }
            std::memcpy(this->m_stagedNormalR.data(), r, count * sizeof(float));
            std::memcpy(this->m_stagedNormalG.data(), g, count * sizeof(float));
            std::memcpy(this->m_stagedNormalB.data(), b, count * sizeof(float));
            this->m_stagedNormalW = width;
            this->m_stagedNormalH = height;
            this->m_pendingNormalUpload = true;
        };

        auto startTime = std::chrono::high_resolution_clock::now();

        bool success = m_engine.ExecuteBake(m_lowPolyMesh, m_settings, results, progressCb, err, livePreviewCb);

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

            std::vector<std::string> allSavedFiles;

            for (const auto& res : results) {
                std::string udimStr = std::to_string(res.udimTile);

                // 1. EXR Formats (16-bit half / 32-bit float)
                if (m_settings.exportEXR) {
                    if (m_settings.bakeDisplacement && !res.displacementMap.empty()) {
                        std::string dispPath = (outFolder / (m_settings.baseFilename + "_Disp." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(dispPath, res.displacementMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 32-bit displacement: " + dispPath);
                            allSavedFiles.push_back(dispPath);
                        } else {
                            m_logEntries.push_back("[Error] Failed writing " + dispPath + ": " + exrErr);
                        }
                    }

                    if (m_settings.bakeNormal && !res.normalMapR.empty()) {
                        std::string normPath = (outFolder / (m_settings.baseFilename + "_Normal." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(normPath, res.normalMapR.data(), res.normalMapG.data(), res.normalMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit normal: " + normPath);
                            allSavedFiles.push_back(normPath);
                        }
                    }

                    if (m_settings.bakeWorldNormal && !res.worldNormalR.empty()) {
                        std::string wnormPath = (outFolder / (m_settings.baseFilename + "_WorldNormal." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(wnormPath, res.worldNormalR.data(), res.worldNormalG.data(), res.worldNormalB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit world normal: " + wnormPath);
                            allSavedFiles.push_back(wnormPath);
                        }
                    }

                    if (m_settings.bakeAmbientOcclusion && !res.aoMap.empty()) {
                        std::string aoPath = (outFolder / (m_settings.baseFilename + "_AO." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(aoPath, res.aoMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit AO map: " + aoPath);
                            allSavedFiles.push_back(aoPath);
                        }
                    }

                    if (m_settings.bakeCavity && !res.cavityMap.empty()) {
                        std::string cavPath = (outFolder / (m_settings.baseFilename + "_Cavity." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(cavPath, res.cavityMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit cavity: " + cavPath);
                            allSavedFiles.push_back(cavPath);
                        }
                    }

                    if (m_settings.bakeCurvature && !res.curvatureMap.empty()) {
                        std::string curvPath = (outFolder / (m_settings.baseFilename + "_Curvature." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(curvPath, res.curvatureMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit curvature: " + curvPath);
                            allSavedFiles.push_back(curvPath);
                        }
                    }

                    if (m_settings.bakeThickness && !res.thicknessMap.empty()) {
                        std::string thickPath = (outFolder / (m_settings.baseFilename + "_Thickness." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveFloatImage(thickPath, res.thicknessMap.data(), res.width, res.height, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit thickness: " + thickPath);
                            allSavedFiles.push_back(thickPath);
                        }
                    }

                    if (m_settings.bakePosition && !res.positionMapR.empty()) {
                        std::string posPath = (outFolder / (m_settings.baseFilename + "_Position." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(posPath, res.positionMapR.data(), res.positionMapG.data(), res.positionMapB.data(), res.width, res.height, false, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 32-bit world position: " + posPath);
                            allSavedFiles.push_back(posPath);
                        }
                    }

                    if (m_settings.bakeMaterialID && !res.materialIdMapR.empty()) {
                        std::string matPath = (outFolder / (m_settings.baseFilename + "_MaterialID." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(matPath, res.materialIdMapR.data(), res.materialIdMapG.data(), res.materialIdMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit material ID: " + matPath);
                            allSavedFiles.push_back(matPath);
                        }
                    }

                    if (m_settings.bakeBaseColor && !res.baseColorMapR.empty()) {
                        std::string colPath = (outFolder / (m_settings.baseFilename + "_BaseColor." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(colPath, res.baseColorMapR.data(), res.baseColorMapG.data(), res.baseColorMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved 16-bit base color: " + colPath);
                            allSavedFiles.push_back(colPath);
                        }
                    }

                    if (m_settings.bakeDiagnosticMap && !res.diagnosticMapR.empty()) {
                        std::string diagPath = (outFolder / (m_settings.baseFilename + "_Diag." + udimStr + ".exr")).string();
                        std::string exrErr;
                        if (ExrWriter::SaveRGBFloatImage(diagPath, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, true, exrErr)) {
                            m_logEntries.push_back("[EXR] Saved diagnostic map: " + diagPath);
                            allSavedFiles.push_back(diagPath);
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
                            allSavedFiles.push_back(normPngPath);
                        }
                    }

                    if (m_settings.bakeWorldNormal && !res.worldNormalR.empty()) {
                        std::string wnormPngPath = (outFolder / (m_settings.baseFilename + "_WorldNormal." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(wnormPngPath, res.worldNormalR.data(), res.worldNormalG.data(), res.worldNormalB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit world normal: " + wnormPngPath);
                            allSavedFiles.push_back(wnormPngPath);
                        }
                    }

                    if (m_settings.bakeAmbientOcclusion && !res.aoMap.empty()) {
                        std::string aoPngPath = (outFolder / (m_settings.baseFilename + "_AO." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(aoPngPath, res.aoMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit AO map: " + aoPngPath);
                            allSavedFiles.push_back(aoPngPath);
                        }
                    }

                    if (m_settings.bakeCavity && !res.cavityMap.empty()) {
                        std::string cavPngPath = (outFolder / (m_settings.baseFilename + "_Cavity." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(cavPngPath, res.cavityMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Cavity map: " + cavPngPath);
                            allSavedFiles.push_back(cavPngPath);
                        }
                    }

                    if (m_settings.bakeCurvature && !res.curvatureMap.empty()) {
                        std::string curvPngPath = (outFolder / (m_settings.baseFilename + "_Curvature." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(curvPngPath, res.curvatureMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Curvature map: " + curvPngPath);
                            allSavedFiles.push_back(curvPngPath);
                        }
                    }

                    if (m_settings.bakeThickness && !res.thicknessMap.empty()) {
                        std::string thickPngPath = (outFolder / (m_settings.baseFilename + "_Thickness." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveGrayscalePNG(thickPngPath, res.thicknessMap.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit thickness map: " + thickPngPath);
                            allSavedFiles.push_back(thickPngPath);
                        }
                    }

                    if (m_settings.bakeMaterialID && !res.materialIdMapR.empty()) {
                        std::string matPngPath = (outFolder / (m_settings.baseFilename + "_MaterialID." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(matPngPath, res.materialIdMapR.data(), res.materialIdMapG.data(), res.materialIdMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Material ID: " + matPngPath);
                            allSavedFiles.push_back(matPngPath);
                        }
                    }

                    if (m_settings.bakeBaseColor && !res.baseColorMapR.empty()) {
                        std::string colPngPath = (outFolder / (m_settings.baseFilename + "_BaseColor." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(colPngPath, res.baseColorMapR.data(), res.baseColorMapG.data(), res.baseColorMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved 8-bit Base Color: " + colPngPath);
                            allSavedFiles.push_back(colPngPath);
                        }
                    }

                    if (m_settings.bakeDiagnosticMap && !res.diagnosticMapR.empty()) {
                        std::string diagPngPath = (outFolder / (m_settings.baseFilename + "_Diag." + udimStr + ".png")).string();
                        std::string pngErr;
                        if (ExrWriter::SaveRGBPNG(diagPngPath, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, pngErr)) {
                            m_logEntries.push_back("[PNG] Saved diagnostic map: " + diagPngPath);
                            allSavedFiles.push_back(diagPngPath);
                        }
                    }
                }
            }

            // 3. Low-Poly OBJ with exact matching baked vertex normals
            std::string lpObjPath = (outFolder / (m_settings.baseFilename + "_LowPoly_Baked.obj")).string();
            std::string objErr;
            if (MeshLoader::SaveLowPolyOBJWithNormals(lpObjPath, m_lowPolyMesh, objErr)) {
                m_logEntries.push_back("[OBJ] Exported baked low-poly mesh with matching normals: " + lpObjPath);
                allSavedFiles.push_back(lpObjPath);
            }

            // Synchronize timestamps of all generated files across all UDIMs to identical timestamp
            // Guarantees consistent side-by-side ordering in Windows File Explorer regardless of sorting mode
            auto syncTime = std::filesystem::file_time_type::clock::now();
            for (const auto& fp : allSavedFiles) {
                std::error_code sec;
                std::filesystem::last_write_time(fp, syncTime, sec);
            }

            // Stage baked normal map for real-time 3D viewport preview
            if (m_settings.bakeNormal && !results.empty()) {
                for (const auto& res : results) {
                    if (!res.normalMapR.empty()) {
                        std::lock_guard<std::mutex> lock(m_normalMutex);
                        m_stagedNormalR = res.normalMapR;
                        m_stagedNormalG = res.normalMapG;
                        m_stagedNormalB = res.normalMapB;
                        m_stagedNormalW = res.width;
                        m_stagedNormalH = res.height;
                        m_pendingNormalUpload = true;
                        break;
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

void UIManager::EnsureCageInitialized() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    if (m_lowPolyMesh.cagePositions.size() != m_lowPolyMesh.vertices.size()) {
        m_lowPolyMesh.cagePositions.resize(m_lowPolyMesh.vertices.size());
        m_lowPolyMesh.cageOffsets.assign(m_lowPolyMesh.vertices.size(), 0.0f);
        if (!m_lowPolyMesh.HasWatertightCageNormals()) {
            m_lowPolyMesh.ComputeWatertightCageNormals(180.0f);
        }
        for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
            Vec3 n = m_lowPolyMesh.HasWatertightCageNormals() ? m_lowPolyMesh.cageNormals[i] : m_lowPolyMesh.vertices[i].normal;
            m_lowPolyMesh.cagePositions[i] = m_lowPolyMesh.vertices[i].position + n * m_settings.cageDistance;
        }
    }
    m_hasExplicitCage = true;
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, 1.0f);
}

void UIManager::EnsureSkewInitialized() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    if (m_lowPolyMesh.vertexSkew.size() != m_lowPolyMesh.vertices.size()) {
        m_lowPolyMesh.vertexSkew.assign(m_lowPolyMesh.vertices.size(), 0.0f);
    }
    m_viewportRenderer.UpdateSkew(m_lowPolyMesh, 1.0f);
}

void UIManager::ResetPaintedCage() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    m_lowPolyMesh.cageOffsets.assign(m_lowPolyMesh.vertices.size(), 0.0f);
    if (!m_lowPolyMesh.HasWatertightCageNormals()) {
        m_lowPolyMesh.ComputeWatertightCageNormals(180.0f);
    }
    for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
        Vec3 n = m_lowPolyMesh.HasWatertightCageNormals() ? m_lowPolyMesh.cageNormals[i] : m_lowPolyMesh.vertices[i].normal;
        m_lowPolyMesh.cagePositions[i] = m_lowPolyMesh.vertices[i].position + n * m_settings.cageDistance;
    }
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, 1.0f);
}

void UIManager::ResetPaintedSkew() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    m_lowPolyMesh.vertexSkew.assign(m_lowPolyMesh.vertices.size(), 0.0f);
    m_viewportRenderer.UpdateSkew(m_lowPolyMesh, 1.0f);
}

void UIManager::PushHistoryState(const std::string& desc) {
    if (!m_hasLowPoly) return;
    if (m_history.GetStateCount() == 0) {
        m_history.PushState("Initial Mesh", m_lowPolyMesh, m_curveManager);
    }
    m_history.PushState(desc, m_lowPolyMesh, m_curveManager);
}

void UIManager::PerformUndo() {
    std::string undoneDesc;
    if (m_history.Undo(m_lowPolyMesh, m_curveManager, undoneDesc)) {
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
        m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
        m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
        m_hasLastSelectedEdge = false;
        m_draggingPointIndex = -1;
        m_curveManager.selectedPointIndex = -1;
        m_curveManager.hoveredPointIndex = -1;
        char msg[128];
        snprintf(msg, sizeof(msg), "Undo: %s", undoneDesc.c_str());
        m_uvStatusMessage = msg;
        m_logEntries.push_back(std::string("[Undo] ") + undoneDesc);
    } else {
        m_uvStatusMessage = "Nothing to undo";
    }
}

void UIManager::PerformRedo() {
    std::string redoneDesc;
    if (m_history.Redo(m_lowPolyMesh, m_curveManager, redoneDesc)) {
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
        m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
        m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
        m_hasLastSelectedEdge = false;
        m_draggingPointIndex = -1;
        m_curveManager.selectedPointIndex = -1;
        m_curveManager.hoveredPointIndex = -1;
        char msg[128];
        snprintf(msg, sizeof(msg), "Redo: %s", redoneDesc.c_str());
        m_uvStatusMessage = msg;
        m_logEntries.push_back(std::string("[Redo] ") + redoneDesc);
    } else {
        m_uvStatusMessage = "Nothing to redo";
    }
}

} // namespace Baker
