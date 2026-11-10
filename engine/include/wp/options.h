#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wp/keymap.h"
#include "wp/ui_text.h"

namespace wp::options {

enum class Action { Up, Down, Previous, Next, Accept, Close, TabPrevious, TabNext };

enum class Part { None, Label, Previous, Value, Next, Close, Tab };

enum class Kind { Choice, Toggle, Slider, Key, Pad, Button };

struct Hover {
    int row = -1;
    Part part = Part::None;
    int tab = -1;

    bool operator==(const Hover& other) const { return row == other.row && part == other.part && tab == other.tab; }
    bool operator!=(const Hover& other) const { return !(*this == other); }
};

struct Row {
    std::string key;
    std::string label;
    std::string value;
    Kind kind = Kind::Choice;
    bool live = false;
    bool on = false;
    double fraction = 0.0;
};

constexpr size_t kTabCount = 5;
constexpr size_t kVisibleRows = 9;

const char* tab_label(size_t tab);
std::vector<std::string> tab_keys(size_t tab);
Kind kind(const std::string& key);
bool live(const std::string& key);
std::vector<std::string> choices(const std::string& key);
std::string shown_value(const std::string& key, const std::string& value);
std::string step_value(const std::string& key, const std::string& value, int step);

class Menu {
public:
    bool open() const { return open_; }
    void set_open(bool open);
    size_t tab() const { return tab_; }
    void select_tab(size_t tab);
    size_t selection() const { return selection_; }
    void select(size_t index);
    size_t scroll() const { return scroll_; }
    void scroll_by(int rows);
    std::vector<Row> rows() const;
    std::string handle(Action action);
    // Moves the slider of this row to a position between 0 and 1.
    std::string set_fraction(size_t row, double fraction);
    // A key or gamepad row waiting for the input to bind.
    bool capturing() const { return capturing_; }
    Kind capture_kind() const;
    void cancel_capture() { capturing_ = false; }
    std::string capture(const std::string& name);
    const Hover& hover() const { return hover_; }
    bool set_hover(const Hover& hover);

private:
    void keep_visible();

    bool open_ = false;
    bool capturing_ = false;
    size_t tab_ = 0;
    size_t selection_ = 0;
    size_t scroll_ = 0;
    Hover hover_;
};

class Repeat {
public:
    Repeat(uint32_t repeating, int64_t delay, int64_t interval) : repeating_(repeating), delay_(delay), interval_(interval) {}
    uint32_t update(uint32_t held, int64_t now);

private:
    uint32_t repeating_;
    int64_t delay_;
    int64_t interval_;
    uint32_t previous_ = 0;
    int64_t next_ = 0;
};

struct Layout {
    int unit = 0;
    int width = 0;
    int height = 0;
    int padding = 0;
    int title_height = 0;
    int tab_height = 0;
    int row_height = 0;
    int footer_height = 0;
    size_t total = 0;
    size_t scroll = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

Layout layout(int client_height, size_t total_rows, size_t scroll);
Rect place(int client_width, int client_height, int width, int height);
Rect tab_rect(const Layout& layout, size_t tab);
Rect rows_rect(const Layout& layout);
Rect row_rect(const Layout& layout, size_t visible_row);
Rect control_rect(const Layout& layout, size_t visible_row);
Rect close_rect(const Layout& layout);
Hover hit_test(const Layout& layout, const Rect& placed, int x, int y);
double slider_fraction(const Layout& layout, const Rect& placed, size_t visible_row, int x);

struct Image {
    std::vector<uint32_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
};

void show_overlay(Image image);
void hide_overlay();
bool take_overlay(uint64_t& version, Image& image, bool& visible);

}
