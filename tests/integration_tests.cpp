#include "hirari/shell.hpp"
#include "hirari/wire.hpp"
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
using K=wire::ValueKind;using M=wire::MessageKind;using V=wire::Value;
void require(bool x,const char* reason){if(!x)throw std::runtime_error(reason);}
template<class F>void rejects(F&& f,const char* message){try{f();}catch(const std::exception&){return;}throw std::runtime_error(message);}
struct MemoryHost:Host {
    std::string out,err;std::mutex mutex;
    void write(std::string_view s)override{std::lock_guard lock(mutex);out+=s;}
    void error(std::string_view s)override{std::lock_guard lock(mutex);err+=s;}
};
std::string quote(const std::string& s){std::string out="\"";for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';}
void write_file(const std::filesystem::path& p,const std::string& s){std::ofstream out(p,std::ios::binary);out<<s;require(static_cast<bool>(out),"test fixture write failed");}
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
// Deliberately a neutral protocol peer, not a substitute for player acceptance.
class TestServer {
    std::thread worker_;std::atomic_bool stopping_{false};std::promise<void> ready_;std::exception_ptr failure_;
    std::int64_t next_type_=1,next_object_=1;std::map<std::int64_t,std::string> types_,objects_;
    static bool transfer(HANDLE pipe,void* buffer,std::size_t n,bool writing){auto* p=static_cast<std::uint8_t*>(buffer);while(n){DWORD done=0;BOOL ok=writing?WriteFile(pipe,p,static_cast<DWORD>(n),&done,nullptr):ReadFile(pipe,p,static_cast<DWORD>(n),&done,nullptr);if(!ok||!done)return false;p+=done;n-=done;}return true;}
    static bool send(HANDLE pipe,const wire::Message& m,bool fragments) {
        auto body=wire::pack(m);std::uint32_t n=static_cast<std::uint32_t>(body.size());wire::Bytes all;
        for(unsigned i=0;i<4;++i)all.push_back(static_cast<std::uint8_t>(n>>(i*8)));all.insert(all.end(),body.begin(),body.end());
        for(std::size_t i=0;i<all.size();){std::size_t step=fragments?std::min<std::size_t>(3,all.size()-i):all.size()-i;if(!transfer(pipe,all.data()+i,step,true))return false;i+=step;}return true;
    }
    static std::optional<wire::Message> receive(HANDLE pipe) {
        std::uint8_t header[4]{};if(!transfer(pipe,header,4,false))return std::nullopt;
        std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=static_cast<std::uint32_t>(header[i])<<(i*8);
        require(n>=13&&n<=wire::max_frame,"client produced invalid frame");wire::Bytes body(n);
        if(!transfer(pipe,body.data(),n,false))return std::nullopt;return wire::unpack(body);
    }
    wire::Message reflect(wire::Message m) {
        auto text=[&](std::size_t i){require(m.values.at(i).kind==K::String,"expected wire string");return std::get<std::string>(m.values[i].data);};
        auto handle=[&](std::size_t i,K kind){require(m.values.at(i).kind==kind,"wrong reference kind");return std::get<std::int64_t>(m.values[i].data);};
        wire::Message reply{M::Result,m.id,{}};
        if(m.kind==M::Type){require(m.values.size()==1,"type payload count");auto name=text(0);auto id=next_type_++;types_[id]=name;reply.values={V::type(id)};}
        else if(m.kind==M::Find){require(m.values.size()==1,"find payload count");for(const auto& [id,name]:objects_)if(name==text(0)){reply.values={V::object(id)};return reply;}throw std::runtime_error("not found");}
        else if(m.kind==M::New){++new_count;require(m.values.size()>=2,"new payload count");require(types_.contains(handle(0,K::Type)),"unknown type");auto signature=std::get<std::int32_t>(m.values[1].data);require(signature==-1||signature==1,"new signature count");if(signature==1)require(m.values[2].kind==K::Type,"new exact signature type");auto index=signature==1?3u:2u;auto id=next_object_++;objects_[id]=m.values.size()>index?text(index):"unnamed";reply.values={V::object(id)};}
        else if(m.kind==M::Get){require(m.values.size()==2,"get payload count");auto member=text(1);if(m.values[0].kind==K::Object){auto id=handle(0,K::Object);require(objects_.contains(id),"unknown object");reply.values={member=="number"?V::i64(9223372036854775807LL):V::string(objects_.at(id))};}else{auto id=handle(0,K::Type);require(types_.contains(id),"unknown type");reply.values={V::string(types_.at(id))};}}
        else if(m.kind==M::Set){require(m.values.size()==3,"set payload count");auto id=handle(0,K::Object);require(objects_.contains(id),"unknown object");auto member=text(1);if(member=="name")objects_[id]=text(2);else if(member=="number")require(m.values[2].kind==K::Int64&&std::get<std::int64_t>(m.values[2].data)==9223372036854775807LL,"lost numeric type or precision");}
        else if(m.kind==M::Component){require(m.values.size()==2,"component payload count");require(objects_.contains(handle(0,K::Object)),"unknown object");require(types_.contains(handle(1,K::Type)),"unknown type");auto id=next_object_++;objects_[id]="component";reply.values={V::object(id)};}
        else if(m.kind==M::Call){require(m.values.size()>=4,"call payload count");auto method=text(1);auto generic=std::get<std::int32_t>(m.values[2].data);std::size_t cursor=3;for(int i=0;i<generic;++i)require(m.values.at(cursor++).kind==K::Type,"generic type expected");auto signature=std::get<std::int32_t>(m.values.at(cursor++).data);for(int i=0;i<signature;++i)require(m.values.at(cursor++).kind==K::Type,"signature type expected");if(signature>=0)require(m.values.size()-cursor==static_cast<std::size_t>(signature),"signature mismatch");if(method=="Fault")throw std::runtime_error("neutral broker failure");if(method=="SetActive")require(m.values.at(cursor).kind==K::Boolean,"boolean name mapping");reply.values=method=="Echo"?std::vector<V>{m.values.at(cursor)}:std::vector<V>{};}
        else if(m.kind==M::Release){require(m.values.size()==1,"release payload count");if(m.values[0].kind==K::Object)objects_.erase(handle(0,K::Object));else types_.erase(handle(0,K::Type));}
        else if(m.kind==M::Batch){for(const auto& packed:m.values){require(packed.kind==K::Bytes,"batch submessage must be Bytes");auto sub=wire::unpack(std::get<wire::Bytes>(packed.data));for(auto& v:sub.values)if(v.kind==K::Batch){auto n=std::get<std::int32_t>(v.data);require(n>=0&&static_cast<std::size_t>(n)<reply.values.size(),"invalid batch ref");v=reply.values[n];}auto subreply=reflect(sub);reply.values.push_back(subreply.values.empty()?V::null():subreply.values[0]);}}
        else throw std::runtime_error("unsupported mock request");return reply;
    }
    void serve(){
        try {
            auto path=L"\\\\.\\pipe\\"+win32::widen(endpoint);
            win32::Handle pipe(CreateNamedPipeW(path.c_str(),PIPE_ACCESS_DUPLEX,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,4096,0,nullptr));
            require(static_cast<bool>(pipe),"test named pipe create failed");ready_.set_value();
            while(!stopping_) {
                if(!ConnectNamedPipe(pipe.get(),nullptr)&&GetLastError()!=ERROR_PIPE_CONNECTED){if(stopping_)break;throw std::runtime_error("test connect failed");}
                ++connections;bool hello=false;
                while(!stopping_) {
                    auto request=receive(pipe.get());if(!request)break;
                    wire::Message reply{M::Result,request->id,{}};
                    if(!hello){require(request->kind==M::Hello&&request->values.size()==1&&request->values[0].kind==K::String&&std::get<std::string>(request->values[0].data)==wire::protocol,"wrong Hello handshake");++hellos;hello=true;reply.kind=M::Ready;auto protocol=bad_version?"skunkworks.wire/999":wire::protocol;std::string session=epoch==1?"11111111111111111111111111111111":"22222222222222222222222222222222";
                        if(d_session)session=epoch==1?"11111111-1111-1111-1111-111111111111":"22222222-2222-2222-2222-222222222222";
                        if(bad_session)session="1111111111111111111111111111111Z";
                        reply.values={V::string(protocol),V::string(session)};}
                    else {
                        ++requests;
                        if(drop)break;
                        if(oversize){std::uint32_t n=wire::max_frame+1;std::uint8_t h[4];for(unsigned i=0;i<4;++i)h[i]=static_cast<std::uint8_t>(n>>(8*i));transfer(pipe.get(),h,4,true);break;}
                        if(hang)std::this_thread::sleep_for(std::chrono::milliseconds(300));
                        try{reply=reflect(*request);}catch(const std::exception& e){reply={M::Fault,request->id,{V::string(e.what())}};}
                        if(wrong_id)++reply.id;
                    }
                    if(!send(pipe.get(),reply,true))break;
                }
                DisconnectNamedPipe(pipe.get());
            }
        }catch(...){failure_=std::current_exception();try{ready_.set_exception(failure_);}catch(...){} }
    }
public:
    std::string endpoint="hirari.test."+std::to_string(GetCurrentProcessId())+"."+std::to_string(GetTickCount64());
    std::atomic_int epoch{1},connections{0},hellos{0},requests{0},new_count{0};
    std::atomic_bool bad_session{false},d_session{false},bad_version{false},wrong_id{false},oversize{false},drop{false},hang{false};
    TestServer(){auto ready=ready_.get_future();worker_=std::thread([this]{serve();});try{ready.get();}catch(...){stop();throw;}}
    ~TestServer(){stop();}
    void stop(){if(worker_.joinable()){stopping_=true;CancelSynchronousIo(static_cast<HANDLE>(worker_.native_handle()));worker_.join();}}
    void check(){if(failure_)std::rethrow_exception(failure_);}
};
void codec() {
    wire::Message m{M::Set,0x0102030405060708LL,{V::string("A"),V::i32(-2)}};
    wire::Bytes golden{14,8,7,6,5,4,3,2,1,2,0,0,0,6,1,0,0,0,65,2,254,255,255,255};
    require(wire::pack(m)==golden,"codec disagrees with BinaryWriter golden body");
    std::vector<V> values{V::null(),V::boolean(true),V::i32(-123),V::i64(-9000000000LL),V::f32(1.25f),V::f64(-2.5),V::string("héllo Ω"),V::object(7),V::type(8),V::bytes({0,1,255}),V::batch(2)};
    auto round=wire::unpack(wire::pack({M::Result,42,values}));require(round.id==42&&round.kind==M::Result&&round.values.size()==values.size(),"codec round trip metadata");
    for(std::size_t i=0;i<values.size();++i)require(round.values[i].kind==values[i].kind&&round.values[i].data==values[i].data,"codec round trip value");
    auto truncated=golden;truncated.pop_back();rejects([&]{wire::unpack(truncated);},"truncated wire accepted");
    auto trailing=golden;trailing.push_back(0);rejects([&]{wire::unpack(trailing);},"trailing wire accepted");
    wire::Bytes bad(13);bad[9]=0xff;bad[10]=0xff;bad[11]=0xff;bad[12]=0xff;rejects([&]{wire::unpack(bad);},"negative count accepted");
    rejects([&]{wire::pack({M::Result,0,std::vector<V>(wire::max_values+1)});},"value limit not enforced");
    rejects([&]{wire::pack({M::Result,0,{V::bytes(wire::Bytes(wire::max_frame))}});},"frame limit not enforced");
}
void unity() {
    TestServer server;Environment endpoint(L"SKUNK_UNITY_PIPE",win32::widen(server.endpoint));MemoryHost host;Shell shell(host);
    auto run=[&](const std::string& source){auto p=shell.execute(source);if(!p)throw std::runtime_error("shell failed: "+source+"\n"+host.err);return p;};
    run("unity bind :t (unity type \"UnityEngine.GameObject\")");run("unity bind :go (unity new t \"GenericProof\")");
    auto go=shell.seed().reduce(shell.seed().binding(shell.seed().intern("go")));require(shell.unity().foreign(go)&&!shell.seed().is_literal(go),"object reference became text");
    require(shell.seed().print(run("unity get go \"name\""))=="\"GenericProof\"","get result");
    run("unity set go \"name\" \"RenamedProof\"");
    require(shell.seed().print(run("unity get go \"name\""))=="\"RenamedProof\"","set/get did not address the same object");
    require(run("go")==go&&run("go")==go&&server.new_count==1,"capture did not retain opaque identity");
    run("unity find \"RenamedProof\"");
    run("unity bind :booleanType (unity type \"System.Boolean\")");run("unity bind :stringType (unity type \"System.String\")");
    run("unity call go \"SetActive\" :false");require(server.new_count==1,"call allocated a new object");run("unity call-exact go \"SetActive\" :(booleanType) :true");
    run("unity call-generic go \"Echo\" :(stringType) \"hello\"");
    run("unity call-generic-exact go \"Echo\" :(stringType) :(stringType) \"hello\"");
    run("unity bind :other (unity new-exact t :(stringType) \"ExactProof\")");
    run("unity bind :transformType (unity type \"UnityEngine.Transform\")");run("unity component go transformType");
    run("unity bind :n (unity get go \"number\")");run("unity set go \"number\" n");
    require(shell.unity().foreign(shell.seed().reduce(shell.seed().binding(shell.seed().intern("n"))))->value.kind==K::Int64,"numeric result lost type");
    run("unity bind :bytes (unity bytes \"00ff01\")");run("unity call go \"Echo\" bytes");
    require(!shell.execute("unity set go \"name\" :notText"),"Name converted to string");
    require(!shell.execute("unity get \"1\" \"name\""),"text reference accepted");
    run("unity bind :results (unity batch :((type \"UnityEngine.GameObject\") (new (unity batch-ref 0) \"BatchProof\") (get (unity batch-ref 1) \"name\")))");
    require(shell.seed().print(run("unity at results 2"))=="\"BatchProof\"","batch primary result");
    require(!shell.execute("unity batch :((get (unity batch-ref 0) \"name\"))"),"forward batch ref accepted");
    require(!shell.execute("unity call go \"Fault\""),"broker Fault not surfaced");run("unity get go \"name\"");
    require(server.hellos==1&&server.connections==1,"connection is not persistent");
    run("unity release other");require(!shell.execute("unity get other \"name\""),"released reference reused");
    auto before_lazy=server.new_count.load();
    run("lazy := unity new t \"LazyProof\"");
    require(shell.seed().is_relation(shell.seed().binding(shell.seed().intern("lazy"))),"native assignment was forced eager");
    auto lazy1=run("lazy");auto lazy2=run("lazy");
    require(lazy1!=lazy2&&server.new_count==before_lazy+3,"native lazy assignment was memoized");
    auto before_inverse=server.new_count.load();
    run("unity new t \"InverseProof\" = inverse");
    require(shell.seed().is_relation(shell.seed().binding(shell.seed().intern("inverse"))),"inverse binding lost its program form");
    auto inverse1=run("inverse");auto inverse2=run("inverse");
    require(inverse1!=inverse2&&server.new_count==before_inverse+3,"inverse binding pre-executed or memoized Unity allocation");
    run("unity disconnect");server.epoch=2;auto before=server.requests.load();
    require(!shell.execute("unity get go \"name\""),"stale session reference reused");require(server.requests==before,"stale reference sent to new session");
    run("unity type \"UnityEngine.GameObject\"");shell.unity().client().disconnect();server.stop();server.check();
}
void failures() {
    TestServer server;MemoryHost host;UnityClient client(host,server.endpoint,100);
    server.bad_version=true;rejects([&]{client.connect();},"wrong version accepted");require(!client.connected(),"bad handshake left client connected");
    server.bad_version=false;server.bad_session=true;
    rejects([&]{client.connect();},"non-hex session accepted");require(!client.connected(),"bad session left client connected");
    server.bad_session=false;client.connect();require(client.session()=="11111111111111111111111111111111","actual N32 session was changed");client.disconnect();
    server.d_session=true;client.connect();require(client.session()=="11111111-1111-1111-1111-111111111111","D36 session was changed");client.disconnect();server.d_session=false;
    client.connect();server.wrong_id=true;
    rejects([&]{client.request({M::Type,0,{V::string("Example")}});},"response id mismatch accepted");require(!client.connected(),"bad response left transport connected");
    server.wrong_id=false;client.connect();server.oversize=true;
    rejects([&]{client.request({M::Type,0,{V::string("Example")}});},"oversize frame accepted");
    server.oversize=false;client.connect();server.drop=true;auto before=server.requests.load();
    rejects([&]{client.request({M::New,0,{V::type(1),V::i32(-1)}});},"disconnect not detected");require(server.requests==before+1,"mutation automatically replayed");
    server.drop=false;client.connect();server.hang=true;
    auto start=std::chrono::steady_clock::now();rejects([&]{client.request({M::Type,0,{V::string("Example")}});},"hung pipe not timed out");
    require(std::chrono::steady_clock::now()-start<std::chrono::seconds(2),"timeout unbounded");client.disconnect();server.stop();server.check();
}
void worlds_and_scripts() {
    Fixtures fixtures;MemoryHost host;Shell shell(host);Environment worldenv(L"SKUNK_WORLD",L"previous world");
    auto first=fixtures.world("First world Ω","greeting := \"hello\"\ngreeting\n\"READY\"\n");
    require(shell.execute("world open "+quote(path_text(first))),"valid world failed");require(shell.worlds().current().has_value(),"world missing");
    require(std::filesystem::current_path()==std::filesystem::canonical(first),"world cwd not established");require(env(L"SKUNK_WORLD")==path_text(std::filesystem::canonical(first)),"world env not established");
    auto broken=fixtures.world("Broken world","\"unterminated\n");
    require(!shell.execute("world open "+quote(path_text(broken))),"bad script world succeeded");
    require(std::filesystem::current_path()==std::filesystem::canonical(first)&&shell.worlds().current()->root==std::filesystem::canonical(first),"world cwd/state rollback failed");require(env(L"SKUNK_WORLD")==path_text(std::filesystem::canonical(first)),"world env rollback failed");
    auto missing=fixtures.world("Missing entry","()");std::filesystem::remove(missing/"start.seed");require(!shell.execute("world open "+quote(path_text(missing))),"missing entry accepted");
    auto escaped=fixtures.world("Escaped entry","()");write_file(escaped/"world.toml","[world]\nname='Neutral'\nversion=1\n[entry]\nprogram='../First world Ω/start.seed'\n");rejects([&]{read_world(escaped);},"entry path escaped world");
    auto duplicate=fixtures.world("Duplicate key","()");write_file(duplicate/"world.toml","[world]\nname='Neutral'\nname='Duplicate'\nversion=1\n[entry]\nprogram='start.seed'\n");rejects([&]{read_world(duplicate);},"duplicate key accepted");
    auto unsupported=fixtures.world("Unknown version","()");write_file(unsupported/"world.toml","[world]\nname='Neutral'\nversion=2\n[entry]\nprogram='start.seed'\n");rejects([&]{read_world(unsupported);},"unknown world version accepted");
    auto multiline=fixtures.root/"multiline.seed";write_file(multiline,"\xef\xbb\xbfgreeting := (\n\"multi\"\n)\ngreeting\n");require(shell.execute_file(multiline),"BOM/multiline script failed");require(shell.seed().print(shell.seed().binding(shell.seed().intern("greeting")))=="\"multi\"","script bindings lost");
    require(shell.execute("world status"),"shell did not recover after world failure");
    Shell empty(host);auto failing=fixtures.world("Initial failure","nonexistent_neutral_program \"arg\"\n");
    auto saved=std::filesystem::current_path();require(!empty.execute("world open "+quote(path_text(failing))),"missing program accepted");require(!empty.worlds().current()&&std::filesystem::current_path()==saved,"first-open failure did not roll back");
}
void semantics_and_processes(const std::filesystem::path& helper) {
    Environment path(L"PATH",helper.parent_path().wstring()+L";"+win32::widen(env(L"SystemRoot"))+L"\\System32");MemoryHost host;Shell shell(host);
    auto run=[&](std::string_view source){auto p=shell.execute(source);if(!p)throw std::runtime_error(host.err);return p;};
    run("greeting := \"hello\"");require(shell.seed().print(run("greeting"))=="\"hello\"","binding regression");
    run("\"reverse\" = greeting");require(shell.seed().print(run("greeting"))=="\"reverse\"","inverse binding regression");
    require(shell.seed().is_name(run(":foo")),"Name became literal");require(shell.seed().is_literal(run("\"foo\"")),"literal became Name");require(shell.seed().is_unit(run("()")),"unit regression");
    require(shell.seed().print(run("if :true \"yes\" \"no\""))=="\"yes\"","if true regression");require(shell.seed().print(run("if :false \"yes\" \"no\""))=="\"no\"","if false regression");
    require(shell.seed().print(run("if (hirari_test_program emit) \"yes\" \"no\""))=="\"yes\"","process success condition");require(shell.seed().print(run("if (hirari_test_program fail) \"yes\" \"no\""))=="\"no\"","process failure condition");
    run("item := \"previous\"");run("for :item :(\"alpha\" \"beta\") item");require(shell.seed().print(run("item"))=="\"previous\"","loop binding restoration");
    ProcessResult p;require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"z\" \"a\" | hirari_test_program sort"),p)&&p.out=="a\nz\n","pipeline regression");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit \"\" \"space value\" \"Ω\""),p)&&p.out=="\nspace value\nΩ\n","portable Unicode/empty argv regression");
    std::string special = "apostrophe'only";
    std::string literalPath = "C:\\ordinary\\trailing\\";
    std::string quoted = "say\"hello\"";
    auto arguments = "hirari_test_program emit " + quote(special) + " " + quote(literalPath) + " " + quote(quoted);
    require(ProcessHost::unpack(shell.seed(),run(arguments),p)&&p.out==special+"\n"+literalPath+"\n"+quoted+"\n","native literal apostrophe/backslash/quote argv regression");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program relay (hirari_test_program emit \"hello\")"),p)&&p.out=="hello\n","consumer composition regression");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program delayed | hirari_test_program observe"),p)&&p.out.starts_with("LIVE\n"),"pipe execution is not live");
    for (auto source : {"hirari_test_program observe (hirari_test_program delayed)",
                        "hirari_test_program observe(hirari_test_program delayed)"})
        require(ProcessHost::unpack(shell.seed(),run(source),p)&&p.out.starts_with("LIVE\n"),"application composition buffered its producer");
    for (auto source : {"hirari_test_program emit \"one\" | hirari_test_program relay",
                        "hirari_test_program relay (hirari_test_program emit \"one\")",
                        "hirari_test_program relay(hirari_test_program emit \"one\")"})
        require(ProcessHost::unpack(shell.seed(),run(source),p)&&p.out=="one\n","composition duplicated or lost process output");
    require(!shell.execute("hirari_test_program () emit \"should not run\""),"foreign capture was silently treated as application");
    require(!shell.execute("\"should not run\" : hirari_test_program"),"capture mirror was silently treated as application");
    require(ProcessHost::unpack(shell.seed(),run("hirari_test_program emit ()"),p)&&p.out=="\n","Unit argv was silently discarded");
    Fixtures fixtures;
    auto script=fixtures.root/"cli Ω file.seed";write_file(script,"greeting := \"CLI_READY\"\ngreeting\n");
    require(ProcessHost::unpack(shell.seed(),run("hirari --file "+quote(path_text(script))),p)&&p.status==0&&p.out.find("CLI_READY")!=std::string::npos,"CLI --file failed from Unicode path");
    write_file(script,"#[Multiline (\n\" ignored #[nested] comment]\n# another ( comment\ngreeting := \"#[literal]\"\ngreeting\n");
    require(shell.execute_file(script)&&shell.seed().print(run("greeting"))=="\"#[literal]\"","script comment continuation or literal failed");
    write_file(script,"# line 1\n#[ unfinished\n");
    require(!shell.execute_file(script)&&host.err.find(":2: incomplete Seed expression")!=std::string::npos,"comment EOF line diagnostic missing");
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
    TestServer server; Environment endpoint(L"SKUNK_UNITY_PIPE",win32::widen(server.endpoint));
    MemoryHost host; Shell shell(host);
    auto run=[&](const std::string& source){auto p=shell.execute(source);if(!p)throw std::runtime_error(host.err);return p;};
    auto& r=shell.seed();
    write_file(fixture.root/"identity.seed", "arg 0\n# trailing comment\n#[nested #[ comment] still a comment]\n\n");
    require(r.print(run("identity :semantic-name"))==":semantic-name", "Seed command lost Name identity");
    require(r.print(run(":semantic-name | identity"))==":semantic-name", "Seed command flow/application mismatch");
    require(r.print(run("identity \"space Ω\""))=="\"space Ω\"", "Seed command Unicode/literal arguments failed");
    write_file(fixture.root/"identity.seed", "\"EDITED\"\n");
    require(r.print(run("identity \"ignored\""))=="\"EDITED\"", "Seed command cached stale source");
    require(r.print(run("identity.seed \"ignored\""))=="\"EDITED\"", "explicit Seed filename was treated as a native executable");
    write_file(fixture.root/"all.seed", "args\n");
    auto all=run("all :foo \"foo\" ()"); require(r.is_name(all), "script args must preserve product as a Name");
    auto all_terms=r.terms(r.as_node(r.as_name(all)->target));
    require(all_terms.size()==3 && r.is_name(all_terms[0]) && r.is_literal(all_terms[1]) && r.is_unit(all_terms[2]), "script arguments lost structure");
    run("local := \"caller\""); auto cell=r.find_cell(r.intern("local")); auto revision=cell->revision;
    write_file(fixture.root/"scope.seed", "local := \"callee\"\nnew-local := \"temporary\"\nlocal\n");
    require(r.print(run("scope"))=="\"callee\"", "Seed command result not returned");
    require(r.print(run("local"))=="\"caller\"" && cell->revision==revision && !r.binding(r.intern("new-local")), "call scope leaked bindings or replaced cells");
    run("source "+quote(path_text(fixture.root/"scope.seed")));
    require(r.print(run("local"))=="\"callee\"", "source did not export bindings");
    auto library=fixture.root/text_path("library Ω");std::filesystem::create_directories(library);
    write_file(library/"outer.seed", "source \"inner.seed\"\narg 0\n");
    write_file(library/"inner.seed", "args\n");
    require(r.print(run("run "+quote(path_text(library/"outer.seed"))+" :outer"))==":outer", "nested source did not restore argument frame or resolve file relative include");
    write_file(library/"bad.seed", "local := \"bad\"\nmissing_neutral_command \"arg\"\n");
    require(!shell.execute("run "+quote(path_text(library/"bad.seed"))) && host.err.find("bad.seed:2:")!=std::string::npos, "nested script line diagnostic missing");
    require(r.print(run("local"))=="\"callee\"", "failed script leaked binding changes");
    require(!shell.execute("arg 0"), "script arguments escaped call frame");
    write_file(library/"missing-arg.seed", "arg 9\n");
    require(!shell.execute("run "+quote(path_text(library/"missing-arg.seed"))), "missing argument accepted");
    write_file(library/"cycle.seed", "source \"cycle.seed\"\n");
    require(!shell.execute("run "+quote(path_text(library/"cycle.seed"))) && host.err.find("nesting limit")!=std::string::npos, "recursive source was unbounded");
    auto executable=library/text_path("external tool Ω.exe");std::filesystem::copy_file(helper,executable);
    ProcessResult process;
    require(ProcessHost::unpack(r,run("hirari_test_program emit source run arg args exec"),process) && process.out=="source\nrun\narg\nargs\nexec\n", "shell commands hijacked ordinary program argument positions");
    require(ProcessHost::unpack(r,run("exec "+quote(path_text(executable))+" emit \"\" \"space Ω\""),process) && process.out=="\nspace Ω\n", "exec flattened empty/Unicode/path arguments");
    write_file(library/"failure.seed", "exec "+quote(path_text(executable))+" fail\n\"UNREACHABLE\"\n");
    require(!shell.execute("run "+quote(path_text(library/"failure.seed"))) && host.out.find("UNREACHABLE")==std::string::npos, "tool failure did not stop the calling Seed program");
    write_file(library/"cli.seed", "arg 0\n");
    require(ProcessHost::unpack(r,run("hirari --file "+quote(path_text(library/"cli.seed"))+" \"CLI Ω ARG\""),process) && process.out=="\"CLI Ω ARG\"\n", "CLI script arguments failed");
    write_file(library/"object.seed", "unity bind :temporary (unity new (unity type \"UnityEngine.GameObject\") (arg 0))\ntemporary\n");
    run("unity bind :kept (run "+quote(path_text(library/"object.seed"))+" \"SURVIVING_OBJECT\")");
    require(r.print(run("unity get kept \"name\""))=="\"SURVIVING_OBJECT\"", "script returned handle lost identity after call scope ended");
    require(!r.binding(r.intern("temporary")), "script local foreign binding escaped");
    shell.unity().client().disconnect();server.stop();server.check();
}

void path_priority(const std::filesystem::path& helper) {
    Fixtures fixture;
    auto preferred=fixture.root/text_path("preferred tools 日本");
    std::filesystem::create_directories(preferred);
    std::filesystem::copy_file(helper,preferred/L"sort.exe");
    Environment path(L"PATH",preferred.wstring()+L";"+helper.parent_path().wstring()+L";"+win32::widen(env(L"SystemRoot"))+L"\\System32");
    MemoryHost host;Shell shell(host);ProcessResult result;
    auto value=shell.execute("sort emit \"PATH_FIRST\"");
    require(value&&ProcessHost::unpack(shell.seed(),value,result)&&result.out=="PATH_FIRST\n","PATH priority lost to Windows system sort");
    value=shell.execute("hirari --eval "+quote("hirari_test_program emit \"RAW_BYTES\""));
    require(value&&ProcessHost::unpack(shell.seed(),value,result)&&result.out=="RAW_BYTES\n","CLI translated child output bytes");
}

}
int wmain(int argc,wchar_t** argv) {
    try {
        require(argc==2,"test helper path required");codec();unity();failures();worlds_and_scripts();semantics_and_processes(std::filesystem::path(argv[1]));shell_programs(std::filesystem::path(argv[1]));path_priority(std::filesystem::path(argv[1]));
        std::cout<<"All generic Hirari integration tests passed.\n";return 0;
    }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
