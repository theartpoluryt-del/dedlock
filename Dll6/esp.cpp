#include "shared.h"
#include "hero_scripts.h"
#include "resource.h"
#include "portable_paths.h"
#include <wincodec.h>
extern bool InvertMatrix(const Matrix4x4&, Matrix4x4&);
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

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
    static thread_local std::string name;
    name = GetVirtualKeyDisplayName(key);
    return name.c_str();
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
        if (fopen_s(&log, Dll6Paths::DataFileA("hero_ids.log").c_str(), "a") == 0 && log) {
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
    name.reserve(128);
    for (size_t i = 0; i < 128; ++i) {
        const unsigned char c = Read<unsigned char>(
            controller + Offsets::PlayerName + i);
        if (!c) break;
        // Source 2 stores m_iszPlayerName as a UTF-8 char[128]. Previously
        // every byte above ASCII 0x7E terminated the read, so Cyrillic and
        // other Unicode names always became an empty string/"Unknown".
        if (c < 0x20 || c == 0x7F) continue;
        name.push_back(static_cast<char>(c));
    }
    return name;
}

void DrawAimBoneSelectorImGui() {
    aimBonesMask &= AimBoneAll;
    if (!aimBonesMask) aimBonesMask = AimBoneHead;
    if (antiFrog && (aimBonesMask & ~AimBoneHead) == 0)
        aimBonesMask |= AimBoneNeck;
    static constexpr const char* labels[]{
        "Head", "Neck", "Torso", "Arms", "Legs"};
    static constexpr int bits[]{
        AimBoneHead, AimBoneNeck, AimBoneTorso, AimBoneArms, AimBoneLegs};
    ImGui::TextUnformatted("Target bones");
    for (int index = 0; index < 5; ++index) {
        if (index != 0) ImGui::SameLine();
        bool selected = (aimBonesMask & bits[index]) != 0;
        if (ImGui::Checkbox(labels[index], &selected)) {
            int next = selected ? aimBonesMask | bits[index]
                                : aimBonesMask & ~bits[index];
            if (next != 0) {
                if (antiFrog && (next & ~AimBoneHead) == 0)
                    next |= AimBoneNeck;
                aimBonesMask = next & AimBoneAll;
            }
        }
    }
}

struct CollisionDiagnosticSnapshot {
    bool valid{};
    std::string hero;
    Vector3 position{};
    Vector3 mins{};
    Vector3 maxs{};
    Vector3 specifiedMins{};
    Vector3 specifiedMaxs{};
    Vector3 surroundingMins{};
    Vector3 surroundingMaxs{};
    Vector3 capsuleCenter1{};
    Vector3 capsuleCenter2{};
    float boundingRadius{};
    uint8_t solidFlags{};
    uint8_t solidType{};
    float capsuleRadius{};
    uint32_t flags{};
    uint8_t moveType{};
    uint8_t actualMoveType{};
    uint8_t collisionGroup{};
    bool toggleDuck{};
    bool ducked{};
    bool colliding{};
};

CollisionDiagnosticSnapshot collisionSnapshot{};

struct OriginalHullState {
    uintptr_t pawn{};
    uintptr_t collision{};
    Vector3 specifiedMins{};
    Vector3 specifiedMaxs{};
    Vector3 surroundingMins{};
    Vector3 surroundingMaxs{};
    Vector3 maxs{};
    float boundingRadius{};
    uint8_t solidFlags{};
    uint8_t solidType{};
    uintptr_t modifierProperty{};
    uint32_t modifierEnabledState{};
    uint32_t modifierTunnelState{};
    uint32_t modifierEnabledPredictedState{};
    uint32_t modifierTunnelPredictedState{};
    uint8_t modifierStatesDirty{};
    bool modifierCaptured{};
    bool captured{};
};

// EModifierState values from the current server/client schema.
// 278 = MODIFIER_STATE_IS_TINY_CHARACTER
// 289 = MODIFIER_STATE_ALLOW_IN_TUNNELS_NO_DUCK
constexpr uint32_t kTinyCharacterState = 278;
constexpr uint32_t kTunnelState = 289;
constexpr uint32_t kModifierStateWordOffset = 0x1E4;
constexpr uint32_t kModifierPredictedStateWordOffset = 0x234;
constexpr uint32_t kModifierDirtyOffset = 0x1C4;

void SetTunnelModifierStates(uintptr_t modifierProperty,
                             OriginalHullState& original) {
    if (!modifierProperty) return;
    if (!original.modifierCaptured ||
        original.modifierProperty != modifierProperty) {
        original.modifierProperty = modifierProperty;
        original.modifierEnabledState =
            Read<uint32_t>(modifierProperty + kModifierStateWordOffset +
                           sizeof(uint32_t) * (kTinyCharacterState / 32));
        original.modifierTunnelState =
            Read<uint32_t>(modifierProperty + kModifierStateWordOffset +
                           sizeof(uint32_t) * (kTunnelState / 32));
        original.modifierEnabledPredictedState =
            Read<uint32_t>(modifierProperty + kModifierPredictedStateWordOffset +
                           sizeof(uint32_t) * (kTinyCharacterState / 32));
        original.modifierTunnelPredictedState =
            Read<uint32_t>(modifierProperty + kModifierPredictedStateWordOffset +
                           sizeof(uint32_t) * (kTunnelState / 32));
        original.modifierStatesDirty =
            Read<uint8_t>(modifierProperty + kModifierDirtyOffset);
        original.modifierCaptured = true;
    }

    const uint32_t tunnelBit = 1u << (kTunnelState % 32);
    const uintptr_t enabledTunnelWord = modifierProperty + kModifierStateWordOffset +
        sizeof(uint32_t) * (kTunnelState / 32);
    const uintptr_t predictedTunnelWord = modifierProperty +
        kModifierPredictedStateWordOffset +
        sizeof(uint32_t) * (kTunnelState / 32);
    Write<uint32_t>(enabledTunnelWord,
                    Read<uint32_t>(enabledTunnelWord) | tunnelBit);
    Write<uint32_t>(predictedTunnelWord,
                    Read<uint32_t>(predictedTunnelWord) | tunnelBit);
    Write<uint8_t>(modifierProperty + kModifierDirtyOffset, 1);
}

void RestoreTunnelModifierStates(OriginalHullState& original) {
    if (!original.modifierCaptured || !original.modifierProperty) return;
    const uintptr_t enabledTinyWord = original.modifierProperty +
        kModifierStateWordOffset +
        sizeof(uint32_t) * (kTinyCharacterState / 32);
    const uintptr_t enabledTunnelWord = original.modifierProperty +
        kModifierStateWordOffset +
        sizeof(uint32_t) * (kTunnelState / 32);
    const uintptr_t predictedTinyWord = original.modifierProperty +
        kModifierPredictedStateWordOffset +
        sizeof(uint32_t) * (kTinyCharacterState / 32);
    const uintptr_t predictedTunnelWord = original.modifierProperty +
        kModifierPredictedStateWordOffset +
        sizeof(uint32_t) * (kTunnelState / 32);
    Write<uint32_t>(enabledTinyWord, original.modifierEnabledState);
    Write<uint32_t>(enabledTunnelWord, original.modifierTunnelState);
    Write<uint32_t>(predictedTinyWord, original.modifierEnabledPredictedState);
    Write<uint32_t>(predictedTunnelWord, original.modifierTunnelPredictedState);
    Write<uint8_t>(original.modifierProperty + kModifierDirtyOffset,
                   original.modifierStatesDirty);
    original.modifierProperty = 0;
    original.modifierCaptured = false;
}

OriginalHullState originalHull{};
OriginalHullState originalServerHull{};
uintptr_t cachedServerEntitySystem{};
uintptr_t cachedServerModule{};
size_t cachedServerModuleSize{};
ULONGLONG lastServerEntitySystemSearch{};
bool serverHullActive{};
bool tunnelEligibilityActive{};
std::unordered_map<uintptr_t, uint8_t> originalTunnelFlags;
std::unordered_map<uintptr_t, uint8_t> originalServerTunnelFlags;

std::string ReadModuleClassName(
    uintptr_t entity, uintptr_t moduleBase, size_t moduleSize);

void RestoreTunnelEligibility() {
    for (const auto& [entity, original] : originalTunnelFlags) {
        if (GetEntityClassName(entity).find("CitadelTunnelTrigger") ==
            std::string::npos)
            continue;
        Write<uint8_t>(entity + 0xA80, original);
    }
    originalTunnelFlags.clear();
    for (const auto& [entity, original] : originalServerTunnelFlags) {
        if (ReadModuleClassName(
                entity, cachedServerModule, cachedServerModuleSize)
                .find("CitadelTunnelTrigger") == std::string::npos)
            continue;
        Write<uint8_t>(entity + 0x8E9, original);
    }
    originalServerTunnelFlags.clear();
    tunnelEligibilityActive = false;
}

void UpdateTunnelEligibility() {
    if (!remSizedHull || !clientBase) {
        RestoreTunnelEligibility();
        return;
    }

    static ULONGLONG lastScan{};
    const ULONGLONG now = GetTickCount64();
    if (now - lastScan < 100ull) return;
    lastScan = now;

    const uintptr_t root =
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    if (!root) {
        return;
    }
    const int reportedHighest = Read<int>(root + Offsets::HighestEntityIndex);
    const uint32_t highest =
        reportedHighest > 0 &&
        reportedHighest <= static_cast<int>(Offsets::HandleIndexMask)
        ? static_cast<uint32_t>(reportedHighest)
        : Offsets::HandleIndexMask;


    for (uint32_t chunkIndex = 0;
         chunkIndex <= (highest >> Offsets::HandleChunkShift);
         ++chunkIndex) {
        const uintptr_t chunk = Read<uintptr_t>(
            root + Offsets::EntityChunks +
            sizeof(uintptr_t) * chunkIndex);
        if (!chunk) continue;
        const uint32_t slotLimit =
            chunkIndex == (highest >> Offsets::HandleChunkShift)
            ? highest & Offsets::HandleChunkMask
            : Offsets::HandleChunkMask;
        for (uint32_t slot = 0; slot <= slotLimit; ++slot) {
            const uintptr_t identity =
                chunk + Offsets::EntityStride * slot;
            const uint32_t handle = Read<uint32_t>(identity + Offsets::EntityHandleOffset);
            const uint32_t expected =
                (chunkIndex << Offsets::HandleChunkShift) | slot;
            if ((handle & Offsets::HandleIndexMask) != expected) continue;
            const uintptr_t entity = Read<uintptr_t>(identity);
            if (!entity) continue;
            if (GetEntityClassName(entity).find("CitadelTunnelTrigger") ==
                std::string::npos)
                continue;

            if (!originalTunnelFlags.contains(entity)) {
                originalTunnelFlags.emplace(entity,
                    Read<uint8_t>(entity + 0xA80));
            }
            Write<uint8_t>(entity + 0xA80, 0);
            tunnelEligibilityActive = true;
        }
    }
}

void UpdateServerTunnelEligibility(uintptr_t entitySystem) {
    if (!entitySystem || !cachedServerModule) return;
    static ULONGLONG lastScan{};
    const ULONGLONG now = GetTickCount64();
    if (now - lastScan < 100ull) return;
    lastScan = now;
    const int reportedHighest = Read<int>(
        entitySystem + Offsets::HighestEntityIndex);
    const uint32_t highest =
        reportedHighest > 0 &&
        reportedHighest <= static_cast<int>(Offsets::HandleIndexMask)
        ? static_cast<uint32_t>(reportedHighest)
        : Offsets::HandleIndexMask;

    for (uint32_t chunkIndex = 0;
         chunkIndex <= (highest >> Offsets::HandleChunkShift);
         ++chunkIndex) {
        const uintptr_t chunk = Read<uintptr_t>(
            entitySystem + Offsets::EntityChunks +
            sizeof(uintptr_t) * chunkIndex);
        if (!chunk) continue;
        const uint32_t slotLimit =
            chunkIndex == (highest >> Offsets::HandleChunkShift)
            ? highest & Offsets::HandleChunkMask
            : Offsets::HandleChunkMask;
        for (uint32_t slot = 0; slot <= slotLimit; ++slot) {
            const uintptr_t identity =
                chunk + Offsets::EntityStride * slot;
            const uint32_t handle = Read<uint32_t>(identity + Offsets::EntityHandleOffset);
            const uint32_t expected =
                (chunkIndex << Offsets::HandleChunkShift) | slot;
            if ((handle & Offsets::HandleIndexMask) != expected) continue;
            const uintptr_t entity = Read<uintptr_t>(identity);
            if (!entity) continue;
            if (ReadModuleClassName(
                    entity, cachedServerModule, cachedServerModuleSize)
                    .find("CitadelTunnelTrigger") == std::string::npos)
                continue;

            if (!originalServerTunnelFlags.contains(entity)) {
                originalServerTunnelFlags.emplace(
                    entity, Read<uint8_t>(entity + 0x8E9));
            }
            Write<uint8_t>(entity + 0x8E9, 0);
            tunnelEligibilityActive = true;
        }
    }
}

bool IsSaneHull(const Vector3& mins, const Vector3& maxs, float radius) {
    return std::isfinite(mins.x) && std::isfinite(mins.y) &&
        std::isfinite(mins.z) && std::isfinite(maxs.x) &&
        std::isfinite(maxs.y) && std::isfinite(maxs.z) &&
        std::isfinite(radius) &&
        mins.x >= -128.0f && mins.x <= 0.0f &&
        mins.y >= -128.0f && mins.y <= 0.0f &&
        mins.z >= -32.0f && mins.z <= 32.0f &&
        maxs.x >= 1.0f && maxs.x <= 128.0f &&
        maxs.y >= 1.0f && maxs.y <= 128.0f &&
        maxs.z >= 20.0f && maxs.z <= 160.0f &&
        radius >= 1.0f && radius <= 160.0f;
}

uint32_t FindClientEntityIndex(uintptr_t target) {
    if (!target || !clientBase) return 0xFFFFFFFFu;
    const uintptr_t root =
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    if (!root) return 0xFFFFFFFFu;

    for (uint32_t chunkIndex = 0;
         chunkIndex <= (Offsets::HandleIndexMask >>
                        Offsets::HandleChunkShift);
         ++chunkIndex) {
        const uintptr_t chunk = Read<uintptr_t>(
            root + Offsets::EntityChunks +
            sizeof(uintptr_t) * chunkIndex);
        if (!chunk) continue;
        for (uint32_t slot = 0; slot <= Offsets::HandleChunkMask; ++slot) {
            const uintptr_t identity =
                chunk + Offsets::EntityStride * slot;
            if (Read<uintptr_t>(identity) != target) continue;
            return (chunkIndex << Offsets::HandleChunkShift) | slot;
        }
    }
    return 0xFFFFFFFFu;
}

struct SpectatorEntry {
    std::string name;
    bool isEnemy{};
};

int GetObserverTargetIndex(uintptr_t pawn) {
    if (!pawn) return -1;
    const uintptr_t services = Read<uintptr_t>(
        pawn + Offsets::ObserverServices);
    if (!services) return -1;
    // Source 2 can retain the last observer target after the player has
    // stopped spectating. The mode is cleared immediately, so do not treat a
    // stale target handle as an active spectator.
    if (Read<uint8_t>(services + Offsets::ObserverMode) == 0)
        return -1;
    const uint32_t handle = Read<uint32_t>(
        services + Offsets::ObserverTarget);
    if (!handle || handle == 0xFFFFFFFFu) return -1;
    return static_cast<int>(handle & Offsets::HandleIndexMask);
}

bool ObserverCameraTargetsLocal(uintptr_t observerPawn) {
    if (!observerPawn || !currentLocalPawn) return true;
    const uintptr_t services = Read<uintptr_t>(
        observerPawn + Offsets::ObserverServices);
    if (!services) return true;

    const Vector3 cameraTarget = Read<Vector3>(
        services + Offsets::ObserverTargetCameraPos);
    if (!std::isfinite(cameraTarget.x) ||
        !std::isfinite(cameraTarget.y) ||
        !std::isfinite(cameraTarget.z) ||
        (fabsf(cameraTarget.x) < 0.01f &&
         fabsf(cameraTarget.y) < 0.01f &&
         fabsf(cameraTarget.z) < 0.01f))
        return true;

    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }
    if (std::find(pawns.begin(), pawns.end(), currentLocalPawn) ==
        pawns.end())
        pawns.push_back(currentLocalPawn);

    uintptr_t nearestPawn = 0;
    float nearestDistanceSquared = FLT_MAX;
    for (const uintptr_t pawn : pawns) {
        if (!pawn) continue;
        Vector3 position{};
        if (!GetEntityRenderPosition(pawn, position) &&
            !GetEntityPosition(pawn, position))
            continue;
        const float dx = cameraTarget.x - position.x;
        const float dy = cameraTarget.y - position.y;
        const float dz = cameraTarget.z - position.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (!std::isfinite(distanceSquared) ||
            distanceSquared >= nearestDistanceSquared)
            continue;
        nearestDistanceSquared = distanceSquared;
        nearestPawn = pawn;
    }
    return !nearestPawn || nearestPawn == currentLocalPawn;
}

void LogObserverState(uintptr_t controller, uintptr_t pawn,
                      const char* source, int localHeroIndex,
                      int localBaseIndex) {
    if (!controller || !pawn || !source) return;
    const uintptr_t services = Read<uintptr_t>(
        pawn + Offsets::ObserverServices);
    const uint8_t mode = services
        ? Read<uint8_t>(services + Offsets::ObserverMode)
        : 0;
    const uint32_t target = services
        ? Read<uint32_t>(services + Offsets::ObserverTarget)
        : 0xFFFFFFFFu;
    const Vector3 cameraTarget = services
        ? Read<Vector3>(services + Offsets::ObserverTargetCameraPos)
        : Vector3{};
    const uint64_t key =
        (static_cast<uint64_t>(controller) >> 4) ^
        (static_cast<uint64_t>(pawn) << 17);
    uint64_t signature =
        static_cast<uint64_t>(target) |
        (static_cast<uint64_t>(mode) << 32);
    signature ^= static_cast<uint64_t>(std::hash<float>{}(cameraTarget.x));
    signature ^= static_cast<uint64_t>(std::hash<float>{}(cameraTarget.y)) << 1;
    signature ^= static_cast<uint64_t>(std::hash<float>{}(cameraTarget.z)) << 2;
    static std::unordered_map<uint64_t, uint64_t> previous;
    const auto it = previous.find(key);
    if (it != previous.end() && it->second == signature) return;
    previous[key] = signature;

    FILE* log = nullptr;
    if (fopen_s(&log,
                Dll6Paths::DataFileA("spectator_debug.log").c_str(),
                "a") != 0 || !log)
        return;
    const uintptr_t resolvedTarget = ResolveEntity(target);
    const std::string name = ReadPlayerName(controller);
    const std::string pawnClass = GetEntityClassName(pawn);
    const std::string targetClass = GetEntityClassName(resolvedTarget);
    fprintf(log,
            "tick=%llu name=%s controller=%p source=%s pawn=%p "
            "pawnClass=%s services=%p mode=%u target=0x%08X "
            "targetIndex=%d targetEntity=%p targetClass=%s "
            "cameraTarget=(%.2f,%.2f,%.2f) cameraSaysLocal=%d "
            "localHeroIndex=%d localBaseIndex=%d\n",
            static_cast<unsigned long long>(GetTickCount64()),
            name.empty() ? "<empty>" : name.c_str(),
            reinterpret_cast<void*>(controller), source,
            reinterpret_cast<void*>(pawn),
            pawnClass.empty() ? "<unknown>" : pawnClass.c_str(),
            reinterpret_cast<void*>(services),
            static_cast<unsigned>(mode), target,
            target == 0xFFFFFFFFu
                ? -1
                : static_cast<int>(target & Offsets::HandleIndexMask),
            reinterpret_cast<void*>(resolvedTarget),
            targetClass.empty() ? "<unknown>" : targetClass.c_str(),
            cameraTarget.x, cameraTarget.y, cameraTarget.z,
            ObserverCameraTargetsLocal(pawn) ? 1 : 0,
            localHeroIndex, localBaseIndex);
    fclose(log);
}

std::vector<SpectatorEntry> CollectCurrentSpectators() {
    std::vector<SpectatorEntry> result;
    if (!clientBase) return result;

    const uintptr_t entitySystem =
        Read<uintptr_t>(clientBase + Offsets::GameEntitySystem);
    if (!entitySystem) return result;

    uintptr_t localController = 0;
    std::vector<uintptr_t> controllers;
    std::vector<uintptr_t> observerPawns;
    std::unordered_set<uintptr_t> seen;
    const int reportedHighest = Read<int>(
        entitySystem + Offsets::HighestEntityIndex);
    const uint32_t highest =
        reportedHighest > 0 &&
        reportedHighest <= static_cast<int>(Offsets::HandleIndexMask)
        ? static_cast<uint32_t>(reportedHighest)
        : Offsets::HandleIndexMask;

    for (uint32_t chunkIndex = 0;
         chunkIndex <= (highest >> Offsets::HandleChunkShift);
         ++chunkIndex) {
        const uintptr_t chunk = Read<uintptr_t>(
            entitySystem + Offsets::EntityChunks +
            Offsets::EntityChunkStride * chunkIndex);
        if (!chunk) continue;
        const uint32_t slotLimit =
            chunkIndex == (highest >> Offsets::HandleChunkShift)
            ? highest & Offsets::HandleChunkMask
            : Offsets::HandleChunkMask;
        for (uint32_t slot = 0; slot <= slotLimit; ++slot) {
            const uintptr_t identity =
                chunk + Offsets::EntityStride * slot;
            const uintptr_t entity = Read<uintptr_t>(identity);
            if (!entity || !seen.insert(entity).second) continue;
            const uint32_t expectedIndex =
                (chunkIndex << Offsets::HandleChunkShift) | slot;
            const uint32_t handle = Read<uint32_t>(
                identity + Offsets::EntityHandleOffset);
            if ((handle & Offsets::HandleIndexMask) != expectedIndex) continue;
            const std::string className = GetEntityClassName(entity);
            if (className.find("PlayerController") != std::string::npos) {
                controllers.push_back(entity);
                if (Read<uint8_t>(
                        entity + Offsets::IsLocalPlayerController) == 1) {
                    localController = entity;
                }
            } else if (className.find("ObserverPawn") != std::string::npos) {
                observerPawns.push_back(entity);
            }
        }
    }

    if (!localController && currentLocalPawn) {
        localController = ResolveEntity(Read<uint32_t>(
            currentLocalPawn + Offsets::PawnController));
    }
    if (!localController) return result;

    const uint32_t localBaseHandle = Read<uint32_t>(
        localController + Offsets::ControllerPawn);
    const int localHeroIndex =
        currentLocalPawnHandle && currentLocalPawnHandle != 0xFFFFFFFFu
        ? static_cast<int>(currentLocalPawnHandle & Offsets::HandleIndexMask)
        : currentLocalPawn
            ? static_cast<int>(FindClientEntityIndex(currentLocalPawn))
            : -1;
    const int localBaseIndex =
        localBaseHandle && localBaseHandle != 0xFFFFFFFFu
        ? static_cast<int>(localBaseHandle & Offsets::HandleIndexMask)
        : -1;
    const uint8_t localTeam = Read<uint8_t>(
        localController + Offsets::Team);

    for (const uintptr_t controller : controllers) {
        if (!controller || controller == localController ||
            Read<uint8_t>(
                controller + Offsets::IsLocalPlayerController) == 1)
            continue;

        const uintptr_t controllerPawn = ResolveEntity(Read<uint32_t>(
            controller + Offsets::ControllerPawn));
        uintptr_t activeObserverPawn = 0;

        LogObserverState(controller, controllerPawn, "controllerPawn",
                         localHeroIndex, localBaseIndex);

        // In current Deadlock builds m_hPawn may remain the dead hero/base
        // pawn while a separate C_CitadelObserverPawn owns the live camera
        // target. Prefer that dedicated pawn and never OR its result with the
        // stale target retained by the old pawn.
        if (controllerPawn &&
            GetEntityClassName(controllerPawn).find("ObserverPawn") !=
                std::string::npos) {
            activeObserverPawn = controllerPawn;
        } else {
            for (const uintptr_t observerPawn : observerPawns) {
                const uintptr_t owner = ResolveEntity(Read<uint32_t>(
                    observerPawn + Offsets::PawnController));
                if (owner != controller) continue;
                LogObserverState(controller, observerPawn,
                                 "dedicatedObserverPawn",
                                 localHeroIndex, localBaseIndex);
                activeObserverPawn = observerPawn;
                break;
            }
        }

        // Fall back only when this controller has no dedicated observer pawn.
        // Once one exists, an invalid target means the player is not currently
        // watching another player and must disappear from the list.
        const uintptr_t observerPawn = activeObserverPawn
            ? activeObserverPawn
            : controllerPawn;
        const int observerTargetIndex =
            GetObserverTargetIndex(observerPawn);
        if (observerTargetIndex < 0) continue;

        const bool watchingHero = observerTargetIndex == localHeroIndex;
        const bool watchingBase = observerTargetIndex == localBaseIndex;
        if (!watchingHero && !watchingBase)
            continue;
        if (!ObserverCameraTargetsLocal(observerPawn))
            continue;

        std::string name = ReadPlayerName(controller);
        if (name.empty()) name = "Unknown";
        result.push_back(SpectatorEntry{
            std::move(name),
            Read<uint8_t>(controller + Offsets::Team) != localTeam});
    }
    return result;
}

std::string ReadModuleClassName(
    uintptr_t entity, uintptr_t moduleBase, size_t moduleSize) {
    if (!entity || !moduleBase || !moduleSize) return {};
    const uintptr_t vtable = Read<uintptr_t>(entity);
    if (vtable < moduleBase || vtable >= moduleBase + moduleSize) return {};
    const uintptr_t locator = Read<uintptr_t>(vtable - sizeof(uintptr_t));
    if (locator < moduleBase || locator >= moduleBase + moduleSize) return {};
    const uint32_t typeRva = Read<uint32_t>(locator + 0x0C);
    if (typeRva >= moduleSize) return {};

    std::string result;
    result.reserve(96);
    for (uintptr_t i = 0; i < 96; ++i) {
        const char c = Read<char>(moduleBase + typeRva + 0x10 + i);
        if (!c) break;
        if (static_cast<unsigned char>(c) < 0x20 ||
            static_cast<unsigned char>(c) > 0x7E) return {};
        result.push_back(c);
    }
    return result;
}

uintptr_t ResolveServerEntityIndex(
    uintptr_t entitySystem, uint32_t index) {
    if (!entitySystem || index > Offsets::HandleIndexMask) return 0;
    const uintptr_t chunk = Read<uintptr_t>(
        entitySystem + Offsets::EntityChunks +
        sizeof(uintptr_t) * (index >> Offsets::HandleChunkShift));
    if (!chunk) return 0;
    const uintptr_t identity =
        chunk + Offsets::EntityStride *
        (index & Offsets::HandleChunkMask);
    const uint32_t storedHandle = Read<uint32_t>(identity + Offsets::EntityHandleOffset);
    if ((storedHandle & Offsets::HandleIndexMask) != index) return 0;
    return Read<uintptr_t>(identity);
}

bool ValidateServerPawn(
    uintptr_t entitySystem, uint32_t index, uintptr_t& pawn,
    uintptr_t& collision) {
    pawn = ResolveServerEntityIndex(entitySystem, index);
    if (!pawn) return false;
    const std::string className = ReadModuleClassName(
        pawn, cachedServerModule, cachedServerModuleSize);
    if (className.find("CCitadelPlayerPawn") == std::string::npos)
        return false;

    // The server CBaseEntity layout is not the client layout. In the current
    // server build m_pCollision is +0x3C8 and points at the model entity's
    // embedded CCollisionProperty at +0x5F8.
    collision = Read<uintptr_t>(pawn + 0x3C8);
    if (!collision || collision != pawn + 0x5F8) return false;
    const Vector3 mins = Read<Vector3>(collision + Offsets::CollisionMins);
    const Vector3 maxs = Read<Vector3>(collision + Offsets::CollisionMaxs);
    const float radius = Read<float>(collision + 0x60);
    return IsSaneHull(mins, maxs, radius);
}

uintptr_t FindServerEntitySystem(uint32_t localIndex) {
    const HMODULE server = GetModuleHandleW(L"server.dll");
    if (!server) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(
            GetCurrentProcess(), server, &info, sizeof(info)))
        return 0;

    cachedServerModule = reinterpret_cast<uintptr_t>(server);
    cachedServerModuleSize = info.SizeOfImage;
    static uintptr_t scannedModule{};
    static size_t scannedModuleSize{};
    static std::vector<uintptr_t> candidateGlobals;
    if (scannedModule != cachedServerModule ||
        scannedModuleSize != cachedServerModuleSize) {
        scannedModule = cachedServerModule;
        scannedModuleSize = cachedServerModuleSize;
        candidateGlobals.clear();
        const auto* image = reinterpret_cast<const uint8_t*>(
            cachedServerModule);
        constexpr uint8_t pattern[] = {
            0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x0D
        };

        for (size_t i = 0;
             i + sizeof(pattern) + sizeof(int32_t) <
                 cachedServerModuleSize;
             ++i) {
            bool matches = true;
            for (size_t j = 0; j < sizeof(pattern); ++j) {
                if (image[i + j] != pattern[j]) {
                    matches = false;
                    break;
                }
            }
            if (!matches) continue;

            const int32_t displacement =
                Read<int32_t>(
                    cachedServerModule + i + sizeof(pattern));
            const uintptr_t globalAddress =
                cachedServerModule + i + sizeof(pattern) +
                sizeof(displacement) + displacement;
            if (globalAddress < cachedServerModule ||
                globalAddress + sizeof(uintptr_t) >
                    cachedServerModule + cachedServerModuleSize)
                continue;
            if (std::find(
                    candidateGlobals.begin(), candidateGlobals.end(),
                    globalAddress) == candidateGlobals.end())
                candidateGlobals.push_back(globalAddress);
        }
    }

    for (const uintptr_t globalAddress : candidateGlobals) {
        const uintptr_t candidate = Read<uintptr_t>(globalAddress);
        uintptr_t pawn{};
        uintptr_t collision{};
        if (ValidateServerPawn(
                candidate, localIndex, pawn, collision))
            return candidate;
    }
    return 0;
}

CollisionDiagnosticSnapshot ReadCollisionDiagnostic() {
    CollisionDiagnosticSnapshot snapshot{};
    const uintptr_t pawn = currentLocalPawn;
    const uintptr_t collision =
        pawn ? Read<uintptr_t>(pawn + Offsets::CollisionProperty) : 0;
    if (!pawn || !collision) return snapshot;

    snapshot.hero = ReadHeroName(pawn);
    GetEntityPosition(pawn, snapshot.position);
    snapshot.mins = Read<Vector3>(collision + 0x40);
    snapshot.maxs = Read<Vector3>(collision + 0x4C);
    snapshot.solidFlags = Read<uint8_t>(collision + 0x5A);
    snapshot.solidType = Read<uint8_t>(collision + 0x5B);
    snapshot.collisionGroup = Read<uint8_t>(collision + 0x5E);
    snapshot.boundingRadius = Read<float>(collision + 0x60);
    snapshot.specifiedMins = Read<Vector3>(collision + 0x64);
    snapshot.specifiedMaxs = Read<Vector3>(collision + 0x70);
    snapshot.surroundingMaxs = Read<Vector3>(collision + 0x7C);
    snapshot.surroundingMins = Read<Vector3>(collision + 0x88);
    snapshot.capsuleCenter1 = Read<Vector3>(collision + 0x94);
    snapshot.capsuleCenter2 = Read<Vector3>(collision + 0xA0);
    snapshot.capsuleRadius = Read<float>(collision + 0xAC);
    snapshot.flags = Read<uint32_t>(pawn + 0x400);
    snapshot.moveType = Read<uint8_t>(pawn + 0x521);
    snapshot.actualMoveType = Read<uint8_t>(pawn + 0x522);

    const uintptr_t movement = Read<uintptr_t>(pawn + 0xF28);
    if (movement) {
        snapshot.toggleDuck = Read<bool>(movement + 0x2A0);
        snapshot.ducked = Read<bool>(movement + 0x2A1);
        snapshot.colliding = Read<bool>(movement + 0x2BC);
    }
    snapshot.valid =
        std::isfinite(snapshot.mins.x) &&
        std::isfinite(snapshot.maxs.z) &&
        std::isfinite(snapshot.capsuleRadius);
    return snapshot;
}

void UpdateCollisionDiagnostic() {
    if (!collisionDiagnostics) return;

    static ULONGLONG lastSample = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastSample < 100) return;
    lastSample = now;

    collisionSnapshot = ReadCollisionDiagnostic();
    if (!collisionSnapshot.valid) return;

    std::ofstream log(
        Dll6Paths::DataFileA("collision_diagnostic.csv"),
        std::ios::app);
    if (!log) return;
    static bool headerWritten = false;
    if (!headerWritten) {
        log << "time_ms,hero,pos_x,pos_y,pos_z,mins_x,mins_y,mins_z,"
               "maxs_x,maxs_y,maxs_z,bounding_radius,capsule_radius,"
               "cap1_x,cap1_y,cap1_z,cap2_x,cap2_y,cap2_z,"
               "specified_min_x,specified_min_y,specified_min_z,"
               "specified_max_x,specified_max_y,specified_max_z,"
               "surrounding_min_x,surrounding_min_y,surrounding_min_z,"
               "surrounding_max_x,surrounding_max_y,surrounding_max_z,"
               "flags,move_type,actual_move_type,solid_flags,solid_type,"
               "collision_group,toggle_duck,ducked,colliding\n";
        headerWritten = true;
    }
    const auto& s = collisionSnapshot;
    log << now << ',' << s.hero << ','
        << s.position.x << ',' << s.position.y << ',' << s.position.z << ','
        << s.mins.x << ',' << s.mins.y << ',' << s.mins.z << ','
        << s.maxs.x << ',' << s.maxs.y << ',' << s.maxs.z << ','
        << s.boundingRadius << ',' << s.capsuleRadius << ','
        << s.capsuleCenter1.x << ',' << s.capsuleCenter1.y << ',' << s.capsuleCenter1.z << ','
        << s.capsuleCenter2.x << ',' << s.capsuleCenter2.y << ',' << s.capsuleCenter2.z << ','
        << s.specifiedMins.x << ',' << s.specifiedMins.y << ',' << s.specifiedMins.z << ','
        << s.specifiedMaxs.x << ',' << s.specifiedMaxs.y << ',' << s.specifiedMaxs.z << ','
        << s.surroundingMins.x << ',' << s.surroundingMins.y << ',' << s.surroundingMins.z << ','
        << s.surroundingMaxs.x << ',' << s.surroundingMaxs.y << ',' << s.surroundingMaxs.z << ','
        << s.flags << ',' << static_cast<unsigned>(s.moveType) << ','
        << static_cast<unsigned>(s.actualMoveType) << ','
        << static_cast<unsigned>(s.solidFlags) << ','
        << static_cast<unsigned>(s.solidType) << ','
        << static_cast<unsigned>(s.collisionGroup) << ','
        << s.toggleDuck << ',' << s.ducked << ',' << s.colliding << '\n';
}

// The camera object still lives at the old data anchor.  Its view and
// projection matrices are stored consecutively in the render-camera state.
bool ReadCurrentViewMatrix(Matrix4x4& matrix) {
    if (!clientBase) return false;

    const uintptr_t camera = clientBase + Offsets::ViewMatrix;
    // The camera state publishes the matrix actually consumed by rendering
    // immediately after the view and projection matrices. Reading view and
    // projection separately and composing them here races the camera update:
    // both inputs can be individually valid while belonging to different
    // render publications. That phase mismatch moves the whole ESP whenever
    // the camera follows a moving pawn.
    const uintptr_t publishedMatrixAddress =
        camera + Offsets::ViewMatrixProjection + sizeof(Matrix4x4);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Matrix4x4 first =
            Read<Matrix4x4>(publishedMatrixAddress);
        const Matrix4x4 published =
            Read<Matrix4x4>(publishedMatrixAddress);
        if (std::memcmp(&first, &published, sizeof(published)) != 0)
            continue;

        bool finite = true;
        for (int row = 0; row < 4 && finite; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!std::isfinite(published.m[row][column])) {
                    finite = false;
                    break;
                }
            }
        }
        const float perspectiveLengthSquared =
            published.m[3][0] * published.m[3][0] +
            published.m[3][1] * published.m[3][1] +
            published.m[3][2] * published.m[3][2];
        if (finite && std::isfinite(perspectiveLengthSquared) &&
            perspectiveLengthSquared > 0.0001f) {
            matrix = published;
            return true;
        }
    }

    // The live client publishes a complete view-projection matrix at +0x80.
    // Never switch ESP to a separately composed pair from another update
    // phase; skip this frame if the complete publication was torn.
    return false;

    // Compatibility fallback for a camera layout that does not expose the
    // published matrix at the expected adjacent slot.
    // The camera hook and Present may run on different threads. A view matrix
    // from one camera update combined with the projection from the next is
    // still finite, but shifts every ESP primitive for one frame. Require two
    // consecutive complete snapshots to match before composing them.
    for (int attempt = 0; attempt < 6; ++attempt) {
        const Matrix4x4 firstView = Read<Matrix4x4>(
            camera + Offsets::ViewMatrixView);
        const Matrix4x4 firstProjection = Read<Matrix4x4>(
            camera + Offsets::ViewMatrixProjection);
        const Matrix4x4 view = Read<Matrix4x4>(
            camera + Offsets::ViewMatrixView);
        const Matrix4x4 projection = Read<Matrix4x4>(
            camera + Offsets::ViewMatrixProjection);
        const bool changedDuringRead =
            std::memcmp(&firstView, &view, sizeof(view)) != 0 ||
            std::memcmp(&firstProjection, &projection,
                        sizeof(projection)) != 0;
        // Never compose a view/projection pair that changed during the read.
        // The previous code accepted the final torn pair after five retries,
        // producing an occasional one-frame jump of the entire ESP.
        if (changedDuringRead) continue;

        bool finite = true;
        for (int row = 0; row < 4 && finite; ++row) {
            for (int column = 0; column < 4; ++column) {
                if (!std::isfinite(view.m[row][column]) ||
                    !std::isfinite(projection.m[row][column])) {
                    finite = false;
                    break;
                }
            }
        }
        if (!finite) continue;

        Matrix4x4 composed{};
        // WorldToScreen consumes a column-vector clip matrix, so compose P * V.
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column) {
                for (int k = 0; k < 4; ++k)
                    composed.m[row][column] +=
                        projection.m[row][k] * view.m[k][column];
            }
        }
        // clip.w uses the complete fourth row. Testing only m[3][2] makes a
        // perfectly valid matrix fail whenever camera pitch approaches 0°:
        // that component naturally crosses zero while the horizontal forward
        // components remain valid. This was the exact point where Normal/
        // Mixed aim made every ESP element disappear.
        const float perspectiveLengthSquared =
            composed.m[3][0] * composed.m[3][0] +
            composed.m[3][1] * composed.m[3][1] +
            composed.m[3][2] * composed.m[3][2];
        if (!std::isfinite(perspectiveLengthSquared) ||
            perspectiveLengthSquared <= 0.0001f)
            continue;
        matrix = composed;
        return true;
    }
    return false;
}

bool ReadCameraWorldPosition(const Matrix4x4& viewProjection,
                             Vector3& position) {
    Matrix4x4 inverse{};
    if (InvertMatrix(viewProjection, inverse)) {
        const auto unproject = [&inverse](float clipX, float clipY,
                                           float clipZ, Vector3& world) {
            const float x = inverse.m[0][0] * clipX +
                inverse.m[0][1] * clipY + inverse.m[0][2] * clipZ +
                inverse.m[0][3];
            const float y = inverse.m[1][0] * clipX +
                inverse.m[1][1] * clipY + inverse.m[1][2] * clipZ +
                inverse.m[1][3];
            const float z = inverse.m[2][0] * clipX +
                inverse.m[2][1] * clipY + inverse.m[2][2] * clipZ +
                inverse.m[2][3];
            const float w = inverse.m[3][0] * clipX +
                inverse.m[3][1] * clipY + inverse.m[3][2] * clipZ +
                inverse.m[3][3];
            if (!std::isfinite(w) || std::fabs(w) < 1e-6f) return false;
            world = {x / w, y / w, z / w};
            return std::isfinite(world.x) && std::isfinite(world.y) &&
                std::isfinite(world.z);
        };

        // A perspective camera is the intersection of any two unprojected
        // screen rays. Recover it from the same coherent VP publication used
        // by WorldToScreen instead of the obsolete, currently zeroed +0xC0
        // CameraOrigin slot.
        Vector3 a0{}, a1{}, b0{}, b1{};
        if (unproject(0.0f, 0.0f, 0.0f, a0) &&
            unproject(0.0f, 0.0f, 0.5f, a1) &&
            unproject(0.5f, 0.0f, 0.0f, b0) &&
            unproject(0.5f, 0.0f, 0.5f, b1)) {
            const Vector3 u{a1.x - a0.x, a1.y - a0.y, a1.z - a0.z};
            const Vector3 v{b1.x - b0.x, b1.y - b0.y, b1.z - b0.z};
            const Vector3 w{a0.x - b0.x, a0.y - b0.y, a0.z - b0.z};
            const auto dot = [](const Vector3& left, const Vector3& right) {
                return static_cast<double>(left.x) * right.x +
                    static_cast<double>(left.y) * right.y +
                    static_cast<double>(left.z) * right.z;
            };
            const double uu = dot(u, u);
            const double uv = dot(u, v);
            const double vv = dot(v, v);
            const double uw = dot(u, w);
            const double vw = dot(v, w);
            const double denominator = uu * vv - uv * uv;
            if (std::isfinite(denominator) &&
                std::fabs(denominator) > 1e-12) {
                const double ta = (uv * vw - vv * uw) / denominator;
                const double tb = (uu * vw - uv * uw) / denominator;
                const Vector3 onA{
                    a0.x + u.x * static_cast<float>(ta),
                    a0.y + u.y * static_cast<float>(ta),
                    a0.z + u.z * static_cast<float>(ta)};
                const Vector3 onB{
                    b0.x + v.x * static_cast<float>(tb),
                    b0.y + v.y * static_cast<float>(tb),
                    b0.z + v.z * static_cast<float>(tb)};
                position = {(onA.x + onB.x) * 0.5f,
                            (onA.y + onB.y) * 0.5f,
                            (onA.z + onB.z) * 0.5f};
                if (std::isfinite(position.x) &&
                    std::isfinite(position.y) &&
                    std::isfinite(position.z))
                    return true;
            }
        }
    }

    if (!clientBase) return false;
    position = Read<Vector3>(
        clientBase + Offsets::ViewMatrix + Offsets::CameraOrigin);
    return std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z) &&
        std::fabs(position.x) + std::fabs(position.y) +
            std::fabs(position.z) > 0.001f;
}

bool GetCurrentCameraForwardImpl(Vector3& forward) {
    if (!currentViewMatrixReady) return false;
    Matrix4x4 inverse{};
    if (!InvertMatrix(currentViewMatrix, inverse)) return false;
    auto unproject = [&](float depth, Vector3& point) {
        const float x = inverse.m[0][2] * depth + inverse.m[0][3];
        const float y = inverse.m[1][2] * depth + inverse.m[1][3];
        const float z = inverse.m[2][2] * depth + inverse.m[2][3];
        const float w = inverse.m[3][2] * depth + inverse.m[3][3];
        if (!std::isfinite(w) || std::fabs(w) < 1e-6f) return false;
        point = {x / w, y / w, z / w};
        return std::isfinite(point.x) && std::isfinite(point.y) &&
               std::isfinite(point.z);
    };
    Vector3 nearPoint{}, farPoint{};
    if (!unproject(0.0f, nearPoint) || !unproject(1.0f, farPoint)) return false;
    const Vector3 delta{farPoint.x - nearPoint.x,
                        farPoint.y - nearPoint.y,
                        farPoint.z - nearPoint.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                    delta.z * delta.z);
    if (!std::isfinite(length) || length < 1e-4f) return false;
    forward = {delta.x / length, delta.y / length, delta.z / length};
    return true;
}

void RestoreStoredHull() {
    if (!originalHull.captured) return;
    if (Read<uintptr_t>(
            originalHull.pawn + Offsets::CollisionProperty) ==
        originalHull.collision) {
        Write<Vector3>(
            originalHull.collision + Offsets::CollisionMaxs,
            originalHull.maxs);
        Write<Vector3>(originalHull.collision + 0x64,
                       originalHull.specifiedMins);
        Write<Vector3>(originalHull.collision + 0x70,
                       originalHull.specifiedMaxs);
        Write<Vector3>(originalHull.collision + 0x7C,
                       originalHull.surroundingMaxs);
        Write<Vector3>(originalHull.collision + 0x88,
                       originalHull.surroundingMins);
        Write<float>(
            originalHull.collision + 0x60,
            originalHull.boundingRadius);
    }
    RestoreTunnelModifierStates(originalHull);
    originalHull = {};
}

void RestoreStoredServerHull() {
    if (!originalServerHull.captured) return;
    if (Read<uintptr_t>(originalServerHull.pawn + 0x3C8) ==
        originalServerHull.collision) {
        Write<Vector3>(
            originalServerHull.collision + Offsets::CollisionMaxs,
            originalServerHull.maxs);
        Write<Vector3>(originalServerHull.collision + 0x64,
                       originalServerHull.specifiedMins);
        Write<Vector3>(originalServerHull.collision + 0x70,
                       originalServerHull.specifiedMaxs);
        Write<Vector3>(originalServerHull.collision + 0x7C,
                       originalServerHull.surroundingMaxs);
        Write<Vector3>(originalServerHull.collision + 0x88,
                       originalServerHull.surroundingMins);
        Write<float>(
            originalServerHull.collision + 0x60,
            originalServerHull.boundingRadius);
    }
    RestoreTunnelModifierStates(originalServerHull);
    originalServerHull = {};
}

}

namespace {
bool IsUsableEspPosition(const Vector3& position) {
    return std::isfinite(position.x) && std::isfinite(position.y) &&
           std::isfinite(position.z) &&
           std::fabs(position.x) < 100000.0f &&
           std::fabs(position.y) < 100000.0f &&
           std::fabs(position.z) < 100000.0f &&
           (std::fabs(position.x) > 0.01f ||
            std::fabs(position.y) > 0.01f ||
            std::fabs(position.z) > 0.01f);
}

bool GetEspFramePosition(uintptr_t entity, Vector3& position) {
    // ESP must follow exactly one coordinate source. m_nodeToWorld is the
    // transform consumed by scene rendering; AbsOrigin and m_vRenderOrigin
    // are published at different phases and mixing them produces a visible
    // forward/back snap. If this render transform is temporarily unavailable,
    // skip the entity for this frame instead of drawing it at another phase.
    return GetEntityRenderTransformPosition(entity, position) &&
           IsUsableEspPosition(position);
}

struct EspScreenTrack {
    float centerX{};
    float centerY{};
    float velocityX{};
    float velocityY{};
    float rawCenterX{};
    float rawCenterY{};
    float width{};
    float height{};
    ULONGLONG rawChangedAt{};
    ULONGLONG lastSeenAt{};
    bool initialized{};
};

std::unordered_map<uintptr_t, EspScreenTrack> espScreenTracks;

bool StabilizeEspScreenBox(uintptr_t entity, float rawLeft, float rawTop,
                           float rawRight, float rawBottom,
                           float& left, float& top,
                           float& right, float& bottom) {
    if (!entity || !std::isfinite(rawLeft) || !std::isfinite(rawTop) ||
        !std::isfinite(rawRight) || !std::isfinite(rawBottom) ||
        rawRight <= rawLeft || rawBottom <= rawTop)
        return false;

    const float rawCenterX = (rawLeft + rawRight) * 0.5f;
    const float rawCenterY = (rawTop + rawBottom) * 0.5f;
    const float rawWidth = rawRight - rawLeft;
    const float rawHeight = rawBottom - rawTop;
    const ULONGLONG now = GetTickCount64();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.050f);
    auto& track = espScreenTracks[entity];

    const float oldRawDx = rawCenterX - track.rawCenterX;
    const float oldRawDy = rawCenterY - track.rawCenterY;
    const float oldRawDistance =
        std::sqrt(oldRawDx * oldRawDx + oldRawDy * oldRawDy);
    const bool reset = !track.initialized ||
        now - track.lastSeenAt > 150 || !std::isfinite(oldRawDistance) ||
        oldRawDistance > 500.0f ||
        std::fabs(rawWidth - track.width) > 300.0f ||
        std::fabs(rawHeight - track.height) > 500.0f;
    if (reset) {
        track.centerX = track.rawCenterX = rawCenterX;
        track.centerY = track.rawCenterY = rawCenterY;
        track.velocityX = track.velocityY = 0.0f;
        track.width = rawWidth;
        track.height = rawHeight;
        track.rawChangedAt = now;
        track.lastSeenAt = now;
        track.initialized = true;
    } else {
        // The camera matrix can remain unchanged for several Presents and
        // then jump on a gameplay-camera publication. Estimate velocity only
        // when a genuinely new screen sample arrives; duplicate Presents must
        // not repeatedly pull the velocity estimate toward zero.
        if (oldRawDistance > 0.05f) {
            const ULONGLONG elapsedMs = now - track.rawChangedAt;
            if (elapsedMs >= 2 && elapsedMs <= 100) {
                const float inverseSeconds =
                    1000.0f / static_cast<float>(elapsedMs);
                const float measuredVelocityX = oldRawDx * inverseSeconds;
                const float measuredVelocityY = oldRawDy * inverseSeconds;
                const float measuredSpeed = std::hypot(
                    measuredVelocityX, measuredVelocityY);
                if (std::isfinite(measuredSpeed) && measuredSpeed < 50000.0f) {
                    constexpr float velocityBlend = 0.65f;
                    track.velocityX +=
                        (measuredVelocityX - track.velocityX) * velocityBlend;
                    track.velocityY +=
                        (measuredVelocityY - track.velocityY) * velocityBlend;
                }
            }
            track.rawCenterX = rawCenterX;
            track.rawCenterY = rawCenterY;
            track.rawChangedAt = now;
        } else if (now - track.rawChangedAt > 80) {
            const float velocityDecay = std::exp(-18.0f * dt);
            track.velocityX *= velocityDecay;
            track.velocityY *= velocityDecay;
        }

        // Correct directly toward the latest primary-swap-chain sample.
        // Do not extrapolate screen velocity: camera rotation is part of the
        // screen displacement, and treating it as pawn velocity caused the
        // filter to overshoot and amplify a matrix jump.
        const float residualX = rawCenterX - track.centerX;
        const float residualY = rawCenterY - track.centerY;
        const float residual = std::hypot(residualX, residualY);
        const float responseRate = 90.0f +
            (std::min)(residual * 4.0f, 130.0f);
        const float centerAlpha = 1.0f - std::exp(-responseRate * dt);
        track.centerX += residualX * centerAlpha;
        track.centerY += residualY * centerAlpha;

        // Width/height have no useful directional velocity. A fast
        // frame-rate-independent response removes capsule publication noise
        // without visibly changing the box size relative to the model.
        const float sizeAlpha = 1.0f - std::exp(-90.0f * dt);
        track.width += (rawWidth - track.width) * sizeAlpha;
        track.height += (rawHeight - track.height) * sizeAlpha;

        track.lastSeenAt = now;
    }

    const float halfWidth = track.width * 0.5f;
    const float halfHeight = track.height * 0.5f;
    left = track.centerX - halfWidth;
    right = track.centerX + halfWidth;
    top = track.centerY - halfHeight;
    bottom = track.centerY + halfHeight;

    static uint32_t cleanupCounter = 0;
    if ((++cleanupCounter & 1023u) == 0u) {
        for (auto it = espScreenTracks.begin();
             it != espScreenTracks.end();) {
            if (now - it->second.lastSeenAt > 2000)
                it = espScreenTracks.erase(it);
            else
                ++it;
        }
    }
    return std::isfinite(left) && std::isfinite(top) &&
           std::isfinite(right) && std::isfinite(bottom) &&
           right > left && bottom > top;
}
}

void RestoreRemSizedHull() {
    RestoreTunnelEligibility();
    RestoreStoredHull();
    RestoreStoredServerHull();
    cachedServerEntitySystem = 0;
    serverHullActive = false;
}

void UpdateRemSizedHull() {
    serverHullActive = false;
    UpdateTunnelEligibility();
    if (!remSizedHull || !currentLocalPawn) {
        RestoreStoredHull();
        RestoreStoredServerHull();
        cachedServerEntitySystem = 0;
        return;
    }

    const uintptr_t collision =
        Read<uintptr_t>(currentLocalPawn + Offsets::CollisionProperty);
    if (!collision) {
        RestoreStoredHull();
        return;
    }

    if (!originalHull.captured ||
        originalHull.pawn != currentLocalPawn ||
        originalHull.collision != collision) {
        RestoreStoredHull();
        originalHull.pawn = currentLocalPawn;
        originalHull.collision = collision;
        originalHull.maxs =
            Read<Vector3>(collision + Offsets::CollisionMaxs);
        originalHull.specifiedMins = Read<Vector3>(collision + 0x64);
        originalHull.specifiedMaxs = Read<Vector3>(collision + 0x70);
        originalHull.surroundingMaxs = Read<Vector3>(collision + 0x7C);
        originalHull.surroundingMins = Read<Vector3>(collision + 0x88);
        originalHull.boundingRadius = Read<float>(collision + 0x60);
        originalHull.solidFlags = Read<uint8_t>(collision + 0x5A);
        originalHull.solidType = Read<uint8_t>(collision + 0x5B);
        originalHull.captured =
            std::isfinite(originalHull.maxs.z) &&
            originalHull.maxs.z >= 40.0f &&
            std::isfinite(originalHull.boundingRadius);
    }
    if (!originalHull.captured) return;

    Vector3 remMaxs = originalHull.maxs;
    remMaxs.x = 16.0f;
    remMaxs.y = 16.0f;
    remMaxs.z = 40.0f;
    Write<Vector3>(collision + Offsets::CollisionMaxs, remMaxs);
    const Vector3 remMins{-16.0f, -16.0f, 0.0f};
    Write<Vector3>(collision + 0x64, remMins);
    Write<Vector3>(collision + 0x70, remMaxs);
    Write<Vector3>(collision + 0x7C, remMaxs);
    Write<Vector3>(collision + 0x88, remMins);
    Write<float>(collision + 0x60, 30.1993f);
    SetTunnelModifierStates(
        Read<uintptr_t>(currentLocalPawn + 0x348), originalHull);

    static uintptr_t indexedClientPawn{};
    static uint32_t localIndex = 0xFFFFFFFFu;
    if (indexedClientPawn != currentLocalPawn) {
        RestoreStoredServerHull();
        cachedServerEntitySystem = 0;
        indexedClientPawn = currentLocalPawn;
        localIndex = FindClientEntityIndex(currentLocalPawn);
    }
    if (localIndex == 0xFFFFFFFFu) return;

    uintptr_t serverPawn{};
    uintptr_t serverCollision{};
    if (!ValidateServerPawn(
            cachedServerEntitySystem, localIndex,
            serverPawn, serverCollision)) {
        RestoreStoredServerHull();
        cachedServerEntitySystem = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastServerEntitySystemSearch < 1000ull) return;
        lastServerEntitySystemSearch = now;
        cachedServerEntitySystem = FindServerEntitySystem(localIndex);
        if (!ValidateServerPawn(
                cachedServerEntitySystem, localIndex,
                serverPawn, serverCollision))
            return;
    }

    UpdateServerTunnelEligibility(cachedServerEntitySystem);

    if (!originalServerHull.captured ||
        originalServerHull.pawn != serverPawn ||
        originalServerHull.collision != serverCollision) {
        RestoreStoredServerHull();
        originalServerHull.pawn = serverPawn;
        originalServerHull.collision = serverCollision;
        originalServerHull.maxs =
            Read<Vector3>(
                serverCollision + Offsets::CollisionMaxs);
        originalServerHull.specifiedMins =
            Read<Vector3>(serverCollision + 0x64);
        originalServerHull.specifiedMaxs =
            Read<Vector3>(serverCollision + 0x70);
        originalServerHull.surroundingMaxs =
            Read<Vector3>(serverCollision + 0x7C);
        originalServerHull.surroundingMins =
            Read<Vector3>(serverCollision + 0x88);
        originalServerHull.boundingRadius =
            Read<float>(serverCollision + 0x60);
        originalServerHull.solidFlags = Read<uint8_t>(serverCollision + 0x5A);
        originalServerHull.solidType = Read<uint8_t>(serverCollision + 0x5B);
        const Vector3 mins =
            Read<Vector3>(
                serverCollision + Offsets::CollisionMins);
        originalServerHull.captured = IsSaneHull(
            mins, originalServerHull.maxs,
            originalServerHull.boundingRadius);
    }
    if (!originalServerHull.captured) return;

    Vector3 serverRemMaxs = originalServerHull.maxs;
    serverRemMaxs.x = 16.0f;
    serverRemMaxs.y = 16.0f;
    serverRemMaxs.z = 40.0f;
    Write<Vector3>(
        serverCollision + Offsets::CollisionMaxs,
        serverRemMaxs);
    Write<Vector3>(serverCollision + 0x64, remMins);
    Write<Vector3>(serverCollision + 0x70, serverRemMaxs);
    Write<Vector3>(serverCollision + 0x7C, serverRemMaxs);
    Write<Vector3>(serverCollision + 0x88, remMins);
    Write<float>(serverCollision + 0x60, 30.1993f);
    SetTunnelModifierStates(
        Read<uintptr_t>(serverPawn + 0x3D0), originalServerHull);
    serverHullActive = true;
}

bool GetCurrentCameraForward(Vector3& forward) {
    return GetCurrentCameraForwardImpl(forward);
}

std::vector<PlayerData> GetPlayers() {
    std::vector<PlayerData> players;
    espStatus = {};

    struct CooldownObservation {
        float endTime{};
        float duration{};
        ULONGLONG observedAt{};
    };
    static std::unordered_map<uintptr_t, CooldownObservation> cooldownObservations;

    if (!clientBase) return players;

    Matrix4x4 viewMatrix{};
    // Read the complete matrix published for the backbuffer at Present. The
    // old path preferred a cached depth-bind matrix for up to 250 ms; during
    // fast camera movement that matrix belonged to an older camera frame and
    // made every ESP primitive freeze and then jump. Entity render transforms
    // below are sampled in this same Present, so only the current publication
    // is coherent with them.
    if (ReadCurrentViewMatrix(viewMatrix)) {
        currentViewMatrix = viewMatrix;
        currentViewMatrixReady = true;
    } else {
        currentViewMatrixReady = false;
        // Never combine current entity positions with an older camera frame.
        // Skipping one invalid Present is preferable to drawing stale data.
        return players;
    }
    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }
    espStatus.heroPawnsFound = !pawns.empty();

    Vector3 localPos{};
    Vector3 cameraPosition{};
    Vector3 distanceOrigin{};
    const bool cameraPositionFound = ReadCameraWorldPosition(
        viewMatrix, cameraPosition);
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
    // Bone-world calculation is one of the most expensive calls in the
    // render path.  ESP boxes only need the scene origin; calculate bones
    // when a feature actually consumes them.
    const bool aimKeyDown = (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool farmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool aimNeedsBones = aimAssist &&
        (aimToggleMode ? aimToggleActive : aimKeyDown);
    const bool farmNeedsBones = farmAssist &&
        (farmToggleMode ? farmToggleActive : farmKeyDown);
    const bool needPlayerBones = drawBones || enemyBonesEnabled || allyBonesEnabled ||
        aimNeedsBones || farmNeedsBones || aimLockedTarget != 0 ||
        HeroScriptsNeedPlayerBones();
    for (const uintptr_t entity : pawns) {
        // World snapshots may contain the local third-person pawn. It must
        // never be treated as an ESP target: its render skeleton is updated
        // in a camera-relative path and produces the giant wandering box/bones
        // visible in the recording.
        if (entity == currentLocalPawn) continue;
        const uintptr_t controller =
            ResolveEntity(Read<uint32_t>(entity + Offsets::PawnController));
        if (controller &&
            Read<uint8_t>(
                controller + Offsets::IsLocalPlayerController) == 1)
            continue;

        const int health = Read<int>(entity + Offsets::Health);
        if (health <= 0) continue;

        const uint8_t lifeState = Read<uint8_t>(entity + Offsets::LifeState);
        if (lifeState != 0) continue;

        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
        if (team != 2 && team != 3) continue;

        Vector3 pos{};
        if (!GetEntityPosition(entity, pos)) continue;
        Vector3 framePosition{};
        if (!GetEspFramePosition(entity, framePosition)) continue;

        PlayerData player;
        player.entity = entity;
        player.pos = pos;
        // Keep the rendered transform as the bone/aim anchor, but place the
        // ESP capsule at AbsOrigin.  The latter is the source used by the
        // jitter-free c3d7f8ff build and is published slightly ahead of
        // m_nodeToWorld while a hero is running.  Using m_nodeToWorld for the
        // box was stable after the Present fence, but left a constant visual
        // delay behind the model.
        player.visualAnchor = framePosition;
        player.hasVisualAnchor = true;
        player.worldPos = pos;
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
        if (needPlayerBones) {
            player.hasHeadBone = GetEntityBonePosition(entity, "head", player.headPos);
            // spine_3 is the stable upper-neck/shoulder joint used by the
            // existing skeleton chain immediately below head. It provides a
            // non-head aim point for Anti-Frog without inventing an offset.
            player.hasNeckBone = GetEntityBonePosition(
                entity, "spine_3", player.neckPos);
            player.hasBodyBone = GetEntityBonePosition(entity, "spine_2", player.bodyPos);
            if (!player.hasBodyBone) {
                player.hasBodyBone = GetEntityBonePosition(
                    entity, "spine_0", player.bodyPos);
            }
            player.hasLeftArmBone = GetEntityBonePosition(
                entity, "arm_upper_L", player.leftArmPos);
            player.hasRightArmBone = GetEntityBonePosition(
                entity, "arm_upper_R", player.rightArmPos);
            player.hasLeftLegBone = GetEntityBonePosition(
                entity, "leg_upper_L", player.leftLegPos);
            player.hasRightLegBone = GetEntityBonePosition(
                entity, "leg_upper_R", player.rightLegPos);
            if (drawBones) GetEntityBoneSkeleton(entity, player.bones);
        }
        player.health = health;
        player.maxHealth = Read<int>(entity + Offsets::MaxHealth);
        if (controller) {
            const int liveMaxHealth = Read<int>(controller + Offsets::ControllerPlayerData + Offsets::PlayerDataHealthMax);
            if (liveMaxHealth > 0 && liveMaxHealth < 100000) player.maxHealth = liveMaxHealth;
            player.playerName = ReadPlayerName(controller);
        }
        player.team = team;
        player.heroName = ReadHeroName(entity);
        // Ability handles and their replicated state are resolved through the
        // runtime schema offsets.  Only the four hero slots are kept; weapons
        // and items occupy other slots in the same network vector.
        const uintptr_t abilityComponent = entity + Offsets::AbilityComponent;
        const int abilityCount = std::clamp(Read<int>(abilityComponent + Offsets::AbilityVector), 0, 64);
        const uintptr_t abilityHandles = Read<uintptr_t>(abilityComponent + Offsets::AbilityVector + 8);
        const auto decodeAbilityLevel = [](uint32_t packedInfo) {
            // The live client encodes purchased tiers as a cumulative mask in
            // bits 16..19: 0x1 = level 0, 0x3 = level 1, 0x7 = level 2,
            // 0xF = level 3.  Bit 0 is an unrelated validity flag.
            const uint32_t tierMask = (packedInfo >> 16) & 0xFu;
            const int setTiers = static_cast<int>(tierMask & 0x1u) +
                                 static_cast<int>((tierMask >> 1) & 0x1u) +
                                 static_cast<int>((tierMask >> 2) & 0x1u) +
                                 static_cast<int>((tierMask >> 3) & 0x1u);
            return std::clamp(setTiers - 1, 0, 3);
        };
        std::array<int, 4> replicatedAbilityLevels{ -1, -1, -1, -1 };
        int upgradeStateCount = 0;
        uintptr_t upgradeStates = 0;
        if (controller) {
            const uintptr_t playerData = controller + Offsets::ControllerPlayerData;
            upgradeStateCount = std::clamp(
                Read<int>(playerData + Offsets::PlayerDataAbilityUpgradeStates), 0, 32);
            upgradeStates = Read<uintptr_t>(
                playerData + Offsets::PlayerDataAbilityUpgradeStates + 8);
            // AbilityUpgradeState_t is 0x38 bytes in the live Windows
            // schema: m_ItemID is at 0x30 and packed m_nUpgradeInfo at 0x34.
            constexpr uintptr_t abilityUpgradeStateStride = 0x38;
            for (int slot = 0; slot < static_cast<int>(replicatedAbilityLevels.size()) &&
                               slot < upgradeStateCount && upgradeStates; ++slot) {
                const uint32_t packedInfo = Read<uint32_t>(upgradeStates +
                    static_cast<uintptr_t>(slot) * abilityUpgradeStateStride +
                    Offsets::AbilityUpgradeStateInfo);
                replicatedAbilityLevels[slot] = decodeAbilityLevel(packedInfo);
            }
        }
        const float gameTime = GetClientGameTime();
        for (int i = 0; i < abilityCount && abilityHandles; ++i) {
            const uintptr_t ability = ResolveEntity(Read<uint32_t>(abilityHandles + i * sizeof(uint32_t)));
            if (!ability) continue;
            const int slot = Read<int>(ability + Offsets::AbilitySlot);
            if (slot < 0 || slot >= static_cast<int>(player.abilities.size())) continue;
            const uint32_t packedUpgrade = Read<uint32_t>(ability + Offsets::AbilityUpgradeInfo);
            auto& info = player.abilities[slot];
            info.valid = true;
            info.level = decodeAbilityLevel(packedUpgrade);
            if (replicatedAbilityLevels[slot] >= 0)
                info.level = replicatedAbilityLevels[slot];
            const float start = Read<float>(ability + Offsets::AbilityCooldownStart);
            const float end = Read<float>(ability + Offsets::AbilityCooldownEnd);
            const float duration = end - start;
            const float remaining = end - gameTime;
            // A candidate GlobalVars pointer is only usable when its clock
            // belongs to this exact server cooldown interval.  This rejects
            // unrelated client clocks that advance normally but have another
            // epoch, which otherwise makes a countdown begin at a wrong value.
            const bool clockMatchesCooldown = std::isfinite(start) &&
                std::isfinite(end) && gameTime >= start - 5.0f && gameTime <= end;
            if (std::isfinite(gameTime) && gameTime > 0.0f &&
                clockMatchesCooldown && remaining > 0.05f && remaining < 180.0f) {
                info.cooldown = remaining;
                info.cooldownDuration = duration > 0.05f && duration < 180.0f ? duration : remaining;
            } else {
                // m_flCooldownStart and m_flCooldownEnd are both replicated
                // GameTime_t fields in the live client schema.  This fallback
                // keeps a genuine countdown even in sessions where GlobalVars
                // cannot be resolved: once the server publishes a new end
                // time, decrease the server-provided duration using a local
                // monotonic clock.
                const ULONGLONG now = GetTickCount64();
                if (std::isfinite(start) && std::isfinite(end) &&
                    duration > 0.05f && duration < 180.0f) {
                    auto& observation = cooldownObservations[ability];
                    if (observation.observedAt == 0 ||
                        std::fabs(observation.endTime - end) > 0.01f) {
                        observation = { end, duration, now };
                    }
                    const float elapsed = static_cast<float>(now - observation.observedAt) / 1000.0f;
                    const float fallbackRemaining = observation.duration - elapsed;
                    info.cooldown = fallbackRemaining > 0.0f ? fallbackRemaining : 0.0f;
                    info.cooldownDuration = observation.duration;
                } else {
                    cooldownObservations.erase(ability);
                    info.cooldown = 0.0f;
                    info.cooldownDuration = 0.0f;
                }
            }
        }
        {
            constexpr uintptr_t abilityUpgradeStateStride = 0x38;
            std::string state = player.heroName + " count=" + std::to_string(upgradeStateCount);
            for (int index = 0; index < upgradeStateCount && upgradeStates; ++index) {
                const uintptr_t entry = upgradeStates +
                    static_cast<uintptr_t>(index) * abilityUpgradeStateStride;
                state += " " + std::to_string(Read<uint32_t>(entry + 0x30)) + ":" +
                    std::to_string(Read<uint32_t>(entry + Offsets::AbilityUpgradeStateInfo));
            }
            static std::unordered_map<uintptr_t, std::string> lastLoggedUpgradeStates;
            if (lastLoggedUpgradeStates[entity] != state) {
                lastLoggedUpgradeStates[entity] = state;
                std::ofstream log(
                    Dll6Paths::DataFileA("ability_levels.log"),
                    std::ios::app);
                if (log) log << state << '\n';
            }
        }
        const float dx = pos.x - distanceOrigin.x;
        const float dy = pos.y - distanceOrigin.y;
        const float dz = pos.z - distanceOrigin.z;
        // Source coordinates are in Hammer Units; 39.37 units correspond to 1 meter.
        player.distance = (localPositionFound || std::isfinite(distanceOrigin.x))
                              ? std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f
                              : 0.0f;
        if (!std::isfinite(player.distance)) continue;

        // AbsOrigin and the camera matrix are sampled in this fenced Present.
        // This matches the proven c3 ESP path without adding screen-space
        // smoothing, so starts/stops remain exact and stable.
        const bool hasScreenBounds = currentViewMatrixReady &&
            GetEntityScreenBounds(entity, pos, viewMatrix,
                                  player.boxLeft, player.boxTop,
                                  player.boxRight, player.boxBottom);
        if (hasScreenBounds || entity == aimLockedTarget) {
            if (!hasScreenBounds) {
                // Keep the locked pawn in the gameplay snapshot through the
                // rear hemisphere. Zero bounds are deliberately invalid for
                // ESP/candidate selection but its world position and bones
                // remain available to all aim paths.
                player.boxLeft = player.boxTop = 0.0f;
                player.boxRight = player.boxBottom = 0.0f;
            }
            players.push_back(player);
        }
    }

    return players;
}

ID3D11ShaderResourceView* GetAbilityIconSrv(const std::string& heroName, int slot) {
    static constexpr std::array<const char*, 38> heroNames = { "Infernus", "Seven", "Vindicta", "Lady Geist", "Abrams", "Wraith", "McGinnis", "Paradox", "Dynamo", "Kelvin", "Haze", "Holliday", "Bebop", "Calico", "Grey Talon", "Mo & Krill", "Shiv", "Ivy", "Warden", "Yamato", "Lash", "Viscous", "Pocket", "Mirage", "Vyper", "Sinclair", "Mina", "Drifter", "Venator", "Victor", "Paige", "The Doorman", "Billy", "Graves", "Apollo", "Rem", "Silver", "Celeste" };
    const auto hero = std::find_if(heroNames.begin(), heroNames.end(), [&](const char* name) { return heroName == name; });
    if (slot < 0 || slot >= 4 || hero == heroNames.end() || !pDevice || !moduleHandle) return nullptr;
    const size_t index = static_cast<size_t>(hero - heroNames.begin()) * 4 + static_cast<size_t>(slot);
    static std::array<ID3D11ShaderResourceView*, 152> icons{};
    static std::array<bool, 152> attempted{};
    if (icons[index] || attempted[index]) return icons[index];
    attempted[index] = true;
    HRSRC resource = FindResourceW(moduleHandle, MAKEINTRESOURCEW(108 + static_cast<UINT>(index)), MAKEINTRESOURCEW(10));
    HGLOBAL data = resource ? LoadResource(moduleHandle, resource) : nullptr;
    BYTE* bytes = data ? static_cast<BYTE*>(LockResource(data)) : nullptr;
    const DWORD size = resource ? SizeofResource(moduleHandle, resource) : 0;
    IWICImagingFactory* factory{}; IWICStream* stream{}; IWICBitmapDecoder* decoder{};
    IWICBitmapFrameDecode* frame{}; IWICFormatConverter* converter{};
    UINT width{}, height{}; std::vector<uint8_t> pixels; D3D11_TEXTURE2D_DESC desc{};
    D3D11_SUBRESOURCE_DATA initial{}; ID3D11Texture2D* texture{};
    if (!bytes || !size || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory))) || FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(bytes, size)) ||
        FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) goto done;
    converter->GetSize(&width, &height);
    pixels.resize(static_cast<size_t>(width) * height * 4);
    if (!width || !height || FAILED(converter->CopyPixels(nullptr, width * 4,
            static_cast<UINT>(pixels.size()), pixels.data()))) goto done;
    desc.Width=width; desc.Height=height; desc.MipLevels=1; desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1; desc.Usage=D3D11_USAGE_DEFAULT;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    initial.pSysMem=pixels.data(); initial.SysMemPitch=width*4;
    if (SUCCEEDED(pDevice->CreateTexture2D(&desc, &initial, &texture))) {
        pDevice->CreateShaderResourceView(texture, nullptr, &icons[index]); texture->Release();
    }
done:
    if(converter)converter->Release(); if(frame)frame->Release(); if(decoder)decoder->Release();
    if(stream)stream->Release(); if(factory)factory->Release();
    return icons[index];
}

void RenderESP(const std::vector<PlayerData>& players) {
    static bool movementDiagKeyLastDown = false;
    const bool movementDiagKeyDown =
        (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    if (movementDiagKeyDown && !movementDiagKeyLastDown) {
        movementDiagnostics = !movementDiagnostics;
        movementProcessCalls.store(0, std::memory_order_relaxed);
        movementCorrectionCalls.store(0, std::memory_order_relaxed);
        userCmdNetworkCalls.store(0, std::memory_order_relaxed);
        movementDiagAfterForward.store(0.0f, std::memory_order_relaxed);
        movementDiagAfterLeft.store(0.0f, std::memory_order_relaxed);
        movementDiagAfterYaw.store(0.0f, std::memory_order_relaxed);
        wishDirectionCalls.store(0, std::memory_order_relaxed);
        wishDirectionCorrectionCalls.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(movementDebugWishMutex);
            movementDebugWishReady = false;
        }
    }
    movementDiagKeyLastDown = movementDiagKeyDown;

    auto drawList = ImGui::GetBackgroundDrawList();
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;

    static float previousAntiFrogGameTime = 0.0f;
    const float antiFrogGameTime = GetClientGameTime();
    if ((previousAntiFrogGameTime > 0.0f && antiFrogGameTime <= 0.0f) ||
        (previousAntiFrogGameTime > 10.0f &&
         antiFrogGameTime + 2.0f < previousAntiFrogGameTime)) {
        ResetAntiFrogStats();
    }
    if (antiFrogGameTime > 0.0f)
        previousAntiFrogGameTime = antiFrogGameTime;

    const bool aimKeyDown = (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool aimEnabled = aimAssist && (aimToggleMode ? aimToggleActive : aimKeyDown);
    const bool farmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool farmEnabled = farmAssist && (farmToggleMode ? farmToggleActive : farmKeyDown);
    const char* aimStatus = aimEnabled
        ? (aimMixedMode ? "AIM  ON  [MIXED]" : (aimSilentMode ? "AIM  ON  [SILENT]" : "AIM  ON"))
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
    float nextStatusY = statusPosition.y + 18.0f;
    if (aimLockedTarget) {
        static uintptr_t cachedLockEntity = 0;
        static std::string cachedLockHero;
        if (cachedLockEntity != aimLockedTarget) {
            cachedLockEntity = aimLockedTarget;
            cachedLockHero.clear();
        }
        for (const PlayerData& player : players) {
            if (player.entity == aimLockedTarget && !player.heroName.empty()) {
                cachedLockHero = player.heroName;
                break;
            }
        }
        const std::string lockText = "Lock Target: " +
            (cachedLockHero.empty() ? std::string("Enemy") : cachedLockHero);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, nextStatusY + 1.0f),
                          ImColor(0, 0, 0, 200), lockText.c_str());
        drawList->AddText(ImVec2(statusPosition.x, nextStatusY),
                          ImColor(255, 205, 65, 245), lockText.c_str());
        nextStatusY += 18.0f;
    }
    if (antiFrog) {
        const float headshotPercent = GetAntiFrogHeadshotPercent();
        const char* slot = GetAntiFrogSlotLabel();
        char antiFrogText[64]{};
        std::snprintf(antiFrogText, sizeof(antiFrogText),
                      "HS%% %.1f%% / %.0f%%  [%s]", headshotPercent,
                      antiFrogHsThreshold, slot);
        const ImColor antiFrogColor =
            headshotPercent > antiFrogHsThreshold
                ? ImColor(255, 90, 90, 235)
                : ImColor(90, 235, 120, 235);
        const ImVec2 antiFrogPosition(
            10.0f, displaySize.y * 0.5f);
        drawList->AddText(ImVec2(antiFrogPosition.x + 1.0f,
                                 antiFrogPosition.y + 1.0f),
                          ImColor(0, 0, 0, 210), antiFrogText);
        drawList->AddText(antiFrogPosition, antiFrogColor, antiFrogText);
    }
    if (farmAssist) {
        const char* farmStatus = farmEnabled ? "CREEP AIM  ON" : "CREEP AIM  OFF";
        const ImColor farmColor = farmEnabled ? ImColor(255, 190, 70, 235) : ImColor(180, 180, 180, 210);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, nextStatusY + 1.0f),
                          ImColor(0, 0, 0, 190), farmStatus);
        drawList->AddText(ImVec2(statusPosition.x, nextStatusY), farmColor, farmStatus);
        nextStatusY += 18.0f;
    }
    if (autoLastHitOrbs) {
        const char* orbStatus = autoLastHitOrbsActive ? "ORB AIM  ON" : "ORB AIM  OFF";
        const ImColor orbStatusColor = autoLastHitOrbsActive
            ? ImColor(80, 220, 120, 235) : ImColor(180, 180, 180, 210);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, nextStatusY + 1.0f),
                          ImColor(0, 0, 0, 190), orbStatus);
        drawList->AddText(ImVec2(statusPosition.x, nextStatusY),
                          orbStatusColor, orbStatus);
        nextStatusY += 18.0f;
    }
    if (freeCam) {
        const char* freeCamStatus = freeCamActive
            ? "FREE CAMERA  ON"
            : "FREE CAMERA  OFF";
        const ImColor freeCamColor = freeCamActive
            ? ImColor(80, 220, 120, 235)
            : ImColor(120, 190, 255, 235);
        drawList->AddText(ImVec2(statusPosition.x + 1.0f, nextStatusY + 1.0f),
                          ImColor(0, 0, 0, 190), freeCamStatus);
        drawList->AddText(ImVec2(statusPosition.x, nextStatusY),
                          freeCamColor, freeCamStatus);
        nextStatusY += 18.0f;
    }
    if (movementDiagnostics && false) {
        const auto processCalls = movementProcessCalls.load(std::memory_order_relaxed);
        const auto correctionCalls = movementCorrectionCalls.load(std::memory_order_relaxed);
        const auto wishCalls = wishDirectionCalls.load(std::memory_order_relaxed);
        const auto wishCorrections = wishDirectionCorrectionCalls.load(std::memory_order_relaxed);
        char movementLine1[160]{};
        char movementLine2[200]{};
        std::snprintf(movementLine1, sizeof(movementLine1),
                      "MOVE: %s   INPUT: LOCAL OK",
                      processCalls > 0 ? "PROC OK" : "PROC NO");
        std::snprintf(movementLine2, sizeof(movementLine2),
                      "SILENT: %s   WISHDIR: %s / %s",
                      correctionCalls > 0 ? "ACTIVE" : "WAITING",
                      wishCalls > 0 ? "HOOK OK" : "HOOK NO",
                      wishCorrections > 0 ? "FIX ON" : "FIX NO");
        const ImVec2 diagnosticPosition(300.0f, 18.0f);
        const ImColor diagnosticColor = correctionCalls > 0
            ? ImColor(80, 220, 120, 235) : ImColor(255, 190, 70, 235);
        drawList->AddText(ImVec2(diagnosticPosition.x + 1.0f,
                                 diagnosticPosition.y + 1.0f),
                          ImColor(0, 0, 0, 190), movementLine1);
        drawList->AddText(diagnosticPosition, diagnosticColor, movementLine1);
        drawList->AddText(ImVec2(diagnosticPosition.x + 1.0f,
                                 diagnosticPosition.y + 17.0f),
                          ImColor(0, 0, 0, 190), movementLine2);
        drawList->AddText(ImVec2(diagnosticPosition.x,
                                 diagnosticPosition.y + 16.0f),
                          diagnosticColor, movementLine2);

        if (currentLocalPositionReady && currentViewMatrixReady) {
            const Vector3 origin = currentLocalPosition;
            auto drawWorldVector = [&](const Vector3& end,
                                       const ImColor& color,
                                       const char* label) {
                Vector2 startScreen{}, endScreen{};
                if (!WorldToScreen(origin, startScreen, currentViewMatrix) ||
                    !WorldToScreen(end, endScreen, currentViewMatrix)) return;
                drawList->AddLine(ImVec2(startScreen.x, startScreen.y),
                                  ImVec2(endScreen.x, endScreen.y), color, 2.0f);
                drawList->AddText(ImVec2(endScreen.x + 4.0f, endScreen.y),
                                  color, label);
            };

            Vector3 cameraForward{};
            if (GetCurrentCameraForwardImpl(cameraForward)) {
                drawWorldVector(
                    Vector3{origin.x + cameraForward.x * 100.0f,
                            origin.y + cameraForward.y * 100.0f,
                            origin.z + cameraForward.z * 100.0f},
                    ImColor(70, 150, 255, 235), "CAMERA");
            }

            Vector3 target{};
            bool targetReady = false;
            {
                std::lock_guard<std::mutex> lock(movementDebugTargetMutex);
                target = movementDebugTarget;
                targetReady = movementDebugTargetReady;
            }
            if (targetReady)
                drawWorldVector(target, ImColor(255, 80, 80, 235), "TARGET");

            Vector3 wishDirection{};
            bool wishReady = false;
            {
                std::lock_guard<std::mutex> lock(movementDebugWishMutex);
                wishDirection = movementDebugWishDirection;
                wishReady = movementDebugWishReady;
            }
            if (wishReady) {
                const float wishLength = std::hypot(wishDirection.x, wishDirection.y);
                if (std::isfinite(wishLength) && wishLength > 0.001f) {
                    drawWorldVector(
                        Vector3{origin.x + wishDirection.x / wishLength * 100.0f,
                                origin.y + wishDirection.y / wishLength * 100.0f,
                                origin.z},
                        ImColor(255, 190, 70, 235), "WISH");
                }
            }

            if (currentLocalPawn) {
                const Vector3 velocity = Read<Vector3>(
                    currentLocalPawn + Offsets::Velocity);
                const float speed = std::sqrt(
                    velocity.x * velocity.x + velocity.y * velocity.y);
                if (std::isfinite(speed) && speed > 1.0f) {
                    drawWorldVector(
                        Vector3{origin.x + velocity.x / speed * 100.0f,
                                origin.y + velocity.y / speed * 100.0f,
                                origin.z},
                        ImColor(80, 230, 120, 235), "MOVE");
                }
            }
        }
    }
    if (drawSpectatorList) {
        static std::vector<SpectatorEntry> cachedSpectators;
        static ULONGLONG lastSpectatorScan = 0;
        const ULONGLONG spectatorNow = GetTickCount64();
        if (spectatorNow - lastSpectatorScan >= 250) {
            cachedSpectators = CollectCurrentSpectators();
            lastSpectatorScan = spectatorNow;
        }
        const auto& spectators = cachedSpectators;
    if (drawSpectatorList) {
        float spectatorY = nextStatusY + 4.0f;
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
            for (const auto& spectator : spectators) {
                drawList->AddText(ImVec2(statusPosition.x + 1.0f, spectatorY + 1.0f),
                                  ImColor(0, 0, 0, 190), spectator.name.c_str());
                drawList->AddText(ImVec2(statusPosition.x, spectatorY),
                                  spectator.isEnemy
                                      ? ImColor(255, 102, 102, 235)
                                      : ImColor(102, 255, 102, 235),
                                  spectator.name.c_str());
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
            // Scanner, ESP and aim share one lifetime validator so an entity
            // that has expired or whose slot was recycled cannot survive in
            // one subsystem after disappearing from another.
            if (!IsXpOrbAlive(orb.entity, orb.handle)) continue;
            Vector3 position = orb.pos;
            Vector2 screen{};
            bool projected = false;
            Vector3 candidate{};
            // RenderOrigin is the visual coordinate tracked by the scanner.
            // Keep it first here so the stale-coordinate detector, ESP and
            // aim all operate on the same point.
            if (GetXpOrbPosition(orb.entity, candidate)) {
                projected = WorldToScreen(candidate, screen, currentViewMatrix);
                if (projected) position = candidate;
            }
            if (!projected && GetEntityPosition(orb.entity, candidate)) {
                projected = WorldToScreen(candidate, screen, currentViewMatrix);
                if (projected) position = candidate;
            }
            if (!projected) projected = WorldToScreen(position, screen, currentViewMatrix);
            if (!projected || !std::isfinite(position.x) || !std::isfinite(position.y) ||
                !std::isfinite(position.z) || !std::isfinite(screen.x) ||
                !std::isfinite(screen.y)) continue;
            if (currentLocalPositionReady || currentCameraPositionReady) {
                const Vector3 distanceOrigin = currentLocalPositionReady
                    ? currentLocalPosition : currentCameraPosition;
                const float orbDistance = std::sqrt(
                    (position.x - distanceOrigin.x) * (position.x - distanceOrigin.x) +
                    (position.y - distanceOrigin.y) * (position.y - distanceOrigin.y) +
                    (position.z - distanceOrigin.z) * (position.z - distanceOrigin.z)) / 39.37f;
                if (!std::isfinite(orbDistance) || orbDistance > orbEspMaxDistance)
                    continue;
            }
            const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
            // WorldToScreen can return a finite but enormous point for an
            // entity behind the camera. Never let an off-screen orb create a
            // full-screen ImGui circle.
            const float screenGuard = (std::max)(displaySize.x, displaySize.y) * 2.0f;
            if (displaySize.x <= 0.0f || displaySize.y <= 0.0f ||
                std::fabs(screen.x) > screenGuard ||
                std::fabs(screen.y) > screenGuard) continue;
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
                            if (!std::isfinite(edge.x) || !std::isfinite(edge.y) ||
                                std::fabs(edge.x) > screenGuard ||
                                std::fabs(edge.y) > screenGuard) continue;
                            outlineRadius = (std::max)(outlineRadius,
                                std::sqrt((edge.x - screen.x) * (edge.x - screen.x) +
                                          (edge.y - screen.y) * (edge.y - screen.y)));
                        }
                    }
                }
            }
            outlineRadius *= 0.4f;
            // A corrupted collision bound or a projection crossing the near
            // plane must never turn the orb marker into a screen-sized fill.
            if (!std::isfinite(outlineRadius) || outlineRadius <= 0.0f) continue;
            outlineRadius = std::clamp(outlineRadius, 4.0f, 96.0f);
            const float fillRadius = outlineRadius * 0.64f;
            if (IsXpOrbAttackable(orb.entity, orb.handle)) {
                drawList->AddCircleFilled(point, fillRadius, orbColor, 24);
            }
            drawList->AddCircle(point, outlineRadius, ImColor(255, 165, 45, 245), 24, 2.0f);
            drawList->AddText(ImVec2(screen.x + outlineRadius + 3.0f, screen.y - 8.0f),
                              ImColor(255, 255, 255, 240), orbLabel);
        }
    }

    if (campTimersEnabled && (campTimersOnScreen || campTimersOnMinimap)) {
        std::vector<CampTimerData> timers;
        { std::lock_guard lock(campTimersMutex); timers = campTimers; }
        const float gameTime = GetCampGameTime();
        std::sort(timers.begin(), timers.end(),
                  [](const CampTimerData& left, const CampTimerData& right) {
                      return left.respawnAt < right.respawnAt;
                  });
        struct ActiveCampLabel {
            const CampTimerData* camp{};
            std::string text;
        };
        std::vector<ActiveCampLabel> activeLabels;
        const auto drawOutlinedText = [&](const ImVec2& position,
                                          ImU32 color,
                                          const char* text) {
            const ImU32 outline = IM_COL32(4, 6, 8, 245);
            drawList->AddText(ImVec2(position.x - 1.0f, position.y), outline, text);
            drawList->AddText(ImVec2(position.x + 1.0f, position.y), outline, text);
            drawList->AddText(ImVec2(position.x, position.y - 1.0f), outline, text);
            drawList->AddText(ImVec2(position.x, position.y + 1.0f), outline, text);
            drawList->AddText(position, color, text);
        };
        const ImU32 timerColor = ImColor(
            campTimerColor[0], campTimerColor[1],
            campTimerColor[2], campTimerColor[3]);
        for (const CampTimerData& camp : timers) {
            if (!std::isfinite(gameTime) || gameTime <= 0.0f ||
                camp.respawnAt <= gameTime) continue;
            const int seconds = static_cast<int>(std::ceil(camp.respawnAt - gameTime));
            char label[32]{};
            const char* tier = camp.campClass == 34 ? "T1" :
                camp.campClass == 35 ? "T2" :
                camp.campClass == 36 ? "T3" : "V";
            std::snprintf(label, sizeof(label), "%s %d:%02d", tier,
                          seconds / 60, seconds % 60);
            activeLabels.push_back({&camp, label});
            if (campTimersOnScreen && currentViewMatrixReady) {
                Vector2 screen{};
                if (WorldToScreen(camp.pos, screen, currentViewMatrix)) {
                    const ImVec2 textSize = ImGui::CalcTextSize(label);
                    drawOutlinedText(ImVec2(screen.x - textSize.x * 0.5f,
                                            screen.y - 10.0f),
                                     timerColor, label);
                }
            }
        }

        if (campTimersOnMinimap && !activeLabels.empty() &&
            displaySize.x > 0.0f && displaySize.y > 0.0f) {
            // Current Valve HUD layout (hud.css): minimap_persp is 440x440,
            // minimap_container is 380x380 centred inside it, and
            // HudMinimapContainer is 105% of that.  The parent clamp_width is
            // capped at 2000px and centred, so its right edge differs from the
            // physical display edge on ultrawide resolutions.
            const float uiScale = displaySize.y / 1080.0f;
            const float logicalDisplayWidth = displaySize.x / uiScale;
            const float clampedWidth = (std::min)(logicalDisplayWidth, 2000.0f);
            const float clampRight = displaySize.x * 0.5f +
                clampedWidth * uiScale * 0.5f;
            constexpr float outerSizeLogical = 440.0f;
            constexpr float mapSizeLogical = 380.0f;
            constexpr float bottomMarginLogical = 15.0f;
            const float mapSize = mapSizeLogical * uiScale;
            const float inset =
                (outerSizeLogical - mapSizeLogical) * 0.5f * uiScale;
            const float mapLeft =
                clampRight - outerSizeLogical * uiScale + inset;
            const float mapTop = displaySize.y -
                (outerSizeLogical + bottomMarginLogical) * uiScale + inset;
            const float hudSize = mapSize * 1.05f;
            const float hudLeft = mapLeft - (hudSize - mapSize) * 0.5f;
            const float hudTop = mapTop - (hudSize - mapSize) * 0.5f;

            for (const ActiveCampLabel& active : activeLabels) {
                const CampTimerData& camp = *active.camp;
                float normalizedX = static_cast<float>(camp.mapX) / 255.0f;
                float normalizedY = 1.0f -
                    static_cast<float>(camp.mapY) / 255.0f;
                if (camp.localTeam == 3) {
                    normalizedX = 1.0f - normalizedX;
                    normalizedY = 1.0f - normalizedY;
                }

                const ImVec2 center(hudLeft + normalizedX * hudSize,
                                    hudTop + normalizedY * hudSize);
                const char* countdown = active.text.c_str();
                if (const char* separator = std::strchr(countdown, ' '))
                    countdown = separator + 1;
                ImFont* font = ImGui::GetFont();
                const float fontSize = std::clamp(
                    campTimerMinimapSize, 15.0f, 30.0f);
                const ImVec2 textSize = font->CalcTextSizeA(
                    fontSize, FLT_MAX, 0.0f, countdown);
                const ImVec2 textPosition(center.x - textSize.x * 0.5f,
                                          center.y - textSize.y * 0.5f);
                const ImU32 outline = IM_COL32(4, 6, 8, 230);
                drawList->AddText(font, fontSize,
                    ImVec2(textPosition.x - 1.0f, textPosition.y),
                    outline, countdown);
                drawList->AddText(font, fontSize,
                    ImVec2(textPosition.x + 1.0f, textPosition.y),
                    outline, countdown);
                drawList->AddText(font, fontSize,
                    ImVec2(textPosition.x, textPosition.y - 1.0f),
                    outline, countdown);
                drawList->AddText(font, fontSize,
                    ImVec2(textPosition.x, textPosition.y + 1.0f),
                    outline, countdown);
                drawList->AddText(font, fontSize, textPosition,
                                  timerColor, countdown);
            }
        }

        // A fixed HUD list keeps every cleared camp visible even when it is
        // outside the camera frustum or on the other half of the map.
        if (campTimersOnScreen && !activeLabels.empty()) {
            const float x = displaySize.x - 175.0f;
            const float y = 120.0f;
            drawList->AddRectFilled(ImVec2(x - 10.0f, y - 8.0f),
                                    ImVec2(displaySize.x - 12.0f,
                                           y + 18.0f * (activeLabels.size() + 1)),
                                    ImColor(8, 11, 16, 205), 4.0f);
            drawList->AddText(ImVec2(x, y), ImColor(235, 235, 235, 245),
                              "CAMP TIMERS");
            for (size_t i = 0; i < activeLabels.size(); ++i)
                drawList->AddText(ImVec2(x, y + 18.0f * (i + 1)),
                                  timerColor, activeLabels[i].text.c_str());
        }
    }

    // Each creep faction has an independent switch. This lets the Creep
    // page selectively show enemy, ally, and neutral units.
    if ((creepEspEnabled || allyCreepEspEnabled || neutralCreepEspEnabled) &&
        currentViewMatrixReady) {
        struct SmoothedCreepBox {
            float left{}, top{}, right{}, bottom{};
            ULONGLONG lastSeen{};
            bool initialized{};
        };
        static std::unordered_map<uintptr_t, SmoothedCreepBox>
            smoothedCreepBoxes;
        const ULONGLONG creepSmoothingNow = GetTickCount64();
        const float creepDt =
            std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.050f);
        // No positional filter: the box must stay on the current render
        // transform and never trail the creep model.
        const float creepSmoothing = 1.0f;

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
            if (creep.health <= 0 || lifeState != 0) continue;

            // Scene-node state and life state above are sufficient to reject
            // stale slots. Calling CalcWorldSpaceBones for every
            // creep here makes Creep ESP scale very poorly in large fights.
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
            const float creepWidth = right - left;
            const float creepHeight = bottom - top;
            const float maxCoordinate = (std::max)(displaySize.x, displaySize.y) * 4.0f;
            if (!projected || !std::isfinite(left) || !std::isfinite(top) ||
                !std::isfinite(right) || !std::isfinite(bottom) ||
                creepWidth < 2.0f || creepHeight < 4.0f ||
                creepWidth > displaySize.x * 1.5f ||
                creepHeight > displaySize.y * 1.5f ||
                std::fabs(left) > maxCoordinate || std::fabs(right) > maxCoordinate ||
                std::fabs(top) > maxCoordinate || std::fabs(bottom) > maxCoordinate) continue;
            liveCreeps.push_back(creep);

            const float rawCenterX = (left + right) * 0.5f;
            const float rawCenterY = (top + bottom) * 0.5f;
            auto& smooth = smoothedCreepBoxes[creep.entity];
            const float oldCenterX = (smooth.left + smooth.right) * 0.5f;
            const float oldCenterY = (smooth.top + smooth.bottom) * 0.5f;
            const bool stale = !smooth.initialized ||
                creepSmoothingNow - smooth.lastSeen > 250 ||
                std::fabs(rawCenterX - oldCenterX) > 500.0f ||
                std::fabs(rawCenterY - oldCenterY) > 500.0f;
            if (stale) {
                smooth.left = left;
                smooth.top = top;
                smooth.right = right;
                smooth.bottom = bottom;
                smooth.initialized = true;
            } else {
                smooth.left += (left - smooth.left) * creepSmoothing;
                smooth.top += (top - smooth.top) * creepSmoothing;
                smooth.right += (right - smooth.right) * creepSmoothing;
                smooth.bottom += (bottom - smooth.bottom) * creepSmoothing;
            }
            smooth.lastSeen = creepSmoothingNow;
            const float originOffsetX =
                (smooth.left + smooth.right) * 0.5f - rawCenterX;
            const float originOffsetY =
                (smooth.top + smooth.bottom) * 0.5f - rawCenterY;
            left = smooth.left;
            top = smooth.top;
            right = smooth.right;
            bottom = smooth.bottom;

            const uint8_t localTeam = currentLocalPawn
                ? Read<uint8_t>(currentLocalPawn + Offsets::Team)
                : 0;
            const bool ally = localTeam != 0 && creep.team == localTeam;
            const bool neutral = creep.team == 4;
            const bool drawThisCreep = neutral ? neutralCreepEspEnabled
                                               : (ally ? allyCreepEspEnabled : creepEspEnabled);
            // Creep ESP intentionally uses one consistent box style. There
            // is no separate Appearance control for creeps: every shown
            // creep receives a corner box.
            const bool drawBoxes = true;
            const bool drawCornerBoxes = true;
            const bool drawDistance = neutral ? creepDistanceEnabled
                                              : (ally ? allyCreepDistanceEnabled : creepDistanceEnabled);
            const bool drawHealth = neutral ? creepHealthEnabled
                                            : (ally ? allyCreepHealthEnabled : creepHealthEnabled);
            const bool drawHealthValues = neutral ? creepHealthValuesEnabled
                                                   : (ally ? allyCreepHealthValuesEnabled : creepHealthValuesEnabled);
            const float* boxColor = ally ? allyCreepBoxColor : creepBoxColor;
            const float* healthColorValue = ally ? allyCreepHealthColor : creepHealthColor;
            const float* healthValueColorValue = ally
                ? allyCreepHealthValueColor : creepHealthValueColor;
            if (!drawThisCreep) continue;
            const float dx = creep.pos.x - currentLocalPosition.x;
            const float dy = creep.pos.y - currentLocalPosition.y;
            const float dz = creep.pos.z - currentLocalPosition.z;
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 39.37f;
            if (distance > creepEspMaxDistance) continue;
            const ImColor color = neutral
                ? ImColor(190, 190, 190, 150)
                : ImColor(boxColor[0], boxColor[1], boxColor[2], boxColor[3]);
            if (drawBoxes) {
                if (!drawCornerBoxes) {
                    drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), color, 0.0f, 0,
                                      ally ? 1.0f : 1.25f);
                } else {
                    const float length = std::clamp((std::min)(right - left, bottom - top) * std::clamp(cornerBoxLength, 0.10f, 0.35f),
                                                    4.0f, 32.0f);
                    const float thickness = std::clamp(boxThickness, 0.5f, 4.0f);
                    drawList->AddLine(ImVec2(left, top), ImVec2(left + length, top), color, thickness);
                    drawList->AddLine(ImVec2(left, top), ImVec2(left, top + length), color, thickness);
                    drawList->AddLine(ImVec2(right - length, top), ImVec2(right, top), color, thickness);
                    drawList->AddLine(ImVec2(right, top), ImVec2(right, top + length), color, thickness);
                    drawList->AddLine(ImVec2(left, bottom - length), ImVec2(left, bottom), color, thickness);
                    drawList->AddLine(ImVec2(left, bottom), ImVec2(left + length, bottom), color, thickness);
                    drawList->AddLine(ImVec2(right - length, bottom), ImVec2(right, bottom), color, thickness);
                    drawList->AddLine(ImVec2(right, bottom - length), ImVec2(right, bottom), color, thickness);
                }
            }

            Vector2 originScreen{};
            if (WorldToScreen(creep.pos, originScreen, currentViewMatrix)) {
                drawList->AddCircleFilled(
                    ImVec2(originScreen.x + originOffsetX,
                           originScreen.y + originOffsetY),
                    3.0f, color, 12);
            }
            char distanceText[24]{};
            std::snprintf(distanceText, sizeof(distanceText), "%.0fm", distance);
            const ImVec2 textSize = ImGui::CalcTextSize(distanceText);
            if (drawDistance) {
                drawList->AddText(ImVec2((left + right - textSize.x) * 0.5f,
                                         bottom + 4.0f),
                                  ImColor(255, 255, 255, 220), distanceText);
            }

            const float healthPercent = creep.maxHealth > 0
                ? std::clamp(static_cast<float>(creep.health) / creep.maxHealth, 0.0f, 1.0f)
                : 0.0f;
            constexpr float barWidth = 3.0f;
            const float barLeft = left - 6.0f;
            drawList->AddRectFilled(ImVec2(barLeft, top), ImVec2(barLeft + barWidth, bottom),
                                    ImColor(40, 40, 40, 130));
            const ImColor healthColor = neutral
                ? ImColor(190, 190, 190, 190)
                : ImColor(healthColorValue[0], healthColorValue[1], healthColorValue[2], healthColorValue[3]);
            if (drawHealth) {
                drawList->AddRectFilled(ImVec2(barLeft, top), ImVec2(barLeft + barWidth, bottom),
                                        ImColor(40, 40, 40, 130));
                drawList->AddRectFilled(ImVec2(barLeft, bottom - (bottom - top) * healthPercent),
                                        ImVec2(barLeft + barWidth, bottom), healthColor);
            }
            if (drawHealthValues) {
                char healthText[32]{};
                std::snprintf(healthText, sizeof(healthText), "%d/%d", creep.health, creep.maxHealth);
                 const ImColor valueColor = neutral
                     ? ImColor(190, 190, 190, 220)
                     : ImColor(healthValueColorValue[0], healthValueColorValue[1],
                               healthValueColorValue[2], healthValueColorValue[3]);
                 drawList->AddText(ImVec2(left, top - 16.0f), valueColor, healthText);
            }
        }
        for (auto it = smoothedCreepBoxes.begin();
             it != smoothedCreepBoxes.end();) {
            if (creepSmoothingNow - it->second.lastSeen > 1000)
                it = smoothedCreepBoxes.erase(it);
            else
                ++it;
        }
        {
            std::lock_guard<std::mutex> lock(farmTargetsMutex);
            farmTargets = std::move(liveCreeps);
        }
    }

    if ((!enemyEspEnabled && !allyEspEnabled) || !currentViewMatrixReady)
        return;

    const ImVec2 snaplineOrigin(displaySize.x * 0.5f, displaySize.y);

    const auto makeColor = [](const float color[4]) {
        return ImColor(color[0], color[1], color[2], color[3]);
    };
    const auto addCornerBox = [&](ImDrawList* list, float left, float top,
                                  float right, float bottom, ImU32 color,
                                  float cornerLength, float thicknessValue) {
        const float width = right - left;
        const float height = bottom - top;
        const float length = std::clamp(cornerLength, 0.05f, 0.35f) *
            (std::min)(width, height);
        const float thickness = std::clamp(thicknessValue, 0.5f, 4.0f);
        list->AddLine(ImVec2(left, top), ImVec2(left + length, top), color, thickness);
        list->AddLine(ImVec2(left, top), ImVec2(left, top + length), color, thickness);
        list->AddLine(ImVec2(right - length, top), ImVec2(right, top), color, thickness);
        list->AddLine(ImVec2(right, top), ImVec2(right, top + length), color, thickness);
        list->AddLine(ImVec2(left, bottom - length), ImVec2(left, bottom), color, thickness);
        list->AddLine(ImVec2(left, bottom), ImVec2(left + length, bottom), color, thickness);
        list->AddLine(ImVec2(right - length, bottom), ImVec2(right, bottom), color, thickness);
        list->AddLine(ImVec2(right, bottom - length), ImVec2(right, bottom), color, thickness);
    };

    const auto projectSnaplinePoint = [&](const Vector3& world,
                                          ImVec2& point) {
        const float clipX = currentViewMatrix.m[0][0] * world.x +
            currentViewMatrix.m[0][1] * world.y +
            currentViewMatrix.m[0][2] * world.z + currentViewMatrix.m[0][3];
        const float clipY = currentViewMatrix.m[1][0] * world.x +
            currentViewMatrix.m[1][1] * world.y +
            currentViewMatrix.m[1][2] * world.z + currentViewMatrix.m[1][3];
        const float clipW = currentViewMatrix.m[3][0] * world.x +
            currentViewMatrix.m[3][1] * world.y +
            currentViewMatrix.m[3][2] * world.z + currentViewMatrix.m[3][3];
        if (!std::isfinite(clipX) || !std::isfinite(clipY) ||
            !std::isfinite(clipW)) return false;

        const float centerX = displaySize.x * 0.5f;
        const float centerY = displaySize.y * 0.5f;
        const float originX = snaplineOrigin.x;
        const float originY = snaplineOrigin.y;
        constexpr float edgeMargin = 6.0f;
        // Derive only the horizontal camera yaw. The full clip-space W also
        // contains pitch and camera-height terms, which makes a stationary
        // rear target slide when the third-person camera bobs.
        float rightX = currentViewMatrix.m[0][0];
        float rightY = currentViewMatrix.m[0][1];
        float forwardX = currentViewMatrix.m[3][0];
        float forwardY = currentViewMatrix.m[3][1];
        const float rightLength =
            std::sqrt(rightX * rightX + rightY * rightY);
        const float forwardLength =
            std::sqrt(forwardX * forwardX + forwardY * forwardY);
        if (!std::isfinite(rightLength) || rightLength <= 0.001f ||
            !std::isfinite(forwardLength) || forwardLength <= 0.001f)
            return false;
        rightX /= rightLength;
        rightY /= rightLength;
        forwardX /= forwardLength;
        forwardY /= forwardLength;

        float rightDistance = clipX / rightLength;
        float forwardDistance = clipW / forwardLength;
        if (currentLocalPositionReady) {
            const float relativeX = world.x - currentLocalPosition.x;
            const float relativeY = world.y - currentLocalPosition.y;
            rightDistance = relativeX * rightX + relativeY * rightY;
            forwardDistance =
                relativeX * forwardX + relativeY * forwardY;
        }
        const float bearing = std::atan2(rightDistance, forwardDistance);
        float bearingDirectionX = std::sin(bearing);
        float bearingDirectionY = -std::cos(bearing);
        float directionX = bearingDirectionX;
        float directionY = bearingDirectionY;

        if (clipW > 0.01f) {
            const float projectedX =
                centerX + (clipX / clipW) * centerX;
            const float projectedY =
                centerY - (clipY / clipW) * centerY;
            if (projectedX >= edgeMargin &&
                projectedX <= displaySize.x - edgeMargin &&
                projectedY >= edgeMargin &&
                projectedY <= displaySize.y - edgeMargin) {
                point = ImVec2(projectedX, projectedY);
                return true;
            }
            float perspectiveDirectionX = projectedX - originX;
            float perspectiveDirectionY = projectedY - originY;
            const float perspectiveLength = std::sqrt(
                perspectiveDirectionX * perspectiveDirectionX +
                perspectiveDirectionY * perspectiveDirectionY);
            if (std::isfinite(perspectiveLength) &&
                perspectiveLength > 0.001f) {
                perspectiveDirectionX /= perspectiveLength;
                perspectiveDirectionY /= perspectiveLength;

                // Perspective projection becomes singular near 90 degrees.
                // Blend into the horizontal camera-space bearing before that
                // point, then keep following the same bearing around the rear
                // hemisphere. No front/rear mode switch is required.
                constexpr float blendStart =
                    60.0f * 0.017453292519943295f;
                constexpr float blendEnd =
                    88.0f * 0.017453292519943295f;
                const float absoluteBearing = std::fabs(bearing);
                float blend = std::clamp(
                    (absoluteBearing - blendStart) /
                    (blendEnd - blendStart), 0.0f, 1.0f);
                blend = blend * blend * (3.0f - 2.0f * blend);
                directionX = perspectiveDirectionX +
                    (bearingDirectionX - perspectiveDirectionX) * blend;
                directionY = perspectiveDirectionY +
                    (bearingDirectionY - perspectiveDirectionY) * blend;
            }
        }

        const float directionLength = std::sqrt(
            directionX * directionX + directionY * directionY);
        if (!std::isfinite(directionLength) || directionLength <= 0.001f)
            return false;
        directionX /= directionLength;
        directionY /= directionLength;
        if (std::fabs(directionX) < 0.001f &&
            std::fabs(directionY) < 0.001f)
            directionY = 1.0f;
        const float horizontalScale = directionX > 0.001f
            ? (displaySize.x - edgeMargin - originX) / directionX
            : (directionX < -0.001f
                ? (edgeMargin - originX) / directionX : FLT_MAX);
        const float verticalScale = directionY > 0.001f
            ? (displaySize.y - edgeMargin - originY) / directionY
            : (directionY < -0.001f
                ? (edgeMargin - originY) / directionY : FLT_MAX);
        const float scale = (std::min)(horizontalScale, verticalScale);
        if (!std::isfinite(scale) || scale <= 0.0f) return false;
        point = ImVec2(originX + directionX * scale,
                       originY + directionY * scale);
        return true;
    };

    for (const auto& player : players) {
        const uint8_t localTeam = currentLocalPawn
            ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
        const bool ally = localTeam != 0 && player.team == localTeam;
        const float teamBoxThickness = boxThickness;
        const float teamCornerLength = cornerBoxLength;
        const bool teamEsp = ally ? allyEspEnabled : enemyEspEnabled;
        const bool teamAbilities = ally ? allyAbilitiesEnabled : enemyAbilitiesEnabled;
        const float teamMaxDistance = ally ? allyEspMaxDistance : enemyEspMaxDistance;
        const bool teamSnaplines = ally ? allySnaplinesEnabled : enemySnaplinesEnabled;
        const bool lockCandidateMarker = !ally && aimLockTarget &&
            !aimLockedTarget && player.entity == aimLockCandidate;
        const bool lockedTargetMarker = !ally && aimLockTarget &&
            player.entity == aimLockedTarget;
        const bool lockMarker = lockCandidateMarker || lockedTargetMarker;
        // Snaplines belong to the same team ESP channel. Do this gate before
        // both the off-screen and on-screen snapline paths below.
        if ((!teamEsp && !lockMarker) ||
            (player.distance > teamMaxDistance && !lockMarker)) continue;
        // worldPos and box bounds use the fenced AbsOrigin sample. Bones are
        // rebuilt from the same completed frame before they are projected.
        const Vector3 visualOrigin = player.worldPos;
        const float targetClipW =
            currentViewMatrix.m[3][0] * visualOrigin.x +
            currentViewMatrix.m[3][1] * visualOrigin.y +
            currentViewMatrix.m[3][2] * visualOrigin.z +
            currentViewMatrix.m[3][3];
        const float rawFrameLeft = player.boxLeft;
        const float rawFrameTop = player.boxTop;
        const float rawFrameRight = player.boxRight;
        const float rawFrameBottom = player.boxBottom;
        const bool hasScreenBounds =
            std::isfinite(rawFrameLeft) && std::isfinite(rawFrameTop) &&
            std::isfinite(rawFrameRight) && std::isfinite(rawFrameBottom) &&
            rawFrameRight > rawFrameLeft && rawFrameBottom > rawFrameTop;
        const bool targetOnScreen = std::isfinite(targetClipW) &&
            targetClipW > 0.01f && hasScreenBounds;
        // Off-screen snaplines still use the bearing projection. On-screen
        // snaplines are drawn below from the same filtered box as ESP so both
        // primitives cannot jump to different one-frame positions.
        if (teamSnaplines && !targetOnScreen) {
            ImVec2 snaplinePoint{};
            if (projectSnaplinePoint(player.worldPos, snaplinePoint)) {
                const ImVec2 lineStart = snaplineOrigin;
                const int alpha = static_cast<int>(
                    std::clamp(snaplineAlpha, 0.0f, 255.0f));
                drawList->AddLine(lineStart, snaplinePoint,
                                  ImColor(255, 255, 255, alpha), 1.0f);
            }
        }
        // Full boxes and bones require a valid perspective projection. The
        // snapline above does not; it continues around the rear hemisphere.
        if (!std::isfinite(targetClipW) || targetClipW <= 0.01f ||
            !hasScreenBounds)
        {
            continue;
        }

        // The visual snapshot is now fenced to the completed game frame.
        // Draw its exact projection. The previous screen-space correction was
        // compensating for mixed-frame samples and made the box trail behind
        // the rendered model even after the source data became coherent.
        const float frameLeft = rawFrameLeft;
        const float frameTop = rawFrameTop;
        const float frameRight = rawFrameRight;
        const float frameBottom = rawFrameBottom;
        const float rawCenterX =
            (rawFrameLeft + rawFrameRight) * 0.5f;
        const float screenX = (frameLeft + frameRight) * 0.5f;
        const float rawCenterY =
            (rawFrameTop + rawFrameBottom) * 0.5f;
        const float filteredCenterY = (frameTop + frameBottom) * 0.5f;
        const float screenOffsetX = screenX - rawCenterX;
        const float screenOffsetY = filteredCenterY - rawCenterY;
        const float screenY = frameBottom;
        const float boxTop = frameTop;
        const float boxHeight = frameBottom - frameTop;
        if (lockMarker) {
            // A small reticle beside the top-right box corner is easier to
            // read than the old flag and does not overlap a name or health.
            // Red means the bind will select this target; gold means locked.
            // Scale it from the box height so its size follows the ESP target
            // at every distance instead of staying a fixed screen-pixel size.
            const float indicatorScale = std::clamp(
                boxHeight / 160.0f, 0.78f, 1.45f);
            const auto scaled = [indicatorScale](float value) {
                return value * indicatorScale;
            };
            const ImColor accent = lockedTargetMarker
                ? ImColor(255, 198, 54, 255)
                : ImColor(244, 67, 76, 255);
            const ImVec2 center(frameRight + scaled(11.0f),
                                boxTop + scaled(9.0f));
            const float radius = scaled(6.0f);
            drawList->AddCircleFilled(center, radius + scaled(1.8f),
                                      ImColor(9, 12, 18, 220), 16);
            drawList->AddCircle(center, radius, accent, 16, scaled(1.3f));
            drawList->AddLine(ImVec2(center.x - radius - scaled(2.0f), center.y),
                              ImVec2(center.x - radius + scaled(0.5f), center.y),
                              accent, scaled(1.1f));
            drawList->AddLine(ImVec2(center.x + radius - scaled(0.5f), center.y),
                              ImVec2(center.x + radius + scaled(2.0f), center.y),
                              accent, scaled(1.1f));
            drawList->AddLine(ImVec2(center.x, center.y - radius - scaled(2.0f)),
                              ImVec2(center.x, center.y - radius + scaled(0.5f)),
                              accent, scaled(1.1f));
            drawList->AddLine(ImVec2(center.x, center.y + radius - scaled(0.5f)),
                              ImVec2(center.x, center.y + radius + scaled(2.0f)),
                              accent, scaled(1.1f));
            if (lockedTargetMarker) {
                // Lock state gets a positive, unambiguous center tick.
                drawList->AddLine(ImVec2(center.x - scaled(2.2f), center.y),
                                  ImVec2(center.x - scaled(0.6f),
                                         center.y + scaled(1.8f)),
                                  ImColor(255, 255, 255, 255), scaled(1.3f));
                drawList->AddLine(ImVec2(center.x - scaled(0.6f),
                                         center.y + scaled(1.8f)),
                                  ImVec2(center.x + scaled(2.8f),
                                         center.y - scaled(2.1f)),
                                  ImColor(255, 255, 255, 255), scaled(1.3f));
            } else {
                drawList->AddCircleFilled(center, scaled(1.3f), accent, 8);
            }
        }
        if (!teamEsp) continue;
        if (teamSnaplines) {
            const ImVec2 lineStart = snaplineOrigin;
            const int alpha = static_cast<int>(
                std::clamp(snaplineAlpha, 0.0f, 255.0f));
            drawList->AddLine(lineStart, ImVec2(screenX, screenY),
                              ImColor(255, 255, 255, alpha), 1.0f);
        }
        const bool teamBoxes = ally ? allyBoxesEnabled : enemyBoxesEnabled;
        const bool teamCornerBoxes = ally ? allyCornerBoxesEnabled : enemyCornerBoxesEnabled;
        const bool teamHealth = ally ? allyHealthEnabled : enemyHealthEnabled;
        const bool teamHealthValues = ally ? allyHealthValuesEnabled : enemyHealthValuesEnabled;
        const bool teamNames = ally ? allyNamesEnabled : enemyNamesEnabled;
        const bool teamPlayerNames = ally ? allyPlayerNamesEnabled : enemyPlayerNamesEnabled;
        const bool teamDistance = ally ? allyDistanceEnabled : enemyDistanceEnabled;
        const bool teamBones = ally ? allyBonesEnabled : enemyBonesEnabled;
        const ImColor boxColor = makeColor(ally ? teammateBoxColor : enemyBoxColor);
        const ImColor nameColor = makeColor(ally ? teammateNameColor : enemyNameColor);
        const ImColor skeletonColor = makeColor(
            ally ? teammateSkeletonColor : enemySkeletonColor);
        const ImColor playerNameColor = makeColor(ally ? teammatePlayerNameColor : enemyPlayerNameColor);
        const ImColor healthBarColor = makeColor(ally ? teammateHealthBarColor : enemyHealthBarColor);
        const ImColor healthValueColor = makeColor(ally ? teammateHealthValueColor : enemyHealthValueColor);

        if (teamBones) {
            for (const auto& bone : player.bones) {
                Vector2 start{}, end{};
                if (WorldToScreen(bone.start, start, currentViewMatrix) &&
                    WorldToScreen(bone.end, end, currentViewMatrix)) {
                    drawList->AddLine(
                        ImVec2(start.x + screenOffsetX,
                               start.y + screenOffsetY),
                        ImVec2(end.x + screenOffsetX,
                               end.y + screenOffsetY),
                        skeletonColor, 1.5f);
                }
            }
        }

        // Stack all header lines above the box. This prevents HP, player name,
        // and hero name from occupying the same y-coordinate.
        float headerY = boxTop - 17.0f;
        const auto drawHeaderLine = [&](const std::string& text,
                                        ImColor color) {
            if (text.empty()) return;
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const ImVec2 position(screenX - size.x * 0.5f, headerY);
            drawList->AddText(ImVec2(position.x + 1.0f, position.y + 1.0f),
                              ImColor(0, 0, 0, 190), text.c_str());
            drawList->AddText(position, color, text.c_str());
            headerY -= 14.0f;
        };
        // drawHeaderLine grows upward.  Emit in reverse visual order so the
        // actual in-game stack reads top-to-bottom: hero, player, HP.
        if (teamHealth && teamHealthValues) {
            const std::string healthText = std::to_string(player.health) + "/" +
                                           std::to_string(player.maxHealth);
            drawHeaderLine(healthText, healthValueColor);
        }
        if (teamPlayerNames) drawHeaderLine(player.playerName, playerNameColor);
        if (teamNames) drawHeaderLine(player.heroName, nameColor);

        if (teamBoxes) {
            if (teamCornerBoxes) {
                addCornerBox(drawList, frameLeft, boxTop, frameRight,
                             screenY, boxColor, teamCornerLength, teamBoxThickness);
            } else {
                drawList->AddRect(ImVec2(frameLeft, boxTop),
                                  ImVec2(frameRight, screenY), boxColor,
                                  0.0f, 0, teamBoxThickness);
            }
        }

        if (teamHealth) {
            const float healthPercent = player.maxHealth > 0
                                            ? std::clamp(static_cast<float>(player.health) / player.maxHealth, 0.0f, 1.0f)
                                            : 0.0f;
            constexpr float barWidth = 4.0f;
            const float barLeft = frameLeft - 7.0f;

            // A vertical bar stays readable at every distance and shows loss from the top.
            drawList->AddRectFilled(
                ImVec2(barLeft, boxTop),
                ImVec2(barLeft + barWidth, screenY),
                ImColor(50, 50, 50, 200)
            );

            drawList->AddRectFilled(
                ImVec2(barLeft, screenY - boxHeight * healthPercent),
                ImVec2(barLeft + barWidth, screenY),
                healthBarColor
            );

        }

        if (teamDistance && player.distance > 0.0f) {
            const std::string distText = std::to_string(static_cast<int>(player.distance)) + "m";
            drawList->AddText(
                ImVec2(screenX - 15, screenY + 6),
                ImColor(255, 255, 255, 200),
                distText.c_str()
            );
        }

        int abilityCount = 0;
        for (const auto& ability : player.abilities) abilityCount += ability.valid ? 1 : 0;
        // Ability state is not replicated for every remote bot/player, but
        // the hero identity is.  Do not hide its artwork merely because the
        // server has no cooldown state to expose for that pawn.
        const bool hasHeroAbilityArt = GetAbilityIconSrv(player.heroName, 0) != nullptr;
        if (hasHeroAbilityArt) abilityCount = static_cast<int>(player.abilities.size());
        if (teamAbilities && abilityCount) {
            constexpr float gap = 3.0f;
            // Scale with the projected box, so ability art follows the same
            // distance/perspective behavior as the rest of the ESP.
            const float iconSize = std::clamp((frameRight - frameLeft - gap * 3.0f) * 0.50f,
                                              26.0f, 72.0f);
            const float rowWidth = abilityCount * iconSize + (abilityCount - 1) * gap;
            float iconX = screenX - rowWidth * 0.5f;
            const float iconY = screenY + (teamDistance ? 25.0f : 8.0f);
            for (size_t slot = 0; slot < player.abilities.size(); ++slot) {
                const auto& ability = player.abilities[slot];
                if (!ability.valid && !hasHeroAbilityArt) continue;
                // Match the compact HUD-style treatment: a clean white tile
                // with the transparent ability artwork rendered as a black
                // silhouette.  This stays legible against every map surface.
                const ImU32 border = IM_COL32(62, 54, 43, 255);
                const ImU32 fill = IM_COL32(221, 213, 195, 255);
                drawList->AddRectFilled(ImVec2(iconX, iconY),
                                        ImVec2(iconX + iconSize, iconY + iconSize), fill, 1.5f);
                if (ID3D11ShaderResourceView* icon = GetAbilityIconSrv(player.heroName, static_cast<int>(slot)))
                    drawList->AddImage(reinterpret_cast<ImTextureID>(icon),
                                       ImVec2(iconX + 2.0f, iconY + 2.0f),
                                       ImVec2(iconX + iconSize - 2.0f, iconY + iconSize - 2.0f),
                                       ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                       IM_COL32(12, 12, 12, 255));
                else {
                    const char slotText[2] = { static_cast<char>('1' + slot), 0 };
                    drawList->AddText(ImVec2(iconX + iconSize * 0.36f, iconY + 3.0f),
                                      IM_COL32(235, 235, 240, 255), slotText);
                }
                if (ability.cooldown > 0.05f) {
                    // Keep the countdown on top of the artwork.  A dark
                    // clock badge makes it readable on both light and dark
                    // ability icons without hiding the entire icon.
                    const ImVec2 center(iconX + iconSize * 0.5f, iconY + iconSize * 0.5f);
                    const float cooldownFraction = ability.cooldownDuration > 0.05f
                        ? std::clamp(ability.cooldown / ability.cooldownDuration, 0.0f, 1.0f)
                        : 0.0f;
                    if (cooldownFraction > 0.005f) {
                        constexpr int maxSegments = 32;
                        const int segments = std::clamp(
                            static_cast<int>(std::ceil(cooldownFraction * maxSegments)), 1, maxSegments);
                        std::array<ImVec2, maxSegments + 2> sector{};
                        sector[0] = center;
                        const float radius = iconSize * 0.5f - 2.0f;
                        constexpr float pi = 3.14159265358979323846f;
                        for (int point = 0; point <= segments; ++point) {
                            const float angle = -pi * 0.5f +
                                (2.0f * pi * cooldownFraction * point / segments);
                            sector[point + 1] = ImVec2(center.x + std::cos(angle) * radius,
                                                       center.y + std::sin(angle) * radius);
                        }
                        drawList->AddConvexPolyFilled(sector.data(), segments + 2,
                                                      IM_COL32(0, 0, 0, 105));
                    }
                    const float radius = iconSize * 0.30f;
                    drawList->AddCircleFilled(center, radius + 2.0f, IM_COL32(0, 0, 0, 210), 20);
                    drawList->AddCircleFilled(center, radius, IM_COL32(26, 30, 38, 238), 20);

                    char cooldown[12]{};
                    std::snprintf(cooldown, sizeof(cooldown),
                                  ability.cooldown < 10.0f ? "%.1f" : "%.0f",
                                  ability.cooldown);
                    const float fontSize = std::clamp(iconSize * 0.31f, 12.0f, 22.0f);
                    const ImVec2 textSize = ImGui::GetFont()->CalcTextSizeA(
                        fontSize, FLT_MAX, 0.0f, cooldown);
                    const ImVec2 textPos(center.x - textSize.x * 0.5f,
                                         center.y - textSize.y * 0.5f);
                    drawList->AddText(nullptr, fontSize,
                                      ImVec2(textPos.x + 1.0f, textPos.y + 1.0f),
                                      IM_COL32(0, 0, 0, 255), cooldown);
                    drawList->AddText(nullptr, fontSize, textPos,
                                      IM_COL32(255, 255, 255, 255), cooldown);
                }
                drawList->AddRect(ImVec2(iconX, iconY),
                                  ImVec2(iconX + iconSize, iconY + iconSize), border, 1.5f);
                {
                    constexpr int maxAbilityLevel = 3;
                    constexpr float levelGap = 3.0f;
                    const ImU32 purchasedColor = IM_COL32(74, 210, 112, 255);
                    const ImU32 unpurchasedColor = IM_COL32(210, 76, 65, 255);
                    const ImU32 levelTrackColor = IM_COL32(58, 50, 39, 255);
                    const float levelY = iconY + iconSize + 3.0f;
                    drawList->AddRectFilled(ImVec2(iconX + 1.0f, levelY - 1.0f),
                                             ImVec2(iconX + iconSize - 1.0f, levelY + 5.0f),
                                             levelTrackColor, 1.0f);
                    const float levelWidth = (iconSize - 6.0f -
                        (maxAbilityLevel - 1) * levelGap) / maxAbilityLevel;
                    for (int level = 0; level < maxAbilityLevel; ++level) {
                        const float levelX = iconX + 3.0f + level * (levelWidth + levelGap);
                        const ImU32 color = level < ability.level ? purchasedColor : unpurchasedColor;
                        drawList->AddRectFilled(ImVec2(levelX, levelY),
                                                 ImVec2(levelX + levelWidth, levelY + 4.0f), color, 0.5f);
                    }
                }
                iconX += iconSize + gap;
            }
        }
    }

}

static void RenderMenuLegacy(size_t playerCount) {
    if (!menuOpen) return;

    const bool wasMenuOpen = menuOpen;
    ImGui::SetNextWindowSize(ImVec2(760.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("Deadlock Internal", &menuOpen, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);

    if (ImGui::BeginTable("TopSections", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Visuals", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("ESP", &drawEsp);
            ImGui::Checkbox("Boxes", &drawBoxes);
            ImGui::Checkbox("Corner boxes", &cornerBoxes);
            ImGui::Checkbox("Show teammates", &drawTeammates);
            ImGui::SliderFloat("Box thickness", &boxThickness, 0.5f, 4.0f, "%.1f");
            ImGui::SliderFloat("Corner length", &cornerBoxLength, 0.10f, 0.35f, "%.2f");
            if (ImGui::TreeNode("ESP colors")) {
                ImGui::ColorEdit4("Enemy box", enemyBoxColor, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Teammate box", teammateBoxColor, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Enemy name", enemyNameColor, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Teammate name", teammateNameColor, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Enemy health", enemyHealthColor, ImGuiColorEditFlags_NoInputs);
                ImGui::ColorEdit4("Teammate health", teammateHealthColor, ImGuiColorEditFlags_NoInputs);
                ImGui::TreePop();
            }
            ImGui::Checkbox("Health Bars", &drawHealth);
            ImGui::Checkbox("Health", &drawHealthValues);
            ImGui::Checkbox("Hero Name", &drawNames);
            ImGui::Checkbox("Player Name", &drawPlayerNames);
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
            ImGui::Checkbox("Orb aim", &autoLastHitOrbs);
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
            if (ImGui::RadioButton("Normal##AimMode", !aimSilentMode && !aimMixedMode)) {
                aimSilentMode = false;
                aimMixedMode = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Silent##AimMode", aimSilentMode && !aimMixedMode)) {
                aimSilentMode = true;
                aimMixedMode = false;
            }
            ImGui::SameLine();
            if (ImGui::RadioButton("Mixed##AimMode", aimMixedMode)) {
                aimSilentMode = false;
                aimMixedMode = true;
            }
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
            DrawAimBoneSelectorImGui();
            ImGui::SliderFloat("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
            ImGui::SliderFloat("Aim smooth", &aimSmooth, 1.0f, 20.0f, "%.1f");
        }
        ImGui::TableNextColumn();
        if (ImGui::CollapsingHeader("Misc", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Auto parry (F)", &autoParry);
            ImGui::Checkbox("Spectator list", &drawSpectatorList);
            ImGui::Checkbox("Free cam", &freeCam);
            ImGui::SameLine();
            if (ImGui::Button(freeCamKeyCapture ? "Press free cam key..." : AimKeyName(freeCamKey)))
                freeCamKeyCapture = true;
            ImGui::SameLine();
            ImGui::TextUnformatted("Free cam bind");
            ImGui::SliderFloat("Free cam speed", &freeCamSpeed,
                               50.0f, 5000.0f, "%.0f units/s");
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

static void RenderMenuV1(size_t playerCount) {
    if (!menuOpen) return;

    static int tab = 0;
    static float tabAlpha = 0.0f;
    const float dt = (std::min)(ImGui::GetIO().DeltaTime, 0.05f);
    tabAlpha = (std::min)(1.0f, tabAlpha + dt * 8.0f);
    const bool wasMenuOpen = menuOpen;

    ImGui::SetNextWindowSize(ImVec2(920.0f, 620.0f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.045f, 0.047f, 0.060f, 0.98f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.25f, 0.26f, 0.32f, 0.75f));
    const bool visible = ImGui::Begin("DEADLOCK  //  CONTROL", &menuOpen,
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);
    if (!visible) {
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        return;
    }

    const ImVec4 accent(0.88f, 0.12f, 0.24f, 1.0f);
    const ImVec4 text(0.90f, 0.90f, 0.94f, 1.0f);
    const ImVec4 muted(0.48f, 0.49f, 0.56f, 1.0f);
    const ImVec4 card(0.075f, 0.078f, 0.098f, 1.0f);

    auto Toggle = [&](const char* label, bool* value) {
        ImGui::PushID(label);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##switch", ImVec2(width, 32.0f));
        if (ImGui::IsItemClicked()) *value = !*value;
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->AddText(ImVec2(pos.x, pos.y + 8),
                      ImGui::ColorConvertFloat4ToU32(*value ? text : muted), label);
        const float x = pos.x + width - 36.0f;
        const ImU32 track = *value
            ? ImGui::ColorConvertFloat4ToU32(accent)
            : ImGui::ColorConvertFloat4ToU32(ImVec4(0.20f, 0.21f, 0.25f, 1.0f));
        list->AddRectFilled(ImVec2(x, pos.y + 8), ImVec2(x + 30, pos.y + 24), track, 8.0f);
        list->AddCircleFilled(ImVec2(x + (*value ? 22.0f : 8.0f), pos.y + 16),
                              6.0f, IM_COL32(245, 245, 248, 255));
        ImGui::PopID();
    };

    auto Card = [&](const char* title, const char* subtitle) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
        ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_Borders, 0);
        ImGui::TextColored(accent, "%s", title);
        ImGui::TextColored(muted, "%s", subtitle);
        ImGui::Separator();
        ImGui::Spacing();
    };
    auto EndCard = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleColor();
    };

    ImGui::BeginChild("##navigation", ImVec2(190, 0), false, 0);
    ImGui::TextColored(accent, "DEADLOCK");
    ImGui::TextColored(muted, "internal control");
    ImGui::Spacing();
    const char* names[] = { "Visuals", "Aim assist", "Farm assist", "Misc" };
    const char* descriptions[] = { "ESP & world", "Targeting", "Creeps & orbs", "Utility" };
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        ImGui::PushStyleColor(ImGuiCol_Button, i == tab ? accent : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.12f, 0.24f, 0.42f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
        if (ImGui::Button(names[i], ImVec2(-1, 38))) {
            tab = i;
            tabAlpha = 0.0f;
        }
        ImGui::PopStyleColor(3);
        ImGui::TextColored(muted, "  %s", descriptions[i]);
        ImGui::Spacing();
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::TextColored(muted, "STATUS");
    ImGui::Text("FPS   %.0f", ImGui::GetIO().Framerate);
    ImGui::Text("PLAYERS   %zu", playerCount);
    ImGui::EndChild();

    ImGui::SameLine(0, 0);
    ImGui::BeginChild("##settings", ImVec2(0, 0), false, 0);
    ImGui::TextColored(text, "%s", names[tab]);
    ImGui::SameLine();
    ImGui::TextColored(muted, "  /  configure module");
    ImGui::Separator();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, tabAlpha);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.125f, 0.15f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.17f, 0.17f, 0.21f, 1));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, accent);
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 0.28f, 0.38f, 1));

    if (tab == 0) {
        Card("ESP overlay", "Information rendered on top of the world");
        Toggle("Enable ESP", &drawEsp);
        Toggle("Boxes", &drawBoxes);
        Toggle("Corner boxes", &cornerBoxes);
        Toggle("Show teammates", &drawTeammates);
        Toggle("Health bars", &drawHealth);
        Toggle("Health values", &drawHealthValues);
        Toggle("Hero names", &drawNames);
        Toggle("Player names", &drawPlayerNames);
        Toggle("Distance", &drawDistance);
        Toggle("Snaplines", &drawSnaplines);
        Toggle("Bones", &drawBones);
        Toggle("Glow", &glowEnabled);
        Toggle("FOV circle", &drawFovCircle);
        ImGui::SliderFloat("Box thickness", &boxThickness, 0.5f, 4.0f, "%.1f");
        ImGui::SliderFloat("Corner length", &cornerBoxLength, 0.10f, 0.35f, "%.2f");
        ImGui::ColorEdit4("Enemy box color", enemyBoxColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Teammate box color", teammateBoxColor, ImGuiColorEditFlags_NoInputs);
        EndCard();
    } else if (tab == 1) {
        Card("Aim assist", "Target selection and silent aim behavior");
        Toggle("Enable aim assist", &aimAssist);
        Toggle("Visibility check", &aimVisibilityCheck);
        const char* modes[] = { "Normal", "pSilent", "Mixed" };
        int mode = aimMixedMode ? 2 : (aimSilentMode ? 1 : 0);
        if (ImGui::Combo("Aim mode", &mode, modes, IM_ARRAYSIZE(modes)))
        {
            aimSilentMode = mode == 1;
            aimMixedMode = mode == 2;
        }
        DrawAimBoneSelectorImGui();
        ImGui::SliderFloat("FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
        int selectionMode = static_cast<int>(aimSelectionMode);
        const char* selections[] = { "Crosshair", "Distance", "Health" };
        if (ImGui::Combo("Target selection", &selectionMode, selections, IM_ARRAYSIZE(selections)))
            aimSelectionMode = static_cast<AimSelectionMode>(std::clamp(selectionMode, 0, 2));
        ImGui::SliderFloat("Pitch smooth", &aimPitchSmooth, 1.0f, 20.0f, "%.1f");
        ImGui::SliderFloat("Yaw smooth", &aimYawSmooth, 1.0f, 20.0f, "%.1f");
        ImGui::Checkbox("Only Yaw", &aimOnlyYaw);
        ImGui::Checkbox("Lock Target", &aimLockTarget);
        ImGui::Checkbox("Prediction", &aimPrediction);
        ImGui::SliderFloat("Hitchance", &aimHitchance, 0.0f, 100.0f, "%.0f%%");
        ImGui::Checkbox("Anti-Frog", &antiFrog);
        if (antiFrog)
            ImGui::SliderFloat("HS threshold", &antiFrogHsThreshold,
                               1.0f, 99.0f, "%.0f%%");
        ImGui::Spacing();
        if (ImGui::Button(aimKeyCapture ? "Press a key..." : AimKeyName(aimAssistKey), ImVec2(170, 0)))
            aimKeyCapture = true;
        ImGui::SameLine();
        ImGui::TextColored(muted, "Aim key");
        Toggle("Toggle activation", &aimToggleMode);
        EndCard();
    } else if (tab == 2) {
        Card("Farm assist", "Creeps, orbs and last-hit helpers");
        Toggle("Creep aim", &farmAssist);
        Toggle("Creep ESP", &drawCreepEsp);
        Toggle("Orb ESP", &drawOrbEsp);
        Toggle("Auto last-hit orbs", &autoLastHitOrbs);
        Toggle("Orb visibility check", &orbAimVisibilityCheck);
        const char* farmModes[] = { "Normal", "Silent" };
        int farmMode = farmSilentMode ? 1 : 0;
        if (ImGui::Combo("Farm mode", &farmMode, farmModes, IM_ARRAYSIZE(farmModes)))
            farmSilentMode = farmMode == 1;
        ImGui::SliderFloat("Creep smooth", &farmAimSmooth, 1.0f, 20.0f, "%.1f");
        ImGui::SliderFloat("Farm FOV", &farmFov, 40.0f, 600.0f, "%.0f px");
        ImGui::SliderFloat("FOV alpha", &farmFovAlpha, 0.0f, 255.0f, "%.0f");
        if (ImGui::Button(farmKeyCapture ? "Press a key..." : AimKeyName(farmAssistKey), ImVec2(170, 0)))
            farmKeyCapture = true;
        ImGui::SameLine();
        ImGui::TextColored(muted, "Farm key");
        EndCard();
    } else {
        Card("Misc", "Utility and session controls");
        Toggle("Auto parry", &autoParry);
        Toggle("Spectator list", &drawSpectatorList);
        Toggle("Free cam", &freeCam);
        ImGui::SliderFloat("Free cam speed", &freeCamSpeed, 50.0f, 5000.0f, "%.0f units/s");
        if (ImGui::Button(freeCamKeyCapture ? "Press a key..." : AimKeyName(freeCamKey), ImVec2(170, 0)))
            freeCamKeyCapture = true;
        ImGui::SameLine();
        ImGui::TextColored(muted, "Free cam key");
        ImGui::Separator();
        if (ImGui::Button("Unload DLL", ImVec2(170, 34))) RequestUnload();
        EndCard();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    if (wasMenuOpen && !menuOpen) {
        SaveConfig();
        SetMenuOpen(false);
    }
}

static void RenderMenuV2(size_t playerCount) {
    if (!menuOpen) return;

    ImGuiIO& io = ImGui::GetIO();
    ImFont* regular = io.FontDefault;
    ImFont* heading = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : regular;
    const float dt = (std::min)(io.DeltaTime, 0.05f);
    const float ease = 1.0f - std::exp(-dt * 14.0f);
    static int activeTab = 0;
    static float pageAlpha = 1.0f;
    static float pageShift = 0.0f;
    static std::unordered_map<ImGuiID, float> toggleAnimations;
    static std::unordered_map<ImGuiID, float> sliderAnimations;
    pageAlpha += (1.0f - pageAlpha) * ease;
    pageShift += (0.0f - pageShift) * ease;
    const bool wasMenuOpen = menuOpen;

    const ImVec4 bg(0.035f, 0.037f, 0.047f, 0.985f);
    const ImVec4 sidebar(0.052f, 0.055f, 0.069f, 1.0f);
    const ImVec4 panel(0.064f, 0.067f, 0.083f, 1.0f);
    const ImVec4 panelHover(0.078f, 0.081f, 0.100f, 1.0f);
    const ImVec4 accent(0.91f, 0.105f, 0.205f, 1.0f);
    const ImVec4 accentSoft(0.91f, 0.105f, 0.205f, 0.14f);
    const ImVec4 primary(0.92f, 0.925f, 0.95f, 1.0f);
    const ImVec4 secondary(0.51f, 0.52f, 0.59f, 1.0f);
    const ImVec4 line(0.14f, 0.145f, 0.18f, 1.0f);

    ImGui::SetNextWindowSize(ImVec2(980.0f, 650.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 11.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);
    ImGui::PushStyleColor(ImGuiCol_Border, line);
    if (!ImGui::Begin("##deadlock_control_v2", &menuOpen,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return;
    }

    ImDrawList* windowDraw = ImGui::GetWindowDrawList();
    const ImVec2 windowPos = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();

    // Header
    windowDraw->AddRectFilled(windowPos,
                              ImVec2(windowPos.x + windowSize.x, windowPos.y + 62.0f),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(0.045f, 0.047f, 0.058f, 1.0f)),
                              11.0f, ImDrawFlags_RoundCornersTop);
    windowDraw->AddLine(ImVec2(windowPos.x, windowPos.y + 62.0f),
                        ImVec2(windowPos.x + windowSize.x, windowPos.y + 62.0f),
                        ImGui::ColorConvertFloat4ToU32(line));
    windowDraw->AddRectFilled(ImVec2(windowPos.x + 22.0f, windowPos.y + 19.0f),
                              ImVec2(windowPos.x + 48.0f, windowPos.y + 43.0f),
                              ImGui::ColorConvertFloat4ToU32(accent), 6.0f);
    windowDraw->AddCircleFilled(ImVec2(windowPos.x + 35.0f, windowPos.y + 31.0f),
                                4.0f, IM_COL32(255, 255, 255, 235));
    ImGui::SetCursorPos(ImVec2(61, 14));
    ImGui::PushFont(heading);
    ImGui::TextColored(primary, "Deadlock");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(62, 36));
    ImGui::TextColored(secondary, "combat assistant");
    ImGui::SetCursorPos(ImVec2(windowSize.x - 146.0f, 18.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.32f, 0.20f, 0.34f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.14f, 0.42f, 0.25f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
    ImGui::Button("  ONLINE  ", ImVec2(92, 27));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    ImGui::SetCursorPos(ImVec2(windowSize.x - 42.0f, 17.0f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accentSoft);
    if (ImGui::Button("X", ImVec2(27, 27))) menuOpen = false;
    ImGui::PopStyleColor(2);

    auto Toggle = [&](const char* label, const char* description, bool* value) {
        ImGui::PushID(label);
        const ImGuiID id = ImGui::GetID("##toggle_anim");
        float& animation = toggleAnimations[id];
        animation += ((*value ? 1.0f : 0.0f) - animation) * ease;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##toggle", ImVec2(width, description ? 47.0f : 38.0f));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) *value = !*value;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hovered)
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + (description ? 47.0f : 38.0f)),
                              ImGui::ColorConvertFloat4ToU32(panelHover), 6.0f);
        dl->AddText(ImVec2(p.x + 4.0f, p.y + 5.0f),
                    ImGui::ColorConvertFloat4ToU32(primary), label);
        if (description)
            dl->AddText(ImVec2(p.x + 4.0f, p.y + 25.0f),
                        ImGui::ColorConvertFloat4ToU32(secondary), description);
        const float switchX = p.x + width - 45.0f;
        const float switchY = p.y + (description ? 14.0f : 10.0f);
        const ImVec4 off(0.18f, 0.19f, 0.23f, 1.0f);
        const ImVec4 trackColor(off.x + (accent.x - off.x) * animation,
                                off.y + (accent.y - off.y) * animation,
                                off.z + (accent.z - off.z) * animation, 1.0f);
        dl->AddRectFilled(ImVec2(switchX, switchY), ImVec2(switchX + 38, switchY + 20),
                          ImGui::ColorConvertFloat4ToU32(trackColor), 10.0f);
        dl->AddCircleFilled(ImVec2(switchX + 10.0f + animation * 18.0f, switchY + 10.0f),
                            7.0f, IM_COL32(246, 246, 249, 255));
        ImGui::PopID();
    };

    auto Slider = [&](const char* label, float* value, float minimum, float maximum,
                      const char* format) {
        ImGui::PushID(label);
        const ImGuiID id = ImGui::GetID("##slider_anim");
        const float target = (*value - minimum) / (maximum - minimum);
        float& animation = sliderAnimations[id];
        if (!std::isfinite(animation)) animation = target;
        animation += (target - animation) * ease;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##slider", ImVec2(width, 51.0f));
        if (ImGui::IsItemActive()) {
            const float mouse = (io.MousePos.x - p.x) / width;
            *value = minimum + (maximum - minimum) * std::clamp(mouse, 0.0f, 1.0f);
        }
        char valueText[48]{};
        std::snprintf(valueText, sizeof(valueText), format, *value);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(p.x + 4, p.y + 3), ImGui::ColorConvertFloat4ToU32(primary), label);
        const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
        dl->AddText(ImVec2(p.x + width - valueSize.x - 4, p.y + 3),
                    ImGui::ColorConvertFloat4ToU32(secondary), valueText);
        const float y = p.y + 34.0f;
        dl->AddRectFilled(ImVec2(p.x + 4, y), ImVec2(p.x + width - 4, y + 4),
                          IM_COL32(42, 44, 54, 255), 2.0f);
        const float fillX = p.x + 4 + (width - 8) * std::clamp(animation, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(p.x + 4, y), ImVec2(fillX, y + 4),
                          ImGui::ColorConvertFloat4ToU32(accent), 2.0f);
        dl->AddCircleFilled(ImVec2(fillX, y + 2), 6.0f,
                            ImGui::ColorConvertFloat4ToU32(accent));
        ImGui::PopID();
    };

    auto Combo = [&](const char* label, int* value, const char* const* items, int count) {
        ImGui::PushID(label);
        ImGui::TextColored(secondary, "%s", label);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.095f, 0.098f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, panelHover);
        ImGui::PushStyleColor(ImGuiCol_Header, accentSoft);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(accent.x, accent.y, accent.z, 0.28f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (ImGui::BeginCombo("##combo", items[*value])) {
            for (int i = 0; i < count; ++i) {
                if (ImGui::Selectable(items[i], *value == i)) *value = i;
                if (*value == i) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopID();
    };

    auto KeyButton = [&](const char* id, bool capture, const char* keyName, bool* captureState) {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.095f, 0.098f, 0.12f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, panelHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, accentSoft);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (ImGui::Button(capture ? "Press any key..." : keyName, ImVec2(-1, 34)))
            *captureState = true;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    auto BeginPanel = [&](const char* id, const char* title, const char* subtitle, float height) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, panel);
        ImGui::PushStyleColor(ImGuiCol_Border, line);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::BeginChild(id, ImVec2(0, height), ImGuiChildFlags_Borders, 0);
        ImGui::PushFont(heading);
        ImGui::TextColored(primary, "%s", title);
        ImGui::PopFont();
        ImGui::TextColored(secondary, "%s", subtitle);
        ImGui::Dummy(ImVec2(0, 5));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 4));
    };
    auto EndPanel = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    };

    // Navigation
    ImGui::SetCursorPos(ImVec2(0, 62));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, sidebar);
    ImGui::BeginChild("##sidebar_v2", ImVec2(205, 588), false, 0);
    ImGui::SetCursorPos(ImVec2(14, 22));
    ImGui::TextColored(secondary, "MODULES");
    const char* tabNames[] = { "Visuals", "Aim assist", "Farm assist", "Misc" };
    const char* tabGlyphs[] = { "V", "A", "F", "M" };
    const char* tabDescriptions[] = { "ESP and rendering", "Combat targeting", "Creeps and souls", "Utility settings" };
    for (int i = 0; i < 4; ++i) {
        ImGui::SetCursorPosX(10.0f);
        ImGui::PushID(i);
        const bool selected = activeTab == i;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              selected ? accentSoft : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(accent.x, accent.y, accent.z, 0.11f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(accent.x, accent.y, accent.z, 0.18f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
        if (ImGui::Button("##tab", ImVec2(185, 55))) {
            activeTab = i;
            pageAlpha = 0.0f;
            pageShift = 16.0f;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        if (selected)
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(itemMin.x, itemMin.y + 9), ImVec2(itemMin.x + 3, itemMin.y + 46),
                ImGui::ColorConvertFloat4ToU32(accent), 2.0f);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(itemMin.x + 25, itemMin.y + 27), 14.0f,
            ImGui::ColorConvertFloat4ToU32(selected ? accent : ImVec4(0.13f, 0.14f, 0.17f, 1.0f)));
        const ImVec2 glyphSize = ImGui::CalcTextSize(tabGlyphs[i]);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(itemMin.x + 25 - glyphSize.x * 0.5f, itemMin.y + 19),
            IM_COL32(245, 245, 248, 255), tabGlyphs[i]);
        ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + 50, itemMin.y + 10),
            ImGui::ColorConvertFloat4ToU32(selected ? primary : ImVec4(0.70f, 0.71f, 0.76f, 1.0f)), tabNames[i]);
        ImGui::GetWindowDrawList()->AddText(ImVec2(itemMin.x + 50, itemMin.y + 31),
            ImGui::ColorConvertFloat4ToU32(secondary), tabDescriptions[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorPos(ImVec2(18, 490));
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextColored(secondary, "SESSION");
    ImGui::TextColored(primary, "%.0f FPS", io.Framerate);
    ImGui::SameLine();
    ImGui::TextColored(secondary, "  %zu players", playerCount);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Content
    ImGui::SetCursorPos(ImVec2(205, 62));
    ImGui::BeginChild("##content_v2", ImVec2(775, 588), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::SetCursorPos(ImVec2(24 + pageShift, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, pageAlpha);
    ImGui::PushFont(heading);
    ImGui::TextColored(primary, "%s", tabNames[activeTab]);
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextColored(secondary, " / configuration");
    ImGui::SetCursorPos(ImVec2(24 + pageShift, 55));

    if (ImGui::BeginTable("##panel_grid", 2, ImGuiTableFlags_SizingStretchSame,
                          ImVec2(727, 510))) {
        ImGui::TableSetupColumn("##left", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##right", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();

        if (activeTab == 0) {
            BeginPanel("##visual_general", "Player ESP", "Core overlay elements", 490);
            Toggle("Enable ESP", "Master overlay switch", &drawEsp);
            Toggle("Bounding boxes", "Draw player bounds", &drawBoxes);
            Toggle("Corner boxes", "Use corner-only box style", &cornerBoxes);
            Toggle("Show teammates", "Include allied heroes", &drawTeammates);
            Toggle("Health bar", nullptr, &drawHealth);
            Toggle("Health value", nullptr, &drawHealthValues);
            Toggle("Skeleton", nullptr, &drawBones);
            EndPanel();

            ImGui::TableNextColumn();
            BeginPanel("##visual_style", "Labels & style", "Appearance and extra information", 490);
            Toggle("Hero names", nullptr, &drawNames);
            Toggle("Player names", nullptr, &drawPlayerNames);
            Toggle("Distance", nullptr, &drawDistance);
            Toggle("Snaplines", nullptr, &drawSnaplines);
            Toggle("Model glow", nullptr, &glowEnabled);
            Toggle("FOV circle", nullptr, &drawFovCircle);
            Slider("Box thickness", &boxThickness, 0.5f, 4.0f, "%.1f px");
            Slider("Corner length", &cornerBoxLength, 0.10f, 0.35f, "%.2f");
            ImGui::TextColored(secondary, "Colors");
            ImGui::ColorEdit4("Enemy##menu_v2", enemyBoxColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
            ImGui::SameLine();
            ImGui::ColorEdit4("Team##menu_v2", teammateBoxColor,
                              ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaPreviewHalf);
            EndPanel();
        } else if (activeTab == 1) {
            BeginPanel("##aim_general", "General", "Main targeting behavior", 490);
            Toggle("Aim assist", "Enable player targeting", &aimAssist);
            Toggle("Visibility check", "Ignore occluded targets", &aimVisibilityCheck);
            int aimMode = aimMixedMode ? 2 : (aimSilentMode ? 1 : 0);
            const char* aimModes[] = { "Normal", "pSilent", "Mixed" };
            Combo("Aim mode", &aimMode, aimModes, 3);
            aimSilentMode = aimMode == 1;
            aimMixedMode = aimMode == 2;
            int bindMode = aimToggleMode ? 1 : 0;
            const char* bindModes[] = { "Hold", "Toggle" };
            Combo("Activation", &bindMode, bindModes, 2);
            aimToggleMode = bindMode == 1;
            ImGui::TextColored(secondary, "Aim key");
            KeyButton("aim_key", aimKeyCapture, AimKeyName(aimAssistKey), &aimKeyCapture);
            EndPanel();

            ImGui::TableNextColumn();
            BeginPanel("##aim_target", "Targeting", "Selection and precision", 490);
            DrawAimBoneSelectorImGui();
            Slider("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
            Slider("Smoothing", &aimSmooth, 1.0f, 20.0f, "%.1f");
            Toggle("Draw FOV", "Show active target radius", &drawFovCircle);
            Slider("FOV opacity", &fovCircleAlpha, 0.0f, 255.0f, "%.0f");
            EndPanel();
        } else if (activeTab == 2) {
            BeginPanel("##farm_creeps", "Creep assist", "Lane and neutral targeting", 490);
            Toggle("Creep aim", "Enable creep targeting", &farmAssist);
            Toggle("Creep ESP", "Highlight valid creeps", &drawCreepEsp);
            int farmMode = farmSilentMode ? 1 : 0;
            const char* farmModes[] = { "Normal", "Silent" };
            Combo("Aim mode", &farmMode, farmModes, 2);
            farmSilentMode = farmMode == 1;
            int farmBind = farmToggleMode ? 1 : 0;
            const char* farmBinds[] = { "Hold", "Toggle" };
            Combo("Activation", &farmBind, farmBinds, 2);
            farmToggleMode = farmBind == 1;
            Slider("Farm FOV", &farmFov, 40.0f, 600.0f, "%.0f px");
            Slider("Smoothing", &farmAimSmooth, 1.0f, 20.0f, "%.1f");
            ImGui::TextColored(secondary, "Farm key");
            KeyButton("farm_key", farmKeyCapture, AimKeyName(farmAssistKey), &farmKeyCapture);
            EndPanel();

            ImGui::TableNextColumn();
            BeginPanel("##farm_orbs", "Soul orbs", "Automatic orb handling", 490);
            Toggle("Orb ESP", "Highlight active soul orbs", &drawOrbEsp);
            Toggle("Orb aim", "Aim at valid soul orbs", &autoLastHitOrbs);
            Toggle("Visibility check", "Ignore occluded orbs", &orbAimVisibilityCheck);
            int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
            const char* fireModes[] = { "Auto fire", "Player fire" };
            Combo("Fire mode", &fireMode, fireModes, 2);
            autoLastHitOrbsAutoFire = fireMode == 0;
            int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
            const char* orbBinds[] = { "Hold", "Toggle" };
            Combo("Activation", &orbBind, orbBinds, 2);
            autoLastHitOrbsToggleMode = orbBind == 1;
            ImGui::TextColored(secondary, "Orb key");
            KeyButton("orb_key", autoLastHitOrbsKeyCapture,
                      AimKeyName(autoLastHitOrbsKey), &autoLastHitOrbsKeyCapture);
            Toggle("Farm FOV circle", nullptr, &drawFarmFovCircle);
            Slider("FOV opacity", &farmFovAlpha, 0.0f, 255.0f, "%.0f");
            EndPanel();
        } else {
            BeginPanel("##misc_utility", "Utility", "Gameplay convenience", 490);
            Toggle("Auto parry", "Automatically use parry", &autoParry);
            Toggle("Spectator list", "Show current observers", &drawSpectatorList);
            Toggle("Free camera", "Detach camera from player", &freeCam);
            Slider("Freecam speed", &freeCamSpeed, 50.0f, 5000.0f, "%.0f u/s");
            ImGui::TextColored(secondary, "Freecam key");
            KeyButton("freecam_key", freeCamKeyCapture,
                      AimKeyName(freeCamKey), &freeCamKeyCapture);
            EndPanel();

            ImGui::TableNextColumn();
            BeginPanel("##misc_session", "Session", "Runtime information and controls", 490);
            ImGui::TextColored(secondary, "Performance");
            ImGui::PushFont(heading);
            ImGui::TextColored(primary, "%.0f FPS", io.Framerate);
            ImGui::PopFont();
            ImGui::TextColored(secondary, "%zu players in snapshot", playerCount);
            ImGui::Dummy(ImVec2(0, 18));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 18));
            ImGui::TextColored(secondary, "Unload the module safely from the game process.");
            ImGui::PushStyleColor(ImGuiCol_Button, accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(1.0f, 0.16f, 0.28f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            if (ImGui::Button("Unload DLL", ImVec2(-1, 38))) RequestUnload();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            EndPanel();
        }
        ImGui::EndTable();
    }

    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (wasMenuOpen && !menuOpen) {
        SaveConfig();
        SetMenuOpen(false);
    }
}

void RenderMenu(size_t playerCount) {
    if (!menuOpen) return;

    ImGuiIO& io = ImGui::GetIO();
    ImFont* regular = io.FontDefault;
    ImFont* bold = io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : regular;
    const float dt = (std::min)(io.DeltaTime, 0.05f);
    const float ease = 1.0f - std::exp(-dt * 16.0f);
    static int activeTab = 0;
    static int visualTeam = 0;
    static float pageAlpha = 1.0f;
    static float pageOffset = 0.0f;
    static std::unordered_map<ImGuiID, float> toggleAnim;
    static std::unordered_map<ImGuiID, float> sliderAnim;
    pageAlpha += (1.0f - pageAlpha) * ease;
    pageOffset += (0.0f - pageOffset) * ease;
    const bool wasMenuOpen = menuOpen;

    const ImVec4 background(0.027f, 0.029f, 0.037f, 0.985f);
    const ImVec4 header(0.035f, 0.037f, 0.046f, 0.72f);
    const ImVec4 side(0.032f, 0.034f, 0.043f, 1.0f);
    const ImVec4 card(0.055f, 0.058f, 0.070f, 0.98f);
    const ImVec4 cardHover(0.072f, 0.075f, 0.090f, 1.0f);
    const ImVec4 red(0.94f, 0.035f, 0.13f, 1.0f);
    const ImVec4 redSoft(0.94f, 0.035f, 0.13f, 0.14f);
    const ImVec4 white(0.94f, 0.94f, 0.96f, 1.0f);
    const ImVec4 grey(0.58f, 0.59f, 0.64f, 1.0f);
    const ImVec4 border(0.17f, 0.18f, 0.22f, 0.82f);

    const float menuWidth = (std::min)(1180.0f, io.DisplaySize.x * 0.865f);
    const float menuHeight = (std::min)(700.0f, io.DisplaySize.y * 0.892f);
    const float headerHeight = menuHeight * 0.089f;
    const float sidebarWidth = menuWidth * 0.214f;
    const float bodyHeight = menuHeight - headerHeight;
    const float contentWidth = menuWidth - sidebarWidth;
    const float visualCardTop = 112.0f;
    const float normalCardTop = 66.0f;
    ImGui::SetNextWindowSize(ImVec2(menuWidth, menuHeight), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    if (!ImGui::Begin("##deadlock_reference_menu", &menuOpen,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar)) {
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
        return;
    }

    ImDrawList* root = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImU32 redU32 = ImGui::ColorConvertFloat4ToU32(red);
    const ImU32 whiteU32 = ImGui::ColorConvertFloat4ToU32(white);
    const ImU32 greyU32 = ImGui::ColorConvertFloat4ToU32(grey);
    const ImU32 borderU32 = ImGui::ColorConvertFloat4ToU32(border);

    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    backdrop->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(3, 4, 8, 150));
    root->AddRectFilledMultiColor(
        wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
        IM_COL32(8, 10, 15, 250), IM_COL32(16, 11, 18, 250),
        IM_COL32(12, 10, 16, 250), IM_COL32(7, 9, 14, 250));
    root->AddRectFilledMultiColor(
        wp, ImVec2(wp.x + ws.x, wp.y + headerHeight),
        IM_COL32(12, 15, 21, 255), IM_COL32(22, 16, 23, 255),
        IM_COL32(16, 12, 18, 255), IM_COL32(10, 13, 19, 255));
    root->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + headerHeight),
                        ImGui::ColorConvertFloat4ToU32(header), 13.0f,
                        ImDrawFlags_RoundCornersTop);
    root->AddLine(ImVec2(wp.x, wp.y + headerHeight),
                  ImVec2(wp.x + ws.x, wp.y + headerHeight), borderU32);
    root->AddLine(ImVec2(wp.x + sidebarWidth, wp.y + headerHeight),
                  ImVec2(wp.x + sidebarWidth, wp.y + ws.y), borderU32);
    root->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                  IM_COL32(235, 12, 55, 45), 13.0f, 0, 2.0f);

    // Angular red logo matching the reference silhouette.
    const ImVec2 lp(wp.x + 27, wp.y + 15);
    root->AddTriangleFilled(ImVec2(lp.x + 18, lp.y), ImVec2(lp.x, lp.y + 35),
                            ImVec2(lp.x + 12, lp.y + 31), redU32);
    root->AddTriangleFilled(ImVec2(lp.x + 18, lp.y), ImVec2(lp.x + 39, lp.y + 39),
                            ImVec2(lp.x + 29, lp.y + 34), redU32);
    root->AddTriangleFilled(ImVec2(lp.x + 11, lp.y + 25), ImVec2(lp.x + 34, lp.y + 17),
                            ImVec2(lp.x + 29, lp.y + 28), redU32);
    root->AddTriangleFilled(ImVec2(lp.x + 6, lp.y + 40), ImVec2(lp.x + 40, lp.y + 40),
                            ImVec2(lp.x + 32, lp.y + 31), redU32);
    ImGui::SetCursorPos(ImVec2(78, 19));
    ImGui::PushFont(bold);
    ImGui::TextColored(white, "Deadlock");
    ImGui::PopFont();
    ImGui::SetCursorPos(ImVec2(ws.x - 47, 17));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, redSoft);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(red.x, red.y, red.z, 0.24f));
    if (ImGui::Button("X", ImVec2(30, 30))) menuOpen = false;
    ImGui::PopStyleColor(3);

    auto DrawTabIcon = [&](ImDrawList* dl, int index, const ImVec2& c, ImU32 color) {
        if (index == 0) {
            dl->AddEllipse(c, ImVec2(14, 8), color, 0.0f, 24, 1.6f);
            dl->AddCircleFilled(c, 3.2f, color);
        } else if (index == 1) {
            dl->AddCircle(c, 10, color, 24, 1.6f);
            dl->AddCircle(c, 3, color, 20, 1.4f);
            dl->AddLine(ImVec2(c.x - 15, c.y), ImVec2(c.x - 7, c.y), color, 1.5f);
            dl->AddLine(ImVec2(c.x + 7, c.y), ImVec2(c.x + 15, c.y), color, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y - 15), ImVec2(c.x, c.y - 7), color, 1.5f);
            dl->AddLine(ImVec2(c.x, c.y + 7), ImVec2(c.x, c.y + 15), color, 1.5f);
        } else if (index == 2) {
            dl->AddLine(ImVec2(c.x, c.y + 12), ImVec2(c.x, c.y - 8), color, 1.7f);
            dl->AddBezierCubic(ImVec2(c.x, c.y), ImVec2(c.x - 12, c.y - 3),
                               ImVec2(c.x - 10, c.y - 13), ImVec2(c.x - 2, c.y - 10), color, 1.7f);
            dl->AddBezierCubic(ImVec2(c.x, c.y - 4), ImVec2(c.x + 11, c.y - 7),
                               ImVec2(c.x + 10, c.y - 16), ImVec2(c.x + 2, c.y - 13), color, 1.7f);
        } else {
            dl->AddCircle(c, 10, color, 20, 1.7f);
            dl->AddCircle(c, 3.5f, color, 18, 1.5f);
            for (int i = 0; i < 8; ++i) {
                const float a = i * 0.78539816f;
                dl->AddLine(ImVec2(c.x + std::cos(a) * 11, c.y + std::sin(a) * 11),
                            ImVec2(c.x + std::cos(a) * 15, c.y + std::sin(a) * 15), color, 2.0f);
            }
        }
    };

    auto Toggle = [&](const char* label, bool* value, const float* colorValue) {
        ImGui::PushID(label);
        const ImGuiID id = ImGui::GetID("##toggle_anim");
        float& a = toggleAnim[id];
        a += ((*value ? 1.0f : 0.0f) - a) * ease;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        constexpr float rowHeight = 48.0f;
        ImGui::InvisibleButton("##row", ImVec2(width, rowHeight));
        if (ImGui::IsItemClicked()) *value = !*value;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (ImGui::IsItemHovered())
            dl->AddRectFilled(p, ImVec2(p.x + width, p.y + rowHeight),
                              ImGui::ColorConvertFloat4ToU32(cardHover), 5.0f);
        dl->AddText(ImVec2(p.x + 5, p.y + 14), whiteU32, label);
        const float colorSpace = colorValue ? 34.0f : 0.0f;
        const float tx = p.x + width - 48.0f - colorSpace;
        const ImVec4 off(0.18f, 0.19f, 0.23f, 1.0f);
        const ImVec4 track(off.x + (red.x - off.x) * a,
                           off.y + (red.y - off.y) * a,
                           off.z + (red.z - off.z) * a, 1.0f);
        dl->AddRectFilled(ImVec2(tx, p.y + 16), ImVec2(tx + 42, p.y + 38),
                          ImGui::ColorConvertFloat4ToU32(track), 11.0f);
        if (a > 0.01f) {
            dl->AddRectFilled(ImVec2(tx + 2, p.y + 18),
                              ImVec2(tx + 40, p.y + 27),
                              IM_COL32(255, 91, 112, static_cast<int>(80.0f * a)),
                              8.0f, ImDrawFlags_RoundCornersTop);
            dl->AddRect(ImVec2(tx - 2, p.y + 14), ImVec2(tx + 44, p.y + 40),
                        IM_COL32(242, 9, 49, static_cast<int>(45.0f * a)),
                        13.0f, 0, 3.0f);
        }
        dl->AddCircleFilled(ImVec2(tx + 11 + a * 20, p.y + 27), 8.0f,
                            IM_COL32(249, 249, 251, 255));
        if (colorValue) {
            dl->AddRectFilled(ImVec2(p.x + width - 27, p.y + 16),
                              ImVec2(p.x + width - 7, p.y + 37),
                              ImGui::ColorConvertFloat4ToU32(
                                  ImVec4(colorValue[0], colorValue[1], colorValue[2], 1.0f)));
            dl->AddRect(ImVec2(p.x + width - 27, p.y + 16),
                        ImVec2(p.x + width - 7, p.y + 37), borderU32);
        }
        dl->AddLine(ImVec2(p.x + 4, p.y + rowHeight - 1),
                    ImVec2(p.x + width - 4, p.y + rowHeight - 1),
                    IM_COL32(42, 43, 52, 160));
        ImGui::PopID();
    };

    auto Slider = [&](const char* label, float* value, float minimum, float maximum,
                      const char* format) {
        ImGui::PushID(label);
        const ImGuiID id = ImGui::GetID("##slider_anim");
        const float fraction = std::clamp((*value - minimum) / (maximum - minimum), 0.0f, 1.0f);
        float& animated = sliderAnim[id];
        animated += (fraction - animated) * ease;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##slider", ImVec2(width, 49));
        const float trackStart = p.x + 145.0f;
        const float trackEnd = p.x + width - 104.0f;
        if (ImGui::IsItemActive()) {
            const float f = std::clamp((io.MousePos.x - trackStart) /
                                       (trackEnd - trackStart), 0.0f, 1.0f);
            *value = minimum + (maximum - minimum) * f;
        }
        char out[48]{};
        std::snprintf(out, sizeof(out), format, *value);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRect(ImVec2(p.x + 4, p.y + 8), ImVec2(p.x + width - 4, p.y + 46),
                    IM_COL32(38, 40, 49, 210), 6.0f);
        dl->AddText(ImVec2(p.x + 18, p.y + 18), whiteU32, label);
        dl->AddRectFilled(ImVec2(trackStart, p.y + 26), ImVec2(trackEnd, p.y + 30),
                          IM_COL32(48, 50, 59, 255), 2.0f);
        const float knob = trackStart + (trackEnd - trackStart) * animated;
        dl->AddRectFilled(ImVec2(trackStart, p.y + 26), ImVec2(knob, p.y + 30), redU32, 2.0f);
        dl->AddCircleFilled(ImVec2(knob, p.y + 28), 8.0f, IM_COL32(15, 16, 21, 255));
        dl->AddCircle(ImVec2(knob, p.y + 28), 8.0f, redU32, 24, 1.8f);
        dl->AddCircleFilled(ImVec2(knob, p.y + 28), 3.5f, IM_COL32(245, 245, 248, 255));
        dl->AddRect(ImVec2(p.x + width - 88, p.y + 8), ImVec2(p.x + width - 4, p.y + 46),
                    IM_COL32(43, 45, 54, 255), 6.0f);
        const ImVec2 textSize = ImGui::CalcTextSize(out);
        dl->AddText(ImVec2(p.x + width - 46 - textSize.x * 0.5f, p.y + 18), greyU32, out);
        ImGui::PopID();
    };

    auto Combo = [&](const char* label, int* value, const char* const* items, int count) {
        ImGui::PushID(label);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::TextColored(white, "%s", label);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(p.x + ImGui::GetContentRegionAvail().x - 125, p.y - 5));
        ImGui::SetNextItemWidth(125);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.075f, 0.078f, 0.092f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, cardHover);
        ImGui::PushStyleColor(ImGuiCol_Header, redSoft);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(red.x, red.y, red.z, 0.28f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (ImGui::BeginCombo("##combo", items[*value])) {
            for (int i = 0; i < count; ++i)
                if (ImGui::Selectable(items[i], *value == i)) *value = i;
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::Dummy(ImVec2(0, 15));
        ImGui::PopID();
    };

    auto KeyBind = [&](const char* id, bool capture, const char* key, bool* captureState) {
        ImGui::PushID(id);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.075f, 0.078f, 0.092f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, cardHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, redSoft);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        if (ImGui::Button(capture ? "Press any key..." : key, ImVec2(-1, 36))) *captureState = true;
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    };

    auto BeginCard = [&](const char* id, const char* title) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, card);
        ImGui::PushStyleColor(ImGuiCol_Border, border);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 9.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(13, 10));
        const float cardTop = activeTab == 0 ? visualCardTop : normalCardTop;
        const float cardHeight = bodyHeight - cardTop - 18.0f;
        const float cardWidth = ImGui::GetContentRegionAvail().x - 18.0f;
        ImGui::BeginChild(id, ImVec2(cardWidth, cardHeight), ImGuiChildFlags_Borders, 0);
        const ImVec2 cardPos = ImGui::GetWindowPos();
        const ImVec2 cardSize = ImGui::GetWindowSize();
        ImGui::GetWindowDrawList()->AddRectFilledMultiColor(
            cardPos, ImVec2(cardPos.x + cardSize.x, cardPos.y + cardSize.y),
            IM_COL32(17, 19, 25, 225), IM_COL32(22, 21, 27, 225),
            IM_COL32(18, 19, 24, 225), IM_COL32(14, 17, 22, 225));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRect(ImVec2(p.x, p.y), ImVec2(p.x + 27, p.y + 27),
                                            redU32, 5.0f, 0, 1.4f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 7, p.y + 7), ImVec2(p.x + 12, p.y + 7), redU32, 1.5f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 7, p.y + 7), ImVec2(p.x + 7, p.y + 12), redU32, 1.5f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 20, p.y + 20), ImVec2(p.x + 15, p.y + 20), redU32, 1.5f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 20, p.y + 20), ImVec2(p.x + 20, p.y + 15), redU32, 1.5f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 38);
        ImGui::PushFont(bold);
        ImGui::TextColored(white, "%s", title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 8));
    };
    auto EndCard = [&]() {
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    };

    // Sidebar, including glowing active tab.
    ImGui::SetCursorPos(ImVec2(0, headerHeight));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, side);
    ImGui::BeginChild("##reference_sidebar", ImVec2(sidebarWidth, bodyHeight), false, 0);
    const char* tabNames[] = { "Visuals", "Aim assist", "Farm assist", "Misc" };
    for (int i = 0; i < 4; ++i) {
        ImGui::SetCursorPos(ImVec2(17, 18 + i * 77.0f));
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool selected = i == activeTab;
        const float tabWidth = sidebarWidth - 34.0f;
        ImGui::InvisibleButton("##tab", ImVec2(tabWidth, 58));
        if (ImGui::IsItemClicked()) {
            activeTab = i;
            pageAlpha = 0.0f;
            pageOffset = 14.0f;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) {
            dl->AddRectFilled(p, ImVec2(p.x + tabWidth, p.y + 58),
                              IM_COL32(18, 15, 21, 248), 8.0f);
            // Soft red light bleeding from the border into the dark center.
            dl->AddRectFilledMultiColor(
                ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + tabWidth - 2, p.y + 56),
                IM_COL32(239, 8, 49, 54), IM_COL32(239, 8, 49, 10),
                IM_COL32(239, 8, 49, 18), IM_COL32(239, 8, 49, 48));
            dl->AddRect(ImVec2(p.x + 2, p.y + 2),
                        ImVec2(p.x + tabWidth - 2, p.y + 56),
                        IM_COL32(248, 18, 58, 90), 7.0f, 0, 3.0f);
            dl->AddRect(ImVec2(p.x - 2, p.y - 2),
                        ImVec2(p.x + tabWidth + 2, p.y + 60),
                        IM_COL32(238, 10, 51, 42), 10.0f, 0, 7.0f);
            dl->AddLine(ImVec2(p.x + 10, p.y + 1),
                        ImVec2(p.x + tabWidth - 10, p.y + 1),
                        IM_COL32(255, 66, 93, 225), 1.0f);
            dl->AddRect(p, ImVec2(p.x + tabWidth, p.y + 58), redU32, 8.0f, 0, 1.4f);
            dl->AddCircleFilled(ImVec2(p.x + tabWidth - 22, p.y + 29), 4.5f, redU32);
        } else if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(p, ImVec2(p.x + tabWidth, p.y + 58),
                              IM_COL32(255, 255, 255, 8), 8.0f);
        }
        DrawTabIcon(dl, i, ImVec2(p.x + 27, p.y + 29), selected ? whiteU32 : greyU32);
        dl->AddText(ImVec2(p.x + 54, p.y + 20), selected ? redU32 : greyU32, tabNames[i]);
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Page heading and body.
    ImGui::SetCursorPos(ImVec2(sidebarWidth, headerHeight));
    ImGui::BeginChild("##reference_content", ImVec2(contentWidth, bodyHeight), false,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, pageAlpha);
    ImGui::SetCursorPos(ImVec2(28 + pageOffset, 20));
    ImGui::PushFont(bold);
    ImGui::TextColored(white, "%s", tabNames[activeTab]);
    ImGui::PopFont();

    if (activeTab == 0) {
        ImGui::SetCursorPos(ImVec2(28 + pageOffset, 62));
        auto Segment = [&](const char* label, int value) {
            ImGui::PushID(value);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool selected = visualTeam == value;
            ImGui::InvisibleButton("##segment", ImVec2(145, 34));
            if (ImGui::IsItemClicked()) visualTeam = value;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (selected) {
                dl->AddRectFilled(ImVec2(p.x - 3, p.y - 3),
                                  ImVec2(p.x + 148, p.y + 37),
                                  IM_COL32(239, 9, 51, 34), 9.0f);
                dl->AddRectFilled(p, ImVec2(p.x + 145, p.y + 34),
                                  IM_COL32(178, 5, 34, 255), 7.0f);
                dl->AddRectFilled(ImVec2(p.x + 3, p.y + 3),
                                  ImVec2(p.x + 142, p.y + 17),
                                  IM_COL32(255, 42, 76, 100), 5.0f,
                                  ImDrawFlags_RoundCornersTop);
                dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8),
                                  ImVec2(p.x + 137, p.y + 29),
                                  IM_COL32(255, 12, 55, 28), 5.0f);
                dl->AddLine(ImVec2(p.x + 10, p.y + 1),
                            ImVec2(p.x + 135, p.y + 1),
                            IM_COL32(255, 105, 125, 220), 1.0f);
                dl->AddRect(p, ImVec2(p.x + 145, p.y + 34), redU32, 7.0f, 0, 1.0f);
            } else {
                dl->AddRectFilled(p, ImVec2(p.x + 145, p.y + 34),
                                  IM_COL32(26, 27, 34, 255), 7.0f);
                if (ImGui::IsItemHovered())
                    dl->AddRectFilled(p, ImVec2(p.x + 145, p.y + 34),
                                      IM_COL32(255, 255, 255, 8), 7.0f);
            }
            const ImVec2 size = ImGui::CalcTextSize(label);
            dl->AddText(ImVec2(p.x + 72.5f - size.x * 0.5f,
                               p.y + 17.0f - size.y * 0.5f),
                        selected ? whiteU32 : greyU32, label);
            ImGui::PopID();
        };
        Segment("Enemy", 0);
        ImGui::SameLine(0, 0);
        Segment("Ally", 1);
    }

    ImGui::SetCursorPos(ImVec2(18 + pageOffset, activeTab == 0 ? 112.0f : 66.0f));
    BeginCard("##reference_card", activeTab == 0 ? "Overlay" :
              activeTab == 1 ? "Aim configuration" :
              activeTab == 2 ? "Farm configuration" : "Miscellaneous");
    ImGui::Dummy(ImVec2(0, 2));

    if (ImGui::BeginTable("##reference_columns", 2,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("##a", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##b", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextColumn();
        ImGui::PushID("left");

        if (activeTab == 0) {
            Toggle("Enable ESP", &drawEsp, nullptr);
            if (visualTeam == 0)
                Toggle("Radar", &enemyRadarEnabled, nullptr);
            Toggle("Bounding boxes", &drawBoxes, visualTeam == 0 ? enemyBoxColor : teammateBoxColor);
            Slider("Box thickness", &boxThickness, 0.5f, 4.0f, "%.2f px");
            Toggle("Corner boxes", &cornerBoxes, enemyHealthColor);
            Slider("Corner length", &cornerBoxLength, 0.10f, 0.35f, "%.2f");
            Toggle("Health bar", &drawHealth, enemyHealthColor);
            Toggle("Health value", &drawHealthValues, teammateHealthColor);
            Toggle("Skeleton", &drawBones, enemyNameColor);
        } else if (activeTab == 1) {
            Toggle("Enable aim assist", &aimAssist, nullptr);
            Toggle("Visibility check", &aimVisibilityCheck, nullptr);
            int aimMode = aimMixedMode ? 2 : (aimSilentMode ? 1 : 0);
            const char* aimModes[] = { "Normal", "pSilent", "Mixed" };
            Combo("Aim mode", &aimMode, aimModes, 3);
            aimSilentMode = aimMode == 1;
            aimMixedMode = aimMode == 2;
            int bindMode = aimToggleMode ? 1 : 0;
            const char* bindModes[] = { "Hold", "Toggle" };
            Combo("Activation", &bindMode, bindModes, 2);
            aimToggleMode = bindMode == 1;
            ImGui::TextColored(grey, "Aim key");
            KeyBind("aim_key_v3", aimKeyCapture, AimKeyName(aimAssistKey), &aimKeyCapture);
        } else if (activeTab == 2) {
            Toggle("Enable creep aim", &farmAssist, nullptr);
            Toggle("Creep ESP", &drawCreepEsp, nullptr);
            int farmMode = farmSilentMode ? 1 : 0;
            const char* farmModes[] = { "Normal", "Silent" };
            Combo("Farm mode", &farmMode, farmModes, 2);
            farmSilentMode = farmMode == 1;
            int farmBind = farmToggleMode ? 1 : 0;
            const char* farmBinds[] = { "Hold", "Toggle" };
            Combo("Activation", &farmBind, farmBinds, 2);
            farmToggleMode = farmBind == 1;
            Slider("Farm FOV", &farmFov, 40.0f, 600.0f, "%.0f px");
            Slider("Smoothing", &farmAimSmooth, 1.0f, 20.0f, "%.1f");
            ImGui::TextColored(grey, "Farm key");
            KeyBind("farm_key_v3", farmKeyCapture, AimKeyName(farmAssistKey), &farmKeyCapture);
        } else {
            Toggle("Auto parry", &autoParry, nullptr);
            Toggle("Spectator list", &drawSpectatorList, nullptr);
            Toggle("Free camera", &freeCam, nullptr);
            Slider("Freecam speed", &freeCamSpeed, 50.0f, 5000.0f, "%.0f u/s");
            ImGui::TextColored(grey, "Freecam key");
            KeyBind("freecam_key_v3", freeCamKeyCapture,
                    AimKeyName(freeCamKey), &freeCamKeyCapture);
        }
        ImGui::PopID();

        ImGui::TableNextColumn();
        ImGui::PushID("right");
        if (activeTab == 0) {
            Toggle("Hero names", &drawNames, enemyNameColor);
            Toggle("Player names", &drawPlayerNames, teammateNameColor);
            Toggle("Distance", &drawDistance, enemyHealthColor);
            Toggle("Snaplines", &drawSnaplines, nullptr);
            Toggle("Model glow", &glowEnabled, enemyBoxColor);
            Toggle("FOV circle", &drawFovCircle, teammateBoxColor);
            Slider("FOV opacity", &fovCircleAlpha, 0.0f, 255.0f, "%.0f");
        } else if (activeTab == 1) {
            DrawAimBoneSelectorImGui();
            Slider("Aim FOV", &aimFov, 40.0f, 600.0f, "%.0f px");
            Slider("Smoothing", &aimSmooth, 1.0f, 20.0f, "%.1f");
            Toggle("Draw FOV circle", &drawFovCircle, nullptr);
            Slider("FOV opacity", &fovCircleAlpha, 0.0f, 255.0f, "%.0f");
        } else if (activeTab == 2) {
            Toggle("Orb ESP", &drawOrbEsp, nullptr);
            Toggle("Orb aim", &autoLastHitOrbs, nullptr);
            Toggle("Orb visibility check", &orbAimVisibilityCheck, nullptr);
            int fireMode = autoLastHitOrbsAutoFire ? 0 : 1;
            const char* fireModes[] = { "Auto fire", "Player fire" };
            Combo("Fire mode", &fireMode, fireModes, 2);
            autoLastHitOrbsAutoFire = fireMode == 0;
            int orbBind = autoLastHitOrbsToggleMode ? 1 : 0;
            const char* orbBinds[] = { "Hold", "Toggle" };
            Combo("Orb activation", &orbBind, orbBinds, 2);
            autoLastHitOrbsToggleMode = orbBind == 1;
            ImGui::TextColored(grey, "Orb key");
            KeyBind("orb_key_v3", autoLastHitOrbsKeyCapture,
                    AimKeyName(autoLastHitOrbsKey), &autoLastHitOrbsKeyCapture);
        } else {
            ImGui::PushFont(bold);
            ImGui::TextColored(white, "Session");
            ImGui::PopFont();
            ImGui::TextColored(grey, "%.0f FPS", io.Framerate);
            ImGui::TextColored(grey, "%zu players in snapshot", playerCount);
            ImGui::Dummy(ImVec2(0, 22));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 22));
            ImGui::TextWrapped("Unload the module and restore all hooks safely.");
            ImGui::PushStyleColor(ImGuiCol_Button, red);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.10f, 0.20f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f);
            if (ImGui::Button("Unload DLL", ImVec2(-1, 40))) RequestUnload();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
        ImGui::PopID();
        ImGui::EndTable();
    }
    EndCard();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (wasMenuOpen && !menuOpen) {
        SaveConfig();
        SetMenuOpen(false);
    }
}
