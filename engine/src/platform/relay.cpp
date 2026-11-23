#include "wp/relay.h"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include "wp/cloud_sync.h"
#include "wp/log.h"

namespace wp::relay {

namespace {

constexpr auto kSendEvery = std::chrono::milliseconds(66);
constexpr size_t kLargestPacket = 1200;
constexpr size_t kMostWaiting = 64;
constexpr size_t kSenderAt = 4;

uint32_t sender_of(const uint8_t* data, size_t size) {
    if (size < kSenderAt + 4) {
        return 0;
    }
    return static_cast<uint32_t>(data[kSenderAt]) << 24 | static_cast<uint32_t>(data[kSenderAt + 1]) << 16 | static_cast<uint32_t>(data[kSenderAt + 2]) << 8 | data[kSenderAt + 3];
}

}

struct Socket::Impl {
    std::mutex mutex;
    std::map<uint32_t, std::vector<uint8_t>> outgoing;
    std::vector<std::vector<uint8_t>> incoming;
    std::atomic<bool> stop{false};
    std::thread reader;
    std::thread writer;
#ifdef _WIN32
    HINTERNET session = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET socket = nullptr;

    void read_loop() {
        std::vector<uint8_t> packet;
        uint8_t buffer[kLargestPacket];
        while (!stop.load()) {
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            if (WinHttpWebSocketReceive(socket, buffer, sizeof buffer, &got, &type) != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                return;
            }
            packet.insert(packet.end(), buffer, buffer + got);
            if (type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE && packet.size() <= kLargestPacket) {
                continue;
            }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE && packet.size() <= kLargestPacket) {
                std::lock_guard<std::mutex> lock(mutex);
                if (incoming.size() < kMostWaiting) {
                    incoming.push_back(packet);
                }
            }
            packet.clear();
        }
    }

    void write_loop() {
        while (!stop.load()) {
            std::map<uint32_t, std::vector<uint8_t>> batch;
            {
                std::lock_guard<std::mutex> lock(mutex);
                batch.swap(outgoing);
            }
            for (auto& [sender, packet] : batch) {
                if (WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, packet.data(), static_cast<DWORD>(packet.size())) != NO_ERROR) {
                    return;
                }
            }
            std::this_thread::sleep_for(kSendEvery);
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
        if (socket) {
            WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket);
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (writer.joinable()) {
            writer.join();
        }
        if (connection) {
            WinHttpCloseHandle(connection);
        }
        if (session) {
            WinHttpCloseHandle(session);
        }
    }
#else
    bool connect(const std::string&) {
        return false;
    }

    void close() {}
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
#ifdef _WIN32
    m_impl->reader = std::thread([impl = m_impl.get()] { impl->read_loop(); });
    m_impl->writer = std::thread([impl = m_impl.get()] { impl->write_loop(); });
#endif
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

}
