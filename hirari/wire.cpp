#include "hirari/wire.hpp"
#include <bit>
#include <stdexcept>
#include <type_traits>
namespace hirari::wire {
namespace {
struct Writer {
    Bytes bytes;
    void ensure(std::size_t n) {
        if(n>max_frame || bytes.size()>max_frame-n) throw std::runtime_error("wire frame too large");
    }
    template<class T> void number(T x) {
        ensure(sizeof(T)); using U=std::make_unsigned_t<T>; auto bits=static_cast<U>(x);
        for(std::size_t i=0;i<sizeof(T);++i) bytes.push_back(static_cast<std::uint8_t>(bits>>(8*i)));
    }
    void blob(std::span<const std::uint8_t> x) {
        ensure(4+x.size()); number(static_cast<std::int32_t>(x.size())); bytes.insert(bytes.end(),x.begin(),x.end());
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes; std::size_t pos=0;
    template<class T> T number() {
        if(sizeof(T)>bytes.size()-pos) throw std::runtime_error("truncated wire value");
        using U=std::make_unsigned_t<T>; U bits=0;
        for(std::size_t i=0;i<sizeof(T);++i) bits=static_cast<U>(bits | (static_cast<U>(bytes[pos++])<<(8*i)));
        return std::bit_cast<T>(bits);
    }
    std::span<const std::uint8_t> blob() {
        auto n=number<std::int32_t>();
        if(n<0 || static_cast<std::size_t>(n)>max_frame || static_cast<std::size_t>(n)>bytes.size()-pos)
            throw std::runtime_error("invalid wire blob length");
        auto x=bytes.subspan(pos,static_cast<std::size_t>(n)); pos+=x.size(); return x;
    }
};
void write(Writer& w,const Value& x) {
    w.number(static_cast<std::uint8_t>(x.kind));
    switch(x.kind) {
    case ValueKind::Null: break;
    case ValueKind::Boolean: w.number<std::uint8_t>(std::get<bool>(x.data)?1:0); break;
    case ValueKind::Int32: case ValueKind::Batch: w.number(std::get<std::int32_t>(x.data)); break;
    case ValueKind::Int64: case ValueKind::Object: case ValueKind::Type: w.number(std::get<std::int64_t>(x.data)); break;
    case ValueKind::Single: w.number(std::bit_cast<std::uint32_t>(std::get<float>(x.data))); break;
    case ValueKind::Double: w.number(std::bit_cast<std::uint64_t>(std::get<double>(x.data))); break;
    case ValueKind::String: {
        const auto& s=std::get<std::string>(x.data);
        w.blob({reinterpret_cast<const std::uint8_t*>(s.data()),s.size()}); break;
    }
    case ValueKind::Bytes: w.blob(std::get<Bytes>(x.data)); break;
    default: throw std::runtime_error("unknown wire value kind");
    }
}
Value read(Reader& r) {
    switch(static_cast<ValueKind>(r.number<std::uint8_t>())) {
    case ValueKind::Null:return Value::null();
    case ValueKind::Boolean:return Value::boolean(r.number<std::uint8_t>()!=0);
    case ValueKind::Int32:return Value::i32(r.number<std::int32_t>());
    case ValueKind::Int64:return Value::i64(r.number<std::int64_t>());
    case ValueKind::Single:return Value::f32(std::bit_cast<float>(r.number<std::uint32_t>()));
    case ValueKind::Double:return Value::f64(std::bit_cast<double>(r.number<std::uint64_t>()));
    case ValueKind::Object:return Value::object(r.number<std::int64_t>());
    case ValueKind::Type:return Value::type(r.number<std::int64_t>());
    case ValueKind::Batch:return Value::batch(r.number<std::int32_t>());
    case ValueKind::String:{auto x=r.blob();return Value::string(std::string(reinterpret_cast<const char*>(x.data()),x.size()));}
    case ValueKind::Bytes:{auto x=r.blob();return Value::bytes(Bytes(x.begin(),x.end()));}
    default:throw std::runtime_error("unknown wire value kind");
    }
}
}
Bytes pack(const Message& m) {
    if(m.values.size()>max_values) throw std::runtime_error("too many wire values");
    Writer w; w.number(static_cast<std::uint8_t>(m.kind));w.number(m.id);w.number(static_cast<std::int32_t>(m.values.size()));
    for(const auto& x:m.values)write(w,x);return std::move(w.bytes);
}
Message unpack(std::span<const std::uint8_t> body) {
    if(body.size()>max_frame)throw std::runtime_error("wire frame too large");
    Reader r{body};auto kind=static_cast<MessageKind>(r.number<std::uint8_t>());auto id=r.number<std::int64_t>();
    auto count=r.number<std::int32_t>();
    if(count<0||static_cast<std::size_t>(count)>max_values)throw std::runtime_error("invalid wire value count");
    Message m{kind,id,{}};m.values.reserve(static_cast<std::size_t>(count));
    for(std::int32_t i=0;i<count;++i)m.values.push_back(read(r));
    if(r.pos!=body.size())throw std::runtime_error("trailing bytes in wire frame");return m;
}
}
