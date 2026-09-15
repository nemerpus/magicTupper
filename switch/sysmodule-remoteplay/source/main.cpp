#include <switch.h>
#include <switch/runtime/devices/fs_dev.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <sys/stat.h>
#include "grcd.hpp"

extern "C" {
    u32 __nx_applet_type = AppletType_None;
    u32 __nx_fs_num_sessions = 1;
    #define INNER_HEAP_SIZE 0x900000
    size_t nx_inner_heap_size = INNER_HEAP_SIZE;
    char nx_inner_heap[INNER_HEAP_SIZE];

    void __libnx_initheap(void) {
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        fake_heap_start = nx_inner_heap;
        fake_heap_end = nx_inner_heap + nx_inner_heap_size;
    }
}

static Result g_appinit_setsys_init_rc = 0;
static Result g_appinit_fw_rc = 0;
static bool g_appinit_hos_was_zero = false;

extern "C" void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) diagAbortWithResult(rc);
    if (hosversionGet() == 0) {
        g_appinit_hos_was_zero = true;
        rc = setsysInitialize();
        g_appinit_setsys_init_rc = rc;
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw{};
            rc = setsysGetFirmwareVersion(&fw);
            g_appinit_fw_rc = rc;
            if (R_SUCCEEDED(rc)) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }
    // Keep the SM session alive: diagnostics, hiddbg and sockets are initialized from main().
    // libnx sysmodule templates initialize dependent services before smExit(); keeping SM
    // alive here avoids service initialization failing before we can write diagnostics.
}

extern "C" void __appExit(void) { smExit(); }

namespace {
constexpr uint16_t kPort = 8767;
constexpr uint16_t kVideoPort = 8768;
constexpr uint16_t kAudioSourcePort = 8769;
constexpr size_t kVideoBufferSize = 0x80000;
constexpr size_t kAudioChunkSize = 0x1000;
constexpr size_t kAudioBufferSize = kAudioChunkSize;
constexpr size_t kHomeJpegBufferSize = 4 * 1024 * 1024; // match proven switch-ocr sysmodule headroom
constexpr uint64_t kHomeProbeIntervalNs = 1'000'000ULL; // rc7.10: preparation throttle only; continuous HOME capture is unpaced
constexpr uint64_t kHomeSuppressAfterH264Ns = 70'000'000ULL; // HOME resumes 70 ms after the last real H.264 AU
constexpr Result kGrcdNotInitialized = 0x3E8D4;
constexpr uint8_t kVersion1 = 1;
constexpr uint8_t kVersion = 2;
constexpr uint8_t kInputType = 1;
constexpr uint8_t kAckType = 0x81;
constexpr size_t kInputSizeV1 = 40;
constexpr size_t kInputSize = 72;
constexpr size_t kAckSize = 24;
constexpr const char* kDiscoverMagic = "MTDISC1";
constexpr const char* kHereMagic = "MTHERE1";
constexpr uint64_t kDetachTimeoutNs = 2'000'000'000ULL;
constexpr uint64_t kNeutralTimeoutNs = 250'000'000ULL;

alignas(0x1000) uint8_t g_hdls_work[0x1000];
HiddbgHdlsSessionId g_hdls_session{};
constexpr size_t kMaxPads = 4;
HiddbgHdlsHandle g_pad_handles[kMaxPads]{};
bool g_hdls_attached = false;
bool g_hdls_ready = false;
bool g_pad_attached[kMaxPads]{};
bool g_fs_initialized = false;
bool g_sd_mounted = false;
FILE* g_log = nullptr;
uint64_t g_packet_count = 0;
bool g_logged_first_ack = false;
bool g_logged_first_invalid = false;
uint64_t g_last_packet_tick[kMaxPads]{};
uint32_t g_last_sequence[kMaxPads]{};
Thread g_video_thread{};
bool g_video_thread_started = false;
alignas(0x1000) uint8_t g_video_buffer[kVideoBufferSize];
alignas(0x1000) uint8_t g_audio_buffer[kAudioBufferSize];

struct AudioSessionContext {
    std::atomic<bool> running{false};
    Service* grcd = nullptr;
    sockaddr_in peer{};
    socklen_t plen = sizeof(sockaddr_in);
    // Control/status messages travel over the already-established video socket.
    // Keeping this endpoint in the audio context lets us diagnose audio capture
    // even when the dedicated UDP audio path itself is not receiving packets.
    int control_sock = -1;
    sockaddr_in control_peer{};
    uint16_t seq = 0;
    uint32_t rtp_ts = 0;
};

Thread g_audio_thread{};
bool g_audio_thread_started = false;
AudioSessionContext g_audio_ctx{};

struct HomeStreamContext {
    std::atomic<bool> running{false};
    int sock = -1;
    sockaddr_in peer{};
    socklen_t plen = sizeof(sockaddr_in);
    uint8_t* jpeg = nullptr;
    size_t jpeg_capacity = 0;
    uint32_t frame_id = 1;
};

Thread g_home_thread{};
bool g_home_thread_started = false;
HomeStreamContext g_home_ctx{};
std::atomic<uint64_t> g_last_h264_frame_tick{0};

// H.264 parameter sets required by host decoders because grc:d frame payloads
// do not carry SPS/PPS. Annex-B start codes are included.
constexpr uint8_t kSps[] = {
    0x00,0x00,0x00,0x01,0x67,0x64,0x0C,0x20,0xAC,0x2B,0x40,0x28,0x02,0xDD,0x35,0x01,
    0x0D,0x01,0xF0,0x00,0x00,0x03,0x00,0x10,0x00,0x00,0x03,0x03,0xC8,0xF0,0x88,0x46,0xA0
};
constexpr uint8_t kPps[] = {0x00,0x00,0x00,0x01,0x68,0xEE,0x3C,0xB0};


void debug_line(const char* text) {
    if (!text) return;
    svcOutputDebugString(text, std::strlen(text));
}

void logf(const char* fmt, ...) {
    char msg[512]{};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    char line[576]{};
    const unsigned long long ms = static_cast<unsigned long long>(armTicksToNs(armGetSystemTick()) / 1'000'000ULL);
    std::snprintf(line, sizeof(line), "[MT-RP %llums] %s\n", ms, msg);
    debug_line(line);
    if (g_log) {
        std::fputs(line, g_log);
        std::fflush(g_log);
    }
}

void init_diagnostics() {
    debug_line("[MT-RP] diagnostics init\n");
    Result rc = fsInitialize();
    if (R_FAILED(rc)) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "[MT-RP] fsInitialize FAILED rc=0x%08X\n", static_cast<unsigned int>(rc));
        debug_line(line);
        return;
    }
    g_fs_initialized = true;

    const int mount_rc = fsdevMountSdmc();
    if (mount_rc != 0) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "[MT-RP] fsdevMountSdmc FAILED rc=%d errno=%d\n", mount_rc, errno);
        debug_line(line);
        return;
    }
    g_sd_mounted = true;

    mkdir("sdmc:/config", 0777);
    mkdir("sdmc:/config/magicTupper", 0777);
    g_log = std::fopen("sdmc:/config/magicTupper/remoteplay-sysmodule.log", "a");
    if (!g_log) {
        char line[128]{};
        std::snprintf(line, sizeof(line), "[MT-RP] fopen log FAILED errno=%d\n", errno);
        debug_line(line);
        return;
    }
    std::setvbuf(g_log, nullptr, _IOLBF, 0);
    logf("============================================================");
    logf("magicTupper Remote Play rc.6 unified starting");
    logf("log path: sdmc:/config/magicTupper/remoteplay-sysmodule.log");
}

void shutdown_diagnostics() {
    if (g_log) {
        logf("diagnostics shutdown");
        std::fclose(g_log);
        g_log = nullptr;
    }
    if (g_sd_mounted) { fsdevUnmountDevice("sdmc"); g_sd_mounted = false; }
    if (g_fs_initialized) { fsExit(); g_fs_initialized = false; }
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (static_cast<uint16_t>(p[1]) << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
static uint64_t g_touch_packets = 0;
static bool g_touch_autopilot_active = false;
static uint64_t g_last_touch_tick = 0;
constexpr uint64_t kTouchWatchdogNs = 300'000'000ULL;

static void release_remote_touch() {
    if (!g_touch_autopilot_active) return;
    Result rc = hiddbgSetTouchScreenAutoPilotState(nullptr, 0);
    if (R_FAILED(rc)) logf("TOUCH release rc=0x%08x", rc);
    g_touch_autopilot_active = false;
}

uint64_t now_ticks();

static bool handle_touch_packet(const uint8_t* packet, size_t n) {
    if (n < 8 || std::memcmp(packet, "MTT1", 4) != 0 || packet[4] != 1) return false;
    const int count = packet[6] > 10 ? 10 : packet[6];
    if (n < 8u + (size_t)count * 12u) return true;
    HidTouchState states[10]{};
    for (int i=0; i<count; ++i) {
        const uint8_t* q = packet + 8 + i*12;
        states[i].delta_time = 0;
        states[i].attributes = 0;
        states[i].finger_id = rd32(q);
        states[i].x = rd16(q+4);
        states[i].y = rd16(q+6);
        states[i].diameter_x = rd16(q+8);
        states[i].diameter_y = rd16(q+10);
        states[i].rotation_angle = 0;
    }
    Result rc = hiddbgSetTouchScreenAutoPilotState(count ? states : nullptr, count);
    if (R_SUCCEEDED(rc)) { g_touch_autopilot_active = count > 0; g_last_touch_tick = now_ticks(); }
    ++g_touch_packets;
    if (g_touch_packets == 1 || (g_touch_packets % 250) == 0)
        logf("TOUCH MTT1 packet=%llu contacts=%d rc=0x%08x", (unsigned long long)g_touch_packets, count, rc);
    return true;
}

uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }
void wr16(uint8_t* p, uint16_t v) { p[0]=v & 0xff; p[1]=(v >> 8) & 0xff; }
void wr32(uint8_t* p, uint32_t v) { for (int i=0;i<4;i++) p[i]=(v >> (8*i)) & 0xff; }
void wr64(uint8_t* p, uint64_t v) { wr32(p, static_cast<uint32_t>(v)); wr32(p+4, static_cast<uint32_t>(v >> 32)); }
float rdf32(const uint8_t* p) { uint32_t u=rd32(p); float f=0; std::memcpy(&f,&u,sizeof(f)); return f; }

uint64_t now_ticks() { return armGetSystemTick(); }
uint64_t ticks_to_ns(uint64_t ticks) { return armTicksToNs(ticks); }
uint32_t monotonic_ms32() { return static_cast<uint32_t>(ticks_to_ns(now_ticks()) / 1'000'000ULL); }

bool valid_packet(const uint8_t* p, size_t n) {
    if (n < kInputSizeV1 || p[0]!='M' || p[1]!='T' || p[2]!='R' || p[3]!='P' || p[5]!=kInputType) return false;
    if (p[4]==kVersion1) return n==kInputSizeV1 && rd16(p+6)==kInputSizeV1;
    if (p[4]==kVersion) return n==kInputSize && rd16(p+6)==kInputSize;
    return false;
}

uint64_t map_buttons(uint32_t b) {
    uint64_t out = 0;
    if (b & (1u<<0)) out |= HidNpadButton_A;
    if (b & (1u<<1)) out |= HidNpadButton_B;
    if (b & (1u<<2)) out |= HidNpadButton_X;
    if (b & (1u<<3)) out |= HidNpadButton_Y;
    if (b & (1u<<4)) out |= HidNpadButton_L;
    if (b & (1u<<5)) out |= HidNpadButton_R;
    if (b & (1u<<6)) out |= HidNpadButton_ZL;
    if (b & (1u<<7)) out |= HidNpadButton_ZR;
    if (b & (1u<<8)) out |= HidNpadButton_Minus;
    if (b & (1u<<9)) out |= HidNpadButton_Plus;
    if (b & (1u<<10)) out |= HidNpadButton_StickL;
    if (b & (1u<<11)) out |= HidNpadButton_StickR;
    if (b & (1u<<12)) out |= HidNpadButton_Up;
    if (b & (1u<<13)) out |= HidNpadButton_Down;
    if (b & (1u<<14)) out |= HidNpadButton_Left;
    if (b & (1u<<15)) out |= HidNpadButton_Right;
    if (b & (1u<<16)) out |= HiddbgNpadButton_Home;
    if (b & (1u<<17)) out |= HiddbgNpadButton_Capture;
    return out;
}

s32 scale_axis(int16_t v) {
    // HDLS uses the normal Npad analog range. Keep a tiny deadzone and scale to +/-0x7fff.
    if (v > -1500 && v < 1500) return 0;
    return static_cast<s32>(v);
}

Result attach_pad(size_t player) {
    if (player >= kMaxPads) return 1;
    if (!g_hdls_ready) return 1;
    if (g_pad_attached[player]) return 0;
    HiddbgHdlsDeviceInfo info{};
    info.deviceType = HidDeviceType_FullKey3;
    info.npadInterfaceType = HidNpadInterfaceType_Bluetooth;
    // Different grip tint per virtual pad to make diagnostics easier.
    static constexpr uint8_t colors[kMaxPads][3] = {
        {36,203,75}, {0,160,255}, {255,120,40}, {180,80,255}
    };
    info.singleColorBody = RGBA8_MAXALPHA(colors[player][0], colors[player][1], colors[player][2]);
    info.singleColorButtons = RGBA8_MAXALPHA(18, 18, 18);
    info.colorLeftGrip = info.singleColorBody;
    info.colorRightGrip = info.singleColorBody;
    Result rc = hiddbgAttachHdlsVirtualDevice(&g_pad_handles[player], &info);
    if (R_SUCCEEDED(rc)) {
        g_pad_attached[player] = true;
        logf("HDLS virtual device P%u attached", static_cast<unsigned>(player + 1));
    } else {
        logf("hiddbgAttachHdlsVirtualDevice P%u FAILED rc=0x%08X", static_cast<unsigned>(player + 1), static_cast<unsigned int>(rc));
    }
    return rc;
}

void detach_pad(size_t player) {
    if (player >= kMaxPads || !g_pad_attached[player]) return;
    HiddbgHdlsState neutral{};
    neutral.battery_level = 4;
    neutral.flags = 1;
    hiddbgSetHdlsState(g_pad_handles[player], &neutral);
    Result rc = hiddbgDetachHdlsVirtualDevice(g_pad_handles[player]);
    logf("HDLS virtual device P%u detached rc=0x%08X", static_cast<unsigned>(player + 1), static_cast<unsigned int>(rc));
    g_pad_attached[player] = false;
    g_pad_handles[player] = {};
}

Result apply_state(size_t player, const uint8_t* p) {
    Result rc = attach_pad(player);
    if (R_FAILED(rc)) return rc;
    HiddbgHdlsState state{};
    state.battery_level = 4;
    state.flags = 1;
    state.buttons = map_buttons(rd32(p+20));
    state.analog_stick_l.x = scale_axis(static_cast<int16_t>(rd16(p+24)));
    state.analog_stick_l.y = scale_axis(static_cast<int16_t>(rd16(p+26)));
    state.analog_stick_r.x = scale_axis(static_cast<int16_t>(rd16(p+28)));
    state.analog_stick_r.y = scale_axis(static_cast<int16_t>(rd16(p+30)));
    if (p[4] == kVersion && (p[37] & 1)) {
        state.six_axis_sensor_angle.x = rdf32(p+40);
        state.six_axis_sensor_angle.y = rdf32(p+44);
        state.six_axis_sensor_angle.z = rdf32(p+48);
        state.six_axis_sensor_acceleration.x = rdf32(p+52);
        state.six_axis_sensor_acceleration.y = rdf32(p+56);
        state.six_axis_sensor_acceleration.z = rdf32(p+60);
    }
    return hiddbgSetHdlsState(g_pad_handles[player], &state);
}

void apply_neutral(size_t player) {
    if (player >= kMaxPads || !g_pad_attached[player]) return;
    HiddbgHdlsState state{};
    state.battery_level = 4;
    state.flags = 1;
    hiddbgSetHdlsState(g_pad_handles[player], &state);
}

bool handle_discovery(int sock, const sockaddr_in& peer, socklen_t peer_len, const uint8_t* p, size_t n) {
    constexpr size_t magic_len = 7;
    if (n < magic_len || std::memcmp(p, kDiscoverMagic, magic_len) != 0) return false;
    char reply[160]{};
    std::snprintf(reply, sizeof(reply), "%s|magicTupper|fix12-network|tcp=8766|input=8767|video=rtp-h264:8768|pads=4|mtrp=2|sixaxis=1|hdls=%s",
                  kHereMagic, g_hdls_ready ? "ready" : "unavailable");
    const int sent = sendto(sock, reply, std::strlen(reply), 0,
                            reinterpret_cast<const sockaddr*>(&peer), peer_len);
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    logf("discovery request from %s:%u reply=%d", ip[0] ? ip : "?", ntohs(peer.sin_port), sent);
    return true;
}



// Experimental HOME/system-UI fallback. caps:sc captures the compositor layer stack as JPEG.
// This is deliberately kept separate from the stable grc:d H.264 path: when a game is
// available we continue to use grc:d unchanged; when grc:d reports NO_GAME we probe the
// full default layer stack and ship JPEG frames over the same UDP video socket.
struct HomeJpegHeader {
    uint8_t magic[4]; // MTJ1
    uint32_t frame_id_be;
    uint32_t offset_be;
    uint32_t total_be;
};

uint32_t host_to_be32(uint32_t v) { return htonl(v); }

bool send_home_jpeg(int sock, const sockaddr_in& peer, socklen_t plen, uint32_t frame_id, const uint8_t* jpeg, size_t jpeg_size) {
    constexpr size_t kDatagram = 1200;
    constexpr size_t kHeader = sizeof(HomeJpegHeader);
    constexpr size_t kPayload = kDatagram - kHeader;
    uint8_t packet[kDatagram]{};
    auto* h = reinterpret_cast<HomeJpegHeader*>(packet);
    h->magic[0]='M'; h->magic[1]='T'; h->magic[2]='J'; h->magic[3]='1';
    h->frame_id_be = host_to_be32(frame_id);
    h->total_be = host_to_be32(static_cast<uint32_t>(jpeg_size));
    for (size_t off=0; off<jpeg_size; off+=kPayload) {
        const size_t n = std::min(kPayload, jpeg_size-off);
        h->offset_be = host_to_be32(static_cast<uint32_t>(off));
        std::memcpy(packet+kHeader, jpeg+off, n);
        const int sent = sendto(sock, packet, kHeader+n, 0, reinterpret_cast<const sockaddr*>(&peer), plen);
        if (sent != static_cast<int>(kHeader+n)) return false;
    }
    return true;
}

void home_stream_thread(void* arg) {
    auto* ctx = static_cast<HomeStreamContext*>(arg);
    if (!ctx || !ctx->jpeg || ctx->sock < 0) return;
    logf("HOME stream thread started: caps:sc Default unpaced/latest-frame mode");
    uint64_t sent_frames = 0;
    uint64_t failures = 0;
    bool suppressed = false;
    while (ctx->running.load(std::memory_order_relaxed)) {
        const uint64_t now_tick = now_ticks();
        const uint64_t last_h264_tick = g_last_h264_frame_tick.load(std::memory_order_relaxed);
        const uint64_t quiet_ns = last_h264_tick == 0 ? UINT64_MAX : ticks_to_ns(now_tick - last_h264_tick);
        const bool game_active = last_h264_tick != 0 && quiet_ns < kHomeSuppressAfterH264Ns;
        if (game_active) {
            if (!suppressed) { logf("HOME stream suspended: H.264 active"); suppressed = true; }
            svcSleepThread(10'000'000LL);
            continue;
        }
        if (suppressed) { logf("HOME stream resumed: H.264 quiet >=%llu ms", (unsigned long long)(kHomeSuppressAfterH264Ns/1000000ULL)); suppressed = false; }
        u64 jpeg_size = 0;
        Result rc = capsscCaptureJpegScreenShot(&jpeg_size, ctx->jpeg, ctx->jpeg_capacity,
                                                 ViLayerStack_Default, 1'000'000'000LL);
        if (R_SUCCEEDED(rc) && jpeg_size > 0 && jpeg_size <= ctx->jpeg_capacity) {
            if (send_home_jpeg(ctx->sock, ctx->peer, ctx->plen, ctx->frame_id++, ctx->jpeg, static_cast<size_t>(jpeg_size))) {
                ++sent_frames;
                if (sent_frames == 1 || sent_frames % 100 == 0)
                    logf("HOME stream frame=%llu jpeg=%llu", (unsigned long long)sent_frames, (unsigned long long)jpeg_size);
            } else {
                ++failures;
            }
        } else {
            ++failures;
            if (failures == 1 || failures % 20 == 0)
                logf("HOME stream capture failed rc=0x%08X jpeg=%llu", (unsigned)rc, (unsigned long long)jpeg_size);
        }
        // rc7.10: no artificial frame pacing. Start the next compositor capture as soon
        // as caps:sc returns; the capture call itself is the natural cadence limiter.
    }
    logf("HOME stream thread ended frames=%llu failures=%llu", (unsigned long long)sent_frames, (unsigned long long)failures);
}

// ---- MTV2 / RTP H.264 -------------------------------------------------------
// LAN video deliberately uses RTP-style H.264 over UDP instead of TCP. Losing a
// late packet drops that access unit rather than blocking all newer frames behind it.
constexpr size_t kRtpMtu = 1200;
constexpr uint8_t kRtpVideoPayloadType = 96;
constexpr uint8_t kRtpAudioPayloadType = 97;

const uint8_t* strip_annexb(const uint8_t* p, size_t& n) {
    if (n >= 4 && p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) { p+=4; n-=4; }
    else if (n >= 3 && p[0]==0 && p[1]==0 && p[2]==1) { p+=3; n-=3; }
    return p;
}

bool rtp_send_packet(int sock, const sockaddr_in& peer, socklen_t plen, uint16_t& seq,
                     uint32_t timestamp, uint32_t ssrc, uint8_t payload_type, bool marker,
                     const uint8_t* payload, size_t payload_len) {
    uint8_t packet[1400]{};
    if (payload_len + 12 > sizeof(packet)) return false;
    packet[0]=0x80; packet[1]=static_cast<uint8_t>((marker?0x80:0) | (payload_type & 0x7F));
    const uint16_t be_seq=htons(seq++); std::memcpy(packet+2,&be_seq,2);
    const uint32_t be_ts=htonl(timestamp), be_ssrc=htonl(ssrc);
    std::memcpy(packet+4,&be_ts,4); std::memcpy(packet+8,&be_ssrc,4);
    std::memcpy(packet+12,payload,payload_len);
    const ssize_t out=sendto(sock,packet,payload_len+12,0,reinterpret_cast<const sockaddr*>(&peer),plen);
    return out==static_cast<ssize_t>(payload_len+12);
}

bool rtp_send_nal(int sock, const sockaddr_in& peer, socklen_t plen, uint16_t& seq,
                  uint32_t timestamp, uint32_t ssrc, const uint8_t* raw, size_t raw_len, bool marker) {
    const uint8_t* nal=strip_annexb(raw,raw_len);
    if (!raw_len) return true;
    constexpr size_t max_payload=kRtpMtu-12;
    if (raw_len <= max_payload)
        return rtp_send_packet(sock,peer,plen,seq,timestamp,ssrc,kRtpVideoPayloadType,marker,nal,raw_len);

    const uint8_t nal_header=nal[0];
    const uint8_t fu_indicator=static_cast<uint8_t>((nal_header & 0xE0) | 28);
    const uint8_t nal_type=nal_header & 0x1F;
    size_t off=1;
    while(off<raw_len) {
        uint8_t payload[kRtpMtu]{};
        const size_t chunk=std::min(max_payload-2, raw_len-off);
        const bool first=(off==1), last=(off+chunk==raw_len);
        payload[0]=fu_indicator; payload[1]=static_cast<uint8_t>(nal_type | (first?0x80:0) | (last?0x40:0));
        std::memcpy(payload+2,nal+off,chunk);
        if(!rtp_send_packet(sock,peer,plen,seq,timestamp,ssrc,kRtpVideoPayloadType,marker&&last,payload,chunk+2)) return false;
        off+=chunk;
    }
    return true;
}

void audio_stream_thread(void* arg) {
    auto* ctx = static_cast<AudioSessionContext*>(arg);
    if (!ctx || !ctx->grcd) return;

    const uint32_t audio_ssrc = 0x4D544155u;
    int audio_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (audio_sock < 0) {
        logf("audio UDP socket FAILED errno=%d", errno);
        return;
    }
    int sndbuf = 0x40000;
    setsockopt(audio_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    int one = 1;
    setsockopt(audio_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in audio_bind{};
    audio_bind.sin_family = AF_INET;
    audio_bind.sin_addr.s_addr = INADDR_ANY;
    audio_bind.sin_port = htons(kAudioSourcePort);
    if (bind(audio_sock, reinterpret_cast<sockaddr*>(&audio_bind), sizeof(audio_bind)) < 0) {
        logf("audio UDP bind %u FAILED errno=%d", kAudioSourcePort, errno);
        if (ctx->control_sock >= 0) {
            const char e[] = "MTV2|AUDIO|ERR|UDP_BIND";
            sendto(ctx->control_sock, e, sizeof(e)-1, 0, reinterpret_cast<const sockaddr*>(&ctx->control_peer), ctx->plen);
        }
        close(audio_sock);
        return;
    }

    bool announced = false;
    uint64_t transfer_ok = 0;
    uint64_t rtp_sent = 0;
    uint64_t rtp_errors = 0;
    uint64_t capture_bytes = 0;
    uint64_t diag_capture_bytes = 0;
    uint64_t last_diag_tick = armGetSystemTick();
    logf("audio thread started on isolated UDP/%u socket fd=%d", kAudioSourcePort, audio_sock);
    if (ctx->control_sock >= 0) {
        const char t[] = "MTV2|AUDIO|THREAD|READY";
        sendto(ctx->control_sock, t, sizeof(t)-1, 0, reinterpret_cast<const sockaddr*>(&ctx->control_peer), ctx->plen);
    }
    while (ctx->running.load(std::memory_order_relaxed)) {
        u32 part_size = 0;
        u64 capture_ts = 0;
        Result arc = mtGrcdTransfer(ctx->grcd, GrcStream_Audio,
                                    g_audio_buffer, kAudioChunkSize,
                                    nullptr, &part_size, &capture_ts);
        if (!ctx->running.load(std::memory_order_relaxed)) break;
        if (R_FAILED(arc)) {
            logf("grc:d audio transfer unavailable rc=0x%08X", (unsigned)arc);
            char err[48]{};
            std::snprintf(err, sizeof(err), "MTV2|AUDIO|ERR|%08X", (unsigned)arc);
            sendto(audio_sock, err, std::strlen(err), 0,
                   reinterpret_cast<const sockaddr*>(&ctx->peer), ctx->plen);
            if (ctx->control_sock >= 0)
                sendto(ctx->control_sock, err, std::strlen(err), 0, reinterpret_cast<const sockaddr*>(&ctx->control_peer), ctx->plen);
            svcSleepThread(50'000'000LL);
            continue;
        }

        if (part_size > kAudioChunkSize) part_size = kAudioChunkSize;
        part_size &= ~static_cast<u32>(3);
        if (part_size < 4) continue;
        ++transfer_ok;
        capture_bytes += part_size;

        int peak = 0;
        uint32_t nonzero = 0;
        const int16_t* pcm = reinterpret_cast<const int16_t*>(g_audio_buffer);
        const size_t samples = part_size / sizeof(int16_t);
        for (size_t i = 0; i < samples; ++i) {
            int v = pcm[i];
            if (v != 0) ++nonzero;
            if (v < 0) v = -v;
            if (v > peak) peak = v;
        }

        if (!announced) {
            announced = true;
            logf("audio capture ACTIVE bytes=%u ts=%llu",
                 (unsigned)part_size, (unsigned long long)capture_ts);
            const char a[] = "MTV2|AUDIO|ACTIVE";
            sendto(audio_sock, a, sizeof(a)-1, 0, reinterpret_cast<const sockaddr*>(&ctx->peer), ctx->plen);
            if (ctx->control_sock >= 0)
                sendto(ctx->control_sock, a, sizeof(a)-1, 0, reinterpret_cast<const sockaddr*>(&ctx->control_peer), ctx->plen);
        }

        constexpr size_t max_audio_payload = ((kRtpMtu - 12) / 4) * 4;
        size_t off = 0;
        while (off < part_size && ctx->running.load(std::memory_order_relaxed)) {
            size_t chunk = std::min(max_audio_payload, static_cast<size_t>(part_size) - off);
            chunk &= ~static_cast<size_t>(3);
            if (!chunk) break;
            const bool marker = (off + chunk >= part_size);
            if (!rtp_send_packet(audio_sock, ctx->peer, ctx->plen, ctx->seq, ctx->rtp_ts,
                                 audio_ssrc, kRtpAudioPayloadType, marker,
                                 g_audio_buffer + off, chunk)) {
                logf("RTP audio send failed errno=%d", errno);
                // Drop this packet rather than ever blocking/starving the video path.
                ++rtp_errors;
                break;
            }
            ++rtp_sent;
            ctx->rtp_ts += static_cast<uint32_t>(chunk / 4);
            off += chunk;
        }

        const uint64_t now_diag = armGetSystemTick();
        const uint64_t diag_ns = armTicksToNs(now_diag - last_diag_tick);
        if (ctx->control_sock >= 0 && diag_ns >= 1000000000ULL) {
            const uint64_t delta_bytes = capture_bytes - diag_capture_bytes;
            const uint64_t cap_bps = diag_ns ? (delta_bytes * 1000000000ULL / diag_ns) : 0;
            char diag[208]{};
            std::snprintf(diag, sizeof(diag),
                          "MTV2|AUDIO|DIAG|xfer=%llu|bytes=%u|capBps=%llu|peak=%d|nonzero=%u|rtp=%llu|senderr=%llu",
                          (unsigned long long)transfer_ok, (unsigned)part_size, (unsigned long long)cap_bps,
                          peak, (unsigned)nonzero, (unsigned long long)rtp_sent, (unsigned long long)rtp_errors);
            sendto(ctx->control_sock, diag, std::strlen(diag), 0,
                   reinterpret_cast<const sockaddr*>(&ctx->control_peer), ctx->plen);
            diag_capture_bytes = capture_bytes;
            last_diag_tick = now_diag;
        }
    }

    close(audio_sock);
    logf("audio thread stopped");
}

void video_server_thread(void*) {
    logf("video thread starting MTV2 RTP/H264 UDP/%u build=1.0.0-stable", kVideoPort);
    bool capssc_ready = false;
    bool capssc_attempted = false;
    uint64_t last_home_capture_ns = 0;
    uint32_t home_frame_id = 1;
    uint8_t* home_jpeg_buffer = nullptr;
    int sock=socket(AF_INET,SOCK_DGRAM,0);
    if(sock<0){logf("video UDP socket FAILED errno=%d",errno);return;}
    int one=1; setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    int sndbuf=0x80000; setsockopt(sock,SOL_SOCKET,SO_SNDBUF,&sndbuf,sizeof(sndbuf));
    sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=INADDR_ANY;addr.sin_port=htons(kVideoPort);
    if(bind(sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))<0){logf("video UDP bind FAILED errno=%d",errno);close(sock);return;}
    logf("video RTP/H264 listening UDP/%u mtu=%zu",kVideoPort,kRtpMtu);

    for(;;){
        uint8_t control[128]{};sockaddr_in peer{};socklen_t plen=sizeof(peer);
        const int n=recvfrom(sock,control,sizeof(control)-1,0,reinterpret_cast<sockaddr*>(&peer),&plen);
        if(n<=0)continue;
        control[n]=0;
        if(n<10 || std::memcmp(control,"MTV2|START",10)!=0)continue;

        char ip[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&peer.sin_addr,ip,sizeof(ip));

        bool audio_requested = std::strstr(reinterpret_cast<const char*>(control), "AUDIO=1") != nullptr;
        uint16_t audio_port = ntohs(peer.sin_port);
        if (const char* ap = std::strstr(reinterpret_cast<const char*>(control), "APORT=")) {
            const unsigned long parsed = std::strtoul(ap + 6, nullptr, 10);
            if (parsed > 0 && parsed <= 65535) audio_port = static_cast<uint16_t>(parsed);
        }

        const char ok[]="MTV2|OK|RTP-H264|PT=96|AUDIO=PCM-S16LE-48K-STEREO|APT=97|MTU=1200|720P30|BUILD=rc7.2-home-phase1b";
        sendto(sock,ok,sizeof(ok)-1,0,reinterpret_cast<const sockaddr*>(&peer),plen);
        logf("video MTV2 client %s:%u audio=%d aport=%u",
             ip[0]?ip:"?",ntohs(peer.sin_port),audio_requested?1:0,(unsigned)audio_port);

        uint16_t seq=static_cast<uint16_t>(now_ticks());
        uint32_t rtp_ts=static_cast<uint32_t>(now_ticks());
        const uint32_t ssrc=0x4D545250u;
        uint64_t frames=0;
        uint64_t last_client_ns=ticks_to_ns(now_ticks());
        uint64_t last_wait_ns=0;
        bool session_active=true;
        Service grcd{};
        Service audio_grcd{};
        bool grcd_open=false;
        bool audio_grcd_open=false;
        bool grcd_begun=false;
        uint64_t last_video_frame_ns=ticks_to_ns(now_ticks());
        bool home_probe_announced=false;

        auto same_peer = [&](const sockaddr_in& a)->bool {
            return a.sin_family==peer.sin_family && a.sin_port==peer.sin_port && a.sin_addr.s_addr==peer.sin_addr.s_addr;
        };
        auto close_grcd = [&]() {
            if(audio_grcd_open){ mtGrcdClose(&audio_grcd); audio_grcd={}; audio_grcd_open=false; }
            if(grcd_open){ mtGrcdClose(&grcd); grcd={}; grcd_open=false; }
            grcd_begun=false;
        };
        auto send_wait_no_game = [&]() {
            const uint64_t now_ns=ticks_to_ns(now_ticks());
            if(now_ns-last_wait_ns < 900'000'000ULL)return;
            last_wait_ns=now_ns;
            const char w[]="MTV2|WAIT|NO_GAME";
            sendto(sock,w,sizeof(w)-1,0,reinterpret_cast<const sockaddr*>(&peer),plen);
        };
        auto poll_control = [&]()->bool {
            for(;;){
                uint8_t c[128]{};sockaddr_in from{};socklen_t flen=sizeof(from);
                const int rn=recvfrom(sock,c,sizeof(c)-1,MSG_DONTWAIT,reinterpret_cast<sockaddr*>(&from),&flen);
                if(rn<0){ if(errno==EAGAIN || errno==EWOULDBLOCK)return true; return true; }
                if(rn==0)return true;
                if(!same_peer(from))continue;
                c[rn]=0;
                if(rn>=9 && std::memcmp(c,"MTV2|STOP",9)==0){
                    logf("video MTV2 STOP from %s:%u",ip[0]?ip:"?",ntohs(peer.sin_port));
                    return false;
                }
                if((rn>=9 && std::memcmp(c,"MTV2|PING",9)==0) || (rn>=10 && std::memcmp(c,"MTV2|START",10)==0)){
                    last_client_ns=ticks_to_ns(now_ticks());
                    if(rn>=10 && std::memcmp(c,"MTV2|START",10)==0)
                        sendto(sock,ok,sizeof(ok)-1,0,reinterpret_cast<const sockaddr*>(&peer),plen);
                }
            }
        };

        auto ensure_audio_thread = [&]() {
            if (g_audio_thread_started || !audio_requested) return;
            if (!audio_grcd_open) {
                logf("audio thread not started: audio grc:d session is not open");
                return;
            }
            g_audio_ctx.grcd = &audio_grcd;
            g_audio_ctx.control_sock = sock;
            g_audio_ctx.control_peer = peer;
            g_audio_ctx.peer = peer;
            g_audio_ctx.peer.sin_port = htons(audio_port);
            g_audio_ctx.plen = plen;
            g_audio_ctx.seq = static_cast<uint16_t>(now_ticks() >> 8);
            g_audio_ctx.rtp_ts = 0;
            g_audio_ctx.running.store(true, std::memory_order_relaxed);
            Result audio_thread_rc = threadCreate(&g_audio_thread, audio_stream_thread, &g_audio_ctx,
                                                  nullptr, 0x6000, 0x2D, 2);
            if (R_SUCCEEDED(audio_thread_rc)) {
                Result audio_start_rc = threadStart(&g_audio_thread);
                if (R_SUCCEEDED(audio_start_rc)) {
                    g_audio_thread_started = true;
                    logf("audio thread started after grc:d became capture-ready on core 2");
                } else {
                    g_audio_ctx.running.store(false, std::memory_order_relaxed);
                    logf("audio threadStart FAILED rc=0x%08X", (unsigned)audio_start_rc);
                    char err[64]{};
                    std::snprintf(err, sizeof(err), "MTV2|AUDIO|ERR|THREAD_START_%08X", (unsigned)audio_start_rc);
                    sendto(sock, err, std::strlen(err), 0, reinterpret_cast<const sockaddr*>(&peer), plen);
                    threadClose(&g_audio_thread);
                }
            } else {
                g_audio_ctx.running.store(false, std::memory_order_relaxed);
                logf("audio threadCreate FAILED rc=0x%08X", (unsigned)audio_thread_rc);
                char err[64]{};
                std::snprintf(err, sizeof(err), "MTV2|AUDIO|ERR|THREAD_CREATE_%08X", (unsigned)audio_thread_rc);
                sendto(sock, err, std::strlen(err), 0, reinterpret_cast<const sockaddr*>(&peer), plen);
            }
        };

        auto probe_home = [&]() {
            // Experimental HOME fallback: capture the final/default VI layer stack as JPEG.
            // At 2 FPS this is only a proof-of-path; once validated we can replace JPEG
            // polling with a lower-level VI/NV buffer path suitable for real-time encoding.
            const uint64_t home_now_ns = ticks_to_ns(now_ticks());
            if (home_now_ns - last_home_capture_ns >= kHomeProbeIntervalNs) {
                last_home_capture_ns = home_now_ns;
                if (!capssc_attempted) {
                    capssc_attempted = true;
                    Result crc = capsscInitialize();
                    capssc_ready = R_SUCCEEDED(crc);
                    logf("HOME probe caps:sc init rc=0x%08X ready=%d", (unsigned)crc, capssc_ready ? 1 : 0);
                    char msg[64]{};
                    std::snprintf(msg, sizeof(msg), capssc_ready ? "MTV2|HOME|CAPSSC|READY" : "MTV2|HOME|CAPSSC|ERR|%08X", (unsigned)crc);
                    sendto(sock, msg, std::strlen(msg), 0, reinterpret_cast<const sockaddr*>(&peer), plen);
                }
                if (capssc_ready) {
                    // Do not reserve another 512 KiB in .bss. This sysmodule already owns a
                    // fixed 3 MiB libnx heap; allocating the HOME probe buffer lazily keeps
                    // the executable memory footprint at the known-good pre-HOME size.
                    if (!home_jpeg_buffer) {
                        home_jpeg_buffer = static_cast<uint8_t*>(std::malloc(kHomeJpegBufferSize));
                        if (!home_jpeg_buffer) {
                            const char msg[] = "MTV2|HOME|CAPTURE|ERR|NO_MEMORY";
                            sendto(sock, msg, sizeof(msg)-1, 0, reinterpret_cast<const sockaddr*>(&peer), plen);
                            logf("HOME JPEG malloc(%u) failed", (unsigned)kHomeJpegBufferSize);
                            capssc_ready = false;
                            return;
                        }
                        logf("HOME JPEG buffer allocated from libnx heap: %u bytes", (unsigned)kHomeJpegBufferSize);
                    }
                    // rc7.10: caps:sc + ViLayerStack_Default are already validated.
                    // Do not spend ~80 ms on the old Default+Screenshot diagnostic probe;
                    // the dedicated HOME worker will perform the first real capture immediately.
                    logf("HOME capture backend prepared: caps:sc Default buffer=%u", (unsigned)kHomeJpegBufferSize);
                }
            }
        };

        while(session_active){
            if(!poll_control()){ session_active=false; break; }
            const uint64_t now_ns=ticks_to_ns(now_ticks());
            if(now_ns-last_client_ns > 5'000'000'000ULL){
                logf("video MTV2 client heartbeat timeout");
                break;
            }

            if(!grcd_open){
                Result open_rc=mtGrcdOpen(&grcd);
                if(R_FAILED(open_rc)){
                    logf("grc:d open FAILED rc=0x%08X",(unsigned)open_rc);
                    const char e[]="MTV2|ERR|GRCD_OPEN";
                    sendto(sock,e,sizeof(e)-1,0,reinterpret_cast<const sockaddr*>(&peer),plen);
                    svcSleepThread(250'000'000LL);
                    continue;
                }
                grcd_open=true;
                grcd_begun=false;

                if (audio_requested) {
                    Result aopen_rc = mtGrcdOpen(&audio_grcd);
                    if (R_FAILED(aopen_rc)) {
                        logf("grc:d audio open FAILED rc=0x%08X", (unsigned)aopen_rc);
                        const char ae[]="MTV2|AUDIO|ERR|OPEN";
                        sockaddr_in audio_peer = peer;
                        audio_peer.sin_port = htons(audio_port);
                        sendto(sock, ae, sizeof(ae)-1, 0,
                               reinterpret_cast<const sockaddr*>(&audio_peer), plen);
                        audio_requested = false;
                    } else {
                        audio_grcd_open = true;
                        logf("grc:d video+audio sessions open");
                    }
                } else {
                    logf("grc:d video session open; audio disabled by client");
                }
            }

            u32 data_size=0,num_frames=0;u64 grc_ts=0;
            Result rc=mtGrcdTransfer(&grcd,GrcStream_Video,g_video_buffer,sizeof(g_video_buffer),&num_frames,&data_size,&grc_ts);
            if(rc==kGrcdNotInitialized && !grcd_begun){
                rc=mtGrcdBegin(&grcd);
                if(R_SUCCEEDED(rc)){
                    grcd_begun=true;
                    last_video_frame_ns=ticks_to_ns(now_ticks());
                    home_probe_announced=false;
                    logf("grc:d Begin OK");
                    // rc7.5 diagnostic: mtGrcdTransfer can block indefinitely while HOME is
                    // displayed, so a post-transfer 700ms timeout can never run. Probe the
                    // compositor once immediately after Begin, before entering Transfer.
                    // This is diagnostic only; GRC remains the normal game video backend.
                    logf("HOME diagnostic pre-transfer probe after grc:d Begin");
                    probe_home();
                    if (capssc_ready && home_jpeg_buffer && !g_home_thread_started) {
                        g_home_ctx.sock = sock;
                        g_home_ctx.peer = peer;
                        g_home_ctx.plen = plen;
                        g_home_ctx.jpeg = home_jpeg_buffer;
                        g_home_ctx.jpeg_capacity = kHomeJpegBufferSize;
                        g_home_ctx.frame_id = home_frame_id++;
                        g_home_ctx.running.store(true, std::memory_order_relaxed);
                        Result hrc = threadCreate(&g_home_thread, home_stream_thread, &g_home_ctx, nullptr, 0x6000, 0x2E, 2);
                        if (R_SUCCEEDED(hrc)) hrc = threadStart(&g_home_thread);
                        if (R_SUCCEEDED(hrc)) {
                            g_home_thread_started = true;
                            logf("HOME continuous stream enabled unpaced; auto-suspend on H.264");
                        } else {
                            g_home_ctx.running.store(false, std::memory_order_relaxed);
                            logf("HOME stream thread start FAILED rc=0x%08X", (unsigned)hrc);
                            threadClose(&g_home_thread);
                            g_home_thread = {};
                        }
                    }
                    ensure_audio_thread();
                    continue;
                }
                // This is the normal state when no compatible game capture is active yet.
                logf("grc:d Begin waiting for game rc=0x%08X",(unsigned)rc);
                send_wait_no_game();
                close_grcd();

                probe_home();
                svcSleepThread(50'000'000LL);
                continue;
            }
            if(R_FAILED(rc)){
                // A title can be closed or changed while Remote Play stays open. Do not turn that
                // normal lifecycle into a fatal client error: return to WAIT and reacquire grc:d.
                logf("grc:d transfer unavailable rc=0x%08X; waiting for game",(unsigned)rc);
                send_wait_no_game();
                close_grcd();
                svcSleepThread(150'000'000LL);
                continue;
            }
            if(data_size<=4){
                send_wait_no_game();
                const uint64_t silent_ns = ticks_to_ns(now_ticks()) - last_video_frame_ns;
                if (silent_ns >= 700'000'000ULL) {
                    if (!home_probe_announced) {
                        home_probe_announced = true;
                        logf("grc:d Begin succeeded but no video frames for >=700ms; probing HOME compositor");
                    }
                    probe_home();
                }
                svcSleepThread(5'000'000LL);
                continue;
            }
            if(data_size>sizeof(g_video_buffer)){
                logf("grc:d invalid oversized frame bytes=%u", data_size);
                svcSleepThread(5'000'000LL);
                continue;
            }
            last_video_frame_ns=ticks_to_ns(now_ticks());
            g_last_h264_frame_tick.store(now_ticks(), std::memory_order_relaxed);
            home_probe_announced=false;

            // Do not touch the audio service before GRC is actually capture-ready.
            // Calling an audio transfer too early can leave that service session blocked/stale.
            ensure_audio_thread();

            struct NalSpan { const uint8_t* p; size_t n; };
            NalSpan nals[32]{}; size_t nal_count=0;
            auto start_code = [&](size_t pos, size_t& sc)->bool {
                sc=0; if(pos+3<=data_size && g_video_buffer[pos]==0 && g_video_buffer[pos+1]==0 && g_video_buffer[pos+2]==1){sc=3;return true;}
                if(pos+4<=data_size && g_video_buffer[pos]==0 && g_video_buffer[pos+1]==0 && g_video_buffer[pos+2]==0 && g_video_buffer[pos+3]==1){sc=4;return true;}
                return false;
            };
            size_t pos=0, sc=0;
            while(pos<data_size && nal_count<32){
                while(pos<data_size && !start_code(pos,sc))++pos;
                if(pos>=data_size)break;
                const size_t begin=pos+sc; size_t next=begin, next_sc=0;
                while(next<data_size && !start_code(next,next_sc))++next;
                if(next>begin)nals[nal_count++]={g_video_buffer+begin,next-begin};
                pos=next;
            }
            if(nal_count==0){ size_t nn=data_size; const uint8_t* np=strip_annexb(g_video_buffer,nn); if(nn)nals[nal_count++]={np,nn}; }
            if(nal_count==0)continue;
            bool key=false, saw_sps=false, saw_pps=false;
            for(size_t i=0;i<nal_count;i++){ const uint8_t t=nals[i].p[0]&0x1F; key|=(t==5); saw_sps|=(t==7); saw_pps|=(t==8); }
            if(key && !saw_sps){if(!rtp_send_nal(sock,peer,plen,seq,rtp_ts,ssrc,kSps,sizeof(kSps),false))break;}
            if(key && !saw_pps){if(!rtp_send_nal(sock,peer,plen,seq,rtp_ts,ssrc,kPps,sizeof(kPps),false))break;}
            bool send_ok=true;
            for(size_t i=0;i<nal_count;i++){
                const bool marker=(i+1==nal_count);
                if(!rtp_send_nal(sock,peer,plen,seq,rtp_ts,ssrc,nals[i].p,nals[i].n,marker)){send_ok=false;break;}
            }
            if(!send_ok){logf("RTP send failed errno=%d",errno);break;}
            ++frames;rtp_ts+=3000;
            if(frames==1 || frames%300==0)logf("RTP AU=%llu bytes=%u nals=%zu key=%d sps=%d pps=%d",(unsigned long long)frames,data_size,nal_count,key?1:0,saw_sps?1:0,saw_pps?1:0);

        }
        if (g_home_thread_started) {
            g_home_ctx.running.store(false, std::memory_order_relaxed);
            threadWaitForExit(&g_home_thread);
            threadClose(&g_home_thread);
            g_home_thread = {};
            g_home_thread_started = false;
            g_home_ctx.jpeg = nullptr;
        }
        if (g_audio_thread_started) {
            g_audio_ctx.running.store(false, std::memory_order_relaxed);
            threadWaitForExit(&g_audio_thread);
            threadClose(&g_audio_thread);
            g_audio_thread = {};
            g_audio_thread_started = false;
            g_audio_ctx.grcd = nullptr;
        }
        close_grcd();
        logf("video MTV2 session ended frames=%llu",(unsigned long long)frames);
    }
}

void send_ack(int sock, const sockaddr_in& peer, socklen_t peer_len, const uint8_t* input) {
    uint8_t ack[kAckSize]{};
    ack[0]='M'; ack[1]='T'; ack[2]='R'; ack[3]='P'; ack[4]=input[4]; ack[5]=kAckType;
    wr16(ack+6, kAckSize);
    wr32(ack+8, rd32(input+8));
    wr64(ack+12, rd64(input+12));
    wr32(ack+20, monotonic_ms32());
    const int sent = sendto(sock, ack, sizeof(ack), 0, reinterpret_cast<const sockaddr*>(&peer), peer_len);
    if (sent == static_cast<int>(sizeof(ack))) {
        if (!g_logged_first_ack) {
            char ip[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            logf("first ACK sent seq=%u peer=%s:%u", rd32(input+8), ip[0] ? ip : "?", ntohs(peer.sin_port));
            g_logged_first_ack = true;
        }
    } else {
        logf("ACK send FAILED sent=%d errno=%d", sent, errno);
    }
}
}

int main(int, char**) {
    init_diagnostics();
    logf("main entered; HOS encoded=0x%08X", static_cast<unsigned int>(hosversionGet()));
    if (g_appinit_hos_was_zero) {
        logf("__appInit HOS was unset: setsysInitialize=0x%08X setsysGetFirmwareVersion=0x%08X",
             static_cast<unsigned int>(g_appinit_setsys_init_rc), static_cast<unsigned int>(g_appinit_fw_rc));
    }

    Result rc = hiddbgInitialize();
    if (R_FAILED(rc)) {
        logf("hiddbgInitialize FAILED rc=0x%08X; UDP diagnostics will continue", static_cast<unsigned int>(rc));
    } else {
        logf("hiddbgInitialize OK");
        rc = hiddbgAttachHdlsWorkBuffer(&g_hdls_session, g_hdls_work, sizeof(g_hdls_work));
        if (R_FAILED(rc)) {
            logf("hiddbgAttachHdlsWorkBuffer FAILED rc=0x%08X; UDP diagnostics will continue", static_cast<unsigned int>(rc));
            hiddbgExit();
        } else {
            g_hdls_attached = true;
            g_hdls_ready = true;
            logf("HDLS work buffer OK (%zu bytes)", sizeof(g_hdls_work));
        }
    }

    // rc.6-fix8: independent UDP sockets for input/discovery and RTP/H264 video.
    // No TCP head-of-line blocking on the realtime video path.
    SocketInitConfig socket_cfg{};
    socket_cfg.tcp_tx_buf_size = 0x20000;
    socket_cfg.tcp_rx_buf_size = 0x20000;
    socket_cfg.tcp_tx_buf_max_size = 0x20000;
    socket_cfg.tcp_rx_buf_max_size = 0x20000;
    // Audio and video use independent UDP sockets. Give the BSD service enough
    // queue space so PCM bursts can never starve H.264 FU-A packets.
    socket_cfg.udp_tx_buf_size = 0x10000;
    socket_cfg.udp_rx_buf_size = 0x10000;
    socket_cfg.sb_efficiency = 1;
    socket_cfg.num_bsd_sessions = 6;
    socket_cfg.bsd_service_type = BsdServiceType_User;

    logf("socketInitialize custom: heap=0x%X tcp=0x%X udp_tx=0x%X udp_rx=0x%X sessions=%u",
         INNER_HEAP_SIZE, socket_cfg.tcp_tx_buf_size, socket_cfg.udp_tx_buf_size, socket_cfg.udp_rx_buf_size, socket_cfg.num_bsd_sessions);
    rc = socketInitialize(&socket_cfg);
    if (R_FAILED(rc)) {
        const Result bsd_rc = socketGetLastResult();
        logf("socketInitialize FAILED rc=0x%08X bsd_last=0x%08X",
             static_cast<unsigned int>(rc), static_cast<unsigned int>(bsd_rc));
        if (g_hdls_attached) hiddbgReleaseHdlsWorkBuffer(g_hdls_session);
        release_remote_touch();
        if (g_hdls_ready || g_hdls_attached) hiddbgExit();
        shutdown_diagnostics();
        return static_cast<int>(rc);
    }
    logf("socketInitialize custom OK");

    Result video_thread_rc = threadCreate(&g_video_thread, video_server_thread, nullptr, nullptr, 0x10000, 0x2C, 3);
    if (R_SUCCEEDED(video_thread_rc)) {
        video_thread_rc = threadStart(&g_video_thread);
        g_video_thread_started = R_SUCCEEDED(video_thread_rc);
    }
    logf("video thread create/start rc=0x%08X started=%d", (unsigned)video_thread_rc, g_video_thread_started ? 1 : 0);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        logf("UDP socket FAILED errno=%d", errno);
        socketExit();
        if (g_hdls_attached) hiddbgReleaseHdlsWorkBuffer(g_hdls_session);
        release_remote_touch();
        if (g_hdls_ready || g_hdls_attached) hiddbgExit();
        shutdown_diagnostics();
        return -1;
    }
    logf("UDP socket OK fd=%d", sock);

    int one = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
        logf("SO_REUSEADDR warning errno=%d", errno);
    const int old_flags = fcntl(sock, F_GETFL, 0);
    if (old_flags < 0 || fcntl(sock, F_SETFL, old_flags | O_NONBLOCK) < 0)
        logf("UDP O_NONBLOCK warning errno=%d", errno);
    else
        logf("UDP socket nonblocking OK");

    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(kPort);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logf("bind UDP/%u FAILED errno=%d", kPort, errno);
        close(sock); socketExit();
        if (g_hdls_attached) hiddbgReleaseHdlsWorkBuffer(g_hdls_session);
        release_remote_touch();
        if (g_hdls_ready || g_hdls_attached) hiddbgExit();
        shutdown_diagnostics();
        return -2;
    }
    logf("bind UDP/%u OK; waiting MTRP packets", kPort);

    uint8_t packet[128]{};
    bool neutral_sent[kMaxPads] = {true, true, true, true};
    uint64_t last_recv_error_log_tick = 0;
    while (true) {
        sockaddr_in peer{}; socklen_t plen = sizeof(peer);
        const int n = recvfrom(sock, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&peer), &plen);
        const uint64_t now = now_ticks();
        if (g_touch_autopilot_active && g_last_touch_tick != 0 && ticks_to_ns(now - g_last_touch_tick) >= kTouchWatchdogNs) {
            logf("TOUCH watchdog: releasing stale remote touch");
            release_remote_touch();
            g_last_touch_tick = 0;
        }
        if (n > 0) {
            if (handle_discovery(sock, peer, plen, packet, static_cast<size_t>(n))) {
                continue;
            }
            if (handle_touch_packet(packet, static_cast<size_t>(n))) {
                continue;
            }
            if (valid_packet(packet, static_cast<size_t>(n))) {
                ++g_packet_count;
                const uint32_t seq = rd32(packet+8);
                const size_t player = packet[36] < kMaxPads ? packet[36] : 0;
                if (g_packet_count == 1 || (g_packet_count % 1000) == 0) {
                    char ip[INET_ADDRSTRLEN]{};
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    logf("MTRP packet #%llu P%u seq=%u from %s:%u HDLS=%s",
                         static_cast<unsigned long long>(g_packet_count), static_cast<unsigned>(player + 1), seq, ip[0] ? ip : "?", ntohs(peer.sin_port),
                         g_hdls_ready ? "ready" : "unavailable");
                }

                // Drop stale/out-of-order states, but ACK them so the client can diagnose the path.
                if (g_last_sequence[player] == 0 || static_cast<int32_t>(seq - g_last_sequence[player]) > 0) {
                    rc = apply_state(player, packet);
                    if (R_SUCCEEDED(rc)) {
                        g_last_sequence[player] = seq;
                        g_last_packet_tick[player] = now;
                        neutral_sent[player] = false;
                    } else if (g_packet_count == 1 || (g_packet_count % 1000) == 0) {
                        logf("apply_state FAILED rc=0x%08X", static_cast<unsigned int>(rc));
                    }
                }
                send_ack(sock, peer, plen, packet);
            } else if (!g_logged_first_invalid) {
                logf("first invalid UDP datagram len=%d magic=%02X%02X%02X%02X", n, packet[0], packet[1], packet[2], packet[3]);
                g_logged_first_invalid = true;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINPROGRESS) {
            // Never spin/log-storm on a transient BSD error. At most one line per second.
            if (last_recv_error_log_tick == 0 || ticks_to_ns(now - last_recv_error_log_tick) >= 1'000'000'000ULL) {
                logf("recvfrom error errno=%d (rate-limited)", errno);
                last_recv_error_log_tick = now;
            }
        }

        for (size_t player = 0; player < kMaxPads; ++player) {
            if (!g_pad_attached[player] || g_last_packet_tick[player] == 0) continue;
            const uint64_t elapsed = ticks_to_ns(now - g_last_packet_tick[player]);
            if (elapsed >= kNeutralTimeoutNs && !neutral_sent[player]) {
                apply_neutral(player); neutral_sent[player] = true;
                logf("failsafe P%u neutral after %llu ms", static_cast<unsigned>(player + 1), static_cast<unsigned long long>(elapsed / 1'000'000ULL));
            }
            if (elapsed >= kDetachTimeoutNs) {
                logf("failsafe P%u detach after %llu ms", static_cast<unsigned>(player + 1), static_cast<unsigned long long>(elapsed / 1'000'000ULL));
                detach_pad(player); g_last_packet_tick[player] = 0; g_last_sequence[player] = 0; neutral_sent[player] = true;
            }
        }

        // Non-blocking UDP loop: 1 ms keeps input responsive without burning a CPU core.
        if (n < 0) svcSleepThread(1'000'000LL);
    }
}
