#include "hirari/vocabulary.hpp"
#include "hirari/win32.hpp"
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <algorithm>
namespace hirari {
namespace {
constexpr std::size_t maximum = 16 * 1024 * 1024, records = 10000;
void integer(std::string& out, std::uint64_t n) { for (int i=0;i<8;++i) out.push_back(static_cast<char>(n>>(i*8))); }
std::uint64_t read_integer(std::string_view in, std::size_t& p) {
    if (in.size()-p < 8) throw std::runtime_error("truncated vocabulary");
    std::uint64_t n=0; for(int i=0;i<8;++i) n|=static_cast<std::uint64_t>(static_cast<unsigned char>(in[p++]))<<(i*8); return n;
}
void string(std::string& out, std::string_view text) { integer(out,text.size()); out.append(text); }
std::string string(std::string_view in,std::size_t& p) {
    auto n=read_integer(in,p); if(n>maximum || n>in.size()-p)throw std::runtime_error("invalid vocabulary length");
    auto s=std::string(in.substr(p,static_cast<std::size_t>(n)));p+=static_cast<std::size_t>(n);return s;
}
void encode(seed::Runtime& r,seed::Ptr p,std::string& out,const std::function<bool(seed::Ptr)>& opaque,unsigned depth=0) {
    if(depth>64 || out.size()>maximum)throw std::runtime_error("definition exceeds persistence budget");
    if(opaque(p))throw std::runtime_error("live CLR identities cannot be persisted; save a program that finds or constructs the object");
    if(r.is_unit(p)){out+='U';return;}
    if(r.is_wildcard(p)){out+='W';return;}
    if(r.is_number(p)){out+='D';string(out,r.as_number(p)->spelling);return;}
    if(r.is_symbol(p)){out+='S';string(out,r.as_symbol(p)->text);return;}
    if(r.is_literal(p)){out+='L';string(out,r.as_literal(p)->bytes);return;}
    if(r.is_name(p)){out+='N';encode(r,r.as_name(p)->target,out,opaque,depth+1);return;}
    if(r.is_cell(p)){out+='C';string(out,r.as_cell(p)->symbol->text);return;}
    if(!r.is_relation(p))throw std::runtime_error("unsupported persistent value");
    out+='P';auto terms=r.terms(r.as_node(p));integer(out,terms.size());
    for(auto term:terms)encode(r,term,out,opaque,depth+1);
}
seed::Ptr decode(seed::Runtime& r,std::string_view in,std::size_t& p,unsigned depth=0) {
    if(depth>64 || p>=in.size())throw std::runtime_error("invalid persistent structure");
    switch(in[p++]) {
    case 'U':return r.arena().unit();
    case 'W':return r.arena().make_wildcard();
    case 'D':return r.arena().make_number(string(in,p));
    case 'S':return r.intern(string(in,p));
    case 'L':return r.arena().make_literal(string(in,p));
    case 'N':return r.arena().make_name(decode(r,in,p,depth+1));
    case 'C':return r.ensure_cell(r.intern(string(in,p)));
    case 'P':{auto n=read_integer(in,p);if(n>records)throw std::runtime_error("persistent product too large");std::vector<seed::Ptr> terms;for(std::uint64_t i=0;i<n;++i)terms.push_back(decode(r,in,p,depth+1));return r.product(std::move(terms));}
    default:throw std::runtime_error("unknown persistent structure tag");
    }
}
seed::Ptr decode(seed::Runtime& r,const std::string& in) {
    if(in.empty())return nullptr;std::size_t p=0;auto value=decode(r,in,p);
    if(p!=in.size())throw std::runtime_error("trailing persistent structure");return value;
}
std::map<std::string,Vocabulary::Revision> current(const std::vector<Vocabulary::Revision>& revisions) {
    std::map<std::string,Vocabulary::Revision> result;for(const auto& v:revisions)result[v.name]=v;return result;
}
}
Vocabulary::Vocabulary(std::filesystem::path directory):directory_(std::filesystem::absolute(std::move(directory))) {}
std::vector<Vocabulary::Revision> Vocabulary::read() const {
    auto file=directory_/"vocabulary.bin";std::error_code error;
    if(!std::filesystem::exists(file,error)){if(error)throw std::runtime_error(error.message());return {};}
    if(std::filesystem::file_size(file)>maximum)throw std::runtime_error("vocabulary exceeds 16 MiB budget");
    std::ifstream stream(file,std::ios::binary);std::string in((std::istreambuf_iterator<char>(stream)),{});
    if(stream.bad() || !in.starts_with("HVOC1\n"))throw std::runtime_error("unsupported or damaged vocabulary; existing file preserved");
    std::size_t p=6;std::vector<Revision> result;
    while(p<in.size()){
        if(result.size()>=records)throw std::runtime_error("vocabulary revision budget exceeded");
        Revision v;v.number=read_integer(in,p);v.name=string(in,p);v.annotation=string(in,p);v.form=string(in,p);
        if(v.number!=result.size()+1 || v.name.empty())throw std::runtime_error("invalid vocabulary revision");result.push_back(std::move(v));
    }return result;
}
void Vocabulary::load(seed::Runtime& r) {
    std::vector<std::pair<seed::Symbol*,seed::Ptr>> values;
    for(const auto& [name,v]:current(read())){
        auto symbol=r.intern(name);if(r.registry().find(symbol))throw std::runtime_error("vocabulary cannot replace built-in: "+name);
        values.emplace_back(symbol,decode(r,v.form));
    }
    for(const auto& [name,form]:values)r.bind(name,form);
}
void Vocabulary::append(Revision v) {
    std::filesystem::create_directories(directory_);
#ifdef _WIN32
    auto lockpath=directory_/"write.lock";HANDLE lock=INVALID_HANDLE_VALUE;
    for(int attempt=0;attempt<100;++attempt){lock=CreateFileW(lockpath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(lock!=INVALID_HANDLE_VALUE)break;if(GetLastError()!=ERROR_SHARING_VIOLATION)break;Sleep(10);}
    if(lock==INVALID_HANDLE_VALUE)throw std::runtime_error("vocabulary is busy or not writable");
    struct Close{HANDLE h;~Close(){CloseHandle(h);}}guard{lock};
#endif
    auto revisions=read();if(revisions.size()>=records)throw std::runtime_error("vocabulary history is full; export it before resetting");
    v.number=revisions.size()+1;revisions.push_back(std::move(v));std::string out="HVOC1\n";
    for(const auto& item:revisions){integer(out,item.number);string(out,item.name);string(out,item.annotation);string(out,item.form);}
    if(out.size()>maximum)throw std::runtime_error("vocabulary exceeds 16 MiB budget");
    auto file=directory_/"vocabulary.bin",temp=directory_/"vocabulary.pending";
    {std::ofstream stream(temp,std::ios::binary|std::ios::trunc);stream.write(out.data(),static_cast<std::streamsize>(out.size()));stream.flush();if(!stream)throw std::runtime_error("cannot write vocabulary snapshot");}
#ifdef _WIN32
    HANDLE pending=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(pending==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot flush vocabulary snapshot");
    bool flushed=FlushFileBuffers(pending)!=0;CloseHandle(pending);
    if(!flushed || !MoveFileExW(temp.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cannot commit vocabulary; previous snapshot preserved");
#else
    std::filesystem::rename(temp,file);
#endif
}
void Vocabulary::save(seed::Runtime& r,const std::string& name,const std::string& annotation,const std::function<bool(seed::Ptr)>& opaque) {
    auto symbol=r.intern(name);if(r.registry().find(symbol))throw std::runtime_error("cannot persist a built-in override");
    auto form=r.binding(symbol);if(!form)throw std::runtime_error("binding not found: "+name);
    std::string encoded;encode(r,form,encoded,opaque);append({0,name,annotation,std::move(encoded)});
}
void Vocabulary::restore(seed::Runtime& r,const std::string& name,std::uint64_t number) {
    if(r.registry().find(r.intern(name)))throw std::runtime_error("cannot restore a built-in override");
    for(const auto& v:read())if(v.name==name && v.number==number){auto form=decode(r,v.form);append({0,name,v.annotation,v.form});r.bind(r.intern(name),form);return;}
    throw std::runtime_error("vocabulary revision not found");
}
void Vocabulary::forget(seed::Runtime& r,const std::string& name) {
    if(r.registry().find(r.intern(name)))throw std::runtime_error("cannot forget a built-in");
    append({0,name,{}, {}});r.bind(r.intern(name),nullptr);
}
std::string Vocabulary::describe(seed::Runtime& r,const std::string& name,bool history) const {
    auto revisions=read();std::ostringstream out;
    auto display=[&](const Revision& v){if(!name.empty() && v.name!=name)return;out<<v.number<<" "<<v.name<<" := ";auto form=decode(r,v.form);out<<(form?r.print(form):"<removed>");if(!v.annotation.empty())out<<" # "<<v.annotation;out<<'\n';};
    if(history){for(const auto& v:revisions)display(v);}else {for(const auto& [key,v]:current(revisions))if(!v.form.empty())display(v);}return out.str();
}
std::string Vocabulary::annotation(const std::string& name) const {auto values=current(read());auto i=values.find(name);return i==values.end()?std::string{}:i->second.annotation;}
}
