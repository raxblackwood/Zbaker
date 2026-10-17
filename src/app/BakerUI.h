// BakerUI.h  -  Modern docked UI kit for the ZBrush UDIM Baker companion
// Requires Dear ImGui DOCKING branch (imgui.h + imgui_internal.h).
//
// Usage:
//   BakerUI::LoadFonts(io, dpiScale);          // before backend font build
//   BakerUI::ApplyTheme(dpiScale);             // once after CreateContext
//   io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
//   io.IniFilename = "baker_layout.ini";
//   // each frame:
//   BakerUI::BeginFrameHost(state);            // menu/toolbar/dockspace/status bar
//   PanelIngestion(); PanelParams(); PanelViewport(); PanelLog(); PanelUdim();
//   BakerUI::EndFrameHost();
#pragma once
#include "imgui.h"
#include "imgui_internal.h"
#include <cstdio>
#include <cmath>
#include <string>

namespace BakerUI {

enum class UITheme {
    MarmosetEmber = 0,
    CyberTeal = 1,
    StudioSlate = 2
};

inline UITheme gCurrentTheme = UITheme::MarmosetEmber;

// ---------------------------------------------------------------- palette
inline ImVec4 Hex(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
namespace Col {
    inline ImVec4 Bg() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0x111317);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x0E1316);
        return Hex(0x131519);
    }
    inline ImVec4 Panel() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0x181B22);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x141B20);
        return Hex(0x191C22);
    }
    inline ImVec4 PanelHi() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0x20242D);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x1A232A);
        return Hex(0x21252D);
    }
    inline ImVec4 Border() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0x2E3442);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x25353F);
        return Hex(0x2C313C);
    }
    inline ImVec4 Text()      { return Hex(0xEDEDF2); }
    inline ImVec4 TextDim()   { return Hex(0x8D94A5); }
    inline ImVec4 Accent() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0xFF7A18); // Warm Marmoset Ember Orange
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x00D2B4); // Cyber Teal
        return Hex(0x4E92F9); // Crisp Studio Blue
    }
    inline ImVec4 AccentHi() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0xFF9438);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x38F0D6);
        return Hex(0x73ABFF);
    }
    inline ImVec4 AccentLo() {
        if (gCurrentTheme == UITheme::MarmosetEmber) return Hex(0xD96206);
        if (gCurrentTheme == UITheme::CyberTeal)     return Hex(0x00A890);
        return Hex(0x3472D9);
    }
    inline ImVec4 Ok()        { return Hex(0x3DDC97); }
    inline ImVec4 Warn()      { return Hex(0xFFB547); }
    inline ImVec4 Err()       { return Hex(0xFF5C6C); }
}

// ---------------------------------------------------------------- fonts
// Drop Inter-Regular.ttf / Inter-SemiBold.ttf / JetBrainsMono-Regular.ttf in assets/fonts.
inline ImFont* gFontBold = nullptr;
inline ImFont* gFontMono = nullptr;
inline void LoadFonts(ImGuiIO& io, float dpi = 1.f) {
    const float px = 15.f * dpi;
    ImFont* f = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-Regular.ttf", px);
    if (!f) f = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", px);
    if (!f) io.Fonts->AddFontDefault(); // graceful fallback

    gFontBold = io.Fonts->AddFontFromFileTTF("assets/fonts/Inter-SemiBold.ttf", px);
    if (!gFontBold) gFontBold = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeuib.ttf", px);
    if (!gFontBold) gFontBold = io.FontDefault;

    gFontMono = io.Fonts->AddFontFromFileTTF("assets/fonts/JetBrainsMono-Regular.ttf", px * 0.95f);
    if (!gFontMono) gFontMono = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/consola.ttf", px * 0.95f);
    if (!gFontMono) gFontMono = io.FontDefault;
}

// ---------------------------------------------------------------- theme
inline void ApplyTheme(float dpi = 1.f) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowPadding     = ImVec2(14, 12);
    s.FramePadding      = ImVec2(10, 7);
    s.ItemSpacing       = ImVec2(10, 9);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.CellPadding       = ImVec2(6, 5);
    s.ScrollbarSize     = 10;
    s.GrabMinSize       = 12;
    s.WindowRounding    = 8;
    s.ChildRounding     = 8;
    s.FrameRounding     = 6;
    s.PopupRounding     = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding      = 6;
    s.TabRounding       = 6;
    s.WindowBorderSize  = 1;
    s.FrameBorderSize   = 0;
    s.TabBarBorderSize  = 1;
    s.DockingSeparatorSize = 2;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ScaleAllSizes(dpi);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = Col::Text();
    c[ImGuiCol_TextDisabled]         = Col::TextDim();
    c[ImGuiCol_WindowBg]             = Col::Panel();
    c[ImGuiCol_ChildBg]              = Col::PanelHi();
    c[ImGuiCol_PopupBg]              = Hex(0x1B2029, 0.98f);
    c[ImGuiCol_Border]               = Col::Border();
    c[ImGuiCol_BorderShadow]         = Hex(0, 0);
    c[ImGuiCol_FrameBg]              = Col::PanelHi();
    c[ImGuiCol_FrameBgHovered]       = Hex(0x232A36);
    c[ImGuiCol_FrameBgActive]        = Hex(0x2A3342);
    c[ImGuiCol_TitleBg]              = Col::Bg();
    c[ImGuiCol_TitleBgActive]        = Col::Bg();
    c[ImGuiCol_TitleBgCollapsed]     = Col::Bg();
    c[ImGuiCol_MenuBarBg]            = Col::Bg();
    c[ImGuiCol_ScrollbarBg]          = Hex(0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Hex(0x2E3544);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x3A4356);
    c[ImGuiCol_ScrollbarGrabActive]  = Col::Accent();
    c[ImGuiCol_CheckMark]            = Col::Accent();
    c[ImGuiCol_SliderGrab]           = Col::Accent();
    c[ImGuiCol_SliderGrabActive]     = Col::AccentHi();
    c[ImGuiCol_Button]               = Hex(0x222936);
    c[ImGuiCol_ButtonHovered]        = Hex(0x2B3445);
    c[ImGuiCol_ButtonActive]         = Hex(0x343F54);
    c[ImGuiCol_Header]               = Hex(0x222936);
    c[ImGuiCol_HeaderHovered]        = Hex(0x2B3445);
    c[ImGuiCol_HeaderActive]         = Hex(0x343F54);
    c[ImGuiCol_Separator]            = Col::Border();
    c[ImGuiCol_SeparatorHovered]     = Col::Accent();
    c[ImGuiCol_SeparatorActive]      = Col::AccentHi();
    c[ImGuiCol_ResizeGrip]           = Hex(0, 0);
    c[ImGuiCol_ResizeGripHovered]    = Col::Accent();
    c[ImGuiCol_ResizeGripActive]     = Col::AccentHi();
    c[ImGuiCol_Tab]                  = Col::Bg();
    c[ImGuiCol_TabHovered]           = Hex(0x232A36);
    c[ImGuiCol_TabSelected]          = Col::Panel();
    c[ImGuiCol_TabDimmed]            = Col::Bg();
    c[ImGuiCol_TabDimmedSelected]    = Col::Panel();
    c[ImGuiCol_DockingPreview]       = Hex(0x5B8CFF, 0.35f);
    c[ImGuiCol_DockingEmptyBg]       = Col::Bg();
    c[ImGuiCol_TableHeaderBg]        = Col::Bg();
    c[ImGuiCol_TableBorderLight]     = Col::Border();
    c[ImGuiCol_TextSelectedBg]       = Hex(0x5B8CFF, 0.35f);
    c[ImGuiCol_PlotHistogram]        = Col::Accent();
}

// ---------------------------------------------------------------- widgets
// Small uppercase section label with hairline.
inline void SectionLabel(const char* text) {
    ImGui::Dummy(ImVec2(0, 2));
    ImGui::PushStyleColor(ImGuiCol_Text, Col::TextDim());
    if (gFontBold) ImGui::PushFont(gFontBold);
    ImGui::TextUnformatted(text);
    if (gFontBold) ImGui::PopFont();
    ImGui::PopStyleColor();
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(a, ImVec2(a.x + ImGui::GetContentRegionAvail().x, a.y),
                                        ImGui::GetColorU32(Col::Border()));
    ImGui::Dummy(ImVec2(0, 4));
}

// Accent (primary) button, full width by default.
inline bool PrimaryButton(const char* label, float w = -FLT_MIN, float h = 0) {
    ImGui::PushStyleColor(ImGuiCol_Button, Col::AccentLo());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::Accent());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col::AccentHi());
    ImGui::PushStyleColor(ImGuiCol_Text, Hex(0xFFFFFF));
    bool r = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopStyleColor(4);
    return r;
}

// Subtle full-width secondary button.
inline bool SecondaryButton(const char* label, float w = -FLT_MIN) {
    return ImGui::Button(label, ImVec2(w, 0));
}

// Pill-shaped toggle (used on the viewport overlay instead of checkboxes).
inline bool ToggleChip(const char* label, bool* v) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 999.f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 5));
    ImGui::PushStyleColor(ImGuiCol_Button,        *v ? Hex(0x5B8CFF, 0.22f) : Hex(0x1B2029, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, *v ? Hex(0x5B8CFF, 0.32f) : Hex(0x2B3445, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Text,          *v ? Col::AccentHi() : Col::TextDim());
    bool clicked = ImGui::Button(label);
    if (clicked) *v = !*v;
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(2);
    return clicked;
}

// Colored status pill ("BVH ready", "No low-poly").
inline void StatusPill(const char* text, ImVec4 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(10, 4);
    ImVec2 sz(ts.x + pad.x * 2 + 12, ts.y + pad.y * 2);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.16f)), 999.f);
    dl->AddCircleFilled(ImVec2(p.x + pad.x + 3, p.y + sz.y * 0.5f), 3.f, ImGui::GetColorU32(color));
    dl->AddText(ImVec2(p.x + pad.x + 12, p.y + pad.y), ImGui::GetColorU32(color), text);
    ImGui::Dummy(sz);
}

// Two-column label | control row. Wrap rows in BeginProps/EndProps.
inline bool BeginProps(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) return false;
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("r", ImGuiTableColumnFlags_WidthStretch, 0.58f);
    return true;
}
inline void EndProps() { ImGui::EndTable(); }
inline void PropLabel(const char* label, const char* tooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Col::TextDim());
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
}
inline bool PropFloat(const char* label, float* v, float speed, float lo, float hi,
                      const char* fmt = "%.4f", const char* tip = nullptr) {
    PropLabel(label, tip);
    char id[64]; snprintf(id, sizeof id, "##%s", label);
    return ImGui::DragFloat(id, v, speed, lo, hi, fmt);
}
inline bool PropInt(const char* label, int* v, int lo, int hi, const char* tip = nullptr) {
    PropLabel(label, tip);
    char id[64]; snprintf(id, sizeof id, "##%s", label);
    return ImGui::SliderInt(id, v, lo, hi);
}
inline bool PropCombo(const char* label, int* cur, const char* const* items, int n) {
    PropLabel(label);
    char id[64]; snprintf(id, sizeof id, "##%s", label);
    return ImGui::Combo(id, cur, items, n);
}
inline bool PropCheck(const char* label, bool* v, const char* tip = nullptr) {
    PropLabel(label, tip);
    char id[64]; snprintf(id, sizeof id, "##%s", label);
    return ImGui::Checkbox(id, v);
}

// Rounded "card" container; use for grouping (e.g. one per mesh source).
inline bool BeginCard(const char* id, float h = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::PushStyleColor(ImGuiCol_Border, Col::Border());
    return ImGui::BeginChild(id, ImVec2(0, h),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
inline void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// Bake button that morphs into a progress bar while running.
// progress < 0 -> idle. Returns true on click (idle) .
inline bool BakeButton(float progress, const char* idleLabel = "START BAKE") {
    const float h = 44.f * (ImGui::GetFontSize() / 16.f);
    ImVec2 size(ImGui::GetContentRegionAvail().x, h);
    if (progress < 0.f) {
        if (gFontBold) ImGui::PushFont(gFontBold);
        bool r = PrimaryButton(idleLabel, size.x, size.y);
        if (gFontBold) ImGui::PopFont();
        return r;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Col::PanelHi()), 6.f);
    dl->AddRectFilled(p, ImVec2(p.x + size.x * progress, p.y + size.y), ImGui::GetColorU32(Col::AccentLo()), 6.f);
    char buf[48]; snprintf(buf, sizeof buf, "Baking...  %d%%", (int)(progress * 100));
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(Hex(0xFFFFFF)), buf);
    ImGui::Dummy(size);
    return false;
}

// ---------------------------------------------------------------- app shell
struct ShellState {
    const char* statusText = "Ready";
    ImVec4      statusColor = Col::Ok();
    float       bakeProgress = -1.f;     // -1 idle
    bool        resetLayout = false;     // View > Reset Layout
    bool        showLog = true, showUdim = true, showOutliner = true;
    bool        openLowPoly = false;
    bool        openHighPoly = false;
    bool        openCage = false;
    bool        exitApp = false;
};

inline void BuildDefaultLayout(ImGuiID ds, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(ds);
    ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ds, size);

    ImGuiID center = ds, left, right, bottom;
    left   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.25f, nullptr, &center);
    right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.26f, nullptr, &center);

    ImGuiID leftTop = left, leftBottom;
    leftBottom = ImGui::DockBuilderSplitNode(leftTop, ImGuiDir_Down, 0.52f, nullptr, &leftTop);

    ImGui::DockBuilderDockWindow("Scene Outliner###outliner", leftTop);
    ImGui::DockBuilderDockWindow("Mesh Ingestion###ingest",   leftBottom);
    ImGui::DockBuilderDockWindow("Bake Parameters###params", right);
    ImGui::DockBuilderDockWindow("Viewport###viewport",       center);
    ImGui::DockBuilderDockWindow("UDIM Layout###udim",        bottom);   // tabbed with log
    ImGui::DockBuilderDockWindow("Output Log###log",          bottom);

    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center))
        n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;               // viewport has no tab strip
    ImGui::DockBuilderFinish(ds);
}

inline float StatusBarHeight() { return ImGui::GetFrameHeight() + 6.f; }
inline float ToolbarHeight()   { return ImGui::GetFrameHeight() + 18.f; }

// Draws menu bar, top toolbar, dockspace host, and bottom status bar.
inline void BeginFrameHost(ShellState& st) {
    // 1) main menu
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Low-Poly...", "Ctrl+O")) st.openLowPoly = true;
            if (ImGui::MenuItem("Open High-Poly...", "Ctrl+Shift+O")) st.openHighPoly = true;
            if (ImGui::MenuItem("Open Cage...", nullptr)) st.openCage = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) st.exitApp = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Outliner", nullptr, &st.showOutliner);
            ImGui::MenuItem("Output Log", nullptr, &st.showLog);
            ImGui::MenuItem("UDIM Layout", nullptr, &st.showUdim);
            ImGui::Separator();
            if (ImGui::BeginMenu("Theme")) {
                if (ImGui::MenuItem("Marmoset Ember (Warm Charcoal)", nullptr, gCurrentTheme == UITheme::MarmosetEmber)) {
                    gCurrentTheme = UITheme::MarmosetEmber;
                    ApplyTheme();
                }
                if (ImGui::MenuItem("Cyber Teal", nullptr, gCurrentTheme == UITheme::CyberTeal)) {
                    gCurrentTheme = UITheme::CyberTeal;
                    ApplyTheme();
                }
                if (ImGui::MenuItem("Studio Slate (Neutral)", nullptr, gCurrentTheme == UITheme::StudioSlate)) {
                    gCurrentTheme = UITheme::StudioSlate;
                    ApplyTheme();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) st.resetLayout = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) { ImGui::MenuItem("About"); ImGui::EndMenu(); }
        ImGui::EndMainMenuBar();
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float tb = ToolbarHeight(), sb = StatusBarHeight();

    // 2) top toolbar (project / mode info + quick actions)
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, tb));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 9));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col::Bg());
    ImGui::Begin("##toolbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    if (gFontBold) ImGui::PushFont(gFontBold);
    ImGui::TextUnformatted("ZBrush UDIM Baker");
    if (gFontBold) ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextDisabled("  |  Embree 3  *  D3D11");
    ImGui::End();
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);

    // 3) dockspace host window
    ImVec2 pos(vp->WorkPos.x, vp->WorkPos.y + tb);
    ImVec2 size(vp->WorkSize.x, vp->WorkSize.y - tb - sb);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col::Bg());
    ImGui::Begin("##dockhost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor(); ImGui::PopStyleVar(3);

    ImGuiID ds = ImGui::GetID("BakerDockSpace");
    if (ImGui::DockBuilderGetNode(ds) == nullptr || st.resetLayout) {
        BuildDefaultLayout(ds, size);
        st.resetLayout = false;
    }
    ImGui::DockSpace(ds, ImVec2(0, 0), ImGuiDockNodeFlags_None);
    ImGui::End();

    // 4) status bar
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - sb));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, sb));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 3));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col::Bg());
    ImGui::Begin("##statusbar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleColor(ImGuiCol_Text, st.statusColor);
    ImGui::Text("*"); ImGui::PopStyleColor();
    ImGui::SameLine(); ImGui::TextUnformatted(st.statusText);
    char fps[32]; snprintf(fps, sizeof fps, "%.0f FPS", ImGui::GetIO().Framerate);
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fps).x - 14);
    ImGui::TextDisabled("%s", fps);
    ImGui::End();
    ImGui::PopStyleColor(); ImGui::PopStyleVar(2);
}
inline void EndFrameHost() {}

} // namespace BakerUI
