#pragma once

#include "wp/cpu.h"
#include "wp/hle.h"

namespace wp::game {

void font_resource(Cpu& c, HleFunction original);
void update_prompts();
void texture_bind(Cpu& c, HleFunction original);

}
