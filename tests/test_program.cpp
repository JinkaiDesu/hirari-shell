#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "hirari/win32.hpp"
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
int wmain(int argc,wchar_t** argv) {
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);
    std::vector<std::string> a;for(int i=1;i<argc;++i)a.push_back(hirari::win32::narrow(argv[i]));
#else
int main(int argc,char** argv) {
    std::vector<std::string> a;for(int i=1;i<argc;++i)a.emplace_back(argv[i]);
#endif
    if(a.empty())return 2;
    if(a[0]=="emit"){for(std::size_t i=1;i<a.size();++i)std::cout<<a[i]<<'\n';return 0;}
    if(a[0]=="fail")return 7;
    if(a[0]=="sort"){
        std::vector<std::string> lines;std::string line;while(std::getline(std::cin,line))lines.push_back(line);
        std::sort(lines.begin(),lines.end());for(const auto& s:lines)std::cout<<s<<'\n';return 0;
    }
    if(a[0]=="relay"){std::cout<<std::cin.rdbuf();return 0;}
    if(a[0]=="environment"){
#ifdef _WIN32
        DWORD n=GetEnvironmentVariableW(L"SKUNK_WORLD",nullptr,0);std::wstring s(n,L'\0');
        if(n){auto c=GetEnvironmentVariableW(L"SKUNK_WORLD",s.data(),n);s.resize(c);std::cout<<hirari::win32::narrow(s)<<'\n';}
#endif
        return 0;
    }
    if(a[0]=="interactive") {
        std::cout<<"INPUT_READY\n"<<std::flush;std::string s;
        while(std::getline(std::cin,s)){if(s=="quit")break;std::cout<<"ECHO:"<<s<<'\n'<<std::flush;}return 0;
    }
    if(a[0]=="delayed") {
        std::cout<<"first\n"<<std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        std::cout<<"last\n"<<std::flush;return 0;
    }
    if(a[0]=="observe") {
        auto before=std::chrono::steady_clock::now();std::string s;
        if(std::getline(std::cin,s)) {
            auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-before).count();
            std::cout<<(elapsed<300?"LIVE\n":"BUFFERED\n")<<s<<'\n';
        }
        while(std::getline(std::cin,s))std::cout<<s<<'\n';return 0;
    }
    return 2;
}
