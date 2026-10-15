#include "seed/native_stack.hpp"
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#endif
namespace seed {
bool native_stack_low(std::size_t reserve) noexcept {
#ifdef _WIN32
    // The reservation's lowest address; stacks grow downward toward it.
    thread_local ULONG_PTR low = 0;
    if (!low) { ULONG_PTR high = 0; GetCurrentThreadStackLimits(&low, &high); }
    auto here = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
    return here < low + reserve;
#else
    (void)reserve;
    return false;
#endif
}
}
