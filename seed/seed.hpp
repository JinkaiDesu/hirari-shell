#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace seed {

using Ptr = void*;

// -----------------------------------------------------------------------------
// Universal Seed relation
// -----------------------------------------------------------------------------

struct Node {
    Ptr head = nullptr;
    Ptr args = nullptr;
    Ptr ref  = nullptr;
};

// -----------------------------------------------------------------------------
// Host payloads
//
// C++ bookkeeping only.
// These are NOT Seed AST types.
// -----------------------------------------------------------------------------

struct Symbol {
    std::string text;
};

struct Name {
    Ptr target = nullptr;
};

struct Literal {
    std::string bytes;
};

struct Number {
    std::string spelling;
    std::int64_t integer = 0;
    double real = 0;
};

// Definition-time descriptions, separate from the executable pointer product.
// Snapshots share these immutable facts; help never has to extract a body.
struct BindingMetadata {
    std::string kind;
    std::string docstring;
    std::vector<std::string> parameters;
    std::string alias;
    std::string provider;
    std::string origin;
    std::string source_file;
};

struct Cell {
    Symbol* symbol = nullptr;
    Ptr head = nullptr;

    std::uint64_t revision = 0;
    std::shared_ptr<const BindingMetadata> metadata;
};

enum class HostKind : std::uint8_t {
    Node,
    Unit,
    Symbol,
    Name,
    Literal,
    Cell,
    Wildcard,
    Integer,
    Real
};

class Runtime;

#ifdef SEED_DEFERRED_EXPERIMENT
// Available only in the separate experimental build, never the Program ABI.
struct DeferredStats {
    std::uint64_t attempts = 0;
    std::uint64_t chains = 0;
    std::uint64_t deferred_betas = 0;
    std::uint64_t materializations = 0;
    std::uint64_t inspection_barriers = 0;
};
#endif

#ifdef SEED_PROFILE
// Work counts are compiled only into the separate profiling benchmark.
struct WorkProfile {
    std::uint64_t nodes = 0;
    std::uint64_t relations = 0;
    std::uint64_t term_walks = 0;
    std::uint64_t terms_copied = 0;
    std::uint64_t abstraction_checks = 0;
    std::uint64_t body_extractions = 0;
    std::uint64_t substitution_visits = 0;
    std::uint64_t reduction_steps = 0;
    std::uint64_t compositions = 0;
    std::uint64_t applications = 0;
};
#endif

// -----------------------------------------------------------------------------
// Program ABI
//
// A Seed Program is:
//
//     relation + implementation + opaque host context
//
// The context is what will later let:
//
//     regex
//     python
//     blender
//     DLL functions
//     Windows processes
//
// all participate through the exact same Program interface.
// -----------------------------------------------------------------------------

using ProgramFn =
    Ptr (*)(
        Runtime& runtime,
        Node* relation,
        Ptr context
    );

enum class ProgramDispatch : std::uint8_t {
    Ordinary,
    // Explicit prefix/suffix identity request, before callable expansion.
    // No ambient callback on unrelated products.
    IdentityQuery
};

struct Program {
    ProgramFn invoke = nullptr;
    Ptr context = nullptr;
    ProgramDispatch dispatch = ProgramDispatch::Ordinary;

    explicit operator bool() const noexcept {
        return invoke != nullptr;
    }
};

// Hirari can supply one lazy fallback resolver.
//
// Example later:
//
//     Symbol("python")
//         ↓
//     Hirari resolver
//         ↓
//     Program{process_invoke, PythonProcess*}
//
// Seed itself knows nothing about PATH or Windows.
using ResolverFn =
    Program (*)(
        Runtime& runtime,
        Symbol* symbol,
        Ptr context
    );

// -----------------------------------------------------------------------------
// Arena
// -----------------------------------------------------------------------------

class Arena {
public:
    Arena();
    std::size_t node_count() const noexcept { return nodes_.size(); }

#ifdef SEED_PROFILE
    WorkProfile& profile() const noexcept { return profile_; }
#endif

    Node* make_node(
        Ptr head = nullptr
    );

    Node* make_relation(
        const std::vector<Ptr>& forms
    );

    Node* unit() noexcept {
        return unit_;
    }

    Symbol* intern(
        std::string_view text
    );

    Name* make_name(
        Ptr target
    );

    Literal* make_literal(
        std::string_view bytes
    );

    Cell* make_cell(
        Symbol* symbol
    );

    // Each anonymous position is fresh and has no symbol or binding cell.
    Ptr make_wildcard();
    Ptr make_number(std::string_view spelling);

    HostKind kind(
        Ptr p
    ) const;

    bool is(
        Ptr p,
        HostKind expected
    ) const;

private:
#ifdef SEED_PROFILE
    mutable WorkProfile profile_;
#endif
    std::vector<
        std::unique_ptr<Node>
    > nodes_;

    std::vector<
        std::unique_ptr<Symbol>
    > symbols_;

    std::vector<
        std::unique_ptr<Name>
    > names_;

    std::vector<
        std::unique_ptr<Literal>
    > literals_;

    std::vector<std::unique_ptr<Number>> numbers_;

    std::vector<
        std::unique_ptr<Cell>
    > cells_;

    std::unordered_map<
        std::string,
        Symbol*
    > interned_;

    std::unordered_map<
        Ptr,
        HostKind
    > meta_;

    Node* unit_ = nullptr;
};

// -----------------------------------------------------------------------------
// Registry
// -----------------------------------------------------------------------------

class Registry {
public:
    void define(
        Symbol* symbol,
        Program program
    );

    Program find(
        Symbol* symbol
    ) const;

private:
    std::unordered_map<
        Symbol*,
        Program
    > programs_;
};

// -----------------------------------------------------------------------------
// Reader
// -----------------------------------------------------------------------------

class Reader {
public:
    explicit Reader(
        Runtime& runtime
    );

    Ptr read(
        std::string_view source
    );

    // Shared by file execution and the interactive continuation prompt.
    static bool source_complete(std::string_view source);
    static bool source_has_form(std::string_view source);
    // File-level lookahead: a leading pipe continues the preceding expression.
    // Comments and blank lines are layout; || is a separate identity.
    static bool source_leading_pipe(std::string_view source);
    std::string documentation(std::string_view source);

private:
    Runtime& runtime_;

    std::string_view source_;
    std::size_t pos_ = 0;

    bool eof() const noexcept;

    char peek(
        std::size_t lookahead = 0
    ) const noexcept;

    char take();

    void skip_space();

    Ptr read_sequence(
        char until = '\0'
    );

    Ptr read_form();
    Ptr read_group();
    Ptr read_literal();
    std::string read_literal_bytes();
    Ptr read_symbol();

    std::string read_symbol_token();
};

// -----------------------------------------------------------------------------
// Printer
// -----------------------------------------------------------------------------

class Printer {
public:
    explicit Printer(
        Runtime& runtime
    )
        : runtime_(runtime) {}

    std::string native(
        Ptr form
    ) const;

    std::string structural(
        Ptr form
    ) const;

private:
    Runtime& runtime_;

    std::string native_form(
        Ptr form
    ) const;

    std::string native_relation(
        Node* relation
    ) const;

    std::string structural_form(
        Ptr form
    ) const;

    std::string structural_member(
        Ptr form
    ) const;

    std::string structural_relation(
        Node* relation
    ) const;

    static std::string quote(
        std::string_view text
    );
};

// -----------------------------------------------------------------------------
// Reducer
// -----------------------------------------------------------------------------

class Reducer {
public:
    explicit Reducer(
        Runtime& runtime
    )
        : runtime_(runtime) {}

    Ptr reduce(
        Ptr form
    );

private:
    Runtime& runtime_;

    Ptr reduce_once(
        Ptr form
#ifdef SEED_DEFERRED_EXPERIMENT
        , std::size_t& extra_steps, std::size_t remaining_steps
#endif
    );

    Ptr reduce_relation(
        Node* relation
#ifdef SEED_DEFERRED_EXPERIMENT
        , std::size_t& extra_steps, std::size_t remaining_steps
#endif
    );

    Program program_for(
        Ptr form
    );

    static constexpr std::size_t
        max_steps_ = 4096;
};

// -----------------------------------------------------------------------------
// Runtime
// -----------------------------------------------------------------------------

class Runtime {
public:
#ifdef SEED_DEFERRED_EXPERIMENT
    explicit Runtime(bool deferred_substitution = true);
    bool deferred_substitution() const noexcept { return deferred_substitution_; }
    void deferred_substitution(bool enabled) noexcept { deferred_substitution_ = enabled; }
    DeferredStats& deferred_stats() noexcept { return deferred_stats_; }
#else
    Runtime();
#endif
    unsigned reduction_depth() const noexcept { return reduction_depth_; }

    Arena& arena() noexcept {
        return arena_;
    }

    const Arena& arena() const noexcept {
        return arena_;
    }

    Registry& registry() noexcept {
        return registry_;
    }

    const Registry& registry() const noexcept {
        return registry_;
    }

    Symbol* intern(
        std::string_view text
    ) {
        return arena_.intern(text);
    }

    // -------------------------------------------------------------------------
    // Programs
    // -------------------------------------------------------------------------

    void define(
        std::string_view symbol,
        ProgramFn invoke,
        Ptr context = nullptr
    );

    void define(
        std::string_view symbol,
        Program program
    );

    void set_resolver(
        ResolverFn resolver,
        Ptr context = nullptr
    );

    Program resolve_program(
        Symbol* symbol
    );
    Program identity_query(Ptr form) const;

    // -------------------------------------------------------------------------
    // Language
    // -------------------------------------------------------------------------

    Ptr read(
        std::string_view source
    );
    std::string source_documentation(std::string_view source) { return reader_.documentation(source); }

    Ptr reduce(
        Ptr form
    );

    Ptr run(
        std::string_view source
    );

    // Unit, empty/zero/false literals, and the identity :false are false.
    bool truth(Ptr value) const;

    std::string print(
        Ptr form
    ) const;

    std::string structural(
        Ptr form
    ) const;

    // -------------------------------------------------------------------------
    // Relations
    // -------------------------------------------------------------------------

    std::vector<Ptr> terms(
        Node* relation
    ) const;

    Ptr product(
        std::vector<Ptr> forms
    );

    // Pure orientation changes: no evaluation or host/process discovery.
    Ptr compose(Ptr consumer, Ptr producer);
    Ptr flow(Node* relation, std::vector<Ptr>& stages);
    // Expand a callable binding head structurally, without reducing its arguments.
    Ptr application(Ptr form);

    // Abstractions remain ordinary pointer products: binder, Unit, body.
    // The mirror body : binder has the same meaning; there is no Lambda payload.
    bool is_abstraction(Ptr form) const;
    bool abstraction(Ptr form, Ptr& binder, Ptr& body);
    // Replace free occurrences of the exact binder identity. Incoming symbols
    // may be captured; Names, literals and cells remain opaque values.
    Ptr substitute(Ptr body, Ptr binder, Ptr replacement);

    bool is_wildcard(Ptr p) const;
    bool is_number(Ptr p) const;
    Number* as_number(Ptr p) const;

    // -------------------------------------------------------------------------
    // Stable cells
    // -------------------------------------------------------------------------

    Cell* ensure_cell(
        Symbol* symbol
    );

    Cell* find_cell(
        Symbol* symbol
    ) const;

    void bind(
        Symbol* symbol,
        Ptr value
    );
    void binding_origin(std::string origin, std::string source_file = {}) {
        binding_origin_ = std::move(origin); binding_source_ = std::move(source_file);
    }
    const std::string& binding_origin() const noexcept { return binding_origin_; }
    const std::string& binding_source() const noexcept { return binding_source_; }

    Ptr binding(
        Symbol* symbol
    ) const;

    Ptr head(
        Cell* cell
    ) const;

    // Host call scopes preserve cell identity without copying or serializing values.
    std::vector<Cell> capture_bindings() const;
    void restore_bindings(const std::vector<Cell>& saved);

    // -------------------------------------------------------------------------
    // Host inspection
    // -------------------------------------------------------------------------

    bool is_relation(
        Ptr p
    ) const;

    bool is_unit(
        Ptr p
    ) const;

    bool is_symbol(
        Ptr p
    ) const;

    bool is_name(
        Ptr p
    ) const;

    bool is_literal(
        Ptr p
    ) const;

    bool is_cell(
        Ptr p
    ) const;

    Symbol* as_symbol(
        Ptr p
    ) const;

    Name* as_name(
        Ptr p
    ) const;

    Literal* as_literal(
        Ptr p
    ) const;

    Cell* as_cell(
        Ptr p
    ) const;

    Node* as_node(
        Ptr p
    ) const;

private:
#ifdef SEED_DEFERRED_EXPERIMENT
    bool deferred_substitution_ = true;
    DeferredStats deferred_stats_;
#endif
    Arena arena_;
    Registry registry_;
    Reader reader_;
    mutable Printer printer_;
    Reducer reducer_;

    std::unordered_map<
        Symbol*,
        Cell*
    > cells_;

    ResolverFn resolver_ = nullptr;
    unsigned reduction_depth_ = 0;
    Ptr resolver_context_ = nullptr;
    std::string binding_origin_ = "runtime";
    std::string binding_source_;
};

} // namespace seed
