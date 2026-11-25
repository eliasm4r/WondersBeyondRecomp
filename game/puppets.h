#pragma once

#include "wp/cpu.h"
#include "wp/hle.h"

namespace wp::game {

bool puppets_blocks_motion(uint32_t accessor);
bool puppets_shot_starting(uint32_t accessor);
void puppets_frame(Cpu& c, bool park, bool active);
void puppets_free_beam(Cpu& c);
void puppets_skip_in_search(Cpu& c, HleFunction original);
uint32_t puppets_model_object(Cpu& c, uint32_t accessor);
void puppets_scene_change(Cpu& c);
void puppets_visibility_step(Cpu& c, HleFunction original);

}
