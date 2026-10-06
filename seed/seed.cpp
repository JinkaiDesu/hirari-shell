#include "seed/seed.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace seed {

namespace {

// Both the reader and continuation scanner use the same comment rules.
bool skip_trivia(std::string_view source, std::size_t& pos) {
    while (pos < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[pos]))) { ++pos; continue; }
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
        if (depth) return false;
    }
    return true;
}

std::string name_spelling(Symbol* symbol) {
    // Attached ':=' now belongs to binding syntax, so quote operator identities
    // structurally rather than printing a different value on the next read.
    if (symbol->text == "=" || symbol->text == ":=") return ":(" + symbol->text + ")";
    return ":" + symbol->text;
}

// =============================================================================
// Bootstrap programs
// =============================================================================

bool is_symbol_text(
    Runtime& runtime,
    Ptr form,
    std::string_view text
) {
    return
        runtime.is_symbol(form) &&
        runtime.as_symbol(form)->text ==
            text;
}

Symbol* symbol_from_identity(
    Runtime& runtime,
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

// -----------------------------------------------------------------------------
// ?
// -----------------------------------------------------------------------------
//
// ? camera
//
// means:
//
// demand/reduce camera.
//
// camera ?
//
// is structurally different and is therefore left untouched.
//

Ptr program_query(
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
            "?"
        )
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

    return runtime.reduce(
        target
    );
}

// -----------------------------------------------------------------------------
// =
// -----------------------------------------------------------------------------
//
// camera := unity :Camera.main  <=>  unity :Camera.main = camera
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
    auto terms =
        runtime.terms(relation);

    if (
        terms.size() != 3 ||
        !is_symbol_text(
            runtime,
            terms[1],
            ":"
        )
    ) {
        return relation;
    }

    return runtime.product({
        terms[2],
        runtime.arena().unit(),
        terms[0]
    });
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
// for :item :("a" "b") (consumer item)
// The iteration binding is restored, including when a body throws.
Ptr demand_program_form(Runtime& runtime, Ptr form) {
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

Ptr program_for(Runtime& runtime, Node* relation, Ptr) {
    auto terms = runtime.terms(relation);
    if (!is_symbol_text(runtime, terms.front(), "for")) return relation;
    if (terms.size() != 4)
        throw std::runtime_error("for expects a binding identity, finite domain, and body");
    Symbol* variable = symbol_from_identity(runtime, terms[1]);
    if (!variable) throw std::runtime_error("for binding must be a Symbol or Name");
    Ptr domain = runtime.reduce(terms[2]);
    if (runtime.is_name(domain)) {
        Ptr target = runtime.as_name(domain)->target;
        if (runtime.is_relation(target) || runtime.is_unit(target)) domain = target;
    }
    std::vector<Ptr> elements;
    if (runtime.is_relation(domain)) elements = runtime.terms(runtime.as_node(domain));
    else if (!runtime.is_unit(domain)) elements.push_back(domain);
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
    if (!symbol) {
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
    if (!skip_trivia(source_, pos_))
        throw std::runtime_error("unterminated bracket comment");
}

bool Reader::source_complete(std::string_view source) {
    int depth = 0;
    std::size_t pos = 0;
    while (pos < source.size()) {
        if (!skip_trivia(source, pos)) return false;
        if (pos == source.size()) break;
        char c = source[pos++];
        if (c == '"') {
            bool closed = false;
            while (pos < source.size()) {
                c = source[pos++];
                if (c == '\\') {
                    if (pos == source.size()) return false;
                    ++pos;
                } else if (c == '"') { closed = true; break; }
            }
            if (!closed) return false;
        } else if (c == '(') ++depth;
        else if (c == ')' && --depth < 0) return true; // Reader diagnoses invalid input.
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

    if (peek() == '(') {
        return read_group();
    }

    if (peek() == ':') {
        if (peek(1) == '=') {
            take(); take();
            return runtime_.intern(":=");
        }
        return read_colon_form();
    }

    if (peek() == '"') {
        return read_literal();
    }

    return read_symbol();
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

Ptr Reader::read_colon_form() {
    take();

    // Detached colon:
    //
    //     a : b
    //
    // is Symbol(":").
    if (
        eof() ||
        std::isspace(
            static_cast<unsigned char>(
                peek()
            )
        ) ||
        peek() == ')' || peek() == '#'
    ) {
        return
            runtime_.arena()
                .intern(":");
    }

    // ::
    //
    // Name of Symbol(":").
    if (peek() == ':') {
        take();

        return
            runtime_.arena()
                .make_name(
                    runtime_.arena()
                        .intern(":")
                );
    }

    // :Form
    return
        runtime_.arena()
            .make_name(
                read_form()
            );
}

Ptr Reader::read_literal() {
    take();

    std::string bytes;

    while (!eof()) {
        const char c =
            take();

        if (c == '"') {
            return
                runtime_.arena()
                    .make_literal(
                        bytes
                    );
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

    while (!eof()) {
        const char c =
            peek();

        if (
            std::isspace(
                static_cast<unsigned char>(c)
            ) ||
            c == '(' ||
            c == ')' ||
            c == '"' ||
            c == ':' || c == '#'
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

    return
        runtime_.arena()
            .intern(token);
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
                return ":()";
            }

            return ":" +
                structural_form(
                    target
                );
        }

        case HostKind::Cell: {
            Cell* cell =
                static_cast<Cell*>(form);

            if (!cell->symbol) {
                return "@ :()";
            }

            return
                "@ " + name_spelling(cell->symbol);
        }

        case HostKind::Node:
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

        if (
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
                return "(:())";
            }

            return "(:" +
                structural_form(
                    target
                ) +
                ")";
        }

        case HostKind::Cell: {
            Cell* cell =
                static_cast<Cell*>(form);

            if (!cell->symbol) {
                return "((@)(:()))";
            }

            return
                "((@)(" + name_spelling(cell->symbol) + "))";
        }

        case HostKind::Node:
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
            return "((@)(:()))";
        }

        return
            "((@)(:" +
            cell->symbol->text +
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
        Ptr next =
            reduce_once(
                current
            );

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
) {
    if (!form) {
        return nullptr;
    }

    if (
        runtime_.is_name(form) ||
        runtime_.is_literal(form) ||
        runtime_.is_unit(form) ||
        runtime_.is_cell(form)
    ) {
        return form;
    }

    if (
        runtime_.is_symbol(form)
    ) {
        Symbol* symbol =
            runtime_.as_symbol(
                form
            );

        if (
            Ptr current =
                runtime_.binding(
                    symbol
                )
        ) {
            return current;
        }

        return form;
    }

    return
        reduce_relation(
            runtime_.as_node(
                form
            )
        );
}

Ptr Reducer::reduce_relation(
    Node* relation
) {
    // Orient the whole relation before any native head can perform effects.
    // In particular, binding and flow must not execute their producer twice.
    auto terms = runtime_.terms(relation);
    for (auto spelling : {":=", "=", "|", ":"}) {
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
    // Registered relation Programs get first refusal anywhere in the product.
    // Lazy foreign resolution is restricted to its consumer/head position.
    for (Ptr term : runtime_.terms(relation)) {
        if (!runtime_.is_symbol(term)) continue;
        auto spelling = runtime_.as_symbol(term)->text;
        if (spelling == ":=" || spelling == "=" || spelling == "|" || spelling == ":") continue;
        Program program = runtime_.registry().find(runtime_.as_symbol(term));
        if (!program) continue;
        Ptr result = program.invoke(runtime_, relation, program.context);
        if (result != relation) return result;
    }
    if (runtime_.is_symbol(relation->head) &&
        runtime_.registry().find(runtime_.as_symbol(relation->head))) return relation;
    Program program = program_for(relation->head);
    if (program) return program.invoke(runtime_, relation, program.context);

    return relation;
}
Ptr Runtime::compose(Ptr consumer, Ptr producer) {
    auto forms = is_relation(consumer) ? terms(as_node(consumer)) : std::vector<Ptr>{consumer};
    forms.push_back(producer);
    return product(std::move(forms));
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

Runtime::Runtime()
    : reader_(*this),
      printer_(*this),
      reducer_(*this) {

    define("truth", &program_truth);
    define("if", &program_if);
    define("for", &program_for);
    define(":=", &program_bind);
    define("|", &program_flow);

    define(
        "?",
        &program_query
    );

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

    define(
        "*",
        &program_head
    );
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

void Runtime::define(
    std::string_view symbol,
    Program program
) {
    registry_.define(
        intern(symbol),
        program
    );
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
    if (!symbol) {
        throw std::invalid_argument(
            "cannot create cell for null symbol"
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
    Cell* cell =
        ensure_cell(
            symbol
        );

    cell->head =
        value;

    ++cell->revision;
}

std::vector<Cell> Runtime::capture_bindings() const {
    std::vector<Cell> saved;
    saved.reserve(cells_.size());
    for (const auto& [symbol, cell] : cells_) saved.push_back(*cell);
    return saved;
}

void Runtime::restore_bindings(const std::vector<Cell>& saved) {
    for (const auto& [symbol, cell] : cells_) cell->head = nullptr;
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
