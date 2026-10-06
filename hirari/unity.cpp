#include "hirari/unity.hpp"
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
namespace hirari {
namespace {
using K=wire::ValueKind;using M=wire::MessageKind;using V=wire::Value;
template<class T>T number(std::string_view s) {
    T x{};auto out=std::from_chars(s.data(),s.data()+s.size(),x);
    if(out.ec!=std::errc{}||out.ptr!=s.data()+s.size()||s.empty())throw std::runtime_error("invalid or out-of-range Unity number");
    if constexpr(std::is_floating_point_v<T>)if(!std::isfinite(x))throw std::runtime_error("Unity number must be finite");
    return x;
}
bool symbol(seed::Runtime& r,seed::Ptr p,std::string_view s){return r.is_symbol(p)&&r.as_symbol(p)->text==s;}
}
UnityProgram::UnityProgram(seed::Runtime& r,Host& h):runtime_(r),client_(h){r.define("unity",&UnityProgram::invoke,this);}
const ForeignRef* UnityProgram::foreign(seed::Ptr p)const {
    auto i=foreign_.find(p);return i==foreign_.end()?nullptr:i->second.get();
}
seed::Ptr UnityProgram::opaque(seed::Runtime&,seed::Node*,seed::Ptr context) {
    return static_cast<ForeignRef*>(context)->identity;
}
seed::Ptr UnityProgram::invoke(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto terms=r.terms(relation);if(terms.empty()||!symbol(r,terms[0],"unity"))return relation;
    return static_cast<UnityProgram*>(context)->run(relation);
}
std::string UnityProgram::text(seed::Ptr p) {
    p=runtime_.reduce(p);
    if(runtime_.is_literal(p))return runtime_.as_literal(p)->bytes;
    throw std::runtime_error("Unity text requires a quoted string; Names are identities, not text");
}
std::vector<seed::Ptr> UnityProgram::list(seed::Ptr p) {
    p=runtime_.reduce(p);
    if(!runtime_.is_name(p)||foreign(p))throw std::runtime_error("Unity list requires a reified product :(...) or :()");
    p=runtime_.as_name(p)->target;
    if(runtime_.is_unit(p))return {};
    if(runtime_.is_relation(p))return runtime_.terms(runtime_.as_node(p));
    return {p};
}
wire::Value UnityProgram::value(seed::Ptr p,bool batch) {
    p=runtime_.reduce(p);
    if(auto* ref=foreign(p)) {
        if(ref->value.kind==K::Object||ref->value.kind==K::Type) {
            if(ref->released)throw std::runtime_error("Unity reference was released");
            if(ref->endpoint!=client_.endpoint()||ref->session!=client_.session())throw std::runtime_error("Unity reference belongs to a different server session");
        }
        if(ref->value.kind==K::Batch&&!batch)throw std::runtime_error("batch-ref is valid only inside unity batch");
        return ref->value;
    }
    if(runtime_.is_unit(p))return V::null();
    if(runtime_.is_literal(p))return V::string(runtime_.as_literal(p)->bytes);
    if(runtime_.is_name(p)) {
        auto target=runtime_.as_name(p)->target;
        if(symbol(runtime_,target,"true"))return V::boolean(true);
        if(symbol(runtime_,target,"false"))return V::boolean(false);
        throw std::runtime_error("Seed Name cannot be sent as Unity text or a forged reference");
    }
    if(runtime_.is_symbol(p)) {
        const auto& s=runtime_.as_symbol(p)->text;
        if(s.find_first_of(".eE")!=std::string::npos)return V::f64(number<double>(s));
        auto n=number<std::int64_t>(s);
        if(n>=std::numeric_limits<std::int32_t>::min()&&n<=std::numeric_limits<std::int32_t>::max())return V::i32(static_cast<std::int32_t>(n));
        return V::i64(n);
    }
    throw std::runtime_error("unsupported Unity argument; use a primitive or opaque reference");
}
wire::Value UnityProgram::reference(seed::Ptr p,bool type_only,bool batch) {
    auto v=value(p,batch);
    if((batch&&v.kind==K::Batch)||v.kind==K::Type||(!type_only&&v.kind==K::Object))return v;
    throw std::runtime_error(type_only?"Unity requires an opaque type reference":"Unity requires an opaque object or type reference");
}
seed::Ptr UnityProgram::box(V v) {
    if(v.kind==K::Null)return runtime_.arena().unit();
    if(v.kind==K::String)return runtime_.arena().make_literal(std::get<std::string>(v.data));
    if(v.kind==K::Boolean)return runtime_.arena().make_name(runtime_.intern(std::get<bool>(v.data)?"true":"false"));
    auto ref=std::make_unique<ForeignRef>();ref->value=std::move(v);
    ref->endpoint=client_.endpoint();ref->session=client_.session();
    std::string label;
    switch(ref->value.kind) {
    case K::Object:label="object";break;case K::Type:label="type";break;
    case K::Int32:label="i32="+std::to_string(std::get<std::int32_t>(ref->value.data));break;
    case K::Int64:label="i64="+std::to_string(std::get<std::int64_t>(ref->value.data));break;
    case K::Single:label="f32="+std::to_string(std::get<float>(ref->value.data));break;
    case K::Double:label="f64="+std::to_string(std::get<double>(ref->value.data));break;
    case K::Bytes:label="bytes";break;case K::Batch:label="batch-ref";break;
    default:throw std::runtime_error("unsupported Unity result kind");
    }
    auto token="<unity."+label+"#"+std::to_string(next_identity_++)+">";
    ref->identity=runtime_.arena().make_name(runtime_.intern(token));
    ref->program={&UnityProgram::opaque,ref.get()};
    runtime_.define(token,ref->program);auto p=ref->identity;foreign_.emplace(p,std::move(ref));return p;
}
seed::Ptr UnityProgram::result(const wire::Message& m) {
    if(m.values.empty())return runtime_.arena().unit();
    if(m.values.size()==1)return box(m.values[0]);
    std::vector<seed::Ptr> values;for(const auto& v:m.values)values.push_back(box(v));
    return runtime_.arena().make_name(runtime_.product(std::move(values)));
}
void UnityProgram::released(const V& v) {
    if(v.kind!=K::Object&&v.kind!=K::Type)return;
    for(auto& [p,ref]:foreign_) {
        (void)p;
        if(ref->endpoint==client_.endpoint()&&ref->session==client_.session()&&ref->value.kind==v.kind&&ref->value.data==v.data)ref->released=true;
    }
}
wire::Message UnityProgram::operation(const std::vector<seed::Ptr>& a,bool batch) {
    if(a.empty()||!runtime_.is_symbol(a[0]))throw std::runtime_error("Unity operation must be a command word");
    const auto& op=runtime_.as_symbol(a[0])->text;
    auto need=[&](std::size_t n){if(a.size()!=n)throw std::runtime_error("wrong argument count for unity "+op);};
    auto target=[&](std::size_t i,bool type=false){return reference(a.at(i),type,batch);};
    wire::Message m{M::Find,0,{}};
    if(op=="find"||op=="type") {need(2);m.kind=op=="find"?M::Find:M::Type;m.values={V::string(text(a[1]))};}
    else if(op=="component") {need(3);m.kind=M::Component;auto object=target(1);if(object.kind==K::Type)throw std::runtime_error("component needs an object");m.values={object,target(2,true)};}
    else if(op=="get") {need(3);m.kind=M::Get;m.values={target(1),V::string(text(a[2]))};}
    else if(op=="set") {need(4);m.kind=M::Set;m.values={target(1),V::string(text(a[2])),value(a[3],batch)};}
    else if(op=="release") {need(2);m.kind=M::Release;m.values={target(1)};}
    else if(op=="new"||op=="new-exact"||op=="call"||op=="call-exact"||op=="call-generic"||op=="call-generic-exact") {
        bool construct=op=="new"||op=="new-exact";bool exact=op.find("exact")!=std::string::npos;bool generic=op.find("generic")!=std::string::npos;
        std::size_t index=construct?2:3;
        if(a.size()<index+(exact?1:0)+(generic?1:0))throw std::runtime_error("missing unity "+op+" arguments");
        m.kind=construct?M::New:M::Call;m.values.push_back(target(1,construct));
        if(!construct) {
            m.values.push_back(V::string(text(a[2])));
            auto types=generic?list(a[index++]):std::vector<seed::Ptr>{};
            m.values.push_back(V::i32(static_cast<std::int32_t>(types.size())));
            for(auto t:types)m.values.push_back(reference(t,true,batch));
        }
        if(exact) {
            auto signature=list(a[index++]);m.values.push_back(V::i32(static_cast<std::int32_t>(signature.size())));
            for(auto t:signature)m.values.push_back(reference(t,true,batch));
            if(signature.size()!=a.size()-index)throw std::runtime_error("Unity signature and argument counts differ");
        }else m.values.push_back(V::i32(-1));
        for(;index<a.size();++index)m.values.push_back(value(a[index],batch));
    }else throw std::runtime_error("unknown Unity operation: "+op);
    return m;
}
seed::Ptr UnityProgram::run(seed::Node* relation) {
    auto a=runtime_.terms(relation);
    if(a.size()<2||!runtime_.is_symbol(a[1]))throw std::runtime_error("usage: unity connect/status/type/find/new/get/set/call/component/release/batch");
    auto op=runtime_.as_symbol(a[1])->text;
    if(op=="status") {
        if(a.size()!=2)throw std::runtime_error("unity status takes no arguments");
        return runtime_.arena().make_literal(client_.connected()?"connected "+client_.endpoint()+" "+client_.session():"disconnected "+client_.endpoint());
    }
    if(op=="connect"||op=="disconnect") {
        if(a.size()!=2)throw std::runtime_error("unity connect/disconnect takes no arguments");
        if(op=="connect")client_.connect();else client_.disconnect();return runtime_.arena().unit();
    }
    if(op=="bind") {
        if(a.size()!=4||!runtime_.is_name(a[2])||!runtime_.is_symbol(runtime_.as_name(a[2])->target))
            throw std::runtime_error("usage: unity bind :symbol (unity operation ...)");
        // Explicit host binding of an evaluated capability value. Native ':=' / inverse '='
        // deliberately keeps its existing lazy RHS representation.
        auto identity=runtime_.reduce(a[3]);
        runtime_.bind(runtime_.as_symbol(runtime_.as_name(a[2])->target),identity);
        return identity;
    }
    if(op=="at") {
        if(a.size()!=4)throw std::runtime_error("usage: unity at result index");
        auto entries=list(a[2]);auto p=runtime_.reduce(a[3]);
        std::string s=runtime_.is_symbol(p)?runtime_.as_symbol(p)->text:text(p);
        auto n=number<std::size_t>(s);if(n>=entries.size())throw std::runtime_error("Unity result index out of range");return entries[n];
    }
    if(op=="i32"||op=="i64"||op=="f32"||op=="f64"||op=="batch-ref"||op=="bytes"||op=="string") {
        if(a.size()!=3)throw std::runtime_error("unity primitive helper takes one argument");
        auto p=runtime_.reduce(a[2]);std::string s=runtime_.is_symbol(p)?runtime_.as_symbol(p)->text:text(p);
        if(op=="string")return runtime_.arena().make_literal(text(a[2]));
        if(op=="i32")return box(V::i32(number<std::int32_t>(s)));
        if(op=="i64")return box(V::i64(number<std::int64_t>(s)));
        if(op=="f32")return box(V::f32(number<float>(s)));
        if(op=="f64")return box(V::f64(number<double>(s)));
        if(op=="batch-ref") {auto n=number<std::int32_t>(s);if(n<0)throw std::runtime_error("negative batch reference");return box(V::batch(n));}
        wire::Bytes bytes;
        if(s.size()%2)throw std::runtime_error("unity bytes requires even-length hexadecimal text");
        for(std::size_t i=0;i<s.size();i+=2) {
            unsigned n=0;auto parsed=std::from_chars(s.data()+i,s.data()+i+2,n,16);
            if(parsed.ec!=std::errc{}||parsed.ptr!=s.data()+i+2)throw std::runtime_error("invalid hexadecimal bytes");bytes.push_back(static_cast<std::uint8_t>(n));
        }
        return box(V::bytes(std::move(bytes)));
    }
    client_.connect();
    if(op=="batch") {
        if(a.size()!=3)throw std::runtime_error("usage: unity batch :((operation ...) (operation ...))");
        wire::Message batch{M::Batch,0,{}};std::vector<wire::Message> operations;
        for(auto p:list(a[2])) {
            if(!runtime_.is_relation(p))throw std::runtime_error("batch operation must be grouped");
            auto m=operation(runtime_.terms(runtime_.as_node(p)),true);
            for(const auto& v:m.values)if(v.kind==K::Batch&&static_cast<std::size_t>(std::get<std::int32_t>(v.data))>=operations.size())throw std::runtime_error("batch-ref must refer to an earlier operation");
            batch.values.push_back(V::bytes(wire::pack(m)));operations.push_back(std::move(m));
        }
        auto reply=client_.request(std::move(batch));
        if(reply.values.size()!=operations.size())throw std::runtime_error("invalid Unity batch result count");
        for(std::size_t i=0;i<operations.size();++i)if(operations[i].kind==M::Release) {
            auto v=operations[i].values[0];if(v.kind==K::Batch)v=reply.values[static_cast<std::size_t>(std::get<std::int32_t>(v.data))];released(v);
        }
        std::vector<seed::Ptr> values;for(const auto& v:reply.values)values.push_back(box(v));
        return runtime_.arena().make_name(runtime_.product(std::move(values)));
    }
    a.erase(a.begin());auto m=operation(a,false);auto reply=client_.request(m);
    if(m.kind==M::Release)released(m.values[0]);return result(reply);
}
}
