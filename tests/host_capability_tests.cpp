#include "hirari/shell.hpp"
#include "hirari/win32.hpp"
#include "hirari/value_wire.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <fstream>
using namespace hirari;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::string quote(std::string_view s){std::string out="\"";for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';}
struct TestHost:Host{std::string out,err;void write(std::string_view s)override{out+=s;}void error(std::string_view s)override{err+=s;}};
int wmain(int argc,wchar_t** argv){
    auto root=std::filesystem::temp_directory_path()/(L"Hirari providers 日本 "+std::to_wstring(GetCurrentProcessId()));
    try{
        require(argc==3,"library and worker fixture paths required");std::filesystem::create_directories(root);
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"vocabulary").c_str());
        TestHost host;Shell shell(host);auto& r=shell.seed();
        auto run=[&](std::string s){auto p=shell.execute(s);if(!p)throw std::runtime_error(s+": "+host.err);return p;};
        auto json=[&](std::string s){return Json::parse(r.as_literal(run("data json ("+s+")"))->bytes);};
        auto rejects=[&](std::string s){require(!shell.execute(s),"invalid operation succeeded");host.err.clear();};
        auto library=quote(win32::narrow(argv[1]));
        run("capability register-library -fixture "+library+" \"Neutral test provider\"");
        run("capability register-library -fixture "+library);
        run("data print (provide -fixture \"next\")");
        require(json("provide -fixture \"count\"")==1,"data print evaluated a mutation twice");
        require(json("data list").empty(),"empty typed list");
        run("data bind -entries (provide -fixture \"echo\" (data list 1 2 3))");
        run("for -entry entries (provide -fixture \"next\")");
        require(json("provide -fixture \"count\"")==4,"typed collection wrapper leaked into iteration");
        require(json("apply -(provide -fixture) (data list \"echo\" 9223372036854775807)")==9223372036854775807LL,"apply typed list / integer precision");
        run("data bind -ref (provide -fixture \"foreign\")");
        run("data bind -refs (data list ref ref)");
        require(json("data size refs")==2,"foreign list size lost identity");
        require(run("provide -fixture \"echo\" (data at refs 0)")==run("ref"),"foreign response changed identity");run("provide -fixture \"echo\" refs");
        rejects("data json ref");rejects("vocab save -ref");rejects("provide -regex \"info\" ref");
        require(r.as_literal(run("help -fixture"))->bytes.find("Neutral test")!=std::string::npos,"provider help executed subject");
        run("capability unregister -fixture");run("capability register-library -fixture "+library);
        rejects("provide -fixture \"echo\" ref");

        const std::string worker=quote(win32::narrow(argv[2]));
        auto start=[&](std::string mode,Json options=Json::object()){
            auto file=root/L"options.json";std::ofstream out(file);out<<Json{{"protocol","hirari.values/1"},{"value",pack_value(options)}};out.close();
            auto result=json("provide -jobs \"start\" "+worker+" (data list \"job-worker\" "+quote(mode)+") (data read "+quote(path_text(file))+")");
            return result.at("id").get<std::string>();
        };
        auto status=[&](const std::string& id){return json("provide -jobs \"status\" "+quote(id));};
        auto wait=[&](const std::string& id){auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);for(;;){auto s=status(id);auto state=s.at("state");if(state=="succeeded"||state=="failed"||state=="cancelled"||state=="superseded")return s;require(std::chrono::steady_clock::now()<deadline,"job did not complete");std::this_thread::sleep_for(std::chrono::milliseconds(5));}};
        auto id=start("success",{{"structured",true},{"parameters",{{"large",9223372036854775807LL},{"text","日本"}}}});
        auto result=wait(id);require(result.at("state")=="succeeded"&&result.at("result").at("large")==9223372036854775807LL,"structured worker result lost precision");
        require(result.at("progress")==0.5&&result.at("stderr").get<std::string>().find("worker diagnostic")!=std::string::npos,("job progress/stderr: "+result.dump()).c_str());
        require(wait(start("failure")).at("exit_status")==7,"job exit status");
        require(wait(start("wrong-id",{{"structured",true}})).at("state")=="failed","stale worker request accepted");
        require(wait(start("duplicate",{{"structured",true}})).at("state")=="failed","duplicate worker result accepted");
        require(wait(start("error",{{"structured",true}})).at("error")=="preparation failed","worker error message lost");
        require(wait(start("slow",{{"timeout_ms",50}})).at("error")=="job timed out","job timeout");
        auto cancel=start("slow");run("provide -jobs \"cancel\" "+quote(cancel));require(wait(cancel).at("state")=="cancelled","job cancellation");
        auto old=start("slow",{{"slot","regenerate"},{"revision","1"}});
        auto fresh=start("success",{{"slot","regenerate"},{"revision","2"}});wait(fresh);
        require(status(old).at("current")==false&&status(old).at("result").is_null(),"obsolete completion exposed");
        auto finished=start("success",{{"slot","completed"}});wait(finished);
        wait(start("success",{{"slot","completed"}}));
        require(status(finished).at("current")==false&&status(finished).at("result").is_null(),"superseded completed result exposed");
        Json cached={{"cache_directory",path_text(root/L"cache")},{"cache_key","explicit-content-digest-v1"},{"structured",true}};
        auto first=wait(start("success",cached));require(!first.at("cache_hit").get<bool>(),"fresh job cache false hit");
        auto second=wait(start("success",cached));require(second.at("cache_hit")==true&&first.at("result")==second.at("result"),"reproducible cache miss");
        cached["parameters"]={{"change",true}};require(!wait(start("success",cached)).at("cache_hit").get<bool>(),"changed inputs reused cache");
        run("provide -jobs \"forget\" "+quote(id));rejects("provide -jobs \"status\" "+quote(id));
        std::cout<<"PASS single evaluation, typed collections, exact numbers, generic library identity, background progress/results/cancel/timeout/staleness/cache\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
    std::error_code error;std::filesystem::remove_all(root,error);return 0;
}
