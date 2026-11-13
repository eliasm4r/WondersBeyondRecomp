#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "wp/canvas.h"
#include "wp/color.h"
#include "wp/control_names.h"
#include "wp/cpu.h"
#include "wp/custom_textures.h"
#include "wp/function_table.h"
#include "wp/hollywood.h"
#include "wp/gx.h"
#include "wp/gx_state.h"
#include "wp/input.h"
#include "wp/ios.h"
#include "wp/keymap.h"
#include "wp/modules.h"
#include "wp/nand.h"
#include "wp/options.h"
#include "wp/options_window.h"
#include "wp/save_backup.h"
#include "wp/padmap.h"
#include "wp/frame_pacing.h"
#include "wp/free_camera.h"
#include "wp/perf.h"
#include "wp/perf_overlay.h"
#include "wp/play_time.h"
#include "wp/input.h"
#include "wp/runtime.h"
#include "wp/screenshot.h"
#include "wp/stall_watch.h"
#include "wp/settings.h"
#include "wp/ui_text.h"
#include "wp/video.h"
#include "wp/wiimote.h"

namespace wp::gx::render {
uint64_t hash_bytes(const uint8_t* data, size_t size);
}

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "line %d: %s\n", line, expression);
        failures++;
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_software_fma() {
    std::mt19937_64 random(2026);
    std::uniform_real_distribution<double> mantissa(-2.0, 2.0);
    std::uniform_int_distribution<int> exponent(-60, 60);
    int mismatches = 0;
    for (int i = 0; i < 200000; i++) {
        double a = std::ldexp(mantissa(random), exponent(random));
        double b = std::ldexp(mantissa(random), exponent(random));
        double c = std::ldexp(mantissa(random), exponent(random) * 2);
        double expected = wp::g_hardware_fma ? wp::hardware_fma(a, b, c) : std::fma(a, b, c);
        if (wp::fpr_bits(wp::software_fma(a, b, c)) != wp::fpr_bits(expected)) {
            mismatches++;
        }
    }
    CHECK(!wp::g_hardware_fma || mismatches == 0);
    CHECK(wp::fpr_bits(wp::software_fma(339099.27191141015, 292340.0665708075, -16.0)) == 0x423714bee95cb1a9ull);
}

void test_memory() {
    wp::wr32(0x80001000, 0x11223344);
    CHECK(wp::rd8(0x80001000) == 0x11);
    CHECK(wp::rd16(0x80001002) == 0x3344);
    CHECK(wp::rd32(0x80001000) == 0x11223344);
    CHECK(wp::rd32_reversed(0x80001000) == 0x44332211);
    wp::wr64(0x80001008, 0x0102030405060708ull);
    CHECK(wp::rd32(0x80001008) == 0x01020304);
    CHECK(wp::rd64(0x80001008) == 0x0102030405060708ull);
    CHECK(wp::host(0x80001000) == wp::host(0xC0001000));
    wp::wrf32(0x80001010, 1.5f);
    CHECK(wp::rd32(0x80001010) == 0x3FC00000);
    CHECK(wp::rdf32(0x80001010) == 1.5f);
    wp::wrf64(0x80001018, -2.25);
    CHECK(wp::rdf64(0x80001018) == -2.25);
    wp::wr32(0x80002000, 0xFFFFFFFF);
    wp::wr32(0x80002020, 0xFFFFFFFF);
    wp::dcbz(0x80002010);
    CHECK(wp::rd32(0x80002000) == 0);
    CHECK(wp::rd32(0x8000201C) == 0);
    CHECK(wp::rd32(0x80002020) == 0xFFFFFFFF);
}

void test_rotate_and_shift() {
    wp::Cpu c{};
    CHECK(wp::rotl(0x80000001u, 1) == 3);
    CHECK(wp::rotl(0x12345678u, 0) == 0x12345678);
    CHECK(wp::rotl(0x12345678u, 32) == 0x12345678);
    CHECK(wp::slw(1, 31) == 0x80000000);
    CHECK(wp::slw(1, 32) == 0);
    CHECK(wp::srw(0x80000000u, 31) == 1);
    CHECK(wp::srw(0x80000000u, 32) == 0);
    CHECK(wp::sraw(c, 0xFFFFFFF0u, 4) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 0);
    CHECK(wp::sraw(c, 0xFFFFFFF1u, 4) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sraw(c, 0x80000000u, 40) == 0xFFFFFFFF);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sraw(c, 0x40000000u, 40) == 0);
    CHECK(c.xer_ca == 0);
}

void test_arithmetic() {
    wp::Cpu c{};
    CHECK(wp::add_carry(c, 0xFFFFFFFFu, 1, 0) == 0);
    CHECK(c.xer_ca == 1);
    CHECK(wp::add_carry(c, 1, 2, 0) == 3);
    CHECK(c.xer_ca == 0);
    c.xer_ca = 1;
    CHECK(wp::add_extended(c, 1, 2) == 4);
    c.xer_ca = 0;
    CHECK(wp::sub_carry(c, 3, 5) == 2);
    CHECK(c.xer_ca == 1);
    CHECK(wp::sub_carry(c, 5, 3) == 0xFFFFFFFE);
    CHECK(c.xer_ca == 0);
    CHECK(wp::sub_carry(c, 5, 5) == 0);
    CHECK(c.xer_ca == 1);
    CHECK(wp::mulhw(0xFFFFFFFFu, 2) == 0xFFFFFFFF);
    CHECK(wp::mulhwu(0xFFFFFFFFu, 2) == 1);
    CHECK(wp::divw(static_cast<uint32_t>(-7), 2) == static_cast<uint32_t>(-3));
    CHECK(wp::divw(5, 0) == 0);
    CHECK(wp::divwu(7, 2) == 3);
    CHECK(wp::cntlzw(0) == 32);
    CHECK(wp::cntlzw(1) == 31);
    CHECK(wp::cntlzw(0x80000000u) == 0);
    CHECK(wp::sign_extend8(0x80) == 0xFFFFFF80);
    CHECK(wp::sign_extend16(0x7FFF) == 0x7FFF);
}

void test_condition_register() {
    wp::Cpu c{};
    wp::compare_signed(c, 3, 0xFFFFFFFFu, 1);
    CHECK(c.cr[3] == 8);
    wp::compare_unsigned(c, 3, 0xFFFFFFFFu, 1);
    CHECK(c.cr[3] == 4);
    wp::compare_signed(c, 3, 5, 5);
    CHECK(c.cr[3] == 2);
    CHECK(wp::cr_get(c, 14));
    CHECK(!wp::cr_get(c, 13));
    CHECK(!wp::cr_get(c, 12));
    wp::cr_set(c, 12, true);
    CHECK(c.cr[3] == 10);
    wp::mtcrf(c, 0xFF, 0x12345678);
    CHECK(wp::mfcr(c) == 0x12345678);
    wp::mtcrf(c, 0x80, 0xF0000000);
    CHECK(wp::mfcr(c) == 0xF2345678);
    c.xer_so = 1;
    wp::record(c, 0);
    CHECK(c.cr[0] == 3);
    wp::mtxer(c, 0xE0000005);
    CHECK(c.xer_so == 1 && c.xer_ov == 1 && c.xer_ca == 1 && c.xer_byte_count == 5);
    CHECK(wp::mfxer(c) == 0xE0000005);
}

void test_floating_point() {
    wp::Cpu c{};
    wp::compare_float(c, 1, 1.0, 2.0);
    CHECK(c.cr[1] == 8);
    wp::compare_float(c, 1, 2.0, 1.0);
    CHECK(c.cr[1] == 4);
    wp::compare_float(c, 1, 2.0, 2.0);
    CHECK(c.cr[1] == 2);
    wp::compare_float(c, 1, std::nan(""), 2.0);
    CHECK(c.cr[1] == 1);
    CHECK(wp::fpr_bits(wp::convert_to_int(-1.9, true)) == 0xFFF80000FFFFFFFFull);
    CHECK(wp::fpr_bits(wp::convert_to_int(2.5, false)) == 0xFFF8000000000002ull);
    CHECK(wp::fpr_bits(wp::convert_to_int(3e10, true)) == 0xFFF800007FFFFFFFull);
    CHECK(wp::fsel(-1.0, 10.0, 20.0) == 10.0);
    CHECK(wp::fsel(0.0, 10.0, 20.0) == 20.0);
    CHECK(wp::round_single(0.1) == static_cast<double>(0.1f));
}

void test_paired_singles() {
    wp::Cpu c{};
    c.f[1] = 1.0;
    c.ps1[1] = 2.0;
    c.f[2] = 10.0;
    c.ps1[2] = 20.0;
    wp::ps_add(c, 3, 1, 2);
    CHECK(c.f[3] == 11.0 && c.ps1[3] == 22.0);
    wp::ps_merge(c, 4, 1, 2, true, false);
    CHECK(c.f[4] == 2.0 && c.ps1[4] == 10.0);
    wp::ps_madd(c, 5, 1, 2, 1, 1.0, 1.0);
    CHECK(c.f[5] == 11.0 && c.ps1[5] == 42.0);
    wp::ps_madd(c, 5, 1, 2, 1, -1.0, 1.0);
    CHECK(c.f[5] == -9.0 && c.ps1[5] == -38.0);
    wp::ps_sum0(c, 6, 1, 2, 1);
    CHECK(c.f[6] == 3.0 && c.ps1[6] == 20.0);
    wp::ps_muls1(c, 7, 1, 2);
    CHECK(c.f[7] == 20.0 && c.ps1[7] == 40.0);
}

void test_quantization() {
    wp::Cpu c{};
    uint32_t size = 0;
    c.spr[wp::kSprGqr0] = 0;
    wp::wrf32(0x80003000, 3.5f);
    wp::wrf32(0x80003004, -1.25f);
    wp::load_quantized(c, 1, 0x80003000, 0, 0, size);
    CHECK(c.f[1] == 3.5 && c.ps1[1] == -1.25 && size == 8);
    wp::load_quantized(c, 2, 0x80003000, 1, 0, size);
    CHECK(c.f[2] == 3.5 && c.ps1[2] == 1.0 && size == 4);

    c.spr[wp::kSprGqr0 + 1] = (7u << 16) | (4u << 24);
    wp::wr16(0x80003010, 0xFFF0);
    wp::wr16(0x80003012, 32);
    wp::load_quantized(c, 3, 0x80003010, 0, 1, size);
    CHECK(c.f[3] == -1.0 && c.ps1[3] == 2.0 && size == 4);

    c.spr[wp::kSprGqr0 + 2] = 7u | (4u << 8);
    c.f[4] = -1.0;
    c.ps1[4] = 2.0;
    wp::store_quantized(c, 4, 0x80003020, 0, 2, size);
    CHECK(wp::rd16(0x80003020) == 0xFFF0 && wp::rd16(0x80003022) == 32 && size == 4);

    c.spr[wp::kSprGqr0 + 3] = 4u | (0u << 8);
    c.f[5] = 300.0;
    wp::store_quantized(c, 5, 0x80003030, 1, 3, size);
    CHECK(wp::rd8(0x80003030) == 255 && size == 1);
}

void test_disc_drive() {
    int32_t drive = wp::ios::open("/dev/di");
    const uint32_t command = 0x80200000;
    const uint32_t output = 0x80201000;

    wp::wr32(command + 4, 0x20);
    wp::wr32(command + 8, 0x460A0000);
    CHECK(wp::ios::ioctl(drive, 0x8D, command, 0x20, output, 0x20) == 2);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0x52100);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0);

    wp::wr32(command + 8, 0);
    CHECK(wp::ios::ioctl(drive, 0x8D, command, 0x20, output, 0x20) == 1);

    CHECK(wp::ios::ioctl(drive, 0xA4, command, 0x20, output, 0x20) == 2);
    CHECK(wp::ios::ioctl(drive, 0xE0, command, 0x20, output, 0x20) == 1);
    CHECK(wp::rd32(output) == 0x53100);
}

void write_file(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void test_save_backups() {
    namespace fs = std::filesystem;
    using wp::saves::Result;
    std::error_code error;
    fs::path root = fs::temp_directory_path(error) / "wp_save_backup_tests";
    fs::remove_all(root, error);
    fs::path nand = root / "nand";
    fs::path backups = wp::saves::backups_beside(nand);
    fs::path relative = wp::saves::save_relative(0x53555045);
    fs::path save = nand / relative;
    const std::time_t start = 1790000000;
    CHECK(relative == fs::path("title/00010000/53555045/data"));
    CHECK(backups == root / "backups");
    CHECK(wp::saves::backups_beside(fs::path(nand) += "/") == backups);

    CHECK(wp::saves::back_up(nand, relative, backups, 5, start).result == Result::NoSave);
    CHECK(wp::saves::list(backups).empty());

    write_file(save / "GD0", "first");
    write_file(save / "banner.bin", "banner");
    std::string problem;
    CHECK(wp::saves::validate(save, problem));
    wp::saves::Outcome first = wp::saves::back_up(nand, relative, backups, 5, start);
    CHECK(first.result == Result::Created);
    CHECK(wp::saves::list(backups).size() == 1);
    CHECK(wp::saves::same_contents(save, first.backup / relative));
    CHECK(read_file(first.backup / relative / "GD0") == "first");

    CHECK(wp::saves::back_up(nand, relative, backups, 5, start + 10).result == Result::Unchanged);
    CHECK(wp::saves::list(backups).size() == 1);

    write_file(save / "GD0", "second");
    wp::saves::Outcome same_second = wp::saves::back_up(nand, relative, backups, 5, start);
    CHECK(same_second.result == Result::Created);
    std::vector<wp::saves::Backup> listed = wp::saves::list(backups);
    CHECK(listed.size() == 2 && listed.front().path == same_second.backup && listed.back().path == first.backup);

    for (int i = 0; i < 4; i++) {
        write_file(save / "GD0", "round " + std::to_string(i));
        CHECK(wp::saves::back_up(nand, relative, backups, 3, start + 100 + i).result == Result::Created);
    }
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "GD0") == "round 3");
    CHECK(listed.size() == 3 && read_file(listed[2].path / relative / "GD0") == "round 1");
    CHECK(!fs::exists(first.backup) && !fs::exists(same_second.backup));
    CHECK(read_file(save / "GD0") == "round 3");

    write_file(save / "GD0", "current");
    wp::saves::Backup oldest = listed.back();
    CHECK(wp::saves::saved_relative(oldest) == relative);
    wp::saves::Outcome restored = wp::saves::restore(nand, backups, oldest, 3, start + 200);
    CHECK(restored.result == Result::Created);
    CHECK(read_file(save / "GD0") == "round 1");
    CHECK(read_file(save / "banner.bin") == "banner");
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "GD0") == "current");
    CHECK(!fs::exists(fs::path(save) += ".replaced") && !fs::exists(fs::path(save) += ".restoring"));
    CHECK(wp::saves::restore(nand, backups, listed[0], 3, start + 300).result == Result::Created);
    CHECK(read_file(save / "GD0") == "current");
    CHECK(wp::saves::restore(nand, backups, listed[0], 3, start + 400).result == Result::Unchanged);
    listed = wp::saves::list(backups);
    CHECK(listed.size() == 3 && read_file(listed[0].path / relative / "GD0") == "round 1");

    wp::saves::Backup empty{"2026-01-01_00-00-00", backups / "2026-01-01_00-00-00"};
    fs::create_directories(empty.path);
    CHECK(wp::saves::restore(nand, backups, empty, 3, start + 500).result == Result::Failed);
    CHECK(read_file(save / "GD0") == "current");
    fs::create_directories(backups / "notes");
    fs::create_directories(backups / "2026-01-01_00-00-00.partial");
    CHECK(wp::saves::list(backups).size() == 4);
    fs::remove_all(root, error);
}

void test_adopt_root_files() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path nand = fs::temp_directory_path(error) / "wp_adopt_tests";
    fs::remove_all(nand, error);
    fs::path relative = wp::saves::save_relative(0x53555050);
    write_file(nand / "GD0", "misplaced");
    write_file(nand / "banner.bin", "banner");
    write_file(nand / "shared2" / "sys" / "SYSCONF", "config");
    std::vector<std::string> moved = wp::saves::adopt_root_files(nand, relative);
    CHECK(moved.size() == 2);
    CHECK(read_file(nand / relative / "GD0") == "misplaced");
    CHECK(read_file(nand / relative / "banner.bin") == "banner");
    CHECK(!fs::exists(nand / "GD0") && !fs::exists(nand / "banner.bin"));
    CHECK(read_file(nand / "shared2" / "sys" / "SYSCONF") == "config");
    write_file(nand / "GD0", "stale");
    CHECK(wp::saves::adopt_root_files(nand, relative).empty());
    CHECK(read_file(nand / relative / "GD0") == "misplaced");
    CHECK(read_file(nand / "GD0") == "stale");
    fs::remove_all(nand, error);
}

void test_title_data_directory() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path parent = fs::temp_directory_path(error) / "wp_title_tests";
    fs::remove_all(parent, error);
    fs::path nand = parent / "nand";
    wp::wr32(0x80000000, 0x53555050);
    CHECK(wp::nand::mount(nand.string()));
    CHECK(fs::is_directory(nand / "title" / "00010000" / "53555050" / "data"));
    fs::remove_all(parent, error);
}

void test_read_directory_forms() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path parent = fs::temp_directory_path(error) / "wp_read_directory_tests";
    fs::remove_all(parent, error);
    fs::path nand = parent / "nand";
    CHECK(wp::nand::mount(nand.string()));
    fs::create_directories(nand / "tmp" / "list", error);
    for (const char* name : {"a", "b", "c"}) {
        write_file(nand / "tmp" / "list" / name, "x");
    }
    int32_t file_system = wp::ios::open("/dev/fs", 0);
    const uint32_t path = 0x80202000;
    const uint32_t vectors = 0x80203000;
    const uint32_t count = 0x80204000;
    const uint32_t sentinel = 0x80204100;
    const uint32_t maximum = 0x80204200;
    const uint32_t names = 0x80205000;
    std::memcpy(wp::host(path), "/tmp/list", 10);

    wp::wr32(vectors, path);
    wp::wr32(vectors + 4, 64);
    wp::wr32(vectors + 8, count);
    wp::wr32(vectors + 12, 4);
    wp::wr32(vectors + 16, sentinel);
    wp::wr32(sentinel, 0xDEADBEEF);
    CHECK(wp::ios::ioctlv(file_system, 4, 1, 1, vectors) == 0);
    CHECK(wp::rd32(count) == 3);
    CHECK(wp::rd32(sentinel) == 0xDEADBEEF);

    wp::wr32(maximum, 2);
    wp::wr32(vectors + 8, maximum);
    wp::wr32(vectors + 12, 4);
    wp::wr32(vectors + 16, names);
    wp::wr32(vectors + 20, 2 * 13);
    wp::wr32(vectors + 24, count);
    wp::wr32(vectors + 28, 4);
    CHECK(wp::ios::ioctlv(file_system, 4, 2, 2, vectors) == 0);
    CHECK(wp::rd32(count) == 2);
    CHECK(wp::rd8(names) != 0 && wp::rd8(names + 13) != 0);
    fs::remove_all(parent, error);
}

void test_file_attributes() {
    namespace fs = std::filesystem;
    std::error_code error;
    fs::path parent = fs::temp_directory_path(error) / "wp_file_attribute_tests";
    fs::remove_all(parent, error);
    fs::path nand = parent / "nand";
    CHECK(wp::nand::mount(nand.string()));
    fs::create_directories(nand / "tmp", error);
    write_file(nand / "tmp" / "save", "x");
    int32_t file_system = wp::ios::open("/dev/fs", 0);
    const uint32_t request = 0x80206000;
    const uint32_t input = 0x80206100;
    const uint32_t output = 0x80206200;
    auto get_attributes = [&](const char* path) {
        std::memset(wp::host(input), 0, 64);
        std::memcpy(wp::host(input), path, std::strlen(path) + 1);
        std::memset(wp::host(output), 0xCC, 80);
        wp::wr32(request, 6);
        wp::wr32(request + 8, static_cast<uint32_t>(file_system));
        wp::wr32(request + 12, 6);
        wp::wr32(request + 16, input);
        wp::wr32(request + 20, 64);
        wp::wr32(request + 24, output);
        wp::wr32(request + 28, 74);
        uint64_t ticks = 0;
        return wp::ios::send(request, ticks);
    };
    CHECK(get_attributes("/tmp/save") == 0);
    CHECK(wp::rd32(output) == 0x1000 && wp::rd16(output + 4) == 0x3031);
    CHECK(std::strcmp(reinterpret_cast<const char*>(wp::host(output + 6)), "/tmp/save") == 0);
    CHECK(wp::rd8(output + 70) == 3 && wp::rd8(output + 71) == 3 && wp::rd8(output + 72) == 3 && wp::rd8(output + 73) == 0);
    CHECK(wp::rd8(output + 74) == 0xCC);
    CHECK(get_attributes("/tmp/missing") < 0);
    fs::remove_all(parent, error);
}

void test_descriptor_reuse() {
    const uint32_t request = 0x80206000;
    auto close = [&](int32_t descriptor) {
        wp::wr32(request, 2);
        wp::wr32(request + 8, static_cast<uint32_t>(descriptor));
        uint64_t ticks = 0;
        return wp::ios::send(request, ticks);
    };
    int32_t kept = wp::ios::open("/dev/di");
    int32_t highest = kept;
    for (int i = 0; i < 300; i++) {
        int32_t first = wp::ios::open("/dev/di");
        int32_t second = wp::ios::open("/dev/di");
        CHECK(first >= 0 && second >= 0 && first != second && first != kept && second != kept);
        highest = std::max(highest, std::max(first, second));
        CHECK(close(first) == 0);
        CHECK(close(second) == 0);
    }
    CHECK(highest < 24);
    CHECK(close(kept) == 0);
}

void test_pad_mapping() {
    using namespace wp::padmap;
    CHECK(format_binding(default_binding(3, true)) == "south");
    CHECK(format_binding(default_binding(2, true)) == "east,west");
    CHECK(format_binding(default_binding(0, true)) == "north,right_shoulder");
    CHECK(format_binding(default_binding(8, true)) == "left_shoulder+dpad_up");
    CHECK(format_binding(default_binding(kPointerAction, true)) == "left_stick");
    CHECK(format_binding(default_binding(0, false)) == "south,right_shoulder");
    CHECK(format_binding(default_binding(3, false)) == "north");
    CHECK(format_binding(parse_binding("auto", 2, true)) == "east,west");
    CHECK(format_binding(parse_binding(" West , EAST ", 2, true)) == "east,west");
    CHECK(format_binding(parse_binding("", 0, true)) == "north,right_shoulder");
    CHECK(format_binding(parse_binding("nimporte", 0, true)) == "north,right_shoulder");
    CHECK(parse_binding("none", 0, true).empty());
    CHECK(format_binding({}) == "none");
    CHECK(format_binding(parse_binding("dpad_up+Left_Shoulder", 8, true)) == "left_shoulder+dpad_up");
    Mapping mapping;
    for (size_t i = 0; i < kActionCount; i++) {
        mapping[i] = default_binding(i, true);
    }
    Pressed south;
    south.held[kSouth] = true;
    CHECK(buttons(south, mapping) == wp::input::kButtonTwo);
    mapping[3] = parse_binding("west", 3, true);
    Pressed west;
    west.held[kWest] = true;
    CHECK((buttons(west, mapping) & wp::input::kButtonTwo) != 0);
    CHECK(buttons(south, mapping) == 0);
    Pressed chord;
    chord.held[kLeftShoulder] = true;
    chord.held[kDpadUp] = true;
    mapping[3] = parse_binding("dpad_up", 3, true);
    CHECK(buttons(chord, mapping) == wp::input::kMotionSwingUp);
    Pressed stick;
    stick.held[kLeftStick] = true;
    CHECK(held(default_binding(kPointerAction, true), stick));
    CHECK(!held(default_binding(kPointerAction, true), south));
    CHECK(!held({}, south));
    CHECK(without("north,right_shoulder", kNorth, 0, true) == "right_shoulder");
    CHECK(without("north", kNorth, 0, true) == "none");
    CHECK(without("left_shoulder+dpad_up", kDpadUp, 8, true) == "left_shoulder+dpad_up");
    CHECK(physical_from_name("left_trigger") == kLeftTrigger);
    CHECK(physical_from_name("auto") < 0);
    CHECK(std::string(physical_name(kRightShoulder)) == "right_shoulder");
    CHECK(setting_key(6) == "pad.home");
}

void test_control_names() {
    using wp::padmap::Family;
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::control_names::pad_button(wp::padmap::kSouth, Family::Xbox) == "A");
    CHECK(wp::control_names::pad_button(wp::padmap::kSouth, Family::Switch) == "B");
    CHECK(wp::control_names::pad_button(wp::padmap::kSouth, Family::PlayStation4) == "Cross");
    CHECK(wp::control_names::pad_button(wp::padmap::kBack, Family::PlayStation5) == "Create");
    CHECK(wp::control_names::pad_button(wp::padmap::kBack, Family::Xbox360) == "Back");
    CHECK(wp::control_names::pad_button(wp::padmap::kStart, Family::PlayStation3) == "Start");
    CHECK(wp::control_names::pad_button(wp::padmap::kLeftTrigger, Family::Switch) == "ZL");
    CHECK(wp::control_names::pad_button(wp::padmap::kRightShoulder, Family::PlayStation4) == "R1");
    CHECK(wp::control_names::pad_binding(wp::padmap::parse_binding("north,right_shoulder", 0, true), Family::PlayStation4) == "Triangle, R1");
    CHECK(wp::control_names::pad_binding(wp::padmap::parse_binding("left_shoulder+dpad_up", 8, true), Family::Xbox) == "LB + D-pad up");
    CHECK(wp::control_names::pad_binding({}, Family::Xbox) == "\xE2\x80\x94");
    wp::ui::set_language(wp::ui::Language::French);
    CHECK(wp::control_names::pad_button(wp::padmap::kSouth, Family::PlayStation4) == "Croix");
    CHECK(wp::control_names::pad_button(wp::padmap::kDpadUp, Family::PlayStation4).find("Croix dir") == 0);
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        wp::ui::set_language(language);
        for (Family family : {Family::Xbox, Family::Xbox360, Family::PlayStation3, Family::PlayStation4, Family::PlayStation5, Family::Switch}) {
            for (int physical = 0; physical < wp::padmap::kPhysicalCount; physical++) {
                std::string name = wp::control_names::pad_button(physical, family);
                CHECK(!name.empty() && name.rfind("pad.", 0) != 0);
            }
        }
    }
    auto azerty = [](int code) -> uint32_t { return code == '1' ? '&' : code == '2' ? 0xE9 : code >= 'A' && code <= 'Z' ? static_cast<uint32_t>(code) : 0; };
    auto nothing = [](int) -> uint32_t { return 0; };
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::control_names::key('1', azerty) == "&");
    CHECK(wp::control_names::key('2', azerty) == "\xC3\xA9");
    CHECK(wp::control_names::key('E', azerty) == "E");
    CHECK(wp::control_names::key('E', nothing) == "E");
    CHECK(wp::control_names::key(0x01, azerty) == "Left click");
    CHECK(wp::control_names::key(0x20, azerty) == "Space");
    CHECK(wp::control_names::key(0x78, azerty) == "F9");
    CHECK(wp::control_names::key(0x26, azerty) == "Up");
    CHECK(wp::control_names::key(0x60, azerty) == "Num 0");
    CHECK(wp::control_names::keys({}) == "—");
    wp::ui::set_language(wp::ui::Language::French);
    CHECK(wp::control_names::key(0x01, azerty) == "Clic gauche");
    CHECK(wp::control_names::key(0x08, azerty) == "Retour arrière");
    CHECK(wp::control_names::key(0x20, azerty) == "Espace");
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        wp::ui::set_language(language);
        for (int code = 0; code < 0x200; code++) {
            if (!wp::keymap::key_name(code).empty()) {
                std::string name = wp::control_names::key(code, nothing);
                CHECK(!name.empty() && name.rfind("key.", 0) != 0);
            }
        }
    }
    wp::ui::set_language(wp::ui::Language::English);
}

void test_custom_texture_hash() {
    using wp::gx::custom_textures::xxh64;
    const char* sentence = "Nobody inspects the spammish repetition";
    uint8_t sequence[256];
    for (int i = 0; i < 256; i++) {
        sequence[i] = static_cast<uint8_t>(i);
    }
    CHECK(xxh64("", 0) == 0xEF46DB3751D8E999ull);
    CHECK(xxh64("a", 1) == 0xD24EC4F1A98C6E5Bull);
    CHECK(xxh64("abc", 3) == 0x44BC2CF5AD770999ull);
    CHECK(xxh64(sentence, std::strlen(sentence)) == 0xFBCEA83C8A378BF1ull);
    CHECK(xxh64(sequence, sizeof sequence) == 0x1FACBE8406CD904Bull);
    CHECK(xxh64("", 0, 0x9E3779B1) == 0xAC75FDA2929B17EFull);
    CHECK(xxh64("abc", 3, 0x9E3779B1) == 0x1318DF30094A85FDull);
    CHECK(xxh64(sentence, std::strlen(sentence), 0x9E3779B1) == 0x56DB22DD5B051147ull);
    CHECK(xxh64(sequence, sizeof sequence, 0x9E3779B1) == 0xD48195F45908996Cull);
}

void test_custom_texture_names() {
    namespace ct = wp::gx::custom_textures;
    const uint8_t c4[] = {0x35, 0x74};
    ct::PaletteRange range = ct::used_palette_range(c4, sizeof c4, 8);
    CHECK(range.first == 3 && range.count == 5);
    const uint8_t c8[] = {10, 200, 50};
    range = ct::used_palette_range(c8, sizeof c8, 9);
    CHECK(range.first == 10 && range.count == 191);
    const uint8_t c14[] = {0x12, 0x34, 0x00, 0x05, 0xC0, 0x07};
    range = ct::used_palette_range(c14, sizeof c14, 10);
    CHECK(range.first == 5 && range.count == 0x1230);
    range = ct::used_palette_range(c8, sizeof c8, 4);
    CHECK(range.count == 0);

    uint8_t data[32];
    for (int i = 0; i < 32; i++) {
        data[i] = static_cast<uint8_t>(i);
    }
    ct::TextureName plain = ct::name_texture(data, sizeof data, 4, 4, 4, false, nullptr);
    CHECK(plain.full() == "tex1_4x4_cbf59c5116ff32b4_4");
    CHECK(plain.wildcard() == "tex1_4x4_cbf59c5116ff32b4_$_4");
    CHECK(ct::name_texture(data, sizeof data, 4, 4, 4, true, nullptr).full() == "tex1_4x4_m_cbf59c5116ff32b4_4");

    uint8_t indices[32];
    for (int i = 0; i < 32; i++) {
        indices[i] = static_cast<uint8_t>(i % 4 + 2);
    }
    uint8_t tlut[512];
    for (int i = 0; i < 512; i++) {
        tlut[i] = static_cast<uint8_t>(i);
    }
    ct::TextureName palette = ct::name_texture(indices, sizeof indices, 8, 4, 9, false, tlut);
    CHECK(palette.full() == "tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9");
    CHECK(palette.wildcard() == "tex1_8x4_5e9da7e61227046a_$_9");
    CHECK(ct::level_name(palette.full(), 0) == palette.full());
    CHECK(ct::level_name(palette.full(), 2) == palette.full() + "_mip2");
}

void test_custom_texture_index() {
    namespace ct = wp::gx::custom_textures;
    ct::Index index;
    ct::TextureName name{"tex1_8x4", "5e9da7e61227046a", "_6ac8c5a2eb076b8a", "9"};
    CHECK(index.resolve(name).empty());
    CHECK(index.add("pack/menus/TEX1_8x4_5E9DA7E61227046A_6AC8C5A2EB076B8A_9.PNG"));
    CHECK(!index.add("pack/tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9.dds"));
    CHECK(!index.add("pack/readme.png"));
    CHECK(index.add("pack/tex1_4x4_0000000000000001_5_arb.png"));
    CHECK(index.add("pack/tex1_4x4_0000000000000001_5_mip1.png"));
    CHECK(index.size() == 3);
    CHECK(index.resolve(name) == "tex1_8x4_5e9da7e61227046a_6ac8c5a2eb076b8a_9");
    CHECK(index.find("tex1_4x4_0000000000000001_5") != nullptr);
    CHECK(index.find(ct::level_name("tex1_4x4_0000000000000001_5", 1)) != nullptr);
    CHECK(index.find(ct::level_name("tex1_4x4_0000000000000001_5", 2)) == nullptr);
    CHECK(index.add("pack/tex1_8x4_5e9da7e61227046a_$_9.png"));
    CHECK(index.resolve(name) == "tex1_8x4_5e9da7e61227046a_$_9");
    name.texture = "0000000000000000";
    CHECK(index.resolve(name).empty());

    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error) / "wp_custom_textures_test";
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root / "S2LP01" / "menus", error);
    std::ofstream(root / "S2LP01" / "menus" / "tex1_4x4_cbf59c5116ff32b4_4.png") << "x";
    std::ofstream(root / "S2LP01" / "notes.txt") << "x";
    ct::Index scanned;
    CHECK(scanned.scan(root) == 1);
    CHECK(scanned.find("tex1_4x4_cbf59c5116ff32b4_4") != nullptr);
    CHECK(scanned.scan(root / "missing") == 0);

    std::vector<uint32_t> pixels = {0xFF0000FFu, 0x8000FF00u, 0x00FF0000u, 0x12345678u, 0xFFFFFFFFu, 0x00000000u};
    std::filesystem::path png = root / "round_trip.png";
    wp::video::save_png_rgba(png.string().c_str(), pixels, 3, 2);
    std::vector<uint32_t> decoded;
    uint32_t width = 0;
    uint32_t height = 0;
    CHECK(ct::decode_png(png, decoded, width, height));
    CHECK(width == 3 && height == 2 && decoded == pixels);
    CHECK(!ct::decode_png(root / "S2LP01" / "menus" / "tex1_4x4_cbf59c5116ff32b4_4.png", decoded, width, height));
    CHECK(width == 3 && height == 2 && decoded == pixels);
    std::filesystem::remove_all(root, error);
}

int blend_factor(wp::gx::BlendFactor factor, int written, int tev, int destination) {
    using wp::gx::BlendFactor;
    switch (factor) {
    case BlendFactor::Zero:
        return 0;
    case BlendFactor::One:
        return 255;
    case BlendFactor::SourceColor:
    case BlendFactor::SourceAlpha:
        return written;
    case BlendFactor::InverseSourceColor:
    case BlendFactor::InverseSourceAlpha:
        return 255 - written;
    case BlendFactor::DestinationColor:
    case BlendFactor::DestinationAlpha:
        return destination;
    case BlendFactor::InverseDestinationColor:
    case BlendFactor::InverseDestinationAlpha:
        return 255 - destination;
    case BlendFactor::TevColor:
    case BlendFactor::TevAlpha:
        return tev;
    case BlendFactor::InverseTevAlpha:
        return 255 - tev;
    }
    return -1;
}

int blend_channel(const wp::gx::BlendState& state, bool alpha, int tev, int destination) {
    using wp::gx::LogicSource;
    int written = state.output == LogicSource::Tev ? tev : (state.output == LogicSource::Zero ? 0 : (state.output == LogicSource::One ? 255 : 255 - tev));
    if (!state.enable) {
        return written;
    }
    double source = written * blend_factor(alpha ? state.source_alpha : state.source, written, tev, destination) / (255.0 * 255.0);
    double target = destination * blend_factor(alpha ? state.destination_alpha : state.destination, written, tev, destination) / (255.0 * 255.0);
    wp::gx::BlendOperation operation = alpha ? state.alpha_operation : state.operation;
    double value = operation == wp::gx::BlendOperation::Add ? source + target
                   : (operation == wp::gx::BlendOperation::Subtract ? source - target : target - source);
    value = value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
    return static_cast<int>(value * 255.0 + 0.5);
}

int logic_reference(uint32_t op, int s, int d) {
    int results[16] = {0, s & d, s & ~d, s, ~s & d, d, s ^ d, s | d, ~(s | d), ~(s ^ d), ~d, s | ~d, ~s, ~s | d, ~(s & d), 255};
    return results[op] & 255;
}

void test_gx_logic_ops() {
    using namespace wp::gx;
    for (uint32_t op = 0; op < 16; op++) {
        uint32_t mode = 2 | (1u << 3) | (1u << 4) | (op << 12);
        BlendState state = blend_state(mode, true, false, true);
        CHECK(state.logic_op);
        CHECK(state.logic_mode == op);
        if (op == 5) {
            CHECK(!state.color_update && !state.alpha_update && !state.enable);
            CHECK(blend_state(mode, true, true, true).alpha_update);
            continue;
        }
        CHECK(state.color_update && state.alpha_update);
        bool binary = true;
        for (int s = 0; s <= 255; s += 255) {
            for (int d = 0; d <= 255; d += 255) {
                binary &= blend_channel(state, false, s, d) == logic_reference(op, s, d);
                binary &= blend_channel(state, true, s, d) == logic_reference(op, s, d);
            }
        }
        CHECK(binary);
        bool exact = true;
        for (int s = 0; s < 256; s++) {
            for (int d = 0; d < 256; d++) {
                exact &= blend_channel(state, false, s, d) == logic_reference(op, s, d) && blend_channel(state, true, s, d) == logic_reference(op, s, d);
            }
        }
        CHECK(exact == logic_op_exact(op));
        BlendState constant = blend_state(mode, true, true, true);
        CHECK(!constant.enable || (constant.source_alpha == BlendFactor::One && constant.destination_alpha == BlendFactor::Zero &&
                                   constant.alpha_operation == BlendOperation::Add));
    }
    CHECK(logic_op_exact(0) && logic_op_exact(3) && logic_op_exact(5) && logic_op_exact(10) && logic_op_exact(12) && logic_op_exact(15));
    CHECK(!logic_op_exact(1) && !logic_op_exact(6) && !logic_op_exact(7) && !logic_op_exact(14));
    CHECK(std::string(logic_op_name(6)) == "xor");

    uint32_t all = 1 | 2 | (1u << 11) | (1u << 3) | (1u << 4) | (6u << 12);
    BlendState subtract = blend_state(all, true, false, true);
    CHECK(subtract.enable && !subtract.logic_op && subtract.operation == BlendOperation::ReverseSubtract);
    CHECK(subtract.source == BlendFactor::One && subtract.destination == BlendFactor::One);
    BlendState subtract_alone = blend_state((1u << 11) | (1u << 3), true, false, true);
    CHECK(!subtract_alone.enable && !subtract_alone.logic_op);
    BlendState subtract_logic = blend_state(2 | (1u << 11) | (1u << 3) | (6u << 12), true, false, true);
    CHECK(subtract_logic.logic_op && subtract_logic.logic_mode == 6 && subtract_logic.operation == BlendOperation::Add);
    BlendState subtract_constant = blend_state(all, true, true, true);
    CHECK(subtract_constant.alpha_operation == BlendOperation::Add && subtract_constant.destination_alpha == BlendFactor::Zero);

    uint32_t blend = 1 | 2 | (1u << 3) | (1u << 4) | (4u << 8) | (5u << 5) | (6u << 12);
    BlendState alpha_blend = blend_state(blend, true, false, true);
    CHECK(alpha_blend.enable && !alpha_blend.logic_op && alpha_blend.operation == BlendOperation::Add);
    CHECK(alpha_blend.source == BlendFactor::TevAlpha && alpha_blend.destination == BlendFactor::InverseTevAlpha);
    CHECK(alpha_blend.source_alpha == BlendFactor::TevAlpha && alpha_blend.destination_alpha == BlendFactor::InverseTevAlpha);
    CHECK(alpha_blend.output == LogicSource::Tev);
    BlendState color_blend = blend_state(1 | (1u << 3) | (1u << 4) | (2u << 8) | (3u << 5), true, false, true);
    CHECK(color_blend.source == BlendFactor::DestinationColor && color_blend.destination == BlendFactor::InverseSourceColor);
    CHECK(color_blend.source_alpha == BlendFactor::DestinationAlpha && color_blend.destination_alpha == BlendFactor::InverseTevAlpha);
    BlendState no_alpha = blend_state(1 | (1u << 3) | (1u << 4) | (6u << 8) | (7u << 5), false, false, true);
    CHECK(no_alpha.source == BlendFactor::One && no_alpha.destination == BlendFactor::Zero && !no_alpha.alpha_update);
    BlendState constant_blend = blend_state(blend, true, true, true);
    CHECK(constant_blend.source_alpha == BlendFactor::One && constant_blend.destination_alpha == BlendFactor::Zero);
    BlendState failing = blend_state(blend, true, false, false);
    CHECK(!failing.color_update && !failing.alpha_update);
    BlendState disabled = blend_state((1u << 3) | (1u << 4) | (4u << 8), true, false, true);
    CHECK(!disabled.enable && !disabled.logic_op && disabled.output == LogicSource::Tev);
}

[[gnu::used]] const char kTestSalt[] = "TSALT22826";

void test_texture_keys() {
    using wp::gx::texture_key;
    // The old XOR key mixed address << 20 with height << 32: these two textures shared an entry.
    uint32_t image0_small = (63u << 10) | 63u;
    uint32_t image0_tall = (127u << 10) | 63u;
    CHECK(texture_key(image0_small, 0x1000, 0, 0) != texture_key(image0_tall, 0x1000, 0, 0));
    CHECK(texture_key(image0_small, 0x1000, 0, 0) != texture_key(image0_small, 0x1001, 0, 0));
    CHECK(texture_key(image0_small, 0x1000, 0, 0) != texture_key(image0_small | (1u << 20), 0x1000, 0, 0));
    CHECK(texture_key(image0_small, 0x1000, 0, 0) != texture_key(image0_small, 0x1000, 1, 0));
    CHECK(texture_key(image0_small, 0x1000, 0, 0x001) != texture_key(image0_small, 0x1000, 0, 0x400));
    CHECK(texture_key(image0_small, 0x1000, 12, 0xFFF) == texture_key(image0_small, 0x1000, 12, 0xFFF));
    CHECK(texture_key(0xFFFFFF, 0xFFFFFF, 15, 0xFFF) == ~0ull);
}

void test_texture_hash_sees_every_byte() {
    for (size_t size : {1u, 7u, 8u, 31u, 32u, 33u, 64u, 71u, 200u}) {
        std::vector<uint8_t> bytes(size);
        for (size_t i = 0; i < size; i++) {
            bytes[i] = static_cast<uint8_t>(i * 37 + 11);
        }
        uint64_t reference = wp::gx::render::hash_bytes(bytes.data(), size);
        for (size_t i = 0; i < size; i++) {
            bytes[i] ^= 0x40;
            CHECK(wp::gx::render::hash_bytes(bytes.data(), size) != reference);
            bytes[i] ^= 0x40;
        }
        CHECK(wp::gx::render::hash_bytes(bytes.data(), size) == reference);
    }
}

void test_fit_motion() {
    const float camera[4][4] = {{0.99f, 0.0f, 0.00000002f, 0.03f}, {0.0f, 1.0f, 0.0f, -0.01f}, {50000.0f, 0.0f, 0.98f, 100000.0f}, {0.01f, 0.0f, 0.0f, 1.02f}};
    std::mt19937 random(7);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::vector<std::array<float, 4>> from;
    std::vector<std::array<float, 4>> to;
    for (int i = 0; i < 300; i++) {
        float w = 5.0f + 100.0f * std::fabs(unit(random));
        std::array<float, 4> f = {unit(random) * w, unit(random) * w, 16000000.0f * (0.5f + 0.4f * unit(random)) * w, w};
        std::array<float, 4> t{};
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                t[r] += camera[r][c] * f[c];
            }
        }
        if (i % 5 == 0) {
            t[0] += 0.3f * w;
            t[1] -= 0.2f * w;
        }
        from.push_back(f);
        to.push_back(t);
    }
    auto points = [](const std::vector<std::array<float, 4>>& list) { return reinterpret_cast<const float(*)[4]>(list.data()); };
    float fitted[4][4];
    CHECK(wp::gx::fit_motion(points(from), points(to), from.size(), fitted));
    for (size_t i = 1; i < from.size(); i += 7) {
        if (i % 5 == 0) {
            continue;
        }
        for (int r : {0, 1, 3}) {
            float value = 0.0f;
            for (int c = 0; c < 4; c++) {
                value += fitted[r][c] * from[i][c];
            }
            CHECK(std::fabs(value - to[i][r]) < 0.002f * to[i][3]);
        }
    }
    for (auto& t : to) {
        t = {unit(random) * 50.0f, unit(random) * 50.0f, 0.0f, 50.0f};
    }
    CHECK(!wp::gx::fit_motion(points(from), points(to), from.size(), fitted));
    CHECK(!wp::gx::fit_motion(points(from), points(to), 5, fitted));
    from.clear();
    to.clear();
    for (int i = 0; i < 40; i++) {
        std::array<float, 4> f = {unit(random), unit(random), 1000.0f, 1.0f};
        from.push_back(f);
        to.push_back({f[0] + 0.1f, f[1], f[2], f[3]});
    }
    CHECK(wp::gx::fit_motion(points(from), points(to), from.size(), fitted));
    CHECK(std::fabs(fitted[0][0] * 0.5f + fitted[0][1] * 0.25f + fitted[0][2] * 1000.0f + fitted[0][3] - 0.6f) < 0.001f);
    from.clear();
    to.clear();
    for (int i = 0; i < 80; i++) {
        std::array<float, 4> f = {unit(random) * 230.0f, unit(random) * 230.0f, 0.00768637f, 230.0f};
        from.push_back(f);
        to.push_back(f);
    }
    for (int i = 0; i < 20; i++) {
        float w = 56.0f + unit(random);
        std::array<float, 4> f = {unit(random) * 10.0f, unit(random) * 10.0f, 0.00943592f, w};
        from.push_back(f);
        to.push_back({f[0] + (i % 2 == 0 ? 20.0f : -20.0f) + 6.0f * unit(random), f[1] + 6.0f * unit(random), f[2], f[3] + unit(random)});
    }
    CHECK(wp::gx::fit_motion(points(from), points(to), from.size(), fitted));
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            CHECK(std::fabs(fitted[r][c] - (r == c ? 1.0f : 0.0f)) < 0.001f);
        }
    }
}

void test_gx_line_point_offsets() {
    using namespace wp::gx;
    uint32_t bp[256] = {};
    CHECK(line_point_offsets(bp).line_coordinates == 0 && line_point_offsets(bp).point_coordinates == 0);
    bp[0x30] = (1u << 18) | 0xFF;
    bp[0x32] = 1u << 19;
    bp[0x34] = 3u << 18;
    bp[0x31] = 3u << 18;
    CHECK(line_point_offsets(bp).line_coordinates == 0 && line_point_offsets(bp).point_coordinates == 0);
    bp[0x22] = (3u << 16) | (5u << 19) | 0x1234;
    LinePointOffsets offsets = line_point_offsets(bp);
    CHECK(offsets.line == 0.25f && offsets.point == 1.0f);
    CHECK(offsets.line_coordinates == 5 && offsets.point_coordinates == 6);
    const float expected[8] = {0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 1.0f / 4.0f, 1.0f / 2.0f, 1.0f, 1.0f, 1.0f};
    for (uint32_t code = 0; code < 8; code++) {
        bp[0x22] = (code << 16) | (code << 19);
        CHECK(line_point_offsets(bp).line == expected[code] && line_point_offsets(bp).point == expected[code]);
    }
    CHECK(line_offset_negative_side(0.5f, 0.1f, false));
    CHECK(!line_offset_negative_side(-0.5f, 0.1f, false));
    CHECK(line_offset_negative_side(0.1f, -0.5f, true));
    CHECK(!line_offset_negative_side(0.1f, 0.5f, true));
    ScreenVertex vertex{};
    vertex.uv[0][0] = 0.5f;
    vertex.uv[2][1] = 0.25f;
    vertex.uv[0][2] = 1.0f;
    vertex.uv[2][2] = 1.0f;
    offset_texture_coordinates(vertex, 5, 0.125f, 0.5f);
    CHECK(vertex.uv[0][0] == 0.625f && vertex.uv[0][1] == 0.5f);
    CHECK(vertex.uv[1][0] == 0.0f && vertex.uv[1][1] == 0.0f);
    CHECK(vertex.uv[2][0] == 0.125f && vertex.uv[2][1] == 0.75f);
}

void test_key_names() {
    using namespace wp::keymap;
    CHECK(key_code("A") == 'A');
    CHECK(key_code("a") == 'A');
    CHECK(key_code("7") == '7');
    CHECK(key_code("F1") == 0x70);
    CHECK(key_code("f24") == 0x87);
    CHECK(key_code("F25") == -1);
    CHECK(key_code("F0") == -1);
    CHECK(key_code("F01") == -1);
    CHECK(key_code("Enter") == 0x0D);
    CHECK(key_code(" space ") == 0x20);
    CHECK(key_code("BACKSPACE") == 0x08);
    CHECK(key_code("Tab") == 0x09);
    CHECK(key_code("Shift") == 0x10);
    CHECK(key_code("LeftShift") == 0xA0);
    CHECK(key_code("Ctrl") == 0x11);
    CHECK(key_code("Alt") == 0x12);
    CHECK(key_code("Up") == 0x26);
    CHECK(key_code("Down") == 0x28);
    CHECK(key_code("Left") == 0x25);
    CHECK(key_code("Right") == 0x27);
    CHECK(key_code("Plus") == 0xBB);
    CHECK(key_code("Minus") == 0xBD);
    CHECK(key_code("NumPlus") == 0x6B);
    CHECK(key_code("MouseLeft") == 0x01);
    CHECK(key_code("MouseRight") == 0x02);
    CHECK(key_code("MouseMiddle") == 0x04);
    CHECK(key_code("MouseX1") == 0x05);
    CHECK(key_code("MouseX2") == 0x06);
    CHECK(key_code("WheelUp") == kWheelUp);
    CHECK(key_code("WheelDown") == kWheelDown);
    CHECK(key_code("") == -1);
    CHECK(key_code("Banana") == -1);
    CHECK(key_code("AB") == -1);
    CHECK(key_name('Q') == "Q");
    CHECK(key_name(0x7A) == "F11");
    CHECK(key_name(0xA0) == "LeftShift");
    CHECK(key_name(kWheelDown) == "WheelDown");
    CHECK(key_name(0x3000).empty());
    for (int code = 0; code < 0x200; code++) {
        std::string name = key_name(code);
        CHECK(name.empty() || key_code(name) == code);
    }
    CHECK(reserved(key_code("F11")));
    CHECK(reserved(key_code("F12")));
    CHECK(reserved(key_code("F10")));
    CHECK(reserved(key_code("F1")));
    CHECK(!reserved(key_code("F9")));
    Parsed parsed = parse(" Enter , space,MouseLeft,,Nope,F12,enter,F11 ");
    CHECK(parsed.codes.size() == 3);
    CHECK(format(parsed.codes) == "Enter,Space,MouseLeft");
    CHECK(format(parsed.codes, ", ") == "Enter, Space, MouseLeft");
    CHECK(parsed.invalid.size() == 3);
    CHECK(parsed.invalid.size() == 3 && parsed.invalid[0] == "Nope" && parsed.invalid[1] == "F12" && parsed.invalid[2] == "F11");
    CHECK(parse("").codes.empty() && parse("").invalid.empty());
}

void test_key_defaults() {
    using namespace wp::keymap;
    Bindings keys = defaults();
    CHECK(format(keys.of(Action::A)) == "E");
    CHECK(format(keys.of(Action::B)) == "C");
    CHECK(format(keys.of(Action::One)) == "MouseLeft,Backspace");
    CHECK(format(keys.of(Action::Two)) == "Space,Enter");
    CHECK(format(keys.of(Action::Plus)) == "Tab");
    CHECK(format(keys.of(Action::Minus)) == "P");
    CHECK(format(keys.of(Action::TiltLeft)) == "J");
    CHECK(format(keys.of(Action::TiltRight)) == "L");
    CHECK(format(keys.of(Action::TiltUp)) == "I");
    CHECK(format(keys.of(Action::TiltDown)) == "K");
    CHECK(!keys.bound(0xA0) && !keys.bound(0x04) && !keys.bound('1') && !keys.bound('2'));
    CHECK(format(keys.of(Action::Home)) == "H");
    CHECK(format(keys.of(Action::Right)) == "Right,D");
    CHECK(std::string(action(Action::Up).defaults) == "Up,W");
    auto azerty = [](int letter) { return letter == 'W' ? 'Z' : letter == 'Z' ? 'W' : letter == 'A' ? 'Q' : letter == 'Q' ? 'A' : letter; };
    CHECK(localize("Up,W", azerty) == "Up,Z");
    CHECK(localize("Left,A", azerty) == "Left,Q");
    CHECK(localize("Q", azerty) == "A");
    CHECK(localize("Enter,Space,MouseLeft", azerty) == "Enter,Space,MouseLeft");
    auto us_file = [](const std::string& key) {
        for (size_t i = 0; i < kActionCount; i++) {
            if (setting_key(i) == key) {
                return std::string(action(static_cast<Action>(i)).defaults);
            }
        }
        return std::string();
    };
    std::vector<size_t> pending = unlocalized(us_file);
    for (size_t i = 0; i < kActionCount; i++) {
        CHECK((std::find(pending.begin(), pending.end(), i) != pending.end()) == (local_defaults(i) != action(static_cast<Action>(i)).defaults));
    }
    if (!pending.empty()) {
        std::string changed = setting_key(pending[0]);
        CHECK(unlocalized([&](const std::string& key) { return key == changed ? std::string("F8") : us_file(key); }).empty());
    }
    std::vector<std::string> ignored;
    auto old_file = [](const std::string& key) {
        return std::string(key == "keys.up" ? "Up,A" : key == "keys.down" ? "Down,D" : key == "keys.left" ? "left, s" : key == "keys.right" ? "Right,W" : key == "keys.a" ? "Z" : "");
    };
    CHECK(legacy_directions(old_file));
    Bindings upgraded = load(old_file, ignored);
    CHECK(format(upgraded.of(Action::Left)) == local_defaults(static_cast<size_t>(Action::Left)));
    CHECK(format(upgraded.of(Action::Down)) == local_defaults(static_cast<size_t>(Action::Down)));
    CHECK(format(upgraded.of(Action::A)) == "Z");
    auto custom_file = [](const std::string& key) {
        return std::string(key == "keys.up" ? "Up,A" : key == "keys.down" ? "Down,D" : key == "keys.left" ? "Left,S" : key == "keys.right" ? "Right,K" : "");
    };
    CHECK(!legacy_directions(custom_file));
    CHECK(format(load(custom_file, ignored).of(Action::Up)) == "Up,A");
    CHECK(format(keys.of(Action::Shake)) == "MouseRight");
    CHECK(format(keys.of(Action::SwingUp)) == "WheelUp,T");
    CHECK(format(keys.of(Action::SwingDown)) == "WheelDown,G");
    CHECK(format(keys.of(Action::Screenshot)) == "F9");
    CHECK(action(Action::Screenshot).buttons == 0);
    CHECK(setting_key(static_cast<size_t>(Action::Screenshot)) == "keys.screenshot");
    CHECK(action(Action::A).buttons == wp::input::kButtonA);
    CHECK(action(Action::TiltDown).buttons == wp::input::kMotionTiltDown);
    CHECK(std::string(action(Action::MouseMotion).defaults) == "Z");
    CHECK(format(keys.of(Action::MouseMotion)) == local_defaults(static_cast<size_t>(Action::MouseMotion)));
    CHECK(action(Action::MouseMotion).buttons == 0);
    wp::input::MouseMotionState mouse;
    CHECK(wp::input::mouse_motion(mouse, false, 0, 0, 0) == 0);
    CHECK(wp::input::mouse_motion(mouse, true, 100, 100, 0) == 0);
    CHECK(wp::input::mouse_motion(mouse, true, 110, 100, 10) == wp::input::kMotionTiltRight);
    CHECK(wp::input::mouse_motion(mouse, true, 110, 100, 60) == wp::input::kMotionTiltRight);
    CHECK(wp::input::mouse_motion(mouse, true, 110, 100, 200) == 0);
    CHECK(wp::input::mouse_motion(mouse, true, 110, 90, 210) == wp::input::kMotionTiltUp);
    CHECK(wp::input::mouse_motion(mouse, true, 110, 40, 220) == wp::input::kMotionSwingUp);
    CHECK(wp::input::mouse_motion(mouse, true, 40, 40, 230) == wp::input::kMotionShake);
    CHECK(wp::input::mouse_motion(mouse, false, 40, 40, 240) == 0);
    CHECK(keys.has(Action::Two, 0x0D));
    CHECK(!keys.has(Action::B, 0x0D));
    CHECK(keys.bound('E'));
    CHECK(!keys.bound(0x7A));
    CHECK(!keys.bound(0x7B));
    for (size_t i = 0; i < kActionCount; i++) {
        CHECK(parse(action(i).defaults).invalid.empty());
        CHECK(!keys.codes[i].empty());
        CHECK(setting_key(i) == std::string("keys.") + action(i).name);
    }
    std::map<std::string, std::string> file = {{"keys.a", "K,Wrong"}, {"keys.screenshot", ""}, {"keys.b", "F12"}};
    std::vector<std::string> invalid;
    Bindings loaded = load(
        [&](const std::string& key) {
            auto it = file.find(key);
            if (it != file.end()) {
                return it->second;
            }
            for (size_t i = 0; i < kActionCount; i++) {
                if (setting_key(i) == key) {
                    return std::string(action(i).defaults);
                }
            }
            return std::string();
        },
        invalid);
    CHECK(format(loaded.of(Action::A)) == "K");
    CHECK(loaded.of(Action::Screenshot).empty());
    CHECK(loaded.of(Action::B).empty());
    CHECK(format(loaded.of(Action::Home)) == "H");
    CHECK(invalid.size() == 2 && invalid[0] == "keys.a: Wrong" && invalid[1] == "keys.b: F12");
}

std::function<std::string(const std::string&)> file_reader(const std::map<std::string, std::string>& file) {
    return [&file](const std::string& key) {
        auto it = file.find(key);
        return it != file.end() ? it->second : std::string();
    };
}

void test_bindings_upgrade() {
    using namespace wp::keymap;
    auto azerty = [](int letter) { return letter == 'W' ? 'Z' : letter == 'Z' ? 'W' : letter == 'A' ? 'Q' : letter == 'Q' ? 'A' : letter; };
    std::map<std::string, std::string> old_file = {
        {"keys.a", "Enter,Space,MouseLeft"}, {"keys.b", "Backspace,MouseRight"}, {"keys.one", "1"},       {"keys.two", "2"},
        {"keys.plus", "Plus,NumPlus"},      {"keys.minus", "Minus,NumMinus"},    {"keys.home", "H"},      {"keys.up", "Up,Z"},
        {"keys.down", "Down,S"},            {"keys.left", "Left,Q"},             {"keys.right", "Right,D"}, {"keys.shake", "MouseMiddle,LeftShift"},
        {"keys.swing_up", "WheelUp,T"},     {"keys.swing_down", "WheelDown,G"},  {"keys.tilt_left", "A"}, {"keys.tilt_right", "E"},
        {"keys.tilt_up", "R"},              {"keys.tilt_down", "F"},             {"keys.screenshot", "F9"}, {"keys.mouse_motion", "W"},
        {"keys.pointer", "Ctrl"},           {"keys.overlay", "F2"},
    };
    CHECK(outdated(file_reader(old_file), azerty).size() == 11);
    std::map<std::string, std::string> us_file = old_file;
    us_file["keys.tilt_left"] = "Q";
    CHECK(outdated(file_reader(us_file), azerty).size() == 11);
    std::map<std::string, std::string> custom = old_file;
    custom["keys.a"] = "Space";
    std::vector<size_t> pending = outdated(file_reader(custom), azerty);
    CHECK(pending.size() == 9);
    CHECK(std::find(pending.begin(), pending.end(), static_cast<size_t>(Action::A)) == pending.end());
    CHECK(std::find(pending.begin(), pending.end(), static_cast<size_t>(Action::Two)) == pending.end());
    std::map<std::string, std::string> holds_e = old_file;
    holds_e["keys.mouse_motion"] = "E";
    std::vector<size_t> kept_a = outdated(file_reader(holds_e), azerty);
    CHECK(kept_a.size() == 8);
    for (Action held_by_a : {Action::A, Action::One, Action::Two}) {
        CHECK(std::find(kept_a.begin(), kept_a.end(), static_cast<size_t>(held_by_a)) == kept_a.end());
    }
    CHECK(std::find(kept_a.begin(), kept_a.end(), static_cast<size_t>(Action::TiltRight)) != kept_a.end());
    std::map<std::string, std::string> fresh;
    for (size_t i = 0; i < kActionCount; i++) {
        fresh[setting_key(i)] = localize(action(i).defaults, azerty);
    }
    CHECK(outdated(file_reader(fresh), azerty).empty());
    CHECK(previous_defaults(static_cast<size_t>(Action::Home)) == nullptr);
    CHECK(std::string(previous_defaults(static_cast<size_t>(Action::One))) == "1");

    const char* path = "runtime_tests_bindings.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[keys]\na=Enter,Space,MouseLeft\nb=Backspace,MouseRight\none=1\ntwo=2\nplus=Plus,NumPlus\nminus=Minus,NumMinus\n"
                "shake=MouseMiddle,LeftShift\ntilt_left=Q\ntilt_right=E\ntilt_up=R\ntilt_down=F\n[pad]\na=auto\none= Auto \n";
    }
    wp::settings::load(path);
    wp::input::upgrade_bindings();
    CHECK(wp::settings::text("keys.one", nullptr) == "MouseLeft,Backspace");
    CHECK(wp::settings::text("keys.a", nullptr) == "E");
    CHECK(wp::settings::text("keys.shake", nullptr) == "MouseRight");
    CHECK(wp::settings::text("pad.a", nullptr) == "south,right_shoulder");
    CHECK(wp::settings::text("pad.one", nullptr) == "west");
    CHECK(wp::settings::number("input.bindings_version", nullptr) == 2);
    wp::settings::store("keys.one", "1");
    wp::input::upgrade_bindings();
    CHECK(wp::settings::text("keys.one", nullptr) == "1");
    std::remove(path);
    wp::settings::load("");
}

void test_keyboard_buttons() {
    wp::keymap::Bindings keys = wp::keymap::defaults();
    auto only = [](int code) { return [code](int c) { return c == code; }; };
    CHECK(wp::input::keyboard_buttons(keys, only(0x01), false) == wp::input::kButtonOne);
    CHECK(wp::input::keyboard_buttons(keys, only(0x01), true) == wp::input::kButtonA);
    CHECK(wp::input::keyboard_buttons(keys, only(0x08), true) == wp::input::kButtonOne);
    CHECK(wp::input::keyboard_buttons(keys, only(0x20), false) == wp::input::kButtonTwo);
    CHECK(wp::input::keyboard_buttons(keys, only(0x02), false) == wp::input::kMotionShake);
    CHECK(wp::input::keyboard_buttons(keys, only(0xA0), false) == 0);
    CHECK(wp::keymap::mouse(0x01) && wp::keymap::mouse(wp::keymap::kWheelUp) && !wp::keymap::mouse(0x20));
}

void test_settings_file_keys() {
    const char* path = "runtime_tests_settings.ini";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[keys]\na = K, MouseX1\nunknown=1\n";
    }
    wp::settings::load(path);
    CHECK(wp::settings::text("keys.a", nullptr) == "K, MouseX1");
    CHECK(wp::settings::text("keys.screenshot", nullptr) == "F9");
    CHECK(wp::settings::text("video.scale", nullptr) == "1");
    std::ifstream file(path, std::ios::binary);
    std::stringstream contents;
    contents << file.rdbuf();
    std::string text = contents.str();
    CHECK(text.find("[keys]\n") != std::string::npos);
    CHECK(text.find("a=K, MouseX1\n") != std::string::npos);
    CHECK(text.find("unknown") == std::string::npos);
    CHECK(text.find("[video]\nscale=1\n") == 0);
    for (size_t i = 0; i < wp::keymap::kActionCount; i++) {
        std::string line = "\n" + std::string(wp::keymap::action(i).name) + "=";
        CHECK(text.find(line, text.find("[keys]")) != std::string::npos);
    }
    file.close();
    std::remove(path);
}

void test_recent_images() {
    using namespace wp::screenshot;
    CHECK(recent_level(640) == 0);
    CHECK(recent_level(1280) == 0);
    CHECK(recent_level(1920) == 1);
    CHECK(recent_level(2560) == 1);
    CHECK(recent_level(3840) == 2);
    CHECK(recent_count(1280, 1056) == 198);
    CHECK(recent_count(640, 528) == 360);
    CHECK(recent_count(0, 528) == 0);
    std::tm time{};
    time.tm_year = 126;
    time.tm_mon = 8;
    time.tm_mday = 6;
    time.tm_hour = 9;
    time.tm_min = 5;
    time.tm_sec = 3;
    CHECK(recent_folder("pokepark2", time) == "pokepark2_2026-09-06_09-05-03_recent");
    CHECK(recent_name(7, false) == "007.png");
    CHECK(recent_name(112, true) == "112_between.png");
}

void test_screenshot_names() {
    std::tm time{};
    time.tm_year = 2026 - 1900;
    time.tm_mon = 8;
    time.tm_mday = 6;
    time.tm_hour = 9;
    time.tm_min = 5;
    time.tm_sec = 3;
    CHECK(wp::screenshot::file_name("pokepark2", time, 1) == "pokepark2_2026-09-06_09-05-03.png");
    CHECK(wp::screenshot::file_name("pokepark2", time, 2) == "pokepark2_2026-09-06_09-05-03_2.png");
    CHECK(wp::screenshot::file_name("pokepark2", time, 13) == "pokepark2_2026-09-06_09-05-03_13.png");
    time.tm_mon = 11;
    time.tm_mday = 31;
    time.tm_hour = 23;
    time.tm_min = 59;
    time.tm_sec = 59;
    CHECK(wp::screenshot::file_name("game", time, 1) == "game_2026-12-31_23-59-59.png");
    CHECK(std::string(wp::screenshot::kFolder) == "screenshots");
    std::vector<uint8_t> png = wp::video::encode_png({0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFFFF, 0, 0x00123456}, 3, 2);
    const uint8_t signature[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(png.size() > 45 && std::equal(signature, signature + 8, png.begin()));
    CHECK(png.size() > 24 && png[12] == 'I' && png[13] == 'H' && png[14] == 'D' && png[15] == 'R' && png[19] == 3 && png[23] == 2);
}

void test_live_settings() {
    const char* path = "runtime_tests_settings.ini";
    wp::settings::load(path);
    wp::settings::LiveFlag mute("audio.mute", nullptr);
    wp::settings::LiveNumber scale("video.scale", nullptr);
    CHECK(!mute());
    CHECK(scale() == 1);
    uint32_t revision = wp::settings::revision();
    wp::settings::store("audio.mute", "1");
    wp::settings::store("video.scale", "3");
    CHECK(wp::settings::revision() == revision + 2);
    CHECK(mute());
    CHECK(scale() == 3);
    wp::settings::load(path);
    CHECK(mute());
    CHECK(scale() == 3);
    CHECK(!wp::settings::reload_if_changed());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << "[video]\nscale=2\n[keys]\ntilt_left=Z\n";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "[audio]\nmute=0\n";
    }
    CHECK(wp::settings::reload_if_changed());
    CHECK(scale() == 2 && !mute());
    CHECK(wp::settings::text("keys.tilt_left", nullptr) == "Z");
    CHECK(!wp::settings::reload_if_changed());
    wp::settings::store("video.scale", "3");
    CHECK(!wp::settings::reload_if_changed());
    std::vector<std::string> keys = wp::settings::keys();
    CHECK(std::find(keys.begin(), keys.end(), "system.options_menu") != keys.end());
    CHECK(wp::settings::flag("system.options_menu", nullptr));
    wp::settings::load("");
    CHECK(!mute());
    CHECK(scale() == 1);
    std::remove(path);
}

void test_pad_motion() {
    using namespace wp::padmap;
    Mapping upright;
    for (size_t i = 0; i < kActionCount; i++) {
        upright[i] = default_binding(i, false);
    }
    Pressed pressed;
    CHECK(buttons(pressed, upright) == 0);
    pressed.held[kLeftTrigger] = true;
    CHECK(buttons(pressed, upright) == wp::input::kMotionShake);
    pressed = Pressed{};
    pressed.held[kRightStickUp] = true;
    CHECK(buttons(pressed, upright) == 0);
    pressed.held[kLeftShoulder] = true;
    CHECK(buttons(pressed, upright) == wp::input::kMotionTiltUp);
    pressed.held[kRightStickUp] = false;
    pressed.held[kRightStickLeft] = true;
    pressed.held[kDpadDown] = true;
    CHECK(buttons(pressed, upright) == (wp::input::kMotionTiltLeft | wp::input::kMotionSwingDown));
    Mapping custom = upright;
    custom[7] = parse_binding("north", 7, false);
    pressed = Pressed{};
    pressed.held[kNorth] = true;
    CHECK(buttons(pressed, custom) == (wp::input::kMotionShake | wp::input::kButtonTwo));
    pressed = Pressed{};
    pressed.held[kLeftTrigger] = true;
    CHECK(buttons(pressed, custom) == 0);
    custom = upright;
    custom[0] = parse_binding("left_trigger", 0, false);
    CHECK(buttons(pressed, custom) == (wp::input::kButtonA | wp::input::kMotionShake));
    CHECK(wp::padmap::device_key("Xbox 360 Controller", 1) == "Xbox 360 Controller#2");
    std::string players = wp::padmap::assign("", "Pad A#1", 3);
    CHECK(players == "Pad A#1=3" && wp::padmap::assigned(players, "Pad A#1") == 3 && wp::padmap::assigned(players, "Pad A#2") == 0);
    players = wp::padmap::assign(players, "Pad B#1", 1);
    players = wp::padmap::assign(players, "Pad A#1", 0);
    CHECK(players == "Pad B#1=1");
    std::vector<std::string> pad_keys = {"Pad A#1", "Pad A#2", "Pad B#1"};
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "", "", false) == std::vector<int>{0, 1, 2}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "", "", true) == std::vector<int>{1, 2, 3}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "Pad A#1", "", false) == std::vector<int>{-1, 0, 1}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "", "Pad B#1=1", false) == std::vector<int>{1, 2, 0}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "", "Pad A#1=2;Pad A#2=2", false) == std::vector<int>{1, 0, 2}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, 2, 0}, "", "", false) == std::vector<int>{1, 2, 0}));
    CHECK((wp::padmap::plan_slots(pad_keys, {-1, -1, -1}, "", "Pad B#1=1", true) == std::vector<int>{1, 2, 0}));
    std::string list = wp::padmap::toggled("", "Xbox 360 Controller#2");
    CHECK(list == "Xbox 360 Controller#2");
    CHECK(wp::padmap::listed(list, "Xbox 360 Controller#2"));
    CHECK(!wp::padmap::listed(list, "Xbox 360 Controller#1"));
    list = wp::padmap::toggled(list, "Pad B#1");
    CHECK(list == "Xbox 360 Controller#2;Pad B#1");
    list = wp::padmap::toggled(list, "Xbox 360 Controller#2");
    CHECK(list == "Pad B#1");
    CHECK(wp::padmap::toggled(list, "Pad B#1").empty());
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        for (const char* key : {"input.player1_keyboard_only", "pad.use", "pad.view_title", "pad.view_none", "pad.cross", "pad.circle", "pad.square", "pad.triangle", "pad.right_stick_up", "pad.right_stick_down", "pad.right_stick_left", "pad.right_stick_right",
                                "pad.motion_help"}) {
            CHECK(std::string(key) != wp::ui::label(key, language));
        }
    }
    std::vector<std::string> keys = wp::settings::keys();
    CHECK(std::find(keys.begin(), keys.end(), "input.player1_keyboard_only") != keys.end());
    CHECK(std::find(keys.begin(), keys.end(), "input.ignored_gamepads") == keys.end());
    wp::settings::load("");
    CHECK(!wp::settings::flag("input.player1_keyboard_only", nullptr));
    CHECK(wp::settings::text("input.ignored_gamepads", nullptr).empty());
}

void test_color() {
    using wp::color::Adjust;
    using wp::color::apply;
    Adjust original;
    CHECK(wp::color::neutral(original));
    CHECK(apply(0x102030, original) == 0x102030);
    CHECK(apply(0xFF0000, Adjust{1.0f, 1.0f, 0.0f, 1.0f}) == 0x363636);
    CHECK(apply(0x804020, Adjust{1.0f, 1.0f, 2.0f, 1.0f}) == 0xB53500);
    CHECK(apply(0x404040, Adjust{1.5f, 1.0f, 1.0f, 1.0f}) == 0x606060);
    CHECK(apply(0xC0C0C0, Adjust{1.5f, 1.0f, 1.0f, 1.0f}) == 0xFFFFFF);
    CHECK(apply(0x404040, Adjust{1.0f, 1.5f, 1.0f, 1.0f}) == 0x202020);
    CHECK(apply(0xC0C0C0, Adjust{1.0f, 1.5f, 1.0f, 1.0f}) == 0xE0E0E0);
    CHECK(apply(0x404040, Adjust{1.0f, 1.0f, 1.0f, 1.5f}) == 0x656565);
    CHECK(apply(0x808080, Adjust{1.0f, 1.0f, 1.0f, 0.5f}) == 0x404040);
    CHECK(apply(0x000000, Adjust{1.0f, 1.0f, 1.0f, 1.5f}) == 0x000000);
    CHECK(apply(0xFFFFFF, Adjust{1.0f, 1.0f, 1.0f, 1.5f}) == 0xFFFFFF);
    CHECK(!wp::color::neutral(Adjust{1.0f, 1.0f, 1.0f, 1.5f}));
    std::vector<uint32_t> pixels = {0xFF0000, 0x00FF00};
    wp::color::apply(pixels, Adjust{1.0f, 1.0f, 0.0f, 1.0f});
    CHECK(pixels[0] == 0x363636 && pixels[1] == 0xB6B6B6);

    wp::settings::load("");
    CHECK(wp::color::neutral(wp::color::current()));
    wp::settings::store("video.brightness", "150");
    wp::settings::store("video.contrast", "10");
    wp::settings::store("video.saturation", "500");
    wp::settings::store("video.gamma", "75");
    Adjust stored = wp::color::current();
    CHECK(stored.brightness == 1.5f && stored.contrast == 0.5f && stored.saturation == 2.0f && stored.gamma == 0.75f);
    wp::settings::store("video.saturation", "0");
    CHECK(wp::color::current().saturation == 0.0f);
    wp::settings::load("");
}

void test_facing_of_thin_triangles() {
    const float a[3] = {0x1.287a06p+9f, 0x1.541966p+9f, 0x1.d3fb02p+9f};
    const float b[3] = {0x1.193f02p+9f, 0x1.44e126p+9f, 0x1.cbf0bap+9f};
    const float c[3] = {0x1.29cd8p+9f, 0x1.566a28p+9f, 0x1.dbbb58p+9f};
    CHECK(wp::gx::facing_area(a, b, c) > 0.0);
    CHECK(wp::gx::facing_area(a, c, b) < 0.0);
    const float p[3] = {0.0f, 0.0f, 1.0f};
    const float q[3] = {1.0f, 0.0f, 1.0f};
    const float r[3] = {0.0f, 1.0f, 1.0f};
    CHECK(wp::gx::facing_area(p, q, r) == 1.0);
}

void test_guest_call_chain() {
    wp::wr32(0x80003000, 0x80003020);
    wp::wr32(0x80003020, 0x80003040);
    wp::wr32(0x80003024, 0x80201234);
    wp::wr32(0x80003040, 0);
    wp::wr32(0x80003044, 0x80205678);
    CHECK((wp::guest_call_chain(0x80003000, 8) == std::vector<uint32_t>{0x80201234, 0x80205678}));
    CHECK((wp::guest_call_chain(0x80003000, 1) == std::vector<uint32_t>{0x80201234}));
    CHECK(wp::guest_call_chain(0, 8).empty());
    CHECK(wp::guest_call_chain(0x12345678, 8).empty());
    wp::wr32(0x80003060, 0x80003060);
    CHECK(wp::guest_call_chain(0x80003060, 8).empty());
}

void test_stall_watch() {
    using std::chrono::seconds;
    std::chrono::steady_clock::time_point start{};
    wp::StallWatch watch(seconds(15));
    CHECK(!watch.update(0, false, start));
    CHECK(!watch.update(0, false, start + seconds(60)));
    CHECK(!watch.update(10, false, start + seconds(61)));
    CHECK(!watch.update(10, false, start + seconds(76)));
    CHECK(watch.update(10, false, start + seconds(77)));
    CHECK(!watch.update(10, false, start + seconds(120)));
    CHECK(!watch.update(11, false, start + seconds(121)));
    CHECK(!watch.update(11, true, start + seconds(140)));
    CHECK(!watch.update(11, false, start + seconds(150)));
    CHECK(watch.update(11, false, start + seconds(156)));
}

void test_token_wait() {
    CHECK(wp::gx::wait_for_token(std::chrono::milliseconds(0)));
}

void test_free_camera() {
    namespace fc = wp::free_camera;
    auto near = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
    fc::View view = fc::from_offset(0.0f, 0.0f, 10.0f);
    CHECK(near(view.yaw, 0.0f) && near(view.elevation, 0.0f) && near(view.distance, 10.0f));
    float offset[3];
    fc::to_offset(fc::View{1.0f, 0.5f, 20.0f}, offset);
    view = fc::from_offset(offset[0], offset[1], offset[2]);
    CHECK(near(view.yaw, 1.0f) && near(view.elevation, 0.5f) && near(view.distance, 20.0f));
    CHECK(near(fc::wrap(7.0f), 7.0f - 2.0f * 3.14159265f) && near(fc::wrap(-4.0f), -4.0f + 2.0f * 3.14159265f));

    fc::Settings settings{true, 1.0f, 1.0f, false};
    fc::Turn right = fc::turn(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, settings);
    CHECK(right.yaw < 0.0f && right.pitch == 0.0f);
    fc::Turn down = fc::turn(0.0f, 10.0f, 0.0f, 0.0f, 0.0f, settings);
    CHECK(down.pitch > 0.0f && down.yaw == 0.0f);
    fc::Turn inverted = fc::turn(0.0f, 10.0f, 0.0f, 0.0f, 0.0f, fc::Settings{true, 1.0f, 1.0f, true});
    CHECK(near(inverted.pitch, -down.pitch));
    fc::Turn faster = fc::turn(10.0f, 0.0f, 0.0f, 0.0f, 0.0f, fc::Settings{true, 2.0f, 1.0f, false});
    CHECK(near(faster.yaw, 2.0f * right.yaw));
    fc::Turn stick = fc::turn(0.0f, 0.0f, 1.0f, 0.0f, 0.5f, settings);
    CHECK(stick.yaw < 0.0f);
    fc::Turn idle = fc::turn(0.0f, 0.0f, 0.1f, -0.1f, 0.5f, settings);
    CHECK(idle.yaw == 0.0f && idle.pitch == 0.0f);

    const float target[3] = {100.0f, 5.0f, -50.0f};
    float game[3];
    fc::to_offset(fc::View{0.3f, 0.2f, 25.0f}, game);
    fc::Orbit orbit;
    float out[3];
    orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(out[0], game[0]) && near(out[1], game[1]) && near(out[2], game[2]));
    orbit.update(game, target, fc::Turn{0.5f, 0.0f}, false, 1.0f / 30.0f, out);
    view = fc::from_offset(out[0], out[1], out[2]);
    CHECK(near(view.yaw, 0.8f) && near(view.elevation, 0.2f) && near(view.distance, 25.0f));
    fc::to_offset(fc::View{-1.0f, 0.2f, 30.0f}, game);
    orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    view = fc::from_offset(out[0], out[1], out[2]);
    CHECK(near(view.yaw, 0.8f) && near(view.distance, 30.0f));

    orbit.update(game, target, fc::Turn{0.0f, 10.0f}, false, 1.0f / 30.0f, out);
    view = fc::from_offset(out[0], out[1], out[2]);
    CHECK(near(view.elevation, fc::kMaxElevation));
    orbit.update(game, target, fc::Turn{0.0f, -0.1f}, false, 1.0f / 30.0f, out);
    view = fc::from_offset(out[0], out[1], out[2]);
    CHECK(near(view.elevation, fc::kMaxElevation - 0.1f));
    orbit.update(game, target, fc::Turn{0.0f, -10.0f}, false, 1.0f / 30.0f, out);
    view = fc::from_offset(out[0], out[1], out[2]);
    CHECK(near(view.elevation, fc::kMinElevation));
    float rested[3];
    fc::to_offset(fc::View{view.yaw, 0.1f, view.distance}, rested);
    orbit.rest_on(rested[0], rested[1], rested[2]);
    orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(fc::from_offset(out[0], out[1], out[2]).elevation, 0.1f));
    orbit.rest_on(out[0], out[1] - 5.0f, out[2]);
    orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(fc::from_offset(out[0], out[1], out[2]).elevation, 0.1f));

    orbit.update(game, target, fc::Turn{}, true, 1.0f / 30.0f, out);
    for (int frame = 0; frame < 60; frame++) {
        orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    }
    CHECK(near(out[0], game[0]) && near(out[1], game[1]) && near(out[2], game[2]));

    orbit.update(game, target, fc::Turn{1.0f, 0.0f}, false, 1.0f / 30.0f, out);
    orbit.update(game, target, fc::Turn{}, true, 1.0f / 30.0f, out);
    orbit.update(game, target, fc::Turn{0.2f, 0.0f}, false, 1.0f / 30.0f, out);
    float before = fc::from_offset(out[0], out[1], out[2]).yaw;
    orbit.update(game, target, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(fc::from_offset(out[0], out[1], out[2]).yaw, before));

    const float moved[3] = {400.0f, 5.0f, -50.0f};
    orbit.update(game, moved, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(out[0], game[0]) && near(out[2], game[2]));
    orbit.update(game, moved, fc::Turn{0.4f, 0.0f}, false, 1.0f / 30.0f, out);
    orbit.reset();
    orbit.update(game, moved, fc::Turn{}, false, 1.0f / 30.0f, out);
    CHECK(near(out[0], game[0]) && near(out[2], game[2]));

    fc::note_buttons(wp::input::kButtonB);
    fc::note_buttons(wp::input::kButtonB);
    CHECK(fc::take_recenter());
    CHECK(!fc::take_recenter());
    fc::note_buttons(0);
    fc::note_buttons(wp::input::kButtonB | wp::input::kButtonA);
    CHECK(fc::take_recenter());

    fc::add_mouse(3, -4);
    fc::add_mouse(2, 1);
    float mx = 0.0f;
    float my = 0.0f;
    fc::take_mouse(mx, my);
    CHECK(mx == 5.0f && my == -3.0f);
    fc::take_mouse(mx, my);
    CHECK(mx == 0.0f && my == 0.0f);

    wp::settings::load("");
    fc::Settings defaults = fc::current();
    CHECK(defaults.enabled && defaults.mouse == 1.0f && defaults.stick == 1.0f && !defaults.invert);
    wp::settings::store("camera.mouse_speed", "200");
    wp::settings::store("camera.stick_speed", "50");
    wp::settings::store("camera.invert_y", "1");
    wp::settings::store("camera.free", "0");
    fc::Settings stored = fc::current();
    CHECK(!stored.enabled && stored.mouse == 2.0f && stored.stick == 0.5f && stored.invert);
    wp::settings::load("");

    std::vector<std::string> keyboard = wp::options::tab_keys(3, wp::options::Device::Keyboard);
    std::vector<std::string> gamepad = wp::options::tab_keys(3, wp::options::Device::Gamepad);
    auto has = [](const std::vector<std::string>& keys, const char* key) { return std::find(keys.begin(), keys.end(), key) != keys.end(); };
    CHECK(has(keyboard, "camera.free") && has(keyboard, "camera.mouse_speed") && has(keyboard, "camera.invert_y") && !has(keyboard, "camera.stick_speed"));
    CHECK(has(gamepad, "camera.free") && has(gamepad, "camera.stick_speed") && has(gamepad, "camera.invert_y") && !has(gamepad, "camera.mouse_speed"));
    CHECK(wp::options::kind("camera.free") == wp::options::Kind::Toggle && wp::options::kind("camera.mouse_speed") == wp::options::Kind::Slider);
    CHECK(wp::options::live("camera.free") && wp::options::live("camera.stick_speed"));
    CHECK(std::strcmp(wp::ui::label("camera.free", wp::ui::Language::French), "Caméra libre") == 0);
}

void test_options_menu() {
    wp::settings::load("");
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        wp::ui::set_language(language);
        for (size_t tab = 0; tab < wp::options::kTabCount; tab++) {
            CHECK(std::string(wp::options::tab_label(tab)).rfind("tab.", 0) != 0);
            wp::options::Menu probe;
            probe.set_open(true);
            probe.select_tab(tab);
            for (int device = 0; device < (tab == 3 ? 2 : 1); device++) {
                if (device == 1) {
                    probe.handle(wp::options::Action::Next);
                }
                for (const wp::options::Row& row : probe.rows()) {
                    CHECK(!row.label.empty() && row.label != row.key);
                    CHECK(!row.help.empty() && row.help.rfind("help.", 0) != 0);
                    if (row.kind == wp::options::Kind::Choice || row.kind == wp::options::Kind::Toggle || row.kind == wp::options::Kind::Slider) {
                        CHECK(!wp::options::choices(row.key).empty());
                    }
                }
            }
        }
    }
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        wp::ui::set_language(language);
        wp::options::Menu controls;
        controls.set_open(true);
        controls.select_tab(3);
        for (int device = 0; device < 2; device++) {
            if (device == 1) {
                controls.handle(wp::options::Action::Next);
            }
            std::set<std::string> helps;
            for (const wp::options::Row& row : controls.rows()) {
                if (row.kind == wp::options::Kind::Key || row.kind == wp::options::Kind::Pad) {
                    helps.insert(row.help);
                    CHECK(row.help.find("L1/LB") == std::string::npos);
                }
            }
            CHECK(helps.size() >= (device == 0 ? 12u : 9u));
        }
    }
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::options::kTabCount == 6);
    CHECK(std::strcmp(wp::ui::label("tab.screen", wp::ui::Language::French), "Écran") == 0);
    CHECK(std::strcmp(wp::ui::label("tab.advanced", wp::ui::Language::English), "Advanced") == 0);
    CHECK(std::strcmp(wp::ui::label("tab.controls", wp::ui::Language::Galician), "Controls") != 0);
    CHECK(std::strcmp(wp::ui::label("help.video.scale", wp::ui::Language::Spanish), wp::ui::label("help.video.scale", wp::ui::Language::English)) != 0);
    CHECK((wp::options::tab_keys(0) == std::vector<std::string>{"video.scale", "video.fullscreen", "system.widescreen", "video.vsync", "video.smooth", "video.anisotropy",
                                                                 "video.brightness", "video.contrast", "video.saturation", "video.gamma", "video.colors_reset"}));
    CHECK(std::strcmp(wp::ui::label("video.colors_reset", wp::ui::Language::French), "Couleurs d'origine") == 0);
    CHECK(std::strcmp(wp::ui::label("video.brightness", wp::ui::Language::French), "Luminosité") == 0);
    CHECK((wp::options::tab_keys(1) == std::vector<std::string>{"video.fps", "saves.backups", "system.discord", "stats.play_time"}));
    CHECK((wp::options::tab_keys(2) == std::vector<std::string>{"audio.volume", "audio.mute", "audio.wiimote_speaker"}));
    CHECK((wp::options::tab_keys(4) == std::vector<std::string>{"system.interface_language", "system.language"}));
    CHECK((wp::options::tab_keys(5) == std::vector<std::string>{"overlay.show", "overlay.level", "overlay.corner", "video.show_fps", "video.custom_textures", "video.dump_textures", "video.bug_recorder"}));
    std::vector<std::string> keyboard = wp::options::tab_keys(3, wp::options::Device::Keyboard);
    CHECK(keyboard[0] == "controls.device" && keyboard[1] == "input.hide_cursor" && keyboard.back() == "keys.reset");
    CHECK(std::find(keyboard.begin(), keyboard.end(), "keys.overlay") != keyboard.end());
    std::vector<std::string> gamepad = wp::options::tab_keys(3, wp::options::Device::Gamepad);
    CHECK(gamepad[0] == "controls.device" && gamepad[1] == "input.gamepads" && gamepad.back() == "pad.reset");
    CHECK(wp::options::kind("stats.play_time") == wp::options::Kind::Info);
    CHECK(wp::options::kind("controls.device") == wp::options::Kind::Choice);
    CHECK(wp::options::kind("overlay.show") == wp::options::Kind::Toggle);
    CHECK(wp::options::live("overlay.show") && wp::options::live("overlay.level") && wp::options::live("overlay.corner"));
    CHECK((wp::options::choices("overlay.level") == std::vector<std::string>{"minimal", "standard", "full"}));
    CHECK((wp::options::choices("overlay.corner") == std::vector<std::string>{"top_left", "top_right", "bottom_left", "bottom_right"}));
    CHECK(wp::settings::text("overlay.level", nullptr) == "full" && !wp::settings::flag("overlay.show", nullptr));
    CHECK(wp::settings::text("overlay.corner", nullptr) == "top_left");
    CHECK(wp::settings::text("keys.overlay", nullptr) == "F2");
    wp::ui::set_language(wp::ui::Language::French);
    CHECK(wp::options::shown_value("overlay.level", "full") == "Complet");
    CHECK(wp::options::shown_value("overlay.corner", "bottom_right") == "Bas droite");
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::options::shown_value("overlay.level", "minimal") == "Minimal");
    CHECK(wp::options::shown_value("audio.mute", "1") == "On");
    CHECK(wp::options::shown_value("audio.volume", "75") == "75%");
    CHECK(wp::options::shown_value("video.anisotropy", "1") == "Off");
    CHECK(wp::options::shown_value("video.anisotropy", "8") == "8x");
    CHECK(wp::options::shown_value("video.fps", "90") == "90");
    wp::pacing::set_display_hz(75);
    CHECK(wp::options::shown_value("video.fps", "90") == "90 > 60 (75 Hz)" && wp::options::shown_value("video.fps", "60") == "60");
    wp::pacing::set_display_hz(0);
    CHECK(wp::options::shown_value("keys.up", "Up,W") == "Up, W");
    CHECK(wp::options::shown_value("pad.a", "auto") == "A, RB");
    CHECK(wp::options::shown_value("pad.a", "south") == "A");
    for (wp::ui::Language language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        wp::ui::set_language(language);
        for (size_t i = 0; i < wp::padmap::kActionCount; i++) {
            std::string shown = wp::options::shown_value(wp::padmap::setting_key(i), "auto");
            CHECK(shown.find("Automati") == std::string::npos && shown.find("utom") == std::string::npos);
        }
    }
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(wp::options::kind("audio.volume") == wp::options::Kind::Slider);
    CHECK(wp::options::kind("keys.a") == wp::options::Kind::Key);
    CHECK(wp::options::kind("pad.a") == wp::options::Kind::Pad);
    CHECK(wp::options::kind("audio.mute") == wp::options::Kind::Toggle);
    CHECK(wp::options::kind("keys.reset") == wp::options::Kind::Button);
    CHECK(wp::options::step_value("audio.volume", "300", 1) == "300");
    CHECK(wp::options::step_value("audio.volume", "0", -1) == "0");
    CHECK(wp::options::step_value("audio.volume", "52", 1) == "55");
    wp::ui::set_language(wp::ui::Language::Spanish);
    CHECK(wp::options::shown_value("audio.mute", "0") == "No");
    wp::ui::set_language(wp::ui::Language::English);

    wp::options::Menu menu;
    menu.set_open(true);
    std::vector<wp::options::Row> rows = menu.rows();
    CHECK(rows[0].key == "video.scale" && rows[0].value == "Native" && rows[0].live);
    CHECK(menu.handle(wp::options::Action::Next) == "video.scale");
    CHECK(wp::settings::number("video.scale", nullptr) == 2);
    CHECK(menu.handle(wp::options::Action::Previous) == "video.scale");
    CHECK(menu.handle(wp::options::Action::Previous) == "video.scale");
    CHECK(wp::settings::number("video.scale", nullptr) == 6);
    CHECK(menu.handle(wp::options::Action::Up).empty());
    CHECK(menu.selection() == rows.size() - 1);

    CHECK(menu.handle(wp::options::Action::TabNext).empty());
    CHECK(menu.tab() == 1 && menu.selection() == 0);
    menu.select(3);
    rows = menu.rows();
    CHECK(rows[3].kind == wp::options::Kind::Info && !rows[3].value.empty() && rows[3].live);
    CHECK(menu.handle(wp::options::Action::Accept).empty() && menu.handle(wp::options::Action::Next).empty());
    CHECK(menu.handle(wp::options::Action::Previous).empty() && !menu.capturing());

    menu.select_tab(3);
    rows = menu.rows();
    CHECK(rows[0].key == "controls.device" && menu.device() == wp::options::Device::Keyboard);
    CHECK(rows[0].value == "Keyboard and mouse" && rows[0].live);
    size_t keys = rows.size();
    CHECK(keys > wp::options::kVisibleRows);
    CHECK(menu.handle(wp::options::Action::Up).empty());
    CHECK(menu.selection() == keys - 1 && menu.scroll() == keys - wp::options::kVisibleRows);
    size_t first = 0;
    while (first < keys && rows[first].key != "keys.a") {
        first++;
    }
    CHECK(first == 5);
    menu.select(first);
    CHECK(menu.rows()[first].help == "Calls a partner Pokémon or switches to it.");
    CHECK(menu.handle(wp::options::Action::Accept).empty());
    CHECK(menu.capturing() && menu.capture_kind() == wp::options::Kind::Key);
    CHECK(menu.rows()[first].help == "Press the new key or mouse button; Esc cancels.");
    CHECK(menu.rows()[first + 1].help != menu.rows()[first].help);
    CHECK(menu.capture("K") == "keys.a");
    CHECK(!menu.capturing());
    CHECK(wp::settings::text("keys.a", nullptr) == "K");
    menu.select(keys - 1);
    CHECK(menu.handle(wp::options::Action::Accept) == "keys.reset");
    CHECK(wp::settings::text("keys.a", nullptr) == "E");
    menu.select(first);
    CHECK(menu.handle(wp::options::Action::Accept).empty());
    CHECK(menu.handle(wp::options::Action::Down).empty());
    CHECK(!menu.capturing() && menu.selection() == first);
    menu.select(0);
    CHECK(menu.handle(wp::options::Action::Next).empty());
    rows = menu.rows();
    CHECK(menu.device() == wp::options::Device::Gamepad && rows[1].key == "input.gamepads" && rows[0].value == "Gamepad");
    CHECK(menu.selection() == 0 && menu.scroll() == 0);
    menu.select(rows.size() - 2);
    CHECK(menu.handle(wp::options::Action::Accept).empty() && menu.capture_kind() == wp::options::Kind::Pad);
    CHECK(menu.rows()[menu.selection()].help == "Press the new button on the gamepad.");
    CHECK(menu.capture("south") == rows[rows.size() - 2].key);
    CHECK(wp::settings::text("pad.pointer", nullptr) == "south");
    CHECK(wp::settings::text("pad.a", nullptr) == "right_shoulder");
    for (size_t i = 0; i < rows.size(); i++) {
        if (rows[i].key == "pad.shake") {
            menu.select(i);
        }
    }
    CHECK(menu.handle(wp::options::Action::Accept).empty());
    CHECK(menu.capture("south") == "pad.shake");
    CHECK(wp::settings::text("pad.shake", nullptr) == "south");
    CHECK(wp::settings::text("pad.pointer", nullptr) == "none");
    menu.select(rows.size() - 1);
    CHECK(menu.handle(wp::options::Action::Accept) == "pad.reset");
    CHECK(wp::settings::text("pad.a", nullptr) == "south,right_shoulder");
    CHECK(wp::settings::text("pad.pointer", nullptr) == "left_stick");
    menu.select(0);
    CHECK(menu.handle(wp::options::Action::Accept).empty() && menu.device() == wp::options::Device::Keyboard);
    menu.handle(wp::options::Action::Previous);
    CHECK(menu.device() == wp::options::Device::Gamepad);
    menu.set_open(true);
    CHECK(menu.device() == wp::options::Device::Keyboard);

    menu.select_tab(0);
    std::vector<wp::options::Row> screen = menu.rows();
    CHECK(screen.size() == 11 && screen[6].key == "video.brightness" && screen[10].key == "video.colors_reset");
    for (size_t i = 6; i < 10; i++) {
        CHECK(screen[i].kind == wp::options::Kind::Slider && screen[i].value == "100%" && screen[i].live && screen[i].fraction == 0.5);
    }
    CHECK(screen[10].kind == wp::options::Kind::Button && screen[10].live);
    menu.select(6);
    CHECK(menu.handle(wp::options::Action::Next) == "video.brightness");
    CHECK(wp::settings::number("video.brightness", nullptr) == 105);
    wp::settings::store("video.saturation", "0");
    menu.select(10);
    CHECK(menu.handle(wp::options::Action::Accept) == "video.colors_reset");
    CHECK(wp::settings::number("video.brightness", nullptr) == 100 && wp::settings::number("video.saturation", nullptr) == 100);

    menu.select_tab(2);
    CHECK(menu.rows()[0].key == "audio.volume");
    CHECK(menu.handle(wp::options::Action::Previous) == "audio.volume");
    CHECK(wp::settings::number("audio.volume", nullptr) == 95);
    CHECK(menu.set_fraction(0, 0.5) == "audio.volume");
    CHECK(wp::settings::number("audio.volume", nullptr) == 150);
    CHECK(menu.rows()[0].fraction == 0.5);
    CHECK(menu.set_fraction(1, 0.5).empty());
    menu.select(1);
    CHECK(menu.rows()[1].kind == wp::options::Kind::Toggle);
    CHECK(menu.handle(wp::options::Action::Next).empty() && !wp::settings::flag("audio.mute", nullptr));
    CHECK(menu.handle(wp::options::Action::Previous).empty() && !wp::settings::flag("audio.mute", nullptr));
    CHECK(menu.handle(wp::options::Action::Accept) == "audio.mute" && wp::settings::flag("audio.mute", nullptr));
    CHECK(menu.handle(wp::options::Action::Next).empty() && wp::settings::flag("audio.mute", nullptr));
    CHECK(menu.set_toggle(1, false) == "audio.mute" && !wp::settings::flag("audio.mute", nullptr));
    CHECK(menu.set_toggle(1, false).empty());
    CHECK(menu.set_toggle(0, true).empty());
    std::string english = menu.rows()[1].help;
    wp::ui::set_language(wp::ui::Language::French);
    CHECK(menu.rows()[1].help != english && menu.rows()[1].label == "Couper le son");
    wp::ui::set_language(wp::ui::Language::English);
    CHECK(menu.handle(wp::options::Action::TabPrevious).empty());
    CHECK(menu.tab() == 1);
    CHECK(menu.handle(wp::options::Action::Close).empty());
    CHECK(!menu.open());
    wp::settings::load("");
}

void test_options_repeat() {
    wp::options::Repeat repeat(0x3, 400, 100);
    CHECK(repeat.update(0x1, 0) == 0x1);
    CHECK(repeat.update(0x1, 100) == 0);
    CHECK(repeat.update(0x1, 399) == 0);
    CHECK(repeat.update(0x1, 400) == 0x1);
    CHECK(repeat.update(0x1, 450) == 0);
    CHECK(repeat.update(0x1, 500) == 0x1);
    CHECK(repeat.update(0x5, 510) == 0x4);
    CHECK(repeat.update(0x5, 700) == 0x1);
    CHECK(repeat.update(0x0, 800) == 0);
    CHECK(repeat.update(0x4, 810) == 0x4);
    CHECK(repeat.update(0x4, 5000) == 0);
}

void test_options_layout() {
    wp::options::Layout frame = wp::options::layout(720, 21, 0);
    CHECK(frame.unit == 18 && frame.edge == 10 && frame.padding == 54);
    CHECK(frame.height <= 720 && frame.width <= 1280);
    CHECK(wp::options::layout(100, 3, 0).unit == 14);
    CHECK(wp::options::layout(4000, 3, 0).unit == 40);
    wp::options::Rect last_tab = wp::options::tab_rect(frame, wp::options::kTabCount - 1);
    wp::options::Rect rows = wp::options::rows_rect(frame);
    CHECK(last_tab.y + last_tab.height <= rows.y + rows.height);
    CHECK(last_tab.x + last_tab.width < rows.x);
    CHECK(rows.x + rows.width <= frame.width - frame.edge - frame.padding);
    wp::options::Rect help = wp::options::help_rect(frame);
    wp::options::Rect hints = wp::options::hints_rect(frame);
    CHECK(help.y >= rows.y + rows.height && hints.y >= help.y + help.height);
    CHECK(hints.y + hints.height <= frame.height - frame.edge);
    CHECK(help.x == frame.padding && help.x + help.width == frame.width - frame.edge - frame.padding);

    wp::options::Rect placed = wp::options::place(1280, 720, frame.width, frame.height);
    CHECK(placed.width == frame.width && placed.height == frame.height);
    CHECK(placed.x == (1280 - frame.width) / 2 && placed.y == (720 - frame.height) / 2);
    wp::options::Rect half = wp::options::place(frame.width / 2, 720, frame.width, frame.height);
    CHECK(half.width == frame.width / 2 && half.x == 0);
    wp::options::Layout small = wp::options::layout(480, 21, 0);
    wp::options::Rect fitted = wp::options::place(640, 480, small.width, small.height);
    CHECK(fitted.x >= 0 && fitted.y >= 0 && fitted.x + fitted.width <= 640 && fitted.y + fitted.height <= 480);

    CHECK(wp::options::hit_test(frame, placed, placed.x + last_tab.x + 4, placed.y + last_tab.y + 4).tab == 5);
    auto centre = [&](const wp::options::Rect& r, int dx = 0) {
        return wp::options::hit_test(frame, placed, placed.x + r.x + r.width / 2 + dx, placed.y + r.y + r.height / 2);
    };
    wp::options::Hover hit = centre(wp::options::tab_rect(frame, 3));
    CHECK(hit.part == wp::options::Part::Tab && hit.tab == 3 && hit.row == -1);
    wp::options::Rect row = wp::options::row_rect(frame, 2);
    hit = wp::options::hit_test(frame, placed, placed.x + row.x + frame.unit, placed.y + row.y + row.height / 2);
    CHECK(hit.row == 2 && hit.part == wp::options::Part::Label);
    wp::options::Rect control = wp::options::control_rect(frame, 2);
    int control_y = placed.y + control.y + control.height / 2;
    hit = wp::options::hit_test(frame, placed, placed.x + control.x + frame.unit / 2, control_y);
    CHECK(hit.row == 2 && hit.part == wp::options::Part::Previous);
    hit = wp::options::hit_test(frame, placed, placed.x + control.x + control.width / 2, control_y);
    CHECK(hit.row == 2 && hit.part == wp::options::Part::Value);
    hit = wp::options::hit_test(frame, placed, placed.x + control.x + control.width - frame.unit / 2, control_y);
    CHECK(hit.row == 2 && hit.part == wp::options::Part::Next);
    wp::options::Layout scrolled = wp::options::layout(720, 21, 5);
    hit = wp::options::hit_test(scrolled, placed, placed.x + control.x + control.width / 2, control_y);
    CHECK(hit.row == 7);
    wp::options::Layout short_tab = wp::options::layout(720, 2, 0);
    wp::options::Rect missing = wp::options::row_rect(short_tab, 4);
    hit = wp::options::hit_test(short_tab, placed, placed.x + missing.x + missing.width / 2, placed.y + missing.y + missing.height / 2);
    CHECK(hit.row == -1);
    wp::options::Rect close = wp::options::close_rect(frame);
    CHECK(close.x + close.width == frame.width - frame.edge - frame.padding);
    hit = centre(close);
    CHECK(hit.row == -1 && hit.part == wp::options::Part::Close);
    hit = centre(help);
    CHECK(hit.row == -1 && hit.part == wp::options::Part::None);
    hit = wp::options::hit_test(frame, placed, placed.x - 5, placed.y + 5);
    CHECK(hit.row == -1 && hit.part == wp::options::Part::None);
    CHECK(wp::options::slider_fraction(frame, placed, 0, placed.x) == 0.0);
    CHECK(wp::options::slider_fraction(frame, placed, 0, placed.x + frame.width) == 1.0);
    wp::options::Rect first = wp::options::control_rect(frame, 0);
    CHECK(wp::options::control_fraction(frame, placed, 0, placed.x + first.x) == 0.0);
    CHECK(wp::options::control_fraction(frame, placed, 0, placed.x + first.x + first.width) == 1.0);
    CHECK(wp::options::control_fraction(frame, placed, 0, placed.x + first.x + first.width / 4) < 0.5);
    CHECK(wp::options::control_fraction(frame, half, 0, half.x + (first.x + first.width * 3 / 4) / 2) > 0.5);

    wp::options::Menu hovering;
    CHECK(hovering.set_hover(hit) == false);
    CHECK(hovering.set_hover(wp::options::Hover{3, wp::options::Part::Next}) == true);
    CHECK(hovering.set_hover(wp::options::Hover{3, wp::options::Part::Next}) == false);
    hovering.set_open(true);
    CHECK(hovering.hover().row == -1);

    uint64_t version = 0;
    wp::options::Image image;
    bool visible = false;
    CHECK(!wp::options::take_overlay(version, image, visible));
    wp::options::show_overlay(wp::options::Image{{1, 2, 3, 4}, 2, 2});
    CHECK(wp::options::take_overlay(version, image, visible));
    CHECK(visible && image.width == 2 && image.pixels.size() == 4);
    CHECK(!wp::options::take_overlay(version, image, visible));
    wp::options::hide_overlay();
    CHECK(wp::options::take_overlay(version, image, visible));
    CHECK(!visible && image.pixels.empty());
}

void test_canvas() {
    CHECK(!wp::canvas::embedded_font("nunito_extrabold").empty());
    CHECK(!wp::canvas::embedded_font("nunito_black").empty());
    CHECK(wp::canvas::embedded_font("missing").empty());
    CHECK(wp::canvas::font_ok(wp::canvas::FontId::Body) && wp::canvas::font_ok(wp::canvas::FontId::Heavy));
    CHECK(wp::canvas::font_ok(wp::canvas::FontId::Mono) && wp::canvas::font_ok(wp::canvas::FontId::System));
    wp::canvas::Face tight = wp::canvas::make_face(wp::canvas::FontId::Body, 20);
    wp::canvas::Face wide = wp::canvas::make_face(wp::canvas::FontId::Body, 20, 0.1f);
    CHECK(wp::canvas::text_width(wide, "Options") > wp::canvas::text_width(tight, "Options") + 10.0f);
    wp::canvas::Canvas canvas{40, 40, std::vector<uint32_t>(1600, 0)};
    canvas.fill_polygon({{{0, 0}}, {{20, 0}}, {{20, 20}}, {{0, 20}}}, 0xFFF27D0C);
    CHECK(canvas.pixels[5 * 40 + 5] == 0xFFF27D0C);
    CHECK(canvas.pixels[30 * 40 + 30] == 0);
    canvas.fill_polygon({{{0, 30}}, {{40, 30.5f}}, {{40, 40}}, {{0, 40}}}, 0xFF000000);
    unsigned edge = canvas.pixels[30 * 40 + 20] >> 24;
    CHECK(edge > 0 && edge < 255);
    canvas.blend(30, 5, 0x80FFFFFF, 255);
    CHECK((canvas.pixels[5 * 40 + 30] >> 24) == 0x80);
}

void test_play_time() {
    std::string path = (std::filesystem::temp_directory_path() / "wp_play_time_test.ini").string();
    std::filesystem::remove(path);
    CHECK(wp::play_time::read_file(path) == 0);
    CHECK(wp::play_time::write_file(path, 45296));
    CHECK(wp::play_time::read_file(path) == 45296);
    {
        std::ofstream bad(path);
        bad << "play_time_seconds=abc" << std::endl;
    }
    CHECK(wp::play_time::read_file(path) == 0);
    CHECK(wp::play_time::format(45296, wp::ui::Language::English) == "12h 34m");
    CHECK(wp::play_time::format(300, wp::ui::Language::English) == "0h 05m");
    CHECK(wp::play_time::format(45296, wp::ui::Language::French) == "12 h 34 min");
    CHECK(wp::play_time::format(59, wp::ui::Language::Spanish) == "0 h 00 min");
    CHECK(wp::play_time::seconds() == 0);
    wp::play_time::write_file(path, 120);
    wp::play_time::start(path);
    CHECK(wp::play_time::seconds() >= 120 && wp::play_time::seconds() < 125);
    wp::play_time::save();
    CHECK(wp::play_time::read_file(path) >= 120);
    std::filesystem::remove(path);
}

void test_system_stats() {
    wp::perf::system_cpu_percent();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double cpu = wp::perf::system_cpu_percent();
    double gpu = wp::perf::gpu_engine_percent();
    double used = 0;
    double total = 0;
    wp::perf::memory(used, total);
#ifdef _WIN32
    CHECK(cpu >= 0.0 && cpu <= 100.0);
    CHECK(used > 0.0 && total > used);
    CHECK(!wp::perf::cpu_name().empty() && wp::perf::cpu_name().back() != ' ');
#endif
    CHECK(cpu == -1.0 || (cpu >= 0.0 && cpu <= 100.0));
    CHECK(gpu == -1.0 || (gpu >= 0.0 && gpu <= 100.0));
    double own = wp::perf::gpu_process_percent();
    CHECK(own == -1.0 || (own >= 0.0 && own <= 100.0));
    wp::perf::Sample sample;
    CHECK(sample.fps == -1.0 && sample.vram_used_gb == -1.0 && sample.render_width == 0);
    sample.fps = 120;
    uint64_t before = wp::perf::revision();
    wp::perf::publish(sample);
    CHECK(wp::perf::revision() == before + 1 && wp::perf::latest().fps == 120);
    wp::perf::SystemFigures figures = wp::perf::system_figures();
    CHECK(figures.cpu_percent == -1.0 || (figures.cpu_percent >= 0.0 && figures.cpu_percent <= 100.0));
}

void save_menu_png(const wp::options::Image& image, const std::string& path) {
    std::vector<uint32_t> composed(image.pixels.size());
    for (uint32_t y = 0; y < image.height; y++) {
        for (uint32_t x = 0; x < image.width; x++) {
            size_t index = static_cast<size_t>(y) * image.width + x;
            uint32_t pixel = image.pixels[index];
            uint32_t alpha = pixel >> 24;
            uint32_t back = ((x / 16 + y / 16) % 2) ? 0x3CA0E0 : 0xF0C040;
            uint32_t result = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                uint32_t front = (pixel >> shift) & 0xFF;
                uint32_t behind = (back >> shift) & 0xFF;
                result |= ((front * alpha + behind * (255 - alpha)) / 255) << shift;
            }
            composed[index] = result;
        }
    }
    wp::video::save_png(path.c_str(), composed, image.width, image.height);
}

bool near(double a, double b) {
    return std::fabs(a - b) < 1e-6;
}

void test_frame_pacing() {
    using wp::pacing::plan_frame;
    double next = 0;
    wp::pacing::FramePlan plan = plan_frame(next, 1.0 / 60, 120, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 0.5) && plan.show_real && near(plan.slot, 1.0 / 120));
    plan = plan_frame(next, 1.0 / 30, 120, 3);
    CHECK(plan.images == 3 && near(plan.fractions[0], 0.25) && near(plan.fractions[1], 0.5) && near(plan.fractions[2], 0.75) && plan.show_real);
    plan = plan_frame(next, 1.0 / 50, 120, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 0.5) && plan.show_real && near(plan.slot, 0.01));

    next = 0;
    plan = plan_frame(next, 1.0 / 60, 90, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 2.0 / 3) && !plan.show_real && near(plan.slot, 1.0 / 90));
    plan = plan_frame(next, 1.0 / 60, 90, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 1.0 / 3) && plan.show_real && near(plan.slot, 1.0 / 90));
    plan = plan_frame(next, 1.0 / 60, 90, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 2.0 / 3) && !plan.show_real);
    next = 0;
    plan = plan_frame(next, 1.0 / 30, 90, 3);
    CHECK(plan.images == 2 && near(plan.fractions[0], 1.0 / 3) && near(plan.fractions[1], 2.0 / 3) && plan.show_real && near(plan.slot, 1.0 / 90));

    next = 0;
    uint32_t outputs = 0;
    uint32_t real = 0;
    for (int frame = 0; frame < 600; frame++) {
        plan = plan_frame(next, 1.0 / 59.94, 90, 3);
        outputs += plan.images + (plan.show_real ? 1 : 0);
        real += plan.show_real ? 1 : 0;
    }
    CHECK(outputs == 900 && real == 300);

    next = 1.0 / 3;
    plan = plan_frame(next, 1.0 / 60, 120, 3);
    CHECK(plan.images == 1 && near(plan.fractions[0], 0.5) && plan.show_real && next == 0);

    next = 0;
    plan = plan_frame(next, 0.1, 120, 3);
    CHECK(plan.images == 3 && plan.show_real && next == 0);
    CHECK(near(plan.fractions[0], 0.25) && near(plan.fractions[1], 0.5) && near(plan.fractions[2], 0.75) && near(plan.slot, 0.025));
    plan = plan_frame(next, 0.04, 120, 3);
    CHECK(plan.images == 3 && near(plan.fractions[0], 0.25) && near(plan.fractions[2], 0.75) && plan.show_real && near(plan.slot, 0.01));
    plan = plan_frame(next, 0.1, 90, 3);
    CHECK(plan.images == 3 && near(plan.fractions[0], 0.25) && plan.show_real && next == 0);
    next = 0.4;
    plan = plan_frame(next, 1.0 / 60, 90, 0);
    CHECK(plan.images == 0 && plan.show_real && next == 0);

    CHECK(wp::pacing::interpolation_wanted(120, 0) && wp::pacing::interpolation_wanted(120, 144) && !wp::pacing::interpolation_wanted(120, 90));
    CHECK(wp::pacing::interpolation_wanted(90, 0) && wp::pacing::interpolation_wanted(90, 89) && wp::pacing::interpolation_wanted(90, 144));
    CHECK(!wp::pacing::interpolation_wanted(90, 75) && !wp::pacing::interpolation_wanted(60, 144) && !wp::pacing::interpolation_wanted(45, 144) && !wp::pacing::interpolation_wanted(30, 0));
    CHECK(wp::pacing::output_rate(90) == 90 && wp::pacing::output_rate(120) == 120);

    constexpr uint64_t input = 1012500;
    constexpr uint64_t marker = input * 2 + 1;
    using wp::pacing::frame_alarm;
    wp::pacing::AlarmPeriods alarm = frame_alarm(input, input * 2, input * 2, 60, false);
    CHECK(alarm.period == input * 2 && alarm.pending == input);
    alarm = frame_alarm(input, input, input, 60, false);
    CHECK(alarm.period == input && alarm.pending == input);
    alarm = frame_alarm(input, input, input, 30, false);
    CHECK(alarm.period == input && alarm.pending == input * 2);
    alarm = frame_alarm(input, input, input, 90, false);
    CHECK(alarm.pending == input);
    alarm = frame_alarm(input, input, input, 120, false);
    CHECK(alarm.pending == input);
    alarm = frame_alarm(input, input, input, 60, true);
    CHECK(alarm.pending == input * 2);
    alarm = frame_alarm(input, input, input, 45, false);
    CHECK(alarm.period == input && alarm.pending == input * 4 / 3);
    alarm = frame_alarm(input, input * 4 / 3, input * 4 / 3, 45, false);
    CHECK(alarm.period == marker && alarm.pending == marker && marker / input == 2);
    alarm = frame_alarm(input, marker, marker, 45, false);
    CHECK(alarm.period == marker && alarm.pending == marker);
    alarm = frame_alarm(input, marker, input * 2, 45, false);
    CHECK(alarm.period == marker && alarm.pending == input * 4 / 3);
    alarm = frame_alarm(input, marker, marker, 60, false);
    CHECK(alarm.period == marker && alarm.pending == input);
    alarm = frame_alarm(input, marker, marker, 30, false);
    CHECK(alarm.pending == input * 2);
    alarm = frame_alarm(input, marker, marker, 45, true);
    CHECK(alarm.pending == input * 2);
    alarm = frame_alarm(0, input, input * 2, 45, false);
    CHECK(alarm.period == input && alarm.pending == input * 2);

    if (wp::pacing::kInBetweenImages) {
        CHECK((wp::options::choices("video.fps") == std::vector<std::string>{"30", "45", "60", "90", "120"}));
    } else {
        CHECK((wp::options::choices("video.fps") == std::vector<std::string>{"30", "45", "60"}));
    }
    for (auto language : {wp::ui::Language::English, wp::ui::Language::French, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
        std::string help = wp::ui::label("help.video.fps_without_between", language);
        CHECK(help.find("45") != std::string::npos && help.find("90") == std::string::npos);
    }
    CHECK(std::string(wp::ui::label("help.video.fps")).find("45") != std::string::npos);
    CHECK(std::string(wp::ui::label("help.video.fps", wp::ui::Language::French)).find("90") != std::string::npos);
    const std::pair<wp::ui::Language, const char*> experimental[] = {
        {wp::ui::Language::English, "experimental"}, {wp::ui::Language::French, "expérimentaux"},
        {wp::ui::Language::Spanish, "experimentales"}, {wp::ui::Language::Galician, "experimentais"}};
    for (const auto& [language, word] : experimental) {
        std::string help = wp::ui::label("help.video.fps", language);
        CHECK(help.find(word) != std::string::npos && help.find("60") != std::string::npos && help.find("120") != std::string::npos);
        for (int height : {720, 1080}) {
            wp::options::Layout frame = wp::options::layout(height, 6, 0);
            wp::options::Rect line = wp::options::help_rect(frame);
            wp::canvas::Face face = wp::canvas::make_face(wp::canvas::FontId::Body, frame.unit * 17 / 20, 0.02f);
            CHECK(wp::canvas::text_width(face, help) < (line.width - frame.unit - frame.unit / 3) * 1.8f);
        }
    }
}

void test_perf_overlay() {
    wp::perf::Sample sample;
    sample.fps = 120;
    sample.frame_ms = 8.33;
    sample.cpu_percent = 23;
    sample.game_thread_percent = 71;
    sample.gpu_thread_percent = 88;
    sample.gpu_percent = 64;
    sample.gpu_ms = 5.1;
    sample.vram_used_gb = 2.1;
    sample.vram_total_gb = 8.0;
    sample.ram_used_gb = 1.8;
    sample.ram_total_gb = 32;
    sample.cpu_name = "CPU X";
    sample.gpu_name = "GPU Y";
    sample.api = "D3D11";
    sample.render_width = 3840;
    sample.render_height = 3168;
    sample.scale = 6;
    sample.refresh_hz = 144;
    sample.vsync = true;
    auto minimal = wp::perf::lines(sample, wp::perf::Level::Minimal);
    CHECK(minimal.size() == 1 && minimal[0][0].text == "FPS" && minimal[0][1].text == "120" && minimal[0][2].text == "8.3 ms");
    auto standard = wp::perf::lines(sample, wp::perf::Level::Standard);
    CHECK(standard.size() == 4 && standard[1][0].text == "CPU" && standard[1][1].text == "23 %");
    CHECK(standard[2][0].text == "GPU" && standard[2][1].text == "64 %" && standard[2][2].text == "2.1 GB" && standard[3][1].text == "1.8 GB");
    auto full = wp::perf::lines(sample, wp::perf::Level::Full);
    CHECK(full.size() == 8 && full[1][2].text == "CPU X");
    CHECK(full[2][1].text == "71 %" && full[2][2].text == "GPU thr. 88 %" && full[3][2].text == "5.1 ms");
    CHECK(full[4][0].text == "VRAM" && full[4][2].text == "/ 8.0 GB" && full[5][2].text == "/ 32 GB");
    CHECK(full[6][0].text == "D3D11" && full[6][1].text == "3840\xC3\x97" "3168" && full[6][2].text == "6x \xC2\xB7 VSync on");
    CHECK(full[7].size() == 1 && full[7][0].text == "GPU Y \xC2\xB7 144 Hz");
    CHECK(full[0][0].color == 0xFFFFFF && full[1][0].color == 0x4FC3F7 && full[3][0].color == 0x81C784 && full[5][0].color == 0xF48FB1);
    CHECK(full[6][0].color == 0xFFB74D && full[0][2].color == 0x9AA0A6);
    auto empty = wp::perf::lines(wp::perf::Sample{}, wp::perf::Level::Full);
    const std::string dash = "\xE2\x80\x93";
    CHECK(empty.size() == 8 && empty[0][1].text == dash && empty[0][2].text == dash && empty[1][2].text == dash);
    CHECK(empty[3][1].text == dash && empty[4][1].text == dash && empty[6][0].text == dash && empty[7][0].text == dash);
    CHECK(wp::perf::level_from("standard") == wp::perf::Level::Standard && wp::perf::level_from("bad") == wp::perf::Level::Full);
    CHECK(wp::perf::corner_from("bottom_right") == wp::perf::Corner::BottomRight && wp::perf::corner_from("") == wp::perf::Corner::TopLeft);

    wp::options::Image image = wp::perf::render(full, 1080);
    CHECK(image.width > 0 && image.height > 0 && image.width < 1920 / 2 && image.height < 1080 / 2);
    CHECK(image.pixels.size() == static_cast<size_t>(image.width) * image.height);
    CHECK((image.pixels[static_cast<size_t>(image.height / 2) * image.width + 2] >> 24) >= 150);
    size_t blue = 0;
    for (uint32_t pixel : image.pixels) {
        blue += (pixel & 0xFFFFFF) == 0x4FC3F7;
    }
    CHECK(blue > 10);
    wp::options::Image tiny = wp::perf::render(minimal, 100);
    CHECK(tiny.height > 0 && tiny.height < 100);
    wp::options::Image dashes = wp::perf::render(empty, 720);
    CHECK(dashes.width > 0);
    if (const char* prefix = std::getenv("WP_MENU_PNG")) {
        save_menu_png(image, std::string(prefix) + "_overlay_full.png");
        save_menu_png(wp::perf::render(standard, 1080), std::string(prefix) + "_overlay_standard.png");
        save_menu_png(wp::perf::render(minimal, 1080), std::string(prefix) + "_overlay_minimal.png");
    }

    wp::options::Rect top_left = wp::perf::corner_rect(1920, 1080, 300, 200, wp::perf::Corner::TopLeft);
    CHECK(top_left.x == 27 && top_left.y == 27 && top_left.width == 300 && top_left.height == 200);
    wp::options::Rect bottom_right = wp::perf::corner_rect(1920, 1080, 300, 200, wp::perf::Corner::BottomRight);
    CHECK(bottom_right.x == 1920 - 27 - 300 && bottom_right.y == 1080 - 27 - 200);
    wp::options::Rect top_right = wp::perf::corner_rect(1920, 1080, 300, 200, wp::perf::Corner::TopRight);
    CHECK(top_right.x == 1920 - 27 - 300 && top_right.y == 27);
    wp::options::Rect squeezed = wp::perf::corner_rect(200, 100, 300, 200, wp::perf::Corner::BottomLeft);
    CHECK(squeezed.x >= 0 && squeezed.y >= 0 && squeezed.x + squeezed.width <= 200 && squeezed.y + squeezed.height <= 100 && squeezed.width > 0);

    CHECK(!wp::keymap::reserved(wp::keymap::key_code("F2")));
    CHECK(wp::input::bindings().has(wp::keymap::Action::Overlay, wp::keymap::key_code("F2")));
    uint64_t version = 0;
    bool visible = false;
    wp::perf::Corner corner = wp::perf::Corner::TopLeft;
    wp::options::Image taken;
    wp::perf::show(image, wp::perf::Corner::BottomRight);
    CHECK(wp::perf::take(version, taken, visible, corner) && visible && corner == wp::perf::Corner::BottomRight && taken.width == image.width);
    CHECK(!wp::perf::take(version, taken, visible, corner));
    wp::perf::hide();
    CHECK(wp::perf::take(version, taken, visible, corner) && !visible);
    CHECK(!wp::options::menu_capturing());
}

void test_options_render() {
    wp::settings::load("");
    const char* prefix = std::getenv("WP_MENU_PNG");
    for (size_t page = 0; page <= wp::options::kTabCount; page++) {
        size_t tab = page == wp::options::kTabCount ? 3 : page;
        wp::options::Menu menu;
        menu.set_open(true);
        menu.select_tab(tab);
        if (page == wp::options::kTabCount) {
            menu.handle(wp::options::Action::Next);
        }
        menu.select(tab == 3 ? 2 : 1);
        if (tab == 3 && page != wp::options::kTabCount) {
            menu.handle(wp::options::Action::Accept);
        }
        for (int height : {480, 720, 1080}) {
            for (wp::ui::Language language : {wp::ui::Language::French, wp::ui::Language::English, wp::ui::Language::Spanish, wp::ui::Language::Galician}) {
                wp::ui::set_language(language);
                wp::options::Image image = wp::options::render(menu, height);
                wp::options::Layout frame = wp::options::layout(height, menu.rows().size(), menu.scroll());
                CHECK(image.width == static_cast<uint32_t>(frame.width) && image.height == static_cast<uint32_t>(frame.height));
                CHECK(image.pixels.size() == static_cast<size_t>(image.width) * image.height);
                if (image.pixels.empty()) {
                    continue;
                }
                CHECK((image.pixels[0] >> 24) == 0);
                CHECK((image.pixels[image.width - 1] >> 24) == 0);
                CHECK((image.pixels[image.pixels.size() - image.width] >> 24) == 0);
                CHECK((image.pixels[static_cast<size_t>(image.height / 2) * image.width + image.width / 2] >> 24) == 255);
                size_t orange = 0;
                size_t panel = 0;
                size_t caps = 0;
                size_t marker = 0;
                for (uint32_t pixel : image.pixels) {
                    bool opaque = (pixel >> 24) == 255;
                    orange += opaque && (pixel & 0xFFFFFF) == 0xF27D0C;
                    panel += opaque && (pixel & 0xFFFFFF) == 0xF8F8F6;
                    caps += opaque && (pixel & 0xFFFFFF) == 0x3B3531;
                    marker += opaque && (pixel & 0xFFFFFF) == 0xE2343B;
                }
                CHECK(orange > 1000);
                CHECK(panel > image.pixels.size() / 4);
                CHECK(caps > 200);
                CHECK(marker > 10);
                if (height == 720 && page == 0) {
                    wp::canvas::Face face = wp::canvas::make_face(wp::canvas::FontId::Body, frame.unit, 0.05f);
                    for (size_t t = 0; t < wp::options::kTabCount; t++) {
                        CHECK(wp::canvas::text_width(face, wp::options::tab_label(t)) <= wp::options::tab_rect(frame, t).width - frame.unit);
                    }
                }
                if (prefix && height == 1080 && (language == wp::ui::Language::French || language == wp::ui::Language::English)) {
                    save_menu_png(image, std::string(prefix) + "_tab" + std::to_string(page) + (language == wp::ui::Language::French ? "_fr.png" : "_en.png"));
                }
            }
        }
    }
    if (prefix) {
        wp::options::Menu small;
        small.set_open(true);
        small.select_tab(0);
        wp::ui::set_language(wp::ui::Language::French);
        save_menu_png(wp::options::render(small, 480), std::string(prefix) + "_small_fr.png");
    }
    wp::ui::set_language(wp::ui::Language::English);
}

}

void test_hollywood_registers() {
    constexpr uint32_t kOut = 0xCD8000C0;
    constexpr uint32_t kDirection = 0xCD8000C4;
    constexpr uint32_t kIn = 0xCD8000C8;
    constexpr uint32_t kBroadwayOwned = 0xC3A0;
    CHECK(wp::hollywood::owns(kOut) && wp::hollywood::owns(kDirection) && wp::hollywood::owns(kIn));
    CHECK(wp::hollywood::owns(0xCD800180) && wp::hollywood::owns(0xCD8001CC) && wp::hollywood::owns(0xCD8001D0));
    CHECK(!wp::hollywood::owns(0xCD8000CC));
    CHECK(wp::hollywood::read32(kIn) == 0x80);
    wp::hollywood::write32(kIn, 0);
    CHECK(wp::hollywood::read32(kIn) == 0x80);
    CHECK(wp::hollywood::read32(kDirection) == 0xFFDF3F);
    wp::hollywood::write32(kOut, 0xFFFFFFFF);
    CHECK(wp::hollywood::read32(kOut) == kBroadwayOwned);
    wp::hollywood::write32(kOut, 0);
    CHECK(wp::hollywood::read32(kOut) == 0);
    wp::hollywood::write32(kDirection, 0);
    CHECK(wp::hollywood::read32(kDirection) == (0xFFDF3F & ~kBroadwayOwned));
    wp::hollywood::write32(kDirection, 0xFFFFFFFF);
    CHECK(wp::hollywood::read32(kDirection) == (0xFFDF3F | kBroadwayOwned));
    for (uint32_t address : {0xCD800180u, 0xCD8001CCu, 0xCD8001D0u}) {
        wp::hollywood::write32(address, 0x12345678);
        CHECK(wp::hollywood::read32(address) == 0);
    }
}

void test_wiimote_speaker_decoder() {
    wp::wiimote::SpeakerDecoder adpcm;
    const uint8_t adpcm_data[3] = {0x80, 0x00, 0x77};
    int16_t out[8] = {};
    CHECK(adpcm.decode(false, adpcm_data, 3, out) == 6);
    CHECK(out[0] == -15);
    CHECK(out[1] == 0);
    CHECK(out[2] == 15);
    CHECK(out[3] == 30);
    CHECK(out[4] > out[3]);
    CHECK(out[5] > out[4]);
    wp::wiimote::SpeakerDecoder big;
    const uint8_t loud[40] = {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
                              0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77};
    int16_t wide[80];
    CHECK(big.decode(false, loud, 40, wide) == 80);
    CHECK(wide[79] == 32767);
    CHECK(big.step <= 24576);
    wp::wiimote::SpeakerDecoder pcm;
    const uint8_t samples[3] = {0x7F, 0x80, 0x00};
    CHECK(pcm.decode(true, samples, 3, out) == 3);
    CHECK(out[0] == 127 * 256);
    CHECK(out[1] == -128 * 256);
    CHECK(out[2] == 0);
    CHECK(wp::wiimote::speaker_rate(false, 2000) == 6000);
    CHECK(wp::wiimote::speaker_rate(true, 6000) == 4000);
    CHECK(wp::wiimote::speaker_rate(false, 0) == 0);
}

void test_linked_fifo_registers() {
    constexpr uint32_t kBase = 0x00200000;
    constexpr uint32_t kRawEnd = kBase + 0x7C;
    auto cp = [](uint32_t low, uint32_t value) {
        wp::mmio::write16(low, static_cast<uint16_t>(value));
        wp::mmio::write16(low + 2, static_cast<uint16_t>(value >> 16));
    };
    auto cp_read = [](uint32_t low) { return static_cast<uint32_t>(wp::mmio::read16(low)) | (static_cast<uint32_t>(wp::mmio::read16(low + 2)) << 16); };
    wp::mmio::write32(0xCC00300C, kBase);
    wp::mmio::write32(0xCC003010, kRawEnd);
    wp::mmio::write32(0xCC003014, kBase);
    cp(0xCC000020, kBase);
    cp(0xCC000024, kRawEnd);
    cp(0xCC000034, kBase);
    cp(0xCC000038, kBase);
    cp(0xCC000030, 0);
    wp::mmio::write16(0xCC000002, 0x0011);
    auto burst = [&](uint8_t fill) {
        for (int i = 0; i < 4; i++) {
            wp::gx::push(0x0000000000000000ull, 8);
        }
        (void)fill;
    };
    burst(0);
    CHECK(wp::mmio::read32(0xCC003014) == kBase + 0x20);
    CHECK(cp_read(0xCC000034) == kBase + 0x20 && cp_read(0xCC000038) == kBase + 0x20 && cp_read(0xCC000030) == 0);
    burst(0);
    burst(0);
    CHECK(wp::mmio::read32(0xCC003014) == kBase + 0x60);
    burst(0);
    CHECK(wp::mmio::read32(0xCC003014) == kBase);
    CHECK(cp_read(0xCC000034) == kBase && cp_read(0xCC000038) == kBase);
    wp::gx::push(0x0000000000000000ull, 8);
    CHECK(wp::mmio::read32(0xCC003014) == kBase);
    wp::mmio::write16(0xCC00100A, 0x000F);
    CHECK(wp::mmio::read16(0xCC00100A) == 0x0003);
    wp::mmio::write16(0xCC000002, 0);
}

int main() {
    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));
    test_memory();
    test_rotate_and_shift();
    test_arithmetic();
    test_condition_register();
    test_floating_point();
    test_paired_singles();
    test_quantization();
    test_disc_drive();
    test_save_backups();
    test_adopt_root_files();
    test_title_data_directory();
    test_read_directory_forms();
    test_file_attributes();
    test_descriptor_reuse();
    test_pad_mapping();
    test_control_names();
    test_custom_texture_hash();
    test_custom_texture_names();
    test_custom_texture_index();
    test_gx_logic_ops();
    test_gx_line_point_offsets();
    test_texture_keys();
    test_fit_motion();
    test_texture_hash_sees_every_byte();
    test_key_names();
    test_key_defaults();
    test_bindings_upgrade();
    test_keyboard_buttons();
    test_settings_file_keys();
    test_screenshot_names();
    test_recent_images();
    test_live_settings();
    test_color();
    test_free_camera();
    test_token_wait();
    test_stall_watch();
    test_guest_call_chain();
    test_facing_of_thin_triangles();
    test_options_menu();
    test_pad_motion();
    test_options_repeat();
    test_options_layout();
    test_canvas();
    test_play_time();
    test_frame_pacing();
    test_system_stats();
    test_options_render();
    test_perf_overlay();
    test_software_fma();
    test_hollywood_registers();
    test_linked_fifo_registers();
    test_wiimote_speaker_decoder();
    std::free(wp::g_memory);
    if (failures == 0) {
        std::puts("all runtime tests passed");
    }
    return failures == 0 ? 0 : 1;
}
