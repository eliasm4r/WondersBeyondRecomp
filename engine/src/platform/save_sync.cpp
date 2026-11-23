#include "wp/save_sync.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <vector>

#include "wp/save_backup.h"

namespace wp::saves {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t kHashStart = 14695981039346656037ull;
constexpr uint64_t kHashStep = 1099511628211ull;
constexpr size_t kChunkSize = 64 * 1024;

fs::path with_suffix(fs::path path, const char* suffix) {
    path += suffix;
    return path;
}

bool describe(const fs::path& root, std::string& out) {
    std::error_code error;
    std::vector<std::string> lines;
    std::vector<char> buffer(kChunkSize);
    fs::recursive_directory_iterator it(root, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (it->is_directory(error)) {
            continue;
        }
        std::ifstream file(it->path(), std::ios::binary);
        if (error || !file) {
            return false;
        }
        uint64_t hash = kHashStart;
        uint64_t size = 0;
        while (file.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) || file.gcount() > 0) {
            size_t count = static_cast<size_t>(file.gcount());
            for (size_t i = 0; i < count; i++) {
                hash = (hash ^ static_cast<unsigned char>(buffer[i])) * kHashStep;
            }
            size += count;
            if (file.eof()) {
                break;
            }
        }
        if (file.bad()) {
            return false;
        }
        lines.push_back(it->path().lexically_relative(root).generic_string() + " " + std::to_string(size) + " " + std::to_string(hash) + "\n");
    }
    if (error) {
        return false;
    }
    std::sort(lines.begin(), lines.end());
    out.clear();
    for (const std::string& line : lines) {
        out += line;
    }
    return true;
}

bool has_files(const fs::path& root) {
    std::error_code error;
    fs::recursive_directory_iterator it(root, error);
    for (; !error && it != fs::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_directory(error)) {
            return true;
        }
    }
    return false;
}

bool replace_tree(const fs::path& from, const fs::path& to) {
    std::error_code error;
    fs::path partial = with_suffix(to, ".partial");
    fs::path old = with_suffix(to, ".old");
    fs::remove_all(partial, error);
    fs::remove_all(old, error);
    fs::create_directories(to.parent_path(), error);
    fs::copy(from, partial, fs::copy_options::recursive, error);
    if (error || !same_contents(from, partial)) {
        fs::remove_all(partial, error);
        return false;
    }
    bool present = fs::exists(to, error);
    if (present) {
        fs::rename(to, old, error);
        if (error) {
            fs::remove_all(partial, error);
            return false;
        }
    }
    fs::rename(partial, to, error);
    if (error) {
        if (present) {
            fs::rename(old, to, error);
        }
        fs::remove_all(partial, error);
        return false;
    }
    fs::remove_all(old, error);
    return true;
}

bool write_manifest(const fs::path& remote) {
    std::string text;
    if (!describe(remote, text)) {
        return false;
    }
    fs::path manifest = with_suffix(remote, ".manifest");
    fs::path partial = with_suffix(manifest, ".partial");
    {
        std::ofstream file(partial, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file.flush()) {
            return false;
        }
    }
    std::error_code error;
    fs::rename(partial, manifest, error);
    return !error;
}

bool complete(const fs::path& remote) {
    std::ifstream file(with_suffix(remote, ".manifest"), std::ios::binary);
    if (!file) {
        return false;
    }
    std::string wanted((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string found;
    return describe(remote, found) && found == wanted;
}

SyncOutcome done(SyncResult result, const std::string& message) {
    SyncOutcome outcome;
    outcome.result = result;
    outcome.message = message;
    return outcome;
}

SyncOutcome push(const fs::path& save, const fs::path& remote, const fs::path& base) {
    if (!replace_tree(save, remote) || !write_manifest(remote)) {
        return done(SyncResult::Failed, "cannot copy the save to the sync folder");
    }
    if (!replace_tree(save, base)) {
        return done(SyncResult::Failed, "the save was sent but its sync record could not be updated");
    }
    return done(SyncResult::Pushed, "sent the save to the sync folder");
}

SyncOutcome pull(const fs::path& nand_root, const fs::path& relative, const fs::path& backups_root, const fs::path& shared, const fs::path& base, int keep,
                 std::time_t now) {
    Outcome restored = restore(nand_root, backups_root, Backup{"sync folder", shared}, keep, now);
    if (restored.result != Result::Created && restored.result != Result::Unchanged) {
        return done(SyncResult::Failed, restored.message);
    }
    if (!replace_tree(nand_root / relative, base)) {
        return done(SyncResult::Failed, "the save was received but its sync record could not be updated");
    }
    return done(SyncResult::Pulled, "received the newer save from the sync folder");
}

SyncOutcome sync_unguarded(const fs::path& nand_root, const fs::path& relative, const fs::path& backups_root, const fs::path& folder, bool may_pull, int keep,
                           std::time_t now) {
    std::error_code error;
    fs::path save = nand_root / relative;
    fs::path shared = folder / relative.parent_path().filename();
    fs::path remote = shared / relative;
    fs::path base = backups_root.parent_path() / "sync_base" / relative;
    if (!fs::is_directory(folder, error)) {
        return done(SyncResult::Failed, "the sync folder is not available");
    }
    if (!fs::is_directory(save, error)) {
        return done(SyncResult::Failed, "no save folder");
    }
    bool local = has_files(save);
    if (!fs::exists(remote, error)) {
        return local ? push(save, remote, base) : done(SyncResult::Same, "nothing to sync yet");
    }
    if (!complete(remote)) {
        return done(SyncResult::Waiting, "the save in the sync folder has not finished arriving");
    }
    if (same_contents(save, remote)) {
        if (!same_contents(save, base) && !replace_tree(save, base)) {
            return done(SyncResult::Failed, "cannot update the sync record");
        }
        return done(SyncResult::Same, "the save matches the sync folder");
    }
    if (!local) {
        return may_pull ? pull(nand_root, relative, backups_root, shared, base, keep, now) : done(SyncResult::Waiting, "a save is waiting in the sync folder");
    }
    if (fs::is_directory(base, error)) {
        if (same_contents(save, base)) {
            return may_pull ? pull(nand_root, relative, backups_root, shared, base, keep, now)
                            : done(SyncResult::Waiting, "a newer save is waiting in the sync folder");
        }
        if (same_contents(remote, base)) {
            return push(save, remote, base);
        }
    }
    return done(SyncResult::Conflict, "this device and the sync folder both hold different progress, so neither was changed");
}

}

SyncOutcome sync(const fs::path& nand_root, const fs::path& relative, const fs::path& backups_root, const fs::path& folder, bool may_pull, int keep,
                 std::time_t now) {
    try {
        return sync_unguarded(nand_root, relative, backups_root, folder, may_pull, keep, now);
    } catch (const std::exception& exception) {
        return done(SyncResult::Failed, exception.what());
    }
}

}
