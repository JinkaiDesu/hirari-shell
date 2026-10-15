#include "hirari/values.hpp"
#include "hirari/value_wire.hpp"
#include <charconv>
#include <fstream>
#include "hirari/win32.hpp"
#include "hirari/world.hpp"
#include <stdexcept>
#include <algorithm>
#include <optional>
namespace hirari {
namespace {
std::string symbol(seed::Runtime& r,seed::Ptr p){if(r.is_name(p))p=r.as_name(p)->target;if(!r.is_symbol(p))throw std::runtime_error("expected an identity");return r.as_symbol(p)->text;}
std::string text(seed::Runtime& r,seed::Ptr p){p=r.reduce(p);if(!r.is_literal(p))throw std::runtime_error("expected literal text");return r.as_literal(p)->bytes;}
std::vector<seed::Ptr> items(seed::Runtime& r,seed::Ptr p){if(r.is_unit(p))return {};return r.is_relation(p)?r.terms(r.as_node(p)):std::vector<seed::Ptr>{p};}
}
Values::Values(seed::Runtime& r,ProcessHost& p,Host& h):runtime_(r),processes_(p),host_(h) {
    r.define("provide",&Values::provide,this);r.define("data",&Values::data,this);
    r.define("capability",&Values::capability,this);
    // Output redirection is a { } stream operator; outside braces > compares.
    p.value_writer([this](seed::Runtime& rt, seed::Ptr value, const std::string& path, bool append) {
        write_value(rt, value, path, append);
    });
    r.set_collection_resolver(&Values::collection, this);
}
bool Values::collection(seed::Runtime& r,seed::Ptr value,std::vector<seed::Ptr>& out,seed::Ptr context) {
    auto& self=*static_cast<Values*>(context);
    if(!self.structured(value))return false;
    auto fields=r.terms(r.as_node(r.as_name(value)->target));
    if(symbol(r,fields[0])!="list")throw std::runtime_error("iteration requires a list, not a structured record or scalar");
    if(!r.is_name(fields[1]))throw std::runtime_error("invalid structured list");
    out=items(r,r.as_name(fields[1])->target);return true;
}
void Values::link(std::string identity, std::string description, LinkedProvider invoke) {
    if(identity.empty() || !invoke || providers_.contains(identity))throw std::runtime_error("capability already registered or invalid: "+identity);
    Provider provider{std::move(description),{},std::move(invoke)};provider.token=next_provider_++;
    providers_.emplace(std::move(identity),std::move(provider));
}
std::string Values::describe_foreign(seed::Ptr value) const {
    auto found=foreign_.find(value);if(found==foreign_.end())return {};
    const auto& ref=found->second;
    auto provider=providers_.find(ref.identity);
    return "provider: "+ref.identity+"\nstate: "+(provider!=providers_.end()&&provider->second.token==ref.provider?"live":"detached")+"\n";
}
std::string Values::description(std::string_view identity) const {
    auto found=providers_.find(std::string(identity));return found==providers_.end()?"":found->second.description;
}
seed::Ptr Values::capability(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto& self=*static_cast<Values*>(context);auto terms=r.terms(relation);
    if(terms.size()<2)throw std::runtime_error("capability list | register -IDENTITY EXECUTABLE [DESCRIPTION]");
    auto op=symbol(r,terms[1]);
    if(op=="register-library" && (terms.size()==4 || terms.size()==5)) {
        auto name=symbol(r,terms[2]);auto path=text(r,terms[3]);
        auto existing=self.providers_.find(name);
        if(existing!=self.providers_.end()) {
            if(existing->second.library && existing->second.executable==path)return r.arena().unit();
            throw std::runtime_error("capability identity already registered: "+name);
        }
        auto library=std::make_shared<NativeLibrary>(path);
        self.link(name,terms.size()==5?text(r,terms[4]):"Optional native capability library",[library,&self](const auto& args){return library->invoke(args,self.host_);});
        auto& provider=self.providers_.at(name);provider.library=library;provider.executable=path;
        return r.arena().unit();
    }
    if(op=="unregister" && terms.size()==3) {
        auto found=self.providers_.find(symbol(r,terms[2]));
        if(found==self.providers_.end())throw std::runtime_error("capability is not registered");
        if(found->second.invoke && !found->second.library)throw std::runtime_error("linked host capabilities cannot be unregistered");
        self.providers_.erase(found);return r.arena().unit();
    }
    if(op=="list" && terms.size()==2){Json out=Json::array();std::vector<std::string> names;for(const auto& [name,p]:self.providers_)names.push_back(name);std::sort(names.begin(),names.end());for(const auto& name:names){const auto& p=self.providers_.at(name);out.push_back({{"identity",name},{"transport",p.library?"library":p.invoke?"linked":"process"},{"description",p.description}});}return self.decode(pack_value(out));}
    if(op=="register" && (terms.size()==4 || terms.size()==5)){
        auto name=symbol(r,terms[2]);auto executable=text(r,terms[3]);auto found=self.providers_.find(name);
        if(found!=self.providers_.end() && (found->second.invoke || found->second.executable!=executable))throw std::runtime_error("capability identity already registered: "+name);
        if(found==self.providers_.end()) {
            Provider provider{terms.size()==5?text(r,terms[4]):"External typed provider: "+executable,executable,{}};
            provider.token=self.next_provider_++;self.providers_.emplace(name,std::move(provider));
        }
        return r.arena().unit();
    }
    throw std::runtime_error("capability list | register -IDENTITY EXECUTABLE [DESCRIPTION]");
}
Json Values::form(seed::Ptr p,std::vector<seed::Ptr> bound,std::vector<seed::Ptr> expanding,unsigned depth) {
    auto& r=runtime_;if(depth>64)throw std::runtime_error("form nesting exceeds 64");
    if(r.is_symbol(p) && std::find(bound.begin(),bound.end(),p)==bound.end()){
        if(auto value=r.binding(r.as_symbol(p))){if(std::find(expanding.begin(),expanding.end(),p)!=expanding.end())throw std::runtime_error("cyclic binding in form snapshot");expanding.push_back(p);return form(value,bound,expanding,depth+1);}
    }
    seed::Ptr binder=nullptr,body=nullptr;
    if(r.is_abstraction(p) && r.abstraction(p,binder,body)){
        bound.push_back(binder);return {{"kind","abstraction"},{"binder",form(binder,bound,expanding,depth+1)},{"body",form(body,bound,expanding,depth+1)}};
    }
    if(r.is_name(p))return encode(p,depth);
    if(r.is_relation(p)){Json items=Json::array();for(auto term:r.terms(r.as_node(p)))items.push_back(form(term,bound,expanding,depth+1));return {{"kind","product"},{"value",items}};}
    return encode(p,depth);
}
bool Values::structured(seed::Ptr p) const {
    auto& r=runtime_;if(!r.is_name(p) || !r.is_relation(r.as_name(p)->target))return false;
    auto terms=r.terms(r.as_node(r.as_name(p)->target));if(terms.size()!=2 || !r.is_name(terms[0]) || !r.is_symbol(r.as_name(terms[0])->target))return false;
    auto tag=r.as_symbol(r.as_name(terms[0])->target)->text;return tag=="record" || tag=="list" || tag=="integer" || tag=="float";
}
Json Values::encode(seed::Ptr p,unsigned depth) {
    auto& r=runtime_;if(depth>64)throw std::runtime_error("value nesting exceeds 64");
    if(auto found=foreign_.find(p);found!=foreign_.end()) {
        if(!active_provider_ || found->second.provider!=active_provider_)
            throw std::runtime_error("foreign identity belongs to a different or detached capability");
        return found->second.value;
    }
    if(r.is_unit(p))return {{"kind","unit"}};
    if(r.is_wildcard(p))throw std::runtime_error("anonymous wildcard needs a provider-specific interpretation before transport");
    if(r.is_number(p)) {
        if(auto found=physical_numbers_.find(p);found!=physical_numbers_.end())return found->second;
        auto number=r.as_number(p);bool integer=r.arena().is(p,seed::HostKind::Integer);
        return {{"kind",integer?"integer":"float"},{"value",integer?std::to_string(number->integer):Json(number->real).dump()}};
    }
    if(r.is_literal(p))return {{"kind","literal"},{"value",r.as_literal(p)->bytes}};
    if(r.is_symbol(p))return {{"kind","symbol"},{"value",r.as_symbol(p)->text}};
    if(r.is_cell(p))throw std::runtime_error("binding cells cannot cross a provider boundary; read the head first");
    if(r.is_name(p)) {
        auto target=r.as_name(p)->target;
        if(r.is_symbol(target)) {
            auto name=r.as_symbol(target)->text;
            if(name=="true" || name=="false")return {{"kind","boolean"},{"value",name=="true"}};
        }
        if(r.is_relation(target)) {
            auto terms=r.terms(r.as_node(target));
            if(terms.size()==2 && r.is_name(terms[0]) && r.is_symbol(r.as_name(terms[0])->target)) {
                auto tag=symbol(r,terms[0]);
                if(tag=="integer" || tag=="float")return {{"kind",tag},{"value",text(r,terms[1])}};
                if(tag=="list" && r.is_name(terms[1])) {
                    Json list=Json::array();for(auto item:items(r,r.as_name(terms[1])->target))list.push_back(encode(item,depth+1));
                    return {{"kind","list"},{"value",list}};
                }
                if(tag=="record" && r.is_name(terms[1])) {
                    Json fields=Json::object();auto fields_form=r.as_name(terms[1])->target;
                    for(auto field:items(r,fields_form)) {
                        if(!r.is_name(field) || !r.is_relation(r.as_name(field)->target))throw std::runtime_error("invalid record field");
                        auto pair=r.terms(r.as_node(r.as_name(field)->target));if(pair.size()!=2)throw std::runtime_error("invalid record pair");
                        fields[symbol(r,pair[0])]=encode(pair[1],depth+1);
                    }
                    return {{"kind","record"},{"value",fields}};
                }
            }
        }
        if(r.is_relation(target) || r.is_unit(target)) {
            Json list=Json::array();if(!r.is_unit(target))for(auto item:r.terms(r.as_node(target)))list.push_back(encode(item,depth+1));
            return {{"kind","list"},{"value",list}};
        }
        return {{"kind","name"},{"value",encode(target,depth+1)}};
    }
    Json terms=Json::array();for(auto item:r.terms(r.as_node(p)))terms.push_back(encode(item,depth+1));
    return {{"kind","product"},{"value",terms}};
}
seed::Ptr Values::decode(const Json& value,unsigned depth) {
    auto& r=runtime_;if(depth>64)throw std::runtime_error("provider value nesting exceeds 64");
    auto kind=value.at("kind").get<std::string>();
    if(kind=="foreign") {
        if(!active_provider_)throw std::runtime_error("foreign values require a live capability response");
        auto key=std::make_pair(active_provider_,value.dump());
        if(auto found=foreign_identities_.find(key);found!=foreign_identities_.end())return found->second;
        auto identity=r.arena().make_name(r.intern("<capability."+active_identity_+"#"+std::to_string(next_foreign_++)+">"));
        foreign_.emplace(identity,Foreign{value,active_provider_,active_identity_});
        foreign_identities_.emplace(std::move(key),identity);return identity;
    }
    if(kind=="unit")return r.arena().unit();
    if(kind=="literal")return r.arena().make_literal(value.at("value").get<std::string>());
    if(kind=="symbol")return r.intern(value.at("value").get<std::string>());
    if(kind=="boolean")return r.arena().make_name(r.intern(value.at("value").get<bool>()?"true":"false"));
    if(kind=="integer" || kind=="float") {
        auto numeric=value.at("value").get<std::string>();auto check=Json::parse(numeric);
        if(!check.is_number() || (kind=="integer" && !check.is_number_integer() && !check.is_number_unsigned()))throw std::runtime_error("invalid provider number");
        // Unsigned values beyond the signed physical substrate retain their
        // explicit provider representation instead of silently losing precision.
        if(kind=="integer" && check.is_number_unsigned() && check.get<std::uint64_t>()>9223372036854775807ULL)
            return r.arena().make_name(r.product({r.arena().make_name(r.intern(kind)),r.arena().make_literal(numeric)}));
        if(kind=="float" && numeric.find_first_of(".eE")==std::string::npos)numeric+=".0";
        auto result=r.arena().make_number(numeric);physical_numbers_.emplace(result,value);return result;
    }
    if(kind=="name")return r.arena().make_name(decode(value.at("value"),depth+1));
    if(kind=="record") {
        std::vector<seed::Ptr> fields;
        for(auto i=value.at("value").begin();i!=value.at("value").end();++i)fields.push_back(r.arena().make_name(r.product({r.arena().make_name(r.intern(i.key())),decode(i.value(),depth+1)})));
        return r.arena().make_name(r.product({r.arena().make_name(r.intern("record")),r.arena().make_name(r.product(fields))}));
    }
    if(kind=="list" || kind=="product") {
        std::vector<seed::Ptr> terms;for(auto item:value.at("value"))terms.push_back(decode(item,depth+1));
        // Received products are quoted data, never implicit executable source.
        if(kind=="list")return r.arena().make_name(r.product({r.arena().make_name(r.intern("list")),r.arena().make_name(r.product(terms))}));
        return r.arena().make_name(r.product(terms));
    }
    throw std::runtime_error("unknown provider value kind: "+kind);
}
seed::Ptr Values::record_get(seed::Ptr record,std::string_view key) {
    auto& r=runtime_;
    if(!structured(record))throw std::runtime_error("data get requires a record");
    // Return the original Ptr, preserving its identity within this shell.
    auto fields=r.terms(r.as_node(r.as_name(record)->target));
    if(symbol(r,fields[0])!="record")throw std::runtime_error("data get requires a record");
    auto form=r.as_name(fields[1])->target;
    for(auto field:items(r,form)) {
        auto pair=r.terms(r.as_node(r.as_name(field)->target));if(symbol(r,pair[0])==key)return pair[1];
    }
    throw std::runtime_error("record field not found: "+std::string(key));
}
seed::Ptr Values::provide(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto& self=*static_cast<Values*>(context);auto terms=r.terms(relation);
    if(terms.empty() || !r.is_symbol(terms[0]) || r.as_symbol(terms[0])->text!="provide")return relation;
    if(terms.size()<2)throw std::runtime_error("provide \"EXECUTABLE\" [STRUCTURED ARGUMENTS ...]");
    auto target=r.reduce(terms[1]);std::string executable;std::optional<Provider> provider;
    if(r.is_name(target)){
        auto name=symbol(r,target);auto found=self.providers_.find(name);
        if(found==self.providers_.end())throw std::runtime_error("capability not registered: "+name);
        provider=found->second;executable=provider->executable;
    }else executable=text(r,target);
    struct Scope { Values& self;std::uint64_t token;std::string identity;
        ~Scope(){self.active_provider_=token;self.active_identity_=std::move(identity);} };
    Scope scope{self,self.active_provider_,self.active_identity_};
    self.active_provider_=provider?provider->token:0;
    self.active_identity_=r.is_name(target)?symbol(r,target):executable;
    Json arguments=Json::array();
    for(std::size_t i=2;i<terms.size();++i)arguments.push_back(self.encode(r.reduce(terms[i])));
    if(provider && provider->invoke){std::vector<Json> args(arguments.begin(),arguments.end());return self.decode(provider->invoke(args));}
    ProcessResult input;input.out=Json{{"protocol",value_protocol},{"arguments",arguments}}.dump()+"\n";
    auto answer=self.processes_.execute(r,executable,{r.arena().make_literal("--values")},nullptr,std::move(input));
    ProcessResult result;if(!ProcessHost::unpack(r,answer,result))throw std::runtime_error("provider returned no process result");
    if(result.status!=0)throw std::runtime_error("provider exited "+std::to_string(result.status)+": "+result.err);
    auto envelope=Json::parse(result.out);
    if(envelope.at("protocol")!=value_protocol)throw std::runtime_error("provider protocol mismatch");
    return self.decode(envelope.at("value"));
}
seed::Ptr Values::data(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto& self=*static_cast<Values*>(context);auto terms=r.terms(relation);
    if(terms.empty() || !r.is_symbol(terms[0]) || r.as_symbol(terms[0])->text!="data")return relation;
    if(terms.size()<2)throw std::runtime_error("data bind|get|at|size|json|text|print|record|env|file|parse-json ...");
    auto operation=symbol(r,terms[1]);
    if(operation=="list") {
        // Keep live references intact instead of serializing through a provider.
        std::vector<seed::Ptr> entries;
        for(std::size_t i=2;i<terms.size();++i)entries.push_back(r.reduce(terms[i]));
        return r.arena().make_name(r.product({r.arena().make_name(r.intern("list")),r.arena().make_name(r.product(entries))}));
    }
    if(operation=="record") {
        // Same structured shape a provider record decodes to, but built from live
        // values: references and Names are kept, never serialized.
        if(terms.size()%2!=0)throw std::runtime_error("data record KEY VALUE [KEY VALUE ...]");
        std::vector<seed::Ptr> fields;std::vector<std::string> keys;
        for(std::size_t i=2;i<terms.size();i+=2) {
            auto key_form=r.reduce(terms[i]);
            auto key=r.is_literal(key_form)?r.as_literal(key_form)->bytes:symbol(r,key_form);
            if(key.empty()||std::find(keys.begin(),keys.end(),key)!=keys.end())throw std::runtime_error("data record keys must be unique and nonempty: "+key);
            keys.push_back(key);
            fields.push_back(r.arena().make_name(r.product({r.arena().make_name(r.intern(key)),r.reduce(terms[i+1])})));
        }
        return r.arena().make_name(r.product({r.arena().make_name(r.intern("record")),r.arena().make_name(r.product(fields))}));
    }
    if(terms.size()<3)throw std::runtime_error("data operation requires a value");
    if(operation=="env" && (terms.size()==3 || terms.size()==4)) {
        // The process environment: read by the shell and inherited by every
        // program it starts. A world entry receives its session context here.
        auto name=text(r,terms[2]);
        if(name.empty()||name.find('=')!=std::string::npos||name.find('\0')!=std::string::npos)throw std::runtime_error("invalid environment variable name");
#ifdef _WIN32
        auto key=win32::widen(name);
        if(terms.size()==4) {
            auto value=r.reduce(terms[3]);std::wstring wide;
            if(!r.is_unit(value)) {
                std::string spelling=r.is_literal(value)?r.as_literal(value)->bytes:r.is_number(value)?r.as_number(value)->spelling:
                    throw std::runtime_error("data env NAME VALUE requires text, a number, or () to unset");
                if(spelling.empty())throw std::runtime_error("use () to unset an environment variable");
                wide=win32::widen(spelling);
            }
            // _wputenv_s keeps the C runtime and the Win32 environment block in step.
            if(_wputenv_s(key.c_str(),wide.c_str())!=0)throw std::runtime_error("cannot set environment variable: "+name);
            return r.arena().unit();
        }
        SetLastError(ERROR_SUCCESS);DWORD size=GetEnvironmentVariableW(key.c_str(),nullptr,0);
        if(!size) {
            if(GetLastError()==ERROR_ENVVAR_NOT_FOUND)return r.arena().unit();
            return r.arena().make_literal("");
        }
        std::wstring buffer(size,L'\0');DWORD count=GetEnvironmentVariableW(key.c_str(),buffer.data(),size);
        if(count>=size)throw std::runtime_error("environment variable changed while reading: "+name);
        buffer.resize(count);return r.arena().make_literal(win32::narrow(buffer));
#else
        throw std::runtime_error("data env requires Windows");
#endif
    }
    if(operation=="file" && terms.size()==3) {
        auto path=text(r,terms[2]);
        std::ifstream input(text_path(path),std::ios::binary|std::ios::ate);
        if(!input)throw std::runtime_error("cannot read file: "+path);
        auto size=input.tellg();if(size<0||size>64*1024*1024)throw std::runtime_error("data file reads at most 64 MiB: "+path);
        std::string bytes(static_cast<std::size_t>(size),'\0');input.seekg(0);
        if(size&&!input.read(bytes.data(),size))throw std::runtime_error("cannot read complete file: "+path);
        return r.arena().make_literal(std::move(bytes));
    }
    if(operation=="print") {
        for(std::size_t i=2;i<terms.size();++i){
            auto value=r.reduce(terms[i]);
            if(i!=2)self.host_.write(" ");self.host_.write(r.print(value));
        }
        self.host_.write("\n");return r.arena().unit();
    }
    if (operation == "read" && terms.size() == 3) {
        std::ifstream input(text_path(text(r, terms[2])), std::ios::binary);
        if (!input) throw std::runtime_error("cannot read structured value file");
        Json envelope; input >> envelope;
        if (envelope.at("protocol") != value_protocol) throw std::runtime_error("value file protocol mismatch");
        input >> std::ws;if (!input.eof()) throw std::runtime_error("data read expects one value; appended files are streams of envelopes");
        return self.decode(envelope.at("value"));
    }
    if(operation=="bind" && terms.size()==4) {auto name=r.intern(symbol(r,terms[2]));auto value=r.reduce(terms[3]);r.bind(name,value);return value;}
    auto value=r.reduce(terms[2]);
    if(operation=="form" && terms.size()==3)return self.decode(pack_value(Json{{"kind","seed.form"},{"term",self.form(value)}}));
    if ((operation == "integer" || operation == "real") && terms.size() == 3) {
        ProcessResult process;std::string spelling;
        if (ProcessHost::unpack(r,value,process)) {
            if(process.status)throw std::runtime_error("cannot decode a failed process answer as a number");
            spelling=process.out;
        } else if(r.is_literal(value))spelling=r.as_literal(value)->bytes;
        else if(r.is_number(value))spelling=r.as_number(value)->spelling;
        else throw std::runtime_error("numeric decoding requires text, a number, or a process answer");
        auto first=spelling.find_first_not_of(" \t\r\n"),last=spelling.find_last_not_of(" \t\r\n");
        if(first==std::string::npos)throw std::runtime_error("empty numeric answer");
        spelling=spelling.substr(first,last-first+1);
        if(operation=="real" && spelling.find_first_of(".eE")==std::string::npos)spelling+=".0";
        auto number=r.arena().make_number(spelling);
        if(operation=="integer" && !r.arena().is(number,seed::HostKind::Integer))throw std::runtime_error("integer decoding does not truncate real values");
        return number;
    }
    if (operation == "kind" && terms.size() == 3) {
        auto encoded = self.encode(value);
        return r.arena().make_name(r.intern(encoded.at("kind").get<std::string>()));
    }
    if(operation=="parse-json" && terms.size()==3) {
        // JSON text becomes ordinary structured data (records, lists, numbers,
        // literals, booleans; null is Unit). It is never evaluated as Seed.
        std::string source;ProcessResult process;
        if(ProcessHost::unpack(r,value,process)) {
            if(process.status)throw std::runtime_error("cannot parse the output of a failed program (status "+std::to_string(process.status)+")");
            source=process.out;
        } else if(r.is_literal(value))source=r.as_literal(value)->bytes;
        else throw std::runtime_error("data parse-json requires JSON text or a program answer");
        if(source.starts_with("\xef\xbb\xbf"))source.erase(0,3);
        auto parsed=Json::parse(source,nullptr,false);
        if(parsed.is_discarded())throw std::runtime_error("invalid JSON text");
        return self.decode(pack_value(parsed));
    }
    if(operation=="get" && terms.size()==4) {
        auto key=text(r,terms[3]);ProcessResult process;
        // A program answer exposes its observable parts as fields.
        if(ProcessHost::unpack(r,value,process)) {
            if(key=="status")return r.arena().make_number(std::to_string(process.status));
            if(key=="stdout")return r.arena().make_literal(process.out);
            if(key=="stderr")return r.arena().make_literal(process.err);
            throw std::runtime_error("program answers have status, stdout and stderr fields");
        }
        return self.record_get(value,key);
    }
    if(operation=="at" && terms.size()==4) {
        auto index=r.print(r.reduce(terms[3]));if(index.empty() || index.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("data at index must be non-negative");
        if(!r.is_name(value))throw std::runtime_error("data at requires a list");auto target=r.as_name(value)->target;
        auto list=items(r,target);
        if(list.size()==2 && r.is_name(list[0]) && r.as_name(list[0])->target==r.intern("list") && r.is_name(list[1]))list=items(r,r.as_name(list[1])->target);
        auto n=std::stoull(index);
        if(n>=list.size())throw std::runtime_error("data index out of range");return list[static_cast<std::size_t>(n)];
    }
    if(operation=="size" && terms.size()==3) {
        if(!self.structured(value))throw std::runtime_error("data size requires a list or record");
        auto fields=r.terms(r.as_node(r.as_name(value)->target));auto kind=symbol(r,fields[0]);
        if(kind!="list" && kind!="record")throw std::runtime_error("data size requires a list or record");
        return self.decode(pack_value(items(r,r.as_name(fields[1])->target).size()));
    }
    if(operation=="json" && terms.size()==3)return r.arena().make_literal(unpack_value(self.encode(value)).dump());
    if(operation=="text" && terms.size()==3) {if(r.is_literal(value))return value;auto wire=self.encode(value);if(wire.at("kind")=="integer" || wire.at("kind")=="float")return r.arena().make_literal(wire.at("value").get<std::string>());return r.arena().make_literal(r.print(value));}
    throw std::runtime_error("invalid data operation or argument count");
}

void Values::write_value(seed::Runtime& r, seed::Ptr value, const std::string& path, bool append) {
    ProcessResult process;
    std::string bytes;
    if (ProcessHost::unpack(r, value, process)) bytes = process.out;
    else if (r.is_literal(value)) bytes = r.as_literal(value)->bytes;
    else bytes = Json{{"protocol",value_protocol},{"value",encode(value)}}.dump() + "\n";
    std::ofstream output(text_path(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();if (!output) throw std::runtime_error("cannot write output destination: " + path);
}
}
