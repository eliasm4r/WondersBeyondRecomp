#include "wp/stall_watch.h"

namespace wp {

StallWatch::StallWatch(std::chrono::seconds limit) : limit_(limit) {}

bool StallWatch::update(uint64_t frames, bool excused, std::chrono::steady_clock::time_point now) {
    if (frames != frames_ || excused || frames == 0) {
        frames_ = frames;
        since_ = now;
        reported_ = false;
        return false;
    }
    if (reported_ || now - since_ <= limit_) {
        return false;
    }
    reported_ = true;
    return true;
}

}
