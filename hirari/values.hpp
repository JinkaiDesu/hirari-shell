#pragma once
#include "seed/seed.hpp"
#include "hirari/process.hpp"
#include "hirari/vendor/json.hpp"
#include <functional>
namespace hirari {
// Generic, inert value transport. Providers own the meaning of their records.
// The wire has no SQL, regex, Unity, or configuration operations.
using Json = nlohmann::json;
inline constexpr const char* value_protocol="hirari.values/1";
class Values {
public:
    Values(seed::Runtime&, ProcessHost&, Host&, std::function<bool(seed::Ptr)>);
    bool structured(seed::Ptr) const;
private:
    static seed::Ptr provide(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr data(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr route(seed::Runtime&, seed::Node*, seed::Ptr);
    Json encode(seed::Ptr, unsigned depth=0);
    seed::Ptr decode(const Json&, unsigned depth=0);
    seed::Ptr record_get(seed::Ptr, std::string_view);
    seed::Runtime& runtime_; ProcessHost& processes_; Host& host_;
    std::function<bool(seed::Ptr)> opaque_;
};
}
