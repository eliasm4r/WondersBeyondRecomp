#pragma once

#include "wp/cpu.h"

namespace wp::game {

bool puppets_blocks_motion(uint32_t accessor);
void puppets_frame(Cpu& c, bool park, bool active);
void puppets_free_beam(Cpu& c);
void puppets_scene_change(Cpu& c);

}
