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
    BlenderDark = 0,     // Neutral Dark Gray Blender Theme
    StudioSlateTeal = 1,
    MarmosetEmber = 2,
    CyberTeal = 3,
    StudioSlate = 4
};

inline UITheme gCurrentTheme = UITheme::BlenderDark;

// ---------------------------------------------------------------- palette
inline ImVec4 Hex(unsigned rgb, float a = 1.f) {
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
namespace Col {
    inline ImVec4 Bg() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x161616); // Dark root/window background
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0x111317);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x0E1316);
        return Hex(0x141414);
    }
    inline ImVec4 Panel() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x1E1E1E); // Dark neutral panel background (RGB 30,30,30)
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0x181B22);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x141B20);
        return Hex(0x1E1E1E);
    }
    inline ImVec4 PanelHi() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x242424); // Card interior (RGB 36,36,36)
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0x20242D);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x1A232A);
        return Hex(0x242424);
    }
    inline ImVec4 Border() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x282828); // Subtle border
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0x2E3442);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x25353F);
        return Hex(0x282828);
    }
    inline ImVec4 Text() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0xE4E4E4); // Crisp readable light silver
        return Hex(0xEDEDF2);
    }
    inline ImVec4 TextDim() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x909090); // Neutral secondary text
        return Hex(0x8D94A5);
    }
    inline ImVec4 Accent() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x4CC3A6); // High-contrast green button center
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0xFF7A18);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x00D2B4);
        return Hex(0x4E92F9);
    }
    inline ImVec4 AccentHi() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x5FD2B6); // Hover mint
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0xFF9438);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x38F0D6);
        return Hex(0x73ABFF);
    }
    inline ImVec4 AccentLo() {
        if (gCurrentTheme == UITheme::BlenderDark || gCurrentTheme == UITheme::StudioSlateTeal) return Hex(0x3EB496); // Pressed mint
        if (gCurrentTheme == UITheme::MarmosetEmber)   return Hex(0xD96206);
        if (gCurrentTheme == UITheme::CyberTeal)       return Hex(0x00A890);
        return Hex(0x3472D9);
    }
    inline ImVec4 AccentText() {
        return Hex(0x0C1412); // High-contrast dark text on vibrant button
    }
    inline ImVec4 Ok() {
        return Hex(0x4CC3A6);
    }
    inline ImVec4 Warn() {
        return Hex(0xF59E0B);
    }
    inline ImVec4 Err() {
        return Hex(0xEF4444);
    }
}

// ---------------------------------------------------------------- fonts
// Drop Inter-Regular.ttf / Inter-SemiBold.ttf / JetBrainsMono-Regular.ttf in assets/fonts.
inline ImFont* gFontBold = nullptr;
inline ImFont* gFontMono = nullptr;
inline void LoadFonts(ImGuiIO& io, float dpi = 1.f) {
    const float px = 14.f * dpi;
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
    s.WindowPadding     = ImVec2(10, 10);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 6);
    s.ItemInnerSpacing  = ImVec2(6, 4);
    s.CellPadding       = ImVec2(6, 4);
    s.ScrollbarSize     = 10;
    s.GrabMinSize       = 10;
    s.WindowRounding    = 6;                // Smooth modern studio rounding
    s.ChildRounding     = 6;
    s.FrameRounding     = 5;
    s.PopupRounding     = 6;
    s.ScrollbarRounding = 4;
    s.GrabRounding      = 4;
    s.TabRounding       = 5;
    s.WindowBorderSize  = 1;
    s.FrameBorderSize   = 1;
    s.TabBarBorderSize  = 1;
    s.DockingSeparatorSize = 2;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ScaleAllSizes(dpi);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = Col::Text();
    c[ImGuiCol_TextDisabled]         = Col::TextDim();
    c[ImGuiCol_WindowBg]             = Col::Panel();         // Neutral dark panel: RGB(30, 30, 30) Hex(0x1E1E1E)
    c[ImGuiCol_ChildBg]              = Col::PanelHi();       // Card body: RGB(36, 36, 36) Hex(0x242424)
    c[ImGuiCol_PopupBg]              = Hex(0x1E1E1E, 0.98f);
    c[ImGuiCol_Border]               = Col::Border();        // Subtle border: Hex(0x282828)
    c[ImGuiCol_BorderShadow]         = Hex(0, 0);
    c[ImGuiCol_FrameBg]              = Hex(0x181818);        // Neutral dark input field (Blender style)
    c[ImGuiCol_FrameBgHovered]       = Hex(0x262626);
    c[ImGuiCol_FrameBgActive]        = Hex(0x2E2E2E);
    c[ImGuiCol_TitleBg]              = Hex(0x161616);
    c[ImGuiCol_TitleBgActive]        = Hex(0x1A1A1A);
    c[ImGuiCol_TitleBgCollapsed]     = Hex(0x161616);
    c[ImGuiCol_MenuBarBg]            = Hex(0x181818);        // Exact top bar: RGB(24, 24, 24)
    c[ImGuiCol_ScrollbarBg]          = Hex(0, 0);
    c[ImGuiCol_ScrollbarGrab]        = Hex(0x2C2C2C);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x3A3A3A);
    c[ImGuiCol_ScrollbarGrabActive]  = Col::Accent();
    c[ImGuiCol_CheckMark]            = Col::Accent();
    c[ImGuiCol_SliderGrab]           = Col::Accent();
    c[ImGuiCol_SliderGrabActive]     = Col::AccentHi();
    c[ImGuiCol_Button]               = Hex(0x282828);        // Neutral charcoal button
    c[ImGuiCol_ButtonHovered]        = Hex(0x333333);
    c[ImGuiCol_ButtonActive]         = Hex(0x202020);
    c[ImGuiCol_Header]               = Hex(0x2A2A2A);
    c[ImGuiCol_HeaderHovered]        = Hex(0x363636);
    c[ImGuiCol_HeaderActive]         = Hex(0x222222);
    c[ImGuiCol_Separator]            = Col::Border();
    c[ImGuiCol_SeparatorHovered]     = Col::Accent();
    c[ImGuiCol_SeparatorActive]      = Col::AccentHi();
    c[ImGuiCol_ResizeGrip]           = Hex(0, 0);
    c[ImGuiCol_ResizeGripHovered]    = Col::Accent();
    c[ImGuiCol_ResizeGripActive]     = Col::AccentHi();
    c[ImGuiCol_Tab]                  = Hex(0x161616);
    c[ImGuiCol_TabHovered]           = Hex(0x262626);
    c[ImGuiCol_TabSelected]          = Hex(0x1E1E1E);        // Active tab merges seamlessly with panel
    c[ImGuiCol_TabDimmed]            = Hex(0x161616);
    c[ImGuiCol_TabDimmedSelected]    = Hex(0x1E1E1E);
    c[ImGuiCol_DockingPreview]       = Hex(0x4CC3A6, 0.40f);
    c[ImGuiCol_DockingEmptyBg]       = Col::Bg();
    c[ImGuiCol_TableHeaderBg]        = Hex(0x181818);
    c[ImGuiCol_TableBorderLight]     = Col::Border();
    c[ImGuiCol_TextSelectedBg]       = Hex(0x4CC3A6, 0.35f);
    c[ImGuiCol_PlotHistogram]        = Col::Accent();
}

// ---------------------------------------------------------------- widgets
// Blender-style Collapsible Section Card with rounded corners, disclosure arrow, 8 grip dots, and clean gap
inline bool BeginSectionCard(const char* label, const char* id, bool defaultOpen = false) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGuiID imguiId = ImGui::GetID(id);
    int openVal = storage->GetInt(imguiId, defaultOpen ? 1 : 0);
    bool isOpen = (openVal != 0);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFontSize() + 10.0f; // Blender header height (~24-26px)
    float rounding = 5.0f;

    // Invisible button to capture clicks over the header
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool isHovered = ImGui::IsItemHovered();
    bool isClicked = ImGui::IsItemClicked();

    if (isClicked) {
        isOpen = !isOpen;
        storage->SetInt(imguiId, isOpen ? 1 : 0);
    }

    ImVec2 headerMin = p;
    ImVec2 headerMax = ImVec2(p.x + w, p.y + h);

    // Blender dark header colors: closed 0x2C2C2C, open 0x303030, hovered 0x383838
    ImU32 headerBg = isHovered 
        ? ImGui::GetColorU32(Hex(0x383838)) 
        : (isOpen ? ImGui::GetColorU32(Hex(0x303030)) : ImGui::GetColorU32(Hex(0x2C2C2C)));
    ImU32 borderColor = ImGui::GetColorU32(Hex(0x222222));

    ImDrawFlags roundFlags = isOpen ? ImDrawFlags_RoundCornersTop : ImDrawFlags_RoundCornersAll;
    dl->AddRectFilled(headerMin, headerMax, headerBg, rounding, roundFlags);
    dl->AddRect(headerMin, headerMax, borderColor, rounding, roundFlags, 1.0f);

    // Blender disclosure triangle
    float arrowCenterX = p.x + 13.0f;
    float arrowCenterY = p.y + h * 0.5f;
    ImU32 arrowCol = ImGui::GetColorU32(Hex(0xCCCCCC));
    if (isOpen) {
        // Pointing down: (x-4, y-2), (x+4, y-2), (x, y+3)
        dl->AddTriangleFilled(
            ImVec2(arrowCenterX - 4.0f, arrowCenterY - 2.0f),
            ImVec2(arrowCenterX + 4.0f, arrowCenterY - 2.0f),
            ImVec2(arrowCenterX, arrowCenterY + 3.0f),
            arrowCol);
    } else {
        // Pointing right: (x-2, y-4), (x-2, y+4), (x+3, y)
        dl->AddTriangleFilled(
            ImVec2(arrowCenterX - 2.0f, arrowCenterY - 4.0f),
            ImVec2(arrowCenterX - 2.0f, arrowCenterY + 4.0f),
            ImVec2(arrowCenterX + 3.0f, arrowCenterY),
            arrowCol);
    }

    // Section Title
    if (gFontBold) ImGui::PushFont(gFontBold);
    dl->AddText(ImVec2(p.x + 25.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(Hex(0xE4E4E4)), label);
    if (gFontBold) ImGui::PopFont();

    // Blender 8-dot grip icon on far right (4 columns x 2 rows)
    float gripRight = p.x + w - 10.0f;
    float dotY1 = p.y + h * 0.5f - 3.0f;
    float dotY2 = p.y + h * 0.5f + 3.0f;
    ImU32 dotCol = ImGui::GetColorU32(Hex(0x606060, 0.85f));
    for (int i = 0; i < 4; ++i) {
        float dotX = gripRight - (3 - i) * 4.0f;
        dl->AddCircleFilled(ImVec2(dotX, dotY1), 1.0f, dotCol);
        dl->AddCircleFilled(ImVec2(dotX, dotY2), 1.0f, dotCol);
    }

    if (!isOpen) {
        // Gap between closed cards (Blender style)
        ImGui::Dummy(ImVec2(0, 4.0f));
        return false;
    }

    // Line separator between header and body
    dl->AddLine(ImVec2(headerMin.x, headerMax.y), ImVec2(headerMax.x, headerMax.y), borderColor);

    // When open, start child body container with rounded bottom corners
    std::string childId = std::string(id) + "_body";
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, rounding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_Border, borderColor);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Hex(0x222222)); // Dark card body interior
    bool childOpen = ImGui::BeginChild(childId.c_str(), ImVec2(0, 0),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    return childOpen;
}

inline void EndSectionCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    // Gap after open card
    ImGui::Dummy(ImVec2(0, 4.0f));
}

// Static section header matching Blender dark design
inline void SectionLabel(const char* text) {
    ImGui::Spacing();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = ImGui::GetFontSize() + 8.0f;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Hex(0x2A2A2A)), 4.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ImGui::GetColorU32(Hex(0x222222)), 4.0f, 0, 1.0f);

    if (gFontBold) ImGui::PushFont(gFontBold);
    dl->AddText(ImVec2(p.x + 8.0f, p.y + (h - ImGui::GetFontSize()) * 0.5f), ImGui::GetColorU32(Hex(0xE0E0E0)), text);
    if (gFontBold) ImGui::PopFont();

    // Grip indicator dots
    float gripRight = p.x + w - 10.0f;
    float dotY1 = p.y + h * 0.5f - 3.0f;
    float dotY2 = p.y + h * 0.5f + 3.0f;
    ImU32 dotCol = ImGui::GetColorU32(Hex(0x555555, 0.75f));
    for (int i = 0; i < 4; ++i) {
        float dotX = gripRight - (3 - i) * 4.0f;
        dl->AddCircleFilled(ImVec2(dotX, dotY1), 1.0f, dotCol);
        dl->AddCircleFilled(ImVec2(dotX, dotY2), 1.0f, dotCol);
    }

    ImGui::Dummy(ImVec2(0, h + 3.0f));
}

// Accent (primary) button, full width by default. High contrast dark text on vibrant mint teal.
inline bool PrimaryButton(const char* label, float w = -FLT_MIN, float h = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Col::Accent());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::AccentHi());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Col::AccentLo());
    ImGui::PushStyleColor(ImGuiCol_Text, Col::AccentText());
    if (gFontBold) ImGui::PushFont(gFontBold);
    bool r = ImGui::Button(label, ImVec2(w, h));
    if (gFontBold) ImGui::PopFont();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return r;
}

// Subtle full-width secondary button.
inline bool SecondaryButton(const char* label, float w = -FLT_MIN, float h = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, Hex(0x262626));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x323232));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Hex(0x1E1E1E));
    ImGui::PushStyleColor(ImGuiCol_Border, Hex(0x282828));
    ImGui::PushStyleColor(ImGuiCol_Text, Hex(0xE2E4E8));
    bool r = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar();
    return r;
}

// Pill-shaped toggle (used on the viewport overlay instead of checkboxes).
inline bool ToggleChip(const char* label, bool* v) {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 4));
    if (*v) {
        ImGui::PushStyleColor(ImGuiCol_Button,        Hex(0x2A2A2A));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x363636));
        ImGui::PushStyleColor(ImGuiCol_Border,         Col::Accent());
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::Accent());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        Hex(0x181818, 0.90f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x24282F, 0.95f));
        ImGui::PushStyleColor(ImGuiCol_Border,         Hex(0x2A2E35));
        ImGui::PushStyleColor(ImGuiCol_Text,          Col::TextDim());
    }
    bool clicked = ImGui::Button(label);
    if (clicked) *v = !*v;
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    return clicked;
}

// Status dot indicator matching reference design
inline void StatusDot(ImVec4 col, bool filled = true, float radius = 3.5f) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lineH = ImGui::GetFrameHeight();
    ImVec2 center(p.x + radius + 2.0f, p.y + lineH * 0.5f);
    if (filled) {
        dl->AddCircleFilled(center, radius, ImGui::GetColorU32(col), 16);
    } else {
        dl->AddCircle(center, radius, ImGui::GetColorU32(col), 16, 1.5f);
    }
    ImGui::Dummy(ImVec2(radius * 2.0f + 6.0f, lineH));
}

// Colored status pill ("BVH ready", "No low-poly").
inline void StatusPill(const char* text, ImVec4 color) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(10, 4);
    ImVec2 sz(ts.x + pad.x * 2 + 12, ts.y + pad.y * 2);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, 0.16f)), 4.f);
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
    ImGui::PushStyleColor(ImGuiCol_Text, Col::Text()); // High contrast readable label
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
    bool r = ImGui::Checkbox(id, v);
    ImGui::SameLine();
    StatusDot(*v ? Col::Ok() : Hex(0x475569), *v, 3.0f);
    return r;
}

// Rounded "card" container; use for grouping.
inline bool BeginCard(const char* id, float h = 0) {
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_Border, Col::Border());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col::PanelHi());
    return ImGui::BeginChild(id, ImVec2(0, h),
        ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
}
inline void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

// Bake button that morphs into a progress bar while running.
// progress < 0 -> idle. Returns true on click (idle).
inline bool BakeButton(float progress, const char* idleLabel = "START BAKE") {
    const float h = 40.f * (ImGui::GetFontSize() / 15.f);
    ImVec2 size(ImGui::GetContentRegionAvail().x, h);
    if (progress < 0.f) {
        if (gFontBold) ImGui::PushFont(gFontBold);
        bool r = PrimaryButton(idleLabel, size.x, size.y);
        if (gFontBold) ImGui::PopFont();
        return r;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Col::PanelHi()), 4.f);
    dl->AddRectFilled(p, ImVec2(p.x + size.x * progress, p.y + size.y), ImGui::GetColorU32(Col::AccentLo()), 4.f);
    char buf[48]; snprintf(buf, sizeof buf, "Baking...  %d%%", (int)(progress * 100));
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(Hex(0xFFFFFF)), buf);
    ImGui::Dummy(size);
    return false;
}

// Floating Bake Bar at the bottom of the 3D viewport canvas
inline bool FloatingBakeBar(
    const ImVec2& vpOrigin,
    const ImVec2& vpSize,
    bool canBake,
    bool isBaking,
    float progress,
    const char* statusText,
    int udimCount,
    int resX,
    int resY
) {
    float barWidth = (std::min)(540.0f, vpSize.x - 32.0f);
    if (barWidth < 260.0f) barWidth = vpSize.x - 16.0f;
    float barHeight = 44.0f;
    float barX = vpOrigin.x + (vpSize.x - barWidth) * 0.5f;
    float barY = vpOrigin.y + vpSize.y - barHeight - 16.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 minP(barX, barY);
    ImVec2 maxP(barX + barWidth, barY + barHeight);

    // Drop shadow
    dl->AddRectFilled(ImVec2(minP.x + 2, minP.y + 3), ImVec2(maxP.x + 2, maxP.y + 4),
                      ImGui::GetColorU32(Hex(0x000000, 0.55f)), 6.0f);
    // Dark neutral panel background
    dl->AddRectFilled(minP, maxP, ImGui::GetColorU32(Hex(0x1E1E1E, 0.96f)), 6.0f);
    // Subtle border
    dl->AddRect(minP, maxP, ImGui::GetColorU32(Hex(0x282828, 0.95f)), 6.0f, 0, 1.2f);

    bool clicked = false;

    if (isBaking) {
        float progClamped = (progress < 0.03f) ? 0.03f : (progress > 1.0f ? 1.0f : progress);
        ImVec2 progMax(minP.x + barWidth * progClamped, maxP.y);
        // Mint teal progress bar fill!
        dl->AddRectFilled(minP, progMax, ImGui::GetColorU32(Hex(0x4EE2B6, 0.90f)), 6.0f);

        char textBuf[128];
        snprintf(textBuf, sizeof(textBuf), "%s (%.0f%%)", statusText ? statusText : "Baking...", progress * 100.0f);
        ImVec2 ts = ImGui::CalcTextSize(textBuf);
        dl->AddText(ImVec2(minP.x + (barWidth - ts.x) * 0.5f, minP.y + (barHeight - ts.y) * 0.5f),
                    ImGui::GetColorU32(Hex(0x0C1412)), textBuf);
    } else {
        // Left info section: UDIMs & Resolution
        char infoBuf[128];
        if (canBake) {
            snprintf(infoBuf, sizeof(infoBuf), "%d UDIM Tile%s  |  %dx%d",
                     udimCount, udimCount == 1 ? "" : "s", resX, resY);
        } else {
            snprintf(infoBuf, sizeof(infoBuf), "Load meshes in Mesh Ingestion tab");
        }

        ImVec2 infoTs = ImGui::CalcTextSize(infoBuf);
        float infoY = minP.y + (barHeight - infoTs.y) * 0.5f;
        dl->AddText(ImVec2(minP.x + 18.0f, infoY), ImGui::GetColorU32(canBake ? Col::Text() : Col::Warn()), infoBuf);

        // Right side: Mint Teal Bake Button with dark text
        float btnW = 160.0f;
        float btnH = 32.0f;
        float btnX = maxP.x - btnW - 8.0f;
        float btnY = minP.y + (barHeight - btnH) * 0.5f;

        ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));
        if (!canBake) {
            ImGui::BeginDisabled();
        }

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, canBake ? Hex(0x4EE2B6) : Hex(0x202834));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, canBake ? Hex(0x63E8BF) : Hex(0x283342));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, canBake ? Hex(0x3BC79D) : Hex(0x19202B));
        ImGui::PushStyleColor(ImGuiCol_Text, canBake ? Hex(0x0C1412) : Hex(0x768699));

        if (gFontBold) ImGui::PushFont(gFontBold);
        if (ImGui::Button(canBake ? "START BAKE##float_bake" : "BAKE (NO MESH)##float_bake", ImVec2(btnW, btnH))) {
            clicked = true;
        }
        if (gFontBold) ImGui::PopFont();

        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();

        if (!canBake) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Load both High-Poly sculpt and Low-Poly UV mesh before baking.");
            }
        }
    }

    return clicked;
}

// ---------------------------------------------------------------- app shell
struct ShellState {
    enum class AppTab {
        Baking = 0,
        UVCutAndPack = 1
    };
    AppTab      activeTab = AppTab::Baking;
    const char* statusText = "Ready";
    ImVec4      statusColor = Col::Ok();
    float       bakeProgress = -1.f;     // -1 idle
    bool        resetLayout = false;     // View > Reset Layout
    bool        showLog = true, showUdim = true, showOutliner = true, showUVPanel = true;
    bool        showMeshPanel = true, showBakeParams = true;
    bool        openLowPoly = false;
    bool        openHighPoly = false;
    bool        openCage = false;
    bool        clearScene = false;
    bool        loadZBrushCache = false;
    bool        exitApp = false;
    // Mode-specific menu action triggers:
    bool        triggerBake = false;
    bool        clearNormalPreview = false;
    bool        triggerUndo = false;
    bool        triggerRedo = false;
    bool        selectEdgeLoop = false;
    bool        selectEdgeRing = false;
    bool        triggerAutoSeams = false;
    bool        triggerCutSeams = false;
    bool        triggerSewSeams = false;
    bool        triggerClearSeams = false;
    bool        triggerUnwrap = false;
    bool        triggerPack = false;
    bool        exportLowPoly = false;
};

inline void BuildDefaultLayout(ImGuiID ds, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(ds);
    ImGui::DockBuilderAddNode(ds, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(ds, size);

    ImGuiID center = ds, right, bottom;
    right  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.26f, nullptr, &center);

    ImGuiID rightTop = right, rightBottom;
    rightBottom = ImGui::DockBuilderSplitNode(rightTop, ImGuiDir_Down, 0.58f, nullptr, &rightTop);

    ImGui::DockBuilderDockWindow("Scene Outliner###outliner", rightTop);
    ImGui::DockBuilderDockWindow("Bake Parameters###params",  rightBottom);
    ImGui::DockBuilderDockWindow("UV Tool###uvpanel",         rightBottom);
    ImGui::DockBuilderDockWindow("Mesh Ingestion###ingest",   rightBottom);
    ImGui::DockBuilderDockWindow("Viewport###viewport",       center);
    ImGui::DockBuilderDockWindow("UDIM Layout###udim",        bottom);   // tabbed with log
    ImGui::DockBuilderDockWindow("Output Log###log",          bottom);

    if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(center))
        n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;               // viewport has no tab strip
    ImGui::DockBuilderFinish(ds);
}

inline float StatusBarHeight() { return ImGui::GetFrameHeight() + 6.f; }
inline float ToolbarHeight()   { return 0.0f; }

// Draws menu bar, top toolbar, dockspace host, and bottom status bar.
inline void BeginFrameHost(ShellState& st) {
    // 1) main menu
    if (ImGui::BeginMainMenuBar()) {
        bool isBaking = (st.activeTab == ShellState::AppTab::Baking);
        bool isUV = (st.activeTab == ShellState::AppTab::UVCutAndPack);

        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New Session (Clear Scene)", "Ctrl+N")) st.clearScene = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Open Low-Poly...", "Ctrl+O")) st.openLowPoly = true;
            if (isBaking) {
                if (ImGui::MenuItem("Open High-Poly...", "Ctrl+Shift+O")) st.openHighPoly = true;
                if (ImGui::MenuItem("Open Cage...", nullptr)) st.openCage = true;
            } else {
                if (ImGui::MenuItem("Export UV Low-Poly OBJ...", "Ctrl+E")) st.exportLowPoly = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Load ZBrush Cache", nullptr)) st.loadZBrushCache = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) st.exitApp = true;
            ImGui::EndMenu();
        }

        // Mode-specific middle menu groups
        if (isBaking) {
            if (ImGui::BeginMenu("Bake")) {
                if (ImGui::MenuItem("Start Bake", "F5")) st.triggerBake = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Baked Normal Preview", nullptr)) st.clearNormalPreview = true;
                ImGui::EndMenu();
            }
        } else {
            if (ImGui::BeginMenu("Edit")) {
                if (ImGui::MenuItem("Undo", "Ctrl+Z")) st.triggerUndo = true;
                if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z")) st.triggerRedo = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Select Edge Loop", "Alt+L")) st.selectEdgeLoop = true;
                if (ImGui::MenuItem("Select Edge Ring", "Alt+R")) st.selectEdgeRing = true;
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("UV")) {
                if (ImGui::MenuItem("Auto Seams by Angle", nullptr)) st.triggerAutoSeams = true;
                if (ImGui::MenuItem("Cut Selected Seams", "C")) st.triggerCutSeams = true;
                if (ImGui::MenuItem("Sew Selected Seams", "S")) st.triggerSewSeams = true;
                if (ImGui::MenuItem("Clear All Seams", nullptr)) st.triggerClearSeams = true;
                ImGui::Separator();
                if (ImGui::MenuItem("Conformal Unwrap", "Ctrl+U")) st.triggerUnwrap = true;
                if (ImGui::MenuItem("Pack UDIM Tiles", "Ctrl+P")) st.triggerPack = true;
                ImGui::EndMenu();
            }
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Scene Outliner", nullptr, &st.showOutliner);
            if (isBaking) {
                ImGui::MenuItem("Mesh Ingestion", nullptr, &st.showMeshPanel);
                ImGui::MenuItem("Bake Parameters", nullptr, &st.showBakeParams);
                ImGui::MenuItem("Output Log", nullptr, &st.showLog);
                ImGui::MenuItem("UDIM Layout", nullptr, &st.showUdim);
            } else {
                ImGui::MenuItem("UV Tool Panel", nullptr, &st.showUVPanel);
                ImGui::MenuItem("UDIM Layout", nullptr, &st.showUdim);
                ImGui::MenuItem("Output Log", nullptr, &st.showLog);
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Theme")) {
                if (ImGui::MenuItem("Blender Dark (Default)", nullptr, gCurrentTheme == UITheme::BlenderDark)) {
                    gCurrentTheme = UITheme::BlenderDark;
                    ApplyTheme();
                }
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

        ImGui::SameLine();
        ImGui::Dummy(ImVec2(18.0f, 0.0f));
        ImGui::SameLine();

        // Workspace mode tabs matching studio styling
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f, 4.0f));

        if (isBaking) {
            ImGui::PushStyleColor(ImGuiCol_Button, Col::Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Col::AccentHi());
            ImGui::PushStyleColor(ImGuiCol_Text, Col::AccentText());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, Hex(0x262626));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x323232));
            ImGui::PushStyleColor(ImGuiCol_Text, Col::TextDim());
        }
        if (gFontBold) ImGui::PushFont(gFontBold);
        if (ImGui::Button("Bake Project")) {
            st.activeTab = ShellState::AppTab::Baking;
        }
        if (gFontBold) ImGui::PopFont();
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 4.0f);

        if (isUV) {
            ImGui::PushStyleColor(ImGuiCol_Button, Hex(0x4CC3A6));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x5FD2B6));
            ImGui::PushStyleColor(ImGuiCol_Text, Hex(0x0C1412));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, Hex(0x262626));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0x323232));
            ImGui::PushStyleColor(ImGuiCol_Text, Col::TextDim());
        }
        if (gFontBold) ImGui::PushFont(gFontBold);
        if (ImGui::Button("UV Cut & Pack")) {
            st.activeTab = ShellState::AppTab::UVCutAndPack;
        }
        if (gFontBold) ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        const char* titleBadge = "ZBrush UDIM Baker  |  Embree 3  *  D3D11";
        float badgeW = ImGui::CalcTextSize(titleBadge).x + 24.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - badgeW);
        ImGui::TextDisabled("%s", titleBadge);

        ImGui::EndMainMenuBar();
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float sb = StatusBarHeight();

    // 2) dockspace host window (direct under menu bar)
    ImVec2 pos(vp->WorkPos.x, vp->WorkPos.y);
    ImVec2 size(vp->WorkSize.x, vp->WorkSize.y - sb);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
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

    // 3) status bar
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
