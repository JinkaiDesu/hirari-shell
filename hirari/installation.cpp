#include "hirari/installation.hpp"
#include "hirari/win32.hpp"
#include <filesystem>
#include <set>
#include <cwctype>
#include <stdexcept>
#ifdef _WIN32
#include <ShlObj.h>
#endif
namespace hirari {
void initialize_installation() {
#ifdef _WIN32
    namespace fs = std::filesystem;
    auto get=[](const wchar_t* key) { DWORD n=GetEnvironmentVariableW(key,nullptr,0); if(!n)return std::wstring{}; std::wstring value(n,L'\0'); DWORD count=GetEnvironmentVariableW(key,value.data(),n); if(count>=n)throw std::runtime_error("environment changed during startup");value.resize(count);return value; };
    auto put=[](const wchar_t* key,const std::wstring& value) { if(!SetEnvironmentVariableW(key,value.c_str()))throw std::runtime_error("cannot establish installation environment"); };
    std::wstring module(32768,L'\0'); auto count=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!count||count>=module.size())throw std::runtime_error("cannot locate Hirari executable");module.resize(count);
    auto root=fs::path(module).parent_path().parent_path().parent_path();
    if(!fs::is_regular_file(root/L"Skunkworks"/L"Skunkworks.exe") || !fs::is_directory(root/L"System"/L"Commands"))return; // Independent shell outside an installation.
    root=fs::canonical(root);
    put(L"SKUNK_ROOT",root.wstring());put(L"SKUNK_SYSTEM",(root/L"System").wstring());
    put(L"SKUNK_TOOLCHAIN",(root/L"Toolchain").wstring());put(L"SKUNK_DATABASE",(root/L"Database").wstring());
    auto selected=get(L"SKUNK_USER"); fs::path user;
    if(!selected.empty()) {user=selected;if(!user.is_absolute())throw std::runtime_error("SKUNK_USER must be absolute");}
    else { PWSTR local=nullptr; if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))throw std::runtime_error("user storage unavailable");user=fs::path(local)/L"Skunkworks";CoTaskMemFree(local); }
    user=fs::absolute(user);fs::create_directories(user/L"Commands");fs::create_directories(user/L"Cache");fs::create_directories(user/L"Logs");
    put(L"SKUNK_USER",user.wstring());put(L"SKUNK_CACHE",(user/L"Cache").wstring());put(L"SKUNK_LOGS",(user/L"Logs").wstring());
    wchar_t temp[32768]; DWORD length=GetTempPathW(32768,temp);if(!length||length>=32768)throw std::runtime_error("OS temp unavailable");put(L"SKUNK_TEMP",std::wstring(temp,length));
    if(get(L"SKUNK_DATA").empty())put(L"SKUNK_DATA",(user/L"Data").wstring());
    if(get(L"HIRARI_STATE").empty())put(L"HIRARI_STATE",(user/L"Hirari"/L"vocabulary").wstring());
    auto compose=[](std::wstring prefix,const std::wstring& tail) {std::wstring out,joined=prefix+L";"+tail;std::set<std::wstring> seen;std::size_t start=0;while(start<=joined.size()){auto end=joined.find(L';',start);auto value=joined.substr(start,end==std::wstring::npos?end:end-start);auto key=value;for(auto& c:key)c=static_cast<wchar_t>(std::towlower(c));if(!value.empty()&&seen.insert(key).second){if(!out.empty())out+=L';';out+=value;}if(end==std::wstring::npos)break;start=end+1;}return out;};
    auto host=get(L"PATH");if(get(L"SKUNK_HOST_PATH")==L"0"){wchar_t system[32768];auto n=GetSystemDirectoryW(system,32768);if(!n||n>=32768)throw std::runtime_error("Windows system directory unavailable");host=std::wstring(system,n);}
    std::wstring paths=(root/L"System"/L"Hirari").wstring();
    for(auto relative:{L"Toolchain/msys64/usr/bin",L"Toolchain/python",L"Toolchain/git/cmd",L"Toolchain/dotnet",L"Toolchain/cmake/bin",L"Database/PostgreSQL/bin"})paths+=L";"+(root/relative).wstring();
    put(L"PATH",compose(paths,host));
    put(L"HIRARI_PATH",compose((user/L"Commands").wstring()+L";"+(root/L"System"/L"Commands").wstring(),get(L"HIRARI_PATH")));
#endif
}
}
