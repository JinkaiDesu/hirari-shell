#pragma once
#include "seed/seed.hpp"
#include "hirari/process.hpp"
#include "hirari/vendor/json.hpp"
#include <functional>
#include "hirari/native_library.hpp"
namespace hirari {
// Generic, inert value transport. Providers own the meaning of their records.
// The wire has no SQL, regex, Unity, or configuration operations.
using Json = nlohmann::json;
inline constexpr const char* value_protocol="hirari.values/1";
class Values {
public:
    Values(seed::Runtime&, ProcessHost&, Host&);
    bool structured(seed::Ptr) const;
    bool opaque(seed::Ptr value) const { return foreign_.contains(value); }
    std::string describe_foreign(seed::Ptr) const;
    // Linked and external capabilities share the same typed argument contract.
    using LinkedProvider = std::function<Json(const std::vector<Json>&)>;
    void link(std::string identity, std::string description, LinkedProvider);
    std::string description(std::string_view identity) const;
    Json pack(seed::Ptr value) { return encode(value); }
    seed::Ptr unpack(const Json& value) { return decode(value); }
private:
    static bool collection(seed::Runtime&, seed::Ptr, std::vector<seed::Ptr>&, seed::Ptr);
    static seed::Ptr provide(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr data(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr route(seed::Runtime&, seed::Node*, seed::Ptr);
    static seed::Ptr capability(seed::Runtime&, seed::Node*, seed::Ptr);
    struct Provider { std::string description, executable; LinkedProvider invoke; std::shared_ptr<NativeLibrary> library; std::uint64_t token=0; };
    struct Foreign { Json value; std::uint64_t provider; std::string identity; };
    std::unordered_map<seed::Ptr,Foreign> foreign_;
    std::map<std::pair<std::uint64_t,std::string>,seed::Ptr> foreign_identities_;
    std::unordered_map<seed::Ptr,Json> physical_numbers_;
    std::uint64_t next_provider_=1,next_foreign_=1,active_provider_=0;
    std::string active_identity_;
    std::unordered_map<std::string, Provider> providers_;
    Json form(seed::Ptr, std::vector<seed::Ptr> bound = {}, std::vector<seed::Ptr> expanding = {}, unsigned depth=0);
    Json encode(seed::Ptr, unsigned depth=0);
    seed::Ptr decode(const Json&, unsigned depth=0);
    seed::Ptr record_get(seed::Ptr, std::string_view);
    seed::Runtime& runtime_; ProcessHost& processes_; Host& host_;
};
}
