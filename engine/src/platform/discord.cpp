#include "wp/discord.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "wp/game.h"
#include "wp/log.h"
#include "wp/online.h"
#include "wp/settings.h"

namespace wp::discord {

namespace {

// Discord's local IPC: frames of a little-endian opcode and length followed by JSON.
constexpr uint32_t kHandshake = 0;
constexpr uint32_t kFrame = 1;
constexpr auto kRetry = std::chrono::seconds(15);
constexpr uint32_t kLargestFrame = 64 * 1024;
constexpr size_t kMostPlayers = 4;

class Pipe {
public:
    ~Pipe() { close(); }

    bool open() {
#ifdef _WIN32
        for (int i = 0; i < 10; i++) {
            std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(i);
            handle_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                return true;
            }
        }
        return false;
#else
        const char* folders[] = {std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"), "/tmp"};
        for (const char* folder : folders) {
            if (!folder) {
                continue;
            }
            for (int i = 0; i < 10; i++) {
                std::string path = std::string(folder) + "/discord-ipc-" + std::to_string(i);
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::snprintf(address.sun_path, sizeof address.sun_path, "%s", path.c_str());
                socket_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (socket_ >= 0 && ::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0) {
                    return true;
                }
                close();
            }
        }
        return false;
#endif
    }

    void close() {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
#else
        if (socket_ >= 0) {
            ::close(socket_);
            socket_ = -1;
        }
#endif
    }

    bool send(uint32_t opcode, const std::string& json) {
        std::string frame(8, '\0');
        uint32_t length = static_cast<uint32_t>(json.size());
        for (int i = 0; i < 4; i++) {
            frame[i] = static_cast<char>((opcode >> (8 * i)) & 0xFF);
            frame[4 + i] = static_cast<char>((length >> (8 * i)) & 0xFF);
        }
        frame += json;
        return write(frame.data(), frame.size());
    }

    int waiting() {
#ifdef _WIN32
        DWORD available = 0;
        if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
            return -1;
        }
        return available > 0 ? 1 : 0;
#else
        pollfd entry{socket_, POLLIN, 0};
        int ready = ::poll(&entry, 1, 0);
        return ready < 0 ? -1 : ready > 0 ? 1 : 0;
#endif
    }

    bool receive(std::string* text = nullptr) {
        char header[8];
        if (!read(header, sizeof header)) {
            return false;
        }
        uint32_t length = 0;
        for (int i = 0; i < 4; i++) {
            length |= static_cast<uint32_t>(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        }
        if (length > kLargestFrame) {
            return false;
        }
        std::string body(length, '\0');
        if (length != 0 && !read(body.data(), length)) {
            return false;
        }
        if (text) {
            *text = std::move(body);
        }
        return true;
    }

private:
    bool write(const char* data, size_t size) {
#ifdef _WIN32
        DWORD written = 0;
        return WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
#else
        return ::send(socket_, data, size, MSG_NOSIGNAL) == static_cast<ssize_t>(size);
#endif
    }

    bool read(char* data, size_t size) {
        size_t done = 0;
        while (done < size) {
#ifdef _WIN32
            DWORD got = 0;
            if (!ReadFile(handle_, data + done, static_cast<DWORD>(size - done), &got, nullptr) || got == 0) {
                return false;
            }
#else
            ssize_t got = ::recv(socket_, data + done, size - done, 0);
            if (got <= 0) {
                return false;
            }
#endif
            done += static_cast<size_t>(got);
        }
        return true;
    }

#ifdef _WIN32
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int socket_ = -1;
#endif
};

int process_id() {
#ifdef _WIN32
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}

std::string nonce() {
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string activity_json(int64_t started, bool shown, const std::string& code, size_t players) {
    const game::Description& game = game::description();
    std::string activity = "null";
    if (shown) {
        activity = std::string("{\"details\":\"") + game.title + "\",\"timestamps\":{\"start\":" + std::to_string(started) +
                   "},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"" + game.app_name + "\"}" + party_json(code, players) + "}";
    }
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(process_id()) + ",\"activity\":" + activity + "},\"nonce\":\"" + nonce() + "\"}";
}

bool read_events(Pipe& pipe) {
    while (true) {
        int waiting = pipe.waiting();
        if (waiting <= 0) {
            return waiting == 0;
        }
        std::string frame;
        if (!pipe.receive(&frame)) {
            return false;
        }
        if (frame.find("\"evt\":\"ERROR\"") != std::string::npos) {
            log::write("discord", "Discord refused: %s", frame.substr(0, 300).c_str());
        }
        std::string code = join_secret(frame);
        if (!code.empty() && code != online::room_code()) {
            log::write("discord", "joining a room from an invitation");
            online::join(code, settings::text("online.name", nullptr));
        }
    }
}

void worker(std::string application) {
    static const settings::LiveFlag enabled("system.discord", "WP_DISCORD");
    int64_t started = static_cast<int64_t>(std::time(nullptr));
    Pipe pipe;
    bool connected = false;
    std::string shown;
    while (true) {
        bool wanted = enabled();
        if (!connected && wanted) {
            connected = pipe.open() && pipe.send(kHandshake, "{\"v\":1,\"client_id\":\"" + application + "\"}") && pipe.receive() &&
                        pipe.send(kFrame, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"args\":{},\"nonce\":\"" + nonce() + "\"}");
            if (connected) {
                log::write("discord", "connected to Discord");
            } else {
                pipe.close();
            }
            shown.clear();
        }
        if (connected) {
            std::string code = wanted && online::state() == online::State::Hosting ? online::room_code() : "";
            size_t players = std::min(online::others() + 1, kMostPlayers);
            std::string state = std::to_string(wanted) + code + (code.empty() ? "" : std::to_string(players));
            if (state != shown) {
                connected = pipe.send(kFrame, activity_json(started, wanted, code, players));
                shown = connected ? state : "";
            }
            connected = connected && read_events(pipe);
            if (!connected) {
                pipe.close();
            }
        }
        std::this_thread::sleep_for(connected ? std::chrono::seconds(1) : kRetry);
    }
}

}

std::string party_json(const std::string& code, size_t players) {
    if (code.empty()) {
        return "";
    }
    return ",\"party\":{\"id\":\"wonders-" + code + "\",\"size\":[" + std::to_string(players) + "," + std::to_string(kMostPlayers) + "]},\"secrets\":{\"join\":\"" + code + "\"},\"instance\":true";
}

std::string join_secret(const std::string& frame) {
    static const std::string kKey = "\"secret\":\"";
    if (frame.find("\"evt\":\"ACTIVITY_JOIN\"") == std::string::npos || frame.find("\"cmd\":\"DISPATCH\"") == std::string::npos) {
        return "";
    }
    size_t at = frame.find(kKey);
    size_t end = at == std::string::npos ? at : frame.find('"', at + kKey.size());
    if (end == std::string::npos) {
        return "";
    }
    std::string code = frame.substr(at + kKey.size(), end - at - kKey.size());
    return online::valid_code(code) ? code : "";
}

void start() {
    const char* application = game::description().discord_application;
    if (!application || !*application) {
        return;
    }
    std::thread(worker, std::string(application)).detach();
}

}
