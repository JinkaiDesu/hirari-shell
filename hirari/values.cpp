#include "hirari/values.hpp"
#include "hirari/value_wire.hpp"
#include <charconv>
#include <fstream>
#include "hirari/win32.hpp"
#include "hirari/world.hpp"
#include <stdexcept>
namespace hirari {
namespace {
std::string symbol(seed::Runtime& r,seed::Ptr p){if(r.is_name(p))p=r.as_name(p)->target;if(!r.is_symbol(p))throw std::runtime_error("expected an identity");return r.as_symbol(p)->text;}
std::string text(seed::Runtime& r,seed::Ptr p){p=r.reduce(p);if(!r.is_literal(p))throw std::runtime_error("expected literal text");return r.as_literal(p)->bytes;}
std::vector<seed::Ptr> items(seed::Runtime& r,seed::Ptr p){if(r.is_unit(p))return {};return r.is_relation(p)?r.terms(r.as_node(p)):std::vector<seed::Ptr>{p};}
}
Values::Values(seed::Runtime& r,ProcessHost& p,Host& h,std::function<bool(seed::Ptr)> opaque):runtime_(r),processes_(p),host_(h),opaque_(std::move(opaque)) {
    r.define("provide",&Values::provide,this);r.define("data",&Values::data,this);
    r.define(">", &Values::route, this);r.define(">>", &Values::route, this);
}
bool Values::structured(seed::Ptr p) const {
    auto& r=runtime_;if(!r.is_name(p) || !r.is_relation(r.as_name(p)->target))return false;
    auto terms=r.terms(r.as_node(r.as_name(p)->target));if(terms.size()!=2 || !r.is_name(terms[0]) || !r.is_symbol(r.as_name(terms[0])->target))return false;
    auto tag=r.as_symbol(r.as_name(terms[0])->target)->text;return tag=="record" || tag=="list" || tag=="integer" || tag=="float";
}
Json Values::encode(seed::Ptr p,unsigned depth) {
    auto& r=runtime_;if(depth>64)throw std::runtime_error("value nesting exceeds 64");
    if(opaque_(p))throw std::runtime_error("live runtime identity cannot cross a structured process boundary");
    if(r.is_unit(p))return {{"kind","unit"}};
    if(r.is_wildcard(p))throw std::runtime_error("anonymous wildcard needs a provider-specific interpretation before transport");
    if(r.is_number(p)) {
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
        return r.arena().make_number(numeric);
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
    auto& r=runtime_;auto encoded=encode(record);
    if(encoded.at("kind")!="record")throw std::runtime_error("data get requires a record");
    // Return the original Ptr, preserving its identity within this shell.
    auto fields=r.terms(r.as_node(r.as_name(record)->target));auto form=r.as_name(fields[1])->target;
    for(auto field:items(r,form)) {
        auto pair=r.terms(r.as_node(r.as_name(field)->target));if(symbol(r,pair[0])==key)return pair[1];
    }
    throw std::runtime_error("record field not found: "+std::string(key));
}
seed::Ptr Values::provide(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto& self=*static_cast<Values*>(context);auto terms=r.terms(relation);
    if(terms.empty() || !r.is_symbol(terms[0]) || r.as_symbol(terms[0])->text!="provide")return relation;
    if(terms.size()<2)throw std::runtime_error("provide \"EXECUTABLE\" [STRUCTURED ARGUMENTS ...]");
    auto executable=text(r,terms[1]);Json arguments=Json::array();
    for(std::size_t i=2;i<terms.size();++i)arguments.push_back(self.encode(r.reduce(terms[i])));
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
    if(terms.size()<3)throw std::runtime_error("data bind|get|at|size|json|text|print ...");
    auto operation=symbol(r,terms[1]);
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
    if(operation=="get" && terms.size()==4)return self.record_get(value,text(r,terms[3]));
    if(operation=="at" && terms.size()==4) {
        auto index=r.print(r.reduce(terms[3]));if(index.empty() || index.find_first_not_of("0123456789")!=std::string::npos)throw std::runtime_error("data at index must be non-negative");
        if(!r.is_name(value))throw std::runtime_error("data at requires a list");auto target=r.as_name(value)->target;
        auto list=items(r,target);
        if(list.size()==2 && r.is_name(list[0]) && r.as_name(list[0])->target==r.intern("list") && r.is_name(list[1]))list=items(r,r.as_name(list[1])->target);
        auto n=std::stoull(index);
        if(n>=list.size())throw std::runtime_error("data index out of range");return list[static_cast<std::size_t>(n)];
    }
    if(operation=="size" && terms.size()==3) {auto encoded=self.encode(value);if(encoded.at("kind")!="list" && encoded.at("kind")!="record")throw std::runtime_error("data size requires a list or record");return self.decode(pack_value(encoded.at("value").size()));}
    if(operation=="json" && terms.size()==3)return r.arena().make_literal(unpack_value(self.encode(value)).dump());
    if(operation=="text" && terms.size()==3) {if(r.is_literal(value))return value;auto wire=self.encode(value);if(wire.at("kind")=="integer" || wire.at("kind")=="float")return r.arena().make_literal(wire.at("value").get<std::string>());return r.arena().make_literal(r.print(value));}
    if(operation=="print") {for(std::size_t i=2;i<terms.size();++i){if(i!=2)self.host_.write(" ");self.host_.write(r.print(r.reduce(terms[i])));}self.host_.write("\n");return r.arena().unit();}
    throw std::runtime_error("invalid data operation or argument count");
}

seed::Ptr Values::route(seed::Runtime& r,seed::Node* relation,seed::Ptr context) {
    auto& self = *static_cast<Values*>(context);
    auto terms = r.terms(relation);
    auto routing = [&](seed::Ptr p) {return r.is_symbol(p) &&
        (r.as_symbol(p)->text == ">" || r.as_symbol(p)->text == ">>");};
    std::size_t at = terms.size();
    for (std::size_t i = 0; i < terms.size(); ++i) if (routing(terms[i])) {
        if (at != terms.size()) throw std::runtime_error("group multiple output destinations explicitly");
        at = i;
    }
    if (at == terms.size()) return relation;
    seed::Ptr producer, destination;
    if (at == 0 && terms.size() == 3) {producer = terms[1];destination = terms[2];}
    else if (at > 0 && at + 2 == terms.size()) {
        producer = r.product(std::vector<seed::Ptr>(terms.begin(), terms.begin() + at));
        destination = terms.back();
    } else throw std::runtime_error("use VALUE > DESTINATION or VALUE >> DESTINATION");
    bool append = r.as_symbol(terms[at])->text == ">>";
    destination = r.reduce(destination);
    if (!r.is_literal(destination)) {
        if (append) destination = r.compose(destination, r.arena().make_name(r.intern("append")));
        return r.reduce(r.compose(destination, producer));
    }
    auto path = r.as_literal(destination)->bytes;
    if (auto result = self.processes_.redirect(r, producer, path, append)) return result;
    auto value = r.reduce(producer);
    ProcessResult process;
    std::string bytes;
    if (ProcessHost::unpack(r, value, process)) bytes = process.out;
    else if (r.is_literal(value)) bytes = r.as_literal(value)->bytes;
    else bytes = Json{{"protocol",value_protocol},{"value",self.encode(value)}}.dump() + "\n";
    std::ofstream output(text_path(path), std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();if (!output) throw std::runtime_error("cannot write output destination: " + path);
    return ProcessHost::unpack(r, value, process) ? value : r.arena().unit();
}
}
