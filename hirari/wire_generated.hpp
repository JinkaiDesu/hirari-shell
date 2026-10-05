// Generated from Skunkworks Dev/Protocol/schema.json.
#pragma once
#include <cstdint>
namespace hirari::wire {
enum class MessageKind : std::uint8_t {
    Hello = 1,
    Ready = 2,
    Find = 10,
    Type = 11,
    Component = 12,
    Get = 13,
    Set = 14,
    Call = 15,
    New = 16,
    Release = 17,
    Batch = 18,
    Result = 20,
    Fault = 21,
};
enum class ValueKind : std::uint8_t {
    Null = 0,
    Boolean = 1,
    Int32 = 2,
    Int64 = 3,
    Single = 4,
    Double = 5,
    String = 6,
    Object = 7,
    Type = 8,
    Bytes = 9,
    Batch = 10,
};
inline constexpr std::uint32_t max_frame = 1048576;
inline constexpr std::uint32_t max_values = 4096;
inline constexpr std::uint32_t version = 1;
inline constexpr const char* protocol = "skunkworks.wire/1";
}
