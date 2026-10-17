#pragma once

#include "../baker/MeshTypes.h"
#include "../baker/BakerEngine.h"
#include <string>
#include <vector>

namespace Baker {

class UIManager {
public:
    UIManager();
    ~UIManager();

    void Initialize();
    void RenderUI();

    void SetCommandLineHighPoly(const std::string& path);
    void SetCommandLineLowPoly(const std::string& path);
    void AutoDetectZBrushCache();

private:
    void RenderMenuBar();
    void RenderMeshPanel();
    void RenderSettingsPanel();
    void RenderUDIMCanvasPanel();
    void RenderBakeProgressPanel();

    void TriggerBake();

    // Session State
    LowPolyMesh m_lowPolyMesh;
    HighPolyMesh m_highPolyMesh;
    bool m_hasLowPoly = false;
    bool m_hasHighPoly = false;
    bool m_hasExplicitCage = false;

    std::string m_lowPolyPath;
    std::string m_cagePath;
    std::string m_highPolyPath;
    std::string m_statusMessage = "Ready. Load a Low-Poly and High-Poly mesh to begin.";

    BakeSettings m_settings;
    BakerEngine m_engine;

    // Progress State
    bool m_isBaking = false;
    float m_bakeProgress = 0.0f;
    std::string m_bakeStatus = "";
    std::vector<std::string> m_logEntries;
};

} // namespace Baker
