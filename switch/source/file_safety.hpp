#pragma once
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

inline bool safeTransferName(const std::string& name){
    if(name.empty()||name.size()>255||name=="."||name==".."||name.back()=='.'||name.back()==' ')return false;
    for(unsigned char c:name)if(c<32||c==127||c=='/'||c=='\\'||c==':')return false;
    return true;
}
inline bool safeSdPath(const std::string& path){
    if(path.size()>4096||path.rfind("sdmc:/",0)!=0)return false;
    size_t start=6;
    while(start<path.size()){
        auto end=path.find('/',start);
        if(!safeTransferName(path.substr(start,end==std::string::npos?end:end-start)))return false;
        if(end==std::string::npos)return true;
        start=end+1;
    }
    return true;
}
inline FILE* createTransferStage(const std::string& target,std::string& stage){
    stage=target+".mt-XXXXXX";
    int fd=mkstemp(&stage[0]);
    if(fd<0){stage.clear();return nullptr;}
    FILE* file=fdopen(fd,"wb");
    if(!file){close(fd);std::remove(stage.c_str());stage.clear();}
    return file;
}
inline bool publishNewTransfer(const std::string& stage,const std::string& target){
    struct stat info{};
    if(lstat(target.c_str(),&info)==0){errno=EEXIST;return false;}
    if(errno!=ENOENT)return false;
    // Never unlink the destination. A failed rename must leave existing data intact.
    return std::rename(stage.c_str(),target.c_str())==0;
}
