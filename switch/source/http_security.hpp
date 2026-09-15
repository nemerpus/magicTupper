#pragma once
#include <curl/curl.h>

inline bool configureHttpSecurity(CURL* curl){
    if(curl_easy_setopt(curl,CURLOPT_SSL_VERIFYPEER,1L)!=CURLE_OK||
       curl_easy_setopt(curl,CURLOPT_SSL_VERIFYHOST,2L)!=CURLE_OK||
       curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L)!=CURLE_OK)return false;
#ifdef __SWITCH__
    // The build-time host CA path is not available on the console.
    if(curl_easy_setopt(curl,CURLOPT_CAINFO,"sdmc:/switch/magictupper/assets/cacert.pem")!=CURLE_OK)return false;
#endif
    return true;
}
