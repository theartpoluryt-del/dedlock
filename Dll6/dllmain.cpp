#include "shared.h"

// Fixed-size personalization slots. The release publisher uploads these
// placeholders unchanged; the authenticated delivery service replaces all
// three slots with independently authenticated ownership records before the
// module reaches a licensed device.
#pragma section(".axwm", read)
extern "C" __declspec(allocate(".axwm"))
const unsigned char g_AxiomWatermarkSlots[3][64] = {
    "AXIOM-WATERMARK-SLOT-01",
    "AXIOM-WATERMARK-SLOT-02",
    "AXIOM-WATERMARK-SLOT-03",
};
#pragma comment(linker, "/include:g_AxiomWatermarkSlots")

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        moduleHandle = hModule;
        HMODULE loaderModule = nullptr;
        manualMappedModule = !GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&DllMain), &loaderModule);
        if (manualMappedModule) {
            wchar_t mappingName[96]{};
            swprintf_s(mappingName,
                L"Local\\Dll6_Deadlock_ManualMap_%lu",
                GetCurrentProcessId());
            manualMapInfoHandle = CreateFileMappingW(
                INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(uintptr_t)), mappingName);
            if (manualMapInfoHandle) {
                void* view = MapViewOfFile(manualMapInfoHandle, FILE_MAP_WRITE,
                                           0, 0, sizeof(uintptr_t));
                if (view) {
                    *static_cast<uintptr_t*>(view) =
                        reinterpret_cast<uintptr_t>(hModule);
                    UnmapViewOfFile(view);
                }
            }
        } else {
            DisableThreadLibraryCalls(hModule);
        }

        if (!manualMappedModule) {
            HANDLE hThread = CreateThread(
                nullptr, 0, InitializeThread, nullptr, 0, nullptr);
            if (hThread) CloseHandle(hThread);
        }
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
        if (manualMapInfoHandle) {
            CloseHandle(manualMapInfoHandle);
            manualMapInfoHandle = nullptr;
        }
        if (gameWindow && oWndProc) {
            SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
            oWndProc = nullptr;
        }

        if (consoleAttached) {
            FreeConsole();
            consoleAttached = false;
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport)
DWORD WINAPI AxiomManualMapInitialize(LPVOID) {
    // Resource APIs need the PE resource directory. Preserve every embedded
    // asset before the launcher intentionally erases the mapped PE headers.
    CacheEmbeddedResources();
    return InitializeThread(nullptr);
}
