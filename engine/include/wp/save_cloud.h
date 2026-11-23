#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace wp::cloud {

using Bytes = std::vector<uint8_t>;
using Key = std::array<uint8_t, 32>;
using Nonce = std::array<uint8_t, 12>;

struct Keys {
    std::string id;
    Key key{};
};

struct SaveFile {
    std::string name;
    Bytes data;
};

struct Snapshot {
    int64_t saved_at = 0;
    uint32_t play_seconds = 0;
    std::string device;
    std::vector<SaveFile> files;
};

enum class Decision { Nothing, Upload, Download, Ask };

struct Facts {
    std::string local;
    std::string agreed;
    std::string remote;
    bool at_start = false;
};

std::string sha256_hex(const uint8_t* data, size_t size);
Bytes pbkdf2_sha256(const std::string& password, const std::string& salt, uint32_t iterations, size_t size);
Bytes seal_with_nonce(const Key& key, const Nonce& nonce, const Bytes& extra, const Bytes& plain);
bool open_sealed(const Key& key, const Nonce& nonce, const Bytes& extra, const uint8_t* sealed, size_t size, Bytes& plain);

std::string new_code();
bool clean_code(const std::string& typed, std::string& code);
std::string shown_code(const std::string& code);
Keys derive_keys(const std::string& code);
Bytes seal(const Key& key, const Snapshot& snapshot);
bool open(const Key& key, const Bytes& sealed, Snapshot& snapshot);

bool read_save(const std::filesystem::path& folder, Snapshot& snapshot);
bool write_save(const std::filesystem::path& folder, const Snapshot& snapshot);
std::string progress_hash(const Snapshot& snapshot);
Decision decide(const Facts& facts);

}
