#include "shared.h"
#include "hero_scripts.h"
#include "portable_paths.h"
#include "usercmd.hpp"
#include "usercmd_runtime.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <MinHook.h>

namespace {

// Camera-relative movement is intentionally disabled. Silent aim must not
// alter the player's normal movement basis.
constexpr bool kCameraRelativeMovement = false;
constexpr bool kSilentCommandMovementCorrection = true;
constexpr bool kSilentMovementYawIsolation = false;
constexpr bool kSilentMoveDataYawIsolation = false;
constexpr bool kRuntimeDiagnostics = false;

uintptr_t FindLocalPawnFromController() {
    if (!clientBase) return 0;
    const uintptr_t entityRoot = Read<uintptr_t>(
        clientBase + Offsets::GameEntitySystem);
    if (!entityRoot) return 0;

    const int reportedHighest = Read<int>(
        entityRoot + Offsets::HighestEntityIndex);
    if (reportedHighest <= 0 ||
        reportedHighest > static_cast<int>(Offsets::HandleIndexMask)) return 0;

    const uint32_t highest = static_cast<uint32_t>(reportedHighest);
    const uint32_t highestChunk = highest >> Offsets::HandleChunkShift;
    for (uint32_t chunkIndex = 0; chunkIndex <= highestChunk; ++chunkIndex) {
        const uintptr_t chunk = Read<uintptr_t>(
            entityRoot + Offsets::EntityChunks +
            Offsets::EntityChunkStride * chunkIndex);
        if (!chunk) continue;
        const uint32_t highestSlot = chunkIndex == highestChunk
            ? (highest & Offsets::HandleChunkMask)
            : Offsets::HandleChunkMask;
        for (uint32_t slot = 0; slot <= highestSlot; ++slot) {
            const uintptr_t identity = chunk + Offsets::EntityStride * slot;
            const uintptr_t controller = Read<uintptr_t>(identity);
            if (!controller || Read<uint8_t>(
                    controller + Offsets::IsLocalPlayerController) != 1) continue;
            const uint32_t pawnHandle = Read<uint32_t>(
                controller + Offsets::ControllerPawn);
            const uintptr_t pawn = ResolveEntity(pawnHandle);
            if (pawn) return pawn;
        }
    }
    return 0;
}

using EntityLifecycleFn = void(__fastcall*)(uintptr_t, uintptr_t, uint32_t);
EntityLifecycleFn originalEntityAdded = nullptr;
EntityLifecycleFn originalEntityRemoved = nullptr;
void* entityAddedTarget = nullptr;
void* entityRemovedTarget = nullptr;

uintptr_t FindClientPattern(const char* pattern, uintptr_t startAddress = 0) {
    if (!clientBase) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase), &info, sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token) bytes.push_back(token == "?" ? -1 : std::strtoul(token.c_str(), nullptr, 16));
    if (bytes.empty() || bytes.size() > info.SizeOfImage) return 0;
    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    size_t begin = 0;
    if (startAddress > clientBase) {
        begin = static_cast<size_t>(startAddress - clientBase);
        if (begin >= info.SizeOfImage) return 0;
    }
    for (size_t i = begin; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && image[i + j] != static_cast<uint8_t>(bytes[j])) { match = false; break; }
        }
        if (match) return clientBase + i;
    }
    return 0;
}

uintptr_t FindUniqueModulePattern(HMODULE module, const char* pattern) {
    if (!module || !pattern) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info,
                              sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token)
        bytes.push_back(token == "?" ? -1
                                      : std::strtoul(token.c_str(), nullptr, 16));
    if (bytes.empty() || bytes.size() > info.SizeOfImage) return 0;

    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* image = reinterpret_cast<const uint8_t*>(base);
    uintptr_t matchAddress = 0;
    unsigned matches = 0;
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 &&
                image[i + j] != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        matchAddress = base + i;
        if (++matches > 1) return 0;
    }
    return matches == 1 ? matchAddress : 0;
}

using ClientOutputFn = void(__fastcall*)(void*, void*, void*, void*);
ClientOutputFn originalClientOutput = nullptr;
void* clientOutputTarget = nullptr;

void __fastcall hkClientOutput(void* a1, void* a2, void* a3, void* a4) {
    static thread_local bool insideCallback = false;
    if (!insideCallback &&
        InterlockedCompareExchange(&unloadRequested, 0, 0) == 0) {
        insideCallback = true;
        PublishVisualFrameSnapshot();
        insideCallback = false;
    }
    if (originalClientOutput)
        originalClientOutput(a1, a2, a3, a4);
}

bool InstallVisualFrameHookInternal() {
    if (clientOutputTarget) return true;
    HMODULE engine = GetModuleHandleA("engine2.dll");
    constexpr const char* ClientOutputPattern =
        "48 89 5C 24 ? 55 56 57 41 54 41 56 48 83 EC ? 48 8D 05";
    clientOutputTarget = reinterpret_cast<void*>(
        FindUniqueModulePattern(engine, ClientOutputPattern));
    if (!clientOutputTarget) return false;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        clientOutputTarget = nullptr;
        return false;
    }
    const MH_STATUS created = MH_CreateHook(
        clientOutputTarget, reinterpret_cast<void*>(&hkClientOutput),
        reinterpret_cast<void**>(&originalClientOutput));
    if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) {
        clientOutputTarget = nullptr;
        originalClientOutput = nullptr;
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(clientOutputTarget);
    if (enabled != MH_OK && enabled != MH_ERROR_ENABLED) {
        MH_RemoveHook(clientOutputTarget);
        clientOutputTarget = nullptr;
        originalClientOutput = nullptr;
        return false;
    }
    return true;
}

void RemoveVisualFrameHookInternal() {
    if (clientOutputTarget) {
        MH_DisableHook(clientOutputTarget);
        MH_RemoveHook(clientOutputTarget);
    }
    clientOutputTarget = nullptr;
    originalClientOutput = nullptr;
}

void __fastcall HookEntityAdded(uintptr_t system, uintptr_t instance, uint32_t handle) {
    if (originalEntityAdded) originalEntityAdded(system, instance, handle);
    QueueOrbEntityAdded(handle);
}

void __fastcall HookEntityRemoved(uintptr_t system, uintptr_t instance, uint32_t handle) {
    QueueOrbEntityRemoved(handle);
    if (originalEntityRemoved) originalEntityRemoved(system, instance, handle);
}

// IDA-confirmed CUserCmd_to_network entrypoint:
// image EA 0x1814E0F00 - image base 0x180000000 = RVA 0x14E0F00.
constexpr uintptr_t UserCmdToNetworkRva = 0x14E0F00;
using UserCmdToNetworkFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
UserCmdToNetworkFn originalUserCmdToNetwork = nullptr;
bool userCmdHookInstalled = false;
std::atomic<unsigned long long> silentAppliedCalls{0};
std::atomic<unsigned long long> createMoveCalls{0};
std::atomic<unsigned long long> userCmdResolvedCalls{0};
std::atomic<bool> bunnyBlockAirJump{false};
std::atomic<bool> bunnyJumpOneShot{false};
std::atomic<bool> bunnyDashJumpOneShot{false};
std::atomic<bool> bunnyFinishDashJumpInput{false};
std::atomic<ULONGLONG> bunnyDashGuardUntil{0};
UserCmdFunctionAddresses runtimeUserCmdFunctions{};
bool pendingHeroSilentOverridesPrimary = false;

using CreateMoveFn = void(__fastcall*)(uintptr_t, uint32_t, char);
using GetUserCmdTickFn = void(__fastcall*)(uintptr_t, int32_t*);
using GetUserCmdArrayFn = uintptr_t(__fastcall*)(uintptr_t, int);
using GetUserCmdBySequenceFn = uintptr_t(__fastcall*)(uintptr_t, uint32_t);
CreateMoveFn originalCreateMove = nullptr;
void* createMoveTarget = nullptr;
#ifndef DLL6_MOVEMENT_ONLY
void RefreshDrifterDarknessForToggle();
#endif

// CreateMove calls input->vtable[6](input, userCmd) after building the
// command and before the command is consumed by local movement. This is the
// correct point for camera-relative correction; changing the command after
// CreateMove returns is too late for the pawn.
using ApplyInputCommandFn = void(__fastcall*)(uintptr_t, uintptr_t);
ApplyInputCommandFn originalApplyInputCommand = nullptr;
void* applyInputCommandTarget = nullptr;
bool applyInputCommandHookInstalled = false;

void ApplyVisibleAimInput(uintptr_t input);
#ifndef DLL6_MOVEMENT_ONLY
void ApplyBunnyHop(uintptr_t userCmd);
#endif

using PawnProcessUserCmdFn = void(__fastcall*)(uintptr_t, uintptr_t);
PawnProcessUserCmdFn originalPawnProcessUserCmd = nullptr;
void* pawnProcessUserCmdTarget = nullptr;
bool pawnProcessUserCmdHookInstalled = false;

struct PawnUserCmdSnapshot {
    uintptr_t userCmd = 0;
    float forward = 0.0f;
    float left = 0.0f;
    float cameraYaw = 0.0f;
    bool valid = false;
};

// CreateMove and the local-pawn command callback run synchronously on the
// game's input thread. Keep the exact command built by the pawn callback so
// the later silent patch cannot accidentally use a different ring entry.
PawnUserCmdSnapshot latestPawnUserCmd{};
std::mutex latestPawnUserCmdMutex;

PawnUserCmdSnapshot ReadLatestPawnUserCmd() {
    std::lock_guard<std::mutex> lock(latestPawnUserCmdMutex);
    return latestPawnUserCmd;
}

// IDA 2026-07-29: CCitadelPlayer_MovementServices::ProcessMovement,
// vtable slot 30, receives the fully prepared movement-data object.
constexpr uintptr_t ProcessMovementRva = 0x7A61A0;
using ProcessMovementFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t);
ProcessMovementFn originalProcessMovement = nullptr;
void* processMovementTarget = nullptr;
bool EnsureProcessMovementHook(uintptr_t pawn);

// IDA: sub_18078CC00 builds the final world-space wish direction in a3.
constexpr uintptr_t BuildWishDirectionRva = 0x78CC00;
using BuildWishDirectionFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t,
                                                     uintptr_t, uintptr_t);
BuildWishDirectionFn originalBuildWishDirection = nullptr;
void* buildWishDirectionTarget = nullptr;

// IDA: sub_180789330 performs the final wish-direction acceleration and
// writes the resulting velocity into MoveData. Correct the inputs at this
// boundary so the game's own acceleration code consumes the camera basis.
constexpr uintptr_t AccelerateMovementRva = 0x789330;
using AccelerateMovementFn = void(__fastcall*)(uintptr_t, uintptr_t, float);
AccelerateMovementFn originalAccelerateMovement = nullptr;
void* accelerateMovementTarget = nullptr;

// Current Deadlock camera-origin writer. This signature resolves the only
// instructions in the current client image that store all three axes into
// ViewMatrix + 0xC0. Free cam temporarily NOPs those two stores, exactly like
// the Source 2 external-freecam technique, and restores them on disable.
constexpr const char* CameraOriginWriterPattern =
    "F2 0F 11 05 ? ? ? ? 41 8B 46 08 89 05 ? ? ? ? "
    "F2 0F 10 45 00";
void* cameraOriginWriterXY = nullptr;
void* cameraOriginWriterZ = nullptr;
std::array<uint8_t, 8> cameraOriginWriterXYBytes{};
std::array<uint8_t, 6> cameraOriginWriterZBytes{};
std::mutex cameraOriginPatchMutex;
bool cameraOriginWritersPatched = false;

// CCitadelCameraManager moves between client patches. Resolve its primary
// vtable through MSVC RTTI and then locate the inline singleton whose active
// camera lives at +0x28. This also distinguishes the secondary base vtable at
// manager+8, which otherwise looks like another manager candidate.
uintptr_t ResolveCitadelCameraManager() {
    static uintptr_t cachedManager = 0;
    if (cachedManager && Read<uintptr_t>(cachedManager))
        return cachedManager;
    if (!clientBase) return 0;

    MODULEINFO info{};
    if (!GetModuleInformation(
            GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase),
            &info, sizeof(info))) return 0;
    const std::size_t imageSize = info.SizeOfImage;
    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    constexpr char typeName[] = ".?AVCCitadelCameraManager@@";
    constexpr std::size_t typeNameLength = sizeof(typeName) - 1;

    uintptr_t nameAddress = 0;
    for (std::size_t offset = 16;
         offset + typeNameLength <= imageSize; ++offset) {
        if (std::memcmp(image + offset, typeName, typeNameLength) == 0) {
            nameAddress = clientBase + offset;
            break;
        }
    }
    if (!nameAddress) return 0;

    const uintptr_t typeDescriptor = nameAddress - 16;
    const uint32_t typeDescriptorRva = static_cast<uint32_t>(
        typeDescriptor - clientBase);
    for (std::size_t fieldOffset = 12;
         fieldOffset + sizeof(uint32_t) <= imageSize; ++fieldOffset) {
        if (Read<uint32_t>(clientBase + fieldOffset) != typeDescriptorRva)
            continue;
        const uintptr_t locator = clientBase + fieldOffset - 12;
        if (Read<uint32_t>(locator) != 1) continue;

        for (std::size_t slotOffset = 0;
             slotOffset + sizeof(uintptr_t) <= imageSize;
             slotOffset += alignof(uintptr_t)) {
            if (Read<uintptr_t>(clientBase + slotOffset) != locator)
                continue;
            const uintptr_t vtable =
                clientBase + slotOffset + sizeof(uintptr_t);
            for (std::size_t objectOffset = 0;
                 objectOffset + 0x30 <= imageSize;
                 objectOffset += alignof(uintptr_t)) {
                const uintptr_t candidate = clientBase + objectOffset;
                if (Read<uintptr_t>(candidate) != vtable) continue;
                const uintptr_t activeCamera =
                    Read<uintptr_t>(candidate + 0x28);
                const uintptr_t cameraVtable = Read<uintptr_t>(activeCamera);
                if (activeCamera > 0x10000 &&
                    cameraVtable >= clientBase &&
                    cameraVtable < clientBase + imageSize) {
                    cachedManager = candidate;
                    return cachedManager;
                }
            }
        }
    }
    return 0;
}

constexpr const char* CreateFreeCameraPattern =
    "40 53 48 83 EC 20 48 8B D9 B9 F0 00 00 00 E8 ? ? ? ? "
    "48 85 C0 74 ? 48 8B D3 48 8B C8 48 83 C4 20 5B E9";
using CreateFreeCameraFn = uintptr_t(__fastcall*)(uintptr_t);
using SwitchCameraFn = void(__fastcall*)(uintptr_t, uintptr_t, float);
using DestroyCameraFn = uintptr_t(__fastcall*)(uintptr_t, uint32_t);
using FreeCameraUpdateFn = void(__fastcall*)(uintptr_t);
using GameplayCameraUpdateFn = void(__fastcall*)(uintptr_t);
uintptr_t builtInFreeCamera = 0;
uintptr_t cameraBeforeFreeCamera = 0;
uintptr_t protectedCameraBeforeFreeCamera = 0;
bool builtInFreeCameraActive = false;
FreeCameraUpdateFn originalFreeCameraUpdate = nullptr;
void* freeCameraUpdateTarget = nullptr;
GameplayCameraUpdateFn originalGameplayCameraUpdate = nullptr;
void* gameplayCameraUpdateTarget = nullptr;
std::atomic<ULONGLONG> lastGameplayCameraHookAt{0};
using GetRenderFovFn = float(__fastcall*)(uintptr_t);
GetRenderFovFn originalGetRenderFov = nullptr;
void* getRenderFovTarget = nullptr;
std::atomic<uintptr_t> freeCameraUserCmd{0};
std::chrono::steady_clock::time_point lastFreeCameraUpdate{};
std::mutex freeCameraLifecycleMutex;
Vector3 freeCameraAnchor{};
bool freeCameraAnchorReady = false;
bool freeCameraStartPending = false;
Vector3 freeCameraStartAngles{};
bool freeCameraStartAnglesReady = false;

void FlushCurrentCameraAimInternal(uintptr_t camera = 0);

float __fastcall hkGetRenderFov(uintptr_t camera) {
    const float stockFov = originalGetRenderFov
        ? originalGetRenderFov(camera) : 90.0f;
    if (!std::isfinite(stockFov) ||
        (!fovChangerEnabled && !overrideScopeFov)) return stockFov;

    // Deadlock uses approximately 70 degrees for the stock scoped view.
    const bool scoped = stockFov <= 70.5f;
    if (scoped && overrideScopeFov)
        return std::clamp(scopedCameraFov, 20.0f, 140.0f);
    if (fovChangerEnabled)
        return std::clamp(cameraFov, 20.0f, 140.0f);
    return stockFov;
}

bool EnsureGetRenderFovHook() {
    if (getRenderFovTarget && originalGetRenderFov) return true;
    const uintptr_t address = FindUniqueClientPattern("F3 0F 10 41 50 C3");
    if (!address) return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void* target = reinterpret_cast<void*>(address);
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&hkGetRenderFov),
        reinterpret_cast<void**>(&originalGetRenderFov));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        return false;
    getRenderFovTarget = target;
    return true;
}

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
GetAsyncKeyStateFn originalGetAsyncKeyState = nullptr;
GetKeyStateFn originalGetKeyState = nullptr;
GetKeyboardStateFn originalGetKeyboardState = nullptr;
GetRawInputDataFn originalGetRawInputData = nullptr;
GetRawInputBufferFn originalGetRawInputBuffer = nullptr;
SetCursorPosFn originalSetCursorPos = nullptr;
ClipCursorFn originalClipCursor = nullptr;

bool WriteCodeBytes(void* address, const void* bytes, size_t size) {
    if (!address || !bytes || !size) return false;
    DWORD oldProtect{};
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
    std::memcpy(address, bytes, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD unused{};
    VirtualProtect(address, size, oldProtect, &unused);
    return true;
}

bool PatchCameraOriginWriters() {
    std::lock_guard<std::mutex> lock(cameraOriginPatchMutex);
    if (cameraOriginWritersPatched) return true;

    const uintptr_t expectedOrigin =
        clientBase + Offsets::ViewMatrix + Offsets::CameraOrigin;
    uintptr_t xy = 0;
    uintptr_t z = 0;
    uintptr_t searchFrom = clientBase;
    while ((xy = FindClientPattern(CameraOriginWriterPattern, searchFrom))) {
        z = xy + 12;
        const int32_t xyDisplacement = Read<int32_t>(xy + 4);
        const int32_t zDisplacement = Read<int32_t>(z + 2);
        const uintptr_t xyTarget = xy + 8 + xyDisplacement;
        const uintptr_t zTarget = z + 6 + zDisplacement;
        if (xyTarget == expectedOrigin && zTarget == expectedOrigin + 8)
            break;
        searchFrom = xy + 1;
        xy = 0;
        z = 0;
    }
    if (!xy || !z) return false;

    cameraOriginWriterXY = reinterpret_cast<void*>(xy);
    cameraOriginWriterZ = reinterpret_cast<void*>(z);
    std::memcpy(cameraOriginWriterXYBytes.data(), cameraOriginWriterXY,
        cameraOriginWriterXYBytes.size());
    std::memcpy(cameraOriginWriterZBytes.data(), cameraOriginWriterZ,
        cameraOriginWriterZBytes.size());

    const std::array<uint8_t, 8> xyNops{
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    const std::array<uint8_t, 6> zNops{
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
    if (!WriteCodeBytes(cameraOriginWriterXY, xyNops.data(), xyNops.size()))
        return false;
    if (!WriteCodeBytes(cameraOriginWriterZ, zNops.data(), zNops.size())) {
        WriteCodeBytes(cameraOriginWriterXY, cameraOriginWriterXYBytes.data(),
            cameraOriginWriterXYBytes.size());
        return false;
    }

    cameraOriginWritersPatched = true;
    return true;
}

void RestoreCameraOriginWriters() {
    std::lock_guard<std::mutex> lock(cameraOriginPatchMutex);
    if (!cameraOriginWritersPatched) return;
    WriteCodeBytes(cameraOriginWriterXY, cameraOriginWriterXYBytes.data(),
        cameraOriginWriterXYBytes.size());
    WriteCodeBytes(cameraOriginWriterZ, cameraOriginWriterZBytes.data(),
        cameraOriginWriterZBytes.size());
    cameraOriginWritersPatched = false;
    cameraOriginWriterXY = nullptr;
    cameraOriginWriterZ = nullptr;
}

void ClearFreeCameraMovement(uintptr_t userCmd) {
    if (!userCmd) return;
    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    if (auto* base = command->cmd.mutable_base()) {
        base->set_forwardmove(0.0f);
        base->set_leftmove(0.0f);
        base->set_upmove(0.0f);
        if (auto* buttons = base->mutable_buttons_pb()) {
            buttons->set_buttonstate1(0);
            buttons->set_buttonstate2(0);
            buttons->set_buttonstate3(0);
        }
    }
    command->buttonStates.buttonState1 = 0;
    command->buttonStates.buttonState2 = 0;
    command->buttonStates.buttonState3 = 0;
}

float NormalizeCameraAngle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

bool IsFreeCameraMovementKey(int key) {
    return key == 'W' || key == 'S' || key == 'A' || key == 'D' ||
        key == VK_SPACE || key == VK_CONTROL || key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_SHIFT || key == VK_LSHIFT ||
        key == VK_RSHIFT;
}

bool IsGameFocused() {
    if (!gameWindow || !IsWindow(gameWindow)) return false;
    const HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    return foreground == gameWindow ||
        GetAncestor(foreground, GA_ROOT) == GetAncestor(gameWindow, GA_ROOT);
}

void __fastcall hkFreeCameraUpdate(uintptr_t camera) {
    std::lock_guard<std::mutex> lifecycleLock(freeCameraLifecycleMutex);
    if (freeCameraStartPending && freeCameraAnchorReady) {
        Write<Vector3>(camera + 0x38, freeCameraAnchor);
        if (freeCameraStartAnglesReady) {
            Write<float>(camera + 0xC4, freeCameraStartAngles.x);
            Write<float>(camera + 0xCC, freeCameraStartAngles.y);
            Write<float>(camera + 0x44, freeCameraStartAngles.x);
            Write<float>(camera + 0x48, freeCameraStartAngles.y);
            Write<float>(camera + 0x4C, freeCameraStartAngles.z);
        }
        freeCameraStartPending = false;
    }
    const Vector3 oldPosition = Read<Vector3>(camera + 0x38);
    const float oldPitch = Read<float>(camera + 0xC4);
    const float oldYaw = Read<float>(camera + 0xCC);

    if (originalFreeCameraUpdate)
        originalFreeCameraUpdate(camera);

    if (!freeCamActive || camera != builtInFreeCamera)
        return;

    // CFreeCamera scales mouse deltas by frame time, which makes its stock
    // sensitivity extremely low. Preserve the engine's own update and only
    // amplify the angle delta it produced this frame.
    constexpr float sensitivityMultiplier = 10.0f;
    const float enginePitch = Read<float>(camera + 0xC4);
    const float engineYaw = Read<float>(camera + 0xCC);
    float pitch = oldPitch +
        NormalizeCameraAngle(enginePitch - oldPitch) * sensitivityMultiplier;
    float yaw = oldYaw +
        NormalizeCameraAngle(engineYaw - oldYaw) * sensitivityMultiplier;
    if (pitch > 89.0f) pitch = 89.0f;
    if (pitch < -89.0f) pitch = -89.0f;
    yaw = NormalizeCameraAngle(yaw);

    Write<float>(camera + 0xC4, pitch);
    Write<float>(camera + 0xCC, yaw);
    Write<float>(camera + 0x44, pitch);
    Write<float>(camera + 0x48, yaw);

    const auto now = std::chrono::steady_clock::now();
    float deltaTime = 1.0f / 60.0f;
    if (lastFreeCameraUpdate.time_since_epoch().count()) {
        deltaTime = std::chrono::duration<float>(
            now - lastFreeCameraUpdate).count();
        if (deltaTime < 0.001f) deltaTime = 0.001f;
        if (deltaTime > 0.05f) deltaTime = 0.05f;
    }
    lastFreeCameraUpdate = now;

    const auto keyDown = [](int key) {
        if (!IsGameFocused()) return false;
        const SHORT state = originalGetAsyncKeyState
            ? originalGetAsyncKeyState(key)
            : GetAsyncKeyState(key);
        return (state & 0x8000) != 0;
    };
    const bool wantsMovement =
        keyDown('W') || keyDown('S') || keyDown('A') || keyDown('D') ||
        keyDown(VK_SPACE) || keyDown(VK_CONTROL);
    // Keep only the engine's angle update. Its position update performs camera
    // collision and pushes the camera onto the top of geometry. Start from
    // the previous origin every frame and apply our own noclip translation.
    Vector3 position = oldPosition;
    if (wantsMovement) {
        constexpr float degreesToRadians = 0.01745329251994329577f;
        const float pitchRadians = pitch * degreesToRadians;
        const float yawRadians = yaw * degreesToRadians;
        const float cosPitch = std::cos(pitchRadians);
        const Vector3 forward{
            cosPitch * std::cos(yawRadians),
            cosPitch * std::sin(yawRadians),
            -std::sin(pitchRadians)};
        const Vector3 right{
            std::sin(yawRadians), -std::cos(yawRadians), 0.0f};

        float forwardAxis =
            (keyDown('W') ? 1.0f : 0.0f) -
            (keyDown('S') ? 1.0f : 0.0f);
        float rightAxis =
            (keyDown('D') ? 1.0f : 0.0f) -
            (keyDown('A') ? 1.0f : 0.0f);
        float upAxis =
            (keyDown(VK_SPACE) ? 1.0f : 0.0f) -
            (keyDown(VK_CONTROL) ? 1.0f : 0.0f);
        const float axisLength = std::sqrt(
            forwardAxis * forwardAxis + rightAxis * rightAxis +
            upAxis * upAxis);
        if (axisLength > 1.0f) {
            forwardAxis /= axisLength;
            rightAxis /= axisLength;
            upAxis /= axisLength;
        }

        const float speed =
            freeCamSpeed * (keyDown(VK_SHIFT) ? 3.0f : 1.0f) * deltaTime;
        position.x +=
            (forward.x * forwardAxis + right.x * rightAxis) * speed;
        position.y +=
            (forward.y * forwardAxis + right.y * rightAxis) * speed;
        position.z +=
            (forward.z * forwardAxis + upAxis) * speed;
    }

    // Deliberately do not clamp the camera to the activation anchor.  The
    // free camera is noclip and may travel any distance; only reject invalid
    // coordinates so a bad input/state cannot write NaNs into the engine.
    if (!std::isfinite(position.x) ||
        !std::isfinite(position.y) ||
        !std::isfinite(position.z)) {
        position = freeCameraAnchorReady
            ? freeCameraAnchor : oldPosition;
    }
    Write<Vector3>(camera + 0x38, position);

    // The camera has already consumed this command. Clear movement only now,
    // so WASD moves CFreeCamera but is not sent as pawn movement.
    ClearFreeCameraMovement(
        freeCameraUserCmd.load(std::memory_order_acquire));
}

void __fastcall hkGameplayCameraUpdate(uintptr_t camera) {
    lastGameplayCameraHookAt.store(GetTickCount64(), std::memory_order_release);
    // Apply once at the beginning of the complete per-frame camera update.
    // The stock update then consumes the angles and publishes a coherent
    // render transform during the same camera frame.
    FlushCurrentCameraAimInternal(camera);
    if (originalGameplayCameraUpdate)
        originalGameplayCameraUpdate(camera);
    // Re-apply the same Normal/Mixed camera target after the stock update.
    // During firing the pitch path is immediate, so recoil added inside the
    // stock callback cannot survive into the next camera state.
    FlushCurrentCameraAimInternal(camera);
}

bool EnsureFreeCameraUpdateHook(uintptr_t camera) {
    if (freeCameraUpdateTarget && originalFreeCameraUpdate)
        return true;
    if (!camera) return false;
    const uintptr_t vtable = Read<uintptr_t>(camera);
    if (!vtable) return false;
    // Current CFreeCamera vtable: slot 3 is the per-frame camera update
    // (IDA RVA 0x16057F0 in this client).
    const uintptr_t updateAddress =
        Read<uintptr_t>(vtable + 3 * sizeof(uintptr_t));
    if (!updateAddress)
        return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    void* target = reinterpret_cast<void*>(updateAddress);
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&hkFreeCameraUpdate),
        reinterpret_cast<void**>(&originalFreeCameraUpdate));
    if (createStatus != MH_OK &&
        createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        return false;

    freeCameraUpdateTarget = target;
    return true;
}

bool EnsureGameplayCameraUpdateHook() {
    if (gameplayCameraUpdateTarget && originalGameplayCameraUpdate)
        return true;
    if (!clientBase || freeCamActive) return false;

    const uintptr_t manager = ResolveCitadelCameraManager();
    if (!manager) return false;
    const uintptr_t camera = Read<uintptr_t>(manager + 0x28);
    if (!camera || camera == builtInFreeCamera) return false;
    const uintptr_t vtable = Read<uintptr_t>(camera);
    if (!vtable) return false;
    const uintptr_t updateAddress =
        Read<uintptr_t>(vtable + 3 * sizeof(uintptr_t));
    if (!updateAddress ||
        reinterpret_cast<void*>(updateAddress) == freeCameraUpdateTarget)
        return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void* target = reinterpret_cast<void*>(updateAddress);
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&hkGameplayCameraUpdate),
        reinterpret_cast<void**>(&originalGameplayCameraUpdate));
    if (createStatus != MH_OK &&
        createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        return false;
    gameplayCameraUpdateTarget = target;
    return true;
}

bool ActivateBuiltInFreeCamera() {
    if (builtInFreeCameraActive && builtInFreeCamera) return true;
    RestoreCameraOriginWriters();

    const uintptr_t manager = ResolveCitadelCameraManager();
    if (!manager) return false;
    const uintptr_t managerVtable = Read<uintptr_t>(manager);
    if (!managerVtable) return false;
    const uintptr_t switchAddress =
        Read<uintptr_t>(managerVtable + 9 * sizeof(uintptr_t));
    const uintptr_t factoryAddress =
        FindClientPattern(CreateFreeCameraPattern);
    if (!managerVtable || !switchAddress || !factoryAddress) return false;

    const uintptr_t oldCamera = Read<uintptr_t>(manager + 0x28);
    if (!oldCamera) return false;
    freeCameraStartAngles = {
        Read<float>(oldCamera + 0x44),
        Read<float>(oldCamera + 0x48),
        Read<float>(oldCamera + 0x4C)};
    freeCameraStartAnglesReady =
        std::isfinite(freeCameraStartAngles.x) &&
        std::isfinite(freeCameraStartAngles.y) &&
        std::isfinite(freeCameraStartAngles.z);
    // The ESP view-matrix origin is not guaranteed to be the active
    // third-person camera origin. Copy the live player's camera object before
    // switching cameras; this preserves the exact position above the pawn and
    // the direction the player is currently looking.
    Vector3 thirdPersonStart = Read<Vector3>(oldCamera + 0x38);
    bool thirdPersonStartReady =
        std::isfinite(thirdPersonStart.x) &&
        std::isfinite(thirdPersonStart.y) &&
        std::isfinite(thirdPersonStart.z);

    if (builtInFreeCamera) {
        if (!EnsureFreeCameraUpdateHook(builtInFreeCamera))
            return false;
        cameraBeforeFreeCamera = oldCamera;
        reinterpret_cast<SwitchCameraFn>(switchAddress)(
            manager, builtInFreeCamera, 0.0f);
        builtInFreeCameraActive =
            Read<uintptr_t>(manager + 0x28) == builtInFreeCamera;
        return builtInFreeCameraActive;
    }

    auto createCamera =
        reinterpret_cast<CreateFreeCameraFn>(factoryAddress);
    const uintptr_t newCamera = createCamera(manager);
    if (!newCamera) return false;

    if (!EnsureFreeCameraUpdateHook(newCamera)) {
        const uintptr_t vtable = Read<uintptr_t>(newCamera);
        const uintptr_t destructor = Read<uintptr_t>(vtable);
        if (destructor)
            reinterpret_cast<DestroyCameraFn>(destructor)(newCamera, 1);
        return false;
    }

    if (thirdPersonStartReady)
        Write<Vector3>(newCamera + 0x38, thirdPersonStart);
    if (freeCameraStartAnglesReady) {
        Write<float>(newCamera + 0xC4, freeCameraStartAngles.x);
        Write<float>(newCamera + 0xCC, freeCameraStartAngles.y);
        Write<float>(newCamera + 0x44, freeCameraStartAngles.x);
        Write<float>(newCamera + 0x48, freeCameraStartAngles.y);
        Write<float>(newCamera + 0x4C, freeCameraStartAngles.z);
    }

    auto switchCamera = reinterpret_cast<SwitchCameraFn>(switchAddress);
    // SwitchCamera normally destroys the outgoing camera unless it matches
    // manager + 0x20. Protect the exact active player camera only for the
    // duration of this switch, then restore the manager's original field.
    protectedCameraBeforeFreeCamera = Read<uintptr_t>(manager + 0x20);
    const bool protectPlayerCamera =
        oldCamera != protectedCameraBeforeFreeCamera;
    if (protectPlayerCamera)
        Write<uintptr_t>(manager + 0x20, oldCamera);
    switchCamera(manager, newCamera, 0.0f);
    if (protectPlayerCamera)
        Write<uintptr_t>(
            manager + 0x20, protectedCameraBeforeFreeCamera);
    if (Read<uintptr_t>(manager + 0x28) != newCamera) {
        const uintptr_t vtable = Read<uintptr_t>(newCamera);
        const uintptr_t destructor = Read<uintptr_t>(vtable);
        if (destructor)
            reinterpret_cast<DestroyCameraFn>(destructor)(newCamera, 1);
        return false;
    }

    cameraBeforeFreeCamera = oldCamera;
    builtInFreeCamera = newCamera;
    builtInFreeCameraActive = true;
    if (thirdPersonStartReady) {
        // Preserve the exact third-person camera position from immediately
        // before the switch, including its current distance and offset.
        freeCameraAnchor = thirdPersonStart;
    } else {
        Vector3 playerOrigin = currentLocalPosition;
        if (!currentLocalPositionReady && currentLocalPawn)
            GetEntityPosition(currentLocalPawn, playerOrigin);
        freeCameraAnchor = playerOrigin;
        freeCameraAnchor.z += 64.0f;
    }
    freeCameraAnchorReady =
        std::isfinite(freeCameraAnchor.x) &&
        std::isfinite(freeCameraAnchor.y) &&
        std::isfinite(freeCameraAnchor.z);
    freeCameraStartPending = freeCameraAnchorReady;
    return true;
}

void DeactivateBuiltInFreeCamera() {
    std::lock_guard<std::mutex> lifecycleLock(freeCameraLifecycleMutex);
    freeCameraUserCmd.store(0, std::memory_order_release);
    lastFreeCameraUpdate = {};
    freeCameraAnchorReady = false;
    freeCameraStartPending = false;
    freeCameraStartAnglesReady = false;
    if (!builtInFreeCamera) {
        builtInFreeCameraActive = false;
        cameraBeforeFreeCamera = 0;
        protectedCameraBeforeFreeCamera = 0;
        return;
    }

    const uintptr_t manager = ResolveCitadelCameraManager();
    if (!manager) {
        builtInFreeCamera = 0;
        cameraBeforeFreeCamera = 0;
        protectedCameraBeforeFreeCamera = 0;
        builtInFreeCameraActive = false;
        return;
    }
    const uintptr_t managerVtable = Read<uintptr_t>(manager);
    const uintptr_t switchAddress =
        Read<uintptr_t>(managerVtable + 9 * sizeof(uintptr_t));
    if (Read<uintptr_t>(manager + 0x28) == builtInFreeCamera &&
        cameraBeforeFreeCamera &&
        cameraBeforeFreeCamera != builtInFreeCamera &&
        switchAddress) {
        reinterpret_cast<SwitchCameraFn>(switchAddress)(
            manager, cameraBeforeFreeCamera, 0.0f);
    }

    // SwitchCamera owns the outgoing camera lifetime and may destroy it before
    // returning. Never call its destructor a second time, and discard our
    // pointer once the manager has completed the switch.
    if (Read<uintptr_t>(manager + 0x28) != builtInFreeCamera)
        builtInFreeCamera = 0;
    cameraBeforeFreeCamera = 0;
    protectedCameraBeforeFreeCamera = 0;
    builtInFreeCameraActive = false;
}

bool IsKeyboardLayoutModifier(int key) {
    return key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
}

BOOL WINAPI hkSetCursorPos(int x, int y) {
    // Source 2 re-enters relative-mouse mode after an Alt-Tab and then warps
    // the OS cursor to the client centre every frame. The menu uses the real
    // absolute cursor, so acknowledge those warps without applying them.
    if (menuOpen) return TRUE;
    return originalSetCursorPos ? originalSetCursorPos(x, y) : FALSE;
}

BOOL WINAPI hkClipCursor(const RECT* rect) {
    // The game also restores its centre-sized cursor clip on activation.
    NotifyGameCursorCapture(rect != nullptr);
    if (menuOpen && rect) return TRUE;
    return originalClipCursor ? originalClipCursor(rect) : FALSE;
}

SHORT WINAPI hkGetAsyncKeyState(int key) {
    if (!IsGameFocused() || (menuOpen && !IsKeyboardLayoutModifier(key)) ||
        (freeCamActive && IsFreeCameraMovementKey(key))) return 0;
    if (MovementReplayVirtualKeyDown(key))
        return static_cast<SHORT>(0x8001);
    const SHORT result =
        originalGetAsyncKeyState ? originalGetAsyncKeyState(key) : 0;
#ifdef DLL6_MOVEMENT_ONLY
    CaptureLocalMovementRawKeyEvent(key, (result & 0x8000) != 0);
#endif
    return result;
}

SHORT WINAPI hkGetKeyState(int key) {
    if (!IsGameFocused() || (menuOpen && !IsKeyboardLayoutModifier(key)) ||
        (freeCamActive && IsFreeCameraMovementKey(key))) return 0;
    if (MovementReplayVirtualKeyDown(key))
        return static_cast<SHORT>(0x8001);
    const SHORT result = originalGetKeyState ? originalGetKeyState(key) : 0;
#ifdef DLL6_MOVEMENT_ONLY
    CaptureLocalMovementRawKeyEvent(key, (result & 0x8000) != 0);
#endif
    return result;
}

BOOL WINAPI hkGetKeyboardState(PBYTE state) {
    if (!IsGameFocused()) {
        if (state) ZeroMemory(state, 256);
        return TRUE;
    }
    const BOOL result =
        originalGetKeyboardState ? originalGetKeyboardState(state) : FALSE;
    if (menuOpen) {
        if (state) {
            const BYTE shift = state[VK_SHIFT];
            const BYTE leftShift = state[VK_LSHIFT];
            const BYTE rightShift = state[VK_RSHIFT];
            const BYTE alt = state[VK_MENU];
            const BYTE leftAlt = state[VK_LMENU];
            const BYTE rightAlt = state[VK_RMENU];
            ZeroMemory(state, 256);
            state[VK_SHIFT] = shift;
            state[VK_LSHIFT] = leftShift;
            state[VK_RSHIFT] = rightShift;
            state[VK_MENU] = alt;
            state[VK_LMENU] = leftAlt;
            state[VK_RMENU] = rightAlt;
        }
        return TRUE;
    }
#ifdef DLL6_MOVEMENT_ONLY
    if (result && state && localMovementRecording) {
        constexpr int recordedKeys[]{
            'W', 'S', 'A', 'D', VK_SPACE, VK_LCONTROL, VK_LSHIFT};
        for (const int key : recordedKeys)
            CaptureLocalMovementRawKeyEvent(
                key, (state[key] & 0x80) != 0);
    }
#endif
    if (result && state && (movementReplayActive || movementReplayCalibrating)) {
        constexpr int replayKeys[]{
            'W', 'S', 'A', 'D', VK_SPACE,
            VK_CONTROL, VK_LCONTROL, VK_RCONTROL,
            VK_SHIFT, VK_LSHIFT, VK_RSHIFT};
        for (const int key : replayKeys) {
            if (MovementReplayVirtualKeyDown(key))
                state[key] = static_cast<BYTE>(state[key] | 0x80);
            else
                state[key] = static_cast<BYTE>(state[key] & 0x7F);
        }
    }
    if (result && freeCamActive && state) {
        constexpr int blockedKeys[] = {
            'W', 'S', 'A', 'D', VK_SPACE, VK_CONTROL, VK_LCONTROL,
            VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT};
        for (const int key : blockedKeys)
            state[key] = 0;
    }
    return result;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    const UINT result = originalGetRawInputData
        ? originalGetRawInputData(handle, command, data, size, headerSize)
        : static_cast<UINT>(-1);
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER)) {
        const auto* input = static_cast<const RAWINPUT*>(data);
        if (input->header.dwType == RIM_TYPEKEYBOARD)
            UpdateGameTextInputKey(input->data.keyboard.VKey,
                (input->data.keyboard.Flags & RI_KEY_BREAK) == 0);
    }
    if (menuOpen && command == RID_INPUT && data &&
        result != static_cast<UINT>(-1) && result >= sizeof(RAWINPUTHEADER)) {
        auto* input = static_cast<RAWINPUT*>(data);
        if (input->header.dwType == RIM_TYPEMOUSE) {
            input->data.mouse.lLastX = 0;
            input->data.mouse.lLastY = 0;
            input->data.mouse.usButtonFlags = 0;
            input->data.mouse.ulRawButtons = 0;
        } else if (input->header.dwType == RIM_TYPEKEYBOARD &&
                   !IsKeyboardLayoutModifier(input->data.keyboard.VKey)) {
            input->data.keyboard.Flags |= RI_KEY_BREAK;
            input->data.keyboard.Message = WM_KEYUP;
        }
    }
#ifdef DLL6_MOVEMENT_ONLY
    if (command == RID_INPUT && data && result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER)) {
        auto* input = static_cast<RAWINPUT*>(data);
        if (input->header.dwType == RIM_TYPEKEYBOARD) {
            CaptureLocalMovementRawKeyEvent(
                input->data.keyboard.VKey,
                (input->data.keyboard.Flags & RI_KEY_BREAK) == 0);
        }
    }
#endif
    if (freeCamActive && command == RID_INPUT && data &&
        result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER)) {
        auto* input = static_cast<RAWINPUT*>(data);
        if (input->header.dwType == RIM_TYPEKEYBOARD &&
            (!IsGameFocused() || freeCamActive) &&
            IsFreeCameraMovementKey(input->data.keyboard.VKey)) {
            input->data.keyboard.Flags |= RI_KEY_BREAK;
            input->data.keyboard.Message = WM_KEYUP;
        }
    }
#ifdef DLL6_MOVEMENT_ONLY
    if ((localMovementPlaybackActive || localMovementPlaybackCalibrating) &&
        command == RID_INPUT && data && result != static_cast<UINT>(-1) &&
        result >= sizeof(RAWINPUTHEADER)) {
        auto* input = static_cast<RAWINPUT*>(data);
        if (input->header.dwType == RIM_TYPEMOUSE) {
            input->data.mouse.lLastX = 0;
            input->data.mouse.lLastY = 0;
        }
    }
#endif
    return result;
}

UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    const UINT result = originalGetRawInputBuffer
        ? originalGetRawInputBuffer(data, size, headerSize)
        : static_cast<UINT>(-1);
    if (data && result != static_cast<UINT>(-1)) {
        const RAWINPUT* input = data;
        for (UINT index = 0; index < result; ++index) {
            if (input->header.dwType == RIM_TYPEKEYBOARD)
                UpdateGameTextInputKey(input->data.keyboard.VKey,
                    (input->data.keyboard.Flags & RI_KEY_BREAK) == 0);
            const UINT alignedSize = (input->header.dwSize + 7u) & ~7u;
            input = reinterpret_cast<const RAWINPUT*>(
                reinterpret_cast<const uint8_t*>(input) + alignedSize);
        }
    }
    if (menuOpen && data && result != static_cast<UINT>(-1)) {
        RAWINPUT* input = data;
        for (UINT index = 0; index < result; ++index) {
            if (input->header.dwType == RIM_TYPEMOUSE) {
                input->data.mouse.lLastX = 0;
                input->data.mouse.lLastY = 0;
                input->data.mouse.usButtonFlags = 0;
                input->data.mouse.ulRawButtons = 0;
            } else if (input->header.dwType == RIM_TYPEKEYBOARD &&
                       !IsKeyboardLayoutModifier(input->data.keyboard.VKey)) {
                input->data.keyboard.Flags |= RI_KEY_BREAK;
                input->data.keyboard.Message = WM_KEYUP;
            }
            const UINT alignedSize = (input->header.dwSize + 7u) & ~7u;
            input = reinterpret_cast<RAWINPUT*>(
                reinterpret_cast<uint8_t*>(input) + alignedSize);
        }
    }
    if (freeCamActive && data && result != static_cast<UINT>(-1)) {
        RAWINPUT* input = data;
        for (UINT i = 0; i < result; ++i) {
            if (input->header.dwType == RIM_TYPEKEYBOARD &&
                (!IsGameFocused() || freeCamActive) &&
                IsFreeCameraMovementKey(input->data.keyboard.VKey)) {
                input->data.keyboard.Flags |= RI_KEY_BREAK;
                input->data.keyboard.Message = WM_KEYUP;
            }
            const UINT alignedSize =
                (input->header.dwSize + 7u) & ~7u;
            input = reinterpret_cast<RAWINPUT*>(
                reinterpret_cast<uint8_t*>(input) + alignedSize);
        }
    }
#ifdef DLL6_MOVEMENT_ONLY
    if (data && result != static_cast<UINT>(-1)) {
        RAWINPUT* input = data;
        for (UINT i = 0; i < result; ++i) {
            if (input->header.dwType == RIM_TYPEKEYBOARD) {
                CaptureLocalMovementRawKeyEvent(
                    input->data.keyboard.VKey,
                    (input->data.keyboard.Flags & RI_KEY_BREAK) == 0);
            } else if ((localMovementPlaybackActive ||
                        localMovementPlaybackCalibrating) &&
                       input->header.dwType == RIM_TYPEMOUSE) {
                input->data.mouse.lLastX = 0;
                input->data.mouse.lLastY = 0;
            }
            const UINT alignedSize = (input->header.dwSize + 7u) & ~7u;
            input = reinterpret_cast<RAWINPUT*>(
                reinterpret_cast<uint8_t*>(input) + alignedSize);
        }
    }
#endif
    return result;
}

uintptr_t GetCurrentController() {
    if (!currentLocalPawn) return 0;
    const uint32_t handle = Read<uint32_t>(currentLocalPawn + Offsets::PawnController);
    if (handle == 0xFFFFFFFFu) return 0;
    return ResolveEntity(handle);
}

uintptr_t GetCurrentUserCmd() {
    if (!runtimeUserCmdFunctions.HasInputPath()) return 0;
    const uintptr_t controller = GetCurrentController();
    if (!controller) return 0;

    auto getTick = reinterpret_cast<GetUserCmdTickFn>(runtimeUserCmdFunctions.getUserCmdTick);
    auto getArray = reinterpret_cast<GetUserCmdArrayFn>(runtimeUserCmdFunctions.getUserCmdArray);
    auto getBySequence = reinterpret_cast<GetUserCmdBySequenceFn>(runtimeUserCmdFunctions.getUserCmdBySequence);

    int32_t outputTick = 0;
    getTick(controller, &outputTick);
    const int32_t tick = outputTick == -1 ? -1 : outputTick - 1;
    const uintptr_t firstArray = Read<uintptr_t>(runtimeUserCmdFunctions.firstUserCmdArrayGlobal);
    if (!firstArray) return 0;
    const uintptr_t array = getArray(firstArray, tick);
    if (!array) return 0;

    // IDA-confirmed current client layout: CreateMove reads the command
    // sequence from CUserCmdArray + 0x6270 in this build.
    const uint32_t sequence = Read<uint32_t>(array + 0x6270);
    if (!sequence) return 0;
    return getBySequence(controller, sequence);
}

void PatchInputHistory(uintptr_t userCmd, const Vector3& angles);

int RotateSubtickMovement(CBaseUserCmdPB* base, float cosine, float sine) {
    if (!base) return 0;

    // CreateMove stores changes of the analog movement vector in the
    // already-created CSubtickMoveStep messages. Rotating only
    // CBaseUserCmdPB::forwardmove/leftmove is therefore undone as those
    // subticks are replayed. Rotate every existing delta by the same basis
    // change. Do not append protobuf messages: this process and the game use
    // different descriptor pools.
    const int count = base->subtick_moves_size();
    if (count <= 0 || count > 64) return 0;

    int patched = 0;
    for (int i = 0; i < count; ++i) {
        auto* step = base->mutable_subtick_moves(i);
        if (!step ||
            (!step->has_analog_forward_delta() &&
             !step->has_analog_left_delta())) {
            continue;
        }

        const float forward = step->has_analog_forward_delta()
            ? step->analog_forward_delta()
            : 0.0f;
        const float left = step->has_analog_left_delta()
            ? step->analog_left_delta()
            : 0.0f;
        if (!std::isfinite(forward) || !std::isfinite(left))
            continue;

        // Source uses a positive "leftmove" axis. Convert the movement vector
        // from the visible-camera basis into the silent-yaw basis.
        step->set_analog_forward_delta(
            forward * cosine - left * sine);
        step->set_analog_left_delta(
            forward * sine + left * cosine);
        ++patched;
    }
    return patched;
}

void RemapDigitalMovementForSilentCommand(CUserCmd* command,
                                          CBaseUserCmdPB* base,
                                          float forward,
                                          float left) {
    if (!command || !base) return;

    const std::uint64_t movementMask =
        static_cast<std::uint64_t>(InputBitMask::Forward) |
        static_cast<std::uint64_t>(InputBitMask::Back) |
        static_cast<std::uint64_t>(InputBitMask::MoveLeft) |
        static_cast<std::uint64_t>(InputBitMask::MoveRight);
    std::uint64_t correctedMask = 0;
    constexpr float MovementEpsilon = 0.001f;
    if (forward > MovementEpsilon)
        correctedMask |= static_cast<std::uint64_t>(InputBitMask::Forward);
    else if (forward < -MovementEpsilon)
        correctedMask |= static_cast<std::uint64_t>(InputBitMask::Back);
    if (left > MovementEpsilon)
        correctedMask |= static_cast<std::uint64_t>(InputBitMask::MoveLeft);
    else if (left < -MovementEpsilon)
        correctedMask |= static_cast<std::uint64_t>(InputBitMask::MoveRight);

    // Source 2 can rebuild the analog vector from held buttons after reading
    // forwardmove/leftmove. Preserve each button-state phase, but replace its
    // movement bits with the directions of the corrected vector. Clearing all
    // four bits makes held keyboard movement stop completely.
    const auto remapState = [movementMask, correctedMask](std::uint64_t state) {
        const bool carriedMovement = (state & movementMask) != 0;
        return (state & ~movementMask) |
            (carriedMovement ? correctedMask : 0);
    };
    command->buttonStates.buttonState1 =
        remapState(command->buttonStates.buttonState1);
    command->buttonStates.buttonState2 =
        remapState(command->buttonStates.buttonState2);
    command->buttonStates.buttonState3 =
        remapState(command->buttonStates.buttonState3);

    if (base->has_buttons_pb()) {
        auto* buttons = base->mutable_buttons_pb();
        buttons->set_buttonstate1(remapState(buttons->buttonstate1()));
        buttons->set_buttonstate2(remapState(buttons->buttonstate2()));
        buttons->set_buttonstate3(remapState(buttons->buttonstate3()));
    }

    const int count = base->subtick_moves_size();
    if (count <= 0 || count > 64) return;
    for (int i = 0; i < count; ++i) {
        auto* step = base->mutable_subtick_moves(i);
        if (!step || !step->has_button()) continue;
        if ((step->button() & movementMask) != 0) {
            step->set_button(
                (step->button() & ~movementMask) | correctedMask);
        }
    }
}

bool UserCmdHasAttack(const CUserCmd* command) {
    if (!command) return false;
    const auto attackMask = static_cast<std::uint64_t>(InputBitMask::Attack);
    if ((command->buttonStates.buttonState1 & attackMask) != 0) return true;
    if (!command->cmd.has_base()) return false;
    const auto& base = command->cmd.base();
    return base.has_buttons_pb() &&
        (base.buttons_pb().buttonstate1() & attackMask) != 0;
}

bool UserCmdHasAnyMask(const CUserCmd* command, std::uint64_t mask) {
    if (!command || !mask) return false;
    if (((command->buttonStates.buttonState1 |
          command->buttonStates.buttonState2 |
          command->buttonStates.buttonState3) & mask) != 0) {
        return true;
    }
    if (!command->cmd.has_base() ||
        !command->cmd.base().has_buttons_pb()) {
        return false;
    }
    const auto& buttons = command->cmd.base().buttons_pb();
    return ((buttons.buttonstate1() |
             buttons.buttonstate2() |
             buttons.buttonstate3()) & mask) != 0;
}

bool GetPendingSilentInputAngle(Vector3& angles) {
    const bool physicalAttack = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (autoLastHitOrbs) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        if (pendingOrbReady && (pendingOrbAttack || physicalAttack)) {
            angles = pendingOrbAngles;
            return true;
        }
    }
    if ((farmSilentMode || farmMixedMode) && physicalAttack) {
        std::lock_guard<std::mutex> lock(creepSilentMutex);
        if (pendingCreepReady) {
            angles = pendingCreepAngles;
            return true;
        }
    }
    if (aimSilentActive && physicalAttack) {
        std::lock_guard<std::mutex> lock(humanSilentMutex);
        if (pendingHumanReady) {
            angles = pendingHumanAngles;
            return true;
        }
    }
    return false;
}

void ApplyPendingUserCmdAngles(uintptr_t userCmd) {
    if (!userCmd) return;
    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    const auto attackMask = static_cast<std::uint64_t>(InputBitMask::Attack);

    // Silent must not rotate ordinary movement commands. The native button
    // state is populated by the game's real input path before CreateMove.
    const bool commandHasAttack = UserCmdHasAttack(command);

    Vector3 angles{};
    float originalPitch = 0.0f;
    bool originalPitchReady = false;
    bool attack = false;
    bool ready = false;
    {
        // A hero ability owns the aim angle of the command that activates it.
        // Otherwise held primary fire replaces Haze's queued dagger angle
        // before the command is sent.
        constexpr std::uint64_t HeroAbilityAimMask =
            0x0000000200000000ull | // Ability 1 (Haze/Shiv)
            0x0000000400000000ull | // Ability 2 (Drifter)
            0x0000000800000000ull | // Ability 3 (Bebop)
            0x0000001000000000ull;  // Ability 4 (Vindicta)
        const bool heroAbilityCommand =
            UserCmdHasAnyMask(command, HeroAbilityAimMask);
        {
            std::lock_guard<std::mutex> lock(silentAnglesMutex);
            if (!ready && pendingSilentAnglesReady &&
                (heroAbilityCommand ||
                 pendingHeroSilentOverridesPrimary)) {
                angles = pendingSilentAngles;
                attack = pendingSilentAttack;
                pendingSilentAttack = false;
                ready = true;
            }
        }

        // A script-generated ability press owns its generated shot, but a
        // merely cached preview angle must not hijack primary fire.
        if (!ready) {
            std::lock_guard<std::mutex> lock(silentAnglesMutex);
            if (pendingSilentAnglesReady && pendingSilentAttack) {
                angles = pendingSilentAngles;
                attack = true;
                pendingSilentAttack = false;
                ready = true;
            }
        }
        // A live orb temporarily owns primary fire, including a shot the
        // player was already holding before the orb appeared.
        if (!ready && autoLastHitOrbs) {
            std::lock_guard<std::mutex> lock(orbSilentMutex);
            if (pendingOrbReady && (pendingOrbAttack || commandHasAttack)) {
                angles = pendingOrbAngles;
                attack = pendingOrbAttack;
                if (!pendingOrbHoldAttack)
                    pendingOrbAttack = false;
                ready = true;
            }
        }
        // While Creep Aim has a live target it owns primary fire in every
        // mode. Player, orb and cached hero angles must not overwrite it.
        if (!ready && (farmNormalActive || farmSilentMode || farmMixedMode) &&
            commandHasAttack) {
            std::lock_guard<std::mutex> lock(creepSilentMutex);
            if (pendingCreepReady) {
                angles = pendingCreepAngles;
                ready = true;
            }
        }
        if (!ready && aimSilentActive && commandHasAttack) {
            std::lock_guard<std::mutex> lock(humanSilentMutex);
            if (pendingHumanReady) {
                angles = pendingHumanAngles;
                ready = true;
            }
        }
    }
    if (!ready) return;

    if (command->cmd.has_ang_camera_angles()) {
        originalPitch = command->cmd.ang_camera_angles().x();
        originalPitchReady = std::isfinite(originalPitch);
    }

    // The local pawn has already consumed this command using the visible
    // camera yaw. Silent changes the outgoing yaw below, so express the same
    // world-space movement in the silent-yaw basis. This preserves
    // camera-relative movement without synthesizing keyboard input.
    bool movementCorrected = false;
    const PawnUserCmdSnapshot pawnSnapshot = ReadLatestPawnUserCmd();
    if (kSilentCommandMovementCorrection && command->cmd.has_base() &&
        command->cmd.has_ang_camera_angles() &&
        std::isfinite(angles.y)) {
        auto* base = command->cmd.mutable_base();
        float cameraYaw = command->cmd.ang_camera_angles().y();
        Vector3 cameraForward{};
        if (GetCurrentCameraForward(cameraForward)) {
            const float horizontal = std::hypot(
                cameraForward.x, cameraForward.y);
            if (std::isfinite(horizontal) && horizontal > 0.001f) {
                cameraYaw = std::atan2(cameraForward.y, cameraForward.x) *
                    57.29577951308232f;
            }
        }
        if (base && std::isfinite(cameraYaw)) {
            const bool forwardDown =
                (GetAsyncKeyState('W') & 0x8000) != 0;
            const bool backDown =
                (GetAsyncKeyState('S') & 0x8000) != 0;
            const bool leftDown =
                (GetAsyncKeyState('A') & 0x8000) != 0;
            const bool rightDown =
                (GetAsyncKeyState('D') & 0x8000) != 0;
            const bool keyboardMovement =
                forwardDown || backDown || leftDown || rightDown;
            const float sourceForward = keyboardMovement
                ? (forwardDown ? 1.0f : 0.0f) -
                    (backDown ? 1.0f : 0.0f)
                : base->forwardmove();
            const float sourceLeft = keyboardMovement
                ? (leftDown ? 1.0f : 0.0f) -
                    (rightDown ? 1.0f : 0.0f)
                : base->leftmove();
            const float radians =
                (cameraYaw - angles.y) *
                0.017453292519943295f;
            const float cosine = std::cos(radians);
            const float sine = std::sin(radians);
            const float correctedForward =
                sourceForward * cosine -
                sourceLeft * sine;
            const float correctedLeft =
                sourceForward * sine +
                sourceLeft * cosine;

            base->set_forwardmove(correctedForward);
            base->set_leftmove(correctedLeft);
            if (!keyboardMovement)
                RotateSubtickMovement(base, cosine, sine);
            RemapDigitalMovementForSilentCommand(
                command, base, correctedForward, correctedLeft);
            movementCorrected = true;
        }
    }

    static ULONGLONG lastCorrelationLog = 0;
    const ULONGLONG correlationNow = GetTickCount64();
    if (kRuntimeDiagnostics && correlationNow - lastCorrelationLog >= 100) {
        lastCorrelationLog = correlationNow;
        static std::mutex correlationLogMutex;
        std::lock_guard<std::mutex> lock(correlationLogMutex);
        std::ofstream log(
            Dll6Paths::DataFileA("movement_correlation_runtime.log"),
            std::ios::app);
        if (log) {
            log << "current=0x" << std::hex << userCmd
                << " pawn=0x" << pawnSnapshot.userCmd << std::dec
                << " match=" << (pawnSnapshot.userCmd == userCmd)
                << " valid=" << pawnSnapshot.valid
                << " corrected=" << movementCorrected
                << " cameraYaw=" << pawnSnapshot.cameraYaw
                << " silentYaw=" << angles.y
                << " source=" << pawnSnapshot.forward << ','
                << pawnSnapshot.left;
            if (command->cmd.has_base()) {
                log << " result=" << command->cmd.base().forwardmove() << ','
                    << command->cmd.base().leftmove();
            }
            log << '\n';
        }
    }

    if (auto* viewAngles = command->cmd.mutable_ang_camera_angles()) {
        if (aimOnlyYaw && aimSilentActive && originalPitchReady)
            angles.x = originalPitch;
        viewAngles->set_x(angles.x);
        viewAngles->set_y(angles.y);
        viewAngles->set_z(angles.z);
    }
    command->cmd.clear_view_delta_x();
    command->cmd.clear_view_delta_y();
    PatchInputHistory(userCmd, angles);
    if (attack) {
        // Do not synthesize OS mouse input. Also do not append protobuf child
        // messages here: the game and this DLL have different descriptor
        // pools, which makes add_subtick_moves() fatal. Set the native and
        // already-existing protobuf button state only.
        command->buttonStates.buttonState1 |= attackMask;
        command->buttonStates.buttonState2 |= attackMask;
        command->buttonStates.buttonState3 |= attackMask;
        if (auto* base = command->cmd.mutable_base()) {
            if (auto* buttons = base->mutable_buttons_pb()) {
                buttons->set_buttonstate1(buttons->buttonstate1() | attackMask);
                buttons->set_buttonstate2(buttons->buttonstate2() | attackMask);
                buttons->set_buttonstate3(buttons->buttonstate3() | attackMask);
            }
        }
        lastSilentAttackAppliedAt = GetTickCount64();
        if (autoLastHitOrbs) InterlockedIncrement(&autoOrbAttackAppliedCount);
    }
    ++silentAppliedCalls;
}

void CorrectMovementBeforeLocalApply(uintptr_t input, uintptr_t userCmd) {
    if (!input || !userCmd) return;

    Vector3 silentAngles{};
    if (!GetPendingSilentInputAngle(silentAngles) ||
        !std::isfinite(silentAngles.y) ||
        std::fabs(silentAngles.y) > 360.0f) {
        return;
    }

    constexpr uintptr_t InputViewAnglesOffset = 0x688;
    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    // Use the same CInput angle that the native input path uses to build and
    // consume movement. The protobuf angle can already be a stale/replayed
    // command angle while silent aim is active.
    float cameraYaw = Read<Vector3>(input + InputViewAnglesOffset).y;
    bool validCameraYaw = std::isfinite(cameraYaw) &&
        std::fabs(cameraYaw) <= 360.0f;
    if (!validCameraYaw && command->cmd.has_ang_camera_angles()) {
        cameraYaw = command->cmd.ang_camera_angles().y();
        validCameraYaw = std::isfinite(cameraYaw) &&
            std::fabs(cameraYaw) <= 360.0f;
    }
    if (!validCameraYaw) return;

    auto* base = command->cmd.mutable_base();
    if (!base) return;

    // Keyboard movement is carried primarily by digital button bits here;
    // forwardmove/leftmove can legitimately still be zero. Reconstruct the
    // visible-camera vector from the physical keys before removing those bits.
    const bool forwardDown = (GetAsyncKeyState('W') & 0x8000) != 0;
    const bool backDown = (GetAsyncKeyState('S') & 0x8000) != 0;
    const bool leftDown = (GetAsyncKeyState('A') & 0x8000) != 0;
    const bool rightDown = (GetAsyncKeyState('D') & 0x8000) != 0;
    const bool keyboardMovement =
        forwardDown || backDown || leftDown || rightDown;
    const float sourceForward = keyboardMovement
        ? (forwardDown ? 1.0f : 0.0f) - (backDown ? 1.0f : 0.0f)
        : base->forwardmove();
    const float sourceLeft = keyboardMovement
        ? (leftDown ? 1.0f : 0.0f) - (rightDown ? 1.0f : 0.0f)
        : base->leftmove();
    if (!std::isfinite(sourceForward) || !std::isfinite(sourceLeft)) return;

    const float radians =
        (cameraYaw - silentAngles.y) * 0.017453292519943295f;
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float correctedForward =
        sourceForward * cosine - sourceLeft * sine;
    const float correctedLeft =
        sourceForward * sine + sourceLeft * cosine;

    static ULONGLONG lastMovementApplyLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (kRuntimeDiagnostics && now - lastMovementApplyLog >= 100) {
        lastMovementApplyLog = now;
        static std::mutex movementApplyLogMutex;
        std::lock_guard<std::mutex> lock(movementApplyLogMutex);
        std::ofstream log(
            Dll6Paths::DataFileA("movement_apply_runtime.log"),
            std::ios::app);
        if (log) {
            log << "hook=1 input=0x" << std::hex << input
                << " cmd=0x" << userCmd << std::dec
                << " cameraYaw=" << cameraYaw
                << " silentYaw=" << silentAngles.y
                << " keyboard=" << keyboardMovement
                << " source=" << sourceForward << ',' << sourceLeft
                << " corrected=" << correctedForward << ',' << correctedLeft
                << '\n';
        }
    }

    // Never write the corrected vector back to CInput. A held key does not
    // necessarily rebuild those floats every command, so doing that rotates
    // an already-corrected vector again on the next tick. Change only this
    // freshly built command before the local movement consumer sees it.
    base->set_forwardmove(correctedForward);
    base->set_leftmove(correctedLeft);
    if (!keyboardMovement)
        RotateSubtickMovement(base, cosine, sine);
    // The engine can rebuild movement from held W/A/S/D after reading the
    // analog vector. Remove those bits only from this silent command so the
    // corrected vector is not replaced in the hidden-yaw basis.
    RemapDigitalMovementForSilentCommand(
        command, base, correctedForward, correctedLeft);
}

void __fastcall hkApplyInputCommand(uintptr_t input, uintptr_t userCmd) {
    // This is the last input stage before local movement consumes the command.
    // Applying here makes Normal/Mixed rotate the actual in-game camera while
    // keeping the OS cursor untouched.
    bool heroScriptModified = false;
#ifdef DLL6_MOVEMENT_ONLY
    // Citadel's callback finalizes analog axes, button transitions and input
    // history. Run it first for recording and playback, then patch the fully
    // built replay command before CreateMove hands it to the local pawn.
    const bool replayWasActive =
        localMovementPlaybackActive || localMovementPlaybackCalibrating;
    if (originalApplyInputCommand)
        originalApplyInputCommand(input, userCmd);
    const bool movementReplayModified = ProcessMovementReplayUserCmd(
        reinterpret_cast<CUserCmd*>(userCmd), input);
    if (replayWasActive && movementReplayModified) {
        auto* command = reinterpret_cast<CUserCmd*>(userCmd);
        if (command && command->cmd.has_ang_camera_angles()) {
            const auto& value = command->cmd.ang_camera_angles();
            PatchInputHistory(userCmd, {value.x(), value.y(), value.z()});
        }
    }
    return;
#else
    #ifndef DLL6_MOVEMENT_ONLY
    heroScriptModified = ProcessHeroScriptsUserCmd(
        reinterpret_cast<CUserCmd*>(userCmd), true, input);
    // Hero abilities publish into the same pending-angle channel as ordinary
    // pSilent. Process them first so angle and Ability button are written to
    // this exact command and its input history together.
    ApplyPendingUserCmdAngles(userCmd);
#endif
    const bool movementReplayModified = ProcessMovementReplayUserCmd(
        reinterpret_cast<CUserCmd*>(userCmd), input);
    if (heroScriptModified || movementReplayModified) {
        auto* command = reinterpret_cast<CUserCmd*>(userCmd);
        if (command && command->cmd.has_ang_camera_angles()) {
            const auto& value = command->cmd.ang_camera_angles();
            PatchInputHistory(userCmd, {value.x(), value.y(), value.z()});
        }
    }
    if (originalApplyInputCommand)
        originalApplyInputCommand(input, userCmd);
    // The stock callback finalizes button states. Filter only after it returns,
    // otherwise it can restore Space into the command before the local pawn
    // consumes it.
    ApplyBunnyHop(userCmd);
#endif
}

bool EnsureApplyInputCommandHook(uintptr_t input) {
    if (applyInputCommandHookInstalled && originalApplyInputCommand)
        return true;
    if (!input) return false;

    const uintptr_t vtable = Read<uintptr_t>(input);
    if (!vtable) return false;
    const uintptr_t target =
        Read<uintptr_t>(vtable + 6 * sizeof(uintptr_t));
    if (!target) return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }
    void* targetAddress = reinterpret_cast<void*>(target);
    const MH_STATUS createStatus = MH_CreateHook(
        targetAddress, reinterpret_cast<void*>(&hkApplyInputCommand),
        reinterpret_cast<void**>(&originalApplyInputCommand));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(targetAddress);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        return false;

    applyInputCommandTarget = targetAddress;
    applyInputCommandHookInstalled = true;
    return true;
}

void LogPawnUserCmd(const char* phase, uintptr_t pawn, uintptr_t userCmd) {
    if constexpr (!kRuntimeDiagnostics) return;
    if (!userCmd) return;
    const auto* command = reinterpret_cast<const CUserCmd*>(userCmd);
    float forward = 0.0f;
    float left = 0.0f;
    float yaw = 0.0f;
    if (command->cmd.has_base()) {
        forward = command->cmd.base().forwardmove();
        left = command->cmd.base().leftmove();
    }
    if (command->cmd.has_ang_camera_angles())
        yaw = command->cmd.ang_camera_angles().y();

    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream log(
        Dll6Paths::DataFileA("pawn_movement_runtime.log"),
        std::ios::app);
    if (log) {
        log << phase
            << " pawn=0x" << std::hex << pawn
            << " cmd=0x" << userCmd << std::dec
            << " move=" << forward << ',' << left
            << " yaw=" << yaw << '\n';
    }
}

void __fastcall hkPawnProcessUserCmd(uintptr_t pawn, uintptr_t userCmd) {
    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    const bool shouldLog = kRuntimeDiagnostics && now - lastLog >= 100;
    if (shouldLog) {
        lastLog = now;
        LogPawnUserCmd("before", pawn, userCmd);
    }
    Vector3 silentAngles{};
    bool isolatedMovementYaw = false;
    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    if (command && command->cmd.has_ang_camera_angles() &&
        GetPendingSilentInputAngle(silentAngles)) {
        Vector3 cameraForward{};
        if (GetCurrentCameraForward(cameraForward)) {
            const float horizontal = std::hypot(
                cameraForward.x, cameraForward.y);
            if (std::isfinite(horizontal) && horizontal > 0.001f) {
                constexpr float RadToDeg = 57.29577951308232f;
                const float visibleYaw = std::atan2(
                    cameraForward.y, cameraForward.x) * RadToDeg;
                if (std::isfinite(visibleYaw)) {
                    command->cmd.mutable_ang_camera_angles()->set_y(
                        visibleYaw);
                    isolatedMovementYaw = true;
                }
            }
        }
    }

    if (originalPawnProcessUserCmd)
        originalPawnProcessUserCmd(pawn, userCmd);

    if (isolatedMovementYaw && command &&
        command->cmd.has_ang_camera_angles()) {
        auto* restored = command->cmd.mutable_ang_camera_angles();
        restored->set_x(silentAngles.x);
        restored->set_y(silentAngles.y);
        restored->set_z(silentAngles.z);
        command->cmd.clear_view_delta_x();
        command->cmd.clear_view_delta_y();
        PatchInputHistory(userCmd, silentAngles);
    }

    // Camera angles are populated by the original pawn callback. Keep the
    // latest complete command for the next ProcessMovement tick; never erase
    // a valid snapshot because an intermediate command lacks angle fields.
    if (userCmd) {
        PawnUserCmdSnapshot candidate{};
            const auto* command = reinterpret_cast<const CUserCmd*>(userCmd);
        if (command->cmd.has_base() &&
            command->cmd.has_ang_camera_angles()) {
            const auto& base = command->cmd.base();
            const float forward = base.forwardmove();
            const float left = base.leftmove();
            const float cameraYaw = command->cmd.ang_camera_angles().y();
            if (std::isfinite(forward) && std::isfinite(left) &&
                std::isfinite(cameraYaw) &&
                std::fabs(cameraYaw) <= 360.0f) {
                candidate.userCmd = userCmd;
                candidate.forward = forward;
                candidate.left = left;
                candidate.cameraYaw = cameraYaw;
                candidate.valid = true;
            }
        }
        if (candidate.valid) {
            std::lock_guard<std::mutex> snapshotLock(latestPawnUserCmdMutex);
            latestPawnUserCmd = candidate;
            movementDiagCameraYaw.store(candidate.cameraYaw,
                                        std::memory_order_relaxed);
        }
    }

    if (shouldLog)
        LogPawnUserCmd("after", pawn, userCmd);
}

bool EnsurePawnProcessUserCmdHook(uintptr_t pawn) {
    if (pawnProcessUserCmdHookInstalled && originalPawnProcessUserCmd)
        return true;
    if (!pawn) return false;
    const uintptr_t vtable = Read<uintptr_t>(pawn);
    if (!vtable) return false;
    // IDA: CreateMove passes the completed CUserCmd to the local pawn through
    // pawn->vtable[0xAA0 / 8] immediately after the input command callback.
    const uintptr_t target = Read<uintptr_t>(vtable + 0xAA0);
    if (!target) return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK &&
        initStatus != MH_ERROR_ALREADY_INITIALIZED) {
        return false;
    }
    void* targetAddress = reinterpret_cast<void*>(target);
    const MH_STATUS createStatus = MH_CreateHook(
        targetAddress, reinterpret_cast<void*>(&hkPawnProcessUserCmd),
        reinterpret_cast<void**>(&originalPawnProcessUserCmd));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(targetAddress);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED)
        return false;

    pawnProcessUserCmdTarget = targetAddress;
    pawnProcessUserCmdHookInstalled = true;
    {
        std::ofstream log(
            Dll6Paths::DataFileA("pawn_movement_runtime.log"),
            std::ios::app);
        if (log) {
            log << "installed target=0x" << std::hex << target
                << " rva=0x" << (target - clientBase) << std::dec << '\n';
        }
    }
    return true;
}

void UpdateFreeCameraCommand(uintptr_t userCmd) {
    if (!freeCam) {
        freeCamActive = false;
        DeactivateBuiltInFreeCamera();
        return;
    }
    if (!freeCamActive) {
        DeactivateBuiltInFreeCamera();
        return;
    }
    if (!userCmd || !ActivateBuiltInFreeCamera())
        return;

    freeCameraUserCmd.store(userCmd, std::memory_order_release);
    // Camera translation uses raw key state in hkFreeCameraUpdate, so the
    // outgoing command can be cleared immediately and the pawn stays still.
    ClearFreeCameraMovement(userCmd);
}

void ApplyVisibleAimInput(uintptr_t input) {
    if (!input || menuOpen || !IsGameFocused() ||
        (!aimNormalActive && !aimMixedMode) ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) return;

    std::lock_guard<std::mutex> lock(humanSilentMutex);
    if (!pendingHumanReady) return;

    constexpr uintptr_t InputViewAnglesOffset = 0x688;
    Vector3 current = Read<Vector3>(input + InputViewAnglesOffset);
    const Vector3 target = pendingHumanAngles;
    if (!std::isfinite(current.x) || !std::isfinite(current.y) ||
        !std::isfinite(target.x) || !std::isfinite(target.y)) return;

    // This is command input, not the render camera. Keep yaw controlled by
    // the regular Normal/Mixed path and replace only the recoil-affected
    // pitch with the exact selected target pitch before CreateMove builds the
    // shot command.
    current.x = target.x;
    Write<Vector3>(input + InputViewAnglesOffset, current);
}

void ApplyVisibleAimRecoilCommand(uintptr_t userCmd) {
    if (!userCmd || menuOpen || !IsGameFocused() ||
        (!aimNormalActive && !aimMixedMode) ||
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0)
        return;
    // Creep Aim publishes the complete command angle itself. Do not replace
    // its pitch with the currently cached player target.
    if (farmNormalActive) return;

    Vector3 target{};
    {
        std::lock_guard<std::mutex> lock(humanSilentMutex);
        if (!pendingHumanReady)
            return;
        target = pendingHumanAngles;
    }
    if (!std::isfinite(target.x))
        return;

    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    if (!command->cmd.has_ang_camera_angles())
        return;
    auto* viewAngles = command->cmd.mutable_ang_camera_angles();
    if (!viewAngles)
        return;

    Vector3 commandAngles{
        target.x, viewAngles->y(), viewAngles->z()};
    if (!std::isfinite(commandAngles.y) ||
        !std::isfinite(commandAngles.z))
        return;
    viewAngles->set_x(commandAngles.x);
    command->cmd.clear_view_delta_x();
    // Every subtick shot carries its own view-angle history. Patch it together
    // with the top-level command or later shots in a held burst can still use
    // the recoil pitch even though the visible crosshair is level.
    PatchInputHistory(userCmd, commandAngles);
}

std::mutex currentCameraAimMutex;
Vector3 queuedCameraAimTarget{};
bool queuedCameraAimReady = false;
ULONGLONG queuedCameraAimAt = 0;
bool queuedCameraAimUsesCustomSmoothing = false;
bool queuedCameraAimUsesDirectAngles = false;
Vector3 queuedCameraAimDirectAngles{};
float queuedCameraAimPitchSmooth = 1.0f;
float queuedCameraAimYawSmooth = 1.0f;

void ApplyCurrentCameraAimInternal(const Vector3& worldTarget) {
    if (freeCamActive || menuOpen || !IsGameFocused() ||
        !std::isfinite(worldTarget.x) || !std::isfinite(worldTarget.y) ||
        !std::isfinite(worldTarget.z))
        return;

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(currentCameraAimMutex);
        queuedCameraAimTarget = worldTarget;
        queuedCameraAimReady = true;
        queuedCameraAimAt = now;
        queuedCameraAimUsesCustomSmoothing = false;
        queuedCameraAimUsesDirectAngles = false;
    }

    // Normal and the visible half of Mixed must not become inert when the
    // gameplay-camera virtual callback is unavailable (for example while its
    // hook is being re-established after injection/camera recreation). Apply
    // the same queued target once from Present only while no callback has
    // actually arrived recently. As soon as the native callback is alive it
    // remains the sole writer, so the two paths never fight or add jitter.
    const ULONGLONG hookAt = lastGameplayCameraHookAt.load(
        std::memory_order_acquire);
    if (hookAt == 0 || now - hookAt > 100)
        FlushCurrentCameraAimInternal();
}

void ApplyHeroScriptCameraAimInternal(const Vector3& worldTarget,
                                      float pitchSmooth, float yawSmooth) {
    if (freeCamActive || menuOpen || !IsGameFocused() ||
        !std::isfinite(worldTarget.x) || !std::isfinite(worldTarget.y) ||
        !std::isfinite(worldTarget.z))
        return;

    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(currentCameraAimMutex);
        queuedCameraAimTarget = worldTarget;
        queuedCameraAimReady = true;
        queuedCameraAimAt = now;
        queuedCameraAimUsesCustomSmoothing = true;
        queuedCameraAimUsesDirectAngles = false;
        queuedCameraAimPitchSmooth = (std::clamp)(pitchSmooth, 1.0f, 20.0f);
        queuedCameraAimYawSmooth = (std::clamp)(yawSmooth, 1.0f, 20.0f);
    }

    const ULONGLONG hookAt = lastGameplayCameraHookAt.load(
        std::memory_order_acquire);
    if (hookAt == 0 || now - hookAt > 100)
        FlushCurrentCameraAimInternal();
}

void ApplyMovementReplayCameraAnglesInternal(const Vector3& angles) {
    if (freeCamActive || menuOpen || !IsGameFocused() ||
        !std::isfinite(angles.x) || !std::isfinite(angles.y))
        return;
    const ULONGLONG now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(currentCameraAimMutex);
        queuedCameraAimReady = true;
        queuedCameraAimAt = now;
        queuedCameraAimUsesCustomSmoothing = true;
        queuedCameraAimUsesDirectAngles = true;
        queuedCameraAimDirectAngles = {
            (std::clamp)(angles.x, -89.0f, 89.0f),
            NormalizeCameraAngle(angles.y), 0.0f};
        queuedCameraAimPitchSmooth = 1.0f;
        queuedCameraAimYawSmooth = 1.0f;
    }
    const ULONGLONG hookAt = lastGameplayCameraHookAt.load(
        std::memory_order_acquire);
    if (hookAt == 0 || now - hookAt > 100)
        FlushCurrentCameraAimInternal();
}

void FlushCurrentCameraAimInternal(uintptr_t camera) {
    if (freeCamActive || menuOpen || !IsGameFocused() || !clientBase)
        return;

    const uintptr_t manager = ResolveCitadelCameraManager();
    if (!manager) return;
    const uintptr_t activeCamera = Read<uintptr_t>(manager + 0x28);
    if (!activeCamera || (camera && camera != activeCamera))
        return;
    if (!camera)
        camera = activeCamera;

    Vector3 worldTarget{};
    bool customSmoothing = false;
    bool directAngles = false;
    Vector3 requestedAngles{};
    float pitchSmooth = aimPitchSmooth;
    float yawSmooth = aimYawSmooth;
    {
        std::lock_guard<std::mutex> lock(currentCameraAimMutex);
        // Keep the latest render-side target available for every gameplay
        // camera callback. Expire it quickly when target acquisition stops so
        // releasing the bind cannot leave the camera following a stale pawn.
        if (!queuedCameraAimReady ||
            GetTickCount64() - queuedCameraAimAt > 100) {
            queuedCameraAimReady = false;
            return;
        }
        worldTarget = queuedCameraAimTarget;
        customSmoothing = queuedCameraAimUsesCustomSmoothing;
        directAngles = queuedCameraAimUsesDirectAngles;
        requestedAngles = queuedCameraAimDirectAngles;
        if (customSmoothing) {
            pitchSmooth = queuedCameraAimPitchSmooth;
            yawSmooth = queuedCameraAimYawSmooth;
        }
    }

    // The normal gameplay camera layout differs from CFreeCamera. Live
    // inspection confirms +0x44/+0x48 are its pitch/yaw. In particular,
    // +0xCC belongs to the camera position and must never receive an angle.
    float pitch = Read<float>(camera + 0x44);
    float yaw = Read<float>(camera + 0x48);
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) return;

    constexpr float RadToDeg = 57.29577951308232f;
    float targetPitch = requestedAngles.x;
    float targetYaw = requestedAngles.y;
    if (!directAngles) {
        Vector3 cameraPosition = Read<Vector3>(camera + 0x38);
        if (!std::isfinite(cameraPosition.x) ||
            !std::isfinite(cameraPosition.y) ||
            !std::isfinite(cameraPosition.z)) {
            if (!currentCameraPositionReady) return;
            cameraPosition = currentCameraPosition;
        }
        const float dx = worldTarget.x - cameraPosition.x;
        const float dy = worldTarget.y - cameraPosition.y;
        const float dz = worldTarget.z - cameraPosition.z;
        const float horizontal = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(horizontal) || horizontal < 0.001f) return;
        targetPitch = -std::atan2(dz, horizontal) * RadToDeg;
        targetYaw = std::atan2(dy, dx) * RadToDeg;
    }
    float pitchDelta = NormalizeCameraAngle(targetPitch - pitch);
    float yawDelta = NormalizeCameraAngle(targetYaw - yaw);

    // Use the exact Normal aim camera path as recoil compensation. While the
    // attack button is held, pitch follows the selected world point without
    // smoothing, so a recoil kick cannot outrun the correction and lift the
    // crosshair above the target. Yaw keeps the user's normal smoothing.
    const bool firingVisibleAim = !customSmoothing &&
        aimAssist && (aimNormalActive || aimMixedMode) &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool instantPitch =
        pitchSmooth <= 1.001f || firingVisibleAim;
    const bool instantYaw = yawSmooth <= 1.001f;
    // "Only Yaw" limits target acquisition, but it must not disable recoil
    // compensation while firing or the crosshair can still climb vertically.
    const bool writePitch = customSmoothing || !aimOnlyYaw || firingVisibleAim;

    // Stop writing once the crosshair has converged. Reapplying tiny deltas
    // from consecutive camera states creates a feedback oscillation that also
    // makes every ESP projection shake.
    constexpr float angularDeadzone = 0.01f;
    if (!instantPitch && std::fabs(pitchDelta) < angularDeadzone)
        pitchDelta = 0.0f;
    if (!instantYaw && std::fabs(yawDelta) < angularDeadzone)
        yawDelta = 0.0f;
    if ((!writePitch || pitchDelta == 0.0f) && yawDelta == 0.0f)
        return;

    // Use time-based exponential damping instead of dividing the error by a
    // fixed number per callback. The camera update rate can differ from
    // Present/FPS while moving around a target; fixed per-callback steps then
    // become visibly uneven and produce the small left/right jitter reported
    // with Normal/Mixed aim.
    static std::chrono::steady_clock::time_point previousAimUpdate{};
    static uintptr_t previousAimCamera = 0;
    const auto now = std::chrono::steady_clock::now();
    float deltaSeconds = previousAimUpdate.time_since_epoch().count() == 0
        ? (1.0f / 60.0f)
        : std::chrono::duration<float>(now - previousAimUpdate).count();
    previousAimUpdate = now;
    if (previousAimCamera != camera || !std::isfinite(deltaSeconds) ||
        deltaSeconds <= 0.0f || deltaSeconds > 0.100f)
        deltaSeconds = 1.0f / 60.0f;
    previousAimCamera = camera;

    const auto smoothingRate = [](float value) {
        // At 60 Hz the slider behaves like the familiar error/divisor model.
        // Smooth=1 is effectively immediate instead of being forced to 50%.
        const float divisor = (std::max)(1.0f, value);
        return -std::log((std::max)(0.001f, 1.0f - 1.0f / divisor)) * 60.0f;
    };
    const float pitchAlpha = 1.0f -
        std::exp(-smoothingRate(pitchSmooth) * deltaSeconds);
    const float yawAlpha = 1.0f -
        std::exp(-smoothingRate(yawSmooth) * deltaSeconds);
    const float averageSmooth = (std::clamp)(
        (pitchSmooth + yawSmooth) * 0.5f, 1.0f, 20.0f);
    const float maxStepAt60Hz = 45.0f -
        (averageSmooth - 1.0f) * (33.0f / 19.0f);
    const float maxStep = maxStepAt60Hz * deltaSeconds * 60.0f;
    if (writePitch) {
        pitch = instantPitch
            ? targetPitch
            : pitch + (std::clamp)(pitchDelta * pitchAlpha, -maxStep, maxStep);
    }
    yaw = instantYaw
        ? targetYaw
        : yaw + (std::clamp)(yawDelta * yawAlpha, -maxStep, maxStep);
    pitch = (std::clamp)(pitch, -89.0f, 89.0f);
    yaw = NormalizeCameraAngle(yaw);

    // Change only the gameplay camera's source angles. The stock update below
    // derives +0xBC/+0xC0 and the quantized/interpolated +0xD4/+0xD8 state and
    // publishes the view matrix in one pass. Overwriting those derived fields
    // every render frame breaks interpolation and makes ESP shake while the
    // local player/camera is moving.
    Write<float>(camera + 0x44, pitch);
    Write<float>(camera + 0x48, yaw);
}

#ifndef DLL6_MOVEMENT_ONLY
uintptr_t FindLocalPrimaryWeaponAbility() {
    if (!currentLocalPawn) return 0;
    const uintptr_t component = currentLocalPawn + Offsets::AbilityComponent;
    const int count = (std::clamp)(
        Read<int>(component + Offsets::AbilityVector), 0, 64);
    const uintptr_t handles = Read<uintptr_t>(
        component + Offsets::AbilityVector + sizeof(uintptr_t));
    for (int index = 0; handles && index < count; ++index) {
        const uintptr_t ability = ResolveEntity(
            Read<uint32_t>(handles + index * sizeof(uint32_t)));
        if (ability &&
            Read<int16_t>(ability + Offsets::AbilitySlot) == 0x15)
            return ability;
    }
    return 0;
}

void ApplyAutoActiveReload(uintptr_t userCmd) {
    static uintptr_t inReloadOffset = 0;
    static uintptr_t canActiveReloadOffset = 0;
    static uintptr_t lastReloadStartOffset = 0;
    static uintptr_t nextPrimaryAttackOffset = 0;
    static uintptr_t attackDelayPauseOffset = 0;
    static uintptr_t simulationTimeOffset = 0;
    static uintptr_t lastWeapon = 0;
    static bool previousReload = false;
    static bool firedThisReload = false;
    static float triggerTime = 0.0f;
    static ULONGLONG reloadObservedAt = 0;
    static ULONGLONG nextOffsetRetry = 0;
    static ULONGLONG nextDiagnosticAt = 0;

    if (!autoActiveReload || !currentLocalPawn) {
        previousReload = false;
        firedThisReload = false;
        triggerTime = 0.0f;
        reloadObservedAt = 0;
        lastWeapon = 0;
        return;
    }
    if (!userCmd) return;

    const ULONGLONG now = GetTickCount64();
    if ((!inReloadOffset || !canActiveReloadOffset ||
         !lastReloadStartOffset || !nextPrimaryAttackOffset ||
         !attackDelayPauseOffset || !simulationTimeOffset) &&
        now >= nextOffsetRetry) {
        nextOffsetRetry = now + 1000;
        if (!inReloadOffset)
            inReloadOffset = ResolveRuntimeSchemaOffset(
                "CCitadel_Ability_PrimaryWeapon", "m_bInReload");
        if (!canActiveReloadOffset)
            canActiveReloadOffset = ResolveRuntimeSchemaOffset(
                "CCitadel_Ability_PrimaryWeapon", "m_bCanActiveReload");
        if (!lastReloadStartOffset)
            lastReloadStartOffset = ResolveRuntimeSchemaOffset(
                "CCitadel_Ability_PrimaryWeapon",
                "m_flLastReloadStartTime");
        if (!nextPrimaryAttackOffset)
            nextPrimaryAttackOffset = ResolveRuntimeSchemaOffset(
                "CCitadel_Ability_PrimaryWeapon", "m_flNextPrimaryAttack");
        if (!attackDelayPauseOffset)
            attackDelayPauseOffset = ResolveRuntimeSchemaOffset(
                "CCitadel_Ability_PrimaryWeapon",
                "m_flAttackDelayPauseTotalTime");
        if (!simulationTimeOffset)
            simulationTimeOffset = ResolveRuntimeSchemaOffset(
                "C_BaseEntity", "m_flSimulationTime");
    }
    if (!inReloadOffset || !canActiveReloadOffset ||
        !lastReloadStartOffset || !nextPrimaryAttackOffset ||
        !attackDelayPauseOffset) {
        if (kRuntimeDiagnostics && now >= nextDiagnosticAt) {
            nextDiagnosticAt = now + 500;
            std::ofstream log(
                Dll6Paths::DataFileA("active_reload_runtime.log"),
                std::ios::app);
            if (log)
                log << "offsets in_reload=0x" << std::hex << inReloadOffset
                    << " can=0x" << canActiveReloadOffset
                    << " start=0x" << lastReloadStartOffset
                    << " next=0x" << nextPrimaryAttackOffset
                    << " pause=0x" << attackDelayPauseOffset
                    << std::dec << '\n';
        }
        return;
    }

    const uintptr_t weapon = FindLocalPrimaryWeaponAbility();
    if (!weapon) {
        previousReload = false;
        firedThisReload = false;
        triggerTime = 0.0f;
        reloadObservedAt = 0;
        lastWeapon = 0;
        return;
    }
    if (weapon != lastWeapon) {
        lastWeapon = weapon;
        previousReload = false;
        firedThisReload = false;
        triggerTime = 0.0f;
        reloadObservedAt = 0;
    }

    const bool inReload = Read<uint8_t>(weapon + inReloadOffset) != 0;
    const bool supportsActiveReload =
        Read<uint8_t>(weapon + canActiveReloadOffset) != 0;
    const float globalGameNow = GetClientGameTime();
    const float simulationNow = simulationTimeOffset
        ? Read<float>(currentLocalPawn + simulationTimeOffset) : 0.0f;
    const float lastReloadStart =
        Read<float>(weapon + lastReloadStartOffset);
    const float nextPrimaryAttack =
        Read<float>(weapon + nextPrimaryAttackOffset);
    const float attackDelayPauseTotal =
        Read<float>(weapon + attackDelayPauseOffset);

    // This is the exact scheduling used by Andromeda 2.5.5. The pause-total
    // term is updated by the game during dashes and other interruptions, so
    // triggerTime moves with the actual reload instead of wall-clock time.
    const float computedTrigger = lastReloadStart +
        (nextPrimaryAttack - lastReloadStart + attackDelayPauseTotal) * 0.5f -
        0.1f;
    const float clockUpperBound = nextPrimaryAttack +
        (std::max)(0.0f, attackDelayPauseTotal) + 2.0f;
    const auto clockMatchesReload = [&](float value) {
        return std::isfinite(value) && value > 0.0f &&
            value >= lastReloadStart - 1.0f && value <= clockUpperBound;
    };
    float reloadNow = 0.0f;
    const char* clockSource = "none";
    if (clockMatchesReload(globalGameNow)) {
        reloadNow = globalGameNow;
        clockSource = "globals";
    } else if (clockMatchesReload(simulationNow)) {
        // Local-player simulation time shares the GameTime_t epoch used by
        // the weapon fields and remains available when the GlobalVars pattern
        // resolves to a stale client clock after a map/session transition.
        reloadNow = simulationNow;
        clockSource = "simulation";
    } else if (inReload && reloadObservedAt) {
        // Last-resort compatibility path for a client build where neither
        // clock is exposed. Pause-total still moves computedTrigger when a
        // dash or another action delays the reload.
        reloadNow = lastReloadStart +
            static_cast<float>(now - reloadObservedAt) / 1000.0f;
        clockSource = "local";
    }
    const bool validSchedule = supportsActiveReload &&
        std::isfinite(lastReloadStart) &&
        std::isfinite(nextPrimaryAttack) &&
        std::isfinite(attackDelayPauseTotal) &&
        std::isfinite(computedTrigger) && lastReloadStart > 0.0f &&
        nextPrimaryAttack > lastReloadStart;

    if (!inReload) {
        triggerTime = 0.0f;
        firedThisReload = false;
        reloadObservedAt = 0;
    } else {
        if (!previousReload || !reloadObservedAt)
            reloadObservedAt = now;
        // Initialize the local fallback after recording the start tick.
        if (reloadNow <= 0.0f) {
            reloadNow = lastReloadStart;
            clockSource = "local";
        }
    }
    if (inReload && validSchedule &&
        ((!previousReload && !firedThisReload) || triggerTime > 0.0f)) {
        if (std::isfinite(computedTrigger) && computedTrigger > 0.0f)
            triggerTime = computedTrigger;
    }
    previousReload = inReload;

    const bool tapReload = inReload && !firedThisReload &&
        triggerTime > 0.0f && reloadNow > 0.0f &&
        triggerTime <= reloadNow;
    if (tapReload) {
        firedThisReload = true;
        triggerTime = 0.0f;
    }
    if (kRuntimeDiagnostics && (now >= nextDiagnosticAt || tapReload)) {
        nextDiagnosticAt = now + 250;
        std::ofstream log(
            Dll6Paths::DataFileA("active_reload_runtime.log"),
            std::ios::app);
        if (log)
            log << "weapon=0x" << std::hex << weapon
                << " in_off=0x" << inReloadOffset
                << " can_off=0x" << canActiveReloadOffset << std::dec
                << " in_reload=" << inReload
                << " supported=" << supportsActiveReload
                << " global_now=" << globalGameNow
                << " simulation_now=" << simulationNow
                << " reload_now=" << reloadNow
                << " clock=" << clockSource
                << " start=" << lastReloadStart
                << " next=" << nextPrimaryAttack
                << " pause_total=" << attackDelayPauseTotal
                << " computed=" << computedTrigger
                << " scheduled=" << triggerTime
                << " tap=" << tapReload << '\n';
    }
    if (!tapReload) return;

    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    const uint64_t reloadMask =
        static_cast<uint64_t>(InputBitMask::Reload);
    command->buttonStates.buttonState1 |= reloadMask;
    command->buttonStates.buttonState2 |= reloadMask;
    command->buttonStates.buttonState3 |= reloadMask;
    if (auto* base = command->cmd.mutable_base()) {
        if (auto* buttons = base->mutable_buttons_pb()) {
            buttons->set_buttonstate1(buttons->buttonstate1() | reloadMask);
            buttons->set_buttonstate2(buttons->buttonstate2() | reloadMask);
            buttons->set_buttonstate3(buttons->buttonstate3() | reloadMask);
        }
    }

}

void ApplyBunnyHop(uintptr_t userCmd) {
    static uintptr_t groundEntityOffset = 0;
    static uintptr_t flagsOffset = 0;
    static ULONGLONG nextOffsetRetry = 0;
    static ULONGLONG nextDiagnosticAt = 0;
    static bool previousOnGround = false;
    static bool previousSpaceDown = false;
    static bool landingArmed = false;
    static bool manualAirborneInput = false;
    static bool airborneReleaseSeen = false;
    static ULONGLONG airborneSince = 0;
    static ULONGLONG airbornePhaseSince = 0;
    static unsigned groundStableFrames = 0;
    static ULONGLONG lastSyntheticJumpAt = 0;
    static ULONGLONG spaceReleasedAt = 0;
    static ULONGLONG lastDashPressedAt = 0;
    static bool previousDashDown = false;
    static uintptr_t trackedPawn = 0;
    static ULONGLONG physicalSpacePressedAt = 0;

    if (!bunnyHop || !currentLocalPawn || !userCmd) {
        bunnyBlockAirJump.store(false, std::memory_order_release);
        bunnyJumpOneShot.store(false, std::memory_order_release);
        bunnyDashJumpOneShot.store(false, std::memory_order_release);
        bunnyFinishDashJumpInput.store(false, std::memory_order_release);
        bunnyDashGuardUntil.store(0, std::memory_order_release);
        previousOnGround = false;
        previousSpaceDown = false;
        landingArmed = false;
        manualAirborneInput = false;
        airborneReleaseSeen = false;
        airborneSince = 0;
        airbornePhaseSince = 0;
        groundStableFrames = 0;
        lastSyntheticJumpAt = 0;
        spaceReleasedAt = 0;
        lastDashPressedAt = 0;
        previousDashDown = false;
        physicalSpacePressedAt = 0;
        bunnyDashGuardUntil.store(0, std::memory_order_release);
        trackedPawn = 0;
        return;
    }

    if (trackedPawn != currentLocalPawn) {
        trackedPawn = currentLocalPawn;
        previousOnGround = false;
        previousSpaceDown = false;
        landingArmed = false;
        manualAirborneInput = false;
        airborneReleaseSeen = false;
        airborneSince = 0;
        airbornePhaseSince = 0;
        groundStableFrames = 0;
        lastSyntheticJumpAt = 0;
        spaceReleasedAt = 0;
        lastDashPressedAt = 0;
        previousDashDown = false;
        physicalSpacePressedAt = 0;
    }

    const ULONGLONG now = GetTickCount64();
    if (bunnyFinishDashJumpInput.exchange(false,
            std::memory_order_acq_rel)) {
        manualAirborneInput = false;
    }
    if ((!groundEntityOffset || !flagsOffset) && now >= nextOffsetRetry) {
        nextOffsetRetry = now + 1000;
        if (!groundEntityOffset)
            groundEntityOffset = ResolveRuntimeSchemaOffset(
                "C_BaseEntity", "m_hGroundEntity");
        if (!flagsOffset)
            flagsOffset = ResolveRuntimeSchemaOffset(
                "C_BaseEntity", "m_fFlags");
    }
    if (!groundEntityOffset && !flagsOffset) return;

    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    // Space is published as two Citadel actions at this stage: mantle/jump
    // assistance and zipline interaction.  BunnyHop may gate only the former;
    // clearing the combined mask makes it impossible to grab a zipline.
    constexpr uint64_t mantleJumpMask = 0x0001000000000000ull;
    constexpr uint64_t standardJumpMask = 0x0000000000000002ull;
    constexpr uint64_t filteredJumpMask =
        mantleJumpMask | standardJumpMask;
    const bool spaceDown = !AreCustomBindsSuppressed() &&
        (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    const uint32_t groundHandle = groundEntityOffset
        ? Read<uint32_t>(currentLocalPawn + groundEntityOffset)
        : 0xFFFFFFFFu;
    const uint32_t flags = flagsOffset
        ? Read<uint32_t>(currentLocalPawn + flagsOffset) : 0u;
    const bool handleOnGround = groundEntityOffset &&
        groundHandle != 0xFFFFFFFFu &&
        (groundHandle & 0x7FFFu) != 0x7FFFu;
    const bool flagsOnGround = flagsOffset && (flags & 1u) != 0;
    // When both fields are available, require them to agree.  Either field can
    // briefly retain/flicker its ground value during a movement transition;
    // treating that as a landing is enough to consume a hero's double jump.
    const bool onGround = groundEntityOffset && flagsOffset
        ? (handleOnGround && flagsOnGround)
        : (handleOnGround || flagsOnGround);

    const bool physicalPress = spaceDown && !previousSpaceDown;
    if (physicalPress) physicalSpacePressedAt = now;
    if (!spaceDown) physicalSpacePressedAt = 0;
    previousSpaceDown = spaceDown;
    previousOnGround = onGround;

    bunnyBlockAirJump.store(false, std::memory_order_release);
    bunnyDashGuardUntil.store(0, std::memory_order_release);

    // A fresh physical Space press belongs entirely to the game. BunnyHop
    // starts filtering only after that press has been held long enough for the
    // native ground/air-jump transition to finish. This prevents the feature
    // from turning the first manual press into two movement impulses.
    constexpr ULONGLONG kFreshPhysicalPressWindowMs = 180;
    const bool freshPhysicalPress = spaceDown && physicalSpacePressedAt &&
        now - physicalSpacePressedAt < kFreshPhysicalPressWindowMs;
    if (onGround || freshPhysicalPress) return;

    const uint64_t keepMask = ~filteredJumpMask;
    command->buttonStates.buttonState1 =
        command->buttonStates.buttonState1 & keepMask;
    command->buttonStates.buttonState2 =
        command->buttonStates.buttonState2 & keepMask;
    command->buttonStates.buttonState3 =
        command->buttonStates.buttonState3 & keepMask;
    if (auto* base = command->cmd.mutable_base()) {
        if (auto* buttons = base->mutable_buttons_pb()) {
            buttons->set_buttonstate1(
                buttons->buttonstate1() & keepMask);
            buttons->set_buttonstate2(
                buttons->buttonstate2() & keepMask);
            buttons->set_buttonstate3(
                buttons->buttonstate3() & keepMask);
        }
        const int subtickCount = base->subtick_moves_size();
        if (subtickCount > 0 && subtickCount <= 64) {
            for (int i = 0; i < subtickCount; ++i) {
                auto* step = base->mutable_subtick_moves(i);
                if (step && step->has_button() &&
                    (step->button() & filteredJumpMask) != 0) {
                    step->set_button(step->button() & keepMask);
                }
            }
        }
    }
}
#endif

void __fastcall hkCreateMove(uintptr_t input, uint32_t splitScreenIndex, char a3) {
    EnsureGameplayCameraUpdateHook();
#ifndef DLL6_MOVEMENT_ONLY
    if (fovChangerEnabled || overrideScopeFov)
        EnsureGetRenderFovHook();
#endif
    if (!currentLocalPawn) {
        const uintptr_t resolvedPawn = FindLocalPawnFromController();
        if (resolvedPawn) currentLocalPawn = resolvedPawn;
    }
    if (kSilentMovementYawIsolation)
        EnsurePawnProcessUserCmdHook(currentLocalPawn);
    // BunnyHop is applied to the finalized user command. Do not install the
    // ProcessMovement hook for it: prediction can call that hook more than
    // once for the same command and consume Jump twice.
    if (kSilentMoveDataYawIsolation || movementReplayEnabled)
        EnsureProcessMovementHook(currentLocalPawn);
    // CreateMove and the following local input stage can both append weapon
    // recoil. Bracket the stock call so the state is clear before a shot is
    // built and immediately clear any state it produced afterwards.
#ifndef DLL6_MOVEMENT_ONLY
    ApplyVisibleAimInput(input);
#endif
#ifdef DLL6_MOVEMENT_ONLY
    // Publish the recorded held keys before the engine constructs this tick's
    // command, allowing it to create the normal native subtick transitions.
    PrepareLocalMovementPlaybackInput();
#endif
    if (originalCreateMove) originalCreateMove(input, splitScreenIndex, a3);
#ifndef DLL6_MOVEMENT_ONLY
    // Apply a live Drifter toggle on the gameplay thread. The modifier Think
    // touches gameplay-owned state and must not be invoked from Present.
    RefreshDrifterDarknessForToggle();
#endif
    // Engine physics tracing is only safe on the gameplay thread. Present
    // queues visibility requests; resolve them here and publish cached results.
#ifndef DLL6_MOVEMENT_ONLY
    ProcessAimVisibilityTraces();
#endif
    EnsureApplyInputCommandHook(input);
    // currentLocalPawn is refreshed by the render/entity path and may become
    // available only after the first CreateMove call. Retry here so the next
    // tick's pawn callback is hooked before it enters ProcessMovement.
    if (!currentLocalPawn) {
        const uintptr_t resolvedPawn = FindLocalPawnFromController();
        if (resolvedPawn) currentLocalPawn = resolvedPawn;
    }
    if (kSilentMovementYawIsolation)
        EnsurePawnProcessUserCmdHook(currentLocalPawn);
    if (kSilentMoveDataYawIsolation || movementReplayEnabled)
        EnsureProcessMovementHook(currentLocalPawn);
    const auto callCount = ++createMoveCalls;
    const uintptr_t userCmd = GetCurrentUserCmd();
    if (userCmd) ++userCmdResolvedCalls;
#ifndef DLL6_MOVEMENT_ONLY
    ApplyVisibleAimRecoilCommand(userCmd);
#endif

    // The pawn callback captures the exact raw movement and visible camera
    // yaw. ApplyPendingUserCmdAngles rotates that same command only when a
    // silent shot is actually written.
    Vector3 silentAngles{};
    bool correctionRequested = false;
    bool correctionApplied = false;
    float sourceForward = 0.0f;
    float sourceLeft = 0.0f;
    float correctedForward = 0.0f;
    float correctedLeft = 0.0f;
    float cameraYaw = 0.0f;
    int subtickMovesPatched = 0;
    bool commandCameraYaw = false;
    if (kCameraRelativeMovement && input && userCmd && GetPendingSilentInputAngle(silentAngles)) {
        correctionRequested = true;
        auto* command = reinterpret_cast<CUserCmd*>(userCmd);
        if (command->cmd.has_ang_camera_angles()) {
            cameraYaw = command->cmd.ang_camera_angles().y();
            commandCameraYaw = std::isfinite(cameraYaw) &&
                std::fabs(cameraYaw) <= 360.0f;
        }
        if (!commandCameraYaw) {
            cameraYaw = Read<Vector3>(input + 0x688).y;
            commandCameraYaw = std::isfinite(cameraYaw) &&
                std::fabs(cameraYaw) <= 360.0f;
        }
        if (commandCameraYaw) {
            sourceForward = Read<float>(input + 0x270);
            sourceLeft = Read<float>(input + 0x274);
        }
    }
    // The vtable[6] hook is the earliest local-input point, but it is an
    // optional runtime hook. Process here as well so hero scripts still work
    // when that auxiliary hook cannot be installed in a particular session.
    // ProcessHeroScriptsUserCmd de-duplicates the same command pointer.
    bool heroScriptModified = false;
#ifndef DLL6_MOVEMENT_ONLY
    heroScriptModified = ProcessHeroScriptsUserCmd(
        reinterpret_cast<CUserCmd*>(userCmd), true, input);
    ApplyAutoActiveReload(userCmd);
    ApplyPendingUserCmdAngles(userCmd);
#endif
    if (heroScriptModified) {
        auto* command = reinterpret_cast<CUserCmd*>(userCmd);
        if (command && command->cmd.has_ang_camera_angles()) {
            const auto& value = command->cmd.ang_camera_angles();
            PatchInputHistory(userCmd, {value.x(), value.y(), value.z()});
        }
    }
    static ULONGLONG lastMovementLog = 0;
    const ULONGLONG movementNow = GetTickCount64();
    const bool movementKeyDown =
        (GetAsyncKeyState('W') & 0x8000) != 0 ||
        (GetAsyncKeyState('A') & 0x8000) != 0 ||
        (GetAsyncKeyState('S') & 0x8000) != 0 ||
        (GetAsyncKeyState('D') & 0x8000) != 0;
    if (kRuntimeDiagnostics && movementKeyDown &&
        movementNow - lastMovementLog >= 100) {
        lastMovementLog = movementNow;
        float commandForward = 0.0f;
        float commandLeft = 0.0f;
        float commandYaw = 0.0f;
        if (userCmd) {
            const auto* command = reinterpret_cast<const CUserCmd*>(userCmd);
            if (command->cmd.has_base()) {
                commandForward = command->cmd.base().forwardmove();
                commandLeft = command->cmd.base().leftmove();
            }
            if (command->cmd.has_ang_camera_angles())
                commandYaw = command->cmd.ang_camera_angles().y();
        }
        static std::mutex movementLogMutex;
        std::lock_guard<std::mutex> movementLogLock(movementLogMutex);
        std::ofstream movementLog(
            Dll6Paths::DataFileA("movement_runtime.log"),
            std::ios::app);
        if (movementLog) {
            movementLog << "requested=" << correctionRequested
                        << " applied=" << correctionApplied
                        << " input=0x" << std::hex << input
                        << " cmd=0x" << userCmd << std::dec
                        << " cameraYaw=" << cameraYaw
                        << " silentYaw=" << silentAngles.y
                        << " cameraSource=" << (commandCameraYaw ? "command" : "invalid")
                        << " source=" << sourceForward << ',' << sourceLeft
                        << " corrected=" << correctedForward << ',' << correctedLeft
                        << " subticks=" << subtickMovesPatched
                        << " cmdMove=" << commandForward << ',' << commandLeft
                        << " cmdYaw=" << commandYaw << '\n';
        }
    }
#ifndef DLL6_MOVEMENT_ONLY
    static bool freeCamKeyLastDown = false;
    const bool freeCamKeyDown = IsGameFocused() &&
        !AreCustomBindsSuppressed() && freeCamKey > 0 &&
        ((GetAsyncKeyState(freeCamKey) & 0x8000) != 0);
    if (freeCamKeyDown && !freeCamKeyLastDown && freeCam)
        freeCamActive = !freeCamActive;
    freeCamKeyLastDown = freeCamKeyDown;
    UpdateFreeCameraCommand(userCmd);
#endif
}

uintptr_t __fastcall hkProcessMovement(uintptr_t movementServices,
                                       uintptr_t moveData) {
    ++movementProcessCalls;
    const bool physicalAttack = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    bool corrected = false;
    float silentYaw = 0.0f;
    float visibleYaw = 0.0f;
    float sourceForward = 0.0f;
    float sourceLeft = 0.0f;
    float correctedForward = 0.0f;
    float correctedLeft = 0.0f;
    if (kSilentMoveDataYawIsolation && moveData && aimSilentActive &&
        physicalAttack) {
        Vector3 cameraForward{};
        Vector3 pendingSilentAngles{};
        if (GetCurrentCameraForward(cameraForward) &&
            GetPendingSilentInputAngle(pendingSilentAngles)) {
            const float horizontal = std::hypot(
                cameraForward.x, cameraForward.y);
            silentYaw = pendingSilentAngles.y;
            sourceForward = Read<float>(moveData + 0x2C);
            sourceLeft = Read<float>(moveData + 0x30);
            if (std::isfinite(horizontal) && horizontal > 0.001f &&
                std::isfinite(silentYaw) &&
                std::isfinite(sourceForward) &&
                std::isfinite(sourceLeft)) {
                constexpr float RadToDeg = 57.29577951308232f;
                constexpr float DegToRad = 0.017453292519943295f;
                visibleYaw = std::atan2(cameraForward.y, cameraForward.x) *
                    RadToDeg;
                if (std::isfinite(visibleYaw)) {
                    // MoveData is already paired with the silent command yaw.
                    // Rotate only its analog movement axes into that basis so
                    // the resulting world direction remains relative to the
                    // visible camera. Digital WASD state is deliberately left
                    // untouched, otherwise Source 2 can suppress movement.
                    const float radians = (visibleYaw - silentYaw) * DegToRad;
                    const float cosine = std::cos(radians);
                    const float sine = std::sin(radians);
                    correctedForward =
                        sourceForward * cosine - sourceLeft * sine;
                    correctedLeft =
                        sourceForward * sine + sourceLeft * cosine;
                    Write<float>(moveData + 0x2C, correctedForward);
                    Write<float>(moveData + 0x30, correctedLeft);
                    corrected = true;
                }
            }
        }
    }

    // The command protobuf alone is too late for several Citadel mechanics:
    // ProcessMovement reads the movement service's live CInButtonState. Feed
    // the replay bits into that final consumer, then restore the object after
    // the call so no synthetic key can remain stuck between commands.
    // CInButtonState lives at +0x50 and begins with an 8-byte vtable.
    // Never overwrite it: the actual button masks start at +0x58.
    constexpr uintptr_t MovementButtonsOffset = 0x58;
    constexpr uint64_t ReplayManagedButtons =
        static_cast<uint64_t>(InputBitMask::Forward) |
        static_cast<uint64_t>(InputBitMask::Back) |
        static_cast<uint64_t>(InputBitMask::MoveLeft) |
        static_cast<uint64_t>(InputBitMask::MoveRight) |
        static_cast<uint64_t>(InputBitMask::Jump) |
        static_cast<uint64_t>(InputBitMask::Duck) |
        static_cast<uint64_t>(InputBitMask::Speed);
    const uint64_t replayHeld = MovementReplayHeldButtons();
    const uint64_t replayPressed = MovementReplayPressedButtons();
    std::array<uint64_t, 2> savedMovementButtons{};
    const bool patchReplayButtons = movementServices &&
        (movementReplayActive || movementReplayCalibrating);
    if (patchReplayButtons) {
        for (size_t i = 0; i < savedMovementButtons.size(); ++i)
            savedMovementButtons[i] = Read<uint64_t>(
                movementServices + MovementButtonsOffset + i * sizeof(uint64_t));
        Write<uint64_t>(movementServices + MovementButtonsOffset,
            (savedMovementButtons[0] & ~ReplayManagedButtons) | replayHeld);
        Write<uint64_t>(movementServices + MovementButtonsOffset + 8,
            (savedMovementButtons[1] & ~ReplayManagedButtons) | replayPressed);
    }

#ifndef DLL6_MOVEMENT_ONLY
    // Citadel consumes jump from the movement service, not only from the
    // serialized usercmd.  In particular m_nButtonDoublePressed can remain
    // queued after the command masks were already cleaned and spend the hero's
    // air jump.  Hide jump/mantle from the live consumer for this call while
    // leaving IN_ZIPLINE (bit 56) completely untouched.
    constexpr uint64_t MovementJumpMask =
        0x0000000000000002ull | 0x0001000000000000ull;
    constexpr uintptr_t QueuedButtonDownOffset = 0x70;
    constexpr uintptr_t QueuedButtonChangeOffset = 0x78;
    constexpr uintptr_t ButtonDoublePressedOffset = 0x80;
    const bool jumpOneShot = false;
    const bool dashJumpOneShot = false;
    const bool patchBunnyButtons = false;
    if (patchBunnyButtons) {
        // Keep held/down/change states intact so native jumps and zipline
        // grabs continue to work. Only discard the duplicate-edge channel.
        Write<uint64_t>(movementServices + ButtonDoublePressedOffset,
            Read<uint64_t>(movementServices + ButtonDoublePressedOffset) &
            ~MovementJumpMask);
    }
#endif

    const uintptr_t result = originalProcessMovement
        ? originalProcessMovement(movementServices, moveData) : 0;
#ifndef DLL6_MOVEMENT_ONLY
    if (patchBunnyButtons) {
        // Prediction can republish double-pressed during the stock call.
        Write<uint64_t>(movementServices + ButtonDoublePressedOffset,
            Read<uint64_t>(movementServices + ButtonDoublePressedOffset) &
            ~MovementJumpMask);
    }
    if (dashJumpOneShot || jumpOneShot) {
        // Let this ProcessMovement call consume Space as a dash-jump, then
        // discard only the queued follow-up that would become an air jump on
        // the next prediction pass.
        Write<uint64_t>(movementServices + QueuedButtonDownOffset,
            Read<uint64_t>(movementServices + QueuedButtonDownOffset) &
            ~MovementJumpMask);
        Write<uint64_t>(movementServices + QueuedButtonChangeOffset,
            Read<uint64_t>(movementServices + QueuedButtonChangeOffset) &
            ~MovementJumpMask);
        Write<uint64_t>(movementServices + ButtonDoublePressedOffset,
            Read<uint64_t>(movementServices + ButtonDoublePressedOffset) &
            ~MovementJumpMask);
        bunnyBlockAirJump.store(true, std::memory_order_release);
        bunnyFinishDashJumpInput.store(true, std::memory_order_release);
    }
#endif
    if (patchReplayButtons) {
        for (size_t i = 0; i < savedMovementButtons.size(); ++i)
            Write<uint64_t>(movementServices + MovementButtonsOffset +
                            i * sizeof(uint64_t), savedMovementButtons[i]);
    }
    if (corrected) {
        Write<float>(moveData + 0x2C, sourceForward);
        Write<float>(moveData + 0x30, sourceLeft);
        ++movementCorrectionCalls;
        movementDiagCameraYaw.store(visibleYaw, std::memory_order_relaxed);
        movementDiagMovementYaw.store(silentYaw, std::memory_order_relaxed);
        movementDiagRawForward.store(sourceForward, std::memory_order_relaxed);
        movementDiagRawLeft.store(sourceLeft, std::memory_order_relaxed);
        movementDiagResultForward.store(correctedForward,
                                         std::memory_order_relaxed);
        movementDiagResultLeft.store(correctedLeft,
                                      std::memory_order_relaxed);
    }

    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (kRuntimeDiagnostics && moveData && now - lastLog >= 100) {
        lastLog = now;
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::ofstream log(
            Dll6Paths::DataFileA("process_movement_runtime.log"),
            std::ios::app);
        if (log) {
            log << "keys="
                << ((GetAsyncKeyState('W') & 0x8000) != 0)
                << ((GetAsyncKeyState('A') & 0x8000) != 0)
                << ((GetAsyncKeyState('S') & 0x8000) != 0)
                << ((GetAsyncKeyState('D') & 0x8000) != 0)
                << " service=0x" << std::hex << movementServices
                << " data=0x" << moveData << std::dec
                << " corrected=" << corrected
                << " visibleYaw=" << visibleYaw
                << " silentYaw=" << silentYaw
                << " source=" << sourceForward << ',' << sourceLeft
                << " result=" << correctedForward << ','
                << correctedLeft << '\n';
        }
    }
    return result;
}

bool EnsureProcessMovementHook(uintptr_t pawn) {
    if (processMovementTarget && originalProcessMovement) return true;
    if (!pawn) return false;

    static uintptr_t movementServicesOffset = 0;
    if (!movementServicesOffset) {
        movementServicesOffset = ResolveRuntimeSchemaOffset(
            "C_BasePlayerPawn", "m_pMovementServices");
        // Current 2026-08-26 client: RTTI inspection of the live local pawn
        // resolves CCitadelPlayer_MovementServices at +0xF28. Keep the live
        // schema authoritative and use this only when SchemaSystem is not
        // published during early injection.
        if (!movementServicesOffset)
            movementServicesOffset = 0xF28;
    }
    if (!movementServicesOffset) return false;

    const uintptr_t movementServices = Read<uintptr_t>(
        pawn + movementServicesOffset);
    const uintptr_t vtable = Read<uintptr_t>(movementServices);
    if (!movementServices || !vtable) return false;

    constexpr uintptr_t ProcessMovementVtableSlot = 30;
    const uintptr_t target = Read<uintptr_t>(
        vtable + ProcessMovementVtableSlot * sizeof(uintptr_t));
    if (!target) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<void*>(target), &memory,
                      sizeof(memory)) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                           PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY)) == 0)
        return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    void* targetAddress = reinterpret_cast<void*>(target);
    const MH_STATUS createStatus = MH_CreateHook(
        targetAddress, reinterpret_cast<void*>(&hkProcessMovement),
        reinterpret_cast<void**>(&originalProcessMovement));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED)
        return false;
    const MH_STATUS enableStatus = MH_EnableHook(targetAddress);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        originalProcessMovement = nullptr;
        return false;
    }
    processMovementTarget = targetAddress;
    if (kRuntimeDiagnostics) {
        std::ofstream log(
            Dll6Paths::DataFileA("process_movement_runtime.log"),
            std::ios::app);
        if (log) {
            log << "installed schemaOffset=0x" << std::hex
                << movementServicesOffset << " service=0x" << movementServices
                << " target=0x" << target << " rva=0x"
                << (target - clientBase) << std::dec << '\n';
        }
    }
    return true;
}

uintptr_t __fastcall hkBuildWishDirection(uintptr_t movementServices,
                                           uintptr_t moveData,
                                           uintptr_t outputDirection,
                                           uintptr_t maxSpeed) {
    ++wishDirectionCalls;
    const uintptr_t result = originalBuildWishDirection
        ? originalBuildWishDirection(movementServices, moveData,
                                     outputDirection, maxSpeed) : 0;

    const bool physicalAttack =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool movementRequested =
        (aimSilentActive && physicalAttack) ||
        ((farmSilentMode || farmMixedMode) && physicalAttack) ||
        (autoLastHitOrbs && autoLastHitOrbsActive && pendingOrbAttack);
    Vector3 silentAngles{};
    const PawnUserCmdSnapshot pawnSnapshot = ReadLatestPawnUserCmd();
    if (!kCameraRelativeMovement || !outputDirection || !movementRequested || !pawnSnapshot.valid ||
        !GetPendingSilentInputAngle(silentAngles) ||
        !std::isfinite(pawnSnapshot.cameraYaw) ||
        !std::isfinite(silentAngles.y)) {
        return result;
    }

    // moveData may already contain the silent-yaw transformed values by the
    // time this helper is called. Use the raw values captured from the local
    // pawn command so the camera basis is applied exactly once.
    const float inputForward = pawnSnapshot.forward;
    const float inputLeft = pawnSnapshot.left;
    if (!std::isfinite(inputForward) || !std::isfinite(inputLeft))
        return result;
    const float inputLength = std::hypot(inputForward, inputLeft);
    if (inputLength < 0.001f) return result;

    // Build the final world-space movement vector directly from the camera
    // basis. This avoids applying a second rotation to a vector already
    // expressed in the silent-character basis.
    Vector3 cameraForward{};
    if (!GetCurrentCameraForward(cameraForward)) return result;
    const float cameraLength = std::hypot(cameraForward.x, cameraForward.y);
    if (!std::isfinite(cameraLength) || cameraLength < 0.001f)
        return result;
    const float cosine = cameraForward.x / cameraLength;
    const float sine = cameraForward.y / cameraLength;
    float* direction = reinterpret_cast<float*>(outputDirection);
    direction[0] = (cosine * inputForward - sine * inputLeft) / inputLength;
    direction[1] = (sine * inputForward + cosine * inputLeft) / inputLength;
    {
        std::lock_guard<std::mutex> lock(movementDebugWishMutex);
        movementDebugWishDirection = Vector3{direction[0], direction[1], 0.0f};
        movementDebugWishReady = true;
    }
    ++wishDirectionCorrectionCalls;
    ++movementCorrectionCalls;
    movementDiagCameraYaw.store(pawnSnapshot.cameraYaw,
                                std::memory_order_relaxed);
    movementDiagMovementYaw.store(silentAngles.y,
                                  std::memory_order_relaxed);
    movementDiagRawForward.store(inputForward, std::memory_order_relaxed);
    movementDiagRawLeft.store(inputLeft, std::memory_order_relaxed);
    movementDiagResultForward.store(direction[0],
                                    std::memory_order_relaxed);
    movementDiagResultLeft.store(direction[1],
                                 std::memory_order_relaxed);
    return result;
}

void __fastcall hkAccelerateMovement(uintptr_t movementServices,
                                     uintptr_t moveData,
                                     float frameTime) {
    if (!originalAccelerateMovement || !moveData) return;

    const bool physicalAttack =
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool movementRequested =
        (aimSilentActive && physicalAttack) ||
        ((farmSilentMode || farmMixedMode) && physicalAttack) ||
        (autoLastHitOrbs && autoLastHitOrbsActive && pendingOrbAttack);
    const PawnUserCmdSnapshot pawnSnapshot = ReadLatestPawnUserCmd();

    Vector3 cameraForward{};
    if (!kCameraRelativeMovement || !movementRequested || !pawnSnapshot.valid ||
        !GetCurrentCameraForward(cameraForward)) {
        originalAccelerateMovement(movementServices, moveData, frameTime);
        return;
    }

    const float cameraLength = std::hypot(cameraForward.x, cameraForward.y);
    if (!std::isfinite(cameraLength) || cameraLength < 0.001f ||
        !std::isfinite(pawnSnapshot.forward) ||
        !std::isfinite(pawnSnapshot.left)) {
        originalAccelerateMovement(movementServices, moveData, frameTime);
        return;
    }

    const float cameraYaw = std::atan2(cameraForward.y, cameraForward.x) *
                            57.29577951308232f;
    const Vector3 savedAngles = Read<Vector3>(moveData + 0x14);
    const float savedForward = Read<float>(moveData + 0x2C);
    const float savedLeft = Read<float>(moveData + 0x30);

    Write<float>(moveData + 0x18, cameraYaw);
    Write<float>(moveData + 0x2C, pawnSnapshot.forward);
    Write<float>(moveData + 0x30, pawnSnapshot.left);
    originalAccelerateMovement(movementServices, moveData, frameTime);

    Write<Vector3>(moveData + 0x14, savedAngles);
    Write<float>(moveData + 0x2C, savedForward);
    Write<float>(moveData + 0x30, savedLeft);
}

void PatchInputHistory(uintptr_t userCmd, const Vector3& angles) {
    if (!userCmd) return;
    const uintptr_t citadel = Read<uintptr_t>(userCmd + 0x48);
    if (!citadel) return;

    // CCitadelUserCmdPB: input-history vector pointer at +0x28,
    // element count at +0x20. The vector stores its first element at +8.
    const uintptr_t history = Read<uintptr_t>(citadel + 0x28);
    const int count = Read<int>(citadel + 0x20);
    if (!history || count <= 0 || count > 64) return;

    for (int i = 0; i < count; ++i) {
        const uintptr_t entry = Read<uintptr_t>(history + 8ull * (i + 1));
        if (!entry) continue;

        // IDA: history angle triple is entry+0x24/+0x28/+0x2C.
        const float historyPitch = Read<float>(entry + 0x24);
        const float historyRoll = Read<float>(entry + 0x2C);
        Write<float>(entry + 0x24, aimOnlyYaw && std::isfinite(historyPitch)
            ? historyPitch : angles.x);
        Write<float>(entry + 0x28, angles.y);
        Write<float>(entry + 0x2C, aimOnlyYaw && std::isfinite(historyRoll)
            ? historyRoll : angles.z);

        // Some builds keep the same angle as a protobuf object at +0x18.
        const uintptr_t qAngle = Read<uintptr_t>(entry + 0x18);
        if (qAngle) {
        Write<float>(qAngle + 0x18, aimOnlyYaw && std::isfinite(historyPitch)
            ? historyPitch : angles.x);
            Write<float>(qAngle + 0x1C, angles.y);
            Write<float>(qAngle + 0x20, angles.z);
        }
    }
}

void LogSilentHook(const char* text) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    std::ofstream log(Dll6Paths::DataFileA("silent_hook.log"), std::ios::app);
    if (log) log << text << '\n';
}

bool GetPendingSerializedAngles(Vector3& angles) {
    const uintptr_t userCmd = GetCurrentUserCmd();
    if (!userCmd || !UserCmdHasAttack(reinterpret_cast<const CUserCmd*>(userCmd))) return false;

    if (aimSilentActive) {
        std::lock_guard<std::mutex> lock(humanSilentMutex);
        if (pendingHumanReady) {
            angles = pendingHumanAngles;
            return true;
        }
    }
    if (autoLastHitOrbs) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        if (pendingOrbReady && pendingOrbAttack) {
            angles = pendingOrbAngles;
            return true;
        }
    }
    if (farmSilentMode || farmMixedMode) {
        std::lock_guard<std::mutex> lock(creepSilentMutex);
        if (pendingCreepReady) {
            angles = pendingCreepAngles;
            return true;
        }
    }
    return false;
}

uintptr_t __fastcall hkUserCmdToNetwork(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    ++userCmdNetworkCalls;
    if (freeCamActive)
        ClearFreeCameraMovement(
            freeCameraUserCmd.load(std::memory_order_acquire));
    return originalUserCmdToNetwork
        ? originalUserCmdToNetwork(a1, a2, a3) : 0;
}

#ifndef DLL6_MOVEMENT_ONLY
using DarknessTargetThinkFn = __int64(__fastcall*)(__int64);
using HasModifierStateFn = bool(__fastcall*)(__int64, uint16_t);
using DarknessZoneManagerFn = uintptr_t(__fastcall*)();
using DarknessZoneRemoveFn = void(__fastcall*)(uintptr_t, int, bool, bool);
using DarknessTargetUpdateFn = __int64(__fastcall*)(__int64);
using DarknessPropertyGetFn = float(__fastcall*)(uintptr_t, uintptr_t);

DarknessTargetThinkFn originalDarknessTargetThink = nullptr;
HasModifierStateFn originalHasModifierState = nullptr;
void* darknessTargetThinkTarget = nullptr;
void* hasModifierStateTarget = nullptr;
std::atomic<uint64_t> darknessTargetThinkCalls{0};
std::atomic<uint64_t> darknessModifierQueries{0};
std::atomic<uint64_t> darknessModifierQueriesBlocked{0};
std::atomic<ULONGLONG> darknessVictimSeenAt{0};
uintptr_t darknessBucketSizeOffset = 0;

struct DarknessModifierSnapshot {
    uintptr_t modifier{};
    float radius[2]{};
    uint8_t lock[2]{};
    bool captured{};
    bool inflated{};
};

DarknessModifierSnapshot darknessModifierSnapshot{};
bool darknessLastAppliedToggle = disableDrifterDarkness;
DarknessZoneManagerFn darknessZoneManager = nullptr;
DarknessZoneRemoveFn darknessZoneRemove = nullptr;
DarknessTargetUpdateFn darknessTargetUpdate = nullptr;
DarknessPropertyGetFn darknessPropertyGet = nullptr;
bool darknessZoneFunctionsResolved = false;

uintptr_t ResolveRelativeCallTarget(uintptr_t callInstruction);

bool IsWritableAddress(uintptr_t address) {
    if (!address) return false;
    MEMORY_BASIC_INFORMATION information{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &information,
                      sizeof(information)) ||
        information.State != MEM_COMMIT ||
        (information.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return false;
    const DWORD protection = information.Protect & 0xFF;
    return protection == PAGE_READWRITE ||
           protection == PAGE_WRITECOPY ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool ValidateDarknessController(uintptr_t candidate,
                                uintptr_t& sizeOffset) {
    if (!IsWritableAddress(candidate)) return false;
    constexpr int states[]{1, 3, 4};
    constexpr uintptr_t offsets[]{0, 0x10};
    for (const uintptr_t offset : offsets) {
        bool plausible = true;
        for (const int state : states) {
            const int size = Read<int>(
                candidate + 0x20 + state * 0x20 + offset);
            if (size < 0 || size > 4096) {
                plausible = false;
                break;
            }
        }
        if (plausible) {
            sizeOffset = offset;
            return true;
        }
    }
    return false;
}

uintptr_t GetDrifterPostProcessController() {
    static uintptr_t controller = 0;
    static bool resolved = false;
    if (resolved) return controller;
    resolved = true;

    constexpr const char* patterns[]{
        "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 40 57 48 83 EC ? BA",
        "48 8D 05 ? ? ? ? C3 CC CC CC CC CC CC CC CC 40 57 48 83 EC"};
    for (const char* pattern : patterns) {
        uintptr_t searchFrom = clientBase;
        for (int matchIndex = 0; matchIndex < 256; ++matchIndex) {
            const uintptr_t getter = FindClientPattern(pattern, searchFrom);
            if (!getter) break;
            searchFrom = getter + 1;
            const int32_t relative = Read<int32_t>(getter + 3);
            const uintptr_t leaTarget = getter + 7 +
                static_cast<intptr_t>(relative);
            uintptr_t sizeOffset = 0;
            if (ValidateDarknessController(leaTarget, sizeOffset)) {
                controller = leaTarget;
                darknessBucketSizeOffset = sizeOffset;
                return controller;
            }
            const uintptr_t indirect = Read<uintptr_t>(leaTarget);
            if (ValidateDarknessController(indirect, sizeOffset)) {
                controller = indirect;
                darknessBucketSizeOffset = sizeOffset;
                return controller;
            }
        }
    }
    return controller;
}

void InflateDrifterTargetRadius(uintptr_t modifier) {
    if (!modifier) return;
    constexpr uintptr_t properties[]{280, 536};
    for (const uintptr_t propertyOffset : properties) {
        const uintptr_t property = modifier + propertyOffset;
        // These fields are read by the current Darkness Target Think at
        // modifier+0x118 and modifier+0x218. Their cache can legitimately be
        // zero on the first victim tick, so range validation would skip the
        // override and leave both the world mask and model culling active.
        Write<float>(property + 0x38, 80000.0f);
        Write<uint8_t>(property + 0x3D, 1);
    }
}

void CaptureDrifterTargetRadius(uintptr_t modifier) {
    if (!modifier || (darknessModifierSnapshot.captured &&
                      darknessModifierSnapshot.modifier == modifier))
        return;
    darknessModifierSnapshot = {};
    darknessModifierSnapshot.modifier = modifier;
    if (!darknessPropertyGet && darknessTargetThinkTarget) {
        darknessPropertyGet = reinterpret_cast<DarknessPropertyGetFn>(
            ResolveRelativeCallTarget(
                reinterpret_cast<uintptr_t>(darknessTargetThinkTarget) +
                0x468));
    }
    constexpr uintptr_t properties[]{280, 536};
    for (size_t index = 0; index < std::size(properties); ++index) {
        const uintptr_t property = modifier + properties[index];
        float radius = Read<float>(property + 0x38);
        __try {
            if (darknessPropertyGet)
                radius = darknessPropertyGet(property, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            darknessPropertyGet = nullptr;
        }
        darknessModifierSnapshot.radius[index] = radius;
        darknessModifierSnapshot.lock[index] =
            Read<uint8_t>(property + 0x3D);
    }
    darknessModifierSnapshot.captured = true;
}

void RestoreDrifterTargetRadius() {
    if (!darknessModifierSnapshot.captured) return;
    constexpr uintptr_t properties[]{280, 536};
    for (size_t index = 0; index < std::size(properties); ++index) {
        const uintptr_t property =
            darknessModifierSnapshot.modifier + properties[index];
        Write<float>(property + 0x38,
                     darknessModifierSnapshot.radius[index]);
        Write<uint8_t>(property + 0x3D,
                       darknessModifierSnapshot.lock[index]);
    }
    darknessModifierSnapshot.inflated = false;
}

uintptr_t ResolveRelativeCallTarget(uintptr_t callInstruction) {
    if (!callInstruction || Read<uint8_t>(callInstruction) != 0xE8)
        return 0;
    return callInstruction + 5 +
        static_cast<intptr_t>(Read<int32_t>(callInstruction + 1));
}

void ResolveDarknessZoneFunctions() {
    if (darknessZoneFunctionsResolved) return;
    darknessZoneFunctionsResolved = true;
    // CModifier_Drifter_Darkness_Target::OnRemoved removes the visibility
    // registration stored at +0x418. Resolve both callees from that native
    // cleanup sequence instead of pinning their update-sensitive RVAs.
    const uintptr_t cleanup = FindUniqueClientPattern(
        "83 BF 18 04 00 00 FF 48 8B 5C 24 ? 74 ? E8 ? ? ? ? "
        "8B 97 18 04 00 00 "
        "41 B1 01 45 33 C0 48 8B C8 E8 ? ? ? ? C7 87 18 04 00 00 "
        "FF FF FF FF");
    if (!cleanup) return;
    darknessZoneManager = reinterpret_cast<DarknessZoneManagerFn>(
        ResolveRelativeCallTarget(cleanup + 14));
    darknessZoneRemove = reinterpret_cast<DarknessZoneRemoveFn>(
        ResolveRelativeCallTarget(cleanup + 34));
    darknessTargetUpdate = reinterpret_cast<DarknessTargetUpdateFn>(
        FindUniqueClientPattern(
            "48 89 5C 24 ? 57 48 83 EC 70 0F 29 74 24 ? "
            "0F 29 7C 24 ? 44 0F 29 44 24 ? 48 8B F9"));
    if (!darknessTargetUpdate && darknessTargetThinkTarget) {
        const uintptr_t adjacentUpdate =
            reinterpret_cast<uintptr_t>(darknessTargetThinkTarget) + 0x510;
        if (Read<uint32_t>(adjacentUpdate) == 0x245C8948)
            darknessTargetUpdate =
                reinterpret_cast<DarknessTargetUpdateFn>(adjacentUpdate);
    }
}

void RemoveTrackedDarknessZone() {
    if (!darknessModifierSnapshot.captured) return;
    ResolveDarknessZoneFunctions();
    if (!darknessZoneManager || !darknessZoneRemove) return;
    const uintptr_t modifier = darknessModifierSnapshot.modifier;
    const int handle = Read<int>(modifier + 0x418);
    if (handle == -1) return;
    __try {
        const uintptr_t manager = darknessZoneManager();
        if (manager) darknessZoneRemove(manager, handle, false, true);
        Write<int>(modifier + 0x418, -1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        darknessZoneManager = nullptr;
        darknessZoneRemove = nullptr;
    }
}

void PurgeDrifterDarknessBuckets() {
    const uintptr_t controller = GetDrifterPostProcessController();
    if (!controller) return;
    // Black, Blinded and DrifterDarknessCaster. Builds differ in whether the
    // bucket exposes m_Size directly or after a 0x10-byte CUtlMemory header;
    // controller discovery validates and records the live layout.
    constexpr int states[]{1, 3, 4};
    for (const int state : states) {
        const uintptr_t sizeAddress = controller + 0x20 +
            state * 0x20 + darknessBucketSizeOffset;
        const int size = Read<int>(sizeAddress);
        if (size >= 0 && size <= 4096)
            Write<int>(sizeAddress, 0);
    }
}

__int64 __fastcall HookDarknessTargetThink(__int64 modifier) {
    darknessTargetThinkCalls.fetch_add(1, std::memory_order_relaxed);
    if (!originalDarknessTargetThink) return 0;
    CaptureDrifterTargetRadius(static_cast<uintptr_t>(modifier));
    darknessLastAppliedToggle = disableDrifterDarkness;
    if (!disableDrifterDarkness) {
        if (darknessModifierSnapshot.inflated)
            RestoreDrifterTargetRadius();
        return originalDarknessTargetThink(modifier);
    }

    // The original Think must run after inflating the target radius: it is the
    // part that republishes heroes outside the real darkness volume. Remove
    // only the post-process buckets afterwards so visibility is preserved
    // without bringing the black screen back.
    InflateDrifterTargetRadius(static_cast<uintptr_t>(modifier));
    darknessModifierSnapshot.inflated = true;
    const __int64 result = originalDarknessTargetThink(modifier);
    PurgeDrifterDarknessBuckets();
    return result;
}

bool __fastcall HookHasModifierState(__int64 entity, uint16_t state) {
    darknessModifierQueries.fetch_add(1, std::memory_order_relaxed);
    const bool actualState = originalHasModifierState
        ? originalHasModifierState(entity, state) : false;
    if (state == 240 && actualState)
        darknessVictimSeenAt.store(GetTickCount64(),
                                   std::memory_order_relaxed);
    // These are query gates only. Do not force m_bVisibleOnMap: doing so also
    // publishes unrelated dormant entities and floods the minimap with junk.
    if (disableDrifterDarkness &&
        (state == 208 || state == 239 || state == 240)) {
        darknessModifierQueriesBlocked.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return actualState;
}

void LogDrifterToggleRefresh(bool enabled, uintptr_t modifier,
                             int oldHandle) {
    std::ofstream toggleLog(
        Dll6Paths::DataFileA("drifter_darkness_toggle.log"),
        std::ios::app);
    if (!toggleLog) return;
    toggleLog << "enabled=" << enabled
              << " modifier=0x" << std::hex << modifier
              << " manager=0x"
              << reinterpret_cast<uintptr_t>(darknessZoneManager)
              << " remove=0x"
              << reinterpret_cast<uintptr_t>(darknessZoneRemove)
              << " update=0x"
              << reinterpret_cast<uintptr_t>(darknessTargetUpdate)
              << std::dec << " old_handle=" << oldHandle
              << " new_handle=" << Read<int>(modifier + 0x418)
              << " radius118=" << Read<float>(modifier + 280 + 0x38)
              << " radius218=" << Read<float>(modifier + 536 + 0x38)
              << " lock118="
              << static_cast<int>(Read<uint8_t>(modifier + 280 + 0x3D))
              << " lock218="
              << static_cast<int>(Read<uint8_t>(modifier + 536 + 0x3D))
              << '\n';
}

void RefreshDrifterDarknessForToggle() {
    const bool enabled = disableDrifterDarkness;
    if (enabled == darknessLastAppliedToggle) return;
    darknessLastAppliedToggle = enabled;

    const ULONGLONG seenAt =
        darknessVictimSeenAt.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    if (!darknessModifierSnapshot.captured || !seenAt || now < seenAt ||
        now - seenAt > 1000)
        return;

    const uintptr_t modifier = darknessModifierSnapshot.modifier;
    if (!IsWritableAddress(modifier + 280)) return;
    // The radius is copied into a manager-owned visibility registration.
    // Merely restoring the network fields cannot shrink an already-created
    // zone, so remove its live handle before rebuilding it below.
    const int oldHandle = Read<int>(modifier + 0x418);
    RemoveTrackedDarknessZone();
    if (enabled) {
        InflateDrifterTargetRadius(modifier);
        darknessModifierSnapshot.inflated = true;
    } else {
        RestoreDrifterTargetRadius();
    }

    // Re-run the victim Think once so the renderer immediately rebuilds its
    // boundary/model visibility using the selected radius.
    __try {
        if (originalDarknessTargetThink)
            originalDarknessTargetThink(static_cast<__int64>(modifier));
        ResolveDarknessZoneFunctions();
        if (darknessTargetUpdate)
            darknessTargetUpdate(static_cast<__int64>(modifier));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        darknessModifierSnapshot = {};
    }
    if (enabled) PurgeDrifterDarknessBuckets();
    LogDrifterToggleRefresh(enabled, modifier, oldHandle);
}
#endif

}

void ApplyCurrentCameraAim(const Vector3& worldTarget) {
    ApplyCurrentCameraAimInternal(worldTarget);
}

bool InstallVisualFrameHook() {
    return InstallVisualFrameHookInternal();
}

void RemoveVisualFrameHook() {
    RemoveVisualFrameHookInternal();
}

void ApplyHeroScriptCameraAim(const Vector3& worldTarget,
                              float pitchSmooth, float yawSmooth) {
    ApplyHeroScriptCameraAimInternal(worldTarget, pitchSmooth, yawSmooth);
}

void ApplyMovementReplayCameraAngles(const Vector3& angles) {
    ApplyMovementReplayCameraAnglesInternal(angles);
}

#ifdef DLL6_MOVEMENT_ONLY
bool GetMovementReplayCameraAngles(Vector3& angles) {
    angles = {};
    const uintptr_t manager = ResolveCitadelCameraManager();
    if (!manager) return false;
    const uintptr_t camera = Read<uintptr_t>(manager + 0x28);
    if (!camera) return false;
    const float pitch = Read<float>(camera + 0x44);
    const float yaw = Read<float>(camera + 0x48);
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) return false;
    angles = {(std::clamp)(pitch, -89.0f, 89.0f),
              NormalizeCameraAngle(yaw), 0.0f};
    return true;
}
#endif

void QueueHeroSilentAngles(const Vector3& angles, bool attack,
                           bool overridePrimaryAim) {
    if (!std::isfinite(angles.x) || !std::isfinite(angles.y)) return;
    std::lock_guard<std::mutex> lock(silentAnglesMutex);
    pendingSilentAngles = angles;
    pendingSilentAnglesReady = true;
    pendingSilentAttack = pendingSilentAttack || attack;
    pendingHeroSilentOverridesPrimary = overridePrimaryAim;
}

void ClearHeroSilentAngles() {
    std::lock_guard<std::mutex> lock(silentAnglesMutex);
    pendingSilentAngles = {};
    pendingSilentAnglesReady = false;
    pendingSilentAttack = false;
    pendingHeroSilentOverridesPrimary = false;
}

void FlushCurrentCameraAim() {
    FlushCurrentCameraAimInternal();
}

bool InstallDrifterDarknessHooks() {
#ifdef DLL6_MOVEMENT_ONLY
    return false;
#else
    if (darknessTargetThinkTarget && hasModifierStateTarget &&
        originalDarknessTargetThink && originalHasModifierState)
        return true;

    darknessTargetThinkTarget = reinterpret_cast<void*>(FindClientPattern(
        "40 55 53 56 48 8D 6C 24 ? 48 81 EC 80 01 00 00 44 0F 29 84 24"));
    hasModifierStateTarget = reinterpret_cast<void*>(FindClientPattern(
        "4C 8B 89 ? ? ? ? 4D 85 C9 74 ? 44 0F B7 C2"));
    const uintptr_t controller = GetDrifterPostProcessController();
    if (!darknessTargetThinkTarget || !hasModifierStateTarget) {
        std::ofstream log(
            Dll6Paths::DataFileA("drifter_darkness_hooks.log"),
            std::ios::trunc);
        if (log) {
            log << "patterns missing think=0x" << std::hex
                << reinterpret_cast<uintptr_t>(darknessTargetThinkTarget)
                << " modifier=0x"
                << reinterpret_cast<uintptr_t>(hasModifierStateTarget)
                << " controller=0x" << controller << '\n';
        }
        darknessTargetThinkTarget = nullptr;
        hasModifierStateTarget = nullptr;
        return false;
    }

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    const MH_STATUS thinkCreate = MH_CreateHook(
        darknessTargetThinkTarget,
        reinterpret_cast<void*>(&HookDarknessTargetThink),
        reinterpret_cast<void**>(&originalDarknessTargetThink));
    const MH_STATUS modifierCreate = MH_CreateHook(
        hasModifierStateTarget,
        reinterpret_cast<void*>(&HookHasModifierState),
        reinterpret_cast<void**>(&originalHasModifierState));
    if ((thinkCreate != MH_OK && thinkCreate != MH_ERROR_ALREADY_CREATED) ||
        (modifierCreate != MH_OK && modifierCreate != MH_ERROR_ALREADY_CREATED)) {
        if (darknessTargetThinkTarget)
            MH_RemoveHook(darknessTargetThinkTarget);
        if (hasModifierStateTarget)
            MH_RemoveHook(hasModifierStateTarget);
        darknessTargetThinkTarget = nullptr;
        hasModifierStateTarget = nullptr;
        originalDarknessTargetThink = nullptr;
        originalHasModifierState = nullptr;
        return false;
    }

    const MH_STATUS thinkEnable = MH_EnableHook(darknessTargetThinkTarget);
    const MH_STATUS modifierEnable = MH_EnableHook(hasModifierStateTarget);
    const bool enabled =
        (thinkEnable == MH_OK || thinkEnable == MH_ERROR_ENABLED) &&
        (modifierEnable == MH_OK || modifierEnable == MH_ERROR_ENABLED);
    if (!enabled) {
        RemoveDrifterDarknessHooks();
        return false;
    }

    std::ofstream log(
        Dll6Paths::DataFileA("drifter_darkness_hooks.log"),
        std::ios::trunc);
    if (log) {
        log << "installed think=0x" << std::hex
            << reinterpret_cast<uintptr_t>(darknessTargetThinkTarget)
            << " modifier=0x"
            << reinterpret_cast<uintptr_t>(hasModifierStateTarget)
            << " controller=0x" << controller << '\n';
    }
    return true;
#endif
}

void RemoveDrifterDarknessHooks() {
#ifndef DLL6_MOVEMENT_ONLY
    SetDrifterPostProcessingSuppressed(false);
    if (darknessTargetThinkTarget) {
        MH_DisableHook(darknessTargetThinkTarget);
        MH_RemoveHook(darknessTargetThinkTarget);
    }
    if (hasModifierStateTarget) {
        MH_DisableHook(hasModifierStateTarget);
        MH_RemoveHook(hasModifierStateTarget);
    }
    darknessTargetThinkTarget = nullptr;
    hasModifierStateTarget = nullptr;
    originalDarknessTargetThink = nullptr;
    originalHasModifierState = nullptr;
#endif
}

void MaintainDrifterDarknessSuppression() {
#ifndef DLL6_MOVEMENT_ONLY
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG victimSeenAt =
        darknessVictimSeenAt.load(std::memory_order_relaxed);
    SetDrifterPostProcessingSuppressed(
        disableDrifterDarkness && victimSeenAt != 0 &&
        now >= victimSeenAt && now - victimSeenAt < 500);
    if (!disableDrifterDarkness) return;

    // TargetThink is not the final writer on every client frame. The global
    // post-process controller may be repopulated later by modifier dispatch,
    // so clear the same three buckets once more immediately before Present.
    PurgeDrifterDarknessBuckets();

    if (!kRuntimeDiagnostics) return;

    static ULONGLONG lastLogAt = 0;
    if (now - lastLogAt < 2000) return;
    lastLogAt = now;
    const uintptr_t controller = GetDrifterPostProcessController();
    std::ofstream log(
        Dll6Paths::DataFileA("drifter_darkness_runtime.log"),
        std::ios::app);
    if (log) {
        log << "think="
            << darknessTargetThinkCalls.load(std::memory_order_relaxed)
            << " queries="
            << darknessModifierQueries.load(std::memory_order_relaxed)
            << " blocked="
            << darknessModifierQueriesBlocked.load(std::memory_order_relaxed)
            << " controller=0x" << std::hex << controller << std::dec
            << " size_offset=0x" << std::hex
            << darknessBucketSizeOffset << std::dec
            << " bucket_sizes="
            << Read<int>(controller + 0x20 + 1 * 0x20 +
                         darknessBucketSizeOffset) << ','
            << Read<int>(controller + 0x20 + 3 * 0x20 +
                         darknessBucketSizeOffset) << ','
            << Read<int>(controller + 0x20 + 4 * 0x20 +
                         darknessBucketSizeOffset) << '\n';
    }
#endif
}

bool InstallOrbEntityHooks() {
    if (!clientBase || orbEntityEventsAvailable) return orbEntityEventsAvailable;
    const char* addPattern = "48 89 74 24 ? 57 48 83 EC ? 41 B9 ? ? ? ? 41 8B C0 41 23 C1 48 8B F2 41 83 F8 ? 48 8B F9 44 0F 45 C8 41 81 F9 ? ? ? ? 73 ? FF 81";
    const char* removePattern = "48 89 74 24 ? 57 48 83 EC ? 41 B9 ? ? ? ? 41 8B C0 41 23 C1 48 8B F2 41 83 F8 ? 48 8B F9 44 0F 45 C8 41 81 F9 ? ? ? ? 73 ? FF 89";
    entityAddedTarget = reinterpret_cast<void*>(FindClientPattern(addPattern));
    entityRemovedTarget = reinterpret_cast<void*>(FindClientPattern(removePattern));
    if (!entityAddedTarget || !entityRemovedTarget) {
        printf("[Orb] entity lifecycle patterns not found\n");
        std::ofstream log(Dll6Paths::DataFileA("orb_hooks.log"), std::ios::app);
        if (log) log << "patterns not found add=0x" << std::hex << reinterpret_cast<uintptr_t>(entityAddedTarget)
                    << " remove=0x" << reinterpret_cast<uintptr_t>(entityRemovedTarget) << "\n";
        return false;
    }
    const MH_STATUS addStatus = MH_CreateHook(entityAddedTarget,
        reinterpret_cast<void*>(&HookEntityAdded), reinterpret_cast<void**>(&originalEntityAdded));
    const MH_STATUS removeStatus = MH_CreateHook(entityRemovedTarget,
        reinterpret_cast<void*>(&HookEntityRemoved), reinterpret_cast<void**>(&originalEntityRemoved));
    if ((addStatus != MH_OK && addStatus != MH_ERROR_ALREADY_CREATED) ||
        (removeStatus != MH_OK && removeStatus != MH_ERROR_ALREADY_CREATED)) {
        printf("[Orb] entity lifecycle hook creation failed add=%d remove=%d\n", addStatus, removeStatus);
        std::ofstream log(Dll6Paths::DataFileA("orb_hooks.log"), std::ios::app);
        if (log) log << "create failed add=" << addStatus << " remove=" << removeStatus << "\n";
        return false;
    }
    const MH_STATUS addEnable = MH_EnableHook(entityAddedTarget);
    const MH_STATUS removeEnable = MH_EnableHook(entityRemovedTarget);
    if ((addEnable != MH_OK && addEnable != MH_ERROR_ENABLED) ||
        (removeEnable != MH_OK && removeEnable != MH_ERROR_ENABLED)) return false;
    orbEntityEventsAvailable = true;
    printf("[Orb] entity lifecycle hooks installed\n");
    std::ofstream log(Dll6Paths::DataFileA("orb_hooks.log"), std::ios::app);
    if (log) log << "installed add=0x" << std::hex << reinterpret_cast<uintptr_t>(entityAddedTarget)
                << " remove=0x" << reinterpret_cast<uintptr_t>(entityRemovedTarget) << "\n";
    return true;
}

void RemoveOrbEntityHooks() {
    if (entityAddedTarget) {
        MH_DisableHook(entityAddedTarget);
        MH_RemoveHook(entityAddedTarget);
    }
    if (entityRemovedTarget) {
        MH_DisableHook(entityRemovedTarget);
        MH_RemoveHook(entityRemovedTarget);
    }
    entityAddedTarget = entityRemovedTarget = nullptr;
    originalEntityAdded = originalEntityRemoved = nullptr;
    orbEntityEventsAvailable = false;
}

bool InstallUserCmdHook() {
    if (!clientBase) return false;
    void* target = reinterpret_cast<void*>(clientBase + UserCmdToNetworkRva);
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) return false;
    const MH_STATUS createStatus = MH_CreateHook(
        target, reinterpret_cast<void*>(&hkUserCmdToNetwork),
        reinterpret_cast<void**>(&originalUserCmdToNetwork));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) return false;
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) return false;
    userCmdHookInstalled = true;
    LogSilentHook("installed CUserCmd_to_network RVA=0x14E0F00 serialized_angles=a3+0x08/+0x14/+0x20");
    return true;
}

bool InstallCreateMoveHook(const UserCmdFunctionAddresses& functions) {
    if (!functions.HasInputPath()) return false;
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) return false;
    createMoveTarget = reinterpret_cast<void*>(functions.createMove);
    const MH_STATUS createStatus = MH_CreateHook(
        createMoveTarget, reinterpret_cast<void*>(&hkCreateMove),
        reinterpret_cast<void**>(&originalCreateMove));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) return false;
    const MH_STATUS enableStatus = MH_EnableHook(createMoveTarget);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) return false;

    if (kCameraRelativeMovement) {
    processMovementTarget =
        reinterpret_cast<void*>(clientBase + ProcessMovementRva);
    const MH_STATUS movementCreateStatus = MH_CreateHook(
        processMovementTarget, reinterpret_cast<void*>(&hkProcessMovement),
        reinterpret_cast<void**>(&originalProcessMovement));
    if (movementCreateStatus != MH_OK &&
        movementCreateStatus != MH_ERROR_ALREADY_CREATED) {
        processMovementTarget = nullptr;
        originalProcessMovement = nullptr;
        return false;
    }
    const MH_STATUS movementEnableStatus =
        MH_EnableHook(processMovementTarget);
    if (movementEnableStatus != MH_OK &&
        movementEnableStatus != MH_ERROR_ENABLED) {
        processMovementTarget = nullptr;
        originalProcessMovement = nullptr;
        return false;
    }

    buildWishDirectionTarget =
        reinterpret_cast<void*>(clientBase + BuildWishDirectionRva);
    const MH_STATUS wishCreateStatus = MH_CreateHook(
        buildWishDirectionTarget, reinterpret_cast<void*>(&hkBuildWishDirection),
        reinterpret_cast<void**>(&originalBuildWishDirection));
    if (wishCreateStatus != MH_OK &&
        wishCreateStatus != MH_ERROR_ALREADY_CREATED) {
        buildWishDirectionTarget = nullptr;
        originalBuildWishDirection = nullptr;
        return false;
    }
    const MH_STATUS wishEnableStatus =
        MH_EnableHook(buildWishDirectionTarget);
    if (wishEnableStatus != MH_OK &&
        wishEnableStatus != MH_ERROR_ENABLED) {
        buildWishDirectionTarget = nullptr;
        originalBuildWishDirection = nullptr;
        return false;
    }

    accelerateMovementTarget =
        reinterpret_cast<void*>(clientBase + AccelerateMovementRva);
    const MH_STATUS accelerateCreateStatus = MH_CreateHook(
        accelerateMovementTarget, reinterpret_cast<void*>(&hkAccelerateMovement),
        reinterpret_cast<void**>(&originalAccelerateMovement));
    if (accelerateCreateStatus != MH_OK &&
        accelerateCreateStatus != MH_ERROR_ALREADY_CREATED) {
        accelerateMovementTarget = nullptr;
        originalAccelerateMovement = nullptr;
        return false;
    }
    const MH_STATUS accelerateEnableStatus =
        MH_EnableHook(accelerateMovementTarget);
    if (accelerateEnableStatus != MH_OK &&
        accelerateEnableStatus != MH_ERROR_ENABLED) {
        accelerateMovementTarget = nullptr;
        originalAccelerateMovement = nullptr;
        return false;
    }
    }

    userCmdHookInstalled = true;
    return true;
}

void RemoveUserCmdHook() {
    DeactivateBuiltInFreeCamera();
    if (accelerateMovementTarget) {
        MH_DisableHook(accelerateMovementTarget);
        MH_RemoveHook(accelerateMovementTarget);
        accelerateMovementTarget = nullptr;
        originalAccelerateMovement = nullptr;
    }
    if (buildWishDirectionTarget) {
        MH_DisableHook(buildWishDirectionTarget);
        MH_RemoveHook(buildWishDirectionTarget);
        buildWishDirectionTarget = nullptr;
        originalBuildWishDirection = nullptr;
    }
    if (processMovementTarget) {
        MH_DisableHook(processMovementTarget);
        MH_RemoveHook(processMovementTarget);
        processMovementTarget = nullptr;
        originalProcessMovement = nullptr;
    }
    if (freeCameraUpdateTarget) {
        MH_DisableHook(freeCameraUpdateTarget);
        MH_RemoveHook(freeCameraUpdateTarget);
        freeCameraUpdateTarget = nullptr;
        originalFreeCameraUpdate = nullptr;
    }
    if (gameplayCameraUpdateTarget) {
        MH_DisableHook(gameplayCameraUpdateTarget);
        MH_RemoveHook(gameplayCameraUpdateTarget);
        gameplayCameraUpdateTarget = nullptr;
        originalGameplayCameraUpdate = nullptr;
    }
    if (getRenderFovTarget) {
        MH_DisableHook(getRenderFovTarget);
        MH_RemoveHook(getRenderFovTarget);
        getRenderFovTarget = nullptr;
        originalGetRenderFov = nullptr;
    }
    if (applyInputCommandHookInstalled && applyInputCommandTarget) {
        MH_DisableHook(applyInputCommandTarget);
        MH_RemoveHook(applyInputCommandTarget);
        applyInputCommandTarget = nullptr;
        originalApplyInputCommand = nullptr;
        applyInputCommandHookInstalled = false;
    }
    if (pawnProcessUserCmdHookInstalled && pawnProcessUserCmdTarget) {
        MH_DisableHook(pawnProcessUserCmdTarget);
        MH_RemoveHook(pawnProcessUserCmdTarget);
        pawnProcessUserCmdTarget = nullptr;
        originalPawnProcessUserCmd = nullptr;
        pawnProcessUserCmdHookInstalled = false;
    }
    if (userCmdHookInstalled && clientBase) {
        void* target = reinterpret_cast<void*>(clientBase + UserCmdToNetworkRva);
        MH_DisableHook(target);
        MH_RemoveHook(target);
        originalUserCmdToNetwork = nullptr;
        userCmdHookInstalled = false;
    }
    if (createMoveTarget) {
        MH_DisableHook(createMoveTarget);
        MH_RemoveHook(createMoveTarget);
        createMoveTarget = nullptr;
        originalCreateMove = nullptr;
    }
    RestoreCameraOriginWriters();
}

bool InstallInputLockHooks() {
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) return false;

    const MH_STATUS keyStatus = MH_CreateHookApi(
        L"user32", "GetAsyncKeyState", reinterpret_cast<LPVOID>(&hkGetAsyncKeyState),
        reinterpret_cast<LPVOID*>(&originalGetAsyncKeyState));
    const MH_STATUS keyStateStatus = MH_CreateHookApi(
        L"user32", "GetKeyState", reinterpret_cast<LPVOID>(&hkGetKeyState),
        reinterpret_cast<LPVOID*>(&originalGetKeyState));
    const MH_STATUS keyboardStatus = MH_CreateHookApi(
        L"user32", "GetKeyboardState", reinterpret_cast<LPVOID>(&hkGetKeyboardState),
        reinterpret_cast<LPVOID*>(&originalGetKeyboardState));
    const MH_STATUS rawStatus = MH_CreateHookApi(
        L"user32", "GetRawInputData", reinterpret_cast<LPVOID>(&hkGetRawInputData),
        reinterpret_cast<LPVOID*>(&originalGetRawInputData));
    const MH_STATUS rawBufferStatus = MH_CreateHookApi(
        L"user32", "GetRawInputBuffer", reinterpret_cast<LPVOID>(&hkGetRawInputBuffer),
        reinterpret_cast<LPVOID*>(&originalGetRawInputBuffer));
    const MH_STATUS cursorPositionStatus = MH_CreateHookApi(
        L"user32", "SetCursorPos", reinterpret_cast<LPVOID>(&hkSetCursorPos),
        reinterpret_cast<LPVOID*>(&originalSetCursorPos));
    const MH_STATUS cursorClipStatus = MH_CreateHookApi(
        L"user32", "ClipCursor", reinterpret_cast<LPVOID>(&hkClipCursor),
        reinterpret_cast<LPVOID*>(&originalClipCursor));
    if ((keyStatus != MH_OK && keyStatus != MH_ERROR_ALREADY_CREATED) ||
        (keyStateStatus != MH_OK && keyStateStatus != MH_ERROR_ALREADY_CREATED) ||
        (keyboardStatus != MH_OK && keyboardStatus != MH_ERROR_ALREADY_CREATED) ||
        (rawBufferStatus != MH_OK && rawBufferStatus != MH_ERROR_ALREADY_CREATED) ||
        (cursorPositionStatus != MH_OK && cursorPositionStatus != MH_ERROR_ALREADY_CREATED) ||
        (cursorClipStatus != MH_OK && cursorClipStatus != MH_ERROR_ALREADY_CREATED) ||
        (rawStatus != MH_OK && rawStatus != MH_ERROR_ALREADY_CREATED)) return false;

    const MH_STATUS enableStatus = MH_EnableHook(MH_ALL_HOOKS);
    return enableStatus == MH_OK || enableStatus == MH_ERROR_ENABLED;
}

void RemoveInputLockHooks() {
    MH_DisableHook(reinterpret_cast<LPVOID>(GetAsyncKeyState));
    MH_DisableHook(reinterpret_cast<LPVOID>(GetKeyState));
    MH_DisableHook(reinterpret_cast<LPVOID>(GetKeyboardState));
    MH_DisableHook(reinterpret_cast<LPVOID>(GetRawInputData));
    MH_DisableHook(reinterpret_cast<LPVOID>(GetRawInputBuffer));
    MH_DisableHook(reinterpret_cast<LPVOID>(SetCursorPos));
    MH_DisableHook(reinterpret_cast<LPVOID>(ClipCursor));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetAsyncKeyState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetKeyState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetKeyboardState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetRawInputData));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetRawInputBuffer));
    MH_RemoveHook(reinterpret_cast<LPVOID>(SetCursorPos));
    MH_RemoveHook(reinterpret_cast<LPVOID>(ClipCursor));
    originalGetAsyncKeyState = nullptr;
    originalGetKeyState = nullptr;
    originalGetKeyboardState = nullptr;
    originalGetRawInputData = nullptr;
    originalGetRawInputBuffer = nullptr;
    originalSetCursorPos = nullptr;
    originalClipCursor = nullptr;
}

void SetupHooks() {
    // Create an isolated D3D11 swap chain solely to obtain the shared Present vtable.
    HWND tempWindow = CreateWindowExA(
        0, "STATIC", "Dll6TempWindow", WS_OVERLAPPEDWINDOW,
        0, 0, 100, 100, nullptr, nullptr, moduleHandle, nullptr);
    if (!tempWindow) {
        printf("[-] Failed to create temp window: %lu\n", GetLastError());
        return;
    }

    IDXGISwapChain* pSwapChain = nullptr;
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = tempWindow;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* pTempDevice = nullptr;
    ID3D11DeviceContext* pTempContext = nullptr;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &desc, &pSwapChain, &pTempDevice, nullptr, &pTempContext
    );

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            nullptr, 0, D3D11_SDK_VERSION,
            &desc, &pSwapChain, &pTempDevice, nullptr, &pTempContext
        );
    }

    if (FAILED(hr) || !pSwapChain || !pTempDevice || !pTempContext) {
        if (pSwapChain) pSwapChain->Release();
        if (pTempDevice) pTempDevice->Release();
        if (pTempContext) pTempContext->Release();
        DestroyWindow(tempWindow);
        printf("[-] Failed to create temp swapchain: 0x%08lX\n", static_cast<unsigned long>(hr));
        return;
    }

    void** pVTable = *(void***)pSwapChain;
    if (!pVTable || !pVTable[8] || !pVTable[13]) {
        pSwapChain->Release();
        pTempDevice->Release();
        pTempContext->Release();
        DestroyWindow(tempWindow);
        return;
    }
    oPresent = (PresentFn)pVTable[8];
    oResizeBuffers = reinterpret_cast<ResizeBuffersFn>(pVTable[13]);
    presentVTable = pVTable;

    // Hook Present and ResizeBuffers together. A backbuffer can be recreated
    // while a map loads, so keeping an RTV or D2D surface from the old one is
    // unsafe.
    DWORD oldProtect;
    if (!VirtualProtect(&pVTable[8], 6 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        pSwapChain->Release();
        pTempDevice->Release();
        pTempContext->Release();
        oPresent = nullptr;
        oResizeBuffers = nullptr;
        presentVTable = nullptr;
        DestroyWindow(tempWindow);
        return;
    }
    pVTable[8] = hkPresent;
    pVTable[13] = hkResizeBuffers;
    DWORD unusedProtect;
    VirtualProtect(&pVTable[8], 6 * sizeof(void*), oldProtect, &unusedProtect);

    pSwapChain->Release();
    pTempDevice->Release();
    pTempContext->Release();
    DestroyWindow(tempWindow);

    printf("[+] VMT Hook installed!\n");
    // Orb discovery is handled by the validated polling scan. Lifecycle
    // detours are disabled because callbacks can run while identities are
    // being rebuilt and are unsafe for ESP lifetime tracking.
    orbEntityEventsAvailable = false;
    printf("[+] Orb entity hooks: disabled; using polling\n");
}

DWORD WINAPI InitializeThread(LPVOID) {
    // A second mapped copy would install another set of detours over the
    // first one. Keep one active instance per game process, but release this
    // guard during the orderly hot-unload so reinjection remains possible.
    const std::wstring instanceGuardName =
        L"Local\\Dll6_Deadlock_SingleInstance_" +
        std::to_wstring(GetCurrentProcessId());
    moduleInstanceGuard = CreateMutexW(
        nullptr, FALSE, instanceGuardName.c_str());
    if (!moduleInstanceGuard || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (moduleInstanceGuard) {
            CloseHandle(moduleInstanceGuard);
            moduleInstanceGuard = nullptr;
        }
        return 0;
    }
    const std::wstring readyEventName =
        L"Local\\Dll6_Deadlock_Ready_" +
        std::to_wstring(GetCurrentProcessId());
    moduleReadyEvent = CreateEventW(nullptr, TRUE, FALSE,
                                    readyEventName.c_str());

#ifdef DLL6_MOVEMENT_ONLY
    // Standalone demonstration build: no menu/config-dependent features.
    menuOpen = false;
    movementProbeEnabled = false;
    movementReplayEnabled = true;
    movementReplayKey = VK_F6;
#else
    LoadConfig();
#endif
    for (int attempt = 0; attempt < 100; ++attempt) {
        clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        if (clientBase) break;
        Sleep(100);
    }

    if (!clientBase) {
        if (moduleReadyEvent) {
            CloseHandle(moduleReadyEvent);
            moduleReadyEvent = nullptr;
        }
        CloseHandle(moduleInstanceGuard);
        moduleInstanceGuard = nullptr;
        return ERROR_MOD_NOT_FOUND;
    }

    // Resolve build-dependent globals from the current client image before
    // any entity worker starts. This is the pattern-based finder used by the
    // reference project; the brittle live-schema walker stays disabled.
    bool patternOffsetsReady = false;
    // A visible window and loaded client.dll do not imply that entity
    // identities exist yet during a cold Steam launch. Keep validation, but
    // allow the game to finish creating its entity system before giving up.
    const ULONGLONG patternDeadline = GetTickCount64() + 180000;
    do {
        patternOffsetsReady = InitializePatternOffsets();
        if (!patternOffsetsReady) Sleep(250);
    } while (!patternOffsetsReady && GetTickCount64() < patternDeadline);
    printf("[+] Pattern offsets: %s\n", patternOffsetsReady ? "ready" : "fallback");

    // Update schema-backed member offsets before entity discovery starts.
    // The schema walker mirrors the embedded CSchemaList layout used by the
    // reference project and keeps the last known value for missing fields.
    bool schemaOffsetsReady = false;
    for (int attempt = 0; attempt < 20 && !schemaOffsetsReady; ++attempt) {
        schemaOffsetsReady = InitializeRuntimeOffsets();
        if (!schemaOffsetsReady) Sleep(100);
    }
    printf("[+] Schema offsets: %s\n",
           schemaOffsetsReady ? "ready" : "fallback");

    // Never continue with stale static addresses on an unknown build. A
    // failed resolver must disable the DLL cleanly instead of letting entity
    // workers or hooks touch arbitrary client memory.
    if (!patternOffsetsReady || !schemaOffsetsReady) {
        std::ofstream failure(
            Dll6Paths::DataFileA("offset_resolution_failed.log"),
            std::ios::trunc);
        if (failure) {
            failure << "fingerprint=" << runtimeBuildKey << '\n'
                    << "patterns=" << patternOffsetsReady << '\n'
                    << "schema=" << schemaOffsetsReady << '\n';
        }
        printf("[!] Runtime resolution failed; hooks and workers are disabled\n");
        // No hooks or workers have started. Do not leave the instance guard
        // behind: a later launch must be allowed to try initialization again.
        if (moduleReadyEvent) {
            CloseHandle(moduleReadyEvent);
            moduleReadyEvent = nullptr;
        }
        CloseHandle(moduleInstanceGuard);
        moduleInstanceGuard = nullptr;
        return ERROR_NOT_READY;
    }

#ifndef DLL6_MOVEMENT_ONLY
    const bool nativeGlowFound = InitializeNativeGlow();
    printf("[Glow] native registration: %s\n",
           nativeGlowFound ? "ready" : "unavailable");
    printf("[Misc] Disable Drifter Darkness hooks: %s\n",
           InstallDrifterDarknessHooks() ? "installed" : "unavailable");
#endif

    {
        std::ofstream marker(
            Dll6Paths::DataFileA("Dll6_runtime.marker"),
            std::ios::trunc);
        if (marker) marker << "axiom-server-module-1.0.54\nclientBase=0x"
                           << std::hex << clientBase << "\n";
    }
    printf("[+] client.dll: 0x%p\n", reinterpret_cast<void*>(clientBase));
    {
        std::ofstream measurement(
            Dll6Paths::DataFileA("speed_measurement.txt"),
            std::ios::trunc);
        if (measurement) measurement << "[measurement started]\n";
    }

    for (int attempt = 0; attempt < 50 && !HookGameWindow(); ++attempt) {
        Sleep(100);
    }
    printf("[+] Game window hook: %s\n", oWndProc ? "ready" : "not found");

    stopHeroDiscoveryEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    if (stopHeroDiscoveryEvent) {
        heroDiscoveryThread = CreateThread(nullptr, 0, HeroDiscoveryWorker, nullptr, 0, nullptr);
#ifndef DLL6_MOVEMENT_ONLY
        farmTargetThread = CreateThread(nullptr, 0, FarmTargetWorker, nullptr, 0, nullptr);
#endif
        if (heroDiscoveryThread)
            SetThreadPriority(heroDiscoveryThread, THREAD_PRIORITY_BELOW_NORMAL);
        if (farmTargetThread)
            SetThreadPriority(farmTargetThread, THREAD_PRIORITY_BELOW_NORMAL);
        // Glow is handled by the native PlayerOutline hook. The legacy
        // CGlowProperty worker is intentionally disabled because it competes
        // with the outline renderer and creates a jittering second layer.
    }
    // Keep the serializer untouched. Silent is applied in CreateMove, where
    // the current command and movement values are available together.
    printf("[!] CUserCmd_to_network silent hook disabled\n");
    const auto userCmdFunctions = ResolveUserCmdFunctions(clientBase);
    runtimeUserCmdFunctions = userCmdFunctions;
    printf("[+] UserCmd patterns: tick=%p array=%p sequence=%p createMove=%p\n",
           reinterpret_cast<void*>(userCmdFunctions.getUserCmdTick),
           reinterpret_cast<void*>(userCmdFunctions.getUserCmdArray),
           reinterpret_cast<void*>(userCmdFunctions.getUserCmdBySequence),
           reinterpret_cast<void*>(userCmdFunctions.createMove));
    {
        std::ofstream patternLog(
            Dll6Paths::DataFileA("usercmd_patterns.log"),
            std::ios::trunc);
        if (patternLog) {
            patternLog << std::hex
                       << "clientBase=0x" << clientBase << "\n"
                       << "getUserCmdTick=0x" << userCmdFunctions.getUserCmdTick << "\n"
                       << "getUserCmdArray=0x" << userCmdFunctions.getUserCmdArray << "\n"
                       << "getUserCmdBySequence=0x" << userCmdFunctions.getUserCmdBySequence << "\n"
                       << "createMove=0x" << userCmdFunctions.createMove << "\n"
                       << "firstUserCmdArrayGlobal=0x" << userCmdFunctions.firstUserCmdArrayGlobal << "\n"
                       << "createMoveHook=enabled-if-input-path\n";
        }
    }
    printf("[+] CreateMove hook: %s\n", InstallCreateMoveHook(userCmdFunctions) ? "installed" : "not installed");
    printf("[+] Input lock hooks: %s\n", InstallInputLockHooks() ? "installed" : "not installed");
    printf("[+] Client output visual hook: %s\n",
           InstallVisualFrameHook() ? "installed" : "not installed; Present fallback");
    printf("[+] Free camera origin patch: signature ready\n");
#ifndef DLL6_MOVEMENT_ONLY
    printf("[+] Sound event hook: %s\n", InstallSoundEventHook() ? "installed" : "not installed");
#endif
    SetupHooks();
    if (moduleReadyEvent) SetEvent(moduleReadyEvent);
    return 0;
}
