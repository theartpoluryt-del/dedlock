#include "hero_scripts.h"

#include "portable_paths.h"
#include "usercmd.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <unordered_map>

bool vindictaAutoSnipeEnabled = false;
bool hazeSleepDaggerEnabled = false;
bool shivSerratedKnivesEnabled = false;
bool bebopAbility3Enabled = false;
bool bebopAbility2AutoEnabled = false;
bool drifterAbility2Enabled = false;
bool heroScriptsShowFov = false;
bool hazePredictionDot = true;
float vindictaSnipeFov = 60.0f;
float vindictaSnipeSmoothX = 5.0f;
float vindictaSnipeSmoothY = 5.0f;
float hazeDaggerFov = 120.0f;
float hazeDaggerSmoothX = 4.0f;
float hazeDaggerSmoothY = 4.0f;
float shivKnivesFov = 120.0f;
float shivKnivesSmoothX = 4.0f;
float shivKnivesSmoothY = 4.0f;
float bebopAbility3Fov = 120.0f;
float bebopAbility3SmoothX = 4.0f;
float bebopAbility3SmoothY = 4.0f;
float drifterAbility2Fov = 120.0f;
float drifterAbility2SmoothX = 4.0f;
float drifterAbility2SmoothY = 4.0f;

namespace {

constexpr uint32_t kVindictaId = 3;
constexpr uint32_t kHazeId = 13;
constexpr uint32_t kShivId = 19;
constexpr uint32_t kBebopId = 15;
constexpr uint32_t kDrifterId = 64;
constexpr uint64_t kAbility1Mask = 0x0000000200000000ull;
constexpr uint64_t kAbility2Mask = 0x0000000400000000ull;
constexpr uint64_t kAbility3Mask = 0x0000000800000000ull;
constexpr uint64_t kAbility4Mask = 0x0000001000000000ull;
constexpr uint64_t kAttackMask = 0x0000000000000001ull;
constexpr float kRadiansToDegrees = 57.29577951308232f;
constexpr float kGravityUnits = 800.0f;
// The server applies late modifier/shield updates that can make a client-side
// preview slightly optimistic. Keep Auto Snipe's lethal decision conservative.
constexpr float kVindictaKillSafetyPercent = 0.10f;
constexpr float kVindictaKillSafetyFlat = 10.0f;
constexpr uintptr_t kSubclassVDataOffset = 0x390;
// CCitadelWeaponInfo is embedded in CitadelAbilityVData.  These three fields
// are at stable positions inside that embedded value.  Schema may be queried
// before this non-networked VData type is published, which previously left
// all three offsets at zero and silently disabled projectile lead.
constexpr uintptr_t kWeaponBulletSpeedFallback = 0xB4;
constexpr uintptr_t kWeaponBulletLifetimeFallback = 0xC8;
constexpr uintptr_t kWeaponRangeFallback = 0x48;
constexpr uintptr_t kWeaponBulletGravityFallback = 0xBC;
constexpr uintptr_t kWeaponBulletInheritFallback = 0xD0;
// Current abilities.vdata stores Sleep Dagger's trajectory in m_WeaponInfo:
// speed 3500, gravity scale 0.2, custom launch attachment "palm_R".
// Keep those exact values as fallbacks for the short startup window in which
// SchemaSystem or the ability VData pointer is not available yet.
constexpr float kHazeDaggerProjectileSpeedFallback = 3500.0f;
constexpr float kHazeDaggerGravityScaleFallback = 0.2f;
constexpr float kHazeDaggerCastDelayFallback = 0.2f;
constexpr float kHeroProjectileOriginHeight = 52.0f;
// C_CitadelPlayerPawn::m_angClientCamera, networked client-camera angles.
// This is the same field updated by the reference post-move implementation.
constexpr uintptr_t kClientCameraAnglesOffset = 0x1248;
constexpr bool kHeroScriptDiagnostics = false;

struct HeroOffsets {
    uintptr_t remainingCharges{};
    uintptr_t upgradeBits{};
    uintptr_t scopeStartTime{};
    uintptr_t weaponInfo{};
    uintptr_t abilityProperties{};
    uintptr_t bulletSpeed{};
    uintptr_t bulletLifetime{};
    uintptr_t weaponRange{};
    uintptr_t bulletGravity{};
    uintptr_t bulletInherit{};
    bool initialized{};
};

struct TargetSnapshot {
    uintptr_t entity{};
    Vector3 point{};
    int boneIndex{-1};
    Vector3 rawAngles{};
    Vector3 angularVelocity{};
    int health{};
    int maxHealth{};
    uint32_t heroId{};
    ULONGLONG updatedAt{};
    bool inAbilityRange{true};
};

struct TargetMotionSample {
    Vector3 point{};
    Vector3 velocity{};
    ULONGLONG at{};
};

enum class ScriptState { Idle, Aiming, Firing, PostFire, Scoping };

struct CommandState {
    ScriptState state = ScriptState::Idle;
    uintptr_t lockedEntity{};
    bool wasAbility1Held{};
    float nextActionTime{};
    ULONGLONG scopeStartedAt{};
    uintptr_t lastCommand{};
    uint64_t clearMask{};
    uint64_t holdMask{};
    uint64_t tapMask{};
    bool writeAngles{};
    Vector3 finalAngles{};
    Vector3 lastAimAngles{};
    bool hasLastAimAngles{};
    TargetSnapshot lockedTarget{};
};

HeroOffsets offsets{};
std::mutex targetMutex;
TargetSnapshot targetSnapshot{};
TargetSnapshot bebopAbility2TargetSnapshot{};
CommandState commandState{};
std::unordered_map<uintptr_t, TargetMotionSample> targetMotionSamples{};

using ComputePropertyValueFn = float(__fastcall*)(const void*, int, int);
using ComputePropertyScaledFn = bool(__fastcall*)(int, int, const void*,
    const char*, float, char, char, float*, float*);
using QueryEntityStatFn = double(__fastcall*)(int, void*, int);

ComputePropertyValueFn computePropertyValue{};
ComputePropertyScaledFn computePropertyScaled{};
QueryEntityStatFn queryEntityStat{};
int32_t* abilityServiceContext{};
std::mutex diagnosticsMutex;
bool ScriptEnabled(uint32_t heroId);
float EntityStat(int stat, uintptr_t entity);

void LogDiagnostics(const char* stage, uint32_t heroId, uintptr_t ability,
                    uintptr_t target, int detail = 0) {
    if constexpr (!kHeroScriptDiagnostics) return;
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    std::ofstream log(Dll6Paths::DataFileA("hero_scripts.log"), std::ios::app);
    if (!log) return;
    log << GetTickCount64() << ' ' << (stage ? stage : "unknown")
        << " hero=" << heroId
        << " enabled=" << ScriptEnabled(heroId)
        << " pawn=0x" << std::hex << currentLocalPawn
        << " ability=0x" << ability
        << " target=0x" << target << std::dec
        << " detail=" << detail << '\n';
}

float NormalizeAngle(float angle) {
    while (angle > 180.0f) angle -= 360.0f;
    while (angle < -180.0f) angle += 360.0f;
    return angle;
}

Vector3 CalculateAngles(const Vector3& source, const Vector3& destination) {
    const float dx = destination.x - source.x;
    const float dy = destination.y - source.y;
    const float dz = destination.z - source.z;
    const float horizontal = std::sqrt(dx * dx + dy * dy);
    Vector3 result{};
    result.x = -std::atan2(dz, horizontal) * kRadiansToDegrees;
    result.y = std::atan2(dy, dx) * kRadiansToDegrees;
    result.x = std::clamp(result.x, -89.0f, 89.0f);
    result.y = NormalizeAngle(result.y);
    return result;
}

bool ResolveHeroAimSource(uint32_t heroId, Vector3& source) {
    if (currentCameraPositionReady) {
        source = currentCameraPosition;
        return true;
    }
    if (!currentLocalPositionReady) return false;
    source = currentLocalPosition;
    source.z += heroId == kHazeId ? kHeroProjectileOriginHeight : 64.0f;
    return std::isfinite(source.x) && std::isfinite(source.y) &&
        std::isfinite(source.z);
}

float AngularError(const Vector3& left, const Vector3& right) {
    const float pitch = NormalizeAngle(left.x - right.x);
    const float yaw = NormalizeAngle(left.y - right.y);
    return std::sqrt(pitch * pitch + yaw * yaw);
}

void InitializeOffsets() {
    if (offsets.initialized) {
        // Hero scripts can start before SchemaSystem finishes publishing all
        // VData fields. Retry only the missing projectile fields; otherwise
        // Haze's dagger keeps a permanent zero speed and never gets a lead.
        static ULONGLONG lastProjectileRetry = 0;
        const ULONGLONG now = GetTickCount64();
        if ((!offsets.bulletSpeed || !offsets.bulletLifetime ||
             !offsets.weaponRange ||
             !offsets.bulletGravity ||
             !offsets.bulletInherit) && now - lastProjectileRetry >= 1000) {
            lastProjectileRetry = now;
            if (!offsets.bulletSpeed)
                offsets.bulletSpeed = ResolveRuntimeSchemaOffset(
                    "CCitadelWeaponInfo", "m_flBulletSpeed");
            if (!offsets.bulletLifetime)
                offsets.bulletLifetime = ResolveRuntimeSchemaOffset(
                    "CCitadelWeaponInfo", "m_flBulletLifetime");
            if (!offsets.weaponRange)
                offsets.weaponRange = ResolveRuntimeSchemaOffset(
                    "CCitadelWeaponInfo", "m_flRange");
            if (!offsets.bulletGravity)
                offsets.bulletGravity = ResolveRuntimeSchemaOffset(
                    "CCitadelWeaponInfo", "m_flBulletGravityScale");
            if (!offsets.bulletInherit)
                offsets.bulletInherit = ResolveRuntimeSchemaOffset(
                    "CCitadelWeaponInfo", "m_flBulletInheritShooterVelocityScale");
        }
        return;
    }
    offsets.initialized = true;
    offsets.remainingCharges = ResolveRuntimeSchemaOffset(
        "C_CitadelBaseAbility", "m_iRemainingCharges");
    offsets.upgradeBits = ResolveRuntimeSchemaOffset(
        "C_CitadelBaseAbility", "m_nUpgradeBits");
    offsets.scopeStartTime = ResolveRuntimeSchemaOffset(
        "CCitadel_Ability_Hornet_Snipe", "m_flScopeStartTime");
    offsets.weaponInfo = ResolveRuntimeSchemaOffset(
        "CitadelAbilityVData", "m_WeaponInfo");
    offsets.abilityProperties = ResolveRuntimeSchemaOffset(
        "CitadelAbilityVData", "m_mapAbilityProperties");
    offsets.bulletSpeed = ResolveRuntimeSchemaOffset(
        "CCitadelWeaponInfo", "m_flBulletSpeed");
    offsets.bulletLifetime = ResolveRuntimeSchemaOffset(
        "CCitadelWeaponInfo", "m_flBulletLifetime");
    offsets.weaponRange = ResolveRuntimeSchemaOffset(
        "CCitadelWeaponInfo", "m_flRange");
    offsets.bulletGravity = ResolveRuntimeSchemaOffset(
        "CCitadelWeaponInfo", "m_flBulletGravityScale");
    offsets.bulletInherit = ResolveRuntimeSchemaOffset(
        "CCitadelWeaponInfo", "m_flBulletInheritShooterVelocityScale");
    // These offsets are also present in the current schema dump bundled with
    // the project reference. Keep the feature operational if SchemaSystem is
    // temporarily unavailable during the first injected frame.
    if (!offsets.remainingCharges) offsets.remainingCharges = 0x780;
    if (!offsets.upgradeBits) offsets.upgradeBits = 0x758;
    if (!offsets.scopeStartTime) offsets.scopeStartTime = 0x1874;
    if (!offsets.weaponInfo) offsets.weaponInfo = 0x158;
    if (!offsets.abilityProperties) offsets.abilityProperties = 0xE68;
    if (!offsets.bulletSpeed) offsets.bulletSpeed = kWeaponBulletSpeedFallback;
    if (!offsets.bulletLifetime)
        offsets.bulletLifetime = kWeaponBulletLifetimeFallback;
    if (!offsets.weaponRange) offsets.weaponRange = kWeaponRangeFallback;
    if (!offsets.bulletGravity) offsets.bulletGravity = kWeaponBulletGravityFallback;
    if (!offsets.bulletInherit) offsets.bulletInherit = kWeaponBulletInheritFallback;

    computePropertyValue = reinterpret_cast<ComputePropertyValueFn>(
        FindUniqueClientPattern(
            "48 63 81 ? ? 00 00 4D 63 C8 4C 8B 81 ? ? 00 00 "
            "F3 42 0F 10 4C 89 ? 4D 8D 14 C0 4D 3B C2 74 ? "
            "F3 0F 10 15 ? ? ? ? F3 0F 10 1D ? ? ? ? 49 8B 08 "
            "48 8B 41 ? 85 50 ? 74 ? 8B 41 ? 83 F8 01 75 ? "
            "F3 42 0F 10 44 89 ? F3 0F 5E C2 F3 0F 58 C3 "
            "F3 0F 59 C8 EB ? 85 C0 75 ? F3 42 0F 58 4C 89 ? "
            "49 83 C0 08 4D 3B C2 75 ? 0F 28 C1 C3"));
    computePropertyScaled = reinterpret_cast<ComputePropertyScaledFn>(
        FindUniqueClientPattern(
            "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 "
            "41 54 41 55 41 56 41 57 48 81 EC A0 05 00 00"));
    queryEntityStat = reinterpret_cast<QueryEntityStatFn>(
        FindUniqueClientPattern(
            "48 89 5C 24 08 48 89 74 24 18 57 48 81 EC A0 00 "
            "00 00 41 8B D8 8B F1 48 85 D2"));

    const uintptr_t contextPattern = FindUniqueClientPattern(
        "F2 0F 10 05 ? ? ? ? 48 8D 0D ? ? ? ? 0F 10 0D ? ? ? ? "
        "8B 05 ? ? ? ? F2 0F 11 05 ? ? ? ? 0F 10 05 ? ? ? ? "
        "89 05 ? ? ? ? 0F 11 0D ? ? ? ? 0F 11 05 ? ? ? ? "
        "F2 0F 10 05 ? ? ? ? F2 0F 11 05 ? ? ? ? E8 ? ? ? ? "
        "48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8D 05 ? ? ? ? 48 83 C4 28 C3");
    if (contextPattern) {
        const uintptr_t lea = contextPattern + 0x60;
        const int32_t displacement = Read<int32_t>(lea + 3);
        abilityServiceContext = reinterpret_cast<int32_t*>(
            lea + 7 + static_cast<intptr_t>(displacement));
    }
    {
        std::lock_guard<std::mutex> lock(diagnosticsMutex);
        std::ofstream log(Dll6Paths::DataFileA("hero_scripts.log"),
                          std::ios::trunc);
        if (log) {
            log << "offsets charges=0x" << std::hex << offsets.remainingCharges
                << " upgrades=0x" << offsets.upgradeBits
                << " scope=0x" << offsets.scopeStartTime
                << " weapon=0x" << offsets.weaponInfo
                << " properties=0x" << offsets.abilityProperties
                << " speed=0x" << offsets.bulletSpeed
                << " lifetime=0x" << offsets.bulletLifetime
                << " range=0x" << offsets.weaponRange
                << " gravity=0x" << offsets.bulletGravity
                << " inherit=0x" << offsets.bulletInherit
                << " compute=" << reinterpret_cast<uintptr_t>(computePropertyValue)
                << " scaled=" << reinterpret_cast<uintptr_t>(computePropertyScaled)
                << " stat=" << reinterpret_cast<uintptr_t>(queryEntityStat)
                << " context=" << reinterpret_cast<uintptr_t>(abilityServiceContext)
                << std::dec << '\n';
        }
    }
}

uint32_t CurrentHeroId() {
    if (!currentLocalPawn) return 0;
    return Read<uint32_t>(currentLocalPawn + Offsets::HeroComponent +
                          Offsets::HeroSpawnedId);
}

bool ScriptEnabled(uint32_t heroId) {
    if (heroId == kVindictaId) return vindictaAutoSnipeEnabled;
    if (heroId == kHazeId) return hazeSleepDaggerEnabled;
    if (heroId == kShivId) return shivSerratedKnivesEnabled;
    if (heroId == kBebopId)
        return bebopAbility3Enabled || bebopAbility2AutoEnabled;
    if (heroId == kDrifterId) return drifterAbility2Enabled;
    return false;
}

float ScriptFov(uint32_t heroId) {
    if (heroId == kVindictaId) return vindictaSnipeFov;
    if (heroId == kHazeId) return hazeDaggerFov;
    if (heroId == kShivId) return shivKnivesFov;
    if (heroId == kBebopId) return bebopAbility3Fov;
    return drifterAbility2Fov;
}

int ScriptAbilitySlot(uint32_t heroId) {
    if (heroId == kVindictaId) return 3;
    if (heroId == kBebopId) return 2;
    if (heroId == kDrifterId) return 1;
    return 0;
}

uint64_t ScriptAbilityMask(uint32_t heroId) {
    if (heroId == kVindictaId) return kAbility4Mask;
    if (heroId == kBebopId) return kAbility3Mask;
    if (heroId == kDrifterId) return kAbility2Mask;
    return kAbility1Mask;
}

uintptr_t FindAbility(int wantedSlot) {
    if (!currentLocalPawn) return 0;
    const uintptr_t component = currentLocalPawn + Offsets::AbilityComponent;
    const int count = std::clamp(
        Read<int>(component + Offsets::AbilityVector), 0, 64);
    const uintptr_t handles = Read<uintptr_t>(
        component + Offsets::AbilityVector + sizeof(uintptr_t));
    for (int index = 0; handles && index < count; ++index) {
        const uintptr_t ability = ResolveEntity(
            Read<uint32_t>(handles + index * sizeof(uint32_t)));
        if (ability && Read<int16_t>(ability + Offsets::AbilitySlot) == wantedSlot)
            return ability;
    }
    return 0;
}

uintptr_t FindPrimaryWeapon() {
    // Primary weapons normally live in the ability vector at
    // ESlot_Weapon_Primary (0x15). Keep the active-weapon handle as a
    // fallback for heroes/client states where that vector is incomplete.
    uintptr_t weapon = FindAbility(0x15);
    static uintptr_t weaponServicesOffset = 0;
    static uintptr_t activeWeaponOffset = 0;
    static ULONGLONG lastOffsetRetry = 0;
    const ULONGLONG now = GetTickCount64();
    if ((!weaponServicesOffset || !activeWeaponOffset) &&
        now - lastOffsetRetry >= 1000) {
        lastOffsetRetry = now;
        if (!weaponServicesOffset) {
            weaponServicesOffset = ResolveRuntimeSchemaOffset(
                "C_BasePlayerPawn", "m_pWeaponServices");
            if (!weaponServicesOffset) weaponServicesOffset = 0xEE8;
        }
        if (!activeWeaponOffset) {
            activeWeaponOffset = ResolveRuntimeSchemaOffset(
                "CPlayer_WeaponServices", "m_hActiveWeapon");
            if (!activeWeaponOffset) activeWeaponOffset = 0x60;
        }
    }
    const uintptr_t services = weaponServicesOffset
        ? Read<uintptr_t>(currentLocalPawn + weaponServicesOffset) : 0;
    const uint32_t weaponHandle = services && activeWeaponOffset
        ? Read<uint32_t>(services + activeWeaponOffset) : 0xFFFFFFFFu;
    if (!weapon && weaponHandle != 0xFFFFFFFFu)
        weapon = ResolveEntity(weaponHandle);
    return weapon;
}

bool AbilityReady(uintptr_t ability, bool requireCharge) {
    if (!ability) return false;
    const float now = GetClientGameTime();
    const float cooldownEnd = Read<float>(ability + Offsets::AbilityCooldownEnd);
    if (std::isfinite(now) && std::isfinite(cooldownEnd) && now < cooldownEnd)
        return false;
    if (requireCharge && offsets.remainingCharges &&
        Read<int>(ability + offsets.remainingCharges) == 0)
        return false;
    return true;
}

const void* FindAbilityProperty(uintptr_t ability, const char* name) {
    if (!ability || !name || !offsets.abilityProperties) return nullptr;
    const uintptr_t vdata = Read<uintptr_t>(ability + kSubclassVDataOffset);
    if (!vdata) return nullptr;
    const uintptr_t map = vdata + offsets.abilityProperties;
    const int count = Read<int>(map + 0x0C) & 0x7FFFFFFF;
    const uintptr_t elements = Read<uintptr_t>(map + 0x10);
    if (!elements || count <= 0 || count > 256) return nullptr;
    __try {
        for (int index = 0; index < count; ++index) {
            const uintptr_t node = elements + static_cast<uintptr_t>(index) * 224;
            const char* key = Read<const char*>(node + 0x10);
            if (key && std::strcmp(key, name) == 0)
                return reinterpret_cast<const void*>(node + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

float AbilityProperty(uintptr_t ability, const char* name, float fallback) {
    const void* property = FindAbilityProperty(ability, name);
    if (!property) return fallback;
    // Current property entries retain their selected base value at +0x20.
    // Use it when the private ComputeValue routine changed after a client
    // update, rather than silently substituting values from an old patch.
    const float liveBase = Read<float>(reinterpret_cast<uintptr_t>(property) + 0x20);
    const bool liveBaseValid = std::isfinite(liveBase) &&
        liveBase >= -10000.0f && liveBase <= 100000.0f;
    if (!computePropertyValue)
        return liveBaseValid ? liveBase : fallback;
    const int upgrades = offsets.upgradeBits
        ? Read<int>(ability + offsets.upgradeBits) : 0;
    float value = std::numeric_limits<float>::quiet_NaN();
    __try {
        value = computePropertyValue(property, upgrades, 0);
        if (computePropertyScaled) {
            int handle = static_cast<int>(currentLocalPawnHandle);
            if (!handle || handle == -1) {
                const uint32_t resolved = FindEntityHandle(currentLocalPawn);
                if (resolved && resolved != 0xFFFFFFFFu) handle = static_cast<int>(resolved);
            }
            if (abilityServiceContext && handle != -1) {
                abilityServiceContext[2] = handle;
                abilityServiceContext[3] = handle;
            }
            float scaled = value;
            float delta = 0.0f;
            if (handle != -1 && computePropertyScaled(
                    handle, upgrades, property, nullptr, value, 0, 0,
                    &scaled, &delta) && std::isfinite(scaled)) {
                value = scaled;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        value = std::numeric_limits<float>::quiet_NaN();
    }
    return std::isfinite(value) ? value : (liveBaseValid ? liveBase : fallback);
}

float BebopHookRange(uintptr_t ability) {
    // The exact property label changed between client builds. Prefer the
    // live VData entry, which includes ability upgrades and range modifiers,
    // and keep 30 m (1200 Source units) as the base Hook range fallback.
    static constexpr const char* kRangeProperties[]{
        "CastRange", "AbilityCastRange", "HookRange", "Range"};
    for (const char* name : kRangeProperties) {
        if (FindAbilityProperty(ability, name)) {
            const float value = AbilityProperty(ability, name, 0.0f);
            if (std::isfinite(value) && value > 1.0f)
                return value;
        }
    }
    return 1200.0f;
}

float ScriptProjectileRange(uint32_t heroId, uintptr_t ability) {
    if (heroId == kVindictaId) return std::numeric_limits<float>::infinity();

    // Direct travel/cast ranges take priority and include upgrades when the
    // live property evaluator is available. Ignore small radius properties:
    // those describe impact AoE, not projectile travel.
    static constexpr const char* kDirectRangeProperties[]{
        "ProjectileRange", "ProjectileDistance", "MaxProjectileRange",
        "MaxRange", "CastRange", "AbilityCastRange", "HookRange", "Range"};
    for (const char* name : kDirectRangeProperties) {
        if (!FindAbilityProperty(ability, name)) continue;
        const float value = AbilityProperty(ability, name, 0.0f);
        if (std::isfinite(value) && value >= 400.0f && value <= 50000.0f)
            return value;
    }

    // Primary weapons store their physical limit directly in the embedded
    // CCitadelWeaponInfo rather than in m_mapAbilityProperties.
    const uintptr_t vdata = ability
        ? Read<uintptr_t>(ability + kSubclassVDataOffset) : 0;
    const uintptr_t weaponInfo = vdata && offsets.weaponInfo
        ? vdata + offsets.weaponInfo : 0;
    float weaponLifetime = weaponInfo && offsets.bulletLifetime
        ? Read<float>(weaponInfo + offsets.bulletLifetime) : 0.0f;
    float weaponSpeed = weaponInfo && offsets.bulletSpeed
        ? Read<float>(weaponInfo + offsets.bulletSpeed) : 0.0f;
    if (heroId == 0) {
        const float liveSpeed = EntityStat(0x4C, currentLocalPawn);
        if (std::isfinite(liveSpeed) && liveSpeed > 100.0f &&
            liveSpeed <= 100000.0f)
            weaponSpeed = liveSpeed;
    }
    float physicalRange = std::numeric_limits<float>::infinity();
    if (std::isfinite(weaponLifetime) && weaponLifetime >= 0.01f &&
        weaponLifetime <= 20.0f && std::isfinite(weaponSpeed) &&
        weaponSpeed > 100.0f && weaponSpeed <= 100000.0f)
        physicalRange = weaponSpeed * weaponLifetime;

    if (heroId == 0) {
        // CCitadelWeaponInfo::m_flRange is the actual maximum attack travel
        // distance. Falloff ranges only change damage and bullet lifetime is
        // commonly much longer than this gameplay limit.
        const float baseRange = weaponInfo && offsets.weaponRange
            ? Read<float>(weaponInfo + offsets.weaponRange) : 0.0f;
        const float liveRange = EntityStat(0x12, currentLocalPawn);
        if (std::isfinite(liveRange) && liveRange >= 100.0f &&
            liveRange <= 100000.0f && std::isfinite(baseRange) &&
            baseRange >= 100.0f && liveRange >= baseRange * 0.25f)
            return std::isfinite(physicalRange)
                ? (std::min)(liveRange, physicalRange) : liveRange;
        if (std::isfinite(baseRange) && baseRange >= 100.0f &&
            baseRange <= 100000.0f)
            return std::isfinite(physicalRange)
                ? (std::min)(baseRange, physicalRange) : baseRange;
        if (std::isfinite(physicalRange)) return physicalRange;
    } else if (std::isfinite(physicalRange)) {
        return physicalRange;
    }

    // Some projectile abilities publish lifetime as an ability property.
    // Their physical travel limit is also speed * lifetime.
    static constexpr const char* kLifetimeProperties[]{
        "ProjectileLifetime", "ProjectileDuration", "ProjectileLifeTime"};
    float lifetime = 0.0f;
    for (const char* name : kLifetimeProperties) {
        if (!FindAbilityProperty(ability, name)) continue;
        const float value = AbilityProperty(ability, name, 0.0f);
        if (std::isfinite(value) && value >= 0.1f && value <= 20.0f) {
            lifetime = value;
            break;
        }
    }
    if (lifetime > 0.0f && std::isfinite(weaponSpeed) &&
        weaponSpeed > 100.0f && weaponSpeed <= 100000.0f) {
        return weaponSpeed * lifetime;
    }

    // Hook's current base range is known even before VData is available.
    if (heroId == kBebopId) return BebopHookRange(ability);
    return std::numeric_limits<float>::infinity();
}

bool TargetInProjectileRange(float range, uintptr_t target) {
    if (!std::isfinite(range)) return true;
    if (!target || !currentLocalPositionReady) return false;
    const uintptr_t sceneNode = Read<uintptr_t>(target + Offsets::GameSceneNode);
    Vector3 targetPosition = sceneNode
        ? Read<Vector3>(sceneNode + Offsets::SceneNodeAbsOrigin) : Vector3{};
    if (!std::isfinite(targetPosition.x) || !std::isfinite(targetPosition.y) ||
        !std::isfinite(targetPosition.z))
        targetPosition = Read<Vector3>(target + Offsets::Pos);
    if (!std::isfinite(targetPosition.x) || !std::isfinite(targetPosition.y) ||
        !std::isfinite(targetPosition.z)) return false;
    const float dx = targetPosition.x - currentLocalPosition.x;
    const float dy = targetPosition.y - currentLocalPosition.y;
    const float dz = targetPosition.z - currentLocalPosition.z;
    // Preserve the hook's collision leeway at the edge of its range rather
    // than rejecting a valid torso hit because the pawn origin is centered.
    const float collisionLeeway = range + 40.0f;
    return dx * dx + dy * dy + dz * dz <=
        collisionLeeway * collisionLeeway;
}

bool ScriptTargetInAbilityRange(uint32_t heroId, uintptr_t ability,
                                uintptr_t target) {
    return TargetInProjectileRange(
        ScriptProjectileRange(heroId, ability), target);
}

float EntityStat(int stat, uintptr_t entity) {
    if (!queryEntityStat || !entity) return 0.0f;
    __try {
        const double result = queryEntityStat(stat, reinterpret_cast<void*>(entity), 0);
        const float value = *reinterpret_cast<const float*>(&result);
        return std::isfinite(value) ? value : 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
}

float SpiritResistance(uintptr_t entity) {
    return std::clamp(EntityStat(14, entity), -100.0f, 100.0f);
}

float CasterSpiritDamageMultiplier() {
    // The damage pipeline applies the global damage scale and the Spirit-only
    // scale separately.  They contain live item/modifier amps (for example,
    // temporary Spirit amplification), which are not represented in VData.
    float multiplier = 1.0f;
    const float global = EntityStat(57, currentLocalPawn); // EDamageScale
    const float tech = EntityStat(61, currentLocalPawn);   // ETechDamageScale
    if (std::isfinite(global) && global >= 0.01f && global <= 10.0f)
        multiplier *= global;
    if (std::isfinite(tech) && tech >= 0.01f && tech <= 10.0f)
        multiplier *= tech;
    // These two values remain populated by the stat service for item and
    // modifier-based Spirit amplification even though their enum labels are
    // marked deprecated. They are additive percentages, unlike *Scale.
    const float techAmp = EntityStat(24, currentLocalPawn);
    const float techAmpBonus = EntityStat(25, currentLocalPawn);
    const float additiveAmp = techAmp + techAmpBonus;
    if (std::isfinite(additiveAmp) && additiveAmp > -0.99f &&
        additiveAmp < 10.0f)
        multiplier *= 1.0f + additiveAmp;
    return multiplier;
}

float AbilityPropertyLiveScale(const void* property) {
    if (!property || computePropertyScaled) return 0.0f;
    // CitadelAbilityProperty_t embeds a CScaleFunctionVData pointer at +0x30.
    // The VData itself names the stat at +0x28 and stores its coefficient at
    // +0x30.  This is the exact data the client uses for properties such as
    // Hornet Snipe's Damage (currently ETechPower * 0.93) and its low-HP
    // bonus (ETechPower * 2.30), rather than a hard-coded patch value.
    const uintptr_t scaleVData = Read<uintptr_t>(
        reinterpret_cast<uintptr_t>(property) + 0x30);
    if (!scaleVData) return 0.0f;
    const int stat = Read<int>(scaleVData + 0x28);
    const float coefficient = Read<float>(scaleVData + 0x30);
    if (stat < 0 || stat >= 97 || !std::isfinite(coefficient) ||
        coefficient < -100.0f || coefficient > 100.0f)
        return 0.0f;
    const float scaled = EntityStat(stat, currentLocalPawn) * coefficient;
    return std::isfinite(scaled) ? scaled : 0.0f;
}

float VindictaDamageProperty(uintptr_t ability, const char* name,
                             float fallback) {
    const void* property = FindAbilityProperty(ability, name);
    return AbilityProperty(ability, name, fallback) +
        AbilityPropertyLiveScale(property);
}

bool VindictaLowHealthTarget(uintptr_t targetEntity, int health,
                             int snapshotMaxHealth, float threshold) {
    if (health <= 0) return false;
    // Assassinate uses PlayerDataGlobal's replicated m_iHealthMax.  The pawn
    // field is not interchangeable here: it can contain a provisional value
    // while temporary health/shield state is being updated and produced false
    // low-HP bonus predictions.
    const uintptr_t controller = targetEntity ? ResolveEntity(
        Read<uint32_t>(targetEntity + Offsets::PawnController)) : 0;
    const int playerDataMaxHealth = controller
        ? Read<int>(controller + Offsets::ControllerPlayerData +
                    Offsets::PlayerDataHealthMax) : 0;
    const auto isLow = [health, threshold](int maxHealth) {
        return maxHealth > 0 && maxHealth < 100000 &&
            100.0f * health <= static_cast<float>(maxHealth) * threshold;
    };
    return isLow(playerDataMaxHealth) ||
        (!playerDataMaxHealth && isLow(snapshotMaxHealth));
}

bool VindictaCanKill(const PlayerData& player, uintptr_t ability) {
    if (!ability) return false;
    const float base = VindictaDamageProperty(ability, "Damage", 90.0f);
    const float lowBonus = VindictaDamageProperty(
        ability, "LowHealthEnemyDamageBonus", 90.0f);
    const float lowThreshold = AbilityProperty(
        ability, "LowHealthEnemyThresholdPct", 50.0f);
    const float headshotScale = 1.0f + AbilityProperty(
        ability, "HeadshotBonus", 20.0f) / 100.0f;
    const float damage = (base + (VindictaLowHealthTarget(
        player.entity, player.health, player.maxHealth, lowThreshold) ? lowBonus : 0.0f)) *
        CasterSpiritDamageMultiplier() * headshotScale *
        (1.0f - SpiritResistance(player.entity) / 100.0f);
    const float requiredDamage = static_cast<float>(player.health) *
        (1.0f + kVindictaKillSafetyPercent) + kVindictaKillSafetyFlat;
    return std::isfinite(damage) && damage >= requiredDamage;
}

float VindictaHeadshotDamage(uintptr_t ability, int health, int maxHealth,
                             uintptr_t targetEntity) {
    if (!ability) return 0.0f;
    const float base = VindictaDamageProperty(ability, "Damage", 90.0f);
    const float lowBonus = VindictaDamageProperty(
        ability, "LowHealthEnemyDamageBonus", 90.0f);
    const float lowThreshold = AbilityProperty(
        ability, "LowHealthEnemyThresholdPct", 50.0f);
    const float headshotScale = 1.0f + AbilityProperty(
        ability, "HeadshotBonus", 20.0f) / 100.0f;
    const float raw = (base + (VindictaLowHealthTarget(
        targetEntity, health, maxHealth, lowThreshold) ? lowBonus : 0.0f)) *
        CasterSpiritDamageMultiplier() * headshotScale; // Fully charged Snipe.
    const float result = raw * (1.0f - SpiritResistance(targetEntity) / 100.0f);
    return std::isfinite(result) ? (std::max)(0.0f, result) : 0.0f;
}

bool FirstVisibleAimPoint(const PlayerData& player, uint32_t heroId,
                          float centerX, float centerY, float fov,
                          int lockedBoneIndex, bool allowOffscreen,
                          Vector3& point,
                          float& screenDistance, int& selectedBoneIndex) {
    struct BoneCandidate { bool valid; Vector3 point; };
    const std::array<BoneCandidate, 7> bones{{
        {player.hasHeadBone, player.headPos},
        {player.hasNeckBone, player.neckPos},
        {player.hasBodyBone, player.bodyPos},
        {player.hasLeftArmBone, player.leftArmPos},
        {player.hasRightArmBone, player.rightArmPos},
        {player.hasLeftLegBone, player.leftLegPos},
        {player.hasRightLegBone, player.rightLegPos}}};
    const size_t first = heroId == kVindictaId ? 0 : 1;
    const auto visible = [&](size_t index) {
        return index >= first && index < bones.size() && bones[index].valid &&
            IsWorldAimPointVisible(bones[index].point, player.entity);
    };
    if (lockedBoneIndex >= static_cast<int>(first) &&
        lockedBoneIndex < static_cast<int>(bones.size())) {
        Vector2 screen{};
        const bool projected = WorldToScreen(
            bones[lockedBoneIndex].point, screen, currentViewMatrix);
        if (visible(static_cast<size_t>(lockedBoneIndex)) &&
            (projected || allowOffscreen)) {
            point = bones[lockedBoneIndex].point;
            if (projected) {
                const float dx = screen.x - centerX;
                const float dy = screen.y - centerY;
                screenDistance = dx * dx + dy * dy;
            } else {
                screenDistance = 0.0f;
            }
            selectedBoneIndex = lockedBoneIndex;
            return allowOffscreen || screenDistance < fov * fov;
        }
    }
    bool found = false;
    float nearestDistance = fov * fov;
    for (size_t index = first; index < bones.size(); ++index) {
        if (!bones[index].valid ||
            !IsWorldAimPointVisible(bones[index].point, player.entity))
            continue;
        Vector2 screen{};
        if (!WorldToScreen(bones[index].point, screen, currentViewMatrix)) {
            if (allowOffscreen) {
                point = bones[index].point;
                screenDistance = 0.0f;
                selectedBoneIndex = static_cast<int>(index);
                return true;
            }
            continue;
        }
        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = dx * dx + dy * dy;
        if (distance >= nearestDistance) continue;
        point = bones[index].point;
        nearestDistance = distance;
        selectedBoneIndex = static_cast<int>(index);
        found = true;
    }
    screenDistance = nearestDistance;
    return found;
}

Vector3 SampleTargetVelocity(uintptr_t target, const Vector3& point) {
    const ULONGLONG now = GetTickCount64();
    TargetMotionSample& previous = targetMotionSamples[target];
    if (previous.at && now > previous.at) {
        const float seconds = static_cast<float>(now - previous.at) / 1000.0f;
        // Origin replication is stepped. Measure over 50 ms rather than two
        // adjacent frames, otherwise every received snapshot becomes a spike.
        if (seconds >= 0.050f && seconds <= 0.25f) {
            const Vector3 instantaneous{(point.x - previous.point.x) / seconds,
                                        (point.y - previous.point.y) / seconds,
                                        (point.z - previous.point.z) / seconds};
            const float length = std::sqrt(
                instantaneous.x * instantaneous.x + instantaneous.y * instantaneous.y +
                instantaneous.z * instantaneous.z);
            if (std::isfinite(length) && length <= 2500.0f) {
                // AbsOrigin advances in discrete network snapshots. Smooth
                // that staircase before it becomes an intercept vector.
                const Vector3 result = (previous.velocity.x != 0.0f ||
                    previous.velocity.y != 0.0f || previous.velocity.z != 0.0f)
                    ? Vector3{previous.velocity.x * 0.65f + instantaneous.x * 0.35f,
                              previous.velocity.y * 0.65f + instantaneous.y * 0.35f,
                              previous.velocity.z * 0.65f + instantaneous.z * 0.35f}
                    : instantaneous;
                previous = {point, result, now};
                return result;
            }
        }
    }
    if (!previous.at) previous = {point, {}, now};
    return previous.velocity;
}

Vector3 PredictProjectilePoint(uintptr_t ability, uintptr_t target,
                               const Vector3& point,
                               const Vector3& observedVelocity,
                               const Vector3& observedLocalVelocity,
                               bool hazeDagger,
                               const Vector3* launchSourceOverride = nullptr) {
    if (!ability || !target || !offsets.weaponInfo || !offsets.bulletSpeed)
        return point;
    const uintptr_t vdata = Read<uintptr_t>(ability + kSubclassVDataOffset);
    if (!vdata) return point;
    const uintptr_t weaponInfo = vdata + offsets.weaponInfo;
    float speed = Read<float>(weaponInfo + offsets.bulletSpeed);
    float gravity = offsets.bulletGravity
        ? Read<float>(weaponInfo + offsets.bulletGravity) * kGravityUnits : 0.0f;
    float inherit = offsets.bulletInherit
        ? Read<float>(weaponInfo + offsets.bulletInherit) : 0.0f;
    if (!std::isfinite(inherit) || inherit < 0.0f || inherit > 1.0f)
        inherit = 0.0f;
    if (launchSourceOverride) {
        // EBulletSpeed is 0x4C in the current EStatsType enum. Prefer the
        // evaluated pawn stat because it includes live item/modifier bonuses;
        // the weapon VData value above remains the exact base-speed fallback.
        const float evaluatedSpeed = EntityStat(0x4C, currentLocalPawn);
        if (std::isfinite(evaluatedSpeed) && evaluatedSpeed > 100.0f &&
            evaluatedSpeed <= 100000.0f) {
            speed = evaluatedSpeed;
        }
    }
    if (hazeDagger) {
        // Sleep Dagger uses CitadelAbilityVData::m_WeaponInfo, not the adjacent
        // m_projectileInfo block. Reading m_projectileInfo yielded zero and
        // hid the live gravity value even though m_WeaponInfo was valid.
        if (!std::isfinite(speed) || speed <= 100.0f || speed > 100000.0f)
            speed = kHazeDaggerProjectileSpeedFallback;
        if (!std::isfinite(gravity) || gravity < 0.0f ||
            gravity > kGravityUnits * 10.0f) {
            gravity = kHazeDaggerGravityScaleFallback * kGravityUnits;
        }
    }
    if (!std::isfinite(speed) || speed <= 100.0f || speed > 100000.0f)
        return point;
    if (!std::isfinite(gravity) || gravity < 0.0f ||
        gravity > kGravityUnits * 10.0f) {
        gravity = 0.0f;
    }

    // Match the archive: the replicated movement velocity is authoritative
    // for direction and speed. Position sampling is only a fallback for the
    // brief interpolation windows where that network vector is zeroed.
    Vector3 velocity = Read<Vector3>(target + Offsets::Velocity);
    float velocityLength = std::sqrt(
        velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    // Non-local pawn network velocity can be zeroed by interpolation; use the
    // measured root-transform velocity only during that brief fallback window.
    if (velocityLength < 10.0f || velocityLength > 2500.0f) {
        velocity = observedVelocity;
        velocityLength = std::sqrt(
            velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    }
    Vector3 localVelocity = currentLocalPawn
        ? Read<Vector3>(currentLocalPawn + Offsets::Velocity) : Vector3{};
    float localVelocityLength = std::sqrt(
        localVelocity.x * localVelocity.x + localVelocity.y * localVelocity.y +
        localVelocity.z * localVelocity.z);
    if ((localVelocityLength < 10.0f || localVelocityLength > 2500.0f) &&
        currentLocalPawn) {
        localVelocity = observedLocalVelocity;
        localVelocityLength = std::sqrt(
            localVelocity.x * localVelocity.x + localVelocity.y * localVelocity.y +
            localVelocity.z * localVelocity.z);
    }
    if (!std::isfinite(localVelocityLength) || localVelocityLength > 2500.0f)
        localVelocity = {};
    velocityLength = std::sqrt(
        velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    // Hero sprint speeds are slightly above 800 Source units/sec, and dashes
    // are higher still. A stationary target still needs ballistic correction,
    // so only reject a genuinely invalid velocity here.
    if (!std::isfinite(velocityLength) || velocityLength > 2500.0f) return point;

    // Projectile travel begins at Haze's right palm, not at the third-person
    // camera. Prefer the exact attachment used by abilities.vdata and retain
    // the old origin-height approximation only as a skeleton-read fallback.
    Vector3 source{};
    if (launchSourceOverride) {
        source = *launchSourceOverride;
    } else {
        source = currentLocalPosition;
        if (!currentLocalPawn ||
            !GetEntityBonePosition(currentLocalPawn, "palm_R", source)) {
            source = currentLocalPosition;
            source.z += kHeroProjectileOriginHeight;
        }
    }
    if (!std::isfinite(source.x) || !std::isfinite(source.y) ||
        !std::isfinite(source.z)) {
        source = currentCameraPosition;
    }
    // AbilityCastDelay is an animation/state delay, but the live projectile
    // direction is based on the current command without adding that delay to
    // the intercept horizon. Including it produces a repeatable 0.2-second
    // overlead. Predict only the actual projectile flight here.
    constexpr float castDelay = 0.0f;
    const Vector3 launchSource = source;
    const Vector3 targetAtLaunch = point;
    const Vector3 relativeFlightVelocity{
        velocity.x - localVelocity.x * inherit,
        velocity.y - localVelocity.y * inherit,
        velocity.z - localVelocity.z * inherit};

    const float dx = targetAtLaunch.x - launchSource.x;
    const float dy = targetAtLaunch.y - launchSource.y;
    const float dz = targetAtLaunch.z - launchSource.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    float time = distance / speed;
    Vector3 predicted = targetAtLaunch;
    for (int iteration = 0; iteration < 12; ++iteration) {
        if (!std::isfinite(time) || time <= 0.0f || time > 5.0f) return point;
        // Gravity accelerates the projectile downward, so aim above the
        // future target by 1/2*g*t^2. This is also necessary when velocity=0.
        const float gravityCompensation = 0.5f * gravity * time * time;
        predicted = {
            targetAtLaunch.x + relativeFlightVelocity.x * time,
            targetAtLaunch.y + relativeFlightVelocity.y * time,
            targetAtLaunch.z + relativeFlightVelocity.z * time +
                gravityCompensation};
        const float px = predicted.x - launchSource.x;
        const float py = predicted.y - launchSource.y;
        const float pz = predicted.z - launchSource.z;
        const float newDistance = std::sqrt(px * px + py * py + pz * pz);
        const float next = newDistance / speed;
        if (std::fabs(next - time) < 0.001f) { time = next; break; }
        time = next;
    }
    if (time <= 0.0f || time > 5.0f) return point;
    if (hazeDagger) {
        static ULONGLONG lastPredictionLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastPredictionLog >= 1000) {
            lastPredictionLog = now;
            const float leadX = predicted.x - point.x;
            const float leadY = predicted.y - point.y;
            const float leadZ = predicted.z - point.z;
            const float lead = std::sqrt(leadX * leadX + leadY * leadY + leadZ * leadZ);
            std::lock_guard<std::mutex> lock(diagnosticsMutex);
            std::ofstream log(Dll6Paths::DataFileA("haze_prediction.log"),
                              std::ios::app);
            if (log) {
                log << now << " distance=" << distance
                    << " speed=" << speed
                    << " gravity=" << gravity
                    << " castDelay=" << castDelay
                    << " flightTime=" << time
                    << " totalTime=" << castDelay + time
                    << " targetVel=" << velocityLength
                    << " vel=(" << velocity.x << ',' << velocity.y << ',' << velocity.z << ')'
                    << " inherit=" << inherit
                    << " gravityComp=" << 0.5f * gravity * time * time
                    << " lead=" << lead
                    << " leadVec=(" << leadX << ',' << leadY << ',' << leadZ << ")\n";
            }
        }
    } else if (launchSourceOverride) {
        static ULONGLONG lastPlayerAimPredictionLog = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastPlayerAimPredictionLog >= 1000) {
            lastPlayerAimPredictionLog = now;
            const float leadX = predicted.x - point.x;
            const float leadY = predicted.y - point.y;
            const float leadZ = predicted.z - point.z;
            std::lock_guard<std::mutex> lock(diagnosticsMutex);
            std::ofstream log(Dll6Paths::DataFileA("player_aim_prediction.log"),
                              std::ios::app);
            if (log) {
                log << now << " weapon=" << std::hex << ability << std::dec
                    << " speed=" << speed
                    << " distance=" << distance
                    << " flightTime=" << time
                    << " targetVel=(" << velocity.x << ',' << velocity.y << ','
                    << velocity.z << ')'
                    << " inherit=" << inherit
                    << " gravity=" << gravity
                    << " leadVec=(" << leadX << ',' << leadY << ',' << leadZ
                    << ")\n";
            }
        }
    }
    return predicted;
}

bool CommandHasMask(const CUserCmd* command, uint64_t mask) {
    if (!command) return false;
    if ((command->buttonStates.buttonState1 & mask) != 0) return true;
    if (!command->cmd.has_base()) return false;
    const auto& base = command->cmd.base();
    return base.has_buttons_pb() &&
        (base.buttons_pb().buttonstate1() & mask) != 0;
}

void ApplyButtonMasks(CUserCmd* command, uint64_t clearMask,
                      uint64_t holdMask, uint64_t tapMask) {
    if (!command) return;
    const uint64_t keepMask = ~clearMask;
    command->buttonStates.buttonState1 =
        (command->buttonStates.buttonState1 & keepMask) | holdMask | tapMask;
    command->buttonStates.buttonState2 =
        (command->buttonStates.buttonState2 & keepMask) | tapMask;
    command->buttonStates.buttonState3 =
        (command->buttonStates.buttonState3 & keepMask) | tapMask;
    if (command->cmd.has_base()) {
        if (auto* buttons = command->cmd.mutable_base()->mutable_buttons_pb()) {
            buttons->set_buttonstate1(
                (buttons->buttonstate1() & keepMask) | holdMask | tapMask);
            buttons->set_buttonstate2(
                (buttons->buttonstate2() & keepMask) | tapMask);
            buttons->set_buttonstate3(
                (buttons->buttonstate3() & keepMask) | tapMask);
        }
    }
}

TargetSnapshot ReadTarget() {
    std::lock_guard<std::mutex> lock(targetMutex);
    return targetSnapshot;
}

TargetSnapshot ReadBebopAbility2Target() {
    std::lock_guard<std::mutex> lock(targetMutex);
    return bebopAbility2TargetSnapshot;
}

bool TargetUsable(const TargetSnapshot& target, uint32_t heroId) {
    return target.entity && target.heroId == heroId &&
        GetTickCount64() - target.updatedAt <= 200;
}

bool TargetCenteredOnScreen(const TargetSnapshot& target) {
    if (!target.entity || !currentViewMatrixReady) return false;
    Vector2 screen{};
    if (!WorldToScreen(target.point, screen, currentViewMatrix)) return false;
    const ImVec2 size = ImGui::GetIO().DisplaySize;
    const float dx = screen.x - size.x * 0.5f;
    const float dy = screen.y - size.y * 0.5f;
    return dx * dx + dy * dy <= 6.0f * 6.0f;
}

bool QueueSilentHeroTarget(const TargetSnapshot& target, bool attack,
                           bool overridePrimaryAim = true) {
    if (!target.inAbilityRange) return false;
    Vector3 source{};
    if (!ResolveHeroAimSource(target.heroId, source)) return false;
    const Vector3 angles = CalculateAngles(source, target.point);
    if (!std::isfinite(angles.x) || !std::isfinite(angles.y))
        return false;

    const Vector3 silentAngles{
        std::clamp(angles.x, -89.0f, 89.0f), NormalizeAngle(angles.y), 0.0f};
    // Projectile casts launch after their animation, so their angle must keep
    // priority over held primary fire until PostFire ends, not only on the
    // initial Ability 1 command.
    QueueHeroSilentAngles(
        silentAngles, attack,
        overridePrimaryAim &&
            (target.heroId == kHazeId || target.heroId == kShivId ||
             target.heroId == kBebopId || target.heroId == kDrifterId));
    return true;
}

void SetCommandAngles(CUserCmd* command, const TargetSnapshot& target,
                       float smoothX, float smoothY, float threshold,
                       bool& aligned, uintptr_t input) {
    if (!command || !command->cmd.has_ang_camera_angles()) return;
    const auto& current = command->cmd.ang_camera_angles();
    Vector3 currentAngles{current.x(), current.y(), current.z()};
    if (commandState.hasLastAimAngles) {
        currentAngles = commandState.lastAimAngles;
    } else if (input) {
        const Vector3 inputAngles = Read<Vector3>(input + 0x688);
        if (std::isfinite(inputAngles.x) && std::isfinite(inputAngles.y))
            currentAngles = inputAngles;
    }
    Vector3 feedForward{currentAngles.x + target.angularVelocity.x,
                        currentAngles.y + target.angularVelocity.y, 0.0f};
    feedForward.x = std::clamp(feedForward.x, -89.0f, 89.0f);
    feedForward.y = NormalizeAngle(feedForward.y);
    const float pitchDifference = NormalizeAngle(target.rawAngles.x - feedForward.x);
    const float yawDifference = NormalizeAngle(target.rawAngles.y - feedForward.y);
    commandState.finalAngles = {
        std::clamp(feedForward.x + pitchDifference / (smoothX + 1.0f), -89.0f, 89.0f),
        NormalizeAngle(feedForward.y + yawDifference / (smoothY + 1.0f)), 0.0f};
    commandState.writeAngles = true;
    commandState.lastAimAngles = commandState.finalAngles;
    commandState.hasLastAimAngles = true;
    aligned = AngularError(commandState.finalAngles, target.rawAngles) <= threshold;
    ApplyCurrentCameraAim(target.point);
}

void ResetCommandState(bool keepHeld = false) {
    const bool held = commandState.wasAbility1Held;
    commandState = {};
    ClearHeroSilentAngles();
    if (keepHeld) commandState.wasAbility1Held = held;
}

float CachedPrimaryWeaponRange() {
    InitializeOffsets();
    static uintptr_t cachedPawn = 0;
    static uintptr_t cachedWeapon = 0;
    static float cachedRange = std::numeric_limits<float>::infinity();
    static ULONGLONG lastRefresh = 0;
    const ULONGLONG now = GetTickCount64();
    if (cachedPawn != currentLocalPawn || now - lastRefresh >= 250) {
        cachedPawn = currentLocalPawn;
        cachedWeapon = FindPrimaryWeapon();
        cachedRange = cachedWeapon
            ? ScriptProjectileRange(0, cachedWeapon)
            : std::numeric_limits<float>::infinity();
        lastRefresh = now;
    }
    // Drifter's swipe projectile has a verified 1080-unit travel limit. Its
    // subclass can expose a generic ability range before the embedded weapon
    // fields are ready, so clamp that transient value to the physical limit.
    constexpr uint32_t kDrifterHeroId = 64;
    const uint32_t heroId = currentLocalPawn
        ? Read<uint32_t>(currentLocalPawn + Offsets::HeroComponent +
                         Offsets::HeroSpawnedId)
        : 0;
    if (heroId == kDrifterHeroId)
        return std::isfinite(cachedRange)
            ? (std::min)(cachedRange, 1080.0f) : 1080.0f;
    return cachedRange;
}

} // namespace

bool PrimaryWeaponTargetInRange(uintptr_t target) {
    // Missing range metadata means the weapon is hitscan or currently cannot
    // be classified, so fail open. A published finite projectile range is
    // enforced with the same collision allowance as scripted abilities.
    if (!target || !currentLocalPawn || !currentLocalPositionReady)
        return true;
    return TargetInProjectileRange(CachedPrimaryWeaponRange(), target);
}

bool PrimaryWeaponPointInRange(const Vector3& point) {
    if (!currentLocalPawn || !currentLocalPositionReady) return true;
    const float range = CachedPrimaryWeaponRange();
    if (!std::isfinite(range)) return true;
    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
        !std::isfinite(point.z)) return false;
    Vector3 source = currentLocalPosition;
    source.z += kHeroProjectileOriginHeight;
    const float dx = point.x - source.x;
    const float dy = point.y - source.y;
    const float dz = point.z - source.z;
    return dx * dx + dy * dy + dz * dz <= range * range;
}

Vector3 PredictPlayerAimPoint(uintptr_t target, const Vector3& point,
                              const Vector3& targetOrigin) {
    if (!target || !currentLocalPawn || !currentLocalPositionReady)
        return point;
    InitializeOffsets();

    // The primary weapon is stored in the pawn's ability vector at the
    // verified ESlot_Weapon_Primary value (0x15). Its CitadelAbilityVData owns
    // the same embedded CCitadelWeaponInfo used by projectile hero abilities.
    // m_hActiveWeapon is a CBasePlayerWeapon and is only a fallback: treating
    // it as CitadelAbilityVData made prediction silently return the raw point.
    uintptr_t weapon = FindPrimaryWeapon();
    if (!weapon) return point;

    const Vector3 observedTargetVelocity =
        SampleTargetVelocity(target, targetOrigin);
    const Vector3 observedLocalVelocity =
        SampleTargetVelocity(currentLocalPawn, currentLocalPosition);

    // Estimate flight time from the pawn's upper body (near the muzzle), not
    // from the third-person camera behind it. The eventual aim angle still
    // uses the normal camera/silent-aim pipeline.
    Vector3 launchSource = currentLocalPosition;
    launchSource.z += kHeroProjectileOriginHeight;
    return PredictProjectilePoint(
        weapon, target, point, observedTargetVelocity,
        observedLocalVelocity, false, &launchSource);
}

bool HeroScriptsNeedPlayerBones() {
    return vindictaAutoSnipeEnabled || hazeSleepDaggerEnabled ||
           shivSerratedKnivesEnabled ||
           drifterAbility2Enabled;
}

void UpdateHeroScriptTargets(const std::vector<PlayerData>& players) {
    InitializeOffsets();
    const uint32_t heroId = CurrentHeroId();
    TargetSnapshot next{};
    if (menuOpen || !ScriptEnabled(heroId) || !currentViewMatrixReady ||
        !currentLocalPawn || !currentLocalPositionReady) {
        std::lock_guard<std::mutex> lock(targetMutex);
        targetSnapshot = {};
        bebopAbility2TargetSnapshot = {};
        return;
    }

    const uint8_t localTeam = Read<uint8_t>(currentLocalPawn + Offsets::Team);
    const float centerX = ImGui::GetIO().DisplaySize.x * 0.5f;
    const float centerY = ImGui::GetIO().DisplaySize.y * 0.5f;

    if (heroId == kBebopId) {
        TargetSnapshot autoTarget{};
        if (bebopAbility2AutoEnabled) {
            const uintptr_t stickyBomb = FindAbility(1);
            const float castRange = AbilityProperty(
                stickyBomb, "AbilityCastRange", 240.0f);
            float nearestDistanceSquared = std::numeric_limits<float>::infinity();
            for (const PlayerData& player : players) {
                if (!player.entity || player.health <= 0 ||
                    player.team == localTeam ||
                    !TargetInProjectileRange(castRange, player.entity))
                    continue;
                Vector3 point{};
                float screenDistance = 0.0f;
                int boneIndex = -1;
                if (!FirstVisibleAimPoint(
                        player, kBebopId, centerX, centerY, 100000.0f, -1,
                        false, point, screenDistance, boneIndex)) {
                    // Sticky Bomb does not need a skeleton-specific hit point.
                    // Keep the automatic cast available during short bone
                    // snapshot gaps, but still require a live visibility trace.
                    point = player.hasBodyBone
                        ? player.bodyPos
                        : Vector3{player.worldPos.x, player.worldPos.y,
                                  player.worldPos.z + 52.0f};
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                        !std::isfinite(point.z) ||
                        !IsWorldAimPointVisible(point, player.entity))
                        continue;
                }
                const float dx = player.worldPos.x - currentLocalPosition.x;
                const float dy = player.worldPos.y - currentLocalPosition.y;
                const float dz = player.worldPos.z - currentLocalPosition.z;
                const float distanceSquared = dx * dx + dy * dy + dz * dz;
                if (distanceSquared >= nearestDistanceSquared) continue;
                nearestDistanceSquared = distanceSquared;
                autoTarget.entity = player.entity;
                autoTarget.point = point;
                autoTarget.boneIndex = boneIndex;
                autoTarget.heroId = kBebopId;
                autoTarget.health = player.health;
                autoTarget.maxHealth = player.maxHealth;
                autoTarget.updatedAt = GetTickCount64();
                autoTarget.inAbilityRange = true;
                Vector3 aimSource{};
                if (ResolveHeroAimSource(kBebopId, aimSource))
                    autoTarget.rawAngles = CalculateAngles(aimSource, point);
            }
        }
        {
            std::lock_guard<std::mutex> lock(targetMutex);
            bebopAbility2TargetSnapshot = autoTarget;
        }
        if (!bebopAbility3Enabled) {
            std::lock_guard<std::mutex> lock(targetMutex);
            targetSnapshot = {};
            return;
        }
    } else {
        std::lock_guard<std::mutex> lock(targetMutex);
        bebopAbility2TargetSnapshot = {};
    }

    const float fov = ScriptFov(heroId);
    const TargetSnapshot previous = ReadTarget();
    const Vector3 observedLocalVelocity = SampleTargetVelocity(
        currentLocalPawn, currentLocalPosition);
    uintptr_t ability = FindAbility(ScriptAbilitySlot(heroId));
    float bestDistance = fov * fov;

    for (const PlayerData& player : players) {
        if (!player.entity || player.health <= 0 || player.team == localTeam)
            continue;
        const bool forcedTarget = aimLockedTarget &&
            player.entity == aimLockedTarget;
        if (aimLockedTarget && !forcedTarget) continue;
        if (heroId == kVindictaId && !VindictaCanKill(player, ability))
            continue;
        Vector3 point{};
        float distance = 0.0f;
        int boneIndex = -1;
        // Vindicta's scope changes the projection scale sharply. A target
        // acquired before opening the scope must not be discarded solely
        // because it is now outside the pre-scope 60px circle.
        const float candidateFov = forcedTarget ? 100000.0f :
            (heroId == kVindictaId && player.entity == previous.entity
                ? 2000.0f : fov);
        // Sleep Dagger must reacquire from scratch every frame. Reusing the
        // previous pawn's bone biases its screen distance and turns the first
        // selected enemy into a sticky target.
        const int lockedBoneIndex = heroId != kHazeId &&
            player.entity == previous.entity ? previous.boneIndex : -1;
        if (!FirstVisibleAimPoint(player, heroId, centerX, centerY,
                                  candidateFov, lockedBoneIndex, forcedTarget,
                                  point, distance, boneIndex)) {
            if (heroId != kBebopId) continue;
            // Hook targets the pawn body and does not require a complete
            // seven-bone pose. Avoid rebuilding every hero skeleton on the
            // engine callback merely because either Bebop script is enabled.
            point = {player.worldPos.x, player.worldPos.y,
                     player.worldPos.z + 52.0f};
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.z) ||
                !IsWorldAimPointVisible(point, player.entity))
                continue;
            Vector2 screen{};
            if (!WorldToScreen(point, screen, currentViewMatrix)) {
                if (!forcedTarget) continue;
                distance = 0.0f;
            } else {
                const float dx = screen.x - centerX;
                const float dy = screen.y - centerY;
                distance = dx * dx + dy * dy;
                if (!forcedTarget && distance >= candidateFov * candidateFov)
                    continue;
            }
            boneIndex = -1;
        }
        if (forcedTarget) {
            // The shared player lock bypasses every aim FOV, including hero
            // scripts. Visibility and each script's readiness rules remain.
        } else if (heroId == kHazeId) {
            if (distance >= bestDistance) continue;
        } else if (player.entity != previous.entity &&
                   distance >= bestDistance) {
            continue;
        }
        if (heroId != kVindictaId) {
            // AbsOrigin is updated ahead of the interpolated skeleton while
            // a pawn is running. Measure that root transform, then apply the
            // velocity to the selected bone point.
            const Vector3 observedVelocity = SampleTargetVelocity(
                player.entity, player.worldPos);
            point = PredictProjectilePoint(
                ability, player.entity, point, observedVelocity, observedLocalVelocity,
                heroId == kHazeId);
        }
        next.entity = player.entity;
        next.point = point;
        next.boneIndex = boneIndex;
        next.heroId = heroId;
        next.health = player.health;
        next.maxHealth = player.maxHealth;
        next.updatedAt = GetTickCount64();
        next.inAbilityRange = ScriptTargetInAbilityRange(
            heroId, ability, player.entity);
        Vector3 aimSource{};
        if (!ResolveHeroAimSource(heroId, aimSource)) continue;
        next.rawAngles = CalculateAngles(aimSource, point);
        if (player.entity == previous.entity && previous.updatedAt) {
            next.angularVelocity.x = std::clamp(
                NormalizeAngle(next.rawAngles.x - previous.rawAngles.x), -2.0f, 2.0f);
            next.angularVelocity.y = std::clamp(
                NormalizeAngle(next.rawAngles.y - previous.rawAngles.y), -2.0f, 2.0f);
        }
        if (heroId != kHazeId && player.entity == previous.entity) break;
        bestDistance = distance;
    }

    // Visibility results are produced on the gameplay thread and can miss one
    // render frame while a trace is refreshed. Keep the last valid lock for a
    // very short grace period instead of cancelling an already opened scope.
    if (heroId == kVindictaId && !next.entity && previous.entity &&
        previous.heroId == heroId &&
        GetTickCount64() - previous.updatedAt <= 150) {
        next = previous;
    }

    static ULONGLONG lastTargetLog = 0;
    const ULONGLONG logNow = GetTickCount64();
    if (logNow - lastTargetLog >= 1000) {
        lastTargetLog = logNow;
        LogDiagnostics("target", heroId, ability, next.entity,
                       static_cast<int>(players.size()));
    }

    std::lock_guard<std::mutex> lock(targetMutex);
    targetSnapshot = next;
}

bool ProcessHeroScriptsUserCmd(CUserCmd* command, bool processInput,
                               uintptr_t input) {
    if (!command) return false;
    InitializeOffsets();
    const uint32_t heroId = CurrentHeroId();
    if (!ScriptEnabled(heroId) || menuOpen) {
        ResetCommandState();
        return false;
    }

    if (processInput &&
        commandState.lastCommand == reinterpret_cast<uintptr_t>(command)) {
        processInput = false;
    }
    if (!processInput) {
        if (commandState.lastCommand == reinterpret_cast<uintptr_t>(command)) {
            ApplyButtonMasks(command, commandState.clearMask,
                             commandState.holdMask, commandState.tapMask);
            if (commandState.writeAngles && command->cmd.has_ang_camera_angles()) {
                auto* angles = command->cmd.mutable_ang_camera_angles();
                angles->set_x(commandState.finalAngles.x);
                angles->set_y(commandState.finalAngles.y);
                angles->set_z(0.0f);
                command->cmd.clear_view_delta_x();
                command->cmd.clear_view_delta_y();
                if (input)
                    Write<Vector3>(input + 0x688, commandState.finalAngles);
            }
        }
        return commandState.writeAngles || commandState.clearMask ||
               commandState.holdMask || commandState.tapMask;
    }

    commandState.lastCommand = reinterpret_cast<uintptr_t>(command);
    commandState.clearMask = 0;
    commandState.holdMask = 0;
    commandState.tapMask = 0;
    commandState.writeAngles = false;
    const TargetSnapshot target = ReadTarget();
    const float now = GetClientGameTime();

    if (heroId == kBebopId && bebopAbility2AutoEnabled) {
        static ULONGLONG nextAutomaticCastAt = 0;
        static uintptr_t stickyAimCommand = 0;
        if (stickyAimCommand &&
            stickyAimCommand != reinterpret_cast<uintptr_t>(command)) {
            ClearHeroSilentAngles();
            stickyAimCommand = 0;
        }
        const ULONGLONG commandNow = GetTickCount64();
        const TargetSnapshot stickyTarget = ReadBebopAbility2Target();
        const uintptr_t stickyBomb = FindAbility(1);
        const float castRange = AbilityProperty(
            stickyBomb, "AbilityCastRange", 240.0f);
        const bool validStickyTarget = stickyTarget.entity &&
            stickyTarget.heroId == kBebopId &&
            commandNow - stickyTarget.updatedAt <= 350 &&
            TargetInProjectileRange(castRange, stickyTarget.entity) &&
            IsWorldAimPointVisible(stickyTarget.point, stickyTarget.entity);
        if (commandNow >= nextAutomaticCastAt && validStickyTarget &&
            AbilityReady(stickyBomb, false)) {
            ResetCommandState(true);
            commandState.lastCommand = reinterpret_cast<uintptr_t>(command);
            if (QueueSilentHeroTarget(stickyTarget, false, false)) {
                commandState.tapMask |= kAbility2Mask;
                stickyAimCommand = reinterpret_cast<uintptr_t>(command);
                nextAutomaticCastAt = commandNow + 300;
                ApplyButtonMasks(command, 0, 0, commandState.tapMask);
                return true;
            }
        }
    }

    // A newly selected shared lock must take effect immediately. Do not let a
    // script finish an old scope/aim state against a different pawn.
    if (aimLockedTarget && commandState.lockedEntity &&
        commandState.lockedEntity != aimLockedTarget) {
        ResetCommandState(true);
    }

    if (heroId == kVindictaId) {
        const uintptr_t ability = FindAbility(3);
        const bool scoped = ability && offsets.scopeStartTime &&
            Read<float>(ability + offsets.scopeStartTime) > 0.0f;
        if (commandState.state == ScriptState::Idle &&
            TargetUsable(target, heroId) && AbilityReady(ability, true)) {
            LogDiagnostics("vindicta-acquire", heroId, ability, target.entity);
            commandState.state = ScriptState::Scoping;
            commandState.lockedEntity = target.entity;
            commandState.lockedTarget = target;
            commandState.scopeStartedAt = GetTickCount64();
        }
        if (commandState.state == ScriptState::Scoping) {
            // The render-side visibility cache can have a one-frame gap,
            // especially at the instant the scope changes the projection.
            // Keep the same target briefly, matching the reference's locked
            // pawn behavior, instead of reopening/cancelling the scope.
            TargetSnapshot activeTarget = commandState.lockedTarget;
            if (TargetUsable(target, heroId) &&
                target.entity == commandState.lockedEntity) {
                activeTarget = target;
                commandState.lockedTarget = target;
            }
            if (!activeTarget.entity || !ability ||
                GetTickCount64() - activeTarget.updatedAt > 500) {
                ResetCommandState();
                return false;
            }
            commandState.holdMask |= kAbility4Mask;
            // Use the project's existing screen-to-command conversion, but
            // publish the result only to CUserCmd. The visible camera remains
            // untouched, which makes the script a genuine silent aim path.
            // ScopeStartTime is absent or stale in some current client builds.
            // Holding the ability starts the scope; this internal delay keeps
            // the firing state independent of that optional networked field.
            // Hornet Snipe reaches full damage after one second of scope. Do
            // not use m_flScopeStartTime as the sole source here: it is
            // replicated a frame late on some builds, whereas this monotonic
            // timer starts on the exact command that holds ability 4.
            const bool scopeDelayElapsed = GetTickCount64() -
                commandState.scopeStartedAt >= 1000;
            const bool attack = scopeDelayElapsed;
            const bool haveSilentAngles =
                QueueSilentHeroTarget(activeTarget, attack);
            if (attack && haveSilentAngles) {
                LogDiagnostics("vindicta-fire", heroId, ability, activeTarget.entity,
                               1);
                commandState.state = ScriptState::PostFire;
                commandState.scopeStartedAt = GetTickCount64();
            }
        } else if (commandState.state == ScriptState::PostFire &&
                   GetTickCount64() - commandState.scopeStartedAt >= 250) {
            ResetCommandState();
        }
    } else {
        const uint64_t abilityMask = ScriptAbilityMask(heroId);
        const bool held = CommandHasMask(command, abilityMask);
        const bool freshPress = held && !commandState.wasAbility1Held;
        commandState.wasAbility1Held = held;
        const uintptr_t ability = FindAbility(ScriptAbilitySlot(heroId));
        const bool requireCharge = heroId == kShivId;
        const bool targetUsable = TargetUsable(target, heroId);
        const bool targetInRange = targetUsable && target.inAbilityRange &&
            ScriptTargetInAbilityRange(heroId, ability, target.entity);
        // The original physical press is already present in CUserCmd before
        // hero scripts run. Explicitly remove it when the selected target is
        // beyond the projectile's travel distance.
        if (held && targetUsable && !targetInRange)
            commandState.clearMask |= abilityMask;
        if (freshPress)
            LogDiagnostics("ability1-press", heroId, ability, target.entity,
                           AbilityReady(ability, requireCharge) ? 1 : 0);
        if (commandState.state == ScriptState::Idle && freshPress &&
            targetUsable && targetInRange &&
            AbilityReady(ability, requireCharge)) {
            commandState.state = ScriptState::Aiming;
            commandState.lockedEntity = target.entity;
        }
        if (commandState.state == ScriptState::Aiming) {
            commandState.clearMask |= abilityMask;
            if (!targetUsable ||
                (heroId != kHazeId &&
                 target.entity != commandState.lockedEntity) ||
                !AbilityReady(ability, requireCharge) ||
                !targetInRange) {
                const bool blockOutOfRange = targetUsable && !targetInRange;
                ResetCommandState(true);
                if (blockOutOfRange) {
                    ApplyButtonMasks(command, abilityMask, 0, 0);
                    return true;
                }
                return false;
            }
            if (heroId == kHazeId) {
                // Follow the currently closest-to-crosshair pawn rather than
                // the one that happened to be selected on button-down.
                commandState.lockedEntity = target.entity;
                // The dagger must be released in the same command that gets
                // its direction. The input hooks process hero scripts before
                // applying pending pSilent angles, so this queue is consumed
                // immediately rather than one command later.
                // Publish through the same pending-angle path as the proven
                // ordinary pSilent implementation. The hook applies this to
                // the protobuf command and every input-history entry after
                // the Ability 1 tap has been added to this same command.
                if (!QueueSilentHeroTarget(target, false))
                    return false;
                commandState.tapMask |= kAbility1Mask;
                commandState.state = ScriptState::PostFire;
                // Keep the corrected angle stable through the short ability
                // animation. The intercept solver deliberately does not add
                // this animation delay to the projectile flight horizon.
                const float castDelay = std::clamp(AbilityProperty(
                    ability, "AbilityCastDelay", kHazeDaggerCastDelayFallback),
                    0.0f, 1.0f);
                commandState.nextActionTime = now + castDelay + 0.05f;
            } else {
                // Shiv, Bebop Ability 3, and Drifter Ability 2 use the same
                // silent projectile-command path as Sleep Dagger. The target
                // snapshot already contains the live intercept, so do not
                // rotate the visible camera or wait for smoothing.
                if (!QueueSilentHeroTarget(target, false))
                    return false;
                commandState.tapMask |= abilityMask;
                commandState.state = ScriptState::PostFire;
                const float castDelay = std::clamp(AbilityProperty(
                    ability, "AbilityCastDelay", 0.0f), 0.0f, 1.0f);
                commandState.nextActionTime = now + castDelay + 0.05f;
            }
        } else if (commandState.state == ScriptState::Firing) {
            commandState.clearMask |= abilityMask;
            if (!targetUsable || !targetInRange ||
                target.entity != commandState.lockedEntity ||
                !AbilityReady(ability, false)) {
                const bool blockOutOfRange = targetUsable && !targetInRange;
                ResetCommandState(true);
                if (blockOutOfRange) {
                    ApplyButtonMasks(command, abilityMask, 0, 0);
                    return true;
                }
                return false;
            }
            commandState.tapMask |= abilityMask;
            commandState.state = ScriptState::PostFire;
            commandState.nextActionTime = now + 0.2f;
        } else if (commandState.state == ScriptState::PostFire) {
            if (targetUsable && targetInRange &&
                (heroId == kHazeId ||
                 target.entity == commandState.lockedEntity) &&
                now < commandState.nextActionTime) {
                if (heroId == kHazeId)
                    commandState.lockedEntity = target.entity;
                if (heroId == kHazeId || heroId == kShivId ||
                    heroId == kBebopId || heroId == kDrifterId) {
                    QueueSilentHeroTarget(target, false);
                }
            } else {
                ResetCommandState(true);
            }
        }
    }

    ApplyButtonMasks(command, commandState.clearMask,
                     commandState.holdMask, commandState.tapMask);
    if (commandState.writeAngles && command->cmd.has_ang_camera_angles()) {
        auto* angles = command->cmd.mutable_ang_camera_angles();
        angles->set_x(commandState.finalAngles.x);
        angles->set_y(commandState.finalAngles.y);
        angles->set_z(0.0f);
        command->cmd.clear_view_delta_x();
        command->cmd.clear_view_delta_y();
        if (input)
            Write<Vector3>(input + 0x688, commandState.finalAngles);
    }
    return commandState.writeAngles || commandState.clearMask ||
           commandState.holdMask || commandState.tapMask;
}

void DrawHeroScriptsOverlay() {
    if (menuOpen || !currentViewMatrixReady) return;
    const uint32_t heroId = CurrentHeroId();
    if (!ScriptEnabled(heroId)) return;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    if (!drawList) return;
    const ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f,
                        ImGui::GetIO().DisplaySize.y * 0.5f);
    if (heroScriptsShowFov) {
        drawList->AddCircle(center, ScriptFov(heroId),
                            IM_COL32(255, 255, 255, 165), 96, 1.0f);
    }
    // For projectile abilities show the exact intercept point used by the
    // command path.  This makes lead/ballistic compensation observable and
    // avoids confusing the predicted point with the model's current position.
    if (heroId == kHazeId && hazePredictionDot) {
        const TargetSnapshot target = ReadTarget();
        Vector2 interceptScreen{};
        if (TargetUsable(target, heroId) &&
            WorldToScreen(target.point, interceptScreen, currentViewMatrix)) {
            const ImVec2 intercept(interceptScreen.x, interceptScreen.y);
            constexpr ImU32 color = IM_COL32(255, 214, 56, 245);
            drawList->AddCircleFilled(intercept, 3.5f, color, 16);
            drawList->AddCircle(intercept, 6.0f, IM_COL32(0, 0, 0, 220), 16,
                                1.0f);
            drawList->AddLine({intercept.x - 9.0f, intercept.y},
                              {intercept.x + 9.0f, intercept.y}, color, 1.25f);
            drawList->AddLine({intercept.x, intercept.y - 9.0f},
                              {intercept.x, intercept.y + 9.0f}, color, 1.25f);
        }
    }
}

void ResetHeroScripts() {
    {
        std::lock_guard<std::mutex> lock(targetMutex);
        targetSnapshot = {};
        bebopAbility2TargetSnapshot = {};
    }
    targetMotionSamples.clear();
    ResetCommandState();
}
