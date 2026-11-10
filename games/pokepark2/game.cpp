#include <cstring>

#include "wp/game.h"
#include "wp/gx.h"
#include "wp/disc.h"
#include "wp/input.h"
#include "wp/log.h"
#include "wp/memory.h"
#include "wp/settings.h"

namespace wp::game {

namespace {

constexpr Description kDescription = {
    "PokePark 2: Wonders Beyond",
    "WondersBeyondRecomp",
    "pokepark2",
    "games/pokepark2/extracted",
    "games/pokepark2/nand",
    "games/pokepark2/settings.ini",
    "games/pokepark2/textures",
    true,
    "",
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

// Strap and Hold also set up the save, the common data and the sound the title needs, so they still run,
// hidden and confirmed by themselves. CheckData is left as is: it can ask about the save.
void start_scene(Cpu& c, uint32_t self, uint32_t scene) {
    bool hidden = scene != kSceneCheckData;
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
    constexpr uint32_t kInputAlarm = 0x804BA4D8;
    constexpr uint32_t kFrameAlarm = 0x804BA548;
    constexpr uint32_t kPeriod = 0x38;
    constexpr uint32_t kPendingPeriod = 0x40;
    constexpr uint32_t kRunning = 0x6C;
    static const settings::LiveNumber fps("video.fps", "WP_FPS");
    if (rd8(kInputAlarm + kRunning) == 0 || rd8(kFrameAlarm + kRunning) == 0) {
        return;
    }
    uint64_t input = (static_cast<uint64_t>(rd32(kInputAlarm + kPeriod)) << 32) | rd32(kInputAlarm + kPeriod + 4);
    bool movie = disc::seconds_since_read("/Movie/") < 0.5;
    uint64_t wanted = fps() >= 60 && !movie ? input : input * 2;
    uint64_t pending = (static_cast<uint64_t>(rd32(kFrameAlarm + kPendingPeriod)) << 32) | rd32(kFrameAlarm + kPendingPeriod + 4);
    if (input != 0 && pending != wanted) {
        wr32(kFrameAlarm + kPendingPeriod, static_cast<uint32_t>(wanted >> 32));
        wr32(kFrameAlarm + kPendingPeriod + 4, static_cast<uint32_t>(wanted));
    }
}

HleFunction replacement(const char* name) {
    return std::strcmp(name, "SequencerBootUpdate") == 0 ? boot_sequence : nullptr;
}

}
