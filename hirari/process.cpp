#include "hirari/process.hpp"
#include "hirari/console.hpp"
#include "hirari/platform.hpp"
#include "hirari/world.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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
    runtime.describe("{}", "{ PRODUCER | CONSUMER ... }  { CONSUMER < \"in\" }  { PRODUCER > \"out\" }  { PRODUCER >> \"out\" }\nPOSIX stream context. Inside braces | is a real OS pipe, < reads the first stage's stdin from a file, > writes and >> appends the last stage's stdout to a file. Every stage is a native process (a PATH executable or exec \"path\" ...); stages run concurrently with ordinary backpressure. A Seed value can also be written: { VALUE > \"file\" } writes text as bytes and structured values as a hirari.values/1 envelope. At top level output streams to the terminal; CONSUMER {PIPELINE} feeds a native consumer live. The answer is --stdout --stderr --status, plus --stages with each stage status. Outside braces | is reverse application (b | a == a b == a(b)) and < > compare values.");
}

ProcessHost::~ProcessHost() { terminal_.reset(); }
std::string ProcessHost::executable_path(const std::string& name) {
    return platform::find_executable(name);
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
    if (terminal_ && !terminal_->running()) terminal_.reset();
}

seed::Ptr ProcessHost::directory(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    auto* self = static_cast<ProcessHost*>(context);
    auto terms = runtime.terms(relation);
    if (!runtime.is_symbol(terms.front())) return relation;
    const auto& command = runtime.as_symbol(terms.front())->text;
    if (command != "cd" && command != "pwd") return relation;
    if (command == "cd") {
        if (terms.size() > 2) throw std::runtime_error("cd takes one path");
        std::string path;
        if (terms.size() == 2) path = self->argument(runtime, terms[1]);
        else {
            path = platform::home_directory();
            if (path.empty()) throw std::runtime_error("cd: no home directory is set");
        }
        std::error_code error;
        std::filesystem::current_path(text_path(path), error);
        if (error) throw std::runtime_error("cd: " + path + ": " + error.message());
        self->last_status_.store(0);
        return runtime.arena().unit();
    }
    if (terms.size() != 1) throw std::runtime_error("pwd takes no arguments");
    auto path = path_text(std::filesystem::current_path());
    self->last_status_.store(0);
    return self->make_result(runtime, path + "\n", {}, 0);
}

// =============================================================================
// Foreground / terminal
// =============================================================================

void ProcessHost::begin_foreground(
    seed::Ptr root
) {
    if (
        terminal_ &&
        !terminal_->running()
    ) {
        terminal_.reset();
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
        terminal_ &&
        terminal_->running();
}

bool ProcessHost::terminal_input(
    std::string_view bytes
) {
    if (!interactive()) {
        return false;
    }

    terminal_->write(
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

    terminal_->resize(
        columns,
        rows
    );
}

void ProcessHost::interrupt() {
    if (!interactive()) {
        return;
    }

    terminal_->interrupt();
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

    std::string executable = platform::find_executable(symbol->text);
    if (executable.empty()) return {};

    auto& foreign = programs_[symbol];
    if (!foreign) foreign = std::make_unique<ForeignProgram>();
    foreign->owner = this;
    foreign->symbol = symbol;
    foreign->executable = std::move(executable);
    return seed::Program{&ProcessHost::invoke, foreign.get()};
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
    // Inside { } the stream operators are < (stdin from a file), > and >>
    // (stdout to a file) and | (a live pipe). Outside, < and > compare.
    Redirection io;
    if (!runtime.is_unit(body)) body = self->strip_redirection(runtime, body, io);
    std::vector<seed::Ptr> segments;
    bool piped = !runtime.is_unit(body) && runtime.flow(runtime.as_node(body), segments) != body;
    // { VALUE > "file" }: a Seed value (text, structured data, a help answer)
    // rather than a process writes the file as bytes or a value envelope.
    if (!io.output.empty() && !piped && !self->is_foreign_program(runtime, body) && !runtime.pipeline_body(body)) {
        if (!io.input.empty()) throw std::runtime_error("{} < \"file\" needs a native consumer");
        if (!self->value_writer_) throw std::runtime_error("{} cannot write values to files in this host");
        auto parts = runtime.terms(runtime.as_node(body));
        auto value = runtime.demand(parts.size() == 1 ? parts.front() : body);
        self->value_writer_(runtime, value, io.output, io.append);
        ProcessResult process;
        return unpack(runtime, value, process) ? value : runtime.arena().unit();
    }
    std::vector<Stage> stages;
    self->build_braced(runtime, body, stages, false);
    if (!io.input.empty()) {
        if (stages.front().input) throw std::runtime_error("{} first stage has stdin from < and from its own producer");
        stages.front().input_file = io.input;
    }
    if (!io.output.empty()) return self->run_pipeline(runtime, std::move(stages), false, io.output, io.append);
    // A pipeline evaluated as the foreground form streams to the shell's own
    // output as bytes arrive; nested ones answer --stdout --stderr --status.
    return self->run_pipeline(runtime, std::move(stages), runtime.reduction_depth() == self->foreground_depth_);
}

seed::Ptr ProcessHost::strip_redirection(seed::Runtime& runtime, seed::Ptr body, Redirection& io) {
    auto terms = runtime.terms(runtime.as_node(body));
    auto is = [&](seed::Ptr p, const char* text) { return symbol_is(runtime, p, text); };
    auto path = [&](seed::Ptr form, const char* op) {
        auto value = runtime.reduce(form);
        if (!runtime.is_literal(value))
            throw std::runtime_error(std::string("{} ") + op + " needs a quoted file path, e.g. " + op + " \"file.txt\"");
        return runtime.as_literal(value)->bytes;
    };
    std::vector<seed::Ptr> kept;
    bool seen_pipe = false;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (is(terms[i], "|")) seen_pipe = true;
        bool input = is(terms[i], "<");
        bool output = is(terms[i], ">") || is(terms[i], ">>");
        if (!input && !output) { kept.push_back(terms[i]); continue; }
        const char* op = input ? "<" : is(terms[i], ">>") ? ">>" : ">";
        if (i + 1 >= terms.size() || is(terms[i + 1], "|"))
            throw std::runtime_error(std::string("{} ") + op + " needs a file after it");
        if (input) {
            if (!io.input.empty()) throw std::runtime_error("{} has more than one < input");
            if (seen_pipe) throw std::runtime_error("{} < belongs to the first stage; later stages read the pipe");
            io.input = path(terms[++i], op);
        } else {
            if (!io.output.empty()) throw std::runtime_error("{} has more than one output redirection");
            if (i + 2 != terms.size() &&
                !(i + 3 < terms.size() && is(terms[i + 2], "<")))
                throw std::runtime_error(std::string("{} ") + op + " \"file\" must end the pipeline");
            io.append = is(terms[i], ">>");
            io.output = path(terms[++i], op);
        }
    }
    if (kept.empty()) throw std::runtime_error("{} redirection needs a producer");
    return runtime.arena().make_relation(kept);
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
            if (flag.starts_with("--(")) return "--" + runtime.as_symbol(target)->text;
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
        auto terms=runtime.terms(runtime.as_node(form));
        auto executable=platform::find_executable(argument(runtime,terms[1]));
        if(executable.empty())throw std::runtime_error("explicit executable not found: "+argument(runtime,terms[1]));
        auto& retained=explicit_programs_[executable];
        if(!retained)retained=std::make_unique<ForeignProgram>();
        *retained={this,runtime.intern("exec"),std::move(executable)};
        program=retained.get();
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
    auto executable = platform::find_executable(path);
    if (executable.empty()) throw std::runtime_error("program not found: " + path);
    ForeignProgram program{this, runtime.intern(path), std::move(executable)};
    Stage stage{&program, {}};
    stage.input=std::move(input);
    for (auto value : arguments) stage.arguments.push_back(argument(runtime, value));
    (void)invocation;
    return run_pipeline(runtime, {std::move(stage)}, streaming_ && runtime.reduction_depth()==foreground_depth_);
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
        terminal_ &&
        terminal_->running()
    ) {
        throw std::runtime_error(
            "foreground terminal is already occupied"
        );
    }

    terminal_ =
        std::make_unique<
            TerminalSession
        >(
            host_
        );

    Stage stage =
        std::move(
            stages.front()
        );

    const auto size = console::size();
    terminal_->start(
        stage.program
            ->executable,
        stage.arguments,
        size.columns,
        size.rows,
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


seed::Ptr ProcessHost::redirect(seed::Runtime& runtime, seed::Ptr producer, const std::string& path, bool append) {
    std::vector<Stage> stages;
    if (seed::Ptr body = runtime.pipeline_body(producer)) {
        // {PIPELINE} > file: the last stage writes the file directly.
        build_braced(runtime, body, stages, false);
        return run_pipeline(runtime, std::move(stages), false, path, append);
    }
    if (runtime.is_relation(producer)) {
        std::vector<seed::Ptr> segments;
        producer = runtime.flow(runtime.as_node(producer), segments);
    }
    if (!build_pipeline(runtime, producer, stages)) return nullptr;
    return run_pipeline(runtime, std::move(stages), false, path, append);
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
