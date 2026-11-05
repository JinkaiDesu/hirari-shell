#pragma once

// What the tests need from the machine they run on.
#include "hirari/platform.hpp"
#include "hirari/world.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include "hirari/win32.hpp"
#else
#include <unistd.h>
#include <sys/types.h>
#endif

namespace hirari::test {

inline const std::string executable_suffix = std::filesystem::path(platform::shell_file_name).extension().string();

// A folder the helper programs live in, ahead of the system's own programs and nothing else.
inline std::string search_path(const std::vector<std::filesystem::path>& first) {
    std::string out;
    for (const auto& folder : first) out += path_text(folder) + platform::path_list_separator;
#ifdef _WIN32
    return out + platform::environment("SystemRoot").value_or("") + "\\System32";
#else
    return out + "/usr/bin" + platform::path_list_separator + "/bin";
#endif
}

// A program of the system's own that succeeds whatever its arguments.
inline const char* native_command() {
#ifdef _WIN32
    return "where";
#else
    return "true";
#endif
}

inline bool alive(unsigned long pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process) return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    // Finished and not yet collected by its parent counts as gone.
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (!std::getline(stat, line)) return false;
    const auto closing = line.rfind(')');
    return closing != std::string::npos && closing + 2 < line.size() && line[closing + 2] != 'Z';
#endif
}

inline std::vector<std::string> arguments(int argc, platform::ArgumentText* argv) {
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.push_back(platform::argument_text(argv[i]));
    return out;
}

} // namespace hirari::test

#ifdef _WIN32
#define HIRARI_TEST_MAIN(body_function) int wmain(int argc, wchar_t** argv) { return body_function(hirari::test::arguments(argc, argv)); }
#else
#define HIRARI_TEST_MAIN(body_function) int main(int argc, char** argv) { return body_function(hirari::test::arguments(argc, argv)); }
#endif
