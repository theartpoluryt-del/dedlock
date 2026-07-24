#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <psapi.h>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cmath>
#include <cfloat>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "psapi.lib")

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================
// OFFSETS
// ============================================================
namespace Offsets {
    // Globals in client.dll from the live signature scan.
    constexpr uintptr_t GameEntitySystem = 0x38B78C8;
    constexpr uintptr_t ViewMatrix = 0x3799830;

    // CBasePlayerController / C_BasePlayerPawn / C_BaseEntity schema fields.
    constexpr uintptr_t ControllerPawn = 0x6BC;
    constexpr uintptr_t IsLocalPlayerController = 0x780;
    constexpr uintptr_t PawnController = 0x10B0;
    constexpr uintptr_t GameSceneNode = 0x330;
    constexpr uintptr_t CollisionProperty = 0x340;
    constexpr uintptr_t SceneNodeAbsOrigin = 0xC8;
    constexpr uintptr_t CollisionMins = 0x40;
    constexpr uintptr_t CollisionMaxs = 0x4C;
    constexpr uintptr_t Glow = 0x7F0;
    constexpr uintptr_t GlowColor = 0x08;
    constexpr uintptr_t GlowType = 0x30;
    constexpr uintptr_t GlowTeam = 0x34;
    constexpr uintptr_t GlowRange = 0x38;
    constexpr uintptr_t GlowRangeMin = 0x3C;
    constexpr uintptr_t GlowColorOverride = 0x40;
    constexpr uintptr_t GlowFlashing = 0x44;
    constexpr uintptr_t GlowTime = 0x48;
    constexpr uintptr_t GlowStartTime = 0x4C;
    // CGlowProperty has two adjacent flags. 0x50 only makes the entity
    // eligible for the screen-highlight pass; 0x51 actually turns the
    // renderer glow on. Writing only 0x50 registers a property but produces
    // no silhouette.
    constexpr uintptr_t IsEligibleForScreenHighlight = 0x50;
    constexpr uintptr_t IsGlowing = 0x51;
    // CGlowProperty::OnGlowTypeChanged in the current client build.
    constexpr uintptr_t OnGlowTypeChanged = 0x162F8A0;
    constexpr uintptr_t MaxHealth = 0x350;
    constexpr uintptr_t Health = 0x354;
    constexpr uintptr_t LifeState = 0x35C;
    constexpr uintptr_t Team = 0x3F3;
    constexpr uintptr_t Pos = 0x1098;

    constexpr uint32_t HandleIndexMask = 0x7FFF;
    constexpr uint32_t HandleChunkShift = 9;
    constexpr uint32_t HandleChunkMask = 0x1FF;
    // Entity handles use 15 index bits. Enemy hero pawns are commonly above
    // the first 2048 slots, so enumerate the complete addressable range.
    constexpr uint32_t MaxEntityIndex = HandleIndexMask + 1;
    constexpr uintptr_t EntityChunks = 0x10;
    constexpr uintptr_t EntityChunkStride = sizeof(uintptr_t);
    constexpr uintptr_t EntityStride = 0x78;
}

struct Vector3 { float x, y, z; };
struct Vector2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };
struct ColorRGBA { uint8_t r, g, b, a; };

uintptr_t clientBase = 0;
bool menuOpen = true;
bool drawEsp = true;
bool drawBoxes = true;
bool drawHealth = true;
bool aimAssist = false;
float aimFov = 180.0f;
float aimSmooth = 6.0f;
bool imguiInitialized = false;
bool consoleAttached = false;

// A screen-space target is not enough for aim assistance: it is also present
// when the pawn is behind a wall.  The DX11 depth buffer is sampled at the
// target point and the aim is disabled when the depth buffer is unavailable.
ID3D11Texture2D* depthStaging = nullptr;
UINT depthWidth = 0;
UINT depthHeight = 0;
DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
bool depthSnapshotReady = false;
int depthDiagnosticState = -1;
Matrix4x4 currentViewMatrix{};
bool currentViewMatrixReady = false;

ID3D11Device* pDevice = nullptr;
ID3D11DeviceContext* pContext = nullptr;
ID3D11RenderTargetView* pRenderTargetView = nullptr;
HWND gameWindow = nullptr;
WNDPROC oWndProc = nullptr;
HMODULE moduleHandle = nullptr;
void** presentVTable = nullptr;
volatile LONG unloadRequested = 0;
volatile LONG unloadThreadStarted = 0;
constexpr UINT ApplyGlowMessage = WM_APP + 0x4D;
std::mutex glowMutex;
std::unordered_set<uintptr_t> registeredGlows;
std::unordered_set<uintptr_t> queuedGlows;

template<typename T>
T Read(uintptr_t address) {
    T value{};
    if (!address) return value;

    __try {
        value = *reinterpret_cast<T*>(address);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        value = T{};
    }

    return value;
}

template<typename T>
void Write(uintptr_t address, const T& value) {
    if (!address) return;

    __try {
        *reinterpret_cast<T*>(address) = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool WorldToScreen(const Vector3& pos, Vector2& screen, const Matrix4x4& matrix) {
    float w = matrix.m[3][0] * pos.x + matrix.m[3][1] * pos.y + matrix.m[3][2] * pos.z + matrix.m[3][3];
    if (w < 0.01f) return false;

    float x = matrix.m[0][0] * pos.x + matrix.m[0][1] * pos.y + matrix.m[0][2] * pos.z + matrix.m[0][3];
    float y = matrix.m[1][0] * pos.x + matrix.m[1][1] * pos.y + matrix.m[1][2] * pos.z + matrix.m[1][3];

    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f) return false;

    float invW = 1.0f / w;
    screen.x = (displaySize.x * 0.5f) + (x * invW * displaySize.x * 0.5f);
    screen.y = (displaySize.y * 0.5f) - (y * invW * displaySize.y * 0.5f);
    return true;
}

bool CaptureDepthSnapshot() {
    depthSnapshotReady = false;
    if (!pContext || !pDevice) return false;

    ID3D11DepthStencilView* depthView = nullptr;
    pContext->OMGetRenderTargets(0, nullptr, &depthView);
    if (!depthView) return false;

    ID3D11Resource* resource = nullptr;
    depthView->GetResource(&resource);
    depthView->Release();
    if (!resource) return false;

    ID3D11Texture2D* depthTexture = nullptr;
    const HRESULT queryResult = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTexture));
    resource->Release();
    if (FAILED(queryResult) || !depthTexture) return false;

    D3D11_TEXTURE2D_DESC desc{};
    depthTexture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_D32_FLOAT && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
         desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && desc.Format != DXGI_FORMAT_R24G8_TYPELESS &&
         desc.Format != DXGI_FORMAT_D16_UNORM)) {
        depthTexture->Release();
        return false;
    }

    if (!depthStaging || depthWidth != desc.Width || depthHeight != desc.Height || depthFormat != desc.Format) {
        if (depthStaging) depthStaging->Release();
        depthStaging = nullptr;
        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        if (FAILED(pDevice->CreateTexture2D(&stagingDesc, nullptr, &depthStaging))) {
            depthWidth = depthHeight = 0;
            depthFormat = DXGI_FORMAT_UNKNOWN;
            depthTexture->Release();
            return false;
        }
        depthWidth = desc.Width;
        depthHeight = desc.Height;
        depthFormat = desc.Format;
    }

    pContext->CopyResource(depthStaging, depthTexture);
    depthTexture->Release();
    depthSnapshotReady = true;
    return true;
}

bool ReadDepthAt(float x, float y, float& depth) {
    depth = 0.0f;
    if (!depthSnapshotReady || !depthStaging || depthWidth == 0 || depthHeight == 0) return false;
    const LONG px = static_cast<LONG>(std::clamp(x, 0.0f, static_cast<float>(depthWidth - 1)));
    const LONG py = static_cast<LONG>(std::clamp(y, 0.0f, static_cast<float>(depthHeight - 1)));

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(pContext->Map(depthStaging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * py;
    if (depthFormat == DXGI_FORMAT_D32_FLOAT || depthFormat == DXGI_FORMAT_R32_TYPELESS) {
        depth = *reinterpret_cast<const float*>(row + px * sizeof(float));
    } else if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFormat == DXGI_FORMAT_R24G8_TYPELESS) {
        const uint32_t packed = *reinterpret_cast<const uint32_t*>(row + px * sizeof(uint32_t));
        // The live resource reports R24G8_TYPELESS (44): the R24 depth
        // component occupies the low 24 bits; G8 is the high byte.
        depth = static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f;
    } else {
        const uint16_t packed = *reinterpret_cast<const uint16_t*>(row + px * sizeof(uint16_t));
        depth = static_cast<float>(packed) / 65535.0f;
    }
    pContext->Unmap(depthStaging, 0);
    return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
}

struct PlayerData {
    Vector3 pos;
    float boxLeft;
    float boxTop;
    float boxRight;
    float boxBottom;
    int health;
    int maxHealth;
    int team;
    float distance;
};

bool GetAimPointScreen(const PlayerData& player, float height, Vector2& screen) {
    if (!currentViewMatrixReady) return false;
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    return WorldToScreen(aimPoint, screen, currentViewMatrix);
}

bool IsAimPointVisible(const PlayerData& player, float height, float screenX, float screenY) {
    if (!depthSnapshotReady || !currentViewMatrixReady) return false;

    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    const Matrix4x4& matrix = currentViewMatrix;
    const float w = matrix.m[3][0] * aimPoint.x + matrix.m[3][1] * aimPoint.y +
                    matrix.m[3][2] * aimPoint.z + matrix.m[3][3];
    if (w < 0.01f) return false;
    const float clipZ = matrix.m[2][0] * aimPoint.x + matrix.m[2][1] * aimPoint.y +
                        matrix.m[2][2] * aimPoint.z + matrix.m[2][3];
    const float targetDepth = clipZ / w;
    if (!std::isfinite(targetDepth) || targetDepth < 0.0f || targetDepth > 1.0f) return false;

    // Compare the actual depth at the target pixel with the projected model
    // depth.  This is deliberately an absolute comparison: it works for both
    // regular-Z and reversed-Z projections and cannot classify a wall as a
    // visible target merely because the depth ordering is inverted.
    constexpr float depthBias = 0.025f;
    constexpr float sampleOffsets[][2] = {
        { 0.0f, 0.0f }, { -3.0f, 0.0f }, { 3.0f, 0.0f },
        { 0.0f, -3.0f }, { 0.0f, 3.0f }
    };
    static int depthSamplesLogged = 0;
    for (const auto& offset : sampleOffsets) {
        float sceneDepth = 0.0f;
        if (ReadDepthAt(screenX + offset[0], screenY + offset[1], sceneDepth)) {
            if (depthSamplesLogged < 40 && offset[0] == 0.0f && offset[1] == 0.0f) {
                FILE* diagnostic = nullptr;
                if (fopen_s(&diagnostic, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\aim_visibility.log", "a") == 0 && diagnostic) {
                    fprintf(diagnostic, "sample height=%.1f x=%.1f y=%.1f w=%.3f target=%.6f scene=%.6f diff=%.6f\\n", height, screenX, screenY, w, targetDepth, sceneDepth, std::fabs(sceneDepth - targetDepth));
                    fclose(diagnostic);
                }
                ++depthSamplesLogged;
            }
            if (std::fabs(sceneDepth - targetDepth) <= depthBias) return true;
        }
    }
    return false;
}

void AimAtClosestEnemy(const std::vector<PlayerData>& players) {
    if (!aimAssist || menuOpen || !(GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        !depthSnapshotReady || !currentViewMatrixReady) return;

    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;
    const PlayerData* best = nullptr;
    Vector2 bestAimScreen{};
    float bestDistance = aimFov * aimFov;
    size_t visibleTargets = 0;
    size_t testedTargets = 0;

    for (const auto& player : players) {
        bool targetVisible = false;
        Vector2 visibleAimScreen{};
        float visibleDistance = FLT_MAX;
        // Try torso, chest and head. A single fixed point can lie inside the
        // model and therefore have a different depth than its rendered surface.
        for (const float height : { 42.0f, 54.0f, 66.0f }) {
            Vector2 aimScreen{};
            if (!GetAimPointScreen(player, height, aimScreen)) continue;
            const float dx = aimScreen.x - cx;
            const float dy = aimScreen.y - cy;
            const float distance = dx * dx + dy * dy;
            if (distance >= bestDistance) continue;
            ++testedTargets;
            if (!IsAimPointVisible(player, height, aimScreen.x, aimScreen.y)) continue;
            targetVisible = true;
            if (distance < visibleDistance) {
                visibleDistance = distance;
                visibleAimScreen = aimScreen;
            }
        }
        if (targetVisible) {
            ++visibleTargets;
            if (visibleDistance < bestDistance) {
                bestDistance = visibleDistance;
                best = &player;
                // Store the point that passed the depth test for the final move.
                bestAimScreen = visibleAimScreen;
            }
        }
    }

    static size_t lastTestedTargets = static_cast<size_t>(-1);
    static size_t lastVisibleTargets = static_cast<size_t>(-1);
    if (testedTargets != lastTestedTargets || visibleTargets != lastVisibleTargets) {
        printf("[Aim] visibility tested=%zu visible=%zu blocked=%zu\n",
               testedTargets, visibleTargets, testedTargets - visibleTargets);
        FILE* diagnostic = nullptr;
        if (fopen_s(&diagnostic, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\aim_visibility.log", "a") == 0 && diagnostic) {
            fprintf(diagnostic, "tested=%zu visible=%zu blocked=%zu\\n", testedTargets, visibleTargets, testedTargets - visibleTargets);
            fclose(diagnostic);
        }
        lastTestedTargets = testedTargets;
        lastVisibleTargets = visibleTargets;
    }

    if (!best) return;
    const float targetX = bestAimScreen.x;
    const float targetY = bestAimScreen.y;
    const LONG moveX = static_cast<LONG>((targetX - cx) / (aimSmooth < 1.0f ? 1.0f : aimSmooth));
    const LONG moveY = static_cast<LONG>((targetY - cy) / (aimSmooth < 1.0f ? 1.0f : aimSmooth));
    if (moveX || moveY) mouse_event(MOUSEEVENTF_MOVE, moveX, moveY, 0, 0);
}

struct EspStatus {
    bool entitySystemReady = false;
    bool localPawnFound = false;
    bool heroPawnsFound = false;
};

EspStatus espStatus;
std::unordered_map<uintptr_t, bool> combatVTables;
std::vector<uintptr_t> heroVTables;
std::vector<uintptr_t> heroPawns;
std::mutex heroPawnsMutex;
HANDLE heroDiscoveryThread = nullptr;
HANDLE glowApplyThread = nullptr;
HANDLE stopHeroDiscoveryEvent = nullptr;

void RequestUnload();
LRESULT __stdcall hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

struct WindowSearchData {
    DWORD processId;
    HWND window;
};

BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM lParam) {
    auto* data = reinterpret_cast<WindowSearchData*>(lParam);
    DWORD windowProcessId = 0;
    GetWindowThreadProcessId(window, &windowProcessId);
    if (windowProcessId != data->processId || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) {
        return TRUE;
    }

    data->window = window;
    return FALSE;
}

bool HookGameWindow() {
    if (oWndProc) return true;

    const DWORD processId = GetCurrentProcessId();
    HWND window = FindWindowA(nullptr, "Deadlock");
    DWORD windowProcessId = 0;
    if (window) GetWindowThreadProcessId(window, &windowProcessId);

    if (!window || windowProcessId != processId) {
        WindowSearchData search{ processId, nullptr };
        EnumWindows(FindGameWindowCallback, reinterpret_cast<LPARAM>(&search));
        window = search.window;
    }
    if (!window) return false;

    SetLastError(0);
    const auto previous = reinterpret_cast<WNDPROC>(
        SetWindowLongPtr(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hkWndProc)));
    if (!previous && GetLastError() != ERROR_SUCCESS) return false;

    gameWindow = window;
    oWndProc = previous;
    return true;
}

uintptr_t ResolveEntity(uint32_t handle) {
    const uintptr_t entitySystem = Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    if (!entitySystem) return 0;

    const uint32_t index = handle & Offsets::HandleIndexMask;
    const uintptr_t chunk = Read<uintptr_t>(
        entitySystem + Offsets::EntityChunks + Offsets::EntityChunkStride * (index >> Offsets::HandleChunkShift));
    if (!chunk) return 0;

    return Read<uintptr_t>(chunk + Offsets::EntityStride * (index & Offsets::HandleChunkMask));
}

bool GetEntityPosition(uintptr_t entity, Vector3& position) {
    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    position = Read<Vector3>(sceneNode + Offsets::SceneNodeAbsOrigin);
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

bool GetEntityScreenBounds(uintptr_t entity, const Vector3& origin, const Matrix4x4& matrix,
                           float& left, float& top, float& right, float& bottom) {
    const uintptr_t collision = Read<uintptr_t>(entity + Offsets::CollisionProperty);
    if (!collision) return false;

    const Vector3 mins = Read<Vector3>(collision + Offsets::CollisionMins);
    const Vector3 maxs = Read<Vector3>(collision + Offsets::CollisionMaxs);
    if (!std::isfinite(mins.x) || !std::isfinite(mins.y) || !std::isfinite(mins.z) ||
        !std::isfinite(maxs.x) || !std::isfinite(maxs.y) || !std::isfinite(maxs.z) ||
        mins.x >= maxs.x || mins.y >= maxs.y || mins.z >= maxs.z) {
        return false;
    }

    left = top = FLT_MAX;
    right = bottom = -FLT_MAX;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                const Vector3 corner{
                    origin.x + (x ? maxs.x : mins.x),
                    origin.y + (y ? maxs.y : mins.y),
                    origin.z + (z ? maxs.z : mins.z)
                };
                Vector2 screen{};
                if (!WorldToScreen(corner, screen, matrix)) return false;
                left = (std::min)(left, screen.x);
                top = (std::min)(top, screen.y);
                right = (std::max)(right, screen.x);
                bottom = (std::max)(bottom, screen.y);
            }
        }
    }

    // Collision bounds end below several hero head models, so extend only the top
    // proportionally. The bottom remains at the feet for every hero height.
    top -= (bottom - top) * 0.30f;

    // A small horizontal margin keeps the outline outside the model at oblique angles.
    const float margin = (right - left) * 0.08f;
    left -= margin;
    right += margin;
    return right > left && bottom > top;
}

bool NotifyGlowTypeChanged(uintptr_t glow) {
    using GlowTypeChangedFn = void(__fastcall*)(void*);
    const auto onGlowTypeChanged = reinterpret_cast<GlowTypeChangedFn>(clientBase + Offsets::OnGlowTypeChanged);
    __try {
        onGlowTypeChanged(reinterpret_cast<void*>(glow));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ApplyHeroGlow(uintptr_t entity) {
    const uintptr_t glow = entity + Offsets::Glow;
    bool shouldNotify = false;

    // Keep the property values refreshed. The engine can reset CGlowProperty
    // while the pawn is being spawned or when its network state changes.
    {
        std::lock_guard lock(glowMutex);
        if (registeredGlows.find(entity) == registeredGlows.end() &&
            queuedGlows.insert(entity).second) {
            shouldNotify = true;
        }
    }

    Write<Vector3>(glow + Offsets::GlowColor, { 0.0f, 1.0f, 0.15f });
    // Types 1-3 are submitted to the renderer; 3 is the full model-outline pass.
    Write<int>(glow + Offsets::GlowType, 3);
    // Zero means no team restriction for the glow pass.
    // The renderer evaluates this against the observing player's team. Team 0
    // registers the property but does not expose it to the local camera.
    Write<int>(glow + Offsets::GlowTeam, 2);
    Write<int>(glow + Offsets::GlowRange, 100000);
    Write<int>(glow + Offsets::GlowRangeMin, 0);
    Write<ColorRGBA>(glow + Offsets::GlowColorOverride, { 0, 255, 38, 255 });
    Write<bool>(glow + Offsets::GlowFlashing, false);
    Write<float>(glow + Offsets::GlowTime, 0.0f);
    Write<float>(glow + Offsets::GlowStartTime, 0.0f);
    Write<bool>(glow + Offsets::IsEligibleForScreenHighlight, true);
    Write<bool>(glow + Offsets::IsGlowing, true);

    // Direct field writes are not enough: the engine adds the property to its
    // render list from this network-change callback.
    // WndProc runs on Deadlock's main thread. Post there instead of mutating
    // the engine-owned render list from the Present/render thread.
    if (shouldNotify && gameWindow && oWndProc) {
        SendMessage(gameWindow, ApplyGlowMessage, static_cast<WPARAM>(glow), 0);
    } else if (shouldNotify) {
        std::lock_guard lock(glowMutex);
        queuedGlows.erase(entity);
    }
}

void DiscoverHeroVTables() {
    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase), &moduleInfo, sizeof(moduleInfo))) return;

    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    const size_t imageSize = moduleInfo.SizeOfImage;
    constexpr char typeName[] = ".?AVC_CitadelPlayerPawn@@";
    constexpr size_t typeNameLength = sizeof(typeName) - 1;

    size_t typeNameOffset = imageSize;
    for (size_t offset = 16; offset + typeNameLength <= imageSize; ++offset) {
        if (memcmp(image + offset, typeName, typeNameLength) == 0) {
            typeNameOffset = offset;
            break;
        }
    }
    if (typeNameOffset == imageSize) return;

    const uint32_t typeDescriptorRva = static_cast<uint32_t>(typeNameOffset - 16);
    for (size_t locatorOffset = 0; locatorOffset + 24 <= imageSize; ++locatorOffset) {
        uint32_t descriptor{};
        uint32_t self{};
        memcpy(&descriptor, image + locatorOffset + 12, sizeof(descriptor));
        memcpy(&self, image + locatorOffset + 20, sizeof(self));
        if (descriptor != typeDescriptorRva || self != locatorOffset) continue;

        const uintptr_t locator = clientBase + locatorOffset;
        for (size_t pointerOffset = 0; pointerOffset + sizeof(uintptr_t) <= imageSize; pointerOffset += sizeof(uintptr_t)) {
            uintptr_t value{};
            memcpy(&value, image + pointerOffset, sizeof(value));
            if (value == locator) heroVTables.push_back(clientBase + pointerOffset + sizeof(uintptr_t));
        }
    }
}

bool IsCombatEntity(uintptr_t entity);

void RefreshHeroPawns() {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t end = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    std::vector<uintptr_t> found;

    while (cursor < end && WaitForSingleObject(stopHeroDiscoveryEvent, 0) != WAIT_OBJECT_0) {
        MEMORY_BASIC_INFORMATION memoryInfo{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memoryInfo, sizeof(memoryInfo))) break;
        const uintptr_t next = cursor + memoryInfo.RegionSize;
        const DWORD readablePages = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                   PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        const bool readable = memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & PAGE_GUARD) &&
                              memoryInfo.Protect != PAGE_NOACCESS && (memoryInfo.Protect & readablePages);
        if (readable && memoryInfo.Type == MEM_PRIVATE && !heroVTables.empty()) {
            const uintptr_t regionEnd = cursor + memoryInfo.RegionSize;
            for (uintptr_t address = cursor; address + sizeof(uintptr_t) <= regionEnd; address += sizeof(uintptr_t)) {
                const uintptr_t vtable = Read<uintptr_t>(address);
                if (std::find(heroVTables.begin(), heroVTables.end(), vtable) == heroVTables.end()) continue;
                const int health = Read<int>(address + Offsets::Health);
                const uint8_t team = Read<uint8_t>(address + Offsets::Team);
                Vector3 position{};
                if (health < 0 || health > 10000 || (team != 2 && team != 3) || !GetEntityPosition(address, position)) continue;
                found.push_back(address);
            }
        }
        cursor = next;
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    const size_t pawnCount = found.size();
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        heroPawns = std::move(found);
    }

    static size_t lastPawnCount = SIZE_MAX;
    if (pawnCount != lastPawnCount) {
        lastPawnCount = pawnCount;
        printf("[+] Hero pawns: %zu\n", lastPawnCount);
    }
}

DWORD WINAPI HeroDiscoveryWorker(LPVOID) {
    DiscoverHeroVTables();
    while (WaitForSingleObject(stopHeroDiscoveryEvent, 0) != WAIT_OBJECT_0) {
        RefreshHeroPawns();
        if (WaitForSingleObject(stopHeroDiscoveryEvent, 1000) == WAIT_OBJECT_0) break;
    }
    return 0;
}

DWORD WINAPI GlowApplyWorker(LPVOID) {
    while (WaitForSingleObject(stopHeroDiscoveryEvent, 0) != WAIT_OBJECT_0) {
        if (!oWndProc) HookGameWindow();

        std::vector<uintptr_t> pawns;
        {
            std::lock_guard<std::mutex> lock(heroPawnsMutex);
            pawns = heroPawns;
        }
        for (const uintptr_t pawn : pawns) {
            const int health = Read<int>(pawn + Offsets::Health);
            const uint8_t lifeState = Read<uint8_t>(pawn + Offsets::LifeState);
            const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
            if (health > 0 && lifeState == 0 && (team == 2 || team == 3)) ApplyHeroGlow(pawn);
        }

        // The renderer clears m_bGlowing after consuming it. Reassert it on
        // the render cadence while keeping the expensive pawn scan at 1 Hz.
        if (WaitForSingleObject(stopHeroDiscoveryEvent, 16) == WAIT_OBJECT_0) break;
    }
    return 0;
}

bool IsCombatEntity(uintptr_t entity) {
    const uintptr_t vtable = Read<uintptr_t>(entity);
    if (!vtable) return false;

    const auto cached = combatVTables.find(vtable);
    if (cached != combatVTables.end()) return cached->second;

    // MSVC stores a CompleteObjectLocator immediately before the vtable.
    const uintptr_t objectLocator = Read<uintptr_t>(vtable - sizeof(uintptr_t));
    const uint32_t typeDescriptorRva = Read<uint32_t>(objectLocator + 0x0C);
    const uintptr_t typeDescriptor = clientBase + typeDescriptorRva;

    std::string typeName;
    typeName.reserve(96);
    for (uintptr_t index = 0; index < 96; ++index) {
        const char character = Read<char>(typeDescriptor + 0x10 + index);
        if (character == '\0') break;
        typeName.push_back(character);
    }

    // These are the replicated combat actors in the current client build. Keeping
    // the check class-based prevents props, towers, pickups, and abilities from ESP.
    const bool isCombatant = typeName.find("CitadelPlayerPawn") != std::string::npos ||
                            typeName.find("NPC_Trooper") != std::string::npos;

    combatVTables.emplace(vtable, isCombatant);
    return isCombatant;
}

void SetMenuOpen(bool open) {
    menuOpen = open;

    if (imguiInitialized && ImGui::GetCurrentContext()) {
        ImGui::GetIO().MouseDrawCursor = open;
    }

    if (open) {
        ClipCursor(nullptr);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

std::vector<PlayerData> GetPlayers() {
    std::vector<PlayerData> players;
    espStatus = {};

    if (!clientBase) return players;

    Matrix4x4 viewMatrix = Read<Matrix4x4>(clientBase + Offsets::ViewMatrix);
    currentViewMatrix = viewMatrix;
    currentViewMatrixReady = true;
    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }
    espStatus.heroPawnsFound = !pawns.empty();

    Vector3 localPos{};
    bool localPositionFound = false;
    // Prefer the actual locally controlled pawn when the controller is present in the entity system.
    for (const uintptr_t pawn : pawns) {
        const uintptr_t controller = ResolveEntity(Read<uint32_t>(pawn + Offsets::PawnController));
        if (controller && Read<uint8_t>(controller + Offsets::IsLocalPlayerController) == 1 &&
            GetEntityPosition(pawn, localPos)) {
            localPositionFound = true;
            break;
        }
    }
    // Training bots may not expose their controller in the regular entity list. In that case,
    // a non-enemy hero is still a much more useful distance reference than a constant value.
    if (!localPositionFound) {
        for (const uintptr_t pawn : pawns) {
            if (Read<uint8_t>(pawn + Offsets::Team) != 3 && GetEntityPosition(pawn, localPos)) {
                localPositionFound = true;
                break;
            }
        }
    }
    espStatus.localPawnFound = localPositionFound;

    for (const uintptr_t entity : pawns) {
        const int health = Read<int>(entity + Offsets::Health);
        if (health <= 0) continue;

        const uint8_t lifeState = Read<uint8_t>(entity + Offsets::LifeState);
        if (lifeState != 0) continue;

        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
        if (team != 3) continue;

        Vector3 pos{};
        if (!GetEntityPosition(entity, pos)) continue;

        PlayerData player;
        player.pos = pos;
        player.health = health;
        player.maxHealth = Read<int>(entity + Offsets::MaxHealth);
        player.team = team;
        const float dx = pos.x - localPos.x;
        const float dy = pos.y - localPos.y;
        const float dz = pos.z - localPos.z;
        // Source coordinates are in Hammer Units; 39.37 units correspond to 1 meter.
        player.distance = localPositionFound
                              ? std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f
                              : 0.0f;
        if (!std::isfinite(player.distance)) continue;

        if (GetEntityScreenBounds(entity, pos, viewMatrix, player.boxLeft, player.boxTop, player.boxRight, player.boxBottom)) {
            players.push_back(player);
        }
    }

    return players;
}

void RenderESP(const std::vector<PlayerData>& players) {
    if (!drawEsp) return;

    auto drawList = ImGui::GetBackgroundDrawList();

    for (const auto& player : players) {
        const float screenX = (player.boxLeft + player.boxRight) * 0.5f;
        const float screenY = player.boxBottom;
        const float boxTop = player.boxTop;
        const float boxHeight = player.boxBottom - player.boxTop;
        const float boxWidth = player.boxRight - player.boxLeft;

        if (drawBoxes) {
            drawList->AddRect(
                ImVec2(player.boxLeft, boxTop),
                ImVec2(player.boxRight, screenY),
                ImColor(255, 0, 0, 255), 1.0f, 0, 1.0f
            );
        }

        if (drawHealth) {
            const float healthPercent = player.maxHealth > 0
                                            ? std::clamp(static_cast<float>(player.health) / player.maxHealth, 0.0f, 1.0f)
                                            : 0.0f;
            constexpr float barWidth = 4.0f;
            const float barLeft = player.boxLeft - 7.0f;

            // A vertical bar stays readable at every distance and shows loss from the top.
            drawList->AddRectFilled(
                ImVec2(barLeft, boxTop),
                ImVec2(barLeft + barWidth, screenY),
                ImColor(50, 50, 50, 200)
            );

            ImColor healthColor = healthPercent > 0.5f ? ImColor(0, 255, 0, 255) : ImColor(255, 0, 0, 255);

            drawList->AddRectFilled(
                ImVec2(barLeft, screenY - boxHeight * healthPercent),
                ImVec2(barLeft + barWidth, screenY),
                healthColor
            );

            const std::string healthText = std::to_string(player.health) + "/" + std::to_string(player.maxHealth);
            drawList->AddText(ImVec2(barLeft - 4.0f, boxTop - 14.0f), ImColor(255, 255, 255, 220), healthText.c_str());
        }

        if (player.distance > 0.0f) {
            const std::string distText = std::to_string(static_cast<int>(player.distance)) + "m";
            drawList->AddText(
                ImVec2(screenX - 15, screenY + 6),
                ImColor(255, 255, 255, 200),
                distText.c_str()
            );
        }
    }
}

void RenderMenu(size_t playerCount) {
    if (!menuOpen) return;

    const bool wasMenuOpen = menuOpen;
    ImGui::Begin("Deadlock Internal", &menuOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Checkbox("ESP", &drawEsp);
    ImGui::Checkbox("Boxes", &drawBoxes);
    ImGui::Checkbox("Health Bars", &drawHealth);
    ImGui::Checkbox("Aim assist (hold RMB)", &aimAssist);
    ImGui::SliderFloat("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
    ImGui::SliderFloat("Aim smooth", &aimSmooth, 1.0f, 20.0f, "%.1f");
    if (ImGui::Button("Unload DLL (Delete)")) {
        RequestUnload();
    }

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Players: %zu", playerCount);
    ImGui::Text("Hero scan: %s", espStatus.heroPawnsFound ? "ready" : "searching");

    ImGui::End();

    if (wasMenuOpen && !menuOpen) {
        SetMenuOpen(false);
    }
}

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
PresentFn oPresent = nullptr;

LRESULT __stdcall hkWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

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
        if (!pContext) return oPresent(pSwapChain, SyncInterval, Flags);

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
    printf("[+] client.dll: 0x%p\n", reinterpret_cast<void*>(clientBase));

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
    SetupHooks();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, LPVOID lpReserved) {
    if (dwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        moduleHandle = hModule;

        HANDLE hThread = CreateThread(nullptr, 0, InitializeThread, nullptr, 0, nullptr);

        if (hThread) CloseHandle(hThread);
    }
    else if (dwReason == DLL_PROCESS_DETACH) {
        if (oWndProc) {
            SetWindowLongPtr(gameWindow, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        }

        if (consoleAttached) {
            FreeConsole();
            consoleAttached = false;
        }
    }
    return TRUE;
}
