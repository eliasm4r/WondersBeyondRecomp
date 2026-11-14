#pragma once

#include <cstdint>
#include <string>

namespace wp::attraction {

enum class Mode : int {
    None,
    Aim,
    Dance,
    Shake,
    Fly,
};

constexpr uint32_t kMoveLeft = 1;
constexpr uint32_t kMoveUp = 2;
constexpr uint32_t kMoveDown = 4;
constexpr uint32_t kMoveRight = 8;
constexpr uint32_t kMoveKick = 16;
constexpr uint32_t kMovePose = 32;

using DanceSwing = int (*)(uint32_t moves, uint32_t report);
using DancePlaying = bool (*)();

void set_mode(Mode mode);
Mode mode();
void set_dance(DanceSwing swing, DancePlaying playing);
int dance_swing(uint32_t moves, uint32_t report);
bool dance_playing();

enum class Arrow : int { None, Left, Up, Down, Right };
enum class Shape : int { None, Cross, Circle, Square, Triangle };

struct Label {
    Arrow arrow = Arrow::None;
    Shape shape = Shape::None;
    std::string text;
};

Label move_label(uint32_t move);

}
