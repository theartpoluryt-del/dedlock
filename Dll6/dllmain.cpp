#include "shared.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        moduleHandle = hModule;

        HANDLE hThread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr);

        if (hThread) CloseHandle(hThread);
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
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
