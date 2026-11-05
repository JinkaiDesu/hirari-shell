#include "hirari/console.hpp"
#include "hirari/win32.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include <fcntl.h>
#include <io.h>

namespace hirari::console {
namespace {
std::atomic_bool interrupt_seen{false};

BOOL WINAPI console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    interrupt_seen.store(true);
    return TRUE;
}
}

void start() {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleCtrlHandler(console_control, TRUE);
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    DWORD mode;
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(output, &mode))
        SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

bool interrupted() { return interrupt_seen.exchange(false); }

bool is_terminal() {
    DWORD mode;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) &&
           GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

void output_lost() {}

Size size() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return {static_cast<std::uint16_t>(std::max(1, info.srWindow.Right - info.srWindow.Left + 1)),
                static_cast<std::uint16_t>(std::max(1, info.srWindow.Bottom - info.srWindow.Top + 1))};
    return {120, 30};
}

KeyboardMode::KeyboardMode() {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved = 0;
    active_ = GetConsoleMode(input, &saved) != 0;
    saved_ = saved;
    SetConsoleMode(input, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
}

KeyboardMode::~KeyboardMode() {
    if (active_) SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_);
}

bool next_key(Key& key, bool& was_interrupted, Host& host, const std::function<void()>& resized) {
    HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t high = 0;
    for (;;) {
        if (host.interrupted()) { was_interrupted = true; return true; }
        DWORD ready = WaitForSingleObject(console, 30);
        if (ready == WAIT_TIMEOUT) continue;
        if (ready != WAIT_OBJECT_0) return false;
        INPUT_RECORD event{};
        DWORD count = 0;
        if (!ReadConsoleInputW(console, &event, 1, &count)) return false;
        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) { if (resized) resized(); continue; }
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
        const auto& record = event.Event.KeyEvent;
        wchar_t c = record.uChar.UnicodeChar;
        if (c >= 0xd800 && c <= 0xdbff) { high = c; continue; }
        key.character = high && c >= 0xdc00 && c <= 0xdfff
            ? 0x10000 + ((static_cast<char32_t>(high) - 0xd800) << 10) + (static_cast<char32_t>(c) - 0xdc00)
            : static_cast<char32_t>(c);
        high = 0;
        const auto vk = record.wVirtualKeyCode;
        using Special = Key::Special;
        switch (vk) {
        case VK_TAB: key.special = Special::Tab; break;
        case VK_LEFT: key.special = Special::Left; break;
        case VK_RIGHT: key.special = Special::Right; break;
        case VK_HOME: key.special = Special::Home; break;
        case VK_END: key.special = Special::End; break;
        case VK_UP: key.special = Special::Up; break;
        case VK_DOWN: key.special = Special::Down; break;
        case VK_DELETE: key.special = Special::Delete; break;
        case VK_BACK: key.special = Special::Backspace; break;
        default: key.special = Special::None; break;
        }
        key.control = (record.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        key.alt = (record.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        DWORD queued = 0;
        key.pending = GetNumberOfConsoleInputEvents(console, &queued) && queued > 1;
        // Modifiers arrive as keys of their own, and they aren't input.
        if (!key.character && (!vk || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL ||
                               vk == VK_NUMLOCK || vk == VK_SCROLL || vk == VK_LWIN || vk == VK_RWIN)) continue;
        return true;
    }
}

bool ends_input(char32_t character) { return character == 4 || character == 26; }

void run_foreground(Host& host, const Foreground& program) {
    HANDLE input_handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved_mode = 0;
    if (!GetConsoleMode(input_handle, &saved_mode)) throw std::runtime_error("foreground terminal input is unavailable");
    SetConsoleMode(input_handle, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    struct ModeGuard {
        HANDLE input; DWORD mode;
        ~ModeGuard() { SetConsoleMode(input, mode); }
    } guard{input_handle, saved_mode};
    auto update_size = [&] { const auto now = size(); program.resize(now.columns, now.rows); };
    update_size();
    wchar_t high_surrogate = 0;
    while (program.running()) {
        if (host.interrupted()) program.interrupt();
        DWORD ready = WaitForSingleObject(input_handle, 30);
        if (ready == WAIT_TIMEOUT) continue;
        if (ready != WAIT_OBJECT_0) { program.interrupt(); break; }
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
            int written = WideCharToMultiByte(CP_UTF8, 0, chars, length, utf8, 8, nullptr, nullptr);
            bytes.assign(utf8, static_cast<std::size_t>(written));
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
        for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) program.input(bytes);
    }
}

} // namespace hirari::console
