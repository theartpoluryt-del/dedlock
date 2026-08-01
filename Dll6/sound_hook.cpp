#include "shared.h"

#include <MinHook.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

// The protobuf type and its generated implementation are kept in the work
// tree supplied with this project. They are the same CMsgSosStartSoundEvent
// used by the original ParseMessage hook.
#include "C:/Users/artpo/source/repos/Dll6/Dll6/work/Andromeda-DeadLock-Base-1.2.0/Andromeda-DeadLock-Base/Andromeda-DeadLock/DeadLock/Protobuf/gameevents.pb.h"

namespace {

constexpr uint16_t kSosStartSoundEvent = 208;
constexpr uintptr_t kParseMessageProtobufOffset = 0x30;
constexpr char kParseMessagePattern[] = "40 56 57 41 57 48 83 EC ? 4C 8B F9";

using ParseMessageFn = bool(__fastcall*)(uintptr_t, uintptr_t, uintptr_t);
ParseMessageFn originalParseMessage = nullptr;
void* parseMessageTarget = nullptr;
bool soundHookInstalled = false;
volatile LONG soundParsingDisabled = 0;
std::mutex soundLogMutex;
std::mutex soundCacheMutex;
std::unordered_map<uint32_t, bool> meleeSoundHashCache;
constexpr char kMeleeChargeSound[] = "Player.Melee.Hold.Shared";

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
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\sound_hook.log", std::ios::app);
    if (log) log << message << '\n';
}

void LogSoundEvent(int sourceEntity, uint32_t hash, const char* soundName) {
    (void)sourceEntity;
    (void)hash;
    (void)soundName;
}

bool IsReadable(uintptr_t address, size_t size = sizeof(uintptr_t)) {
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

bool __fastcall HookParseMessage(uintptr_t demoRecorder, uintptr_t serializer, uintptr_t netMessage) {
    if (serializer && netMessage)
        TryHandleSoundMessage(serializer, netMessage);
    return originalParseMessage ? originalParseMessage(demoRecorder, serializer, netMessage) : false;
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
    LogSound("installed");
    return true;
}

void RemoveSoundEventHook() {
    if (!soundHookInstalled) return;
    MH_DisableHook(parseMessageTarget);
    MH_RemoveHook(parseMessageTarget);
    parseMessageTarget = nullptr;
    originalParseMessage = nullptr;
    soundHookInstalled = false;
}
