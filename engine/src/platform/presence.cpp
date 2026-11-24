#include "wp/presence.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <random>
#include <string>

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

constexpr uint32_t kMagic = 0x57425233;
constexpr uint32_t kGoodbye = 0x57425258;
constexpr size_t kGoodbyeBytes = 8;
constexpr size_t kVersionAt = 28;
constexpr size_t kVersionBytes = 16;
constexpr size_t kNameBytes = 12;
constexpr size_t kNameAt = kVersionAt + kVersionBytes;
constexpr size_t kRotationAt = kNameAt + kNameBytes;
constexpr size_t kCharacterAt = kRotationAt + 12;
constexpr size_t kMotionAt = kCharacterAt + 4;
constexpr size_t kMotionBytes = 16;
constexpr size_t kMotionFlagsAt = kMotionAt + kMotionBytes;
constexpr size_t kMotionSerialAt = kMotionFlagsAt + 4;
constexpr size_t kEventCountAt = kMotionSerialAt + 4;
constexpr size_t kSentAt = kEventCountAt + 4;
constexpr size_t kFlagsAt = kSentAt + 4;
constexpr size_t kClockAt = kFlagsAt + 4;
constexpr size_t kEchoIdAt = kClockAt + 4;
constexpr size_t kEchoTimeAt = kEchoIdAt + 4;
constexpr size_t kEchoHoldAt = kEchoTimeAt + 4;
constexpr size_t kPacketBytes = kEchoHoldAt + 4;
constexpr uint32_t kFlagHosting = 1;
constexpr int kLongestPing = 10000;
constexpr size_t kEventWords = 14;
constexpr size_t kEventBytes = kEventWords * 4;
constexpr uint32_t kEventKinds = 11;
constexpr size_t kMaxOthers = 3;
constexpr size_t kKeptEvents = kMostEvents * 4;
constexpr double kPeerTimeout = 5.0;
constexpr float kLargestCoordinate = 1e6f;
constexpr int kMaxPacketsPerPump = 64;
constexpr int kPortGuesses = 8;
constexpr size_t kLargestDatagram = kPacketBytes + kMostEvents * kEventBytes;
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr size_t kStunHeader = 20;
constexpr int kStunServers = 2;

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

std::string clean_name(const char* text, size_t size, size_t limit = kNameBytes) {
    std::string name;
    for (size_t i = 0; i < size && text[i] != 0 && name.size() < limit; i++) {
        unsigned char character = static_cast<unsigned char>(text[i]);
        name.push_back(character >= 0x20 && character < 0x7f ? static_cast<char>(character) : '?');
    }
    return name;
}

bool decode(const uint8_t* data, size_t size, Peer& peer) {
    if (size < kPacketBytes || get32(data) != kMagic) {
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
    for (int i = 0; i < 3; i++) {
        uint32_t bits = get32(data + kRotationAt + 4 * i);
        std::memcpy(&peer.pose.rotation[i], &bits, sizeof bits);
        if (!std::isfinite(peer.pose.rotation[i]) || std::fabs(peer.pose.rotation[i]) > kLargestCoordinate) {
            return false;
        }
    }
    peer.pose.character = get32(data + kCharacterAt);
    peer.pose.motion.clear();
    for (size_t i = 0; i < kMotionBytes && data[kMotionAt + i] != 0; i++) {
        unsigned char letter = data[kMotionAt + i];
        if (!std::isalnum(letter) && letter != '_') {
            peer.pose.motion.clear();
            break;
        }
        peer.pose.motion.push_back(static_cast<char>(letter));
    }
    peer.pose.motion_flags = get32(data + kMotionFlagsAt);
    peer.pose.motion_serial = get32(data + kMotionSerialAt);
    peer.pose.sent_ms = get32(data + kSentAt);
    peer.hosting = (get32(data + kFlagsAt) & kFlagHosting) != 0;
    peer.clock = get32(data + kClockAt);
    peer.echo_id = get32(data + kEchoIdAt);
    peer.echo_time = get32(data + kEchoTimeAt);
    peer.echo_hold = get32(data + kEchoHoldAt);
    uint32_t count = get32(data + kEventCountAt);
    if (count > kMostEvents || size != kPacketBytes + count * kEventBytes) {
        return false;
    }
    peer.pose.events.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* at = data + kPacketBytes + i * kEventBytes;
        Event& event = peer.pose.events[i];
        event.serial = get32(at);
        event.kind = get32(at + 4);
        event.id = get32(at + 8);
        event.a = get32(at + 12);
        event.b = get32(at + 16);
        event.c = get32(at + 20);
        std::array<float, 8> numbers;
        for (size_t n = 0; n < numbers.size(); n++) {
            uint32_t bits = get32(at + 24 + 4 * n);
            std::memcpy(&numbers[n], &bits, sizeof bits);
            if (!std::isfinite(numbers[n]) || std::fabs(numbers[n]) > kLargestCoordinate) {
                return false;
            }
        }
        if (event.kind >= kEventKinds) {
            return false;
        }
        event.f1 = numbers[0];
        event.f2 = numbers[1];
        event.offset = {numbers[2], numbers[3], numbers[4]};
        event.vector = {numbers[5], numbers[6], numbers[7]};
    }
    peer.zone = get32(data + 24);
    peer.name = clean_name(reinterpret_cast<const char*>(data + kNameAt), kNameBytes);
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

bool same_host(const sockaddr_storage& a, const sockaddr_storage& b) {
    if (a.ss_family != b.ss_family) {
        return false;
    }
    if (a.ss_family == AF_INET) {
        return reinterpret_cast<const sockaddr_in&>(a).sin_addr.s_addr == reinterpret_cast<const sockaddr_in&>(b).sin_addr.s_addr;
    }
    return std::memcmp(&reinterpret_cast<const sockaddr_in6&>(a).sin6_addr, &reinterpret_cast<const sockaddr_in6&>(b).sin6_addr, sizeof(in6_addr)) == 0;
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

struct Target {
    sockaddr_storage address{};
    double until = 0.0;
};

uint32_t big32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) << 24 | static_cast<uint32_t>(in[1]) << 16 | static_cast<uint32_t>(in[2]) << 8 | in[3];
}

uint16_t big16(const uint8_t* in) {
    return static_cast<uint16_t>(in[0] << 8 | in[1]);
}

bool parse_endpoint(const std::string& text, sockaddr_storage& out) {
    size_t colon = text.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    int port = std::atoi(text.c_str() + colon + 1);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (port <= 0 || port > 65535 || inet_pton(AF_INET, text.substr(0, colon).c_str(), &address.sin_addr) != 1) {
        return false;
    }
    out = sockaddr_storage{};
    std::memcpy(&out, &address, sizeof address);
    return true;
}

}

struct Link::Impl {
    Socket socket = kNoSocket;
    bool hosting = false;
    uint32_t id = 0;
    uint32_t sequence = 0;
    char name[kNameBytes] = {};
    sockaddr_storage server{};
    bool has_server = false;
    std::vector<Client> clients;
    std::vector<Target> targets;
    double now = 0.0;
    uint8_t stun_id[11] = {};
    std::string mapped[kStunServers];
    std::function<void(const uint8_t*, size_t)> relay;
    char version[kVersionBytes] = {};
    std::string other_version;
    bool host_left = false;
    bool closing = false;
    size_t echo_next = 0;

    void set_version(const std::string& text) {
        std::memset(version, 0, sizeof version);
        std::memcpy(version, text.data(), std::min(text.size(), sizeof version));
    }

    bool read_stun(const uint8_t* data, size_t size) {
        if (size < kStunHeader || big16(data) != 0x0101 || big32(data + 4) != kStunCookie || data[8] >= kStunServers || std::memcmp(data + 9, stun_id, sizeof stun_id) != 0) {
            return false;
        }
        size_t at = kStunHeader;
        size_t end = std::min(size, kStunHeader + big16(data + 2));
        while (at + 4 <= end) {
            uint16_t type = big16(data + at);
            size_t length = big16(data + at + 2);
            if (at + 4 + length > end) {
                break;
            }
            if (type == 0x0020 && length == 8 && data[at + 5] == 1) {
                uint32_t ip = big32(data + at + 8) ^ kStunCookie;
                uint16_t port = big16(data + at + 6) ^ static_cast<uint16_t>(kStunCookie >> 16);
                mapped[data[8]] = std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) + "." + std::to_string(ip & 255) + ":" + std::to_string(port);
                break;
            }
            at += 4 + ((length + 3) & ~size_t{3});
        }
        return true;
    }

    void transmit(const uint8_t* data, size_t length, const sockaddr_storage& to) const {
        if (to.ss_family == AF_UNSPEC) {
            if (relay) {
                relay(data, length);
            }
            return;
        }
        sendto(socket, reinterpret_cast<const char*>(data), static_cast<int>(length), 0, reinterpret_cast<const sockaddr*>(&to), address_size(to));
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

Link::Link() : m_impl(new Impl) {
    m_impl->set_version(WP_VERSION);
}

uint32_t Link::id() const {
    return m_impl->id;
}

void Link::set_version(const std::string& version) {
    m_impl->set_version(version);
}

std::string Link::other_version() const {
    return m_impl->other_version;
}

bool Link::host_left() const {
    return m_impl->host_left;
}

bool Link::host_present() const {
    return std::any_of(m_peers.begin(), m_peers.end(), [](const Peer& peer) { return peer.hosting; });
}

void Link::reset_route() {
    m_impl->has_server = false;
}

void Link::goodbye() {
    if (m_impl->socket == kNoSocket || m_impl->id == 0) {
        return;
    }
    m_impl->closing = true;
    uint8_t packet[kGoodbyeBytes];
    put32(packet, kGoodbye);
    put32(packet + 4, m_impl->id);
    if (m_impl->hosting) {
        for (const Client& client : m_impl->clients) {
            m_impl->transmit(packet, sizeof packet, client.address);
        }
    } else if (m_impl->has_server) {
        m_impl->transmit(packet, sizeof packet, m_impl->server);
    }
}

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
    m_impl->has_server = true;
    freeaddrinfo(found);
    m_impl->prepare(name);
    return true;
}

bool Link::open_room(bool hosting, uint16_t port, const std::string& name) {
    if (m_impl->socket != kNoSocket) {
        close_socket(m_impl->socket);
        m_impl->socket = kNoSocket;
    }
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
    m_impl->hosting = hosting;
    m_impl->has_server = false;
    m_impl->prepare(name);
    std::random_device random;
    for (uint8_t& byte : m_impl->stun_id) {
        byte = static_cast<uint8_t>(random());
    }
    return true;
}

void Link::add_target(const std::string& endpoint, double seconds) {
    sockaddr_storage address{};
    if (!parse_endpoint(endpoint, address)) {
        return;
    }
    double until = m_impl->now + seconds;
    for (Target& target : m_impl->targets) {
        if (same_address(target.address, address)) {
            target.until = until;
            return;
        }
    }
    if (m_impl->targets.size() < 16) {
        m_impl->targets.push_back({address, until});
    }
}

std::vector<std::string> Link::stun_servers() {
    std::vector<std::string> found;
    for (const char* name : {"stun.cloudflare.com", "stun.l.google.com"}) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        std::string text;
        const char* port = std::strcmp(name, "stun.cloudflare.com") == 0 ? "3478" : "19302";
        if (getaddrinfo(name, port, &hints, &result) == 0 && result) {
            char ip[INET_ADDRSTRLEN] = {};
            const auto* address = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
            if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof ip)) {
                text = std::string(ip) + ":" + port;
            }
            freeaddrinfo(result);
        }
        found.push_back(text);
    }
    return found;
}

void Link::ask_stun(const std::vector<std::string>& servers) {
    if (m_impl->socket == kNoSocket) {
        return;
    }
    for (size_t i = 0; i < servers.size() && i < kStunServers; i++) {
        sockaddr_storage to{};
        if (!parse_endpoint(servers[i], to)) {
            continue;
        }
        uint8_t request[kStunHeader] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
        request[8] = static_cast<uint8_t>(i);
        std::memcpy(request + 9, m_impl->stun_id, sizeof m_impl->stun_id);
        sendto(m_impl->socket, reinterpret_cast<const char*>(request), static_cast<int>(sizeof request), 0, reinterpret_cast<const sockaddr*>(&to), address_size(to));
    }
}

std::string Link::mapped() const {
    return m_impl->mapped[0].empty() ? m_impl->mapped[1] : m_impl->mapped[0];
}

bool Link::symmetric() const {
    return !m_impl->mapped[0].empty() && !m_impl->mapped[1].empty() && m_impl->mapped[0] != m_impl->mapped[1];
}

std::string Link::local() const {
    std::string ip = "127.0.0.1";
    Socket probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != kNoSocket) {
        sockaddr_in outside{};
        outside.sin_family = AF_INET;
        outside.sin_port = htons(53);
        inet_pton(AF_INET, "1.1.1.1", &outside.sin_addr);
        sockaddr_in own{};
        socklen_t size = sizeof own;
        char text[INET_ADDRSTRLEN] = {};
        if (connect(probe, reinterpret_cast<const sockaddr*>(&outside), sizeof outside) == 0 && getsockname(probe, reinterpret_cast<sockaddr*>(&own), &size) == 0 &&
            inet_ntop(AF_INET, &own.sin_addr, text, sizeof text)) {
            ip = text;
        }
        close_socket(probe);
    }
    return ip + ":" + std::to_string(port());
}

uint16_t Link::port() const {
    sockaddr_in address{};
    socklen_t size = sizeof address;
    if (m_impl->socket == kNoSocket || getsockname(m_impl->socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

Snapshot snapshot_at(const std::vector<Snapshot>& snapshots, double time, float teleport) {
    if (snapshots.empty()) {
        return {};
    }
    if (time <= snapshots.front().time) {
        return snapshots.front();
    }
    for (size_t i = 1; i < snapshots.size(); i++) {
        const Snapshot& before = snapshots[i - 1];
        const Snapshot& after = snapshots[i];
        if (time > after.time) {
            continue;
        }
        float gap = 0.0f;
        for (int axis = 0; axis < 3; axis++) {
            float step = after.position[axis] - before.position[axis];
            gap += step * step;
        }
        if (after.time <= before.time || gap > teleport * teleport) {
            return after;
        }
        float share = static_cast<float>((time - before.time) / (after.time - before.time));
        Snapshot blended;
        blended.time = time;
        for (int axis = 0; axis < 3; axis++) {
            blended.position[axis] = before.position[axis] + (after.position[axis] - before.position[axis]) * share;
            float turn = std::fmod(after.rotation[axis] - before.rotation[axis], 360.0f);
            if (turn > 180.0f) {
                turn -= 360.0f;
            } else if (turn < -180.0f) {
                turn += 360.0f;
            }
            blended.rotation[axis] = before.rotation[axis] + turn * share;
        }
        return blended;
    }
    return snapshots.back();
}

void Link::send(const std::array<float, 3>& position, uint32_t zone, const Pose& pose) {
    if (m_impl->socket == kNoSocket || m_impl->closing) {
        return;
    }
    uint8_t packet[kLargestDatagram] = {};
    size_t events = std::min(pose.events.size(), kMostEvents);
    size_t length = kPacketBytes + events * kEventBytes;
    put32(packet, kMagic);
    put32(packet + 4, m_impl->id);
    put32(packet + 8, ++m_impl->sequence);
    for (int i = 0; i < 3; i++) {
        uint32_t bits;
        std::memcpy(&bits, &position[i], sizeof bits);
        put32(packet + 12 + 4 * i, bits);
    }
    put32(packet + 24, zone);
    for (int i = 0; i < 3; i++) {
        uint32_t bits;
        std::memcpy(&bits, &pose.rotation[i], sizeof bits);
        put32(packet + kRotationAt + 4 * i, bits);
    }
    put32(packet + kCharacterAt, pose.character);
    std::memcpy(packet + kMotionAt, pose.motion.data(), std::min(pose.motion.size(), kMotionBytes));
    put32(packet + kMotionFlagsAt, pose.motion_flags);
    put32(packet + kMotionSerialAt, pose.motion_serial);
    put32(packet + kSentAt, pose.sent_ms);
    std::memcpy(packet + kVersionAt, m_impl->version, kVersionBytes);
    put32(packet + kFlagsAt, m_impl->hosting ? kFlagHosting : 0);
    put32(packet + kClockAt, static_cast<uint32_t>(m_impl->now * 1000.0));
    if (!m_peers.empty()) {
        const Peer& echoed = m_peers[m_impl->echo_next++ % m_peers.size()];
        put32(packet + kEchoIdAt, echoed.id);
        put32(packet + kEchoTimeAt, echoed.clock);
        put32(packet + kEchoHoldAt, static_cast<uint32_t>(std::max(0.0, m_impl->now - echoed.seen) * 1000.0));
    }
    put32(packet + kEventCountAt, static_cast<uint32_t>(events));
    for (size_t i = 0; i < events; i++) {
        uint8_t* at = packet + kPacketBytes + i * kEventBytes;
        const Event& event = pose.events[i];
        put32(at, event.serial);
        put32(at + 4, event.kind);
        put32(at + 8, event.id);
        put32(at + 12, event.a);
        put32(at + 16, event.b);
        put32(at + 20, event.c);
        const std::array<float, 8> numbers = {event.f1, event.f2, event.offset[0], event.offset[1], event.offset[2], event.vector[0], event.vector[1], event.vector[2]};
        for (size_t n = 0; n < numbers.size(); n++) {
            uint32_t bits;
            std::memcpy(&bits, &numbers[n], sizeof bits);
            put32(at + 24 + 4 * n, bits);
        }
    }
    std::memcpy(packet + kNameAt, m_impl->name, kNameBytes);
    if (m_impl->hosting) {
        for (const Client& client : m_impl->clients) {
            m_impl->transmit(packet, length, client.address);
        }
    } else if (m_impl->has_server) {
        m_impl->transmit(packet, length, m_impl->server);
    }
    if (!m_impl->hosting && !m_impl->has_server && m_impl->relay) {
        m_impl->relay(packet, length);
    }
    if (m_impl->hosting || !m_impl->has_server) {
        for (const Target& target : m_impl->targets) {
            bool known = std::any_of(m_impl->clients.begin(), m_impl->clients.end(), [&](const Client& c) { return same_address(c.address, target.address); });
            if (known || target.until <= m_impl->now) {
                continue;
            }
            m_impl->transmit(packet, length, target.address);
            if (target.address.ss_family != AF_INET) {
                continue;
            }
            for (int offset = -kPortGuesses; offset <= kPortGuesses; offset++) {
                sockaddr_storage guess = target.address;
                auto& ipv4 = reinterpret_cast<sockaddr_in&>(guess);
                int port = ntohs(ipv4.sin_port) + offset;
                if (offset != 0 && port > 0 && port <= 65535) {
                    ipv4.sin_port = htons(static_cast<uint16_t>(port));
                    m_impl->transmit(packet, length, guess);
                }
            }
        }
    }
}

void Link::use_relay(std::function<void(const uint8_t*, size_t)> out) {
    m_impl->relay = std::move(out);
}

void Link::relay_packet(const uint8_t* data, size_t size) {
    sockaddr_storage from{};
    accept(data, size, &from, true);
}

void Link::accept(const uint8_t* data, size_t size, const void* source, bool relayed) {
    const sockaddr_storage& from = *static_cast<const sockaddr_storage*>(source);
    double now = m_impl->now;
    if (!relayed && m_impl->read_stun(data, size)) {
        return;
    }
    static const bool relay_only = std::getenv("WP_ONLINE_RELAY_ONLY") != nullptr;
    if (relay_only && !relayed) {
        return;
    }
    if (size == kGoodbyeBytes && get32(data) == kGoodbye) {
        uint32_t leaving = get32(data + 4);
        if (m_impl->hosting) {
            auto client = std::find_if(m_impl->clients.begin(), m_impl->clients.end(), [&](const Client& c) { return c.id == leaving; });
            if (client == m_impl->clients.end() || !same_address(client->address, from)) {
                return;
            }
            m_impl->clients.erase(client);
            for (const Client& other : m_impl->clients) {
                if (!(relayed && other.address.ss_family == AF_UNSPEC)) {
                    m_impl->transmit(data, size, other.address);
                }
            }
        } else if (!m_impl->has_server || !same_address(m_impl->server, from)) {
            return;
        }
        auto peer = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return p.id == leaving; });
        if (peer != m_peers.end()) {
            m_impl->host_left = m_impl->host_left || peer->hosting;
            m_peers.erase(peer);
        }
        return;
    }
    if (size >= kNameAt && get32(data) == kMagic && std::memcmp(data + kVersionAt, m_impl->version, kVersionBytes) != 0) {
        m_impl->other_version = clean_name(reinterpret_cast<const char*>(data + kVersionAt), kVersionBytes, kVersionBytes);
        return;
    }
    Peer incoming;
    if (!decode(data, size, incoming) || incoming.id == m_impl->id) {
        return;
    }
    if (m_impl->hosting) {
        auto client = std::find_if(m_impl->clients.begin(), m_impl->clients.end(), [&](const Client& c) { return c.id == incoming.id; });
        if (client == m_impl->clients.end()) {
            if (m_impl->clients.size() >= kMaxOthers) {
                return;
            }
            m_impl->clients.push_back({from, incoming.id, now});
            client = m_impl->clients.end() - 1;
        } else if (!same_address(client->address, from)) {
            return;
        }
        client->seen = now;
    } else if (!m_impl->has_server) {
        bool expected = relayed || std::any_of(m_impl->targets.begin(), m_impl->targets.end(), [&](const Target& target) { return same_host(target.address, from); });
        if (!expected) {
            return;
        }
        m_impl->server = from;
        m_impl->has_server = true;
        m_impl->targets.clear();
    } else if (!same_address(m_impl->server, from)) {
        return;
    }
    auto peer = std::find_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return p.id == incoming.id; });
    if (peer != m_peers.end() && static_cast<int32_t>(incoming.sequence - peer->sequence) <= 0) {
        return;
    }
    if (m_impl->hosting) {
        for (const Client& client : m_impl->clients) {
            if (client.id != incoming.id && !(relayed && client.address.ss_family == AF_UNSPEC)) {
                m_impl->transmit(data, size, client.address);
            }
        }
    }
    incoming.seen = now;
    incoming.ping_ms = peer != m_peers.end() ? peer->ping_ms : -1;
    if (incoming.echo_id == m_impl->id && incoming.echo_time != 0) {
        int ping = static_cast<int>(static_cast<uint32_t>(now * 1000.0) - incoming.echo_time - incoming.echo_hold);
        if (ping >= 0 && ping < kLongestPing) {
            incoming.ping_ms = incoming.ping_ms < 0 ? ping : (incoming.ping_ms * 7 + ping) / 8;
        }
    }
    if (peer != m_peers.end()) {
        std::vector<Event> events;
        for (const Event& old : peer->pose.events) {
            if (std::none_of(incoming.pose.events.begin(), incoming.pose.events.end(), [&](const Event& event) { return event.serial == old.serial; })) {
                events.push_back(old);
            }
        }
        events.insert(events.end(), incoming.pose.events.begin(), incoming.pose.events.end());
        if (events.size() > kKeptEvents) {
            events.erase(events.begin(), events.end() - kKeptEvents);
        }
        incoming.pose.events = std::move(events);
        *peer = incoming;
    } else if (m_peers.size() < kMaxOthers) {
        m_peers.push_back(incoming);
    }
}

void Link::pump(double now) {
    if (m_impl->socket == kNoSocket) {
        return;
    }
    m_impl->now = now;
    uint8_t data[kLargestDatagram];
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
        accept(data, static_cast<size_t>(got), &from, false);
    }
    m_peers.erase(std::remove_if(m_peers.begin(), m_peers.end(), [&](const Peer& p) { return now - p.seen > kPeerTimeout; }), m_peers.end());
    auto& clients = m_impl->clients;
    clients.erase(std::remove_if(clients.begin(), clients.end(), [&](const Client& c) { return now - c.seen > kPeerTimeout; }), clients.end());
}

}
