#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/presence.h"

namespace wp::online {

enum class State { Off, Connecting, Hosting, Joined, Failed };
enum class Problem { None, NoInternet, NoRoom, Full, Blocked, Server };

constexpr uint16_t kPort = 47917;
constexpr size_t kCodeLength = 6;

bool valid_address(const std::string& address);
bool valid_code(const std::string& code);
std::string clean_code(const std::string& text);
bool host(const std::string& name, uint16_t port = kPort, bool use_room = true);
bool join(const std::string& target, const std::string& name, uint16_t port = kPort);
void leave();
void start_from_environment();
void update(const std::array<float, 3>& position, uint32_t zone);
std::vector<presence::Peer> peers();
State state();
Problem problem();
bool running();
size_t others();
std::string room_code();
std::string local_address();
uint64_t version();

}
