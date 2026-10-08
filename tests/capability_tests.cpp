#include "hirari/shell.hpp"
#include "hirari/value_wire.hpp"
#include <iostream>
#include <stdexcept>
using namespace hirari;
struct TestHost:Host {std::string out,err;void write(std::string_view s)override{out+=s;}void error(std::string_view s)override{err+=s;}};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
int main(){try{
    TestHost host;Shell shell(host);auto& r=shell.seed();
    auto run=[&](std::string_view source){auto answer=shell.execute(source);if(!answer)throw std::runtime_error(std::string(source)+": "+host.err);return answer;};
    auto text=[&](std::string_view source){auto answer=run(source);require(r.is_literal(answer),"expected text answer");return r.as_literal(answer)->bytes;};
    require(r.print(run("provide -regex \"match\" \"^\\\\d+$\" \"123\""))=="-true","linked match");
    require(r.print(run("provide -regex \"match\" \"^\\\\d+$\" \"oak\""))=="-false","negative linked match");
    require(text("provide -regex \"replace\" \"(?<number>[0-9]+)\" \"[${number}]\" \"x12 y34\"")=="x[12] y[34]","native named replacement");
    require(text("data get (data at (data get (provide -regex \"parse\" \"(?<word>[A-Za-z_]+)\" \"oak\") \"rows\") 0) \"word\"")=="oak","named capture relation");
    require(r.print(run("data size (data get (provide -regex \"capture\" \"a*\" \"b\") \"rows\")"))=="2","empty matches terminate and progress");
    require(r.print(run("provide -regex \"match\" \"^\\\\w+$\" \"日本\""))=="-true","Unicode matching");
    require(r.print(run("provide -regex \"match\" \"a.b\" \"before a.b after\" \"q\""))=="-true","literal pattern mode");
    run("data bind -pattern (provide -regex \"pattern\" \"(?<word>oak)\")");
    require(text("data get (data at (data get (provide -regex \"parse\" pattern \"oak\") \"rows\") 0) \"word\"")=="oak","engine-tagged reusable pattern");
    require(text("help -regex").find("PCRE2")!=std::string::npos,"nonexecuting capability help");
    run("cutoff := 5");run("small := n() (n < cutoff)");
    auto structure=Json::parse(text("data json (data form small)"));
    require(structure.at("kind")=="seed.form" && structure.at("term").at("kind")=="abstraction","abstraction snapshot");
    auto body=structure.at("term").at("body");require(body.at("kind")=="product" && body.dump().find("integer")!=std::string::npos,"captured literal preserved");
    run("data form (n() (exec \"missing_should_never_execute\" n))");require(host.err.empty(),"form snapshot executed body");
    require(!shell.execute("provide -missing \"match\""),"unknown provider accepted");host.err.clear();
    require(!shell.execute("provide -regex \"match\" \"(\" \"text\""),"malformed pattern accepted");host.err.clear();
    require(!shell.execute("capability register -regex \"other.exe\""),"linked capability overwritten");host.err.clear();
    require(!shell.execute("provide -regex \"match\" \"x\" \"x\" \"z\""),"unknown regex flag accepted");
    auto named=unpack_argument(Json{{"kind","name"},{"value",{{"kind","symbol"},{"value","n"}}}});require(named.at("$seed")=="name" && named.at("value").at("$seed")=="symbol","Name flattened during provider transport");
    std::cout<<"PASS linked regex, structural snapshots, identity transport, capability registration and help\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
