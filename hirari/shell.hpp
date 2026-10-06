#pragma once

#include "hirari/host.hpp"
#include "hirari/process.hpp"
#include "seed/seed.hpp"
#include "hirari/unity.hpp"
#include "hirari/world.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace hirari {

class Shell {
public:
    explicit Shell(
        Host& host
    );

    // Committed Hirari/Seed line.
    bool input(
        std::string_view line
    );

    // Raw terminal bytes.
    //
    // The console uses this while a ConPTY child owns
    // the foreground terminal.
    bool terminal_input(
        std::string_view bytes
    );

    void resize(
        std::uint16_t columns,
        std::uint16_t rows
    );

    void interrupt();

    int run();

    bool interactive() const noexcept {
        return processes_.interactive();
    }

    seed::Ptr execute(
        std::string_view source
    );

    bool execute_file(const std::filesystem::path& path);
    bool execute_startup();
    int status() const noexcept { return execution_status_; }
    UnityProgram& unity() noexcept { return unity_; }
    const WorldHost& worlds() const noexcept { return worlds_; }

    void cancel_input();

    bool waiting() const noexcept {
        return !pending_.empty();
    }

    seed::Runtime& seed() noexcept {
        return seed_;
    }

    ProcessHost& processes() noexcept {
        return processes_;
    }

private:
    static seed::Ptr world(seed::Runtime&, seed::Node*, seed::Ptr);
    seed::Ptr evaluate(std::string_view source);
    void evaluate_script(const std::filesystem::path& path);
    void report(const std::exception& error);

    static bool source_complete(
        std::string_view source
    );

    void present(
        seed::Ptr result
    );

    Host& host_;

    seed::Runtime seed_;

    ProcessHost processes_;

    UnityProgram unity_;
    WorldHost worlds_;
    int execution_status_ = 0;
    std::string pending_;
};

} // namespace hirari
