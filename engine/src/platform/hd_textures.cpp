#include "wp/hd_textures.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "wp/custom_textures.h"
#include "wp/disc_textures.h"
#include "wp/game.h"
#include "wp/hint.h"
#include "wp/log.h"
#include "wp/ui_text.h"

namespace wp::hd_textures {

namespace {

constexpr const char* kDot = " \xC2\xB7 ";
constexpr const char* kModels[] = {"anime", "general", "drawn"};
constexpr int kModelCount = 3;
constexpr double kWorstError = 30.0;
constexpr double kMostWrongPixels = 0.02;
constexpr int kWrongPixel = 64;
constexpr double kMostFarPixels = 0.1;

std::mutex g_mutex;
Progress g_progress;
int64_t g_phase_started = 0;
uint32_t g_phase_done = 0;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string fill(const char* pattern, uint32_t first, uint32_t second = 0, uint32_t third = 0) {
    std::string text = pattern;
    const uint32_t values[] = {first, second, third};
    for (int i = 0; i < 3; i++) {
        std::string mark = "{" + std::to_string(i + 1) + "}";
        for (size_t at = text.find(mark); at != std::string::npos; at = text.find(mark, at)) {
            text.replace(at, mark.size(), std::to_string(values[i]));
        }
    }
    return text;
}

std::string time_left(const Progress& job) {
    if (job.seconds_left == 0) {
        return "";
    }
    if (job.seconds_left < 60) {
        return kDot + std::string(ui::label("hd.soon"));
    }
    return kDot + fill(ui::label("hd.minutes"), (job.seconds_left + 59) / 60);
}

Progress snapshot_locked() {
    Progress job = g_progress;
    job.seconds_left = 0;
    bool counting = job.state == State::Enlarging || job.state == State::Retrying || job.state == State::Checking;
    uint32_t moved = job.done > g_phase_done ? job.done - g_phase_done : 0;
    int64_t elapsed = now_ms() - g_phase_started;
    if (counting && job.total > job.done && moved >= 3 && elapsed >= 5000) {
        double per_item = static_cast<double>(elapsed) / moved;
        job.seconds_left = static_cast<uint32_t>(per_item * (job.total - job.done) / 1000.0) + 1;
    }
    return job;
}

void begin_phase_locked(State state, uint32_t done, uint32_t total) {
    g_progress.state = state;
    g_progress.done = done;
    g_progress.total = total;
    g_phase_started = now_ms();
    g_phase_done = done;
}

void show_popup() {
    Progress job = progress();
    double fraction = 0.0;
    if (job.state == State::Done) {
        fraction = 1.0;
    } else if ((job.state == State::Enlarging || job.state == State::Retrying || job.state == State::Checking) && job.total) {
        fraction = static_cast<double>(job.done) / job.total;
    }
    hint::note(describe(job), fraction);
}

bool number_then(const std::string& line, const char* rest, uint32_t& value) {
    char* end = nullptr;
    unsigned long parsed = std::strtoul(line.c_str(), &end, 10);
    if (end == line.c_str() || std::string(end).rfind(rest, 0) != 0) {
        return false;
    }
    value = static_cast<uint32_t>(parsed);
    return true;
}

void seen(const std::string& line) {
    log::write("hd", "%s", line.c_str());
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        unsigned done = 0;
        unsigned total = 0;
        uint32_t count = 0;
        if (std::sscanf(line.c_str(), "Again %u/%u", &done, &total) == 2) {
            if (g_progress.state != State::Retrying) {
                begin_phase_locked(State::Retrying, done, total);
            }
            g_progress.done = done;
            g_progress.total = total;
        } else if (line.rfind("Still empty:", 0) == 0) {
            g_progress.still_empty++;
        } else if (number_then(line, " textures came out empty", count)) {
            g_progress.again = count;
            begin_phase_locked(State::Retrying, 0, count);
        } else if (number_then(line, " gray textures keep", count)) {
            g_progress.kept = count;
        } else if (number_then(line, " larger textures written", count)) {
            g_progress.made = count;
        } else if (std::sscanf(line.c_str(), "%u/%u", &done, &total) == 2) {
            if (g_progress.state != State::Enlarging || g_progress.total != total) {
                begin_phase_locked(State::Enlarging, done, total);
            }
            g_progress.done = done;
        } else if (line.rfind("Downloading", 0) == 0) {
            g_progress.state = State::Downloading;
        } else if (line.rfind("Enlarging", 0) == 0) {
            begin_phase_locked(State::Enlarging, 0, 0);
        } else if (line.rfind("Turn on", 0) != 0 && line.rfind("tex1_", 0) != 0 && line.find(" left out") == std::string::npos) {
            g_progress.error = line;
        }
        g_progress.version++;
    }
    show_popup();
}

uint64_t finish(bool made, const char* error) {
    uint64_t version = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.state = made ? State::Done : State::Failed;
        if (error) {
            g_progress.error = error;
        }
        version = ++g_progress.version;
    }
    show_popup();
    return version;
}

#ifdef _WIN32
constexpr const char* kTool = "tools/hd_textures.py";

std::wstring python(bool& launcher) {
    std::error_code error;
    std::filesystem::path bundled = std::filesystem::absolute("python/python.exe", error);
    launcher = false;
    if (std::filesystem::is_regular_file(bundled, error)) {
        return bundled.wstring();
    }
    wchar_t found[MAX_PATH];
    for (const wchar_t* name : {L"py.exe", L"python.exe"}) {
        DWORD length = SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr);
        if (length > 0 && length < MAX_PATH && std::wstring(found).find(L"\\WindowsApps\\") == std::wstring::npos) {
            launcher = name[1] == L'y';
            return found;
        }
    }
    return L"";
}

const char* run_tool(const std::filesystem::path& source, const char* model, bool& made) {
    made = false;
    bool launcher = false;
    std::wstring program = python(launcher);
    std::error_code error;
    if (program.empty()) {
        log::write("hd", "Python was not found");
        return "Python was not found";
    }
    if (!std::filesystem::is_regular_file(kTool, error)) {
        log::write("hd", "%s is missing", kTool);
        return "tools/hd_textures.py is missing";
    }
    std::wstring command = L"\"" + program + L"\"" + (launcher ? L" -3" : L"") + L" -I -X utf8 " + std::filesystem::path(kTool).wstring() + L" --source \"" +
                           std::filesystem::absolute(source, error).wstring() + L"\" --model " + std::wstring(model, model + std::strlen(model));
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE reader = nullptr;
    HANDLE writer = nullptr;
    if (!CreatePipe(&reader, &writer, &inherit, 0)) {
        return "Python could not start";
    }
    SetHandleInformation(reader, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writer;
    startup.hStdError = writer;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) {
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    PROCESS_INFORMATION process{};
    bool started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
                                  nullptr, &startup, &process);
    CloseHandle(writer);
    if (!started) {
        log::write("hd", "Python could not start (error %lu)", static_cast<unsigned long>(GetLastError()));
        CloseHandle(reader);
        if (job) {
            CloseHandle(job);
        }
        return "Python could not start";
    }
    if (job) {
        AssignProcessToJobObject(job, process.hProcess);
    }
    ResumeThread(process.hThread);
    std::string line;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(reader, buffer, sizeof buffer, &read, nullptr) && read > 0) {
        for (DWORD i = 0; i < read; i++) {
            if (buffer[i] != '\n' && buffer[i] != '\r') {
                line.push_back(buffer[i]);
            } else if (!line.empty()) {
                seen(line);
                line.clear();
            }
        }
    }
    if (!line.empty()) {
        seen(line);
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(reader);
    if (job) {
        CloseHandle(job);
    }
    if (code != 0) {
        log::write("hd", "%s ended with code %lu", kTool, static_cast<unsigned long>(code));
    }
    made = code == 0;
    return nullptr;
}

uint32_t check(const std::filesystem::path& source, const std::filesystem::path& target) {
    std::vector<std::filesystem::path> pairs;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(source, error)) {
        if (entry.path().extension() == ".png" && std::filesystem::is_regular_file(target / entry.path().filename(), error)) {
            pairs.push_back(entry.path().filename());
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        begin_phase_locked(State::Checking, 0, static_cast<uint32_t>(pairs.size()));
        g_progress.version++;
    }
    show_popup();
    std::atomic<size_t> next{0};
    std::atomic<uint32_t> finished{0};
    std::atomic<uint32_t> wrong{0};
    std::atomic<uint32_t> evened{0};
    auto work = [&] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (size_t i = next++; i < pairs.size(); i = next++) {
            std::vector<uint32_t> original;
            std::vector<uint32_t> larger;
            uint32_t width = 0;
            uint32_t height = 0;
            uint32_t larger_width = 0;
            uint32_t larger_height = 0;
            std::error_code removed;
            if (gx::custom_textures::decode_png(source / pairs[i], original, width, height)) {
                Range range;
                bool read = gx::custom_textures::decode_png(target / pairs[i], larger, larger_width, larger_height);
                if (read && matches(original, width, height, larger, larger_width, larger_height)) {
                    range = keep_in_range(original, width, height, larger, larger_width, larger_height);
                }
                if (!read || !range.matched || range.strayed > kMostFarPixels) {
                    log::write("hd", "Wrong, made again: %s", pairs[i].string().c_str());
                    std::filesystem::remove(target / pairs[i], removed);
                    wrong++;
                } else if (range.changed) {
                    if (gx::custom_textures::write_png(target / pairs[i], larger, larger_width, larger_height)) {
                        evened++;
                    } else {
                        log::write("hd", "Cannot write %s", pairs[i].string().c_str());
                    }
                }
            }
            finished++;
        }
    };
    unsigned helpers = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    std::vector<std::thread> threads;
    for (unsigned k = 0; k < helpers; k++) {
        threads.emplace_back(work);
    }
    while (finished < pairs.size()) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progress.done = finished;
            g_progress.wrong = wrong;
            g_progress.version++;
        }
        show_popup();
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.done = finished;
        g_progress.wrong = wrong;
        g_progress.version++;
    }
    show_popup();
    log::write("hd", "%u of %u textures checked were wrong, %u kept within their original's colors", wrong.load(), static_cast<unsigned>(pairs.size()),
               evened.load());
    return wrong;
}

void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    show_popup();
    const game::Description& game = game::description();
    std::filesystem::path source = std::filesystem::path(game.textures_directory) / "disc";
    std::filesystem::path target = std::filesystem::path(game.textures_directory) / "load" / "hd";
    size_t count = gx::disc_textures::export_flat(std::filesystem::path(game.data_directory) / "files", source);
    log::write("hd", "%u textures without mipmaps found on the disc", static_cast<unsigned>(count));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.found = static_cast<uint32_t>(count);
        begin_phase_locked(State::Enlarging, 0, 0);
        g_progress.version++;
    }
    show_popup();
    std::filesystem::path kept_list = target / "kept.txt";
    std::set<std::string> kept_names;
    uint32_t skipped = 0;
    {
        std::ifstream listed(kept_list);
        std::error_code skip;
        for (std::string name; std::getline(listed, name);) {
            if (!name.empty() && name.back() == '\r') {
                name.pop_back();
            }
            if (!name.empty() && kept_names.insert(name).second && std::filesystem::remove(source / name, skip)) {
                skipped++;
            }
        }
    }
    log::write("hd", "%u textures left out, kept as they are by an earlier run", skipped);
    bool made = false;
    const char* error = count > 0 ? nullptr : "No texture could be read from the disc";
    uint32_t wrong_total = 0;
    uint32_t wrong = 0;
    for (int round = 0; !error && round < kModelCount; round++) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progress.kept = 0;
            g_progress.still_empty = 0;
        }
        bool round_made = false;
        error = run_tool(source, kModels[round], round_made);
        uint32_t left = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progress.made = 0;
            left = g_progress.kept + g_progress.still_empty;
        }
        made = made || round_made;
        if (error || (!round_made && round == 0)) {
            break;
        }
        wrong = check(source, target);
        wrong_total += wrong;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_progress.wrong = wrong_total;
        }
        if (wrong == 0 && left == 0) {
            break;
        }
    }
    uint32_t ready = 0;
    uint32_t left_out = 0;
    std::error_code listed;
    for (const auto& entry : std::filesystem::directory_iterator(source, listed)) {
        if (std::filesystem::is_regular_file(target / entry.path().filename(), listed)) {
            ready++;
        } else if (entry.path().extension() == ".png") {
            left_out++;
            kept_names.insert(entry.path().filename().string());
        }
    }
    if (!error && left_out > 0) {
        std::ofstream list(kept_list, std::ios::trunc);
        for (const std::string& name : kept_names) {
            list << name << '\n';
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_progress.made = ready;
        g_progress.kept = skipped + (left_out > g_progress.still_empty ? left_out - g_progress.still_empty : 0);
        g_progress.wrong = wrong_total > wrong ? wrong_total - wrong : 0;
    }
    std::error_code removed;
    std::filesystem::remove_all(source, removed);
    uint64_t version = finish(made, error);
    std::this_thread::sleep_for(std::chrono::seconds(30));
    bool unchanged = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        unchanged = g_progress.version == version;
    }
    if (unchanged) {
        hint::clear_note();
    }
}
#endif

}

bool available() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

void start() {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        State now = g_progress.state;
        if (!available() || now == State::Reading || now == State::Downloading || now == State::Enlarging || now == State::Retrying || now == State::Checking) {
            return;
        }
        uint64_t previous = g_progress.version;
        g_progress = Progress{};
        g_progress.state = State::Reading;
        g_progress.version = previous + 1;
    }
#ifdef _WIN32
    std::thread(run).detach();
#endif
}

bool matches(const std::vector<uint32_t>& original, uint32_t width, uint32_t height, const std::vector<uint32_t>& larger, uint32_t larger_width,
             uint32_t larger_height) {
    if (width == 0 || height == 0 || original.size() != static_cast<size_t>(width) * height || larger.size() != static_cast<size_t>(larger_width) * larger_height ||
        larger_width % width != 0 || larger_height % height != 0 || larger_width / width != larger_height / height) {
        return false;
    }
    uint32_t scale = larger_width / width;
    uint32_t area = scale * scale;
    double color_error = 0.0;
    double weight = 0.0;
    double alpha_error = 0.0;
    size_t wrong_pixels = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t sum[4] = {};
            for (uint32_t dy = 0; dy < scale; dy++) {
                const uint32_t* row = larger.data() + static_cast<size_t>(y * scale + dy) * larger_width + x * scale;
                for (uint32_t dx = 0; dx < scale; dx++) {
                    for (int channel = 0; channel < 4; channel++) {
                        sum[channel] += row[dx] >> (channel * 8) & 255;
                    }
                }
            }
            uint32_t pixel = original[static_cast<size_t>(y) * width + x];
            double alpha = pixel >> 24;
            double larger_alpha = static_cast<double>(sum[3]) / area;
            double worst = std::abs(alpha - larger_alpha);
            for (int channel = 0; channel < 3; channel++) {
                double value = pixel >> (channel * 8) & 255;
                double larger_value = static_cast<double>(sum[channel]) / area;
                color_error += std::abs(value - larger_value) * alpha / 255.0;
                worst = std::max(worst, std::abs(value * alpha - larger_value * larger_alpha) / 255.0);
            }
            weight += alpha / 255.0;
            alpha_error += std::abs(alpha - larger_alpha);
            if (worst > kWrongPixel) {
                wrong_pixels++;
            }
        }
    }
    double pixels = static_cast<double>(width) * height;
    double error = std::max(weight > 0.0 ? color_error / (weight * 3.0) : 0.0, alpha_error / pixels);
    return error <= kWorstError && static_cast<double>(wrong_pixels) / pixels <= kMostWrongPixels;
}

Range keep_in_range(const std::vector<uint32_t>& original, uint32_t width, uint32_t height, std::vector<uint32_t>& larger, uint32_t larger_width,
                    uint32_t larger_height) {
    if (width == 0 || height == 0 || original.size() != static_cast<size_t>(width) * height || larger.size() != static_cast<size_t>(larger_width) * larger_height ||
        larger_width % width != 0 || larger_height % height != 0 || larger_width / width != larger_height / height) {
        return {};
    }
    uint32_t scale = larger_width / width;
    std::vector<uint32_t> lowest(original.size());
    std::vector<uint32_t> highest(original.size());
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t low = 0;
            uint32_t high = 0;
            for (int channel = 0; channel < 4; channel++) {
                uint32_t least = 255;
                uint32_t most = 0;
                for (uint32_t ny = y > 0 ? y - 1 : 0; ny <= std::min(y + 1, height - 1); ny++) {
                    for (uint32_t nx = x > 0 ? x - 1 : 0; nx <= std::min(x + 1, width - 1); nx++) {
                        uint32_t value = original[static_cast<size_t>(ny) * width + nx] >> (channel * 8) & 255;
                        least = std::min(least, value);
                        most = std::max(most, value);
                    }
                }
                low |= least << (channel * 8);
                high |= most << (channel * 8);
            }
            lowest[static_cast<size_t>(y) * width + x] = low;
            highest[static_cast<size_t>(y) * width + x] = high;
        }
    }
    Range range;
    range.matched = true;
    size_t strayed = 0;
    for (uint32_t y = 0; y < larger_height; y++) {
        for (uint32_t x = 0; x < larger_width; x++) {
            size_t block = static_cast<size_t>(y / scale) * width + x / scale;
            uint32_t& pixel = larger[static_cast<size_t>(y) * larger_width + x];
            uint32_t kept = 0;
            uint32_t moved = 0;
            for (int channel = 3; channel >= 0; channel--) {
                uint32_t shift = channel * 8;
                uint32_t value = pixel >> shift & 255;
                uint32_t limited = std::clamp(value, lowest[block] >> shift & 255, highest[block] >> shift & 255);
                uint32_t step = value > limited ? value - limited : limited - value;
                moved = std::max(moved, channel == 3 ? step : step * (kept >> 24) / 255);
                kept |= limited << shift;
            }
            if (moved > kWrongPixel) {
                strayed++;
            }
            if (kept != pixel) {
                pixel = kept;
                range.changed = true;
            }
        }
    }
    range.strayed = static_cast<double>(strayed) / larger.size();
    return range;
}

Progress progress() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return snapshot_locked();
}

std::vector<std::string> describe(const Progress& job) {
    std::string title = std::string(ui::label("hd.title")) + kDot;
    std::vector<std::string> lines;
    switch (job.state) {
    case State::Reading:
        lines.push_back(title + ui::label("hd.reading"));
        break;
    case State::Downloading:
        lines.push_back(title + ui::label("hd.downloading"));
        break;
    case State::Enlarging:
        lines.push_back(title + ui::label("hd.enlarging"));
        if (job.found) {
            lines.push_back(fill(ui::label("hd.found"), job.found));
        }
        if (job.total) {
            lines.push_back(fill(ui::label("hd.count"), job.done, job.total) + time_left(job));
        }
        break;
    case State::Retrying:
        lines.push_back(title + ui::label("hd.retrying"));
        lines.push_back(fill(ui::label("hd.empty"), job.again));
        lines.push_back(fill(ui::label("hd.redone"), job.done > job.still_empty ? job.done - job.still_empty : 0, job.total) +
                        (job.still_empty ? kDot + fill(ui::label("hd.still"), job.still_empty) : std::string()) + time_left(job));
        break;
    case State::Checking:
        lines.push_back(title + ui::label("hd.checking"));
        lines.push_back(fill(ui::label("hd.checked"), job.done, job.total) + (job.wrong ? kDot + fill(ui::label("hd.wrong"), job.wrong) : std::string()) +
                        time_left(job));
        break;
    case State::Done: {
        lines.push_back(title + ui::label("hd.done"));
        std::string made = fill(ui::label("hd.made"), job.made);
        if (job.again > job.still_empty || job.wrong) {
            made += kDot + fill(ui::label("hd.second"), (job.again > job.still_empty ? job.again - job.still_empty : 0) + job.wrong);
        }
        lines.push_back(made);
        if (job.kept) {
            lines.push_back(fill(ui::label("hd.kept"), job.kept));
        }
        if (job.still_empty) {
            lines.push_back(fill(ui::label("hd.failed_some"), job.still_empty));
        }
        break;
    }
    case State::Failed:
        lines.push_back(title + ui::label("hd.failed"));
        if (!job.error.empty()) {
            lines.push_back(job.error);
        }
        lines.push_back(ui::label("hd.details"));
        break;
    default:
        break;
    }
    return lines;
}

}
