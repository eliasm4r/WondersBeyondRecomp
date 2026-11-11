#pragma once

#include <cstdint>
#include <string>

namespace wp::perf {

struct Sample {
    double fps = -1;
    double frame_ms = -1;
    double game_fps = -1;
    double late_per_second = -1;
    double cpu_percent = -1;
    double game_thread_percent = -1;
    double gpu_thread_percent = -1;
    double gpu_percent = -1;
    double gpu_ms = -1;
    double vram_used_gb = -1;
    double vram_total_gb = -1;
    double ram_used_gb = -1;
    double ram_total_gb = -1;
    std::string cpu_name;
    std::string gpu_name;
    std::string api;
    int render_width = 0;
    int render_height = 0;
    int scale = 0;
    int refresh_hz = 0;
    bool vsync = false;
};

struct SystemFigures {
    double cpu_percent = -1;
    double gpu_percent = -1;
    double gpu_process_percent = -1;
    double ram_used_gb = -1;
    double ram_total_gb = -1;
};

void publish(const Sample& sample);
Sample latest();
uint64_t revision();

double system_cpu_percent();
double gpu_engine_percent();
double gpu_process_percent();
void memory(double& used_gb, double& total_gb);
std::string cpu_name();
SystemFigures system_figures();

}
