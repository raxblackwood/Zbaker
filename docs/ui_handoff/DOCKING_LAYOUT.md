# Docking Hierarchy & Layout Configuration

## 1. Docking Tree Overview
The application uses the Dear ImGui docking branch with a programmatic default layout builder defined in `BakerUI::BuildDefaultLayout`.
Persistent layout geometry is stored in `imgui.ini` (configured via `io.IniFilename = "baker_layout.ini"` in the companion app, and `standalone_baker_layout.ini` in standalone mode).

### Docking Architecture Diagram
```text
+---------------------------------------------------------------------------------------------+
| Main Menu Bar (File, View, Theme, Help)                                                     |
+----------------------------------------------------+----------------------------------------+
|                                                    | Scene Outliner (ID: 0x05, 30% width)   |
| Viewport (ID: 0x03, Central Node, No Tab Bar)      |                                        |
| - 3D Direct3D 11 Render Target Canvas              +----------------------------------------+
| - Top Overlay Toolbar (Wireframe, Cage, Normal...) | Bake Parameters (ID: 0x06, Tab 1)      |
| - Floating Bake Bar (Bottom Center)                | Mesh Ingestion  (ID: 0x06, Tab 2)      |
|                                                    |                                        |
+----------------------------------------------------+                                        |
| UDIM Layout (ID: 0x04, Tab 1)                      |                                        |
| Output Log  (ID: 0x04, Tab 2)                      |                                        |
+----------------------------------------------------+----------------------------------------+
| Status Bar (Status indicator, message text, FPS counter)                                    |
+---------------------------------------------------------------------------------------------+
```

---

## 2. Programmatic Layout Splits (`BakerUI.h`)
When no existing `.ini` configuration is found or when the user invokes `View -> Reset Layout`, the layout is constructed dynamically:
1. **Central Root Node (`BakerDockSpace`)**:
   - Initialized to fill the entire application workspace between the main menu bar and the bottom status bar.
2. **Right Split (0.30 ratio / 30% screen width)**:
   - Root is split horizontally: Right column gets 30%, Center retains 70%.
3. **Bottom Split (0.26 ratio / 26% screen height)**:
   - The remaining center node is split vertically: Bottom region gets 26%, Viewport retains 74%.
4. **Right-Column Vertical Split (0.58 ratio / 58% lower height)**:
   - The right column is split vertically: Top panel gets 42% height (`Scene Outliner`), Bottom panel gets 58% height (`Bake Parameters` tabbed together with `Mesh Ingestion`).
5. **No Tab Bar Flag on Central Viewport**:
   - `ImGuiDockNodeFlags_NoTabBar` is explicitly applied to the viewport node so the 3D canvas feels like a dedicated viewport rather than a document tab.

---

## 3. Persistent Layout File (`imgui.ini`)
The copied file `docs/ui_handoff/imgui.ini` defines the exact saved window coordinates and dock IDs:
- **`[Window][outliner]`**: Pos=(881, 28), Size=(379, 293), DockId=`0x00000005`.
- **`[Window][params]`**: Pos=(881, 323), Size=(379, 404), DockId=`0x00000006`, TabIndex=1.
- **`[Window][ingest]`**: Pos=(881, 323), Size=(379, 404), DockId=`0x00000006`, TabIndex=0.
- **`[Window][viewport]`**: Pos=(4, 28), Size=(875, 507), DockId=`0x00000003` (Central node).
- **`[Window][udim]`**: Pos=(4, 537), Size=(875, 190), DockId=`0x00000004`, TabIndex=0.
- **`[Window][log]`**: Pos=(4, 537), Size=(875, 190), DockId=`0x00000004`, TabIndex=1.
