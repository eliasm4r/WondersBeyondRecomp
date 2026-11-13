#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

#include "wp/discord.h"
#include "wp/big_stack.h"
#include "wp/boot.h"
#include "wp/cpu.h"
#include "wp/exi.h"
#include "wp/disc.h"
#include "wp/dol.h"
#include "wp/game.h"
#include "wp/host_stacks.h"
#include "wp/gx.h"
#include "wp/input.h"
#include "wp/log.h"
#include "wp/nand.h"
#include "wp/options_window.h"
#include "wp/perf.h"
#include "wp/ipc.h"
#include "wp/settings.h"
#include "wp/stall_watch.h"
#include "wp/threads.h"
#include "wp/video.h"
#include "wp/watch.h"
#include "wp/play_time.h"
#include "wp/runtime.h"

#ifdef _WIN32
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace {

#ifdef _WIN32
// Native frames of the crashing thread as module+offset, which addr2line or the linker map turn into functions.
void print_host_stack(const CONTEXT& crashed) {
    CONTEXT context = crashed;
    for (int depth = 0; depth < 32 && context.Rip != 0; depth++) {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(context.Rip), &module)) {
            GetModuleFileNameA(module, name, sizeof name);
        }
        const char* file = std::strrchr(name, 92);
        unsigned long long offset = context.Rip - reinterpret_cast<uintptr_t>(module);
        std::fprintf(stderr, "host frame %d: %s+%llx", depth, file ? file + 1 : name, offset);
        std::fputc(10, stderr);
        wp::log::write("crash", "host frame %d: %s+%llx", depth, file ? file + 1 : name, offset);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
        if (!function) {
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handler_data = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context, &handler_data, &frame, nullptr);
    }
}

LONG WINAPI report_crash(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    wp::log::write("crash", "exception %08lx at host address %p (module offset %llx)", record->ExceptionCode, record->ExceptionAddress,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr))));
    std::fprintf(stderr, "crash: exception %08lx at host address %p (module offset %llx)", record->ExceptionCode, record->ExceptionAddress,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record->ExceptionAddress) - reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr))));
    if (record->NumberParameters >= 2 && (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)) {
        std::fprintf(stderr, ", %s address %p (guest memory offset %llx)", record->ExceptionInformation[0] ? "writing" : "reading",
                     reinterpret_cast<void*>(record->ExceptionInformation[1]),
                     static_cast<unsigned long long>(record->ExceptionInformation[1] - reinterpret_cast<uintptr_t>(wp::g_memory)));
    }
    std::fputc(10, stderr);
    print_host_stack(*info->ContextRecord);
    wp::print_guest_registers();
    wp::print_call_stack();
    wp::print_thread_stacks();
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

void report_crash(int signal) {
    std::fprintf(stderr, "crash: signal %d", signal);
    std::fputc(10, stderr);
    wp::log::write("crash", "signal %d", signal);
    wp::print_guest_registers();
    wp::print_call_stack();
    wp::print_thread_stacks();
    std::fflush(stderr);
    std::_Exit(128 + signal);
}

void report_terminate() {
    try {
        std::exception_ptr pending = std::current_exception();
        if (pending) {
            std::rethrow_exception(pending);
        }
        std::fputs("crash: terminate called without an exception", stderr);
        wp::log::write("crash", "terminate called without an exception");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "crash: unhandled exception: %s", error.what());
        wp::log::write("crash", "unhandled exception: %s", error.what());
    } catch (...) {
        std::fputs("crash: unhandled exception of an unknown type", stderr);
        wp::log::write("crash", "unhandled exception of an unknown type");
    }
    std::fputc(10, stderr);
    wp::print_guest_registers();
    wp::print_call_stack();
    wp::print_thread_stacks();
    std::fflush(stderr);
    std::_Exit(134);
}

void move_old_game_folder() {
    namespace fs = std::filesystem;
    const wp::game::Description& game = wp::game::description();
    fs::path old_folder = fs::path("games") / game.short_name;
    fs::path folder = fs::path(game.settings_file).parent_path();
    std::error_code error;
    if (folder.empty() || !fs::is_directory(old_folder, error)) {
        return;
    }
    fs::create_directories(folder, error);
    for (const char* name : {"disc", "extracted", "nand", "backups", "textures", "settings.ini", "stats.ini"}) {
        if (fs::exists(old_folder / name, error) && !fs::exists(folder / name, error)) {
            fs::rename(old_folder / name, folder / name, error);
        }
    }
}

void use_default_log() {
    if (std::getenv("WP_LOG_FILE") || std::getenv("WP_HEADLESS")) {
        return;
    }
    std::error_code error;
    std::filesystem::create_directories("logs", error);
    std::filesystem::rename("logs/pokepark2.log", "logs/pokepark2.previous.log", error);
#ifdef _WIN32
    _putenv_s("WP_LOG_FILE", "logs/pokepark2.log");
#else
    setenv("WP_LOG_FILE", "logs/pokepark2.log", 1);
#endif
}

void log_machine() {
#ifdef _WIN32
    SYSTEM_POWER_STATUS power{};
    GetSystemPowerStatus(&power);
    wp::log::write("boot", "%s, %u logical processors, %s", wp::perf::cpu_name().c_str(), std::thread::hardware_concurrency(),
                   power.ACLineStatus == 1 ? "on mains power" : power.ACLineStatus == 0 ? "on battery" : "power source unknown");
#endif
}

}

int run(int argc, char** argv) {
    wp::settings::load(wp::game::description().settings_file);
    if (!std::getenv("WP_HEADLESS")) {
        wp::play_time::start(wp::play_time::stats_path(wp::game::description().settings_file));
    }
    wp::input::upgrade_bindings();
    std::string extracted = argc > 1 ? argv[1] : wp::game::description().data_directory;
    int timeout_seconds = argc > 2 ? std::atoi(argv[2]) : 0;

    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));
    if (!wp::g_memory) {
        std::fputs("cannot allocate guest memory\n", stderr);
        return 1;
    }
    uint32_t entry = 0;
    if (!wp::load_dol(extracted + "/sys/main.dol", entry)) {
        std::fprintf(stderr, "cannot load %s/sys/main.dol\n", extracted.c_str());
        return 1;
    }
    if (!wp::init_boot_memory(extracted)) {
        std::fprintf(stderr, "cannot read boot files in %s/sys\n", extracted.c_str());
        return 1;
    }

    std::string nand_root = argc > 3 ? argv[3] : wp::game::description().nand_directory;
    if (!wp::nand::mount(nand_root)) {
        std::fprintf(stderr, "cannot prepare the virtual NAND in %s\n", nand_root.c_str());
        return 1;
    }
    wp::exi::mount((std::filesystem::path(nand_root).lexically_normal().parent_path() / "sram.bin").string());
    if (!wp::disc::mount(extracted)) {
        std::fprintf(stderr, "cannot read the disc layout in %s\n", extracted.c_str());
        return 1;
    }

    if (const char* watch = std::getenv("WP_WATCH")) {
        wp::start_watch(static_cast<uint32_t>(std::strtoul(watch, nullptr, 16)));
    }
    if (std::getenv("WP_PROFILE")) {
        wp::start_profiler();
    }
    if (timeout_seconds > 0) {
        std::thread([timeout_seconds] {
            std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));
            std::fprintf(stderr, "no progress after %d seconds\n", timeout_seconds);
            wp::print_call_stack();
            wp::print_thread_stacks();
            wp::print_guest_threads();
            wp::ipc::report();
            wp::print_profile();
            wp::play_time::save();
            std::_Exit(3);
        }).detach();
    }

    wp::video::start();
    std::thread([] {
        constexpr auto kStall = std::chrono::seconds(15);
        wp::StallWatch watch(kStall);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (watch.update(wp::gx::frames_drawn(), wp::options::menu_open(), std::chrono::steady_clock::now())) {
                wp::log::write("hang", "the game has drawn no frame for %d seconds", static_cast<int>(kStall.count()));
                wp::print_guest_threads();
                wp::ipc::report();
                wp::host_stacks::print();
            }
        }
    }).detach();
    if (!std::getenv("WP_HEADLESS")) {
        wp::discord::start();
    }
    wp::Cpu cpu{};
    wp::init_threads(cpu);
    wp::call(cpu, entry);
    std::puts("entry returned");
    return 0;
}

int main(int argc, char** argv) {
    std::set_terminate(report_terminate);
    if (const char* root = std::getenv("WP_ROOT")) {
        std::error_code error;
        std::filesystem::current_path(root, error);
    }
    move_old_game_folder();
    use_default_log();
    if (const char* log_path = std::getenv("WP_LOG_FILE")) {
        wp::log::write("boot", "log started");
        std::freopen(log_path, "a", stderr);
        log_machine();
    }
    // Redirected stderr is fully buffered; a crash would lose the end of its report.
    std::setvbuf(stderr, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(report_crash);
    timeBeginPeriod(1);
    std::signal(SIGABRT, report_crash);
#ifdef WP_OPENGL
    wp::start_with_stack(size_t{512} << 20, [argc, argv] { std::exit(run(argc, argv)); });
    wp::video::serve_main_thread();
    return 0;
#else
    return run(argc, argv);
#endif
#else
    std::signal(SIGSEGV, report_crash);
    std::signal(SIGBUS, report_crash);
    std::signal(SIGILL, report_crash);
    std::signal(SIGFPE, report_crash);
    std::signal(SIGABRT, report_crash);
    wp::start_with_stack(size_t{512} << 20, [argc, argv] {
        wp::host_stacks::watch_current_thread("game");
        std::exit(run(argc, argv));
    });
    wp::video::serve_main_thread();
    return 0;
#endif
}
