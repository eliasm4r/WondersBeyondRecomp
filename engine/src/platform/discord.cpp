#include "wp/discord.h"

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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "wp/game.h"
#include "wp/log.h"
#include "wp/settings.h"

namespace wp::discord {

namespace {

// Discord's local IPC: frames of a little-endian opcode and length followed by JSON.
constexpr uint32_t kHandshake = 0;
constexpr uint32_t kFrame = 1;
constexpr auto kRetry = std::chrono::seconds(15);

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

    // Reads and discards one reply frame; false when the connection is gone.
    bool receive() {
        char header[8];
        if (!read(header, sizeof header)) {
            return false;
        }
        uint32_t length = 0;
        for (int i = 0; i < 4; i++) {
            length |= static_cast<uint32_t>(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        }
        std::string body(length, '\0');
        return length == 0 || read(body.data(), length);
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

std::string activity_json(int64_t started, bool shown) {
    const game::Description& game = game::description();
    std::string activity = "null";
    if (shown) {
        activity = std::string("{\"details\":\"") + game.title + "\",\"timestamps\":{\"start\":" + std::to_string(started) +
                   "},\"assets\":{\"large_image\":\"logo\",\"large_text\":\"" + game.app_name + "\"}}";
    }
    return "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(process_id()) + ",\"activity\":" + activity + "},\"nonce\":\"" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "\"}";
}

void worker(std::string application) {
    static const settings::LiveFlag enabled("system.discord", "WP_DISCORD");
    int64_t started = static_cast<int64_t>(std::time(nullptr));
    Pipe pipe;
    bool connected = false;
    int shown = -1;
    while (true) {
        bool wanted = enabled();
        if (!connected && wanted) {
            connected = pipe.open() && pipe.send(kHandshake, "{\"v\":1,\"client_id\":\"" + application + "\"}") && pipe.receive();
            if (connected) {
                log::write("discord", "connected to Discord");
            } else {
                pipe.close();
            }
            shown = -1;
        }
        if (connected && shown != static_cast<int>(wanted)) {
            connected = pipe.send(kFrame, activity_json(started, wanted)) && pipe.receive();
            shown = connected ? static_cast<int>(wanted) : -1;
            if (!connected) {
                pipe.close();
            }
        }
        std::this_thread::sleep_for(connected ? std::chrono::seconds(1) : kRetry);
    }
}

}

void start() {
    const char* application = game::description().discord_application;
    if (!application || !*application) {
        return;
    }
    std::thread(worker, std::string(application)).detach();
}

}
