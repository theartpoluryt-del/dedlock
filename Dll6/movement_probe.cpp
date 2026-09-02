#include "shared.h"
#include "portable_paths.h"
#include "usercmd.hpp"

#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// Recording is deliberately on by default for the movement-training setup.
// The user can still turn it off in Movement Lab; its state is persisted.
bool movementProbeEnabled = true;
bool movementReplayEnabled = true;
bool movementReplayActive = false;
bool movementReplayCalibrating = false;
bool movementReplayKeyCapture = false;
int movementReplayKey = VK_F6;
#ifdef DLL6_MOVEMENT_ONLY
bool localMovementRecording = false;
bool localMovementRecordingReady = false;
bool localMovementPlaybackActive = false;
bool localMovementPlaybackCalibrating = false;
int localMovementRecordKey = VK_F5;
#endif

namespace {

constexpr uint32_t kHazeId = 13;
constexpr float kReplayStartTolerance = 450.0f;
// Sub-unit accuracy is important for zipline interaction, but ordinary pawn
// acceleration cannot reliably settle inside 0.35 without teleporting. This
// remains tighter than one world unit while allowing a stable physical stop.
constexpr float kLocalCalibrationPositionTolerance = 0.75f;
constexpr float kLocalCalibrationSettleSpeed = 8.0f;
constexpr float kReplayAbortDeviation = 650.0f;
constexpr float kDegreesToRadians = 0.017453292519943295f;
constexpr uint32_t kZiplineRecordedReleaseMs = 250;
constexpr uint32_t kZiplineAttachTimeoutMs = 2500;
constexpr float kZiplineReleaseHeightTolerance = 4.0f;
constexpr uint32_t kReplaySpatialLeadMs = 150;
constexpr uint32_t kReplayMaximumActionDelayMs = 300;
constexpr uint32_t kReplayTickRate = 64;
// Lockify's replicated action label changes 2-3 server ticks after the
// corresponding movement impulse is already visible in the Bot2 trajectory.
constexpr uint32_t kReplayActionLeadMs = 47;
// The route clock starts on the same command as the zipline-attach edge.
// A separate preroll delayed every recorded mechanic relative to the pull.
constexpr uint32_t kReplayJumpPreroll = 0;
constexpr uint32_t kBot2DurationMs = 10810;
constexpr Vector3 kReplayGroundStart{-29.03f, -1653.44f, 376.0f};
// A point on the actual rising zipline segment from the recorded ghost. Aim
// from the local third-person camera at world space, rather than reusing the
// remote bot's pawn eye angles (which do not include local camera parallax).
constexpr Vector3 kReplayZiplineAimPoint{454.47f, -1956.34f, 946.19f};

struct ReplayKeyframe {
    Vector3 position{};
    float pitch{};
    float yaw{};
    uint32_t milliseconds{};
};

// Clean bot2 pass recorded on the movement-training map. Sparse camera
// samples are interpolated; steering targets the next point and continuously
// corrects small deviations from the recorded line.
constexpr std::array<ReplayKeyframe, 42> kBot2Route{{
    { Vector3{-29.03f, -1653.44f, 376.00f}, -43.07f, -31.29f, 0 },
    { Vector3{28.43f, -1697.87f, 513.92f}, -32.69f, -30.59f, 250 },
    { Vector3{96.93f, -1750.30f, 677.83f}, -25.14f, -29.71f, 516 },
    { Vector3{454.47f, -1956.34f, 946.19f}, 0.35f, -28.12f, 1031 },
    { Vector3{569.84f, -2016.24f, 933.34f}, 7.38f, -26.72f, 1250 },
    { Vector3{719.94f, -2094.15f, 876.43f}, 5.80f, -63.63f, 1500 },
    { Vector3{932.20f, -2107.94f, 849.35f}, 42.54f, -69.43f, 1766 },
    { Vector3{1160.08f, -2107.94f, 847.50f}, 34.80f, -63.98f, 2000 },
    { Vector3{1437.50f, -2087.02f, 875.58f}, 19.16f, -3.87f, 2266 },
    { Vector3{1705.47f, -2046.94f, 942.41f}, 20.57f, 3.87f, 2500 },
    { Vector3{1964.68f, -2031.70f, 965.06f}, 23.73f, -2.29f, 2719 },
    { Vector3{2281.37f, -2048.59f, 935.24f}, 27.60f, -11.95f, 3016 },
    { Vector3{2510.38f, -2089.26f, 864.66f}, 29.00f, -21.27f, 3250 },
    { Vector3{2746.59f, -2159.74f, 735.07f}, 27.42f, -28.30f, 3500 },
    { Vector3{2942.44f, -2230.25f, 648.53f}, 29.36f, -36.39f, 3734 },
    { Vector3{3153.63f, -2322.88f, 648.03f}, 35.51f, -41.31f, 4000 },
    { Vector3{3347.27f, -2401.32f, 662.06f}, 25.31f, -30.41f, 4250 },
    { Vector3{3516.13f, -2454.82f, 698.74f}, 23.38f, -23.56f, 4500 },
    { Vector3{3698.29f, -2497.30f, 691.08f}, 23.38f, -14.77f, 4734 },
    { Vector3{3898.66f, -2538.89f, 619.46f}, 25.14f, -19.16f, 5016 },
    { Vector3{4042.36f, -2590.34f, 517.10f}, 25.84f, -23.20f, 5250 },
    { Vector3{4206.51f, -2619.17f, 351.36f}, 23.20f, -12.66f, 5500 },
    { Vector3{4341.01f, -2629.58f, 248.34f}, 27.25f, -13.54f, 5734 },
    { Vector3{4496.13f, -2633.10f, 248.03f}, 31.46f, -16.52f, 5984 },
    { Vector3{4639.49f, -2614.00f, 257.44f}, 25.49f, -7.73f, 6250 },
    { Vector3{4764.92f, -2580.71f, 298.84f}, 21.80f, 2.11f, 6500 },
    { Vector3{4943.58f, -2559.21f, 302.20f}, 27.77f, -1.76f, 6766 },
    { Vector3{5103.14f, -2559.22f, 302.59f}, 28.12f, -4.92f, 6984 },
    { Vector3{5300.59f, -2572.65f, 282.32f}, 33.57f, -18.46f, 7250 },
    { Vector3{5457.30f, -2583.62f, 256.12f}, 41.13f, -14.06f, 7500 },
    { Vector3{5612.61f, -2537.09f, 256.03f}, 29.88f, 9.14f, 7750 },
    { Vector3{5741.04f, -2480.89f, 257.50f}, 30.06f, 12.30f, 8000 },
    { Vector3{5865.40f, -2447.58f, 297.60f}, 24.61f, 9.14f, 8250 },
    { Vector3{6020.88f, -2438.72f, 304.25f}, 31.29f, -6.33f, 8516 },
    { Vector3{6120.72f, -2448.17f, 271.81f}, 31.11f, -16.00f, 8734 },
    { Vector3{6258.47f, -2428.21f, 256.03f}, 32.87f, 2.81f, 8984 },
    { Vector3{6376.57f, -2388.74f, 256.03f}, 35.33f, -2.11f, 9250 },
    { Vector3{6495.61f, -2357.03f, 256.17f}, 39.73f, -46.23f, 9531 },
    { Vector3{6635.56f, -2301.31f, 256.66f}, 39.02f, -28.48f, 9750 },
    { Vector3{6770.23f, -2247.99f, 256.02f}, 37.44f, -14.77f, 10000 },
    { Vector3{6911.55f, -2226.99f, 256.03f}, 44.30f, -20.21f, 10250 },
    { Vector3{7029.23f, -2243.12f, 256.03f}, 31.82f, -11.07f, 10469 },
}};

enum ReplayVirtualKey : uint32_t {
    ReplayW = 1u << 0,
    ReplayS = 1u << 1,
    ReplayA = 1u << 2,
    ReplayD = 1u << 3,
    ReplayJump = 1u << 4,
    ReplayDuck = 1u << 5,
    ReplayDash = 1u << 6,
};

struct ReplayInputFrame {
    uint32_t milliseconds;
    uint32_t keys;
};

#ifdef DLL6_MOVEMENT_ONLY
struct LocalSubtickFrame {
    uint64_t button{};
    bool pressed{};
    float when{};
    float analogForwardDelta{};
    float analogLeftDelta{};
    float pitchDelta{};
    float yawDelta{};
};

struct LocalCommandFrame {
    uint32_t relativeTick{};
    uint32_t milliseconds{};
    Vector3 position{};
    Vector3 velocity{};
    // Keep the rendered camera separate from the two command-angle streams.
    // Citadel can lag/interpolate the rendered camera independently; feeding
    // that value back into usercmd changes the movement basis on turns.
    Vector3 camera{};
    Vector3 commandAngles{};
    Vector3 viewAngles{};
    uint32_t virtualKeys{};
    float forward{};
    float left{};
    float up{};
    uint64_t nativeHeld{};
    uint64_t nativeChanged{};
    uint64_t nativeScroll{};
    uint64_t protobufHeld{};
    uint64_t protobufChanged{};
    uint64_t protobufScroll{};
    uint32_t groundEntity{0xFFFFFFFFu};
    uint32_t attachZipline{0xFFFFFFFFu};
    bool inAir{};
    bool isDashing{};
    bool dashJump{};
    bool slideJump{};
    bool wantsSlide{};
    bool crouching{};
    bool sprinting{};
    std::vector<LocalSubtickFrame> subticks;
};

struct LocalRawKeyEvent {
    uint32_t milliseconds{};
    uint32_t keyBit{};
    bool pressed{};
};

std::vector<LocalCommandFrame> localCommandFrames;
bool localRecordingLoaded = false;
bool localRecordKeyWasDown = false;
bool localReplayKeyWasDown = false;
int32_t localRecordingStartTick = 0;
ULONGLONG localRecordingStartedAt = 0;
int32_t localLastRecordedTick = INT32_MIN;
size_t localPlaybackIndex = 0;
ULONGLONG localCalibrationReachedAt = 0;
int32_t localPlaybackStartClientTick = 0;
uint32_t localPlaybackCurrentRelativeTick = 0;
int32_t localPlaybackPhaseOffsetTicks = 0;
size_t localPlaybackLastInputFrame = SIZE_MAX;
std::vector<LocalRawKeyEvent> localRawKeyEvents;
std::mutex localRawKeyEventsMutex;
std::atomic<bool> localRawReplayStop{false};
HANDLE localRawReplayThread = nullptr;
ULONGLONG localRawReplayStartedAt = 0;
uint32_t localRawRecordedKeys = 0;
uint32_t localRawCommandKeys = 0;
#endif

std::vector<ReplayKeyframe> recordedRoute;
std::vector<ReplayInputFrame> recordedInputs;
std::atomic<uint32_t> observedBotInputKeys{0};
bool recordedRouteLoaded = false;

struct PacketEntitySnapshot {
    uint64_t packetSequence{};
    int32_t serverTick{-1};
    int32_t deltaFrom{-1};
    int32_t updatedEntries{-1};
    uint32_t entityDataBytes{};
    uint64_t entityDataHash{};
    uint64_t tickMilliseconds{};
    int64_t performanceCounter{};
    uintptr_t entity{};
    uint32_t handle{0xFFFFFFFFu};
    Vector3 networkOrigin{};
    Vector3 absoluteOrigin{};
    Vector3 renderOrigin{};
    Vector3 nodeToWorldOrigin{};
    Vector3 velocity{};
    Vector3 eyeAngles{};
    int animationSequence{-1};
    float animationStartTime{};
    uint32_t attachZipline{0xFFFFFFFFu};
    int ziplineLane{-1};
    bool droppedZipline{};
    bool isDashing{};
    bool dashJump{};
    bool slideJump{};
    bool wantsSlide{};
    bool crouching{};
    bool inAir{};
    bool sprinting{};
    float zipStart{};
    float zipStop{};
    float lastZip{};
    float dashStart{};
    float dashEnd{};
    float dashJumpStart{};
    float dashJumpEnd{};
    Vector3 dashDirection{};
    uint32_t groundEntity{0xFFFFFFFFu};
};

std::atomic<uintptr_t> packetTrackedBot2{0};
std::atomic<uint32_t> packetTrackedBot2Handle{0xFFFFFFFFu};
std::atomic<uintptr_t> packetEyeAnglesOffset{0};
std::mutex packetSnapshotMutex;
std::vector<PacketEntitySnapshot> pendingPacketSnapshots;
constexpr size_t kMaximumPendingPacketSnapshots = 4096;

void ResetPacketSnapshotQueue() {
    packetTrackedBot2.store(0, std::memory_order_release);
    packetTrackedBot2Handle.store(0xFFFFFFFFu, std::memory_order_release);
    std::lock_guard<std::mutex> lock(packetSnapshotMutex);
    pendingPacketSnapshots.clear();
    pendingPacketSnapshots.reserve(kMaximumPendingPacketSnapshots);
}

void FlushPacketSnapshots(bool force = false) {
    static ULONGLONG lastFlushAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (!force && lastFlushAt && now - lastFlushAt < 100) return;
    std::vector<PacketEntitySnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(packetSnapshotMutex);
        if (pendingPacketSnapshots.empty()) return;
        snapshots.swap(pendingPacketSnapshots);
    }
    std::ofstream output(
        Dll6Paths::DataFileA("movement_packet_entities.csv"), std::ios::app);
    if (!output) return;
    lastFlushAt = now;
    for (const PacketEntitySnapshot& s : snapshots) {
        output << s.packetSequence << ',' << s.serverTick << ','
               << s.deltaFrom << ',' << s.updatedEntries << ','
               << s.entityDataBytes << ',' << std::hex << s.entityDataHash
               << ',' << s.entity << ',' << s.handle << std::dec << ','
               << s.tickMilliseconds << ',' << s.performanceCounter << ','
               << s.networkOrigin.x << ',' << s.networkOrigin.y << ','
               << s.networkOrigin.z << ',' << s.absoluteOrigin.x << ','
               << s.absoluteOrigin.y << ',' << s.absoluteOrigin.z << ','
               << s.renderOrigin.x << ',' << s.renderOrigin.y << ','
               << s.renderOrigin.z << ',' << s.nodeToWorldOrigin.x << ','
               << s.nodeToWorldOrigin.y << ',' << s.nodeToWorldOrigin.z << ','
               << s.velocity.x << ',' << s.velocity.y << ',' << s.velocity.z
               << ',' << s.eyeAngles.x << ',' << s.eyeAngles.y << ','
               << s.eyeAngles.z << ',' << s.animationSequence << ','
               << s.animationStartTime << ',' << std::hex << s.attachZipline
               << std::dec << ',' << s.ziplineLane << ',' << s.droppedZipline
               << ',' << s.zipStart << ',' << s.zipStop << ',' << s.lastZip
               << ',' << s.isDashing << ',' << s.dashJump << ','
               << s.slideJump << ',' << s.wantsSlide << ',' << s.crouching
               << ',' << s.inAir << ',' << s.sprinting << ',' << s.dashStart
               << ',' << s.dashEnd << ',' << s.dashJumpStart << ','
               << s.dashJumpEnd << ',' << s.dashDirection.x << ','
               << s.dashDirection.y << ',' << s.dashDirection.z << ','
               << std::hex << s.groundEntity << std::dec << '\n';
    }
}

size_t ReplayRouteSize() {
    return recordedRoute.size() >= 2 ? recordedRoute.size() : kBot2Route.size();
}

const ReplayKeyframe& ReplayRouteAt(size_t index) {
    return recordedRoute.size() >= 2 ? recordedRoute[index] : kBot2Route[index];
}

const ReplayKeyframe& ReplayRouteFront() {
    return ReplayRouteAt(0);
}

const ReplayKeyframe& ReplayRouteBack() {
    return ReplayRouteAt(ReplayRouteSize() - 1);
}

bool HasRecordedRoute() {
    return recordedRoute.size() >= 2 && !recordedInputs.empty();
}

uint32_t ReplayDurationMs() {
    return HasRecordedRoute() ? ReplayRouteBack().milliseconds : kBot2DurationMs;
}

// Exact networked m_messageText transitions emitted by Lockify's Bot2 ghost
// for "Switching Lines (10.81s)". Unlike the old inferred windows, this
// preserves simultaneous opposite keys and the real press/release boundaries.
constexpr std::array<ReplayInputFrame, 55> kBot2Inputs{{
    {0, ReplayW | ReplayJump},
    {250, ReplayW},
    {641, ReplayW | ReplayDuck},
    {688, ReplayW | ReplayDuck | ReplayJump},
    {953, ReplayW | ReplayDuck},
    {1047, ReplayW},
    {1094, ReplayW | ReplayD},
    {1563, ReplayW | ReplayD | ReplayDash},
    {1797, ReplayW | ReplayD},
    {2157, ReplayW | ReplayD | ReplayJump},
    {2547, ReplayW | ReplayD},
    {2578, ReplayW | ReplayD | ReplayDuck},
    {2625, ReplayD | ReplayDuck},
    {3453, ReplayW | ReplayD | ReplayDuck},
    {3719, ReplayW | ReplayDuck},
    {4141, ReplayW | ReplayA | ReplayDuck},
    {4235, ReplayW | ReplayA | ReplayDuck | ReplayJump},
    {4469, ReplayW | ReplayA | ReplayDuck},
    {4578, ReplayA | ReplayDuck},
    {4813, ReplayA | ReplayD | ReplayDuck},
    {5078, ReplayD | ReplayDuck},
    {5141, ReplayA | ReplayD | ReplayDuck},
    {5375, ReplayA | ReplayDuck},
    {5500, ReplayW | ReplayA | ReplayDuck},
    {5813, ReplayW | ReplayDuck},
    {6000, ReplayW | ReplayA | ReplayDuck},
    {6188, ReplayW | ReplayA | ReplayDuck | ReplayJump},
    {6422, ReplayW | ReplayA | ReplayDuck},
    {6594, ReplayW | ReplayA | ReplayDash},
    {6844, ReplayW | ReplayA},
    {6891, ReplayW | ReplayA | ReplayD},
    {6922, ReplayW | ReplayA | ReplayD | ReplayDuck},
    {7125, ReplayW | ReplayD | ReplayDuck},
    {7203, ReplayD | ReplayDuck},
    {7250, ReplayA | ReplayD | ReplayDuck},
    {7485, ReplayW | ReplayA | ReplayDuck},
    {7844, ReplayW | ReplayDuck},
    {7891, ReplayW | ReplayD | ReplayDuck},
    {7985, ReplayW | ReplayD | ReplayDuck | ReplayJump},
    {8235, ReplayW | ReplayD | ReplayDuck},
    {8266, ReplayD | ReplayDuck},
    {8578, ReplayA | ReplayD | ReplayDuck},
    {8719, ReplayW | ReplayA | ReplayD | ReplayDuck},
    {8844, ReplayW | ReplayA | ReplayDuck},
    {9063, ReplayW | ReplayDuck},
    {9344, ReplayW | ReplayA | ReplayDuck},
    {9469, ReplayW | ReplayA | ReplayDuck | ReplayDash | ReplayJump},
    {9547, ReplayW | ReplayA | ReplayDash | ReplayJump},
    {9719, ReplayW | ReplayA},
    {9875, ReplayW | ReplayA | ReplayDuck},
    {10000, ReplayW | ReplayDuck},
    {10078, ReplayW | ReplayD | ReplayDuck},
    {10485, ReplayS | ReplayD | ReplayDuck},
    {10532, ReplayS | ReplayDuck},
    {10578, ReplayS},
}};

bool replayKeyWasDown = false;
ULONGLONG replayStartedAt = 0;
ULONGLONG replayRouteStartedAt = 0;
int32_t replayRouteStartTick = 0;
uint32_t replayTimelineMs = 0;
float replayStartDistance = FLT_MAX;
float replayStartZ = 0.0f;
ULONGLONG replayLastLogAt = 0;
ULONGLONG replayCalibrationReachedAt = 0;
std::atomic<uint32_t> replayVirtualKeys{0};
std::atomic<uint64_t> replayHeldButtons{0};
std::atomic<uint64_t> replayPressedButtons{0};
uint32_t replayInjectedActionKeys = 0;
uint32_t replayPreviousInputKeys = 0;
Vector3 replayZiplineTarget{};
bool replayZiplineTargetReady = false;
ULONGLONG replayZiplineScanAt = 0;
bool replayCameraFilterReady = false;
Vector3 replayFilteredCamera{};
ULONGLONG replayCameraFilterAt = 0;

float NormalizeAngle(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float Distance3D(const Vector3& a, const Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool IsFinite(const Vector3& value);

void SaveRecordedRouteProfile() {
    if (!HasRecordedRoute()) return;
    std::ofstream output(
        Dll6Paths::DataFileA("movement_last_route.csv"), std::ios::trunc);
    if (!output) return;
    output << "ms,x,y,z,pitch,yaw,keys\n";
    size_t inputIndex = 0;
    uint32_t keys = recordedInputs.front().keys;
    for (const ReplayKeyframe& frame : recordedRoute) {
        while (inputIndex + 1 < recordedInputs.size() &&
               recordedInputs[inputIndex + 1].milliseconds <= frame.milliseconds) {
            ++inputIndex;
            keys = recordedInputs[inputIndex].keys;
        }
        output << frame.milliseconds << ',' << frame.position.x << ','
               << frame.position.y << ',' << frame.position.z << ','
               << frame.pitch << ',' << frame.yaw << ',' << keys << '\n';
    }
}

bool EqualsAsciiInsensitive(const std::string& left, const char* right);

// Lockify's remote bot does not replicate its CInButtonState to clients, but
// the probe still contains a high-rate authoritative trajectory and camera
// track. Import that fresh pass and pair it with the previously decoded Bot2
// input transitions so one observed run is immediately replayable.
bool ImportLatestBot2ProbeRoute() {
    std::ifstream input(Dll6Paths::DataFileA("movement_haze_probe.csv"));
    if (!input) return false;

    std::vector<ReplayKeyframe> route;
    std::string line;
    std::getline(input, line);
    uint32_t firstTimestamp = 0;
    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::array<std::string, 55> columns{};
        bool complete = true;
        for (std::string& column : columns) {
            if (!std::getline(row, column, ',')) {
                complete = false;
                break;
            }
        }
        if (!complete || !EqualsAsciiInsensitive(columns[2], "bot2"))
            continue;
        try {
            const uint32_t timestamp = static_cast<uint32_t>(
                std::stoul(columns[0]));
            if (route.empty()) firstTimestamp = timestamp;
            if (timestamp < firstTimestamp) continue;
            ReplayKeyframe frame{};
            frame.milliseconds = timestamp - firstTimestamp;
            frame.position = {std::stof(columns[4]), std::stof(columns[5]),
                              std::stof(columns[6])};
            frame.pitch = NormalizeAngle(std::stof(columns[15]));
            frame.yaw = NormalizeAngle(std::stof(columns[16]));
            if (!std::isfinite(frame.position.x) ||
                !std::isfinite(frame.position.y) ||
                !std::isfinite(frame.position.z) ||
                !std::isfinite(frame.pitch) || !std::isfinite(frame.yaw) ||
                (!route.empty() &&
                 frame.milliseconds <= route.back().milliseconds))
                continue;
            route.push_back(frame);
        } catch (...) {
        }
    }

    if (route.size() < 20 || route.back().milliseconds < 3000 ||
        route.back().milliseconds > 120000 ||
        Distance3D(route.front().position, route.back().position) < 1000.0f)
        return false;

    std::vector<ReplayInputFrame> inputs;
    for (const ReplayInputFrame& frame : kBot2Inputs) {
        if (frame.milliseconds > route.back().milliseconds) break;
        inputs.push_back(frame);
    }
    if (inputs.empty()) return false;
    if (inputs.back().milliseconds < route.back().milliseconds)
        inputs.push_back({route.back().milliseconds, 0});

    recordedRoute = std::move(route);
    recordedInputs = std::move(inputs);
    SaveRecordedRouteProfile();
    std::ofstream log(Dll6Paths::DataFileA("movement_replay.log"),
                      std::ios::app);
    if (log)
        log << "imported Bot2 probe frames=" << recordedRoute.size()
            << " duration_ms=" << recordedRoute.back().milliseconds << '\n';
    return true;
}

// PacketEntities is applied before this trace is sampled, so net_x/net_y/net_z
// are the newest replicated values rather than the render-interpolated scene
// origin used by the legacy Present probe. Server ticks also give the route a
// stable 64 Hz clock even when two packets arrive during one rendered frame.
bool ImportLatestPacketEntityRoute() {
    std::ifstream input(
        Dll6Paths::DataFileA("movement_packet_entities.csv"));
    if (!input) return false;

    std::vector<ReplayKeyframe> bestRoute;
    std::vector<ReplayKeyframe> candidate;
    std::string line;
    std::getline(input, line);
    int32_t startTick = -1;
    int32_t previousTick = -1;
    Vector3 previousPosition{};

    const auto finishCandidate = [&]() {
        if (candidate.size() >= 20 &&
            candidate.back().milliseconds >= 3000 &&
            candidate.back().milliseconds <= 120000 &&
            Distance3D(candidate.front().position,
                       candidate.back().position) >= 1000.0f &&
            (bestRoute.empty() || candidate.back().milliseconds >
                                      bestRoute.back().milliseconds)) {
            bestRoute = candidate;
        }
        candidate.clear();
        startTick = -1;
        previousTick = -1;
        previousPosition = {};
    };

    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::array<std::string, 51> columns{};
        bool complete = true;
        for (std::string& column : columns) {
            if (!std::getline(row, column, ',')) {
                complete = false;
                break;
            }
        }
        if (!complete) continue;

        try {
            const int32_t serverTick = std::stoi(columns[1]);
            const Vector3 position{std::stof(columns[10]),
                                   std::stof(columns[11]),
                                   std::stof(columns[12])};
            const Vector3 velocity{std::stof(columns[22]),
                                   std::stof(columns[23]),
                                   std::stof(columns[24])};
            const Vector3 angles{NormalizeAngle(std::stof(columns[25])),
                                 NormalizeAngle(std::stof(columns[26])),
                                 NormalizeAngle(std::stof(columns[27]))};
            if (serverTick < 0 || !IsFinite(position) ||
                !IsFinite(velocity) || !IsFinite(angles))
                continue;

            if (candidate.empty()) {
                const float launchSpeed = std::sqrt(
                    velocity.x * velocity.x + velocity.y * velocity.y +
                    velocity.z * velocity.z);
                if (Distance3D(position, kReplayGroundStart) > 150.0f ||
                    launchSpeed < 80.0f)
                    continue;
                startTick = serverTick;
                previousTick = serverTick;
                previousPosition = position;
                candidate.push_back({position, angles.x, angles.y, 0});
                continue;
            }

            const int32_t tickDelta = serverTick - previousTick;
            const float positionDelta = Distance3D(position, previousPosition);
            if (tickDelta < 0 || tickDelta > 32 || positionDelta > 2000.0f) {
                finishCandidate();
                continue;
            }
            const int32_t routeTicks = serverTick - startTick;
            if (routeTicks < 0) {
                finishCandidate();
                continue;
            }
            const uint32_t elapsed = static_cast<uint32_t>(
                (static_cast<uint64_t>(routeTicks) * 1000 +
                 kReplayTickRate / 2) / kReplayTickRate);
            if (candidate.empty() || elapsed > candidate.back().milliseconds) {
                candidate.push_back(
                    {position, angles.x, angles.y, elapsed});
            }
            previousTick = serverTick;
            previousPosition = position;
        } catch (...) {
        }
    }
    finishCandidate();
    if (bestRoute.empty()) return false;

    std::vector<ReplayInputFrame> inputs;
    for (const ReplayInputFrame& frame : kBot2Inputs) {
        if (frame.milliseconds > bestRoute.back().milliseconds) break;
        inputs.push_back(frame);
    }
    if (inputs.empty()) return false;
    if (inputs.back().milliseconds < bestRoute.back().milliseconds)
        inputs.push_back({bestRoute.back().milliseconds, 0});

    recordedRoute = std::move(bestRoute);
    recordedInputs = std::move(inputs);
    SaveRecordedRouteProfile();
    std::ofstream log(Dll6Paths::DataFileA("movement_replay.log"),
                      std::ios::app);
    if (log)
        log << "imported PacketEntities route frames=" << recordedRoute.size()
            << " duration_ms=" << recordedRoute.back().milliseconds << '\n';
    return true;
}

bool LoadRecordedRouteProfile() {
    if (recordedRouteLoaded) return HasRecordedRoute();
    recordedRouteLoaded = true;
    if (ImportLatestPacketEntityRoute()) return true;
    std::ifstream input(Dll6Paths::DataFileA("movement_last_route.csv"));
    if (!input) return ImportLatestBot2ProbeRoute();
    std::vector<ReplayKeyframe> route;
    std::vector<ReplayInputFrame> inputs;
    std::string line;
    std::getline(input, line);
    uint32_t previousKeys = UINT32_MAX;
    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::array<std::string, 7> columns{};
        bool complete = true;
        for (std::string& column : columns) {
            if (!std::getline(row, column, ',')) {
                complete = false;
                break;
            }
        }
        if (!complete) continue;
        try {
            ReplayKeyframe frame{};
            frame.milliseconds = static_cast<uint32_t>(std::stoul(columns[0]));
            frame.position = {std::stof(columns[1]), std::stof(columns[2]),
                              std::stof(columns[3])};
            frame.pitch = std::stof(columns[4]);
            frame.yaw = std::stof(columns[5]);
            const uint32_t keys = static_cast<uint32_t>(std::stoul(columns[6]));
            if (!std::isfinite(frame.position.x) ||
                !std::isfinite(frame.position.y) ||
                !std::isfinite(frame.position.z) || !std::isfinite(frame.pitch) ||
                !std::isfinite(frame.yaw)) continue;
            if (!route.empty() && frame.milliseconds <= route.back().milliseconds)
                continue;
            route.push_back(frame);
            if (keys != previousKeys) {
                inputs.push_back({frame.milliseconds, keys});
                previousKeys = keys;
            }
        } catch (...) {
        }
    }
    if (route.size() < 20 || inputs.empty() ||
        route.back().milliseconds < 300)
        return ImportLatestBot2ProbeRoute();
    recordedRoute = std::move(route);
    recordedInputs = std::move(inputs);
    return true;
}

void UpdateAutomaticRouteCapture(ULONGLONG now, const Vector3& position,
                                 const Vector3& viewAngles,
                                 const Vector3& velocity, float speed2d) {
    static bool capturing = false;
    static ULONGLONG captureStartedAt = 0;
    static ULONGLONG lastActiveAt = 0;
    static uint32_t previousKeys = UINT32_MAX;
    static std::vector<ReplayKeyframe> route;
    static std::vector<ReplayInputFrame> inputs;

    const uint32_t keys = observedBotInputKeys.load(std::memory_order_acquire);
    const bool moving = speed2d > 45.0f || std::fabs(velocity.z) > 45.0f;
    const bool active = moving || keys != 0;
    if (!capturing) {
        // The server does not always replicate the overhead input text. A
        // real movement sample near the known launch area is therefore also
        // a valid start, while the position gate rejects the parking-to-start
        // teleport spike.
        const bool movingFromRouteStart = moving &&
            Distance3D(position, kReplayGroundStart) < kReplayStartTolerance;
        if (keys == 0 && !movingFromRouteStart) return;
        capturing = true;
        captureStartedAt = now;
        lastActiveAt = now;
        previousKeys = UINT32_MAX;
        route.clear();
        inputs.clear();
    }

    const uint32_t elapsed = static_cast<uint32_t>(now - captureStartedAt);
    if (active) lastActiveAt = now;
    if (active || now - lastActiveAt < 500) {
        route.push_back({position, NormalizeAngle(viewAngles.x),
                         NormalizeAngle(viewAngles.y), elapsed});
        if (keys != previousKeys) {
            inputs.push_back({elapsed, keys});
            previousKeys = keys;
        }
    }

    const bool completed = !active && now - lastActiveAt >= 500;
    const bool timedOut = elapsed >= 120000;
    if (!completed && !timedOut) return;
    capturing = false;
    if (route.size() < 20 || elapsed < 300) return;

    while (!route.empty() && route.back().milliseconds >
           static_cast<uint32_t>(lastActiveAt - captureStartedAt + 50))
        route.pop_back();
    if (route.size() < 20) return;
    const bool capturedRealInputs = std::any_of(
        inputs.begin(), inputs.end(),
        [](const ReplayInputFrame& frame) { return frame.keys != 0; });
    if (!capturedRealInputs) {
        inputs.clear();
        for (const ReplayInputFrame& frame : kBot2Inputs) {
            if (frame.milliseconds > route.back().milliseconds) break;
            inputs.push_back(frame);
        }
    }
    if (inputs.empty()) return;
    if (inputs.back().milliseconds < route.back().milliseconds)
        inputs.push_back({route.back().milliseconds, 0});
    recordedRoute = std::move(route);
    recordedInputs = std::move(inputs);
    recordedRouteLoaded = true;
    SaveRecordedRouteProfile();
}

uint32_t ClosestRouteProgress(const Vector3& position) {
    float bestDistanceSquared = FLT_MAX;
    uint32_t bestMilliseconds = 0;
    for (size_t i = 1; i < ReplayRouteSize(); ++i) {
        const ReplayKeyframe& start = ReplayRouteAt(i - 1);
        const ReplayKeyframe& end = ReplayRouteAt(i);
        const Vector3 segment{end.position.x - start.position.x,
                              end.position.y - start.position.y,
                              end.position.z - start.position.z};
        const Vector3 offset{position.x - start.position.x,
                             position.y - start.position.y,
                             position.z - start.position.z};
        const float lengthSquared = segment.x * segment.x +
            segment.y * segment.y + segment.z * segment.z;
        const float projection = lengthSquared > 0.001f
            ? std::clamp((offset.x * segment.x + offset.y * segment.y +
                          offset.z * segment.z) / lengthSquared, 0.0f, 1.0f)
            : 0.0f;
        const Vector3 closest{start.position.x + segment.x * projection,
                              start.position.y + segment.y * projection,
                              start.position.z + segment.z * projection};
        const float dx = position.x - closest.x;
        const float dy = position.y - closest.y;
        const float dz = position.z - closest.z;
        const float distanceSquared = dx * dx + dy * dy + dz * dz;
        if (distanceSquared >= bestDistanceSquared) continue;
        bestDistanceSquared = distanceSquared;
        bestMilliseconds = start.milliseconds + static_cast<uint32_t>(
            (end.milliseconds - start.milliseconds) * projection);
    }
    return bestMilliseconds;
}

uint32_t ClosestRouteHorizontalProgress(const Vector3& position,
                                        uint32_t nearProgress) {
    float bestDistanceSquared = FLT_MAX;
    uint32_t bestMilliseconds = 0;
    for (size_t i = 1; i < ReplayRouteSize(); ++i) {
        const ReplayKeyframe& start = ReplayRouteAt(i - 1);
        const ReplayKeyframe& end = ReplayRouteAt(i);
        if (end.milliseconds + 600u < nearProgress ||
            start.milliseconds > nearProgress + 600u)
            continue;
        const float segmentX = end.position.x - start.position.x;
        const float segmentY = end.position.y - start.position.y;
        const float lengthSquared = segmentX * segmentX + segmentY * segmentY;
        const float offsetX = position.x - start.position.x;
        const float offsetY = position.y - start.position.y;
        const float projection = lengthSquared > 0.001f
            ? std::clamp((offsetX * segmentX + offsetY * segmentY) /
                             lengthSquared,
                         0.0f, 1.0f)
            : 0.0f;
        const float closestX = start.position.x + segmentX * projection;
        const float closestY = start.position.y + segmentY * projection;
        const float dx = position.x - closestX;
        const float dy = position.y - closestY;
        const float distanceSquared = dx * dx + dy * dy;
        if (distanceSquared >= bestDistanceSquared) continue;
        bestDistanceSquared = distanceSquared;
        bestMilliseconds = start.milliseconds + static_cast<uint32_t>(
            (end.milliseconds - start.milliseconds) * projection);
    }
    return bestMilliseconds;
}

bool FindReplayZiplineTarget(Vector3& result) {
    if (!clientBase || !currentCameraPositionReady) return false;
    const ULONGLONG now = GetTickCount64();
    if (replayZiplineScanAt && now - replayZiplineScanAt < 2000) {
        if (replayZiplineTargetReady) result = replayZiplineTarget;
        return replayZiplineTargetReady;
    }
    replayZiplineScanAt = now;
    const uintptr_t entityRoot = Read<uintptr_t>(
        clientBase + Offsets::GameEntitySystem);
    if (!entityRoot) return false;
    const int highest = Read<int>(entityRoot + Offsets::HighestEntityIndex);
    if (highest <= 0 || highest > static_cast<int>(Offsets::HandleIndexMask))
        return false;

    const float expectedPitch = ReplayRouteFront().pitch * kDegreesToRadians;
    const float expectedYaw = ReplayRouteFront().yaw * kDegreesToRadians;
    const Vector3 expected{
        std::cos(expectedPitch) * std::cos(expectedYaw),
        std::cos(expectedPitch) * std::sin(expectedYaw),
        -std::sin(expectedPitch)};
    float bestScore = -FLT_MAX;
    Vector3 best{};
    std::ofstream candidates(
        Dll6Paths::DataFileA("movement_zipline_candidates.log"),
        std::ios::trunc);
    for (int index = 0; index <= highest; ++index) {
        const uintptr_t entity = ResolveEntityIndex(static_cast<uint32_t>(index));
        if (!entity) continue;
        std::string className = GetEntityClassName(entity);
        std::transform(className.begin(), className.end(), className.begin(),
            [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        if (className.find("zipline") == std::string::npos &&
            className.find("zip_line") == std::string::npos) continue;
        Vector3 position{};
        if (!GetEntityPosition(entity, position) ||
            !std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) continue;
        const Vector3 delta{position.x - currentCameraPosition.x,
                            position.y - currentCameraPosition.y,
                            position.z - currentCameraPosition.z};
        const float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y +
                                         delta.z * delta.z);
        if (distance < 20.0f || distance > 3000.0f) continue;
        const float dot = (delta.x * expected.x + delta.y * expected.y +
                           delta.z * expected.z) / distance;
        const float score = dot * 8.0f - distance / 3000.0f;
        if (candidates)
            candidates << index << ',' << className << ',' << position.x << ','
                       << position.y << ',' << position.z << ',' << distance << ','
                       << dot << ',' << score << '\n';
        if (dot > 0.70f && score > bestScore) {
            bestScore = score;
            best = position;
        }
    }
    replayZiplineTargetReady = bestScore > -FLT_MAX;
    if (replayZiplineTargetReady) replayZiplineTarget = best;
    result = best;
    return replayZiplineTargetReady;
}

Vector3 ReplayCameraAngles(const ReplayKeyframe& frame, bool ziplinePhase) {
    Vector3 target{};
    if (ziplinePhase && currentCameraPositionReady) {
        if (!FindReplayZiplineTarget(target))
            target = kReplayZiplineAimPoint;
        const float dx = target.x - currentCameraPosition.x;
        const float dy = target.y - currentCameraPosition.y;
        const float dz = target.z - currentCameraPosition.z;
        const float horizontal = std::hypot(dx, dy);
        return {-std::atan2(dz, horizontal) * 57.29577951308232f,
                std::atan2(dy, dx) * 57.29577951308232f, 0.0f};
    }
    return {frame.pitch, frame.yaw, 0.0f};
}

ReplayKeyframe InterpolateRoute(uint32_t elapsed) {
    if (elapsed <= ReplayRouteFront().milliseconds) return ReplayRouteFront();
    if (elapsed >= ReplayRouteBack().milliseconds) return ReplayRouteBack();
    for (size_t i = 1; i < ReplayRouteSize(); ++i) {
        if (elapsed > ReplayRouteAt(i).milliseconds) continue;
        const ReplayKeyframe& left = ReplayRouteAt(i - 1);
        const ReplayKeyframe& right = ReplayRouteAt(i);
        const float span = static_cast<float>(right.milliseconds - left.milliseconds);
        const float t = span > 0.0f
            ? static_cast<float>(elapsed - left.milliseconds) / span : 0.0f;
        ReplayKeyframe result{};
        result.position = {left.position.x + (right.position.x - left.position.x) * t,
                           left.position.y + (right.position.y - left.position.y) * t,
                           left.position.z + (right.position.z - left.position.z) * t};
        result.pitch = left.pitch + NormalizeAngle(right.pitch - left.pitch) * t;
        result.yaw = NormalizeAngle(left.yaw + NormalizeAngle(right.yaw - left.yaw) * t);
        result.milliseconds = elapsed;
        return result;
    }
    return ReplayRouteBack();
}

Vector3 ReplayPathCameraAngles(uint32_t elapsed, uint32_t inputKeys) {
    // m_angEyeAngles from a remote Lockify replay pawn is an animation/aim
    // parameter, not the camera which originally produced its user commands.
    // In the captured trace it can jump by 20-40 degrees in one server tick.
    // Feeding that value back as a local camera angle makes the pawn steer
    // straight into a wall.
    //
    // Reconstruct the missing camera basis from two things that *are* present
    // in the recording: the path heading and the held movement axes. Use a
    // small path window so network position jitter cannot twitch the camera.
    const uint32_t behindMs = elapsed > 45u ? elapsed - 45u : 0u;
    const uint32_t aheadMs = (std::min)(
        ReplayDurationMs(), elapsed + kReplaySpatialLeadMs);
    const ReplayKeyframe behind = InterpolateRoute(behindMs);
    const ReplayKeyframe ahead = InterpolateRoute(aheadMs);
    const float dx = ahead.position.x - behind.position.x;
    const float dy = ahead.position.y - behind.position.y;
    const ReplayKeyframe current = InterpolateRoute(elapsed);
    if (std::hypot(dx, dy) < 1.0f)
        return {current.pitch, current.yaw, 0.0f};

    const float forward =
        ((inputKeys & ReplayW) ? 1.0f : 0.0f) -
        ((inputKeys & ReplayS) ? 1.0f : 0.0f);
    const float left =
        ((inputKeys & ReplayA) ? 1.0f : 0.0f) -
        ((inputKeys & ReplayD) ? 1.0f : 0.0f);
    const float pathYaw = std::atan2(dy, dx) * 57.29577951308232f;
    // A single side key mathematically produces a 90-degree basis offset,
    // but Lockify's action text drops W for short air-strafe windows while the
    // pawn keeps its forward momentum. Applying the full 90 degrees made the
    // camera turn from about -43 to -95 at route_ms 2625 and sent the replay
    // into the right wall. Preserve the established diagonal camera basis
    // through those W-up windows instead of doubling the turn.
    const float rawInputOffset = std::hypot(forward, left) > 0.1f
        ? std::atan2(left, forward) * 57.29577951308232f : 0.0f;
    const float inputOffset = (std::clamp)(rawInputOffset, -45.0f, 45.0f);
    const Vector3 desired{
        (std::clamp)(current.pitch, -70.0f, 70.0f),
        NormalizeAngle(pathYaw + inputOffset), 0.0f};

    // Input changes are discrete, but a real camera is not. In particular,
    // W -> W+D used to add 45 degrees in one command and produced the visible
    // snap reported as the camera turning into the wall. Rate-limit both axes
    // while retaining the route-derived steering target.
    const ULONGLONG now = GetTickCount64();
    if (!replayCameraFilterReady) {
        replayFilteredCamera = desired;
        replayCameraFilterReady = true;
        replayCameraFilterAt = now;
        return replayFilteredCamera;
    }
    const float seconds = (std::clamp)(
        static_cast<float>(now - replayCameraFilterAt) / 1000.0f,
        0.001f, 0.050f);
    replayCameraFilterAt = now;
    const float maximumYawStep = 120.0f * seconds;
    const float maximumPitchStep = 100.0f * seconds;
    replayFilteredCamera.y = NormalizeAngle(replayFilteredCamera.y +
        (std::clamp)(NormalizeAngle(desired.y - replayFilteredCamera.y),
                     -maximumYawStep, maximumYawStep));
    replayFilteredCamera.x +=
        (std::clamp)(desired.x - replayFilteredCamera.x,
                     -maximumPitchStep, maximumPitchStep);
    replayFilteredCamera.z = 0.0f;
    return replayFilteredCamera;
}

uint32_t ReplayInputAt(uint32_t elapsed) {
    const bool recorded = HasRecordedRoute();
    const ReplayInputFrame* frames = recorded
        ? recordedInputs.data() : kBot2Inputs.data();
    const size_t count = recorded ? recordedInputs.size() : kBot2Inputs.size();
    uint32_t keys = frames[0].keys;
    for (size_t i = 0; i < count; ++i) {
        const ReplayInputFrame& frame = frames[i];
        if (frame.milliseconds > elapsed) break;
        keys = frame.keys;
    }
    return keys;
}

uint64_t ReplayButtonMask(uint32_t keys) {
    uint64_t mask = 0;
    if (keys & ReplayW) mask |= static_cast<uint64_t>(InputBitMask::Forward);
    if (keys & ReplayS) mask |= static_cast<uint64_t>(InputBitMask::Back);
    if (keys & ReplayA) mask |= static_cast<uint64_t>(InputBitMask::MoveLeft);
    if (keys & ReplayD) mask |= static_cast<uint64_t>(InputBitMask::MoveRight);
    if (keys & ReplayJump) mask |= static_cast<uint64_t>(InputBitMask::Jump);
    if (keys & ReplayDuck) mask |= static_cast<uint64_t>(InputBitMask::Duck);
    if (keys & ReplayDash) mask |= static_cast<uint64_t>(InputBitMask::Speed);
    return mask;
}

void ApplyReplayButtons(CUserCmd* command, uint64_t holdMask,
                        uint64_t changeMask) {
    const uint64_t movementMask =
        static_cast<uint64_t>(InputBitMask::Forward) |
        static_cast<uint64_t>(InputBitMask::Back) |
        static_cast<uint64_t>(InputBitMask::MoveLeft) |
        static_cast<uint64_t>(InputBitMask::MoveRight) |
        static_cast<uint64_t>(InputBitMask::Jump) |
        static_cast<uint64_t>(InputBitMask::Duck) |
        static_cast<uint64_t>(InputBitMask::Speed);
    const uint64_t keep = ~movementMask;
    command->buttonStates.buttonState1 =
        (command->buttonStates.buttonState1 & keep) | holdMask;
    command->buttonStates.buttonState2 =
        (command->buttonStates.buttonState2 & keep) | changeMask;
    if (command->cmd.has_base()) {
        auto* buttons = command->cmd.mutable_base()->mutable_buttons_pb();
        buttons->set_buttonstate1((buttons->buttonstate1() & keep) |
                                  holdMask);
        buttons->set_buttonstate2((buttons->buttonstate2() & keep) | changeMask);
    }
}

void PublishReplayVirtualKeyMask(uint32_t keys) {
    replayVirtualKeys.store(keys, std::memory_order_release);

    // Citadel samples action keys before ApplyInputCommand. Editing the
    // already-built protobuf (or m_nButtons later in ProcessMovement) moves
    // WASD but cannot initiate jump/zipline/dash. Send only action-key edges
    // into the OS input stream so the next game input pump builds them into
    // the native command. Direction remains controlled by the usercmd.
    const uint32_t actionKeys = keys & (ReplayJump | ReplayDuck | ReplayDash);
    struct InjectedKey { uint32_t bit; WORD virtualKey; };
    constexpr InjectedKey injectedKeys[]{
        {ReplayJump, VK_SPACE},
        {ReplayDuck, VK_LCONTROL},
        {ReplayDash, VK_LSHIFT},
    };
    for (const InjectedKey& key : injectedKeys) {
        const bool wasDown = (replayInjectedActionKeys & key.bit) != 0;
        const bool isDown = (actionKeys & key.bit) != 0;
        if (wasDown == isDown) continue;
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wVk = 0;
        event.ki.wScan = static_cast<WORD>(MapVirtualKeyW(
            key.virtualKey, MAPVK_VK_TO_VSC));
        event.ki.dwFlags = KEYEVENTF_SCANCODE |
            (isDown ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP));
        SendInput(1, &event, sizeof(event));
    }
    replayInjectedActionKeys = actionKeys;
}

void PublishReplayVirtualKeys(float forward, float left,
                              bool jump, bool duck, bool dash = false) {
    uint32_t keys = 0;
    if (forward > 0.15f) keys |= ReplayW;
    else if (forward < -0.15f) keys |= ReplayS;
    if (left > 0.15f) keys |= ReplayA;
    else if (left < -0.15f) keys |= ReplayD;
    if (jump) keys |= ReplayJump;
    if (duck) keys |= ReplayDuck;
    if (dash) keys |= ReplayDash;
    PublishReplayVirtualKeyMask(keys);
}

void PublishReplayButtons(uint64_t held, uint64_t pressed) {
    replayHeldButtons.store(held, std::memory_order_release);
    replayPressedButtons.store(pressed, std::memory_order_release);
}

#ifdef DLL6_MOVEMENT_ONLY
void InjectRawReplayKeyState(uint32_t previousKeys, uint32_t keys) {
    struct ReplayKey { uint32_t bit; WORD virtualKey; };
    constexpr ReplayKey replayKeys[]{
        {ReplayW, 'W'}, {ReplayS, 'S'}, {ReplayA, 'A'}, {ReplayD, 'D'},
        {ReplayJump, VK_SPACE}, {ReplayDuck, VK_LCONTROL},
        {ReplayDash, VK_LSHIFT},
    };
    for (const ReplayKey& key : replayKeys) {
        const bool wasDown = (previousKeys & key.bit) != 0;
        const bool isDown = (keys & key.bit) != 0;
        if (wasDown == isDown) continue;
        INPUT event{};
        event.type = INPUT_KEYBOARD;
        event.ki.wScan = static_cast<WORD>(MapVirtualKeyW(
            key.virtualKey, MAPVK_VK_TO_VSC));
        event.ki.dwFlags = KEYEVENTF_SCANCODE |
            (isDown ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP));
        SendInput(1, &event, sizeof(event));
    }
    replayVirtualKeys.store(keys, std::memory_order_release);
}

DWORD WINAPI LocalRawReplayThreadProc(LPVOID) {
    std::vector<LocalRawKeyEvent> events;
    {
        std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
        events = localRawKeyEvents;
    }
    uint32_t keys = 0;
    for (const LocalRawKeyEvent& event : events) {
        while (!localRawReplayStop.load(std::memory_order_acquire)) {
            const ULONGLONG elapsed =
                GetTickCount64() - localRawReplayStartedAt;
            if (elapsed >= event.milliseconds) break;
            const ULONGLONG remaining = event.milliseconds - elapsed;
            if (remaining > 2) Sleep(1);
            else YieldProcessor();
        }
        if (localRawReplayStop.load(std::memory_order_acquire)) break;
        const uint32_t previous = keys;
        if (event.pressed) keys |= event.keyBit;
        else keys &= ~event.keyBit;
        InjectRawReplayKeyState(previous, keys);
    }
    InjectRawReplayKeyState(keys, 0);
    return 0;
}

void StopLocalRawInputReplay() {
    localRawReplayStop.store(true, std::memory_order_release);
    if (localRawReplayThread) {
        WaitForSingleObject(localRawReplayThread, 2000);
        CloseHandle(localRawReplayThread);
        localRawReplayThread = nullptr;
    }
    InjectRawReplayKeyState(localRawCommandKeys, 0);
    localRawCommandKeys = 0;
}

void StartLocalRawInputReplay() {
    StopLocalRawInputReplay();
    {
        std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
        if (localRawKeyEvents.empty()) return;
    }
    // Input is advanced synchronously from PrepareLocalMovementPlaybackInput
    // using client-relative ticks. A wall-clock worker allowed the same event
    // to fall on neighboring simulation ticks in different runs.
    localRawReplayStop.store(false, std::memory_order_release);
    localRawCommandKeys = 0;
}

bool HasLocalRawKeyEvents() {
    std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
    return !localRawKeyEvents.empty();
}
#endif

bool IsFinite(const Vector3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

#ifdef DLL6_MOVEMENT_ONLY
bool IsUsableLocalMovementPosition(const Vector3& value) {
    return IsFinite(value) &&
           std::fabs(value.x) < 100000.0f &&
           std::fabs(value.y) < 100000.0f &&
           std::fabs(value.z) < 100000.0f &&
           (std::fabs(value.x) > 0.01f ||
            std::fabs(value.y) > 0.01f ||
            std::fabs(value.z) > 0.01f);
}

Vector3 ReadLocalMovementPosition() {
    Vector3 position{};
    if (currentLocalPawn) {
        // m_vOldOrigin is a replicated snapshot and can stay frozen for many
        // commands before jumping forward.  The local pawn's node-to-world
        // transform is prediction-updated and is the position actually used
        // by the renderer, so recording and playback must both use it.
        if (GetEntityRenderTransformPosition(currentLocalPawn, position) &&
            IsUsableLocalMovementPosition(position))
            return position;
        if (GetEntityRenderPosition(currentLocalPawn, position) &&
            IsUsableLocalMovementPosition(position))
            return position;
        if (GetEntityPosition(currentLocalPawn, position) &&
            IsUsableLocalMovementPosition(position))
            return position;
    }
    return currentLocalPosition;
}
#endif

const char* MovementPhase(const Vector3& velocity, float horizontalSpeed) {
    if (velocity.z > 120.0f) return "rising";
    if (velocity.z < -120.0f) return "falling";
    if (horizontalSpeed > 900.0f) return "burst";
    if (horizontalSpeed > 80.0f) return "moving";
    return "idle";
}

bool EqualsAsciiInsensitive(const std::string& left, const char* right) {
    if (!right || left.size() != std::strlen(right)) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i]))) return false;
    }
    return true;
}

uint32_t ParseBotInputLabel(const std::string& message) {
    if (message.empty() || message.size() > 24) return 0;
    std::string upper;
    upper.reserve(message.size());
    for (const unsigned char character : message) {
        if (character >= 'a' && character <= 'z')
            upper.push_back(static_cast<char>(character - 'a' + 'A'));
        else
            upper.push_back(static_cast<char>(character));
    }
    const auto compactKey = [&upper](char key) {
        bool found = false;
        for (const char character : upper) {
            if (character == key && !found) {
                found = true;
                continue;
            }
            if (character != '_' && character != '-' && character != ' ' &&
                character != '\t' && character != '.') return false;
        }
        return found;
    };
    uint32_t keys = 0;
    if (compactKey('W')) keys |= ReplayW;
    if (compactKey('A')) keys |= ReplayA;
    if (compactKey('S')) keys |= ReplayS;
    if (compactKey('D')) keys |= ReplayD;
    if (upper.find("JUMP") != std::string::npos || upper == "SPACE")
        keys |= ReplayJump;
    if (upper.find("DUCK") != std::string::npos || upper == "CTRL")
        keys |= ReplayDuck;
    if (upper.find("DASH") != std::string::npos || upper == "SHIFT")
        keys |= ReplayDash;
    return keys;
}

bool ReadBotInputMessage(uintptr_t entity, uintptr_t offset,
                         std::string& message) {
    message.clear();
    message.reserve(32);
    for (uintptr_t i = 0; i < 64; ++i) {
        const unsigned char character = Read<unsigned char>(entity + offset + i);
        if (!character) break;
        if (character == ',' || character == '\r' || character == '\n')
            message.push_back(' ');
        else if (character >= 32 && character < 127)
            message.push_back(static_cast<char>(character));
        else {
            message.clear();
            return false;
        }
    }
    if (message.empty()) return false;
    return ParseBotInputLabel(message) != 0;
}

uintptr_t ResolveMovementField(const char* fieldName) {
    constexpr const char* classes[]{
        "CCitadelPlayer_MovementServices",
        "CPlayer_MovementServices_Humanoid",
        "CPlayer_MovementServices",
    };
    for (const char* className : classes) {
        const uintptr_t offset = ResolveRuntimeSchemaOffset(className, fieldName);
        if (offset) return offset;
    }
    return 0;
}

struct MovementProbeOffsets {
    uintptr_t services{};
    uintptr_t attachZipLine{};
    uintptr_t attachedZipLineLane{};
    uintptr_t droppedFromZipline{};
    uintptr_t timeStartZipping{};
    uintptr_t timeStopZipping{};
    uintptr_t lastTimeOnZipLine{};
    uintptr_t isDashing{};
    uintptr_t dashJump{};
    uintptr_t inSlideJump{};
    uintptr_t wantsSlide{};
    uintptr_t crouching{};
    uintptr_t inAir{};
    uintptr_t sprinting{};
    uintptr_t dashStartTime{};
    uintptr_t dashEndTime{};
    uintptr_t dashJumpStartTime{};
    uintptr_t dashJumpEndTime{};
    uintptr_t dashDirection{};
    uintptr_t groundEntity{};
};

MovementProbeOffsets& GetMovementProbeOffsets() {
    static MovementProbeOffsets offsets{};
    static bool resolved = false;
    if (resolved) return offsets;
    // Resolve once while the initial client schema is stable. Private fields
    // legitimately return zero; retrying them forever races SchemaSystem and
    // scene teardown during level transitions.
    resolved = true;
    offsets.services = ResolveRuntimeSchemaOffset(
        "C_BasePlayerPawn", "m_pMovementServices");
    if (!offsets.services) offsets.services = 0xF28;
    offsets.attachZipLine = ResolveMovementField("m_hAttachZipLine");
    offsets.attachedZipLineLane = ResolveMovementField("m_iAttachedZipLineLane");
    offsets.droppedFromZipline = ResolveMovementField("m_bDroppedFromZipline");
    offsets.timeStartZipping = ResolveMovementField("m_flTimeStartZipping");
    offsets.timeStopZipping = ResolveMovementField("m_flTimeStopZipping");
    offsets.lastTimeOnZipLine = ResolveMovementField("m_flLastTimeOnZipLine");
    offsets.isDashing = ResolveMovementField("m_bIsDashing");
    offsets.dashJump = ResolveMovementField("m_bDashJump");
    offsets.inSlideJump = ResolveMovementField("m_bInSlideJump");
    offsets.wantsSlide = ResolveMovementField("m_bWantsSlide");
    offsets.crouching = ResolveMovementField("m_bCrouching");
    offsets.inAir = ResolveMovementField("m_bInAir");
    offsets.sprinting = ResolveMovementField("m_bSprinting");
    offsets.dashStartTime = ResolveMovementField("m_flDashStartTime");
    offsets.dashEndTime = ResolveMovementField("m_flDashEndTime");
    offsets.dashJumpStartTime = ResolveMovementField("m_flDashJumpStartTime");
    offsets.dashJumpEndTime = ResolveMovementField("m_flDashJumpEndTime");
    offsets.dashDirection = ResolveMovementField("m_vDashDirection");
    offsets.groundEntity = ResolveMovementField("m_hGroundEntity");

    std::ofstream log(Dll6Paths::DataFileA("movement_probe_schema.log"),
                      std::ios::trunc);
    if (log) {
        log << std::hex
            << "services=0x" << offsets.services << '\n'
            << "attach_zip=0x" << offsets.attachZipLine << '\n'
            << "zip_lane=0x" << offsets.attachedZipLineLane << '\n'
            << "dropped_zip=0x" << offsets.droppedFromZipline << '\n'
            << "zip_start=0x" << offsets.timeStartZipping << '\n'
            << "zip_stop=0x" << offsets.timeStopZipping << '\n'
            << "last_zip=0x" << offsets.lastTimeOnZipLine << '\n'
            << "is_dashing=0x" << offsets.isDashing << '\n'
            << "dash_jump=0x" << offsets.dashJump << '\n'
            << "slide_jump=0x" << offsets.inSlideJump << '\n'
            << "wants_slide=0x" << offsets.wantsSlide << '\n'
            << "crouching=0x" << offsets.crouching << '\n'
            << "in_air=0x" << offsets.inAir << '\n'
            << "sprinting=0x" << offsets.sprinting << '\n'
            << "dash_start=0x" << offsets.dashStartTime << '\n'
            << "dash_end=0x" << offsets.dashEndTime << '\n'
            << "dash_jump_start=0x" << offsets.dashJumpStartTime << '\n'
            << "dash_jump_end=0x" << offsets.dashJumpEndTime << '\n'
            << "dash_direction=0x" << offsets.dashDirection << '\n'
            << "ground_entity=0x" << offsets.groundEntity << '\n';
    }
    return offsets;
}

template <typename T>
T ReadMovementValue(uintptr_t services, uintptr_t offset, T fallback = {}) {
    return services && offset ? Read<T>(services + offset) : fallback;
}

bool ReadPacketSnapshotUnsafe(PacketEntitySnapshot& snapshot) {
    const uintptr_t entity = packetTrackedBot2.load(std::memory_order_acquire);
    if (!entity || Read<uint32_t>(entity + Offsets::HeroComponent +
                                  Offsets::HeroSpawnedId) != kHazeId)
        return false;

    snapshot.entity = entity;
    snapshot.handle = packetTrackedBot2Handle.load(std::memory_order_acquire);
    snapshot.networkOrigin = Read<Vector3>(entity + Offsets::Pos);
    snapshot.velocity = Read<Vector3>(entity + Offsets::Velocity);

    const uintptr_t sceneNode = Read<uintptr_t>(
        entity + Offsets::GameSceneNode);
    if (sceneNode) {
        snapshot.absoluteOrigin = Read<Vector3>(
            sceneNode + Offsets::SceneNodeAbsOrigin);
        snapshot.renderOrigin = Read<Vector3>(
            sceneNode + Offsets::SceneNodeRenderOrigin);
        snapshot.nodeToWorldOrigin = Read<Vector3>(sceneNode + 0x10);
    }

    const uintptr_t eyeAngles = packetEyeAnglesOffset.load(
        std::memory_order_acquire);
    if (eyeAngles) snapshot.eyeAngles = Read<Vector3>(entity + eyeAngles);

    const uintptr_t graphManager = Read<uintptr_t>(
        entity + Offsets::GraphControllerManager);
    const uintptr_t graph = graphManager
        ? Read<uintptr_t>(graphManager + Offsets::MainGraphController) : 0;
    if (graph) {
        snapshot.animationSequence = Read<int>(
            graph + Offsets::AnimSequence);
        snapshot.animationStartTime = Read<float>(
            graph + Offsets::AnimSequenceStartTime);
    }

    const MovementProbeOffsets& offsets = GetMovementProbeOffsets();
    const uintptr_t services = offsets.services
        ? Read<uintptr_t>(entity + offsets.services) : 0;
    snapshot.attachZipline = ReadMovementValue<uint32_t>(
        services, offsets.attachZipLine, 0xFFFFFFFFu);
    snapshot.ziplineLane = ReadMovementValue<int>(
        services, offsets.attachedZipLineLane, -1);
    snapshot.droppedZipline = ReadMovementValue<uint8_t>(
        services, offsets.droppedFromZipline) != 0;
    snapshot.zipStart = ReadMovementValue<float>(
        services, offsets.timeStartZipping);
    snapshot.zipStop = ReadMovementValue<float>(
        services, offsets.timeStopZipping);
    snapshot.lastZip = ReadMovementValue<float>(
        services, offsets.lastTimeOnZipLine);
    snapshot.isDashing = ReadMovementValue<uint8_t>(
        services, offsets.isDashing) != 0;
    snapshot.dashJump = ReadMovementValue<uint8_t>(
        services, offsets.dashJump) != 0;
    snapshot.slideJump = ReadMovementValue<uint8_t>(
        services, offsets.inSlideJump) != 0;
    snapshot.wantsSlide = ReadMovementValue<uint8_t>(
        services, offsets.wantsSlide) != 0;
    snapshot.crouching = ReadMovementValue<uint8_t>(
        services, offsets.crouching) != 0;
    snapshot.inAir = ReadMovementValue<uint8_t>(
        services, offsets.inAir) != 0;
    snapshot.sprinting = ReadMovementValue<uint8_t>(
        services, offsets.sprinting) != 0;
    snapshot.dashStart = ReadMovementValue<float>(
        services, offsets.dashStartTime);
    snapshot.dashEnd = ReadMovementValue<float>(
        services, offsets.dashEndTime);
    snapshot.dashJumpStart = ReadMovementValue<float>(
        services, offsets.dashJumpStartTime);
    snapshot.dashJumpEnd = ReadMovementValue<float>(
        services, offsets.dashJumpEndTime);
    snapshot.dashDirection = ReadMovementValue<Vector3>(
        services, offsets.dashDirection);
    snapshot.groundEntity = ReadMovementValue<uint32_t>(
        services, offsets.groundEntity, 0xFFFFFFFFu);
    return IsFinite(snapshot.networkOrigin) &&
        IsFinite(snapshot.absoluteOrigin) && IsFinite(snapshot.velocity);
}

bool TryReadPacketSnapshot(PacketEntitySnapshot& snapshot) {
    __try {
        return ReadPacketSnapshotUnsafe(snapshot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#ifdef DLL6_MOVEMENT_ONLY
uint32_t ReplayVirtualKeysFromButtons(uint64_t buttons) {
    uint32_t keys = 0;
    if (buttons & static_cast<uint64_t>(InputBitMask::Forward)) keys |= ReplayW;
    if (buttons & static_cast<uint64_t>(InputBitMask::Back)) keys |= ReplayS;
    if (buttons & static_cast<uint64_t>(InputBitMask::MoveLeft)) keys |= ReplayA;
    if (buttons & static_cast<uint64_t>(InputBitMask::MoveRight)) keys |= ReplayD;
    if (buttons & static_cast<uint64_t>(InputBitMask::Jump)) keys |= ReplayJump;
    if (buttons & static_cast<uint64_t>(InputBitMask::Duck)) keys |= ReplayDuck;
    if (buttons & static_cast<uint64_t>(InputBitMask::Speed)) keys |= ReplayDash;
    return keys;
}

size_t SpatiallyGatedReplayInputIndex(size_t desiredIndex) {
    if (desiredIndex == 0 || desiredIndex >= localCommandFrames.size())
        return desiredIndex;

    // Find the latest physical-key transition that led to the desired state.
    // Continue gating that transition even if phase sync has moved several
    // frames beyond it.
    size_t transitionIndex = desiredIndex;
    while (transitionIndex > 0 &&
           localCommandFrames[transitionIndex].virtualKeys ==
               localCommandFrames[transitionIndex - 1].virtualKeys)
        --transitionIndex;
    if (transitionIndex == 0) return desiredIndex;

    const LocalCommandFrame& transition =
        localCommandFrames[transitionIndex];
    const float speed = std::sqrt(
        transition.velocity.x * transition.velocity.x +
        transition.velocity.y * transition.velocity.y +
        transition.velocity.z * transition.velocity.z);
    // Stationary startup actions (notably holding Space to acquire a zipline)
    // must remain timeline driven because no spatial direction exists yet.
    if (speed < 100.0f) return desiredIndex;

    const Vector3 actual = ReadLocalMovementPosition();
    const Vector3 delta{
        actual.x - transition.position.x,
        actual.y - transition.position.y,
        actual.z - transition.position.z};
    const float signedProgress =
        (delta.x * transition.velocity.x +
         delta.y * transition.velocity.y +
         delta.z * transition.velocity.z) / speed;
    // Stay on the pre-transition input state until the pawn reaches the same
    // plane along the recorded direction of travel. A small negative margin
    // avoids a one-tick early Ctrl/Space/Shift caused by phase prediction.
    return signedProgress < -1.0f ? transitionIndex - 1 : desiredIndex;
}

void StopLocalPlayback() {
    StopLocalRawInputReplay();
    localMovementPlaybackActive = false;
    localMovementPlaybackCalibrating = false;
    movementReplayActive = false;
    movementReplayCalibrating = false;
    localPlaybackIndex = 0;
    localCalibrationReachedAt = 0;
    localPlaybackStartClientTick = 0;
    localPlaybackCurrentRelativeTick = 0;
    localPlaybackPhaseOffsetTicks = 0;
    localPlaybackLastInputFrame = SIZE_MAX;
    PublishReplayVirtualKeyMask(0);
    PublishReplayButtons(0, 0);
}

void SaveLocalMovementRecording() {
    if (localCommandFrames.empty()) return;
    std::ofstream output(
        Dll6Paths::DataFileA("movement_local_recording.csv"),
        std::ios::trunc);
    if (!output) return;
    output << "frame,relative_tick,time_ms,x,y,z,vx,vy,vz,pitch,yaw,roll,"
              "cmd_pitch,cmd_yaw,cmd_roll,view_pitch,view_yaw,view_roll,"
              "virtual_keys,"
              "forward,left,up,native_held,native_changed,native_scroll,"
              "pb_held,pb_changed,pb_scroll,ground_entity,attach_zipline,"
              "in_air,is_dashing,dash_jump,slide_jump,wants_slide,crouching,"
              "sprinting,subticks\n";
    output << std::setprecision(9);
    for (size_t index = 0; index < localCommandFrames.size(); ++index) {
        const LocalCommandFrame& frame = localCommandFrames[index];
        output << index << ',' << frame.relativeTick << ','
               << frame.milliseconds << ',' << frame.position.x << ','
               << frame.position.y << ',' << frame.position.z << ','
               << frame.velocity.x << ',' << frame.velocity.y << ','
               << frame.velocity.z << ',' << frame.camera.x << ','
               << frame.camera.y << ',' << frame.camera.z << ','
               << frame.commandAngles.x << ',' << frame.commandAngles.y << ','
               << frame.commandAngles.z << ',' << frame.viewAngles.x << ','
               << frame.viewAngles.y << ',' << frame.viewAngles.z << ','
               << frame.virtualKeys << ','
               << frame.forward << ',' << frame.left << ',' << frame.up << ','
               << frame.nativeHeld << ',' << frame.nativeChanged << ','
               << frame.nativeScroll << ',' << frame.protobufHeld << ','
               << frame.protobufChanged << ',' << frame.protobufScroll << ','
               << frame.groundEntity << ',' << frame.attachZipline << ','
               << frame.inAir << ',' << frame.isDashing << ','
               << frame.dashJump << ',' << frame.slideJump << ','
               << frame.wantsSlide << ',' << frame.crouching << ','
               << frame.sprinting << ',';
        for (size_t stepIndex = 0; stepIndex < frame.subticks.size(); ++stepIndex) {
            if (stepIndex) output << ';';
            const LocalSubtickFrame& step = frame.subticks[stepIndex];
            output << step.button << ':' << step.pressed << ':' << step.when
                   << ':' << step.analogForwardDelta << ':'
                   << step.analogLeftDelta << ':' << step.pitchDelta << ':'
                   << step.yawDelta;
        }
        if (frame.subticks.empty()) output << '-';
        output << '\n';
    }
    {
        // Normalize transitions to the finalized command timeline. Keyboard
        // polling can report a physical change one command later than the
        // native usercmd that actually consumed it.
        std::vector<LocalRawKeyEvent> normalizedEvents;
        uint32_t previousKeys = 0;
        constexpr uint32_t keyBits[]{
            ReplayW, ReplayS, ReplayA, ReplayD,
            ReplayJump, ReplayDuck, ReplayDash};
        for (const LocalCommandFrame& frame : localCommandFrames) {
            const uint32_t changed = previousKeys ^ frame.virtualKeys;
            for (const uint32_t bit : keyBits) {
                if (changed & bit) {
                    normalizedEvents.push_back({
                        frame.milliseconds, bit,
                        (frame.virtualKeys & bit) != 0});
                }
            }
            previousKeys = frame.virtualKeys;
        }
        std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
        localRawKeyEvents = std::move(normalizedEvents);
    }
    {
        std::ofstream rawOutput(
            Dll6Paths::DataFileA("movement_local_raw_input.csv"),
            std::ios::trunc);
        if (rawOutput) {
            rawOutput << "time_ms,key_bit,pressed\n";
            std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
            for (const LocalRawKeyEvent& event : localRawKeyEvents)
                rawOutput << event.milliseconds << ',' << event.keyBit << ','
                          << event.pressed << '\n';
        }
    }
    localMovementRecordingReady = !localCommandFrames.empty();
}

bool LoadLocalMovementRecording() {
    if (localRecordingLoaded) return localMovementRecordingReady;
    localRecordingLoaded = true;
    std::ifstream input(
        Dll6Paths::DataFileA("movement_local_recording.csv"));
    if (!input) return false;
    std::string line;
    std::getline(input, line);
    std::vector<LocalCommandFrame> frames;
    while (std::getline(input, line)) {
        std::stringstream row(line);
        std::array<std::string, 38> columns{};
        bool complete = true;
        for (std::string& column : columns) {
            if (!std::getline(row, column, ',')) {
                complete = false;
                break;
            }
        }
        if (!complete) continue;
        try {
            LocalCommandFrame frame{};
            frame.relativeTick = static_cast<uint32_t>(std::stoul(columns[1]));
            frame.milliseconds = static_cast<uint32_t>(std::stoul(columns[2]));
            frame.position = {std::stof(columns[3]), std::stof(columns[4]),
                              std::stof(columns[5])};
            frame.velocity = {std::stof(columns[6]), std::stof(columns[7]),
                              std::stof(columns[8])};
            frame.camera = {std::stof(columns[9]), std::stof(columns[10]),
                            std::stof(columns[11])};
            frame.commandAngles = {std::stof(columns[12]),
                                   std::stof(columns[13]),
                                   std::stof(columns[14])};
            frame.viewAngles = {std::stof(columns[15]),
                                std::stof(columns[16]),
                                std::stof(columns[17])};
            frame.virtualKeys = static_cast<uint32_t>(std::stoul(columns[18]));
            frame.forward = std::stof(columns[19]);
            frame.left = std::stof(columns[20]);
            frame.up = std::stof(columns[21]);
            frame.nativeHeld = std::stoull(columns[22]);
            frame.nativeChanged = std::stoull(columns[23]);
            frame.nativeScroll = std::stoull(columns[24]);
            frame.protobufHeld = std::stoull(columns[25]);
            frame.protobufChanged = std::stoull(columns[26]);
            frame.protobufScroll = std::stoull(columns[27]);
            frame.groundEntity = static_cast<uint32_t>(std::stoul(columns[28]));
            frame.attachZipline = static_cast<uint32_t>(std::stoul(columns[29]));
            frame.inAir = std::stoi(columns[30]) != 0;
            frame.isDashing = std::stoi(columns[31]) != 0;
            frame.dashJump = std::stoi(columns[32]) != 0;
            frame.slideJump = std::stoi(columns[33]) != 0;
            frame.wantsSlide = std::stoi(columns[34]) != 0;
            frame.crouching = std::stoi(columns[35]) != 0;
            frame.sprinting = std::stoi(columns[36]) != 0;
            std::stringstream steps(columns[37]);
            std::string encoded;
            while (std::getline(steps, encoded, ';')) {
                if (encoded.empty() || encoded == "-") continue;
                std::stringstream values(encoded);
                std::array<std::string, 7> parts{};
                bool stepComplete = true;
                for (std::string& part : parts) {
                    if (!std::getline(values, part, ':')) {
                        stepComplete = false;
                        break;
                    }
                }
                if (!stepComplete) continue;
                frame.subticks.push_back({
                    std::stoull(parts[0]), std::stoi(parts[1]) != 0,
                    std::stof(parts[2]), std::stof(parts[3]),
                    std::stof(parts[4]), std::stof(parts[5]),
                    std::stof(parts[6])});
            }
            if (!IsFinite(frame.position) || !IsFinite(frame.velocity) ||
                !IsFinite(frame.camera) || !IsFinite(frame.commandAngles) ||
                !IsFinite(frame.viewAngles))
                continue;
            frames.push_back(std::move(frame));
        } catch (...) {
        }
    }
    if (frames.empty()) return false;
    localCommandFrames = std::move(frames);
    {
        std::ifstream rawInput(
            Dll6Paths::DataFileA("movement_local_raw_input.csv"));
        std::string rawLine;
        std::getline(rawInput, rawLine);
        std::vector<LocalRawKeyEvent> events;
        while (std::getline(rawInput, rawLine)) {
            std::stringstream row(rawLine);
            std::array<std::string, 3> columns{};
            if (!std::getline(row, columns[0], ',') ||
                !std::getline(row, columns[1], ',') ||
                !std::getline(row, columns[2], ','))
                continue;
            try {
                events.push_back({
                    static_cast<uint32_t>(std::stoul(columns[0])),
                    static_cast<uint32_t>(std::stoul(columns[1])),
                    std::stoi(columns[2]) != 0});
            } catch (...) {
            }
        }
        // Always rebuild the active replay stream from finalized command
        // states. This also upgrades recordings made by the earlier polling-
        // timestamp implementation without requiring another route capture.
        events.clear();
        uint32_t previousKeys = 0;
        constexpr uint32_t keyBits[]{
            ReplayW, ReplayS, ReplayA, ReplayD,
            ReplayJump, ReplayDuck, ReplayDash};
        for (const LocalCommandFrame& frame : localCommandFrames) {
            const uint32_t changed = previousKeys ^ frame.virtualKeys;
            for (const uint32_t bit : keyBits) {
                if (changed & bit) {
                    events.push_back({
                        frame.milliseconds, bit,
                        (frame.virtualKeys & bit) != 0});
                }
            }
            previousKeys = frame.virtualKeys;
        }
        std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
        localRawKeyEvents = std::move(events);
    }
    localMovementRecordingReady = true;
    return true;
}

void CaptureLocalMovementCommand(CUserCmd* command) {
    if (!command || !command->cmd.has_base()) return;
    const auto& base = command->cmd.base();
    const int32_t clientTick = base.has_client_tick()
        ? base.client_tick() : localLastRecordedTick + 1;
    if (clientTick == localLastRecordedTick) return;
    localLastRecordedTick = clientTick;
    LocalCommandFrame frame{};
    frame.relativeTick = static_cast<uint32_t>((std::max)(
        0, clientTick - localRecordingStartTick));
    frame.milliseconds = static_cast<uint32_t>(
        (static_cast<uint64_t>(frame.relativeTick) * 1000) / kReplayTickRate);
    if (currentLocalPawn) {
        frame.position = ReadLocalMovementPosition();
        frame.velocity = Read<Vector3>(currentLocalPawn + Offsets::Velocity);
    } else {
        frame.position = currentLocalPosition;
    }
    if (GetMovementReplayCameraAngles(frame.camera)) {
        // Captured from the actual rendered gameplay camera.
    } else if (command->cmd.has_ang_camera_angles()) {
        const auto& angle = command->cmd.ang_camera_angles();
        frame.camera = {angle.x(), angle.y(), angle.z()};
    } else if (base.has_viewangles()) {
        const auto& angle = base.viewangles();
        frame.camera = {angle.x(), angle.y(), angle.z()};
    }
    if (command->cmd.has_ang_camera_angles()) {
        const auto& angle = command->cmd.ang_camera_angles();
        frame.commandAngles = {angle.x(), angle.y(), angle.z()};
    } else {
        frame.commandAngles = frame.camera;
    }
    if (base.has_viewangles()) {
        const auto& angle = base.viewangles();
        frame.viewAngles = {angle.x(), angle.y(), angle.z()};
    } else {
        frame.viewAngles = frame.commandAngles;
    }
    const auto keyDown = [](int key) {
        return (GetAsyncKeyState(key) & 0x8000) != 0;
    };
    if (keyDown('W')) frame.virtualKeys |= ReplayW;
    if (keyDown('S')) frame.virtualKeys |= ReplayS;
    if (keyDown('A')) frame.virtualKeys |= ReplayA;
    if (keyDown('D')) frame.virtualKeys |= ReplayD;
    if (keyDown(VK_SPACE)) frame.virtualKeys |= ReplayJump;
    if (keyDown(VK_CONTROL) || keyDown(VK_LCONTROL) ||
        keyDown(VK_RCONTROL)) frame.virtualKeys |= ReplayDuck;
    if (keyDown(VK_SHIFT) || keyDown(VK_LSHIFT) ||
        keyDown(VK_RSHIFT)) frame.virtualKeys |= ReplayDash;
    frame.forward = base.forwardmove();
    frame.left = base.leftmove();
    frame.up = base.upmove();
    frame.nativeHeld = command->buttonStates.buttonState1;
    frame.nativeChanged = command->buttonStates.buttonState2;
    frame.nativeScroll = command->buttonStates.buttonState3;
    if (base.has_buttons_pb()) {
        const auto& buttons = base.buttons_pb();
        frame.protobufHeld = buttons.buttonstate1();
        frame.protobufChanged = buttons.buttonstate2();
        frame.protobufScroll = buttons.buttonstate3();
    }
    frame.subticks.reserve(static_cast<size_t>(base.subtick_moves_size()));
    for (int index = 0; index < base.subtick_moves_size(); ++index) {
        const auto& step = base.subtick_moves(index);
        frame.subticks.push_back({
            step.button(), step.pressed(), step.when(),
            step.analog_forward_delta(), step.analog_left_delta(),
            step.pitch_delta(), step.yaw_delta()});
    }
    if (currentLocalPawn) {
        const MovementProbeOffsets& offsets = GetMovementProbeOffsets();
        const uintptr_t services = offsets.services
            ? Read<uintptr_t>(currentLocalPawn + offsets.services) : 0;
        frame.groundEntity = ReadMovementValue<uint32_t>(
            services, offsets.groundEntity, 0xFFFFFFFFu);
        frame.attachZipline = ReadMovementValue<uint32_t>(
            services, offsets.attachZipLine, 0xFFFFFFFFu);
        frame.inAir = ReadMovementValue<uint8_t>(services, offsets.inAir) != 0;
        frame.isDashing = ReadMovementValue<uint8_t>(
            services, offsets.isDashing) != 0;
        frame.dashJump = ReadMovementValue<uint8_t>(
            services, offsets.dashJump) != 0;
        frame.slideJump = ReadMovementValue<uint8_t>(
            services, offsets.inSlideJump) != 0;
        frame.wantsSlide = ReadMovementValue<uint8_t>(
            services, offsets.wantsSlide) != 0;
        frame.crouching = ReadMovementValue<uint8_t>(
            services, offsets.crouching) != 0;
        frame.sprinting = ReadMovementValue<uint8_t>(
            services, offsets.sprinting) != 0;
    }
    if (IsFinite(frame.position) && IsFinite(frame.camera) &&
        IsFinite(frame.commandAngles) && IsFinite(frame.viewAngles))
        localCommandFrames.push_back(std::move(frame));
    if (localCommandFrames.size() >= kReplayTickRate * 120u) {
        localMovementRecording = false;
        SaveLocalMovementRecording();
    }
}

bool ProcessLocalMovementRecorderReplay(CUserCmd* command, uintptr_t input) {
    LoadLocalMovementRecording();
    if (!command || !command->cmd.has_base()) return false;
    const bool recordDown = !AreCustomBindsSuppressed() && localMovementRecordKey > 0 &&
        (GetAsyncKeyState(localMovementRecordKey) & 0x8000) != 0;
    const bool recordPressed = recordDown && !localRecordKeyWasDown;
    localRecordKeyWasDown = recordDown;
    if (recordPressed) {
        if (localMovementRecording) {
            localMovementRecording = false;
            SaveLocalMovementRecording();
        } else {
            StopLocalPlayback();
            localCommandFrames.clear();
            {
                std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
                localRawKeyEvents.clear();
            }
            localRawRecordedKeys = 0;
            localMovementRecordingReady = false;
            localMovementRecording = true;
            localRecordingStartedAt = GetTickCount64();
            const auto& base = command->cmd.base();
            localRecordingStartTick = base.has_client_tick()
                ? base.client_tick() + 1 : 0;
            localLastRecordedTick = INT32_MIN;
        }
        return false;
    }
    if (localMovementRecording) {
        CaptureLocalMovementCommand(command);
        return false;
    }

    const bool replayDown = !AreCustomBindsSuppressed() && movementReplayKey > 0 &&
        (GetAsyncKeyState(movementReplayKey) & 0x8000) != 0;
    const bool replayPressed = replayDown && !localReplayKeyWasDown;
    localReplayKeyWasDown = replayDown;
    if (replayPressed) {
        if (localMovementPlaybackActive || localMovementPlaybackCalibrating) {
            StopLocalPlayback();
        } else if (localMovementRecordingReady && currentLocalPositionReady) {
            const Vector3 actualPosition = ReadLocalMovementPosition();
            const float distance = Distance3D(
                actualPosition, localCommandFrames.front().position);
            if (distance <= kReplayStartTolerance) {
                localMovementPlaybackCalibrating = true;
                movementReplayCalibrating = true;
                localCalibrationReachedAt = 0;
                localPlaybackIndex = 0;
            }
        }
    }

    if (!localMovementPlaybackActive && !localMovementPlaybackCalibrating) {
        PublishReplayVirtualKeyMask(0);
        PublishReplayButtons(0, 0);
        return false;
    }

    const LocalCommandFrame& first = localCommandFrames.front();
    if (localMovementPlaybackCalibrating) {
        const Vector3 actualPosition = ReadLocalMovementPosition();
        const float dx = first.position.x - actualPosition.x;
        const float dy = first.position.y - actualPosition.y;
        const float dz = first.position.z - actualPosition.z;
        const float distance2d = std::hypot(dx, dy);
        const Vector3 velocity = currentLocalPawn
            ? Read<Vector3>(currentLocalPawn + Offsets::Velocity)
            : Vector3{};
        const float horizontalSpeed = std::hypot(velocity.x, velocity.y);
        float forward = 0.0f;
        float left = 0.0f;
        if (distance2d > kLocalCalibrationPositionTolerance) {
            // Citadel ignores command magnitudes below roughly 0.15. Near the
            // target, use a minimum effective pulse only while almost still,
            // then release all directions and let friction stop the pawn. Do
            // not reverse immediately after crossing START: that was the
            // source of the old endless oscillation.
            const bool nearTarget = distance2d <= 6.0f;
            const bool coastNearTarget = nearTarget &&
                horizontalSpeed > kLocalCalibrationSettleSpeed;
            if (!coastNearTarget) {
                const float worldYaw =
                    std::atan2(dy, dx) * 57.29577951308232f;
                const float delta = NormalizeAngle(
                    worldYaw - first.viewAngles.y) * kDegreesToRadians;
                const float calibrationSpeed = (std::clamp)(
                    distance2d / 48.0f, 0.16f, 0.40f);
                forward = std::cos(delta) * calibrationSpeed;
                left = std::sin(delta) * calibrationSpeed;
            }
            localCalibrationReachedAt = 0;
        } else if (std::fabs(dz) <= 48.0f) {
            if (horizontalSpeed > kLocalCalibrationSettleSpeed) {
                // No counter-steering here. Releasing movement produces a
                // smaller and more repeatable final step than a reverse key.
                localCalibrationReachedAt = 0;
            } else if (!localCalibrationReachedAt) {
                localCalibrationReachedAt = GetTickCount64();
            }
            if (localCalibrationReachedAt &&
                GetTickCount64() - localCalibrationReachedAt >= 180) {
                localMovementPlaybackCalibrating = false;
                movementReplayCalibrating = false;
                localMovementPlaybackActive = true;
                movementReplayActive = true;
                localPlaybackIndex = 0;
                const auto& calibrationBase = command->cmd.base();
                localPlaybackStartClientTick = calibrationBase.has_client_tick()
                    ? calibrationBase.client_tick() + 1 : 0;
                localPlaybackCurrentRelativeTick = 0;
                localPlaybackPhaseOffsetTicks = 0;
                localPlaybackLastInputFrame = SIZE_MAX;
                StartLocalRawInputReplay();
                std::ofstream replayLog(
                    Dll6Paths::DataFileA("movement_local_replay.log"),
                    std::ios::trunc);
                if (replayLog)
                    replayLog << "frame,actual_x,actual_y,actual_z,reference_x,"
                                 "reference_y,reference_z,deviation,available_subticks,"
                                 "recorded_subticks,phase_offset,effective_tick,"
                                 "input_frame\n";
            }
        }
        auto* camera = command->cmd.mutable_ang_camera_angles();
        camera->set_x(first.commandAngles.x);
        camera->set_y(first.commandAngles.y);
        camera->set_z(first.commandAngles.z);
        ApplyMovementReplayCameraAngles(first.camera);
        auto* base = command->cmd.mutable_base();
        auto* view = base->mutable_viewangles();
        view->set_x(first.viewAngles.x);
        view->set_y(first.viewAngles.y);
        view->set_z(first.viewAngles.z);
        base->set_forwardmove(forward);
        base->set_leftmove(left);
        base->set_upmove(0.0f);
        uint64_t held = 0;
        // Keep the matching digital direction present for the analog pulse.
        constexpr float kCalibrationDirectionEpsilon = 0.005f;
        if (forward > kCalibrationDirectionEpsilon)
            held |= static_cast<uint64_t>(InputBitMask::Forward);
        else if (forward < -kCalibrationDirectionEpsilon)
            held |= static_cast<uint64_t>(InputBitMask::Back);
        if (left > kCalibrationDirectionEpsilon)
            held |= static_cast<uint64_t>(InputBitMask::MoveLeft);
        else if (left < -kCalibrationDirectionEpsilon)
            held |= static_cast<uint64_t>(InputBitMask::MoveRight);
        ApplyReplayButtons(command, held, 0);
        PublishReplayVirtualKeyMask(ReplayVirtualKeysFromButtons(held));
        PublishReplayButtons(held, 0);
        return true;
    }

    const auto& playbackBase = command->cmd.base();
    if (playbackBase.has_client_tick() && localPlaybackStartClientTick) {
        const int32_t tickDelta = playbackBase.client_tick() -
                                  localPlaybackStartClientTick;
        if (tickDelta >= 0)
            localPlaybackCurrentRelativeTick =
                static_cast<uint32_t>(tickDelta);
    }
    // Deterministic spatial follower. The old accumulated phase offset made
    // the same route choose different command phases after server impulses.
    // Progress is now monotonic and derived from the pawn's actual position;
    // wall-clock time only bounds how far ahead we are allowed to inspect.
    localPlaybackPhaseOffsetTicks = 0;
    const uint32_t effectiveRelativeTick = localPlaybackCurrentRelativeTick;
    const bool rawInputMode = HasLocalRawKeyEvents();
    if (rawInputMode) {
        while (localPlaybackIndex + 1 < localCommandFrames.size() &&
               localCommandFrames[localPlaybackIndex + 1].relativeTick <=
                   effectiveRelativeTick)
            ++localPlaybackIndex;
    } else {
    const Vector3 phaseActual = ReadLocalMovementPosition();
    size_t searchEnd = localPlaybackIndex;
    const uint32_t lookAheadTick = effectiveRelativeTick + 32;
    while (searchEnd + 1 < localCommandFrames.size() &&
           localCommandFrames[searchEnd + 1].relativeTick <= lookAheadTick)
        ++searchEnd;

    size_t bestIndex = localPlaybackIndex;
    float bestDistance = Distance3D(
        phaseActual, localCommandFrames[bestIndex].position);
    for (size_t index = localPlaybackIndex + 1;
         index <= searchEnd; ++index) {
        const float distance = Distance3D(
            phaseActual, localCommandFrames[index].position);
        if (distance + 0.25f < bestDistance) {
            bestDistance = distance;
            bestIndex = index;
        }
    }
    if (bestIndex > localPlaybackIndex) {
        // Stop on (not beyond) the first input transition. Its spatial gate
        // below decides when that action is physically allowed to fire.
        for (size_t index = localPlaybackIndex + 1;
             index <= bestIndex; ++index) {
            const LocalCommandFrame& previous =
                localCommandFrames[index - 1];
            const LocalCommandFrame& candidate =
                localCommandFrames[index];
            if (candidate.virtualKeys != previous.virtualKeys ||
                candidate.nativeHeld != previous.nativeHeld) {
                bestIndex = index;
                break;
            }
        }
        localPlaybackIndex = bestIndex;
    } else if (localPlaybackIndex + 1 < localCommandFrames.size()) {
        const LocalCommandFrame& current =
            localCommandFrames[localPlaybackIndex];
        const LocalCommandFrame& next =
            localCommandFrames[localPlaybackIndex + 1];
        const bool nextIsDue = next.relativeTick <= effectiveRelativeTick;
        const bool stationaryReference =
            Distance3D(current.position, next.position) <= 0.5f;
        const float currentDistance = Distance3D(
            phaseActual, current.position);
        const float nextDistance = Distance3D(
            phaseActual, next.position);
        if (nextIsDue &&
            (stationaryReference || nextDistance <= currentDistance + 1.0f))
            ++localPlaybackIndex;
    }
    }
    if (localPlaybackIndex + 1 >= localCommandFrames.size() &&
        effectiveRelativeTick > localCommandFrames.back().relativeTick + 8) {
        StopLocalPlayback();
        return true;
    }
    const LocalCommandFrame& frame = localCommandFrames[localPlaybackIndex];
    const size_t gatedInputIndex = rawInputMode
        ? localPlaybackIndex
        : SpatiallyGatedReplayInputIndex(localPlaybackIndex);
    const LocalCommandFrame& inputFrame =
        localCommandFrames[gatedInputIndex];
    const bool newInputFrame =
        gatedInputIndex != localPlaybackLastInputFrame;
    auto* camera = command->cmd.mutable_ang_camera_angles();
    camera->set_x(frame.commandAngles.x);
    camera->set_y(frame.commandAngles.y);
    camera->set_z(frame.commandAngles.z);
    ApplyMovementReplayCameraAngles(frame.camera);
    auto* base = command->cmd.mutable_base();
    auto* view = base->mutable_viewangles();
    view->set_x(frame.viewAngles.x);
    view->set_y(frame.viewAngles.y);
    view->set_z(frame.viewAngles.z);
    const int availableSubticks = base->subtick_moves_size();
    int copiedSubticks = 0;
    if (!rawInputMode) {
        base->set_forwardmove(frame.forward);
        base->set_leftmove(frame.left);
        base->set_upmove(frame.up);
        command->buttonStates.buttonState1 = inputFrame.nativeHeld;
        command->buttonStates.buttonState2 =
            newInputFrame ? inputFrame.nativeChanged : 0;
        command->buttonStates.buttonState3 = inputFrame.nativeScroll;
        if (auto* buttons = base->mutable_buttons_pb()) {
            buttons->set_buttonstate1(inputFrame.protobufHeld);
            buttons->set_buttonstate2(
                newInputFrame ? inputFrame.protobufChanged : 0);
            buttons->set_buttonstate3(inputFrame.protobufScroll);
        }
        copiedSubticks = (std::min)(
            availableSubticks, static_cast<int>(inputFrame.subticks.size()));
        for (int index = 0; index < copiedSubticks; ++index) {
            auto* step = base->mutable_subtick_moves(index);
            const LocalSubtickFrame& saved = inputFrame.subticks[index];
            step->set_button(saved.button);
            step->set_pressed(saved.pressed);
            step->set_when(saved.when);
            step->set_analog_forward_delta(saved.analogForwardDelta);
            step->set_analog_left_delta(saved.analogLeftDelta);
            step->set_pitch_delta(saved.pitchDelta);
            step->set_yaw_delta(saved.yawDelta);
        }
        PublishReplayVirtualKeyMask(inputFrame.virtualKeys);
        PublishReplayButtons(
            inputFrame.nativeHeld,
            newInputFrame ? inputFrame.nativeChanged : 0);
        localPlaybackLastInputFrame = gatedInputIndex;
    } else {
        // The stock callback has converted the injected keys into Citadel's
        // authoritative held/change masks for this command. Forward those
        // exact masks to the final movement-service consumer.
        PublishReplayButtons(
            command->buttonStates.buttonState1,
            command->buttonStates.buttonState2);
    }
    static ULONGLONG lastLogAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (!lastLogAt || now - lastLogAt >= 100) {
        lastLogAt = now;
        std::ofstream log(
            Dll6Paths::DataFileA("movement_local_replay.log"),
            std::ios::app);
        if (log) {
            const Vector3 actual = ReadLocalMovementPosition();
            log << localPlaybackIndex << ',' << actual.x << ','
                << actual.y << ',' << actual.z << ',' << frame.position.x
                << ',' << frame.position.y << ',' << frame.position.z << ','
                << Distance3D(actual, frame.position) << ','
                << availableSubticks << ',' << inputFrame.subticks.size() << ','
                << localPlaybackPhaseOffsetTicks << ','
                << effectiveRelativeTick << ',' << gatedInputIndex << '\n';
        }
    }
    return true;
}
#endif

} // namespace

#ifdef DLL6_MOVEMENT_ONLY
void PrepareLocalMovementPlaybackInput() {
    if (!localMovementPlaybackActive)
        return;
    const uint32_t rawNextRelativeTick =
        localPlaybackCurrentRelativeTick + 1;
    if (HasLocalRawKeyEvents()) {
        const uint32_t targetMilliseconds = static_cast<uint32_t>(
            (static_cast<uint64_t>(rawNextRelativeTick) * 1000u) /
            kReplayTickRate);
        uint32_t keys = 0;
        {
            std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
            for (const LocalRawKeyEvent& event : localRawKeyEvents) {
                if (event.milliseconds > targetMilliseconds) break;
                if (event.pressed) keys |= event.keyBit;
                else keys &= ~event.keyBit;
            }
        }
        InjectRawReplayKeyState(localRawCommandKeys, keys);
        PublishReplayVirtualKeyMask(keys);
        localRawCommandKeys = keys;
        return;
    }
    if (localPlaybackIndex >= localCommandFrames.size()) {
        PublishReplayVirtualKeyMask(0);
        return;
    }
    size_t nextIndex = localPlaybackIndex;
    const int64_t adjustedNextRelativeTick =
        static_cast<int64_t>(rawNextRelativeTick) -
        static_cast<int64_t>(localPlaybackPhaseOffsetTicks);
    const uint32_t nextRelativeTick = static_cast<uint32_t>((std::max)(
        int64_t{0}, adjustedNextRelativeTick));
    while (nextIndex + 1 < localCommandFrames.size() &&
           localCommandFrames[nextIndex + 1].relativeTick <= nextRelativeTick)
        ++nextIndex;
    nextIndex = SpatiallyGatedReplayInputIndex(nextIndex);
    const LocalCommandFrame& frame = localCommandFrames[nextIndex];
    PublishReplayVirtualKeyMask(frame.virtualKeys);
}

void CaptureLocalMovementRawKeyEvent(int virtualKey, bool pressed) {
    if (!localMovementRecording || localMovementPlaybackActive ||
        localMovementPlaybackCalibrating)
        return;
    uint32_t bit = 0;
    switch (virtualKey) {
        case 'W': bit = ReplayW; break;
        case 'S': bit = ReplayS; break;
        case 'A': bit = ReplayA; break;
        case 'D': bit = ReplayD; break;
        case VK_SPACE: bit = ReplayJump; break;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: bit = ReplayDuck; break;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: bit = ReplayDash; break;
        default: return;
    }
    const bool wasPressed = (localRawRecordedKeys & bit) != 0;
    if (wasPressed == pressed) return;
    if (pressed) localRawRecordedKeys |= bit;
    else localRawRecordedKeys &= ~bit;
    const uint32_t milliseconds = static_cast<uint32_t>(
        GetTickCount64() - localRecordingStartedAt);
    std::lock_guard<std::mutex> lock(localRawKeyEventsMutex);
    localRawKeyEvents.push_back({milliseconds, bit, pressed});
}
#endif

void CaptureMovementPacketEntitySnapshot(uint64_t packetSequence,
                                         int32_t serverTick,
                                         int32_t deltaFrom,
                                         int32_t updatedEntries,
                                         uint32_t entityDataBytes,
                                         uint64_t entityDataHash) {
    if (!movementProbeEnabled ||
        !packetTrackedBot2.load(std::memory_order_acquire)) return;

    PacketEntitySnapshot snapshot{};
    snapshot.packetSequence = packetSequence;
    snapshot.serverTick = serverTick;
    snapshot.deltaFrom = deltaFrom;
    snapshot.updatedEntries = updatedEntries;
    snapshot.entityDataBytes = entityDataBytes;
    snapshot.entityDataHash = entityDataHash;
    snapshot.tickMilliseconds = GetTickCount64();
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    snapshot.performanceCounter = counter.QuadPart;
    if (!TryReadPacketSnapshot(snapshot)) return;

    std::lock_guard<std::mutex> lock(packetSnapshotMutex);
    if (pendingPacketSnapshots.size() >= kMaximumPendingPacketSnapshots)
        return;
    pendingPacketSnapshots.push_back(snapshot);
}

bool MovementReplayVirtualKeyDown(int key) {
    const uint32_t keys = replayVirtualKeys.load(std::memory_order_acquire);
    switch (key) {
        case 'W': return (keys & ReplayW) != 0;
        case 'S': return (keys & ReplayS) != 0;
        case 'A': return (keys & ReplayA) != 0;
        case 'D': return (keys & ReplayD) != 0;
        case VK_SPACE: return (keys & ReplayJump) != 0;
        case VK_CONTROL:
        case VK_LCONTROL:
        case VK_RCONTROL: return (keys & ReplayDuck) != 0;
        case VK_SHIFT:
        case VK_LSHIFT:
        case VK_RSHIFT: return (keys & ReplayDash) != 0;
        default: return false;
    }
}

uint64_t MovementReplayHeldButtons() {
    return replayHeldButtons.load(std::memory_order_acquire);
}

uint64_t MovementReplayPressedButtons() {
    return replayPressedButtons.exchange(0, std::memory_order_acq_rel);
}

void UpdateMovementProbe(const std::vector<PlayerData>& players) {
    static bool wasEnabled = false;
    static ULONGLONG startedAt = 0;
    static ULONGLONG lastSampleAt = 0;
    static uintptr_t trackedEntity = 0;
    static Vector3 previousPosition{};
    static ULONGLONG previousPositionAt = 0;
    static uintptr_t eyeAnglesOffset = 0;
    static bool eyeAnglesOffsetResolved = false;

    if (players.empty()) {
        // Level teardown invalidates every cached entity/scene pointer. Stop
        // replay before the overlay can scan for a zipline in the old world.
        movementReplayActive = false;
        movementReplayCalibrating = false;
        replayStartedAt = 0;
        replayRouteStartedAt = 0;
        replayRouteStartTick = 0;
        replayTimelineMs = 0;
        replayPreviousInputKeys = 0;
        replayZiplineTargetReady = false;
        replayZiplineScanAt = 0;
        PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
        PublishReplayButtons(0, 0);
        packetTrackedBot2.store(0, std::memory_order_release);
        packetTrackedBot2Handle.store(0xFFFFFFFFu,
                                      std::memory_order_release);
        FlushPacketSnapshots(true);
    }

    if (!movementProbeEnabled) {
        if (wasEnabled) FlushPacketSnapshots(true);
        packetTrackedBot2.store(0, std::memory_order_release);
        packetTrackedBot2Handle.store(0xFFFFFFFFu,
                                      std::memory_order_release);
        wasEnabled = false;
        startedAt = 0;
        lastSampleAt = 0;
        trackedEntity = 0;
        previousPositionAt = 0;
        return;
    }

    // This recorder exists specifically for the Lockify replay pawn named
    // bot2. Previously the fallback selected any Haze in a regular match and
    // synchronously appended a wide CSV row every 16 ms, causing severe
    // frame-time spikes while playing Haze. Never arm capture without the
    // intended replay bot.
    bool hasReplayBot = false;
    for (const PlayerData& player : players) {
        if (player.entity && EqualsAsciiInsensitive(player.playerName, "bot2")) {
            hasReplayBot = true;
            break;
        }
    }
    if (!hasReplayBot) {
        if (wasEnabled) FlushPacketSnapshots(true);
        packetTrackedBot2.store(0, std::memory_order_release);
        packetTrackedBot2Handle.store(0xFFFFFFFFu,
                                      std::memory_order_release);
        wasEnabled = false;
        startedAt = 0;
        lastSampleAt = 0;
        trackedEntity = 0;
        previousPositionAt = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (!wasEnabled) {
        // Import the completed trace from the preceding session before this
        // new probe session truncates its CSV.
        LoadRecordedRouteProfile();
        // A new recording starts from a clean CSV every time the toggle is
        // enabled. Keeping a single predictable path makes it simple to send
        // the trace back for conversion into a replay track.
        std::ofstream output(Dll6Paths::DataFileA("movement_haze_probe.csv"),
                             std::ios::trunc);
        if (output) {
            output << "elapsed_ms,entity,player_name,hero_id,x,y,z,net_vx,net_vy,net_vz,"
                   << "derived_vx,derived_vy,derived_vz,speed2d,travel_yaw,"
                   << "view_pitch,view_yaw,animation_sequence,animation_age,phase,"
                   << "movement_services,attach_zipline,zipline_lane,dropped_zipline,"
                   << "zip_start,zip_stop,last_zip,is_dashing,dash_jump,slide_jump,"
                   << "wants_slide,crouching,in_air,sprinting,dash_start,dash_end,"
                   << "dash_jump_start,dash_jump_end,dash_dir_x,dash_dir_y,dash_dir_z,"
                   << "ground_entity,buttons_held,buttons_pressed,buttons_released,"
                   << "forward_move,left_move,up_move,key_w,key_a,key_s,key_d,"
                   << "key_jump,key_duck,key_speed\n";
        }
        std::ofstream inputText(
            Dll6Paths::DataFileA("movement_bot2_inputs.csv"),
            std::ios::trunc);
        if (inputText)
            inputText << "tick_ms,entity,class,text,x,y,z,bot_x,bot_y,bot_z,distance\n";
        {
            std::lock_guard<std::mutex> lock(packetSnapshotMutex);
            pendingPacketSnapshots.clear();
            pendingPacketSnapshots.reserve(kMaximumPendingPacketSnapshots);
        }
        std::ofstream packetOutput(
            Dll6Paths::DataFileA("movement_packet_entities.csv"),
            std::ios::trunc);
        if (packetOutput) {
            packetOutput
                << "packet_sequence,server_tick,delta_from,updated_entries,"
                   "entity_data_bytes,entity_data_hash,entity,handle,client_ms,qpc,"
                   "net_x,net_y,net_z,abs_x,abs_y,abs_z,render_x,render_y,render_z,"
                   "world_x,world_y,world_z,vel_x,vel_y,vel_z,eye_pitch,eye_yaw,"
                   "eye_roll,animation_sequence,animation_start,attach_zipline,"
                   "zipline_lane,dropped_zipline,zip_start,zip_stop,last_zip,"
                   "is_dashing,dash_jump,slide_jump,wants_slide,crouching,in_air,"
                   "sprinting,dash_start,dash_end,dash_jump_start,dash_jump_end,"
                   "dash_dir_x,dash_dir_y,dash_dir_z,ground_entity\n";
        }
        wasEnabled = true;
        startedAt = now;
    }
    FlushPacketSnapshots();
    // 60 Hz preserves jump/dash transitions but avoids dumping multiple
    // identical Present samples when the renderer runs at 144 Hz or higher.
    if (now - lastSampleAt < 16) return;
    lastSampleAt = now;

    const float centerX = ImGui::GetIO().DisplaySize.x * 0.5f;
    const float centerY = ImGui::GetIO().DisplaySize.y * 0.5f;
    const PlayerData* selected = nullptr;
    const PlayerData* bot2 = nullptr;
    float bestScreenDistance = FLT_MAX;
    for (const PlayerData& player : players) {
        if (!player.entity || Read<uint32_t>(player.entity + Offsets::HeroComponent +
                                             Offsets::HeroSpawnedId) != kHazeId)
            continue;
        if (EqualsAsciiInsensitive(player.playerName, "bot2")) {
            bot2 = &player;
            break;
        }
        Vector2 screen{};
        if (!WorldToScreen(player.worldPos, screen, currentViewMatrix)) continue;
        const float dx = screen.x - centerX;
        const float dy = screen.y - centerY;
        const float distance = dx * dx + dy * dy;
        if (distance < bestScreenDistance) {
            bestScreenDistance = distance;
            selected = &player;
        }
    }
    if (bot2) selected = bot2;
    if (!selected || !IsFinite(selected->worldPos)) {
        packetTrackedBot2.store(0, std::memory_order_release);
        packetTrackedBot2Handle.store(0xFFFFFFFFu,
                                      std::memory_order_release);
        return;
    }

    if (!eyeAnglesOffsetResolved) {
        eyeAnglesOffsetResolved = true;
        eyeAnglesOffset = ResolveRuntimeSchemaOffset(
            "C_BasePlayerPawn", "m_angEyeAngles");
        if (!eyeAnglesOffset) {
            eyeAnglesOffset = ResolveRuntimeSchemaOffset(
                "C_CitadelPlayerPawn", "m_angEyeAngles");
        }
    }
    packetEyeAnglesOffset.store(eyeAnglesOffset, std::memory_order_release);
    if (bot2 && selected == bot2) {
        const uintptr_t previousBot = packetTrackedBot2.load(
            std::memory_order_acquire);
        if (previousBot != selected->entity) {
            packetTrackedBot2Handle.store(
                FindEntityHandle(selected->entity), std::memory_order_release);
        }
        // Publish the pointer only after every offset and the handle are ready.
        packetTrackedBot2.store(selected->entity, std::memory_order_release);
    } else {
        packetTrackedBot2.store(0, std::memory_order_release);
        packetTrackedBot2Handle.store(0xFFFFFFFFu,
                                      std::memory_order_release);
    }

    const Vector3 position = selected->worldPos;
    Vector3 netVelocity = Read<Vector3>(selected->entity + Offsets::Velocity);
    if (!IsFinite(netVelocity)) netVelocity = {};
    Vector3 derivedVelocity{};
    if (trackedEntity == selected->entity && previousPositionAt &&
        now > previousPositionAt) {
        const float seconds = static_cast<float>(now - previousPositionAt) / 1000.0f;
        if (seconds > 0.0001f) {
            derivedVelocity = {(position.x - previousPosition.x) / seconds,
                               (position.y - previousPosition.y) / seconds,
                               (position.z - previousPosition.z) / seconds};
        }
    }
    const Vector3& movement = std::hypot(netVelocity.x, netVelocity.y) > 1.0f
        ? netVelocity : derivedVelocity;
    const float speed2d = std::hypot(movement.x, movement.y);
    const float travelYaw = speed2d > 1.0f
        ? std::atan2(movement.y, movement.x) * 57.29577951308232f : 0.0f;
    Vector3 viewAngles{};
    if (eyeAnglesOffset)
        viewAngles = Read<Vector3>(selected->entity + eyeAnglesOffset);
    if (!IsFinite(viewAngles)) viewAngles = {};
    const uintptr_t graphManager = Read<uintptr_t>(
        selected->entity + Offsets::GraphControllerManager);
    const uintptr_t graph = graphManager
        ? Read<uintptr_t>(graphManager + Offsets::MainGraphController) : 0;
    const int animationSequence = graph ? Read<int>(graph + Offsets::AnimSequence) : -1;
    const float animationStartedAt = graph
        ? Read<float>(graph + Offsets::AnimSequenceStartTime) : 0.0f;
    const float animationAge = GetClientGameTime() - animationStartedAt;
    const MovementProbeOffsets& offsets = GetMovementProbeOffsets();
    const uintptr_t movementServices = offsets.services
        ? Read<uintptr_t>(selected->entity + offsets.services) : 0;
    const uint32_t attachZipLine = ReadMovementValue<uint32_t>(
        movementServices, offsets.attachZipLine, 0xFFFFFFFFu);
    const int ziplineLane = ReadMovementValue<int>(
        movementServices, offsets.attachedZipLineLane, -1);
    const bool droppedZipline = ReadMovementValue<uint8_t>(
        movementServices, offsets.droppedFromZipline) != 0;
    const float zipStart = ReadMovementValue<float>(
        movementServices, offsets.timeStartZipping);
    const float zipStop = ReadMovementValue<float>(
        movementServices, offsets.timeStopZipping);
    const float lastZip = ReadMovementValue<float>(
        movementServices, offsets.lastTimeOnZipLine);
    const bool isDashing = ReadMovementValue<uint8_t>(
        movementServices, offsets.isDashing) != 0;
    const bool dashJump = ReadMovementValue<uint8_t>(
        movementServices, offsets.dashJump) != 0;
    const bool slideJump = ReadMovementValue<uint8_t>(
        movementServices, offsets.inSlideJump) != 0;
    const bool wantsSlide = ReadMovementValue<uint8_t>(
        movementServices, offsets.wantsSlide) != 0;
    const bool crouching = ReadMovementValue<uint8_t>(
        movementServices, offsets.crouching) != 0;
    const bool inAir = ReadMovementValue<uint8_t>(
        movementServices, offsets.inAir) != 0;
    const bool sprinting = ReadMovementValue<uint8_t>(
        movementServices, offsets.sprinting) != 0;
    const float dashStart = ReadMovementValue<float>(
        movementServices, offsets.dashStartTime);
    const float dashEnd = ReadMovementValue<float>(
        movementServices, offsets.dashEndTime);
    const float dashJumpStart = ReadMovementValue<float>(
        movementServices, offsets.dashJumpStartTime);
    const float dashJumpEnd = ReadMovementValue<float>(
        movementServices, offsets.dashJumpEndTime);
    Vector3 dashDirection = ReadMovementValue<Vector3>(
        movementServices, offsets.dashDirection);
    if (!IsFinite(dashDirection)) dashDirection = {};
    const uint32_t groundEntity = ReadMovementValue<uint32_t>(
        movementServices, offsets.groundEntity, 0xFFFFFFFFu);
    // These inherited fields are part of the published client layout. Unlike
    // the private zipline fields, their offsets are stable and can expose the
    // exact command the engine retained for Bot2.
    // CInButtonState starts at +0x50, but its first 8 bytes are the vtable.
    // The three button-state masks themselves begin at +0x58.
    constexpr uintptr_t ButtonsOffset = 0x58;
    constexpr uintptr_t ForwardMoveOffset = 0x1B4;
    constexpr uintptr_t LeftMoveOffset = 0x1B8;
    constexpr uintptr_t UpMoveOffset = 0x1BC;
    const uint64_t buttonsHeld = ReadMovementValue<uint64_t>(
        movementServices, ButtonsOffset);
    const uint64_t buttonsPressed = ReadMovementValue<uint64_t>(
        movementServices, ButtonsOffset + 8);
    const uint64_t buttonsReleased = ReadMovementValue<uint64_t>(
        movementServices, ButtonsOffset + 16);
    const float recordedForward = ReadMovementValue<float>(
        movementServices, ForwardMoveOffset);
    const float recordedLeft = ReadMovementValue<float>(
        movementServices, LeftMoveOffset);
    const float recordedUp = ReadMovementValue<float>(
        movementServices, UpMoveOffset);
    const auto buttonDown = [buttonsHeld](InputBitMask button) {
        return (buttonsHeld & static_cast<uint64_t>(button)) != 0;
    };

    if (bot2 && selected == bot2)
        UpdateAutomaticRouteCapture(now, position, viewAngles, movement, speed2d);

    std::ofstream output(Dll6Paths::DataFileA("movement_haze_probe.csv"),
                         std::ios::app);
    if (output) {
        output << (now - startedAt) << ',' << std::hex << selected->entity
               << std::dec << ',' << selected->playerName << ',' << kHazeId << ','
               << position.x << ',' << position.y << ',' << position.z << ','
               << netVelocity.x << ',' << netVelocity.y << ',' << netVelocity.z << ','
               << derivedVelocity.x << ',' << derivedVelocity.y << ','
               << derivedVelocity.z << ',' << speed2d << ',' << travelYaw << ','
               << viewAngles.x << ',' << viewAngles.y << ','
               << animationSequence << ',' << animationAge << ','
               << MovementPhase(movement, speed2d) << ','
               << std::hex << movementServices << ',' << attachZipLine << std::dec << ','
               << ziplineLane << ',' << droppedZipline << ','
               << zipStart << ',' << zipStop << ',' << lastZip << ','
               << isDashing << ',' << dashJump << ',' << slideJump << ','
               << wantsSlide << ',' << crouching << ',' << inAir << ','
               << sprinting << ',' << dashStart << ',' << dashEnd << ','
               << dashJumpStart << ',' << dashJumpEnd << ','
               << dashDirection.x << ',' << dashDirection.y << ','
               << dashDirection.z << ',' << std::hex << groundEntity << std::dec
               << ',' << std::hex << buttonsHeld << ',' << buttonsPressed << ','
               << buttonsReleased << std::dec << ',' << recordedForward << ','
               << recordedLeft << ',' << recordedUp << ','
               << buttonDown(InputBitMask::Forward) << ','
               << buttonDown(InputBitMask::MoveLeft) << ','
               << buttonDown(InputBitMask::Back) << ','
               << buttonDown(InputBitMask::MoveRight) << ','
               << buttonDown(InputBitMask::Jump) << ','
               << buttonDown(InputBitMask::Duck) << ','
               << buttonDown(InputBitMask::Speed)
               << '\n';
    }
    trackedEntity = selected->entity;
    previousPosition = position;
    previousPositionAt = now;
}

void UpdateMovementBotInputText(const std::vector<PlayerData>& players) {
    static ULONGLONG lastSampleAt = 0;
    static ULONGLONG lastEntityRefreshAt = 0;
    static std::vector<std::pair<uintptr_t, uintptr_t>> textEntities;
    static uintptr_t worldTextOffset = 0;
    static uintptr_t panelTextOffset = 0;
    static bool offsetsResolved = false;
    static bool discoveryLogged = false;
    static std::unordered_set<std::string> loggedCandidateClasses;

    if (!movementProbeEnabled || players.empty() || !clientBase) {
        observedBotInputKeys.store(0, std::memory_order_release);
        if (players.empty()) {
            textEntities.clear();
            lastEntityRefreshAt = 0;
        }
        return;
    }

    const PlayerData* bot2 = nullptr;
    for (const PlayerData& player : players) {
        if (player.entity && EqualsAsciiInsensitive(player.playerName, "bot2")) {
            bot2 = &player;
            break;
        }
    }
    if (!bot2) {
        observedBotInputKeys.store(0, std::memory_order_release);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (lastSampleAt && now - lastSampleAt < 15) return;
    lastSampleAt = now;

    if (!offsetsResolved) {
        offsetsResolved = true;
        worldTextOffset = ResolveRuntimeSchemaOffset(
            "C_PointWorldText", "m_messageText");
        panelTextOffset = ResolveRuntimeSchemaOffset(
            "C_PointClientUIWorldTextPanel", "m_messageText");
        if (!worldTextOffset) worldTextOffset = 0x9D0;
        if (!panelTextOffset) panelTextOffset = 0xC00;
        std::ofstream schema(
            Dll6Paths::DataFileA("movement_text_schema.log"), std::ios::trunc);
        if (schema)
            schema << std::hex << "world=0x" << worldTextOffset
                   << " panel=0x" << panelTextOffset << '\n';
    }

    // Lockify reuses a small set of networked world-text entities and changes
    // their messages as the ghost presses/releases controls. Cache that set,
    // but sample every rendered game frame for accurate transition times.
    if (textEntities.empty() || !lastEntityRefreshAt ||
        now - lastEntityRefreshAt >= 200) {
        lastEntityRefreshAt = now;
        textEntities.clear();
        const uintptr_t entityRoot = Read<uintptr_t>(
            clientBase + Offsets::GameEntitySystem);
        const int highest = entityRoot
            ? Read<int>(entityRoot + Offsets::HighestEntityIndex) : -1;
        if (highest >= 0 &&
            highest <= static_cast<int>(Offsets::HandleIndexMask)) {
            for (int index = 0; index <= highest; ++index) {
                const uintptr_t entity = ResolveEntityIndex(
                    static_cast<uint32_t>(index));
                if (!entity) continue;
                const std::string className = GetEntityClassName(entity);
                std::string lowerClass = className;
                std::transform(lowerClass.begin(), lowerClass.end(),
                    lowerClass.begin(), [](unsigned char value) {
                        return static_cast<char>(std::tolower(value));
                    });
                const bool textLikeClass =
                    lowerClass.find("text") != std::string::npos ||
                    lowerClass.find("panel") != std::string::npos ||
                    lowerClass.find("world") != std::string::npos ||
                    lowerClass.find("point") != std::string::npos ||
                    lowerClass.find("ui") != std::string::npos;
                if (textLikeClass &&
                    loggedCandidateClasses.emplace(className).second) {
                    Vector3 candidatePosition{};
                    const bool hasPosition = GetEntityPosition(
                        entity, candidatePosition) && IsFinite(candidatePosition);
                    std::ofstream inventory(Dll6Paths::DataFileA(
                        "movement_text_entities.log"), std::ios::app);
                    if (inventory)
                        inventory << std::hex << entity << std::dec << ','
                                  << className << ',' << hasPosition << ','
                                  << candidatePosition.x << ','
                                  << candidatePosition.y << ','
                                  << candidatePosition.z << '\n';
                }
                if (lowerClass.find("clientuiworldtext") != std::string::npos)
                    textEntities.emplace_back(entity, panelTextOffset);
                else if (lowerClass.find("worldtext") != std::string::npos)
                    textEntities.emplace_back(entity, worldTextOffset);

                // Lockify has used more than one text entity class. Discover
                // input labels by their contents when RTTI no longer matches
                // the two known class names. Candidate reads are bounded and
                // only retained when they decode to an actual input label.
                if (textLikeClass) {
                    std::string discoveredMessage;
                    for (uintptr_t offset = 0x600; offset <= 0x1000;
                         offset += sizeof(uintptr_t)) {
                        if (!ReadBotInputMessage(entity, offset,
                                                 discoveredMessage)) continue;
                        textEntities.emplace_back(entity, offset);
                        if (!discoveryLogged) {
                            std::ofstream discovery(Dll6Paths::DataFileA(
                                "movement_text_discovery.log"), std::ios::app);
                            if (discovery)
                                discovery << std::hex << entity << ",0x" << offset
                                          << std::dec << ',' << className << ','
                                          << discoveredMessage << '\n';
                        }
                        break;
                    }
                }
            }
            if (!textEntities.empty()) discoveryLogged = true;
        }
    }

    std::ofstream output(Dll6Paths::DataFileA("movement_bot2_inputs.csv"),
                         std::ios::app);
    uint32_t observedKeys = 0;
    for (const auto& candidate : textEntities) {
        const uintptr_t entity = candidate.first;
        std::string message;
        if (!ReadBotInputMessage(entity, candidate.second, message)) continue;
        Vector3 position{};
        if (!GetEntityPosition(entity, position) || !IsFinite(position))
            continue;
        const float distance = Distance3D(position, bot2->worldPos);
        if (distance > 2500.0f) continue;
        if (distance <= 650.0f) observedKeys |= ParseBotInputLabel(message);
        if (output)
            output << static_cast<uint32_t>(now) << ',' << std::hex << entity
                   << std::dec << ',' << GetEntityClassName(entity) << ','
                   << message << ',' << position.x << ',' << position.y << ','
                   << position.z << ',' << bot2->worldPos.x << ','
                   << bot2->worldPos.y << ',' << bot2->worldPos.z << ','
                   << distance << '\n';
    }
    observedBotInputKeys.store(observedKeys, std::memory_order_release);
}

bool ProcessMovementReplayUserCmd(CUserCmd* command, uintptr_t input) {
#ifdef DLL6_MOVEMENT_ONLY
    return ProcessLocalMovementRecorderReplay(command, input);
#else
    if (!command || !command->cmd.has_base()) return false;
    LoadRecordedRouteProfile();
    // Record/replay mode must never silently run the old development sample.
    // If no complete Bot2 pass has been captured yet, leave player input
    // untouched until movement_last_route.csv exists.
    if (!HasRecordedRoute()) {
        movementReplayActive = false;
        movementReplayCalibrating = false;
        replayStartedAt = 0;
        replayRouteStartedAt = 0;
        replayRouteStartTick = 0;
        replayTimelineMs = 0;
        replayPreviousInputKeys = 0;
        PublishReplayVirtualKeyMask(0);
        PublishReplayButtons(0, 0);
        return false;
    }
    const Vector3 replayStart = ReplayRouteFront().position;
    replayStartDistance = currentLocalPositionReady
        ? Distance3D(currentLocalPosition, replayStart)
        : FLT_MAX;

    const bool keyDown = !AreCustomBindsSuppressed() && movementReplayEnabled && movementReplayKey > 0 &&
        (GetAsyncKeyState(movementReplayKey) & 0x8000) != 0;
    const bool pressed = keyDown && !replayKeyWasDown;
    replayKeyWasDown = keyDown;
    if (pressed) {
        if (movementReplayActive || movementReplayCalibrating) {
            movementReplayActive = false;
            movementReplayCalibrating = false;
            replayStartedAt = 0;
            replayRouteStartedAt = 0;
            replayRouteStartTick = 0;
            replayTimelineMs = 0;
            replayPreviousInputKeys = 0;
            PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
            PublishReplayButtons(0, 0);
        } else if (!menuOpen && currentLocalPositionReady &&
                   replayStartDistance <= kReplayStartTolerance) {
            movementReplayCalibrating = true;
            replayCalibrationReachedAt = 0;
            replayCameraFilterReady = false;
            replayCameraFilterAt = 0;
        }
    }
    if (!movementReplayEnabled || menuOpen ||
        (!movementReplayActive && !movementReplayCalibrating) ||
        !currentLocalPositionReady) {
        if (!movementReplayEnabled || menuOpen) {
            movementReplayActive = false;
            movementReplayCalibrating = false;
            replayStartedAt = 0;
            replayRouteStartedAt = 0;
            replayRouteStartTick = 0;
            replayTimelineMs = 0;
            replayPreviousInputKeys = 0;
        }
        PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
        PublishReplayButtons(0, 0);
        return false;
    }

    if (movementReplayCalibrating) {
        // Calibration includes the camera, not only the player's origin.
        // Track the actual zipline while walking to the recorded start so the
        // following Space hold can attach without any manual mouse input.
        const Vector3 calibrationCamera = ReplayCameraAngles(
            ReplayRouteFront(), true);
        const float visibleYaw = calibrationCamera.y;
        auto* commandCamera = command->cmd.mutable_ang_camera_angles();
        commandCamera->set_x(calibrationCamera.x);
        commandCamera->set_y(calibrationCamera.y);
        commandCamera->set_z(0.0f);
        ApplyMovementReplayCameraAngles(calibrationCamera);
        const float dx = replayStart.x - currentLocalPosition.x;
        const float dy = replayStart.y - currentLocalPosition.y;
        const float distance2d = std::hypot(dx, dy);
        float forward = 0.0f;
        float left = 0.0f;
        if (distance2d > 7.0f) {
            const float worldYaw = std::atan2(dy, dx) * 57.29577951308232f;
            const float delta = NormalizeAngle(
                worldYaw - visibleYaw) * kDegreesToRadians;
            forward = std::cos(delta);
            left = std::sin(delta);
            replayCalibrationReachedAt = 0;
        } else {
            if (!replayCalibrationReachedAt)
                replayCalibrationReachedAt = GetTickCount64();
            if (GetTickCount64() - replayCalibrationReachedAt >= 350) {
                movementReplayCalibrating = false;
                movementReplayActive = true;
                replayStartedAt = GetTickCount64();
                // Every captured Bot2 trace begins after the ghost has
                // started its zipline pull. The local replay must reproduce
                // that missing pre-roll instead of starting frame zero as an
                // ordinary ground jump.
                replayRouteStartedAt = 0;
                replayRouteStartTick = 0;
                replayTimelineMs = 0;
                replayPreviousInputKeys = 0;
                replayStartZ = currentLocalPosition.z;
                replayLastLogAt = 0;
                std::ofstream log(Dll6Paths::DataFileA("movement_replay.log"),
                                  std::ios::trunc);
                if (log)
                    log << "run_ms,route_ms,x,y,z,ref_x,ref_y,ref_z,deviation,"
                           "forward,left,jump,duck,dash,input_keys,cam_pitch,cam_yaw,"
                           "tick_ms,spatial_ms,action_ms,action_sample_ms\n";
            }
        }
        auto* base = command->cmd.mutable_base();
        base->set_forwardmove(forward);
        base->set_leftmove(left);
        // Movement axes are transformed into the forced zipline-facing camera
        // basis, so position and view finish calibration together.
        uint64_t holdMask = 0;
        if (forward > 0.15f) holdMask |= static_cast<uint64_t>(InputBitMask::Forward);
        else if (forward < -0.15f) holdMask |= static_cast<uint64_t>(InputBitMask::Back);
        if (left > 0.15f) holdMask |= static_cast<uint64_t>(InputBitMask::MoveLeft);
        else if (left < -0.15f) holdMask |= static_cast<uint64_t>(InputBitMask::MoveRight);
        ApplyReplayButtons(command, holdMask, 0);
        PublishReplayVirtualKeys(forward, left, false, false);
        PublishReplayButtons(holdMask, 0);
        return true;
    }

    const ULONGLONG now = GetTickCount64();
    const uint32_t attachElapsed = static_cast<uint32_t>(now - replayStartedAt);

    // The recorded ghost was already being pulled when its 250 ms Space-up
    // transition arrived. A fixed local timer releases Space before network
    // attachment on many runs, leaving Haze in an ordinary ground jump. Hold
    // the attach input and the zipline-facing camera until the pawn reaches
    // the recorded release height, then enter the recorded timeline at its
    // real 250 ms release frame.
    if (!replayRouteStartedAt) {
        const Vector3 attachCamera = ReplayCameraAngles(
            ReplayRouteFront(), true);
        auto* commandCamera = command->cmd.mutable_ang_camera_angles();
        commandCamera->set_x(attachCamera.x);
        commandCamera->set_y(attachCamera.y);
        commandCamera->set_z(0.0f);
        ApplyMovementReplayCameraAngles(attachCamera);
        const float recordedReleaseHeight =
            InterpolateRoute(kZiplineRecordedReleaseMs).position.z;
        if (currentLocalPosition.z >=
            recordedReleaseHeight - kZiplineReleaseHeightTolerance) {
            replayRouteStartedAt = now - kZiplineRecordedReleaseMs;
            const auto& base = command->cmd.base();
            if (base.has_client_tick()) {
                constexpr int32_t releaseTicks = static_cast<int32_t>(
                    (kZiplineRecordedReleaseMs * kReplayTickRate + 500) / 1000);
                replayRouteStartTick = base.client_tick() - releaseTicks;
            }
            replayTimelineMs = kZiplineRecordedReleaseMs;
            replayPreviousInputKeys = ReplayW | ReplayJump;
        } else if (attachElapsed >= kZiplineAttachTimeoutMs) {
            movementReplayActive = false;
            replayStartedAt = 0;
            replayRouteStartedAt = 0;
            replayRouteStartTick = 0;
            replayTimelineMs = 0;
            replayPreviousInputKeys = 0;
            PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
            PublishReplayButtons(0, 0);
            ApplyReplayButtons(command, 0, 0);
            auto* base = command->cmd.mutable_base();
            base->set_forwardmove(0.0f);
            base->set_leftmove(0.0f);
            return true;
        } else {
            constexpr uint32_t attachKeys = ReplayW | ReplayJump;
            auto* base = command->cmd.mutable_base();
            base->set_forwardmove(1.0f);
            base->set_leftmove(0.0f);
            const uint64_t holdMask = ReplayButtonMask(attachKeys);
            const uint64_t changedMask = ReplayButtonMask(
                attachKeys ^ replayPreviousInputKeys);
            replayPreviousInputKeys = attachKeys;
            ApplyReplayButtons(command, holdMask, changedMask);
            PublishReplayVirtualKeyMask(attachKeys);
            PublishReplayButtons(holdMask, changedMask);
            return true;
        }
    }

    uint32_t tickElapsed = static_cast<uint32_t>(now - replayRouteStartedAt);
    const auto& commandBase = command->cmd.base();
    if (replayRouteStartTick && commandBase.has_client_tick()) {
        const int32_t tickDelta = commandBase.client_tick() - replayRouteStartTick;
        if (tickDelta >= 0)
            tickElapsed = static_cast<uint32_t>(
                (static_cast<uint64_t>(tickDelta) * 1000) / kReplayTickRate);
    }
    const uint32_t spatialProgress = ClosestRouteProgress(currentLocalPosition);
    const uint32_t actionProgress = ClosestRouteHorizontalProgress(
        currentLocalPosition, spatialProgress);
    // Buttons remain on the recorded server-tick clock. Camera timing is
    // corrected separately below from spatial progress.
    replayTimelineMs = (std::max)(replayTimelineMs, tickElapsed);
    const uint32_t runElapsed = replayTimelineMs;
    const uint32_t elapsed = runElapsed > kReplayJumpPreroll
        ? runElapsed - kReplayJumpPreroll : 0;
    if (elapsed >= ReplayDurationMs()) {
        movementReplayActive = false;
        replayRouteStartedAt = 0;
        replayRouteStartTick = 0;
        replayTimelineMs = 0;
        replayPreviousInputKeys = 0;
        PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
        PublishReplayButtons(0, 0);
        ApplyReplayButtons(command, 0, 0);
        auto* base = command->cmd.mutable_base();
        base->set_forwardmove(0.0f);
        base->set_leftmove(0.0f);
        return true;
    }

    const ReplayKeyframe spatialReference = InterpolateRoute(spatialProgress);
    const float deviation = Distance3D(currentLocalPosition,
                                       spatialReference.position);
    if (elapsed >= 1000 && deviation > kReplayAbortDeviation) {
        movementReplayActive = false;
        replayRouteStartedAt = 0;
        replayRouteStartTick = 0;
        replayTimelineMs = 0;
        replayPreviousInputKeys = 0;
        PublishReplayVirtualKeys(0.0f, 0.0f, false, false, false);
        PublishReplayButtons(0, 0);
        ApplyReplayButtons(command, 0, 0);
        auto* base = command->cmd.mutable_base();
        base->set_forwardmove(0.0f);
        base->set_leftmove(0.0f);
        std::ofstream log(Dll6Paths::DataFileA("movement_replay.log"),
                          std::ios::app);
        if (log)
            log << "aborted deviation=" << deviation
                << " route_ms=" << elapsed << '\n';
        return true;
    }
    // Trigger steering and actions at the position where Bot2 triggered them.
    // The local pawn was ~150 ms behind the recording in the last run; using
    // elapsed time therefore pressed D and turned the camera at route_ms 944,
    // while the captured transition belongs at route_ms 1094.
    constexpr uint32_t directionMask = ReplayW | ReplayS | ReplayA | ReplayD;
    constexpr uint32_t actionMask = ReplayJump | ReplayDuck | ReplayDash;
    const uint32_t directionKeys = ReplayInputAt(spatialProgress);
    const uint32_t actionSampleProgress = (std::min)(
        ReplayDurationMs(), actionProgress + kReplayActionLeadMs);
    const uint32_t actionKeys = ReplayInputAt(actionSampleProgress);
    const uint32_t inputKeys = (directionKeys & directionMask) |
                               (actionKeys & actionMask);
    // Turn according to spatial progress, not elapsed time: local physics can
    // trail the server recording by a few ticks. The raw replicated eye yaw is
    // deliberately not used here because it is not the replay bot's camera.
    const Vector3 replayCamera = ReplayPathCameraAngles(spatialProgress,
                                                        inputKeys);
    auto* commandCamera = command->cmd.mutable_ang_camera_angles();
    commandCamera->set_x(replayCamera.x);
    commandCamera->set_y(replayCamera.y);
    commandCamera->set_z(0.0f);
    ApplyMovementReplayCameraAngles(replayCamera);
    const float forward =
        ((inputKeys & ReplayW) ? 1.0f : 0.0f) -
        ((inputKeys & ReplayS) ? 1.0f : 0.0f);
    const float left =
        ((inputKeys & ReplayA) ? 1.0f : 0.0f) -
        ((inputKeys & ReplayD) ? 1.0f : 0.0f);

    auto* base = command->cmd.mutable_base();
    base->set_forwardmove(forward);
    base->set_leftmove(left);
    // The packet trace now supplies the bot's replicated eye angles on the
    // same server-tick clock as its position. Patch both this command and the
    // visible gameplay camera so movement physics and the rendered view use
    // one camera basis.

    const uint64_t holdMask = ReplayButtonMask(inputKeys);
    // buttonState2 is the transition mask: buttonState1 determines whether a
    // changed bit is a press or release. buttonState3 is the scroll mask and
    // must not receive ordinary key transitions.
    const uint32_t changedKeys = inputKeys ^ replayPreviousInputKeys;
    const uint64_t changedMask = ReplayButtonMask(changedKeys);
    replayPreviousInputKeys = inputKeys;
    const bool jump = (inputKeys & ReplayJump) != 0;
    const bool duck = (inputKeys & ReplayDuck) != 0;
    const bool dash = (inputKeys & ReplayDash) != 0;
    ApplyReplayButtons(command, holdMask, changedMask);
    PublishReplayVirtualKeyMask(inputKeys);
    PublishReplayButtons(holdMask, changedMask);

    if (!replayLastLogAt || now - replayLastLogAt >= 100) {
        replayLastLogAt = now;
        std::ofstream log(Dll6Paths::DataFileA("movement_replay.log"),
                          std::ios::app);
        if (log) {
            log << runElapsed << ',' << elapsed << ','
                << currentLocalPosition.x << ',' << currentLocalPosition.y << ','
                << currentLocalPosition.z << ',' << spatialReference.position.x << ','
                << spatialReference.position.y << ',' << spatialReference.position.z << ','
                << deviation << ',' << forward << ',' << left << ','
                << jump << ',' << duck << ',' << dash << ','
                << std::hex << inputKeys << std::dec << ','
                << (command->cmd.has_ang_camera_angles()
                        ? command->cmd.ang_camera_angles().x() : 0.0f) << ','
                << (command->cmd.has_ang_camera_angles()
                        ? command->cmd.ang_camera_angles().y() : 0.0f) << ','
                << tickElapsed << ',' << spatialProgress << ','
                << actionProgress << ',' << actionSampleProgress << '\n';
        }
    }
    return true;
#endif
}

void DrawMovementReplayOverlay() {
    if (!movementReplayEnabled || !ImGui::GetCurrentContext()) return;
#ifdef DLL6_MOVEMENT_ONLY
    LoadLocalMovementRecording();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    if (localMovementRecordingReady && !localCommandFrames.empty()) {
        Vector2 startScreen{};
        if (currentViewMatrixReady && WorldToScreen(
                localCommandFrames.front().position, startScreen,
                currentViewMatrix)) {
            const float distance = currentLocalPositionReady
                ? Distance3D(currentLocalPosition,
                             localCommandFrames.front().position) : FLT_MAX;
            const ImU32 markerColor =
                distance <= kLocalCalibrationPositionTolerance
                ? IM_COL32(70, 240, 125, 255)
                : distance <= kReplayStartTolerance
                    ? IM_COL32(255, 205, 70, 255)
                    : IM_COL32(245, 75, 75, 255);
            draw->AddCircleFilled(
                ImVec2(startScreen.x, startScreen.y), 6.0f,
                markerColor, 24);
            draw->AddCircle(
                ImVec2(startScreen.x, startScreen.y), 12.0f,
                markerColor, 24, 2.0f);
            draw->AddText(
                ImVec2(startScreen.x + 15.0f, startScreen.y - 9.0f),
                markerColor, "START");
        }
    }
    char status[192]{};
    ImU32 color = IM_COL32(255, 255, 255, 235);
    if (localMovementRecording) {
        std::snprintf(status, sizeof(status),
                      "Movement demo: RECORDING [%s to stop]  frames=%zu",
                      GetVirtualKeyDisplayName(localMovementRecordKey).c_str(),
                      localCommandFrames.size());
        color = IM_COL32(255, 85, 85, 245);
    } else if (localMovementPlaybackCalibrating) {
        std::snprintf(status, sizeof(status),
                      "Movement demo: CALIBRATING  distance=%.1f",
                      localCommandFrames.empty() ? 0.0f : Distance3D(
                          currentLocalPosition,
                          localCommandFrames.front().position));
        color = IM_COL32(255, 205, 70, 245);
    } else if (localMovementPlaybackActive) {
        std::snprintf(status, sizeof(status),
                      "Movement demo: PLAYING  frame=%zu/%zu",
                      localPlaybackIndex, localCommandFrames.size());
        color = IM_COL32(75, 225, 125, 245);
    } else if (localMovementRecordingReady) {
        const float distance = currentLocalPositionReady
            ? Distance3D(currentLocalPosition,
                         localCommandFrames.front().position) : 0.0f;
        std::snprintf(status, sizeof(status),
                      "Movement demo: READY  [%s record] [%s replay]  start=%.1f",
                      GetVirtualKeyDisplayName(localMovementRecordKey).c_str(),
                      GetVirtualKeyDisplayName(movementReplayKey).c_str(),
                      distance);
        color = distance <= kReplayStartTolerance
            ? IM_COL32(75, 225, 125, 245)
            : IM_COL32(255, 185, 70, 245);
    } else {
        std::snprintf(status, sizeof(status),
                      "Movement demo: [%s] start recording",
                      GetVirtualKeyDisplayName(localMovementRecordKey).c_str());
        color = IM_COL32(255, 185, 70, 245);
    }
    draw->AddText(ImVec2(18.0f, 82.0f), color, status);
    return;
#else
    LoadRecordedRouteProfile();
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    if (!draw) return;
    if (!HasRecordedRoute()) {
        draw->AddText(ImVec2(18.0f, 128.0f), IM_COL32(255, 185, 70, 255),
                      "Movement: run Bot2 once to record a route");
        return;
    }
    Vector2 startScreen{};
    if (currentViewMatrixReady && WorldToScreen(
            ReplayRouteFront().position, startScreen, currentViewMatrix)) {
        const ImU32 color = replayStartDistance <= kReplayStartTolerance
            ? IM_COL32(70, 230, 120, 235) : IM_COL32(240, 80, 80, 220);
        draw->AddCircle(ImVec2(startScreen.x, startScreen.y), 11.0f,
                        color, 24, 2.0f);
        draw->AddText(ImVec2(startScreen.x + 14.0f, startScreen.y - 9.0f),
                      color, "bot2 movement start");
    }
    char status[128]{};
    if (movementReplayCalibrating) {
        std::snprintf(status, sizeof(status),
                      "Movement bot2: CALIBRATING (%.0f u)",
                      replayStartDistance);
    } else if (movementReplayActive) {
        std::snprintf(status, sizeof(status), "Movement bot2: RUN (network camera) %.1fs",
                      (GetTickCount64() - replayStartedAt) / 1000.0f);
    } else if (replayStartDistance <= kReplayStartTolerance) {
        std::snprintf(status, sizeof(status), "Movement bot2: READY [%s]",
                      GetVirtualKeyDisplayName(movementReplayKey).c_str());
    } else {
        std::snprintf(status, sizeof(status),
                      "Movement bot2: go to start (%.0f u)",
                      replayStartDistance);
    }
    draw->AddText(ImVec2(18.0f, 82.0f), IM_COL32(255, 255, 255, 230), status);
#endif
}
