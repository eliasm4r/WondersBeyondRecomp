#include "wp/update_check.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#endif

#include "wp/game.h"
#include "wp/log.h"
#include "wp/settings.h"

namespace wp::update {

namespace {

constexpr size_t kMaxReply = 512 * 1024;
constexpr size_t kMaxTag = 40;

std::atomic<State> g_state{State::Off};
std::atomic<bool> g_started{false};
std::mutex g_mutex;
std::string g_latest;

bool enabled() {
    static const settings::LiveFlag flag("system.check_updates", "WP_CHECK_UPDATES");
    return flag();
}

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

std::string fetch() {
    std::string agent = std::string(game::description().app_name) + "/" + version();
    std::string path = std::string("/repos/") + game::description().releases_repository + "/releases/latest";
    std::string reply;
    HINTERNET session = WinHttpOpen(widen(agent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return reply;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 10000, 10000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", widen(path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                                        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                   : nullptr;
    DWORD status = 0;
    DWORD size = sizeof status;
    if (request) {
        WinHttpAddRequestHeaders(request, L"Accept: application/vnd.github+json", static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);
    }
    if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr) &&
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                            WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        char buffer[8192];
        DWORD read = 0;
        while (reply.size() < kMaxReply && WinHttpReadData(request, buffer, sizeof buffer, &read) && read > 0) {
            reply.append(buffer, read);
        }
    } else {
        log::write("update", "GitHub did not answer (status %lu, error %lu)", static_cast<unsigned long>(status), static_cast<unsigned long>(GetLastError()));
    }
    if (request) {
        WinHttpCloseHandle(request);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return reply;
}
#else
std::string fetch() {
    return "";
}
#endif

void check() {
    std::string tag = tag_of(fetch());
    const char* pretend = std::getenv("WP_UPDATE_VERSION");
    std::string mine = pretend && *pretend ? pretend : version();
    int theirs[3];
    int ours[3];
    State result = State::Failed;
    if (!tag.empty() && parse_version(tag, theirs) && parse_version(mine, ours)) {
        result = newer(tag, mine) ? State::Newer : State::Current;
        log::write("update", "latest release %s, this is %s", tag.c_str(), mine.c_str());
    } else {
        log::write("update", "could not read the latest release");
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_latest = tag;
    }
    g_state = result;
}

}

const char* version() {
    return WP_VERSION;
}

bool available() {
#ifdef _WIN32
    return game::description().releases_repository[0] != 0;
#else
    return false;
#endif
}

void start() {
    if (!available() || !enabled() || g_started.exchange(true)) {
        return;
    }
    g_state = State::Checking;
    std::thread(check).detach();
}

State state() {
    if (!available() || !enabled()) {
        return State::Off;
    }
    start();
    return g_state.load();
}

std::string latest() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_latest;
}

void open_page() {
#ifdef _WIN32
    std::string page = std::string("https://github.com/") + game::description().releases_repository + "/releases/latest";
    ShellExecuteA(nullptr, "open", page.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
}

bool parse_version(const std::string& text, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    const char* at = text.c_str();
    if (*at == 'v' || *at == 'V') {
        at++;
    }
    return std::sscanf(at, "%d.%d.%d", &out[0], &out[1], &out[2]) >= 2;
}

std::string tag_of(const std::string& json) {
    size_t at = json.find("\"tag_name\"");
    if (at != std::string::npos) {
        at = json.find(':', at + 10);
    }
    if (at != std::string::npos) {
        at = json.find('"', at);
    }
    if (at == std::string::npos) {
        return "";
    }
    std::string tag;
    for (at++; at < json.size() && json[at] != '"'; at++) {
        char letter = json[at];
        bool allowed = (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') || letter == '.' || letter == '-';
        if (!allowed || tag.size() >= kMaxTag) {
            return "";
        }
        tag.push_back(letter);
    }
    return at < json.size() ? tag : "";
}

bool newer(const std::string& theirs, const std::string& mine) {
    int a[3];
    int b[3];
    if (!parse_version(theirs, a) || !parse_version(mine, b)) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

}
