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
seed::Ptr consume_twice(seed::Runtime& rt, seed::Node* relation, seed::Ptr context) {
    auto terms = rt.terms(relation);
    if (terms.size() != 3) return relation;
    auto& effects = *static_cast<Effects*>(context);
    require(rt.reduce(terms[1]) == effects.value && rt.reduce(terms[2]) == effects.value,
            "duplicated substitution lost opaque identity");
    return effects.value;
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
    for (auto spelling : {"consume (source -input)", "source -input | consume", "consume(source -input)"}) {
        effects.trace.clear();
        require(rt.run(spelling) == effects.value && effects.trace == "PC", "application/flow effects differ");
    }
    effects.trace.clear();
    require(rt.run("source -input | consume | consume") == effects.value && effects.trace == "PCC", "flow chain order changed");
    effects.trace.clear();
    require(rt.print(rt.run("if -false (source -input | consume) \"idle\"")) == "\"idle\"" && effects.trace.empty(), "orientation eagerly executed an unchosen branch");
    require(rt.structural(rt.run("a b")) == rt.structural(rt.run("b | a")), "product mirror mismatch");
    require(rt.structural(rt.run("a b")) == rt.structural(rt.run("a(b)")), "call product mismatch");
    require(rt.structural(rt.run("a () b")) == rt.structural(rt.run("b : a")), "capture mirror mismatch");
    require(rt.structural(rt.run("a : b")) != rt.structural(rt.run("a -b")), "detached colon became Name application");
    require(rt.structural(rt.run("a () b")) != rt.structural(rt.run("a b")), "Unit was discarded");
    auto captured = rt.run("consume () (source -input)");
    require(rt.is_relation(captured), "capture became ordinary application");

    auto cell = rt.run("@ -x");
    require(rt.run(rt.structural(cell)) == cell, "structural Cell printing changed binding identity");
    auto rawAlias = rt.arena().make_name(rt.intern("lt"));
    require(rt.as_name(rt.read(rt.print(rawAlias)))->target == rt.intern("lt"), "raw identity became an operator alias on print/read");
    require(rt.as_name(rt.read("-lt"))->target == rt.intern("<"), "named operator alias lost its target");
    rt.run("lt := 3");auto aliasCell = rt.run("@lt");
    require(rt.run(rt.structural(aliasCell)) == aliasCell, "binding Cell became an operator alias on print/read");
    effects.trace.clear();
    require(rt.run("x := source -input") == effects.value && effects.trace == "P", "forward binding effects");
    auto head = rt.binding(rt.intern("x"));
    require(rt.is_relation(head) && rt.run("@ x") == cell, "binding lost lazy form or stable cell");
    auto revision = rt.as_cell(cell)->revision;
    effects.trace.clear();
    require(rt.run("source -input = x") == effects.value && effects.trace == "P", "reverse binding effects");
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
    auto answer = rt.run("demand matches -nearby");
    require(rt.is_name(answer) && rt.terms(rt.as_node(rt.as_name(answer)->target)).size() == 2, "provider query lost matching identities");
    require(rt.is_unit(rt.as_name(rt.run("demand matches -missing"))->target), "empty matches not represented");
    require(rt.structural(rt.run("(matches -nearby)?")) == rt.structural(answer), "attached answer lost plural identities");
    require(rt.is_unit(rt.as_name(rt.run("(matches -missing)?"))->target), "attached answer lost empty query result");
    rt.bind(rt.intern("producer"), rt.read("source -input"));
    for (auto spelling : {"consume producer?", "producer? | consume", "consume(producer?)"}) {
        effects.trace.clear();
        require(rt.run(spelling) == effects.value && effects.trace == "PC", "attached producer duplicated effects or lost opaque identity");
    }
    require(rt.structural(rt.read("a b?")) == rt.structural(rt.run("b? | a")), "attached producer flow lowering mismatch");
    require(rt.structural(rt.read("b?")) != rt.structural(rt.read("b ?")), "attachment became detached help");
    effects.trace.clear();
    require(rt.print(rt.run("if -false producer? \"idle\"")) == "\"idle\"" && effects.trace.empty(), "attached answer eagerly evaluated an unchosen branch");
    rt.bind(rt.intern("predicate"), rt.read("-true"));
    require(rt.print(rt.run("predicate?")) == "-true", "predicate answer changed value kind");
    seed::Runtime unhosted;
    require(!unhosted.registry().find(unhosted.intern("?")) &&
            unhosted.print(unhosted.run("? source -input")) == "? source -input",
            "standalone '?' still supplies demand semantics instead of leaving help to its host");
    require(rt.print(rt.run("unbound ?")) == "unbound ?", "postfix demand invented semantics");

    require(rt.print(rt.run("#[You have #[nested] apples]\n3 # ignore (\n")) == "3", "comment parsing failed");
    require(rt.print(rt.run("\"#[literal] # := |\"")) == "\"#[literal] # := |\"", "comment or operator inside literal changed");
    require(seed::Reader::source_complete("# ignore ( \"\n()"), "line comment changed continuation");
    require(!seed::Reader::source_complete("#[unfinished (\n") && !seed::Reader::source_complete("(\"literal\""), "continuation ended early");
    require(seed::Reader::source_complete("#[ ) \" #[inner] ] ()"), "bracket comment syntax leaked into scanner");
    rejects([&]{rt.read("#[unfinished");}, "unterminated comment accepted");
    rejects([&]{rt.read(")");}, "unmatched group accepted");
}

void abstractions() {
    seed::Runtime rt;
    auto reduces = [&](std::string_view source, std::string_view expected) {
        auto actual = rt.run(source);
        if (rt.structural(actual) != rt.structural(rt.read(expected))) {
            std::cerr << "SOURCE: " << source << "\nEXPECTED: " << expected
                      << "\nACTUAL: " << rt.print(actual) << '\n';
            throw std::runtime_error("abstraction reduction mismatch");
        }
    };
    reduces("(a() a) b", "b");
    reduces("(a() b) c", "b");
    reduces("(a() (f a)) x", "f x");
    reduces("(a() f a) x", "f x");
    reduces("(a() (b() a)) x", "b() x");
    reduces("(a() (b() b)) x", "b() b");
    reduces("(a() (a() a)) b", "a() a"); // Same-identity shadowing.
    reduces("(a() (b() (a c))) d", "b() (d c)"); // c stays free.
    reduces("(a() (b() a)) b", "b() b"); // Deliberate capture.
    reduces("((a() (b() a)) b) c", "c");
    reduces("(a() (z() a)) b", "z() b"); // Renaming changes capture.
    reduces("(a() (a a)) (a() a)", "a() a"); // Terminating self-application.
    reduces("(a() a) (a() a)", "a() a");
    reduces("(a() (b() a b)) b", "b() (b b)"); // Incoming b is captured.
    reduces("(a() -a) b", "-a"); // Names are values, not variable occurrences.
    reduces("(a() -(a a)) b", "-(a a)");
    reduces("(a() \"a\") b", "\"a\"");
    reduces("(a() a) ()", "()"); // Unit is a real supplied value.
    reduces("(a() ()) b", "()");
    reduces("3 : Int", "Int() 3");
    reduces("(3 : Int) -not-an-integer", "3"); // No implicit classification.
    reduces("(a : a) b", "b");
    reduces("(a() (a : b)) b", "b() b"); // Colon respects nested binders.
    reduces("(a() (a : a)) b", "a() a");
    reduces("b | (a() a)", "b");
    reduces("b | (a : a)", "b");
    reduces("apply -(a() a) -(b)", "b");
    reduces("apply -(a() (b() a)) -(x y)", "x");
    reduces("(f x : x) 3", "f 3");
    reduces("(f x : x : f) (v : v) 3", "3");
    reduces("(f (g x) : x : g : f) (v : v) (v : v) 3", "3");
    reduces("(a : b : a) b", "b() b");
    reduces("(x x : x) (v : v)", "v() v");
    reduces("(_() \"yes\") (missing -anything)", "\"yes\"");
    reduces("(\"yes\" : _) (missing -anything)", "\"yes\"");
    for (auto spelling : {"a:b", "a: b", "a :b", "a : b"}) reduces(spelling, "b() a");
    rt.run("second := a : a : b");
    reduces("second 1 2", "2");

    rt.run("identity := a() a");
    reduces("identity b", "b");
    reduces("identity ()", "()");
    reduces("b | identity", "b");
    rt.run("(a() a) = reverseIdentity");
    reduces("reverseIdentity b", "b");
    rt.run("first := a() (b() a)");
    reduces("first x y", "x");
    reduces("first x", "b() x");
    reduces("(first x) y", "x");
    rt.run("alias := first");
    reduces("alias x y", "x");

    int payload = 42;
    Effects effects{"", rt.arena().make_name(&payload)};
    rt.define("source", &source, &effects);
    rt.define("consume", &consume, &effects);
    rt.run("use := a() (consume a)");
    require(effects.trace.empty(), "defining an abstraction ran its body");
    require(rt.run("use (source -input)") == effects.value && effects.trace == "PC",
            "abstraction application lost provider effects or opaque identity");
    effects.trace.clear();
    rt.run("ignore := a() -done");
    reduces("ignore (source -input)", "-done");
    require(effects.trace.empty(), "unused abstraction argument was demanded");
    rt.run("consume() (source -input)");
    require(effects.trace.empty(), "native program binder ran before application");
    rt.run("a := \"global\"");
    reduces("a() a", "a() a");
    reduces("(a() a) \"local\"", "\"local\"");
    require(rt.print(rt.binding(rt.intern("a"))) == "\"global\"",
            "substitution mutated the global binding cell");
    auto retained = rt.binding(rt.intern("use"));
    auto before = rt.structural(retained);
    rt.run("use (source -input)");
    require(rt.binding(rt.intern("use")) == retained && rt.structural(retained) == before,
            "application mutated its retained abstraction");

    seed::Runtime divergent;
    try {
        divergent.run("(a() (a a)) (a() (a a))");
        throw std::runtime_error("divergent self-application falsely terminated");
    } catch (const std::exception& e) {
        require(std::string(e.what()) == "Seed reduction exceeded step budget",
                "divergent abstraction failed outside the reducer budget");
    }
    require(divergent.print(divergent.run("(a() a) \"recovered\"")) == "\"recovered\"",
            "reducer did not recover after divergent application");
}

void abstraction_reuse() {
    seed::Runtime rt;
    rt.run("identity := x : x");
    rt.run("compose := f (g x) : x : g : f");
    int payload = 42;
    Effects effects{"", rt.arena().make_name(&payload)};
    rt.define("source", &source, &effects);
    rt.define("consume", &consume, &effects);
    rt.define("consumeTwice", &consume_twice, &effects);
    auto curried = rt.read("compose consume consume (source -input)");
    for (int i = 0; i < 2; ++i) {
        effects.trace.clear();
        require(rt.reduce(curried) == effects.value && effects.trace == "PCC",
                "reused curry cached or reordered effects");
    }
    rt.run("twice := x() (consumeTwice x x)");
    effects.trace.clear();
    require(rt.run("twice (source -input)") == effects.value && effects.trace == "PP",
            "duplicated argument was evaluated only once");

    rt.run("freeValue := 3");
    rt.run("live := x() freeValue");
    auto live = rt.read("live 0");
    require(rt.print(rt.reduce(live)) == "3", "initial free binding missing");
    rt.run("freeValue := 7");
    require(rt.print(rt.reduce(live)) == "7", "repeated application froze a free binding");
    rt.run("live := x() 9");
    require(rt.print(rt.reduce(live)) == "9", "repeated application froze its callable binding");

    // Even direct host edits to the existing pointer product must stay visible.
    auto retained = rt.as_node(rt.binding(rt.intern("live")));
    auto bodyNode = static_cast<seed::Node*>(static_cast<seed::Node*>(retained->ref)->ref);
    bodyNode->head = rt.read("8");
    require(rt.print(rt.reduce(live)) == "8", "abstraction descriptor survived a host edit");
    static_cast<seed::Node*>(retained->ref)->head = rt.intern("ordinary");
    require(!rt.is_abstraction(retained), "shape recognition cached a changed unit marker");

    auto flat = rt.read("x() f x");
    auto before = rt.structural(flat);
    seed::Ptr binder = nullptr, body = nullptr;
    require(rt.abstraction(flat, binder, body), "flat abstraction not recognized");
    rt.as_node(body)->head = rt.intern("changed");
    require(rt.structural(flat) == before, "extracted flat body aliases retained spine");
    auto colon = rt.read("f (g x) : x : g : f");
    before = rt.structural(colon);
    require(rt.substitute(colon, rt.intern("absent"), rt.read("0")) == colon &&
            rt.structural(colon) == before, "unchanged substitution rebuilt or mutated a colon chain");
    auto singleton = rt.arena().make_relation({rt.intern("x")});
    auto replaced = rt.substitute(singleton, rt.intern("x"), rt.read("3"));
    require(rt.is_relation(replaced) && rt.terms(rt.as_node(replaced)).size() == 1,
            "substitution collapsed a host's singleton relation");
}

void source_layout() {
    seed::Runtime rt;
    auto same = [&](std::string_view source, std::string_view expected) {
        require(rt.structural(rt.read(source)) == rt.structural(rt.read(expected)),
                "source layout changed product structure");
    };
    same("a\tb\n  c", "a b c");
    same("a b \\\n  c", "a b c");
    same("a b \\\r\n\t c", "a b c");
    same("a\\\nb", "a b"); // Layout separates, never concatenates tokens.
    same("a\\\n?", "a ?"); // Continuation does not create attached demand.
    same("consumer @\\\nname", "consumer @ name");
    same("consumer *\\\r\n\tname", "consumer * name");
    same("-\\\nname", "- name"); // A Name requires actual attachment.
    same("a() \\\n\t(a b)", "a() (a b)");
    same("a # ignored \\\n b", "a b");
    same("a #[ ignored \\\n #[nested] ] b", "a b");
    same("a\\ordinary", "a\\ordinary");
    for (auto source : {"a \\" , "a \\\n", "a \\\r\n\t "}) {
        require(!seed::Reader::source_complete(source), "continuation prematurely completed");
        rejects([&] { rt.read(source); }, "dangling source continuation accepted");
    }
    require(seed::Reader::source_complete("a \\\n b") &&
            seed::Reader::source_complete("a \\\n\n"), "completed continuation still pending");
    require(seed::Reader::source_complete("# ignored \\"), "comment continued source");
    require(!seed::Reader::source_has_form("\\\n\n # comment\n") &&
            seed::Reader::source_has_form("\\\n \"value\""), "layout-only source counted as a form");
    auto literal = rt.read(R"seed("a\n\r\t\\\" b")seed");
    require(rt.as_literal(literal)->bytes == "a\n\r\t\\\" b", "explicit literal escapes changed");
    for (auto source : {R"seed("first \
    second")seed", R"seed("""first \
    second""")seed"}) {
        require(rt.as_literal(rt.read(source))->bytes == "first \\\n    second",
                "source continuation erased literal whitespace");
        require(seed::Reader::source_complete(source), "literal continuation confused scanner");
    }
    require(rt.as_literal(rt.read("\"\"\"first\r\n\tsecond\"\"\""))->bytes == "first\r\n\tsecond",
            "multiline literal normalized physical whitespace");
}

void updated_surface() {
    seed::Runtime rt;
    auto expect = [&](std::string_view source, std::string_view expected) {
        require(rt.structural(rt.run(source)) == rt.structural(rt.read(expected)), "updated surface reduction");
    };
    for (auto spelling : {"3", "-3", "3.14", "1e3", "-0.5", "9223372036854775807", "-9223372036854775808"}) {
        auto value = rt.read(spelling);
        require(rt.is_number(value) && rt.reduce(value) == value, "numeric literal resolved as vocabulary");
        require(rt.print(value) == spelling && rt.is_number(rt.read(rt.print(value))), "numeric roundtrip");
    }
    rejects([&]{rt.read("9223372036854775808");}, "integer overflow accepted");
    rejects([&]{rt.read("1e9999");}, "nonfinite real accepted");
    rt.bind(rt.intern("3"), rt.read("\"wrong\""));expect("3", "3");
    auto positions = rt.terms(rt.as_node(rt.read("f _ _")));
    require(positions[1] != positions[2] && rt.is_wildcard(positions[1]), "wildcards shared identity");
    rejects([&]{rt.run("_ := 3");}, "wildcard bound");
    rejects([&]{rt.run("@_");}, "wildcard cell created");
    rejects([&]{rt.ensure_cell(rt.intern("_"));}, "host created wildcard cell");
    rt.run("x := 3");auto cell = rt.run("@x");
    expect("3=-r", "3");expect("*r", "3");
    rejects([&]{rt.run("-r=3");}, "binding orientation reversed implicitly");
    require(rt.is_cell(cell) && rt.run("@ x") == cell, "attached reference changed identity");
    rt.run("x := 4");require(rt.run("@x") == cell, "reference changed on rebinding");expect("*x", "4");
    expect("3<5", "-true");expect("5<3", "-false");expect("3<=3", "-true");
    expect("9007199254740993 < 9007199254740992.0", "-false");
    expect("9223372036854775807 < 9223372036854775808.0", "-true");
    expect("-2 < -1.5", "-true");expect("\"a\" < \"b\"", "-true");
    expect("(3 -lt 5)?", "-true");
    require(rt.run("3 -lt 5") != rt.run("3 < 5"), "named proposition evaluated without demand");
    expect("-false && (missing -thing)", "-false");
    expect("-true || (missing -thing)", "-true");
    expect("-false || \"right\"", "\"right\"");expect("-true && \"right\"", "\"right\"");
    expect("-true || -false && \"left-association\"", "\"left-association\"");
    expect("&& -true \"right\"", "\"right\"");expect("apply -or -(-false 7)", "7");
    expect("0||7", "7");expect("1&&7", "7");
    require(rt.as_name(rt.read("-pipe"))->target == rt.intern("|"), "pipe Name lost program identity");
    require(rt.structural(rt.read("a (|) b")) == rt.structural(rt.read("a | b")), "grouping transparency changed");
    auto multiline=rt.read(R"seed("""first "quote" 'single'
# not a comment #[nor this]
last\nline""")seed");
    require(rt.is_literal(multiline) && rt.as_literal(multiline)->bytes=="first \"quote\" 'single'\n# not a comment #[nor this]\nlast\nline", "triple string changed literal content");
    require(rt.structural(rt.read(rt.print(multiline)))==rt.structural(multiline), "multiline string did not roundtrip");
    require(!seed::Reader::source_complete("\"\"\"first\n\"quote\""), "triple string completed at a single quote");
    require(seed::Reader::source_complete("\"\"\"first\n# ignored syntax (\n\"\"\""), "triple string leaked comment/group syntax");
    rejects([&]{rt.read("\"\"\"unfinished");}, "unfinished triple string accepted");
    rt.run("documented := x() (\"\"\"Returns its input.\n# doc text\"\"\" x)");
    expect("documented 3", "3");
    expect("(x() \"constant\") 3", "\"constant\"");
}

} // namespace

int run_all();
int main() {
    try { return run_all(); }
    catch (const std::exception& error) { std::cerr << "seed_tests: FAIL: " << error.what() << std::endl; return EXIT_FAILURE; }
}
int run_all() {
    semantics();
    abstractions();
    abstraction_reuse();
    source_layout();
    updated_surface();
    seed::Runtime rt;

    expect_native(rt, "()", "()");
    expect_native(rt, "a", "a");
    expect_native(rt, "-a", "-a");
    expect_native(rt, "\"a\"", "\"a\"");

    expect_native(rt, "a b c", "a b c");
    expect_structural(rt, "a b c", "((a)(b)(c))");

    expect_native(rt, "a() b", "a() b");
    expect_native(rt, "a () b", "a() b");
    expect_structural(rt, "a() b", "((a)(())(b))");

    expect_native(rt, "? a", "? a");
    expect_native(rt, "?a", "?a");
    expect_native(rt, "a ?", "a ?");
    expect_native(rt, "a?", "demand a");

    expect_structural(rt, "? a", "((?)(a))");
    expect_structural(rt, "?a", "(?a)");
    expect_structural(rt, "a ?", "((a)(?))");
    expect_structural(rt, "a?", "((demand)(a))");
    require(rt.structural(rt.read("text(value)")) == rt.structural(rt.read("text (value)")),
            "attached opening parenthesis is not a delimiter");
    require(rt.structural(rt.read("text() value")) == rt.structural(rt.read("text () value")),
            "attached Unit changed capture structure");

    expect_native(rt, "-foo", "-foo");
    expect_native(rt, "-()", "-()");
    expect_native(rt, "-pipe", "-pipe");
    expect_native(rt, "-(:)", "-(:)");
    expect_native(rt, "-((a)(())(b))", "-((a)(())(b))");

    expect_native(rt, "a : b", "a : b");
    expect_structural(rt, "a : b", "((a)(:)(b))");

    expect_native(rt, "\"a() b | ?\"", "\"a() b | ?\"");
    expect_structural(rt, "\"a() b | ?\"", "(\"a() b | ?\")");

    expect_native(rt,
        "camera := unity -Camera.main",
        "camera := unity -Camera.main");

    expect_structural(rt,
        "camera := unity -Camera.main",
        "((camera)(:=)(unity)(-Camera.main))");

    rt.define("?", &capture);
    require(rt.print(rt.run("? camera")) == "? camera",
            "registered '?' did not receive its relation");

    const char* samples[] = {
        "()",
        "a",
        "-a",
        "-(=)",
        "-(:=)",
        "\"hello world\"",
        "a b c",
        "a() b",
        "? a",
        "?a",
        "a ?",
        "a?",
        "(a b)?",
        "-a?",
        "(-a)?",
        "\"literal?\"",
        "camera := unity -Camera.main",
        "-((a)(())(b))",
        "{a | b}",
        "{}",
        "{ls}",
        "{a | b}?",
        "c {a | b | d}",
        "a (b {c | d})",
        "{ a (b | c) | d }",
        "{(a b)}",
        "\"{x}\"",
        "-(pipe)"
    };

    for (const char* source : samples) {
        seed::Ptr one = rt.read(source);
        std::string printed = rt.print(one);
        seed::Ptr two = rt.read(printed);

        require(rt.structural(one) == rt.structural(two),
                "round trip changed structure");
        require(rt.structural(rt.read(rt.structural(one))) == rt.structural(one),
                "structural round trip changed structure");
    }

    // `{ }` is a pipeline context: a reserved head over a nested body. Its |
    // never orients the enclosing relation; outside braces | is unchanged.
    {
        seed::Ptr braced = rt.read("c {a | b}");
        auto outer = rt.terms(rt.as_node(braced));
        require(outer.size() == 2 && rt.pipeline_body(outer[1]), "brace did not lower to one pipeline term");
        auto body = rt.terms(rt.as_node(rt.pipeline_body(outer[1])));
        require(body.size() == 3 && rt.as_symbol(body[1])->text == "|", "brace body lost its pipe");
        auto grouped = rt.terms(rt.as_node(rt.read("c (a | b)")));
        require(grouped.size() == 2 && !rt.pipeline_body(grouped[1]) &&
                rt.terms(rt.as_node(grouped[1])).size() == 3, "parenthesized pipe changed");
        require(rt.pipeline_body(rt.read("{ls}")) && rt.is_relation(rt.pipeline_body(rt.read("{ls}"))),
                "single-stage brace body must stay a relation");
        require(rt.is_unit(rt.pipeline_body(rt.read("{}"))), "empty pipeline body is Unit");
        require(rt.print(rt.read("c {a | b}")) == "c { a | b }", "brace printing changed");
        require(rt.is_literal(rt.read("\"{a | b}\"")), "quoted brace must remain a literal");
        bool rejected = false;
        try { rt.read("{a | b"); } catch (const std::exception&) { rejected = true; }
        require(rejected, "unterminated brace accepted");
        rejected = false;
        try { rt.read("(a }"); } catch (const std::exception&) { rejected = true; }
        require(rejected, "mismatched brace accepted");
        require(!seed::Reader::source_complete("{ a |") && seed::Reader::source_complete("{ a | b }") &&
                seed::Reader::source_complete("\"{\""), "brace continuation depth");
        // In a host-less runtime the context is inert data: no process is implied.
        require(rt.pipeline_body(rt.run("{a | b}")), "core runtime must not reduce a pipeline context");
    }

    // Operators are standalone tokens. Attached spellings keep their meaning:
    // -3 is a number and -foo a Name, so (3 - 3) and (3 -3) differ.
    {
        seed::Runtime ops;
        auto value = [&](std::string_view source) { return ops.print(ops.run(source)); };
        require(value("3 - 3") == "0" && value("3 -3") == "3 -3", "spaced minus must subtract; attached -3 stays a number");
        require(value("1 + 2 * 3") == "7" && value("(1 + 2) * 3") == "9" && value("10 - 4 - 3") == "3",
                "arithmetic precedence or left associativity changed");
        require(value("8 / 2") == "4" && value("10 / 4") == "2.5" && value("2.5 * 2") == "5.0",
                "division or real arithmetic changed");
        require(value("\"ab\" + \"cd\"") == "\"abcd\"" && value("- 5") == "-5", "string join or negation changed");
        bool failed = false;
        try { ops.run("1 / 0"); } catch (const std::exception&) { failed = true; }
        require(failed, "division by zero must fail");
        failed = false;
        try { ops.run("9223372036854775807 + 1"); } catch (const std::exception&) { failed = true; }
        require(failed, "integer overflow must fail");
        for (auto [source, expected] : {std::pair{"3 < 5", "-true"}, {"5 > 3", "-true"}, {"3 >= 3", "-true"},
                                        {"3 <= 2", "-false"}, {"3 == 3.0", "-true"}, {"\"a\" != \"b\"", "-true"},
                                        {"1 == \"1\"", "-false"}, {"-x == -x", "-true"}, {"1 + 2 < 4", "-true"},
                                        {"> 5 3", "-true"}})
            require(value(source) == expected, "comparison operator changed");
        require(ops.print(ops.read("3 -gt 5")) == "3 -gt 5" && value("(3 -gt 5)?") == "-false",
                "named comparison lost its identity or demand");
        failed = false;
        try { ops.run("1 < 2 < 3"); } catch (const std::exception&) { failed = true; }
        require(failed, "chained comparison must be rejected");
        // Tokens: == => >= != are single identities.
        auto token_terms = ops.terms(ops.as_node(ops.read("a == b => c >= d != e")));
        require(token_terms.size() == 9 && ops.as_symbol(token_terms[1])->text == "==" &&
                ops.as_symbol(token_terms[3])->text == "=>" && ops.as_symbol(token_terms[5])->text == ">=" &&
                ops.as_symbol(token_terms[7])->text == "!=", "operator tokens split incorrectly");
        // A registered Program is never an abstraction binder.
        require(value("if () \"yes\" \"no\"") == "\"no\"", "if () became an abstraction");
    }

    // => advances exactly one reduction step and never runs an effect.
    {
        seed::Runtime steps;
        auto step = [&](std::string_view source) { return steps.print(steps.run(source)); };
        require(step("((x() (x x)) 5) =>") == "-((5)(5))", "=> did not take one beta step");
        require(step("(2 + 3) * 4 =>") == "-((5)(*)(4))", "=> did not compute the leftmost operand first");
        require(step("(2 + 3) * 4 => =>") == "-(20)", "=> => did not take two steps");
        steps.run("twice := (x x) : x");
        require(step("twice 4 => =>") == "-((4)(4))", "=> did not unfold then substitute");
        require(step("5 =>") == "-(5)", "=> changed a normal form");
        bool stopped = false;
        try { steps.run("if -true 1 2 =>"); } catch (const std::exception& error) {
            stopped = std::string(error.what()).find("effect") != std::string::npos;
        }
        require(stopped, "=> ran a Program instead of stopping before the effect");
    }

    // A ( that ends its line opens a line block: lines are statements.
    // A group that starts on the same line stays one expression.
    {
        seed::Runtime blocks;
        seed::Ptr block = blocks.read("(\n  a := 1\n  a + 1\n)");
        require(blocks.is_block(block) && blocks.terms(blocks.as_node(block)).size() == 3, "line block did not lower");
        require(blocks.print(blocks.run("(\n  a := 1\n  a + 1\n)")) == "2", "line block did not run in order");
        require(!blocks.is_block(blocks.read("(data list\n  1 2)")), "same-line group became a block");
        require(blocks.print(blocks.read("(\n  only one\n)")) == "only one", "single-statement block changed");
        require(blocks.print(blocks.read("(\n  a\n  | b\n)")) == "a | b", "leading | did not continue a block line");
        require(blocks.print(blocks.read("(\n  a \\\n    b\n  c\n)")) == "(\na b\nc\n)", "continuation or block printing changed");
        require(blocks.print(blocks.run("if -true (\n  b := 2\n  b * 3\n) ()")) == "6", "block branch did not run");
        require(!seed::Reader::source_complete("if c (\n a\n") && seed::Reader::source_complete("if c (\n a\n)"),
                "block continuation depth");
    }

    // [a b c] is a list literal: each element evaluated once.
    {
        seed::Runtime lists;
        require(lists.print(lists.read("[1 (2 + 3) -x]")) == "[1 (2 + 3) -x]", "list literal printing changed");
        require(lists.print(lists.run("[1 (2 + 3) \"x\"]")) == "-((-list)(-((1)(5)(\"x\"))))", "list literal did not evaluate its elements");
        require(lists.collection(lists.run("[1 2 3]")).size() == 3 && lists.collection(lists.run("[]")).empty(),
                "list literal is not a collection");
        require(lists.is_list_form(lists.read("#[ a comment ] [a]")), "#[ comment no longer precedes a list");
    }

    std::cout << "seed_tests: PASS\n";
    return EXIT_SUCCESS;
}
