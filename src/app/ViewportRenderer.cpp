#include "ViewportRenderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace Baker {

struct ViewportVertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 norm;
};

struct CBFrame {
    DirectX::XMMATRIX WorldViewProj;
    DirectX::XMMATRIX World;
    DirectX::XMFLOAT4 MeshColor;
    DirectX::XMFLOAT4 LightDir;
    DirectX::XMFLOAT4 CameraPos;
    int ShadingMode; // 0=Clay, 1=Wax, 2=Normal, 3=Flat, 4=Wireframe, 5=Grid
    float Opacity;
    float Padding[2];
};

static const char* g_ShaderSource = R"(
cbuffer CBFrame : register(b0) {
    matrix WorldViewProj;
    matrix World;
    float4 MeshColor;
    float4 LightDir;
    float4 CameraPos;
    int ShadingMode;
    float Opacity;
    float2 Padding;
};

struct VS_INPUT {
    float3 Pos : POSITION;
    float3 Norm : NORMAL;
};

struct PS_INPUT {
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION0;
    float3 WorldNorm : NORMAL0;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    output.PosH = mul(float4(input.Pos, 1.0f), WorldViewProj);
    output.WorldPos = mul(float4(input.Pos, 1.0f), World).xyz;
    output.WorldNorm = mul(float4(input.Norm, 0.0f), World).xyz;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_TARGET {
    // 4: Pure Wireframe / Line
    if (ShadingMode == 4) {
        return float4(MeshColor.rgb, Opacity);
    }

    // 5: Ground Grid
    if (ShadingMode == 5) {
        return float4(MeshColor.rgb, Opacity);
    }

    float3 N = normalize(input.WorldNorm);
    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 L = normalize(LightDir.xyz);

    // 2: Surface Normals Visualizer
    if (ShadingMode == 2) {
        return float4(N * 0.5f + 0.5f, Opacity);
    }

    // Studio Lighting setup
    float keyDiff = saturate(dot(N, L));
    float3 fillDir = normalize(float3(-L.x, 0.3f, -L.z));
    float fillDiff = saturate(dot(N, fillDir)) * 0.3f;
    float3 topAmb = float3(0.24f, 0.25f, 0.28f);
    float3 botAmb = float3(0.10f, 0.10f, 0.12f);
    float3 ambient = lerp(botAmb, topAmb, N.y * 0.5f + 0.5f);

    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(N, H)), 24.0f) * 0.4f;
    float rim = pow(1.0f - saturate(dot(N, V)), 3.0f) * 0.25f;

    float3 baseAlbedo = MeshColor.rgb;

    // 0: Clay Matcap (Substance Painter Sculpt look)
    if (ShadingMode == 0) {
        baseAlbedo = float3(0.76f, 0.70f, 0.63f);
    }
    // 1: ZBrush Classic Wax Red
    else if (ShadingMode == 1) {
        baseAlbedo = float3(0.82f, 0.36f, 0.30f);
        ambient += float3(0.18f, 0.05f, 0.04f); // Subsurface bleed simulation
    }

    float3 finalColor = baseAlbedo * (ambient + keyDiff * 0.75f + fillDiff) + spec + rim * baseAlbedo;
    return float4(finalColor, Opacity);
}
)";

ViewportRenderer::ViewportRenderer() {
}

ViewportRenderer::~ViewportRenderer() {
    Release();
}

void ViewportRenderer::Release() {
    m_highPolyGPU.Release();
    m_lowPolyGPU.Release();
    m_cageGPU.Release();
    m_gridGPU.Release();

    if (m_colorSRV) { m_colorSRV->Release(); m_colorSRV = nullptr; }
    if (m_colorRTV) { m_colorRTV->Release(); m_colorRTV = nullptr; }
    if (m_colorTex) { m_colorTex->Release(); m_colorTex = nullptr; }

    if (m_depthDSV) { m_depthDSV->Release(); m_depthDSV = nullptr; }
    if (m_depthTex) { m_depthTex->Release(); m_depthTex = nullptr; }

    if (m_vertexShader) { m_vertexShader->Release(); m_vertexShader = nullptr; }
    if (m_pixelShader) { m_pixelShader->Release(); m_pixelShader = nullptr; }
    if (m_inputLayout) { m_inputLayout->Release(); m_inputLayout = nullptr; }
    if (m_constantBuffer) { m_constantBuffer->Release(); m_constantBuffer = nullptr; }

    if (m_solidRS) { m_solidRS->Release(); m_solidRS = nullptr; }
    if (m_solidNoCullRS) { m_solidNoCullRS->Release(); m_solidNoCullRS = nullptr; }
    if (m_wireframeRS) { m_wireframeRS->Release(); m_wireframeRS = nullptr; }
    if (m_depthState) { m_depthState->Release(); m_depthState = nullptr; }
    if (m_depthBiasState) { m_depthBiasState->Release(); m_depthBiasState = nullptr; }
    if (m_alphaBlendState) { m_alphaBlendState->Release(); m_alphaBlendState = nullptr; }
    if (m_opaqueBlendState) { m_opaqueBlendState->Release(); m_opaqueBlendState = nullptr; }
}

bool ViewportRenderer::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;

    if (!CreateShaders()) return false;
    if (!CreateRasterizerAndBlendStates()) return false;
    CreateGridBuffers();

    Resize(m_vpWidth, m_vpHeight);
    return true;
}

bool ViewportRenderer::CreateShaders() {
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;

    HRESULT hr = D3DCompile(
        g_ShaderSource, strlen(g_ShaderSource),
        "ViewportShader", nullptr, nullptr,
        "VSMain", "vs_4_0", 0, 0, &vsBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) errorBlob->Release();
        return false;
    }

    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) {
        vsBlob->Release();
        return false;
    }

    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,                            D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ViewportVertex, norm), D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = m_device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
    vsBlob->Release();
    if (FAILED(hr)) return false;

    ID3DBlob* psBlob = nullptr;
    hr = D3DCompile(
        g_ShaderSource, strlen(g_ShaderSource),
        "ViewportShader", nullptr, nullptr,
        "PSMain", "ps_4_0", 0, 0, &psBlob, &errorBlob
    );

    if (FAILED(hr)) {
        if (errorBlob) errorBlob->Release();
        return false;
    }

    hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader);
    psBlob->Release();
    if (FAILED(hr)) return false;

    // Constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(CBFrame);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
    return SUCCEEDED(hr);
}

bool ViewportRenderer::CreateRasterizerAndBlendStates() {
    // 1. Solid Cull Back
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    rd.AntialiasedLineEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_solidRS);

    // 2. Solid No Cull
    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_solidNoCullRS);

    // 3. Wireframe with Depth Bias to prevent z-fighting on low-poly
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthBias = -500;
    rd.SlopeScaledDepthBias = -1.0f;
    m_device->CreateRasterizerState(&rd, &m_wireframeRS);

    // Depth Stencil State
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_device->CreateDepthStencilState(&dsd, &m_depthState);

    // Alpha Blend State (for semi-transparent cage)
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, &m_alphaBlendState);

    // Opaque Blend State
    bd.RenderTarget[0].BlendEnable = FALSE;
    m_device->CreateBlendState(&bd, &m_opaqueBlendState);

    return true;
}

void ViewportRenderer::CreateGridBuffers() {
    std::vector<ViewportVertex> gridVerts;
    std::vector<uint32_t> gridIndices;

    float extent = 50.0f;
    int steps = 20;
    float stepSize = extent * 2.0f / steps;

    for (int i = 0; i <= steps; ++i) {
        float c = -extent + i * stepSize;

        // Line along X
        uint32_t idx = (uint32_t)gridVerts.size();
        gridVerts.push_back({ { -extent, 0.0f, c }, { 0.0f, 1.0f, 0.0f } });
        gridVerts.push_back({ {  extent, 0.0f, c }, { 0.0f, 1.0f, 0.0f } });
        gridIndices.push_back(idx);
        gridIndices.push_back(idx + 1);

        // Line along Z
        idx = (uint32_t)gridVerts.size();
        gridVerts.push_back({ { c, 0.0f, -extent }, { 0.0f, 1.0f, 0.0f } });
        gridVerts.push_back({ { c, 0.0f,  extent }, { 0.0f, 1.0f, 0.0f } });
        gridIndices.push_back(idx);
        gridIndices.push_back(idx + 1);
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(gridVerts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { gridVerts.data() };
    m_device->CreateBuffer(&vbd, &vInit, &m_gridGPU.vertexBuffer);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(gridIndices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { gridIndices.data() };
    m_device->CreateBuffer(&ibd, &iInit, &m_gridGPU.indexBuffer);

    m_gridGPU.indexCount = (uint32_t)gridIndices.size();
}

void ViewportRenderer::Resize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    if (width == m_vpWidth && height == m_vpHeight && m_colorSRV != nullptr) return;

    m_vpWidth = width;
    m_vpHeight = height;

    if (m_colorSRV) { m_colorSRV->Release(); m_colorSRV = nullptr; }
    if (m_colorRTV) { m_colorRTV->Release(); m_colorRTV = nullptr; }
    if (m_colorTex) { m_colorTex->Release(); m_colorTex = nullptr; }
    if (m_depthDSV) { m_depthDSV->Release(); m_depthDSV = nullptr; }
    if (m_depthTex) { m_depthTex->Release(); m_depthTex = nullptr; }

    // Color texture
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = m_vpWidth;
    td.Height = m_vpHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&td, nullptr, &m_colorTex);
    m_device->CreateRenderTargetView(m_colorTex, nullptr, &m_colorRTV);
    m_device->CreateShaderResourceView(m_colorTex, nullptr, &m_colorSRV);

    // Depth buffer
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    m_device->CreateTexture2D(&td, nullptr, &m_depthTex);
    m_device->CreateDepthStencilView(m_depthTex, nullptr, &m_depthDSV);
}

void ViewportRenderer::SetLowPolyMesh(const LowPolyMesh& mesh, float scaleMultiplier) {
    m_lowPolyGPU.Release();
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    float s = scaleMultiplier > 1e-5f ? scaleMultiplier : 1.0f;

    std::vector<ViewportVertex> verts(mesh.vertices.size());
    m_lowPolyMin = { 1e9f, 1e9f, 1e9f };
    m_lowPolyMax = { -1e9f, -1e9f, -1e9f };

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        float px = mesh.vertices[i].position.x * s;
        float py = mesh.vertices[i].position.y * s;
        float pz = mesh.vertices[i].position.z * s;
        verts[i].pos = { px, py, pz };
        verts[i].norm = { mesh.vertices[i].normal.x, mesh.vertices[i].normal.y, mesh.vertices[i].normal.z };

        m_lowPolyMin.x = (std::min)(m_lowPolyMin.x, px);
        m_lowPolyMin.y = (std::min)(m_lowPolyMin.y, py);
        m_lowPolyMin.z = (std::min)(m_lowPolyMin.z, pz);
        m_lowPolyMax.x = (std::max)(m_lowPolyMax.x, px);
        m_lowPolyMax.y = (std::max)(m_lowPolyMax.y, py);
        m_lowPolyMax.z = (std::max)(m_lowPolyMax.z, pz);
    }

    std::vector<uint32_t> indices;
    indices.reserve(mesh.triangles.size() * 3);
    for (const auto& tri : mesh.triangles) {
        indices.push_back(tri.indices[0]);
        indices.push_back(tri.indices[1]);
        indices.push_back(tri.indices[2]);
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { verts.data() };
    m_device->CreateBuffer(&vbd, &vInit, &m_lowPolyGPU.vertexBuffer);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { indices.data() };
    m_device->CreateBuffer(&ibd, &iInit, &m_lowPolyGPU.indexBuffer);

    m_lowPolyGPU.indexCount = (uint32_t)indices.size();
    m_lowPolyGPU.vertexCount = (uint32_t)verts.size();

    FrameAll();
}

void ViewportRenderer::SetHighPolyMesh(const HighPolyMesh& mesh) {
    m_highPolyGPU.Release();
    if (mesh.positions.empty() || mesh.indices.empty()) return;

    size_t vertCount = mesh.positions.size();
    std::vector<ViewportVertex> verts(vertCount);

    m_highPolyMin = { 1e9f, 1e9f, 1e9f };
    m_highPolyMax = { -1e9f, -1e9f, -1e9f };

    bool hasNormals = (mesh.normals.size() == vertCount);
    for (size_t i = 0; i < vertCount; ++i) {
        float px = mesh.positions[i].x;
        float py = mesh.positions[i].y;
        float pz = mesh.positions[i].z;
        verts[i].pos = { px, py, pz };
        if (hasNormals) {
            verts[i].norm = { mesh.normals[i].x, mesh.normals[i].y, mesh.normals[i].z };
        } else {
            verts[i].norm = { 0.0f, 1.0f, 0.0f };
        }

        m_highPolyMin.x = (std::min)(m_highPolyMin.x, px);
        m_highPolyMin.y = (std::min)(m_highPolyMin.y, py);
        m_highPolyMin.z = (std::min)(m_highPolyMin.z, pz);
        m_highPolyMax.x = (std::max)(m_highPolyMax.x, px);
        m_highPolyMax.y = (std::max)(m_highPolyMax.y, py);
        m_highPolyMax.z = (std::max)(m_highPolyMax.z, pz);
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { verts.data() };
    m_device->CreateBuffer(&vbd, &vInit, &m_highPolyGPU.vertexBuffer);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(mesh.indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { mesh.indices.data() };
    m_device->CreateBuffer(&ibd, &iInit, &m_highPolyGPU.indexBuffer);

    m_highPolyGPU.indexCount = (uint32_t)mesh.indices.size();
    m_highPolyGPU.vertexCount = (uint32_t)vertCount;

    FrameAll();
}

void ViewportRenderer::UpdateCage(const LowPolyMesh& mesh, float cageDistance, float scaleMultiplier) {
    m_cageGPU.Release();
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    float s = scaleMultiplier > 1e-5f ? scaleMultiplier : 1.0f;
    bool hasExplicitCage = mesh.HasExplicitCage();

    std::vector<ViewportVertex> verts(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        if (hasExplicitCage) {
            verts[i].pos = {
                mesh.cagePositions[i].x * s,
                mesh.cagePositions[i].y * s,
                mesh.cagePositions[i].z * s
            };
        } else {
            verts[i].pos = {
                (mesh.vertices[i].position.x + mesh.vertices[i].normal.x * cageDistance) * s,
                (mesh.vertices[i].position.y + mesh.vertices[i].normal.y * cageDistance) * s,
                (mesh.vertices[i].position.z + mesh.vertices[i].normal.z * cageDistance) * s
            };
        }
        verts[i].norm = { mesh.vertices[i].normal.x, mesh.vertices[i].normal.y, mesh.vertices[i].normal.z };
    }

    std::vector<uint32_t> indices;
    indices.reserve(mesh.triangles.size() * 3);
    for (const auto& tri : mesh.triangles) {
        indices.push_back(tri.indices[0]);
        indices.push_back(tri.indices[1]);
        indices.push_back(tri.indices[2]);
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { verts.data() };
    m_device->CreateBuffer(&vbd, &vInit, &m_cageGPU.vertexBuffer);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = (UINT)(indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { indices.data() };
    m_device->CreateBuffer(&ibd, &iInit, &m_cageGPU.indexBuffer);

    m_cageGPU.indexCount = (uint32_t)indices.size();
    m_cageGPU.vertexCount = (uint32_t)verts.size();
}

void ViewportRenderer::RecalculateSceneBounds() {
    FrameAll();
}

void ViewportRenderer::FrameBounds(const DirectX::XMFLOAT3& minB, const DirectX::XMFLOAT3& maxB) {
    m_sceneMin = minB;
    m_sceneMax = maxB;
    m_cameraTarget = {
        (minB.x + maxB.x) * 0.5f,
        (minB.y + maxB.y) * 0.5f,
        (minB.z + maxB.z) * 0.5f
    };

    float dx = maxB.x - minB.x;
    float dy = maxB.y - minB.y;
    float dz = maxB.z - minB.z;
    float diag = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (diag < 1e-4f) diag = 2.0f;

    m_sceneRadius = diag * 0.5f;
    // Fit comfortably in viewport with standard 45 FOV
    m_cameraDistance = diag * 1.35f;
    m_cameraPitch = 0.35f;
    m_cameraYaw = 0.785f;
}

void ViewportRenderer::FrameAll() {
    DirectX::XMFLOAT3 bMin = { 1e9f, 1e9f, 1e9f };
    DirectX::XMFLOAT3 bMax = { -1e9f, -1e9f, -1e9f };
    bool hasGeom = false;

    if (m_highPolyGPU.vertexCount > 0 && showHighPoly) {
        bMin.x = (std::min)(bMin.x, m_highPolyMin.x);
        bMin.y = (std::min)(bMin.y, m_highPolyMin.y);
        bMin.z = (std::min)(bMin.z, m_highPolyMin.z);
        bMax.x = (std::max)(bMax.x, m_highPolyMax.x);
        bMax.y = (std::max)(bMax.y, m_highPolyMax.y);
        bMax.z = (std::max)(bMax.z, m_highPolyMax.z);
        hasGeom = true;
    }

    if (m_lowPolyGPU.vertexCount > 0 && (showLowPoly || !hasGeom)) {
        bMin.x = (std::min)(bMin.x, m_lowPolyMin.x);
        bMin.y = (std::min)(bMin.y, m_lowPolyMin.y);
        bMin.z = (std::min)(bMin.z, m_lowPolyMin.z);
        bMax.x = (std::max)(bMax.x, m_lowPolyMax.x);
        bMax.y = (std::max)(bMax.y, m_lowPolyMax.y);
        bMax.z = (std::max)(bMax.z, m_lowPolyMax.z);
        hasGeom = true;
    }

    if (!hasGeom) {
        bMin = { -10.0f, -10.0f, -10.0f };
        bMax = { 10.0f, 10.0f, 10.0f };
    }

    FrameBounds(bMin, bMax);
}

void ViewportRenderer::HandleMouseInput(float deltaX, float deltaY, int buttonState, float wheelDelta) {
    // buttonState: 1 = Left (Orbit), 2 = Middle/Right (Pan), 3 = Alt+Right (Dolly Zoom)
    if (buttonState == 1) {
        // Orbit
        m_cameraYaw += deltaX * 0.007f;
        m_cameraPitch += deltaY * 0.007f;
        m_cameraPitch = std::clamp(m_cameraPitch, -1.50f, 1.50f);
    } else if (buttonState == 2) {
        // Pan: dynamically scaled by distance so it feels effortless at any zoom
        float panSpeed = m_cameraDistance * 0.0016f;
        float cosY = std::cos(m_cameraYaw);
        float sinY = std::sin(m_cameraYaw);

        DirectX::XMVECTOR right = DirectX::XMVectorSet(cosY, 0.0f, -sinY, 0.0f);
        DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        DirectX::XMVECTOR offset = DirectX::XMVectorAdd(
            DirectX::XMVectorScale(right, -deltaX * panSpeed),
            DirectX::XMVectorScale(up, deltaY * panSpeed)
        );

        m_cameraTarget.x += DirectX::XMVectorGetX(offset);
        m_cameraTarget.y += DirectX::XMVectorGetY(offset);
        m_cameraTarget.z += DirectX::XMVectorGetZ(offset);
    } else if (buttonState == 3) {
        // Smooth Drag Zoom (Alt + RMB)
        float zoomSpeed = 0.015f;
        m_cameraDistance *= (1.0f - deltaY * zoomSpeed);
        float minDist = std::max(0.001f, m_sceneRadius * 0.005f);
        float maxDist = std::max(500.0f, m_sceneRadius * 50.0f);
        m_cameraDistance = std::clamp(m_cameraDistance, minDist, maxDist);
    }

    // Scroll Wheel Zoom: Fast, responsive exponential zoom
    if (wheelDelta != 0.0f) {
        // ~20% zoom change per scroll notch
        float zoomFactor = std::pow(0.80f, wheelDelta);
        m_cameraDistance *= zoomFactor;

        float minDist = std::max(0.001f, m_sceneRadius * 0.005f);
        float maxDist = std::max(500.0f, m_sceneRadius * 50.0f);
        m_cameraDistance = std::clamp(m_cameraDistance, minDist, maxDist);
    }
}

void ViewportRenderer::UpdateConstantBuffer(const DirectX::XMMATRIX& world, const DirectX::XMFLOAT4& color, int mode) {
    // Camera Eye calculation
    float cosP = std::cos(m_cameraPitch);
    float sinP = std::sin(m_cameraPitch);
    float cosY = std::cos(m_cameraYaw);
    float sinY = std::sin(m_cameraYaw);

    DirectX::XMFLOAT3 eyePos = {
        m_cameraTarget.x + m_cameraDistance * cosP * sinY,
        m_cameraTarget.y + m_cameraDistance * sinP,
        m_cameraTarget.z + m_cameraDistance * cosP * cosY
    };

    DirectX::XMVECTOR eye = DirectX::XMLoadFloat3(&eyePos);
    DirectX::XMVECTOR target = DirectX::XMLoadFloat3(&m_cameraTarget);
    DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

    DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, target, up);
    float aspect = (float)m_vpWidth / (float)m_vpHeight;
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(45.0f),
        aspect,
        0.05f,
        10000.0f
    );

    DirectX::XMMATRIX wvp = DirectX::XMMatrixMultiply(world, DirectX::XMMatrixMultiply(view, proj));

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m_context->Map(m_constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CBFrame* cb = (CBFrame*)mapped.pData;
        cb->WorldViewProj = DirectX::XMMatrixTranspose(wvp);
        cb->World = DirectX::XMMatrixTranspose(world);
        cb->MeshColor = color;
        cb->LightDir = { 0.45f, 0.85f, 0.25f, 0.0f }; // Studio key light
        cb->CameraPos = { eyePos.x, eyePos.y, eyePos.z, 1.0f };
        cb->ShadingMode = mode;
        cb->Opacity = (mode == 4 ? 0.9f : (mode == 5 ? 0.35f : color.w));
        m_context->Unmap(m_constantBuffer, 0);
    }
}

void ViewportRenderer::SetLowPolyTransform(const DirectX::XMFLOAT3& translation, const DirectX::XMFLOAT3& rotationDeg, const DirectX::XMFLOAT3& scale) {
    m_lpTranslation = translation;
    m_lpRotation = rotationDeg;
    m_lpScale = scale;
}

DirectX::XMFLOAT3 ViewportRenderer::GetHighPolyCenter() const {
    return {
        (m_highPolyMin.x + m_highPolyMax.x) * 0.5f,
        (m_highPolyMin.y + m_highPolyMax.y) * 0.5f,
        (m_highPolyMin.z + m_highPolyMax.z) * 0.5f
    };
}

DirectX::XMFLOAT3 ViewportRenderer::GetLowPolyCenter() const {
    return {
        (m_lowPolyMin.x + m_lowPolyMax.x) * 0.5f,
        (m_lowPolyMin.y + m_lowPolyMax.y) * 0.5f,
        (m_lowPolyMin.z + m_lowPolyMax.z) * 0.5f
    };
}

DirectX::XMFLOAT3 ViewportRenderer::GetHighPolyExtents() const {
    return {
        m_highPolyMax.x - m_highPolyMin.x,
        m_highPolyMax.y - m_highPolyMin.y,
        m_highPolyMax.z - m_highPolyMin.z
    };
}

DirectX::XMFLOAT3 ViewportRenderer::GetLowPolyExtents() const {
    return {
        m_lowPolyMax.x - m_lowPolyMin.x,
        m_lowPolyMax.y - m_lowPolyMin.y,
        m_lowPolyMax.z - m_lowPolyMin.z
    };
}

void ViewportRenderer::Render() {
    if (!m_colorRTV || !m_depthDSV) return;

    // Viewport setup
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)m_vpWidth;
    vp.Height = (float)m_vpHeight;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    // Blender 4.x neutral dark gray viewport background (#303030)
    const float clearColor[4] = { 0.19f, 0.19f, 0.19f, 1.0f };
    m_context->ClearRenderTargetView(m_colorRTV, clearColor);
    m_context->ClearDepthStencilView(m_depthDSV, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    m_context->OMSetRenderTargets(1, &m_colorRTV, m_depthDSV);
    m_context->OMSetDepthStencilState(m_depthState, 0);

    m_context->IASetInputLayout(m_inputLayout);
    m_context->VSSetShader(m_vertexShader, nullptr, 0);
    m_context->PSSetShader(m_pixelShader, nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, &m_constantBuffer);
    m_context->PSSetConstantBuffers(0, 1, &m_constantBuffer);

    DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();

    // Compute Low-Poly and Cage world transform matrix (Scale -> Rotate -> Translate)
    DirectX::XMMATRIX matScale = DirectX::XMMatrixScaling(m_lpScale.x, m_lpScale.y, m_lpScale.z);
    DirectX::XMMATRIX matRot = DirectX::XMMatrixRotationRollPitchYaw(
        DirectX::XMConvertToRadians(m_lpRotation.x),
        DirectX::XMConvertToRadians(m_lpRotation.y),
        DirectX::XMConvertToRadians(m_lpRotation.z)
    );
    DirectX::XMMATRIX matTrans = DirectX::XMMatrixTranslation(m_lpTranslation.x, m_lpTranslation.y, m_lpTranslation.z);
    DirectX::XMMATRIX worldLP = DirectX::XMMatrixMultiply(DirectX::XMMatrixMultiply(matScale, matRot), matTrans);

    UINT stride = sizeof(ViewportVertex);
    UINT offset = 0;

    // 1. Render Ground Grid (Blender subtle grid)
    if (showGrid && m_gridGPU.vertexBuffer) {
        m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidNoCullRS);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);

        UpdateConstantBuffer(identity, { 0.26f, 0.26f, 0.26f, 0.40f }, 5);
        m_context->IASetVertexBuffers(0, 1, &m_gridGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_gridGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_gridGPU.indexCount, 0, 0);
    }

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 2. Render High-Poly Sculpt
    if (showHighPoly && m_highPolyGPU.vertexBuffer) {
        m_context->OMSetBlendState(m_opaqueBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidRS);

        int mode = (int)highPolyShading;
        DirectX::XMFLOAT4 hpColor = { 0.82f, 0.77f, 0.70f, highPolyOpacity };
        UpdateConstantBuffer(identity, hpColor, mode);

        m_context->IASetVertexBuffers(0, 1, &m_highPolyGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_highPolyGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_highPolyGPU.indexCount, 0, 0);
    }

    // 3. Render Low-Poly Mesh
    if (showLowPoly && m_lowPolyGPU.vertexBuffer) {
        // Shaded surface
        m_context->OMSetBlendState(showHighPoly ? m_alphaBlendState : m_opaqueBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidNoCullRS);

        DirectX::XMFLOAT4 lpColor = { 0.65f, 0.63f, 0.58f, lowPolyOpacity };
        UpdateConstantBuffer(worldLP, lpColor, 3); // Warm graphite/clay

        m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_lowPolyGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_lowPolyGPU.indexCount, 0, 0);

        // Wireframe overlay
        if (showLowPolyWireframe) {
            m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
            m_context->RSSetState(m_wireframeRS);

            DirectX::XMFLOAT4 wireColor = { 0.10f, 0.12f, 0.15f, 0.90f };
            UpdateConstantBuffer(worldLP, wireColor, 4);

            m_context->DrawIndexed(m_lowPolyGPU.indexCount, 0, 0);
        }
    }

    // 4. Render Cage Shell (Marmoset Toolbag Warm Amber Cage)
    if (showCage && m_cageGPU.vertexBuffer) {
        // Semi-transparent surface
        m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidNoCullRS);

        DirectX::XMFLOAT4 cageColor = { 1.0f, 0.52f, 0.12f, cageOpacity };
        UpdateConstantBuffer(worldLP, cageColor, 3);

        m_context->IASetVertexBuffers(0, 1, &m_cageGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_cageGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_cageGPU.indexCount, 0, 0);

        // Cage wireframe
        m_context->RSSetState(m_wireframeRS);
        DirectX::XMFLOAT4 cageWire = { 1.0f, 0.68f, 0.22f, 0.92f };
        UpdateConstantBuffer(worldLP, cageWire, 4);
        m_context->DrawIndexed(m_cageGPU.indexCount, 0, 0);
    }
}

} // namespace Baker
