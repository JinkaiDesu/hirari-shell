#include "hirari/shell.hpp"
#include "hirari/win32.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <sstream>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif
#include <string>
#include <string_view>

namespace hirari {

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
        &ProcessHost::resolve,
        &processes_
    );
    seed_.define("world", &Shell::world, this);
}

int Shell::run() {
    host_.write("ƸӜƷ Hirari · Seed `/\n\n");
    execute_startup(); // Failure reports a diagnostic and still reaches the recovery prompt.
    if (processes_.quitting()) return 0;
    std::string line;
    for (;;) {
#ifdef _WIN32
        if (interactive()) {
            HANDLE input_handle = GetStdHandle(STD_INPUT_HANDLE);
            DWORD saved_mode = 0;
            if (!GetConsoleMode(input_handle, &saved_mode)) break;
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
            continue;
        }
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

seed::Ptr Shell::evaluate(std::string_view source) {
    seed::Ptr form = seed_.read(source);
    if (seed_.is_symbol(form) && !seed_.binding(seed_.as_symbol(form)))
        form = seed_.arena().make_relation({form});
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
    if (!processes_.interactive()) present(result);
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
void Shell::evaluate_script(const std::filesystem::path& path) {
    auto source = read_source(path);
    std::istringstream lines(source);
    std::string line, pending;
    std::size_t number = 0, start = 1;
    execution_status_ = 0;
    while (std::getline(lines, line)) {
        ++number;
        if (pending.empty()) start = number;
        pending += line;
        pending += '\n';
        if (!source_complete(pending)) continue;
        try {
            evaluate(pending);
            if (processes_.interactive()) throw std::runtime_error("interactive programs require the interactive shell");
            if (execution_status_ != 0) throw std::runtime_error("entry program returned status " + std::to_string(execution_status_));
        } catch (const std::exception& error) {
            throw std::runtime_error(path_text(path) + ':' + std::to_string(start) + ": " + error.what());
        }
        pending.clear();
        if (processes_.quitting()) return;
    }
    if (!pending.empty()) throw std::runtime_error(path_text(path) + ':' + std::to_string(start) + ": incomplete Seed expression");
}
bool Shell::execute_file(const std::filesystem::path& path) {
    try { evaluate_script(path); return true; }
    catch (const std::exception& error) { report(error); return false; }
}
bool Shell::execute_startup() {
#ifdef _WIN32
    DWORD size = GetEnvironmentVariableW(L"HIRARI_STARTUP", nullptr, 0);
    if (!size) return true;
    std::wstring source(size, L'\0');
    DWORD count = GetEnvironmentVariableW(L"HIRARI_STARTUP", source.data(), size);
    if (!count || count >= size) { host_.error("error: startup environment changed while reading\n"); execution_status_ = 1; return false; }
    source.resize(count);
    return execute_file(std::filesystem::path(source));
#else
    const char* source = std::getenv("HIRARI_STARTUP");
    return !source || !*source ? true : execute_file(text_path(source));
#endif
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
    if (op == "root") {
        if (a.size() != 2) throw std::runtime_error("world root takes no arguments");
        if (!shell.worlds_.current()) throw std::runtime_error("no world is selected");
        return r.arena().make_literal(path_text(shell.worlds_.current()->root));
    }
    if (op != "open" || a.size() != 3) throw std::runtime_error("usage: world open \"root\" | world status");
    auto root = r.reduce(a[2]);
    if (!r.is_literal(root)) throw std::runtime_error("world root requires a quoted path");
    shell.worlds_.open(text_path(r.as_literal(root)->bytes), [&shell](const auto& path) { shell.evaluate_script(path); });
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
