#include "hirari/vendor/json.hpp"
#include <string>
using J=nlohmann::json;
extern "C" __declspec(dllexport) const char* capability_invoke_v1(const char* bytes,std::size_t length,std::size_t* size) {
    static std::string result;static int counter=0;
    auto args=J::parse(bytes,bytes+length).at("arguments");
    auto op=args.at(0).at("value").get<std::string>();J value;
    if(op=="next")value={{"kind","integer"},{"value",std::to_string(++counter)}};
    else if(op=="count")value={{"kind","integer"},{"value",std::to_string(counter)}};
    else if(op=="foreign")value={{"kind","foreign"},{"value",{{"identity","fixture-1"}}}};
    else if(op=="echo")value=args.at(1);
    else value={{"kind","unit"}};
    result=J{{"protocol","hirari.values/1"},{"value",value}}.dump();*size=result.size();return result.data();
}
