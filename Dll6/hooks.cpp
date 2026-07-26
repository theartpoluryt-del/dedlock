#include "shared.h"
#include "usercmd.hpp"
#include "usercmd_runtime.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdlib>
#include <atomic>
#include <MinHook.h>

namespace {

using EntityLifecycleFn = void(__fastcall*)(uintptr_t, uintptr_t, uint32_t);
EntityLifecycleFn originalEntityAdded = nullptr;
EntityLifecycleFn originalEntityRemoved = nullptr;
void* entityAddedTarget = nullptr;
void* entityRemovedTarget = nullptr;

uintptr_t FindClientPattern(const char* pattern) {
    if (!clientBase) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase), &info, sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token) bytes.push_back(token == "?" ? -1 : std::strtoul(token.c_str(), nullptr, 16));
    if (bytes.empty() || bytes.size() > info.SizeOfImage) return 0;
    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
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

SHORT WINAPI hkGetAsyncKeyState(int key) {
    if (menuOpen) return 0;
    return originalGetAsyncKeyState ? originalGetAsyncKeyState(key) : 0;
}

SHORT WINAPI hkGetKeyState(int key) {
    if (menuOpen) return 0;
    return originalGetKeyState ? originalGetKeyState(key) : 0;
}

BOOL WINAPI hkGetKeyboardState(PBYTE state) {
    if (menuOpen) {
        if (state) ZeroMemory(state, 256);
        return TRUE;
    }
    return originalGetKeyboardState ? originalGetKeyboardState(state) : FALSE;
}

UINT WINAPI hkGetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT headerSize) {
    if (menuOpen) {
        if (size) *size = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<UINT>(-1);
    }
    return originalGetRawInputData
        ? originalGetRawInputData(handle, command, data, size, headerSize)
        : static_cast<UINT>(-1);
}

UINT WINAPI hkGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    if (menuOpen) {
        if (size) *size = 0;
        SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<UINT>(-1);
    }
    return originalGetRawInputBuffer
        ? originalGetRawInputBuffer(data, size, headerSize)
        : static_cast<UINT>(-1);
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

void ApplyPendingUserCmdAngles(uintptr_t userCmd) {
    if (!userCmd || (!aimSilentMode && !farmSilentMode && !autoLastHitOrbs)) return;
    Vector3 angles{};
    bool attack = false;
    {
        std::lock_guard<std::mutex> lock(silentAnglesMutex);
        if (!pendingSilentAnglesReady) return;
        angles = pendingSilentAngles;
        attack = pendingSilentAttack;
        pendingSilentAnglesReady = false;
        pendingSilentAttack = false;
    }

    auto* command = reinterpret_cast<CUserCmd*>(userCmd);
    auto* viewAngles = command->cmd.mutable_ang_camera_angles();
    if (!viewAngles) return;
    viewAngles->set_x(angles.x);
    viewAngles->set_y(angles.y);
    viewAngles->set_z(angles.z);
    command->cmd.clear_view_delta_x();
    command->cmd.clear_view_delta_y();
    if (attack) {
        const auto attackMask = static_cast<std::uint64_t>(InputBitMask::Attack);
        command->buttonStates.buttonState1 |= attackMask;
        command->buttonStates.buttonState2 |= attackMask;
        command->buttonStates.buttonState3 |= attackMask;

        // The network serializer reads the protobuf button block. On ticks
        // without physical input it may be absent, so merely changing the
        // native CInButtonState is not enough to produce an attack command.
        // Materialize both optional messages before setting the attack bit.
        auto* base = command->cmd.mutable_base();
        if (base) {
            auto* buttons = base->mutable_buttons_pb();
            if (buttons) {
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

void __fastcall hkCreateMove(uintptr_t input, uint32_t splitScreenIndex, char a3) {
    if (originalCreateMove) originalCreateMove(input, splitScreenIndex, a3);
    const auto callCount = ++createMoveCalls;
    const uintptr_t userCmd = GetCurrentUserCmd();
    if (userCmd) ++userCmdResolvedCalls;
    ApplyPendingUserCmdAngles(userCmd);
    if ((callCount % 120) == 0) {
        std::ofstream log(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\usercmd_runtime.log",
            std::ios::app);
        if (log) {
            log << "createMove=" << callCount
                << " userCmd=" << userCmdResolvedCalls.load()
                << " silentApplied=" << silentAppliedCalls.load() << '\n';
        }
    }
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
        Write<float>(entry + 0x24, angles.x);
        Write<float>(entry + 0x28, angles.y);
        Write<float>(entry + 0x2C, angles.z);

        // Some builds keep the same angle as a protobuf object at +0x18.
        const uintptr_t qAngle = Read<uintptr_t>(entry + 0x18);
        if (qAngle) {
            Write<float>(qAngle + 0x18, angles.x);
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

uintptr_t __fastcall hkUserCmdToNetwork(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    // Silent is now handled by paired mouse movement in aim.cpp. Keep the
    // serializer untouched so it cannot modify ordinary input or fight the
    // camera restoration step.
    return originalUserCmdToNetwork
        ? originalUserCmdToNetwork(a1, a2, a3) : 0;
}

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
    userCmdHookInstalled = true;
    return true;
}

void RemoveUserCmdHook() {
    if (!userCmdHookInstalled || !clientBase) return;
    void* target = reinterpret_cast<void*>(clientBase + UserCmdToNetworkRva);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    originalUserCmdToNetwork = nullptr;
    userCmdHookInstalled = false;
    if (createMoveTarget) {
        MH_DisableHook(createMoveTarget);
        MH_RemoveHook(createMoveTarget);
        createMoveTarget = nullptr;
        originalCreateMove = nullptr;
    }
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

    AllocConsole();
    consoleAttached = true;
    FILE* fDummy;
    freopen_s(&fDummy, "CONOUT$", "w", stdout);
    printf("[+] Deadlock Internal loaded!\n");
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
        // DX11 ESP is intentionally independent from the unresolved native
        // glow experiment. Do not mutate game render properties in the stable
        // build.
    }
    // UserCmd serializer hook disabled: restore stable pre-silent behavior.
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
    printf("[+] Sound event hook: %s\n", InstallSoundEventHook() ? "installed" : "not installed");
    SetupHooks();
    return 0;
}
