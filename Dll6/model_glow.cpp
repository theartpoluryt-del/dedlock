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
using GeneratePrimitivesFn = void(__fastcall*)(
    uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using PlayerOutlineFn = __int64(__fastcall*)(
    __int64, uint32_t*, float*);
using OutlineHealthFractionFn = float(__fastcall*)(__int64);
using PlayerHealthGlowRenderFn = void(__fastcall*)(
    void*, void*, void*, void*);
using GlowCompositeFn = void(__fastcall*)(
    void*, int, int, int, int, int, int, int);
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
using RenderDepthStateFn = void(__fastcall*)(uintptr_t, void*, uint32_t);
using CreateInterfaceFn = void*(__cdecl*)(const char*, int*);
struct KeyValues3Storage {
    alignas(16) std::array<uint8_t, 0x20> bytes{};
};
struct KV3ID {
    const char* name;
    uint64_t key1;
    uint64_t key2;
};
using LoadKV3Fn = bool(__fastcall*)(
    KeyValues3Storage*, void*, const char*, const KV3ID&, const char*, uint32_t);
using CreateMaterialFn = void(__fastcall*)(
    void*, void*, const char*, KeyValues3Storage*, int, unsigned char);

DrawModelFn originalDrawModel = nullptr;
GeneratePrimitivesFn originalGeneratePrimitives = nullptr;
PlayerOutlineFn originalPlayerOutline = nullptr;
OutlineHealthFractionFn originalOutlineHealthFraction = nullptr;
PlayerHealthGlowRenderFn originalPlayerHealthGlowRender = nullptr;
GlowCompositeFn originalGlowComposite = nullptr;
DrawIndexedFn originalDrawIndexed = nullptr;
DrawFn originalDraw = nullptr;
DrawIndexedInstancedFn originalDrawIndexedInstanced = nullptr;
DrawInstancedFn originalDrawInstanced = nullptr;
DrawIndexedInstancedIndirectFn originalDrawIndexedInstancedIndirect = nullptr;
DrawInstancedIndirectFn originalDrawInstancedIndirect = nullptr;
CreateDeferredContextFn originalCreateDeferredContext = nullptr;
RenderDepthStateFn originalRenderDepthState = nullptr;

void* drawModelTarget = nullptr;
void* generatePrimitivesTarget = nullptr;
void* playerOutlineTarget = nullptr;
void* outlineHealthFractionTarget = nullptr;
void* playerHealthGlowRenderTarget = nullptr;
void* glowCompositeTarget = nullptr;
void* drawIndexedTarget = nullptr;
void* drawTarget = nullptr;
void* drawIndexedInstancedTarget = nullptr;
void* drawInstancedTarget = nullptr;
void* drawIndexedInstancedIndirectTarget = nullptr;
void* drawInstancedIndirectTarget = nullptr;
void* createDeferredContextTarget = nullptr;
void* renderDepthStateTarget = nullptr;

ID3D11PixelShader* glowPixelShader = nullptr;
ID3D11Buffer* glowColorBuffer = nullptr;
ID3D11DepthStencilState* glowDepthState = nullptr;
ID3D11DepthStencilState* invisibleChamsDepthState = nullptr;
ID3D11BlendState* glowBlendState = nullptr;
ID3D11RasterizerState* glowRasterizerState = nullptr;
void* flatChamsMaterial = nullptr;
void* invisibleChamsMaterial = nullptr;

// DrawModel can submit work from one render worker while the D3D context
// executes it on another. A thread_local marker therefore made the second
// pass lose its state before the actual Draw* call.
std::atomic_bool renderGlowPass = false;
std::atomic_int renderGlowTeam = -1;
std::atomic_bool resourcesReady = false;
std::atomic_bool firstEnemyPassLogged = false;
std::atomic_bool drawModelLayoutLogged = false;
std::atomic_bool renderDepthProbeLogged = false;
std::atomic_bool renderDepthHookInstalling = false;
std::atomic_int nativeGlowRenderPassCount = 0;
std::atomic_bool glowCompositeHookInstalling = false;
thread_local bool nativeGlowCompositeActive = false;
std::atomic_uintptr_t renderInvisibleChamsContext = 0;
std::atomic_uintptr_t invisiblePassOriginalDepth = 0;
std::atomic_uint32_t invisiblePassOriginalStencilRef = 0;
std::atomic_bool invisiblePassDepthCaptured = false;
std::mutex invisiblePassMutex;
std::atomic_uint64_t drawCallCount = 0;
std::atomic_uint64_t glowDrawCallCount = 0;
std::atomic_uint64_t glowPipelineCount = 0;
std::atomic_uint64_t enemyBatchCount = 0;

constexpr char DrawModelPattern[] =
    "48 8B C4 53 57 41 54 48 81 EC D0 00 00";
constexpr char GeneratePrimitivesVtablePattern[] =
    "48 8D 05 ? ? ? ? 48 89 07 48 8B 7C 24 48";
constexpr char PlayerOutlinePattern[] =
    "4C 89 44 24 ? 48 89 54 24 ? 55 53 56 57 41 56 41 57 "
    "48 8D AC 24";
constexpr char OutlineHealthFractionPattern[] =
    "40 53 48 83 EC ? 48 8B 01 48 8B D9 FF 90 ? ? ? ? 85 C0 75";
constexpr char PlayerHealthGlowRenderPattern[] =
    "48 8B C4 4C 89 48 20 48 89 48 08 55 48 8D A8 ? ? ? ? "
    "48 81 EC 20 06 00 00";
constexpr size_t MeshEntryStride = 0x68;
constexpr size_t MeshSceneObject = 0x18;
constexpr size_t MeshMaterial = 0x20;
constexpr size_t MeshColor = 0x50;
constexpr size_t SceneObjectOwner = 0xC0;
constexpr size_t MeshMaterialDescriptor = 0x08;
constexpr size_t MaterialDescriptorSize = 0x108;
constexpr size_t MaterialTintOffset = 0x04;
constexpr size_t MaterialAlphaOffset = 0x10;
struct alignas(8) MeshEntryCopy {
    std::array<uint8_t, MeshEntryStride> bytes;
};
struct alignas(8) MaterialDescriptorCopy {
    std::array<uint8_t, MaterialDescriptorSize> bytes;
};

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

bool CreateFlatChamsMaterial() {
    if (flatChamsMaterial && invisibleChamsMaterial) return true;

    HMODULE materialModule = GetModuleHandleA("materialsystem2.dll");
    HMODULE tier0Module = GetModuleHandleA("tier0.dll");
    if (!materialModule || !tier0Module) {
        LogGlowHook("chams material modules unavailable");
        return false;
    }

    const auto createInterface = reinterpret_cast<CreateInterfaceFn>(
        GetProcAddress(materialModule, "CreateInterface"));
    const auto loadKV3 = reinterpret_cast<LoadKV3Fn>(GetProcAddress(
        tier0Module,
        "?LoadKV3@@YA_NPEAVKeyValues3@@PEAVCUtlString@@PEBDAEBUKV3ID_t@@2I@Z"));
    void* materialSystem = createInterface
        ? createInterface("VMaterialSystem2_001", nullptr) : nullptr;
    if (!materialSystem || !loadKV3) {
        LogGlowHook("material interface or LoadKV3 unavailable");
        return false;
    }

    // Current generic KV3 format id used by Source 2 material loaders.
    constexpr KV3ID genericFormat{
        "generic", 0x41B818518343427Eull, 0xB5F447C23C0CDF8Cull};
    constexpr char materialText[] = R"kv3(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
shader = "pbr.vfx"
F_UNLIT = 1
F_RENDER_BACKFACES = 1
g_nTextureColorTintMode1 = 1
g_bMaskColorTint1 = 1
g_vColorTint1 = [1.0, 1.0, 1.0]
g_tColor = resource:"materials/default/default_color_tga_22e6f7.vtex"
g_tNormalRoughness = resource:"materials/default/default_normal_tga_7be61377.vtex"
g_tTintMask = resource:"materials/default/default_mask_tga_344101f8.vtex"
g_tSelfIllumMask = resource:"materials/default/default_mask_tga_344101f8.vtex"
g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})kv3";
    constexpr char invisibleMaterialText[] = R"kv3(<!-- kv3 encoding:text:version{e21c7f3c-8a33-41c5-9977-a76d3a32aa0d} format:generic:version{7412167c-06e9-4698-aff2-e63eb59037e7} -->
{
shader = "pbr.vfx"
F_UNLIT = 1
F_RENDER_BACKFACES = 0
F_DISABLE_Z_BUFFERING = 1
g_nTextureColorTintMode1 = 1
g_bMaskColorTint1 = 1
g_vColorTint1 = [1.0, 1.0, 1.0]
g_tColor = resource:"materials/default/default_color_tga_22e6f7.vtex"
g_tNormalRoughness = resource:"materials/default/default_normal_tga_7be61377.vtex"
g_tTintMask = resource:"materials/default/default_mask_tga_344101f8.vtex"
g_tSelfIllumMask = resource:"materials/default/default_mask_tga_344101f8.vtex"
g_tAmbientOcclusion = resource:"materials/default/default_ao_tga_559f1ac6.vtex"
})kv3";

    // Leave headroom around the opaque node exactly as the engine-side KV3
    // loaders do; LoadKV3 initializes the 0x20-byte node itself.
    alignas(16) std::array<uint8_t, 0x1000> kvStorage{};
    auto* kv3 = reinterpret_cast<KeyValues3Storage*>(kvStorage.data() + 0x100);
    if (!loadKV3(kv3, nullptr, materialText, genericFormat,
                 "axiom_flat_chams.vmat", 0)) {
        LogGlowHook("flat chams KV3 parsing failed");
        return false;
    }

    void** vtable = *reinterpret_cast<void***>(materialSystem);
    if (!vtable || !vtable[29]) {
        LogGlowHook("CreateMaterial vtable entry unavailable");
        return false;
    }
    const auto createMaterial = reinterpret_cast<CreateMaterialFn>(vtable[29]);
    void** handle = nullptr;
    createMaterial(nullptr, &handle, "materials/axiom_flat_chams_2.vmat",
                   kv3, 0, 1);
    flatChamsMaterial = handle ? *handle : nullptr;
    alignas(16) std::array<uint8_t, 0x1000> invisibleKvStorage{};
    auto* invisibleKv3 = reinterpret_cast<KeyValues3Storage*>(
        invisibleKvStorage.data() + 0x100);
    if (loadKV3(invisibleKv3, nullptr, invisibleMaterialText, genericFormat,
                "axiom_invisible_chams.vmat", 0)) {
        void** invisibleHandle = nullptr;
        createMaterial(
            nullptr, &invisibleHandle,
            "materials/axiom_invisible_chams_1.vmat",
            invisibleKv3, 0, 1);
        invisibleChamsMaterial = invisibleHandle ? *invisibleHandle : nullptr;
    }
    LogGlowHook(flatChamsMaterial && invisibleChamsMaterial
        ? "visible and invisible opaque pbr chams materials created"
        : "one or more pbr chams materials failed");
    return flatChamsMaterial != nullptr && invisibleChamsMaterial != nullptr;
}

bool InstallDrawHooksOnContext(ID3D11DeviceContext* context);
bool CreateGlowResources();

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

uintptr_t FindGeneratePrimitivesTarget(HMODULE sceneModule) {
    const uintptr_t constructor = FindPattern(
        sceneModule, GeneratePrimitivesVtablePattern);
    if (!constructor) return 0;

    const int32_t displacement = Read<int32_t>(constructor + 3);
    const uintptr_t vtable = constructor + 7 + displacement;
    const uintptr_t candidate = Read<uintptr_t>(vtable + 0x20);
    if (!candidate) return 0;

    MODULEINFO info{};
    if (!GetModuleInformation(
            GetCurrentProcess(), sceneModule, &info, sizeof(info)))
        return 0;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    const uintptr_t end = begin + info.SizeOfImage;
    return candidate >= begin && candidate < end ? candidate : 0;
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
    const bool teamEspEnabled = ally ? allyEspEnabled : enemyEspEnabled;
    const float maxDistance = ally ? allyEspMaxDistance : enemyEspMaxDistance;
    if (currentLocalPositionReady) {
        Vector3 position{};
        if (GetEntityPosition(pawn, position)) {
            const float dx = position.x - currentLocalPosition.x;
            const float dy = position.y - currentLocalPosition.y;
            const float dz = position.z - currentLocalPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
            if (!std::isfinite(distance) || distance > maxDistance) return false;
        }
    }
    return teamEspEnabled && (ally ? allyGlowEnabled : enemyGlowEnabled);
}

bool IsChamsEnabledForPawn(uintptr_t pawn) {
    if (!pawn || pawn == currentLocalPawn || !currentLocalPawn) return false;
    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const uint8_t pawnTeam = Read<uint8_t>(pawn + Offsets::Team);
    if ((localTeam != 2 && localTeam != 3) ||
        (pawnTeam != 2 && pawnTeam != 3)) return false;
    const bool ally = pawnTeam == localTeam;
    const bool enabled = ally
        ? (allyChamsEnabled || allyInvisibleChamsEnabled)
        : (enemyChamsEnabled || enemyInvisibleChamsEnabled);
    if (!enabled) return false;
    if (currentLocalPositionReady) {
        Vector3 position{};
        if (GetEntityPosition(pawn, position)) {
            const float dx = position.x - currentLocalPosition.x;
            const float dy = position.y - currentLocalPosition.y;
            const float dz = position.z - currentLocalPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
            const float maxDistance = ally ? allyEspMaxDistance : enemyEspMaxDistance;
            if (!std::isfinite(distance) || distance > maxDistance) return false;
        }
    }
    return Read<int>(pawn + Offsets::Health) > 0 &&
           Read<uint8_t>(pawn + Offsets::LifeState) == 0;
}

// PlayerOutlineRenderer never consumes CGlowProperty for NPC_Trooper.  Apply
// their chams in the model submission path instead, where every animated
// trooper mesh is available together with its owning entity.
const float* GetTrooperChamsTint(uintptr_t entity) {
    if (!entity || !currentLocalPawn) return nullptr;
    const std::string className = GetEntityClassName(entity);
    if (className.find("NPC_Trooper") == std::string::npos ||
        className.find("TrooperBoss") != std::string::npos ||
        Read<int>(entity + Offsets::Health) <= 0 ||
        Read<uint8_t>(entity + Offsets::LifeState) != 0) {
        return nullptr;
    }

    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
    const bool neutral = team == 4;
    const bool ally = !neutral && localTeam >= 2 && localTeam <= 3 && team == localTeam;
    const bool enabled = neutral ? neutralChams
        : (ally ? allyTrooperChams : enemyTrooperChams);
    if (!enabled) return nullptr;

    if (currentLocalPositionReady) {
        Vector3 position{};
        if (!GetEntityPosition(entity, position)) return nullptr;
        const float dx = position.x - currentLocalPosition.x;
        const float dy = position.y - currentLocalPosition.y;
        const float dz = position.z - currentLocalPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
        if (!std::isfinite(distance) || distance > creepEspMaxDistance)
            return nullptr;
    }

    return neutral ? neutralChamsColor
                   : (ally ? allyTrooperChamsColor : enemyTrooperChamsColor);
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

bool IsNormalFillPawn(uintptr_t pawn) {
    if (!pawn || pawn == currentLocalPawn || !currentLocalPawn) return false;
    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const uint8_t pawnTeam = Read<uint8_t>(pawn + Offsets::Team);
    if ((localTeam != 2 && localTeam != 3) ||
        (pawnTeam != 2 && pawnTeam != 3)) return false;
    const bool ally = pawnTeam == localTeam;
    const bool teamEspEnabled = ally ? allyEspEnabled : enemyEspEnabled;
    return teamEspEnabled && (ally ? allyGlowEnabled : enemyGlowEnabled) &&
           (ally ? allyGlowMode : enemyGlowMode) == 1;
}

static uint32_t GlowPackedColor(const float color[4]) {
    const uint32_t r = static_cast<uint32_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f);
    const uint32_t g = static_cast<uint32_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f);
    const uint32_t b = static_cast<uint32_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f);
    const uint32_t a = static_cast<uint32_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f);
    return (a << 24) | (b << 16) | (g << 8) | r;
}

void __fastcall HookGeneratePrimitives(
    uintptr_t thisptr, uintptr_t sceneObject, uintptr_t sceneView,
    uintptr_t primitiveBuffer) {
    if (!originalGeneratePrimitives) return;

    const bool chamsActive = enemyChamsEnabled || allyChamsEnabled;
    if (!chamsActive || !flatChamsMaterial || !sceneObject ||
        !primitiveBuffer || !currentLocalPawn) {
        originalGeneratePrimitives(
            thisptr, sceneObject, sceneView, primitiveBuffer);
        return;
    }

    struct CachedOwner {
        uint32_t handle{0xFFFFFFFFu};
        uintptr_t pawn{};
        bool hero{};
    };
    thread_local std::array<CachedOwner, 128> ownerCache{};

    const uint32_t ownerHandle = Read<uint32_t>(
        sceneObject + SceneObjectOwner);
    auto& cached = ownerCache[ownerHandle % ownerCache.size()];
    if (cached.handle != ownerHandle) {
        cached.handle = ownerHandle;
        cached.pawn = ResolveEntity(ownerHandle);
        cached.hero = false;
    }
    if (cached.pawn && cached.pawn != currentLocalPawn && !cached.hero) {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        cached.hero = std::find(
            heroPawns.begin(), heroPawns.end(), cached.pawn) !=
            heroPawns.end();
    }
    const uintptr_t pawn = cached.pawn;
    if (!pawn || pawn == currentLocalPawn) {
        originalGeneratePrimitives(
            thisptr, sceneObject, sceneView, primitiveBuffer);
        return;
    }

    // Reject the overwhelming majority of scene objects before taking the
    // hero-list lock or performing any distance/health work.
    const uint8_t pawnTeam = Read<uint8_t>(pawn + Offsets::Team);
    if (pawnTeam != 2 && pawnTeam != 3) {
        originalGeneratePrimitives(
            thisptr, sceneObject, sceneView, primitiveBuffer);
        return;
    }

    if (!cached.hero || !IsChamsEnabledForPawn(pawn)) {
        originalGeneratePrimitives(
            thisptr, sceneObject, sceneView, primitiveBuffer);
        return;
    }

    const int previousCount = Read<int>(primitiveBuffer + 0xC);
    originalGeneratePrimitives(thisptr, sceneObject, sceneView, primitiveBuffer);

    const int primitiveCount = Read<int>(primitiveBuffer + 0xC);
    const uintptr_t primitives = Read<uintptr_t>(primitiveBuffer);
    if (!primitives || previousCount < 0 || primitiveCount < previousCount ||
        primitiveCount - previousCount > 256)
        return;

    const uint8_t localTeam =
        Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const bool ally = localTeam >= 2 && localTeam <= 3 &&
                      pawnTeam == localTeam;
    const float* sourceColor = ally ? allyChamsColor : enemyChamsColor;
    const float colorComponents[4] = {
        sourceColor[0], sourceColor[1], sourceColor[2], 1.0f};
    const uint32_t color = GlowPackedColor(colorComponents);

    for (int i = previousCount; i < primitiveCount; ++i) {
        const uintptr_t primitive =
            primitives + static_cast<uintptr_t>(i) * 0x68;
        Write<uintptr_t>(primitive + 0x20,
                         reinterpret_cast<uintptr_t>(flatChamsMaterial));
        Write<uint32_t>(primitive + 0x50, color);
    }

    if (primitiveCount > previousCount &&
        !firstEnemyPassLogged.exchange(true))
        LogGlowHook("first GeneratePrimitives chams override applied");
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
        const bool teamEspEnabled = ally ? allyEspEnabled : enemyEspEnabled;
        const bool teamGlowEnabled = ally ? allyGlowEnabled : enemyGlowEnabled;
        const bool teamInvisibleChamsEnabled = ally
            ? allyInvisibleChamsEnabled : enemyInvisibleChamsEnabled;
        const float maxDistance = ally ? allyEspMaxDistance : enemyEspMaxDistance;
        bool withinDistance = true;
        if (currentLocalPositionReady) {
            Vector3 position{};
            if (GetEntityPosition(static_cast<uintptr_t>(pawn), position)) {
                const float dx = position.x - currentLocalPosition.x;
                const float dy = position.y - currentLocalPosition.y;
                const float dz = position.z - currentLocalPosition.z;
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
                withinDistance = std::isfinite(distance) && distance <= maxDistance;
            }
        }
        if (!validTeam) return originalResult;
        if (!teamEspEnabled ||
            (!teamGlowEnabled && !teamInvisibleChamsEnabled) ||
            !withinDistance)
            return 0;
        const float* glowColor = teamInvisibleChamsEnabled
            ? (ally ? allyInvisibleChamsColor : enemyInvisibleChamsColor)
            : (ally ? teammateGlowColor : enemyGlowColor);

        float adjusted[4] = {
            glowColor[0], glowColor[1], glowColor[2], glowColor[3]};
        if (teamInvisibleChamsEnabled) {
            constexpr float saturationBoost = 1.25f;
            const float average =
                (glowColor[0] + glowColor[1] + glowColor[2]) / 3.0f;
            adjusted[0] = std::clamp(
                average + (glowColor[0] - average) * saturationBoost,
                0.0f, 1.0f);
            adjusted[1] = std::clamp(
                average + (glowColor[1] - average) * saturationBoost,
                0.0f, 1.0f);
            adjusted[2] = std::clamp(
                average + (glowColor[2] - average) * saturationBoost,
                0.0f, 1.0f);
            adjusted[3] = 1.0f;
        }
        if (color) *color = GlowPackedColor(adjusted);
        // Repeating the native fill to reach opaque coverage must not also
        // accumulate its temporally-jittered edge blur. Invisible Chams use
        // the solid interior mask only; ordinary Glow keeps its outline.
        if (width) *width = teamInvisibleChamsEnabled ? 0.0f : 4.0f;

        // Preserve the existing HP-based mode. Normal fill now follows the
        // get_outline_mode hook contract supplied for the full outline: mode 2.
        return 2;
    }

    return originalResult;
}

// The mode hook selects the outline pipeline, while this native helper
// supplies its vertical health fraction.  Only Normal fill replaces that
// fraction; HP-based continues through the original function unchanged.
float __fastcall HookOutlineHealthFraction(__int64 pawn) {
    const float fraction = originalOutlineHealthFraction
        ? originalOutlineHealthFraction(pawn) : 0.0f;
    return IsNormalFillPawn(static_cast<uintptr_t>(pawn)) ? 1.0f : fraction;
}

void __fastcall HookGlowComposite(
    void* context, int a2, int a3, int a4,
    int a5, int a6, int a7, int a8) {
    if (!originalGlowComposite) return;
    const int passCount = nativeGlowCompositeActive ? 10 : 1;
    for (int pass = 0; pass < passCount; ++pass)
        originalGlowComposite(context, a2, a3, a4, a5, a6, a7, a8);
}

void EnsureGlowCompositeHook(void* renderContext) {
    if (!renderContext || glowCompositeTarget ||
        glowCompositeHookInstalling.exchange(true, std::memory_order_acq_rel))
        return;
    void* candidate = nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(renderContext);
        candidate = vtable ? vtable[93] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        candidate = nullptr;
    }
    if (candidate) {
        const MH_STATUS created = MH_CreateHook(
            candidate, reinterpret_cast<void*>(&HookGlowComposite),
            reinterpret_cast<void**>(&originalGlowComposite));
        if (created == MH_OK) {
            const MH_STATUS enabled = MH_EnableHook(candidate);
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED) {
                glowCompositeTarget = candidate;
                LogGlowHook("native glow final composite hook installed");
            } else {
                MH_RemoveHook(candidate);
                originalGlowComposite = nullptr;
                LogGlowHook("native glow final composite hook enable failed");
            }
        } else {
            originalGlowComposite = nullptr;
            LogGlowHook("native glow final composite hook creation failed");
        }
    }
    glowCompositeHookInstalling.store(false, std::memory_order_release);
}

// Build the native mask once; opacity accumulation happens only in its final
// render-context composite so temporal model sampling is not repeated.
void __fastcall HookPlayerHealthGlowRender(
    void* renderer, void* arg1, void* arg2, void* arg3) {
    if (originalPlayerHealthGlowRender) {
        const bool useOpaqueGlowPass = enemyInvisibleChamsEnabled ||
                                       allyInvisibleChamsEnabled;
        EnsureGlowCompositeHook(arg2);
        if (useOpaqueGlowPass)
            nativeGlowRenderPassCount.fetch_add(1, std::memory_order_acq_rel);
        nativeGlowCompositeActive = useOpaqueGlowPass &&
                                    glowCompositeTarget != nullptr;
        originalPlayerHealthGlowRender(renderer, arg1, arg2, arg3);
        nativeGlowCompositeActive = false;
        if (useOpaqueGlowPass)
            nativeGlowRenderPassCount.fetch_sub(1, std::memory_order_acq_rel);
    }
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
    ID3D11DeviceContext* context, SavedPipelineState& saved,
    int explicitMode = -100) {
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
        const int mode = explicitMode == -100
            ? renderGlowTeam.load(std::memory_order_acquire) : explicitMode;
        float modulationColor[4] = {};
        const float* glowColor = enemyChamsColor;
        if (mode == 1) {
            glowColor = allyChamsColor;
        } else if (mode == 2) {
            // World/sky geometry is submitted without an entity owner.
            modulationColor[0] = disableSkybox ? 0.0f : skyboxColor[0] * skyboxBrightness * lightColor[0] * lightBrightness;
            modulationColor[1] = disableSkybox ? 0.0f : skyboxColor[1] * skyboxBrightness * lightColor[1] * lightBrightness;
            modulationColor[2] = disableSkybox ? 0.0f : skyboxColor[2] * skyboxBrightness * lightColor[2] * lightBrightness;
            modulationColor[3] = 1.0f;
            glowColor = modulationColor;
        } else if (mode == 3) {
            modulationColor[0] = propsColor[0] * lightColor[0] * lightBrightness;
            modulationColor[1] = propsColor[1] * lightColor[1] * lightBrightness;
            modulationColor[2] = propsColor[2] * lightColor[2] * lightBrightness;
            modulationColor[3] = 1.0f;
            glowColor = modulationColor;
        }
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

// Current rendersystemdx11 slot 43 ABI (verified from the installed binary):
//   void SetDepthStencilState(RenderContext*, StateWrapper*, uint32 stencilRef)
// StateWrapper::m_pDx11State is at +0x10.  Replacing only that pointer keeps
// the engine's normal state cache and command recording intact.
void __fastcall HookRenderDepthState(
    uintptr_t context, void* stateWrapper, uint32_t stencilRef) {
    if (!originalRenderDepthState) return;
    if (context != renderInvisibleChamsContext.load(std::memory_order_acquire) ||
        !invisibleChamsDepthState || !stateWrapper) {
        originalRenderDepthState(context, stateWrapper, stencilRef);
        return;
    }

    struct alignas(8) DepthStateWrapperView {
        std::array<uint8_t, 0x10> prefix{};
        ID3D11DepthStencilState* state{};
    } replacement{};
    __try {
        std::memcpy(replacement.prefix.data(), stateWrapper, 0x10);
        bool expected = false;
        if (invisiblePassDepthCaptured.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            invisiblePassOriginalDepth.store(
                reinterpret_cast<uintptr_t>(
                    *reinterpret_cast<ID3D11DepthStencilState**>(
                        reinterpret_cast<uintptr_t>(stateWrapper) + 0x10)),
                std::memory_order_release);
            invisiblePassOriginalStencilRef.store(
                stencilRef, std::memory_order_release);
        }
        replacement.state = invisibleChamsDepthState;
        originalRenderDepthState(context, &replacement, stencilRef);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        originalRenderDepthState(context, stateWrapper, stencilRef);
    }
}

void RestoreInvisiblePassDepthState() {
    const uintptr_t context =
        renderInvisibleChamsContext.load(std::memory_order_acquire);
    if (!originalRenderDepthState ||
        !invisiblePassDepthCaptured.load(std::memory_order_acquire) || !context)
        return;
    struct alignas(8) DepthStateWrapperView {
        std::array<uint8_t, 0x10> prefix{};
        ID3D11DepthStencilState* state{};
    } restore{};
    restore.state = reinterpret_cast<ID3D11DepthStencilState*>(
        invisiblePassOriginalDepth.load(std::memory_order_acquire));
    originalRenderDepthState(
        context, &restore,
        invisiblePassOriginalStencilRef.load(std::memory_order_acquire));
    invisiblePassDepthCaptured.store(false, std::memory_order_release);
    invisiblePassOriginalDepth.store(0, std::memory_order_release);
    invisiblePassOriginalStencilRef.store(0, std::memory_order_release);
}

void EnsureRenderDepthHook(uintptr_t renderContext) {
    if (!renderContext || renderDepthStateTarget ||
        renderDepthHookInstalling.exchange(true, std::memory_order_acq_rel))
        return;

    void* candidate = nullptr;
    __try {
        void** vtable = *reinterpret_cast<void***>(renderContext);
        candidate = vtable ? vtable[43] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        candidate = nullptr;
    }

    if (candidate) {
        const MH_STATUS created = MH_CreateHook(
            candidate, reinterpret_cast<void*>(&HookRenderDepthState),
            reinterpret_cast<void**>(&originalRenderDepthState));
        if (created == MH_OK) {
            const MH_STATUS enabled = MH_EnableHook(candidate);
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED) {
                renderDepthStateTarget = candidate;
                LogGlowHook("verified RenderSystem depth-state hook installed");
            } else {
                MH_RemoveHook(candidate);
                originalRenderDepthState = nullptr;
                LogGlowHook("RenderSystem depth-state hook enable failed");
            }
        } else {
            originalRenderDepthState = nullptr;
            LogGlowHook("RenderSystem depth-state hook creation failed");
        }
    }
    renderDepthHookInstalling.store(false, std::memory_order_release);
}

void ProbeRenderDepthMethod(uintptr_t renderContext) {
    if (!renderContext || renderDepthProbeLogged.exchange(
            true, std::memory_order_acq_rel))
        return;
    char message[1024]{};
    __try {
        void** vtable = *reinterpret_cast<void***>(renderContext);
        void* candidate = vtable ? vtable[43] : nullptr;
        HMODULE module = GetModuleHandleA("rendersystemdx11.dll");
        const uintptr_t base = reinterpret_cast<uintptr_t>(module);
        const uintptr_t address = reinterpret_cast<uintptr_t>(candidate);
        int used = sprintf_s(
            message, "render depth probe context=%p vtable=%p slot43=%p rva=0x%llx bytes=",
            reinterpret_cast<void*>(renderContext), vtable, candidate,
            static_cast<unsigned long long>(base && address >= base
                ? address - base : 0));
        if (used > 0 && candidate) {
            const auto* bytes = reinterpret_cast<const uint8_t*>(candidate);
            for (int i = 0; i < 32 && used < 980; ++i) {
                used += sprintf_s(
                    message + used, sizeof(message) - used,
                    "%02X", bytes[i]);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strcpy_s(message, "render depth probe failed while reading context");
    }
    LogGlowHook(message);
}

void** __fastcall HookDrawModel(
    __int64 sceneObjectDesc, __int64 dx11, __int64* meshDraws,
    int meshCount, __int64 sceneView, __int64 sceneLayer, __int64 a7) {
    ProbeRenderDepthMethod(static_cast<uintptr_t>(dx11));
    std::vector<MeshEntryCopy> worldMeshes;
    std::vector<MaterialDescriptorCopy> worldDescriptors;
    __int64* submittedMeshes = meshDraws;
    const int safeWorldCount = meshCount > 0 && meshCount < 512 ? meshCount : 0;
    const bool trooperChamsActive = enemyTrooperChams || allyTrooperChams || neutralChams;
    if ((worldModulationEnabled || trooperChamsActive) &&
        meshDraws && safeWorldCount > 0) {
        worldMeshes.resize(static_cast<size_t>(safeWorldCount));
        worldDescriptors.resize(static_cast<size_t>(safeWorldCount));
        const uintptr_t source = reinterpret_cast<uintptr_t>(meshDraws);
        bool complete = true;
        for (int i = 0; i < safeWorldCount; ++i) {
            const uintptr_t entry = source + static_cast<uintptr_t>(i) * MeshEntryStride;
            auto& mesh = worldMeshes[static_cast<size_t>(i)];
            if (!CopyReadableBytes(mesh.bytes.data(), reinterpret_cast<const void*>(entry),
                                   MeshEntryStride)) {
                complete = false;
                break;
            }
            const uintptr_t descriptor = Read<uintptr_t>(entry + MeshMaterialDescriptor);
            auto& descriptorCopy = worldDescriptors[static_cast<size_t>(i)];
            if (!descriptor || !CopyReadableBytes(descriptorCopy.bytes.data(),
                                                   reinterpret_cast<const void*>(descriptor),
                                                   MaterialDescriptorSize))
                continue;

            const uintptr_t sceneObject = Read<uintptr_t>(entry + MeshSceneObject);
            const uint32_t ownerHandle = sceneObject
                ? Read<uint32_t>(sceneObject + SceneObjectOwner) : 0xFFFFFFFFu;
            const uintptr_t owner = ownerHandle != 0xFFFFFFFFu
                ? ResolveEntity(ownerHandle) : 0;
            const bool hero = owner == currentLocalPawn || GetGlowHeroMeshPawn(entry) != 0;
            const float* trooperTint = GetTrooperChamsTint(owner);
            if (trooperTint) {
                const float tint[4] = {
                    std::clamp(trooperTint[0], 0.0f, 1.0f),
                    std::clamp(trooperTint[1], 0.0f, 1.0f),
                    std::clamp(trooperTint[2], 0.0f, 1.0f),
                    1.0f
                };
                SetGlowDescriptorTint(descriptorCopy.bytes, tint);
            } else if (worldModulationEnabled && !hero) {
                const float* base = owner ? propsColor : worldColor;
                const float skyFactor = owner ? 1.0f : skyboxBrightness;
                const float tint[4] = {
                    std::clamp(base[0] * lightColor[0] * lightBrightness *
                               (owner ? 1.0f : skyboxColor[0]) * skyFactor, 0.0f, 4.0f),
                    std::clamp(base[1] * lightColor[1] * lightBrightness *
                               (owner ? 1.0f : skyboxColor[1]) * skyFactor, 0.0f, 4.0f),
                    std::clamp(base[2] * lightColor[2] * lightBrightness *
                               (owner ? 1.0f : skyboxColor[2]) * skyFactor, 0.0f, 4.0f),
                    disableSkybox && !owner ? 0.0f : 1.0f
                };
                SetGlowDescriptorTint(descriptorCopy.bytes, tint);
            }
            const uintptr_t descriptorAddress = reinterpret_cast<uintptr_t>(
                descriptorCopy.bytes.data());
            std::memcpy(mesh.bytes.data() + MeshMaterialDescriptor,
                        &descriptorAddress, sizeof(descriptorAddress));
        }
        if (complete)
            submittedMeshes = reinterpret_cast<__int64*>(worldMeshes.data());
    }

    bool hasOwnedMesh = false;
    bool hasHeroMesh = false;
    if (worldModulationEnabled && meshDraws && safeWorldCount > 0) {
        const uintptr_t source = reinterpret_cast<uintptr_t>(meshDraws);
        for (int i = 0; i < safeWorldCount; ++i) {
            const uintptr_t entry = source + static_cast<uintptr_t>(i) * MeshEntryStride;
            const uintptr_t sceneObject = Read<uintptr_t>(entry + MeshSceneObject);
            const uint32_t handle = sceneObject
                ? Read<uint32_t>(sceneObject + SceneObjectOwner) : 0xFFFFFFFFu;
            const uintptr_t owner = handle != 0xFFFFFFFFu ? ResolveEntity(handle) : 0;
            if (!owner) continue;
            hasOwnedMesh = true;
            if (GetEntityClassName(owner).find("CitadelPlayerPawn") != std::string::npos) {
                hasHeroMesh = true;
                break;
            }
        }
    }
    // Do not recolour player model batches.  All non-player scene batches use
    // the active D3D context, which affects the final shader rather than an
    // unused material-descriptor copy.
    auto* modulationContext = worldModulationEnabled && !hasHeroMesh &&
            LooksLikeD3D11Context(reinterpret_cast<void*>(dx11))
        ? reinterpret_cast<ID3D11DeviceContext*>(dx11) : nullptr;
    SavedPipelineState modulationSaved{};
    const bool modulationOverridden = modulationContext &&
        BeginGlowPipeline(modulationContext, modulationSaved,
                          hasOwnedMesh ? 3 : 2);

    struct SavedChamsMesh {
        uintptr_t entry{};
        uintptr_t material{};
        uint32_t color{};
        uint32_t visibleColor{};
        uint32_t invisibleColor{};
        bool visible{};
        bool invisible{};
    };
    struct CachedDrawOwner {
        uint32_t handle{0xFFFFFFFFu};
        uintptr_t pawn{};
        bool hero{};
        ULONGLONG nextHeroCheck{};
    };
    thread_local std::array<CachedDrawOwner, 128> ownerCache{};
    thread_local std::array<SavedChamsMesh, 256> savedChams{};
    size_t savedChamsCount = 0;

    const bool anyVisibleChams = enemyChamsEnabled || allyChamsEnabled;
    const bool anyInvisibleChams = enemyInvisibleChamsEnabled ||
                                   allyInvisibleChamsEnabled;
    if ((anyVisibleChams || anyInvisibleChams) && flatChamsMaterial &&
        invisibleChamsMaterial &&
        submittedMeshes && meshCount > 0 && meshCount <= 256) {
        const uintptr_t source = reinterpret_cast<uintptr_t>(submittedMeshes);
        uint32_t evaluatedHandle = 0xFFFFFFFFu;
        bool evaluatedVisible = false;
        bool evaluatedInvisible = false;
        uint32_t evaluatedVisibleColor = 0xFFFFFFFFu;
        uint32_t evaluatedInvisibleColor = 0xFFFFFFFFu;

        for (int i = 0; i < meshCount; ++i) {
            const uintptr_t entry =
                source + static_cast<uintptr_t>(i) * MeshEntryStride;
            const uintptr_t sceneObject =
                Read<uintptr_t>(entry + MeshSceneObject);
            if (!sceneObject) continue;
            const uint32_t ownerHandle =
                Read<uint32_t>(sceneObject + SceneObjectOwner);

            if (ownerHandle != evaluatedHandle) {
                evaluatedHandle = ownerHandle;
                evaluatedVisible = false;
                evaluatedInvisible = false;

                auto& cached = ownerCache[ownerHandle % ownerCache.size()];
                if (cached.handle != ownerHandle) {
                    cached.handle = ownerHandle;
                    cached.pawn = ResolveEntity(ownerHandle);
                    cached.hero = false;
                    cached.nextHeroCheck = 0;
                }

                const ULONGLONG now = GetTickCount64();
                if (cached.pawn && !cached.hero && now >= cached.nextHeroCheck) {
                    cached.nextHeroCheck = now + 1000;
                    std::unique_lock<std::mutex> lock(
                        heroPawnsMutex, std::try_to_lock);
                    if (lock.owns_lock()) {
                        cached.hero = std::find(
                            heroPawns.begin(), heroPawns.end(), cached.pawn) !=
                            heroPawns.end();
                    }
                }

                if (cached.hero && IsChamsEnabledForPawn(cached.pawn)) {
                    const uint8_t localTeam = Read<uint8_t>(
                        currentLocalPawn + Offsets::Team);
                    const uint8_t pawnTeam = Read<uint8_t>(
                        cached.pawn + Offsets::Team);
                    const bool ally = pawnTeam == localTeam;
                    evaluatedVisible = ally
                        ? allyChamsEnabled : enemyChamsEnabled;
                    evaluatedInvisible = ally
                        ? allyInvisibleChamsEnabled
                        : enemyInvisibleChamsEnabled;
                    const float* visibleColor =
                        ally ? allyChamsColor : enemyChamsColor;
                    const float* invisibleColor = ally
                        ? allyInvisibleChamsColor
                        : enemyInvisibleChamsColor;
                    const float visibleComponents[4] = {
                        visibleColor[0], visibleColor[1], visibleColor[2], 1.0f};
                    const float invisibleComponents[4] = {
                        invisibleColor[0], invisibleColor[1],
                        invisibleColor[2], 1.0f};
                    evaluatedVisibleColor = GlowPackedColor(visibleComponents);
                    evaluatedInvisibleColor = GlowPackedColor(invisibleComponents);
                }
            }

            if (!evaluatedVisible && !evaluatedInvisible) continue;
            savedChams[savedChamsCount++] = {
                entry,
                Read<uintptr_t>(entry + MeshMaterial),
                Read<uint32_t>(entry + MeshColor),
                evaluatedVisibleColor,
                evaluatedInvisibleColor,
                evaluatedVisible,
                evaluatedInvisible};
        }
    }

    // Native PlayerHealthGlowRenderer already owns a stable through-wall
    // render target and visibility mask.  During that pass replace only hero
    // meshes with the opaque PBR material; do not touch the scene depth state.
    if (savedChamsCount && anyInvisibleChams &&
        nativeGlowRenderPassCount.load(std::memory_order_acquire) > 0) {
        for (size_t i = 0; i < savedChamsCount; ++i) {
            const auto& saved = savedChams[i];
            if (!saved.invisible) continue;
            Write<uintptr_t>(saved.entry + MeshMaterial,
                             reinterpret_cast<uintptr_t>(
                                 invisibleChamsMaterial));
            Write<uint32_t>(saved.entry + MeshColor, saved.invisibleColor);
        }
        void** result = originalDrawModel(
            sceneObjectDesc, dx11, submittedMeshes, meshCount,
            sceneView, sceneLayer, a7);
        for (size_t i = 0; i < savedChamsCount; ++i) {
            const auto& saved = savedChams[i];
            Write<uintptr_t>(saved.entry + MeshMaterial, saved.material);
            Write<uint32_t>(saved.entry + MeshColor, saved.color);
        }
        if (modulationOverridden)
            EndGlowPipeline(modulationContext, modulationSaved);
        return result;
    }

    for (size_t i = 0; i < savedChamsCount; ++i) {
        const auto& saved = savedChams[i];
        if (saved.visible) {
            Write<uintptr_t>(saved.entry + MeshMaterial,
                             reinterpret_cast<uintptr_t>(flatChamsMaterial));
            Write<uint32_t>(saved.entry + MeshColor, saved.visibleColor);
        } else {
            Write<uintptr_t>(saved.entry + MeshMaterial, saved.material);
            Write<uint32_t>(saved.entry + MeshColor, saved.color);
        }
    }
    void** result = originalDrawModel(
        sceneObjectDesc, dx11, submittedMeshes, meshCount,
        sceneView, sceneLayer, a7);
    for (size_t i = 0; i < savedChamsCount; ++i) {
        const auto& saved = savedChams[i];
        Write<uintptr_t>(saved.entry + MeshMaterial, saved.material);
        Write<uint32_t>(saved.entry + MeshColor, saved.color);
    }
    if (modulationOverridden) {
        EndGlowPipeline(modulationContext, modulationSaved);
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

    // Opaque through-wall pass: accept every fragment without modifying the
    // scene depth buffer.  The normal visible pass is submitted immediately
    // afterwards and retains the engine's original depth/write behaviour.
    D3D11_DEPTH_STENCIL_DESC invisibleDepth{};
    invisibleDepth.DepthEnable = TRUE;
    invisibleDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    invisibleDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    result = pDevice->CreateDepthStencilState(
        &invisibleDepth, &invisibleChamsDepthState);
    if (FAILED(result) || !invisibleChamsDepthState) {
        LogGlowHook("invisible chams depth state creation failed");
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

    // Material creation stays on the initialization/UI path. The render hook
    // below only swaps already-created pointers in copied mesh entries.
    CreateFlatChamsMaterial();

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

    if (!outlineHealthFractionTarget) {
        HMODULE client = GetModuleHandleA("client.dll");
        const uintptr_t candidate = client
            ? FindPattern(client, OutlineHealthFractionPattern) : 0;
        if (candidate && InstallContextHook(
                outlineHealthFractionTarget,
                reinterpret_cast<void*>(candidate),
                reinterpret_cast<void*>(&HookOutlineHealthFraction),
                originalOutlineHealthFraction)) {
            LogGlowHook("client outline health fraction hook installed");
        } else {
            LogGlowHook("client outline health fraction pattern not found or hook failed");
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

    // Chams now operate entirely on the SceneSystem DrawModel mesh array. Do
    // not hook ID3D11DeviceContext::Draw* here: those callbacks run for every
    // draw in the frame and the former atomic bookkeeping alone caused a
    // severe CPU/render-thread bottleneck.  World modulation only needs the
    // pipeline resources and applies them directly from HookDrawModel.
    if (pContext && CreateGlowResources())
        LogGlowHook("D3D glow resources created without Draw hooks");
    else
        LogGlowHook("D3D glow resources unavailable");

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
    renderInvisibleChamsContext.store(0, std::memory_order_release);
    RemoveHookTarget(renderDepthStateTarget);
    RemoveHookTarget(drawInstancedTarget);
    RemoveHookTarget(drawInstancedIndirectTarget);
    RemoveHookTarget(drawIndexedInstancedIndirectTarget);
    RemoveHookTarget(drawIndexedInstancedTarget);
    RemoveHookTarget(drawTarget);
    RemoveHookTarget(drawIndexedTarget);
    RemoveHookTarget(generatePrimitivesTarget);
    RemoveHookTarget(drawModelTarget);
    RemoveHookTarget(createDeferredContextTarget);
    RemoveHookTarget(playerOutlineTarget);
    RemoveHookTarget(outlineHealthFractionTarget);
    RemoveHookTarget(glowCompositeTarget);
    RemoveHookTarget(playerHealthGlowRenderTarget);

    originalDrawInstanced = nullptr;
    originalDrawInstancedIndirect = nullptr;
    originalDrawIndexedInstancedIndirect = nullptr;
    originalDrawIndexedInstanced = nullptr;
    originalDraw = nullptr;
    originalDrawIndexed = nullptr;
    originalGeneratePrimitives = nullptr;
    originalDrawModel = nullptr;
    originalCreateDeferredContext = nullptr;
    originalRenderDepthState = nullptr;
    originalPlayerOutline = nullptr;
    originalOutlineHealthFraction = nullptr;
    originalGlowComposite = nullptr;
    originalPlayerHealthGlowRender = nullptr;

    if (glowRasterizerState) glowRasterizerState->Release();
    if (glowBlendState) glowBlendState->Release();
    if (glowDepthState) glowDepthState->Release();
    if (invisibleChamsDepthState) invisibleChamsDepthState->Release();
    if (glowPixelShader) glowPixelShader->Release();
    if (glowColorBuffer) glowColorBuffer->Release();
    glowRasterizerState = nullptr;
    glowBlendState = nullptr;
    glowDepthState = nullptr;
    invisibleChamsDepthState = nullptr;
    glowPixelShader = nullptr;
    glowColorBuffer = nullptr;
}
