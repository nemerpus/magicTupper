#include "../source/sources.hpp"
#include "../source/http_security.hpp"
#include <cassert>
#include <string>

static size_t discard(char*,size_t a,size_t b,void*){return a*b;}
int main(int argc,char** argv){
    assert(argc==3);
    for(std::string url:{"https://portal.example","https://portal.example:443/","http://192.168.1.2:8765"})assert(sourceUrl(url));
    for(std::string url:{"https://","https://user:secret@host","https://host/api","https://host?token=x","file:///etc/passwd"})assert(!sourceUrl(url));
    curl_global_init(CURL_GLOBAL_DEFAULT);
    auto* curl=curl_easy_init();assert(curl&&configureHttpSecurity(curl));
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,discard);curl_easy_setopt(curl,CURLOPT_TIMEOUT,5L);
    curl_easy_setopt(curl,CURLOPT_CAINFO,argv[2]);
    std::string url=std::string(argv[1])+"/health";curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    assert(curl_easy_perform(curl)==CURLE_OK);
    url=std::string(argv[1])+"/redirect";curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    assert(curl_easy_perform(curl)==CURLE_OK);long status=0,redirects=0;
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);curl_easy_getinfo(curl,CURLINFO_REDIRECT_COUNT,&redirects);assert(status==302&&redirects==0);
    url=std::string(argv[1])+"/health";url.replace(url.find("localhost"),9,"127.0.0.1");curl_easy_setopt(curl,CURLOPT_URL,url.c_str());
    assert(curl_easy_perform(curl)==CURLE_PEER_FAILED_VERIFICATION);
    url=std::string(argv[1])+"/health";curl_easy_setopt(curl,CURLOPT_URL,url.c_str());curl_easy_setopt(curl,CURLOPT_CAINFO,"/etc/ssl/certs/ca-certificates.crt");
    assert(curl_easy_perform(curl)==CURLE_PEER_FAILED_VERIFICATION);
    curl_easy_cleanup(curl);curl_global_cleanup();
    puts("PASS: HTTPS sources, trusted CA, rejected unknown CA, hostname validation, redirects not followed");
}
