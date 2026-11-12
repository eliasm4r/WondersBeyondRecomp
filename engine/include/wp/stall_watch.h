#pragma once

#include <chrono>
#include <cstdint>

namespace wp {

class StallWatch {
public:
    explicit StallWatch(std::chrono::seconds limit);
    bool update(uint64_t frames, bool excused, std::chrono::steady_clock::time_point now);

private:
    std::chrono::seconds limit_;
    std::chrono::steady_clock::time_point since_{};
    uint64_t frames_ = 0;
    bool reported_ = false;
};

}
