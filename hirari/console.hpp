#pragma once

// The terminal hirari itself runs on: its modes, its keys and its signals.
#include "hirari/host.hpp"

#include <cstdint>
#include <functional>
#include <string_view>

namespace hirari::console {

// Prepares this process's terminal and interrupt handling. Called once, at startup.
void start();
// True once for each interrupt (Ctrl+C from outside the prompt) since the last call.
bool interrupted();
// True when stdin and stdout are both a terminal.
bool is_terminal();
// Stdout has gone away, so nobody can see anything else this process would say.
void output_lost();

struct Size { std::uint16_t columns, rows; };
// The terminal's size, or a usual one when there is no terminal.
Size size();

struct Key {
    enum class Special { None, Tab, Left, Right, Home, End, Up, Down, Delete, Backspace };
    char32_t character = 0;
    Special special = Special::None;
    bool control = false, alt = false;
    // More input is already waiting behind this key: pasted text, not typing.
    bool pending = false;
};

// Single keys, without echo, for this object's life.
class KeyboardMode {
public:
    KeyboardMode();
    ~KeyboardMode();
    KeyboardMode(const KeyboardMode&) = delete;
    KeyboardMode& operator=(const KeyboardMode&) = delete;
private:
    unsigned long saved_ = 0;
    bool active_ = false;
};

// Waits for a key. False at the end of input. `interrupted` is set instead of `key` on an interrupt.
bool next_key(Key& key, bool& interrupted, Host& host, const std::function<void()>& resized);
// The control character that ends the shell on an empty line.
bool ends_input(char32_t character);

struct Foreground {
    std::function<bool()> running;
    std::function<void(std::string_view)> input;
    std::function<void(std::uint16_t, std::uint16_t)> resize;
    std::function<void()> interrupt;
};
// Hands this terminal's keys and size to a foreground program until it ends.
void run_foreground(Host& host, const Foreground& program);

} // namespace hirari::console
