// Linux realization of `{ }` pipelines and native programs: one process group, pipes and exec.
#include "hirari/process.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace hirari {
namespace {
using Clock = std::chrono::steady_clock;

[[noreturn]] void system_failure(const std::string& what) { throw std::runtime_error(what + ": " + std::strerror(errno)); }

class Descriptor {
public:
    Descriptor() = default;
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { reset(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    int get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ >= 0; }
    void reset() noexcept { if (value_ >= 0) close(value_); value_ = -1; }
private:
    int value_ = -1;
};

// Above the three standard numbers, so moving a stage's streams into place never overwrites one of them.
Descriptor kept(int descriptor, const char* what) {
    if (descriptor < 0) system_failure(what);
    if (descriptor > 2) return Descriptor(descriptor);
    const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    const int reason = errno;
    close(descriptor);
    errno = reason;
    if (moved < 0) system_failure(what);
    return Descriptor(moved);
}

struct Pipe { Descriptor read, write; };

Pipe make_pipe() {
    int ends[2];
    if (pipe2(ends, O_CLOEXEC)) system_failure("cannot make a pipe");
    Pipe result;
    result.read = kept(ends[0], "cannot make a pipe");
    result.write = kept(ends[1], "cannot make a pipe");
    return result;
}

void non_blocking(const Descriptor& descriptor) {
    const int flags = fcntl(descriptor.get(), F_GETFL);
    if (flags < 0 || fcntl(descriptor.get(), F_SETFL, flags | O_NONBLOCK)) system_failure("cannot configure a pipe");
}

struct Spawned { pid_t pid = -1; bool complete = false; int status = 0; bool signalled = false; };

// Every stage and whatever it started: they end with the pipeline, whichever way it ends.
struct ProcessGroup {
    pid_t id = 0;
    std::vector<Spawned>* members = nullptr;
    void signal_all(int number) const { if (id > 0) kill(-id, number); }
    ~ProcessGroup() {
        signal_all(SIGKILL);
        if (!members) return;
        for (auto& member : *members) {
            if (member.complete) continue;
            int status;
            while (waitpid(member.pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
};

// While a top-level pipeline runs its group owns the terminal, so Ctrl+C and /dev/tty reach its stages directly.
struct TerminalOwner {
    bool held = false;
    void take(pid_t group) {
        if (!isatty(STDIN_FILENO) || tcgetpgrp(STDIN_FILENO) != getpgrp()) return;
        signal(SIGTTOU, SIG_IGN);
        held = tcsetpgrp(STDIN_FILENO, group) == 0;
    }
    ~TerminalOwner() { if (held) tcsetpgrp(STDIN_FILENO, getpgrp()); }
};

// Takes everything available. False once the pipe has reached its end.
bool drain(Descriptor& pipe, std::string& output) {
    char buffer[65536];
    for (int batch = 0; batch < 16; ++batch) {
        const auto count = read(pipe.get(), buffer, sizeof(buffer));
        if (count > 0) { output.append(buffer, static_cast<std::size_t>(count)); continue; }
        if (count == 0) return false;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return true;
        system_failure("cannot read a pipeline's output");
    }
    return true;
}

// Only async-signal-safe calls between fork and exec.
[[noreturn]] void become(const char* executable, char* const* argv, int input, int output, int error, pid_t group,
                         const char* directory, bool terminal_group, int report, pid_t parent) {
    setpgid(0, group);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parent) _exit(127);
    for (int number : {SIGPIPE, SIGINT, SIGQUIT, SIGTERM, SIGTTIN, SIGTTOU, SIGWINCH, SIGTSTP}) signal(number, SIG_DFL);
    // hirari has no job control to bring a stopped stage back, so a pipeline can't be stopped from the keyboard.
    if (terminal_group) signal(SIGTSTP, SIG_IGN);
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    dup2(input, STDIN_FILENO);
    dup2(output, STDOUT_FILENO);
    dup2(error, STDERR_FILENO);
    if (directory && chdir(directory)) goto failed;
    execve(executable, argv, environ);
failed:
    const int reason = errno;
    (void)!write(report, &reason, sizeof(reason));
    _exit(127);
}

int exit_status(int status, bool& signalled) {
    signalled = WIFSIGNALED(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
} // namespace

seed::Ptr ProcessHost::run_pipeline(
    seed::Runtime& runtime,
    std::vector<Stage> stages, bool passthrough,
    const std::string& output_file, bool append
) {
    if (stages.empty()) return runtime.arena().unit();
    // A stage that has gone answers this process's writes with EPIPE; each stage itself starts with the usual SIGPIPE.
    static const bool broken_pipes_are_errors = (signal(SIGPIPE, SIG_IGN), true);
    (void)broken_pipes_are_errors;

    std::vector<Pipe> links;
    for (std::size_t i = 0; i + 1 < stages.size(); ++i) links.push_back(make_pipe());

    Pipe stdout_capture = make_pipe(), stderr_capture = make_pipe();
    non_blocking(stdout_capture.read);
    non_blocking(stderr_capture.read);

    Descriptor file_output;
    if (!output_file.empty()) {
        file_output = kept(open(output_file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC), 0666),
                           ("cannot write " + output_file).c_str());
    }

    Descriptor first_input, input_write;
    if (!stages.front().input_file.empty()) {
        first_input = kept(open(stages.front().input_file.c_str(), O_RDONLY | O_CLOEXEC), ("cannot read " + stages.front().input_file).c_str());
    } else if (stages.front().input) {
        auto input_pipe = make_pipe();
        first_input = std::move(input_pipe.read);
        input_write = std::move(input_pipe.write);
        non_blocking(input_write);
    } else if (passthrough && streaming_) {
        first_input = kept(fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3), "cannot share stdin");
    } else {
        first_input = kept(open("/dev/null", O_RDONLY | O_CLOEXEC), "cannot open /dev/null");
    }

    // A job's worker never touches the terminal; only the shell's own thread does.
    const bool foreground_terminal = allow_breakaway_ && host_.terminal_capable();

    std::vector<Spawned> processes;
    processes.reserve(stages.size());
    ProcessGroup group;
    group.members = &processes;
    TerminalOwner terminal;

    const pid_t parent = getpid();
    auto spawn_stage = [&](const Stage& stage, int input, int output, int error) {
        if (!stage.program) throw std::runtime_error("pipeline stage has no program");
        for (const auto& argument : stage.arguments)
            if (argument.find('\0') != std::string::npos) throw std::runtime_error("NUL is not valid in a native argument");
        std::vector<std::string> words{stage.program->executable};
        words.insert(words.end(), stage.arguments.begin(), stage.arguments.end());
        std::vector<char*> argv;
        for (auto& word : words) argv.push_back(word.data());
        argv.push_back(nullptr);

        // Reports why exec failed; closes unread when it succeeds.
        int report[2];
        if (pipe2(report, O_CLOEXEC)) system_failure("cannot make a pipe");
        Descriptor report_read = kept(report[0], "cannot make a pipe"), report_write = kept(report[1], "cannot make a pipe");
        const char* directory = working_directory_.empty() ? nullptr : working_directory_.c_str();

        const pid_t child = fork();
        if (child < 0) system_failure("cannot start " + stage.program->executable);
        if (child == 0)
            become(argv[0], argv.data(), input, output, error, group.id, directory, foreground_terminal, report_write.get(), parent);

        setpgid(child, group.id ? group.id : child);
        if (!group.id) group.id = child;
        processes.push_back({child});
        report_write.reset();
        int reason = 0;
        ssize_t got;
        while ((got = read(report_read.get(), &reason, sizeof(reason))) < 0 && errno == EINTR) {}
        if (got == static_cast<ssize_t>(sizeof(reason)))
            throw std::runtime_error("cannot start " + stage.program->executable + ": " + std::strerror(reason));
        if (spawn_observer_) spawn_observer_(static_cast<unsigned long>(child));
    };

    for (std::size_t i = 0; i < stages.size(); ++i) {
        const int input = i == 0 ? first_input.get() : links[i - 1].read.get();
        const int output = i + 1 == stages.size()
            ? (file_output ? file_output.get() : stdout_capture.write.get())
            : links[i].write.get();
        spawn_stage(stages[i], input, output, stderr_capture.write.get());
    }
    if (foreground_terminal) {
        terminal.take(group.id);
        // A stage that reached the terminal before it was handed over stopped there.
        group.signal_all(SIGCONT);
    }

    // Children now own the necessary descriptors. Parent copies must disappear so EOF propagates.
    first_input.reset();
    file_output.reset();

    // Feed materialized answers while stdout/stderr are drained, so large input
    // cannot deadlock against a consumer writing output.
    std::thread input_writer;
    std::atomic_bool input_stop{false};
    std::atomic_int input_error{0};
    struct InputCleanup {
        std::thread& writer; std::atomic_bool& stop;
        ~InputCleanup() { stop.store(true); if (writer.joinable()) writer.join(); }
    } input_cleanup{input_writer, input_stop};
    if (stages.front().input) {
        input_writer = std::thread([pipe = std::move(input_write), bytes = std::move(stages.front().input->out), &input_stop, &input_error]() mutable {
            std::size_t offset = 0;
            while (offset < bytes.size() && !input_stop.load()) {
                const auto written = write(pipe.get(), bytes.data() + offset, std::min<std::size_t>(65536, bytes.size() - offset));
                if (written > 0) { offset += static_cast<std::size_t>(written); continue; }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    pollfd ready{pipe.get(), POLLOUT, 0};
                    poll(&ready, 1, 50);
                    continue;
                }
                if (errno == EINTR) continue;
                if (errno != EPIPE) input_error = errno;
                break;
            }
        });
    }

    for (Pipe& link : links) { link.read.reset(); link.write.reset(); }
    stdout_capture.write.reset();
    stderr_capture.write.reset();

    std::string out;
    std::string err = stages.front().input ? stages.front().input->err : std::string{};
    bool out_open = true, err_open = true;

    auto drain_both = [&] {
        auto old_out = out.size(), old_err = err.size();
        if (out_open) out_open = drain(stdout_capture.read, out);
        if (err_open) err_open = drain(stderr_capture.read, err);
        if (output_observer_) {
            output_observer_(std::string_view(out).substr(old_out), false);
            output_observer_(std::string_view(err).substr(old_err), true);
        }
        if (passthrough) {
            if (!out.empty()) host_.write(out);
            if (!err.empty()) host_.error(err);
            out.clear();
            err.clear();
        }
    };

    std::size_t remaining = processes.size();
    // When the final stage has exited, upstream writers have no reader. Most end on the broken pipe;
    // one that ignores it is ended after a grace period so it cannot hold the pipeline open.
    std::vector<bool> severed(processes.size(), false);
    std::optional<Clock::time_point> consumer_exited, interrupt_sent;
    bool interrupt_forced = false;
    constexpr auto grace = std::chrono::milliseconds(2000);

    while (remaining > 0) {
        if (processes.size() > 1 && processes.back().complete) {
            if (!consumer_exited) consumer_exited = Clock::now();
            else if (Clock::now() - *consumer_exited > grace)
                for (std::size_t i = 0; i + 1 < processes.size(); ++i)
                    if (!processes[i].complete && !severed[i]) { severed[i] = true; kill(processes[i].pid, SIGKILL); }
        }
        if (host_.interrupted() && !interrupt_sent) {
            interrupt_sent = Clock::now();
            group.signal_all(SIGINT);
        }
        if (interrupt_sent && !interrupt_forced && Clock::now() - *interrupt_sent > grace) {
            interrupt_forced = true;
            group.signal_all(SIGKILL);
        }

        pollfd ready[2];
        nfds_t count = 0;
        if (out_open) ready[count++] = {stdout_capture.read.get(), POLLIN, 0};
        if (err_open) ready[count++] = {stderr_capture.read.get(), POLLIN, 0};
        poll(ready, count, 5);
        drain_both();
        if (out.size() > 16 * 1024 * 1024 || err.size() > 16 * 1024 * 1024)
            throw std::runtime_error("captured output exceeds 16 MiB; use live streams or an artifact file");

        for (Spawned& process : processes) {
            if (process.complete) continue;
            int status = 0;
            const pid_t done = waitpid(process.pid, &status, WNOHANG);
            if (done == 0 || (done < 0 && errno == EINTR)) continue;
            if (done < 0) system_failure("cannot wait for a pipeline stage");
            process.status = exit_status(status, process.signalled);
            process.complete = true;
            --remaining;
        }
    }

    // Descendants cannot retain capture descriptors after the pipeline completes.
    group.signal_all(SIGKILL);
    input_stop.store(true);
    if (input_writer.joinable()) input_writer.join();
    if (input_error.load()) { errno = input_error.load(); system_failure("cannot feed a pipeline's input"); }
    // One that left the group may still hold them open, so this doesn't wait for it for long.
    const auto drain_deadline = Clock::now() + std::chrono::milliseconds(200);
    while ((out_open || err_open) && Clock::now() < drain_deadline) {
        pollfd ready[2];
        nfds_t count = 0;
        if (out_open) ready[count++] = {stdout_capture.read.get(), POLLIN, 0};
        if (err_open) ready[count++] = {stderr_capture.read.get(), POLLIN, 0};
        poll(ready, count, 10);
        drain_both();
    }

    for (std::size_t i = 0; i < processes.size(); ++i) {
        auto& process = processes[i];
        if (severed[i]) process.status = 141;
        // A stage ended by the broken pipe was only cut off, as one severed above was.
        else if (process.signalled && process.status == 128 + SIGPIPE && i + 1 < processes.size()) severed[i] = true;
        else if (interrupt_sent && process.signalled) process.status = 130;
    }

    last_status_ = processes.back().status;
    if (last_status_.load() == 0)
        for (std::size_t i = 0; i < processes.size(); ++i)
            if (processes[i].status != 0 && !severed[i]) { last_status_ = processes[i].status; break; }
    if (last_status_.load() == 0 && stages.front().input) last_status_ = stages.front().input->status;
    std::vector<int> statuses;
    if (processes.size() > 1) for (const auto& process : processes) statuses.push_back(process.status);

    return make_result(runtime, std::move(out), std::move(err), last_status_.load(), statuses);
}

} // namespace hirari
