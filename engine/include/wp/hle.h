#pragma once

#include "wp/cpu.h"

namespace wp {

using HleFunction = void (*)(Cpu&);
using HookFunction = void (*)(Cpu&, HleFunction original);

HleFunction find_replacement(const char* name);
HookFunction find_hook(const char* name);

}
