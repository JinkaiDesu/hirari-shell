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
    };

    struct Stage {
        ForeignProgram* program = nullptr;

        std::vector<std::string>
            arguments;
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

    bool build_pipeline(
        seed::Runtime& runtime,
        seed::Ptr form,
        std::vector<Stage>& stages
    );

    seed::Ptr run(
        seed::Runtime& runtime,
        ForeignProgram& program,
        seed::Node* relation
    );

    seed::Ptr run_pipeline(
        seed::Runtime& runtime,
        std::vector<Stage> stages
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

    std::unordered_map<
        seed::Symbol*,
        std::unique_ptr<ForeignProgram>
    > programs_;

    std::unique_ptr<ConPtySession>
        conpty_;

    seed::Ptr foreground_root_ =
        nullptr;

    std::atomic_int last_status_{0};
};

} // namespace hirari