#pragma once

#include <cstddef>

#include <string>

namespace wp::discord {

std::string party_json(const std::string& code, size_t players);
std::string join_secret(const std::string& frame);

// Shows "playing" with the elapsed time in Discord while the game runs, when Discord is open on this computer and the
// game has a Discord application. Retries in the background, so starting Discord later works too.
void start();

}
