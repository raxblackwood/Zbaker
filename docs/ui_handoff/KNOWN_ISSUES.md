# Companion Baker UI Known Layout Issues & Glitches

This document catalogs all observed UI layout glitches, hard-coded pixel sizing, overlapping element collisions, and spacing inconsistencies identified during the architectural review.

---

## 1. Hard-Coded Pixel Sizes & High-DPI Scaling Defects

### A. Floating Bake Bar Pixel Hard-Coding (`BakerUI.h`)
- **Code Location**: `BakerUI::FloatingBakeBar()` (Lines 354-405).
- **Issue**:
  - The bar height is hard-coded to `44.0f` pixels (`float barHeight = 44.0f;`).
  - The bake button width and height are hard-coded to `160.0f x 32.0f` pixels (`float btnW = 160.0f; float btnH = 32.0f;`).
- **Consequence on 4K / High-DPI Displays**:
  - When Windows display scaling is set to 150% or 200%, the font size scales up to 21px-28px, but the button and bar heights remain static at 32px and 44px.
  - This results in severe text clipping where `"START BAKE"` and tile count labels overflow outside the bar borders.
- **Recommended Redesign**:
  - Scale all floating bar dimensions proportionally using `ImGui::GetFontSize()` or `ImGuiStyle::ScaleAllSizes(dpi)`.

### B. Property Table Column Ratio Constraint (`BakerUI.h`)
- **Code Location**: `BakerUI::BeginProps()` (Lines 266-272).
- **Issue**:
  - Column split ratios are hard-coded to `0.42f` (42% label) and `0.58f` (58% control).
- **Consequence in Narrow Docks**:
  - When the right parameter panel is docked into a narrow column (~280px-320px width), the label column receives only 115px-130px.
  - Long technical labels such as `"Displacement / Height (32f)"`, `"Material ID (Polygroups)"`, or `"Ambient Occlusion (AO)"` truncate or collide directly with the checkbox/input control.
- **Recommended Redesign**:
  - Use `ImGuiTableColumnFlags_WidthFixed` for control columns or implement adaptive label wrapping / tooltips.

### C. Hard-Coded Browse Button Widths (`UIManager.cpp`)
- **Code Location**: `UIManager.cpp` (Line 1309).
- **Issue**:
  - File picker buttons are hard-coded to `ImVec2(80, 0)`: `ImGui::Button("Browse...", ImVec2(80, 0))`.
- **Consequence**:
  - On larger font sizes or translated interfaces, `"Browse..."` clips.

---

## 2. Overlapping Elements & Viewport Collisions

### A. Floating Bake Bar Colliding with Top Overlay Toolbar
- **Code Location**: `UIManager::DrawViewportPanel()`.
- **Issue**:
  - The top overlay toolbar (`Wireframe`, `Cage`, `Normals`, `MatCap`, `Reset Cam`) is pinned to the top of the viewport canvas.
  - The `FloatingBakeBar` is anchored `16.0f` pixels above the bottom edge of the viewport canvas.
- **Consequence**:
  - If the user resizes the bottom dock panel (`UDIM Layout` / `Output Log`) upward such that the 3D viewport height drops below ~220px, the floating bake bar renders directly on top of the overlay chips.
  - Both toolbars intercept mouse clicks simultaneously, causing accidental bake triggers when attempting to toggle wireframe.
- **Recommended Redesign**:
  - Enforce a minimum viewport height constraint or hide the floating bake bar when vertical viewport space is under 260px (relying on the in-panel bake button instead).

---

## 3. Docking Hierarchy & Tab Bar Anomalies

### A. Central Viewport `NoTabBar` Flag Loss on Drag
- **Code Location**: `BakerUI::BuildDefaultLayout()` (Lines 470-472).
- **Issue**:
  - `ImGuiDockNodeFlags_NoTabBar` is applied to the central node during initialization.
- **Consequence**:
  - If a user accidentally docks an auxiliary window (e.g. `Scene Outliner`) into the central viewport node and subsequently undocks it, Dear ImGui creates a permanent tab bar strip across the top of the 3D viewport.
  - The tab bar cannot be removed without executing `View -> Reset Layout`.
- **Recommended Redesign**:
  - Add `ImGuiDockNodeFlags_NoDockingOverMe` to the central viewport dock node to permanently prevent foreign panels from docking into the 3D canvas.

### B. Conflicting `.ini` Filenames Between Targets
- **Code Location**:
  - `Main.cpp`: `io.IniFilename = "baker_layout.ini"`.
  - `StandaloneMain.cpp`: `io.IniFilename = "standalone_baker_layout.ini"`.
- **Issue**:
  - Standalone mode and companion mode maintain separate layout configuration files.
  - Custom panel arrangements made in the standalone tool do not carry over when launched via ZBrush.

---

## 4. Spacing & Visual Inconsistencies

### A. Nested Padding Inside Ingestion Cards
- **Code Location**: `BakerUI::BeginCard()` and `UIManager.cpp` (Lines 790-880).
- **Issue**:
  - `BeginCard()` applies child window padding of `ImVec2(10, 8)`.
  - Property tables inside cards invoke `BeginProps()`, which introduces table cell padding of `ImVec2(6, 4)`.
- **Consequence**:
  - Content within cards is indented by 16px-20px, creating cramped input fields compared to the un-carded Bake Parameters panel.

### B. Inconsistent Section Header Margins
- **Code Location**: `BakerUI::SectionLabel()`.
- **Issue**:
  - Section headers include hard-coded top dummy padding of `4.0f` px and bottom dummy padding of `h + 4.0f` px.
  - Some panels insert an extra `ImGui::Spacing()` before `SectionLabel()`, while others do not, resulting in irregular vertical rhythm throughout the sidebar.

---

## 5. 2D UDIM Tile Grid Aspect Ratio Distortion

### A. Non-Square UDIM Tile Projection
- **Code Location**: `UIManager::DrawUDIMPanel()`.
- **Issue**:
  - The 2D UV tile grid computes tile boundaries by dividing the available window width and height by the row/column count without locking a 1:1 aspect ratio.
- **Consequence**:
  - On widescreen displays, square UV tiles (`1001`, `1002`) stretch into wide horizontal rectangles.
  - UV polygon wireframes appear horizontally distorted.
- **Recommended Redesign**:
  - Enforce square tile aspect ratio (`min(availWidth / cols, availHeight / rows)`) with centered canvas panning.
