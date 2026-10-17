#pragma once

#include "../baker/MeshTypes.h"
#include "../baker/BakerEngine.h"
#include "ViewportRenderer.h"
#include "BakerUI.h"
#include <string>
#include <vector>

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

    enum class ViewportMode {
        View3D = 0,
        View2D = 1,
        SplitBoth = 2
    };

private:
    void RenderOutlinerPanel();
    void RenderViewportPanel();
    void Render2DUVCanvas(const ImVec2& origin, const ImVec2& size);
    void RenderMeshPanel();
    void RenderSettingsPanel();
    void RenderUDIMCanvasPanel();
    void RenderBakeProgressPanel();

    void TriggerBake();
    void SyncTransforms();
    void AutoAlignMeshCenters();
    void AutoFitMeshScale();
    void ResetTransforms();

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
    float m_uvZoom = 1.0f;
    ImVec2 m_uvPan = ImVec2(0.0f, 0.0f);
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
};

} // namespace Baker
