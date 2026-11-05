#pragma once

#include "hirari/host.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hirari {

// A native program on a pseudoterminal of its own: ConPTY on Windows, a pty on Linux.
class TerminalSession {
public:
    using ExitHandler =
        std::function<void(int)>;

    explicit TerminalSession(
        Host& host
    );

    ~TerminalSession();

    TerminalSession(
        const TerminalSession&
    ) = delete;

    TerminalSession& operator=(
        const TerminalSession&
    ) = delete;

    void start(
        const std::string& executable,
        const std::vector<std::string>& arguments,
        std::uint16_t columns = 120,
        std::uint16_t rows = 30,
        ExitHandler exited = {}
    );

    bool running() const noexcept;

    void write(
        std::string_view bytes
    );

    void resize(
        std::uint16_t columns,
        std::uint16_t rows
    );

    void interrupt();

    void close();

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace hirari