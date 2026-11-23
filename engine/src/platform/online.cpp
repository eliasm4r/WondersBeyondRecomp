#include "wp/online.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

#include "wp/log.h"

namespace wp::online {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPumpEvery = std::chrono::milliseconds(16);
constexpr auto kSendEvery = std::chrono::milliseconds(33);
constexpr auto kQuietAfter = std::chrono::seconds(1);

std::mutex g_control;
std::mutex g_mutex;
std::unique_ptr<presence::Link> g_link;
std::vector<presence::Peer> g_peers;
std::array<float, 3> g_position{};
uint32_t g_zone = 0;
Clock::time_point g_updated{};
std::atomic<State> g_state{State::Off};
std::atomic<uint64_t> g_version{1};
std::atomic<bool> g_stop{false};
std::thread g_thread;

void worker() {
    const Clock::time_point started = Clock::now();
    Clock::time_point next_send = started;
    while (!g_stop.load()) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Clock::time_point now = Clock::now();
            g_link->pump(std::chrono::duration<double>(now - started).count());
            if (now >= next_send) {
                g_link->send(g_position, now - g_updated < kQuietAfter ? g_zone : 0);
                next_send = now + kSendEvery;
            }
            size_t before = g_peers.size();
            g_peers = g_link->peers();
            if (g_peers.size() != before) {
                log::write("online", "%zu other player(s) connected", g_peers.size());
                g_version++;
            }
        }
        std::this_thread::sleep_for(kPumpEvery);
    }
}

void stop_worker() {
    if (g_thread.joinable()) {
        g_stop.store(true);
        g_thread.join();
        g_stop.store(false);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_link.reset();
    g_peers.clear();
}

bool begin(std::unique_ptr<presence::Link> link, bool ok, State state) {
    if (!ok) {
        g_state.store(State::Failed);
        g_version++;
        log::write("online", "could not start");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_link = std::move(link);
        g_zone = 0;
    }
    g_state.store(state);
    g_version++;
    g_thread = std::thread(worker);
    return true;
}

struct Shutdown {
    ~Shutdown() { leave(); }
};

Shutdown g_shutdown;

}

bool valid_address(const std::string& address) {
    int parts = 0;
    size_t at = 0;
    while (true) {
        size_t end = address.find('.', at);
        std::string part = address.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos || std::atoi(part.c_str()) > 255) {
            return false;
        }
        parts++;
        if (end == std::string::npos) {
            return parts == 4;
        }
        at = end + 1;
    }
}

bool host(const std::string& name, uint16_t port) {
    std::lock_guard<std::mutex> control(g_control);
    stop_worker();
    auto link = std::make_unique<presence::Link>();
    bool ok = link->host(port, name);
    log::write("online", "hosting as %s on port %u: %s", name.c_str(), port, ok ? "ok" : "failed");
    return begin(std::move(link), ok, State::Hosting);
}

bool join(const std::string& address, const std::string& name, uint16_t port) {
    std::lock_guard<std::mutex> control(g_control);
    stop_worker();
    auto link = std::make_unique<presence::Link>();
    bool ok = valid_address(address) && link->join(address, port, name);
    log::write("online", "joining %s:%u as %s: %s", address.c_str(), port, name.c_str(), ok ? "ok" : "failed");
    return begin(std::move(link), ok, State::Joined);
}

void leave() {
    std::lock_guard<std::mutex> control(g_control);
    stop_worker();
    if (g_state.exchange(State::Off) != State::Off) {
        g_version++;
        log::write("online", "left");
    }
}

void start_from_environment() {
    const char* given = std::getenv("WP_ONLINE");
    if (!given) {
        return;
    }
    const char* named = std::getenv("WP_ONLINE_NAME");
    const std::string name = named ? named : "Player";
    const std::string text = given;
    size_t last = text.rfind(':');
    int port = last == std::string::npos ? 0 : std::atoi(text.c_str() + last + 1);
    if (port <= 0 || port >= 65536) {
        return;
    }
    if (text.rfind("host:", 0) == 0) {
        host(name, static_cast<uint16_t>(port));
    } else if (text.rfind("join:", 0) == 0 && last > 5) {
        join(text.substr(5, last - 5), name, static_cast<uint16_t>(port));
    }
}

void update(const std::array<float, 3>& position, uint32_t zone) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_position = position;
    g_zone = zone;
    g_updated = Clock::now();
}

std::vector<presence::Peer> peers() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peers;
}

State state() {
    return g_state.load();
}

bool running() {
    State now = state();
    return now == State::Hosting || now == State::Joined;
}

size_t others() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peers.size();
}

uint64_t version() {
    return g_version.load();
}

}
