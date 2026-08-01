#include "shared.h"
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
UserCmdFunctionAddresses runtimeUserCmdFunctions{};

using CreateMoveFn = void(__fastcall*)(uintptr_t, uint32_t, char);
using GetUserCmdTickFn = void(__fastcall*)(uintptr_t, int32_t*);
using GetUserCmdArrayFn = uintptr_t(__fastcall*)(uintptr_t, int);
using GetUserCmdBySequenceFn = uintptr_t(__fastcall*)(uintptr_t, uint32_t);
CreateMoveFn originalCreateMove = nullptr;
void* createMoveTarget = nullptr;

// CreateMove calls input->vtable[6](input, userCmd) after building the
// command and before the command is consumed by local movement. This is the
// correct point for camera-relative correction; changing the command after
// CreateMove returns is too late for the pawn.
using ApplyInputCommandFn = void(__fastcall*)(uintptr_t, uintptr_t);
ApplyInputCommandFn originalApplyInputCommand = nullptr;
void* applyInputCommandTarget = nullptr;
bool applyInputCommandHookInstalled = false;

void ApplyVisibleAimInput(uintptr_t input);

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
std::atomic<uintptr_t> freeCameraUserCmd{0};
std::chrono::steady_clock::time_point lastFreeCameraUpdate{};
std::mutex freeCameraLifecycleMutex;
Vector3 freeCameraAnchor{};
bool freeCameraAnchorReady = false;
bool freeCameraStartPending = false;
Vector3 freeCameraStartAngles{};
bool freeCameraStartAnglesReady = false;

void FlushCurrentCameraAimInternal(uintptr_t camera = 0);

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyStateFn = SHORT(WINAPI*)(int);
using GetKeyboardStateFn = BOOL(WINAPI*)(PBYTE);
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
GetAsyncKeyStateFn originalGetAsyncKeyState = nullptr;
GetKeyStateFn originalGetKeyState = nullptr;
GetKeyboardStateFn originalGetKeyboardState = nullptr;
GetRawInputDataFn originalGetRawInputData = nullptr;
GetRawInputBufferFn originalGetRawInputBuffer = nullptr;

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
    // Apply once at the beginning of the complete per-frame camera update.
    // The stock update then consumes the angles and publishes a coherent
    // render transform during the same camera frame.
    FlushCurrentCameraAimInternal(camera);
    if (originalGameplayCameraUpdate)
        originalGameplayCameraUpdate(camera);
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

SHORT WINAPI hkGetAsyncKeyState(int key) {
    if (!IsGameFocused() || menuOpen ||
        (freeCamActive && IsFreeCameraMovementKey(key))) return 0;
    return originalGetAsyncKeyState ? originalGetAsyncKeyState(key) : 0;
}

SHORT WINAPI hkGetKeyState(int key) {
    if (!IsGameFocused() || menuOpen ||
        (freeCamActive && IsFreeCameraMovementKey(key))) return 0;
    return originalGetKeyState ? originalGetKeyState(key) : 0;
}

BOOL WINAPI hkGetKeyboardState(PBYTE state) {
    if (!IsGameFocused() || menuOpen) {
        if (state) ZeroMemory(state, 256);
        return TRUE;
    }
    const BOOL result =
        originalGetKeyboardState ? originalGetKeyboardState(state) : FALSE;
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
    if (menuOpen) {
        if (size) *size = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<UINT>(-1);
    }
    const UINT result = originalGetRawInputData
        ? originalGetRawInputData(handle, command, data, size, headerSize)
        : static_cast<UINT>(-1);
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
    return result;
}

UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    if (menuOpen) {
        if (size) *size = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<UINT>(-1);
    }
    const UINT result = originalGetRawInputBuffer
        ? originalGetRawInputBuffer(data, size, headerSize)
        : static_cast<UINT>(-1);
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

void RemoveDigitalMovementFromSilentCommand(CUserCmd* command,
                                            CBaseUserCmdPB* base) {
    if (!command || !base) return;

    const std::uint64_t movementMask =
        static_cast<std::uint64_t>(InputBitMask::Forward) |
        static_cast<std::uint64_t>(InputBitMask::Back) |
        static_cast<std::uint64_t>(InputBitMask::MoveLeft) |
        static_cast<std::uint64_t>(InputBitMask::MoveRight);
    const std::uint64_t keepMask = ~movementMask;

    // Digital W/A/S/D would make prediction reconstruct a cardinal movement
    // vector in the silent-yaw basis and override the rotated analog vector.
    // Keep every non-movement button (including Attack) untouched.
    command->buttonStates.buttonState1 &= keepMask;
    command->buttonStates.buttonState2 &= keepMask;
    command->buttonStates.buttonState3 &= keepMask;

    if (base->has_buttons_pb()) {
        auto* buttons = base->mutable_buttons_pb();
        buttons->set_buttonstate1(buttons->buttonstate1() & keepMask);
        buttons->set_buttonstate2(buttons->buttonstate2() & keepMask);
        buttons->set_buttonstate3(buttons->buttonstate3() & keepMask);
    }

    const int count = base->subtick_moves_size();
    if (count <= 0 || count > 64) return;
    for (int i = 0; i < count; ++i) {
        auto* step = base->mutable_subtick_moves(i);
        if (!step || !step->has_button()) continue;
        if ((step->button() & movementMask) != 0)
            step->set_button(0);
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

bool GetPendingSilentInputAngle(Vector3& angles) {
    const bool physicalAttack = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (aimSilentActive && physicalAttack) {
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
    if ((farmSilentMode || farmMixedMode) && physicalAttack) {
        std::lock_guard<std::mutex> lock(creepSilentMutex);
        if (pendingCreepReady) {
            angles = pendingCreepAngles;
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
    bool humanReady = false;
    {
        // A held player shot has priority over autonomous orb/farm helpers.
        // Keep the selected angle alive for every CreateMove command while the
        // render-side target remains valid; consuming it once left later
        // subtick shots with the old camera angle.
        if (aimSilentActive && commandHasAttack) {
            std::lock_guard<std::mutex> lock(humanSilentMutex);
            if (pendingHumanReady) {
                angles = pendingHumanAngles;
                ready = true;
                humanReady = true;
            }
        }
        if (!ready && autoLastHitOrbs) {
            std::lock_guard<std::mutex> lock(orbSilentMutex);
            if (pendingOrbReady && (pendingOrbAttack || commandHasAttack)) {
                angles = pendingOrbAngles;
                attack = pendingOrbAttack;
                // Attack is a pulse, but the angle must remain valid for all
                // commands until AutoLastHitOrbs drops or changes the target.
                pendingOrbAttack = false;
                ready = true;
            }
        }
        if (!ready && (farmSilentMode || farmMixedMode) && commandHasAttack) {
            std::lock_guard<std::mutex> lock(creepSilentMutex);
            if (pendingCreepReady) {
                angles = pendingCreepAngles;
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
    if (kCameraRelativeMovement && command->cmd.has_base() &&
        command->cmd.has_ang_camera_angles() &&
        std::isfinite(angles.y)) {
        auto* base = command->cmd.mutable_base();
        const float cameraYaw = command->cmd.ang_camera_angles().y();
        if (base && std::isfinite(cameraYaw)) {
            const float sourceForward = base->forwardmove();
            const float sourceLeft = base->leftmove();
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
            RotateSubtickMovement(base, cosine, sine);
            movementCorrected = true;
        }
    }

    static ULONGLONG lastCorrelationLog = 0;
    const ULONGLONG correlationNow = GetTickCount64();
    if (correlationNow - lastCorrelationLog >= 100) {
        lastCorrelationLog = correlationNow;
        static std::mutex correlationLogMutex;
        std::lock_guard<std::mutex> lock(correlationLogMutex);
        std::ofstream log(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\movement_correlation_runtime.log",
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
    const float sourceForward = base->forwardmove();
    const float sourceLeft = base->leftmove();
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
    if (now - lastMovementApplyLog >= 100) {
        lastMovementApplyLog = now;
        static std::mutex movementApplyLogMutex;
        std::lock_guard<std::mutex> lock(movementApplyLogMutex);
        std::ofstream log(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\movement_apply_runtime.log",
            std::ios::app);
        if (log) {
            log << "hook=1 input=0x" << std::hex << input
                << " cmd=0x" << userCmd << std::dec
                << " cameraYaw=" << cameraYaw
                << " silentYaw=" << silentAngles.y
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
    RotateSubtickMovement(base, cosine, sine);
}

void __fastcall hkApplyInputCommand(uintptr_t input, uintptr_t userCmd) {
    // This is the last input stage before local movement consumes the command.
    // Applying here makes Normal/Mixed rotate the actual in-game camera while
    // keeping the OS cursor untouched.
    ApplyPendingUserCmdAngles(userCmd);
    if (kCameraRelativeMovement)
        CorrectMovementBeforeLocalApply(input, userCmd);
    if (originalApplyInputCommand)
        originalApplyInputCommand(input, userCmd);
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
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\pawn_movement_runtime.log",
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
    const bool shouldLog = now - lastLog >= 100;
    if (shouldLog) {
        lastLog = now;
        LogPawnUserCmd("before", pawn, userCmd);
    }
    if (originalPawnProcessUserCmd)
        originalPawnProcessUserCmd(pawn, userCmd);

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
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\pawn_movement_runtime.log",
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
        (!aimNormalActive && !aimMixedMode)) return;

    std::lock_guard<std::mutex> lock(humanSilentMutex);
    if (!pendingHumanReady) return;

    constexpr uintptr_t InputViewAnglesOffset = 0x688;
    Vector3 current = Read<Vector3>(input + InputViewAnglesOffset);
    const Vector3 target = pendingHumanAngles;
    if (!std::isfinite(current.x) || !std::isfinite(current.y) ||
        !std::isfinite(target.x) || !std::isfinite(target.y)) return;

    const float pitchSmooth = (std::max)(1.0f, aimPitchSmooth);
    const float yawSmooth = (std::max)(1.0f, aimYawSmooth);
    if (!aimOnlyYaw)
        current.x += NormalizeCameraAngle(target.x - current.x) / pitchSmooth;
    current.y += NormalizeCameraAngle(target.y - current.y) / yawSmooth;
    Write<Vector3>(input + InputViewAnglesOffset, current);
}

std::mutex currentCameraAimMutex;
Vector3 queuedCameraAimTarget{};
bool queuedCameraAimReady = false;
ULONGLONG queuedCameraAimAt = 0;

void ApplyCurrentCameraAimInternal(const Vector3& worldTarget) {
    if (freeCamActive || menuOpen || !IsGameFocused() ||
        !std::isfinite(worldTarget.x) || !std::isfinite(worldTarget.y) ||
        !std::isfinite(worldTarget.z))
        return;

    std::lock_guard<std::mutex> lock(currentCameraAimMutex);
    queuedCameraAimTarget = worldTarget;
    queuedCameraAimReady = true;
    queuedCameraAimAt = GetTickCount64();
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
    }

    // The normal gameplay camera layout differs from CFreeCamera. Live
    // inspection confirms +0x44/+0x48 are its pitch/yaw. In particular,
    // +0xCC belongs to the camera position and must never receive an angle.
    float pitch = Read<float>(camera + 0x44);
    float yaw = Read<float>(camera + 0x48);
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) return;

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
    constexpr float RadToDeg = 57.29577951308232f;
    const float targetPitch = -std::atan2(dz, horizontal) * RadToDeg;
    const float targetYaw = std::atan2(dy, dx) * RadToDeg;
    float pitchDelta = NormalizeCameraAngle(targetPitch - pitch);
    float yawDelta = NormalizeCameraAngle(targetYaw - yaw);

    // Stop writing once the crosshair has converged. Reapplying tiny deltas
    // from consecutive camera states creates a feedback oscillation that also
    // makes every ESP projection shake.
    constexpr float angularDeadzone = 0.01f;
    if (std::fabs(pitchDelta) < angularDeadzone) pitchDelta = 0.0f;
    if (std::fabs(yawDelta) < angularDeadzone) yawDelta = 0.0f;
    if ((aimOnlyYaw || pitchDelta == 0.0f) && yawDelta == 0.0f)
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
        std::exp(-smoothingRate(aimPitchSmooth) * deltaSeconds);
    const float yawAlpha = 1.0f -
        std::exp(-smoothingRate(aimYawSmooth) * deltaSeconds);
    const float averageSmooth = (std::clamp)(
        (aimPitchSmooth + aimYawSmooth) * 0.5f, 1.0f, 20.0f);
    const float maxStepAt60Hz = 45.0f -
        (averageSmooth - 1.0f) * (33.0f / 19.0f);
    const float maxStep = maxStepAt60Hz * deltaSeconds * 60.0f;
    if (!aimOnlyYaw)
        pitch += (std::clamp)(pitchDelta * pitchAlpha, -maxStep, maxStep);
    yaw += (std::clamp)(yawDelta * yawAlpha, -maxStep, maxStep);
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

void __fastcall hkCreateMove(uintptr_t input, uint32_t splitScreenIndex, char a3) {
    EnsureGameplayCameraUpdateHook();
    if (!currentLocalPawn) {
        const uintptr_t resolvedPawn = FindLocalPawnFromController();
        if (resolvedPawn) currentLocalPawn = resolvedPawn;
    }
    if (kCameraRelativeMovement)
        EnsurePawnProcessUserCmdHook(currentLocalPawn);
    if (originalCreateMove) originalCreateMove(input, splitScreenIndex, a3);
    // Engine physics tracing is only safe on the gameplay thread. Present
    // queues visibility requests; resolve them here and publish cached results.
    ProcessAimVisibilityTraces();
    EnsureApplyInputCommandHook(input);
    // currentLocalPawn is refreshed by the render/entity path and may become
    // available only after the first CreateMove call. Retry here so the next
    // tick's pawn callback is hooked before it enters ProcessMovement.
    if (!currentLocalPawn) {
        const uintptr_t resolvedPawn = FindLocalPawnFromController();
        if (resolvedPawn) currentLocalPawn = resolvedPawn;
    }
    if (kCameraRelativeMovement)
        EnsurePawnProcessUserCmdHook(currentLocalPawn);
    const auto callCount = ++createMoveCalls;
    const uintptr_t userCmd = GetCurrentUserCmd();
    if (userCmd) ++userCmdResolvedCalls;

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
    ApplyPendingUserCmdAngles(userCmd);
    static ULONGLONG lastMovementLog = 0;
    const ULONGLONG movementNow = GetTickCount64();
    const bool movementKeyDown =
        (GetAsyncKeyState('W') & 0x8000) != 0 ||
        (GetAsyncKeyState('A') & 0x8000) != 0 ||
        (GetAsyncKeyState('S') & 0x8000) != 0 ||
        (GetAsyncKeyState('D') & 0x8000) != 0;
    if (movementKeyDown && movementNow - lastMovementLog >= 100) {
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
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\movement_runtime.log",
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
    static bool freeCamKeyLastDown = false;
    const bool freeCamKeyDown = IsGameFocused() && !menuOpen && freeCamKey > 0 &&
        ((GetAsyncKeyState(freeCamKey) & 0x8000) != 0);
    if (freeCamKeyDown && !freeCamKeyLastDown && freeCam)
        freeCamActive = !freeCamActive;
    freeCamKeyLastDown = freeCamKeyDown;
    UpdateFreeCameraCommand(userCmd);
}

uintptr_t __fastcall hkProcessMovement(uintptr_t movementServices,
                                       uintptr_t moveData) {
    ++movementProcessCalls;
    bool corrected = false;
    float cameraYaw = 0.0f;
    float movementYaw = 0.0f;
    float silentYaw = 0.0f;
    float sourceForward = 0.0f;
    float sourceLeft = 0.0f;
    float correctedForward = 0.0f;
    float correctedLeft = 0.0f;
    float afterForward = 0.0f;
    float afterLeft = 0.0f;

    // Movement must not wait for the protobuf/network serializer.  That hook
    // is not part of the local movement path and may not run for every tick.
    // Once an attack is physically held, use the camera basis continuously;
    // ProcessMovement already provides the character's current yaw.
    const bool physicalAttack = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool movementCorrectionRequested =
        (aimSilentActive && physicalAttack) ||
        ((farmSilentMode || farmMixedMode) && physicalAttack) ||
        (autoLastHitOrbs && autoLastHitOrbsActive && pendingOrbAttack);
    const PawnUserCmdSnapshot pawnSnapshot = ReadLatestPawnUserCmd();
    Vector3 movementSilentAngles{};
    const bool hasSilentMovementBasis =
        kCameraRelativeMovement && movementCorrectionRequested &&
        GetPendingSilentInputAngle(movementSilentAngles) &&
        std::isfinite(movementSilentAngles.y);
    if (moveData && pawnSnapshot.valid && hasSilentMovementBasis &&
        std::isfinite(pawnSnapshot.cameraYaw)) {
        cameraYaw = pawnSnapshot.cameraYaw;
        // IDA: sub_18078CC00 calls AngleVectors with MoveData + 0x14.
        // Therefore +0x18 is the yaw that defines the actual movement basis.
        movementYaw = Read<float>(moveData + 0x18);
        silentYaw = movementSilentAngles.y;
        sourceForward = Read<float>(moveData + 0x2C);
        sourceLeft = Read<float>(moveData + 0x30);
        if (std::isfinite(movementYaw) &&
            std::isfinite(sourceForward) &&
            std::isfinite(sourceLeft)) {
            correctedForward = sourceForward;
            correctedLeft = sourceLeft;
            corrected = true;
        }
    }

    const uintptr_t result = originalProcessMovement
        ? originalProcessMovement(movementServices, moveData) : 0;
    if (moveData) {
        afterForward = Read<float>(moveData + 0x20);
        afterLeft = Read<float>(moveData + 0x24);
        movementDiagAfterYaw.store(Read<float>(moveData + 0x18),
                                   std::memory_order_relaxed);
    }
    if (corrected) {
        ++movementCorrectionCalls;
        movementDiagCameraYaw.store(cameraYaw, std::memory_order_relaxed);
        movementDiagMovementYaw.store(movementYaw, std::memory_order_relaxed);
        movementDiagRawForward.store(sourceForward, std::memory_order_relaxed);
        movementDiagRawLeft.store(sourceLeft, std::memory_order_relaxed);
        movementDiagResultForward.store(correctedForward, std::memory_order_relaxed);
        movementDiagResultLeft.store(correctedLeft, std::memory_order_relaxed);
        movementDiagAfterForward.store(afterForward, std::memory_order_relaxed);
        movementDiagAfterLeft.store(afterLeft, std::memory_order_relaxed);
    }

    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (moveData && now - lastLog >= 100) {
        lastLog = now;
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::ofstream log(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\process_movement_runtime.log",
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
                << " cameraYaw=" << cameraYaw
                << " movementYaw=" << movementYaw
                << " silentYaw=" << silentYaw
                << " source=" << sourceForward << ',' << sourceLeft
                << " result=" << correctedForward << ','
                << correctedLeft
                << " after=" << afterForward << ',' << afterLeft << '\n';
        }
    }
    return result;
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
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\silent_hook.log", std::ios::app);
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

}

void ApplyCurrentCameraAim(const Vector3& worldTarget) {
    ApplyCurrentCameraAimInternal(worldTarget);
}

void FlushCurrentCameraAim() {
    FlushCurrentCameraAimInternal();
}

bool InstallOrbEntityHooks() {
    if (!clientBase || orbEntityEventsAvailable) return orbEntityEventsAvailable;
    const char* addPattern = "48 89 74 24 ? 57 48 83 EC ? 41 B9 ? ? ? ? 41 8B C0 41 23 C1 48 8B F2 41 83 F8 ? 48 8B F9 44 0F 45 C8 41 81 F9 ? ? ? ? 73 ? FF 81";
    const char* removePattern = "48 89 74 24 ? 57 48 83 EC ? 41 B9 ? ? ? ? 41 8B C0 41 23 C1 48 8B F2 41 83 F8 ? 48 8B F9 44 0F 45 C8 41 81 F9 ? ? ? ? 73 ? FF 89";
    entityAddedTarget = reinterpret_cast<void*>(FindClientPattern(addPattern));
    entityRemovedTarget = reinterpret_cast<void*>(FindClientPattern(removePattern));
    if (!entityAddedTarget || !entityRemovedTarget) {
        printf("[Orb] entity lifecycle patterns not found\n");
        std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_hooks.log", std::ios::app);
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
        std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_hooks.log", std::ios::app);
        if (log) log << "create failed add=" << addStatus << " remove=" << removeStatus << "\n";
        return false;
    }
    const MH_STATUS addEnable = MH_EnableHook(entityAddedTarget);
    const MH_STATUS removeEnable = MH_EnableHook(entityRemovedTarget);
    if ((addEnable != MH_OK && addEnable != MH_ERROR_ENABLED) ||
        (removeEnable != MH_OK && removeEnable != MH_ERROR_ENABLED)) return false;
    orbEntityEventsAvailable = true;
    printf("[Orb] entity lifecycle hooks installed\n");
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_hooks.log", std::ios::app);
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
    if ((keyStatus != MH_OK && keyStatus != MH_ERROR_ALREADY_CREATED) ||
        (keyStateStatus != MH_OK && keyStateStatus != MH_ERROR_ALREADY_CREATED) ||
        (keyboardStatus != MH_OK && keyboardStatus != MH_ERROR_ALREADY_CREATED) ||
        (rawBufferStatus != MH_OK && rawBufferStatus != MH_ERROR_ALREADY_CREATED) ||
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
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetAsyncKeyState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetKeyState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetKeyboardState));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetRawInputData));
    MH_RemoveHook(reinterpret_cast<LPVOID>(GetRawInputBuffer));
    originalGetAsyncKeyState = nullptr;
    originalGetKeyState = nullptr;
    originalGetKeyboardState = nullptr;
    originalGetRawInputData = nullptr;
    originalGetRawInputBuffer = nullptr;
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
    if (!pVTable || !pVTable[8]) {
        pSwapChain->Release();
        pTempDevice->Release();
        pTempContext->Release();
        DestroyWindow(tempWindow);
        return;
    }
    oPresent = (PresentFn)pVTable[8];
    presentVTable = pVTable;

    // Хукаем VMT
    DWORD oldProtect;
    if (!VirtualProtect(&pVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        pSwapChain->Release();
        pTempDevice->Release();
        pTempContext->Release();
        oPresent = nullptr;
        presentVTable = nullptr;
        DestroyWindow(tempWindow);
        return;
    }
    pVTable[8] = hkPresent;
    DWORD unusedProtect;
    VirtualProtect(&pVTable[8], sizeof(void*), oldProtect, &unusedProtect);

    pSwapChain->Release();
    pTempDevice->Release();
    pTempContext->Release();
    DestroyWindow(tempWindow);

    printf("[+] VMT Hook installed!\n");
    // The detours only enqueue handles; entity memory is read later by the
    // worker thread. This avoids touching partially constructed entities.
    // Orb discovery is handled by the validated polling scan. Lifecycle
    // detours are disabled because callbacks can run while identities are
    // being rebuilt and are unsafe for ESP lifetime tracking.
    orbEntityEventsAvailable = false;
    printf("[+] Orb entity hooks: disabled; using polling\n");
}

DWORD WINAPI InitializeThread(LPVOID) {
    LoadConfig();
    for (int attempt = 0; attempt < 100; ++attempt) {
        clientBase = reinterpret_cast<uintptr_t>(GetModuleHandleA("client.dll"));
        if (clientBase) break;
        Sleep(100);
    }

    if (!clientBase) return 0;

    // Resolve build-dependent globals from the current client image before
    // any entity worker starts. This is the pattern-based finder used by the
    // reference project; the brittle live-schema walker stays disabled.
    bool patternOffsetsReady = false;
    for (int attempt = 0; attempt < 50 && !patternOffsetsReady; ++attempt) {
        patternOffsetsReady = InitializePatternOffsets();
        if (!patternOffsetsReady) Sleep(100);
    }
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
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\offset_resolution_failed.log",
            std::ios::trunc);
        if (failure) {
            failure << "fingerprint=" << runtimeBuildKey << '\n'
                    << "patterns=" << patternOffsetsReady << '\n'
                    << "schema=" << schemaOffsetsReady << '\n';
        }
        printf("[!] Runtime resolution failed; hooks and workers are disabled\n");
        return 0;
    }

    const bool nativeGlowFound = InitializeNativeGlow();
    printf("[Glow] native registration: %s\n",
           nativeGlowFound ? "ready" : "unavailable");

    {
        std::ofstream marker(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\Dll6_runtime.marker",
            std::ios::trunc);
        if (marker) marker << "protobuf-silent-no-flick-build-2026-07-25-0012\nclientBase=0x"
                           << std::hex << clientBase << "\n";
    }
    printf("[+] client.dll: 0x%p\n", reinterpret_cast<void*>(clientBase));
    {
        std::ofstream measurement(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\speed_measurement.txt",
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
        farmTargetThread = CreateThread(nullptr, 0, FarmTargetWorker, nullptr, 0, nullptr);
        if (heroDiscoveryThread)
            SetThreadPriority(heroDiscoveryThread, THREAD_PRIORITY_BELOW_NORMAL);
        if (farmTargetThread)
            SetThreadPriority(farmTargetThread, THREAD_PRIORITY_BELOW_NORMAL);
        // Glow is handled by the native PlayerOutline hook. The legacy
        // CGlowProperty worker is intentionally disabled because it competes
        // with the outline renderer and creates a jittering second layer.
    }
    InstallModelGlowHook();
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
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\usercmd_patterns.log",
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
    printf("[+] Free camera origin patch: signature ready\n");
    printf("[+] Sound event hook: %s\n", InstallSoundEventHook() ? "installed" : "not installed");
    SetupHooks();
    return 0;
}
