#pragma once
#include "package.hpp"
#include <array>

namespace mtinstall {
struct MetaContent {
    std::array<uint8_t,32> hash{};
    std::array<uint8_t,24> info{};
    uint64_t size=0;
};
struct ParsedMeta {
    uint64_t id=0,application=0;uint32_t version=0;uint8_t type=0;
    std::vector<MetaContent> contents;
    std::vector<uint8_t> database;
};
inline bool cnmt(const std::vector<uint8_t>& bytes,const std::array<uint8_t,24>& own,ParsedMeta& out,std::string& error){
    auto bad=[&](const char* message){error=message;return false;};
    if(bytes.size()<32)return bad("Cabecera CNMT incompleta.");
    const auto* p=bytes.data();ParsedMeta result;
    result.id=le(p,8);result.version=le(p+8,4);result.type=p[12];
    const size_t ext=le(p+14,2),count=le(p+16,2),metaCount=le(p+18,2);
    if((result.type!=0x80&&result.type!=0x81&&result.type!=0x82)||p[13]!=0||p[22]!=0)return bad("Tipo de título no compatible: solo juegos, updates y DLC completos.");
    if((result.id>>56)!=1||ext<16||ext>bytes.size()-32||count>8192||metaCount>8192||32+ext+count*56+metaCount*16>bytes.size())return bad("Estructura CNMT fuera de límites.");
    result.application=result.type==0x80?result.id:le(p+32,8);
    if((result.application>>56)!=1)return bad("El paquete no es una aplicación de usuario.");
    std::set<std::array<uint8_t,16>> ids;
    for(size_t i=0;i<count;i++){
        const uint8_t* record=p+32+ext+i*56;
        if(record[32+22]==6)continue; // Delta fragments are not full-install content.
        if(record[32+22]>5)return bad("Tipo de contenido CNMT no compatible.");
        MetaContent content;memcpy(content.hash.data(),record,32);memcpy(content.info.data(),record+32,24);content.size=le(record+32+16,5);
        std::array<uint8_t,16> id{};memcpy(id.data(),content.info.data(),16);
        if(!content.size||!ids.insert(id).second)return bad("Contenido CNMT vacío o duplicado.");
        result.contents.push_back(content);
    }
    const size_t tail=32+ext+count*56;size_t extendedData=0;
    if(result.type==0x81){if(ext<24)return bad("Cabecera de actualización incompleta.");extendedData=le(p+44,4);}
    const size_t remaining=bytes.size()-tail-metaCount*16;
    if(remaining<32||extendedData>remaining-32)return bad("Datos extendidos o resumen CNMT incompletos.");
    result.database.resize(8+ext+(result.contents.size()+1)*24+metaCount*16+extendedData);
    auto* dest=result.database.data();
    dest[0]=ext;dest[1]=ext>>8;
    const size_t installedCount=result.contents.size()+1;dest[2]=installedCount;dest[3]=installedCount>>8;
    dest[4]=metaCount;dest[5]=metaCount>>8;dest[6]=p[20];dest[7]=0;dest+=8;
    memcpy(dest,p+32,ext);dest+=ext;
    memcpy(dest,own.data(),24);dest+=24;
    for(const auto& content:result.contents){memcpy(dest,content.info.data(),24);dest+=24;}
    memcpy(dest,p+tail,metaCount*16+extendedData);
    out=std::move(result);return true;
}
}
