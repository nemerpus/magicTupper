#pragma once
#include "sources.hpp"
#include <deque>
#include <cstdint>

struct Item {std::string id,title,filename,category;int64_t size=0;std::string relativePath;std::string titleId,applicationId;int64_t version=-1;};
struct Task {
    int kind=0;std::string url,token,user,password;Item item;std::string sourceId,localPath;
    int destination=5;std::string role="standard";std::string usbDevice;
};
inline bool localTask(const Task& task){return task.kind==4||task.kind==10;}
inline bool transferTask(const Task& task){return (task.kind>=2&&task.kind<=4)||task.kind==10||task.kind==12;}
inline std::string taskLabel(const Task& task){return task.kind==12?"App y recursos / SD":(task.kind==3||task.kind==10)?(task.destination==4?"Instalar / interna":"Instalar / SD"):task.kind==4?"Copiar USB / SD":"Descargar / SD";}
inline bool sameJob(const Task& a,const Task& b){return a.kind==b.kind&&a.sourceId==b.sourceId&&a.item.id==b.item.id&&a.destination==b.destination&&a.localPath==b.localPath;}
inline bool enqueueJob(std::deque<Task>& queue,const Task& task,const Task* active){
    if(queue.size()>=50||(active&&sameJob(task,*active)))return false;
    for(const auto& old:queue)if(sameJob(task,old))return false;
    queue.push_back(task);return true;
}
inline bool saveJobs(const std::deque<Task>& queue,const Task* active,bool paused,const std::string& path){
    json_object* root=json_object_new_object();auto* list=json_object_new_array();
    json_object_object_add(root,"version",json_object_new_int(1));json_object_object_add(root,"paused",json_object_new_boolean(paused));
    auto append=[&](const Task& task){
        if(task.kind!=2&&task.kind!=3&&task.kind!=12)return;
        auto* job=json_object_new_object();
        json_object_object_add(job,"kind",json_object_new_int(task.kind));json_object_object_add(job,"destination",json_object_new_int(task.destination));
        json_object_object_add(job,"source",json_object_new_string(task.sourceId.c_str()));json_object_object_add(job,"id",json_object_new_string(task.item.id.c_str()));
        json_object_object_add(job,"title",json_object_new_string(task.item.title.c_str()));json_object_object_add(job,"filename",json_object_new_string(task.item.filename.c_str()));
        json_object_object_add(job,"size",json_object_new_int64(task.item.size));json_object_array_add(list,job);
        if(task.item.category=="browser"){json_object_object_add(job,"browserPath",json_object_new_string(task.item.relativePath.c_str()));}
    };
    if(active)append(*active);
    for(const auto& task:queue)append(task);
    json_object_object_add(root,"jobs",list);
    const std::string body=json_object_to_json_string_ext(root,JSON_C_TO_STRING_PRETTY);json_object_put(root);
    const std::string tmp=path+".tmp",backup=path+".bak";
    FILE* f=fopen(tmp.c_str(),"wb");if(!f)return false;
    const bool written=fwrite(body.data(),1,body.size(),f)==body.size();const bool closed=fclose(f)==0;if(!written||!closed)return false;
    // Retain a recoverable previous generation when FAT cannot replace by rename.
    if(rename(tmp.c_str(),path.c_str())==0)return true;
    if(remove(backup.c_str())!=0&&errno!=ENOENT)return false;
    if(rename(path.c_str(),backup.c_str())!=0&&errno!=ENOENT)return false;
    if(rename(tmp.c_str(),path.c_str())==0)return true;
    rename(backup.c_str(),path.c_str());return false;
}
inline bool loadJobs(std::deque<Task>& queue,bool& paused,const std::string& path){
    json_object* root=json_object_from_file(path.c_str());
    if(!root)root=json_object_from_file((path+".bak").c_str());
    if(!root)return false;
    json_object *list=nullptr,*value=nullptr;bool ok=json_object_object_get_ex(root,"version",&value)&&json_object_get_int(value)==1&&
        json_object_object_get_ex(root,"jobs",&list)&&json_object_is_type(list,json_type_array)&&json_object_array_length(list)<=51;
    std::deque<Task> loaded;
    if(ok)for(size_t i=0;i<json_object_array_length(list);i++){
        auto* job=json_object_array_get_idx(list,i);Task task;
        task.sourceId=sourceText(job,"source");task.item.id=sourceText(job,"id");task.item.title=sourceText(job,"title");task.item.filename=sourceText(job,"filename");
        task.item.relativePath=sourceText(job,"browserPath");if(!task.item.relativePath.empty())task.item.category="browser";
        if(task.item.relativePath.size()>4096||task.item.relativePath.find_first_of("\\:")!=std::string::npos||(!task.item.relativePath.empty()&&task.item.relativePath.front()=='/')){ok=false;break;}
        if(!json_object_object_get_ex(job,"kind",&value)||!json_object_is_type(value,json_type_int)){ok=false;break;}task.kind=json_object_get_int(value);
        if(!json_object_object_get_ex(job,"size",&value)||!json_object_is_type(value,json_type_int)){ok=false;break;}task.item.size=json_object_get_int64(value);
        if(!json_object_object_get_ex(job,"destination",&value)||!json_object_is_type(value,json_type_int)){ok=false;break;}task.destination=json_object_get_int(value);
        if((task.kind!=2&&task.kind!=3&&task.kind!=12)||(task.destination!=4&&task.destination!=5)||((task.kind==2||task.kind==12)&&task.destination!=5)||task.item.size<0||
            task.item.id.size()!=64||task.item.id.find_first_not_of("0123456789abcdef")!=std::string::npos||task.sourceId.empty()||task.sourceId.size()>12||task.sourceId.find_first_not_of("0123456789")!=std::string::npos||
            task.item.title.size()>1024||task.item.filename.empty()||task.item.filename.size()>255||task.item.filename.find_first_of("/\\:")!=std::string::npos){ok=false;break;}
        for(const auto& old:loaded)if(sameJob(old,task))ok=false;
        if(!ok)break;
        loaded.push_back(task);
    }
    if(ok){paused=json_object_object_get_ex(root,"paused",&value)&&json_object_get_boolean(value);queue=std::move(loaded);}
    json_object_put(root);return ok;
}
