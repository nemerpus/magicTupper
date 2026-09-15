#include "../source/explorer.hpp"
#include <cassert>
using Uint32=uint32_t;
static Uint32 SDL_GetTicks(){return 100;}
#include "../source/pc_explorer.hpp"
#include "../source/file_safety.hpp"
int main(){
    assert(safeSdPath("sdmc:/")&&safeSdPath("sdmc:/switch/example.nro"));
    for(const auto* name:{"../bad","a/b","a\\b","a:b",".","..","bad.","bad "})assert(!safeTransferName(name));
    assert(!safeTransferName(std::string("a\0b",3)));
    assert(!safeSdPath("sdmc:/../boot.dat")&&!safeSdPath("sdmc://boot.dat"));
    char directory[]="/tmp/magictupper-safety-XXXXXX";assert(mkdtemp(directory));
    const std::string target=std::string(directory)+"/destination";
    std::string stage;FILE* staged=createTransferStage(target,stage);assert(staged);
    assert(fputs("new",staged)>=0&&fclose(staged)==0);
    FILE* old=fopen(target.c_str(),"wb");assert(old);assert(fputs("old",old)>=0&&fclose(old)==0);
    assert(!publishNewTransfer(stage,target));
    char content[4]={};old=fopen(target.c_str(),"rb");assert(old&&fread(content,1,3,old)==3);fclose(old);assert(std::string(content)=="old");
    assert(std::remove(target.c_str())==0&&publishNewTransfer(stage,target));
    assert(std::remove(target.c_str())==0&&rmdir(directory)==0);
    assert(browserParent("pc:/")=="explorer:/");
    assert(browserParent("pc:/Users")=="pc:/");
    assert(browserHeading("pc:/Users")=="PC / C:/Users");
    assert(pcInstallable("GAME.NSP")&&pcInstallable("game.xci")&&!pcInstallable("notes.txt"));
    pcExplorerAsk("pc:/Users","list",128);
    auto* request=json_tokener_parse(pcExplorerRequest.c_str());
    assert(pcExplorerField(request,"path")=="pc:/Users");
    json_object* offset=nullptr;assert(json_object_object_get_ex(request,"offset",&offset)&&json_object_get_int(offset)==128);
    json_object_put(request);
    auto previous=pcExplorerId;pcExplorerResponse="stale";
    pcExplorerAsk("pc:/Users/file.txt","send");
    assert(pcExplorerId==previous+1&&pcExplorerResponse.empty());
    for(const char* invalid:{"null","[]","{\"id\":1,\"ok\":null}","{\"id\":1,\"ok\":true,\"data\":{\"entries\":false}}"}){
        auto* obj=json_tokener_parse(invalid);assert(!pcExplorerValidResponse(obj,1));if(obj)json_object_put(obj);
    }
    auto* valid=json_tokener_parse("{\"id\":1,\"ok\":true}");assert(pcExplorerValidResponse(valid,1)&&!pcExplorerValidResponse(valid,2));json_object_put(valid);
    std::vector<Item> items={{"a","A","a.xci","games",10,"Juegos/Serie/a.xci"},{"b","B","b.nsp","games",20,"Juegos/b.nsp"},{"c","C","c.nro","apps",1,"Apps/c.nro"},{"d","D","d.nsp","games",1,"../private/d.nsp"}};
    auto root=serverEntries(items,"server:/");assert(root.size()==2&&root[0].name=="Apps"&&root[1].name=="Juegos");
    auto games=serverEntries(items,"server:/Juegos");assert(games.size()==2&&games[0].directory&&games[1].itemId=="b");
    assert(serverEntries(items,"server:/Juego").empty());
    assert(serverEntries(items,"server:/Juegos/Serie")[0].itemId=="a");
    items[0].relativePath.clear();assert(serverEntries(items,"server:/").empty());
    assert(browserHeading("server:/Juegos/Serie")=="Servidor / Biblioteca / Juegos / Serie");
    assert(browserHeading("sdmc:/")=="Tarjeta SD");
    for(const auto* path:{"../x","/absolute","a//b","a/../b","a/./b","C:/x","a\\b","a/"})assert(!validRelativePath(path));
    assert(validRelativePath("Juegos/Árbol.xci"));
    assert(browserParent("ums12:/folder")=="ums12:/");assert(browserParent("ums12:/")=="usb:/");
    assert(browserParent("sdmc:/")=="explorer:/");assert(browserParent("server:/Juegos")=="server:/");
    assert(browserParent("server:/")=="explorer:/");assert(browserParent("usb:/")=="explorer:/");
    puts("PASS: explorer hierarchy, old catalogs, relative paths and parent navigation");
}
