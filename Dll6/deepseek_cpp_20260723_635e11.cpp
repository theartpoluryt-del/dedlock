#include "shared.h"

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

// ============================================================
// SILENT AIM: ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ
// ============================================================

Vector3 CalculateAngle(const Vector3& src, const Vector3& dst) {
    Vector3 delta = { dst.x - src.x, dst.y - src.y, dst.z - src.z };
    float hyp = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (!std::isfinite(hyp) || hyp < 0.001f) return {};
    
    Vector3 angles;
    angles.x = -asinf(delta.z / hyp) * (180.0f / 3.1415926535f);
    angles.y = atan2f(delta.y, delta.x) * (180.0f / 3.1415926535f);
    angles.z = 0.0f;
    return angles;
}

Vector3 NormalizeAngles(Vector3 angles) {
    while (angles.x > 89.0f) angles.x -= 180.0f;
    while (angles.x < -89.0f) angles.x += 180.0f;
    while (angles.y > 180.0f) angles.y -= 360.0f;
    while (angles.y < -180.0f) angles.y += 360.0f;
    return angles;
}

// ============================================================
// ОСТАЛЬНЫЕ ФУНКЦИИ (БЕЗ ИЗМЕНЕНИЙ)
// ============================================================

static ID3D11Texture2D* cachedGameDepth = nullptr;
static std::vector<uint8_t> depthSnapshotData;
static UINT depthSnapshotRowPitch = 0;

constexpr uintptr_t PhysicsTraceWrapperRva = 0x149F7E0;
constexpr uintptr_t PhysicsTraceContextRva = 0x2E8BC50;
constexpr uintptr_t PhysicsRayShapeInitRva = 0x026FA40;
constexpr uintptr_t TraceResultInitRva = 0x1E913B0;
constexpr uintptr_t RayShapeGlobalRva = 0x021D1F28;
constexpr uintptr_t TraceFilterGroupRva = 0x14611D0;
constexpr uintptr_t TraceFilterLayerRva = 0x146E270;
constexpr uintptr_t TraceFilterVtableRva = 0x021D2768;
constexpr uintptr_t NativeLosRva = 0x0AC4370;
constexpr uintptr_t NativeLosContextRva = 0x0355190;

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
    if (!clientBase) return false;

    // Visibility must originate at the local pawn. The guessed render-camera
    // field at ViewMatrix+0x28 moves with the camera and is not a reliable
    // collision-space origin in the current client build.
    if (currentLocalPositionReady && std::isfinite(currentLocalPosition.x) &&
        std::isfinite(currentLocalPosition.y) && std::isfinite(currentLocalPosition.z)) {
        start = currentLocalPosition;
        // Pos is the pawn origin/feet in collision space. Starting there
        // makes the LOS ray touch the local capsule or floor and produces
        // fraction 0/startSolid even when the enemy is in the open. Use the
        // gameplay eye height for the ray origin.
        start.z += 64.0f;
        return true;
    }

    start = Read<Vector3>(clientBase + Offsets::ViewMatrix + 0x28);
    if (std::isfinite(start.x) && std::isfinite(start.y) && std::isfinite(start.z))
        return true;

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
    // Current client native LOS setup uses 0xC1001 here (not the previous
    // build's 0xC10301). The old mask silently excluded world geometry.
    *reinterpret_cast<uint64_t*>(filter + 8) = 0xC1001;
    *reinterpret_cast<uint32_t*>(filter + 32) = 0;
    *reinterpret_cast<int32_t*>(filter + 36) = -1;
    *reinterpret_cast<int32_t*>(filter + 40) = -1;
    *reinterpret_cast<int32_t*>(filter + 44) = -1;
    *reinterpret_cast<uint32_t*>(filter + 50) = 0xFFFF0000u;
    *reinterpret_cast<uint16_t*>(filter + 54) = 256;
    filter[56] = 3;
    filter[57] = 0x49;
    // The native CTraceFilter construction in IDA leaves its ignore-entity
    // field at zero. Do not inject the separately resolved pawn handle here:
    // after the update that stale value makes every ray begin solid.
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
        const bool nativeTraceHit = trace(reinterpret_cast<void*>(physics), shape, &start, &end, filter, result);

        const float fraction = *reinterpret_cast<const float*>(result + 172);
        const bool startSolid = *reinterpret_cast<const bool*>(result + 183);
        static uint32_t traceDiagnostics = 0;
        if ((++traceDiagnostics % 30u) == 0u) {
            FILE* file = nullptr;
            if (fopen_s(&file, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\visibility_runtime.log", "a") == 0 && file) {
                fprintf(file, "trace call=%u start=%.1f,%.1f,%.1f end=%.1f,%.1f,%.1f fraction=%.6f startSolid=%d nativeHit=%d result=%d\n",
                        traceDiagnostics, start.x, start.y, start.z, end.x, end.y, end.z,
                        fraction, startSolid ? 1 : 0, nativeTraceHit ? 1 : 0,
                        std::isfinite(fraction) && fraction >= 0.995f && !startSolid ? 1 : 0);
                fclose(file);
            }
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
    // Present can be reached before the copy is submitted to the immediate
    // context. Flush here so ReadDepthAt never observes the staging clear value
    // (0.0) for every pixel.
    pContext->Flush();
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

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(pContext->Map(depthStaging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint8_t* row = static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * py;
    if (depthFormat == DXGI_FORMAT_D32_FLOAT || depthFormat == DXGI_FORMAT_R32_TYPELESS) {
        depth = *reinterpret_cast<const float*>(row + px * sizeof(float));
    } else if (depthFormat == DXGI_FORMAT_D24_UNORM_S8_UINT || depthFormat == DXGI_FORMAT_R24G8_TYPELESS) {
        const uint32_t packed = *reinterpret_cast<const uint32_t*>(row + px * sizeof(uint32_t));
        depth = static_cast<float>(packed & 0x00FFFFFFu) / 16777215.0f;
    } else {
        const uint16_t packed = *reinterpret_cast<const uint16_t*>(row + px * sizeof(uint16_t));
        depth = static_cast<float>(packed) / 65535.0f;
    }
    pContext->Unmap(depthStaging, 0);
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

bool IsAimPointDepthVisible(const PlayerData& player, float height, float screenX, float screenY) {
    static uint32_t diagnosticCalls = 0;
    const bool logThisCall = (++diagnosticCalls % 30u) == 0u;
    if (!depthSnapshotReady || !currentViewMatrixReady) {
        if (logThisCall) {
            FILE* file = nullptr;
            if (fopen_s(&file, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\visibility_runtime.log", "a") == 0 && file) {
                fprintf(file, "call=%u depthReady=%d viewReady=%d result=1 reason=no_snapshot height=%.1f screen=%.1f,%.1f\n",
                        diagnosticCalls, depthSnapshotReady ? 1 : 0, currentViewMatrixReady ? 1 : 0,
                        height, screenX, screenY);
                fclose(file);
            }
        }
        return true;
    }

    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    const Matrix4x4& matrix = currentViewMatrix;
    const float w = matrix.m[3][0] * aimPoint.x + matrix.m[3][1] * aimPoint.y +
                    matrix.m[3][2] * aimPoint.z + matrix.m[3][3];
    if (w < 0.01f) return false;
    const float clipZ = matrix.m[2][0] * aimPoint.x + matrix.m[2][1] * aimPoint.y +
                        matrix.m[2][2] * aimPoint.z + matrix.m[2][3];
    const float targetDepth = clipZ / w;
    if (!std::isfinite(targetDepth) || targetDepth < 0.0f || targetDepth > 1.0f) return false;

    constexpr float depthBias = 0.025f;
    constexpr float sampleOffsets[][2] = {
        { 0.0f, 0.0f }, { -4.0f, 0.0f }, { 4.0f, 0.0f },
        { 0.0f, -4.0f }, { 0.0f, 4.0f }
    };
    bool gotDepthSample = false;
    bool gotNonZeroDepth = false;
    bool matched = false;
    float firstSceneDepth = -1.0f;
    for (const auto& offset : sampleOffsets) {
        float sceneDepth = 0.0f;
        if (!ReadDepthAt(screenX + offset[0], screenY + offset[1], sceneDepth)) continue;
        gotDepthSample = true;
        if (sceneDepth > 0.001f) gotNonZeroDepth = true;
        if (firstSceneDepth < 0.0f) firstSceneDepth = sceneDepth;
        if (std::fabs(sceneDepth - targetDepth) <= depthBias) matched = true;
    }
    if (logThisCall) {
        FILE* file = nullptr;
        if (fopen_s(&file, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\visibility_runtime.log", "a") == 0 && file) {
            fprintf(file, "call=%u depthReady=1 height=%.1f screen=%.1f,%.1f w=%.4f target=%.6f scene=%.6f got=%d matched=%d result=%d\n",
                    diagnosticCalls, height, screenX, screenY, w, targetDepth, firstSceneDepth,
                    gotDepthSample ? 1 : 0, matched ? 1 : 0, (matched || !gotDepthSample) ? 1 : 0);
            fclose(file);
        }
    }
    // A copied depth buffer can be present but contain only zeroes when the
    // game is rendering through a different pass (UI/overlay or a transient
    // swap-chain state). It is not a valid occlusion result. PhysicsTraceVisible
    // has already made the authoritative world-geometry decision, so do not
    // reject every target merely because this optional depth resource is empty.
    // The copied depth resource is optional and is empty in the current DX11
    // pass. In that case the authoritative physics LOS result remains valid;
    // only a populated depth sample is allowed to veto it.
    if (gotDepthSample && !gotNonZeroDepth) return true;
    return gotDepthSample && matched;
}

bool IsAimPointVisible(const PlayerData& player, float height, float screenX, float screenY) {
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    Vector3 traceStart{};
    if (!GetCameraTraceStart(traceStart)) return false;
    if (!PhysicsTraceVisible(traceStart, aimPoint)) return false;
    return IsAimPointDepthVisible(player, height, screenX, screenY);
}

// ============================================================
// ГЛАВНАЯ ФУНКЦИЯ АИМА (С ПОДДЕРЖКОЙ SILENT)
// ============================================================

void AimAtClosestEnemy(const std::vector<PlayerData>& players) {
    if (!aimAssist || menuOpen || !(GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
        !currentViewMatrixReady || !currentLocalPositionReady || !currentLocalPawn) {
        std::lock_guard<std::mutex> lock(silentAnglesMutex);
        pendingSilentAnglesReady = false;
        return;
    }

    // The native trace RVAs are build-specific.  Keep this optional so a stale
    // trace address cannot make the complete target list disappear.
    // Visibility is mandatory; there is intentionally no menu switch for it.
    aimVisibilityCheck = true;
    const bool useVisibilityCheck = true;
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
        for (const float height : { 32.0f, 44.0f, 56.0f, 68.0f, 80.0f }) {
            Vector2 aimScreen{};
            if (!GetAimPointScreen(player, height, aimScreen)) continue;
            const float dx = aimScreen.x - cx;
            const float dy = aimScreen.y - cy;
            const float distance = dx * dx + dy * dy;
            if (distance >= bestDistance) continue;
            ++testedTargets;
            if (useVisibilityCheck && !IsAimPointVisible(player, height, aimScreen.x, aimScreen.y)) continue;
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
                bestAimScreen = visibleAimScreen;
            }
        }
    }

    static uint32_t aimDiagnostics = 0;
    if ((++aimDiagnostics % 60u) == 0u) {
        FILE* file = nullptr;
        if (fopen_s(&file, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\visibility_runtime.log", "a") == 0 && file) {
            fprintf(file, "aim state visibility=%d tested=%zu visible=%zu localPawn=%p view=%d localPos=%d\\n",
                    useVisibilityCheck ? 1 : 0, testedTargets, visibleTargets,
                    reinterpret_cast<void*>(currentLocalPawn), currentViewMatrixReady ? 1 : 0,
                    currentLocalPositionReady ? 1 : 0);
            fclose(file);
        }
    }

    if (!best) {
        std::lock_guard<std::mutex> lock(silentAnglesMutex);
        pendingSilentAnglesReady = false;
        return;
    }
    
    const float targetX = bestAimScreen.x;
    const float targetY = bestAimScreen.y;
    const LONG moveX = static_cast<LONG>((targetX - cx) / (aimSmooth < 1.0f ? 1.0f : aimSmooth));
    const LONG moveY = static_cast<LONG>((targetY - cy) / (aimSmooth < 1.0f ? 1.0f : aimSmooth));

    // ============================================================
    // === SILENT AIM: ПИШЕМ В СЕРВЕРНЫЕ УГЛЫ ===
    // ============================================================
    if (aimSilentMode) {
        if (currentLocalPositionReady) {
            // Берем позицию цели из best (плюс смещение на голову/грудь)
            Vector3 targetPos = best->pos;
            targetPos.z += 56.0f;  // центр корпуса
            
            // Получаем позицию игрока
            Vector3 localPos = currentLocalPosition;
            // SceneNodeAbsOrigin is the pawn origin at the feet; view angles
            // are calculated from the eye position.
            localPos.z += 64.0f;
            
            // Рассчитываем углы до цели
            const Vector3 angles = NormalizeAngles(CalculateAngle(localPos, targetPos));
            
            // Передаём углы в обработчик CUserCmd. Запись в pawn здесь не делаем:
            // m_angEyeAngles является сетевым состоянием, а не углами конкретного выстрела.
            {
                std::lock_guard<std::mutex> lock(silentAnglesMutex);
                pendingSilentAngles = angles;
                pendingSilentAnglesReady = true;
            }
            
            // Логируем для отладки (можно убрать)
            static int silentLogCount = 0;
            if (silentLogCount++ < 10) {
                printf("[SilentAim] angles: %.2f %.2f %.2f | target: %.1f %.1f %.1f\n",
                       angles.x, angles.y, angles.z,
                       targetPos.x, targetPos.y, targetPos.z);
            }
        }
        return;  // Выходим, чтобы не двигать мышь
    }
    
    // ============================================================
    // === ОБЫЧНЫЙ ВИЗУАЛЬНЫЙ AIM (через SendInput) ===
    // ============================================================
    if (moveX || moveY) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = moveX;
        input.mi.dy = moveY;
        SendInput(1, &input, sizeof(INPUT));
    }
}

// ============================================================
// ОСТАЛЬНЫЕ ФУНКЦИИ (БЕЗ ИЗМЕНЕНИЙ)
// ============================================================

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
