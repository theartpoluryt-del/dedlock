#include "shared.h"
#include "offsets_runtime.h"

void SetMenuOpen(bool open) {
    menuOpen = open;

    if (imguiInitialized && ImGui::GetCurrentContext()) {
        ImGui::GetIO().MouseDrawCursor = open;
    }

    if (open) {
        ClipCursor(nullptr);
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }
}

namespace {

// The camera object still lives at the old data anchor.  Its view and
// projection matrices are stored consecutively in the render-camera state.
bool ReadCurrentViewMatrix(Matrix4x4& matrix) {
    if (!clientBase) return false;

    const uintptr_t camera = clientBase + GetRuntimeOffsets().viewMatrixRva;
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
    position = Read<Vector3>(clientBase + GetRuntimeOffsets().viewMatrixRva + 0x28);
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

    for (const uintptr_t entity : pawns) {
        const int health = Read<int>(entity + Offsets::Health);
        if (health <= 0) continue;

        const uint8_t lifeState = Read<uint8_t>(entity + Offsets::LifeState);
        if (lifeState != 0) continue;

        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
        if (team != 3) continue;

        Vector3 pos{};
        if (!GetEntityPosition(entity, pos)) continue;

        PlayerData player;
        player.pos = pos;
        player.health = health;
        player.maxHealth = Read<int>(entity + Offsets::MaxHealth);
        player.team = team;
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
    if (!drawEsp) return;

    auto drawList = ImGui::GetBackgroundDrawList();

    for (const auto& player : players) {
        const float screenX = (player.boxLeft + player.boxRight) * 0.5f;
        const float screenY = player.boxBottom;
        const float boxTop = player.boxTop;
        const float boxHeight = player.boxBottom - player.boxTop;
        const float boxWidth = player.boxRight - player.boxLeft;

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

            const std::string healthText = std::to_string(player.health) + "/" + std::to_string(player.maxHealth);
            drawList->AddText(ImVec2(barLeft - 4.0f, boxTop - 14.0f), ImColor(255, 255, 255, 220), healthText.c_str());
        }

        if (player.distance > 0.0f) {
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

    ImGui::Checkbox("ESP", &drawEsp);
    ImGui::Checkbox("Boxes", &drawBoxes);
    ImGui::Checkbox("Health Bars", &drawHealth);
    ImGui::Checkbox("Aim assist (hold RMB)", &aimAssist);
    ImGui::Checkbox("Auto parry (F)", &autoParry);
    ImGui::Checkbox("Silent Aim (No Visual)", &aimSilentMode);

    ImGui::SliderFloat("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
    ImGui::SliderFloat("Aim smooth", &aimSmooth, 1.0f, 20.0f, "%.1f");
    if (ImGui::Button("Unload DLL (Delete)")) {
        RequestUnload();
    }

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    ImGui::Text("Players: %zu", playerCount);
    ImGui::Text("Hero scan: %s", espStatus.heroPawnsFound ? "ready" : "searching");

    ImGui::End();

    if (wasMenuOpen && !menuOpen) {
        SetMenuOpen(false);
    }
}
