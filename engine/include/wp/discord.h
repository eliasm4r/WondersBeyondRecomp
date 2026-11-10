#pragma once

namespace wp::discord {

// Shows "playing" with the elapsed time in Discord while the game runs, when Discord is open on this computer and the
// game has a Discord application. Retries in the background, so starting Discord later works too.
void start();

}
