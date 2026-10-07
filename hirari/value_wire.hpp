#pragma once
#include "hirari/vendor/json.hpp"
#include <stdexcept>
namespace hirari {
using Json=nlohmann::json;
inline Json pack_value(const Json& value) {
    if(value.is_null())return {{"kind","unit"}};
    if(value.is_boolean())return {{"kind","boolean"},{"value",value}};
    if(value.is_number_integer() || value.is_number_unsigned())return {{"kind","integer"},{"value",value.dump()}};
    if(value.is_number_float())return {{"kind","float"},{"value",value.dump()}};
    if(value.is_string())return {{"kind","literal"},{"value",value}};
    if(value.is_array()){Json items=Json::array();for(const auto& item:value)items.push_back(pack_value(item));return {{"kind","list"},{"value",items}};}
    Json fields=Json::object();for(auto i=value.begin();i!=value.end();++i)fields[i.key()]=pack_value(i.value());
    return {{"kind","record"},{"value",fields}};
}
inline Json unpack_value(const Json& value, unsigned depth=0) {
    if(depth>64)throw std::runtime_error("provider value nesting exceeds 64");
    auto kind=value.at("kind").get<std::string>();
    if(kind=="unit")return nullptr;
    if(kind=="literal" || kind=="boolean")return value.at("value");
    if(kind=="integer" || kind=="float")return Json::parse(value.at("value").get<std::string>());
    if(kind=="list"){Json items=Json::array();for(const auto& item:value.at("value"))items.push_back(unpack_value(item,depth+1));return items;}
    if(kind=="record"){Json fields=Json::object();for(auto i=value.at("value").begin();i!=value.at("value").end();++i)fields[i.key()]=unpack_value(i.value(),depth+1);return fields;}
    // Semantic Names/Symbols are never implicitly converted to text.
    throw std::runtime_error("this provider does not accept Seed "+kind+" as data; pass a literal or typed value");
}
}
