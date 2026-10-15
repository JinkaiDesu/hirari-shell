#include "hirari/shell.hpp"

#include "hirari/win32.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
namespace {
using namespace hirari;

void require(bool x,const char* reason){if(!x)throw std::runtime_error(reason);}
template<class F>void rejects(F&& f,const char* message){try{f();}catch(const std::exception&){return;}throw std::runtime_error(message);}
struct MemoryHost:Host {
    std::string out,err;std::mutex mutex;
    void write(std::string_view s)override{std::lock_guard lock(mutex);out+=s;}
    void error(std::string_view s)override{std::lock_guard lock(mutex);err+=s;}
};
std::string quote(const std::string& s){std::string out="\"";for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';}
void write_file(const std::filesystem::path& p,const std::string& s){
    std::ofstream out(p,std::ios::binary);
    auto prefix=s.starts_with("\xef\xbb\xbf")?3u:0u;
    out<<s.substr(0,prefix);
    if(p.extension()==".seed" && !s.substr(prefix).starts_with("#!"))out<<"#!/usr/bin/env hirari\n";
    out<<s.substr(prefix);require(static_cast<bool>(out),"test fixture write failed");
}
struct Environment {
    std::wstring name;std::optional<std::wstring> previous;
    Environment(std::wstring n,std::wstring value):name(std::move(n)) {
        DWORD size=GetEnvironmentVariableW(name.c_str(),nullptr,0);
        if(size){std::wstring s(size,L'\0');auto count=GetEnvironmentVariableW(name.c_str(),s.data(),size);s.resize(count);previous=s;}
        require(SetEnvironmentVariableW(name.c_str(),value.c_str()),"test environment set failed");
    }
    ~Environment(){SetEnvironmentVariableW(name.c_str(),previous?previous->c_str():nullptr);}
};
std::string env(const wchar_t* key){DWORD n=GetEnvironmentVariableW(key,nullptr,0);if(!n)return {};std::wstring s(n,L'\0');auto count=GetEnvironmentVariableW(key,s.data(),n);s.resize(count);return win32::narrow(s);}
struct Fixtures {
    std::filesystem::path before=std::filesystem::current_path(),root;
    Fixtures(){root=std::filesystem::temp_directory_path()/text_path("Hirari portable Ω "+std::to_string(GetCurrentProcessId())+" "+std::to_string(GetTickCount64()));std::filesystem::create_directories(root);}
    ~Fixtures(){std::error_code e;std::filesystem::current_path(before,e);std::filesystem::remove_all(root,e);}
    std::filesystem::path world(const std::string& name,const std::string& source){auto p=root/text_path(name);std::filesystem::create_directories(p);write_file(p/"world.toml","[world]\nname = \"Neutral fixture\"\nversion = 1\n[entry]\nprogram = \"start.seed\"\n");write_file(p/"start.seed",source);return p;}
};
void worlds_and_scripts() {
    Fixtures fixtures;MemoryHost host;Shell shell(host);Environment worldenv(L"HIRARI_WORLD",L"previous world");
    auto first=fixtures.world("First world Ω","greeting := \"hello\"\ngreeting\n\"READY\"\n");
    require(shell.execute("world open "+quote(path_text(first))),"valid world failed");require(shell.worlds().current().has_value(),"world missing");
    require(std::filesystem::current_path()==std::filesystem::canonical(first),"world cwd not established");require(env(L"HIRARI_WORLD")==path_text(std::filesystem::canonical(first)),"world env not established");
    auto broken=fixtures.world("Broken world","\"unterminated\n");
    require(!shell.execute("world open "+quote(path_text(broken))),"bad script world succeeded");
    require(std::filesystem::current_path()==std::filesystem::canonical(first)&&shell.worlds().current()->root==std::filesystem::canonical(first),"world cwd/state rollback failed");require(env(L"HIRARI_WORLD")==path_text(std::filesystem::canonical(first)),"world env rollback failed");
    auto missing=fixtures.world("Missing entry","()");std::filesystem::remove(missing/"start.seed");require(!shell.execute("world open "+quote(path_text(missing))),"missing entry accepted");
    auto escaped=fixtures.world("Escaped entry","()");write_file(escaped/"world.toml","[world]\nname='Neutral'\nversion=1\n[entry]\nprogram='../First world Ω/start.seed'\n");rejects([&]{read_world(escaped);},"entry path escaped world");
    auto duplicate=fixtures.world("Duplicate key","()");write_file(duplicate/"world.toml","[world]\nname='Neutral'\nname='Duplicate'\nversion=1\n[entry]\nprogram='start.seed'\n");rejects([&]{read_world(duplicate);},"duplicate key accepted");
    auto unsupported=fixtures.world("Unknown version","()");write_file(unsupported/"world.toml","[world]\nname='Neutral'\nversion=2\n[entry]\nprogram='start.seed'\n");rejects([&]{read_world(unsupported);},"unknown world version accepted");
    auto multiline=fixtures.root/"multiline.seed";write_file(multiline,"\xef\xbb\xbfgreeting := (\n\"multi\"\n)\ngreeting\n");require(shell.execute_file(multiline),"BOM/multiline script failed");require(shell.seed().print(shell.seed().binding(shell.seed().intern("greeting")))=="\"multi\"","script bindings lost");
    require(shell.execute("world status"),"shell did not recover after world failure");
    Shell empty(host);auto failing=fixtures.world("Initial failure","nonexistent_neutral_program \"arg\"\n");
    auto saved=std::filesystem::current_path();require(!empty.execute("world open "+quote(path_text(failing))),"missing program accepted");require(!empty.worlds().current()&&std::filesystem::current_path()==saved,"first-open failure did not roll back");
}
struct InterruptHost:MemoryHost {
    std::atomic<bool> stop{false};
    bool interrupted()override{return stop.exchange(false);}
};
bool process_alive(DWORD pid) {
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!process)return false;
    bool alive=WaitForSingleObject(process,0)==WAIT_TIMEOUT;CloseHandle(process);return alive;
}
// `{ }` stage failures, cancellation, early downstream exit and cleanup.
void pipeline_failures(Shell& shell,MemoryHost& host) {
    auto run=[&](std::string_view source){auto p=shell.execute(source);if(!p)throw std::runtime_error(std::string(source)+"\n"+host.err);return p;};
    auto fails=[&](std::string_view source,std::string_view expected){
        host.err.clear();require(!shell.execute(source),"pipeline failure was accepted");
        if(host.err.find(expected)==std::string::npos)throw std::runtime_error("pipeline failure lacked context: "+host.err);
    };
    ProcessResult p;
    require(ProcessHost::unpack(shell.seed(),run("data bind -middle {hirari_test_program emit \"a\" | hirari_test_program fail | hirari_test_program relay}"),p)&&
            p.status==7&&p.stages==std::vector<int>{0,7,0},"nonzero middle stage status was hidden");
    require(shell.seed().print(run("if {hirari_test_program fail | hirari_test_program relay} \"yes\" \"no\""))=="\"no\"","pipeline status lost its truth");
    fails("{hirari_test_program emit \"a\" | missing_neutral_program}","{} stage 2 is not a native process: missing_neutral_program");
    run("seed-stage := input() input");
    fails("{hirari_test_program emit \"a\" | seed-stage}","{} stage 2 is not a native process");
    fails("{\"text\" | hirari_test_program relay}","{} stage 1 is not a native process");
    fails("{}","empty pipeline context");
    // The consumer exits after one line; a producer that ignores the broken
    // pipe is ended (status 141) instead of holding the pipeline open.
    auto started=std::chrono::steady_clock::now();
    require(ProcessHost::unpack(shell.seed(),run("data bind -cut {hirari_test_program stubborn | hirari_test_program head-one}"),p)&&
            p.status==0&&p.out=="x\n"&&p.stages==std::vector<int>{141,0},"downstream early exit left the producer running");
    require(std::chrono::steady_clock::now()-started<std::chrono::seconds(15),"early-exit cleanup took too long");
    // Ctrl+C ends every stage; no child survives.
    InterruptHost cancelling;Shell cancellable(cancelling);
    std::thread interrupter([&]{std::this_thread::sleep_for(std::chrono::milliseconds(800));cancelling.stop=true;});
    auto cancelled=cancellable.execute("data bind -cancelled {hirari_test_program stubborn | hirari_test_program pid-sleep 30000}");
    interrupter.join();
    require(cancelled&&ProcessHost::unpack(cancellable.seed(),cancelled,p)&&p.status==130,"interrupt did not end the pipeline");
    DWORD pid=static_cast<DWORD>(std::stoul(p.out));
    require(!process_alive(pid),"interrupted pipeline left a stage running");
    // {PIPELINE} > file: the last stage writes the file, beyond the capture budget.
    Fixtures sink;auto output=sink.root/"piped.bin";
    run("{hirari_test_program bulk | hirari_test_program relay} > "+quote(path_text(output)));
    require(std::filesystem::file_size(output)==20*1024*1024,"pipeline redirection buffered or truncated");
    // Braces continue across lines in a script; | elsewhere is unaffected.
    auto script=sink.root/"braced.seed";
    write_file(script,"{ hirari_test_program emit \"z\" \"a\"\n  | hirari_test_program sort\n}\n");
    host.out.clear();require(shell.execute_file(script),"multi-line pipeline script failed");
    require(host.out=="a\nz\n","multi-line pipeline context did not stream");
}
void semantics_and_processes(const std::filesystem::path& helper) {
    Fixtures streaming;
    Environment stream_ack(L"HIRARI_STREAM_ACK",(streaming.root/L"first output received").wstring());
    Environment path(L"PATH",helper.parent_path().wstring()+L";"+win32::widen(env(L"SystemRoot"))+L"\\System32");MemoryHost host;Shell shell(host);
    auto run=[&](std::string_view source){auto p=shell.execute(source);if(!p)throw std::runtime_error(std::string(source)+"\n"+host.err);return p;};
    run("greeting := \"hello\"");require(shell.seed().print(run("greeting"))=="\"hello\"","binding regression");
    run("\"reverse\" = greeting");require(shell.seed().print(run("greeting"))=="\"reverse\"","inverse binding regression");
    require(shell.seed().is_name(run("-foo")),"Name became literal");require(shell.seed().is_literal(run("\"foo\"")),"literal became Name");require(shell.seed().is_unit(run("()")),"unit regression");
    require(shell.seed().print(run("if -true \"yes\" \"no\""))=="\"yes\"","if true regression");require(shell.seed().print(run("if -false \"yes\" \"no\""))=="\"no\"","if false regression");
    require(shell.seed().print(run("if (hirari_test_program emit) \"yes\" \"no\""))=="\"yes\"","process success condition");require(shell.seed().print(run("if (hirari_test_program fail) \"yes\" \"no\""))=="\"no\"","process failure condition");
    run("item := \"previous\"");run("for -item -(\"alpha\" \"beta\") item");require(shell.seed().print(run("item"))=="\"previous\"","loop binding restoration");
    ProcessResult p;require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"z\" \"a\" | hirari_test_program sort"),p)&&p.out=="a\nz\n","pipeline regression");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"\" \"space value\" \"Ω\""),p)&&p.out=="\nspace value\nΩ\n","portable Unicode/empty argv regression");
    std::string special = "apostrophe'only";
    std::string literalPath = "C:\\ordinary\\trailing\\";
    std::string quoted = "say\"hello\"";
    auto arguments = "hirari_test_program emit " + quote(special) + " " + quote(literalPath) + " " + quote(quoted);
    require(ProcessHost::unpack(shell.seed(),run(arguments),p)&&p.out==special+"\n"+literalPath+"\n"+quoted+"\n","native literal apostrophe/backslash/quote argv regression");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program relay (hirari_test_program emit \"hello\")"),p)&&p.out=="hello\n","consumer composition regression");
    for (auto spelling : {"hirari_test_program relay \"answer Ω\\n\"?",
                          "\"answer Ω\\n\"? | hirari_test_program relay",
                          "hirari_test_program relay(\"answer Ω\\n\"?)"})
        require(ProcessHost::unpack(shell.seed(),run(spelling),p)&&p.status==0&&p.out=="answer Ω\n", "materialized answer application/flow mismatch");
    require(ProcessHost::unpack(shell.seed(),run("(exec \"hirari_test_program\" fail)? | hirari_test_program relay"),p)&&p.status==7, "materialized process answer lost failure status");
    run("large-answer := "+quote(std::string(128*1024, 'x')));
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program relay large-answer?"),p)&&p.status==0&&p.out==std::string(128*1024,'x'), "materialized answer deadlocked or truncated beyond a pipe buffer");
    require(ProcessHost::unpack(shell.seed(),run("exec \"hirari_test_program\" emit \"argv\"?"),p)&&p.out=="argv\n", "exec treated explicit argv as stdin");
    // Application: an exec producer's answer becomes a native consumer's stdin;
    // exec as the consumer keeps explicit argv, so its operand stays argv.
    require(ProcessHost::unpack(shell.seed(),run("exec \"hirari_test_program\" emit \"z\" \"a\" | hirari_test_program sort"),p)&&p.status==0&&p.out=="a\nz\n",
            "exec producer application lost its answer");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"q\" | exec \"hirari_test_program\" emit"),p)&&p.status==0&&p.out=="q\n\n",
            "exec consumer reinterpreted its explicit argv operand as stdin");
    // The POSIX context streams explicit exec stages too.
    for(auto source:{"{exec \"hirari_test_program\" emit \"z\" \"a\" | hirari_test_program sort}",
                     "{hirari_test_program emit \"z\" \"a\" | exec \"hirari_test_program\" sort}",
                     "{exec \"hirari_test_program\" emit \"z\" \"a\" | exec \"hirari_test_program\" sort}"}) {
        host.out.clear();
        require(ProcessHost::unpack(shell.seed(),run(source),p)&&p.status==0&&p.out.empty()&&host.out=="a\nz\n","explicit exec pipeline lost argv or stream");
    }
    auto provider="provide "+quote(path_text(helper));
    run("data bind -held ("+provider+")");
    auto held=run("held");require(shell.seed().is_name(held)&&run("held")==held,"structured result lost retained identity");
    require(shell.seed().is_number(run("data get held \"large\""))&&shell.seed().print(run("data get held \"large\""))=="9223372036854775807","structured integer lost precision or type");
    require(shell.seed().is_unit(run("data get held \"null\""))&&shell.seed().is_literal(run("data get held \"empty\"")),"Unit collapsed to empty text");
    require(shell.seed().print(run("if (data get held \"flag\") \"yes\" \"no\""))=="\"no\"","structured Boolean lost truth semantics");
    require(shell.seed().print(run("data at (data get held \"items\") 0"))=="\"日本\"","structured list lost Unicode");
    require(shell.seed().print(run("data get (data get held \"source\") \"snapshot\""))=="\"neutral-fixture\"",
            "single-field record lost source context");
    auto printed=shell.seed().print(held);
    auto restored=shell.seed().read(printed);
    shell.seed().bind(shell.seed().intern("restored"),restored);
    require(shell.seed().print(run("data json restored"))==shell.seed().print(run("data json held")),
            "structured record/list representation did not survive print/read");
    require(shell.seed().is_name(run(provider+" -semantic"))&&shell.seed().is_literal(run(provider+" \"semantic\"")),"provider flattened Names to text");
    require(shell.seed().is_unit(run(provider+" ()")),"provider lost Unit");
    require(!shell.execute("hirari_test_program emit held"),"structured result silently flattened to argv");
    require(ProcessHost::unpack(shell.seed(),run("exec \"hirari_test_program\" emit (data json held)"),p)&&p.out.find("9223372036854775807")!=std::string::npos,"explicit projection failed");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit -c -3 3.14"),p)&&p.out=="-c\n-3\n3.14\n", "Name flag or physical literal argv changed");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program fail || hirari_test_program emit \"recovered\""),p)&&p.status==0&&p.out=="recovered\n", "process OR did not recover failure");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program fail && missing_neutral_program"),p)&&p.status==7, "process AND demanded skipped branch");
    Fixtures routing;
    auto output=routing.root/text_path("routed 日本.bin");auto destination=quote(path_text(output));
    run("hirari_test_program emit \"first\" > "+destination);
    run("hirari_test_program emit \"last\" >> "+destination);
    {std::ifstream file(output,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});require(bytes=="first\nlast\n", "native redirection or append corrupted bytes");}
    run("hirari_test_program bulk > "+destination);
    require(std::filesystem::file_size(output)==20*1024*1024, "native redirection buffered into capture budget");
    run("held > "+destination);
    run("data bind -routed (data read "+destination+")");
    require(shell.seed().print(run("data json routed"))==shell.seed().print(run("data json held")), "structured file sink flattened data");
    run("\"a\" > "+destination);run("\"b\" >> "+destination);
    {std::ifstream file(output,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});require(bytes=="ab", "literal redirection inserted formatting");}
    // Application and | are Seed application: the producer completes and its
    // answer becomes stdin. Only the { } context joins live OS pipes.
    for (auto source : {"hirari_test_program stamped | hirari_test_program latency",
                        "hirari_test_program latency (hirari_test_program stamped)",
                        "hirari_test_program latency(hirari_test_program stamped)",
                        "(hirari_test_program stamped)? | hirari_test_program latency"})
        require(ProcessHost::unpack(shell.seed(),run(source),p)&&p.status==0&&p.out.starts_with("BUFFERED\n")&&p.out.ends_with("last\n"),
                "application created an OS pipe");
    host.out.clear();
    require(ProcessHost::unpack(shell.seed(),run("{hirari_test_program delayed | hirari_test_program observe}"),p)&&p.status==0&&host.out.starts_with("LIVE\nfirst\nlast\n"),
            "pipeline context is not live");
    for (auto source : {"{hirari_test_program stamped | hirari_test_program latency}",
                        "hirari_test_program latency {hirari_test_program stamped}",
                        "hirari_test_program latency{hirari_test_program stamped}"}) {
        host.out.clear();
        auto value=run(source);
        // A top-level native consumer of a pipeline answers after its stages.
        std::string seen=host.out;if(ProcessHost::unpack(shell.seed(),value,p))seen+=p.out;
        require(seen.starts_with("LIVE\n")&&seen.ends_with("last\n"),"pipeline producer was buffered");
    }
    // Nested inside an expression, the pipeline answers -stdout -stderr -status -stages.
    require(ProcessHost::unpack(shell.seed(),run("data bind -piped {hirari_test_program emit \"z\" \"a\" | hirari_test_program sort}"),p)&&p.out=="a\nz\n"&&p.stages==std::vector<int>{0,0},
            "nested pipeline answer lost output or stage statuses");
    // The | inside parentheses is application, never handed to a process as argv.
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program relay (hirari_test_program emit \"z\" \"a\" | hirari_test_program sort)"),p)&&p.out=="a\nz\n",
            "grouped application passed | to a process as argv");
    // Quoted | and braces remain literal argv; the grouped producer is stdin.
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"|\" \"{x}\" (hirari_test_program emit \"a\" | hirari_test_program relay)"),p)&&
            p.out=="|\n{x}\n","literal pipe or brace text changed meaning");
    for (auto source : {"hirari_test_program emit \"one\" | hirari_test_program relay",
                        "hirari_test_program relay (hirari_test_program emit \"one\")",
                        "hirari_test_program relay(hirari_test_program emit \"one\")"})
        require(ProcessHost::unpack(shell.seed(),run(source),p)&&p.out=="one\n","composition duplicated or lost process output");
    require(shell.seed().is_abstraction(run("hirari_test_program () emit \"should not run\"")),"native binder was executed instead of abstracted");
    require(shell.seed().is_abstraction(run("\"should not run\" : hirari_test_program")),"colon abstraction was executed instead of retained");
    run("emit-value := value() (hirari_test_program emit value)");
    require(ProcessHost::unpack(shell.seed(),run("emit-value \"日本\""),p)&&p.status==0&&p.out=="日本\n",
            "abstraction did not compose a native executable");
    run("observe-input := input() (hirari_test_program observe input)");
    // Substitution carries a { } producer as written, so the native consumer in
    // the body still receives a live stream.
    run("latency-input := input() (hirari_test_program latency input)");
    for(auto spelling:{"latency-input {hirari_test_program stamped}",
                       "(input() (hirari_test_program latency input)) {hirari_test_program stamped}"}) {
        host.out.clear();auto value=run(spelling);std::string seen=host.out;if(ProcessHost::unpack(shell.seed(),value,p))seen+=p.out;
        require(seen.starts_with("LIVE\n"),"substitution buffered a pipeline producer");
    }
    for(auto spelling:{"latency-input (hirari_test_program stamped)",
                       "hirari_test_program stamped | latency-input"})
        require(ProcessHost::unpack(shell.seed(),run(spelling),p)&&p.status==0&&p.out.starts_with("BUFFERED\n"),
                "substituted application created an OS pipe");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit ()"),p)&&p.out=="\n","Unit argv was silently discarded");
    pipeline_failures(shell,host);
    Fixtures fixtures;
    auto script=fixtures.root/"cli Ω file.seed";write_file(script,"greeting := \"CLI_READY\"\ngreeting\n");
    require(ProcessHost::unpack(shell.seed(),run("hirari --file "+quote(path_text(script))),p)&&p.status==0&&p.out.find("CLI_READY")!=std::string::npos,"CLI --file failed from Unicode path");
    write_file(script,"#[Multiline (\n\" ignored #[nested] comment]\n# another ( comment\ngreeting := \"#[literal]\"\ngreeting\n");
    require(shell.execute_file(script)&&shell.seed().print(run("greeting"))=="\"#[literal]\"","script comment continuation or literal failed");
    write_file(script,"# line 1\n#[ unfinished\n");
    require(!shell.execute_file(script)&&host.err.find(":3: incomplete Seed expression")!=std::string::npos,"comment EOF line diagnostic missing");
    write_file(script,"\"incomplete\n");
    require(ProcessHost::unpack(shell.seed(),run("hirari --file "+quote(path_text(script))),p)&&p.status!=0&&p.err.find("incomplete Seed expression")!=std::string::npos,"CLI --file error status/diagnostic missing");
    auto world=fixtures.world("Child env","hirari_test_program environment\n");require(shell.execute("world open "+quote(path_text(world))),"child env world failed");require(host.out.find(path_text(std::filesystem::canonical(world)))!=std::string::npos,"child process did not inherit world env");
    auto bad=fixtures.world("Process failure","hirari_test_program fail\n");auto saved=std::filesystem::current_path();require(!shell.execute("world open "+quote(path_text(bad))),"nonzero entry process status ignored");require(std::filesystem::current_path()==saved,"process failure world rollback");
    Shell quitting(host);require(quitting.execute("exit")&&quitting.processes().quitting(),"exit regression");Shell quitting2(host);require(quitting2.execute("quit")&&quitting2.processes().quitting(),"quit regression");
}
void shell_programs(const std::filesystem::path& helper) {
    Fixtures fixture;
    Environment path(L"PATH", helper.parent_path().wstring()+L";"+win32::widen(env(L"SystemRoot"))+L"\\System32");
    Environment namespace_path(L"HIRARI_PATH", fixture.root.wstring());

}

void path_priority(const std::filesystem::path& helper) {
    Fixtures fixture;
    auto preferred=fixture.root/text_path("preferred tools 日本");
    std::filesystem::create_directories(preferred);
    std::filesystem::copy_file(helper,preferred/L"sort.exe");
    std::filesystem::create_directory(preferred/L"hirari_test_program");
    Environment path(L"PATH",preferred.wstring()+L";"+helper.parent_path().wstring()+L";"+win32::widen(env(L"SystemRoot"))+L"\\System32");
    MemoryHost host;Shell shell(host);ProcessResult result;
    auto value=shell.execute("sort emit \"PATH_FIRST\"");
    require(value&&ProcessHost::unpack(shell.seed(),value,result)&&result.out=="PATH_FIRST\n","PATH priority lost to Windows system sort");
    value=shell.execute("hirari_test_program emit \"DIRECTORY_SKIPPED\"");
    require(value&&ProcessHost::unpack(shell.seed(),value,result)&&result.out=="DIRECTORY_SKIPPED\n","PATH directory masked a native executable");
    value=shell.execute("hirari --eval "+quote("hirari_test_program emit \"RAW_BYTES\""));
    require(value&&ProcessHost::unpack(shell.seed(),value,result)&&result.out=="RAW_BYTES\n","CLI translated child output bytes");
}

}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2,"test helper path required");worlds_and_scripts();semantics_and_processes(std::filesystem::path(argv[1]));shell_programs(std::filesystem::path(argv[1]));path_priority(std::filesystem::path(argv[1]));
        std::cout<<"All generic Hirari integration tests passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
