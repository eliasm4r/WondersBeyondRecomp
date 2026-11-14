#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "wp/cpu.h"
#include "wp/game.h"
#include "wp/gx.h"
#include "wp/disc.h"
#include "wp/frame_pacing.h"
#include "wp/free_camera.h"
#include "wp/gamepad.h"
#include "wp/input.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/settings.h"
#include "lookup.h"

namespace wp::game {

namespace {

constexpr Description kDescription = {
    "PokePark 2: Wonders Beyond",
    "WondersBeyondRecomp",
    "pokepark2",
    "game/extracted",
    "game/nand",
    "game/settings.ini",
    "game/textures",
    true,
    "1569649565127409705",
};

// nsNile::nsSequence::SequencerBoot (vtable 0x80453800), state at +4. The game plays Strap (scene 1), which
// also loads the save and the common data, then CheckData (scene 3) when the save is not ready, then
// Hold (scene 2, how to hold the Wii Remote), and returns 2 to go on to the title.
constexpr uint32_t kSaveReady = 0x801313e0;
constexpr uint32_t kStartScene = 0x801313a0;
constexpr uint32_t kSceneFinished = 0x80131290;
constexpr uint32_t kNotStarted = 0xFFFFFFFF;
constexpr uint32_t kSceneStrap = 1;
constexpr uint32_t kSceneHold = 2;
constexpr uint32_t kSceneCheckData = 3;
constexpr uint32_t kStay = 0xFFFFFFFF;
constexpr uint32_t kToTitle = 2;
constexpr uint32_t kParkPrepareState = 0x804bb21c;
constexpr uint32_t kParkPrepared = 2;
constexpr uint32_t kInputAlarm = 0x804BA4D8;
constexpr uint32_t kAlarmPeriod = 0x38;
constexpr uint32_t kAlarmPending = 0x40;
constexpr uint32_t kApplyAlarmPeriod = 0x8000efa0;
constexpr uint64_t kBootSpeed = 4;

bool g_boot_fast = false;

bool boot_speedup() {
    static const bool wanted = [] {
        const char* value = std::getenv("WP_BOOT_SPEEDUP");
        return !value || *value != '0';
    }();
    return wanted;
}

uint64_t read_u64(uint32_t address) {
    return (static_cast<uint64_t>(rd32(address)) << 32) | rd32(address + 4);
}

void write_u64(uint32_t address, uint64_t value) {
    wr32(address, static_cast<uint32_t>(value >> 32));
    wr32(address + 4, static_cast<uint32_t>(value));
}

void pace_boot(Cpu& c, bool fast) {
    static uint64_t normal = 0;
    uint64_t period = read_u64(kInputAlarm + kAlarmPeriod);
    if (fast && normal == 0) {
        normal = period;
        log::write("boot", "park data ready, hidden screens run %llu times faster", static_cast<unsigned long long>(kBootSpeed));
    }
    if (normal == 0) {
        return;
    }
    uint64_t wanted = fast ? normal / kBootSpeed : normal;
    if (period != wanted) {
        write_u64(kInputAlarm + kAlarmPending, wanted);
        uint32_t saved = c.r[3];
        c.r[3] = kInputAlarm;
        call(c, kApplyAlarmPeriod);
        c.r[3] = saved;
    }
    if (!fast) {
        normal = 0;
    }
}

// Strap and Hold also set up the save, the common data and the sound the title needs, so they still run,
// hidden and confirmed by themselves. CheckData is left as is: it can ask about the save.
void start_scene(Cpu& c, uint32_t self, uint32_t scene) {
    bool hidden = scene != kSceneCheckData;
    g_boot_fast = hidden && boot_speedup();
    gx::set_screen_hidden(hidden);
    input::set_auto_confirm(hidden);
    wr32(self + 4, scene);
    c.r[3] = self;
    c.r[4] = scene;
    call(c, kStartScene);
}

// The same sequence as the game's, with Strap and Hold never shown.
void boot_sequence(Cpu& c) {
    uint32_t self = c.r[3];
    uint32_t state = rd32(self + 4);
    pace_boot(c, g_boot_fast && (state != kSceneStrap || rd32(kParkPrepareState) == kParkPrepared));
    if (state == kNotStarted) {
        start_scene(c, self, kSceneStrap);
        c.r[3] = kStay;
        return;
    }
    call(c, kSceneFinished);
    if (c.r[3] != 1) {
        c.r[3] = kStay;
        return;
    }
    if (state == kSceneHold) {
        gx::set_screen_hidden(false);
        input::set_auto_confirm(false);
        g_boot_fast = false;
        pace_boot(c, false);
        log::write("boot", "title");
        c.r[3] = kToTitle;
        return;
    }
    uint32_t next = kSceneHold;
    if (state == kSceneStrap) {
        call(c, kSaveReady);
        next = c.r[3] == 1 ? kSceneHold : kSceneCheckData;
    }
    log::write("boot", "scene %u", next);
    start_scene(c, self, next);
    c.r[3] = kStay;
}

}

const Description& description() {
    return kDescription;
}

// The main loop is paced by two periodic alarms (docs/NOTES.md, "Frame pacing"): input at 60 Hz and frames at
// 30 Hz, and it runs one logic step per input tick. Giving the frame alarm the input period renders every step.
// The loop applies a pending period by itself (0x8000efa0). Movies advance one picture per rendered frame, so they
// keep 30 fps while their file is being streamed.
void retrace() {
    next_lookup_retrace();
    constexpr uint32_t kFrameAlarm = 0x804BA548;
    constexpr uint32_t kPeriod = 0x38;
    constexpr uint32_t kPendingPeriod = 0x40;
    constexpr uint32_t kRunning = 0x6C;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    if (rd8(kInputAlarm + kRunning) == 0 || rd8(kFrameAlarm + kRunning) == 0) {
        return;
    }
    auto read64 = [](uint32_t address) { return (static_cast<uint64_t>(rd32(address)) << 32) | rd32(address + 4); };
    auto write64 = [](uint32_t address, uint64_t value) {
        wr32(address, static_cast<uint32_t>(value >> 32));
        wr32(address + 4, static_cast<uint32_t>(value));
    };
    bool movie = disc::seconds_since_read("/Movie/") < 0.5;
    uint64_t period = read64(kFrameAlarm + kPeriod);
    uint64_t pending = read64(kFrameAlarm + kPendingPeriod);
    pacing::AlarmPeriods wanted = pacing::frame_alarm(read64(kInputAlarm + kPeriod), period, pending, static_cast<int>(fps()), movie);
    if (wanted.period != period) {
        write64(kFrameAlarm + kPeriod, wanted.period);
    }
    if (wanted.pending != pending) {
        write64(kFrameAlarm + kPendingPeriod, wanted.pending);
    }
}

HleFunction replacement(const char* name) {
    return std::strcmp(name, "SequencerBootUpdate") == 0 ? boot_sequence : nullptr;
}

namespace {

constexpr uint32_t kCameraEye = 0x0c;
constexpr uint32_t kCameraShownEye = 0x18;
constexpr uint32_t kCameraTarget = 0x28;
constexpr uint32_t kFollowHandle = 0x8001a610;
constexpr uint32_t kFollowMissing = 0x800c6710;
constexpr uint32_t kFollowPosition = 0x800c5d80;
constexpr uint32_t kFollowRelease = 0x800c64c0;
constexpr uint32_t kFollowFrame = 0x40;
constexpr uint32_t kGroundProbe = 0x800277b0;
constexpr uint32_t kFieldCollision = 0x80076e90;
constexpr uint32_t kCollisionSegment = 0x8012dd40;
constexpr uint32_t kCollisionPoint = 0x8012fe60;
constexpr float kTargetHeight = 4.1f;
constexpr float kEyeClearance = 1.5f;
constexpr float kProbeAbove = 3.0f;
constexpr float kWallClearance = 1.0f;
constexpr float kSlopeReach = 40.0f;
constexpr float kNearestEye = 2.0f;
constexpr float kReachRate = 3.0f;
constexpr float kHeightRate = 10.0f;
constexpr float kFloorRiseRate = 16.0f;
constexpr float kFloorFallRate = 8.0f;
constexpr auto kCameraIdle = std::chrono::milliseconds(500);
constexpr auto kPlaceLogPeriod = std::chrono::seconds(2);

void write_vector(uint32_t address, const float value[3]) {
    for (uint32_t i = 0; i < 3; i++) {
        wrf32(address + 4 * i, value[i]);
    }
}

bool same_vector(uint32_t address, const float value[3]) {
    return rdf32(address) == value[0] && rdf32(address + 4) == value[1] && rdf32(address + 8) == value[2];
}

bool followed_position(Cpu& c, uint32_t camera, float out[3]) {
    uint32_t stack = c.r[1];
    uint32_t saved_r3 = c.r[3];
    double saved_f1 = c.f[1];
    uint32_t frame = stack - kFollowFrame;
    wr32(frame, stack);
    c.r[1] = frame;
    uint32_t handle = frame + 0x10;
    c.r[3] = handle;
    c.r[4] = camera;
    c.r[5] = 0xFFFFFFFF;
    call(c, kFollowHandle);
    c.r[3] = handle;
    call(c, kFollowMissing);
    bool found = c.r[3] == 0;
    if (found) {
        c.r[3] = handle;
        call(c, kFollowPosition);
        for (uint32_t i = 0; i < 3; i++) {
            out[i] = rdf32(c.r[3] + 4 * i);
        }
    }
    c.r[3] = handle;
    c.r[4] = 0xFFFFFFFF;
    call(c, kFollowRelease);
    c.r[1] = stack;
    c.r[3] = saved_r3;
    c.f[1] = saved_f1;
    return found;
}

// The game's own camera asks the field collision whether there is ground in a vertical span (`0x800277b0`: point,
// reach above, reach below); the point it met is then read from the collision manager. The collision only covers
// where one can walk: past its edge the free camera stays above the height of the followed character's feet, or above
// the slope from those feet to the last ground found on the way to the eye when that is higher.
bool ground_under(Cpu& c, const float point[3], float above, float below, float& ground) {
    uint32_t stack = c.r[1];
    uint32_t saved_r3 = c.r[3];
    double saved_f1 = c.f[1];
    double saved_f2 = c.f[2];
    uint32_t frame = stack - kFollowFrame;
    wr32(frame, stack);
    c.r[1] = frame;
    uint32_t vector = frame + 0x10;
    write_vector(vector, point);
    c.r[3] = vector;
    c.f[1] = above;
    c.f[2] = below;
    call(c, kGroundProbe);
    bool found = c.r[3] != 0;
    if (found) {
        call(c, kFieldCollision);
        call(c, kCollisionPoint);
        ground = rdf32(c.r[3] + 4);
    }
    c.r[1] = stack;
    c.r[3] = saved_r3;
    c.f[1] = saved_f1;
    c.f[2] = saved_f2;
    return found;
}

// The same collision, asked along any segment (`0x8012dd40`: manager, start, start, extent, 1, 0, 0, 0). What stands
// between the target and the eye brings the eye in front of it; the eye then eases back out.
bool collision_between(Cpu& c, const float start[3], const float extent[3], float met[3]) {
    uint32_t stack = c.r[1];
    uint32_t saved_r3 = c.r[3];
    double saved_f1 = c.f[1];
    uint32_t frame = stack - kFollowFrame;
    wr32(frame, stack);
    c.r[1] = frame;
    write_vector(frame + 0x10, start);
    write_vector(frame + 0x20, extent);
    call(c, kFieldCollision);
    c.r[4] = frame + 0x10;
    c.r[5] = frame + 0x10;
    c.r[6] = frame + 0x20;
    c.r[7] = 1;
    c.r[8] = 0;
    c.r[9] = 0;
    c.r[10] = 0;
    call(c, kCollisionSegment);
    bool found = c.r[3] != 0;
    if (found) {
        call(c, kFieldCollision);
        call(c, kCollisionPoint);
        for (uint32_t i = 0; i < 3; i++) {
            met[i] = rdf32(c.r[3] + 4 * i);
        }
    }
    c.r[1] = stack;
    c.r[3] = saved_r3;
    c.f[1] = saved_f1;
    return found;
}

void field_camera(Cpu& c, HleFunction original) {
    static free_camera::Orbit orbit;
    static uint32_t owner = 0;
    static bool wrote = false;
    static float game_eye[3];
    static float game_target[3];
    static float our_eye[3];
    static float our_target[3];
    static float height = 0.0f;
    static std::chrono::steady_clock::time_point last;
    uint32_t camera = c.r[3];
    auto now = std::chrono::steady_clock::now();
    bool fresh = camera != owner || now - last > kCameraIdle;
    if (wrote && !fresh && same_vector(camera + kCameraEye, our_eye) && same_vector(camera + kCameraTarget, our_target)) {
        write_vector(camera + kCameraEye, game_eye);
        write_vector(camera + kCameraShownEye, game_eye);
        write_vector(camera + kCameraTarget, game_target);
    }
    wrote = false;
    original(c);
    static std::chrono::steady_clock::time_point logged;
    if (now - logged >= kPlaceLogPeriod) {
        logged = now;
        log::write("place", "camera target %.0f %.0f %.0f", rdf32(camera + kCameraTarget), rdf32(camera + kCameraTarget + 4), rdf32(camera + kCameraTarget + 8));
    }
    free_camera::Settings settings = free_camera::current();
    float mouse_x = 0.0f;
    float mouse_y = 0.0f;
    free_camera::take_mouse(mouse_x, mouse_y);
    bool recenter = free_camera::take_recenter();
    if (!settings.enabled) {
        owner = 0;
        return;
    }
    float seconds = fresh ? 0.0f : std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    float offset[3];
    for (uint32_t i = 0; i < 3; i++) {
        game_eye[i] = rdf32(camera + kCameraEye + 4 * i);
        game_target[i] = rdf32(camera + kCameraTarget + 4 * i);
        offset[i] = game_eye[i] - game_target[i];
        our_target[i] = game_target[i];
    }
    float follow[3];
    bool following = followed_position(c, camera, follow);
    if (following) {
        float wanted = follow[1] + kTargetHeight;
        if (fresh || std::fabs(wanted - height) > 40.0f) {
            height = wanted;
        }
        height += (wanted - height) * std::min(1.0f, seconds * kHeightRate);
        our_target[0] = follow[0];
        our_target[1] = height;
        our_target[2] = follow[2];
    }
    if (fresh) {
        orbit.reset();
    }
    owner = camera;
    last = now;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    gamepad::camera_stick(stick_x, stick_y);
    free_camera::Turn turn = free_camera::turn(mouse_x, mouse_y, stick_x, stick_y, seconds, settings);
    float out[3];
    orbit.update(offset, our_target, turn, recenter, seconds, out);
    for (int i = 0; i < 3; i++) {
        our_eye[i] = our_target[i] + out[i];
    }
    float ground = 0.0f;
    float above = std::max(0.0f, our_target[1] - our_eye[1]) + kProbeAbove;
    bool grounded = ground_under(c, our_eye, above, kEyeClearance, ground);
    if (!grounded && following) {
        ground = follow[1];
        grounded = true;
        for (float part : {0.75f, 0.5f, 0.25f}) {
            float nearer[3] = {our_target[0] + out[0] * part, our_eye[1], our_target[2] + out[2] * part};
            float known = 0.0f;
            if (ground_under(c, nearer, above, kSlopeReach, known)) {
                ground = std::max({ground, known, follow[1] + (known - follow[1]) / part});
                break;
            }
        }
    }
    float free_height = our_eye[1] - our_target[1];
    float lowest = grounded ? ground + kEyeClearance : our_eye[1];
    our_eye[1] = std::max(our_eye[1], lowest);
    static float reach = 1.0f;
    float extent[3];
    float length = 0.0f;
    for (int i = 0; i < 3; i++) {
        extent[i] = our_eye[i] - our_target[i];
        length += extent[i] * extent[i];
    }
    length = std::sqrt(length);
    float allowed = 1.0f;
    float met[3];
    if (length > kNearestEye) {
        float longer = (length + kWallClearance) / length;
        float probe[3] = {extent[0] * longer, extent[1] * longer, extent[2] * longer};
        if (collision_between(c, our_target, probe, met)) {
            float to_met = std::sqrt((met[0] - our_target[0]) * (met[0] - our_target[0]) + (met[1] - our_target[1]) * (met[1] - our_target[1]) +
                                     (met[2] - our_target[2]) * (met[2] - our_target[2]));
            allowed = std::clamp((to_met - kWallClearance) / length, kNearestEye / length, 1.0f);
        }
    }
    reach = fresh ? allowed : std::min(allowed, reach + seconds * kReachRate);
    for (int i = 0; i < 3; i++) {
        our_eye[i] = our_target[i] + extent[i] * reach;
    }
    if (reach < 1.0f && ground_under(c, our_eye, above, kEyeClearance, ground)) {
        lowest = ground + kEyeClearance;
    }
    static float floor = 0.0f;
    floor = fresh ? lowest : floor + (lowest - floor) * std::min(1.0f, seconds * (lowest > floor ? kFloorRiseRate : kFloorFallRate));
    our_eye[1] = our_target[1] + free_height * reach;
    if (floor > our_eye[1]) {
        our_eye[1] = floor;
        if (turn.pitch < 0.0f) {
            orbit.rest_on(our_eye[0] - our_target[0], our_eye[1] - our_target[1], our_eye[2] - our_target[2]);
        }
    }
    write_vector(camera + kCameraEye, our_eye);
    write_vector(camera + kCameraShownEye, our_eye);
    write_vector(camera + kCameraTarget, our_target);
    wrote = true;
    free_camera::note_driving();
}

}

namespace {

constexpr uint32_t kDeferredFree = 0x8000e0b0;
constexpr uint32_t kFreeSlot = 0x1f8;
constexpr uint32_t kFreePending = 0x1fc;
constexpr uint32_t kNoSlot = 0xFFFFFFFF;
constexpr uint32_t kHeapStride = 0x3c;
constexpr uint32_t kHeapFreeList = 0xc;
constexpr uint32_t kBlockNext = 0x8;
constexpr uint32_t kMaxHeapBlocks = 10000;
constexpr auto kTokenWait = std::chrono::milliseconds(500);

struct LateFree {
    uint32_t pool;
    uint32_t pointer;
};

std::vector<LateFree> g_late_frees;

bool frees_pending(uint32_t pool) {
    return rd32(pool + kFreePending) != kNoSlot;
}

bool heap_block(uint32_t block) {
    return (block & 3) == 0 && !slow_address(block) && (block & kAddressMask) + kBlockNext + 4 <= kPhysicalSize;
}

uint32_t largest_free(uint32_t heap) {
    uint32_t largest = 0;
    uint32_t blocks = 0;
    uint32_t previous = heap;
    for (uint32_t block = rd32(heap + kHeapFreeList); block != 0 && blocks < kMaxHeapBlocks; block = rd32(block + kBlockNext)) {
        if (!heap_block(block)) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                log::write("memory", "temporary heap at %08x: free block %u after %08x links to %08x, the rest of the list is skipped", heap, blocks, previous, block);
            }
            break;
        }
        largest = std::max(largest, rd32(block));
        previous = block;
        blocks++;
    }
    return largest;
}

uint32_t block_size(uint32_t size, uint32_t alignment) {
    alignment = std::max<uint32_t>(alignment, 4);
    uint32_t wanted = std::max<uint32_t>(size + 8, std::max<uint32_t>(alignment, 16));
    if (alignment > 4) {
        return (wanted + 2 * (alignment - 1)) & ~(alignment - 1);
    }
    return (wanted + 3) & ~3u;
}

void deferred_free(Cpu& c, HleFunction original) {
    static size_t most = 0;
    uint32_t pool = c.r[3];
    if (rd32(pool + kFreeSlot) != kNoSlot) {
        original(c);
        return;
    }
    g_late_frees.push_back({pool, c.r[4]});
    if (g_late_frees.size() > most) {
        most = g_late_frees.size();
        log::write("memory", "the game's list of frees waiting for the GPU is full, %u kept aside", static_cast<unsigned>(most));
    }
}

void release_deferred_frees(Cpu& c, HleFunction original) {
    uint32_t pool = c.r[3];
    original(c);
    size_t kept = 0;
    for (size_t i = 0; i < g_late_frees.size(); i++) {
        LateFree entry = g_late_frees[i];
        if (entry.pool == pool && rd32(pool + kFreeSlot) != kNoSlot) {
            c.r[3] = pool;
            c.r[4] = entry.pointer;
            call(c, kDeferredFree);
        } else {
            g_late_frees[kept++] = entry;
        }
    }
    g_late_frees.resize(kept);
}

void temp_pool_alloc(Cpu& c, HleFunction original) {
    uint32_t pool = c.r[3];
    uint32_t index = c.r[4];
    uint32_t size = c.r[5];
    uint32_t alignment = c.r[6];
    uint32_t heap = pool + index * kHeapStride;
    uint32_t needed = block_size(size, alignment);
    uint32_t before = largest_free(heap);
    if (before < needed && frees_pending(pool)) {
        auto start = std::chrono::steady_clock::now();
        bool reached = gx::wait_for_token(kTokenWait);
        poll_interrupts(c);
        double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        log::write("memory", "temporary heap %u had %u KB for a %u KB request: waited %.1f ms for the GPU (%s), now %u KB", index, before >> 10, needed >> 10,
                   waited, reached ? "reached" : "not reached", largest_free(heap) >> 10);
        c.r[3] = pool;
        c.r[4] = index;
        c.r[5] = size;
        c.r[6] = alignment;
    }
    original(c);
}

}

HookFunction hook(const char* name) {
    static const struct {
        const char* name;
        HookFunction function;
    } kHooks[] = {{"FieldCameraUpdate", field_camera}, {"DeferredFree", deferred_free}, {"ReleaseDeferredFrees", release_deferred_frees}, {"TempPoolAlloc", temp_pool_alloc},
                   {"FindResourceByName", find_resource_by_name},
                   {"FindResourceByNameOther", find_resource_by_name},
                   {"FindResourceByNumber", find_resource_by_number}};
    for (const auto& entry : kHooks) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.function;
        }
    }
    return nullptr;
}

}
