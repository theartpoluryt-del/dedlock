#include "shared.h"
#include <fstream>
#include <random>
#include <sstream>

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

using CalcWorldSpaceBonesFn = void(__fastcall*)(uintptr_t, unsigned int);
using GetBoneIdByNameFn = int(__fastcall*)(uintptr_t, const char*);

struct BoneRuntimeFunctions {
    CalcWorldSpaceBonesFn calcWorldSpaceBones{};
    GetBoneIdByNameFn getBoneIdByName{};
    bool resolved = false;
};

BoneRuntimeFunctions boneFunctions{};

uintptr_t FindBonePattern(uintptr_t base, std::size_t size, const char* pattern) {
    std::vector<int> bytes;
    std::istringstream stream(pattern ? pattern : "");
    std::string token;
    while (stream >> token) {
        bytes.push_back(token == "?" || token == "??" ? -1 : std::stoi(token, nullptr, 16));
    }
    if (bytes.empty() || bytes.size() > size) return 0;
    for (std::size_t i = 0; i <= size - bytes.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && Read<uint8_t>(base + i + j) != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (match) return base + i;
    }
    return 0;
}

void ResolveBoneFunctions() {
    if (boneFunctions.resolved) return;
    boneFunctions.resolved = true;
    if (!clientBase) return;

    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase), &moduleInfo, sizeof(moduleInfo))) return;
    const auto imageSize = static_cast<std::size_t>(moduleInfo.SizeOfImage);
    const uintptr_t calc = FindBonePattern(clientBase, imageSize,
        "48 89 4C 24 ? 55 53 56 57 41 54 41 55 41 56 41 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8D 6C 24 ? 48 8B 81");
    const uintptr_t boneId = FindBonePattern(clientBase, imageSize,
        "40 53 48 83 EC 20 48 8B 89 ? ? ? ? 48 8B DA 48 8B 01 FF 50 ? 48 8B C8");
    boneFunctions.calcWorldSpaceBones = reinterpret_cast<CalcWorldSpaceBonesFn>(calc);
    boneFunctions.getBoneIdByName = reinterpret_cast<GetBoneIdByNameFn>(boneId);
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
    std::lock_guard<std::mutex> lock(humanSilentMutex);
    pendingHumanReady = false;
}

void ClearPendingCreepAngles() {
    std::lock_guard<std::mutex> lock(creepSilentMutex);
    pendingCreepReady = false;
}

bool silentFlickActive = false;
LONG silentReturnX = 0;
LONG silentReturnY = 0;

void RestoreSilentFlick() {
    if (!silentFlickActive) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = silentReturnX;
    input.mi.dy = silentReturnY;
    SendInput(1, &input, sizeof(input));
    silentFlickActive = false;
    silentReturnX = 0;
    silentReturnY = 0;
}

void ApplySilentFlick(LONG moveX, LONG moveY) {
    if (!moveX && !moveY) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = moveX;
    input.mi.dy = moveY;
    SendInput(1, &input, sizeof(input));
    silentReturnX -= moveX;
    silentReturnY -= moveY;
    silentFlickActive = true;
}

}

static ID3D11Texture2D* cachedGameDepth = nullptr;
static std::vector<uint8_t> depthSnapshotData;
static UINT depthSnapshotRowPitch = 0;

// Source 2 client physics trace entry points identified in client.dll.i64.
// These are relative to the client module base and are kept together so the
// game-trace path can be disabled cleanly if the client build changes.
constexpr uintptr_t PhysicsTraceWrapperRva = 0x14A0120;
constexpr uintptr_t PhysicsTraceContextRva = 0x2E8CCE0;
constexpr uintptr_t PhysicsRayShapeInitRva = 0x026FA80;
constexpr uintptr_t TraceResultInitRva = 0x1E91D30;
constexpr uintptr_t RayShapeGlobalRva = 0x021D1F08;
constexpr uintptr_t TraceFilterConstructorRva = 0x1047720;
constexpr uintptr_t TraceFilterGroupRva = 0x1461B10;
constexpr uintptr_t TraceFilterLayerRva = 0x146EBB0;
constexpr uintptr_t TraceFilterVtableRva = 0x021D2748;

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

bool GetEntityBonePosition(uintptr_t entity, const char* boneName, Vector3& position) {
    position = {};
    if (!entity || !boneName || !*boneName) return false;
    ResolveBoneFunctions();
    if (!boneFunctions.calcWorldSpaceBones || !boneFunctions.getBoneIdByName) return false;

    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    __try {
        boneFunctions.calcWorldSpaceBones(sceneNode, 0xFFFFFu);
        const int boneIndex = boneFunctions.getBoneIdByName(entity, boneName);
        if (boneIndex < 0 || boneIndex > 512) return false;

        // CModelState starts at CSkeletonInstance + 0x150 and the runtime
        // bone pointer is the first field after its 0x80-byte prefix.
        const uintptr_t bones = Read<uintptr_t>(sceneNode + 0x150 + 0x80);
        if (!bones) return false;
        position = Read<Vector3>(bones + static_cast<uintptr_t>(boneIndex) * 0x20);
        return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        position = {};
        return false;
    }
}

bool GetEntityBoneSkeleton(uintptr_t entity, std::vector<BoneSegment>& segments) {
    segments.clear();
    if (!entity) return false;
    ResolveBoneFunctions();
    if (!boneFunctions.calcWorldSpaceBones || !boneFunctions.getBoneIdByName) return false;

    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    struct BonePair { const char* start; const char* end; };
    static constexpr BonePair pairs[] = {
        { "spine_0", "spine_1" }, { "spine_1", "spine_2" },
        { "spine_2", "spine_3" }, { "spine_3", "head" },
        { "spine_3", "arm_upper_L" }, { "arm_upper_L", "arm_lower_L" },
        { "arm_lower_L", "arm_lower_L_TWIST" }, { "arm_lower_L_TWIST", "arm_lower_L_TWIST1" },
        { "arm_lower_L", "hand_L" },
        { "arm_upper_L", "forearm_L" }, { "forearm_L", "hand_L" },
        { "arm_upper_L", "elbow_L" }, { "elbow_L", "wrist_L" },
        { "spine_3", "arm_upper_R" }, { "arm_upper_R", "arm_lower_R" },
        { "arm_lower_R", "arm_lower_R_TWIST" }, { "arm_lower_R_TWIST", "arm_lower_R_TWIST1" },
        { "arm_lower_R", "hand_R" },
        { "arm_upper_R", "forearm_R" }, { "forearm_R", "hand_R" },
        { "arm_upper_R", "elbow_R" }, { "elbow_R", "wrist_R" },
        { "spine_0", "leg_upper_L" }, { "leg_upper_L", "leg_lower_L" },
        { "leg_lower_L", "leg_L_IKTARGET" },
        { "spine_0", "leg_upper_R" }, { "leg_upper_R", "leg_lower_R" },
        { "leg_lower_R", "leg_R_IKTARGET" }
    };

    __try {
        boneFunctions.calcWorldSpaceBones(sceneNode, 0xFFFFFu);
        const uintptr_t bones = Read<uintptr_t>(sceneNode + 0x150 + 0x80);
        if (!bones) return false;

        auto readBone = [&](const char* name, Vector3& position) {
            const int index = boneFunctions.getBoneIdByName(entity, name);
            if (index < 0 || index > 512) return false;
            position = Read<Vector3>(bones + static_cast<uintptr_t>(index) * 0x20);
            return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
        };

        for (const auto& pair : pairs) {
            Vector3 start{}, end{};
            if (readBone(pair.start, start) && readBone(pair.end, end)) {
                segments.push_back({ start, end });
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        segments.clear();
    }
    return !segments.empty();
}

bool GetAimAnglesFromScreen(float screenX, float screenY, Vector3& angles) {
    if (!currentViewMatrixReady) return false;

    Matrix4x4 inverse{};
    if (!InvertMatrix(currentViewMatrix, inverse)) return false;
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f) return false;

    const auto unproject = [&](float depth, Vector3& world) {
        const float clipX = (screenX / displaySize.x) * 2.0f - 1.0f;
        const float clipY = 1.0f - (screenY / displaySize.y) * 2.0f;
        const float clipZ = depth;
        const float x = inverse.m[0][0] * clipX + inverse.m[0][1] * clipY + inverse.m[0][2] * clipZ + inverse.m[0][3];
        const float y = inverse.m[1][0] * clipX + inverse.m[1][1] * clipY + inverse.m[1][2] * clipZ + inverse.m[1][3];
        const float z = inverse.m[2][0] * clipX + inverse.m[2][1] * clipY + inverse.m[2][2] * clipZ + inverse.m[2][3];
        const float w = inverse.m[3][0] * clipX + inverse.m[3][1] * clipY + inverse.m[3][2] * clipZ + inverse.m[3][3];
        if (!std::isfinite(w) || std::fabs(w) < 1e-6f) return false;
        world = { x / w, y / w, z / w };
        return std::isfinite(world.x) && std::isfinite(world.y) && std::isfinite(world.z);
    };

    Vector3 nearPoint{}, farPoint{};
    if (!unproject(0.0f, nearPoint) || !unproject(1.0f, farPoint)) return false;
    const float dx = farPoint.x - nearPoint.x;
    const float dy = farPoint.y - nearPoint.y;
    const float dz = farPoint.z - nearPoint.z;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(horizontal) || horizontal < 1e-5f || !std::isfinite(dz)) return false;

    constexpr float RadToDeg = 57.29577951308232f;
    angles = {
        -std::atan2(dz, horizontal) * RadToDeg,
        std::atan2(dy, dx) * RadToDeg,
        0.0f
    };
    return std::isfinite(angles.x) && std::isfinite(angles.y);
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

bool PhysicsTraceVisible(const Vector3& start, const Vector3& end,
                         uintptr_t targetEntity) {
    if (!clientBase || !currentLocalPositionReady) return false;

    using ShapeInitFn = uintptr_t(__fastcall*)(void*, const void*);
    using ResultInitFn = uintptr_t(__fastcall*)(void*);
    using TraceFn = bool(__fastcall*)(void*, void*, const Vector3*, const Vector3*, void*, void*);
    using TraceFilterConstructorFn = void(__fastcall*)(void*, uintptr_t, int);

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
    *reinterpret_cast<uint64_t*>(filter + 0x10) = 0;
    *reinterpret_cast<uint64_t*>(filter + 0x18) = 0;
    *reinterpret_cast<int64_t*>(filter + 0x20) = -1;
    *reinterpret_cast<int64_t*>(filter + 0x28) = -1;
    *reinterpret_cast<uint32_t*>(filter + 0x30) = 0;
    *reinterpret_cast<uint32_t*>(filter + 0x34) = 0x0100FFFFu;
    filter[0x38] = 3;
    filter[0x39] = 0x49;
    filter[0x40] = 0;

    __try {
        auto initShape = reinterpret_cast<ShapeInitFn>(clientBase + PhysicsRayShapeInitRva);
        auto initResult = reinterpret_cast<ResultInitFn>(clientBase + TraceResultInitRva);
        auto trace = reinterpret_cast<TraceFn>(clientBase + PhysicsTraceWrapperRva);
        auto filterCtor = reinterpret_cast<TraceFilterConstructorFn>(
            clientBase + TraceFilterConstructorRva);

        filterCtor(filter, currentLocalPawn, 0);

        initShape(shape, reinterpret_cast<const void*>(clientBase + RayShapeGlobalRva));
        initResult(result);
        // TraceShape returns whether it hit something. A clear line returns
        // false, so the visibility decision must use the populated result
        // rather than this return value.
        trace(reinterpret_cast<void*>(physics), shape, &start, &end, filter, result);

        const float fraction = *reinterpret_cast<const float*>(result + 172);
        const bool startSolid = *reinterpret_cast<const bool*>(result + 183);
        const uintptr_t hitEntity =
            *reinterpret_cast<const uintptr_t*>(result + 8);
        // A stale/partially initialized trace result must not remove every
        // target from the aim list. Only a valid native result may veto aim.
        if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
            return true;
        if (startSolid) return true;
        return fraction >= 0.995f ||
            (targetEntity && hitEntity == targetEntity);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
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

bool IsDepthPointVisible(const Vector3& point) {
    if (!depthSnapshotReady || !currentViewMatrixReady) return false;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return false;

    const float clipX = currentViewMatrix.m[0][0] * point.x +
        currentViewMatrix.m[0][1] * point.y + currentViewMatrix.m[0][2] * point.z +
        currentViewMatrix.m[0][3];
    const float clipY = currentViewMatrix.m[1][0] * point.x +
        currentViewMatrix.m[1][1] * point.y + currentViewMatrix.m[1][2] * point.z +
        currentViewMatrix.m[1][3];
    const float clipZ = currentViewMatrix.m[2][0] * point.x +
        currentViewMatrix.m[2][1] * point.y + currentViewMatrix.m[2][2] * point.z +
        currentViewMatrix.m[2][3];
    const float w = currentViewMatrix.m[3][0] * point.x +
        currentViewMatrix.m[3][1] * point.y + currentViewMatrix.m[3][2] * point.z +
        currentViewMatrix.m[3][3];
    if (!std::isfinite(clipX) || !std::isfinite(clipY) || !std::isfinite(clipZ) ||
        !std::isfinite(w) || w <= 0.01f) return false;

    const float screenX = display.x * 0.5f + clipX / w * display.x * 0.5f;
    const float screenY = display.y * 0.5f - clipY / w * display.y * 0.5f;
    const float targetDepth = clipZ / w * 0.5f + 0.5f;
    if (!std::isfinite(screenX) || !std::isfinite(screenY) ||
        !std::isfinite(targetDepth) || targetDepth < 0.0f || targetDepth > 1.0f)
        return false;

    // Use a small neighborhood: a single depth pixel can belong to a thin
    // model part or be unsettled while the render target is changing.
    constexpr float offsets[][2] = {{0, 0}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    bool sampled = false;
    float nearestSceneDepth = 1.0f;
    for (const auto& offset : offsets) {
        float sceneDepth = 0.0f;
        if (!ReadDepthAt(screenX + offset[0], screenY + offset[1], sceneDepth)) continue;
        if (sceneDepth <= 0.001f) continue;
        sampled = true;
        nearestSceneDepth = (std::min)(nearestSceneDepth, sceneDepth);
    }
    if (!sampled) return false;
    return targetDepth <= nearestSceneDepth + 0.025f;
}

bool GetAimPointScreen(const PlayerData& player, float height, Vector2& screen) {
    if (!currentViewMatrixReady) return false;
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    return WorldToScreen(aimPoint, screen, currentViewMatrix);
}

void FarmAimAssist(const std::vector<PlayerData>& players) {
    const bool configuredFarmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool configuredFarmKeyPressed = configuredFarmKeyDown && !farmToggleLastDown;
    if (farmAssist && farmToggleMode && configuredFarmKeyPressed) {
        farmToggleActive = !farmToggleActive;
    }
    farmToggleLastDown = configuredFarmKeyDown;
    if (!farmAssist) {
        farmToggleActive = false;
        ClearPendingCreepAngles();
        return;
    }
    const bool farmAiming = farmToggleMode ? farmToggleActive : configuredFarmKeyDown;
    if (!farmSilentMode) ClearPendingCreepAngles();
    if (menuOpen || !currentViewMatrixReady || !farmAiming) {
        ClearPendingCreepAngles();
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float cx = display.x * 0.5f;
    const float cy = display.y * 0.5f;
    // AimAtClosestEnemy runs immediately before this function and sets this
    // only when it found a live target inside Human FOV (including its
    // visibility rules). A player merely present on screen must not suppress
    // creep aim.
    if (humanAimTargetFound) {
        ClearPendingCreepAngles();
        return;
    }

    FarmTarget best{};
    Vector2 bestScreen{};
    float bestDistance = farmFov * farmFov;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team)
        : 0;
    std::vector<FarmTarget> targetSnapshot;
    {
        std::lock_guard lock(farmTargetsMutex);
        targetSnapshot = farmTargets;
    }
    struct FarmCandidate {
        float screenDistance;
        FarmTarget target;
    };
    std::vector<FarmCandidate> candidates;
    candidates.reserve(targetSnapshot.size());
    for (const auto& target : targetSnapshot) {
        if (localTeam != 0 && target.team == localTeam) continue;
        if (!target.entity || Read<int>(target.entity + Offsets::Health) <= 0 ||
            Read<uint8_t>(target.entity + Offsets::LifeState) != 0) continue;
        Vector2 screen{};
        const Vector3 approximatePoint{ target.pos.x, target.pos.y, target.pos.z + 24.0f };
        if (!WorldToScreen(approximatePoint, screen, currentViewMatrix)) continue;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        const float distance = dx * dx + dy * dy;
        if (distance < farmFov * farmFov)
            candidates.push_back({ distance, target });
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const FarmCandidate& left, const FarmCandidate& right) {
            return left.screenDistance < right.screenDistance;
        });

    const bool traceAvailable = clientBase &&
        Read<uintptr_t>(clientBase + PhysicsTraceContextRva) != 0;
    for (const auto& candidate : candidates) {
        Vector3 point{ candidate.target.pos.x, candidate.target.pos.y,
                       candidate.target.pos.z + 24.0f };
        // Only nearby screen candidates reach this expensive engine call.
        GetEntityBonePosition(candidate.target.entity, "head", point);
        if (aimVisibilityCheck && traceAvailable &&
            !IsWorldAimPointVisible(point, candidate.target.entity)) continue;
        Vector2 screen{};
        if (!WorldToScreen(point, screen, currentViewMatrix)) continue;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        const float distance = dx * dx + dy * dy;
        if (distance >= bestDistance) continue;
        best = candidate.target;
        bestScreen = screen;
        bestDistance = distance;
        // Candidates are ordered by their inexpensive screen estimate. The
        // first visible result is the same practical target without tracing
        // every creep in the lane.
        break;
    }
    if (!best.entity) {
        ClearPendingCreepAngles();
        return;
    }

    if (farmSilentMode) {
        Vector3 commandAngles{};
        if (GetAimAnglesFromScreen(bestScreen.x, bestScreen.y, commandAngles)) {
            std::lock_guard<std::mutex> lock(creepSilentMutex);
            pendingCreepAngles = commandAngles;
            pendingCreepReady = true;
        }
        return;
    }

    const float smooth = farmAimSmooth < 1.0f ? 1.0f : farmAimSmooth;
    const LONG moveX = static_cast<LONG>((bestScreen.x - cx) / smooth);
    const LONG moveY = static_cast<LONG>((bestScreen.y - cy) / smooth);
    if (!moveX && !moveY) return;
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = moveX;
    input.mi.dy = moveY;
    SendInput(1, &input, sizeof(input));
}

void AutoLastHitOrbs() {
    static bool orbToggleLastDown = false;
    const bool configuredKeyDown = (GetAsyncKeyState(autoLastHitOrbsKey) & 0x8000) != 0;
    const bool configuredKeyPressed = configuredKeyDown && !orbToggleLastDown;
    if (autoLastHitOrbs && autoLastHitOrbsToggleMode && configuredKeyPressed) {
        autoLastHitOrbsActive = !autoLastHitOrbsActive;
    }
    orbToggleLastDown = configuredKeyDown;
    if (!autoLastHitOrbs) autoLastHitOrbsActive = false;
    else if (!autoLastHitOrbsToggleMode) autoLastHitOrbsActive = configuredKeyDown;
    const bool orbAimActive = autoLastHitOrbs && autoLastHitOrbsActive;
    if (!orbAimActive || menuOpen || !currentViewMatrixReady) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAttack = false;
        pendingOrbReady = false;
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float centerX = display.x * 0.5f;
    const float centerY = display.y * 0.5f;
    OrbTarget best{};
    Vector2 bestScreen{};
    float bestDistance = FLT_MAX;
    static uintptr_t lastOrb = 0;
    static LONG attackBaseline = 0;
    static ULONGLONG attackStarted = 0;
    static ULONGLONG lastAttackApplied = 0;
    static uint8_t attackPulsesApplied = 0;
    static ULONGLONG firstAttackDelay = 0;
    static ULONGLONG secondAttackDelay = 45;
    static std::mt19937 jitterRng{ std::random_device{}() };
    static std::uniform_int_distribution<int> firstDelayDistribution(0, 35);
    static std::uniform_int_distribution<int> secondDelayDistribution(35, 80);
    {
        std::lock_guard<std::mutex> lock(orbTargetsMutex);
        for (const auto& orb : orbTargets) {
            if (!orb.entity) continue;
            // The orb is visible immediately after launch, but it has no
            // hitbox until CItemXP.m_flAttackableTime has elapsed.
            if (!IsXpOrbAttackable(orb.entity)) continue;
            Vector3 point{};
            // Keep orb aim on the same stable world position as Orb ESP.
            // RenderOrigin can be a view-dependent render-cache coordinate;
            // AbsOrigin is the networked position used by the entity itself.
            if (!GetEntityPosition(orb.entity, point) &&
                !GetXpOrbPosition(orb.entity, point)) {
                point = orb.pos;
            }
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) continue;
            const bool traceAvailable = clientBase &&
                Read<uintptr_t>(clientBase + PhysicsTraceContextRva) != 0;
            if (orbAimVisibilityCheck && traceAvailable &&
                !IsWorldAimPointVisible(point, orb.entity)) continue;
            Vector2 screen{};
            if (!WorldToScreen(point, screen, currentViewMatrix)) continue;
            if (screen.x < 0.0f || screen.y < 0.0f ||
                screen.x > display.x || screen.y > display.y) continue;
            const float dx = screen.x - centerX;
            const float dy = screen.y - centerY;
            const float distance = dx * dx + dy * dy;
            if (distance < bestDistance) {
                best = orb;
                best.pos = point;
                bestScreen = screen;
                bestDistance = distance;
            }
        }
    }
    if (!best.entity) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAttack = false;
        pendingOrbReady = false;
        lastOrb = 0;
        lastAttackApplied = 0;
        return;
    }

    Vector3 commandAngles{};
    if (!GetAimAnglesFromScreen(bestScreen.x, bestScreen.y, commandAngles)) return;

    const ULONGLONG now = GetTickCount64();
    if (best.entity != lastOrb) {
        lastOrb = best.entity;
        attackBaseline = autoOrbAttackAppliedCount;
        attackStarted = now;
        lastAttackApplied = 0;
        attackPulsesApplied = 0;
        firstAttackDelay = static_cast<ULONGLONG>(firstDelayDistribution(jitterRng));
        secondAttackDelay = static_cast<ULONGLONG>(secondDelayDistribution(jitterRng));
    }
    // Count only commands that actually reached CreateMove. Two pulses are
    // enough to cover the moving hitbox while preventing repeated firing.
    if (autoOrbAttackAppliedCount != attackBaseline) {
        attackBaseline = autoOrbAttackAppliedCount;
        lastAttackApplied = now;
        if (attackPulsesApplied < 2) ++attackPulsesApplied;
        if (attackPulsesApplied == 1) {
            secondAttackDelay = static_cast<ULONGLONG>(secondDelayDistribution(jitterRng));
        }
    }
    const bool fire = autoLastHitOrbsAutoFire && attackPulsesApplied < 2 &&
        ((attackPulsesApplied == 0 && now - attackStarted >= firstAttackDelay) ||
         (attackPulsesApplied == 1 && now - lastAttackApplied >= secondAttackDelay)) &&
        now - attackStarted < 800;
    {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAngles = commandAngles;
        pendingOrbAttack = fire;
        pendingOrbReady = true;
    }
}

bool GetWorldAimPointScreen(const Vector3& point, Vector2& screen) {
    return currentViewMatrixReady && WorldToScreen(point, screen, currentViewMatrix);
}

float ResolveAimHeightFromBox(const PlayerData& player, float verticalFraction, float fallback) {
    if (!currentViewMatrixReady || !std::isfinite(player.boxTop) ||
        !std::isfinite(player.boxBottom) || player.boxBottom <= player.boxTop) {
        return fallback;
    }

    const float wantedY = player.boxTop + (player.boxBottom - player.boxTop) * verticalFraction;
    float low = -200.0f;
    float high = 300.0f;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const float middle = (low + high) * 0.5f;
        Vector2 screen{};
        if (!GetAimPointScreen(player, middle, screen)) return fallback;
        // Screen Y decreases as world Z increases.
        if (screen.y > wantedY) low = middle;
        else high = middle;
    }
    return (low + high) * 0.5f;
}

bool IsAimPointVisible(const PlayerData& player, float height, float screenX, float screenY) {
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    if (!clientBase || !currentLocalPositionReady) return false;
    if (!Read<uintptr_t>(clientBase + PhysicsTraceContextRva)) return false;
    Vector3 traceStart = currentLocalPosition;
    traceStart.z += 64.0f;
    return PhysicsTraceVisible(traceStart, aimPoint, player.entity);
}

bool IsWorldAimPointVisible(const Vector3& point, uintptr_t targetEntity) {
    if (!clientBase || !currentLocalPositionReady) return false;
    if (!Read<uintptr_t>(clientBase + PhysicsTraceContextRva)) return false;
    Vector3 traceStart = currentLocalPosition;
    traceStart.z += 64.0f;
    return PhysicsTraceVisible(traceStart, point, targetEntity);
}

void AimAtClosestEnemy(const std::vector<PlayerData>& players) {
    humanAimTargetFound = false;
    const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rightButtonDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (!aimSilentMode || !leftButtonDown) ClearPendingSilentAngles();
    const bool configuredAimKeyDown = (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool configuredAimKeyPressed = configuredAimKeyDown && !aimToggleLastDown;
    if (aimToggleMode && configuredAimKeyPressed) aimToggleActive = !aimToggleActive;
    aimToggleLastDown = configuredAimKeyDown;
    const bool keyActive = aimToggleMode ? aimToggleActive : configuredAimKeyDown;
    const bool aiming = aimSilentMode ? (keyActive && leftButtonDown) : keyActive;
    if (!aimAssist || menuOpen || !aiming || !currentViewMatrixReady) return;

    const bool traceAvailable = clientBase &&
        Read<uintptr_t>(clientBase + PhysicsTraceContextRva) != 0;
    const bool useDepthWallCheck = aimVisibilityCheck && traceAvailable;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const PlayerData* best = nullptr;
    Vector2 bestAimScreen{};
    float bestDistance = aimFov * aimFov;
    size_t visibleTargets = 0;
    size_t testedTargets = 0;

    for (const auto& player : players) {
        if (localTeam != 0 && player.team == localTeam) continue;
        const float modelHeight = player.modelHeight > 20.0f ? player.modelHeight : 80.0f;
        const float modelMinZ = std::isfinite(player.modelMinZ) ? player.modelMinZ : 0.0f;
        const float fallbackHead = modelMinZ + modelHeight * 0.92f;
        const float fallbackBody = modelMinZ + modelHeight * 0.62f;
        const float headHeight = ResolveAimHeightFromBox(player, 0.12f, fallbackHead);
        const float bodyHeight = ResolveAimHeightFromBox(player, 0.60f, fallbackBody);
        const float centerHeight = ResolveAimHeightFromBox(player, 0.50f, modelMinZ + modelHeight * 0.50f);
        const Vector3 fallbackHeadPoint{ player.pos.x, player.pos.y, player.pos.z + headHeight };
        const Vector3 fallbackBodyPoint{ player.pos.x, player.pos.y, player.pos.z + bodyHeight };
        const Vector3 fallbackCenterPoint{ player.pos.x, player.pos.y, player.pos.z + centerHeight };
        struct AimCandidate { Vector3 point; bool bone; };
        AimCandidate candidates[3]{};
        int candidateCount = 0;
        if (aimTargetMode == AimTargetMode::Head) {
            candidates[candidateCount++] = { player.hasHeadBone ? player.headPos : fallbackHeadPoint, player.hasHeadBone };
        } else if (aimTargetMode == AimTargetMode::Body) {
            candidates[candidateCount++] = { player.hasBodyBone ? player.bodyPos : fallbackBodyPoint, player.hasBodyBone };
        } else {
            if (player.hasHeadBone) candidates[candidateCount++] = { player.headPos, true };
            if (player.hasBodyBone) candidates[candidateCount++] = { player.bodyPos, true };
            if (candidateCount == 0) candidates[candidateCount++] = { fallbackCenterPoint, false };
        }
        bool targetVisible = false;
        Vector2 visibleAimScreen{};
        float visibleDistance = FLT_MAX;
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            const Vector3 point = candidates[candidateIndex].point;
            Vector2 aimScreen{};
            if (!GetWorldAimPointScreen(point, aimScreen)) continue;
            const float dx = aimScreen.x - cx;
            const float dy = aimScreen.y - cy;
            const float distance = dx * dx + dy * dy;
            if (distance >= bestDistance) continue;
            ++testedTargets;
            if (useDepthWallCheck && (candidates[candidateIndex].bone
                ? !IsWorldAimPointVisible(point, player.entity)
                : !IsAimPointVisible(player, point.z - player.pos.z, aimScreen.x, aimScreen.y))) continue;
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

    if (!best) {
        if (aimSilentMode) ClearPendingSilentAngles();
        return;
    }
    humanAimTargetFound = true;

    const float targetX = bestAimScreen.x;
    const float targetY = bestAimScreen.y;
    const float smooth = aimSilentMode ? 1.0f : (aimSmooth < 1.0f ? 1.0f : aimSmooth);
    const LONG moveX = static_cast<LONG>((targetX - cx) / smooth);
    const LONG moveY = static_cast<LONG>((targetY - cy) / smooth);
    if (aimSilentMode) {
        Vector3 commandAngles{};
        if (GetAimAnglesFromScreen(targetX, targetY, commandAngles)) {
            std::lock_guard<std::mutex> lock(humanSilentMutex);
            pendingHumanAngles = commandAngles;
            pendingHumanReady = true;
        }
        return;
    }
    if (moveX || moveY) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = moveX;
        input.mi.dy = moveY;
        SendInput(1, &input, sizeof(INPUT));
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

    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    for (const uintptr_t pawn : pawns) {
        const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
        if (!pawn || pawn == currentLocalPawn || localTeam == 0 ||
            (team != 2 && team != 3) || team == localTeam) continue;

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
