#include "seed/seed.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void expect_native(seed::Runtime& rt,
                   std::string_view source,
                   std::string_view expected) {
    seed::Ptr form = rt.read(source);
    const std::string actual = rt.print(form);

    if (actual != expected) {
        std::cerr << "SOURCE: " << source
                  << "\nEXPECTED: " << expected
                  << "\nACTUAL: " << actual << '\n';
        throw std::runtime_error("native print mismatch");
    }
}

void expect_structural(seed::Runtime& rt,
                       std::string_view source,
                       std::string_view expected) {
    seed::Ptr form = rt.read(source);
    const std::string actual = rt.structural(form);

    if (actual != expected) {
        std::cerr << "SOURCE: " << source
                  << "\nEXPECTED: " << expected
                  << "\nACTUAL: " << actual << '\n';
        throw std::runtime_error("structural print mismatch");
    }
}

seed::Ptr capture(seed::Runtime&, seed::Node* relation, seed::Ptr) {
    return relation;
}

template<class F> void rejects(F fn, const char* message) {
    try { fn(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

struct Effects { std::string trace; seed::Ptr value; };
seed::Ptr source(seed::Runtime& rt, seed::Node* relation, seed::Ptr context) {
    auto terms = rt.terms(relation);
    require(terms.size() == 2 && rt.is_name(terms[1]), "producer ran before orientation");
    auto& effects = *static_cast<Effects*>(context);
    effects.trace += 'P';
    return effects.value;
}
seed::Ptr consume(seed::Runtime& rt, seed::Node* relation, seed::Ptr context) {
    auto terms = rt.terms(relation);
    if (terms.size() != 2) return relation; // Captured products remain distinct.
    auto& effects = *static_cast<Effects*>(context);
    auto value = rt.reduce(terms[1]);
    require(value == effects.value, "composition lost opaque identity");
    effects.trace += 'C';
    return value;
}
seed::Ptr matches(seed::Runtime& rt, seed::Node* relation, seed::Ptr) {
    auto terms = rt.terms(relation);
    require(terms.size() == 2 && rt.is_name(terms[1]), "query provider request shape");
    auto role = rt.as_name(terms[1])->target;
    std::vector<seed::Ptr> result;
    if (role == rt.intern("nearby")) result = {rt.arena().make_name(rt.intern("one")), rt.arena().make_name(rt.intern("two"))};
    return rt.arena().make_name(rt.product(std::move(result)));
}

void semantics() {
    seed::Runtime rt;
    int payload = 42;
    Effects effects{"", rt.arena().make_name(&payload)};
    rt.define("source", &source, &effects);
    rt.define("consume", &consume, &effects);
    for (auto spelling : {"consume (source :input)", "source :input | consume", "consume(source :input)"}) {
        effects.trace.clear();
        require(rt.run(spelling) == effects.value && effects.trace == "PC", "application/flow effects differ");
    }
    effects.trace.clear();
    require(rt.run("source :input | consume | consume") == effects.value && effects.trace == "PCC", "flow chain order changed");
    effects.trace.clear();
    require(rt.print(rt.run("if :false (source :input | consume) \"idle\"")) == "\"idle\"" && effects.trace.empty(), "orientation eagerly executed an unchosen branch");
    require(rt.structural(rt.run("a b")) == rt.structural(rt.run("b | a")), "product mirror mismatch");
    require(rt.structural(rt.run("a b")) == rt.structural(rt.run("a(b)")), "call product mismatch");
    require(rt.structural(rt.run("a () b")) == rt.structural(rt.run("b : a")), "capture mirror mismatch");
    require(rt.structural(rt.run("a : b")) != rt.structural(rt.run("a :b")), "detached colon became Name application");
    require(rt.structural(rt.run("a () b")) != rt.structural(rt.run("a b")), "Unit was discarded");
    auto captured = rt.run("consume () (source :input)");
    require(rt.is_relation(captured), "capture became ordinary application");

    auto cell = rt.run("@ :x");
    effects.trace.clear();
    require(rt.run("x := source :input") == effects.value && effects.trace == "P", "forward binding effects");
    auto head = rt.binding(rt.intern("x"));
    require(rt.is_relation(head) && rt.run("@ x") == cell, "binding lost lazy form or stable cell");
    auto revision = rt.as_cell(cell)->revision;
    effects.trace.clear();
    require(rt.run("source :input = x") == effects.value && effects.trace == "P", "reverse binding effects");
    require(rt.as_cell(cell)->revision == revision + 1 && rt.run("@ x") == cell, "inverse binding cell mismatch");
    require(rt.structural(head) == rt.structural(rt.binding(rt.intern("x"))), "inverse binding expression changed");
    effects.trace.clear();
    require(rt.run("x") == effects.value && rt.run("* x") == effects.value && effects.trace == "PP", "binding ceased to be lazy");
    rt.run("\"hello\" = greeting");
    require(rt.print(rt.run("greeting")) == "\"hello\"", "reverse value binding failed");
    rt.run("compact:=\"yes\"");
    require(rt.print(rt.run("compact")) == "\"yes\"", "walrus lexer precedence failed");
    rejects([&]{rt.run("x :=");}, "empty binding accepted");
    rejects([&]{rt.run("x := y = z");}, "ambiguous chained binding accepted");
    rejects([&]{rt.run("\"bad\" := value");}, "literal binding identity accepted");
    rejects([&]{rt.run("| consume");}, "empty producer accepted");
    rejects([&]{rt.run("source | ");}, "empty consumer accepted");

    rt.define("matches", &matches);
    auto answer = rt.run("? matches :nearby");
    require(rt.is_name(answer) && rt.terms(rt.as_node(rt.as_name(answer)->target)).size() == 2, "provider query lost matching identities");
    require(rt.is_unit(rt.as_name(rt.run("? matches :missing"))->target), "empty matches not represented");
    require(rt.print(rt.run("unbound ?")) == "unbound ?", "postfix demand invented semantics");

    require(rt.print(rt.run("#[You have #[nested] apples]\n3 # ignore (\n")) == "3", "comment parsing failed");
    require(rt.print(rt.run("\"#[literal] # := |\"")) == "\"#[literal] # := |\"", "comment or operator inside literal changed");
    require(seed::Reader::source_complete("# ignore ( \"\n()"), "line comment changed continuation");
    require(!seed::Reader::source_complete("#[unfinished (\n") && !seed::Reader::source_complete("(\"literal\""), "continuation ended early");
    require(seed::Reader::source_complete("#[ ) \" #[inner] ] ()"), "bracket comment syntax leaked into scanner");
    rejects([&]{rt.read("#[unfinished");}, "unterminated comment accepted");
    rejects([&]{rt.read(")");}, "unmatched group accepted");
}

} // namespace

int main() {
    semantics();
    seed::Runtime rt;

    expect_native(rt, "()", "()");
    expect_native(rt, "a", "a");
    expect_native(rt, ":a", ":a");
    expect_native(rt, "\"a\"", "\"a\"");

    expect_native(rt, "a b c", "a b c");
    expect_structural(rt, "a b c", "((a)(b)(c))");

    expect_native(rt, "a() b", "a() b");
    expect_native(rt, "a () b", "a() b");
    expect_structural(rt, "a() b", "((a)(())(b))");

    expect_native(rt, "? a", "? a");
    expect_native(rt, "?a", "?a");
    expect_native(rt, "a ?", "a ?");
    expect_native(rt, "a?", "a?");

    expect_structural(rt, "? a", "((?)(a))");
    expect_structural(rt, "?a", "(?a)");
    expect_structural(rt, "a ?", "((a)(?))");
    expect_structural(rt, "a?", "(a?)");

    expect_native(rt, ":foo", ":foo");
    expect_native(rt, ":()", ":()");
    expect_native(rt, ":|", ":|");
    expect_native(rt, "::", "::");
    expect_native(rt, ":((a)(())(b))", ":((a)(())(b))");

    expect_native(rt, "a : b", "a : b");
    expect_structural(rt, "a : b", "((a)(:)(b))");

    expect_native(rt, "\"a() b | ?\"", "\"a() b | ?\"");
    expect_structural(rt, "\"a() b | ?\"", "(\"a() b | ?\")");

    expect_native(rt,
        "camera := unity :Camera.main",
        "camera := unity :Camera.main");

    expect_structural(rt,
        "camera := unity :Camera.main",
        "((camera)(:=)(unity)(:Camera.main))");

    rt.define("?", &capture);
    require(rt.print(rt.run("? camera")) == "? camera",
            "registered '?' did not receive its relation");

    const char* samples[] = {
        "()",
        "a",
        ":a",
        ":(=)",
        ":(:=)",
        "\"hello world\"",
        "a b c",
        "a() b",
        "? a",
        "?a",
        "a ?",
        "a?",
        "camera := unity :Camera.main",
        ":((a)(())(b))"
    };

    for (const char* source : samples) {
        seed::Ptr one = rt.read(source);
        std::string printed = rt.print(one);
        seed::Ptr two = rt.read(printed);

        require(rt.structural(one) == rt.structural(two),
                "round trip changed structure");
    }

    std::cout << "seed_tests: PASS\n";
    return EXIT_SUCCESS;
}
