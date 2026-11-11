#include "wp/perf.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "wp/settings.h"

namespace wp::perf {

namespace {

std::mutex g_sample_mutex;
Sample g_sample;
std::atomic<uint64_t> g_revision{0};

std::mutex g_figures_mutex;
SystemFigures g_figures;

void sample_system() {
    static const settings::LiveFlag shown("overlay.show", nullptr);
    bool primed = false;
    for (;;) {
        if (!shown()) {
            primed = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            continue;
        }
        if (!primed) {
            system_cpu_percent();
            gpu_engine_percent();
            primed = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        SystemFigures figures;
        figures.cpu_percent = system_cpu_percent();
        figures.gpu_percent = gpu_engine_percent();
        figures.gpu_process_percent = gpu_process_percent();
        memory(figures.ram_used_gb, figures.ram_total_gb);
        {
            std::lock_guard<std::mutex> lock(g_figures_mutex);
            g_figures = figures;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}

void publish(const Sample& sample) {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    g_sample = sample;
    g_revision++;
}

Sample latest() {
    std::lock_guard<std::mutex> lock(g_sample_mutex);
    return g_sample;
}

uint64_t revision() {
    return g_revision.load();
}

SystemFigures system_figures() {
    static std::once_flag started;
    std::call_once(started, [] { std::thread(sample_system).detach(); });
    std::lock_guard<std::mutex> lock(g_figures_mutex);
    return g_figures;
}

}
