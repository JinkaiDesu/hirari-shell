#include "hirari/native_library.hpp"
#include "hirari/world.hpp"
#include "hirari/host.hpp"
#include <stdexcept>
#ifdef _WIN32
#include "hirari/win32.hpp"
#else
#include <dlfcn.h>
#endif
namespace hirari {
namespace {
#ifdef _WIN32
using Module=HMODULE;
Module open_library(const std::filesystem::path& full){
    auto module=LoadLibraryExW(full.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module)throw std::runtime_error("cannot load capability library: "+win32::windows_error(GetLastError()));
    return module;
}
template<class F>F symbol(Module module,const char* name){return reinterpret_cast<F>(GetProcAddress(module,name));}
void close_library(Module module){FreeLibrary(module);}
#else
using Module=void*;
Module open_library(const std::filesystem::path& full){
    auto module=dlopen(full.c_str(),RTLD_NOW|RTLD_LOCAL);
    if(!module)throw std::runtime_error(std::string("cannot load capability library: ")+dlerror());
    return module;
}
template<class F>F symbol(Module module,const char* name){return reinterpret_cast<F>(dlsym(module,name));}
void close_library(Module module){dlclose(module);}
#endif
}
struct NativeLibrary::Impl {
    Module module=nullptr;
    using Invoke=const char* (*)(const char*,std::size_t,std::size_t*);
    Invoke invoke=nullptr;
    using Cancellable=const char* (*)(const char*,std::size_t,std::size_t*,int (*)(void*),void*);
    Cancellable cancellable=nullptr;
    using Shutdown=void (*)();Shutdown shutdown=nullptr;
    ~Impl(){if(module){if(shutdown)shutdown();close_library(module);}}
};
NativeLibrary::NativeLibrary(const std::string& path):impl_(std::make_unique<Impl>()) {
    auto full=std::filesystem::absolute(text_path(path));
    impl_->module=open_library(full);
    impl_->invoke=symbol<Impl::Invoke>(impl_->module,"capability_invoke_v1");
    impl_->cancellable=symbol<Impl::Cancellable>(impl_->module,"capability_invoke_cancellable_v1");
    impl_->shutdown=symbol<Impl::Shutdown>(impl_->module,"capability_shutdown_v1");
    if(!impl_->invoke)throw std::runtime_error("library does not export capability_invoke_v1");
}
NativeLibrary::~NativeLibrary()=default;
nlohmann::json NativeLibrary::invoke(const std::vector<nlohmann::json>& arguments, Host& host) {
    auto request=nlohmann::json{{"protocol","hirari.values/1"},{"arguments",arguments}}.dump();
    if(request.size()>16*1024*1024)throw std::runtime_error("capability request exceeds 16 MiB");
    auto cancelled=[](void* context)->int {return static_cast<Host*>(context)->interrupted()?1:0;};
    std::size_t length=0;
    const char* bytes=impl_->cancellable?impl_->cancellable(request.data(),request.size(),&length,cancelled,&host):impl_->invoke(request.data(),request.size(),&length);
    if(!bytes || length>16*1024*1024)throw std::runtime_error("invalid capability library response size");
    auto envelope=nlohmann::json::parse(bytes,bytes+length);
    if(envelope.at("protocol")!="hirari.values/1")throw std::runtime_error("capability library protocol mismatch");
    if(envelope.contains("error"))throw std::runtime_error(envelope.at("error").get<std::string>());
    return envelope.at("value");
}
}
