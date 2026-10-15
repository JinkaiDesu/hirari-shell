#pragma once

#include "hirari/host.hpp"
#include "hirari/process.hpp"
#include "seed/seed.hpp"
#include "hirari/world.hpp"
#include "hirari/vocabulary.hpp"
#include "hirari/values.hpp"
#include "hirari/jobs.hpp"
#include "hirari/control.hpp"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_set>

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

    bool execute_file(const std::filesystem::path& path, const std::vector<std::string>& arguments = {});
    // An explicit `exit STATUS` wins over the status of the last evaluation.
    int status() const noexcept { return processes_.exit_code().value_or(execution_status_); }
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
    static seed::Program resolve(seed::Runtime&, seed::Symbol*, seed::Ptr);
    static seed::Ptr script_command(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr script_builtin(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr script_argument(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr exec(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr help(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr inspect(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr vocabulary(seed::Runtime&, seed::Node*, seed::Ptr);
    void wait_foreground();
    seed::Ptr evaluate(std::string_view source, bool display = true);
    seed::Ptr evaluate_script(const std::filesystem::path& path, std::vector<seed::Ptr> arguments = {}, bool local = false);
    std::filesystem::path find_script(const std::string& name, bool explicit_file = false) const;
    struct ScriptProgram { Shell* shell; std::filesystem::path path; };
    static bool prefer_script(std::string_view);
    struct ScriptFrame { std::filesystem::path path; std::vector<seed::Ptr> arguments; };
    std::unordered_map<seed::Symbol*, std::unique_ptr<ScriptProgram>> scripts_;
    std::vector<ScriptFrame> script_frames_;
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

    Values values_;
    Jobs jobs_;
    Control control_;
    WorldHost worlds_;
    std::unique_ptr<Vocabulary> vocabulary_;
    bool interactive_session_ = false;
    std::unordered_set<seed::Ptr> descriptions_;
    int execution_status_ = 0;
    std::string pending_;
};

} // namespace hirari
