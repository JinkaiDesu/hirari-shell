#pragma once
#include <cstddef>
namespace seed {
// True when fewer than `reserve` bytes of the calling thread's native stack
// remain. A host safety boundary for nested evaluation, not a reduction rule.
bool native_stack_low(std::size_t reserve) noexcept;
}
