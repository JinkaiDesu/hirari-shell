#include "seed/seed.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr int count = 5000;
constexpr int repetitions = 9;
constexpr int warmup = 200;
constexpr std::array<const char*, 4> names{
    "identity", "curried composition", "short circuit", "canonical curry"};
constexpr std::array<const char*, 4> sources{
    "identity 3", "compose identity identity 3", "-false || identity 3",
    "canonical identity identity 3"};

void prepare(seed::Runtime& runtime) {
    runtime.run("identity := x : x");
    runtime.run("compose := f (g x) : x : g : f");
    runtime.run("canonical := f() (g() (x() (f (g x))))");
}

void validate(seed::Runtime& runtime, seed::Ptr value) {
    if (!runtime.is_number(value) || runtime.as_number(value)->integer != 3)
        throw std::runtime_error("benchmark changed semantics");
}
}

int main(int argc, char** argv) {
#ifdef SEED_DEFERRED_EXPERIMENT
    const bool enabled = argc == 1 || std::string_view(argv[1]) != "--literal";
    std::cout << (enabled ? "Deferred experiment\n" : "Literal reference in experimental build\n");
#else
    (void)argc;
    (void)argv;
#endif
#ifdef SEED_PROFILE
    // Profile one reduction in a fresh arena. Never use this instrumented
    // executable for timing comparisons with the production library.
    for (std::size_t i = 0; i < sources.size(); ++i) {
#ifdef SEED_DEFERRED_EXPERIMENT
        seed::Runtime runtime(enabled);
#else
        seed::Runtime runtime;
#endif
        prepare(runtime);
        auto form = runtime.read(sources[i]);
        runtime.arena().profile() = {};
#ifdef SEED_DEFERRED_EXPERIMENT
        runtime.deferred_stats() = {};
#endif
        auto value = runtime.reduce(form);
        auto work = runtime.arena().profile();
        validate(runtime, value);
        std::cout << names[i] << ": nodes=" << work.nodes
                  << ", relations=" << work.relations
                  << ", term_walks=" << work.term_walks
                  << ", terms_copied=" << work.terms_copied
                  << ", abstraction_checks=" << work.abstraction_checks
                  << ", body_extractions=" << work.body_extractions
                  << ", substitution_visits=" << work.substitution_visits
                  << ", reduction_steps=" << work.reduction_steps
                  << ", compositions=" << work.compositions
                  << ", applications=" << work.applications;
#ifdef SEED_DEFERRED_EXPERIMENT
        const auto& deferred = runtime.deferred_stats();
        std::cout << ", chains=" << deferred.chains
                  << ", deferred_betas=" << deferred.deferred_betas
                  << ", materializations=" << deferred.materializations
                  << ", inspection_barriers=" << deferred.inspection_barriers;
#endif
        std::cout << '\n';
    }
#else
    std::array<std::vector<double>, sources.size()> samples;
    // Rotate workload order and give every sample its own identically prepared
    // Runtime. Parsing, printing, validation and destruction are outside timing.
    for (int repeat = 0; repeat < repetitions; ++repeat) {
        for (std::size_t offset = 0; offset < sources.size(); ++offset) {
            auto i = (offset + repeat) % sources.size();
#ifdef SEED_DEFERRED_EXPERIMENT
            seed::Runtime runtime(enabled);
#else
            seed::Runtime runtime;
#endif
            prepare(runtime);
            auto form = runtime.read(sources[i]);
            seed::Ptr value = nullptr;
            for (int n = 0; n < warmup; ++n) value = runtime.reduce(form);
            validate(runtime, value);
            auto begin = std::chrono::steady_clock::now();
            for (int n = 0; n < count; ++n) value = runtime.reduce(form);
            auto elapsed = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - begin).count();
            validate(runtime, value);
            samples[i].push_back(elapsed / count);
        }
    }
    std::cout << "Reduction only; " << repetitions << " fresh runtimes/workload, "
              << count << " reductions/sample, " << warmup << " warmup\n";
    for (std::size_t i = 0; i < sources.size(); ++i) {
        auto& values = samples[i];
        std::sort(values.begin(), values.end());
        std::cout << names[i] << ": median=" << values[values.size()/2]
                  << " us/reduction, min=" << values.front()
                  << ", max=" << values.back() << '\n';
    }
#endif
}
