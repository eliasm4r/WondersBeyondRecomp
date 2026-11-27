#include "wp/online.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "wp/cloud_sync.h"
#include "wp/log.h"
#include "wp/relay.h"
#include "wp/settings.h"

namespace wp::online {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kPumpEvery = std::chrono::milliseconds(16);
constexpr auto kSendEvery = std::chrono::milliseconds(33);
constexpr auto kQuietAfter = std::chrono::minutes(10);
constexpr auto kStunEvery = std::chrono::milliseconds(300);
constexpr auto kStunPatience = std::chrono::seconds(4);
constexpr auto kHostPollEvery = std::chrono::seconds(5);
constexpr auto kHostSearchEvery = std::chrono::seconds(2);
constexpr auto kJoinRetryEvery = std::chrono::seconds(3);
constexpr auto kDirectPatience = std::chrono::seconds(15);
constexpr int kJoinAttempts = 10;
constexpr int kRelayAfterAttempts = 2;
constexpr int kNetworkRetries = 3;
constexpr double kPunchSeconds = 25.0;
constexpr double kDirectSeconds = 3600.0;
constexpr auto kLostPatience = std::chrono::seconds(30);
constexpr auto kWatchEvery = std::chrono::milliseconds(250);
constexpr size_t kMostNotices = 16;
constexpr std::array<uint32_t, 9> kChosenColors = {0, 0xFFD23F, 0x4FC3F7, 0x81E67B, 0xFF8A80, 0xB39DDB, 0xFFB74D, 0xF48FB1, 0xFFFFFF};
static_assert(std::size(kColorNames) == kChosenColors.size());

uint32_t own_color_index() {
    std::string chosen = settings::text("online.color", nullptr);
    for (uint32_t i = 1; i < kChosenColors.size(); i++) {
        if (chosen == kColorNames[i]) {
            return i;
        }
    }
    return 0;
}

struct Session {
    presence::Link link;
    std::unique_ptr<relay::Socket> relay;
    std::mutex mutex;
    std::atomic<bool> stop{false};
    std::atomic<bool> relay_kept{false};
    bool hosting = false;
};

std::mutex g_mutex;
std::shared_ptr<Session> g_session;
std::vector<presence::Peer> g_peers;
std::array<float, 3> g_position{};
presence::Pose g_pose;
uint32_t g_zone = 0;
Clock::time_point g_updated{};
std::string g_code;
std::string g_local;
std::atomic<State> g_state{State::Off};
std::atomic<Problem> g_problem{Problem::None};
std::atomic<uint64_t> g_version{1};
std::atomic<bool> g_reconnecting{false};
std::vector<Notice> g_notices;
std::string g_other_version;
uint32_t g_own_id = 0;
std::string g_own_name;

bool current(const std::shared_ptr<Session>& session) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_session == session && !session->stop.load();
}

void set_state(const std::shared_ptr<Session>& session, State state, Problem problem = Problem::None) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session == session) {
        g_state.store(state);
        g_problem.store(problem);
        g_version++;
    }
}

void clear_session() {
    if (g_session) {
        g_session->stop.store(true);
        {
            std::lock_guard<std::mutex> lock(g_session->mutex);
            g_session->link.goodbye();
        }
        g_session.reset();
    }
    g_reconnecting.store(false);
    g_own_id = 0;
    g_peers.clear();
    g_code.clear();
    g_local.clear();
}

void fail(const std::shared_ptr<Session>& session, Problem problem) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_session != session) {
        return;
    }
    clear_session();
    g_state.store(State::Failed);
    g_problem.store(problem);
    g_version++;
    log::write("online", "stopped, problem %d", static_cast<int>(problem));
}

void add_notice(Notice notice) {
    if (g_notices.size() < kMostNotices) {
        g_notices.push_back(std::move(notice));
    }
}

void note_changes(const std::vector<presence::Peer>& before, const std::vector<presence::Peer>& after) {
    for (const presence::Peer& peer : after) {
        auto old = std::find_if(before.begin(), before.end(), [&](const presence::Peer& entry) { return entry.id == peer.id; });
        if (old == before.end()) {
            add_notice({Notice::Kind::Joined, peer.name, peer.zone});
        } else if (peer.zone != old->zone && peer.zone != 0) {
            add_notice({Notice::Kind::Zone, peer.name, peer.zone});
        }
    }
    for (const presence::Peer& peer : before) {
        if (std::none_of(after.begin(), after.end(), [&](const presence::Peer& entry) { return entry.id == peer.id; })) {
            add_notice({Notice::Kind::Left, peer.name, peer.zone});
        }
    }
}

void pump_loop(std::shared_ptr<Session> session) {
    const Clock::time_point started = Clock::now();
    Clock::time_point next_send = started;
    while (!session->stop.load()) {
        std::array<float, 3> position;
        presence::Pose pose;
        uint32_t zone;
        Clock::time_point updated;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            position = g_position;
            pose = g_pose;
            zone = g_zone;
            updated = g_updated;
        }
        std::vector<presence::Peer> seen;
        std::string other;
        Clock::time_point now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->link.pump(std::chrono::duration<double>(now - started).count());
            if (session->relay) {
                for (const std::vector<uint8_t>& packet : session->relay->take()) {
                    session->link.relay_packet(packet.data(), packet.size());
                }
            }
            if (now >= next_send) {
                session->link.send(position, now - updated < kQuietAfter ? zone : 0, pose);
                next_send = now + kSendEvery;
            }
            seen = session->link.peers();
            other = session->link.other_version();
        }
        if (!other.empty() && !session->hosting && seen.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_other_version = other;
            }
            fail(session, Problem::Version);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_session != session) {
                return;
            }
            if (other != g_other_version) {
                g_other_version = other;
                add_notice({Notice::Kind::Version, other, 0});
                g_version++;
            }
            note_changes(g_peers, seen);
            if (seen.size() != g_peers.size()) {
                log::write("online", "%zu other player(s) connected", seen.size());
                g_version++;
            }
            g_peers = std::move(seen);
        }
        std::this_thread::sleep_for(kPumpEvery);
    }
}

bool wait(const std::shared_ptr<Session>& session, Clock::duration duration) {
    const Clock::time_point end = Clock::now() + duration;
    while (Clock::now() < end) {
        if (!current(session)) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return current(session);
}

bool wait_for_player(const std::shared_ptr<Session>& session, Clock::duration duration) {
    const Clock::time_point end = Clock::now() + duration;
    while (Clock::now() < end && current(session)) {
        if (others() > 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

std::string own_endpoints(const std::shared_ptr<Session>& session, const std::vector<std::string>& servers) {
    const Clock::time_point end = Clock::now() + kStunPatience;
    while (Clock::now() < end && current(session)) {
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->link.mapped().empty()) {
                if (session->link.symmetric()) {
                    log::write("online", "this network changes the port for each destination");
                }
                return session->link.mapped() + " " + session->link.local();
            }
            session->link.ask_stun(servers);
        }
        std::this_thread::sleep_for(kStunEvery);
    }
    return "";
}

bool call(const std::shared_ptr<Session>& session, const char* method, const std::string& path, const std::string& body, int& status, std::string& reply) {
    for (int attempt = 0; attempt < kNetworkRetries; attempt++) {
        if (cloud::room_request(method, path, body, status, reply) && status < 500) {
            return true;
        }
        if (!wait(session, std::chrono::seconds(1))) {
            return false;
        }
    }
    return false;
}

void add_targets(const std::shared_ptr<Session>& session, const std::string& text) {
    std::istringstream words(text);
    std::string endpoint;
    std::lock_guard<std::mutex> lock(session->mutex);
    while (words >> endpoint) {
        session->link.add_target(endpoint, kPunchSeconds);
    }
}

void keep_relay(std::shared_ptr<Session> session, std::string code) {
    int pause = 1;
    while (current(session)) {
        auto socket = std::make_unique<relay::Socket>();
        if (!socket->open(code)) {
            int refused = socket->refused();
            log::write("online", "relay not reachable");
            if (refused == 404) {
                session->relay_kept.store(false);
                return;
            }
            socket.reset();
            if (!wait(session, std::chrono::seconds(pause))) {
                return;
            }
            pause = std::min(pause * 2, 8);
            continue;
        }
        auto opened = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            relay::Socket* raw = socket.get();
            session->relay = std::move(socket);
            session->link.use_relay([raw](const uint8_t* data, size_t size) { raw->send(data, size); });
        }
        log::write("online", "using the relay");
        while (wait(session, std::chrono::milliseconds(500))) {
            std::lock_guard<std::mutex> lock(session->mutex);
            if (!session->relay->alive()) {
                break;
            }
        }
        if (!current(session)) {
            return;
        }
        std::unique_ptr<relay::Socket> old;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->link.use_relay(nullptr);
            old = std::move(session->relay);
        }
        old.reset();
        log::write("online", "relay lost, reconnecting");
        if (std::chrono::steady_clock::now() - opened > std::chrono::seconds(30)) {
            pause = 1;
        } else {
            if (!wait(session, std::chrono::seconds(pause))) {
                return;
            }
            pause = std::min(pause * 2, 8);
        }
    }
}

void open_relay(const std::shared_ptr<Session>& session, const std::string& code) {
    if (!session->relay_kept.exchange(true)) {
        std::thread(keep_relay, session, code).detach();
    }
}

void watch_host(const std::shared_ptr<Session>& session, const std::string& code, const std::string& endpoints) {
    bool losing = false;
    Clock::time_point lost{};
    Clock::time_point next_try{};
    while (wait(session, kWatchEvery)) {
        bool left = false;
        bool present = false;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            left = session->link.host_left();
            present = session->link.host_present();
        }
        if (left) {
            fail(session, Problem::HostLeft);
            return;
        }
        if (present) {
            if (losing) {
                losing = false;
                g_reconnecting.store(false);
                g_version++;
                log::write("online", "host is back");
            }
            continue;
        }
        Clock::time_point now = Clock::now();
        if (!losing) {
            losing = true;
            lost = now;
            next_try = now;
            g_reconnecting.store(true);
            g_version++;
            log::write("online", "host lost, trying to reconnect");
            std::lock_guard<std::mutex> lock(session->mutex);
            session->link.reset_route();
        }
        if (now - lost > kLostPatience) {
            fail(session, Problem::Lost);
            return;
        }
        if (now < next_try) {
            continue;
        }
        next_try = now + kJoinRetryEvery;
        if (code.empty()) {
            add_targets(session, endpoints);
            continue;
        }
        int status = 0;
        std::string reply;
        if (cloud::room_request("POST", "/v1/rooms/" + code + "/join", endpoints, status, reply)) {
            if (status == 404) {
                fail(session, Problem::HostLeft);
                return;
            }
            if (status == 200) {
                add_targets(session, reply);
                open_relay(session, code);
            }
        }
    }
}

void run_host(std::shared_ptr<Session> session) {
    std::vector<std::string> servers = presence::Link::stun_servers();
    std::string endpoints = own_endpoints(session, servers);
    if (endpoints.empty()) {
        log::write("online", "no answer from the address servers");
        set_state(session, State::Hosting, Problem::NoInternet);
        return;
    }
    int status = 0;
    std::string reply;
    if (!call(session, "POST", "/v1/rooms", endpoints, status, reply) || status != 201) {
        log::write("online", "room not created, status %d", status);
        set_state(session, State::Hosting, Problem::Server);
        return;
    }
    std::istringstream parts(reply);
    std::string code;
    std::string token;
    parts >> code >> token;
    if (!valid_code(code) || token.empty()) {
        set_state(session, State::Hosting, Problem::Server);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_session == session) {
            g_code = code;
        }
    }
    log::write("online", "room %s is open", code.c_str());
    set_state(session, State::Hosting);
    const std::string path = "/v1/rooms/" + code + "/" + token;
    while (wait(session, others() > 0 ? kHostPollEvery : kHostSearchEvery)) {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->link.ask_stun(servers);
            body = session->link.mapped() + " " + session->link.local() + " " + std::to_string(session->link.peers().size());
        }
        if (!cloud::room_request("POST", path, body, status, reply)) {
            continue;
        }
        if (status == 200) {
            add_targets(session, reply);
            if (reply.find("relay") != std::string::npos) {
                open_relay(session, code);
            }
        } else if (status == 404) {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_session == session) {
                    g_code.clear();
                }
            }
            set_state(session, State::Hosting, Problem::Server);
            return;
        }
    }
    cloud::room_request("DELETE", path, "", status, reply);
}

void run_join(std::shared_ptr<Session> session, std::string code) {
    std::vector<std::string> servers = presence::Link::stun_servers();
    std::string endpoints = own_endpoints(session, servers);
    if (!current(session)) {
        return;
    }
    if (endpoints.empty()) {
        fail(session, Problem::NoInternet);
        return;
    }
    const std::string path = "/v1/rooms/" + code + "/join";
    bool reached = false;
    for (int attempt = 0; attempt < kJoinAttempts; attempt++) {
        int status = 0;
        std::string reply;
        if (call(session, "POST", path, endpoints, status, reply)) {
            if (status == 404) {
                fail(session, Problem::NoRoom);
                return;
            }
            if (status == 409) {
                fail(session, Problem::Full);
                return;
            }
            if (status == 200) {
                reached = true;
                add_targets(session, reply);
                set_state(session, State::Joined);
            }
        }
        if (!current(session)) {
            return;
        }
        if (wait_for_player(session, kJoinRetryEvery)) {
            watch_host(session, code, endpoints);
            return;
        }
        if (reached && attempt + 1 >= kRelayAfterAttempts) {
            open_relay(session, code);
        }
    }
    fail(session, reached ? Problem::Blocked : Problem::NoInternet);
}

void run_direct(std::shared_ptr<Session> session, std::string endpoint) {
    if (wait_for_player(session, kDirectPatience)) {
        watch_host(session, "", endpoint);
    } else if (current(session)) {
        fail(session, Problem::Blocked);
    }
}

std::shared_ptr<Session> install(std::shared_ptr<Session> session, State state, std::string local, const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        clear_session();
        g_session = session;
        g_local = std::move(local);
        g_own_id = session->link.id();
        g_own_name = name;
        g_other_version.clear();
        g_state.store(state);
        g_problem.store(Problem::None);
        g_version++;
    }
    std::thread(pump_loop, session).detach();
    return session;
}

void refuse(Problem problem) {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_session();
    g_state.store(State::Failed);
    g_problem.store(problem);
    g_version++;
}

struct Shutdown {
    ~Shutdown() {
        std::lock_guard<std::mutex> lock(g_mutex);
        clear_session();
    }
};

Shutdown g_shutdown;

}

bool valid_address(const std::string& address) {
    int parts = 0;
    size_t at = 0;
    while (true) {
        size_t end = address.find('.', at);
        std::string part = address.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos || std::atoi(part.c_str()) > 255) {
            return false;
        }
        parts++;
        if (end == std::string::npos) {
            return parts == 4;
        }
        at = end + 1;
    }
}

bool valid_code(const std::string& code) {
    return code.size() == kCodeLength && code.find_first_not_of(cloud::code_letters()) == std::string::npos;
}

std::string clean_code(const std::string& text) {
    std::string out;
    for (char given : text) {
        char character = static_cast<char>(std::toupper(static_cast<unsigned char>(given)));
        character = character == 'O' ? '0' : (character == 'I' || character == 'L') ? '1' : character;
        if (character != 0 && std::strchr(cloud::code_letters(), character)) {
            out.push_back(character);
        }
    }
    return out;
}

bool host(const std::string& name, uint16_t port, bool use_room) {
    auto session = std::make_shared<Session>();
    if (!session->link.open_room(true, port, name) && (port == 0 || !session->link.open_room(true, 0, name))) {
        log::write("online", "could not open a network port");
        refuse(Problem::Server);
        return false;
    }
    session->hosting = true;
    log::write("online", "hosting as %s on port %u", name.c_str(), session->link.port());
    install(session, use_room ? State::Connecting : State::Hosting, session->link.local(), name);
    if (use_room) {
        std::thread(run_host, session).detach();
    }
    return true;
}

bool join(const std::string& target, const std::string& name, uint16_t port) {
    bool direct = valid_address(target);
    std::string code = direct ? "" : clean_code(target);
    if (!direct && !valid_code(code)) {
        refuse(Problem::NoRoom);
        return false;
    }
    auto session = std::make_shared<Session>();
    if (!session->link.open_room(false, 0, name)) {
        refuse(Problem::Server);
        return false;
    }
    log::write("online", "joining %s as %s", target.c_str(), name.c_str());
    if (direct) {
        session->link.add_target(target + ":" + std::to_string(port), kDirectSeconds);
    }
    install(session, direct ? State::Joined : State::Connecting, session->link.local(), name);
    if (direct) {
        std::thread(run_direct, session, target + ":" + std::to_string(port)).detach();
    } else {
        std::thread(run_join, session, code).detach();
    }
    return true;
}

void leave() {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_session();
    g_problem.store(Problem::None);
    if (g_state.exchange(State::Off) != State::Off) {
        g_version++;
        log::write("online", "left");
    }
}

void start_from_environment() {
    const char* given = std::getenv("WP_ONLINE");
    if (!given) {
        return;
    }
    const char* named = std::getenv("WP_ONLINE_NAME");
    const std::string name = named ? named : "Player";
    const std::string text = given;
    size_t last = text.rfind(':');
    int port = last == std::string::npos ? 0 : std::atoi(text.c_str() + last + 1);
    if (text == "room") {
        host(name);
    } else if (text.rfind("code:", 0) == 0) {
        join(text.substr(5), name);
    } else if (port > 0 && port < 65536 && text.rfind("host:", 0) == 0) {
        host(name, static_cast<uint16_t>(port), false);
    } else if (port > 0 && port < 65536 && text.rfind("join:", 0) == 0 && last > 5) {
        join(text.substr(5, last - 5), name, static_cast<uint16_t>(port));
    }
}

void update(const std::array<float, 3>& position, uint32_t zone, const presence::Pose& pose) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_position = position;
    g_pose = pose;
    g_pose.color = own_color_index();
    g_pose.sent_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
    g_zone = zone;
    g_updated = Clock::now();
}

std::vector<presence::Peer> peers() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peers;
}

State state() {
    return g_state.load();
}

Problem problem() {
    return g_problem.load();
}

bool running() {
    State now = state();
    return now == State::Connecting || now == State::Hosting || now == State::Joined;
}

size_t others() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_peers.size();
}

std::string room_code() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_code;
}

std::string local_address() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_local;
}

uint64_t version() {
    return g_version.load();
}

std::vector<Notice> take_notices() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Notice> notices;
    notices.swap(g_notices);
    return notices;
}

std::string other_version() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_other_version;
}

bool reconnecting() {
    return g_reconnecting.load();
}

void say_goodbye() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_session) {
        return;
    }
    std::lock_guard<std::mutex> session_lock(g_session->mutex);
    g_session->link.goodbye();
    if (g_session->relay) {
        g_session->relay->flush();
    }
}

uint32_t player_color(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<std::pair<uint32_t, uint32_t>> players;
    if (g_own_id != 0) {
        players.push_back({g_own_id, g_pose.color});
    }
    for (const presence::Peer& peer : g_peers) {
        players.push_back({peer.id, peer.pose.color});
    }
    if (std::none_of(players.begin(), players.end(), [&](const auto& player) { return player.first == id; })) {
        players.push_back({id, 0});
    }
    std::sort(players.begin(), players.end());
    bool taken[kChosenColors.size()] = {};
    std::vector<uint32_t> given(players.size(), 0);
    for (size_t i = 0; i < players.size(); i++) {
        uint32_t wanted = players[i].second;
        if (wanted != 0 && wanted < kChosenColors.size() && !taken[wanted]) {
            taken[wanted] = true;
            given[i] = wanted;
        }
    }
    for (size_t i = 0; i < players.size(); i++) {
        if (given[i] != 0) {
            continue;
        }
        uint32_t free = 1;
        while (free + 1 < kChosenColors.size() && taken[free]) {
            free++;
        }
        taken[free] = true;
        given[i] = free;
    }
    for (size_t i = 0; i < players.size(); i++) {
        if (players[i].first == id) {
            return kChosenColors[given[i]];
        }
    }
    return kChosenColors[1];
}

uint32_t own_id() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_own_id;
}

void own_state(std::string& name, uint32_t& character, uint32_t& zone) {
    std::lock_guard<std::mutex> lock(g_mutex);
    name = g_own_name;
    character = g_pose.character;
    zone = g_zone;
}

}
