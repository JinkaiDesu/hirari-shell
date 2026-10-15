#include "seed/seed.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Flat products and explicitly nested left applications have one application
// observation. Names remain opaque to substitution, but their retained target
// can be inspected here; addresses are not portable semantic observations.
std::string observe(seed::Runtime& r, seed::Ptr p, unsigned depth = 0) {
    require(depth < 512, "conformance observation depth exceeded");
    if (r.is_unit(p)) return "U";
    if (r.is_symbol(p)) return "S[" + r.as_symbol(p)->text + "]";
    if (r.is_name(p)) return "N[" + observe(r, r.as_name(p)->target, depth + 1) + "]";
    seed::Ptr binder = nullptr, body = nullptr;
    if (r.abstraction(p, binder, body)) {
        return "L[" + (r.is_wildcard(binder) ? std::string("_") : r.print(binder)) +
               "," + observe(r, body, depth + 1) + "]";
    }
    if (r.is_relation(p)) {
        auto terms = r.terms(r.as_node(p));
        require(!terms.empty(), "empty relation observation");
        auto answer = observe(r, terms.front(), depth + 1);
        for (std::size_t i = 1; i < terms.size(); ++i)
            answer = "A[" + answer + "," + observe(r, terms[i], depth + 1) + "]";
        return answer;
    }
    return "V[" + std::to_string(static_cast<unsigned>(r.arena().kind(p))) +
           "," + r.print(p) + "]";
}

std::vector<std::string> fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t start = 0;
    for (;;) {
        auto end = line.find('\t', start);
        result.push_back(line.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) return result;
        start = end + 1;
    }
}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        require(argc == 2, "usage: seed_lean_conformance CORPUS.tsv");
        std::ifstream input{std::filesystem::path(argv[1]), std::ios::binary};
        require(input.good(), "cannot read Lean conformance corpus");
        std::string line;
        require(static_cast<bool>(std::getline(input, line)) &&
                line == "# seed-lean-conformance-v1", "wrong conformance corpus version");
        std::size_t count = 0, substitutions = 0, reductions = 0;
#ifdef SEED_DEFERRED_EXPERIMENT
        std::uint64_t fused = 0;
#endif
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            auto f = fields(line);
            require(f.size() == 5, "malformed corpus row " + std::to_string(count + 1));
#ifdef SEED_DEFERRED_EXPERIMENT
            seed::Runtime runtime(true);
#else
            seed::Runtime runtime;
#endif
            auto sentinel = runtime.intern("conformance_sentinel");
            auto retained_value = runtime.read("-retained");
            runtime.bind(sentinel, retained_value);
            auto original = runtime.read(f[2]);
            const auto original_shape = runtime.structural(original);
            seed::Ptr actual = nullptr;
            if (f[0] == "S") {
                actual = runtime.substitute(original, runtime.intern(f[1]), runtime.read(f[3]));
                ++substitutions;
            } else {
                require(f[0] == "R", "unknown conformance operation");
                actual = runtime.reduce(original);
                ++reductions;
            }
            auto expected = runtime.read(f[4]);
            require(observe(runtime, actual) == observe(runtime, expected),
                    "Lean conformance mismatch at row " + std::to_string(count + 1) +
                    "\nsource: " + f[2] + "\nexpected: " + runtime.print(expected) +
                    "\nactual: " + runtime.print(actual));
            require(runtime.structural(original) == original_shape, "retained source mutated");
            require(runtime.binding(sentinel) == retained_value, "global binding mutated");
            // Opaque host identities must survive substitution by pointer identity.
            int payload = 42;
            auto foreign = runtime.arena().make_name(&payload);
            require(runtime.substitute(foreign, runtime.intern("x"), expected) == foreign,
                    "opaque foreign Name identity changed");
#ifdef SEED_DEFERRED_EXPERIMENT
            fused += runtime.deferred_stats().chains;
#endif
            ++count;
        }
        require(count > 0 && input.eof(), "empty or unreadable conformance corpus");
        std::cout << "LEAN_CPP_CONFORMANCE_OK " << count << " cases; substitutions="
                  << substitutions << "; reductions=" << reductions;
#ifdef SEED_DEFERRED_EXPERIMENT
        require(fused > 0, "conformance never exercised the optimized curry path");
        std::cout << "; fused=" << fused;
#endif
        std::cout << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
