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

} // namespace

int main() {
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
        "camera = unity :Camera.main",
        "camera = unity :Camera.main");

    expect_structural(rt,
        "camera = unity :Camera.main",
        "((camera)(=)(unity)(:Camera.main))");

    rt.define("?", &capture);
    require(rt.print(rt.run("? camera")) == "? camera",
            "registered '?' did not receive its relation");

    const char* samples[] = {
        "()",
        "a",
        ":a",
        "\"hello world\"",
        "a b c",
        "a() b",
        "? a",
        "?a",
        "a ?",
        "a?",
        "camera = unity :Camera.main",
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
