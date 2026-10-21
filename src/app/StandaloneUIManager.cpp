#include "StandaloneUIManager.h"
#include "MeshLoader.h"
#include "BakerUI.h"
#include "../baker/ExrWriter.h"
#include <windows.h>
#include <shobjidl.h>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <cmath>
#include <algorithm>

namespace Baker {

static std::string OpenFileDialog(const wchar_t* filter, HWND owner = nullptr) {
    std::string resultPath;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool coInitialized = SUCCEEDED(hr);

    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        COMDLG_FILTERSPEC filterSpecs[] = {
            { L"All Supported 3D Meshes (*.obj, *.fbx, *.goz, *.bin)", L"*.obj;*.fbx;*.goz;*.bin" },
            { L"Wavefront OBJ (*.obj)", L"*.obj" },
            { L"Autodesk FBX (*.fbx)", L"*.fbx" },
            { L"GoZ Binary (*.goz)", L"*.goz" },
            { L"Binary Mesh (*.bin)", L"*.bin" },
            { L"All Files (*.*)", L"*.*" }
        };
        pFileOpen->SetFileTypes(ARRAYSIZE(filterSpecs), filterSpecs);
        pFileOpen->SetTitle(L"Open 3D Mesh");

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

    if (coInitialized) CoUninitialize();
    return resultPath;
}

static std::string OpenFolderDialog(HWND owner = nullptr) {
    std::string resultPath;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool coInitialized = SUCCEEDED(hr);

    IFileOpenDialog* pFileOpen = nullptr;
    hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions = 0;
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

    if (coInitialized) CoUninitialize();
    return resultPath;
}

StandaloneUIManager::StandaloneUIManager() {
    m_settings.resolution = 2048;
    m_settings.cageDistance = 0.05f;
    m_settings.maxRayDistance = 0.2f;
    m_settings.antiAliasing = 1;
    m_settings.bakeNormal = true;
    m_settings.bakeDisplacement = true;
    m_settings.bakeAmbientOcclusion = true;
    m_settings.bakeCurvature = true;
    m_settings.bakeCavity = true;
    m_settings.flipYNormal = false; // Default OpenGL (+Y) standard for Blender/Maya
    m_settings.angleThreshold = 75.0f;

    m_logEntries.push_back("[System] UDIM Baker Studio initialized.");
    m_logEntries.push_back("[System] Ready for High-Poly and Low-Poly mesh ingestion.");
}

StandaloneUIManager::~StandaloneUIManager() {
    m_viewportRenderer.Release();
}

void StandaloneUIManager::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (device && context) {
        if (m_viewportRenderer.Initialize(device, context)) {
            m_logEntries.push_back("[DirectX 11] Viewport hardware acceleration initialized.");
        } else {
            m_logEntries.push_back("[Error] Failed to initialize ViewportRenderer.");
        }
    }
}

bool StandaloneUIManager::IsUDIMSelected(int tile) const {
    return std::find(m_selectedUDIMs.begin(), m_selectedUDIMs.end(), tile) != m_selectedUDIMs.end();
}

void StandaloneUIManager::ToggleUDIM(int tile) {
    auto it = std::find(m_selectedUDIMs.begin(), m_selectedUDIMs.end(), tile);
    if (it != m_selectedUDIMs.end()) {
        m_selectedUDIMs.erase(it);
    } else {
        m_selectedUDIMs.push_back(tile);
    }
}

void StandaloneUIManager::SelectAllUDIMs() {
    m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;
}

void StandaloneUIManager::SelectOnlyUDIM(int tile) {
    m_selectedUDIMs.clear();
    m_selectedUDIMs.push_back(tile);
}

void StandaloneUIManager::SetHighPolyMeshPath(const std::string& path) {
    if (path.empty()) return;
    if (m_isLoadingHighPoly) {
        m_logEntries.push_back("[Warning] A high-poly mesh is already being loaded in background.");
        return;
    }

    m_highPolyPath = path;
    m_isLoadingHighPoly = true;
    m_loadingStatusText = "Ingesting High-Poly Mesh...";
    m_logEntries.push_back("[Ingest] Ingesting High-Poly in background: " + path);

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
                    m_logEntries.push_back("[Embree 3] High-Poly BVH accelerated structure built successfully.");
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

void StandaloneUIManager::SetLowPolyMeshPath(const std::string& path) {
    if (!path.empty()) {
        m_lowPolyPath = path;
        m_logEntries.push_back("[Ingest] Ingesting Low-Poly mesh: " + path);
        std::string err;
        if (MeshLoader::LoadLowPolyMesh(path, m_lowPolyMesh, err)) {
            m_hasLowPoly = true;
            ResetTransforms();
            m_logEntries.push_back("[Mesh] Loaded Low-Poly (" +
                std::to_string(m_lowPolyMesh.vertices.size()) + " verts, " +
                std::to_string(m_lowPolyMesh.detectedUDIMs.size()) + " UDIMs).");

            m_selectedUDIMs = m_lowPolyMesh.detectedUDIMs;

            float lpDiag = m_lowPolyMesh.GetBoundingBoxDiagonal();
            if (lpDiag > 1e-4f) {
                m_settings.cageDistance = lpDiag * 0.015f;
                m_settings.maxRayDistance = lpDiag * 0.08f;
            }

            m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
            m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
            m_viewportRenderer.FrameAll();

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

void StandaloneUIManager::SetCageMeshPath(const std::string& path) {
    if (!path.empty()) {
        std::string err;
        if (MeshLoader::LoadCageMesh(path, m_lowPolyMesh, err)) {
            m_hasExplicitCage = true;
            m_cagePath = path;
            m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
            m_logEntries.push_back("[Cage] Loaded explicit cage: " + path);
        } else {
            m_logEntries.push_back("[Error] Failed loading cage: " + err);
        }
    }
}

void StandaloneUIManager::ClearLowPoly() {
    m_lowPolyMesh.Clear();
    m_hasLowPoly = false;
    m_lowPolyPath.clear();
    m_selectedUDIMs.clear();
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, 1.0f);
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, 0.0f, 1.0f);
    m_logEntries.push_back("[Scene] Low-Poly mesh removed.");
}

void StandaloneUIManager::ClearHighPoly() {
    std::lock_guard<std::mutex> lock(m_meshMutex);
    m_highPolyMesh.Clear();
    m_hasHighPoly = false;
    m_highPolyPath.clear();
    m_pendingMeshUpload = false;
    m_viewportRenderer.SetHighPolyMesh(m_highPolyMesh);
    m_logEntries.push_back("[Scene] High-Poly mesh removed.");
}

void StandaloneUIManager::ClearCage() {
    m_hasExplicitCage = false;
    m_cagePath.clear();
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
    m_logEntries.push_back("[Scene] Explicit cage cleared. Using procedural extrusion.");
}

void StandaloneUIManager::ClearScene() {
    ClearLowPoly();
    ClearHighPoly();
    ClearCage();
    m_viewportRenderer.FrameAll();
    m_logEntries.push_back("[Scene] Workspace completely cleared.");
}

void StandaloneUIManager::CheckScaleAndAutoCalibrate() {
    if (!m_hasLowPoly || !m_hasHighPoly) return;

    float lpDiag = m_lowPolyMesh.GetBoundingBoxDiagonal();
    float hpDiag = m_highPolyMesh.GetBoundingBoxDiagonal();

    if (lpDiag > 1e-4f && hpDiag > 1e-4f) {
        m_detectedScaleRatio = hpDiag / lpDiag;
        if (std::abs(m_detectedScaleRatio - 1.0f) > 0.05f) {
            char logBuf[256];
            snprintf(logBuf, sizeof(logBuf), "[Scale Detector] Scale mismatch detected! High-Poly (%.3f) vs Low-Poly (%.3f). Ratio: %.3fx",
                hpDiag, lpDiag, m_detectedScaleRatio);
            m_logEntries.push_back(logBuf);
        }
    }
}

void StandaloneUIManager::AutoAlignMeshCenters() {
    DirectX::XMFLOAT3 hpC = m_viewportRenderer.GetHighPolyCenter();
    DirectX::XMFLOAT3 lpC = m_viewportRenderer.GetLowPolyCenter();

    m_settings.lowPolyTranslation.x += (hpC.x - lpC.x);
    m_settings.lowPolyTranslation.y += (hpC.y - lpC.y);
    m_settings.lowPolyTranslation.z += (hpC.z - lpC.z);
    SyncTransforms();
    m_logEntries.push_back("[Transform] Low-Poly center aligned to High-Poly bounding center.");
}

void StandaloneUIManager::AutoFitMeshScale() {
    if (m_detectedScaleRatio > 1e-4f) {
        m_settings.lowPolyScale = m_detectedScaleRatio;
        m_settings.lowPolyScaleVec = { m_detectedScaleRatio, m_detectedScaleRatio, m_detectedScaleRatio };
        SyncTransforms();
        char logBuf[128];
        snprintf(logBuf, sizeof(logBuf), "[Transform] Low-Poly scale calibrated to %.4fx.", m_detectedScaleRatio);
        m_logEntries.push_back(logBuf);
    }
}

void StandaloneUIManager::SyncTransforms() {
    m_viewportRenderer.SetLowPolyTransform(
        DirectX::XMFLOAT3(m_settings.lowPolyTranslation.x, m_settings.lowPolyTranslation.y, m_settings.lowPolyTranslation.z),
        DirectX::XMFLOAT3(m_settings.lowPolyRotation.x, m_settings.lowPolyRotation.y, m_settings.lowPolyRotation.z),
        DirectX::XMFLOAT3(m_settings.lowPolyScaleVec.x, m_settings.lowPolyScaleVec.y, m_settings.lowPolyScaleVec.z)
    );
    m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
}

void StandaloneUIManager::ResetTransforms() {
    m_settings.lowPolyTranslation = { 0.0f, 0.0f, 0.0f };
    m_settings.lowPolyRotation = { 0.0f, 0.0f, 0.0f };
    m_settings.lowPolyScaleVec = { 1.0f, 1.0f, 1.0f };
    m_settings.lowPolyScale = 1.0f;
    SyncTransforms();
    m_logEntries.push_back("[Transform] Low-Poly transforms reset to default.");
}

void StandaloneUIManager::RenderUI() {
    // Check pending GPU mesh upload from background loader
    if (m_pendingMeshUpload) {
        std::lock_guard<std::mutex> lock(m_meshMutex);
        m_viewportRenderer.SetHighPolyMesh(m_highPolyMesh);
        m_viewportRenderer.FrameAll();
        CheckScaleAndAutoCalibrate();
        m_pendingMeshUpload = false;
    }

    // Check pending GPU baked normal map upload from bake worker
    if (m_pendingNormalUpload) {
        std::lock_guard<std::mutex> lock(m_normalMutex);
        m_viewportRenderer.SetBakedNormalMap(m_stagedNormalR.data(), m_stagedNormalG.data(), m_stagedNormalB.data(), m_stagedNormalW, m_stagedNormalH);
        m_pendingNormalUpload = false;
    }

    // Update Status Bar info
    if (m_isBaking) {
        m_shellState.statusText = m_bakeStatus.c_str();
        m_shellState.statusColor = BakerUI::Col::Accent();
        m_shellState.bakeProgress = m_bakeProgress;
    } else {
        if (m_hasHighPoly && m_hasLowPoly) {
            m_shellState.statusText = "Ready to Bake";
            m_shellState.statusColor = BakerUI::Col::Ok();
        } else if (m_hasLowPoly) {
            m_shellState.statusText = "Awaiting High-Poly Mesh...";
            m_shellState.statusColor = BakerUI::Col::Warn();
        } else if (m_hasHighPoly) {
            m_shellState.statusText = "Awaiting Low-Poly Mesh...";
            m_shellState.statusColor = BakerUI::Col::Warn();
        } else {
            m_shellState.statusText = "Awaiting Meshes";
            m_shellState.statusColor = BakerUI::Col::TextDim();
        }
        m_shellState.bakeProgress = -1.0f;
    }

    // Begin Host (Main Menu Bar, Top Toolbar, DockSpace, Status Bar)
    BakerUI::BeginFrameHost(m_shellState);

    // Handle menu triggers
    if (m_shellState.clearScene) {
        m_shellState.clearScene = false;
        ClearScene();
    }
    if (m_shellState.openLowPoly) {
        m_shellState.openLowPoly = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0");
        if (!path.empty()) SetLowPolyMeshPath(path);
    }
    if (m_shellState.openHighPoly) {
        m_shellState.openHighPoly = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx;*.goz;*.bin)\0*.obj;*.fbx;*.goz;*.bin\0");
        if (!path.empty()) SetHighPolyMeshPath(path);
    }
    if (m_shellState.openCage) {
        m_shellState.openCage = false;
        std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0");
        if (!path.empty()) SetCageMeshPath(path);
    }
    if (m_shellState.exitApp) {
        PostQuitMessage(0);
    }

    // Mode-specific tab activation & focus
    if (m_shellState.activeTab == BakerUI::ShellState::AppTab::UVCutAndPack) {
        if (!m_focusUVTab) {
            ImGui::SetNextWindowFocus();
            m_focusUVTab = true;
        }
        m_viewportRenderer.showSeams = true;
    } else {
        m_focusUVTab = false;
    }

    // 0. Scene Outliner
    if (m_shellState.showOutliner) {
        RenderOutlinerPanel();
    }

    // 1. Center / Main Stage: Viewport (3D / 2D / Split)
    RenderViewportPanel();

    // 2. Left Column: Mesh Ingestion & Transforms
    RenderMeshPanel();

    // 3. Right Column: Bake Settings & UV Tool
    if (m_shellState.activeTab == BakerUI::ShellState::AppTab::Baking) {
        RenderSettingsPanel();
    }
    if (m_shellState.showUVPanel) {
        RenderUVCutAndPackPanel();
    }

    // 4. Bottom Tab: UDIM Canvas
    if (m_shellState.showUdim) {
        RenderUDIMCanvasPanel();
    }

    // 5. Bottom Tab: Log
    if (m_shellState.showLog) {
        RenderLogPanel();
    }

    // 6. Bake Progress Overlay (Modal)
    RenderBakeProgressPanel();

    // End Host DockSpace Frame
    BakerUI::EndFrameHost();
}

void StandaloneUIManager::RenderOutlinerPanel() {
    ImGui::Begin("Scene Outliner###outliner", &m_shellState.showOutliner);

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
                if (hpName.empty()) hpName = "HighPoly_Source";

                ImGui::PushID("hp_node_hdr");
                bool hpOpen = ImGui::TreeNodeEx("##hp_tree", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth,
                                                "[HP] %s", hpName.c_str());
                ImGui::SameLine(ImGui::GetWindowWidth() - 95.0f);
                BakerUI::ToggleChip("View", &m_viewportRenderer.showHighPoly);
                ImGui::SameLine();
                if (ImGui::SmallButton("X##hp_del")) {
                    ClearHighPoly();
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unload high-poly mesh");
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
                        const char* shadingModes[] = { "Clay", "Wax Red", "Normals", "Flat" };
                        int currentShading = (int)m_viewportRenderer.highPolyShading;
                        if (ImGui::Combo("Shading##hp_shade", &currentShading, shadingModes, IM_ARRAYSIZE(shadingModes))) {
                            m_viewportRenderer.highPolyShading = (ShadingMode)currentShading;
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

void StandaloneUIManager::RenderViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Viewport###viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 64.0f) avail.x = 64.0f;
    if (avail.y < 64.0f) avail.y = 64.0f;

    m_viewportRenderer.Resize((int)avail.x, (int)avail.y);

    ImVec2 screenPos = ImGui::GetCursorScreenPos();
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsWindowHovered()) {
        int button = 0;
        if (io.KeyAlt) {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) button = 1;       // Maya Alt + LMB: Tumble/Orbit
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) button = 2; // Maya Alt + MMB: Track/Pan
            else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) button = 3;  // Maya Alt + RMB: Dolly/Zoom
        } else {
            if (m_toolMode == ToolMode::Navigate) {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) button = 2;      // MMB: Track/Pan
                else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) button = 3; // RMB: Dolly/Zoom
                else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) button = 1;  // LMB: Orbit
            } else {
                if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) button = 2;      // MMB: Track/Pan
                else if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) button = 3; // RMB: Dolly/Zoom
            }
        }

        if (button != 0 || io.MouseWheel != 0.0f) {
            m_viewportRenderer.HandleMouseInput(io.MouseDelta.x, io.MouseDelta.y, button, io.MouseWheel);
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            m_viewportRenderer.FrameAll();
        }
    }

    m_viewportRenderer.Render();

    ID3D11ShaderResourceView* srv = m_viewportRenderer.GetViewportSRV();
    if (srv) {
        ImGui::Image((ImTextureID)srv, avail);
    }

    // Marmoset-style 3D interactive brush ray-casting & reticle
    if (m_toolMode != ToolMode::Navigate && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? avail.x * 0.5f : avail.x;
        float relX = io.MousePos.x - screenPos.x;
        float relY = io.MousePos.y - screenPos.y;

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
                    // Visual screen-space reticle ring
                    DirectX::XMFLOAT3 p1(hitPos.x, hitPos.y, hitPos.z);
                    DirectX::XMFLOAT3 p2(hitPos.x + m_brushRadius, hitPos.y, hitPos.z);
                    float s1X, s1Y, s2X, s2Y;
                    m_viewportRenderer.ProjectToScreen(p1, s1X, s1Y);
                    m_viewportRenderer.ProjectToScreen(p2, s2X, s2Y);
                    float screenBrushR = std::hypot(s2X - s1X, s2Y - s1Y);
                    if (screenBrushR < 6.0f) screenBrushR = 6.0f;
                    if (screenBrushR > 400.0f) screenBrushR = 400.0f;

                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    ImU32 reticleCol = (m_toolMode == ToolMode::PaintCage)
                        ? IM_COL32(40, 190, 255, 230)
                        : IM_COL32(255, 175, 40, 230);
                    drawList->AddCircle(io.MousePos, screenBrushR, reticleCol, 36, 2.0f);
                    drawList->AddCircleFilled(io.MousePos, 3.5f, IM_COL32(255, 255, 255, 255));

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
    if (m_toolMode == ToolMode::CutSeam && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        float maxActiveW = (m_viewportMode == ViewportMode::SplitBoth) ? avail.x * 0.5f : avail.x;
        float relX = io.MousePos.x - screenPos.x;
        float relY = io.MousePos.y - screenPos.y;

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
                if (m_lowPolyMesh.Raycast(localOrg, localDir, hitT, hitPos, hitNormal, hitTriIdx) && hitTriIdx >= 0) {
                    const auto& tri = m_lowPolyMesh.triangles[hitTriIdx];
                    uint32_t bestV0 = 0, bestV1 = 0;
                    float minEdgeDist = 1e9f;
                    for (int e = 0; e < 3; ++e) {
                        uint32_t ea = tri.indices[e];
                        uint32_t eb = tri.indices[(e + 1) % 3];
                        Vec3 A = m_lowPolyMesh.vertices[ea].position;
                        Vec3 B = m_lowPolyMesh.vertices[eb].position;
                        Vec3 AB = B - A;
                        float lenSq = AB.Dot(AB);
                        float t = (lenSq > 1e-12f) ? std::clamp((hitPos - A).Dot(AB) / lenSq, 0.0f, 1.0f) : 0.0f;
                        Vec3 proj = A + AB * t;
                        float d = (hitPos - proj).Length();
                        if (d < minEdgeDist) {
                            minEdgeDist = d;
                            bestV0 = ea;
                            bestV1 = eb;
                        }
                    }

                    // Highlight edge under cursor in neon cyan
                    Vec3 pA = m_lowPolyMesh.vertices[bestV0].position;
                    Vec3 pB = m_lowPolyMesh.vertices[bestV1].position;
                    float sAX, sAY, sBX, sBY;
                    m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(pA.x, pA.y, pA.z), sAX, sAY);
                    m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(pB.x, pB.y, pB.z), sBX, sBY);

                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    drawList->AddLine(ImVec2(sAX, sAY), ImVec2(sBX, sBY), IM_COL32(0, 255, 255, 255), 3.0f);
                    drawList->AddCircle(io.MousePos, 7.0f, IM_COL32(0, 255, 255, 240), 16, 2.0f);

                    if (ImGui::IsMouseClicked(0)) {
                        UVCutter::ToggleEdgeSeam(m_lowPolyMesh, bestV0, bestV1);
                        m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
                        m_viewportRenderer.showSeams = true;
                        char msg[128];
                        snprintf(msg, sizeof(msg), "Toggled seam on edge (%u, %u) - total %zu seams",
                            bestV0, bestV1, m_lowPolyMesh.cutSeams.size());
                        m_uvStatusMessage = msg;
                        m_logEntries.push_back(std::string("[UV Cut] ") + msg);
                    }
                }
            }
        }
    }

    // Viewport Mode overlay buttons (Top Left)
    ImGui::SetCursorScreenPos(ImVec2(screenPos.x + 12.0f, screenPos.y + 12.0f));
    ImGui::BeginGroup();
    {
        bool is3D = (m_viewportMode == ViewportMode::View3D);
        if (BakerUI::ToggleChip("3D View", &is3D)) {
            m_viewportMode = ViewportMode::View3D;
        }
        ImGui::SameLine();
        bool is2D = (m_viewportMode == ViewportMode::View2D);
        if (BakerUI::ToggleChip("2D UVs", &is2D)) {
            m_viewportMode = ViewportMode::View2D;
        }
        ImGui::SameLine();
        bool isSplit = (m_viewportMode == ViewportMode::SplitBoth);
        if (BakerUI::ToggleChip("Split", &isSplit)) {
            m_viewportMode = ViewportMode::SplitBoth;
        }

        ImGui::SameLine();
        BakerUI::ToggleChip("Grid", &m_viewportRenderer.showGrid);

        if (m_viewportRenderer.HasBakedNormalMap()) {
            ImGui::SameLine();
            BakerUI::ToggleChip("Baked Normals", &m_viewportRenderer.showBakedNormals);
        }

        ImGui::SameLine();
        BakerUI::ToggleChip("Cage", &m_viewportRenderer.showCage);

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(10.0f, 0.0f));
        ImGui::SameLine();

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
        ImGui::SameLine();
        bool isCutSeam = (m_toolMode == ToolMode::CutSeam);
        if (BakerUI::ToggleChip("Cut Seam", &isCutSeam)) {
            m_toolMode = isCutSeam ? ToolMode::CutSeam : ToolMode::Navigate;
            if (m_toolMode == ToolMode::CutSeam) {
                m_viewportRenderer.showSeams = true;
            }
        }

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(16.0f, 0.0f));
        ImGui::SameLine();

        const char* shadingNames[] = { "Clay Matcap", "Wax Red", "Normal Color", "Flat Shaded" };
        int currentShading = (int)m_viewportRenderer.highPolyShading;
        ImGui::SetNextItemWidth(130.0f);
        if (ImGui::Combo("##shading", &currentShading, shadingNames, IM_ARRAYSIZE(shadingNames))) {
            m_viewportRenderer.highPolyShading = (ShadingMode)currentShading;
        }
    }
    ImGui::EndGroup();

    // Floating Marmoset 3D Brush Settings Bar
    if (m_toolMode != ToolMode::Navigate && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
        ImGui::SetCursorScreenPos(ImVec2(screenPos.x + 12.0f, screenPos.y + 44.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.14f, 0.15f, 0.18f, 0.94f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.32f, 0.38f, 0.85f));
        float barW = (std::min)(580.0f, avail.x - 24.0f);
        ImGui::BeginChild("##brush_bar", ImVec2(barW, 36.0f), true, ImGuiWindowFlags_NoScrollbar);
        {
            if (m_toolMode == ToolMode::CutSeam) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.0f, 0.95f, 1.0f, 1.0f), "SEAM CUTTER");
                ImGui::SameLine();
                ImGui::TextDisabled("Click low-poly edges in 3D to toggle seams");
                ImGui::SameLine();
                if (ImGui::Button("Clear Seams")) {
                    ClearSeams();
                }
            } else {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(m_toolMode == ToolMode::PaintCage ? ImVec4(0.3f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                                   m_toolMode == ToolMode::PaintCage ? "CAGE BRUSH" : "SKEW BRUSH");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("Radius##b", &m_brushRadius, 0.02f, 10.0f, "%.2f");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("Strength##b", &m_brushStrength, 0.05f, 1.0f, "%.2f");
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

    // 2D UV Canvas overlay if 2D or Split mode is active
    if (m_viewportMode == ViewportMode::View2D) {
        Render2DUVCanvas(screenPos, avail);
    } else if (m_viewportMode == ViewportMode::SplitBoth) {
        ImVec2 splitPos = ImVec2(screenPos.x + avail.x * 0.5f, screenPos.y);
        ImVec2 splitSize = ImVec2(avail.x * 0.5f, avail.y);
        Render2DUVCanvas(splitPos, splitSize);
    }

    // Floating Bake Button at the bottom of the 3D viewport canvas
    int udimCount = m_lowPolyMesh.detectedUDIMs.empty() ? 1 : (int)m_lowPolyMesh.detectedUDIMs.size();
    bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking && !m_isLoadingHighPoly;
    if (BakerUI::FloatingBakeBar(screenPos, avail, canBake, m_isBaking, m_bakeProgress, m_bakeStatus.c_str(),
                                 udimCount, m_settings.resolution, m_settings.resolution)) {
        if (canBake) {
            TriggerBake();
        }
    }

    ImGui::End();
}

void StandaloneUIManager::Render2DUVCanvas(const ImVec2& origin, const ImVec2& size) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), ImGui::GetColorU32(BakerUI::Col::Bg()));
    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

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

            float mx = io.MousePos.x - origin.x;
            float my = io.MousePos.y - origin.y;
            m_uvPan.x = mx - (mx - m_uvPan.x) * (m_uvZoom / prevZoom);
            m_uvPan.y = my - (my - m_uvPan.y) * (m_uvZoom / prevZoom);
        }
    }

    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float currentTileSize = baseTileSize * m_uvZoom;

    // Draw UDIM tiles (1001..1010, up to 4 rows)
    for (int vTile = 0; vTile < 4; ++vTile) {
        for (int uTile = 0; uTile < 10; ++uTile) {
            int udim = 1001 + uTile + vTile * 10;

            float x0 = origin.x + m_uvPan.x + uTile * currentTileSize;
            float y0 = origin.y + m_uvPan.y + (1.0f - (vTile + 1)) * currentTileSize;
            float x1 = x0 + currentTileSize;
            float y1 = y0 + currentTileSize;

            if (x1 < origin.x || x0 > origin.x + size.x || y1 < origin.y || y0 > origin.y + size.y) {
                continue;
            }

            bool isDetected = std::find(m_lowPolyMesh.detectedUDIMs.begin(), m_lowPolyMesh.detectedUDIMs.end(), udim) != m_lowPolyMesh.detectedUDIMs.end();
            bool isSelected = IsUDIMSelected(udim);

            ImU32 tileBg = isSelected ? ImGui::GetColorU32(ImVec4(0.12f, 0.28f, 0.44f, 0.35f)) :
                           (isDetected ? ImGui::GetColorU32(ImVec4(0.20f, 0.22f, 0.25f, 0.25f)) :
                                         ImGui::GetColorU32(ImVec4(0.12f, 0.14f, 0.16f, 0.15f)));

            drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), tileBg);

            // Live Baked Texture Display (xNormal / Substance 2D canvas style)
            if (m_viewportRenderer.GetBakedNormalSRV() && (udim == 1001 || (isDetected && isSelected))) {
                drawList->AddImage(
                    (ImTextureID)m_viewportRenderer.GetBakedNormalSRV(),
                    ImVec2(x0, y0),
                    ImVec2(x1, y1),
                    ImVec2(0.0f, 0.0f),
                    ImVec2(1.0f, 1.0f)
                );
            }

            drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), isSelected ? ImGui::GetColorU32(BakerUI::Col::Accent()) : ImGui::GetColorU32(BakerUI::Col::Border()));

            char tileStr[16];
            snprintf(tileStr, sizeof(tileStr), "%d", udim);
            drawList->AddText(ImVec2(x0 + 8.0f, y0 + 6.0f), isSelected ? ImGui::GetColorU32(BakerUI::Col::Accent()) : ImGui::GetColorU32(BakerUI::Col::TextDim()), tileStr);
        }
    }

    // Draw UV triangles
    if (m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        ImU32 uvWireCol = ImGui::GetColorU32(ImVec4(0.95f, 0.95f, 0.98f, 0.40f));
        for (const auto& tri : m_lowPolyMesh.triangles) {
            const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
            const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
            const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];

            ImVec2 p0(origin.x + m_uvPan.x + v0.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v0.uv.v) * currentTileSize);
            ImVec2 p1(origin.x + m_uvPan.x + v1.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v1.uv.v) * currentTileSize);
            ImVec2 p2(origin.x + m_uvPan.x + v2.uv.u * currentTileSize, origin.y + m_uvPan.y + (1.0f - v2.uv.v) * currentTileSize);

            drawList->AddTriangle(p0, p1, p2, uvWireCol, 1.0f);
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

    // Draw packed island bounds
    if (m_hasUnwrappedIslands && !m_currentIslands.empty()) {
        for (const auto& isl : m_currentIslands) {
            int uTile = (isl.targetUDIM - 1001) % 10;
            int vTile = (isl.targetUDIM - 1001) / 10;
            float tileX = origin.x + m_uvPan.x + uTile * currentTileSize;
            float tileY = origin.y + m_uvPan.y + (1.0f - (vTile + 1)) * currentTileSize;

            ImVec2 b0(tileX + isl.bounds.minU * currentTileSize, tileY + (1.0f - isl.bounds.maxV) * currentTileSize);
            ImVec2 b1(tileX + isl.bounds.maxU * currentTileSize, tileY + (1.0f - isl.bounds.minV) * currentTileSize);

            drawList->AddRectFilled(b0, b1, IM_COL32(0, 180, 255, 22), 2.0f);
            drawList->AddRect(b0, b1, IM_COL32(0, 200, 255, 120), 2.0f, 0, 1.0f);
        }
    }

    drawList->PopClipRect();
}

void StandaloneUIManager::RenderMeshPanel() {
    ImGui::Begin("Mesh Ingestion###ingest", nullptr);

    // 1. High-Poly Source Card
    BakerUI::SectionLabel("HIGH-POLY SCULPT / SOURCE");
    bool openHP = BakerUI::BeginCard("##hp_card");
    if (openHP) {
        if (BakerUI::PrimaryButton("Browse High-Poly Mesh...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx;*.goz;*.bin)\0*.obj;*.fbx;*.goz;*.bin\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetHighPolyMeshPath(path);
            }
        }

        if (m_isLoadingHighPoly) {
            ImGui::Spacing();
            BakerUI::StatusPill("Ingesting in background...", BakerUI::Col::Warn());
            ImGui::TextDisabled("Reading vertices & building BVH tree...");
        } else if (m_hasHighPoly) {
            ImGui::Spacing();
            if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
            ImGui::TextDisabled("%s", m_highPolyPath.c_str());
            ImGui::Text("Positions: %zu", m_highPolyMesh.positions.size());
            if (m_highPolyMesh.isQuadMesh && !m_highPolyMesh.quadIndices.empty()) {
                ImGui::Text("Quads:     %zu", m_highPolyMesh.quadIndices.size() / 4);
            } else {
                ImGui::Text("Triangles: %zu", m_highPolyMesh.indices.size() / 3);
            }
            ImGui::Text("Diagonal:  %.4f", m_highPolyMesh.GetBoundingBoxDiagonal());
            if (BakerUI::gFontMono) ImGui::PopFont();
            ImGui::Spacing();
            BakerUI::StatusPill("BVH Ready (Embree 3)", BakerUI::Col::Ok());
            ImGui::SameLine();
            if (BakerUI::SecondaryButton("Remove##hp_rm")) {
                ClearHighPoly();
            }
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No High-Poly loaded", BakerUI::Col::TextDim());
        }
    }
    BakerUI::EndCard();

    // 2. Low-Poly Target Card
    BakerUI::SectionLabel("LOW-POLY TARGET (UDIM)");
    bool openLP = BakerUI::BeginCard("##lp_card");
    if (openLP) {
        if (BakerUI::PrimaryButton("Browse Low-Poly Mesh...")) {
            std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0All Files (*.*)\0*.*\0");
            if (!path.empty()) {
                SetLowPolyMeshPath(path);
            }
        }

        if (m_hasLowPoly) {
            ImGui::Spacing();
            if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
            ImGui::TextDisabled("%s", m_lowPolyPath.c_str());
            ImGui::Text("Vertices:   %zu", m_lowPolyMesh.vertices.size());
            if (m_lowPolyMesh.quadCount > 0 && m_lowPolyMesh.triCount == 0) {
                ImGui::Text("Quads:      %u", m_lowPolyMesh.quadCount);
            } else if (m_lowPolyMesh.quadCount > 0) {
                ImGui::Text("Faces:      %u (%u Quads, %u Tris)", m_lowPolyMesh.polyCount, m_lowPolyMesh.quadCount, m_lowPolyMesh.triCount);
            } else {
                ImGui::Text("Triangles:  %zu", m_lowPolyMesh.triangles.size());
            }
            ImGui::Text("UDIM Tiles: %zu detected", m_lowPolyMesh.detectedUDIMs.size());
            if (BakerUI::gFontMono) ImGui::PopFont();

            // Detected UDIM Tile Checkboxes
            ImGui::Spacing();
            ImGui::Text("UDIM Selection:");
            ImGui::SameLine();
            if (ImGui::SmallButton("All")) SelectAllUDIMs();
            ImGui::SameLine();
            if (ImGui::SmallButton("None")) m_selectedUDIMs.clear();

            for (size_t i = 0; i < m_lowPolyMesh.detectedUDIMs.size(); ++i) {
                int tile = m_lowPolyMesh.detectedUDIMs[i];
                bool sel = IsUDIMSelected(tile);
                char label[32];
                snprintf(label, sizeof(label), "%d##chk_%d", tile, tile);
                if (ImGui::Checkbox(label, &sel)) {
                    ToggleUDIM(tile);
                }
                if ((i + 1) % 4 != 0 && (i + 1) < m_lowPolyMesh.detectedUDIMs.size()) {
                    ImGui::SameLine();
                }
            }

            ImGui::Spacing();
            if (BakerUI::SecondaryButton("Remove Low-Poly")) {
                ClearLowPoly();
            }
        } else {
            ImGui::Spacing();
            BakerUI::StatusPill("No Low-Poly loaded", BakerUI::Col::TextDim());
        }
    }
    BakerUI::EndCard();

    // 3. Alignment & Transforms Card
    BakerUI::SectionLabel("ALIGNMENT & TRANSFORMS");
    bool openAlign = BakerUI::BeginCard("##align_card");
    if (openAlign) {
        bool changed = false;
        changed |= ImGui::DragFloat3("Offset (XYZ)", &m_settings.lowPolyTranslation.x, 0.01f);
        changed |= ImGui::DragFloat3("Rotate (Deg)", &m_settings.lowPolyRotation.x, 0.5f);

        ImGui::Checkbox("Uniform Scale", &m_uniformScale);
        if (m_uniformScale) {
            if (ImGui::DragFloat("Scale", &m_settings.lowPolyScale, 0.01f, 0.0001f, 1000.0f)) {
                m_settings.lowPolyScaleVec = { m_settings.lowPolyScale, m_settings.lowPolyScale, m_settings.lowPolyScale };
                changed = true;
            }
        } else {
            if (ImGui::DragFloat3("Scale (XYZ)", &m_settings.lowPolyScaleVec.x, 0.01f, 0.0001f, 1000.0f)) {
                changed = true;
            }
        }

        if (changed) SyncTransforms();

        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Auto-Align Centers")) {
            AutoAlignMeshCenters();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Fit Scale")) {
            AutoFitMeshScale();
        }
        ImGui::SameLine();
        if (BakerUI::SecondaryButton("Reset")) {
            ResetTransforms();
        }
    }
    BakerUI::EndCard();

    // 4. Cage Configuration Card
    BakerUI::SectionLabel("CAGE DEFINITION");
    bool openCage = BakerUI::BeginCard("##cage_card");
    if (openCage) {
        if (m_hasExplicitCage) {
            BakerUI::StatusPill("Explicit Cage Active", BakerUI::Col::Ok());
            ImGui::TextDisabled("Source: %s", m_cagePath.c_str());
            if (BakerUI::SecondaryButton("Revert to Procedural Cage")) {
                ClearCage();
            }
        } else {
            ImGui::TextDisabled("Type: Procedural Vertex Normal Push");
            if (ImGui::SliderFloat("Push Distance", &m_settings.cageDistance, 0.0f, 0.5f, "%.4f")) {
                m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
            }
            if (BakerUI::SecondaryButton("Browse Explicit Cage (*.obj;*.fbx)...")) {
                std::string path = OpenFileDialog(L"Mesh Files (*.obj;*.fbx)\0*.obj;*.fbx\0");
                if (!path.empty()) SetCageMeshPath(path);
            }
        }
    }
    BakerUI::EndCard();

    ImGui::End();
}

void StandaloneUIManager::RenderSettingsPanel() {
    ImGui::Begin("Bake Parameters###params", nullptr);

    static const char* kRes[] = { "1024", "2048", "4096", "8192 (8K)", "16384 (16K)" };
    static int currentResIdx = 1; // 2048

    static const char* kAA[] = { "1x (Draft)", "2x2 (4 Samples - Prod)", "4x4 (16 Samples - Max)" };
    static int currentAAIdx = 1; // 2x2

    float maxSliderDist = (m_hasHighPoly ? m_highPolyMesh.GetBoundingBoxDiagonal() * 0.5f : 10.0f);
    if (maxSliderDist < 1.0f) maxSliderDist = 1.0f;

    BakerUI::SectionLabel("PROJECTION & QUALITY");
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
        BakerUI::PropCheck("Bidirectional Rays", &m_settings.useBidirectionalRays, "Marmoset/xNormal dual-direction rays");
        BakerUI::PropCheck("Isolate Mesh Parts", &m_settings.isolateMeshParts, "Prevent ray bleeding between adjacent parts");
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

    BakerUI::SectionLabel("OUTPUT MAPS");
    if (BakerUI::BeginProps("##maps")) {
        BakerUI::PropCheck("Tangent Normal (MikkT)", &m_settings.bakeNormal, "World-standard tangent space normal map");
        BakerUI::PropCheck("World / Object Normal", &m_settings.bakeWorldNormal, "Raw XYZ world surface vectors");
        BakerUI::PropCheck("Displacement / Height (32f)", &m_settings.bakeDisplacement, "Vector projection height difference");
        BakerUI::PropCheck("Ambient Occlusion (AO)", &m_settings.bakeAmbientOcclusion, "Cosine-weighted diffuse contact shadows");
        if (m_settings.bakeAmbientOcclusion) {
            BakerUI::PropInt("AO Samples", &m_settings.aoSamples, 4, 128);
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
        BakerUI::PropCheck("Material ID / Groups", &m_settings.bakeMaterialID, "Distinct color per polygroup/submesh");
        BakerUI::PropCheck("Base Color / Vertex Col", &m_settings.bakeBaseColor, "High-poly vertex colors / polypaint");
        BakerUI::PropCheck("Diagnostic map", &m_settings.bakeDiagnosticMap, "Missed rays and ray coverage analysis");
        static const char* const kNormalFormats[] = { "OpenGL (+Y) (Blender/Maya)", "DirectX (-Y) (Unreal/Max)" };
        int curNormalFmt = m_settings.flipYNormal ? 1 : 0;
        if (BakerUI::PropCombo("Normal Format", &curNormalFmt, kNormalFormats, 2)) {
            m_settings.flipYNormal = (curNormalFmt == 1);
        }
        BakerUI::PropCheck("Compact BVH", &m_settings.useCompactBVH, "For dense sculpts");
        BakerUI::EndProps();
    }

    BakerUI::SectionLabel("DESTINATION & FORMATS");
    bool openDest = BakerUI::BeginCard("##dest_card");
    if (openDest) {
        ImGui::TextDisabled("Output Folder:");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 90);
        ImGui::InputText("##outdir", m_outDirBuf, sizeof(m_outDirBuf));
        m_settings.outputDir = m_outDirBuf;
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(80, 0))) {
            std::string folder = OpenFolderDialog();
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

    // Pin BakeButton at the bottom
    float btnH = 44.f * (ImGui::GetFontSize() / 15.f) + ImGui::GetStyle().WindowPadding.y;
    float remain = ImGui::GetContentRegionAvail().y - btnH;
    if (remain > 0) ImGui::Dummy(ImVec2(0, remain));

    bool canBake = (m_hasLowPoly && m_hasHighPoly && !m_isBaking && !m_selectedUDIMs.empty());
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

void StandaloneUIManager::MarkSeamsByAngle() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByAngle(m_lowPolyMesh, m_seamAngleThreshold);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "Marked %zu seams by angle (%.1f deg)", m_lowPolyMesh.cutSeams.size(), m_seamAngleThreshold);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void StandaloneUIManager::MarkSeamsByPolygroup() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByPolygroup(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "Marked %zu seams along Polygroup boundaries", m_lowPolyMesh.cutSeams.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void StandaloneUIManager::MarkSeamsByUVBorders() {
    if (!m_hasLowPoly) {
        m_uvStatusMessage = "Please load a low-poly mesh first.";
        return;
    }
    UVCutter::MarkSeamsByUVBorders(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    char msg[128];
    snprintf(msg, sizeof(msg), "Detected %zu seams from existing UV borders", m_lowPolyMesh.cutSeams.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Cut] ") + msg);
}

void StandaloneUIManager::ClearSeams() {
    UVCutter::ClearSeams(m_lowPolyMesh);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_uvStatusMessage = "Cleared all seams.";
    m_logEntries.push_back("[UV Cut] Cleared all seams.");
}

void StandaloneUIManager::UnwrapMesh() {
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
    char msg[128];
    snprintf(msg, sizeof(msg), "Unwrapped %zu islands successfully", m_currentIslands.size());
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Unwrap] ") + msg);
}

void StandaloneUIManager::PackUVs() {
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

    char msg[256];
    snprintf(msg, sizeof(msg), "Packed %d islands across %d tile(s). Efficiency: %.1f%%",
        m_packStats.packedIslands, m_packStats.udimTilesUsed, m_packStats.packingEfficiency * 100.0f);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Pack] ") + msg);
}

void StandaloneUIManager::AutoCutUnwrapAndPack() {
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

    char msg[256];
    snprintf(msg, sizeof(msg), "Auto Cut & Pack complete! %d islands packed across %d tile(s) (%.1f%% efficiency)",
        m_packStats.packedIslands, m_packStats.udimTilesUsed, m_packStats.packingEfficiency * 100.0f);
    m_uvStatusMessage = msg;
    m_logEntries.push_back(std::string("[UV Auto] ") + msg);
}

void StandaloneUIManager::ExportUnwrappedOBJ() {
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

void StandaloneUIManager::RenderUVCutAndPackPanel() {
    ImGui::Begin("UV Tool###uvpanel", &m_shellState.showUVPanel);

    // 1. One-Click Header Card
    BakerUI::SectionLabel("ONE-CLICK UV WORKFLOW");
    bool openAuto = BakerUI::BeginCard("##auto_uv_card");
    if (openAuto) {
        ImGui::TextWrapped("Automatically mark seams by angle/polygroup, unwrap conformal islands, and pack with multi-rotation MaxRects BSSF.");
        ImGui::Spacing();
        if (BakerUI::PrimaryButton("ONE-CLICK AUTO CUT, UNWRAP & PACK", -FLT_MIN, 36.0f)) {
            AutoCutUnwrapAndPack();
        }
        ImGui::Spacing();
        BakerUI::StatusPill(m_uvStatusMessage.c_str(), BakerUI::Col::Accent());
    }
    BakerUI::EndCard();

    // 2. Seam Cutting Section (Zen UV Inspired)
    BakerUI::SectionLabel("1. SEAM CUTTING");
    bool openSeams = BakerUI::BeginCard("##seam_card");
    if (openSeams) {
        if (BakerUI::BeginProps("##seam_props")) {
            BakerUI::PropFloat("Angle Threshold", &m_seamAngleThreshold, 0.5f, 5.0f, 175.0f, "%.1f deg",
                "Dihedral angle threshold between triangle face normals");
            BakerUI::EndProps();
        }

        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Mark Seams by Angle")) {
            MarkSeamsByAngle();
        }
        if (BakerUI::SecondaryButton("Mark Seams by Polygroup")) {
            MarkSeamsByPolygroup();
        }
        if (BakerUI::SecondaryButton("Detect from Current UV Borders")) {
            MarkSeamsByUVBorders();
        }

        ImGui::Spacing();
        bool isCutting = (m_toolMode == ToolMode::CutSeam);
        if (BakerUI::ToggleChip(isCutting ? "Exit Interactive Seam Mode" : "Interactive Seam Mode (Click Edges)", &isCutting)) {
            m_toolMode = isCutting ? ToolMode::CutSeam : ToolMode::Navigate;
            if (m_toolMode == ToolMode::CutSeam) {
                m_viewportRenderer.showSeams = true;
            }
        }

        ImGui::Spacing();
        if (ImGui::Button("Clear All Seams")) {
            ClearSeams();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Seams: %zu edges", m_lowPolyMesh.cutSeams.size());
    }
    BakerUI::EndCard();

    // 3. Unwrap Section
    BakerUI::SectionLabel("2. CONFORMAL UNWRAP");
    bool openUnwrap = BakerUI::BeginCard("##unwrap_card");
    if (openUnwrap) {
        if (BakerUI::BeginProps("##unwrap_props")) {
            BakerUI::PropInt("Spring Iterations", &m_relaxIterations, 5, 200, "Relaxation passes to minimize texture distortion");
            BakerUI::EndProps();
        }
        ImGui::Spacing();
        if (BakerUI::SecondaryButton("Unwrap Islands (Conformal + Relax)")) {
            UnwrapMesh();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Islands: %zu", m_currentIslands.size());
    }
    BakerUI::EndCard();

    // 4. Advanced Packing Section (UVPackmaster 4 Inspired)
    BakerUI::SectionLabel("3. ADVANCED PACKING (UVPACKMASTER)");
    bool openPack = BakerUI::BeginCard("##pack_card");
    if (openPack) {
        static const char* kResOptions[] = { "512", "1024", "2048", "4096", "8192", "16384" };
        static int resIdx = 2; // 2048
        if (BakerUI::BeginProps("##pack_props")) {
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
    }
    BakerUI::EndCard();

    // 5. Packing Statistics Card
    if (m_packStats.packedIslands > 0) {
        BakerUI::SectionLabel("PACKING METRICS");
        bool openStats = BakerUI::BeginCard("##stats_card");
        if (openStats) {
            ImGui::Text("Packed Islands: %d", m_packStats.packedIslands);
            ImGui::Text("UDIM Tiles Used: %d", m_packStats.udimTilesUsed);
            ImGui::Text("Packing Efficiency: %.1f%%", m_packStats.packingEfficiency * 100.0f);
            float udimProg = m_packStats.packingEfficiency;
            ImGui::ProgressBar(udimProg, ImVec2(-FLT_MIN, 0.0f));
        }
        BakerUI::EndCard();
    }

    // 6. Export Card
    BakerUI::SectionLabel("EXPORT MESH");
    bool openExp = BakerUI::BeginCard("##exp_card");
    if (openExp) {
        if (BakerUI::SecondaryButton("Export Low-Poly OBJ (with New UVs)...")) {
            ExportUnwrappedOBJ();
        }
    }
    BakerUI::EndCard();

    ImGui::End();
}

void StandaloneUIManager::RenderUDIMCanvasPanel() {
    ImGui::Begin("UDIM Layout###udim", &m_shellState.showUdim);
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    Render2DUVCanvas(pos, avail);
    ImGui::End();
}

void StandaloneUIManager::RenderLogPanel() {
    ImGui::Begin("Output Log###log", &m_shellState.showLog);
    if (ImGui::SmallButton("Clear Log")) {
        m_logEntries.clear();
    }
    ImGui::Separator();
    ImGui::BeginChild("LogScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& entry : m_logEntries) {
        if (entry.find("[Error]") != std::string::npos) {
            ImGui::TextColored(BakerUI::Col::Warn(), "%s", entry.c_str());
        } else if (entry.find("[Pass]") != std::string::npos || entry.find("success") != std::string::npos) {
            ImGui::TextColored(BakerUI::Col::Ok(), "%s", entry.c_str());
        } else {
            ImGui::TextUnformatted(entry.c_str());
        }
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void StandaloneUIManager::RenderBakeProgressPanel() {
    if (!m_isBaking) return;

    ImGui::OpenPopup("Bake Execution");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 180));

    if (ImGui::BeginPopupModal("Bake Execution", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Spacing();
        ImGui::Text("Baking Multi-UDIM Textures in Parallel...");
        ImGui::Spacing();
        ImGui::ProgressBar(m_bakeProgress, ImVec2(-1, 24), "");
        ImGui::Spacing();
        ImGui::Text("%s", m_bakeStatus.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Cancel Bake", ImVec2(-1, 0))) {
            m_isBaking = false;
        }
        ImGui::EndPopup();
    }
}

void StandaloneUIManager::TriggerBake() {
    if (!m_hasLowPoly || !m_hasHighPoly || m_isBaking || m_selectedUDIMs.empty()) return;

    m_isBaking = true;
    m_bakeProgress = 0.0f;
    m_bakeStatus = "Initializing Multi-UDIM Bake...";
    m_logEntries.push_back("[Bake] Initiating bake for " + std::to_string(m_selectedUDIMs.size()) + " UDIM tiles...");

    m_settings.outputDir = m_outDirBuf;
    m_settings.baseFilename = m_baseNameBuf;
    m_settings.targetUDIMs = m_selectedUDIMs;

    BakeSettings settingsCopy = m_settings;

    std::thread([this, settingsCopy]() {
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

        bool success = m_engine.ExecuteBake(m_lowPolyMesh, settingsCopy, results, progressCb, err, livePreviewCb);

        auto endTime = std::chrono::high_resolution_clock::now();
        double elapsedSec = std::chrono::duration<double>(endTime - startTime).count();

        if (success) {
            std::ostringstream oss;
            oss << "[Bake] Ray-tracing finished in " << elapsedSec << "s. Writing output files...";
            m_logEntries.push_back(oss.str());

            std::filesystem::path outFolder(settingsCopy.outputDir);
            if (settingsCopy.outputDir.empty() || settingsCopy.outputDir == "./" || settingsCopy.outputDir == ".\\") {
                outFolder = std::filesystem::current_path();
            }
            std::error_code ec;
            std::filesystem::create_directories(outFolder, ec);

            std::vector<std::string> allSavedFiles;

            for (const auto& res : results) {
                std::string udimStr = std::to_string(res.udimTile);

                // 1. EXR Formats (32-bit float Displacement)
                if (settingsCopy.bakeDisplacement && !res.displacementMap.empty()) {
                    std::string dispPath = (outFolder / (settingsCopy.baseFilename + "_Disp." + udimStr + ".exr")).string();
                    std::string exrErr;
                    if (ExrWriter::SaveFloatImage(dispPath, res.displacementMap.data(), res.width, res.height, exrErr)) {
                        m_logEntries.push_back("[EXR] Saved 32-bit displacement: " + dispPath);
                        allSavedFiles.push_back(dispPath);
                    }
                }

                // 2. PNG Formats (8-bit per channel - Instant Preview & Game Engines)
                if (settingsCopy.bakeNormal && !res.normalMapR.empty()) {
                    std::string normPngPath = (outFolder / (settingsCopy.baseFilename + "_Normal." + udimStr + ".png")).string();
                    std::string pngErr;
                    if (ExrWriter::SaveRGBPNG(normPngPath, res.normalMapR.data(), res.normalMapG.data(), res.normalMapB.data(), res.width, res.height, pngErr)) {
                        m_logEntries.push_back("[PNG] Saved 8-bit normal: " + normPngPath);
                        allSavedFiles.push_back(normPngPath);
                    }
                }

                if (settingsCopy.bakeAmbientOcclusion && !res.aoMap.empty()) {
                    std::string aoPngPath = (outFolder / (settingsCopy.baseFilename + "_AO." + udimStr + ".png")).string();
                    std::string pngErr;
                    if (ExrWriter::SaveGrayscalePNG(aoPngPath, res.aoMap.data(), res.width, res.height, pngErr)) {
                        m_logEntries.push_back("[PNG] Saved 8-bit AO: " + aoPngPath);
                        allSavedFiles.push_back(aoPngPath);
                    }
                }

                if (settingsCopy.bakeCavity && !res.cavityMap.empty()) {
                    std::string cavPngPath = (outFolder / (settingsCopy.baseFilename + "_Cavity." + udimStr + ".png")).string();
                    std::string pngErr;
                    if (ExrWriter::SaveGrayscalePNG(cavPngPath, res.cavityMap.data(), res.width, res.height, pngErr)) {
                        m_logEntries.push_back("[PNG] Saved 8-bit Cavity: " + cavPngPath);
                        allSavedFiles.push_back(cavPngPath);
                    }
                }

                if (settingsCopy.bakeCurvature && !res.curvatureMap.empty()) {
                    std::string curvPngPath = (outFolder / (settingsCopy.baseFilename + "_Curvature." + udimStr + ".png")).string();
                    std::string pngErr;
                    if (ExrWriter::SaveGrayscalePNG(curvPngPath, res.curvatureMap.data(), res.width, res.height, pngErr)) {
                        m_logEntries.push_back("[PNG] Saved 8-bit Curvature: " + curvPngPath);
                        allSavedFiles.push_back(curvPngPath);
                    }
                }

                if (settingsCopy.bakeDiagnosticMap && !res.diagnosticMapR.empty()) {
                    std::string diagPngPath = (outFolder / (settingsCopy.baseFilename + "_Diag." + udimStr + ".png")).string();
                    std::string pngErr;
                    if (ExrWriter::SaveRGBPNG(diagPngPath, res.diagnosticMapR.data(), res.diagnosticMapG.data(), res.diagnosticMapB.data(), res.width, res.height, pngErr)) {
                        m_logEntries.push_back("[PNG] Saved diagnostic map: " + diagPngPath);
                        allSavedFiles.push_back(diagPngPath);
                    }
                }
            }

            // 3. Low-Poly OBJ with exact matching baked vertex normals
            std::string lpObjPath = (outFolder / (settingsCopy.baseFilename + "_LowPoly_Baked.obj")).string();
            std::string objErr;
            if (MeshLoader::SaveLowPolyOBJWithNormals(lpObjPath, m_lowPolyMesh, objErr)) {
                m_logEntries.push_back("[OBJ] Exported baked low-poly mesh with matching normals: " + lpObjPath);
                allSavedFiles.push_back(lpObjPath);
            }

            if (!allSavedFiles.empty()) {
                auto syncTime = std::filesystem::file_time_type::clock::now();
                for (const auto& fp : allSavedFiles) {
                    try {
                        std::filesystem::last_write_time(fp, syncTime);
                    } catch (...) {}
                }
                m_logEntries.push_back("[System] Synchronized file timestamps for " + std::to_string(allSavedFiles.size()) + " baked maps.");
            }

            // Stage baked normal map for real-time 3D viewport preview
            if (settingsCopy.bakeNormal && !results.empty()) {
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

            char finishedBuf[128];
            snprintf(finishedBuf, sizeof(finishedBuf), "[Success] Bake completed in %.2f seconds! Output saved to %s",
                elapsedSec, settingsCopy.outputDir.c_str());
            m_logEntries.push_back(finishedBuf);
            m_bakeStatus = "Bake Complete!";
        } else {
            m_logEntries.push_back("[Error] Bake failed: " + err);
            m_bakeStatus = "Bake Failed";
        }

        m_bakeProgress = 1.0f;
        m_isBaking = false;
    }).detach();
}

void StandaloneUIManager::EnsureCageInitialized() {
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

void StandaloneUIManager::EnsureSkewInitialized() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    if (m_lowPolyMesh.vertexSkew.size() != m_lowPolyMesh.vertices.size()) {
        m_lowPolyMesh.vertexSkew.assign(m_lowPolyMesh.vertices.size(), 0.0f);
    }
    m_viewportRenderer.UpdateSkew(m_lowPolyMesh, 1.0f);
}

void StandaloneUIManager::ResetPaintedCage() {
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

void StandaloneUIManager::ResetPaintedSkew() {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;
    m_lowPolyMesh.vertexSkew.assign(m_lowPolyMesh.vertices.size(), 0.0f);
    m_viewportRenderer.UpdateSkew(m_lowPolyMesh, 1.0f);
}

} // namespace Baker
