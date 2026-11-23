#pragma once

#include <ctime>
#include <filesystem>
#include <string>

namespace wp::saves {

enum class SyncResult { Same, Pulled, Pushed, Waiting, Conflict, Failed };

struct SyncOutcome {
    SyncResult result = SyncResult::Failed;
    std::string message;
};

SyncOutcome sync(const std::filesystem::path& nand_root, const std::filesystem::path& relative, const std::filesystem::path& backups_root,
                 const std::filesystem::path& folder, bool may_pull, int keep, std::time_t now);

}
