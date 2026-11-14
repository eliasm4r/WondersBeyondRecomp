#include "lookup.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "wp/log.h"
#include "wp/memory.h"

namespace wp::game {

namespace {

constexpr uint32_t kSmallTable = 16;
constexpr size_t kMaxTables = 256;
constexpr uint32_t kEnds = 64;

struct Index {
    uint32_t count = 0;
    uint32_t size = 0;
    uint32_t slots = 0;
    uint32_t checked = 0;
    std::vector<uint8_t> copy;
    std::vector<uint32_t> entry;
    std::vector<uint32_t> position;
};

std::unordered_map<uint32_t, Index> g_indexes;
uint32_t g_retrace = 1;

const int g_mode = [] {
    const char* value = std::getenv("WP_NATIVE_LOOKUP");
    return value ? std::atoi(value) : 1;
}();

uint32_t be16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 8 | p[1];
}

uint32_t be32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 | static_cast<uint32_t>(p[2]) << 8 | p[3];
}

uint32_t hash_name(const char* name) {
    uint32_t hash = 2166136261u;
    while (*name) {
        hash = (hash ^ static_cast<uint8_t>(*name++)) * 16777619u;
    }
    return hash;
}

uint32_t walk(uint32_t table, const char* wanted) {
    uint32_t count = rd16(table + 4);
    const uint8_t* p = host(table + 8);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t length = be16(p);
        const char* name = reinterpret_cast<const char*>(p) + 2;
        p += 2 + length;
        if (name[0] == wanted[0] && std::strcmp(name, wanted) == 0) {
            return table + be32(p);
        }
        p += 8;
    }
    return 0;
}

Index* index_table(uint32_t table, bool again) {
    uint32_t count = rd16(table + 4);
    auto found = g_indexes.find(table);
    if (!again && found != g_indexes.end() && found->second.count == count) {
        Index& index = found->second;
        if (index.checked == g_retrace) {
            return &index;
        }
        if (std::memcmp(index.copy.data(), host(table), index.size) == 0) {
            index.checked = g_retrace;
            return &index;
        }
    }
    if (found == g_indexes.end() && g_indexes.size() >= kMaxTables) {
        g_indexes.clear();
    }
    uint32_t end = table + 8;
    for (uint32_t i = 0; i < count; i++) {
        end += 2 + rd16(end) + 8;
    }
    Index& index = g_indexes[table];
    index.count = count;
    index.checked = g_retrace;
    index.size = end - table;
    index.slots = 16;
    while (index.slots < count * 2) {
        index.slots *= 2;
    }
    index.copy.assign(host(table), host(table) + index.size);
    index.entry.assign(index.slots, 0);
    index.position.resize(count);
    uint32_t offset = 8;
    for (uint32_t i = 0; i < count; i++) {
        const char* name = reinterpret_cast<const char*>(index.copy.data()) + offset + 2;
        uint32_t slot = hash_name(name) & (index.slots - 1);
        uint32_t entry = 0;
        while ((entry = index.entry[slot]) != 0 && std::strcmp(reinterpret_cast<const char*>(index.copy.data()) + entry - 1 + 2, name) != 0) {
            slot = (slot + 1) & (index.slots - 1);
        }
        if (entry == 0) {
            index.entry[slot] = offset + 1;
        }
        index.position[i] = offset;
        offset += 2 + be16(index.copy.data() + offset) + 8;
    }
    return &index;
}

bool same_ends(const Index& index, uint32_t table) {
    uint32_t n = index.size < kEnds ? index.size : kEnds;
    return std::memcmp(index.copy.data(), host(table), n) == 0 && std::memcmp(index.copy.data() + index.size - n, host(table + index.size - n), n) == 0;
}

uint32_t find_by_name(uint32_t header, uint32_t name) {
    uint32_t table = header + rd32(header);
    const char* wanted = reinterpret_cast<const char*>(host(name));
    if (rd16(table + 4) <= kSmallTable) {
        return walk(table, wanted);
    }
    Index* index = index_table(table, false);
    uint32_t slot = hash_name(wanted) & (index->slots - 1);
    uint32_t entry = 0;
    for (; (entry = index->entry[slot]) != 0; slot = (slot + 1) & (index->slots - 1)) {
        const uint8_t* kept = index->copy.data() + entry - 1;
        if (std::strcmp(reinterpret_cast<const char*>(kept) + 2, wanted) == 0) {
            const uint8_t* live = host(table + entry - 1);
            uint32_t length = be16(live);
            if (length == be16(kept) && std::strcmp(reinterpret_cast<const char*>(live) + 2, wanted) == 0) {
                return table + be32(live + 2 + length);
            }
            break;
        }
    }
    if (entry == 0 && same_ends(*index, table)) {
        return 0;
    }
    uint32_t result = walk(table, wanted);
    if (result != 0 || entry != 0) {
        index_table(table, true);
    }
    return result;
}

uint32_t find_by_number(uint32_t header, uint32_t number) {
    uint32_t table = header + rd32(header);
    uint32_t count = rd16(table + 4);
    if (number >= count) {
        return 0;
    }
    uint32_t at = 0;
    if (count > kSmallTable) {
        Index* index = index_table(table, false);
        if (!same_ends(*index, table)) {
            index = index_table(table, true);
        }
        at = table + index->position[number];
    } else {
        at = table + 8;
        for (uint32_t i = 0; i < number; i++) {
            at += 2 + rd16(at) + 8;
        }
    }
    return table + rd32(at + 2 + rd16(at));
}

void check(Cpu& c, HleFunction original, uint32_t native, const char* kind) {
    static uint64_t calls = 0;
    static uint64_t differ = 0;
    uint32_t header = c.r[3];
    uint32_t argument = c.r[4];
    original(c);
    calls++;
    if (c.r[3] != native) {
        differ++;
        log::write("lookup", "%s lookup in %08x for %08x: game %08x, native %08x", kind, header, argument, c.r[3], native);
    }
    if ((calls & 0xFFFF) == 0) {
        log::write("lookup", "%llu lookups checked, %llu differ", static_cast<unsigned long long>(calls), static_cast<unsigned long long>(differ));
    }
}

}

void find_resource_by_name(Cpu& c, HleFunction original) {
    if (g_mode == 0) {
        original(c);
        return;
    }
    uint32_t native = find_by_name(c.r[3], c.r[4]);
    if (g_mode == 2) {
        check(c, original, native, "name");
        return;
    }
    c.r[3] = native;
}

void find_resource_by_number(Cpu& c, HleFunction original) {
    if (g_mode == 0) {
        original(c);
        return;
    }
    uint32_t native = find_by_number(c.r[3], c.r[4]);
    if (g_mode == 2) {
        check(c, original, native, "number");
        return;
    }
    c.r[3] = native;
}

void next_lookup_retrace() {
    g_retrace++;
}

}
