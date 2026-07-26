#include "shared.h"

void SetMenuOpen(bool open) {
    menuOpen = open;

    if (imguiInitialized && ImGui::GetCurrentContext()) {
        ImGui::GetIO().MouseDrawCursor = open;
    }

    if (open) {
        if (gameWindow) {
            SetCapture(gameWindow);
            SetFocus(gameWindow);
        }
        ClipCursor(nullptr);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    } else {
        ReleaseCapture();
    }
}

namespace {

const char* AimKeyName(int key) {
    switch (key) {
        case VK_LBUTTON: return "LMB";
        case VK_RBUTTON: return "RMB";
        case VK_MBUTTON: return "MMB";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_SHIFT: return "Shift";
        case VK_CONTROL: return "Ctrl";
        case VK_MENU: return "Alt";
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        default: {
            static char name[16];
            if (key >= 'A' && key <= 'Z') {
                name[0] = static_cast<char>(key);
                name[1] = '\0';
                return name;
            }
            std::snprintf(name, sizeof(name), "VK 0x%02X", key & 0xFF);
            return name;
        }
    }
}

std::string HeroNameFromId(uint32_t heroId) {
    switch (heroId) {
        case 1: return "Infernus";
        case 2: return "Seven";
        case 3: return "Vindicta";
        case 4: return "Lady Geist";
        case 6: return "Abrams";
        case 7: return "Wraith";
        case 8: return "McGinnis";
        case 10: return "Paradox";
        case 11: return "Dynamo";
        case 12: return "Kelvin";
        case 13: return "Haze";
        case 14: return "Holliday";
        case 15: return "Bebop";
        case 16: return "Calico";
        case 17: return "Grey Talon";
        case 18: return "Mo & Krill";
        case 19: return "Shiv";
        case 20: return "Ivy";
        case 25: return "Warden";
        case 27: return "Yamato";
        case 31: return "Lash";
        case 35: return "Viscous";
        case 50: return "Pocket";
        case 52: return "Mirage";
        case 55: return "Training Dummy";
        case 58: return "Vyper";
        case 60: return "Sinclair";
        case 63: return "Mina";
        case 64: return "Drifter";
        case 65: return "Venator";
        case 66: return "Victor";
        case 67: return "Paige";
        case 69: return "The Doorman";
        case 72: return "Billy";
        case 76: return "Graves";
        case 77: return "Apollo";
        case 79: return "Rem";
        case 80: return "Silver";
        case 81: return "Celeste";
        default: return "Unknown hero";
    }
}

std::string ReadHeroName(uintptr_t entity) {
    if (!entity) return "Unknown hero";
    const uint32_t heroId = Read<uint32_t>(entity + Offsets::HeroComponent + Offsets::HeroSpawnedId);
    static std::unordered_set<uint32_t> loggedIds;
    if (loggedIds.insert(heroId).second) {
        FILE* log = nullptr;
        if (fopen_s(&log, "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\hero_ids.log", "a") == 0 && log) {
            fprintf(log, "entity=%p spawned=0x%X loading=0x%X component=0x%X portrait=0x%X\\n",
                    reinterpret_cast<void*>(entity),
                    heroId,
                    Read<uint32_t>(entity + Offsets::HeroComponent + 0x30),
                    Read<uint32_t>(entity + Offsets::HeroComponent),
                    Read<uint32_t>(entity + 0x10C8));
            fclose(log);
        }
    }
    return HeroNameFromId(heroId);
}

std::string ReadPlayerName(uintptr_t controller) {
    if (!controller) return {};
    std::string name;
    for (size_t i = 0; i < 128; ++i) {
        const char c = Read<char>(controller + Offsets::PlayerName + i);
        if (!c) break;
        if (static_cast<unsigned char>(c) < 0x20 ||
            static_cast<unsigned char>(c) > 0x7E) break;
        name.push_back(c);
    }
    return name;
}

// The camera object still lives at the old data anchor.  Its view and
// projection matrices are stored consecutively in the render-camera state.
bool ReadCurrentViewMatrix(Matrix4x4& matrix) {
    if (!clientBase) return false;

    const uintptr_t camera = clientBase + 0x3799830;
    const Matrix4x4 view = Read<Matrix4x4>(camera + 0x80);
    const Matrix4x4 projection = Read<Matrix4x4>(camera + 0xC0);

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            if (!std::isfinite(view.m[row][column]) || !std::isfinite(projection.m[row][column]))
                return false;
        }
    }

    // WorldToScreen consumes a column-vector clip matrix, so compose P * V.
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            matrix.m[row][column] = 0.0f;
            for (int k = 0; k < 4; ++k)
                matrix.m[row][column] += projection.m[row][k] * view.m[k][column];
        }
    }
    return std::isfinite(matrix.m[3][2]) && std::fabs(matrix.m[3][2]) > 0.001f;
}

bool ReadCameraWorldPosition(Vector3& position) {
    if (!clientBase) return false;
    position = Read<Vector3>(clientBase + 0x3799830 + 0x28);
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}

}

std::vector<PlayerData> GetPlayers() {
    std::vector<PlayerData> players;
    espStatus = {};

    if (!clientBase) return players;

    Matrix4x4 viewMatrix{};
    currentViewMatrixReady = ReadCurrentViewMatrix(viewMatrix);
    if (!currentViewMatrixReady) return players;
    currentViewMatrix = viewMatrix;
    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }
    espStatus.heroPawnsFound = !pawns.empty();

    Vector3 localPos{};
    Vector3 cameraPosition{};
    Vector3 distanceOrigin{};
    const bool cameraPositionFound = ReadCameraWorldPosition(cameraPosition);
    currentCameraPosition = cameraPosition;
    currentCameraPositionReady = cameraPositionFound;
    uintptr_t localPawn = 0;
    bool localPositionFound = false;
    // Prefer the actual locally controlled pawn when the controller is present in the entity system.
    for (const uintptr_t pawn : pawns) {
        const uintptr_t controller = ResolveEntity(Read<uint32_t>(pawn + Offsets::PawnController));
        if (controller && Read<uint8_t>(controller + Offsets::IsLocalPlayerController) == 1 &&
            GetEntityPosition(pawn, localPos)) {
            localPositionFound = true;
            localPawn = pawn;
            break;
        }
    }
    // Training bots may not expose their controller in the regular entity list. In that case,
    // a non-enemy hero is still a much more useful distance reference than a constant value.
    if (!localPositionFound && cameraPositionFound) {
        float bestDistance = FLT_MAX;
        for (const uintptr_t pawn : pawns) {
            if (Read<uint8_t>(pawn + Offsets::Team) == 3) continue;
            Vector3 candidate{};
            if (!GetEntityPosition(pawn, candidate)) continue;
            const float dx = candidate.x - cameraPosition.x;
            const float dy = candidate.y - cameraPosition.y;
            const float dz = candidate.z - cameraPosition.z;
            const float candidateDistance = dx * dx + dy * dy + dz * dz;
            if (candidateDistance < bestDistance) {
                bestDistance = candidateDistance;
                localPos = candidate;
                localPawn = pawn;
                localPositionFound = true;
            }
        }
    }
    espStatus.localPawnFound = localPositionFound;
    currentLocalPosition = localPos;
    currentLocalPositionReady = localPositionFound;
    distanceOrigin = localPositionFound ? localPos : cameraPosition;
    if (localPawn != currentLocalPawn) {
        currentLocalPawn = localPawn;
        // Native trace uses the engine's default filter and does not require a
        // separately resolved pawn handle. Avoid scanning the full entity
        // table every frame after the handle layout changed.
        currentLocalPawnHandle = 0xFFFFFFFFu;
    }
    const uint8_t localTeam = currentLocalPawn ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;

    for (const uintptr_t entity : pawns) {
        const int health = Read<int>(entity + Offsets::Health);
        if (health <= 0) continue;

        const uint8_t lifeState = Read<uint8_t>(entity + Offsets::LifeState);
        if (lifeState != 0) continue;

        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
        if (team != 2 && team != 3) continue;
        if (localTeam != 0 && team == localTeam) continue;

        Vector3 pos{};
        if (!GetEntityPosition(entity, pos)) continue;

        PlayerData player;
        player.pos = pos;
        const uintptr_t collision = Read<uintptr_t>(entity + Offsets::CollisionProperty);
        const Vector3 collisionMins = Read<Vector3>(collision + Offsets::CollisionMins);
        const Vector3 collisionMaxs = Read<Vector3>(collision + Offsets::CollisionMaxs);
        const bool validBounds = collision && std::isfinite(collisionMins.z) &&
                                 std::isfinite(collisionMaxs.z) &&
                                 collisionMins.z < collisionMaxs.z &&
                                 collisionMaxs.z - collisionMins.z >= 20.0f &&
                                 collisionMaxs.z - collisionMins.z <= 200.0f;
        player.modelMinZ = validBounds ? collisionMins.z : 0.0f;
        player.modelMaxZ = validBounds ? collisionMaxs.z : 80.0f;
        player.modelHeight = player.modelMaxZ - player.modelMinZ;
        player.hasHeadBone = GetEntityBonePosition(entity, "head", player.headPos);
        player.hasBodyBone = GetEntityBonePosition(entity, "spine_2", player.bodyPos);
        if (!player.hasBodyBone) player.hasBodyBone = GetEntityBonePosition(entity, "spine_0", player.bodyPos);
        if (drawBones) GetEntityBoneSkeleton(entity, player.bones);
        player.health = health;
        player.maxHealth = Read<int>(entity + Offsets::MaxHealth);
        const uintptr_t controller = ResolveEntity(Read<uint32_t>(entity + Offsets::PawnController));
        if (controller) {
            const int liveMaxHealth = Read<int>(controller + Offsets::ControllerPlayerData + Offsets::PlayerDataHealthMax);
            if (liveMaxHealth > 0 && liveMaxHealth < 100000) player.maxHealth = liveMaxHealth;
        }
        player.team = team;
        player.heroName = ReadHeroName(entity);
        const float dx = pos.x - distanceOrigin.x;
        const float dy = pos.y - distanceOrigin.y;
        const float dz = pos.z - distanceOrigin.z;
        // Source coordinates are in Hammer Units; 39.37 units correspond to 1 meter.
        player.distance = (localPositionFound || std::isfinite(distanceOrigin.x))
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
    auto drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    const bool aimKeyDown = (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool aimEnabled = aimAssist && (aimToggleMode ? aimToggleActive : aimKeyDown);
    const bool farmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool farmEnabled = farmAssist && (farmToggleMode ? farmToggleActive : farmKeyDown);
    const char* aimStatus = aimEnabled
        ? (aimSilentMode ? "AIM  ON  [SILENT]" : "AIM  ON")
        : "AIM  OFF";
    const ImColor aimColor = aimEnabled ? ImColor(80, 220, 120, 235) : ImColor(220, 90, 90, 220);
    const ImVec2 statusPosition(18.0f, 18.0f);
    char fpsText[32]{};
    std::snprintf(fpsText, sizeof(fpsText), "FPS: %.0f", ImGui::GetIO().Framerate);
    const ImVec2 fpsSize = ImGui::CalcTextSize(fpsText);
    const ImVec2 fpsPosition(displaySize.x - fpsSize.x - 18.0f, 18.0f);
    drawList->AddText(ImVec2(fpsPosition.x + 1.0f, fpsPosition.y + 1.0f),
                      ImColor(0, 0, 0, 190), fpsText);
    drawList->AddText(fpsPosition, ImColor(255, 255, 255, 235), fpsText);
    drawList->AddText(ImVec2(statusPosition.x + 1.0f, statusPosition.y + 1.0f),
                      ImColor(0, 0, 0, 190), aimStatus);
    drawList->AddText(statusPosition, aimColor, aimStatus);
    if (farmAssist) {
        const char* farmStatus = farmEnabled ? "CREEP AIM  ON" : "CREEP AIM  OFF";
        const ImColor farmColor = farmEnabled ? ImColor(255, 190, 70, 235) : ImColor(180, 180, 180, 210);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, statusPosition.y + 19.0f),
                          ImColor(0, 0, 0, 190), farmStatus);
        drawList->AddText(ImVec2(statusPosition.x, statusPosition.y + 18.0f), farmColor, farmStatus);
    }
    if (autoLastHitOrbs) {
        const char* orbStatus = autoLastHitOrbsActive ? "ORB AIM  ON" : "ORB AIM  OFF";
        const ImColor orbStatusColor = autoLastHitOrbsActive
            ? ImColor(80, 220, 120, 235) : ImColor(180, 180, 180, 210);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, statusPosition.y + 37.0f),
                          ImColor(0, 0, 0, 190), orbStatus);
        drawList->AddText(ImVec2(statusPosition.x, statusPosition.y + 36.0f),
                          orbStatusColor, orbStatus);
    }
    if (drawSpectatorList) {
    // Enumerate player controllers directly. Spectators use
    // C_CitadelObserverPawn, which is intentionally not part of heroPawns.
    std::vector<std::string> spectators;
    std::unordered_set<std::string> spectatorNames;
    const uintptr_t entityRoot = clientBase
        ? Read<uintptr_t>(clientBase + Offsets::GameEntitySystem) : 0;
    if (entityRoot) {
        for (uint32_t chunkIndex = 0;
             chunkIndex <= (Offsets::MaxEntityIndex >> Offsets::HandleChunkShift); ++chunkIndex) {
            const uintptr_t chunk = Read<uintptr_t>(entityRoot + Offsets::EntityChunks +
                Offsets::EntityChunkStride * chunkIndex);
            if (!chunk) continue;
            for (uint32_t slot = 0; slot <= Offsets::HandleChunkMask; ++slot) {
                const uintptr_t identity = chunk + Offsets::EntityStride * slot;
                const uint32_t handle = Read<uint32_t>(identity + 0x10);
                const uint32_t expectedIndex = (chunkIndex << Offsets::HandleChunkShift) | slot;
                if ((handle & Offsets::HandleIndexMask) != expectedIndex) continue;
                const uintptr_t controller = Read<uintptr_t>(identity);
                if (!controller) continue;
                const std::string className = GetEntityClassName(controller);
                if (className.find("PlayerController") == std::string::npos) continue;
                const uintptr_t pawn = ResolveEntity(Read<uint32_t>(controller + Offsets::ControllerPawn));
                if (!pawn || pawn == currentLocalPawn) continue;
                const uintptr_t observerServices = Read<uintptr_t>(pawn + Offsets::ObserverServices);
                if (!observerServices) continue;
                const uint8_t observerMode = Read<uint8_t>(observerServices + Offsets::ObserverMode);
                const uint32_t observerTarget = Read<uint32_t>(observerServices + Offsets::ObserverTarget);
                if (observerMode == 0 || observerTarget == 0xFFFFFFFFu) continue;
                std::string name = ReadPlayerName(controller);
                if (name.empty()) name = "Unknown";
                if (spectatorNames.insert(name).second) spectators.push_back(std::move(name));
            }
        }
    }
    if (drawSpectatorList) {
        float spectatorY = statusPosition.y + 58.0f;
        const char* title = "SPECTATOR LIST";
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, spectatorY + 1.0f),
                          ImColor(0, 0, 0, 190), title);
        drawList->AddText(ImVec2(statusPosition.x, spectatorY),
                          ImColor(255, 210, 90, 235), title);
        spectatorY += 18.0f;
        if (spectators.empty()) {
            const char* emptyText = "No spectators";
            drawList->AddText(ImVec2(statusPosition.x + 1.0f, spectatorY + 1.0f),
                              ImColor(0, 0, 0, 190), emptyText);
            drawList->AddText(ImVec2(statusPosition.x, spectatorY),
                              ImColor(180, 180, 180, 220), emptyText);
        } else {
            for (const auto& name : spectators) {
                drawList->AddText(ImVec2(statusPosition.x + 1.0f, spectatorY + 1.0f),
                                  ImColor(0, 0, 0, 190), name.c_str());
                drawList->AddText(ImVec2(statusPosition.x, spectatorY),
                                  ImColor(255, 255, 255, 235), name.c_str());
                spectatorY += 17.0f;
            }
        }
    }
    }
    if (drawFovCircle && aimAssist && aimFov > 0.0f && displaySize.x > 0.0f && displaySize.y > 0.0f) {
        const ImVec2 screenCenter(displaySize.x * 0.5f, displaySize.y * 0.5f);
        const int alpha = static_cast<int>(std::clamp(fovCircleAlpha, 0.0f, 255.0f));
        drawList->AddCircle(screenCenter, aimFov, ImColor(255, 255, 255, alpha), 96, 1.0f);
    }
    if (drawFarmFovCircle && farmAssist && farmFov > 0.0f &&
        displaySize.x > 0.0f && displaySize.y > 0.0f) {
        const ImVec2 screenCenter(displaySize.x * 0.5f, displaySize.y * 0.5f);
        const int alpha = static_cast<int>(std::clamp(farmFovAlpha, 0.0f, 255.0f));
        drawList->AddCircle(screenCenter, farmFov, ImColor(255, 190, 70, alpha), 96, 1.0f);
    }

    if (drawOrbEsp && currentViewMatrixReady) {
        std::vector<OrbTarget> orbs;
        {
            std::lock_guard<std::mutex> lock(orbTargetsMutex);
            orbs = orbTargets;
        }
        for (const auto& orb : orbs) {
            // Entity slots are recycled by the game. Never draw an old orb
            // record after its handle resolves to a different entity.
            if (orb.handle != 0 && ResolveEntity(orb.handle) != orb.entity) continue;
            const std::string liveClass = GetEntityClassName(orb.entity);
            if (!liveClass.empty() && liveClass.find("ItemXP") == std::string::npos) continue;
            Vector3 position = orb.pos;
            Vector2 screen{};
            bool projected = false;
            Vector3 candidate{};
            // Use the networked/absolute entity origin first. RenderOrigin is
            // a render-cache value and can be updated differently depending
            // on which side of the entity the camera is viewing from.
            if (GetEntityPosition(orb.entity, candidate)) {
                projected = WorldToScreen(candidate, screen, currentViewMatrix);
                if (projected) position = candidate;
            }
            if (!projected && GetXpOrbPosition(orb.entity, candidate)) {
                projected = WorldToScreen(candidate, screen, currentViewMatrix);
                if (projected) position = candidate;
            }
            if (!projected) projected = WorldToScreen(position, screen, currentViewMatrix);
            if (!projected || !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z)) continue;
            if (projected) {
                std::lock_guard<std::mutex> lock(orbTargetsMutex);
                for (auto& cachedOrb : orbTargets) {
                    if ((orb.handle != 0 && cachedOrb.handle == orb.handle) || cachedOrb.entity == orb.entity) {
                        cachedOrb.pos = position;
                        break;
                    }
                }
            }
            const ImVec2 point(screen.x, screen.y);
            const uint8_t localTeam = currentLocalPawn
                ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
            const bool friendly = localTeam != 0 && orb.team != 0 && orb.team == localTeam;
            // The game uses the opposite visual convention for these items:
            // enemy orbs are green and allied orbs are red.
            const ImColor orbColor = friendly
                ? ImColor(245, 65, 65, 235)
                : ImColor(50, 235, 90, 235);
            const char* orbLabel = friendly ? "Ally orb" : "Enemy orb";
            // C_BaseModelEntity::m_Collision is embedded in the entity.  The old
            // code treated 0x340 as a pointer, producing a distance-dependent,
            // oversized ring.  Project the real bounding sphere instead.
            const uintptr_t orbSceneNode = Read<uintptr_t>(orb.entity + Offsets::GameSceneNode);
            const float absScale = orbSceneNode ? Read<float>(orbSceneNode + Offsets::SceneNodeAbsScale) : 1.0f;
            float outlineRadius = 14.0f;
            const float scale = std::isfinite(absScale) && absScale > 0.001f && absScale < 100.0f
                ? absScale : 1.0f;
            const uintptr_t collision = orb.entity + Offsets::OrbCollisionProperty;
            const Vector3 mins = Read<Vector3>(collision + Offsets::CollisionMins);
            const Vector3 maxs = Read<Vector3>(collision + Offsets::CollisionMaxs);
            const bool validBounds = std::isfinite(mins.x) && std::isfinite(mins.y) && std::isfinite(mins.z) &&
                std::isfinite(maxs.x) && std::isfinite(maxs.y) && std::isfinite(maxs.z) &&
                mins.x < maxs.x && mins.y < maxs.y && mins.z < maxs.z &&
                maxs.x - mins.x < 256.0f && maxs.y - mins.y < 256.0f && maxs.z - mins.z < 256.0f;
            if (validBounds) {
                for (int x = 0; x < 2; ++x) {
                    for (int y = 0; y < 2; ++y) {
                        for (int z = 0; z < 2; ++z) {
                            const Vector3 corner{
                                position.x + (x ? maxs.x : mins.x) * scale,
                                position.y + (y ? maxs.y : mins.y) * scale,
                                position.z + (z ? maxs.z : mins.z) * scale
                            };
                            Vector2 edge{};
                            if (!WorldToScreen(corner, edge, currentViewMatrix)) continue;
                            outlineRadius = (std::max)(outlineRadius,
                                std::sqrt((edge.x - screen.x) * (edge.x - screen.x) +
                                          (edge.y - screen.y) * (edge.y - screen.y)));
                        }
                    }
                }
            }
            outlineRadius *= 0.4f;
            const float fillRadius = outlineRadius * 0.64f;
            if (IsXpOrbAttackable(orb.entity)) {
                drawList->AddCircleFilled(point, fillRadius, orbColor, 24);
            }
            drawList->AddCircle(point, outlineRadius, ImColor(255, 165, 45, 245), 24, 2.0f);
            drawList->AddText(ImVec2(screen.x + outlineRadius + 3.0f, screen.y - 8.0f),
                              ImColor(255, 255, 255, 240), orbLabel);
        }
    }

    if (drawCreepEsp && currentViewMatrixReady) {
        std::vector<FarmTarget> creeps;
        {
            std::lock_guard<std::mutex> lock(farmTargetsMutex);
            creeps = farmTargets;
        }
        std::vector<FarmTarget> liveCreeps;
        liveCreeps.reserve(creeps.size());
        for (auto& creep : creeps) {
            if (creep.className.find("TrooperBoss") != std::string::npos) continue;
            // The worker discovers entities slowly to avoid scanning the full
            // entity table on the render thread. Positions and health, however,
            // must be refreshed every frame so moving creeps do not leave stale
            // boxes behind.
            Vector3 livePosition{};
            if (!GetEntityPosition(creep.entity, livePosition)) continue;
            const uintptr_t sceneNode = Read<uintptr_t>(creep.entity + Offsets::GameSceneNode);
            if (!sceneNode || Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) != 0) continue;
            creep.pos = livePosition;
            creep.health = Read<int>(creep.entity + Offsets::Health);
            creep.maxHealth = Read<int>(creep.entity + Offsets::MaxHealth);
            const uint8_t lifeState = Read<uint8_t>(creep.entity + Offsets::LifeState);
            const uint32_t npcState = Read<uint32_t>(creep.entity + Offsets::NPCState);
            if (creep.health <= 0 || lifeState != 0 || npcState == 5 || npcState == 6 ||
                Read<uint8_t>(creep.entity + Offsets::FadeCorpse) != 0) continue;

            // A stale entity slot can retain the NPC class and a positive
            // health value after its render object is gone. Require a valid
            // model bone before drawing the diagnostic box.
            Vector3 headPosition{};
            if (!GetEntityBonePosition(creep.entity, "head", headPosition)) continue;
            const float headDeltaX = headPosition.x - creep.pos.x;
            const float headDeltaY = headPosition.y - creep.pos.y;
            const float headDeltaZ = headPosition.z - creep.pos.z;
            const float headDistanceSquared = headDeltaX * headDeltaX +
                                              headDeltaY * headDeltaY +
                                              headDeltaZ * headDeltaZ;
            if (!std::isfinite(headDistanceSquared) || headDistanceSquared > 300.0f * 300.0f ||
                headDeltaZ < 8.0f || headDeltaZ > 180.0f) continue;
            // Creep collision bounds are not reliable for diagnostics: on some
            // units they describe a separate collision object, not the render
            // model. Project a conservative box around the scene-node origin.
            constexpr float halfWidth = 22.0f;
            constexpr float height = 72.0f;
            const Vector3 corners[] = {
                { creep.pos.x - halfWidth, creep.pos.y - halfWidth, creep.pos.z },
                { creep.pos.x + halfWidth, creep.pos.y - halfWidth, creep.pos.z },
                { creep.pos.x - halfWidth, creep.pos.y + halfWidth, creep.pos.z },
                { creep.pos.x + halfWidth, creep.pos.y + halfWidth, creep.pos.z },
                { creep.pos.x - halfWidth, creep.pos.y - halfWidth, creep.pos.z + height },
                { creep.pos.x + halfWidth, creep.pos.y - halfWidth, creep.pos.z + height },
                { creep.pos.x - halfWidth, creep.pos.y + halfWidth, creep.pos.z + height },
                { creep.pos.x + halfWidth, creep.pos.y + halfWidth, creep.pos.z + height }
            };
            float left = FLT_MAX, top = FLT_MAX, right = -FLT_MAX, bottom = -FLT_MAX;
            bool projected = false;
            for (const auto& corner : corners) {
                Vector2 screen{};
                if (!WorldToScreen(corner, screen, currentViewMatrix)) continue;
                projected = true;
                left = (std::min)(left, screen.x);
                top = (std::min)(top, screen.y);
                right = (std::max)(right, screen.x);
                bottom = (std::max)(bottom, screen.y);
            }
            if (!projected || right <= left || bottom <= top) continue;
            liveCreeps.push_back(creep);
            const uint8_t localTeam = currentLocalPawn
                ? Read<uint8_t>(currentLocalPawn + Offsets::Team)
                : 0;
            const bool ally = localTeam != 0 && creep.team == localTeam;
            const bool neutral = creep.team == 4;
            const ImColor color = neutral
                ? ImColor(190, 190, 190, 150)
                : (ally ? ImColor(90, 170, 255, 150) : ImColor(255, 170, 0, 220));
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), color, 0.0f, 0, ally ? 1.0f : 1.25f);

            Vector2 originScreen{};
            if (WorldToScreen(creep.pos, originScreen, currentViewMatrix)) {
                drawList->AddCircleFilled(ImVec2(originScreen.x, originScreen.y), 3.0f, color, 12);
            }
            const float dx = creep.pos.x - currentLocalPosition.x;
            const float dy = creep.pos.y - currentLocalPosition.y;
            const float dz = creep.pos.z - currentLocalPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
            char distanceText[24]{};
            std::snprintf(distanceText, sizeof(distanceText), "%.0fm", distance);
            const ImVec2 textSize = ImGui::CalcTextSize(distanceText);
            drawList->AddText(ImVec2((left + right - textSize.x) * 0.5f,
                                     bottom + 4.0f),
                              ImColor(255, 255, 255, 220), distanceText);

            const float healthPercent = creep.maxHealth > 0
                ? std::clamp(static_cast<float>(creep.health) / creep.maxHealth, 0.0f, 1.0f)
                : 0.0f;
            constexpr float barWidth = 3.0f;
            const float barLeft = left - 6.0f;
            drawList->AddRectFilled(ImVec2(barLeft, top), ImVec2(barLeft + barWidth, bottom),
                                    ImColor(40, 40, 40, 130));
            const ImColor healthColor = neutral
                ? ImColor(190, 190, 190, 190)
                : (ally ? ImColor(80, 180, 255, 190) : ImColor(70, 220, 100, 220));
            drawList->AddRectFilled(ImVec2(barLeft, bottom - (bottom - top) * healthPercent),
                                    ImVec2(barLeft + barWidth, bottom), healthColor);
        }
        {
            std::lock_guard<std::mutex> lock(farmTargetsMutex);
            farmTargets = std::move(liveCreeps);
        }
    }

    if (!drawEsp) return;

    Vector2 localScreen{};
    const bool localOnScreen = currentLocalPositionReady &&
                               WorldToScreen(currentLocalPosition, localScreen, currentViewMatrix);

    for (const auto& player : players) {
        const float screenX = (player.boxLeft + player.boxRight) * 0.5f;
        const float screenY = player.boxBottom;
        const float boxTop = player.boxTop;
        const float boxHeight = player.boxBottom - player.boxTop;
        const float boxWidth = player.boxRight - player.boxLeft;

        if (drawBones) {
            for (const auto& bone : player.bones) {
                Vector2 start{}, end{};
                if (WorldToScreen(bone.start, start, currentViewMatrix) &&
                    WorldToScreen(bone.end, end, currentViewMatrix)) {
                    drawList->AddLine(ImVec2(start.x, start.y), ImVec2(end.x, end.y), ImColor(255, 220, 40, 220), 1.5f);
                }
            }
        }

        if (drawSnaplines) {
            const ImVec2 lineStart = localOnScreen
                ? ImVec2(localScreen.x, localScreen.y)
                : ImVec2(displaySize.x * 0.5f, displaySize.y);
            const int alpha = static_cast<int>(std::clamp(snaplineAlpha, 0.0f, 255.0f));
            drawList->AddLine(lineStart, ImVec2(screenX, screenY), ImColor(255, 255, 255, alpha), 1.0f);
        }

        if (drawNames) {
            const ImVec2 nameSize = ImGui::CalcTextSize(player.heroName.c_str());
            drawList->AddText(
                ImVec2(screenX - nameSize.x * 0.5f, boxTop - 30.0f),
                ImColor(255, 0, 0, 255),
                player.heroName.c_str()
            );
        }

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

            if (drawHealthValues) {
                const std::string healthText = std::to_string(player.health) + "/" + std::to_string(player.maxHealth);
                drawList->AddText(ImVec2(barLeft - 4.0f, boxTop - 14.0f), ImColor(255, 255, 255, 220), healthText.c_str());
            }
        }

        if (drawDistance && player.distance > 0.0f) {
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
    ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Deadlock Internal", &menuOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::BeginTable("TopSections", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Visuals", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("ESP", &drawEsp);
            ImGui::Checkbox("Boxes", &drawBoxes);
            ImGui::Checkbox("Health Bars", &drawHealth);
            ImGui::Checkbox("Health Values", &drawHealthValues);
            ImGui::Checkbox("Hero Names", &drawNames);
            ImGui::Checkbox("Distance", &drawDistance);
            ImGui::Checkbox("Snaplines", &drawSnaplines);
            ImGui::Checkbox("Bones", &drawBones);
            if (drawSnaplines) ImGui::SliderFloat("Snapline alpha", &snaplineAlpha, 0.0f, 255.0f, "%.0f");
            ImGui::Checkbox("Glow", &glowEnabled);
            ImGui::Checkbox("FOV circle", &drawFovCircle);
            if (drawFovCircle) ImGui::SliderFloat("FOV circle alpha", &fovCircleAlpha, 0.0f, 255.0f, "%.0f");
        }

        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Farm Assist", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Creep aim", &farmAssist);
            ImGui::SameLine();
            if (ImGui::RadioButton("Normal##FarmMode", !farmSilentMode)) farmSilentMode = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Silent##FarmMode", farmSilentMode)) farmSilentMode = true;
            if (ImGui::Button(farmKeyCapture ? "Press creep key..." : AimKeyName(farmAssistKey))) {
                farmKeyCapture = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Creep key");
            if (ImGui::RadioButton("Hold", !farmToggleMode)) {
                farmToggleMode = false;
                farmToggleActive = false;
                farmToggleLastDown = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Toggle", farmToggleMode)) {
                farmToggleMode = true;
                farmToggleActive = false;
                farmToggleLastDown = false;
            }
            ImGui::SliderFloat("Creep smooth", &farmAimSmooth, 1.0f, 20.0f, "%.1f");
            ImGui::SliderFloat("Farm assist FOV", &farmFov, 40.0f, 600.0f, "%.0f px");
            ImGui::Checkbox("Farm assist FOV circle", &drawFarmFovCircle);
            if (drawFarmFovCircle) ImGui::SliderFloat("Farm assist FOV alpha", &farmFovAlpha, 0.0f, 255.0f, "%.0f");
            ImGui::Checkbox("Creep ESP", &drawCreepEsp);
            ImGui::Checkbox("Orb ESP", &drawOrbEsp);
            ImGui::Checkbox("Auto Last Hit Orbs", &autoLastHitOrbs);
            if (autoLastHitOrbs) {
                ImGui::SameLine();
                if (ImGui::RadioButton("Auto fire##OrbMode", autoLastHitOrbsAutoFire)) autoLastHitOrbsAutoFire = true;
                ImGui::SameLine();
                if (ImGui::RadioButton("Player fire##OrbMode", !autoLastHitOrbsAutoFire)) autoLastHitOrbsAutoFire = false;
                if (ImGui::Button(autoLastHitOrbsKeyCapture ? "Press orb key..." : AimKeyName(autoLastHitOrbsKey))) {
                    autoLastHitOrbsKeyCapture = true;
                }
                ImGui::SameLine();
                ImGui::TextUnformatted("Orb key");
                if (ImGui::RadioButton("Hold##OrbBind", !autoLastHitOrbsToggleMode)) autoLastHitOrbsToggleMode = false;
                ImGui::SameLine();
                if (ImGui::RadioButton("Toggle##OrbBind", autoLastHitOrbsToggleMode)) autoLastHitOrbsToggleMode = true;
                ImGui::Checkbox("Orb visibility check", &orbAimVisibilityCheck);
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::BeginTable("AimMiscSections", 2,
                          ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Aim", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Aim assist", &aimAssist);
            ImGui::SameLine();
            if (ImGui::RadioButton("Normal##AimMode", !aimSilentMode)) aimSilentMode = false;
            ImGui::SameLine();
            if (ImGui::RadioButton("Silent##AimMode", aimSilentMode)) aimSilentMode = true;
            if (ImGui::Button(aimKeyCapture ? "Press aim key..." : AimKeyName(aimAssistKey))) {
                aimKeyCapture = true;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted("Aim key");
            if (ImGui::RadioButton("Hold", !aimToggleMode)) {
                aimToggleMode = false;
                aimToggleActive = false;
                aimToggleLastDown = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Toggle", aimToggleMode)) {
                aimToggleMode = true;
                aimToggleActive = false;
                aimToggleLastDown = false;
            }
            ImGui::Checkbox("Visibility check", &aimVisibilityCheck);
            int targetMode = static_cast<int>(aimTargetMode);
            const char* targetModes[] = { "Head", "Body", "Closest" };
            if (ImGui::Combo("Aim target", &targetMode, targetModes, IM_ARRAYSIZE(targetModes))) {
                aimTargetMode = static_cast<AimTargetMode>(std::clamp(targetMode, 0, 2));
            }
            ImGui::SliderFloat("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
            ImGui::SliderFloat("Aim smooth", &aimSmooth, 1.0f, 20.0f, "%.1f");
        }
        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Misc", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Auto parry (F)", &autoParry);
            ImGui::Checkbox("Spectator list", &drawSpectatorList);
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Unload DLL")) {
        RequestUnload();
    }

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Players: %zu", playerCount);
    const char* heroScanState = !espStatus.heroScanComplete
                                    ? "scanning"
                                    : (espStatus.heroPawnsFound ? "ready" : "no pawns");
    ImGui::Text("Hero scan: %s", heroScanState);
    if (drawCreepEsp) {
        std::lock_guard<std::mutex> lock(farmTargetsMutex);
        ImGui::Text("Creep scan: %zu targets", farmTargets.size());
    }

    ImGui::End();

    if (wasMenuOpen && !menuOpen) {
        SaveConfig();
        SetMenuOpen(false);
    }
}
