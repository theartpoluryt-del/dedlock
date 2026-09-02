#include "shared.h"
#include "hero_scripts.h"
#include "portable_paths.h"
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

// Temporary measurement mode: suppress verbose aim/parry diagnostics.
#define printf(...) do { } while (0)

static Vector3 cachedFarmAimPoint{};

bool WorldToScreen(const Vector3& pos, Vector2& screen, const Matrix4x4& matrix) {
    float w = matrix.m[3][0] * pos.x + matrix.m[3][1] * pos.y + matrix.m[3][2] * pos.z + matrix.m[3][3];
    if (w < 0.01f) return false;

    float x = matrix.m[0][0] * pos.x + matrix.m[0][1] * pos.y + matrix.m[0][2] * pos.z + matrix.m[0][3];
    float y = matrix.m[1][0] * pos.x + matrix.m[1][1] * pos.y + matrix.m[1][2] * pos.z + matrix.m[1][3];

    const float displayWidth =
        overlayProjectionWidth.load(std::memory_order_acquire);
    const float displayHeight =
        overlayProjectionHeight.load(std::memory_order_acquire);
    if (displayWidth <= 0.0f || displayHeight <= 0.0f) return false;

    float invW = 1.0f / w;
    screen.x = (displayWidth * 0.5f) + (x * invW * displayWidth * 0.5f);
    screen.y = (displayHeight * 0.5f) - (y * invW * displayHeight * 0.5f);
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
    uintptr_t found = 0;
    for (std::size_t i = 0; i <= size - bytes.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && Read<uint8_t>(base + i + j) != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (found) return 0;
        found = base + i;
    }
    return found;
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

float normalMouseRemainderX = 0.0f;
float normalMouseRemainderY = 0.0f;

void ResetNormalMouseAim() {
    normalMouseRemainderX = 0.0f;
    normalMouseRemainderY = 0.0f;
}

void ApplyNormalMouseAim(float deltaX, float deltaY) {
    if (!gameWindow || !IsWindow(gameWindow)) return;
    const HWND foreground = GetForegroundWindow();
    if (!foreground || (foreground != gameWindow &&
        GetAncestor(foreground, GA_ROOT) != GetAncestor(gameWindow, GA_ROOT))) {
        ResetNormalMouseAim();
        return;
    }

    const float yawSmooth = (std::max)(1.0f, aimYawSmooth);
    const float pitchSmooth = (std::max)(1.0f, aimPitchSmooth);
    float stepX = std::fabs(deltaX) < 0.25f ? 0.0f : deltaX / yawSmooth;
    float stepY = std::fabs(deltaY) < 0.25f ? 0.0f : deltaY / pitchSmooth;

    // Limit the complete movement vector instead of clamping each axis to
    // eight counts. Independent axis clamps turn diagonal tracking into a
    // staircase, while the old tiny limit also made smooth=1 unnecessarily
    // slow. Keep low smoothing fast and progressively lower the safety cap.
    const float averageSmooth = (yawSmooth + pitchSmooth) * 0.5f;
    const float maxMagnitude = 96.0f / std::sqrt(averageSmooth);
    const float magnitude = std::hypot(stepX, stepY);
    if (magnitude > maxMagnitude && magnitude > 0.0f) {
        const float scale = maxMagnitude / magnitude;
        stepX *= scale;
        stepY *= scale;
    }
    normalMouseRemainderX += stepX;
    normalMouseRemainderY += stepY;
    const LONG moveX = static_cast<LONG>(std::lround(normalMouseRemainderX));
    const LONG moveY = static_cast<LONG>(std::lround(normalMouseRemainderY));
    normalMouseRemainderX -= static_cast<float>(moveX);
    normalMouseRemainderY -= static_cast<float>(moveY);
    if (!moveX && !moveY) return;

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = moveX;
    input.mi.dy = moveY;
    SendInput(1, &input, sizeof(input));
}

}

static ID3D11Texture2D* cachedGameDepth = nullptr;
static std::vector<uint8_t> depthSnapshotData;
static UINT depthSnapshotRowPitch = 0;
static std::mutex trackedDepthMutex;
static std::atomic<bool> trackedDepthArmed{true};
static ID3D11Query* frameFenceQuery = nullptr;
static std::atomic<bool> mainDepthSeen{false};
static std::atomic<bool> frameFenceIssued{false};
// Native engine traces own all aim visibility in the current build. Keep the
// depth implementation available as a fallback, but do not track or read back
// D3D depth resources while it is unused.
constexpr bool EnableNativeAimTrace = true;

void ArmGameDepthCapture() {
    mainDepthSeen.store(false, std::memory_order_release);
    trackedDepthArmed.store(true, std::memory_order_release);
}

void TrackGameDepthStencil(ID3D11DepthStencilView* depthView) {
    if (!depthView ||
        !trackedDepthArmed.load(std::memory_order_acquire)) return;

    ID3D11Resource* resource = nullptr;
    depthView->GetResource(&resource);
    if (!resource) return;
    ID3D11Texture2D* texture = nullptr;
    const HRESULT queryResult = resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
    resource->Release();
    if (FAILED(queryResult) || !texture) return;

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    RECT client{};
    const bool haveClient = gameWindow && GetClientRect(gameWindow, &client);
    const UINT clientWidth = haveClient ? static_cast<UINT>(client.right - client.left) : 0;
    const UINT clientHeight = haveClient ? static_cast<UINT>(client.bottom - client.top) : 0;
    const bool supportedFormat =
        desc.Format == DXGI_FORMAT_D32_FLOAT ||
        desc.Format == DXGI_FORMAT_R32_TYPELESS ||
        desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
        desc.Format == DXGI_FORMAT_R24G8_TYPELESS ||
        desc.Format == DXGI_FORMAT_D16_UNORM;
    const bool displaySized = clientWidth && clientHeight &&
        desc.Width == clientWidth && desc.Height == clientHeight;
    if (!supportedFormat || desc.SampleDesc.Count != 1 || !displaySized) {
        texture->Release();
        return;
    }

    bool expected = true;
    if (!trackedDepthArmed.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel)) {
        texture->Release();
        return;
    }
    mainDepthSeen.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(trackedDepthMutex);
    if (cachedGameDepth) cachedGameDepth->Release();
    cachedGameDepth = texture;
}

void SignalEarlyFrameFence(ID3D11DeviceContext* context) {
    if (!context || context != pContext || !pDevice ||
        !mainDepthSeen.load(std::memory_order_acquire))
        return;
    bool expected = false;
    if (!frameFenceIssued.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel))
        return;
    if (!frameFenceQuery) {
        const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
        if (FAILED(pDevice->CreateQuery(&description, &frameFenceQuery))) {
            frameFenceIssued.store(false, std::memory_order_release);
            return;
        }
    }
    context->End(frameFenceQuery);
}

// Native visibility is resolved from the live client image. TraceShape, the
// manager global and the complete CTraceFilter constructor all move between
// game patches, so none of them is kept as a hardcoded RVA.
constexpr char TraceShapePattern[] =
    "48 89 5C 24 20 48 89 4C 24 08 55 56 41 54 41 55 41 56 "
    "48 8D AC 24 10 E0 FF FF B8 F0 20 00 00 E8 ? ? ? ? "
    "48 2B E0 44 8B 15 ? ? ? ? 48 8B F1";
constexpr char TraceFilterConstructorPattern[] =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? "
    "0F B6 41 ? 33 FF";

struct NativeVisibilityRuntime {
    uintptr_t traceShape{};
    uintptr_t filterConstructor{};
    uintptr_t managerGlobal{};
    std::size_t imageSize{};
    ULONGLONG lastAttemptAt{};
    bool attempted{};
    bool ready{};
};

static NativeVisibilityRuntime nativeVisibility{};
static std::mutex nativeVisibilityMutex;

uintptr_t FindVisibilityPattern(const char* pattern) {
    std::vector<int> bytes;
    std::istringstream stream(pattern ? pattern : "");
    std::string token;
    while (stream >> token) {
        bytes.push_back(token == "?" || token == "??"
            ? -1 : std::stoi(token, nullptr, 16));
    }
    if (bytes.empty() || bytes.size() > nativeVisibility.imageSize)
        return 0;

    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    const std::size_t last = nativeVisibility.imageSize - bytes.size();
    uintptr_t found = 0;
    for (std::size_t offset = 0; offset <= last; ++offset) {
        bool matches = true;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (bytes[i] >= 0 &&
                image[offset + i] != static_cast<uint8_t>(bytes[i])) {
                matches = false;
                break;
            }
        }
        if (!matches) continue;
        if (found) return 0;
        found = clientBase + offset;
    }
    return found;
}

bool IsInClientImage(uintptr_t address, std::size_t size = 1) {
    if (!clientBase || !nativeVisibility.imageSize || address < clientBase)
        return false;
    const uintptr_t offset = address - clientBase;
    return offset <= nativeVisibility.imageSize &&
        size <= nativeVisibility.imageSize - offset;
}

uintptr_t FindTraceManagerGlobal(uintptr_t traceShape) {
    if (!traceShape || nativeVisibility.imageSize < 64) return 0;

    std::unordered_map<uintptr_t, unsigned int> candidates;
    const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
    const uintptr_t end = clientBase + nativeVisibility.imageSize - 5;
    for (uintptr_t call = clientBase; call < end; ++call) {
        if (image[call - clientBase] != 0xE8) continue;
        const int32_t relative = Read<int32_t>(call + 1);
        if (call + 5 + static_cast<intptr_t>(relative) != traceShape) continue;

        const uintptr_t first =
            call > clientBase + 64 ? call - 64 : clientBase;
        for (uintptr_t probe = call; probe-- > first;) {
            const auto offset = probe - clientBase;
            if (image[offset] != 0x48 ||
                image[offset + 1] != 0x8B ||
                image[offset + 2] != 0x0D) {
                continue;
            }
            const int32_t displacement = Read<int32_t>(probe + 3);
            const uintptr_t global =
                probe + 7 + static_cast<intptr_t>(displacement);
            if (!IsInClientImage(global, sizeof(uintptr_t))) continue;
            const uintptr_t object = Read<uintptr_t>(global);
            if (object <= 0x10000) continue;
            ++candidates[global];
            break;
        }
    }

    uintptr_t best{};
    unsigned int bestCount{};
    for (const auto& [candidate, count] : candidates) {
        if (count > bestCount) {
            best = candidate;
            bestCount = count;
        }
    }
    return best;
}

bool ResolveNativeVisibility() {
    std::lock_guard<std::mutex> lock(nativeVisibilityMutex);
    if (nativeVisibility.ready) return true;
    const ULONGLONG now = GetTickCount64();
    if (nativeVisibility.attempted &&
        now - nativeVisibility.lastAttemptAt < 1000) {
        return false;
    }
    nativeVisibility.attempted = true;
    nativeVisibility.lastAttemptAt = now;
    if (!clientBase) return false;

    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(
            GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase),
            &moduleInfo, sizeof(moduleInfo))) {
        return false;
    }
    nativeVisibility.imageSize =
        static_cast<std::size_t>(moduleInfo.SizeOfImage);
    nativeVisibility.traceShape =
        FindVisibilityPattern(TraceShapePattern);
    nativeVisibility.filterConstructor =
        FindVisibilityPattern(TraceFilterConstructorPattern);
    nativeVisibility.managerGlobal =
        FindTraceManagerGlobal(nativeVisibility.traceShape);
    nativeVisibility.ready =
        nativeVisibility.traceShape &&
        nativeVisibility.filterConstructor &&
        nativeVisibility.managerGlobal;

    std::ofstream log(
        Dll6Paths::DataFileA("visibility_runtime.log"),
        std::ios::app);
    if (log) {
        log << "trace=0x" << std::hex
            << (nativeVisibility.traceShape - clientBase)
            << " filterConstructor=0x"
            << (nativeVisibility.filterConstructor - clientBase)
            << " manager=0x"
            << (nativeVisibility.managerGlobal - clientBase)
            << std::dec << " ready=" << nativeVisibility.ready << '\n';
    }
    return nativeVisibility.ready;
}

struct VisibilityTraceRequest {
    Vector3 point{};
    Vector3 start{};
    uintptr_t targetEntity{};
    uintptr_t cacheKey{};
    ULONGLONG requestedAt{};
    bool customStart = false;
};

struct VisibilityTraceCacheEntry {
    Vector3 point{};
    Vector3 start{};
    bool visible = true;
    ULONGLONG completedAt{};
};

static std::mutex visibilityTraceMutex;
static std::unordered_map<uintptr_t, VisibilityTraceRequest> pendingVisibilityTraces;
static std::unordered_map<uintptr_t, VisibilityTraceCacheEntry> visibilityTraceCache;

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
    if (!boneFunctions.calcWorldSpaceBones ||
        !boneFunctions.getBoneIdByName) return false;

    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    __try {
        // Match the proven c3 visual path.  The per-Present depth fence has
        // completed the rendered frame, so rebuilding once here produces the
        // current world-space pose instead of consuming the lagging CPU cache.
        boneFunctions.calcWorldSpaceBones(sceneNode, 0xFFFFFu);
        const int boneIndex = boneFunctions.getBoneIdByName(entity, boneName);
        if (boneIndex < 0 || boneIndex > 512) return false;

        // CModelState starts at CSkeletonInstance + 0x150 and the runtime
        // bone pointer is the first field after its 0x80-byte prefix.
        const uintptr_t bones = Read<uintptr_t>(sceneNode + 0x150 + 0x80);
        if (!bones) return false;
        const uintptr_t boneAddress =
            bones + static_cast<uintptr_t>(boneIndex) * 0x20;
        for (int attempt = 0; attempt < 4; ++attempt) {
            const Vector3 first = Read<Vector3>(boneAddress);
            const Vector3 second = Read<Vector3>(boneAddress);
            if (std::memcmp(&first, &second, sizeof(first)) != 0) continue;
            position = second;
            return std::isfinite(position.x) &&
                   std::isfinite(position.y) &&
                   std::isfinite(position.z);
        }
        position = {};
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        position = {};
        return false;
    }
}

bool GetEntityBoneSkeleton(uintptr_t entity, std::vector<BoneSegment>& segments) {
    segments.clear();
    if (!entity) return false;
    ResolveBoneFunctions();
    if (!boneFunctions.calcWorldSpaceBones ||
        !boneFunctions.getBoneIdByName) return false;

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
            const uintptr_t boneAddress =
                bones + static_cast<uintptr_t>(index) * 0x20;
            for (int attempt = 0; attempt < 4; ++attempt) {
                const Vector3 first = Read<Vector3>(boneAddress);
                const Vector3 second = Read<Vector3>(boneAddress);
                if (std::memcmp(&first, &second, sizeof(first)) != 0)
                    continue;
                position = second;
                return std::isfinite(position.x) &&
                       std::isfinite(position.y) &&
                       std::isfinite(position.z);
            }
            position = {};
            return false;
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

bool GetEntityPreviewSkeleton(uintptr_t entity,
                              std::array<Vector3, 18>& points,
                              std::array<bool, 18>& valid) {
    points = {};
    valid = {};
    if (!entity) return false;
    ResolveBoneFunctions();
    if (!boneFunctions.calcWorldSpaceBones ||
        !boneFunctions.getBoneIdByName) return false;
    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;
    static constexpr const char* names[18]{
        "head", "spine_3", "spine_2",
        "arm_upper_L", "arm_lower_L", "hand_L", "wrist_L",
        "arm_upper_R", "arm_lower_R", "hand_R", "wrist_R",
        "spine_0", "leg_upper_L", "leg_lower_L", "foot_L",
        "leg_upper_R", "leg_lower_R", "foot_R"
    };
    __try {
        boneFunctions.calcWorldSpaceBones(sceneNode, 0xFFFFFu);
        const uintptr_t bones = Read<uintptr_t>(sceneNode + 0x1D0);
        if (!bones) return false;
        for (size_t i = 0; i < std::size(names); ++i) {
            int index = boneFunctions.getBoneIdByName(entity, names[i]);
            const auto tryBone = [&](const char* alternate) {
                if (index < 0)
                    index = boneFunctions.getBoneIdByName(entity, alternate);
            };
            if (i == 4) { tryBone("forearm_L"); tryBone("elbow_L"); }
            if (i == 5 || i == 6) { tryBone("wrist_L"); tryBone("hand_L"); }
            if (i == 8) { tryBone("forearm_R"); tryBone("elbow_R"); }
            if (i == 9 || i == 10) { tryBone("wrist_R"); tryBone("hand_R"); }
            if (i == 14) { tryBone("ankle_L"); tryBone("leg_L_IKTARGET"); }
            if (i == 17) { tryBone("ankle_R"); tryBone("leg_R_IKTARGET"); }
            if (index < 0 || index > 512) continue;
            const Vector3 position = Read<Vector3>(
                bones + static_cast<uintptr_t>(index) * 0x20);
            if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z)) continue;
            points[i] = position;
            valid[i] = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        points = {};
        valid = {};
        return false;
    }
    return std::count(valid.begin(), valid.end(), true) >= 10;
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

bool PopulatePlayerAimBones(uintptr_t entity, PlayerData& player,
                            bool includeSkeleton) {
    if (!entity) return false;
    ResolveBoneFunctions();
    if (!boneFunctions.getBoneIdByName) return false;
    const uintptr_t sceneNode =
        Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    __try {
        // Rendered heroes already own a current world-space pose. Forcing the
        // engine to rebuild it for every hero on every visual frame duplicated
        // animation work and reduced the game's FPS. Only initialize a pose
        // when the engine has not published its bone array yet.
        uintptr_t bones = Read<uintptr_t>(sceneNode + 0x1D0);
        if (!bones && boneFunctions.calcWorldSpaceBones) {
            boneFunctions.calcWorldSpaceBones(sceneNode, 0xFFFFFu);
            bones = Read<uintptr_t>(sceneNode + 0x1D0);
        }
        if (!bones) return false;
        const auto readBone = [&](const char* name, Vector3& position) {
            const int index = boneFunctions.getBoneIdByName(entity, name);
            if (index < 0 || index > 512) return false;
            const uintptr_t address =
                bones + static_cast<uintptr_t>(index) * 0x20;
            const Vector3 first = Read<Vector3>(address);
            const Vector3 second = Read<Vector3>(address);
            if (std::memcmp(&first, &second, sizeof(first)) != 0) return false;
            position = second;
            return std::isfinite(position.x) && std::isfinite(position.y) &&
                   std::isfinite(position.z);
        };
        player.hasHeadBone = readBone("head", player.headPos);
        player.hasNeckBone = readBone("spine_3", player.neckPos);
        player.hasBodyBone = readBone("spine_2", player.bodyPos);
        if (!player.hasBodyBone)
            player.hasBodyBone = readBone("spine_0", player.bodyPos);
        player.hasLeftArmBone =
            readBone("arm_upper_L", player.leftArmPos);
        player.hasRightArmBone =
            readBone("arm_upper_R", player.rightArmPos);
        player.hasLeftLegBone =
            readBone("leg_upper_L", player.leftLegPos);
        player.hasRightLegBone =
            readBone("leg_upper_R", player.rightLegPos);
        if (includeSkeleton) {
            struct BonePair { const char* start; const char* end; };
            static constexpr BonePair pairs[] = {
                {"spine_0", "spine_1"}, {"spine_1", "spine_2"},
                {"spine_2", "spine_3"}, {"spine_3", "head"},
                {"spine_3", "arm_upper_L"},
                {"arm_upper_L", "arm_lower_L"},
                {"arm_lower_L", "arm_lower_L_TWIST"},
                {"arm_lower_L_TWIST", "arm_lower_L_TWIST1"},
                {"arm_lower_L", "hand_L"},
                {"arm_upper_L", "forearm_L"}, {"forearm_L", "hand_L"},
                {"arm_upper_L", "elbow_L"}, {"elbow_L", "wrist_L"},
                {"spine_3", "arm_upper_R"},
                {"arm_upper_R", "arm_lower_R"},
                {"arm_lower_R", "arm_lower_R_TWIST"},
                {"arm_lower_R_TWIST", "arm_lower_R_TWIST1"},
                {"arm_lower_R", "hand_R"},
                {"arm_upper_R", "forearm_R"}, {"forearm_R", "hand_R"},
                {"arm_upper_R", "elbow_R"}, {"elbow_R", "wrist_R"},
                {"spine_0", "leg_upper_L"},
                {"leg_upper_L", "leg_lower_L"},
                {"leg_lower_L", "leg_L_IKTARGET"},
                {"spine_0", "leg_upper_R"},
                {"leg_upper_R", "leg_lower_R"},
                {"leg_lower_R", "leg_R_IKTARGET"}
            };
            player.bones.clear();
            player.bones.reserve(std::size(pairs));
            for (const auto& pair : pairs) {
                Vector3 start{}, end{};
                if (readBone(pair.start, start) && readBone(pair.end, end))
                    player.bones.push_back({start, end});
            }
        }
        return player.hasHeadBone || player.hasNeckBone ||
               player.hasBodyBone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GetAimAnglesToWorldPoint(const Vector3& point, Vector3& angles) {
    Vector3 source{};
    bool sourceReady = currentCameraPositionReady;
    if (sourceReady) source = currentCameraPosition;
    if (!sourceReady && currentLocalPositionReady) {
        source = currentLocalPosition;
        source.z += 64.0f;
        sourceReady = true;
    }
    if (!sourceReady) return false;

    const float dx = point.x - source.x;
    const float dy = point.y - source.y;
    const float dz = point.z - source.z;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    if (!std::isfinite(horizontal) || horizontal < 1e-5f ||
        !std::isfinite(dz))
        return false;

    constexpr float RadToDeg = 57.29577951308232f;
    angles = {
        std::clamp(-std::atan2(dz, horizontal) * RadToDeg, -89.0f, 89.0f),
        std::atan2(dy, dx) * RadToDeg,
        0.0f
    };
    angles = NormalizeAimAngles(angles);
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
    if (!ResolveNativeVisibility()) return true;

    using TraceFn = bool(__fastcall*)(void*, void*, const Vector3*, const Vector3*, void*, void*);
    // Native ABI verified in the current client: (this, skip entity, mask,
    // layer, unknown). The public Andromeda wrapper deliberately reorders the
    // user-facing mask/entity parameters before making this call.
    using FilterConstructorFn = void(__fastcall*)(
        void*, uintptr_t, uint64_t, int, uint16_t);
    const uintptr_t physics =
        Read<uintptr_t>(nativeVisibility.managerGlobal);
    if (!physics) return false;

    // The guide's current line-trace ABI uses a zero-initialized 0x40 Ray_t.
    alignas(16) uint8_t ray[0x40]{};
    alignas(16) uint8_t filter[0x110]{};
    alignas(16) uint8_t result[256]{};

    __try {
        if (!currentLocalPawn) return true;
        auto constructFilter = reinterpret_cast<FilterConstructorFn>(
            nativeVisibility.filterConstructor);
        // Current TraceToEntityEndPos mask from the visibility implementation.
        constructFilter(filter, currentLocalPawn, 0x1C3003, 3, 15);

        auto trace =
            reinterpret_cast<TraceFn>(nativeVisibility.traceShape);
        const bool traced = trace(reinterpret_cast<void*>(physics), ray,
                                  &start, &end, filter, result);

        const float fraction =
            *reinterpret_cast<const float*>(result + 0xAC);
        const bool startSolid = result[0xB7] != 0;
        const uintptr_t hitEntity =
            *reinterpret_cast<const uintptr_t*>(result + 0x08);
        // A stale/partially initialized trace result must not remove every
        // target from the aim list. Only a valid native result may veto aim.
        if (!std::isfinite(fraction) || fraction < 0.0f || fraction > 1.0f)
            return true;
        // TraceToEntityEndPos in Andromeda treats the target as visible when
        // the native trace's first hit entity is the requested pawn.  A trace
        // that reaches almost the entire segment is also unobstructed: moving
        // bones can finish just outside the target's current collision shape,
        // leaving hitEntity null with fraction == 1.
        // TraceShape returns false when the full segment is unobstructed; in
        // that case fraction is 1 and hitEntity is null. The return value only
        // says whether a shape was hit, not whether the line is usable.
        (void)traced;
        return !startSolid &&
            ((targetEntity && hitEntity == targetEntity) || fraction > 0.93f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

bool QueueAimVisibilityTrace(const Vector3& point, uintptr_t targetEntity) {
    const uintptr_t key = targetEntity ? targetEntity : 1;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(visibilityTraceMutex);
    pendingVisibilityTraces[key] = {
        point, {}, targetEntity, key, now, false};

    const auto cached = visibilityTraceCache.find(key);
    // Unknown visibility must never be treated as visible. That optimistic
    // fallback let a newly acquired/previously visible target pull through a
    // wall until CreateMove published the delayed trace result.
    if (cached == visibilityTraceCache.end() ||
        now - cached->second.completedAt > 50)
        return false;
    const float dx = cached->second.point.x - point.x;
    const float dy = cached->second.point.y - point.y;
    const float dz = cached->second.point.z - point.z;
    // Keep a recent result across ordinary per-frame animation/movement, but
    // require a fresh trace after a teleport or a materially different point.
    if (dx * dx + dy * dy + dz * dz > 96.0f * 96.0f) return false;
    return cached->second.visible;
}

bool TryGetWorldVisibilitySnapshot(
    const Vector3& point, uintptr_t targetEntity, bool& visible) {
    if (!currentCameraPositionReady) return false;
    constexpr uintptr_t CameraVisibilityKey =
        uintptr_t{1} << (sizeof(uintptr_t) * 8 - 1);
    const uintptr_t entityKey = targetEntity ? targetEntity : 1;
    const uintptr_t key = entityKey ^ CameraVisibilityKey;
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(visibilityTraceMutex);
    pendingVisibilityTraces[key] = {
        point, currentCameraPosition, targetEntity, key, now, true};
    const auto cached = visibilityTraceCache.find(key);
    if (cached == visibilityTraceCache.end() ||
        now - cached->second.completedAt > 100)
        return false;
    const float dx = cached->second.point.x - point.x;
    const float dy = cached->second.point.y - point.y;
    const float dz = cached->second.point.z - point.z;
    if (dx * dx + dy * dy + dz * dz > 96.0f * 96.0f)
        return false;
    const float sx = cached->second.start.x - currentCameraPosition.x;
    const float sy = cached->second.start.y - currentCameraPosition.y;
    const float sz = cached->second.start.z - currentCameraPosition.z;
    if (sx * sx + sy * sy + sz * sz > 32.0f * 32.0f)
        return false;
    visible = cached->second.visible;
    return true;
}

void ProcessAimVisibilityTraces() {
    if constexpr (!EnableNativeAimTrace) return;
    if (!clientBase || !currentLocalPositionReady ||
        !ResolveNativeVisibility() ||
        !Read<uintptr_t>(nativeVisibility.managerGlobal)) return;

    std::vector<VisibilityTraceRequest> requests;
    {
        std::lock_guard<std::mutex> lock(visibilityTraceMutex);
        requests.reserve((std::min)(pendingVisibilityTraces.size(), size_t{16}));
        for (auto it = pendingVisibilityTraces.begin();
             it != pendingVisibilityTraces.end() && requests.size() < 16;) {
            requests.push_back(it->second);
            it = pendingVisibilityTraces.erase(it);
        }
    }

    for (const auto& request : requests) {
        Vector3 traceStart = request.start;
        if (!request.customStart) {
            traceStart = currentLocalPosition;
            traceStart.z += 64.0f;
        }
        const bool visible =
            PhysicsTraceVisible(traceStart, request.point, request.targetEntity);
        const uintptr_t key = request.cacheKey
            ? request.cacheKey
            : (request.targetEntity ? request.targetEntity : 1);
        std::lock_guard<std::mutex> lock(visibilityTraceMutex);
        visibilityTraceCache[key] =
            {request.point, traceStart, visible, GetTickCount64()};
    }
}

bool CaptureDepthSnapshot() {
    depthSnapshotReady = false;
    if constexpr (EnableNativeAimTrace) {
        if (!pContext || !pDevice) return true;
        if (!frameFenceQuery) {
            const D3D11_QUERY_DESC description{D3D11_QUERY_EVENT, 0};
            if (FAILED(pDevice->CreateQuery(&description, &frameFenceQuery)))
                return true;
        }
        bool expected = false;
        if (frameFenceIssued.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            // Fallback for render paths that never explicitly unbind the main
            // depth target. Normally the query was already issued earlier.
            pContext->End(frameFenceQuery);
        }
        LARGE_INTEGER frequency{}, started{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&started);
        unsigned spinCount = 0;
        for (;;) {
            // Submit pending commands once. Further polls must not repeatedly
            // flush the immediate context while the GPU catches up.
            const UINT flags = spinCount == 0
                ? 0u : D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const HRESULT status = pContext->GetData(
                frameFenceQuery, nullptr, 0, flags);
            if (status == S_OK) {
                frameFenceIssued.store(false, std::memory_order_release);
                return true;
            }
            if (FAILED(status)) {
                frameFenceIssued.store(false, std::memory_order_release);
                return false;
            }
            LARGE_INTEGER current{};
            QueryPerformanceCounter(&current);
            const double elapsedMs = frequency.QuadPart
                ? (current.QuadPart - started.QuadPart) * 1000.0 /
                      frequency.QuadPart
                : 0.0;
            if (elapsedMs >= 16.0) {
                frameFenceQuery->Release();
                frameFenceQuery = nullptr;
                frameFenceIssued.store(false, std::memory_order_release);
                return false;
            }
            if ((++spinCount & 0xFFu) == 0)
                SwitchToThread();
            else
                YieldProcessor();
        }
    }
    if (frameFenceQuery) {
        frameFenceQuery->Release();
        frameFenceQuery = nullptr;
    }
    frameFenceIssued.store(false, std::memory_order_release);
    mainDepthSeen.store(false, std::memory_order_release);
    auto setDepthState = [](int state) {
        if (depthDiagnosticState == state) return;
        depthDiagnosticState = state;
        FILE* file = nullptr;
        if (fopen_s(&file, Dll6Paths::DataFileA("visibility_runtime.log").c_str(), "a") == 0 && file) {
            fprintf(file, "depth_state=%d\\n", state);
            fclose(file);
        }
    };
    if (!pContext || !pDevice) {
        setDepthState(0);
        // An unavailable depth sample is not proof of occlusion. Do not
        // suppress the complete aim target list while the DSV is changing.
        return true;
    }

    // Prefer the DSV that is still bound at this Present.  Mapping a copy of
    // that exact resource fences the frame being displayed.  The tracked DSV
    // is only a fallback for render paths that unbind depth before Present;
    // preferring it introduced a stable one-frame phase delay for all ESP.
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
            std::lock_guard<std::mutex> lock(trackedDepthMutex);
            if (cachedGameDepth) cachedGameDepth->Release();
            cachedGameDepth = depthTexture;
            cachedGameDepth->AddRef();
        }
    } else {
        std::lock_guard<std::mutex> lock(trackedDepthMutex);
        if (cachedGameDepth) {
            depthTexture = cachedGameDepth;
            depthTexture->AddRef();
        }
    }

    if (!depthTexture) {
        setDepthState(1);
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    depthTexture->GetDesc(&desc);
    if (desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1 ||
        (desc.Format != DXGI_FORMAT_D32_FLOAT && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
         desc.Format != DXGI_FORMAT_D24_UNORM_S8_UINT && desc.Format != DXGI_FORMAT_R24G8_TYPELESS &&
         desc.Format != DXGI_FORMAT_D16_UNORM)) {
        depthTexture->Release();
        setDepthState(2);
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
            setDepthState(3);
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
        setDepthState(4);
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
    static uint32_t depthDiagnostics = 0;
    if ((++depthDiagnostics % 120u) == 0u) {
        float minDepth = 1.0f;
        float maxDepth = 0.0f;
        uint32_t populated = 0;
        constexpr UINT grid = 16;
        for (UINT gy = 0; gy < grid; ++gy) {
            for (UINT gx = 0; gx < grid; ++gx) {
                float sample = 0.0f;
                if (!ReadDepthAt(depthWidth * (gx + 0.5f) / grid,
                                 depthHeight * (gy + 0.5f) / grid, sample)) continue;
                if (sample < minDepth) minDepth = sample;
                if (sample > maxDepth) maxDepth = sample;
                if (sample > 0.001f && sample < 0.9999f) ++populated;
            }
        }
        FILE* file = nullptr;
        if (fopen_s(&file, Dll6Paths::DataFileA("visibility_runtime.log").c_str(), "a") == 0 && file) {
            fprintf(file, "depth_range format=%u min=%.6f max=%.6f populated=%u\\n",
                    static_cast<unsigned>(depthFormat), minDepth, maxDepth, populated);
            fclose(file);
        }
    }
    setDepthState(5);
    return true;
}

void ReleaseAimResources() {
    if (frameFenceQuery) {
        frameFenceQuery->Release();
        frameFenceQuery = nullptr;
    }
    frameFenceIssued.store(false, std::memory_order_release);
    mainDepthSeen.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(trackedDepthMutex);
        if (cachedGameDepth) {
            cachedGameDepth->Release();
            cachedGameDepth = nullptr;
        }
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

    // The crosshair can point at sky, so checking only the centre falsely
    // reports an empty depth buffer. Search a sparse screen grid instead.
    constexpr float samples[] = { 0.10f, 0.25f, 0.40f, 0.55f, 0.70f, 0.85f };
    for (const float y : samples) {
        for (const float x : samples) {
            float depth = 0.0f;
            // 1.0 is the cleared D3D depth value, not scene geometry.
            if (ReadDepthAt(depthWidth * x, depthHeight * y, depth) &&
                depth > 0.001f && depth < 0.9999f)
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

    const float displayX = display.x * 0.5f + clipX / w * display.x * 0.5f;
    const float displayY = display.y * 0.5f - clipY / w * display.y * 0.5f;
    // Source 2 uses the DirectX depth convention: after the perspective
    // divide, clipZ/w is already the [0, 1] depth written to the DSV.
    const float projectedDepth = clipZ / w;
    if (!std::isfinite(displayX) || !std::isfinite(displayY) ||
        !std::isfinite(projectedDepth) || projectedDepth < 0.0f || projectedDepth > 1.0f)
        return false;

    const float screenX = displayX * static_cast<float>(depthWidth) / display.x;
    const float screenY = displayY * static_cast<float>(depthHeight) / display.y;

    // Use a small neighborhood: a single depth pixel can belong to a thin
    // model part or be unsettled while the render target is changing.
    constexpr float offsets[][2] = {{0, 0}, {-2, 0}, {2, 0}, {0, -2}, {0, 2}};
    static uint32_t visibilityDiagnostics = 0;
    const bool logVisibility = (++visibilityDiagnostics % 60u) == 0u;
    bool sampled = false;
    bool matched = false;
    float firstSceneDepth = -1.0f;
    float comparisonTargetDepth = projectedDepth;
    for (const auto& offset : offsets) {
        float sceneDepth = 0.0f;
        if (!ReadDepthAt(screenX + offset[0], screenY + offset[1], sceneDepth)) continue;
        if (sceneDepth <= 0.001f || sceneDepth >= 0.9999f) continue;
        sampled = true;
        if (firstSceneDepth < 0.0f) firstSceneDepth = sceneDepth;
        // The current client renders its main scene with reversed-Z. Detect it
        // from the sampled range so the same code remains valid for normal-Z
        // render targets. A closer wall moves depth in the opposite direction
        // from the target and therefore fails this directional comparison.
        const bool reversedZ = sceneDepth < 0.5f && projectedDepth > 0.5f;
        comparisonTargetDepth = reversedZ ? 1.0f - projectedDepth : projectedDepth;
        constexpr float surfaceTolerance = 0.015f;
        if (reversedZ) {
            if (sceneDepth <= comparisonTargetDepth + surfaceTolerance)
                matched = true;
        } else if (sceneDepth >= comparisonTargetDepth - surfaceTolerance) {
            matched = true;
        }
    }
    if (!sampled) {
        if (logVisibility) {
            FILE* file = nullptr;
            if (fopen_s(&file, Dll6Paths::DataFileA("visibility_runtime.log").c_str(), "a") == 0 && file) {
                fprintf(file, "vis sampled=0 target=%.6f screen=%.1f,%.1f depth=%ux%u\\n",
                        comparisonTargetDepth, screenX, screenY,
                        depthWidth, depthHeight);
                fclose(file);
            }
        }
        // No usable depth sample means the render target is unavailable, not
        // that the target is behind a wall.
        return true;
    }
    const bool visible = matched;
    if (logVisibility) {
        FILE* file = nullptr;
        if (fopen_s(&file, Dll6Paths::DataFileA("visibility_runtime.log").c_str(), "a") == 0 && file) {
            fprintf(file, "vis sampled=1 target=%.6f scene=%.6f matched=%d result=%d screen=%.1f,%.1f depth=%ux%u\\n",
                    comparisonTargetDepth, firstSceneDepth, matched ? 1 : 0,
                    visible ? 1 : 0, screenX, screenY, depthWidth, depthHeight);
            fclose(file);
        }
    }
    return visible;
}

bool GetAimPointScreen(const PlayerData& player, float height, Vector2& screen) {
    if (!currentViewMatrixReady) return false;
    const Vector3 aimPoint{ player.pos.x, player.pos.y, player.pos.z + height };
    return WorldToScreen(aimPoint, screen, currentViewMatrix);
}

void FarmAimAssist(const std::vector<PlayerData>& players) {
    farmNormalActive = false;
    const bool textInputActive = AreCustomBindsSuppressed();
    const bool configuredFarmKeyDown = !textInputActive &&
        (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
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
    const bool farmAiming = !textInputActive &&
        (farmToggleMode ? farmToggleActive : configuredFarmKeyDown);
    if (!currentViewMatrixReady || !farmAiming) {
        ClearPendingCreepAngles();
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float cx = display.x * 0.5f;
    const float cy = display.y * 0.5f;
    FarmTarget best{};
    Vector3 bestPoint{};
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
        if (!target.entity || Read<int>(target.entity + Offsets::Health) <= 0 ||
            Read<uint8_t>(target.entity + Offsets::LifeState) != 0) continue;
        const uintptr_t sceneNode =
            Read<uintptr_t>(target.entity + Offsets::GameSceneNode);
        if (!sceneNode || Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) != 0)
            continue;
        const std::string liveClassName = GetEntityClassName(target.entity);
        if ((liveClassName.find("NPC_Trooper") == std::string::npos &&
             liveClassName.find("C_NPC_Trooper") == std::string::npos) ||
            liveClassName.find("TrooperBoss") != std::string::npos)
            continue;
        const uint8_t liveTeam =
            Read<uint8_t>(target.entity + Offsets::Team);
        if (liveTeam != 2 && liveTeam != 3 && liveTeam != 4) continue;
        if (localTeam != 0 && liveTeam == localTeam) continue;
        // The worker publishes positions asynchronously. Refresh the origin
        // before selecting a target so a recycled/stale snapshot can never
        // turn into an aim point somewhere else in the world.
        Vector3 livePosition{};
        if (!GetEntityPosition(target.entity, livePosition)) continue;
        Vector2 screen{};
        const Vector3 approximatePoint{
            livePosition.x, livePosition.y, livePosition.z + 64.0f };
        if (!WorldToScreen(approximatePoint, screen, currentViewMatrix)) continue;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        const float distance = dx * dx + dy * dy;
        if (distance < farmFov * farmFov) {
            FarmTarget liveTarget = target;
            liveTarget.pos = livePosition;
            liveTarget.team = liveTeam;
            liveTarget.className = liveClassName;
            candidates.push_back({ distance, std::move(liveTarget) });
        }
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const FarmCandidate& left, const FarmCandidate& right) {
            return left.screenDistance < right.screenDistance;
        });

    const bool depthAvailable =
        EnableNativeAimTrace ? clientBase != 0 : depthSnapshotReady;
    for (const auto& candidate : candidates) {
        // Trooper head height is stable across the creep models and is also
        // independent of the optional hero-bone resolver (which is not
        // populated for every NPC_Trooper variant).
        Vector3 point{ candidate.target.pos.x, candidate.target.pos.y,
                       candidate.target.pos.z + 64.0f };
        if (!PrimaryWeaponPointInRange(point)) continue;
        if (aimVisibilityCheck && depthAvailable &&
            !IsWorldAimPointVisible(point, candidate.target.entity)) continue;
        Vector2 screen{};
        if (!WorldToScreen(point, screen, currentViewMatrix)) continue;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        const float distance = dx * dx + dy * dy;
        if (distance >= bestDistance) continue;
        best = candidate.target;
        bestPoint = point;
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

    Vector3 commandAngles{};
    // Publish the exact selected angle for Normal too. The visible camera and
    // the firing command must agree even when another aim helper is enabled.
    if (GetAimAnglesToWorldPoint(bestPoint, commandAngles)) {
        std::lock_guard<std::mutex> lock(creepSilentMutex);
        pendingCreepAngles = commandAngles;
        pendingCreepReady = true;
    } else {
        ClearPendingCreepAngles();
        return;
    }
    if (farmSilentMode || farmMixedMode) {
        if (farmSilentMode)
            return;
    }

    // Normal creep aim uses the same per-frame gameplay-camera path as the
    // current human Normal/Mixed aim. It never moves the OS cursor and uses
    // the same time-based pitch/yaw smoothing in FlushCurrentCameraAim.
    cachedFarmAimPoint = bestPoint;
    farmNormalActive = true;
}

void AutoLastHitOrbs() {
    static bool orbToggleLastDown = false;
    const bool textInputActive = AreCustomBindsSuppressed();
    const bool configuredKeyDown = !textInputActive &&
        (GetAsyncKeyState(autoLastHitOrbsKey) & 0x8000) != 0;
    const bool configuredKeyPressed = configuredKeyDown && !orbToggleLastDown;
    if (autoLastHitOrbs && autoLastHitOrbsToggleMode && configuredKeyPressed) {
        autoLastHitOrbsActive = !autoLastHitOrbsActive;
    }
    orbToggleLastDown = configuredKeyDown;
    if (!autoLastHitOrbs) autoLastHitOrbsActive = false;
    else if (!autoLastHitOrbsToggleMode) autoLastHitOrbsActive = configuredKeyDown;
    const bool orbAimActive = autoLastHitOrbs && autoLastHitOrbsActive &&
        !textInputActive;
    if (!orbAimActive || menuOpen || !currentViewMatrixReady) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAttack = false;
        pendingOrbHoldAttack = false;
        pendingOrbReady = false;
        return;
    }

    constexpr uint32_t kBebopHeroId = 15;
    const uint32_t localHeroId = currentLocalPawn
        ? Read<uint32_t>(currentLocalPawn + Offsets::HeroComponent +
                         Offsets::HeroSpawnedId) : 0;
    const bool bebopWindup = localHeroId == kBebopHeroId &&
        autoLastHitOrbsAutoFire;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float centerX = display.x * 0.5f;
    const float centerY = display.y * 0.5f;
    OrbTarget best{};
    Vector2 bestScreen{};
    float bestDistance = FLT_MAX;
    bool bestAttackable = false;
    static uintptr_t lastOrb = 0;
    static LONG attackBaseline = 0;
    static ULONGLONG attackStarted = 0;
    static ULONGLONG bebopHoldEnds = 0;
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
            if (!IsXpOrbAlive(orb.entity, orb.handle)) continue;
            // The orb is visible immediately after launch, but it has no
            // hitbox until CItemXP.m_flAttackableTime has elapsed.
            const bool attackable = IsXpOrbAttackable(orb.entity, orb.handle);
            if (!attackable && !bebopWindup) continue;
            Vector3 point{};
            // Keep orb aim on the same visual position as ESP and the
            // scanner; this is also the coordinate used for stale detection.
            if (!GetXpOrbPosition(orb.entity, point) &&
                !GetEntityPosition(orb.entity, point)) {
                point = orb.pos;
            }
            if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z)) continue;
            if (!PrimaryWeaponPointInRange(point)) continue;
            if (orbAimVisibilityCheck && (EnableNativeAimTrace
                ? clientBase != 0 : depthSnapshotReady) &&
                !IsWorldAimPointVisible(point, orb.entity)) continue;
            Vector2 screen{};
            if (!WorldToScreen(point, screen, currentViewMatrix)) continue;
            if (screen.x < 0.0f || screen.y < 0.0f ||
                screen.x > display.x || screen.y > display.y) continue;
            const float dx = screen.x - centerX;
            const float dy = screen.y - centerY;
            const float distance = dx * dx + dy * dy;
            if ((attackable && !bestAttackable) ||
                (attackable == bestAttackable && distance < bestDistance)) {
                best = orb;
                best.pos = point;
                bestScreen = screen;
                bestDistance = distance;
                bestAttackable = attackable;
            }
        }
    }
    if (!best.entity) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAttack = false;
        pendingOrbHoldAttack = false;
        pendingOrbReady = false;
        lastOrb = 0;
        lastAttackApplied = 0;
        return;
    }

    Vector3 commandAngles{};
    if (!GetAimAnglesFromScreen(bestScreen.x, bestScreen.y, commandAngles)) {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAttack = false;
        pendingOrbHoldAttack = false;
        pendingOrbReady = false;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (best.entity != lastOrb) {
        lastOrb = best.entity;
        attackBaseline = autoOrbAttackAppliedCount;
        attackStarted = now;
        bebopHoldEnds = bebopWindup ? now + 250 + 360 : 0;
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
    const bool fire = bebopWindup
        ? now - attackStarted >= 250 && now < bebopHoldEnds
        : autoLastHitOrbsAutoFire && attackPulsesApplied < 2 &&
            ((attackPulsesApplied == 0 && now - attackStarted >= firstAttackDelay) ||
             (attackPulsesApplied == 1 && now - lastAttackApplied >= secondAttackDelay)) &&
            now - attackStarted < 800;
    {
        std::lock_guard<std::mutex> lock(orbSilentMutex);
        pendingOrbAngles = commandAngles;
        pendingOrbAttack = fire;
        pendingOrbHoldAttack = bebopWindup && fire;
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
    (void)screenX;
    (void)screenY;
    if constexpr (EnableNativeAimTrace) {
        return QueueAimVisibilityTrace(aimPoint, player.entity);
    }
    return IsDepthPointVisible(aimPoint);
}

bool IsWorldAimPointVisible(const Vector3& point, uintptr_t targetEntity) {
    if constexpr (EnableNativeAimTrace) {
        return QueueAimVisibilityTrace(point, targetEntity);
    }
    return IsDepthPointVisible(point);
}

namespace {

Vector3 cachedVisibleAimPoint{};
bool cachedVisibleAimReady = false;

std::mutex antiFrogStateMutex;
int antiFrogDamageHits = 0;
int antiFrogHeadHits = 0;
float antiFrogHeadshotPercent = 0.0f;
bool antiFrogHeadSlot = true;
bool antiFrogBodySlot = false;
bool antiFrogNeckSlot = false;
int antiFrogSlotRemaining = 0;

bool AntiFrogUsesHeadSlot() {
    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    return antiFrogHeadSlot;
}

bool AntiFrogUsesBodySlot() {
    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    return antiFrogBodySlot;
}

bool RollHitchance() {
    if (aimHitchance >= 99.99f) return true;
    if (aimHitchance <= 0.01f) return false;
    static std::mt19937 generator(std::random_device{}());
    std::uniform_real_distribution<float> distribution(0.0f, 100.0f);
    return distribution(generator) <= std::clamp(aimHitchance, 0.0f, 100.0f);
}

}

void UpdateAimTargetLock(const std::vector<PlayerData>& players) {
    if (!aimLockTarget) {
        aimLockCandidate = 0;
        aimLockedTarget = 0;
        aimLockKeyLastDown = false;
        return;
    }

    // A lock survives leaving the configured FOV and briefly leaving the
    // screen. Only death, invalidation, or changing team releases it.
    if (aimLockedTarget) {
        const int health = Read<int>(aimLockedTarget + Offsets::Health);
        const uint8_t lifeState = Read<uint8_t>(
            aimLockedTarget + Offsets::LifeState);
        const uint8_t targetTeam = Read<uint8_t>(
            aimLockedTarget + Offsets::Team);
        const uint8_t localTeam = currentLocalPawn
            ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
        if (health <= 0 || lifeState != 0 ||
            (localTeam && targetTeam == localTeam)) {
            aimLockedTarget = 0;
        }
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float centerX = display.x * 0.5f;
    const float centerY = display.y * 0.5f;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    float bestDistance = FLT_MAX;
    aimLockCandidate = 0;
    for (const PlayerData& player : players) {
        if (!player.entity || player.health <= 0 ||
            (localTeam && player.team == localTeam)) {
            continue;
        }
        const float screenX = (player.boxLeft + player.boxRight) * 0.5f;
        const float screenY = (player.boxTop + player.boxBottom) * 0.5f;
        if (!std::isfinite(screenX) || !std::isfinite(screenY) ||
            player.boxRight <= player.boxLeft ||
            player.boxBottom <= player.boxTop)
            continue;
        const float dx = screenX - centerX;
        const float dy = screenY - centerY;
        const float distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            aimLockCandidate = player.entity;
        }
    }

    const bool keyDown = !AreCustomBindsSuppressed() && aimLockKey > 0 &&
        (GetAsyncKeyState(aimLockKey) & 0x8000) != 0;
    const bool keyPressed = keyDown && !aimLockKeyLastDown;
    aimLockKeyLastDown = keyDown;
    if (!menuOpen && keyPressed) {
        if (aimLockedTarget)
            aimLockedTarget = 0;
        else if (aimLockCandidate)
            aimLockedTarget = aimLockCandidate;
    }
}

void ResetAntiFrogStats() {
    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    antiFrogDamageHits = 0;
    antiFrogHeadHits = 0;
    antiFrogHeadshotPercent = 0.0f;
    antiFrogHeadSlot = true;
    antiFrogBodySlot = false;
    antiFrogNeckSlot = false;
    antiFrogSlotRemaining = 0;
}

void NotifyAntiFrogDamage(int attackerEntityIndex, int victimEntityIndex,
                          int hitgroupId) {
    if (hitgroupId <= 0 || !currentLocalPawn) return;

    uint32_t localHandle = currentLocalPawnHandle;
    if (localHandle == 0 || localHandle == 0xFFFFFFFFu) {
        localHandle = FindEntityHandle(currentLocalPawn);
        if (localHandle != 0 && localHandle != 0xFFFFFFFFu)
            currentLocalPawnHandle = localHandle;
    }
    if (localHandle == 0 || localHandle == 0xFFFFFFFFu ||
        attackerEntityIndex != static_cast<int>(
            localHandle & Offsets::HandleIndexMask)) {
        return;
    }

    const uintptr_t victim = ResolveEntityIndex(
        static_cast<uint32_t>(victimEntityIndex));
    if (!victim) return;
    const std::string victimClass = GetEntityClassName(victim);
    if (victimClass.find("CitadelPlayerPawn") == std::string::npos)
        return;

    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    ++antiFrogDamageHits;
    if (hitgroupId == 1)
        ++antiFrogHeadHits;
    antiFrogHeadshotPercent = antiFrogDamageHits > 0
        ? static_cast<float>(antiFrogHeadHits) /
            static_cast<float>(antiFrogDamageHits) * 100.0f
        : 0.0f;
    if (!antiFrog || --antiFrogSlotRemaining > 0)
        return;

    const float threshold = std::clamp(antiFrogHsThreshold, 1.0f, 99.0f);
    const float targetRatio = threshold / 100.0f;
    const float currentRatio = antiFrogHeadshotPercent / 100.0f;
    static std::mt19937 generator(std::random_device{}());
    std::uniform_real_distribution<float> probability(0.0f, 1.0f);
    const float roll = probability(generator);
    antiFrogHeadSlot = false;
    antiFrogBodySlot = false;
    antiFrogNeckSlot = false;

    // Use the observed hit ratio as feedback. Below the requested percentage
    // Head is preferred; above it, Neck/Body become more likely. The chance
    // stays bounded so no slot is ever completely forbidden.
    const float correction = (targetRatio - currentRatio) * 1.35f;
    const float headChance = std::clamp(
        targetRatio + correction, 0.08f, 0.92f);
    if (roll < headChance) {
        antiFrogHeadSlot = true;
    } else {
        const float overTarget = (std::max)(
            0.0f, currentRatio - targetRatio);
        const float bodyChance = std::clamp(
            0.20f + overTarget * 1.50f, 0.20f, 0.75f);
        if (probability(generator) < bodyChance)
            antiFrogBodySlot = true;
        else
            antiFrogNeckSlot = true;
    }

    std::uniform_int_distribution<int> headLength(1, 2);
    std::uniform_int_distribution<int> bodyLength(2, 4);
    std::uniform_int_distribution<int> neckLength(2, 4);
    antiFrogSlotRemaining = antiFrogHeadSlot ? headLength(generator)
        : (antiFrogBodySlot ? bodyLength(generator) : neckLength(generator));
}

float GetAntiFrogHeadshotPercent() {
    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    return antiFrogHeadshotPercent;
}

const char* GetAntiFrogSlotLabel() {
    std::lock_guard<std::mutex> lock(antiFrogStateMutex);
    if (antiFrogHeadSlot) return "HEAD";
    return antiFrogBodySlot ? "BODY" : "NECK";
}

void AimAtClosestEnemy(const std::vector<PlayerData>& players) {
    humanAimTargetFound = false;
    const bool textInputActive = AreCustomBindsSuppressed();
    const bool leftButtonDown = !textInputActive &&
        (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rightButtonDown = !textInputActive &&
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    // Treat the two persisted booleans as one mode. Older menu variants and
    // configs could leave both true; in that state the old ternaries made
    // Mixed wait for LMB as if it were pure pSilent and its visible pass died.
    const bool mixedMode = aimMixedMode;
    const bool silentMode = aimSilentMode && !mixedMode;
    const bool silentPass = silentMode || (mixedMode && leftButtonDown);
    aimSilentActive = silentPass;
    if ((!mixedMode && !leftButtonDown) || (!silentMode && !mixedMode))
        ClearPendingSilentAngles();
    const bool configuredAimKeyDown = !textInputActive &&
        (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool configuredAimKeyPressed = configuredAimKeyDown && !aimToggleLastDown;
    if (aimToggleMode && configuredAimKeyPressed) aimToggleActive = !aimToggleActive;
    aimToggleLastDown = configuredAimKeyDown;
    const bool keyActive = aimToggleMode ? aimToggleActive : configuredAimKeyDown;
    aimNormalActive = !silentMode && !mixedMode && keyActive;
    const bool aiming = silentMode ? (keyActive && leftButtonDown) : keyActive;
    if (!aimAssist || textInputActive || !aiming || !currentViewMatrixReady) {
        cachedVisibleAimReady = false;
        ResetNormalMouseAim();
        aimSilentActive = false;
        aimNormalActive = false;
        ClearPendingSilentAngles();
        return;
    }
    // Do not bypass the visibility predicate when the GPU snapshot is not
    // usable. With the option enabled, an unknown depth result is blocked;
    // otherwise the assist silently falls back to aiming through walls.
    const bool useDepthWallCheck = aimVisibilityCheck &&
        (EnableNativeAimTrace ? clientBase != 0 : depthSnapshotReady);
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const PlayerData* best = nullptr;
    Vector3 bestAimPoint{};
    const float fovDistance = aimFov * aimFov;
    float bestSelectionScore = FLT_MAX;
    size_t visibleTargets = 0;
    size_t testedTargets = 0;

    for (const auto& player : players) {
        if (localTeam != 0 && player.team == localTeam) continue;
        const bool forcedTarget = aimLockedTarget &&
            player.entity == aimLockedTarget;
        if (aimLockedTarget && !forcedTarget) continue;
        if (!PrimaryWeaponTargetInRange(player.entity)) continue;
        const float modelHeight = player.modelHeight > 20.0f ? player.modelHeight : 80.0f;
        const float modelMinZ = std::isfinite(player.modelMinZ) ? player.modelMinZ : 0.0f;
        const float fallbackHead = modelMinZ + modelHeight * 0.92f;
        const float fallbackNeck = modelMinZ + modelHeight * 0.82f;
        const float fallbackBody = modelMinZ + modelHeight * 0.62f;
        const float headHeight = ResolveAimHeightFromBox(player, 0.12f, fallbackHead);
        const float neckHeight = ResolveAimHeightFromBox(player, 0.24f, fallbackNeck);
        const float bodyHeight = ResolveAimHeightFromBox(player, 0.60f, fallbackBody);
        const Vector3 fallbackHeadPoint{ player.pos.x, player.pos.y, player.pos.z + headHeight };
        const Vector3 fallbackNeckPoint{ player.pos.x, player.pos.y, player.pos.z + neckHeight };
        const Vector3 fallbackBodyPoint{ player.pos.x, player.pos.y, player.pos.z + bodyHeight };
        struct AimCandidate { Vector3 point; bool bone; };
        AimCandidate candidates[8]{};
        int candidateCount = 0;
        int effectiveMask = aimBonesMask & AimBoneAll;
        if (!effectiveMask)
            effectiveMask = AimBoneHead;
        if (antiFrog) {
            // Adaptive Anti-Frog keeps Head available while the real HS%% is
            // below the configured target. Above target it temporarily uses
            // Neck/Body, then returns to Head as the measured ratio drops.
            // Use the selected slot directly so additional manual bones
            // cannot accidentally turn a Body correction into an arm hit.
            if (AntiFrogUsesHeadSlot())
                effectiveMask = AimBoneHead;
            else if (AntiFrogUsesBodySlot())
                effectiveMask = AimBoneTorso;
            else
                effectiveMask = AimBoneNeck;
        }
        if (effectiveMask & AimBoneHead) {
            candidates[candidateCount++] = { player.hasHeadBone ? player.headPos : fallbackHeadPoint, player.hasHeadBone };
        }
        if (effectiveMask & AimBoneNeck) {
            candidates[candidateCount++] = { player.hasNeckBone ? player.neckPos : fallbackNeckPoint, player.hasNeckBone };
        }
        if (effectiveMask & AimBoneTorso) {
            candidates[candidateCount++] = { player.hasBodyBone ? player.bodyPos : fallbackBodyPoint, player.hasBodyBone };
        }
        if ((effectiveMask & AimBoneArms) && player.hasLeftArmBone)
            candidates[candidateCount++] = { player.leftArmPos, true };
        if ((effectiveMask & AimBoneArms) && player.hasRightArmBone)
            candidates[candidateCount++] = { player.rightArmPos, true };
        if ((effectiveMask & AimBoneLegs) && player.hasLeftLegBone)
            candidates[candidateCount++] = { player.leftLegPos, true };
        if ((effectiveMask & AimBoneLegs) && player.hasRightLegBone)
            candidates[candidateCount++] = { player.rightLegPos, true };
        if (candidateCount == 0)
            continue;
        bool targetVisible = false;
        Vector2 visibleAimScreen{};
        Vector3 visibleAimPoint{};
        float visibleDistance = FLT_MAX;
        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            Vector3 point = candidates[candidateIndex].point;
            if (candidates[candidateIndex].bone && player.hasVisualAnchor) {
                // Bones belong to the interpolated render snapshot, while
                // player.pos is the latest simulation origin. Preserve the
                // rendered pose but move it onto the current pawn origin so
                // aim does not trail a running target by one interpolation
                // interval.
                const Vector3 originDelta{
                    player.pos.x - player.visualAnchor.x,
                    player.pos.y - player.visualAnchor.y,
                    player.pos.z - player.visualAnchor.z};
                const float deltaSquared =
                    originDelta.x * originDelta.x +
                    originDelta.y * originDelta.y +
                    originDelta.z * originDelta.z;
                if (std::isfinite(deltaSquared) &&
                    deltaSquared <= 128.0f * 128.0f) {
                    point.x += originDelta.x;
                    point.y += originDelta.y;
                    point.z += originDelta.z;
                }
            }
            Vector2 aimScreen{};
            const bool projected = GetWorldAimPointScreen(point, aimScreen);
            if (!projected && !forcedTarget) continue;
            float distance = 0.0f;
            if (projected) {
                const float dx = aimScreen.x - cx;
                const float dy = aimScreen.y - cy;
                distance = dx * dx + dy * dy;
            }
            if (!forcedTarget && distance >= fovDistance) continue;
            ++testedTargets;
            if (useDepthWallCheck && (candidates[candidateIndex].bone
                ? !IsWorldAimPointVisible(point, player.entity)
                : !IsAimPointVisible(player, point.z - player.pos.z, aimScreen.x, aimScreen.y))) continue;
            targetVisible = true;
            if (distance < visibleDistance) {
                visibleDistance = distance;
                visibleAimScreen = aimScreen;
                visibleAimPoint = point;
            }
        }
        if (targetVisible) {
            ++visibleTargets;
            float selectionScore = visibleDistance;
            if (aimSelectionMode == AimSelectionMode::Distance)
                selectionScore = player.distance;
            else if (aimSelectionMode == AimSelectionMode::Health)
                selectionScore = static_cast<float>(player.health);
            if (forcedTarget) selectionScore = -FLT_MAX;
            const bool better = forcedTarget || selectionScore < bestSelectionScore;
            if (better && (forcedTarget || aimSelectionMode != AimSelectionMode::Crosshair ||
                           visibleDistance < aimFov * aimFov)) {
                bestSelectionScore = selectionScore;
                best = &player;
                // Store the point that passed the depth test for the final move.
                bestAimPoint = visibleAimPoint;
            }
        }
    }

    if (!best) {
        cachedVisibleAimReady = false;
        ResetNormalMouseAim();
        ClearPendingSilentAngles();
        aimNormalActive = false;
        std::lock_guard<std::mutex> lock(movementDebugTargetMutex);
        movementDebugTargetReady = false;
        return;
    }
    if (!RollHitchance()) {
        ResetNormalMouseAim();
        ClearPendingSilentAngles();
        return;
    }
    if (aimPrediction) {
        // Keep target selection and visibility on the current rendered bone,
        // then lead only the final command/camera point. This avoids making
        // FOV selection jump ahead of a fast target.
        bestAimPoint = PredictPlayerAimPoint(
            best->entity, bestAimPoint, best->worldPos);
    }
    humanAimTargetFound = true;
    {
        std::lock_guard<std::mutex> lock(movementDebugTargetMutex);
        movementDebugTarget = bestAimPoint;
        movementDebugTargetReady = true;
    }

    // Mixed always keeps the current visible Normal camera path. While the
    // attack button is held, pSilent is applied in addition to it.
    const bool visibleMouseAim = aimNormalActive || mixedMode;
    cachedVisibleAimPoint = bestAimPoint;
    cachedVisibleAimReady = visibleMouseAim;
    // Normal aim is applied by UpdateVisibleAimCamera/camera hook. Do not
    // also publish the same target through CreateMove: applying both paths
    // fights over the view angles and makes the camera and ESP oscillate.
    // Mixed keeps the input-angle path only for its silent attack pass.
    // Publish the exact selected angle for both the command-side recoil
    // correction in Normal and the existing pSilent path in Mixed/Silent.
    // Previously Normal only moved the render camera, so the command used to
    // create a bullet could retain the game's upward recoil pitch.
    if (silentPass || visibleMouseAim) {
        Vector3 commandAngles{};
        // A locked pawn may be anywhere around the player. Derive command
        // angles from world coordinates so targets behind the camera do not
        // depend on WorldToScreen and remain valid across the full 360°.
        const bool haveCommandAngles = GetAimAnglesToWorldPoint(
            cachedVisibleAimPoint, commandAngles);
        if (haveCommandAngles) {
            std::lock_guard<std::mutex> lock(humanSilentMutex);
            pendingHumanAngles = commandAngles;
            pendingHumanReady = true;
        }
    }
    if (silentPass) {
        // Pure pSilent intentionally stops here so the visible camera stays still.
        if (silentPass && !mixedMode)
            return;
    }
    // Normal/Mixed keeps the selected world target alive for the gameplay
    // camera hook. UpdateVisibleAimCamera publishes it every render frame and
    // the hook applies smoothing on every native camera update; using OS mouse
    // packets here limited the cadence and produced visible stair-stepping.
    if (visibleMouseAim) {
        ResetNormalMouseAim();
    }
}

void UpdateVisibleAimCamera() {
    const bool humanVisibleAim = cachedVisibleAimReady && aimAssist &&
        (aimNormalActive || aimMixedMode);
    const bool creepVisibleAim = farmNormalActive && farmAssist;
    if ((!humanVisibleAim && !creepVisibleAim) || menuOpen ||
        !currentViewMatrixReady)
        return;
    if (creepVisibleAim) {
        ApplyCurrentCameraAim(cachedFarmAimPoint);
        return;
    }
    const bool instantVisibleAim =
        aimPitchSmooth <= 1.001f && aimYawSmooth <= 1.001f;
    const Vector3 aimPoint = cachedVisibleAimPoint;

    // Bone/render snapshots are not published atomically with the camera
    // snapshot. With low aim smoothing, feeding every tiny snapshot change
    // straight into the camera creates visible micro-oscillation and makes
    // ESP appear to shake on the target. Filter only this measurement noise;
    // the user-selected angular smoothing below still controls responsiveness.
    static Vector3 filteredPoint{};
    static bool filteredPointReady = false;
    static std::chrono::steady_clock::time_point filteredAt{};
    if (instantVisibleAim) {
        // At the minimum slider value Normal must use the same current point
        // as pSilent. The 18 ms measurement filter was the remaining visible
        // trail behind a running head even though camera smoothing was 1.
        filteredPoint = aimPoint;
        filteredPointReady = true;
        filteredAt = std::chrono::steady_clock::now();
        ApplyCurrentCameraAim(aimPoint);
        return;
    }
    const auto filterNow = std::chrono::steady_clock::now();
    float filterDelta = filteredAt.time_since_epoch().count() == 0
        ? (1.0f / 60.0f)
        : std::chrono::duration<float>(filterNow - filteredAt).count();
    filteredAt = filterNow;
    if (!std::isfinite(filterDelta) || filterDelta <= 0.0f ||
        filterDelta > 0.100f)
        filterDelta = 1.0f / 60.0f;
    const float filterDx = aimPoint.x - filteredPoint.x;
    const float filterDy = aimPoint.y - filteredPoint.y;
    const float filterDz = aimPoint.z - filteredPoint.z;
    const float filterDistanceSquared =
        filterDx * filterDx + filterDy * filterDy + filterDz * filterDz;
    // A target switch/teleport must not be dragged through the old target.
    if (!filteredPointReady || !std::isfinite(filterDistanceSquared) ||
        filterDistanceSquared > 256.0f * 256.0f) {
        filteredPoint = aimPoint;
        filteredPointReady = true;
    } else {
        constexpr float filterTimeConstant = 0.018f;
        const float alpha = 1.0f -
            std::exp(-filterDelta / filterTimeConstant);
        filteredPoint.x += filterDx * alpha;
        filteredPoint.y += filterDy * alpha;
        filteredPoint.z += filterDz * alpha;
    }
    ApplyCurrentCameraAim(filteredPoint);
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
