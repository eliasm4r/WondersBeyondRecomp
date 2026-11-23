#include "wp/presence.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
constexpr Socket kNoSocket = -1;
#endif

namespace wp::presence {

namespace {

constexpr uint32_t kMagic = 0x57425231;
constexpr size_t kNameBytes = 12;
constexpr size_t kPacketBytes = 24 + kNameBytes;
constexpr size_t kMaxOthers = 3;
constexpr double kPeerTimeout = 5.0;
constexpr float kLargestCoordinate = 1e6f;
constexpr int kMaxPacketsPerPump = 64;

void put32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t get32(const uint8_t* in) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    }
    return value;
}

std::string clean_name(const char* text, size_t size) {
    std::string name;
    for (size_t i = 0; i < size && text[i] != 0 && name.size() < kNameBytes; i++) {
        unsigned char character = static_cast<unsigned char>(text[i]);
        name.push_back(character >= 0x20 && character < 0x7f ? static_cast<char>(character) : '?');
    }
    return name;
}

bool decode(const uint8_t* data, size_t size, Peer& peer) {
    if (size != kPacketBytes || get32(data) != kMagic) {
        return false;
    }
    peer.id = get32(data + 4);
    peer.sequence = get32(data + 8);
    for (int i = 0; i < 3; i++) {
        uint32_t bits = get32(data + 12 + 4 * i);
        std::memcpy(&peer.position[i], &bits, sizeof bits);
        if (!std::isfinite(peer.position[i]) || std::fabs(peer.position[i]) > kLargestCoordinate) {
            return false;
        }
    }
    peer.name = clean_name(reinterpret_cast<const char*>(data + 24), kNameBytes);
    return peer.id != 0;
}

bool same_address(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        const auto& x = reinterpret_cast<const sockaddr_in&>(a);
        const auto& y = reinterpret_cast<const sockaddr_in&>(b);
        return x.sin_port == y.sin_port && x.sin_addr.s_addr == y.sin_addr.s_addr;
    }
    const auto& x = reinterpret_cast<const sockaddr_in6&>(a);
    const auto& y = reinterpret_cast<const sockaddr_in6&>(b);
    return x.sin6_port == y.sin6_port && std::memcmp(&x.sin6_addr, &y.sin6_addr, sizeof x.sin6_addr) == 0;
}

socklen_t address_size(const sockaddr_storage& address) {
    return address.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
}

void close_socket(Socket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool make_nonblocking(Socket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

struct Client {
    sockaddr_storage address{};
    uint32_t id = 0;
    double seen = 0.0;
};

}

struct Link::Impl {
    Socket socket = kNoSocket;
    bool hosting = false;
    uint32_t id = 0;
    uint32_t sequence = 0;
    char name[kNameBytes] = {};
    sockaddr_storage server{};
    std::vector<Client> clients;

    void transmit(const uint8_t* data, const sockaddr_storage& to) const {
        sendto(socket, reinterpret_cast<const char*>(data), static_cast<int>(kPacketBytes), 0, reinterpret_cast<const sockaddr*>(&to), address_size(to));
    }

    void prepare(const std::string& text) {
        std::random_device random;
        do {
            id = random();
        } while (id == 0);
        std::string cleaned = clean_name(text.c_str(), text.size());
        std::memset(name, 0, sizeof name);
        std::memcpy(name, cleaned.data(), std::min(cleaned.size(), sizeof name));
    }

    bool open(int family) {
#ifdef _WIN32
        static const bool started = [] {
            WSADATA data;
            return WSAStartup(MAKEWORD(2, 2), &data) == 0;
        }();
        if (!started) {
            return false;
        }
#endif
        socket = ::socket(family, SOCK_DGRAM, IPPROTO_UDP);
        return socket != kNoSocket && make_nonblocking(socket);
    }
};

Link::Link() : m_impl(new Impl) {}

Link::~Link() {
    if (m_impl->socket != kNoSocket) {
        close_socket(m_impl->socket);
    }
    delete m_impl;
}

bool Link::host(uint16_t port, const std::string& name) {
    if (!m_impl->open(AF_INET)) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (bind(m_impl->socket, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        return false;
    }
    m_impl->hosting = true;
    m_impl->prepare(name);
    return true;
}

bool Link::join(const std::string& where, uint16_t port, const std::string& name) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    if (!m_impl->open(AF_INET) || getaddrinfo(where.c_str(), std::to_string(port).c_str(), &hints, &found) != 0 || !found) {
        return false;
    }
    std::memcpy(&m_impl->server, found->ai_addr, found->ai_addrlen);
    freeaddrinfo(found);
    m_impl->prepare(name);
    return true;
}

uint16_t Link::port() const {
    sockaddr_in address{};
    socklen_t size = sizeof address;
    if (m_impl->socket == kNoSocket || getsockname(m_impl->socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

void Link::send(const std::array<float, 3>& position) {
    if (m_impl->socket == kNoSocket) {
        return;
    }
    uint8_t packet[kPacketBytes] = {};
    put32(packet, kMagic);
    put32(packet + 4, m_impl->id);
    put32(packet + 8, ++m_impl->sequence);
    for (int i = 0; i < 3; i++) {
        uint32_t bits;
        std::memcpy(&bits, &position[i], sizeof bits);
        put32(packet + 12 + 4 * i, bits);
    }
    std::memcpy(packet + 24, m_impl->name, kNameBytes);
    if (m_impl->hosting) {
        for (const Client& client : m_impl->clients) {
            m_impl->transmit(packet, client.address);
        }
    } else {
        m_impl->transmit(packet, m_impl->server);
    }
}

void Link::pump(double now) {
    if (m_impl->socket == kNoSocket) {
        return;
    }
    uint8_t data[kPacketBytes + 1];
    for (int i = 0; i < kMaxPacketsPerPump; i++) {
        sockaddr_storage from{};
        socklen_t from_size = sizeof from;
        auto got = recvfrom(m_impl->socket, reinterpret_cast<char*>(data), sizeof data, 0, reinterpret_cast<sockaddr*>(&from), &from_size);
        if (got < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAECONNRESET) {
                continue;
            }
#endif
            break;
        }
        Peer incoming;
        if (!decode(data, static_cast<size_t>(got), incoming) || incoming.id == m_impl->id) {
            continue;
        }
        if (m_impl->hosting) {
            auto client = std::find_if(m_impl->clients.begin(), m_impl->clients.end(), [&](const Client& c) { return c.id == incoming.id; });
            if (client == m_impl->clients.end()) {
                if (m_impl->clients.size() >= kMaxOthers) {
                    continue;
                }
                m_impl->clients.push_back({from, incoming.id, now});
                client = m_impl->clients.end() - 1;
            } else if (!same_address(client->address, from)) {
                continue;
            }
            client->seen = now;
        } else if (!same_address(m_impl->server, from)) {
            continue;
        }
        auto peer = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return p.id == incoming.id; });
        if (peer != m_peers.end() && static_cast<int32_t>(incoming.sequence - peer->sequence) <= 0) {
            continue;
        }
        if (m_impl->hosting) {
            for (const Client& client : m_impl->clients) {
                if (client.id != incoming.id) {
                    m_impl->transmit(data, client.address);
                }
            }
        }
        incoming.seen = now;
        if (peer != m_peers.end()) {
            *peer = incoming;
        } else if (m_peers.size() < kMaxOthers) {
            m_peers.push_back(incoming);
        }
    }
    m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return now - p.seen > kPeerTimeout; }), m_peers.end());
    auto& clients = m_impl->clients;
    clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const Client& c) { return now - c.seen > kPeerTimeout; }), clients.end());
}

}
