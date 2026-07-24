#include "shared.h"

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
    if (!sceneNode) return false;

    position = Read<Vector3>(sceneNode + Offsets::SceneNodeAbsOrigin);
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
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
