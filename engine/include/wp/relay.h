#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wp::relay {

class Socket {
public:
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    bool open(const std::string& code);
    void send(const uint8_t* data, size_t size);
    std::vector<std::vector<uint8_t>> take();
    bool alive() const;
    int refused() const;
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

std::vector<uint8_t> frame(const uint8_t* data, size_t size, const uint8_t mask[4]);
bool unframe(std::vector<uint8_t>& stream, std::vector<uint8_t>& message, int& opcode);

}
