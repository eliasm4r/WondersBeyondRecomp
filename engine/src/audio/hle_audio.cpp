#include "wp/hle_audio.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "wp/audio.h"
#include "wp/cpu.h"
#include "wp/fast_clock.h"
#include "wp/log.h"
#include "wp/memory.h"

namespace wp::hle_audio {

namespace {

using Clock = FastClock;

constexpr uint32_t kRate = 32000;
constexpr int kFrameSamples = 96;
constexpr uint32_t kVoiceSize = 0x140;
constexpr int kAuxBuses = 3;
constexpr uint32_t kAuxChannel = 0x180;

enum : uint32_t {
    kSrcType = 0x08,
    kState = 0x10,
    kIsStream = 0x12,
    kMixLeft = 0x14,
    kMixRight = 0x18,
    kMixAuxLeft = 0x1C,
    kMixSurround = 0x34,
    kMixAuxSurround = 0x38,
    kEnvelope = 0x6A,
    kLoopFlag = 0x6E,
    kFormat = 0x70,
    kLoopAddress = 0x72,
    kEndAddress = 0x76,
    kCurrentAddress = 0x7A,
    kAdpcmCoefficients = 0x7E,
    kAdpcmScale = 0xA0,
    kAdpcmHistory1 = 0xA2,
    kAdpcmHistory2 = 0xA4,
    kSrcRatio = 0xA6,
    kSrcFraction = 0xAA,
    kSrcLast = 0xAC,
    kLoopScale = 0xB4,
    kLoopHistory1 = 0xB6,
    kLoopHistory2 = 0xB8,
    kFilterOn = 0xBA,
    kFilterHistory = 0xBC,
    kFilterA0 = 0xBE,
    kFilterB0 = 0xC0,
};

enum : int { kAdpcm = 0x00, kPcm16 = 0x0A, kPcm8 = 0x19 };

Layout g_layout;
bool g_configured = false;
uint32_t g_dma_callback = 0;
uint32_t g_dma_bytes = 0;
bool g_running = false;
Clock::time_point g_next_frame;
uint32_t g_aux_in[kAuxBuses] = {};
uint32_t g_aux_out[kAuxBuses] = {};
uint32_t g_small_data = 0;

struct AuxMix {
    int32_t left[kFrameSamples];
    int32_t right[kFrameSamples];
    int32_t surround[kFrameSamples];
};

struct Voice {
    uint32_t block = 0;
    uint32_t current = 0;
    uint32_t end = 0;
    uint32_t loop = 0;
    int format = 0;
    bool looping = false;
    bool stream = false;
    bool running = true;
    int32_t scale = 0;
    int32_t history1 = 0;
    int32_t history2 = 0;
};

uint32_t read_pair(uint32_t address) {
    return (static_cast<uint32_t>(rd16(address)) << 16) | rd16(address + 2);
}

void write_pair(uint32_t address, uint32_t value) {
    wr16(address, static_cast<uint16_t>(value >> 16));
    wr16(address + 2, static_cast<uint16_t>(value));
}

int16_t clamp16(int32_t value) {
    return static_cast<int16_t>(std::clamp(value, -32768, 32767));
}

int32_t read_sample(Voice& v) {
    if (!v.running) {
        return 0;
    }
    int32_t sample = 0;
    switch (v.format) {
    case kAdpcm: {
        if ((v.current & 15) < 2) {
            v.current = (v.current & ~15u) + 2;
        }
        if ((v.current & 15) == 2) {
            v.scale = rd8((v.current & ~15u) >> 1);
        }
        uint8_t byte = rd8(v.current >> 1);
        int32_t nibble = (v.current & 1) ? (byte & 15) : (byte >> 4);
        if (nibble >= 8) {
            nibble -= 16;
        }
        uint32_t pair = v.block + kAdpcmCoefficients + ((v.scale >> 4) & 7) * 4;
        int32_t c1 = static_cast<int16_t>(rd16(pair));
        int32_t c2 = static_cast<int16_t>(rd16(pair + 2));
        sample = clamp16((c1 * v.history1 + c2 * v.history2 + ((nibble << (v.scale & 15)) << 11) + 1024) >> 11);
        v.history2 = v.history1;
        v.history1 = sample;
        break;
    }
    case kPcm16:
        sample = static_cast<int16_t>(rd16(v.current * 2));
        break;
    case kPcm8:
        sample = static_cast<int8_t>(rd8(v.current)) * 256;
        break;
    default:
        break;
    }
    if (v.current == v.end) {
        v.current = v.loop;
        if (!v.looping) {
            v.running = false;
        } else if (v.format == kAdpcm && !v.stream) {
            v.scale = rd16(v.block + kLoopScale);
            v.history1 = static_cast<int16_t>(rd16(v.block + kLoopHistory1));
            v.history2 = static_cast<int16_t>(rd16(v.block + kLoopHistory2));
        }
    } else {
        v.current++;
    }
    return sample;
}

void mix_voice(uint32_t block, int32_t* left, int32_t* right, AuxMix* aux) {
    Voice v;
    v.block = block;
    v.current = read_pair(block + kCurrentAddress);
    v.end = read_pair(block + kEndAddress);
    v.loop = read_pair(block + kLoopAddress);
    v.format = rd16(block + kFormat);
    v.looping = rd16(block + kLoopFlag) != 0;
    v.stream = rd16(block + kIsStream) != 0;
    v.scale = rd16(block + kAdpcmScale);
    v.history1 = static_cast<int16_t>(rd16(block + kAdpcmHistory1));
    v.history2 = static_cast<int16_t>(rd16(block + kAdpcmHistory2));
    uint32_t ratio = rd16(block + kSrcType) == 2 ? 0x10000 : read_pair(block + kSrcRatio);
    uint32_t fraction = rd16(block + kSrcFraction);
    int32_t last0 = static_cast<int16_t>(rd16(block + kSrcLast));
    int32_t last1 = static_cast<int16_t>(rd16(block + kSrcLast + 2));
    int32_t envelope = rd16(block + kEnvelope);
    int32_t surround = rd16(block + kMixSurround);
    int32_t volume_left = rd16(block + kMixLeft) + surround / 2;
    int32_t volume_right = rd16(block + kMixRight) + surround / 2;
    int32_t aux_left[kAuxBuses] = {};
    int32_t aux_right[kAuxBuses] = {};
    int32_t aux_surround[kAuxBuses] = {};
    int32_t sends = 0;
    for (int b = 0; b < kAuxBuses; b++) {
        if (g_aux_in[b]) {
            aux_left[b] = rd16(block + kMixAuxLeft + 8 * b);
            aux_right[b] = rd16(block + kMixAuxLeft + 8 * b + 4);
            aux_surround[b] = rd16(block + kMixAuxSurround + 4 * b);
        }
        sends |= aux_left[b] | aux_right[b] | aux_surround[b];
    }
    bool filter = rd16(block + kFilterOn) != 0;
    int32_t filter_history = static_cast<int16_t>(rd16(block + kFilterHistory));
    int32_t filter_a = rd16(block + kFilterA0);
    int32_t filter_b = rd16(block + kFilterB0);
    for (int i = 0; i < kFrameSamples; i++) {
        int32_t s = last0 + static_cast<int32_t>((static_cast<int64_t>(last1 - last0) * static_cast<int32_t>(fraction)) >> 16);
        s = (s * envelope) >> 15;
        if (filter) {
            s = filter_history = clamp16((filter_a * s + filter_b * filter_history) >> 15);
        }
        left[i] += (s * volume_left) >> 15;
        right[i] += (s * volume_right) >> 15;
        for (int b = 0; sends && b < kAuxBuses; b++) {
            aux[b].left[i] += (s * aux_left[b]) >> 15;
            aux[b].right[i] += (s * aux_right[b]) >> 15;
            aux[b].surround[i] += (s * aux_surround[b]) >> 15;
        }
        for (fraction += ratio; fraction >= 0x10000; fraction -= 0x10000) {
            last0 = last1;
            last1 = read_sample(v);
        }
    }
    write_pair(block + kCurrentAddress, v.current);
    wr16(block + kSrcFraction, static_cast<uint16_t>(fraction));
    wr16(block + kSrcLast, static_cast<uint16_t>(last0));
    wr16(block + kSrcLast + 2, static_cast<uint16_t>(last1));
    wr16(block + kAdpcmScale, static_cast<uint16_t>(v.scale));
    wr16(block + kAdpcmHistory1, static_cast<uint16_t>(v.history1));
    wr16(block + kAdpcmHistory2, static_cast<uint16_t>(v.history2));
    wr16(block + kFilterHistory, static_cast<uint16_t>(filter_history));
    if (!v.running) {
        wr16(block + kState, 0);
    }
}

void mix_frame() {
    uint32_t blocks = rd32(g_layout.voices);
    uint32_t count = rd32(g_layout.voice_count);
    int32_t left[kFrameSamples] = {};
    int32_t right[kFrameSamples] = {};
    AuxMix aux[kAuxBuses];
    std::memset(aux, 0, sizeof aux);
    for (uint32_t i = 0; blocks && i < count; i++) {
        uint32_t block = blocks + i * kVoiceSize;
        if (rd16(block + kState) == 1) {
            mix_voice(block, left, right, aux);
        }
    }
    for (int b = 0; b < kAuxBuses; b++) {
        uint32_t in = g_aux_in[b];
        uint32_t from = g_aux_out[b];
        int32_t back = g_small_data ? rd16(g_small_data + static_cast<uint32_t>(g_layout.aux_return[b])) : 0;
        if (in) {
            for (int i = 0; i < kFrameSamples; i++) {
                wr32(in + i * 4, static_cast<uint32_t>(aux[b].left[i]));
                wr32(in + kAuxChannel + i * 4, static_cast<uint32_t>(aux[b].right[i]));
                wr32(in + 2 * kAuxChannel + i * 4, static_cast<uint32_t>(aux[b].surround[i]));
            }
        }
        if (from && back) {
            for (int i = 0; i < kFrameSamples; i++) {
                int64_t l = static_cast<int32_t>(rd32(from + i * 4));
                int64_t r = static_cast<int32_t>(rd32(from + kAuxChannel + i * 4));
                int64_t s = static_cast<int32_t>(rd32(from + 2 * kAuxChannel + i * 4));
                left[i] += static_cast<int32_t>(((l + s / 2) * back) >> 15);
                right[i] += static_cast<int32_t>(((r + s / 2) * back) >> 15);
            }
        }
    }
    int16_t out[kFrameSamples * 2];
    for (int i = 0; i < kFrameSamples; i++) {
        out[i * 2] = clamp16(left[i]);
        out[i * 2 + 1] = clamp16(right[i]);
    }
    audio::push(out, kFrameSamples, kRate);
}

std::chrono::nanoseconds frame_period() {
    uint32_t samples = g_dma_bytes / 4;
    return std::chrono::nanoseconds(static_cast<int64_t>(samples) * 1000000000 / kRate);
}

}

bool wanted() {
    static const bool on = [] {
        if (std::getenv("WP_DSP_EMULATED")) {
            return false;
        }
#ifdef __ANDROID__
        return true;
#else
        return std::getenv("WP_HLE_AUDIO") != nullptr;
#endif
    }();
    return on;
}

bool active() {
    return g_configured && wanted();
}

void configure(const Layout& layout) {
    g_layout = layout;
    g_configured = true;
    if (active()) {
        log::write("audio", "the game's mixer runs natively instead of on the emulated DSP");
    }
}

void set_dma_callback(uint32_t callback) {
    g_dma_callback = callback;
}

void set_dma_bytes(uint32_t bytes) {
    g_dma_bytes = bytes;
}

void start_dma() {
    if (!g_running && g_dma_bytes >= 4) {
        g_running = true;
        g_next_frame = Clock::now() + frame_period();
        audio::start_output();
    }
}

void stop_dma() {
    g_running = false;
}

void set_aux(int bus, bool output, uint32_t address, uint32_t small_data) {
    if (bus < 0 || bus >= kAuxBuses) {
        return;
    }
    (output ? g_aux_out : g_aux_in)[bus] = address;
    g_small_data = small_data;
}

void run_due_frames(Cpu& c) {
    if (!g_running || !active()) {
        return;
    }
    Clock::time_point now = Clock::now();
    if (now < g_next_frame) {
        return;
    }
    std::chrono::nanoseconds period = frame_period();
    if (period.count() <= 0) {
        return;
    }
    if (now - g_next_frame > period * 20) {
        g_next_frame = now;
    }
    for (int frames = 0; frames < 4 && now >= g_next_frame && g_running; frames++) {
        g_next_frame += period;
        mix_frame();
        run_interrupt_callback(c, g_layout.resume_callback);
        if (g_dma_callback) {
            run_interrupt_callback(c, g_dma_callback);
        }
    }
}

}
