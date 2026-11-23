add_library(wp_engine STATIC
    src/core/runtime.cpp
    src/core/fast_clock.cpp
    src/core/fpu.cpp
    src/core/fiber.cpp
    src/core/big_stack.cpp
    src/core/dol.cpp
    src/core/boot.cpp
    src/core/frame_pacing.cpp
    src/core/hle.cpp
    src/core/stall_watch.cpp
    src/core/host_stacks.cpp
    src/core/threads.cpp
    src/core/interrupts.cpp
    src/core/modules.cpp
    src/core/format.cpp
    src/core/log.cpp
    src/gpu/gx.cpp
    src/gpu/gx_lighting.cpp
    src/gpu/custom_textures.cpp
    src/gpu/disc_textures.cpp
    src/gpu/gx_state.cpp
    src/gpu/gx_render_common.cpp
    src/audio/dsp.cpp
    src/audio/audio.cpp
    src/audio/hle_audio.cpp
    src/audio/dsp_roms.cpp
    src/ios/ios.cpp
    src/ios/ipc.cpp
    src/ios/nand.cpp
    src/ios/disc.cpp
    src/input/bluetooth.cpp
    src/input/wiimote.cpp
    src/input/attraction.cpp
    src/input/input.cpp
    src/input/keymap.cpp
    src/input/padmap.cpp
    src/input/free_camera.cpp
    src/input/touch.cpp
    src/input/si.cpp
    src/core/exi.cpp
    src/core/hollywood.cpp
    src/core/io.cpp
    src/platform/performance_hints.cpp
    src/platform/png.cpp
    src/platform/online.cpp
    src/platform/presence.cpp
    src/platform/save_backup.cpp
    src/platform/cloud_sync.cpp
    src/platform/save_cloud.cpp
    src/platform/screenshot.cpp
    src/platform/settings.cpp
    src/platform/play_time.cpp
    src/platform/perf.cpp
    src/platform/perf_overlay.cpp
    src/platform/ghosts.cpp
    src/platform/hint.cpp
    src/platform/system_stats.cpp
    src/platform/ui_text.cpp
    src/platform/color.cpp
    src/platform/control_names.cpp
    src/platform/options.cpp
    src/platform/options_window.cpp
    src/platform/discord.cpp
    src/platform/update_check.cpp
    src/platform/hd_textures.cpp
)
if(WIN32)
    target_link_libraries(wp_engine PUBLIC ws2_32)
    target_link_libraries(wp_engine PUBLIC bcrypt)
endif()
option(WP_OPENGL "Draw with OpenGL instead of Direct3D 11 on Windows" OFF)
option(WP_METAL "Draw with Metal instead of OpenGL on macOS" ON)
if(WIN32 AND NOT WP_OPENGL)
    target_sources(wp_engine PRIVATE src/gpu/gx_render.cpp src/platform/video.cpp)
endif()
if(WIN32 AND WP_OPENGL)
    target_compile_definitions(wp_engine PUBLIC WP_OPENGL)
endif()
target_sources(wp_engine PRIVATE src/platform/options_render_stb.cpp src/platform/canvas.cpp src/platform/embedded_fonts.cpp)
set(WP_FONT_DIR "${CMAKE_SOURCE_DIR}/third_party/fonts/nunito")
set_source_files_properties(src/platform/embedded_fonts.cpp PROPERTIES
    COMPILE_DEFINITIONS "WP_FONT_DIR=\"${WP_FONT_DIR}\""
    OBJECT_DEPENDS "${WP_FONT_DIR}/Nunito-ExtraBold.ttf;${WP_FONT_DIR}/Nunito-Black.ttf")
target_include_directories(wp_engine SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/stb")
target_include_directories(wp_engine PUBLIC include)
target_compile_definitions(wp_engine PRIVATE WP_BUILD="${WP_BUILD}" WP_VERSION="${WP_VERSION}"
    WP_DSP_IROM_PATH="${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_rom.bin"
    WP_DSP_COEF_PATH="${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_coef.bin")
set_source_files_properties(src/audio/dsp_roms.cpp PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_rom.bin;${CMAKE_SOURCE_DIR}/third_party/dolphin/Data/Sys/GC/dsp_coef.bin")
target_link_libraries(wp_engine PUBLIC wp_dolphin_dsp)
if(WIN32)
    target_link_libraries(wp_engine PUBLIC gdi32 user32 d3d11 d3dcompiler dxgi ole32 windowscodecs dwmapi winmm pdh advapi32 winhttp shell32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(wp_engine PUBLIC Threads::Threads)
    if(ANDROID)
        target_link_libraries(wp_engine PUBLIC log)
    endif()
endif()
option(WP_ASM_FIBERS "Switch guest threads with the built-in assembly routine instead of ucontext" OFF)
if(WP_ASM_FIBERS)
    target_compile_definitions(wp_engine PRIVATE WP_ASM_FIBERS)
endif()
if(WP_TRACE_CALLS)
    target_compile_definitions(wp_engine PUBLIC WP_TRACE)
endif()
target_compile_options(wp_engine PRIVATE -Wall -Wextra)

if(WIN32)
    set(WP_SDL3_DIR "${CMAKE_SOURCE_DIR}/build/deps/SDL3-3.4.16/x86_64-w64-mingw32" CACHE PATH "SDL3 development files for gamepad support")
    find_package(SDL3 CONFIG QUIET GLOBAL PATHS "${WP_SDL3_DIR}/lib/cmake/SDL3" NO_DEFAULT_PATH)
else()
    find_package(SDL3 3.4 CONFIG QUIET GLOBAL)
    option(WP_FETCH_SDL3 "Download and build SDL3 when the system has none" ON)
    if(NOT SDL3_FOUND AND WP_FETCH_SDL3)
        include(FetchContent)
        set(SDL_SHARED OFF CACHE BOOL "" FORCE)
        set(SDL_STATIC ON CACHE BOOL "" FORCE)
        set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(sdl3
            URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
            URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68
            DOWNLOAD_EXTRACT_TIMESTAMP ON)
        FetchContent_MakeAvailable(sdl3)
        set(SDL3_FOUND TRUE)
        set(WP_SDL_PACKAGES "Debian and Ubuntu: sudo apt install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libegl-dev libgl-dev libpulse-dev libasound2-dev libudev-dev libdbus-1-dev\nFedora: sudo dnf install libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXi-devel libXScrnSaver-devel libXtst-devel libxkbcommon-devel wayland-devel wayland-protocols-devel libdecor-devel mesa-libEGL-devel mesa-libGL-devel pulseaudio-libs-devel alsa-lib-devel systemd-devel dbus-devel\nArch: sudo pacman -S --needed sdl3")
        foreach(WP_SDL_FEATURE X11 WAYLAND PULSEAUDIO PIPEWIRE ALSA)
            get_directory_property(WP_HAVE_${WP_SDL_FEATURE} DIRECTORY "${sdl3_SOURCE_DIR}" DEFINITION HAVE_${WP_SDL_FEATURE})
        endforeach()
        if(NOT APPLE AND NOT ANDROID AND NOT WP_HAVE_X11 AND NOT WP_HAVE_WAYLAND)
            message(FATAL_ERROR "SDL3 found no X11 or Wayland development files, so the game could not open a window. Install them and build again:\n${WP_SDL_PACKAGES}")
        endif()
        if(NOT APPLE AND NOT ANDROID AND NOT WP_HAVE_PULSEAUDIO AND NOT WP_HAVE_PIPEWIRE AND NOT WP_HAVE_ALSA)
            message(FATAL_ERROR "SDL3 found no PulseAudio, PipeWire or ALSA development files, so the game would have no sound. Install them and build again:\n${WP_SDL_PACKAGES}")
        endif()
    endif()
    option(WP_REQUIRE_SDL3 "Stop when SDL3 is missing instead of building the game without a window" OFF)
    if(NOT SDL3_FOUND AND WP_REQUIRE_SDL3)
        message(FATAL_ERROR "SDL3 was not found. Install the SDL3 development package of your distribution, or allow the download of SDL3 (WP_FETCH_SDL3).")
    endif()
endif()
if(SDL3_FOUND)
    target_sources(wp_engine PRIVATE src/input/gamepad.cpp src/input/real_wiimote.cpp)
    if(TARGET SDL3::SDL3)
        target_link_libraries(wp_engine PUBLIC SDL3::SDL3)
    else()
        target_link_libraries(wp_engine PUBLIC SDL3::SDL3-static)
    endif()
else()
    target_sources(wp_engine PRIVATE src/input/no_gamepad.cpp src/input/no_real_wiimote.cpp)
endif()
if(NOT WIN32 OR WP_OPENGL)
    if(SDL3_FOUND AND APPLE AND WP_METAL)
        target_sources(wp_engine PRIVATE src/gpu/gx_render_metal.mm src/platform/video_sdl.cpp src/platform/titlebar_sdl.cpp)
        set_source_files_properties(src/gpu/gx_render_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_compile_definitions(wp_engine PRIVATE WP_METAL)
        target_link_libraries(wp_engine PUBLIC "-framework Metal" "-framework QuartzCore" "-framework Foundation")
        if(NOT WIN32)
            target_compile_definitions(wp_engine PRIVATE WP_SDL_PLATFORM)
        endif()
    elseif(SDL3_FOUND)
        target_sources(wp_engine PRIVATE src/gpu/gx_render_gl.cpp src/platform/video_sdl.cpp src/platform/titlebar_sdl.cpp)
        if(ANDROID)
            find_program(WP_GLSLC glslc HINTS "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}" "${CMAKE_ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}")
        endif()
        if(WP_GLSLC AND (ANDROID OR WP_DESKTOP_VULKAN))
            set(WP_VK_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/vk_shaders")
            set(WP_VK_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/vk_shaders")
            file(MAKE_DIRECTORY "${WP_VK_OUTPUT}")
            set(WP_VK_SHADERS
                "fullscreen.vert|vk_fullscreen_vert|"
                "present.frag|vk_present_frag|"
                "present.frag|vk_present_smooth_frag|-DWP_SMOOTH"
                "overlay.frag|vk_overlay_frag|"
                "clear.frag|vk_clear_frag|"
                "copy.frag|vk_copy_frag|"
                "tev.vert|vk_tev_vert|"
                "run.vert|vk_run_vert|"
                "tev.frag|vk_tev_frag|"
                "tev.frag|vk_tev_early_frag|-DWP_EARLY"
                "tev.frag|vk_tev_single_frag|-DWP_SINGLE_OUTPUT"
                "tev.frag|vk_tev_early_single_frag|-DWP_EARLY|-DWP_SINGLE_OUTPUT")
            set(WP_VK_SIZED
                "tev.vert|vk_tev_vert|kTevVertexSized|"
                "run.vert|vk_run_vert|kRunVertexSized|"
                "tev.frag|vk_tev_frag|kTevSized|"
                "tev.frag|vk_tev_early_frag|kTevEarlySized|-DWP_EARLY"
                "tev.frag|vk_tev_single_frag|kTevSingleSized|-DWP_SINGLE_OUTPUT"
                "tev.frag|vk_tev_early_single_frag|kTevEarlySingleSized|-DWP_EARLY|-DWP_SINGLE_OUTPUT")
            set(WP_VK_SIZED_HEADER "")
            foreach(entry IN LISTS WP_VK_SIZED)
                string(REPLACE "|" ";" parts "${entry}")
                list(GET parts 0 shader_source)
                list(GET parts 1 shader_name)
                list(GET parts 2 array_name)
                list(SUBLIST parts 3 -1 shader_defines)
                list(FILTER shader_defines EXCLUDE REGEX "^$")
                set(sizes "")
                foreach(size RANGE 0 7)
                    set(sized_entry "${shader_source}|${shader_name}_io${size}|-DTEX_IO=${size}")
                    foreach(define IN LISTS shader_defines)
                        string(APPEND sized_entry "|${define}")
                    endforeach()
                    list(APPEND WP_VK_SHADERS "${sized_entry}")
                    string(APPEND WP_VK_SIZED_HEADER "const uint32_t ${array_name}${size}[] =
#include \"${shader_name}_io${size}.inc\"
    ;
")
                    string(APPEND sizes "{${array_name}${size}, sizeof ${array_name}${size}}, ")
                endforeach()
                string(APPEND WP_VK_SIZED_HEADER "const ShaderCode ${array_name}[8] = {${sizes}};
")
            endforeach()
            file(CONFIGURE OUTPUT "${WP_VK_OUTPUT}/vk_sized_shaders.inc" CONTENT "${WP_VK_SIZED_HEADER}" @ONLY)
            set(WP_VK_HEADERS)
            foreach(entry IN LISTS WP_VK_SHADERS)
                string(REPLACE "|" ";" parts "${entry}")
                list(GET parts 0 shader_source)
                list(GET parts 1 shader_name)
                list(LENGTH parts part_count)
                set(shader_defines)
                if(part_count GREATER 2)
                    list(SUBLIST parts 2 -1 shader_defines)
                endif()
                set(shader_header "${WP_VK_OUTPUT}/${shader_name}.inc")
                add_custom_command(OUTPUT "${shader_header}"
                    COMMAND "${WP_GLSLC}" --target-env=vulkan1.0 -O -mfmt=c ${shader_defines} "${WP_VK_SOURCE}/${shader_source}" -o "${shader_header}"
                    DEPENDS "${WP_VK_SOURCE}/${shader_source}"
                    VERBATIM)
                list(APPEND WP_VK_HEADERS "${shader_header}")
            endforeach()
            add_custom_target(wp_vk_shaders DEPENDS ${WP_VK_HEADERS})
            add_dependencies(wp_engine wp_vk_shaders)
            target_sources(wp_engine PRIVATE src/gpu/gx_render_vk.cpp src/gpu/gx_render_select.cpp)
            target_include_directories(wp_engine PRIVATE "${WP_VK_OUTPUT}")
            target_compile_definitions(wp_engine PRIVATE WP_VULKAN)
            if(WP_DESKTOP_VULKAN)
                target_include_directories(wp_engine PRIVATE "${WP_VULKAN_INCLUDE}")
                target_link_libraries(wp_engine PUBLIC "${WP_VULKAN_LIBRARY}")
            else()
                target_link_libraries(wp_engine PUBLIC vulkan)
            endif()
        endif()
        if(NOT WIN32)
            target_compile_definitions(wp_engine PRIVATE WP_SDL_PLATFORM)
        endif()
    else()
        target_sources(wp_engine PRIVATE src/gpu/null_render.cpp src/platform/video_headless.cpp)
    endif()
endif()

add_library(wp_app INTERFACE)
target_sources(wp_app INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp")
if(WIN32)
    target_link_options(wp_app INTERFACE -Wl,--stack,536870912)
endif()
