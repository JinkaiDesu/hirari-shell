#pragma once

#include "hirari/conpty.hpp"
#include "hirari/host.hpp"
#include "seed/seed.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <functional>
#include <optional>

namespace hirari {

struct ProcessResult {
    std::string out;
    std::string err;

    int status = 0;
};

class ProcessHost {
public:
    ProcessHost(
        seed::Runtime& runtime,
        Host& host
    );

    ~ProcessHost();
    void record_failure() noexcept { last_status_.store(1); }

    static seed::Program resolve(
        seed::Runtime& runtime,
        seed::Symbol* symbol,
        seed::Ptr context
    );

    static bool unpack(
        seed::Runtime& runtime,
        seed::Ptr form,
        ProcessResult& result
    );
    static std::string executable_path(const std::string& name);

    // Explicit executable paths are values, not parser tokens or shell command strings.
    seed::Ptr execute(seed::Runtime&, const std::string& path, const std::vector<seed::Ptr>& arguments, seed::Node* invocation = nullptr, std::optional<ProcessResult> input = {});
    seed::Ptr redirect(seed::Runtime&, seed::Ptr producer, const std::string& path, bool append);
    struct StreamProgram { std::wstring executable; std::vector<std::string> prefix; };
    void stream_resolver(std::function<StreamProgram(seed::Symbol*)> resolver) { stream_resolver_ = std::move(resolver); }
    void streaming(bool enabled) { streaming_ = enabled; }
    void opaque_values(std::function<bool(seed::Ptr)> predicate) { opaque_values_ = std::move(predicate); }

    // Hirari marks exactly one Seed form as the
    // foreground/root evaluation.
    void begin_foreground(
        seed::Ptr root
    );

    void end_foreground();

    bool interactive() const noexcept;

    bool terminal_input(
        std::string_view bytes
    );

    void resize(
        std::uint16_t columns,
        std::uint16_t rows
    );

    void interrupt();

    bool quitting() const noexcept { return quitting_; }
    void reap_foreground();

    int last_status() const noexcept {
        return last_status_.load();
    }

private:
    struct ForeignProgram {
        ProcessHost* owner = nullptr;
        seed::Symbol* symbol = nullptr;

        std::wstring executable;
        std::vector<std::string> prefix;
        bool script = false;
    };

    struct Stage {
        ForeignProgram* program = nullptr;

        std::vector<std::string>
            arguments;
        std::optional<ProcessResult> input;
    };

    static seed::Ptr truth(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr directory(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr quit(seed::Runtime&, seed::Node*, seed::Ptr);
    bool quitting_ = false;
    static seed::Ptr invoke(
        seed::Runtime& runtime,
        seed::Node* relation,
        seed::Ptr context
    );

    static seed::Ptr pipe(
        seed::Runtime& runtime,
        seed::Node* relation,
        seed::Ptr context
    );

    seed::Program resolve_symbol(
        seed::Runtime& runtime,
        seed::Symbol* symbol
    );

    ForeignProgram* foreign_program(
        seed::Runtime& runtime,
        seed::Ptr form
    );

    bool is_foreign_program(
        seed::Runtime& runtime,
        seed::Ptr form
    );
    bool is_explicit_program(seed::Runtime&, seed::Ptr);

    bool build_pipeline(
        seed::Runtime& runtime,
        seed::Ptr form,
        std::vector<Stage>& stages,
        bool has_upstream = false
    );

    seed::Ptr run(
        seed::Runtime& runtime,
        ForeignProgram& program,
        seed::Node* relation
    );

    seed::Ptr run_pipeline(
        seed::Runtime& runtime,
        std::vector<Stage> stages,
        bool passthrough = false,
        const std::wstring& output_file = {}, bool append = false
    );

    std::string argument(
        seed::Runtime& runtime,
        seed::Ptr form
    );

    seed::Ptr make_result(
        seed::Runtime& runtime,
        std::string out,
        std::string err,
        int status
    );

    Host& host_;
    seed::Runtime& runtime_;

    std::unordered_map<
        seed::Symbol*,
        std::unique_ptr<ForeignProgram>
    > programs_;
    std::unordered_map<seed::Symbol*, std::unique_ptr<ForeignProgram>> stream_programs_;
    std::unordered_map<std::wstring, std::unique_ptr<ForeignProgram>> explicit_programs_;
    std::function<StreamProgram(seed::Symbol*)> stream_resolver_;
    bool streaming_ = false;
    std::function<bool(seed::Ptr)> opaque_values_;

    std::unique_ptr<ConPtySession>
        conpty_;

    seed::Ptr foreground_root_ =
        nullptr;
    std::vector<seed::Ptr> foreground_stack_;
    unsigned foreground_depth_ = 0;
    std::vector<unsigned> foreground_depths_;

    std::atomic_int last_status_{0};
};

} // namespace hirari
