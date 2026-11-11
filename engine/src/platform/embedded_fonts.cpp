#include "wp/canvas.h"

#include <cstring>

#if defined(__APPLE__)
#define WP_FONT_SECTION ".const_data"
#define WP_FONT_PREFIX "_"
#elif defined(_WIN32)
#define WP_FONT_SECTION ".section .rdata,\"dr\""
#define WP_FONT_PREFIX ""
#else
#define WP_FONT_SECTION ".section .rodata"
#define WP_FONT_PREFIX ""
#endif

#define WP_FONT_BLOB(name, file)                                                                                                                     \
    WP_FONT_SECTION "\n.balign 16\n.globl " WP_FONT_PREFIX "wp_font_" name "\n" WP_FONT_PREFIX "wp_font_" name ":\n.incbin \"" WP_FONT_DIR "/" file \
                    "\"\n.globl " WP_FONT_PREFIX "wp_font_" name "_end\n" WP_FONT_PREFIX "wp_font_" name "_end:\n.byte 0\n"

asm(WP_FONT_BLOB("nunito_extrabold", "Nunito-ExtraBold.ttf") WP_FONT_BLOB("nunito_black", "Nunito-Black.ttf") ".text\n");

extern "C" const unsigned char wp_font_nunito_extrabold[];
extern "C" const unsigned char wp_font_nunito_extrabold_end[];
extern "C" const unsigned char wp_font_nunito_black[];
extern "C" const unsigned char wp_font_nunito_black_end[];

namespace wp::canvas {

namespace {

struct Entry {
    const char* name;
    const unsigned char* begin;
    const unsigned char* end;
};

const Entry kEntries[] = {
    {"nunito_extrabold", wp_font_nunito_extrabold, wp_font_nunito_extrabold_end},
    {"nunito_black", wp_font_nunito_black, wp_font_nunito_black_end},
};

}

std::string_view embedded_font(const char* name) {
    for (const Entry& entry : kEntries) {
        if (std::strcmp(entry.name, name) == 0) {
            return {reinterpret_cast<const char*>(entry.begin), static_cast<size_t>(entry.end - entry.begin)};
        }
    }
    return {};
}

}
