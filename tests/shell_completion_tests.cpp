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
void write(const std::filesystem::path& path,std::string_view source){std::ofstream out(path,std::ios::binary);if(path.extension()==".seed"&&!source.starts_with("#!"))out<<"#!/usr/bin/env hirari\n";out<<source;require(static_cast<bool>(out),"fixture write");}
}
int wmain(int argc,wchar_t** argv) {
    auto before=std::filesystem::current_path();
    // Scripts report their canonical directory, so the root is canonical even when TEMP is an 8.3 path.
    auto root=std::filesystem::canonical(std::filesystem::temp_directory_path())/(L"Hirari completion 日本 "+std::to_wstring(GetCurrentProcessId()));
    try {
        require(argc==2,"native helper path required");auto helper=std::filesystem::path(argv[1]);
        std::filesystem::create_directories(root);std::filesystem::current_path(root);
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"user vocabulary").c_str());
        SetEnvironmentVariableW(L"HIRARI_PATH",root.c_str());
        SetEnvironmentVariableW(L"HIRARI_STREAM_ACK",(root/L"first output received").c_str());
        auto system=std::wstring(_wgetenv(L"SystemRoot"));auto path=helper.parent_path().wstring()+L";"+system+L"\\System32";SetEnvironmentVariableW(L"PATH",path.c_str());
        Host host;hirari::Shell shell(host);auto& r=shell.seed();
        auto run=[&](const std::string& s){auto p=shell.execute(s);if(!p)throw std::runtime_error(s+"\n"+host.errorText);return p;};
        auto process=[&](const std::string& s){hirari::ProcessResult result;require(hirari::ProcessHost::unpack(r,run(s),result),"expected process result");return result;};
        run("say := hirari_test_program emit");
        require(process("say \"hello Ω\" \"\"").out=="hello Ω\n\n","callable product did not forward argv");
        require(process("say -c --long").out=="-c\n--long\n","a flag did not reach the program as written");
        run("forward := say");require(process("forward \"chain\"").out=="chain\n","alias chain");
        run("same := value() value");
        require(r.print(run("same --semantic"))=="--semantic", "abstraction lost a Name value");
        require(r.as_literal(run("? same"))->bytes.find("arguments:")!=std::string::npos,
                "help did not recognize an abstraction");
        require(process("say \"z\" \"a\" | hirari_test_program sort").out=="a\nz\n","alias pipeline expansion");
        require(process("apply --(hirari_test_program emit \"prefix\") --(\"appended\" --semantic)").out=="prefix\nappended\n--semantic\n","apply lost structural argument forwarding");
        write(root/L"named.seed","# Returns its argument without flattening identity\narg 0\n");
        write(root/L"where.seed","# Editable vocabulary wins over Windows where.exe\narg 0\n");
        require(r.is_name(run("WHERE --semantic")), "PATH executable shadowed Seed vocabulary");
        require(r.is_name(run("--semantic | WHERE")), "pipe planning bypassed Seed vocabulary");
        require(process("where \"where.exe\"").status==0,
                "lowercase command did not retain native lookup");
        require(r.as_literal(run("help --WHERE"))->bytes.find("Seed program:")!=std::string::npos &&
                r.as_literal(run("help --where"))->bytes.find("native executable:")!=std::string::npos,
                "help ignored command capitalization");
        require(process("exec \"where.exe\" \"where.exe\"").status==0,
                "explicit native execution stopped working after a vocabulary collision");
        write(root/L"last.seed","first := 1\n3");
        require(r.is_number(run("last")) && r.print(run("last"))=="3", "EOF lost final literal or required a terminator");
        write(root/L"empty.seed","");require(r.is_unit(run("empty")), "empty source did not yield Unit");
        write(root/L"continued.seed","layoutValue := same \\\r\n\t--semantic\r\nlayoutValue\r\n");
        require(r.is_name(run("continued")), "script continuation lost Name identity or form order");
        write(root/L"continued-process.seed","hirari_test_program emit \\\n  \"one\" \\\n\t\"two\"\n");
        require(process("continued-process").out == "one\ntwo\n", "script continuation lost native argv");
        write(root/L"logical-pipeline.seed",
              "\"\"\"One logical pipeline, not one invocation per physical line.\"\"\"\n"
              "prefix := \"z\"\n"
              "hirari_test_program emit prefix \"a\"\n"
              "  | hirari_test_program sort\n"
              "\n# A comment is layout between stages\n"
              "#[ outer\n#[ nested ]\n]\n"
              "\t| hirari_test_program relay");
        require(process("logical-pipeline").out == "a\nz\n",
                "leading pipe committed a producer early or broke EOF continuation");
        write(root/L"logical-crlf.seed",
              "hirari_test_program emit \"z\" \"a\"\r\n"
              "|hirari_test_program sort\r\n");
        require(process("logical-crlf").out == "a\nz\n", "CRLF or attached pipe split logical expressions");
        write(root/L"logical-scopes.seed",
              "data bind --rows (\n"
              "  provide --regex \"parse\" \"(?<word>[A-Za-z]+)\" \"oak pine\"\n"
              ")\n"
              "data get rows \"rows\"\n"
              "| data size\n"
              "value := 17\n"
              "value");
        require(r.print(run("logical-scopes")) == "17" && !r.binding(r.intern("rows")),
                "pipeline swallowed a subsequent expression or changed run scope");
        write(root/L"logical-if.seed",
              "if --true (\n"
              "  hirari_test_program emit \"z\" \"a\"\n"
              "  | hirari_test_program sort\n"
              ") (\n"
              "  hirari_test_program fail\n"
              ")\n");
        require(process("logical-if").out == "a\nz\n", "nested if lost pipeline structure or evaluated its other branch");
        write(root/L"logical-string.seed", "\"\"\"first\n| literal data\nlast\"\"\"");
        require(r.as_literal(run("logical-string"))->bytes == "first\n| literal data\nlast",
                "literal pipe/newline became source layout");
        write(root/L"logical-or.seed", "1\n|| --false --true\n");
        require(r.print(run("logical-or")) == "--true", "leading || was treated as a pipe continuation");
        require(!seed::Reader::source_leading_pipe("# | ignored\n\"| literal\"") &&
                !seed::Reader::source_leading_pipe("--(|)\n") &&
                !seed::Reader::source_leading_pipe("#[ unterminated") &&
                seed::Reader::source_leading_pipe("#[ comment ]\n| consumer"),
                "continuation lookahead ignored lexical context");
        write(root/L"continued-doc.seed","\\\n\n\"\"\"Only documentation.\"\"\"\n");
        require(r.as_literal(run("continued-doc"))->bytes == "Only documentation.",
                "layout-only continuation consumed the first form");
        require(r.as_literal(run("help continued-doc"))->bytes.find("Only documentation.") != std::string::npos,
                "help failed to read documentation after continued layout");
        write(root/L"unfinished-continuation.seed","hirari_test_program emit \\");
        require(!shell.execute_file(root/L"unfinished-continuation.seed") &&
                host.errorText.find(":2: incomplete Seed expression") != std::string::npos,
                "dangling script continuation lacked a source diagnostic");
        host.errorText.clear();
        require(shell.input("promptValue := same \\") && shell.waiting() &&
                !r.binding(r.intern("promptValue")), "prompt executed before continuation completed");
        require(shell.input("\t--semantic") && !shell.waiting() && r.is_name(run("promptValue")),
                "prompt continuation failed");
        require(shell.input("cancelledValue := same \\") && shell.waiting(), "prompt did not wait");
        shell.interrupt();
        require(!shell.waiting() && !r.binding(r.intern("cancelledValue")), "interrupt ran pending source");
        require(shell.input("same --semantic \\") && shell.waiting() && shell.input("") &&
                !shell.waiting(), "blank committed line failed to end continuation");
        require(shell.input("multilineValue := \"\"\"first") && shell.waiting() &&
                shell.input("\tsecond\"\"\"") && !shell.waiting() &&
                r.as_literal(run("multilineValue"))->bytes == "first\n\tsecond",
                "prompt multiline literal changed physical whitespace");
        write(root/L"forward-file.seed","apply --(hirari_test_program emit (script-directory) (argc)) (args)\n");
        require(process("forward-file \"appended\" --semantic").out==hirari::path_text(root)+"\n2\nappended\n--semantic\n","script frame values failed structural forwarding");
        write(root/L"forward-name.seed","apply --named args?\n");
        require(r.is_name(run("forward-name --semantic")),"forwarding a script argument flattened its identity");
        require(!r.binding(r.intern("args")) && !r.binding(r.intern("argc")) && !r.binding(r.intern("script-directory")),"script frame facts escaped their call scope");
        run("named-alias := named");require(r.is_name(run("named-alias --semantic")),"script alias lost Name identity");
        run("danger := hirari_test_program fail");
        auto help=run("? danger");require(r.is_literal(help)&&r.as_literal(help)->bytes.find("danger :=")!=std::string::npos,"help did not inspect retained binding");
        require(r.as_literal(run("danger help"))->bytes==r.as_literal(help)->bytes,"prefix/postfix help mismatch");
        require(shell.status()==0,"help executed failing subject");
        require(r.as_literal(run("help --pipe"))->bytes.find("PRODUCER | CONSUMER")!=std::string::npos,"named operator help lost Program identity");
        run("documented := x() (\"\"\"Returns the supplied value.\nNo effects during help.\"\"\" x)");
        require(r.as_literal(run("help --documented"))->bytes.find("No effects during help.")!=std::string::npos,"abstraction docstring missing");
        auto binding = r.find_cell(r.intern("documented"));
        auto metadata = binding->metadata;
        auto revision = binding->revision;
        auto query = r.read("help --documented");
        auto nodeCount = r.arena().node_count();
        auto metadataHelp = r.reduce(query);
        require(r.is_literal(metadataHelp) && r.arena().node_count() == nodeCount &&
                binding->metadata == metadata && binding->revision == revision,
                "help extracted executable products or changed a binding");
        run("curried-doc := a() (b() (\"Curried documentation.\" a))");
        require(r.find_cell(r.intern("curried-doc"))->metadata->parameters ==
                std::vector<std::string>{"a", "b"}, "canonical arguments not indexed");
        run("colon-doc := a (b c) : c : b : a");
        require(r.find_cell(r.intern("colon-doc"))->metadata->parameters ==
                std::vector<std::string>{"a", "b", "c"}, "colon arguments not indexed");
        auto originalMetadata = r.find_cell(r.intern("curried-doc"))->metadata;
        run("curried-doc := x() (\"Updated documentation.\" x)");
        require(r.as_literal(run("help --curried-doc"))->bytes.find("Updated documentation.") != std::string::npos &&
                originalMetadata->docstring == "Curried documentation.", "binding metadata revision mutated in place");
        auto outputPath = root / L"help output.txt";
        run("{help --documented > " + quote(hirari::path_text(outputPath)) + "}");
        require(std::filesystem::file_size(outputPath) > 0, "metadata help routing failed");
        // | is application: a Seed answer reaches a native consumer's stdin when
        // explicitly demanded with ?, otherwise it is an argument.
        require(process("(help --documented)? | hirari_test_program relay").out.find("No effects during help.") != std::string::npos,
                "metadata help pipeline failed");
        auto forbiddenPath = root / L"must not exist.txt";
        // help documents the { } context that owns this form; it never runs it.
        shell.execute("help ({hirari_test_program emit \"oops\" > " + quote(hirari::path_text(forbiddenPath)) + "})");
        require(!std::filesystem::exists(forbiddenPath), "help executed a compound subject");
        require(!shell.execute("help (hirari_test_program emit \"oops\")"), "help accepted a compound subject");
        host.errorText.clear();
        auto detailed = run("inspect --documented");
        require(r.as_literal(detailed)->bytes.find("documented :=") != std::string::npos &&
                r.as_literal(detailed)->bytes.find("x()") != std::string::npos, "explicit inspection lost structure");
        write(root/L"documented-command.seed","\"\"\"Never execute while inspecting.\n# this is doc text\"\"\"\nhirari_test_program fail\n");
        require(r.as_literal(run("help documented-command"))->bytes.find("Never execute while inspecting.")!=std::string::npos && shell.status()==0,"script docstring inspection executed its body");
        require(r.as_literal(run("? named"))->bytes.find("Returns its argument")!=std::string::npos,"script doc discovery");
        write(root/L"metadata-only.seed", "\"\"\"Documentation survives a broken body.\"\"\"\n(unterminated\n");
        query = r.read("help --metadata-only"); nodeCount = r.arena().node_count();
        require(r.as_literal(r.reduce(query))->bytes.find("Documentation survives a broken body.") != std::string::npos &&
                r.arena().node_count() == nodeCount, "script help parsed/materialized the executable body");
        write(root/L"define-origin.seed", "origin-fn := x() (\"File metadata.\" x)\n");
        run("source \"define-origin.seed\"");
        require(r.find_cell(r.intern("origin-fn"))->metadata->source_file.find("define-origin.seed") != std::string::npos,
                "source metadata missing");
        require(process("demand (hirari_test_program emit \"demand\")").out=="demand\n","explicit demand lost evaluator semantics");
        run("vocab save --say \"A structured greeting tool\"");
        run("vocab save --documented");
        run("vocab save --same \"Return the supplied value\"");
        run("ignore := \"ignored\" : _");run("vocab save --ignore");
        run("count := 9223372036854775807");run("vocab save --count");
        run("meaning := --semantic");run("vocab save --meaning");
        run("text := \"semantic\"");run("vocab save --text");
        {Host second;hirari::Shell restored(second);auto value=restored.execute("say \"after restart\"");hirari::ProcessResult result;
            require(value&&hirari::ProcessHost::unpack(restored.seed(),value,result)&&result.out=="after restart\n","durable callable did not survive restart");
            require(restored.seed().is_name(restored.execute("meaning"))&&restored.seed().is_literal(restored.execute("text")),"persistence lost Name/text distinction");
            require(restored.seed().print(restored.execute("same --semantic"))=="--semantic",
                    "durable abstraction did not survive a shell restart");
            require(restored.seed().print(restored.execute("ignore missing"))=="\"ignored\"", "anonymous abstraction persistence");
            require(restored.seed().is_number(restored.execute("count")), "numeric persistence lost physical kind");
            auto restoredHelp = restored.execute("help --documented");
            require(restored.seed().as_literal(restoredHelp)->bytes.find("No effects during help.") != std::string::npos &&
                    restored.seed().as_literal(restoredHelp)->bytes.find("durable vocabulary") != std::string::npos,
                    "durable metadata was not recovered independently of execution");
            require(second.errorText.empty(),"valid store load failed");}
        run("say := hirari_test_program emit \"prefix\"");run("vocab save --say");
        run("vocab restore --say 1");require(process("say \"restored\"").out=="restored\n","historical structural restore");
        require(r.as_literal(run("vocab history --say"))->bytes.find("A structured greeting tool")!=std::string::npos,"annotation/history lost");
        run("data bind --opaque (data list 4)");run("vocab save --opaque");
        require(!shell.execute("hirari_test_program emit opaque"),"structured provider identity flattened into native argv");
        require(!shell.execute("hirari_test_program relay opaque?"),"structured provider query answer flattened into native stdin");
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"interactive vocabulary").c_str());
        {
            Host interactiveHost;hirari::Shell interactiveShell(interactiveHost);
            std::istringstream input("broken := hirari_test_program fail\nmissing := unavailable_program \"arg\"\nvocab restore --broken 1\nvocab use "+quote(hirari::path_text(root/L"interactive vocabulary"))+"\nquit\n");
            struct InputGuard { std::streambuf* old; ~InputGuard(){std::cin.rdbuf(old);std::cin.clear();} } inputGuard{std::cin.rdbuf(input.rdbuf())};
            interactiveShell.run();
            auto history=interactiveShell.execute("vocab history --broken");
            auto lines=interactiveShell.seed().as_literal(history)->bytes;
            require(std::count(lines.begin(),lines.end(),'\n')==2,"restore/reload duplicated durable history commits");
            Host restoredHost;hirari::Shell restored(restoredHost);
            auto help=restored.execute("? broken");
            require(restored.seed().as_literal(help)->bytes.find("broken :=")!=std::string::npos,"failed initial command prevented definition persistence");
            require(restored.seed().as_literal(restored.execute("? missing"))->bytes.find("missing :=")!=std::string::npos,"initial evaluator error prevented definition persistence");
        }
        SetEnvironmentVariableW(L"HIRARI_STATE",(root/L"user vocabulary").c_str());
        // Script stages compose by Seed application in this shell: the script's
        // answer becomes the native consumer's stdin. They are not { } stages.
        write(root/L"producer.seed","hirari_test_program emit \"first\" \"last\"\n");
        for(auto form:{"producer? | hirari_test_program relay", "hirari_test_program relay producer?",
                       "hirari_test_program relay(producer?)"}){
            auto answer=process(form);
            require(answer.status==0 && answer.out=="first\nlast\n","script composition lost its answer");
        }
        // A bare identity among a longer argv product stays a scalar argument.
        require(process("producer | hirari_test_program emit").out=="producer\n","bare script operand was not scalar argv");
        require(!shell.execute("{producer | hirari_test_program relay}"),"a .seed script was accepted as a { } stage");
        write(root/L"failure.seed","hirari_test_program fail\n");
        // A failed script answer is an evaluation error, never an empty stdin.
        require(!shell.execute("failure? | hirari_test_program relay"),"application hid a failed script answer");
        // Native argv keeps its words, and a program that parses its own command line can read it.
        require(process("exec \"cmd.exe\" \"/c\" \"echo hi there\"").out=="hi there\r\n","cmd /c could not read its quoted command");
        require(process("hirari_test_program emit \"it's\" \"a b\" \"\" \"plain\" \"tail\\\\\"").out=="it's\na b\n\nplain\ntail\\\n","argv words changed through quoting");
        // Text prints as itself; values print as they read.
        host.out.clear();run("data print \"raw\" 3");require(host.out=="raw 3\n","data print quoted text");
        host.out.clear();run("data print \"line\\n\"");require(host.out=="line\n","data print doubled a text's own line ending");
        host.out.clear();run("data print [1 2]");require(host.out=="[1 2]\n","data print showed a list's wire form");
        host.out.clear();run("[1 2]");require(host.out=="[1 2]\n","a list did not display as a list");
        host.out.clear();run("data record --a 1 --bb \"x\"");
        require(std::count(host.out.begin(),host.out.end(),'\n')==2 && host.out.find("--record")==std::string::npos && host.out.find("bb")!=std::string::npos && host.out.find("\"x\"")!=std::string::npos,"a record did not display one field per line");
        host.out.clear();run("for --i [1 2] (data print i)");require(host.out=="1\n2\n","a loop of Unit bodies displayed its results");
        // Definitions are quiet, and still answer and bind.
        host.out.clear();
        require(r.print(run("quiet-value := 41"))=="41" && r.print(run("data bind --held-value 42"))=="42" && r.print(run("43 = reversed-value"))=="43" && host.out.empty(),"a definition displayed its value or lost its answer");
        require(r.print(run("quiet-value"))=="41" && r.print(run("held-value"))=="42" && r.print(run("reversed-value"))=="43","a quiet definition did not bind");
        host.out.clear();run("noisy-value := hirari_test_program emit \"hidden\"");require(host.out.empty(),"a definition displayed a program's output");
        write(root/L"quiet-definitions.seed","first-part := 1\nsecond-part := 2\nfirst-part + second-part\n");
        host.out.clear();run("quiet-definitions");require(host.out=="3\n","a script displayed its definitions");
        write(root/L"only-definitions.seed","only-part := 1\n");
        host.out.clear();require(r.print(run("run \"only-definitions.seed\""))=="1" && host.out.empty(),"a script ending in a definition displayed it or lost its answer");
        // A wrong name says what was meant.
        host.errorText.clear();
        require(!shell.execute("capabilty list") && host.errorText.find("program not found: capabilty")!=std::string::npos && host.errorText.find("Did you mean")!=std::string::npos && host.errorText.find("capability")!=std::string::npos,"a near-miss program had no suggestion");
        host.errorText.clear();
        require(r.as_literal(run("help --capabilty"))->bytes.find("Did you mean")!=std::string::npos,"help of a near-miss name had no suggestion");
        require(r.as_literal(run("help"))->bytes.find("Built-ins")!=std::string::npos,"help did not list the built-ins");
        // Piped input is not a person: no banner and no prompt.
        {
            Host piped;hirari::Shell pipedShell(piped);std::istringstream input("1 + 2\n");
            struct InputGuard { std::streambuf* old; ~InputGuard(){std::cin.rdbuf(old);std::cin.clear();} } inputGuard{std::cin.rdbuf(input.rdbuf())};
            pipedShell.run();
            require(piped.out=="3\n","piped input received a banner or a prompt");
        }
        // Tab completion, from the text before the cursor.
        auto insertions=[&](std::string_view text){return shell.complete(text).insertions;};
        auto offers=[&](std::string_view text,std::string_view prefix){auto found=insertions(text);return std::any_of(found.begin(),found.end(),[&](const std::string& value){return value.starts_with(prefix);});};
        require(offers("capabil","capability") && shell.complete("capabil").start==0,"a command name did not complete");
        require(offers("help --capabil","--capability") && shell.complete("help --capabil").start==5,"a Name did not complete");
        for(const auto& found:insertions("hel"))require(found.compare(0,3,"hel")==0,"a program's capitals replaced what was typed");
        run("completable-binding := 1");require(offers("completable-b","completable-binding"),"a session definition did not complete");
        require(insertions("zzzz-no-such-word").empty(),"nothing to complete offered something");
        write(root/L"complete me.txt","x");std::filesystem::create_directories(root/L"complete-dir");
        require(offers("data file \"complete m","\"complete me.txt\"") && shell.complete("data file \"complete m").start==10,"a file name in a string did not complete and close");
        {auto found=insertions("data file \"complete-d");require(found.size()==1 && found.front()=="\"complete-dir/","a directory in a string did not stay open");}
        require(offers("data file complete-d","\"complete-dir/"),"a bare argument did not complete as a quoted path");
        {auto found=shell.complete("data file \"complete");require(found.insertions.size()==2 && found.labels.size()==2,"several matches were not all offered");}
        // The filesystem capability.
        run("provide --files \"mkdir\" \"tree/a/b\" \"tree/c\"");require(std::filesystem::is_directory(root/L"tree"/L"a"/L"b") && std::filesystem::is_directory(root/L"tree"/L"c"),"mkdir did not create parents");
        run("provide --files \"mkdir\" \"tree/a\"");
        write(root/L"tree"/L"a"/L"b"/L"f.txt","payload");
        require(r.print(run("data size (provide --files \"list\" \"tree/a/b\")"))=="1","list did not see the file");
        run("data bind --listed (data at (provide --files \"list\" \"tree/a/b\") 0)");
        require(r.as_literal(run("data get listed \"name\""))->bytes=="f.txt" && r.as_literal(run("data get listed \"kind\""))->bytes=="file" && r.print(run("data get listed \"size\""))=="7","list record wrong");
        require(r.as_literal(run("data get (data at (provide --files \"list\" \"tree\") 0) \"kind\""))->bytes=="directory","list did not name a directory");
        require(!shell.execute("provide --files \"list\" \"no such place\"") && host.errorText.find("no such place")!=std::string::npos,"a missing path was not named");host.errorText.clear();
        run("provide --files \"cp\" \"tree/a/b/f.txt\" \"copied.txt\"");require(std::filesystem::file_size(root/L"copied.txt")==7,"cp did not copy a file");
        run("provide --files \"cp\" \"copied.txt\" \"tree/c\"");require(std::filesystem::exists(root/L"tree"/L"c"/L"copied.txt"),"cp did not copy into a directory");
        run("provide --files \"mv\" \"copied.txt\" \"moved.txt\"");require(!std::filesystem::exists(root/L"copied.txt") && std::filesystem::exists(root/L"moved.txt"),"mv did not rename");
        run("provide --files \"mv\" \"moved.txt\" \"tree/c\"");require(!std::filesystem::exists(root/L"moved.txt") && std::filesystem::exists(root/L"tree"/L"c"/L"moved.txt"),"mv did not move into a directory");
        require(!shell.execute("provide --files \"cp\" \"tree\" \"tree-copy\"") && !std::filesystem::exists(root/L"tree-copy"),"cp took a directory without --r");host.errorText.clear();
        run("provide --files \"cp\" --r \"tree\" \"tree-copy\"");require(std::filesystem::file_size(root/L"tree-copy"/L"a"/L"b"/L"f.txt")==7,"cp --r did not copy a tree");
        require(!shell.execute("provide --files \"rm\" \"tree\"") && std::filesystem::exists(root/L"tree"/L"a"/L"b"/L"f.txt"),"rm took a directory without --r");host.errorText.clear();
        run("provide --files \"rm\" \"tree/c/moved.txt\"");require(!std::filesystem::exists(root/L"tree"/L"c"/L"moved.txt"),"rm did not remove a file");
        run("provide --files \"rm\" --r \"tree\" \"tree-copy\"");require(!std::filesystem::exists(root/L"tree") && !std::filesystem::exists(root/L"tree-copy"),"rm --r did not remove trees");
        require(!shell.execute("provide --files \"rm\" \"tree\"") && host.errorText.find("tree")!=std::string::npos,"removing a missing path did not fail with its name");host.errorText.clear();
        require(!shell.execute("provide --files \"no-such-operation\""),"an unknown operation was accepted");host.errorText.clear();
        write(root/L"user vocabulary"/L"vocabulary.bin","DAMAGED");
        require(!shell.execute("vocab save --say"),"damaged vocabulary overwritten");
        Host recovery;hirari::Shell safe(recovery);require(safe.execute("\"shell survives\"")&&recovery.errorText.find("unavailable")!=std::string::npos,"damaged store killed shell");
        std::filesystem::current_path(before);std::filesystem::remove_all(root);std::cout<<"SHELL_COMPLETION_OK\n";return 0;
    }catch(const std::exception& error){std::filesystem::current_path(before);std::cerr<<error.what()<<"\nEvidence: "<<hirari::path_text(root)<<'\n';return 1;}
}
