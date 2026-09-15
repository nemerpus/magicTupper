#pragma once
#include "explorer.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <functional>
#include <set>

struct AppFile {std::string path,id,sha256;uint64_t size=0;};
inline bool validAppPath(const std::string& path){
    if(!validRelativePath(path)||path.size()>220||path.find_first_of("<>\"|?*")!=std::string::npos)return false;
    size_t start=0;while(start<path.size()){auto end=path.find('/',start);auto part=path.substr(start,end==std::string::npos?end:end-start);if(part.front()=='.'||part.back()=='.'||part.back()==' ')return false;if(end==std::string::npos)break;start=end+1;}return true;
}
inline bool appPlan(json_object* data,std::vector<AppFile>& files,std::string& executable,uint64_t& total){
    json_object *list=nullptr,*value=nullptr;files.clear();total=0;executable=sourceText(data,"executable");
    if(!validAppPath(executable)||executable.size()<4||executable.substr(executable.size()-4)!=".nro"||!json_object_object_get_ex(data,"version",&value)||json_object_get_int(value)!=1||!json_object_object_get_ex(data,"files",&list)||!json_object_is_type(list,json_type_array)||json_object_array_length(list)==0||json_object_array_length(list)>2048)return false;
    const auto slash=executable.find('/');const std::string prefix=slash==std::string::npos?"":executable.substr(0,slash+1);std::set<std::string> paths;bool hasExecutable=false;
    for(size_t i=0;i<json_object_array_length(list);i++){
        auto* row=json_object_array_get_idx(list,i);AppFile file{sourceText(row,"path"),sourceText(row,"id"),sourceText(row,"sha256"),0};
        if(!validAppPath(file.path)||(prefix.empty()?file.path!=executable:file.path.rfind(prefix,0)!=0)||file.id.size()!=64||file.sha256.size()!=64||file.id.find_first_not_of("0123456789abcdef")!=std::string::npos||file.sha256.find_first_not_of("0123456789abcdef")!=std::string::npos||!json_object_object_get_ex(row,"size",&value)||!json_object_is_type(value,json_type_int)||json_object_get_int64(value)<0)return false;
        file.size=json_object_get_int64(value);if(file.size>=0x100000000ull||file.size>2ull*1024*1024*1024-total)return false;total+=file.size;
        auto key=file.path;for(char& c:key)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));if(!paths.insert(key).second)return false;
        hasExecutable|=file.path==executable;files.push_back(file);
    }
    // Reject a file also used as a parent directory (including FAT case aliases).
    for(const auto& path:paths)for(size_t slash=path.find('/');slash!=std::string::npos;slash=path.find('/',slash+1))if(paths.count(path.substr(0,slash)))return false;
    return hasExecutable;
}
inline bool appParents(const std::string& root,const std::string& relative,bool create){
    std::string current=root;size_t begin=0;
    for(size_t slash=relative.find('/');slash!=std::string::npos;slash=relative.find('/',begin)){
        current+="/"+relative.substr(begin,slash-begin);struct stat stat{};
        if(lstat(current.c_str(),&stat)==0){if(!S_ISDIR(stat.st_mode)||S_ISLNK(stat.st_mode))return false;}
        else if(errno!=ENOENT)return false;else if(create&&mkdir(current.c_str(),0777)!=0&&errno!=EEXIST)return false;
        begin=slash+1;
    }return true;
}
// Existing files must match. A different installed version is preserved, never overwritten.
inline bool installAppFiles(const std::vector<AppFile>& files,const std::string& root,const std::string& stage,
    const std::function<bool(const AppFile&,const std::string&)>& fetch,
    const std::function<std::string(const std::string&)>& digest,const std::function<bool()>& cancelled,std::string& error){
    std::vector<size_t> missing,published;
    for(size_t i=0;i<files.size();i++){
        if(!validAppPath(files[i].path)||!appParents(root,files[i].path,false)){error="App: ruta de destino no válida.";return false;}
        struct stat info{};const auto target=root+"/"+files[i].path;
        if(lstat(target.c_str(),&info)==0){if(!S_ISREG(info.st_mode)||uint64_t(info.st_size)!=files[i].size||digest(target)!=files[i].sha256){error="App distinta ya presente: "+files[i].path+". Se conserva la versión existente.";return false;}}
        else if(errno==ENOENT)missing.push_back(i);else {error="No se pudo comprobar la App instalada.";return false;}
    }
    if(missing.empty())return true;
    struct stat stageInfo{};
    if(lstat(stage.c_str(),&stageInfo)==0||errno!=ENOENT){error="Hay una instalación de App pendiente de revisar en "+stage;return false;}
    if(mkdir(stage.c_str(),0777)!=0){error="No se pudo crear el temporal de la App.";return false;}
    auto cleanup=[&](){for(size_t i:missing)std::remove((stage+"/"+std::to_string(i)).c_str());rmdir(stage.c_str());};
    for(size_t i:missing){const auto part=stage+"/"+std::to_string(i);if(cancelled()||!fetch(files[i],part)||digest(part)!=files[i].sha256){if(error.empty())error="App cancelada o archivo con integridad incorrecta.";cleanup();return false;}}
    auto order=missing;std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b){auto nro=[&](size_t i){const auto& p=files[i].path;return p.size()>=4&&p.substr(p.size()-4)==".nro";};return nro(a)<nro(b);});
    for(size_t i:order){const auto target=root+"/"+files[i].path;struct stat info{};
        if(cancelled()||!appParents(root,files[i].path,true)||lstat(target.c_str(),&info)==0||errno!=ENOENT||rename((stage+"/"+std::to_string(i)).c_str(),target.c_str())!=0){
            error="No se pudo publicar la App. Se retiraron los archivos añadidos en este intento.";for(size_t done:published)std::remove((root+"/"+files[done].path).c_str());cleanup();return false;
        }published.push_back(i);
    }
    cleanup();return true;
}
