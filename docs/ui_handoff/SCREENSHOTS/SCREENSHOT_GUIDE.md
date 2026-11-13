# UI Screenshot Capture Guide & State Specifications

## Why Manual Capture is Specified
Implementing a synthetic headless `--ui-gallery` mock runner inside the C++ runtime would require introducing artificial mock meshes, mocking Intel Embree BVH worker threads, synthesizing false hardware ray-tracing passes, and injecting fake modal states into production code.
Because the core project instruction is: **"Do not change any UI behavior in this step"**, manual screen capture preserves total code stability and provides authentic, pixel-accurate captures of DirectX 11 swapchain presentations.

This guide provides the exact specifications, resolutions, and configurations for capturing the 7 required application states.

---

## 1. Resolution Targets
For every state, capture two aspect-ratio variants:
1. **Primary High-Resolution Target**: `1920 x 1080` (Standard Full HD desktop monitor).
2. **Compact Resolution Target**: `1280 x 720` (Standard 720p minimum layout test).

Place all captured PNG files directly in this directory (`docs/ui_handoff/SCREENSHOTS/`).

---

## 2. The 7 Required States & Setup Instructions

### State 1: Empty (Cold Launch)
- **Filenames**:
  - `state_01_empty_1920x1080.png`
  - `state_01_empty_1280x720.png`
- **Application State**:
  - Cold startup with no meshes loaded.
- **Visual Checklist**:
  - Viewport: Slate gray canvas with infinite procedural ground grid visible.
  - Floating Bake Bar (Bottom): Displays amber warning text `"Load meshes in Mesh Ingestion tab"` and disabled button `"BAKE (NO MESH)"`.
  - Scene Outliner (Right Top): Displays empty collection indicator or prompt to load meshes.
  - Right Lower Panel: Active tab is **Mesh Ingestion**, showing empty High-Poly and Low-Poly cards with `"Browse..."` buttons.
  - Bottom Panel: Active tab is **UDIM Layout**, showing empty coordinate grid.
  - Status Bar: Green bullet with `"Ready"`, live FPS indicator on the right.

### State 2: Loading / Ingestion In-Progress
- **Filenames**:
  - `state_02_loading_1920x1080.png`
  - `state_02_loading_1280x720.png`
- **Application State**:
  - High-poly file opened and Embree 3 BVH acceleration structure actively building on worker threads.
- **Visual Checklist**:
  - Scene Outliner: High-Poly card displays amber `StatusPill("Building BVH...", Warn)`.
  - Viewport: Mouse cursor shows background work indicator, camera remains responsive.
  - Status Bar: Amber bullet with text `"Building Intel Embree BVH for 15.8M triangles..."`.
  - Output Log (switch bottom tab to Log): Shows live log entries with timing benchmarks.

### State 3: Loaded (Ready to Bake)
- **Filenames**:
  - `state_03_loaded_1920x1080.png`
  - `state_03_loaded_1280x720.png`
- **Application State**:
  - Both high-poly sculpt and low-poly game mesh fully loaded with watertight normals verified.
- **Visual Checklist**:
  - Viewport: 3D low-poly mesh framed in 3/4 isometric view, wireframe chip toggled ON (`ToggleChip("Wireframe")`), displaying clean quad edges without diagonal splits.
  - Outliner: High-poly showing vertex count (`15,804,962 verts`), low-poly showing UDIM count (`1 UDIM Tile`), green `StatusPill("BVH Ready", Ok)` and `StatusPill("Watertight", Ok)`.
  - Floating Bake Bar: Active with bright Blender orange button `"START BAKE"` and label `"1 UDIM Tile | 2048x2048"`.
  - Bottom Panel: UDIM Layout tab showing active `[1001]` card with low-poly UV polygon preview.
  - Status Bar: Green bullet with `"Ready for High-Poly and Low-Poly mesh ingestion"`.

### State 4: Baking In-Progress
- **Filenames**:
  - `state_04_baking_1920x1080.png`
  - `state_04_baking_1280x720.png`
- **Application State**:
  - Baking triggered and actively executing multi-threaded Embree ray casting across CPU cores.
- **Visual Checklist**:
  - Floating Bake Bar: Morphed into deep orange progress bar filled to approximately 45-50%, with centered text `"Baking Tile 1001... 45%"`.
  - Modal Popup (if visible): Modal dialog titled `"Bake Execution"` with progress bar and active pass status.
  - Viewport: Floating bake button disabled to prevent duplicate execution.
  - Status Bar: Displays active tile and thread status.

### State 5: Results & Verification
- **Filenames**:
  - `state_05_results_1920x1080.png`
  - `state_05_results_1280x720.png`
- **Application State**:
  - Baking successfully concluded, maps saved to disk, and results loaded into viewport preview.
- **Visual Checklist**:
  - Viewport: Low-poly model shaded with tangent-space normal map or MatCap in viewport.
  - Bottom Panel: Output Log tab selected, displaying green completion logs (e.g. `"Bake finished in 18.2s"`, `"Saved normal map: ..."`).
  - Floating Bake Bar: Reverted to idle state ready for subsequent iterations.
  - Status Bar: Green bullet with `"Bake completed successfully"`.

### State 6: Error / Warning Condition
- **Filenames**:
  - `state_06_error_1920x1080.png`
  - `state_06_error_1280x720.png`
- **Application State**:
  - Low-poly mesh loaded without UV coordinates, or high-poly file read error.
- **Visual Checklist**:
  - Scene Outliner: Red `StatusPill("Missing UVs", Err)` or `"File Not Found"`.
  - Right Lower Panel: Red text warning explaining that UV coordinates are required for tangent-space baking.
  - Floating Bake Bar: Disabled with tooltip `"Load both High-Poly sculpt and Low-Poly UV mesh before baking."`.
  - Status Bar: Red bullet with error explanation text.

### State 7: Bake Parameters / Settings Tab
- **Filenames**:
  - `state_07_settings_1920x1080.png`
  - `state_07_settings_1280x720.png`
- **Application State**:
  - Detailed view of all baking parameters and format options.
- **Visual Checklist**:
  - Right Lower Panel: Active tab is **Bake Parameters**.
  - All sections expanded: `"PROJECTION & QUALITY"`, `"OUTPUT MAPS (MARMOSET SUITE)"`, and `"DESTINATION & FORMATS"`.
  - Sliders visible: Cage offset, Max ray length, Skew factor, Seam padding (px).
  - Checkboxes visible: Tangent Normal, Cavity, Curvature, AO, Thickness, Material ID.
  - Dropdown visible: Resolution (`2048`), Anti-Aliasing (`2x2`).
