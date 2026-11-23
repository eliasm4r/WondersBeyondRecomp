#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::presence {

constexpr size_t kMostEvents = 16;

struct Event {
    uint32_t serial = 0;
    uint32_t kind = 0;
    uint32_t id = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t c = 0;
    float f1 = 0.0f;
    float f2 = 0.0f;
    std::array<float, 3> offset{};
    std::array<float, 3> vector{};
};

struct Pose {
    std::array<float, 3> rotation{};
    uint32_t character = 0;
    std::string motion;
    uint32_t motion_flags = 0;
    uint32_t motion_serial = 0;
    std::vector<Event> events;
    uint32_t sent_ms = 0;
};

struct Snapshot {
    double time = 0.0;
    std::array<float, 3> position{};
    std::array<float, 3> rotation{};
};

Snapshot snapshot_at(const std::vector<Snapshot>& snapshots, double time, float teleport);

struct Peer {
    uint32_t id = 0;
    uint32_t sequence = 0;
    uint32_t zone = 0;
    std::array<float, 3> position{};
    Pose pose;
    std::string name;
    double seen = 0.0;
};

class Link {
public:
    Link();
    ~Link();
    Link(const Link&) = delete;
    Link& operator=(const Link&) = delete;

    bool host(uint16_t port, const std::string& name);
    bool join(const std::string& address, uint16_t port, const std::string& name);
    bool open_room(bool hosting, uint16_t port, const std::string& name);
    void add_target(const std::string& endpoint, double seconds);
    void ask_stun(const std::vector<std::string>& servers);
    std::string mapped() const;
    bool symmetric() const;
    std::string local() const;
    static std::vector<std::string> stun_servers();
    void send(const std::array<float, 3>& position, uint32_t zone = 0, const Pose& pose = {});
    void pump(double now);
    const std::vector<Peer>& peers() const { return m_peers; }
    uint16_t port() const;

private:
    struct Impl;
    Impl* m_impl;
    std::vector<Peer> m_peers;
};

}
