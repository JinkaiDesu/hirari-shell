#pragma once
#include "hirari/wire_generated.hpp"
#include <span>
#include <string>
#include <variant>
#include <vector>
namespace hirari::wire {
using Bytes = std::vector<std::uint8_t>;
struct Value {
    ValueKind kind = ValueKind::Null;
    std::variant<std::monostate, bool, std::int32_t, std::int64_t, float, double, std::string, Bytes> data;
    static Value null() { return {}; }
    static Value boolean(bool x) { return {ValueKind::Boolean,x}; }
    static Value i32(std::int32_t x) { return {ValueKind::Int32,x}; }
    static Value i64(std::int64_t x) { return {ValueKind::Int64,x}; }
    static Value f32(float x) { return {ValueKind::Single,x}; }
    static Value f64(double x) { return {ValueKind::Double,x}; }
    static Value string(std::string x) { return {ValueKind::String,std::move(x)}; }
    static Value object(std::int64_t x) { return {ValueKind::Object,x}; }
    static Value type(std::int64_t x) { return {ValueKind::Type,x}; }
    static Value bytes(Bytes x) { return {ValueKind::Bytes,std::move(x)}; }
    static Value batch(std::int32_t x) { return {ValueKind::Batch,x}; }
};
struct Message { MessageKind kind; std::int64_t id=0; std::vector<Value> values; };
// Body only; transport prepends its little-endian int32 length.
Bytes pack(const Message&);
Message unpack(std::span<const std::uint8_t>);
}
