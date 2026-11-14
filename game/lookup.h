#pragma once

#include "wp/hle.h"

namespace wp::game {

void find_resource_by_name(Cpu& c, HleFunction original);
void find_resource_by_number(Cpu& c, HleFunction original);
void next_lookup_retrace();

}
