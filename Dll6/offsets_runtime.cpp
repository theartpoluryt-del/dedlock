#include "shared.h"
#include "offsets_runtime.h"

#include <fstream>
#include <sstream>
#include <cstring>

namespace {

RuntimeOffsets g_offsets{};

std::vector<int> ParsePattern(const char* text) {
    std::istringstream input(text);
    std::vector<int> bytes;
    std::string token;
    while (input >> token) {
        if (token == "?" || token == "??") bytes.push_back(-1);
        else bytes.push_back(std::stoi(token, nullptr, 16));
    }
    return bytes;
}

uintptr_t ScanRange(const uint8_t* data, size_t size, const std::vector<int>& pattern) {
    if (!data || pattern.empty() || size < pattern.size()) return 0;
    for (size_t i = 0; i <= size - pattern.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            if (pattern[j] >= 0 && data[i + j] != static_cast<uint8_t>(pattern[j])) {
                match = false;
                break;
            }
        }
        if (match) return reinterpret_cast<uintptr_t>(data + i);
    }
    return 0;
}

uintptr_t ScanText(HMODULE module, const char* pattern) {
    MODULEINFO info{};
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return 0;
    const auto* base = static_cast<const uint8_t*>(info.lpBaseOfDll);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const auto parsed = ParsePattern(pattern);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (memcmp(section->Name, ".text", 5) != 0) continue;
        const auto* address = base + section->VirtualAddress;
        return ScanRange(address, section->Misc.VirtualSize, parsed);
    }
    return 0;
}

void LogOffset(std::ofstream& log, const char* name, uintptr_t value, uintptr_t base, bool scanned) {
    log << name << "=0x" << std::hex << value
        << " rva=0x" << (value >= base ? value - base : 0)
        << " source=" << (scanned ? "signature" : "fallback") << '\n';
}

}

const RuntimeOffsets& GetRuntimeOffsets() {
    return g_offsets;
}

bool InitializeRuntimeOffsets(uintptr_t moduleBase) {
    g_offsets = {};
    if (!moduleBase) return false;
    const HMODULE module = reinterpret_cast<HMODULE>(moduleBase);
    std::ofstream log("C:\\Users\\artpo\\source\\repos\\Dll6\\Dll6\\x64\\Release\\offsets_runtime.log", std::ios::trunc);
    if (log) log << "clientBase=0x" << std::hex << moduleBase << '\n';

    const uintptr_t traceWrapper = ScanText(module, "E8 ?? ?? ?? ?? 41 0F B6 47 08 4C 8B BC 24 C8 00 00 00 D0 E8 A8 01 74 43 48 83 BB 40 03 00 00 00 00");
    const uintptr_t traceResult = ScanText(module, "1F C1 E8 1F 84 C0 75 14 48 8B 05 ?? ?? ?? ?? 48 8B 53 78 48 8B 08 48 8B 01 FF 50 18");
    const uintptr_t traceGroup = ScanText(module, "5C 24 40 0F 57 FF F3 0F 10 44 24 44 0F 28 E3 F3 0F 10 4C 24 48 0F 28 EB");
    const uintptr_t traceLayer = ScanText(module, "FF FF FF 7F B8 00 00 00 00 48 8B 4D C0 48 8B 57 08 48 0F 44 C8");

    if (traceWrapper) g_offsets.traceWrapperRva = traceWrapper - moduleBase;
    if (traceResult) g_offsets.traceResultInitRva = traceResult - moduleBase;
    if (traceGroup) g_offsets.traceGroupRva = traceGroup - moduleBase;
    if (traceLayer) g_offsets.traceLayerRva = traceLayer - moduleBase;
    g_offsets.traceSignaturesReady = traceWrapper && traceResult && traceGroup && traceLayer;

    if (log) {
        LogOffset(log, "traceWrapper", moduleBase + g_offsets.traceWrapperRva, moduleBase, traceWrapper != 0);
        LogOffset(log, "traceResultInit", moduleBase + g_offsets.traceResultInitRva, moduleBase, traceResult != 0);
        LogOffset(log, "traceGroup", moduleBase + g_offsets.traceGroupRva, moduleBase, traceGroup != 0);
        LogOffset(log, "traceLayer", moduleBase + g_offsets.traceLayerRva, moduleBase, traceLayer != 0);
        LogOffset(log, "traceContext", moduleBase + g_offsets.traceContextRva, moduleBase, false);
        LogOffset(log, "viewMatrix", moduleBase + g_offsets.viewMatrixRva, moduleBase, false);
        log << "traceSignaturesReady=" << (g_offsets.traceSignaturesReady ? 1 : 0) << '\n';
    }
    return g_offsets.traceSignaturesReady;
}
