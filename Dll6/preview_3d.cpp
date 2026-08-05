#include "shared.h"
#include "preview_3d.h"
#include "resource.h"

#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace {

constexpr UINT kTargetWidth = 384;
constexpr UINT kTargetHeight = 600;
constexpr UINT kMaxJoints = 160;

#pragma pack(push, 1)
struct AssetHeader {
    char magic[8];
    uint32_t version;
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t primitiveCount;
    uint32_t jointCount;
    uint32_t frameCount;
    uint32_t pointCount;
    uint32_t materialCount;
    uint32_t imageCount;
    float fps;
    float duration;
    float boundsMin[3];
    float boundsMax[3];
};

struct AssetPrimitive {
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t material;
    uint32_t reserved;
};

struct AssetMaterial {
    int32_t baseImage;
    int32_t emissiveImage;
    int32_t alphaBlend;
    int32_t reserved;
    float baseFactor[4];
    float emissiveFactor[3];
    float padding;
};

struct AssetVertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint16_t joints[4];
    float weights[4];
};
#pragma pack(pop)

static_assert(sizeof(AssetHeader) == 76);
static_assert(sizeof(AssetPrimitive) == 16);
static_assert(sizeof(AssetMaterial) == 48);
static_assert(sizeof(AssetVertex) == 56);

struct SceneConstants {
    XMFLOAT4X4 viewProjection;
    XMFLOAT4 lightDirection;
    XMFLOAT4 modelParameters;
};

struct MaterialConstants {
    XMFLOAT4 baseFactor;
    XMFLOAT4 emissiveFactor;
};

struct Runtime {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11Buffer> sceneBuffer;
    ComPtr<ID3D11Buffer> boneBuffer;
    ComPtr<ID3D11Buffer> materialBuffer;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11RasterizerState> outlineRasterizer;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11DepthStencilState> outlineDepthState;
    ComPtr<ID3D11Texture2D> targetTexture;
    ComPtr<ID3D11RenderTargetView> targetView;
    ComPtr<ID3D11Texture2D> stagingTexture;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11ShaderResourceView> whiteTexture;
    std::vector<ComPtr<ID3D11ShaderResourceView>> images;
    std::vector<AssetPrimitive> primitives;
    std::vector<AssetMaterial> materials;
    AssetHeader header{};
    const float* frameMatrices = nullptr;
    const float* framePoints = nullptr;
    bool initialized = false;
    bool failed = false;
} runtime;

bool Advance(const uint8_t*& cursor, const uint8_t* end, size_t bytes,
             const void*& output) {
    if (bytes > static_cast<size_t>(end - cursor)) return false;
    output = cursor;
    cursor += bytes;
    return true;
}

bool CompileShader(const char* source, const char* entry, const char* profile,
                   ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), "preview_3d", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        bytecode.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result)) {
        if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    return true;
}

bool DecodeImage(ID3D11Device* device, const uint8_t* bytes, uint32_t size,
                 ComPtr<ID3D11ShaderResourceView>& output) {
    if (!device || !bytes || !size) return false;
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(
        CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(result)) {
        result = CoCreateInstance(
            CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
    }
    if (FAILED(result)) return false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    UINT width = 0, height = 0;
    if (FAILED(factory->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(reinterpret_cast<const BYTE*>(bytes)), size)) ||
        FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) ||
        FAILED(frame->GetSize(&width, &height)) || !width || !height ||
        FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0,
            WICBitmapPaletteTypeCustom))) {
        return false;
    }

    const UINT stride = width * 4;
    std::vector<uint8_t> pixels(static_cast<size_t>(stride) * height);
    if (FAILED(converter->CopyPixels(nullptr, stride,
                                     static_cast<UINT>(pixels.size()),
                                     pixels.data()))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    // The preview target is consumed by a D2D UNORM bitmap. Keep texture
    // sampling in the same color space so the model does not become too dark
    // from an unmatched sRGB decode/encode pair.
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = pixels.data();
    data.SysMemPitch = stride;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(
            &description, &data, texture.GetAddressOf())) ||
        FAILED(device->CreateShaderResourceView(
            texture.Get(), nullptr, output.GetAddressOf()))) {
        return false;
    }
    return true;
}

bool CreateWhiteTexture(ID3D11Device* device) {
    const uint32_t pixel = 0xffffffffu;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = 1;
    description.Height = 1;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{&pixel, sizeof(pixel), 0};
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(
               &description, &data, texture.GetAddressOf())) &&
           SUCCEEDED(device->CreateShaderResourceView(
               texture.Get(), nullptr, runtime.whiteTexture.GetAddressOf()));
}

bool CreateRenderTarget(ID3D11Device* device) {
    D3D11_TEXTURE2D_DESC color{};
    color.Width = kTargetWidth;
    color.Height = kTargetHeight;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    color.SampleDesc.Count = 1;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(
            &color, nullptr, runtime.targetTexture.GetAddressOf())) ||
        FAILED(device->CreateRenderTargetView(
            runtime.targetTexture.Get(), nullptr,
            runtime.targetView.GetAddressOf()))) {
        return false;
    }
    D3D11_TEXTURE2D_DESC staging = color;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(
            &staging, nullptr, runtime.stagingTexture.GetAddressOf()))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC depth = color;
    depth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(
            &depth, nullptr, runtime.depthTexture.GetAddressOf())) ||
        FAILED(device->CreateDepthStencilView(
            runtime.depthTexture.Get(), nullptr,
            runtime.depthView.GetAddressOf()))) {
        return false;
    }
    return true;
}

bool CreatePipeline(ID3D11Device* device) {
    static constexpr char shader[] = R"(
cbuffer Scene : register(b0) {
    float4x4 ViewProjection;
    float4 LightDirection;
    float4 ModelParameters;
};
cbuffer Skin : register(b1) { float4 BoneRows[480]; };
cbuffer Material : register(b2) {
    float4 BaseFactor;
    float4 EmissiveFactor;
};
Texture2D BaseTexture : register(t0);
Texture2D EmissiveTexture : register(t1);
SamplerState LinearSampler : register(s0);

struct VSInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    uint4 joints : BLENDINDICES0;
    float4 weights : BLENDWEIGHT0;
};
struct VSOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    float4 localPosition = 0;
    float3 localNormal = 0;
    [unroll] for (int i = 0; i < 4; ++i) {
        uint row = input.joints[i] * 3;
        float4 position = float4(input.position, 1.0);
        float3 skinnedPosition = float3(
            dot(BoneRows[row + 0], position),
            dot(BoneRows[row + 1], position),
            dot(BoneRows[row + 2], position));
        float3 skinnedNormal = float3(
            dot(BoneRows[row + 0].xyz, input.normal),
            dot(BoneRows[row + 1].xyz, input.normal),
            dot(BoneRows[row + 2].xyz, input.normal));
        localPosition += float4(skinnedPosition, 1.0) * input.weights[i];
        localNormal += skinnedNormal * input.weights[i];
    }
    if (EmissiveFactor.w > 0.5)
        localPosition.xyz += normalize(localNormal) * 0.014;
    localPosition.xy += ModelParameters.zw;
    float cosine = ModelParameters.x;
    float sine = ModelParameters.y;
    float3 worldPosition = float3(
        localPosition.x * cosine + localPosition.z * sine,
        localPosition.y,
        -localPosition.x * sine + localPosition.z * cosine);
    float3 worldNormal = normalize(float3(
        localNormal.x * cosine + localNormal.z * sine,
        localNormal.y,
        -localNormal.x * sine + localNormal.z * cosine));
    VSOutput output;
    output.position = mul(ViewProjection, float4(worldPosition, 1.0));
    output.normal = worldNormal;
    output.uv = input.uv;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET {
    if (EmissiveFactor.w > 0.5)
        return BaseFactor;
    float4 base = BaseTexture.Sample(LinearSampler, input.uv) * BaseFactor;
    float3 emissive = EmissiveTexture.Sample(LinearSampler, input.uv).rgb * EmissiveFactor.rgb;
    float light = 0.38 + 0.62 * saturate(dot(normalize(input.normal), normalize(-LightDirection.xyz)));
    float rim = pow(1.0 - saturate(abs(input.normal.z)), 3.0) * 0.16;
    return float4(base.rgb * light + emissive * 0.80 + rim, base.a);
}
)";

    ComPtr<ID3DBlob> vertexCode;
    ComPtr<ID3DBlob> pixelCode;
    if (!CompileShader(shader, "VSMain", "vs_5_0", vertexCode) ||
        !CompileShader(shader, "PSMain", "ps_5_0", pixelCode) ||
        FAILED(device->CreateVertexShader(
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
            nullptr, runtime.vertexShader.GetAddressOf())) ||
        FAILED(device->CreatePixelShader(
            pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
            nullptr, runtime.pixelShader.GetAddressOf()))) {
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC elements[]{
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R16G16B16A16_UINT, 0, 32,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40,
         D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(device->CreateInputLayout(
            elements, static_cast<UINT>(std::size(elements)),
            vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
            runtime.inputLayout.GetAddressOf()))) {
        return false;
    }

    auto createConstantBuffer = [&](UINT size, ComPtr<ID3D11Buffer>& output) {
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = (size + 15u) & ~15u;
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(
            &description, nullptr, output.GetAddressOf()));
    };
    if (!createConstantBuffer(sizeof(SceneConstants), runtime.sceneBuffer) ||
        !createConstantBuffer(sizeof(XMFLOAT4) * kMaxJoints * 3,
                              runtime.boneBuffer) ||
        !createConstantBuffer(sizeof(MaterialConstants),
                              runtime.materialBuffer)) {
        return false;
    }

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(
            &sampler, runtime.sampler.GetAddressOf()))) return false;

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    rasterizer.ScissorEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(
            &rasterizer, runtime.rasterizer.GetAddressOf()))) return false;
    rasterizer.CullMode = D3D11_CULL_FRONT;
    if (FAILED(device->CreateRasterizerState(
            &rasterizer, runtime.outlineRasterizer.GetAddressOf()))) return false;

    D3D11_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = TRUE;
    target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOp = D3D11_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D11_BLEND_ONE;
    target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(
            &blend, runtime.blendState.GetAddressOf()))) return false;

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    if (FAILED(device->CreateDepthStencilState(
            &depth, runtime.depthState.GetAddressOf()))) return false;
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    if (FAILED(device->CreateDepthStencilState(
            &depth, runtime.outlineDepthState.GetAddressOf()))) return false;
    return true;
}

bool Initialize(ID3D11Device* device) {
    if (!device || !moduleHandle) return false;
    HRSRC resource = FindResourceW(
        moduleHandle, MAKEINTRESOURCEW(IDR_ESP_PREVIEW_3D), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(moduleHandle, resource);
    const auto* bytes = static_cast<const uint8_t*>(LockResource(loaded));
    const size_t size = SizeofResource(moduleHandle, resource);
    if (!bytes || size < sizeof(AssetHeader)) return false;

    const uint8_t* cursor = bytes;
    const uint8_t* end = bytes + size;
    const void* block = nullptr;
    if (!Advance(cursor, end, sizeof(AssetHeader), block)) return false;
    std::memcpy(&runtime.header, block, sizeof(runtime.header));
    if (std::memcmp(runtime.header.magic, "D6P3D001", 8) != 0 ||
        runtime.header.version != 1 ||
        runtime.header.jointCount > kMaxJoints ||
        runtime.header.pointCount != 18 ||
        !runtime.header.vertexCount || !runtime.header.indexCount ||
        !runtime.header.frameCount) {
        return false;
    }

    const size_t primitiveBytes =
        static_cast<size_t>(runtime.header.primitiveCount) * sizeof(AssetPrimitive);
    if (!Advance(cursor, end, primitiveBytes, block)) return false;
    runtime.primitives.assign(
        static_cast<const AssetPrimitive*>(block),
        static_cast<const AssetPrimitive*>(block) + runtime.header.primitiveCount);

    const size_t materialBytes =
        static_cast<size_t>(runtime.header.materialCount) * sizeof(AssetMaterial);
    if (!Advance(cursor, end, materialBytes, block)) return false;
    runtime.materials.assign(
        static_cast<const AssetMaterial*>(block),
        static_cast<const AssetMaterial*>(block) + runtime.header.materialCount);

    const size_t vertexBytes =
        static_cast<size_t>(runtime.header.vertexCount) * sizeof(AssetVertex);
    if (!Advance(cursor, end, vertexBytes, block)) return false;
    D3D11_BUFFER_DESC vertexDescription{};
    vertexDescription.ByteWidth = static_cast<UINT>(vertexBytes);
    vertexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    vertexDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vertexData{block, 0, 0};
    if (FAILED(device->CreateBuffer(
            &vertexDescription, &vertexData,
            runtime.vertexBuffer.GetAddressOf()))) return false;

    const size_t indexBytes =
        static_cast<size_t>(runtime.header.indexCount) * sizeof(uint32_t);
    if (!Advance(cursor, end, indexBytes, block)) return false;
    D3D11_BUFFER_DESC indexDescription{};
    indexDescription.ByteWidth = static_cast<UINT>(indexBytes);
    indexDescription.Usage = D3D11_USAGE_IMMUTABLE;
    indexDescription.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA indexData{block, 0, 0};
    if (FAILED(device->CreateBuffer(
            &indexDescription, &indexData,
            runtime.indexBuffer.GetAddressOf()))) return false;

    const size_t matrixBytes = static_cast<size_t>(runtime.header.frameCount) *
        runtime.header.jointCount * 16 * sizeof(float);
    if (!Advance(cursor, end, matrixBytes, block)) return false;
    runtime.frameMatrices = static_cast<const float*>(block);
    const size_t pointBytes = static_cast<size_t>(runtime.header.frameCount) *
        runtime.header.pointCount * 3 * sizeof(float);
    if (!Advance(cursor, end, pointBytes, block)) return false;
    runtime.framePoints = static_cast<const float*>(block);

    runtime.images.resize(runtime.header.imageCount);
    for (uint32_t i = 0; i < runtime.header.imageCount; ++i) {
        if (!Advance(cursor, end, sizeof(uint32_t), block)) return false;
        uint32_t imageSize = 0;
        std::memcpy(&imageSize, block, sizeof(imageSize));
        if (!Advance(cursor, end, imageSize, block) ||
            !DecodeImage(device, static_cast<const uint8_t*>(block), imageSize,
                         runtime.images[i])) return false;
        const size_t padding = (4 - imageSize % 4) % 4;
        if (padding && !Advance(cursor, end, padding, block)) return false;
    }

    runtime.device = device;
    return CreateWhiteTexture(device) && CreateRenderTarget(device) &&
           CreatePipeline(device);
}

template <typename T>
bool UpdateBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
                  const T& value) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &value, sizeof(value));
    context->Unmap(buffer, 0);
    return true;
}

bool UpdateBones(ID3D11DeviceContext* context, uint32_t frameIndex) {
    const size_t matrixCount = runtime.header.jointCount;
    const float* source = runtime.frameMatrices +
        static_cast<size_t>(frameIndex) * matrixCount * 16;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(runtime.boneBuffer.Get(), 0,
                            D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    // Store explicit affine rows instead of HLSL matrix values. This avoids
    // all row/column-major packing ambiguity between glTF and D3D constant
    // buffers, including the translation column used by skinning.
    auto* destination = static_cast<float*>(mapped.pData);
    for (size_t matrix = 0; matrix < matrixCount; ++matrix) {
        for (size_t row = 0; row < 3; ++row) {
            for (size_t column = 0; column < 4; ++column) {
                destination[matrix * 12 + row * 4 + column] =
                    source[matrix * 16 + column * 4 + row];
            }
        }
    }
    context->Unmap(runtime.boneBuffer.Get(), 0);
    return true;
}

Preview3DPoint ProjectPoint(const float* point, const XMMATRIX& world,
                            const XMMATRIX& view, const XMMATRIX& projection) {
    const XMVECTOR source = XMVectorSet(point[0], point[1], point[2], 1.0f);
    const XMVECTOR projected = XMVector3Project(
        source, 0.0f, 0.0f, static_cast<float>(kTargetWidth),
        static_cast<float>(kTargetHeight), 0.0f, 1.0f,
        projection, view, world);
    XMFLOAT3 value{};
    XMStoreFloat3(&value, projected);
    return {value.x / kTargetWidth, value.y / kTargetHeight,
            std::isfinite(value.x) && std::isfinite(value.y)};
}

void BuildFrameGeometry(uint32_t frameIndex, const XMMATRIX& world,
                        const XMMATRIX& view, const XMMATRIX& projection,
                        Preview3DFrame& frame) {
    const float* points = runtime.framePoints +
        static_cast<size_t>(frameIndex) * runtime.header.pointCount * 3;
    for (uint32_t i = 0; i < runtime.header.pointCount; ++i)
        frame.skeleton[i] = ProjectPoint(points + i * 3, world, view, projection);

    float left = 1.0f, top = 1.0f, right = 0.0f, bottom = 0.0f;
    for (int mask = 0; mask < 8; ++mask) {
        const float point[3]{
            runtime.header.boundsMin[0] +
                (runtime.header.boundsMax[0] - runtime.header.boundsMin[0]) * ((mask & 1) != 0),
            runtime.header.boundsMin[1] +
                (runtime.header.boundsMax[1] - runtime.header.boundsMin[1]) * ((mask & 2) != 0),
            runtime.header.boundsMin[2] +
                (runtime.header.boundsMax[2] - runtime.header.boundsMin[2]) * ((mask & 4) != 0),
        };
        const Preview3DPoint projected = ProjectPoint(point, world, view, projection);
        left = (std::min)(left, projected.x);
        top = (std::min)(top, projected.y);
        right = (std::max)(right, projected.x);
        bottom = (std::max)(bottom, projected.y);
    }
    frame.left = std::clamp(left - 0.012f, 0.0f, 1.0f);
    frame.top = std::clamp(top - 0.010f, 0.0f, 1.0f);
    frame.right = std::clamp(right + 0.012f, 0.0f, 1.0f);
    frame.bottom = std::clamp(bottom + 0.010f, 0.0f, 1.0f);
}

} // namespace

bool RenderPreview3D(ID3D11Device* device, ID3D11DeviceContext* context,
                     float elapsedSeconds, bool glowEnabled,
                     const float* glowColor, Preview3DFrame& frame) {
    frame = {};
    if (!device || !context || runtime.failed) return false;
    if (runtime.device.Get() != device) ShutdownPreview3D();
    if (!runtime.initialized) {
        runtime.initialized = Initialize(device);
        if (!runtime.initialized) {
            runtime.failed = true;
            return false;
        }
    }

    const float animationTime = std::fmod(
        (std::max)(0.0f, elapsedSeconds), runtime.header.duration);
    const uint32_t frameIndex = (std::min)(
        runtime.header.frameCount - 1,
        static_cast<uint32_t>(animationTime * runtime.header.fps));

    // A preview card needs a stable, front-facing hero-card composition.
    // Perspective made whichever leg was closer to the camera appear huge,
    // giving the model an unwanted bottom-up angle in the narrow portrait UI.
    const float yaw = XM_PI;
    // VRF has already converted Source coordinates to glTF metres/Y-up.
    const XMMATRIX world = XMMatrixTranslation(0.08f, -1.32f, 0.0f) *
                           XMMatrixRotationY(yaw);
    const XMMATRIX view = XMMatrixLookAtLH(
        XMVectorSet(0, 0.04f, -10.6f, 1), XMVectorSet(0, 0.04f, 0, 1),
        XMVectorSet(0, 1, 0, 0));
    const float heroCardHeight = 3.55f;
    const XMMATRIX projection = XMMatrixOrthographicLH(
        heroCardHeight * static_cast<float>(kTargetWidth) / kTargetHeight,
        heroCardHeight, 1.0f, 1000.0f);

    SceneConstants scene{};
    XMStoreFloat4x4(&scene.viewProjection,
                    XMMatrixTranspose(view * projection));
    scene.lightDirection = XMFLOAT4(-0.24f, -0.42f, 0.88f, 0.0f);
    scene.modelParameters = XMFLOAT4(std::cos(yaw), std::sin(yaw),
                                     0.08f, -1.32f);
    if (!UpdateBuffer(context, runtime.sceneBuffer.Get(), scene) ||
        !UpdateBones(context, frameIndex)) return false;

    const float clear[4]{0, 0, 0, 0};
    context->OMSetRenderTargets(1, runtime.targetView.GetAddressOf(),
                                runtime.depthView.Get());
    context->ClearRenderTargetView(runtime.targetView.Get(), clear);
    context->ClearDepthStencilView(runtime.depthView.Get(),
                                   D3D11_CLEAR_DEPTH, 1.0f, 0);
    const D3D11_VIEWPORT viewport{
        0, 0, static_cast<float>(kTargetWidth),
        static_cast<float>(kTargetHeight), 0, 1};
    const D3D11_RECT scissor{0, 0, static_cast<LONG>(kTargetWidth),
                             static_cast<LONG>(kTargetHeight)};
    context->RSSetViewports(1, &viewport);
    context->RSSetScissorRects(1, &scissor);
    context->RSSetState(runtime.rasterizer.Get());
    context->OMSetDepthStencilState(runtime.depthState.Get(), 0);
    const float blendFactor[4]{0, 0, 0, 0};
    context->OMSetBlendState(runtime.blendState.Get(), blendFactor, 0xffffffffu);

    const UINT stride = sizeof(AssetVertex), offset = 0;
    ID3D11Buffer* vertexBuffer = runtime.vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    context->IASetIndexBuffer(runtime.indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    context->IASetInputLayout(runtime.inputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(runtime.vertexShader.Get(), nullptr, 0);
    ID3D11Buffer* vertexConstants[]{runtime.sceneBuffer.Get(),
                                    runtime.boneBuffer.Get()};
    context->VSSetConstantBuffers(0, 2, vertexConstants);
    context->PSSetShader(runtime.pixelShader.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = runtime.sampler.Get();
    context->PSSetSamplers(0, 1, &sampler);

    if (glowEnabled && glowColor) {
        context->RSSetState(runtime.outlineRasterizer.Get());
        context->OMSetDepthStencilState(runtime.outlineDepthState.Get(), 0);
        MaterialConstants outline{};
        outline.baseFactor = XMFLOAT4(glowColor[0], glowColor[1],
                                      glowColor[2], 0.92f);
        outline.emissiveFactor = XMFLOAT4(0, 0, 0, 1);
        if (UpdateBuffer(context, runtime.materialBuffer.Get(), outline)) {
            ID3D11Buffer* outlineBuffer = runtime.materialBuffer.Get();
            context->VSSetConstantBuffers(2, 1, &outlineBuffer);
            context->PSSetConstantBuffers(2, 1, &outlineBuffer);
            for (const AssetPrimitive& primitive : runtime.primitives)
                context->DrawIndexed(primitive.indexCount,
                                     primitive.firstIndex, 0);
        }
        context->RSSetState(runtime.rasterizer.Get());
        context->OMSetDepthStencilState(runtime.depthState.Get(), 0);
    }

    for (const AssetPrimitive& primitive : runtime.primitives) {
        if (primitive.material >= runtime.materials.size()) continue;
        const AssetMaterial& material = runtime.materials[primitive.material];
        MaterialConstants constants{};
        std::memcpy(&constants.baseFactor, material.baseFactor,
                    sizeof(material.baseFactor));
        constants.emissiveFactor = XMFLOAT4(
            material.emissiveFactor[0], material.emissiveFactor[1],
            material.emissiveFactor[2], 0.0f);
        if (!UpdateBuffer(context, runtime.materialBuffer.Get(), constants))
            continue;
        ID3D11Buffer* materialBuffer = runtime.materialBuffer.Get();
        context->VSSetConstantBuffers(2, 1, &materialBuffer);
        context->PSSetConstantBuffers(2, 1, &materialBuffer);
        ID3D11ShaderResourceView* textures[2]{runtime.whiteTexture.Get(),
                                             runtime.whiteTexture.Get()};
        if (material.baseImage >= 0 &&
            static_cast<size_t>(material.baseImage) < runtime.images.size())
            textures[0] = runtime.images[material.baseImage].Get();
        if (material.emissiveImage >= 0 &&
            static_cast<size_t>(material.emissiveImage) < runtime.images.size())
            textures[1] = runtime.images[material.emissiveImage].Get();
        context->PSSetShaderResources(0, 2, textures);
        context->DrawIndexed(primitive.indexCount, primitive.firstIndex, 0);
    }

    ID3D11ShaderResourceView* nullTextures[2]{};
    context->PSSetShaderResources(0, 2, nullTextures);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    BuildFrameGeometry(frameIndex, world, view, projection, frame);
    frame.texture = runtime.targetTexture.Get();
    return true;
}

bool ReadPreview3DPixels(ID3D11DeviceContext* context,
                         std::vector<uint8_t>& pixels,
                         uint32_t& width, uint32_t& height,
                         uint32_t& stride) {
    width = kTargetWidth;
    height = kTargetHeight;
    stride = kTargetWidth * 4;
    if (!context || !runtime.initialized || !runtime.targetTexture ||
        !runtime.stagingTexture) return false;
    context->CopyResource(runtime.stagingTexture.Get(),
                          runtime.targetTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(runtime.stagingTexture.Get(), 0,
                            D3D11_MAP_READ, 0, &mapped))) return false;
    pixels.resize(static_cast<size_t>(stride) * height);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(pixels.data() + static_cast<size_t>(row) * stride,
                    static_cast<const uint8_t*>(mapped.pData) +
                        static_cast<size_t>(row) * mapped.RowPitch,
                    stride);
    }
    context->Unmap(runtime.stagingTexture.Get(), 0);
    return true;
}

void ShutdownPreview3D() {
    runtime = {};
}
