#include "shared.h"
#include <fstream>

void RestorePresentHook() {
    if (!presentVTable || !oPresent) return;

    DWORD oldProtect;
    if (VirtualProtect(&presentVTable[8], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        presentVTable[8] = reinterpret_cast<void*>(oPresent);
        DWORD unusedProtect;
        VirtualProtect(&presentVTable[8], sizeof(void*), oldProtect, &unusedProtect);
    }

    presentVTable = nullptr;
}

void ShutdownOverlay() {
    RemoveUserCmdHook();
    RemoveMeleeStateMonitor();
    if (stopHeroDiscoveryEvent) SetEvent(stopHeroDiscoveryEvent);
    if (heroDiscoveryThread) {
        WaitForSingleObject(heroDiscoveryThread, 2000);
        CloseHandle(heroDiscoveryThread);
        heroDiscoveryThread = nullptr;
    }
    if (glowApplyThread) {
        WaitForSingleObject(glowApplyThread, 2000);
        CloseHandle(glowApplyThread);
        glowApplyThread = nullptr;
    }
    if (stopHeroDiscoveryEvent) {
        CloseHandle(stopHeroDiscoveryEvent);
        stopHeroDiscoveryEvent = nullptr;
    }

    if (gameWindow && oWndProc) {
        SetWindowLongPtr(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(oWndProc));
        oWndProc = nullptr;
    }

    if (imguiInitialized) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiInitialized = false;
    }

    if (pRenderTargetView) {
        pRenderTargetView->Release();
        pRenderTargetView = nullptr;
    }
    if (depthStaging) {
        depthStaging->Release();
        depthStaging = nullptr;
    }
    ReleaseAimResources();
    depthSnapshotReady = false;
    depthWidth = depthHeight = 0;
    depthFormat = DXGI_FORMAT_UNKNOWN;
    if (pContext) {
        pContext->Release();
        pContext = nullptr;
    }
    if (pDevice) {
        pDevice->Release();
        pDevice = nullptr;
    }

    if (consoleAttached) {
        FreeConsole();
        consoleAttached = false;
    }
}

DWORD WINAPI UnloadThread(LPVOID) {
    // The VMT is restored before this thread starts; wait for the current Present call to return.
    Sleep(250);
    FreeLibraryAndExitThread(moduleHandle, 0);
}

void RequestUnload() {
    InterlockedExchange(&unloadRequested, 1);
}

HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags) {
    if (!pSwapChain || !oPresent) return E_FAIL;

    static bool presentMarkerWritten = false;
    if (!presentMarkerWritten) {
        presentMarkerWritten = true;
        std::ofstream marker(
            "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\Dll6_present.marker",
            std::ios::trunc);
        if (marker) marker << "hkPresent reached\n";
    }

    if (InterlockedCompareExchange(&unloadRequested, 0, 0) != 0) {
        ShutdownOverlay();
        RestorePresentHook();

        const HRESULT result = oPresent(pSwapChain, SyncInterval, Flags);
        if (InterlockedCompareExchange(&unloadThreadStarted, 1, 0) == 0) {
            HANDLE thread = CreateThread(nullptr, 0, UnloadThread, nullptr, 0, nullptr);
            if (thread) CloseHandle(thread);
        }
        return result;
    }

    if (!pDevice) {
        HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice);
        if (FAILED(hr) || !pDevice) return oPresent(pSwapChain, SyncInterval, Flags);

        pDevice->GetImmediateContext(&pContext);
        if (!pContext) {
            pDevice->Release();
            pDevice = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        DXGI_SWAP_CHAIN_DESC desc{};
        if (FAILED(pSwapChain->GetDesc(&desc)) || !desc.OutputWindow) {
            pContext->Release();
            pDevice->Release();
            pDevice = nullptr;
            pContext = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
        gameWindow = desc.OutputWindow;

        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        ImGui::StyleColorsDark();

        if (!ImGui_ImplWin32_Init(gameWindow)) {
            ImGui::DestroyContext();
            pContext->Release();
            pDevice->Release();
            pDevice = nullptr;
            pContext = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!ImGui_ImplDX11_Init(pDevice, pContext)) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            pContext->Release();
            pDevice->Release();
            pDevice = nullptr;
            pContext = nullptr;
            return oPresent(pSwapChain, SyncInterval, Flags);
        }

        if (!oWndProc) {
            oWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtr(gameWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
        }
        imguiInitialized = true;
        SetMenuOpen(menuOpen);
    }

    if (!imguiInitialized) return oPresent(pSwapChain, SyncInterval, Flags);

    if (!pRenderTargetView) {
        ID3D11Texture2D* pBackBuffer = nullptr;
        if (FAILED(pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer)) || !pBackBuffer) {
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
        if (FAILED(pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &pRenderTargetView))) {
            pBackBuffer->Release();
            return oPresent(pSwapChain, SyncInterval, Flags);
        }
        pBackBuffer->Release();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    const bool depthReady = CaptureDepthSnapshot();
    const int depthState = depthReady ? 1 : 0;
    if (depthState != depthDiagnosticState) {
        const char* status = depthReady ? "ready" : "unavailable";
        printf("[Aim] DX11 depth visibility: %s\n", status);
        FILE* diagnostic = nullptr;
        if (fopen_s(&diagnostic, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\aim_visibility.log", "a") == 0 && diagnostic) {
            fprintf(diagnostic, "depth=%s width=%u height=%u format=%u\n", status, depthWidth, depthHeight, static_cast<unsigned>(depthFormat));
            fclose(diagnostic);
        }
        depthDiagnosticState = depthState;
    }
    const auto players = GetPlayers();
    AutoParry(players);
    AimAtClosestEnemy(players);
    RenderESP(players);
    RenderMenu(players.size());

    ImGui::EndFrame();
    ImGui::Render();

    pContext->OMSetRenderTargets(1, &pRenderTargetView, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return oPresent(pSwapChain, SyncInterval, Flags);
}

LRESULT __stdcall hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == ApplyGlowMessage) {
        const uintptr_t glow = static_cast<uintptr_t>(wParam);
        const uintptr_t entity = glow - Offsets::Glow;
        const bool applied = NotifyGlowTypeChanged(glow);
        printf("[Glow] entity=0x%p property=0x%p callback=%s\n",
               reinterpret_cast<void*>(entity), reinterpret_cast<void*>(glow),
               applied ? "ok" : "failed");
        std::lock_guard lock(glowMutex);
        queuedGlows.erase(entity);
        if (applied) registeredGlows.insert(entity);
        return 0;
    }

    if (uMsg == WM_KEYUP && wParam == VK_INSERT) {
        SetMenuOpen(!menuOpen);
        return 0;
    }

    if (menuOpen && imguiInitialized && ImGui::GetCurrentContext()) {
        if (uMsg == WM_SETCURSOR) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }

        if (ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam)) {
            return 1;
        }

        switch (uMsg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_CHAR:
            return 1;
        }
    }

    return oWndProc ? CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam)
                    : DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void* DetourFunc(BYTE* src, const BYTE* dst, const int len) {
    BYTE* jmp = (BYTE*)malloc(len + 5);
    DWORD dwback;

    VirtualProtect(src, len, PAGE_READWRITE, &dwback);

    memcpy(jmp, src, len);
    jmp += len;
    jmp[0] = 0xE9;
    *(DWORD*)(jmp + 1) = (DWORD)(src + len - jmp) - 5;

    src[0] = 0xE9;
    *(DWORD*)(src + 1) = (DWORD)(dst - src) - 5;

    VirtualProtect(src, len, dwback, &dwback);
    return (jmp - len);
}
