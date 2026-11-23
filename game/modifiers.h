#pragma once

#include "wp/cpu.h"
#include "wp/hle.h"

namespace wp::game {

void modifiers_frame(Cpu& c, bool park);
void kinetic_energy_update(Cpu& c, HleFunction original);
void chase_step(Cpu& c, HleFunction original);
void note_field_camera(uint32_t camera);
void player_move(Cpu& c, HleFunction original);
void motion_start(Cpu& c, HleFunction original);
void sound_play(Cpu& c, HleFunction original);
void beam_start(Cpu& c, HleFunction original);
void beam_set(Cpu& c, HleFunction original);
void beam_move(Cpu& c, HleFunction original);
void beam_stop(Cpu& c, HleFunction original);
void minimap_update(Cpu& c, HleFunction original);
void sound_stop(Cpu& c, HleFunction original);
void sound_stop_all(Cpu& c, HleFunction original);
void effect_at(Cpu& c, HleFunction original);
void effect_on(Cpu& c, HleFunction original);
void note_effect_stop(Cpu& c, uint32_t slot);
template <uint32_t Slot>
void effect_stop(Cpu& c, HleFunction original) {
    note_effect_stop(c, Slot);
    original(c);
}
void life_damage(Cpu& c, HleFunction original);

}
