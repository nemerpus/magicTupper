#pragma once
#include "jobs.hpp"
#include <set>
struct BrowserEntry {std::string name,path;bool directory=false;int64_t size=0;std::string itemId;};
inline bool validRelativePath(const std::string& path){
    if(path.empty()||path.size()>4096||path.front()=='/'||path.find_first_of("\\:")!=std::string::npos)return false;
    for(unsigned char c:path)if(c<32)return false;
    size_t start=0;while(start<path.size()){auto end=path.find('/',start);auto part=path.substr(start,end==std::string::npos?end:end-start);if(part.empty()||part=="."||part=="..")return false;if(end==std::string::npos)return true;start=end+1;}return false;
}
inline std::vector<BrowserEntry> serverEntries(const std::vector<Item>& items,const std::string& directory){
    std::vector<BrowserEntry> result;if(directory.rfind("server:/",0)!=0)return result;
    // Without relative paths we cannot reconstruct real folders; never invent a flat filesystem.
    if(std::any_of(items.begin(),items.end(),[](const Item& item){return item.relativePath.empty();}))return result;
    auto prefix=directory.substr(8);if(!prefix.empty()){if(!validRelativePath(prefix))return result;prefix+="/";}
    std::set<std::string> folders;
    for(const auto& item:items){const auto& path=item.relativePath.empty()?item.filename:item.relativePath;if(!validRelativePath(path)||path.rfind(prefix,0)!=0)continue;
        auto tail=path.substr(prefix.size());auto slash=tail.find('/');
        if(slash!=std::string::npos){auto name=tail.substr(0,slash);if(folders.insert(name).second)result.push_back({name,"server:/"+prefix+name,true,0,""});}
        else result.push_back({tail,"server:/"+path,false,item.size,item.id});
    }
    std::sort(result.begin(),result.end(),[](const BrowserEntry& a,const BrowserEntry& b){return a.directory!=b.directory?a.directory>b.directory:a.name<b.name;});return result;
}
inline std::string browserHeading(const std::string& path){
    if(path.rfind("pc:/",0)==0)return "PC / C:/"+path.substr(4);
    if(path=="explorer:/")return "Explorador / Elige una ubicación";
    if(path=="usb:/")return "USB / Unidades conectadas";
    const auto split=path.find(":/");const auto prefix=path.substr(0,split);
    std::string title=prefix=="sdmc"?"Tarjeta SD":prefix=="server"?"Servidor / Biblioteca":"USB / "+prefix;
    if(split!=std::string::npos&&split+2<path.size()){
        auto tail=path.substr(split+2);for(char c:tail){if(c=='/')title+=" / ";else title+=c;}
        // Separate the root label from the first folder.
        const auto rootSize=(prefix=="sdmc"?std::string("Tarjeta SD"):prefix=="server"?std::string("Servidor / Biblioteca"):"USB / "+prefix).size();title.insert(rootSize," / ");
    }
    return title;
}
inline std::string browserParent(const std::string& path){
    if(path=="explorer:/")return path;
    if(path=="sdmc:/"||path=="usb:/"||path=="server:/"||path=="pc:/")return "explorer:/";
    if(path.rfind("ums",0)==0&&path.find(":/")==path.size()-2)return "usb:/";
    const auto slash=path.find_last_of('/');if(slash==std::string::npos)return "explorer:/";
    auto parent=path.substr(0,slash);if(!parent.empty()&&parent.back()==':')parent+="/";return parent;
}
