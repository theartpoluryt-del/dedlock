#include "shared.h"
#include <fstream>
#include <shared_mutex>
#include <cctype>
#include <atomic>

namespace {
std::unordered_map<uintptr_t, int> registeredGlowMode;

std::string NormalizeEntityName(const std::string& name) {
    std::string normalized;
    normalized.reserve(name.size());
    for (const unsigned char c : name) {
        if (std::isalnum(c)) normalized.push_back(static_cast<char>(std::tolower(c)));
    }
    return normalized;
}

void LogNativeGlow(const char* message) {
    std::ofstream log(
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\native_glow.log",
        std::ios::app);
    if (log) log << message << '\n';
}

// CItemXP stays in the identity table after its scene object has gone away.
// Do not use its replicated lifetime/health fields for that decision: client
// captures show those fields have the same values for both flying and stale
// entries.  The scene node's dormant bit is the actual visual-state signal.
struct OrbSceneState {
    ULONGLONG lastObservedAt = 0;
    ULONGLONG lastDormantSampleAt = 0;
    uint8_t consecutiveDormantSamples = 0;
    bool hasBeenVisiblyActive = false;
};

bool ShouldPublishXpOrb(uint32_t handle, uintptr_t entity, ULONGLONG now) {
    static std::unordered_map<uint32_t, OrbSceneState> states;
    constexpr ULONGLONG KnownOrbSampleIntervalMs = 16;
    constexpr uint8_t KnownOrbSamplesToRemove = 2;
    constexpr ULONGLONG UnknownOrbSampleIntervalMs = 100;
    constexpr uint8_t UnknownOrbSamplesToRemove = 3;
    constexpr ULONGLONG StateRetentionMs = 30000;

    if ((now & 0x3ff) < 16) {
        for (auto it = states.begin(); it != states.end();) {
            if (now - it->second.lastObservedAt > StateRetentionMs)
                it = states.erase(it);
            else
                ++it;
        }
    }

    OrbSceneState& state = states[handle];
    state.lastObservedAt = now;
    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    // A scene node can briefly be absent while a newly spawned orb is being
    // initialized.  That is not evidence that it has disappeared.
    if (!sceneNode) {
        state.consecutiveDormantSamples = 0;
        return true;
    }

    if (Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) == 0) {
        state.consecutiveDormantSamples = 0;
        state.lastDormantSampleAt = now;
        state.hasBeenVisiblyActive = true;
        return true;
    }

    // An orb that was active in this client session is safe to remove after
    // two adjacent worker passes. Objects already dormant at first sight get
    // a longer grace period so an in-progress spawn cannot blink out.
    const ULONGLONG sampleInterval = state.hasBeenVisiblyActive
        ? KnownOrbSampleIntervalMs : UnknownOrbSampleIntervalMs;
    const uint8_t samplesToRemove = state.hasBeenVisiblyActive
        ? KnownOrbSamplesToRemove : UnknownOrbSamplesToRemove;
    if (now - state.lastDormantSampleAt >= sampleInterval) {
        state.lastDormantSampleAt = now;
        if (state.consecutiveDormantSamples < samplesToRemove)
            ++state.consecutiveDormantSamples;
    }
    return state.consecutiveDormantSamples < samplesToRemove;
}
}

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
    const uint32_t index = handle & Offsets::HandleIndexMask;
    if (!clientBase || index == 0) return 0;
    // IDA confirms the 0x70-byte identity and the complete handle at +0x10.
    // Ability handles can live in one of the auxiliary chunk tables, so try
    // both the direct global and the indirection used by some client builds.
    const uintptr_t roots[] = {
        clientBase + Offsets::GameEntitySystem,
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem)
    };
    const uintptr_t tableOffsets[] = { 0, 0x10, 0x110, 0x100, 0x20 };
    for (const uintptr_t root : roots) {
        if (!root) continue;
        for (const uintptr_t tableOffset : tableOffsets) {
            const uintptr_t chunk = Read<uintptr_t>(
                root + tableOffset + Offsets::EntityChunkStride *
                             (index >> Offsets::HandleChunkShift));
            if (!chunk) continue;
            const uintptr_t identity =
                chunk + Offsets::EntityStride * (index & Offsets::HandleChunkMask);
            if (Read<uint32_t>(identity + Offsets::EntityHandleOffset) != handle) continue;
            return Read<uintptr_t>(identity);
        }
    }
    return 0;
}

void DebugEntityHandle(uint32_t handle) {
    const uint32_t index = handle & Offsets::HandleIndexMask;
    if (!clientBase || index == 0) return;

    const uintptr_t roots[] = {
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem),
        clientBase + Offsets::GameEntitySystem
    };
    const uintptr_t chunkOffsets[] = { 0, 0x10, 0x110, 0x100, 0x20 };
    printf("[Parry] resolve handle=0x%X index=0x%X\n", handle, index);
    for (size_t rootIndex = 0; rootIndex < sizeof(roots) / sizeof(roots[0]); ++rootIndex) {
        const uintptr_t root = roots[rootIndex];
        if (!root) continue;
        for (const uintptr_t tableOffset : chunkOffsets) {
            const uintptr_t chunk = Read<uintptr_t>(
                root + tableOffset + Offsets::EntityChunkStride * (index >> Offsets::HandleChunkShift));
            if (!chunk) continue;
            const uintptr_t identity = chunk + Offsets::EntityStride * (index & Offsets::HandleChunkMask);
            const uint32_t storedHandle = Read<uint32_t>(identity + Offsets::EntityHandleOffset);
            const uintptr_t entity = Read<uintptr_t>(identity);
            printf("[Parry] table root=%zu off=0x%llX chunk=0x%p identity=0x%p stored=0x%X entity=0x%p class=%s\n",
                   rootIndex, static_cast<unsigned long long>(tableOffset),
                   reinterpret_cast<void*>(chunk), reinterpret_cast<void*>(identity), storedHandle,
                   reinterpret_cast<void*>(entity), GetEntityClassName(entity).c_str());
        }
    }
}

uint32_t FindEntityHandle(uintptr_t target) {
    if (!target || !clientBase) return 0xFFFFFFFFu;
    const uintptr_t roots[] = {
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem),
        clientBase + Offsets::GameEntitySystem
    };
    const uintptr_t chunkOffsets[] = { 0, 0x10, 0x110, 0x100, 0x20 };
    for (const uintptr_t root : roots) {
        if (!root) continue;
        for (const uintptr_t chunkOffset : chunkOffsets) {
            for (uint32_t chunkIndex = 0; chunkIndex <= (Offsets::MaxEntityIndex >> Offsets::HandleChunkShift); ++chunkIndex) {
                const uintptr_t chunk = Read<uintptr_t>(
                    root + chunkOffset + Offsets::EntityChunkStride * chunkIndex);
                if (!chunk) continue;
                for (uint32_t slot = 0; slot <= Offsets::HandleChunkMask; ++slot) {
                    const uintptr_t identity = chunk + Offsets::EntityStride * slot;
                    if (Read<uintptr_t>(identity) != target) continue;
                    constexpr uintptr_t handleOffsets[] = { 0x08, 0x10, 0x18, 0x20 };
                    for (const uintptr_t handleOffset : handleOffsets) {
                        const uint32_t handle = Read<uint32_t>(identity + handleOffset);
                        if (handle == 0xFFFFFFFFu || (handle & Offsets::HandleIndexMask) == 0) continue;
                        if (ResolveEntity(handle) == target) return handle;
                    }
                }
            }
        }
    }
    return 0xFFFFFFFFu;
}

bool GetEntityPosition(uintptr_t entity, Vector3& position) {
    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (sceneNode) {
        position = Read<Vector3>(sceneNode + Offsets::SceneNodeAbsOrigin);
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::fabs(position.x) < 100000.0f && std::fabs(position.y) < 100000.0f &&
            std::fabs(position.z) < 100000.0f &&
            (std::fabs(position.x) > 0.01f || std::fabs(position.y) > 0.01f ||
             std::fabs(position.z) > 0.01f)) {
            return true;
        }
    }
    // CItemXP can be networked before its render scene node is created.
    position = Read<Vector3>(entity + Offsets::Pos);
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
           (std::fabs(position.x) > 0.01f || std::fabs(position.y) > 0.01f || std::fabs(position.z) > 0.01f);
}

bool GetEntityRenderPosition(uintptr_t entity, Vector3& position) {
    const uintptr_t sceneNode =
        Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (sceneNode) {
        position =
            Read<Vector3>(sceneNode + Offsets::SceneNodeRenderOrigin);
        if (std::isfinite(position.x) && std::isfinite(position.y) &&
            std::isfinite(position.z) &&
            std::fabs(position.x) < 100000.0f &&
            std::fabs(position.y) < 100000.0f &&
            std::fabs(position.z) < 100000.0f &&
            (std::fabs(position.x) > 0.01f ||
             std::fabs(position.y) > 0.01f ||
             std::fabs(position.z) > 0.01f)) {
            return true;
        }
    }
    return GetEntityPosition(entity, position);
}

bool GetEntityRenderTransformPosition(uintptr_t entity, Vector3& position) {
    position = {};
    if (!entity) return false;
    const uintptr_t sceneNode =
        Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (!sceneNode) return false;

    // CGameSceneNode::m_nodeToWorld starts at +0x10 in the current layout;
    // its CTransform position is the transform consumed by scene rendering.
    // Unlike m_vRenderOrigin, this field is populated for hero pawns (the
    // latter is FLT_MAX for Training Dummy in the current client).
    constexpr uintptr_t NodeToWorldPosition = 0x10;
    const uintptr_t address = sceneNode + NodeToWorldPosition;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Vector3 first = Read<Vector3>(address);
        const Vector3 second = Read<Vector3>(address);
        if (std::memcmp(&first, &second, sizeof(first)) != 0) continue;
        position = second;
        return std::isfinite(position.x) &&
               std::isfinite(position.y) &&
               std::isfinite(position.z) &&
               std::fabs(position.x) < 100000.0f &&
               std::fabs(position.y) < 100000.0f &&
               std::fabs(position.z) < 100000.0f &&
               (std::fabs(position.x) > 0.01f ||
                std::fabs(position.y) > 0.01f ||
                std::fabs(position.z) > 0.01f);
    }
    position = {};
    return false;
}

bool GetXpOrbPosition(uintptr_t entity, Vector3& position) {
    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
    if (sceneNode) {
        // RenderOrigin is the position used by the visual orb while it is
        // travelling; AbsOrigin/network Pos can lag until the next update.
        position = Read<Vector3>(sceneNode + Offsets::SceneNodeRenderOrigin);
        if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
            std::fabs(position.x) < 100000.0f && std::fabs(position.y) < 100000.0f &&
            std::fabs(position.z) < 100000.0f &&
            (std::fabs(position.x) > 0.01f || std::fabs(position.y) > 0.01f || std::fabs(position.z) > 0.01f)) {
            return true;
        }
    }
    // Freshly spawned CItemXP instances can be rendered from nodeToWorld
    // before both RenderOrigin and the replicated entity position arrive.
    // Use that transform before treating the orb as positionless.
    if (GetEntityRenderTransformPosition(entity, position)) return true;
    position = Read<Vector3>(entity + Offsets::Pos);
    if (std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z) &&
        std::fabs(position.x) < 100000.0f && std::fabs(position.y) < 100000.0f &&
        std::fabs(position.z) < 100000.0f &&
        (std::fabs(position.x) > 0.01f || std::fabs(position.y) > 0.01f || std::fabs(position.z) > 0.01f)) {
        return true;
    }
    return GetEntityPosition(entity, position);
}

bool GetEntityScreenBounds(uintptr_t entity, const Vector3& origin, const Matrix4x4& matrix,
                           float& left, float& top, float& right, float& bottom) {
    if (!entity) return false;

    const uintptr_t collision = Read<uintptr_t>(entity + Offsets::CollisionProperty);
    if (!collision) return false;
    const Vector3 mins = Read<Vector3>(collision + Offsets::CollisionMins);
    const Vector3 maxs = Read<Vector3>(collision + Offsets::CollisionMaxs);
    if (!std::isfinite(mins.x) || !std::isfinite(mins.y) || !std::isfinite(mins.z) ||
        !std::isfinite(maxs.x) || !std::isfinite(maxs.y) || !std::isfinite(maxs.z) ||
        mins.x >= maxs.x || mins.y >= maxs.y || mins.z >= maxs.z ||
        maxs.x - mins.x > 300.0f || maxs.y - mins.y > 300.0f || maxs.z - mins.z > 400.0f) {
        return false;
    }

    left = top = FLT_MAX;
    right = bottom = -FLT_MAX;
    bool projected = false;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                const Vector3 corner{
                    origin.x + (x ? maxs.x : mins.x),
                    origin.y + (y ? maxs.y : mins.y),
                    origin.z + (z ? maxs.z : mins.z)
                };
                Vector2 screen{};
                if (!WorldToScreen(corner, screen, matrix)) continue;
                projected = true;
                left = (std::min)(left, screen.x);
                right = (std::max)(right, screen.x);
                top = (std::min)(top, screen.y);
                bottom = (std::max)(bottom, screen.y);
            }
        }
    }
    if (!projected || !std::isfinite(left) || !std::isfinite(top) ||
        !std::isfinite(right) || !std::isfinite(bottom)) return false;
    const float height = bottom - top;
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const float width = right - left;
    const float maxCoordinate = (std::max)(displaySize.x, displaySize.y) * 4.0f;
    if (height < 4.0f || width < 2.0f ||
        height > displaySize.y * 1.5f || width > displaySize.x * 1.5f ||
        std::fabs(left) > maxCoordinate || std::fabs(right) > maxCoordinate ||
        std::fabs(top) > maxCoordinate || std::fabs(bottom) > maxCoordinate) return false;

    // The collision capsule stops short of the head and feet on several hero
    // models. Keep the projected horizontal bounds, but compensate vertically
    // for the render model rather than drawing a box around only the capsule.
    top -= height * 0.30f;
    bottom += height * 0.08f;
    const float margin = (right - left) * 0.08f;
    left -= margin;
    right += margin;
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
           std::isfinite(bottom) && right > left && bottom > top &&
           right - left <= displaySize.x * 1.5f &&
           bottom - top <= displaySize.y * 1.5f;
}

std::string GetEntityClassName(uintptr_t entity) {
    if (!entity || !clientBase) return {};
    const uintptr_t vtable = Read<uintptr_t>(entity);
    if (!vtable) return {};
    static std::unordered_map<uintptr_t, std::string> classCache;
    static std::shared_mutex classCacheMutex;
    {
        std::shared_lock<std::shared_mutex> lock(classCacheMutex);
        const auto cached = classCache.find(vtable);
        if (cached != classCache.end()) return cached->second;
    }
    const uintptr_t objectLocator = Read<uintptr_t>(vtable - sizeof(uintptr_t));
    if (!objectLocator) return {};
    const uint32_t typeDescriptorRva = Read<uint32_t>(objectLocator + 0x0C);
    if (typeDescriptorRva >= 0x10000000u) return {};
    const uintptr_t typeDescriptor = clientBase + typeDescriptorRva;
    std::string typeName;
    typeName.reserve(96);
    for (uintptr_t index = 0; index < 96; ++index) {
        const char character = Read<char>(typeDescriptor + 0x10 + index);
        if (character == '\0') break;
        typeName.push_back(character);
    }
    {
        std::unique_lock<std::shared_mutex> lock(classCacheMutex);
        if (classCache.size() < 4096) classCache.emplace(vtable, typeName);
    }
    return typeName;
}

namespace {
bool NotifyNativeGlowRegistration(uintptr_t glow, uintptr_t entity, bool isTrooper) {
    __try {
        // The fixed callback moved with the client build and currently
        // raises an exception. Use the validated native wrapper discovered
        // from the live client image instead.
        Write<bool>(glow + Offsets::GlowEligible, true);
        Write<bool>(glow + Offsets::IsGlowing, true);
        const bool registered = isTrooper
            ? RegisterNativeTrooperGlow(entity)
            : RegisterNativeGlow(entity);
        if (!registered) {
            LogNativeGlow(isTrooper ? "native trooper wrapper failed"
                                   : "native player wrapper failed");
            return false;
        }
        LogNativeGlow(isTrooper ? "native trooper wrapper ok"
                                : "native player wrapper ok");
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogNativeGlow("native wrapper exception");
        return false;
    }
}
}

bool NotifyGlowTypeChanged(uintptr_t glow) {
    if (!glow) return false;
    const uintptr_t entity = glow - Offsets::Glow;
    const std::string className = GetEntityClassName(entity);
    const bool isTrooper = className.find("NPC_Trooper") != std::string::npos;
    return NotifyNativeGlowRegistration(glow, entity, isTrooper);
}

void ApplyHeroGlow(uintptr_t entity) {
    static std::atomic_bool firstGlowLogged = false;
    if (!firstGlowLogged.exchange(true)) {
        LogNativeGlow("ApplyHeroGlow reached");
    }
    const uintptr_t glow = entity + Offsets::Glow;
    bool shouldNotify = false;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const uint8_t entityTeam = Read<uint8_t>(entity + Offsets::Team);
    const bool ally = localTeam >= 2 && localTeam <= 3 && entityTeam == localTeam;
    const int teamGlowMode = ally ? allyGlowMode : enemyGlowMode;

    // The client can reset m_iGlowType after a network update even though the
    // property object remains alive. Re-register whenever the complete-model
    // glow pass is no longer active.
    {
        std::lock_guard lock(glowMutex);
        // Keep CGlowProperty in sync with the outline-manager contract:
        // type 1 is health-clipped, type 2 is the complete model fill.
        const int targetGlowType = teamGlowMode == 1 ? 2 : 1;
        const int currentType = Read<int>(glow + Offsets::GlowType);
        const auto modeIt = registeredGlowMode.find(entity);
        const bool modeChanged = modeIt == registeredGlowMode.end() ||
            modeIt->second != teamGlowMode;
        if ((currentType != targetGlowType || modeChanged) &&
            queuedGlows.insert(entity).second) {
            shouldNotify = true;
        }
        if (shouldNotify) registeredGlowMode[entity] = teamGlowMode;
    }

    const float* glowColor = ally ? teammateGlowColor : enemyGlowColor;
    const int health = Read<int>(entity + Offsets::Health);
    const int maxHealth = Read<int>(entity + Offsets::MaxHealth);
    const float healthAlpha = maxHealth > 0
        ? std::clamp(static_cast<float>(health) / maxHealth, 0.0f, 1.0f) : 0.0f;
    const float glowAlpha = teamGlowMode == 0
        ? glowColor[3] * healthAlpha : 1.0f;
    Write<Vector3>(glow + Offsets::GlowColor,
                   { glowColor[0], glowColor[1], glowColor[2] });
    Write<int>(glow + Offsets::GlowType, teamGlowMode == 1 ? 2 : 1);
    Write<int>(glow + Offsets::GlowTeam, -1);
    Write<int>(glow + Offsets::GlowRange, 0);
    Write<int>(glow + Offsets::GlowRangeMin, 0);
    Write<ColorRGBA>(glow + Offsets::GlowColorOverride,
                     { static_cast<uint8_t>(std::clamp(glowColor[0], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowColor[1], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowColor[2], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowAlpha, 0.0f, 1.0f) * 255.0f) });
    Write<bool>(glow + Offsets::GlowFlashing, false);
    Write<float>(glow + Offsets::GlowTime, teamGlowMode == 1 ? 0.0f : 1.0f);
    Write<float>(glow + Offsets::GlowStartTime, 0.0f);
    Write<bool>(glow + Offsets::GlowEligible, true);
    Write<bool>(glow + Offsets::IsGlowing, true);
    Write<float>(entity + Offsets::GlowBackfaceMult, 1.0f);

    // Register only after the property has its final values. The native
    // wrapper snapshots the current CGlowProperty state when it adds the
    // entity to the renderer's highlight list.
    if (nativeGlowReady && shouldNotify) {
        const bool registered = RegisterNativeGlow(entity);
        if (registered) LogNativeGlow("worker native wrapper ok");
        else LogNativeGlow("worker native wrapper failed");
    }


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

void ApplyTrooperGlow(uintptr_t entity) {
    if (!entity) return;
    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const uint8_t entityTeam = Read<uint8_t>(entity + Offsets::Team);
    const bool neutral = entityTeam == 4;
    const bool ally = !neutral && localTeam >= 2 && localTeam <= 3 && entityTeam == localTeam;
    const bool enabled = neutral ? neutralChams
        : (ally ? allyTrooperChams : enemyTrooperChams);
    const float* color = neutral ? neutralChamsColor
        : (ally ? allyTrooperChamsColor : enemyTrooperChamsColor);
    const uintptr_t glow = entity + Offsets::Glow;

    const auto clearTrooperGlow = [&] {
        Write<bool>(glow + Offsets::IsGlowing, false);
        Write<bool>(glow + Offsets::GlowEligible, false);
        Write<int>(glow + Offsets::GlowType, 0);
        std::lock_guard lock(glowMutex);
        registeredGlows.erase(entity);
        queuedGlows.erase(entity);
    };

    if (!enabled) {
        clearTrooperGlow();
        return;
    }

    Vector3 position{};
    if (currentLocalPositionReady && GetEntityPosition(entity, position)) {
        const float dx = position.x - currentLocalPosition.x;
        const float dy = position.y - currentLocalPosition.y;
        const float dz = position.z - currentLocalPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
        if (!std::isfinite(distance) || distance > creepEspMaxDistance) {
            clearTrooperGlow();
            return;
        }
    }

    Write<Vector3>(glow + Offsets::GlowColor, {color[0], color[1], color[2]});
    Write<int>(glow + Offsets::GlowType, 2);
    Write<int>(glow + Offsets::GlowTeam, -1);
    Write<int>(glow + Offsets::GlowRange, 0);
    Write<int>(glow + Offsets::GlowRangeMin, 0);
    Write<ColorRGBA>(glow + Offsets::GlowColorOverride,
                     {static_cast<uint8_t>(std::clamp(color[0], 0.0f, 1.0f) * 255.0f),
                      static_cast<uint8_t>(std::clamp(color[1], 0.0f, 1.0f) * 255.0f),
                      static_cast<uint8_t>(std::clamp(color[2], 0.0f, 1.0f) * 255.0f),
                      static_cast<uint8_t>(std::clamp(color[3], 0.0f, 1.0f) * 255.0f)});
    Write<bool>(glow + Offsets::GlowFlashing, false);
    Write<float>(glow + Offsets::GlowTime, 0.0f);
    Write<bool>(glow + Offsets::GlowEligible, true);
    Write<bool>(glow + Offsets::IsGlowing, true);
    Write<float>(entity + Offsets::GlowBackfaceMult, 1.0f);

    bool shouldNotify = false;
    {
        std::lock_guard lock(glowMutex);
        // An NPC is removed from this set whenever its chams channel or its
        // distance condition becomes false. A later enable must therefore
        // invoke the engine's generic NPC registration again.
        if (registeredGlows.find(entity) == registeredGlows.end() &&
            queuedGlows.insert(entity).second) {
            shouldNotify = true;
        }
    }
    if (shouldNotify && gameWindow && oWndProc) {
        // Registration modifies the engine-owned highlight list, so it has
        // to execute on the game window thread rather than this scan worker.
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
void RefreshHeroPawns() {
    std::vector<uintptr_t> found;

    // The old implementation walked every committed private page in the
    // process.  That is effectively an unbounded heap scan and is the reason
    // the menu could remain on "searching" for minutes.  Heroes are already
    // registered in CEntitySystem, so enumerate its 0x70-byte identities.
    if (clientBase) {
        const uintptr_t roots[] = {
            Read<uintptr_t>(clientBase + Offsets::GameEntitySystem),
            clientBase + Offsets::GameEntitySystem
        };
        const uintptr_t tableOffsets[] = { Offsets::EntityChunks, 0, 0x110, 0x100, 0x20 };
        std::unordered_set<uintptr_t> seen;
        const bool haveHeroVTables = !heroVTables.empty();

        for (const uintptr_t root : roots) {
            if (!root || WaitForSingleObject(stopHeroDiscoveryEvent, 0) == WAIT_OBJECT_0) break;
            for (const uintptr_t tableOffset : tableOffsets) {
                for (uint32_t chunkIndex = 0;
                     chunkIndex <= (Offsets::MaxEntityIndex >> Offsets::HandleChunkShift);
                     ++chunkIndex) {
                    if (WaitForSingleObject(stopHeroDiscoveryEvent, 0) == WAIT_OBJECT_0) break;
                    const uintptr_t chunk = Read<uintptr_t>(
                        root + tableOffset + Offsets::EntityChunkStride * chunkIndex);
                    if (!chunk) continue;

                    for (uint32_t slot = 0; slot <= Offsets::HandleChunkMask; ++slot) {
                        const uintptr_t identity = chunk + Offsets::EntityStride * slot;
                        const uintptr_t entity = Read<uintptr_t>(identity);
                        if (!entity || !seen.insert(entity).second) continue;

                        bool heroVTableMatch = false;
                        if (haveHeroVTables) {
                            const uintptr_t vtable = Read<uintptr_t>(entity);
                            heroVTableMatch =
                                std::find(heroVTables.begin(), heroVTables.end(),
                                          vtable) != heroVTables.end();
                            if (!heroVTableMatch) continue;
                        }

                        const int health = Read<int>(entity + Offsets::Health);
                        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
                        if (health < 0 || health > 10000 || (team != 2 && team != 3)) continue;
                        // A discovered hero vtable is already an exact RTTI
                        // match. Do not reject it because a second RTTI walk
                        // transiently fails while modules/entities initialize.
                        if (!heroVTableMatch &&
                            GetEntityClassName(entity).find("CitadelPlayerPawn") ==
                                std::string::npos)
                            continue;

                        // Entity-system membership, hero type, team and health
                        // identify the pawn. Do not require a scene-node
                        // position here: Source 2 may stop publishing render
                        // transforms for objects outside the current camera.
                        found.push_back(entity);
                    }
                }
            }
        }
    }

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    const size_t pawnCount = found.size();
    // Do not publish a transient empty scan. During entity-table updates the
    // renderer can briefly expose no valid positions; replacing the live list
    // in that window makes the native outline disappear for a frame.
    if (!found.empty()) {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        heroPawns = std::move(found);
    }
    static size_t lastPawnCount = SIZE_MAX;
    if (pawnCount != lastPawnCount) {
        lastPawnCount = pawnCount;
        printf("[+] Hero pawns: %zu\n", lastPawnCount);
    }
    espStatus.heroScanComplete = true;
}

uintptr_t ResolveEntityIndex(uint32_t entityIndex) {
    const uint32_t index = entityIndex & Offsets::HandleIndexMask;
    if (!clientBase || index == 0) return 0;

    const uintptr_t roots[] = {
        clientBase + Offsets::GameEntitySystem,
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem)
    };
    const uintptr_t tableOffsets[] = { 0, 0x10, 0x110, 0x100, 0x20 };
    for (const uintptr_t root : roots) {
        if (!root) continue;
        for (const uintptr_t tableOffset : tableOffsets) {
            const uintptr_t chunk = Read<uintptr_t>(
                root + tableOffset + Offsets::EntityChunkStride *
                             (index >> Offsets::HandleChunkShift));
            if (!chunk) continue;
            const uintptr_t identity = chunk + Offsets::EntityStride *
                (index & Offsets::HandleChunkMask);
            const uintptr_t entity = Read<uintptr_t>(identity);
            if (entity) return entity;
        }
    }
    return 0;
}

float GetClientGameTime() {
    static uintptr_t globalVars = 0;
    static bool searched = false;
    if (!searched && clientBase) {
        searched = true;
        MODULEINFO moduleInfo{};
        if (GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase),
                                 &moduleInfo, sizeof(moduleInfo))) {
            const auto* image = reinterpret_cast<const uint8_t*>(clientBase);
            const size_t size = moduleInfo.SizeOfImage;
            for (size_t i = 0; i + 31 < size; ++i) {
                if (image[i] != 0x48 || image[i + 1] != 0x8B || image[i + 2] != 0x05 ||
                    image[i + 7] != 0x44 || image[i + 8] != 0x8B || image[i + 9] != 0xB7 ||
                    image[i + 10] != 0xDC || image[i + 11] != 0x00 || image[i + 12] != 0x00 ||
                    image[i + 13] != 0x00) continue;
                int32_t displacement{};
                memcpy(&displacement, image + i + 3, sizeof(displacement));
                const uintptr_t pointerAddress = clientBase + i + 7 + displacement;
                globalVars = Read<uintptr_t>(pointerAddress);
                if (globalVars) break;
            }
        }
    }
    return globalVars ? Read<float>(globalVars + 0x0C) : 0.0f;
}

bool IsXpOrbAttackable(uintptr_t entity, uint32_t handle) {
    if (!entity) return false;

    struct OrbObservation {
        ULONGLONG firstSeen{};
        ULONGLONG lastSeen{};
        bool attackable{};
    };
    static std::unordered_map<uintptr_t, OrbObservation> observations;
    static ULONGLONG lastObservationCleanup = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastObservationCleanup >= 5000) {
        lastObservationCleanup = now;
        for (auto it = observations.begin(); it != observations.end();) {
            if (it->second.lastSeen && now - it->second.lastSeen > 5000)
                it = observations.erase(it);
            else
                ++it;
        }
    }
    const uintptr_t observationKey = handle != 0 && handle != 0xFFFFFFFFu
        ? static_cast<uintptr_t>(handle) : entity;
    auto& observation = observations[observationKey];
    if (observation.lastSeen != 0 && now - observation.lastSeen > 1000) {
        observation = {};
    }
    observation.lastSeen = now;
    if (observation.firstSeen == 0) observation.firstSeen = now;
    if (observation.attackable) return true;

    // The replicated attackable-time field is not reliable for every orb
    // spawn: it can contain a plausible but stale value. The game transition
    // is one-way, so latch the second stage after the observed launch delay
    // instead of allowing that field to keep the orb in stage one forever.
    constexpr ULONGLONG kOrbAttackableDelayMs = 600;
    if (now - observation.firstSeen >= kOrbAttackableDelayMs) {
        observation.attackable = true;
        return true;
    }

    // The orb starts non-attackable and transitions to attackable once. It
    // remains attackable until the entity is removed, so do not use the
    // neighbouring 0x0A10 field as an end time.
    const float attackableTime = Read<float>(entity + 0x0A0C);
    const float gameTime = GetClientGameTime();
    const auto fallbackAttackable = [&]() {
        observation.attackable = now - observation.firstSeen >= 600;
        return observation.attackable;
    };
    if (!std::isfinite(gameTime) || gameTime <= 0.0f) {
        // Some client sessions do not expose GlobalVars through the pattern
        // used above. In that case retain only the launch observation
        // fallback; never latch attackability indefinitely.
        return fallbackAttackable();
    }
    if (!std::isfinite(attackableTime) || attackableTime <= 0.0f) {
        return fallbackAttackable();
    }
    // A value more than two minutes ahead is not a valid launch timestamp;
    // it is an unresolved/unused field in this client build.
    if (attackableTime > gameTime + 120.0f) {
        return fallbackAttackable();
    }
    observation.attackable = gameTime >= attackableTime;
    return observation.attackable;
}

std::string GetEntityDesignerName(uint32_t handle) {
    if (!clientBase || handle == 0xFFFFFFFFu) return {};
    const uint32_t index = handle & Offsets::HandleIndexMask;
    const uintptr_t roots[] = {
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem),
        clientBase + Offsets::GameEntitySystem
    };
    const uintptr_t tableOffsets[] = { 0, Offsets::EntityChunks, 0x110, 0x100, 0x20 };
    for (const uintptr_t root : roots) {
        if (!root) continue;
        for (const uintptr_t tableOffset : tableOffsets) {
            const uintptr_t chunk = Read<uintptr_t>(root + tableOffset +
                Offsets::EntityChunkStride * (index >> Offsets::HandleChunkShift));
            if (!chunk) continue;
            const uintptr_t identity = chunk + Offsets::EntityStride *
                (index & Offsets::HandleChunkMask);
            if (Read<uint32_t>(identity + Offsets::EntityHandleOffset) != handle) continue;
            const uintptr_t name = Read<uintptr_t>(identity + 0x20);
            if (!name) return {};
            std::string result;
            for (size_t i = 0; i < 64; ++i) {
                const char c = Read<char>(name + i);
                if (!c) break;
                result.push_back(c);
            }
            return result;
        }
    }
    return {};
}

std::string ReadIdentityDesignerName(uintptr_t identity) {
    if (!identity) return {};
    const uintptr_t stringPointer = Read<uintptr_t>(identity + 0x20);
    std::string result;
    if (stringPointer) {
        for (size_t i = 0; i < 64; ++i) {
            const char c = Read<char>(stringPointer + i);
            if (!c) break;
            if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                result.clear();
                break;
            }
            result.push_back(c);
        }
        if (!result.empty()) return result;
    }
    // Support builds where the designer name is stored inline in the identity.
    for (size_t i = 0; i < 64; ++i) {
        const char c = Read<char>(identity + 0x20 + i);
        if (!c) break;
        if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) return {};
        result.push_back(c);
    }
    return result;
}

bool IsSoulDesignerName(const std::string& name) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find("soul") != std::string::npos &&
           lower.find("spawner") == std::string::npos;
}

bool IsExpiredXpOrb(uintptr_t entity) {
    if (!entity) return true;
    const float attackableTime = Read<float>(entity + 0x0A0C);
    const float endAttackableTime = Read<float>(entity + 0x0A10);
    const float gameTime = GetClientGameTime();
    if (!std::isfinite(attackableTime) || attackableTime <= 0.0f ||
        !std::isfinite(endAttackableTime) || endAttackableTime <= 0.0f ||
        !std::isfinite(gameTime) || gameTime <= 0.0f) return false;
    const float lifetime = endAttackableTime - attackableTime;
    if (lifetime < 0.05f || lifetime > 30.0f ||
        attackableTime > gameTime + 120.0f ||
        endAttackableTime > gameTime + 120.0f) return false;
    return gameTime >= endAttackableTime;
}

bool IsXpOrbAlive(uintptr_t entity, uint32_t handle) {
    if (!entity || handle == 0 || handle == 0xFFFFFFFFu ||
        ResolveEntity(handle) != entity || IsExpiredXpOrb(entity)) return false;
    std::string className = GetEntityClassName(entity);
    std::transform(className.begin(), className.end(), className.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (className.find("itemxp") != std::string::npos) return true;
    return IsSoulDesignerName(GetEntityDesignerName(handle));
}

bool NotifyOrbEntityAdded(uint32_t handle) {
    const uintptr_t entity = ResolveEntity(handle);
    const std::string designerName = GetEntityDesignerName(handle);
    if (!entity) return false;
    const std::string className = GetEntityClassName(entity);
    if (className.empty() && !IsSoulDesignerName(designerName)) return false;
    if (NormalizeEntityName(className).find("itemxp") == std::string::npos &&
        !IsSoulDesignerName(designerName)) return true;
    Vector3 position{};
    GetXpOrbPosition(entity, position);
    std::lock_guard<std::mutex> lock(orbTargetsMutex);
    for (const auto& orb : orbTargets) {
        if (orb.handle == handle || (orb.entity && orb.entity == entity)) return true;
    }
    orbTargets.push_back({ entity, position, designerName.empty() ? className : designerName, handle,
                           Read<uint8_t>(entity + Offsets::Team) });
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_runtime.log", std::ios::app);
    if (log) log << "event add class=" << className << " designer=" << designerName << " handle=0x"
                << std::hex << handle << std::dec << "\n";
    return true;
}

void NotifyOrbEntityRemoved(uint32_t handle) {
    std::lock_guard<std::mutex> lock(orbTargetsMutex);
    orbTargets.erase(std::remove_if(orbTargets.begin(), orbTargets.end(),
        [handle](const OrbTarget& orb) {
            return orb.handle == handle ||
                (orb.handle != 0 &&
                 (orb.handle & Offsets::HandleIndexMask) ==
                 (handle & Offsets::HandleIndexMask));
        }), orbTargets.end());
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_runtime.log", std::ios::app);
    if (log) log << "event remove handle=0x" << std::hex << handle << std::dec << "\n";
}

namespace {
std::mutex pendingOrbEventsMutex;
struct PendingOrbEvent { uint32_t handle{}; bool added{}; };
std::vector<PendingOrbEvent> pendingOrbEvents;
}

void QueueOrbEntityAdded(uint32_t handle) {
    std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
    pendingOrbEvents.push_back({handle, true});
}

void QueueOrbEntityRemoved(uint32_t handle) {
    std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
    pendingOrbEvents.push_back({handle, false});
}

void RefreshFarmTargets() {
    static ULONGLONG lastRefresh = 0;
    static bool trooperChamsWereActive = false;
    const ULONGLONG now = GetTickCount64();
    const bool configuredFarmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool farmActive = farmAssist &&
        (farmToggleMode ? farmToggleActive : configuredFarmKeyDown);
    const bool trooperChamsActive = enemyTrooperChams || allyTrooperChams || neutralChams;
    const bool worldEntityScanActive = powerupEspEnabled || talonEspEnabled || campTimersEnabled;
    // `drawCreepEsp` is used by the established Aim-page control.  Keep it
    // wired to the scanner alongside the newer per-team Visuals controls;
    // otherwise that control reports enabled while the worker clears every
    // target before Creep ESP/Aim can consume it.
    if (!farmActive && !drawCreepEsp && !creepEspEnabled && !allyCreepEspEnabled && !neutralCreepEspEnabled &&
        !autoLastHitOrbs && !drawOrbEsp && !trooperChamsActive &&
        !trooperChamsWereActive && !worldEntityScanActive) {
        {
            std::lock_guard<std::mutex> eventLock(pendingOrbEventsMutex);
            pendingOrbEvents.clear();
        }
        std::lock_guard lock(farmTargetsMutex);
        farmTargets.clear();
        std::lock_guard orbLock(orbTargetsMutex);
        orbTargets.clear();
        std::lock_guard worldLock(worldEspTargetsMutex);
        worldEspTargets.clear();
        return;
    }
    // Flying souls can exist for less than a second.  A one-second scan
    // interval misses them entirely when ESP is enabled without auto-hit.
    std::vector<PendingOrbEvent> lifecycleEvents;
    {
        std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
        lifecycleEvents.swap(pendingOrbEvents);
    }
    for (const PendingOrbEvent& event : lifecycleEvents) {
        if (event.added) NotifyOrbEntityAdded(event.handle);
        else NotifyOrbEntityRemoved(event.handle);
    }

    // The table scan remains the authoritative, stable discovery path.
    if (now - lastRefresh < 16ull) return;
    lastRefresh = now;

    std::vector<FarmTarget> found;
    std::vector<OrbTarget> foundOrbs;
    std::vector<WorldEspTarget> foundWorldTargets;
    std::vector<OrbTarget> eventOrbs;
    {
        std::lock_guard<std::mutex> lock(orbTargetsMutex);
        eventOrbs = orbTargets;
    }
    uint32_t scannedEntities = 0;
    uint32_t npcClasses = 0;
    uint32_t trooperClasses = 0;
    uint32_t trooperWithSceneNode = 0;
    uint32_t trooperAlive = 0;
    std::unordered_set<std::string> observedNpcClasses;
    if (!clientBase) return;
    // NPC troopers are published through the primary identity table. Keeping
    // this scan on that table avoids starving farm target refreshes with
    // speculative auxiliary-table walks.
    const uintptr_t root = Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    const uintptr_t tableOffset = Offsets::EntityChunks;
    std::unordered_set<uintptr_t> seen;
    if (root) {
        // HighestEntityIndex is a volatile cache; during spawns it can lag
        // behind a newly allocated handle.  Scan every allocated table chunk
        // instead, otherwise a short-lived orb can be missed completely.
        const uint32_t highestChunk =
            Offsets::HandleIndexMask >> Offsets::HandleChunkShift;
        for (uint32_t chunkIndex = 0; chunkIndex <= highestChunk; ++chunkIndex) {
                const uintptr_t chunk = Read<uintptr_t>(root + tableOffset +
                    Offsets::EntityChunkStride * chunkIndex);
                if (!chunk) continue;
                const uint32_t highestSlot = Offsets::HandleChunkMask;
                for (uint32_t slot = 0; slot <= highestSlot; ++slot) {
                    const uintptr_t identity = chunk + Offsets::EntityStride * slot;
                    const uint32_t storedHandle = Read<uint32_t>(identity + Offsets::EntityHandleOffset);
                    const uint32_t expectedIndex = (chunkIndex << Offsets::HandleChunkShift) | slot;
                    // Several auxiliary arrays sit near the entity table. A
                    // pointer from one of those arrays can still look like a
                    // valid NPC, so require the identity's handle to match
                    // the chunk/slot we are currently enumerating.
                    if ((storedHandle & Offsets::HandleIndexMask) != expectedIndex) continue;
                    const uintptr_t entity = Read<uintptr_t>(identity);
                    if (!entity || !seen.insert(entity).second || entity == currentLocalPawn) continue;
                    ++scannedEntities;

                    const std::string className = GetEntityClassName(entity);
                    if (className.find("NPC") != std::string::npos &&
                        observedNpcClasses.size() < 32) {
                        ++npcClasses;
                        observedNpcClasses.insert(className);
                    }
                    // Reading CUtlSymbolLarge character-by-character is much
                    // more expensive than the class-name check. Only read it
                    // for classes that can plausibly be an orb; CItemXP is
                    // identified directly by its class name.
                    std::string lowerClassName = className;
                    std::transform(lowerClassName.begin(), lowerClassName.end(),
                        lowerClassName.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                    const bool possibleOrbClass =
                        lowerClassName.find("orb") != std::string::npos ||
                        lowerClassName.find("gold") != std::string::npos ||
                        lowerClassName.find("pickup") != std::string::npos ||
                        lowerClassName.find("xp") != std::string::npos ||
                        lowerClassName.find("soul") != std::string::npos;
                    const bool possibleWorldClass = className.find("Power") != std::string::npos ||
                        className.find("Rune") != std::string::npos ||
                        className.find("Pickup") != std::string::npos ||
                        className.find("Talon") != std::string::npos ||
                        className.find("Guided_Arrow") != std::string::npos ||
                        className.find("GuidedArrow") != std::string::npos ||
                        className.find("Projectile") != std::string::npos ||
                        className.find("Ability") != std::string::npos;
                    // Some soul-orbs use a generic client class and are
                    // identifiable only by their designer name. Read that
                    // name for every non-NPC object as well as known class
                    // candidates, so those orbs are not skipped at spawn.
                    const bool nonNpcObject =
                        className.find("NPC") == std::string::npos;
                    // A number of world pickups use a generic entity class.
                    // In that case only the designer name identifies a rune or
                    // powerup, so do not discard the entity before reading it.
                    const std::string designerName =
                         (possibleOrbClass || possibleWorldClass || worldEntityScanActive ||
                         nonNpcObject)
                        ? ReadIdentityDesignerName(identity) : std::string{};
                    if (possibleWorldClass || !designerName.empty()) {
                        std::string markerName = className + " " + designerName;
                        std::transform(markerName.begin(), markerName.end(), markerName.begin(),
                            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        const bool pickupCandidate = markerName.find("_pickup_") != std::string::npos ||
                            markerName.find("itempickup") != std::string::npos;
                        const bool excludedPickup = markerName.find("itemxp") != std::string::npos ||
                            markerName.find("gold") != std::string::npos ||
                            markerName.find("currency") != std::string::npos;
                        const bool isPowerup = markerName.find("powerup") != std::string::npos ||
                            markerName.find("rune") != std::string::npos ||
                            (pickupCandidate && !excludedPickup);
                        const bool isTalon = markerName.find("guided_arrow") != std::string::npos ||
                            markerName.find("guidedarrow") != std::string::npos ||
                            (markerName.find("talon") != std::string::npos &&
                             (markerName.find("projectile") != std::string::npos ||
                              markerName.find("ability") != std::string::npos ||
                              markerName.find("ultimate") != std::string::npos ||
                              markerName.find("owl") != std::string::npos));
                        if ((powerupEspEnabled && isPowerup) || (talonEspEnabled && isTalon)) {
                            Vector3 markerPosition{};
                            if (GetEntityPosition(entity, markerPosition)) {
                                foundWorldTargets.push_back(
                                    {entity, markerPosition, className,
                                     designerName.empty() ? className : designerName});
                            }
                        }
                    }
                    if (possibleOrbClass) {
                        static std::unordered_set<std::string> loggedCandidates;
                        if (loggedCandidates.insert(className).second) {
                            std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_candidates.log", std::ios::app);
                            if (log) log << className << "\n";
                        }
                    }
                    // CItemXP is the actual flying XP/gold orb. Pickup_* and
                    // *OrbSpawner are world helpers, not the shootable orb.
                    const std::string normalizedClassName = NormalizeEntityName(className);
                    const bool isOrb = normalizedClassName.find("itemxp") != std::string::npos ||
                        IsSoulDesignerName(designerName);
                    if (isOrb) {
                        if (IsExpiredXpOrb(entity)) continue;
                        Vector3 position{};
                        // Use the visual position first.  The lifetime guard
                        // above filters retained identity-table entries after
                        // their visible CItemXP has expired.
                        bool hasPosition = GetXpOrbPosition(entity, position);
                        if (!hasPosition) hasPosition = GetEntityPosition(entity, position);
                        if (!hasPosition) continue;
                        if (!ShouldPublishXpOrb(storedHandle, entity, now)) continue;

                        foundOrbs.push_back({ entity, position,
                            designerName.empty() ? className : designerName, storedHandle,
                            Read<uint8_t>(entity + Offsets::Team) });
                        static std::unordered_set<std::string> loggedOrbClasses;
                        if (loggedOrbClasses.insert(className).second) {
                            std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_runtime.log", std::ios::app);
                            if (log) log << "class=" << className << " entity=0x" << std::hex << entity
                                         << std::dec << "\n";
                        }
                        continue;
                    }
                    if (className.find("NPC_Trooper") == std::string::npos &&
                        className.find("C_NPC_Trooper") == std::string::npos) continue;
                    ++trooperClasses;
                    if (className.find("TrooperBoss") != std::string::npos) continue;
                    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
                    if (!sceneNode || Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) != 0) continue;
                    ++trooperWithSceneNode;
                    const int health = Read<int>(entity + Offsets::Health);
                    if (health <= 0 || health > 100000 || Read<uint8_t>(entity + Offsets::LifeState) != 0) continue;
                    ++trooperAlive;
                    Vector3 position{};
                    if (!GetEntityPosition(entity, position)) continue;
                    FarmTarget target{};
                    target.entity = entity;
                    target.pos = position;
                    target.health = health;
                    target.maxHealth = Read<int>(entity + Offsets::MaxHealth);
                    target.team = Read<uint8_t>(entity + Offsets::Team);
                    if (target.team != 2 && target.team != 3 && target.team != 4) continue;
                    if (trooperChamsActive || trooperChamsWereActive)
                        ApplyTrooperGlow(entity);
                    target.className = className;
                    found.push_back(target);
                }
        }
    }
    // Preserve a valid event-discovered orb if the identity scan briefly
    // misses its slot or position during spawn. Exact handle validation and
    // the lifetime check below still remove it immediately after destruction.
    for (OrbTarget& orb : eventOrbs) {
        if (!IsXpOrbAlive(orb.entity, orb.handle)) continue;
        const bool alreadyFound = std::any_of(
            foundOrbs.begin(), foundOrbs.end(), [&](const OrbTarget& foundOrb) {
                return foundOrb.handle == orb.handle ||
                       foundOrb.entity == orb.entity;
            });
        if (alreadyFound) continue;
        Vector3 position{};
        if (GetXpOrbPosition(orb.entity, position) ||
            GetEntityPosition(orb.entity, position)) {
            if (!ShouldPublishXpOrb(orb.handle, orb.entity, now)) continue;
            orb.pos = position;
            orb.team = Read<uint8_t>(orb.entity + Offsets::Team);
            foundOrbs.push_back(std::move(orb));
        }
    }
    // Include lifecycle events that arrived while the table was being
    // enumerated; otherwise the final replacement below can erase a fresh,
    // short-lived orb before the next worker pass.
    {
        std::vector<OrbTarget> lateEventOrbs;
        {
            std::lock_guard<std::mutex> orbLock(orbTargetsMutex);
            lateEventOrbs = orbTargets;
        }
        for (OrbTarget& orb : lateEventOrbs) {
            if (!IsXpOrbAlive(orb.entity, orb.handle)) continue;
            const bool alreadyFound = std::any_of(
                foundOrbs.begin(), foundOrbs.end(),
                [&](const OrbTarget& foundOrb) {
                    return foundOrb.handle == orb.handle ||
                           foundOrb.entity == orb.entity;
                });
            if (alreadyFound) continue;
            Vector3 position{};
            if (GetXpOrbPosition(orb.entity, position) ||
                GetEntityPosition(orb.entity, position)) {
                if (!ShouldPublishXpOrb(orb.handle, orb.entity, now)) continue;
                orb.pos = position;
                orb.team = Read<uint8_t>(orb.entity + Offsets::Team);
                foundOrbs.push_back(std::move(orb));
            }
        }
    }
    std::lock_guard lock(farmTargetsMutex);
    farmTargets = std::move(found);
    {
        std::lock_guard orbLock(orbTargetsMutex);
        orbTargets = std::move(foundOrbs);
    }
    {
        std::lock_guard worldLock(worldEspTargetsMutex);
        worldEspTargets = std::move(foundWorldTargets);
    }
    static ULONGLONG lastCreepScanLog = 0;
    if ((drawCreepEsp || creepEspEnabled || allyCreepEspEnabled || neutralCreepEspEnabled || farmActive) &&
        now - lastCreepScanLog >= 1000) {
        lastCreepScanLog = now;
        std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\creep_scan.log",
                          std::ios::app);
        if (log) {
            log << "entities=" << scannedEntities << " npc=" << npcClasses
                << " trooperClass=" << trooperClasses << " scene=" << trooperWithSceneNode
                << " alive=" << trooperAlive << " targets=" << farmTargets.size() << " classes=";
            for (const auto& name : observedNpcClasses) log << '[' << name << ']';
            log << '\n';
        }
    }
    trooperChamsWereActive = trooperChamsActive;
}

DWORD WINAPI FarmTargetWorker(LPVOID) {
    while (!stopHeroDiscoveryEvent ||
           WaitForSingleObject(stopHeroDiscoveryEvent, 0) != WAIT_OBJECT_0) {
        RefreshFarmTargets();
        // Orb lifetimes can be shorter than a 50 ms polling gap. Keep the
        // worker aligned with the render cadence so each allocated handle is
        // observed before it disappears.
        Sleep(16);
    }
    return 0;
}

DWORD WINAPI HeroDiscoveryWorker(LPVOID) {
    DiscoverHeroVTables();
    if (heroVTables.empty()) {
        printf("[!] Hero RTTI/vtable was not found; using entity-system class fallback\n");
    }
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
            const uint8_t localTeam = currentLocalPawn
                ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
            const int health = Read<int>(pawn + Offsets::Health);
            const uint8_t lifeState = Read<uint8_t>(pawn + Offsets::LifeState);
            const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
            if (health > 0 && lifeState == 0 && (team == 2 || team == 3) &&
                pawn != currentLocalPawn &&
                (localTeam == 0 || localTeam == 2 || localTeam == 3)) {
                const bool ally = localTeam >= 2 && localTeam <= 3 && team == localTeam;
                // Model glow is a presentation layer of the corresponding
                // hero ESP channel. It must never survive when that channel
                // is disabled, even if the standalone master glow switch is
                // still enabled.
                const bool teamEspEnabled = ally ? allyEspEnabled : enemyEspEnabled;
                const bool teamGlowEnabled = ally ? allyGlowEnabled : enemyGlowEnabled;
                const float maxDistance = ally ? allyEspMaxDistance : enemyEspMaxDistance;
                bool withinDistance = true;
                if (currentLocalPositionReady) {
                    Vector3 position{};
                    if (GetEntityPosition(pawn, position)) {
                        const float dx = position.x - currentLocalPosition.x;
                        const float dy = position.y - currentLocalPosition.y;
                        const float dz = position.z - currentLocalPosition.z;
                        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
                        withinDistance = std::isfinite(distance) && distance <= maxDistance;
                    }
                }
                if (glowEnabled && teamEspEnabled && teamGlowEnabled && withinDistance) {
                    ApplyHeroGlow(pawn);
                } else {
                    Write<bool>(pawn + Offsets::Glow + Offsets::GlowEligible, false);
                    Write<bool>(pawn + Offsets::Glow + Offsets::IsGlowing, false);
                    Write<int>(pawn + Offsets::Glow + Offsets::GlowType, 0);
                    std::lock_guard lock(glowMutex);
                    registeredGlowMode.erase(pawn);
                    registeredGlows.erase(pawn);
                    queuedGlows.erase(pawn);
                }
            }
        }

        // The renderer clears m_bGlowing after consuming it. Reassert it on
        // the render cadence while keeping the expensive pawn scan at 1 Hz.
        if (WaitForSingleObject(stopHeroDiscoveryEvent, 50) == WAIT_OBJECT_0) break;
    }
    return 0;
}

bool IsCombatEntity(uintptr_t entity) {
    const uintptr_t vtable = Read<uintptr_t>(entity);
    if (!vtable) return false;

    const auto cached = combatVTables.find(vtable);
    if (cached != combatVTables.end()) return cached->second;

    // MSVC stores a CompleteObjectLocator immediately before the vtable.
    const std::string typeName = GetEntityClassName(entity);

    // These are the replicated combat actors in the current client build. Keeping
    // the check class-based prevents props, towers, pickups, and abilities from ESP.
    const bool isCombatant = typeName.find("CitadelPlayerPawn") != std::string::npos ||
                            typeName.find("NPC_Trooper") != std::string::npos;

    combatVTables.emplace(vtable, isCombatant);
    return isCombatant;
}
