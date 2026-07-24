#include "shared.h"

// Temporary measurement mode: suppress verbose aim/parry diagnostics.
#define printf(...) do { } while (0)

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
    return std::isfinite(screen.x) && std::isfinite(screen.y);
}

namespace {

Vector3 CalculateAimAngles(const Vector3& source, const Vector3& target) {
    constexpr float RadToDeg = 57.29577951308232f;
    const float dx = target.x - source.x;
    const float dy = target.y - source.y;
    const float dz = target.z - source.z;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(horizontal) || !std::isfinite(dz) || horizontal < 0.001f) {
        return {};
    }

    Vector3 angles{};
    angles.x = -std::atan2(dz, horizontal) * RadToDeg;
    angles.y = std::atan2(dy, dx) * RadToDeg;
    angles.z = 0.0f;
    return angles;
}

Vector3 NormalizeAimAngles(Vector3 angles) {
    while (angles.x > 89.0f) angles.x -= 180.0f;
    while (angles.x < -89.0f) angles.x += 180.0f;
    while (angles.y > 180.0f) angles.y -= 360.0f;
    while (angles.y < -180.0f) angles.y += 360.0f;
    angles.z = 0.0f;
    return angles;
}

void ClearPendingSilentAngles() {
    std::lock_guard<std::mutex> lock(silentAnglesMutex);
    pendingSilentAnglesReady = false;
}

}

static ID3D11Texture2D* cachedGameDepth = nullptr;
static std::vector<uint8_t> depthSnapshotData;
static UINT depthSnapshotRowPitch = 0;

// Source 2 client physics trace entry points identified in client.dll.i64.
// These are relative to the client module base and are kept together so the
// game-trace path can be disabled cleanly if the client build changes.
constexpr uintptr_t PhysicsTraceWrapperRva = 0x149F7E0;
constexpr uintptr_t PhysicsTraceContextRva = 0x2E8BC50;
constexpr uintptr_t PhysicsRayShapeInitRva = 0x026FA40;
constexpr uintptr_t TraceResultInitRva = 0x1E913B0;
constexpr uintptr_t RayShapeGlobalRva = 0x021D1F28;
constexpr uintptr_t TraceFilterGroupRva = 0x14611D0;
constexpr uintptr_t TraceFilterLayerRva = 0x146E270;
constexpr uintptr_t TraceFilterVtableRva = 0x021D2768;

bool InvertMatrix(const Matrix4x4& matrix, Matrix4x4& inverse) {
    const float* m = &matrix.m[0][0];
    float inv[16]{};

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (!std::isfinite(determinant) || std::fabs(determinant) < 1e-6f) return false;
    for (int i = 0; i < 16; ++i) reinterpret_cast<float*>(&inverse.m[0][0])[i] = inv[i] / determinant;
    return true;
}

bool UnprojectCenter(const Matrix4x4& inverse, float depth, Vector3& world) {
    const float x = inverse.m[0][2] * depth + inverse.m[0][3];
    const float y = inverse.m[1][2] * depth + inverse.m[1][3];
    const float z = inverse.m[2][2] * depth + inverse.m[2][3];
    const float w = inverse.m[3][2] * depth + inverse.m[3][3];
    if (!std::isfinite(w) || std::fabs(w) < 1e-6f) return false;
    world = { x / w, y / w, z / w };
    return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
}

bool GetCameraTraceStart(Vector3& start) {
    if (!currentViewMatrixReady || !currentLocalPositionReady) return false;
    Matrix4x4 inverse{};
    if (!InvertMatrix(currentViewMatrix, inverse)) return false;

    Vector3 depthZero{};
    Vector3 depthOne{};
    if (!UnprojectCenter(inverse, 0.0f, depthZero) || !UnprojectCenter(inverse, 1.0f, depthOne)) return false;

    const auto distanceSquared = [](const Vector3& a, const Vector3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    };
    start = distanceSquared(depthZero, currentLocalPosition) < distanceSquared(depthOne, currentLocalPosition)
                ? depthZero : depthOne;
    return true;
}

bool PhysicsTraceVisible(const Vector3& start, const Vector3& end) {
    if (!clientBase || !currentLocalPositionReady) return false;

    using ShapeInitFn = uintptr_t(__fastcall*)(void*, const void*);
    using ResultInitFn = uintptr_t(__fastcall*)(void*);
    using TraceFn = bool(__fastcall*)(void*, void*, const Vector3*, const Vector3*, void*, void*);
    using TraceFilterGroupFn = uint32_t(__fastcall*)(uintptr_t);
    using TraceFilterLayerFn = uint16_t(__fastcall*)(uintptr_t);

    const uintptr_t physics = Read<uintptr_t>(clientBase + PhysicsTraceContextRva);
    if (!physics) return false;

    alignas(16) uint8_t shape[64]{};
    alignas(16) uint8_t filter[96]{};
    alignas(16) uint8_t result[192]{};

    // Native callers install the current CTraceFilter vtable before invoking
    // TraceShape. A zero vtable makes the wrapper return a clear ray even
    // though the remaining filter fields look valid.
    *reinterpret_cast<uintptr_t*>(filter) = clientBase + TraceFilterVtableRva;
    *reinterpret_cast<uint64_t*>(filter + 8) = 0xC1001;
    *reinterpret_cast<uint32_t*>(filter + 32) = 0;
    *reinterpret_cast<int32_t*>(filter + 36) = -1;
    *reinterpret_cast<int32_t*>(filter + 40) = -1;
    *reinterpret_cast<int32_t*>(filter + 44) = -1;
    *reinterpret_cast<uint32_t*>(filter + 50) = 0xFFFF0000u;
    *reinterpret_cast<uint16_t*>(filter + 54) = 256;
    filter[56] = 3;
    filter[57] = 0x49;
    *reinterpret_cast<uint64_t*>(filter + 64) = 0;

    __try {
        auto initShape = reinterpret_cast<ShapeInitFn>(clientBase + PhysicsRayShapeInitRva);
        auto initResult = reinterpret_cast<ResultInitFn>(clientBase + TraceResultInitRva);
        auto trace = reinterpret_cast<TraceFn>(clientBase + PhysicsTraceWrapperRva);
        auto filterGroup = reinterpret_cast<TraceFilterGroupFn>(clientBase + TraceFilterGroupRva);
        auto filterLayer = reinterpret_cast<TraceFilterLayerFn>(clientBase + TraceFilterLayerRva);

        *reinterpret_cast<uint32_t*>(filter + 32) = filterGroup(0);
        *reinterpret_cast<uint16_t*>(filter + 48) = filterLayer(0);

        initShape(shape, reinterpret_cast<const void*>(clientBase + RayShapeGlobalRva));
        initResult(result);
        // TraceShape returns whether it hit something. A clear line returns
        // false, so the visibility decision must use the populated result
        // rather than this return value.
        trace(reinterpret_cast<void*>(physics), shape, &start, &end, filter, result);

        const float fraction = *reinterpret_cast<const float*>(result + 172);
        const bool startSolid = *reinterpret_cast<const bool*>(result + 183);
        static int traceLogCount = 0;
        if (traceLogCount < 40) {
            FILE* traceLog = nullptr;
            if (fopen_s(&traceLog, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\visibility_runtime.log", "a") == 0 && traceLog) {
                fprintf(traceLog, "current trace start=%.1f,%.1f,%.1f end=%.1f,%.1f,%.1f fraction=%.6f startSolid=%d\n",
                        start.x, start.y, start.z, end.x, end.y, end.z,
                        fraction, startSolid ? 1 : 0);
                fclose(traceLog);
            }
            ++traceLogCount;
        }
        return std::isfinite(fraction) && fraction >= 0.995f && !startSolid;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CaptureDepthSnapshot() {
    depthSnapshotReady = false;
    if (!pContext || !pDevice) return false;

    ID3D11DepthStencilView* depthView = nullptr;
    pContext->OMGetRenderTargets(0, nullptr, &depthView);

    ID3D11Texture2D* depthTexture = nullptr;
    if (depthView) {
        ID3D11Resource* resource = nullptr;
        depthView->GetResource(&resource);
        depthView->Release();
        if (resource) {
            const HRESULT queryResult = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTexture));
            resource->Release();
            if (FAILED(queryResult) || !depthTexture) depthTexture = nullptr;
        }
        if (depthTexture) {
            if (cachedGameDepth) cachedGameDepth->Release();
            cachedGameDepth = depthTexture;
            cachedGameDepth->AddRef();
        }
    } else if (cachedGameDepth) {
        depthTexture = cachedGameDepth;
        depthTexture->AddRef();
    }

    if (!depthTexture) return false;

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

    // Read the staging copy once per frame.  The previous DO_NOT_WAIT mode
    // rejected nearly every copy while the GPU was still completing it, so
    // aim assist never received a usable visibility snapshot.
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(pContext->Map(depthStaging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    const size_t snapshotSize = static_cast<size_t>(mapped.RowPitch) * depthHeight;
    depthSnapshotData.resize(snapshotSize);
    for (UINT row = 0; row < depthHeight; ++row) {
        memcpy(depthSnapshotData.data() + static_cast<size_t>(row) * mapped.RowPitch,
               static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch,
               mapped.RowPitch);
    }
    depthSnapshotRowPitch = mapped.RowPitch;
    pContext->Unmap(depthStaging, 0);
    depthSnapshotReady = true;
    return true;
}

void ReleaseAimResources() {
    if (cachedGameDepth) {
        cachedGameDepth->Release();
        cachedGameDepth = nullptr;
    }
    depthSnapshotData.clear();
    depthSnapshotRowPitch = 0;
}

bool ReadDepthAt(float x, float y, float& depth) {
    depth = 0.0f;
    if (!depthSnapshotReady || !depthStaging || depthWidth == 0 || depthHeight == 0) return false;
    const LONG px = static_cast<LONG>(std::clamp(x, 0.0f, static_cast<float>(depthWidth - 1)));
    const LONG py = static_cast<LONG>(std::clamp(y, 0.0f, static_cast<float>(depthHeight - 1)));

    if (depthSnapshotData.empty() || depthSnapshotRowPitch == 0) return false;
    const uint8_t* row = depthSnapshotData.data() + static_cast<size_t>(depthSnapshotRowPitch) * py;
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
    return std::isfinite(depth) && depth >= 0.0f && depth <= 1.0f;
}

bool IsDepthBufferPopulated() {
    if (!depthSnapshotReady || depthWidth == 0 || depthHeight == 0) return false;

    constexpr float samplePoints[][2] = {
        { 0.50f, 0.50f }, { 0.25f, 0.50f }, { 0.75f, 0.50f },
        { 0.50f, 0.25f }, { 0.50f, 0.75f }
    };
    for (const auto& point : samplePoints) {
        float depth = 0.0f;
        if (ReadDepthAt(depthWidth * point[0], depthHeight * point[1], depth) && depth > 0.001f) {
            return true;
        }
    }
    return false;
}

bool GetAimPointScreen(const PlayerData& player, float height, Vector2& screen) {
    if (!currentViewMatrixReady) return false;
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    return WorldToScreen(aimPoint, screen, currentViewMatrix);
}

bool IsAimPointVisible(const PlayerData& player, float height, float screenX, float screenY) {
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    if (!clientBase || !currentLocalPositionReady) return false;
    if (!Read<uintptr_t>(clientBase + PhysicsTraceContextRva)) return false;
    Vector3 traceStart = currentLocalPosition;
    traceStart.z += 64.0f;
    // TraceShape's bool is a hit flag, not a success flag. PhysicsTraceVisible
    // intentionally ignores that return value and evaluates the populated
    // fraction/startSolid result instead.
    return PhysicsTraceVisible(traceStart, aimPoint);
}

void AimAtClosestEnemy(const std::vector<PlayerData>& players) {
    const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rightButtonDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

    if (!aimSilentMode || !leftButtonDown) {
        ClearPendingSilentAngles();
    }

    const bool aiming = aimSilentMode ? leftButtonDown : rightButtonDown;
    if (!aimAssist || menuOpen || !aiming || !currentViewMatrixReady) return;

    const bool useDepthWallCheck = true;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;
    const PlayerData* best = nullptr;
    Vector2 bestAimScreen{};
    float bestAimHeight = 54.0f;
    float bestDistance = aimFov * aimFov;
    size_t visibleTargets = 0;
    size_t testedTargets = 0;

    for (const auto& player : players) {
        const float modelHeight = player.modelHeight > 20.0f ? player.modelHeight : 80.0f;
        float targetHeights[3] = { modelHeight * 0.45f, modelHeight * 0.60f, modelHeight * 0.90f };
        int targetHeightCount = 3;
        if (aimTargetMode == AimTargetMode::Head) {
            targetHeights[0] = modelHeight * 0.90f;
            targetHeightCount = 1;
        } else if (aimTargetMode == AimTargetMode::Body) {
            targetHeights[0] = modelHeight * 0.60f;
            targetHeightCount = 1;
        }
        bool targetVisible = false;
        Vector2 visibleAimScreen{};
        float visibleAimHeight = 54.0f;
        float visibleDistance = FLT_MAX;
        // Try torso, chest and head. A single fixed point can lie inside the
        // model and therefore have a different depth than its rendered surface.
        for (int heightIndex = 0; heightIndex < targetHeightCount; ++heightIndex) {
            const float height = targetHeights[heightIndex];
            Vector2 aimScreen{};
            if (!GetAimPointScreen(player, height, aimScreen)) continue;
            const float dx = aimScreen.x - cx;
            const float dy = aimScreen.y - cy;
            const float distance = dx * dx + dy * dy;
            if (distance >= bestDistance) continue;
            ++testedTargets;
            if (useDepthWallCheck && !IsAimPointVisible(player, height, aimScreen.x, aimScreen.y)) continue;
            targetVisible = true;
            if (distance < visibleDistance) {
                visibleDistance = distance;
                visibleAimScreen = aimScreen;
                visibleAimHeight = height;
            }
        }
        if (targetVisible) {
            ++visibleTargets;
            if (visibleDistance < bestDistance) {
                bestDistance = visibleDistance;
                best = &player;
                // Store the point that passed the depth test for the final move.
                bestAimScreen = visibleAimScreen;
                bestAimHeight = visibleAimHeight;
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
            fprintf(diagnostic, "tested=%zu visible=%zu blocked=%zu depthCheck=%d\n", testedTargets, visibleTargets, testedTargets - visibleTargets, useDepthWallCheck ? 1 : 0);
            fclose(diagnostic);
        }
        lastTestedTargets = testedTargets;
        lastVisibleTargets = visibleTargets;
    }

    if (!best) {
        return;
    }

    const float targetX = bestAimScreen.x;
    const float targetY = bestAimScreen.y;
    const float smooth = aimSilentMode ? 1.0f : (aimSmooth < 1.0f ? 1.0f : aimSmooth);
    const LONG moveX = static_cast<LONG>((targetX - cx) / smooth);
    const LONG moveY = static_cast<LONG>((targetY - cy) / smooth);
    if (moveX || moveY) {
        if (aimSilentMode) {
            const Vector3 targetWorld{
                best->pos.x,
                best->pos.y,
                best->pos.z + bestAimHeight
            };
            const Vector3 cameraOrigin = currentCameraPositionReady
                ? currentCameraPosition : currentLocalPosition;
            const float dx = targetWorld.x - cameraOrigin.x;
            const float dy = targetWorld.y - cameraOrigin.y;
            const float dz = targetWorld.z - cameraOrigin.z;
            const float horizontal = std::sqrt(dx * dx + dy * dy);
            Vector3 commandAngles{
                -std::atan2(dz, horizontal) * 57.29577951308232f,
                std::atan2(dy, dx) * 57.29577951308232f,
                0.0f
            };
            std::lock_guard<std::mutex> lock(silentAnglesMutex);
            pendingSilentAngles = commandAngles;
            pendingSilentAnglesReady = true;
        }
        if (!aimSilentMode) {
            INPUT input{};
            input.type = INPUT_MOUSE;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            input.mi.dx = moveX;
            input.mi.dy = moveY;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
}

namespace {

std::string GetControllerRuntimeType(uintptr_t object) {
    const uintptr_t vtable = Read<uintptr_t>(object);
    if (!vtable) return {};
    const uintptr_t locator = Read<uintptr_t>(vtable - sizeof(uintptr_t));
    if (!locator || Read<uint32_t>(locator) != 1) return {};

    const int32_t typeDescriptorRva = Read<int32_t>(locator + 12);
    if (typeDescriptorRva <= 0) return {};
    const uintptr_t name = clientBase + static_cast<uintptr_t>(typeDescriptorRva) + 16;
    std::string result;
    result.reserve(96);
    for (size_t index = 0; index < 95; ++index) {
        const char value = Read<char>(name + index);
        if (!value) break;
        if (value < 0x20 || value > 0x7E) return {};
        result.push_back(value);
    }
    return result;
}

void MonitorRemoteMeleeAnimations() {
    if (!clientBase || !currentLocalPositionReady) return;

    static std::unordered_map<uintptr_t, uint32_t> lastSequences;
    static ULONGLONG lastPrune = 0;
    const ULONGLONG now = GetTickCount64();
    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }

    for (const uintptr_t pawn : pawns) {
        if (!pawn || pawn == currentLocalPawn || Read<uint8_t>(pawn + Offsets::Team) != 3) continue;

        Vector3 position{};
        if (!GetEntityPosition(pawn, position)) continue;
        const float dx = position.x - currentLocalPosition.x;
        const float dy = position.y - currentLocalPosition.y;
        const float dz = position.z - currentLocalPosition.z;
        if (dx * dx + dy * dy + dz * dz > 400.0f * 400.0f) continue;

        // The main pointer can be an abstract base controller, while the
        // manager additionally owns concrete networked graph controllers.
        // Probe both layouts and emit only actual sequence transitions.
        uintptr_t controllers[3] = {
            Read<uintptr_t>(pawn + Offsets::MainGraphController),
            Read<uintptr_t>(pawn + Offsets::GraphControllerManager + 8),
            Read<uintptr_t>(pawn + Offsets::GraphControllerManager + 16)
        };
        const int managerCount = std::clamp(
            Read<int>(pawn + Offsets::GraphControllerManager), 0, 8);
        const uintptr_t managerItems = Read<uintptr_t>(pawn + Offsets::GraphControllerManager + 8);

        for (int index = -1; index < managerCount; ++index) {
            const uintptr_t controller = index < 0 ? controllers[0]
                : Read<uintptr_t>(managerItems + static_cast<uintptr_t>(index) * sizeof(uintptr_t));
            if (!controller) continue;

            const uint32_t sequence = Read<uint32_t>(controller + Offsets::AnimSequence);
            // 0x7F7FFFFF is the engine's "no networked sequence" sentinel.
            if (sequence == 0x7F7FFFFFu) continue;
            const uint32_t pawnHandle = FindEntityHandle(pawn);

            const auto found = lastSequences.find(controller);
            if (found != lastSequences.end() && found->second == sequence) continue;
            lastSequences[controller] = sequence;

            const float startTime = Read<float>(controller + Offsets::AnimSequenceStartTime);
            const bool changed = Read<uint8_t>(controller + 0x15C6) != 0;
            const std::string typeName = GetControllerRuntimeType(controller);
            printf("[Anim] remote pawn=0x%X controller=0x%p type=%s sequence=%u start=%.3f networkChanged=%d\n",
                   pawnHandle, reinterpret_cast<void*>(controller), typeName.c_str(), sequence,
                   startTime, changed ? 1 : 0);
        }
    }

    if (now - lastPrune > 5000) {
        lastPrune = now;
        if (lastSequences.size() > 64) {
            lastSequences.clear();
        }
    }
}

} // namespace
