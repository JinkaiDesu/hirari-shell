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

// A demanded process is still a live producer. Inspect its lowering without
// executing it while constructing the pipe chain.
seed::Ptr producer_form(seed::Runtime& runtime, seed::Ptr form) {
    while (runtime.is_relation(form)) {
        auto terms = runtime.terms(runtime.as_node(form));
        if (terms.size() != 2 || !symbol_is(runtime, terms.front(), "demand")) break;
        form = terms[1];
    }
    return form;
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

    auto result = std::wstring(
        buffer.data(),
        length
    );
    // SearchPath can return a directory (for example PowerShell's "tr" locale).
    // It is not a launchable extensionless program; continue with .exe lookup.
    DWORD attributes = GetFileAttributesW(result.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return {};
    return result;
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
    : host_(host),runtime_(runtime) {
    // Use Seed's orientation law, specializing foreign stages as live pipes.
    runtime.define("truth", &ProcessHost::truth, this);
    runtime.define("cd", &ProcessHost::directory, this);
    runtime.define("pwd", &ProcessHost::directory, this);
    runtime.define("exit", &ProcessHost::quit, this);
    runtime.define("quit", &ProcessHost::quit, this);
    // Outside braces | remains Seed's reverse application. OS pipes exist only
    // in the explicit `{ }` context, which the reader lowers to ({} BODY).
    runtime.define("{}", &ProcessHost::pipeline, this);
    runtime.describe("{}", "{ PRODUCER | CONSUMER ... }\nPOSIX pipeline context. Inside braces | is a real OS pipe: every stage is a native process (a PATH executable or exec \"path\" ...), all stages run concurrently, and PRODUCER.stdout streams into CONSUMER.stdin with ordinary backpressure. Seed callables and .seed scripts are not stages. At top level output streams to the terminal; CONSUMER {PIPELINE} feeds a native consumer live. The answer is -stdout -stderr -status, plus -stages with each stage status. Outside braces | is ordinary reverse application: b | a == a b == a(b).");
}

ProcessHost::~ProcessHost() { conpty_.reset(); }
std::string ProcessHost::executable_path(const std::string& name) {
#ifdef _WIN32
    return narrow(find_executable(name));
#else
    (void)name;return {};
#endif
}

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
    if (terms.size() > 2) throw std::runtime_error("usage: exit [STATUS]");
    int status = 0;
    if (terms.size() == 2) {
        auto value = runtime.reduce(terms[1]);
        if (!runtime.arena().is(value, seed::HostKind::Integer) || runtime.as_number(value)->integer < 0 ||
            runtime.as_number(value)->integer > 2147483647)
            throw std::runtime_error("exit STATUS must be an integer 0..2147483647");
        status = static_cast<int>(runtime.as_number(value)->integer);
    }
    auto* self = static_cast<ProcessHost*>(context);
    self->quitting_ = true;
    self->exit_code_ = status;
    // Unwind every enclosing loop, script and world entry immediately.
    throw ExitRequest{status};
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

    foreground_stack_.push_back(foreground_root_);
    foreground_depths_.push_back(foreground_depth_);
    foreground_depth_=runtime_.reduction_depth()+1;
    foreground_root_ = root;
}

void ProcessHost::end_foreground() {
    foreground_depth_=foreground_depths_.empty()?0:foreground_depths_.back();
    if(!foreground_depths_.empty())foreground_depths_.pop_back();
    foreground_root_ = foreground_stack_.empty() ? nullptr : foreground_stack_.back();
    if (!foreground_stack_.empty()) foreground_stack_.pop_back();
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

seed::Ptr ProcessHost::pipeline(
    seed::Runtime& runtime,
    seed::Node* relation,
    seed::Ptr context
) {
    seed::Ptr body = runtime.pipeline_body(relation);
    if (!context || !body) return relation;
    auto* self = static_cast<ProcessHost*>(context);
    std::vector<Stage> stages;
    self->build_braced(runtime, body, stages, false);
    // A pipeline evaluated as the foreground form streams to the shell's own
    // output as bytes arrive; nested ones answer -stdout -stderr -status.
    return self->run_pipeline(runtime, std::move(stages), runtime.reduction_depth() == self->foreground_depth_);
}

void ProcessHost::build_braced(seed::Runtime& runtime, seed::Ptr body, std::vector<Stage>& stages, bool has_upstream) {
    if (runtime.is_unit(body)) throw std::runtime_error("empty pipeline context {}");
    // Split on this context's own | without composing or reducing: the stages
    // are processes joined by OS pipes, not a Seed application.
    std::vector<seed::Ptr> segments;
    if (runtime.flow(runtime.as_node(body), segments) == body)
        segments = {runtime.product(runtime.terms(runtime.as_node(body)))};
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const bool upstream = has_upstream || !stages.empty();
        if (seed::Ptr nested = runtime.pipeline_body(segments[i])) {
            if (upstream) throw std::runtime_error("{} stage " + std::to_string(i + 1) + " cannot nest a pipeline after |");
            build_braced(runtime, nested, stages, false);
            continue;
        }
        std::vector<Stage> part;
        bool native = false;
        try { native = build_pipeline(runtime, segments[i], part, upstream); }
        catch (const std::exception& error) {
            throw std::runtime_error("{} stage " + std::to_string(i + 1) + " (" + runtime.print(segments[i]) + "): " + error.what());
        }
        if (!native || std::any_of(part.begin(), part.end(), [](const Stage& stage) { return stage.program->script; }))
            throw std::runtime_error("{} stage " + std::to_string(i + 1) + " is not a native process: " + runtime.print(segments[i]) +
                                     " (stages are PATH executables or exec \"path\" ...)");
        if (upstream && (part.size() > 1 || part.front().input))
            throw std::runtime_error("{} stage " + std::to_string(i + 1) + " has stdin from | and from its own producer");
        stages.insert(stages.end(), std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    }
}

// =============================================================================
// Foreign-program discovery
// =============================================================================

ProcessHost::ForeignProgram*
ProcessHost::foreign_program(
    seed::Runtime& runtime,
    seed::Ptr form
) {
    form = producer_form(runtime, form);
    if(runtime.is_symbol(form) && runtime.binding(runtime.as_symbol(form)))form=runtime.arena().make_relation({form});
    form=runtime.application(form);
    if (runtime.is_abstraction(form)) return nullptr;
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

    // Match Shell's command discovery order when planning native pipelines.
    // A vocabulary script can share a spelling with a PATH executable.
    if(stream_resolver_) {
        auto stream=stream_resolver_(symbol);
        if(!stream.executable.empty()) {
            auto& p=stream_programs_[symbol];if(!p)p=std::make_unique<ForeignProgram>();
            *p={this,symbol,std::move(stream.executable),std::move(stream.prefix),true};return p.get();
        }
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
    return is_explicit_program(runtime, form) ||
        foreign_program(
            runtime,
            form
        ) != nullptr;
}

bool ProcessHost::is_explicit_program(seed::Runtime& runtime, seed::Ptr form) {
    form=runtime.application(producer_form(runtime,form));
    if (runtime.is_abstraction(form)) return false;
    if(!runtime.is_relation(form))return false;
    auto terms=runtime.terms(runtime.as_node(form));
    return terms.size()>=2 && symbol_is(runtime,terms[0],"exec");
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

    if (runtime.is_wildcard(value))
        throw std::runtime_error("anonymous _ is not text; quote \"_\" for a native argument");
    if (runtime.is_number(value)) return runtime.as_number(value)->spelling;

    if (opaque_values_ && opaque_values_(value))
        throw std::runtime_error("a runtime identity or structured provider value cannot cross native argv or a byte-stream script boundary; use a structured call or explicit data projection");

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
            auto flag = runtime.print(value);
            if (flag.starts_with("-(")) return "-" + runtime.as_symbol(target)->text;
            return flag;
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
    std::vector<Stage>& stages, bool has_upstream
) {
    form = producer_form(runtime, form);
    form = runtime.application(form);
    const bool explicit_argv=is_explicit_program(runtime,form);
    ForeignProgram* program =
        foreign_program(
            runtime,
            form
        );

    if (explicit_argv) {
#ifdef _WIN32
        auto terms=runtime.terms(runtime.as_node(form));
        auto executable=find_executable(argument(runtime,terms[1]));
        if(executable.empty())throw std::runtime_error("explicit executable not found: "+argument(runtime,terms[1]));
        auto& retained=explicit_programs_[executable];
        if(!retained)retained=std::make_unique<ForeignProgram>();
        *retained={this,runtime.intern("exec"),std::move(executable)};
        program=retained.get();
#endif
    }
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
                program->prefix
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

    Stage stage;

    stage.program =
        program;
    stage.arguments = program->prefix;

    // A bare binary Program product is consumer-first composition.
    // Longer argv products use explicit nested producer relations; their bare
    // operand Symbols stay scalar. Literals/Names disambiguate executable-named
    // scalar arguments in a binary product.
    seed::Ptr producer =
        nullptr;

    for (
        std::size_t i = explicit_argv ? 2 : 1;
        i < terms.size();
        ++i
    ) {
        seed::Ptr term =
            terms[i];

        if (
            !explicit_argv && (runtime.is_relation(term) || (!has_upstream && terms.size() == 2 && i == 1)) &&
            (runtime.pipeline_body(term) || is_foreign_program(runtime, term) || producer_form(runtime, term) != term)
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
        if (seed::Ptr body = runtime.pipeline_body(producer)) {
            // CONSUMER {PIPELINE}: the only application that joins OS pipes,
            // because the producer explicitly asked for a live stream.
            build_braced(runtime, body, stages, false);
        } else {
            // Ordinary application: the producer is reduced to its answer
            // (a process runs to completion) and that answer becomes stdin.
            // A bare program identity is invoked, as truth does for its operand.
            if (runtime.is_symbol(producer) && !runtime.binding(runtime.as_symbol(producer)))
                producer = runtime.arena().make_relation({producer});
            auto value = runtime.reduce(producer);
            ProcessResult input;
            if (!unpack(runtime, value, input)) input.out = argument(runtime, value);
            stage.input = std::move(input);
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

seed::Ptr ProcessHost::execute(seed::Runtime& runtime, const std::string& path,
                              const std::vector<seed::Ptr>& arguments,seed::Node* invocation, std::optional<ProcessResult> input) {
#ifdef _WIN32
    auto executable = find_executable(path);
    if (executable.empty()) throw std::runtime_error("program not found: " + path);
    ForeignProgram program{this, runtime.intern(path), std::move(executable)};
    Stage stage{&program, {}};
    stage.input=std::move(input);
    for (auto value : arguments) stage.arguments.push_back(argument(runtime, value));
    (void)invocation;
    return run_pipeline(runtime, {std::move(stage)}, streaming_ && runtime.reduction_depth()==foreground_depth_);
#else
    throw std::runtime_error("OS process adapter requires Windows");
#endif
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
        runtime.reduction_depth()==foreground_depth_ &&
        stages.size() == 1 && !stages.front().input;

    if (!foreground) {
        // Only a { } producer makes this a multi-stage OS pipeline; as the
        // foreground form it streams to the terminal like a bare { }.
        const bool live = runtime.reduction_depth()==foreground_depth_ && (streaming_ || stages.size() > 1);
        return run_pipeline(
            runtime,
            std::move(
                stages
            ), live
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
    std::vector<Stage> stages, bool passthrough,
    const std::wstring& output_file, bool append
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
        file_output = Handle(CreateFileW(output_file.c_str(), append ? FILE_APPEND_DATA : GENERIC_WRITE,
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
    if (stages.front().input) {
        auto input_pipe = make_pipe(false);
        null_input = std::move(input_pipe.read);
        input_write = std::move(input_pipe.write);
    } else if(passthrough && streaming_) {
        HANDLE input=nullptr;
        if(!DuplicateHandle(GetCurrentProcess(),GetStdHandle(STD_INPUT_HANDLE),GetCurrentProcess(),&input,0,TRUE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error(windows_error(GetLastError()));
        null_input=Handle(input);
    }

    auto spawn_stage =
        [&job,this](
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
                working_directory_.empty()?nullptr:working_directory_.c_str(),
                &startup.StartupInfo,
                &info
            );

        if (!created) {
            throw std::runtime_error(
                "cannot start " + narrow(stage.program->executable) + ": " +
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

#else

    (void)runtime;
    (void)stages;

    throw std::runtime_error(
        "foreign process realization requires Windows"
    );

#endif
}

seed::Ptr ProcessHost::redirect(seed::Runtime& runtime, seed::Ptr producer, const std::string& path, bool append) {
#ifdef _WIN32
    std::vector<Stage> stages;
    if (seed::Ptr body = runtime.pipeline_body(producer)) {
        // {PIPELINE} > file: the last stage writes the file directly.
        build_braced(runtime, body, stages, false);
        return run_pipeline(runtime, std::move(stages), false, widen(path), append);
    }
    if (runtime.is_relation(producer)) {
        std::vector<seed::Ptr> segments;
        producer = runtime.flow(runtime.as_node(producer), segments);
    }
    if (!build_pipeline(runtime, producer, stages)) return nullptr;
    return run_pipeline(runtime, std::move(stages), false, widen(path), append);
#else
    return nullptr;
#endif
}

// =============================================================================
// Seed-native process result
// =============================================================================

seed::Ptr ProcessHost::make_result(
    seed::Runtime& runtime,
    std::string out,
    std::string err,
    int status,
    const std::vector<int>& stages
) {
    if (!stages.empty()) {
        std::string text;
        for (int stage : stages) text += (text.empty() ? "" : " ") + std::to_string(stage);
        return runtime.product({
            runtime.arena().make_name(runtime.intern("stdout")), runtime.arena().make_literal(out),
            runtime.arena().make_name(runtime.intern("stderr")), runtime.arena().make_literal(err),
            runtime.arena().make_name(runtime.intern("status")), runtime.arena().make_literal(std::to_string(status)),
            runtime.arena().make_name(runtime.intern("stages")), runtime.arena().make_literal(text)});
    }
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
        terms.size() != 6 && terms.size() != 8
    ) {
        return false;
    }
    if (terms.size() == 8 && (!name_is(runtime, terms[6], "stages") || !runtime.is_literal(terms[7])))
        return false;

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
    result.stages.clear();
    if (terms.size() == 8) {
        const std::string& text = runtime.as_literal(terms[7])->bytes;
        for (std::size_t at = 0; at < text.size();) {
            auto space = text.find(' ', at);
            if (space == std::string::npos) space = text.size();
            int stage = 0;
            auto parsed = std::from_chars(text.data() + at, text.data() + space, stage);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + space) return false;
            result.stages.push_back(stage);
            at = space + 1;
        }
    }

    return true;
}

} // namespace hirari
