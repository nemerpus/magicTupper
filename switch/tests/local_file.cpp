#include "../source/local_file.hpp"
#include "../source/package.hpp"
#include <cassert>
#include <array>

// Model the USB driver's unsupported descriptor metadata operation.
extern "C" int __wrap_fstat(int,struct stat*){errno=ENOSYS;return -1;}

int main(int argc,char** argv){
    assert(argc==2);const std::string path=argv[1];std::string error;bool cancelled=false;
    mtinstall::Hooks hooks;hooks.cancelled=[&](){return cancelled;};
    std::array<unsigned char,48> bytes{};memcpy(bytes.data(),"PFS0",4);bytes[4]=1;bytes[8]=4;bytes[24]=4;memcpy(bytes.data()+40,"a\0\0\0DATA",8);
    FILE* f=fopen(path.c_str(),"wb");assert(f);assert(fwrite(bytes.data(),1,bytes.size(),f)==bytes.size());assert(fclose(f)==0);
    struct stat unsupported{};assert(fstat(-1,&unsupported)==-1&&errno==ENOSYS);
    {
        mtinstall::LocalFile file(path,48,hooks,error);assert(file.ready());std::vector<mtinstall::Entry> entries;
        assert(mtinstall::package([&](uint64_t p,void* b,size_t n){return file.read(p,b,n);},48,".nsp",entries,error));
        assert(entries.size()==1&&entries[0].size==4);char data[4];assert(file.read(entries[0].offset,data,4));assert(memcmp(data,"DATA",4)==0);
        assert(!file.read(47,data,4));cancelled=true;assert(!file.read(0,data,4));cancelled=false;
    }
    {mtinstall::LocalFile file(path,49,hooks,error);assert(!file.ready());}
    {mtinstall::LocalFile directory("/tmp",48,hooks,error);assert(!directory.ready());}
    const uint64_t large=5ULL*1024*1024*1024;
    f=fopen(path.c_str(),"wb");assert(f);assert(fseeko(f,large,SEEK_SET)==0);assert(fputc(123,f)!=EOF);assert(fclose(f)==0);
    {mtinstall::LocalFile file(path,large+1,hooks,error);assert(file.ready());unsigned char byte=0;assert(file.read(large,&byte,1)&&byte==123);}
    remove(path.c_str());{mtinstall::LocalFile file(path,48,hooks,error);assert(!file.ready());}
    puts("PASS: local NSP reader, bounds, cancellation, changed/missing files and offsets above 4 GiB");
}
