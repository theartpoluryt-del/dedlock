#include "shared.h"
#include "hero_scripts.h"
#include "panorama_preview.h"
#include "portable_paths.h"
#include "resource.h"
#include <cwctype>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>

namespace {
std::mutex embeddedResourceMutex;
std::unordered_map<UINT, std::vector<unsigned char>> embeddedResources;
bool embeddedResourcesCached = false;

template <typename T>
const T* ImageAt(const unsigned char* image, size_t imageSize,
                 size_t offset, size_t count = 1) {
    if (!image || count > (std::numeric_limits<size_t>::max)() / sizeof(T))
        return nullptr;
    const size_t byteCount = sizeof(T) * count;
    if (offset > imageSize || byteCount > imageSize - offset) return nullptr;
    return reinterpret_cast<const T*>(image + offset);
}

bool CacheRcDataFromMappedImage(
    HMODULE module,
    std::unordered_map<UINT, std::vector<unsigned char>>& resources) {
    const auto* image = reinterpret_cast<const unsigned char*>(module);
    if (!image) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x100000) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        image + static_cast<size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& resourceData = nt->OptionalHeader.DataDirectory[
        IMAGE_DIRECTORY_ENTRY_RESOURCE];
    const size_t resourceRva = resourceData.VirtualAddress;
    const size_t resourceSize = resourceData.Size;
    if (!resourceRva || resourceSize < sizeof(IMAGE_RESOURCE_DIRECTORY) ||
        resourceRva > imageSize || resourceSize > imageSize - resourceRva)
        return false;

    const auto directoryAt = [&](DWORD relativeOffset)
        -> const IMAGE_RESOURCE_DIRECTORY* {
        if (relativeOffset > resourceSize ||
            sizeof(IMAGE_RESOURCE_DIRECTORY) > resourceSize - relativeOffset)
            return nullptr;
        return ImageAt<IMAGE_RESOURCE_DIRECTORY>(
            image, imageSize, resourceRva + relativeOffset);
    };
    const auto entriesFor = [&](const IMAGE_RESOURCE_DIRECTORY* directory)
        -> const IMAGE_RESOURCE_DIRECTORY_ENTRY* {
        if (!directory) return nullptr;
        const size_t count = static_cast<size_t>(directory->NumberOfNamedEntries) +
            directory->NumberOfIdEntries;
        const size_t offset = reinterpret_cast<const unsigned char*>(directory) -
            image + sizeof(IMAGE_RESOURCE_DIRECTORY);
        return ImageAt<IMAGE_RESOURCE_DIRECTORY_ENTRY>(
            image, imageSize, offset, count);
    };

    const auto* root = directoryAt(0);
    const auto* typeEntries = entriesFor(root);
    if (!root || !typeEntries) return false;
    const size_t typeCount = static_cast<size_t>(root->NumberOfNamedEntries) +
        root->NumberOfIdEntries;
    const IMAGE_RESOURCE_DIRECTORY* rcDataDirectory = nullptr;
    for (size_t i = 0; i < typeCount; ++i) {
        if (!typeEntries[i].NameIsString && typeEntries[i].Id == 10 &&
            typeEntries[i].DataIsDirectory) {
            rcDataDirectory = directoryAt(typeEntries[i].OffsetToDirectory);
            break;
        }
    }
    const auto* nameEntries = entriesFor(rcDataDirectory);
    if (!rcDataDirectory || !nameEntries) return false;
    const size_t nameCount =
        static_cast<size_t>(rcDataDirectory->NumberOfNamedEntries) +
        rcDataDirectory->NumberOfIdEntries;

    for (size_t i = 0; i < nameCount; ++i) {
        const auto& nameEntry = nameEntries[i];
        if (nameEntry.NameIsString || !nameEntry.DataIsDirectory) continue;
        const auto* languageDirectory = directoryAt(nameEntry.OffsetToDirectory);
        const auto* languageEntries = entriesFor(languageDirectory);
        if (!languageDirectory || !languageEntries) continue;
        const size_t languageCount =
            static_cast<size_t>(languageDirectory->NumberOfNamedEntries) +
            languageDirectory->NumberOfIdEntries;
        for (size_t language = 0; language < languageCount; ++language) {
            if (languageEntries[language].DataIsDirectory) continue;
            const DWORD dataOffset = languageEntries[language].OffsetToData;
            if (dataOffset > resourceSize ||
                sizeof(IMAGE_RESOURCE_DATA_ENTRY) > resourceSize - dataOffset)
                continue;
            const auto* data = ImageAt<IMAGE_RESOURCE_DATA_ENTRY>(
                image, imageSize, resourceRva + dataOffset);
            if (!data || !data->Size || data->OffsetToData > imageSize ||
                data->Size > imageSize - data->OffsetToData)
                continue;
            const auto* bytes = image + data->OffsetToData;
            resources[static_cast<UINT>(nameEntry.Id)].assign(
                bytes, bytes + data->Size);
            break;
        }
    }
    return !resources.empty();
}
}

bool CacheEmbeddedResources() {
    std::lock_guard<std::mutex> lock(embeddedResourceMutex);
    if (embeddedResourcesCached) return !embeddedResources.empty();
    if (!moduleHandle) return false;
    std::unordered_map<UINT, std::vector<unsigned char>> resources;
    if (!CacheRcDataFromMappedImage(moduleHandle, resources)) return false;
    embeddedResources.swap(resources);
    embeddedResourcesCached = true;
    return true;
}

bool GetEmbeddedResource(UINT resourceId, const void*& bytes, DWORD& size) {
    bytes = nullptr;
    size = 0;
    std::lock_guard<std::mutex> lock(embeddedResourceMutex);
    const auto resource = embeddedResources.find(resourceId);
    if (resource == embeddedResources.end() || resource->second.empty())
        return false;
    bytes = resource->second.data();
    size = static_cast<DWORD>(resource->second.size());
    return true;
}

bool freeCam=false;
bool disableDrifterDarkness=false;
bool autoActiveReload=false;
bool bunnyHop=false;
bool freeCamActive=false;
bool movementDiagnostics=false;
std::atomic<unsigned long long> movementProcessCalls{0};
std::atomic<unsigned long long> movementCorrectionCalls{0};
std::atomic<unsigned long long> userCmdNetworkCalls{0};
std::atomic<float> movementDiagCameraYaw{0.0f};
std::atomic<float> movementDiagMovementYaw{0.0f};
std::atomic<float> movementDiagRawForward{0.0f};
std::atomic<float> movementDiagRawLeft{0.0f};
std::atomic<float> movementDiagResultForward{0.0f};
std::atomic<float> movementDiagResultLeft{0.0f};
std::atomic<float> movementDiagAfterForward{0.0f};
std::atomic<float> movementDiagAfterLeft{0.0f};
std::atomic<float> movementDiagAfterYaw{0.0f};
std::atomic<unsigned long long> wishDirectionCalls{0};
std::atomic<unsigned long long> wishDirectionCorrectionCalls{0};
std::mutex movementDebugTargetMutex; Vector3 movementDebugTarget{}; bool movementDebugTargetReady=false;
std::mutex movementDebugWishMutex; Vector3 movementDebugWishDirection{}; bool movementDebugWishReady=false;
std::wstring GetVirtualKeyDisplayNameW(int key) {
    if (key <= 0) return L"Not bound";
    switch (key) {
        case VK_LBUTTON: return L"LMB";
        case VK_RBUTTON: return L"RMB";
        case VK_MBUTTON: return L"MMB";
        case VK_XBUTTON1: return L"Mouse 4";
        case VK_XBUTTON2: return L"Mouse 5";
        case VK_SHIFT: return L"Shift";
        case VK_CONTROL: return L"Ctrl";
        case VK_MENU: return L"Alt";
        case VK_LWIN: return L"Left Windows";
        case VK_RWIN: return L"Right Windows";
    }

    UINT scanCode = MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC);
    switch (key) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT:
        case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_DIVIDE:
            scanCode |= 0x100;
            break;
    }
    wchar_t name[64]{};
    const LONG keyNameParam = static_cast<LONG>(scanCode << 16);
    if (scanCode && GetKeyNameTextW(keyNameParam, name,
                                    static_cast<int>(std::size(name))) > 0) {
        return name;
    }
    wchar_t fallback[24]{};
    std::swprintf(fallback, std::size(fallback), L"VK 0x%02X", key & 0xFF);
    return fallback;
}

std::string GetVirtualKeyDisplayName(int key) {
    const std::wstring wide = GetVirtualKeyDisplayNameW(key);
    if (wide.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                          static_cast<int>(wide.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return "Unknown";
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), result.data(), bytes,
                        nullptr, nullptr);
    return result;
}

volatile ULONGLONG lastSilentAttackAppliedAt = 0;
volatile LONG autoOrbAttackAppliedCount = 0;
uintptr_t clientBase=0; bool menuOpen=false,drawEsp=true,drawBoxes=true,drawHealth=true,drawHealthValues=true,drawNames=true,drawDistance=true,drawSnaplines=false,drawFovCircle=true,drawFarmFovCircle=false,drawBones=false,drawCreepEsp=false,farmAssist=false,autoLastHitOrbs=false,drawOrbEsp=false,drawSpectatorList=false,collisionDiagnostics=false,remSizedHull=false,glowEnabled=true,aimAssist=true,autoParry=true,imguiInitialized=false,consoleAttached=false; float aimFov=180.0f,farmFov=180.0f,aimSmooth=6.0f,fovCircleAlpha=110.0f,farmFovAlpha=110.0f,snaplineAlpha=180.0f; bool aimVisibilityCheck=true; Vector3 currentLocalPosition{}; std::atomic_bool currentLocalPositionReady=false; Vector3 currentCameraPosition{}; std::atomic_bool currentCameraPositionReady=false; std::atomic<uintptr_t> currentLocalPawn=0; std::atomic<uint32_t> currentLocalPawnHandle=0xFFFFFFFFu; std::mutex meleeObjectsMutex; std::vector<uintptr_t> meleeObjects; std::mutex silentAnglesMutex; Vector3 pendingSilentAngles{}; bool pendingSilentAnglesReady=false,pendingSilentAttack=false; std::mutex humanSilentMutex,creepSilentMutex,orbSilentMutex; Vector3 pendingHumanAngles{},pendingCreepAngles{},pendingOrbAngles{}; bool pendingHumanReady=false,pendingCreepReady=false,pendingOrbReady=false,pendingOrbAttack=false; std::mutex farmTargetsMutex; std::vector<FarmTarget> farmTargets; std::mutex orbTargetsMutex; std::vector<OrbTarget> orbTargets; std::mutex worldEspTargetsMutex; std::vector<WorldEspTarget> worldEspTargets;
ID3D11Texture2D* depthStaging=nullptr; UINT depthWidth=0,depthHeight=0; DXGI_FORMAT depthFormat=DXGI_FORMAT_UNKNOWN; bool depthSnapshotReady=false; int depthDiagnosticState=-1; Matrix4x4 currentViewMatrix{}; bool currentViewMatrixReady=false;
bool pendingOrbHoldAttack = false;
std::atomic_bool gameTextInputActive{false};
namespace {
std::atomic_bool gameConsoleInputActive{false};
std::atomic_bool gameChatInputActive{false};
std::atomic_bool consoleToggleHeld{false};
std::atomic_bool enterHeld{false};
std::atomic_bool escapeHeld{false};
std::atomic_bool gameCursorReleased{false};

void PublishGameTextInputState() {
    gameTextInputActive.store(
        gameConsoleInputActive.load(std::memory_order_acquire) ||
        gameChatInputActive.load(std::memory_order_acquire),
        std::memory_order_release);
}
}

bool AreCustomBindsSuppressed() {
    return menuOpen || gameTextInputActive.load(std::memory_order_acquire) ||
        gameCursorReleased.load(std::memory_order_acquire);
}

void UpdateGameTextInputKey(int key, bool down) {
    std::atomic_bool* held = nullptr;
    // Deadlock's shipped/current user_keys.vcfg binds toggleconsole to F7.
    // Keep OEM_3 as the standard Source fallback.
    if (key == VK_OEM_3 || key == VK_F7) held = &consoleToggleHeld;
    else if (key == VK_RETURN) held = &enterHeld;
    else if (key == VK_ESCAPE) held = &escapeHeld;
    else return;

    const bool wasDown = held->exchange(down, std::memory_order_acq_rel);
    if (!down || wasDown || menuOpen) return;

    if (key == VK_OEM_3 || key == VK_F7) {
        const bool active = !gameConsoleInputActive.load(
            std::memory_order_acquire);
        gameConsoleInputActive.store(active, std::memory_order_release);
        if (active)
            gameChatInputActive.store(false, std::memory_order_release);
    } else if (key == VK_RETURN) {
        // Enter executes a console command without closing the console. It
        // only opens/closes the ordinary game chat when the console is shut.
        if (!gameConsoleInputActive.load(std::memory_order_acquire)) {
            const bool active = !gameChatInputActive.load(
                std::memory_order_acquire);
            gameChatInputActive.store(active, std::memory_order_release);
        }
    } else {
        gameConsoleInputActive.store(false, std::memory_order_release);
        gameChatInputActive.store(false, std::memory_order_release);
    }
    PublishGameTextInputState();
}

void ResetGameTextInputState() {
    gameConsoleInputActive.store(false, std::memory_order_release);
    gameChatInputActive.store(false, std::memory_order_release);
    consoleToggleHeld.store(false, std::memory_order_release);
    enterHeld.store(false, std::memory_order_release);
    escapeHeld.store(false, std::memory_order_release);
    gameCursorReleased.store(false, std::memory_order_release);
    gameTextInputActive.store(false, std::memory_order_release);
}

void SetGameImeInputActive() {
    gameChatInputActive.store(true, std::memory_order_release);
    PublishGameTextInputState();
}

void NotifyGameCursorCapture(bool captured) {
    if (menuOpen) return;
    if (!captured) {
        gameCursorReleased.store(true, std::memory_order_release);
        return;
    }
    // Clicking the Panorama console's close button restores relative mouse
    // capture without sending the console toggle key or Escape.
    if (gameCursorReleased.exchange(false, std::memory_order_acq_rel)) {
        gameConsoleInputActive.store(false, std::memory_order_release);
        consoleToggleHeld.store(false, std::memory_order_release);
        PublishGameTextInputState();
    }
}
std::atomic<float> overlayProjectionWidth{0.0f};
std::atomic<float> overlayProjectionHeight{0.0f};
std::mutex visualFrameStateMutex;
ID3D11Device* pDevice=nullptr; ID3D11DeviceContext* pContext=nullptr; ID3D11RenderTargetView* pRenderTargetView=nullptr; HWND gameWindow=nullptr; WNDPROC oWndProc=nullptr; HMODULE moduleHandle=nullptr; HANDLE moduleInstanceGuard=nullptr,moduleReadyEvent=nullptr,manualMapInfoHandle=nullptr; bool manualMappedModule=false; void** presentVTable=nullptr; volatile LONG unloadRequested=0,unloadThreadStarted=0; std::mutex glowMutex,heroPawnsMutex; std::unordered_set<uintptr_t> registeredGlows,queuedGlows; EspStatus espStatus; std::unordered_map<uintptr_t,bool> combatVTables; std::vector<uintptr_t> heroVTables,heroPawns; HANDLE heroDiscoveryThread=nullptr,glowApplyThread=nullptr,farmTargetThread=nullptr,stopHeroDiscoveryEvent=nullptr; PresentFn oPresent=nullptr; ResizeBuffersFn oResizeBuffers=nullptr;
bool humanAimTargetFound = false;
bool aimSilentMode = false;
bool aimMixedMode = false;
bool aimNormalActive = false;
bool farmNormalActive = false;
bool aimSilentActive = false;
bool aimOnlyYaw = false;
bool aimLockTarget = false;
int aimLockKey = 0;
bool aimLockKeyCapture = false;
bool aimLockKeyLastDown = false;
uintptr_t aimLockCandidate = 0;
uintptr_t aimLockedTarget = 0;
bool aimPrediction = false;
bool antiFrog = false;
float antiFrogHsThreshold = 45.0f;
float aimHitchance = 100.0f;
float aimPitchSmooth = 6.0f;
float aimYawSmooth = 6.0f;
AimSelectionMode aimSelectionMode = AimSelectionMode::Crosshair;
int aimBonesMask = AimBoneHead | AimBoneNeck | AimBoneTorso;
int aimAssistKey = VK_RBUTTON;
bool aimKeyCapture = false;
int farmAssistKey = VK_XBUTTON1;
bool farmKeyCapture = false;
bool aimToggleMode = false;
bool aimToggleActive = false;
bool aimToggleLastDown = false;
bool farmToggleMode = false;
bool farmToggleActive = false;
bool farmToggleLastDown = false;
float farmAimSmooth = 6.0f;
bool farmSilentMode = false;
bool farmMixedMode = false;
bool autoLastHitOrbsAutoFire = true;
bool autoLastHitOrbsToggleMode = false;
bool autoLastHitOrbsKeyCapture = false;
bool autoLastHitOrbsActive = false;
int autoLastHitOrbsKey = VK_XBUTTON2;
bool orbAimVisibilityCheck = true;
int freeCamKey = VK_F6;
float freeCamSpeed = 450.0f;
bool freeCamKeyCapture = false;
bool orbEntityEventsAvailable = false;
bool drawTeammates = false;
bool drawPlayerNames = false;
bool cornerBoxes = true;
bool enemyEspEnabled = true, allyEspEnabled = false;
bool enemyAbilitiesEnabled = true, allyAbilitiesEnabled = true, creepAbilitiesEnabled = false;
bool enemyBoxesEnabled = true, allyBoxesEnabled = true;
bool enemyCornerBoxesEnabled = true, allyCornerBoxesEnabled = true;
bool enemyHealthEnabled = true, allyHealthEnabled = true;
bool enemyHealthValuesEnabled = true, allyHealthValuesEnabled = true;
bool enemyNamesEnabled = true, allyNamesEnabled = true;
bool enemyPlayerNamesEnabled = true, allyPlayerNamesEnabled = true;
bool enemyDistanceEnabled = true, allyDistanceEnabled = true;
bool enemySnaplinesEnabled = true, allySnaplinesEnabled = true;
bool enemyBonesEnabled = true, allyBonesEnabled = true;
float menuAccentColor[4] = {0.15f, 0.62f, 1.00f, 1.00f};
float enemyBoxColor[4] = {0.20f, 1.00f, 0.10f, 1.00f};
float teammateBoxColor[4] = {0.20f, 0.60f, 1.00f, 1.00f};
float enemyPlayerNameColor[4] = {0.25f, 0.85f, 1.00f, 1.00f};
float teammatePlayerNameColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float enemyHealthBarColor[4] = {0.20f, 1.00f, 0.25f, 1.00f};
float teammateHealthBarColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float enemyHealthValueColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateHealthValueColor[4] = {0.70f, 0.85f, 1.00f, 1.00f};
float enemyGlowColor[4] = {0.10f, 1.00f, 0.18f, 1.00f};
float teammateGlowColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
bool enemyGlowEnabled = true, allyGlowEnabled = false;
bool enemyRadarEnabled = false;
int enemyGlowMode = 0, allyGlowMode = 0;
bool enemyChamsEnabled = false, allyChamsEnabled = false;
float enemyChamsColor[4] = {1.00f, 0.35f, 0.75f, 1.00f};
float allyChamsColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float enemyNameColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateNameColor[4] = {0.35f, 0.75f, 1.00f, 1.00f};
float enemySkeletonColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float teammateSkeletonColor[4] = {0.35f, 0.75f, 1.00f, 1.00f};
float enemyHealthColor[4] = {0.20f, 1.00f, 0.25f, 1.00f};
float teammateHealthColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float boxThickness = 1.20f;
float cornerBoxLength = 0.24f;
bool creepEspEnabled = false, neutralCreepEspEnabled = false,
     creepBoxesEnabled = true, creepCornerBoxesEnabled = false;
bool creepHealthEnabled = true, creepHealthValuesEnabled = true, creepDistanceEnabled = true;
float creepBoxColor[4] = {1.00f, 0.67f, 0.05f, 1.00f};
float creepHealthColor[4] = {0.25f, 0.90f, 0.35f, 1.00f};
float creepHealthValueColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
bool allyCreepEspEnabled = false, allyCreepBoxesEnabled = true, allyCreepCornerBoxesEnabled = false;
bool allyCreepHealthEnabled = true, allyCreepHealthValuesEnabled = true, allyCreepDistanceEnabled = true;
float allyCreepBoxColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float allyCreepHealthColor[4] = {0.25f, 0.65f, 1.00f, 1.00f};
float allyCreepHealthValueColor[4] = {0.70f, 0.85f, 1.00f, 1.00f};
float enemyEspMaxDistance = 150.0f, allyEspMaxDistance = 150.0f,
      creepEspMaxDistance = 80.0f, orbEspMaxDistance = 80.0f;
bool powerupEspEnabled = false;
bool enemyTrooperChams = false, allyTrooperChams = false,
     neutralChams = false;
float enemyTrooperChamsColor[4] = {1.00f, 0.22f, 0.12f, 1.00f};
float allyTrooperChamsColor[4] = {0.20f, 0.60f, 1.00f, 1.00f};
float neutralChamsColor[4] = {1.00f, 0.72f, 0.12f, 1.00f};
bool fovChangerEnabled = false, overrideScopeFov = false;
float cameraFov = 90.0f, scopedCameraFov = 90.0f;
bool worldModulationEnabled = false, disableSkybox = false;
float skyboxColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float skyboxBrightness = 1.0f;
float propsColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float lightColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
float lightBrightness = 1.0f;
float worldColor[4] = {1.00f, 1.00f, 1.00f, 1.00f};
bool talonEspEnabled = false, campTimersEnabled = false,
     campTimersOnScreen = true, campTimersOnMinimap = true;
float talonEspColor[4] = {0.95f, 0.35f, 0.12f, 1.00f};
float campTimerColor[4] = {1.00f, 0.82f, 0.20f, 1.00f};
float campTimerMinimapSize = 15.0f;
std::mutex campTimersMutex; std::vector<CampTimerData> campTimers;
namespace {
std::string configPathOverride;
std::mutex configFileMutex;
std::unordered_map<std::string, std::string> savedConfigContents;

std::string ConfigPath() {
    if (!configPathOverride.empty()) return configPathOverride;
    // A manual-mapped image has no loader path. LOCALAPPDATA is available for
    // both LoadLibrary and manual-map injection and remains writable when the
    // game itself is installed below Program Files.
    return Dll6Paths::DataFileA("menu.ini");
}

std::wstring NormalizeProfileName(std::wstring name) {
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (name.size() > 48) name.resize(48);
    if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".ini") == 0)
        name.resize(name.size() - 4);
    while (!name.empty() && (iswspace(name.back()) || name.back() == L'.'))
        name.pop_back();
    if (name.empty() || name == L"." || name == L"..")
        return {};
    for (wchar_t character : name) {
        if (character < 32 || wcschr(L"<>:\"/\\|?*", character)) return {};
    }
    return name;
}

std::string WideToAnsi(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_ACP, 0, value.c_str(),
                                            static_cast<int>(value.size()),
                                            nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, value.c_str(), static_cast<int>(value.size()),
                        result.data(), length, nullptr, nullptr);
    return result;
}

std::string ProfilePath(const std::wstring& rawName) {
    const std::wstring name = NormalizeProfileName(rawName);
    if (name.empty()) return {};
    const std::string ansiName = WideToAnsi(name);
    if (ansiName.empty()) return {};
    return Dll6Paths::DataFileA((ansiName + ".ini").c_str());
}

bool TryMigrateLegacyConfig(const std::string& destination) {
    auto tryCopy = [&destination](const std::string& source) {
        if (source.empty() || _stricmp(source.c_str(), destination.c_str()) == 0)
            return false;
        const DWORD attributes = GetFileAttributesA(source.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
        return CopyFileA(source.c_str(), destination.c_str(), TRUE) != FALSE;
    };

    // Preserve settings created by builds that used %LOCALAPPDATA%\Dll6.
    char localAppData[32768]{};
    const DWORD localLength = GetEnvironmentVariableA(
        "LOCALAPPDATA", localAppData,
        static_cast<DWORD>(std::size(localAppData)));
    if (localLength && localLength < std::size(localAppData)) {
        std::string oldConfig(localAppData, localLength);
        while (!oldConfig.empty() &&
               (oldConfig.back() == '\\' || oldConfig.back() == '/'))
            oldConfig.pop_back();
        oldConfig += "\\Dll6\\Dll6.ini";
        if (tryCopy(oldConfig)) return true;
    }

    // LoadLibrary builds historically kept Dll6.ini beside the DLL.
    char modulePath[32768]{};
    if (moduleHandle) {
        const DWORD length = GetModuleFileNameA(
            moduleHandle, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (length && length < std::size(modulePath)) {
            std::string sibling(modulePath, length);
            const size_t separator = sibling.find_last_of("\\/");
            if (separator != std::string::npos) {
                sibling.resize(separator + 1);
                sibling += "Dll6.ini";
                if (tryCopy(sibling)) return true;
            }
        }
    }

    // Compatibility with the stable fallback used by older manual-map builds.
    return tryCopy(
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\Dll6.ini");
}

void ApplyPendingConfigRestore(const std::string& destination) {
    const std::string pending = Dll6Paths::DataFileA("menu.restore.ini");
    const DWORD attributes = GetFileAttributesA(pending.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return;

    // Keep the displaced file once for recovery, then atomically consume the
    // prepared restore on the next injection. This also avoids fighting an
    // older injected build that may still be autosaving while the game runs.
    const std::string backup = Dll6Paths::DataFileA("menu.before_restore.ini");
    CopyFileA(destination.c_str(), backup.c_str(), TRUE);
    MoveFileExA(pending.c_str(), destination.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool ExtractBundledDefaultConfig(const std::string& path) {
    const void* bytes = nullptr;
    DWORD size = 0;
    if (!GetEmbeddedResource(IDR_DEFAULT_CONFIG, bytes, size)) return false;

    const HANDLE file = CreateFileA(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(file, bytes, size, &written, nullptr) &&
                         written == size;
    CloseHandle(file);
    if (!success) DeleteFileA(path.c_str());
    return success;
}
}

void LoadConfig() {
    const std::string path = ConfigPath();
    const bool loadingProfile = !configPathOverride.empty();
    if (!loadingProfile) ApplyPendingConfigRestore(path);
    std::ifstream input(path);
    if (!input) {
        if (loadingProfile) return;
        input.close();
        input.clear();
        if (TryMigrateLegacyConfig(path) || ExtractBundledDefaultConfig(path))
            input.open(path);
        if (!input) {
            // Resource extraction can fail only when the target directory is
            // not writable. Preserve the compiled defaults in that case.
            SaveConfig();
            return;
        }
    }
    std::string key;
    double number = 0.0;
    bool loadedAimBonesMask = false;
    while (input >> key >> number) {
        const bool value = number != 0.0;
        if (key == "previewEnemyHero")
            SetPanoramaPreviewHeroForRole(0, static_cast<int>(number));
        else if (key == "previewAllyHero")
            SetPanoramaPreviewHeroForRole(1, static_cast<int>(number));
        else if (key == "previewCreepHero")
            SetPanoramaPreviewHeroForRole(2, static_cast<int>(number));
        if (key == "drawEsp") drawEsp = value;
        if (key == "enemyEspEnabled") enemyEspEnabled = value;
        else if (key == "allyEspEnabled") allyEspEnabled = value;
        else if (key == "enemyAbilitiesEnabled") enemyAbilitiesEnabled = value;
        else if (key == "allyAbilitiesEnabled") allyAbilitiesEnabled = value;
        else if (key == "creepAbilitiesEnabled") creepAbilitiesEnabled = value;
        else if (key == "enemyBoxesEnabled") enemyBoxesEnabled = value;
        else if (key == "allyBoxesEnabled") allyBoxesEnabled = value;
        else if (key == "enemyCornerBoxesEnabled") enemyCornerBoxesEnabled = value;
        else if (key == "allyCornerBoxesEnabled") allyCornerBoxesEnabled = value;
        if (key == "enemyHealthEnabled") enemyHealthEnabled = value;
        else if (key == "allyHealthEnabled") allyHealthEnabled = value;
        else if (key == "enemyHealthValuesEnabled") enemyHealthValuesEnabled = value;
        else if (key == "allyHealthValuesEnabled") allyHealthValuesEnabled = value;
        if (key == "enemyNamesEnabled") enemyNamesEnabled = value;
        else if (key == "allyNamesEnabled") allyNamesEnabled = value;
        else if (key == "enemyPlayerNamesEnabled") enemyPlayerNamesEnabled = value;
        else if (key == "allyPlayerNamesEnabled") allyPlayerNamesEnabled = value;
        if (key == "enemyDistanceEnabled") enemyDistanceEnabled = value;
        else if (key == "allyDistanceEnabled") allyDistanceEnabled = value;
        else if (key == "enemySnaplinesEnabled") enemySnaplinesEnabled = value;
        else if (key == "allySnaplinesEnabled") allySnaplinesEnabled = value;
        else if (key == "enemyBonesEnabled") enemyBonesEnabled = value;
        else if (key == "allyBonesEnabled") allyBonesEnabled = value;
        if (key == "drawBoxes") drawBoxes = value;
        else if (key == "drawHealth") drawHealth = value;
        else if (key == "drawHealthValues") drawHealthValues = value;
        else if (key == "drawNames") drawNames = value;
        else if (key == "drawDistance") drawDistance = value;
        else if (key == "drawSnaplines") drawSnaplines = value;
        else if (key == "drawFovCircle") drawFovCircle = value;
        else if (key == "drawFarmFovCircle") drawFarmFovCircle = value;
        else if (key == "drawBones") drawBones = value;
        else if (key == "drawCreepEsp") drawCreepEsp = value;
        else if (key == "creepEspEnabled") creepEspEnabled = value;
        else if (key == "neutralCreepEspEnabled") neutralCreepEspEnabled = value;
        else if (key == "creepBoxesEnabled") creepBoxesEnabled = value;
        else if (key == "creepCornerBoxesEnabled") creepCornerBoxesEnabled = value;
        else if (key == "creepHealthEnabled") creepHealthEnabled = value;
        else if (key == "creepHealthValuesEnabled") creepHealthValuesEnabled = value;
        else if (key == "creepDistanceEnabled") creepDistanceEnabled = value;
        else if (key == "allyCreepEspEnabled") allyCreepEspEnabled = value;
        else if (key == "allyCreepBoxesEnabled") allyCreepBoxesEnabled = value;
        else if (key == "allyCreepCornerBoxesEnabled") allyCreepCornerBoxesEnabled = value;
        else if (key == "allyCreepHealthEnabled") allyCreepHealthEnabled = value;
        else if (key == "allyCreepHealthValuesEnabled") allyCreepHealthValuesEnabled = value;
        else if (key == "allyCreepDistanceEnabled") allyCreepDistanceEnabled = value;
        else if (key == "enemyEspMaxDistance") enemyEspMaxDistance = static_cast<float>(number);
        else if (key == "allyEspMaxDistance") allyEspMaxDistance = static_cast<float>(number);
        else if (key == "creepEspMaxDistance") creepEspMaxDistance = static_cast<float>(number);
        else if (key == "orbEspMaxDistance") orbEspMaxDistance = static_cast<float>(number);
        else if (key == "powerupEspEnabled") powerupEspEnabled = value;
        else if (key == "enemyTrooperChams") enemyTrooperChams = value;
        else if (key == "allyTrooperChams") allyTrooperChams = value;
        else if (key == "neutralChams") neutralChams = value;
        else if (key == "fovChangerEnabled") fovChangerEnabled = value;
        else if (key == "overrideScopeFov") overrideScopeFov = value;
        else if (key == "cameraFov") cameraFov = static_cast<float>(number);
        else if (key == "scopedCameraFov") scopedCameraFov = static_cast<float>(number);
        else if (key == "worldModulationEnabled") worldModulationEnabled = value;
        else if (key == "disableSkybox") disableSkybox = value;
        else if (key == "skyboxBrightness") skyboxBrightness = static_cast<float>(number);
        else if (key == "lightBrightness") lightBrightness = static_cast<float>(number);
        else if (key == "talonEspEnabled") talonEspEnabled = value;
        else if (key == "campTimersEnabled") campTimersEnabled = value;
        else if (key == "campTimersOnScreen") campTimersOnScreen = value;
        else if (key == "campTimersOnMinimap") campTimersOnMinimap = value;
        else if (key == "campTimerMinimapSize")
            campTimerMinimapSize = std::clamp(static_cast<float>(number),
                                              15.0f, 30.0f);
        else if (key == "drawTeammates") drawTeammates = value;
        else if (key == "drawPlayerNames") drawPlayerNames = value;
        else if (key == "cornerBoxes") cornerBoxes = value;
        else if (key == "boxThickness") boxThickness = static_cast<float>(number);
        else if (key == "cornerBoxLength") cornerBoxLength = static_cast<float>(number);
        else if (key == "menuAccentR") menuAccentColor[0] = std::clamp(static_cast<float>(number), 0.0f, 1.0f);
        else if (key == "menuAccentG") menuAccentColor[1] = std::clamp(static_cast<float>(number), 0.0f, 1.0f);
        else if (key == "menuAccentB") menuAccentColor[2] = std::clamp(static_cast<float>(number), 0.0f, 1.0f);
        else if (key == "menuAccentA") menuAccentColor[3] = std::clamp(static_cast<float>(number), 0.0f, 1.0f);
        else if (key == "enemyBoxR") enemyBoxColor[0] = static_cast<float>(number);
        else if (key == "enemyBoxG") enemyBoxColor[1] = static_cast<float>(number);
        else if (key == "enemyBoxB") enemyBoxColor[2] = static_cast<float>(number);
        else if (key == "enemyBoxA") enemyBoxColor[3] = static_cast<float>(number);
        else if (key == "teammateBoxR") teammateBoxColor[0] = static_cast<float>(number);
        else if (key == "teammateBoxG") teammateBoxColor[1] = static_cast<float>(number);
        else if (key == "teammateBoxB") teammateBoxColor[2] = static_cast<float>(number);
        else if (key == "teammateBoxA") teammateBoxColor[3] = static_cast<float>(number);
        else if (key == "enemyNameR") enemyNameColor[0] = static_cast<float>(number);
        else if (key == "enemyNameG") enemyNameColor[1] = static_cast<float>(number);
        else if (key == "enemyNameB") enemyNameColor[2] = static_cast<float>(number);
        else if (key == "enemyNameA") enemyNameColor[3] = static_cast<float>(number);
        else if (key == "teammateNameR") teammateNameColor[0] = static_cast<float>(number);
        else if (key == "teammateNameG") teammateNameColor[1] = static_cast<float>(number);
        else if (key == "teammateNameB") teammateNameColor[2] = static_cast<float>(number);
        else if (key == "teammateNameA") teammateNameColor[3] = static_cast<float>(number);
        if (key == "enemySkeletonR") enemySkeletonColor[0] = static_cast<float>(number);
        else if (key == "enemySkeletonG") enemySkeletonColor[1] = static_cast<float>(number);
        else if (key == "enemySkeletonB") enemySkeletonColor[2] = static_cast<float>(number);
        else if (key == "enemySkeletonA") enemySkeletonColor[3] = static_cast<float>(number);
        else if (key == "teammateSkeletonR") teammateSkeletonColor[0] = static_cast<float>(number);
        else if (key == "teammateSkeletonG") teammateSkeletonColor[1] = static_cast<float>(number);
        else if (key == "teammateSkeletonB") teammateSkeletonColor[2] = static_cast<float>(number);
        else if (key == "teammateSkeletonA") teammateSkeletonColor[3] = static_cast<float>(number);
        if (key == "enemyPlayerNameR") enemyPlayerNameColor[0] = static_cast<float>(number);
        else if (key == "enemyPlayerNameG") enemyPlayerNameColor[1] = static_cast<float>(number);
        else if (key == "enemyPlayerNameB") enemyPlayerNameColor[2] = static_cast<float>(number);
        else if (key == "enemyPlayerNameA") enemyPlayerNameColor[3] = static_cast<float>(number);
        else if (key == "teammatePlayerNameR") teammatePlayerNameColor[0] = static_cast<float>(number);
        else if (key == "teammatePlayerNameG") teammatePlayerNameColor[1] = static_cast<float>(number);
        else if (key == "teammatePlayerNameB") teammatePlayerNameColor[2] = static_cast<float>(number);
        else if (key == "teammatePlayerNameA") teammatePlayerNameColor[3] = static_cast<float>(number);
        if (key == "enemyHealthBarR") enemyHealthBarColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthBarG") enemyHealthBarColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthBarB") enemyHealthBarColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthBarA") enemyHealthBarColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthBarR") teammateHealthBarColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthBarG") teammateHealthBarColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthBarB") teammateHealthBarColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthBarA") teammateHealthBarColor[3] = static_cast<float>(number);
        else if (key == "enemyHealthValueR") enemyHealthValueColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthValueG") enemyHealthValueColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthValueB") enemyHealthValueColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthValueA") enemyHealthValueColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthValueR") teammateHealthValueColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthValueG") teammateHealthValueColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthValueB") teammateHealthValueColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthValueA") teammateHealthValueColor[3] = static_cast<float>(number);
        if (key == "enemyHealthR") enemyHealthColor[0] = static_cast<float>(number);
        else if (key == "enemyHealthG") enemyHealthColor[1] = static_cast<float>(number);
        else if (key == "enemyHealthB") enemyHealthColor[2] = static_cast<float>(number);
        else if (key == "enemyHealthA") enemyHealthColor[3] = static_cast<float>(number);
        else if (key == "teammateHealthR") teammateHealthColor[0] = static_cast<float>(number);
        else if (key == "teammateHealthG") teammateHealthColor[1] = static_cast<float>(number);
        else if (key == "teammateHealthB") teammateHealthColor[2] = static_cast<float>(number);
        else if (key == "teammateHealthA") teammateHealthColor[3] = static_cast<float>(number);
        if (key == "enemyGlowR") enemyGlowColor[0] = static_cast<float>(number);
        else if (key == "enemyGlowG") enemyGlowColor[1] = static_cast<float>(number);
        else if (key == "enemyGlowB") enemyGlowColor[2] = static_cast<float>(number);
        else if (key == "enemyGlowA") enemyGlowColor[3] = static_cast<float>(number);
        else if (key == "teammateGlowR") teammateGlowColor[0] = static_cast<float>(number);
        else if (key == "teammateGlowG") teammateGlowColor[1] = static_cast<float>(number);
        else if (key == "teammateGlowB") teammateGlowColor[2] = static_cast<float>(number);
        else if (key == "teammateGlowA") teammateGlowColor[3] = static_cast<float>(number);
        else if (key == "enemyGlowEnabled") enemyGlowEnabled = value;
        else if (key == "allyGlowEnabled") allyGlowEnabled = value;
        else if (key == "enemyRadarEnabled") enemyRadarEnabled = value;
        else if (key == "enemyChamsEnabled") enemyChamsEnabled = value;
        else if (key == "allyChamsEnabled") allyChamsEnabled = value;
        else if (key == "enemyChamsR") enemyChamsColor[0] = static_cast<float>(number);
        else if (key == "enemyChamsG") enemyChamsColor[1] = static_cast<float>(number);
        else if (key == "enemyChamsB") enemyChamsColor[2] = static_cast<float>(number);
        else if (key == "enemyChamsA") enemyChamsColor[3] = static_cast<float>(number);
        else if (key == "allyChamsR") allyChamsColor[0] = static_cast<float>(number);
        else if (key == "allyChamsG") allyChamsColor[1] = static_cast<float>(number);
        else if (key == "allyChamsB") allyChamsColor[2] = static_cast<float>(number);
        else if (key == "allyChamsA") allyChamsColor[3] = static_cast<float>(number);
        if (key == "creepBoxR") creepBoxColor[0] = static_cast<float>(number);
        else if (key == "creepBoxG") creepBoxColor[1] = static_cast<float>(number);
        else if (key == "creepBoxB") creepBoxColor[2] = static_cast<float>(number);
        else if (key == "creepBoxA") creepBoxColor[3] = static_cast<float>(number);
        else if (key == "creepHealthR") creepHealthColor[0] = static_cast<float>(number);
        else if (key == "creepHealthG") creepHealthColor[1] = static_cast<float>(number);
        else if (key == "creepHealthB") creepHealthColor[2] = static_cast<float>(number);
        else if (key == "creepHealthA") creepHealthColor[3] = static_cast<float>(number);
        else if (key == "creepHealthValueR") creepHealthValueColor[0] = static_cast<float>(number);
        else if (key == "creepHealthValueG") creepHealthValueColor[1] = static_cast<float>(number);
        else if (key == "creepHealthValueB") creepHealthValueColor[2] = static_cast<float>(number);
        else if (key == "creepHealthValueA") creepHealthValueColor[3] = static_cast<float>(number);
        else if (key == "allyCreepBoxR") allyCreepBoxColor[0] = static_cast<float>(number);
        else if (key == "allyCreepBoxG") allyCreepBoxColor[1] = static_cast<float>(number);
        else if (key == "allyCreepBoxB") allyCreepBoxColor[2] = static_cast<float>(number);
        else if (key == "allyCreepBoxA") allyCreepBoxColor[3] = static_cast<float>(number);
        else if (key == "allyCreepHealthR") allyCreepHealthColor[0] = static_cast<float>(number);
        else if (key == "allyCreepHealthG") allyCreepHealthColor[1] = static_cast<float>(number);
        else if (key == "allyCreepHealthB") allyCreepHealthColor[2] = static_cast<float>(number);
        else if (key == "allyCreepHealthA") allyCreepHealthColor[3] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueR") allyCreepHealthValueColor[0] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueG") allyCreepHealthValueColor[1] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueB") allyCreepHealthValueColor[2] = static_cast<float>(number);
        else if (key == "allyCreepHealthValueA") allyCreepHealthValueColor[3] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsR") enemyTrooperChamsColor[0] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsG") enemyTrooperChamsColor[1] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsB") enemyTrooperChamsColor[2] = static_cast<float>(number);
        else if (key == "enemyTrooperChamsA") enemyTrooperChamsColor[3] = static_cast<float>(number);
        else if (key == "allyTrooperChamsR") allyTrooperChamsColor[0] = static_cast<float>(number);
        else if (key == "allyTrooperChamsG") allyTrooperChamsColor[1] = static_cast<float>(number);
        else if (key == "allyTrooperChamsB") allyTrooperChamsColor[2] = static_cast<float>(number);
        else if (key == "allyTrooperChamsA") allyTrooperChamsColor[3] = static_cast<float>(number);
        else if (key == "neutralChamsR") neutralChamsColor[0] = static_cast<float>(number);
        else if (key == "neutralChamsG") neutralChamsColor[1] = static_cast<float>(number);
        else if (key == "neutralChamsB") neutralChamsColor[2] = static_cast<float>(number);
        else if (key == "neutralChamsA") neutralChamsColor[3] = static_cast<float>(number);
        else if (key == "skyboxColorR") skyboxColor[0] = static_cast<float>(number);
        else if (key == "skyboxColorG") skyboxColor[1] = static_cast<float>(number);
        else if (key == "skyboxColorB") skyboxColor[2] = static_cast<float>(number);
        else if (key == "skyboxColorA") skyboxColor[3] = static_cast<float>(number);
        else if (key == "propsColorR") propsColor[0] = static_cast<float>(number);
        else if (key == "propsColorG") propsColor[1] = static_cast<float>(number);
        else if (key == "propsColorB") propsColor[2] = static_cast<float>(number);
        else if (key == "propsColorA") propsColor[3] = static_cast<float>(number);
        else if (key == "lightColorR") lightColor[0] = static_cast<float>(number);
        else if (key == "lightColorG") lightColor[1] = static_cast<float>(number);
        else if (key == "lightColorB") lightColor[2] = static_cast<float>(number);
        else if (key == "lightColorA") lightColor[3] = static_cast<float>(number);
        else if (key == "worldColorR") worldColor[0] = static_cast<float>(number);
        else if (key == "worldColorG") worldColor[1] = static_cast<float>(number);
        else if (key == "worldColorB") worldColor[2] = static_cast<float>(number);
        else if (key == "worldColorA") worldColor[3] = static_cast<float>(number);
        else if (key == "talonEspColorR") talonEspColor[0] = static_cast<float>(number);
        else if (key == "talonEspColorG") talonEspColor[1] = static_cast<float>(number);
        else if (key == "talonEspColorB") talonEspColor[2] = static_cast<float>(number);
        else if (key == "talonEspColorA") talonEspColor[3] = static_cast<float>(number);
        else if (key == "campTimerColorR") campTimerColor[0] = static_cast<float>(number);
        else if (key == "campTimerColorG") campTimerColor[1] = static_cast<float>(number);
        else if (key == "campTimerColorB") campTimerColor[2] = static_cast<float>(number);
        else if (key == "campTimerColorA") campTimerColor[3] = static_cast<float>(number);
        else if (key == "enemyGlowMode") enemyGlowMode = static_cast<int>(number);
        else if (key == "allyGlowMode") allyGlowMode = static_cast<int>(number);
        // Compatibility with older configs that had one shared mode.
        else if (key == "glowMode") {
            enemyGlowMode = static_cast<int>(number);
            allyGlowMode = static_cast<int>(number);
        }
        if (key == "drawOrbEsp") drawOrbEsp = value;
        else if (key == "drawSpectatorList") drawSpectatorList = value;
        if (key == "freeCam") freeCam = value;
        else if (key == "disableDrifterDarkness") disableDrifterDarkness = value;
        else if (key == "autoActiveReload") autoActiveReload = value;
        else if (key == "bunnyHop") bunnyHop = value;
        else if (key == "freeCamKey") freeCamKey = static_cast<int>(number);
        else if (key == "freeCamSpeed") freeCamSpeed = static_cast<float>(number);
        else if (key == "movementProbeEnabled") movementProbeEnabled = value;
        else if (key == "movementReplayEnabled") movementReplayEnabled = value;
        else if (key == "movementReplayKey") movementReplayKey = static_cast<int>(number);
        if (key == "farmAssist") farmAssist = value;
        if (key == "autoLastHitOrbs") autoLastHitOrbs = value;
        else if (key == "autoLastHitOrbsAutoFire") autoLastHitOrbsAutoFire = value;
        else if (key == "autoLastHitOrbsToggleMode") autoLastHitOrbsToggleMode = value;
        else if (key == "autoLastHitOrbsKey") autoLastHitOrbsKey = static_cast<int>(number);
        else if (key == "orbAimVisibilityCheck") orbAimVisibilityCheck = value;
        if (key == "glowEnabled") glowEnabled = value;
        if (key == "aimAssist") aimAssist = value;
        else if (key == "autoParry") autoParry = value;
        else if (key == "aimSilentMode") aimSilentMode = value;
        else if (key == "aimMixedMode") aimMixedMode = value;
        else if (key == "aimOnlyYaw") aimOnlyYaw = value;
        else if (key == "aimLockTarget") aimLockTarget = value;
        else if (key == "aimLockKey") aimLockKey = static_cast<int>(number);
        else if (key == "aimPrediction") aimPrediction = value;
        else if (key == "antiFrog") antiFrog = value;
        else if (key == "antiFrogHsThreshold")
            antiFrogHsThreshold = std::clamp(static_cast<float>(number), 1.0f, 99.0f);
        else if (key == "aimVisibilityCheck") aimVisibilityCheck = value;
        else if (key == "aimToggleMode") aimToggleMode = value;
        if (key == "farmToggleMode") farmToggleMode = value;
        else if (key == "farmSilentMode") farmSilentMode = value;
        else if (key == "farmMixedMode") farmMixedMode = value;
        if (key == "aimAssistKey") aimAssistKey = static_cast<int>(number);
        else if (key == "farmAssistKey") farmAssistKey = static_cast<int>(number);
        if (key == "aimFov") aimFov = static_cast<float>(number);
        else if (key == "farmFov") farmFov = static_cast<float>(number);
        else if (key == "aimSmooth") aimSmooth = static_cast<float>(number);
        else if (key == "aimHitchance") aimHitchance = static_cast<float>(number);
        else if (key == "aimPitchSmooth") aimPitchSmooth = static_cast<float>(number);
        else if (key == "aimYawSmooth") aimYawSmooth = static_cast<float>(number);
        else if (key == "farmAimSmooth") farmAimSmooth = static_cast<float>(number);
        else if (key == "fovCircleAlpha") fovCircleAlpha = static_cast<float>(number);
        else if (key == "farmFovAlpha") farmFovAlpha = static_cast<float>(number);
        else if (key == "snaplineAlpha") snaplineAlpha = static_cast<float>(number);
        else if (key == "aimBonesMask") {
            aimBonesMask = std::clamp(static_cast<int>(number), 1, AimBoneAll);
            loadedAimBonesMask = true;
        } else if (key == "aimTargetMode" && !loadedAimBonesMask) {
            // One-time migration: Head, Body, Closest from older configs.
            const int oldMode = static_cast<int>(number);
            aimBonesMask = oldMode == 0 ? AimBoneHead
                : oldMode == 1 ? AimBoneTorso
                : AimBoneHead | AimBoneTorso;
        }
        else if (key == "aimSelectionMode") aimSelectionMode = static_cast<AimSelectionMode>(static_cast<int>(number));
        else if (key == "vindictaAutoSnipeEnabled") vindictaAutoSnipeEnabled = value;
        else if (key == "hazeSleepDaggerEnabled") hazeSleepDaggerEnabled = value;
        else if (key == "shivSerratedKnivesEnabled") shivSerratedKnivesEnabled = value;
        else if (key == "bebopAbility3Enabled") bebopAbility3Enabled = value;
        else if (key == "bebopAbility2AutoEnabled") bebopAbility2AutoEnabled = value;
        else if (key == "drifterAbility2Enabled") drifterAbility2Enabled = value;
        else if (key == "heroScriptsShowFov") heroScriptsShowFov = value;
        else if (key == "hazePredictionDot") hazePredictionDot = value;
        else if (key == "vindictaSnipeFov") vindictaSnipeFov = std::clamp(static_cast<float>(number), 10.0f, 500.0f);
        else if (key == "vindictaSnipeSmoothX") vindictaSnipeSmoothX = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "vindictaSnipeSmoothY") vindictaSnipeSmoothY = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "hazeDaggerFov") hazeDaggerFov = std::clamp(static_cast<float>(number), 10.0f, 500.0f);
        else if (key == "hazeDaggerSmoothX") hazeDaggerSmoothX = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "hazeDaggerSmoothY") hazeDaggerSmoothY = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "shivKnivesFov") shivKnivesFov = std::clamp(static_cast<float>(number), 10.0f, 500.0f);
        else if (key == "shivKnivesSmoothX") shivKnivesSmoothX = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "shivKnivesSmoothY") shivKnivesSmoothY = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "bebopAbility3Fov") bebopAbility3Fov = std::clamp(static_cast<float>(number), 10.0f, 500.0f);
        else if (key == "bebopAbility3SmoothX") bebopAbility3SmoothX = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "bebopAbility3SmoothY") bebopAbility3SmoothY = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "drifterAbility2Fov") drifterAbility2Fov = std::clamp(static_cast<float>(number), 10.0f, 500.0f);
        else if (key == "drifterAbility2SmoothX") drifterAbility2SmoothX = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
        else if (key == "drifterAbility2SmoothY") drifterAbility2SmoothY = std::clamp(static_cast<float>(number), 1.0f, 30.0f);
    }
    // The aim mode is a three-way choice, even though compatibility with old
    // configs stores it as two booleans. Mixed wins if an old build saved an
    // invalid state with both flags enabled.
    if (aimMixedMode)
        aimSilentMode = false;
}

std::vector<std::wstring> GetConfigProfiles() {
    std::vector<std::wstring> profiles;
    const auto appendDirectory = [&profiles](const std::wstring& directory) {
        WIN32_FIND_DATAW data{};
        HANDLE search = FindFirstFileW((directory + L"\\*.ini").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = data.cFileName;
            if (name.size() >= 4 &&
                _wcsicmp(name.c_str() + name.size() - 4, L".ini") == 0)
                name.resize(name.size() - 4);
            if (name.empty() || _wcsicmp(name.c_str(), L"menu.restore") == 0 ||
                _wcsicmp(name.c_str(), L"menu.before_restore") == 0 ||
                _wcsicmp(name.c_str(), L"menu") == 0)
                continue;
            const bool duplicate = std::any_of(
                profiles.begin(), profiles.end(), [&name](const std::wstring& existing) {
                    return _wcsicmp(existing.c_str(), name.c_str()) == 0;
                });
            if (!duplicate) profiles.push_back(std::move(name));
        } while (FindNextFileW(search, &data));
        FindClose(search);
    };
    appendDirectory(Dll6Paths::DataDirectoryW());
    // Profiles made by the immediately preceding build lived in this
    // subdirectory. Keep them visible and loadable during migration.
    appendDirectory(Dll6Paths::DataFileW(L"configs"));
    std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });
    return profiles;
}

bool SaveConfigProfile(const std::wstring& name) {
    const std::string path = ProfilePath(name);
    if (path.empty()) return false;
    configPathOverride = path;
    SaveConfig();
    configPathOverride.clear();
    const DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool LoadConfigProfile(const std::wstring& name) {
    std::string path = ProfilePath(name);
    if (path.empty()) return false;
    DWORD attributes = GetFileAttributesA(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const std::wstring normalized = NormalizeProfileName(name);
        const std::string ansiName = WideToAnsi(normalized);
        if (!ansiName.empty()) {
            path = Dll6Paths::DataFileA("configs") + "\\" + ansiName + ".ini";
            attributes = GetFileAttributesA(path.c_str());
        }
    }
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    configPathOverride = path;
    LoadConfig();
    configPathOverride.clear();
    return true;
}

void SaveConfig() {
    std::lock_guard<std::mutex> lock(configFileMutex);
    const std::string path = ConfigPath();
    std::ostringstream output;
    output << "drawEsp " << drawEsp << '\n'
           << "previewEnemyHero " << GetPanoramaPreviewHeroForRole(0) << '\n'
           << "previewAllyHero " << GetPanoramaPreviewHeroForRole(1) << '\n'
           << "previewCreepHero " << GetPanoramaPreviewHeroForRole(2) << '\n'
           << "enemyEspEnabled " << enemyEspEnabled << '\n'
           << "allyEspEnabled " << allyEspEnabled << '\n'
           << "enemyAbilitiesEnabled " << enemyAbilitiesEnabled << '\n'
           << "allyAbilitiesEnabled " << allyAbilitiesEnabled << '\n'
           << "creepAbilitiesEnabled " << creepAbilitiesEnabled << '\n'
           << "enemyBoxesEnabled " << enemyBoxesEnabled << '\n'
           << "allyBoxesEnabled " << allyBoxesEnabled << '\n'
           << "enemyCornerBoxesEnabled " << enemyCornerBoxesEnabled << '\n'
           << "allyCornerBoxesEnabled " << allyCornerBoxesEnabled << '\n'
           << "enemyHealthEnabled " << enemyHealthEnabled << '\n'
           << "allyHealthEnabled " << allyHealthEnabled << '\n'
           << "enemyHealthValuesEnabled " << enemyHealthValuesEnabled << '\n'
           << "allyHealthValuesEnabled " << allyHealthValuesEnabled << '\n'
           << "enemyNamesEnabled " << enemyNamesEnabled << '\n'
           << "allyNamesEnabled " << allyNamesEnabled << '\n'
           << "enemyPlayerNamesEnabled " << enemyPlayerNamesEnabled << '\n'
           << "allyPlayerNamesEnabled " << allyPlayerNamesEnabled << '\n'
           << "enemyDistanceEnabled " << enemyDistanceEnabled << '\n'
           << "allyDistanceEnabled " << allyDistanceEnabled << '\n'
           << "enemySnaplinesEnabled " << enemySnaplinesEnabled << '\n'
           << "allySnaplinesEnabled " << allySnaplinesEnabled << '\n'
           << "enemyBonesEnabled " << enemyBonesEnabled << '\n'
           << "allyBonesEnabled " << allyBonesEnabled << '\n'
           << "drawBoxes " << drawBoxes << '\n'
           << "drawHealth " << drawHealth << '\n'
           << "drawHealthValues " << drawHealthValues << '\n'
           << "drawNames " << drawNames << '\n'
           << "drawDistance " << drawDistance << '\n'
           << "drawSnaplines " << drawSnaplines << '\n'
           << "drawFovCircle " << drawFovCircle << '\n'
           << "drawFarmFovCircle " << drawFarmFovCircle << '\n'
           << "drawBones " << drawBones << '\n'
           << "drawCreepEsp " << drawCreepEsp << '\n'
           << "creepEspEnabled " << creepEspEnabled << '\n'
           << "neutralCreepEspEnabled " << neutralCreepEspEnabled << '\n'
           << "creepBoxesEnabled " << creepBoxesEnabled << '\n'
           << "creepCornerBoxesEnabled " << creepCornerBoxesEnabled << '\n'
           << "creepHealthEnabled " << creepHealthEnabled << '\n'
           << "creepHealthValuesEnabled " << creepHealthValuesEnabled << '\n'
           << "creepDistanceEnabled " << creepDistanceEnabled << '\n'
           << "allyCreepEspEnabled " << allyCreepEspEnabled << '\n'
           << "allyCreepBoxesEnabled " << allyCreepBoxesEnabled << '\n'
           << "allyCreepCornerBoxesEnabled " << allyCreepCornerBoxesEnabled << '\n'
           << "allyCreepHealthEnabled " << allyCreepHealthEnabled << '\n'
           << "allyCreepHealthValuesEnabled " << allyCreepHealthValuesEnabled << '\n'
           << "allyCreepDistanceEnabled " << allyCreepDistanceEnabled << '\n'
           << "enemyEspMaxDistance " << enemyEspMaxDistance << '\n'
           << "allyEspMaxDistance " << allyEspMaxDistance << '\n'
           << "creepEspMaxDistance " << creepEspMaxDistance << '\n'
           << "orbEspMaxDistance " << orbEspMaxDistance << '\n'
           << "powerupEspEnabled " << powerupEspEnabled << '\n'
           << "enemyTrooperChams " << enemyTrooperChams << '\n'
           << "allyTrooperChams " << allyTrooperChams << '\n'
           << "neutralChams " << neutralChams << '\n'
           << "fovChangerEnabled " << fovChangerEnabled << '\n'
           << "overrideScopeFov " << overrideScopeFov << '\n'
           << "cameraFov " << cameraFov << '\n'
           << "scopedCameraFov " << scopedCameraFov << '\n'
           << "worldModulationEnabled " << worldModulationEnabled << '\n'
           << "disableSkybox " << disableSkybox << '\n'
           << "skyboxBrightness " << skyboxBrightness << '\n'
           << "lightBrightness " << lightBrightness << '\n'
           << "talonEspEnabled " << talonEspEnabled << '\n'
           << "campTimersEnabled " << campTimersEnabled << '\n'
           << "campTimersOnScreen " << campTimersOnScreen << '\n'
           << "campTimersOnMinimap " << campTimersOnMinimap << '\n'
           << "campTimerMinimapSize " << campTimerMinimapSize << '\n'
           << "drawTeammates " << drawTeammates << '\n'
           << "drawPlayerNames " << drawPlayerNames << '\n'
           << "cornerBoxes " << cornerBoxes << '\n'
           << "boxThickness " << boxThickness << '\n'
           << "cornerBoxLength " << cornerBoxLength << '\n'
           << "menuAccentR " << menuAccentColor[0] << '\n'
           << "menuAccentG " << menuAccentColor[1] << '\n'
           << "menuAccentB " << menuAccentColor[2] << '\n'
           << "menuAccentA " << menuAccentColor[3] << '\n'
           << "enemyBoxR " << enemyBoxColor[0] << '\n'
           << "enemyBoxG " << enemyBoxColor[1] << '\n'
           << "enemyBoxB " << enemyBoxColor[2] << '\n'
           << "enemyBoxA " << enemyBoxColor[3] << '\n'
           << "teammateBoxR " << teammateBoxColor[0] << '\n'
           << "teammateBoxG " << teammateBoxColor[1] << '\n'
           << "teammateBoxB " << teammateBoxColor[2] << '\n'
           << "teammateBoxA " << teammateBoxColor[3] << '\n'
           << "enemyNameR " << enemyNameColor[0] << '\n'
           << "enemyNameG " << enemyNameColor[1] << '\n'
           << "enemyNameB " << enemyNameColor[2] << '\n'
           << "enemyNameA " << enemyNameColor[3] << '\n'
           << "teammateNameR " << teammateNameColor[0] << '\n'
           << "teammateNameG " << teammateNameColor[1] << '\n'
            << "teammateNameB " << teammateNameColor[2] << '\n'
            << "teammateNameA " << teammateNameColor[3] << '\n'
            << "enemySkeletonR " << enemySkeletonColor[0] << '\n'
            << "enemySkeletonG " << enemySkeletonColor[1] << '\n'
            << "enemySkeletonB " << enemySkeletonColor[2] << '\n'
            << "enemySkeletonA " << enemySkeletonColor[3] << '\n'
            << "teammateSkeletonR " << teammateSkeletonColor[0] << '\n'
            << "teammateSkeletonG " << teammateSkeletonColor[1] << '\n'
            << "teammateSkeletonB " << teammateSkeletonColor[2] << '\n'
            << "teammateSkeletonA " << teammateSkeletonColor[3] << '\n'
           << "enemyPlayerNameR " << enemyPlayerNameColor[0] << '\n'
           << "enemyPlayerNameG " << enemyPlayerNameColor[1] << '\n'
           << "enemyPlayerNameB " << enemyPlayerNameColor[2] << '\n'
           << "enemyPlayerNameA " << enemyPlayerNameColor[3] << '\n'
           << "teammatePlayerNameR " << teammatePlayerNameColor[0] << '\n'
           << "teammatePlayerNameG " << teammatePlayerNameColor[1] << '\n'
           << "teammatePlayerNameB " << teammatePlayerNameColor[2] << '\n'
           << "teammatePlayerNameA " << teammatePlayerNameColor[3] << '\n'
           << "enemyHealthR " << enemyHealthColor[0] << '\n'
           << "enemyHealthG " << enemyHealthColor[1] << '\n'
           << "enemyHealthB " << enemyHealthColor[2] << '\n'
           << "enemyHealthA " << enemyHealthColor[3] << '\n'
           << "teammateHealthR " << teammateHealthColor[0] << '\n'
           << "teammateHealthG " << teammateHealthColor[1] << '\n'
           << "teammateHealthB " << teammateHealthColor[2] << '\n'
           << "teammateHealthA " << teammateHealthColor[3] << '\n'
           << "enemyGlowR " << enemyGlowColor[0] << '\n'
           << "enemyGlowG " << enemyGlowColor[1] << '\n'
           << "enemyGlowB " << enemyGlowColor[2] << '\n'
           << "enemyGlowA " << enemyGlowColor[3] << '\n'
           << "teammateGlowR " << teammateGlowColor[0] << '\n'
           << "teammateGlowG " << teammateGlowColor[1] << '\n'
           << "teammateGlowB " << teammateGlowColor[2] << '\n'
           << "teammateGlowA " << teammateGlowColor[3] << '\n'
           << "enemyGlowEnabled " << enemyGlowEnabled << '\n'
           << "allyGlowEnabled " << allyGlowEnabled << '\n'
           << "enemyRadarEnabled " << enemyRadarEnabled << '\n'
           << "enemyChamsEnabled " << enemyChamsEnabled << '\n'
           << "allyChamsEnabled " << allyChamsEnabled << '\n'
           << "enemyChamsR " << enemyChamsColor[0] << '\n'
           << "enemyChamsG " << enemyChamsColor[1] << '\n'
           << "enemyChamsB " << enemyChamsColor[2] << '\n'
           << "enemyChamsA " << enemyChamsColor[3] << '\n'
           << "allyChamsR " << allyChamsColor[0] << '\n'
           << "allyChamsG " << allyChamsColor[1] << '\n'
           << "allyChamsB " << allyChamsColor[2] << '\n'
           << "allyChamsA " << allyChamsColor[3] << '\n'
           << "enemyHealthBarR " << enemyHealthBarColor[0] << '\n'
           << "enemyHealthBarG " << enemyHealthBarColor[1] << '\n'
           << "enemyHealthBarB " << enemyHealthBarColor[2] << '\n'
           << "enemyHealthBarA " << enemyHealthBarColor[3] << '\n'
           << "teammateHealthBarR " << teammateHealthBarColor[0] << '\n'
           << "teammateHealthBarG " << teammateHealthBarColor[1] << '\n'
           << "teammateHealthBarB " << teammateHealthBarColor[2] << '\n'
           << "teammateHealthBarA " << teammateHealthBarColor[3] << '\n'
           << "enemyHealthValueR " << enemyHealthValueColor[0] << '\n'
           << "enemyHealthValueG " << enemyHealthValueColor[1] << '\n'
           << "enemyHealthValueB " << enemyHealthValueColor[2] << '\n'
           << "enemyHealthValueA " << enemyHealthValueColor[3] << '\n'
           << "teammateHealthValueR " << teammateHealthValueColor[0] << '\n'
           << "teammateHealthValueG " << teammateHealthValueColor[1] << '\n'
           << "teammateHealthValueB " << teammateHealthValueColor[2] << '\n'
           << "teammateHealthValueA " << teammateHealthValueColor[3] << '\n'
           << "creepBoxR " << creepBoxColor[0] << '\n'
           << "creepBoxG " << creepBoxColor[1] << '\n'
           << "creepBoxB " << creepBoxColor[2] << '\n'
           << "creepBoxA " << creepBoxColor[3] << '\n'
           << "creepHealthR " << creepHealthColor[0] << '\n'
           << "creepHealthG " << creepHealthColor[1] << '\n'
            << "creepHealthB " << creepHealthColor[2] << '\n'
            << "creepHealthA " << creepHealthColor[3] << '\n'
            << "creepHealthValueR " << creepHealthValueColor[0] << '\n'
            << "creepHealthValueG " << creepHealthValueColor[1] << '\n'
            << "creepHealthValueB " << creepHealthValueColor[2] << '\n'
            << "creepHealthValueA " << creepHealthValueColor[3] << '\n'
           << "allyCreepBoxR " << allyCreepBoxColor[0] << '\n'
           << "allyCreepBoxG " << allyCreepBoxColor[1] << '\n'
           << "allyCreepBoxB " << allyCreepBoxColor[2] << '\n'
           << "allyCreepBoxA " << allyCreepBoxColor[3] << '\n'
           << "allyCreepHealthR " << allyCreepHealthColor[0] << '\n'
           << "allyCreepHealthG " << allyCreepHealthColor[1] << '\n'
            << "allyCreepHealthB " << allyCreepHealthColor[2] << '\n'
            << "allyCreepHealthA " << allyCreepHealthColor[3] << '\n'
            << "allyCreepHealthValueR " << allyCreepHealthValueColor[0] << '\n'
            << "allyCreepHealthValueG " << allyCreepHealthValueColor[1] << '\n'
            << "allyCreepHealthValueB " << allyCreepHealthValueColor[2] << '\n'
             << "allyCreepHealthValueA " << allyCreepHealthValueColor[3] << '\n'
             << "enemyTrooperChamsR " << enemyTrooperChamsColor[0] << '\n'
             << "enemyTrooperChamsG " << enemyTrooperChamsColor[1] << '\n'
             << "enemyTrooperChamsB " << enemyTrooperChamsColor[2] << '\n'
             << "enemyTrooperChamsA " << enemyTrooperChamsColor[3] << '\n'
             << "allyTrooperChamsR " << allyTrooperChamsColor[0] << '\n'
             << "allyTrooperChamsG " << allyTrooperChamsColor[1] << '\n'
             << "allyTrooperChamsB " << allyTrooperChamsColor[2] << '\n'
             << "allyTrooperChamsA " << allyTrooperChamsColor[3] << '\n'
             << "neutralChamsR " << neutralChamsColor[0] << '\n'
             << "neutralChamsG " << neutralChamsColor[1] << '\n'
             << "neutralChamsB " << neutralChamsColor[2] << '\n'
             << "neutralChamsA " << neutralChamsColor[3] << '\n'
             << "skyboxColorR " << skyboxColor[0] << '\n'
             << "skyboxColorG " << skyboxColor[1] << '\n'
             << "skyboxColorB " << skyboxColor[2] << '\n'
             << "skyboxColorA " << skyboxColor[3] << '\n'
             << "propsColorR " << propsColor[0] << '\n'
             << "propsColorG " << propsColor[1] << '\n'
             << "propsColorB " << propsColor[2] << '\n'
             << "propsColorA " << propsColor[3] << '\n'
             << "lightColorR " << lightColor[0] << '\n'
             << "lightColorG " << lightColor[1] << '\n'
             << "lightColorB " << lightColor[2] << '\n'
             << "lightColorA " << lightColor[3] << '\n'
             << "worldColorR " << worldColor[0] << '\n'
             << "worldColorG " << worldColor[1] << '\n'
             << "worldColorB " << worldColor[2] << '\n'
             << "worldColorA " << worldColor[3] << '\n'
             << "talonEspColorR " << talonEspColor[0] << '\n'
             << "talonEspColorG " << talonEspColor[1] << '\n'
             << "talonEspColorB " << talonEspColor[2] << '\n'
             << "talonEspColorA " << talonEspColor[3] << '\n'
             << "campTimerColorR " << campTimerColor[0] << '\n'
             << "campTimerColorG " << campTimerColor[1] << '\n'
             << "campTimerColorB " << campTimerColor[2] << '\n'
             << "campTimerColorA " << campTimerColor[3] << '\n'
           << "enemyGlowMode " << enemyGlowMode << '\n'
           << "allyGlowMode " << allyGlowMode << '\n'
           << "drawOrbEsp " << drawOrbEsp << '\n'
           << "drawSpectatorList " << drawSpectatorList << '\n'
           << "freeCam " << freeCam << '\n'
           << "disableDrifterDarkness " << disableDrifterDarkness << '\n'
           << "autoActiveReload " << autoActiveReload << '\n'
           << "bunnyHop " << bunnyHop << '\n'
           << "freeCamKey " << freeCamKey << '\n'
           << "freeCamSpeed " << freeCamSpeed << '\n'
           << "movementProbeEnabled " << movementProbeEnabled << '\n'
           << "movementReplayEnabled " << movementReplayEnabled << '\n'
           << "movementReplayKey " << movementReplayKey << '\n'
           << "farmAssist " << farmAssist << '\n'
           << "autoLastHitOrbs " << autoLastHitOrbs << '\n'
           << "autoLastHitOrbsAutoFire " << autoLastHitOrbsAutoFire << '\n'
           << "autoLastHitOrbsToggleMode " << autoLastHitOrbsToggleMode << '\n'
           << "autoLastHitOrbsKey " << autoLastHitOrbsKey << '\n'
           << "orbAimVisibilityCheck " << orbAimVisibilityCheck << '\n'
           << "glowEnabled " << glowEnabled << '\n'
           << "aimAssist " << aimAssist << '\n'
           << "autoParry " << autoParry << '\n'
           << "aimSilentMode " << aimSilentMode << '\n'
           << "aimMixedMode " << aimMixedMode << '\n'
           << "aimOnlyYaw " << aimOnlyYaw << '\n'
           << "aimLockTarget " << aimLockTarget << '\n'
           << "aimLockKey " << aimLockKey << '\n'
           << "aimPrediction " << aimPrediction << '\n'
           << "antiFrog " << antiFrog << '\n'
           << "antiFrogHsThreshold " << antiFrogHsThreshold << '\n'
           << "aimVisibilityCheck " << aimVisibilityCheck << '\n'
           << "aimToggleMode " << aimToggleMode << '\n'
           << "farmToggleMode " << farmToggleMode << '\n'
           << "farmSilentMode " << farmSilentMode << '\n'
           << "farmMixedMode " << farmMixedMode << '\n'
           << "aimAssistKey " << aimAssistKey << '\n'
           << "farmAssistKey " << farmAssistKey << '\n'
           << "aimFov " << aimFov << '\n'
           << "farmFov " << farmFov << '\n'
           << "aimSmooth " << aimSmooth << '\n'
           << "aimHitchance " << aimHitchance << '\n'
           << "aimPitchSmooth " << aimPitchSmooth << '\n'
           << "aimYawSmooth " << aimYawSmooth << '\n'
           << "farmAimSmooth " << farmAimSmooth << '\n'
           << "fovCircleAlpha " << fovCircleAlpha << '\n'
           << "farmFovAlpha " << farmFovAlpha << '\n'
           << "snaplineAlpha " << snaplineAlpha << '\n'
           << "aimBonesMask " << (aimBonesMask & AimBoneAll) << '\n'
           << "aimSelectionMode " << static_cast<int>(aimSelectionMode) << '\n';
    output << "vindictaAutoSnipeEnabled " << vindictaAutoSnipeEnabled << '\n'
           << "hazeSleepDaggerEnabled " << hazeSleepDaggerEnabled << '\n'
           << "shivSerratedKnivesEnabled " << shivSerratedKnivesEnabled << '\n'
           << "bebopAbility3Enabled " << bebopAbility3Enabled << '\n'
           << "bebopAbility2AutoEnabled " << bebopAbility2AutoEnabled << '\n'
           << "drifterAbility2Enabled " << drifterAbility2Enabled << '\n'
           << "heroScriptsShowFov " << heroScriptsShowFov << '\n'
           << "hazePredictionDot " << hazePredictionDot << '\n'
           << "vindictaSnipeFov " << vindictaSnipeFov << '\n'
           << "vindictaSnipeSmoothX " << vindictaSnipeSmoothX << '\n'
           << "vindictaSnipeSmoothY " << vindictaSnipeSmoothY << '\n'
           << "hazeDaggerFov " << hazeDaggerFov << '\n'
           << "hazeDaggerSmoothX " << hazeDaggerSmoothX << '\n'
           << "hazeDaggerSmoothY " << hazeDaggerSmoothY << '\n'
           << "shivKnivesFov " << shivKnivesFov << '\n'
           << "shivKnivesSmoothX " << shivKnivesSmoothX << '\n'
           << "shivKnivesSmoothY " << shivKnivesSmoothY << '\n';
    output << "bebopAbility3Fov " << bebopAbility3Fov << '\n'
           << "bebopAbility3SmoothX " << bebopAbility3SmoothX << '\n'
           << "bebopAbility3SmoothY " << bebopAbility3SmoothY << '\n'
           << "drifterAbility2Fov " << drifterAbility2Fov << '\n'
           << "drifterAbility2SmoothX " << drifterAbility2SmoothX << '\n'
           << "drifterAbility2SmoothY " << drifterAbility2SmoothY << '\n';
    if (!output.good()) return;

    const std::string contents = output.str();
    auto cached = savedConfigContents.find(path);
    if (cached == savedConfigContents.end()) {
        std::ifstream existing(path, std::ios::binary);
        std::string existingContents;
        if (existing) {
            existingContents.assign(std::istreambuf_iterator<char>(existing),
                                    std::istreambuf_iterator<char>());
        }
        cached = savedConfigContents.emplace(path, std::move(existingContents)).first;
    }

    const DWORD attributes = GetFileAttributesA(path.c_str());
    const bool targetExists =
        attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    if (targetExists && cached->second == contents) return;

    const std::string temporary = path + ".tmp";
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) return;
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.flush();
    if (!file.good()) {
        file.close();
        DeleteFileA(temporary.c_str());
        return;
    }
    file.close();
    // Never truncate the live config. A crash or game termination can at
    // worst leave the temporary file behind; the previous menu.ini remains
    // intact and is still loaded on the next start.
    if (MoveFileExA(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        cached->second = contents;
    } else {
        DeleteFileA(temporary.c_str());
    }
}
