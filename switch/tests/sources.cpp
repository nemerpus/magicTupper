#include "../source/sources.hpp"
#include <cassert>
#include <fstream>
#include <iterator>
#include <unistd.h>

int main(){
    char directory[]="/tmp/magictupper-sources-XXXXXX";
    assert(mkdtemp(directory));
    const std::string file=std::string(directory)+"/sources.json";
    Sources sources;
    sources.entries.push_back({sources.allocateId(),"Casa","http://192.168.1.10:8765","alice","secret-session-one"});
    sources.entries.push_back({sources.allocateId(),"NAS","http://192.168.1.20:8765","bob","secret-session-two"});
    sources.active="2";
    assert(saveSources(sources,file));
    std::ifstream input(file);
    const std::string raw((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    assert(raw.find("secret-session")==std::string::npos);
    Sources restored;
    assert(loadSources(restored,file));
    assert(restored.entries.size()==2&&restored.active=="2"&&restored.nextId==3);
    assert(restored.find("1")->user=="alice"&&restored.find("2")->user=="bob");
    assert(restored.find("1")->token.empty()&&restored.find("2")->token.empty());
    restored.entries.push_back({restored.allocateId(),"Prueba","http://192.168.1.30:8765","carol",""});
    assert(saveSources(restored,file));
    Sources added;assert(loadSources(added,file));assert(added.entries.size()==3&&added.find("3")->name=="Prueba");
    restored.entries.pop_back();restored.active="1";
    assert(saveSources(restored,file));
    Sources removed;assert(loadSources(removed,file));assert(removed.allocateId()=="3");
    restored.entries.push_back({"4","Duplicate","http://192.168.1.10:8765","other",""});
    assert(saveSources(restored,file));
    assert(!loadSources(removed,file));
    assert(removed.entries.size()==1);
    std::string valid="http://server.local:8765/";
    assert(sourceUrl(valid)&&valid=="http://server.local:8765");
    for(std::string invalid:{"file:///etc/passwd","http://user:pass@host","http://host/path","http://"})assert(!sourceUrl(invalid));
    assert(unlink(file.c_str())==0);assert(rmdir(directory)==0);
    puts("PASS: persistencia, sesiones no guardadas, IDs no reutilizados, duplicados y rutas de fuentes.");
}
