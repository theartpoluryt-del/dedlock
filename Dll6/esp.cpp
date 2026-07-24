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
    if (drawFovCircle && aimAssist && aimFov > 0.0f && displaySize.x > 0.0f && displaySize.y > 0.0f) {
        const ImVec2 screenCenter(displaySize.x * 0.5f, displaySize.y * 0.5f);
        const int alpha = static_cast<int>(std::clamp(fovCircleAlpha, 0.0f, 255.0f));
        drawList->AddCircle(screenCenter, aimFov, ImColor(255, 255, 255, alpha), 96, 1.0f);
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
    ImGui::Begin("Deadlock Internal", &menuOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

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

    if (ImGui::CollapsingHeader("Aim", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Aim assist", &aimAssist);
        if (ImGui::Button(aimKeyCapture ? "Press aim key..." : AimKeyName(aimAssistKey))) {
            aimKeyCapture = true;
        }
        ImGui::SameLine();
        ImGui::TextUnformatted("Aim-assist key");
        ImGui::SameLine();
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
        ImGui::Checkbox("Auto parry (F)", &autoParry);
        ImGui::Checkbox("Silent Aim (No Visual)", &aimSilentMode);
        ImGui::Checkbox("Visibility check", &aimVisibilityCheck);
        int targetMode = static_cast<int>(aimTargetMode);
        const char* targetModes[] = { "Head", "Body", "Closest" };
        if (ImGui::Combo("Aim target", &targetMode, targetModes, IM_ARRAYSIZE(targetModes))) {
            aimTargetMode = static_cast<AimTargetMode>(std::clamp(targetMode, 0, 2));
        }
        ImGui::SliderFloat("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
        ImGui::SliderFloat("Aim smooth", &aimSmooth, 1.0f, 20.0f, "%.1f");
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

    ImGui::End();

    if (wasMenuOpen && !menuOpen) {
        SetMenuOpen(false);
    }
}
