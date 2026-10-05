#include "hirari/unity_client.hpp"
#include "hirari/win32.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <thread>
namespace hirari {
struct UnityClient::Impl {
    Host& host;std::string endpoint,session;unsigned timeout_ms;std::int64_t next_id=1;
    Impl(Host& h,std::string e,unsigned t):host(h),endpoint(std::move(e)),timeout_ms(t){}
#ifdef _WIN32
    win32::Handle pipe;
    using Clock=std::chrono::steady_clock;
    void transfer(void* buffer,std::size_t length,bool writing,Clock::time_point deadline) {
        auto* bytes=static_cast<std::uint8_t*>(buffer);
        while(length) {
            OVERLAPPED op{};win32::Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
            if(!event)throw std::runtime_error("cannot create Unity I/O event");op.hEvent=event.get();DWORD done=0;
            BOOL ok=writing?WriteFile(pipe.get(),bytes,static_cast<DWORD>(length),&done,&op)
                           :ReadFile(pipe.get(),bytes,static_cast<DWORD>(length),&done,&op);
            if(!ok) {
                auto error=GetLastError();
                if(error!=ERROR_IO_PENDING)throw std::runtime_error("Unity pipe I/O: "+win32::windows_error(error));
                for(;;) {
                    if(host.interrupted()||Clock::now()>=deadline) {
                        CancelIoEx(pipe.get(),&op);GetOverlappedResult(pipe.get(),&op,&done,TRUE);
                        throw std::runtime_error("Unity request cancelled or timed out; operation may have executed");
                    }
                    auto ready=WaitForSingleObject(event.get(),30);
                    if(ready==WAIT_TIMEOUT)continue;
                    if(ready!=WAIT_OBJECT_0) {
                        CancelIoEx(pipe.get(),&op);GetOverlappedResult(pipe.get(),&op,&done,TRUE);
                        throw std::runtime_error("Unity pipe wait failed");
                    }
                    if(!GetOverlappedResult(pipe.get(),&op,&done,FALSE))throw std::runtime_error("Unity pipe I/O: "+win32::windows_error(GetLastError()));
                    break;
                }
            }
            if(!done)throw std::runtime_error("Unity disconnected");bytes+=done;length-=done;
        }
    }
    wire::Message exchange(wire::Message m) {
        if(next_id==std::numeric_limits<std::int64_t>::max())throw std::runtime_error("Unity request id exhausted");
        m.id=next_id++;auto body=wire::pack(m);auto n=static_cast<std::uint32_t>(body.size());std::array<std::uint8_t,4> header{};
        for(unsigned i=0;i<4;++i)header[i]=static_cast<std::uint8_t>(n>>(8*i));
        auto deadline=Clock::now()+std::chrono::milliseconds(timeout_ms);
        transfer(header.data(),4,true,deadline);transfer(body.data(),body.size(),true,deadline);transfer(header.data(),4,false,deadline);
        n=0;for(unsigned i=0;i<4;++i)n|=static_cast<std::uint32_t>(header[i])<<(8*i);
        if(n<13||n>wire::max_frame)throw std::runtime_error("invalid Unity frame length");body.resize(n);transfer(body.data(),n,false,deadline);
        auto reply=wire::unpack(body);if(reply.id!=m.id)throw std::runtime_error("Unity response id mismatch");return reply;
    }
#endif
};
UnityClient::UnityClient(Host& host,std::string endpoint,unsigned timeout_ms):impl_(std::make_unique<Impl>(host,std::move(endpoint),timeout_ms)) {
    if(!timeout_ms)throw std::runtime_error("Unity timeout must be positive");
    if(impl_->endpoint.empty()) {
#ifdef _WIN32
        DWORD n=GetEnvironmentVariableW(L"SKUNK_UNITY_PIPE",nullptr,0);
        if(n){std::wstring value(n,L'\0');DWORD c=GetEnvironmentVariableW(L"SKUNK_UNITY_PIPE",value.data(),n);if(c&&c<n){value.resize(c);impl_->endpoint=win32::narrow(value);}}
#else
        if(auto* env=std::getenv("SKUNK_UNITY_PIPE"))impl_->endpoint=env;
#endif
        if(impl_->endpoint.empty())impl_->endpoint="skunkworks.unity";
    }
    if(impl_->endpoint.find_first_of("\\/:")!=std::string::npos||impl_->endpoint.find('\0')!=std::string::npos)
        throw std::runtime_error("SKUNK_UNITY_PIPE must be a local pipe name");
}
UnityClient::~UnityClient()=default;
void UnityClient::disconnect() noexcept {
#ifdef _WIN32
    impl_->pipe.reset();
#endif
    impl_->session.clear();
}
bool UnityClient::connected()const noexcept{return !impl_->session.empty();}
const std::string& UnityClient::endpoint()const noexcept{return impl_->endpoint;}
const std::string& UnityClient::session()const noexcept{return impl_->session;}
void UnityClient::connect() {
    if(connected())return;
#ifdef _WIN32
    try {
        auto path=L"\\\\.\\pipe\\"+win32::widen(impl_->endpoint);auto deadline=Impl::Clock::now()+std::chrono::milliseconds(impl_->timeout_ms);
        for(;;) {
            impl_->pipe.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
                                         FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr));
            if(impl_->pipe)break;auto error=GetLastError();
            if(error!=ERROR_PIPE_BUSY&&error!=ERROR_FILE_NOT_FOUND)throw std::runtime_error("Unity connect: "+win32::windows_error(error));
            if(impl_->host.interrupted()||Impl::Clock::now()>=deadline)throw std::runtime_error("Unity pipe unavailable: "+impl_->endpoint);
            if(error==ERROR_PIPE_BUSY)WaitNamedPipeW(path.c_str(),30);else std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        auto reply=impl_->exchange({wire::MessageKind::Hello,0,{wire::Value::string(wire::protocol)}});
        if(reply.kind!=wire::MessageKind::Ready||reply.values.size()!=2||reply.values[0].kind!=wire::ValueKind::String||
           reply.values[1].kind!=wire::ValueKind::String||std::get<std::string>(reply.values[0].data)!=wire::protocol)
            throw std::runtime_error("Unity protocol handshake rejected");
        auto session=std::get<std::string>(reply.values[1].data);
        // ControlServer uses Guid.ToString("N"); accept D form peers too.
        // Preserve the exact session string for foreign reference guards.
        if(session.size()!=32&&session.size()!=36)throw std::runtime_error("invalid Unity session GUID");
        for(std::size_t i=0;i<session.size();++i) {
            bool dash=session.size()==36&&(i==8||i==13||i==18||i==23);auto c=session[i];
            if(dash?c!='-':!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))throw std::runtime_error("invalid Unity session GUID");
        }
        impl_->session=std::move(session);
    }catch(...){disconnect();throw;}
#else
    throw std::runtime_error("Unity named pipes require Windows");
#endif
}
wire::Message UnityClient::request(wire::Message m) {
    connect();
#ifdef _WIN32
    wire::Message reply{wire::MessageKind::Fault,0,{}};
    try {reply=impl_->exchange(std::move(m));if(reply.kind!=wire::MessageKind::Result&&reply.kind!=wire::MessageKind::Fault)throw std::runtime_error("unexpected Unity response kind");}
    catch(...){disconnect();throw;}
    if(reply.kind==wire::MessageKind::Fault) {
        if(reply.values.empty()||reply.values[0].kind!=wire::ValueKind::String)throw std::runtime_error("invalid Unity fault");
        throw std::runtime_error("Unity: "+std::get<std::string>(reply.values[0].data));
    }
    return reply;
#else
    (void)m;throw std::runtime_error("Unity named pipes require Windows");
#endif
}
}
