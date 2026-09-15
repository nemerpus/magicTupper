#pragma once
#include <mutex>
#include <json-c/json.h>

static std::mutex pcExplorerMutex;
static std::string pcExplorerRequest,pcExplorerResponse;
static uint64_t pcExplorerId=0;
static Uint32 pcExplorerStarted=0;
static bool pcInstallable(std::string name){
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return std::tolower(c);});
    return name.size()>4&&(name.substr(name.size()-4)==".nsp"||name.substr(name.size()-4)==".xci");
}
static std::string pcExplorerField(json_object* obj,const char* key){
    json_object* value=nullptr;
    if(!obj||!json_object_object_get_ex(obj,key,&value)||!json_object_is_type(value,json_type_string))return "";
    return std::string(json_object_get_string(value),static_cast<size_t>(json_object_get_string_len(value)));
}
static bool pcExplorerValidResponse(json_object* obj,uint64_t expected){
    json_object *id=nullptr,*ok=nullptr,*data=nullptr,*entries=nullptr;
    if(!obj||!json_object_is_type(obj,json_type_object)||!json_object_object_get_ex(obj,"id",&id)||!json_object_is_type(id,json_type_int)||
        json_object_get_int64(id)<0||static_cast<uint64_t>(json_object_get_int64(id))!=expected||
        !json_object_object_get_ex(obj,"ok",&ok)||!json_object_is_type(ok,json_type_boolean))return false;
    if(!json_object_object_get_ex(obj,"data",&data))return true;
    if(!json_object_is_type(data,json_type_object)||!json_object_object_get_ex(data,"entries",&entries)||
        !json_object_is_type(entries,json_type_array)||json_object_array_length(entries)>128)return false;
    for(const char* key:{"offset","next"}){
        json_object* page=nullptr;
        if(!json_object_object_get_ex(data,key,&page)||!json_object_is_type(page,json_type_int))return false;
        auto number=json_object_get_int64(page);
        if(number<-1||number>1000128||(number!=-1&&number%128!=0))return false;
    }
    for(size_t i=0;i<json_object_array_length(entries);++i){
        auto* entry=json_object_array_get_idx(entries,i);json_object *dir=nullptr,*size=nullptr;
        const auto path=pcExplorerField(entry,"path"),name=pcExplorerField(entry,"name");
        if(name.empty()||name.size()>1024||name.find('/')!=std::string::npos||!validRelativePath(name)||path.size()>4096||path.rfind("pc:/",0)!=0||!validRelativePath(path.substr(4))||
            !json_object_object_get_ex(entry,"directory",&dir)||!json_object_is_type(dir,json_type_boolean)||
            !json_object_object_get_ex(entry,"size",&size)||!json_object_is_type(size,json_type_int)||json_object_get_int64(size)<0)return false;
    }
    return true;
}
static void pcExplorerAsk(const std::string& path,const char* op="list",int offset=0){
    std::lock_guard<std::mutex> lock(pcExplorerMutex);
    auto* obj=json_object_new_object();
    json_object_object_add(obj,"id",json_object_new_int64(++pcExplorerId));
    json_object_object_add(obj,"path",json_object_new_string(path.c_str()));
    json_object_object_add(obj,"op",json_object_new_string(op));
    json_object_object_add(obj,"offset",json_object_new_int(offset));
    pcExplorerRequest=json_object_to_json_string_ext(obj,JSON_C_TO_STRING_PLAIN);
    json_object_put(obj);pcExplorerResponse.clear();pcExplorerStarted=SDL_GetTicks();
}
