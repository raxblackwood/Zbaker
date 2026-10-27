# Command Inventory & Placement Specification

**Document**: `docs/ui_handoff/COMMAND_INVENTORY.md`  
**Status**: Authoritative Single Source of Truth  
**Target Architecture**: UV Tool UI Redesign (Claude Architecture 2026-10-27)  

---

## 1. Architectural Contract & Placement Rules

Every command in the application must belong to the central `CommandRegistry` (`src/app/CommandRegistry.h`).
A command declares its intended **target surfaces** using the `Surface` bitmask (`Menu`, `Header`, `Toolkit`, `Marking`, `Help`).

### Core Rules
- **Rule R-1 (One Home)**: A command appears in at most one of {Tool header, UV Toolkit}.
  Menu, hotkey, and marking menu mirrors are allowed.
- **Rule R-2 (State Shown Once)**: Selection mode is shown only in the header.
  Element counts and statistics are shown only in the status bar.
- **Rule R-3 (Single Primary Action)**: At most one primary (filled accent) button is visible per panel.
  In the UV workspace, this is strictly the pinned Auto UV footer (`Auto UV: Cut, Unwrap, Pack`).
- **Rule R-4 (Standard Labels)**: Labels are Title Case, plain language, and never use vendor names or ALL CAPS.
- **Rule R-5 (Registry Enforcement)**: No UI button may be rendered without a corresponding `CommandRegistry` ID.
- **Rule R-6 (Direct Access)**: High-frequency tools (Cut, Sew, Unwrap, Pack) must never be hidden behind obscure nested submenus.

---

## 2. Complete Command Registry Inventory

| Command ID | Label | Target Surfaces | Hotkey | Tooltip | Current Physical Locations |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `file.new_session` | New Session (Clear Scene) | `Menu` | `Ctrl+N` | Clear active meshes, BVH, and baked maps | Main Menu (File) |
| `file.open_low_poly` | Open Low-Poly... | `Menu` | `Ctrl+O` | Open low-poly target mesh (OBJ, FBX) | Main Menu (File), Ingestion Tab |
| `file.open_high_poly` | Open High-Poly... | `Menu` | `Ctrl+Shift+O` | Open high-poly sculpt (OBJ, FBX, BIN) | Main Menu (File), Ingestion Tab |
| `file.open_cage` | Open Cage... | `Menu` | | Open explicit morph cage mesh | Main Menu (File), Ingestion Tab, Outliner |
| `file.export_low_poly` | Export UV Low-Poly OBJ... | `Menu \| Toolkit` | `Ctrl+E` | Export unwrapped low-poly OBJ with UVs | Main Menu (File), UV Panel (Export) |
| `file.load_zbrush_cache` | Load ZBrush Cache | `Menu` | | Scan for active ZBrush GoZ cache files | Main Menu (File) |
| `file.exit` | Exit | `Menu` | `Alt+F4` | Close and exit ZBrush UDIM Baker | Main Menu (File) |
| `edit.undo` | Undo | `Menu` | `Ctrl+Z` | Undo last mesh, seam, or curve action | 3D Overlay Button, Main Menu |
| `edit.redo` | Redo | `Menu` | `Ctrl+Y` | Redo previously undone action | 3D Overlay Button, Main Menu |
| `select.mode_vertex` | Vertex | `Menu \| Header` | `1` | Vertex Selection Mode | 2D UV Header (Tier 1), Hint Strip |
| `select.mode_edge` | Edge | `Menu \| Header` | `2` | Edge Selection Mode | 2D UV Header (Tier 1), Hint Strip |
| `select.mode_face` | Face | `Menu \| Header` | `3` | Face Selection Mode | 2D UV Header (Tier 1), Hint Strip |
| `select.mode_shell` | Shell | `Menu \| Header` | `4` | UV Shell Selection Mode | 2D UV Header (Tier 1), Hint Strip |
| `select.edge_loop` | Select Edge Loop | `Menu \| Marking` | `Alt+Double-LMB` | Select continuous edge loop | Marking Menu (East), Main Menu |
| `select.edge_ring` | Select Edge Ring | `Menu \| Marking` | `Alt+Shift+LMB` | Select parallel edge ring across quads | Marking Menu (NE), Main Menu |
| `select.clear` | Clear Selection | `Menu` | `Esc` | Deselect all components | 2D UV Header ("Clear Sel"), Outliner ("Clear") |
| `tool.select` | Select Tool | `Header` | `Q` | Selection Box Tool without gizmo handles | 2D UV Canvas (Implicit default) |
| `tool.move` | Move Tool | `Header` | `W` | Move Gizmo Tool (Translate UVs) | 2D UV Header (Tier 1) |
| `tool.rotate` | Rotate Tool | `Header` | `E` | Rotate Gizmo Tool (Rotate UVs) | 2D UV Header (Tier 1) |
| `tool.scale` | Scale Tool | `Header` | `R` | Scale Gizmo Tool (Scale UVs) | 2D UV Header (Tier 1) |
| `view.layout_3d` | 3D View | `Menu \| Header` | | Switch to 3D Viewport Only | 3D Floating Bar, Main Menu |
| `view.layout_split` | Split 3D \| 2D | `Menu \| Header` | | Switch to Split 3D and 2D Viewports | 3D Floating Bar, Main Menu |
| `view.layout_2d` | 2D UV | `Menu \| Header` | | Switch to 2D UV Viewport Only | 3D Floating Bar, Main Menu |
| `view.frame_all` | Frame All | `Menu` | `Home` | Fit and frame entire mesh and UV canvas in view | 3D Viewport ("Focus (F)"), 2D UV Header ("Frame (F)") |
| `view.frame_selected` | Frame Selected | `Menu` | `F` | Fit and frame selected components in view | 2D UV Header ("Frame (F)") |
| `view.reset_view` | Reset View | `Menu` | | Reset 2D UV zoom to 1.0x and pan to center | 2D UV Header ("Reset View"), Bottom Canvas |
| `view.3d_low_poly` | Low-Poly Mesh | `Menu \| Header` | | Toggle Low-Poly 3D mesh rendering | 3D Top Overlay, Outliner Eye |
| `view.3d_wireframe` | Wireframe | `Menu \| Header` | | Toggle quad wireframe display | 3D Top Overlay |
| `view.3d_seams` | UV Seams | `Menu \| Header` | | Toggle 3D seam lines visualization | 3D Top Overlay |
| `view.3d_grid` | Ground Grid | `Menu \| Header` | | Toggle 3D ground reference grid | 3D Top Overlay |
| `view.3d_high_poly` | High-Poly Sculpt | `Menu \| Header` | | Toggle high-poly sculpt visibility | 3D Top Overlay, Outliner Eye |
| `view.uv_borders` | Texture Borders | `Menu \| Header` | | Toggle UV texture border / seam highlighting | 2D UV Header (Tier 1) |
| `view.uv_shells` | Shell Shading | `Menu \| Header` | | Toggle pastel color coding for each UV island | 2D UV Header (Tier 1) |
| `view.uv_checker` | Checker Grid | `Menu \| Header` | | Toggle 16x16 UV checker background grid | 2D UV Header (Tier 1) |
| `view.uv_distortion` | Distortion Heatmap | `Menu \| Header` | | Toggle area stretch and compression feedback | 2D UV Header (Tier 1) |
| `view.uv_wireframe` | UV Wireframe | `Menu \| Header` | | Toggle UV polygon wireframe display | 2D UV Header (Tier 1) |
| `uv.cut` | Cut Seam | `Menu \| Toolkit \| Marking` | `Shift+X` | Cut UV seams along selected edges | 2D UV Header (Tier 2), UV Tool Panel, Marking Menu |
| `uv.sew` | Sew Seam | `Menu \| Toolkit \| Marking` | `Shift+V` | Sew and stitch selected seam edges together | 2D UV Header (Tier 2), UV Tool Panel, Marking Menu |
| `uv.clear_seams` | Clear All Seams | `Menu \| Toolkit \| Marking` | | Clear all marked UV seam lines on mesh | UV Tool Panel, Marking Menu |
| `uv.auto_seams` | Auto Seams | `Menu \| Toolkit` | | Mark boundary seams by dihedral angle threshold | UV Tool Panel |
| `uv.curve_cut` | Curve Cut | `Menu \| Toolkit` | | Interactive 3D surface curve cutting | UV Tool Panel (3D Surface Curves) |
| `uv.unwrap` | Unwrap | `Menu \| Toolkit \| Marking` | `Ctrl+U` | Unwrap islands using conformal least-squares mapping | 2D UV Header (Tier 2), UV Tool Panel, Marking Menu |
| `uv.relax` | Relax | `Menu \| Toolkit` | | Optimize UV shell stretch iteratively | UV Tool Panel (Spring Iterations) |
| `uv.flip_u` | Flip U | `Menu \| Toolkit` | | Flip selected UVs horizontally along U-axis | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.flip_v` | Flip V | `Menu \| Toolkit` | | Flip selected UVs vertically along V-axis | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.rot_ccw` | Rotate -90° | `Menu \| Toolkit` | | Rotate selected UVs 90 degrees CCW | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.rot_cw` | Rotate +90° | `Menu \| Toolkit` | | Rotate selected UVs 90 degrees CW | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.straighten` | Straighten Shells | `Menu \| Toolkit` | | Align and straighten UV island boundaries | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.orient` | Orient to World Up | `Menu \| Toolkit` | | Rotate islands to align with 3D world up axis | UV Tool Panel |
| `uv.stack_identical` | Stack Identical Shells | `Menu \| Toolkit` | | Stack UV islands with matching geometry and area | UV Tool Panel |
| `uv.separate_overlaps` | Separate Overlaps | `Menu \| Toolkit` | | Offset overlapping islands by +1 tile | UV Tool Panel |
| `uv.td_get` | Pick Texel Density | `Menu \| Toolkit` | | Measure texel density from selected UV faces | UV Tool Panel |
| `uv.td_set` | Set Texel Density | `Menu \| Toolkit` | | Scale islands to match target px/cm | UV Tool Panel |
| `uv.td_equalize` | Equalize Texel Density | `Menu \| Toolkit` | | Equalize average texel density across all islands | UV Tool Panel |
| `uv.pack` | Pack Islands | `Menu \| Toolkit` | `Ctrl+P` | Pack UV islands across UDIM tiles | 2D UV Header (Tier 2), UV Tool Panel |
| `uv.auto_cut_unwrap_pack` | Auto UV: Cut, Unwrap, Pack | `Menu \| Toolkit \| Marking` | | Partition into charts, unwrap, and pack into UDIMs | 2D UV Header ("Unwrap"), UV Tool Panel, Marking Menu |

---

## 3. Current Duplication Audit Table

The following table explicitly audits every duplicated command currently active in the codebase and defines its target removal step under the swapped implementation plan:

| Command ID | Current Duplicated Locations | Authoritative Target Home | Removal Step |
| :--- | :--- | :--- | :--- |
| `uv.cut` | 2D UV Canvas Header Tier 2, UV Tool Panel, Marking Menu | UV Toolkit (`Cut & Sew`), UV Menu, Marking Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.sew` | 2D UV Canvas Header Tier 2, UV Tool Panel, Marking Menu | UV Toolkit (`Cut & Sew`), UV Menu, Marking Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.unwrap` | 2D UV Canvas Header Tier 2, UV Tool Panel, Marking Menu | UV Toolkit (`Unfold`), UV Menu, Marking Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.pack` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Pack`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.flip_u` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Transform`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.flip_v` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Transform`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.rot_ccw` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Transform`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.rot_cw` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Transform`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `uv.straighten` | 2D UV Canvas Header Tier 2, UV Tool Panel | UV Toolkit (`Transform`), UV Menu | Phase 1 Step 2 (Remove from UV Header) |
| `view.frame_all` | 3D Viewport Top ("Focus (F)"), 2D UV Header ("Frame (F)") | View Menu (`Frame All`), Hotkey `Home` | Phase 1 Step 2 (Remove Header Buttons) |
| `view.frame_selected` | 2D UV Canvas Header ("Frame (F)") | View Menu (`Frame Selected`), Hotkey `F` | Phase 1 Step 2 (Remove Header Button) |
| `view.reset_view` | 3D Viewport ("Reset Cam"), 2D UV Header ("Reset View") | View Menu (`Reset View`), Hotkey `Home` | Phase 1 Step 2 (Remove Header Buttons) |
| `select.clear` | 2D UV Canvas Header ("Clear Sel"), Outliner ("Clear") | Select Menu (`Clear Selection`), Hotkey `Esc` | Phase 1 Step 2 (Remove Header Button) |
| `edit.undo` | 3D Viewport Overlay button, Hotkey `Ctrl+Z` | Edit Menu (`Undo`), Hotkey `Ctrl+Z` | Phase 1 Step 2 (Remove Overlay Button) |
| `edit.redo` | 3D Viewport Overlay button, Hotkey `Ctrl+Y` | Edit Menu (`Redo`), Hotkey `Ctrl+Y` | Phase 1 Step 2 (Remove Overlay Button) |

---

## 4. Swapped Execution Plan (Phases 1 & 2)

Per user approval and amendment (1):
1. **Phase 1 Step 1 (Build Homes)**:
   - Complete top main menu bar items: `File`, `Edit`, `Select`, `UV`, `View`, `Help` generated from `CommandRegistry`.
   - Build complete reorganized `UV Toolkit` accordion groups (`Cut & Sew`, `Unfold`, `Transform`, `Stack & Overlap`, `Texel Density`, `Pack`, `Export`) and the pinned `Auto UV` footer button.
   - All buttons in new menus and toolkit rendered exclusively through `DrawCommand(id)`.
2. **Phase 1 Step 2 (Safely Remove Duplicates)**:
   - With the new menus and UV Toolkit groups 100% functional and verified, remove the duplicate buttons from the 2D UV canvas header and 3D overlay.
   - Artists never experience missing functionality during transition.
