#include "shared.h"
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
            const uint32_t handle = Read<uint32_t>(identity + 0x10);
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
            const uint32_t handle = Read<uint32_t>(identity + 0x10);
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
    const uint32_t storedHandle = Read<uint32_t>(identity + 0x10);
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
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\collision_diagnostic.csv",
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
    // Bone-world calculation is one of the most expensive calls in the
    // render path.  ESP boxes only need the scene origin; calculate bones
    // when a feature actually consumes them.
    const bool aimKeyDown = (GetAsyncKeyState(aimAssistKey) & 0x8000) != 0;
    const bool farmKeyDown = (GetAsyncKeyState(farmAssistKey) & 0x8000) != 0;
    const bool aimNeedsBones = aimAssist &&
        (aimToggleMode ? aimToggleActive : aimKeyDown);
    const bool farmNeedsBones = farmAssist &&
        (farmToggleMode ? farmToggleActive : farmKeyDown);
    const bool needPlayerBones = drawBones || aimNeedsBones || farmNeedsBones;

    for (const uintptr_t entity : pawns) {
        const int health = Read<int>(entity + Offsets::Health);
        if (health <= 0) continue;

        const uint8_t lifeState = Read<uint8_t>(entity + Offsets::LifeState);
        if (lifeState != 0) continue;

        const uint8_t team = Read<uint8_t>(entity + Offsets::Team);
        if (team != 2 && team != 3) continue;
        if (localTeam != 0 && team == localTeam) continue;

        Vector3 pos{};
        // Visual ESP must follow the interpolated render transform. AbsOrigin
        // advances at the network tick rate and makes boxes/text visibly step.
        if (!GetEntityRenderPosition(entity, pos)) continue;

        PlayerData player;
        player.entity = entity;
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
        if (needPlayerBones) {
            player.hasHeadBone = GetEntityBonePosition(entity, "head", player.headPos);
            player.hasBodyBone = GetEntityBonePosition(entity, "spine_2", player.bodyPos);
            if (!player.hasBodyBone) player.hasBodyBone = GetEntityBonePosition(entity, "spine_0", player.bodyPos);
            if (drawBones) GetEntityBoneSkeleton(entity, player.bones);
        }
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
        static std::vector<std::string> cachedSpectators;
        static ULONGLONG lastSpectatorScan = 0;
        const ULONGLONG spectatorNow = GetTickCount64();
        if (spectatorNow - lastSpectatorScan >= 1000) {
            // Enumerate player controllers directly. Spectators use
            // C_CitadelObserverPawn, which is intentionally not part of heroPawns.
            std::vector<std::string> refreshedSpectators;
            std::unordered_set<std::string> spectatorNames;
            const uintptr_t entityRoot = clientBase
                ? Read<uintptr_t>(clientBase + Offsets::GameEntitySystem) : 0;
            if (entityRoot) {
        const int reportedHighest = Read<int>(entityRoot + Offsets::HighestEntityIndex);
        const uint32_t highestEntityIndex =
            reportedHighest > 0 && reportedHighest <= static_cast<int>(Offsets::HandleIndexMask)
                ? static_cast<uint32_t>(reportedHighest)
                : Offsets::HandleIndexMask;
        const uint32_t highestChunk = highestEntityIndex >> Offsets::HandleChunkShift;
        for (uint32_t chunkIndex = 0;
             chunkIndex <= highestChunk; ++chunkIndex) {
            const uintptr_t chunk = Read<uintptr_t>(entityRoot + Offsets::EntityChunks +
                Offsets::EntityChunkStride * chunkIndex);
            if (!chunk) continue;
            const uint32_t highestSlot = chunkIndex == highestChunk
                ? (highestEntityIndex & Offsets::HandleChunkMask)
                : Offsets::HandleChunkMask;
            for (uint32_t slot = 0; slot <= highestSlot; ++slot) {
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
                // A spectator list must contain only controllers currently
                // observing the local pawn. Previously every observer in the
                // match was shown, regardless of their selected target.
                const uintptr_t observedPawn = ResolveEntity(observerTarget);
                if (!observedPawn || observedPawn != currentLocalPawn) continue;
                std::string name = ReadPlayerName(controller);
                if (name.empty()) name = "Unknown";
                if (spectatorNames.insert(name).second) refreshedSpectators.push_back(std::move(name));
            }
        }
            }
            cachedSpectators = std::move(refreshedSpectators);
            lastSpectatorScan = spectatorNow;
        }
        const auto& spectators = cachedSpectators;
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
                !std::isfinite(position.z) || !std::isfinite(screen.x) ||
                !std::isfinite(screen.y)) continue;
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
            if (IsXpOrbAttackable(orb.entity)) {
                drawList->AddCircleFilled(point, fillRadius, orbColor, 24);
            }
            drawList->AddCircle(point, outlineRadius, ImColor(255, 165, 45, 245), 24, 2.0f);
            drawList->AddText(ImVec2(screen.x + outlineRadius + 3.0f, screen.y - 8.0f),
                              ImColor(255, 255, 255, 240), orbLabel);
        }
    }

    if (drawCreepEsp && currentViewMatrixReady) {
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
        const float creepSmoothing =
            1.0f - std::exp(-140.0f * creepDt);

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
            if (!GetEntityRenderPosition(creep.entity, livePosition)) continue;
            const uintptr_t sceneNode = Read<uintptr_t>(creep.entity + Offsets::GameSceneNode);
            if (!sceneNode || Read<uint8_t>(sceneNode + Offsets::SceneNodeDormant) != 0) continue;
            creep.pos = livePosition;
            creep.health = Read<int>(creep.entity + Offsets::Health);
            creep.maxHealth = Read<int>(creep.entity + Offsets::MaxHealth);
            const uint8_t lifeState = Read<uint8_t>(creep.entity + Offsets::LifeState);
            const uint32_t npcState = Read<uint32_t>(creep.entity + Offsets::NPCState);
            if (creep.health <= 0 || lifeState != 0 || npcState == 5 || npcState == 6 ||
                Read<uint8_t>(creep.entity + Offsets::FadeCorpse) != 0) continue;

            // Scene-node state, life state and NPC state above are sufficient
            // to reject stale slots. Calling CalcWorldSpaceBones for every
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
            const ImColor color = neutral
                ? ImColor(190, 190, 190, 150)
                : (ally ? ImColor(90, 170, 255, 150) : ImColor(255, 170, 0, 220));
            drawList->AddRect(ImVec2(left, top), ImVec2(right, bottom), color, 0.0f, 0, ally ? 1.0f : 1.25f);

            Vector2 originScreen{};
            if (WorldToScreen(creep.pos, originScreen, currentViewMatrix)) {
                drawList->AddCircleFilled(
                    ImVec2(originScreen.x + originOffsetX,
                           originScreen.y + originOffsetY),
                    3.0f, color, 12);
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

    if (!drawEsp) return;

    Vector2 localScreen{};
    const bool localOnScreen = currentLocalPositionReady &&
                               WorldToScreen(currentLocalPosition, localScreen, currentViewMatrix);

    struct SmoothedEspBox {
        float left{}, top{}, right{}, bottom{};
        ULONGLONG lastSeen{};
        bool initialized{};
    };
    static std::unordered_map<uintptr_t, SmoothedEspBox> smoothedBoxes;
    const ULONGLONG smoothingNow = GetTickCount64();
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.050f);
    // A short time constant removes network-step shimmer without making the
    // box trail far behind the render model.
    const float smoothing = 1.0f - std::exp(-140.0f * dt);

    for (const auto& player : players) {
        auto& smooth = smoothedBoxes[player.entity];
        const float rawCenterX = (player.boxLeft + player.boxRight) * 0.5f;
        const float rawCenterY = (player.boxTop + player.boxBottom) * 0.5f;
        const float oldCenterX = (smooth.left + smooth.right) * 0.5f;
        const float oldCenterY = (smooth.top + smooth.bottom) * 0.5f;
        const bool stale = !smooth.initialized ||
            smoothingNow - smooth.lastSeen > 250 ||
            std::fabs(rawCenterX - oldCenterX) > 500.0f ||
            std::fabs(rawCenterY - oldCenterY) > 500.0f;
        if (stale) {
            smooth.left = player.boxLeft;
            smooth.top = player.boxTop;
            smooth.right = player.boxRight;
            smooth.bottom = player.boxBottom;
            smooth.initialized = true;
        } else {
            smooth.left +=
                (player.boxLeft - smooth.left) * smoothing;
            smooth.top +=
                (player.boxTop - smooth.top) * smoothing;
            smooth.right +=
                (player.boxRight - smooth.right) * smoothing;
            smooth.bottom +=
                (player.boxBottom - smooth.bottom) * smoothing;
        }
        smooth.lastSeen = smoothingNow;

        const float screenX = (smooth.left + smooth.right) * 0.5f;
        const float screenY = smooth.bottom;
        const float boxTop = smooth.top;
        const float boxHeight = smooth.bottom - smooth.top;
        const float boxWidth = smooth.right - smooth.left;
        const float screenOffsetX = screenX - rawCenterX;
        const float screenOffsetY =
            (smooth.top + smooth.bottom) * 0.5f - rawCenterY;
        const uint8_t localTeam = currentLocalPawn
            ? Read<uint8_t>(currentLocalPawn + Offsets::Team) : 0;
        const bool ally = localTeam != 0 && player.team == localTeam;

        if (drawBones) {
            for (const auto& bone : player.bones) {
                Vector2 start{}, end{};
                if (WorldToScreen(bone.start, start, currentViewMatrix) &&
                    WorldToScreen(bone.end, end, currentViewMatrix)) {
                    drawList->AddLine(
                        ImVec2(start.x + screenOffsetX,
                               start.y + screenOffsetY),
                        ImVec2(end.x + screenOffsetX,
                               end.y + screenOffsetY),
                        ImColor(255, 220, 40, 220), 1.5f);
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
                ImVec2(smooth.left, boxTop),
                ImVec2(smooth.right, screenY),
                ImColor(255, 0, 0, 255), 1.0f, 0, 1.0f
            );
        }

        if (drawHealth) {
            const float healthPercent = player.maxHealth > 0
                                            ? std::clamp(static_cast<float>(player.health) / player.maxHealth, 0.0f, 1.0f)
                                            : 0.0f;
            constexpr float barWidth = 4.0f;
            const float barLeft = smooth.left - 7.0f;

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
    for (auto it = smoothedBoxes.begin(); it != smoothedBoxes.end();) {
        if (smoothingNow - it->second.lastSeen > 1000)
            it = smoothedBoxes.erase(it);
        else
            ++it;
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
