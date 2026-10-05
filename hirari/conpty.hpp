#pragma once

#include "hirari/host.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hirari {

class ConPtySession {
public:
    using ExitHandler =
        std::function<void(int)>;

    explicit ConPtySession(
        Host& host
    );

    ~ConPtySession();

    ConPtySession(
        const ConPtySession&
    ) = delete;

    ConPtySession& operator=(
        const ConPtySession&
    ) = delete;

    void start(
        const std::wstring& executable,
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