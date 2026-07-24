#pragma once

#include <cstdint>
#include <limits>

#include "protobuf/citadel_usercmd.pb.h"

// Source 2 input masks used by the generated Citadel user-command object.
enum class InputBitMask : std::uint64_t {
    None       = 0x0000000000000000ull,
    Attack     = 0x0000000000000001ull,
    Jump       = 0x0000000000000002ull,
    Duck       = 0x0000000000000004ull,
    Forward    = 0x0000000000000008ull,
    Back       = 0x0000000000000010ull,
    Use        = 0x0000000000000020ull,
    MoveLeft   = 0x0000000000000200ull,
    MoveRight  = 0x0000000000000400ull,
    Attack2    = 0x0000000000000800ull,
    Reload     = 0x0000000000002000ull,
};

struct CInButtonState {
    void* vtable{};
    std::uint64_t buttonState1{};
    std::uint64_t buttonState2{};
    std::uint64_t buttonState3{};
};

// The native command is a small wrapper around the protobuf payload. This
// mirrors the layout used by the reference project without importing its SDK.
#pragma pack(push, 1)
struct CUserCmd {
    std::uint8_t pad0[0x10]{};
    CCitadelUserCmdPB cmd{};
    CInButtonState buttonStates{};
    std::uint8_t pad1[0x18]{};
};
#pragma pack(pop)

static_assert(offsetof(CUserCmd, cmd) == 0x10);

