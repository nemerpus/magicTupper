#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "package.hpp"

namespace mtinstall {
inline bool validDestination(int value){return value==4||value==5;}
struct Hooks {
    std::function<bool()> cancelled;
    std::function<void(const std::string&,uint64_t,uint64_t)> progress;
};
bool installLocal(const std::string& path,const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error);
// Instala desde cualquier lector aleatorio (USB/TCP/HTTP/local) sin temporal intermedio.
bool installReader(const ReadAt& read,const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error);
// NCM user storage only: 5 = SD, 4 = internal user memory, never system storage.
bool install(const std::string& url,const std::string& token,const std::string& id,
             const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error);
}
