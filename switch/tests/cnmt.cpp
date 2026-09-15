#include "../source/cnmt.hpp"
#include <cassert>
#include <iostream>
using namespace mtinstall;
static void put(std::vector<uint8_t>& b,size_t off,uint64_t v,size_t n){for(size_t i=0;i<n;i++)b.at(off+i)=v>>(8*i);}
static std::vector<uint8_t> make(uint8_t type){
    const size_t ext=type==0x81?24:16;std::vector<uint8_t> b(32+ext+2*56+16+32);
    put(b,0,0x0100000000010000ULL+(type==0x81?0x800:type==0x82?0x1001:0),8);put(b,8,65536,4);b[12]=type;
    put(b,14,ext,2);put(b,16,2,2);put(b,18,1,2);b[20]=2;
    put(b,32,0x0100000000010000ULL,8);
    for(size_t i=0;i<2;i++){
        const size_t at=32+ext+i*56;for(size_t j=0;j<32;j++)b[at+j]=j+i;
        b[at+32]=i+1;put(b,at+48,uint64_t(5)*1024*1024*1024+i,5);b[at+54]=i==0?1:6;
    }
    b[32+ext+112]=0x44;return b;
}
int main(){
    ParsedMeta result;std::string error;std::array<uint8_t,24> own{};own[0]=9;own[16]=7;
    for(uint8_t type:{0x80,0x81,0x82}){
        const auto bytes=make(type);assert(cnmt(bytes,own,result,error));
        assert(result.type==type&&result.application==0x0100000000010000ULL&&result.version==65536);
        assert(result.contents.size()==1&&result.contents[0].size==uint64_t(5)*1024*1024*1024);
        assert(le(result.database.data()+2,2)==2&&le(result.database.data()+4,2)==1);
        const size_t ext=type==0x81?24:16;assert(result.database.size()==8+ext+48+16);
        assert(result.database[8+ext]==9&&result.database[8+ext+24]==1&&result.database.back()==0);
        assert(result.database[8+ext+48]==0x44);assert(result.database[6]==2);
    }
    auto b=make(0x81);b.insert(b.end()-32,4,0x55);put(b,44,4,4);assert(cnmt(b,own,result,error));assert(result.database.back()==0x55);
    put(b,44,UINT32_MAX,4);assert(!cnmt(b,own,result,error));
    b=make(0x80);b.resize(b.size()-1);assert(!cnmt(b,own,result,error));
    b=make(0x80);put(b,14,65535,2);assert(!cnmt(b,own,result,error));
    b=make(0x80);put(b,16,65535,2);assert(!cnmt(b,own,result,error));
    b=make(0x80);b[12]=3;assert(!cnmt(b,own,result,error));
    b=make(0x80);b[13]=1;assert(!cnmt(b,own,result,error));
    b=make(0x80);b[22]=1;assert(!cnmt(b,own,result,error));
    b=make(0x80);b[7]=0;assert(!cnmt(b,own,result,error));
    b=make(0x80);b[32+16+56+32]=1;b[32+16+56+54]=1;assert(!cnmt(b,own,result,error));
    b=make(0x80);b[32+16+54]=7;assert(!cnmt(b,own,result,error));
    assert(!cnmt({},own,result,error));
    std::cout<<"PASS: application/update/DLC CNMT, delta filtering, >4 GiB contents, metadata tails, hashes and malformed/truncated records\n";
}
