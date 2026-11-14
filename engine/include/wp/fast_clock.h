#pragma once

#include <chrono>

namespace wp {

struct FastClock {
    using duration = std::chrono::nanoseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FastClock>;
    static constexpr bool is_steady = true;
    static time_point now() noexcept;
};

}
