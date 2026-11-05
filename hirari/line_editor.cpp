#include "hirari/line_editor.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace hirari {
namespace {
constexpr std::size_t history_limit = 1000;

std::u32string decode(std::string_view bytes) {
    std::u32string out;
    for (std::size_t i = 0; i < bytes.size();) {
        auto lead = static_cast<unsigned char>(bytes[i]);
        std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : lead < 0xf0 ? 3 : 4;
        char32_t c = length == 1 ? lead : lead & (0xff >> (length + 1));
        for (std::size_t k = 1; k < length && i + k < bytes.size(); ++k)
            c = (c << 6) | (static_cast<unsigned char>(bytes[i + k]) & 0x3f);
        out.push_back(c);
        i += length;
    }
    return out;
}

std::string encode(std::u32string_view text) {
    std::string out;
    for (char32_t c : text) {
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else if (c < 0x800) { out.push_back(static_cast<char>(0xc0 | (c >> 6))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else if (c < 0x10000) { out.push_back(static_cast<char>(0xe0 | (c >> 12))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
        else { out.push_back(static_cast<char>(0xf0 | (c >> 18))); out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f))); out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f))); out.push_back(static_cast<char>(0x80 | (c & 0x3f))); }
    }
    return out;
}

int cell_width(char32_t c) {
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f)) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3041 && c <= 0x4dbf) ||
        (c >= 0x4e00 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) ||
        (c >= 0xfe30 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
        (c >= 0x1f300 && c <= 0x1faff) || (c >= 0x20000 && c <= 0x3fffd)) return 2;
    return 1;
}

// Where text lands on a terminal `width` cells wide, wrapping as it does.
struct Position {
    int row = 0, column = 0;
    void advance(char32_t c, int width) {
        int cells = cell_width(c);
        if (column + cells > width) { ++row; column = 0; }
        column += cells;
        if (column >= width) { ++row; column = 0; }
    }
    void advance(std::u32string_view text, int width) { for (char32_t c : text) advance(c, width); }
};

// Colour sequences take no cells.
std::u32string visible(std::string_view prompt) {
    std::u32string out;
    for (char32_t c : decode(prompt)) {
        if (c == 0x1b) out.push_back(c);
        else if (!out.empty() && out.back() == 0x1b) { if (c != U'[' && !(c >= U'0' && c <= U'9') && c != U';') out.pop_back(); }
        else out.push_back(c);
    }
    return out;
}

bool word_character(char32_t c) {
    return c >= 0x80 || std::isalnum(static_cast<int>(c)) || c == U'_' || c == U'-';
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string shared_prefix(const std::vector<std::string>& values) {
    auto prefix = values.front();
    for (const auto& value : values) {
        std::size_t i = 0;
        while (i < prefix.size() && i < value.size() && lower(prefix[i]) == lower(value[i])) ++i;
        prefix.resize(i);
    }
    // A cut through a UTF-8 sequence is not text.
    while (!prefix.empty() && (static_cast<unsigned char>(prefix.back()) & 0xc0) == 0x80) prefix.pop_back();
    if (!prefix.empty() && static_cast<unsigned char>(prefix.back()) >= 0xc0) prefix.pop_back();
    return prefix;
}
}

LineEditor::LineEditor(Host& host, std::filesystem::path history_file, Completer completer)
    : host_(host), history_file_(std::move(history_file)), completer_(std::move(completer)) {
    if (history_file_.empty()) return;
    std::ifstream input(history_file_, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) history_.push_back(line);
    }
    input.close();
    if (history_.size() > history_limit * 2) {
        history_.erase(history_.begin(), history_.end() - history_limit);
        std::ofstream output(history_file_, std::ios::binary | std::ios::trunc);
        for (const auto& entry : history_) output << entry << '\n';
    }
}

void LineEditor::remember(const std::string& line) {
    if (line.find_first_not_of(" \t") == std::string::npos || (!history_.empty() && history_.back() == line)) return;
    history_.push_back(line);
    if (history_file_.empty()) return;
    std::error_code ignored;
    std::filesystem::create_directories(history_file_.parent_path(), ignored);
    std::ofstream(history_file_, std::ios::binary | std::ios::app) << line << '\n';
}

int LineEditor::columns() const {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return std::max(1, info.srWindow.Right - info.srWindow.Left + 1);
#endif
    return 80;
}

void LineEditor::render() {
    const int width = columns();
    auto prompt = searching_
        ? std::string(search_failed_ ? "failed search '" : "search '") + encode(query_) + "': "
        : prompt_;
    auto shown = visible(prompt);
    Position origin;
    origin.advance(shown, width);
    Position at_cursor = origin, at_end = origin;
    at_cursor.advance(std::u32string_view(text_).substr(0, cursor_), width);
    at_end.advance(text_, width);

    std::string frame = "\r";
    if (rows_above_ > 0) frame += "\x1b[" + std::to_string(rows_above_) + "A";
    frame += "\x1b[J" + prompt + encode(text_);
    // A line that fills its last row leaves the cursor pending there; make the wrap real.
    if (at_end.column == 0 && at_end.row > 0) frame += "\r\n";
    if (at_end.row > at_cursor.row) frame += "\x1b[" + std::to_string(at_end.row - at_cursor.row) + "A";
    frame += "\r";
    if (at_cursor.column > 0) frame += "\x1b[" + std::to_string(at_cursor.column) + "C";
    rows_above_ = at_cursor.row;
    host_.write(frame);
}

void LineEditor::recall(std::size_t index) {
    if (history_index_ == history_.size()) draft_ = text_;
    history_index_ = index;
    text_ = index == history_.size() ? draft_ : decode(history_[index]);
    cursor_ = text_.size();
}

void LineEditor::kill(std::size_t first, std::size_t last) {
    if (first >= last) return;
    killed_ = text_.substr(first, last - first);
    text_.erase(first, last - first);
    cursor_ = first;
}

std::size_t LineEditor::word_left() const {
    auto at = cursor_;
    while (at > 0 && !word_character(text_[at - 1])) --at;
    while (at > 0 && word_character(text_[at - 1])) --at;
    return at;
}

std::size_t LineEditor::word_right() const {
    auto at = cursor_;
    while (at < text_.size() && !word_character(text_[at])) ++at;
    while (at < text_.size() && word_character(text_[at])) ++at;
    return at;
}

void LineEditor::complete() {
    auto before = encode(std::u32string_view(text_).substr(0, cursor_));
    auto found = completer_(before);
    if (found.insertions.empty()) return;
    auto replacement = found.insertions.size() == 1 ? found.insertions.front() : shared_prefix(found.insertions);
    if (replacement.size() > before.size() - found.start) {
        auto start = decode(std::string_view(before).substr(0, found.start)).size();
        text_.replace(start, cursor_ - start, decode(replacement));
        cursor_ = start + decode(replacement).size();
        return;
    }
    if (found.insertions.size() == 1) return;

    // Nothing more can be typed for the user: show the choices under the line.
    auto at_end = cursor_;
    cursor_ = text_.size();
    render();
    cursor_ = at_end;
    std::string listing = "\r\n";
    if (found.labels.size() > 200) listing += std::to_string(found.labels.size()) + " matches; type more first.\r\n";
    else {
        std::size_t longest = 0;
        for (const auto& label : found.labels) longest = std::max(longest, decode(label).size());
        std::size_t cell = longest + 2, across = std::max<std::size_t>(1, static_cast<std::size_t>(columns()) / cell), column = 0;
        for (const auto& label : found.labels) {
            listing += label;
            if (++column == across) { listing += "\r\n"; column = 0; }
            else listing.append(cell - decode(label).size(), ' ');
        }
        if (column) listing += "\r\n";
    }
    host_.write(listing);
    rows_above_ = 0;
}

void LineEditor::search_step(std::size_t from) {
    for (std::size_t i = from; i > 0; --i) {
        auto candidate = decode(history_[i - 1]);
        auto at = candidate.find(query_);
        if (at == std::u32string::npos) continue;
        search_index_ = i - 1;
        search_failed_ = false;
        text_ = std::move(candidate);
        cursor_ = at;
        return;
    }
    search_failed_ = true;
}

// Reverse search owns the keys it understands; any other key ends the search and
// is then handled as ordinary editing, on the line that was found.
bool LineEditor::handle_search(const Key& key, bool& accepted) {
    const char32_t c = key.character;
    if (c == 18) { if (!query_.empty()) search_step(search_index_); return true; }
    if (c == 8) {
        if (!query_.empty()) query_.pop_back();
        if (!query_.empty()) search_step(history_.size());
        return true;
    }
    if (c == 27 || c == 7) {
        searching_ = false;
        text_ = draft_;
        cursor_ = text_.size();
        return true;
    }
    if (c >= 32 && c != 127 && !key.control && !key.alt) {
        query_.push_back(c);
        search_step(search_failed_ || query_.size() == 1 ? history_.size() : search_index_ + 1);
        return true;
    }
    searching_ = false;
    history_index_ = history_.size();
    accepted = c == U'\r' || c == U'\n';
    return accepted;
}

bool LineEditor::next_key(Key& key, bool& interrupted) {
#ifdef _WIN32
    HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
    wchar_t high = 0;
    for (;;) {
        if (host_.interrupted()) { interrupted = true; return true; }
        DWORD ready = WaitForSingleObject(console, 30);
        if (ready == WAIT_TIMEOUT) continue;
        if (ready != WAIT_OBJECT_0) return false;
        INPUT_RECORD event{};
        DWORD count = 0;
        if (!ReadConsoleInputW(console, &event, 1, &count)) return false;
        if (event.EventType == WINDOW_BUFFER_SIZE_EVENT) { render(); continue; }
        if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown) continue;
        const auto& record = event.Event.KeyEvent;
        wchar_t c = record.uChar.UnicodeChar;
        if (c >= 0xd800 && c <= 0xdbff) { high = c; continue; }
        key.character = high && c >= 0xdc00 && c <= 0xdfff
            ? 0x10000 + ((static_cast<char32_t>(high) - 0xd800) << 10) + (static_cast<char32_t>(c) - 0xdc00)
            : static_cast<char32_t>(c);
        high = 0;
        key.virtual_key = record.wVirtualKeyCode;
        key.control = (record.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        key.alt = (record.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        DWORD queued = 0;
        key.pending = GetNumberOfConsoleInputEvents(console, &queued) && queued > 1;
        // Modifiers arrive as keys of their own, and they aren't input.
        const auto vk = key.virtual_key;
        if (!key.character && (!vk || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_CAPITAL ||
                               vk == VK_NUMLOCK || vk == VK_SCROLL || vk == VK_LWIN || vk == VK_RWIN)) continue;
        return true;
    }
#else
    (void)key; (void)interrupted;
    return false;
#endif
}

LineEditor::Outcome LineEditor::read(const std::string& prompt, std::string& line) {
#ifdef _WIN32
    HANDLE console = GetStdHandle(STD_INPUT_HANDLE);
    DWORD saved = 0;
    GetConsoleMode(console, &saved);
    SetConsoleMode(console, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS);
    struct ModeGuard { HANDLE console; DWORD mode; ~ModeGuard() { SetConsoleMode(console, mode); } } guard{console, saved};
#endif
    prompt_ = prompt;
    text_.clear();
    draft_.clear();
    cursor_ = 0;
    rows_above_ = 0;
    searching_ = false;
    history_index_ = history_.size();
    render();
    auto finish = [&](std::string_view ending) {
        searching_ = false;
        cursor_ = text_.size();
        render();
        host_.write(ending);
    };
    for (;;) {
        Key key;
        bool interrupted = false;
        if (!next_key(key, interrupted)) { finish("\n"); return Outcome::End; }
        const char32_t c = key.character;
        if (interrupted || c == 3) { finish("^C\n"); return Outcome::Cancelled; }
        if (searching_) {
            bool accepted = false;
            if (handle_search(key, accepted) && !accepted) { render(); continue; }
            if (accepted) { finish("\n"); line = encode(text_); return Outcome::Line; }
        }
#ifdef _WIN32
        const unsigned vk = key.virtual_key;
        if (c == U'\r' || c == U'\n') { finish("\n"); line = encode(text_); return Outcome::Line; }
        if ((c == 4 || c == 26) && text_.empty()) { finish("\n"); return Outcome::End; }
        if (vk == VK_TAB || c == U'\t') {
            // Tabs arriving inside a burst of keys are pasted text, not requests.
            if (key.pending) { text_.insert(cursor_++, 1, U' '); }
            else complete();
        }
        else if (vk == VK_LEFT) cursor_ = key.control ? word_left() : cursor_ - (cursor_ > 0);
        else if (vk == VK_RIGHT) cursor_ = key.control ? word_right() : cursor_ + (cursor_ < text_.size());
        else if (vk == VK_HOME || c == 1) cursor_ = 0;
        else if (vk == VK_END || c == 5) cursor_ = text_.size();
        else if (vk == VK_UP || c == 16) { if (history_index_ > 0) recall(history_index_ - 1); }
        else if (vk == VK_DOWN || c == 14) { if (history_index_ < history_.size()) recall(history_index_ + 1); }
        else if (vk == VK_DELETE || c == 4) { if (cursor_ < text_.size()) text_.erase(cursor_, 1); }
        else if (c == 2) cursor_ -= cursor_ > 0;
        else if (c == 6) cursor_ += cursor_ < text_.size();
        else if (c == 11) kill(cursor_, text_.size());
        else if (c == 21) kill(0, cursor_);
        else if (c == 23 || c == 127 || (vk == VK_BACK && (key.control || key.alt))) kill(word_left(), cursor_);
        else if (vk == VK_BACK || c == 8) { if (cursor_ > 0) text_.erase(--cursor_, 1); }
        else if (c == 25) { text_.insert(cursor_, killed_); cursor_ += killed_.size(); }
        else if (c == 12) { host_.write("\x1b[2J\x1b[3J\x1b[H"); rows_above_ = 0; }
        else if (c == 27) { text_.clear(); cursor_ = 0; history_index_ = history_.size(); }
        else if (c == 18) {
            searching_ = true; search_failed_ = false; query_.clear();
            search_index_ = history_.size(); draft_ = text_;
        }
        else if (key.alt && !key.control && (c == U'b' || c == U'B')) cursor_ = word_left();
        else if (key.alt && !key.control && (c == U'f' || c == U'F')) cursor_ = word_right();
        else if (key.alt && !key.control && (c == U'd' || c == U'D')) kill(cursor_, word_right());
        else if (c >= 32) text_.insert(cursor_++, 1, c);
        else continue;
        render();
#else
        (void)c;
        return Outcome::End;
#endif
    }
}

} // namespace hirari
