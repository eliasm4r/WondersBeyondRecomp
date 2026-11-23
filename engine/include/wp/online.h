#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/presence.h"

namespace wp::online {

enum class State { Off, Connecting, Hosting, Joined, Failed };
enum class Problem { None, NoInternet, NoRoom, Full, Blocked, Server, Version, HostLeft, Lost };

struct Notice {
    enum class Kind { Joined, Left, Zone, Version };
    Kind kind = Kind::Joined;
    std::string name;
    uint32_t zone = 0;
};

constexpr uint16_t kPort = 47917;
constexpr size_t kCodeLength = 6;

bool valid_address(const std::string& address);
bool valid_code(const std::string& code);
std::string clean_code(const std::string& text);
bool host(const std::string& name, uint16_t port = kPort, bool use_room = true);
bool join(const std::string& target, const std::string& name, uint16_t port = kPort);
void leave();
void start_from_environment();
void update(const std::array<float, 3>& position, uint32_t zone, const presence::Pose& pose = {});
std::vector<presence::Peer> peers();
State state();
Problem problem();
bool running();
size_t others();
std::string room_code();
std::string local_address();
uint64_t version();
std::vector<Notice> take_notices();
std::string other_version();
bool reconnecting();
void say_goodbye();
uint32_t player_color(uint32_t id);
uint32_t own_id();
void own_state(std::string& name, uint32_t& character, uint32_t& zone);

}
