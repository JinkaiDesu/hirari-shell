#include "hirari/process.hpp"
#include "hirari/win32.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#endif

namespace hirari {

namespace {

// =============================================================================
// Seed helpers
// =============================================================================

bool symbol_is(
    seed::Runtime& runtime,
    seed::Ptr form,
    std::string_view text
) {
    return
        runtime.is_symbol(form) &&
        runtime.as_symbol(form)
            ->text == text;
}

bool name_is(
    seed::Runtime& runtime,
    seed::Ptr form,
    std::string_view text
) {
    if (
        !runtime.is_name(form)
    ) {
        return false;
    }

    seed::Ptr target =
        runtime.as_name(
            form
        )->target;

    return
        runtime.is_symbol(target) &&
        runtime.as_symbol(target)
            ->text == text;
}

// consumer + producer
//
// becomes:
//
#ifdef _WIN32

using namespace win32;

// =============================================================================
// PATH
// =============================================================================

std::wstring search_path_once(
    std::wstring_view name,
    const wchar_t* extension
) {
    std::vector<wchar_t>
        buffer(32768);

    const std::wstring owned(
        name
    );

    // Search the caller's PATH explicitly. NULL uses Windows' registry-dependent
    // system order, which can select System32/sort.exe ahead of bundled tools.
    DWORD capacity = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring environment_path;
    while (capacity != 0) {
        std::vector<wchar_t> path_buffer(capacity);
        DWORD count = GetEnvironmentVariableW(L"PATH", path_buffer.data(), capacity);
        if (count == 0) break;
        if (count >= capacity) { capacity = count + 1; continue; }
        environment_path.assign(path_buffer.data(), count);
        break;
    }
    const wchar_t* lookup_path = environment_path.empty() ? nullptr : environment_path.c_str();

    DWORD length =
        SearchPathW(
            lookup_path,
            owned.c_str(),
            extension,
            static_cast<DWORD>(
                buffer.size()
            ),
            buffer.data(),
            nullptr
        );

    if (length == 0) {
        return {};
    }

    if (
        length >=
        buffer.size()
    ) {
        buffer.resize(
            static_cast<std::size_t>(
                length
            ) + 1
        );

        length =
            SearchPathW(
                lookup_path,
                owned.c_str(),
                extension,
                static_cast<DWORD>(
                    buffer.size()
                ),
                buffer.data(),
                nullptr
            );

        if (
            length == 0 ||
            length >=
                buffer.size()
        ) {
            return {};
        }
    }

    return std::wstring(
        buffer.data(),
        length
    );
}

std::wstring find_executable(
    std::string_view name
) {
    const std::wstring wide =
        widen(
            name
        );

    if (
        std::wstring path =
            search_path_once(
                wide,
                nullptr
            );
        !path.empty()
    ) {
        return path;
    }

    if (
        std::wstring path =
            search_path_once(
                wide,
                L".exe"
            );
        !path.empty()
    ) {
        return path;
    }

    return {};
}

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

#endif

} // namespace

// =============================================================================
// Construction
// =============================================================================

ProcessHost::ProcessHost(
    seed::Runtime& runtime,
    Host& host
)
    : host_(host) {
    // Use Seed's orientation law, specializing foreign stages as live pipes.
    runtime.define("truth", &ProcessHost::truth, this);
    runtime.define("cd", &ProcessHost::directory, this);
    runtime.define("pwd", &ProcessHost::directory, this);
    runtime.define("exit", &ProcessHost::quit, this);
    runtime.define("quit", &ProcessHost::quit, this);
    runtime.define(
        "|",
        &ProcessHost::pipe,
        this
    );
}

ProcessHost::~ProcessHost() { conpty_.reset(); }

seed::Ptr ProcessHost::truth(seed::Runtime& runtime, seed::Node* relation, seed::Ptr) {
    auto terms = runtime.terms(relation);
    if (!symbol_is(runtime, terms.front(), "truth")) return relation;
    if (terms.size() != 2) throw std::runtime_error("truth expects one value");
    seed::Ptr operand = terms[1];
    if (runtime.is_symbol(operand) && !runtime.binding(runtime.as_symbol(operand)) &&
        runtime.resolve_program(runtime.as_symbol(operand)))
        operand = runtime.arena().make_relation({operand});
    seed::Ptr value = runtime.reduce(operand);
    ProcessResult result;
    bool selected = unpack(runtime, value, result) ? result.status == 0 : runtime.truth(value);
    return selected ? static_cast<seed::Ptr>(runtime.arena().make_name(runtime.intern("true")))
                    : static_cast<seed::Ptr>(runtime.arena().unit());
}

seed::Ptr ProcessHost::quit(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    auto terms = runtime.terms(relation);
    if (!runtime.is_symbol(terms.front())) return relation;
    const auto& head = runtime.as_symbol(terms.front())->text;
    if (head != "exit" && head != "quit") return relation;
    if (terms.size() != 1) throw std::runtime_error("exit/quit takes no arguments");
    static_cast<ProcessHost*>(context)->quitting_ = true;
    return runtime.arena().unit();
}

void ProcessHost::reap_foreground() {
    if (conpty_ && !conpty_->running()) conpty_.reset();
}

seed::Ptr ProcessHost::directory(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    auto* self = static_cast<ProcessHost*>(context);
    auto terms = runtime.terms(relation);
    if (!runtime.is_symbol(terms.front())) return relation;
    const auto& command = runtime.as_symbol(terms.front())->text;
    if (command != "cd" && command != "pwd") return relation;
#ifdef _WIN32
    if (command == "cd") {
        if (terms.size() > 2) throw std::runtime_error("cd takes one path");
        std::wstring path;
        if (terms.size() == 2) path = widen(self->argument(runtime, terms[1]));
        else {
            DWORD size = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
            if (!size) throw std::runtime_error("USERPROFILE is not set");
            path.resize(size);
            DWORD count = GetEnvironmentVariableW(L"USERPROFILE", path.data(), size);
            if (!count || count >= size) throw std::runtime_error("cannot read USERPROFILE");
            path.resize(count);
        }
        if (!SetCurrentDirectoryW(path.c_str())) throw std::runtime_error("cd: " + windows_error(GetLastError()));
        self->last_status_.store(0);
        return runtime.arena().unit();
    }
    if (terms.size() != 1) throw std::runtime_error("pwd takes no arguments");
    DWORD size = GetCurrentDirectoryW(0, nullptr);
    std::wstring path(size, L'\0');
    DWORD count = GetCurrentDirectoryW(size, path.data());
    if (!count || count >= size) throw std::runtime_error(windows_error(GetLastError()));
    path.resize(count);
    self->last_status_.store(0);
    return self->make_result(runtime, narrow(path) + "\n", {}, 0);
#else
    (void)self;
    throw std::runtime_error("directory programs require Windows");
#endif
}

// =============================================================================
// Foreground / terminal
// =============================================================================

void ProcessHost::begin_foreground(
    seed::Ptr root
) {
    if (
        conpty_ &&
        !conpty_->running()
    ) {
        conpty_.reset();
    }

    foreground_root_ =
        root;
}

void ProcessHost::end_foreground() {
    foreground_root_ =
        nullptr;
}

bool ProcessHost::interactive() const noexcept {
    return
        conpty_ &&
        conpty_->running();
}

bool ProcessHost::terminal_input(
    std::string_view bytes
) {
    if (!interactive()) {
        return false;
    }

    conpty_->write(
        bytes
    );

    return true;
}

void ProcessHost::resize(
    std::uint16_t columns,
    std::uint16_t rows
) {
    if (!interactive()) {
        return;
    }

    conpty_->resize(
        columns,
        rows
    );
}

void ProcessHost::interrupt() {
    if (!interactive()) {
        return;
    }

    conpty_->interrupt();
}

// =============================================================================
// Resolver
// =============================================================================

seed::Program ProcessHost::resolve(
    seed::Runtime& runtime,
    seed::Symbol* symbol,
    seed::Ptr context
) {
    if (
        !context ||
        !symbol
    ) {
        return {};
    }

    auto* host =
        static_cast<
            ProcessHost*
        >(
            context
        );

    return host->resolve_symbol(
        runtime,
        symbol
    );
}

seed::Program
ProcessHost::resolve_symbol(
    seed::Runtime&,
    seed::Symbol* symbol
) {
    if (!symbol) {
        return {};
    }


#ifdef _WIN32

    std::wstring executable =
        find_executable(
            symbol->text
        );

    if (
        executable.empty()
    ) {
        return {};
    }

    auto& foreign = programs_[symbol];
    if (!foreign) foreign = std::make_unique<ForeignProgram>();
    foreign->owner = this;
    foreign->symbol = symbol;
    foreign->executable = std::move(executable);
    return seed::Program{&ProcessHost::invoke, foreign.get()};

#else

    return {};

#endif
}

// =============================================================================
// |
// =============================================================================

seed::Ptr ProcessHost::pipe(
    seed::Runtime& runtime,
    seed::Node* relation,
    seed::Ptr context
) {
    if (
        !context ||
        !relation
    ) {
        return relation;
    }

    std::vector<seed::Ptr> segments;
    seed::Ptr canonical = runtime.flow(relation, segments);
    if (canonical == relation) return relation;
    bool has_foreign_stage = false;
    for (seed::Ptr segment : segments) {
        if (static_cast<ProcessHost*>(context)->foreign_program(runtime, segment))
            has_foreign_stage = true;
    }

    std::vector<Stage> stages;
    if (!static_cast<ProcessHost*>(context)->build_pipeline(runtime, canonical, stages)) {
        auto head = runtime.is_relation(canonical) ? runtime.as_node(canonical)->head : canonical;
        if (!has_foreign_stage || (runtime.is_symbol(head) && runtime.registry().find(runtime.as_symbol(head))))
            return runtime.reduce(canonical);
        throw std::runtime_error("pipeline consumer is not an executable: " + runtime.print(head));
    }
    if (stages.size() != segments.size())
        throw std::runtime_error("pipeline contains an unresolved or non-process stage");
    return static_cast<ProcessHost*>(context)->run_pipeline(runtime, std::move(stages));
}

// =============================================================================
// Foreign-program discovery
// =============================================================================

ProcessHost::ForeignProgram*
ProcessHost::foreign_program(
    seed::Runtime& runtime,
    seed::Ptr form
) {
    seed::Symbol* symbol =
        nullptr;

    if (
        runtime.is_symbol(
            form
        )
    ) {
        symbol =
            runtime.as_symbol(
                form
            );
    }
    else if (
        runtime.is_relation(
            form
        )
    ) {
        const auto terms =
            runtime.terms(
                runtime.as_node(
                    form
                )
            );

        if (
            !terms.empty() &&
            runtime.is_symbol(
                terms.front()
            )
        ) {
            symbol =
                runtime.as_symbol(
                    terms.front()
                );
        }
    }

    if (!symbol || runtime.binding(symbol)) {
        return nullptr;
    }

    // An explicitly-defined Seed/Hirari program wins.
    //
    // Do not reinterpret something like ? or = as an .exe
    // merely because PATH happens to contain that spelling.
    if (
        runtime.registry()
            .find(
                symbol
            )
    ) {
        return nullptr;
    }

    seed::Program resolved =
        resolve_symbol(
            runtime,
            symbol
        );

    if (
        !resolved ||
        resolved.invoke !=
            &ProcessHost::invoke
    ) {
        return nullptr;
    }

    return static_cast<
        ForeignProgram*
    >(
        resolved.context
    );
}

bool ProcessHost::is_foreign_program(
    seed::Runtime& runtime,
    seed::Ptr form
) {
    return
        foreign_program(
            runtime,
            form
        ) != nullptr;
}

// =============================================================================
// Foreign argv projection
// =============================================================================

std::string ProcessHost::argument(
    seed::Runtime& runtime,
    seed::Ptr form
) {
    seed::Ptr value =
        runtime.reduce(
            form
        );

    ProcessResult process;

    if (
        unpack(
            runtime,
            value,
            process
        )
    ) {
        return process.out;
    }

    if (
        runtime.is_literal(
            value
        )
    ) {
        return
            runtime.as_literal(
                value
            )->bytes;
    }

    if (
        runtime.is_symbol(
            value
        )
    ) {
        return
            runtime.as_symbol(
                value
            )->text;
    }

    if (
        runtime.is_name(
            value
        )
    ) {
        seed::Ptr target =
            runtime.as_name(
                value
            )->target;

        if (
            runtime.is_symbol(
                target
            )
        ) {
            return
                runtime.as_symbol(
                    target
                )->text;
        }

        return runtime.print(
            target
        );
    }

    if (
        runtime.is_unit(
            value
        )
    ) {
        return {};
    }

    if (
        runtime.is_cell(
            value
        )
    ) {
        seed::Ptr current =
            runtime.head(
                runtime.as_cell(
                    value
                )
            );

        if (!current) {
            return {};
        }

        return argument(
            runtime,
            current
        );
    }

    return runtime.print(
        value
    );
}

// =============================================================================
// Pipeline construction
// =============================================================================

bool ProcessHost::build_pipeline(
    seed::Runtime& runtime,
    seed::Ptr form,
    std::vector<Stage>& stages
) {
    ForeignProgram* program =
        foreign_program(
            runtime,
            form
        );

    if (!program) {
        return false;
    }

    if (
        runtime.is_symbol(
            form
        )
    ) {
        stages.push_back(
            Stage{
                program,
                {}
            }
        );

        return true;
    }

    const auto terms =
        runtime.terms(
            runtime.as_node(
                form
            )
        );

    // Unit in the capture position is real structure, not empty argv sugar.
    if (terms.size() >= 3 && runtime.is_unit(terms[1]))
        throw std::runtime_error("OS process adapter does not implement capture products");

    Stage stage;

    stage.program =
        program;

    // A bare binary Program product is consumer-first composition.
    // Longer argv products use explicit nested producer relations; their bare
    // operand Symbols stay scalar. Literals/Names disambiguate executable-named
    // scalar arguments in a binary product.
    seed::Ptr producer =
        nullptr;

    for (
        std::size_t i = 1;
        i < terms.size();
        ++i
    ) {
        seed::Ptr term =
            terms[i];

        if (
            (runtime.is_relation(term) || (terms.size() == 2 && i == 1)) &&
            is_foreign_program(runtime, term)
        ) {
            if (producer) {
                throw std::runtime_error(
                    "a process cannot have multiple stdin producers"
                );
            }

            producer =
                term;

            continue;
        }

        stage.arguments.push_back(
            argument(
                runtime,
                term
            )
        );
    }

    if (producer) {
        if (
            !build_pipeline(
                runtime,
                producer,
                stages
            )
        ) {
            throw std::runtime_error(
                "unable to build producer process"
            );
        }
    }

    stages.push_back(
        std::move(stage)
    );

    return true;
}

// =============================================================================
// Invocation
// =============================================================================

seed::Ptr ProcessHost::invoke(
    seed::Runtime& runtime,
    seed::Node* relation,
    seed::Ptr context
) {
    if (
        !context ||
        !relation
    ) {
        return relation;
    }

    auto* program =
        static_cast<
            ForeignProgram*
        >(
            context
        );

    if (
        !program->owner ||
        !program->symbol
    ) {
        return relation;
    }

    const auto terms =
        runtime.terms(
            relation
        );

    if (
        terms.empty() ||
        terms.front() !=
            static_cast<seed::Ptr>(
                program->symbol
            )
    ) {
        return relation;
    }

    return program
        ->owner
        ->run(
            runtime,
            *program,
            relation
        );
}

seed::Ptr ProcessHost::run(
    seed::Runtime& runtime,
    ForeignProgram&,
    seed::Node* relation
) {
    std::vector<Stage> stages;

    if (
        !build_pipeline(
            runtime,
            relation,
            stages
        )
    ) {
        return relation;
    }

    // A single top-level foreign program may own the terminal.
    //
    // Nested programs and actual process products use ordinary
    // streams/pipes instead.
    const bool foreground =
        host_.terminal_capable() &&
        foreground_root_ ==
            static_cast<seed::Ptr>(
                relation
            ) &&
        stages.size() == 1;

    if (!foreground) {
        return run_pipeline(
            runtime,
            std::move(
                stages
            )
        );
    }

    if (
        conpty_ &&
        conpty_->running()
    ) {
        throw std::runtime_error(
            "foreground terminal is already occupied"
        );
    }

    conpty_ =
        std::make_unique<
            ConPtySession
        >(
            host_
        );

    Stage stage =
        std::move(
            stages.front()
        );

    conpty_->start(
        stage.program
            ->executable,
        stage.arguments,
        120,
        30,
        [this](
            int status
        ) {
            last_status_ =
                status;
        }
    );

    // Live terminal program: no completed Seed value yet.
    return nullptr;
}

// =============================================================================
// Pipeline realization
// =============================================================================

seed::Ptr ProcessHost::run_pipeline(
    seed::Runtime& runtime,
    std::vector<Stage> stages
) {
#ifdef _WIN32

    if (
        stages.empty()
    ) {
        return
            runtime.arena()
                .unit();
    }

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_info{};
    job_info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
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

    Pipe stderr_capture =
        make_pipe(
            true
        );

    Handle null_input =
        make_null_input();

    auto spawn_stage =
        [&job](
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

        std::wstring command =
            make_command_line(
                stage.program
                    ->executable,
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
                stage.program
                    ->executable
                    .c_str(),
                mutable_command.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &info
            );

        if (!created) {
            throw std::runtime_error(
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
                ? stdout_capture
                      .write
                      .get()
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
    std::string err;

    std::size_t remaining =
        processes.size();

    while (
        remaining > 0
    ) {
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

    last_status_ =
        static_cast<int>(
            processes.back()
                .status
        );

    return make_result(
        runtime,
        std::move(out),
        std::move(err),
        last_status_.load()
    );

#else

    (void)runtime;
    (void)stages;

    throw std::runtime_error(
        "foreign process realization requires Windows"
    );

#endif
}

// =============================================================================
// Seed-native process result
// =============================================================================

seed::Ptr ProcessHost::make_result(
    seed::Runtime& runtime,
    std::string out,
    std::string err,
    int status
) {
    return runtime.product({
        runtime.arena()
            .make_name(
                runtime.intern(
                    "stdout"
                )
            ),

        runtime.arena()
            .make_literal(
                out
            ),

        runtime.arena()
            .make_name(
                runtime.intern(
                    "stderr"
                )
            ),

        runtime.arena()
            .make_literal(
                err
            ),

        runtime.arena()
            .make_name(
                runtime.intern(
                    "status"
                )
            ),

        runtime.arena()
            .make_literal(
                std::to_string(
                    status
                )
            )
    });
}

bool ProcessHost::unpack(
    seed::Runtime& runtime,
    seed::Ptr form,
    ProcessResult& result
) {
    if (
        !runtime.is_relation(
            form
        )
    ) {
        return false;
    }

    const auto terms =
        runtime.terms(
            runtime.as_node(
                form
            )
        );

    if (
        terms.size() != 6
    ) {
        return false;
    }

    if (
        !name_is(
            runtime,
            terms[0],
            "stdout"
        ) ||
        !runtime.is_literal(
            terms[1]
        ) ||
        !name_is(
            runtime,
            terms[2],
            "stderr"
        ) ||
        !runtime.is_literal(
            terms[3]
        ) ||
        !name_is(
            runtime,
            terms[4],
            "status"
        ) ||
        !runtime.is_literal(
            terms[5]
        )
    ) {
        return false;
    }

    result.out =
        runtime.as_literal(
            terms[1]
        )->bytes;

    result.err =
        runtime.as_literal(
            terms[3]
        )->bytes;

    const std::string& status_text =
        runtime.as_literal(
            terms[5]
        )->bytes;

    int status = 0;

    const auto conversion =
        std::from_chars(
            status_text.data(),
            status_text.data() +
                status_text.size(),
            status
        );

    if (
        conversion.ec !=
            std::errc{} ||
        conversion.ptr !=
            status_text.data() +
                status_text.size()
    ) {
        return false;
    }

    result.status =
        status;

    return true;
}

} // namespace hirari
