#include "hirari/shell.hpp"
#include "hirari/win32.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <functional>
#include <cctype>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <string>
#include <string_view>

namespace hirari {

namespace {
struct OnExit { std::function<void()> action; ~OnExit() { action(); } };
bool source_has_form(std::string_view source) {
    std::size_t i = 0;
    while (i < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[i]))) { ++i; continue; }
        if (source[i] != '#') return true;
        if (i + 1 < source.size() && source[i + 1] == '[') {
            i += 2;
            unsigned depth = 1;
            while (i < source.size() && depth) {
                if (source[i] == '#' && i + 1 < source.size() && source[i + 1] == '[') { ++depth; i += 2; }
                else if (source[i] == ']') { --depth; ++i; }
                else ++i;
            }
        } else {
            while (i < source.size() && source[i] != '\n') ++i;
        }
    }
    return false;
}
std::string shell_environment(const wchar_t* key) {
#ifdef _WIN32
    DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (!size) return {};
    std::wstring value(size, L'\0');
    value.resize(GetEnvironmentVariableW(key, value.data(), size));
    return win32::narrow(value);
#else
    return {};
#endif
}
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
    ), unity_(seed_, host_) {
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
    seed_.define("help", &Shell::help, this);
    seed_.define("?", &Shell::help, this);
    seed_.define("vocab", &Shell::vocabulary, this);
    processes_.opaque_values([this](seed::Ptr value) { return unity_.foreign(value) != nullptr; });
    processes_.stream_resolver([this](seed::Symbol* symbol) {
        auto path=find_script(symbol->text);if(path.empty())return ProcessHost::StreamProgram{};
#ifdef _WIN32
        std::wstring executable(32768,L'\0');auto length=GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size()));
        if(!length || length>=executable.size())throw std::runtime_error("cannot locate Hirari for script stream stage");
        executable.resize(length);
        auto sibling=std::filesystem::path(executable).parent_path()/L"hirari.exe";
        if(std::filesystem::is_regular_file(sibling))executable=sibling.wstring();
        return ProcessHost::StreamProgram{executable,{"--stream-file",path_text(path)}};
#else
        return ProcessHost::StreamProgram{};
#endif
    });
    auto directory=shell_environment(L"HIRARI_STATE");
    if(directory.empty()){
        auto local=shell_environment(L"LOCALAPPDATA");
        if(!local.empty())directory=path_text(text_path(local)/"Hirari"/"vocabulary");
    }
    if(!directory.empty() && directory!="-") {
        vocabulary_=std::make_unique<Vocabulary>(text_path(directory));
        try{vocabulary_->load(seed_);}catch(const std::exception& error){host_.error(std::string("vocabulary unavailable: ")+error.what()+"\n");}
    }
}

std::filesystem::path Shell::find_script(const std::string& name, bool explicit_file) const {
    auto file = text_path(name);
    if (file.extension() != ".seed") {
        if (explicit_file) throw std::runtime_error("Seed programs require a .seed file");
        if (file.has_extension()) return {};
        file += ".seed";
    }
    std::vector<std::filesystem::path> roots;
    if (!script_frames_.empty()) roots.push_back(script_frames_.back().path.parent_path());
    roots.push_back(std::filesystem::current_path());
    auto system=shell_environment(L"SKUNK_SYSTEM");
    if(!system.empty())roots.push_back(text_path(system)/"Commands");
    if (!explicit_file && !file.has_parent_path()) {
        for (const auto* key : {L"HIRARI_PATH", L"PATH"}) {
            std::istringstream paths(shell_environment(key));
            std::string root;
            while (std::getline(paths, root, ';')) if (!root.empty()) roots.push_back(text_path(root));
        }
    }
    if (file.is_absolute()) roots = {std::filesystem::path{}};
    for (const auto& root : roots) {
        auto candidate = root / file;
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error)) return std::filesystem::canonical(candidate);
    }
    return {};
}

seed::Program Shell::resolve(seed::Runtime& r, seed::Symbol* symbol, seed::Ptr context) {
    auto& shell = *static_cast<Shell*>(context);
    if (r.binding(symbol)) return {};
    // Native programs retain PATH priority; explicitly named .seed files bypass it.
    if (text_path(symbol->text).extension() != ".seed")
        if (auto program = ProcessHost::resolve(r, symbol, &shell.processes_)) return program;
    auto path = shell.find_script(symbol->text);
    if (path.empty()) return {};
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
        if(terms.size()!=1)throw std::runtime_error("argc takes no arguments");return r.intern(std::to_string(arguments.size()));
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
    if (!r.is_symbol(index_form)) throw std::runtime_error("arg INDEX requires a non-negative integer");
    auto text = r.as_symbol(index_form)->text;
    if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("arg INDEX requires a non-negative integer");
    auto index = std::stoull(text);
    if (index >= arguments.size()) throw std::runtime_error("script argument out of range: " + text);
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
#ifdef _WIN32
            HANDLE input_handle = GetStdHandle(STD_INPUT_HANDLE);
            DWORD saved_mode = 0;
            if (!GetConsoleMode(input_handle, &saved_mode)) throw std::runtime_error("foreground terminal input is unavailable");
            SetConsoleMode(input_handle, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
            struct ModeGuard {
                HANDLE input; DWORD mode;
                ~ModeGuard() { SetConsoleMode(input, mode); }
            } guard{input_handle, saved_mode};
            auto update_size = [this]() {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
                    resize(static_cast<std::uint16_t>(info.srWindow.Right - info.srWindow.Left + 1),
                           static_cast<std::uint16_t>(info.srWindow.Bottom - info.srWindow.Top + 1));
            };
            update_size();
            wchar_t high_surrogate = 0;
            while (interactive()) {
                if (host_.interrupted()) interrupt();
                DWORD ready = WaitForSingleObject(input_handle, 30);
                if (ready == WAIT_TIMEOUT) continue;
                if (ready != WAIT_OBJECT_0) { processes_.interrupt(); break; }
                INPUT_RECORD event{};
                DWORD count = 0;
                if (!ReadConsoleInputW(input_handle, &event, 1, &count)) break;
                if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) { update_size(); continue; }
                if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
                const auto& key = event.Event.KeyEvent;
                std::string bytes;
                wchar_t c = key.uChar.UnicodeChar;
                if (c) {
                    if (c >= 0xd800 && c <= 0xdbff) { high_surrogate = c; continue; }
                    wchar_t chars[2]{c, 0};
                    int length = 1;
                    if (high_surrogate && c >= 0xdc00 && c <= 0xdfff) {
                        chars[0] = high_surrogate; chars[1] = c; length = 2;
                    }
                    high_surrogate = 0;
                    char utf8[8];
                    int size = WideCharToMultiByte(CP_UTF8, 0, chars, length, utf8, 8, nullptr, nullptr);
                    bytes.assign(utf8, static_cast<std::size_t>(size));
                } else {
                    switch (key.wVirtualKeyCode) {
                    case VK_UP: bytes = "\x1b[A"; break;
                    case VK_DOWN: bytes = "\x1b[B"; break;
                    case VK_RIGHT: bytes = "\x1b[C"; break;
                    case VK_LEFT: bytes = "\x1b[D"; break;
                    case VK_HOME: bytes = "\x1b[H"; break;
                    case VK_END: bytes = "\x1b[F"; break;
                    case VK_INSERT: bytes = "\x1b[2~"; break;
                    case VK_DELETE: bytes = "\x1b[3~"; break;
                    case VK_PRIOR: bytes = "\x1b[5~"; break;
                    case VK_NEXT: bytes = "\x1b[6~"; break;
                    default:
                        if (key.wVirtualKeyCode >= VK_F1 && key.wVirtualKeyCode <= VK_F4)
                            bytes = std::string("\x1bO") + static_cast<char>('P' + key.wVirtualKeyCode - VK_F1);
                        else if (key.wVirtualKeyCode >= VK_F5 && key.wVirtualKeyCode <= VK_F12) {
                            static constexpr int codes[]{15, 17, 18, 19, 20, 21, 23, 24};
                            bytes = "\x1b[" + std::to_string(codes[key.wVirtualKeyCode - VK_F5]) + "~";
                        }
                        break;
                    }
                }
                const bool alt = (key.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
                const bool ctrl = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                const bool shift = (key.dwControlKeyState & SHIFT_PRESSED) != 0;
                const int modifier = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
                if (!c && modifier > 1 && !bytes.empty()) {
                    if (bytes[1] == 'O') bytes = "\x1b[1;" + std::to_string(modifier) + bytes.back();
                    else if (bytes.back() == '~') bytes.insert(bytes.size() - 1, ";" + std::to_string(modifier));
                    else bytes = "\x1b[1;" + std::to_string(modifier) + bytes.back();
                } else if (c && alt && !ctrl) bytes.insert(0, "\x1b");
                for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) terminal_input(bytes);
            }
            processes_.reap_foreground();
#endif
}

int Shell::run() {
    interactive_session_ = true;
    host_.write("ƸӜƷ Hirari · Seed `/\n\n");
    std::string line;
    for (;;) {
#ifdef _WIN32
        if (interactive()) { wait_foreground(); continue; }
#endif
        host_.write(waiting() ? ".....> " : "hirari> ");
#ifdef _WIN32
        if (host_.terminal_capable()) {
            HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
            DWORD saved_mode = 0;
            GetConsoleMode(console, &saved_mode);
            SetConsoleMode(console, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
            struct InputModeGuard {
                HANDLE console; DWORD mode;
                ~InputModeGuard() { SetConsoleMode(console, mode); }
            } guard{console, saved_mode};
            std::wstring text;
            bool cancelled = false, ended = false, entered = false;
            while (!entered && !cancelled && !ended) {
                if (host_.interrupted()) { cancelled = true; break; }
                DWORD ready = WaitForSingleObject(console, 30);
                if (ready == WAIT_TIMEOUT) continue;
                if (ready != WAIT_OBJECT_0) { ended = true; break; }
                INPUT_RECORD event{};
                DWORD count = 0;
                if (!ReadConsoleInputW(console, &event, 1, &count)) { ended = true; break; }
                if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
                const auto& key = event.Event.KeyEvent;
                wchar_t c = key.uChar.UnicodeChar;
                if (c == 3) { cancelled = true; break; }
                if ((c == 4 || c == 26) && text.empty()) { ended = true; break; }
                if (c == '\r' || c == '\n') { entered = true; break; }
                for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) {
                    if (c == '\b') {
                        if (!text.empty()) {
                            wchar_t last = text.back();
                            text.pop_back();
                            if (last >= 0xdc00 && last <= 0xdfff && !text.empty()) text.pop_back();
                            host_.write("\b \b");
                        }
                    } else if (c >= L' ' || c == L'\t') {
                        text.push_back(c);
                        if (c >= 0xd800 && c <= 0xdbff) continue;
                        std::wstring_view echo(&text.back(), 1);
                        if (c >= 0xdc00 && c <= 0xdfff && text.size() >= 2)
                            echo = std::wstring_view(text.data() + text.size() - 2, 2);
                        host_.write(win32::narrow(echo));
                    }
                }
            }
            if (ended) break;
            if (cancelled) { cancel_input(); host_.write("^C\n"); continue; }
            host_.write("\n");
            line = win32::narrow(text);
        } else
#endif
        {
            if (!std::getline(std::cin, line)) break;
            if (host_.interrupted()) { cancel_input(); continue; }
        }
        if (!input(line)) break;
    }
    return processes_.last_status();
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

    if (!pending_.empty()) {
        pending_.push_back(
            '\n'
        );
    }

    pending_.append(
        line.data(),
        line.size()
    );

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
    auto before=interactive_session_ && script_frames_.empty()?seed_.capture_bindings():std::vector<seed::Cell>{};
    seed::Ptr form = seed_.read(source);
    if (seed_.is_symbol(form) && !seed_.binding(seed_.as_symbol(form)))
        form = seed_.arena().make_relation({form});
    bool inspecting=false;
    if(seed_.is_relation(form)) {
        auto terms=seed_.terms(seed_.as_node(form));
        inspecting=!terms.empty() && ((seed_.is_symbol(terms.front()) &&
            (seed_.as_symbol(terms.front())->text=="?" || seed_.as_symbol(terms.front())->text=="help")) ||
            (seed_.is_symbol(terms.back()) && seed_.as_symbol(terms.back())->text=="help"));
    }
    if(!inspecting)form = seed_.application(form);
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
                try {vocabulary_->save(seed_,cell.symbol->text,vocabulary_->annotation(cell.symbol->text),[this](auto p){return unity_.foreign(p)!=nullptr;});}
                catch(const std::exception& error){host_.error("definition remains session-local: "+cell.symbol->text+": "+error.what()+"\n");}
            }
        } catch(const std::exception& error) {host_.error(std::string("definition persistence failed: ")+error.what()+"\n");}
    }};
    processes_.begin_foreground(form);
    struct ForegroundGuard {
        ProcessHost& processes;
        ~ForegroundGuard() { processes.end_foreground(); }
    } guard{processes_};
    seed::Ptr result = seed_.reduce(form);
    if (result == form && seed_.is_relation(form)) {
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
    host_.error(std::string("error: ") + error.what() + '\n');
}
seed::Ptr Shell::execute(std::string_view source) {
    try { return evaluate(source); }
    catch (const std::exception& error) { report(error); return nullptr; }
}
seed::Ptr Shell::evaluate_script(const std::filesystem::path& path, std::vector<seed::Ptr> arguments, bool local) {
    if (script_frames_.size() >= 64) throw std::runtime_error("Seed program nesting limit (64) exceeded");
    auto source = read_source(path);
    auto saved_bindings = local ? seed_.capture_bindings() : std::vector<seed::Cell>{};
    auto saved_directory = std::filesystem::current_path();
    auto saved_world = worlds_;
    auto saved_world_env = shell_environment(L"SKUNK_WORLD");
    script_frames_.push_back({std::filesystem::absolute(path), std::move(arguments)});
    OnExit scope{[&, local] {
        script_frames_.pop_back();
        if (local) {
            seed_.restore_bindings(saved_bindings);
            std::error_code ignored;
            std::filesystem::current_path(saved_directory, ignored);
            worlds_ = saved_world;
#ifdef _WIN32
            auto wide = win32::widen(saved_world_env);
            SetEnvironmentVariableW(L"SKUNK_WORLD", wide.empty() ? nullptr : wide.c_str());
#endif
        }
    }};
    std::istringstream lines(source);
    std::string line, pending;
    std::size_t number = 0, start = 1;
    execution_status_ = 0;
    seed::Ptr result = seed_.arena().unit();
    while (std::getline(lines, line)) {
        ++number;
        if (pending.empty()) start = number;
        pending += line;
        pending += '\n';
        if (!source_complete(pending)) continue;
        if (!source_has_form(pending)) { pending.clear(); continue; }
        try {
            if (host_.interrupted()) throw std::runtime_error("Seed program interrupted");
            present(result);
            result = evaluate(pending, false);
            if (processes_.interactive()) { wait_foreground(); execution_status_ = processes_.last_status(); result = seed_.arena().unit(); }
            if (execution_status_ != 0) {
                present(result);
                throw std::runtime_error("entry program returned status " + std::to_string(execution_status_));
            }
        } catch (const std::exception& error) {
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
    if (!result || seed_.is_unit(result)) {
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

    const std::string text =
        seed_.print(
            result
        );

    host_.write(
        text
    );

    host_.write(
        "\n"
    );
}

} // namespace hirari
