#include "../source/apps.hpp"
#include <cassert>
#include <fstream>
#include <filesystem>
static void write(const std::string& p,const std::string& text){std::ofstream out(p);out<<text;}
int main(){
    char temp[]="/tmp/magictupper-app-test-XXXXXX";assert(mkdtemp(temp));const std::string root=temp,stage=root+"/stage";
    auto digest=[](const std::string& p){std::ifstream f(p);char c=0;f.get(c);return f?std::string(64,c):std::string();};
    std::vector<AppFile> files={{"demo/assets/a.png",std::string(64,'a'),std::string(64,'a'),1},{"demo/main.nro",std::string(64,'b'),std::string(64,'b'),1}};
    auto fetch=[](const AppFile& f,const std::string& dest){write(dest,f.path.find(".nro")!=std::string::npos?"b":"a");return true;};std::string error;
    int checks=0;assert(!installAppFiles(files,root,stage,fetch,digest,[&](){return ++checks==4;},error));assert(!std::filesystem::exists(root+"/demo/assets/a.png"));assert(!std::filesystem::exists(root+"/demo/main.nro"));
    assert(installAppFiles(files,root,stage,fetch,digest,[](){return false;},error));assert(digest(root+"/demo/assets/a.png")==std::string(64,'a'));assert(digest(root+"/demo/main.nro")==std::string(64,'b'));
    write(root+"/demo/main.nro","c");int calls=0;assert(!installAppFiles(files,root,stage,[&](const AppFile&,const std::string&){calls++;return false;},digest,[](){return false;},error));assert(calls==0&&digest(root+"/demo/main.nro")==std::string(64,'c'));
    for(const auto* path:{"../bad","other/../bad","/absolute","app/.secret","app/bad?","app/trailing.","app/end "})assert(!validAppPath(path));
    auto* plan=json_object_new_object();json_object_object_add(plan,"version",json_object_new_int(1));json_object_object_add(plan,"executable",json_object_new_string("demo/main.nro"));auto* list=json_object_new_array();json_object_object_add(plan,"files",list);
    for(const auto& f:files){auto* row=json_object_new_object();json_object_object_add(row,"path",json_object_new_string(f.path.c_str()));json_object_object_add(row,"id",json_object_new_string(f.id.c_str()));json_object_object_add(row,"sha256",json_object_new_string(f.sha256.c_str()));json_object_object_add(row,"size",json_object_new_int64(f.size));json_object_array_add(list,row);}
    std::vector<AppFile> parsed;std::string executable;uint64_t total=0;assert(appPlan(plan,parsed,executable,total)&&total==2);
    json_object_object_add(json_object_array_get_idx(list,0),"path",json_object_new_string("other/assets.png"));assert(!appPlan(plan,parsed,executable,total));json_object_put(plan);
    std::filesystem::remove_all(root);puts("PASS: App manifests, bounded destinations, staged publication, cancellation rollback and preservation of existing files");
}
