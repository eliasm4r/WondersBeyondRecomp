#pragma once

#include "wp/cpu.h"

namespace wp::game {

void puppets_frame(Cpu& c, bool park, bool active);
void puppets_free_beam(Cpu& c);

}
