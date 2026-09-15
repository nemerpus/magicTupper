#pragma once
#include "install.hpp"
#include "package.hpp"
#include <curl/curl.h>
#include "http_security.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
namespace mtinstall {
constexpr size_t blockSize=1024*1024;
class Remote {
    CURL* curl=nullptr;curl_slist* headers=nullptr;
    const Hooks& hooks;std::string& error;uint64_t size;std::function<void(const std::string&)> logger;std::function<void()> wait;
    struct Block {uint8_t* dest;size_t wanted,received=0;uint64_t first,last,total;bool range=false,valid=false;long status=0;};
    Block active{};char detail[CURL_ERROR_SIZE]{};
    static size_t header(char* p,size_t a,size_t b,void* data){
        auto& block=*static_cast<Block*>(data);const size_t n=a*b;std::string line(p,n);
        if(line.rfind("HTTP/",0)==0){block.range=false;block.valid=false;const auto space=line.find(' ');block.status=space==std::string::npos?0:strtol(line.c_str()+space+1,nullptr,10);}
        std::string lower=line;for(char& c:lower)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(lower.rfind("content-range:",0)==0){
            unsigned long long first=0,last=0,total=0;char extra=0;
            block.range=sscanf(lower.c_str()+14," bytes %llu-%llu/%llu %c",&first,&last,&total,&extra)==3&&
                first==block.first&&last==block.last&&total==block.total;
        }
        if(line=="\r\n"||line=="\n")block.valid=block.status==206&&block.range;
        return n;
    }
    static size_t write(char* p,size_t a,size_t b,void* data){
        auto& block=*static_cast<Block*>(data);size_t n=a*b;
        if(!block.valid||n>block.wanted-block.received)return 0;
        memcpy(block.dest+block.received,p,n);block.received+=n;return n;
    }
    static int progress(void* data,curl_off_t,curl_off_t,curl_off_t,curl_off_t){return static_cast<Remote*>(data)->hooks.cancelled()?1:0;}
public:
    Remote(const std::string& url,const std::string& token,uint64_t length,const Hooks& cb,std::string& out,std::function<void(const std::string&)> logFn,std::function<void()> waitFn):hooks(cb),error(out),size(length),logger(std::move(logFn)),wait(std::move(waitFn)){
        curl=curl_easy_init();if(!curl)return;
        if(!configureHttpSecurity(curl)){curl_easy_cleanup(curl);curl=nullptr;error="No se pudo configurar TLS.";return;}
        headers=curl_slist_append(headers,("Authorization: Bearer "+token).c_str());
        curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers);
        curl_easy_setopt(curl,CURLOPT_FAILONERROR,1L);curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,30L);curl_easy_setopt(curl,CURLOPT_LOW_SPEED_LIMIT,128L);curl_easy_setopt(curl,CURLOPT_LOW_SPEED_TIME,60L);
        curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,256L*1024);
        curl_easy_setopt(curl,CURLOPT_USERAGENT,"MagicTupper/0.2-direct");
        curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,header);curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,write);
        curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,this);
    }
    ~Remote(){if(curl)curl_easy_cleanup(curl);curl_slist_free_all(headers);}
    bool read(uint64_t offset,void* dest,size_t length){
        if(!curl){error="No se pudo iniciar HTTP.";return false;}
        if(offset>size||length>size-offset){error="Lectura HTTP fuera del paquete.";return false;}
        auto* output=static_cast<uint8_t*>(dest);
        for(size_t done=0;done<length;){
            const size_t count=std::min(blockSize,length-done);bool ok=false;
            for(int attempt=0;attempt<4;attempt++){
                if(hooks.cancelled()){error="Instalación cancelada.";return false;}
                active=Block{output+done,count,0,offset+done,offset+done+count-1,size,false,false,0};
                Block& block=active;
                const std::string range=std::to_string(block.first)+"-"+std::to_string(block.last);
                detail[0]=0;
                curl_easy_setopt(curl,CURLOPT_RANGE,range.c_str());curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,detail);
                curl_easy_setopt(curl,CURLOPT_HEADERDATA,&block);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&block);
                const CURLcode rc=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
                if(hooks.cancelled()){error="Instalación cancelada.";return false;}
                if(rc==CURLE_OK&&block.valid&&block.received==count){ok=true;break;}
                error="HTTP de instalación: curl "+std::to_string(rc)+", HTTP "+std::to_string(status)+".";
                if(status==200)error="El servidor no respeta Range: no se puede instalar directamente.";
                logger(error+" offset="+std::to_string(block.first)+" received="+std::to_string(block.received)+" attempt="+std::to_string(attempt+1)+" detail="+detail);
                const bool retry=rc==CURLE_PARTIAL_FILE||rc==CURLE_RECV_ERROR||rc==CURLE_OPERATION_TIMEDOUT||rc==CURLE_COULDNT_CONNECT||rc==CURLE_GOT_NOTHING;
                if(!retry||status>=400)break;
                for(int pause=0;pause<10*(attempt+1)&&!hooks.cancelled();pause++)wait();
            }
            if(!ok)return false;
            done+=count;
        }
        return true;
    }
};


}
