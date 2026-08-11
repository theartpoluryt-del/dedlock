#include "shared.h"
#include "panorama_preview.h"
#include "preview_3d.h"

#include <MinHook.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

using RunScriptFn = void(__fastcall*)(uintptr_t, uintptr_t, const char*,
                                      const char*, int);
using HeroPanelCreateFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t);

RunScriptFn originalRunScript = nullptr;
HeroPanelCreateFn originalHeroPanelCreate = nullptr;
void* runScriptTarget = nullptr;
void* heroPanelCreateTarget = nullptr;
void* skeletonSetModelByNameTarget = nullptr;

uintptr_t panoramaBase = 0;
uintptr_t runScriptGlobal = 0;
uintptr_t cuiEngineGlobal = 0;
uintptr_t hudMinimapGlobal = 0;
uintptr_t portraitEntityListGlobal = 0;
std::atomic<uintptr_t> firstCreatedPanel{0};
std::atomic<uintptr_t> lastCreatedPanel{0};
std::atomic<uintptr_t> previewHeroPanel{0};
std::atomic<uintptr_t> liveCuiEngine{0};
std::atomic<uintptr_t> liveContextPanel{0};
std::atomic<bool> initialized{false};
std::atomic<bool> panelSpawned{false};
std::atomic<bool> panelVisible{false};
std::atomic<bool> rendererReady{false};
std::atomic<bool> reloadExhausted{false};
std::atomic<bool> captureReleasePending{false};
std::atomic<uintptr_t> activeCuiEngine{0};
std::atomic<uintptr_t> activeContextPanel{0};
std::atomic<float> requestedLeft{0.0f};
std::atomic<float> requestedTop{0.0f};
std::atomic<float> requestedRight{0.0f};
std::atomic<float> requestedBottom{0.0f};
std::atomic<bool> requestedVisible{false};
std::atomic<uint64_t> requestedGeneration{0};
std::atomic<uint64_t> appliedGeneration{0};
std::atomic<int> requestedHeroId{1};
std::atomic<int> appliedHeroId{0};
std::atomic<int> enemyHeroId{1};
std::atomic<int> allyHeroId{1};
std::atomic<int> creepPreviewId{55};
std::atomic<int> previewRole{0};
std::atomic<uintptr_t> previewGlowUnit{0};
std::atomic<bool> previewGlowRegistrationQueued{false};
std::atomic<bool> previewNativeGlowActive{false};
ULONGLONG lastInitializeAttemptAt = 0;
ULONGLONG lastSpawnAttemptAt = 0;
ULONGLONG spawnIssuedAt = 0;
ULONGLONG lastHeroReloadAt = 0;
ULONGLONG lastUiDispatchAt = 0;
ULONGLONG lastCreepModelAttemptAt = 0;
std::atomic<int> heroReloadAttempts{0};
bool spawnIssued = false;
uintptr_t creepPreviewUnit = 0;
uintptr_t creepPreviewModel = 0;
bool creepPreviewModelReady = false;

ComPtr<ID3D11Texture2D> captureTexture;
ComPtr<ID3D11Texture2D> resolveTexture;
UINT captureWidth = 0;
UINT captureHeight = 0;
DXGI_FORMAT captureFormat = DXGI_FORMAT_UNKNOWN;
UINT resolveWidth = 0;
UINT resolveHeight = 0;
DXGI_FORMAT resolveFormat = DXGI_FORMAT_UNKNOWN;
float previousLeft = -1.0f, previousTop = -1.0f;
float previousRight = -1.0f, previousBottom = -1.0f;
std::atomic<int> settleFrames{0};
uint64_t capturedGeneration = 0;
std::atomic<uint64_t> captureSerial{0};
std::atomic<uint64_t> displayedSerial{0};
std::atomic<uint64_t> failedBindingSerial{0};
uintptr_t configuredPortraitCamera = 0;
float configuredPortraitFov = 0.0f;
std::mutex logMutex;
std::string logPath =
    "C:\\Users\\artpo\\source\\repos\\Dll6\\x64\\Release\\panorama_preview.log";

void ProcessPendingUiWork(uintptr_t engine, uintptr_t contextPanel);
void Log(const char* message);

void UpdatePortraitNativeGlow(uintptr_t unit) {
    if (!unit) return;
    const int role = previewRole.load(std::memory_order_acquire);
    const bool enabled = glowEnabled &&
        ((role == 0 && enemyEspEnabled && enemyGlowEnabled) ||
         (role == 1 && allyEspEnabled && allyGlowEnabled));
    const uintptr_t glow = unit + Offsets::Glow;
    if (!enabled || role == 2) {
        Write<bool>(glow + Offsets::IsGlowing, false);
        Write<bool>(glow + Offsets::GlowEligible, false);
        previewNativeGlowActive.store(false, std::memory_order_release);
        return;
    }

    const float* color = role == 1 ? teammateGlowColor : enemyGlowColor;
    const int mode = role == 1 ? allyGlowMode : enemyGlowMode;
    Write<Vector3>(glow + Offsets::GlowColor,
                   {color[0], color[1], color[2]});
    Write<int>(glow + Offsets::GlowType, mode == 1 ? 2 : 1);
    Write<int>(glow + Offsets::GlowTeam, -1);
    Write<int>(glow + Offsets::GlowRange, 0);
    Write<int>(glow + Offsets::GlowRangeMin, 0);
    Write<ColorRGBA>(glow + Offsets::GlowColorOverride,
        {static_cast<uint8_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f),
         static_cast<uint8_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f),
         static_cast<uint8_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f),
         static_cast<uint8_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f)});
    Write<bool>(glow + Offsets::GlowFlashing, false);
    Write<float>(glow + Offsets::GlowTime, mode == 1 ? 0.0f : 1.0f);
    Write<float>(glow + Offsets::GlowStartTime, 0.0f);
    Write<bool>(glow + Offsets::GlowEligible, true);
    Write<bool>(glow + Offsets::IsGlowing, true);
    Write<float>(unit + Offsets::GlowBackfaceMult, 1.0f);

    if (previewGlowUnit.load(std::memory_order_acquire) != unit) {
        previewGlowUnit.store(unit, std::memory_order_release);
        previewNativeGlowActive.store(false, std::memory_order_release);
        previewGlowRegistrationQueued.store(false, std::memory_order_release);
    }
    if (!previewNativeGlowActive.load(std::memory_order_acquire) &&
        !previewGlowRegistrationQueued.exchange(true,
                                                 std::memory_order_acq_rel) &&
        gameWindow) {
        PostMessageW(gameWindow, PanoramaPreviewGlowMessage,
                     static_cast<WPARAM>(unit), 0);
    }
}

enum class HeroPanelStatus {
    Invalid,
    WaitingForRenderer,
    Ready,
};

bool IsExecutableAddress(uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &information,
                      sizeof(information))) return false;
    if (information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = information.Protect & 0xFF;
    return protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

void SuppressPortraitParticles(uintptr_t panel, uintptr_t entityList,
                               uint32_t callbackChunk) {
    static uintptr_t previousPanel = 0;
    static ULONGLONG lastScanAt = 0;
    static bool suppressionLogged = false;
    const ULONGLONG now = GetTickCount64();
    if (previousPanel != panel) {
        previousPanel = panel;
        lastScanAt = 0;
        suppressionLogged = false;
    }
    if (!entityList || now - lastScanAt < 250) return;
    lastScanAt = now;

    size_t suppressed = 0;
    const uint32_t first = callbackChunk > 3 ? callbackChunk - 3 : 0;
    const uint32_t last = (std::min)(63u, callbackChunk + 3);
    for (uint32_t chunkIndex = first; chunkIndex <= last; ++chunkIndex) {
        const uintptr_t entries = Read<uintptr_t>(
            entityList + static_cast<uintptr_t>(chunkIndex) * 8);
        if (!entries) continue;
        for (uint32_t slot = 0; slot < 512; ++slot) {
            const uintptr_t entity = Read<uintptr_t>(
                entries + static_cast<uintptr_t>(slot) * 0x70);
            if (!entity) continue;
            const std::string className = GetEntityClassName(entity);
            if (className.find("ParticleSystem") == std::string::npos)
                continue;
            const uintptr_t sceneNode = Read<uintptr_t>(
                entity + Offsets::GameSceneNode);
            if (!sceneNode) continue;
            Write<bool>(sceneNode + Offsets::SceneNodeDormant, true);
            ++suppressed;
        }
    }
    if (suppressed && !suppressionLogged) {
        char message[160]{};
        std::snprintf(message, sizeof(message),
                      "[ESP_PREVIEW] PortraitParticles=DISABLED count=%zu",
                      suppressed);
        Log(message);
        suppressionLogged = true;
    }
}

void Log(const char* message) {
    std::lock_guard<std::mutex> lock(logMutex);
    // Normal LoadLibrary injection lets us keep the log beside the module.
    // Manual-map has no loader entry, so GetModuleHandleEx fails and the
    // absolute project output path above remains the reliable destination.
    {
        HMODULE ownModule = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(&Log), &ownModule)) {
            char modulePath[MAX_PATH]{};
            if (GetModuleFileNameA(ownModule, modulePath, MAX_PATH)) {
                logPath = modulePath;
                const size_t separator = logPath.find_last_of("\\/");
                logPath.resize(separator == std::string::npos ? 0 : separator + 1);
                logPath += "panorama_preview.log";
            }
        }
    }
    std::ofstream file(logPath, std::ios::app);
    if (file) file << GetTickCount64() << " " << (message ? message : "") << '\n';
}

uintptr_t FindPattern(HMODULE module, const char* pattern) {
    if (!module || !pattern) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info,
                              sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token)
        bytes.push_back(token == "?" ? -1 : std::strtoul(token.c_str(), nullptr, 16));
    if (bytes.empty()) return 0;
    const auto* image = reinterpret_cast<const uint8_t*>(module);
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && image[i + j] != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (match) return reinterpret_cast<uintptr_t>(module) + i;
    }
    return 0;
}

uintptr_t RipGlobal(uintptr_t instruction, size_t displacementOffset) {
    if (!instruction) return 0;
    const int32_t displacement = Read<int32_t>(instruction + displacementOffset);
    return instruction + displacementOffset + sizeof(int32_t) + displacement;
}

uintptr_t __fastcall HookHeroPanelCreate(uintptr_t a1, uintptr_t a2) {
    const uintptr_t created = originalHeroPanelCreate
        ? originalHeroPanelCreate(a1, a2) : 0;
    if (created) {
        lastCreatedPanel.store(created, std::memory_order_release);
        uintptr_t expected = 0;
        firstCreatedPanel.compare_exchange_strong(
            expected, created, std::memory_order_acq_rel);
    }
    return created;
}

void __fastcall HookRunScript(uintptr_t engine, uintptr_t panel,
                              const char* script, const char* debugName,
                              int unknown) {
    if (engine) liveCuiEngine.store(engine, std::memory_order_release);
    if (panel) liveContextPanel.store(panel, std::memory_order_release);
    if (originalRunScript)
        originalRunScript(engine, panel, script, debugName, unknown);
}

void ClearCreatedPanels() {
    firstCreatedPanel.store(0, std::memory_order_release);
    lastCreatedPanel.store(0, std::memory_order_release);
}

uintptr_t CreatedPanel() {
    const uintptr_t first = firstCreatedPanel.load(std::memory_order_acquire);
    return first ? first : lastCreatedPanel.load(std::memory_order_acquire);
}

bool RunPanoramaScriptOnUiThread(uintptr_t observedEngine,
                                uintptr_t observedPanel,
                                const char* script) {
    if (!originalRunScript || !script) {
        Log("RunScript prerequisites missing");
        return false;
    }
    // Use the known live HUD panel from the guide. A panel observed by the
    // RunScript hook can belong to a transient popup that cannot create this
    // client panel type.
    uintptr_t engine = cuiEngineGlobal ? Read<uintptr_t>(cuiEngineGlobal) : 0;
    const uintptr_t hudMinimap = hudMinimapGlobal
        ? Read<uintptr_t>(hudMinimapGlobal) : 0;
    uintptr_t panel = hudMinimap ? Read<uintptr_t>(hudMinimap + 0x08) : 0;
    if (!engine || !panel) {
        engine = observedEngine;
        panel = observedPanel;
    }
    if (!engine || !panel) {
        Log("CUIEngine or HudMinimap panel is null");
        return false;
    }
    originalRunScript(engine, panel, script, "Dll6_PanoramaPreview", 0);
    return true;
}

bool RunPanoramaScript(uintptr_t engine, uintptr_t contextPanel,
                       const char* script) {
    return RunPanoramaScriptOnUiThread(engine, contextPanel, script);
}

HeroPanelStatus ConfigureHeroPanel(uintptr_t panel) {
    if (!panel) {
        Log("HeroScenePanelCreate hook did not capture a panel");
        return HeroPanelStatus::Invalid;
    }
    // Current client (2026-07-31): CCitadel_UI_HeroScenePanel_New grows the
    // rotatable-scene base to 0xF8.  Its scene rebuild method reads the hero
    // definition and variant from +0xF8/+0xFC.  +0xF0/+0xF4 are base-class
    // fields and changing them leaves the actual hero id at zero.
    constexpr uintptr_t HeroId = 0xF8;
    constexpr uintptr_t HeroVariant = 0xFC;
    constexpr int ReloadSlot = 339;
    const int currentHeroId = requestedHeroId.load(std::memory_order_acquire);
    // Keep the default standing Infernus scene. Camera framing is adjusted
    // independently below; FULL_BODY changes the environment and animation.
    constexpr int CurrentHeroVariant = 0;
    const bool creep = currentHeroId == 55;
    // CitadelHeroScenePanelNew can only create portrait worlds from HeroData.
    // For the creep role create Infernus' known-good portrait world first;
    // once C_PortraitWorldUnit exists, its CSkeletonInstance model is replaced
    // with the already-loaded model of a real lane trooper.
    const int sceneHeroId = creep ? 1 : currentHeroId;
    Write<int>(panel + HeroId, sceneHeroId);
    Write<int>(panel + HeroVariant, CurrentHeroVariant);
    if (creep) {
        creepPreviewUnit = 0;
        creepPreviewModel = 0;
        creepPreviewModelReady = false;
        lastCreepModelAttemptAt = 0;
    }
    const uintptr_t cui = Read<uintptr_t>(panel + 0x08);
    const uintptr_t vtable = Read<uintptr_t>(cui);
    if (!cui || !vtable) {
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "captured panel invalid panel=0x%llx cui=0x%llx vt=0x%llx",
                      static_cast<unsigned long long>(panel),
                      static_cast<unsigned long long>(cui),
                      static_cast<unsigned long long>(vtable));
        Log(message);
        return HeroPanelStatus::Invalid;
    }
    const uintptr_t reload = Read<uintptr_t>(vtable + ReloadSlot * sizeof(uintptr_t));
    if (!IsExecutableAddress(reload)) {
        Log("PREVIEW_FAIL_RELOAD_INVALID");
        return HeroPanelStatus::Invalid;
    }
    reinterpret_cast<void(__fastcall*)(uintptr_t)>(reload)(cui);
    // CGame_UI_ScenePanel stores its C_Citadel_PortraitWorld_New at +0x40.
    const uintptr_t renderer = Read<uintptr_t>(panel + 0x40);
    char readyMessage[192]{};
    std::snprintf(readyMessage, sizeof(readyMessage),
                  "Hero panel configured panel=0x%llx renderer=0x%llx hero=%d variant=%d",
                  static_cast<unsigned long long>(panel),
                  static_cast<unsigned long long>(renderer),
                  Read<int>(panel + HeroId), Read<int>(panel + HeroVariant));
    Log(readyMessage);
    return renderer ? HeroPanelStatus::Ready
                    : HeroPanelStatus::WaitingForRenderer;
}

uintptr_t ResolvePortraitEntityHandle(int handle) {
    if (!portraitEntityListGlobal || handle == -1 || handle == -2) return 0;
    const uintptr_t entityList = Read<uintptr_t>(portraitEntityListGlobal);
    if (!entityList) return 0;
    const uint32_t index = static_cast<uint32_t>(handle) & 0x7FFFu;
    const uintptr_t chunk = Read<uintptr_t>(
        entityList + static_cast<uintptr_t>(index >> 9) * 8);
    const uintptr_t identity = chunk
        ? chunk + static_cast<uintptr_t>(index & 0x1FFu) * 0x70
        : 0;
    if (!identity || Read<int>(identity + 0x10) != handle) return 0;
    return Read<uintptr_t>(identity);
}

uintptr_t FindPortraitWorldUnit(uintptr_t panel, int heroId) {
    if (!panel || !portraitEntityListGlobal) return 0;
    const uintptr_t portraitWorld = Read<uintptr_t>(panel + 0x40);
    const int callbackHandle = portraitWorld
        ? Read<int>(portraitWorld + 0xC8) : -1;
    if (!portraitWorld || callbackHandle == -1 || callbackHandle == -2)
        return 0;
    const uintptr_t entityList = Read<uintptr_t>(portraitEntityListGlobal);
    if (!entityList) return 0;

    const uint32_t callbackChunk =
        (static_cast<uint32_t>(callbackHandle) & 0x7FFFu) >> 9;
    const auto scanChunk = [&](uint32_t chunkIndex) -> uintptr_t {
        const uintptr_t entries = Read<uintptr_t>(
            entityList + static_cast<uintptr_t>(chunkIndex) * 8);
        if (!entries) return 0;
        for (uint32_t slot = 0; slot < 512; ++slot) {
            const uintptr_t candidate = Read<uintptr_t>(
                entries + static_cast<uintptr_t>(slot) * 0x70);
            if (!candidate) continue;
            const std::string className = GetEntityClassName(candidate);
            if (className.find("C_PortraitWorldUnit") == std::string::npos)
                continue;
            if (Read<int>(candidate + 0x10C8) == heroId) return candidate;
        }
        return 0;
    };

    const uint32_t first = callbackChunk > 2 ? callbackChunk - 2 : 0;
    const uint32_t last = (std::min)(63u, callbackChunk + 2);
    for (uint32_t chunk = first; chunk <= last; ++chunk)
        if (const uintptr_t unit = scanChunk(chunk)) return unit;
    for (uint32_t chunk = 0; chunk < 64; ++chunk) {
        if (chunk >= first && chunk <= last) continue;
        if (const uintptr_t unit = scanChunk(chunk)) return unit;
    }
    return 0;
}

bool ApplyCreepPreviewModel(uintptr_t panel) {
    if (!panel || !skeletonSetModelByNameTarget) return false;
    const uintptr_t unit = FindPortraitWorldUnit(panel, 1);
    if (!unit) return false;
    const uintptr_t sceneNode = Read<uintptr_t>(
        unit + Offsets::GameSceneNode);
    if (!sceneNode) return false;
    const uintptr_t currentModel = Read<uintptr_t>(sceneNode + 0x1F0);
    if (creepPreviewUnit != unit) {
        __try {
            // This is CSkeletonInstance::SetModel(const char *). It owns the
            // async resource request and invokes the normal strong-handle
            // SetModel overload from its completion callback.
            reinterpret_cast<void(__fastcall*)(uintptr_t, const char*)>(
                skeletonSetModelByNameTarget)(
                    sceneNode, "models/npc/trooper/trooper.vmdl");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("PREVIEW_FAIL_CREEP_MODEL_LOAD_EXCEPTION");
            return false;
        }
        creepPreviewUnit = unit;
        creepPreviewModel = currentModel;
        creepPreviewModelReady = false;
        settleFrames.store(16, std::memory_order_release);
        captureReleasePending.store(true, std::memory_order_release);
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "[ESP_PREVIEW] CreepModelLoad=ISSUED unit=0x%llx",
                      static_cast<unsigned long long>(unit));
        Log(message);
        return false;
    }

    if (currentModel && currentModel != creepPreviewModel) {
        creepPreviewModel = currentModel;
        creepPreviewModelReady = true;
        char message[224]{};
        std::snprintf(message, sizeof(message),
                      "[ESP_PREVIEW] CreepModel=PASS unit=0x%llx model=0x%llx",
                      static_cast<unsigned long long>(unit),
                      static_cast<unsigned long long>(currentModel));
        Log(message);
        settleFrames.store(8, std::memory_order_release);
        captureReleasePending.store(true, std::memory_order_release);
        return true;
    }
    return creepPreviewModelReady && currentModel != 0;
}

uintptr_t ConfigurePortraitCamera(uintptr_t panel) {
    // sub_1808A3F70 stores the selected C_PointCamera handle in the portrait
    // renderer at +0xAB0. C_PointCamera::m_FOV is schema-confirmed at +0x5F0.
    const uintptr_t renderer = panel ? Read<uintptr_t>(panel + 0x38) : 0;
    const int cameraHandle = renderer ? Read<int>(renderer + 0xAB0) : -1;
    const uintptr_t camera = ResolvePortraitEntityHandle(cameraHandle);
    if (!camera) return 0;
    const std::string className = GetEntityClassName(camera);
    if (className.find("C_PointCamera") == std::string::npos) return 0;
    if (configuredPortraitCamera != camera) {
        const float originalFov = Read<float>(camera + 0x5F0);
        if (!std::isfinite(originalFov) || originalFov < 20.0f ||
            originalFov > 140.0f) return 0;
        const float targetFov = (std::min)(100.0f, originalFov + 12.0f);
        Write<float>(camera + 0x5F0, targetFov);
        configuredPortraitCamera = camera;
        configuredPortraitFov = targetFov;
        settleFrames.store(4, std::memory_order_release);
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "[ESP_PREVIEW] PortraitCamera=PASS entity=0x%llx fov=%.1f->%.1f",
                      static_cast<unsigned long long>(camera), originalFov,
                      targetFov);
        Log(message);
    }
    return camera;
}

bool SpawnPanel(uintptr_t engine, uintptr_t contextPanel) {
    if (panelSpawned.load(std::memory_order_acquire)) return true;
    const ULONGLONG now = GetTickCount64();
    if (!spawnIssued || now - spawnIssuedAt >= 5000) {
        if (now - lastSpawnAttemptAt < 1000) return false;
        lastSpawnAttemptAt = now;
        // Clear exactly once. In this client the factory can run one or two
        // frames after RunScript returns; clearing again would replace the
        // guide's "first after clear" object with a later child/stale scene.
        ClearCreatedPanels();
        constexpr const char* script = R"JS(
(function(){
    var r = $.GetContextPanel();
    while (r.GetParent()) r = r.GetParent();
    var old = r.FindChildTraverse('Dll6_esp_preview');
    if (old) old.DeleteAsync(0);
    var box = $.CreatePanel('Panel', r, 'Dll6_esp_preview');
    box.style.width = '300px';
    box.style.height = '400px';
    // The D2D stage already supplies the rounded border. Keep the source
    // surface pure black so the captured panorama has no grey card behind it.
    box.style.backgroundColor = '#000000ff';
    box.style.borderRadius = '0px';
    box.style.overflow = 'squish';
    box.style.horizontalAlign = 'left';
    box.style.verticalAlign = 'top';
    box.style.visibility = 'collapse';
    box.style.zIndex = '2000000000';
    var scene = $.CreatePanel('CitadelHeroScenePanelNew', box, 'Dll6_esp_scene');
    scene.style.width = '100%';
    scene.style.height = '100%';
    scene.style.horizontalAlign = 'center';
    scene.style.verticalAlign = 'center';
    scene.style.washColor = 'none';
    // The default portrait camera is vertically biased for store/UI cards.
    // FOV below provides the missing feet; this translation centers the
    // complete model in the ESP stage without changing its pose/environment.
    scene.style.transform = 'translate3d(0px,-42px,0px)';
})();
)JS";
        if (!RunPanoramaScript(engine, contextPanel, script)) return false;
        spawnIssued = true;
        spawnIssuedAt = now;
        Log("Hero panel spawn issued; waiting for first factory result");
    }
    const uintptr_t panel = CreatedPanel();
    if (!panel) return false;
    const HeroPanelStatus status = ConfigureHeroPanel(panel);
    if (status == HeroPanelStatus::Invalid) {
        Log("Hero panel factory result is invalid");
        return false;
    }
    previewHeroPanel.store(panel, std::memory_order_release);
    lastHeroReloadAt = GetTickCount64();
    heroReloadAttempts.store(1, std::memory_order_release);
    rendererReady.store(status == HeroPanelStatus::Ready,
                        std::memory_order_release);
    reloadExhausted.store(false, std::memory_order_release);
    panelSpawned.store(true, std::memory_order_release);
    spawnIssued = false;
    spawnIssuedAt = 0;
    return true;
}

bool SetPanelTransform(uintptr_t engine, uintptr_t contextPanel,
                       float left, float top, float right, float bottom,
                       bool visible) {
    const float x = std::floor(left);
    const float y = std::floor(top);
    const float width = (std::max)(1.0f, std::floor(right) - x);
    const float height = (std::max)(1.0f, std::floor(bottom) - y);
    char script[1400]{};
    std::snprintf(script, sizeof(script), R"JS(
(function(){var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();
var p=r.FindChildTraverse('Dll6_esp_preview');
if(p){p.style.width='%0.0fpx';p.style.height='%0.0fpx';
p.style.transform='translate3d(%0.0fpx,%0.0fpx,0px)';
p.style.opacity='1.0';p.style.visibility='%s';
    var h=r.FindChildTraverse('Dll6_esp_scene');
    if(h)h.style.visibility='%s';}})();
)JS", width, height, x, y, visible ? "visible" : "collapse",
        visible ? "visible" : "collapse");
    return RunPanoramaScript(engine, contextPanel, script);
}

void InvalidatePanelState(uintptr_t engine, uintptr_t contextPanel) {
    activeCuiEngine.store(engine, std::memory_order_release);
    activeContextPanel.store(contextPanel, std::memory_order_release);
    previewHeroPanel.store(0, std::memory_order_release);
    panelSpawned.store(false, std::memory_order_release);
    panelVisible.store(false, std::memory_order_release);
    rendererReady.store(false, std::memory_order_release);
    reloadExhausted.store(false, std::memory_order_release);
    heroReloadAttempts.store(0, std::memory_order_release);
    ClearCreatedPanels();
    spawnIssued = false;
    spawnIssuedAt = 0;
    lastSpawnAttemptAt = 0;
    lastHeroReloadAt = 0;
    configuredPortraitCamera = 0;
    configuredPortraitFov = 0.0f;
    appliedGeneration.store(0, std::memory_order_release);
    appliedHeroId.store(0, std::memory_order_release);
    previewGlowUnit.store(0, std::memory_order_release);
    previewGlowRegistrationQueued.store(false, std::memory_order_release);
    previewNativeGlowActive.store(false, std::memory_order_release);
    settleFrames.store(0, std::memory_order_release);
    captureReleasePending.store(true, std::memory_order_release);
    Log("Panorama context changed; preview state invalidated");
}

void ProcessPendingUiWork(uintptr_t engine, uintptr_t contextPanel) {
    if (engine != activeCuiEngine.load(std::memory_order_acquire) ||
        contextPanel != activeContextPanel.load(std::memory_order_acquire)) {
        InvalidatePanelState(engine, contextPanel);
    }
    const uint64_t generation = requestedGeneration.load(std::memory_order_acquire);
    const bool visible = requestedVisible.load(std::memory_order_acquire);
    const float left = requestedLeft.load(std::memory_order_relaxed);
    const float top = requestedTop.load(std::memory_order_relaxed);
    const float right = requestedRight.load(std::memory_order_relaxed);
    const float bottom = requestedBottom.load(std::memory_order_relaxed);

    if (generation != appliedGeneration.load(std::memory_order_acquire)) {
        if (visible) {
            if (!SpawnPanel(engine, contextPanel))
                return;
            if (!SetPanelTransform(engine, contextPanel,
                                   left, top, right, bottom, true)) {
                Log("PREVIEW_FAIL_PANEL_TRANSFORM");
                return;
            }
            settleFrames.store(4, std::memory_order_release);
        } else if (panelSpawned.load(std::memory_order_acquire)) {
            SetPanelTransform(engine, contextPanel,
                              left, top, right, bottom, false);
        }
        panelVisible.store(visible && panelSpawned.load(std::memory_order_acquire),
                           std::memory_order_release);
        appliedGeneration.store(generation, std::memory_order_release);
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "UI request applied generation=%llu rect=%.0f,%.0f-%.0f,%.0f visible=%d",
                      static_cast<unsigned long long>(generation),
                      left, top, right, bottom, visible ? 1 : 0);
        Log(message);
    }

    const uintptr_t heroPanel = previewHeroPanel.load(std::memory_order_acquire);
    const ULONGLONG now = GetTickCount64();
    const int requestedHero = requestedHeroId.load(std::memory_order_acquire);
    if (visible && heroPanel &&
        appliedHeroId.load(std::memory_order_acquire) != requestedHero) {
        configuredPortraitCamera = 0;
        configuredPortraitFov = 0.0f;
        rendererReady.store(false, std::memory_order_release);
        heroReloadAttempts.store(0, std::memory_order_release);
        const HeroPanelStatus status = ConfigureHeroPanel(heroPanel);
        if (status != HeroPanelStatus::Invalid) {
            appliedHeroId.store(requestedHero, std::memory_order_release);
            rendererReady.store(status == HeroPanelStatus::Ready,
                                std::memory_order_release);
            settleFrames.store(8, std::memory_order_release);
            char script[1600]{};
            std::snprintf(script, sizeof(script), R"JS(
(function(){var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();
var ids=[1,13,7,12,19,27];for(var i=0;i<ids.length;++i){
var p=r.FindChildTraverse('Dll6_hero_'+ids[i]);if(p){
p.style.opacity=ids[i]==%d?'1.0':'0.58';
p.style.border=ids[i]==%d?'2px solid #35ff64':'1px solid #ffffff22';}}
var h=r.FindChildTraverse('Dll6_esp_scene');
if(h)h.style.visibility='visible';})()
)JS", requestedHero, requestedHero);
            RunPanoramaScript(engine, contextPanel, script);
            captureReleasePending.store(true, std::memory_order_release);
        }
    }
    const int attempts = heroReloadAttempts.load(std::memory_order_acquire);
    if (visible && heroPanel && !rendererReady.load(std::memory_order_acquire) &&
        attempts < 16 &&
        now - lastHeroReloadAt >= 500) {
        const HeroPanelStatus status = ConfigureHeroPanel(heroPanel);
        lastHeroReloadAt = now;
        heroReloadAttempts.store(attempts + 1, std::memory_order_release);
        if (status == HeroPanelStatus::Ready) {
            rendererReady.store(true, std::memory_order_release);
            settleFrames.store(4, std::memory_order_release);
            Log("[ESP_PREVIEW] Renderer=PASS HeroReload=PASS");
        } else if (status == HeroPanelStatus::Invalid) {
            reloadExhausted.store(true, std::memory_order_release);
            Log("PREVIEW_FAIL_WRONG_CREATED_PANEL");
        } else if (attempts + 1 >= 16) {
            reloadExhausted.store(true, std::memory_order_release);
            Log("PREVIEW_FAIL_RENDERER_NULL");
        }
    }
    if (visible && heroPanel && requestedHero == 55 &&
        rendererReady.load(std::memory_order_acquire) &&
        now - lastCreepModelAttemptAt >= 250) {
        lastCreepModelAttemptAt = now;
        if (!ApplyCreepPreviewModel(heroPanel) &&
            now - spawnIssuedAt >= 2000) {
            static ULONGLONG lastFailureLogAt = 0;
            if (now - lastFailureLogAt >= 2000) {
                lastFailureLogAt = now;
                Log("PREVIEW_WAIT_CREEP_UNIT_OR_MODEL_LOAD");
            }
        }
    }
}

bool EnsureCaptureTexture(ID3D11Device* device, UINT width, UINT height,
                          DXGI_FORMAT format) {
    if (captureTexture && captureWidth == width && captureHeight == height &&
        captureFormat == format) return true;
    captureTexture.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr,
                                       captureTexture.GetAddressOf()))) return false;
    captureWidth = width;
    captureHeight = height;
    captureFormat = format;
    char message[224]{};
    std::snprintf(message, sizeof(message),
                  "[ESP_PREVIEW] CaptureResource=PASS size=%ux%u format=%u",
                  width, height, static_cast<unsigned>(format));
    Log(message);
    return true;
}

bool EnsureResolveTexture(ID3D11Device* device,
                          const D3D11_TEXTURE2D_DESC& sourceDesc) {
    if (resolveTexture && resolveWidth == sourceDesc.Width &&
        resolveHeight == sourceDesc.Height &&
        resolveFormat == sourceDesc.Format) return true;
    resolveTexture.Reset();
    D3D11_TEXTURE2D_DESC desc = sourceDesc;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D(&desc, nullptr,
                                       resolveTexture.GetAddressOf()))) {
        Log("PREVIEW_FAIL_MSAA_RESOLVE_RESOURCE");
        return false;
    }
    resolveWidth = desc.Width;
    resolveHeight = desc.Height;
    resolveFormat = desc.Format;
    Log("[ESP_PREVIEW] MSAAResolveResource=PASS");
    return true;
}

void ProbeCapturedPixels(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context || !captureTexture) return;
    const bool rgba = captureFormat == DXGI_FORMAT_R8G8B8A8_UNORM ||
        captureFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const bool bgra = captureFormat == DXGI_FORMAT_B8G8R8A8_UNORM ||
        captureFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    if (!rgba && !bgra) {
        Log("[ESP_PREVIEW] PixelProbe=SKIP_UNSUPPORTED_FORMAT");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    captureTexture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()))) {
        Log("PREVIEW_FAIL_PIXEL_PROBE_RESOURCE");
        return;
    }
    context->CopyResource(staging.Get(), captureTexture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        Log("PREVIEW_FAIL_PIXEL_PROBE_MAP");
        return;
    }
    unsigned minimum = 255, maximum = 0;
    uint64_t sum = 0;
    unsigned samples = 0;
    for (UINT y = 0; y < desc.Height; y += (std::max)(1u, desc.Height / 24)) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; x += (std::max)(1u, desc.Width / 24)) {
            const uint8_t* pixel = row + static_cast<size_t>(x) * 4;
            const unsigned r = rgba ? pixel[0] : pixel[2];
            const unsigned g = pixel[1];
            const unsigned b = rgba ? pixel[2] : pixel[0];
            const unsigned luminance = (r * 54 + g * 183 + b * 19) / 256;
            minimum = (std::min)(minimum, luminance);
            maximum = (std::max)(maximum, luminance);
            sum += luminance;
            ++samples;
        }
    }
    context->Unmap(staging.Get(), 0);
    char message[224]{};
    std::snprintf(message, sizeof(message),
                  "[ESP_PREVIEW] PixelProbe=PASS min=%u max=%u mean=%u samples=%u",
                  minimum, maximum, samples ? static_cast<unsigned>(sum / samples) : 0,
                  samples);
    Log(message);
}

} // namespace

bool InitializePanoramaPreview() {
    if (initialized.load(std::memory_order_acquire)) return true;
    const ULONGLONG now = GetTickCount64();
    if (now - lastInitializeAttemptAt < 1000) return false;
    lastInitializeAttemptAt = now;
    HMODULE panorama = GetModuleHandleA("panorama.dll");
    HMODULE client = GetModuleHandleA("client.dll");
    if (!panorama || !client) {
        Log("client.dll or panorama.dll is not loaded");
        return false;
    }
    panoramaBase = reinterpret_cast<uintptr_t>(panorama);
    const uintptr_t runScript = FindPattern(
        panorama, "48 89 5C 24 18 4C 89 4C 24 20 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00");
    const uintptr_t cuiGlobalPattern = FindPattern(
        panorama, "83 39 00 75 06 83 79 04 FF 74 ? 4C 8B 05 ? ? ? ? 4D 85 C0");
    const uintptr_t hudPattern = FindPattern(
        client, "48 8D 8F 50 01 00 00 E8 ? ? ? ? 48 89 3D ? ? ? ? B2 01 48");
    const uintptr_t portraitEntityListPattern = FindPattern(
        client, "4C 8B 05 ? ? ? ? 4D 85 C0 74 ? 83 FA FE");
    // IDA 2026-07-31: registered CitadelHeroScenePanelNew factory
    // sub_181A5A600. The current object is 0x100 bytes; the guide's older
    // build allocated 0xF8, which also matches unrelated panel factories.
    uintptr_t heroCreate = FindPattern(
        client, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 "
                "48 8B F1 48 8B FA B9 00 01 00 00 E8 ? ? ? ? "
                "48 8B D8 48 85 C0 74 5C");
    skeletonSetModelByNameTarget = reinterpret_cast<void*>(FindPattern(
        client, "48 89 54 24 10 53 48 83 EC 30 48 8B D9 48 85 D2 "
                "0F 84 ? ? ? ? 80 3A 00 0F 84 ? ? ? ? "
                "48 8B 91 E0 03 00 00"));
    if (!runScript || !cuiGlobalPattern || !hudPattern || !heroCreate) {
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "patterns missing run=0x%llx cui=0x%llx hud=0x%llx hero=0x%llx",
                      static_cast<unsigned long long>(runScript),
                      static_cast<unsigned long long>(cuiGlobalPattern),
                      static_cast<unsigned long long>(hudPattern),
                      static_cast<unsigned long long>(heroCreate));
        Log(message);
        return false;
    }
    runScriptGlobal = runScript;
    cuiEngineGlobal = RipGlobal(cuiGlobalPattern, 14);
    hudMinimapGlobal = RipGlobal(hudPattern, 15);
    portraitEntityListGlobal = portraitEntityListPattern
        ? RipGlobal(portraitEntityListPattern, 3) : 0;
    if (!cuiEngineGlobal || !hudMinimapGlobal) {
        Log("RIP-relative globals are invalid");
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MinHook initialization failed");
        return false;
    }
    runScriptTarget = reinterpret_cast<void*>(runScript);
    heroPanelCreateTarget = reinterpret_cast<void*>(heroCreate);
    if (MH_CreateHook(runScriptTarget,
                      reinterpret_cast<void*>(&HookRunScript),
                      reinterpret_cast<void**>(&originalRunScript)) != MH_OK) {
        Log("CUIEngine::RunScript hook creation failed");
        return false;
    }
    if (MH_CreateHook(heroPanelCreateTarget,
                      reinterpret_cast<void*>(&HookHeroPanelCreate),
                      reinterpret_cast<void**>(&originalHeroPanelCreate)) != MH_OK) {
        Log("HeroScenePanelCreate hook creation failed");
        MH_RemoveHook(runScriptTarget);
        return false;
    }
    if (MH_EnableHook(runScriptTarget) != MH_OK) {
        Log("CUIEngine::RunScript hook could not be enabled");
        MH_RemoveHook(heroPanelCreateTarget);
        MH_RemoveHook(runScriptTarget);
        return false;
    }
    if (MH_EnableHook(heroPanelCreateTarget) != MH_OK) {
        Log("HeroScenePanelCreate hook could not be enabled");
        MH_DisableHook(runScriptTarget);
        MH_RemoveHook(heroPanelCreateTarget);
        MH_RemoveHook(runScriptTarget);
        return false;
    }
    initialized.store(true, std::memory_order_release);
    Log("=== ESP PREVIEW SESSION START ===");
    char addresses[320]{};
    std::snprintf(addresses, sizeof(addresses),
                  "[ESP_PREVIEW] Signatures=PASS run=0x%llx cuiGlobal=0x%llx "
                  "hudGlobal=0x%llx heroFactory=0x%llx",
                  static_cast<unsigned long long>(runScript),
                  static_cast<unsigned long long>(cuiEngineGlobal),
                  static_cast<unsigned long long>(hudMinimapGlobal),
                  static_cast<unsigned long long>(heroCreate));
    Log(addresses);
    if (portraitEntityListGlobal) {
        char skeletonAddress[160]{};
        std::snprintf(skeletonAddress, sizeof(skeletonAddress),
                      "[ESP_PREVIEW] PortraitEntityList=PASS global=0x%llx",
                      static_cast<unsigned long long>(portraitEntityListGlobal));
        Log(skeletonAddress);
    } else {
        Log("PREVIEW_FAIL_PORTRAIT_ENTITY_LIST_PATTERN");
    }
    if (skeletonSetModelByNameTarget) {
        char setModelAddress[144]{};
        std::snprintf(setModelAddress, sizeof(setModelAddress),
                      "[ESP_PREVIEW] SkeletonSetModelByName=PASS address=0x%llx",
                      static_cast<unsigned long long>(
                          reinterpret_cast<uintptr_t>(
                              skeletonSetModelByNameTarget)));
        Log(setModelAddress);
    } else {
        Log("PREVIEW_FAIL_SKELETON_SET_MODEL_BY_NAME_PATTERN");
    }
    Log("Panorama hooks initialized");
    return true;
}

void UpdatePanoramaPreview(IDXGISwapChain* swapChain,
                           ID3D11DeviceContext* context,
                           float left, float top, float right, float bottom,
                           bool visible) {
    if (!swapChain || !context || !InitializePanoramaPreview()) return;
    if (captureReleasePending.exchange(false, std::memory_order_acq_rel)) {
        captureTexture.Reset();
        resolveTexture.Reset();
        captureWidth = captureHeight = resolveWidth = resolveHeight = 0;
        captureFormat = resolveFormat = DXGI_FORMAT_UNKNOWN;
        capturedGeneration = 0;
        captureSerial.store(0, std::memory_order_release);
        displayedSerial.store(0, std::memory_order_release);
        failedBindingSerial.store(0, std::memory_order_release);
        Log("[ESP_PREVIEW] RenderResourcesInvalidated=PASS");
    }
    const bool changed = left != previousLeft || top != previousTop ||
        right != previousRight || bottom != previousBottom ||
        visible != requestedVisible.load(std::memory_order_acquire);
    if (changed) {
        previousLeft = left; previousTop = top;
        previousRight = right; previousBottom = bottom;
        requestedLeft.store(left, std::memory_order_relaxed);
        requestedTop.store(top, std::memory_order_relaxed);
        requestedRight.store(right, std::memory_order_relaxed);
        requestedBottom.store(bottom, std::memory_order_relaxed);
        requestedVisible.store(visible, std::memory_order_relaxed);
        const uint64_t generation = requestedGeneration.fetch_add(
            1, std::memory_order_release) + 1;
        char message[192]{};
        std::snprintf(message, sizeof(message),
                      "preview request generation=%llu rect=%.0f,%.0f-%.0f,%.0f visible=%d",
                      static_cast<unsigned long long>(generation),
                      left, top, right, bottom, visible ? 1 : 0);
        Log(message);
        lastUiDispatchAt = 0;
    }
    if (!visible) {
        panelVisible.store(false, std::memory_order_release);
        if (changed && gameWindow)
            PostMessageW(gameWindow, PanoramaPreviewUiMessage, 0, 0);
        return;
    }
    const ULONGLONG now = GetTickCount64();
    const uintptr_t currentEngine = cuiEngineGlobal
        ? Read<uintptr_t>(cuiEngineGlobal) : 0;
    const uintptr_t currentHud = hudMinimapGlobal
        ? Read<uintptr_t>(hudMinimapGlobal) : 0;
    const uintptr_t currentContext = currentHud
        ? Read<uintptr_t>(currentHud + 0x08) : 0;
    const bool contextChanged = currentEngine && currentContext &&
        (currentEngine != activeCuiEngine.load(std::memory_order_acquire) ||
         currentContext != activeContextPanel.load(std::memory_order_acquire));
    const bool requestPending = requestedGeneration.load(std::memory_order_acquire) !=
        appliedGeneration.load(std::memory_order_acquire);
    const bool reloadPending = !rendererReady.load(std::memory_order_acquire) &&
        !reloadExhausted.load(std::memory_order_acquire);
    // UI script execution is expensive enough to stall the render thread when
    // delivered synchronously. Keep it on the window message queue.
    if ((requestPending || reloadPending || contextChanged) &&
        gameWindow && now - lastUiDispatchAt >= 16) {
        PostMessageW(gameWindow, PanoramaPreviewUiMessage, 0, 0);
        lastUiDispatchAt = now;
        if (requestPending || contextChanged) return;
    }
    if (!panelSpawned.load(std::memory_order_acquire) ||
        !panelVisible.load(std::memory_order_acquire) ||
        !rendererReady.load(std::memory_order_acquire)) return;
    ConfigurePortraitCamera(previewHeroPanel.load(std::memory_order_acquire));
    int remainingSettle = settleFrames.load(std::memory_order_acquire);
    if (remainingSettle > 0) {
        settleFrames.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    ID3D11Device* device = nullptr;
    if (FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device))) || !device) return;
    DXGI_SWAP_CHAIN_DESC swapDesc{};
    if (FAILED(swapChain->GetDesc(&swapDesc))) {
        device->Release();
        return;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer) {
        device->Release();
        return;
    }
    D3D11_TEXTURE2D_DESC backDesc{};
    backBuffer->GetDesc(&backDesc);
    const LONG srcLeft = (std::max)(0L, static_cast<LONG>(std::floor(left)));
    const LONG srcTop = (std::max)(0L, static_cast<LONG>(std::floor(top)));
    const LONG srcRight = (std::min)(static_cast<LONG>(backDesc.Width),
                                     static_cast<LONG>(std::floor(right)));
    const LONG srcBottom = (std::min)(static_cast<LONG>(backDesc.Height),
                                      static_cast<LONG>(std::floor(bottom)));
    if (srcRight <= srcLeft || srcBottom <= srcTop ||
        !EnsureCaptureTexture(device, static_cast<UINT>(srcRight - srcLeft),
                              static_cast<UINT>(srcBottom - srcTop), backDesc.Format)) {
        Log("PREVIEW_FAIL_CAPTURE_RECT_OR_RESOURCE");
        backBuffer->Release(); device->Release(); return;
    }
    ID3D11Texture2D* copySource = backBuffer;
    if (backDesc.SampleDesc.Count > 1) {
        if (!EnsureResolveTexture(device, backDesc)) {
            backBuffer->Release(); device->Release(); return;
        }
        context->ResolveSubresource(resolveTexture.Get(), 0, backBuffer, 0,
                                    backDesc.Format);
        copySource = resolveTexture.Get();
    }
    D3D11_BOX box{static_cast<UINT>(srcLeft), static_cast<UINT>(srcTop), 0,
                  static_cast<UINT>(srcRight), static_cast<UINT>(srcBottom), 1};
    context->CopySubresourceRegion(captureTexture.Get(), 0, 0, 0, 0,
                                   copySource, 0, &box);
    const uint64_t generation = requestedGeneration.load(std::memory_order_acquire);
    if (capturedGeneration != generation) {
        capturedGeneration = generation;
        const uint64_t serial = captureSerial.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        char message[256]{};
        std::snprintf(message, sizeof(message),
                      "[ESP_PREVIEW] CaptureCopy=PASS serial=%llu generation=%llu "
                      "src=%ld,%ld-%ld,%ld backbuffer=%ux%u samples=%u format=%u",
                      static_cast<unsigned long long>(serial),
                      static_cast<unsigned long long>(generation),
                      srcLeft, srcTop, srcRight, srcBottom,
                      backDesc.Width, backDesc.Height, backDesc.SampleDesc.Count,
                      static_cast<unsigned>(backDesc.Format));
        Log(message);
        ProbeCapturedPixels(device, context);
    }
    backBuffer->Release();
    device->Release();
}

ID3D11Texture2D* GetPanoramaPreviewTexture() {
    // Retain the most recent valid frame while the source panel is hidden
    // during a menu drag.  D2D can then move the preview smoothly without a
    // delayed, duplicate Panorama panel being visible elsewhere.
    return rendererReady.load(std::memory_order_acquire) &&
        captureSerial.load(std::memory_order_acquire) != 0
        ? captureTexture.Get() : nullptr;
}

uint64_t GetPanoramaPreviewCaptureSerial() {
    return captureSerial.load(std::memory_order_acquire);
}

bool GetPanoramaPreviewSkeleton(Preview3DPoint* points, std::size_t count) {
    if (!points || count < 18 || !portraitEntityListGlobal) return false;
    for (std::size_t i = 0; i < count; ++i) points[i] = {};
    const uintptr_t panel = previewHeroPanel.load(std::memory_order_acquire);
    const uintptr_t portraitWorld = panel ? Read<uintptr_t>(panel + 0x40) : 0;
    const int handle = portraitWorld ? Read<int>(portraitWorld + 0xC8) : -1;
    if (!portraitWorld || handle == -1 || handle == -2) return false;

    const uintptr_t entityList = Read<uintptr_t>(portraitEntityListGlobal);
    if (!entityList) return false;
    const uint32_t index = static_cast<uint32_t>(handle) & 0x7FFFu;
    SuppressPortraitParticles(panel, entityList, index >> 9);
    const uintptr_t chunk = entityList
        ? Read<uintptr_t>(entityList + static_cast<uintptr_t>(index >> 9) * 8)
        : 0;
    const uintptr_t identity = chunk
        ? chunk + static_cast<uintptr_t>(index & 0x1FFu) * 0x70
        : 0;
    if (!identity || Read<int>(identity + 0x10) != handle) return false;
    const uintptr_t callbackEntity = Read<uintptr_t>(identity);
    if (!callbackEntity) return false;

    // +0xC8 is the C_PortraitWorldCallbackHandler handle, not the rendered
    // hero.  The actual skinned model is a C_PortraitWorldUnit created in the
    // same isolated entity list.  Cache that unit after locating it by its
    // exact RTTI class; calling the bone API on the callback was why the old
    // implementation could never produce a skeleton.
    static uintptr_t cachedPanel = 0;
    static uintptr_t cachedUnit = 0;
    static ULONGLONG lastScanAt = 0;
    if (cachedPanel != panel) {
        cachedPanel = panel;
        cachedUnit = 0;
        lastScanAt = 0;
    }

    std::array<Vector3, 18> bones{};
    std::array<bool, 18> valid{};
    const int selectedHero = requestedHeroId.load(std::memory_order_acquire);
    if (cachedUnit && Read<int>(cachedUnit + 0x10C8) != selectedHero)
        cachedUnit = 0;
    if (cachedUnit && !GetEntityPreviewSkeleton(cachedUnit, bones, valid))
        cachedUnit = 0;

    const ULONGLONG now = GetTickCount64();
    if (!cachedUnit && (lastScanAt == 0 || now - lastScanAt >= 250)) {
        lastScanAt = now;
        const uint32_t callbackChunk = index >> 9;
        const auto scanChunk = [&](uint32_t chunkIndex) -> bool {
            const uintptr_t entries = Read<uintptr_t>(
                entityList + static_cast<uintptr_t>(chunkIndex) * 8);
            if (!entries) return false;
            for (uint32_t slot = 0; slot < 512; ++slot) {
                const uintptr_t candidateIdentity = entries +
                    static_cast<uintptr_t>(slot) * 0x70;
                const uintptr_t candidate = Read<uintptr_t>(candidateIdentity);
                if (!candidate || candidate == callbackEntity) continue;
                const std::string className = GetEntityClassName(candidate);
                if (className.find("C_PortraitWorldUnit") == std::string::npos)
                    continue;
                // C_PortraitWorldUnit::m_heroID is schema-confirmed at +0x10C8.
                // Ignore auxiliary and stale units left by a hero reload.
                if (Read<int>(candidate + 0x10C8) != selectedHero) continue;
                std::array<Vector3, 18> candidateBones{};
                std::array<bool, 18> candidateValid{};
                if (!GetEntityPreviewSkeleton(
                        candidate, candidateBones, candidateValid)) continue;
                cachedUnit = candidate;
                bones = candidateBones;
                valid = candidateValid;
                char message[192]{};
                std::snprintf(message, sizeof(message),
                              "[ESP_PREVIEW] SkeletonUnit=PASS entity=0x%llx class=%s",
                              static_cast<unsigned long long>(candidate),
                              className.c_str());
                Log(message);
                return true;
            }
            return false;
        };

        // Portrait entities are normally allocated beside their callback.
        // Search those chunks first, then the remaining handle space once as
        // a robust fallback for a fragmented entity list.
        const uint32_t first = callbackChunk > 2 ? callbackChunk - 2 : 0;
        const uint32_t last = (std::min)(63u, callbackChunk + 2);
        bool found = false;
        for (uint32_t chunkIndex = first; chunkIndex <= last && !found;
             ++chunkIndex)
            found = scanChunk(chunkIndex);
        for (uint32_t chunkIndex = 0; chunkIndex < 64 && !found; ++chunkIndex) {
            if (chunkIndex >= first && chunkIndex <= last) continue;
            found = scanChunk(chunkIndex);
        }
    }
    if (!cachedUnit) return false;
    if (std::count(valid.begin(), valid.end(), true) < 10 &&
        !GetEntityPreviewSkeleton(cachedUnit, bones, valid)) return false;
    ConfigurePortraitCamera(panel);
    const uintptr_t renderer = Read<uintptr_t>(panel + 0x38);
    if (!renderer) return false;

    // CCitadel_PortraitWorldRenderer_New owns a CViewSetup at +0x20.  Source
    // 2 builds its real world-to-projection matrix at CViewSetup::+0x288;
    // using that matrix is essential because the portrait renderer modifies
    // the camera setup after reading C_PointCamera.  Reconstructing a camera
    // from the entity quaternion/FOV therefore cannot match the rendered
    // pixels (especially for an animated, non-axis-aligned pose).
    struct Matrix4x4 { float m[4][4]; };
    const Matrix4x4 worldToProjection = Read<Matrix4x4>(renderer + 0x2A8);
    bool matrixFinite = true;
    for (const auto& row : worldToProjection.m)
        for (float value : row)
            matrixFinite = matrixFinite && std::isfinite(value);
    if (!matrixFinite) return false;
    const float panelWidth = requestedRight.load(std::memory_order_relaxed) -
        requestedLeft.load(std::memory_order_relaxed);
    const float panelHeight = requestedBottom.load(std::memory_order_relaxed) -
        requestedTop.load(std::memory_order_relaxed);
    if (panelWidth < 1.0f || panelHeight < 1.0f) return false;
    for (size_t i = 0; i < bones.size(); ++i) {
        points[i] = {};
        if (!valid[i]) continue;
        const Vector3& bone = bones[i];
        const float clipX = worldToProjection.m[0][0] * bone.x +
            worldToProjection.m[0][1] * bone.y +
            worldToProjection.m[0][2] * bone.z + worldToProjection.m[0][3];
        const float clipY = worldToProjection.m[1][0] * bone.x +
            worldToProjection.m[1][1] * bone.y +
            worldToProjection.m[1][2] * bone.z + worldToProjection.m[1][3];
        const float clipW = worldToProjection.m[3][0] * bone.x +
            worldToProjection.m[3][1] * bone.y +
            worldToProjection.m[3][2] * bone.z + worldToProjection.m[3][3];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
            !std::isfinite(clipW) || clipW <= 0.001f) continue;
        points[i].x = 0.5f + 0.5f * clipX / clipW;
        // The scene panel itself is translated upward inside the capture
        // card, so convert renderer-local NDC to capture-local coordinates.
        const float sceneY = 0.5f - 0.5f * clipY / clipW;
        points[i].y = sceneY - 42.0f / panelHeight;
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) ||
            points[i].x < -0.25f || points[i].x > 1.25f ||
            points[i].y < -0.25f || points[i].y > 1.25f) continue;
        points[i].visible = true;
    }
    return std::count_if(points, points + 18,
                         [](const Preview3DPoint& point) {
                             return point.visible;
                         }) >= 10;
}

void SetPanoramaPreviewHero(int heroId) {
    switch (heroId) {
        case 1: case 2: case 3: case 4: case 6: case 7: case 8:
        case 10: case 11: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20: case 25: case 27: case 31:
        case 35: case 50: case 52: case 58: case 60: case 63: case 64:
        case 65: case 66: case 67: case 69: case 72: case 76: case 77:
        case 79: case 80: case 81:
            break;
        case 55: // models/npc/trooper/trooper.vmdl_c
            break;
        default:
            return;
    }
    if (requestedHeroId.exchange(heroId, std::memory_order_acq_rel) == heroId)
        return;
    lastUiDispatchAt = 0;
    if (gameWindow)
        PostMessageW(gameWindow, PanoramaPreviewUiMessage, 0, 0);
}

void SetPanoramaPreviewRole(int role) {
    role = std::clamp(role, 0, 2);
    previewRole.store(role, std::memory_order_release);
    const int selected = role == 0 ? enemyHeroId.load(std::memory_order_acquire) :
        role == 1 ? allyHeroId.load(std::memory_order_acquire) :
        creepPreviewId.load(std::memory_order_acquire);
    SetPanoramaPreviewHero(selected);
}

void SetPanoramaPreviewHeroForRole(int role, int heroId) {
    if (role == 0) enemyHeroId.store(heroId, std::memory_order_release);
    else if (role == 1) allyHeroId.store(heroId, std::memory_order_release);
    else if (role == 2) creepPreviewId.store(heroId, std::memory_order_release);
    if (previewRole.load(std::memory_order_acquire) == role)
        SetPanoramaPreviewHero(heroId);
}

int GetPanoramaPreviewHeroForRole(int role) {
    if (role == 0) return enemyHeroId.load(std::memory_order_acquire);
    if (role == 1) return allyHeroId.load(std::memory_order_acquire);
    return creepPreviewId.load(std::memory_order_acquire);
}

bool IsPanoramaPreviewNativeGlowActive() {
    return previewNativeGlowActive.load(std::memory_order_acquire);
}

void ReportPanoramaPreviewGlowRegistration(bool success) {
    previewNativeGlowActive.store(success, std::memory_order_release);
    previewGlowRegistrationQueued.store(false, std::memory_order_release);
    Log(success ? "[ESP_PREVIEW] NativeGlow=PASS"
                : "[ESP_PREVIEW] NativeGlow=FAIL");
}

int GetPanoramaPreviewHero() {
    return requestedHeroId.load(std::memory_order_acquire);
}

void ReportPanoramaPreviewBinding(bool success) {
    const uint64_t serial = captureSerial.load(std::memory_order_acquire);
    if (!serial) return;
    if (success) {
        const uint64_t previous = displayedSerial.exchange(
            serial, std::memory_order_acq_rel);
        if (previous != serial) {
            char message[128]{};
            std::snprintf(message, sizeof(message),
                          "[ESP_PREVIEW] Displayed=YES serial=%llu",
                          static_cast<unsigned long long>(serial));
            Log(message);
        }
    } else {
        const uint64_t previous = failedBindingSerial.exchange(
            serial, std::memory_order_acq_rel);
        if (previous != serial) Log("PREVIEW_FAIL_D2D_BIND");
    }
}

void ProcessPanoramaPreviewUiThread() {
    if (!initialized.load(std::memory_order_acquire)) return;
    uintptr_t engine = cuiEngineGlobal ? Read<uintptr_t>(cuiEngineGlobal) : 0;
    const uintptr_t hudMinimap = hudMinimapGlobal
        ? Read<uintptr_t>(hudMinimapGlobal) : 0;
    uintptr_t panel = hudMinimap ? Read<uintptr_t>(hudMinimap + 0x08) : 0;
    if (!engine || !panel) {
        engine = liveCuiEngine.load(std::memory_order_acquire);
        panel = liveContextPanel.load(std::memory_order_acquire);
    }
    if (!engine || !panel) {
        Log("WndProc dispatch has no CUIEngine/context panel");
        return;
    }
    ProcessPendingUiWork(engine, panel);
}

void ShutdownPanoramaPreview() {
    // The Panorama source is an independent game panel.  Remove it while the
    // RunScript hook is still live; otherwise it can outlast an unload as a
    // black/stale preview rectangle on screen.
    const uintptr_t engine = activeCuiEngine.load(std::memory_order_acquire);
    const uintptr_t contextPanel = activeContextPanel.load(std::memory_order_acquire);
    if (engine && contextPanel && originalRunScript) {
        constexpr const char* removePreviewScript = R"JS(
(function(){var r=$.GetContextPanel();while(r.GetParent())r=r.GetParent();
var p=r.FindChildTraverse('Dll6_esp_preview');if(p)p.DeleteAsync(0);})();
)JS";
        RunPanoramaScript(engine, contextPanel, removePreviewScript);
    }
    if (runScriptTarget) {
        MH_DisableHook(runScriptTarget);
        MH_RemoveHook(runScriptTarget);
    }
    if (heroPanelCreateTarget) {
        MH_DisableHook(heroPanelCreateTarget);
        MH_RemoveHook(heroPanelCreateTarget);
    }
    runScriptTarget = nullptr;
    heroPanelCreateTarget = nullptr;
    skeletonSetModelByNameTarget = nullptr;
    originalRunScript = nullptr;
    originalHeroPanelCreate = nullptr;
    captureTexture.Reset();
    resolveTexture.Reset();
    captureWidth = captureHeight = 0;
    resolveWidth = resolveHeight = 0;
    captureFormat = DXGI_FORMAT_UNKNOWN;
    resolveFormat = DXGI_FORMAT_UNKNOWN;
    panelSpawned.store(false, std::memory_order_release);
    panelVisible.store(false, std::memory_order_release);
    requestedVisible.store(false, std::memory_order_release);
    requestedLeft.store(0.0f, std::memory_order_relaxed);
    requestedTop.store(0.0f, std::memory_order_relaxed);
    requestedRight.store(0.0f, std::memory_order_relaxed);
    requestedBottom.store(0.0f, std::memory_order_relaxed);
    requestedGeneration.store(0, std::memory_order_release);
    appliedGeneration.store(0, std::memory_order_release);
    requestedHeroId.store(1, std::memory_order_release);
    previewGlowUnit.store(0, std::memory_order_release);
    previewGlowRegistrationQueued.store(false, std::memory_order_release);
    previewNativeGlowActive.store(false, std::memory_order_release);
    appliedHeroId.store(0, std::memory_order_release);
    enemyHeroId.store(1, std::memory_order_release);
    allyHeroId.store(1, std::memory_order_release);
    creepPreviewId.store(55, std::memory_order_release);
    previewRole.store(0, std::memory_order_release);
    initialized.store(false, std::memory_order_release);
    firstCreatedPanel.store(0, std::memory_order_release);
    lastCreatedPanel.store(0, std::memory_order_release);
    liveCuiEngine.store(0, std::memory_order_release);
    liveContextPanel.store(0, std::memory_order_release);
    previewHeroPanel.store(0, std::memory_order_release);
    portraitEntityListGlobal = 0;
    configuredPortraitCamera = 0;
    configuredPortraitFov = 0.0f;
    lastInitializeAttemptAt = 0;
    lastSpawnAttemptAt = 0;
    spawnIssuedAt = 0;
    lastHeroReloadAt = 0;
    lastUiDispatchAt = 0;
    lastCreepModelAttemptAt = 0;
    heroReloadAttempts.store(0, std::memory_order_release);
    rendererReady.store(false, std::memory_order_release);
    reloadExhausted.store(false, std::memory_order_release);
    captureReleasePending.store(false, std::memory_order_release);
    activeCuiEngine.store(0, std::memory_order_release);
    activeContextPanel.store(0, std::memory_order_release);
    spawnIssued = false;
    creepPreviewUnit = 0;
    creepPreviewModel = 0;
    creepPreviewModelReady = false;
    previousLeft = previousTop = previousRight = previousBottom = -1.0f;
    settleFrames.store(0, std::memory_order_release);
    capturedGeneration = 0;
    captureSerial.store(0, std::memory_order_release);
    displayedSerial.store(0, std::memory_order_release);
    failedBindingSerial.store(0, std::memory_order_release);
}
