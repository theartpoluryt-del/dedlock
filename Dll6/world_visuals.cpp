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
EnvSkyUpdateFn updateSky{};
ULONGLONG nextScan{};
uintptr_t skyTint{}, skyLightTint{}, skyBrightness{}, skyEnabled{};
bool resolved{};
bool worldStateApplied{};

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

void ApplySky(uintptr_t entity) {
    if (!skyTint || !skyLightTint || !skyBrightness || !skyEnabled) return;
    auto [it, fresh] = skyStates.try_emplace(entity);
    if (fresh) it->second = {Read<ColorRGBA>(entity + skyTint), Read<ColorRGBA>(entity + skyLightTint),
                             Read<float>(entity + skyBrightness), Read<bool>(entity + skyEnabled)};
    const bool blackSky = skyboxBrightness <= 0.0001f;
    const float black[4] = {0.f, 0.f, 0.f, 1.f};
    Write<ColorRGBA>(entity + skyTint,
                     ColorFrom(blackSky ? black : skyboxColor));
    Write<ColorRGBA>(entity + skyLightTint, ColorFrom(lightColor));
    // Source 2 treats an exact zero brightness scale as "not supplied" and
    // falls back to the material's default exposure.  A black tint plus a
    // tiny positive scale produces an actually black sky at the slider's 0.
    Write<float>(entity + skyBrightness,
                 blackSky ? 0.0001f
                          : std::clamp(skyboxBrightness, 0.0001f, 50.f));
    Write<bool>(entity + skyEnabled, !disableSkybox);
    NotifySky(entity);
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
    SetCVarBool(drawSkybox, !disableSkybox); SetCVarBool(draw3dSkybox, !disableSkybox);
    const ULONGLONG now = GetTickCount64();
    if (now < nextScan) return;
    nextScan = now + 100;
    for (uint32_t index = 0; index < 4096; ++index) {
        const uintptr_t entity = ResolveEntityIndex(index);
        if (!entity) continue;
        const std::string name = GetEntityClassName(entity);
        if (name.find("EnvSky") != std::string::npos) ApplySky(entity);
    }
}
