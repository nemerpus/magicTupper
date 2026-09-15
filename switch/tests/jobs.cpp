#include "../source/jobs.hpp"
#include "../source/install.hpp"
#include <cassert>
#include <fstream>
#include <iterator>

int main(int argc,char** argv){
    assert(argc==2);const std::string path=argv[1];
    Task active;active.kind=3;active.destination=4;active.sourceId="1";
    active.item={std::string(64,'a'),"Instalación", "game.nsp","",3992977408LL};
    active.token="secret-token";active.password="secret-password";active.url="http://private";
    Task next=active;next.destination=5;
    std::deque<Task> queue;
    assert(!enqueueJob(queue,active,&active));assert(enqueueJob(queue,next,&active));assert(!enqueueJob(queue,next,&active));
    assert(saveJobs(queue,&active,true,path));
    std::ifstream file(path);std::string body((std::istreambuf_iterator<char>(file)),{});file.close();
    assert(body.find("secret")==std::string::npos&&body.find("http:")==std::string::npos);
    std::deque<Task> loaded;bool paused=false;assert(loadJobs(loaded,paused,path));
    assert(paused&&loaded.size()==2&&loaded[0].destination==4&&loaded[1].destination==5);
    assert(loaded[0].token.empty()&&loaded[0].password.empty()&&loaded[0].item.size==3992977408LL);
    auto* root=json_object_from_file(path.c_str());json_object *jobs=nullptr;
    assert(json_object_object_get_ex(root,"jobs",&jobs));
    json_object_object_add(json_object_array_get_idx(jobs,0),"destination",json_object_new_int(3));
    assert(json_object_to_file(path.c_str(),root)==0);json_object_put(root);
    assert(!loadJobs(loaded,paused,path));assert(loaded.size()==2);
    assert(saveJobs(queue,nullptr,false,path+".bak"));assert(remove(path.c_str())==0);
    assert(loadJobs(loaded,paused,path)&&loaded.size()==1&&!paused);
    queue.clear();for(int i=0;i<50;i++){next.sourceId=std::to_string(i+1);assert(enqueueJob(queue,next,nullptr));}
    next.sourceId="51";assert(!enqueueJob(queue,next,nullptr));
    Sources sources;sources.active="1";sources.entries.push_back({"1","Repo","http://localhost:8765","admin","","admin"});
    assert(saveSources(sources,path));
    root=json_object_from_file(path.c_str());assert(json_object_object_get_ex(root,"sources",&jobs));
    json_object_object_add(json_object_array_get_idx(jobs,0),"role",json_object_new_string("admin"));
    assert(json_object_to_file(path.c_str(),root)==0);json_object_put(root);
    Sources restored;assert(loadSources(restored,path));assert(restored.entries[0].role.empty());
    assert(mtinstall::validDestination(4)&&mtinstall::validDestination(5));
    assert(!mtinstall::validDestination(3)&&!mtinstall::validDestination(0));
    Task usb;usb.kind=10;usb.localPath="ums0:/game.nsp";usb.usbDevice="device";usb.item=active.item;
    assert(localTask(usb)&&transferTask(usb));queue.clear();assert(enqueueJob(queue,usb,nullptr));
    assert(!enqueueJob(queue,usb,nullptr));usb.kind=4;assert(enqueueJob(queue,usb,nullptr));
    assert(localTask(usb)&&transferTask(usb));assert(saveJobs(queue,nullptr,false,path));
    assert(loadJobs(loaded,paused,path)&&loaded.empty());
    Task browser=active;browser.item.category="browser";browser.item.relativePath="roms/Other/game.nsp";queue={browser};
    assert(saveJobs(queue,nullptr,false,path));assert(loadJobs(loaded,paused,path)&&loaded.size()==1);
    assert(loaded[0].item.category=="browser"&&loaded[0].item.relativePath==browser.item.relativePath);
    Task app=active;app.kind=12;app.destination=5;queue={app};assert(saveJobs(queue,nullptr,false,path));assert(loadJobs(loaded,paused,path)&&loaded[0].kind==12);
    remove(path.c_str());remove((path+".bak").c_str());
    puts("PASS: queue persistence, destinations, recovery, limits and untrusted roles");
}
