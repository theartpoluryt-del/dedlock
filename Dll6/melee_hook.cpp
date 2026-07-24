#include "shared.h"

#include <cstdint>
#include <cstring>

#define printf(...) do { } while (0)

namespace {

constexpr uintptr_t State1Rva = 0x2792C0;
constexpr uintptr_t State2Rva = 0x279520;
constexpr uintptr_t State3Rva = 0x278FE0;
constexpr uintptr_t AbilityOwnerHandle = 0x51C;
constexpr int PatchLength = 5;

using StateWriterFn = __int64(__fastcall*)(uintptr_t);

StateWriterFn originalState1 = nullptr;
StateWriterFn originalState2 = nullptr;
StateWriterFn originalState3 = nullptr;
struct HookSlot {
    uint8_t* target = nullptr;
    void* relay = nullptr;
    void* trampoline = nullptr;
    uint8_t original[PatchLength]{};
};

HookSlot hookState1, hookState2, hookState3;
volatile LONG monitorInstalled = 0;
ULONGLONG lastWriterLog = 0;

void LogStateWriter(uintptr_t ability, uintptr_t rva, int state) {
    if (!ability) return;
    const uint32_t ownerHandle = Read<uint32_t>(ability + AbilityOwnerHandle);
    if (ownerHandle == 0xFFFFFFFFu || ownerHandle == currentLocalPawnHandle) return;
    const ULONGLONG now = GetTickCount64();
    if (now - lastWriterLog < 40) return;
    lastWriterLog = now;
    printf("[ParryWriter] client+0x%llX owner=0x%X state=%d ability=0x%p\n",
           static_cast<unsigned long long>(rva), ownerHandle, state,
           reinterpret_cast<void*>(ability));
}

__int64 __fastcall HookedState1(uintptr_t ability) { LogStateWriter(ability, State1Rva, 1); return originalState1 ? originalState1(ability) : 0; }
__int64 __fastcall HookedState2(uintptr_t ability) { LogStateWriter(ability, State2Rva, 2); return originalState2 ? originalState2(ability) : 0; }
__int64 __fastcall HookedState3(uintptr_t ability) { LogStateWriter(ability, State3Rva, 3); return originalState3 ? originalState3(ability) : 0; }

bool FitsRelativeJump(const uint8_t* target, const void* destination) {
    const intptr_t distance = reinterpret_cast<const uint8_t*>(destination) - (target + PatchLength);
    return distance >= INT32_MIN && distance <= INT32_MAX;
}

void WriteAbsoluteJump(uint8_t* address, const void* destination) {
    address[0] = 0x48;
    address[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(address + 2) = reinterpret_cast<uint64_t>(destination);
    address[10] = 0xFF;
    address[11] = 0xE0;
}

void* AllocateRelayNear(uint8_t* target) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(target) & ~static_cast<uintptr_t>(0xFFFF);
    constexpr uintptr_t maxDistance = 0x7FFF0000;
    constexpr uintptr_t step = 0x10000;
    for (uintptr_t distance = step; distance <= maxDistance; distance += step) {
        const uintptr_t candidates[] = { base + distance, base - distance };
        for (const uintptr_t candidate : candidates) {
            void* relay = VirtualAlloc(reinterpret_cast<void*>(candidate), 0x1000,
                                        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (relay && FitsRelativeJump(target, relay)) return relay;
            if (relay) VirtualFree(relay, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

bool InstallOne(HookSlot& slot, uintptr_t rva, const void* detour, StateWriterFn* original) {
    slot.target = reinterpret_cast<uint8_t*>(clientBase + rva);
    slot.relay = AllocateRelayNear(slot.target);
    slot.trampoline = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!slot.relay || !slot.trampoline) {
        if (slot.relay) VirtualFree(slot.relay, 0, MEM_RELEASE);
        if (slot.trampoline) VirtualFree(slot.trampoline, 0, MEM_RELEASE);
        slot = HookSlot{};
        return false;
    }

    std::memcpy(slot.original, slot.target, PatchLength);
    auto* trampolineBytes = static_cast<uint8_t*>(slot.trampoline);
    std::memcpy(trampolineBytes, slot.original, PatchLength);
    WriteAbsoluteJump(trampolineBytes + PatchLength, slot.target + PatchLength);
    *original = reinterpret_cast<StateWriterFn>(slot.trampoline);

    WriteAbsoluteJump(static_cast<uint8_t*>(slot.relay), detour);
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot.target, PatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(slot.relay, 0, MEM_RELEASE);
        VirtualFree(slot.trampoline, 0, MEM_RELEASE);
        slot = HookSlot{};
        return false;
    }
    slot.target[0] = 0xE9;
    *reinterpret_cast<int32_t*>(slot.target + 1) = static_cast<int32_t>(
        static_cast<uint8_t*>(slot.relay) - (slot.target + PatchLength));
    DWORD unused = 0;
    VirtualProtect(slot.target, PatchLength, oldProtect, &unused);
    FlushInstructionCache(GetCurrentProcess(), slot.target, PatchLength);
    FlushInstructionCache(GetCurrentProcess(), slot.relay, 12);
    FlushInstructionCache(GetCurrentProcess(), slot.trampoline, PatchLength + 12);
    return true;
}

} // namespace

bool InstallMeleeStateMonitor() {
    if (InterlockedCompareExchange(&monitorInstalled, 1, 0) != 0) return true;
    if (!clientBase) { InterlockedExchange(&monitorInstalled, 0); return false; }

    if (!InstallOne(hookState1, State1Rva, reinterpret_cast<void*>(&HookedState1), &originalState1) ||
        !InstallOne(hookState2, State2Rva, reinterpret_cast<void*>(&HookedState2), &originalState2) ||
        !InstallOne(hookState3, State3Rva, reinterpret_cast<void*>(&HookedState3), &originalState3)) {
        printf("[-] Melee writer monitor: target is not safely detourable\n");
        RemoveMeleeStateMonitor();
        return false;
    }
    printf("[+] Melee writer monitor: ready (state1/2/3)\n");
    return true;
}

void RemoveMeleeStateMonitor() {
    if (InterlockedExchange(&monitorInstalled, 0) == 0) return;
    HookSlot* slots[] = { &hookState1, &hookState2, &hookState3 };
    for (HookSlot* slot : slots) {
        if (!slot->target) continue;
        DWORD oldProtect = 0;
        if (VirtualProtect(slot->target, PatchLength, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            std::memcpy(slot->target, slot->original, PatchLength);
            DWORD unused = 0;
            VirtualProtect(slot->target, PatchLength, oldProtect, &unused);
            FlushInstructionCache(GetCurrentProcess(), slot->target, PatchLength);
        }
        if (slot->relay) VirtualFree(slot->relay, 0, MEM_RELEASE);
        if (slot->trampoline) VirtualFree(slot->trampoline, 0, MEM_RELEASE);
        *slot = HookSlot{};
    }
    originalState1 = originalState2 = originalState3 = nullptr;
}
