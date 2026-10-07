#include "hirari/shell.hpp"
#include "hirari/win32.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <stdexcept>
#include <thread>
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
struct Host:hirari::Host {std::string out,errorText;void write(std::string_view s)override{out+=s;}void error(std::string_view s)override{errorText+=s;}};
std::string quote(std::string_view s){std::string out="\"";for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';}
void write(const std::filesystem::path& path,std::string_view source){std::ofstream out(path,std::ios::binary);out<<source;require(static_cast<bool>(out),"fixture write");}
}
int wmain(int argc,wchar_t** argv) {
    auto before=std::filesystem::current_path();
    auto root=std::filesystem::temp_directory_path()/(L"Hirari completion 日本 "+std::to_wstring(GetCurrentProcessId()));
    try {
        require(argc==2,"native helper path required");auto helper=std::filesystem::path(argv[1]);
        std::filesystem::create_directories(root);std::filesystem::current_path(root);
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"user vocabulary").c_str());
        SetEnvironmentVariableW(L"HIRARI_PATH",root.c_str());
        auto system=std::wstring(_wgetenv(L"SystemRoot"));auto path=helper.parent_path().wstring()+L";"+system+L"\\System32";SetEnvironmentVariableW(L"PATH",path.c_str());
        Host host;hirari::Shell shell(host);auto& r=shell.seed();
        auto run=[&](const std::string& s){auto p=shell.execute(s);if(!p)throw std::runtime_error(s+"\n"+host.errorText);return p;};
        auto process=[&](const std::string& s){hirari::ProcessResult result;require(hirari::ProcessHost::unpack(r,run(s),result),"expected process result");return result;};
        run("say := hirari_test_program emit");
        require(process("say \"hello Ω\" \"\"").out=="hello Ω\n\n","callable product did not forward argv");
        run("forward := say");require(process("forward \"chain\"").out=="chain\n","alias chain");
        require(process("say \"z\" \"a\" | hirari_test_program sort").out=="a\nz\n","alias pipeline expansion");
        require(process("apply :(hirari_test_program emit \"prefix\") :(\"appended\" :semantic)").out=="prefix\nappended\nsemantic\n","apply lost structural argument forwarding");
        write(root/L"named.seed","# Returns its argument without flattening identity\narg 0\n");
        write(root/L"forward-file.seed","apply :(hirari_test_program emit (script-directory) (argc)) (args)\n");
        require(process("forward-file \"appended\" :semantic").out==hirari::path_text(root)+"\n2\nappended\nsemantic\n","script frame values failed structural forwarding");
        write(root/L"forward-name.seed","apply :named (args)\n");
        require(r.is_name(run("forward-name :semantic")),"forwarding a script argument flattened its identity");
        require(!r.binding(r.intern("args")) && !r.binding(r.intern("argc")) && !r.binding(r.intern("script-directory")),"script frame facts escaped their call scope");
        run("named-alias := named");require(r.is_name(run("named-alias :semantic")),"script alias lost Name identity");
        run("danger := hirari_test_program fail");
        auto help=run("? danger");require(r.is_literal(help)&&r.as_literal(help)->bytes.find("danger :=")!=std::string::npos,"help did not inspect retained binding");
        require(r.as_literal(run("danger help"))->bytes==r.as_literal(help)->bytes,"prefix/postfix help mismatch");
        require(shell.status()==0,"help executed failing subject");
        require(r.as_literal(run("? named"))->bytes.find("Returns its argument")!=std::string::npos,"script doc discovery");
        require(process("demand (hirari_test_program emit \"demand\")").out=="demand\n","explicit demand lost evaluator semantics");
        run("vocab save :say \"A structured greeting tool\"");
        run("meaning := :semantic");run("vocab save :meaning");
        run("text := \"semantic\"");run("vocab save :text");
        {Host second;hirari::Shell restored(second);auto value=restored.execute("say \"after restart\"");hirari::ProcessResult result;
            require(value&&hirari::ProcessHost::unpack(restored.seed(),value,result)&&result.out=="after restart\n","durable callable did not survive restart");
            require(restored.seed().is_name(restored.execute("meaning"))&&restored.seed().is_literal(restored.execute("text")),"persistence lost Name/text distinction");
            require(second.errorText.empty(),"valid store load failed");}
        run("say := hirari_test_program emit \"prefix\"");run("vocab save :say");
        run("vocab restore :say 1");require(process("say \"restored\"").out=="restored\n","historical structural restore");
        require(r.as_literal(run("vocab history :say"))->bytes.find("A structured greeting tool")!=std::string::npos,"annotation/history lost");
        run("unity bind :opaque (unity i32 4)");require(!shell.execute("vocab save :opaque"),"opaque CLR value serialized as a forged Name");
        require(!shell.execute("hirari_test_program emit opaque"),"opaque CLR identity flattened into native argv");
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"interactive vocabulary").c_str());
        {
            Host interactiveHost;hirari::Shell interactiveShell(interactiveHost);
            std::istringstream input("broken := hirari_test_program fail\nmissing := unavailable_program \"arg\"\nvocab restore :broken 1\nvocab use "+quote(hirari::path_text(root/L"interactive vocabulary"))+"\nquit\n");
            struct InputGuard { std::streambuf* old; ~InputGuard(){std::cin.rdbuf(old);std::cin.clear();} } inputGuard{std::cin.rdbuf(input.rdbuf())};
            interactiveShell.run();
            auto history=interactiveShell.execute("vocab history :broken");
            auto lines=interactiveShell.seed().as_literal(history)->bytes;
            require(std::count(lines.begin(),lines.end(),'\n')==2,"restore/reload duplicated durable history commits");
            Host restoredHost;hirari::Shell restored(restoredHost);
            auto help=restored.execute("? broken");
            require(restored.seed().as_literal(help)->bytes.find("broken :=")!=std::string::npos,"failed initial command prevented definition persistence");
            require(restored.seed().as_literal(restored.execute("? missing"))->bytes.find("missing :=")!=std::string::npos,"initial evaluator error prevented definition persistence");
        }
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"user vocabulary").c_str());
        // Live byte stages invoke independent script processes; ordinary calls above stay structured.
        write(root/L"producer.seed","hirari_test_program delayed\n");
        write(root/L"consumer.seed","hirari_test_program relay-live\n");
        require(process("producer | hirari_test_program observe").out.starts_with("LIVE\n"),"script producer buffered its output");
        require(process("hirari_test_program delayed | consumer | hirari_test_program observe").out.starts_with("LIVE\n"),"mixed script consumer buffered stdin");
        write(root/L"failure.seed","hirari_test_program fail\n");
        require(process("failure | hirari_test_program relay").status!=0,"pipeline hid failed script stage");
        write(root/L"user vocabulary"/L"vocabulary.bin","DAMAGED");
        require(!shell.execute("vocab save :say"),"damaged vocabulary overwritten");
        Host recovery;hirari::Shell safe(recovery);require(safe.execute("\"shell survives\"")&&recovery.errorText.find("unavailable")!=std::string::npos,"damaged store killed shell");
        std::filesystem::current_path(before);std::filesystem::remove_all(root);std::cout<<"SHELL_COMPLETION_OK\n";return 0;
    }catch(const std::exception& error){std::filesystem::current_path(before);std::cerr<<error.what()<<"\nEvidence: "<<hirari::path_text(root)<<'\n';return 1;}
}
