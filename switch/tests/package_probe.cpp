#include "../source/package.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
using namespace mtinstall;
struct Segment{uint64_t offset;std::vector<uint8_t> data;};
int main(int argc,char** argv){
    assert(argc==3);std::ifstream file(argv[1],std::ios::binary);
    uint64_t size=0,count=0;file.read(reinterpret_cast<char*>(&size),8);file.read(reinterpret_cast<char*>(&count),8);assert(count<10);
    std::vector<Segment> segments;
    for(uint64_t i=0;i<count;i++){Segment s;uint64_t length=0;file.read(reinterpret_cast<char*>(&s.offset),8);file.read(reinterpret_cast<char*>(&length),8);assert(length<8*1024*1024);s.data.resize(length);file.read(reinterpret_cast<char*>(s.data.data()),length);segments.push_back(std::move(s));}
    assert(file.good());
    ReadAt read=[&](uint64_t offset,void* data,size_t length){for(const auto& s:segments)if(offset>=s.offset&&offset-s.offset<=s.data.size()&&length<=s.data.size()-(offset-s.offset)){memcpy(data,s.data.data()+offset-s.offset,length);return true;}return false;};
    std::vector<Entry> entries;std::string error;
    if(!package(read,size,argv[2],entries,error)){std::cerr<<error<<"\n";return 1;}
    size_t meta=0,nca=0;for(const auto& e:entries){if(ends(e.name,".cnmt.nca"))meta++;if(ends(e.name,".nca"))nca++;}
    assert(meta>0&&nca>=meta);std::cout<<"PASS: "<<nca<<" NCAs, "<<meta<<" CNMTs, source size="<<size<<"\n";
}
