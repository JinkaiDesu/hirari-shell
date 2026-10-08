#include "hirari/shell.hpp"
#include "hirari/win32.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Trace { std::string events; seed::Ptr identity = nullptr; };
seed::Ptr emit(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    auto& trace = *static_cast<Trace*>(context);
    trace.events += "emit:" + runtime.structural(relation) + "\n";
    return trace.identity;
}
seed::Ptr twice(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    auto terms = runtime.terms(relation);
    if (terms.size() != 3) return relation;
    auto& trace = *static_cast<Trace*>(context);
    auto first = runtime.reduce(terms[1]);
    auto second = runtime.reduce(terms[2]);
    require(first == trace.identity && second == trace.identity, "opaque identity changed");
    trace.events += "twice\n";
    return trace.identity;
}
seed::Ptr inspect(seed::Runtime& runtime, seed::Node* relation, seed::Ptr context) {
    static_cast<Trace*>(context)->events += runtime.structural(relation) + "\n";
    return relation;
}
std::string outcome(seed::Runtime& runtime, std::string_view source) {
    try { return "value:" + runtime.structural(runtime.run(source)); }
    catch (const std::exception& error) { return "error:" + std::string(error.what()); }
}

std::size_t comparisons = 0;
std::uint64_t chains = 0;
void compare(const std::string& source, bool inspection = false) {
    seed::Runtime literal(false), deferred(true);
    Trace left{"", literal.read("-opaque")};
    Trace right{"", deferred.read("-opaque")};
    literal.define("emit", emit, &left);
    deferred.define("emit", emit, &right);
    literal.define("twice", twice, &left);
    deferred.define("twice", twice, &right);
    literal.bind(literal.intern("effect"), literal.read("emit -input"));
    deferred.bind(deferred.intern("effect"), deferred.read("emit -input"));
    if (inspection) {
        literal.define("help", inspect, &left);
        deferred.define("help", inspect, &right);
    }
    const auto expected = outcome(literal, source);
    const auto actual = outcome(deferred, source);
    require(actual == expected, source + "\n" + expected + "\n" + actual);
    require(left.events == right.events, "effect/inspection order changed: " + source);
    chains += deferred.deferred_stats().chains;
    // Merely registering help no longer installs an ambient observer.
    ++comparisons;
}

void generated_cases() {
    constexpr std::array binders{"x", "y", "z"};
    constexpr std::array arguments{"x", "y", "z", "3", "0", "-opaque", "()",
                                   "\"doc\"", "(emit -input)", "(x() x)", "(<)"};
    constexpr std::array bodies{"x", "y", "z", "(x y)", "(y x)", "(x() x)",
        "(y() x)", "(z() (x y))", "-(x y)", "(3 x 5)", "(emit x)",
        "(twice x x)", "(\"doc\" x y)", "(x () y)", "(x : y)"};
    for (auto outer : binders) for (auto inner : binders)
        for (auto body : bodies) for (auto first : arguments) for (auto second : arguments)
            compare("(" + std::string(outer) + "() (" + inner + "() " + body +
                    ")) " + first + " " + second);
    // Three chronological frames expose capture and non-retroactive rewriting.
    for (auto a : binders) for (auto b : binders) for (auto c : binders)
        for (auto x : binders) for (auto y : binders) for (auto z : binders)
            for (auto body : {"x", "y", "z", "(x y z)", "(y() x)"})
                compare("(" + std::string(a) + "() (" + b + "() (" + c + "() " +
                        body + "))) " + x + " " + y + " " + z);
}

void boundaries() {
    for (const auto& source : {
        "(x() (y() x)) y 3", // Earlier incoming y is intentionally captured.
        "(x() (y() y)) z x", // Later x is not rewritten retroactively to z.
        "(x() (x() x)) y 3", // Original same-identity shadowing.
        "(x() (y() (twice x x))) (emit -input) 3",
        "(x() (y() (twice x x))) effect 3",
        "(x() (y() -done)) effect 3",
        "(x() (y() -done)) (emit -input) 3",
        "(x() (y() (emit x))) -input 3",
        "(x() (y() (emit x))) -input 3 4",
        "(x() (y() (\"doc\" x))) y 3",
        "(x() (\"outer doc\" (y() x))) y 3",
        "(op() (3 op 5)) (<)",
        "(x() (x x)) (x() (x x))"}) {
        compare(source);
        compare(source, true);
    }
    for (bool enabled : {false, true}) {
        seed::Runtime runtime(enabled);
        runtime.run("free := 3");
        auto invocation = runtime.read("(x() (y() free)) 1 2");
        require(runtime.print(runtime.reduce(invocation)) == "3", "initial live binding");
        runtime.run("free := 7");
        require(runtime.print(runtime.reduce(invocation)) == "7", "free binding was frozen");
        auto template_form = runtime.read("x() (y() x)");
        auto partial_a = runtime.reduce(runtime.compose(template_form, runtime.read("3")));
        auto partial_b = runtime.reduce(runtime.compose(template_form, runtime.read("7")));
        auto original_inner = runtime.as_node(runtime.terms(runtime.as_node(template_form))[2]);
        static_cast<seed::Node*>(static_cast<seed::Node*>(original_inner->ref)->ref)->head = runtime.read("9");
        require(runtime.print(runtime.reduce(runtime.compose(partial_a, runtime.read("0")))) == "3",
                "escaped partial changed with original template");
        require(runtime.print(runtime.reduce(runtime.compose(partial_b, runtime.read("0")))) == "7",
                "partial environments shared state");
        require(runtime.print(runtime.reduce(runtime.product({template_form, runtime.read("1"), runtime.read("2")}))) == "9",
                "direct host mutation was hidden");
        runtime.run("live := x() (y() free)");
        auto retained = runtime.read("live 1 2");
        require(runtime.print(runtime.reduce(retained)) == "7", "initial callable");
        runtime.run("live := x() (y() 9)");
        require(runtime.print(runtime.reduce(retained)) == "9", "callable binding was frozen");
        // Same budget edge with and without fusion: no extra chance to converge.
        for (int depth : {4093, 4094}) {
            for (int i = 0; i < depth; ++i)
                runtime.bind(runtime.intern("budget" + std::to_string(i)),
                    i + 1 == depth ? runtime.read("3") : runtime.intern("budget" + std::to_string(i + 1)));
            try {
                auto value = runtime.run("(x() (y() budget0)) 0 0");
                require(depth == 4093 && runtime.print(value) == "3", "budget extended");
            } catch (const std::exception& error) {
                require(depth == 4094 && std::string(error.what()) == "Seed reduction exceeded step budget",
                        "budget error changed");
            }
        }
        require(runtime.print(runtime.run("(x() x) 3")) == "3", "budget recovery failed");
    }
}

struct Host : hirari::Host {
    std::string errors;
    void write(std::string_view) override {}
    void error(std::string_view text) override { errors += text; }
};
void real_shell() {
    SetEnvironmentVariableW(L"HIRARI_STATE", L"-");
    SetEnvironmentVariableW(L"HIRARI_PATH", L"");
    Host host;
    hirari::Shell shell(host);
    auto& runtime = shell.seed();
    shell.execute("canonical := f() (g() (x() (f (g x))))");
    shell.execute("identity := x() x");
    runtime.deferred_stats() = {};
    for (int i = 0; i < 100; ++i)
        require(runtime.print(shell.execute("canonical identity identity 3")) == "3", "real shell result");
    const auto stats = runtime.deferred_stats();
    require(host.errors.empty() && stats.chains == 100 && stats.inspection_barriers == 0,
            "explicit help still blocked unrelated curry fusion");
    auto cell = runtime.find_cell(runtime.intern("canonical"));
    auto revision = cell->revision;
    auto metadata = cell->metadata;
    auto query = runtime.read("help -canonical");
    auto nodes = runtime.arena().node_count();
    const auto result = runtime.reduce(query);
    require(runtime.is_literal(result) && runtime.arena().node_count() == nodes &&
            cell->revision == revision && cell->metadata == metadata,
            "metadata help materialized or changed the callable");
    std::cout << "Real Hirari: attempts=" << stats.attempts << ", chains=" << stats.chains
              << ", inspection_barriers=" << stats.inspection_barriers << '\n';
}
}

int main() {
    try {
        generated_cases();
        boundaries();
        real_shell();
        require(chains > 0, "differential suite never exercised deferred substitution");
        std::cout << "Differential comparisons=" << comparisons << ", fused chains=" << chains
                  << "; effects, opacity, live bindings, mutable templates and budgets passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
