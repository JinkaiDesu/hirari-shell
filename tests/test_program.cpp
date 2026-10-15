#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include "hirari/win32.hpp"
#include "hirari/value_wire.hpp"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
int wmain(int argc,wchar_t** argv) {
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);_setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> a;for(int i=1;i<argc;++i)a.push_back(hirari::win32::narrow(argv[i]));
#else
int main(int argc,char** argv) {
    std::vector<std::string> a;for(int i=1;i<argc;++i)a.emplace_back(argv[i]);
#endif
    std::filesystem::path stream_ack;
#ifdef _WIN32
    DWORD ack_size=GetEnvironmentVariableW(L"HIRARI_STREAM_ACK",nullptr,0);
    if(ack_size){std::wstring value(ack_size,L'\0');value.resize(GetEnvironmentVariableW(L"HIRARI_STREAM_ACK",value.data(),ack_size));stream_ack=value;}
#else
    if(auto value=std::getenv("HIRARI_STREAM_ACK"))stream_ack=value;
#endif
    if(a.empty())return 2;
    if(a[0]=="job-worker") {
        auto request=hirari::Json::parse(std::cin);auto id=request.at("request_id");
        auto emit=[&](const char* kind,hirari::Json value){std::cout<<hirari::Json{{"protocol","hirari.jobs/1"},{"request_id",id},{"kind",kind},{"value",value}}.dump()<<'\n'<<std::flush;};
        if(a.at(1)=="wrong-id")id="obsolete-request";
        emit("progress",0.5);std::cerr<<"worker diagnostic\n"<<std::flush;
        if(a.at(1)=="slow")std::this_thread::sleep_for(std::chrono::seconds(30));
        if(a.at(1)=="failure")return 7;
        if(a.at(1)=="error") {
            std::cout<<hirari::Json{{"protocol","hirari.jobs/1"},{"request_id",id},{"kind","error"},{"message","preparation failed"}}.dump()<<'\n'<<std::flush;
            return 0;
        }
        emit("result",request.at("parameters"));
        if(a.at(1)=="duplicate")emit("result",request.at("parameters"));
        return 0;
    }
    if(a[0]=="--values") {
        auto request=hirari::Json::parse(std::cin);
        auto arguments=request.at("arguments");
        auto value=arguments.empty()?hirari::pack_value(hirari::Json{{"empty",""},{"null",nullptr},{"large",9223372036854775807LL},{"flag",false},{"items",hirari::Json::array({"日本",1})},{"source",{{"snapshot","neutral-fixture"}}}}):arguments[0];
        std::cout<<hirari::Json{{"protocol","hirari.values/1"},{"value",value}}.dump()<<'\n';return 0;
    }
    if(a[0]=="emit"){for(std::size_t i=1;i<a.size();++i)std::cout<<a[i]<<'\n';return 0;}
    if(a[0]=="fail")return 7;
#ifdef _WIN32
    // Supervision fixtures: identity, live output, duration and exit status.
    // Liveness without a handshake: the producer stamps wall-clock time, then
    // pauses; a consumer reached through a live pipe reads the stamp at once.
    if(a[0]=="stamped") {
        auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::cout<<"first "<<now<<'\n'<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        std::cout<<"last\n"<<std::flush;return 0;
    }
    if(a[0]=="latency") {
        std::string s;
        if(std::getline(std::cin,s) && s.starts_with("first ")) {
            auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::cout<<(now-std::stoll(s.substr(6))<300?"LIVE\n":"BUFFERED\n");
        }
        while(std::getline(std::cin,s))std::cout<<s<<'\n';return 0;
    }
    // A producer that ignores write failures, as a SIGPIPE-less program may.
    if(a[0]=="stubborn") {
        for(;;){std::cout<<"x\n"<<std::flush;std::cout.clear();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    }
    if(a[0]=="head-one") {std::string s;if(std::getline(std::cin,s))std::cout<<s<<'\n';return 0;}
    if(a[0]=="pid-sleep"&&a.size()==2) {
        std::cout<<GetCurrentProcessId()<<'\n'<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoll(a[1])));
        std::cout<<"done\n"<<std::flush;return 0;
    }
    if(a[0]=="exit-code"&&a.size()==2){std::cerr<<"exiting "<<a[1]<<'\n';return std::stoi(a[1]);}
    if(a[0]=="print-env"&&a.size()==2) {
        auto name=hirari::win32::widen(a[1]);DWORD n=GetEnvironmentVariableW(name.c_str(),nullptr,0);
        if(!n){std::cout<<"<unset>\n";return 0;}
        std::wstring s(n,L'\0');s.resize(GetEnvironmentVariableW(name.c_str(),s.data(),n));std::cout<<hirari::win32::narrow(s)<<'\n';return 0;
    }
#endif
    if(a[0]=="startup-environment") {
        bool inherited=std::getenv("HIRARI_STARTUP") || std::getenv("HIRARI_STARTUP_STATUS");
        std::cout<<(inherited?"STARTUP_ENV_LEAKED\n":"STARTUP_ENV_CLEARED\n");return inherited?7:0;
    }
    if(a[0]=="bulk") {std::string chunk(65536,'x');for(int i=0;i<320;++i)std::cout.write(chunk.data(),chunk.size());return 0;}
    if(a[0]=="sort"){
        std::vector<std::string> lines;std::string line;while(std::getline(std::cin,line))lines.push_back(line);
        std::sort(lines.begin(),lines.end());for(const auto& s:lines)std::cout<<s<<'\n';return 0;
    }
    if(a[0]=="relay"){std::cout<<std::cin.rdbuf();return 0;}
    if(a[0]=="relay-live"){std::string s;while(std::getline(std::cin,s))std::cout<<s<<'\n'<<std::flush;return 0;}
    if(a[0]=="environment"){
#ifdef _WIN32
        DWORD n=GetEnvironmentVariableW(L"HIRARI_WORLD",nullptr,0);std::wstring s(n,L'\0');
        if(n){auto c=GetEnvironmentVariableW(L"HIRARI_WORLD",s.data(),n);s.resize(c);std::cout<<hirari::win32::narrow(s)<<'\n';}
#endif
        return 0;
    }
    if(a[0]=="interactive") {
        std::cout<<"INPUT_READY\n"<<std::flush;std::string s;
        while(std::getline(std::cin,s)){if(s=="quit")break;std::cout<<"ECHO:"<<s<<'\n'<<std::flush;}return 0;
    }
    if(a[0]=="delayed") {
        if(!stream_ack.empty()){std::error_code error;std::filesystem::remove(stream_ack,error);}
        std::cout<<"first\n"<<std::flush;
        if(stream_ack.empty())std::this_thread::sleep_for(std::chrono::milliseconds(400));
        else {
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            while(!std::filesystem::exists(stream_ack) && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if(!std::filesystem::exists(stream_ack)){std::cerr<<"consumer did not acknowledge live output\n";return 8;}
        }
        std::cout<<"last\n"<<std::flush;return 0;
    }
    if(a[0]=="observe") {
        auto before=std::chrono::steady_clock::now();std::string s;
        if(std::getline(std::cin,s)) {
            auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count();
            if(!stream_ack.empty()){
                std::ofstream ack(stream_ack);ack<<"first output received\n";ack.close();
                if(!ack){std::cerr<<"could not acknowledge live output\n";return 9;}
            }
            std::cout<<(!stream_ack.empty() || elapsed<300?"LIVE\n":"BUFFERED\n")<<s<<'\n';
        }
        while(std::getline(std::cin,s))std::cout<<s<<'\n';return 0;
    }
    return 2;
}
