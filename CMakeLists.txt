cmake_minimum_required(VERSION 3.20)
project(ZBrushUDIMBaker VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Multi-threading and optimizations for MSVC
if(MSVC)
    add_compile_options(/W3 /MP /O2 /EHsc)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS -DNOMINMAX)
endif()

# Global include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/imgui/backends
)

# -------------------------------------------------------------
# 1. ZBrush C-ABI Bridge DLL
# -------------------------------------------------------------
add_library(UDIMBakerBridge SHARED
    src/bridge/BridgeMain.cpp
    src/bridge/ZBrushSDK.h
)

target_include_directories(UDIMBakerBridge PRIVATE
    src/bridge
)

# -------------------------------------------------------------
# 2. Companion Baker Application (Dear ImGui + DX11 + Embree)
# -------------------------------------------------------------
set(IMGUI_SOURCES
    thirdparty/imgui/imgui.cpp
    thirdparty/imgui/imgui_draw.cpp
    thirdparty/imgui/imgui_tables.cpp
    thirdparty/imgui/imgui_widgets.cpp
    thirdparty/imgui/backends/imgui_impl_win32.cpp
    thirdparty/imgui/backends/imgui_impl_dx11.cpp
)

set(BAKER_SOURCES
    src/app/Main.cpp
    src/app/UIManager.cpp
    src/app/ViewportRenderer.cpp
    src/app/MeshLoader.cpp
    src/baker/BakerEngine.cpp
    src/baker/GoZReader.cpp
    src/baker/MikkTSpaceBridge.cpp
    src/baker/ExrWriter.cpp
    src/uv/UVCutter.cpp
    src/uv/UVUnwrapper.cpp
    src/uv/UVPacker.cpp
    include/mikktspace.c
    include/miniz.c
    include/ufbx.c
)

add_executable(UDIMBakerApp WIN32
    ${BAKER_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(UDIMBakerApp PRIVATE
    src/app
    src/baker
    src/uv
)

target_link_libraries(UDIMBakerApp PRIVATE
    d3d11
    dxgi
    d3dcompiler
    comdlg32
    shell32
    ole32
    ${CMAKE_CURRENT_SOURCE_DIR}/include/embree3.lib
)

set(ZBRUSH_DIR "C:/Program Files/Maxon ZBrush 2026")
if(EXISTS "${ZBRUSH_DIR}/embree3.dll")
    add_custom_command(TARGET UDIMBakerApp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZBRUSH_DIR}/embree3.dll"
            $<TARGET_FILE_DIR:UDIMBakerApp>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZBRUSH_DIR}/tbb12.dll"
            $<TARGET_FILE_DIR:UDIMBakerApp>
        COMMENT "Copying ZBrush Embree3 and TBB12 runtime DLLs to output directory"
    )
endif()

# -------------------------------------------------------------
# 3. Standalone Baker Application (For non-ZBrush users)
# -------------------------------------------------------------
set(STANDALONE_SOURCES
    src/app/StandaloneMain.cpp
    src/app/StandaloneUIManager.cpp
    src/app/ViewportRenderer.cpp
    src/app/MeshLoader.cpp
    src/baker/BakerEngine.cpp
    src/baker/GoZReader.cpp
    src/baker/MikkTSpaceBridge.cpp
    src/baker/ExrWriter.cpp
    src/uv/UVCutter.cpp
    src/uv/UVUnwrapper.cpp
    src/uv/UVPacker.cpp
    include/mikktspace.c
    include/miniz.c
    include/ufbx.c
)

add_executable(StandaloneBaker WIN32
    ${STANDALONE_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(StandaloneBaker PRIVATE
    src/app
    src/baker
    src/uv
)

target_link_libraries(StandaloneBaker PRIVATE
    d3d11
    dxgi
    d3dcompiler
    comdlg32
    shell32
    ole32
    ${CMAKE_CURRENT_SOURCE_DIR}/include/embree3.lib
)

# -------------------------------------------------------------
# 4. Standalone Test Executable
# -------------------------------------------------------------
add_executable(TestBaker
    test/TestBaker.cpp
    src/app/MeshLoader.cpp
    src/baker/BakerEngine.cpp
    src/baker/GoZReader.cpp
    src/baker/MikkTSpaceBridge.cpp
    src/baker/ExrWriter.cpp
    src/uv/UVCutter.cpp
    src/uv/UVUnwrapper.cpp
    src/uv/UVPacker.cpp
    include/mikktspace.c
    include/miniz.c
    include/ufbx.c
)

target_include_directories(TestBaker PRIVATE
    src/app
    src/baker
    src/uv
)

target_link_libraries(TestBaker PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/embree3.lib
)

if(EXISTS "${ZBRUSH_DIR}/embree3.dll")
    add_custom_command(TARGET TestBaker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZBRUSH_DIR}/embree3.dll"
            $<TARGET_FILE_DIR:TestBaker>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ZBRUSH_DIR}/tbb12.dll"
            $<TARGET_FILE_DIR:TestBaker>
        COMMENT "Copying ZBrush Embree3 and TBB12 runtime DLLs to test directory"
    )
endif()

add_executable(UserBakeTest
    test/UserBakeTest.cpp
    src/app/MeshLoader.cpp
    src/baker/BakerEngine.cpp
    src/baker/GoZReader.cpp
    src/baker/MikkTSpaceBridge.cpp
    src/baker/ExrWriter.cpp
    include/mikktspace.c
    include/miniz.c
    include/ufbx.c
)

target_include_directories(UserBakeTest PRIVATE
    src/app
    src/baker
)

target_link_libraries(UserBakeTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/embree3.lib
)

# -------------------------------------------------------------
# 5. GoZ Reader Unit Test
# -------------------------------------------------------------
add_executable(TestGoZReader
    test/TestGoZReader.cpp
    src/baker/GoZReader.cpp
)

target_include_directories(TestGoZReader PRIVATE
    src/baker
)



