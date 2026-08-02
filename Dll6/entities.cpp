#include "shared.h"
#include <fstream>
#include <shared_mutex>
#include <cctype>
#include <atomic>

namespace {
std::unordered_map<uintptr_t, int> registeredGlowMode;

void LogNativeGlow(const char* message) {
    std::ofstream log(
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\native_glow.log",
        std::ios::app);
    if (log) log << message << '\n';
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

bool NotifyGlowTypeChanged(uintptr_t glow) {
    if (!glow) return false;
    const uintptr_t entity = glow - Offsets::Glow;
    __try {
        // The fixed callback moved with the client build and currently
        // raises an exception. Use the validated native wrapper discovered
        // from the live client image instead.
        Write<bool>(glow + Offsets::GlowEligible, true);
        Write<bool>(glow + Offsets::IsGlowing, true);
        if (!RegisterNativeGlow(entity)) {
            LogNativeGlow("native wrapper failed");
            return false;
        }
        LogNativeGlow("native wrapper ok");
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogNativeGlow("native wrapper exception");
        return false;
    }
}

void ApplyHeroGlow(uintptr_t entity) {
    static std::atomic_bool firstGlowLogged = false;
    if (!firstGlowLogged.exchange(true)) {
        LogNativeGlow("ApplyHeroGlow reached");
    }
    const uintptr_t glow = entity + Offsets::Glow;
    bool shouldNotify = false;

    // The client can reset m_iGlowType after a network update even though the
    // property object remains alive. Re-register whenever the complete-model
    // glow pass is no longer active.
    {
        std::lock_guard lock(glowMutex);
        // The current client uses 2 for the HP-clipped pass and 3 for the
        // complete, non-HP-clipped model fill.  Values 1/2 belong to the
        // previous client build and make Normal fill render as the wrong
        // highlight style (or not render at all).
        const int targetGlowType = glowMode == 1 ? 3 : 2;
        const int currentType = Read<int>(glow + Offsets::GlowType);
        const auto modeIt = registeredGlowMode.find(entity);
        const bool modeChanged = modeIt == registeredGlowMode.end() ||
            modeIt->second != glowMode;
        if ((currentType != targetGlowType || modeChanged) &&
            queuedGlows.insert(entity).second) {
            shouldNotify = true;
        }
        if (shouldNotify) registeredGlowMode[entity] = glowMode;
    }

    const uint8_t localTeam = currentLocalPawn
        ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
    const uint8_t entityTeam = Read<uint8_t>(entity + Offsets::Team);
    const bool ally = localTeam >= 2 && localTeam <= 3 && entityTeam == localTeam;
    const float* glowColor = ally ? teammateGlowColor : enemyGlowColor;
    const int health = Read<int>(entity + Offsets::Health);
    const int maxHealth = Read<int>(entity + Offsets::MaxHealth);
    const float healthAlpha = maxHealth > 0
        ? std::clamp(static_cast<float>(health) / maxHealth, 0.0f, 1.0f) : 0.0f;
    const float glowAlpha = glowMode == 0
        ? glowColor[3] * healthAlpha : 1.0f;
    Write<Vector3>(glow + Offsets::GlowColor,
                   { glowColor[0], glowColor[1], glowColor[2] });
    Write<int>(glow + Offsets::GlowType, glowMode == 1 ? 3 : 2);
    Write<int>(glow + Offsets::GlowTeam, -1);
    Write<int>(glow + Offsets::GlowRange, 0);
    Write<int>(glow + Offsets::GlowRangeMin, 0);
    Write<ColorRGBA>(glow + Offsets::GlowColorOverride,
                     { static_cast<uint8_t>(std::clamp(glowColor[0], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowColor[1], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowColor[2], 0.0f, 1.0f) * 255.0f),
                       static_cast<uint8_t>(std::clamp(glowAlpha, 0.0f, 1.0f) * 255.0f) });
    Write<bool>(glow + Offsets::GlowFlashing, false);
    Write<float>(glow + Offsets::GlowTime, glowMode == 1 ? 0.0f : 1.0f);
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

bool IsXpOrbAttackable(uintptr_t entity) {
    if (!entity) return false;

    struct OrbObservation {
        ULONGLONG firstSeen{};
        ULONGLONG lastSeen{};
        bool attackable{};
    };
    static std::unordered_map<uintptr_t, OrbObservation> observations;
    const ULONGLONG now = GetTickCount64();
    auto& observation = observations[entity];
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
    return lower.rfind("soul", 0) == 0 ||
           lower.rfind("citadel_soul", 0) == 0 ||
           lower.rfind("generic_soul", 0) == 0;
}

bool IsExpiredXpOrb(uintptr_t entity) {
    const float endAttackableTime = Read<float>(entity + 0x0A10);
    const float gameTime = GetClientGameTime();
    if (!std::isfinite(endAttackableTime) || endAttackableTime <= 0.0f ||
        !std::isfinite(gameTime) || gameTime <= 0.0f) return false;
    // Ignore a bad GlobalVars resolution. Valid orb timestamps are close to
    // the current game clock; an unrelated float must never hide an orb.
    if (endAttackableTime > gameTime + 120.0f) return false;
    return gameTime >= endAttackableTime;
}

bool NotifyOrbEntityAdded(uint32_t handle) {
    const uintptr_t entity = ResolveEntity(handle);
    const std::string designerName = GetEntityDesignerName(handle);
    if (!entity) return false;
    const std::string className = GetEntityClassName(entity);
    if (className.empty() && !IsSoulDesignerName(designerName)) return false;
    if (className.find("ItemXP") == std::string::npos && !IsSoulDesignerName(designerName)) return true;
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
std::vector<uint32_t> pendingOrbAdds;
std::vector<uint32_t> pendingOrbRemoves;
}

void QueueOrbEntityAdded(uint32_t handle) {
    std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
    pendingOrbAdds.push_back(handle);
}

void QueueOrbEntityRemoved(uint32_t handle) {
    std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
    pendingOrbRemoves.push_back(handle);
}

void RefreshFarmTargets() {
    static ULONGLONG lastRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    const bool configuredFarmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool farmActive = farmAssist &&
        (farmToggleMode ? farmToggleActive : configuredFarmKeyDown);
    if (!farmActive && !creepEspEnabled && !autoLastHitOrbs && !drawOrbEsp) {
        std::lock_guard lock(farmTargetsMutex);
        farmTargets.clear();
        std::lock_guard orbLock(orbTargetsMutex);
        orbTargets.clear();
        return;
    }
    // Flying souls can exist for less than a second.  A one-second scan
    // interval misses them entirely when ESP is enabled without auto-hit.
    std::vector<uint32_t> addedEvents;
    std::vector<uint32_t> removedEvents;
    {
        std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
        addedEvents.swap(pendingOrbAdds);
        removedEvents.swap(pendingOrbRemoves);
    }
    for (const uint32_t handle : removedEvents) NotifyOrbEntityRemoved(handle);
    for (const uint32_t handle : addedEvents) {
        if (!NotifyOrbEntityAdded(handle)) {
            std::lock_guard<std::mutex> lock(pendingOrbEventsMutex);
            pendingOrbAdds.push_back(handle);
        }
    }

    // The table scan remains the authoritative, stable discovery path.
    if (now - lastRefresh < 16ull) return;
    lastRefresh = now;

    std::vector<FarmTarget> found;
    std::vector<OrbTarget> foundOrbs;
    struct OrbMotionState {
        Vector3 visualPosition{};
        Vector3 entityPosition{};
        bool hasEntityPosition{};
        ULONGLONG lastSeen{};
        uint8_t stationarySamples{};
    };
    static std::unordered_map<uintptr_t, OrbMotionState> orbMotion;
    if (!clientBase) return;
    // The current dump identifies this as a pointer to the client entity
    // system. Its entity identity chunk array starts at +0x10. Do not scan
    // speculative auxiliary tables: that multiplies the work and can make
    // the render thread compete with the worker for CPU time.
    const uintptr_t root = Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    const uintptr_t tableOffset = Offsets::EntityChunks;
    std::unordered_set<uintptr_t> seen;
    if (root) {
        const int reportedHighest = Read<int>(root + Offsets::HighestEntityIndex);
        const uint32_t highestEntityIndex =
            reportedHighest > 0 && reportedHighest <= static_cast<int>(Offsets::HandleIndexMask)
                ? static_cast<uint32_t>(reportedHighest)
                : Offsets::HandleIndexMask;
        const uint32_t highestChunk = highestEntityIndex >> Offsets::HandleChunkShift;
        for (uint32_t chunkIndex = 0; chunkIndex <= highestChunk; ++chunkIndex) {
                const uintptr_t chunk = Read<uintptr_t>(root + tableOffset +
                    Offsets::EntityChunkStride * chunkIndex);
                if (!chunk) continue;
                const uint32_t highestSlot = chunkIndex == highestChunk
                    ? (highestEntityIndex & Offsets::HandleChunkMask)
                    : Offsets::HandleChunkMask;
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

                    const std::string className = GetEntityClassName(entity);
                    // Reading CUtlSymbolLarge character-by-character is much
                    // more expensive than the class-name check. Only read it
                    // for classes that can plausibly be an orb; CItemXP is
                    // identified directly by its class name.
                    const bool possibleOrbClass = className.find("Orb") != std::string::npos ||
                        className.find("Gold") != std::string::npos ||
                        className.find("Pickup") != std::string::npos ||
                        className.find("XP") != std::string::npos ||
                        className.find("Soul") != std::string::npos;
                    const std::string designerName = possibleOrbClass
                        ? ReadIdentityDesignerName(identity) : std::string{};
                    if (possibleOrbClass) {
                        static std::unordered_set<std::string> loggedCandidates;
                        if (loggedCandidates.insert(className).second) {
                            std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\orb_candidates.log", std::ios::app);
                            if (log) log << className << "\n";
                        }
                    }
                    // CItemXP is the actual flying XP/gold orb. Pickup_* and
                    // *OrbSpawner are world helpers, not the shootable orb.
                    const bool isOrb = className.find("ItemXP") != std::string::npos ||
                        IsSoulDesignerName(designerName);
                    if (isOrb) {
                        Vector3 position{};
                        // Keep discovery and ESP on the same stable position
                        // source. RenderOrigin is a transient render cache and
                        // can make the target list flicker between scans.
                        bool hasPosition = GetEntityPosition(entity, position);
                        if (!hasPosition) hasPosition = GetXpOrbPosition(entity, position);
                        if (!hasPosition) continue;

                        // RenderOrigin is the visual point used for drawing,
                        // but it can remain unchanged while the networked
                        // entity is still travelling. Use the entity origin
                        // exclusively for the stopped-state test.
                        Vector3 motionPosition{};
                        const bool hasMotionPosition = GetEntityPosition(entity, motionPosition);
                        if (!hasMotionPosition) motionPosition = position;

                        // Networked orb coordinates can legitimately remain
                        // unchanged for several worker scans. Requiring a few
                        // stationary samples avoids flickering the target out
                        // between snapshots while still pruning stale slots.
                        // Entity pointers are recycled by the game. The
                        // handle identifies the current orb lifetime, so a
                        // newly spawned orb must not inherit the old orb's
                        // stationary counter.
                        const uintptr_t motionKey = storedHandle != 0
                            ? static_cast<uintptr_t>(storedHandle) : entity;
                        auto& motion = orbMotion[motionKey];
                        const bool newEntity = motion.lastSeen == 0 ||
                            now - motion.lastSeen > 1000;
                        if (newEntity) {
                            motion.visualPosition = position;
                            motion.entityPosition = motionPosition;
                            motion.hasEntityPosition = hasMotionPosition;
                            motion.stationarySamples = 0;
                        } else {
                            const bool visualMoved = position.x != motion.visualPosition.x ||
                                position.y != motion.visualPosition.y ||
                                position.z != motion.visualPosition.z;
                            const bool entityMoved = hasMotionPosition && motion.hasEntityPosition &&
                                (motionPosition.x != motion.entityPosition.x ||
                                 motionPosition.y != motion.entityPosition.y ||
                                 motionPosition.z != motion.entityPosition.z);
                            if (visualMoved || entityMoved) {
                                motion.visualPosition = position;
                                motion.entityPosition = motionPosition;
                                motion.hasEntityPosition = hasMotionPosition;
                                motion.stationarySamples = 0;
                            } else if (motion.stationarySamples < UINT8_MAX) {
                                ++motion.stationarySamples;
                            }
                        }
                        motion.lastSeen = now;
                        // Two consecutive scans with the same coordinates are
                        // the disappearance signal used by the orb tracker.
                        // Keep this filter authoritative: ESP and aim both
                        // consume orbTargets produced below.
                        if (motion.stationarySamples >= 1) continue;

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
                    if (className.find("TrooperBoss") != std::string::npos) continue;
                    const uintptr_t sceneNode = Read<uintptr_t>(entity + Offsets::GameSceneNode);
                    if (!sceneNode || Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) != 0) continue;
                    const int health = Read<int>(entity + Offsets::Health);
                    if (health <= 0 || health > 100000 || Read<uint8_t>(entity + Offsets::LifeState) != 0) continue;
                    Vector3 position{};
                    if (!GetEntityPosition(entity, position)) continue;
                    FarmTarget target{};
                    target.entity = entity;
                    target.pos = position;
                    target.health = health;
                    target.maxHealth = Read<int>(entity + Offsets::MaxHealth);
                    target.team = Read<uint8_t>(entity + Offsets::Team);
                    if (target.team != 2 && target.team != 3 && target.team != 4) continue;
                    target.className = className;
                    found.push_back(target);
                }
        }
    }
    for (auto it = orbMotion.begin(); it != orbMotion.end();) {
        if (now - it->second.lastSeen > 2000) it = orbMotion.erase(it);
        else ++it;
    }
    std::lock_guard lock(farmTargetsMutex);
    farmTargets = std::move(found);
    {
        std::lock_guard orbLock(orbTargetsMutex);
        orbTargets = std::move(foundOrbs);
    }
}

DWORD WINAPI FarmTargetWorker(LPVOID) {
    while (!stopHeroDiscoveryEvent ||
           WaitForSingleObject(stopHeroDiscoveryEvent, 0) != WAIT_OBJECT_0) {
        RefreshFarmTargets();
        // Entity additions/removals are delivered through the event queue;
        // the table scan only needs to refresh positions and validate stale
        // slots. Scanning it every 16 ms needlessly competes with rendering
        // during teamfights, so leave a small CPU budget for the game.
        Sleep(50);
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
                if (glowEnabled) {
                    ApplyHeroGlow(pawn);
                } else {
                    Write<bool>(pawn + Offsets::Glow + Offsets::GlowEligible, false);
                    Write<bool>(pawn + Offsets::Glow + Offsets::IsGlowing, false);
                    Write<int>(pawn + Offsets::Glow + Offsets::GlowType, 0);
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
