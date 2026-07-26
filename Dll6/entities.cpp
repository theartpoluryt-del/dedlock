#include "shared.h"
#include <fstream>
#include <cctype>

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
            if (Read<uint32_t>(identity + 0x10) != handle) continue;
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
            const uint32_t storedHandle = Read<uint32_t>(identity + 0x10);
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
    if (height < 4.0f || height > ImGui::GetIO().DisplaySize.y * 1.5f) return false;

    // The collision capsule stops short of the head and feet on several hero
    // models. Keep the projected horizontal bounds, but compensate vertically
    // for the render model rather than drawing a box around only the capsule.
    top -= height * 0.30f;
    bottom += height * 0.08f;
    const float margin = (right - left) * 0.08f;
    left -= margin;
    right += margin;
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
           std::isfinite(bottom) && right > left && bottom > top;
}

std::string GetEntityClassName(uintptr_t entity) {
    if (!entity || !clientBase) return {};
    const uintptr_t vtable = Read<uintptr_t>(entity);
    if (!vtable) return {};
    static std::unordered_map<uintptr_t, std::string> classCache;
    static std::mutex classCacheMutex;
    {
        std::lock_guard<std::mutex> lock(classCacheMutex);
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
        std::lock_guard<std::mutex> lock(classCacheMutex);
        if (classCache.size() < 512) classCache.emplace(vtable, typeName);
    }
    return typeName;
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

                        if (haveHeroVTables) {
                            const uintptr_t vtable = Read<uintptr_t>(entity);
                            if (std::find(heroVTables.begin(), heroVTables.end(), vtable) == heroVTables.end()) continue;
                        }

                        const int health = Read<int>(entity + Offsets::Health);
                        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
                        if (health < 0 || health > 10000 || (team != 2 && team != 3)) continue;
                        if (GetEntityClassName(entity).find("CitadelPlayerPawn") == std::string::npos) continue;

                        Vector3 position{};
                        if (GetEntityPosition(entity, position)) found.push_back(entity);
                    }
                }
            }
        }
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
            if (Read<uint32_t>(identity + 0x10) != handle) continue;
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
    if (!farmActive && !drawCreepEsp && !autoLastHitOrbs && !drawOrbEsp) {
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
    constexpr uintptr_t tableOffset = Offsets::EntityChunks;
    std::unordered_set<uintptr_t> seen;
    if (root) {
        for (uint32_t chunkIndex = 0; chunkIndex <= (Offsets::MaxEntityIndex >> Offsets::HandleChunkShift); ++chunkIndex) {
                const uintptr_t chunk = Read<uintptr_t>(root + tableOffset +
                    Offsets::EntityChunkStride * chunkIndex);
                if (!chunk) continue;
                for (uint32_t slot = 0; slot <= Offsets::HandleChunkMask; ++slot) {
                    const uintptr_t identity = chunk + Offsets::EntityStride * slot;
                    const uint32_t storedHandle = Read<uint32_t>(identity + 0x10);
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
                        bool hasPosition = GetXpOrbPosition(entity, position);
                        if (!hasPosition) hasPosition = GetEntityPosition(entity, position);
                        if (!hasPosition) continue;

                        // RenderOrigin is the visual point used for drawing,
                        // but it can remain unchanged while the networked
                        // entity is still travelling. Use the entity origin
                        // exclusively for the stopped-state test.
                        Vector3 motionPosition{};
                        const bool hasMotionPosition = GetEntityPosition(entity, motionPosition);
                        if (!hasMotionPosition) motionPosition = position;

                        // A valid position that is unchanged for two complete
                        // scans means the orb has already disappeared. A
                        // single unchanged sample is retained so slow orbs do
                        // not flicker out of ESP.
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
                            constexpr float kPositionEpsilon = 0.001f;
                            const float visualDx = position.x - motion.visualPosition.x;
                            const float visualDy = position.y - motion.visualPosition.y;
                            const float visualDz = position.z - motion.visualPosition.z;
                            const bool visualMoved = visualDx * visualDx + visualDy * visualDy +
                                visualDz * visualDz > kPositionEpsilon * kPositionEpsilon;
                            const float entityDx = motionPosition.x - motion.entityPosition.x;
                            const float entityDy = motionPosition.y - motion.entityPosition.y;
                            const float entityDz = motionPosition.z - motion.entityPosition.z;
                            const bool entityMoved = hasMotionPosition && motion.hasEntityPosition &&
                                entityDx * entityDx + entityDy * entityDy + entityDz * entityDz >
                                kPositionEpsilon * kPositionEpsilon;
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
                        if (motion.stationarySamples >= 2) continue;

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
                    const uint32_t npcState = Read<uint32_t>(entity + Offsets::NPCState);
                    if (npcState == 5 || npcState == 6 || Read<uint8_t>(entity + Offsets::FadeCorpse) != 0) continue;
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
                pawn != currentLocalPawn && (localTeam == 0 || team != localTeam)) {
                if (glowEnabled) {
                    ApplyHeroGlow(pawn);
                } else {
                    Write<bool>(pawn + Offsets::Glow + Offsets::IsGlowing, false);
                    Write<int>(pawn + Offsets::Glow + Offsets::GlowType, 0);
                }
            }
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
    const std::string typeName = GetEntityClassName(entity);

    // These are the replicated combat actors in the current client build. Keeping
    // the check class-based prevents props, towers, pickups, and abilities from ESP.
    const bool isCombatant = typeName.find("CitadelPlayerPawn") != std::string::npos ||
                            typeName.find("NPC_Trooper") != std::string::npos;

    combatVTables.emplace(vtable, isCombatant);
    return isCombatant;
}
