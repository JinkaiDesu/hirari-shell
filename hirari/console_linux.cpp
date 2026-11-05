#include "hirari/console.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <string>

#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace hirari::console {
namespace {
std::atomic_bool interrupt_seen{false};
std::atomic_bool size_changed{false};
// Bytes read from the terminal that aren't a whole key yet.
std::string typed;
termios keyboard_saved{};

void on_interrupt(int) { interrupt_seen.store(true); }
void on_resize(int) { size_changed.store(true); }

bool wait_readable(int timeout_ms) {
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const int ready = poll(&input, 1, timeout_ms);
    return ready > 0;
}

// Reads what's there into `typed`. False at the end of input.
bool fill() {
    char buffer[256];
    for (;;) {
        const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count > 0) { typed.append(buffer, static_cast<std::size_t>(count)); return true; }
        if (count == 0) return false;
        if (errno != EINTR && errno != EAGAIN) return false;
        if (errno == EINTR) return true;
    }
}

using Special = Key::Special;

void modifiers(Key& key, int parameter) {
    if (parameter <= 1) return;
    const int bits = parameter - 1;
    key.alt = (bits & 2) != 0;
    key.control = (bits & 4) != 0;
}

// An escape sequence after ESC, at `typed`'s start. False while it's still incomplete.
bool parse_escape(Key& key) {
    if (typed.size() < 2) return false;
    const char kind = typed[1];
    if (kind == '[' || kind == 'O') {
        std::size_t at = 2;
        int first = 0, second = 0;
        bool in_second = false;
        while (at < typed.size() && ((typed[at] >= '0' && typed[at] <= '9') || typed[at] == ';')) {
            if (typed[at] == ';') in_second = true;
            else (in_second ? second : first) = (in_second ? second : first) * 10 + (typed[at] - '0');
            ++at;
        }
        if (at >= typed.size()) return false;
        const char final_byte = typed[at];
        typed.erase(0, at + 1);
        switch (final_byte) {
        case 'A': key.special = Special::Up; break;
        case 'B': key.special = Special::Down; break;
        case 'C': key.special = Special::Right; break;
        case 'D': key.special = Special::Left; break;
        case 'H': key.special = Special::Home; break;
        case 'F': key.special = Special::End; break;
        case '~':
            if (first == 1 || first == 7) key.special = Special::Home;
            else if (first == 4 || first == 8) key.special = Special::End;
            else if (first == 3) key.special = Special::Delete;
            break;
        default: break;
        }
        modifiers(key, second);
        return true;
    }
    // Alt held with an ordinary key.
    typed.erase(0, 1);
    key.alt = true;
    return true;
}

// One key from `typed`'s start. False while it's still incomplete.
bool parse_key(Key& key, bool escape_timed_out) {
    if (typed.empty()) return false;
    const auto lead = static_cast<unsigned char>(typed[0]);
    if (lead == 0x1b) {
        if (typed.size() == 1) {
            if (!escape_timed_out) return false;
            typed.erase(0, 1);
            key.character = 27;
            return true;
        }
        if (!parse_escape(key)) return escape_timed_out ? (typed.clear(), key.character = 27, true) : false;
        if (!key.alt || key.special != Special::None) return true;
        return parse_key(key, escape_timed_out);
    }
    if (lead == 0x7f) { typed.erase(0, 1); key.character = 8; key.special = Special::Backspace; return true; }
    if (lead == '\t') { typed.erase(0, 1); key.character = U'\t'; key.special = Special::Tab; return true; }
    if (lead < 0x80) { typed.erase(0, 1); key.character = lead; return true; }
    const std::size_t length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
    if (length == 1) { typed.erase(0, 1); return true; }
    if (typed.size() < length) { if (!escape_timed_out) return false; typed.clear(); return true; }
    char32_t c = lead & (0xff >> (length + 1));
    for (std::size_t k = 1; k < length; ++k) c = (c << 6) | (static_cast<unsigned char>(typed[k]) & 0x3f);
    typed.erase(0, length);
    key.character = c;
    return true;
}

bool key_was_read(const Key& key) { return key.character || key.special != Special::None; }
} // namespace

void start() {
    struct sigaction action{};
    action.sa_handler = on_interrupt;
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    action.sa_handler = on_resize;
    sigaction(SIGWINCH, &action, nullptr);
    // A write to a program that has gone answers EPIPE here; each program starts with the usual SIGPIPE.
    signal(SIGPIPE, SIG_IGN);
}

bool interrupted() { return interrupt_seen.exchange(false); }
bool is_terminal() { return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO); }
void output_lost() { _exit(141); }

Size size() {
    winsize window{};
    if (!ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) && window.ws_col && window.ws_row) return {window.ws_col, window.ws_row};
    return {120, 30};
}

KeyboardMode::KeyboardMode() {
    if (tcgetattr(STDIN_FILENO, &keyboard_saved)) return;
    termios mode = keyboard_saved;
    // Output keeps its ordinary line endings; Ctrl+C reaches the editor as a key.
    mode.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
    mode.c_iflag &= ~static_cast<tcflag_t>(ICRNL | IXON | INLCR | IGNCR);
    mode.c_cc[VMIN] = 1;
    mode.c_cc[VTIME] = 0;
    active_ = tcsetattr(STDIN_FILENO, TCSANOW, &mode) == 0;
}

KeyboardMode::~KeyboardMode() {
    if (active_) tcsetattr(STDIN_FILENO, TCSANOW, &keyboard_saved);
}

bool next_key(Key& key, bool& was_interrupted, Host& host, const std::function<void()>& resized) {
    bool quiet_once = false;
    for (;;) {
        if (host.interrupted()) { was_interrupted = true; return true; }
        if (size_changed.exchange(false) && resized) resized();
        if (!typed.empty()) {
            Key parsed;
            if (parse_key(parsed, quiet_once)) {
                if (!key_was_read(parsed)) continue;
                parsed.pending = !typed.empty() || wait_readable(0);
                key = parsed;
                return true;
            }
        }
        if (wait_readable(30)) {
            if (!fill()) { if (typed.empty()) return false; quiet_once = true; }
            else quiet_once = false;
        } else {
            // A lone ESC with nothing after it is the Escape key.
            quiet_once = !typed.empty();
        }
    }
}

bool ends_input(char32_t character) { return character == 4; }

void run_foreground(Host& host, const Foreground& program) {
    termios saved{};
    const bool terminal = tcgetattr(STDIN_FILENO, &saved) == 0;
    if (terminal) {
        termios raw = saved;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    struct Restore {
        bool terminal; const termios& saved;
        ~Restore() { if (terminal) tcsetattr(STDIN_FILENO, TCSANOW, &saved); }
    } restore{terminal, saved};

    auto update_size = [&] { const auto now = size(); program.resize(now.columns, now.rows); };
    update_size();
    bool input_open = true;
    while (program.running()) {
        if (host.interrupted()) program.interrupt();
        if (size_changed.exchange(false)) update_size();
        pollfd input{STDIN_FILENO, POLLIN, 0};
        if (poll(&input, input_open ? 1 : 0, 30) <= 0) continue;
        if (input.revents & (POLLIN | POLLHUP)) {
            char buffer[4096];
            const auto count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count > 0) program.input(std::string_view(buffer, static_cast<std::size_t>(count)));
            else if (count == 0 || errno != EINTR) input_open = false;
        }
    }
}

} // namespace hirari::console
