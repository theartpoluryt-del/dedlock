#pragma once

#include <cstdint>

struct UserCmdFunctionAddresses {
    uintptr_t getUserCmdTick{};
    uintptr_t getUserCmdArray{};
    uintptr_t getUserCmdBySequence{};
    uintptr_t createMove{};
    uintptr_t firstUserCmdArrayGlobal{};

    bool HasCreateMove() const { return createMove != 0; }
    bool HasInputPath() const {
        return getUserCmdTick && getUserCmdArray && getUserCmdBySequence && createMove && firstUserCmdArrayGlobal;
    }
};

// Resolves reference-project signatures inside the loaded client module.
// Resolution is read-only; callers decide whether a hook is appropriate.
UserCmdFunctionAddresses ResolveUserCmdFunctions(uintptr_t moduleBase);
