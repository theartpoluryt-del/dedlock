#include "shared.h"
#include <fstream>
#include <atomic>
#include <MinHook.h>

namespace {

// IDA-confirmed CUserCmd_to_network entrypoint:
// image EA 0x1814E0F00 - image base 0x180000000 = RVA 0x14E0F00.
constexpr uintptr_t UserCmdToNetworkRva = 0x14E0F00;
using UserCmdToNetworkFn = uintptr_t(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
UserCmdToNetworkFn originalUserCmdToNetwork = nullptr;
bool userCmdHookInstalled = false;
std::atomic<unsigned long long> silentAppliedCalls{0};

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

void RemoveUserCmdHook() {
    if (!userCmdHookInstalled || !clientBase) return;
    void* target = reinterpret_cast<void*>(clientBase + UserCmdToNetworkRva);
    MH_DisableHook(target);
    MH_RemoveHook(target);
    originalUserCmdToNetwork = nullptr;
    userCmdHookInstalled = false;
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
}

DWORD WINAPI InitializeThread(LPVOID) {
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
        if (marker) marker << "native-los-validated-build-2026-07-24-1520\nclientBase=0x"
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
        // DX11 ESP is intentionally independent from the unresolved native
        // glow experiment. Do not mutate game render properties in the stable
        // build.
    }
    // UserCmd serializer hook disabled: restore stable pre-silent behavior.
    printf("[!] CUserCmd_to_network silent hook disabled\n");
    SetupHooks();
    return 0;
}
