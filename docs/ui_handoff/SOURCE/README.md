# UI Source Code & Framework Technical Overview

## 1. Dear ImGui Configuration
- **Version**: `1.93.1 WIP` (Numeric define: `IMGUI_VERSION_NUM 19301`).
- **Branch**: `docking` branch (explicitly uses `IMGUI_HAS_VIEWPORT` and `IMGUI_HAS_DOCK`).
- **Backend Bindings**:
  - **Renderer Backend**: DirectX 11 via standard `imgui_impl_dx11.cpp` and `imgui_impl_dx11.h`.
  - **Platform Backend**: Win32 via standard `imgui_impl_win32.cpp` and `imgui_impl_win32.h`.
- **Config Flags**:
  - `ImGuiConfigFlags_NavEnableKeyboard`: Enabled.
  - `ImGuiConfigFlags_DockingEnable`: Enabled.
  - `ImGuiConfigFlags_ViewportsEnable`: Not active by default to maintain self-contained companion windowing.

---

## 2. Directory Structure of Copied Files
All source files that invoke `ImGui::` or manage viewport rendering are mirrored here:
- `src/app/BakerUI.h`: Master UI design system, color palettes, themes, custom widgets, and docking host shell.
- `src/app/Main.cpp`: Companion application entry point (launched via ZBrush bridge), Win32 window creation, D3D11 initialization, and render loop.
- `src/app/StandaloneMain.cpp`: Standalone application entry point (desktop version), Win32 setup, and argument routing.
- `src/app/UIManager.h` & `src/app/UIManager.cpp`: Full UI manager for the ZBrush companion tool (Outliner, Viewport, Ingestion, Parameters, UDIM grid, Log, and Modal).
- `src/app/StandaloneUIManager.h` & `src/app/StandaloneUIManager.cpp`: Full UI manager for the standalone executable.
- `src/app/ViewportRenderer.h` & `src/app/ViewportRenderer.cpp`: DirectX 11 viewport subsystem, camera controller, ground grid shader, mesh rasterization, and texture generation for ImGui display.

---

## 3. Theme & Styling System
The application visual identity is implemented in `BakerUI.h`.
The default visual style is inspired by modern 3D DCC tools (Blender 4.x / Marmoset Toolbag 4).

### Supported Themes
1. **Blender Dark (Default)**:
   - Window Background: Deep Charcoal (`#1D1D1D`)
   - Panel Background: Neutral Gray (`#282828`)
   - Elevated Insets / Cards: (`#323232`)
   - Accent Primary: Blender Signature Warm Orange (`#E87D0D`)
   - Accent Hover: Bright Orange (`#F68E28`)
   - Accent Active: Deep Amber (`#B85F00`)
   - Crisp Border: Slate (`#1A1A1A`)
2. **Marmoset Ember**:
   - Window Background: Near Black (`#111317`)
   - Panel Background: Obsidian Slate (`#181B22`)
   - Accent: Warm Fire Orange (`#FF7A18`)
3. **Cyber Teal**:
   - Window Background: Deep Teal Carbon (`#0E1316`)
   - Panel Background: Cold Steel (`#141B20`)
   - Accent: Bright Cyan / Teal (`#00D2B4`)
4. **Studio Slate**:
   - Window Background: Clean Dark Blue-Gray (`#131519`)
   - Panel Background: Studio Neutral (`#191C22`)
   - Accent: CAD Blue (`#4E92F9`)

---

## 4. Typography & Font Hierarchy
Fonts are loaded dynamically in `BakerUI::LoadFonts(io, dpi)`:
- **Base UI Font**:
  - Path: `assets/fonts/Inter-Regular.ttf` (Size: `14px * dpi`).
  - Fallback 1: Windows native `C:/Windows/Fonts/segoeui.ttf`.
  - Fallback 2: ImGui default embedded ProggyClean.
- **Bold UI Font (`gFontBold`)**:
  - Path: `assets/fonts/Inter-SemiBold.ttf` (Size: `14px * dpi`).
  - Fallback 1: Windows native `C:/Windows/Fonts/segoeuib.ttf`.
  - Fallback 2: Base UI font.
- **Monospace Code/Log Font (`gFontMono`)**:
  - Path: `assets/fonts/JetBrainsMono-Regular.ttf` (Size: `13.3px * dpi`).
  - Fallback 1: Windows native `C:/Windows/Fonts/consola.ttf`.
  - Fallback 2: Base UI font.
- **Icon Font**:
  - No external icon font file is required.
  - Symbols are rendered using standard Unicode characters and custom vector shapes drawn directly to `ImDrawList` (e.g. status pills, chips, indicators).

---

## 5. DPI & Multi-Scale Handling
- High-DPI scale factor is queried via Win32 API (`GetDpiForWindow` / `GetDpiForSystem`).
- Font sizes are multiplied by the DPI ratio before compilation into the font atlas.
- `ImGuiStyle::ScaleAllSizes(dpi)` scales frames, paddings, scrollbars, and borders.
- Offscreen 3D viewport buffers render to the exact pixel dimensions of the central ImGui window content region, ensuring 1:1 crisp display on 4K / Retina displays.

---

## 6. Viewport Integration Architecture
The 3D interactive viewport is decoupled from ImGui drawing loops:
1. `ViewportRenderer` allocates an offscreen D3D11 render target texture (`ID3D11Texture2D`) and Shader Resource View (`ID3D11ShaderResourceView*`).
2. When the user resizes the `Viewport###viewport` ImGui panel, `ViewportRenderer::Resize(width, height)` re-allocates the texture to prevent blurriness.
3. In `UIManager::DrawViewportPanel()`, the texture pointer is rendered into the layout via `ImGui::Image((ImTextureID)m_viewportRenderer.GetShaderResourceView(), availSize)`.
4. Mouse input events (orbit, pan, scroll zoom) are captured only when `ImGui::IsItemHovered()` is true, preventing camera movements when interacting with floating overlay controls.
