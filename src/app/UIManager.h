#pragma once

#include "../baker/MeshTypes.h"
#include "../baker/BakerEngine.h"
#include "../uv/UVTypes.h"
#include "../uv/UVCutter.h"
#include "../uv/UVUnwrapper.h"
#include "../uv/UVPacker.h"
#include "../uv/UVSurfaceCurve.h"
#include "ViewportRenderer.h"
#include "BakerUI.h"
#include "HistoryManager.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>

namespace Baker {

class UIManager {
public:
    UIManager();
    ~UIManager();

    void Initialize(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr);
    void RenderUI();

    void SetCommandLineHighPoly(const std::string& path);
    void SetCommandLineLowPoly(const std::string& path);
    void AutoDetectZBrushCache(bool force = false);
    void CheckZBrushLiveSync();
    void ClearLowPoly();
    void ClearHighPoly();
    void ClearCage();
    void ClearScene();

    enum class ViewportMode {
        View3D = 0,
        View2D = 1,
        SplitBoth = 2
    };

    enum class ToolMode {
        Navigate = 0,
        PaintCage = 1,
        PaintSkew = 2,
        CutSeam = 3,
        DrawCurve = 4
    };

private:
    void RenderOutlinerPanel();
    void RenderViewportPanel();
    void RenderFloatingUVBar(const ImVec2& origin, const ImVec2& avail);
    void Render2DUVCanvas(const ImVec2& origin, const ImVec2& size);
    void RenderMeshPanel();
    void RenderSettingsPanel();
    void RenderUVCutAndPackPanel();
    void RenderUDIMCanvasPanel();
    void RenderBakeProgressPanel();

    // UV Cutting & Packing Actions
    void MarkSeamsByAngle();
    void MarkSeamsByPolygroup();
    void MarkSeamsByUVBorders();
    void ClearSeams();
    void UnwrapMesh();
    void PackUVs();
    void AutoCutUnwrapAndPack();
    void ExportUnwrappedOBJ();

    // UV Curve Actions
    void SnapActiveCurveToSeams();
    void CutActiveCurveIntoTopology();
    void BakeAllCurvesToSeams(bool cutIntoTopology);
    void ClearActiveCurve();
    void ClearAllCurves();

    void TriggerBake();
    void SyncTransforms();
    void AutoAlignMeshCenters();
    void AutoFitMeshScale();
    void ResetTransforms();

    void EnsureCageInitialized();
    void EnsureSkewInitialized();
    void ResetPaintedCage();
    void ResetPaintedSkew();

    // Session State
    LowPolyMesh m_lowPolyMesh;
    HighPolyMesh m_highPolyMesh;
    bool m_hasLowPoly = false;
    bool m_hasHighPoly = false;
    bool m_hasExplicitCage = false;

    std::string m_lowPolyPath;
    std::string m_cagePath;
    std::string m_highPolyPath;
    BakeSettings m_settings;
    BakerEngine m_engine;
    ViewportRenderer m_viewportRenderer;

    ViewportMode m_viewportMode = ViewportMode::View3D;
    ToolMode m_toolMode = ToolMode::Navigate;
    float m_brushRadius = 0.5f;
    float m_brushStrength = 0.5f;
    bool m_brushInvert = false;
    float m_uvZoom = 1.0f;
    ImVec2 m_uvPan = ImVec2(0.0f, 0.0f);
    float m_splitRatio = 0.5f; // Draggable sliding splitter ratio between 3D & 2D views
    int m_selectedUdimTile = 1001;
    bool m_uniformScale = true;

    float m_detectedScaleRatio = 1.0f;
    bool m_autoScaleLowPoly = false;
    void CheckScaleAndAutoCalibrate();

    std::vector<int> m_selectedUDIMs;
    bool IsUDIMSelected(int tile) const;
    void ToggleUDIM(int tile);
    void SelectAllUDIMs();
    void SelectOnlyUDIM(int tile);

    char m_outDirBuf[512] = "./";
    char m_baseNameBuf[128] = "Sculpt_Bake";

    // Progress State & Shell State
    bool m_isBaking = false;
    float m_bakeProgress = 0.0f;
    std::string m_bakeStatus = "Ready";
    std::vector<std::string> m_logEntries;
    BakerUI::ShellState m_shellState;
    BakerUI::ShellState::AppTab m_prevActiveTab = BakerUI::ShellState::AppTab::Baking;

    // UV Cutting & Packing State (ZenUV & UVPackmaster Facilities)
    enum class UVSelectionMode {
        Vertex,
        Edge,
        Face,
        Shell
    };
    UVSelectionMode m_uvSelectionMode = UVSelectionMode::Edge;
    std::set<uint32_t> m_selectedUVVertices;
    std::set<uint32_t> m_selectedUVFaces;
    std::set<uint32_t> m_selectedUVShells;

    // 2D Marquee Box Selection State
    bool m_isMarqueeSelecting = false;
    ImVec2 m_marqueeStart = ImVec2(0, 0);
    ImVec2 m_marqueeEnd = ImVec2(0, 0);

    // 2D UV Transform Gizmo State
    enum class UVGizmoMode {
        Translate,
        Rotate,
        Scale
    };
    enum class UVGizmoDragPart {
        None,
        CenterFree,
        AxisU,
        AxisV,
        RotateRing,
        ScaleUniform,
        ScaleU,
        ScaleV
    };
    UVGizmoMode m_uvGizmoMode = UVGizmoMode::Translate;
    UVGizmoDragPart m_activeGizmoPart = UVGizmoDragPart::None;
    ImVec2 m_gizmoDragMouseStart = ImVec2(0, 0);
    UVPoint m_gizmoCenterStart = UVPoint(0, 0);
    std::vector<UVPoint> m_gizmoInitialUVs;
    std::vector<uint32_t> m_gizmoSelectedVerts;
    float m_gizmoInitialAngle = 0.0f;
    float m_gizmoInitialDist = 1.0f;

    bool m_showUVChecker = false;

    std::pair<uint32_t, uint32_t> m_lastSelectedEdge = { UINT32_MAX, UINT32_MAX };
    bool m_hasLastSelectedEdge = false;
    float m_seamAngleThreshold = 60.0f;
    PackSettings m_packSettings;
    PackStats m_packStats;
    std::vector<UVIsland> m_currentIslands;
    bool m_hasUnwrappedIslands = false;
    std::string m_uvStatusMessage = "Ready";
    int m_relaxIterations = 40;
    bool m_focusUVTab = false;

    // ZenUV & UVPM Pro Features
    float m_targetTexelDensity = 10.24f; // px/cm (default for 2K map)
    float m_calculatedTexelDensity = 0.0f;
    bool m_lockOverlapping = true;
    bool m_showDistortionHeatmap = false;
    void CalculateTexelDensity();
    void SetTargetTexelDensity(float targetTD);
    void EqualizeTexelDensity();
    void StackIdenticalIslands();
    void SeparateOverlappingIslands();
    void StraightenIslands();
    void OrientIslandsToWorld();

    // Maya UV Viewport Actions
    void CutCurrentUVSelection();
    void SewCurrentUVSelection();
    void ClearUVSelection();
    void ApplyCameraProjection();

    // UV Surface Curves State
    CurveManager m_curveManager;
    bool m_curveCutTopology = false; // false = snap to mesh edges, true = cut into topology
    int m_draggingPointIndex = -1;
    bool m_wasDraggingPoint = false;

    // Undo / Redo History System (Ctrl+Z / Ctrl+Shift+Z)
    HistoryManager m_history;
    void PushHistoryState(const std::string& desc);
    void PerformUndo();
    void PerformRedo();

    // Maya-Style Marking Menu State
    struct MarkingMenuState {
        bool active = false;
        ImVec2 centerPos = ImVec2(0, 0);
        int armedWedge = -1; // 0=North(Cut), 1=South(Sew), 2=East(Loop), 3=West(Unwrap), 4=NE(SelectSeams), 5=SW(Clear)
        float dragDist = 0.0f;
    } m_markingMenu;

    void RenderMarkingMenu();
    void ExecuteMarkingMenuAction(int wedge);

    // Background Thread Ingestion State
    std::mutex m_meshMutex;
    std::atomic<bool> m_isLoadingHighPoly{false};
    std::atomic<bool> m_pendingMeshUpload{false};
    std::string m_loadingStatusText;

    // Background Staged Baked Normal Map State
    std::mutex m_normalMutex;
    std::vector<float> m_stagedNormalR;
    std::vector<float> m_stagedNormalG;
    std::vector<float> m_stagedNormalB;
    int m_stagedNormalW = 0;
    int m_stagedNormalH = 0;
    std::atomic<bool> m_pendingNormalUpload{false};
};

} // namespace Baker
