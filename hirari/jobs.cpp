#include "hirari/jobs.hpp"
#include "hirari/value_wire.hpp"
#include "hirari/world.hpp"
#include "hirari/win32.hpp"
#include <fstream>
#include <chrono>
#ifdef _WIN32
#include <bcrypt.h>
#endif
namespace hirari {
namespace {
using Clock=std::chrono::steady_clock;
std::string identity(const Json& value){auto kind=value.at("kind").get<std::string>();if(kind=="name")return identity(value.at("value"));if(kind=="symbol"||kind=="literal")return value.at("value").get<std::string>();throw std::runtime_error("job operation requires an identity");}
std::string digest(std::string_view text) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("SHA-256 unavailable");
    struct Cleanup{BCRYPT_ALG_HANDLE& a;BCRYPT_HASH_HANDLE& h;~Cleanup(){if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);}} cleanup{algorithm,hash};
    unsigned char result[32];
    if(text.size()>ULONG_MAX||BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)<0||BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(text.data())),static_cast<ULONG>(text.size()),0)<0||BCryptFinishHash(hash,result,32,0)<0)throw std::runtime_error("SHA-256 failed");
    std::string output;for(auto b:result){output+="0123456789abcdef"[b>>4];output+="0123456789abcdef"[b&15];}return output;
#else
    (void)text;throw std::runtime_error("jobs require Windows");
#endif
}
}
Jobs::Jobs(Values& values,Host& host):host_(host) {
    values.link("jobs","General background jobs: start EXECUTABLE ARGUMENT_LIST [OPTIONS], status ID, wait ID [TIMEOUT_MS], cancel ID, forget ID, list. Options: parameters, timeout_ms, directory, slot, revision, cache_directory, cache_key, structured. A job runs until it exits or is cancelled; only an explicit timeout_ms terminates it. wait returns the job state with wait=\"completed\" or wait=\"timeout\" and never stops the job. State carries pid, live stdout/stderr, exit_status and elapsed_ms. Workers receive hirari.jobs/1 JSON on stdin; matching progress/result/error records on stdout are observed. Shell-owned jobs stop on shell exit.",[this](const auto& args){return invoke(args);});
}
Jobs::~Jobs(){for(auto& [id,job]:jobs_)job->cancelled=true;for(auto& [id,job]:jobs_)if(job->worker.joinable())job->worker.join();}
std::shared_ptr<Jobs::Job> Jobs::find(const Json& argument){
    auto id=unpack_value(argument);if(!id.is_string())throw std::runtime_error("job ID must be text");
    auto found=jobs_.find(id.get<std::string>());if(found==jobs_.end())throw std::runtime_error("unknown job ID");return found->second;
}
Json Jobs::snapshot(const std::shared_ptr<Job>& job){
    std::lock_guard lock(job->mutex);auto state=job->state;
    if(!job->done)state["elapsed_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-job->started).count();
    auto slot=state.at("slot").get<std::string>();
    auto found=latest_.find(slot);
    bool current=slot.empty()||(found!=latest_.end()&&found->second==state.at("id").get<std::string>());
    state["current"]=current;
    if(!current){state["state"]="superseded";state["result"]=nullptr;}
    return state;
}
Json Jobs::invoke(const std::vector<Json>& arguments) {
    if(arguments.empty())throw std::runtime_error("job operation required");auto op=identity(arguments[0]);
    if(op=="list"&&arguments.size()==1){Json list=Json::array();for(auto& [id,job]:jobs_)list.push_back(snapshot(job));return pack_value(list);}
    if(op=="wait") {
        if(arguments.size()!=2&&arguments.size()!=3)throw std::runtime_error("job wait ID [TIMEOUT_MS]");
        auto job=find(arguments[1]);std::optional<Clock::time_point> deadline;
        if(arguments.size()==3) {
            auto limit=unpack_value(arguments[2]);
            if(!limit.is_number_integer()||limit.get<long long>()<0||limit.get<long long>()>86400000)throw std::runtime_error("job wait timeout must be 0..86400000 ms");
            deadline=Clock::now()+std::chrono::milliseconds(limit.get<long long>());
        }
        // Waiting observes; it never owns the job's lifetime. A wait deadline
        // returns the live state and leaves the process running.
        while(!job->done) {
            if(host_.interrupted())throw Interruption("job wait interrupted; the job is still running");
            if(deadline&&Clock::now()>=*deadline)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        auto state=snapshot(job);state["wait"]=job->done?"completed":"timeout";return pack_value(state);
    }
    if(op=="status"||op=="cancel"||op=="forget") {
        if(arguments.size()!=2)throw std::runtime_error("job status/cancel/forget requires an ID");
        auto job=find(arguments[1]);auto id=job->state.at("id").get<std::string>();
        if(op=="cancel"){job->cancelled=true;std::lock_guard lock(job->mutex);if(!job->done)job->state["state"]="cancelling";}
        if(op=="forget"){if(!job->done)throw std::runtime_error("cancel and wait before forgetting a running job");if(job->worker.joinable())job->worker.join();auto slot=job->state.at("slot").get<std::string>();if(!slot.empty()&&latest_[slot]==id)latest_.erase(slot);jobs_.erase(id);return pack_value(nullptr);}
        return pack_value(snapshot(job));
    }
    if(op!="start"||(arguments.size()!=3&&arguments.size()!=4))throw std::runtime_error("job start EXECUTABLE ARGUMENT_LIST [OPTIONS]");
    std::size_t active=0;for(auto& [id,job]:jobs_)if(!job->done)++active;
    if(active>=8||jobs_.size()>=256)throw std::runtime_error("job limit reached: 8 active / 256 retained; cancel or forget jobs");
    auto executable=ProcessHost::executable_path(unpack_value(arguments[1]).get<std::string>());
    if(executable.empty())throw std::runtime_error("job executable not found");
    executable=path_text(std::filesystem::absolute(text_path(executable)));
    auto argv=unpack_value(arguments[2]).get<std::vector<std::string>>();
    Json options=arguments.size()==4?unpack_value(arguments[3]):Json::object();
    if(!options.is_object())throw std::runtime_error("job options must be a record");
    for(auto f=options.begin();f!=options.end();++f)if(f.key()!="parameters"&&f.key()!="timeout_ms"&&f.key()!="directory"&&f.key()!="slot"&&f.key()!="revision"&&f.key()!="cache_directory"&&f.key()!="cache_key"&&f.key()!="structured")throw std::runtime_error("unknown job option: "+f.key());
    // No implicit lifetime limit: an owned job runs until it exits, is cancelled,
    // or the shell exits. Only an explicit timeout_ms terminates it.
    std::optional<long long> timeout;
    if(options.contains("timeout_ms")) {
        auto& limit=options.at("timeout_ms");
        if(!limit.is_number_integer()||limit.get<long long>()<1||limit.get<long long>()>86400000)throw std::runtime_error("job timeout must be 1..86400000 ms");
        timeout=limit.get<long long>();
    }
    std::string slot=options.value("slot",std::string{}),revision=options.value("revision",std::string{});
    std::string id="job-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(Clock::now().time_since_epoch().count())+"-"+std::to_string(sequence_++);
    Json request={{"protocol","hirari.jobs/1"},{"request_id",id},{"revision",revision},{"parameters",options.value("parameters",Json::object())}};
    std::string input=request.dump()+"\n";if(input.size()>1024*1024)throw std::runtime_error("job parameters exceed 1 MiB");
    auto cwd=std::filesystem::current_path();std::filesystem::path cache;
    if(options.contains("directory")) {
        if(!options.at("directory").is_string())throw std::runtime_error("job directory must be text");
        cwd=std::filesystem::absolute(text_path(options.at("directory").get<std::string>()));
        if(!std::filesystem::is_directory(cwd))throw std::runtime_error("job directory does not exist: "+path_text(cwd));
    }
    auto cache_key=options.value("cache_key",std::string{});
    if(!cache_key.empty()){
        if(!options.contains("cache_directory"))throw std::runtime_error("cache_key requires cache_directory");
        auto directory=std::filesystem::absolute(text_path(options.at("cache_directory").get<std::string>()));
        // Caller supplies immutable input/code digests in cache_key. The rest of
        // the invocation is also included; filenames alone are not content hashes.
        auto key=Json{{"executable",executable},{"arguments",argv},{"cwd",path_text(cwd)},{"parameters",request.at("parameters")},{"revision",revision},{"inputs",cache_key}}.dump();
        std::filesystem::create_directories(directory);cache=directory/(digest(key)+".json");
    }
    auto job=std::make_shared<Job>();job->state={{"id",id},{"state","starting"},{"pid",nullptr},{"executable",executable},{"arguments",argv},{"directory",path_text(cwd)},
        {"timeout_ms",timeout?Json(*timeout):Json(nullptr)},{"timed_out",false},{"slot",slot},{"revision",revision},{"progress",nullptr},{"result",nullptr},{"error",nullptr},
        {"stdout",""},{"stderr",""},{"exit_status",nullptr},{"cache_hit",false},{"elapsed_ms",0}};
    if(!slot.empty()) {
        auto old=latest_.find(slot);if(old!=latest_.end()) {auto previous=jobs_.find(old->second);if(previous!=jobs_.end()){previous->second->obsolete=true;previous->second->cancelled=true;}}
        latest_[slot]=id;
    }
    jobs_.emplace(id,job);
    if(!cache.empty()) {
        try{std::ifstream file(cache,std::ios::binary);if(file){if(std::filesystem::file_size(cache)>16*1024*1024)throw std::runtime_error("oversized cache");Json stored;file>>stored;file>>std::ws;if(!file.eof()||stored.at("protocol")!="hirari.job-cache/1")throw std::runtime_error("invalid cache");job->state["result"]=stored.at("result");job->state["state"]="succeeded";job->state["exit_status"]=0;job->state["cache_hit"]=true;job->done=true;return pack_value(snapshot(job));}}
        catch(...){/* An incomplete/corrupt cache is a miss, never a completion. */}
    }
    try {job->worker=std::thread([job,id,executable,argv,request,input,timeout,cwd,cache,structured=options.value("structured",false)] {
        auto started=job->started;bool received=false;Json answer=nullptr;std::string worker_error;
        struct WorkerHost:Host {
            std::shared_ptr<Job> job;std::optional<Clock::time_point> deadline;
            WorkerHost(std::shared_ptr<Job> value,std::optional<Clock::time_point> limit):job(std::move(value)),deadline(limit){}
            void write(std::string_view)override{}void error(std::string_view)override{}
            bool expired()const{return deadline&&Clock::now()>=*deadline;}
            bool interrupted()override{return job->cancelled||expired();}
        } host(job,timeout?std::optional<Clock::time_point>(started+std::chrono::milliseconds(*timeout)):std::nullopt);
        try {
            {std::lock_guard lock(job->mutex);job->state["state"]="running";}
            // A private arena only packages argv/process results; no user source
            // is parsed or evaluated on the worker thread.
            seed::Runtime runtime;ProcessHost process(runtime,host);
            process.worker(cwd.wstring(),[&](std::string_view chunk,bool error){
                if(chunk.empty())return;
                // Live output stays observable while the job runs.
                {std::lock_guard lock(job->mutex);job->state[error?"stderr":"stdout"].get_ref<std::string&>().append(chunk);}
                if(error)return;job->pending.append(chunk);
                std::size_t at;
                while((at=job->pending.find('\n'))!=std::string::npos) {
                    if(at>1024*1024)throw std::runtime_error("job protocol line exceeds 1 MiB");
                    auto line=job->pending.substr(0,at);job->pending.erase(0,at+1);auto message=Json::parse(line,nullptr,false);
                    if(message.is_discarded()||!message.is_object()||message.value("protocol",std::string{})!="hirari.jobs/1")continue;
                    if(message.at("request_id")!=id)throw std::runtime_error("job completion request ID mismatch");
                    auto kind=message.at("kind").get<std::string>();
                    if(kind=="progress"){std::lock_guard lock(job->mutex);job->state["progress"]=message.at("value");}
                    else if(kind=="result"){if(received)throw std::runtime_error("duplicate job result");received=true;answer=message.at("value");}
                    else if(kind=="error")worker_error=message.at("message").get<std::string>();
                    else throw std::runtime_error("unknown job protocol message");
                }
                if(job->pending.size()>1024*1024)throw std::runtime_error("job protocol line exceeds 1 MiB");
            },[&](unsigned long pid){std::lock_guard lock(job->mutex);job->state["pid"]=pid;});
            std::vector<seed::Ptr> args;for(auto& arg:argv)args.push_back(runtime.arena().make_literal(arg));
            ProcessResult feed;feed.out=input;auto value=process.execute(runtime,executable,args,nullptr,std::move(feed));ProcessResult output;
            if(!ProcessHost::unpack(runtime,value,output))throw std::runtime_error("background process returned no result");
            std::lock_guard lock(job->mutex);auto& state=job->state;state["stdout"]=output.out;state["stderr"]=output.err;state["exit_status"]=output.status;
            if(job->obsolete)state["state"]="superseded";
            else if(job->cancelled)state["state"]="cancelled";
            else if(host.expired()){state["state"]="failed";state["error"]="job timed out";state["timed_out"]=true;}
            else if(output.status||!worker_error.empty()||(structured&&!received)){state["state"]="failed";state["error"]=!worker_error.empty()?worker_error:structured&&!received?"worker produced no structured result":"worker exited "+std::to_string(output.status);}
            else {state["state"]="succeeded";state["result"]=received?answer:Json(output.out);
                if(!cache.empty()) {
                    auto temporary=cache;temporary+=std::filesystem::path("."+id+".tmp");
                    try{std::ofstream file(temporary,std::ios::binary);file<<Json{{"protocol","hirari.job-cache/1"},{"result",state.at("result")}}.dump();file.flush();if(!file)throw std::runtime_error("cache write failed");file.close();if(job->obsolete||job->cancelled){std::filesystem::remove(temporary);}else {std::error_code ec;std::filesystem::rename(temporary,cache,ec);if(ec)std::filesystem::remove(temporary);}}
                    catch(...){std::error_code ec;std::filesystem::remove(temporary,ec);state["cache_warning"]="cache publication failed";}
                }
            }
        }catch(const std::exception& error){std::lock_guard lock(job->mutex);job->state["state"]=job->obsolete?"superseded":job->cancelled?"cancelled":"failed";job->state["error"]=error.what();}
        {std::lock_guard lock(job->mutex);job->state["elapsed_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-started).count();}
        job->done=true;
    });}catch(...){jobs_.erase(id);throw;}
    return pack_value(snapshot(job));
}
}
