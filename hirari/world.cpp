#include "hirari/world.hpp"
#include "hirari/win32.hpp"
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
namespace hirari {
namespace {
std::string trim(std::string s){auto a=s.find_first_not_of(" \t\r\n");if(a==std::string::npos)return {};return s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);}
std::string uncomment(const std::string& s) {
    char quote=0;bool escape=false;
    for(std::size_t i=0;i<s.size();++i) {
        char c=s[i];if(escape){escape=false;continue;}
        if(quote=='"'&&c=='\\'){escape=true;continue;}
        if(quote){if(c==quote)quote=0;continue;}
        if(c=='"'||c=='\'')quote=c;else if(c=='#')return s.substr(0,i);
    }return s;
}
void utf8(std::string& out,unsigned x) {
    if(x>0x10ffff||(x>=0xd800&&x<=0xdfff))throw std::runtime_error("invalid TOML Unicode escape");
    if(x<0x80)out+=static_cast<char>(x);
    else if(x<0x800){out+=static_cast<char>(0xc0|(x>>6));out+=static_cast<char>(0x80|(x&63));}
    else if(x<0x10000){out+=static_cast<char>(0xe0|(x>>12));out+=static_cast<char>(0x80|((x>>6)&63));out+=static_cast<char>(0x80|(x&63));}
    else{out+=static_cast<char>(0xf0|(x>>18));out+=static_cast<char>(0x80|((x>>12)&63));out+=static_cast<char>(0x80|((x>>6)&63));out+=static_cast<char>(0x80|(x&63));}
}
std::string string_value(const std::string& s) {
    if(s.size()<2||(s.front()!='"'&&s.front()!='\'')||s.back()!=s.front())throw std::runtime_error("world descriptor requires single-line quoted TOML strings");
    std::string out;char quote=s.front();
    for(std::size_t i=1;i+1<s.size();++i) {
        char c=s[i];if(c==quote||static_cast<unsigned char>(c)<32&&c!='\t')throw std::runtime_error("invalid TOML string");
        if(quote=='"'&&c=='\\') {
            if(++i>=s.size()-1)throw std::runtime_error("incomplete TOML escape");
            switch(s[i]) {
            case '"':out+='"';break;case '\\':out+='\\';break;case 'b':out+='\b';break;case 't':out+='\t';break;
            case 'n':out+='\n';break;case 'f':out+='\f';break;case 'r':out+='\r';break;
            case 'u':case 'U': {
                std::size_t count=s[i]=='u'?4:8;if(i+count>=s.size()-1)throw std::runtime_error("incomplete TOML Unicode escape");
                unsigned x=0;auto result=std::from_chars(s.data()+i+1,s.data()+i+count+1,x,16);
                if(result.ec!=std::errc{}||result.ptr!=s.data()+i+count+1)throw std::runtime_error("invalid TOML Unicode escape");
                utf8(out,x);i+=count;break;
            }
            default:throw std::runtime_error("invalid TOML escape");
            }
        }else out+=c;
    }return out;
}
std::optional<std::wstring> environment() {
#ifdef _WIN32
    SetLastError(ERROR_SUCCESS);DWORD n=GetEnvironmentVariableW(L"HIRARI_WORLD",nullptr,0);
    if(!n){if(GetLastError()==ERROR_ENVVAR_NOT_FOUND)return std::nullopt;return std::wstring{};}
    std::wstring s(n,L'\0');DWORD count=GetEnvironmentVariableW(L"HIRARI_WORLD",s.data(),n);
    if(count>=n)throw std::runtime_error("world environment changed concurrently");s.resize(count);return s;
#else
    return std::nullopt;
#endif
}
void environment(const std::optional<std::wstring>& value) {
#ifdef _WIN32
    if(!SetEnvironmentVariableW(L"HIRARI_WORLD",value?value->c_str():nullptr))throw std::runtime_error("cannot set HIRARI_WORLD");
#else
    (void)value;throw std::runtime_error("world environment requires Windows");
#endif
}
}
std::string path_text(const std::filesystem::path& path){auto s=path.u8string();return std::string(reinterpret_cast<const char*>(s.data()),s.size());}
std::filesystem::path text_path(const std::string& s) {
    if(s.find('\0')!=std::string::npos)throw std::runtime_error("NUL is invalid in a path");
#ifdef _WIN32
    return std::filesystem::path(win32::widen(s));
#else
    return std::filesystem::path(s);
#endif
}
std::string read_source(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);if(!in)throw std::runtime_error("cannot read source: "+path_text(path));
    auto n=in.tellg();if(n<0||n>16*1024*1024)throw std::runtime_error("source file exceeds 16 MiB");
    std::string s(static_cast<std::size_t>(n),'\0');in.seekg(0);
    if(n&&!in.read(s.data(),n))throw std::runtime_error("cannot read complete source file");
    if(s.starts_with("\xef\xbb\xbf"))s.erase(0,3);
#ifdef _WIN32
    (void)win32::widen(s);
#endif
    return s;
}
WorldDescriptor read_world(const std::filesystem::path& input) {
    namespace fs=std::filesystem;
    WorldDescriptor world;world.root=fs::canonical(input);
    if(!fs::is_directory(world.root))throw std::runtime_error("world root is not a directory");
    auto source=read_source(world.root/"world.toml");if(source.size()>65536)throw std::runtime_error("world.toml exceeds 64 KiB");
    std::map<std::string,std::string> values;std::string section,line;std::map<std::string,bool> sections;std::istringstream lines(source);
    while(std::getline(lines,line)) {
        line=trim(uncomment(line));if(line.empty())continue;
        if(line.front()=='[') {
            if(line!="[world]"&&line!="[entry]")throw std::runtime_error("unsupported world.toml table");
            section=line.substr(1,line.size()-2);if(!sections.emplace(section,true).second)throw std::runtime_error("duplicate world.toml table");continue;
        }
        auto equals=line.find('=');if(section.empty()||equals==std::string::npos)throw std::runtime_error("invalid world.toml assignment");
        auto key=section+"."+trim(line.substr(0,equals));
        if(key!="world.name"&&key!="world.version"&&key!="entry.program")throw std::runtime_error("unsupported world.toml key: "+key);
        if(!values.emplace(key,trim(line.substr(equals+1))).second)throw std::runtime_error("duplicate world.toml key: "+key);
    }
    if(values.size()!=3)throw std::runtime_error("world.toml requires world.name, world.version and entry.program");
    if(values.at("world.version")!="1")throw std::runtime_error("unsupported world version; expected 1");
    world.name=string_value(values.at("world.name"));if(trim(world.name).empty())throw std::runtime_error("world name is empty");
    auto program=text_path(string_value(values.at("entry.program")));
    if(program.empty()||program.has_root_path())throw std::runtime_error("entry.program must be relative to its world root");
    world.entry=fs::canonical(world.root/program);
    auto rel=world.entry.lexically_relative(world.root);
    if(rel.empty()||rel.has_root_path()||*rel.begin()==".."||!fs::is_regular_file(world.entry))throw std::runtime_error("world entry must be a file inside its world root");
    return world;
}
void WorldHost::open(const std::filesystem::path& root,const std::function<void(const std::filesystem::path&)>& entry) {
    auto descriptor=read_world(root);auto previous=current_;auto cwd=std::filesystem::current_path();auto env=environment();
    try {
        std::filesystem::current_path(descriptor.root);environment(descriptor.root.wstring());current_=descriptor;entry(descriptor.entry);
    }catch(...) {
        auto original=std::current_exception();current_=std::move(previous);
        try{std::filesystem::current_path(cwd);environment(env);}catch(const std::exception& error){throw std::runtime_error(std::string("world launch failed and rollback failed: ")+error.what());}
        std::rethrow_exception(original);
    }
}
std::string WorldHost::status()const{return current_?"open "+current_->name+" "+path_text(current_->root):"stopped";}
}
