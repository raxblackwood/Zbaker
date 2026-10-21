# Companion Baker UI Comprehensive Inventory

This document provides a comprehensive widget-by-widget specification of the companion baker UI for redesign and review without running the executable.

---

## 1. Application Window Shell & Dimensions
- **Initial Companion Viewport Dimensions (`Main.cpp`)**: `1280 x 800 px`.
- **Initial Standalone Studio Dimensions (`StandaloneMain.cpp`)**: `1400 x 850 px`.
- **Minimum Usable Resolution**: `1024 x 640 px`.
- **Recommended Redesign Target**: `1920 x 1080 px` (with responsive adaptation down to `1280 x 720 px`).

---

## 2. Global Keyboard Shortcuts & Input Navigation
| Shortcut | Action Triggered | Associated Handler |
| :--- | :--- | :--- |
| `Ctrl + N` | Clears high-poly, low-poly, BVH, UDIM selection, and baked results | `ShellState::clearScene` / `ClearScene()` |
| `Ctrl + O` | Opens Win32 file picker for Low-Poly UV mesh (`.obj`, `.fbx`, `.goz`, `.bin`) | `ShellState::openLowPoly` / `LoadLowPolyMesh()` |
| `Ctrl + Shift + O` | Opens Win32 file picker for High-Poly sculpt (`.obj`, `.fbx`, `.goz`, `.bin`) | `ShellState::openHighPoly` / `LoadHighPolyMesh()` |
| `Alt + F4` | Terminates companion process and releases Direct3D 11 resources | `ShellState::exitApp` / `PostQuitMessage(0)` |
| `LMB Drag` (Viewport) | 3D Camera Orbit around mesh bounding box center | `ViewportRenderer::OnMouseMove` (Orbit mode) |
| `MMB Drag` or `Shift + LMB` | 3D Camera Pan in screen space | `ViewportRenderer::OnMouseMove` (Pan mode) |
| `Mouse Wheel` | 3D Camera Dolly / Zoom along look vector | `ViewportRenderer::OnMouseWheel` |

---

## 3. Main Menu Bar (`##MainMenuBar`)
Always visible at the top edge of the window.

### "File" Menu
- **`New Session (Clear Scene)`** (`Ctrl+N`):
  - Action: Clears active high-poly, low-poly, BVH structure, and diagnostic maps.
  - State: Resets `m_highPoly`, `m_lowPoly`, `m_bvhReady`, `m_bakeResults`.
- **`Open Low-Poly...`** (`Ctrl+O`):
  - Action: Opens native Windows file selection dialog.
  - State: Loads mesh into `m_lowPoly`, generates watertight normals, detects UDIM tiles.
- **`Open High-Poly...`** (`Ctrl+Shift+O`):
  - Action: Opens native Windows file selection dialog.
  - State: Loads mesh into `m_highPoly`, builds Intel Embree 3 BVH acceleration structure.
- **`Open Cage...`**:
  - Action: Opens file selection dialog for explicit morph cage mesh.
  - State: Loads cage vertices into `m_lowPoly.cagePositions`.
- **`Load ZBrush Cache`**:
  - Action: Scans `C:/Users/Public/Documents/ZBrushData2026/ZPluginData/UDIMBaker/` for cached GoZ/OBJ transfers.
  - State: Reads `UDIM_HighPoly.obj` and `UDIM_LowPoly.obj`.
- **`Exit`** (`Alt+F4`):
  - Action: Shuts down application.

### "View" Menu
- **`Scene Outliner`**:
  - Widget: Checkbox MenuItem.
  - State Variable: `m_shellState.showOutliner`.
- **`Output Log`**:
  - Widget: Checkbox MenuItem.
  - State Variable: `m_shellState.showLog`.
- **`UDIM Layout`**:
  - Widget: Checkbox MenuItem.
  - State Variable: `m_shellState.showUdim`.
- **`Theme` Submenu**:
  - `Blender Dark (Default)`: Activates `UITheme::BlenderDark` (`0x1D1D1D` background, `0xE87D0D` orange accent).
  - `Marmoset Ember (Warm Charcoal)`: Activates `UITheme::MarmosetEmber` (`0x111317` background, `0xFF7A18` ember accent).
  - `Cyber Teal`: Activates `UITheme::CyberTeal` (`0x0E1316` background, `0x00D2B4` teal accent).
  - `Studio Slate (Neutral)`: Activates `UITheme::StudioSlate` (`0x131519` background, `0x4E92F9` blue accent).
- **`Reset Layout`**:
  - Action: Invokes `BakerUI::BuildDefaultLayout()`, restoring default split percentages and window docking.

### "Help" Menu
- **`About`**:
  - Action: Displays version info, Intel Embree 3.13 link status, and D3D11 feature level.

---

## 4. Scene Outliner Window (`Scene Outliner###outliner`)
- **Dock Location**: Right column, top panel (Dock ID: `0x00000005`).
- **Default Size**: `379 x 293 px`.
- **Visibility**: Controlled by `m_shellState.showOutliner`.

### Widgets & State Variables
1. **Section Header**: `"SCENE COLLECTION"`
2. **Button `"Clear##outliner_clr"`**:
   - Action: Clears active scene meshes and BVH structure.
   - Visibility: Shown only when high-poly or low-poly is loaded.
3. **High-Poly Tree Node (`"High-Poly Sculpt##outliner_hp"`)**:
   - Icon / Visibility Chip (`"Eye##hp_vis"`): Toggles `m_showHighPoly`.
   - Text Display: File path, vertex count, triangle count, SubTool count.
   - Status Indicator: `StatusPill("BVH Ready", Ok)` or `StatusPill("Building...", Warn)`.
   - Button `"X##hp_del"`: Unloads high-poly mesh and frees BVH memory.
4. **Low-Poly Tree Node (`"Low-Poly UV Target##outliner_lp"`)**:
   - Icon / Visibility Chip (`"Eye##lp_vis"`): Toggles `m_showLowPoly`.
   - Text Display: File path, vertex count, polygon count, detected UDIM tile count.
   - Status Indicator: `StatusPill("Watertight", Ok)`.
   - Buttons `"All##sel_all"` / `"None##sel_none"`: Selects or deselects all detected UDIM tiles.
   - Button `"X##lp_del"`: Unloads low-poly mesh.
5. **Cage Tree Node (`"Projection Cage##outliner_cage"`)**:
   - Icon / Visibility Chip: Toggles `m_showCage`.
   - Text Display: Procedural normal offset distance or explicit cage filename.
   - Button `"X##cage_del"`: Removes explicit cage and reverts to procedural offset.

---

## 5. Viewport Window (`Viewport###viewport`)
- **Dock Location**: Central node (Dock ID: `0x00000003`, `ImGuiDockNodeFlags_NoTabBar`).
- **Default Size**: `875 x 507 px`.
- **Visibility**: Always visible.

### Widgets & State Variables
1. **Render Texture Canvas (`ImGui::Image`)**:
   - Texture Pointer: `m_viewportRenderer.GetShaderResourceView()`.
   - Sizing: Matches available window content region (`availSize = ImGui::GetContentRegionAvail()`).
   - Mouse Interaction: When hovered, routes mouse delta to orbit, pan, and dolly controls.
2. **Top Overlay Toolbar (Floating row of `ToggleChip` pills)**:
   - `ToggleChip("Wireframe##vp_wire", &m_showWireframe)`: Renders low-poly quad wireframe without diagonal splits.
   - `ToggleChip("Cage##vp_cage", &m_showCage)`: Visualizes expanded translucent cage envelope.
   - `ToggleChip("Normals##vp_norm", &m_showNormals)`: Displays vertex normal ray vectors.
   - `ToggleChip("High-Poly##vp_hp", &m_showHighPoly)`: Toggles visibility of high-poly sculpt.
   - `ToggleChip("Low-Poly##vp_lp", &m_showLowPoly)`: Toggles visibility of low-poly mesh.
   - `ToggleChip("Grid##vp_grid", &m_showGrid)`: Toggles Blender-style infinite anti-aliased ground grid.
   - `ToggleChip("MatCap##vp_matcap", &m_useMatCap)`: Toggles between PBR studio lighting and clay MatCap.
   - `Button("Reset Cam##vp_cam")`: Centers and fits camera view to bounding box.
3. **Floating Bake Bar (`FloatingBakeBar`)**:
   - Anchored at bottom center of the viewport canvas.
   - Width: `min(540px, viewportWidth - 32px)`, Height: `44px`.
   - **Idle State**:
     - Left Label: Formatted string `"%d UDIM Tiles | %dx%d"` or `"Load meshes in Mesh Ingestion tab"`.
     - Right Button: Orange primary button `"START BAKE"` (or disabled `"BAKE (NO MESH)"`).
     - Driving Variable: Enabled when `m_highPolyLoaded && m_lowPolyLoaded && m_bvhReady`.
   - **Baking State**:
     - Animated progress bar fill (`#B85F00`).
     - Centered Label: Formatted string `"%s (%.0f%%)"` (e.g. `"Baking Tile 1001... 45%"`).

---

## 6. Mesh Ingestion Window (`Mesh Ingestion###ingest`)
- **Dock Location**: Right column, lower panel (Dock ID: `0x00000006`, tabbed with Bake Parameters).
- **Default Size**: `379 x 404 px`.

### Widgets & State Variables
1. **High-Poly Sculpt Card (`BeginCard("hp_card")`)**:
   - Header: `"HIGH-POLY SCULPT / SOURCE"` + `StatusPill`.
   - Primary Button `"Browse High-Poly Mesh..."`: Launches Win32 file picker.
   - Read-only InputText: File path string (`m_highPolyPath`).
   - Button `"SecondaryButton("Remove##hp_rm")"`: Clears high-poly mesh.
2. **Low-Poly Target Card (`BeginCard("lp_card")`)**:
   - Header: `"LOW-POLY TARGET (UDIM)"` + `StatusPill`.
   - Primary Button `"Browse Low-Poly Mesh..."`: Launches Win32 file picker.
   - Read-only InputText: File path string (`m_lowPolyPath`).
   - Small Buttons `"All"` / `"None"`: Bulk UDIM tile toggle.
   - Secondary Button `"Remove Low-Poly"`: Clears low-poly mesh.
3. **Alignment & Transforms Card**:
   - Secondary Button `"Auto-Align Centers"`: Translates low-poly to match high-poly center of mass.
   - Secondary Button `"Fit Scale"`: Scales low-poly bounding box to match high-poly dimensions.
   - Secondary Button `"Reset"`: Reverts position, rotation, and scale back to identity.
4. **Cage Definition Card**:
   - Secondary Button `"Browse Explicit Cage (*.obj;*.fbx)..."`: Loads morph cage.
   - Secondary Button `"Revert to Procedural Cage"`: Discards explicit cage.

---

## 7. Bake Parameters Window (`Bake Parameters###params`)
- **Dock Location**: Right column, lower panel (Dock ID: `0x00000006`, tabbed with Ingestion).
- **Default Size**: `379 x 404 px`.

### Widgets & State Variables
1. **Section `"PROJECTION & QUALITY"`**:
   - `PropCombo("Resolution")`: Options: `1024`, `2048`, `4096`, `8192`. Drives `m_settings.resolution`.
   - `PropCombo("Antialiasing")`: Options: `1x (Draft)`, `2x2 (4 rays/pixel)`, `4x4 (16 rays/pixel)`. Drives `m_settings.antiAliasing`.
   - `PropFloat("Skew factor")`: Range: `0.00` to `1.00`. Drives `m_settings.skewFactor` (0 = Smooth Normals, 1 = Flat Face Normals).
   - `PropFloat("Cage offset")`: Range: `0.0001` to dynamic maximum. Drives `m_settings.cageDistance`.
   - `PropFloat("Max ray length")`: Range: `0.001` to dynamic maximum. Drives `m_settings.maxRayDistance`.
   - `PropInt("Padding (px)")`: Range: `0` to `64`. Drives `m_settings.seamPadding` (Jump Flooding dilation).
   - `PropCheck("Discard Backface")`: Prevents border inversion. Drives `m_settings.discardBackfaceHits`.
   - `PropFloat("Angle Threshold")`: Range: `30.0` to `90.0 deg`. Drives `m_settings.angleThreshold`.
   - `PropCheck("Bidirectional Rays")`: Dual-direction ray-tracing. Drives `m_settings.useBidirectionalRays`.
   - `PropCheck("Isolate Mesh Parts")`: Prevents ray bleeding between subtools. Drives `m_settings.isolateMeshParts`.
2. **Section `"OUTPUT MAPS (MARMOSET SUITE)"`**:
   - `PropCheck("Tangent Normal (MikkT)")`: Drives `m_settings.bakeNormal`.
   - `PropCheck("World / Object Normal")`: Drives `m_settings.bakeWorldNormal`.
   - `PropCheck("Displacement / Height (32f)")`: Drives `m_settings.bakeDisplacement`.
   - `PropCheck("Ambient Occlusion (AO)")`: Drives `m_settings.bakeAmbientOcclusion`.
     - Sub-Prop `PropInt("AO Samples")`: Range: `4` to `64`. Drives `m_settings.aoSamples`.
     - Sub-Prop `PropFloat("AO Radius")`: Drives `m_settings.aoMaxDistance`.
   - `PropCheck("Cavity Map")`: Drives `m_settings.bakeCavity`.
   - `PropCheck("Curvature Map")`: Drives `m_settings.bakeCurvature`.
     - Sub-Prop `PropFloat("Cavity Contrast")`: Range: `0.1x` to `10.0x`. Drives `m_settings.cavityStrength`.
   - `PropCheck("Thickness / SSS Map")`: Drives `m_settings.bakeThickness`.
     - Sub-Prop `PropFloat("Thickness Depth")`: Drives `m_settings.thicknessMaxDistance`.
   - `PropCheck("World Position Map")`: Drives `m_settings.bakePosition`.
   - `PropCheck("Material ID (Polygroups)")`: Drives `m_settings.bakeMaterialID`.
   - `PropCheck("Base Color (Polypaint)")`: Drives `m_settings.bakeBaseColor`.
   - `PropCheck("Diagnostic map")`: Drives `m_settings.bakeDiagnosticMap`.
   - `PropCombo("Normal Format")`: Options: `OpenGL (+Y)` vs `DirectX (-Y)`. Drives `m_settings.flipYNormal`.
   - `PropCheck("Compact BVH")`: Optimizes RAM for 50M-100M poly sculpts. Drives `m_settings.useCompactBVH`.
3. **Section `"DESTINATION & FORMATS"`**:
   - InputText + Button `"Browse..."`: Output directory path. Drives `m_settings.outputDir`.
   - Secondary Button `"Open Output Folder in Explorer"`: Launches `explorer.exe` at `m_settings.outputDir`.
   - `PropCheck("Export 8-Bit PNG")`: Drives `m_settings.exportPNG`.
   - `PropCheck("Export Float EXR")`: Drives `m_settings.exportEXR`.
4. **Bottom Button**:
   - `BakerUI::BakeButton(bakeProgress, "START HARDWARE-ACCELERATED BAKE")`: Full width in-panel bake trigger.

---

## 8. UDIM Layout Window (`UDIM Layout###udim`)
- **Dock Location**: Bottom panel (Dock ID: `0x00000004`, tabbed with Output Log).
- **Default Size**: `875 x 190 px`.
- **Visibility**: Controlled by `m_shellState.showUdim`.

### Widgets & State Variables
1. **Section Header**: `"TILE SELECTION FOR BAKE"`
2. **Button `"Select All Tiles"`**: Marks all detected UDIM IDs into `m_selectedUDIMs`.
3. **Button `"Select Only 1001"`**: Isolates tile 1001.
4. **Button `"Deselect All"`**: Clears `m_selectedUDIMs`.
5. **Interactive 2D Grid Canvas**:
   - Drawn using `ImDrawList` showing grid coordinates `[1001]`, `[1002]`, `[1003]`...
   - Tiles with UV polygons rendered with colored boundary cards.
   - Clicking a tile toggles its inclusion in `m_selectedUDIMs`.

---

## 9. Output Log Window (`Output Log###log`)
- **Dock Location**: Bottom panel (Dock ID: `0x00000004`, tabbed with UDIM Layout).
- **Default Size**: `875 x 190 px`.
- **Visibility**: Controlled by `m_shellState.showLog`.

### Widgets & State Variables
1. **Button `"Clear"`**: Empties log vector `m_logEntries`.
2. **Button `"Copy All"`**: Copies formatted log string to Windows clipboard via `ImGui::SetClipboardText`.
3. **Checkbox `"Auto-scroll"`**: Drives boolean `m_logAutoScroll`.
4. **Scrollable Region (`ImGui::BeginChild("LogScrollRegion")`)**:
   - Displays line-by-line log messages with timestamp prefix.
   - Text color driven by category: `[System]` / `[Info]` = white, `[Warn]` = amber, `[Error]` = red.

---

## 10. Status Bar (`##statusbar`)
Always docked at the very bottom of the host window.
- **Status Bullet**: Circle icon with color driven by `ShellState::statusColor`.
- **Status Message**: Readout driven by `ShellState::statusText` (e.g. `"Ready"`, `"BVH Ready (15,804,962 verts)"`, `"Baking completed in 18.4s"`).
- **FPS Indicator**: Right-aligned framerate monitor (`"%.0f FPS"`).
