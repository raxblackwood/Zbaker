# Project Memory: ZBrush UDIM Baker

**Project Code Name**: `zbrush-udim-baker`  
**Root Repository**: `e:\CODES\zbrush plugins\baker`  
**Standalone Directory**: `E:\CODES\standalone_baker`  
**ZBrush Plugin Directory**: `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBakerData`  
**Last Updated**: `2026-10-24T19:30:00+05:30`  
**Status**: `Active | ZBrush Plugin Priority Focus | Standalone Paused | Production Ready`

---

## 1. Executive Summary & Purpose
Maxon ZBrush handles 100M+ polygon digital sculpts via an implicit-quad CPU rasterizer, but lacks native support for modern **UDIM multi-tile UV layouts** (e.g. 1001, 1002, 1003...).
Exporting 50M-100M polygon OBJs to external baking tools (Substance Painter, Marmoset Toolbag) creates prohibitive 10-30 GB files and halts production.

**ZBrush UDIM Baker** is an enterprise hardware-accelerated baking companion:
1. **Host ZPlugin**: A one-click button in ZBrush (`ZPlugin:UDIM Baker -> Launch UDIM Baker`).
2. **Native C-ABI Bridge**: `UDIMBakerBridge.dll` streams high-poly cache data non-blocking via `DETACHED_PROCESS`.
3. **Companion Viewport**: `UDIMBakerApp.exe` featuring a Direct3D 11 3D viewport, 2D UDIM canvas, and a Blender Dark docking interface.
4. **Hardware Ray-Tracing Core**: Intel Embree 3.13.5 multithreaded ray tracer with MikkTSpace tangent basis, bidirectional ray casting, closest-distance depth arbitration, and Jump Flooding dilation (JFA).

### 1.1 Development Focus Directive (MANDATORY)
Standalone application development is temporarily paused per user directive.
All ongoing engineering, debugging, and feature development is strictly focused on the ZBrush plugin.
Do not work on the standalone application until the user explicitly requests to resume standalone work.
Once the plugin workflow and architecture are completely finalized, necessary adaptations will be applied back to the standalone application.

---

## 2. Complete UI Design & Viewport Overview

### 2.1 Visual Design Identity
- **Visual Language**: Modeled on Blender 4.x/5.1 and Marmoset Toolbag 4.
- **Color Palette (`BakerUI.h`)**:
  - Background Canvas: Deep Slate Charcoal (`#1D1D1D`).
  - Panel Body: Neutral Dark Gray (`#282828`).
  - Cards & Inset Surfaces: (`#323232`).
  - Signature Accent: Blender Warm Orange (`#E87D0D`), Hover (`#F68E28`), Active (`#B85F00`).
  - Crisp Borders: Subtle Slate (`#1A1A1A`).
- **Selectable Themes**: `Blender Dark` (Default), `Marmoset Ember` (`#FF7A18`), `Cyber Teal` (`#00D2B4`), `Studio Slate` (`#4E92F9`).
- **Typography**: Inter (14px base, 14px SemiBold headings) with JetBrains Mono (13.3px monospace log).
- **Rounding**: Subtle 4px radius on cards, buttons, tabs, and frames.

### 2.2 Screen Layout & Docking Tree
The window uses Dear ImGui `1.93.1 WIP` (docking branch) with DirectX 11 + Win32 backends:
```text
+---------------------------------------------------------------------------------------------+
| Main Menu Bar (File, View, Theme, Help)                                                     |
+----------------------------------------------------+----------------------------------------+
|                                                    | Scene Outliner (ID: 0x05, 30% width)   |
| Viewport (ID: 0x03, Central Node, No Tab Bar)      | - High-poly sculpt, Low-poly, Cage     |
| - 3D Direct3D 11 Hardware Render Target            +----------------------------------------+
| - Top Overlay Toolbar (Wireframe, Cage, Normals...) | Bake Parameters (ID: 0x06, Tab 1)      |
| - Floating Bake Bar (Bottom Center)                | Mesh Ingestion  (ID: 0x06, Tab 2)      |
|                                                    |                                        |
+----------------------------------------------------+                                        |
| UDIM Layout (ID: 0x04, Tab 1, 26% height)          |                                        |
| Output Log  (ID: 0x04, Tab 2, 26% height)          |                                        |
+----------------------------------------------------+----------------------------------------+
| Status Bar (Status bullet, active message, live FPS monitor)                                |
+---------------------------------------------------------------------------------------------+
```

### 2.3 Window-by-Window Functions
1. **3D Viewport (`Viewport###viewport`)**:
   - Central D3D11 offscreen render target (`ID3D11ShaderResourceView*`) passed to `ImGui::Image`.
   - **Top Overlay Chips**: `Wireframe` (quad-preserving lines without diagonal splits), `Cage`, `Normals`, `High-Poly`, `Low-Poly`, `Grid`, `MatCap`, `Reset Cam`.
   - **Floating Bake Bar HUD**: Centered at bottom canvas. Shows tile count, resolution, and orange `"START BAKE"` button in idle state; morphs into animated progress bar during baking.
2. **Scene Outliner (`Scene Outliner###outliner`)**:
   - Tree hierarchy of scene assets with visibility eye icons and status pills (`"BVH Ready"`, `"Watertight"`).
3. **Mesh Ingestion (`Mesh Ingestion###ingest`)**:
   - Rounded cards for High-Poly sculpt, Low-Poly UV target, Auto-Alignment/Scale transforms, and Cage definition.
4. **Bake Parameters (`Bake Parameters###params`)**:
   - Projection heuristics: Resolution (`1024` to `8192`), AA (`1x` to `4x4`), Skew factor, Cage offset, Max ray length, Seam padding (32px), Discard Backface, Bidirectional rays.
   - Marmoset Output Map Suite: Tangent Normal (MikkTSpace), World Normal, Displacement (32f EXR), AO, Cavity, Curvature, Thickness, World Position, Material ID, Base Color, Diagnostic Map.
5. **UDIM Layout (`UDIM Layout###udim`)**:
   - 2D UV coordinate visualizer displaying tile boxes (`[1001]`, `[1002]`, etc.) with wireframe overlays and toggle selection buttons.
6. **Output Log (`Output Log###log`)**:
   - Real-time timestamped console for BVH build times, ray-tracing statistics, and file save locations.
7. **Status Bar (`##statusbar`)**:
   - Bottom status indicator bullet, descriptive status string, and live FPS readout.

### 2.4 UI State Machine
The UI transitions through 6 distinct operational states:
1. **Empty**: Cold start; ground grid rendered; floating bar disabled with prompt `"Load meshes in Mesh Ingestion tab"`.
2. **Ingesting**: Mesh files reading; Intel Embree BVH building on background worker threads; amber status pills active.
3. **Loaded**: Meshes in memory; watertight normals computed; floating bar enabled with bright orange button.
4. **Baking**: Scanline ray casting active across CPU cores; floating bar morphs into progress fill; modal dialog active.
5. **Results**: Maps saved to disk; viewport updates with normal map preview; green logs displayed.
6. **Error**: Missing UVs or read errors; red status pills and warning tooltips displayed.

### 2.5 Context-Aware Workspace Segregation
The interface strictly partitions tools, menus, panels, and floating HUD elements between two primary operational workspaces.

#### Shared Persistent Outliner
- **Scene Outliner (`Scene Outliner###outliner`)**: Docked into `rightTop` and persistent across both workspaces.
- In Bake Project mode, it displays the full scene hierarchy including Bake Groups, High-Poly sculpt, Low-Poly mesh, and Cage envelopes with visibility toggles and opacity controls.
- In UV Cut & Pack mode, it displays the active Low-Poly target mesh, detected UDIM tiles checklist, marked seams count, selected edges count, and reference sculpt toggle.

#### Bake Project Workspace
- **Top Main Menus**: `File`, `Bake` (Start Bake, Clear Normal Preview), `View` (Outliner, Mesh Ingestion, Bake Parameters, Output Log, UDIM Layout), and `Help`.
- **Viewport Overlay Toolbar**: Displays mesh visibility chips (`High`, `Low`, `Wire`, `Cage`, `Grid`, `Baked Normals`), interactive tools (`Navigate`, `Paint Cage`, `Paint Skew`), and studio shading mode combo.
- **Right Panels**: Shows `Mesh Ingestion` and `Bake Parameters`.
- **Bottom HUD**: Prominent `FloatingBakeBar` with tile count, resolution, and orange `[START BAKE]` button / animated progress bar.
- **Focus**: Defaults viewport to `View3D` with focus on `Output Log`.

#### UV Cut & Pack Workspace
- **Top Main Menus**: `File`, `Edit` (Undo, Redo, Select Loop, Select Ring), `UV` (Auto Seams, Cut Seams, Sew Seams, Clear Seams, Conformal Unwrap, Pack UDIMs), `View`, and `Help`.
- **Viewport Overlay Toolbar**: Displays geometry chips (`Low`, `Wire`, `Seams`, `Grid`), history buttons (`Undo`, `Redo`), interactive UV tools (`Navigate`, `Cut Seam`, `Curve Cut`), and studio shading mode combo.
- **Right Panels**: Shows `UV Tool` with Maya gesture cutting, 3D surface curves, conformal unwrap, ZenUV texel density system, island stacking/alignment, and UVPackmaster 4 packing.
- **Bottom HUD**: Sleek `FloatingUVBar` displaying live seam count, island count, `[Unwrap (Ctrl+U)]`, `[Pack (Ctrl+P)]`, and `[Export OBJ]`.
- **Focus**: Defaults viewport to `Split 3D | 2D` with focus on `UDIM Layout`.

### 2.6 ImGui Window Stack Invariant & Section Card Lifecycle
- **Problem**: When switching dock tabs to `Bake Parameters`, an inactive `Mesh Ingestion` child window caused Dear ImGui assertions (`Must call EndChild() and not End()!`, `Missing PopStyleColor()`, `Missing PopStyleVar()`) that followed the mouse cursor.
- **Root Cause**: In Dear ImGui, `BeginChild` returns false if the child or parent window is clipped or inactive.
- The previous implementation returned `false` on early clipping without calling `EndChild()`, causing the caller to skip `EndSectionCard()` while the child remained unclosed on ImGui's stack.
- Additionally, panel rendering methods (`RenderMeshPanel`, `RenderSettingsPanel`, `RenderOutlinerPanel`, `RenderUVCutAndPackPanel`, `RenderUDIMCanvasPanel`, `RenderBakeProgressPanel`) did not guard against `!ImGui::Begin()`.
- **Resolution**:
  1. `BeginSectionCard` now guarantees immediate cleanup (`EndChild()`, `PopStyleColor(2)`, `PopStyleVar(2)`) if `BeginChild` returns false, ensuring zero unclosed child windows on stack.
  2. All panel rendering functions now cleanly early-exit with `if (!ImGui::Begin(...)) { ImGui::End(); return; }`.

### 2.7 ZenUV and UVPackmaster 4 DCC Feature Parity
- **Texel Density Toolkit**:
  - Live calculation of 3D world area versus 2D UV area.
  - Target Texel Density in px/cm with one-click uniform scaling across all UV shells.
  - Equalize Texel Density button to eliminate texture stretching across multi-UDIM assets.
- **Island Stacking & Overlap Arbitration**:
  - Topological isomorphism detection to stack identical UV shells at identical UV coordinates for shared texture budget.
  - Overlap separation mode to offset duplicate shells by +1 along U axis into isolated bake tiles.
  - Straighten Islands (ZenUV Quadrify/Reshape) via 2D covariance principal axis alignment.
  - Orient Islands to 3D World Up (+Y) vector.
- **Advanced Heuristic Packing**:
  - Configurable rotation search (0°, 90°, 45°, Free 1°).
  - Lock overlapping islands option to preserve stacked symmetry shells during repack.
  - Fixed scale packing mode to strictly preserve author-specified Texel Density.
  - Multi-UDIM packing across tiles 1001-1010 with MaxRects BSSF heuristic search.

---

## 3. Normal Map Seam Resolution & Marmoset Alignment
- **Problem Resolved**: Raw low-poly OBJs exported from ZBrush contain 0 `vn` directives. Marmoset Toolbag defaults to 100% smooth shading across all edges when importing meshes lacking normals.
- **Solution Implemented**:
  1. Default low-poly shading normal calculation updated to 180° crease angle (`SmoothNormalsIfMissing(180.0f)` in `MeshLoader.cpp` and `MeshTypes.h`), matching Marmoset and DCC standards.
  2. Cage normals decoupled from shading normals (`ComputeWatertightCageNormals(180.0f)`) to guarantee watertight envelope ray casting without diagonal cross-over.
  3. Default seam dilation increased to 32 pixels at 2K (64 pixels at 4K) via Jump Flooding Dilation (JFA) to eliminate mipmap background color bleeding.
- **Verification Metrics**:
  - Seam normal median error across all 189 seam vertices dropped from **28.01° down to 8.22°** (matching Marmoset's **6.71°**).
  - Material shaded viewport comparison in Blender 5.1 demonstrates an average pixel difference of only **1.69 out of 255 levels** (less than 0.7% total difference).
  - Exported low-poly mesh [`UDIM_LowPoly_LowPoly_Baked.obj`](file:///C:/Users/raman/Downloads/temp/bb/UDIM_LowPoly_LowPoly_Baked.obj) preserves 1,044 unique geometric vertices with explicit 100% smooth `vn` normals.

---

## 4. UI Handoff Package & Documentation Map
All UI specifications, layout configs, and source codes are organized in [`docs/`](file:///e:/CODES/zbrush%20plugins/baker/docs/):
- **UI Architecture Specification**: [`docs/UI_ARCHITECTURE.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/UI_ARCHITECTURE.md) (Master UI architecture document).
- **Full Widget Inventory**: [`docs/ui_handoff/UI_INVENTORY.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/UI_INVENTORY.md) (Every widget, label, state variable, callback, and shortcut).
- **Known Layout Issues**: [`docs/ui_handoff/KNOWN_ISSUES.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/KNOWN_ISSUES.md) (Catalog of hard-coded pixel sizing, high-DPI scaling defects, and docking collisions).
- **Docking Configuration**: [`docs/ui_handoff/DOCKING_LAYOUT.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/DOCKING_LAYOUT.md) and [`docs/ui_handoff/imgui.ini`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/imgui.ini).
- **Screenshot Capture Guide**: [`docs/ui_handoff/SCREENSHOTS/SCREENSHOT_GUIDE.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/SCREENSHOTS/SCREENSHOT_GUIDE.md) (Visual checklist for capturing all 7 states at 1080p and 720p).
- **UI Source Code Archive**: [`docs/ui_handoff/SOURCE/`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/SOURCE/) (Standalone copy of all ImGui and D3D11 viewport files).

---

## 5. Viewport Advancements & Ultra-Resolution Support

### 5.1 Blender Dynamic Anti-Aliased Ground Grid
The 3D ground plane has been completely upgraded from coarse pixelated line primitives to Blender's procedural multi-scale grid shader.
- Evaluated entirely within the pixel shader using screen-space screen derivatives (`fwidth`).
- Computes dual hierarchical subdivision scales (`s0` and `s1`) based on camera distance log-LOD.
- Fine grid lines smoothly cross-fade into primary major grid units as the camera pulls back.
- Colored primary coordinate axes: Red X axis (`z = 0`) and Green Z axis (`x = 0`).
- Features smooth radial horizon fade (`1.0 - smoothstep(maxRadius * 0.45, maxRadius, dist)`), eliminating harsh edge clipping.

### 5.2 Real-Time Baked Normal Map Viewport Preview
The 3D viewport now supports immediate in-situ inspection of baked normal maps directly on the low-poly mesh.
- Low-poly mesh vertices supply full tangent space frames (`pos`, `norm`, `uv`, `tangent`, `bitangent`).
- Upon completion of a bake, the normal map buffer is staged to the Direct3D 11 render thread via `SetBakedNormalMap`.
- The pixel shader samples the normal map texture at `input.UV` and re-orthogonalizes the basis via Gram-Schmidt math.
- The perturbed surface normal is illuminated by three-point studio lighting with Blinn-Phong specularity.
- Viewport toggle chip `[Baked Normals]` allows instant A/B toggling against the unperturbed low-poly mesh.

### 5.3 8K and 16K Extreme-Resolution Baking
The baking engine now supports both `8192 (8K)` and `16384 (16K)` texture resolutions.
- Memory indexing and vector buffer allocations updated to 64-bit `size_t` math across scanline loops and Jump Flooding Dilation passes.
- Jump Flooding coordinate tracking (`SeedPoint`) utilizes signed 16-bit integers with a range up to 32,767, natively accommodating 16K grids.
- Multi-threaded scanline rasterization scales seamlessly across all available hardware CPU cores.

---

### 5.4 Design System Spec Polish & Tactile Affordances (2026-10-26)
Following a comprehensive 5-axis design reference interpretation audit against Design System Spec and Dark Pro Tool principles:
- Keyboard Navigation Focus Ring: Explicitly mapped ImGuiCol_NavHighlight and ImGuiCol_NavWindowingHighlight to Col::Accent() (#4CC3A6) with dimmed background (#000000, 50% opacity) for high-contrast accessibility across all interactive widgets.
- Radial Marking Menu Readability: Added a 1px dark drop shadow (IM_COL32(0, 0, 0, 220)) beneath radial wedge text labels and the central hub MAYA brand text, ensuring clear legibility even over bright, high-key clay sculpt surfaces.
- Splitter Hover and Drag Affordance: Enhanced the 3D/2D vertical sliding divider with smooth hover color transitions (#4CC3A6), active dragging feedback, and a tactile rounded center pill grab handle.

---

### 5.5 Solid Opaque Low-Poly Shading, Quad UVs, and Cursor Polish (2026-10-26)
Addressed rendering artifacts and UI visual clarity:
- Low-Poly Solid Opaque Shading: Fixed low-poly mesh transparency and missing face holes by setting default lowPolyOpacity to 1.0f (100% opaque), enforcing m_solidRS (D3D11_CULL_BACK), and utilizing m_opaqueBlendState when opacity is 1.0f. This completely eliminates depth-buffer write conflicts and backface penetration.
- Pure Quad UV Wireframe: Updated 2D UV canvas to render perimeter outlines directly from polygonFaces (and wireframeLineIndices) rather than triangle edges, eliminating internal diagonal splits. Removed distracting vertex dot circles for clean Blender-identical UV shells.
- Circular Reticle Removal: Completely removed the screen-space circular cursor reticle from the 3D viewport, restoring standard OS mouse navigation across all modes.

---

## 6. Marmoset Paint Cage & Paint Skew Architecture (Phase 2 Roadmap)
To achieve full parity with Marmoset Toolbag's interactive 3D brush toolset:
1. **Interactive 3D Ray-Mesh Picking**:
   - Direct screen-space mouse ray casting against low-poly mesh triangles using Embree RTCIntersect.
2. **Paint Cage Brush**:
   - A 3D spherical falloff brush allowing artists to push or pull cage offsets per-vertex.
   - Vertices store custom displacement vectors `cagePositions[i] = v[i].pos + v[i].norm * (cageDist + brushOffset[i])`.
   - Real-time cage vertex buffer updating on mouse drag.
3. **Paint Skew Brush**:
   - Per-vertex skew direction averaging.
   - Smooths ray casting directions across hard mechanical corners and angled bevels to prevent skewed details.

---

## 7. Toolchain & Build Environment
- **Compiler**: Microsoft Visual C++ (`MSVC v143` / `v144`, Hostx64/x64).
- **CMake**: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
- **MSBuild**: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\MSBuild\Current\Bin\MSBuild.exe`.
- **Build Command**:
  ```powershell
  & "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release
  ```
- **Deployment Script**:
  ```powershell
  powershell -ExecutionPolicy Bypass -File .\deploy_plugin.ps1
  ```
