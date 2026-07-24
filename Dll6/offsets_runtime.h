#pragma once

#include <cstdint>

struct RuntimeOffsets {
    uintptr_t traceWrapperRva = 0x149F7E0;
    uintptr_t traceContextRva = 0x2E8BC50;
    uintptr_t traceResultInitRva = 0x1E913B0;
    uintptr_t traceGroupRva = 0x14611D0;
    uintptr_t traceLayerRva = 0x146E270;
    uintptr_t traceFilterVtableRva = 0x021D2768;
    uintptr_t viewMatrixRva = 0x3799830;
    bool traceSignaturesReady = false;
};

const RuntimeOffsets& GetRuntimeOffsets();
bool InitializeRuntimeOffsets(uintptr_t moduleBase);
