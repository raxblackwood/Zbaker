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
    m_shellState.showLog = false; // the log is an overlay the user opens from the status bar
    InitCommandRegistry();
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

// Marking menu layout. Each wedge is a registry command; angle is screen-space degrees (0 = east, -90 = north).
struct MarkingWedge { const char* commandId; const char* label; float angleDeg; };
static const MarkingWedge kMarkingWedges[8] = {
    { "uv.cut",                  "Cut",               -90.0f  },
    { "select.edge_ring",        "Edge ring",         -45.0f  },
    { "select.edge_loop",        "Edge loop",           0.0f  },
    { "uv.auto_cut_unwrap_pack", "Auto UV",            45.0f  },
    { "uv.sew",                  "Sew",                90.0f  },
    { "select.clear",            "Clear selection",   135.0f  },
    { "uv.unwrap",               "Unwrap",            180.0f  },
    { "uv.camera_project",       "Camera projection", -135.0f  },
};

void UIManager::InitCommandRegistry() {
    using BakerUI::Surface;
    using BakerUI::Command;

    m_commandRegistry.Clear();

    // 1. File Commands
    m_commandRegistry.Register(Command{
        "file.new_session", "New Session (Clear Scene)", "Clear all active high-poly, low-poly, and baked data", "",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { ClearScene(); }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "file.open_low_poly", "Open Low-Poly...", "Open low-poly target mesh (OBJ, FBX)", "",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { m_shellState.openLowPoly = true; }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "file.open_high_poly", "Open High-Poly...", "Open high-poly sculpt (OBJ, FBX, BIN)", "",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { m_shellState.openHighPoly = true; }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "file.open_cage", "Open Cage...", "Open explicit morph cage mesh", "",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { m_shellState.openCage = true; }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "file.export_low_poly", "Export UV Low-Poly OBJ...", "Export unwrapped low-poly OBJ with UV coordinates", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "File", "Export",
        [this]() { ExportUnwrappedOBJ(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.vertices.empty(); },
        nullptr,
        []() { return "No low-poly mesh loaded to export"; }
    });
    m_commandRegistry.Register(Command{
        "file.load_zbrush_cache", "Load ZBrush Cache", "Scan for active ZBrush GoZ cache files", "",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { AutoDetectZBrushCache(true); }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "file.exit", "Exit", "Close and exit ZBrush UDIM Baker", "Alt+F4",
        static_cast<uint32_t>(Surface::Menu), "File", "",
        [this]() { PostQuitMessage(0); }, nullptr, nullptr, nullptr
    });

    // 2. Edit Commands
    m_commandRegistry.Register(Command{
        "edit.undo", "Undo", "Undo last mesh or seam action", "Ctrl+Z",
        static_cast<uint32_t>(Surface::Menu), "Edit", "",
        [this]() { PerformUndo(); },
        [this]() { return m_history.CanUndo(); },
        nullptr,
        []() { return "No action to undo"; }
    });
    m_commandRegistry.Register(Command{
        "edit.redo", "Redo", "Redo previously undone action", "Ctrl+Y",
        static_cast<uint32_t>(Surface::Menu), "Edit", "",
        [this]() { PerformRedo(); },
        [this]() { return m_history.CanRedo(); },
        nullptr,
        []() { return "No action to redo"; }
    });

    // 3. Selection Modes
    m_commandRegistry.Register(Command{
        "select.mode_vertex", "Vertex", "Select vertex components", "1",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "Select", "",
        [this]() { SetUVSelectionMode(UVSelectionMode::Vertex, "Vertex Selection Mode [Hotkey: 1]"); },
        nullptr,
        [this]() { return m_uvSelectionMode == UVSelectionMode::Vertex; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "select.mode_edge", "Edge", "Select edge components", "2",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "Select", "",
        [this]() { SetUVSelectionMode(UVSelectionMode::Edge, "Edge Selection Mode [Hotkey: 2]"); },
        nullptr,
        [this]() { return m_uvSelectionMode == UVSelectionMode::Edge; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "select.mode_face", "Face", "Select face components", "3",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "Select", "",
        [this]() { SetUVSelectionMode(UVSelectionMode::Face, "Face Selection Mode [Hotkey: 3]"); },
        nullptr,
        [this]() { return m_uvSelectionMode == UVSelectionMode::Face; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "select.mode_shell", "Shell", "Select uv shell components", "4",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "Select", "",
        [this]() { SetUVSelectionMode(UVSelectionMode::Shell, "UV Shell Selection Mode [Hotkey: 4]"); },
        nullptr,
        [this]() { return m_uvSelectionMode == UVSelectionMode::Shell; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "select.edge_loop", "Select Edge Loop", "Select continuous loop through connected quads", "Double-Click Edge",
        static_cast<uint32_t>(Surface::Menu | Surface::Marking), "Select", "",
        [this]() {
            auto edges = m_lowPolyMesh.selectedEdges;
            for (const auto& e : edges) UVCutter::SelectEdgeLoop(m_lowPolyMesh, e.first, e.second, true);
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Select Edge Loop");
        },
        [this]() { return !m_lowPolyMesh.selectedEdges.empty(); },
        nullptr,
        []() { return "Select an edge first"; }
    });
    m_commandRegistry.Register(Command{
        "select.edge_ring", "Select Edge Ring", "Select parallel edge ring across quads", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Marking), "Select", "",
        [this]() {
            auto edges = m_lowPolyMesh.selectedEdges;
            for (const auto& e : edges) UVCutter::SelectEdgeRing(m_lowPolyMesh, e.first, e.second, true);
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            PushHistoryState("Select Edge Ring");
        },
        [this]() { return !m_lowPolyMesh.selectedEdges.empty(); },
        nullptr,
        []() { return "Select an edge first"; }
    });
    m_commandRegistry.Register(Command{
        "select.seams", "Select Seams", "Select every marked seam edge", "",
        static_cast<uint32_t>(Surface::Menu), "Select", "",
        [this]() {
            UVCutter::SelectAllSeams(m_lowPolyMesh);
            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
            SetUVSelectionMode(UVSelectionMode::Edge, "Selected all seam edges");
            PushHistoryState("Select Seams");
        },
        [this]() { return !m_lowPolyMesh.cutSeams.empty(); },
        nullptr,
        []() { return "No seams marked yet"; }
    });
    m_commandRegistry.Register(Command{
        "select.clear", "Clear Selection", "Deselect all components", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Marking), "Select", "",
        [this]() { ClearUVSelection(); PushHistoryState("Clear Selection"); },
        [this]() { return HasUVComponentSelection(); },
        nullptr,
        []() { return "Nothing is selected"; }
    });

    // 4. Transform Tools (Gizmo)
    m_commandRegistry.Register(Command{
        "tool.select", "Select", "Selection box tool without gizmo handles", "",
        static_cast<uint32_t>(Surface::Header), "", "",
        [this]() { m_uvGizmoMode = UVGizmoMode::Select; },
        nullptr,
        [this]() { return m_uvGizmoMode == UVGizmoMode::Select; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "tool.move", "Move", "Move gizmo (translate UVs)", "",
        static_cast<uint32_t>(Surface::Header), "", "",
        [this]() { m_uvGizmoMode = UVGizmoMode::Translate; },
        nullptr,
        [this]() { return m_uvGizmoMode == UVGizmoMode::Translate; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "tool.rotate", "Rotate", "Rotate gizmo (rotate UVs)", "",
        static_cast<uint32_t>(Surface::Header), "", "",
        [this]() { m_uvGizmoMode = UVGizmoMode::Rotate; },
        nullptr,
        [this]() { return m_uvGizmoMode == UVGizmoMode::Rotate; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "tool.scale", "Scale", "Scale gizmo (scale UVs)", "",
        static_cast<uint32_t>(Surface::Header), "", "",
        [this]() { m_uvGizmoMode = UVGizmoMode::Scale; },
        nullptr,
        [this]() { return m_uvGizmoMode == UVGizmoMode::Scale; },
        nullptr
    });

    // 5. Layout Modes & Navigation
    m_commandRegistry.Register(Command{
        "view.layout_3d", "3D View", "Switch to 3D Viewport Only", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/Layout", "",
        [this]() { m_viewportMode = ViewportMode::View3D; },
        nullptr,
        [this]() { return m_viewportMode == ViewportMode::View3D; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.layout_split", "Split 3D | 2D", "Switch to Split 3D and 2D Viewports", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/Layout", "",
        [this]() { m_viewportMode = ViewportMode::SplitBoth; },
        nullptr,
        [this]() { return m_viewportMode == ViewportMode::SplitBoth; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.layout_2d", "2D UV", "Switch to 2D UV Viewport Only", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/Layout", "",
        [this]() { m_viewportMode = ViewportMode::View2D; },
        nullptr,
        [this]() { return m_viewportMode == ViewportMode::View2D; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.frame_all", "Frame All", "Fit and frame the hovered 3D viewport or 2D UV canvas", "F",
        static_cast<uint32_t>(Surface::Menu), "View", "",
        [this]() { 
            m_viewportRenderer.FrameAll();
            FrameAllUVs();
        },
        nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "view.frame_selected", "Frame Selected", "Fit and frame selected components in view", "",
        static_cast<uint32_t>(Surface::Menu), "View", "",
        [this]() { 
            FrameSelectedUVs();
        },
        nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "view.reset_view", "Reset View", "Reset 2D UV zoom to 1.0x and pan to center", "",
        static_cast<uint32_t>(Surface::Menu), "View", "",
        [this]() { m_uvZoom = 1.0f; m_uvPan = ImVec2(0.0f, 0.0f); },
        nullptr, nullptr, nullptr
    });

    // 6. Display Toggles (Popovers in Header)
    m_commandRegistry.Register(Command{
        "view.3d_low_poly", "Low-Poly Mesh", "Toggle Low-Poly 3D mesh rendering", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showLowPoly = !m_viewportRenderer.showLowPoly; },
        nullptr,
        [this]() { return m_viewportRenderer.showLowPoly; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_wireframe", "Wireframe", "Toggle quad wireframe display", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showLowPolyWireframe = !m_viewportRenderer.showLowPolyWireframe; },
        nullptr,
        [this]() { return m_viewportRenderer.showLowPolyWireframe; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_seams", "UV Seams", "Toggle 3D seam lines visualization", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showSeams = !m_viewportRenderer.showSeams; },
        nullptr,
        [this]() { return m_viewportRenderer.showSeams; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_grid", "Ground Grid", "Toggle 3D ground reference grid", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showGrid = !m_viewportRenderer.showGrid; },
        nullptr,
        [this]() { return m_viewportRenderer.showGrid; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_uv_checker", "UV Checker", "Show a UV checker pattern on the low-poly mesh", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showUVChecker = !m_viewportRenderer.showUVChecker; },
        nullptr,
        [this]() { return m_viewportRenderer.showUVChecker; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_uv_stretch", "UV Stretch", "Color the low-poly mesh by UV stretch: blue compressed, green even, red stretched", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showUVStretch = !m_viewportRenderer.showUVStretch; },
        nullptr,
        [this]() { return m_viewportRenderer.showUVStretch; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.uv_borders", "Texture Borders", "Toggle UV texture border / seam highlighting", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/UV Display", "",
        [this]() { m_showTextureBorders = !m_showTextureBorders; },
        nullptr,
        [this]() { return m_showTextureBorders; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.uv_shells", "Shell Shading", "Toggle pastel color coding for each UV island", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/UV Display", "",
        [this]() { m_showShellShading = !m_showShellShading; },
        nullptr,
        [this]() { return m_showShellShading; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.uv_checker", "Checker Grid", "Toggle 16x16 UV checker background grid", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/UV Display", "",
        [this]() { m_showUVChecker = !m_showUVChecker; },
        nullptr,
        [this]() { return m_showUVChecker; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.uv_distortion", "Distortion Heatmap", "Toggle area stretch and compression feedback", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/UV Display", "",
        [this]() { m_showDistortionHeatmap = !m_showDistortionHeatmap; },
        nullptr,
        [this]() { return m_showDistortionHeatmap; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.uv_wireframe", "UV Wireframe", "Toggle UV polygon wireframe display", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/UV Display", "",
        [this]() { m_showUVWireframe = !m_showUVWireframe; },
        nullptr,
        [this]() { return m_showUVWireframe; },
        nullptr
    });

    // 7. UV Operations (UV Toolkit)
    m_commandRegistry.Register(Command{
        "uv.cut", "Cut Seam", "Cut UV seams along selected edges", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV/Cut & Sew", "Cut & Sew",
        [this]() { CutCurrentUVSelection(); },
        [this]() { return !m_lowPolyMesh.selectedEdges.empty(); },
        nullptr,
        []() { return "No edges selected to cut"; }
    });
    m_commandRegistry.Register(Command{
        "uv.sew", "Sew Seam", "Sew and stitch selected seam edges together", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV/Cut & Sew", "Cut & Sew",
        [this]() { SewCurrentUVSelection(); },
        [this]() { return !m_lowPolyMesh.selectedEdges.empty(); },
        nullptr,
        []() { return "No edges selected to sew"; }
    });
    m_commandRegistry.Register(Command{
        "uv.clear_seams", "Clear All Seams", "Clear all marked UV seam lines on mesh", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV/Cut & Sew", "Cut & Sew",
        [this]() { ClearSeams(); },
        [this]() { return !m_lowPolyMesh.cutSeams.empty(); },
        nullptr,
        []() { return "No marked seams to clear"; }
    });
    m_commandRegistry.Register(Command{
        "uv.auto_seams", "Auto Seams", "Mark boundary seams by dihedral angle threshold", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew", "Cut & Sew",
        [this]() { MarkSeamsByAngle(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.mark_seams_polygroup", "Seams from Polygroups", "Mark a seam wherever two polygroups meet", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew", "Cut & Sew",
        [this]() { MarkSeamsByPolygroup(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.mark_seams_uv_borders", "Seams from UV Borders", "Mark seams along the current UV island borders", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew", "Cut & Sew",
        [this]() { MarkSeamsByUVBorders(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_new", "New Curve", "Add a surface curve and start drawing it on the 3D mesh", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            m_curveManager.AddCurve();
            m_toolMode = ToolMode::DrawCurve;
            m_viewportRenderer.showSeams = true;
        },
        [this]() { return m_hasLowPoly; },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_delete", "Delete Curve", "Delete the active surface curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { m_curveManager.RemoveCurve(m_curveManager.activeCurveIndex); },
        [this]() { return m_curveManager.GetActiveCurve() != nullptr; },
        nullptr,
        []() { return "No active curve"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_draw_mode", "Draw Mode", "Click the 3D mesh to add points to the active curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            m_toolMode = (m_toolMode == ToolMode::DrawCurve) ? ToolMode::Navigate : ToolMode::DrawCurve;
            if (m_toolMode == ToolMode::DrawCurve) m_viewportRenderer.showSeams = true;
        },
        [this]() { return m_hasLowPoly; },
        [this]() { return m_toolMode == ToolMode::DrawCurve; },
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_mark_seam", "Mark as Seam", "Mark the mesh edges closest to the active curve as seams. The mesh keeps its topology.", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { SnapActiveCurveToSeams(); },
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            return m_hasLowPoly && c && c->points.size() >= 2;
        },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_cut_mesh", "Cut Mesh", "Split the mesh exactly along the active curve and mark the new edges as seams", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { CutActiveCurveIntoTopology(); },
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            return m_hasLowPoly && c && c->points.size() >= 2;
        },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_mark_all", "Mark All as Seams", "Mark seams along every visible curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { BakeAllCurvesToSeams(false); },
        [this]() { return m_hasLowPoly && !m_curveManager.curves.empty(); },
        nullptr,
        []() { return "No curves to apply"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_cut_all", "Cut Mesh Along All", "Split the mesh along every visible curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { BakeAllCurvesToSeams(true); },
        [this]() { return m_hasLowPoly && !m_curveManager.curves.empty(); },
        nullptr,
        []() { return "No curves to apply"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_toggle_visible", "Show Curve", "Show or hide the active curve in the 3D viewport", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            if (SurfaceCurve* c = m_curveManager.GetActiveCurve()) c->visible = !c->visible;
        },
        [this]() { return m_curveManager.GetActiveCurve() != nullptr; },
        [this]() { const SurfaceCurve* c = m_curveManager.GetActiveCurve(); return c && c->visible; },
        []() { return "No active curve"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_show_all", "Show All Curves", "Show every curve in the 3D viewport", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { for (auto& c : m_curveManager.curves) c.visible = true; },
        [this]() { return !m_curveManager.curves.empty(); },
        nullptr,
        []() { return "No curves yet"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_hide_all", "Hide All Curves", "Hide every curve in the 3D viewport", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { for (auto& c : m_curveManager.curves) c.visible = false; },
        [this]() { return !m_curveManager.curves.empty(); },
        nullptr,
        []() { return "No curves yet"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_clear_all", "Clear All Curves", "Delete every surface curve (undoable)", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { ClearAllCurves(); },
        [this]() { return !m_curveManager.curves.empty(); },
        nullptr,
        []() { return "No curves to clear"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_delete_point", "Delete Point", "Delete the selected control point from the active curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() { m_curveManager.RemoveActivePoint(m_curveManager.selectedPointIndex); },
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            return c && m_curveManager.selectedPointIndex >= 0 && m_curveManager.selectedPointIndex < static_cast<int>(c->points.size());
        },
        nullptr,
        []() { return "Select a control point first"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_smooth_segment", "Smooth Segment", "Relax kinks between the From and To points", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            int p0 = 0, p1 = 0;
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (!c || !GetActiveCurveSegment(p0, p1)) return;
            PushHistoryState("Smooth Curve Segment");
            c->SmoothSegment(p0, p1, m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
            char msg[128];
            snprintf(msg, sizeof(msg), "Smoothed segment between P%d and P%d (%d iterations).", p0, p1, m_curveManager.smoothIterations);
            m_uvStatusMessage = msg;
            m_logEntries.push_back(std::string("[Curve Edit] ") + msg);
        },
        [this]() { int p0, p1; return GetActiveCurveSegment(p0, p1); },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_straighten_segment", "Straighten Segment", "Replace the path between the From and To points with a straight chord", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            int p0 = 0, p1 = 0;
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (!c || !GetActiveCurveSegment(p0, p1)) return;
            PushHistoryState("Straighten Curve Segment");
            c->StraightenSegment(p0, p1, m_lowPolyMesh);
            char msg[128];
            snprintf(msg, sizeof(msg), "Straightened segment between P%d and P%d.", p0, p1);
            m_uvStatusMessage = msg;
            m_logEntries.push_back(std::string("[Curve Edit] ") + msg);
        },
        [this]() { int p0, p1; return GetActiveCurveSegment(p0, p1); },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_smooth_all", "Smooth Curve", "Relax kinks along the whole active curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (!c) return;
            PushHistoryState("Smooth Entire Curve");
            c->SmoothEntireCurve(m_curveManager.smoothIterations, m_curveManager.smoothFactor, m_lowPolyMesh);
            m_uvStatusMessage = "Smoothed entire curve.";
            m_logEntries.push_back("[Curve Edit] Smoothed entire curve.");
        },
        [this]() { int p0, p1; return GetActiveCurveSegment(p0, p1); },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_straighten_all", "Straighten Curve", "Straighten every segment of the active curve", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (!c) return;
            PushHistoryState("Straighten Entire Curve");
            c->StraightenEntireCurve(m_lowPolyMesh);
            m_uvStatusMessage = "Straightened all segments on curve.";
            m_logEntries.push_back("[Curve Edit] Straightened all segments on curve.");
        },
        [this]() { int p0, p1; return GetActiveCurveSegment(p0, p1); },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.curve_recalc_path", "Recalculate Path", "Discard segment edits and re-trace the curve from the current camera", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Cut & Sew/Curve Cut", "Cut & Sew",
        [this]() {
            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (!c) return;
            c->ResetSegmentCustomization(0, c->points.size());
            DirectX::XMMATRIX worldMat = m_viewportRenderer.GetLowPolyWorldMatrix();
            DirectX::XMMATRIX viewProjMat = m_viewportRenderer.GetViewProjectionMatrix();
            c->UpdateSampledPath(m_lowPolyMesh, &worldMat, &viewProjMat, m_viewportRenderer.GetWidth(), m_viewportRenderer.GetHeight());
            m_uvStatusMessage = "Recalculated sampled curve path from camera.";
        },
        [this]() { int p0, p1; return GetActiveCurveSegment(p0, p1); },
        nullptr,
        []() { return "Active curve needs at least 2 points"; }
    });
    m_commandRegistry.Register(Command{
        "uv.unwrap", "Unwrap", "Unwrap islands with conformal mapping and pack them", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV/Unfold", "Unfold",
        [this]() { UnwrapMesh(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.flip_u", "Flip U", "Flip selected UV components horizontally along U-axis", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { FlipSelectedUVs(true); },
        [this]() { return HasUVComponentSelection(); },
        nullptr,
        []() { return "No UV components selected"; }
    });
    m_commandRegistry.Register(Command{
        "uv.flip_v", "Flip V", "Flip selected UV components vertically along V-axis", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { FlipSelectedUVs(false); },
        [this]() { return HasUVComponentSelection(); },
        nullptr,
        []() { return "No UV components selected"; }
    });
    m_commandRegistry.Register(Command{
        "uv.rot_ccw", "Rotate -90", "Rotate selected UVs 90 degrees counter-clockwise", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { RotateSelectedUVs(-90.0f); },
        [this]() { return HasUVComponentSelection(); },
        nullptr,
        []() { return "No UV components selected"; }
    });
    m_commandRegistry.Register(Command{
        "uv.rot_cw", "Rotate +90", "Rotate selected UVs 90 degrees clockwise", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { RotateSelectedUVs(90.0f); },
        [this]() { return HasUVComponentSelection(); },
        nullptr,
        []() { return "No UV components selected"; }
    });
    m_commandRegistry.Register(Command{
        "uv.straighten", "Straighten Shells", "Straighten and align UV island boundaries to grid axes", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { StraightenIslands(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.orient", "Orient to World Up", "Rotate islands to align with 3D mesh world up axis", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Transform", "Transform",
        [this]() { OrientIslandsToWorld(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.stack_identical", "Stack Identical Shells", "Stack UV islands with matching 3D geometry and area", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Stack & Overlap", "Stack & Overlap",
        [this]() { StackIdenticalIslands(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.separate_overlaps", "Separate Overlaps", "Offset overlapping islands by +1 tile for baking", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Stack & Overlap", "Stack & Overlap",
        [this]() { SeparateOverlappingIslands(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.td_get", "Measure Density", "Measure the current average texel density of the UV layout", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Texel Density", "Texel Density",
        [this]() { CalculateTexelDensity(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.td_set", "Set Density", "Scale islands to match the target px/cm", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Texel Density", "Texel Density",
        [this]() { SetTargetTexelDensity(m_targetTexelDensity); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.td_equalize", "Equalize Density", "Equalize texel density across all islands", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Texel Density", "Texel Density",
        [this]() { EqualizeTexelDensity(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.relax", "Relax (Reduce Stretch)", "Reduce UV stretch and shear in place without folding triangles. Click again to relax further.", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Texel Density", "Texel Density",
        [this]() { RelaxUVs(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.snap_pixels", "Snap Islands to Pixels", "Move each island so its corner sits on a texel boundary of the pack resolution", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Pack", "Pack",
        [this]() { SnapIslandsToPixels(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.pack", "Pack Islands", "Pack UV islands across UDIM tiles", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Pack", "Pack",
        [this]() { PackUVs(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    m_commandRegistry.Register(Command{
        "uv.auto_cut_unwrap_pack", "Auto UV: Cut, Unwrap, Pack", "Segment into charts, unwrap, and pack into UDIMs", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV", "Auto UV",
        [this]() { AutoCutUnwrapAndPack(); },
        [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); },
        nullptr,
        []() { return "Requires low-poly mesh"; }
    });

    // Projection and alignment (Maya UV Editor: Create / Align)
    const auto hasMesh = [this]() { return m_hasLowPoly && !m_lowPolyMesh.triangles.empty(); };
    m_commandRegistry.Register(Command{
        "uv.camera_project", "Camera Projection", "Project UVs from the current 3D camera view. Acts on the selected faces, or the whole mesh.", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Toolkit | Surface::Marking), "UV/Project", "Unfold",
        [this]() { ApplyCameraProjection(); },
        hasMesh, nullptr,
        []() { return "Requires low-poly mesh"; }
    });
    static const char* kAxisNames[3] = { "X", "Y", "Z" };
    for (int axis = 0; axis < 3; ++axis) {
        m_commandRegistry.Register(Command{
            std::string("uv.project_") + static_cast<char>('x' + axis), std::string("Planar ") + kAxisNames[axis],
            std::string("Project UVs flat along the ") + kAxisNames[axis] + " axis. Acts on the selected faces, or the whole mesh.", "",
            static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Project", "Unfold",
            [this, axis]() { PlanarProjectUVs(axis); },
            hasMesh, nullptr,
            []() { return "Requires low-poly mesh"; }
        });
    }
    static const char* kAlignIds[6]    = { "uv.align_left", "uv.align_right", "uv.align_top", "uv.align_bottom", "uv.align_center_u", "uv.align_center_v" };
    static const char* kAlignLabels[6] = { "Align Left", "Align Right", "Align Top", "Align Bottom", "Center Horizontally", "Center Vertically" };
    for (int mode = 0; mode < 6; ++mode) {
        m_commandRegistry.Register(Command{
            kAlignIds[mode], kAlignLabels[mode], "Align the selected UVs to the edge or center of their bounding box", "",
            static_cast<uint32_t>(Surface::Menu | Surface::Toolkit), "UV/Align", "Transform",
            [this, mode]() { AlignSelectedUVs(mode); },
            [this]() { return HasUVComponentSelection(); }, nullptr,
            []() { return "No UV components selected"; }
        });
    }

    // Marking menu: every wedge must be a registered command (the menu draws and runs them through the registry)
    for (const MarkingWedge& w : kMarkingWedges) {
        assert(m_commandRegistry.Find(w.commandId) && "Marking menu wedge references an unregistered command");
    }

    // 8. Scene objects (Outliner rows and Properties panel)
    m_commandRegistry.Register(Command{
        "view.3d_high_poly", "High-Poly Sculpt", "Show or hide the high-poly sculpt in the 3D viewport", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showHighPoly = !m_viewportRenderer.showHighPoly; },
        nullptr,
        [this]() { return m_viewportRenderer.showHighPoly; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "view.3d_cage", "Cage", "Show or hide the bake cage in the 3D viewport", "",
        static_cast<uint32_t>(Surface::Menu | Surface::Header), "View/3D Display", "",
        [this]() { m_viewportRenderer.showCage = !m_viewportRenderer.showCage; },
        nullptr,
        [this]() { return m_viewportRenderer.showCage; },
        nullptr
    });
    m_commandRegistry.Register(Command{
        "scene.remove_high_poly", "Remove from Scene", "Unload the high-poly sculpt. This cannot be undone.", "",
        static_cast<uint32_t>(Surface::None), "", "",
        [this]() { ClearHighPoly(); },
        [this]() { return m_hasHighPoly && !m_isLoadingHighPoly; },
        nullptr,
        []() { return "Wait for the sculpt to finish loading"; }
    });
    m_commandRegistry.Register(Command{
        "scene.remove_low_poly", "Remove from Scene", "Unload the low-poly mesh and its UVs. This cannot be undone.", "",
        static_cast<uint32_t>(Surface::None), "", "",
        [this]() { ClearLowPoly(); },
        [this]() { return m_hasLowPoly; },
        nullptr,
        []() { return "No low-poly mesh loaded"; }
    });
    m_commandRegistry.Register(Command{
        "scene.remove_cage", "Remove Cage File", "Drop the explicit cage and return to the push-offset cage", "",
        static_cast<uint32_t>(Surface::None), "", "",
        [this]() { ClearCage(); },
        [this]() { return m_hasExplicitCage; },
        nullptr,
        []() { return "The push-offset cage is generated from the low-poly mesh"; }
    });

    // 9. Help
    m_commandRegistry.Register(Command{
        "help.shortcuts", "Keyboard Shortcuts", "List every command that has a shortcut", "",
        static_cast<uint32_t>(Surface::Menu), "Help", "",
        [this]() { m_showShortcutsWindow = true; }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "help.gestures", "Viewport Gestures", "Mouse gestures and the marking menu layout", "",
        static_cast<uint32_t>(Surface::Menu), "Help", "",
        [this]() { m_showGesturesWindow = true; }, nullptr, nullptr, nullptr
    });
    m_commandRegistry.Register(Command{
        "help.about", "About ZBrush UDIM Baker", "Version and build information", "",
        static_cast<uint32_t>(Surface::Menu), "Help", "",
        [this]() { m_showAboutWindow = true; }, nullptr, nullptr, nullptr
    });
}

void UIManager::SetUVSelectionMode(UVSelectionMode mode, const char* statusText) {
    m_uvSelectionMode = mode;
    if (m_shellState.step == BakerUI::ShellState::Step::UV) {
        m_toolMode = ToolMode::CutSeam;
    }
    m_uvStatusMessage = statusText;
}

// From/To control points used by the segment edit commands (defaults to the selected point and its neighbour)
bool UIManager::GetActiveCurveSegment(int& p0, int& p1) {
    SurfaceCurve* c = m_curveManager.GetActiveCurve();
    if (!c || c->points.size() < 2) return false;
    int count = static_cast<int>(c->points.size());
    p0 = (m_curveManager.selectedPointIndex >= 0) ? m_curveManager.selectedPointIndex : 0;
    if (p0 >= count) p0 = count - 1;
    int defaultP1 = (p0 + 1 < count) ? (p0 + 1) : (c->isClosed ? 0 : p0);
    p1 = (m_curveManager.selectedPointEndIndex >= 0) ? m_curveManager.selectedPointEndIndex : defaultP1;
    if (p1 >= count) p1 = count - 1;
    return true;
}

bool UIManager::HasUVComponentSelection() const {
    return !m_selectedUVVertices.empty() || !m_lowPolyMesh.selectedEdges.empty() || !m_selectedUVFaces.empty();
}

void UIManager::FrameAllUVs() {
    if (m_lowPolyMesh.vertices.empty()) return;
    float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
    for (const auto& v : m_lowPolyMesh.vertices) {
        minU = (std::min)(minU, v.uv.u); minV = (std::min)(minV, v.uv.v);
        maxU = (std::max)(maxU, v.uv.u); maxV = (std::max)(maxV, v.uv.v);
    }
    float rangeU = (std::max)(maxU - minU, 0.05f);
    float rangeV = (std::max)(maxV - minV, 0.05f);
    float headerH = 30.0f;
    ImVec2 size = ImGui::GetWindowSize();
    float availW = size.x * 0.85f;
    float availH = (size.y - headerH) * 0.85f;
    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float newTileSize = (std::min)(availW / rangeU, availH / rangeV);
    m_uvZoom = std::clamp(newTileSize / baseTileSize, 0.05f, 50.0f);
    float currentTileSize = baseTileSize * m_uvZoom;
    float midU = (minU + maxU) * 0.5f;
    float midV = (minV + maxV) * 0.5f;
    m_uvPan.x = (size.x * 0.5f) - midU * currentTileSize;
    m_uvPan.y = (headerH + (size.y - headerH) * 0.5f) - (1.0f - midV) * currentTileSize;
}

void UIManager::FrameSelectedUVs() {
    if (m_lowPolyMesh.vertices.empty()) return;

    std::vector<uint32_t> verts;
    if (m_uvSelectionMode == UVSelectionMode::Vertex) {
        verts.assign(m_selectedUVVertices.begin(), m_selectedUVVertices.end());
    } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
        for (const auto& edge : m_lowPolyMesh.selectedEdges) {
            verts.push_back(edge.first);
            verts.push_back(edge.second);
        }
    } else { // Face or Shell
        for (uint32_t f : m_selectedUVFaces) {
            if (f < m_lowPolyMesh.triangles.size()) {
                const auto& tri = m_lowPolyMesh.triangles[f];
                verts.push_back(tri.indices[0]);
                verts.push_back(tri.indices[1]);
                verts.push_back(tri.indices[2]);
            }
        }
    }

    if (verts.empty()) {
        FrameAllUVs();
        return;
    }

    float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
    for (uint32_t vIdx : verts) {
        if (vIdx < m_lowPolyMesh.vertices.size()) {
            const auto& v = m_lowPolyMesh.vertices[vIdx];
            minU = (std::min)(minU, v.uv.u); minV = (std::min)(minV, v.uv.v);
            maxU = (std::max)(maxU, v.uv.u); maxV = (std::max)(maxV, v.uv.v);
        }
    }

    float rangeU = (std::max)(maxU - minU, 0.05f);
    float rangeV = (std::max)(maxV - minV, 0.05f);
    float headerH = 30.0f;
    ImVec2 size = ImGui::GetWindowSize();
    float availW = size.x * 0.85f;
    float availH = (size.y - headerH) * 0.85f;
    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float newTileSize = (std::min)(availW / rangeU, availH / rangeV);
    m_uvZoom = std::clamp(newTileSize / baseTileSize, 0.05f, 50.0f);
    float currentTileSize = baseTileSize * m_uvZoom;
    float midU = (minU + maxU) * 0.5f;
    float midV = (minV + maxV) * 0.5f;
    m_uvPan.x = (size.x * 0.5f) - midU * currentTileSize;
    m_uvPan.y = (headerH + (size.y - headerH) * 0.5f) - (1.0f - midV) * currentTileSize;
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
    m_bakeStatus = "Ready";
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

        // Blender Component Selection Modes: 1 = Vertex, 2 = Edge, 3 = Face, 4 = UV Shell
        if (!io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
            if (ImGui::IsKeyPressed(ImGuiKey_1, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad1, false)) {
                SetUVSelectionMode(UVSelectionMode::Vertex, "Vertex Selection Mode [Hotkey: 1]");
            } else if (ImGui::IsKeyPressed(ImGuiKey_2, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad2, false)) {
                SetUVSelectionMode(UVSelectionMode::Edge, "Edge Selection Mode [Hotkey: 2]");
            } else if (ImGui::IsKeyPressed(ImGuiKey_3, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad3, false)) {
                SetUVSelectionMode(UVSelectionMode::Face, "Face Selection Mode [Hotkey: 3]");
            } else if (ImGui::IsKeyPressed(ImGuiKey_4, false) || ImGui::IsKeyPressed(ImGuiKey_Keypad4, false)) {
                SetUVSelectionMode(UVSelectionMode::Shell, "UV Shell Selection Mode [Hotkey: 4]");
            }
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
    BakerUI::BeginStageHost(m_shellState, m_commandRegistry);

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
    if (m_shellState.exportLowPoly) {
        m_shellState.exportLowPoly = false;
        ExportUnwrappedOBJ();
    }

    // Keep the stepper's completion marks current
    m_shellState.stepDone[0] = m_hasLowPoly && m_hasHighPoly;
    m_shellState.stepDone[1] = m_hasLowPoly && !m_lowPolyMesh.detectedUDIMs.empty();
    m_shellState.stepDone[2] = (m_bakeStatus == "Complete!");

    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !io.WantTextInput) m_shellState.triggerBake = true;

    // Step transition: configure the viewport for the new stage
    if (m_shellState.step != m_prevStep) {
        if (m_shellState.step == BakerUI::ShellState::Step::UV) {
            m_viewportMode = ViewportMode::SplitBoth;
            m_toolMode = ToolMode::CutSeam;
            m_viewportRenderer.showHighPoly = false; // expose the low-poly
            m_viewportRenderer.showCage = false;
            m_viewportRenderer.showLowPoly = true;
            m_viewportRenderer.showLowPolyWireframe = true;
            m_viewportRenderer.showSeams = true;
        } else {
            m_viewportMode = ViewportMode::View3D;
            m_toolMode = ToolMode::Navigate;
            m_viewportRenderer.showHighPoly = true;
            m_viewportRenderer.showCage = false;
            m_viewportRenderer.showLowPoly = true;
        }
        m_prevStep = m_shellState.step;
    }

    RenderViewportPanel();
    RenderScenePanel();
    RenderActionPanel();
    if (m_shellState.showLog) RenderBakeProgressPanel();

    RenderHelpWindows();

    BakerUI::EndFrameHost();
}

// UV step: the frequent operations sit in one row at the top and every other stage is a collapsible section
// below. Auto UV, the step's single primary action (R-3), is pinned to the bottom by RenderInspector.
// Every button goes through BakerUI::DrawCommand; parameters stay as plain property rows.
void UIManager::RenderUVStep() {
    using BakerUI::CommandStyle;
    using BakerUI::DrawCommand;

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 fullW(-FLT_MIN, 0.0f);
    auto halfW = [&style]() { return ImVec2((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f, 0.0f); };
    auto drawPair = [&](const char* left, const char* right) {
        DrawCommand(m_commandRegistry, left, nullptr, halfW());
        ImGui::SameLine();
        DrawCommand(m_commandRegistry, right, nullptr, fullW);
    };
    if (!m_hasLowPoly) {
        ImGui::TextDisabled("Load a low-poly mesh to use the UV tools.");
    }

    {
        const float quarterW = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 3.0f) * 0.25f;
        const float rowH = ImGui::GetFrameHeight() * 1.25f;
        DrawCommand(m_commandRegistry, "uv.cut", "Cut", ImVec2(quarterW, rowH));
        ImGui::SameLine();
        DrawCommand(m_commandRegistry, "uv.sew", "Sew", ImVec2(quarterW, rowH));
        ImGui::SameLine();
        DrawCommand(m_commandRegistry, "uv.unwrap", "Unwrap", ImVec2(quarterW, rowH));
        ImGui::SameLine();
        DrawCommand(m_commandRegistry, "uv.pack", "Pack", ImVec2(-FLT_MIN, rowH));
        ImGui::Spacing();
    }
    {
        if (BakerUI::BeginSectionCard("Seams", "##uvt_seams", true)) {
            if (BakerUI::BeginProps("##uvt_auto_seams_props")) {
                BakerUI::PropFloat("Angle Crease", &m_seamAngleThreshold, 1.0f, 15.0f, 90.0f, "%.1f deg", "Edges sharper than this dihedral angle become seams");
                BakerUI::EndProps();
            }
            DrawCommand(m_commandRegistry, "uv.auto_seams", "Mark Seams by Angle", fullW);
            drawPair("uv.mark_seams_polygroup", "uv.mark_seams_uv_borders");
            DrawCommand(m_commandRegistry, "uv.clear_seams", nullptr, fullW, CommandStyle::Destructive);
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Curves", "##uvt_curvecut", true)) {
            if (m_curveManager.curves.empty()) {
                ImGui::TextDisabled("No curves yet. New Curve starts drawing on the 3D mesh.");
            } else {
                const float listH = ImGui::GetFrameHeightWithSpacing() * static_cast<float>((std::min)(m_curveManager.curves.size(), static_cast<size_t>(5))) + style.WindowPadding.y * 2.0f;
                if (ImGui::BeginChild("##uvt_curve_list", ImVec2(0.0f, listH), ImGuiChildFlags_Borders)) {
                    for (size_t i = 0; i < m_curveManager.curves.size(); ++i) {
                        SurfaceCurve& curve = m_curveManager.curves[i];
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::Checkbox("##visible", &curve.visible);
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show in the 3D viewport");
                        ImGui::SameLine();
                        char rowLabel[128];
                        snprintf(rowLabel, sizeof(rowLabel), "%s   %zu pts%s##row", curve.name.c_str(), curve.points.size(), curve.isBaked ? "   applied" : "");
                        if (ImGui::Selectable(rowLabel, static_cast<int>(i) == m_curveManager.activeCurveIndex)) {
                            m_curveManager.activeCurveIndex = static_cast<int>(i);
                            m_curveManager.selectedPointIndex = -1;
                            m_curveManager.selectedPointEndIndex = -1;
                        }
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
            }
            if (SurfaceCurve* active = m_curveManager.GetActiveCurve()) {
                if (BakerUI::BeginProps("##uvt_curve_props")) {
                    BakerUI::PropCheck("Close Loop", &active->isClosed, "Connect the last point back to the first");
                    BakerUI::EndProps();
                }
            }
            drawPair("uv.curve_new", "uv.curve_delete");
            DrawCommand(m_commandRegistry, "uv.curve_draw_mode", nullptr, fullW);
            drawPair("uv.curve_show_all", "uv.curve_hide_all");

            ImGui::SeparatorText("Apply to mesh");
            drawPair("uv.curve_mark_seam", "uv.curve_cut_mesh");
            drawPair("uv.curve_mark_all", "uv.curve_cut_all");
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("Mark as Seam keeps the topology. Cut Mesh splits the polygons along the curve.");
            ImGui::PopStyleColor();
            DrawCommand(m_commandRegistry, "uv.curve_clear_all", nullptr, fullW, CommandStyle::Destructive);

            SurfaceCurve* c = m_curveManager.GetActiveCurve();
            if (c && !c->points.empty() && ImGui::TreeNodeEx("Edit Points##uvt_edit_points", ImGuiTreeNodeFlags_SpanAvailWidth)) {
                const float rowH = ImGui::GetTextLineHeightWithSpacing();
                const float listH = rowH * static_cast<float>((std::min)(c->points.size(), static_cast<size_t>(6))) + style.WindowPadding.y * 2.0f;
                if (ImGui::BeginChild("##uvt_curve_points", ImVec2(0.0f, listH), ImGuiChildFlags_Borders)) {
                    for (size_t p = 0; p < c->points.size(); ++p) {
                        char rowLabel[96];
                        snprintf(rowLabel, sizeof(rowLabel), "P%zu    %.2f, %.2f, %.2f##pt%zu", p,
                                 c->points[p].position.x, c->points[p].position.y, c->points[p].position.z, p);
                        bool isStart = (m_curveManager.selectedPointIndex == static_cast<int>(p));
                        bool isEnd = (m_curveManager.selectedPointEndIndex == static_cast<int>(p));
                        if (ImGui::Selectable(rowLabel, isStart || isEnd)) {
                            if (ImGui::GetIO().KeyShift) {
                                m_curveManager.selectedPointEndIndex = static_cast<int>(p);
                            } else {
                                m_curveManager.selectedPointIndex = static_cast<int>(p);
                                m_curveManager.selectedPointEndIndex = -1;
                            }
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::TextDisabled("Click selects From. Shift+click selects To.");
                DrawCommand(m_commandRegistry, "uv.curve_delete_point", nullptr, fullW, CommandStyle::Destructive);

                int p0 = 0, p1 = 0;
                if (GetActiveCurveSegment(p0, p1)) {
                    int ptMax = static_cast<int>(c->points.size()) - 1;
                    if (BakerUI::BeginProps("##uvt_curve_edit_props")) {
                        if (BakerUI::PropInt("From Point", &p0, 0, ptMax, "Start control point of the segment to edit")) m_curveManager.selectedPointIndex = p0;
                        if (BakerUI::PropInt("To Point", &p1, 0, ptMax, "End control point of the segment to edit")) m_curveManager.selectedPointEndIndex = p1;
                        BakerUI::PropInt("Smooth Iterations", &m_curveManager.smoothIterations, 1, 50, "Laplacian relaxation passes");
                        BakerUI::PropFloat("Smooth Strength", &m_curveManager.smoothFactor, 0.01f, 0.05f, 1.0f, "%.2f", "Smoothing blend weight (0.50 recommended)");
                        BakerUI::EndProps();
                    }
                    drawPair("uv.curve_smooth_segment", "uv.curve_straighten_segment");
                    drawPair("uv.curve_smooth_all", "uv.curve_straighten_all");
                    DrawCommand(m_commandRegistry, "uv.curve_recalc_path", nullptr, fullW);
                }
                ImGui::TreePop();
            }
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Transform", "##uvt_transform", false)) {
            drawPair("uv.flip_u", "uv.flip_v");
            drawPair("uv.rot_ccw", "uv.rot_cw");
            drawPair("uv.straighten", "uv.orient");
            ImGui::SeparatorText("Align");
            drawPair("uv.align_left", "uv.align_right");
            drawPair("uv.align_top", "uv.align_bottom");
            drawPair("uv.align_center_u", "uv.align_center_v");
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Project", "##uvt_project", false)) {
            DrawCommand(m_commandRegistry, "uv.camera_project", nullptr, fullW);
            {
                const float thirdW = (ImGui::GetContentRegionAvail().x - style.ItemSpacing.x * 2.0f) / 3.0f;
                DrawCommand(m_commandRegistry, "uv.project_x", nullptr, ImVec2(thirdW, 0.0f));
                ImGui::SameLine();
                DrawCommand(m_commandRegistry, "uv.project_y", nullptr, ImVec2(thirdW, 0.0f));
                ImGui::SameLine();
                DrawCommand(m_commandRegistry, "uv.project_z", nullptr, ImVec2(-FLT_MIN, 0.0f));
            }
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Stack & Overlap", "##uvt_stackoverlap", false)) {
            if (BakerUI::BeginProps("##uvt_stack_props")) {
                BakerUI::PropCheck("Lock Overlaps", &m_lockOverlapping, "Treat stacked identical islands as a single unit during pack");
                BakerUI::EndProps();
            }
            drawPair("uv.stack_identical", "uv.separate_overlaps");
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Texel Density", "##uvt_texeldensity", false)) {
            if (BakerUI::BeginProps("##uvt_td_props")) {
                BakerUI::PropFloat("Target (px/cm)", &m_targetTexelDensity, 0.5f, 0.1f, 100.0f, "%.2f px/cm", "Desired texel density per world centimetre");
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted("Measured");
                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                if (m_calculatedTexelDensity > 0.0f) {
                    ImGui::Text("%.2f px/cm", m_calculatedTexelDensity);
                } else {
                    ImGui::TextDisabled("Not measured");
                }
                BakerUI::EndProps();
            }
            drawPair("uv.td_get", "uv.td_set");
            DrawCommand(m_commandRegistry, "uv.td_equalize", nullptr, fullW);
            DrawCommand(m_commandRegistry, "uv.relax", nullptr, fullW);
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Pack Options", "##uvt_packoptions", false)) {
            if (BakerUI::BeginProps("##uvt_pack_props")) {
                BakerUI::PropCheck("Multi-UDIM", &m_packSettings.multiUDIM, "Pack across tiles 1001, 1002, 1003...");
                if (m_packSettings.multiUDIM) {
                    BakerUI::PropInt("Max Tiles", &m_packSettings.maxUDIMCount, 1, 10, "Maximum number of UDIM tiles to fill");
                }
                BakerUI::PropInt("Island Padding (px)", &m_packSettings.paddingPixels, 0, 64, "Spacing between UV islands in pixels");
                static const char* kRotOptions[] = { "None", "90 deg Steps", "45 deg Steps", "Free Rotation" };
                int rotIdx = static_cast<int>(m_packSettings.rotation);
                if (BakerUI::PropCombo("Rotation Steps", &rotIdx, kRotOptions, IM_ARRAYSIZE(kRotOptions))) {
                    m_packSettings.rotation = static_cast<PackSettings::RotationMode>(rotIdx);
                }
                BakerUI::EndProps();
            }
            if (ImGui::TreeNodeEx("Advanced##uvt_pack_adv", ImGuiTreeNodeFlags_SpanAvailWidth)) {
                static const int kResValues[] = { 512, 1024, 2048, 4096, 8192, 16384 };
                static const char* kResOptions[] = { "512", "1024", "2048", "4096", "8192", "16384" };
                int resIdx = 2;
                for (int i = 0; i < IM_ARRAYSIZE(kResValues); ++i) {
                    if (kResValues[i] == m_packSettings.resolution) resIdx = i;
                }
                if (BakerUI::BeginProps("##uvt_pack_adv_props")) {
                    if (BakerUI::PropCombo("Resolution", &resIdx, kResOptions, IM_ARRAYSIZE(kResOptions))) {
                        m_packSettings.resolution = kResValues[resIdx];
                    }
                    BakerUI::PropInt("Tile Margin (px)", &m_packSettings.marginPixels, 0, 64, "Margin from the outer UDIM border in pixels");
                    BakerUI::PropCheck("Normalize Texel Density", &m_packSettings.normalizeTexelDensity, "Scale islands by 3D surface area before packing");
                    BakerUI::PropCheck("Group by Polygroup", &m_packSettings.packByPolygroup, "Keep islands of the same polygroup on dedicated tiles");
                    BakerUI::EndProps();
                }
                ImGui::TreePop();
            }
            DrawCommand(m_commandRegistry, "uv.snap_pixels", nullptr, fullW);
            if (m_packStats.packedIslands > 0) {
                ImGui::SeparatorText("Last Pack");
                ImGui::TextDisabled("%d islands on %d tile(s), %.1f%% efficient",
                                    m_packStats.packedIslands, m_packStats.udimTilesUsed, m_packStats.packingEfficiency * 100.0f);
                ImGui::ProgressBar(m_packStats.packingEfficiency, ImVec2(-FLT_MIN, 0.0f), "");
            }
            BakerUI::EndSectionCard();
        }

        if (BakerUI::BeginSectionCard("Export", "##uvt_export", false)) {
            DrawCommand(m_commandRegistry, "file.export_low_poly", "Export Mesh (OBJ)...", fullW);
            BakerUI::EndSectionCard();
        }
    }
}

void UIManager::Render2DUVCanvas(const ImVec2& origin, const ImVec2& size) {
    if (size.x < 50.0f || size.y < 50.0f) return;

    ImGui::SetCursorScreenPos(origin);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(38, 38, 41, 255));
    bool childOpen = ImGui::BeginChild("##uv_canvas_viewport", size, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!childOpen) {
        ImGui::EndChild();
        return;
    }

    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Deep dark background matching Blender UV Editor
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(38, 38, 41, 255));

    // Clip to canvas rectangle
    drawList->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Pan and Zoom interaction
    ImGuiIO& io = ImGui::GetIO();
    bool isHovered = (io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + size.x &&
                      io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + size.y);

    float baseTileSize = (std::min)(size.x, size.y) * 0.72f;
    if (baseTileSize < 120.0f) baseTileSize = 300.0f;
    float currentTileSize = baseTileSize * m_uvZoom;

    float headerH = 30.0f;
    bool inCanvas = isHovered && (io.MousePos.y > origin.y + headerH) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    // Helper functions for true UV <-> Screen mapping
    auto UVToScreen = [&](float u, float v) -> ImVec2 {
        return ImVec2(origin.x + m_uvPan.x + u * currentTileSize,
                      origin.y + m_uvPan.y + (1.0f - v) * currentTileSize);
    };

    auto FrameAllUVs = [&]() {
        if (m_lowPolyMesh.vertices.empty()) return;
        float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
        for (const auto& v : m_lowPolyMesh.vertices) {
            minU = (std::min)(minU, v.uv.u); minV = (std::min)(minV, v.uv.v);
            maxU = (std::max)(maxU, v.uv.u); maxV = (std::max)(maxV, v.uv.v);
        }
        float rangeU = (std::max)(maxU - minU, 0.05f);
        float rangeV = (std::max)(maxV - minV, 0.05f);
        float availW = size.x * 0.85f;
        float availH = (size.y - headerH) * 0.85f;
        float newTileSize = (std::min)(availW / rangeU, availH / rangeV);
        m_uvZoom = std::clamp(newTileSize / baseTileSize, 0.05f, 50.0f);
        currentTileSize = baseTileSize * m_uvZoom;
        float midU = (minU + maxU) * 0.5f;
        float midV = (minV + maxV) * 0.5f;
        m_uvPan.x = (size.x * 0.5f) - midU * currentTileSize;
        m_uvPan.y = (headerH + (size.y - headerH) * 0.5f) - (1.0f - midV) * currentTileSize;
    };

    // Shortcut: F key to Frame All in 2D UV canvas
    if (inCanvas && ImGui::IsKeyPressed(ImGuiKey_F)) {
        FrameAllUVs();
    }

    if (inCanvas) {
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
    std::vector<int> tilesToDraw = { 1001 };
    for (int t : m_lowPolyMesh.detectedUDIMs) {
        if (t >= 1001 && t <= 1099) {
            if (std::find(tilesToDraw.begin(), tilesToDraw.end(), t) == tilesToDraw.end()) {
                tilesToDraw.push_back(t);
            }
        }
    }

    // Draw active tile canvases & subgrids
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

        // Tile fill
        drawList->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(48, 48, 52, 255));

        // UV Checker Pattern (optional Maya/Blender style)
        if (m_showUVChecker && currentTileSize > 30.0f) {
            int checkRes = 16;
            float cStep = currentTileSize / checkRes;
            for (int cy = 0; cy < checkRes; ++cy) {
                for (int cx = 0; cx < checkRes; ++cx) {
                    if ((cx + cy) % 2 == 1) {
                        float px0 = x0 + cx * cStep;
                        float py0 = y0 + cy * cStep;
                        drawList->AddRectFilled(ImVec2(px0, py0), ImVec2(px0 + cStep, py0 + cStep), IM_COL32(58, 58, 64, 255));
                    }
                }
            }
        }

        // Live Baked Texture Display
        if (m_viewportRenderer.GetBakedNormalSRV() && udim == 1001) {
            drawList->AddImage(
                (ImTextureID)m_viewportRenderer.GetBakedNormalSRV(),
                ImVec2(x0, y0),
                ImVec2(x1, y1),
                ImVec2(0.0f, 0.0f),
                ImVec2(1.0f, 1.0f)
            );
        }

        // Subgrid lines (10x10 decimal UV subgrid with continuous zoom fade)
        if (currentTileSize > 35.0f) {
            float fade = std::clamp((currentTileSize - 35.0f) / 45.0f, 0.0f, 1.0f);
            int subAlpha = static_cast<int>(fade * 65.0f);
            ImU32 subLineCol = IM_COL32(78, 80, 88, subAlpha);
            for (int g = 1; g < 10; ++g) {
                float fx = x0 + (g / 10.0f) * currentTileSize;
                float fy = y0 + (g / 10.0f) * currentTileSize;
                drawList->AddLine(ImVec2(fx, y0), ImVec2(fx, y1), subLineCol, 1.0f);
                drawList->AddLine(ImVec2(x0, fy), ImVec2(x1, fy), subLineCol, 1.0f);
            }
        }

        drawList->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(92, 92, 100, 255), 0.0f, 0, 1.0f);

        char tileStr[16];
        snprintf(tileStr, sizeof(tileStr), "%d", udim);
        drawList->AddText(ImVec2(x0 + 8.0f, y0 + 6.0f), IM_COL32(150, 150, 158, 255), tileStr);
    }

    // Point in 2D triangle helper
    auto PointInTri2D = [](const ImVec2& pt, const ImVec2& a, const ImVec2& b, const ImVec2& c) -> bool {
        auto Sign = [](const ImVec2& p1, const ImVec2& p2, const ImVec2& p3) {
            return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
        };
        float d1 = Sign(pt, a, b);
        float d2 = Sign(pt, b, c);
        float d3 = Sign(pt, c, a);
        bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
        bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
        return !(hasNeg && hasPos);
    };

    // Draw Low-Poly UV Faces and Wireframe (Maya Shell Shading, Distortion Heatmap & Clean Wireframe)
    if (m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        ImU32 faceFillCol = IM_COL32(220, 225, 235, 18);   // Subtle translucent face fill
        ImU32 edgeCol = IM_COL32(225, 232, 240, 235);      // Bright polygon edges so quads read over shell fills and triangle seams

        // Palette of vibrant translucent colors matching Maya UV Editor Shell Shading (Hotkey 5)
        static const ImU32 kMayaShellColors[] = {
            IM_COL32(64, 180, 210, 80),   // Maya Cyan
            IM_COL32(230, 115, 80, 80),   // Coral
            IM_COL32(110, 205, 120, 80),  // Emerald
            IM_COL32(185, 110, 230, 80),  // Lavender Purple
            IM_COL32(240, 175, 55, 80),   // Golden Amber
            IM_COL32(235, 95, 150, 80),   // Rose Pink
            IM_COL32(65, 210, 175, 80),   // Mint Green
            IM_COL32(115, 140, 245, 80),  // Royal Blue
            IM_COL32(215, 140, 60, 80),   // Terracotta
            IM_COL32(160, 220, 70, 80)    // Lime
        };

        // 1. Base Face fill / Maya UV Shell Shading / Distortion Heatmap
        for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
            const auto& tri = m_lowPolyMesh.triangles[t];
            const auto& v0 = m_lowPolyMesh.vertices[tri.indices[0]];
            const auto& v1 = m_lowPolyMesh.vertices[tri.indices[1]];
            const auto& v2 = m_lowPolyMesh.vertices[tri.indices[2]];
            ImVec2 p0 = UVToScreen(v0.uv.u, v0.uv.v);
            ImVec2 p1 = UVToScreen(v1.uv.u, v1.uv.v);
            ImVec2 p2 = UVToScreen(v2.uv.u, v2.uv.v);

            ImU32 fillCol = faceFillCol;
            if (m_showDistortionHeatmap) {
                Vec3 d01(v1.position.x - v0.position.x, v1.position.y - v0.position.y, v1.position.z - v0.position.z);
                Vec3 d02(v2.position.x - v0.position.x, v2.position.y - v0.position.y, v2.position.z - v0.position.z);
                Vec3 crossP(d01.y * d02.z - d01.z * d02.y, d01.z * d02.x - d01.x * d02.z, d01.x * d02.y - d01.y * d02.x);
                float a3D = 0.5f * std::sqrt(crossP.x * crossP.x + crossP.y * crossP.y + crossP.z * crossP.z);
                float aUV = 0.5f * std::abs((v1.uv.u - v0.uv.u) * (v2.uv.v - v0.uv.v) - (v2.uv.u - v0.uv.u) * (v1.uv.v - v0.uv.v));
                float ratio = (a3D > 1e-6f) ? (aUV / a3D) : 1.0f;
                fillCol = (ratio > 1.35f) ? IM_COL32(245, 60, 60, 95) : ((ratio < 0.70f) ? IM_COL32(50, 110, 255, 95) : IM_COL32(60, 220, 110, 75));
            } else if (m_showShellShading) {
                fillCol = kMayaShellColors[tri.meshIndex % 10];
            }
            drawList->AddTriangleFilled(p0, p1, p2, fillCol);
        }

        // 2. Selected Faces / Shells Highlight (Translucent Amber / Gold Fill)
        if (!m_selectedUVFaces.empty()) {
            ImU32 selFaceCol = IM_COL32(255, 185, 30, 85);
            ImU32 selFaceBorderCol = IM_COL32(255, 215, 20, 240);
            for (uint32_t fIdx : m_selectedUVFaces) {
                if (fIdx < m_lowPolyMesh.triangles.size()) {
                    const auto& tri = m_lowPolyMesh.triangles[fIdx];
                    ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                    ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                    ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                    drawList->AddTriangleFilled(p0, p1, p2, selFaceCol);
                    drawList->AddTriangle(p0, p1, p2, selFaceBorderCol, 1.5f);
                }
            }
        }

        // 3. Wireframe: faint triangulation underneath, bright polygon (quad) edges on top
        if (m_showUVWireframe) {
            const ImU32 triEdgeCol = IM_COL32(200, 210, 220, 60);
            for (const auto& tri : m_lowPolyMesh.triangles) {
                ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                drawList->AddTriangle(p0, p1, p2, triEdgeCol, 1.0f);
            }
            if (!m_lowPolyMesh.polygonFaces.empty()) {
                for (const auto& face : m_lowPolyMesh.polygonFaces) {
                    size_t fv = face.size();
                    for (size_t v = 0; v < fv; ++v) {
                        uint32_t i0 = face[v];
                        uint32_t i1 = face[(v + 1) % fv];
                        if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                            ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[i0].uv.u, m_lowPolyMesh.vertices[i0].uv.v);
                            ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[i1].uv.u, m_lowPolyMesh.vertices[i1].uv.v);
                            drawList->AddLine(p0, p1, edgeCol, 1.0f);
                        }
                    }
                }
            } else if (!m_lowPolyMesh.wireframeLineIndices.empty()) {
                for (size_t i = 0; i + 1 < m_lowPolyMesh.wireframeLineIndices.size(); i += 2) {
                    uint32_t i0 = m_lowPolyMesh.wireframeLineIndices[i];
                    uint32_t i1 = m_lowPolyMesh.wireframeLineIndices[i + 1];
                    if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                        ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[i0].uv.u, m_lowPolyMesh.vertices[i0].uv.v);
                        ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[i1].uv.u, m_lowPolyMesh.vertices[i1].uv.v);
                        drawList->AddLine(p0, p1, edgeCol, 1.0f);
                    }
                }
            }
        }

        // 4. Selected Vertices (Glowing Gold Dots)
        if (!m_selectedUVVertices.empty()) {
            for (uint32_t vIdx : m_selectedUVVertices) {
                if (vIdx < m_lowPolyMesh.vertices.size()) {
                    ImVec2 p = UVToScreen(m_lowPolyMesh.vertices[vIdx].uv.u, m_lowPolyMesh.vertices[vIdx].uv.v);
                    drawList->AddCircleFilled(p, 4.0f, IM_COL32(255, 215, 20, 255));
                    drawList->AddCircle(p, 4.0f, IM_COL32(20, 20, 25, 255), 12, 1.0f);
                }
            }
        }
    }

    // Draw marked cut seams on 2D UV canvas (Maya Texture Borders in Neon Cyan)
    if (m_showTextureBorders && m_hasLowPoly && !m_lowPolyMesh.seamLineIndices.empty()) {
        ImU32 seamCol = IM_COL32(0, 240, 255, 230);
        for (size_t i = 0; i + 1 < m_lowPolyMesh.seamLineIndices.size(); i += 2) {
            uint32_t i0 = m_lowPolyMesh.seamLineIndices[i];
            uint32_t i1 = m_lowPolyMesh.seamLineIndices[i + 1];
            if (i0 < m_lowPolyMesh.vertices.size() && i1 < m_lowPolyMesh.vertices.size()) {
                ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[i0].uv.u, m_lowPolyMesh.vertices[i0].uv.v);
                ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[i1].uv.u, m_lowPolyMesh.vertices[i1].uv.v);
                drawList->AddLine(p0, p1, seamCol, 2.0f);
            }
        }
    }

    // Draw selected edges on 2D UV canvas (Glowing Golden Yellow)
    if (m_hasLowPoly && !m_lowPolyMesh.selectedEdges.empty()) {
        ImU32 selCol = IM_COL32(255, 215, 20, 255);
        for (const auto& edge : m_lowPolyMesh.selectedEdges) {
            if (edge.first < m_lowPolyMesh.vertices.size() && edge.second < m_lowPolyMesh.vertices.size()) {
                ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[edge.first].uv.u, m_lowPolyMesh.vertices[edge.first].uv.v);
                ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[edge.second].uv.u, m_lowPolyMesh.vertices[edge.second].uv.v);
                drawList->AddLine(p0, p1, selCol, 3.5f);
            }
        }
    }

    // Interactive Component Selection & Marquee Box Drag
    if (inCanvas && m_hasLowPoly && !m_lowPolyMesh.triangles.empty()) {
        // Component Hover Detection & Cursor Triangle Tracking
        int hoverVert = -1;
        int hoverEdgeV0 = -1, hoverEdgeV1 = -1;
        int hoverFace = -1;
        std::set<uint32_t> hoverShellFaces;

        // Trace cursor triangle across UV space (active in all selection modes)
        int cursorTri = -1;
        for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
            const auto& tri = m_lowPolyMesh.triangles[t];
            ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
            ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
            ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
            if (PointInTri2D(io.MousePos, p0, p1, p2)) {
                cursorTri = static_cast<int>(t);
                break;
            }
        }

        if (m_uvSelectionMode == UVSelectionMode::Vertex) {
            float minVertDist = 12.0f;
            for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
                ImVec2 p = UVToScreen(m_lowPolyMesh.vertices[i].uv.u, m_lowPolyMesh.vertices[i].uv.v);
                float d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
                if (d < minVertDist) {
                    minVertDist = d;
                    hoverVert = static_cast<int>(i);
                }
            }
            if (hoverVert >= 0) {
                ImVec2 p = UVToScreen(m_lowPolyMesh.vertices[hoverVert].uv.u, m_lowPolyMesh.vertices[hoverVert].uv.v);
                drawList->AddCircle(p, 6.0f, IM_COL32(0, 255, 255, 255), 16, 2.0f);
            }
        } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
            float minEdgeDist2D = 14.0f;
            ImVec2 hP0, hP1;
            for (const auto& tri : m_lowPolyMesh.triangles) {
                for (int e = 0; e < 3; ++e) {
                    uint32_t ea = tri.indices[e];
                    uint32_t eb = tri.indices[(e + 1) % 3];
                    ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[ea].uv.u, m_lowPolyMesh.vertices[ea].uv.v);
                    ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[eb].uv.u, m_lowPolyMesh.vertices[eb].uv.v);
                    float dist = DistToSegment2D(io.MousePos, p0, p1);
                    if (dist < minEdgeDist2D) {
                        minEdgeDist2D = dist;
                        hoverEdgeV0 = static_cast<int>(ea);
                        hoverEdgeV1 = static_cast<int>(eb);
                        hP0 = p0;
                        hP1 = p1;
                    }
                }
            }
            if (hoverEdgeV0 >= 0) {
                drawList->AddLine(hP0, hP1, IM_COL32(0, 255, 255, 255), 3.5f);
            }
        } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
            hoverFace = cursorTri;
            if (hoverFace >= 0) {
                if (m_uvSelectionMode == UVSelectionMode::Face) {
                    const auto& tri = m_lowPolyMesh.triangles[hoverFace];
                    ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                    ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                    ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                    drawList->AddTriangleFilled(p0, p1, p2, IM_COL32(0, 230, 255, 70));
                    drawList->AddTriangle(p0, p1, p2, IM_COL32(0, 255, 255, 255), 1.5f);
                } else {
                    hoverShellFaces = GetConnectedUVIsland(static_cast<uint32_t>(hoverFace));
                    for (uint32_t fIdx : hoverShellFaces) {
                        const auto& tri = m_lowPolyMesh.triangles[fIdx];
                        ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                        ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                        ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                        drawList->AddTriangleFilled(p0, p1, p2, IM_COL32(0, 230, 255, 50));
                    }
                }
            }
        }

        // Immediate Double-Click or 'L' Key Connected Island Selection
        bool didDoubleOrL = false;
        if (!io.KeyAlt && (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_L))) {
            int targetTri = (cursorTri >= 0) ? cursorTri : hoverFace;
            if (targetTri < 0 && hoverVert >= 0) {
                for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
                    const auto& tri = m_lowPolyMesh.triangles[t];
                    if (tri.indices[0] == static_cast<uint32_t>(hoverVert) ||
                        tri.indices[1] == static_cast<uint32_t>(hoverVert) ||
                        tri.indices[2] == static_cast<uint32_t>(hoverVert)) {
                        targetTri = static_cast<int>(t);
                        break;
                    }
                }
            } else if (targetTri < 0 && hoverEdgeV0 >= 0) {
                for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
                    const auto& tri = m_lowPolyMesh.triangles[t];
                    for (int e = 0; e < 3; ++e) {
                        if (MakeEdgeKey(tri.indices[e], tri.indices[(e + 1) % 3]) == MakeEdgeKey(hoverEdgeV0, hoverEdgeV1)) {
                            targetTri = static_cast<int>(t);
                            break;
                        }
                    }
                    if (targetTri >= 0) break;
                }
            }

            if (targetTri >= 0) {
                auto island = GetConnectedUVIsland(static_cast<uint32_t>(targetTri));
                if (!io.KeyShift) {
                    ClearUVSelection();
                }
                if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                    for (uint32_t f : island) {
                        const auto& tri = m_lowPolyMesh.triangles[f];
                        m_selectedUVVertices.insert(tri.indices[0]);
                        m_selectedUVVertices.insert(tri.indices[1]);
                        m_selectedUVVertices.insert(tri.indices[2]);
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
                    for (uint32_t f : island) {
                        const auto& tri = m_lowPolyMesh.triangles[f];
                        for (int e = 0; e < 3; ++e) {
                            m_lowPolyMesh.selectedEdges.insert(MakeEdgeKey(tri.indices[e], tri.indices[(e + 1) % 3]));
                        }
                    }
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                } else {
                    m_selectedUVFaces.insert(island.begin(), island.end());
                }
                PushHistoryState("Select Connected UV Shell");
                char msg[128];
                snprintf(msg, sizeof(msg), "Selected connected UV island (%zu faces)", island.size());
                m_uvStatusMessage = msg;
                m_isMarqueeSelecting = false;
                didDoubleOrL = true;
            }
        }

        // -------------------------------------------------------------
        // 2D UV Transform Gizmo Interaction & Rendering
        // -------------------------------------------------------------
        m_gizmoSelectedVerts.clear();
        if (m_uvSelectionMode == UVSelectionMode::Vertex) {
            m_gizmoSelectedVerts.assign(m_selectedUVVertices.begin(), m_selectedUVVertices.end());
        } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
            for (const auto& edge : m_lowPolyMesh.selectedEdges) {
                m_gizmoSelectedVerts.push_back(edge.first);
                m_gizmoSelectedVerts.push_back(edge.second);
            }
        } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
            for (uint32_t fIdx : m_selectedUVFaces) {
                if (fIdx < m_lowPolyMesh.triangles.size()) {
                    const auto& tri = m_lowPolyMesh.triangles[fIdx];
                    m_gizmoSelectedVerts.push_back(tri.indices[0]);
                    m_gizmoSelectedVerts.push_back(tri.indices[1]);
                    m_gizmoSelectedVerts.push_back(tri.indices[2]);
                }
            }
        }
        std::sort(m_gizmoSelectedVerts.begin(), m_gizmoSelectedVerts.end());
        m_gizmoSelectedVerts.erase(std::unique(m_gizmoSelectedVerts.begin(), m_gizmoSelectedVerts.end()), m_gizmoSelectedVerts.end());

        bool gizmoHovered = false;

        if (!m_gizmoSelectedVerts.empty()) {
            float minU = 1e9f, minV = 1e9f, maxU = -1e9f, maxV = -1e9f;
            for (uint32_t v : m_gizmoSelectedVerts) {
                if (v < m_lowPolyMesh.vertices.size()) {
                    minU = (std::min)(minU, m_lowPolyMesh.vertices[v].uv.u);
                    maxU = (std::max)(maxU, m_lowPolyMesh.vertices[v].uv.u);
                    minV = (std::min)(minV, m_lowPolyMesh.vertices[v].uv.v);
                    maxV = (std::max)(maxV, m_lowPolyMesh.vertices[v].uv.v);
                }
            }
            UVPoint centerUV((minU + maxU) * 0.5f, (minV + maxV) * 0.5f);
            ImVec2 centerScr = UVToScreen(centerUV.u, centerUV.v);

            float dCenter = std::hypot(io.MousePos.x - centerScr.x, io.MousePos.y - centerScr.y);
            float dRing = std::abs(dCenter - 44.0f);
            float dU = DistToSegment2D(io.MousePos, centerScr, ImVec2(centerScr.x + 65.0f, centerScr.y));
            float dV = DistToSegment2D(io.MousePos, centerScr, ImVec2(centerScr.x, centerScr.y - 65.0f));
            bool inScaleBox = (io.MousePos.x >= centerScr.x + 38.0f && io.MousePos.x <= centerScr.x + 54.0f &&
                               io.MousePos.y >= centerScr.y - 54.0f && io.MousePos.y <= centerScr.y - 38.0f);

            bool canHitCenter = (dCenter <= 12.0f);
            bool canHitU = (dU <= 9.0f && io.MousePos.x >= centerScr.x - 2.0f && io.MousePos.x <= centerScr.x + 75.0f);
            bool canHitV = (dV <= 9.0f && io.MousePos.y <= centerScr.y + 2.0f && io.MousePos.y >= centerScr.y - 75.0f);
            bool canHitRing = (dRing <= 7.0f);

            if (canHitCenter || canHitU || canHitV || canHitRing || inScaleBox) {
                gizmoHovered = true;
            }

            // Transform shortcuts: G/W = Move, E/R = Rotate, S = Scale
            if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
                if (ImGui::IsKeyPressed(ImGuiKey_G) || ImGui::IsKeyPressed(ImGuiKey_W)) {
                    m_activeGizmoPart = UVGizmoDragPart::CenterFree;
                    m_gizmoDragMouseStart = io.MousePos;
                    m_gizmoCenterStart = centerUV;
                    m_gizmoInitialUVs.clear();
                    for (uint32_t v : m_gizmoSelectedVerts) {
                        m_gizmoInitialUVs.push_back(UVPoint(m_lowPolyMesh.vertices[v].uv.u, m_lowPolyMesh.vertices[v].uv.v));
                    }
                    m_gizmoInitialAngle = std::atan2(io.MousePos.y - centerScr.y, io.MousePos.x - centerScr.x);
                    m_gizmoInitialDist = (std::max)(12.0f, dCenter);
                } else if (ImGui::IsKeyPressed(ImGuiKey_R) || ImGui::IsKeyPressed(ImGuiKey_E)) {
                    m_activeGizmoPart = UVGizmoDragPart::RotateRing;
                    m_gizmoDragMouseStart = io.MousePos;
                    m_gizmoCenterStart = centerUV;
                    m_gizmoInitialUVs.clear();
                    for (uint32_t v : m_gizmoSelectedVerts) {
                        m_gizmoInitialUVs.push_back(UVPoint(m_lowPolyMesh.vertices[v].uv.u, m_lowPolyMesh.vertices[v].uv.v));
                    }
                    m_gizmoInitialAngle = std::atan2(io.MousePos.y - centerScr.y, io.MousePos.x - centerScr.x);
                    m_gizmoInitialDist = (std::max)(12.0f, dCenter);
                } else if (ImGui::IsKeyPressed(ImGuiKey_S)) {
                    m_activeGizmoPart = UVGizmoDragPart::ScaleUniform;
                    m_gizmoDragMouseStart = io.MousePos;
                    m_gizmoCenterStart = centerUV;
                    m_gizmoInitialUVs.clear();
                    for (uint32_t v : m_gizmoSelectedVerts) {
                        m_gizmoInitialUVs.push_back(UVPoint(m_lowPolyMesh.vertices[v].uv.u, m_lowPolyMesh.vertices[v].uv.v));
                    }
                    m_gizmoInitialAngle = std::atan2(io.MousePos.y - centerScr.y, io.MousePos.x - centerScr.x);
                    m_gizmoInitialDist = (std::max)(12.0f, dCenter);
                }
            }

            // Gizmo mouse press
            if (!io.KeyAlt && ImGui::IsMouseClicked(0)) {
                if (inScaleBox) {
                    m_activeGizmoPart = UVGizmoDragPart::ScaleUniform;
                } else if (canHitCenter) {
                    m_activeGizmoPart = UVGizmoDragPart::CenterFree;
                } else if (canHitU) {
                    m_activeGizmoPart = UVGizmoDragPart::AxisU;
                } else if (canHitV) {
                    m_activeGizmoPart = UVGizmoDragPart::AxisV;
                } else if (canHitRing) {
                    m_activeGizmoPart = UVGizmoDragPart::RotateRing;
                }

                if (m_activeGizmoPart != UVGizmoDragPart::None) {
                    m_gizmoDragMouseStart = io.MousePos;
                    m_gizmoCenterStart = centerUV;
                    m_gizmoInitialUVs.clear();
                    m_gizmoInitialUVs.reserve(m_gizmoSelectedVerts.size());
                    for (uint32_t v : m_gizmoSelectedVerts) {
                        m_gizmoInitialUVs.push_back(UVPoint(m_lowPolyMesh.vertices[v].uv.u, m_lowPolyMesh.vertices[v].uv.v));
                    }
                    m_gizmoInitialAngle = std::atan2(io.MousePos.y - centerScr.y, io.MousePos.x - centerScr.x);
                    m_gizmoInitialDist = (std::max)(12.0f, dCenter);
                }
            }

            // Gizmo dragging
            if (m_activeGizmoPart != UVGizmoDragPart::None && io.MouseDown[0]) {
                if (m_activeGizmoPart == UVGizmoDragPart::CenterFree) {
                    float du = (io.MousePos.x - m_gizmoDragMouseStart.x) / currentTileSize;
                    float dv = -(io.MousePos.y - m_gizmoDragMouseStart.y) / currentTileSize;
                    for (size_t i = 0; i < m_gizmoSelectedVerts.size(); ++i) {
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.u = m_gizmoInitialUVs[i].u + du;
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.v = m_gizmoInitialUVs[i].v + dv;
                    }
                } else if (m_activeGizmoPart == UVGizmoDragPart::AxisU) {
                    float du = (io.MousePos.x - m_gizmoDragMouseStart.x) / currentTileSize;
                    for (size_t i = 0; i < m_gizmoSelectedVerts.size(); ++i) {
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.u = m_gizmoInitialUVs[i].u + du;
                    }
                } else if (m_activeGizmoPart == UVGizmoDragPart::AxisV) {
                    float dv = -(io.MousePos.y - m_gizmoDragMouseStart.y) / currentTileSize;
                    for (size_t i = 0; i < m_gizmoSelectedVerts.size(); ++i) {
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.v = m_gizmoInitialUVs[i].v + dv;
                    }
                } else if (m_activeGizmoPart == UVGizmoDragPart::RotateRing) {
                    float curAngle = std::atan2(io.MousePos.y - centerScr.y, io.MousePos.x - centerScr.x);
                    float deltaAngle = curAngle - m_gizmoInitialAngle;
                    float cosA = std::cos(-deltaAngle);
                    float sinA = std::sin(-deltaAngle);
                    for (size_t i = 0; i < m_gizmoSelectedVerts.size(); ++i) {
                        float du = m_gizmoInitialUVs[i].u - m_gizmoCenterStart.u;
                        float dv = m_gizmoInitialUVs[i].v - m_gizmoCenterStart.v;
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.u = m_gizmoCenterStart.u + (du * cosA - dv * sinA);
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.v = m_gizmoCenterStart.v + (du * sinA + dv * cosA);
                    }
                } else if (m_activeGizmoPart == UVGizmoDragPart::ScaleUniform) {
                    float curDist = std::hypot(io.MousePos.x - centerScr.x, io.MousePos.y - centerScr.y);
                    float s = (std::max)(0.001f, curDist / m_gizmoInitialDist);
                    for (size_t i = 0; i < m_gizmoSelectedVerts.size(); ++i) {
                        float du = m_gizmoInitialUVs[i].u - m_gizmoCenterStart.u;
                        float dv = m_gizmoInitialUVs[i].v - m_gizmoCenterStart.v;
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.u = m_gizmoCenterStart.u + du * s;
                        m_lowPolyMesh.vertices[m_gizmoSelectedVerts[i]].uv.v = m_gizmoCenterStart.v + dv * s;
                    }
                }
            }

            if (m_activeGizmoPart != UVGizmoDragPart::None && io.MouseReleased[0]) {
                m_activeGizmoPart = UVGizmoDragPart::None;
                m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
                PushHistoryState("Transform UVs");
                m_uvStatusMessage = "Transformed selected UVs";
            }

            // Render Gizmo Visuals
            ImVec2 bMin = UVToScreen(minU, maxV);
            ImVec2 bMax = UVToScreen(maxU, minV);
            drawList->AddRect(bMin, bMax, IM_COL32(255, 204, 0, 100), 0.0f, 0, 1.0f);

            // Rotate Ring
            ImU32 ringCol = (m_activeGizmoPart == UVGizmoDragPart::RotateRing || canHitRing) ? IM_COL32(0, 255, 255, 255) : IM_COL32(0, 160, 255, 180);
            drawList->AddCircle(centerScr, 44.0f, ringCol, 36, (m_activeGizmoPart == UVGizmoDragPart::RotateRing || canHitRing) ? 3.0f : 2.0f);

            // Axis U Arrow (Coral Red)
            ImU32 uCol = (m_activeGizmoPart == UVGizmoDragPart::AxisU || canHitU) ? IM_COL32(255, 240, 100, 255) : IM_COL32(255, 65, 65, 240);
            ImVec2 tipU(centerScr.x + 65.0f, centerScr.y);
            drawList->AddLine(centerScr, tipU, uCol, (m_activeGizmoPart == UVGizmoDragPart::AxisU || canHitU) ? 4.0f : 2.5f);
            drawList->AddTriangleFilled(ImVec2(tipU.x + 9.0f, tipU.y), ImVec2(tipU.x, tipU.y - 5.0f), ImVec2(tipU.x, tipU.y + 5.0f), uCol);
            drawList->AddText(ImVec2(tipU.x + 12.0f, tipU.y - 8.0f), uCol, "U");

            // Axis V Arrow (Emerald Green)
            ImU32 vCol = (m_activeGizmoPart == UVGizmoDragPart::AxisV || canHitV) ? IM_COL32(255, 240, 100, 255) : IM_COL32(50, 220, 90, 240);
            ImVec2 tipV(centerScr.x, centerScr.y - 65.0f);
            drawList->AddLine(centerScr, tipV, vCol, (m_activeGizmoPart == UVGizmoDragPart::AxisV || canHitV) ? 4.0f : 2.5f);
            drawList->AddTriangleFilled(ImVec2(tipV.x, tipV.y - 9.0f), ImVec2(tipV.x - 5.0f, tipV.y), ImVec2(tipV.x + 5.0f, tipV.y), vCol);
            drawList->AddText(ImVec2(tipV.x - 4.0f, tipV.y - 22.0f), vCol, "V");

            // Uniform Scale Box (Gold)
            ImU32 sCol = (m_activeGizmoPart == UVGizmoDragPart::ScaleUniform || inScaleBox) ? IM_COL32(0, 255, 255, 255) : IM_COL32(255, 204, 0, 240);
            ImVec2 sBoxMin(centerScr.x + 38.0f, centerScr.y - 54.0f);
            ImVec2 sBoxMax(centerScr.x + 54.0f, centerScr.y - 38.0f);
            drawList->AddRectFilled(sBoxMin, sBoxMax, sCol);
            drawList->AddRect(sBoxMin, sBoxMax, IM_COL32(0, 0, 0, 220), 0.0f, 0, 1.0f);

            // Center Free Move Handle (Square)
            ImU32 cCol = (m_activeGizmoPart == UVGizmoDragPart::CenterFree || canHitCenter) ? IM_COL32(0, 255, 255, 255) : IM_COL32(255, 255, 255, 220);
            drawList->AddRectFilled(ImVec2(centerScr.x - 7.0f, centerScr.y - 7.0f), ImVec2(centerScr.x + 7.0f, centerScr.y + 7.0f), cCol, 2.0f);
            drawList->AddRect(ImVec2(centerScr.x - 7.0f, centerScr.y - 7.0f), ImVec2(centerScr.x + 7.0f, centerScr.y + 7.0f), IM_COL32(0, 0, 0, 220), 2.0f, 0, 1.0f);
        }

        // Marquee Box Drag State Handling (only when gizmo not hovered/dragged)
        if (!io.KeyAlt && !gizmoHovered && m_activeGizmoPart == UVGizmoDragPart::None && ImGui::IsMouseClicked(0)) {
            m_isMarqueeSelecting = true;
            m_marqueeStart = io.MousePos;
            m_marqueeEnd = io.MousePos;
        }

        if (m_isMarqueeSelecting && io.MouseDown[0]) {
            m_marqueeEnd = io.MousePos;
            float dragDist = std::hypot(m_marqueeEnd.x - m_marqueeStart.x, m_marqueeEnd.y - m_marqueeStart.y);
            if (dragDist > 4.0f) {
                ImVec2 minP((std::min)(m_marqueeStart.x, m_marqueeEnd.x), (std::min)(m_marqueeStart.y, m_marqueeEnd.y));
                ImVec2 maxP((std::max)(m_marqueeStart.x, m_marqueeEnd.x), (std::max)(m_marqueeStart.y, m_marqueeEnd.y));
                drawList->AddRectFilled(minP, maxP, IM_COL32(0, 200, 255, 30));
                drawList->AddRect(minP, maxP, IM_COL32(0, 230, 255, 220), 0.0f, 0, 1.5f);
            }
        }

        if (m_isMarqueeSelecting && io.MouseReleased[0]) {
            m_isMarqueeSelecting = false;
            float dragDist = std::hypot(m_marqueeEnd.x - m_marqueeStart.x, m_marqueeEnd.y - m_marqueeStart.y);

            if (dragDist > 6.0f) {
                // Box Selection
                ImVec2 minP((std::min)(m_marqueeStart.x, m_marqueeEnd.x), (std::min)(m_marqueeStart.y, m_marqueeEnd.y));
                ImVec2 maxP((std::max)(m_marqueeStart.x, m_marqueeEnd.x), (std::max)(m_marqueeStart.y, m_marqueeEnd.y));

                if (!io.KeyShift) {
                    ClearUVSelection();
                }

                if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                    for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
                        ImVec2 p = UVToScreen(m_lowPolyMesh.vertices[i].uv.u, m_lowPolyMesh.vertices[i].uv.v);
                        if (p.x >= minP.x && p.x <= maxP.x && p.y >= minP.y && p.y <= maxP.y) {
                            m_selectedUVVertices.insert(static_cast<uint32_t>(i));
                        }
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
                    for (const auto& tri : m_lowPolyMesh.triangles) {
                        for (int e = 0; e < 3; ++e) {
                            uint32_t ea = tri.indices[e];
                            uint32_t eb = tri.indices[(e + 1) % 3];
                            ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[ea].uv.u, m_lowPolyMesh.vertices[ea].uv.v);
                            ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[eb].uv.u, m_lowPolyMesh.vertices[eb].uv.v);
                            ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
                            if (mid.x >= minP.x && mid.x <= maxP.x && mid.y >= minP.y && mid.y <= maxP.y) {
                                m_lowPolyMesh.selectedEdges.insert(MakeEdgeKey(ea, eb));
                            }
                        }
                    }
                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                } else if (m_uvSelectionMode == UVSelectionMode::Face) {
                    for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
                        const auto& tri = m_lowPolyMesh.triangles[t];
                        ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                        ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                        ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                        ImVec2 centroid((p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f);
                        if (centroid.x >= minP.x && centroid.x <= maxP.x && centroid.y >= minP.y && centroid.y <= maxP.y) {
                            m_selectedUVFaces.insert(static_cast<uint32_t>(t));
                        }
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Shell) {
                    std::set<uint32_t> hitTris;
                    for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
                        const auto& tri = m_lowPolyMesh.triangles[t];
                        ImVec2 p0 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[0]].uv.u, m_lowPolyMesh.vertices[tri.indices[0]].uv.v);
                        ImVec2 p1 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[1]].uv.u, m_lowPolyMesh.vertices[tri.indices[1]].uv.v);
                        ImVec2 p2 = UVToScreen(m_lowPolyMesh.vertices[tri.indices[2]].uv.u, m_lowPolyMesh.vertices[tri.indices[2]].uv.v);
                        ImVec2 centroid((p0.x + p1.x + p2.x) / 3.0f, (p0.y + p1.y + p2.y) / 3.0f);
                        if (centroid.x >= minP.x && centroid.x <= maxP.x && centroid.y >= minP.y && centroid.y <= maxP.y) {
                            hitTris.insert(static_cast<uint32_t>(t));
                        }
                    }
                    for (uint32_t sTri : hitTris) {
                        auto sh = GetConnectedUVIsland(sTri);
                        m_selectedUVFaces.insert(sh.begin(), sh.end());
                    }
                }
                PushHistoryState("Box Select UV Components");
            } else if (!didDoubleOrL) {
                // Single Click Selection
                if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                    if (hoverVert >= 0) {
                        if (io.KeyShift) {
                            if (m_selectedUVVertices.count(hoverVert)) m_selectedUVVertices.erase(hoverVert);
                            else m_selectedUVVertices.insert(hoverVert);
                        } else {
                            m_selectedUVVertices.clear();
                            m_selectedUVVertices.insert(hoverVert);
                        }
                    } else if (!io.KeyShift) {
                        m_selectedUVVertices.clear();
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
                    if (hoverEdgeV0 >= 0) {
                        auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(hoverEdgeV0), static_cast<uint32_t>(hoverEdgeV1));
                        if (io.KeyShift && m_hasLastSelectedEdge && !m_lowPolyMesh.selectedEdges.empty() && m_lastSelectedEdge != edgeKey) {
                            UVCutter::SelectShortestPath(m_lowPolyMesh, m_lastSelectedEdge.first, m_lastSelectedEdge.second,
                                                         edgeKey.first, edgeKey.second, true);
                            m_lastSelectedEdge = edgeKey;
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Select Shortest Path in 2D");
                        } else if (io.KeyShift) {
                            if (m_lowPolyMesh.selectedEdges.count(edgeKey)) m_lowPolyMesh.selectedEdges.erase(edgeKey);
                            else m_lowPolyMesh.selectedEdges.insert(edgeKey);
                            m_lastSelectedEdge = edgeKey;
                            m_hasLastSelectedEdge = true;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                        } else {
                            m_lowPolyMesh.selectedEdges.clear();
                            m_lowPolyMesh.selectedEdges.insert(edgeKey);
                            m_lastSelectedEdge = edgeKey;
                            m_hasLastSelectedEdge = true;
                            m_lowPolyMesh.GenerateSelectedEdgeLines();
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                        }
                    } else if (!io.KeyShift) {
                        m_lowPolyMesh.selectedEdges.clear();
                        m_hasLastSelectedEdge = false;
                        m_lowPolyMesh.GenerateSelectedEdgeLines();
                        m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Face) {
                    if (hoverFace >= 0) {
                        if (io.KeyShift) {
                            if (m_selectedUVFaces.count(hoverFace)) m_selectedUVFaces.erase(hoverFace);
                            else m_selectedUVFaces.insert(hoverFace);
                        } else {
                            m_selectedUVFaces.clear();
                            m_selectedUVFaces.insert(hoverFace);
                        }
                    } else if (!io.KeyShift) {
                        m_selectedUVFaces.clear();
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Shell) {
                    if (!hoverShellFaces.empty()) {
                        if (io.KeyShift) {
                            bool allIn = true;
                            for (uint32_t f : hoverShellFaces) if (!m_selectedUVFaces.count(f)) { allIn = false; break; }
                            if (allIn) {
                                for (uint32_t f : hoverShellFaces) m_selectedUVFaces.erase(f);
                            } else {
                                m_selectedUVFaces.insert(hoverShellFaces.begin(), hoverShellFaces.end());
                            }
                        } else {
                            m_selectedUVFaces = hoverShellFaces;
                        }
                    } else if (!io.KeyShift) {
                        m_selectedUVFaces.clear();
                    }
                }
            }
        }

        // Shift + RMB: Maya Radial Marking Menu
        if (io.KeyShift && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            OpenMarkingMenu();
        }
    }

    // Draw UV Editor Header Toolbar (single pinned row across top of 2D UV Viewport)
    drawList->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + headerH), IM_COL32(36, 37, 41, 255));
    drawList->AddLine(ImVec2(origin.x, origin.y + headerH), ImVec2(origin.x + size.x, origin.y + headerH), IM_COL32(60, 63, 70, 255), 1.0f);

    bool compact = (size.x < 620.0f);

    // ==========================================
    // TIER 1: Selection Modes, Gizmos, Display Toggles & Viewport Switcher
    // ==========================================
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + 3.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(3, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(compact ? 4.0f : 5.0f, 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

    auto ModeButton = [&](const char* labelFull, const char* labelCompact, UVSelectionMode mode, const char* tooltip) {
        bool active = (m_uvSelectionMode == mode);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x4CC3A6));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0x0C1412));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x3A3B3F));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0xCCCCCC));
        }
        if (ImGui::Button(compact ? labelCompact : labelFull)) {
            SetUVSelectionMode(mode, tooltip);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };

    ModeButton("Vert (1)", "V 1", UVSelectionMode::Vertex, "Vertex Selection Mode [Hotkey: 1]");
    ModeButton("Edge (2)", "E 2", UVSelectionMode::Edge, "Edge Selection Mode [Hotkey: 2]");
    ModeButton("Face (3)", "F 3", UVSelectionMode::Face, "Face Selection Mode [Hotkey: 3]");
    ModeButton("Shell (4)", "S 4", UVSelectionMode::Shell, "UV Shell Selection Mode [Hotkey: 4]");

    ImGui::TextDisabled("|");
    ImGui::SameLine();

    auto GizmoButton = [&](const char* labelFull, const char* labelCompact, UVGizmoMode mode, const char* tooltip) {
        bool active = (m_uvGizmoMode == mode);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x4CC3A6));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0x0C1412));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x3A3B3F));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0xCCCCCC));
        }
        if (ImGui::Button(compact ? labelCompact : labelFull)) {
            m_uvGizmoMode = mode;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
        ImGui::SameLine();
    };

    GizmoButton("Move (W)", "W", UVGizmoMode::Translate, "Move Tool (Shortcut: W or G)");
    GizmoButton("Rot (E)", "E", UVGizmoMode::Rotate, "Rotate Tool (Shortcut: E or R)");
    GizmoButton("Scale (R)", "R", UVGizmoMode::Scale, "Scale Tool (Shortcut: R or S)");

    ImGui::TextDisabled("|");
    ImGui::SameLine();

    auto DisplayToggle = [&](const char* labelFull, const char* labelCompact, bool* var, const char* tip) {
        if (*var) {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x3B82F6));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0xFFFFFF));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, BakerUI::Hex(0x3A3B3F));
            ImGui::PushStyleColor(ImGuiCol_Text, BakerUI::Hex(0x888890));
        }
        if (ImGui::Button(compact ? labelCompact : labelFull)) *var = !*var;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
    };

    DisplayToggle("Borders", "Brd", &m_showTextureBorders, "Toggle UV texture border / seam highlighting (Maya Texture Borders)");
    DisplayToggle("Shells", "Col", &m_showShellShading, "Toggle Maya UV Shell Shading (Color-code each island uniquely)");
    DisplayToggle("Checker", "Grid", &m_showUVChecker, "Toggle 16x16 UV checker background grid");
    DisplayToggle("Distort", "Dist", &m_showDistortionHeatmap, "Toggle UV area stretch/compression distortion feedback");
    DisplayToggle("Wire", "Wire", &m_showUVWireframe, "Toggle UV polygon wireframe display");

    // Selection Stats summary on right of Tier 1
    char selStat[64];
    if (m_uvSelectionMode == UVSelectionMode::Vertex) {
        snprintf(selStat, sizeof(selStat), "V: %zu", m_selectedUVVertices.size());
    } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
        snprintf(selStat, sizeof(selStat), "E: %zu", m_lowPolyMesh.selectedEdges.size());
    } else if (m_uvSelectionMode == UVSelectionMode::Face) {
        snprintf(selStat, sizeof(selStat), "F: %zu", m_selectedUVFaces.size());
    } else {
        snprintf(selStat, sizeof(selStat), "Shell: %zu", m_selectedUVFaces.size());
    }

    float statW = ImGui::CalcTextSize(selStat).x;
    float rightGroupW = statW + 16.0f;
    float availRight = (origin.x + size.x) - ImGui::GetCursorScreenPos().x;

    if (availRight > rightGroupW + 8.0f) {
        ImGui::Dummy(ImVec2(availRight - rightGroupW - 4.0f, 0));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", selStat);
    }

    ImGui::PopStyleVar(3);

    // 2D UV Canvas bottom status hint text matching reference design
    char hintUV[140];
    snprintf(hintUV, sizeof(hintUV), "Texel density %.2f px/cm  *  padding %d px  *  Zoom: %.1fx  *  RMB menu  *  RMB drag: Pan  *  Scroll: Zoom",
             m_packStats.averageTexelDensity > 0.01f ? m_packStats.averageTexelDensity : 10.24f,
             m_packSettings.paddingPixels,
             m_uvZoom);
    drawList->AddText(ImVec2(origin.x + 14.0f, origin.y + size.y - 20.0f), ImGui::GetColorU32(BakerUI::Hex(0x5A6878)), hintUV);

    drawList->PopClipRect();
    ImGui::EndChild();
}

void UIManager::RenderViewportPanel() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    {
        ImGuiViewport* mvp = ImGui::GetMainViewport();
        const float x0 = mvp->WorkPos.x + BakerUI::kPanelMargin + BakerUI::kLeftW + BakerUI::kPanelMargin;
        const float x1 = mvp->WorkPos.x + mvp->WorkSize.x - BakerUI::kPanelMargin - BakerUI::kRightW - BakerUI::kPanelMargin;
        const float y0 = mvp->WorkPos.y + BakerUI::TopBarHeight();
        const float y1 = mvp->WorkPos.y + mvp->WorkSize.y - BakerUI::StatusBarHeight() - BakerUI::kPanelMargin;
        ImGui::SetNextWindowPos(ImVec2(x0, y0));
        ImGui::SetNextWindowSize(ImVec2((std::max)(x1 - x0, 200.0f), (std::max)(y1 - y0, 120.0f)));
    }
    bool open = ImGui::Begin("Viewport###viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
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
    const bool popupBlocking = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId); // a click that dismisses a menu must not also edit the scene

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
                    bool isUVActive = (m_toolMode == ToolMode::CutSeam) || (m_shellState.step == BakerUI::ShellState::Step::UV);
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
    if ((m_toolMode == ToolMode::PaintCage || m_toolMode == ToolMode::PaintSkew) && !popupBlocking && !ImGui::GetIO().KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
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
    bool isUVCuttingActive = (m_toolMode == ToolMode::CutSeam) || (m_shellState.step == BakerUI::ShellState::Step::UV);
    if (isUVCuttingActive && !popupBlocking && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
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

                if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                    int bestV = -1;
                    float bestScreenDist = 1e9f;
                    ImVec2 bestScreenPos(0, 0);

                    auto TestVert = [&](uint32_t vIdx) {
                        if (vIdx >= m_lowPolyMesh.vertices.size()) return;
                        Vec3 p = m_lowPolyMesh.vertices[vIdx].position;
                        DirectX::XMVECTOR v = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p.x, p.y, p.z, 1.0f), worldLP);
                        DirectX::XMFLOAT3 w;
                        DirectX::XMStoreFloat3(&w, v);
                        float sX, sY;
                        if (m_viewportRenderer.ProjectToScreen(w, sX, sY)) {
                            ImVec2 scr(origin.x + sX, origin.y + sY);
                            float d = std::hypot(io.MousePos.x - scr.x, io.MousePos.y - scr.y);
                            if (d < bestScreenDist) {
                                bestScreenDist = d;
                                bestV = static_cast<int>(vIdx);
                                bestScreenPos = scr;
                            }
                        }
                    };

                    if (hasHit && hitTriIdx >= 0 && hitTriIdx < static_cast<int>(m_lowPolyMesh.triangles.size())) {
                        const auto& tri = m_lowPolyMesh.triangles[hitTriIdx];
                        TestVert(tri.indices[0]);
                        TestVert(tri.indices[1]);
                        TestVert(tri.indices[2]);
                    }
                    if (bestV < 0 || bestScreenDist > 18.0f) {
                        for (size_t i = 0; i < m_lowPolyMesh.vertices.size(); ++i) {
                            TestVert(static_cast<uint32_t>(i));
                        }
                    }

                    if (bestV >= 0 && bestScreenDist <= 28.0f) {
                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                        drawList->AddCircle(bestScreenPos, 6.0f, IM_COL32(0, 255, 255, 255), 12, 2.0f);
                        drawList->AddCircleFilled(bestScreenPos, 3.5f, IM_COL32(0, 255, 255, 200));

                        if (ImGui::IsMouseClicked(0)) {
                            uint32_t uV = static_cast<uint32_t>(bestV);
                            if (io.KeyShift) {
                                if (m_selectedUVVertices.count(uV)) m_selectedUVVertices.erase(uV);
                                else m_selectedUVVertices.insert(uV);
                            } else {
                                m_selectedUVVertices.clear();
                                m_selectedUVVertices.insert(uV);
                            }
                            PushHistoryState("Select Vertex");
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Selected vertex %u (%zu total selected)", uV, m_selectedUVVertices.size());
                            m_uvStatusMessage = msg;
                        }
                    } else if (ImGui::IsMouseClicked(0) && !io.KeyShift && !io.KeyAlt) {
                        if (!m_selectedUVVertices.empty()) {
                            m_selectedUVVertices.clear();
                            PushHistoryState("Clear Selected Vertices");
                        }
                    }
                } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
                    if (hasHit && hitTriIdx >= 0 && hitTriIdx < static_cast<int>(m_lowPolyMesh.triangles.size())) {
                        const auto& tri = m_lowPolyMesh.triangles[hitTriIdx];
                        Vec3 p0 = m_lowPolyMesh.vertices[tri.indices[0]].position;
                        Vec3 p1 = m_lowPolyMesh.vertices[tri.indices[1]].position;
                        Vec3 p2 = m_lowPolyMesh.vertices[tri.indices[2]].position;

                        DirectX::XMVECTOR v0 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p0.x, p0.y, p0.z, 1.0f), worldLP);
                        DirectX::XMVECTOR v1 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p1.x, p1.y, p1.z, 1.0f), worldLP);
                        DirectX::XMVECTOR v2 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p2.x, p2.y, p2.z, 1.0f), worldLP);
                        DirectX::XMFLOAT3 w0, w1, w2;
                        DirectX::XMStoreFloat3(&w0, v0);
                        DirectX::XMStoreFloat3(&w1, v1);
                        DirectX::XMStoreFloat3(&w2, v2);

                        float s0X, s0Y, s1X, s1Y, s2X, s2Y;
                        if (m_viewportRenderer.ProjectToScreen(w0, s0X, s0Y) &&
                            m_viewportRenderer.ProjectToScreen(w1, s1X, s1Y) &&
                            m_viewportRenderer.ProjectToScreen(w2, s2X, s2Y)) {
                            ImVec2 s0(origin.x + s0X, origin.y + s0Y);
                            ImVec2 s1(origin.x + s1X, origin.y + s1Y);
                            ImVec2 s2(origin.x + s2X, origin.y + s2Y);

                            ImDrawList* drawList = ImGui::GetWindowDrawList();
                            drawList->AddTriangleFilled(s0, s1, s2, IM_COL32(0, 255, 255, 45));
                            drawList->AddTriangle(s0, s1, s2, IM_COL32(0, 255, 255, 240), 2.0f);

                            if (io.MouseDoubleClicked[0] || ImGui::IsKeyPressed(ImGuiKey_L)) {
                                auto islTris = GetConnectedUVIsland(static_cast<uint32_t>(hitTriIdx));
                                if (!io.KeyShift) {
                                    m_selectedUVFaces.clear();
                                    m_selectedUVVertices.clear();
                                    m_lowPolyMesh.selectedEdges.clear();
                                }
                                if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                                    for (uint32_t t : islTris) {
                                        const auto& tRef = m_lowPolyMesh.triangles[t];
                                        m_selectedUVVertices.insert(tRef.indices[0]);
                                        m_selectedUVVertices.insert(tRef.indices[1]);
                                        m_selectedUVVertices.insert(tRef.indices[2]);
                                    }
                                } else if (m_uvSelectionMode == UVSelectionMode::Edge) {
                                    for (uint32_t t : islTris) {
                                        const auto& tRef = m_lowPolyMesh.triangles[t];
                                        for (int e = 0; e < 3; ++e) {
                                            m_lowPolyMesh.selectedEdges.insert(MakeEdgeKey(tRef.indices[e], tRef.indices[(e+1)%3]));
                                        }
                                    }
                                    m_lowPolyMesh.GenerateSelectedEdgeLines();
                                    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                                } else {
                                    m_selectedUVFaces.insert(islTris.begin(), islTris.end());
                                }
                                PushHistoryState("Select Connected UV Shell");
                                char msg[128];
                                snprintf(msg, sizeof(msg), "Selected entire connected UV shell (%zu faces)", islTris.size());
                                m_uvStatusMessage = msg;
                            } else if (ImGui::IsMouseClicked(0)) {
                                uint32_t fIdx = static_cast<uint32_t>(hitTriIdx);
                                if (m_uvSelectionMode == UVSelectionMode::Face) {
                                    if (io.KeyShift) {
                                        if (m_selectedUVFaces.count(fIdx)) m_selectedUVFaces.erase(fIdx);
                                        else m_selectedUVFaces.insert(fIdx);
                                    } else {
                                        m_selectedUVFaces.clear();
                                        m_selectedUVFaces.insert(fIdx);
                                    }
                                    PushHistoryState("Select Face");
                                    char msg[128];
                                    snprintf(msg, sizeof(msg), "Selected face %u (%zu total selected)", fIdx, m_selectedUVFaces.size());
                                    m_uvStatusMessage = msg;
                                } else {
                                    auto islands = UVCutter::SegmentIslands(m_lowPolyMesh);
                                    for (const auto& islTris : islands) {
                                        bool contains = false;
                                        for (int t : islTris) {
                                            if (t == hitTriIdx) { contains = true; break; }
                                        }
                                        if (contains) {
                                            if (!io.KeyShift) m_selectedUVFaces.clear();
                                            for (int t : islTris) {
                                                m_selectedUVFaces.insert(static_cast<uint32_t>(t));
                                            }
                                            break;
                                        }
                                    }
                                    PushHistoryState("Select Shell");
                                    char msg[128];
                                    snprintf(msg, sizeof(msg), "Selected shell with %zu faces", m_selectedUVFaces.size());
                                    m_uvStatusMessage = msg;
                                }
                            }
                        }
                    } else if (ImGui::IsMouseClicked(0) && !io.KeyShift && !io.KeyAlt) {
                        if (!m_selectedUVFaces.empty()) {
                            m_selectedUVFaces.clear();
                            PushHistoryState("Clear Selected Faces");
                        }
                    }
                } else {
                    // UVSelectionMode::Edge
                    if (hasHit && hitTriIdx >= 0) {
                        const auto& tri = m_lowPolyMesh.triangles[hitTriIdx];
                        TestEdge(tri.indices[0], tri.indices[1]);
                        TestEdge(tri.indices[1], tri.indices[2]);
                        TestEdge(tri.indices[2], tri.indices[0]);
                    }

                    if (bestV0 < 0 || bestScreenDist > 18.0f) {
                        for (size_t i = 0; i + 1 < m_lowPolyMesh.wireframeLineIndices.size(); i += 2) {
                            uint32_t ea = m_lowPolyMesh.wireframeLineIndices[i];
                            uint32_t eb = m_lowPolyMesh.wireframeLineIndices[i + 1];
                            TestEdge(ea, eb);
                        }
                    }

                    if (bestV0 >= 0 && bestScreenDist <= 28.0f) {
                        ImDrawList* drawList = ImGui::GetWindowDrawList();
                        drawList->AddLine(bestScreenA, bestScreenB, IM_COL32(0, 255, 255, 255), 3.5f);

                        if (io.MouseDoubleClicked[0]) {
                            UVCutter::SelectEdgeLoop(m_lowPolyMesh, static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1), io.KeyShift);
                            m_lastSelectedEdge = MakeEdgeKey(static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1));
                            m_hasLastSelectedEdge = true;
                            m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
                            PushHistoryState("Select Edge Loop");
                            char msg[128];
                            snprintf(msg, sizeof(msg), "Selected edge loop: %zu edges selected", m_lowPolyMesh.selectedEdges.size());
                            m_uvStatusMessage = msg;
                        } else if (ImGui::IsMouseClicked(0)) {
                            auto edgeKey = MakeEdgeKey(static_cast<uint32_t>(bestV0), static_cast<uint32_t>(bestV1));
                            if (io.KeyShift && m_hasLastSelectedEdge && !m_lowPolyMesh.selectedEdges.empty() && m_lastSelectedEdge != edgeKey) {
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
                            OpenMarkingMenu();
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
                    }
                }

                // Render selected 3D vertices
                if (!m_selectedUVVertices.empty()) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    for (uint32_t vIdx : m_selectedUVVertices) {
                        if (vIdx < m_lowPolyMesh.vertices.size()) {
                            Vec3 p = m_lowPolyMesh.vertices[vIdx].position;
                            DirectX::XMVECTOR v = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p.x, p.y, p.z, 1.0f), worldLP);
                            DirectX::XMFLOAT3 w;
                            DirectX::XMStoreFloat3(&w, v);
                            float sX, sY;
                            if (m_viewportRenderer.ProjectToScreen(w, sX, sY)) {
                                ImVec2 scr(origin.x + sX, origin.y + sY);
                                drawList->AddCircleFilled(scr, 4.0f, IM_COL32(255, 204, 0, 255));
                                drawList->AddCircle(scr, 6.0f, IM_COL32(255, 230, 100, 255), 12, 1.5f);
                            }
                        }
                    }
                }
                // Render selected 3D faces
                if (!m_selectedUVFaces.empty()) {
                    ImDrawList* drawList = ImGui::GetWindowDrawList();
                    for (uint32_t fIdx : m_selectedUVFaces) {
                        if (fIdx < m_lowPolyMesh.triangles.size()) {
                            const auto& tri = m_lowPolyMesh.triangles[fIdx];
                            Vec3 p0 = m_lowPolyMesh.vertices[tri.indices[0]].position;
                            Vec3 p1 = m_lowPolyMesh.vertices[tri.indices[1]].position;
                            Vec3 p2 = m_lowPolyMesh.vertices[tri.indices[2]].position;
                            DirectX::XMVECTOR v0 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p0.x, p0.y, p0.z, 1.0f), worldLP);
                            DirectX::XMVECTOR v1 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p1.x, p1.y, p1.z, 1.0f), worldLP);
                            DirectX::XMVECTOR v2 = DirectX::XMVector3TransformCoord(DirectX::XMVectorSet(p2.x, p2.y, p2.z, 1.0f), worldLP);
                            DirectX::XMFLOAT3 w0, w1, w2;
                            DirectX::XMStoreFloat3(&w0, v0);
                            DirectX::XMStoreFloat3(&w1, v1);
                            DirectX::XMStoreFloat3(&w2, v2);
                            float s0X, s0Y, s1X, s1Y, s2X, s2Y;
                            if (m_viewportRenderer.ProjectToScreen(w0, s0X, s0Y) &&
                                m_viewportRenderer.ProjectToScreen(w1, s1X, s1Y) &&
                                m_viewportRenderer.ProjectToScreen(w2, s2X, s2Y)) {
                                ImVec2 s0(origin.x + s0X, origin.y + s0Y);
                                ImVec2 s1(origin.x + s1X, origin.y + s1Y);
                                ImVec2 s2(origin.x + s2X, origin.y + s2Y);
                                drawList->AddTriangleFilled(s0, s1, s2, IM_COL32(255, 204, 0, 75));
                                drawList->AddTriangle(s0, s1, s2, IM_COL32(255, 204, 0, 220), 1.5f);
                            }
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
            OpenMarkingMenu();
        }
    }

    // Interactive 3D Surface Curve Drawing and Point Movement
    if (m_toolMode == ToolMode::DrawCurve && !popupBlocking && !io.KeyAlt && m_hasLowPoly && (m_viewportMode == ViewportMode::View3D || m_viewportMode == ViewportMode::SplitBoth)) {
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
                if (activeCurve && activeCurve->visible) {
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

                // 1b. Magnetic snap to endpoints of any previously drawn curve (or start of active curve)
                bool snapFound = false;
                Vec3 snapPos = hitPos;
                Vec3 snapNorm = hitNormal;
                int snapTri = hitTriIdx;
                ImVec2 snapScreenP(0, 0);

                if (hoveredPt < 0) {
                    for (size_t cIdx = 0; cIdx < m_curveManager.curves.size(); ++cIdx) {
                        const auto& c = m_curveManager.curves[cIdx];
                        if (c.points.empty() || !c.visible) continue;
                        size_t testPts[2] = { 0, c.points.size() - 1 };
                        size_t numTests = (c.points.size() == 1) ? 1 : 2;
                        for (size_t t = 0; t < numTests; ++t) {
                            size_t pIdx = testPts[t];
                            if (&c == activeCurve && activeCurve->points.size() > 0 && pIdx == activeCurve->points.size() - 1) continue;

                            const auto& pt = c.points[pIdx];
                            float sX, sY;
                            if (m_viewportRenderer.ProjectToScreen(DirectX::XMFLOAT3(pt.position.x, pt.position.y, pt.position.z), sX, sY)) {
                                ImVec2 scr(origin.x + sX, origin.y + sY);
                                float dScreen = std::hypot(io.MousePos.x - scr.x, io.MousePos.y - scr.y);
                                if (dScreen <= 18.0f) {
                                    snapFound = true;
                                    snapPos = pt.position;
                                    snapNorm = pt.normal;
                                    snapTri = pt.triIndex;
                                    snapScreenP = scr;
                                    break;
                                }
                            }
                        }
                        if (snapFound) break;
                    }
                }

                if (snapFound) {
                    ImDrawList* dl = ImGui::GetWindowDrawList();
                    dl->AddRect(ImVec2(snapScreenP.x - 7, snapScreenP.y - 7), ImVec2(snapScreenP.x + 7, snapScreenP.y + 7), IM_COL32(0, 255, 200, 255), 0.0f, 0, 2.0f);
                    dl->AddCircleFilled(snapScreenP, 4.0f, IM_COL32(0, 255, 200, 255));
                    dl->AddText(ImVec2(snapScreenP.x + 10, snapScreenP.y - 7), IM_COL32(0, 255, 200, 240), "[SNAP TO END]");
                }

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
                    if (activeCurve) activeCurve->visible = true; // drawing on a hidden curve would be invisible
                    if (hoveredPt >= 0) {
                        if (io.KeyShift) {
                            m_curveManager.selectedPointEndIndex = hoveredPt;
                        } else {
                            m_draggingPointIndex = hoveredPt;
                            m_curveManager.selectedPointIndex = hoveredPt;
                            m_curveManager.selectedPointEndIndex = -1;
                        }
                    } else if (snapFound) {
                        m_curveManager.AddPointToActive(snapPos, snapNorm, snapTri);
                        m_curveManager.UpdateActiveSampledPath(m_lowPolyMesh, &worldMat, &viewProjMat, vpW, vpH);
                        PushHistoryState("Add Snapped Curve Point");

                        char msg[128];
                        snprintf(msg, sizeof(msg), "Snapped curve point %zu precisely to previous curve endpoint.",
                                 activeCurve ? activeCurve->points.size() : 1);
                        m_uvStatusMessage = msg;
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

                // Enter finishes this curve and starts the next. Applying a curve to the mesh is explicit
                // (Mark as Seam / Cut Mesh), and right-click opens the context menu instead of committing.
                if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && activeCurve && activeCurve->points.size() >= 2) {
                    m_draggingPointIndex = -1;
                    m_curveManager.selectedPointIndex = -1;
                    m_curveManager.AddCurve();
                    PushHistoryState("Finish Curve");
                    m_uvStatusMessage = "Curve finished. Use Mark as Seam or Cut Mesh to apply it.";
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) m_commandRegistry.Execute("uv.curve_delete_point");
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !m_markingMenu.active) {
                    m_toolMode = (m_shellState.step == BakerUI::ShellState::Step::UV) ? ToolMode::CutSeam : ToolMode::Navigate;
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
            if (curve.points.empty() || !curve.visible) continue;

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

    bool isUVMode = (m_shellState.step == BakerUI::ShellState::Step::UV);

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
            BakerUI::ToggleChip("Checker", &m_viewportRenderer.showUVChecker); ImGui::SameLine();
            BakerUI::ToggleChip("Stretch", &m_viewportRenderer.showUVStretch); ImGui::SameLine();
            if (m_viewportRenderer.showUVStretch) {
                ImGui::SetNextItemWidth(64.0f);
                ImGui::SliderFloat("##stretchRange", &m_viewportRenderer.uvStretchRange, 0.25f, 3.0f, "%.2f");
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Stretch range (log2): 1.0 = full red/blue at 2x density error");
                ImGui::SameLine();
            }

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

        if (!isUVMode) {
            ImGui::SetNextItemWidth(100);
            const char* shadingModes[] = { "Studio White", "Clay", "Wax Red", "Normals", "Flat" };
            int currentShading = (m_viewportRenderer.highPolyShading == ShadingMode::FlatShaded) ? 4 : (int)m_viewportRenderer.highPolyShading;
            if (ImGui::Combo("##Shading", &currentShading, shadingModes, IM_ARRAYSIZE(shadingModes))) {
                m_viewportRenderer.highPolyShading = (currentShading == 4) ? ShadingMode::FlatShaded : (ShadingMode)currentShading;
            }
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
                    using BakerUI::DrawCommand;
                    SurfaceCurve* c = m_curveManager.GetActiveCurve();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(BakerUI::Col::Accent(), "Curve cut");
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s, %zu pts, %zu seams", c ? c->name.c_str() : "none", c ? c->points.size() : 0, m_lowPolyMesh.cutSeams.size());
                    ImGui::SameLine();
                    DrawCommand(m_commandRegistry, "uv.curve_mark_seam");
                    ImGui::SameLine();
                    DrawCommand(m_commandRegistry, "uv.curve_cut_mesh");
                    ImGui::SameLine();
                    DrawCommand(m_commandRegistry, "uv.curve_toggle_visible", "Visible");
                    ImGui::SameLine();
                    DrawCommand(m_commandRegistry, "uv.curve_new");
                    if (c) {
                        ImGui::SameLine();
                        ImGui::Checkbox("Loop", &c->isClosed);
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("Enter: next curve   Esc: exit");
                } else if (m_toolMode == ToolMode::CutSeam) {
                    ImGui::AlignTextToFramePadding();
                    const char* modeLabel = (m_uvSelectionMode == UVSelectionMode::Vertex) ? "VERT (1)" :
                                           (m_uvSelectionMode == UVSelectionMode::Face)   ? "FACE (3)" :
                                           (m_uvSelectionMode == UVSelectionMode::Shell)  ? "SHELL (4)" : "EDGE (2)";
                    ImGui::TextColored(BakerUI::Hex(0x4CC3A6), "[%s]", modeLabel);
                    ImGui::SameLine();
                    ImGui::TextDisabled("| 1: Vert | 2: Edge | 3: Face | 4: Shell | Shift+RMB: Menu");
                    ImGui::SameLine();
                    if (m_uvSelectionMode == UVSelectionMode::Vertex) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.12f, 1.0f), "| Verts: %zu", m_selectedUVVertices.size());
                    } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.12f, 1.0f), "| Faces: %zu", m_selectedUVFaces.size());
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.12f, 1.0f), "| Edges: %zu", m_lowPolyMesh.selectedEdges.size());
                    }
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
        const char* hint3D = "Alt+LMB orbit  *  RMB menu  *  Shift+RMB marking menu";
        dl->AddText(ImVec2(origin.x + 14.0f, origin.y + avail.y - 20.0f), ImGui::GetColorU32(BakerUI::Hex(0x5A6878)), hint3D);
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
                                           ImGui::GetColorU32(BakerUI::Hex(0x262626)));
        dl->AddRectFilled(ImVec2(origin.x + leftW, origin.y),
                          ImVec2(origin.x + leftW + splitterW, origin.y + avail.y), splitBg);

        ImU32 splitLineCol = isSplitActive ? ImGui::GetColorU32(BakerUI::Col::Accent()) :
                             (isSplitHovered ? ImGui::GetColorU32(BakerUI::Col::AccentHi()) :
                                                ImGui::GetColorU32(BakerUI::Hex(0x3C3C3C)));
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

    {
        const float x2D = origin.x + (m_viewportMode == ViewportMode::SplitBoth ? leftW + splitterW : 0.0f);
        const bool inY = io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + avail.y;
        const bool in3D = leftW > 0.0f && inY && io.MousePos.x >= origin.x && io.MousePos.x <= origin.x + leftW;
        const bool in2D = rightW > 0.0f && inY && io.MousePos.x >= x2D && io.MousePos.x <= origin.x + avail.x &&
                          io.MousePos.y > origin.y + 30.0f; // below the UV header toolbar
        UpdateContextMenuTrigger(in3D, in2D);
        RenderContextMenu();
    }
    RenderMarkingMenu();

    ImGui::End();
}

void UIManager::OpenMarkingMenu() {
    m_markingMenu.active = true;
    m_markingMenu.sticky = false;
    m_markingMenu.centerPos = ImGui::GetIO().MousePos;
    m_markingMenu.armedWedge = -1;
}

// Radial menu: hold Shift+RMB, flick toward a label, release. Releasing without a flick keeps it open for clicking.
void UIManager::RenderMarkingMenu() {
    if (!m_markingMenu.active) return;

    constexpr float kPi = 3.14159265f;
    constexpr float kDeadZone = 20.0f;   // flick distance before a wedge arms
    constexpr float kHubR = 20.0f;
    constexpr float kRx = 165.0f, kRy = 110.0f; // label ellipse; sized so neighbouring labels never overlap
    constexpr float kPillH = 30.0f, kPillMinW = 96.0f;

    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    const ImVec2 c = m_markingMenu.centerPos;
    const ImVec2 d(io.MousePos.x - c.x, io.MousePos.y - c.y);
    const float dist = std::sqrt(d.x * d.x + d.y * d.y);
    m_markingMenu.dragDist = dist;

    // Label rectangles, kept inside the window so menus opened near an edge stay readable
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 pillMin[8], pillMax[8];
    for (int i = 0; i < 8; ++i) {
        const float rad = kMarkingWedges[i].angleDeg * kPi / 180.0f;
        const float w = (std::max)(ImGui::CalcTextSize(kMarkingWedges[i].label).x + 28.0f, kPillMinW);
        float cx = c.x + std::cos(rad) * kRx, cy = c.y + std::sin(rad) * kRy;
        cx = std::clamp(cx, vp->WorkPos.x + w * 0.5f + 4.0f, vp->WorkPos.x + vp->WorkSize.x - w * 0.5f - 4.0f);
        cy = std::clamp(cy, vp->WorkPos.y + kPillH * 0.5f + 4.0f, vp->WorkPos.y + vp->WorkSize.y - kPillH * 0.5f - 4.0f);
        pillMin[i] = ImVec2(cx - w * 0.5f, cy - kPillH * 0.5f);
        pillMax[i] = ImVec2(cx + w * 0.5f, cy + kPillH * 0.5f);
    }

    // Arm by label hover, or by flick direction while the button is still held
    int armed = -1;
    for (int i = 0; i < 8; ++i) {
        if (io.MousePos.x >= pillMin[i].x && io.MousePos.x <= pillMax[i].x &&
            io.MousePos.y >= pillMin[i].y && io.MousePos.y <= pillMax[i].y) armed = i;
    }
    if (armed < 0 && !m_markingMenu.sticky && dist > kDeadZone) {
        const float a = std::atan2(d.y, d.x) * 180.0f / kPi;
        float best = 1e9f;
        for (int i = 0; i < 8; ++i) {
            const float delta = std::fabs(std::remainder(a - kMarkingWedges[i].angleDeg, 360.0f));
            if (delta < best) { best = delta; armed = i; }
        }
    }
    m_markingMenu.armedWedge = armed;

    auto EnabledAt = [&](int i) {
        const BakerUI::Command* cmd = m_commandRegistry.Find(kMarkingWedges[i].commandId);
        return cmd && (!cmd->enabled || cmd->enabled());
    };

    const auto WithAlpha = [](ImVec4 c4, float a) { c4.w = a; return ImGui::GetColorU32(c4); };
    const ImU32 accent   = ImGui::GetColorU32(BakerUI::Col::Accent());
    const ImU32 panel    = WithAlpha(BakerUI::Col::Panel(), 0.96f);
    const ImU32 border   = ImGui::GetColorU32(BakerUI::Col::Border());
    const ImU32 textCol  = ImGui::GetColorU32(BakerUI::Col::Text());
    const ImU32 dimCol   = WithAlpha(BakerUI::Col::TextDim(), 0.75f);
    const ImU32 onAccent = ImGui::GetColorU32(BakerUI::Hex(0x0C1412));

    // Spokes from hub to each label, then the armed sector on top of them
    for (int i = 0; i < 8; ++i) {
        const float rad = kMarkingWedges[i].angleDeg * kPi / 180.0f;
        const ImVec2 dir(std::cos(rad), std::sin(rad));
        const bool on = (i == armed) && EnabledAt(i);
        dl->AddLine(ImVec2(c.x + dir.x * (kHubR + 4.0f), c.y + dir.y * (kHubR + 4.0f)),
                    ImVec2((pillMin[i].x + pillMax[i].x) * 0.5f, (pillMin[i].y + pillMax[i].y) * 0.5f),
                    on ? accent : border, on ? 2.0f : 1.0f);
    }
    if (armed >= 0) {
        const float rad = kMarkingWedges[armed].angleDeg * kPi / 180.0f;
        dl->PathArcTo(c, kHubR + 14.0f, rad - 0.34f, rad + 0.34f, 16);
        dl->PathStroke(EnabledAt(armed) ? accent : border, 0, 18.0f);
    }
    dl->AddCircleFilled(c, kHubR, panel);
    dl->AddCircle(c, kHubR, border, 32, 1.5f);

    // Labels
    for (int i = 0; i < 8; ++i) {
        const bool isArmed = (i == armed);
        const bool enabled = EnabledAt(i);
        const ImU32 fill = (isArmed && enabled) ? accent : panel;
        dl->AddRectFilled(pillMin[i], pillMax[i], fill, 6.0f);
        dl->AddRect(pillMin[i], pillMax[i], isArmed ? accent : border, 6.0f, 0, 1.0f);
        const char* label = kMarkingWedges[i].label;
        const ImVec2 ts = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2((pillMin[i].x + pillMax[i].x - ts.x) * 0.5f, (pillMin[i].y + pillMax[i].y - ts.y) * 0.5f),
                    !enabled ? dimCol : (isArmed ? onAccent : textCol), label);
    }

    // Why the armed wedge cannot run
    if (armed >= 0 && !EnabledAt(armed)) {
        const BakerUI::Command* cmd = m_commandRegistry.Find(kMarkingWedges[armed].commandId);
        if (cmd && cmd->disabledReason) {
            const std::string reason = cmd->disabledReason();
            const ImVec2 ts = ImGui::CalcTextSize(reason.c_str());
            dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + kRy + kPillH + 10.0f), ImGui::GetColorU32(BakerUI::Col::Warn()), reason.c_str());
        }
    }

    auto Close = [&]() { m_markingMenu.active = false; m_markingMenu.sticky = false; m_markingMenu.armedWedge = -1; };
    auto Run = [&](int i) { Close(); m_commandRegistry.Execute(kMarkingWedges[i].commandId); };

    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { Close(); return; }
    if (m_markingMenu.sticky) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { if (armed >= 0) Run(armed); else Close(); }
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (armed >= 0) Run(armed);
        else if (dist < kDeadZone) m_markingMenu.sticky = true;
        else Close();
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && armed >= 0) {
        Run(armed);
    }
}

// RMB click without a drag opens the context menu; RMB drag keeps dollying (3D) or panning (2D).
void UIManager::UpdateContextMenuTrigger(bool in3DZone, bool in2DZone) {
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && (in3DZone || in2DZone) && !io.KeyShift && !io.KeyAlt &&
        !m_markingMenu.active && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        m_rmbPressArmed = true;
        m_contextIn2D = in2DZone;
    }
    if (m_rmbPressArmed && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 4.0f)) m_rmbPressArmed = false;
    if (m_rmbPressArmed && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        m_rmbPressArmed = false;
        ImGui::OpenPopup("##viewport_ctx");
    }
}

// Built entirely from registry commands, so labels, shortcuts, checks and disabled reasons match every other surface.
void UIManager::RenderContextMenu() {
    using BakerUI::DrawCommandMenuItem;
    using BakerUI::DrawCommandMenuContents;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 4.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ImGui::GetFontSize() * 15.0f, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool open = ImGui::BeginPopup("##viewport_ctx");
    ImGui::PopStyleVar(4);
    if (!open) return;

    BakerUI::CommandRegistry& r = m_commandRegistry;
    const bool uvStep = (m_shellState.step == BakerUI::ShellState::Step::UV);
    if (uvStep) {
        const char* modeName = "Edge";
        size_t count = m_lowPolyMesh.selectedEdges.size();
        if (m_uvSelectionMode == UVSelectionMode::Vertex) { modeName = "Vertex"; count = m_selectedUVVertices.size(); }
        else if (m_uvSelectionMode == UVSelectionMode::Face) { modeName = "Face"; count = m_selectedUVFaces.size(); }
        else if (m_uvSelectionMode == UVSelectionMode::Shell) { modeName = "Shell"; count = m_selectedUVFaces.size(); }
        ImGui::TextDisabled("%s selection - %zu selected", modeName, count);
        ImGui::Separator();

        if (m_toolMode == ToolMode::DrawCurve) {
            DrawCommandMenuItem(r, "uv.curve_mark_seam");
            DrawCommandMenuItem(r, "uv.curve_cut_mesh");
            DrawCommandMenuItem(r, "uv.curve_toggle_visible");
            DrawCommandMenuItem(r, "uv.curve_delete_point");
            DrawCommandMenuItem(r, "uv.curve_new");
            ImGui::Separator();
        }
        DrawCommandMenuItem(r, "uv.cut");
        DrawCommandMenuItem(r, "uv.sew");
        ImGui::Separator();
        if (ImGui::BeginMenu("Selection mode")) {
            for (const char* id : { "select.mode_vertex", "select.mode_edge", "select.mode_face", "select.mode_shell" }) DrawCommandMenuItem(r, id);
            ImGui::EndMenu();
        }
        for (const char* id : { "select.edge_loop", "select.edge_ring", "select.seams", "select.clear" }) DrawCommandMenuItem(r, id);
        ImGui::Separator();
        for (const char* id : { "uv.unwrap", "uv.pack", "uv.auto_cut_unwrap_pack" }) DrawCommandMenuItem(r, id);
        ImGui::Separator();
        struct Sub { const char* title; const char* path; };
        for (const Sub& sub : { Sub{ "Project", "UV/Project" }, Sub{ "Transform", "UV/Transform" }, Sub{ "Align", "UV/Align" },
                                Sub{ "Curves", "UV/Cut & Sew/Curve Cut" }, Sub{ "Stack and overlap", "UV/Stack & Overlap" },
                                Sub{ "Texel density", "UV/Texel Density" } }) {
            if (ImGui::BeginMenu(sub.title)) { DrawCommandMenuContents(r, sub.path, false); ImGui::EndMenu(); }
        }
        ImGui::Separator();
    }
    if (ImGui::BeginMenu("View")) { DrawCommandMenuContents(r, "View", false); ImGui::EndMenu(); }
    DrawCommandMenuItem(r, "edit.undo");
    DrawCommandMenuItem(r, "edit.redo");
    ImGui::EndPopup();
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

// ---------------------------------------------------------------------------------------------
// Two side windows around the viewport.
//   Left  : the scene - every mesh (sculpt, game mesh, cage) and how it sits in the world.
//   Right : what to do with it - the UV tools or the bake settings, following the stepper,
//           with that step's main button pinned at the bottom.
// ---------------------------------------------------------------------------------------------
void UIManager::RenderScenePanel() {
    if (!BakerUI::BeginSidePanel("Scene###scene", true, BakerUI::kLeftW)) {
        ImGui::End();
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().y;
    if (ImGui::BeginChild("##outliner", ImVec2(0.0f, avail * 0.36f))) RenderOutliner();
    ImGui::EndChild();

    ImGui::Separator();
    if (BakerUI::gFontBold) ImGui::PushFont(BakerUI::gFontBold);
    ImGui::TextUnformatted("Details");
    if (BakerUI::gFontBold) ImGui::PopFont();
    ImGui::Spacing();
    if (ImGui::BeginChild("##details")) RenderSceneContents();
    ImGui::EndChild();
    ImGui::End();
}

// Outliner: Scene Collection > Collection > one row per mesh (eye, name, kind). Click a row to see its details below.
void UIManager::RenderOutliner() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float rowH = ImGui::GetFrameHeight();

    auto drawEye = [&](const char* cmdId) {
        const BakerUI::Command* cmd = m_commandRegistry.Find(cmdId);
        bool visible = cmd && cmd->checked && cmd->checked();
        ImVec2 p = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton(cmdId, ImVec2(rowH, rowH));
        bool hovered = ImGui::IsItemHovered();
        if (cmd) BakerUI::DrawCommandTooltip(*cmd, true);
        if (clicked) m_commandRegistry.Execute(cmdId);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 c(p.x + rowH * 0.5f, p.y + rowH * 0.5f);
        float rx = rowH * 0.30f, ry = rowH * 0.18f;
        ImVec4 col = visible ? BakerUI::Col::Text() : BakerUI::Col::TextDim();
        col.w = hovered ? 1.0f : (visible ? 0.85f : 0.55f);
        ImU32 u = ImGui::GetColorU32(col);
        dl->AddBezierQuadratic(ImVec2(c.x - rx, c.y), ImVec2(c.x, c.y - ry * 2.0f), ImVec2(c.x + rx, c.y), u, 1.2f);
        dl->AddBezierQuadratic(ImVec2(c.x - rx, c.y), ImVec2(c.x, c.y + ry * 2.0f), ImVec2(c.x + rx, c.y), u, 1.2f);
        if (visible) dl->AddCircleFilled(c, rowH * 0.09f, u);
        else dl->AddLine(ImVec2(c.x - rx * 0.8f, c.y + ry * 1.4f), ImVec2(c.x + rx * 0.8f, c.y - ry * 1.4f), u, 1.2f);
    };

    auto row = [&](SceneObject obj, const char* visCmd, const char* removeCmd, const char* replaceCmd, const std::string& name, const char* kind, bool present) {
        ImGui::PushID(static_cast<int>(obj));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
        const float eyeW = rowH + style.ItemSpacing.x;
        ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, present ? BakerUI::Col::Text() : BakerUI::Col::TextDim());
        ImGui::SetCursorScreenPos(ImVec2(start.x + eyeW, start.y));
        if (ImGui::Selectable(name.c_str(), m_selectedObject == obj, ImGuiSelectableFlags_None, ImVec2(ImGui::GetContentRegionAvail().x, rowH)))
            m_selectedObject = obj;
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        // Right-click: select the row, then act on it (same commands as the Details buttons)
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m_selectedObject = obj;
        if (ImGui::BeginPopupContextItem("##row_ctx")) {
            if (ImGui::MenuItem(present ? "Replace..." : "Add...")) m_commandRegistry.Execute(replaceCmd);
            if (present) {
                if (ImGui::MenuItem("Toggle Visibility")) m_commandRegistry.Execute(visCmd);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) m_commandRegistry.Execute(removeCmd);
            }
            ImGui::EndPopup();
        }
        ImVec2 rMax = ImGui::GetItemRectMax();
        ImVec2 ks = ImGui::CalcTextSize(kind);
        ImGui::GetWindowDrawList()->AddText(ImVec2(rMax.x - ks.x - style.FramePadding.x, start.y + (rowH - ks.y) * 0.5f),
                                            ImGui::GetColorU32(BakerUI::Col::TextDim()), kind);
        ImGui::SetCursorScreenPos(start);
        if (present) drawEye(visCmd); else ImGui::Dummy(ImVec2(rowH, rowH));
        ImGui::PopID();
    };

    if (BakerUI::gFontBold) ImGui::PushFont(BakerUI::gFontBold);
    ImGui::TextUnformatted("Scene Collection");
    if (BakerUI::gFontBold) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Indent(style.IndentSpacing * 0.6f);

    std::string hp = m_hasHighPoly ? std::filesystem::path(m_highPolyPath).stem().string() : std::string("Sculpt (not added)");
    std::string lp = m_hasLowPoly ? std::filesystem::path(m_lowPolyPath).stem().string() : std::string("Game mesh (not added)");
    row(SceneObject::HighPoly, "view.3d_high_poly", "scene.remove_high_poly", "file.open_high_poly", m_isLoadingHighPoly ? "Loading..." : hp, "Sculpt", m_hasHighPoly);
    row(SceneObject::LowPoly, "view.3d_low_poly", "scene.remove_low_poly", "file.open_low_poly", lp, "Game mesh", m_hasLowPoly);

    ImGui::Unindent(style.IndentSpacing * 0.6f);
}

void UIManager::RenderSceneContents() {
    const ImVec2 fullW(-FLT_MIN, 0.0f);
    const ImGuiStyle& style = ImGui::GetStyle();
    auto halfW = [&]() { return ImVec2((ImGui::GetContentRegionAvail().x - style.ItemSpacing.x) * 0.5f, 0.0f); };
    char buf[128];

    auto cacheLabel = [](const char* prefix, const char* path, char* out, size_t n) {
        std::error_code ec;
        uintmax_t sz = std::filesystem::is_regular_file(path, ec) ? std::filesystem::file_size(path, ec) : 0;
        if (sz == 0) return false;
        if (sz > (1ull << 30)) snprintf(out, n, "%s (%.2f GB)", prefix, sz / 1073741824.0);
        else                   snprintf(out, n, "%s (%.1f MB)", prefix, sz / 1048576.0);
        return true;
    };
    auto header = [&](const char* title, const char* pillText, const ImVec4& pillCol) {
        if (BakerUI::gFontBold) ImGui::PushFont(BakerUI::gFontBold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(title);
        if (BakerUI::gFontBold) ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(pillText).x - 12.0f);
        BakerUI::StatusPill(pillText, pillCol);
        ImGui::Spacing();
    };
    auto fileLine = [](const std::string& path) {
        std::string name = std::filesystem::path(path).filename().string();
        if (BakerUI::gFontMono) ImGui::PushFont(BakerUI::gFontMono);
        ImGui::TextUnformatted(name.c_str());
        if (BakerUI::gFontMono) ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
    };

    // ---- Sculpt
    if (m_selectedObject == SceneObject::HighPoly && BakerUI::BeginCard("##sc_hp")) {
        header("Sculpt", m_isLoadingHighPoly ? "Loading" : (m_hasHighPoly ? "Ready" : "Needed"),
               m_isLoadingHighPoly ? BakerUI::Col::Warn() : (m_hasHighPoly ? BakerUI::Col::Ok() : BakerUI::Col::TextDim()));
        if (m_isLoadingHighPoly) {
            ImGui::TextDisabled("Reading the mesh and building the BVH...");
        } else {
            if (m_hasHighPoly) {
                fileLine(m_highPolyPath);
                if (BakerUI::BeginProps("##sc_hp_props")) {
                    snprintf(buf, sizeof(buf), "%zu", m_highPolyMesh.positions.size());
                    BakerUI::PropValue("Vertices", buf);
                    if (m_highPolyMesh.isQuadMesh && !m_highPolyMesh.quadIndices.empty()) snprintf(buf, sizeof(buf), "%zu quads", m_highPolyMesh.quadIndices.size() / 4);
                    else                                                                snprintf(buf, sizeof(buf), "%zu triangles", m_highPolyMesh.indices.size() / 3);
                    BakerUI::PropValue("Faces", buf);
                    BakerUI::PropValue("Polypaint", m_highPolyMesh.colors.empty() ? "None" : "Yes");
                    BakerUI::PropValue("Polygroups", m_highPolyMesh.polygroupPerFace.empty() ? "None" : "Yes");
                    BakerUI::PropFloat("Opacity", &m_viewportRenderer.highPolyOpacity, 0.01f, 0.05f, 1.0f, "%.2f");
                    BakerUI::EndProps();
                }
            }
            if (cacheLabel("Sync from ZBrush", "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_HighPoly.obj", buf, sizeof(buf)))
                BakerUI::DrawCommand(m_commandRegistry, "file.load_zbrush_cache", buf, fullW, BakerUI::CommandStyle::Primary);
            BakerUI::DrawCommand(m_commandRegistry, "file.open_high_poly", m_hasHighPoly ? "Replace..." : "Browse...", m_hasHighPoly ? halfW() : fullW);
            if (m_hasHighPoly) {
                ImGui::SameLine();
                BakerUI::DrawCommand(m_commandRegistry, "scene.remove_high_poly", "Remove", fullW, BakerUI::CommandStyle::Destructive);
            }
        }
        BakerUI::EndCard();
    }

    // ---- Game mesh
    if (m_selectedObject == SceneObject::LowPoly && BakerUI::BeginCard("##sc_lp")) {
        header("Game mesh", m_hasLowPoly ? "Ready" : "Needed", m_hasLowPoly ? BakerUI::Col::Ok() : BakerUI::Col::TextDim());
        if (m_hasLowPoly) {
            fileLine(m_lowPolyPath);
            if (BakerUI::BeginProps("##sc_lp_props")) {
                snprintf(buf, sizeof(buf), "%zu", m_lowPolyMesh.vertices.size());
                BakerUI::PropValue("Vertices", buf);
                if (m_lowPolyMesh.quadCount > 0 && m_lowPolyMesh.triCount == 0) snprintf(buf, sizeof(buf), "%u quads", m_lowPolyMesh.quadCount);
                else if (m_lowPolyMesh.quadCount > 0) snprintf(buf, sizeof(buf), "%u (%u quads, %u tris)", m_lowPolyMesh.polyCount, m_lowPolyMesh.quadCount, m_lowPolyMesh.triCount);
                else snprintf(buf, sizeof(buf), "%zu triangles", m_lowPolyMesh.triangles.size());
                BakerUI::PropValue("Faces", buf);
                snprintf(buf, sizeof(buf), "%zu", m_lowPolyMesh.detectedUDIMs.size());
                BakerUI::PropValue("UDIM tiles", buf);
                BakerUI::PropFloat("Opacity", &m_viewportRenderer.lowPolyOpacity, 0.01f, 0.05f, 1.0f, "%.2f");
                BakerUI::EndProps();
            }
        }
        static const char* kLowCache = "C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/UDIM_LowPoly.obj";
        if (cacheLabel("Sync from ZBrush", kLowCache, buf, sizeof(buf)) && BakerUI::PrimaryButton(buf)) SetCommandLineLowPoly(kLowCache);
        BakerUI::DrawCommand(m_commandRegistry, "file.open_low_poly", m_hasLowPoly ? "Replace..." : "Browse...", m_hasLowPoly ? halfW() : fullW);
        if (m_hasLowPoly) {
            ImGui::SameLine();
            BakerUI::DrawCommand(m_commandRegistry, "scene.remove_low_poly", "Remove", fullW, BakerUI::CommandStyle::Destructive);
        }
        BakerUI::EndCard();
    }

    // ---- Cage: a property of the game mesh, shown with it
    if (m_selectedObject == SceneObject::LowPoly && m_hasLowPoly && BakerUI::BeginCard("##sc_cage")) {
        header("Cage", m_hasExplicitCage ? "Custom" : "Automatic", m_hasExplicitCage ? BakerUI::Col::Ok() : BakerUI::Col::TextDim());
        BakerUI::DrawCommand(m_commandRegistry, "view.3d_cage", "Show cage", fullW);
        if (m_hasExplicitCage) fileLine(m_cagePath);
        else {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("The game mesh pushed out along its normals. Set the distance in Bake, or use your own cage mesh.");
            ImGui::PopTextWrapPos();
        }
        ImGui::Spacing();
        if (!m_hasLowPoly) ImGui::BeginDisabled();
        BakerUI::DrawCommand(m_commandRegistry, "file.open_cage", m_hasExplicitCage ? "Replace..." : "Use a cage mesh...", m_hasExplicitCage ? halfW() : fullW);
        if (!m_hasLowPoly) ImGui::EndDisabled();
        if (m_hasExplicitCage) {
            ImGui::SameLine();
            BakerUI::DrawCommand(m_commandRegistry, "scene.remove_cage", "Remove", fullW, BakerUI::CommandStyle::Destructive);
        }
        BakerUI::EndCard();
    }

    // ---- Placement of the game mesh against the sculpt
    if (m_selectedObject == SceneObject::LowPoly && m_hasLowPoly && BakerUI::BeginCard("##sc_align")) {
        if (BakerUI::gFontBold) ImGui::PushFont(BakerUI::gFontBold);
        ImGui::TextUnformatted("Game mesh placement");
        if (BakerUI::gFontBold) ImGui::PopFont();
        ImGui::Spacing();
        bool changed = false;
        ImGui::TextDisabled("Position");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat3("##pos", &m_settings.lowPolyTranslation.x, 0.01f, -10000.0f, 10000.0f, "%.3f");
        ImGui::TextDisabled("Rotation (degrees)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        changed |= ImGui::DragFloat3("##rot", &m_settings.lowPolyRotation.x, 0.5f, -360.0f, 360.0f, "%.1f");
        ImGui::TextDisabled("Scale");
        ImGui::SameLine();
        ImGui::Checkbox("Uniform", &m_uniformScale);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (m_uniformScale) {
            float s = m_settings.lowPolyScaleVec.x;
            if (ImGui::DragFloat("##scale_u", &s, 0.005f, 0.0001f, 10000.0f, "%.4f")) {
                m_settings.lowPolyScaleVec = { s, s, s };
                m_settings.lowPolyScale = s;
                changed = true;
            }
        } else {
            changed |= ImGui::DragFloat3("##scale_xyz", &m_settings.lowPolyScaleVec.x, 0.005f, 0.0001f, 10000.0f, "%.4f");
        }
        if (changed) SyncTransforms();
        ImGui::Spacing();
        if (m_hasHighPoly) {
            if (BakerUI::SecondaryButton("Auto-align", halfW().x)) AutoAlignMeshCenters();
            ImGui::SameLine();
            if (BakerUI::SecondaryButton("Auto-fit scale")) AutoFitMeshScale();
            if (ImGui::Checkbox("Auto-match scale", &m_autoScaleLowPoly)) CheckScaleAndAutoCalibrate();
            ImGui::TextDisabled("Detected scale ratio %.4fx", m_detectedScaleRatio);
        }
        if (BakerUI::SecondaryButton("Reset placement")) ResetTransforms();
        BakerUI::EndCard();
    }
}

// Right window. Body scrolls; the footer (what is ready + the step's main button) stays at the bottom.
void UIManager::RenderActionPanel() {
    using Step = BakerUI::ShellState::Step;
    const Step step = m_shellState.step;
    static const char* kTitle[] = { "Get started", "UV", "Bake" };
    static const char* kHint[] = { "Add both meshes on the left", "Optional if already unwrapped", "Choose tiles and maps" };
    const int si = static_cast<int>(step);

    if (!BakerUI::BeginSidePanel("Actions###actions", false, BakerUI::kRightW)) {
        ImGui::End();
        return;
    }
    if (BakerUI::gFontBold) ImGui::PushFont(BakerUI::gFontBold);
    ImGui::TextUnformatted(kTitle[si]);
    if (BakerUI::gFontBold) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", kHint[si]);
    ImGui::Separator();

    const ImGuiStyle& style = ImGui::GetStyle();
    const float btnH = ImGui::GetFrameHeight() * 1.5f;
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float slack = style.WindowPadding.y + 10.0f;
    const float footerH = slack + ((step == Step::Import) ? (lineH * 2.0f + btnH + style.ItemSpacing.y * 3.0f)
                        : (step == Step::UV)     ? (lineH + btnH * 2.0f + style.ItemSpacing.y * 4.0f)
                                                 : (lineH * 2.0f + btnH * 2.0f + style.ItemSpacing.y * 4.0f));

    if (ImGui::BeginChild("##action_body", ImVec2(0.0f, -footerH))) {
        if (step == Step::Import) {
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("1. Add a sculpt and a game mesh in the Scene window.");
            ImGui::TextDisabled("2. Prepare the game mesh UVs, or skip this if it is already unwrapped.");
            ImGui::TextDisabled("3. Choose the maps and bake.");
            ImGui::PopTextWrapPos();
        } else if (step == Step::UV) {
            RenderUVStep();
        } else {
            RenderBakeStep();
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    const ImVec2 fullW(-FLT_MIN, btnH);
    auto check = [](bool ok, const char* text) {
        BakerUI::StatusDot(ok ? BakerUI::Col::Ok() : BakerUI::Col::TextDim(), ok);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ok ? BakerUI::Col::Text() : BakerUI::Col::TextDim(), "%s", text);
    };

    switch (step) {
    case Step::Import:
        check(m_hasHighPoly, "Sculpt loaded");
        check(m_hasLowPoly, "Game mesh loaded");
        if (!m_hasLowPoly) ImGui::BeginDisabled();
        if (BakerUI::PrimaryButton("Next: UV", fullW.x, fullW.y)) m_shellState.step = Step::UV;
        if (!m_hasLowPoly) ImGui::EndDisabled();
        break;
    case Step::UV:
        ImGui::TextDisabled("%zu seams, %zu islands", m_lowPolyMesh.cutSeams.size(), m_currentIslands.size());
        BakerUI::DrawCommand(m_commandRegistry, "uv.auto_cut_unwrap_pack", "Auto UV", fullW, BakerUI::CommandStyle::Primary);
        if (BakerUI::SecondaryButton("Next: Bake", fullW.x, fullW.y)) m_shellState.step = Step::Bake;
        break;
    case Step::Bake: {
        const bool canBake = m_hasLowPoly && m_hasHighPoly && !m_isBaking && !m_isLoadingHighPoly;
        int maps = m_settings.bakeNormal + m_settings.bakeWorldNormal + m_settings.bakeDisplacement + m_settings.bakeAmbientOcclusion +
                   m_settings.bakeCavity + m_settings.bakeCurvature + m_settings.bakeThickness + m_settings.bakePosition +
                   m_settings.bakeMaterialID + m_settings.bakeBaseColor + m_settings.bakeDiagnosticMap;
        ImGui::TextDisabled("%d tile(s), %d px, %d map(s)", static_cast<int>(m_selectedUDIMs.empty() ? 1 : m_selectedUDIMs.size()),
                            m_settings.resolution, maps);
        if (m_isBaking)                       ImGui::TextColored(BakerUI::Col::Accent(), "%s", m_bakeStatus.c_str());
        else if (!canBake)                    ImGui::TextColored(BakerUI::Col::Warn(), "%s", !m_hasHighPoly ? "Add a sculpt first." : "Add a game mesh first.");
        else if (m_bakeStatus == "Complete!") ImGui::TextColored(BakerUI::Col::Ok(), "Baked to the output folder.");
        else                                  ImGui::TextDisabled("Ready.");
        if (BakerUI::SecondaryButton("Open output folder", fullW.x, fullW.y))
            ShellExecuteA(NULL, "open", m_settings.outputDir.c_str(), NULL, NULL, SW_SHOWNORMAL);
        if (!canBake && !m_isBaking) ImGui::BeginDisabled();
        if (BakerUI::BakeButton(m_isBaking ? m_bakeProgress : -1.0f, "Bake") && canBake) TriggerBake();
        if (!canBake && !m_isBaking) ImGui::EndDisabled();
        break;
    }
    }
    ImGui::End();
}

// Bake settings in the order a bake is set up. Sections are always open: scroll, do not hunt.
void UIManager::RenderBakeStep() {
    static const char* kRes[] = { "1024", "2048", "4096", "8192 (8K)", "16384 (16K)" };
    static const int kResPx[] = { 1024, 2048, 4096, 8192, 16384 };
    static const char* kAA[] = { "1x (Draft)", "2x2 (Production)", "4x4 (Max)" };
    static const int kAAv[] = { 1, 2, 4 };
    int resIdx = 1, aaIdx = 1;
    for (int i = 0; i < 5; ++i) if (kResPx[i] == m_settings.resolution) resIdx = i;
    for (int i = 0; i < 3; ++i) if (kAAv[i] == m_settings.antiAliasing) aaIdx = i;

    float maxDist = m_hasHighPoly ? m_highPolyMesh.GetBoundingBoxDiagonal() * 0.5f : 10.0f;
    if (maxDist < 1.0f) maxDist = 1.0f;
    char buf[64];

    if (m_hasLowPoly && !m_lowPolyMesh.detectedUDIMs.empty()) {
        BakerUI::SectionLabel("Tiles to bake");
        bool all = true;
        for (int t : m_lowPolyMesh.detectedUDIMs) if (!IsUDIMSelected(t)) { all = false; break; }
        if (ImGui::Checkbox("All", &all)) { if (all) SelectAllUDIMs(); else m_selectedUDIMs.clear(); }
        for (int t : m_lowPolyMesh.detectedUDIMs) {
            bool sel = IsUDIMSelected(t);
            snprintf(buf, sizeof(buf), "%d", t);
            ImGui::SameLine();
            if (ImGui::Checkbox(buf, &sel)) ToggleUDIM(t);
        }
    }

    BakerUI::SectionLabel("Quality");
    if (BakerUI::BeginProps("##bk_quality")) {
        if (BakerUI::PropCombo("Resolution", &resIdx, kRes, 5)) m_settings.resolution = kResPx[resIdx];
        if (BakerUI::PropCombo("Antialiasing", &aaIdx, kAA, 3)) m_settings.antiAliasing = kAAv[aaIdx];
        BakerUI::PropInt("Padding (px)", &m_settings.seamPadding, 0, 64, "Dilation past the UV island edge");
        BakerUI::EndProps();
    }

    BakerUI::SectionLabel("Maps");
    if (BakerUI::BeginProps("##bk_maps")) {
        BakerUI::PropCheck("Tangent normal", &m_settings.bakeNormal, "MikkTSpace tangent-space normal map");
        BakerUI::PropCheck("World normal", &m_settings.bakeWorldNormal, "Raw XYZ world surface vectors");
        BakerUI::PropCheck("Displacement (32f)", &m_settings.bakeDisplacement, "Projection height difference");
        BakerUI::PropCheck("Ambient occlusion", &m_settings.bakeAmbientOcclusion, "Cosine-weighted contact shadows");
        if (m_settings.bakeAmbientOcclusion) {
            BakerUI::PropInt("AO samples", &m_settings.aoSamples, 4, 64);
            BakerUI::PropFloat("AO radius", &m_settings.aoMaxDistance, 0.005f, 0.001f, maxDist, "%.4f");
        }
        BakerUI::PropCheck("Cavity", &m_settings.bakeCavity, "High-frequency micro-crevices");
        BakerUI::PropCheck("Curvature", &m_settings.bakeCurvature, "Convex ridges and concave crevices");
        if (m_settings.bakeCavity || m_settings.bakeCurvature)
            BakerUI::PropFloat("Cavity contrast", &m_settings.cavityStrength, 0.1f, 0.1f, 10.0f, "%.1fx");
        BakerUI::PropCheck("Thickness", &m_settings.bakeThickness, "Inward ray thickness for SSS");
        if (m_settings.bakeThickness)
            BakerUI::PropFloat("Thickness depth", &m_settings.thicknessMaxDistance, 0.005f, 0.001f, maxDist, "%.4f");
        BakerUI::PropCheck("World position", &m_settings.bakePosition, "World XYZ gradient (32f EXR)");
        BakerUI::PropCheck("Material ID", &m_settings.bakeMaterialID, "Distinct color per polygroup");
        BakerUI::PropCheck("Base color", &m_settings.bakeBaseColor, "Polypaint baked to texture");
        BakerUI::PropCheck("Diagnostic", &m_settings.bakeDiagnosticMap, "Missed rays and coverage analysis");
        static const char* const kNormalFormats[] = { "OpenGL (+Y)", "DirectX (-Y)" };
        int fmt = m_settings.flipYNormal ? 1 : 0;
        if (BakerUI::PropCombo("Normal format", &fmt, kNormalFormats, 2)) m_settings.flipYNormal = (fmt == 1);
        BakerUI::EndProps();
    }

    BakerUI::SectionLabel("Projection");
    if (BakerUI::BeginProps("##bk_projection")) {
        if (!m_hasExplicitCage) {
            if (BakerUI::PropFloat("Cage offset", &m_settings.cageDistance, 0.002f, 0.0001f, maxDist, "%.4f", "Push distance along vertex normal"))
                m_viewportRenderer.UpdateCage(m_lowPolyMesh, m_settings.cageDistance, m_settings.lowPolyScale);
        }
        BakerUI::PropFloat("Max ray length", &m_settings.maxRayDistance, 0.005f, 0.001f, maxDist, "%.4f");
        BakerUI::PropFloat("Skew", &m_settings.skewFactor, 0.01f, 0.0f, 1.0f, "%.2f", "0 = smooth normals, 1 = flat face normals");
        BakerUI::PropCheck("Discard backfaces", &m_settings.discardBackfaceHits, "Prevents inverted border artifacts");
        if (m_settings.discardBackfaceHits) BakerUI::PropFloat("Angle limit", &m_settings.angleThreshold, 0.5f, 30.0f, 90.0f, "%.1f deg");
        BakerUI::PropCheck("Bidirectional rays", &m_settings.useBidirectionalRays, "Dual-direction rays catch crevices and protrusions");
        BakerUI::PropCheck("Isolate parts", &m_settings.isolateMeshParts, "Prevent ray bleeding between adjacent parts");
        BakerUI::PropCheck("Compact BVH", &m_settings.useCompactBVH, "For 50M-100M poly sculpts");
        BakerUI::EndProps();
    }

    BakerUI::SectionLabel("Save to");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 70.0f);
    ImGui::InputText("##dir", m_outDirBuf, sizeof(m_outDirBuf));
    m_settings.outputDir = m_outDirBuf;
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(-FLT_MIN, 0))) {
        std::string folder = ChooseFolderDialog();
        if (!folder.empty()) {
            m_settings.outputDir = folder;
            strncpy_s(m_outDirBuf, folder.c_str(), sizeof(m_outDirBuf) - 1);
        }
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##basename", "File name", m_baseNameBuf, sizeof(m_baseNameBuf));
    m_settings.baseFilename = m_baseNameBuf;
    if (BakerUI::BeginProps("##bk_formats")) {
        BakerUI::PropCheck("PNG (8-bit)", &m_settings.exportPNG, "Instant preview and game engines");
        BakerUI::PropCheck("EXR (float)", &m_settings.exportEXR, "Production renderers");
        BakerUI::EndProps();
    }
    if (m_viewportRenderer.HasBakedNormalMap() && BakerUI::SecondaryButton("Clear baked normal preview")) m_shellState.clearNormalPreview = true;
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

    // Split vertices along seams so islands have independent vertex buffers
    UVCutter::SplitMeshAlongSeams(m_lowPolyMesh, islandsTris);

    m_currentIslands = UVUnwrapper::UnwrapMesh(m_lowPolyMesh, islandsTris);
    m_hasUnwrappedIslands = true;
    m_packStats = UVPacker::PackIslands(m_currentIslands, m_packSettings);
    UVPacker::ApplyToMesh(m_lowPolyMesh, m_currentIslands);
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);

    PushHistoryState("Unwrap Mesh");
    char msg[128];
    snprintf(msg, sizeof(msg), "Unwrapped and packed %zu islands successfully", m_currentIslands.size());
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
    // 1. Generate clean closed seams using normal clustering charts (Smart UV Charts)
    UVCutter::GenerateSmartSeams(m_lowPolyMesh, m_seamAngleThreshold);
    if (m_packSettings.packByPolygroup) {
        UVCutter::MarkSeamsByPolygroup(m_lowPolyMesh);
    }
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;

    // 2. Segment islands
    auto islandsTris = UVCutter::SegmentIslands(m_lowPolyMesh);

    // 3. Split vertices along seams so islands have independent vertex buffers
    UVCutter::SplitMeshAlongSeams(m_lowPolyMesh, islandsTris);

    // 4. Conformal unwrap
    m_currentIslands = UVUnwrapper::UnwrapMesh(m_lowPolyMesh, islandsTris);
    m_hasUnwrappedIslands = true;

    // 5. MaxRects BSSF multi-rotation packing
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

// Direct UV edits (relax, snap) change the mesh; keep m_currentIslands matching so later pack/stack use them.
void UIManager::SyncIslandsFromMesh() {
    for (auto& isl : m_currentIslands) {
        for (size_t i = 0; i < isl.vertexIndices.size() && i < isl.uvs.size(); ++i) {
            const auto& uv = m_lowPolyMesh.vertices[isl.vertexIndices[i]].uv;
            isl.uvs[i] = UVPoint(uv.u, uv.v);
        }
        isl.ComputeBounds();
    }
}

void UIManager::RelaxUVs() {
    if (!m_hasLowPoly) return;
    const float before = UVUnwrapper::MeasureDistortion(m_lowPolyMesh);
    if (before <= 0.0f) {
        m_uvStatusMessage = "Nothing to relax: unwrap the mesh first.";
        return;
    }
    const float after = UVUnwrapper::RelaxUVs(m_lowPolyMesh, m_relaxIterations);
    SyncIslandsFromMesh();
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Relax UVs");
    char buf[128];
    snprintf(buf, sizeof(buf), "Relaxed UVs: distortion %.3f -> %.3f (1.0 = no stretch)", before, after);
    m_uvStatusMessage = buf;
    m_logEntries.push_back(std::string("[UV Relax] ") + buf);
}

void UIManager::SnapIslandsToPixels() {
    if (!m_hasLowPoly) return;
    UVUnwrapper::SnapIslandsToPixels(m_lowPolyMesh, m_packSettings.resolution);
    SyncIslandsFromMesh();
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState("Snap Islands to Pixels");
    m_uvStatusMessage = "Snapped island corners to the " + std::to_string(m_packSettings.resolution) + "px grid";
    m_logEntries.push_back("[UV Snap] " + m_uvStatusMessage);
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

void UIManager::CutCurrentUVSelection() {
    if (!m_hasLowPoly) return;
    if (m_uvSelectionMode == UVSelectionMode::Edge) {
        UVCutter::CutSelectedEdges(m_lowPolyMesh);
    } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
        std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
        for (uint32_t fIdx : m_selectedUVFaces) {
            if (fIdx < m_lowPolyMesh.triangles.size()) {
                const auto& tri = m_lowPolyMesh.triangles[fIdx];
                for (int e = 0; e < 3; ++e) {
                    auto edge = MakeEdgeKey(tri.indices[e], tri.indices[(e + 1) % 3]);
                    edgeCount[edge]++;
                }
            }
        }
        for (const auto& kv : edgeCount) {
            if (kv.second == 1) {
                m_lowPolyMesh.cutSeams.insert(kv.first);
            }
        }
        UVCutter::GenerateSeamLineIndices(m_lowPolyMesh);
    } else if (m_uvSelectionMode == UVSelectionMode::Vertex) {
        for (const auto& tri : m_lowPolyMesh.triangles) {
            for (int e = 0; e < 3; ++e) {
                uint32_t v0 = tri.indices[e];
                uint32_t v1 = tri.indices[(e + 1) % 3];
                if (m_selectedUVVertices.count(v0) && m_selectedUVVertices.count(v1)) {
                    m_lowPolyMesh.cutSeams.insert(MakeEdgeKey(v0, v1));
                }
            }
        }
        UVCutter::GenerateSeamLineIndices(m_lowPolyMesh);
    }
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    m_viewportRenderer.showSeams = true;
    PushHistoryState("Cut UV Seams");
    m_uvStatusMessage = "Cut seams along selected UV components";
}

void UIManager::SewCurrentUVSelection() {
    if (!m_hasLowPoly) return;
    if (m_uvSelectionMode == UVSelectionMode::Edge) {
        UVCutter::SewSelectedEdges(m_lowPolyMesh);
    } else if (m_uvSelectionMode == UVSelectionMode::Face || m_uvSelectionMode == UVSelectionMode::Shell) {
        std::map<std::pair<uint32_t, uint32_t>, int> edgeCount;
        for (uint32_t fIdx : m_selectedUVFaces) {
            if (fIdx < m_lowPolyMesh.triangles.size()) {
                const auto& tri = m_lowPolyMesh.triangles[fIdx];
                for (int e = 0; e < 3; ++e) {
                    auto edge = MakeEdgeKey(tri.indices[e], tri.indices[(e + 1) % 3]);
                    edgeCount[edge]++;
                }
            }
        }
        for (const auto& kv : edgeCount) {
            if (kv.second == 1) {
                m_lowPolyMesh.selectedEdges.insert(kv.first);
            }
        }
        UVCutter::SewSelectedEdges(m_lowPolyMesh);
        m_lowPolyMesh.selectedEdges.clear();
        m_lowPolyMesh.GenerateSelectedEdgeLines();
    } else if (m_uvSelectionMode == UVSelectionMode::Vertex) {
        for (const auto& tri : m_lowPolyMesh.triangles) {
            for (int e = 0; e < 3; ++e) {
                uint32_t v0 = tri.indices[e];
                uint32_t v1 = tri.indices[(e + 1) % 3];
                if (m_selectedUVVertices.count(v0) && m_selectedUVVertices.count(v1)) {
                    m_lowPolyMesh.selectedEdges.insert(MakeEdgeKey(v0, v1));
                }
            }
        }
        UVCutter::SewSelectedEdges(m_lowPolyMesh);
        m_lowPolyMesh.selectedEdges.clear();
        m_lowPolyMesh.GenerateSelectedEdgeLines();
    }
    m_viewportRenderer.UpdateSeams(m_lowPolyMesh);
    PushHistoryState("Sew UV Seams");
    m_uvStatusMessage = "Sewed selected UV seams";
}

void UIManager::ClearUVSelection() {
    m_selectedUVVertices.clear();
    m_lowPolyMesh.selectedEdges.clear();
    m_selectedUVFaces.clear();
    m_selectedUVShells.clear();
    m_hasLastSelectedEdge = false;
    m_lowPolyMesh.GenerateSelectedEdgeLines();
    m_viewportRenderer.UpdateSelectedEdges(m_lowPolyMesh);
    m_uvStatusMessage = "Cleared UV component selection";
}

void UIManager::ApplyCameraProjection() {
    if (!m_hasLowPoly) return;
    DirectX::XMMATRIX viewProj = m_viewportRenderer.GetViewProjectionMatrix();

    if (!m_selectedUVFaces.empty()) {
        std::set<uint32_t> faceVerts;
        for (uint32_t fIdx : m_selectedUVFaces) {
            if (fIdx < m_lowPolyMesh.triangles.size()) {
                faceVerts.insert(m_lowPolyMesh.triangles[fIdx].indices[0]);
                faceVerts.insert(m_lowPolyMesh.triangles[fIdx].indices[1]);
                faceVerts.insert(m_lowPolyMesh.triangles[fIdx].indices[2]);
            }
        }
        for (uint32_t vIdx : faceVerts) {
            if (vIdx < m_lowPolyMesh.vertices.size()) {
                const Vec3& p = m_lowPolyMesh.vertices[vIdx].position;
                DirectX::XMVECTOR pW = DirectX::XMVectorSet(p.x, p.y, p.z, 1.0f);
                DirectX::XMVECTOR pClip = DirectX::XMVector3TransformCoord(pW, viewProj);
                float ndcX = DirectX::XMVectorGetX(pClip);
                float ndcY = DirectX::XMVectorGetY(pClip);
                m_lowPolyMesh.vertices[vIdx].uv.u = std::clamp((ndcX + 1.0f) * 0.5f, 0.0f, 1.0f);
                m_lowPolyMesh.vertices[vIdx].uv.v = std::clamp((ndcY + 1.0f) * 0.5f, 0.0f, 1.0f);
            }
        }
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
        PushHistoryState("Camera Project Selected Faces");
        m_uvStatusMessage = "Projected selected faces from camera view";
    } else {
        UVCutter::CameraProjectUVs(m_lowPolyMesh, viewProj);
        auto islandsTris = UVCutter::SegmentIslands(m_lowPolyMesh);
        if (islandsTris.size() > 1) {
            std::vector<UVIsland> islands;
            islands.reserve(islandsTris.size());
            for (size_t islIdx = 0; islIdx < islandsTris.size(); ++islIdx) {
                UVIsland isl;
                isl.islandId = static_cast<int>(islIdx);
                isl.triangleIndices = islandsTris[islIdx];
                std::set<uint32_t> uniqueVerts;
                for (int tIdx : isl.triangleIndices) {
                    if (tIdx >= 0 && tIdx < static_cast<int>(m_lowPolyMesh.triangles.size())) {
                        uniqueVerts.insert(m_lowPolyMesh.triangles[tIdx].indices[0]);
                        uniqueVerts.insert(m_lowPolyMesh.triangles[tIdx].indices[1]);
                        uniqueVerts.insert(m_lowPolyMesh.triangles[tIdx].indices[2]);
                    }
                }
                isl.vertexIndices.assign(uniqueVerts.begin(), uniqueVerts.end());
                isl.uvs.reserve(isl.vertexIndices.size());
                for (uint32_t vIdx : isl.vertexIndices) {
                    isl.uvs.push_back(UVPoint(m_lowPolyMesh.vertices[vIdx].uv.u, m_lowPolyMesh.vertices[vIdx].uv.v));
                }
                isl.ComputeBounds();
                islands.push_back(std::move(isl));
            }
            m_packStats = UVPacker::PackIslands(islands, m_packSettings);
            UVPacker::ApplyToMesh(m_lowPolyMesh, islands);
            m_currentIslands = std::move(islands);
            m_hasUnwrappedIslands = true;
        }
        m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
        PushHistoryState("Maya Camera Projection");
        m_uvStatusMessage = "Projected UVs from camera view across partitioned shells";
    }
}

std::set<uint32_t> UIManager::GetConnectedUVIsland(uint32_t seedTri) {
    std::set<uint32_t> island;
    if (!m_hasLowPoly || seedTri >= m_lowPolyMesh.triangles.size()) return island;

    // Helper to quantize UV coordinate into a 64-bit integer
    auto QuantizeUV = [](const Baker::Vec2& uv) -> uint64_t {
        int32_t qu = static_cast<int32_t>(std::round(uv.u * 10000.0f));
        int32_t qv = static_cast<int32_t>(std::round(uv.v * 10000.0f));
        return (static_cast<uint64_t>(static_cast<uint32_t>(qu)) << 32) | static_cast<uint32_t>(qv);
    };

    typedef std::pair<uint64_t, uint64_t> UVEdgeKey;
    auto MakeUVEdgeKey = [](uint64_t k0, uint64_t k1) -> UVEdgeKey {
        return (k0 < k1) ? UVEdgeKey(k0, k1) : UVEdgeKey(k1, k0);
    };

    std::map<UVEdgeKey, std::vector<uint32_t>> uvEdgeToTris;
    for (size_t t = 0; t < m_lowPolyMesh.triangles.size(); ++t) {
        const auto& tri = m_lowPolyMesh.triangles[t];
        uint64_t k0 = QuantizeUV(m_lowPolyMesh.vertices[tri.indices[0]].uv);
        uint64_t k1 = QuantizeUV(m_lowPolyMesh.vertices[tri.indices[1]].uv);
        uint64_t k2 = QuantizeUV(m_lowPolyMesh.vertices[tri.indices[2]].uv);

        auto e01 = MakeEdgeKey(tri.indices[0], tri.indices[1]);
        auto e12 = MakeEdgeKey(tri.indices[1], tri.indices[2]);
        auto e20 = MakeEdgeKey(tri.indices[2], tri.indices[0]);

        if (!m_lowPolyMesh.cutSeams.count(e01)) {
            uvEdgeToTris[MakeUVEdgeKey(k0, k1)].push_back(static_cast<uint32_t>(t));
        }
        if (!m_lowPolyMesh.cutSeams.count(e12)) {
            uvEdgeToTris[MakeUVEdgeKey(k1, k2)].push_back(static_cast<uint32_t>(t));
        }
        if (!m_lowPolyMesh.cutSeams.count(e20)) {
            uvEdgeToTris[MakeUVEdgeKey(k2, k0)].push_back(static_cast<uint32_t>(t));
        }
    }

    std::vector<uint32_t> q;
    q.push_back(seedTri);
    island.insert(seedTri);

    while (!q.empty()) {
        uint32_t cur = q.back();
        q.pop_back();

        const auto& tri = m_lowPolyMesh.triangles[cur];
        uint64_t k[3] = {
            QuantizeUV(m_lowPolyMesh.vertices[tri.indices[0]].uv),
            QuantizeUV(m_lowPolyMesh.vertices[tri.indices[1]].uv),
            QuantizeUV(m_lowPolyMesh.vertices[tri.indices[2]].uv)
        };

        for (int e = 0; e < 3; ++e) {
            UVEdgeKey edgeKey = MakeUVEdgeKey(k[e], k[(e + 1) % 3]);
            auto it = uvEdgeToTris.find(edgeKey);
            if (it != uvEdgeToTris.end()) {
                for (uint32_t n : it->second) {
                    if (island.insert(n).second) {
                        q.push_back(n);
                    }
                }
            }
        }
    }

    return island;
}

// Vertices the UV edit commands act on: the active selection, or the whole mesh when nothing is selected.
std::vector<uint32_t> UIManager::GetTargetUVVerts() const {
    std::vector<uint32_t> verts;
    if (m_uvSelectionMode == UVSelectionMode::Vertex && !m_selectedUVVertices.empty()) {
        verts.assign(m_selectedUVVertices.begin(), m_selectedUVVertices.end());
    } else if (m_uvSelectionMode == UVSelectionMode::Edge && !m_lowPolyMesh.selectedEdges.empty()) {
        for (const auto& e : m_lowPolyMesh.selectedEdges) {
            verts.push_back(e.first);
            verts.push_back(e.second);
        }
    } else if (!m_selectedUVFaces.empty()) {
        for (uint32_t f : m_selectedUVFaces) {
            if (f < m_lowPolyMesh.triangles.size()) {
                const auto& tri = m_lowPolyMesh.triangles[f];
                verts.insert(verts.end(), { tri.indices[0], tri.indices[1], tri.indices[2] });
            }
        }
    } else {
        verts.resize(m_lowPolyMesh.vertices.size());
        for (size_t i = 0; i < verts.size(); ++i) verts[i] = static_cast<uint32_t>(i);
    }
    std::sort(verts.begin(), verts.end());
    verts.erase(std::unique(verts.begin(), verts.end()), verts.end());
    return verts;
}

// mode: 0 left, 1 right, 2 top, 3 bottom, 4 center U, 5 center V
void UIManager::AlignSelectedUVs(int mode) {
    if (!m_hasLowPoly) return;
    std::vector<uint32_t> verts = GetTargetUVVerts();
    if (verts.empty()) return;
    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    for (uint32_t v : verts) {
        const auto& uv = m_lowPolyMesh.vertices[v].uv;
        minU = (std::min)(minU, uv.u); maxU = (std::max)(maxU, uv.u);
        minV = (std::min)(minV, uv.v); maxV = (std::max)(maxV, uv.v);
    }
    for (uint32_t v : verts) {
        auto& uv = m_lowPolyMesh.vertices[v].uv;
        switch (mode) {
            case 0: uv.u = minU; break;
            case 1: uv.u = maxU; break;
            case 2: uv.v = maxV; break;
            case 3: uv.v = minV; break;
            case 4: uv.u = (minU + maxU) * 0.5f; break;
            default: uv.v = (minV + maxV) * 0.5f; break;
        }
    }
    static const char* kNames[] = { "Left", "Right", "Top", "Bottom", "Center U", "Center V" };
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    std::string msg = std::string("Align UVs ") + kNames[mode];
    PushHistoryState(msg);
    m_uvStatusMessage = msg;
}

// Flat projection of the selected faces (or the whole mesh) along a world axis, normalised into the 0-1 tile.
void UIManager::PlanarProjectUVs(int axis) {
    if (!m_hasLowPoly) return;
    std::vector<uint32_t> verts = GetTargetUVVerts();
    if (verts.empty()) return;
    const int a = (axis + 1) % 3, b = (axis + 2) % 3;
    auto Comp = [](const Vec3& p, int i) { return i == 0 ? p.x : (i == 1 ? p.y : p.z); };
    float minA = 1e9f, maxA = -1e9f, minB = 1e9f, maxB = -1e9f;
    for (uint32_t v : verts) {
        const Vec3& p = m_lowPolyMesh.vertices[v].position;
        minA = (std::min)(minA, Comp(p, a)); maxA = (std::max)(maxA, Comp(p, a));
        minB = (std::min)(minB, Comp(p, b)); maxB = (std::max)(maxB, Comp(p, b));
    }
    const float span = (std::max)((std::max)(maxA - minA, maxB - minB), 1e-6f); // uniform scale keeps texel density square
    for (uint32_t v : verts) {
        auto& vert = m_lowPolyMesh.vertices[v];
        vert.uv.u = (Comp(vert.position, a) - minA) / span;
        vert.uv.v = (Comp(vert.position, b) - minB) / span;
    }
    static const char* kAxis[] = { "X", "Y", "Z" };
    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    std::string msg = std::string("Planar projection along ") + kAxis[axis];
    PushHistoryState(msg);
    m_uvStatusMessage = msg;
}

void UIManager::FlipSelectedUVs(bool horizontal) {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;

    std::vector<uint32_t> targetVerts = GetTargetUVVerts();
    if (targetVerts.empty()) return;

    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    for (uint32_t v : targetVerts) {
        if (v < m_lowPolyMesh.vertices.size()) {
            minU = (std::min)(minU, m_lowPolyMesh.vertices[v].uv.u);
            maxU = (std::max)(maxU, m_lowPolyMesh.vertices[v].uv.u);
            minV = (std::min)(minV, m_lowPolyMesh.vertices[v].uv.v);
            maxV = (std::max)(maxV, m_lowPolyMesh.vertices[v].uv.v);
        }
    }
    float centerU = (minU + maxU) * 0.5f;
    float centerV = (minV + maxV) * 0.5f;

    for (uint32_t v : targetVerts) {
        if (v < m_lowPolyMesh.vertices.size()) {
            if (horizontal) {
                m_lowPolyMesh.vertices[v].uv.u = centerU - (m_lowPolyMesh.vertices[v].uv.u - centerU);
            } else {
                m_lowPolyMesh.vertices[v].uv.v = centerV - (m_lowPolyMesh.vertices[v].uv.v - centerV);
            }
        }
    }

    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    PushHistoryState(horizontal ? "Flip UVs Horizontally (U)" : "Flip UVs Vertically (V)");
    m_uvStatusMessage = horizontal ? "Flipped selected UVs along U-axis" : "Flipped selected UVs along V-axis";
}

void UIManager::RotateSelectedUVs(float angleDeg) {
    if (!m_hasLowPoly || m_lowPolyMesh.vertices.empty()) return;

    std::vector<uint32_t> targetVerts = GetTargetUVVerts();
    if (targetVerts.empty()) return;

    float minU = 1e9f, maxU = -1e9f, minV = 1e9f, maxV = -1e9f;
    for (uint32_t v : targetVerts) {
        if (v < m_lowPolyMesh.vertices.size()) {
            minU = (std::min)(minU, m_lowPolyMesh.vertices[v].uv.u);
            maxU = (std::max)(maxU, m_lowPolyMesh.vertices[v].uv.u);
            minV = (std::min)(minV, m_lowPolyMesh.vertices[v].uv.v);
            maxV = (std::max)(maxV, m_lowPolyMesh.vertices[v].uv.v);
        }
    }
    float centerU = (minU + maxU) * 0.5f;
    float centerV = (minV + maxV) * 0.5f;

    float rad = DirectX::XMConvertToRadians(angleDeg);
    float cosA = std::cos(rad);
    float sinA = std::sin(rad);

    for (uint32_t v : targetVerts) {
        if (v < m_lowPolyMesh.vertices.size()) {
            float du = m_lowPolyMesh.vertices[v].uv.u - centerU;
            float dv = m_lowPolyMesh.vertices[v].uv.v - centerV;
            m_lowPolyMesh.vertices[v].uv.u = centerU + (du * cosA - dv * sinA);
            m_lowPolyMesh.vertices[v].uv.v = centerV + (du * sinA + dv * cosA);
        }
    }

    m_viewportRenderer.SetLowPolyMesh(m_lowPolyMesh, m_settings.lowPolyScale);
    char buf[64];
    snprintf(buf, sizeof(buf), "Rotate UVs (%.0f deg)", angleDeg);
    PushHistoryState(buf);
    m_uvStatusMessage = buf;
}

void UIManager::RenderHelpWindows() {
    if (m_showShortcutsWindow) {
        const float em = ImGui::GetFontSize();
        ImGui::SetNextWindowSize(ImVec2(em * 30.0f, em * 26.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Keyboard Shortcuts###help_shortcuts", &m_showShortcutsWindow, ImGuiWindowFlags_NoDocking)) {
            if (ImGui::BeginTable("##shortcut_table", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Shortcut", ImGuiTableColumnFlags_WidthFixed, em * 10.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();
                for (const std::string& id : m_commandRegistry.GetOrderedIds()) {
                    const BakerUI::Command* cmd = m_commandRegistry.Find(id);
                    if (!cmd || cmd->hotkey.empty()) continue;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(cmd->label.c_str());
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("%s", cmd->hotkey.c_str());
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    if (m_showGesturesWindow) {
        if (ImGui::Begin("Viewport Gestures###help_gestures", &m_showGesturesWindow, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::SeparatorText("3D Viewport and 2D Canvas");
            ImGui::BulletText("Double-click edge: select edge loop");
            ImGui::BulletText("Shift+click edge: select shortest path between edges");
            ImGui::BulletText("Double-click or L over a shell: select the whole shell");
            ImGui::BulletText("Click empty space: clear the selection");
            ImGui::BulletText("Alt+LMB drag: orbit (3D) or pan (2D)");
            ImGui::BulletText("Right-click (no drag): context menu");
            ImGui::BulletText("Right-drag: dolly (3D) or pan (2D)");
            ImGui::BulletText("Curve tool: click to add points, Enter for the next curve, Esc to exit");
            ImGui::SeparatorText("Marking menu (Shift+Right-click, then flick)");
            ImGui::TextDisabled("Release without flicking to keep it open and click a label.");
            if (ImGui::BeginTable("##gesture_wedges", 3, ImGuiTableFlags_SizingFixedSame)) {
                // Same layout as kMarkingWedges, read as a 3x3 grid (angles in screen space, 0 = east)
                const char* grid[3][3] = {};
                for (const MarkingWedge& w : kMarkingWedges) {
                    const float rad = w.angleDeg * 3.14159265f / 180.0f;
                    const int col = 1 + static_cast<int>(std::lround(std::cos(rad)));
                    const int row = 1 + static_cast<int>(std::lround(std::sin(rad)));
                    grid[row][col] = w.label;
                }
                for (auto& row : grid) {
                    ImGui::TableNextRow();
                    for (int col = 0; col < 3; ++col) {
                        ImGui::TableSetColumnIndex(col);
                        ImGui::TextUnformatted(row[col] ? row[col] : "");
                    }
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    if (m_showAboutWindow) {
        if (ImGui::Begin("About###help_about", &m_showAboutWindow, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted("ZBrush UDIM Baker");
            ImGui::TextDisabled("UDIM bake and UV companion for ZBrush");
            ImGui::TextDisabled("Embree 3  *  D3D11");
        }
        ImGui::End();
    }
}

void UIManager::RenderBakeProgressPanel() {
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->WorkPos.x + 24.0f, ImGui::GetMainViewport()->WorkPos.y + BakerUI::TopBarHeight() + 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(620.0f, 240.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Output Log###log", &m_shellState.showLog, ImGuiWindowFlags_NoDocking)) {
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
