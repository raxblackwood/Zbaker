# ZBrush UDIM & Fast-Bake Suite (Architecture & Implementation Plan)

> **Document Purpose**: This document specifies the complete architecture, system environment, and step-by-step implementation plan for the **ZBrush UDIM Baker** plugin. Any AI assistant or software engineer reading this document can immediately understand the system environment, component boundaries, and implementation steps to build the project without requiring additional context.

---

## 1. Executive Summary & Problem Definition

### 1.1 The Problem
Maxon ZBrush is the global standard for high-resolution digital sculpting (handling 100M+ polygons via a proprietary implicit-quad CPU rasterizer). However, its UV and texture baking workflows are historically constrained:
- ZBrush defaults to standard `0.0 to 1.0` UV tile space.
- Handling modern multi-tile **UDIMs** (e.g., 1001, 1002, 1003...) requires tedious workarounds (e.g., *UV to PolyGroups*, manual subdivision baking per group, and *Multi Map Exporter*).
- Exporting 50M–100M polygon meshes to external baking tools (Substance Painter, Marmoset Toolbag) generates massive file footprints (10–30 GB OBJs) and slows production iterations.

### 1.2 The Solution
A companion plugin and application architecture:
1. **Inside ZBrush**: A lightweight `ZPlugin` button exports/streams the active high-res SubTool(s) via a native C-ABI bridge DLL.
2. **Companion Viewport**: A dedicated, modern GPU window opens with its own 3D viewport and 2D UDIM tile canvas.
3. **Low-Poly Ingestion**: The artist loads their low-poly production mesh with full UDIM coordinates intact.
4. **Hardware-Accelerated Baking**: A fast ray-tracing baker (Intel Embree / GPU DXR) projects details from the high-poly sculpt to low-poly UDIM tiles, outputting 32-bit float EXR displacement and 16-bit tangent-space normal maps in seconds.

---

## 2. Target System Environment Profile (This Machine)

This machine is pre-configured with all necessary toolchains to compile C++ DLLs, CMake projects, and Python utilities:

| Component | Path / Detail |
|---|---|
| **Operating System** | Windows 11 (x64) |
| **ZBrush Installation** | `C:\Program Files\Maxon ZBrush 2026` |
| **ZBrush Executable** | `C:\Program Files\Maxon ZBrush 2026\ZBrush.exe` (v2026, 64-bit PE) |
| **ZBrush Plugin Directory** | `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64` |
| **Embedded ZBrush Libraries** | `embree3.dll` (Intel Embree), `tbb12.dll` (Intel TBB), `python311.dll` |
| **C/C++ Compiler (MSVC)** | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe` |
| **CMake Executable** | `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe` |
| **Windows 10/11 SDK** | `C:\Program Files (x86)\Windows Kits\10` |
| **Python Runtimes** | `C:\Users\raman\AppData\Local\Programs\Python\Python311\python.exe` (Python 3.11) & `C:\Python314\python.exe` |
| **Project Workspace** | `C:\Users\raman\.gemini\antigravity\scratch\zbrush_udim_baker` |

---

## 3. System Architecture Diagram

```
┌────────────────────────────────────────────────────────────────────────┐
│                        ZBRUSH 2026 HOST RUNTIME                        │
│                                                                        │
│   [ZPlugin:UDIM Baker]  ──► [UDIMBaker.zsc] (ZScript Frontend)         │
│                                      │                                 │
│                                      ▼ [FileExecute]                   │
│                       [UDIMBakerBridge.dll] (C-ABI)                    │
└──────────────────────────────────────┬─────────────────────────────────┘
                                       │ High-poly memory cache / MMF
                                       ▼
┌────────────────────────────────────────────────────────────────────────┐
│             COMPANION VIEWPORT & BAKER (UDIMBakerApp.exe)              │
│                                                                        │
│   ┌──────────────────────────────┐    ┌─────────────────────────────┐  │
│   │ 3D Viewport (DirectX 12/DXR) │    │ 2D UDIM Canvas Editor       │  │
│   │ • High-poly display          │    │ • Tiles 1001, 1002, 1003... │  │
│   │ • Low-poly mesh + Cage view  │    │ • Per-tile resolution       │  │
│   │ • Ray length preview         │    │ • Margin / Seam Bleed       │  │
│   └──────────────┬───────────────┘    └──────────────┬──────────────┘  │
│                  └───────────────────┬───────────────┘                 │
│                                      ▼                                 │
│                 ┌────────────────────────────────────────┐             │
│                 │      Ray-Tracing Baking Engine         │             │
│                 │   (Intel Embree 3 / DXR / OptiX)       │             │
│                 │ • 32-bit Float Displacement (EXR)      │             │
│                 │ • 16-bit Tangent Normal (MikkTSpace)   │             │
│                 │ • Cavity, Ambient Occlusion, Curvature │             │
│                 └────────────────────┬───────────────────┘             │
└──────────────────────────────────────┼─────────────────────────────────┘
                                       ▼
            Production Files: `Character_Disp.1001.exr`, `.1002.exr`...
```

---

## 4. Component Breakdown & Protocols

### Component 1: ZScript Frontend (`UDIMBaker.txt` -> `UDIMBaker.zsc`)
- **Location**: `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBaker.zsc`
- **Responsibility**: Adds buttons to `ZPlugin:UDIM Baker`, queries active SubTool metadata, and invokes the C-ABI DLL using `[FileExecute]`.
- **ZScript Skeleton**:
  ```text
  // ZPlugin:UDIM Baker
  [ISubPalette, "ZPlugin:UDIM Baker"]

  [IButton, "ZPlugin:UDIM Baker:Launch Baker", "Open the UDIM Baking Viewport",
      [VarDef, dllPath, "ZStartup/ZPlugs64/UDIMBakerData/UDIMBakerBridge.dll"]
      [VarDef, result, 0]
      // Export current active tool to temp binary stream or call bridge
      [VarSet, result, [FileExecute, dllPath, "LaunchBakerSession", 0]]
  ]
  ```

### Component 2: ZBrush C-ABI Bridge DLL (`UDIMBakerBridge.dll`)
- **Calling Convention**: Standard ZBrush Plugin C-ABI.
- **Function Signature Required by ZBrush**:
  ```cpp
  #define DLL_EXPORT extern "C" __declspec(dllexport)

  DLL_EXPORT float LaunchBakerSession(
      char* pData,             // Buffer containing incoming string/arguments
      double optValue,         // Numerical parameter passed from ZScript
      char* pOutputBuffer,     // Buffer to write return strings to ZScript
      double* optOutputValue   // Optional return numerical status
  ) {
      // 1. Snapshot active tool data or launch companion application
      // 2. Spawn UDIMBakerApp.exe detached or in-process
      // 3. Return 1.0f on success
      return 1.0f;
  }
  ```

### Component 3: Companion Viewport Application (`UDIMBakerApp.exe`)
- **Technology Stack**:
  - **Windowing & GUI**: Dear ImGui + DirectX 11/12 (fast, lightweight, loads instantly).
  - **Mesh Loading**: `tiny_obj_loader.h` and `OpenFBX` / `assimp` for low-poly mesh import with full UV tile preservation.
  - **UDIM Mapping**: Identifies UV coordinates outside $[0, 1]$ and maps them to standard tile indices:
    $$\text{Tile} = 1001 + \lfloor u \rfloor + 10 \times \lfloor v \rfloor$$
  - **Cage Math**: Normal push offset:
    $$V_{\text{cage}} = V_{\text{low}} + \vec{N}_{\text{low}} \times d_{\text{cage}}$$

### Component 4: High-Performance Ray-Tracing Baker
- **Ray Caster**: Intel Embree 3 (using `embree3.dll` already shipped with ZBrush) or DXR compute.
- **Sampling Loop**:
  For each texel $(u, v)$ on a UDIM tile:
  1. Interpolate low-poly world position $P$ and low-poly surface normal $\vec{N}$.
  2. Cast ray from $P + \vec{N} \times d_{\text{cage}}$ inward along $-\vec{N}$.
  3. Find intersection point $P_{\text{high}}$ and normal $\vec{N}_{\text{high}}$ on the high-poly sculpt.
  4. Compute Displacement:
     $$\Delta = \text{dot}(P_{\text{high}} - P, \vec{N})$$
  5. Compute Tangent Normal using **MikkTSpace** (industry standard for Unreal/Unity/Maya).
  6. Write to image buffer and apply post-bake seam dilation/padding.
- **Output Formats**:
  - Displacement: 32-bit floating point `.exr` (via `tinyexr`).
  - Normal Map: 16-bit `.png` or `.tif`.

---

## 5. Directory Structure & Workspace Layout

Create the project workspace at:
`C:\Users\raman\.gemini\antigravity\scratch\zbrush_udim_baker\`

```
zbrush_udim_baker/
├── CMakeLists.txt                  # Root build configuration
├── src/
│   ├── bridge/
│   │   ├── BridgeMain.cpp          # ZBrush [FileExecute] DLL export
│   │   └── ZBrushSDK.h             # Calling convention declarations
│   ├── app/
│   │   ├── Main.cpp                # Companion App entry point
│   │   ├── Viewport3D.cpp          # DirectX 3D render viewport
│   │   ├── ViewportUDIM.cpp        # 2D UDIM grid canvas
│   │   └── UIManager.cpp           # ImGui dockable window layout
│   └── baker/
│       ├── BakerEngine.cpp         # Embree / ray-tracing projection core
│       ├── MikkTSpaceBridge.cpp    # Tangent vector evaluation
│       └── ExrWriter.cpp           # 32-bit float EXR file writer
├── include/
│   ├── tiny_obj_loader.h
│   ├── tinyexr.h
│   └── mikktspace.h
├── zscript/
│   └── UDIMBaker.txt               # Source ZScript
└── build/                          # CMake build artifact directory
```

---

## 6. Step-by-Step Implementation Roadmap

### Phase 1: Bridge & ZScript Foundation (Milestone 1)
- [ ] Configure `CMakeLists.txt` targeting MSVC 14.51 (`cl.exe`).
- [ ] Build `UDIMBakerBridge.dll` implementing `LaunchBakerSession`.
- [ ] Write `UDIMBaker.txt`, test loading in ZBrush via `ZScript -> Load`.
- [ ] Verify `[FileExecute]` calls the DLL and returns status without error.

### Phase 2: Standalone Companion Window & Low-Poly Ingestion (Milestone 2)
- [ ] Create `UDIMBakerApp.exe` using Dear ImGui + DirectX 11/12.
- [ ] Implement Low-Poly FBX/OBJ loader.
- [ ] Implement UDIM Tile detector (detects tiles 1001, 1002, 1003 automatically).
- [ ] Implement 2D UV tile view and 3D preview viewport with cage adjustment slider.

### Phase 3: High-Poly Transfer & Ray-Tracing Bake Core (Milestone 3)
- [ ] Link `embree3.dll` for high-poly BVH construction.
- [ ] Implement inward ray-casting from low-poly cage to high-poly surface.
- [ ] Calculate scalar height delta ($P_{\text{high}} - P_{\text{low}}$) and tangent-space normal vector.
- [ ] Add MikkTSpace integration for seamless tangent vectors.

### Phase 4: Multi-Tile EXR Writer & Seam Bleed (Milestone 4)
- [ ] Integrate `tinyexr.h` for native 32-bit floating point EXR export.
- [ ] Implement edge padding / dilation algorithm across UV border seams.
- [ ] Multi-thread the baking loop across all CPU cores with OpenMP or Intel TBB.
- [ ] Output files formatted as: `<Mesh>_Displacement.<UDIM>.exr`.

### Phase 5: Packaging & Installation to ZStartup (Milestone 5)
- [ ] Compile release binaries:
  - Copy `UDIMBaker.zsc` directly to `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\`.
  - Copy `UDIMBakerBridge.dll` and `UDIMBakerApp.exe` to `C:\Program Files\Maxon ZBrush 2026\ZStartup\ZPlugs64\UDIMBakerData\`.
- [ ] End-to-end verification inside ZBrush 2026 on a multi-million polygon sculpt.

---

## 7. How Any AI Assistant Should Resume This Project

When initiating work in a new chat:
1. Provide this document or reference its location:
   `C:\Users\raman\.gemini\antigravity\scratch\ZBrush_UDIM_Baker_Architecture_Plan.md`
2. Give the exact instruction:
   > *"We are building the ZBrush UDIM Baker project according to the Architecture Plan. Review Section 2 for compiler and tool paths on this system, and proceed with Phase 1 (generating the CMakeLists.txt and ZScript bridge DLL)."*
3. The AI can execute build steps using the verified MSVC compiler and CMake paths declared in Section 2.
