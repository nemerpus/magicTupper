#include "../source/updates.hpp"
#include <cassert>
int main(){
    InstalledTitles installed;installed.complete=true;installed.games.insert(0x0100123456780000ull);
    Item item;item.category="updates";item.titleId="0100123456780800";item.applicationId="0100123456780000";item.version=65536;
    assert(updateStatus(item,installed)==2);
    assert(updateMissing(item,installed));assert(updateVisible(item,installed,1));
    installed.patches[0x0100123456780000ull]=65536;assert(updateStatus(item,installed)==3);
    assert(!updateMissing(item,installed));
    assert(updateBadge(item,installed)=="INSTALADO");
    item.version=131072;assert(updateStatus(item,installed)==2);
    assert(updateMissing(item,installed));
    installed.complete=false;assert(updateStatus(item,installed)==1);
    assert(!updateMissing(item,installed));
    item.version=-1;assert(updateStatus(item,installed)==1);
    installed.complete=true;assert(!updateMissing(item,installed));installed.patches.clear();assert(updateMissing(item,installed));
    item.applicationId="0100123456790000";assert(updateStatus(item,installed)==0);
    item.titleId="0100123456790800";assert(updateBadge(item,installed)=="NO PROCEDE: FALTA EL JUEGO");
    item.applicationId="0100123456780000";item.titleId="not an id";assert(updateStatus(item,installed)==0);
    assert(titleNumber("ffffffffffffffff")==UINT64_MAX);assert(!titleNumber("010012345678000g"));
    Task a;a.kind=3;a.item.id=std::string(64,'a');Task b=a;b.item.id=std::string(64,'b');std::deque<Task> queue;
    assert(enqueueUpdateBatch(queue,{a,b},nullptr)&&queue.size()==2);assert(!enqueueUpdateBatch(queue,{b},nullptr)&&queue.size()==2);
    queue.clear();for(int i=0;i<49;i++){Task t=a;t.sourceId=std::to_string(i);queue.push_back(t);}assert(!enqueueUpdateBatch(queue,{a,b},nullptr)&&queue.size()==49);
    Item dlc;dlc.category="dlc";dlc.titleId="0100123456781001";dlc.applicationId="0100123456780000";dlc.version=-1;
    assert(updateBadge(dlc,installed)=="DLC DISPONIBLE");
    installed.addons[0x0100123456781001ull]=0;
    assert(updateBadge(dlc,installed)=="INSTALADO");assert(!updateMissing(dlc,installed));
    dlc.titleId="0100123456781002";assert(updateMissing(dlc,installed));
    installed.games.clear();assert(updateBadge(dlc,installed)=="NO PROCEDE: FALTA EL JUEGO");
    installed.complete=false;assert(updateBadge(dlc,installed)=="ESTADO SIN CONFIRMAR");
    puts("PASS: updates and DLC match installed titles, versions and missing base games");
}
