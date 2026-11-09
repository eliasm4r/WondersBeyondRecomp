#include <cstring>

#include "wp/game.h"
#include "wp/gx.h"
#include "wp/input.h"
#include "wp/log.h"
#include "wp/memory.h"

namespace wp::game {

namespace {

constexpr Description kDescription = {
    "PokePark 2: Wonders Beyond",
    "pokepark2",
    "games/pokepark2/extracted",
    "games/pokepark2/nand",
    "games/pokepark2/settings.ini",
    "games/pokepark2/textures",
    true,
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

HleFunction replacement(const char* name) {
    return std::strcmp(name, "SequencerBootUpdate") == 0 ? boot_sequence : nullptr;
}

}
