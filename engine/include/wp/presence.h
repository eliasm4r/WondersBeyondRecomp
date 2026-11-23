#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wp::presence {

struct Peer {
    uint32_t id = 0;
    uint32_t sequence = 0;
    std::array<float, 3> position{};
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
    void send(const std::array<float, 3>& position);
    void pump(double now);
    const std::vector<Peer>& peers() const { return m_peers; }
    uint16_t port() const;

private:
    struct Impl;
    Impl* m_impl;
    std::vector<Peer> m_peers;
};

}
