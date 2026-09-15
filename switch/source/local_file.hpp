#pragma once
#include "install.hpp"
#include <cstdio>
#include <cerrno>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace mtinstall {
class LocalFile {
    FILE* file=nullptr;uint64_t size;const Hooks& hooks;std::string& error;
public:
    LocalFile(const std::string& path,uint64_t expected,const Hooks& cb,std::string& out):size(expected),hooks(cb),error(out){
        // libusbhsfs/FatFs implements stat(path) and seek, but fstat(fd) returns ENOSYS.
        struct stat info{};
        if(stat(path.c_str(),&info)!=0){error="USB: no se pudo consultar el archivo (errno="+std::to_string(errno)+").";return;}
        if(!S_ISREG(info.st_mode)){error="USB: la ruta no es un archivo normal.";return;}
        if(info.st_size<0||static_cast<uint64_t>(info.st_size)!=size){error="USB: tamaño cambiado. Esperado="+std::to_string(size)+", actual="+std::to_string(info.st_size)+".";return;}
        file=fopen(path.c_str(),"rb");if(!file){error="USB: no se pudo abrir el archivo (errno="+std::to_string(errno)+").";return;}
        auto fail=[&](const std::string& text){error=text;fclose(file);file=nullptr;};
        // Also validate the opened stream, in case the path changed between stat and fopen.
        if(fseeko(file,0,SEEK_END)!=0){fail("USB: no se pudo consultar el tamaño abierto (errno="+std::to_string(errno)+").");return;}
        const off_t openedSize=ftello(file);
        if(openedSize<0){fail("USB: no se pudo obtener la posición (errno="+std::to_string(errno)+").");return;}
        if(static_cast<uint64_t>(openedSize)!=size){fail("USB: el tamaño del archivo abierto no coincide con la cola.");return;}
        if(fseeko(file,0,SEEK_SET)!=0){fail("USB: no se pudo volver al inicio (errno="+std::to_string(errno)+").");return;}
    }
    ~LocalFile(){if(file)fclose(file);}
    LocalFile(const LocalFile&)=delete;LocalFile& operator=(const LocalFile&)=delete;
    bool ready() const{return file!=nullptr;}
    bool read(uint64_t offset,void* dest,size_t length){
        if(!file)return false;
        if(hooks.cancelled()){error="Instalación cancelada.";return false;}
        if(offset>size||length>size-offset||offset>static_cast<uint64_t>(std::numeric_limits<off_t>::max())){error="Lectura USB fuera del paquete.";return false;}
        if(fseeko(file,static_cast<off_t>(offset),SEEK_SET)!=0||fread(dest,1,length,file)!=length){error="No se pudo leer el USB. Comprueba que sigue conectado.";return false;}
        return true;
    }
};
}
