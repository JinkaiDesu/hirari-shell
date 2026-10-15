#include "seed/seed.hpp"
#include "seed/native_stack.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace seed {

#ifdef SEED_PROFILE
#define SEED_COUNT(runtime, member) (++(runtime).arena().profile().member)
#else
#define SEED_COUNT(runtime, member) ((void)0)
#endif

namespace {

enum class TriviaStatus { Complete, OpenComment, OpenContinuation };

// Only a backslash touching a physical line ending is layout. A final
// backslash is pending input; ordinary backslashes elsewhere remain symbols.
std::size_t continuation_width(std::string_view source, std::size_t pos) {
    if (pos >= source.size() || source[pos] != '\\') return 0;
    if (pos + 1 == source.size()) return 1;
    if (source[pos + 1] == '\n') return 2;
    if (source[pos + 1] == '\r' && pos + 2 < source.size() && source[pos + 2] == '\n') return 3;
    return 0;
}

// The reader, continuation prompt and script form scanner share layout rules.
// `newline` reports a physical line break that is not a `\` continuation and
// not inside a bracket comment: the statement separator of files and blocks.
TriviaStatus skip_trivia(std::string_view source, std::size_t& pos, bool* newline = nullptr) {
    while (pos < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[pos]))) {
            if (newline && source[pos] == '\n') *newline = true;
            ++pos; continue;
        }
        if (auto width = continuation_width(source, pos)) {
            pos += width;
            while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t')) ++pos;
            if (pos == source.size()) return TriviaStatus::OpenContinuation;
            continue;
        }
        if (source[pos] != '#') break;
        if (pos + 1 == source.size() || source[pos + 1] != '[') {
            while (pos < source.size() && source[pos] != '\n') ++pos;
            continue;
        }
        pos += 2;
        std::size_t depth = 1;
        while (pos < source.size() && depth) {
            if (source[pos] == '#' && pos + 1 < source.size() && source[pos + 1] == '[') {
                ++depth; pos += 2;
            } else if (source[pos++] == ']') --depth;
        }
        if (depth) return TriviaStatus::OpenComment;
    }
    return TriviaStatus::Complete;
}

// Reserved heads the reader lowers to. Each contains a delimiter, so source
// can never spell them as ordinary symbols.
constexpr std::string_view block_head = "(;)";
constexpr std::string_view list_head = "[]";

constexpr std::pair<const char*, const char*> operator_names[] = {
    {"<", "lt"}, {"<=", "le"}, {">", "gt"}, {">=", "ge"}, {"==", "eq"}, {"!=", "ne"},
    {"|", "pipe"}, {"=", "bind"}, {"&&", "and"}, {"||", "or"}};

std::string name_spelling(Symbol* symbol) {
    for (auto pair : operator_names)
        if (symbol->text == pair.first) return "-" + std::string(pair.second);
    // Explicit grouping preserves a raw identity that happens to spell an
    // operator alias, including provider field names and ordinary bindings.
    for (auto pair : operator_names)
        if (symbol->text == pair.second) return "-(" + symbol->text + ")";
    if (!symbol->text.empty() && (std::isalpha(static_cast<unsigned char>(symbol->text[0])) ||
        static_cast<unsigned char>(symbol->text[0]) >= 128 || symbol->text[0] == '_'))
        return "-" + symbol->text;
    return "-(" + symbol->text + ")";
}

Symbol* named_operation(Runtime& runtime, std::string_view spelling) {
    for (auto pair : operator_names)
        if (spelling == pair.second) return runtime.intern(pair.first);
    return runtime.intern(spelling);
}

// =============================================================================
// Bootstrap programs
// =============================================================================

bool is_symbol_text(
    const Runtime& runtime,
    Ptr form,
    std::string_view text
) {
    return
        runtime.is_symbol(form) &&
        static_cast<Symbol*>(form)->text ==
            text;
}

// Operator dispatch order. The first operator present in a product owns the
// whole product, so earlier entries bind loosest: binding, one reduction step,
// short circuit, comparison, reverse application, then arithmetic.
constexpr const char* orientation_order[] = {
    ":=", "=", "=>", "&&", "||", "==", "!=", "<", "<=", ">", ">=", ">>", "|",
    "+", "-", "*", "/", ":"};

bool is_orientation_text(std::string_view text) {
    for (auto spelling : orientation_order) if (text == spelling) return true;
    return false;
}

bool is_comparison_text(std::string_view t) {
    return t == "<" || t == "<=" || t == ">" || t == ">=" || t == "==" || t == "!=";
}

bool is_arithmetic_text(std::string_view t) {
    return t == "+" || t == "-" || t == "*" || t == "/";
}

bool has_orientation(const Runtime& runtime, const std::vector<Ptr>& forms) {
    for (Ptr form : forms)
        if (runtime.is_symbol(form) && is_orientation_text(static_cast<Symbol*>(form)->text)) return true;
    return false;
}

Symbol* symbol_from_identity(
    const Runtime& runtime,
    Ptr form
) {
    if (runtime.is_symbol(form)) {
        return runtime.as_symbol(form);
    }

    if (runtime.is_name(form)) {
        Ptr target =
            runtime.as_name(form)->target;

        if (runtime.is_symbol(target)) {
            return runtime.as_symbol(
                target
            );
        }
    }

    if (runtime.is_cell(form)) {
        return runtime.as_cell(
            form
        )->symbol;
    }

    return nullptr;
}

Ptr binder_identity(const Runtime& runtime, Ptr form) {
    if (runtime.is_wildcard(form)) return form;
    return symbol_from_identity(runtime, form);
}

// Borrow a range only while examining it. Nodes remain the semantic structure;
// no persistent descriptor or evaluated-result cache can outlive host mutations.
struct RelationSlice {
    Node* begin = nullptr;
    Node* end = nullptr;
};

struct AbstractionView {
    Ptr binder = nullptr;
    RelationSlice body;
    bool canonical = false;
};

Node* next_term(Node* node) {
    return static_cast<Node*>(node->ref);
}

bool abstraction_view(const Runtime& runtime, RelationSlice slice, AbstractionView& view) {
    SEED_COUNT(runtime, abstraction_checks);
    auto first = slice.begin;
    if (first == slice.end) return false;
    auto second = next_term(first);
    if (second == slice.end) return false;
    auto third = next_term(second);
    if (third == slice.end) return false;
    if (runtime.is_unit(second->head)) {
        // A registered Program is never a binder: `if () a b` calls if with a
        // Unit condition, and the reserved block/list heads stay products.
        if (runtime.is_symbol(first->head) && runtime.registry().find(runtime.as_symbol(first->head)))
            return false;
        view = {binder_identity(runtime, first->head), {third, slice.end}, true};
        return view.binder != nullptr;
    }
    // Binding owns its RHS; otherwise the final colon is the outermost binder.
    Node* previous = nullptr;
    Node* last = nullptr;
    for (auto node = first; node != slice.end; node = next_term(node)) {
        if (runtime.is_symbol(node->head)) {
            const auto& text = static_cast<Symbol*>(node->head)->text;
            if (text == ":=" || text == "=") return false;
        }
        previous = last;
        last = node;
    }
    if (!is_symbol_text(runtime, previous->head, ":")) return false;
    view = {binder_identity(runtime, last->head), {first, previous}, false};
    return view.binder != nullptr;
}

Ptr slice_product(Runtime& runtime, RelationSlice slice) {
    if (slice.begin == slice.end) return runtime.arena().unit();
    if (next_term(slice.begin) == slice.end) return slice.begin->head;
    // Preserve the independently owned product that extraction used to return.
    SEED_COUNT(runtime, term_walks);
    std::vector<Ptr> forms;
    for (auto node = slice.begin; node != slice.end; node = next_term(node)) {
        SEED_COUNT(runtime, terms_copied);
        forms.push_back(node->head);
    }
    return runtime.product(std::move(forms));
}

struct Substitution {
    Ptr value = nullptr;
    bool changed = false;
};

Substitution substitute_product(Runtime&, RelationSlice, Ptr, Ptr);

Substitution substitute_body(Runtime& runtime, RelationSlice slice, Ptr binder, Ptr replacement) {
    if (next_term(slice.begin) == slice.end) {
        Ptr original = slice.begin->head;
        Ptr value = runtime.substitute(original, binder, replacement);
        return {value, value != original};
    }
    SEED_COUNT(runtime, substitution_visits);
    return substitute_product(runtime, slice, binder, replacement);
}

Substitution substitute_product(Runtime& runtime, RelationSlice slice, Ptr binder, Ptr replacement) {
    AbstractionView view;
    if (abstraction_view(runtime, slice, view)) {
        if (view.binder == binder) return {}; // Same-identity shadowing.
        auto result = substitute_body(runtime, view.body, binder, replacement);
        if (!result.changed) return {};
        // Incoming identities may deliberately be captured by this binder.
        return {runtime.product({view.binder, runtime.arena().unit(), result.value}), true};
    }
    SEED_COUNT(runtime, term_walks);
    std::vector<Ptr> forms;
    bool changed = false;
    for (auto node = slice.begin; node != slice.end; node = next_term(node)) {
        Ptr value = runtime.substitute(node->head, binder, replacement);
        changed |= value != node->head;
        SEED_COUNT(runtime, terms_copied);
        forms.push_back(value);
    }
    // An unchanged range never needs a reconstructed intermediate body.
    return changed ? Substitution{runtime.arena().make_relation(forms), true} : Substitution{};
}

#ifdef SEED_DEFERRED_EXPERIMENT
// An ordered environment, not a recursively resolved map. Later substitutions
// can capture earlier replacements; earlier substitutions never rewrite later
// arguments. Keep it local to an uninterrupted chain and materialize before
// returning any Ptr. This first experiment deliberately retains literal
// substitution for the final body, avoiding new public payloads or lazy Nodes.
struct PendingSubstitution { Ptr binder; Ptr replacement; };

bool stable_curry_argument(const Runtime& runtime, Ptr value) {
    if (!value || runtime.is_relation(value) || runtime.is_unit(value) ||
        runtime.is_literal(value)) return false;
    if (runtime.is_symbol(value) && is_orientation_text(runtime.as_symbol(value)->text)) return false;
    return true;
}

Ptr deferred_curry(Runtime& runtime, Ptr binder, Ptr body,
                   const std::vector<Ptr>& terms, std::size_t remaining_steps,
                   std::size_t& extra_steps) {
    auto& stats = runtime.deferred_stats();
    ++stats.attempts;
    // Explicit identity queries dispatch before beta reduction. Registered
    // commands are no longer ambient observers of every intermediate product.
    if (terms.size() < 3 || remaining_steps < 2 ||
        !stable_curry_argument(runtime, terms[1])) return nullptr;
    std::vector<PendingSubstitution> pending{{binder, terms[1]}};
    std::size_t applied = 1;
    Ptr current = body;
    while (applied + 1 < terms.size() && applied < remaining_steps) {
        if (!stable_curry_argument(runtime, terms[applied + 1]) ||
            !runtime.is_relation(current)) break;
        AbstractionView inner;
        if (!abstraction_view(runtime, {runtime.as_node(current), nullptr}, inner) ||
            !inner.canonical || next_term(inner.body.begin) != inner.body.end) break;
        Ptr next_body = inner.body.begin->head;
        // Do not elide body extraction, colon normalization or docstring
        // stripping when they can change the intermediate product.
        if (runtime.is_relation(next_body)) {
            auto first = runtime.as_node(next_body);
            if (first->ref && runtime.is_literal(first->head)) break;
        }
        // Same-identity shadowing blocks a prior substitution into this entire
        // original body. An inserted free identity is still captured by the
        // new frame below when final materialization applies frames in order.
        std::erase_if(pending, [&](const PendingSubstitution& frame) {
            return frame.binder == inner.binder;
        });
        pending.push_back({inner.binder, terms[applied + 1]});
        current = next_body;
        ++applied;
    }
    if (applied == 1) return nullptr;
    for (const auto& frame : pending)
        current = runtime.substitute(current, frame.binder, frame.replacement);
    for (std::size_t i = applied + 1; i < terms.size(); ++i)
        current = runtime.compose(current, terms[i]);
    ++stats.chains;
    stats.deferred_betas += applied;
    ++stats.materializations;
    extra_steps = applied - 1; // Charge exactly the omitted beta steps.
    return current;
}
#endif

Ptr demand_program_form(Runtime& runtime, Ptr form);

// -----------------------------------------------------------------------------
// demand
// -----------------------------------------------------------------------------
//
// demand camera
//
// means:
//
// demand/reduce camera.
//
// camera demand
//
// is structurally different and is therefore left untouched.
//

Ptr program_demand(
    Runtime& runtime,
    Node* relation,
    Ptr
) {
    auto terms =
        runtime.terms(relation);

    if (
        terms.empty() ||
        !is_symbol_text(runtime, terms.front(), "demand")
    ) {
        return relation;
    }

    std::vector<Ptr> tail(
        terms.begin() + 1,
        terms.end()
    );

    Ptr target =
        runtime.product(
            std::move(tail)
        );

    return demand_program_form(runtime,
        target
    );
}

// -----------------------------------------------------------------------------
// =
// -----------------------------------------------------------------------------
//
// camera := unity -Camera.main  <=>  unity -Camera.main = camera
//
// establishes:
//
// Cell(camera).head
//     =
// unity :Camera.main
//
// The expression is retained lazily; returning it preserves normal reduction
// of the statement result. This is not memoization or once-only capture.
//

Ptr program_bind(
    Runtime& runtime,
    Node* relation,
    Ptr
) {
    auto terms =
        runtime.terms(relation);

    std::size_t eq = terms.size();
    bool forward = false;
    for (std::size_t i = 0; i < terms.size(); ++i) {
        if (!is_symbol_text(runtime, terms[i], "=") &&
            !is_symbol_text(runtime, terms[i], ":=")) continue;
        if (eq != terms.size())
            throw std::runtime_error("group nested bindings explicitly");
        eq = i;
        forward = is_symbol_text(runtime, terms[i], ":=");
    }
    if (eq == terms.size()) return relation;
    if ((forward && (eq != 1 || eq + 1 >= terms.size())) ||
        (!forward && (eq == 0 || eq + 2 != terms.size())))
        throw std::runtime_error("binding requires name := expression or expression = name");
    Symbol* destination = symbol_from_identity(runtime, forward ? terms.front() : terms.back());
    if (!destination) throw std::runtime_error("binding destination must be an identity");
    std::vector<Ptr> rhs_terms(
        forward ? terms.begin() + 2 : terms.begin(),
        forward ? terms.end() : terms.begin() + static_cast<std::ptrdiff_t>(eq));

    Ptr rhs =
        runtime.product(
            std::move(rhs_terms)
        );

    runtime.bind(
        destination,
        rhs
    );

    return rhs;
}

Ptr program_flow(Runtime& runtime, Node* relation, Ptr) {
    std::vector<Ptr> stages;
    return runtime.flow(relation, stages);
}
Ptr program_apply(Runtime& runtime, Node* relation, Ptr) {
    auto terms=runtime.terms(relation);
    if(terms.empty() || !is_symbol_text(runtime,terms.front(),"apply"))return relation;
    if(terms.size()!=3)throw std::runtime_error("usage: apply -PROGRAM -(ARGUMENTS ...)");
    auto program=runtime.reduce(terms[1]);auto arguments=runtime.reduce(terms[2]);
    if(!runtime.is_name(program) || !runtime.is_name(arguments))throw std::runtime_error("apply requires reified program and argument products");
    auto head=runtime.as_name(program)->target,tail=runtime.as_name(arguments)->target;
    auto forms=runtime.is_relation(head) && !runtime.is_abstraction(head)
        ? runtime.terms(runtime.as_node(head)) : std::vector<Ptr>{head};
    if(!runtime.is_unit(tail)) {
        auto values=runtime.collection(arguments,true);
        forms.insert(forms.end(),values.begin(),values.end());
    }
    return runtime.arena().make_relation(forms);
}

// -----------------------------------------------------------------------------
// :
// -----------------------------------------------------------------------------
//
// Detached:
//
// b : a
//
// is:
//
// a() b
//
// Attached:
//
// :a
//
// never reaches this program.
// The Reader already creates Name(a).
//

Ptr program_colon(
    Runtime& runtime,
    Node* relation,
    Ptr
) {
    Ptr binder = nullptr, body = nullptr;
    if (!runtime.abstraction(relation, binder, body)) return relation;
    return runtime.product({binder, runtime.arena().unit(), body});
}

// -----------------------------------------------------------------------------
// @
// -----------------------------------------------------------------------------
//
// @ camera
//
// returns the stable cell for camera.
//
// The cell exists independently of whatever its current head becomes.
//
// @ :camera
//
// is equivalent at this level because :camera already denotes the
// semantic identity camera.
//

Ptr program_reference(
    Runtime& runtime,
    Node* relation,
    Ptr
) {
    auto terms =
        runtime.terms(relation);

    if (
        terms.size() != 2 ||
        !is_symbol_text(
            runtime,
            terms[0],
            "@"
        )
    ) {
        return relation;
    }

    Symbol* symbol =
        symbol_from_identity(
            runtime,
            terms[1]
        );

    if (!symbol) {
        if (runtime.is_wildcard(terms[1]))
            throw std::runtime_error("anonymous _ has no binding cell");
        return relation;
    }

    return runtime.ensure_cell(
        symbol
    );
}

// -----------------------------------------------------------------------------
// *
// -----------------------------------------------------------------------------
//
// * camera
//
// reads camera's current cell head.
//
// * :camera
//
// reads the same head.
//
// * (@ camera)
//
// also works because @ camera reduces to a Cell.
//
// An unbound cell realizes to Unit for now.
//

Ptr program_head(
    Runtime& runtime,
    Node* relation,
    Ptr
) {
    auto terms =
        runtime.terms(relation);

    if (
        terms.empty() ||
        !is_symbol_text(
            runtime,
            terms.front(),
            "*"
        )
    ) {
        return relation;
    }

    if (terms.size() < 2) {
        return relation;
    }

    std::vector<Ptr> tail(
        terms.begin() + 1,
        terms.end()
    );

    Ptr target =
        runtime.product(
            std::move(tail)
        );

    if (runtime.is_wildcard(target))
        throw std::runtime_error("anonymous _ has no binding head");

    // Direct Cell.
    if (runtime.is_cell(target)) {
        Ptr current =
            runtime.head(
                runtime.as_cell(target)
            );

        return
            current
                ? current
                : runtime.arena().unit();
    }

    // Symbol / Name identity.
    if (
        Symbol* symbol =
            symbol_from_identity(
                runtime,
                target
            )
    ) {
        if (
            Cell* cell =
                runtime.find_cell(symbol)
        ) {
            Ptr current =
                runtime.head(cell);

            return
                current
                    ? current
                    : runtime.arena().unit();
        }

        return runtime.arena().unit();
    }

    // Otherwise reduce the target.
    Ptr reduced =
        runtime.reduce(target);

    if (runtime.is_cell(reduced)) {
        Ptr current =
            runtime.head(
                runtime.as_cell(reduced)
            );

        return
            current
                ? current
                : runtime.arena().unit();
    }

    return reduced;
}

// Native control flow uses only ordinary products and the Program ABI.
//
// if condition consequent [alternate]
// for :element domain body
//
// Multi-term branches/bodies are grouped. A reified product is a data domain:
// for -item -("a" "b") (consumer item)
// The iteration binding is restored, including when a body throws.
Ptr demand_program_form(Runtime& runtime, Ptr form) {
    if (runtime.is_relation(form)) {
        auto terms = runtime.terms(runtime.as_node(form));
        // A named relation stays data during ordinary reduction. Explicit
        // demand activates its provider, using the ordinary Program ABI.
        bool callable_head = runtime.is_symbol(terms.front()) &&
            static_cast<bool>(runtime.resolve_program(runtime.as_symbol(terms.front())));
        if (!callable_head && terms.size() >= 3 && runtime.is_name(terms[1])) {
            auto operation = runtime.as_name(terms[1])->target;
            if (runtime.is_symbol(operation) && runtime.resolve_program(runtime.as_symbol(operation))) {
                std::vector<Ptr> call{operation, terms[0]};
                call.insert(call.end(), terms.begin()+2, terms.end());
                form = runtime.product(std::move(call));
            }
        }
    }
    if (runtime.is_symbol(form) && !runtime.binding(runtime.as_symbol(form))) {
        if (runtime.resolve_program(runtime.as_symbol(form)))
            form = runtime.arena().make_relation({form});
    }
    return runtime.reduce(form);
}

Ptr program_truth(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), "truth")) return relation;
    if (terms.size() != 2) throw std::runtime_error("truth expects one value");
    Ptr value = demand_program_form(runtime, terms[1]);
    return runtime.truth(value) ? static_cast<Ptr>(runtime.arena().make_name(runtime.intern("true")))
                                : static_cast<Ptr>(runtime.arena().unit());
}

Ptr program_if(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), "if")) return relation;
    if (terms.size() != 3 && terms.size() != 4)
        throw std::runtime_error("if expects condition, consequent, and optional alternate");
    Ptr condition = demand_program_form(runtime, terms[1]);
    // The truth Program is replaceable by a domain host without changing Seed.
    Ptr decision = runtime.reduce(runtime.product({runtime.intern("truth"), condition}));
    if (runtime.truth(decision)) return demand_program_form(runtime, terms[2]);
    if (terms.size() == 4) return demand_program_form(runtime, terms[3]);
    return runtime.arena().unit();
}

// Equal-precedence, left-to-right short circuit, retaining the selected value.
// Prefix use (&& A B / || A B) uses the same lazy binary operation.
Ptr program_boolean(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    auto boolean = [&](Ptr p) {
        return is_symbol_text(runtime, p, "&&") || is_symbol_text(runtime, p, "||");
    };
    auto choose = [&](Ptr operation, Ptr left, Ptr right) {
        Ptr value = demand_program_form(runtime, left);
        Ptr decision = runtime.reduce(runtime.product({runtime.intern("truth"), value}));
        bool success = runtime.truth(decision);
        bool use_right = is_symbol_text(runtime, operation, "&&") ? success : !success;
        return use_right ? demand_program_form(runtime, right) : value;
    };
    if (boolean(terms.front())) {
        if (terms.size() != 3) throw std::runtime_error("&& / || expect two grouped operands");
        return choose(terms[0], terms[1], terms[2]);
    }
    // Splitting on the last operator gives a left-associated structure.
    for (std::size_t i = terms.size(); i-- > 0;) {
        if (!boolean(terms[i])) continue;
        if (i == 0 || i + 1 == terms.size())
            throw std::runtime_error("&& / || require both operands");
        return choose(terms[i],
            runtime.product(std::vector<Ptr>(terms.begin(), terms.begin() + i)),
            runtime.product(std::vector<Ptr>(terms.begin() + i + 1, terms.end())));
    }
    return relation;
}

// -----------------------------------------------------------------------------
// Operators: comparison, arithmetic, one reduction step
// -----------------------------------------------------------------------------
//
// An operator is a standalone symbol. Attached spellings keep their meaning:
// -3 is a number, -foo a Name, *cell reads a cell. So (3 - 3) is 0 while
// (3 -3) is the product of 3 and -3.

struct OperatorCall {
    Ptr op = nullptr;
    Ptr left = nullptr;   // unevaluated operand forms
    Ptr right = nullptr;  // nullptr for unary minus
    bool prefix = false;
};

// Find the operator of one precedence class that owns this product: the
// rightmost infix occurrence (left associativity), or a prefix spelling.
bool split_operator(Runtime& runtime, const std::vector<Ptr>& terms,
                    bool (*member)(std::string_view), OperatorCall& call) {
    auto is_member = [&](Ptr p) {
        return runtime.is_symbol(p) && member(runtime.as_symbol(p)->text);
    };
    if (terms.empty()) return false;
    for (std::size_t i = terms.size() - 1; i >= 1; --i) {
        if (!is_member(terms[i]) || i + 1 >= terms.size()) continue;
        // An operator directly after another is not infix (e.g. a - - b).
        if (runtime.is_symbol(terms[i - 1]) && is_orientation_text(runtime.as_symbol(terms[i - 1])->text)) continue;
        call = {terms[i],
                runtime.product(std::vector<Ptr>(terms.begin(), terms.begin() + static_cast<std::ptrdiff_t>(i))),
                runtime.product(std::vector<Ptr>(terms.begin() + static_cast<std::ptrdiff_t>(i) + 1, terms.end())),
                false};
        return true;
    }
    if (is_member(terms.front())) {
        if (terms.size() == 3) { call = {terms[0], terms[1], terms[2], true}; return true; }
        if (terms.size() == 2) { call = {terms[0], terms[1], nullptr, true}; return true; }
    }
    return false;
}

bool is_value_form(const Runtime& runtime, Ptr p) {
    return runtime.is_number(p) || runtime.is_literal(p) || runtime.is_name(p) ||
           runtime.is_unit(p) || runtime.is_cell(p);
}

Ptr truth_name(Runtime& runtime, bool value) {
    return runtime.arena().make_name(runtime.intern(value ? "true" : "false"));
}

// -1/0/1 for two numbers or two strings; nullopt when the kinds differ.
std::optional<int> scalar_order(Runtime& runtime, Ptr left, Ptr right) {
    if (runtime.is_number(left) && runtime.is_number(right)) {
        auto a = runtime.as_number(left), b = runtime.as_number(right);
        bool ai = runtime.arena().is(left, HostKind::Integer), bi = runtime.arena().is(right, HostKind::Integer);
        if (ai && bi) return (a->integer > b->integer) - (a->integer < b->integer);
        if (!ai && !bi) return (a->real > b->real) - (a->real < b->real);
        auto integer = ai ? a->integer : b->integer;
        auto real = ai ? b->real : a->real;
        // Preserve exact int64 ordering at the double precision boundary.
        int mixed;
        if (real >= 9223372036854775808.0) mixed = -1;
        else if (real < -9223372036854775808.0) mixed = 1;
        else {
            auto whole = static_cast<std::int64_t>(real);
            mixed = (integer > whole) - (integer < whole);
            if (!mixed) mixed = (static_cast<double>(whole) > real) - (static_cast<double>(whole) < real);
        }
        return ai ? mixed : -mixed;
    }
    if (runtime.is_literal(left) && runtime.is_literal(right)) {
        auto order = runtime.as_literal(left)->bytes.compare(runtime.as_literal(right)->bytes);
        return (order > 0) - (order < 0);
    }
    return std::nullopt;
}

bool same_value(Runtime& runtime, Ptr left, Ptr right) {
    if (left == right) return true;
    if (auto order = scalar_order(runtime, left, right)) return *order == 0;
    if (runtime.is_name(left) && runtime.is_name(right)) {
        Ptr a = runtime.as_name(left)->target, b = runtime.as_name(right)->target;
        if (a == b) return true;
        return runtime.is_relation(a) && runtime.is_relation(b) && runtime.structural(a) == runtime.structural(b);
    }
    return false;
}

Ptr compare_values(Runtime& runtime, std::string_view op, Ptr left, Ptr right) {
    if (op == "==") return truth_name(runtime, same_value(runtime, left, right));
    if (op == "!=") return truth_name(runtime, !same_value(runtime, left, right));
    auto order = scalar_order(runtime, left, right);
    if (!order)
        throw std::runtime_error("ordering requires two numbers or two strings; domain ordering belongs to its provider");
    bool result = op == "<" ? *order < 0 : op == "<=" ? *order <= 0 : op == ">" ? *order > 0 : *order >= 0;
    return truth_name(runtime, result);
}

Ptr make_integer(Runtime& runtime, std::int64_t value) {
    return runtime.arena().make_number(std::to_string(value));
}

Ptr make_real(Runtime& runtime, double value) {
    if (!std::isfinite(value)) throw std::runtime_error("arithmetic result is not a finite number");
    char buffer[64];
    auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    std::string spelling(buffer, written.ptr);
    if (spelling.find_first_of(".eE") == std::string::npos) spelling += ".0";
    return runtime.arena().make_number(spelling);
}

double real_of(Runtime& runtime, Ptr number) {
    auto n = runtime.as_number(number);
    return runtime.arena().is(number, HostKind::Integer) ? static_cast<double>(n->integer) : n->real;
}

Ptr arithmetic_values(Runtime& runtime, std::string_view op, Ptr left, Ptr right) {
    if (!right) { // unary minus
        if (!runtime.is_number(left)) throw std::runtime_error("- negates a number");
        if (runtime.arena().is(left, HostKind::Integer)) {
            auto v = runtime.as_number(left)->integer;
            if (v == INT64_MIN) throw std::runtime_error("integer overflow");
            return make_integer(runtime, -v);
        }
        return make_real(runtime, -runtime.as_number(left)->real);
    }
    if (op == "+" && runtime.is_literal(left) && runtime.is_literal(right))
        return runtime.arena().make_literal(runtime.as_literal(left)->bytes + runtime.as_literal(right)->bytes);
    if (!runtime.is_number(left) || !runtime.is_number(right))
        throw std::runtime_error(std::string(op) + " requires two numbers" + (op == "+" ? " or two strings" : ""));
    bool integers = runtime.arena().is(left, HostKind::Integer) && runtime.arena().is(right, HostKind::Integer);
    if (integers) {
        auto a = runtime.as_number(left)->integer, b = runtime.as_number(right)->integer;
        auto overflow = [] { return std::runtime_error("integer overflow"); };
        if (op == "+") {
            if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) throw overflow();
            return make_integer(runtime, a + b);
        }
        if (op == "-") {
            if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) throw overflow();
            return make_integer(runtime, a - b);
        }
        if (op == "*") {
            if (a != 0 && b != 0) {
                if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) throw overflow();
                auto product = a * b;  // checked below by division
                if (product / b != a) throw overflow();
                return make_integer(runtime, product);
            }
            return make_integer(runtime, 0);
        }
        if (b == 0) throw std::runtime_error("division by zero");
        if (a == INT64_MIN && b == -1) throw overflow();
        // Exact division stays an integer; otherwise the quotient is real.
        if (a % b == 0) return make_integer(runtime, a / b);
        return make_real(runtime, static_cast<double>(a) / static_cast<double>(b));
    }
    double a = real_of(runtime, left), b = real_of(runtime, right);
    if (op == "+") return make_real(runtime, a + b);
    if (op == "-") return make_real(runtime, a - b);
    if (op == "*") return make_real(runtime, a * b);
    if (b == 0) throw std::runtime_error("division by zero");
    return make_real(runtime, a / b);
}

bool additive_member(std::string_view t) { return t == "+" || t == "-"; }
bool multiplicative_member(std::string_view t) { return t == "*" || t == "/"; }

Ptr program_head(Runtime& runtime, Node* relation, Ptr);

Ptr program_arithmetic(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    OperatorCall call;
    // Prefix * is the cell read (* name); only infix * multiplies.
    if (is_symbol_text(runtime, terms.front(), "*")) return program_head(runtime, relation, nullptr);
    if (!split_operator(runtime, terms, additive_member, call) &&
        !split_operator(runtime, terms, multiplicative_member, call)) return relation;
    auto op = runtime.as_symbol(call.op)->text;
    if (!call.right && op != "-") return relation;
    if (call.prefix && op == "*") return relation;
    Ptr left = demand_program_form(runtime, call.left);
    Ptr right = call.right ? demand_program_form(runtime, call.right) : nullptr;
    return arithmetic_values(runtime, op, left, right);
}

Ptr program_compare(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    OperatorCall call;
    if (!split_operator(runtime, terms, is_comparison_text, call) || !call.right) {
        for (Ptr term : terms)
            if (runtime.is_symbol(term) && is_comparison_text(runtime.as_symbol(term)->text) && terms.size() > 1)
                throw std::runtime_error("comparison requires both operands: A < B or < A B");
        return relation;
    }
    std::size_t count = 0;
    for (Ptr term : terms)
        if (runtime.is_symbol(term) && is_comparison_text(runtime.as_symbol(term)->text)) ++count;
    if (count > 1) throw std::runtime_error("chained comparisons are not supported; join them with && or ||");
    Ptr left = demand_program_form(runtime, call.left);
    Ptr right = demand_program_form(runtime, call.right);
    return compare_values(runtime, runtime.as_symbol(call.op)->text, left, right);
}

Ptr program_append_outside_braces(Runtime& runtime, Node* relation, Ptr) {
    for (Ptr term : runtime.terms(relation))
        if (is_symbol_text(runtime, term, ">>"))
            throw std::runtime_error(">> appends output only inside { }: { PRODUCER >> \"file\" }");
    return relation;
}

// -----------------------------------------------------------------------------
// => : one reduction step
// -----------------------------------------------------------------------------
//
// EXPR => advances EXPR by exactly one step of the reducer's own order and
// returns the resulting form reified, so it is not reduced further. Steps are
// mathematical: orienting |, normalizing a colon abstraction, unfolding a
// binding, beta-substituting an argument, or computing an arithmetic or
// comparison operator on values. Invoking any other Program (builtins, native
// processes, pipelines, bindings, blocks) is an effect: => stops before it and
// says which, instead of running it.

struct Step {
    enum Kind { Normal, Reduced, Effect } kind = Normal;
    Ptr form = nullptr;
    std::string note;
};

Step step_form(Runtime& runtime, Ptr form);

Step step_operator(Runtime& runtime, Ptr form, std::string_view spelling) {
    auto terms = runtime.terms(runtime.as_node(form));
    if (spelling == ":=" || spelling == "=") return {Step::Effect, form, "binding a name"};
    if (spelling == "=>") return {Step::Effect, form, "a nested =>"};
    if (spelling == "&&" || spelling == "||") return {Step::Effect, form, "short-circuit " + std::string(spelling) + " evaluates its operands"};
    if (spelling == ">>") return {Step::Effect, form, ">> outside { }"};
    if (spelling == ":") return {Step::Normal, form, {}};  // abstractions are handled by shape
    if (spelling == "|") {
        std::vector<Ptr> stages;
        Ptr oriented = runtime.flow(runtime.as_node(form), stages);
        if (oriented != form) return {Step::Reduced, oriented, "orient |"};
        return {Step::Normal, form, {}};
    }
    OperatorCall call;
    bool comparison = is_comparison_text(spelling);
    if (spelling == "*" && is_symbol_text(runtime, terms.front(), "*"))
        return {Step::Effect, form, "reading a binding cell"};
    bool found = comparison ? split_operator(runtime, terms, is_comparison_text, call)
                            : (split_operator(runtime, terms, additive_member, call) ||
                               split_operator(runtime, terms, multiplicative_member, call));
    if (!found) return {Step::Normal, form, {}};
    auto op = runtime.as_symbol(call.op)->text;
    auto rebuild = [&](Ptr left, Ptr right) {
        if (call.prefix) return right ? runtime.product({call.op, left, right}) : runtime.product({call.op, left});
        return runtime.product({left, call.op, right});
    };
    // Operands reduce left to right, one step at a time.
    if (!is_value_form(runtime, call.left)) {
        auto inner = step_form(runtime, call.left);
        if (inner.kind == Step::Reduced) return {Step::Reduced, rebuild(inner.form, call.right), inner.note};
        return {inner.kind == Step::Effect ? Step::Effect : Step::Normal, form, inner.note};
    }
    if (call.right && !is_value_form(runtime, call.right)) {
        auto inner = step_form(runtime, call.right);
        if (inner.kind == Step::Reduced) return {Step::Reduced, rebuild(call.left, inner.form), inner.note};
        return {inner.kind == Step::Effect ? Step::Effect : Step::Normal, form, inner.note};
    }
    Ptr value = comparison ? compare_values(runtime, op, call.left, call.right)
                           : arithmetic_values(runtime, op, call.left, call.right);
    return {Step::Reduced, value, "compute " + op};
}

Step step_form(Runtime& runtime, Ptr form) {
    if (runtime.is_symbol(form)) {
        auto symbol = runtime.as_symbol(form);
        if (Ptr bound = runtime.binding(symbol)) return {Step::Reduced, bound, "unfold " + symbol->text};
        if (runtime.resolve_program(symbol)) return {Step::Effect, form, "running program " + symbol->text};
        return {Step::Normal, form, {}};
    }
    if (!runtime.is_relation(form)) return {Step::Normal, form, {}};
    if (runtime.pipeline_body(form)) return {Step::Effect, form, "running a { } pipeline"};
    auto node = runtime.as_node(form);
    if (is_symbol_text(runtime, node->head, block_head)) return {Step::Effect, form, "running a line block"};
    if (is_symbol_text(runtime, node->head, list_head)) return {Step::Effect, form, "building a list"};
    AbstractionView view;
    if (abstraction_view(runtime, {node, nullptr}, view)) {
        if (!view.canonical)
            return {Step::Reduced, runtime.product({view.binder, runtime.arena().unit(), slice_product(runtime, view.body)}),
                    "normalize :"};
        return {Step::Normal, form, {}};
    }
    auto terms = runtime.terms(node);
    // Same dispatch order as the reducer: each present operator may own it.
    if (has_orientation(runtime, terms)) {
        for (auto spelling : orientation_order) {
            if (std::none_of(terms.begin(), terms.end(), [&](Ptr term) { return is_symbol_text(runtime, term, spelling); }))
                continue;
            auto step = step_operator(runtime, form, spelling);
            if (step.kind != Step::Normal) return step;
        }
    }
    Ptr first = terms.front();
    if (runtime.is_symbol(first)) {
        auto symbol = runtime.as_symbol(first);
        Ptr expanded = runtime.application(form);
        if (expanded != form) return {Step::Reduced, expanded, "unfold " + symbol->text};
        if (runtime.resolve_program(symbol)) return {Step::Effect, form, "running program " + symbol->text};
        return {Step::Normal, form, {}};
    }
    if (runtime.is_relation(first) && terms.size() >= 2) {
        Ptr binder = nullptr, body = nullptr;
        if (runtime.abstraction(first, binder, body)) {
            if (runtime.is_relation(body)) {
                auto leading = static_cast<Node*>(body);
                if (leading->ref && runtime.is_literal(leading->head))
                    body = slice_product(runtime, {next_term(leading), nullptr});
            }
            Ptr result = runtime.substitute(body, binder, terms[1]);
            for (std::size_t i = 2; i < terms.size(); ++i) result = runtime.compose(result, terms[i]);
            return {Step::Reduced, result, "apply abstraction"};
        }
        auto inner = step_form(runtime, first);
        if (inner.kind != Step::Reduced) return {inner.kind, form, inner.note};
        Ptr result = inner.form;
        for (std::size_t i = 1; i < terms.size(); ++i) result = runtime.compose(result, terms[i]);
        return {Step::Reduced, result, inner.note};
    }
    return {Step::Normal, form, {}};
}

Ptr program_redux(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    std::size_t steps = 0;
    std::vector<Ptr> operand;
    bool seen_operand = false, trailing = false;
    for (Ptr term : terms) {
        if (is_symbol_text(runtime, term, "=>")) { ++steps; if (seen_operand) trailing = true; continue; }
        if (trailing) throw std::runtime_error("=> steps the whole expression: write EXPR => (or => EXPR)");
        operand.push_back(term);
        seen_operand = true;
    }
    if (!steps) return relation;
    if (operand.empty()) throw std::runtime_error("=> needs an expression to step");
    Ptr form = runtime.product(std::move(operand));
    // Continue from a reified step result, from an expression or a binding.
    auto reified = [&](Ptr p) -> Ptr {
        return runtime.is_name(p) && runtime.is_relation(runtime.as_name(p)->target) ? runtime.as_name(p)->target : nullptr;
    };
    if (Ptr reified_form = reified(form)) form = reified_form;
    else if (runtime.is_symbol(form)) {
        if (Ptr bound = runtime.binding(runtime.as_symbol(form)); bound && reified(bound)) form = reified(bound);
    } else if (runtime.is_relation(form) && has_orientation(runtime, runtime.terms(runtime.as_node(form)))) {
        for (Ptr term : runtime.terms(runtime.as_node(form)))
            if (is_symbol_text(runtime, term, "=>")) {
                if (Ptr earlier = reified(runtime.reduce(form))) form = earlier;
                break;
            }
    }
    for (std::size_t i = 0; i < steps; ++i) {
        auto step = step_form(runtime, form);
        if (step.kind == Step::Effect)
            throw std::runtime_error("=> stops before an effect (" + step.note + "): " + runtime.print(form) +
                                     "; demand it with ? to run it");
        if (step.kind == Step::Normal) break;
        form = step.form;
    }
    return runtime.arena().make_name(form);
}

// -----------------------------------------------------------------------------
// Line blocks and lists
// -----------------------------------------------------------------------------
//
// A group whose ( ends its line reads its lines as statements, as a file does,
// and lowers to ((;) STATEMENT ...). It runs them in order in the enclosing
// scope and answers the last value; a leading string is documentation.
// [a b c] lowers to ([] a b c) and evaluates each element once, like
// data list.

Ptr program_block(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), block_head)) return relation;
    std::size_t first = terms.size() > 2 && runtime.is_literal(terms[1]) ? 2 : 1;
    Ptr result = runtime.arena().unit();
    for (std::size_t i = first; i < terms.size(); ++i) result = demand_program_form(runtime, terms[i]);
    return result;
}

Ptr list_value(Runtime& runtime, std::vector<Ptr> elements) {
    return runtime.arena().make_name(runtime.product({runtime.arena().make_name(runtime.intern("list")),
                                                      runtime.arena().make_name(runtime.product(std::move(elements)))}));
}

Ptr program_list(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), list_head)) return relation;
    std::vector<Ptr> elements;
    for (std::size_t i = 1; i < terms.size(); ++i) elements.push_back(runtime.reduce(terms[i]));
    return list_value(runtime, std::move(elements));
}

Ptr program_for(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), "for")) return relation;
    if (terms.size() != 4)
        throw std::runtime_error("for expects a binding identity, finite domain, and body");
    Symbol* variable = symbol_from_identity(runtime, terms[1]);
    if (!variable) throw std::runtime_error("for binding must be a Symbol or Name");
    auto elements = runtime.collection(runtime.reduce(terms[2]));
    Cell* cell = runtime.ensure_cell(variable);
    Ptr previous = runtime.head(cell);
    struct BindingGuard {
        Runtime& runtime; Symbol* variable; Ptr previous;
        ~BindingGuard() { runtime.bind(variable, previous); }
    } guard{runtime, variable, previous};
    std::vector<Ptr> results;
    results.reserve(elements.size());
    for (Ptr element : elements) {
        runtime.bind(variable, element);
        results.push_back(demand_program_form(runtime, terms[3]));
    }
    // Results are finite data, not a new consumer-first executable relation.
    return runtime.arena().make_name(runtime.product(std::move(results)));
}

} // namespace

// =============================================================================
// Arena
// =============================================================================

Arena::Arena() {
    auto unit =
        std::make_unique<Node>();

    unit_ = unit.get();

    // Unit has a real stable address.
    //
    // nullptr remains reserved for:
    //
    //     no head
    //     no args
    //     no ref
    //
    unit_->head = unit_;

    nodes_.push_back(
        std::move(unit)
    );

    meta_.emplace(
        static_cast<Ptr>(unit_),
        HostKind::Unit
    );
}

Node* Arena::make_node(
    Ptr head
) {
#ifdef SEED_PROFILE
    ++profile_.nodes;
#endif
    auto node =
        std::make_unique<Node>();

    node->head = head;

    Node* raw =
        node.get();

    nodes_.push_back(
        std::move(node)
    );

    meta_.emplace(
        static_cast<Ptr>(raw),
        HostKind::Node
    );

    return raw;
}

Node* Arena::make_relation(
    const std::vector<Ptr>& forms
) {
#ifdef SEED_PROFILE
    ++profile_.relations;
#endif
    if (forms.empty()) {
        return unit_;
    }

    Node* first = nullptr;
    Node* previous = nullptr;

    for (Ptr form : forms) {
        Node* current =
            make_node(form);

        if (!first) {
            first = current;
        }

        if (previous) {
            previous->ref =
                current;
        }

        previous =
            current;
    }

    return first;
}

Symbol* Arena::intern(
    std::string_view text
) {
    const std::string key(
        text
    );

    if (
        auto it =
            interned_.find(key);
        it != interned_.end()
    ) {
        return it->second;
    }

    auto symbol =
        std::make_unique<Symbol>();

    symbol->text =
        key;

    Symbol* raw =
        symbol.get();

    symbols_.push_back(
        std::move(symbol)
    );

    meta_.emplace(
        static_cast<Ptr>(raw),
        HostKind::Symbol
    );

    interned_.emplace(
        raw->text,
        raw
    );

    return raw;
}

Name* Arena::make_name(
    Ptr target
) {
    auto name =
        std::make_unique<Name>();

    name->target =
        target;

    Name* raw =
        name.get();

    names_.push_back(
        std::move(name)
    );

    meta_.emplace(
        static_cast<Ptr>(raw),
        HostKind::Name
    );

    return raw;
}

Literal* Arena::make_literal(
    std::string_view bytes
) {
    auto literal =
        std::make_unique<Literal>();

    literal->bytes.assign(
        bytes
    );

    Literal* raw =
        literal.get();

    literals_.push_back(
        std::move(literal)
    );

    meta_.emplace(
        static_cast<Ptr>(raw),
        HostKind::Literal
    );

    return raw;
}

Cell* Arena::make_cell(
    Symbol* symbol
) {
    if (!symbol || symbol->text == "_") {
        throw std::invalid_argument(
            "cell requires symbol"
        );
    }

    auto cell =
        std::make_unique<Cell>();

    cell->symbol =
        symbol;

    Cell* raw =
        cell.get();

    cells_.push_back(
        std::move(cell)
    );

    meta_.emplace(
        static_cast<Ptr>(raw),
        HostKind::Cell
    );

    return raw;
}

Ptr Arena::make_wildcard() {
    auto value = make_node();
    meta_[value] = HostKind::Wildcard;
    return value;
}

Ptr Arena::make_number(std::string_view spelling) {
    auto value = std::make_unique<Number>();
    value->spelling = spelling;
    bool real = spelling.find_first_of(".eE") != std::string_view::npos;
    auto first = spelling.data(), last = first + spelling.size();
    auto result = real ? std::from_chars(first, last, value->real)
                       : std::from_chars(first, last, value->integer);
    if (result.ec != std::errc{} || result.ptr != last || (real && !std::isfinite(value->real)))
        throw std::runtime_error("invalid or out-of-range numeric literal");
    Ptr raw = value.get();
    numbers_.push_back(std::move(value));
    meta_[raw] = real ? HostKind::Real : HostKind::Integer;
    return raw;
}

HostKind Arena::kind(
    Ptr p
) const {
    if (!p) {
        throw std::invalid_argument(
            "null is not a Seed form"
        );
    }

    const auto it =
        meta_.find(p);

    if (
        it ==
        meta_.end()
    ) {
        throw std::invalid_argument(
            "pointer is not owned by this Seed runtime"
        );
    }

    return it->second;
}

bool Arena::is(
    Ptr p,
    HostKind expected
) const {
    if (!p) {
        return false;
    }

    const auto it =
        meta_.find(p);

    return
        it != meta_.end() &&
        it->second == expected;
}

// =============================================================================
// Registry
// =============================================================================

void Registry::define(
    Symbol* symbol,
    Program program
) {
    if (!symbol) {
        throw std::invalid_argument(
            "define requires a symbol"
        );
    }

    if (!program) {
        throw std::invalid_argument(
            "define requires a program"
        );
    }

    programs_[symbol] =
        program;
}

void Registry::describe(Symbol* symbol, std::string text) {
    if (!symbol) throw std::invalid_argument("describe requires a symbol");
    descriptions_[symbol] = std::move(text);
}

std::string_view Registry::description(Symbol* symbol) const {
    auto it = descriptions_.find(symbol);
    return it == descriptions_.end() ? std::string_view{} : std::string_view{it->second};
}

Program Registry::find(
    Symbol* symbol
) const {
    if (!symbol) {
        return {};
    }

    const auto it =
        programs_.find(symbol);

    if (
        it ==
        programs_.end()
    ) {
        return {};
    }

    return it->second;
}

// =============================================================================
// Reader
// =============================================================================

Reader::Reader(
    Runtime& runtime
)
    : runtime_(runtime) {}

bool Reader::eof() const noexcept {
    return
        pos_ >=
        source_.size();
}

char Reader::peek(
    std::size_t lookahead
) const noexcept {
    const std::size_t index =
        pos_ + lookahead;

    return
        index >= source_.size()
            ? '\0'
            : source_[index];
}

char Reader::take() {
    return
        eof()
            ? '\0'
            : source_[pos_++];
}

void Reader::skip_space() {
    auto status = skip_trivia(source_, pos_);
    if (status == TriviaStatus::OpenComment)
        throw std::runtime_error("unterminated bracket comment");
    if (status == TriviaStatus::OpenContinuation)
        throw std::runtime_error("unterminated source continuation");
}

bool Reader::source_has_form(std::string_view source) {
    std::size_t pos = 0;
    return skip_trivia(source, pos) == TriviaStatus::Complete && pos < source.size();
}

bool Reader::source_leading_pipe(std::string_view source) {
    std::size_t pos = 0;
    if (skip_trivia(source, pos) != TriviaStatus::Complete || pos >= source.size()) return false;
    return source[pos] == '|' && (pos + 1 == source.size() || source[pos + 1] != '|');
}

bool Reader::source_complete(std::string_view source) {
    int depth = 0;
    std::size_t pos = 0;
    while (pos < source.size()) {
        if (skip_trivia(source, pos) != TriviaStatus::Complete) return false;
        if (pos == source.size()) break;
        char c = source[pos++];
        if (c == '"') {
            bool triple = pos + 1 < source.size() && source[pos] == '"' && source[pos+1] == '"';
            if (triple) pos += 2;
            bool closed = false;
            while (pos < source.size()) {
                c = source[pos++];
                if (c == '\\') {
                    if (pos == source.size()) return false;
                    ++pos;
                } else if (c == '"' && (!triple || (pos + 1 < source.size() && source[pos] == '"' && source[pos+1] == '"'))) {
                    if (triple) pos += 2;
                    closed = true; break;
                }
            }
            if (!closed) return false;
        } else if (c == '(' || c == '{' || c == '[') ++depth;
        else if ((c == ')' || c == '}' || c == ']') && --depth < 0) return true; // Reader diagnoses invalid input.
    }
    return depth == 0;
}

Ptr Reader::read(
    std::string_view source
) {
    source_ =
        source;

    pos_ = 0;

    Ptr result =
        read_sequence();

    skip_space();

    if (!eof()) {
        throw std::runtime_error(
            "unexpected trailing input"
        );
    }

    return result;
}

Ptr Reader::read_sequence(
    char until
) {
    std::vector<Ptr> forms;

    while (true) {
        skip_space();

        if (eof()) {
            if (
                until != '\0'
            ) {
                throw std::runtime_error(
                    "unterminated group"
                );
            }

            break;
        }

        if (
            until != '\0' &&
            peek() == until
        ) {
            break;
        }

        forms.push_back(
            read_form()
        );
    }

    return runtime_.product(
        std::move(forms)
    );
}

Ptr Reader::read_form() {
    if (eof()) {
        throw std::runtime_error(
            "expected form"
        );
    }

    Ptr form;
    if ((peek() == '@' || peek() == '*') && peek(1) != '\0' &&
        !std::isspace(static_cast<unsigned char>(peek(1))) && peek(1) != ')' && peek(1) != '}' &&
        peek(1) != ']' && peek(1) != '#' && peek(1) != '?' && continuation_width(source_, pos_ + 1) == 0) {
        auto prefix = std::string(1, take());
        form = runtime_.product({runtime_.intern(prefix), read_form()});
    } else if (peek() == '(') {
        form = read_group();
    } else if (peek() == '[') {
        form = read_list();
    } else if (peek() == ']') {
        throw std::runtime_error("unexpected ']'");
    } else if (peek() == '{') {
        form = read_pipeline();
    } else if (peek() == '-' && (peek(1) == '(' || peek(1) == '{' ||
        std::isalpha(static_cast<unsigned char>(peek(1))) ||
        static_cast<unsigned char>(peek(1)) >= 128 || peek(1) == '_')) {
        bool grouped = peek(1) == '(' || peek(1) == '{';
        take();
        Ptr target = read_form();
        if (!grouped && runtime_.is_symbol(target)) target = named_operation(runtime_, runtime_.as_symbol(target)->text);
        form = runtime_.arena().make_name(target);
    } else if (peek() == ':') {
        if (peek(1) == '=') {
            take(); take();
            form = runtime_.intern(":=");
        } else { take(); form = runtime_.intern(":"); }
    } else if (peek() == '"') {
        form = read_literal();
    } else {
        form = read_symbol();
    }
    // Attachment is lexical: a? is a demanded product, a ? is two terms.
    // Lower into existing Ptr relations; no new evaluator or host payload kind.
    while (peek() == '?' && !eof()) {
        take();
        form = runtime_.arena().make_relation({runtime_.intern("demand"), form});
    }
    return form;
}

Ptr Reader::read_group() {
    take();

    // ()
    if (peek() == ')') {
        take();

        return
            runtime_.arena()
                .unit();
    }

    if (opens_line_block()) return read_line_block();

    Ptr inside =
        read_sequence(')');

    if (
        eof() ||
        take() != ')'
    ) {
        throw std::runtime_error(
            "expected ')'"
        );
    }

    return inside;
}

// A ( that ends its physical line (after spaces, tabs or a line comment)
// opens a line block. A group that starts on the same line stays one
// expression whose newlines are whitespace.
bool Reader::opens_line_block() const {
    std::size_t p = pos_;
    while (p < source_.size() && (source_[p] == ' ' || source_[p] == '\t' || source_[p] == '\r')) ++p;
    if (p < source_.size() && source_[p] == '#' && (p + 1 == source_.size() || source_[p + 1] != '['))
        while (p < source_.size() && source_[p] != '\n') ++p;
    return p < source_.size() && source_[p] == '\n';
}

// Lines are statements, with the file rules: a newline ends a statement,
// a trailing \ or an open group continues it, and a line starting with |
// (not ||) continues the previous one.
Ptr Reader::read_line_block() {
    std::vector<Ptr> statements{runtime_.intern(block_head)};
    std::vector<Ptr> current;
    auto flush = [&] {
        if (!current.empty()) statements.push_back(runtime_.product(std::move(current)));
        current.clear();
    };
    while (true) {
        bool newline = false;
        auto status = skip_trivia(source_, pos_, &newline);
        if (status == TriviaStatus::OpenComment) throw std::runtime_error("unterminated bracket comment");
        if (status == TriviaStatus::OpenContinuation) throw std::runtime_error("unterminated source continuation");
        if (eof()) throw std::runtime_error("unterminated group");
        if (peek() == ')') { take(); break; }
        if (newline && !current.empty() && !(peek() == '|' && peek(1) != '|')) flush();
        current.push_back(read_form());
    }
    flush();
    if (statements.size() == 1) return runtime_.arena().unit();
    if (statements.size() == 2) return statements[1];
    return runtime_.arena().make_relation(statements);
}

// [a b c] lowers to ([] a b c); [] is the empty list.
Ptr Reader::read_list() {
    take();
    std::vector<Ptr> forms{runtime_.intern(list_head)};
    while (true) {
        skip_space();
        if (eof()) throw std::runtime_error("unterminated list: expected ']'");
        if (peek() == ']') { take(); break; }
        if (peek() == ')' || peek() == '}') throw std::runtime_error("expected ']'");
        forms.push_back(read_form());
    }
    return runtime_.arena().make_relation(forms);
}

// `{ forms }` lowers to ({} (forms)): a reserved head over a nested body, so
// the body's | never orients the enclosing relation and no new host kind is
// needed. Once { is a delimiter, source cannot spell the symbol {} itself.
Ptr Reader::read_pipeline() {
    take();
    skip_space();
    Ptr body;
    if (peek() == '}') {
        take();
        body = runtime_.arena().unit();
    } else {
        std::vector<Ptr> forms;
        while (true) {
            skip_space();
            if (eof()) throw std::runtime_error("unterminated pipeline context: expected '}'");
            if (peek() == '}') { take(); break; }
            if (peek() == ')') throw std::runtime_error("expected '}' before ')'");
            forms.push_back(read_form());
        }
        body = runtime_.arena().make_relation(forms);
    }
    return runtime_.arena().make_relation({runtime_.intern("{}"), body});
}

Ptr Reader::read_literal() {
    return runtime_.arena().make_literal(read_literal_bytes());
}

std::string Reader::documentation(std::string_view source) {
    const auto saved_source = source_;
    const auto saved_pos = pos_;
    struct Restore {
        Reader& reader; std::string_view source; std::size_t pos;
        ~Restore() { reader.source_ = source; reader.pos_ = pos; }
    } restore{*this, saved_source, saved_pos};
    source_ = source; pos_ = 0;
    skip_space();
    return peek() == '"' ? read_literal_bytes() : std::string{};
}

std::string Reader::read_literal_bytes() {
    take();
    bool triple = peek() == '"' && peek(1) == '"';
    if (triple) { take(); take(); }

    std::string bytes;

    while (!eof()) {
        const char c =
            take();

        if (c == '"' && (!triple || (peek() == '"' && peek(1) == '"'))) {
            if (triple) { take(); take(); }
            return bytes;
        }

        if (c == '\\') {
            if (eof()) {
                throw std::runtime_error(
                    "unterminated string escape"
                );
            }

            switch (
                const char escaped =
                    take()
            ) {
                case 'n':
                    bytes.push_back('\n');
                    break;

                case 'r':
                    bytes.push_back('\r');
                    break;

                case 't':
                    bytes.push_back('\t');
                    break;

                case '\\':
                    bytes.push_back('\\');
                    break;

                case '"':
                    bytes.push_back('"');
                    break;

                default:
                    bytes.push_back('\\');
                    bytes.push_back(
                        escaped
                    );
                    break;
            }

            continue;
        }

        bytes.push_back(c);
    }

    throw std::runtime_error(
        "unterminated string literal"
    );
}

std::string Reader::read_symbol_token() {
    std::string token;

    // || is one identity; | | is two pipe identities. These are delimiters
    // even without surrounding spaces, as ordinary shell control operators are.
    if (peek() == '|') {
        token.push_back(take());
        if (peek() == '|') token.push_back(take());
        return token;
    }
    // > >> >=   < <=   = == =>   !=
    if (peek() == '>') { token.push_back(take()); if (peek() == '>' || peek() == '=') token.push_back(take()); return token; }
    if (peek() == '<') { token.push_back(take()); if (peek() == '=') token.push_back(take()); return token; }
    if (peek() == '=') { token.push_back(take()); if (peek() == '=' || peek() == '>') token.push_back(take()); return token; }
    if (peek() == '!' && peek(1) == '=') { take(); take(); return "!="; }
    if (peek() == '&' && peek(1) == '&') { take(); take(); return "&&"; }

    while (!eof()) {
        const char c =
            peek();

        if (
            std::isspace(
                static_cast<unsigned char>(c)
            ) ||
            c == '(' ||
            c == ')' ||
            c == '{' ||
            c == '}' ||
            c == '[' ||
            c == ']' ||
            c == '"' ||
            c == ':' || c == '#' || c == '|' || c == '<' || c == '>' || c == '=' ||
            continuation_width(source_, pos_) != 0 ||
            (c == '&' && peek(1) == '&') ||
            (c == '!' && peek(1) == '=') ||
            (c == '?' && !token.empty())
        ) {
            break;
        }

        token.push_back(
            take()
        );
    }

    return token;
}

Ptr Reader::read_symbol() {
    std::string token =
        read_symbol_token();

    if (token.empty()) {
        throw std::runtime_error(
            "expected symbol"
        );
    }

    if (token == "_") return runtime_.arena().make_wildcard();
    auto digit = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t start = token[0] == '-' ? 1 : 0;
    if (start < token.size() && (digit(token[start]) ||
        (token[start] == '.' && start + 1 < token.size() && digit(token[start+1]))))
        return runtime_.arena().make_number(token);
    return runtime_.arena().intern(token);
}

// =============================================================================
// Printer
// =============================================================================

std::string Printer::quote(
    std::string_view text
) {
    std::string out =
        "\"";

    for (char c : text) {
        switch (c) {
            case '\n':
                out += "\\n";
                break;

            case '\r':
                out += "\\r";
                break;

            case '\t':
                out += "\\t";
                break;

            case '\\':
                out += "\\\\";
                break;

            case '"':
                out += "\\\"";
                break;

            default:
                out.push_back(c);
                break;
        }
    }

    out += '"';

    return out;
}

std::string Printer::native(
    Ptr form
) const {
    return
        native_form(form);
}

std::string Printer::native_form(
    Ptr form
) const {
    switch (
        runtime_.arena()
            .kind(form)
    ) {
        case HostKind::Unit:
            return "()";

        case HostKind::Wildcard:
            return "_";
        case HostKind::Integer:
        case HostKind::Real:
            return static_cast<Number*>(form)->spelling;

        case HostKind::Symbol:
            return
                static_cast<Symbol*>(form)
                    ->text;

        case HostKind::Literal:
            return
                quote(
                    static_cast<Literal*>(form)
                        ->bytes
                );

        case HostKind::Name: {
            Ptr target =
                static_cast<Name*>(form)
                    ->target;

            if (
                runtime_.is_symbol(target)
            ) {
                return name_spelling(static_cast<Symbol*>(target));
            }

            if (
                runtime_.is_unit(target)
            ) {
                return "-()";
            }

            return "-" +
                structural_form(
                    target
                );
        }

        case HostKind::Cell: {
            Cell* cell =
                static_cast<Cell*>(form);

            if (!cell->symbol) {
                return "@ -()";
            }

            return
                "@ " + name_spelling(cell->symbol);
        }

        case HostKind::Node:
            if (Ptr body = runtime_.pipeline_body(form))
                return runtime_.is_unit(body) ? "{}" : "{ " + native_relation(static_cast<Node*>(body)) + " }";
            if (runtime_.is_block(form) || runtime_.is_list_form(form)) {
                bool block = runtime_.is_block(form);
                std::string out = block ? "(\n" : "[";
                bool first = true;
                for (Node* current = static_cast<Node*>(static_cast<Node*>(form)->ref); current;
                     current = static_cast<Node*>(current->ref)) {
                    Ptr term = current->head;
                    bool nested = runtime_.is_relation(term) && !runtime_.pipeline_body(term) &&
                                  !runtime_.is_block(term) && !runtime_.is_list_form(term);
                    if (block) out += (nested ? native_relation(static_cast<Node*>(term)) : native_form(term)) + "\n";
                    else {
                        if (!first) out += ' ';
                        out += nested ? "(" + native_relation(static_cast<Node*>(term)) + ")" : native_form(term);
                    }
                    first = false;
                }
                return out + (block ? ")" : "]");
            }
            return
                native_relation(
                    static_cast<Node*>(form)
                );
    }

    throw std::runtime_error(
        "unknown Seed host kind"
    );
}

std::string Printer::native_relation(
    Node* relation
) const {
    std::string out;

    for (
        Node* current = relation;
        current;
        current =
            current->ref
                ? static_cast<Node*>(
                      current->ref
                  )
                : nullptr
    ) {
        Ptr form =
            current->head;

        if (
            runtime_.is_unit(form)
        ) {
            if (out.empty()) {
                out = "()";
            }
            else {
                out += "()";
            }

            continue;
        }

        if (!out.empty()) {
            out += ' ';
        }

        if (runtime_.pipeline_body(form) || runtime_.is_block(form) || runtime_.is_list_form(form)) {
            out += native_form(form);
        }
        else if (
            runtime_.is_relation(form)
        ) {
            out += '(';

            out +=
                native_relation(
                    static_cast<Node*>(
                        form
                    )
                );

            out += ')';
        }
        else {
            out +=
                native_form(form);
        }
    }

    return out;
}

std::string Printer::structural(
    Ptr form
) const {
    return
        structural_form(form);
}

std::string Printer::structural_member(
    Ptr form
) const {
    switch (
        runtime_.arena()
            .kind(form)
    ) {
        case HostKind::Unit:
            return "(())";

        case HostKind::Wildcard:
            return "(_)";
        case HostKind::Integer:
        case HostKind::Real:
            return "(" + static_cast<Number*>(form)->spelling + ")";

        case HostKind::Symbol:
            return "(" +
                static_cast<Symbol*>(form)
                    ->text +
                ")";

        case HostKind::Literal:
            return "(" +
                quote(
                    static_cast<Literal*>(form)
                        ->bytes
                ) +
                ")";

        case HostKind::Name: {
            Ptr target =
                static_cast<Name*>(form)
                    ->target;

            if (
                runtime_.is_symbol(target)
            ) {
                return "(" + name_spelling(static_cast<Symbol*>(target)) + ")";
            }

            if (
                runtime_.is_unit(target)
            ) {
                return "(-())";
            }

            return "(-" +
                structural_form(
                    target
                ) +
                ")";
        }

        case HostKind::Cell: {
            Cell* cell =
                static_cast<Cell*>(form);

            if (!cell->symbol) {
                return "((@)(-()))";
            }

            return
                "((@)(" + name_spelling(cell->symbol) + "))";
        }

        case HostKind::Node:
            if (runtime_.pipeline_body(form)) return "(" + structural_form(form) + ")";
            if (runtime_.is_block(form) || runtime_.is_list_form(form)) return native_form(form);
            return "(" +
                structural_relation(
                    static_cast<Node*>(
                        form
                    )
                ) +
                ")";
    }

    throw std::runtime_error(
        "unknown Seed host kind"
    );
}

std::string Printer::structural_form(
    Ptr form
) const {
    if (
        runtime_.is_unit(form)
    ) {
        return "()";
    }

    if (runtime_.is_block(form) || runtime_.is_list_form(form)) return native_form(form);

    if (Ptr body = runtime_.pipeline_body(form)) {
        if (runtime_.is_unit(body)) return "{}";
        std::string out = "{";
        for (Node* current = static_cast<Node*>(body); current;
             current = current->ref ? static_cast<Node*>(current->ref) : nullptr)
            out += structural_member(current->head);
        return out + "}";
    }

    if (
        runtime_.is_relation(form)
    ) {
        return
            structural_relation(
                static_cast<Node*>(
                    form
                )
            );
    }

    if (
        runtime_.is_cell(form)
    ) {
        Cell* cell =
            runtime_.as_cell(form);

        if (!cell->symbol) {
            return "((@)(-()))";
        }

        return
            "((@)(" +
            name_spelling(cell->symbol) +
            "))";
    }

    return
        structural_member(form);
}

std::string Printer::structural_relation(
    Node* relation
) const {
    std::string out =
        "(";

    for (
        Node* current = relation;
        current;
        current =
            current->ref
                ? static_cast<Node*>(
                      current->ref
                  )
                : nullptr
    ) {
        out +=
            structural_member(
                current->head
            );
    }

    out += ")";

    return out;
}

// =============================================================================
// Reducer
// =============================================================================

Program Reducer::program_for(
    Ptr form
) {
    if (
        !runtime_.is_symbol(form)
    ) {
        return {};
    }

    return
        runtime_.resolve_program(
            runtime_.as_symbol(form)
        );
}

Ptr Reducer::reduce(
    Ptr form
) {
    Ptr current =
        form;

    for (
        std::size_t step = 0;
        step < max_steps_;
        ++step
    ) {
#ifdef SEED_DEFERRED_EXPERIMENT
        std::size_t extra_steps = 0;
        Ptr next =
            reduce_once(
                current, extra_steps, max_steps_ - step
            );
        step += extra_steps;
#else
        Ptr next = reduce_once(current);
#endif

        if (
            next ==
            current
        ) {
            return current;
        }

        current =
            next;
    }

    throw std::runtime_error(
        "Seed reduction exceeded step budget"
    );
}

Ptr Reducer::reduce_once(
    Ptr form
#ifdef SEED_DEFERRED_EXPERIMENT
    , std::size_t& extra_steps, std::size_t remaining_steps
#endif
) {
    SEED_COUNT(runtime_, reduction_steps);
    if (!form) {
        return nullptr;
    }

    switch (runtime_.arena().kind(form)) {
    case HostKind::Node:
#ifdef SEED_DEFERRED_EXPERIMENT
        return reduce_relation(static_cast<Node*>(form), extra_steps, remaining_steps);
#else
        return reduce_relation(static_cast<Node*>(form));
#endif
    case HostKind::Symbol:
        if (Ptr current = runtime_.binding(static_cast<Symbol*>(form))) return current;
        return form;
    default:
        return form;
    }
}

Ptr Reducer::reduce_relation(
    Node* relation
#ifdef SEED_DEFERRED_EXPERIMENT
    , std::size_t& extra_steps, std::size_t remaining_steps
#endif
) {
    Ptr binder = nullptr;
    Ptr body = nullptr;
    // Reader-lowered blocks and lists belong to their own Program: their
    // terms are statements or elements, never operators of this product.
    if (is_symbol_text(runtime_, relation->head, block_head) ||
        is_symbol_text(runtime_, relation->head, list_head)) {
        Program program = runtime_.registry().find(runtime_.as_symbol(relation->head));
        return program ? program.invoke(runtime_, relation, program.context) : relation;
    }
    AbstractionView view;
    if (abstraction_view(runtime_, {relation, nullptr}, view)) {
        // An abstraction never demands its body or resolves its binder as a
        // native program. Only normalize the reverse-facing colon spelling.
        if (!view.canonical)
            return runtime_.product({view.binder, runtime_.arena().unit(),
                                     slice_product(runtime_, view.body)});
        return relation;
    }
    if (auto query = runtime_.identity_query(relation))
        return query.invoke(runtime_, relation, query.context);
    // Orient the whole relation before any native head can perform effects.
    // In particular, binding and flow must not execute their producer twice.
    auto terms = runtime_.terms(relation);
    // An ordinary application has no orientation Program to consult. If one
    // exists, retain the full dispatch order and recheck after every callback.
    if (has_orientation(runtime_, terms)) {
        for (auto spelling : orientation_order) {
            for (Ptr term : terms) {
                if (!is_symbol_text(runtime_, term, spelling)) continue;
                Program program = runtime_.registry().find(runtime_.as_symbol(term));
                if (program) {
                    Ptr result = program.invoke(runtime_, relation, program.context);
                    if (result != relation) return result;
                }
                break;
            }
        }
    }
    Ptr expanded = runtime_.application(relation);
    if (expanded != relation) return expanded;
    if (terms.size() >= 2 && runtime_.is_relation(terms.front())) {
        Ptr consumer = terms.front();
        if (runtime_.abstraction(consumer, binder, body)) {
            // A leading string documents a body with remaining executable
            // structure. A lone string is still a constant abstraction.
            if (runtime_.is_relation(body)) {
                auto first = static_cast<Node*>(body);
                if (first->ref && runtime_.is_literal(first->head))
                    body = slice_product(runtime_, {next_term(first), nullptr});
            }
            // The argument is substituted as written, not demanded first.
#ifdef SEED_DEFERRED_EXPERIMENT
            if (runtime_.deferred_substitution()) {
                if (Ptr result = deferred_curry(runtime_, binder, body, terms,
                                                 remaining_steps, extra_steps))
                    return result;
            }
#endif
            Ptr result = runtime_.substitute(body, binder, terms[1]);
            for (std::size_t i = 2; i < terms.size(); ++i)
                result = runtime_.compose(result, terms[i]);
            return result;
        }
        Ptr reduced = runtime_.reduce(consumer);
        if (reduced != consumer) {
            for (std::size_t i = 1; i < terms.size(); ++i)
                reduced = runtime_.compose(reduced, terms[i]);
            return reduced;
        }
    }
    // Registered relation Programs get first refusal anywhere in the product.
    // Lazy foreign resolution is restricted to its consumer/head position.
    for (Ptr term : runtime_.terms(relation)) {
        if (!runtime_.is_symbol(term)) continue;
        // Operators were offered the product above, in precedence order.
        if (is_orientation_text(runtime_.as_symbol(term)->text)) continue;
        Program program = runtime_.registry().find(runtime_.as_symbol(term));
        if (!program || program.dispatch != ProgramDispatch::Ordinary) continue;
        Ptr result = program.invoke(runtime_, relation, program.context);
        if (result != relation) return result;
    }
    if (runtime_.is_symbol(relation->head) &&
        runtime_.registry().find(runtime_.as_symbol(relation->head))) return relation;
    Program program = program_for(relation->head);
    if (program) return program.invoke(runtime_, relation, program.context);
    // A shell host asks for applications of unknown programs to fail rather
    // than silently becoming inert data, wherever they are nested.
    if (runtime_.strict_heads() && runtime_.is_symbol(relation->head) &&
        !runtime_.binding(runtime_.as_symbol(relation->head)))
        throw std::runtime_error("program not found: " + runtime_.as_symbol(relation->head)->text);

    return relation;
}
Ptr Runtime::compose(Ptr consumer, Ptr producer) {
    SEED_COUNT(*this, compositions);
    auto forms = is_relation(consumer) && !is_abstraction(consumer)
        ? terms(as_node(consumer)) : std::vector<Ptr>{consumer};
    forms.push_back(producer);
    return product(std::move(forms));
}

std::vector<Ptr> Runtime::collection(Ptr value, bool unquote_atom) {
    std::vector<Ptr> out;
    if (collection_resolver_ && collection_resolver_(*this, value, out, collection_context_)) return out;
    // The structured list shape built by [ ... ] and data list.
    if (is_name(value) && is_relation(as_name(value)->target)) {
        auto fields = terms(as_node(as_name(value)->target));
        if (fields.size() == 2 && is_name(fields[0]) && as_name(fields[0])->target == arena_.intern("list") &&
            is_name(fields[1])) {
            Ptr items = as_name(fields[1])->target;
            if (is_unit(items)) return {};
            return is_relation(items) ? terms(as_node(items)) : std::vector<Ptr>{items};
        }
    }
    if (is_name(value)) {
        Ptr target = as_name(value)->target;
        if (unquote_atom || is_relation(target) || is_unit(target)) value = target;
    }
    if (is_unit(value)) return {};
    return is_relation(value) ? terms(as_node(value)) : std::vector<Ptr>{value};
}

Ptr Runtime::flow(Node* relation, std::vector<Ptr>& stages) {
    stages.clear();
    std::vector<Ptr> segment;
    bool found = false;
    for (Ptr term : terms(relation)) {
        if (!is_symbol_text(*this, term, "|")) { segment.push_back(term); continue; }
        found = true;
        if (segment.empty()) throw std::runtime_error("empty producer before |");
        stages.push_back(product(std::move(segment)));
        segment.clear();
    }
    if (!found) return relation;
    if (segment.empty()) throw std::runtime_error("empty consumer after |");
    stages.push_back(product(std::move(segment)));
    Ptr canonical = stages.front();
    for (std::size_t i = 1; i < stages.size(); ++i) canonical = compose(stages[i], canonical);
    return canonical;
}

// =============================================================================
// Runtime
// =============================================================================

#ifdef SEED_DEFERRED_EXPERIMENT
Runtime::Runtime(bool deferred_substitution)
    : deferred_substitution_(deferred_substitution), reader_(*this),
      printer_(*this), reducer_(*this) {
#else
Runtime::Runtime()
    : reader_(*this),
      printer_(*this),
      reducer_(*this) {
#endif

    define("truth", &program_truth);
    define("apply", &program_apply);
    define("demand", &program_demand);
    define("if", &program_if);
    define("&&", &program_boolean);
    define("||", &program_boolean);
    for (auto op : {"<", "<=", ">", ">=", "==", "!="}) define(op, &program_compare);
    describe("<", "A < B | < A B\nStrict ordering of two numbers or two strings. -lt names this Program.");
    describe("<=", "A <= B | <= A B\nOrdering or equal value of two numbers or two strings. -le names this Program.");
    describe(">", "A > B | > A B\nGreater than, for two numbers or two strings. -gt names this Program. "
             "Output redirection is written inside { }: { PRODUCER > \"file\" }.");
    describe(">=", "A >= B | >= A B\nGreater than or equal, for two numbers or two strings. -ge names this Program.");
    describe("==", "A == B | == A B\nEqual values: numbers by value, strings by bytes, Names by identity or "
             "structure. Different kinds are unequal, not an error. -eq names this Program.");
    describe("!=", "A != B | != A B\nNot equal: the negation of ==. -ne names this Program.");
    for (auto op : {"+", "-", "/"}) define(op, &program_arithmetic);
    define(">>", &program_append_outside_braces);
    describe(">>", "{ PRODUCER >> \"file\" }\nAppend output to a file. Only meaningful inside { }.");
    describe("+", "A + B | + A B\nAdd two numbers, or join two strings. Integers stay exact (overflow fails); "
             "any real operand gives a real. Write the operator with spaces: (3 + 3).");
    describe("-", "A - B | - A\nSubtract, or negate one number. Attached -3 is a number and -foo a Name, "
             "so (3 - 3) is 0 while (3 -3) is the product 3 -3.");
    describe("/", "A / B\nDivide. Exact integer division stays an integer; otherwise the quotient is real. "
             "Dividing by zero fails.");
    define("=>", &program_redux);
    describe("=>", "EXPR => | => EXPR\nOne reduction step: orient |, normalize :, unfold a binding, substitute an "
             "argument into an abstraction, or compute an operator on values. Answers the next form reified, "
             "without evaluating further. Stops before an effect (a Program, process, pipeline, binding or "
             "block) and names it; demand with ? to run it. EXPR => => takes two steps.");
    define(std::string(block_head), &program_block);
    define(std::string(list_head), &program_list);
    describe(std::string(list_head), "[ELEMENT ...]\nA list: each element is evaluated once, like data list. [] is "
             "the empty list.");
    define("for", &program_for);
    define(":=", &program_bind);
    define("|", &program_flow);
    describe("|", "PRODUCER | CONSUMER\nReverse application: b | a == a b == a(b). The relation is "
             "oriented and reduced as ordinary Seed application; a native consumer receives the "
             "producer's completed answer. Live OS pipes are written explicitly as { b | a }.");

    define(
        "=",
        &program_bind
    );

    define(
        ":",
        &program_colon
    );

    define(
        "@",
        &program_reference
    );

    // Prefix * reads a cell; infix A * B multiplies.
    define(
        "*",
        &program_arithmetic
    );
    describe("*", "* NAME | A * B\nPrefix: read the binding cell's current head. Infix with spaces: multiply two numbers.");
}

bool Runtime::is_block(Ptr form) const {
    return is_relation(form) && is_symbol_text(*this, as_node(form)->head, block_head);
}

bool Runtime::is_list_form(Ptr form) const {
    return is_relation(form) && is_symbol_text(*this, as_node(form)->head, list_head);
}

Ptr Runtime::demand(Ptr form) {
    return demand_program_form(*this, form);
}

void Runtime::define(
    std::string_view symbol,
    ProgramFn invoke,
    Ptr context
) {
    define(
        symbol,
        Program{
            invoke,
            context
        }
    );
}

void Runtime::describe(std::string_view symbol, std::string text) {
    registry_.describe(intern(symbol), std::move(text));
}

Ptr Runtime::pipeline_body(Ptr form) const {
    if (!is_relation(form)) return nullptr;
    auto first = as_node(form);
    if (!is_symbol_text(*this, first->head, "{}")) return nullptr;
    auto body = next_term(first);
    if (!body || body->ref) return nullptr;
    return is_relation(body->head) || is_unit(body->head) ? body->head : nullptr;
}

void Runtime::define(
    std::string_view symbol,
    Program program
) {
    registry_.define(
        intern(symbol),
        program
    );
}

Program Runtime::identity_query(Ptr form) const {
    if (!is_relation(form) || is_block(form) || is_list_form(form) || pipeline_body(form)) return {};
    auto first = as_node(form);
    auto lookup = [&](Ptr identity) {
        if (!is_symbol(identity)) return Program{};
        auto program = registry_.find(as_symbol(identity));
        return program.dispatch == ProgramDispatch::IdentityQuery ? program : Program{};
    };
    if (auto prefix = lookup(first->head)) {
        auto argument = next_term(first);
        auto route = argument ? next_term(argument) : nullptr;
        // Routing a query's answer is ordinary composition. Only a complete
        // one-identity request can leave its routing to the enclosing product.
        if (route && (is_symbol(argument->head) || is_cell(argument->head) ||
            (is_name(argument->head) && is_symbol(as_name(argument->head)->target))) &&
            (is_symbol_text(*this, route->head, "|") ||
             is_symbol_text(*this, route->head, ">") || is_symbol_text(*this, route->head, ">>")))
            return {};
        return prefix;
    }
    auto last = first;
    while (last->ref) last = next_term(last);
    return last != first ? lookup(last->head) : Program{};
}

Ptr Runtime::read(
    std::string_view source
) {
    return
        reader_.read(
            source
        );
}

Ptr Runtime::reduce(
    Ptr form
) {
    // Host safety boundary, not a reduction rule: refuse one more nested
    // reduction when the native stack is nearly exhausted, instead of letting
    // uncontrolled recursion crash the process. Terminating reductions within
    // the stack reservation are unaffected.
    if (native_stack_low(256 * 1024))
        throw std::runtime_error("Seed evaluation nested too deeply for the native stack (reduction depth " +
                                 std::to_string(reduction_depth_) + "); repeat with while or for instead of recursion");
    ++reduction_depth_;
    struct Depth { unsigned& value; ~Depth() { --value; } } scope{reduction_depth_};
    return
        reducer_.reduce(
            form
        );
}

Ptr Runtime::run(
    std::string_view source
) {
    return
        reduce(
            read(source)
        );
}

bool Runtime::truth(Ptr value) const {
    if (!value || is_unit(value)) return false;
    if (is_number(value)) {
        auto n = as_number(value);
        return arena_.is(value, HostKind::Integer) ? n->integer != 0 : n->real != 0;
    }
    if (is_literal(value)) {
        const auto& bytes = as_literal(value)->bytes;
        return !bytes.empty() && bytes != "0" && bytes != "false";
    }
    if (is_name(value)) value = as_name(value)->target;
    if (is_symbol(value)) return as_symbol(value)->text != "false";
    return !is_unit(value);
}

void Runtime::set_resolver(
    ResolverFn resolver,
    Ptr context
) {
    resolver_ =
        resolver;

    resolver_context_ =
        context;
}

Program Runtime::resolve_program(
    Symbol* symbol
) {
    if (!symbol) {
        return {};
    }

    // Fast path:
    // already known to Seed.
    if (
        Program program =
            registry_.find(symbol)
    ) {
        return program;
    }

    // No host resolver installed.
    if (!resolver_) {
        return {};
    }

    // Ask Hirari whether this Symbol corresponds to
    // some foreign/native capability.
    Program program =
        resolver_(
            *this,
            symbol,
            resolver_context_
        );

    if (!program) {
        return {};
    }

    return program;
}

std::string Runtime::print(
    Ptr form
) const {
    return
        printer_.native(
            form
        );
}

std::string Runtime::structural(
    Ptr form
) const {
    return
        printer_.structural(
            form
        );
}

std::vector<Ptr> Runtime::terms(
    Node* relation
) const {
    SEED_COUNT(*this, term_walks);
    std::vector<Ptr> result;

    for (
        Node* current = relation;
        current;
        current =
            current->ref
                ? static_cast<Node*>(
                      current->ref
                  )
                : nullptr
    ) {
        SEED_COUNT(*this, terms_copied);
        result.push_back(
            current->head
        );
    }

    return result;
}

Ptr Runtime::product(
    std::vector<Ptr> forms
) {
    if (forms.empty()) {
        return
            arena_.unit();
    }

    if (
        forms.size() == 1
    ) {
        return
            forms.front();
    }

    return
        arena_.make_relation(
            forms
        );
}

Cell* Runtime::ensure_cell(
    Symbol* symbol
) {
    if (!symbol || symbol->text == "_") {
        throw std::invalid_argument(
            "anonymous _ has no binding cell"
        );
    }

    if (
        auto it =
            cells_.find(symbol);
        it != cells_.end()
    ) {
        return it->second;
    }

    Cell* cell =
        arena_.make_cell(
            symbol
        );

    cells_.emplace(
        symbol,
        cell
    );

    return cell;
}

Cell* Runtime::find_cell(
    Symbol* symbol
) const {
    if (!symbol) {
        return nullptr;
    }

    const auto it =
        cells_.find(symbol);

    return
        it == cells_.end()
            ? nullptr
            : it->second;
}

void Runtime::bind(
    Symbol* symbol,
    Ptr value
) {
    auto metadata = std::make_shared<BindingMetadata>();
    metadata->origin = binding_origin_;
    metadata->source_file = binding_source_;
    if (!value) metadata->kind = "unbound";
    else if (is_relation(value)) {
        metadata->kind = "program";
        RelationSlice current{as_node(value), nullptr};
        std::vector<RelationSlice> seen;
        for (unsigned depth = 0; depth < 64; ++depth) {
            if (std::any_of(seen.begin(), seen.end(), [&](RelationSlice old) {
                return old.begin == current.begin && old.end == current.end;
            })) break;
            seen.push_back(current);
            AbstractionView view;
            if (!abstraction_view(*this, current, view)) {
                auto first = current.begin->head;
                if (is_symbol(first)) metadata->provider = as_symbol(first)->text;
                break;
            }
            metadata->kind = "abstraction";
            metadata->parameters.push_back(is_wildcard(view.binder) ? "_" : as_symbol(view.binder)->text);
            auto body = view.body;
            Ptr first = body.begin->head;
            AbstractionView nested;
            if (next_term(body.begin) != body.end && abstraction_view(*this, body, nested)) {
                current = body; continue;
            }
            if (next_term(body.begin) == body.end && is_relation(first)) {
                if (is_abstraction(first)) { current = {as_node(first), nullptr}; continue; }
                body = {as_node(first), nullptr};
                first = body.begin->head;
            }
            if (is_literal(first)) {
                if (metadata->docstring.empty()) metadata->docstring = as_literal(first)->bytes;
                body.begin = next_term(body.begin);
            }
            if (body.begin == body.end) break;
            if (next_term(body.begin) != body.end) {
                if (is_symbol(body.begin->head)) metadata->provider = as_symbol(body.begin->head)->text;
                break;
            }
            Ptr remaining = body.begin->head;
            if (!is_relation(remaining)) {
                if (is_symbol(remaining)) metadata->provider = as_symbol(remaining)->text;
                break;
            }
            current = {as_node(remaining), nullptr};
        }
    } else if (is_symbol(value)) {
        metadata->kind = "alias";
        metadata->alias = as_symbol(value)->text;
    } else metadata->kind = "value";
    if (std::find(metadata->parameters.begin(), metadata->parameters.end(), metadata->provider) != metadata->parameters.end())
        metadata->provider.clear();
    Cell* cell =
        ensure_cell(
            symbol
        );

    cell->head =
        value;

    ++cell->revision;
    cell->metadata = std::move(metadata);
}

std::vector<Cell> Runtime::capture_bindings() const {
    std::vector<Cell> saved;
    saved.reserve(cells_.size());
    for (const auto& [symbol, cell] : cells_) saved.push_back(*cell);
    return saved;
}

Ptr Runtime::application(Ptr form) {
    SEED_COUNT(*this, applications);
    if (!is_relation(form)) return form;
    if (is_abstraction(form)) return form;
    if (identity_query(form)) return form;
    auto relation = static_cast<Node*>(form);
    if (!is_symbol(relation->head)) return form;
    auto consumer = static_cast<Symbol*>(relation->head);
    if (registry().find(consumer)) return form;
    auto candidate = binding(consumer);
    if (!candidate || (!is_symbol(candidate) && !is_relation(candidate))) return form;
    // Allocate a flattened invocation only when there is a callable binding.
    auto forms = terms(relation);
    if (has_orientation(*this, forms)) return form;
    std::vector<Symbol*> seen;
    bool changed = false;
    while (!forms.empty() && is_symbol(forms.front())) {
        auto symbol = as_symbol(forms.front());
        // Core/host programs retain their explicit dispatch contracts.
        if (registry().find(symbol)) break;
        auto retained = binding(symbol);
        if (!retained || (!is_symbol(retained) && !is_relation(retained))) break;
        if (std::find(seen.begin(), seen.end(), symbol) != seen.end() || seen.size() >= 64)
            throw std::runtime_error("cyclic callable binding: " + symbol->text);
        seen.push_back(symbol);
        auto head = is_relation(retained) && !is_abstraction(retained)
            ? terms(as_node(retained)) : std::vector<Ptr>{retained};
        if (head.empty()) break;
        head.insert(head.end(), forms.begin() + 1, forms.end());
        forms = std::move(head);
        changed = true;
    }
    return changed ? product(std::move(forms)) : form;
}

bool Runtime::is_abstraction(Ptr form) const {
    if (!is_relation(form)) return false;
    AbstractionView view;
    return abstraction_view(*this, {static_cast<Node*>(form), nullptr}, view);
}

bool Runtime::abstraction(Ptr form, Ptr& binder, Ptr& body) {
    SEED_COUNT(*this, body_extractions);
    if (!is_relation(form)) return false;
    AbstractionView view;
    if (!abstraction_view(*this, {static_cast<Node*>(form), nullptr}, view)) return false;
    binder = view.binder;
    body = slice_product(*this, view.body);
    return true;
}

Ptr Runtime::substitute(Ptr body, Ptr binder, Ptr replacement) {
    SEED_COUNT(*this, substitution_visits);
    if (is_wildcard(binder)) return body; // Anonymous positions are never holes.
    if (body == binder) return replacement;
    if (!is_relation(body)) return body;

    auto result = substitute_product(*this, {static_cast<Node*>(body), nullptr}, binder, replacement);
    return result.changed ? result.value : body;
}

bool Runtime::is_wildcard(Ptr p) const {
    return arena_.is(p, HostKind::Wildcard);
}

bool Runtime::is_number(Ptr p) const {
    return arena_.is(p, HostKind::Integer) || arena_.is(p, HostKind::Real);
}

Number* Runtime::as_number(Ptr p) const {
    if (!is_number(p)) throw std::invalid_argument("not a numeric literal");
    return static_cast<Number*>(p);
}

void Runtime::restore_bindings(const std::vector<Cell>& saved) {
    for (const auto& [symbol, cell] : cells_) { cell->head = nullptr; cell->metadata.reset(); }
    for (const auto& state : saved) *ensure_cell(state.symbol) = state;
}

Ptr Runtime::binding(
    Symbol* symbol
) const {
    Cell* cell =
        find_cell(
            symbol
        );

    if (!cell) {
        return nullptr;
    }

    return cell->head;
}

Ptr Runtime::head(
    Cell* cell
) const {
    if (!cell) {
        return nullptr;
    }

    return cell->head;
}

bool Runtime::is_relation(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Node
        );
}

bool Runtime::is_unit(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Unit
        );
}

bool Runtime::is_symbol(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Symbol
        );
}

bool Runtime::is_name(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Name
        );
}

bool Runtime::is_literal(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Literal
        );
}

bool Runtime::is_cell(
    Ptr p
) const {
    return
        arena_.is(
            p,
            HostKind::Cell
        );
}

Symbol* Runtime::as_symbol(
    Ptr p
) const {
    if (
        !is_symbol(p)
    ) {
        throw std::invalid_argument(
            "not a Symbol"
        );
    }

    return
        static_cast<Symbol*>(p);
}

Name* Runtime::as_name(
    Ptr p
) const {
    if (
        !is_name(p)
    ) {
        throw std::invalid_argument(
            "not a Name"
        );
    }

    return
        static_cast<Name*>(p);
}

Literal* Runtime::as_literal(
    Ptr p
) const {
    if (
        !is_literal(p)
    ) {
        throw std::invalid_argument(
            "not a Literal"
        );
    }

    return
        static_cast<Literal*>(p);
}

Cell* Runtime::as_cell(
    Ptr p
) const {
    if (
        !is_cell(p)
    ) {
        throw std::invalid_argument(
            "not a Cell"
        );
    }

    return
        static_cast<Cell*>(p);
}

Node* Runtime::as_node(
    Ptr p
) const {
    if (
        !is_relation(p)
    ) {
        throw std::invalid_argument(
            "not a relation Node"
        );
    }

    return
        static_cast<Node*>(p);
}

} // namespace seed
