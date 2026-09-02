#include "shared.h"
#include <cstring>
#include <unordered_map>

namespace {
using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);
using EnvSkyUpdateFn = void* (__fastcall*)(uintptr_t);

struct ConVarEntry { uintptr_t cvar{}; int16_t unknown{}; int16_t next{-1}; int32_t unknown2{}; };
struct SkyState { ColorRGBA tint{}, lightTint{}; float brightness{1.f}; bool enabled{true}; };

std::unordered_map<uintptr_t, SkyState> skyStates;
uintptr_t drawSkybox{}, draw3dSkybox{};
bool oldDrawSkybox{true}, oldDraw3dSkybox{true}, cvarsCaptured{};
uintptr_t postProcessEnable{};
bool oldPostProcessEnable{true}, postProcessCaptured{}, postProcessSuppressed{};
EnvSkyUpdateFn updateSky{};
ULONGLONG nextScan{};
uint32_t nextEntityIndex{};
uintptr_t skyTint{}, skyLightTint{}, skyBrightness{}, skyEnabled{};
bool resolved{};
bool worldStateApplied{};
bool appliedSkySettingsValid{};
ColorRGBA appliedSkyTint{}, appliedLightTint{};
float appliedSkyBrightness{};
bool appliedSkyEnabled{};

bool Readable(uintptr_t address, size_t size = sizeof(uintptr_t)) {
    if (address < 0x10000 || !size) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address <= end && size <= end - address;
}

ColorRGBA ColorFrom(const float* value) {
    return {static_cast<uint8_t>(std::clamp(value[0], 0.f, 1.f) * 255.f + .5f),
            static_cast<uint8_t>(std::clamp(value[1], 0.f, 1.f) * 255.f + .5f),
            static_cast<uint8_t>(std::clamp(value[2], 0.f, 1.f) * 255.f + .5f), 255};
}

bool SameColor(const ColorRGBA& a, const ColorRGBA& b) {
    return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

void ResolveFields() {
    if (resolved) return;
    skyTint = ResolveRuntimeSchemaOffset("C_EnvSky", "m_vTintColor");
    skyLightTint = ResolveRuntimeSchemaOffset("C_EnvSky", "m_vTintColorLightingOnly");
    skyBrightness = ResolveRuntimeSchemaOffset("C_EnvSky", "m_flBrightnessScale");
    skyEnabled = ResolveRuntimeSchemaOffset("C_EnvSky", "m_bEnabled");
    updateSky = reinterpret_cast<EnvSkyUpdateFn>(FindUniqueClientPattern(
        "40 53 48 83 EC 30 48 8B D9 E8 ? ? ? ? 48 8B 43"));
    resolved = true;
}

uintptr_t FindConVar(const char* wanted) {
    HMODULE tier0 = GetModuleHandleA("tier0.dll");
    auto factory = tier0 ? reinterpret_cast<CreateInterfaceFn>(GetProcAddress(tier0, "CreateInterface")) : nullptr;
    const uintptr_t system = factory ? reinterpret_cast<uintptr_t>(factory("VEngineCvar007", nullptr)) : 0;
    const uintptr_t entries = system ? Read<uintptr_t>(system + 0x48) : 0;
    if (!Readable(entries, sizeof(ConVarEntry))) return 0;
    for (uint16_t index = 0, count = 0; index != 0xFFFFu && count++ < 65535;) {
        const ConVarEntry entry = Read<ConVarEntry>(entries + static_cast<uintptr_t>(index) * sizeof(ConVarEntry));
        const uintptr_t name = entry.cvar ? Read<uintptr_t>(entry.cvar) : 0;
        if (Readable(name, 1)) {
            __try { if (std::strcmp(reinterpret_cast<const char*>(name), wanted) == 0) return entry.cvar; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        index = static_cast<uint16_t>(entry.next);
    }
    return 0;
}

bool CVarBool(uintptr_t cvar) { return cvar && Read<uint8_t>(cvar + 0x58) != 0; }
void SetCVarBool(uintptr_t cvar, bool value) { if (cvar) Write<uint8_t>(cvar + 0x58, value ? 1 : 0); }

void NotifySky(uintptr_t entity) {
    if (!updateSky) return;
    __try { updateSky(entity); } __except (EXCEPTION_EXECUTE_HANDLER) { updateSky = nullptr; }
}

void ApplySky(uintptr_t entity, const ColorRGBA& tint,
              const ColorRGBA& lightingTint, float brightness,
              bool enabled, bool force = false) {
    if (!skyTint || !skyLightTint || !skyBrightness || !skyEnabled) return;
    auto [it, fresh] = skyStates.try_emplace(entity);
    if (fresh) it->second = {Read<ColorRGBA>(entity + skyTint), Read<ColorRGBA>(entity + skyLightTint),
                             Read<float>(entity + skyBrightness), Read<bool>(entity + skyEnabled)};
    bool changed = force || fresh;
    if (force || !SameColor(Read<ColorRGBA>(entity + skyTint), tint)) {
        Write<ColorRGBA>(entity + skyTint, tint);
        changed = true;
    }
    if (force || !SameColor(Read<ColorRGBA>(entity + skyLightTint), lightingTint)) {
        Write<ColorRGBA>(entity + skyLightTint, lightingTint);
        changed = true;
    }
    if (force || std::fabs(Read<float>(entity + skyBrightness) - brightness) > 0.0001f) {
        Write<float>(entity + skyBrightness, brightness);
        changed = true;
    }
    if (force || Read<bool>(entity + skyEnabled) != enabled) {
        Write<bool>(entity + skyEnabled, enabled);
        changed = true;
    }
    if (changed) NotifySky(entity);
}

}

void SetDrifterPostProcessingSuppressed(bool suppressed) {
    if (!postProcessEnable)
        postProcessEnable = FindConVar("r_postprocess_enable");
    if (!postProcessEnable) return;

    if (suppressed) {
        if (!postProcessSuppressed) {
            oldPostProcessEnable = CVarBool(postProcessEnable);
            postProcessCaptured = true;
        }
        SetCVarBool(postProcessEnable, false);
        postProcessSuppressed = true;
    } else if (postProcessSuppressed) {
        if (postProcessCaptured)
            SetCVarBool(postProcessEnable, oldPostProcessEnable);
        postProcessSuppressed = false;
        postProcessCaptured = false;
    }
}

void RestoreWorldVisuals() {
    RestoreWorldRenderState();
    SetCVarBool(drawSkybox, oldDrawSkybox);
    SetCVarBool(draw3dSkybox, oldDraw3dSkybox);
    for (const auto& [entity, state] : skyStates) {
        if (GetEntityClassName(entity).find("EnvSky") == std::string::npos) continue;
        Write<ColorRGBA>(entity + skyTint, state.tint);
        Write<ColorRGBA>(entity + skyLightTint, state.lightTint);
        Write<float>(entity + skyBrightness, state.brightness);
        Write<bool>(entity + skyEnabled, state.enabled);
        NotifySky(entity);
    }
    skyStates.clear();
    worldStateApplied = false;
    appliedSkySettingsValid = false;
    nextEntityIndex = 0;
    nextScan = 0;
}

void UpdateWorldVisuals() {
    ResolveFields();
    if (!drawSkybox) drawSkybox = FindConVar("r_drawskybox");
    if (!draw3dSkybox) draw3dSkybox = FindConVar("r_draw3dskybox");
    if (!cvarsCaptured && (drawSkybox || draw3dSkybox)) {
        oldDrawSkybox = CVarBool(drawSkybox); oldDraw3dSkybox = CVarBool(draw3dSkybox); cvarsCaptured = true;
    }
    if (!worldModulationEnabled) {
        if (worldStateApplied) RestoreWorldVisuals();
        return;
    }
    worldStateApplied = true;
    const bool skyVisible = !disableSkybox;
    if (drawSkybox && CVarBool(drawSkybox) != skyVisible)
        SetCVarBool(drawSkybox, skyVisible);
    if (draw3dSkybox && CVarBool(draw3dSkybox) != skyVisible)
        SetCVarBool(draw3dSkybox, skyVisible);

    const bool blackSky = skyboxBrightness <= 0.0001f;
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    const ColorRGBA desiredSkyTint = ColorFrom(blackSky ? black : skyboxColor);
    const ColorRGBA desiredLightTint = ColorFrom(lightColor);
    const float desiredBrightness = blackSky
        ? 0.0001f : std::clamp(skyboxBrightness, 0.0001f, 50.f);
    const bool settingsChanged = !appliedSkySettingsValid ||
        !SameColor(appliedSkyTint, desiredSkyTint) ||
        !SameColor(appliedLightTint, desiredLightTint) ||
        std::fabs(appliedSkyBrightness - desiredBrightness) > 0.0001f ||
        appliedSkyEnabled != skyVisible;
    if (settingsChanged) {
        appliedSkyTint = desiredSkyTint;
        appliedLightTint = desiredLightTint;
        appliedSkyBrightness = desiredBrightness;
        appliedSkyEnabled = skyVisible;
        appliedSkySettingsValid = true;
        for (const auto& [entity, unused] : skyStates) {
            (void)unused;
            if (Readable(entity, sizeof(uintptr_t)))
                ApplySky(entity, desiredSkyTint, desiredLightTint,
                         desiredBrightness, skyVisible, true);
        }
    }

    // A full 4096-entity scan every 100 ms caused a visible CPU hitch. Spread
    // discovery across frames: one complete pass now takes roughly two
    // seconds at 120 FPS, while already discovered skies update immediately.
    const ULONGLONG now = GetTickCount64();
    if (now < nextScan) return;
    constexpr uint32_t ScanBudget = 32;
    bool completedScan = false;
    for (uint32_t scanned = 0; scanned < ScanBudget; ++scanned) {
        const uint32_t index = nextEntityIndex++;
        if (nextEntityIndex >= 4096) {
            nextEntityIndex = 0;
            completedScan = true;
        }
        const uintptr_t entity = ResolveEntityIndex(index);
        if (!entity) continue;
        const std::string name = GetEntityClassName(entity);
        if (name.find("EnvSky") != std::string::npos)
            ApplySky(entity, desiredSkyTint, desiredLightTint,
                     desiredBrightness, skyVisible);
    }
    // A completed discovery pass is enough for the current map. Recheck only
    // occasionally for reconnects or newly created sky entities.
    nextScan = now + (completedScan ? 5000 : 16);
}
