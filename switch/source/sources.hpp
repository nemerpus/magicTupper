#pragma once
#include <json-c/json.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <string>
#include <vector>

// Role is runtime-only and comes from /api/me, never from editable sources.json.
struct Source { std::string id,name,url,user,token,role; };
struct Sources {
    std::vector<Source> entries;
    std::string active;
    long long nextId=1;
    Source* find(const std::string& id){for(auto& source:entries)if(source.id==id)return &source;return nullptr;}
    std::string allocateId(){return std::to_string(nextId++);}
};
static std::string sourceText(json_object* object,const char* name){
    json_object* value=nullptr;
    return object&&json_object_object_get_ex(object,name,&value)&&json_object_is_type(value,json_type_string)?json_object_get_string(value):"";
}
static bool sourceToken(const std::string& token){
    return token.empty()||(token.size()==64&&token.find_first_not_of("0123456789abcdef") == std::string::npos);
}
// Sources are API origins. Credentials and redirects are never inferred.
static bool sourceUrl(std::string& url){
    while(!url.empty()&&url.back()=='/')url.pop_back();
    const size_t prefix=url.rfind("https://",0)==0?8:url.rfind("http://",0)==0?7:0;
    if(url.size()>255||prefix==0)return false;
    const auto host=url.substr(prefix);
    for(unsigned char c:host)if(c<=32||c==127)return false;
    return !host.empty()&&host.find_first_of(" /\\\t\r\n?#@") == std::string::npos;
}
static bool saveSources(const Sources& sources,const std::string& path){
    if(sources.entries.size()>20||sources.nextId>999999999999LL)return false;
    for(const auto& source:sources.entries)if(source.name.size()>255||source.user.size()>255||!sourceToken(source.token))return false;
    auto* root=json_object_new_object();auto* list=json_object_new_array();
    json_object_object_add(root,"version",json_object_new_int(1));
    json_object_object_add(root,"nextId",json_object_new_int64(sources.nextId));
    json_object_object_add(root,"active",json_object_new_string(sources.active.c_str()));
    for(const auto& source:sources.entries){
        auto* entry=json_object_new_object();
        json_object_object_add(entry,"id",json_object_new_string(source.id.c_str()));
        json_object_object_add(entry,"name",json_object_new_string(source.name.c_str()));
        json_object_object_add(entry,"url",json_object_new_string(source.url.c_str()));
        json_object_object_add(entry,"user",json_object_new_string(source.user.c_str()));
        if(!source.token.empty())json_object_object_add(entry,"token",json_object_new_string(source.token.c_str()));
        json_object_array_add(list,entry);
    }
    json_object_object_add(root,"sources",list);
    const std::string body=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PRETTY);json_object_put(root);
    const std::string temporary=path+".tmp";
    FILE* file=fopen(temporary.c_str(),"wb");if(!file)return false;
    bool ok=fwrite(body.data(),1,body.size(),file)==body.size();if(fclose(file)!=0)ok=false;
    if(!ok)return false;
    // FAT does not replace an existing file consistently with rename().
    if(rename(temporary.c_str(),path.c_str())==0)return true;
    if(remove(path.c_str())!=0&&errno!=ENOENT){remove(temporary.c_str());return false;}
    if(rename(temporary.c_str(),path.c_str())==0)return true;
    // Some SD drivers still reject rename after removing the old entry.
    file=fopen(path.c_str(),"wb");if(!file){remove(temporary.c_str());return false;}
    const bool written=fwrite(body.data(),1,body.size(),file)==body.size();
    const bool closed=fclose(file)==0;
    ok=written&&closed;
    if(!ok){remove(temporary.c_str());return false;}
    return remove(temporary.c_str())==0||errno==ENOENT;
}
static bool loadSources(Sources& sources,const std::string& path){
    auto* root=json_object_from_file(path.c_str());if(!root)return false;
    json_object *list=nullptr,*counter=nullptr;
    bool ok=json_object_object_get_ex(root,"sources",&list)&&json_object_is_type(list,json_type_array)&&json_object_array_length(list)<=20;
    Sources loaded;
    if(ok){
        for(size_t i=0;i<json_object_array_length(list);i++){
            auto* entry=json_object_array_get_idx(list,i);
            Source source{sourceText(entry,"id"),sourceText(entry,"name"),sourceText(entry,"url"),sourceText(entry,"user"),""};
            source.token=sourceText(entry,"token");
            if(source.id.empty()||source.id.size()>12||source.id.front()=='0'||source.id.find_first_not_of("0123456789")!=std::string::npos||source.name.empty()||source.name.size()>255||source.user.size()>255||!sourceUrl(source.url)||!sourceToken(source.token)||loaded.find(source.id)){ok=false;break;}
            for(const auto& previous:loaded.entries)if(previous.url==source.url)ok=false;
            if(!ok)break;
            loaded.nextId=std::max(loaded.nextId,static_cast<long long>(strtoll(source.id.c_str(),nullptr,10)+1));
            loaded.entries.push_back(source);
        }
        if(json_object_object_get_ex(root,"nextId",&counter))loaded.nextId=std::max(loaded.nextId,static_cast<long long>(json_object_get_int64(counter)));
        if(loaded.nextId>999999999999LL)ok=false;
        loaded.active=sourceText(root,"active");
        if(!loaded.find(loaded.active))loaded.active=loaded.entries.empty()?"":loaded.entries.front().id;
    }
    json_object_put(root);if(ok)sources=loaded;return ok;
}
