#include "wp/relay.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "wp/cloud_sync.h"
#include "wp/log.h"

namespace wp::relay {

namespace {

constexpr auto kSendEvery = std::chrono::milliseconds(66);
constexpr size_t kLargestPacket = 1200;
constexpr size_t kMostWaiting = 64;
constexpr size_t kSenderAt = 4;
constexpr uint8_t kBinary = 2;
constexpr uint8_t kClose = 8;
constexpr uint8_t kPing = 9;
constexpr uint8_t kPong = 10;
constexpr uint8_t kFinal = 0x80;
constexpr uint8_t kMasked = 0x80;
constexpr size_t kLongestHeader = 4096;
constexpr int64_t kSilentMs = 10000;

uint32_t sender_of(const uint8_t* data, size_t size) {
    if (size < kSenderAt + 4) {
        return 0;
    }
    return static_cast<uint32_t>(data[kSenderAt]) << 24 | static_cast<uint32_t>(data[kSenderAt + 1]) << 16 | static_cast<uint32_t>(data[kSenderAt + 2]) << 8 | data[kSenderAt + 3];
}

}

std::vector<uint8_t> frame(const uint8_t* data, size_t size, const uint8_t mask[4]) {
    std::vector<uint8_t> out = {static_cast<uint8_t>(kFinal | kBinary)};
    if (size < 126) {
        out.push_back(static_cast<uint8_t>(kMasked | size));
    } else {
        out.push_back(kMasked | 126);
        out.push_back(static_cast<uint8_t>(size >> 8));
        out.push_back(static_cast<uint8_t>(size));
    }
    out.insert(out.end(), mask, mask + 4);
    for (size_t i = 0; i < size; i++) {
        out.push_back(data[i] ^ mask[i % 4]);
    }
    return out;
}

bool unframe(std::vector<uint8_t>& stream, std::vector<uint8_t>& message, int& opcode) {
    if (stream.size() < 2) {
        return false;
    }
    size_t length = stream[1] & 0x7f;
    size_t at = 2;
    if (length == 126) {
        if (stream.size() < 4) {
            return false;
        }
        length = static_cast<size_t>(stream[2]) << 8 | stream[3];
        at = 4;
    } else if (length == 127) {
        if (stream.size() < 10) {
            return false;
        }
        length = 0;
        for (size_t i = 2; i < 10; i++) {
            length = length << 8 | stream[i];
        }
        at = 10;
    }
    if (length > kLargestPacket) {
        opcode = kClose;
        message.clear();
        stream.clear();
        return true;
    }
    size_t mask_at = at;
    bool masked = (stream[1] & kMasked) != 0;
    at += masked ? 4 : 0;
    if (stream.size() < at + length) {
        return false;
    }
    opcode = stream[0] & 0x0f;
    message.assign(stream.begin() + static_cast<std::ptrdiff_t>(at), stream.begin() + static_cast<std::ptrdiff_t>(at + length));
    if (masked) {
        for (size_t i = 0; i < length; i++) {
            message[i] ^= stream[mask_at + i % 4];
        }
    }
    stream.erase(stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(at + length));
    return true;
}

struct Socket::Impl {
    std::mutex mutex;
    std::mutex send_mutex;
    std::map<uint32_t, std::vector<uint8_t>> outgoing;
    std::vector<std::vector<uint8_t>> incoming;
    std::atomic<bool> stop{false};
    std::atomic<bool> up{false};
    std::atomic<int> refused{0};
    std::atomic<int64_t> heard{0};
    std::thread reader;
    std::thread writer;

    static int64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void receive(std::vector<uint8_t>&& packet) {
        heard.store(now_ms());
        std::lock_guard<std::mutex> lock(mutex);
        if (incoming.size() < kMostWaiting) {
            incoming.push_back(std::move(packet));
        }
    }

    bool send_waiting() {
        std::map<uint32_t, std::vector<uint8_t>> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            batch.swap(outgoing);
        }
        std::lock_guard<std::mutex> lock(send_mutex);
        for (auto& [sender, packet] : batch) {
            if (!write(packet)) {
                return false;
            }
        }
        return true;
    }

    void write_loop() {
        while (!stop.load()) {
            if (!send_waiting()) {
                up.store(false);
                return;
            }
            std::this_thread::sleep_for(kSendEvery);
        }
    }

#ifdef _WIN32
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;

    bool write(const std::vector<uint8_t>& packet) {
        return WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<uint8_t*>(packet.data()), static_cast<DWORD>(packet.size())) == NO_ERROR;
    }

    void read_loop() {
        std::vector<uint8_t> packet;
        uint8_t buffer[kLargestPacket];
        while (!stop.load()) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            if (WinHttpWebSocketReceive(socket, buffer, sizeof buffer, &got, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                up.store(false);
                return;
            }
            packet.insert(packet.end(), buffer, buffer + got);
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE && packet.size() <= kLargestPacket) {
                continue;
            }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && packet.size() <= kLargestPacket) {
                receive(std::move(packet));
            }
            packet.clear();
        }
    }

    bool connect(const std::string& code) {
        session = WinHttpOpen(L"WondersBeyondRecomp", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session) {
            return false;
        }
        WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
        std::string host = cloud::rooms_host();
        connection = WinHttpConnect(session, std::wstring(host.begin(), host.end()).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection) {
            return false;
        }
        std::string path = "/v1/rooms/" + code + "/relay";
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", std::wstring(path.begin(), path.end()).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request) {
            return false;
        }
        DWORD status = 0;
        DWORD size = sizeof status;
        bool ok = WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr) &&
                  WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 101;
        if (ok) {
            socket = WinHttpWebSocketCompleteUpgrade(request, 0);
        } else {
            refused.store(static_cast<int>(status));
            log::write("online", "relay refused, status %lu", static_cast<unsigned long>(status));
        }
        WinHttpCloseHandle(request);
        if (!socket) {
            return false;
        }
        DWORD no_timeout = 0;
        WinHttpSetOption(socket, WINHTTP_OPTION_RECEIVE_TIMEOUT, &no_timeout, sizeof no_timeout);
        return true;
    }

    void close() {
        stop.store(true);
        if (writer.joinable()) {
            writer.join();
        }
        if (socket) {
            if (up.load()) {
                send_waiting();
            }
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (connection) {
            WinHttpCloseHandle(connection);
        }
        if (session) {
            WinHttpCloseHandle(session);
        }
    }
#else
    int socket = -1;

    bool write_all(const uint8_t* data, size_t size) {
        size_t done = 0;
        while (done < size) {
#ifdef MSG_NOSIGNAL
            ssize_t sent = ::send(socket, data + done, size - done, MSG_NOSIGNAL);
#else
            ssize_t sent = ::send(socket, data + done, size - done, 0);
#endif
            if (sent <= 0) {
                return false;
            }
            done += static_cast<size_t>(sent);
        }
        return true;
    }

    bool write(const std::vector<uint8_t>& packet) {
        static thread_local std::mt19937 random{std::random_device{}()};
        uint32_t bits = random();
        uint8_t mask[4];
        std::memcpy(mask, &bits, sizeof mask);
        std::vector<uint8_t> framed = frame(packet.data(), packet.size(), mask);
        return write_all(framed.data(), framed.size());
    }

    void read_loop(std::vector<uint8_t> stream) {
        uint8_t buffer[2048];
        std::vector<uint8_t> message;
        while (!stop.load()) {
            int opcode = 0;
            while (unframe(stream, message, opcode)) {
                if (opcode == kBinary) {
                    receive(std::move(message));
                    message.clear();
                } else if (opcode == kPing) {
                    std::vector<uint8_t> reply = {static_cast<uint8_t>(kFinal | kPong), kMasked, 0, 0, 0, 0};
                    std::lock_guard<std::mutex> lock(send_mutex);
                    write_all(reply.data(), reply.size());
                } else if (opcode == kClose) {
                    up.store(false);
                    return;
                }
            }
            pollfd waiting{socket, POLLIN, 0};
            int ready = ::poll(&waiting, 1, 250);
            if (ready < 0) {
                up.store(false);
                return;
            }
            if (ready == 0) {
                continue;
            }
            ssize_t got = ::recv(socket, buffer, sizeof buffer, 0);
            if (got <= 0) {
                up.store(false);
                return;
            }
            stream.insert(stream.end(), buffer, buffer + got);
        }
    }

    bool connect(const std::string& code) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(cloud::rooms_host(), "80", &hints, &found) != 0) {
            return false;
        }
        timeval patience{4, 0};
        for (addrinfo* entry = found; entry; entry = entry->ai_next) {
            socket = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (socket < 0) {
                continue;
            }
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
            if (::connect(socket, entry->ai_addr, entry->ai_addrlen) == 0) {
                break;
            }
            ::close(socket);
            socket = -1;
        }
        freeaddrinfo(found);
        if (socket < 0) {
            return false;
        }
        int on = 1;
        setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
#ifdef SO_NOSIGPIPE
        setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
        std::string request = "GET /v1/rooms/" + code + "/relay HTTP/1.1\r\nHost: " + cloud::rooms_host() +
                              "\r\nUser-Agent: WondersBeyondRecomp\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: V29uZGVyc0JleW9uZFJlbGF5\r\n\r\n";
        if (!write_all(reinterpret_cast<const uint8_t*>(request.data()), request.size())) {
            return false;
        }
        std::string header;
        char byte = 0;
        while (header.size() < kLongestHeader && header.find("\r\n\r\n") == std::string::npos) {
            if (::recv(socket, &byte, 1, 0) != 1) {
                return false;
            }
            header.push_back(byte);
        }
        size_t space = header.find(' ');
        int status = space == std::string::npos ? 0 : std::atoi(header.c_str() + space + 1);
        if (status != 101) {
            refused.store(status);
            log::write("online", "relay refused, status %d", status);
            return false;
        }
        timeval forever{0, 0};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &forever, sizeof forever);
        return true;
    }

    void close() {
        stop.store(true);
        if (writer.joinable()) {
            writer.join();
        }
        if (socket >= 0 && up.load()) {
            send_waiting();
            const uint8_t closing[] = {static_cast<uint8_t>(kFinal | kClose), kMasked, 0, 0, 0, 0};
            write_all(closing, sizeof closing);
        }
        if (socket >= 0) {
            ::shutdown(socket, SHUT_RDWR);
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (socket >= 0) {
            ::close(socket);
        }
    }
#endif
};

Socket::Socket() : m_impl(std::make_unique<Impl>()) {}

Socket::~Socket() {
    m_impl->close();
}

bool Socket::open(const std::string& code) {
    if (!m_impl->connect(code)) {
        return false;
    }
    m_impl->up.store(true);
#ifdef _WIN32
    m_impl->reader = std::thread([impl = m_impl.get()] { impl->read_loop(); });
#else
    m_impl->reader = std::thread([impl = m_impl.get()] { impl->read_loop({}); });
#endif
    m_impl->writer = std::thread([impl = m_impl.get()] { impl->write_loop(); });
    return true;
}

void Socket::send(const uint8_t* data, size_t size) {
    if (size > kLargestPacket) {
        return;
    }
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->outgoing[sender_of(data, size)].assign(data, data + size);
}

std::vector<std::vector<uint8_t>> Socket::take() {
    std::vector<std::vector<uint8_t>> packets;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    packets.swap(m_impl->incoming);
    return packets;
}

bool Socket::alive() const {
    int64_t heard = m_impl->heard.load();
    return m_impl->up.load() && (heard == 0 || Impl::now_ms() - heard < kSilentMs);
}

int Socket::refused() const {
    return m_impl->refused.load();
}

void Socket::flush() {
    if (m_impl->up.load()) {
        m_impl->send_waiting();
    }
}

}
