#pragma once

#include <d3d11.h>
#include <DirectXMath.h>
#include <vector>
#include <string>
#include "../baker/MeshTypes.h"

namespace Baker {

enum class ShadingMode {
    ClayMatcap = 0,     // Substance/ZBrush warm clay
    WaxRed = 1,         // ZBrush classic wax preview
    NormalColor = 2,    // Visualized surface normals
    FlatShaded = 3      // Clean flat shaded
};

class ViewportRenderer {
public:
    ViewportRenderer();
    ~ViewportRenderer();

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Release();

    // Resize offscreen render target to match ImGui viewport window
    void Resize(int width, int height);

    // Update GPU buffers when meshes load or change
    void SetLowPolyMesh(const LowPolyMesh& mesh, float scaleMultiplier);
    void SetHighPolyMesh(const HighPolyMesh& mesh);
    void UpdateCage(const LowPolyMesh& mesh, float cageDistance, float scaleMultiplier);

    // Render 3D scene to offscreen texture
    void Render();

    // Returns the shader resource view to be drawn with ImGui::Image
    ID3D11ShaderResourceView* GetViewportSRV() const { return m_colorSRV; }
    int GetWidth() const { return m_vpWidth; }
    int GetHeight() const { return m_vpHeight; }

    // Camera control
    void HandleMouseInput(float deltaX, float deltaY, int buttonState, float wheelDelta);
    void FrameAll();

    // Transform & Alignment
    void SetLowPolyTransform(const DirectX::XMFLOAT3& translation, const DirectX::XMFLOAT3& rotationDeg, const DirectX::XMFLOAT3& scale);
    DirectX::XMFLOAT3 GetHighPolyCenter() const;
    DirectX::XMFLOAT3 GetLowPolyCenter() const;
    DirectX::XMFLOAT3 GetHighPolyExtents() const;
    DirectX::XMFLOAT3 GetLowPolyExtents() const;

    // Display Toggles & Settings
    bool showHighPoly = true;
    bool showLowPoly = true;
    bool showLowPolyWireframe = true;
    bool showCage = false;
    bool showGrid = true;
    ShadingMode highPolyShading = ShadingMode::ClayMatcap;
    float highPolyOpacity = 1.0f;
    float lowPolyOpacity = 0.85f;
    float cageOpacity = 0.4f;

    DirectX::XMFLOAT3 m_lpTranslation = { 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 m_lpRotation = { 0.0f, 0.0f, 0.0f }; // In degrees
    DirectX::XMFLOAT3 m_lpScale = { 1.0f, 1.0f, 1.0f };

private:
    bool CreateShaders();
    bool CreateRasterizerAndBlendStates();
    void CreateGridBuffers();
    void UpdateConstantBuffer(const DirectX::XMMATRIX& world, const DirectX::XMFLOAT4& color, int mode);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Offscreen render targets
    ID3D11Texture2D* m_colorTex = nullptr;
    ID3D11RenderTargetView* m_colorRTV = nullptr;
    ID3D11ShaderResourceView* m_colorSRV = nullptr;

    ID3D11Texture2D* m_depthTex = nullptr;
    ID3D11DepthStencilView* m_depthDSV = nullptr;

    // Pipeline states
    ID3D11VertexShader* m_vertexShader = nullptr;
    ID3D11PixelShader* m_pixelShader = nullptr;
    ID3D11InputLayout* m_inputLayout = nullptr;
    ID3D11Buffer* m_constantBuffer = nullptr;

    ID3D11RasterizerState* m_solidRS = nullptr;
    ID3D11RasterizerState* m_solidNoCullRS = nullptr;
    ID3D11RasterizerState* m_wireframeRS = nullptr;
    ID3D11DepthStencilState* m_depthState = nullptr;
    ID3D11DepthStencilState* m_depthNoWriteState = nullptr;
    ID3D11DepthStencilState* m_depthBiasState = nullptr;
    ID3D11BlendState* m_alphaBlendState = nullptr;
    ID3D11BlendState* m_opaqueBlendState = nullptr;

    // Mesh GPU Buffers
    struct MeshGPUBuffer {
        ID3D11Buffer* vertexBuffer = nullptr;
        ID3D11Buffer* indexBuffer = nullptr;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;

        void Release() {
            if (vertexBuffer) { vertexBuffer->Release(); vertexBuffer = nullptr; }
            if (indexBuffer) { indexBuffer->Release(); indexBuffer = nullptr; }
            indexCount = 0;
            vertexCount = 0;
        }
    };

    MeshGPUBuffer m_highPolyGPU;
    MeshGPUBuffer m_lowPolyGPU;
    MeshGPUBuffer m_lowPolyWireGPU; // Pure polygon edges (no internal triangulation diagonals!)
    MeshGPUBuffer m_cageGPU;
    MeshGPUBuffer m_cageWireGPU;
    MeshGPUBuffer m_groundPlaneGPU; // Blender-style procedural anti-aliased ground plane

    // Camera State
    DirectX::XMFLOAT3 m_cameraTarget = { 0.0f, 0.0f, 0.0f };
    float m_cameraYaw = 0.785f;     // 45 degrees
    float m_cameraPitch = 0.35f;    // ~20 degrees
    float m_cameraDistance = 50.0f;
    float m_sceneRadius = 20.0f;

    int m_vpWidth = 800;
    int m_vpHeight = 600;

    DirectX::XMFLOAT3 m_sceneMin = { -10.0f, -10.0f, -10.0f };
    DirectX::XMFLOAT3 m_sceneMax = { 10.0f, 10.0f, 10.0f };
    DirectX::XMFLOAT3 m_lowPolyMin = { -10.0f, -10.0f, -10.0f };
    DirectX::XMFLOAT3 m_lowPolyMax = { 10.0f, 10.0f, 10.0f };
    DirectX::XMFLOAT3 m_highPolyMin = { -10.0f, -10.0f, -10.0f };
    DirectX::XMFLOAT3 m_highPolyMax = { 10.0f, 10.0f, 10.0f };

    void FrameBounds(const DirectX::XMFLOAT3& minB, const DirectX::XMFLOAT3& maxB);
    void RecalculateSceneBounds();
};

} // namespace Baker
