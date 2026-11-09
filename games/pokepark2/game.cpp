#include "wp/game.h"

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
    0x80210d90,
};

}

const Description& description() {
    return kDescription;
}

}
