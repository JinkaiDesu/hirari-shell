#include "seed/native_stack.hpp"
#include <cstdint>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#else
#include <pthread.h>
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
    // The stack's lowest address; it grows downward toward it.
    thread_local std::uintptr_t low = 0;
    if (!low) {
        pthread_attr_t attributes;
        if (pthread_getattr_np(pthread_self(), &attributes)) return false;
        void* address = nullptr;
        std::size_t size = 0;
        const bool known = pthread_attr_getstack(&attributes, &address, &size) == 0;
        pthread_attr_destroy(&attributes);
        if (!known) return false;
        low = reinterpret_cast<std::uintptr_t>(address);
    }
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) < low + reserve;
#endif
}
}
