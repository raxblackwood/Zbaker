# ZBrush UDIM Baker

> **Hardware-Accelerated UDIM & Fast-Bake Suite for Maxon ZBrush**  
> Powered by Intel Embree 3, Direct3D 11, Dear ImGui Docking, and MikkTSpace.

---

## Overview

**ZBrush UDIM Baker** is an enterprise-grade companion plugin and standalone ray-tracing baker for digital sculptors and game artists. It bridges Maxon ZBrush with a high-performance GPU viewport and multi-core ray-tracing engine, baking multi-million polygon digital sculpts (20M–100M+ triangles) directly onto low-poly production meshes across **multi-tile UDIM UV layouts** in seconds.

### Key Highlights
* **Native ZBrush Integration**: One-click launch from the `ZPlugin:UDIM Baker` palette.
* **Instant 3D/2D Viewport**: Direct3D 11 hardware viewport with Maya/Substance style navigation, split 3D/2D view, and interactive UDIM tile canvas.
* **Modern DCC Dark UI**: Blender Dark themed docked interface (`0x1D1D1D` background, `0xE87D0D` warm orange accents) with interactive 3D viewport HUD, floating bake progress bar, scene outliner, and collapsible parameter cards.
* **Intel Embree 3 Ray Tracing**: Multithreaded scanline projection with subpixel antialiasing (1x, 2x2, 4x4) and closest-distance subpixel depth testing to cleanly handle overlapping UVs and modular pieces.
* **Full Marmoset Map Suite**:
  * 16-bit Tangent Normal (MikkTSpace basis) & World/Object Normal
  * 32-bit Float Displacement (EXR)
  * Cosine-Weighted Ray-Traced Ambient Occlusion (AO)
  * Multi-Scale Normal Divergence Cavity & Curvature Maps
  * Subsurface Translucency / Thickness Map
  * Normalized 32-bit World Position Map
  * Material ID (ZBrush PolyGroups) & Base Color (ZBrush Polypaint)
  * Visual Diagnostic Ray Coverage Map
* **Jump Flooding Dilation (JFA)**: Nearest-texel seam bleed eliminating UV island border seams.

---

## Documentation

Comprehensive documentation has been authored for artists, technical directors, and software engineers:

* 📐 **[System Architecture Specification](file:///e:/CODES/zbrush%20plugins/baker/docs/ARCHITECTURE.md)**: Component boundaries, ZScript C-ABI bridge interop, Embree BVH architecture, MikkTSpace tangent math, closest-distance depth testing, and JFA seam dilation algorithms.
* 🖥️ **[UI Architecture & Design System](file:///e:/CODES/zbrush%20plugins/baker/docs/UI_ARCHITECTURE.md)**: Complete guide to the UI framework, Blender Dark design language, docking hierarchy, 3D viewport integration, and state machine.
* 📦 **[UI Handoff Package](file:///e:/CODES/zbrush%20plugins/baker/docs/ui_handoff/)**: Full widget-by-widget inventory, known layout issues, persistent docking configs, screenshot capture guide, and source code archive.
* 🚀 **[Engineering & Artist Handoff Guide](file:///e:/CODES/zbrush%20plugins/baker/docs/HANDOFF.md)**: Compilation steps, deployment to ZBrush 2026, 3D/2D viewport navigation guide, UDIM tile selection, troubleshooting, and codebase file index.

---

## Quick Start

### 1. Compile & Deploy to ZBrush
Run from PowerShell in the project root:
```powershell
# Automated build and deployment
powershell -ExecutionPolicy Bypass -File .\deploy_plugin.ps1
```

### 2. Using in ZBrush
1. In ZBrush 2026, go to **ZPlugin $\rightarrow$ UDIM Baker**.
2. Click **Launch UDIM Baker**.
3. Load your Low-Poly mesh (OBJ or FBX).
4. Verify your active UDIM tiles in the **UDIM Layout** panel or 2D UV canvas.
5. Click **START HARDWARE-ACCELERATED BAKE**!
