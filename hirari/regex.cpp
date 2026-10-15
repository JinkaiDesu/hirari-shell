// Host capability module. PCRE2 owns pattern parsing and matching; Seed does not.
#include "hirari/regex.hpp"
#include "hirari/value_wire.hpp"
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <memory>
#include <atomic>
#include <chrono>
#include <set>

namespace hirari {
namespace {
constexpr std::size_t output_limit=16*1024*1024;
std::string message(int code){PCRE2_UCHAR bytes[256];pcre2_get_error_message(code,bytes,sizeof(bytes));return reinterpret_cast<const char*>(bytes);}
std::string literal(const Json& v){if(!v.is_string())throw std::runtime_error("regex requires text or a regex.pattern value");return v.get<std::string>();}
struct Pattern {
    std::string source,flags;
    std::unique_ptr<pcre2_code,decltype(&pcre2_code_free)> code{nullptr,pcre2_code_free};
    Pattern(const Json& value,std::string options){
        if(value.is_object()){
            if(value.value("kind","")!="regex.pattern" || value.value("engine","")!="pcre2" || value.value("version",0)!=1)throw std::runtime_error("linked regex expects a version-1 PCRE2 pattern; choose its original provider for other dialects");
            source=literal(value.at("pattern"));if(options.empty())options=value.value("flags","");
        }else source=literal(value);
        flags=std::move(options);uint32_t bits=PCRE2_UTF|PCRE2_UCP;
        for(char flag:flags){switch(flag){case 'i':bits|=PCRE2_CASELESS;break;case 'm':case 'n':bits|=PCRE2_MULTILINE;break;case 's':bits|=PCRE2_DOTALL;break;case 'x':bits|=PCRE2_EXTENDED;break;case 'g':break;case 'q':bits|=PCRE2_LITERAL;break;default:throw std::runtime_error("unknown PCRE2 regex flag: "+std::string(1,flag));}}
        if(bits&PCRE2_LITERAL){if(bits&(PCRE2_EXTENDED|PCRE2_MULTILINE|PCRE2_DOTALL))throw std::runtime_error("literal regex flag q cannot combine with m/n/s/x");bits&=~PCRE2_UCP;}
        int error;PCRE2_SIZE offset;
        code.reset(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(source.data()),source.size(),bits,&error,&offset,nullptr));
        if(!code)throw std::runtime_error("PCRE2 pattern error at byte "+std::to_string(offset)+": "+message(error));
    }
};
Json run_regex(const std::vector<Json>& wire){
    std::vector<Json> args;for(const auto& value:wire)args.push_back(unpack_value(value));
    if(args.empty())throw std::runtime_error("regex match|capture|parse|replace|pattern|info ...");
    auto op=literal(args[0]);
    if(op=="info" && args.size()==1)return pack_value(Json{{"engine","pcre2"},{"version",PCRE2_MAJOR==10?"10."+std::to_string(PCRE2_MINOR):"unknown"},{"transport","linked"},{"unicode",true},{"offsets","UTF-8 bytes"}});
    if(args.size()<2)throw std::runtime_error("regex operation requires a pattern");
    if(op=="pattern"){
        if(args.size()>3)throw std::runtime_error("regex pattern PATTERN [FLAGS]");Pattern compiled(args[1],args.size()==3?literal(args[2]):"");
        return pack_value(Json{{"kind","regex.pattern"},{"engine","pcre2"},{"pattern",compiled.source},{"flags",compiled.flags},{"version",1}});
    }
    bool named_columns=op=="parse" && args.size()>2 && args[2].is_array();
    std::size_t text_index=op=="replace" || named_columns?3:2;
    if(args.size()<text_index+1 || args.size()>text_index+2)throw std::runtime_error("regex operation argument count mismatch");
    std::string flags=args.size()==text_index+2?literal(args.back()):"";
    Pattern pattern(args[1],flags);auto subject=literal(args[text_index]);
    auto ptr=reinterpret_cast<PCRE2_SPTR>(subject.data());
    auto context=std::unique_ptr<pcre2_match_context,decltype(&pcre2_match_context_free)>(pcre2_match_context_create(nullptr),pcre2_match_context_free);
    if(!context)throw std::runtime_error("cannot allocate regex match context");
    pcre2_set_match_limit(context.get(),1000000);pcre2_set_depth_limit(context.get(),10000);
    pcre2_set_heap_limit(context.get(),64*1024);
    if(op=="replace"){
        auto replacement=literal(args[2]);uint32_t options=PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
        if(pattern.flags.empty() || pattern.flags.find('g')!=std::string::npos)options|=PCRE2_SUBSTITUTE_GLOBAL;
        PCRE2_SIZE size=0;int result=pcre2_substitute(pattern.code.get(),ptr,subject.size(),0,options,nullptr,context.get(),reinterpret_cast<PCRE2_SPTR>(replacement.data()),replacement.size(),nullptr,&size);
        if(result<0 && result!=PCRE2_ERROR_NOMEMORY)throw std::runtime_error("PCRE2 replacement: "+message(result));
        if(size>output_limit)throw std::runtime_error("regex output exceeds 16 MiB");std::vector<PCRE2_UCHAR> output(size+1);size=output.size();
        result=pcre2_substitute(pattern.code.get(),ptr,subject.size(),0,options,nullptr,context.get(),reinterpret_cast<PCRE2_SPTR>(replacement.data()),replacement.size(),output.data(),&size);
        if(result<0)throw std::runtime_error("PCRE2 replacement: "+message(result));
        return pack_value(std::string(reinterpret_cast<const char*>(output.data()),size));
    }
    if(op!="match" && op!="capture" && op!="parse")throw std::runtime_error("unknown regex operation: "+op);
    auto data=std::unique_ptr<pcre2_match_data,decltype(&pcre2_match_data_free)>(pcre2_match_data_create_from_pattern(pattern.code.get(),nullptr),pcre2_match_data_free);
    if(!data)throw std::runtime_error("cannot allocate regex match state");
    uint32_t count=0;pcre2_pattern_info(pattern.code.get(),PCRE2_INFO_CAPTURECOUNT,&count);
    Json columns=Json::array(),names=Json::array();
    if(op=="parse"){
        if(named_columns){names=args[2];if(names.size()!=count)throw std::runtime_error("capture count differs from column names");}
        else {
            for(uint32_t i=1;i<=count;++i)names.push_back("group"+std::to_string(i));
            uint32_t name_count=0,entry_size=0;PCRE2_SPTR table=nullptr;
            pcre2_pattern_info(pattern.code.get(),PCRE2_INFO_NAMECOUNT,&name_count);pcre2_pattern_info(pattern.code.get(),PCRE2_INFO_NAMEENTRYSIZE,&entry_size);pcre2_pattern_info(pattern.code.get(),PCRE2_INFO_NAMETABLE,&table);
            for(uint32_t i=0;i<name_count;++i){auto entry=table+i*entry_size;uint32_t group=(entry[0]<<8)|entry[1];names[group-1]=reinterpret_cast<const char*>(entry+2);}
        }
        if(names.empty())throw std::runtime_error("regex parse requires capture groups");std::set<std::string> seen;
        for(const auto& name:names){auto key=literal(name);if(key.empty() || !seen.insert(key).second)throw std::runtime_error("duplicate or empty capture name");columns.push_back({{"name",key},{"oid",25},{"type","text"}});}
    }else columns=Json::array({Json{{"name","groups"},{"oid",3802},{"type","jsonb"}}});
    Json rows=Json::array();PCRE2_SIZE offset=0;uint32_t options=0;std::size_t output_size=0;
    while(offset<=subject.size()){
        int result=pcre2_match(pattern.code.get(),ptr,subject.size(),offset,options,data.get(),context.get());
        if(result==PCRE2_ERROR_NOMATCH){if(options && offset<subject.size()){++offset;while(offset<subject.size() && (static_cast<unsigned char>(subject[offset])&0xc0)==0x80)++offset;options=0;continue;}break;}
        if(result<0)throw std::runtime_error("PCRE2 matching: "+message(result));
        if(op=="match")return pack_value(true);
        auto spans=pcre2_get_ovector_pointer(data.get());Json groups=Json::array();
        for(uint32_t i=count?1:0;i<=(count?count:0);++i){auto first=spans[2*i],last=spans[2*i+1];if(first==PCRE2_UNSET)groups.push_back(nullptr);else groups.push_back(subject.substr(first,last-first));}
        Json row=Json::object();if(op=="parse")for(std::size_t i=0;i<names.size();++i)row[literal(names[i])]=groups[i];else row["groups"]=groups;
        output_size+=row.dump().size();if(output_size>output_limit || rows.size()>=100000)throw std::runtime_error("regex relation exceeds materialization limit");rows.push_back(std::move(row));
        if(!pattern.flags.empty() && pattern.flags.find('g')==std::string::npos)break;
        offset=spans[1];options=spans[0]==spans[1]?PCRE2_NOTEMPTY_ATSTART|PCRE2_ANCHORED:0;
    }
    if(op=="match")return pack_value(false);
    static std::atomic_uint64_t next=0;auto stamp=std::chrono::system_clock::now().time_since_epoch().count();
    return pack_value(Json{{"kind","relation"},{"id","pcre2:"+std::to_string(stamp)+":"+std::to_string(++next)},{"columns",columns},{"rows",rows},{"source",{{"provider","pcre2"},{"engine","pcre2"},{"engine_version","10."+std::to_string(PCRE2_MINOR)},{"pattern",pattern.source},{"flags",pattern.flags},{"lifetime","materialized"}}}});
}
}
void install_regex(Values& values){values.link("regex","Linked PCRE2 regex (Unicode).\nprovide -regex \"match\" PATTERN TEXT [FLAGS]\nOperations: match, capture, parse, replace, pattern, info. parse can infer named captures; replace uses PCRE2 $1/${name}. Flags: i/m/s/x/g/q (n aliases m). No external service connection required.",run_regex);}
}
