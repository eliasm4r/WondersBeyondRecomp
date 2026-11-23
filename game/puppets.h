#pragma once

#include "wp/cpu.h"

namespace wp::game {

bool hold_puppet(uint32_t object);
void puppets_frame(Cpu& c, bool park, bool active);

}
