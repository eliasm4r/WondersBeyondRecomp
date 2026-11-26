#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr wchar_t kHost[] = L"wondersbeyond-rooms.save-sync.workers.dev";
constexpr wchar_t kPath[] = L"/v1/census";
constexpr UINT kReport = WM_APP + 1;
constexpr UINT_PTR kTimer = 1;
constexpr UINT kRefreshMs = 15000;
constexpr int kRefreshButton = 100;

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
        if (c == '{') {
            out.kind = Json::Kind::Object;
            i++;
            space();
            if (i < s.size() && s[i] == '}') {
                i++;
                return true;
            }
            while (true) {
                Json key;
                space();
                if (!parse(key) || key.kind != Json::Kind::Text) {
                    return false;
                }
                space();
                if (i >= s.size() || s[i++] != ':') {
                    return false;
                }
                Json value;
                if (!parse(value)) {
                    return false;
                }
                out.object.emplace_back(key.text, std::move(value));
                space();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                return i < s.size() && s[i++] == '}';
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::List;
            i++;
            space();
            if (i < s.size() && s[i] == ']') {
                i++;
                return true;
            }
            while (true) {
                Json value;
                if (!parse(value)) {
                    return false;
                }
                out.list.push_back(std::move(value));
                space();
                if (i < s.size() && s[i] == ',') {
                    i++;
                    continue;
                }
                return i < s.size() && s[i++] == ']';
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
        if (s.compare(i, 4, "null") == 0 || s.compare(i, 4, "true") == 0) {
            i += 4;
            return true;
        }
        if (s.compare(i, 5, "false") == 0) {
            i += 5;
            return true;
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

struct Report {
    bool ok = false;
    std::wstring error;
    Json data;
};

HWND g_window;
HWND g_summary;
HWND g_details;
HWND g_rooms;
HWND g_refresh;
HFONT g_big;
HFONT g_normal;
std::wstring g_key;
bool g_busy = false;

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring folder_of_exe() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring text = path;
    return text.substr(0, text.find_last_of(L"\\/") + 1);
}

std::wstring read_key() {
    std::ifstream file(std::filesystem::path(folder_of_exe() + L"monitor_key.txt"));
    std::string key;
    std::getline(file, key);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) {
        key.pop_back();
    }
    return widen(key);
}

Report fetch_report(const std::wstring& key) {
    Report report;
    HINTERNET session = WinHttpOpen(L"WondersBeyondMonitor/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        report.error = L"Could not start the connection.";
        return report;
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
        report.error = L"The server could not be reached. Check the internet connection.";
    } else if (status == 404) {
        report.error = L"The server refused the key in monitor_key.txt.";
    } else if (status != 200) {
        report.error = L"The server answered with an error (" + std::to_wstring(status) + L").";
    } else {
        Parser parser{body};
        report.ok = parser.parse(report.data) && report.data.kind == Json::Kind::Object;
        if (!report.ok) {
            report.error = L"The server sent an answer this monitor cannot read.";
        }
    }
    return report;
}

std::wstring duration(double seconds) {
    long total = static_cast<long>(seconds);
    wchar_t text[32];
    if (total >= 3600) {
        std::swprintf(text, 32, L"%ld h %02ld min", total / 3600, (total / 60) % 60);
    } else {
        std::swprintf(text, 32, L"%ld min %02ld s", total / 60, total % 60);
    }
    return text;
}

double number(const Json* value) {
    return value && value->kind == Json::Kind::Number ? value->number : 0;
}

std::wstring counts(const Json* value) {
    std::wstring text;
    if (!value) {
        return text;
    }
    for (const auto& [name, count] : value->object) {
        if (!text.empty()) {
            text += L",  ";
        }
        text += widen(name) + L" " + std::to_wstring(static_cast<long>(number(&count)));
    }
    return text.empty() ? L"none" : text;
}

void show(const Report& report) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t clock[16];
    std::swprintf(clock, 16, L"%02d:%02d:%02d", now.wHour, now.wMinute, now.wSecond);
    if (!report.ok) {
        SetWindowTextW(g_details, (report.error + L"\r\nLast try: " + clock).c_str());
        return;
    }
    const Json& data = report.data;
    const Json* rooms = data.get("rooms");
    size_t room_count = rooms ? rooms->list.size() : 0;
    std::wstring summary = std::to_wstring(static_cast<long>(number(data.get("players")))) + L" playing now     " +
                           std::to_wstring(static_cast<long>(number(data.get("online")))) + L" in multiplayer     " + std::to_wstring(room_count) +
                           (room_count == 1 ? L" session" : L" sessions");
    SetWindowTextW(g_summary, summary.c_str());
    std::wstring details = L"Systems: " + counts(data.get("platforms")) + L"\r\nVersions: " + counts(data.get("versions")) + L"\r\nUpdated at " + clock +
                           L", every 15 seconds. A player counts as playing for about 5 minutes after their game last checked in.";
    SetWindowTextW(g_details, details.c_str());
    ListView_DeleteAllItems(g_rooms);
    if (!rooms) {
        return;
    }
    int row = 0;
    for (const Json& room : rooms->list) {
        const Json* code = room.get("code");
        std::wstring code_text = code ? widen(code->text) : L"?";
        std::wstring players = std::to_wstring(static_cast<long>(number(room.get("players")))) + L" / 4";
        std::wstring open = duration(number(room.get("seconds")));
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = row;
        item.pszText = code_text.data();
        ListView_InsertItem(g_rooms, &item);
        ListView_SetItemText(g_rooms, row, 1, players.data());
        ListView_SetItemText(g_rooms, row, 2, open.data());
        row++;
    }
}

void refresh() {
    if (g_busy) {
        return;
    }
    g_busy = true;
    EnableWindow(g_refresh, FALSE);
    std::wstring key = g_key;
    HWND window = g_window;
    std::thread([key, window] {
        auto* report = new Report(fetch_report(key));
        if (!PostMessageW(window, kReport, 0, reinterpret_cast<LPARAM>(report))) {
            delete report;
        }
    }).detach();
}

void layout(int width, int height) {
    const int margin = 16;
    MoveWindow(g_summary, margin, margin, width - 2 * margin - 110, 36, TRUE);
    MoveWindow(g_refresh, width - margin - 100, margin + 2, 100, 30, TRUE);
    MoveWindow(g_details, margin, margin + 44, width - 2 * margin, 56, TRUE);
    MoveWindow(g_rooms, margin, margin + 108, width - 2 * margin, height - margin * 2 - 108, TRUE);
    int column = (width - 2 * margin - 24) / 3;
    for (int i = 0; i < 3; i++) {
        ListView_SetColumnWidth(g_rooms, i, column);
    }
}

void add_column(int index, const wchar_t* title) {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    column.pszText = const_cast<wchar_t*>(title);
    column.cx = 150;
    column.iSubItem = index;
    ListView_InsertColumn(g_rooms, index, &column);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_window = window;
        HINSTANCE instance = reinterpret_cast<CREATESTRUCTW*>(lparam)->hInstance;
        g_big = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_normal = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_summary = CreateWindowExW(0, L"STATIC", L"Loading...", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        g_details = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window, nullptr, instance, nullptr);
        g_refresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRefreshButton)), instance, nullptr);
        g_rooms = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER, 0, 0, 0, 0, window, nullptr,
                                  instance, nullptr);
        ListView_SetExtendedListViewStyle(g_rooms, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        SendMessageW(g_summary, WM_SETFONT, reinterpret_cast<WPARAM>(g_big), TRUE);
        for (HWND control : {g_details, g_refresh, g_rooms}) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(g_normal), TRUE);
        }
        add_column(0, L"Session code");
        add_column(1, L"Players");
        add_column(2, L"Open for");
        g_key = read_key();
        if (g_key.empty()) {
            SetWindowTextW(g_summary, L"No key");
            SetWindowTextW(g_details, L"Put the census key in monitor_key.txt next to this program, then open it again.");
            EnableWindow(g_refresh, FALSE);
            return 0;
        }
        SetTimer(window, kTimer, kRefreshMs, nullptr);
        refresh();
        return 0;
    }
    case WM_SIZE:
        layout(LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {520, 360};
        return 0;
    case WM_TIMER:
        refresh();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == kRefreshButton) {
            refresh();
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case kReport: {
        std::unique_ptr<Report> report(reinterpret_cast<Report*>(lparam));
        g_busy = false;
        EnableWindow(g_refresh, TRUE);
        show(*report);
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
    INITCOMMONCONTROLSEX controls{sizeof controls, ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.hIcon = LoadIcon(instance, MAKEINTRESOURCE(1));
    window_class.lpszClassName = L"WondersBeyondMonitor";
    RegisterClassW(&window_class);
    HWND window = CreateWindowExW(0, window_class.lpszClassName, L"Wonders Beyond - Players", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 720, 480, nullptr,
                                  nullptr, instance, nullptr);
    ShowWindow(window, show_command);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}
