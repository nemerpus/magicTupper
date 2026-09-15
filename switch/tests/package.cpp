#include "../source/package.hpp"
#include <cassert>
#include <iostream>

using namespace mtinstall;
using Bytes=std::vector<uint8_t>;
static void put(Bytes& b,size_t at,uint64_t v,size_t n){for(size_t i=0;i<n;i++)b.at(at+i)=v>>(8*i);}
static Bytes index(bool hfs,const std::vector<Entry>& entries){
    size_t strings=0;for(const auto& e:entries)strings+=e.name.size()+1;
    const size_t stride=hfs?64:24;Bytes b(16+entries.size()*stride+strings);
    memcpy(b.data(),hfs?"HFS0":"PFS0",4);put(b,4,entries.size(),4);put(b,8,strings,4);
    size_t nameOffset=0;
    for(size_t i=0;i<entries.size();i++){
        put(b,16+i*stride,entries[i].offset,8);put(b,24+i*stride,entries[i].size,8);put(b,32+i*stride,nameOffset,4);
        memcpy(b.data()+16+entries.size()*stride+nameOffset,entries[i].name.c_str(),entries[i].name.size()+1);nameOffset+=entries[i].name.size()+1;
    }
    return b;
}
static ReadAt reader(const Bytes& b){return [&b](uint64_t off,void* dest,size_t n){if(off>b.size()||n>b.size()-off)return false;memcpy(dest,b.data()+off,n);return true;};}
int main(){
    const std::string name="0123456789abcdef0123456789abcdef.nca";
    std::vector<Entry> out;std::string error;uint8_t id[16];
    assert(contentId(name,id)&&id[0]==1&&id[15]==0xef);
    assert(contentId(name.substr(0,32)+".cnmt.nca",id));
    assert(!contentId("../"+name,id));assert(!contentId(std::string(32,'g')+".nca",id));
    Bytes nsp=index(false,{{name,0,4096}});const size_t header=nsp.size();nsp.resize(header+4096);
    assert(package(reader(nsp),nsp.size(),".nsp",out,error));assert(out.size()==1&&out[0].offset==header&&out[0].size==4096);
    // The parser reads only the index, including for multi-GiB remote files.
    const uint64_t huge=uint64_t(9)*1024*1024*1024;
    Bytes large=index(false,{{name,0,huge}});size_t readBytes=0;
    ReadAt sparse=[&](uint64_t off,void* dest,size_t n){readBytes+=n;return reader(large)(off,dest,n);};
    assert(package(sparse,large.size()+huge,".nsp",out,error));assert(out[0].size==huge&&readBytes==large.size());
    Bytes bad=nsp;bad[0]='X';assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;put(bad,4,0xffffffff,4);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;put(bad,8,0xffffffff,4);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;put(bad,16,UINT64_MAX,8);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;put(bad,24,UINT64_MAX,8);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;put(bad,32,0xffffffff,4);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=nsp;bad[header-1]='x';assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=index(false,{{"../escape.nca",0,1}});bad.push_back(0);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=index(false,{{name,0,1},{name,1,1}});bad.resize(bad.size()+2);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    bad=index(false,{{"a",0,10},{"b",5,10}});bad.resize(bad.size()+15);assert(!package(reader(bad),bad.size(),".nsp",out,error));
    assert(!package(reader(nsp),nsp.size(),".nsz",out,error));
    Bytes inner=index(true,{{name,0,4096}});const size_t innerHeader=inner.size();inner.resize(innerHeader+4096);
    Bytes root=index(true,{{"secure",0,inner.size()}});const size_t rootHeader=root.size();root.insert(root.end(),inner.begin(),inner.end());
    Bytes xci(0xf000);memcpy(xci.data()+0x100,"HEAD",4);put(xci,0x130,0xf000,8);xci.insert(xci.end(),root.begin(),root.end());
    assert(package(reader(xci),xci.size(),".xci",out,error));assert(out[0].offset==0xf000+rootHeader+innerHeader);
    bad=xci;put(bad,0x130,UINT64_MAX,8);assert(!package(reader(bad),bad.size(),".xci",out,error));
    bad=xci;bad[0xf000+16+64]='X';assert(!package(reader(bad),bad.size(),".xci",out,error));
    assert(!package([](uint64_t,void*,size_t){return false;},1024,".nsp",out,error));
    std::cout<<"PASS: NSP/XCI, index-only reads, >4 GiB, malformed bounds, names, overlapping entries, missing secure partition, unsupported compression and read failures\n";
}
