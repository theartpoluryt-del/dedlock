#include "shared.h"
#include "usercmd_runtime.hpp"

#include <psapi.h>
#include <sstream>

namespace {

std::vector<int> ParsePattern(const char* pattern) {
    std::vector<int> result;
    std::istringstream stream(pattern ? pattern : "");
    std::string token;
    while (stream >> token) {
        if (token == "?" || token == "??") {
            result.push_back(-1);
        } else {
            result.push_back(std::stoi(token, nullptr, 16));
        }
    }
    return result;
}

uintptr_t FindPattern(uintptr_t base, std::size_t size, const char* pattern) {
    const auto bytes = ParsePattern(pattern);
    if (bytes.empty() || bytes.size() > size) return 0;

    uintptr_t found = 0;
    for (std::size_t i = 0; i <= size - bytes.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < bytes.size(); ++j) {
            if (bytes[j] >= 0 && Read<std::uint8_t>(base + i + j) != static_cast<std::uint8_t>(bytes[j])) {
                match = false;
                break;
            }
        }
        if (!match) continue;
        if (found) return 0;
        found = base + i;
    }
    return found;
}

uintptr_t ResolveRipRelative(uintptr_t instruction, std::size_t displacementOffset, std::size_t instructionLength) {
    const auto displacement = Read<std::int32_t>(instruction + displacementOffset);
    return instruction + instructionLength + static_cast<std::intptr_t>(displacement);
}

std::size_t ModuleImageSize(uintptr_t base) {
    MODULEINFO info{};
    HMODULE module = reinterpret_cast<HMODULE>(base);
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) return 0;
    return static_cast<std::size_t>(info.SizeOfImage);
}

}

UserCmdFunctionAddresses ResolveUserCmdFunctions(uintptr_t moduleBase) {
    UserCmdFunctionAddresses result{};
    const std::size_t imageSize = ModuleImageSize(moduleBase);
    if (!moduleBase || !imageSize) return result;

    result.getUserCmdTick = FindPattern(moduleBase, imageSize,
        "48 83 EC ? 4C 8B 0D ? ? ? ? 4C 8B DA");
    result.getUserCmdArray = FindPattern(moduleBase, imageSize,
        "48 89 4C 24 ? 41 56 41 57");
    result.getUserCmdBySequence = FindPattern(moduleBase, imageSize,
        "40 53 48 83 EC ? 8B DA E8 ? ? ? ? 4C 8B C0");
    result.createMove = FindPattern(moduleBase, imageSize,
        "85 D2 0F 85 ? ? ? ? 48 8B C4 44 88 40 18");
    const uintptr_t firstArrayLoad = FindPattern(moduleBase, imageSize,
        "48 8B 0D ? ? ? ? E8 ? ? ? ? 48 8B CF 48 8B F0");
    if (firstArrayLoad) {
        result.firstUserCmdArrayGlobal = ResolveRipRelative(firstArrayLoad, 3, 7);
    }
    return result;
}
