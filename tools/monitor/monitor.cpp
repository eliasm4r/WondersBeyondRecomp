#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace Gdiplus;

constexpr wchar_t kHost[] = L"wondersbeyond-rooms.save-sync.workers.dev";
constexpr wchar_t kPath[] = L"/v1/census";
constexpr UINT kReport = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kRefreshMs = 15000;
constexpr int kMaxPlayers = 4;

const Color kBackground(255, 18, 20, 26);
const Color kCard(255, 30, 33, 42);
const Color kCardEdge(255, 44, 48, 60);
const Color kText(255, 236, 238, 244);
const Color kMuted(255, 146, 152, 168);
const Color kFaint(255, 96, 102, 118);
const Color kGreen(255, 76, 201, 122);
const Color kRed(255, 235, 87, 87);
const Color kAmber(255, 245, 176, 65);
const Color kBlue(255, 92, 156, 255);
const Color kViolet(255, 168, 120, 255);
const Color kRow(255, 24, 27, 35);

struct Json {
    enum class Kind { Null, Number, Text, List, Object } kind = Kind::Null;
    double number = 0;
    std::string text;
    std::vector<Json> list;
    std::vector<std::pair<std::string, Json>> object;

    const Json* get(const char* key) const {
        for (const auto& [name, value] : object) {
            if (name == key) {
                return &value;
            }
        }
        return nullptr;
    }
};

struct Parser {
    const std::string& s;
    size_t i = 0;

    void space() {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
            i++;
        }
    }

    bool parse(Json& out) {
        space();
        if (i >= s.size()) {
            return false;
        }
        char c = s[i];
        if (c == '{' || c == '[') {
            bool object = c == '{';
            char close = object ? '}' : ']';
            out.kind = object ? Json::Kind::Object : Json::Kind::List;
            i++;
            space();
            if (i < s.size() && s[i] == close) {
                i++;
                return true;
            }
            while (true) {
                Json key;
                if (object) {
                    space();
                    if (!parse(key) || key.kind != Json::Kind::Text) {
                        return false;
                    }
                    space();
                    if (i >= s.size() || s[i++] != ':') {
                        return false;
                    }
                }
                Json value;
                if (!parse(value)) {
                    return false;
                }
                if (object) {
                    out.object.emplace_back(key.text, std::move(value));
                } else {
                    out.list.push_back(std::move(value));
                }
                space();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                return i < s.size() && s[i++] == close;
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::Text;
            i++;
            while (i < s.size() && s[i] != '"') {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    i++;
                }
                out.text.push_back(s[i++]);
            }
            i++;
            return true;
        }
        for (const char* word : {"null", "true", "false"}) {
            size_t length = std::strlen(word);
            if (s.compare(i, length, word) == 0) {
                i += length;
                return true;
            }
        }
        char* end = nullptr;
        out.kind = Json::Kind::Number;
        out.number = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) {
            return false;
        }
        i = static_cast<size_t>(end - s.c_str());
        return true;
    }
};

struct Session {
    std::wstring code;
    int players = 0;
    long seconds = 0;
};

struct Census {
    int playing = 0;
    int online = 0;
    std::vector<std::pair<std::wstring, int>> systems;
    std::vector<std::pair<std::wstring, int>> versions;
    std::vector<Session> sessions;
};

struct Reply {
    bool ok = false;
    std::wstring error;
    Census census;
};

HWND g_window;
std::wstring g_key;
bool g_busy = false;
bool g_loaded = false;
bool g_failing = false;
std::wstring g_error;
std::wstring g_updated;
Census g_census;
RectF g_refresh_area;

REAL scale_of(HWND window) {
    return static_cast<REAL>(GetDpiForWindow(window)) / 96.0f;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring read_key() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::ifstream file(std::filesystem::path(path).parent_path() / "monitor_key.txt");
    std::string key;
    std::getline(file, key);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
        key.pop_back();
    }
    return widen(key);
}

std::wstring system_name(const std::string& id) {
    if (id == "windows") {
        return L"Windows";
    }
    if (id == "linux") {
        return L"Linux";
    }
    if (id == "macos") {
        return L"macOS";
    }
    if (id == "android") {
        return L"Android";
    }
    return widen(id);
}

double number(const Json* value) {
    return value && value->kind == Json::Kind::Number ? value->number : 0;
}

std::vector<std::pair<std::wstring, int>> counts(const Json* value, bool systems) {
    std::vector<std::pair<std::wstring, int>> out;
    if (value) {
        for (const auto& [name, count] : value->object) {
            out.emplace_back(systems ? system_name(name) : L"v" + widen(name), static_cast<int>(number(&count)));
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

Reply fetch(const std::wstring& key) {
    Reply reply;
    HINTERNET session = WinHttpOpen(L"WondersBeyondMonitor/2", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        reply.error = L"Could not start the connection";
        return reply;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 8000, 8000);
    HINTERNET connection = WinHttpConnect(session, kHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", kPath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::string body;
    DWORD status = 0;
    if (request) {
        std::wstring headers = L"X-Key: " + key + L"\r\n";
        if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
            DWORD length = sizeof status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX);
            char buffer[8192];
            DWORD read = 0;
            while (WinHttpReadData(request, buffer, sizeof buffer, &read) && read > 0 && body.size() < (4u << 20)) {
                body.append(buffer, read);
            }
        }
        WinHttpCloseHandle(request);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    if (status == 0) {
        reply.error = L"Server unreachable, check your internet";
        return reply;
    }
    if (status == 404) {
        reply.error = L"Key refused by the server";
        return reply;
    }
    if (status != 200) {
        reply.error = L"Server error " + std::to_wstring(status);
        return reply;
    }
    Json data;
    Parser parser{body};
    if (!parser.parse(data) || data.kind != Json::Kind::Object) {
        reply.error = L"Unreadable answer from the server";
        return reply;
    }
    reply.ok = true;
    reply.census.playing = static_cast<int>(number(data.get("players")));
    reply.census.online = static_cast<int>(number(data.get("online")));
    reply.census.systems = counts(data.get("platforms"), true);
    reply.census.versions = counts(data.get("versions"), false);
    if (const Json* rooms = data.get("rooms")) {
        for (const Json& room : rooms->list) {
            const Json* code = room.get("code");
            reply.census.sessions.push_back(
                {code ? widen(code->text) : L"?", static_cast<int>(number(room.get("players"))), static_cast<long>(number(room.get("seconds")))});
        }
    }
    return reply;
}

std::wstring duration(long seconds) {
    wchar_t text[32];
    if (seconds < 60) {
        return L"just now";
    }
    if (seconds >= 3600) {
        std::swprintf(text, 32, L"%ld h %02ld min", seconds / 3600, (seconds / 60) % 60);
    } else {
        std::swprintf(text, 32, L"%ld min", seconds / 60);
    }
    return text;
}

void refresh() {
    if (g_busy || g_key.empty()) {
        return;
    }
    g_busy = true;
    InvalidateRect(g_window, nullptr, FALSE);
    std::wstring key = g_key;
    HWND window = g_window;
    std::thread([key, window] {
        auto* reply = new Reply(fetch(key));
        if (!PostMessageW(window, kReport, 0, reinterpret_cast<LPARAM>(reply))) {
            delete reply;
        }
    }).detach();
}

void rounded(GraphicsPath& path, RectF r, REAL radius) {
    REAL d = radius * 2;
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void panel(Graphics& g, RectF r, const Color& fill, const Color& edge, REAL radius) {
    GraphicsPath path;
    rounded(path, r, radius);
    SolidBrush brush(fill);
    g.FillPath(&brush, &path);
    Pen pen(edge, 1);
    g.DrawPath(&pen, &path);
}

struct Fonts {
    FontFamily family{L"Segoe UI"};
    Font title{&family, 20, FontStyleBold, UnitPixel};
    Font huge{&family, 44, FontStyleBold, UnitPixel};
    Font label{&family, 15, FontStyleBold, UnitPixel};
    Font body{&family, 14, FontStyleRegular, UnitPixel};
    Font small{&family, 12, FontStyleRegular, UnitPixel};
    Font code{L"Consolas", 17, FontStyleBold, UnitPixel};
};

REAL text_width(Graphics& g, const std::wstring& text, const Font& font) {
    RectF box;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), StringFormat::GenericTypographic(), &box);
    return box.Width;
}

void text(Graphics& g, const std::wstring& value, const Font& font, const Color& color, REAL x, REAL y) {
    SolidBrush brush(color);
    g.DrawString(value.c_str(), -1, &font, PointF(x, y), StringFormat::GenericTypographic(), &brush);
}

void centered(Graphics& g, const std::wstring& value, const Font& font, const Color& color, RectF area) {
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color);
    g.DrawString(value.c_str(), -1, &font, area, &format, &brush);
}

void dot(Graphics& g, REAL x, REAL y, REAL size, const Color& color) {
    SolidBrush brush(color);
    g.FillEllipse(&brush, x, y, size, size);
}

void stat_card(Graphics& g, const Fonts& f, RectF r, const std::wstring& value, const std::wstring& label, const std::wstring& hint, const Color& accent) {
    panel(g, r, kCard, kCardEdge, 12);
    SolidBrush bar(accent);
    g.FillRectangle(&bar, r.X + 18, r.Y + 18, 28.0f, 4.0f);
    text(g, value, f.huge, kText, r.X + 16, r.Y + 30);
    text(g, label, f.label, kText, r.X + 18, r.Y + 86);
    text(g, hint, f.small, kMuted, r.X + 18, r.Y + 106);
}

REAL chips(Graphics& g, const Fonts& f, REAL x, REAL y, REAL right, const std::wstring& title, const std::vector<std::pair<std::wstring, int>>& items) {
    text(g, title, f.small, kMuted, x, y + 6);
    REAL cx = x + 70;
    if (items.empty()) {
        text(g, L"nobody yet", f.body, kFaint, cx, y + 4);
        return y + 30;
    }
    for (const auto& [name, count] : items) {
        std::wstring value = std::to_wstring(count);
        REAL w = text_width(g, name, f.body) + text_width(g, value, f.label) + 30;
        if (cx + w > right) {
            cx = x + 70;
            y += 32;
        }
        panel(g, RectF(cx, y, w, 26), kCard, kCardEdge, 13);
        text(g, name, f.body, kText, cx + 11, y + 4);
        text(g, value, f.label, kAmber, cx + w - 11 - text_width(g, value, f.label), y + 4);
        cx += w + 8;
    }
    return y + 32;
}

void paint(HDC target, int pixel_width, int pixel_height, REAL scale) {
    Bitmap canvas(pixel_width, pixel_height, PixelFormat32bppPARGB);
    Graphics g(&canvas);
    g.ScaleTransform(scale, scale);
    const int width = static_cast<int>(pixel_width / scale);
    const int height = static_cast<int>(pixel_height / scale);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.Clear(kBackground);
    Fonts f;
    const REAL m = 22;
    const REAL right = static_cast<REAL>(width) - m;

    text(g, L"Wonders Beyond", f.title, kText, m, 18);
    text(g, L"Live players", f.title, kMuted, m + text_width(g, L"Wonders Beyond", f.title) + 10, 18);

    std::wstring status;
    Color status_color = kGreen;
    if (g_key.empty()) {
        status = L"No key";
        status_color = kRed;
    } else if (g_busy && !g_loaded) {
        status = L"Connecting...";
        status_color = kAmber;
    } else if (g_failing) {
        status = g_error;
        status_color = kRed;
    } else {
        status = g_busy ? L"Refreshing..." : L"Live, updated " + g_updated;
    }
    REAL sw = text_width(g, status, f.body);
    REAL sx = right - sw - 12;
    g_refresh_area = RectF(sx - 22, 14, sw + 34, 30);
    panel(g, g_refresh_area, kCard, kCardEdge, 15);
    dot(g, sx - 10, 25, 8, status_color);
    text(g, status, f.body, kText, sx + 2, 21);

    if (g_key.empty()) {
        centered(g, L"Put the census key in monitor_key.txt next to this program, then open it again.", f.body, kMuted,
                 RectF(m, 60, right - m, static_cast<REAL>(height) - 120));
    } else {
        const Census& c = g_census;
        REAL top = 62;
        REAL gap = 14;
        REAL cw = (right - m - 2 * gap) / 3;
        auto shown = [&](int value) { return g_loaded ? std::to_wstring(value) : std::wstring(L"-"); };
        stat_card(g, f, RectF(m, top, cw, 132), shown(c.playing), L"Playing now", L"Games open right now", kGreen);
        stat_card(g, f, RectF(m + cw + gap, top, cw, 132), shown(c.online), L"In multiplayer", L"Players inside a session", kBlue);
        stat_card(g, f, RectF(m + 2 * (cw + gap), top, cw, 132), shown(static_cast<int>(c.sessions.size())), L"Sessions", L"Multiplayer rooms open", kViolet);

        REAL y = top + 132 + 18;
        y = chips(g, f, m, y, right, L"Systems", c.systems);
        y = chips(g, f, m, y + 2, right, L"Versions", c.versions);

        y += 14;
        text(g, L"Multiplayer sessions", f.label, kText, m, y);
        y += 28;
        REAL list_bottom = static_cast<REAL>(height) - 40;
        RectF list(m, y, right - m, std::max<REAL>(list_bottom - y, 60));
        panel(g, list, kRow, kCardEdge, 12);
        REAL c1 = m + 18;
        REAL c2 = m + (right - m) * 0.38f;
        REAL c3 = m + (right - m) * 0.70f;
        text(g, L"ROOM CODE", f.small, kFaint, c1, y + 12);
        text(g, L"PLAYERS", f.small, kFaint, c2, y + 12);
        text(g, L"OPEN FOR", f.small, kFaint, c3, y + 12);
        Pen line(kCardEdge, 1);
        g.DrawLine(&line, m, y + 34, right, y + 34);
        REAL row_y = y + 34;
        const REAL row_h = 42;
        if (c.sessions.empty()) {
            centered(g, g_loaded ? L"No multiplayer session right now" : L"", f.body, kMuted, RectF(m, row_y, right - m, list.GetBottom() - row_y));
        }
        size_t index = 0;
        for (; index < c.sessions.size() && row_y + row_h <= list.GetBottom() - (index + 1 < c.sessions.size() ? 24 : 0); index++) {
            const Session& s = c.sessions[index];
            text(g, s.code, f.code, kText, c1, row_y + 11);
            for (int p = 0; p < kMaxPlayers; p++) {
                dot(g, c2 + p * 16, row_y + 16, 10, p < s.players ? kBlue : kCardEdge);
            }
            text(g, std::to_wstring(s.players) + L" / " + std::to_wstring(kMaxPlayers), f.body, kMuted, c2 + kMaxPlayers * 16 + 8, row_y + 12);
            text(g, duration(s.seconds), f.body, kText, c3, row_y + 12);
            row_y += row_h;
            if (index + 1 < c.sessions.size()) {
                g.DrawLine(&line, m + 12, row_y, right - 12, row_y);
            }
        }
        if (index < c.sessions.size()) {
            text(g, L"+ " + std::to_wstring(c.sessions.size() - index) + L" more, make the window taller to see them", f.small, kMuted, c1, row_y + 4);
        }
    }

    text(g, L"Refreshes every 15 s. Click the status or press F5 to refresh now. A player stays counted about 5 minutes after their game last checked in.",
         f.small, kFaint, m, static_cast<REAL>(height) - 26);

    Graphics screen(target);
    screen.DrawImage(&canvas, 0, 0);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(window, 20, &dark, sizeof dark);
        g_key = read_key();
        if (!g_key.empty()) {
            SetTimer(window, kTimer, kRefreshMs, nullptr);
            refresh();
        }
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window, &ps);
        RECT r;
        GetClientRect(window, &r);
        if (r.right > 0 && r.bottom > 0) {
            paint(dc, r.right, r.bottom, scale_of(window));
        }
        EndPaint(window, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* wanted = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, wanted->left, wanted->top, wanted->right - wanted->left, wanted->bottom - wanted->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {static_cast<LONG>(640 * scale_of(window)), static_cast<LONG>(520 * scale_of(window))};
        return 0;
    case WM_TIMER:
        refresh();
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_F5) {
            refresh();
        }
        return 0;
    case WM_LBUTTONUP: {
        REAL x = static_cast<REAL>(static_cast<short>(LOWORD(lparam))) / scale_of(window);
        REAL y = static_cast<REAL>(static_cast<short>(HIWORD(lparam))) / scale_of(window);
        if (g_refresh_area.Contains(x, y)) {
            refresh();
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(window, &p);
            SetCursor(LoadCursor(nullptr, g_refresh_area.Contains(p.x / scale_of(window), p.y / scale_of(window)) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case kReport: {
        std::unique_ptr<Reply> reply(reinterpret_cast<Reply*>(lparam));
        g_busy = false;
        g_failing = !reply->ok;
        if (reply->ok) {
            g_census = std::move(reply->census);
            g_loaded = true;
            SYSTEMTIME now;
            GetLocalTime(&now);
            wchar_t clock[16];
            std::swprintf(clock, 16, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
            g_updated = clock;
        } else {
            g_error = reply->error;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, kTimer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    GdiplusStartupInput startup;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &startup, nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"WondersBeyondMonitor";
    RegisterClassW(&window_class);
    REAL scale = static_cast<REAL>(GetDpiForSystem()) / 96.0f;
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Wonders Beyond - Live players", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                  static_cast<int>(760 * scale), static_cast<int>(640 * scale), nullptr, nullptr, instance, nullptr);
    ShowWindow(window, show_command);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    GdiplusShutdown(token);
    return 0;
}
