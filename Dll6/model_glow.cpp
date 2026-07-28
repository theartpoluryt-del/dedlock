#include "shared.h"
#include <MinHook.h>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

using DrawModelFn = void**(__fastcall*)(
    __int64, __int64, __int64*, int, __int64, __int64, __int64);
using PlayerOutlineFn = __int64(__fastcall*)(
    __int64, uint32_t*, float*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawIndexedInstancedIndirectFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);
using DrawInstancedIndirectFn = void(STDMETHODCALLTYPE*)(
    ID3D11DeviceContext*, ID3D11Buffer*, UINT);

DrawModelFn originalDrawModel = nullptr;
PlayerOutlineFn originalPlayerOutline = nullptr;
DrawIndexedFn originalDrawIndexed = nullptr;
DrawFn originalDraw = nullptr;
DrawIndexedInstancedFn originalDrawIndexedInstanced = nullptr;
DrawInstancedFn originalDrawInstanced = nullptr;
DrawIndexedInstancedIndirectFn originalDrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn originalDrawInstancedIndirect = nullptr;

void* drawModelTarget = nullptr;
void* playerOutlineTarget = nullptr;
void* drawIndexedTarget = nullptr;
void* drawTarget = nullptr;
void* drawIndexedInstancedTarget = nullptr;
void* drawInstancedTarget = nullptr;
void* drawIndexedInstancedIndirectTarget = nullptr;
void* drawInstancedIndirectTarget = nullptr;

ID3D11PixelShader* glowPixelShader = nullptr;
ID3D11DepthStencilState* glowDepthState = nullptr;
ID3D11BlendState* glowBlendState = nullptr;
ID3D11RasterizerState* glowRasterizerState = nullptr;

// DrawModel can submit work from one render worker while the D3D context
// executes it on another. A thread_local marker therefore made the second
// pass lose its state before the actual Draw* call.
std::atomic_bool renderGlowPass = false;
std::atomic_bool resourcesReady = false;
std::atomic_bool firstEnemyPassLogged = false;
std::atomic_bool drawModelLayoutLogged = false;
std::atomic_uint64_t drawCallCount = 0;
std::atomic_uint64_t glowDrawCallCount = 0;
std::atomic_uint64_t glowPipelineCount = 0;
std::atomic_uint64_t enemyBatchCount = 0;

constexpr char DrawModelPattern[] =
    "48 8B C4 53 57 41 54 48 81 EC D0 00 00";
constexpr char PlayerOutlinePattern[] =
    "4C 89 44 24 ? 48 89 54 24 ? 55 53 56 57 41 56 41 57 "
    "48 8D AC 24";
constexpr size_t MeshEntryStride = 0x68;
constexpr size_t MeshSceneObject = 0x18;
constexpr size_t SceneObjectOwner = 0xC0;
constexpr size_t MeshMaterialDescriptor = 0x08;
constexpr size_t MaterialDescriptorSize = 0x108;
constexpr size_t MaterialTintOffset = 0x04;
constexpr size_t MaterialAlphaOffset = 0x10;

void LogGlowHook(const char* message) {
    std::ofstream log(
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\model_glow.log",
        std::ios::app);
    if (log) log << message << '\n';
}

void LogGlowCounters() {
    std::ostringstream stream;
    stream << "counters draw=" << drawCallCount.load()
           << " enemyBatch=" << enemyBatchCount.load()
           << " glowDraw=" << glowDrawCallCount.load()
           << " glowPipeline=" << glowPipelineCount.load();
    LogGlowHook(stream.str().c_str());
}


uintptr_t FindPattern(HMODULE module, const char* pattern) {
    if (!module || !pattern) return 0;

    MODULEINFO info{};
    if (!GetModuleInformation(
            GetCurrentProcess(), module, &info, sizeof(info))) {
        return 0;
    }

    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token) {
        bytes.push_back(token == "?" ? -1 : static_cast<int>(
            std::strtoul(token.c_str(), nullptr, 16)));
    }
    if (bytes.empty() || bytes.size() > info.SizeOfImage) return 0;

    const auto* image = static_cast<const uint8_t*>(info.lpBaseOfDll);
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool matches = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 &&
                image[i + j] != static_cast<uint8_t>(bytes[j])) {
                matches = false;
                break;
            }
        }
        if (matches) return reinterpret_cast<uintptr_t>(image + i);
    }
    return 0;
}

bool IsEnemyHeroMesh(uintptr_t entry) {
    const uintptr_t sceneObject =
        Read<uintptr_t>(entry + MeshSceneObject);
    if (!sceneObject || !currentLocalPawn) return false;

    const uint32_t ownerHandle =
        Read<uint32_t>(sceneObject + SceneObjectOwner);
    const uintptr_t pawn = ResolveEntity(ownerHandle);
    if (!pawn || pawn == currentLocalPawn) return false;

    bool isHero = false;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        isHero = std::find(heroPawns.begin(), heroPawns.end(), pawn) !=
                 heroPawns.end();
    }
    if (!isHero) return false;

    const uint8_t localTeam =
        Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
    return localTeam >= 2 && localTeam <= 3 &&
           team >= 2 && team <= 3 && team != localTeam &&
           Read<int>(pawn + Offsets::Health) > 0 &&
           Read<uint8_t>(pawn + Offsets::LifeState) == 0;
}

bool IsEnemyOutlinePawn(uintptr_t pawn) {
    // This is the player's outline query, so keep the render callback free of
    // locks, entity scans, and transient health/team reads. Those operations
    // can stall the render worker and show up as visible outline jitter.
    return pawn != 0 && pawn != currentLocalPawn;
}

__int64 __fastcall HookPlayerOutline(
    __int64 pawn, uint32_t* color, float* width) {
    // Do not call the original query for enemy pawns. In this build the
    // original routine can clear the outline state during the same update,
    // which makes a forced outline alternate on and off between frames.
    if (glowEnabled && IsEnemyOutlinePawn(static_cast<uintptr_t>(pawn))) {
        // Mode 2 is the native through-wall player outline in this build.
        if (color) *color = 0xFF2EFF1Au;
        if (width) *width = 2.0f;
        return 2;
    }

    return originalPlayerOutline
        ? originalPlayerOutline(pawn, color, width) : 0;
}

struct SavedPipelineState {
    ID3D11PixelShader* pixelShader{};
    ID3D11DepthStencilState* depthState{};
    ID3D11BlendState* blendState{};
    ID3D11RasterizerState* rasterizerState{};
    UINT stencilRef{};
    FLOAT blendFactor[4]{};
    UINT sampleMask{};
};

bool BeginGlowPipeline(
    ID3D11DeviceContext* context, SavedPipelineState& saved) {
    if (!context || !resourcesReady.load(std::memory_order_acquire))
        return false;

    const uint64_t pipelineCount =
        glowPipelineCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((pipelineCount % 100000) == 0)
        LogGlowCounters();

    context->PSGetShader(&saved.pixelShader, nullptr, nullptr);
    context->OMGetDepthStencilState(&saved.depthState, &saved.stencilRef);
    context->OMGetBlendState(
        &saved.blendState, saved.blendFactor, &saved.sampleMask);
    context->RSGetState(&saved.rasterizerState);

    context->PSSetShader(glowPixelShader, nullptr, 0);
    context->OMSetDepthStencilState(glowDepthState, 0);
    const FLOAT factor[4] = {0.f, 0.f, 0.f, 0.f};
    context->OMSetBlendState(glowBlendState, factor, 0xFFFFFFFFu);
    context->RSSetState(glowRasterizerState);
    return true;
}

// SceneSystem can render through a deferred D3D11 context passed to DrawModel.
// The old implementation only toggled a global flag and changed the immediate
// context from Present, which misses that command stream entirely.  When the
// second model submission exposes a D3D11 context, change its state around the
// submission itself so the state is recorded in the same command list.
bool LooksLikeD3D11Context(void* value) {
    if (!value) return false;
    __try {
        auto** vtable = *reinterpret_cast<void***>(value);
        if (!vtable) return false;
        HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
        if (!d3d11) return false;
        MODULEINFO info{};
        if (!GetModuleInformation(GetCurrentProcess(), d3d11, &info,
                                  sizeof(info))) return false;
        const auto begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
        const auto end = begin + info.SizeOfImage;
        const auto first = reinterpret_cast<uintptr_t>(vtable[0]);
        // ID3D11DeviceContext's first methods are implemented by d3d11.dll.
        return first >= begin && first < end;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyReadableBytes(void* destination, const void* source, size_t size) {
    if (!destination || !source || size == 0) return false;
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(destination, 0, size);
        return false;
    }
}

void SetGlowDescriptorTint(std::array<uint8_t, MaterialDescriptorSize>& descriptor) {
    const Vector3 tint{0.08f, 1.0f, 0.18f};
    const float alpha = 1.0f;
    std::memcpy(descriptor.data() + MaterialTintOffset, &tint, sizeof(tint));
    std::memcpy(descriptor.data() + MaterialAlphaOffset, &alpha, sizeof(alpha));
}

void EndGlowPipeline(
    ID3D11DeviceContext* context, SavedPipelineState& saved) {
    context->PSSetShader(saved.pixelShader, nullptr, 0);
    context->OMSetDepthStencilState(saved.depthState, saved.stencilRef);
    context->OMSetBlendState(
        saved.blendState, saved.blendFactor, saved.sampleMask);
    context->RSSetState(saved.rasterizerState);

    if (saved.pixelShader) saved.pixelShader->Release();
    if (saved.depthState) saved.depthState->Release();
    if (saved.blendState) saved.blendState->Release();
    if (saved.rasterizerState) saved.rasterizerState->Release();
}

void STDMETHODCALLTYPE HookDrawIndexed(
    ID3D11DeviceContext* context, UINT indexCount,
    UINT startIndex, INT baseVertex) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDrawIndexed(context, indexCount, startIndex, baseVertex);
    if (overridden) EndGlowPipeline(context, saved);
}

void STDMETHODCALLTYPE HookDraw(
    ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDraw(context, vertexCount, startVertex);
    if (overridden) EndGlowPipeline(context, saved);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(
    ID3D11DeviceContext* context, UINT indexCountPerInstance,
    UINT instanceCount, UINT startIndex, INT baseVertex,
    UINT startInstance) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDrawIndexedInstanced(
        context, indexCountPerInstance, instanceCount, startIndex,
        baseVertex, startInstance);
    if (overridden) EndGlowPipeline(context, saved);
}

void STDMETHODCALLTYPE HookDrawInstanced(
    ID3D11DeviceContext* context, UINT vertexCountPerInstance,
    UINT instanceCount, UINT startVertex, UINT startInstance) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDrawInstanced(
        context, vertexCountPerInstance, instanceCount,
        startVertex, startInstance);
    if (overridden) EndGlowPipeline(context, saved);
}

void STDMETHODCALLTYPE HookDrawIndexedInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT alignedOffset) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDrawIndexedInstancedIndirect(context, args, alignedOffset);
    if (overridden) EndGlowPipeline(context, saved);
}

void STDMETHODCALLTYPE HookDrawInstancedIndirect(
    ID3D11DeviceContext* context, ID3D11Buffer* args, UINT alignedOffset) {
    drawCallCount.fetch_add(1, std::memory_order_relaxed);
    if (renderGlowPass.load(std::memory_order_acquire))
        glowDrawCallCount.fetch_add(1, std::memory_order_relaxed);
    SavedPipelineState saved{};
    const bool overridden =
        renderGlowPass.load(std::memory_order_acquire) &&
            BeginGlowPipeline(context, saved);
    originalDrawInstancedIndirect(context, args, alignedOffset);
    if (overridden) EndGlowPipeline(context, saved);
}

void** __fastcall HookDrawModel(
    __int64 sceneObjectDesc, __int64 dx11, __int64* meshDraws,
    int meshCount, __int64 sceneView, __int64 sceneLayer, __int64 a7) {
    void** result = originalDrawModel(
        sceneObjectDesc, dx11, meshDraws, meshCount,
        sceneView, sceneLayer, a7);

    if (!glowEnabled || !meshDraws || meshCount <= 0 ||
        !resourcesReady.load(std::memory_order_acquire)) {
        return result;
    }

    const int safeCount = meshCount < 256 ? meshCount : 256;
    struct alignas(8) MeshEntry {
        std::array<uint8_t, MeshEntryStride> bytes;
    };
    struct alignas(8) MaterialDescriptor {
        std::array<uint8_t, MaterialDescriptorSize> bytes;
    };
    std::vector<MeshEntry> enemyMeshes;
    std::vector<MaterialDescriptor> glowDescriptors;
    enemyMeshes.reserve(static_cast<size_t>(safeCount));
    glowDescriptors.reserve(static_cast<size_t>(safeCount));

    const uintptr_t source = reinterpret_cast<uintptr_t>(meshDraws);
    for (int i = 0; i < safeCount; ++i) {
        const uintptr_t entry =
            source + static_cast<uintptr_t>(i) * MeshEntryStride;
        if (!IsEnemyHeroMesh(entry)) continue;
        MeshEntry copy{};
        if (!CopyReadableBytes(copy.bytes.data(),
                               reinterpret_cast<const void*>(entry), MeshEntryStride))
            continue;
        const uintptr_t descriptor = Read<uintptr_t>(entry + MeshMaterialDescriptor);
        if (!descriptor || !CopyReadableBytes(
                glowDescriptors.emplace_back().bytes.data(),
                reinterpret_cast<const void*>(descriptor), MaterialDescriptorSize)) {
            if (!glowDescriptors.empty()) glowDescriptors.pop_back();
            continue;
        }
        SetGlowDescriptorTint(glowDescriptors.back().bytes);
        const uintptr_t descriptorCopy = reinterpret_cast<uintptr_t>(
            glowDescriptors.back().bytes.data());
        std::memcpy(copy.bytes.data() + MeshMaterialDescriptor,
                    &descriptorCopy, sizeof(descriptorCopy));
        enemyMeshes.push_back(copy);
    }

    if (!enemyMeshes.empty()) {
        enemyBatchCount.fetch_add(1, std::memory_order_relaxed);
        if (!firstEnemyPassLogged.exchange(true))
            LogGlowHook("first enemy model glow pass submitted with descriptor tint override");
        if (!drawModelLayoutLogged.exchange(true)) {
            std::ostringstream layout;
            layout << "DrawModel args dx11=0x" << std::hex
                   << static_cast<uintptr_t>(dx11)
                   << " sceneObject=0x" << static_cast<uintptr_t>(sceneObjectDesc)
                   << " meshDraws=0x" << reinterpret_cast<uintptr_t>(meshDraws)
                   << " count=" << std::dec << meshCount;
            LogGlowHook(layout.str().c_str());
            const uintptr_t entry = reinterpret_cast<uintptr_t>(meshDraws);
            for (size_t offset = 0; offset < MeshEntryStride; offset += 8) {
                std::ostringstream value;
                value << "mesh[0]+0x" << std::hex << offset << "=0x"
                      << Read<uintptr_t>(entry + offset);
                LogGlowHook(value.str().c_str());
            }
            constexpr size_t pointerOffsets[] = {
                0x00, 0x08, 0x18, 0x20, 0x30, 0x40, 0x48};
            constexpr size_t objectOffsets[] = {
                0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38,
                0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x70, 0x80,
                0x90, 0xA0, 0xB0, 0xC0, 0xD0, 0xE0, 0xF0, 0x100,
                0x108, 0x110};
            for (const size_t meshOffset : pointerOffsets) {
                const uintptr_t object = Read<uintptr_t>(entry + meshOffset);
                if (!object) continue;
                std::ostringstream header;
                header << "object mesh+0x" << std::hex << meshOffset
                       << " ptr=0x" << object;
                LogGlowHook(header.str().c_str());
                for (const size_t objectOffset : objectOffsets) {
                    std::ostringstream value;
                    value << "  +0x" << std::hex << objectOffset << "=0x"
                          << Read<uintptr_t>(object + objectOffset);
                    LogGlowHook(value.str().c_str());
                }
            }
            if (sceneObjectDesc) {
                for (size_t offset = 0xA0; offset <= 0xE0; offset += 8) {
                    std::ostringstream value;
                    value << "scene+0x" << std::hex << offset << "=0x"
                          << Read<uintptr_t>(static_cast<uintptr_t>(sceneObjectDesc) + offset);
                    LogGlowHook(value.str().c_str());
                }
            }
        }
        SavedPipelineState saved{};
        auto* submittedContext = LooksLikeD3D11Context(
            reinterpret_cast<void*>(dx11))
            ? reinterpret_cast<ID3D11DeviceContext*>(dx11) : nullptr;
        if (drawModelLayoutLogged.load(std::memory_order_acquire)) {
            static std::atomic_bool contextLogged = false;
            if (!contextLogged.exchange(true))
                LogGlowHook(submittedContext
                    ? "DrawModel dx11 is a D3D11 context"
                    : "DrawModel dx11 is not a D3D11 context");
        }
        const bool overridden = submittedContext &&
            BeginGlowPipeline(submittedContext, saved);

        renderGlowPass.store(!submittedContext, std::memory_order_release);
        originalDrawModel(
            sceneObjectDesc, dx11,
            reinterpret_cast<__int64*>(enemyMeshes.data()),
            static_cast<int>(enemyMeshes.size()),
            sceneView, sceneLayer, a7);
        renderGlowPass.store(false, std::memory_order_release);

        if (overridden) EndGlowPipeline(submittedContext, saved);
    }
    return result;
}

bool CreateGlowResources() {
    if (resourcesReady.load(std::memory_order_acquire)) return true;
    if (!pDevice) return false;

    constexpr char shaderSource[] =
        "float4 main() : SV_Target {"
        " return float4(0.10, 1.00, 0.18, 0.62);"
        "}";
    ID3DBlob* shaderBlob = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT compileResult = D3DCompile(
        shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr,
        "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shaderBlob, &errors);
    if (errors) errors->Release();
    if (FAILED(compileResult) || !shaderBlob) {
        LogGlowHook("glow pixel shader compilation failed");
        return false;
    }

    HRESULT result = pDevice->CreatePixelShader(
        shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
        nullptr, &glowPixelShader);
    shaderBlob->Release();
    if (FAILED(result) || !glowPixelShader) {
        LogGlowHook("glow pixel shader creation failed");
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = FALSE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    result = pDevice->CreateDepthStencilState(&depth, &glowDepthState);
    if (FAILED(result) || !glowDepthState) {
        LogGlowHook("glow depth state creation failed");
        return false;
    }

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
    result = pDevice->CreateBlendState(&blend, &glowBlendState);
    if (FAILED(result) || !glowBlendState) {
        LogGlowHook("glow blend state creation failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    result = pDevice->CreateRasterizerState(
        &rasterizer, &glowRasterizerState);
    if (FAILED(result) || !glowRasterizerState) {
        LogGlowHook("glow rasterizer state creation failed");
        return false;
    }

    resourcesReady.store(true, std::memory_order_release);
    return true;
}

template<typename HookType>
bool InstallContextHook(
    void*& target, void* candidate, void* hook, HookType& original) {
    if (target) return true;
    const MH_STATUS created = MH_CreateHook(
        candidate, hook, reinterpret_cast<void**>(&original));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enabled = MH_EnableHook(candidate);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(candidate);
        original = nullptr;
        return false;
    }
    target = candidate;
    return true;
}

bool InstallDrawHooks() {
    if (!pContext || !CreateGlowResources()) return false;
    void** vtable = *reinterpret_cast<void***>(pContext);
    if (!vtable) return false;

    const bool indexed = InstallContextHook(
        drawIndexedTarget, vtable[12],
        reinterpret_cast<void*>(&HookDrawIndexed), originalDrawIndexed);
    const bool draw = InstallContextHook(
        drawTarget, vtable[13],
        reinterpret_cast<void*>(&HookDraw), originalDraw);
    const bool indexedInstanced = InstallContextHook(
        drawIndexedInstancedTarget, vtable[20],
        reinterpret_cast<void*>(&HookDrawIndexedInstanced),
        originalDrawIndexedInstanced);
    const bool instanced = InstallContextHook(
        drawInstancedTarget, vtable[21],
        reinterpret_cast<void*>(&HookDrawInstanced),
        originalDrawInstanced);
    const bool indexedIndirect = InstallContextHook(
        drawIndexedInstancedIndirectTarget, vtable[39],
        reinterpret_cast<void*>(&HookDrawIndexedInstancedIndirect),
        originalDrawIndexedInstancedIndirect);
    const bool indirect = InstallContextHook(
        drawInstancedIndirectTarget, vtable[40],
        reinterpret_cast<void*>(&HookDrawInstancedIndirect),
        originalDrawInstancedIndirect);
    return indexed && draw && indexedInstanced && instanced &&
        indexedIndirect && indirect;
}

void RemoveHookTarget(void*& target) {
    if (!target) return;
    MH_DisableHook(target);
    MH_RemoveHook(target);
    target = nullptr;
}

} // namespace

bool InstallModelGlowHook() {
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        LogGlowHook("MinHook initialization failed");
        return false;
    }

    if (!playerOutlineTarget) {
        HMODULE client = GetModuleHandleA("client.dll");
        const uintptr_t candidate = client
            ? FindPattern(client, PlayerOutlinePattern) : 0;
        if (candidate) {
            std::ostringstream stream;
            stream << "client PlayerOutline candidate=0x"
                   << std::hex << candidate;
            LogGlowHook(stream.str().c_str());
        }
        if (candidate && InstallContextHook(
                playerOutlineTarget,
                reinterpret_cast<void*>(candidate),
                reinterpret_cast<void*>(&HookPlayerOutline),
                originalPlayerOutline)) {
            LogGlowHook("client PlayerOutline hook installed");
        } else {
            LogGlowHook("client PlayerOutline pattern not found or hook failed");
        }
    }
    return playerOutlineTarget != nullptr;
}

void RemoveModelGlowHook() {
    resourcesReady.store(false, std::memory_order_release);
    RemoveHookTarget(drawInstancedTarget);
    RemoveHookTarget(drawInstancedIndirectTarget);
    RemoveHookTarget(drawIndexedInstancedIndirectTarget);
    RemoveHookTarget(drawIndexedInstancedTarget);
    RemoveHookTarget(drawTarget);
    RemoveHookTarget(drawIndexedTarget);
    RemoveHookTarget(drawModelTarget);
    RemoveHookTarget(playerOutlineTarget);

    originalDrawInstanced = nullptr;
    originalDrawInstancedIndirect = nullptr;
    originalDrawIndexedInstancedIndirect = nullptr;
    originalDrawIndexedInstanced = nullptr;
    originalDraw = nullptr;
    originalDrawIndexed = nullptr;
    originalDrawModel = nullptr;
    originalPlayerOutline = nullptr;

    if (glowRasterizerState) glowRasterizerState->Release();
    if (glowBlendState) glowBlendState->Release();
    if (glowDepthState) glowDepthState->Release();
    if (glowPixelShader) glowPixelShader->Release();
    glowRasterizerState = nullptr;
    glowBlendState = nullptr;
    glowDepthState = nullptr;
    glowPixelShader = nullptr;
}
