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
    const char* moduleName() const { return Read<const char*>(reinterpret_cast<uintptr_t>(this) + 0x10); }
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
    // CSchemaList is embedded in the type scope. It is not a pointer stored
    // at +0x5C0.
    uintptr_t classContainer() const {
        return reinterpret_cast<uintptr_t>(this) + 0x5C0;
    }
};

bool SafeStringEquals(uintptr_t address, const char* expected) {
    if (!address || !expected) return false;
    for (size_t i = 0; i < 256; ++i) {
        const char actual = Read<char>(address + i);
        if (actual != expected[i]) return false;
        if (actual == '\0') return true;
    }
    return false;
}

uintptr_t FindModulePattern(HMODULE module, const char* pattern, uintptr_t startAddress = 0) {
    if (!module) return 0;
    MODULEINFO info{};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return 0;
    std::vector<int> bytes;
    std::stringstream stream(pattern);
    std::string token;
    while (stream >> token) {
        bytes.push_back(token == "?" || token == "??"
            ? -1 : static_cast<int>(std::strtoul(token.c_str(), nullptr, 16)));
    }
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

uintptr_t ResolveRipRelativeAddress(uintptr_t instruction) {
    if (!instruction) return 0;
    const int32_t displacement = Read<int32_t>(instruction + 3);
    return instruction + 7 + displacement;
}

bool IsPlausibleEntitySystem(uintptr_t system) {
    if (!system) return false;
    const int highest = Read<int>(system + Offsets::HighestEntityIndex);
    if (highest <= 0 || highest > static_cast<int>(Offsets::HandleIndexMask)) return false;
    const uintptr_t firstChunk = Read<uintptr_t>(system + Offsets::EntityChunks);
    if (!firstChunk) return false;
    const uintptr_t identity = Read<uintptr_t>(firstChunk);
    const uint32_t handle = Read<uint32_t>(firstChunk + 0x10);
    return identity != 0 && (handle & Offsets::HandleIndexMask) == 0;
}

bool IsPlausibleCameraMatrices(uintptr_t matrixBase,
                               uintptr_t viewOffset,
                               uintptr_t projectionOffset) {
    if (!matrixBase) return false;
    const Matrix4x4 view = Read<Matrix4x4>(matrixBase + viewOffset);
    const Matrix4x4 projection = Read<Matrix4x4>(matrixBase + projectionOffset);
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            if (!std::isfinite(view.m[row][column]) ||
                !std::isfinite(projection.m[row][column]))
                return false;
        }
    }
    return std::fabs(view.m[3][3] - 1.0f) < 0.05f &&
           std::fabs(projection.m[0][0]) > 0.01f &&
           std::fabs(projection.m[0][0]) < 10.0f &&
           std::fabs(projection.m[1][1]) > 0.01f &&
           std::fabs(projection.m[1][1]) < 10.0f &&
           std::fabs(projection.m[3][2]) > 0.01f &&
           std::fabs(projection.m[3][3]) < 0.05f;
}

uintptr_t FindSchemaOffsetInBinding(SchemaBinding* binding, const char* className, const char* fieldName) {
    for (int depth = 0; binding && depth < 32; ++depth, binding = binding->base()) {
        const char* bindingName = binding->name();
        const char* moduleName = binding->moduleName();
        if (!SafeStringEquals(reinterpret_cast<uintptr_t>(bindingName), className) ||
            !SafeStringEquals(reinterpret_cast<uintptr_t>(moduleName), "client"))
            continue;
        const uint16_t count = binding->fieldCount();
        SchemaField* fields = binding->fields();
        if (!fields || count > 4096) return 0;
        for (uint16_t i = 0; i < count; ++i) {
            const uintptr_t field = reinterpret_cast<uintptr_t>(fields) +
                static_cast<uintptr_t>(i) * sizeof(SchemaField);
            const uintptr_t name = Read<uintptr_t>(field);
            if (SafeStringEquals(name, fieldName))
                return static_cast<uintptr_t>(Read<int>(field + 0x10));
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

    const uintptr_t scopePattern = FindModulePattern(
        schemaModule, "48 8B 05 ? ? ? ? 48 8B D6 0F B7 CB 48 8B 3C C8");
    if (!scopePattern) return 0;
    const int32_t displacement = Read<int32_t>(scopePattern + 3);
    const uintptr_t scopeArrayAddress = scopePattern + 7 + displacement;
    auto scopes = Read<SchemaTypeScope**>(scopeArrayAddress);
    const uint16_t scopeCount = Read<uint16_t>(scopeArrayAddress - 8);
    if (!scopes || scopeCount == 0 || scopeCount > 256) return 0;

    for (uint16_t scopeIndex = 0; scopeIndex < scopeCount; ++scopeIndex) {
        SchemaTypeScope* scope = scopes ? Read<SchemaTypeScope*>(reinterpret_cast<uintptr_t>(scopes) + scopeIndex * sizeof(uintptr_t)) : nullptr;
        if (!scope) continue;
        const uintptr_t container = scope->classContainer();
        // Current schemasystem stores a capacity/usage value here rather
        // than a plain class count. Client scopes can legitimately report
        // values above 0x10000 (for example 123330). The previous bound
        // therefore skipped the entire client scope and returned 0/25.
        const int blockCount = Read<int>(container - 0x10);
        if (blockCount <= 0 || blockCount > 1000000) continue;
        int visited = 0;
        for (int blockIndex = 0; blockIndex < 256; ++blockIndex) {
            // BlockContainer is three pointers wide; first block is +0x10.
            const uintptr_t blockContainer =
                container + static_cast<uintptr_t>(blockIndex) * 0x18;
            for (SchemaBlock* block = Read<SchemaBlock*>(blockContainer + 0x10);
                 block && visited < blockCount;
                 block = block->next(), ++visited) {
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
    loaded += SetRuntimeField("CGameSceneNode", "m_vRenderOrigin", Offsets::SceneNodeRenderOrigin);
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
    loaded += SetRuntimeField("C_BaseEntity", "m_iTeamNum", Offsets::Team);
    loaded += SetRuntimeField("C_CitadelPlayerPawn", "m_CCitadelAbilityComponent", Offsets::AbilityComponent);
    loaded += SetRuntimeField("CCitadelAbilityComponent", "m_vecAbilities", Offsets::AbilityVector);
    loaded += SetRuntimeField("C_CitadelPlayerPawn", "m_CCitadelHeroComponent", Offsets::HeroComponent);
    return loaded >= required;
}

}

bool runtimeOffsetsReady = false;
std::string runtimeBuildKey;
bool nativeGlowReady = false;

bool InitializePatternOffsets() {
    if (!clientBase) return false;

    // Andromeda resolves the global through a RIP-relative load instead of
    // hard-coding the address. Keep the same signature and convert the
    // resolved global address back to a client-relative offset because the
    // rest of DLL6 reads it as clientBase + Offsets::GameEntitySystem.
    constexpr char kGameEntitySystemPattern[] =
        "48 8B ? ? ? ? ? 8B D0 E8 ? ? ? ? 44 8B 83 ? ? ? ? 33 FF";
    const uintptr_t instruction = FindModulePattern(
        reinterpret_cast<HMODULE>(clientBase), kGameEntitySystemPattern);
    if (!instruction) {
        printf("[!] GameEntitySystem pattern not found\n");
        return false;
    }

    const uintptr_t globalAddress = ResolveRipRelativeAddress(instruction);
    MODULEINFO moduleInfo{};
    if (!GetModuleInformation(GetCurrentProcess(),
                              reinterpret_cast<HMODULE>(clientBase),
                              &moduleInfo, sizeof(moduleInfo))) {
        return false;
    }
    const uintptr_t clientEnd = clientBase + moduleInfo.SizeOfImage;
    if (globalAddress < clientBase || globalAddress + sizeof(uintptr_t) > clientEnd) {
        printf("[!] GameEntitySystem global is outside client.dll: %p\n",
               reinterpret_cast<void*>(globalAddress));
        return false;
    }

    const uintptr_t system = Read<uintptr_t>(globalAddress);
    if (!IsPlausibleEntitySystem(system)) {
        printf("[!] Resolved GameEntitySystem is invalid: global=%p value=%p\n",
               reinterpret_cast<void*>(globalAddress),
               reinterpret_cast<void*>(system));
        return false;
    }

    Offsets::GameEntitySystem = globalAddress - clientBase;
    // Resolve the camera data anchor from the current client too. The old
    // ViewMatrix RVA is not stable between game updates; an invalid matrix
    // makes GetPlayers() return an empty snapshot for every visual feature.
    constexpr char kCameraOriginWriterPattern[] =
        "F2 0F 11 05 ? ? ? ? 41 8B 46 08 89 05 ? ? ? ? F2 0F 10 45 00";
    const uintptr_t cameraInstruction = FindModulePattern(
        reinterpret_cast<HMODULE>(clientBase), kCameraOriginWriterPattern);
    uintptr_t cameraOrigin = 0;
    if (cameraInstruction) {
        const int32_t xyDisplacement = Read<int32_t>(cameraInstruction + 4);
        const uintptr_t zInstruction = cameraInstruction + 12;
        const int32_t zDisplacement = Read<int32_t>(zInstruction + 2);
        const uintptr_t xyTarget = cameraInstruction + 8 + xyDisplacement;
        const uintptr_t zTarget = zInstruction + 6 + zDisplacement;
        if (xyTarget >= clientBase + 0xC0 && xyTarget + 16 <= clientEnd &&
            zTarget == xyTarget + 8) {
            // The camera object layout changed with the client update. The
            // writer gives us the origin address; validate the known layouts
            // against the live matrices before selecting the new base.
            constexpr uintptr_t layouts[][3] = {
                {0xC0, 0x00, 0x40}, // current client layout
                {0x28, 0x80, 0xC0}, // origin, view, projection
            };
            for (const auto& layout : layouts) {
                const uintptr_t matrixBase = xyTarget - layout[0];
                if (!IsPlausibleCameraMatrices(matrixBase, layout[1], layout[2]))
                    continue;
                cameraOrigin = xyTarget;
                Offsets::ViewMatrix = matrixBase - clientBase;
                Offsets::CameraOrigin = layout[0];
                Offsets::ViewMatrixView = layout[1];
                Offsets::ViewMatrixProjection = layout[2];
                break;
            }
        }
    }
    std::ofstream log(
        "C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\pattern_offsets.log",
        std::ios::trunc);
    if (log) {
        log << std::hex
            << "clientBase=0x" << clientBase << '\n'
            << "instruction=0x" << instruction << '\n'
            << "globalAddress=0x" << globalAddress << '\n'
            << "GameEntitySystem=0x" << Offsets::GameEntitySystem << '\n'
            << "system=0x" << system << '\n'
            << "HighestEntityIndex=0x" << Offsets::HighestEntityIndex << '\n'
            << "cameraInstruction=0x" << cameraInstruction << '\n'
            << "cameraOrigin=0x" << cameraOrigin << '\n'
            << "ViewMatrix=0x" << Offsets::ViewMatrix << '\n'
            << "ViewMatrixView=0x" << Offsets::ViewMatrixView << '\n'
            << "ViewMatrixProjection=0x" << Offsets::ViewMatrixProjection << '\n'
            << "CameraOrigin=0x" << Offsets::CameraOrigin << '\n';
    }
    printf("[+] GameEntitySystem: global=%p offset=0x%llX system=%p\n",
           reinterpret_cast<void*>(globalAddress),
           static_cast<unsigned long long>(Offsets::GameEntitySystem),
           reinterpret_cast<void*>(system));
    printf("[+] ViewMatrix: %s offset=0x%llX\n",
           cameraOrigin ? "pattern" : "fallback",
           static_cast<unsigned long long>(Offsets::ViewMatrix));
    return true;
}

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
    char clientPath[MAX_PATH]{};
    const DWORD clientLength = clientBase
        ? GetModuleFileNameA(reinterpret_cast<HMODULE>(clientBase),
                             clientPath, sizeof(clientPath))
        : 0;
    std::vector<std::filesystem::path> candidates;
    const char* configured = std::getenv("DLL6_SCHEMA_DIR");
    if (configured && *configured) candidates.emplace_back(configured);
    candidates.emplace_back("D:\\Downloads\\schema-dump\\deadlock");
    if (length) {
        const auto releaseDir = std::filesystem::path(modulePath).parent_path();
        candidates.emplace_back(releaseDir / "schema-dump" / "deadlock");
    }
    for (const auto& candidate : candidates) {
        const auto dump = candidate / "client.txt";
        if (!std::filesystem::exists(dump)) continue;
        if (clientLength) {
            std::error_code error;
            const auto dumpTime = std::filesystem::last_write_time(dump, error);
            if (error) continue;
            const auto clientTime =
                std::filesystem::last_write_time(clientPath, error);
            if (error || dumpTime < clientTime) continue;
        }
        return candidate;
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
        << "source=" << (runtimeOffsetsReady ? "live-schema-or-validated-fallback" : "static-fallback") << "\n"
        << std::hex
        << "ControllerPawn=0x" << Offsets::ControllerPawn << "\n"
        << "PawnController=0x" << Offsets::PawnController << "\n"
        << "GameSceneNode=0x" << Offsets::GameSceneNode << "\n"
        << "SceneNodeAbsOrigin=0x" << Offsets::SceneNodeAbsOrigin << "\n"
        << "Glow=0x" << Offsets::Glow << "\n"
        << "Health=0x" << Offsets::Health << "\n"
        << "MaxHealth=0x" << Offsets::MaxHealth << "\n"
        << "LifeState=0x" << Offsets::LifeState << "\n"
        << "Team=0x" << Offsets::Team << "\n"
        << "HeroComponent=0x" << Offsets::HeroComponent << "\n"
        << "AbilityComponent=0x" << Offsets::AbilityComponent << "\n";
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
    // GameEntitySystem is resolved and validated by InitializePatternOffsets.
    // Never replace it with a possibly stale value from an on-disk dump.

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
