#include "hirari/terminal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace hirari {

struct TerminalSession::Impl {
    explicit Impl(Host& terminal) : host(terminal) {}
    Host& host;
    std::atomic_bool running{false}, stopping{false};
    int master = -1;
    pid_t child = -1;
    std::mutex input_mutex;
    std::condition_variable input_ready;
    std::string pending_input;
    bool input_stopping = false;
    std::thread reader, writer, waiter;
    ExitHandler exited;

    void stop_input() {
        {
            std::lock_guard lock(input_mutex);
            input_stopping = true;
            pending_input.clear();
        }
        input_ready.notify_all();
        if (writer.joinable()) writer.join();
    }
    void close_master() {
        if (master >= 0) ::close(master);
        master = -1;
    }
};

TerminalSession::TerminalSession(Host& host) : impl_(std::make_unique<Impl>(host)) {}
TerminalSession::~TerminalSession() { close(); }

bool TerminalSession::running() const noexcept { return impl_->running.load(); }

void TerminalSession::start(const std::string& executable,
                            const std::vector<std::string>& arguments,
                            std::uint16_t columns, std::uint16_t rows,
                            ExitHandler exited) {
    close();
    std::vector<std::string> words{executable};
    words.insert(words.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& word : words) {
        if (word.find('\0') != std::string::npos) throw std::runtime_error("NUL is not valid in a native argument");
        argv.push_back(word.data());
    }
    argv.push_back(nullptr);

    // Reports why exec failed; closes unread when it succeeds.
    int report[2];
    if (pipe2(report, O_CLOEXEC)) throw std::runtime_error(std::string("cannot make a pipe: ") + std::strerror(errno));

    winsize window{};
    window.ws_col = std::max<std::uint16_t>(columns, 1);
    window.ws_row = std::max<std::uint16_t>(rows, 1);
    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &window);
    if (child < 0) {
        const std::string why = std::strerror(errno);
        ::close(report[0]);
        ::close(report[1]);
        throw std::runtime_error("cannot open a pseudoterminal: " + why);
    }
    if (child == 0) {
        for (int number : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGTTIN, SIGTTOU, SIGWINCH, SIGTSTP}) signal(number, SIG_DFL);
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, nullptr);
        execve(argv[0], argv.data(), environ);
        const int reason = errno;
        (void)!::write(report[1], &reason, sizeof(reason));
        _exit(127);
    }
    ::close(report[1]);
    int reason = 0;
    ssize_t got;
    while ((got = read(report[0], &reason, sizeof(reason))) < 0 && errno == EINTR) {}
    ::close(report[0]);
    if (got == static_cast<ssize_t>(sizeof(reason))) {
        int status;
        waitpid(child, &status, 0);
        ::close(master);
        throw std::runtime_error("cannot start " + executable + ": " + std::strerror(reason));
    }
    fcntl(master, F_SETFD, FD_CLOEXEC);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);

    impl_->master = master;
    impl_->child = child;
    impl_->exited = std::move(exited);
    impl_->input_stopping = false;
    impl_->stopping.store(false);
    impl_->running.store(true);
    impl_->reader = std::thread([this] {
        char buffer[8192];
        for (;;) {
            pollfd ready{impl_->master, POLLIN, 0};
            const int events = poll(&ready, 1, 20);
            if (events == 0) { if (impl_->stopping.load()) return; continue; }
            if (events < 0) { if (errno == EINTR) continue; return; }
            const auto count = read(impl_->master, buffer, sizeof(buffer));
            if (count > 0) impl_->host.write(std::string_view(buffer, static_cast<std::size_t>(count)));
            else if (count == 0 || (errno != EAGAIN && errno != EINTR)) return;  // EIO: every program on the terminal has gone.
        }
    });
    impl_->writer = std::thread([this] {
        for (;;) {
            std::string input;
            {
                std::unique_lock lock(impl_->input_mutex);
                impl_->input_ready.wait(lock, [this] { return impl_->input_stopping || !impl_->pending_input.empty(); });
                if (impl_->input_stopping) return;
                input.swap(impl_->pending_input);
            }
            std::size_t offset = 0;
            while (offset < input.size()) {
                {
                    std::lock_guard lock(impl_->input_mutex);
                    if (impl_->input_stopping) return;
                }
                const auto count = ::write(impl_->master, input.data() + offset, input.size() - offset);
                if (count > 0) { offset += static_cast<std::size_t>(count); continue; }
                if (errno == EAGAIN || errno == EINTR) {
                    pollfd ready{impl_->master, POLLOUT, 0};
                    poll(&ready, 1, 20);
                    continue;
                }
                break;
            }
        }
    });
    impl_->host.foreground(true, 0);
    impl_->waiter = std::thread([this] {
        int status = 0;
        while (waitpid(impl_->child, &status, 0) < 0 && errno == EINTR) {}
        const int result = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        // The program's foreground group ends with it; output already written is still read.
        kill(-impl_->child, SIGKILL);
        impl_->stopping.store(true);
        impl_->stop_input();
        if (impl_->reader.joinable()) impl_->reader.join();
        impl_->close_master();
        impl_->host.foreground(false, result);
        if (impl_->exited) impl_->exited(result);
        // Publish completion only after output and callbacks finish.
        impl_->running.store(false);
    });
}

void TerminalSession::write(std::string_view bytes) {
    if (!running() || bytes.empty()) return;
    {
        std::lock_guard lock(impl_->input_mutex);
        if (impl_->input_stopping) return;
        impl_->pending_input.append(bytes);
    }
    impl_->input_ready.notify_one();
}

void TerminalSession::resize(std::uint16_t columns, std::uint16_t rows) {
    if (!running() || impl_->master < 0) return;
    winsize window{};
    window.ws_col = std::max<std::uint16_t>(columns, 1);
    window.ws_row = std::max<std::uint16_t>(rows, 1);
    ioctl(impl_->master, TIOCSWINSZ, &window);
}

void TerminalSession::interrupt() { write("\x03"); }

void TerminalSession::close() {
    if (impl_->child > 0 && impl_->running.load()) kill(impl_->child, SIGKILL);
    // Only the waiter operates the live session's I/O teardown.
    if (impl_->waiter.joinable()) impl_->waiter.join();
    impl_->running.store(false);
    impl_->stopping.store(true);
    impl_->stop_input();
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->close_master();
    impl_->child = -1;
}

} // namespace hirari
