#include "hirari/native_library.hpp"
#include "hirari/win32.hpp"
#include "hirari/world.hpp"
#include "hirari/host.hpp"
#include <stdexcept>
namespace hirari {
struct NativeLibrary::Impl {
#ifdef _WIN32
    HMODULE module=nullptr;
    using Invoke=const char* (*)(const char*,std::size_t,std::size_t*);
    Invoke invoke=nullptr;
    using Cancellable=const char* (*)(const char*,std::size_t,std::size_t*,int (*)(void*),void*);
    Cancellable cancellable=nullptr;
    using Shutdown=void (*)();Shutdown shutdown=nullptr;
    ~Impl(){if(module){if(shutdown)shutdown();FreeLibrary(module);}}
#endif
};
NativeLibrary::NativeLibrary(const std::string& path):impl_(std::make_unique<Impl>()) {
#ifdef _WIN32
    auto full=std::filesystem::absolute(text_path(path));
    impl_->module=LoadLibraryExW(full.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!impl_->module)throw std::runtime_error("cannot load capability library: "+win32::windows_error(GetLastError()));
    impl_->invoke=reinterpret_cast<Impl::Invoke>(GetProcAddress(impl_->module,"capability_invoke_v1"));
    impl_->cancellable=reinterpret_cast<Impl::Cancellable>(GetProcAddress(impl_->module,"capability_invoke_cancellable_v1"));
    impl_->shutdown=reinterpret_cast<Impl::Shutdown>(GetProcAddress(impl_->module,"capability_shutdown_v1"));
    if(!impl_->invoke)throw std::runtime_error("library does not export capability_invoke_v1");
#else
    (void)path;throw std::runtime_error("native capability libraries are currently supported on Windows");
#endif
}
NativeLibrary::~NativeLibrary()=default;
nlohmann::json NativeLibrary::invoke(const std::vector<nlohmann::json>& arguments, Host& host) {
#ifdef _WIN32
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
#else
    (void)arguments;throw std::runtime_error("native capability libraries require Windows");
#endif
}
}
