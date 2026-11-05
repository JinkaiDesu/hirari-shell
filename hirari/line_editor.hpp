#pragma once

#include "hirari/host.hpp"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace hirari {

// What Tab may do to the text before the cursor: replace the bytes from `start`
// with one of `insertions`. `labels` is what a listing shows for each.
struct Completion {
    std::size_t start = 0;
    std::vector<std::string> insertions, labels;
};

// The prompt's line editor: cursor movement, kill keys, durable history,
// reverse search and completion, over the console's key events.
class LineEditor {
public:
    enum class Outcome { Line, Cancelled, End };
    using Completer = std::function<Completion(std::string_view before_cursor)>;

    LineEditor(Host& host, std::filesystem::path history_file, Completer completer);

    Outcome read(const std::string& prompt, std::string& line);
    void remember(const std::string& line);

private:
    struct Key { char32_t character = 0; unsigned virtual_key = 0; bool control = false, alt = false, pending = false; };
    bool next_key(Key&, bool& interrupted);
    bool handle_search(const Key&, bool& accepted);
    void render();
    void complete();
    void recall(std::size_t index);
    void search_step(std::size_t from);
    void kill(std::size_t first, std::size_t last);
    std::size_t word_left() const;
    std::size_t word_right() const;
    int columns() const;

    Host& host_;
    std::filesystem::path history_file_;
    Completer completer_;
    std::vector<std::string> history_;
    std::size_t history_index_ = 0;
    std::u32string draft_, text_, killed_, query_;
    std::size_t cursor_ = 0;
    std::string prompt_;
    int rows_above_ = 0;
    bool searching_ = false, search_failed_ = false;
    std::size_t search_index_ = 0;
};

} // namespace hirari
