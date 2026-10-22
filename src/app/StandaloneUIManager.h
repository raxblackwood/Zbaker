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
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace Baker {

class StandaloneUIManager {
public:
    StandaloneUIManager();
    ~StandaloneUIManager();

    void Initialize(ID3D11Device* device = nullptr, ID3D11DeviceContext* context = nullptr);
    void RenderUI();

    void SetHighPolyMeshPath(const std::string& path);
    void SetLowPolyMeshPath(const std::string& path);
    void SetCageMeshPath(const std::string& path);

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
    void Render2DUVCanvas(const ImVec2& origin, const ImVec2& size);
    void RenderMeshPanel();
    void RenderSettingsPanel();
    void RenderUVCutAndPackPanel();
    void RenderUDIMCanvasPanel();
    void RenderBakeProgressPanel();
    void RenderLogPanel();

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
    void CheckScaleAndAutoCalibrate();

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
    int m_selectedUdimTile = 1001;
    bool m_uniformScale = true;

    float m_detectedScaleRatio = 1.0f;
    bool m_autoScaleLowPoly = false;

    std::vector<int> m_selectedUDIMs;
    bool IsUDIMSelected(int tile) const;
    void ToggleUDIM(int tile);
    void SelectAllUDIMs();
    void SelectOnlyUDIM(int tile);

    char m_outDirBuf[512] = "./bakes";
    char m_baseNameBuf[128] = "BakedMesh";

    // Progress State & Shell State
    bool m_isBaking = false;
    float m_bakeProgress = 0.0f;
    std::string m_bakeStatus = "Ready";
    std::vector<std::string> m_logEntries;
    BakerUI::ShellState m_shellState;

    // UV Cutting & Packing State
    float m_seamAngleThreshold = 60.0f;
    PackSettings m_packSettings;
    PackStats m_packStats;
    std::vector<UVIsland> m_currentIslands;
    bool m_hasUnwrappedIslands = false;
    std::string m_uvStatusMessage = "Ready";
    int m_relaxIterations = 40;
    bool m_focusUVTab = false;

    // UV Surface Curves State
    CurveManager m_curveManager;
    bool m_curveCutTopology = false; // false = snap to mesh edges, true = cut into topology
    int m_draggingPointIndex = -1;

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
