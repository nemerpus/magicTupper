#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include "sources.hpp"
#include "assets.hpp"
#include "install.hpp"
#include "jobs.hpp"
#include "explorer.hpp"
#include "pc_explorer.hpp"
#include "file_safety.hpp"
#include "updates.hpp"
#include "apps.hpp"
#include "http_range.hpp"
#include "usb_protocol.hpp"
#include <map>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <optional>
#include <fcntl.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#ifdef MAGICTUPPER_USBHSFS
#include <usbhsfs.h>
#endif
#include <switch/runtime/devices/usb_comms.h>

struct AdminUser {std::string id,username,role;bool enabled=true;};

struct State {
    std::string name="MagicTupper",tagline="Laboratorio C-137 / Transferencias interdimensionales",joke="Morty, el multiverso es infinito. Tu SD, jodidamente no.",username;
    std::string url="http://192.168.1.100:8765",token,message="Pulsa Y para configurar tu portal e iniciar sesión.";
    std::vector<Item> items;
    std::vector<BrowserEntry> serverBrowser;
    std::string serverBrowserPath,serverBrowserError;
    std::vector<AdminUser> users;
    std::deque<std::string> history;
    std::string role,taskMessage,activeTitle,activeDestination;
    Sources sources;
    SDL_Color accent{181,255,57,255};
    double done=0,total=0,speed=0;
} state;
static SDL_mutex* mutex;
static SDL_mutex* sourceFileMutex;
static bool writeSources(const Sources& sources,const std::string& path){SDL_LockMutex(sourceFileMutex);const bool ok=saveSources(sources,path);SDL_UnlockMutex(sourceFileMutex);return ok;}
static std::atomic<bool> busy{false},cancelled{false},controlBusy{false},quitting{false};
static std::atomic<bool> explorerCopyRunning{false};
static SDL_Thread* explorerCopyThread=nullptr;
struct ExplorerCopyJob { std::string source,destination; };
static bool magicUsbDevice=false;
static SDL_Thread* magicUsbThread=nullptr;
static std::atomic<bool> magicUsbStop{false};
static std::atomic<bool> magicUsbWorkerEntered{false};
static bool magicUsbLocked=false;
static bool linkBenchActive=false;
static uint64_t linkBenchExpected=0,linkBenchReceived=0;
static Uint32 linkBenchStarted=0;

// MT2 high-throughput data plane.  The legacy MTUSB1 control plane stays enabled
// for compatibility, while bulk data uses sequence numbers, explicit offsets and
// cumulative/window ACKs (similar to modern stream flow-control protocols).
static constexpr size_t kMtp2HeaderSize=32;
static constexpr uint32_t kMtp2ChunkSize=1024*1024;
static constexpr uint32_t kMtp2WindowSize=128*1024*1024; // MT2.4: ventana lógica, no reserva 128 MiB
static bool mtp2BenchActive=false;
static uint32_t mtp2BenchStream=0,mtp2BenchExpectedSeq=0;
static uint64_t mtp2BenchExpected=0,mtp2BenchReceived=0;
static Uint32 mtp2BenchStarted=0;
static uint32_t mtp2TransferExpectedSeq=0;
static uint32_t pcDirectV2ExpectedSeq=0;
static uint32_t pcDirectV2Received=0;

static int magicTcpListen=-1;
static std::mutex magicTcpSocketMutex;
static int magicTcpClient=-1;
static std::atomic<bool> magicTcpConnected{false};
static std::atomic<int> magicTcpApproval{0}; // 0 idle, 1 pending, 2 allowed, 3 rejected.
static std::mutex magicTcpPeerMutex;
static std::string magicTcpPeer;
static SDL_Thread* magicTcpThread=nullptr;
static std::atomic<bool> magicTcpStop{false};
// El protocolo USB se declara antes de las utilidades de archivos; esta
// declaración permite crear el directorio temporal desde el manejador.
static bool makeDirectories(const std::string& directory);


// ===== REMOTE PLAY INPUT LAB (UDP/8767) =====
// Phase 1 only receives and acknowledges normalized controller state. The
// Horizon virtual-controller injection layer is intentionally kept separate so
// transport/latency can be tested before a background sysmodule is introduced.
static constexpr uint16_t kRemoteInputPort=8767;
static int remoteInputSocket=-1;
static SDL_Thread* remoteInputThread=nullptr;
static std::atomic<bool> remoteInputStop{false};
static std::atomic<uint32_t> remoteInputSequence{0};
static std::atomic<uint32_t> remoteInputButtons{0};
static std::atomic<int16_t> remoteInputLX{0},remoteInputLY{0},remoteInputRX{0},remoteInputRY{0};
static std::atomic<uint16_t> remoteInputLT{0},remoteInputRT{0};
static std::atomic<Uint32> remoteInputLastTick{0};
static std::atomic<uint64_t> remoteInputPackets{0};

static uint16_t rpRead16(const unsigned char* p){return static_cast<uint16_t>(p[0])|static_cast<uint16_t>(p[1])<<8;}
static uint32_t rpRead32(const unsigned char* p){return static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;}
static uint64_t rpRead64(const unsigned char* p){uint64_t v=0;for(int i=0;i<8;i++)v|=static_cast<uint64_t>(p[i])<<(i*8);return v;}
static void rpWrite16(unsigned char* p,uint16_t v){p[0]=v&0xff;p[1]=(v>>8)&0xff;}
static void rpWrite32(unsigned char* p,uint32_t v){for(int i=0;i<4;i++)p[i]=(v>>(i*8))&0xff;}
static void rpWrite64(unsigned char* p,uint64_t v){for(int i=0;i<8;i++)p[i]=(v>>(i*8))&0xff;}

static int remoteInputWorker(void*){
    sockaddr_in bindAddress{};bindAddress.sin_family=AF_INET;bindAddress.sin_port=htons(kRemoteInputPort);bindAddress.sin_addr.s_addr=INADDR_ANY;
    remoteInputSocket=socket(AF_INET,SOCK_DGRAM,0);if(remoteInputSocket<0)return 0;
    int yes=1;setsockopt(remoteInputSocket,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    timeval timeout{};timeout.tv_sec=0;timeout.tv_usec=200000;setsockopt(remoteInputSocket,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    if(bind(remoteInputSocket,reinterpret_cast<sockaddr*>(&bindAddress),sizeof(bindAddress))<0){close(remoteInputSocket);remoteInputSocket=-1;return 0;}
    unsigned char packet[128];
    while(!remoteInputStop){
        sockaddr_in peer{};socklen_t peerLength=sizeof(peer);const int n=recvfrom(remoteInputSocket,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&peer),&peerLength);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK)continue;if(remoteInputStop)break;continue;}
        if(n!=40||memcmp(packet,"MTRP",4)!=0||packet[4]!=1||packet[5]!=1||rpRead16(packet+6)!=40)continue;
        const uint32_t sequence=rpRead32(packet+8);const uint64_t clientTimestamp=rpRead64(packet+12);
        remoteInputSequence=sequence;remoteInputButtons=rpRead32(packet+20);
        remoteInputLX=static_cast<int16_t>(rpRead16(packet+24));remoteInputLY=static_cast<int16_t>(rpRead16(packet+26));
        remoteInputRX=static_cast<int16_t>(rpRead16(packet+28));remoteInputRY=static_cast<int16_t>(rpRead16(packet+30));
        remoteInputLT=rpRead16(packet+32);remoteInputRT=rpRead16(packet+34);remoteInputLastTick=SDL_GetTicks();remoteInputPackets.fetch_add(1);
        unsigned char ack[24]={'M','T','R','P',1,0x81,24,0};rpWrite32(ack+8,sequence);rpWrite64(ack+12,clientTimestamp);rpWrite32(ack+20,SDL_GetTicks());
        sendto(remoteInputSocket,ack,sizeof(ack),0,reinterpret_cast<sockaddr*>(&peer),peerLength);
    }
    if(remoteInputSocket>=0){close(remoteInputSocket);remoteInputSocket=-1;}return 0;
}

static bool remoteInputActive(){const Uint32 last=remoteInputLastTick.load();return last!=0&&SDL_GetTicks()-last<1000;}


// MT2.5 network pull benchmark.  Inspired by the Awoo/Tinfoil network model:
// the PC exposes an HTTP byte-range source and the Switch pulls the data.
// Multiple independent ranges let us test whether Horizon/libcurl benefits from
// more than one TCP flow without changing Installer/NCM yet.
struct NetHttpBenchJob {
    std::string url;
    uint64_t first=0,last=0,received=0;
    CURLcode rc=CURLE_OK;
    long http=0;
    char error[CURL_ERROR_SIZE]{};
};
static size_t netHttpBenchSink(char* ptr,size_t size,size_t nmemb,void* userdata){
    (void)ptr;
    auto* job=static_cast<NetHttpBenchJob*>(userdata);
    const size_t n=size*nmemb;
    job->received+=n;
    return n;
}
static int netHttpBenchWorker(void* arg){
    auto* job=static_cast<NetHttpBenchJob*>(arg);
    CURL* curl=curl_easy_init();
    if(!curl){job->rc=CURLE_FAILED_INIT;return 0;}
    const std::string range=std::to_string(job->first)+"-"+std::to_string(job->last);
    curl_easy_setopt(curl,CURLOPT_URL,job->url.c_str());
    curl_easy_setopt(curl,CURLOPT_RANGE,range.c_str());
    curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,netHttpBenchSink);
    curl_easy_setopt(curl,CURLOPT_ERRORBUFFER,job->error);
    curl_easy_setopt(curl,CURLOPT_WRITEDATA,job);
    curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);
    curl_easy_setopt(curl,CURLOPT_TIMEOUT,120L);
    curl_easy_setopt(curl,CURLOPT_BUFFERSIZE,256L*1024);
    curl_easy_setopt(curl,CURLOPT_TCP_NODELAY,1L);
    curl_easy_setopt(curl,CURLOPT_HTTP_VERSION,CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl,CURLOPT_FRESH_CONNECT,1L);
    curl_easy_setopt(curl,CURLOPT_FORBID_REUSE,1L);
    curl_easy_setopt(curl,CURLOPT_USERAGENT,"MagicTupper/MT2.5-netpull");
    job->rc=curl_easy_perform(curl);
    curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&job->http);
    curl_easy_cleanup(curl);
    return 0;
}

// ===== DEBUG LOGGER =====
static const char* DEBUG_LOG_PATH = "sdmc:/switch/magictupper/debug.log";
static std::atomic<uint32_t> debugSeq{0};
static std::mutex debugLogMutex;

static void debug_log(const char* fmt, ...) {
#ifndef MAGICTUPPER_USB_DIAGNOSTICS
    (void)fmt;
    return;
#endif
    std::lock_guard<std::mutex> lock(debugLogMutex);
    FILE* f = fopen(DEBUG_LOG_PATH, "a");
    if (!f) return;
    uint32_t seq = debugSeq.fetch_add(1);
    Uint32 ticks = SDL_GetTicks();
    fprintf(f, "[%08u][%08u] ", seq, ticks);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

static void debug_hex(const char* tag, const void* data, size_t len, size_t max_dump = 128) {
#ifndef MAGICTUPPER_USB_DIAGNOSTICS
    (void)tag;(void)data;(void)len;(void)max_dump;
    return;
#endif
    std::lock_guard<std::mutex> lock(debugLogMutex);
    FILE* f = fopen(DEBUG_LOG_PATH, "a");
    if (!f) return;
    uint32_t seq = debugSeq.fetch_add(1);
    Uint32 ticks = SDL_GetTicks();
    fprintf(f, "[%08u][%08u] [HEX][%s] len=%zu\n", seq, ticks, tag, len);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t dump_len = len < max_dump ? len : max_dump;
    for (size_t i = 0; i < dump_len; i++) {
        if (i % 16 == 0) fprintf(f, "%04zx: ", i);
        fprintf(f, "%02x ", p[i]);
        if ((i + 1) % 16 == 0) fprintf(f, "\n");
    }
    if (dump_len % 16 != 0) fprintf(f, "\n");
    if (len > max_dump) fprintf(f, "... (%zu bytes more)\n", len - max_dump);
    fflush(f);
    fclose(f);
}

static void debug_usb_init(Result rc, int interface_num, int ep_in, int ep_out, int max_packet_in, int max_packet_out) {
    debug_log("[USB][INIT] rc=0x%08X iface=%d ep_in=0x%02X ep_out=0x%02X max_pkt_in=%d max_pkt_out=%d",
              rc, interface_num, ep_in, ep_out, max_packet_in, max_packet_out);
}

static void debug_usb_rx(const char* stage, size_t requested, size_t received) {
    debug_log("[USB][RX][%s] requested=%zu received=%zu", stage, requested, received);
}

static void debug_usb_tx(const char* stage, size_t requested, size_t sent) {
    debug_log("[USB][TX][%s] requested=%zu sent=%zu", stage, requested, sent);
}

static void debug_usb_stall(const char* endpoint, const char* reason) {
    debug_log("[USB][STALL] endpoint=%s reason=%s", endpoint, reason);
}

static void debug_frame(const char* dir, const unsigned char* data, size_t len) {
    if (len < 12) {
        debug_log("[FRAME][%s] INCOMPLETE len=%zu", dir, len);
        return;
    }
    char magic[7] = {0};
    memcpy(magic, data, 6);
    unsigned version = data[6];
    unsigned command = data[7];
    uint32_t payload_len = static_cast<uint32_t>(data[8]) | (static_cast<uint32_t>(data[9]) << 8) |
                           (static_cast<uint32_t>(data[10]) << 16) | (static_cast<uint32_t>(data[11]) << 24);
    debug_log("[FRAME][%s] magic=%s version=%u command=0x%02X (%u) payload_len=%u total_len=%zu",
              dir, magic, version, command, command, payload_len, len);
    debug_hex(dir, data, len);
}

static void debug_batch_begin(const char* stage, bool usbBatchActive, size_t payload_len, bool magic_ok, bool version_ok,
                              bool length_ok, bool state_ok, bool payload_ok) {
    debug_log("[USB][BATCH_BEGIN][%s] state_before=%s payload_len=%zu magic_ok=%d version_ok=%d length_ok=%d state_ok=%d payload_ok=%d",
              stage, usbBatchActive ? "active" : "idle", payload_len,
              magic_ok, version_ok, length_ok, state_ok, payload_ok);
}

static void debug_batch_begin_reject(const char* reason) {
    debug_log("[USB][BATCH_BEGIN][REJECT] reason=%s", reason);
}

static void debug_protocol_state(const char* transition) {
    debug_log("[USB][STATE] %s", transition);
}
struct UsbBatchFile { std::string name; uint64_t size=0, received=0; bool selected=true; };
static std::vector<UsbBatchFile> usbBatch;
static bool usbBatchActive=false;
static unsigned char usbBatchAction=0,usbBatchDestination=0;

// ===== PC -> Switch interactive transfer (USB o TCP, MTUSB1 v1 commands 0x20..0x27) =====
enum class UsbTransferAction : unsigned char { Cancel=0, InstallSd=1, InstallNand=2, CopySd=3 };
struct UsbIncomingTransfer {
    bool pending=false;
    bool decisionReady=false;
    bool choosingPath=false;
    bool active=false;
    bool processing=false;
    bool finished=false;
    bool succeeded=false;
    uint32_t fileIndex=0;
    uint32_t totalFiles=0;
    uint64_t size=0;
    uint64_t received=0;
    uint64_t operationDone=0;
    uint64_t operationTotal=0;
    std::string name;
    std::string directory="sdmc:/";
    std::string finalPath;
    std::string receivePath;
    std::string phase;
    std::string resultText;
    UsbTransferAction action=UsbTransferAction::Cancel;
    int menuChoice=0;
};
static UsbIncomingTransfer usbIncoming;
static std::mutex usbIncomingMutex;
static std::condition_variable usbIncomingCv;
static FILE* usbIncomingFile=nullptr; // solo lo usa el worker USB
static std::vector<BrowserEntry> usbIncomingBrowse;
static int usbIncomingBrowseSelected=0;
static SDL_Thread* usbIncomingInstallThread=nullptr;
static std::atomic<bool> usbIncomingCancelRequested{false};
// Canal de lectura aleatoria para instalación directa. El PC sigue siendo quien
// inicia cada intercambio: DIRECT_POLL recoge la petición y DIRECT_DATA la satisface.
static std::condition_variable pcDirectCv;
static bool pcDirectEnabled=false,pcDirectRequestPending=false,pcDirectDataReady=false;
static uint32_t pcDirectRequestId=0;
static uint64_t pcDirectOffset=0;
static uint32_t pcDirectLength=0;
static std::vector<unsigned char> pcDirectData;
// Caché de lectura directa: el Installer suele consumir las NCA secuencialmente en bloques de 1 MiB.
// MT2.4 mantiene dos slots de hasta 32 MiB: uno consumido por Installer y otro llenándose por USB.
static uint64_t pcDirectCacheOffset=0;
static std::vector<unsigned char> pcDirectCache;
static constexpr uint32_t kPcDirectMaxData = 32*1024*1024; // MT2.4: 2 slots de 32 MiB ~= 64 MiB productor/consumidor
static constexpr uint32_t kPcDirectPrefetchThreshold = 256*1024;
// MT2.6: telemetría del pipeline directo. Permite distinguir tiempo de espera
// del productor PC frente a trabajo del Installer/NCM sin contaminar el hot path.
static uint64_t pcDirectWaitMs=0,pcDirectCacheHits=0,pcDirectRequests=0,pcDirectRequestedBytes=0;
static std::vector<BrowserEntry> browse(const std::string& directory);

static void usbIncomingResetFile(bool removePartial){
    if(usbIncomingFile){fflush(usbIncomingFile);fclose(usbIncomingFile);usbIncomingFile=nullptr;}
    if(removePartial&&!usbIncoming.receivePath.empty())std::remove(usbIncoming.receivePath.c_str());
}
static void usbIncomingResetState(){
    usbIncoming.pending=false;usbIncoming.decisionReady=false;usbIncoming.choosingPath=false;usbIncoming.active=false;usbIncoming.processing=false;usbIncoming.finished=false;usbIncoming.succeeded=false;
    usbIncoming.fileIndex=usbIncoming.totalFiles=0;usbIncoming.size=usbIncoming.received=0;usbIncoming.operationDone=usbIncoming.operationTotal=0;usbIncoming.name.clear();
    usbIncoming.directory="sdmc:/";usbIncoming.finalPath.clear();usbIncoming.receivePath.clear();usbIncoming.phase.clear();usbIncoming.resultText.clear();usbIncoming.action=UsbTransferAction::Cancel;usbIncoming.menuChoice=0;usbIncomingCancelRequested=false;
    pcDirectEnabled=false;pcDirectRequestPending=false;pcDirectDataReady=false;pcDirectData.clear();pcDirectCache.clear();pcDirectCacheOffset=0;pcDirectV2ExpectedSeq=0;pcDirectV2Received=0;pcDirectWaitMs=pcDirectCacheHits=pcDirectRequests=pcDirectRequestedBytes=0;
}

struct UsbIncomingInstallJob {
    std::string path,name;
    uint64_t size=0;
    int destination=5;
};
static int usbIncomingInstallWorker(void* raw){
    std::unique_ptr<UsbIncomingInstallJob> job(static_cast<UsbIncomingInstallJob*>(raw));
    std::string error;
    mtinstall::Hooks hooks;
    hooks.cancelled=[](){return magicUsbStop.load()||usbIncomingCancelRequested.load();};
    hooks.progress=[](const std::string& phase,uint64_t done,uint64_t total){
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        usbIncoming.phase=phase;usbIncoming.operationDone=done;usbIncoming.operationTotal=total;
    };
    debug_log("[TRANSFER][INSTALL] begin path=%s name=%s destination=%d",job->path.c_str(),job->name.c_str(),job->destination);
    const bool ok=mtinstall::installLocal(job->path,job->name,job->size,job->destination,hooks,error);
    std::remove(job->path.c_str());
    {
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        usbIncoming.processing=false;usbIncoming.finished=true;usbIncoming.succeeded=ok&&!usbIncomingCancelRequested.load();
        if(usbIncomingCancelRequested.load())usbIncoming.resultText="CANCELLED";
        else if(ok)usbIncoming.resultText="INSTALL_OK";
        else usbIncoming.resultText="ERR install: "+error;
        usbIncoming.phase=usbIncoming.resultText=="INSTALL_OK"?"Instalación completada":usbIncoming.resultText=="CANCELLED"?"Instalación cancelada":("Error: "+error);
        if(ok)usbIncoming.operationDone=usbIncoming.operationTotal;
    }
    debug_log("[TRANSFER][INSTALL] finished ok=%d cancelled=%d error=%s",ok?1:0,usbIncomingCancelRequested.load()?1:0,error.c_str());
    return 0;
}

struct PcDirectInstallJob { std::string name; uint64_t size=0; int destination=5; };
static int pcDirectInstallWorker(void* raw){
    std::unique_ptr<PcDirectInstallJob> job(static_cast<PcDirectInstallJob*>(raw));
    std::string error;
    mtinstall::Hooks hooks;
    hooks.cancelled=[](){return magicUsbStop.load()||usbIncomingCancelRequested.load();};
    hooks.progress=[](const std::string& phase,uint64_t done,uint64_t total){std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.phase=phase;usbIncoming.operationDone=done;usbIncoming.operationTotal=total;};
    const uint64_t sourceSize=job->size;
    mtinstall::ReadAt reader=[sourceSize](uint64_t offset,void* out,size_t amount)->bool{
        if(amount==0||offset>sourceSize||amount>sourceSize-offset)return false;
        for(int attempt=0;attempt<4;attempt++){
            std::unique_lock<std::mutex> lock(usbIncomingMutex);
            if(usbIncomingCancelRequested.load()||magicUsbStop.load())return false;

            auto schedule=[sourceSize](uint64_t requestOffset,uint64_t requestAmount){
                requestAmount=std::min<uint64_t>(requestAmount,sourceSize-requestOffset);
                requestAmount=std::min<uint64_t>(requestAmount,kPcDirectMaxData);
                if(!requestAmount)return;
                pcDirectRequestId++;pcDirectOffset=requestOffset;pcDirectLength=static_cast<uint32_t>(requestAmount);
                pcDirectDataReady=false;pcDirectRequestPending=true;pcDirectData.clear();pcDirectData.reserve(pcDirectLength);
                pcDirectV2ExpectedSeq=0;pcDirectV2Received=0;pcDirectRequests++;pcDirectRequestedBytes+=requestAmount;pcDirectCv.notify_all();
            };

            // 1) Consumir la caché actual. En el primer hit secuencial disparamos ya el
            // siguiente bloque: el PC lo envía mientras Installer/NCM procesa el actual.
            if(!pcDirectCache.empty()&&offset>=pcDirectCacheOffset){
                const uint64_t rel=offset-pcDirectCacheOffset;
                if(rel<=pcDirectCache.size()&&amount<=pcDirectCache.size()-static_cast<size_t>(rel)){
                    memcpy(out,pcDirectCache.data()+static_cast<size_t>(rel),amount);pcDirectCacheHits++;
                    const uint64_t nextOffset=pcDirectCacheOffset+pcDirectCache.size();
                    if(nextOffset<sourceSize&&!pcDirectRequestPending&&!pcDirectDataReady){
                        schedule(nextOffset,kPcDirectMaxData);
                    }
                    return true;
                }
            }

            // 2) Si el prefetch en segundo plano ya terminó y cubre esta lectura, promoverlo
            // sin otra ida/vuelta de control.
            if(pcDirectDataReady&&!pcDirectData.empty()&&offset>=pcDirectOffset){
                const uint64_t rel=offset-pcDirectOffset;
                if(rel<=pcDirectData.size()&&amount<=pcDirectData.size()-static_cast<size_t>(rel)){
                    pcDirectCacheOffset=pcDirectOffset;pcDirectCache=std::move(pcDirectData);
                    pcDirectDataReady=false;pcDirectRequestPending=false;pcDirectData.clear();
                    memcpy(out,pcDirectCache.data()+static_cast<size_t>(rel),amount);pcDirectCacheHits++;
                    return true;
                }
            }

            // 3) Si hay un prefetch en vuelo, esperar a que finalice antes de decidir si
            // sirve para esta lectura o hay que hacer una petición aleatoria nueva.
            if(pcDirectRequestPending&&!pcDirectDataReady){
                const auto waitStarted=std::chrono::steady_clock::now();const bool woke=pcDirectCv.wait_for(lock,std::chrono::seconds(20),[]{return pcDirectDataReady||usbIncomingCancelRequested.load()||magicUsbStop.load();});pcDirectWaitMs+=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-waitStarted).count();
                if(usbIncomingCancelRequested.load()||magicUsbStop.load())return false;
                if(woke&&pcDirectDataReady&&!pcDirectData.empty()&&offset>=pcDirectOffset){
                    const uint64_t rel=offset-pcDirectOffset;
                    if(rel<=pcDirectData.size()&&amount<=pcDirectData.size()-static_cast<size_t>(rel)){
                        pcDirectCacheOffset=pcDirectOffset;pcDirectCache=std::move(pcDirectData);
                        pcDirectDataReady=false;pcDirectRequestPending=false;pcDirectData.clear();
                        memcpy(out,pcDirectCache.data()+static_cast<size_t>(rel),amount);
                        return true;
                    }
                }
                pcDirectRequestPending=false;pcDirectDataReady=false;pcDirectData.clear();
            }

            // 4) Miss real/random access: pedir hasta 32 MiB y bloquear solo esta primera vez.
            uint64_t wanted=amount;
            if(amount>=kPcDirectPrefetchThreshold)wanted=std::max<uint64_t>(wanted,kPcDirectMaxData);
            wanted=std::min<uint64_t>(wanted,sourceSize-offset);
            wanted=std::min<uint64_t>(wanted,kPcDirectMaxData);
            schedule(offset,wanted);
            const auto waitStarted=std::chrono::steady_clock::now();const bool woke=pcDirectCv.wait_for(lock,std::chrono::seconds(20),[]{return pcDirectDataReady||usbIncomingCancelRequested.load()||magicUsbStop.load();});pcDirectWaitMs+=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-waitStarted).count();
            if(usbIncomingCancelRequested.load()||magicUsbStop.load())return false;
            if(woke&&pcDirectDataReady&&pcDirectData.size()==wanted){
                pcDirectCacheOffset=offset;pcDirectCache=std::move(pcDirectData);
                pcDirectDataReady=false;pcDirectRequestPending=false;pcDirectData.clear();
                memcpy(out,pcDirectCache.data(),amount);
                return true;
            }
            pcDirectRequestPending=false;pcDirectDataReady=false;pcDirectData.clear();pcDirectCache.clear();
        }
        return false;
    };
    const bool ok=mtinstall::installReader(reader,job->name,job->size,job->destination,hooks,error);
    {
        std::lock_guard<std::mutex> lock(usbIncomingMutex);pcDirectEnabled=false;pcDirectRequestPending=false;pcDirectDataReady=false;pcDirectData.clear();usbIncoming.processing=false;usbIncoming.finished=true;usbIncoming.succeeded=ok&&!usbIncomingCancelRequested.load();
        if(usbIncomingCancelRequested.load())usbIncoming.resultText="CANCELLED";else if(ok)usbIncoming.resultText="INSTALL_OK|wait_ms="+std::to_string(pcDirectWaitMs)+"|hits="+std::to_string(pcDirectCacheHits)+"|requests="+std::to_string(pcDirectRequests)+"|requested="+std::to_string(pcDirectRequestedBytes);else usbIncoming.resultText="ERR direct: "+error;
        usbIncoming.phase=ok?"Instalación directa completada":(usbIncomingCancelRequested.load()?"Instalación cancelada":"Error instalación directa");if(ok)usbIncoming.operationDone=usbIncoming.operationTotal;
    }
    pcDirectCv.notify_all();return 0;
}


static uint16_t mtp2Read16(const unsigned char* p){return static_cast<uint16_t>(p[0])|static_cast<uint16_t>(p[1])<<8;}
static uint32_t mtp2Read32(const unsigned char* p){return static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;}
static uint64_t mtp2Read64(const unsigned char* p){uint64_t v=0;for(int i=0;i<8;i++)v|=static_cast<uint64_t>(p[i])<<(8*i);return v;}
static void mtp2Put16(std::vector<unsigned char>& out,uint16_t v){out.push_back(static_cast<unsigned char>(v));out.push_back(static_cast<unsigned char>(v>>8));}
static void mtp2Put32(std::vector<unsigned char>& out,uint32_t v){for(int i=0;i<4;i++)out.push_back(static_cast<unsigned char>(v>>(8*i)));}
static void mtp2Put64(std::vector<unsigned char>& out,uint64_t v){for(int i=0;i<8;i++)out.push_back(static_cast<unsigned char>(v>>(8*i)));}
static void mtp2Response(std::vector<unsigned char>& out,unsigned char type,uint16_t flags,uint32_t stream,uint32_t seq,uint32_t ackSeq,uint64_t offset,const unsigned char* payload=nullptr,size_t payloadLen=0){
    out.clear();out.reserve(kMtp2HeaderSize+payloadLen);out.insert(out.end(),{'M','T','P','2',2,type});mtp2Put16(out,flags);mtp2Put32(out,stream);mtp2Put32(out,seq);mtp2Put32(out,static_cast<uint32_t>(payloadLen));mtp2Put32(out,ackSeq);mtp2Put64(out,offset);if(payloadLen)out.insert(out.end(),payload,payload+payloadLen);
}
static bool magicMtp2Frame(const unsigned char* data,size_t size,std::vector<unsigned char>& response){
    response.clear();
    if(size<kMtp2HeaderSize||memcmp(data,"MTP2",4)!=0||data[4]!=2)return false;
    const unsigned char type=data[5];const uint16_t flags=mtp2Read16(data+6);const uint32_t stream=mtp2Read32(data+8);const uint32_t seq=mtp2Read32(data+12);const uint32_t length=mtp2Read32(data+16);const uint64_t offset=mtp2Read64(data+24);
    if(length>kMtp2ChunkSize||size!=kMtp2HeaderSize+static_cast<size_t>(length))return false;
    const unsigned char* payload=data+kMtp2HeaderSize;const bool ackRequired=(flags&1)!=0;const bool endOfStream=(flags&2)!=0;
    if(type==0x01){
        if(length!=12)return false;const uint32_t wantedChunk=mtp2Read32(payload);const uint32_t wantedWindow=mtp2Read32(payload+4);const uint32_t chunk=std::max<uint32_t>(64*1024,std::min<uint32_t>(wantedChunk,kMtp2ChunkSize));uint32_t window=std::max<uint32_t>(chunk,std::min<uint32_t>(wantedWindow,kMtp2WindowSize));window=(window/chunk)*chunk;if(!window)window=chunk;unsigned char answer[12];for(int i=0;i<4;i++){answer[i]=static_cast<unsigned char>(chunk>>(8*i));answer[4+i]=static_cast<unsigned char>(window>>(8*i));answer[8+i]=static_cast<unsigned char>(1u>>(8*i));}mtp2Response(response,0x81,0,stream,seq,seq,0,answer,sizeof(answer));return true;
    }
    if(type==0x10){
        if(length!=8)return false;const uint64_t total=mtp2Read64(payload);if(total==0||total>2ull*1024*1024*1024)return false;mtp2BenchActive=true;mtp2BenchStream=stream;mtp2BenchExpectedSeq=0;mtp2BenchExpected=total;mtp2BenchReceived=0;mtp2BenchStarted=SDL_GetTicks();mtp2Response(response,0x90,0,stream,seq,seq,0);return true;
    }
    if(type==0x11){
        if(!mtp2BenchActive||stream!=mtp2BenchStream||seq!=mtp2BenchExpectedSeq||offset!=mtp2BenchReceived||length==0||mtp2BenchReceived+length>mtp2BenchExpected){mtp2BenchActive=false;return false;}mtp2BenchReceived+=length;mtp2BenchExpectedSeq++;if(ackRequired||endOfStream||mtp2BenchReceived==mtp2BenchExpected)mtp2Response(response,0x91,0,stream,seq,seq,mtp2BenchReceived);return true;
    }
    if(type==0x13){
        if(length!=0||!mtp2BenchActive||stream!=mtp2BenchStream)return false;const Uint32 elapsed=std::max<Uint32>(1,SDL_GetTicks()-mtp2BenchStarted);const double mib=mtp2BenchReceived/1048576.0;const double rate=mib/(elapsed/1000.0);char out[192];snprintf(out,sizeof(out),"RAW MT2 windowed: %.2f MiB/s · %.0f MiB · %.2fs%s",rate,mib,elapsed/1000.0,mtp2BenchReceived==mtp2BenchExpected?"":" · INCOMPLETO");mtp2Response(response,0x93,0,stream,seq,seq,mtp2BenchReceived,reinterpret_cast<const unsigned char*>(out),strlen(out));mtp2BenchActive=false;return true;
    }
    if(type==0x21){
        if(usbIncomingCancelRequested.load())return false;std::lock_guard<std::mutex> lock(usbIncomingMutex);if(!usbIncoming.active||usbIncoming.processing||stream!=usbIncoming.fileIndex+1||seq!=mtp2TransferExpectedSeq||offset!=usbIncoming.received||length==0||offset+length>usbIncoming.size||usbIncoming.receivePath.empty())return false;if(!usbIncomingFile){const auto slash=usbIncoming.receivePath.find_last_of('/');const std::string dir=slash==std::string::npos?"sdmc:/":usbIncoming.receivePath.substr(0,slash);if(!makeDirectories(dir))return false;usbIncomingFile=fopen(usbIncoming.receivePath.c_str(),"wb");if(!usbIncomingFile)return false;setvbuf(usbIncomingFile,nullptr,_IOFBF,8*1024*1024);}const size_t written=fwrite(payload,1,length,usbIncomingFile);if(written!=length){usbIncomingResetFile(true);usbIncoming.active=false;return false;}usbIncoming.received+=written;usbIncoming.operationDone=usbIncoming.received;usbIncoming.operationTotal=usbIncoming.size;mtp2TransferExpectedSeq++;if(ackRequired||endOfStream||usbIncoming.received==usbIncoming.size)mtp2Response(response,0xA1,0,stream,seq,seq,usbIncoming.received);return true;
    }
    if(type==0x27){
        std::lock_guard<std::mutex> lock(usbIncomingMutex);if(!pcDirectEnabled||!pcDirectRequestPending||stream!=pcDirectRequestId||seq!=pcDirectV2ExpectedSeq||length==0||offset!=pcDirectOffset+pcDirectV2Received||pcDirectV2Received+length>pcDirectLength)return false;pcDirectData.insert(pcDirectData.end(),payload,payload+length);pcDirectV2Received+=length;pcDirectV2ExpectedSeq++;const bool complete=pcDirectV2Received==pcDirectLength;if(complete){pcDirectDataReady=true;pcDirectRequestPending=false;pcDirectCv.notify_all();}if(ackRequired||endOfStream||complete)mtp2Response(response,0xA7,0,stream,seq,seq,pcDirectV2Received);return true;
    }
    return false;
}

static bool magicUsbText(std::vector<unsigned char>& response,const char* message){
    response.resize(12);response.insert(response.end(),message,message+strlen(message));
    const uint32_t n=static_cast<uint32_t>(response.size()-12);
    response[8]=n;response[9]=n>>8;response[10]=n>>16;response[11]=n>>24;return true;
}
static bool magicUsbError(std::vector<unsigned char>& response,const std::string& message){
    response.resize(12);
    const char* text=message.c_str();response.insert(response.end(),text,text+message.size());
    if(response.size()>=12){const uint32_t n=static_cast<uint32_t>(response.size()-12);response[8]=static_cast<unsigned char>(n);response[9]=static_cast<unsigned char>(n>>8);response[10]=static_cast<unsigned char>(n>>16);response[11]=static_cast<unsigned char>(n>>24);}return true;
}
static bool magicUsbFrame(const unsigned char* data,size_t size,std::vector<unsigned char>& response){
    if(size<12||memcmp(data,"MTUSB1",6)!=0||data[6]!=1)return false;
    const unsigned command=data[7];const uint32_t length=static_cast<uint32_t>(data[8])|static_cast<uint32_t>(data[9])<<8|static_cast<uint32_t>(data[10])<<16|static_cast<uint32_t>(data[11])<<24;
    if(length>2*1024*1024||12+length>size)return false;
    response.assign(data,data+12);response[7]=static_cast<unsigned char>(command|0x80);response[8]=response[9]=response[10]=response[11]=0;
    if(command==1){const char ok[]="MagicTupper 0.4.0-rc.2 MTUSB1 MT2.7 ASYNC1 TCPBASE1 PCFS1 NETCONSENT1 HTTPS1";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==0x30){
        std::lock_guard<std::mutex> lock(pcExplorerMutex);
        return magicUsbText(response,pcExplorerRequest.c_str());
    }
    else if(command==0x31){
        std::lock_guard<std::mutex> lock(pcExplorerMutex);
        std::string payload(reinterpret_cast<const char*>(data+12),length);
        if(length>256*1024)return magicUsbError(response,"ERR oversized PC response");
        auto* obj=json_tokener_parse(payload.c_str());
        const bool matches=pcExplorerValidResponse(obj,pcExplorerId);
        if(obj)json_object_put(obj);
        if(!matches||length>256*1024)return magicUsbError(response,"ERR stale or oversized PC request");
        pcExplorerResponse=payload;pcExplorerRequest.clear();return magicUsbText(response,"OK");
    }
    else if(command==8){magicUsbLocked=true;const char ok[]="LOCKED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==9){magicUsbLocked=false;const char ok[]="UNMOUNTED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==5){
        if(!magicUsbLocked)return magicUsbError(response,"ERR not locked");
        if(length>=27&&data[12]=='B'&&data[13]=='W'){const unsigned index=data[14];if(index>=usbBatch.size())return magicUsbError(response,"ERR batch index");uint64_t offset=0;for(int i=0;i<8;i++)offset|=static_cast<uint64_t>(data[15+i])<<(i*8);const uint32_t amount=static_cast<uint32_t>(data[23])|static_cast<uint32_t>(data[24])<<8|static_cast<uint32_t>(data[25])<<16|static_cast<uint32_t>(data[26])<<24;if(amount>1024*1024||27+amount>12+length)return magicUsbError(response,"ERR batch write frame");const std::string dir="sdmc:/switch/magictupper/incoming";if(!makeDirectories(dir)){const std::string result="ERR mkdir "+std::to_string(errno);return magicUsbError(response,result);}const std::string partial=dir+"/"+std::to_string(index)+".part";FILE* file=fopen(partial.c_str(),offset?"rb+":"wb");if(!file){const std::string result="ERR open "+std::to_string(errno);return magicUsbError(response,result);}if(fseeko(file,static_cast<off_t>(offset),SEEK_SET)!=0){const std::string result="ERR seek "+std::to_string(errno);fclose(file);return magicUsbError(response,result);}const size_t written=fwrite(data+27,1,amount,file);const bool ok=written==amount&&fflush(file)==0;const int writeError=errno;fclose(file);if(!ok){const std::string result="ERR write "+std::to_string(writeError);return magicUsbError(response,result);}usbBatch[index].received=offset+written;const std::string result="OK "+std::to_string(written);response.insert(response.end(),result.begin(),result.end());}
        else {if(length<28||data[12]!='W'||data[13]!='2')return magicUsbError(response,"ERR write frame");const unsigned char* request=data+14;uint64_t offset=0;for(int i=0;i<8;i++)offset|=static_cast<uint64_t>(request[i])<<(i*8);const uint32_t amount=static_cast<uint32_t>(request[8])|static_cast<uint32_t>(request[9])<<8|static_cast<uint32_t>(request[10])<<16|static_cast<uint32_t>(request[11])<<24;const uint16_t pathLength=static_cast<uint16_t>(request[12])|static_cast<uint16_t>(request[13])<<8;if(pathLength==0||pathLength>4096||amount>1024*1024||28+pathLength+amount>12+length)return magicUsbError(response,"ERR write params");std::string path(reinterpret_cast<const char*>(data+28),pathLength);if(!safeSdPath(path))return magicUsbError(response,"ERR path");const std::string partial=path+".part";if(amount==0){if(std::rename(partial.c_str(),path.c_str())!=0)return magicUsbError(response,"ERR rename");const char result[]="COMMITTED";response.insert(response.end(),result,result+sizeof(result)-1);}else{FILE* file=fopen(partial.c_str(),offset?"rb+":"wb");if(!file)return magicUsbError(response,"ERR open");if(fseeko(file,static_cast<off_t>(offset),SEEK_SET)!=0){fclose(file);return magicUsbError(response,"ERR seek");}const size_t written=fwrite(data+28+pathLength,1,amount,file);const bool ok=written==amount&&fflush(file)==0;fclose(file);if(!ok)return magicUsbError(response,"ERR write");const std::string result="OK "+std::to_string(written);response.insert(response.end(),result.begin(),result.end());}}
    }
    else if(command==0x20||command==0x2C||command==0x2D){
        if(command!=0x2D){
        // TRANSFER_BEGIN: size u64 + index u32 + total u32 + nameLen u16 + UTF-8 name.
        // Lo usan por igual USB bulk y TCP: ambos transportes terminan en magicUsbFrame().
        if(!magicUsbLocked)return magicUsbError(response,"ERR not locked");
        if(length<18)return magicUsbError(response,"ERR transfer begin");
        const unsigned char* p=data+12;
        uint64_t fileSize=0;for(int i=0;i<8;i++)fileSize|=static_cast<uint64_t>(p[i])<<(i*8);
        const uint32_t fileIndex=static_cast<uint32_t>(p[8])|static_cast<uint32_t>(p[9])<<8|static_cast<uint32_t>(p[10])<<16|static_cast<uint32_t>(p[11])<<24;
        const uint32_t totalFiles=static_cast<uint32_t>(p[12])|static_cast<uint32_t>(p[13])<<8|static_cast<uint32_t>(p[14])<<16|static_cast<uint32_t>(p[15])<<24;
        const uint16_t nameLen=static_cast<uint16_t>(p[16])|static_cast<uint16_t>(p[17])<<8;
        if(nameLen==0||nameLen>4096||18+nameLen>length||totalFiles==0||fileIndex>=totalFiles)return magicUsbError(response,"ERR transfer metadata");
        {
            std::lock_guard<std::mutex> lock(usbIncomingMutex);
            if(usbIncoming.processing||usbIncoming.active||usbIncoming.pending)return magicUsbError(response,"ERR transfer busy");
            usbIncomingResetFile(true);usbIncomingResetState();usbIncomingCancelRequested=false;
            usbIncoming.pending=true;usbIncoming.fileIndex=fileIndex;usbIncoming.totalFiles=totalFiles;usbIncoming.size=fileSize;
            usbIncoming.name.assign(reinterpret_cast<const char*>(p+18),nameLen);
            if(!safeTransferName(usbIncoming.name)){usbIncomingResetState();return magicUsbError(response,"ERR file name");}
            if(!pcInstallable(usbIncoming.name))usbIncoming.menuChoice=2;
            debug_log("[TRANSFER][BEGIN] index=%u/%u name=%s size=%llu",fileIndex+1,totalFiles,usbIncoming.name.c_str(),static_cast<unsigned long long>(fileSize));
        }
        }
        if(command==0x2C)return magicUsbText(response,"OFFERED");
        {
            std::unique_lock<std::mutex> lock(usbIncomingMutex);
            if(command==0x2D&&!usbIncoming.pending)return magicUsbError(response,"ERR no offer");
            if(command==0x2D&&!usbIncoming.decisionReady)return magicUsbText(response,"WAIT");
            usbIncomingCv.wait(lock,[]{return magicUsbStop.load()||quitting.load()||usbIncoming.decisionReady;});
            if(magicUsbStop||quitting)return magicUsbError(response,"CANCEL");
            std::string safeName=usbIncoming.name;
            if(safeName.find('/')!=std::string::npos||safeName.find('\\')!=std::string::npos||safeName=="."||safeName==".."){
                usbIncoming.action=UsbTransferAction::Cancel;
            }
            const unsigned char action=static_cast<unsigned char>(usbIncoming.action);
            const std::string path=usbIncoming.action==UsbTransferAction::CopySd?usbIncoming.directory:std::string();
            response.push_back(action);
            const uint16_t pathLen=static_cast<uint16_t>(std::min<size_t>(path.size(),65535));
            response.push_back(static_cast<unsigned char>(pathLen));response.push_back(static_cast<unsigned char>(pathLen>>8));
            response.insert(response.end(),path.begin(),path.begin()+pathLen);
            usbIncoming.pending=false;
            usbIncoming.active=usbIncoming.action!=UsbTransferAction::Cancel;
            usbIncoming.processing=false;usbIncoming.operationDone=0;usbIncoming.operationTotal=usbIncoming.size;mtp2TransferExpectedSeq=0;
            usbIncoming.phase="Recibiendo desde el PC";
            if(usbIncoming.active&&usbIncoming.action==UsbTransferAction::CopySd){
                std::string dir=usbIncoming.directory.empty()?"sdmc:/":usbIncoming.directory;
                if(dir.back()!='/')dir.push_back('/');
                usbIncoming.finalPath=dir+safeName;
                struct stat existing{};
                if(!safeSdPath(usbIncoming.finalPath)||lstat(usbIncoming.finalPath.c_str(),&existing)==0){usbIncoming.active=false;return magicUsbError(response,"ERR destination exists or invalid");}
                usbIncomingFile=createTransferStage(usbIncoming.finalPath,usbIncoming.receivePath);
                if(!usbIncomingFile){usbIncoming.active=false;return magicUsbError(response,"ERR create temporary");}
                setvbuf(usbIncomingFile,nullptr,_IOFBF,8*1024*1024);
            }else if(usbIncoming.active){
                const std::string dir="sdmc:/switch/magictupper/incoming";
                if(!makeDirectories(dir)){
                    usbIncoming.action=UsbTransferAction::Cancel;usbIncoming.active=false;
                }else{
                    // Temporal común para instalación desde USB o TCP. El instalador local
                    // necesita acceso aleatorio, por eso se completa primero el paquete.
                    usbIncoming.receivePath=dir+"/pc-"+std::to_string(SDL_GetTicks())+"-"+std::to_string(usbIncoming.fileIndex)+".part";
                }
            }
            debug_log("[TRANSFER][DECISION] action=%u path=%s receive=%s",action,path.c_str(),usbIncoming.receivePath.c_str());
        }
    }
    else if(command==0x21){
        // TRANSFER_DATA: index u32 + offset u64 + amount u32 + bytes.
        if(usbIncomingCancelRequested.load()){std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncomingResetFile(true);usbIncoming.active=false;usbIncoming.finished=true;usbIncoming.resultText="CANCELLED";return magicUsbError(response,"ERR cancelled");}
        if(length<16)return magicUsbError(response,"ERR transfer data");
        const unsigned char* p=data+12;
        const uint32_t index=static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;
        uint64_t offset=0;for(int i=0;i<8;i++)offset|=static_cast<uint64_t>(p[4+i])<<(i*8);
        const uint32_t amount=static_cast<uint32_t>(p[12])|static_cast<uint32_t>(p[13])<<8|static_cast<uint32_t>(p[14])<<16|static_cast<uint32_t>(p[15])<<24;
        if(16ull+amount!=length||amount==0)return magicUsbError(response,"ERR transfer data size");
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        if(!usbIncoming.active||usbIncoming.processing)return magicUsbError(response,"ERR transfer inactive");
        if(index!=usbIncoming.fileIndex||offset!=usbIncoming.received||offset+amount>usbIncoming.size)return magicUsbError(response,"ERR transfer offset");
        if(usbIncoming.receivePath.empty())return magicUsbError(response,"ERR transfer path");
        if(!usbIncomingFile){
            const auto slash=usbIncoming.receivePath.find_last_of('/');
            const std::string dir=slash==std::string::npos?"sdmc:/":usbIncoming.receivePath.substr(0,slash);
            if(!makeDirectories(dir))return magicUsbError(response,"ERR mkdir");
            usbIncomingFile=fopen(usbIncoming.receivePath.c_str(),"wb");
            if(!usbIncomingFile)return magicUsbError(response,"ERR open");
            setvbuf(usbIncomingFile,nullptr,_IOFBF,4*1024*1024);
        }
        const size_t written=fwrite(p+16,1,amount,usbIncomingFile);
        if(written!=amount){usbIncomingResetFile(true);usbIncoming.active=false;return magicUsbError(response,"ERR write");}
        usbIncoming.received+=written;usbIncoming.operationDone=usbIncoming.received;usbIncoming.operationTotal=usbIncoming.size;
        for(int i=0;i<8;i++)response.push_back(static_cast<unsigned char>((usbIncoming.received>>(8*i))&0xff));
        debug_log("[TRANSFER][DATA] index=%u offset=%llu bytes=%u progress=%llu/%llu",index,static_cast<unsigned long long>(offset),amount,static_cast<unsigned long long>(usbIncoming.received),static_cast<unsigned long long>(usbIncoming.size));
    }
    else if(command==0x22){
        // TRANSFER_END: para copia confirma el archivo; para instalación conecta
        // el temporal recibido con mtinstall::installLocal().
        if(length!=4)return magicUsbError(response,"ERR transfer end");
        const unsigned char* p=data+12;
        const uint32_t index=static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;
        UsbTransferAction action=UsbTransferAction::Cancel;std::string receivePath,finalPath,name;uint64_t sizeValue=0;
        {
            std::lock_guard<std::mutex> lock(usbIncomingMutex);
            if(!usbIncoming.active||index!=usbIncoming.fileIndex||usbIncoming.received!=usbIncoming.size){usbIncomingResetFile(true);usbIncoming.active=false;return magicUsbError(response,"ERR incomplete");}
            if(usbIncomingFile){
                const bool flushed=fflush(usbIncomingFile)==0;
                const bool closed=fclose(usbIncomingFile)==0;usbIncomingFile=nullptr;
                if(!flushed||!closed){usbIncomingResetFile(true);usbIncoming.active=false;return magicUsbError(response,"ERR flush or close");}
            }
            action=usbIncoming.action;receivePath=usbIncoming.receivePath;finalPath=usbIncoming.finalPath;name=usbIncoming.name;sizeValue=usbIncoming.size;
        }
        if(action==UsbTransferAction::CopySd){
            bool renamed=false;
            if(sizeValue==0){FILE* empty=fopen(receivePath.c_str(),"wb");if(empty){renamed=fclose(empty)==0;}}
            else renamed=true;
            if(renamed)renamed=publishNewTransfer(receivePath,finalPath);
            if(!renamed){std::remove(receivePath.c_str());std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.active=false;return magicUsbError(response,"ERR rename");}
            {
                std::lock_guard<std::mutex> lock(usbIncomingMutex);
                usbIncoming.active=false;usbIncoming.phase="Copia completada";usbIncoming.operationDone=usbIncoming.operationTotal=sizeValue;
            }
            const char ok[]="COPY_OK";response.insert(response.end(),ok,ok+sizeof(ok)-1);
            debug_log("[TRANSFER][END] copy index=%u path=%s bytes=%llu",index,finalPath.c_str(),static_cast<unsigned long long>(sizeValue));
        }else if(action==UsbTransferAction::InstallSd||action==UsbTransferAction::InstallNand){
            const int destination=action==UsbTransferAction::InstallSd?5:4;
            {
                std::lock_guard<std::mutex> lock(usbIncomingMutex);
                usbIncoming.processing=true;usbIncoming.finished=false;usbIncoming.succeeded=false;usbIncoming.resultText.clear();
                usbIncoming.phase=action==UsbTransferAction::InstallSd?"Instalando en SD":"Instalando en memoria interna";
                usbIncoming.operationDone=0;usbIncoming.operationTotal=sizeValue;
            }
            usbIncomingCancelRequested=false;
            if(usbIncomingInstallThread){SDL_WaitThread(usbIncomingInstallThread,nullptr);usbIncomingInstallThread=nullptr;}
            auto* job=new UsbIncomingInstallJob{receivePath,name,sizeValue,destination};
            usbIncomingInstallThread=SDL_CreateThread(usbIncomingInstallWorker,"pc-install",job);
            if(!usbIncomingInstallThread){delete job;std::remove(receivePath.c_str());std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.processing=false;usbIncoming.active=false;usbIncoming.finished=true;usbIncoming.resultText="ERR install thread";return magicUsbError(response,"ERR install thread");}
            const char started[]="INSTALL_STARTED";response.insert(response.end(),started,started+sizeof(started)-1);
        }else{
            std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncomingResetFile(true);usbIncoming.active=false;return magicUsbError(response,"ERR action");
        }
    }
    else if(command==0x25){
        if(length!=4)return magicUsbError(response,"ERR direct start");
        const unsigned char* p=data+12;const uint32_t index=static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        if(!usbIncoming.active||usbIncoming.processing||index!=usbIncoming.fileIndex||(usbIncoming.action!=UsbTransferAction::InstallSd&&usbIncoming.action!=UsbTransferAction::InstallNand))return magicUsbError(response,"ERR direct state");
        usbIncomingResetFile(true);usbIncoming.receivePath.clear();usbIncoming.received=0;usbIncoming.processing=true;usbIncoming.finished=false;usbIncoming.resultText.clear();usbIncoming.phase="Instalación directa desde PC";usbIncoming.operationDone=0;usbIncoming.operationTotal=usbIncoming.size;usbIncomingCancelRequested=false;pcDirectEnabled=true;pcDirectRequestPending=false;pcDirectDataReady=false;pcDirectData.clear();pcDirectCache.clear();pcDirectCacheOffset=0;pcDirectV2ExpectedSeq=0;pcDirectV2Received=0;pcDirectWaitMs=pcDirectCacheHits=pcDirectRequests=pcDirectRequestedBytes=0;
        if(usbIncomingInstallThread){SDL_WaitThread(usbIncomingInstallThread,nullptr);usbIncomingInstallThread=nullptr;}
        auto* job=new PcDirectInstallJob{usbIncoming.name,usbIncoming.size,usbIncoming.action==UsbTransferAction::InstallSd?5:4};
        usbIncomingInstallThread=SDL_CreateThread(pcDirectInstallWorker,"pc-direct-install",job);if(!usbIncomingInstallThread){delete job;usbIncoming.processing=false;pcDirectEnabled=false;return magicUsbError(response,"ERR direct thread");}
        const char ok[]="DIRECT_STARTED";response.insert(response.end(),ok,ok+sizeof(ok)-1);
    }
    else if(command==0x26){
        if(length!=0)return magicUsbError(response,"ERR direct poll");std::lock_guard<std::mutex> lock(usbIncomingMutex);
        if(pcDirectEnabled&&pcDirectRequestPending&&!pcDirectDataReady){response.push_back('R');response.push_back('Q');for(int i=0;i<4;i++)response.push_back(static_cast<unsigned char>(pcDirectRequestId>>(8*i)));for(int i=0;i<8;i++)response.push_back(static_cast<unsigned char>(pcDirectOffset>>(8*i)));for(int i=0;i<4;i++)response.push_back(static_cast<unsigned char>(pcDirectLength>>(8*i)));}
        else {std::string status=usbIncoming.processing?"WAIT":usbIncoming.finished?usbIncoming.resultText:"IDLE";response.insert(response.end(),status.begin(),status.end());}
    }
    else if(command==0x27){
        if(length<16)return magicUsbError(response,"ERR direct data");const unsigned char* p=data+12;const uint32_t requestId=static_cast<uint32_t>(p[0])|static_cast<uint32_t>(p[1])<<8|static_cast<uint32_t>(p[2])<<16|static_cast<uint32_t>(p[3])<<24;uint64_t offset=0;for(int i=0;i<8;i++)offset|=static_cast<uint64_t>(p[4+i])<<(8*i);const uint32_t amount=static_cast<uint32_t>(p[12])|static_cast<uint32_t>(p[13])<<8|static_cast<uint32_t>(p[14])<<16|static_cast<uint32_t>(p[15])<<24;if(amount==0||amount>kPcDirectMaxData||16ull+amount!=length)return magicUsbError(response,"ERR direct size");
        std::lock_guard<std::mutex> lock(usbIncomingMutex);if(!pcDirectEnabled||!pcDirectRequestPending||requestId!=pcDirectRequestId||offset!=pcDirectOffset||amount!=pcDirectLength)return magicUsbError(response,"ERR direct request");pcDirectData.assign(p+16,p+16+amount);pcDirectDataReady=true;pcDirectRequestPending=false;pcDirectCv.notify_all();const char ok[]="DATA_OK";response.insert(response.end(),ok,ok+sizeof(ok)-1);
    }
    else if(command==0x28){
        if(length!=8)return magicUsbError(response,"ERR bench start");
        uint64_t total=0;for(int i=0;i<8;i++)total|=static_cast<uint64_t>(data[12+i])<<(8*i);
        if(total==0||total>2ull*1024*1024*1024)return magicUsbError(response,"ERR bench total");
        linkBenchActive=true;linkBenchExpected=total;linkBenchReceived=0;linkBenchStarted=SDL_GetTicks();
        const char ok[]="BENCH_READY";response.insert(response.end(),ok,ok+sizeof(ok)-1);
    }
    else if(command==0x29){
        if(!linkBenchActive||length<5)return magicUsbError(response,"ERR bench data");
        const uint64_t amount=length-4;linkBenchReceived+=amount;
        if(linkBenchReceived>linkBenchExpected){linkBenchActive=false;return magicUsbError(response,"ERR bench overflow");}
        for(int i=0;i<8;i++)response.push_back(static_cast<unsigned char>((linkBenchReceived>>(8*i))&0xff));
    }
    else if(command==0x2A){
        if(length!=0||!linkBenchActive)return magicUsbError(response,"ERR bench end");
        const Uint32 elapsed=std::max<Uint32>(1,SDL_GetTicks()-linkBenchStarted);
        const double mib=linkBenchReceived/1048576.0;const double rate=mib/(elapsed/1000.0);
        char out[160];snprintf(out,sizeof(out),"RAW MTUSB: %.2f MiB/s · %.0f MiB · %.2fs%s",rate,mib,elapsed/1000.0,linkBenchReceived==linkBenchExpected?"":" · INCOMPLETO");
        response.insert(response.end(),out,out+strlen(out));linkBenchActive=false;
    }
    else if(command==0x23){
        usbIncomingCancelRequested=true;pcDirectCv.notify_all();
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        if(usbIncoming.processing){usbIncoming.phase="Cancelando instalación...";const char ok[]="CANCEL_REQUESTED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
        else {usbIncomingResetFile(true);usbIncomingResetState();usbIncomingCv.notify_all();const char ok[]="CANCELLED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
        debug_log("[TRANSFER][CANCEL]");
    }
    else if(command==0x24){
        std::lock_guard<std::mutex> lock(usbIncomingMutex);
        std::string status;
        if(usbIncoming.processing)status="INSTALLING";
        else if(usbIncoming.finished)status=usbIncoming.resultText.empty()?(usbIncoming.succeeded?"INSTALL_OK":"ERR install"):usbIncoming.resultText;
        else if(usbIncoming.active)status="RECEIVING";
        else status="IDLE";
        response.insert(response.end(),status.begin(),status.end());
    }
    else if(command==0x2B){
        // NET_HTTP_BENCH: streams u8 + total u64 LE + urlLen u16 LE + url.
        // The Switch becomes the HTTP client (Awoo/Tinfoil style) and may pull
        // independent byte ranges concurrently. This measures the useful LAN
        // ceiling of libcurl/Horizon independently of our custom TCP framing.
        if(length<11)return magicUsbError(response,"ERR http bench frame");
        const unsigned char* p=data+12;
        const uint32_t streams=p[0];
        uint64_t total=0;for(int i=0;i<8;i++)total|=static_cast<uint64_t>(p[1+i])<<(8*i);
        const uint16_t urlLen=static_cast<uint16_t>(p[9])|static_cast<uint16_t>(p[10])<<8;
        if(streams<1||streams>4||total<1024*1024||urlLen==0||urlLen>1024||11u+urlLen!=length)return magicUsbError(response,"ERR http bench params");
        const std::string url(reinterpret_cast<const char*>(p+11),urlLen);
        if(url.rfind("http://",0)!=0)return magicUsbError(response,"ERR http bench url");
        std::vector<NetHttpBenchJob> jobs(streams);
        std::vector<SDL_Thread*> threads(streams,nullptr);
        const uint64_t base=total/streams,extra=total%streams;
        uint64_t cursor=0;
        const Uint32 started=SDL_GetTicks();
        bool launchOk=true;
        for(uint32_t i=0;i<streams;i++){
            const uint64_t amount=base+(i<extra?1:0);
            jobs[i].url=url;jobs[i].first=cursor;jobs[i].last=cursor+amount-1;cursor+=amount;
            threads[i]=SDL_CreateThread(netHttpBenchWorker,"net-http-bench",&jobs[i]);
            if(!threads[i]){launchOk=false;break;}
        }
        uint64_t received=0;bool ok=launchOk;
        for(uint32_t i=0;i<streams;i++)if(threads[i]){SDL_WaitThread(threads[i],nullptr);threads[i]=nullptr;received+=jobs[i].received;if(jobs[i].rc!=CURLE_OK||jobs[i].http!=206||jobs[i].received!=(jobs[i].last-jobs[i].first+1))ok=false;}
        const Uint32 elapsed=std::max<Uint32>(1,SDL_GetTicks()-started);
        const double rate=received/1048576.0/(elapsed/1000.0);
        char out[320];
        if(ok&&received==total)
            snprintf(out,sizeof(out),"HTTP_PULL streams=%u: %.2f MiB/s · %.0f MiB · %.2fs · OK",streams,rate,received/1048576.0,elapsed/1000.0);
        else {
            const NetHttpBenchJob* failed=nullptr;
            for(uint32_t i=0;i<streams;i++)if(jobs[i].rc!=CURLE_OK||jobs[i].http!=206||jobs[i].received!=(jobs[i].last-jobs[i].first+1)){failed=&jobs[i];break;}
            if(failed) snprintf(out,sizeof(out),"HTTP_PULL streams=%u: %.2f MiB/s · %.0f MiB · %.2fs · INCOMPLETO · curl=%d http=%ld got=%llu expected=%llu · %s",streams,rate,received/1048576.0,elapsed/1000.0,static_cast<int>(failed->rc),failed->http,static_cast<unsigned long long>(failed->received),static_cast<unsigned long long>(failed->last-failed->first+1),failed->error[0]?failed->error:"sin detalle");
            else snprintf(out,sizeof(out),"HTTP_PULL streams=%u: %.2f MiB/s · %.0f MiB · %.2fs · INCOMPLETO",streams,rate,received/1048576.0,elapsed/1000.0);
        }
        response.insert(response.end(),out,out+strlen(out));
        debug_log("[NET][HTTP_PULL] streams=%u bytes=%llu elapsed=%u rate=%.2f ok=%d",streams,static_cast<unsigned long long>(received),elapsed,rate,ok&&received==total);
    }
    else if(command==10){
        debug_log("[USB][BATCH_BEGIN] recibido state_before=%s payload_len=%u", usbBatchActive?"active":"idle", length);
        debug_hex("BATCH_BEGIN_RX", data, size);
        
        bool magic_ok = (size >= 6 && memcmp(data,"MTUSB1",6)==0);
        bool version_ok = (size >= 7 && data[6]==1);
        bool length_ok = (length >= 6);
        bool state_ok = true;
        bool payload_ok = false;
        
        if(length<6){
            debug_batch_begin("validando", usbBatchActive, length, magic_ok, version_ok, length_ok, state_ok, payload_ok);
            debug_batch_begin_reject("length < 6");
            return magicUsbError(response,"ERR begin frame");
        }
        bool marker_ok = (data[12]=='B' && data[13]=='2');
        debug_log("[USB][BATCH_BEGIN] marker='%c%c' marker_ok=%d", data[12], data[13], marker_ok);
        if(!marker_ok){
            debug_batch_begin("validando", usbBatchActive, length, magic_ok, version_ok, length_ok, state_ok, payload_ok);
            debug_batch_begin_reject("invalid marker (expected 'B''2')");
            return magicUsbError(response,"ERR begin frame");
        }
        
        const uint32_t count=data[14]|data[15]<<8|data[16]<<16|data[17]<<24;
        debug_log("[USB][BATCH_BEGIN] count=%u (max 64)", count);
        if(count>64){
            debug_batch_begin("validando", usbBatchActive, length, magic_ok, version_ok, length_ok, state_ok, payload_ok);
            debug_batch_begin_reject("count > 64");
            return magicUsbError(response,"ERR too many files");
        }
        
        payload_ok = true;
        debug_batch_begin("validando_ok", usbBatchActive, length, magic_ok, version_ok, length_ok, state_ok, payload_ok);
        
        usbBatch.clear();usbBatch.resize(count);usbBatchActive=true;const char ok[]="BATCH_READY";response.insert(response.end(),ok,ok+sizeof(ok)-1);
        debug_log("[USB][BATCH_BEGIN] ACCEPTED count=%u -> BATCH_READY", count);
    }
    else if(command==11){if(!usbBatchActive)return magicUsbError(response,"ERR no batch");if(length<25||data[12]!='B'||data[13]!='2')return magicUsbError(response,"ERR file frame");const unsigned index=data[14];if(index>=usbBatch.size())return magicUsbError(response,"ERR file index");uint64_t sizeValue=0;for(int i=0;i<8;i++)sizeValue|=static_cast<uint64_t>(data[15+i])<<(i*8);const uint16_t n=data[23]|data[24]<<8;if(n==0||n>4096||25+n>12+length)return magicUsbError(response,"ERR file name");usbBatch[index]={std::string(reinterpret_cast<const char*>(data+25),n),sizeValue,0,true};const char ok[]="FILE_READY";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==12){if(length<2||data[12]>=usbBatch.size())return magicUsbError(response,"ERR select");usbBatch[data[12]].selected=data[13]!=0;const char ok[]="SELECTED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==13||command==14){if(length<2)return magicUsbError(response,"ERR decision");usbBatchAction=data[12];usbBatchDestination=data[13];const char ok[]="DECISION_OK";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==15){if(length<1||data[12]>=usbBatch.size())return magicUsbError(response,"ERR commit");const unsigned i=data[12];if(usbBatch[i].received!=usbBatch[i].size)return magicUsbError(response,"ERR incomplete");usbBatch[i].selected=false;const char ok[]="FILE_COMMITTED";response.insert(response.end(),ok,ok+sizeof(ok)-1);}
    else if(command==16||command==17){const std::string status=usbBatchActive?"BATCH_RECEIVING":"BATCH_IDLE";response.insert(response.end(),status.begin(),status.end());}
    else if(command==2||command==3||command==4){
        std::string path="sdmc:/";const bool rangedRead=command==4&&length>=28&&data[12]=='R'&&data[13]=='2';if(length&&!rangedRead){path.assign(reinterpret_cast<const char*>(data+12),length);if(path=="sdmc:/"){}else if(!safeSdPath(path))return magicUsbError(response,"ERR path");}
        if(command==3){struct stat info{};if(stat(path.c_str(),&info)!=0)return magicUsbError(response,"ERR stat");const std::string result=S_ISDIR(info.st_mode)?"D ":"F "+std::to_string(static_cast<long long>(info.st_size));response.insert(response.end(),result.begin(),result.end());}
        else if(command==4){uint64_t offset=0;uint32_t wanted=512*1024;if(length>=28&&data[12]=='R'&&data[13]=='2'){const unsigned char* request=data+14;for(int i=0;i<8;i++)offset|=static_cast<uint64_t>(request[i])<<(i*8);wanted=static_cast<uint32_t>(request[8])|static_cast<uint32_t>(request[9])<<8|static_cast<uint32_t>(request[10])<<16|static_cast<uint32_t>(request[11])<<24;const uint16_t pathLength=static_cast<uint16_t>(request[12])|static_cast<uint16_t>(request[13])<<8;if(pathLength==0||pathLength>4096||28+pathLength>12+length||wanted==0||wanted>512*1024)return magicUsbError(response,"ERR read params");path.assign(reinterpret_cast<const char*>(data+28),pathLength);if(!safeSdPath(path))return magicUsbError(response,"ERR path");}struct stat info{};if(stat(path.c_str(),&info)!=0||!S_ISREG(info.st_mode)||offset>=static_cast<uint64_t>(info.st_size))return magicUsbError(response,"ERR stat");FILE* file=fopen(path.c_str(),"rb");if(!file)return magicUsbError(response,"ERR open");if(fseeko(file,static_cast<off_t>(offset),SEEK_SET)!=0){fclose(file);return magicUsbError(response,"ERR seek");}const size_t amount=static_cast<size_t>(std::min<uint64_t>(wanted,static_cast<uint64_t>(info.st_size)-offset));std::vector<unsigned char> content(amount);const size_t got=fread(content.data(),1,content.size(),file);fclose(file);response.insert(response.end(),content.begin(),content.begin()+got);}
        else {
        DIR* dir=opendir(path.c_str());if(!dir)return magicUsbError(response,"ERR opendir");std::string listing;
        for(dirent* item=readdir(dir);item;item=readdir(dir)){const std::string name=item->d_name;if(name=="."||name=="..")continue;const std::string full=path+(path.back()=='/'?"":"/")+name;struct stat info{};if(stat(full.c_str(),&info)!=0)continue;listing+=(S_ISDIR(info.st_mode)?"D ":"F ")+name+"\n";if(listing.size()>1024*1024){closedir(dir);return magicUsbError(response,"ERR directory listing too large");}}
        closedir(dir);response.insert(response.end(),listing.begin(),listing.end());
        }
    } else return magicUsbError(response,"ERR unknown");
    const uint32_t n=static_cast<uint32_t>(response.size()-12);response[8]=n;response[9]=n>>8;response[10]=n>>16;response[11]=n>>24;return true;
}

static void magicUsbStopAndCleanup() {
    if (magicUsbDevice) {
        debug_log("[USB] Stopping and cleaning up USB device");
        magicUsbStop = true;
        usbIncomingCv.notify_all();
        if (magicUsbThread) {
            SDL_WaitThread(magicUsbThread, nullptr);
            magicUsbThread = nullptr;
        }
        usbCommsExit();
        magicUsbDevice = false;
    }
}

static int magicUsbWorker(void*); // forward declaration

static bool magicUsbStart() {
    if (magicUsbDevice) {
        debug_log("[USB] Already initialized");
        return true;
    }
    const Result rc = usbCommsInitialize();
    debug_usb_init(rc, 0, 0x81, 0x01, 512, 512);
    if (R_FAILED(rc)) {
        debug_log("[USB] usbCommsInitialize failed rc=0x%08X", rc);
        return false;
    }
    magicUsbDevice = true;
    magicUsbStop = false;
    magicUsbWorkerEntered = false;
    magicUsbThread = SDL_CreateThread(magicUsbWorker, "magic-usb", nullptr);
    if (!magicUsbThread) {
        debug_log("[USB] Failed to create worker thread");
        usbCommsExit();
        magicUsbDevice = false;
        return false;
    }
    debug_log("[USB] usbComms initialized, SDL worker thread created");
    return true;
}

static int magicUsbWorker(void*){
    magicUsbWorkerEntered = true;
    debug_log("[USB][WORKER] ENTER aligned bulk-frame mode");

    // libnx usbComms funciona de forma mucho mas predecible para transferencias
    // grandes cuando el buffer entregado al endpoint esta alineado a pagina.
    // Usamos el mismo patron habitual de homebrew USB de Switch: 0x1000 de
    // alineacion y un buffer de transferencia de 2 MiB.
    constexpr size_t kHeaderSize = 12;
    constexpr size_t kUsbBufferSize = 0x200000; // 2 MiB, multiplo de 0x1000
    constexpr size_t kMaxPayload = kUsbBufferSize - kHeaderSize;

    unsigned char* frame = static_cast<unsigned char*>(aligned_alloc(0x1000, kUsbBufferSize));
    if (!frame) {
        debug_log("[USB][WORKER] ERROR aligned_alloc(0x1000, 0x%zx) failed", kUsbBufferSize);
        return -1;
    }

    const uintptr_t frameAddr = reinterpret_cast<uintptr_t>(frame);
    debug_log("[USB][WORKER] RX buffer=%p capacity=%zu alignment_offset=0x%03llX",
              frame,
              kUsbBufferSize,
              static_cast<unsigned long long>(frameAddr & 0xFFFu));

    std::vector<unsigned char> response;

    while(!magicUsbStop){
        const size_t received = usbCommsRead(frame, kUsbBufferSize);

        if (magicUsbStop) break;
        if (!received) {
            // Un retorno 0 no es un frame MTUSB1 valido. No tocamos el estado
            // del protocolo y volvemos a publicar una lectura limpia.
            debug_log("[USB][WORKER] Frame read returned 0");
            continue;
        }
        const bool mtp2 = received >= 4 && memcmp(frame,"MTP2",4)==0;
        if (mtp2) {
            if(received<kMtp2HeaderSize){debug_log("[USB][MT2] short header %zu",received);continue;}
            const uint32_t payload=mtp2Read32(frame+16);const size_t expected=kMtp2HeaderSize+static_cast<size_t>(payload);
            if(payload>kMtp2ChunkSize||received!=expected){debug_log("[USB][MT2] invalid frame received=%zu expected=%zu payload=%u",received,expected,payload);continue;}
            response.clear();if(!magicMtp2Frame(frame,received,response)){debug_log("[USB][MT2] rejected type=0x%02X",frame[5]);continue;}
        } else {
            if (received < kHeaderSize) {debug_log("[USB][WORKER] Short frame: got %zu, minimum=%zu", received, kHeaderSize);continue;}
            debug_frame("RX_HEADER", frame, kHeaderSize);
            const uint32_t payload = static_cast<uint32_t>(frame[8]) |(static_cast<uint32_t>(frame[9]) << 8) |(static_cast<uint32_t>(frame[10]) << 16) |(static_cast<uint32_t>(frame[11]) << 24);
            if (payload > kMaxPayload) {debug_log("[USB][WORKER] Payload too large: %u, max=%zu", payload, kMaxPayload);continue;}
            const size_t expected = kHeaderSize + static_cast<size_t>(payload);
            if (received != expected) {debug_log("[USB][WORKER] Frame size mismatch: received=%zu expected=%zu payload=%u",received, expected, payload);continue;}
            debug_usb_rx("frame", expected, received);debug_protocol_state("Processing frame");response.clear();
            if (!magicUsbFrame(frame, received, response)) {debug_log("[USB][WORKER] magicUsbFrame returned false, not responding");continue;}
        }

        // MT2 intentionally returns no response for most DATA frames.  Only a
        // cumulative window ACK is emitted, which removes the old stop-and-wait.
        if(response.empty())continue;
        size_t sent = 0;
        while(sent < response.size() && !magicUsbStop){
            const size_t wanted = response.size() - sent;const size_t n = usbCommsWrite(response.data() + sent, wanted);debug_usb_tx(mtp2?"mt2_response":"response_chunk", wanted, n);
            if (!n) {debug_log("[USB][WORKER] Response write returned 0 after %zu/%zu bytes", sent, response.size());break;}sent += n;
        }
        if (sent != response.size()) debug_log("[USB][WORKER] Response incomplete: sent %zu of %zu", sent, response.size());
    }

    free(frame);
    debug_log("[USB][WORKER] Stopped");
    return 0;
}
static bool magicTcpRecvExact(int fd,unsigned char* dst,size_t wanted){size_t got=0;while(got<wanted&&!magicTcpStop){const int n=recv(fd,dst+got,wanted-got,0);if(n<=0)return false;got+=static_cast<size_t>(n);}return got==wanted;}
static int magicTcpWorker(void*){
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(8766);address.sin_addr.s_addr=0;
    magicTcpListen=socket(AF_INET,SOCK_STREAM,0);if(magicTcpListen<0)return 0;int yes=1;setsockopt(magicTcpListen,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    if(bind(magicTcpListen,(sockaddr*)&address,sizeof(address))<0||listen(magicTcpListen,1)<0){close(magicTcpListen);magicTcpListen=-1;return 0;}
    while(!magicTcpStop){
        sockaddr_in client{};socklen_t size=sizeof(client);const int fd=accept(magicTcpListen,(sockaddr*)&client,&size);if(fd<0)continue;
        {std::lock_guard<std::mutex> lock(magicTcpSocketMutex);if(magicTcpStop){close(fd);break;}magicTcpClient=fd;}
        {std::lock_guard<std::mutex> lock(magicTcpPeerMutex);magicTcpPeer=inet_ntoa(client.sin_addr);}
        magicTcpApproval=1;
        const Uint32 approvalStarted=SDL_GetTicks();
        while(!magicTcpStop&&magicTcpApproval==1&&SDL_GetTicks()-approvalStarted<60000)SDL_Delay(50);
        if(magicTcpStop||magicTcpApproval!=2){
            magicTcpApproval=0;std::lock_guard<std::mutex> lock(magicTcpSocketMutex);magicTcpClient=-1;close(fd);continue;
        }
        magicTcpApproval=0;magicTcpConnected=true;
        int one=1;int sockbuf=1024*1024; // MT2.6: restaura el baseline de ~11 MiB/s; MTS4 perfila SO_RCVBUF
        setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
        setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
        setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&sockbuf,sizeof(sockbuf));
        setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&sockbuf,sizeof(sockbuf));
        timeval sendTimeout{30,0};setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&sendTimeout,sizeof(sendTimeout));
        timeval receiveTimeout{120,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&receiveTimeout,sizeof(receiveTimeout));
        std::vector<unsigned char> frame(2*1024*1024+64),response;
        while(!magicTcpStop){
            if(!magicTcpRecvExact(fd,frame.data(),4))break;size_t total=0;
            if(memcmp(frame.data(),"MTS4",4)==0){
                // MT2.4 TCP profiler: mide el mismo stream continuo con varios SO_RCVBUF.
                // Cabecera: MTS4 + version u32 + requested_rcvbuf u32 + total u64.
                if(!magicTcpRecvExact(fd,frame.data()+4,16))break;
                const uint32_t version=mtp2Read32(frame.data()+4); if(version!=1)break;
                const uint32_t requested=mtp2Read32(frame.data()+8);
                uint64_t wanted=0; for(int i=0;i<8;i++)wanted|=static_cast<uint64_t>(frame[12+i])<<(i*8);
                if(wanted==0||wanted>1024ULL*1024*1024)break;
                if(requested>=32*1024&&requested<=4*1024*1024){int rcv=static_cast<int>(requested);setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&rcv,sizeof(rcv));}
                int actual=0;socklen_t actualLen=sizeof(actual);getsockopt(fd,SOL_SOCKET,SO_RCVBUF,&actual,&actualLen);
                std::vector<unsigned char> streamBuf(1024*1024); uint64_t got=0; const Uint64 started=SDL_GetPerformanceCounter();
                while(got<wanted&&!magicTcpStop){const size_t ask=static_cast<size_t>(std::min<uint64_t>(streamBuf.size(),wanted-got));const int n=recv(fd,streamBuf.data(),ask,0);if(n<=0)break;got+=static_cast<uint64_t>(n);}
                const Uint64 ended=SDL_GetPerformanceCounter(); if(got!=wanted)break;
                unsigned char ack[20]={'M','4','O','K'}; for(int i=0;i<8;i++)ack[4+i]=static_cast<unsigned char>((got>>(i*8))&0xff);
                const uint32_t ms=static_cast<uint32_t>((ended-started)*1000ULL/SDL_GetPerformanceFrequency()); for(int i=0;i<4;i++)ack[12+i]=static_cast<unsigned char>((ms>>(i*8))&0xff);
                const uint32_t actualU=actual>0?static_cast<uint32_t>(actual):0;for(int i=0;i<4;i++)ack[16+i]=static_cast<unsigned char>((actualU>>(i*8))&0xff);
                size_t a=0;while(a<sizeof(ack)){const int n=send(fd,ack+a,sizeof(ack)-a,0);if(n<=0){a=0;break;}a+=static_cast<size_t>(n);}if(!a)break;continue;
            } else if(memcmp(frame.data(),"MTS3",4)==0){
                // MT2.3 native TCP stream: no per-frame headers or application ACKs.
                if(!magicTcpRecvExact(fd,frame.data()+4,12))break;
                const uint32_t version=mtp2Read32(frame.data()+4); if(version!=1)break;
                uint64_t wanted=0; for(int i=0;i<8;i++)wanted|=static_cast<uint64_t>(frame[8+i])<<(i*8);
                if(wanted==0||wanted>1024ULL*1024*1024)break;
                std::vector<unsigned char> streamBuf(1024*1024); uint64_t got=0; const Uint64 started=SDL_GetPerformanceCounter();
                while(got<wanted&&!magicTcpStop){const size_t ask=static_cast<size_t>(std::min<uint64_t>(streamBuf.size(),wanted-got));const int n=recv(fd,streamBuf.data(),ask,0);if(n<=0)break;got+=static_cast<uint64_t>(n);}
                const Uint64 ended=SDL_GetPerformanceCounter(); if(got!=wanted)break;
                unsigned char ack[16]={'M','3','O','K'}; for(int i=0;i<8;i++)ack[4+i]=static_cast<unsigned char>((got>>(i*8))&0xff);
                const uint32_t ms=static_cast<uint32_t>((ended-started)*1000ULL/SDL_GetPerformanceFrequency()); for(int i=0;i<4;i++)ack[12+i]=static_cast<unsigned char>((ms>>(i*8))&0xff);
                size_t a=0;while(a<sizeof(ack)){const int n=send(fd,ack+a,sizeof(ack)-a,0);if(n<=0){a=0;break;}a+=static_cast<size_t>(n);}if(!a)break;continue;
            } else if(memcmp(frame.data(),"MTP2",4)==0){
                if(!magicTcpRecvExact(fd,frame.data()+4,kMtp2HeaderSize-4))break;const uint32_t payload=mtp2Read32(frame.data()+16);if(payload>kMtp2ChunkSize)break;if(!magicTcpRecvExact(fd,frame.data()+kMtp2HeaderSize,payload))break;total=kMtp2HeaderSize+payload;if(!magicMtp2Frame(frame.data(),total,response))break;
            }else{
                if(!magicTcpRecvExact(fd,frame.data()+4,8))break;if(memcmp(frame.data(),"MTUSB1",6)!=0)break;const uint32_t payload=static_cast<uint32_t>(frame[8])|static_cast<uint32_t>(frame[9])<<8|static_cast<uint32_t>(frame[10])<<16|static_cast<uint32_t>(frame[11])<<24;if(payload>2*1024*1024)break;if(!magicTcpRecvExact(fd,frame.data()+12,payload))break;total=12+payload;if(!magicUsbFrame(frame.data(),total,response))break;
            }
            if(response.empty())continue;size_t sent=0;while(sent<response.size()){const int n=send(fd,response.data()+sent,response.size()-sent,0);if(n<=0){sent=0;break;}sent+=static_cast<size_t>(n);}if(!sent)break;
        }
        {std::lock_guard<std::mutex> lock(magicTcpSocketMutex);magicTcpClient=-1;magicTcpConnected=false;close(fd);}
    }
    close(magicTcpListen);magicTcpListen=-1;return 0;
}
static SDL_Renderer* renderer;
static SDL_Texture* backgroundTexture;
static SDL_Texture* mascotTexture;
static TTF_Font *smallFont,*font,*bigFont;
static const char* categories[]={"all","games","dlc","updates","apps","explorer"};
static const char* labels[]={"Todo","ROMS","DLC","Updates","Apps","Explorador"};
static constexpr int categoryCount=6,explorerCategory=5;
struct Guard { Guard(){SDL_LockMutex(mutex);} ~Guard(){SDL_UnlockMutex(mutex);} };
static void message(const std::string& value){Guard guard;state.message=value;}
static void transferMessage(const std::string& value){Guard guard;state.taskMessage=value;}
static void invalidateSession(const std::string& url){Guard guard;for(auto& source:state.sources.entries)if(source.url==url){source.token.clear();source.role.clear();source.role.clear();}if(state.url==url){state.token.clear();state.role.clear();state.users.clear();}}
static const char* sourcesPath="sdmc:/switch/magictupper/sources.json";
static std::string field(json_object* obj,const char* key){json_object* value=nullptr;return obj&&json_object_object_get_ex(obj,key,&value)&&json_object_is_type(value,json_type_string)?json_object_get_string(value):"";}
static bool hexId(const std::string& id){return id.size()==64&&id.find_first_not_of("0123456789abcdef")==std::string::npos;}
static std::string bytes(double n){char b[64];if(n<0)return "N/D";if(n>=1073741824.)snprintf(b,sizeof(b),"%.1f GiB",n/1073741824.);else snprintf(b,sizeof(b),"%.1f MiB",n/1048576.);return b;}
static std::string usbIdentity(const std::string& path){
#ifdef MAGICTUPPER_USBHSFS
    UsbHsFsDevice device{};if(!usbHsFsGetDeviceByPath(path.c_str(),&device))return "";
    return std::to_string(device.usb_if_id)+":"+std::to_string(device.lun)+":"+std::to_string(device.fs_idx)+":"+device.serial_number+":"+std::to_string(device.capacity);
#else
    return "";
#endif
}
static bool usbTaskPresent(const Task& task){return !task.usbDevice.empty()&&task.localPath.rfind("ums",0)==0&&usbIdentity(task.localPath)==task.usbDevice;}
static std::vector<BrowserEntry> browse(const std::string& directory){
    if(directory.rfind("pc:/",0)==0){pcExplorerAsk(directory);return {};}
    std::vector<BrowserEntry> entries;
    if(directory=="explorer:/"){
        std::vector<BrowserEntry> result={{"Explorar SD","sdmc:/",true,0,""},{"Explorar USB","usb:/",true,0,""},{"Explorar PC","pc-host:/",true,0,""},{"Montar XCI · próximamente","mount-xci:/",true,0,""}};
        Guard guard;for(const auto& source:state.sources.entries)result.push_back({"Explorar "+source.name,"source:/"+source.id,true,0,source.id});
        result.push_back({"Conectar SD al PC","pc-connect:/",true,0,""});return result;
    }
    if(directory.rfind("server:/",0)==0){Guard guard;return state.serverBrowserPath==directory?state.serverBrowser:std::vector<BrowserEntry>{};}
    if(directory=="usb:/"){
#ifdef MAGICTUPPER_USBHSFS
        std::vector<UsbHsFsDevice> devices(usbHsFsGetMountedDeviceCount());
        const auto count=devices.empty()?0:usbHsFsListMountedDevices(devices.data(),devices.size());
        for(size_t i=0;i<count;i++){const auto& d=devices[i];entries.push_back({std::string(d.name)+" "+d.product_name+" / "+LIBUSBHSFS_FS_TYPE_STR(d.fs_type),std::string(d.name)+"/",true,0});}
#endif
        return entries;
    }
    DIR* handle=opendir(directory.c_str());if(!handle)return entries;
    for(dirent* item=readdir(handle);item;item=readdir(handle)){
        const std::string name=item->d_name;if(name=="."||name=="..")continue;
        const std::string path=directory+(directory.back()=='/'?"":"/")+name;struct stat info{};
        if(stat(path.c_str(),&info)!=0)continue;
        entries.push_back({name,path,S_ISDIR(info.st_mode),info.st_size});
    }
    closedir(handle);std::sort(entries.begin(),entries.end(),[](const BrowserEntry& a,const BrowserEntry& b){if(a.directory!=b.directory)return a.directory>b.directory;return a.name<b.name;});return entries;
}
static bool makeDirectories(const std::string& directory){
    if(directory.empty())return false;
    std::string current;
    for(size_t i=0;i<directory.size();i++){
        current+=directory[i];
        if(directory[i]=='/'&&current.size()>1)mkdir(current.c_str(),0777);
    }
    return mkdir(directory.c_str(),0777)==0||errno==EEXIST;
}
static size_t bodyWrite(char* ptr,size_t a,size_t b,void* out){auto& s=*static_cast<std::string*>(out);if(s.size()+a*b>16*1024*1024)return 0;s.append(ptr,a*b);return a*b;}
static int progress(void*,curl_off_t total,curl_off_t done,curl_off_t,curl_off_t){return cancelled?1:0;}
static int controlProgress(void*,curl_off_t,curl_off_t,curl_off_t,curl_off_t){return quitting?1:0;}
static CURL* connection(const std::string& url){
    CURL* c=curl_easy_init();if(!c)return nullptr;
    if(!configureHttpSecurity(c)){curl_easy_cleanup(c);return nullptr;}
    curl_easy_setopt(c,CURLOPT_URL,url.c_str());curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,30L);
    curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(c,CURLOPT_FAILONERROR,1L);
    // Buffers grandes reducen las llamadas de libcurl/SDL y aprovechan mejor la
    // red local; TCP_NODELAY no aporta nada a una transferencia masiva.
    curl_easy_setopt(c,CURLOPT_BUFFERSIZE,4L*1024L*1024L);
    // La SD puede bloquearse durante ráfagas largas; no confundirlo con una
    // caída de red. Solo abortar tras cinco minutos sin alcanzar 128 B/s.
    curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,128L);curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,300L);
    curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(c,CURLOPT_USERAGENT,"MagicTupper/0.1");return c;
}
static json_object* request(const std::string& url,const std::string& token,const std::string* post=nullptr){
    CURL* c=connection(url);if(!c)return nullptr;
    std::string out;curl_slist* h=nullptr;h=curl_slist_append(h,"Content-Type: application/json");
    if(!token.empty())h=curl_slist_append(h,("Authorization: Bearer "+token).c_str());
    const bool catalogRequest=url.size()>=12&&url.compare(url.size()-12,12,"/api/catalog")==0;
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);curl_easy_setopt(c,CURLOPT_TIMEOUT,catalogRequest?180L:30L);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,controlProgress);curl_easy_setopt(c,CURLOPT_FAILONERROR,0L);
    char detail[CURL_ERROR_SIZE]{};curl_easy_setopt(c,CURLOPT_ERRORBUFFER,detail);
    curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,bodyWrite);curl_easy_setopt(c,CURLOPT_WRITEDATA,&out);
    if(post)curl_easy_setopt(c,CURLOPT_POSTFIELDS,post->c_str());
    CURLcode code=curl_easy_perform(c);long status=0;curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
    char* peer=nullptr;long port=0,osError=0;
    curl_easy_getinfo(c,CURLINFO_PRIMARY_IP,&peer);curl_easy_getinfo(c,CURLINFO_PRIMARY_PORT,&port);curl_easy_getinfo(c,CURLINFO_OS_ERRNO,&osError);
    const std::string peerAddress=peer?peer:"";
    curl_slist_free_all(h);curl_easy_cleanup(c);
    if(code!=CURLE_OK||status>=300){
        FILE* log=fopen("sdmc:/switch/magictupper/transfer.log","a");
        if(log){fprintf(log,"request=%s curl=%d http=%ld peer=%s:%ld os_errno=%ld received=%zu detail=%s\n",catalogRequest?"catalog":"session",static_cast<int>(code),status,peerAddress.c_str(),port,osError,out.size(),detail);fclose(log);}
        if(status==401){
            const auto end=url.find("/api/");invalidateSession(url.substr(0,end));
            Sources saved;{Guard guard;saved=state.sources;}writeSources(saved,sourcesPath);
        }
        message(quitting?"Petición cancelada.":status==401?"Sesión inválida. Pulsa Y y elige la fuente para acceder.":status==429?"Demasiados intentos. Espera un minuto.":
            catalogRequest&&code==CURLE_OPERATION_TIMEDOUT?"El catálogo tarda demasiado. Pulsa - para reintentar.":
            code==CURLE_COULDNT_CONNECT?"No conecta al portal. Revisa dominio, puerto y acceso al proxy. Ver transfer.log.":
            code==CURLE_PEER_FAILED_VERIFICATION?"Certificado HTTPS no valido. Revisa dominio, fecha y cadena del proxy.":
            code==CURLE_SSL_CACERT_BADFILE?"Falta o no se puede leer assets/cacert.pem en MagicTupper.":
            status>=300&&status<400?"El portal redirige. Configura la URL HTTPS final de la API.":
            "Error al cargar "+std::string(catalogRequest?"catálogo":"sesión")+" (curl "+std::to_string(static_cast<int>(code))+", HTTP "+std::to_string(status)+"). Ver transfer.log.");
        if(status>=400&&status!=401){auto* problem=json_tokener_parse(out.c_str());const auto explanation=field(problem,"error");if(!explanation.empty())message(explanation);if(problem)json_object_put(problem);}
        return nullptr;
    }
    auto* result=json_tokener_parse(out.c_str());if(!result)message("El servidor devolvió una respuesta inválida.");return result;
}
struct Transfer{FILE* file;double offset,total;Uint32 started,lastUpdate=0;};
static int downloadProgress(void* data,curl_off_t,curl_off_t now,curl_off_t,curl_off_t){
    auto* t=static_cast<Transfer*>(data);const Uint32 tick=SDL_GetTicks();
    // No bloquear el hilo de curl por cada callback de progreso.
    if(tick-t->lastUpdate>=250||now+ t->offset>=t->total){Guard guard;state.done=t->offset+now;state.total=t->total;
        state.speed=now/std::max(0.001,(tick-t->started)/1000.0);t->lastUpdate=tick;}
    return cancelled?1:0;
}
static size_t fileWrite(char* p,size_t a,size_t b,void* data){return fwrite(p,1,a*b,static_cast<Transfer*>(data)->file);}
static std::string safeHomebrewName(const std::string& value);
static bool copyUsb(const Task& task){
    struct stat source{};if(stat(task.localPath.c_str(),&source)!=0||!S_ISREG(source.st_mode)){transferMessage("USB: archivo no accesible.");return false;}
    if(source.st_size!=task.item.size){transferMessage("USB: el archivo ha cambiado desde que se añadió a la cola.");return false;}
    const std::string directory="sdmc:/switch/magictupper/imports";
    if(!makeDirectories(directory)){transferMessage("USB: no se pudo crear imports en la SD.");return false;}
    struct statvfs space{};if(statvfs(directory.c_str(),&space)!=0||static_cast<uint64_t>(space.f_bavail)*space.f_frsize<static_cast<uint64_t>(source.st_size)){transferMessage("No hay espacio disponible en la SD para copiar el archivo.");return false;}
    const std::string destination=directory+"/"+safeHomebrewName(task.item.filename);
    FILE* input=fopen(task.localPath.c_str(),"rb");if(!input){transferMessage("USB: no se pudo abrir el archivo.");return false;}
    const int fd=open(destination.c_str(),O_WRONLY|O_CREAT|O_EXCL,0666);
    if(fd<0){fclose(input);transferMessage(errno==EEXIST?"Ya existe ese archivo en imports. No se ha sobrescrito.":"No se pudo crear el archivo en la SD.");return false;}
    FILE* output=fdopen(fd,"wb");if(!output){close(fd);fclose(input);std::remove(destination.c_str());transferMessage("No se pudo abrir el destino en la SD.");return false;}
    // USBHSFS funciona mejor con lecturas secuenciales grandes; no es un
    // límite de velocidad, sino el tamaño del búfer de cada iteración.
    constexpr size_t copyBufferSize=16*1024*1024;
    setvbuf(input,nullptr,_IOFBF,copyBufferSize);setvbuf(output,nullptr,_IOFBF,copyBufferSize);
    std::vector<unsigned char> buffer(copyBufferSize);int64_t done=0;bool ok=true;const Uint32 started=SDL_GetTicks();Uint32 lastProgress=started;
    transferMessage("Copiando desde USB a imports...");
    while(done<source.st_size&&!cancelled){const size_t wanted=static_cast<size_t>(std::min<int64_t>(buffer.size(),source.st_size-done));const size_t got=fread(buffer.data(),1,wanted,input);if(got!=wanted||fwrite(buffer.data(),1,got,output)!=got){ok=false;break;}done+=got;const Uint32 now=SDL_GetTicks();if(now-lastProgress>=250||done==source.st_size){Guard guard;state.done=done;state.total=source.st_size;state.speed=done/std::max(0.001,(now-started)/1000.0);lastProgress=now;}}
    const bool writeError=ferror(output)!=0;fclose(input);const bool closed=fclose(output)==0;
    if(cancelled){std::remove(destination.c_str());transferMessage("Copia USB anulada.");}
    else if(ok&&closed&&done==source.st_size){transferMessage("Copia USB completada en imports.");return true;}
    else {std::remove(destination.c_str());transferMessage(writeError?"Copia USB fallida: la SD rechazó la escritura.":"Copia USB fallida; no se dejó un archivo parcial.");}
    return false;
}
static std::string safeHomebrewName(const std::string& value){
    std::string result=value.empty()?"homebrew.nro":value;
    for(char& character:result)if(character=='/'||character=='\\'||character==':'||character<32)character='_';
    return result.size()>120?result.substr(result.size()-120):result;
}
static bool explorerCopyFile(const std::string& source,const std::string& destination){
    struct stat info{};if(stat(source.c_str(),&info)!=0||!S_ISREG(info.st_mode)){message("Copiar: solo se pueden copiar archivos.");return false;}
    FILE* input=fopen(source.c_str(),"rb");if(!input){message("Copiar: no se pudo abrir el origen.");return false;}
    FILE* output=fopen(destination.c_str(),"wb");if(!output){fclose(input);message("Pegar: no se pudo crear el destino.");return false;}
    setvbuf(input,nullptr,_IOFBF,8*1024*1024);setvbuf(output,nullptr,_IOFBF,8*1024*1024);std::vector<unsigned char> buffer(8*1024*1024);int64_t done=0;bool ok=true;const Uint32 started=SDL_GetTicks();transferMessage("Copiando: "+destination);
    {Guard guard;state.done=0;state.total=info.st_size;state.speed=0;}
    while(done<static_cast<int64_t>(info.st_size)&&!cancelled){const size_t wanted=static_cast<size_t>(std::min<int64_t>(buffer.size(),static_cast<int64_t>(info.st_size)-done));const size_t got=fread(buffer.data(),1,wanted,input);if(got!=wanted||fwrite(buffer.data(),1,got,output)!=got){ok=false;break;}done+=got;{Guard guard;state.done=done;state.total=info.st_size;state.speed=done/std::max(0.001,(SDL_GetTicks()-started)/1000.0);}}
    const bool closed=fclose(output)==0;fclose(input);{Guard guard;state.done=done;state.total=info.st_size;}
    if(cancelled){std::remove(destination.c_str());transferMessage("Copia cancelada; no se dejó archivo parcial.");return false;}
    if(!ok||!closed||done!=info.st_size){std::remove(destination.c_str());transferMessage("Copia fallida; no se dejó archivo parcial.");return false;}
    transferMessage("Copia completada: "+destination);return true;
}
static int explorerCopyWorker(void* raw){auto* job=static_cast<ExplorerCopyJob*>(raw);explorerCopyFile(job->source,job->destination);delete job;explorerCopyRunning=false;return 0;}
static bool download(const Task& task){
    const auto& item=task.item;
    if(!hexId(item.id)||item.size<0)return false;
    // Keep filenames bounded and independent of server-supplied paths.
    auto dot=item.filename.find_last_of('.');std::string ext=dot==std::string::npos?".bin":item.filename.substr(dot);
    if(ext.size()>8||ext.find_first_not_of(".abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=std::string::npos)ext=".bin";
    if(task.sourceId.empty()||task.sourceId.find_first_not_of("0123456789")!=std::string::npos)return false;
    const bool homebrew=ext==".nro";
    const std::string directory=homebrew?"sdmc:/switch":"sdmc:/switch/magictupper/downloads/source-"+task.sourceId;
    if(!makeDirectories(directory)){transferMessage("No se pudo crear la carpeta de descarga en la SD.");return false;}
    std::string final=directory+"/"+(homebrew?safeHomebrewName(item.filename):item.id+ext),part=final+".part";
    struct stat st{};if(stat(final.c_str(),&st)==0){transferMessage("Ya está descargado en la SD.");return false;}
    int64_t offset=stat(part.c_str(),&st)==0?st.st_size:0;
    if(offset>item.size){transferMessage("El parcial no coincide. Elimina el .part desde tu PC.");return false;}
    struct statvfs sd{};
    if(statvfs("sdmc:/",&sd)!=0){transferMessage("No se puede consultar el espacio libre de la SD.");return false;}
    const uint64_t available=static_cast<uint64_t>(sd.f_bavail)*sd.f_frsize;
    const uint64_t required=static_cast<uint64_t>(item.size-offset)+16*1024*1024;
    if(required>available){transferMessage("No cabe: libres "+bytes(static_cast<double>(available))+" · necesarios "+bytes(static_cast<double>(required))+".");return false;}
    // FAT32 cannot store a single file >= 4 GiB. No automatic splitting is implemented.
    if(item.size>=4294967296LL){transferMessage("Archivo de 4 GiB o más: transferencia pendiente de soporte dividido.");return false;}
    FILE* f=fopen(part.c_str(),"ab");if(!f){transferMessage("No se pudo crear el archivo en la SD.");return false;}
    setvbuf(f,nullptr,_IOFBF,4*1024*1024);
    Transfer t{f,static_cast<double>(offset),static_cast<double>(item.size),SDL_GetTicks()};
    CURL* c=connection(task.url+"/api/files/"+item.id);
    if(!c){fclose(f);transferMessage("No se pudo iniciar la transferencia.");return false;}
    auto* h=curl_slist_append(nullptr,("Authorization: Bearer "+task.token).c_str());
    curl_easy_setopt(c,CURLOPT_HTTPHEADER,h);curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,fileWrite);curl_easy_setopt(c,CURLOPT_WRITEDATA,&t);
    curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,downloadProgress);curl_easy_setopt(c,CURLOPT_XFERINFODATA,&t);
    if(offset)curl_easy_setopt(c,CURLOPT_RESUME_FROM_LARGE,static_cast<curl_off_t>(offset));
    transferMessage(homebrew?"Instalando homebrew: "+item.title:"Descargando: "+item.title);
    char curlError[CURL_ERROR_SIZE]{};
    curl_easy_setopt(c,CURLOPT_ERRORBUFFER,curlError);
    CURLcode result=CURLE_OK;long status=0;
    bool recoverable=false;
    for(int attempt=0;offset<item.size;attempt++){
        curlError[0]=0;
        result=curl_easy_perform(c);
        curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&status);
        errno=0;
        const bool flushed=fflush(f)==0;
        const int flushError=flushed?0:errno;
        // Consultar el descriptor abierto: stat(path) puede fallar mientras
        // libnx mantiene el archivo abierto para escritura en la SD.
        errno=0;
        const auto saved=flushed?ftello(f):static_cast<off_t>(-1);
        const int positionError=saved<0?errno:0;
        recoverable=flushed&&!ferror(f)&&saved>=offset&&saved<item.size&&
            (status==0||status==200||status==206)&&
            (result==CURLE_PARTIAL_FILE||result==CURLE_RECV_ERROR||result==CURLE_OPERATION_TIMEDOUT||
             result==CURLE_COULDNT_CONNECT||result==CURLE_GOT_NOTHING);
        if(result!=CURLE_OK){
            FILE* log=fopen("sdmc:/switch/magictupper/transfer.log","a");
            if(log){fprintf(log,"id=%s attempt=%d offset=%lld saved=%lld expected=%lld curl=%d http=%ld flush_errno=%d position_errno=%d retryable=%d detail=%s\n",
                item.id.c_str(),attempt+1,static_cast<long long>(offset),static_cast<long long>(saved),
                static_cast<long long>(item.size),static_cast<int>(result),status,flushError,positionError,recoverable?1:0,curlError);fclose(log);}
        }
        if(cancelled||!recoverable||attempt>=3)break;
        offset=static_cast<int64_t>(saved);
        transferMessage("Conexión interrumpida. Reanudando desde "+bytes(offset)+" ("+std::to_string(attempt+1)+"/3).");
        for(int wait=0;wait<10*(attempt+1)&&!cancelled;wait++)SDL_Delay(100);
        if(cancelled)break;
        t.offset=static_cast<double>(offset);t.started=SDL_GetTicks();t.lastUpdate=0;
        curl_easy_setopt(c,CURLOPT_RESUME_FROM_LARGE,static_cast<curl_off_t>(offset));
    }
    const bool writeError=ferror(f)!=0;bool closed=fclose(f)==0;curl_slist_free_all(h);curl_easy_cleanup(c);
    if(result==CURLE_OK&&closed&&stat(part.c_str(),&st)==0&&st.st_size==item.size&&rename(part.c_str(),final.c_str())==0){
        FILE* title=fopen((final+".txt").c_str(),"w");if(title){fputs(item.filename.c_str(),title);fclose(title);}
        transferMessage(homebrew?"Homebrew instalado en /switch.":"Descarga terminada. Guardada en /switch/magictupper/downloads.");return true;
    }else {
        // Conservar los bytes confirmados ante cortes de red recuperables.
        const bool aborted=cancelled;
        const bool keepPartial=(aborted||recoverable)&&!writeError&&closed;
        if(!keepPartial)std::remove(part.c_str());
        if(status==401)invalidateSession(task.url);
        transferMessage(aborted&&keepPartial
            ? "Descarga anulada. El parcial se conserva. Pulsa A para reanudar."
                : writeError||!closed
                    ? "Descarga fallida: la SD rechazó la escritura. Comprueba el sistema de archivos y la tarjeta."
                : status==401
                    ? "Descarga fallida: sesión caducada. El parcial se eliminó. Pulsa Y para acceder."
                : "Descarga fallida (curl "+std::to_string(static_cast<int>(result))+", HTTP "+std::to_string(status)+"). "+
                    (keepPartial?"Parcial conservado. Pulsa A para reanudar.":"El parcial se eliminó. Pulsa A para reintentarlo desde cero."));
    }
    return false;
}
static std::string appDigest(const std::string& path){
    FILE* file=fopen(path.c_str(),"rb");if(!file)return "";Sha256Context context;sha256ContextCreate(&context);std::vector<uint8_t> buffer(256*1024);
    size_t read;while((read=fread(buffer.data(),1,buffer.size(),file)))sha256ContextUpdate(&context,buffer.data(),read);
    const bool ok=!ferror(file);fclose(file);if(!ok)return "";uint8_t digest[32];sha256ContextGetHash(&context,digest);char hex[65];for(int i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",digest[i]);return hex;
}
static bool installApp(const Task& task){
    auto* manifest=request(task.url+"/api/apps/"+task.item.id,task.token);if(!manifest){transferMessage("No se pudo preparar la App. Actualiza el servidor y el catálogo.");return false;}
    std::vector<AppFile> files;std::string executable,error;uint64_t total=0;const bool valid=appPlan(manifest,files,executable,total);json_object_put(manifest);
    if(!valid){transferMessage("Manifiesto de App inválido.");return false;}
    struct statvfs sd{};if(statvfs("sdmc:/",&sd)!=0||uint64_t(sd.f_bavail)*sd.f_frsize<total+16*1024*1024){transferMessage("No hay espacio suficiente para preparar la App y sus recursos.");return false;}
    if(!hexId(task.item.id)||!makeDirectories("sdmc:/switch/.magictupper-apps")){transferMessage("No se pudo preparar el temporal de Apps.");return false;}
    uint64_t completed=0;const auto started=SDL_GetTicks();
    mtinstall::Hooks hooks;hooks.cancelled=[](){return cancelled.load();};hooks.progress=[&](const std::string& phase,uint64_t done,uint64_t size){Guard guard;state.taskMessage=phase;state.done=done;state.total=size;state.speed=done/std::max(0.001,(SDL_GetTicks()-started)/1000.0);};
    auto fetch=[&](const AppFile& file,const std::string& target){
        FILE* output=fopen(target.c_str(),"wb");if(!output){error="No se pudo guardar un recurso de la App.";return false;}
        mtinstall::Remote remote(task.url+"/api/files/"+file.id,task.token,file.size,hooks,error,[](const std::string&){},[](){SDL_Delay(250);});
        std::vector<uint8_t> buffer(256*1024);bool ok=true;
        for(uint64_t offset=0;offset<file.size;){size_t count=std::min<uint64_t>(buffer.size(),file.size-offset);if(!remote.read(offset,buffer.data(),count)||fwrite(buffer.data(),1,count,output)!=count){ok=false;break;}offset+=count;hooks.progress("App / "+file.path,completed+offset,total);}
        if(fclose(output)!=0)ok=false;
        completed+=file.size;return ok;
    };
    const bool ok=installAppFiles(files,"sdmc:/switch","sdmc:/switch/.magictupper-apps/"+task.item.id,fetch,appDigest,hooks.cancelled,error);
    if(ok)hooks.progress("App y recursos instalados: "+executable,total,total);else transferMessage(error);return ok;
}
static bool sessionUser(const Task& task,bool admin=false){
    auto* result=request(task.url+"/api/me",task.token);if(!result)return false;
    json_object* user=nullptr;std::string role,name;
    if(json_object_object_get_ex(result,"user",&user)){role=field(user,"role");name=field(user,"username");}
    json_object_put(result);if(role=="user"||role=="standar")role="standard";
    if(role!="admin"&&role!="standard"){message("La sesión no tiene un rol válido.");return false;}
    {Guard guard;if(auto* source=state.sources.find(task.sourceId))source->role=role;if(state.sources.active==task.sourceId){state.role=role;state.username=name;}}
    if(admin&&role!="admin"){message("Solo un administrador puede modificar o borrar.");return false;}
    return true;
}
static void refreshUsers(const Task& task){
    auto* result=request(task.url+"/api/admin/users",task.token);if(!result)return;
    json_object* list=nullptr;std::vector<AdminUser> users;
    if(json_object_object_get_ex(result,"users",&list)&&json_object_is_type(list,json_type_array)){
        for(size_t i=0;i<json_object_array_length(list);i++){auto* row=json_object_array_get_idx(list,i);json_object* enabled=nullptr;AdminUser user{field(row,"id"),field(row,"username"),field(row,"role"),true};if(json_object_object_get_ex(row,"enabled",&enabled))user.enabled=json_object_get_boolean(enabled);users.push_back(user);}
        Guard guard;if(state.sources.active==task.sourceId)state.users=std::move(users);
    }
    json_object_put(result);
}
static bool prepareBrowserTransfer(const Task& task){
    if(task.item.category!="browser")return true;
    if(!validRelativePath(task.item.relativePath))return false;
    const auto slash=task.item.relativePath.find_last_of('/');const std::string parent=slash==std::string::npos?"":task.item.relativePath.substr(0,slash);
    CURL* encoder=curl_easy_init();if(!encoder)return false;char* escaped=curl_easy_escape(encoder,parent.c_str(),0);
    auto* data=escaped?request(task.url+"/api/browse?path="+escaped,task.token):nullptr;
    if(escaped)curl_free(escaped);
    curl_easy_cleanup(encoder);
    json_object* entries=nullptr;bool found=false;
    if(data&&json_object_object_get_ex(data,"entries",&entries)&&json_object_is_type(entries,json_type_array)){
        for(size_t i=0;i<json_object_array_length(entries);i++){auto* item=json_object_array_get_idx(entries,i);if(field(item,"relativePath")==task.item.relativePath&&field(item,"id")==task.item.id){found=true;break;}}
    }
    if(data)json_object_put(data);
    return found;
}
static int worker(void* ptr){
    auto* task=static_cast<Task*>(ptr);
    bool succeeded=false;
    const bool browserReady=!transferTask(*task)||localTask(*task)||prepareBrowserTransfer(*task);
    if(task->kind==1){
        auto* data=json_object_new_object();json_object_object_add(data,"username",json_object_new_string(task->user.c_str()));json_object_object_add(data,"password",json_object_new_string(task->password.c_str()));
        std::string body=json_object_to_json_string_ext(data,JSON_C_TO_STRING_PLAIN);json_object_put(data);
        auto* login=request(task->url+"/api/login","",&body);std::fill(body.begin(),body.end(),0);std::fill(task->password.begin(),task->password.end(),0);
        if(login){
            task->token=field(login,"token");
            json_object* loggedUser=nullptr;std::string loggedName;
            if(json_object_object_get_ex(login,"user",&loggedUser))loggedName=field(loggedUser,"username");
            json_object_put(login);
            Sources saved;
            {Guard guard;state.username=loggedName;state.message="Bienvenido, "+loggedName+". El pepinillo ha validado tu acceso.";auto* source=state.sources.find(task->sourceId);if(source)source->token=task->token;if(state.sources.active==task->sourceId)state.token=task->token;saved=state.sources;}
            if(!writeSources(saved,sourcesPath))message("Sesión iniciada, pero no se pudo guardar el token en la SD.");
        }
        else task->token.clear();
    }
    if(!browserReady)transferMessage("No se pudo recuperar el archivo del explorador. Revisa la conexión y vuelve a seleccionarlo si cambió.");
    else if(task->kind==3||task->kind==10){
        std::string error;const Uint32 started=SDL_GetTicks();
        mtinstall::Hooks hooks;
        hooks.cancelled=[](){return cancelled.load();};
        hooks.progress=[&](const std::string& phase,uint64_t done,uint64_t total){Guard guard;state.taskMessage=phase;state.done=done;state.total=total;state.speed=done/std::max(0.001,(SDL_GetTicks()-started)/1000.0);};
        if(task->kind==10){
            const bool fromSd=task->localPath.rfind("sdmc:/",0)==0;
            const bool fromUsb=task->localPath.rfind("ums",0)==0;
            if(fromUsb&&!usbTaskPresent(*task))transferMessage("USB desconectado o cambiado. Vuelve a seleccionar el archivo.");
            else if(!fromSd&&!fromUsb)transferMessage("Origen local no permitido para instalación.");
            else {succeeded=mtinstall::installLocal(task->localPath,task->item.filename,task->item.size,task->destination,hooks,error);if(!succeeded)transferMessage(error+" Consulta install.log.");}
        }
        else if(!hexId(task->item.id)||task->item.size<=0)transferMessage("Paquete de instalación inválido.");
        else {succeeded=mtinstall::install(task->url,task->token,task->item.id,task->item.filename,task->item.size,task->destination,hooks,error);if(!succeeded)transferMessage(error+" Consulta install.log.");}
    }
    else if(task->kind==12)succeeded=installApp(*task);
    else if(task->kind==2)succeeded=download(*task);
    else if(task->kind==4){if(usbTaskPresent(*task))succeeded=copyUsb(*task);else transferMessage("USB desconectado o cambiado. Vuelve a seleccionar el archivo.");}
    else if(task->kind==5||task->kind==6){
        if(sessionUser(*task,true)){
            if(task->kind==6){
                auto* body=json_object_new_object();json_object_object_add(body,"username",json_object_new_string(task->user.c_str()));json_object_object_add(body,"password",json_object_new_string(task->password.c_str()));json_object_object_add(body,"role",json_object_new_string(task->role.c_str()));
                std::string encoded=json_object_to_json_string_ext(body,JSON_C_TO_STRING_PLAIN);json_object_put(body);
                auto* result=request(task->url+"/api/admin/users",task->token,&encoded);std::fill(encoded.begin(),encoded.end(),0);std::fill(task->password.begin(),task->password.end(),0);
                if(result){json_object_put(result);message("Usuario creado. Ya puede acceder a MagicTupper.");refreshUsers(*task);}
            }else refreshUsers(*task);
        }
    }
    else if(task->kind==7||task->kind==8){
        if(sessionUser(*task,true)){
            if(task->localPath.rfind("sdmc:/",0)!=0)message("Ruta de gestión no permitida.");
            else if(task->kind==7)message(std::remove(task->localPath.c_str())==0?"Archivo eliminado.":"No se pudo eliminar el archivo.");
            else message(makeDirectories(task->localPath)?"Carpeta creada.":"No se pudo crear la carpeta.");
        }
    }
    else if(task->kind==9){
        const std::string body="{}";auto* result=request(task->url+"/api/logout",task->token,&body);if(result)json_object_put(result);
        invalidateSession(task->url);Sources saved;{Guard guard;saved=state.sources;}writeSources(saved,sourcesPath);message("Sesión cerrada. Pulsa A en la fuente para acceder.");
    }
    else if(task->kind==11){
        std::vector<BrowserEntry> entries;
        CURL* encoder=curl_easy_init();char* encoded=encoder?curl_easy_escape(encoder,task->localPath.substr(8).c_str(),0):nullptr;
        auto* data=encoded?request(task->url+"/api/browse?path="+encoded,task->token):nullptr;
        if(encoded)curl_free(encoded);
        if(encoder)curl_easy_cleanup(encoder);
        json_object* list=nullptr;
        const bool valid=data&&json_object_object_get_ex(data,"entries",&list)&&json_object_is_type(list,json_type_array);
        if(valid){
            for(size_t i=0;i<json_object_array_length(list);i++){
                auto* row=json_object_array_get_idx(list,i);json_object *dir=nullptr,*size=nullptr;
                const auto relative=field(row,"relativePath"),id=field(row,"id");
                if(!validRelativePath(relative))continue;
                bool directory=json_object_object_get_ex(row,"directory",&dir)&&json_object_get_boolean(dir);
                int64_t bytes=json_object_object_get_ex(row,"size",&size)?json_object_get_int64(size):0;
                if(directory||hexId(id))entries.push_back({field(row,"name"),"server:/"+relative,directory,bytes,id});
            }
        }
        if(data)json_object_put(data);
        Guard guard;if(state.sources.active==task->sourceId){state.serverBrowserPath=task->localPath;state.serverBrowser=std::move(entries);state.serverBrowserError=valid?"":"No se pudo abrir la carpeta. Revisa el mensaje inferior y actualiza el servidor a 0.3.5.";if(valid)state.message="Carpeta del servidor / "+std::to_string(state.serverBrowser.size())+" elementos";}
    }
    else if(!task->token.empty()&&sessionUser(*task)){
        message("Cargando catálogo. Espera mientras el servidor revisa la biblioteca...");
        auto* data=request(task->url+"/api/catalog",task->token);
        if(data){
            json_object *list=nullptr,*shop=nullptr;std::vector<Item> items;
            if(json_object_object_get_ex(data,"items",&list)&&json_object_is_type(list,json_type_array)){
                for(size_t i=0;i<json_object_array_length(list);i++){
                    auto* obj=json_object_array_get_idx(list,i);json_object* size=nullptr;
                    Item item;item.id=field(obj,"id");item.title=field(obj,"title");item.filename=field(obj,"filename");item.category=field(obj,"category");item.relativePath=field(obj,"relativePath");
                    if(json_object_object_get_ex(obj,"size",&size))item.size=json_object_get_int64(size);
                    item.titleId=field(obj,"titleId");item.applicationId=field(obj,"applicationId");json_object* version=nullptr;
                    if(json_object_object_get_ex(obj,"version",&version)&&json_object_is_type(version,json_type_int))item.version=json_object_get_int64(version);
                    if(hexId(item.id)&&item.size>=0)items.push_back(item);
                }
                Guard guard;state.items=std::move(items);state.message="Portal conectado. Elige tu próxima mala decisión.";
                if(json_object_object_get_ex(data,"shop",&shop)){
                    state.name=field(shop,"name");state.tagline=field(shop,"tagline");
                    const auto color=field(shop,"accent");unsigned r,g,b;
                    if(color.size()==7&&sscanf(color.c_str(),"#%2x%2x%2x",&r,&g,&b)==3)state.accent={static_cast<Uint8>(r),static_cast<Uint8>(g),static_cast<Uint8>(b),255};
                    json_object* jokes=nullptr;if(json_object_object_get_ex(shop,"jokes",&jokes)&&json_object_is_type(jokes,json_type_array)&&json_object_array_length(jokes)){
                        auto* joke=json_object_array_get_idx(jokes,time(nullptr)%json_object_array_length(jokes));if(json_object_is_type(joke,json_type_string))state.joke=json_object_get_string(joke);
                    }
                }
            }else message("Catálogo inválido.");json_object_put(data);
        }
    }
    const bool transfer=transferTask(*task);
    if(transfer){Guard guard;const std::string outcome=succeeded?"Completada":cancelled?"Cancelada":"Error";state.history.push_front(outcome+" | "+taskLabel(*task)+" | "+task->item.title);if(state.history.size()>12)state.history.pop_back();}
    std::fill(task->password.begin(),task->password.end(),0);delete task;
    if(transfer)busy=false;else controlBusy=false;
    return 0;
}
static bool keyboard(const char* title,std::string& text,bool password=false){
    SwkbdConfig config;char buffer[512]={0};if(R_FAILED(swkbdCreate(&config,0)))return false;
    swkbdConfigMakePresetDefault(&config);swkbdConfigSetHeaderText(&config,title);swkbdConfigSetStringLenMax(&config,255);
    swkbdConfigSetInitialText(&config,text.c_str());swkbdConfigSetPasswordFlag(&config,password?1:0);
    Result result=swkbdShow(&config,buffer,sizeof(buffer));swkbdClose(&config);
    if(R_SUCCEEDED(result))text=buffer;
    memset(buffer,0,sizeof(buffer));
    return R_SUCCEEDED(result)&&!text.empty();
}
static void rect(int x,int y,int w,int h,SDL_Color c){SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a);SDL_Rect r{x,y,w,h};SDL_RenderFillRect(renderer,&r);}
struct CachedText {SDL_Texture* texture=nullptr;int width=0,height=0;uint64_t used=0;size_t allocated=0;};
static std::map<std::string,CachedText> textCache;
static uint64_t textClock=0;static size_t textCacheBytes=0;
static void clearTextCache(){for(auto& entry:textCache)SDL_DestroyTexture(entry.second.texture);textCache.clear();textCacheBytes=0;}
static void text(const std::string& value,int x,int y,SDL_Color color,TTF_Font* face=nullptr,int maxWidth=1200){
    if(value.empty()||maxWidth<=0)return;
    if(!face)face=font;
    const std::string key=std::to_string(reinterpret_cast<uintptr_t>(face))+":"+std::to_string(uint32_t(color.r)<<24|uint32_t(color.g)<<16|uint32_t(color.b)<<8|color.a)+":"+std::to_string(maxWidth)+":"+value;
    auto found=textCache.find(key);
    if(found==textCache.end()){
        std::string display=value;int width=0,height=0;
        if(TTF_SizeUTF8(face,display.c_str(),&width,&height)!=0)return;
        if(width>maxWidth){
            do{size_t last=display.size()-1;while(last&&(static_cast<unsigned char>(display[last])&0xc0)==0x80)--last;display.resize(last);TTF_SizeUTF8(face,(display+"…").c_str(),&width,&height);}while(!display.empty()&&width>maxWidth);
            display+="…";
        }
        auto* surface=TTF_RenderUTF8_Blended(face,display.c_str(),color);if(!surface)return;
        auto* texture=SDL_CreateTextureFromSurface(renderer,surface);CachedText cached{texture,std::min(surface->w,maxWidth),surface->h,++textClock};
        const size_t allocated=size_t(surface->w)*surface->h*4;cached.allocated=allocated;SDL_FreeSurface(surface);if(!texture)return;
        while(!textCache.empty()&&(textCache.size()>=192||textCacheBytes+allocated>8*1024*1024)){
            auto oldest=std::min_element(textCache.begin(),textCache.end(),[](const auto& a,const auto& b){return a.second.used<b.second.used;});
            textCacheBytes-=oldest->second.allocated;SDL_DestroyTexture(oldest->second.texture);textCache.erase(oldest);
        }
        textCacheBytes+=cached.allocated;found=textCache.emplace(key,cached).first;
    }
    auto& cached=found->second;cached.used=++textClock;SDL_Rect src{0,0,cached.width,cached.height},dst{x,y,cached.width,cached.height};SDL_RenderCopy(renderer,cached.texture,&src,&dst);
}
static void ellipse(int x,int y,int rx,int ry,SDL_Color c,bool fill=false){
    SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a);
    if(fill){for(int yy=-ry;yy<=ry;yy++){int dx=rx*sqrt(std::max(0.,1.-double(yy*yy)/(ry*ry)));SDL_RenderDrawLine(renderer,x-dx,y+yy,x+dx,y+yy);}}
    else for(int i=0;i<360;i++){double a=i*3.14159265/180,b=(i+1)*3.14159265/180;SDL_RenderDrawLine(renderer,x+rx*cos(a),y+ry*sin(a),x+rx*cos(b),y+ry*sin(b));}
}
static void line(int x1,int y1,int x2,int y2,SDL_Color c,int width=1){
    SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a);for(int i=0;i<width;i++)SDL_RenderDrawLine(renderer,x1,y1+i,x2,y2+i);
}
static void background(){
    if(backgroundTexture){SDL_Rect destination{0,0,1280,720};SDL_RenderCopy(renderer,backgroundTexture,nullptr,&destination);SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);rect(0,0,1280,720,{8,18,27,150});}
    else rect(0,0,1280,720,{8,18,27,255});
    // Dejar que el fondo y Rick se integren con el panel; antes este rectángulo opaco
    // ocultaba casi toda la imagen de fondo.
    rect(0,54,1280,626,{22,24,26,205});
    rect(0,54,1280,3,{85,132,72,190});
    // El portal ya forma parte de fondo.jpg; no lo dibujamos otra vez para
    // evitar una doble ilustración detrás de la tarjeta de Rick.
    rect(24,234,902,357,{5,13,18,165});
}
static void mascot(SDL_Color accent){
    if(mascotTexture){
        rect(952,242,298,244,{13,25,30,245});
        SDL_Rect destination{966,252,270,169};
        SDL_RenderCopy(renderer,mascotTexture,nullptr,&destination);return;
    }
    rect(952,242,298,244,{13,25,30,245});
    ellipse(1100,333,68,82,{19,60,44,255},true);ellipse(1095,326,53,72,{82,181,80,255},true);
    ellipse(1074,310,18,14,{188,241,202,255},true);ellipse(1116,310,18,14,{188,241,202,255},true);
    line(1092,310,1098,310,{9,30,27,255},4);ellipse(1077,310,5,7,{9,30,27,255},true);ellipse(1113,310,5,7,{9,30,27,255},true);
    line(1080,343,1113,343,{9,30,27,255},4);rect(1066,364,66,35,{232,239,219,255});rect(1111,372,9,17,accent);

}
struct Telemetry{std::string battery="BAT N/D",temp="TEMP N/D",sd="SD N/D",internal="INTERNA N/D",ip="IP N/D";};
static std::string switchIp(){
    // gethostid() en libnx devuelve la IP en orden anfitrión; la pasamos de
    // little-endian a la notación decimal clásica.
    const uint32_t host=static_cast<uint32_t>(gethostid());
    if(host!=0&&host!=0x7f000001)return std::to_string(host&255)+"."+std::to_string((host>>8)&255)+"."+std::to_string((host>>16)&255)+"."+std::to_string((host>>24)&255);
    return "";
}
static Telemetry telemetry(bool psm,bool tc,bool ncm){
    Telemetry t;u32 charge;PsmChargerType charger;
    if(psm&&R_SUCCEEDED(psmGetBatteryChargePercentage(&charge))){t.battery="BAT "+std::to_string(charge)+"%";if(R_SUCCEEDED(psmGetChargerType(&charger))&&charger!=PsmChargerType_Unconnected)t.battery+=" +";}
    s32 temp;if(tc&&R_SUCCEEDED(tcGetSkinTemperatureMilliC(&temp)))t.temp="PIEL "+std::to_string(temp/1000)+" °C";
    struct statvfs sd{};if(statvfs("sdmc:/",&sd)==0)t.sd="SD "+bytes(double(sd.f_bavail)*sd.f_frsize)+" libres / "+bytes(double(sd.f_blocks)*sd.f_frsize);
    NcmContentStorage storage{};s64 free,total;
    if(ncm&&R_SUCCEEDED(ncmOpenContentStorage(&storage,NcmStorageId_BuiltInUser))){if(R_SUCCEEDED(ncmContentStorageGetFreeSpaceSize(&storage,&free))&&R_SUCCEEDED(ncmContentStorageGetTotalSpaceSize(&storage,&total)))t.internal="INTERNA "+bytes(free)+" / "+bytes(total);ncmContentStorageClose(&storage);}
    const auto ip=switchIp();if(!ip.empty())t.ip="IP "+ip;return t;
}
static InstalledTitles readInstalledTitles(bool available){
    InstalledTitles result;result.complete=available;if(!available)return result;
    for(const auto storage:{NcmStorageId_BuiltInUser,NcmStorageId_SdCard,NcmStorageId_GameCard}){
        NcmContentMetaDatabase db{};const bool required=storage!=NcmStorageId_GameCard;
        if(R_FAILED(ncmOpenContentMetaDatabase(&db,storage))){if(required)result.complete=false;continue;}
        std::vector<NcmContentMetaKey> keys(256);s32 total=0,written=0;
        Result rc=ncmContentMetaDatabaseList(&db,&total,&written,keys.data(),keys.size(),NcmContentMetaType_Unknown,0,0,UINT64_MAX,NcmContentInstallType_Full);
        if(R_SUCCEEDED(rc)&&total>written&&total<=65536){keys.resize(total);rc=ncmContentMetaDatabaseList(&db,&total,&written,keys.data(),keys.size(),NcmContentMetaType_Unknown,0,0,UINT64_MAX,NcmContentInstallType_Full);}
        ncmContentMetaDatabaseClose(&db);
        if(R_FAILED(rc)||written<0||total>written){if(required)result.complete=false;continue;}
        for(int i=0;i<written;i++){const auto& key=keys[i];if(key.type==0x80)result.games.insert(key.id);else if(key.type==0x81&&(key.id&0xfff)==0x800){auto& version=result.patches[key.id-0x800];version=std::max(version,key.version);}else if(key.type==0x82){auto& version=result.addons[key.id];version=std::max(version,key.version);}}
    }
    return result;
}
int main(int argc,char** argv){
    // Restore the pre-0.3.7 pool configuration for a controlled comparison.
    // Enlarging it did not increase the reported SO_RCVBUF on the tested console.
    if(R_FAILED(socketInitializeDefault()))return 1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    bool psm=R_SUCCEEDED(psmInitialize()),tc=R_SUCCEEDED(tcInitialize()),ncm=R_SUCCEEDED(ncmInitialize()),pl=R_SUCCEEDED(plInitialize(PlServiceType_User));
#ifdef MAGICTUPPER_USBHSFS
    usbHsFsSetFileSystemMountFlags(UsbHsFsMountFlags_ReadOnly);
    const Result usbInitResult=usbHsFsInitialize(0);
    const bool usbReady=R_SUCCEEDED(usbInitResult);
#else
    const bool usbReady=false;
    const Result usbInitResult=0xffffffff;
#endif
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER)!=0||TTF_Init()!=0)return 1;
    auto* window=SDL_CreateWindow("MagicTupper",0,0,1280,720,SDL_WINDOW_SHOWN);renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    PlFontData shared{};if(!renderer||!pl||R_FAILED(plGetSharedFontByType(&shared,PlSharedFontType_Standard)))return 1;
    smallFont=TTF_OpenFontRW(SDL_RWFromConstMem(shared.address,shared.size),1,18);font=TTF_OpenFontRW(SDL_RWFromConstMem(shared.address,shared.size),1,23);bigFont=TTF_OpenFontRW(SDL_RWFromConstMem(shared.address,shared.size),1,42);
    if(!smallFont||!font||!bigFont)return 1;
    auto loadAsset=[](const unsigned char* data,size_t size,const char* fallback){SDL_RWops* memory=SDL_RWFromConstMem(data,static_cast<int>(size));auto* surface=memory?SDL_LoadBMP_RW(memory,1):nullptr;if(!surface)surface=SDL_LoadBMP(fallback);if(!surface)return static_cast<SDL_Texture*>(nullptr);auto* texture=SDL_CreateTextureFromSurface(renderer,surface);SDL_FreeSurface(surface);return texture;};
    backgroundTexture=loadAsset(magictupper_fondo_bmp,magictupper_fondo_bmp_len,"sdmc:/switch/magictupper/assets/fondo.bmp");
    mascotTexture=loadAsset(magictupper_rick_bmp,magictupper_rick_bmp_len,"sdmc:/switch/magictupper/assets/rick.bmp");
    mutex=SDL_CreateMutex();sourceFileMutex=SDL_CreateMutex();if(!mutex||!sourceFileMutex)return 1;
    // El servidor TCP puede arrancar ya; el dispositivo USB se inicia más abajo,
    // cuando la inicialización general de la aplicación ya ha terminado.
    magicTcpStop=false;magicTcpThread=SDL_CreateThread(magicTcpWorker,"magic-tcp",nullptr);
    // rc.5: UDP/8767 belongs to the Atmosphere Remote Play sysmodule.
    // Keep the old LAB receiver compiled for rollback/debugging, but do not bind the port from the NRO.
    remoteInputStop=true;remoteInputThread=nullptr;
    mkdir("sdmc:/switch",0777);mkdir("sdmc:/switch/magictupper",0777);mkdir("sdmc:/switch/magictupper/downloads",0777);
    struct stat sourcesStat{};
    bool sourcesHealthy=true;
    if(stat(sourcesPath,&sourcesStat)==0){
        sourcesHealthy=loadSources(state.sources,sourcesPath);
        if(!sourcesHealthy)state.message="sources.json inválido. Corrígelo en el PC; no se sobrescribirá.";
    }else{
        FILE* config=fopen("sdmc:/switch/magictupper/server.txt","r");
        if(config){
            char b[512];if(fgets(b,sizeof(b),config)){
                std::string url=b;while(!url.empty()&&(url.back()=='\n'||url.back()=='\r'))url.pop_back();
                if(sourceUrl(url)){Source source{state.sources.allocateId(),"Mi PC",url,"",""};state.sources.entries.push_back(source);state.sources.active=source.id;if(!writeSources(state.sources,sourcesPath))state.message="No se pudo guardar la fuente importada.";}
            }fclose(config);
        }
    }
    if(auto* source=state.sources.find(state.sources.active)){state.url=source->url;state.name=source->name;}else state.url.clear();
    PadState pad;padConfigureInput(1,HidNpadStyleSet_NpadStandard);padInitializeDefault(&pad);
    int category=0,selected=0,sourceSelected=0,browserSelected=0,actionChoice=-1,queueSelected=0,adminSelected=0;
    bool sourcesOpen=false,browserOpen=false,queueOpen=false,adminOpen=false,queuePaused=false,newAdmin=false,usbInfoOpen=false;
    bool accountConfirm=false;
    std::string accountName,accountPassword;
    auto clearAccount=[&](){std::fill(accountPassword.begin(),accountPassword.end(),0);accountPassword.clear();accountName.clear();accountConfirm=false;};
    Item actionItem;std::string query,deleteSource,browserPath="sdmc:/",browserDelete,actionLocalPath,actionUsbDevice,clipboardPath,clipboardName;Uint32 usbSampled=0;
    std::vector<BrowserEntry> browserEntries=browse(browserPath);std::deque<Task> queue;std::optional<Task> activeJob;
    SDL_Thread *thread=nullptr,*controlThread=nullptr;Telemetry stats;Uint32 sampled=0;
    const std::string queuePath="sdmc:/switch/magictupper/queue.json";struct stat queueStat{};bool queueHealthy=true;
    if(stat(queuePath.c_str(),&queueStat)==0||stat((queuePath+".bak").c_str(),&queueStat)==0)queueHealthy=loadJobs(queue,queuePaused,queuePath);
    if(!queueHealthy)message("queue.json inválido. Corrígelo antes de añadir tareas.");
    auto persistQueue=[&](){if(!queueHealthy)return false;const bool ok=saveJobs(queue,activeJob?&*activeJob:nullptr,queuePaused,queuePath);if(!ok)message("No se pudo guardar la cola en la SD.");return ok;};
    auto launch=[&](Task* task){
        const bool transfer=transferTask(*task);
        if(transfer){cancelled=false;busy=true;activeJob=*task;{Guard guard;state.done=state.total=state.speed=0;state.taskMessage="Iniciando tarea...";state.activeTitle=task->item.title;state.activeDestination=taskLabel(*task);}thread=SDL_CreateThread(worker,"transfer",task);}
        else {controlBusy=true;controlThread=SDL_CreateThread(worker,"control",task);}
        if(transfer?!thread:!controlThread){delete task;if(transfer){busy=false;activeJob.reset();}else controlBusy=false;message("No se pudo iniciar la tarea.");return false;}
        return true;
    };
    auto enqueue=[&](const Task& task){if(!queueHealthy){message("La cola guardada necesita revisión.");return;}if(enqueueJob(queue,task,activeJob?&*activeJob:nullptr)){persistQueue();message("En cola: "+taskLabel(task)+" | "+task.item.title);}else message("La tarea ya está en cola o se alcanzó el límite de 50.");};
    auto commitSources=[&](const Sources& sources){
        if(!sourcesHealthy){message("Corrige sources.json en el PC antes de editar fuentes.");return false;}
        if(!writeSources(sources,sourcesPath)){message("No se pudieron guardar las fuentes en la SD.");return false;}
        Guard guard;state.sources=sources;return true;
    };
    auto resetCatalog=[&](){Guard guard;state.items.clear();state.serverBrowser.clear();state.serverBrowserPath.clear();state.users.clear();state.token.clear();state.role.clear();state.username.clear();state.name="MagicTupper";state.tagline="Tu alijo de otra dimensión";state.accent={181,255,57,255};state.url.clear();if(auto* source=state.sources.find(state.sources.active)){state.name=source->name;state.url=source->url;state.token=source->token;state.role=source->token.empty()?"":source->role;}};
    if(auto* source=state.sources.find(state.sources.active);source&&!source->token.empty()){Task task{0,source->url,source->token,"","",{},source->id};resetCatalog();launch(new Task(task));}
    InstalledTitles installed=readInstalledTitles(ncm);
    int updateFilter=0;std::set<std::string> markedUpdates;std::vector<Item> actionBatch;std::string markedSource;
    Uint32 repeatNext[4]={0,0,0,0};

    // Arranca MTUSB1 al final de la inicialización general. Esto evita que el
    // worker USB compita con la creación inicial de recursos/directorios.
    debug_log("[USB][STARTUP] General initialization complete; starting USB device");
    const bool usbStarted = magicUsbStart();
    SDL_Delay(100);
    debug_log("[USB][CHECK] start_ok=%d device=%d worker_thread=%p worker_entered=%d",
              usbStarted ? 1 : 0, magicUsbDevice ? 1 : 0,
              static_cast<void*>(magicUsbThread), magicUsbWorkerEntered.load() ? 1 : 0);

    while(appletMainLoop()){
        SDL_Event event;bool sdlExit=false;while(SDL_PollEvent(&event)){if(event.type==SDL_CONTROLLERBUTTONDOWN&&event.cbutton.button==SDL_CONTROLLER_BUTTON_START)sdlExit=true;}padUpdate(&pad);u64 keys=padGetButtonsDown(&pad),held=padGetButtons(&pad);const u64 repeatButtons[]={HidNpadButton_Up,HidNpadButton_Down,HidNpadButton_Left,HidNpadButton_Right};const Uint32 now=SDL_GetTicks();
        for(int i=0;i<4;i++){
            if(!(held&repeatButtons[i])){repeatNext[i]=0;continue;}
            if(keys&repeatButtons[i])repeatNext[i]=now+280;
            else if(repeatNext[i]&&now>=repeatNext[i]){keys|=repeatButtons[i];repeatNext[i]=now+85;}
        }
        if(sdlExit||(keys&HidNpadButton_Plus)){clearAccount();break;}
        if(thread&&!busy){SDL_WaitThread(thread,nullptr);thread=nullptr;installed=readInstalledTitles(ncm);activeJob.reset();{Guard guard;state.message=state.taskMessage;}persistQueue();}
        if(controlThread&&!controlBusy){SDL_WaitThread(controlThread,nullptr);controlThread=nullptr;if(browserOpen){browserEntries=browse(browserPath);browserSelected=0;}}
        State s;{Guard guard;s=state;}
        // Modal de decisión para transferencias iniciadas desde el PC.
        bool transferModal=false;
        {
            std::lock_guard<std::mutex> lock(usbIncomingMutex);
            transferModal=usbIncoming.pending&&!usbIncoming.decisionReady;
        }
        if(magicTcpApproval==1){
            if(keys&HidNpadButton_A)magicTcpApproval=2;
            else if(keys&HidNpadButton_B)magicTcpApproval=3;
            keys=0;
        }
        if(transferModal){
            bool choosing=false;int choice=0;
            {std::lock_guard<std::mutex> lock(usbIncomingMutex);choosing=usbIncoming.choosingPath;choice=usbIncoming.menuChoice;}
            if(!choosing){
                if(keys&HidNpadButton_Down)choice=(choice+1)%4;
                if(keys&HidNpadButton_Up)choice=(choice+3)%4;
                {std::lock_guard<std::mutex> lock(usbIncomingMutex);if(!pcInstallable(usbIncoming.name)&&choice<2)choice=(keys&HidNpadButton_Up)?3:2;}
                if(keys&HidNpadButton_B){std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.action=UsbTransferAction::Cancel;usbIncoming.decisionReady=true;usbIncomingCv.notify_all();}
                else if(keys&HidNpadButton_A){
                    std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.menuChoice=choice;
                    if(choice==0){usbIncoming.action=UsbTransferAction::InstallSd;usbIncoming.decisionReady=true;usbIncomingCv.notify_all();}
                    else if(choice==1){usbIncoming.action=UsbTransferAction::InstallNand;usbIncoming.decisionReady=true;usbIncomingCv.notify_all();}
                    else if(choice==2){usbIncoming.choosingPath=true;usbIncoming.directory="sdmc:/";usbIncomingBrowse=browse("sdmc:/");usbIncomingBrowseSelected=0;}
                    else {usbIncoming.action=UsbTransferAction::Cancel;usbIncoming.decisionReady=true;usbIncomingCv.notify_all();}
                }else{std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.menuChoice=choice;}
            }else{
                if(keys&HidNpadButton_Down)usbIncomingBrowseSelected++;
                if(keys&HidNpadButton_Up)usbIncomingBrowseSelected--;
                usbIncomingBrowseSelected=std::max(0,std::min(usbIncomingBrowseSelected,static_cast<int>(usbIncomingBrowse.size())-1));
                if(keys&HidNpadButton_A&&!usbIncomingBrowse.empty()&&usbIncomingBrowse[usbIncomingBrowseSelected].directory){
                    const std::string next=usbIncomingBrowse[usbIncomingBrowseSelected].path;{std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.directory=next;}usbIncomingBrowse=browse(next);usbIncomingBrowseSelected=0;
                }
                if(keys&HidNpadButton_B){
                    std::string current;{std::lock_guard<std::mutex> lock(usbIncomingMutex);current=usbIncoming.directory;}
                    if(current=="sdmc:/"){std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.choosingPath=false;}
                    else {const std::string parent=browserParent(current);{std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.directory=parent;}usbIncomingBrowse=browse(parent);usbIncomingBrowseSelected=0;}
                }
                if(keys&HidNpadButton_Y){std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.action=UsbTransferAction::CopySd;usbIncoming.decisionReady=true;usbIncoming.choosingPath=false;usbIncomingCv.notify_all();}
            }
            keys=0; // El modal consume la entrada y evita acciones en la UI principal.
        }
        {
            bool incomingActive=false;
            {std::lock_guard<std::mutex> lock(usbIncomingMutex);incomingActive=usbIncoming.active;}
            if(incomingActive&&(keys&HidNpadButton_B)){usbIncomingCancelRequested=true;std::lock_guard<std::mutex> lock(usbIncomingMutex);usbIncoming.phase=usbIncoming.processing?"Cancelando instalación...":"Cancelando transferencia...";keys=0;}
        }
        if(adminOpen&&(s.role!="admin"||s.token.empty())){adminOpen=false;clearAccount();}
        if((keys&HidNpadButton_StickR)&&actionChoice<0){queueOpen=!queueOpen;sourcesOpen=browserOpen=adminOpen=false;keys=0;}
        if((keys&HidNpadButton_StickL)&&actionChoice<0){
            clearAccount();newAdmin=false;
            if(s.role=="admin"&&!s.token.empty()){adminOpen=!adminOpen;sourcesOpen=browserOpen=queueOpen=false;if(adminOpen&&!controlThread)launch(new Task{5,s.url,s.token,"","",{},s.sources.active});}
            else message("Panel de usuarios: solo administradores.");
            keys=0;
        }
        if(browserOpen&&actionChoice<0&&(keys&(HidNpadButton_L|HidNpadButton_R))){category=(category+((keys&HidNpadButton_R)?1:categoryCount-1))%categoryCount;browserOpen=false;selected=0;keys=0;}
        if((keys&HidNpadButton_Y)&&!queueOpen&&!adminOpen&&actionChoice<0&&!browserOpen){browserOpen=false;sourcesOpen=!sourcesOpen;deleteSource.clear();keys=0;}
        if((keys&HidNpadButton_ZR)&&category!=3&&!sourcesOpen&&!browserOpen&&!queueOpen&&!adminOpen&&actionChoice<0){category=explorerCategory;browserOpen=true;browserPath="sdmc:/";browserEntries=browse(browserPath);browserSelected=0;keys=0;}
        if((keys&HidNpadButton_ZL)&&category!=3&&!sourcesOpen&&!browserOpen&&!queueOpen&&!adminOpen&&actionChoice<0){category=explorerCategory;browserOpen=true;browserPath="usb:/";browserEntries=browse(browserPath);browserSelected=0;message(usbReady?"USB: elige una unidad. FAT32 / exFAT. X actualiza.":"No se pudo iniciar USB. Abre MagicTupper en modo aplicación.");keys=0;}
        if(queueOpen){
            if(keys&HidNpadButton_B)queueOpen=false;
            if(keys&HidNpadButton_Down)queueSelected++;
            if(keys&HidNpadButton_Up)queueSelected--;
            queueSelected=std::max(0,std::min(queueSelected,static_cast<int>(queue.size())-1));
            if(keys&HidNpadButton_X){queuePaused=!queuePaused;persistQueue();}
            if(keys&HidNpadButton_R)cancelled=true;
            if((keys&HidNpadButton_Minus)&&!queue.empty()){queue.erase(queue.begin()+queueSelected);persistQueue();}
            queueSelected=std::max(0,std::min(queueSelected,static_cast<int>(queue.size())-1));
            if((keys&HidNpadButton_ZL)&&queueSelected>0){std::swap(queue[queueSelected],queue[queueSelected-1]);queueSelected--;persistQueue();}
            if((keys&HidNpadButton_ZR)&&queueSelected+1<static_cast<int>(queue.size())){std::swap(queue[queueSelected],queue[queueSelected+1]);queueSelected++;persistQueue();}
            keys=0;
        }else if(adminOpen){
            if(accountConfirm){
                if(keys&HidNpadButton_B)clearAccount();
                else if((keys&HidNpadButton_A)&&!controlThread){
                    auto* task=new Task{6,s.url,s.token,accountName,"",{},s.sources.active};
                    task->password.swap(accountPassword);task->role=newAdmin?"admin":"standard";
                    launch(task);clearAccount();newAdmin=false;
                }
                keys=0;
            }
            if(keys&HidNpadButton_B){adminOpen=false;clearAccount();}
            if(keys&HidNpadButton_Down)adminSelected++;
            if(keys&HidNpadButton_Up)adminSelected--;
            if(keys&HidNpadButton_L)newAdmin=!newAdmin;
            if((keys&HidNpadButton_R)&&!controlThread)launch(new Task{5,s.url,s.token,"","",{},s.sources.active});
            if((keys&HidNpadButton_X)&&!controlThread){
                clearAccount();std::string repeated;
                if(keyboard("Nuevo usuario",accountName)&&keyboard("Contrasena (minimo 6 caracteres)",accountPassword,true)&&keyboard("Repetir contrasena",repeated,true)){
                    if(accountPassword!=repeated){message("Las contrasenas no coinciden.");clearAccount();}
                    else if(accountPassword.size()<6){message("La contrasena necesita al menos 6 caracteres.");clearAccount();}
                    else accountConfirm=true;
                }else{
                    clearAccount();
                }
                std::fill(repeated.begin(),repeated.end(),0);
            }
            keys=0;
        }else if(browserOpen&&actionChoice<0){
            if(browserPath.rfind("pc:/",0)==0){
                if(keys&HidNpadButton_X){browserEntries=browse(browserPath);browserSelected=0;keys=0;}
                if(keys&HidNpadButton_Y){message("El PC es de solo lectura. Selecciona un archivo con A.");keys=0;}
            }
            if(usbInfoOpen){
                if(keys&HidNpadButton_B){usbInfoOpen=false;keys=0;}
                else if(keys&HidNpadButton_X){const auto ip=switchIp();message(ip.empty()?"RED: no se pudo obtener la IP. Comprueba la conexión WiFi y vuelve a intentarlo.":"RED: el servidor TCP escucha en "+ip+":8766. En el PC Client, escribe esta IP y pulsa Conectar Switch.");keys=0;}
                else if(keys&HidNpadButton_Y){
                    if(!magicUsbDevice){
                        debug_log("[USB][USER] Y pressed - initializing USB device");
                        magicUsbStart();
                    }
                    message(magicUsbDevice?"USB: solicitud enviada. Abre MagicTupper-PC-Client.exe.":"USB: no se pudo activar USB-device.");keys=0;
                }
            }
            if(!usbInfoOpen&&((keys&HidNpadButton_X)||(browserPath=="usb:/"&&SDL_GetTicks()-usbSampled>1000))){browserEntries=browse(browserPath);browserSelected=std::max(0,std::min(browserSelected,static_cast<int>(browserEntries.size())-1));usbSampled=SDL_GetTicks();}
            if(!usbInfoOpen&&(keys&HidNpadButton_X)&&!browserEntries.empty()&&browserPath!="explorer:/"){
                const auto& entry=browserEntries[browserSelected];if(entry.directory)message("Copiar: selecciona un archivo, no una carpeta.");else if(entry.path.rfind("server:/",0)==0)message("Fuente preparada: pega este archivo en SD o USB desde el catálogo.");else {clipboardPath=entry.path;clipboardName=entry.name;message("Copiado en memoria: "+clipboardName+". Navega al destino y pulsa Y para pegar.");}keys=0;
            }
            if(!usbInfoOpen&&(keys&HidNpadButton_Y)&&!clipboardPath.empty()&&browserPath!="explorer:/"){
                if(browserPath.rfind("server:/",0)==0)message("Pegar en una fuente no está permitido.");else {const std::string destination=browserPath+(browserPath.back()=='/'?"":"/")+clipboardName;if(destination==clipboardPath)message("El origen y el destino son iguales.");else if(explorerCopyRunning)message("Ya hay una copia en curso.");else {cancelled=false;explorerCopyRunning=true;explorerCopyThread=SDL_CreateThread(explorerCopyWorker,"explorer-copy",new ExplorerCopyJob{clipboardPath,destination});if(!explorerCopyThread){explorerCopyRunning=false;message("No se pudo iniciar la copia.");}}}keys=0;
            }
            if((keys&HidNpadButton_ZR)&&(browserPath.rfind("sdmc:/",0)!=0||s.role!="admin"||s.token.empty()||controlThread||busy)){
                message(s.role!="admin"?"Modo estándar: la creación de carpetas requiere administrador.":"Espera a que termine la tarea antes de modificar archivos.");keys=0;
            }
            if((keys&HidNpadButton_Minus)&&(browserPath.rfind("sdmc:/",0)!=0||controlThread||busy)){
                message(browserPath.rfind("sdmc:/",0)!=0?"Borrar está disponible para archivos de la SD.":"Espera a que termine la tarea antes de borrar archivos.");keys=0;
            }
            if((keys&HidNpadButton_X)&&browserPath.rfind("server:/",0)==0&&!controlThread){Guard guard;state.serverBrowserPath.clear();}
            if(keys&HidNpadButton_ZR){
                std::string name;
                if(keyboard("Nombre de la nueva carpeta",name)&&name!="."&&name!=".."&&name.find_first_of("/\\:")==std::string::npos){
                    launch(new Task{8,s.url,s.token,"","",{},s.sources.active,browserPath+"/"+name});
                }
                keys=0;
            }
            if(keys&HidNpadButton_Minus&&!browserEntries.empty()){
                const auto& entry=browserEntries[browserSelected];
                if(entry.directory)message("Solo se pueden eliminar archivos desde aquí.");
                else if(browserDelete!=entry.path){browserDelete=entry.path;message("Pulsa - otra vez para eliminar "+entry.name);}
                else {browserDelete.clear();launch(new Task{7,s.url,s.token,"","",{},s.sources.active,entry.path});}
                keys=0;
            }
            if(keys&HidNpadButton_Minus&&explorerCopyRunning){cancelled=true;transferMessage("Cancelando copia...");keys=0;}
            if(keys&HidNpadButton_B){
                if(browserPath=="explorer:/"){category=0;browserOpen=false;}
                else {browserPath=browserParent(browserPath);browserEntries=browse(browserPath);browserSelected=0;}
                keys=0;
            }
            if((keys&HidNpadButton_Down)||(browserPath=="explorer:/"&&(keys&HidNpadButton_Right))){browserSelected++;keys=0;}if((keys&HidNpadButton_Up)||(browserPath=="explorer:/"&&(keys&HidNpadButton_Left))){browserSelected--;keys=0;}
            browserSelected=std::max(0,std::min(browserSelected,static_cast<int>(browserEntries.size())-1));
            if(keys&HidNpadButton_A&&!browserEntries.empty()){
                const auto entry=browserEntries[browserSelected];
                if(entry.path=="pc-host:/"){
                    browserPath="pc:/";browserEntries=browse(browserPath);browserSelected=0;message("Cargando C: desde el cliente PC...");
                    keys=0;
                }else if(browserPath.rfind("pc:/",0)==0){
                    if(!entry.itemId.empty()){pcExplorerAsk(browserPath,"list",std::stoi(entry.itemId));browserEntries.clear();browserSelected=0;}
                    else if(entry.directory){browserPath=entry.path;browserEntries=browse(browserPath);browserSelected=0;}
                    else {pcExplorerAsk(entry.path,"send");message("Solicitando archivo al PC...");}
                    keys=0;
                }else if(entry.path=="mount-xci:/"){
                    message("Montar XCI: función prevista para una próxima versión. Aún no está implementada.");
                    keys=0;
                }else if(entry.path=="pc-connect:/"){
                    usbInfoOpen=true;
                    if(!magicUsbDevice){
                        debug_log("[USB][USER] pc-connect selected - initializing USB device");
                        magicUsbStart();
                    }
                    if(!magicUsbDevice){message("USB-device no disponible. Comprueba Atmosphere y el cable.");}
                    message(magicUsbDevice?"USB-device MagicTupper enumerado. Aún no se exponen archivos: falta el protocolo y el cliente Windows.":"REQUISITOS PC: cliente MagicTupper y driver libusbK/WinUSB. La SD no se expone todavía.");
                }
                else if(entry.path.rfind("source:/",0)==0){
                    const std::string sourceId=entry.path.substr(8);auto* selectedSource=state.sources.find(sourceId);
                    if(selectedSource){Sources changed;{Guard guard;changed=state.sources;changed.active=sourceId;}if(writeSources(changed,sourcesPath)){resetCatalog();browserPath="server:/";browserEntries=browse(browserPath);browserSelected=0;State active;{Guard guard;active=state;}if(active.token.empty())message("Fuente seleccionada: "+selectedSource->name+". Pulsa Y para iniciar sesión.");else launch(new Task{0,active.url,active.token,"","",{},sourceId});}}
                }
                else if(entry.directory){browserPath=entry.path;browserEntries=browse(browserPath);browserSelected=0;if(browserPath=="server:/"&&s.token.empty())message("Pulsa Y para acceder a una fuente del servidor.");}
                else if(browserPath.rfind("server:/",0)==0){
                    if(!s.token.empty()){
                        Item item{entry.itemId,entry.name,entry.name,"browser",entry.size,entry.path.substr(8)};
                        auto ext=entry.name.substr(entry.name.find_last_of('.')==std::string::npos?entry.name.size():entry.name.find_last_of('.'));for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if(ext==".nsp"||ext==".xci"){actionItem=item;actionLocalPath.clear();actionUsbDevice.clear();actionChoice=0;}
                        else enqueue(Task{2,s.url,"","","",item,s.sources.active});
                    }else message("Accede al servidor y actualiza esta carpeta.");
                }
                else if(browserPath.rfind("sdmc:/",0)==0){
                    const auto dot=entry.name.find_last_of('.');auto ext=dot==std::string::npos?"":entry.name.substr(dot);for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if(ext==".nsp"||ext==".xci"){
                        actionItem={"",entry.name,entry.name,"sd",entry.size};actionLocalPath=entry.path;actionUsbDevice.clear();actionChoice=1;
                        message("Paquete local: elige instalar en SD o memoria interna.");
                    }else message("Archivo: "+entry.name+" · "+bytes(entry.size));
                }
                else if(browserPath.rfind("ums",0)==0){
                    const auto device=usbIdentity(entry.path);
                    if(device.empty())message("USB desconectado. Pulsa X para actualizar.");
                    else {
                        actionItem={"",entry.name,entry.name,"usb",entry.size};actionLocalPath=entry.path;actionUsbDevice=device;
                        const auto dot=entry.name.find_last_of('.');auto ext=dot==std::string::npos?"":entry.name.substr(dot);for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if(ext==".nsp"||ext==".xci"){actionChoice=0;}
                        else {Task task{4,"","","","",actionItem,"",entry.path};task.usbDevice=device;enqueue(task);}
                    }
                } else message("Archivo: "+entry.name+" · "+bytes(entry.size));
                keys=0;
            }
        }else if(sourcesOpen){
            if(keys&HidNpadButton_B){if(!deleteSource.empty())deleteSource.clear();else sourcesOpen=false;keys=0;}
            if((keys&(HidNpadButton_X|HidNpadButton_ZR|HidNpadButton_Minus))&&!s.token.empty()&&s.role!="admin"){message("Solo un administrador puede modificar o eliminar fuentes.");keys=0;}
            if(controlThread&&(keys&(HidNpadButton_A|HidNpadButton_X|HidNpadButton_ZR|HidNpadButton_Minus)))message("Espera a que termine la petición al servidor.");
            if(!controlThread){
                if(keys&HidNpadButton_Down){sourceSelected++;deleteSource.clear();}if(keys&HidNpadButton_Up){sourceSelected--;deleteSource.clear();}
                sourceSelected=std::max(0,std::min(sourceSelected,static_cast<int>(s.sources.entries.size())-1));
                const bool hasSource=!s.sources.entries.empty();
                if((keys&HidNpadButton_R)&&hasSource&&!s.sources.entries[sourceSelected].token.empty()){
                    const auto& source=s.sources.entries[sourceSelected];launch(new Task{9,source.url,source.token,"","",{},source.id});keys=0;
                }
                if((keys&HidNpadButton_Minus)&&hasSource)deleteSource=s.sources.entries[sourceSelected].id;
                if((keys&HidNpadButton_A)&&!deleteSource.empty()){
                    Sources edited=s.sources;const auto id=deleteSource;
                    edited.entries.erase(std::remove_if(edited.entries.begin(),edited.entries.end(),[&](const Source& source){return source.id==id;}),edited.entries.end());
                    if(edited.active==id)edited.active=edited.entries.empty()?"":edited.entries.front().id;
                    if(commitSources(edited)){queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const Task& task){return task.sourceId==id;}),queue.end());persistQueue();resetCatalog();message("Fuente eliminada. Sus archivos descargados se conservan.");}
                    deleteSource.clear();
                }else if((keys&HidNpadButton_A)&&hasSource){
                    Sources edited=s.sources;auto& source=edited.entries[sourceSelected];std::string password;
                    bool ready=!source.token.empty();
                    if(!ready)ready=keyboard("Usuario de esta fuente",source.user)&&keyboard("Contraseña de esta fuente",password,true);
                    if(ready){edited.active=source.id;if(commitSources(edited)){resetCatalog();category=selected=0;query.clear();sourcesOpen=false;launch(new Task{source.token.empty()?1:0,source.url,source.token,source.user,password,{},source.id});}}
                    else message("Acceso cancelado.");
                    std::fill(password.begin(),password.end(),0);
                }else if((keys&HidNpadButton_X)||((keys&HidNpadButton_ZR)&&hasSource)){
                    deleteSource.clear();const bool adding=(keys&HidNpadButton_X)!=0;Sources edited=s.sources;
                    if(adding&&edited.entries.size()>=20)message("Puedes guardar hasta 20 fuentes.");
                    else{
                        Source source=adding?Source{"","","http://192.168.1.100:8765","",""}:edited.entries[sourceSelected];
                        const auto oldId=source.id,oldUrl=source.url,oldUser=source.user;
                        if(keyboard("Nombre de la fuente",source.name)&&keyboard("Servidor: https://dominio o http://IP:puerto",source.url)&&keyboard("Usuario de esta fuente",source.user)){
                            if(!sourceUrl(source.url))message("Direccion invalida. Usa https://dominio o http://IP:puerto, sin rutas ni credenciales.");
                            else{
                                bool duplicate=false;for(const auto& existing:edited.entries)if(existing.id!=oldId&&existing.url==source.url)duplicate=true;
                                if(duplicate)message("Ese servidor ya está guardado. Edita su fuente existente.");
                                else{
                                    if(adding||source.url!=oldUrl){source.id=edited.allocateId();source.token.clear();source.role.clear();}else if(source.user!=oldUser)source.token.clear();source.role.clear();
                                    if(adding)edited.entries.push_back(source);else edited.entries[sourceSelected]=source;
                                    if(edited.active.empty()||edited.active==oldId)edited.active=source.id;
                                    if(commitSources(edited)){
                                        if(!adding&&(source.url!=oldUrl||source.user!=oldUser)){queue.erase(std::remove_if(queue.begin(),queue.end(),[&](const Task& task){return task.sourceId==oldId;}),queue.end());persistQueue();}
                                        if(s.sources.active!=edited.active||s.sources.active==oldId)resetCatalog();
                                        sourceSelected=adding?static_cast<int>(edited.entries.size())-1:sourceSelected;
                                        if(sourceSelected<0)sourceSelected=0;
                                        if(sourceSelected>=static_cast<int>(edited.entries.size()))sourceSelected=std::max(0,static_cast<int>(edited.entries.size())-1);
                                        message("Fuente guardada. Pulsa A para abrir su portal.");
                                    }
                                }
                            }
                        }else message("Edición cancelada.");
                    }
                }
            }
            keys=0;
        }else{
            if(category==3&&(keys&HidNpadButton_ZL)&&actionChoice<0){updateFilter=1-updateFilter;markedUpdates.clear();selected=0;keys=0;}
            if((keys&HidNpadButton_B)&&actionChoice<0)cancelled=true;
            if((keys&HidNpadButton_Minus)&&!controlThread&&!s.token.empty()&&actionChoice<0){if(!busy)installed=readInstalledTitles(ncm);launch(new Task{0,s.url,s.token,"","",{},s.sources.active});}
            if((keys&HidNpadButton_X)&&actionChoice<0){keyboard("Buscar (un espacio para mostrar todo)",query);if(query==" ")query.clear();selected=0;}
            if(keys&HidNpadButton_R){category=(category+1)%categoryCount;selected=0;}if(keys&HidNpadButton_L){category=(category+categoryCount-1)%categoryCount;selected=0;}
        }
        if(category==explorerCategory&&!sourcesOpen&&!queueOpen&&!adminOpen&&!browserOpen&&actionChoice<0){browserOpen=true;browserPath="explorer:/";browserEntries=browse(browserPath);browserSelected=0;keys=0;}
        {Guard guard;s=state;}
        {
            std::string reply;bool timedOut=false;
            {std::lock_guard<std::mutex> lock(pcExplorerMutex);
                reply.swap(pcExplorerResponse);
                if(!reply.empty())pcExplorerStarted=0;
                if(pcExplorerStarted&&SDL_GetTicks()-pcExplorerStarted>15000){pcExplorerStarted=0;pcExplorerRequest.clear();timedOut=true;}
            }
            if(timedOut)message("PC sin respuesta. Conecta el cliente actualizado por USB o RED y pulsa X.");
            if(!reply.empty()){
                auto* obj=json_tokener_parse(reply.c_str());json_object *ok=nullptr,*data=nullptr,*entries=nullptr;
                if(!obj||!json_object_object_get_ex(obj,"ok",&ok)||!json_object_get_boolean(ok))message("PC: "+pcExplorerField(obj,"error"));
                else if(browserOpen&&browserPath.rfind("pc:/",0)==0&&json_object_object_get_ex(obj,"data",&data)&&json_object_object_get_ex(data,"entries",&entries)){
                    browserEntries.clear();json_object *offset=nullptr,*next=nullptr;
                    if(json_object_object_get_ex(data,"offset",&offset)&&json_object_get_int(offset)>0)browserEntries.push_back({"< Pagina anterior",browserPath,false,0,std::to_string(std::max(0,json_object_get_int(offset)-128))});
                    for(size_t i=0;i<json_object_array_length(entries);++i){auto* e=json_object_array_get_idx(entries,i);json_object *dir=nullptr,*size=nullptr;
                        json_object_object_get_ex(e,"directory",&dir);json_object_object_get_ex(e,"size",&size);
                        browserEntries.push_back({pcExplorerField(e,"name"),pcExplorerField(e,"path"),bool(json_object_get_boolean(dir)),json_object_get_int64(size),""});
                    }
                    if(json_object_object_get_ex(data,"next",&next)&&json_object_get_int(next)>=0)browserEntries.push_back({"Pagina siguiente >",browserPath,false,0,std::to_string(json_object_get_int(next))});
                    browserSelected=0;message(browserEntries.empty()?"Carpeta vacia.":"PC conectado / C:");
                }
                if(obj)json_object_put(obj);
            }
        }
        if(browserOpen&&browserPath.rfind("server:/",0)==0&&!s.token.empty()&&!controlThread&&s.serverBrowserPath!=browserPath){
            message("Abriendo carpeta del servidor...");launch(new Task{11,s.url,s.token,"","",{},s.sources.active,browserPath});
        }
        std::vector<Item> shown;for(const auto& item:s.items)if((category==0||item.category==categories[category])&&(category!=3||updateVisible(item,installed,updateFilter))&&(query.empty()||item.title.find(query)!=std::string::npos))shown.push_back(item);
        if(category==3)std::stable_sort(shown.begin(),shown.end(),[&](const Item& a,const Item& b){auto rank=[&](const Item& i){int status=updateStatus(i,installed);return status==2?3:status==1?2:status==3?1:0;};return rank(a)>rank(b);});
        if(markedSource!=s.sources.active||category!=3){markedUpdates.clear();markedSource=s.sources.active;}
        for(auto it=markedUpdates.begin();it!=markedUpdates.end();){if(std::none_of(shown.begin(),shown.end(),[&](const Item& item){return item.id==*it;}))it=markedUpdates.erase(it);else ++it;}
        if(actionChoice>=0){
            const bool actionFromSd=actionLocalPath.rfind("sdmc:/",0)==0;
            if(keys&HidNpadButton_B){actionChoice=-1;actionBatch.clear();}
            else if(keys&HidNpadButton_Left)actionChoice=actionFromSd?(actionChoice==1?2:1):(actionChoice+2)%3;
            else if(keys&HidNpadButton_Right)actionChoice=actionFromSd?(actionChoice==2?1:2):(actionChoice+1)%3;
            else if(keys&HidNpadButton_A){
                if(!actionBatch.empty()){
                    std::vector<Task> tasks;for(const auto& item:actionBatch){Task task{actionChoice==0?2:3,s.url,"","","",item,s.sources.active};task.destination=actionChoice==2?4:5;tasks.push_back(task);}
                    auto previous=queue;
                    if(queueHealthy&&enqueueUpdateBatch(queue,tasks,activeJob?&*activeJob:nullptr)){
                        if(persistQueue()){message(std::to_string(tasks.size())+" updates añadidos a la cola.");markedUpdates.clear();}
                        else queue=std::move(previous);
                    }else message("No se añadió el lote: cola llena, tarea duplicada o cola pendiente de revisión.");
                    actionBatch.clear();
                }else{Task task{actionLocalPath.empty()?(actionChoice==0?2:3):(actionChoice==0?4:10),s.url,"","","",actionItem,actionLocalPath.empty()?s.sources.active:"",actionLocalPath};task.usbDevice=actionUsbDevice;task.destination=actionChoice==2?4:5;enqueue(task);}
                actionChoice=-1;
            }
            keys=0;
        }
        if(keys&HidNpadButton_Down)selected++;
        if(keys&HidNpadButton_Up)selected--;
        selected=std::max(0,std::min(selected,static_cast<int>(shown.size())-1));
        if(category==3&&actionChoice<0&&(keys&HidNpadButton_ZR)&&!shown.empty()){const auto& id=shown[selected].id;if(!markedUpdates.erase(id))markedUpdates.insert(id);keys=0;}
        if(category==3&&actionChoice<0&&(keys&HidNpadButton_A)&&!markedUpdates.empty()&&!s.token.empty()){
            actionBatch.clear();for(const auto& item:shown)if(markedUpdates.count(item.id))actionBatch.push_back(item);
            actionItem={};actionItem.title=std::to_string(actionBatch.size())+" updates seleccionados";actionLocalPath.clear();actionUsbDevice.clear();actionChoice=0;keys=0;
        }
        if(actionChoice<0&&(keys&HidNpadButton_A)&&!shown.empty()&&!s.token.empty()){
            const auto item=shown[selected];
            const auto dot=item.filename.find_last_of('.');auto ext=dot==std::string::npos?"":item.filename.substr(dot);for(char& c:ext)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if(ext==".nsp"||ext==".nsz"||ext==".xci"||ext==".xcz"){actionLocalPath.clear();actionUsbDevice.clear();actionItem=item;actionChoice=0;message("Elige Descargar o Instalar · izquierda/derecha · A confirmar · B cancelar");}
            else enqueue(Task{item.category=="apps"&&ext==".nro"?12:2,s.url,"","","",item,s.sources.active});
        }
        if(queueHealthy&&!queuePaused&&!busy&&!thread&&!controlThread&&!queue.empty()){
            const auto pending=std::find_if(queue.begin(),queue.end(),[&](const Task& task){if(localTask(task))return true;auto* source=s.sources.find(task.sourceId);return source&&!source->token.empty();});
            if(pending!=queue.end()){auto task=*pending;queue.erase(pending);auto* source=s.sources.find(task.sourceId);if(source){task.token=source->token;task.url=source->url;}if(!launch(new Task(task)))queue.push_front(task);persistQueue();}
        }
        if(SDL_GetTicks()-sampled>2000||sampled==0){stats=telemetry(psm,tc,ncm);sampled=SDL_GetTicks();}
        SDL_Color white{235,241,233,255},muted{143,160,158,255},panel{31,33,35,255};
        background();rect(0,0,1280,54,panel);
        text(stats.battery,24,17,s.accent,smallFont);text(stats.temp,184,17,white,smallFont);text(stats.ip,344,17,white,smallFont,190);text(stats.sd,540,17,white,smallFont,390);text(stats.internal,935,17,white,smallFont,325);
        text(s.name,30,75,white,bigFont,890);text(s.tagline,32,128,muted,font,900);
        text(magicTcpConnected?"PC / RED CONECTADA":s.token.empty()?"PORTAL SIN SESIÓN":"SESIÓN GUARDADA",960,84,{92,210,227,255},smallFont,290);
        if(!s.username.empty())text(s.username+" / "+(s.role=="admin"?"ADMIN":"ESTÁNDAR"),960,146,s.accent,smallFont,290);
        if(auto* source=s.sources.find(s.sources.active))text(source->name,960,117,white,smallFont,290);
        rect(30,179,1220,44,{14,24,29,255});
        for(int i=0;i<categoryCount;i++){int x=30+i*203;rect(x+2,181,195,40,i==category?SDL_Color{38,59,40,255}:SDL_Color{14,24,29,255});if(i==category)rect(x+12,218,171,3,s.accent);text(labels[i],x+16,189,i==category?s.accent:muted,font,178);}
        text(category==3?std::string(updateFilter?"UPDATES / PARA MIS JUEGOS":"UPDATES / TODOS")+" / "+std::to_string(markedUpdates.size())+" seleccionados":query.empty()?"BIBLIOTECA / "+std::to_string(shown.size())+" ARCHIVOS":"BUSCAR / "+query,32,244,muted,smallFont,870);
        if(shown.empty()){text("Este portal está vacío.",45,324,white,bigFont,810);text(s.token.empty()?"Pulsa Y para conectar con tu PC.":"Añade archivos a tu carpeta y pulsa - para actualizar.",46,383,muted,font,835);}
        int first=selected/5*5;
        for(int i=first;i<std::min(first+5,static_cast<int>(shown.size()));i++){
            int y=280+(i-first)*61;rect(30,y,890,54,i==selected?SDL_Color{43,59,42,255}:panel);if(i==selected)rect(30,y,4,54,s.accent);
            const int status=updateStatus(shown[i],installed);if(status==1||status==2)rect(30,y,4,54,updateMissing(shown[i],installed)?SDL_Color{255,100,100,255}:SDL_Color{100,198,255,255});
            text((category==3?(markedUpdates.count(shown[i].id)?"[x] ":"[ ] "):"")+shown[i].title,48,y+5,i==selected?white:SDL_Color{207,220,219,255},font,690);
            const auto badge=updateBadge(shown[i],installed);text(badge.empty()?(shown[i].category=="games"?"ROMS":shown[i].category=="apps"?"APPS":shown[i].category=="dlc"?"DLC":"UPDATES"):badge,48,y+32,updateMissing(shown[i],installed)?SDL_Color{255,100,100,255}:status==3?SDL_Color{111,230,135,255}:muted,smallFont,650);text(bytes(shown[i].size),765,y+17,s.accent,smallFont,147);
        }
        mascot(s.accent);text(category==3?"TUS ACTUALIZACIONES":"PORTAL SWITCH",970,435,s.accent,smallFont,275);text(category==3?std::to_string(std::count_if(shown.begin(),shown.end(),[&](const Item& i){return updateMissing(i,installed);}))+" pendientes / "+std::to_string(installed.games.size())+" juegos":"ROMS · DLC · UPDATES · APPS",970,466,muted,smallFont,275);
        text("COLA  "+std::to_string(queue.size())+(queuePaused?" / PAUSA":""),976,501,white,font,290);text(busy?s.activeDestination:"R3 Cola / L3 Admin",976,538,muted,smallFont,290);
        rect(30,598,1220,6,panel);if(s.total>0)rect(30,598,static_cast<int>(1220*std::min(1.,s.done/s.total)),6,s.accent);
        text(s.message,30,614,white,smallFont,925);
        text(busy?bytes(s.speed)+"/s | "+s.taskMessage:s.joke,30,645,muted,smallFont,940);
        rect(960,560,290,34,busy?SDL_Color{103,55,42,255}:panel);
        text(busy?"B  CANCELAR TAREA":"A  ELEGIR ACCIÓN",974,568,busy?SDL_Color{255,220,190,255}:s.accent,smallFont,270);
        if(s.total>0)text(std::to_string(static_cast<int>(100*std::min(1.,s.done/s.total)))+"% · "+bytes(s.done),991,645,s.accent,smallFont,270);
        rect(0,680,1280,40,panel);text(category==3?"ZL Todos/Mis juegos  ZR Marcar  A Acción/lote  L/R Categoría  X Buscar  R3 Cola  + Salir":"L/R Categoría  X Buscar  Y Fuentes  R3 Cola  L3 Admin  - Actualizar  + Salir",30,690,white,smallFont,1220);
        if(sourcesOpen){
            rect(0,64,1280,540,{10,17,22,255});text("FUENTES / TUS PORTALES",30,82,white,bigFont,1220);
            text("Elige qué servidor alimenta tu biblioteca.",32,144,muted,font,1200);
            if(s.sources.entries.empty())text("No hay fuentes. Pulsa X para añadir tu primer servidor.",32,260,white,font,1200);
            sourceSelected=std::max(0,std::min(sourceSelected,static_cast<int>(s.sources.entries.size())-1));
            const int firstSource=sourceSelected/5*5;
            for(int i=firstSource;i<std::min(firstSource+5,static_cast<int>(s.sources.entries.size()));i++){
                const auto& source=s.sources.entries[i];int y=201+(i-firstSource)*71;
                rect(30,y,1220,63,i==sourceSelected?SDL_Color{43,59,42,255}:panel);if(i==sourceSelected)rect(30,y,4,63,s.accent);
                text(source.name,49,y+6,white,font,690);text(source.url,49,y+36,muted,smallFont,835);
                text(source.id==s.sources.active?"ACTIVA":"",950,y+7,s.accent,smallFont,270);
                text(source.token.empty()?"Sin sesión":"Sesión guardada",950,y+35,white,smallFont,270);
            }
            if(!deleteSource.empty())text("¿Eliminar la fuente seleccionada? A confirma · B cancela",32,568,{255,181,110,255},font,1220);
            else text(busy?"Transferencia en curso. Puedes volver con B o Y.":"Fuentes "+std::to_string(s.sources.entries.empty()?0:firstSource+1)+"-"+std::to_string(std::min(firstSource+5,static_cast<int>(s.sources.entries.size())))+" de "+std::to_string(s.sources.entries.size())+" · X añade · ZR edita",32,568,muted,smallFont,1220);
            rect(0,680,1280,40,panel);text("A Acceder  X Añadir  ZR Editar  - Eliminar  R Cerrar sesión  B/Y Volver",30,690,white,smallFont,1220);
        }
        if(browserOpen){
            rect(0,232,1280,362,{8,16,22,255});
            text(browserHeading(browserPath),32,242,white,font,1220);
            
            if(browserPath=="explorer:/")text("A abrir · ↑/↓ elegir · L/R cambiar pestaña",32,277,muted,smallFont,1180);
            else text("A abrir / acciones · X actualizar · B subir · L/R pestaña",32,277,muted,smallFont,1180);
            if(browserEntries.empty())text(browserPath=="usb:/"?(usbReady?"USB iniciado, pero no hay particiones montadas.":"USB no iniciado. Abre el NRO en modo aplicación."):browserPath.rfind("server:/",0)==0&&s.token.empty()?"Servidor sin sesión. Pulsa Y para acceder.":"Carpeta vacía o no accesible.",45,330,white,font,1190);
            if(browserPath=="usb:/"){
                char diagnostic[160];snprintf(diagnostic,sizeof(diagnostic),"USB init: 0x%08X / particiones: %zu / modo: %s",usbInitResult,browserEntries.size(),appletGetAppletType()==AppletType_Application?"aplicación":"applet");
                text(diagnostic,32,575,muted,smallFont,1210);
#ifdef MAGICTUPPER_USB_DIAGNOSTICS
                if(browserEntries.empty()){text("DIAGNÓSTICO USB / registro: /libusbhsfs.log",45,375,s.accent,font,1180);text("Conecta el USB, espera 15 s y sal con + para guardar el registro.",45,414,white,smallFont,1180);}
#endif
            }
            if(browserPath.rfind("server:/",0)==0&&browserEntries.empty()){
                rect(30,310,1220,70,panel);
                text(s.token.empty()?"Pulsa Y para acceder al servidor.":s.serverBrowserPath!=browserPath?"Cargando carpeta del servidor...":!s.serverBrowserError.empty()?s.serverBrowserError:"Carpeta vacía.",45,330,white,font,1190);
            }
            if(browserPath=="explorer:/"){
                text("Selecciona un origen",45,300,white,font,1180);
                const int firstRoot=(browserSelected/6)*6;
                for(int i=firstRoot;i<std::min(firstRoot+6,static_cast<int>(browserEntries.size()));i++){const auto& item=browserEntries[i];const int y=330+(i-firstRoot)*48;const bool chosen=i==browserSelected;rect(30,y,1220,40,chosen?SDL_Color{34,53,43,255}:panel);if(chosen)rect(30,y,4,40,s.accent);text(item.name,55,y+8,white,font,720);text(i<2?(i==0?"Archivos locales":"FAT32 / exFAT"):item.path=="pc-host:/"?"Cliente Windows":item.path=="mount-xci:/"?"Próxima función":item.path=="pc-connect:/"?"Requisitos Windows":s.token.empty()?"Sin sesión":"Fuente disponible",900,y+11,chosen?s.accent:muted,smallFont,310);}
            }
            const int firstBrowser=browserSelected/5*5;
            for(int i=firstBrowser;browserPath!="explorer:/"&&i<std::min(firstBrowser+5,static_cast<int>(browserEntries.size()));i++){
                const auto& entry=browserEntries[i];int y=310+(i-firstBrowser)*54;
                rect(30,y,1220,48,i==browserSelected?SDL_Color{43,59,42,255}:panel);if(i==browserSelected)rect(30,y,4,48,s.accent);
                if(entry.directory){rect(49,y+14,16,6,s.accent);rect(49,y+19,25,17,s.accent);}else rect(53,y+14,16,23,muted);
                text(entry.name,87,y+7,white,font,865);
                text(entry.directory?"Carpeta":bytes(entry.size),990,y+14,entry.directory?s.accent:muted,smallFont,230);
            }
            rect(0,680,1280,40,panel);text(browserPath.rfind("pc:/",0)==0?"A Abrir / copiar / instalar   X Actualizar   B Subir   L/R Pestaña":explorerCopyRunning?"A Abrir   X Copiar   Y Pegar   - Cancelar copia   B Subir":browserPath.rfind("sdmc:/",0)==0?(s.role=="admin"?"A Abrir/instalar   X Copiar   Y Pegar   ZR Crear carpeta   - Borrar   B Subir":"A Abrir/instalar   X Copiar   Y Pegar   - Borrar   B Subir"):"A Abrir / acciones   X Copiar   Y Pegar   B Subir   L/R Pestaña",30,690,white,smallFont,1220);
            if(usbInfoOpen){
                rect(120,150,1040,410,{7,15,20,250});rect(120,150,1040,5,s.accent);
                text("CONECTAR SD AL PC",180,185,white,bigFont,900);
                text("Requisitos de Windows · USB",180,235,s.accent,font,800);
                text("1. MagicTupper-PC-Client.exe",180,275,white,smallFont,850);
                text("2. Zadig para asociar el dispositivo",180,305,white,smallFont,850);
                text("3. Driver libusbK / WinUSB",180,335,white,smallFont,850);
                text("4. Cable USB de datos y MagicTupper abierto",180,365,white,smallFont,850);
                text("5. El EXE portable no requiere instalar .NET",180,395,white,smallFont,850);
                const auto ipNow=switchIp();
                text("RED: "+std::string(ipNow.empty()?"IP N/D":ipNow)+" · puerto 8766",180,440,s.accent,smallFont,850);
                text("REMOTE PAD: sysmodule Atmosphere · UDP 8767",180,470,s.accent,smallFont,850);
                text("A no hacer nada   B cerrar   X RED   Y USB",180,515,muted,smallFont,850);
            }
        }
        if(queueOpen){
            rect(0,64,1280,526,{8,16,22,255});text("COLA DE DESCARGAS E INSTALACIONES",30,82,white,bigFont,1220);
            text(queuePaused?"Pausada: la tarea activa continúa; las siguientes esperan.":"En segundo plano: puedes seguir navegando por MagicTupper.",32,140,s.accent,smallFont,1220);
            text(busy?"ACTIVA: "+s.activeTitle:"Sin tarea activa",32,175,white,font,1180);
            const int firstQueue=queueSelected/5*5;
            for(int i=firstQueue;i<std::min(firstQueue+5,static_cast<int>(queue.size()));i++){
                int y=220+(i-firstQueue)*49;rect(30,y,1220,44,i==queueSelected?SDL_Color{43,59,42,255}:panel);
                text(std::to_string(i+1)+". "+queue[i].item.title,45,y+8,white,smallFont,810);text(taskLabel(queue[i]),895,y+8,s.accent,smallFont,340);
            }
            if(queue.empty())text("No hay tareas pendientes. Añádelas desde el catálogo.",32,245,muted,font,1180);
            for(size_t i=0;i<std::min<size_t>(3,s.history.size());i++)text(s.history[i],32,489+i*28,muted,smallFont,1210);
            rect(0,680,1280,40,panel);text("X Pausar cola  R Cancelar activa  - Quitar pendiente  ZL/ZR Reordenar  B/R3 Volver",30,690,white,smallFont,1220);
        }
        if(adminOpen){
            rect(0,64,1280,526,{8,16,22,255});text("ADMINISTRACIÓN / USUARIOS",30,82,white,bigFont,1220);
            text("NUEVA CUENTA / "+std::string(newAdmin?"ADMINISTRADOR":"ESTANDAR"),32,145,s.accent,smallFont,1220);
            adminSelected=std::max(0,std::min(adminSelected,static_cast<int>(s.users.size())-1));const int firstUser=adminSelected/6*6;
            for(int i=firstUser;i<std::min(firstUser+6,static_cast<int>(s.users.size()));i++){
                int y=202+(i-firstUser)*53;rect(30,y,1220,46,i==adminSelected?SDL_Color{43,59,42,255}:panel);
                text(s.users[i].username,48,y+9,white,font,720);text(s.users[i].role=="admin"?"ADMIN":"ESTÁNDAR",780,y+12,s.accent,smallFont,225);text(s.users[i].enabled?"Activo":"Desactivado",1020,y+12,muted,smallFont,200);
            }
            text("Estándar: descargar e instalar. Admin: también gestiona usuarios y archivos.",32,553,muted,smallFont,1210);
            rect(0,680,1280,40,panel);text("X Crear usuario  L Rol del alta  R Actualizar  ↑/↓ Elegir  B/L3 Volver",30,690,white,smallFont,1220);
            if(accountConfirm){
                rect(180,210,920,310,{7,15,20,255});rect(180,210,920,5,s.accent);
                text("CONFIRMAR NUEVA CUENTA",215,240,white,bigFont,850);
                text(accountName,215,305,white,font,850);
                text(newAdmin?"ROL: ADMINISTRADOR":"ROL: ESTANDAR",215,355,newAdmin?SDL_Color{255,181,100,255}:s.accent,font,850);
                text("A Crear cuenta                 B Cancelar",215,440,white,font,850);
            }
        }
        if(actionChoice>=0){
            rect(150,205,980,260,{7,15,20,245});rect(150,205,980,5,s.accent);
            text("¿QUÉ DESEAS HACER?",300,240,white,bigFont,680);
            text(actionItem.title,300,300,muted,font,680);
            const bool actionFromSd=actionLocalPath.rfind("sdmc:/",0)==0;
            if(!actionFromSd)text(actionLocalPath.empty()?(actionChoice==0?"> DESCARGAR":"  DESCARGAR"):(actionChoice==0?"> COPIAR A SD":"  COPIAR A SD"),190,370,actionChoice==0?s.accent:muted,font,260);
            text(actionChoice==1?"> INSTALAR SD":"  INSTALAR SD",actionFromSd?300:485,370,actionChoice==1?s.accent:muted,font,290);
            text(actionChoice==2?"> INSTALAR INTERNA":"  INSTALAR INTERNA",actionFromSd?670:785,370,actionChoice==2?s.accent:muted,font,340);
            text(actionLocalPath.empty()?"Instalar usa el repositorio sin guardar el paquete.":actionFromSd?"Instalación local directa desde la SD.":"Instalar lee el USB sin copiar el paquete a la SD.",300,410,muted,smallFont,720);
            text("←/→ elegir   A confirmar   B cancelar",360,440,white,smallFont,560);
        }
        // Modal superior para la transferencia PC -> Switch.
        {
            std::lock_guard<std::mutex> lock(usbIncomingMutex);
            if(usbIncoming.pending&&!usbIncoming.decisionReady){
                rect(105,110,1070,500,{7,15,20,252});rect(105,110,1070,6,s.accent);
                text("PORTAL DE ENTRADA / PC",160,145,white,bigFont,950);
                text(usbIncoming.name,160,205,s.accent,font,930);
                text(bytes(static_cast<double>(usbIncoming.size))+" · "+std::to_string(usbIncoming.fileIndex+1)+"/"+std::to_string(usbIncoming.totalFiles),160,240,muted,smallFont,930);
                if(!usbIncoming.choosingPath){
                    const char* options[]={"INSTALAR EN SD","INSTALAR EN MEMORIA INTERNA","COPIAR A SD","CANCELAR"};
                    for(int i=0;i<4;i++){const int y=300+i*56;rect(155,y,970,46,i==usbIncoming.menuChoice?SDL_Color{43,59,42,255}:panel);if(i==usbIncoming.menuChoice)rect(155,y,5,46,s.accent);const bool enabled=i>=2||pcInstallable(usbIncoming.name);text(std::string(i==usbIncoming.menuChoice?"> ":"  ")+options[i],180,y+10,!enabled?muted:i==usbIncoming.menuChoice?s.accent:white,font,900);}
                    text("↑/↓ elegir · A confirmar · B cancelar",260,548,muted,smallFont,760);
                }else{
                    text("COPIAR A SD · "+usbIncoming.directory,160,285,white,font,930);
                    const int first=usbIncomingBrowseSelected/4*4;
                    for(int i=first;i<std::min(first+4,static_cast<int>(usbIncomingBrowse.size()));i++){const auto& e=usbIncomingBrowse[i];const int y=330+(i-first)*48;rect(155,y,970,41,i==usbIncomingBrowseSelected?SDL_Color{43,59,42,255}:panel);text(std::string(e.directory?"[DIR] ":"      ")+e.name,180,y+8,i==usbIncomingBrowseSelected?s.accent:white,smallFont,900);}
                    if(usbIncomingBrowse.empty())text("Carpeta vacía",180,350,muted,font,850);
                    text("A abrir carpeta · B subir/volver · Y usar esta carpeta",205,548,muted,smallFont,860);
                }
            }else if(usbIncoming.active){
                rect(24,551,1232,121,{19,21,23,255});
                rect(24,551,4,121,s.accent);
                const std::string phase=usbIncoming.phase.empty()?"Recibiendo desde el PC":usbIncoming.phase;
                text(phase,45,560,{92,210,227,255},smallFont,930);
                text(usbIncoming.name,45,586,white,smallFont,1130);
                const uint64_t done=usbIncoming.processing?usbIncoming.operationDone:usbIncoming.received;
                const uint64_t total=usbIncoming.processing?usbIncoming.operationTotal:usbIncoming.size;
                static uint64_t previousDone=0;
                static Uint32 previousTick=0,previousIndex=UINT32_MAX;
                static bool previousProcessing=false;
                static double measuredSpeed=0;
                const Uint32 now=SDL_GetTicks();
                if(previousIndex!=usbIncoming.fileIndex||previousProcessing!=usbIncoming.processing||done<previousDone||!previousTick){
                    previousIndex=usbIncoming.fileIndex;previousProcessing=usbIncoming.processing;
                    previousDone=done;previousTick=now;measuredSpeed=0;
                }else if(now-previousTick>=500){
                    measuredSpeed=(done-previousDone)*1000.0/(now-previousTick);
                    previousDone=done;previousTick=now;
                }
                const double pct=total?100.0*static_cast<double>(done)/static_cast<double>(total):0.0;
                const auto eta=measuredSpeed>0&&total>done?std::to_string(static_cast<uint64_t>((total-done)/measuredSpeed))+" s":"--";
                char progress[256];snprintf(progress,sizeof(progress),"%.1f%%   /   %.1f de %.1f MiB   /   %.2f MiB/s   /   ETA %s",pct,done/1048576.0,total/1048576.0,measuredSpeed/1048576.0,eta.c_str());
                text(progress,45,615,s.accent,smallFont,960);
                text("B  Cancelar",1090,615,{255,135,145,255},smallFont,150);
                rect(45,652,1185,6,panel);
                if(total>0)rect(45,652,static_cast<int>(1185*std::min(1.0,pct/100.0)),6,s.accent);
            }
        }
        if(magicTcpApproval==1){
            std::string peer;{std::lock_guard<std::mutex> lock(magicTcpPeerMutex);peer=magicTcpPeer;}
            rect(105,160,1070,360,{19,21,23,255});rect(105,160,1070,6,s.accent);
            text("AUTORIZAR PC / RED",155,200,white,bigFont,960);
            text("Origen: "+peer,155,268,s.accent,font,960);
            text("Permite leer y modificar la SD durante esta conexion.",155,325,white,smallFont,960);
            text("Acepta solo tu PC en una red de confianza.",155,365,muted,smallFont,960);
            text("A Autorizar   B Rechazar",155,445,white,font,960);
        }
        SDL_RenderPresent(renderer);
    }
    persistQueue();quitting=true;cancelled=true;remoteInputStop=true;if(remoteInputSocket>=0)shutdown(remoteInputSocket,SHUT_RDWR);if(remoteInputThread){SDL_WaitThread(remoteInputThread,nullptr);remoteInputThread=nullptr;}magicTcpStop=true;usbIncomingCancelRequested=true;usbIncomingCv.notify_all();pcDirectCv.notify_all();{std::lock_guard<std::mutex> lock(magicTcpSocketMutex);if(magicTcpClient>=0)shutdown(magicTcpClient,SHUT_RDWR);}if(magicTcpListen>=0){shutdown(magicTcpListen,SHUT_RDWR);}if(magicTcpThread){SDL_WaitThread(magicTcpThread,nullptr);magicTcpThread=nullptr;}usbIncomingCancelRequested=true;if(usbIncomingInstallThread){SDL_WaitThread(usbIncomingInstallThread,nullptr);usbIncomingInstallThread=nullptr;}magicUsbStopAndCleanup();if(thread)SDL_WaitThread(thread,nullptr);if(controlThread)SDL_WaitThread(controlThread,nullptr);
    if(backgroundTexture)SDL_DestroyTexture(backgroundTexture);
    if(mascotTexture)SDL_DestroyTexture(mascotTexture);
    clearTextCache();
    TTF_CloseFont(smallFont);TTF_CloseFont(font);TTF_CloseFont(bigFont);SDL_DestroyMutex(sourceFileMutex);SDL_DestroyMutex(mutex);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);TTF_Quit();SDL_Quit();
    if(pl)plExit();
    if(ncm)ncmExit();
    if(tc)tcExit();
    if(psm)psmExit();
    curl_global_cleanup();
#ifdef MAGICTUPPER_USBHSFS
    if(usbReady)usbHsFsExit();
#endif
    socketExit();return 0;
}
