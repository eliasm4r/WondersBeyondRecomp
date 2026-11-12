#include "wp/game.h"
#include "wp/gamepad.h"

namespace wp::game {

const Description& description() {
    static constexpr Description kDescription = {
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
    return kDescription;
}

}

namespace wp::gamepad {

padmap::Family shown_family() {
    return padmap::Family::Xbox;
}

}
