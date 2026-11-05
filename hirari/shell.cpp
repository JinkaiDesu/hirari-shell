#include "hirari/shell.hpp"
#include "hirari/platform.hpp"
#include "hirari/regex.hpp"
#include "hirari/files.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <functional>
#include <fstream>
#include <cctype>
#include <string>
#include <string_view>

namespace hirari {

namespace {
struct OnExit { std::function<void()> action; ~OnExit() { action(); } };
std::string lower_case(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
bool source_has_form(std::string_view source) {
    return seed::Reader::source_has_form(source);
}
std::string shell_environment(const char* key) { return platform::environment(key).value_or(std::string{}); }
std::string script_text(seed::Runtime& r, seed::Ptr form) {
    auto value = r.reduce(form);
    ProcessResult process;
    if(ProcessHost::unpack(r,value,process)) {
        if(process.status!=0)throw std::runtime_error("path-producing program failed: "+process.err);
        auto text=process.out;while(!text.empty() && (text.back()=='\n' || text.back()=='\r'))text.pop_back();
        if(text.empty() || text.find_first_of("\r\n")!=std::string::npos || text.find('\0')!=std::string::npos)
            throw std::runtime_error("path-producing program must return one nonempty path");
        return text;
    }
    if (!r.is_literal(value)) throw std::runtime_error("file/program path requires a quoted string");
    return r.as_literal(value)->bytes;
}
// NAME := EXPR, EXPR = NAME and data bind: statements that exist for their binding.
bool definition(seed::Runtime& r, seed::Ptr form) {
    while (r.is_relation(form)) {
        auto terms = r.terms(r.as_node(form));
        if (terms.size() == 1) { form = terms[0]; continue; }
        auto named = [&](seed::Ptr term, std::string_view text) {
            if (r.is_name(term)) term = r.as_name(term)->target;
            return r.is_symbol(term) && r.as_symbol(term)->text == text;
        };
        if (terms.size() < 2) return false;
        return named(terms[0], ":=") || named(terms[1], ":=") || named(terms[0], "=") || named(terms[1], "=") ||
               (named(terms[0], "data") && named(terms[1], "bind"));
    }
    return false;
}
}

Shell::Shell(
    Host& host
)
    : host_(
        host
    ),
      seed_(),
      processes_(
        seed_,
        host_
    ), values_(seed_,processes_,host_),jobs_(values_,host_),control_(seed_,host_) {
    install_regex(values_);
    install_files(values_);
    seed_.set_resolver(
        &Shell::resolve,
        this
    );
    seed_.define("world", &Shell::world, this);
    seed_.define("source", &Shell::script_builtin, this);
    seed_.define("run", &Shell::script_builtin, this);
    seed_.define("arg", &Shell::script_argument, this);
    seed_.define("args", &Shell::script_argument, this);
    seed_.define("argc", &Shell::script_argument, this);
    seed_.define("script-directory", &Shell::script_argument, this);
    seed_.define("path", &Shell::script_argument, this);
    seed_.define("exec", &Shell::exec, this);
    seed_.define("help", seed::Program{&Shell::help, this, seed::ProgramDispatch::IdentityQuery});
    seed_.define("?", seed::Program{&Shell::help, this, seed::ProgramDispatch::IdentityQuery});
    seed_.define("inspect", seed::Program{&Shell::inspect, this, seed::ProgramDispatch::IdentityQuery});
    seed_.define("vocab", &Shell::vocabulary, this);
    // Line blocks behave like files: each line's result is shown and a failing
    // line stops the block. Unknown programs fail wherever they are applied.
    seed_.define("(;)", &Shell::block, this);
    seed_.strict_heads(true);
    processes_.opaque_values([this](seed::Ptr value) { return values_.opaque(value) || values_.structured(value); });
    processes_.stream_resolver([this](seed::Symbol* symbol) {
        if(!prefer_script(symbol->text) && !ProcessHost::executable_path(symbol->text).empty())
            return ProcessHost::StreamProgram{};
        auto path=find_script(symbol->text);if(path.empty())return ProcessHost::StreamProgram{};
        auto executable=platform::own_executable();
        if(executable.empty())throw std::runtime_error("cannot locate Hirari for script stream stage");
        auto sibling=executable.parent_path()/platform::shell_file_name;
        if(std::filesystem::is_regular_file(sibling))executable=sibling;
        return ProcessHost::StreamProgram{path_text(executable),{"--stream-file",path_text(path)}};
    });
    auto directory=shell_environment("HIRARI_STATE");
    if(directory.empty()){
        directory=path_text(platform::default_state_directory());
    }
    if(!directory.empty() && directory!="-") {
        vocabulary_=std::make_unique<Vocabulary>(text_path(directory));
        try{vocabulary_->load(seed_);}catch(const std::exception& error){host_.error(std::string("vocabulary unavailable: ")+error.what()+"\n");}
    }
}

seed::Ptr Shell::block(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto& shell = *static_cast<Shell*>(context);
    auto terms = r.terms(relation);
    if (!r.is_block(relation)) return relation;
    // A leading string documents the block, as it documents a file.
    std::size_t first = terms.size() > 2 && r.is_literal(terms[1]) ? 2 : 1;
    seed::Ptr result = r.arena().unit();
    for (std::size_t i = first; i < terms.size(); ++i) {
        if (shell.host_.interrupted()) throw std::runtime_error("Seed block interrupted");
        shell.quiet_ = nullptr;
        result = r.demand(terms[i]);
        if (definition(r, terms[i])) shell.quiet_ = result;
        if (i + 1 == terms.size()) break;
        ProcessResult process;
        bool failed = ProcessHost::unpack(r, result, process) && process.status != 0;
        if (failed) shell.quiet_ = nullptr;
        shell.present(result);
        if (failed)
            throw std::runtime_error("block line " + std::to_string(i - first + 1) + " (" + r.print(terms[i]) +
                                     ") returned status " + std::to_string(process.status));
    }
    return result;
}

bool Shell::prefer_script(std::string_view name) {
    if(text_path(std::string(name)).extension()==".seed")return true;
    // All-capital command identities prefer vocabulary; this is host lookup,
    // not a SQL keyword list or a change to Seed's Symbol representation.
    bool letter=false;
    for(unsigned char c:name){if(c>='a' && c<='z')return false;if(c>='A' && c<='Z')letter=true;}
    return letter;
}

std::vector<std::filesystem::path> Shell::script_roots(bool search_path) const {
    std::vector<std::filesystem::path> roots;
    if (!script_frames_.empty()) roots.push_back(script_frames_.back().path.parent_path());
    roots.push_back(std::filesystem::current_path());
    if(auto executable=platform::own_executable();!executable.empty())roots.push_back(executable.parent_path()/"Commands");
    if (search_path) {
        for (const auto* key : {"HIRARI_PATH", "PATH"}) {
            std::istringstream paths(shell_environment(key));
            std::string root;
            while (std::getline(paths, root, platform::path_list_separator)) if (!root.empty()) roots.push_back(text_path(root));
        }
    }
    return roots;
}

std::filesystem::path Shell::installed_command(const std::string& name) {
    auto file = text_path(name);
    if (file.has_parent_path() || file.has_extension()) return {};
    auto executable = platform::own_executable();
    if (executable.empty()) return {};
    file += ".seed";
    auto candidate = executable.parent_path() / "Commands" / file;
    std::error_code error;
    return std::filesystem::is_regular_file(candidate, error) ? std::filesystem::canonical(candidate) : std::filesystem::path{};
}

bool Shell::native_first(const std::string& name) {
    return !prefer_script(name) && installed_command(name).empty();
}

std::filesystem::path Shell::find_script(const std::string& name, bool explicit_file) const {
    if (!explicit_file)
        if (auto installed = installed_command(name); !installed.empty()) return installed;
    auto file = text_path(name);
    if (file.extension() != ".seed") {
        if (explicit_file) throw std::runtime_error("Seed programs require a .seed file");
        if (file.has_extension()) return {};
        file += ".seed";
    }
    auto roots = script_roots(!explicit_file && !file.has_parent_path());
    if (file.is_absolute()) roots = {std::filesystem::path{}};
    for (const auto& root : roots) {
        auto candidate = root / file;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return std::filesystem::canonical(candidate);
    }
    // An all-capital command identity names the same command wherever file names keep their case.
    if (!explicit_file && prefer_script(name) && file.stem() != text_path(lower_case(name))) {
        if (auto lowered = find_script(lower_case(name)); !lowered.empty()) return lowered;
    }
    return {};
}

seed::Program Shell::resolve(seed::Runtime& r, seed::Symbol* symbol, seed::Ptr context) {
    auto& shell = *static_cast<Shell*>(context);
    if (r.binding(symbol)) return {};
    if(native_first(symbol->text))
        if(auto native=ProcessHost::resolve(r,symbol,&shell.processes_))return native;
    // Capitalization chooses vocabulary before implicit executable lookup.
    auto path = shell.find_script(symbol->text);
    if (path.empty()) return ProcessHost::resolve(r, symbol, &shell.processes_);
    auto& program = shell.scripts_[symbol];
    if (!program) program = std::make_unique<ScriptProgram>();
    *program = {&shell, std::move(path)};
    return {&Shell::script_command, program.get()};
}

seed::Ptr Shell::script_command(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto& program = *static_cast<ScriptProgram*>(context);
    auto terms = r.terms(relation);
    std::vector<seed::Ptr> arguments;
    for (std::size_t i = 1; i < terms.size(); ++i) arguments.push_back(r.reduce(terms[i]));
    // Copy before nesting: command discovery can refresh this program's cached path.
    auto path = program.path;
    return program.shell->evaluate_script(path, std::move(arguments), true);
}

seed::Ptr Shell::script_builtin(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto terms = r.terms(relation);
    if (terms.empty() || !r.is_symbol(terms[0]) ||
        (r.as_symbol(terms[0])->text != "source" && r.as_symbol(terms[0])->text != "run")) return relation;
    if (terms.size() < 2) throw std::runtime_error("usage: source|run \"file.seed\" [arguments]");
    auto& shell = *static_cast<Shell*>(context);
    auto name = script_text(r, terms[1]);
    auto path = shell.find_script(name, true);
    if (path.empty()) throw std::runtime_error("Seed program not found: " + name);
    std::vector<seed::Ptr> arguments;
    for (std::size_t i = 2; i < terms.size(); ++i) arguments.push_back(r.reduce(terms[i]));
    return shell.evaluate_script(path, std::move(arguments), r.as_symbol(terms[0])->text == "run");
}

seed::Ptr Shell::script_argument(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto terms = r.terms(relation);
    if (terms.empty() || !r.is_symbol(terms[0]) ||
        (r.as_symbol(terms[0])->text != "arg" && r.as_symbol(terms[0])->text != "args" &&
         r.as_symbol(terms[0])->text != "argc" && r.as_symbol(terms[0])->text != "script-directory" && r.as_symbol(terms[0])->text != "path")) return relation;
    auto& shell = *static_cast<Shell*>(context);
    if(r.as_symbol(terms[0])->text=="path") {
        if(terms.size()<2)throw std::runtime_error("usage: path \"ROOT\" \"SEGMENT\" ...");
        std::filesystem::path result;for(std::size_t i=1;i<terms.size();++i)result/=text_path(script_text(r,terms[i]));
        return r.arena().make_literal(path_text(result.lexically_normal()));
    }
    if (shell.script_frames_.empty()) throw std::runtime_error("arg/args require an active Seed program");
    const auto& arguments = shell.script_frames_.back().arguments;
    if(r.as_symbol(terms[0])->text=="argc"){
        if(terms.size()!=1)throw std::runtime_error("argc takes no arguments");return r.arena().make_number(std::to_string(arguments.size()));
    }
    if(r.as_symbol(terms[0])->text=="script-directory"){
        if(terms.size()!=1)throw std::runtime_error("script-directory takes no arguments");
        return r.arena().make_literal(path_text(shell.script_frames_.back().path.parent_path()));
    }
    if (r.as_symbol(terms[0])->text == "args") {
        if (terms.size() != 1) throw std::runtime_error("args takes no arguments");
        return r.arena().make_name(r.product(arguments));
    }
    if (terms.size() != 2) throw std::runtime_error("usage: arg INDEX (zero based)");
    auto index_form = r.reduce(terms[1]);
    if (!r.arena().is(index_form, seed::HostKind::Integer)) throw std::runtime_error("arg INDEX requires a non-negative integer");
    auto text = r.as_number(index_form)->spelling;
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("arg INDEX requires a non-negative integer");
    auto index = std::stoull(text);
    if (index >= arguments.size()) throw std::runtime_error("arg " + text + ": this program received " + std::to_string(arguments.size()) + " argument(s)");
    return arguments[static_cast<std::size_t>(index)];
}

seed::Ptr Shell::exec(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto terms = r.terms(relation);
    if (terms.empty() || !r.is_symbol(terms[0]) || r.as_symbol(terms[0])->text != "exec") return relation;
    if (terms.size() < 2) throw std::runtime_error("usage: exec \"executable\" [arguments]");
    auto path = script_text(r, terms[1]);
    return static_cast<Shell*>(context)->processes_.execute(r, path, {terms.begin() + 2, terms.end()}, relation);
}

void Shell::wait_foreground() {
    console::run_foreground(host_, {
        [this] { return interactive(); },
        [this](std::string_view bytes) { terminal_input(bytes); },
        [this](std::uint16_t columns, std::uint16_t rows) { resize(columns, rows); },
        [this] { interrupt(); },
    });
    processes_.reap_foreground();
}

std::string Shell::prompt() const {
    if (waiting()) return ".....> ";
    auto directory = path_text(std::filesystem::current_path());
    auto home = platform::home_directory();
    if (!home.empty() && directory.size() >= home.size() && directory.compare(0, home.size(), home) == 0 &&
        (directory.size() == home.size() || directory[home.size()] == '\\' || directory[home.size()] == '/'))
        directory = "~" + directory.substr(home.size());
    std::string text;
    if (execution_status_ != 0) text = "\x1b[31m[" + std::to_string(execution_status_) + "]\x1b[0m ";
    return text + "\x1b[36m" + directory + "\x1b[0m > ";
}

int Shell::run() {
    interactive_session_ = true;
    const bool terminal = host_.terminal_capable();
    auto banner=shell_environment("HIRARI_BANNER");
    if(!banner.empty())host_.write(banner+"\n");
    else if(terminal)host_.write("hirari. `help` lists what's here, `help --seed` teaches the language, Tab completes.\n\n");
    // The caller may name a startup program.
    // Evaluate in this session so definitions/world context reach the prompt.
    auto startup = shell_environment("HIRARI_STARTUP");
    auto completion = shell_environment("HIRARI_STARTUP_STATUS");
    // One-shot host request: an interactive child must not boot the world again.
    platform::set_environment("HIRARI_STARTUP", std::nullopt);
    platform::set_environment("HIRARI_STARTUP_STATUS", std::nullopt);
    if (!startup.empty()) {
        std::string failure;
        try {
            present(evaluate_script(text_path(startup)));
        } catch (const ExitRequest& request) {
            if (request.status != 0) failure = "startup program exited with status " + std::to_string(request.status);
        } catch (const std::exception& error) { failure = error.what(); report(error); }
        if (!completion.empty()) {
            try {
                auto destination = text_path(completion);
                auto temporary = destination; temporary += ".tmp";
                {
                    std::ofstream status(temporary, std::ios::binary | std::ios::trunc);
                    status.exceptions(std::ios::failbit | std::ios::badbit);
                    status << (failure.empty() ? "ready\n" : "failed\n" + failure);
                    status.close();
                }
                // The caller supplies a fresh path for every shell it starts.
                std::filesystem::rename(temporary, destination);
            } catch (const std::exception& error) {
                host_.error(std::string("startup completion unavailable: ") + error.what() + '\n');
            }
        }
        if (processes_.quitting()) return status();
    }
    std::string line;
    std::filesystem::path history;
    if (vocabulary_) history = vocabulary_->directory() / "history";
    LineEditor editor(host_, history, [this](std::string_view before) { return complete(before); });
    for (;;) {
        if (interactive()) { wait_foreground(); execution_status_ = processes_.last_status(); continue; }
        if (terminal) {
            auto outcome = editor.read(prompt(), line);
            if (outcome == LineEditor::Outcome::End) break;
            if (outcome == LineEditor::Outcome::Cancelled) { cancel_input(); continue; }
            editor.remember(line);
        } else {
            if (!std::getline(std::cin, line)) break;
            if (host_.interrupted()) { cancel_input(); continue; }
        }
        if (!input(line)) break;
    }
    return processes_.exit_code().value_or(processes_.last_status());
}

bool Shell::source_complete(
    std::string_view source
) {
    return seed::Reader::source_complete(source);
}

bool Shell::input(
    std::string_view line
) {
    // If a terminal child owns foreground, a committed
    // line can still be forwarded conveniently.
    //
    // The console uses terminal_input()
    // so arrows, Ctrl keys, VT sequences, etc. remain raw.
    if (
        processes_.interactive()
    ) {
        std::string bytes(
            line
        );

        bytes.push_back(
            '\r'
        );

        processes_.terminal_input(
            bytes
        );

        return true;
    }

    pending_.append(
        line.data(),
        line.size()
    );
    // Every committed prompt line has a physical ending, just like file input.
    pending_.push_back('\n');

    if (
        !source_complete(
            pending_
        )
    ) {
        return true;
    }

    const std::string source =
        std::move(
            pending_
        );

    pending_.clear();

    execute(
        source
    );

    return !processes_.quitting();
}

bool Shell::terminal_input(
    std::string_view bytes
) {
    return
        processes_.terminal_input(
            bytes
        );
}

void Shell::resize(
    std::uint16_t columns,
    std::uint16_t rows
) {
    processes_.resize(
        columns,
        rows
    );
}

void Shell::interrupt() {
    if (
        processes_.interactive()
    ) {
        processes_.interrupt();
        return;
    }

    cancel_input();
}

seed::Ptr Shell::evaluate(std::string_view source, bool display) {
    auto previous_origin = seed_.binding_origin(), previous_source = seed_.binding_source();
    seed_.binding_origin(script_frames_.empty() ? "session" : "Seed program",
                         script_frames_.empty() ? std::string{} : path_text(script_frames_.back().path));
    OnExit origin_scope{[&] { seed_.binding_origin(previous_origin, previous_source); }};
    auto before=interactive_session_ && script_frames_.empty()?seed_.capture_bindings():std::vector<seed::Cell>{};
    seed::Ptr form = seed_.read(source);
    if (seed_.is_symbol(form) && !seed_.binding(seed_.as_symbol(form)))
        form = seed_.arena().make_relation({form});
    form = seed_.application(form);
    bool managing_vocabulary = false;
    if(seed_.is_relation(form)) {
        auto terms=seed_.terms(seed_.as_node(form));
        managing_vocabulary=!terms.empty() && seed_.is_symbol(terms.front()) && seed_.as_symbol(terms.front())->text=="vocab";
    }
    // A binding already exists even if demanding its initial head fails. Persist
    // the definition on both return and unwind; explicit store operations own
    // their commits and must not be saved a second time by this observer.
    OnExit persistence{[&, managing_vocabulary] {
        if(!vocabulary_ || !interactive_session_ || !script_frames_.empty() || managing_vocabulary)return;
        try {
            for(const auto& cell:seed_.capture_bindings()) {
                auto old=std::find_if(before.begin(),before.end(),[&](const auto& v){return v.symbol==cell.symbol;});
                if(!cell.head || (old!=before.end() && old->revision==cell.revision))continue;
                try {vocabulary_->save(seed_,cell.symbol->text,vocabulary_->annotation(cell.symbol->text),[this](auto p){return values_.opaque(p);});}
                catch(const std::exception& error){host_.error("definition remains session-local: "+cell.symbol->text+": "+error.what()+"\n");}
            }
        } catch(const std::exception& error) {host_.error(std::string("definition persistence failed: ")+error.what()+"\n");}
    }};
    processes_.begin_foreground(form);
    struct ForegroundGuard {
        ProcessHost& processes;
        ~ForegroundGuard() { processes.end_foreground(); }
    } guard{processes_};
    quiet_ = nullptr;
    seed::Ptr result = seed_.reduce(form);
    if (definition(seed_, form)) quiet_ = result;
    if (result == form && seed_.is_relation(form) && !seed_.is_abstraction(form)) {
        auto terms = seed_.terms(seed_.as_node(form));
        if (!terms.empty() && seed_.is_symbol(terms.front()) &&
            !seed_.registry().find(seed_.as_symbol(terms.front())) &&
            !seed_.binding(seed_.as_symbol(terms.front())))
            throw std::runtime_error("program not found: " + seed_.as_symbol(terms.front())->text);
    }
    execution_status_ = 0;
    ProcessResult process;
    if (ProcessHost::unpack(seed_, result, process)) execution_status_ = process.status;
    if (display && !processes_.interactive()) present(result);
    return result;
}
void Shell::report(const std::exception& error) {
    execution_status_ = 1;
    processes_.record_failure();
    std::string text = error.what();
    static constexpr std::string_view missing = "program not found: ";
    if (auto at = text.find(missing); at != std::string::npos)
        if (auto hint = suggestion(text.substr(at + missing.size())); !hint.empty()) text += "." + hint;
    host_.error((host_.terminal_capable() ? "\x1b[31merror:\x1b[0m " : "error: ") + text + '\n');
}
seed::Ptr Shell::execute(std::string_view source) {
    try { return evaluate(source); }
    // Exit is an orderly end of execution, not a failure.
    catch (const ExitRequest&) { return seed_.arena().unit(); }
    catch (const std::exception& error) { report(error); return nullptr; }
}
seed::Ptr Shell::evaluate_script(const std::filesystem::path& path, std::vector<seed::Ptr> arguments, bool local) {
    if (script_frames_.size() >= 64) throw std::runtime_error("Seed program nesting limit (64) exceeded");
    auto source = read_source(path);
    auto saved_bindings = local ? seed_.capture_bindings() : std::vector<seed::Cell>{};
    auto saved_directory = std::filesystem::current_path();
    auto saved_world = worlds_;
    auto saved_world_env = platform::environment("HIRARI_WORLD");
    // Frame facts are values, so grouping (args), (argc) and
    // (script-directory) reads them without inventing zero-argument call syntax.
    std::vector<seed::Cell> frame_cells;
    for(const auto* name:{"args","argc","script-directory"})frame_cells.push_back(*seed_.ensure_cell(seed_.intern(name)));
    script_frames_.push_back({std::filesystem::absolute(path), std::move(arguments)});
    OnExit scope{[&, local] {
        script_frames_.pop_back();
        for(const auto& cell:frame_cells)*seed_.ensure_cell(cell.symbol)=cell;
        if (local) {
            seed_.restore_bindings(saved_bindings);
            std::error_code ignored;
            std::filesystem::current_path(saved_directory, ignored);
            worlds_ = saved_world;
            platform::set_environment("HIRARI_WORLD", saved_world_env);
        }
    }};
    seed_.bind(seed_.intern("args"),seed_.arena().make_name(seed_.product(script_frames_.back().arguments)));
    seed_.bind(seed_.intern("argc"),seed_.arena().make_number(std::to_string(script_frames_.back().arguments.size())));
    seed_.bind(seed_.intern("script-directory"),seed_.arena().make_literal(path_text(script_frames_.back().path.parent_path())));
    std::istringstream lines(source);
    std::string line, pending;
    std::size_t number = 0, start = 1, consumed = 0;
    execution_status_ = 0;
    seed::Ptr result = seed_.arena().unit();
    bool first_form = true, previous_docstring = false;
    while (std::getline(lines, line)) {
        ++number;
        consumed += line.size();
        if (consumed < source.size() && source[consumed] == '\n') ++consumed;
        if (pending.empty()) start = number;
        pending += line;
        pending += '\n';
        if (!source_complete(pending)) continue;
        if (!source_has_form(pending)) { pending.clear(); continue; }
        // Do not commit a complete physical line until the next semantic token
        // is known. A leading | belongs to this expression, even across comments.
        if (seed::Reader::source_leading_pipe(std::string_view(source).substr(consumed))) continue;
        try {
            if (host_.interrupted()) throw Interruption("Seed program interrupted");
            if (!previous_docstring) present(result);
            auto parsed = first_form ? seed_.read(pending) : nullptr;
            result = evaluate(pending, false);
            previous_docstring = first_form && seed_.is_literal(parsed);
            first_form = false;
            if (processes_.interactive()) { wait_foreground(); execution_status_ = processes_.last_status(); result = seed_.arena().unit(); }
            if (execution_status_ != 0) {
                quiet_ = nullptr;
                present(result);
                throw std::runtime_error("program exited with status " + std::to_string(execution_status_));
            }
        } catch (const std::exception& error) {
            // The shell's own commands fail in the caller's terms, without their insides.
            auto roots = script_roots(false);
            if (std::filesystem::absolute(path).parent_path() == roots.back()) throw;
            throw std::runtime_error(path_text(path) + ':' + std::to_string(start) + ": " + error.what());
        }
        pending.clear();
        if (processes_.quitting()) return result;
    }
    if (!pending.empty()) throw std::runtime_error(path_text(path) + ':' + std::to_string(start) + ": incomplete Seed expression");
    return result;
}
bool Shell::execute_file(const std::filesystem::path& path, const std::vector<std::string>& arguments) {
    try {
        std::vector<seed::Ptr> values;
        for (const auto& argument : arguments) values.push_back(seed_.arena().make_literal(argument));
        present(evaluate_script(path, std::move(values)));
        return true;
    }
    catch (const ExitRequest& request) { return request.status == 0; }
    catch (const std::exception& error) { report(error); return false; }
}
seed::Ptr Shell::world(seed::Runtime& r, seed::Node* relation, seed::Ptr context) {
    auto a = r.terms(relation);
    if (a.empty() || !r.is_symbol(a[0]) || r.as_symbol(a[0])->text != "world") return relation;
    auto& shell = *static_cast<Shell*>(context);
    if (a.size() < 2 || !r.is_symbol(a[1])) throw std::runtime_error("usage: world open \"root\" | world status");
    auto op = r.as_symbol(a[1])->text;
    if (op == "status") {
        if (a.size() != 2) throw std::runtime_error("world status takes no arguments");
        return r.arena().make_literal(shell.worlds_.status());
    }
    if (op != "open" || a.size() != 3) throw std::runtime_error("usage: world open \"root\" | world status");
    auto root = r.reduce(a[2]);
    if (!r.is_literal(root)) throw std::runtime_error("world root requires a quoted path");
    shell.worlds_.open(text_path(r.as_literal(root)->bytes), [&shell](const auto& path) { shell.present(shell.evaluate_script(path)); });
    return r.arena().make_literal(shell.worlds_.status());
}

void Shell::cancel_input() {
    pending_.clear();
}

void Shell::present(
    seed::Ptr result
) {
    if (!result || result == quiet_ || seed_.is_unit(result)) {
        return;
    }
    if (descriptions_.contains(result) && seed_.is_literal(result)) {
        host_.write(seed_.as_literal(result)->bytes);return;
    }

    ProcessResult process;

    if (
        ProcessHost::unpack(
            seed_,
            result,
            process
        )
    ) {
        if (
            !process.out.empty()
        ) {
            host_.write(
                process.out
            );
        }

        if (
            !process.err.empty()
        ) {
            host_.error(
                process.err
            );
        }

        return;
    }

    // Iteration returns an ordinary product of body results. Present nested
    // completed process results as output without inventing a result type.
    auto contains_process = [this](auto&& visit, seed::Ptr form) -> bool {
        ProcessResult value;
        if (ProcessHost::unpack(seed_, form, value)) return true;
        if (seed_.is_name(form)) return visit(visit, seed_.as_name(form)->target);
        if (!seed_.is_relation(form)) return false;
        for (auto term : seed_.terms(seed_.as_node(form)))
            if (visit(visit, term)) return true;
        return false;
    };
    if (seed_.is_name(result) && contains_process(contains_process, result)) {
        present(seed_.as_name(result)->target);
        return;
    }
    if (seed_.is_relation(result) && contains_process(contains_process, result)) {
        for (auto term : seed_.terms(seed_.as_node(result))) present(term);
        return;
    }

    const std::string text = values_.show(result);
    if (text.empty()) return;
    host_.write(text);
    host_.write("\n");
}

} // namespace hirari
