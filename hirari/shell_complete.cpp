// What Tab offers at the prompt: paths inside strings, names and quoted paths outside.
#include "hirari/shell.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <system_error>

namespace hirari {
namespace {
namespace fs = std::filesystem;

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool starts_with_folded(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (lower(text[i]) != lower(prefix[i])) return false;
    return true;
}

// Sorted and unique by what would be inserted.
using Offers = std::map<std::string, std::string>;

// Paths matching `typed`, inserted as an opened Seed string. Backslashes become
// forward slashes, because \ is the escape character inside strings.
void offer_paths(std::string_view typed, Offers& offers) {
    std::string directory(typed);
    std::replace(directory.begin(), directory.end(), '\\', '/');
    auto slash = directory.rfind('/');
    std::string base = slash == std::string::npos ? directory : directory.substr(slash + 1);
    directory.resize(slash == std::string::npos ? 0 : slash + 1);
    std::error_code error;
    for (fs::directory_iterator walk(directory.empty() ? fs::path(".") : text_path(directory), fs::directory_options::skip_permission_denied, error), end;
         !error && walk != end; walk.increment(error)) {
        auto name = path_text(walk->path().filename());
        if (!starts_with_folded(name, base)) continue;
        std::error_code kind;
        if (walk->is_directory(kind)) name += '/';
        offers.emplace("\"" + directory + name, name);
    }
}

// A single file is finished: its string closes and the next word can begin.
Completion finished(std::size_t start, const Offers& offers) {
    Completion completion;
    completion.start = start;
    for (const auto& [insertion, label] : offers) {
        completion.insertions.push_back(insertion);
        completion.labels.push_back(label);
    }
    if (completion.insertions.size() == 1) {
        auto& only = completion.insertions.front();
        if (only.front() != '"') only += ' ';
        else if (only.back() != '/') only += "\" ";
    }
    return completion;
}
}

Completion Shell::complete(std::string_view before) {
    bool inside = false;
    std::size_t opened = 0;
    std::string typed;
    for (std::size_t i = 0; i < before.size(); ++i) {
        const char c = before[i];
        if (!inside) {
            if (c == '"') { inside = true; opened = i; typed.clear(); }
            continue;
        }
        if (c == '\\' && i + 1 < before.size() && before[i + 1] == '\\') { typed += '\\'; ++i; continue; }
        if (c == '\\' && i + 1 < before.size() && before[i + 1] == '"') { typed += '"'; ++i; continue; }
        if (c == '"') { inside = false; continue; }
        typed += c;
    }
    Offers offers;
    if (inside) {
        offer_paths(typed, offers);
        return finished(opened, offers);
    }

    auto start = before.size();
    while (start > 0 && !std::isspace(static_cast<unsigned char>(before[start - 1])) &&
           std::string_view("([{|").find(before[start - 1]) == std::string_view::npos) --start;
    auto word = before.substr(start);
    std::string_view sigil;
    if (word.starts_with("--")) sigil = word.substr(0, 2);
    else if (!word.empty() && (word.front() == '@' || word.front() == '*')) sigil = word.substr(0, 1);
    auto prefix = word.substr(sigil.size());

    auto previous = start;
    while (previous > 0 && std::isspace(static_cast<unsigned char>(before[previous - 1]))) --previous;
    const bool command = previous == 0 || std::string_view("({|").find(before[previous - 1]) != std::string_view::npos;

    if (prefix.find_first_of("/\\") != std::string_view::npos) {
        // The sigil stays where it was typed, ahead of the opened string.
        offer_paths(prefix, offers);
        return finished(start + sigil.size(), offers);
    }
    if (command && word.empty()) return {};
    if (!command && sigil.empty()) offer_paths(word, offers);
    // After a command, an empty word asks for files, not for every name there is.
    if (word.empty()) return finished(start, offers);
    for (const auto& name : known_names(command || !sigil.empty()))
        if (starts_with_folded(name, prefix)) offers.emplace(std::string(sigil) + name, name);
    return finished(start, offers);
}

} // namespace hirari
