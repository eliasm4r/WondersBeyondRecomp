#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/presence.h"

namespace wp::online {

enum class State { Off, Hosting, Joined, Failed };

constexpr uint16_t kPort = 47917;

bool valid_address(const std::string& address);
bool host(const std::string& name, uint16_t port = kPort);
bool join(const std::string& address, const std::string& name, uint16_t port = kPort);
void leave();
void start_from_environment();
void update(const std::array<float, 3>& position, uint32_t zone);
std::vector<presence::Peer> peers();
State state();
bool running();
size_t others();
uint64_t version();

}
