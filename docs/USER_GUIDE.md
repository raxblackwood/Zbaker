# ZBrush UDIM Baker — Artist User Guide & Production Manual

Welcome to the **ZBrush UDIM Baker**, a high-performance hardware-accelerated baking suite designed specifically for baking massive high-resolution digital sculpts (20M–100M+ polygons) onto low-poly game and VFX assets with multi-tile **UDIM layouts**.

---

## Table of Contents
1. [Quick Start Workflow](#1-quick-start-workflow)
2. [Viewport & UV Canvas Navigation](#2-viewport--uv-canvas-navigation)
3. [Understanding UDIM Multi-Tile Baking](#3-understanding-udim-multi-tile-baking)
4. [Projection & Ray-Tracing Settings](#4-projection--ray-tracing-settings)
5. [The 9 Production Output Maps](#5-the-9-production-output-maps)
6. [Handling Instanced & Repeating Meshes](#6-handling-instanced--repeating-meshes)
7. [File Organization & Windows File Explorer Tips](#7-file-organization--windows-file-explorer-tips)
8. [Troubleshooting Common Issues](#8-troubleshooting-common-issues)

---

## 1. Quick Start Workflow

### Launching directly from ZBrush 2026:
1. In ZBrush, load your tool with your high-poly sculpts and low-poly target meshes.
2. Navigate to top menu: **ZPlugin -> UDIM Baker**.
3. Click **Launch UDIM Baker**.
4. The standalone companion application (`UDIMBakerApp.exe`) opens automatically with your sculpt cached and ready.
5. In the **Mesh Ingestion** panel, click **Browse Low-Poly FBX/OBJ** to load your low-poly retopology.
6. Check your desired output maps, set resolution (e.g., 2048 or 4096), and click **START BAKE**.

### Standalone Desktop Mode:
You can also launch `UDIMBakerApp.exe` directly from `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBakerData\UDIMBakerApp.exe` and load any FBX or OBJ files directly from your disk.

---

## 2. Viewport & UV Canvas Navigation

The companion app features a modern docked workspace with both 3D Direct3D 11 rendering and an interactive 2D UV canvas.

### Viewport Controls:
* **Orbit / Rotate**: Left-Click + Drag
* **Pan**: Middle-Click (or Right-Click) + Drag
* **Zoom**: Scroll Wheel
* **Focus on Mesh**: Press the **Focus (Frame All)** button in the viewport HUD to instantly center and frame your entire asset.
* **View Modes (Top Right)**:
  * **3D**: Dedicated full 3D viewport.
  * **2D**: Dedicated full 2D UV tile canvas showing all UDIM grid boundaries and UV wireframes.
  * **Split**: Side-by-side interactive 3D and 2D view.

---

## 3. Understanding UDIM Multi-Tile Baking

### How UDIM Tiles Work:
In standard UV mapping, all UV islands sit within $[0, 1] \times [0, 1]$ (UDIM **1001**).
In modern production pipelines (Substance Painter, Unreal Engine, Maya, Mari), UVs are organized across multiple tiles:
* **UDIM 1001**: $U \in [0, 1), V \in [0, 1)$ (Tile 1, Row 1)
* **UDIM 1002**: $U \in [1, 2), V \in [0, 1)$ (Tile 2, Row 1)
* **UDIM 1003**: $U \in [2, 3), V \in [0, 1)$ (Tile 3, Row 1)
* **UDIM 1011**: $U \in [0, 1), V \in [1, 2)$ (Tile 1, Row 2)

### Spatial Ray Tracing Rule:
> [!IMPORTANT]
> **Rays are cast in 3D world space, NOT in UV space.**
> When the baker projects UDIM 1001, it takes the triangles assigned to tile 1001 and shoots rays at their **exact 3D world coordinates**.
> For UDIM 1002, the baker shoots rays from the triangles assigned to tile 1002 at their 3D world coordinates.
> If your low-poly mesh has geometry on UDIM 1002, **there must be a corresponding high-poly sculpt at that same 3D world position**.

---

## 4. Projection & Ray-Tracing Settings

| Setting | Recommended Default | Description |
| :--- | :--- | :--- |
| **Resolution** | 2048 or 4096 | Output map dimensions per UDIM tile. |
| **Anti-Aliasing (AA)** | **2x2 (4 rays/pixel)** | 1x = Draft (fast), 2x2 = Production standard, 4x4 = Ultra. |
| **Cage Offset** | $0.015 \times \text{Diagonal}$ | Distance the virtual cage envelope is pushed outward along vertex normals. |
| **Max Ray Length** | $0.08 \times \text{Diagonal}$ | Maximum ray search distance. Prevents rays from shooting through entire meshes. |
| **Bidirectional Rays** | **Enabled (Checked)** | Marmoset/xNormal dual-direction rays. Casts inward from cage and outward from surface, picking the closest hit to eliminate missed crests. |
| **Watertight Seams** | **Automatic** | Automatically welds vertex positions across UV seams when computing cage normals, preventing cage splitting. |
| **Isolate Mesh Parts** | Optional | Uses 32-bit Embree geometry masks so rays from one SubTool/stone are blind to adjacent stones. |
| **Discard Backface** | **Enabled (Checked)** | Rejects rays that hit the back side of triangles inside the mesh. |
| **Angle Threshold** | **$90.0^\circ$** | Prevents steep crevice walls and rock fissures from being rejected as grazing backfaces. |
| **Seam Padding** | 16 px | Jump Flooding Algorithm (JFA) dilation that bleeds edge texels into background space. |

---

## 5. The 9 Production Output Maps

1. **Tangent Normal (MikkTSpace)**: World-standard tangent space normal map, fully calibrated for Unreal Engine 5, Unity, and Blender.
2. **World / Object Space Normal**: Absolute XYZ world-space normal vectors.
3. **Displacement Map (32-bit Float EXR)**: True signed surface displacement along the low-poly surface normal.
4. **Ambient Occlusion (AO)**: Cosine-weighted ray-traced ambient occlusion capturing realistic contact shadows.
5. **Cavity Map**: Multi-scale tangent normal divergence map that highlights fine stone fissures, pore details, and micro-grooves.
6. **Curvature Map**: Normalized surface curvature ($0.5$ = flat, $<0.5$ = concave valleys, $>0.5$ = convex ridges).
7. **Thickness Map**: Subsurface scattering / translucency depth map for skin, jade, marble, and wax.
8. **Material ID Map**: Generates unique color IDs from ZBrush PolyGroups or FBX materials.
9. **Base Color (Polypaint)**: Bakes vertex paint directly from the high-poly sculpt to texture.
10. **Diagnostic Map**: Heatmap showing ray coverage (Green = Clean Hit, Red = Missed Ray, Black = Background).

---

## 6. Handling Instanced & Repeating Meshes

A common scenario in game art is **modular or repeating assets** (e.g., a modular stone wall made of 3 stone sections where Stone 3 is an identical copy of Stone 1).

### Why UDIM 1002 was flat in your test:
In your test asset:
* **Low-Poly Wall 1 & Wall 2**: Positioned at $X \in [-3.71, +1.05]$ $\rightarrow$ assigned to **UDIM 1001**.
* **High-Poly Sculpt**: Positioned at $X \in [-3.72, +1.06]$ $\rightarrow$ matches Wall 1 & Wall 2.
* **Low-Poly Wall 3**: Positioned at $X \in [+1.35, +3.51]$ $\rightarrow$ assigned to **UDIM 1002**.
* **High-Poly Sculpt at $X \in [+1.35, +3.51]$**: **Does not exist.** The sculpt only had 2 wall sections!

Because there was no high-poly stone sculpted at $X \in [1.35, 3.51]$, rays from Wall 3 shot into empty air, producing 0 hits and a flat default map.

### How to properly set up instanced meshes:
* **Option A (Modular / Stacked UVs — Best Practice)**:
  If Wall 3 is an instance of Wall 1, keep Wall 3's UVs overlapping Wall 1 in **UDIM 1001** (or offset by exactly 1 UV tile for baking). Then in the game engine, Wall 1 and Wall 3 share the same texture tile.
* **Option B (Unique High-Poly Sculpt)**:
  If you want UDIM 1002 to have unique sculpted details, duplicate and place your high-poly stone wall over Wall 3 at $X \in [1.35, 3.51]$ before exporting to the baker.

---

## 7. File Organization & Windows File Explorer Tips

### Standard Filename Convention:
All exported maps use the industry-standard naming format:
```
<BaseName>_<MapType>.<UDIM>.<ext>
```
Examples:
* `StoneWall_Normal.1001.png`
* `StoneWall_Normal.1002.png`
* `StoneWall_Cavity.1001.png`
* `StoneWall_Cavity.1002.png`

### Why Windows File Explorer sorted 1002 before 1001:
In Windows 11, folders located in `Downloads` (such as `Downloads\temp\bb`) default their view to **"Group by Date Modified" (Today)** sorted by **Date Modified (Descending)**.
Because UDIM 1002 was baked right after UDIM 1001, its files had a timestamp 1-2 seconds newer, causing Windows to display all 1002 files at the top!

### How to see them side-by-side:
1. In Windows File Explorer, click the **"Name"** column header (or right-click empty space -> **Sort by -> Name**).
2. All maps will immediately group side-by-side alphabetically:
   * `StoneWall_Normal.1001.png`
   * `StoneWall_Normal.1002.png`
   * `StoneWall_Cavity.1001.png`
   * `StoneWall_Cavity.1002.png`
3. **Automatic Timestamp Synchronization**: The baker now automatically synchronizes the modification timestamp of all exported files in a batch to the exact same second, preventing Windows from splitting them apart when sorted by Date.

---

## 8. Troubleshooting Common Issues

| Issue | Cause | Solution |
| :--- | :--- | :--- |
| **One UDIM tile is flat / empty** | No high-poly mesh exists at that tile's 3D world coordinates. | Align your high-poly sculpt to cover that low-poly section, or stack UVs on UDIM 1001. |
| **Cracks / seams along UV borders** | Cage split at hard edges or split vertex normals. | Ensure **Watertight Seams** is enabled (automatic in current build). |
| **Rays bleeding between adjacent stones** | Adjacent subtools occluding each other. | Enable **Isolate Mesh Parts** in bake parameters. |
| **Normal map has missing spots on sharp crests** | High-poly crests protrude beyond cage distance. | Ensure **Bidirectional Rays** is enabled (checked by default). |
| **Normal map looks inverted or inverted green channel** | DirectX vs OpenGL normal convention. | Toggle **Flip Y Normal** in output parameters. |
