#pragma once

// What differs between operating systems, apart from processes and the terminal.
// Text crossing this boundary is UTF-8 on every platform.
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hirari::platform {

#ifdef _WIN32
inline constexpr char path_list_separator = ';';
inline constexpr const char* shell_file_name = "hirari.exe";
using ArgumentText = wchar_t*;
#else
inline constexpr char path_list_separator = ':';
inline constexpr const char* shell_file_name = "hirari";
using ArgumentText = char*;
#endif

std::string argument_text(ArgumentText argument);
std::filesystem::path native_path(const std::string& text);
// Rejects what can't be source text here.
void require_text(std::string_view text);

// nullopt when the variable is unset, as distinct from set and empty.
std::optional<std::string> environment(const std::string& name);
void set_environment(const std::string& name, const std::optional<std::string>& value);

// Empty when it can't be found.
std::filesystem::path own_executable();
std::string home_directory();
std::filesystem::path default_state_directory();
unsigned long process_id();

// The full path of a program, by PATH lookup or as named. Empty when there is none.
std::string find_executable(std::string_view name);
// The name a PATH entry runs under, or empty when it isn't a program.
std::string program_name(const std::filesystem::directory_entry& entry);
// A tool the system itself ships, by full path, so an earlier one on PATH never stands in. Empty when absent.
std::filesystem::path system_tool(const char* name);

struct Ran { int status; std::string output; };
// Runs a program with its stdout and stderr gathered in `output`, killing it after the timeout.
// Throws when it can't be started or doesn't finish in time.
Ran run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments,
                const std::filesystem::path& output, unsigned timeout_ms);

// A file held exclusively for this object's life, and gone afterwards.
class HeldFile {
public:
    HeldFile();
    ~HeldFile();
    // False with an empty `failure` when another holder has it; false with a reason when it can't be opened.
    bool acquire(const std::filesystem::path& file, std::string& failure);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hirari::platform
