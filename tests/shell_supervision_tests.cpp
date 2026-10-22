#include "hirari/shell.hpp"
#include "hirari/win32.hpp"
#include "hirari/value_wire.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace hirari;
namespace {
void require(bool value,const std::string& message){if(!value)throw std::runtime_error(message);}
std::string quote(std::string_view s){std::string out="\"";for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}return out+'"';}
struct TestHost:Host{std::string out,err;void write(std::string_view s)override{out+=s;}void error(std::string_view s)override{err+=s;}};
void write(const std::filesystem::path& path,std::string_view source){std::ofstream out(path,std::ios::binary);out<<source;require(static_cast<bool>(out),"fixture write");}
bool alive(unsigned long pid){HANDLE h=OpenProcess(SYNCHRONIZE,FALSE,pid);if(!h)return false;bool running=WaitForSingleObject(h,0)==WAIT_TIMEOUT;CloseHandle(h);return running;}
}
int wmain(int argc,wchar_t** argv){
    auto root=std::filesystem::temp_directory_path()/(L"Hirari supervision 日本 "+std::to_wstring(GetCurrentProcessId()));
    try{
        require(argc==3,"test program and hirari paths required");
        std::filesystem::create_directories(root);std::filesystem::current_path(root);
        SetEnvironmentVariableW(L"HIRARI_STATE",L"-");
        auto helper=std::filesystem::path(argv[1]);
        auto path=helper.parent_path().wstring()+L";"+std::wstring(_wgetenv(L"SystemRoot"))+L"\\System32";SetEnvironmentVariableW(L"PATH",path.c_str());
        const auto program=quote(win32::narrow(argv[1])),hirari=quote(win32::narrow(argv[2])),here=quote(path_text(root));
        TestHost host;Shell shell(host);auto& r=shell.seed();
        auto run=[&](const std::string& s){auto p=shell.execute(s);if(!p)throw std::runtime_error(s+"\n"+host.err);return p;};
        auto json=[&](const std::string& s){return Json::parse(r.as_literal(run("data json ("+s+")"))->bytes);};
        auto text=[&](const std::string& s){auto p=run(s);require(r.is_literal(p),"expected text: "+s+" -> "+r.print(p));return r.as_literal(p)->bytes;};
        auto rejects=[&](const std::string& s){require(!shell.execute(s),"invalid operation succeeded: "+s);host.err.clear();};

        // Polls with short bounded waits until a running job has written its first output, so a slow machine
        // delays the answer instead of failing it. Live output is still required while the job runs.
        auto first_output=[&](const std::string& job){
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
            for(;;){auto seen=json("job wait "+job+" 100");
                auto pending=seen.at("state")=="starting"||(seen.at("state")=="running"&&seen.at("stdout").get<std::string>().empty());
                if(!pending||std::chrono::steady_clock::now()>deadline)return seen;}
        };

        // ---- Jobs: identity, live output, non-terminating wait, ownership --------
        run("data bind -long (job start "+program+" (data list \"pid-sleep\" \"16000\") (data record -directory "+here+"))");
        auto started=json("long");
        require(started.at("timeout_ms").is_null(),"a job acquired an implicit timeout");
        require(started.at("directory")==path_text(root),"job directory option ignored");
        run("data bind -id (data get long \"id\")");
        auto partial=first_output("id");
        require(partial.at("wait")=="timeout"&&partial.at("state")=="running","bounded wait did not report a running job: "+partial.dump());
        auto pid=partial.at("pid").get<unsigned long>();
        require(partial.at("stdout")==std::to_string(pid)+"\n","job pid differs from the child's own identity, or output is not live: "+partial.dump());
        require(alive(pid),"a wait timeout terminated the job");
        auto finished=json("job wait id");
        require(finished.at("wait")=="completed"&&finished.at("state")=="succeeded"&&finished.at("exit_status")==0,"job did not complete: "+finished.dump());
        require(finished.at("stdout").get<std::string>().ends_with("done\n")&&!alive(pid),"completed job output/lifetime");

        // A sustained { } pipeline as one job: one hirari process for the job,
        // live stdout while it runs, no implicit timeout, and owned cleanup.
        const auto piped=quote("{hirari_test_program pid-sleep 16000 | hirari_test_program relay-live}");
        run("data bind -piped (data get (job start "+hirari+" (data list \"--eval\" "+piped+")) \"id\")");
        auto streaming=first_output("piped");
        require(streaming.at("wait")=="timeout"&&streaming.at("state")=="running"&&!streaming.at("stdout").get<std::string>().empty(),
                "pipeline job ended early or never streamed: "+streaming.dump());
        auto first_stage=std::stoul(streaming.at("stdout").get<std::string>());
        require(alive(first_stage),"pipeline job output was not live while its stages ran: "+streaming.dump());
        auto piped_done=json("job wait piped");
        require(piped_done.at("state")=="succeeded"&&piped_done.at("stdout").get<std::string>().ends_with("done\n")&&!alive(first_stage),
                "pipeline job did not complete with its streamed output: "+piped_done.dump());
        run("data bind -endless (data get (job start "+hirari+" (data list \"--eval\" "+
            quote("{hirari_test_program stubborn | hirari_test_program relay-live}")+")) \"id\")");
        auto endless=first_output("endless");
        require(endless.at("state")=="running"&&endless.at("stdout").get<std::string>().starts_with("x\n"),"endless pipeline job did not stream: "+endless.dump());
        auto endless_pid=endless.at("pid").get<unsigned long>();
        run("job cancel endless");
        require(json("job wait endless").at("state")=="cancelled"&&!alive(endless_pid),"cancelled pipeline job left its owner running");

        run("data bind -bounded (data get (job start "+program+" (data list \"pid-sleep\" \"30000\") (data record -timeout_ms 100)) \"id\")");
        auto expired=json("job wait bounded");
        require(expired.at("state")=="failed"&&expired.at("timed_out")==true&&expired.at("error")=="job timed out","explicit job timeout: "+expired.dump());

        run("data bind -doomed (data get (job start "+program+" (data list \"pid-sleep\" \"30000\")) \"id\")");
        auto running=first_output("doomed");auto doomed_pid=running.at("pid").get<unsigned long>();
        require(alive(doomed_pid),"job not running before cancel");
        run("job cancel doomed");
        require(json("job wait doomed").at("state")=="cancelled"&&!alive(doomed_pid),"cancellation did not stop the owned process");

        auto failed=json("job wait (data get (job start "+program+" (data list \"exit-code\" \"7\")) \"id\")");
        require(failed.at("exit_status")==7&&failed.at("state")=="failed"&&failed.at("stderr")=="exiting 7\n","job exit status/stderr: "+failed.dump());
        rejects("job wait id -1");rejects("job wait \"no-such-job\"");
        rejects("job start "+program+" (data list \"emit\") (data record -directory \"Z:/no/such/directory\")");

        // ---- Data: environment, JSON, structured results, records -------------
        require(r.is_unit(run("data env \"HIRARI_SUPERVISION_UNSET\"")),"unset variable is not Unit");
        run("data env \"HIRARI_SUPERVISION_PROBE\" \"session Ω 1\"");
        require(text("data env \"HIRARI_SUPERVISION_PROBE\"")=="session Ω 1","environment round trip");
        require(text("data get (hirari_test_program print-env \"HIRARI_SUPERVISION_PROBE\") \"stdout\"")=="session Ω 1\n","child did not inherit environment");
        run("data env \"HIRARI_SUPERVISION_PROBE\" ()");
        require(text("data get (hirari_test_program print-env \"HIRARI_SUPERVISION_PROBE\") \"stdout\"")=="<unset>\n","unset did not reach children");
        rejects("data env \"A=B\"");

        write(root/"manifest.json",R"({"record":{"name":"Example","version":1,"ready":true,"scale":0.5,"pins":["a","b"],"missing":null}})");
        run("data bind -manifest (data parse-json (data file \"manifest.json\"))");
        run("data bind -record (data get manifest \"record\")");
        require(text("data get record \"name\"")=="Example","JSON text field");
        require(r.print(run("data get record \"version\""))=="1"&&r.print(run("data get record \"scale\""))=="0.5","JSON numbers");
        require(r.print(run("data get record \"ready\""))=="-true","JSON boolean");
        require(r.is_unit(run("data get record \"missing\"")),"JSON null is not Unit");
        require(text("data at (data get record \"pins\") 1")=="b","JSON list");
        require(json("data size (data get (data parse-json (hirari_test_program emit \"{\\\"k\\\":[1,2,3]}\")) \"k\")")==3,"JSON from program output");
        rejects("data parse-json \"{not json\"");
        rejects("data parse-json (hirari_test_program exit-code \"2\")");
        rejects("data file \"no-such-file.json\"");

        require(json("data get (hirari_test_program exit-code \"3\") \"status\"")==3,"program status field");
        require(text("data get (hirari_test_program exit-code \"3\") \"stderr\"")=="exiting 3\n","program stderr field");
        require(text("data get (hirari_test_program emit \"out\") \"stdout\"")=="out\n","program stdout field");
        require(json("data record -name \"x\" \"count\" 2 -flag -true")==Json({{"name","x"},{"count",2},{"flag",true}}),"record construction");
        require(r.is_name(run("data get (data record -tag -semantic) \"tag\"")),"record lost Name identity");
        rejects("data record -a 1 -a 2");rejects("data record -odd");

        // ---- Control: iteration, sleep, failure, recovery ----------------------
        run("data bind -s "+quote(std::string(20000,'x')));
        run("while (\"\" < s) (data bind -s (provide -regex \"replace\" \"^.\" \"\" s))");
        require(text("s").empty(),"20000 loop iterations did not complete");
        auto before=std::chrono::steady_clock::now();run("sleep 0.2");
        require(std::chrono::steady_clock::now()-before>=std::chrono::milliseconds(190),"sleep returned early");
        rejects("sleep -1");rejects("sleep \"soon\"");
        run("same := a() (b() ((a <= b) && (b <= a)))");
        run("data bind -poll (data get (job start "+program+" (data list \"pid-sleep\" \"500\")) \"id\")");
        run("while (same (data get (job wait poll 20) \"wait\") \"timeout\") (sleep 0.02)");
        require(json("job status poll").at("state")=="succeeded","timed polling loop ended before completion");

        run("data bind -oops (attempt (fail \"manifest missing:\" \"x.json\"))");
        require(text("data get oops \"status\"")=="error"&&text("data get oops \"error\"")=="manifest missing: x.json","attempt error");
        run("data bind -exited (attempt (hirari_test_program exit-code \"7\"))");
        require(text("data get exited \"status\"")=="failed"&&r.print(run("data get exited \"exit_status\""))=="7"&&text("data get exited \"error\"")=="exiting 7\n","attempt program failure");
        run("data bind -fine (attempt (data get record \"name\"))");
        require(text("data get fine \"status\"")=="ok"&&text("data get fine \"value\"")=="Example","attempt success");
        run("data bind -falsy (attempt (if -false \"x\"))");
        require(text("data get falsy \"status\"")=="ok"&&r.is_unit(run("data get falsy \"value\"")),"false value reported as failure");
        require(!shell.execute("if (fail \"not a false value\") \"yes\" \"no\"")&&host.err.find("not a false value")!=std::string::npos,"failure treated as false");host.err.clear();
        require(!shell.execute("fail \"boom\"")&&host.err.find("boom")!=std::string::npos,"fail message lost");host.err.clear();
        run("data print \"continues after failure\"");

        // ---- Recursion safety -------------------------------------------------
        run("down := s() (if (s <= \"\") \"done\" (down (provide -regex \"replace\" \"^.\" \"\" s)))");
        require(text("down "+quote(std::string(1000,'x')))=="done","1000-level terminating recursion failed");
        run("forever := x() (if -true (forever x))");
        require(!shell.execute("forever 1")&&host.err.find("nested too deeply")!=std::string::npos,"uncontrolled recursion was not refused: "+host.err);host.err.clear();
        require(text("down \"xyz\"")=="done","shell unusable after refusing recursion");

        // ---- Exit status propagation ------------------------------------------
        {
            TestHost exit_host;Shell exiting(exit_host);
            exiting.execute("attempt (exit 4)");require(exiting.status()==4&&exiting.processes().quitting(),"attempt absorbed exit");
        }
        {
            write(root/"outer.seed","#!/usr/bin/env hirari\nrun \"inner.seed\"\ndata print \"after-run\"\n");
            write(root/"inner.seed","#!/usr/bin/env hirari\nwhile -true (exit 5)\ndata print \"after-loop\"\n");
            TestHost exit_host;Shell exiting(exit_host);
            exiting.execute_file(root/"outer.seed");
            require(exiting.status()==5&&exit_host.out.find("after")==std::string::npos,"exit did not unwind nested run/while: "+exit_host.out);
        }
        {
            TestHost exit_host;Shell exiting(exit_host);
            exiting.execute("for -i (data list 1 2 3) (if ((i <= 2) && (2 <= i)) (exit 6) (data print i))");
            require(exiting.status()==6&&exit_host.out=="1\n","exit did not stop a for loop: "+exit_host.out);
        }
        require(json("data get (attempt (exec "+hirari+" \"--eval\" \"exit 3\")) \"exit_status\"")==3,"hirari --eval exit status");
        write(root/"failing.seed","#!/usr/bin/env hirari\nfail \"entry rejected\"\n");
        run("data bind -script (attempt (exec "+hirari+" \"--file\" \"failing.seed\"))");
        require(json("data get script \"exit_status\"")==1&&text("data get script \"error\"").find("failing.seed:2: entry rejected")!=std::string::npos,"script failure diagnostic");
        std::cout<<"PASS job identity/live output/non-terminating wait/cancel/explicit timeout, environment, JSON, program fields, records, while, sleep, polling, attempt, fail, recursion refusal, exit propagation\n";
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';std::filesystem::current_path(std::filesystem::temp_directory_path());return 1;}
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    std::error_code error;std::filesystem::remove_all(root,error);return 0;
}
