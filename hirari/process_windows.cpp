// Windows realization of `{ }` pipelines and native programs: job objects, inherited handles and pipes.
#include "hirari/process.hpp"
#include "hirari/win32.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace hirari {
namespace {
using namespace win32;

// =============================================================================
// Pipe
// =============================================================================

struct Pipe {
    Handle read;
    Handle write;
};

Pipe make_pipe(
    bool parent_reads
) {
    SECURITY_ATTRIBUTES security{};

    security.nLength =
        sizeof(security);

    security.bInheritHandle =
        TRUE;

    HANDLE read_raw =
        nullptr;

    HANDLE write_raw =
        nullptr;

    if (
        !CreatePipe(
            &read_raw,
            &write_raw,
            &security,
            0
        )
    ) {
        throw std::runtime_error(
            windows_error(
                GetLastError()
            )
        );
    }

    Pipe result{
        Handle(read_raw),
        Handle(write_raw)
    };

    if (parent_reads) {
        if (
            !SetHandleInformation(
                result.read.get(),
                HANDLE_FLAG_INHERIT,
                0
            )
        ) {
            throw std::runtime_error(
                windows_error(
                    GetLastError()
                )
            );
        }
    }

    return result;
}

Handle make_null_input() {
    SECURITY_ATTRIBUTES security{};

    security.nLength =
        sizeof(security);

    security.bInheritHandle =
        TRUE;

    HANDLE handle =
        CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ |
            FILE_SHARE_WRITE,
            &security,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

    if (
        handle ==
        INVALID_HANDLE_VALUE
    ) {
        throw std::runtime_error(
            windows_error(
                GetLastError()
            )
        );
    }

    return Handle(
        handle
    );
}

// =============================================================================
// Restricted handle inheritance
// =============================================================================

class AttributeList {
public:
    explicit AttributeList(
        std::vector<HANDLE> handles
    )
        : handles_(
            std::move(handles)
        ) {
        SIZE_T bytes =
            0;

        InitializeProcThreadAttributeList(
            nullptr,
            1,
            0,
            &bytes
        );

        storage_.resize(
            bytes
        );

        list_ =
            reinterpret_cast<
                LPPROC_THREAD_ATTRIBUTE_LIST
            >(
                storage_.data()
            );

        if (
            !InitializeProcThreadAttributeList(
                list_,
                1,
                0,
                &bytes
            )
        ) {
            throw std::runtime_error(
                windows_error(
                    GetLastError()
                )
            );
        }

        if (
            !UpdateProcThreadAttribute(
                list_,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles_.data(),
                handles_.size() *
                    sizeof(HANDLE),
                nullptr,
                nullptr
            )
        ) {
            const DWORD error = GetLastError();
            DeleteProcThreadAttributeList(list_);
            list_ = nullptr;
            throw std::runtime_error(windows_error(error));
        }
    }

    ~AttributeList() {
        if (list_) {
            DeleteProcThreadAttributeList(
                list_
            );
        }
    }

    AttributeList(
        const AttributeList&
    ) = delete;

    AttributeList& operator=(
        const AttributeList&
    ) = delete;

    LPPROC_THREAD_ATTRIBUTE_LIST
    get() const noexcept {
        return list_;
    }

private:
    std::vector<HANDLE>
        handles_;

    std::vector<std::byte>
        storage_;

    LPPROC_THREAD_ATTRIBUTE_LIST
        list_ = nullptr;
};

// =============================================================================
// Output draining
// =============================================================================

bool drain_available(
    HANDLE pipe,
    std::string& output
) {
    bool read_any =
        false;

    for (int batch = 0; batch < 16; ++batch) {
        DWORD available =
            0;

        if (
            !PeekNamedPipe(
                pipe,
                nullptr,
                0,
                nullptr,
                &available,
                nullptr
            )
        ) {
            const DWORD error =
                GetLastError();

            if (
                error ==
                ERROR_BROKEN_PIPE
            ) {
                return read_any;
            }

            throw std::runtime_error(
                windows_error(
                    error
                )
            );
        }

        if (
            available == 0
        ) {
            return read_any;
        }

        char buffer[4096];

        const DWORD wanted =
            available <
                    sizeof(buffer)
                ? available
                : static_cast<DWORD>(
                    sizeof(buffer)
                );

        DWORD count =
            0;

        if (
            !ReadFile(
                pipe,
                buffer,
                wanted,
                &count,
                nullptr
            )
        ) {
            const DWORD error =
                GetLastError();

            if (
                error ==
                ERROR_BROKEN_PIPE
            ) {
                return read_any;
            }

            throw std::runtime_error(
                windows_error(
                    error
                )
            );
        }

        if (
            count == 0
        ) {
            return read_any;
        }

        output.append(
            buffer,
            count
        );

        read_any =
            true;
    }
    return read_any;
}

void drain_to_eof(
    HANDLE pipe,
    std::string& output
) {
    char buffer[4096];

    for (;;) {
        DWORD count =
            0;

        const BOOL ok =
            ReadFile(
                pipe,
                buffer,
                static_cast<DWORD>(
                    sizeof(buffer)
                ),
                &count,
                nullptr
            );

        if (
            ok &&
            count > 0
        ) {
            output.append(
                buffer,
                count
            );

            continue;
        }

        if (!ok) {
            const DWORD error =
                GetLastError();

            if (
                error !=
                ERROR_BROKEN_PIPE
            ) {
                throw std::runtime_error(
                    windows_error(
                        error
                    )
                );
            }
        }

        return;
    }
}

struct Spawned {
    Handle process;

    bool complete =
        false;

    DWORD status =
        0;
};
} // namespace

seed::Ptr ProcessHost::run_pipeline(
    seed::Runtime& runtime,
    std::vector<Stage> stages, bool passthrough,
    const std::string& output_file, bool append
) {
    if (
        stages.empty()
    ) {
        return
            runtime.arena()
                .unit();
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    // Explicitly detached services may outlive a command; ordinary descendants
    // remain owned and killed with this pipeline.
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (allow_breakaway_?JOB_OBJECT_LIMIT_BREAKAWAY_OK:0);
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
                                         &job_info, sizeof(job_info)))
        throw std::runtime_error(windows_error(GetLastError()));

    std::vector<Pipe> links;

    if (
        stages.size() > 1
    ) {
        links.reserve(
            stages.size() - 1
        );

        for (
            std::size_t i = 0;
            i + 1 < stages.size();
            ++i
        ) {
            links.push_back(
                make_pipe(
                    false
                )
            );
        }
    }

    Pipe stdout_capture =
        make_pipe(
            true
        );

    Handle file_output;
    if (!output_file.empty()) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        file_output = Handle(CreateFileW(widen(output_file).c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
            FILE_SHARE_READ, &security, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file_output) throw std::runtime_error(windows_error(GetLastError()));
    }

    Pipe stderr_capture =
        make_pipe(
            true
        );

    Handle null_input =
        make_null_input();
    Handle input_write;
    if (!stages.front().input_file.empty()) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        Handle file(CreateFileW(widen(stages.front().input_file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) throw std::runtime_error("cannot read " + stages.front().input_file + ": " + windows_error(GetLastError()));
        null_input = std::move(file);
    } else if (stages.front().input) {
        auto input_pipe = make_pipe(false);
        null_input = std::move(input_pipe.read);
        input_write = std::move(input_pipe.write);
    } else if(passthrough && streaming_) {
        HANDLE input=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_INPUT_HANDLE),GetCurrentProcess(),&input,0,TRUE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error(windows_error(GetLastError()));
        null_input=Handle(input);
    }

    const std::wstring working_directory = widen(working_directory_);
    auto spawn_stage =
        [&job,&working_directory,this](
            const Stage& stage,
            HANDLE input,
            HANDLE output,
            HANDLE error
        ) -> Spawned {

        if (
            !stage.program
        ) {
            throw std::runtime_error(
                "pipeline stage has no program"
            );
        }

        const std::wstring executable = widen(stage.program->executable);

        std::wstring command =
            make_command_line(
                executable,
                stage.arguments
            );

        std::vector<wchar_t>
            mutable_command(
                command.begin(),
                command.end()
            );

        mutable_command.push_back(
            L'\0'
        );

        AttributeList attributes(
            {
                input,
                output,
                error
            }
        );

        STARTUPINFOEXW startup{};

        startup.StartupInfo.cb =
            sizeof(startup);

        startup.StartupInfo.dwFlags =
            STARTF_USESTDHANDLES;

        startup.StartupInfo.hStdInput =
            input;

        startup.StartupInfo.hStdOutput =
            output;

        startup.StartupInfo.hStdError =
            error;

        startup.lpAttributeList =
            attributes.get();

        PROCESS_INFORMATION info{};

        const BOOL created =
            CreateProcessW(
                executable.c_str(),
                mutable_command.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                nullptr,
                working_directory.empty()?nullptr:working_directory.c_str(),
                &startup.StartupInfo,
                &info
            );

        if (!created) {
            throw std::runtime_error(
                "cannot start " + stage.program->executable + ": " +
                windows_error(
                    GetLastError()
                )
            );
        }

        Handle thread(
            info.hThread
        );

        Handle process(info.hProcess);
        if (!AssignProcessToJobObject(job.get(), process.get()) ||
            ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            DWORD error_code = GetLastError();
            TerminateProcess(process.get(), 1);
            throw std::runtime_error(windows_error(error_code));
        }
        if (spawn_observer_) spawn_observer_(info.dwProcessId);
        return Spawned{
            std::move(process),
            false,
            0
        };
    };

    std::vector<Spawned>
        processes;

    processes.reserve(
        stages.size()
    );

    for (
        std::size_t i = 0;
        i < stages.size();
        ++i
    ) {
        HANDLE input =
            i == 0
                ? null_input.get()
                : links[i - 1]
                      .read
                      .get();

        HANDLE output =
            i + 1 ==
                    stages.size()
                ? (file_output ? file_output.get() : stdout_capture.write.get())
                : links[i]
                      .write
                      .get();

        processes.push_back(
            spawn_stage(
                stages[i],
                input,
                output,
                stderr_capture
                    .write
                    .get()
            )
        );
    }

    // Children now own the necessary handles.
    // Parent copies must disappear so EOF propagates.
    null_input.reset();

    // Feed materialized answers while stdout/stderr are drained, so large input
    // cannot deadlock against a consumer writing output. Process producers above
    // still use direct live links and do not pass through this buffer.
    std::thread input_writer;
    std::atomic<DWORD> input_error{0};
    struct InputCleanup {
        std::thread& writer;
        HANDLE job;
        ~InputCleanup() {
            if (!writer.joinable()) return;
            TerminateJobObject(job, 1);
            CancelSynchronousIo(writer.native_handle());
            writer.join();
        }
    } input_cleanup{input_writer, job.get()};
    if (stages.front().input) {
        input_writer = std::thread([pipe = std::move(input_write),
                                   bytes = std::move(stages.front().input->out), &input_error]() mutable {
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                DWORD written = 0;
                if (!WriteFile(pipe.get(), bytes.data() + offset,
                               static_cast<DWORD>(std::min<std::size_t>(65536, bytes.size() - offset)), &written, nullptr)) {
                    auto error = GetLastError();
                    if (error != ERROR_BROKEN_PIPE && error != ERROR_NO_DATA && error != ERROR_OPERATION_ABORTED)
                        input_error = error;
                    break;
                }
                if (!written) break;
                offset += written;
            }
            pipe.reset();
        });
    }

    for (
        Pipe& link :
        links
    ) {
        link.read.reset();
        link.write.reset();
    }

    stdout_capture
        .write
        .reset();

    stderr_capture
        .write
        .reset();

    std::string out;
    std::string err = stages.front().input ? stages.front().input->err : std::string{};

    std::size_t remaining =
        processes.size();
    // When the final stage has exited, upstream writers have no reader. Most
    // exit on the write error; one that ignores it is ended after a grace
    // period (the SIGPIPE analogue) so it cannot hold the pipeline open.
    std::vector<bool> severed(processes.size(), false);
    ULONGLONG consumer_exited = 0;

    while (
        remaining > 0
    ) {
        if (processes.size() > 1 && processes.back().complete) {
            if (!consumer_exited) consumer_exited = GetTickCount64();
            else if (GetTickCount64() - consumer_exited > 2000)
                for (std::size_t i = 0; i + 1 < processes.size(); ++i)
                    if (!processes[i].complete && !severed[i]) { severed[i] = true; TerminateProcess(processes[i].process.get(), 141); }
        }
        auto old_out=out.size(),old_err=err.size();
        if (host_.interrupted()) TerminateJobObject(job.get(), 130);
        drain_available(
            stdout_capture
                .read
                .get(),
            out
        );

        drain_available(
            stderr_capture
                .read
                .get(),
            err
        );
        if(output_observer_){output_observer_(std::string_view(out).substr(old_out),false);output_observer_(std::string_view(err).substr(old_err),true);}
        if(passthrough){if(!out.empty())host_.write(out);if(!err.empty())host_.error(err);out.clear();err.clear();}
        if(out.size()>16*1024*1024 || err.size()>16*1024*1024)
            throw std::runtime_error("captured output exceeds 16 MiB; use live streams or an artifact file");

        for (
            Spawned& process :
            processes
        ) {
            if (
                process.complete
            ) {
                continue;
            }

            const DWORD state =
                WaitForSingleObject(
                    process.process
                        .get(),
                    0
                );

            if (
                state ==
                WAIT_TIMEOUT
            ) {
                continue;
            }

            if (
                state ==
                WAIT_FAILED
            ) {
                throw std::runtime_error(
                    windows_error(
                        GetLastError()
                    )
                );
            }

            if (
                !GetExitCodeProcess(
                    process.process
                        .get(),
                    &process.status
                )
            ) {
                throw std::runtime_error(
                    windows_error(
                        GetLastError()
                    )
                );
            }

            process.complete =
                true;

            --remaining;
        }

        if (
            remaining > 0
        ) {
            Sleep(1);
        }
    }

    // Descendants cannot retain capture handles after the pipeline completes.
    TerminateJobObject(job.get(), 0);
    if (input_writer.joinable()) input_writer.join();
    if (input_error.load()) throw std::runtime_error(windows_error(input_error.load()));
    auto old_out=out.size(),old_err=err.size();
    drain_to_eof(
        stdout_capture
            .read
            .get(),
        out
    );

    drain_to_eof(
        stderr_capture
            .read
            .get(),
        err
    );
    if(output_observer_){output_observer_(std::string_view(out).substr(old_out),false);output_observer_(std::string_view(err).substr(old_err),true);}
    if(passthrough){host_.write(out);host_.error(err);out.clear();err.clear();}

    last_status_ =
        static_cast<int>(
            processes.back()
                .status
        );
    if(last_status_.load()==0)
        for(std::size_t i=0;i<processes.size();++i)
            if(processes[i].status!=0 && !severed[i]){last_status_=static_cast<int>(processes[i].status);break;}
    if(last_status_.load()==0 && stages.front().input)last_status_=stages.front().input->status;
    std::vector<int> statuses;
    if(processes.size()>1)for(const auto& process:processes)statuses.push_back(static_cast<int>(process.status));

    return make_result(
        runtime,
        std::move(out),
        std::move(err),
        last_status_.load(),
        statuses
    );
}

} // namespace hirari
