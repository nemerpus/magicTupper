#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <vector>

// Bounded, platform-independent readers for PFS0 (NSP) and HFS0 (XCI).
namespace mtinstall {
using ReadAt=std::function<bool(uint64_t,void*,size_t)>;
struct Entry { std::string name; uint64_t offset=0,size=0; };
inline uint64_t le(const uint8_t* p,size_t n){uint64_t v=0;for(size_t i=0;i<n;i++)v|=uint64_t(p[i])<<(8*i);return v;}
inline bool ends(const std::string& s,const std::string& suffix){return s.size()>=suffix.size()&&s.compare(s.size()-suffix.size(),suffix.size(),suffix)==0;}
inline bool table(const ReadAt& read,uint64_t base,uint64_t length,bool hfs,std::vector<Entry>& entries,std::string& error){
    auto bad=[&](){error="Índice del paquete inválido o fuera de límites.";return false;};
    uint8_t head[16];if(length<16)return bad();if(!read(base,head,sizeof(head)))return false;
    if(std::memcmp(head,hfs?"HFS0":"PFS0",4))return bad();
    const uint64_t count=le(head+4,4),strings=le(head+8,4),stride=hfs?64:24;
    if(!count||count>8192||strings>4*1024*1024)return bad();
    const uint64_t headerSize=16+count*stride+strings;
    if(headerSize>length)return bad();
    std::vector<uint8_t> header(static_cast<size_t>(headerSize-16));
    if(!read(base+16,header.data(),header.size()))return false;
    std::set<std::string> names;std::vector<Entry> parsed;
    for(size_t i=0;i<count;i++){
        const uint8_t* p=header.data()+i*stride;
        const uint64_t offset=le(p,8),size=le(p+8,8),nameOffset=le(p+16,4);
        if(nameOffset>=strings||offset>length-headerSize||size>length-headerSize-offset)return bad();
        const char* name=reinterpret_cast<const char*>(header.data()+count*stride+nameOffset);
        const char* end=static_cast<const char*>(std::memchr(name,0,strings-nameOffset));
        if(!end||end==name||end-name>255)return bad();
        const std::string value(name,end);
        if(value=="."||value==".."||value.find_first_of("/\\:")!=std::string::npos||!names.insert(value).second)return bad();
        parsed.push_back({value,base+headerSize+offset,size});
    }
    auto sorted=parsed;std::sort(sorted.begin(),sorted.end(),[](const Entry& a,const Entry& b){return a.offset<b.offset;});
    for(size_t i=1;i<sorted.size();i++)if(sorted[i-1].size>sorted[i].offset-sorted[i-1].offset)return bad();
    entries=std::move(parsed);return true;
}
inline bool package(const ReadAt& read,uint64_t size,const std::string& extension,std::vector<Entry>& entries,std::string& error){
    if(extension==".nsp")return table(read,0,size,false,entries,error);
    if(extension!=".xci"){error="Instalación directa: usa NSP o XCI sin comprimir.";return false;}
    uint8_t head[0x140];
    if(size<sizeof(head)){error="Cabecera XCI incompleta.";return false;}
    if(!read(0,head,sizeof(head)))return false;
    if(std::memcmp(head+0x100,"HEAD",4)){error="Cabecera XCI inválida.";return false;}
    const uint64_t root=le(head+0x130,8);
    if(root<sizeof(head)||root>size){error="Partición XCI fuera de límites.";return false;}
    std::vector<Entry> partitions;if(!table(read,root,size-root,true,partitions,error))return false;
    for(const auto& entry:partitions)if(entry.name=="secure")return table(read,entry.offset,entry.size,true,entries,error);
    error="El XCI no contiene una partición secure.";return false;
}
inline bool contentId(const std::string& name,uint8_t* out){
    if(name.size()!=36&&name.size()!=41)return false;
    if(name.substr(32)!=".nca"&&name.substr(32)!=".cnmt.nca")return false;
    for(size_t i=0;i<16;i++){
        auto hex=[](char c)->int{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;};
        int a=hex(name[2*i]),b=hex(name[2*i+1]);if(a<0||b<0)return false;out[i]=uint8_t(a*16+b);
    }
    return true;
}
}
