#include "shared.h"
#include "portable_paths.h"

#include <MinHook.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <atomic>

// The protobuf type and its generated implementation are kept in the work
// tree supplied with this project. They are the same CMsgSosStartSoundEvent
// used by the original ParseMessage hook.
#include "protobuf/gameevents.pb.h"

namespace {

constexpr uint16_t kSosStartSoundEvent = 208;
constexpr uint16_t kCitadelDamageMessage = 300;
constexpr uintptr_t kParseMessageProtobufOffset = 0x30;
constexpr char kParseMessagePattern[] = "40 56 57 41 57 48 83 EC ? 4C 8B F9";

using ParseMessageFn = bool(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
ParseMessageFn originalParseMessage = nullptr;
void* parseMessageTarget = nullptr;
bool soundHookInstalled = false;
volatile LONG soundParsingDisabled = 0;
volatile LONG damageParsingDisabled = 0;
volatile LONG networkDumpDisabled = 0;
std::mutex soundLogMutex;
std::mutex networkDumpMutex;
std::mutex soundCacheMutex;
std::unordered_map<uint32_t, bool> meleeSoundHashCache;
struct NetworkMessageStats {
    uint64_t count = 0;
    uint64_t lastPayloadHash = 0;
    uint32_t loggedPayloads = 0;
};
std::unordered_map<std::string, NetworkMessageStats> networkMessageStats;
uint64_t networkDumpLines = 0;
constexpr char kMeleeChargeSound[] = "Player.Melee.Hold.Shared";
constexpr uint64_t kMaxNetworkDumpLines = 50000;
constexpr uint32_t kMaxPayloadsPerType = 2000;
constexpr size_t kMaxNetworkPayloadChars = 8192;
bool IsReadable(uintptr_t address, size_t size = sizeof(uintptr_t));
std::atomic<uint64_t> packetEntitiesSequence{0};
std::atomic<int32_t> latestServerTick{-1};
std::atomic<bool> packetEntitiesSchemaLogged{false};

struct PacketEntitiesMetadata {
    bool valid = false;
    int32_t serverTick = -1;
    int32_t deltaFrom = -1;
    int32_t updatedEntries = -1;
    uint32_t entityDataBytes = 0;
    uint64_t entityDataHash = 0;
};

uint64_t HashBytes(const std::string& value) {
    // FNV-1a is deterministic across processes, unlike std::hash.
    uint64_t hash = 1469598103934665603ull;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool ReadIntegralField(const google::protobuf::Message& message,
                       const google::protobuf::FieldDescriptor* field,
                       int64_t& value) {
    if (!field || field->is_repeated()) return false;
    const auto* reflection = message.GetReflection();
    if (!reflection || !reflection->HasField(message, field)) return false;
    using CppType = google::protobuf::FieldDescriptor::CppType;
    switch (field->cpp_type()) {
        case CppType::CPPTYPE_INT32:
            value = reflection->GetInt32(message, field); return true;
        case CppType::CPPTYPE_UINT32:
            value = reflection->GetUInt32(message, field); return true;
        case CppType::CPPTYPE_INT64:
            value = reflection->GetInt64(message, field); return true;
        case CppType::CPPTYPE_UINT64:
            value = static_cast<int64_t>(reflection->GetUInt64(message, field));
            return true;
        case CppType::CPPTYPE_BOOL:
            value = reflection->GetBool(message, field) ? 1 : 0; return true;
        case CppType::CPPTYPE_ENUM:
            value = reflection->GetEnumValue(message, field); return true;
        default:
            return false;
    }
}

bool ReadNamedIntegralField(const google::protobuf::Message& message,
                            const char* name, int64_t& value) {
    const auto* descriptor = message.GetDescriptor();
    return descriptor && ReadIntegralField(
        message, descriptor->FindFieldByName(name), value);
}

void ObserveNetworkTimingMessageUnsafe(uintptr_t netMessage) {
    auto* message = reinterpret_cast<google::protobuf::Message*>(
        netMessage + kParseMessageProtobufOffset);
    const auto* descriptor = message->GetDescriptor();
    if (!descriptor || descriptor->name() != "CNETMsg_Tick") return;
    int64_t tick = -1;
    if (ReadNamedIntegralField(*message, "tick", tick))
        latestServerTick.store(static_cast<int32_t>(tick),
                               std::memory_order_release);
}

PacketEntitiesMetadata ReadPacketEntitiesMetadataUnsafe(uintptr_t netMessage) {
    PacketEntitiesMetadata metadata{};
    auto* message = reinterpret_cast<google::protobuf::Message*>(
        netMessage + kParseMessageProtobufOffset);
    const auto* descriptor = message->GetDescriptor();
    const auto* reflection = message->GetReflection();
    if (!descriptor || !reflection ||
        descriptor->name() != "CSVCMsg_PacketEntities") return metadata;

    metadata.valid = true;
    if (!packetEntitiesSchemaLogged.exchange(true,
                                              std::memory_order_acq_rel)) {
        std::ofstream schema(Dll6Paths::DataFileA(
            "lockify_packet_entities_schema.log"), std::ios::trunc);
        if (schema) {
            schema << "message=" << descriptor->full_name() << '\n';
            for (int index = 0; index < descriptor->field_count(); ++index) {
                const auto* field = descriptor->field(index);
                if (!field) continue;
                schema << field->number() << ',' << field->name() << ','
                       << field->type_name() << ',' << field->cpp_type_name()
                       << ",repeated=" << field->is_repeated() << '\n';
            }
        }
    }
    metadata.serverTick = latestServerTick.load(std::memory_order_acquire);
    int64_t value = 0;
    if (ReadNamedIntegralField(*message, "server_tick", value))
        metadata.serverTick = static_cast<int32_t>(value);
    if (ReadNamedIntegralField(*message, "delta_from", value))
        metadata.deltaFrom = static_cast<int32_t>(value);
    if (ReadNamedIntegralField(*message, "updated_entries", value))
        metadata.updatedEntries = static_cast<int32_t>(value);

    const auto* entityData = descriptor->FindFieldByName("entity_data");
    if (entityData && !entityData->is_repeated() &&
        entityData->cpp_type() ==
            google::protobuf::FieldDescriptor::CPPTYPE_STRING &&
        reflection->HasField(*message, entityData)) {
        std::string scratch;
        const std::string& bytes = reflection->GetStringReference(
            *message, entityData, &scratch);
        metadata.entityDataBytes = static_cast<uint32_t>(
            (std::min)(bytes.size(), static_cast<size_t>(UINT32_MAX)));
        metadata.entityDataHash = HashBytes(bytes);
    }
    return metadata;
}

PacketEntitiesMetadata TryReadPacketEntitiesMetadata(uintptr_t serializer,
                                                     uintptr_t netMessage) {
    PacketEntitiesMetadata metadata{};
    if (!movementProbeEnabled || !IsReadable(serializer, 0x2A) ||
        !IsReadable(netMessage + kParseMessageProtobufOffset)) return metadata;
    __try {
        const uint16_t type = Read<uint16_t>(serializer + 0x28);
        if (type == 4)
            ObserveNetworkTimingMessageUnsafe(netMessage);
        else if (type == 55)
            metadata = ReadPacketEntitiesMetadataUnsafe(netMessage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        metadata = {};
    }
    return metadata;
}

bool FindCachedMeleeSound(uint32_t hash, bool& isMeleeCharge) {
    std::lock_guard<std::mutex> lock(soundCacheMutex);
    const auto cached = meleeSoundHashCache.find(hash);
    if (cached == meleeSoundHashCache.end()) return false;
    isMeleeCharge = cached->second;
    return true;
}

void CacheMeleeSound(uint32_t hash, bool isMeleeCharge) {
    std::lock_guard<std::mutex> lock(soundCacheMutex);
    if (meleeSoundHashCache.size() < 4096)
        meleeSoundHashCache.emplace(hash, isMeleeCharge);
}

void LogSound(const char* message) {
    std::lock_guard<std::mutex> lock(soundLogMutex);
    std::ofstream log(Dll6Paths::DataFileA("sound_hook.log"), std::ios::app);
    if (log) log << message << '\n';
}

void LogSoundEvent(int sourceEntity, uint32_t hash, const char* soundName) {
    (void)sourceEntity;
    (void)hash;
    (void)soundName;
}

bool IsReadable(uintptr_t address, size_t size) {
    if (!address || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t end = address + size;
    const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= address && end <= regionEnd;
}

bool IsExecutable(uintptr_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi))) return false;
    const DWORD protection = mbi.Protect & 0xFF;
    return mbi.State == MEM_COMMIT &&
        (protection == PAGE_EXECUTE || protection == PAGE_EXECUTE_READ ||
         protection == PAGE_EXECUTE_READWRITE || protection == PAGE_EXECUTE_WRITECOPY);
}

std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> bytes;
    std::istringstream stream(pattern ? pattern : "");
    std::string token;
    while (stream >> token)
        bytes.push_back(token == "?" || token == "??" ? -1 : std::strtol(token.c_str(), nullptr, 16));
    return bytes;
}

uintptr_t FindPattern(HMODULE module, const char* pattern) {
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
        return 0;

    const auto bytes = ParsePattern(pattern);
    const auto* base = static_cast<const uint8_t*>(info.lpBaseOfDll);
    uintptr_t found = 0;
    for (size_t i = 0; i + bytes.size() <= info.SizeOfImage; ++i) {
        bool match = true;
        for (size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && base[i + j] != static_cast<uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (found) return 0;
        found = reinterpret_cast<uintptr_t>(base + i);
    }
    return found;
}

class CSoundEventManager {
public:
    const char* GetSoundEventName(uint32_t hash) {
        if (!IsReadable(reinterpret_cast<uintptr_t>(this), sizeof(void*)) ||
            !IsReadable(Read<uintptr_t>(reinterpret_cast<uintptr_t>(this)), sizeof(void*) * 3))
            return nullptr;
        using Fn = const char*(__fastcall*)(CSoundEventManager*, uint32_t);
        const auto fn = reinterpret_cast<Fn>(Read<uintptr_t>(Read<uintptr_t>(reinterpret_cast<uintptr_t>(this)) + sizeof(void*) * 2));
        if (!IsExecutable(reinterpret_cast<uintptr_t>(fn))) return nullptr;
        return fn ? fn(this, hash) : nullptr;
    }
};

class CSoundOpSystem {
public:
    CSoundEventManager* GetSoundEventManager() {
        // CUSTOM_OFFSET_RAW from the reference project returns an object at
        // this + offset; it does not dereference a pointer stored there.
        return reinterpret_cast<CSoundEventManager*>(reinterpret_cast<uintptr_t>(this) + 0x8);
    }
};

CSoundEventManager* GetSoundEventManager() {
    static CSoundEventManager* cached = nullptr;
    if (cached && IsReadable(reinterpret_cast<uintptr_t>(cached))) return cached;
    auto* module = GetModuleHandleA("soundsystem.dll");
    if (!module) return nullptr;
    using CreateInterfaceFn = void*(*)(const char*, int*);
    const auto factory = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
    if (!factory) return nullptr;
    int returnCode = 0;
    auto* system = static_cast<CSoundOpSystem*>(factory("SoundOpSystem001", &returnCode));
    if (!system || !IsReadable(reinterpret_cast<uintptr_t>(system), 0x10)) return nullptr;
    cached = system->GetSoundEventManager();
    return cached && IsReadable(reinterpret_cast<uintptr_t>(cached)) ? cached : nullptr;
}

void TryHandleSoundMessage(uintptr_t serializer, uintptr_t netMessage) {
    // Sound parsing exists solely for Auto Parry. Avoid protobuf inspection,
    // VirtualQuery calls and sound-name resolution for every combat sound
    // while the feature is disabled.
    if (!autoParry || InterlockedCompareExchange(&soundParsingDisabled, 0, 0) != 0 ||
        !IsReadable(serializer, 0x2A) || !IsReadable(netMessage + kParseMessageProtobufOffset)) return;

    int sourceEntity = 0;
    uint32_t soundHash = 0;
    char soundName[128]{};
    __try {
        if (Read<uint16_t>(serializer + 0x28) != kSosStartSoundEvent) return;
        auto* message = reinterpret_cast<CMsgSosStartSoundEvent*>(netMessage + kParseMessageProtobufOffset);
        if (!message->has_source_entity_index() || !message->has_soundevent_hash()) return;
        sourceEntity = message->source_entity_index();
        soundHash = message->soundevent_hash();
        bool cachedMeleeCharge = false;
        if (FindCachedMeleeSound(soundHash, cachedMeleeCharge)) {
            if (cachedMeleeCharge) NotifyParrySound(sourceEntity, kMeleeChargeSound);
            return;
        }
        const char* gameSoundName = nullptr;
        if (auto* manager = GetSoundEventManager())
            gameSoundName = manager->GetSoundEventName(soundHash);
        if (gameSoundName && IsReadable(reinterpret_cast<uintptr_t>(gameSoundName), 1)) {
            for (size_t i = 0; i + 1 < sizeof(soundName); ++i) {
                const uintptr_t address = reinterpret_cast<uintptr_t>(gameSoundName) + i;
                if (!IsReadable(address, 1)) break;
                soundName[i] = Read<char>(address);
                if (soundName[i] == '\0') break;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&soundParsingDisabled, 1);
        LogSound("parser exception; sound parsing disabled");
        return;
    }

    const bool isMeleeCharge = soundName[0] &&
        std::strcmp(soundName, kMeleeChargeSound) == 0;
    CacheMeleeSound(soundHash, isMeleeCharge);
    if (isMeleeCharge) NotifyParrySound(sourceEntity, kMeleeChargeSound);
}

void HandleDamageMessageUnsafe(uintptr_t serializer, uintptr_t netMessage) {
    if (Read<uint16_t>(serializer + 0x28) != kCitadelDamageMessage)
        return;
    // The reference hook receives a fully decoded protobuf object at
    // netMessage + 0x30. Reflection lets us read the three fields used by
    // Anti-Frog without importing the archive's 2 MB generated message unit
    // and all of its unrelated GC dependencies.
    auto* message = reinterpret_cast<google::protobuf::Message*>(
        netMessage + kParseMessageProtobufOffset);
    const auto* descriptor = message->GetDescriptor();
    const auto* reflection = message->GetReflection();
    if (!descriptor || !reflection ||
        descriptor->name() != "CCitadelUserMessage_Damage") {
        return;
    }
    const auto* attacker = descriptor->FindFieldByName("entindex_attacker");
    const auto* victim = descriptor->FindFieldByName("entindex_victim");
    const auto* hitgroup = descriptor->FindFieldByName("hitgroup_id");
    if (!attacker || !victim || !hitgroup ||
        !reflection->HasField(*message, attacker) ||
        !reflection->HasField(*message, victim) ||
        !reflection->HasField(*message, hitgroup)) {
        return;
    }
    NotifyAntiFrogDamage(
        reflection->GetInt32(*message, attacker),
        reflection->GetInt32(*message, victim),
        reflection->GetInt32(*message, hitgroup));
}

void TryHandleDamageMessage(uintptr_t serializer, uintptr_t netMessage) {
    if (InterlockedCompareExchange(&damageParsingDisabled, 0, 0) != 0 ||
        !IsReadable(serializer, 0x2A) ||
        !IsReadable(netMessage + kParseMessageProtobufOffset)) {
        return;
    }
    __try {
        HandleDamageMessageUnsafe(serializer, netMessage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&damageParsingDisabled, 1);
        LogSound("damage parser exception; Anti-Frog events disabled");
    }
}

bool IsInterestingNetworkMessage(const std::string& fullName) {
    std::string lowered = fullName;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    if (lowered.find("sosstartsound") != std::string::npos ||
        lowered.find("damage") != std::string::npos) {
        return false;
    }
    constexpr const char* needles[]{
        "usermessage", "gameevent", "event", "custom", "text", "chat",
        "command", "console", "script", "panorama", "hud"
    };
    for (const char* needle : needles) {
        if (lowered.find(needle) != std::string::npos) return true;
    }
    return false;
}

void DumpNetworkMessageUnsafe(uintptr_t serializer, uintptr_t netMessage) {
    const uint16_t typeId = Read<uint16_t>(serializer + 0x28);
    auto* message = reinterpret_cast<google::protobuf::Message*>(
        netMessage + kParseMessageProtobufOffset);
    const auto* descriptor = message->GetDescriptor();
    if (!descriptor) return;

    const std::string fullName = descriptor->full_name();
    const bool interesting = IsInterestingNetworkMessage(fullName);
    std::lock_guard<std::mutex> lock(networkDumpMutex);
    NetworkMessageStats& stats = networkMessageStats[fullName];
    ++stats.count;
    if (networkDumpLines >= kMaxNetworkDumpLines) return;

    // Keep a catalog entry for every decoded protobuf type. For likely
    // Lockify control/user messages, also retain every changed payload.
    if (!interesting) {
        if (stats.count != 1) return;
        std::ofstream catalog(
            Dll6Paths::DataFileA("lockify_net_types.log"), std::ios::app);
        if (catalog) {
            catalog << GetTickCount64() << ",type=" << typeId
                    << ",name=" << fullName
                    << ",bytes=" << message->ByteSizeLong() << '\n';
        }
        return;
    }
    if (stats.loggedPayloads >= kMaxPayloadsPerType) return;

    std::string payload = message->ShortDebugString();
    const uint64_t payloadHash = static_cast<uint64_t>(
        std::hash<std::string>{}(payload));
    if (stats.loggedPayloads && payloadHash == stats.lastPayloadHash) return;
    stats.lastPayloadHash = payloadHash;
    ++stats.loggedPayloads;
    ++networkDumpLines;
    if (payload.size() > kMaxNetworkPayloadChars) {
        payload.resize(kMaxNetworkPayloadChars);
        payload += " ...<truncated>";
    }
    std::replace(payload.begin(), payload.end(), '\n', ' ');
    std::replace(payload.begin(), payload.end(), '\r', ' ');

    std::ofstream log(
        Dll6Paths::DataFileA("lockify_net_messages.log"), std::ios::app);
    if (log) {
        log << GetTickCount64() << ",type=" << typeId
            << ",name=" << fullName
            << ",bytes=" << message->ByteSizeLong()
            << ",payload=" << payload << '\n';
    }
}

void TryDumpNetworkMessage(uintptr_t serializer, uintptr_t netMessage) {
    if (!movementProbeEnabled ||
        InterlockedCompareExchange(&networkDumpDisabled, 0, 0) != 0 ||
        !IsReadable(serializer, 0x2A) ||
        !IsReadable(netMessage + kParseMessageProtobufOffset)) {
        return;
    }
    __try {
        DumpNetworkMessageUnsafe(serializer, netMessage);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&networkDumpDisabled, 1);
        LogSound("network protobuf dumper exception; capture disabled");
    }
}

bool __fastcall HookParseMessage(uintptr_t demoRecorder, uintptr_t serializer, uintptr_t netMessage) {
    PacketEntitiesMetadata packetMetadata{};
    if (serializer && netMessage) {
        packetMetadata = TryReadPacketEntitiesMetadata(serializer, netMessage);
        TryDumpNetworkMessage(serializer, netMessage);
        TryHandleSoundMessage(serializer, netMessage);
        TryHandleDamageMessage(serializer, netMessage);
    }
    const bool result = originalParseMessage
        ? originalParseMessage(demoRecorder, serializer, netMessage) : false;
    if (result && packetMetadata.valid) {
        const uint64_t sequence = packetEntitiesSequence.fetch_add(
            1, std::memory_order_relaxed) + 1;
        CaptureMovementPacketEntitySnapshot(
            sequence, packetMetadata.serverTick, packetMetadata.deltaFrom,
            packetMetadata.updatedEntries, packetMetadata.entityDataBytes,
            packetMetadata.entityDataHash);
    }
    return result;
}

} // namespace

bool InstallSoundEventHook() {
    if (soundHookInstalled) return true;
    auto* engine2 = GetModuleHandleA("engine2.dll");
    parseMessageTarget = reinterpret_cast<void*>(FindPattern(engine2, kParseMessagePattern));
    if (!parseMessageTarget) {
        LogSound("ParseMessage pattern not found");
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED) return false;
    const MH_STATUS createStatus = MH_CreateHook(
        parseMessageTarget, reinterpret_cast<void*>(&HookParseMessage),
        reinterpret_cast<void**>(&originalParseMessage));
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) return false;
    const MH_STATUS enableStatus = MH_EnableHook(parseMessageTarget);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) return false;
    soundHookInstalled = true;
    {
        std::lock_guard<std::mutex> lock(networkDumpMutex);
        networkMessageStats.clear();
        networkDumpLines = 0;
        packetEntitiesSequence.store(0, std::memory_order_relaxed);
        latestServerTick.store(-1, std::memory_order_relaxed);
        packetEntitiesSchemaLogged.store(false, std::memory_order_relaxed);
        InterlockedExchange(&networkDumpDisabled, 0);
        std::ofstream types(
            Dll6Paths::DataFileA("lockify_net_types.log"), std::ios::trunc);
        if (types) types << "timestamp,type,name,bytes\n";
        std::ofstream messages(
            Dll6Paths::DataFileA("lockify_net_messages.log"),
            std::ios::trunc);
        if (messages)
            messages << "timestamp,type,name,bytes,payload\n";
    }
    LogSound("installed");
    return true;
}

void RemoveSoundEventHook() {
    if (!soundHookInstalled) return;
    MH_DisableHook(parseMessageTarget);
    MH_RemoveHook(parseMessageTarget);
    {
        std::lock_guard<std::mutex> lock(networkDumpMutex);
        std::ofstream summary(
            Dll6Paths::DataFileA("lockify_net_summary.log"),
            std::ios::trunc);
        if (summary) {
            summary << "name,count,logged_payloads\n";
            for (const auto& [name, stats] : networkMessageStats)
                summary << name << ',' << stats.count << ','
                        << stats.loggedPayloads << '\n';
        }
        networkMessageStats.clear();
    }
    parseMessageTarget = nullptr;
    originalParseMessage = nullptr;
    soundHookInstalled = false;
}
