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
    SaveConfig();
    freeCam = false;
    RemoveUserCmdHook();
    RemoveInputLockHooks();
    RemoveSoundEventHook();
    RemoveModelGlowHook();
    RemoveOrbEntityHooks();
    RemoveMeleeStateMonitor();
    if (stopHeroDiscoveryEvent) SetEvent(stopHeroDiscoveryEvent);
    if (heroDiscoveryThread) {
        WaitForSingleObject(heroDiscoveryThread, 2000);
        CloseHandle(heroDiscoveryThread);
        heroDiscoveryThread = nullptr;
    }
    if (farmTargetThread) {
        WaitForSingleObject(farmTargetThread, 2000);
        CloseHandle(farmTargetThread);
        farmTargetThread = nullptr;
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
        // The model hook is installed during initialization, but the DX11
        // draw hooks can only be attached after the immediate context exists.
        InstallModelGlowHook();
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

    // Keep a depth snapshot for visibility. The current physics filter can
    // report a clear ray for world geometry, while the depth buffer still
    // gives a reliable screen-space occlusion result.
    // Entity discovery and assist logic are heavier than ImGui drawing. Keep
    // one coherent snapshot and update it at a bounded cadence so high-refresh
    // Present calls do not starve the render path.
    static ULONGLONG lastAssistUpdate = 0;
    static std::vector<PlayerData> visualSnapshot;
    const ULONGLONG now = GetTickCount64();
    // Rebuild the visual snapshot on every Present so ESP positions are
    // refreshed once per rendered frame, including 144 Hz displays.
    const std::vector<PlayerData> nextSnapshot = GetPlayers();
    static ULONGLONG lastNonEmptySnapshot = 0;
    if (!nextSnapshot.empty()) {
        visualSnapshot = nextSnapshot;
        lastNonEmptySnapshot = now;
    } else if (lastNonEmptySnapshot == 0 || now - lastNonEmptySnapshot > 150) {
        visualSnapshot.clear();
    }
    if (lastAssistUpdate == 0 || now - lastAssistUpdate >= 16) {
        AutoParry(visualSnapshot);
        AimAtClosestEnemy(visualSnapshot);
        FarmAimAssist(visualSnapshot);
        AutoLastHitOrbs();
        lastAssistUpdate = now;
    }
    RenderESP(visualSnapshot);
    RenderMenu(visualSnapshot.size());

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
        if (farmKeyCapture) {
            const bool keyboardKey = uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN;
            const bool mouseKey = uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN ||
                                  uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN;
            if (keyboardKey || mouseKey) {
                if (keyboardKey) farmAssistKey = static_cast<int>(wParam);
                else if (uMsg == WM_LBUTTONDOWN) farmAssistKey = VK_LBUTTON;
                else if (uMsg == WM_RBUTTONDOWN) farmAssistKey = VK_RBUTTON;
                else if (uMsg == WM_MBUTTONDOWN) farmAssistKey = VK_MBUTTON;
                else farmAssistKey = HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                farmKeyCapture = false;
                return 1;
            }
        }
        if (autoLastHitOrbsKeyCapture) {
            const bool keyboardKey = uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN;
            const bool mouseKey = uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN ||
                                  uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN;
            if (keyboardKey || mouseKey) {
                if (keyboardKey) autoLastHitOrbsKey = static_cast<int>(wParam);
                else if (uMsg == WM_LBUTTONDOWN) autoLastHitOrbsKey = VK_LBUTTON;
                else if (uMsg == WM_RBUTTONDOWN) autoLastHitOrbsKey = VK_RBUTTON;
                else if (uMsg == WM_MBUTTONDOWN) autoLastHitOrbsKey = VK_MBUTTON;
                else autoLastHitOrbsKey = HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                autoLastHitOrbsKeyCapture = false;
                return 1;
            }
        }
        if (aimKeyCapture) {
            const bool keyboardKey = uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN;
            const bool mouseKey = uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN ||
                                  uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN;
            if (keyboardKey || mouseKey) {
                if (keyboardKey) aimAssistKey = static_cast<int>(wParam);
                else if (uMsg == WM_LBUTTONDOWN) aimAssistKey = VK_LBUTTON;
                else if (uMsg == WM_RBUTTONDOWN) aimAssistKey = VK_RBUTTON;
                else if (uMsg == WM_MBUTTONDOWN) aimAssistKey = VK_MBUTTON;
                else aimAssistKey = HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                aimKeyCapture = false;
                return 1;
            }
        }
        if (freeCamKeyCapture) {
            const bool keyboardKey = uMsg == WM_KEYDOWN || uMsg == WM_SYSKEYDOWN;
            const bool mouseKey = uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN ||
                                  uMsg == WM_MBUTTONDOWN || uMsg == WM_XBUTTONDOWN;
            if (keyboardKey || mouseKey) {
                if (keyboardKey) freeCamKey = static_cast<int>(wParam);
                else if (uMsg == WM_LBUTTONDOWN) freeCamKey = VK_LBUTTON;
                else if (uMsg == WM_RBUTTONDOWN) freeCamKey = VK_RBUTTON;
                else if (uMsg == WM_MBUTTONDOWN) freeCamKey = VK_MBUTTON;
                else freeCamKey = HIWORD(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
                freeCamKeyCapture = false;
                return 1;
            }
        }
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
        case WM_MOUSEHWHEEL:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_INPUT:
        case WM_MOUSEACTIVATE:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
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
