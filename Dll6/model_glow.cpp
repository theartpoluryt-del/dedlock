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
using PlayerHealthGlowRenderFn = void(__fastcall*)(
    void*, void*, void*, void*);
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
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*, UINT, ID3D11DeviceContext**);

DrawModelFn originalDrawModel = nullptr;
PlayerOutlineFn originalPlayerOutline = nullptr;
PlayerHealthGlowRenderFn originalPlayerHealthGlowRender = nullptr;
DrawIndexedFn originalDrawIndexed = nullptr;
DrawFn originalDraw = nullptr;
DrawIndexedInstancedFn originalDrawIndexedInstanced = nullptr;
DrawInstancedFn originalDrawInstanced = nullptr;
DrawIndexedInstancedIndirectFn originalDrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn originalDrawInstancedIndirect = nullptr;
CreateDeferredContextFn originalCreateDeferredContext = nullptr;

void* drawModelTarget = nullptr;
void* playerOutlineTarget = nullptr;
void* playerHealthGlowRenderTarget = nullptr;
void* drawIndexedTarget = nullptr;
void* drawTarget = nullptr;
void* drawIndexedInstancedTarget = nullptr;
void* drawInstancedTarget = nullptr;
void* drawIndexedInstancedIndirectTarget = nullptr;
void* drawInstancedIndirectTarget = nullptr;
void* createDeferredContextTarget = nullptr;

ID3D11PixelShader* glowPixelShader = nullptr;
ID3D11Buffer* glowColorBuffer = nullptr;
ID3D11DepthStencilState* glowDepthState = nullptr;
ID3D11BlendState* glowBlendState = nullptr;
ID3D11RasterizerState* glowRasterizerState = nullptr;

// DrawModel can submit work from one render worker while the D3D context
// executes it on another. A thread_local marker therefore made the second
// pass lose its state before the actual Draw* call.
std::atomic_bool renderGlowPass = false;
std::atomic_int renderGlowTeam = -1;
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
constexpr char PlayerHealthGlowRenderPattern[] =
    "48 8B C4 4C 89 48 20 48 89 48 08 55 48 8D A8 ? ? ? ? "
    "48 81 EC 20 06 00 00";
constexpr size_t MeshEntryStride = 0x68;
constexpr size_t MeshSceneObject = 0x18;
constexpr size_t SceneObjectOwner = 0xC0;
constexpr size_t MeshMaterialDescriptor = 0x08;
constexpr size_t MaterialDescriptorSize = 0x108;
constexpr size_t MaterialTintOffset = 0x04;
constexpr size_t MaterialAlphaOffset = 0x10;
// The native outline manager owns both selectable modes. In the current
// client, CPlayerHealthGlowRenderer consumes only manager entries with mode 1;
// mode 2 bypasses the health-clipped pass and marks the full model instead.
// Keep the experimental duplicate DrawModel pass out of the render path.
constexpr bool EnableExperimentalModelGlowPass = false;

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

bool InstallDrawHooksOnContext(ID3D11DeviceContext* context);

HRESULT STDMETHODCALLTYPE HookCreateDeferredContext(
    ID3D11Device* device, UINT flags, ID3D11DeviceContext** context) {
    if (!originalCreateDeferredContext)
        return E_FAIL;
    const HRESULT result = originalCreateDeferredContext(device, flags, context);
    if (SUCCEEDED(result) && context && *context) {
        if (InstallDrawHooksOnContext(*context))
            LogGlowHook("deferred D3D11 context draw hooks installed");
        else
            LogGlowHook("deferred D3D11 context draw hooks failed");
    }
    return result;
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
    uintptr_t found = 0;
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool matches = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 &&
                image[i + j] != static_cast<uint8_t>(bytes[j])) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (found) return 0;
        found = reinterpret_cast<uintptr_t>(image + i);
    }
    return found;
}

uintptr_t GetEnemyHeroMeshPawn(uintptr_t entry) {
    const uintptr_t sceneObject =
        Read<uintptr_t>(entry + MeshSceneObject);
    if (!sceneObject || !currentLocalPawn) return 0;

    const uint32_t ownerHandle =
        Read<uint32_t>(sceneObject + SceneObjectOwner);
    const uintptr_t pawn = ResolveEntity(ownerHandle);
    if (!pawn || pawn == currentLocalPawn) return 0;

    bool isHero = false;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        isHero = std::find(heroPawns.begin(), heroPawns.end(), pawn) !=
                 heroPawns.end();
    }
    if (!isHero) return 0;

    const uint8_t localTeam =
        Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
    return localTeam >= 2 && localTeam <= 3 &&
           team >= 2 && team <= 3 && team != localTeam &&
           Read<int>(pawn + Offsets::Health) > 0 &&
           Read<uint8_t>(pawn + Offsets::LifeState) == 0 ? pawn : 0;
}

uintptr_t GetGlowHeroMeshPawn(uintptr_t entry) {
    const uintptr_t sceneObject = Read<uintptr_t>(entry + MeshSceneObject);
    if (!sceneObject || !currentLocalPawn) return 0;

    const uint32_t ownerHandle = Read<uint32_t>(sceneObject + SceneObjectOwner);
    const uintptr_t pawn = ResolveEntity(ownerHandle);
    if (!pawn || pawn == currentLocalPawn) return 0;

    bool isHero = false;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        isHero = std::find(heroPawns.begin(), heroPawns.end(), pawn) != heroPawns.end();
    }
    if (!isHero) return 0;

    const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
    const int health = Read<int>(pawn + Offsets::Health);
    const uint8_t lifeState = Read<uint8_t>(pawn + Offsets::LifeState);
    return (team == 2 || team == 3) && health > 0 && lifeState == 0 ? pawn : 0;
}

bool IsGlowEnabledForPawn(uintptr_t pawn) {
    if (!pawn) return false;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const uint8_t pawnTeam = Read<uint8_t>(pawn + Offsets::Team);
    if (pawnTeam != 2 && pawnTeam != 3) return false;
    const bool ally = localTeam >= 2 && localTeam <= 3 && pawnTeam == localTeam;
    return ally ? allyGlowEnabled : enemyGlowEnabled;
}

bool IsEnemyHeroMesh(uintptr_t entry) {
    const uintptr_t pawn = GetGlowHeroMeshPawn(entry);
    return pawn != 0 && IsGlowEnabledForPawn(pawn);
}

bool IsEnemyOutlinePawn(uintptr_t pawn) {
    // This is the player's outline query, so keep the render callback free of
    // locks, entity scans, and transient health/team reads. Those operations
    // can stall the render worker and show up as visible outline jitter.
    return pawn != 0 && pawn != currentLocalPawn;
}

static uint32_t GlowPackedColor(const float color[4]) {
    const uint32_t r = static_cast<uint32_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f);
    const uint32_t g = static_cast<uint32_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f);
    const uint32_t b = static_cast<uint32_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f);
    const uint32_t a = static_cast<uint32_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

__int64 __fastcall HookPlayerOutline(
    __int64 pawn, uint32_t* color, float* width) {
    const __int64 originalResult = originalPlayerOutline
        ? originalPlayerOutline(pawn, color, width) : 0;

    if (IsEnemyOutlinePawn(static_cast<uintptr_t>(pawn))) {
        const uint8_t localTeam = currentLocalPawn
            ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
        const uint8_t pawnTeam = Read<uint8_t>(static_cast<uintptr_t>(pawn) + Offsets::Team);
        const bool validTeam = pawnTeam == 2 || pawnTeam == 3;
        const bool ally = localTeam >= 2 && localTeam <= 3 && pawnTeam == localTeam;
        const bool teamGlowEnabled = ally ? allyGlowEnabled : enemyGlowEnabled;
        if (!validTeam || !teamGlowEnabled)
            return originalResult;
        const float* glowColor = ally ? teammateGlowColor : enemyGlowColor;

        float adjusted[4] = {
            glowColor[0], glowColor[1], glowColor[2], glowColor[3]};
        if (color) *color = GlowPackedColor(adjusted);
        if (width) *width = 4.0f;

        // Verified against the current client call chain:
        //   mode 1 -> CPlayerHealthGlowRenderer (height clipped by health)
        //   mode 2 -> complete-model highlight, no health clipping
        const int teamGlowMode = ally ? allyGlowMode : enemyGlowMode;
        return teamGlowMode == 1 ? 2 : 1;
    }

    return originalResult;
}

// PlayerHealthGlowRenderer converts mode-1 manager entries into the vertical
// health fill. Normal fill uses mode 2 and therefore never enters that path.
void __fastcall HookPlayerHealthGlowRender(
    void* renderer, void* arg1, void* arg2, void* arg3) {
    if (originalPlayerHealthGlowRender)
        originalPlayerHealthGlowRender(renderer, arg1, arg2, arg3);
}

struct SavedPipelineState {
    ID3D11PixelShader* pixelShader{};
    ID3D11Buffer* pixelConstantBuffer{};
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
    context->PSGetConstantBuffers(0, 1, &saved.pixelConstantBuffer);
    context->OMGetDepthStencilState(&saved.depthState, &saved.stencilRef);
    context->OMGetBlendState(
        &saved.blendState, saved.blendFactor, &saved.sampleMask);
    context->RSGetState(&saved.rasterizerState);

    context->PSSetShader(glowPixelShader, nullptr, 0);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(glowColorBuffer, 0, D3D11_MAP_WRITE_DISCARD,
                               0, &mapped))) {
        const float* glowColor = renderGlowTeam.load(std::memory_order_acquire) == 1
            ? teammateGlowColor : enemyGlowColor;
        const float color[4] = {
            glowColor[0], glowColor[1], glowColor[2], glowColor[3]};
        std::memcpy(mapped.pData, color, sizeof(color));
        context->Unmap(glowColorBuffer, 0);
    }
    context->PSSetConstantBuffers(0, 1, &glowColorBuffer);
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

void SetGlowDescriptorTint(
    std::array<uint8_t, MaterialDescriptorSize>& descriptor,
    const float glowColor[4]) {
    const Vector3 tint{glowColor[0], glowColor[1], glowColor[2]};
    const float alpha = glowColor[3];
    std::memcpy(descriptor.data() + MaterialTintOffset, &tint, sizeof(tint));
    std::memcpy(descriptor.data() + MaterialAlphaOffset, &alpha, sizeof(alpha));
}

void EndGlowPipeline(
    ID3D11DeviceContext* context, SavedPipelineState& saved) {
    context->PSSetShader(saved.pixelShader, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &saved.pixelConstantBuffer);
    context->OMSetDepthStencilState(saved.depthState, saved.stencilRef);
    context->OMSetBlendState(
        saved.blendState, saved.blendFactor, saved.sampleMask);
    context->RSSetState(saved.rasterizerState);

    if (saved.pixelShader) saved.pixelShader->Release();
    if (saved.pixelConstantBuffer) saved.pixelConstantBuffer->Release();
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

    if (!EnableExperimentalModelGlowPass ||
        enemyGlowMode != 1 && allyGlowMode != 1 || !meshDraws || meshCount <= 0 ||
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
    int batchTeam = -1;
    enemyMeshes.reserve(static_cast<size_t>(safeCount));
    glowDescriptors.reserve(static_cast<size_t>(safeCount));

    const uintptr_t source = reinterpret_cast<uintptr_t>(meshDraws);
    for (int i = 0; i < safeCount; ++i) {
        const uintptr_t entry =
            source + static_cast<uintptr_t>(i) * MeshEntryStride;
        const uintptr_t pawn = GetGlowHeroMeshPawn(entry);
        if (!pawn || !IsGlowEnabledForPawn(pawn)) continue;
        const uint8_t localTeam = currentLocalPawn
            ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
        const uint8_t pawnTeam = Read<uint8_t>(pawn + Offsets::Team);
        const bool ally = localTeam >= 2 && localTeam <= 3 && pawnTeam == localTeam;
        if (batchTeam < 0) batchTeam = ally ? 1 : 0;
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
        SetGlowDescriptorTint(
            glowDescriptors.back().bytes,
            ally ? teammateGlowColor : enemyGlowColor);
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

        renderGlowTeam.store(batchTeam, std::memory_order_release);
        renderGlowPass.store(!submittedContext, std::memory_order_release);
        originalDrawModel(
            sceneObjectDesc, dx11,
            reinterpret_cast<__int64*>(enemyMeshes.data()),
            static_cast<int>(enemyMeshes.size()),
            sceneView, sceneLayer, a7);
        renderGlowPass.store(false, std::memory_order_release);
        renderGlowTeam.store(-1, std::memory_order_release);

        if (overridden) EndGlowPipeline(submittedContext, saved);
    }
    return result;
}

bool CreateGlowResources() {
    if (resourcesReady.load(std::memory_order_acquire)) return true;
    if (!pDevice) return false;

    constexpr char shaderSource[] =
        "cbuffer GlowColor : register(b0) { float4 color; };"
        "float4 main() : SV_Target {"
        " return color;"
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

    D3D11_BUFFER_DESC colorBufferDesc{};
    colorBufferDesc.ByteWidth = 16;
    colorBufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    colorBufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    colorBufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    result = pDevice->CreateBuffer(&colorBufferDesc, nullptr, &glowColorBuffer);
    if (FAILED(result) || !glowColorBuffer) {
        LogGlowHook("glow color constant buffer creation failed");
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

bool InstallDrawHooksOnContext(ID3D11DeviceContext* context) {
    if (!context || !CreateGlowResources()) return false;
    void** vtable = *reinterpret_cast<void***>(context);
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
    bool deviceHook = true;
    if (pDevice && !createDeferredContextTarget) {
        void** deviceVtable = *reinterpret_cast<void***>(pDevice);
        if (deviceVtable) {
            deviceHook = InstallContextHook(
                createDeferredContextTarget, deviceVtable[27],
                reinterpret_cast<void*>(&HookCreateDeferredContext),
                originalCreateDeferredContext);
        }
    }
    return indexed && draw && indexedInstanced && instanced &&
        indexedIndirect && indirect && deviceHook;
}

bool InstallDrawHooks() {
    return InstallDrawHooksOnContext(pContext);
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

    if (!drawModelTarget) {
        // The current model submission is emitted by scenesystem.dll. Keep
        // engine2/client as compatibility fallbacks for older builds.
        HMODULE scene = GetModuleHandleA("scenesystem.dll");
        HMODULE sceneSystem = GetModuleHandleA("engine2.dll");
        HMODULE client = GetModuleHandleA("client.dll");
        uintptr_t drawCandidate = scene
            ? FindPattern(scene, DrawModelPattern) : 0;
        if (!drawCandidate && sceneSystem)
            drawCandidate = FindPattern(sceneSystem, DrawModelPattern);
        if (!drawCandidate && client)
            drawCandidate = FindPattern(client, DrawModelPattern);
        if (drawCandidate && InstallContextHook(
                drawModelTarget,
                reinterpret_cast<void*>(drawCandidate),
                reinterpret_cast<void*>(&HookDrawModel),
                originalDrawModel)) {
            LogGlowHook("SceneSystem DrawModel hook installed");
        } else {
            LogGlowHook("SceneSystem DrawModel pattern not found or hook failed");
        }
    }

    if (pContext && InstallDrawHooks())
        LogGlowHook("D3D model glow draw hooks installed");
    else
        LogGlowHook("D3D model glow draw hooks unavailable");

    if (!playerHealthGlowRenderTarget) {
        HMODULE client = GetModuleHandleA("client.dll");
        const uintptr_t renderCandidate = client
            ? FindPattern(client, PlayerHealthGlowRenderPattern) : 0;
        if (renderCandidate && InstallContextHook(
                playerHealthGlowRenderTarget,
                reinterpret_cast<void*>(renderCandidate),
                reinterpret_cast<void*>(&HookPlayerHealthGlowRender),
                originalPlayerHealthGlowRender)) {
            LogGlowHook("PlayerHealthGlowRenderer render hook installed");
        } else {
            LogGlowHook("PlayerHealthGlowRenderer render hook failed");
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
    RemoveHookTarget(createDeferredContextTarget);
    RemoveHookTarget(playerOutlineTarget);
    RemoveHookTarget(playerHealthGlowRenderTarget);

    originalDrawInstanced = nullptr;
    originalDrawInstancedIndirect = nullptr;
    originalDrawIndexedInstancedIndirect = nullptr;
    originalDrawIndexedInstanced = nullptr;
    originalDraw = nullptr;
    originalDrawIndexed = nullptr;
    originalDrawModel = nullptr;
    originalCreateDeferredContext = nullptr;
    originalPlayerOutline = nullptr;
    originalPlayerHealthGlowRender = nullptr;

    if (glowRasterizerState) glowRasterizerState->Release();
    if (glowBlendState) glowBlendState->Release();
    if (glowDepthState) glowDepthState->Release();
    if (glowPixelShader) glowPixelShader->Release();
    if (glowColorBuffer) glowColorBuffer->Release();
    glowRasterizerState = nullptr;
    glowBlendState = nullptr;
    glowDepthState = nullptr;
    glowPixelShader = nullptr;
    glowColorBuffer = nullptr;
}
