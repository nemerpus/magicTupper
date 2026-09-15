#pragma once
#include <map>
#include <set>
#include <string>
#include <cstdint>
#include "jobs.hpp"
struct InstalledTitles {std::set<uint64_t> games;std::map<uint64_t,uint32_t> patches,addons;bool complete=false;};
inline uint64_t titleNumber(const std::string& value){
    if(value.size()!=16||value.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos)return 0;
    uint64_t result=0;for(char c:value){result=(result<<4)|(c>='0'&&c<='9'?c-'0':(c|32)-'a'+10);}return result;
}
inline int updateStatus(const Item& item,const InstalledTitles& installed){
    // 0 unknown/not applicable, 1 installed game, 2 newer version, 3 already covered.
    if(item.category!="updates"&&item.category!="dlc")return 0;
    const auto id=titleNumber(item.titleId),app=titleNumber(item.applicationId);
    if(!id||!app||!installed.games.count(app))return 0;
    if(item.category=="dlc"){
        if((id&0x1fff)<0x1000||((id^0x1000)&~uint64_t(0xfff))!=app)return 0;
        const auto addon=installed.addons.find(id);if(addon==installed.addons.end())return installed.complete?2:1;
        return item.version>=0&&uint64_t(item.version)>addon->second?(installed.complete?2:1):3;
    }
    if((id&0xfff)!=0x800||id-0x800!=app)return 0;
    auto patch=installed.patches.find(app);
    if(patch!=installed.patches.end()&&item.version>=0)return uint64_t(item.version)>patch->second?(installed.complete?2:1):3;
    if(installed.complete&&patch==installed.patches.end()&&item.version>0)return 2;
    return 1;
}
inline std::string updateLabel(int status){return status==3?"INSTALADO":status==1||status==2?"PARA MI JUEGO":"";}
inline bool updateMissing(const Item& item,const InstalledTitles& installed){const int status=updateStatus(item,installed);return installed.complete&&(status==2||(item.category=="updates"&&status==1&&!installed.patches.count(titleNumber(item.applicationId))));}
inline std::string updateBadge(const Item& item,const InstalledTitles& installed){
    if(item.category!="updates"&&item.category!="dlc")return "";
    if(updateMissing(item,installed))return item.category=="dlc"?"DLC DISPONIBLE":"ACTUALIZACIÓN DISPONIBLE";
    const int status=updateStatus(item,installed);if(status)return updateLabel(status);
    const auto id=titleNumber(item.titleId),app=titleNumber(item.applicationId);
    const bool related=item.category=="dlc"?(id&0x1fff)>=0x1000&&((id^0x1000)&~uint64_t(0xfff))==app:(id&0xfff)==0x800&&id-0x800==app;
    if(id&&app&&related&&installed.complete&&!installed.games.count(app))return "NO PROCEDE: FALTA EL JUEGO";
    return "ESTADO SIN CONFIRMAR";
}
inline bool updateVisible(const Item& item,const InstalledTitles& installed,int filter){return filter==0||(filter==1&&updateStatus(item,installed)!=0);}
inline bool enqueueUpdateBatch(std::deque<Task>& queue,const std::vector<Task>& tasks,const Task* active){
    if(tasks.empty())return false;
    auto next=queue;for(const auto& task:tasks)if(!enqueueJob(next,task,active))return false;queue=std::move(next);return true;
}
