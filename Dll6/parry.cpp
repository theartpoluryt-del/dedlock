#include "shared.h"
#include <fstream>
#include <cstring>

#define printf(...) do { } while (0)

namespace {

constexpr char kMeleeChargeSound[] = "Player.Melee.Hold.Shared";
constexpr ULONGLONG kMeleeSoundWindowMs = 600;
// Deadlock world units used by this project: approximately 200 units ~= 2 m.
constexpr float kSoundOnlyDistance = 200.0f;
constexpr float kLastMomentDistance = 100.0f;
constexpr float kMinApproachSpeed = 50.0f;
constexpr float kDefaultParryDelayMs = 500.0f;
constexpr float kCloseParryDelayMs = 250.0f;

std::mutex parrySoundMutex;
std::unordered_map<uint32_t, ULONGLONG> meleeSoundUntil;

} // namespace

void NotifyParrySound(int entityIndex, const char* soundName) {
    if (!autoParry || entityIndex <= 0 || !soundName ||
        std::strcmp(soundName, kMeleeChargeSound) != 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(parrySoundMutex);
    meleeSoundUntil[static_cast<uint32_t>(entityIndex) & Offsets::HandleIndexMask] =
        GetTickCount64() + kMeleeSoundWindowMs;
}

static void WriteSpeedMeasurement(uintptr_t pawn, uint8_t team, float distance, float closingSpeed) {
    // This diagnostic used to open/append a file from the Present thread for
    // every pawn every 100 ms.  In a teamfight that causes visible stalls on
    // the game's main thread.  Keep the hook/API surface intact, but make the
    // optional measurement side effect free.
    (void)pawn;
    (void)team;
    (void)distance;
    (void)closingSpeed;
}

void AutoParry(const std::vector<PlayerData>&) {
    if (!autoParry || menuOpen || !currentLocalPawn || !clientBase) return;

    static ULONGLONG lastParry = 0;
    static ULONGLONG lastScan = 0;
    static ULONGLONG lastDiagnostic = 0;
    static ULONGLONG lastPuppetVelocityLog = 0;
    struct VelocitySample {
        ULONGLONG time = 0;
        float speed = 0.0f;
    };
    static std::unordered_map<uintptr_t, VelocitySample> velocitySamples;
    const ULONGLONG now = GetTickCount64();
    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const auto isEnemy = [localTeam](uintptr_t pawn) {
        const uint8_t team = Read<uint8_t>(pawn + Offsets::Team);
        return (team == 2 || team == 3) && localTeam != 0 && team != localTeam;
    };
    if (now - lastScan < 8) return;
    lastScan = now;

    std::vector<uintptr_t> pawns;
    {
        std::lock_guard<std::mutex> lock(heroPawnsMutex);
        pawns = heroPawns;
    }

    static ULONGLONG lastPawnDiagnostic = 0;
    static ULONGLONG lastAbilityRefresh = 0;
    static bool abilityScanActive = false;
    static constexpr bool disableStaleAbilityScan = true;
    static size_t abilityScanRoot = 0;
    static size_t abilityScanTable = 0;
    static uint32_t abilityScanChunk = 0;
    static uint32_t abilityScanSlot = 0;
    static uintptr_t abilityScanChunkPtr = 0;

    struct CachedMeleeAbility {
        uintptr_t entity;
        uint32_t ownerHandle;
    };
    static std::vector<CachedMeleeAbility> meleeAbilities;
    static std::unordered_map<uintptr_t, bool> meleeVTables;
    struct MotionSample {
        Vector3 position{};
        ULONGLONG time = 0;
        ULONGLONG armedUntil = 0;
        ULONGLONG parryAt = 0;
        float closingSpeed = 0.0f;
    };
    static std::unordered_map<uintptr_t, MotionSample> enemyMotion;
    static ULONGLONG lastHeuristicParry = 0;
    static constexpr bool heuristicParryEnabled = true;
    static constexpr bool legacyStateParryEnabled = false;

    // The ability vector contains stale handles in this client build. Build a
    // small cache from live entity identities and their m_hOwnerEntity field.
    // The runtime dump confirmed the direct entity-system root. The previous
    // single-table scan completed but found zero HoldMelee entities, so check
    // each known identity-table offset under this root.
    const uintptr_t roots[] = { clientBase + Offsets::GameEntitySystem };
    const uintptr_t tableOffsets[] = { 0, 0x10, 0x110, 0x100, 0x20 };
    constexpr size_t scanBudget = 512;
    if (!disableStaleAbilityScan && !abilityScanActive && now - lastAbilityRefresh >= 1000) {
        meleeAbilities.clear();
        abilityScanActive = true;
        abilityScanRoot = abilityScanTable = 0;
        abilityScanChunk = abilityScanSlot = 0;
        abilityScanChunkPtr = 0;
    }
    if (abilityScanActive) {
        size_t processed = 0;
        while (processed++ < scanBudget && abilityScanRoot < 1) {
            const uintptr_t root = roots[abilityScanRoot];
            const uintptr_t tableOffset = tableOffsets[abilityScanTable];
            if (!root) {
                ++abilityScanRoot;
                abilityScanTable = 0;
                abilityScanChunk = abilityScanSlot = 0;
                abilityScanChunkPtr = 0;
            } else {
                if (!abilityScanChunkPtr) {
                    abilityScanChunkPtr = Read<uintptr_t>(
                        root + tableOffset + Offsets::EntityChunkStride * abilityScanChunk);
                }
                if (abilityScanChunkPtr) {
                    const uintptr_t identity = abilityScanChunkPtr +
                        Offsets::EntityStride * abilityScanSlot;
                    const uintptr_t entity = Read<uintptr_t>(identity);
                    if (entity) {
                        const uintptr_t vtable = Read<uintptr_t>(entity);
                        if (vtable) {
                            const auto cached = meleeVTables.find(vtable);
                            const bool isMelee = cached != meleeVTables.end()
                                ? cached->second
                                : GetEntityClassName(entity).find("Ability_HoldMelee") != std::string::npos;
                            meleeVTables.emplace(vtable, isMelee);
                            if (isMelee) {
                                const uint32_t ownerHandle = Read<uint32_t>(entity + 0x51C);
                                if (ownerHandle != 0xFFFFFFFFu)
                                    meleeAbilities.push_back({ entity, ownerHandle });
                            }
                        }
                    }
                }
                if (++abilityScanSlot > Offsets::HandleChunkMask) {
                    abilityScanSlot = 0;
                    abilityScanChunkPtr = 0;
                    if (++abilityScanChunk > (Offsets::MaxEntityIndex >> Offsets::HandleChunkShift)) {
                        abilityScanChunk = 0;
                        if (++abilityScanTable >= sizeof(tableOffsets) / sizeof(tableOffsets[0])) {
                            abilityScanTable = 0;
                            ++abilityScanRoot;
                        }
                    }
                }
            }
        }
        if (abilityScanRoot >= 1) {
        std::sort(meleeAbilities.begin(), meleeAbilities.end(),
                  [](const CachedMeleeAbility& a, const CachedMeleeAbility& b) {
                      return a.entity < b.entity;
                  });
        meleeAbilities.erase(std::unique(meleeAbilities.begin(), meleeAbilities.end(),
                                         [](const CachedMeleeAbility& a, const CachedMeleeAbility& b) {
                                             return a.entity == b.entity;
                                         }), meleeAbilities.end());
        lastAbilityRefresh = now;
        printf("[Parry] melee cache refreshed: %zu abilities\n", meleeAbilities.size());
            abilityScanActive = false;
        }
    }

    for (const uintptr_t pawn : pawns) {
        if (!pawn || pawn == currentLocalPawn) continue;
        const int health = Read<int>(pawn + Offsets::Health);
        if (health <= 0 || Read<uint8_t>(pawn + Offsets::LifeState) != 0) continue;

        Vector3 enemyPos{};
        if (!GetEntityPosition(pawn, enemyPos)) continue;
        const float dx = enemyPos.x - currentLocalPosition.x;
        const float dy = enemyPos.y - currentLocalPosition.y;
        const float dz = enemyPos.z - currentLocalPosition.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const Vector3 enemyVelocity = Read<Vector3>(pawn + Offsets::Velocity);
        const float enemySpeed = std::sqrt(
            enemyVelocity.x * enemyVelocity.x +
            enemyVelocity.y * enemyVelocity.y +
            enemyVelocity.z * enemyVelocity.z);
        const VelocitySample previousVelocity = velocitySamples[pawn];
        float speedAcceleration = 0.0f;
        if (previousVelocity.time && now > previousVelocity.time) {
            const float dt = static_cast<float>(now - previousVelocity.time) / 1000.0f;
            if (dt > 0.0f && dt <= 0.5f)
                speedAcceleration = (enemySpeed - previousVelocity.speed) / dt;
        }
        velocitySamples[pawn] = { now, enemySpeed };

        if (pawn == 0x29235979000ULL && now - lastPuppetVelocityLog >= 100) {
            lastPuppetVelocityLog = now;
            std::fprintf(stdout, "[PuppetSpeed] vx=%.1f vy=%.1f vz=%.1f speed=%.1f accel=%.1f distance=%.1f\n",
                         enemyVelocity.x, enemyVelocity.y, enemyVelocity.z,
                         enemySpeed, speedAcceleration, distance);
        }

        // Fallback heuristic for remote bots whose ability handles/state are
        // not exposed client-side. It only reacts to fast closing movement;
        // stationary attacks intentionally remain out of scope for this mode.
        const MotionSample previous = enemyMotion[pawn];
        MotionSample current = previous;
        float closingSpeed = previous.closingSpeed;
        if (!previous.time) {
            current.position = enemyPos;
            current.time = now;
            current.closingSpeed = 0.0f;
            closingSpeed = 0.0f;
        } else if (now > previous.time && now - previous.time >= 16) {
            const float dt = static_cast<float>(now - previous.time) / 1000.0f;
            if (dt <= 0.5f && distance > 0.01f) {
                const Vector3 velocity{
                    (enemyPos.x - previous.position.x) / dt,
                    (enemyPos.y - previous.position.y) / dt,
                    (enemyPos.z - previous.position.z) / dt
                };
                const float invDistance = 1.0f / distance;
                const Vector3 toLocal{
                    -dx * invDistance, -dy * invDistance, -dz * invDistance
                };
                closingSpeed = velocity.x * toLocal.x + velocity.y * toLocal.y + velocity.z * toLocal.z;
            }
            current.position = enemyPos;
            current.time = now;
            current.closingSpeed = closingSpeed;
        }
        const float closingAcceleration = current.closingSpeed - previous.closingSpeed;
        WriteSpeedMeasurement(pawn, Read<uint8_t>(pawn + Offsets::Team), distance, closingSpeed);
        current.armedUntil = previous.armedUntil;
        current.parryAt = previous.parryAt;
        const bool rangedMeleeSignal = distance > 400.0f &&
            enemySpeed >= 748.0f && enemySpeed <= 752.0f;
        const bool closeMeleeSignal = distance <= 400.0f &&
            enemySpeed >= 400.0f &&
            speedAcceleration >= 1700.0f && speedAcceleration <= 2000.0f;

        bool soundMeleeSignal = false;
        {
            uint32_t pawnEntityIndex = 0;
            const uintptr_t identity = Read<uintptr_t>(pawn + 0x10);
            if (identity)
                pawnEntityIndex = Read<uint32_t>(identity + Offsets::EntityHandleOffset) & Offsets::HandleIndexMask;

            std::lock_guard<std::mutex> lock(parrySoundMutex);
            const auto soundIt = meleeSoundUntil.find(pawnEntityIndex);
            if (soundIt != meleeSoundUntil.end()) {
                soundMeleeSignal = soundIt->second >= now;
                if (!soundMeleeSignal)
                    meleeSoundUntil.erase(soundIt);
            }
        }

        const bool soundOnlyRange = distance <= kSoundOnlyDistance;
        const bool movementMeleeSignal = rangedMeleeSignal || closeMeleeSignal;

        // At close range require the sound. Outside the 2 m zone, retain the
        // previous speed/acceleration detection and also accept the sound.
        const bool parrySignal = distance <= 900.0f &&
            (soundMeleeSignal || (!soundOnlyRange && movementMeleeSignal));
        if (parrySignal) {
            // Schedule once per signal. The delay is the estimated time to
            // reach the last-moment distance, instead of pressing instantly.
            if (previous.armedUntil < now || previous.parryAt == 0) {
                if (soundOnlyRange && soundMeleeSignal) {
                    // At 0-2 m use only the sound, with a short fixed delay.
                    current.parryAt = now + static_cast<ULONGLONG>(kCloseParryDelayMs);
                } else {
                    // Use only the component directed toward the local player.
                    // Total enemy speed is unsuitable here because strafing can
                    // make the delay much too short or much too long.
                    float delayMs = kDefaultParryDelayMs;
                    if (current.closingSpeed > kMinApproachSpeed) {
                        const float distanceToLastMoment =
                            (std::max)(0.0f, distance - kLastMomentDistance);
                        delayMs = distanceToLastMoment / current.closingSpeed * 1000.0f;
                    }
                    const ULONGLONG scheduledDelayMs = static_cast<ULONGLONG>(
                        std::clamp(delayMs, 400.0f, 700.0f));
                    current.parryAt = now + scheduledDelayMs;
                }
            }
            current.armedUntil = now + 500;
        }
        enemyMotion[pawn] = current;

        if (heuristicParryEnabled && isEnemy(pawn) && distance <= 400.0f &&
            current.armedUntil >= now && current.parryAt <= now &&
            now - lastHeuristicParry >= 250) {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 'F';
            SendInput(1, &input, sizeof(input));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(input));
            lastHeuristicParry = now;
            printf("[ParryHeuristic] parry sent distance=%.1f closing=%.1f pawn=0x%p\n",
                   distance, closingSpeed, reinterpret_cast<void*>(pawn));

            // Consume the sound event so the same hold sound cannot trigger
            // a second parry while its 600 ms signal window is still active.
            const uintptr_t identity = Read<uintptr_t>(pawn + 0x10);
            const uint32_t pawnEntityIndex = identity
                ? Read<uint32_t>(identity + Offsets::EntityHandleOffset) & Offsets::HandleIndexMask
                : 0;
            if (pawnEntityIndex) {
                std::lock_guard<std::mutex> lock(parrySoundMutex);
                meleeSoundUntil.erase(pawnEntityIndex);
            }

            current.armedUntil = 0;
            current.parryAt = 0;
            enemyMotion[pawn] = current;
            return;
        }
        if (!disableStaleAbilityScan && isEnemy(pawn) && distance <= 180.0f &&
            now - lastPawnDiagnostic >= 1000) {
            const uintptr_t component = pawn + Offsets::AbilityComponent;
            // CNetworkUtlVectorBase layout in this client is {count, elements, capacity}.
            const int count = std::clamp(Read<int>(component + Offsets::AbilityVector), 0, 64);
            const uintptr_t handles = Read<uintptr_t>(component + Offsets::AbilityVector + 8);
            const uint64_t raw0 = Read<uint64_t>(component + Offsets::AbilityVector);
            const uint64_t raw8 = Read<uint64_t>(component + Offsets::AbilityVector + 8);
            const uint64_t raw16 = Read<uint64_t>(component + Offsets::AbilityVector + 16);
            printf("[Parry] pawn=0x%p team=%u dist=%.1f component=0x%p vec=[%llX %llX %llX] handles=0x%p count=%d\n",
                   reinterpret_cast<void*>(pawn), Read<uint8_t>(pawn + Offsets::Team), distance,
                   reinterpret_cast<void*>(component),
                   static_cast<unsigned long long>(raw0), static_cast<unsigned long long>(raw8),
                   static_cast<unsigned long long>(raw16), reinterpret_cast<void*>(handles), count);
            const uint32_t selectedHandle = Read<uint32_t>(component + 0xC8);
            const uint32_t channellingHandle = Read<uint32_t>(component + 0xCC);
            const uintptr_t selectedEntity = ResolveEntity(selectedHandle);
            const uintptr_t channellingEntity = ResolveEntity(channellingHandle);
            printf("[Parry] selected handle=0x%X entity=0x%p class=%s; channelling handle=0x%X entity=0x%p class=%s\n",
                   selectedHandle, reinterpret_cast<void*>(selectedEntity), GetEntityClassName(selectedEntity).c_str(),
                   channellingHandle, reinterpret_cast<void*>(channellingEntity), GetEntityClassName(channellingEntity).c_str());
            for (int i = 0; i < count; ++i) {
                const uint32_t handle = Read<uint32_t>(handles + sizeof(uint32_t) * i);
                const uintptr_t entity = ResolveEntity(handle);
                static bool dumpedFailedHandle = false;
                if (!entity && !dumpedFailedHandle) {
                    DebugEntityHandle(handle);
                    dumpedFailedHandle = true;
                }
                printf("[Parry] enemy ability[%d] handle=0x%X entity=0x%p class=%s\n",
                       i, handle, reinterpret_cast<void*>(entity),
                       GetEntityClassName(entity).c_str());
            }
            lastPawnDiagnostic = now;
        }
        if (!isEnemy(pawn)) continue;

        const uintptr_t identity = Read<uintptr_t>(pawn + 0x10);
        const uint32_t pawnHandle = identity ? Read<uint32_t>(identity + Offsets::EntityHandleOffset) : 0xFFFFFFFFu;
        if (distance <= 180.0f && now - lastDiagnostic < 30) {
            printf("[Parry] enemy pawn=0x%p handle=0x%X distance=%.1f\n",
                   reinterpret_cast<void*>(pawn), pawnHandle, distance);
        }
        if (pawnHandle == 0xFFFFFFFFu || distance > 180.0f) continue;
        if (!legacyStateParryEnabled) continue;
        for (const CachedMeleeAbility& cached : meleeAbilities) {
            if ((cached.ownerHandle & Offsets::HandleIndexMask) !=
                (pawnHandle & Offsets::HandleIndexMask)) continue;
            const int state = Read<int>(cached.entity + Offsets::MeleeAttackState);
            printf("[Parry] melee ability=0x%p owner=0x%X state=%d dist=%.1f\n",
                   reinterpret_cast<void*>(cached.entity), cached.ownerHandle, state, distance);
            if (state < 1 || state > 3 || now - lastParry < 180) continue;

            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = 'F';
            SendInput(1, &input, sizeof(input));
            input.ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(1, &input, sizeof(input));
            lastParry = now;
            printf("[Parry] parry sent state=%d distance=%.1f\n", state, distance);
            return;
        }
    }
}
