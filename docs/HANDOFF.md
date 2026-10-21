# ZBrush UDIM Baker — Engineering & Artist Handoff Guide

## 1. System Requirements & Prerequisites

* **Operating System**: Windows 10 or Windows 11 (64-bit).
* **Target Host**: Maxon ZBrush 2024, 2025, or 2026 (`C:\Program Files\Maxon ZBrush 2026`).
* **Graphics API**: Direct3D 11 compatible GPU (NVIDIA GTX 900+ / RTX, AMD Radeon RX, Intel Arc).
* **Build Toolchain** (For compilation only):
  * Visual Studio 2022 / 2026 Build Tools (MSVC `v143` or `v144`).
  * CMake 3.20+ (bundled with Visual Studio).
  * Windows 10/11 SDK.

---

## 2. Compilation & Deployment

### 2.1 One-Click Automated Deployment
From the project root directory, run PowerShell:
```powershell
powershell -ExecutionPolicy Bypass -File .\deploy_plugin.ps1
```
This script will:
1. Copy `UDIMBaker.txt` to `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\`.
2. Create `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBakerData\`.
3. Copy `UDIMBakerBridge.dll`, `UDIMBakerApp.exe`, `embree3.dll`, and `tbb12.dll` to `UDIMBakerData`.

### 2.2 Building from Source with CMake
If modifying C++ source code:
```powershell
# 1. Generate CMake Build Files
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" -B build -G "Visual Studio 18" -A x64

# 2. Build in Release Configuration
& "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build build --config Release

# 3. Deploy Plugin to ZBrush
powershell -ExecutionPolicy Bypass -File .\deploy_plugin.ps1
```

---

## 3. Artist & User Guide

### 3.1 Launching inside ZBrush
1. Open **Maxon ZBrush 2026**.
2. Navigate to the top menu: **ZPlugin $\rightarrow$ UDIM Baker**.
3. Click **Launch UDIM Baker**.
4. The companion app will open immediately as an independent window with the active sculpt loaded.

### 3.2 Standalone Usage (Outside ZBrush)
You can launch the companion app at any time by double-clicking:
`C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBakerData\UDIMBakerApp.exe`
(or `build\bin\Release\UDIMBakerApp.exe` in your dev workspace).

### 3.3 Loading Meshes
* **High-Poly Sculpt**: Click `Browse OBJ / FBX / BIN...` in the **Mesh Ingestion** panel. Supports dense sculpts (tested up to 28M+ vertices / 10M+ triangles).
* **Low-Poly Target**: Click `Browse Low-Poly OBJ / FBX...`. The baker will automatically detect all UDIM tiles present in the UV coordinates and configure the output path.

### 3.4 3D Viewport Navigation (Industry Standard)
The 3D Viewport uses Maya / Substance Painter navigation conventions:
* **Orbit / Tumble**: `Alt + Left Mouse Button` (or drag with Left Mouse Button).
* **Pan / Track**: `Alt + Middle Mouse Button` (or `Alt + Right Mouse Button`).
* **Zoom / Dolly**: `Mouse Scroll Wheel` (exponential, responsive scaling) or `Alt + Right Mouse Button` dragging.
* **Frame All (Focus)**: Press the **`F`** key on your keyboard or click the top-right `[ 🎯 Frame All ]` button to instantly center the active mesh in the camera's view.

### 3.5 Split-Screen (3D View vs 2D UV View)
In the top-right toolbar, toggle between view modes:
* **`[ 3D View ]`**: Fullscreen 3D viewport rendering the high-poly sculpt, low-poly cage envelope, and surface normals.
* **`[ 2D UV ]`**: Fullscreen 2D UV Canvas with UDIM tile grid lines, coordinate labels, and low-poly UV wireframe overlay. Pan with `MMB`/`RMB`, zoom with the scroll wheel.
* **`[ Split 3D | 2D ]`**: Side-by-side viewport displaying 3D geometry on the left and 2D UV tile layouts on the right.

### 3.6 UDIM Tile Selection & Modular Mesh Placement
In production, low-poly assets often include instanced duplicate modular pieces placed across the scene that reuse the same texture coordinates (e.g. Wall 3 placed to the right with UVs placed in tile 1002, while the high-poly sculpt only contains Walls 1 and 2):
* In the **UDIM Layout** panel, inspect detected tiles.
* Uncheck tiles that do not have a high-poly sculpt counterpart (e.g. uncheck `1002`).
* Or click **`[ Select Only 1001 ]`** to bake only the main sculpt tile.
* Alternatively, in the **2D UV Canvas**, clicking directly on any tile toggles its bake status (`[✓ Bake]` vs `[Excluded]`).

### 3.7 Scale & Transform Synchronization
If your low-poly mesh was scaled or translated differently in an external DCC:
* **`Auto-Align Centers`**: Automatically matches the bounding box center of the low-poly mesh to the high-poly sculpt.
* **`Auto-Fit Scale`**: Automatically scales the low-poly mesh to match the high-poly sculpt's bounding diagonal.
* **Manual Sliders**: Fine-tune Translation ($X, Y, Z$), Rotation (Pitch, Yaw, Roll), and Scale.

### 3.8 Triggering the Bake
1. Set your desired **Resolution** (`1024`, `2048`, `4096`, `8192`).
2. Choose **Antialiasing** (`1x Draft`, `2x2 Production Standard`, `4x4 Ultra Quality`).
3. Check your desired output maps in the **Marmoset Map Suite** section.
4. Verify your output destination folder and filename.
5. Click the large amber button: **`START HARDWARE-ACCELERATED BAKE`**.
6. The multithreaded engine will ray-trace all tiles concurrently and save ready-to-use PNG and EXR files to disk.

---

## 4. Troubleshooting & Known Edge Cases

### Issue 1: "My normal map is coming out completely flat blue!"
* **Cause**: The Low-Poly mesh has triangles on a UDIM tile (e.g. tile 1002) that sit in empty space where no high-poly sculpt exists.
* **Solution**: Open the **UDIM Layout** panel and uncheck tile `1002` (or click `[ Select Only 1001 ]`). Ensure the selected tiles correspond to areas where sculpt geometry actually exists.

### Issue 2: "Rays are missing deep stone crevices or high protrusions"
* **Cause**: The `Max ray length` or `Cage offset` is too small for a large real-world scale sculpt (e.g. a 7.5-meter stone wall).
* **Solution**: The baker auto-calibrates these values on load (`cageDistance = diag * 0.015f`, `maxRayDistance = diag * 0.08f`). If needed, increase `Cage offset` and `Max ray length` sliders in the **Projection** panel.

### Issue 3: "Seams are visible in the game engine at UV island borders"
* **Cause**: Texture filtering (mipmapping) is picking up black background texels.
* **Solution**: Keep `Padding (px)` set to `16` or `32` (uses the built-in Jump Flooding Dilation algorithm to flood edge normals seamlessly into empty texture space).

### Issue 4: "Only the edges of stones have details, while the centers of the islands are flat blue"
* **Cause**: The Low-Poly mesh scale was inadvertently changed or offset from $(0,0,0)$, shrinking the low-poly envelope so rays inside the stones miss the sculpt completely. This commonly happened when high-poly and low-poly had different numbers of modular walls (e.g. 2 sculpt walls vs 3 low-poly walls), causing bounding-box diagonal ratios to deviate from 1.0.
* **Solution**: Keep **Scale at 1.000x** and **Position at (0, 0, 0)**. Meshes exported in real world units from your DCC/ZBrush already share identical origins and scale. If needed, click **`Reset Transform`** to restore exact 1:1 scale.

---

## 5. UI Architecture & Design System Handoff

For redesigning, evaluating, or auditing the user interface without launching the application, refer to the dedicated UI documentation:

1. **[UI Architecture Specification](file:///e:/CODES/zbrush%20plugins/baker/docs/UI_ARCHITECTURE.md)**:
   - Full overview of the Blender Dark visual identity, theme system, color palettes, and Dear ImGui 1.93.1 docking hierarchy.
   - Complete application state machine (`Empty`, `Ingesting`, `Loaded`, `Baking`, `Results`, `Error`).
   - Specification of the Direct3D 11 viewport render target integration and the floating bake bar HUD.

2. **[UI Handoff Package](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/)**:
   - [`UI_INVENTORY.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/UI_INVENTORY.md): Comprehensive inventory of every window, card, button, slider, checkbox, driving variable, callback action, and shortcut.
   - [`KNOWN_ISSUES.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/KNOWN_ISSUES.md): Full audit of layout collisions, hard-coded pixel sizing, high-DPI scaling defects, and docking tab glitches.
   - [`DOCKING_LAYOUT.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/DOCKING_LAYOUT.md) & [`imgui.ini`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/imgui.ini): Exact docking node IDs, split ratios, and persistent window coordinates.
   - [`SCREENSHOTS/SCREENSHOT_GUIDE.md`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/SCREENSHOTS/SCREENSHOT_GUIDE.md): Setup checklist and resolution guide (`1920x1080` and `1280x720`) for capturing all 7 application states.
   - [`SOURCE/`](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/SOURCE/): Standalone archive containing all UI source code and viewport rendering implementations.

---

## 6. Codebase Maintenance & File Index

| Directory / File | Description |
|---|---|
| [`src/app/Main.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/app/Main.cpp) | Win32 entry point, Direct3D 11 device creation, Dear ImGui initialization, and message loop. |
| [`src/app/UIManager.h`](file:///e:/CODES/zbrush%20plugins/baker/src/app/UIManager.h) / [`.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/app/UIManager.cpp) | Main UI controller: docking panels, transform controls, UDIM selection, progress reporting, and bake dispatching. |
| [`src/app/BakerUI.h`](file:///e:/CODES/zbrush%20plugins/baker/src/app/BakerUI.h) | Modern Marmoset Ember theme styling, color palettes, custom widgets, cards, and theme switcher. |
| [`src/app/ViewportRenderer.h`](file:///e:/CODES/zbrush%20plugins/baker/src/app/ViewportRenderer.h) / [`.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/app/ViewportRenderer.cpp) | Direct3D 11 hardware viewport: Phong lighting, cage visualization, and orbit/pan/zoom camera math. |
| [`src/app/MeshLoader.h`](file:///e:/CODES/zbrush%20plugins/baker/src/app/MeshLoader.h) / [`.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/app/MeshLoader.cpp) | High-poly and low-poly mesh import via TinyOBJLoader, OpenFBX, and binary stream caches. |
| [`src/baker/BakerEngine.h`](file:///e:/CODES/zbrush%20plugins/baker/src/baker/BakerEngine.h) / [`.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/baker/BakerEngine.cpp) | Hardware ray-tracing core: Intel Embree 3 BVH, multithreaded scanline workers, closest-depth testing, cavity/curvature, and JFA dilation. |
| [`src/baker/MeshTypes.h`](file:///e:/CODES/zbrush%20plugins/baker/src/baker/MeshTypes.h) | Mathematical types (`Vec3`, `UVCoord`, `Vertex`, `Triangle`, `LowPolyMesh`, `HighPolyMesh`). |
| [`src/baker/ExrWriter.h`](file:///e:/CODES/zbrush%20plugins/baker/src/baker/ExrWriter.h) / [`.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/baker/ExrWriter.cpp) | 16-bit / 32-bit float OpenEXR writing via TinyEXR and 8-bit PNG writing via STB Image Write. |
| [`src/bridge/UDIMBakerBridge.cpp`](file:///e:/CODES/zbrush%20plugins/baker/src/bridge/UDIMBakerBridge.cpp) | ZBrush C-ABI export function `LaunchBakerSession`. |
| [`zscript/UDIMBaker.txt`](file:///e:/CODES/zbrush%20plugins/baker/zscript/UDIMBaker.txt) | ZScript frontend creating the `ZPlugin:UDIM Baker` palette. |
| [`deploy_plugin.ps1`](file:///e:/CODES/zbrush%20plugins/baker/deploy_plugin.ps1) | PowerShell deployment script installing the suite into ZBrush 2026. |
