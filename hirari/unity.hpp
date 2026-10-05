#pragma once
#include "hirari/unity_client.hpp"
#include "seed/seed.hpp"
#include <memory>
#include <unordered_map>
namespace hirari {
// Payload lives in the host Program context. Seed sees an opaque Name identity,
// never a handle encoded as an ordinary Literal or a new evaluator value kind.
struct ForeignRef {
    wire::Value value;
    std::string endpoint, session;
    bool released=false;
    seed::Program program;
    seed::Ptr identity=nullptr;
};
class UnityProgram {
public:
    UnityProgram(seed::Runtime&,Host&);
    UnityClient& client() noexcept { return client_; }
    const ForeignRef* foreign(seed::Ptr identity) const;
private:
    static seed::Ptr invoke(seed::Runtime&,seed::Node*,seed::Ptr);
    static seed::Ptr opaque(seed::Runtime&,seed::Node*,seed::Ptr);
    seed::Ptr run(seed::Node*);
    wire::Message operation(const std::vector<seed::Ptr>&,bool batch);
    wire::Value value(seed::Ptr,bool batch=false);
    wire::Value reference(seed::Ptr,bool type_only,bool batch);
    seed::Ptr box(wire::Value);
    seed::Ptr result(const wire::Message&);
    std::string text(seed::Ptr);
    std::vector<seed::Ptr> list(seed::Ptr);
    void released(const wire::Value&);
    seed::Runtime& runtime_;
    UnityClient client_;
    std::unordered_map<seed::Ptr,std::unique_ptr<ForeignRef>> foreign_;
    std::uint64_t next_identity_=1;
};
}
