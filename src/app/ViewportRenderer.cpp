#include "ViewportRenderer.h"
#include "MeshLoader.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>

namespace Baker {

struct ViewportVertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 norm;
    DirectX::XMFLOAT2 uv;
    DirectX::XMFLOAT3 tangent;
    DirectX::XMFLOAT3 bitangent;
    DirectX::XMFLOAT4 color;
};

struct CBFrame {
    DirectX::XMMATRIX WorldViewProj;
    DirectX::XMMATRIX World;
    DirectX::XMFLOAT4 MeshColor;
    DirectX::XMFLOAT4 LightDir;
    DirectX::XMFLOAT4 CameraPos;
    int ShadingMode; // 0=Clay, 1=Wax, 2=Normal, 3=Flat, 4=Wireframe, 5=Grid, 6=SkewHeatmap
    float Opacity;
    int HasNormalMap; // 1 if previewing normal map, 0 otherwise
    float GridScale;  // Dynamic grid LOD reference
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
    int HasNormalMap;
    float GridScale;
};

Texture2D NormalMap : register(t0);
SamplerState SamplerLinear : register(s0);

struct VS_INPUT {
    float3 Pos : POSITION;
    float3 Norm : NORMAL;
    float2 UV : TEXCOORD0;
    float3 Tangent : TANGENT;
    float3 Bitangent : BINORMAL;
    float4 Color : COLOR0;
};

struct PS_INPUT {
    float4 PosH : SV_POSITION;
    float3 WorldPos : POSITION0;
    float3 WorldNorm : NORMAL0;
    float2 UV : TEXCOORD0;
    float3 WorldTangent : TANGENT0;
    float3 WorldBitangent : BINORMAL0;
    float4 Color : COLOR0;
};

PS_INPUT VSMain(VS_INPUT input) {
    PS_INPUT output;
    float3 pos = input.Pos;
    // For wireframe / seams (mode 4):
    // 1. Geometric outward normal offset so line sits directly on the outer mesh boundary
    if (ShadingMode == 4) {
        pos += input.Norm * 0.0004f;
    }
    output.PosH = mul(float4(pos, 1.0f), WorldViewProj);
    // 2. Perspective-safe NDC depth bias towards camera (ensures line never fails depth test against solid mesh)
    if (ShadingMode == 4) {
        output.PosH.z -= 0.00030f * output.PosH.w;
    }
    output.WorldPos = mul(float4(input.Pos, 1.0f), World).xyz;
    output.WorldNorm = mul(float4(input.Norm, 0.0f), World).xyz;
    output.UV = input.UV;
    output.WorldTangent = mul(float4(input.Tangent, 0.0f), World).xyz;
    output.WorldBitangent = mul(float4(input.Bitangent, 0.0f), World).xyz;
    output.Color = input.Color;
    return output;
}

float4 PSMain(PS_INPUT input) : SV_TARGET {
    // 4: Pure Wireframe / Line
    if (ShadingMode == 4) {
        return float4(MeshColor.rgb, Opacity);
    }

    // 5: Blender Procedural Dynamic Anti-Aliased Ground Grid & Colored Axes
    // 5: Blender Procedural Dynamic Anti-Aliased Ground Grid & Colored Axes
    if (ShadingMode == 5) {
        float2 coord = input.WorldPos.xz;
        float3 viewVec = input.WorldPos - CameraPos.xyz;
        float distToCam = length(viewVec);
        float3 viewDir = viewVec / max(distToCam, 0.0001f);

        // Blender/Maya clean unit grid with dynamic scale:
        float unit = 1.0f;
        if (GridScale > 500.0f) unit = 50.0f;
        else if (GridScale > 50.0f) unit = 5.0f;
        else if (GridScale < 0.5f) unit = 0.1f;

        float2 dCoord = max(fwidth(coord), 0.000001f);

        // Minor Grid (1 unit) with analytic smoothstep anti-aliasing (1.5px line width)
        float2 uvMinor = coord / unit;
        float2 fwMinor = max(fwidth(uvMinor), 0.000001f);
        float2 halfWMinor = fwMinor * 0.75f;
        float2 distMinor = abs(frac(uvMinor - 0.5f) - 0.5f);
        float2 aaMinor = 1.0f - smoothstep(halfWMinor * 0.4f, halfWMinor * 1.6f, distMinor);
        float lineMinor = max(aaMinor.x, aaMinor.y);
        // Nyquist anti-aliasing fade: as pixel size approaches cell size, smoothly dissolve to prevent moire
        float fadeMinor = saturate(1.0f - (max(fwMinor.x, fwMinor.y) - 0.08f) / 0.35f);
        lineMinor *= fadeMinor;

        // Major Grid (every 5 units) with analytic smoothstep anti-aliasing
        float majorUnit = unit * 5.0f;
        float2 uvMajor = coord / majorUnit;
        float2 fwMajor = max(fwidth(uvMajor), 0.000001f);
        float2 halfWMajor = fwMajor * 0.85f;
        float2 distMajor = abs(frac(uvMajor - 0.5f) - 0.5f);
        float2 aaMajor = 1.0f - smoothstep(halfWMajor * 0.4f, halfWMajor * 1.6f, distMajor);
        float lineMajor = max(aaMajor.x, aaMajor.y);
        float fadeMajor = saturate(1.0f - (max(fwMajor.x, fwMajor.y) - 0.10f) / 0.40f);
        lineMajor *= fadeMajor;

        // Red X Axis (Z == 0) and Green Z Axis (X == 0) with smooth anti-aliased boundaries
        float halfWAxisX = dCoord.y * 1.0f;
        float alphaAxisX = 1.0f - smoothstep(halfWAxisX * 0.4f, halfWAxisX * 1.6f, abs(input.WorldPos.z));

        float halfWAxisZ = dCoord.x * 1.0f;
        float alphaAxisZ = 1.0f - smoothstep(halfWAxisZ * 0.4f, halfWAxisZ * 1.6f, abs(input.WorldPos.x));

        // Horizon Grazing Angle Fade: smoothly dissolves into background as view vector approaches horizontal
        float grazingAngle = abs(viewDir.y);
        float horizonFade = smoothstep(0.004f, 0.08f, grazingAngle);

        // Distance horizon fade extending to 3500+ units (eliminates harsh circle/sphere cutoff)
        float maxFadeDist = max(GridScale * 40.0f, 3500.0f);
        float distFade = smoothstep(maxFadeDist, maxFadeDist * 0.15f, distToCam);
        float totalFade = horizonFade * distFade;

        if (totalFade <= 0.001f) discard;

        // Blender-style ground grid: neutral dark lines matching Blender dark theme
        float3 col = float3(0.20f, 0.20f, 0.20f); // Neutral dark gray for minor grid lines
        float alpha = lineMinor * 0.22f;

        // Major subdivisions: clean, refined neutral gray
        if (lineMajor > 0.001f) {
            col = lerp(col, float3(0.28f, 0.28f, 0.28f), lineMajor);
            alpha = max(alpha, lineMajor * 0.45f);
        }

        // Red X Axis (along Z = 0 in DX coordinate space)
        if (alphaAxisX > 0.001f) {
            col = lerp(col, float3(0.85f, 0.25f, 0.25f), alphaAxisX); // Blender Red X
            alpha = max(alpha, alphaAxisX * 0.85f);
        }
        // Green Z Axis (along X = 0 in DX coordinate space)
        if (alphaAxisZ > 0.001f) {
            col = lerp(col, float3(0.34f, 0.64f, 0.15f), alphaAxisZ); // Blender Green Y
            alpha = max(alpha, alphaAxisZ * 0.85f);
        }

        alpha *= totalFade;
        if (alpha < 0.002f) discard;

        return float4(col, alpha);
    }

    float3 N = normalize(input.WorldNorm);

    // Live Baked Normal Map Perturbation
    if (HasNormalMap == 1) {
        float4 nSample = NormalMap.Sample(SamplerLinear, input.UV);
        float3 ts = nSample.xyz * 2.0f - 1.0f;
        float3 T = normalize(input.WorldTangent);
        float3 B = normalize(input.WorldBitangent);
        // Gram-Schmidt re-orthogonalization in pixel shader:
        T = normalize(T - N * dot(N, T));
        B = normalize(B - N * dot(N, B) - T * dot(T, B));
        N = normalize(ts.x * T + ts.y * B + ts.z * N);
    }

    float3 V = normalize(CameraPos.xyz - input.WorldPos);
    float3 L = normalize(LightDir.xyz);

    // 3: Surface Normals Visualizer
    if (ShadingMode == 3) {
        return float4(N * 0.5f + 0.5f, Opacity);
    }

    // 6: Marmoset-style Skew Heatmap Mode (cool navy to electric amber/orange)
    if (ShadingMode == 6) {
        float3 coldCol = float3(0.18f, 0.24f, 0.32f);
        float3 hotCol = float3(1.0f, 0.65f, 0.05f);
        float3 skewVis = lerp(coldCol, hotCol, saturate(input.Color.a));
        float diff = saturate(dot(N, L)) * 0.7f + 0.3f;
        return float4(skewVis * diff, Opacity);
    }

    // 7: Clean Flat Shaded
    if (ShadingMode == 7) {
        float3 flatBase = MeshColor.rgb;
        if (input.Color.a > 0.001f) {
            flatBase = lerp(flatBase, float3(1.0f, 0.62f, 0.08f), input.Color.a * 0.85f);
        }
        float diff = saturate(dot(N, L)) * 0.75f + 0.25f;
        return float4(flatBase * diff, Opacity);
    }

    // Studio Lighting setup (Blender 4.x Studio Matcap style)
    float keyDiff = saturate(dot(N, L));
    float3 fillDir = normalize(float3(-L.x, 0.35f, -L.z));
    float fillDiff = saturate(dot(N, fillDir)) * 0.35f;
    float3 topAmb = float3(0.30f, 0.31f, 0.33f);
    float3 botAmb = float3(0.16f, 0.16f, 0.18f);
    float3 ambient = lerp(botAmb, topAmb, N.y * 0.5f + 0.5f);

    float3 H = normalize(L + V);
    float spec = pow(saturate(dot(N, H)), 28.0f) * 0.35f;
    float rim = pow(1.0f - saturate(dot(N, V)), 3.0f) * 0.20f;

    float3 baseAlbedo = MeshColor.rgb;

    // Marmoset-style dynamic highlight on painted skew areas
    if (input.Color.a > 0.001f) {
        baseAlbedo = lerp(baseAlbedo, float3(1.0f, 0.62f, 0.08f), input.Color.a * 0.85f);
    }

    // 0: Blender Studio White (Default clean crisp studio white)
    if (ShadingMode == 0) {
        baseAlbedo = float3(0.92f, 0.92f, 0.94f);
        if (input.Color.a > 0.001f) {
            baseAlbedo = lerp(baseAlbedo, float3(1.0f, 0.62f, 0.08f), input.Color.a * 0.85f);
        }
    }
    // 1: Clay Matcap (Substance Painter Sculpt look)
    else if (ShadingMode == 1) {
        baseAlbedo = float3(0.76f, 0.70f, 0.63f);
        if (input.Color.a > 0.001f) {
            baseAlbedo = lerp(baseAlbedo, float3(1.0f, 0.62f, 0.08f), input.Color.a * 0.85f);
        }
    }
    // 2: ZBrush Classic Wax Red
    else if (ShadingMode == 2) {
        baseAlbedo = float3(0.82f, 0.36f, 0.30f);
        if (input.Color.a > 0.001f) {
            baseAlbedo = lerp(baseAlbedo, float3(1.0f, 0.62f, 0.08f), input.Color.a * 0.85f);
        }
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
    m_lowPolyWireGPU.Release();
    m_cageGPU.Release();
    m_cageWireGPU.Release();
    m_seamLineGPU.Release();
    m_selectedEdgeGPU.Release();
    m_groundPlaneGPU.Release();

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
    if (m_depthNoWriteState) { m_depthNoWriteState->Release(); m_depthNoWriteState = nullptr; }
    if (m_depthBiasState) { m_depthBiasState->Release(); m_depthBiasState = nullptr; }
    if (m_alphaBlendState) { m_alphaBlendState->Release(); m_alphaBlendState = nullptr; }
    if (m_opaqueBlendState) { m_opaqueBlendState->Release(); m_opaqueBlendState = nullptr; }
    if (m_linearSampler) { m_linearSampler->Release(); m_linearSampler = nullptr; }
    ClearBakedNormalMap();
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
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ViewportVertex, pos),       D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ViewportVertex, norm),      D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, offsetof(ViewportVertex, uv),        D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TANGENT",  0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ViewportVertex, tangent),   D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "BINORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, offsetof(ViewportVertex, bitangent), D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ViewportVertex, color),     D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = m_device->CreateInputLayout(layout, 6, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout);
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

    // 3. Wireframe / Line rasterizer state (Antialiased lines)
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.AntialiasedLineEnable = TRUE;
    rd.MultisampleEnable = TRUE;
    rd.DepthBias = -1000;
    rd.SlopeScaledDepthBias = 0.0f;
    m_device->CreateRasterizerState(&rd, &m_wireframeRS);

    // Depth Stencil State
    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_device->CreateDepthStencilState(&dsd, &m_depthState);

    // Depth Stencil State with DepthWriteMask = ZERO (for ground grid & transparent overlays)
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_device->CreateDepthStencilState(&dsd, &m_depthNoWriteState);

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

    // Linear Texture Sampler for Normal Map Preview
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sampDesc, &m_linearSampler);

    return true;
}

void ViewportRenderer::CreateGridBuffers() {
    m_groundPlaneGPU.Release();

    // 32x32 subdivided ground mesh covering [-4000, 4000] for infinite horizon feel without clipping
    const int GRID_RES = 32;
    const float GRID_SIZE = 4000.0f;
    const float STEP = (GRID_SIZE * 2.0f) / GRID_RES;

    std::vector<ViewportVertex> groundVerts;
    groundVerts.reserve((GRID_RES + 1) * (GRID_RES + 1));
    for (int z = 0; z <= GRID_RES; ++z) {
        float pz = -GRID_SIZE + z * STEP;
        for (int x = 0; x <= GRID_RES; ++x) {
            float px = -GRID_SIZE + x * STEP;
            groundVerts.push_back({ { px, 0.0f, pz }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f, 1.0f, 0.0f } });
        }
    }

    std::vector<uint32_t> groundIndices;
    groundIndices.reserve(GRID_RES * GRID_RES * 6);
    for (int z = 0; z < GRID_RES; ++z) {
        for (int x = 0; x < GRID_RES; ++x) {
            uint32_t i00 = z * (GRID_RES + 1) + x;
            uint32_t i10 = i00 + 1;
            uint32_t i01 = (z + 1) * (GRID_RES + 1) + x;
            uint32_t i11 = i01 + 1;

            groundIndices.push_back(i00);
            groundIndices.push_back(i10);
            groundIndices.push_back(i11);

            groundIndices.push_back(i00);
            groundIndices.push_back(i11);
            groundIndices.push_back(i01);
        }
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = static_cast<UINT>(groundVerts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vInit = { groundVerts.data() };
    m_device->CreateBuffer(&vbd, &vInit, &m_groundPlaneGPU.vertexBuffer);

    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = static_cast<UINT>(groundIndices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA iInit = { groundIndices.data() };
    m_device->CreateBuffer(&ibd, &iInit, &m_groundPlaneGPU.indexBuffer);

    m_groundPlaneGPU.indexCount = static_cast<UINT>(groundIndices.size());
    m_groundPlaneGPU.vertexCount = static_cast<UINT>(groundVerts.size());
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
    m_lowPolyWireGPU.Release();
    if (mesh.vertices.empty() || mesh.triangles.empty()) return;

    // Ensure wireframe lines are deduplicated per geometric edge
    MeshLoader::DeduplicateWireframeLines(const_cast<LowPolyMesh&>(mesh));

    float s = scaleMultiplier > 1e-5f ? scaleMultiplier : 1.0f;

    std::vector<ViewportVertex> verts(mesh.vertices.size());
    m_lowPolyMin = { 1e9f, 1e9f, 1e9f };
    m_lowPolyMax = { -1e9f, -1e9f, -1e9f };

    bool hasSkew = mesh.HasVertexSkew();
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        float px = mesh.vertices[i].position.x * s;
        float py = mesh.vertices[i].position.y * s;
        float pz = mesh.vertices[i].position.z * s;
        verts[i].pos = { px, py, pz };
        verts[i].norm = { mesh.vertices[i].normal.x, mesh.vertices[i].normal.y, mesh.vertices[i].normal.z };
        verts[i].uv = { mesh.vertices[i].uv.u, mesh.vertices[i].uv.v };

        Vec3 n = mesh.vertices[i].normal;
        Vec3 t(mesh.vertices[i].tangent.x, mesh.vertices[i].tangent.y, mesh.vertices[i].tangent.z);
        Vec3 b = n.Cross(t) * mesh.vertices[i].tangent.w;
        verts[i].tangent = { t.x, t.y, t.z };
        verts[i].bitangent = { b.x, b.y, b.z };

        float sk = hasSkew ? mesh.vertexSkew[i] : 0.0f;
        verts[i].color = { 1.0f, 0.62f, 0.08f, sk };

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

    // 1. Shaded mesh triangles
    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_DEFAULT;
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

    // 2. Wireframe lines: Pure polygon edges (NO diagonal triangulation splits!)
    if (!mesh.wireframeLineIndices.empty()) {
        D3D11_BUFFER_DESC wibd = {};
        wibd.ByteWidth = (UINT)(mesh.wireframeLineIndices.size() * sizeof(uint32_t));
        wibd.Usage = D3D11_USAGE_IMMUTABLE;
        wibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA wiInit = { mesh.wireframeLineIndices.data() };
        m_device->CreateBuffer(&wibd, &wiInit, &m_lowPolyWireGPU.indexBuffer);
        m_lowPolyWireGPU.indexCount = (uint32_t)mesh.wireframeLineIndices.size();
    }

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
        verts[i].uv = { 0.0f, 0.0f };
        verts[i].tangent = { 0.0f, 0.0f, 0.0f };
        verts[i].bitangent = { 0.0f, 0.0f, 0.0f };
        verts[i].color = { 1.0f, 1.0f, 1.0f, 0.0f };

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
    m_cageWireGPU.Release();
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
        verts[i].uv = { mesh.vertices[i].uv.u, mesh.vertices[i].uv.v };
        verts[i].tangent = { 0.0f, 0.0f, 0.0f };
        verts[i].bitangent = { 0.0f, 0.0f, 0.0f };
        verts[i].color = { 0.2f, 0.7f, 1.0f, 0.5f };
    }

    if (m_cageGPU.vertexBuffer && m_cageGPU.vertexCount == verts.size() && m_context) {
        m_context->UpdateSubresource(m_cageGPU.vertexBuffer, 0, nullptr, verts.data(), 0, 0);
        return;
    }

    m_cageGPU.Release();
    m_cageWireGPU.Release();

    std::vector<uint32_t> indices;
    indices.reserve(mesh.triangles.size() * 3);
    for (const auto& tri : mesh.triangles) {
        indices.push_back(tri.indices[0]);
        indices.push_back(tri.indices[1]);
        indices.push_back(tri.indices[2]);
    }

    D3D11_BUFFER_DESC vbd = {};
    vbd.ByteWidth = (UINT)(verts.size() * sizeof(ViewportVertex));
    vbd.Usage = D3D11_USAGE_DEFAULT;
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

    // Cage wireframe index buffer: Pure polygon edges
    if (!mesh.wireframeLineIndices.empty()) {
        D3D11_BUFFER_DESC wibd = {};
        wibd.ByteWidth = (UINT)(mesh.wireframeLineIndices.size() * sizeof(uint32_t));
        wibd.Usage = D3D11_USAGE_IMMUTABLE;
        wibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA wiInit = { mesh.wireframeLineIndices.data() };
        m_device->CreateBuffer(&wibd, &wiInit, &m_cageWireGPU.indexBuffer);
        m_cageWireGPU.indexCount = (uint32_t)mesh.wireframeLineIndices.size();
    }
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
    // buttonState: 1 = Left (Maya Tumble/Orbit), 2 = Middle (Maya Track/Pan), 3 = Right (Maya Dolly/Zoom)
    if (buttonState == 1) {
        // Maya Orbit (Tumble): Alt + LMB
        m_cameraYaw += deltaX * 0.005f;
        m_cameraPitch += deltaY * 0.005f;
        m_cameraPitch = std::clamp(m_cameraPitch, -1.55f, 1.55f);
    } else if (buttonState == 2) {
        // Maya Track (Pan): Alt + MMB - True Screen-Plane tracking without perspective distortion
        float cosP = std::cos(m_cameraPitch);
        float sinP = std::sin(m_cameraPitch);
        float cosY = std::cos(m_cameraYaw);
        float sinY = std::sin(m_cameraYaw);

        DirectX::XMFLOAT3 eye = {
            m_cameraTarget.x + m_cameraDistance * cosP * sinY,
            m_cameraTarget.y + m_cameraDistance * sinP,
            m_cameraTarget.z + m_cameraDistance * cosP * cosY
        };

        DirectX::XMVECTOR vEye = DirectX::XMLoadFloat3(&eye);
        DirectX::XMVECTOR vTarget = DirectX::XMLoadFloat3(&m_cameraTarget);
        DirectX::XMVECTOR vFwd = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(vTarget, vEye));

        DirectX::XMVECTOR vWorldUp = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        DirectX::XMVECTOR vRight = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(vFwd, vWorldUp));
        DirectX::XMVECTOR vCameraUp = DirectX::XMVector3Normalize(DirectX::XMVector3Cross(vRight, vFwd));

        float panSpeed = m_cameraDistance * 0.0013f;
        DirectX::XMVECTOR offset = DirectX::XMVectorAdd(
            DirectX::XMVectorScale(vRight, deltaX * panSpeed),
            DirectX::XMVectorScale(vCameraUp, deltaY * panSpeed)
        );

        m_cameraTarget.x += DirectX::XMVectorGetX(offset);
        m_cameraTarget.y += DirectX::XMVectorGetY(offset);
        m_cameraTarget.z += DirectX::XMVectorGetZ(offset);
    } else if (buttonState == 3) {
        // Maya Dolly (Zoom): Alt + RMB drag
        float delta = deltaX - deltaY;
        float zoomFactor = 1.0f - delta * 0.007f;
        m_cameraDistance *= zoomFactor;
        float minDist = std::max(0.001f, m_sceneRadius * 0.002f);
        float maxDist = std::max(1000.0f, m_sceneRadius * 100.0f);
        m_cameraDistance = std::clamp(m_cameraDistance, minDist, maxDist);
    }

    // Scroll Wheel Zoom: Smooth exponential dolly
    if (wheelDelta != 0.0f) {
        float zoomFactor = std::pow(0.85f, wheelDelta);
        m_cameraDistance *= zoomFactor;
        float minDist = std::max(0.001f, m_sceneRadius * 0.002f);
        float maxDist = std::max(1000.0f, m_sceneRadius * 100.0f);
        m_cameraDistance = std::clamp(m_cameraDistance, minDist, maxDist);
    }
}

void ViewportRenderer::UpdateConstantBuffer(const DirectX::XMMATRIX& world, const DirectX::XMFLOAT4& color, int mode, int hasNormalMap) {
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
        cb->HasNormalMap = hasNormalMap;
        cb->GridScale = (m_sceneRadius > 1e-4f ? m_sceneRadius : 1.0f);
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

    // Blender Dark 3D Viewport background: neutral dark gray RGB(42, 42, 42) Hex(#2A2A2A)
    const float clearColor[4] = { 42.0f / 255.0f, 42.0f / 255.0f, 42.0f / 255.0f, 1.0f };
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

    // 1. Render Ground Grid (Blender Procedural Anti-Aliased Ground Grid & Colored Axes)
    if (showGrid && m_groundPlaneGPU.vertexBuffer) {
        m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
        m_context->OMSetDepthStencilState(m_depthNoWriteState, 0);
        m_context->RSSetState(m_solidNoCullRS);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        UpdateConstantBuffer(identity, { 1.0f, 1.0f, 1.0f, 1.0f }, 5);
        m_context->IASetVertexBuffers(0, 1, &m_groundPlaneGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_groundPlaneGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_groundPlaneGPU.indexCount, 0, 0);

        m_context->OMSetDepthStencilState(m_depthState, 0);
    }

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // 2. Render High-Poly Sculpt
    if (showHighPoly && m_highPolyGPU.vertexBuffer) {
        m_context->OMSetBlendState(m_opaqueBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidRS);

        int mode = (int)highPolyShading;
        DirectX::XMFLOAT4 hpColor = { 0.92f, 0.92f, 0.94f, highPolyOpacity };
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

        bool useNormalMap = (showBakedNormals && m_bakedNormalSRV != nullptr);
        if (useNormalMap) {
            m_context->PSSetShaderResources(0, 1, &m_bakedNormalSRV);
            m_context->PSSetSamplers(0, 1, &m_linearSampler);
        }

        DirectX::XMFLOAT4 lpColor = { 0.92f, 0.92f, 0.94f, lowPolyOpacity };
        int lpMode = showSkew ? 6 : (int)highPolyShading;
        UpdateConstantBuffer(worldLP, lpColor, lpMode, useNormalMap ? 1 : 0);

        m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_lowPolyGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_lowPolyGPU.indexCount, 0, 0);

        if (useNormalMap) {
            ID3D11ShaderResourceView* nullSRV = nullptr;
            m_context->PSSetShaderResources(0, 1, &nullSRV);
        }

        // Wireframe overlay (Pure polygon edges, NO diagonal triangulation splits!)
        if (showLowPolyWireframe) {
            m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
            m_context->RSSetState(m_wireframeRS);

            DirectX::XMFLOAT4 wireColor = { 0.26f, 0.26f, 0.26f, 0.85f };
            UpdateConstantBuffer(worldLP, wireColor, 4);

            if (m_lowPolyWireGPU.indexBuffer && m_lowPolyWireGPU.indexCount > 0) {
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
                m_context->IASetIndexBuffer(m_lowPolyWireGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                m_context->DrawIndexed(m_lowPolyWireGPU.indexCount, 0, 0);
                m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            } else {
                m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
                m_context->IASetIndexBuffer(m_lowPolyGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
                m_context->DrawIndexed(m_lowPolyGPU.indexCount, 0, 0);
            }
        }

        // 3b. Marked Cut Seams (Vivid Neon Cyan)
        if (showSeams && m_seamLineGPU.indexBuffer && m_seamLineGPU.indexCount > 0) {
            m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
            m_context->RSSetState(m_wireframeRS);

            DirectX::XMFLOAT4 seamColor = { 0.12f, 0.90f, 1.0f, 1.0f }; // Neon Cyan
            UpdateConstantBuffer(worldLP, seamColor, 4);

            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
            m_context->IASetIndexBuffer(m_seamLineGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            m_context->DrawIndexed(m_seamLineGPU.indexCount, 0, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }

        // 3c. Selected Edges (Bright Glowing Golden Yellow)
        if (m_selectedEdgeGPU.indexBuffer && m_selectedEdgeGPU.indexCount > 0) {
            m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
            m_context->RSSetState(m_wireframeRS);

            DirectX::XMFLOAT4 selColor = { 1.0f, 0.85f, 0.12f, 1.0f }; // Bright Golden Yellow
            UpdateConstantBuffer(worldLP, selColor, 4);

            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_context->IASetVertexBuffers(0, 1, &m_lowPolyGPU.vertexBuffer, &stride, &offset);
            m_context->IASetIndexBuffer(m_selectedEdgeGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            m_context->DrawIndexed(m_selectedEdgeGPU.indexCount, 0, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        }
    }

    // 4. Render Cage Shell (Marmoset Toolbag Warm Amber Cage)
    if (showCage && m_cageGPU.vertexBuffer) {
        // Semi-transparent surface
        m_context->OMSetBlendState(m_alphaBlendState, nullptr, 0xFFFFFFFF);
        m_context->RSSetState(m_solidNoCullRS);

        DirectX::XMFLOAT4 cageColor = { 0.30f, 0.88f, 0.72f, cageOpacity * 0.35f };
        UpdateConstantBuffer(worldLP, cageColor, 3);

        m_context->IASetVertexBuffers(0, 1, &m_cageGPU.vertexBuffer, &stride, &offset);
        m_context->IASetIndexBuffer(m_cageGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
        m_context->DrawIndexed(m_cageGPU.indexCount, 0, 0);

        // Cage wireframe (Pure polygon edges)
        m_context->RSSetState(m_wireframeRS);
        DirectX::XMFLOAT4 cageWire = { 0.30f, 0.88f, 0.72f, 0.85f };
        UpdateConstantBuffer(worldLP, cageWire, 4);

        if (m_cageWireGPU.indexBuffer && m_cageWireGPU.indexCount > 0) {
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            m_context->IASetVertexBuffers(0, 1, &m_cageGPU.vertexBuffer, &stride, &offset);
            m_context->IASetIndexBuffer(m_cageWireGPU.indexBuffer, DXGI_FORMAT_R32_UINT, 0);
            m_context->DrawIndexed(m_cageWireGPU.indexCount, 0, 0);
            m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        } else {
            m_context->DrawIndexed(m_cageGPU.indexCount, 0, 0);
        }
    }
}

void ViewportRenderer::ClearBakedNormalMap() {
    if (m_bakedNormalSRV) {
        m_bakedNormalSRV->Release();
        m_bakedNormalSRV = nullptr;
    }
    if (m_bakedNormalTex) {
        m_bakedNormalTex->Release();
        m_bakedNormalTex = nullptr;
    }
    m_bakedNormalWidth = 0;
    m_bakedNormalHeight = 0;
    m_hasBakedNormalMap = false;
}

void ViewportRenderer::SetBakedNormalMap(const float* r, const float* g, const float* b, int width, int height) {
    if (!r || !g || !b || width <= 0 || height <= 0 || !m_device) return;

    size_t totalPixels = static_cast<size_t>(width) * height;
    std::vector<uint8_t> rgba(totalPixels * 4);

    for (size_t i = 0; i < totalPixels; ++i) {
        float rf = std::clamp(r[i], 0.0f, 1.0f);
        float gf = std::clamp(g[i], 0.0f, 1.0f);
        float bf = std::clamp(b[i], 0.0f, 1.0f);

        rgba[i * 4 + 0] = static_cast<uint8_t>(rf * 255.0f + 0.5f);
        rgba[i * 4 + 1] = static_cast<uint8_t>(gf * 255.0f + 0.5f);
        rgba[i * 4 + 2] = static_cast<uint8_t>(bf * 255.0f + 0.5f);
        rgba[i * 4 + 3] = 255;
    }

    // Dynamic streaming update if dimensions match existing GPU texture
    if (m_bakedNormalTex && m_bakedNormalSRV && m_bakedNormalWidth == width && m_bakedNormalHeight == height && m_context) {
        m_context->UpdateSubresource(m_bakedNormalTex, 0, nullptr, rgba.data(), static_cast<UINT>(width * 4), 0);
        m_hasBakedNormalMap = true;
        showBakedNormals = true;
        return;
    }

    ClearBakedNormalMap();

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT; // D3D11_USAGE_DEFAULT allows UpdateSubresource for live progressive streaming
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = rgba.data();
    initData.SysMemPitch = static_cast<UINT>(width * 4);
    initData.SysMemSlicePitch = static_cast<UINT>(totalPixels * 4);

    HRESULT hr = m_device->CreateTexture2D(&desc, &initData, &m_bakedNormalTex);
    if (FAILED(hr)) return;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    hr = m_device->CreateShaderResourceView(m_bakedNormalTex, &srvDesc, &m_bakedNormalSRV);
    if (SUCCEEDED(hr)) {
        m_bakedNormalWidth = width;
        m_bakedNormalHeight = height;
        m_hasBakedNormalMap = true;
        showBakedNormals = true;
    }
}

void ViewportRenderer::UpdateSkew(const LowPolyMesh& mesh, float scaleMultiplier) {
    if (!m_lowPolyGPU.vertexBuffer || mesh.vertices.empty()) return;

    float s = scaleMultiplier > 1e-5f ? scaleMultiplier : 1.0f;
    bool hasSkew = mesh.HasVertexSkew();

    std::vector<ViewportVertex> verts(mesh.vertices.size());
    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        float px = mesh.vertices[i].position.x * s;
        float py = mesh.vertices[i].position.y * s;
        float pz = mesh.vertices[i].position.z * s;
        verts[i].pos = { px, py, pz };
        verts[i].norm = { mesh.vertices[i].normal.x, mesh.vertices[i].normal.y, mesh.vertices[i].normal.z };
        verts[i].uv = { mesh.vertices[i].uv.u, mesh.vertices[i].uv.v };

        Vec3 n = mesh.vertices[i].normal;
        Vec3 t(mesh.vertices[i].tangent.x, mesh.vertices[i].tangent.y, mesh.vertices[i].tangent.z);
        Vec3 b = n.Cross(t) * mesh.vertices[i].tangent.w;
        verts[i].tangent = { t.x, t.y, t.z };
        verts[i].bitangent = { b.x, b.y, b.z };

        float sk = hasSkew ? mesh.vertexSkew[i] : 0.0f;
        verts[i].color = { 1.0f, 0.62f, 0.08f, sk };
    }

    if (m_context && m_lowPolyGPU.vertexBuffer && m_lowPolyGPU.vertexCount == verts.size()) {
        m_context->UpdateSubresource(m_lowPolyGPU.vertexBuffer, 0, nullptr, verts.data(), 0, 0);
    }
}

void ViewportRenderer::UpdateSeams(const LowPolyMesh& mesh) {
    m_seamLineGPU.Release();
    if (mesh.seamLineIndices.empty() || !m_device) return;

    D3D11_BUFFER_DESC sibd = {};
    sibd.ByteWidth = static_cast<UINT>(mesh.seamLineIndices.size() * sizeof(uint32_t));
    sibd.Usage = D3D11_USAGE_DEFAULT;
    sibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA siInit = { mesh.seamLineIndices.data() };
    m_device->CreateBuffer(&sibd, &siInit, &m_seamLineGPU.indexBuffer);
    m_seamLineGPU.indexCount = static_cast<uint32_t>(mesh.seamLineIndices.size());
}

void ViewportRenderer::UpdateSelectedEdges(const LowPolyMesh& mesh) {
    m_selectedEdgeGPU.Release();
    if (mesh.selectedEdgeLineIndices.empty() || !m_device) return;

    D3D11_BUFFER_DESC sibd = {};
    sibd.ByteWidth = static_cast<UINT>(mesh.selectedEdgeLineIndices.size() * sizeof(uint32_t));
    sibd.Usage = D3D11_USAGE_DEFAULT;
    sibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA siInit = { mesh.selectedEdgeLineIndices.data() };
    m_device->CreateBuffer(&sibd, &siInit, &m_selectedEdgeGPU.indexBuffer);
    m_selectedEdgeGPU.indexCount = static_cast<uint32_t>(mesh.selectedEdgeLineIndices.size());
}

DirectX::XMMATRIX ViewportRenderer::GetLowPolyWorldMatrix() const {
    return DirectX::XMMatrixMultiply(
        DirectX::XMMatrixMultiply(
            DirectX::XMMatrixScaling(m_lpScale.x, m_lpScale.y, m_lpScale.z),
            DirectX::XMMatrixRotationRollPitchYaw(
                DirectX::XMConvertToRadians(m_lpRotation.x),
                DirectX::XMConvertToRadians(m_lpRotation.y),
                DirectX::XMConvertToRadians(m_lpRotation.z)
            )
        ),
        DirectX::XMMatrixTranslation(m_lpTranslation.x, m_lpTranslation.y, m_lpTranslation.z)
    );
}

DirectX::XMMATRIX ViewportRenderer::GetViewProjectionMatrix() const {
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
    float aspect = (m_vpHeight > 0) ? ((float)m_vpWidth / (float)m_vpHeight) : 1.0f;
    DirectX::XMMATRIX proj = DirectX::XMMatrixPerspectiveFovLH(
        DirectX::XMConvertToRadians(45.0f),
        aspect,
        0.05f,
        10000.0f
    );
    return DirectX::XMMatrixMultiply(view, proj);
}

bool ViewportRenderer::UnprojectRay(float screenX, float screenY, DirectX::XMFLOAT3& outRayOrg, DirectX::XMFLOAT3& outRayDir) const {
    if (m_vpWidth <= 0 || m_vpHeight <= 0) return false;

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

    DirectX::XMMATRIX invViewProj = DirectX::XMMatrixInverse(nullptr, DirectX::XMMatrixMultiply(view, proj));

    float ndcX = (2.0f * screenX) / (float)m_vpWidth - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY) / (float)m_vpHeight;

    DirectX::XMVECTOR nearPointNDC = DirectX::XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    DirectX::XMVECTOR farPointNDC  = DirectX::XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);

    DirectX::XMVECTOR nearWorld = DirectX::XMVector3TransformCoord(nearPointNDC, invViewProj);
    DirectX::XMVECTOR farWorld  = DirectX::XMVector3TransformCoord(farPointNDC, invViewProj);

    DirectX::XMVECTOR dir = DirectX::XMVector3Normalize(DirectX::XMVectorSubtract(farWorld, nearWorld));

    DirectX::XMStoreFloat3(&outRayOrg, nearWorld);
    DirectX::XMStoreFloat3(&outRayDir, dir);
    return true;
}

bool ViewportRenderer::ProjectToScreen(const DirectX::XMFLOAT3& worldPos, float& outScreenX, float& outScreenY) const {
    if (m_vpWidth <= 0 || m_vpHeight <= 0) return false;

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

    DirectX::XMVECTOR pW = DirectX::XMLoadFloat3(&worldPos);
    DirectX::XMVECTOR pView = DirectX::XMVector3Transform(pW, view);
    if (DirectX::XMVectorGetZ(pView) <= 0.05f) return false;

    DirectX::XMVECTOR pClip = DirectX::XMVector3TransformCoord(pW, DirectX::XMMatrixMultiply(view, proj));

    float ndcX = DirectX::XMVectorGetX(pClip);
    float ndcY = DirectX::XMVectorGetY(pClip);

    outScreenX = (ndcX + 1.0f) * 0.5f * (float)m_vpWidth;
    outScreenY = (1.0f - ndcY) * 0.5f * (float)m_vpHeight;
    return true;
}

} // namespace Baker
