#include "hirari/terminal.hpp"
#include "hirari/win32.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace hirari {
using namespace win32;

struct TerminalSession::Impl {
    explicit Impl(Host& terminal) : host(terminal) {}
    Host& host;
    std::atomic_bool running{false};
    Handle input_write, output_read, process, job;
    HPCON pseudo = nullptr;
    std::mutex terminal_mutex, input_mutex;
    std::condition_variable input_ready;
    std::string pending_input;
    bool input_stopping = false;
    std::atomic_bool writer_done{true};
    std::thread reader, writer, waiter;
    ExitHandler exited;

    void close_pseudo() {
        std::lock_guard lock(terminal_mutex);
        if (pseudo) {
            ClosePseudoConsole(pseudo);
            pseudo = nullptr;
        }
    }

    void stop_input() {
        {
            std::lock_guard lock(input_mutex);
            input_stopping = true;
            pending_input.clear();
        }
        input_ready.notify_all();
        // The writer never holds a state lock during synchronous pipe I/O.
        // Retry cancellation across the small gap before it enters WriteFile.
        while (writer.joinable() && !writer_done.load()) {
            CancelSynchronousIo(writer.native_handle());
            std::unique_lock lock(input_mutex);
            input_ready.wait_for(lock, std::chrono::milliseconds(10));
        }
        if (writer.joinable()) writer.join();
        input_write.reset();
    }
};

TerminalSession::TerminalSession(Host& host) : impl_(std::make_unique<Impl>(host)) {}
TerminalSession::~TerminalSession() { close(); }

bool TerminalSession::running() const noexcept {
    return impl_->running.load();
}

void TerminalSession::start(const std::string& executable_text,
                         const std::vector<std::string>& arguments,
                         std::uint16_t columns, std::uint16_t rows,
                         ExitHandler exited) {
    const std::wstring executable = widen(executable_text);
    close();
    Handle input_read, output_write;
    HANDLE read = nullptr, write = nullptr;
    // ConPTY communication handles need no inheritance.
    if (!CreatePipe(&read, &write, nullptr, 0))
        throw std::runtime_error(windows_error(GetLastError()));
    input_read.reset(read);
    impl_->input_write.reset(write);
    if (!CreatePipe(&read, &write, nullptr, 0)) {
        impl_->input_write.reset();
        throw std::runtime_error(windows_error(GetLastError()));
    }
    impl_->output_read.reset(read);
    output_write.reset(write);

    COORD size{static_cast<SHORT>(std::clamp<unsigned>(columns, 1, 32767)),
               static_cast<SHORT>(std::clamp<unsigned>(rows, 1, 32767))};
    HRESULT created = CreatePseudoConsole(size, input_read.get(), output_write.get(),
                                         0, &impl_->pseudo);
    if (FAILED(created)) {
        close();
        throw std::runtime_error("CreatePseudoConsole failed (HRESULT " +
                                 std::to_string(static_cast<long>(created)) + ")");
    }
    input_read.reset();
    output_write.reset();

    try {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        std::vector<std::byte> storage(bytes);
        auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
            throw std::runtime_error(windows_error(GetLastError()));
        struct AttributeGuard {
            LPPROC_THREAD_ATTRIBUTE_LIST value;
            ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
        } guard{attributes};
        if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       impl_->pseudo, sizeof(HPCON), nullptr, nullptr))
            throw std::runtime_error(windows_error(GetLastError()));

        impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!impl_->job.get() ||
            !SetInformationJobObject(impl_->job.get(), JobObjectExtendedLimitInformation,
                                     &limits, sizeof(limits)))
            throw std::runtime_error(windows_error(GetLastError()));

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        // With no handles named, the child opens the pseudoconsole, even when this process's own stdio is redirected.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        std::wstring command = make_command_line(executable, arguments);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr,
                            &startup.StartupInfo, &info))
            throw std::runtime_error(windows_error(GetLastError()));
        impl_->process.reset(info.hProcess);
        Handle primary_thread(info.hThread);
        if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get())) {
            DWORD error = GetLastError();
            TerminateProcess(impl_->process.get(), 1);
            throw std::runtime_error(windows_error(error));
        }
        impl_->exited = std::move(exited);
        impl_->input_stopping = false;
        impl_->writer_done.store(false);
        impl_->running.store(true);
        impl_->reader = std::thread([this] {
            char buffer[8192];
            DWORD count;
            while (ReadFile(impl_->output_read.get(), buffer, sizeof(buffer), &count, nullptr)
                   && count)
                impl_->host.write(std::string_view(buffer, count));
        });
        impl_->writer = std::thread([this] {
            for (;;) {
                std::string input;
                {
                    std::unique_lock lock(impl_->input_mutex);
                    impl_->input_ready.wait(lock, [this] {
                        return impl_->input_stopping || !impl_->pending_input.empty();
                    });
                    if (impl_->input_stopping) break;
                    input.swap(impl_->pending_input);
                }
                std::size_t offset = 0;
                while (offset < input.size()) {
                    DWORD count = 0;
                    DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(input.size() - offset, 4096));
                    if (!WriteFile(impl_->input_write.get(), input.data() + offset,
                                   chunk, &count, nullptr) || !count) break;
                    offset += count;
                }
            }
            impl_->writer_done.store(true);
            impl_->input_ready.notify_all();
        });
        impl_->host.foreground(true, 0);
        impl_->waiter = std::thread([this] {
            WaitForSingleObject(impl_->process.get(), INFINITE);
            DWORD status = 1;
            GetExitCodeProcess(impl_->process.get(), &status);
            TerminateJobObject(impl_->job.get(), status);
            // Keep draining output while ClosePseudoConsole flushes it.
            impl_->close_pseudo();
            impl_->stop_input();
            if (impl_->reader.joinable()) impl_->reader.join();
            int result = static_cast<int>(status);
            impl_->host.foreground(false, result);
            if (impl_->exited) impl_->exited(result);
            // Publish completion only after output and callbacks finish.
            impl_->running.store(false);
        });
        if (ResumeThread(primary_thread.get()) == static_cast<DWORD>(-1))
            throw std::runtime_error(windows_error(GetLastError()));
    } catch (...) {
        close();
        throw;
    }
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
    std::lock_guard lock(impl_->terminal_mutex);
    if (impl_->pseudo)
        ResizePseudoConsole(impl_->pseudo,
            COORD{static_cast<SHORT>(std::clamp<unsigned>(columns, 1, 32767)),
                  static_cast<SHORT>(std::clamp<unsigned>(rows, 1, 32767))});
}

void TerminalSession::interrupt() { write("\x03"); }

void TerminalSession::close() {
    impl_->running.store(false);
    if (impl_->job.get()) TerminateJobObject(impl_->job.get(), 1);
    // Only the waiter operates the live session's I/O teardown.
    if (impl_->waiter.joinable()) impl_->waiter.join();
    impl_->close_pseudo();
    impl_->stop_input();
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->output_read.reset();
    impl_->process.reset();
    impl_->job.reset();
}
} // namespace hirari
