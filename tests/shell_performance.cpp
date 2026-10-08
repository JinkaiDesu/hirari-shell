#include "hirari/shell.hpp"
#include "hirari/win32.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
struct Host : hirari::Host {
    std::string errors;
    void write(std::string_view) override {}
    void error(std::string_view text) override { errors += text; }
};
constexpr std::array sources{"identity 3", "compose identity identity 3",
    "canonical identity identity 3", "help -canonical"};
constexpr std::array names{"identity", "colon composition", "canonical curry", "metadata help"};
constexpr int repetitions = 9, count = 1000, warmup = 100;
void validate(seed::Runtime& runtime, seed::Ptr value, std::size_t case_index) {
    if (case_index == 3 ? (!runtime.is_literal(value) || runtime.as_literal(value)->bytes.find("arguments: f g x") == std::string::npos)
                       : (!runtime.is_number(value) || runtime.as_number(value)->integer != 3))
        throw std::runtime_error("shell benchmark result changed");
}
}
int main() {
    try {
        SetEnvironmentVariableW(L"HIRARI_STATE", L"-");
        SetEnvironmentVariableW(L"HIRARI_PATH", L"");
        std::array<std::array<std::vector<double>, sources.size()>, 2> samples;
        std::array<std::array<std::uint64_t, sources.size()>, 2> chains{};
        for (int repeat = 0; repeat < repetitions; ++repeat) {
            for (std::size_t offset = 0; offset < sources.size(); ++offset) {
                auto index = (offset + repeat) % sources.size();
                for (std::size_t mode_offset = 0; mode_offset < 2; ++mode_offset) {
                    auto mode = (mode_offset + repeat) % 2;
                    Host host; hirari::Shell shell(host); auto& runtime = shell.seed();
                    runtime.deferred_substitution(mode != 0);
                    shell.execute("identity := x() x");
                    shell.execute("compose := f (g x) : x : g : f");
                    shell.execute("canonical := f() (g() (x() (f (g x))))");
                    seed::Ptr value = nullptr;
                    for (int n = 0; n < warmup; ++n) value = shell.execute(sources[index]);
                    validate(runtime, value, index);
                    runtime.deferred_stats() = {};
                    auto begin = std::chrono::steady_clock::now();
                    for (int n = 0; n < count; ++n) value = shell.execute(sources[index]);
                    auto elapsed = std::chrono::duration<double, std::micro>(
                        std::chrono::steady_clock::now() - begin).count();
                    validate(runtime, value, index);
                    if (!host.errors.empty()) throw std::runtime_error(host.errors);
                    const auto fused = runtime.deferred_stats().chains;
                    if (fused != (mode && (index == 1 || index == 2) ? count : 0))
                        throw std::runtime_error("unexpected shell fusion coverage");
                    samples[mode][index].push_back(elapsed / count);
                    chains[mode][index] += fused;
                }
            }
        }
        std::cout << "Actual Shell::execute, parsing + reduction + result presentation; " << repetitions
                  << " rotating fresh shells/workload/mode, " << count << " calls/sample, " << warmup << " warmup\n";
        for (std::size_t mode = 0; mode < 2; ++mode) for (std::size_t index = 0; index < sources.size(); ++index) {
            auto& values = samples[mode][index]; std::sort(values.begin(), values.end());
            std::cout << (mode ? "deferred " : "literal ") << names[index]
                      << ": median=" << values[values.size()/2] << " us/call, min=" << values.front()
                      << ", max=" << values.back() << ", fused_chains=" << chains[mode][index] << '\n';
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
