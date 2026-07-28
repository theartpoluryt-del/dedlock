#include "shared.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <cctype>
#include <cstdlib>

namespace {

using CreateInterfaceFn = void* (__cdecl*)(const char*, int*);

struct SchemaField {
    const char* name;
    void* type;
    int offset;
    uint8_t padding[12];
};

struct SchemaBinding {
    const char* name() const { return Read<const char*>(reinterpret_cast<uintptr_t>(this) + 0x08); }
    uint16_t fieldCount() const { return Read<uint16_t>(reinterpret_cast<uintptr_t>(this) + 0x1C); }
    SchemaField* fields() const { return Read<SchemaField*>(reinterpret_cast<uintptr_t>(this) + 0x28); }
    SchemaBinding* base() const {
        const uintptr_t baseInfo = Read<uintptr_t>(reinterpret_cast<uintptr_t>(this) + 0x30);
        return baseInfo ? Read<SchemaBinding*>(baseInfo + 0x08) : nullptr;
    }
};

struct SchemaBlock {
    SchemaBlock* next() const { return Read<SchemaBlock*>(reinterpret_cast<uintptr_t>(this) + 0x08); }
    SchemaBinding* binding() const { return Read<SchemaBinding*>(reinterpret_cast<uintptr_t>(this) + 0x10); }
};

struct SchemaTypeScope {
    uintptr_t classContainer() const { return Read<uintptr_t>(reinterpret_cast<uintptr_t>(this) + 0x5C0); }
};

uintptr_t FindModulePattern(HMODULE module, const char* pattern, uintptr_t startAddress = 0) {
    if (!module) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token) bytes.push_back(token == "?" ? -1 : std::strtoul(token.c_str(), nullptr, 16));
    if (bytes.empty()) return 0;
    const auto* image = reinterpret_cast<const uint8_t*>(module);
    size_t start = 0;
    if (startAddress > reinterpret_cast<uintptr_t>(module))
        start = static_cast<size_t>(startAddress - reinterpret_cast<uintptr_t>(module));
    for (size_t i = start; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && image[i + j] != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (match) return reinterpret_cast<uintptr_t>(module) + i;
    }
    return 0;
}

uintptr_t FindSchemaOffsetInBinding(SchemaBinding* binding, const char* className, const char* fieldName) {
    for (int depth = 0; binding && depth < 32; ++depth, binding = binding->base()) {
        const char* bindingName = binding->name();
        if (!bindingName || std::strcmp(bindingName, className) != 0) continue;
        const uint16_t count = binding->fieldCount();
        SchemaField* fields = binding->fields();
        if (!fields || count > 4096) return 0;
        for (uint16_t i = 0; i < count; ++i) {
            if (fields[i].name && std::strcmp(fields[i].name, fieldName) == 0)
                return static_cast<uintptr_t>(fields[i].offset);
        }
    }
    return 0;
}

uintptr_t FindSchemaOffset(const char* className, const char* fieldName) {
    HMODULE schemaModule = GetModuleHandleA("schemasystem.dll");
    if (!schemaModule) return 0;
    auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(schemaModule, "CreateInterface"));
    if (!factory) return 0;
    void* schemaSystem = factory("SchemaSystem_001", nullptr);
    if (!schemaSystem) return 0;

    using GlobalScopeFn = SchemaTypeScope* (__fastcall*)(void*);
    auto vtable = *reinterpret_cast<uintptr_t**>(schemaSystem);
    if (!vtable || !vtable[11]) return 0;
    auto globalScope = reinterpret_cast<GlobalScopeFn>(vtable[11])(schemaSystem);

    const uintptr_t scopePattern = FindModulePattern(
        schemaModule, "48 8B 05 ? ? ? ? 48 8B D6 0F B7 CB 48 8B 3C C8");
    if (!scopePattern) return 0;
    const int32_t displacement = Read<int32_t>(scopePattern + 3);
    const uintptr_t scopeArrayAddress = scopePattern + 7 + displacement;
    auto scopes = Read<SchemaTypeScope**>(scopeArrayAddress);
    const uint16_t scopeCount = Read<uint16_t>(scopePattern - 8);
    (void)globalScope;

    for (uint16_t scopeIndex = 0; scopeIndex < scopeCount && scopeIndex < 256; ++scopeIndex) {
        SchemaTypeScope* scope = scopes ? Read<SchemaTypeScope*>(reinterpret_cast<uintptr_t>(scopes) + scopeIndex * sizeof(uintptr_t)) : nullptr;
        if (!scope) continue;
        const uintptr_t container = scope->classContainer();
        const int blockCount = Read<int>(container + 0x74);
        if (blockCount <= 0 || blockCount > 65536) continue;
        for (int blockIndex = 0; blockIndex < 256; ++blockIndex) {
            const uintptr_t blockContainer = container + static_cast<uintptr_t>(blockIndex) * 0x10;
            for (SchemaBlock* block = Read<SchemaBlock*>(blockContainer + 0x10); block; block = block->next()) {
                if (const uintptr_t offset = FindSchemaOffsetInBinding(block->binding(), className, fieldName))
                    return offset;
            }
        }
    }
    return 0;
}

bool SetRuntimeField(const char* className, const char* fieldName, uintptr_t& destination) {
    const uintptr_t value = FindSchemaOffset(className, fieldName);
    if (!value) return false;
    destination = value;
    return true;
}

bool InitializeLiveSchemaOffsets(size_t& loaded, size_t required) {
    loaded = 0;
    loaded += SetRuntimeField("CBasePlayerController", "m_hPawn", Offsets::ControllerPawn);
    loaded += SetRuntimeField("CBasePlayerController", "m_bIsLocalPlayerController", Offsets::IsLocalPlayerController);
    loaded += SetRuntimeField("C_BasePlayerPawn", "m_hController", Offsets::PawnController);
    loaded += SetRuntimeField("C_BaseEntity", "m_pGameSceneNode", Offsets::GameSceneNode);
    loaded += SetRuntimeField("CGameSceneNode", "m_vecAbsOrigin", Offsets::SceneNodeAbsOrigin);
    loaded += SetRuntimeField("CGameSceneNode", "m_flScale", Offsets::SceneNodeAbsScale);
    loaded += SetRuntimeField("CGameSceneNode", "m_bDormant", Offsets::SceneNodeDormant);
    loaded += SetRuntimeField("C_BaseModelEntity", "m_Collision", Offsets::OrbCollisionProperty);
    loaded += SetRuntimeField("CCollisionProperty", "m_vecMins", Offsets::CollisionMins);
    loaded += SetRuntimeField("CCollisionProperty", "m_vecMaxs", Offsets::CollisionMaxs);
    loaded += SetRuntimeField("C_BaseModelEntity", "m_Glow", Offsets::Glow);
    loaded += SetRuntimeField("CGlowProperty", "m_fGlowColor", Offsets::GlowColor);
    loaded += SetRuntimeField("CGlowProperty", "m_iGlowType", Offsets::GlowType);
    loaded += SetRuntimeField("CGlowProperty", "m_iGlowTeam", Offsets::GlowTeam);
    loaded += SetRuntimeField("CGlowProperty", "m_nGlowRange", Offsets::GlowRange);
    loaded += SetRuntimeField("CGlowProperty", "m_nGlowRangeMin", Offsets::GlowRangeMin);
    loaded += SetRuntimeField("CGlowProperty", "m_glowColorOverride", Offsets::GlowColorOverride);
    loaded += SetRuntimeField("CGlowProperty", "m_bFlashing", Offsets::GlowFlashing);
    loaded += SetRuntimeField("CGlowProperty", "m_flGlowTime", Offsets::GlowTime);
    loaded += SetRuntimeField("CGlowProperty", "m_flGlowStartTime", Offsets::GlowStartTime);
    loaded += SetRuntimeField("CGlowProperty", "m_bGlowing", Offsets::IsGlowing);
    loaded += SetRuntimeField("C_BaseModelEntity", "m_flGlowBackfaceMult", Offsets::GlowBackfaceMult);
    loaded += SetRuntimeField("C_BaseEntity", "m_iMaxHealth", Offsets::MaxHealth);
    loaded += SetRuntimeField("C_BaseEntity", "m_iHealth", Offsets::Health);
    loaded += SetRuntimeField("C_BaseEntity", "m_lifeState", Offsets::LifeState);
    return loaded >= required;
}

}

bool runtimeOffsetsReady = false;
std::string runtimeBuildKey;
bool nativeGlowReady = false;

namespace {

using OffsetMap = std::unordered_map<std::string, uintptr_t>;

bool ReadOffsetLine(const std::string& line, std::string& key, uintptr_t& value) {
    const size_t equals = line.find(" = 0x");
    if (equals == std::string::npos) return false;
    const size_t valueStart = equals + 5;
    size_t valueEnd = valueStart;
    while (valueEnd < line.size() && std::isxdigit(static_cast<unsigned char>(line[valueEnd]))) ++valueEnd;
    if (valueEnd == valueStart) return false;
    key = line.substr(0, equals);
    value = std::strtoull(line.substr(valueStart, valueEnd - valueStart).c_str(), nullptr, 16);
    return value != 0;
}

void ReadDump(const std::filesystem::path& path, OffsetMap& result) {
    std::ifstream file(path);
    if (!file) return;
    std::string line, key;
    uintptr_t value = 0;
    while (std::getline(file, line)) {
        if (ReadOffsetLine(line, key, value)) result[key] = value;
    }
}

bool SetField(const OffsetMap& fields, const char* name, uintptr_t& destination) {
    const auto it = fields.find(name);
    if (it == fields.end()) return false;
    destination = it->second;
    return true;
}

std::filesystem::path FindSchemaDir() {
    char modulePath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(moduleHandle, modulePath, sizeof(modulePath));
    std::vector<std::filesystem::path> candidates;
    const char* configured = std::getenv("DLL6_SCHEMA_DIR");
    if (configured && *configured) candidates.emplace_back(configured);
    candidates.emplace_back("D:\\Downloads\\schema-dump\\deadlock");
    if (length) {
        const auto releaseDir = std::filesystem::path(modulePath).parent_path();
        candidates.emplace_back(releaseDir / "schema-dump" / "deadlock");
    }
    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "client.txt")) return candidate;
    }
    return {};
}

std::string GetClientFingerprint() {
    MODULEINFO info{};
    if (!clientBase || !GetModuleInformation(GetCurrentProcess(), reinterpret_cast<HMODULE>(clientBase), &info, sizeof(info)))
        return "unknown";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(clientBase);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(clientBase + dos->e_lfanew);
    std::ostringstream out;
    out << std::hex << "size=" << info.SizeOfImage << ";timestamp=" << nt->FileHeader.TimeDateStamp;
    return out.str();
}

void WriteRuntimeLog(const std::filesystem::path& schemaDir, size_t loaded, size_t required) {
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\runtime_offsets.log", std::ios::trunc);
    if (!log) return;
    log << "client=" << runtimeBuildKey << "\n"
        << "schema=" << (schemaDir.empty() ? "not-found" : schemaDir.string()) << "\n"
        << "loaded=" << loaded << "/" << required << "\n"
        << "source=schema-dump; fixed addresses are retained when no schema value exists\n";
}
}

bool InitializeRuntimeOffsets() {
    runtimeBuildKey = GetClientFingerprint();
    const auto schemaDir = FindSchemaDir();
    OffsetMap fields, globals;
    if (!schemaDir.empty()) {
        ReadDump(schemaDir / "client.txt", fields);
        ReadDump(schemaDir / "_globals.txt", globals);
    }

    const size_t required = 25;
    size_t loaded = 0;
    SetField(globals, "client.dll::CGameEntitySystem", Offsets::GameEntitySystem);

    // Prefer the live SchemaSystem. It is generated by the running client and
    // therefore follows class layout changes without a new dump file.
    const bool liveSchemaReady = InitializeLiveSchemaOffsets(loaded, required - 1);
    if (!liveSchemaReady) {
        loaded = 0;
        loaded += SetField(fields, "CBasePlayerController.m_hPawn", Offsets::ControllerPawn);
        loaded += SetField(fields, "CBasePlayerController.m_bIsLocalPlayerController", Offsets::IsLocalPlayerController);
        loaded += SetField(fields, "C_BasePlayerPawn.m_hController", Offsets::PawnController);
        loaded += SetField(fields, "C_BaseEntity.m_pGameSceneNode", Offsets::GameSceneNode);
        loaded += SetField(fields, "CGameSceneNode.m_vecAbsOrigin", Offsets::SceneNodeAbsOrigin);
        loaded += SetField(fields, "CGameSceneNode.m_flScale", Offsets::SceneNodeAbsScale);
        loaded += SetField(fields, "CGameSceneNode.m_bDormant", Offsets::SceneNodeDormant);
        loaded += SetField(fields, "C_BaseModelEntity.m_Collision", Offsets::OrbCollisionProperty);
        loaded += SetField(fields, "CCollisionProperty.m_vecMins", Offsets::CollisionMins);
        loaded += SetField(fields, "CCollisionProperty.m_vecMaxs", Offsets::CollisionMaxs);
        loaded += SetField(fields, "C_BaseModelEntity.m_Glow", Offsets::Glow);
        loaded += SetField(fields, "CGlowProperty.m_fGlowColor", Offsets::GlowColor);
        loaded += SetField(fields, "CGlowProperty.m_iGlowType", Offsets::GlowType);
        loaded += SetField(fields, "CGlowProperty.m_iGlowTeam", Offsets::GlowTeam);
        loaded += SetField(fields, "CGlowProperty.m_nGlowRange", Offsets::GlowRange);
        loaded += SetField(fields, "CGlowProperty.m_nGlowRangeMin", Offsets::GlowRangeMin);
        loaded += SetField(fields, "CGlowProperty.m_glowColorOverride", Offsets::GlowColorOverride);
        loaded += SetField(fields, "CGlowProperty.m_bFlashing", Offsets::GlowFlashing);
        loaded += SetField(fields, "CGlowProperty.m_flGlowTime", Offsets::GlowTime);
        loaded += SetField(fields, "CGlowProperty.m_flGlowStartTime", Offsets::GlowStartTime);
        loaded += SetField(fields, "CGlowProperty.m_bGlowing", Offsets::IsGlowing);
        loaded += SetField(fields, "C_BaseModelEntity.m_flGlowBackfaceMult", Offsets::GlowBackfaceMult);
        loaded += SetField(fields, "C_BaseEntity.m_iMaxHealth", Offsets::MaxHealth);
        loaded += SetField(fields, "C_BaseEntity.m_iHealth", Offsets::Health);
        loaded += SetField(fields, "C_BaseEntity.m_lifeState", Offsets::LifeState);
    }

    runtimeOffsetsReady = liveSchemaReady || loaded >= 20;
    WriteRuntimeLog(schemaDir, loaded, required);
    printf("[+] Runtime offsets: %s (%zu/%zu, %s)\n", liveSchemaReady ? "live schema" : (runtimeOffsetsReady ? "dump fallback" : "static fallback"), loaded, required, runtimeBuildKey.c_str());
    return runtimeOffsetsReady;
}

namespace {
using NativeGlowRegisterFn = void(__fastcall*)(uintptr_t);
NativeGlowRegisterFn nativeGlowRegister = nullptr;

uintptr_t FindNativeGlowWrapper() {
    const char* pattern =
        "48 89 5C 24 18 57 48 83 EC 30 48 8B F9 E8 ? ? ? ? "
        "48 8B 07 4C 8D 44 24 48 48 8D 54 24 40 C7 44 24 40 00 00 00 00 "
        "48 8B CF FF 90 60 09 00 00 8B D8 E8 ? ? ? ? F3 0F 10 44 24 48 "
        "4C 8D 4C 24 40 44 8B C3 F3 0F 11 44 24 20 48 8B D7 48 8B C8 E8 ? ? ? ?";
    const uintptr_t first = FindModulePattern(reinterpret_cast<HMODULE>(clientBase), pattern);
    if (!first) return 0;
    // The first match handles another glowable entity type. The following
    // identical wrapper is the player-pawn path in the current client.
    return FindModulePattern(reinterpret_cast<HMODULE>(clientBase), pattern, first + 1);
}
}

bool InitializeNativeGlow() {
    const uintptr_t wrapper = FindNativeGlowWrapper();
    if (!wrapper) {
        nativeGlowReady = false;
        return false;
    }
    nativeGlowRegister = reinterpret_cast<NativeGlowRegisterFn>(wrapper);
    nativeGlowReady = true;
    printf("[+] Native glow wrapper: %p\n", reinterpret_cast<void*>(wrapper));
    return true;
}

bool RegisterNativeGlow(uintptr_t entity) {
    if (!nativeGlowReady || !nativeGlowRegister || !entity) return false;
    __try {
        nativeGlowRegister(entity);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nativeGlowReady = false;
        nativeGlowRegister = nullptr;
        printf("[!] Native glow disabled after exception\n");
        return false;
    }
}
