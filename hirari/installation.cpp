#include "hirari/installation.hpp"
#include "hirari/win32.hpp"
#include <filesystem>
#include <stdexcept>
namespace hirari {
void initialize_installation() {
#ifdef _WIN32
    // Launchers select PATH, providers, profiles and writable state directories.
    // A standalone shell only identifies its own installation.
    if(GetEnvironmentVariableW(L"HIRARI_HOME",nullptr,0))return;
    std::wstring module(32768,L'\0');
    auto count=GetModuleFileNameW(nullptr,module.data(),static_cast<DWORD>(module.size()));
    if(!count||count>=module.size())throw std::runtime_error("cannot locate Hirari executable");
    module.resize(count);
    auto directory=std::filesystem::path(module).parent_path().wstring();
    if(!SetEnvironmentVariableW(L"HIRARI_HOME",directory.c_str()))throw std::runtime_error("cannot set HIRARI_HOME");
#endif
}
}
