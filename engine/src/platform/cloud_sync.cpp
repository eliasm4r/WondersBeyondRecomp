#include "wp/cloud_sync.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#elif defined(__ANDROID__)
#include <SDL3/SDL_system.h>
#include <jni.h>
#endif

#include "wp/game.h"
#include "wp/log.h"
#include "wp/play_time.h"
#include "wp/save_backup.h"
#include "wp/save_cloud.h"
#include "wp/update_check.h"

namespace wp::cloud {

namespace {

namespace fs = std::filesystem;

constexpr char kHost[] = "wondersbeyond-saves.save-sync.workers.dev";
constexpr char kRoomsHost[] = "wondersbeyond-rooms.save-sync.workers.dev";
constexpr char kLetters[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr size_t kLargestReply = 128 * 1024;
constexpr int kAttempts = 3;
constexpr auto kQuietAfterSave = std::chrono::seconds(2);
constexpr auto kReadAgainAfter = std::chrono::milliseconds(200);

#if defined(_WIN32)
constexpr char kDevice[] = "PC";
#elif defined(__ANDROID__)
constexpr char kDevice[] = "Android";
#elif defined(__APPLE__)
constexpr char kDevice[] = "Mac";
#else
constexpr char kDevice[] = "Linux";
#endif

struct Reply {
    int status = 0;
    uint64_t revision = 0;
    Bytes body;
};

struct Job {
    bool linking = false;
    std::string code;
};

struct Shared {
    std::mutex lock;
    std::condition_variable wake;
    Status status = Status::Off;
    std::string code;
    std::string agreed;
    std::string remote;
    std::string device;
    uint32_t seconds = 0;
    bool pending = false;
    uint64_t version = 0;
    std::string keys_code;
    Keys keys;
    fs::path root;
    fs::path relative;
    fs::path backups;
    fs::path file;
    int keep = 5;
    std::deque<Job> jobs;
    bool dirty = false;
    bool running = false;
    bool worker = false;
    std::chrono::steady_clock::time_point last_save;
};

Shared g;

void set_status(Status status) {
    std::lock_guard<std::mutex> hold(g.lock);
    g.status = status;
    g.version++;
}

void store_state() {
    if (g.file.empty()) {
        return;
    }
    std::error_code error;
    if (g.code.empty()) {
        fs::remove(g.file, error);
        return;
    }
    fs::path partial = g.file;
    partial += ".partial";
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out << "code=" << g.code << "\nagreed=" << g.agreed << "\npending=" << (g.pending ? 1 : 0) << "\n";
        if (!out.flush()) {
            return;
        }
    }
    fs::rename(partial, g.file, error);
}

void load_state() {
    g.code.clear();
    g.agreed.clear();
    g.pending = false;
    std::ifstream in(g.file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        size_t split = line.find('=');
        if (split == std::string::npos) {
            continue;
        }
        std::string name = line.substr(0, split);
        std::string value = line.substr(split + 1);
        if (name == "code") {
            clean_code(value, g.code);
        } else if (name == "agreed") {
            g.agreed = value;
        } else if (name == "pending") {
            g.pending = value == "1";
        }
    }
}

std::string agent() {
    return std::string(game::description().app_name) + "/" + update::version();
}

#if defined(_WIN32)
std::wstring widen(const std::string& text) {
    return std::wstring(text.begin(), text.end());
}

bool fetch(const char* host, const char* method, const std::string& address, const Bytes* body, uint64_t revision, Reply& reply) {
    std::wstring path = widen(address);
    HINTERNET session = WinHttpOpen(widen(agent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        return false;
    }
    WinHttpSetTimeouts(session, 3000, 3000, 4000, 4000);
    HINTERNET connection = WinHttpConnect(session, widen(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET handle = connection ? WinHttpOpenRequest(connection, widen(method).c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                       WINHTTP_FLAG_SECURE)
                                  : nullptr;
    bool ok = false;
    if (handle) {
        std::wstring headers = L"Content-Type: application/octet-stream\r\nX-Revision: " + std::to_wstring(revision) + L"\r\n";
        LPVOID data = body ? const_cast<uint8_t*>(body->data()) : WINHTTP_NO_REQUEST_DATA;
        DWORD size = body ? static_cast<DWORD>(body->size()) : 0;
        if (WinHttpSendRequest(handle, headers.c_str(), static_cast<DWORD>(-1), data, size, size, 0) && WinHttpReceiveResponse(handle, nullptr)) {
            DWORD status = 0;
            DWORD length = sizeof status;
            ok = WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &length,
                                     WINHTTP_NO_HEADER_INDEX) != FALSE;
            reply.status = static_cast<int>(status);
            wchar_t text[32] = {};
            DWORD text_size = sizeof text;
            if (WinHttpQueryHeaders(handle, WINHTTP_QUERY_CUSTOM, L"X-Revision", text, &text_size, WINHTTP_NO_HEADER_INDEX)) {
                reply.revision = std::wcstoull(text, nullptr, 10);
            }
            uint8_t buffer[8192];
            DWORD read = 0;
            while (ok && reply.body.size() < kLargestReply && WinHttpReadData(handle, buffer, sizeof buffer, &read) && read > 0) {
                reply.body.insert(reply.body.end(), buffer, buffer + read);
            }
        }
        WinHttpCloseHandle(handle);
    }
    if (connection) {
        WinHttpCloseHandle(connection);
    }
    WinHttpCloseHandle(session);
    return ok;
}
#elif defined(__ANDROID__)
bool fetch(const char* host, const char* method, const std::string& address, const Bytes* body, uint64_t revision, Reply& reply) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) {
        return false;
    }
    bool ok = false;
    jclass type = env->GetObjectClass(activity);
    jmethodID call = type ? env->GetMethodID(type, "cloudRequest", "(Ljava/lang/String;Ljava/lang/String;[BJ)[B") : nullptr;
    if (call) {
        std::string url = std::string("https://") + host + address;
        jstring method_text = env->NewStringUTF(method);
        jstring url_text = env->NewStringUTF(url.c_str());
        jbyteArray data = nullptr;
        if (body) {
            data = env->NewByteArray(static_cast<jsize>(body->size()));
            env->SetByteArrayRegion(data, 0, static_cast<jsize>(body->size()), reinterpret_cast<const jbyte*>(body->data()));
        }
        auto answer = static_cast<jbyteArray>(env->CallObjectMethod(activity, call, method_text, url_text, data, static_cast<jlong>(revision)));
        if (!env->ExceptionCheck() && answer) {
            jsize size = env->GetArrayLength(answer);
            if (size >= 12 && static_cast<size_t>(size) <= kLargestReply) {
                Bytes all(static_cast<size_t>(size));
                env->GetByteArrayRegion(answer, 0, size, reinterpret_cast<jbyte*>(all.data()));
                for (int i = 0; i < 4; i++) {
                    reply.status = reply.status << 8 | all[static_cast<size_t>(i)];
                }
                for (int i = 4; i < 12; i++) {
                    reply.revision = reply.revision << 8 | all[static_cast<size_t>(i)];
                }
                reply.body.assign(all.begin() + 12, all.end());
                ok = true;
            }
        }
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            ok = false;
        }
        if (answer) {
            env->DeleteLocalRef(answer);
        }
        if (data) {
            env->DeleteLocalRef(data);
        }
        env->DeleteLocalRef(url_text);
        env->DeleteLocalRef(method_text);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    if (type) {
        env->DeleteLocalRef(type);
    }
    env->DeleteLocalRef(activity);
    return ok;
}
#else
std::string shell_quoted(const std::string& text) {
    std::string out = "'";
    for (char character : text) {
        if (character == '\'') {
            out += "'\\''";
        } else {
            out.push_back(character);
        }
    }
    return out + "'";
}

bool fetch(const char* host, const char* method, const std::string& address, const Bytes* body, uint64_t revision, Reply& reply) {
    std::error_code error;
    std::string stem = (fs::temp_directory_path(error) / ("wbr-sync-" + new_code())).string();
    std::string headers_path = stem + ".head";
    std::string output_path = stem + ".out";
    std::string input_path = stem + ".in";
    std::string command = "curl -s -m 8 -X " + std::string(method) + " -A " + shell_quoted(agent()) + " -D " + shell_quoted(headers_path) + " -o " + shell_quoted(output_path) +
                          " -w '%{http_code}' -H 'Content-Type: application/octet-stream' -H 'X-Revision: " + std::to_string(revision) + "'";
    if (body) {
        std::ofstream input(input_path, std::ios::binary);
        input.write(reinterpret_cast<const char*>(body->data()), static_cast<std::streamsize>(body->size()));
        if (!input.flush()) {
            return false;
        }
        command += " --data-binary @" + shell_quoted(input_path);
    }
    command += " " + shell_quoted(std::string("https://") + host + address) + " 2>/dev/null";
    bool ok = false;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char text[16] = {};
        size_t got = std::fread(text, 1, sizeof text - 1, pipe);
        ok = pclose(pipe) == 0 && got > 0;
        reply.status = std::atoi(text);
    }
    if (ok) {
        std::ifstream head(headers_path);
        std::string line;
        while (std::getline(head, line)) {
            std::string lower = line;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.rfind("x-revision:", 0) == 0) {
                reply.revision = std::strtoull(line.c_str() + 11, nullptr, 10);
            }
        }
        std::ifstream out(output_path, std::ios::binary);
        reply.body.assign(std::istreambuf_iterator<char>(out), std::istreambuf_iterator<char>());
        if (reply.body.size() > kLargestReply) {
            ok = false;
        }
    }
    fs::remove(headers_path, error);
    fs::remove(output_path, error);
    fs::remove(input_path, error);
    return ok;
}
#endif

bool request(const char* method, const std::string& id, const Bytes* body, uint64_t revision, Reply& reply) {
    return fetch(kHost, method, "/v1/saves/" + id, body, revision, reply);
}

bool read_stable(const fs::path& folder, Snapshot& snapshot) {
    Snapshot second;
    if (!read_save(folder, snapshot)) {
        return false;
    }
    std::this_thread::sleep_for(kReadAgainAfter);
    return read_save(folder, second) && progress_hash(snapshot) == progress_hash(second);
}

const Keys& keys_for(const std::string& code) {
    if (g.keys_code != code) {
        g.keys = derive_keys(code);
        g.keys_code = code;
    }
    return g.keys;
}

bool take_remote(const Snapshot& remote, const std::string& hash) {
    fs::path root;
    fs::path relative;
    fs::path backups;
    int keep;
    {
        std::lock_guard<std::mutex> hold(g.lock);
        root = g.root;
        relative = g.relative;
        backups = g.backups;
        keep = g.keep;
    }
    saves::Outcome backup = saves::back_up(root, relative, backups, std::max(keep, 1), std::time(nullptr));
    if (backup.result == saves::Result::Failed) {
        log::write("cloud", "no backup before receiving: %s", backup.message.c_str());
        return false;
    }
    if (!write_save(root / relative, remote)) {
        log::write("cloud", "could not write the received save");
        return false;
    }
    std::lock_guard<std::mutex> hold(g.lock);
    g.agreed = hash;
    g.pending = false;
    g.status = Status::Synced;
    g.version++;
    store_state();
    return true;
}

void agree(const std::string& hash) {
    std::lock_guard<std::mutex> hold(g.lock);
    g.agreed = hash;
    g.status = Status::Synced;
    g.version++;
    store_state();
}

void run(bool at_start) {
    std::string code;
    std::string agreed;
    bool pending;
    fs::path folder;
    {
        std::lock_guard<std::mutex> hold(g.lock);
        code = g.code;
        agreed = g.agreed;
        pending = g.pending;
        folder = g.root / g.relative;
        if (code.empty()) {
            return;
        }
        g.status = Status::Working;
        g.version++;
    }
    const Keys& keys = keys_for(code);
    Snapshot local;
    if (!read_stable(folder, local)) {
        set_status(Status::Error);
        return;
    }
    std::string local_hash = progress_hash(local);
    for (int attempt = 0; attempt < kAttempts; attempt++) {
        Reply got;
        if (!request("GET", keys.id, nullptr, 0, got) || (got.status != 200 && got.status != 404)) {
            log::write("cloud", "the server did not answer (status %d)", got.status);
            set_status(Status::Offline);
            return;
        }
        Snapshot remote;
        std::string remote_hash;
        uint64_t revision = 0;
        if (got.status == 200) {
            if (!open(keys.key, got.body, remote)) {
                log::write("cloud", "the save on the server could not be opened");
                set_status(Status::Error);
                return;
            }
            revision = got.revision;
            remote_hash = progress_hash(remote);
        }
        if (pending && got.status == 200) {
            if (!at_start) {
                set_status(Status::Restart);
            } else if (!take_remote(remote, remote_hash)) {
                set_status(Status::Error);
            } else {
                log::write("cloud", "received the chosen save");
            }
            return;
        }
        Facts facts;
        facts.local = local_hash;
        facts.agreed = agreed;
        facts.remote = remote_hash;
        facts.at_start = at_start;
        switch (decide(facts)) {
        case Decision::Nothing:
            if (remote_hash.empty() || remote_hash == local_hash) {
                agree(remote_hash.empty() ? agreed : local_hash);
            } else {
                set_status(Status::Restart);
            }
            return;
        case Decision::Download:
            if (take_remote(remote, remote_hash)) {
                log::write("cloud", "received newer progress");
            } else {
                set_status(Status::Error);
            }
            return;
        case Decision::Ask: {
            std::lock_guard<std::mutex> hold(g.lock);
            g.remote = remote_hash;
            g.device = remote.device;
            g.seconds = remote.play_seconds;
            g.status = Status::Conflict;
            g.version++;
            log::write("cloud", "this device and the server hold different progress");
            return;
        }
        case Decision::Upload: {
            local.saved_at = static_cast<int64_t>(std::time(nullptr));
            local.play_seconds = static_cast<uint32_t>(play_time::seconds());
            local.device = kDevice;
            Bytes sealed = seal(keys.key, local);
            Reply sent;
            if (!request("PUT", keys.id, &sealed, revision, sent)) {
                set_status(Status::Offline);
                return;
            }
            if (sent.status == 200) {
                agree(local_hash);
                log::write("cloud", "sent the save");
                return;
            }
            if (sent.status != 409) {
                log::write("cloud", "the server refused the save (status %d)", sent.status);
                set_status(sent.status == 429 || sent.status >= 500 ? Status::Offline : Status::Error);
                return;
            }
            break;
        }
        }
    }
    set_status(Status::Offline);
}

void run_link(const std::string& code) {
    set_status(Status::Working);
    Keys keys = derive_keys(code);
    Reply got;
    if (!request("GET", keys.id, nullptr, 0, got) || (got.status != 200 && got.status != 404)) {
        set_status(Status::Offline);
        return;
    }
    Snapshot remote;
    if (got.status == 404 || !open(keys.key, got.body, remote)) {
        set_status(Status::Unknown);
        return;
    }
    {
        std::lock_guard<std::mutex> hold(g.lock);
        g.code = code;
        g.agreed.clear();
        g.pending = false;
        store_state();
    }
    run(false);
}

void work() {
    std::unique_lock<std::mutex> hold(g.lock);
    while (true) {
        g.wake.wait(hold, [] { return !g.jobs.empty() || g.dirty; });
        if (g.jobs.empty()) {
            auto ready = g.last_save + kQuietAfterSave;
            if (std::chrono::steady_clock::now() < ready) {
                g.wake.wait_until(hold, ready);
                continue;
            }
        }
        Job job;
        if (!g.jobs.empty()) {
            job = g.jobs.front();
            g.jobs.pop_front();
        } else {
            g.dirty = false;
        }
        g.running = true;
        hold.unlock();
        try {
            if (job.linking) {
                run_link(job.code);
            } else {
                run(false);
            }
        } catch (const std::exception& problem) {
            log::write("cloud", "sync failed: %s", problem.what());
            set_status(Status::Error);
        }
        hold.lock();
        g.running = false;
        g.version++;
    }
}

void enqueue(Job job) {
    std::lock_guard<std::mutex> hold(g.lock);
    g.jobs.push_back(std::move(job));
    if (!g.worker) {
        g.worker = true;
        std::thread(work).detach();
    }
    g.wake.notify_all();
}

}

void start(const fs::path& nand_root, const fs::path& relative, const fs::path& backups, int keep) {
    {
        std::lock_guard<std::mutex> hold(g.lock);
        g.root = nand_root;
        g.relative = relative;
        g.backups = backups;
        g.keep = keep;
        g.file = backups.parent_path() / "cloud_sync.txt";
        load_state();
        g.status = g.code.empty() ? Status::Off : Status::Working;
        g.version++;
    }
    try {
        run(true);
    } catch (const std::exception& problem) {
        log::write("cloud", "sync failed: %s", problem.what());
        set_status(Status::Error);
    }
}

void saved() {
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.code.empty()) {
        return;
    }
    g.dirty = true;
    g.last_save = std::chrono::steady_clock::now();
    if (!g.worker) {
        g.worker = true;
        std::thread(work).detach();
    }
    g.wake.notify_all();
}

void create() {
    {
        std::lock_guard<std::mutex> hold(g.lock);
        g.code = new_code();
        g.agreed.clear();
        g.pending = false;
        g.status = Status::Working;
        g.version++;
        store_state();
    }
    enqueue({});
}

void link(const std::string& typed) {
    std::string cleaned;
    if (!clean_code(typed, cleaned)) {
        set_status(Status::Unknown);
        return;
    }
    set_status(Status::Working);
    enqueue({true, cleaned});
}

void keep_here() {
    {
        std::lock_guard<std::mutex> hold(g.lock);
        if (g.status != Status::Conflict) {
            return;
        }
        g.agreed = g.remote;
        g.status = Status::Working;
        g.version++;
        store_state();
    }
    enqueue({});
}

void keep_other() {
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.status != Status::Conflict) {
        return;
    }
    g.pending = true;
    g.status = Status::Restart;
    g.version++;
    store_state();
}

void disconnect() {
    std::lock_guard<std::mutex> hold(g.lock);
    g.code.clear();
    g.agreed.clear();
    g.remote.clear();
    g.pending = false;
    g.dirty = false;
    g.jobs.clear();
    g.status = Status::Off;
    g.version++;
    store_state();
}

Status status() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.status;
}

std::string code() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.code;
}

uint64_t version() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.version;
}

bool busy() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.running || g.dirty || !g.jobs.empty();
}

uint32_t other_seconds() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.seconds;
}

std::string other_device() {
    std::lock_guard<std::mutex> hold(g.lock);
    return g.device;
}

const char* code_letters() {
    return kLetters;
}

const char* rooms_host() {
    return kRoomsHost;
}


bool room_request(const char* method, const std::string& path, const std::string& body, int& status, std::string& reply) {
    Bytes data(body.begin(), body.end());
    Reply answer;
    bool ok = fetch(kRoomsHost, method, path, body.empty() ? nullptr : &data, 0, answer);
    status = answer.status;
    reply.assign(answer.body.begin(), answer.body.end());
    return ok;
}

}
