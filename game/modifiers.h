#pragma once

#include "wp/cpu.h"
#include "wp/hle.h"

namespace wp::game {

void modifiers_frame(Cpu& c, bool park);
void kinetic_energy_update(Cpu& c, HleFunction original);
void chase_step(Cpu& c, HleFunction original);

}
