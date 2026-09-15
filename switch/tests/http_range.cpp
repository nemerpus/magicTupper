#include "../source/http_range.hpp"
#include <cassert>
#include <iostream>
using namespace mtinstall;
int main(int argc,char** argv){
    assert(argc==2);curl_global_init(CURL_GLOBAL_DEFAULT);
    Hooks hooks;bool cancel=false;hooks.cancelled=[&](){return cancel;};
    hooks.progress=[](const std::string&,uint64_t,uint64_t){};
    std::string error;std::vector<std::string> logs;auto logger=[&](const std::string& s){logs.push_back(s);};
    const uint64_t size=uint64_t(9)*1024*1024*1024;
    const std::string base=argv[1];std::vector<uint8_t> bytes(blockSize+777);
    for(const char* mode:{"/ok","/cut"}){
        Remote remote(base+mode,"test-token",size,hooks,error,logger,[](){});
        const uint64_t offset=uint64_t(5)*1024*1024*1024+123;
        assert(remote.read(offset,bytes.data(),bytes.size()));
        for(size_t i=0;i<bytes.size();i++)assert(bytes[i]==(offset+i)%251);
    }
    assert(!logs.empty());
    for(const char* mode:{"/ignore","/wrong","/short","/over","/unauthorized"}){
        Remote remote(base+mode,"test-token",size,hooks,error,logger,[](){});
        assert(!remote.read(123,bytes.data(),99));
    }
    {
        Remote remote(base+"/ok","test-token",size,hooks,error,logger,[](){});
        assert(!remote.read(size-1,bytes.data(),2));
        cancel=true;assert(!remote.read(0,bytes.data(),1));
    }
    {
        Hooks during=hooks;int calls=0;during.cancelled=[&](){return ++calls>=3;};
        Remote remote(base+"/ok","test-token",size,during,error,logger,[](){});
        assert(!remote.read(0,bytes.data(),bytes.size()));assert(error=="Instalación cancelada.");
    }
    curl_global_cleanup();
    std::cout<<"PASS: exact HTTP ranges >4 GiB, interrupted block retry, ignored/wrong ranges, short/oversized bodies, 401, bounds and cancellation\n";
}
