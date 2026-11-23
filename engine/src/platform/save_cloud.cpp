#include "wp/save_cloud.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#endif

namespace wp::cloud {

namespace {

namespace fs = std::filesystem;

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kCodeLength = 10;
constexpr char kSalt[] = "WondersBeyondRecomp save sync v1";
constexpr uint32_t kIterations = 200000;
constexpr char kExtra[] = "WBR save v1";
constexpr uint32_t kFormat = 0x31534257;
constexpr size_t kTagBytes = 16;
constexpr size_t kLargestFile = 64 * 1024;
constexpr size_t kLongestDevice = 40;
constexpr const char* kFiles[] = {"GD0", "GD0M", "GD1", "GD1M", "banner.bin"};
constexpr const char* kProgressFiles[] = {"GD0", "GD0M", "GD1", "GD1M"};
constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74,
    0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d,
    0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e,
    0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t right(uint32_t value, int bits) {
    return (value >> bits) | (value << (32 - bits));
}

uint32_t left(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

uint32_t le32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | static_cast<uint32_t>(in[1]) << 8 | static_cast<uint32_t>(in[2]) << 16 | static_cast<uint32_t>(in[3]) << 24;
}

void put_le32(uint8_t* out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

class Sha256 {
public:
    void add(const uint8_t* data, size_t size) {
        total_ += size;
        while (size > 0) {
            size_t count = std::min(size, sizeof block_ - used_);
            std::memcpy(block_ + used_, data, count);
            used_ += count;
            data += count;
            size -= count;
            if (used_ == sizeof block_) {
                compress();
                used_ = 0;
            }
        }
    }

    std::array<uint8_t, 32> finish() {
        uint64_t bits = total_ * 8;
        uint8_t mark = 0x80;
        add(&mark, 1);
        uint8_t zero = 0;
        while (used_ != 56) {
            add(&zero, 1);
        }
        uint8_t length[8];
        for (int i = 0; i < 8; i++) {
            length[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
        }
        add(length, sizeof length);
        std::array<uint8_t, 32> out{};
        for (int i = 0; i < 8; i++) {
            for (int j = 0; j < 4; j++) {
                out[static_cast<size_t>(4 * i + j)] = static_cast<uint8_t>(state_[i] >> (24 - 8 * j));
            }
        }
        return out;
    }

private:
    void compress() {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = static_cast<uint32_t>(block_[4 * i]) << 24 | static_cast<uint32_t>(block_[4 * i + 1]) << 16 | static_cast<uint32_t>(block_[4 * i + 2]) << 8 |
                   block_[4 * i + 3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = right(w[i - 15], 7) ^ right(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = right(w[i - 2], 17) ^ right(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        std::memcpy(v, state_, sizeof v);
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = v[7] + (right(v[4], 6) ^ right(v[4], 11) ^ right(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + kRound[i] + w[i];
            uint32_t t2 = (right(v[0], 2) ^ right(v[0], 13) ^ right(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v[7] = v[6];
            v[6] = v[5];
            v[5] = v[4];
            v[4] = v[3] + t1;
            v[3] = v[2];
            v[2] = v[1];
            v[1] = v[0];
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; i++) {
            state_[i] += v[i];
        }
    }

    uint32_t state_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t block_[64] = {};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

class Hmac {
public:
    explicit Hmac(const std::string& key) {
        uint8_t pad[64] = {};
        if (key.size() > sizeof pad) {
            Sha256 hash;
            hash.add(reinterpret_cast<const uint8_t*>(key.data()), key.size());
            auto digest = hash.finish();
            std::memcpy(pad, digest.data(), digest.size());
        } else {
            std::memcpy(pad, key.data(), key.size());
        }
        uint8_t inner[64];
        uint8_t outer[64];
        for (size_t i = 0; i < sizeof pad; i++) {
            inner[i] = pad[i] ^ 0x36;
            outer[i] = pad[i] ^ 0x5c;
        }
        inner_.add(inner, sizeof inner);
        outer_.add(outer, sizeof outer);
    }

    std::array<uint8_t, 32> of(const uint8_t* data, size_t size) const {
        Sha256 inner = inner_;
        inner.add(data, size);
        auto digest = inner.finish();
        Sha256 outer = outer_;
        outer.add(digest.data(), digest.size());
        return outer.finish();
    }

private:
    Sha256 inner_;
    Sha256 outer_;
};

void chacha_block(const Key& key, uint32_t counter, const Nonce& nonce, uint8_t out[64]) {
    uint32_t start[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
    for (int i = 0; i < 8; i++) {
        start[4 + i] = le32(key.data() + 4 * i);
    }
    start[12] = counter;
    for (int i = 0; i < 3; i++) {
        start[13 + i] = le32(nonce.data() + 4 * i);
    }
    uint32_t x[16];
    std::memcpy(x, start, sizeof x);
    auto quarter = [&](int a, int b, int c, int d) {
        x[a] += x[b];
        x[d] = left(x[d] ^ x[a], 16);
        x[c] += x[d];
        x[b] = left(x[b] ^ x[c], 12);
        x[a] += x[b];
        x[d] = left(x[d] ^ x[a], 8);
        x[c] += x[d];
        x[b] = left(x[b] ^ x[c], 7);
    };
    for (int i = 0; i < 10; i++) {
        quarter(0, 4, 8, 12);
        quarter(1, 5, 9, 13);
        quarter(2, 6, 10, 14);
        quarter(3, 7, 11, 15);
        quarter(0, 5, 10, 15);
        quarter(1, 6, 11, 12);
        quarter(2, 7, 8, 13);
        quarter(3, 4, 9, 14);
    }
    for (int i = 0; i < 16; i++) {
        put_le32(out + 4 * i, x[i] + start[i]);
    }
}

void chacha_xor(const Key& key, const Nonce& nonce, uint8_t* data, size_t size) {
    uint8_t stream[64];
    uint32_t counter = 1;
    for (size_t at = 0; at < size; at += sizeof stream) {
        chacha_block(key, counter++, nonce, stream);
        size_t count = std::min(size - at, sizeof stream);
        for (size_t i = 0; i < count; i++) {
            data[at + i] ^= stream[i];
        }
    }
}

std::array<uint8_t, 16> poly1305(const uint8_t key[32], const Bytes& blocks) {
    constexpr uint32_t kMask = 0x3ffffff;
    const uint64_t r0 = le32(key) & 0x3ffffff;
    const uint64_t r1 = (le32(key + 3) >> 2) & 0x3ffff03;
    const uint64_t r2 = (le32(key + 6) >> 4) & 0x3ffc0ff;
    const uint64_t r3 = (le32(key + 9) >> 6) & 0x3f03fff;
    const uint64_t r4 = (le32(key + 12) >> 8) & 0x00fffff;
    const uint64_t s1 = r1 * 5;
    const uint64_t s2 = r2 * 5;
    const uint64_t s3 = r3 * 5;
    const uint64_t s4 = r4 * 5;
    uint32_t h0 = 0, h1 = 0, h2 = 0, h3 = 0, h4 = 0;
    for (size_t at = 0; at + 16 <= blocks.size(); at += 16) {
        const uint8_t* m = blocks.data() + at;
        h0 += le32(m) & kMask;
        h1 += (le32(m + 3) >> 2) & kMask;
        h2 += (le32(m + 6) >> 4) & kMask;
        h3 += (le32(m + 9) >> 6) & kMask;
        h4 += (le32(m + 12) >> 8) | (1u << 24);
        uint64_t d0 = h0 * r0 + h1 * s4 + h2 * s3 + h3 * s2 + h4 * s1;
        uint64_t d1 = h0 * r1 + h1 * r0 + h2 * s4 + h3 * s3 + h4 * s2;
        uint64_t d2 = h0 * r2 + h1 * r1 + h2 * r0 + h3 * s4 + h4 * s3;
        uint64_t d3 = h0 * r3 + h1 * r2 + h2 * r1 + h3 * r0 + h4 * s4;
        uint64_t d4 = h0 * r4 + h1 * r3 + h2 * r2 + h3 * r1 + h4 * r0;
        d1 += d0 >> 26;
        h0 = static_cast<uint32_t>(d0) & kMask;
        d2 += d1 >> 26;
        h1 = static_cast<uint32_t>(d1) & kMask;
        d3 += d2 >> 26;
        h2 = static_cast<uint32_t>(d2) & kMask;
        d4 += d3 >> 26;
        h3 = static_cast<uint32_t>(d3) & kMask;
        uint64_t carry = d4 >> 26;
        h4 = static_cast<uint32_t>(d4) & kMask;
        uint64_t low = h0 + carry * 5;
        h0 = static_cast<uint32_t>(low) & kMask;
        h1 += static_cast<uint32_t>(low >> 26);
    }
    uint32_t c = h1 >> 26;
    h1 &= kMask;
    h2 += c;
    c = h2 >> 26;
    h2 &= kMask;
    h3 += c;
    c = h3 >> 26;
    h3 &= kMask;
    h4 += c;
    c = h4 >> 26;
    h4 &= kMask;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= kMask;
    h1 += c;
    uint32_t g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= kMask;
    uint32_t g1 = h1 + c;
    c = g1 >> 26;
    g1 &= kMask;
    uint32_t g2 = h2 + c;
    c = g2 >> 26;
    g2 &= kMask;
    uint32_t g3 = h3 + c;
    c = g3 >> 26;
    g3 &= kMask;
    uint32_t g4 = h4 + c - (1u << 26);
    uint32_t keep = (g4 >> 31) - 1;
    h0 = (h0 & ~keep) | (g0 & keep);
    h1 = (h1 & ~keep) | (g1 & keep);
    h2 = (h2 & ~keep) | (g2 & keep);
    h3 = (h3 & ~keep) | (g3 & keep);
    h4 = (h4 & ~keep) | (g4 & keep);
    uint32_t w0 = h0 | h1 << 26;
    uint32_t w1 = h1 >> 6 | h2 << 20;
    uint32_t w2 = h2 >> 12 | h3 << 14;
    uint32_t w3 = h3 >> 18 | h4 << 8;
    uint64_t f = static_cast<uint64_t>(w0) + le32(key + 16);
    std::array<uint8_t, 16> tag{};
    put_le32(tag.data(), static_cast<uint32_t>(f));
    f = static_cast<uint64_t>(w1) + le32(key + 20) + (f >> 32);
    put_le32(tag.data() + 4, static_cast<uint32_t>(f));
    f = static_cast<uint64_t>(w2) + le32(key + 24) + (f >> 32);
    put_le32(tag.data() + 8, static_cast<uint32_t>(f));
    f = static_cast<uint64_t>(w3) + le32(key + 28) + (f >> 32);
    put_le32(tag.data() + 12, static_cast<uint32_t>(f));
    return tag;
}

std::array<uint8_t, 16> aead_tag(const Key& key, const Nonce& nonce, const Bytes& extra, const uint8_t* cipher, size_t size) {
    uint8_t first[64];
    chacha_block(key, 0, nonce, first);
    Bytes blocks(extra);
    blocks.resize((blocks.size() + 15) / 16 * 16);
    blocks.insert(blocks.end(), cipher, cipher + size);
    blocks.resize((blocks.size() + 15) / 16 * 16);
    uint8_t lengths[16] = {};
    for (int i = 0; i < 8; i++) {
        lengths[i] = static_cast<uint8_t>(static_cast<uint64_t>(extra.size()) >> (8 * i));
        lengths[8 + i] = static_cast<uint8_t>(static_cast<uint64_t>(size) >> (8 * i));
    }
    blocks.insert(blocks.end(), lengths, lengths + sizeof lengths);
    return poly1305(first, blocks);
}

void random_bytes(uint8_t* out, size_t size) {
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw std::runtime_error("no system random source");
    }
#else
    std::ifstream source("/dev/urandom", std::ios::binary);
    if (!source.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("no system random source");
    }
#endif
}

Bytes extra_bytes() {
    return Bytes(kExtra, kExtra + sizeof kExtra - 1);
}

void put_u32(Bytes& out, uint32_t value) {
    size_t at = out.size();
    out.resize(at + 4);
    put_le32(out.data() + at, value);
}

Bytes pack(const Snapshot& snapshot) {
    Bytes out;
    put_u32(out, kFormat);
    put_u32(out, static_cast<uint32_t>(static_cast<uint64_t>(snapshot.saved_at)));
    put_u32(out, static_cast<uint32_t>(static_cast<uint64_t>(snapshot.saved_at) >> 32));
    put_u32(out, snapshot.play_seconds);
    std::string device = snapshot.device.substr(0, kLongestDevice);
    out.push_back(static_cast<uint8_t>(device.size()));
    out.insert(out.end(), device.begin(), device.end());
    out.push_back(static_cast<uint8_t>(snapshot.files.size()));
    for (const SaveFile& file : snapshot.files) {
        out.push_back(static_cast<uint8_t>(file.name.size()));
        out.insert(out.end(), file.name.begin(), file.name.end());
        put_u32(out, static_cast<uint32_t>(file.data.size()));
        out.insert(out.end(), file.data.begin(), file.data.end());
    }
    return out;
}

bool known_file(const std::string& name) {
    return std::any_of(std::begin(kFiles), std::end(kFiles), [&](const char* known) { return name == known; });
}

bool unpack(const Bytes& in, Snapshot& snapshot) {
    size_t at = 0;
    auto u32 = [&](uint32_t& value) {
        if (in.size() - at < 4) {
            return false;
        }
        value = le32(in.data() + at);
        at += 4;
        return true;
    };
    auto text = [&](std::string& value) {
        if (at >= in.size()) {
            return false;
        }
        size_t size = in[at++];
        if (in.size() - at < size) {
            return false;
        }
        value.assign(reinterpret_cast<const char*>(in.data() + at), size);
        at += size;
        return true;
    };
    Snapshot read;
    uint32_t format = 0, low = 0, high = 0;
    if (!u32(format) || format != kFormat || !u32(low) || !u32(high) || !u32(read.play_seconds) || !text(read.device) || read.device.size() > kLongestDevice ||
        at >= in.size()) {
        return false;
    }
    read.saved_at = static_cast<int64_t>(static_cast<uint64_t>(high) << 32 | low);
    size_t count = in[at++];
    if (count > std::size(kFiles)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        SaveFile file;
        uint32_t size = 0;
        if (!text(file.name) || !known_file(file.name) || !u32(size) || size > kLargestFile || in.size() - at < size) {
            return false;
        }
        bool repeated = std::any_of(read.files.begin(), read.files.end(), [&](const SaveFile& other) { return other.name == file.name; });
        if (repeated) {
            return false;
        }
        file.data.assign(in.begin() + static_cast<std::ptrdiff_t>(at), in.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        read.files.push_back(std::move(file));
    }
    if (at != in.size()) {
        return false;
    }
    snapshot = std::move(read);
    return true;
}

}

std::string sha256_hex(const uint8_t* data, size_t size) {
    Sha256 hash;
    hash.add(data, size);
    auto digest = hash.finish();
    std::string out;
    char pair[3];
    for (uint8_t byte : digest) {
        std::snprintf(pair, sizeof pair, "%02x", byte);
        out += pair;
    }
    return out;
}

Bytes pbkdf2_sha256(const std::string& password, const std::string& salt, uint32_t iterations, size_t size) {
    Hmac hmac(password);
    Bytes out;
    for (uint32_t block = 1; out.size() < size; block++) {
        Bytes first(salt.begin(), salt.end());
        put_u32(first, 0);
        for (int i = 0; i < 4; i++) {
            first[first.size() - 4 + static_cast<size_t>(i)] = static_cast<uint8_t>(block >> (24 - 8 * i));
        }
        auto u = hmac.of(first.data(), first.size());
        auto total = u;
        for (uint32_t i = 1; i < iterations; i++) {
            u = hmac.of(u.data(), u.size());
            for (size_t j = 0; j < u.size(); j++) {
                total[j] ^= u[j];
            }
        }
        out.insert(out.end(), total.begin(), total.end());
    }
    out.resize(size);
    return out;
}

Bytes seal_with_nonce(const Key& key, const Nonce& nonce, const Bytes& extra, const Bytes& plain) {
    Bytes out(plain);
    chacha_xor(key, nonce, out.data(), out.size());
    auto tag = aead_tag(key, nonce, extra, out.data(), out.size());
    out.insert(out.end(), tag.begin(), tag.end());
    return out;
}

bool open_sealed(const Key& key, const Nonce& nonce, const Bytes& extra, const uint8_t* sealed, size_t size, Bytes& plain) {
    if (size < kTagBytes) {
        return false;
    }
    size_t body = size - kTagBytes;
    auto tag = aead_tag(key, nonce, extra, sealed, body);
    uint8_t difference = 0;
    for (size_t i = 0; i < kTagBytes; i++) {
        difference |= static_cast<uint8_t>(tag[i] ^ sealed[body + i]);
    }
    if (difference != 0) {
        return false;
    }
    plain.assign(sealed, sealed + body);
    chacha_xor(key, nonce, plain.data(), plain.size());
    return true;
}

std::string new_code() {
    std::string code;
    while (code.size() < kCodeLength) {
        uint8_t value = 0;
        random_bytes(&value, 1);
        if (value < 248) {
            code.push_back(kAlphabet[value % 32]);
        }
    }
    return code;
}

bool clean_code(const std::string& typed, std::string& code) {
    std::string cleaned;
    for (char raw : typed) {
        char character = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (character == '-' || character == ' ') {
            continue;
        }
        if (character == 'O') {
            character = '0';
        } else if (character == 'I' || character == 'L') {
            character = '1';
        }
        if (character == 0 || !std::strchr(kAlphabet, character)) {
            return false;
        }
        cleaned.push_back(character);
    }
    if (cleaned.size() != kCodeLength) {
        return false;
    }
    code = cleaned;
    return true;
}

std::string shown_code(const std::string& code) {
    return code.size() == kCodeLength ? code.substr(0, 5) + "-" + code.substr(5) : code;
}

Keys derive_keys(const std::string& code) {
    Bytes derived = pbkdf2_sha256(code, kSalt, kIterations, 64);
    Keys keys;
    keys.id = sha256_hex(derived.data(), 32);
    std::copy(derived.begin() + 32, derived.end(), keys.key.begin());
    return keys;
}

Bytes seal(const Key& key, const Snapshot& snapshot) {
    Nonce nonce{};
    random_bytes(nonce.data(), nonce.size());
    Bytes sealed = seal_with_nonce(key, nonce, extra_bytes(), pack(snapshot));
    sealed.insert(sealed.begin(), nonce.begin(), nonce.end());
    return sealed;
}

bool open(const Key& key, const Bytes& sealed, Snapshot& snapshot) {
    Nonce nonce{};
    if (sealed.size() < nonce.size() + kTagBytes) {
        return false;
    }
    std::copy_n(sealed.begin(), nonce.size(), nonce.begin());
    Bytes plain;
    return open_sealed(key, nonce, extra_bytes(), sealed.data() + nonce.size(), sealed.size() - nonce.size(), plain) && unpack(plain, snapshot);
}

bool read_save(const fs::path& folder, Snapshot& snapshot) {
    Snapshot read;
    std::error_code error;
    for (const char* name : kFiles) {
        fs::path path = folder / name;
        if (!fs::exists(path, error)) {
            continue;
        }
        uintmax_t size = fs::file_size(path, error);
        if (error || size > kLargestFile) {
            return false;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            return false;
        }
        SaveFile entry;
        entry.name = name;
        entry.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        if (file.bad() || entry.data.size() != size) {
            return false;
        }
        read.files.push_back(std::move(entry));
    }
    snapshot.files = std::move(read.files);
    return true;
}

bool write_save(const fs::path& folder, const Snapshot& snapshot) {
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) {
        return false;
    }
    auto temporary = [&](const std::string& name) { return folder / (name + ".receiving"); };
    bool written = true;
    for (const SaveFile& entry : snapshot.files) {
        std::ofstream file(temporary(entry.name), std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(entry.data.data()), static_cast<std::streamsize>(entry.data.size()));
        written = written && file.flush().good();
    }
    if (!written) {
        for (const SaveFile& entry : snapshot.files) {
            fs::remove(temporary(entry.name), error);
        }
        return false;
    }
    for (const SaveFile& entry : snapshot.files) {
        fs::rename(temporary(entry.name), folder / entry.name, error);
        if (error) {
            return false;
        }
    }
    for (const char* name : kFiles) {
        bool sent = std::any_of(snapshot.files.begin(), snapshot.files.end(), [&](const SaveFile& entry) { return entry.name == name; });
        if (!sent) {
            fs::remove(folder / name, error);
        }
    }
    return true;
}

std::string progress_hash(const Snapshot& snapshot) {
    Bytes joined;
    for (const char* name : kProgressFiles) {
        auto found = std::find_if(snapshot.files.begin(), snapshot.files.end(), [&](const SaveFile& entry) { return entry.name == name; });
        if (found != snapshot.files.end()) {
            joined.insert(joined.end(), name, name + std::strlen(name));
            put_u32(joined, static_cast<uint32_t>(found->data.size()));
            joined.insert(joined.end(), found->data.begin(), found->data.end());
        }
    }
    return joined.empty() ? std::string() : sha256_hex(joined.data(), joined.size());
}

Decision decide(const Facts& facts) {
    if (facts.remote.empty()) {
        return facts.local.empty() ? Decision::Nothing : Decision::Upload;
    }
    if (facts.remote == facts.local) {
        return Decision::Nothing;
    }
    if (facts.local.empty()) {
        return facts.at_start ? Decision::Download : Decision::Nothing;
    }
    if (facts.agreed.empty()) {
        return Decision::Ask;
    }
    if (facts.local == facts.agreed) {
        return facts.at_start ? Decision::Download : Decision::Nothing;
    }
    if (facts.remote == facts.agreed) {
        return Decision::Upload;
    }
    return Decision::Ask;
}

}
