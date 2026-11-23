#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace wp::cloud {

enum class Status { Off, Working, Synced, Offline, Restart, Conflict, Unknown, Error };

void start(const std::filesystem::path& nand_root, const std::filesystem::path& relative, const std::filesystem::path& backups, int keep);
void saved();
void create();
void link(const std::string& code);
void keep_here();
void keep_other();
void disconnect();

Status status();
std::string code();
uint64_t version();
bool busy();
uint32_t other_seconds();
std::string other_device();
const char* code_letters();
bool room_request(const char* method, const std::string& path, const std::string& body, int& status, std::string& reply);

}
