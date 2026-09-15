#include "install.hpp"
#include "http_range.hpp"
#include "local_file.hpp"
#include "package.hpp"
#include "cnmt.hpp"
#include <switch.h>
#include <curl/curl.h>
#include <array>
#include <cctype>
#include <cstdio>
#include <map>
#include <limits>

namespace mtinstall {
namespace {

void log(const std::string& value){FILE* f=fopen("sdmc:/switch/magictupper/install.log","a");if(f){fprintf(f,"%s\n",value.c_str());fclose(f);}}
std::string resultText(const char* stage,Result rc){char b[160];snprintf(b,sizeof(b),"%s (Switch 0x%08X)",stage,rc);return b;}
std::string idText(const NcmContentId& id){char b[33];for(size_t i=0;i<16;i++)snprintf(b+2*i,3,"%02x",id.c[i]);return b;}

struct Content {Entry entry;NcmContentId id{};bool existed=false;std::array<uint8_t,32> hash{};bool hasHash=false;};
struct Meta {NcmContentMetaKey key{};std::vector<uint8_t> data;uint64_t application=0;};
struct Record {NcmContentMetaKey key;uint64_t storage;};
static_assert(sizeof(Record)==24,"NS record ABI");

class Installer {
    const Hooks& hooks;std::string& error;ReadAt read;NcmStorageId destination;
    NcmContentStorage storage{};NcmContentMetaDatabase database{};Service manager{},es{};
    bool ncmReady=false,nsReady=false,splReady=false,storageReady=false,databaseReady=false,managerReady=false,esReady=false;
    bool placeholderReady=false,publishing=false;
    NcmPlaceHolderId placeholder{};
    std::array<uint8_t,32> headerKey{};
    std::vector<Content> contents;std::vector<NcmContentId> created;std::vector<Meta> metas;
    uint64_t total=0,done=0;
    const char* destinationName() const{return destination==NcmStorageId_SdCard?"SD":"memoria interna";}
    bool check(Result rc,const char* stage){if(R_SUCCEEDED(rc))return true;error=resultText(stage,rc);log(error);return false;}
    bool fail(const std::string& value){error=value;log(value);return false;}
    void report(const std::string& stage){hooks.progress(stage,done,total);}
    bool keys(){
        // SPL derives the header key on the console. No key files are written.
        const uint8_t kekSource[16]={0x1f,0x12,0x91,0x3a,0x4a,0xcb,0xf0,0x0d,0x4c,0xde,0x3a,0xf6,0xd5,0x23,0x88,0x2a};
        const uint8_t keySource[32]={0x5a,0x3e,0xd8,0x4f,0xde,0xc0,0xd8,0x26,0x31,0xf7,0xe2,0x5d,0x19,0x7b,0xf5,0xd0,0x1c,0x9b,0x7b,0xfa,0xf6,0x28,0x18,0x3d,0x71,0xf6,0x4d,0x73,0xf1,0x50,0xb9,0xd2};
        uint8_t kek[16]{};
        const bool ok=check(splCryptoGenerateAesKek(kekSource,0,0,kek),"Derivar KEK de cabecera")&&
            check(splCryptoGenerateAesKey(kek,keySource,headerKey.data()),"Derivar cabecera (1)")&&
            check(splCryptoGenerateAesKey(kek,keySource+16,headerKey.data()+16),"Derivar cabecera (2)");
        memset(kek,0,sizeof(kek));return ok;
    }
    bool prepareHeader(Content& content,std::vector<uint8_t>& data){
        if(data.size()<0xc00)return fail("NCA demasiado pequeño.");
        std::array<uint8_t,0xc00> plain{};Aes128XtsContext crypto;
        aes128XtsContextCreate(&crypto,headerKey.data(),headerKey.data()+16,false);
        for(size_t i=0;i<plain.size();i+=0x200){aes128XtsContextResetSector(&crypto,i/0x200,true);aes128XtsDecrypt(&crypto,plain.data()+i,data.data()+i,0x200);}
        if(memcmp(plain.data()+0x200,"NCA3",4)&&memcmp(plain.data()+0x200,"NCA2",4))return fail("Cabecera NCA no reconocida.");
        if(le(plain.data()+0x208,8)!=content.entry.size)return fail("El tamaño NCA no coincide con el paquete.");
        if(plain[0x204]>1)return fail("Distribución NCA inválida.");
        if(plain[0x204]==1){
            plain[0x204]=0;
            aes128XtsContextCreate(&crypto,headerKey.data(),headerKey.data()+16,true);
            for(size_t i=0;i<plain.size();i+=0x200){aes128XtsContextResetSector(&crypto,i/0x200,true);aes128XtsEncrypt(&crypto,data.data()+i,plain.data()+i,0x200);}
        }
        return true;
    }
    bool transfer(Content& content){
        if(hooks.cancelled())return fail("Instalación cancelada.");
        if(content.existed){done+=content.entry.size;report("Contenido ya instalado.");return true;}
        if(!check(ncmContentStorageGeneratePlaceHolderId(&storage,&placeholder),"Crear identificador temporal"))return false;
        if(!check(ncmContentStorageCreatePlaceHolder(&storage,&content.id,&placeholder,content.entry.size),"Reservar contenido"))return false;
        placeholderReady=true;
        Sha256Context sha;sha256ContextCreate(&sha);std::vector<uint8_t> buffer;
        for(uint64_t offset=0;offset<content.entry.size;){
            if(hooks.cancelled())return fail("Instalación cancelada.");
            const size_t count=std::min<uint64_t>(blockSize,content.entry.size-offset);buffer.resize(count);
            if(!read(content.entry.offset+offset,buffer.data(),count))return false;
            sha256ContextUpdate(&sha,buffer.data(),count);
            if(offset==0&&!prepareHeader(content,buffer))return false;
            if(!check(ncmContentStorageWritePlaceHolder(&storage,&placeholder,offset,buffer.data(),count),"Escribir contenido"))return false;
            offset+=count;done+=count;report(std::string(destination==NcmStorageId_SdCard?"Instalando en SD: ":"Instalando en interna: ")+content.entry.name);
        }
        uint8_t digest[32];sha256ContextGetHash(&sha,digest);
        if(memcmp(digest,content.id.c,16)||(content.hasHash&&memcmp(digest,content.hash.data(),32)))return fail("SHA-256 incorrecto: contenido descartado.");
        if(!check(ncmContentStorageFlushPlaceHolder(&storage),"Confirmar escritura")||!check(ncmContentStorageRegister(&storage,&content.id,&placeholder),"Registrar contenido"))return false;
        placeholderReady=false;created.push_back(content.id);content.existed=true;return true;
    }
    bool readMeta(Content& content){
        char path[FS_MAX_PATH]{};
        if(!check(ncmContentStorageGetPath(&storage,path,sizeof(path),&content.id),"Localizar metadatos"))return false;
        FsFileSystem fs{};
        if(!check(fsOpenFileSystemWithId(&fs,0,FsFileSystemType_ContentMeta,path,FsContentAttributes_None),"Abrir metadatos NCA"))return false;
        FsDir dir{};Result rc=fsFsOpenDirectory(&fs,"/",FsDirOpenMode_ReadFiles,&dir);
        if(!check(rc,"Abrir directorio CNMT")){fsFsClose(&fs);return false;}
        std::vector<uint8_t> bytes;bool ok=true;int found=0;
        for(;;){
            FsDirectoryEntry entry{};s64 count=0;
            if(!check(fsDirRead(&dir,&count,1,&entry),"Leer directorio CNMT")){ok=false;break;}
            if(!count)break;
            if(!ends(entry.name,".cnmt"))continue;
            if(++found!=1||entry.file_size<32||entry.file_size>16*1024*1024){ok=fail("CNMT ambiguo o demasiado grande.");break;}
            FsFile file{};const std::string name="/"+std::string(entry.name);
            if(!check(fsFsOpenFile(&fs,name.c_str(),FsOpenMode_Read,&file),"Abrir CNMT")){ok=false;break;}
            bytes.resize(entry.file_size);u64 received=0;
            rc=fsFileRead(&file,0,bytes.data(),bytes.size(),FsReadOption_None,&received);fsFileClose(&file);
            if(!check(rc,"Leer CNMT")||received!=bytes.size()){ok=false;error="Lectura CNMT incompleta.";break;}
        }
        fsDirClose(&dir);fsFsClose(&fs);
        if(!ok)return false;
        if(found!=1)return fail("No se encontró el CNMT.");
        NcmContentInfo own{};own.content_id=content.id;ncmU64ToContentInfoSize(content.entry.size,&own);own.content_type=NcmContentType_Meta;
        static_assert(sizeof(own)==24,"NCM content info ABI");
        std::array<uint8_t,24> ownBytes{};memcpy(ownBytes.data(),&own,sizeof(own));
        ParsedMeta parsed;if(!cnmt(bytes,ownBytes,parsed,error))return false;
        Meta meta;meta.key.id=parsed.id;meta.key.version=parsed.version;meta.key.type=parsed.type;meta.application=parsed.application;meta.data=std::move(parsed.database);
        for(const auto& info:parsed.contents){
            auto foundContent=std::find_if(contents.begin(),contents.end(),[&](const Content& c){return !memcmp(c.id.c,info.info.data(),16);});
            if(foundContent==contents.end()||foundContent->entry.size!=info.size)return fail("Falta un contenido del CNMT o su tamaño no coincide.");
            if(foundContent->hasHash&&foundContent->hash!=info.hash)return fail("CNMT con hashes contradictorios.");
            foundContent->hash=info.hash;foundContent->hasHash=true;
        }
        for(const auto& old:metas)if(!memcmp(&old.key,&meta.key,sizeof(meta.key)))return fail("Metadatos duplicados.");
        metas.push_back(std::move(meta));return true;
    }
    bool tickets(const std::vector<Entry>& entries){
        for(const auto& ticket:entries)if(ends(ticket.name,".tik")){
            const std::string certName=ticket.name.substr(0,ticket.name.size()-4)+".cert";
            const auto cert=std::find_if(entries.begin(),entries.end(),[&](const Entry& e){return e.name==certName;});
            if(cert==entries.end()||!ticket.size||ticket.size>1024*1024||!cert->size||cert->size>1024*1024)return fail("Ticket o certificado ausente o inválido.");
            if(!esReady){if(!check(smGetService(&es,"es"),"Abrir servicio de tickets"))return false;esReady=true;}
            std::vector<uint8_t> tik(ticket.size),crt(cert->size);
            if(!read(ticket.offset,tik.data(),tik.size())||!read(cert->offset,crt.data(),crt.size()))return false;
            const Result rc=serviceDispatch(&es,1,
                .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_In,SfBufferAttr_HipcMapAlias|SfBufferAttr_In},
                .buffers={{tik.data(),tik.size()},{crt.data(),crt.size()}});
            memset(tik.data(),0,tik.size());if(!check(rc,"Importar ticket"))return false;
        }
        return true;
    }
    bool publish(){
        report("Registrando títulos en HOME...");
        // Once metadata publication starts, preserve verified content on error.
        // A subsequent install reuses it and repeats the registration phase.
        publishing=true;
        for(const auto& meta:metas)if(!check(ncmContentMetaDatabaseSet(&database,&meta.key,meta.data.data(),meta.data.size()),"Guardar metadatos"))return false;
        if(!check(ncmContentMetaDatabaseCommit(&database),"Confirmar metadatos"))return false;
        std::set<uint64_t> applications;for(const auto& meta:metas)applications.insert(meta.application);
        for(uint64_t app:applications){
            std::vector<Record> records;
            // Include already installed versions, updates and DLC in both stores.
            for(NcmStorageId target:{NcmStorageId_SdCard,NcmStorageId_BuiltInUser}){
                NcmContentMetaDatabase db{};if(!check(ncmOpenContentMetaDatabase(&db,target),"Leer títulos instalados"))return false;
                std::vector<NcmContentMetaKey> keys(4096);s32 totalKeys=0,written=0;
                const Result rc=ncmContentMetaDatabaseList(&db,&totalKeys,&written,keys.data(),keys.size(),NcmContentMetaType_Unknown,0,app,app+0x1fff,NcmContentInstallType_Full);
                ncmContentMetaDatabaseClose(&db);
                if(!check(rc,"Enumerar títulos instalados"))return false;
                if(totalKeys>written)return fail("Demasiados metadatos para registrar esta aplicación.");
                for(int i=0;i<written;i++){
                    const auto& key=keys[i];uint64_t base=key.type==0x81?(key.id^0x800):key.type==0x82?((key.id^0x1000)&~uint64_t(0xfff)):key.id;
                    if(base==app&&(key.type==0x80||key.type==0x81||key.type==0x82))records.push_back({key,uint64_t(target)});
                }
            }
            if(records.empty())return fail("No hay contenidos para registrar en HOME.");
            struct {u8 event;u8 padding[7];u64 application;} input{};input.event=3;input.application=app;
            if(!check(serviceDispatchIn(&manager,16,input,
                .buffer_attrs={SfBufferAttr_HipcMapAlias|SfBufferAttr_In},
                .buffers={{records.data(),records.size()*sizeof(Record)}}),"Registrar aplicación en HOME"))return false;
        }
        return true;
    }
public:
    Installer(ReadAt reader,NcmStorageId target,const Hooks& cb,std::string& out):hooks(cb),error(out),read(std::move(reader)),destination(target){}
    ~Installer(){
        if(placeholderReady){const Result rc=ncmContentStorageDeletePlaceHolder(&storage,&placeholder);if(R_FAILED(rc))log(resultText("Limpiar temporal pendiente",rc));}
        if(!publishing)for(auto it=created.rbegin();it!=created.rend();++it){const Result rc=ncmContentStorageDelete(&storage,&*it);if(R_FAILED(rc))log(resultText("Limpiar contenido nuevo",rc));}
        if(esReady)serviceClose(&es);
        if(managerReady)serviceClose(&manager);
        if(databaseReady)ncmContentMetaDatabaseClose(&database);
        if(storageReady)ncmContentStorageClose(&storage);
        if(splReady)splCryptoExit();
        if(nsReady)nsExit();
        if(ncmReady)ncmExit();
        memset(headerKey.data(),0,headerKey.size());
    }
    bool run(const std::string& filename,uint64_t size){
        if(appletGetAppletType()!=AppletType_Application&&appletGetAppletType()!=AppletType_SystemApplication)return fail("Abre MagicTupper en modo aplicación para instalar.");
        if(hosversionBefore(3,0,0))return fail("El instalador necesita firmware 3.0.0 o posterior.");
        report("Leyendo índice remoto...");
        const auto dot=filename.find_last_of('.');std::string ext=dot==std::string::npos?"":filename.substr(dot);
        for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::vector<Entry> entries;
        if(!package([&](uint64_t p,void* b,size_t n){return read(p,b,n);},size,ext,entries,error))return false;
        std::set<std::string> ids;
        for(const auto& entry:entries){
            if(ends(entry.name,".ncz"))return fail("Contenido comprimido no compatible con instalación directa.");
            if(!ends(entry.name,".nca"))continue;
            Content content;content.entry=entry;
            if(!contentId(entry.name,content.id.c)||entry.size<0xc00||entry.size>uint64_t(INT64_MAX)||!ids.insert(idText(content.id)).second)return fail("Entrada NCA inválida o duplicada.");
            if(total>uint64_t(INT64_MAX)-entry.size)return fail("Paquete demasiado grande.");
            total+=entry.size;contents.push_back(content);
        }
        if(contents.empty()||std::none_of(contents.begin(),contents.end(),[](const Content& c){return ends(c.entry.name,".cnmt.nca");}))return fail("El paquete no contiene metadatos instalables.");
        if(!check(ncmInitialize(),"Abrir NCM"))return false;
        ncmReady=true;
        if(!check(ncmOpenContentStorage(&storage,destination),"Abrir almacenamiento elegido"))return false;
        storageReady=true;
        if(!check(ncmOpenContentMetaDatabase(&database,destination),"Abrir base de metadatos"))return false;
        databaseReady=true;
        if(!check(nsInitialize(),"Abrir NS"))return false;
        nsReady=true;
        if(!check(nsGetApplicationManagerInterface(&manager),"Abrir gestor de aplicaciones"))return false;
        managerReady=true;
        if(!check(splCryptoInitialize(),"Abrir criptografía del sistema"))return false;
        splReady=true;if(!keys())return false;
        uint64_t required=16*1024*1024;
        for(auto& content:contents){
            if(!check(ncmContentStorageHas(&storage,&content.existed,&content.id),"Consultar contenido existente"))return false;
            if(content.existed){s64 length=0;if(!check(ncmContentStorageGetSizeFromContentId(&storage,&length,&content.id),"Consultar tamaño instalado"))return false;if(length<0||uint64_t(length)!=content.entry.size)return fail("Contenido existente con tamaño distinto. No se sobrescribe.");}
            else if(ends(content.entry.name,".cnmt.nca"))required+=content.entry.size;
        }
        s64 free=0;if(!check(ncmContentStorageGetFreeSpaceSize(&storage,&free),"Consultar espacio libre"))return false;
        if(free<0||required>uint64_t(free))return fail("No cabe la instalación en "+std::string(destinationName())+": necesita "+std::to_string(required/1048576)+" MiB libres.");
        report("Comprobando tickets...");if(!tickets(entries))return false;
        for(auto& content:contents)if(ends(content.entry.name,".cnmt.nca")){if(!transfer(content)||!readMeta(content))return false;}
        // CNMT gives the exact set needed by a full installation; exclude
        // unreferenced archives and delta fragments from both space and progress.
        total=0;required=16*1024*1024;
        for(const auto& content:contents)if(ends(content.entry.name,".cnmt.nca")||content.hasHash){
            total+=content.entry.size;if(!content.existed)required+=content.entry.size;
        }
        if(!check(ncmContentStorageGetFreeSpaceSize(&storage,&free),"Consultar espacio para contenidos"))return false;
        if(free<0||required>uint64_t(free))return fail("No cabe la instalación: necesita "+std::to_string(required/1048576)+" MiB libres en "+destinationName()+".");
        for(auto& content:contents)if(!ends(content.entry.name,".cnmt.nca")&&content.hasHash){if(!transfer(content))return false;}
        if(hooks.cancelled())return fail("Instalación cancelada.");
        if(!publish())return false;
        created.clear();done=total;report("Instalación terminada en "+std::string(destinationName())+". Juego registrado en HOME.");return true;
    }
};
}
bool install(const std::string& url,const std::string& token,const std::string& id,const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error){
    if(!validDestination(destination)){error="Destino de instalación no permitido.";return false;}
    log("start version=0.3.6 id="+id+" size="+std::to_string(size)+" destination="+std::to_string(destination));
    struct Playback {Playback(){appletSetMediaPlaybackState(true);}~Playback(){appletSetMediaPlaybackState(false);}} playback;
    Remote remote(url+"/api/files/"+id,token,size,hooks,error,log,[](){svcSleepThread(100000000);});
    Installer installer([&](uint64_t p,void* b,size_t n){return remote.read(p,b,n);},static_cast<NcmStorageId>(destination),hooks,error);
    const bool ok=installer.run(filename,size);log(ok?"complete id="+id:"failed id="+id+" "+error);return ok;
}
bool installReader(const ReadAt& read,const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error){
    if(!validDestination(destination)){error="Destino de instalación no permitido.";return false;}
    log("start version=0.3.7 source=PC_DIRECT size="+std::to_string(size)+" destination="+std::to_string(destination));
    struct Playback {Playback(){appletSetMediaPlaybackState(true);}~Playback(){appletSetMediaPlaybackState(false);}} playback;
    Installer installer(read,static_cast<NcmStorageId>(destination),hooks,error);
    const bool ok=installer.run(filename,size);log(ok?"complete source=PC_DIRECT":"failed source=PC_DIRECT "+error);return ok;
}
bool installLocal(const std::string& path,const std::string& filename,uint64_t size,int destination,const Hooks& hooks,std::string& error){
    if(!validDestination(destination)){error="Destino de instalación no permitido.";return false;}
    log("start version=0.3.6 source=LOCAL size="+std::to_string(size)+" destination="+std::to_string(destination));
    LocalFile file(path,size,hooks,error);if(!file.ready()){log("failed source=LOCAL stage=open "+error);return false;}
    struct Playback {Playback(){appletSetMediaPlaybackState(true);}~Playback(){appletSetMediaPlaybackState(false);}} playback;
    Installer installer([&](uint64_t p,void* b,size_t n){return file.read(p,b,n);},static_cast<NcmStorageId>(destination),hooks,error);
    const bool ok=installer.run(filename,size);log(ok?"complete source=LOCAL":"failed source=LOCAL "+error);return ok;
}
}
