#pragma once

#include "wp/cpu.h"
#include "wp/hle.h"

namespace wp::game {

void pick_scene(Cpu& c, HleFunction original);
void add_scene_parameter(Cpu& c, HleFunction original);
void scene_manager_run_begin(Cpu& c, HleFunction original);
void dance_frame(Cpu& c, HleFunction original);
void dance_frame_second_part(Cpu& c, HleFunction original);

}
