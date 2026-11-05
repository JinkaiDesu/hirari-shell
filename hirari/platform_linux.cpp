#include "hirari/platform.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hirari::platform {
namespace fs = std::filesystem;

std::string argument_text(ArgumentText argument) { return argument; }
fs::path native_path(const std::string& text) { return fs::path(text); }

void require_text(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) throw std::runtime_error("NUL is not valid in text");
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const std::size_t length = lead < 0x80 ? 1 : lead >= 0xc2 && lead < 0xe0 ? 2 : lead >= 0xe0 && lead < 0xf0 ? 3 : lead >= 0xf0 && lead < 0xf5 ? 4 : 0;
        if (!length || i + length > text.size()) throw std::runtime_error("invalid UTF-8");
        for (std::size_t k = 1; k < length; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xc0) != 0x80) throw std::runtime_error("invalid UTF-8");
        i += length;
    }
}

std::optional<std::string> environment(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (!value) return std::nullopt;
    return std::string(value);
}

void set_environment(const std::string& name, const std::optional<std::string>& value) {
    const int failed = value ? setenv(name.c_str(), value->c_str(), 1) : unsetenv(name.c_str());
    if (failed) throw std::runtime_error("cannot set environment variable: " + name);
}

fs::path own_executable() {
    std::error_code error;
    auto file = fs::read_symlink("/proc/self/exe", error);
    return error ? fs::path{} : file;
}

std::string home_directory() { return environment("HOME").value_or(std::string{}); }

fs::path default_state_directory() {
    if (auto state = environment("XDG_STATE_HOME"); state && !state->empty()) return fs::path(*state) / "hirari" / "vocabulary";
    auto home = home_directory();
    return home.empty() ? fs::path{} : fs::path(home) / ".local" / "state" / "hirari" / "vocabulary";
}

unsigned long process_id() { return static_cast<unsigned long>(getpid()); }

namespace {
bool runnable(const fs::path& file) {
    std::error_code error;
    return fs::is_regular_file(file, error) && access(file.c_str(), X_OK) == 0;
}
}

std::string find_executable(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos) return {};
    const std::string wanted(name);
    if (wanted.find('/') != std::string::npos) {
        std::error_code error;
        auto full = fs::absolute(fs::path(wanted), error);
        return !error && runnable(full) ? full.string() : std::string{};
    }
    std::istringstream roots(environment("PATH").value_or(std::string{}));
    for (std::string root; std::getline(roots, root, path_list_separator);)
        if (!root.empty() && runnable(fs::path(root) / wanted)) return (fs::path(root) / wanted).string();
    return {};
}

std::string program_name(const fs::directory_entry& entry) {
    return runnable(entry.path()) ? entry.path().filename().string() : std::string{};
}

fs::path system_tool(const char* name) {
    for (const char* folder : {"/usr/bin", "/bin"})
        if (auto tool = fs::path(folder) / name; runnable(tool)) return tool;
    return {};
}

Ran run_program(const fs::path& program, const std::vector<std::string>& arguments, const fs::path& output, unsigned timeout_ms) {
    std::vector<std::string> words{program.string()};
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);

    int sink = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int nothing = open("/dev/null", O_RDONLY | O_CLOEXEC);
    int report[2] = {-1, -1};
    if (sink < 0 || nothing < 0 || pipe2(report, O_CLOEXEC)) {
        const std::string why = std::strerror(errno);
        if (sink >= 0) close(sink);
        if (nothing >= 0) close(nothing);
        throw std::runtime_error("cannot open " + output.string() + ": " + why);
    }
    const pid_t child = fork();
    if (child == 0) {
        signal(SIGPIPE, SIG_DFL);
        dup2(nothing, 0);
        dup2(sink, 1);
        dup2(sink, 2);
        execv(argv[0], argv.data());
        const int reason = errno;
        (void)!write(report[1], &reason, sizeof(reason));
        _exit(127);
    }
    close(sink);
    close(nothing);
    close(report[1]);
    int reason = 0;
    const bool failed_to_start = child > 0 && read(report[0], &reason, sizeof(reason)) == static_cast<ssize_t>(sizeof(reason));
    close(report[0]);
    if (child < 0) throw std::runtime_error("cannot run " + program.string() + ": " + std::strerror(errno));

    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        const pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child || (done < 0 && errno != EINTR)) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(child, SIGKILL);
            waitpid(child, &status, 0);
            throw std::runtime_error(program.string() + " did not finish in time");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (failed_to_start) throw std::runtime_error("cannot run " + program.string() + ": " + std::strerror(reason));
    std::ifstream in(output, std::ios::binary);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status), std::string(std::istreambuf_iterator<char>(in), {})};
}

struct HeldFile::Impl {
    int descriptor = -1;
    fs::path file;
    ~Impl() {
        if (descriptor < 0) return;
        std::error_code ignored;
        fs::remove(file, ignored);
        close(descriptor);
    }
};
HeldFile::HeldFile() : impl_(std::make_unique<Impl>()) {}
HeldFile::~HeldFile() = default;
bool HeldFile::acquire(const fs::path& file, std::string& failure) {
    const int descriptor = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) { failure = std::strerror(errno); return false; }
    if (flock(descriptor, LOCK_EX | LOCK_NB)) {
        const int reason = errno;
        close(descriptor);
        if (reason != EWOULDBLOCK) failure = std::strerror(reason);
        return false;
    }
    impl_->descriptor = descriptor;
    impl_->file = file;
    return true;
}

} // namespace hirari::platform
