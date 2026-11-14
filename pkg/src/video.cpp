#include "video.h"
#include "service.h"
#include "log.h"

#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/AudioOut.h>
#include <orbis/Sysmodule.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <vector>

// ---------------------------------------------------------------- структуры libSceAvPlayer (SysV ABI)

namespace {

typedef void* (*AvAlloc)(void* p, uint32_t align, uint32_t size);
typedef void  (*AvFree)(void* p, void* mem);
typedef int32_t  (*AvOpenFile)(void* p, const char* name);
typedef int32_t  (*AvCloseFile)(void* p);
typedef int32_t  (*AvReadFile)(void* p, uint8_t* buf, uint64_t pos, uint32_t len);
typedef uint64_t (*AvSizeFile)(void* p);

struct AvMem { void* obj; AvAlloc alloc; AvFree free_; AvAlloc allocTex; AvFree freeTex; };
struct AvFile { void* obj; AvOpenFile open; AvCloseFile close; AvReadFile read; AvSizeFile size; };
struct AvEvent { void* obj; void* cb; };
struct AvInit {
    AvMem mem;
    AvFile file;
    AvEvent event;
    uint32_t debugLevel;
    uint32_t basePriority;
    int32_t numFrameBuffers;
    bool autoStart;
    uint8_t reserved[3];
    const char* defaultLanguage;
};
struct AvStreamInfo {
    uint32_t type;                 // 0 - видео, 1 - звук, 2 - субтитры
    uint8_t reserved[4];
    uint8_t details[16];           // для видео: width, height (u32); для звука: channels u16, rate u32 @4
    uint64_t duration;             // мс
    uint64_t startTime;
};
struct AvVideoEx {
    uint32_t width, height;
    float aspect;
    uint8_t lang[4];
    uint32_t framerate, cropL, cropR, cropT, cropB, pitch;
    uint8_t lumaBits, chromaBits;
    bool fullRange;
};
struct AvFrameEx {                 // AvPlayerFrameInfoEx
    void* data;
    uint8_t reserved[4];
    uint64_t timestamp;
    AvVideoEx video;               // начало union details (остальное - запас под размер)
    uint8_t pad[64];
};
struct AvFrame {                   // AvPlayerFrameInfo (звук)
    uint8_t* data;
    uint8_t reserved[4];
    uint64_t timestamp;
    uint8_t details[16];           // для звука: channels u16 @0, sample_rate u32 @4, size u32 @8
};

typedef void* (*Fn_Init)(AvInit*);
typedef int32_t (*Fn_AddSource)(void*, const char*);
typedef int32_t (*Fn_StreamCount)(void*);
typedef int32_t (*Fn_GetStreamInfo)(void*, uint32_t, void*);
typedef int32_t (*Fn_Enable)(void*, uint32_t);
typedef int32_t (*Fn_H)(void*);
typedef bool    (*Fn_Active)(void*);
typedef bool    (*Fn_GetFrame)(void*, void*);
typedef uint64_t (*Fn_Time)(void*);
typedef int32_t (*Fn_Jump)(void*, uint64_t);

struct Api {
    Fn_Init init; Fn_AddSource addSource; Fn_StreamCount streamCount; Fn_GetStreamInfo getStreamInfo; Fn_Enable enable;
    Fn_H start, stop, close, pause, resume; Fn_Active isActive; Fn_GetFrame getVideoEx, getAudio; Fn_Time curTime; Fn_Jump jump;
    bool ok = false;
};
Api g_api;
bool g_apiTried = false;
std::string g_loadErr;

struct SymDef { const char* name; const char* nid; void** dst; };

bool load_api()
{
    if (g_apiTried) return g_api.ok;
    g_apiTried = true;
    // Приложение (не payload) грузит системные модули через sceSysmodule, прямой путь в /system ему недоступен.
    int sr = sceSysmoduleLoadModule(ORBIS_SYSMODULE_AV_PLAYER);
    logf_("video: sceSysmoduleLoadModule(AV_PLAYER) = 0x%08X", (unsigned)sr);
    uint32_t mod = 0;
    bool have = false;
    {
        OrbisKernelModule ids[256];
        size_t num = 0;
        int lr = sceKernelGetModuleList(ids, 256, &num);
        logf_("video: module list ret=0x%08X count=%d", (unsigned)lr, (int)num);
        for (size_t i = 0; i < num && i < 256; i++) {
            OrbisKernelModuleInfo info;
            memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetModuleInfo(ids[i], &info) != 0) continue;
            logf_("video: module %d: %s", (int)ids[i], info.name);
            if (strstr(info.name, "AvPlayer")) { mod = (uint32_t)ids[i]; have = true; }
        }
    }
    if (!have) {
        mod = sceKernelLoadStartModule("/system/common/lib/libSceAvPlayer.sprx", 0, NULL, 0, NULL, NULL);
        logf_("video: LoadStartModule(libSceAvPlayer) = 0x%08X", mod);
        if ((int32_t)mod < 0) { g_loadErr = "module"; return false; }
    }
    logf_("video: libSceAvPlayer module id = %d", (int)mod);
    SymDef defs[] = {
        { "sceAvPlayerInit", "aS66RI0gGgo", (void**)&g_api.init },
        { "sceAvPlayerAddSource", "KMcEa+rHsIo", (void**)&g_api.addSource },
        { "sceAvPlayerStreamCount", "hdTyRzCXQeQ", (void**)&g_api.streamCount },
        { "sceAvPlayerGetStreamInfo", "d8FcbzfAdQw", (void**)&g_api.getStreamInfo },
        { "sceAvPlayerEnableStream", "ODJK2sn9w4A", (void**)&g_api.enable },
        { "sceAvPlayerStart", "ET4Gr-Uu07s", (void**)&g_api.start },
        { "sceAvPlayerStop", "ZC17w3vB5Lo", (void**)&g_api.stop },
        { "sceAvPlayerClose", "NkJwDzKmIlw", (void**)&g_api.close },
        { "sceAvPlayerPause", "9y5v+fGN4Wk", (void**)&g_api.pause },
        { "sceAvPlayerResume", "w5moABNwnRY", (void**)&g_api.resume },
        { "sceAvPlayerIsActive", "UbQoYawOsfY", (void**)&g_api.isActive },
        { "sceAvPlayerGetVideoDataEx", "JdksQu8pNdQ", (void**)&g_api.getVideoEx },
        { "sceAvPlayerGetAudioData", "Wnp1OVcrZgk", (void**)&g_api.getAudio },
        { "sceAvPlayerCurrentTime", "wwM99gjFf1Y", (void**)&g_api.curTime },
        { "sceAvPlayerJumpToTime", "XC9wM+xULz8", (void**)&g_api.jump },
    };
    for (auto& d : defs) {
        void* a = NULL;
        int r = sceKernelDlsym((int32_t)mod, d.name, &a);
        if (r != 0 || !a) { a = NULL; r = sceKernelDlsym((int32_t)mod, d.nid, &a); }
        if (r != 0 || !a) { logf_("video: symbol %s not found (0x%08X)", d.name, (unsigned)r); g_loadErr = std::string("symbol ") + d.name; return false; }
        *d.dst = a;
    }
    g_api.ok = true;
    return true;
}

// ---------------------------------------------------------------- память для библиотеки

OrbisPthreadMutex g_memMu;
bool g_memMuInit = false;
struct TexBlock { void* addr; size_t size; off_t phys; bool direct; };
std::vector<TexBlock> g_tex;

void mem_init() { if (!g_memMuInit) { g_memMuInit = true; scePthreadMutexInit(&g_memMu, NULL, "pc_avmem"); } }

void* cb_alloc(void*, uint32_t align, uint32_t size)
{
    if (align < 16) align = 16;
    void* raw = malloc((size_t)size + align + sizeof(void*));
    if (!raw) return NULL;
    uintptr_t a = ((uintptr_t)raw + sizeof(void*) + align - 1) & ~(uintptr_t)(align - 1);
    ((void**)a)[-1] = raw;
    return (void*)a;
}
void cb_free(void*, void* mem) { if (mem) free(((void**)mem)[-1]); }

// Кадры видеодекодера - память прямого доступа (физически непрерывная), доступная и CPU, и GPU/декодеру.
void* cb_alloc_tex(void*, uint32_t align, uint32_t size)
{
    size_t al = std::max<size_t>(align, 0x4000);
    size_t sz = (size + al - 1) & ~(al - 1);
    off_t phys = 0;
    void* addr = NULL;
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, 0 /* WB_ONION: кэшируемая для CPU; тип 3 (WC_GARLIC) читается очень медленно */, &phys);
    if (r == 0) {
        r = sceKernelMapDirectMemory(&addr, sz, 0x33, 0, phys, al);
        if (r == 0 && addr) {
            scePthreadMutexLock(&g_memMu); g_tex.push_back({ addr, sz, phys, true }); scePthreadMutexUnlock(&g_memMu);
            return addr;
        }
        sceKernelReleaseDirectMemory(phys, sz);
    }
    logf_("video: direct memory alloc failed (0x%08X), using malloc", (unsigned)r);
    void* m = cb_alloc(NULL, (uint32_t)al, (uint32_t)sz);
    if (m) { scePthreadMutexLock(&g_memMu); g_tex.push_back({ m, sz, 0, false }); scePthreadMutexUnlock(&g_memMu); }
    return m;
}
void cb_free_tex(void*, void* mem)
{
    if (!mem) return;
    TexBlock b = { NULL, 0, 0, false };
    scePthreadMutexLock(&g_memMu);
    for (size_t i = 0; i < g_tex.size(); i++) if (g_tex[i].addr == mem) { b = g_tex[i]; g_tex.erase(g_tex.begin() + i); break; }
    scePthreadMutexUnlock(&g_memMu);
    if (!b.addr) return;
    if (b.direct) { sceKernelMunmap(b.addr, b.size); sceKernelReleaseDirectMemory(b.phys, b.size); }
    else cb_free(NULL, b.addr);
}

// ---------------------------------------------------------------- чтение файла из службы

// Сетевое чтение выполняет отдельный собственный поток; поток плеера только ставит запрос и ждёт ответ
// (так сокеты не используются из внутренних потоков библиотеки, а данные копируются в её буфер уже готовыми).
struct FileCtx {
    std::string path;
    std::atomic<unsigned long long> size{ 0 };
    std::atomic<int> state{ 0 };          // 0 - свободно, 1 - запрос ждёт, 2 - готово
    std::atomic<int> quit{ 0 };
    unsigned long long reqPos = 0;
    unsigned reqLen = 0;
    long result = -1;
    std::vector<unsigned char> data;
    struct Slot { unsigned long long start = 0; std::vector<unsigned char> data; unsigned long long stamp = 0; };
    Slot slots[4];                        // кэш недавно прочитанных кусков (по 2 МБ): сеть/SMB медленно отвечают на мелкие запросы
    unsigned long long tick = 0;
    OrbisPthreadMutex mu;                 // один запрос за раз
    OrbisPthread th;
    bool thValid = false;
};

void* reader_main(void* arg)
{
    FileCtx* c = (FileCtx*)arg;
    while (!c->quit) {
        if (c->state == 1) {
            c->data.resize(c->reqLen);
            c->result = service_read_range(c->path, c->reqPos, c->reqLen, c->data.data(), NULL);
            c->state = 2;
        } else sceKernelUsleep(500);
    }
    return NULL;
}

int32_t cb_open(void* p, const char* name)
{
    FileCtx* c = (FileCtx*)p;
    logf_("video: file open '%s' (size %llu)", name ? name : "", (unsigned long long)c->size.load());
    return 0;
}
int32_t cb_close(void*) { return 0; }
uint64_t cb_size(void* p) { return ((FileCtx*)p)->size.load(); }
int32_t cb_read(void* p, uint8_t* buf, uint64_t pos, uint32_t len)
{
    FileCtx* c = (FileCtx*)p;
    unsigned long long sz = c->size.load();
    if (sz && pos >= sz) return 0;
    if (sz && pos + len > sz) len = (uint32_t)(sz - pos);
    static std::atomic<int> failLogs{ 0 };
    scePthreadMutexLock(&c->mu);
    int32_t ret = -1;
    // 1) из кэша
    for (auto& s : c->slots) {
        if (!s.data.empty() && pos >= s.start && pos + len <= s.start + s.data.size()) {
            memcpy(buf, s.data.data() + (pos - s.start), len);
            s.stamp = ++c->tick;
            scePthreadMutexUnlock(&c->mu);
            return (int32_t)len;
        }
    }
    // 2) промах: читаем кусок побольше и кладём в самый старый слот
    unsigned want = std::max<unsigned>(len, 2u * 1024 * 1024);
    if (sz && pos + want > sz) want = (unsigned)(sz - pos);
    if (want < len) want = len;
    for (int attempt = 0; attempt < 3 && ret < 0 && !c->quit; attempt++) {
        c->reqPos = pos; c->reqLen = want; c->result = -1;
        c->state = 1;
        for (int i = 0; i < 40000 && c->state != 2 && !c->quit; i++) sceKernelUsleep(500);   // до 20 с
        if (c->state == 2 && c->result >= 0) {
            long got = c->result;
            FileCtx::Slot* v = &c->slots[0];
            for (auto& s : c->slots) if (s.stamp < v->stamp) v = &s;
            v->data.swap(c->data);
            v->data.resize((size_t)got);
            v->start = pos;
            v->stamp = ++c->tick;
            unsigned n = (unsigned)std::min<long>(got, (long)len);
            memcpy(buf, v->data.data(), n);
            ret = (int32_t)n;
        }
        c->state = 0;
    }
    scePthreadMutexUnlock(&c->mu);
    static std::atomic<int> readLogs{ 0 };
    if (ret >= 0 && readLogs++ < 30) logf_("video: read miss pos=%llu len=%u -> %d", (unsigned long long)pos, len, (int)ret);
    if (ret < 0 && failLogs++ < 5) logf_("video: read failed at %llu len %u", (unsigned long long)pos, len);
    return ret;
}


// ---------------------------------------------------------------- разбор MP4: какой кодек у видеодорожки

static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Mp4Track { std::string handler, fourcc; int profile = 0, level = 0, width = 0, height = 0; };

static long long g_mvhdMs = 0, g_mvhdTs = 0;
static uint64_t g_mehdUnits = 0;

static void mp4_parse(const unsigned char* d, size_t n, std::vector<Mp4Track>& out, Mp4Track* cur, int depth)
{
    size_t p = 0;
    while (p + 8 <= n) {
        uint64_t sz = be32(d + p);
        const unsigned char* t = d + p + 4;
        size_t hdr = 8;
        if (sz == 1 && p + 16 <= n) { sz = ((uint64_t)be32(d + p + 8) << 32) | be32(d + p + 12); hdr = 16; }
        if (sz == 0) sz = n - p;
        if (sz < hdr || p + sz > n) sz = n - p;
        const unsigned char* b = d + p + hdr;
        size_t bl = (size_t)sz - hdr;
        if (!memcmp(t, "trak", 4) && depth < 4) {
            Mp4Track tr;
            mp4_parse(b, bl, out, &tr, depth + 1);
            out.push_back(tr);
        } else if ((!memcmp(t, "moov", 4) || !memcmp(t, "mvex", 4) || !memcmp(t, "mdia", 4) || !memcmp(t, "minf", 4) || !memcmp(t, "stbl", 4)) && depth < 8) {
            mp4_parse(b, bl, out, cur, depth + 1);
        } else if (!memcmp(t, "mvhd", 4) && bl >= 32) {
            uint64_t ts, du;
            if (b[0] == 1) { ts = be32(b + 20); du = ((uint64_t)be32(b + 24) << 32) | be32(b + 28); }
            else { ts = be32(b + 12); du = be32(b + 16); }
            g_mvhdTs = (long long)ts;
            if (ts) g_mvhdMs = (long long)(du * 1000ULL / ts);
        } else if (!memcmp(t, "mehd", 4) && bl >= 8) {                 // фрагментированный MP4: полная длительность лежит здесь
            uint64_t du = b[0] == 1 && bl >= 12 ? (((uint64_t)be32(b + 4) << 32) | be32(b + 8)) : be32(b + 4);
            g_mehdUnits = du;
        } else if (cur && !memcmp(t, "hdlr", 4) && bl >= 12) {
            cur->handler.assign((const char*)b + 8, 4);
        } else if (cur && !memcmp(t, "stsd", 4) && bl >= 16 + 86) {
            const unsigned char* e = b + 8;                  // первая запись описания
            cur->fourcc.assign((const char*)e + 4, 4);
            cur->width = (int)((e[32] << 8) | e[33]);
            cur->height = (int)((e[34] << 8) | e[35]);
            size_t el = std::min<size_t>(be32(e), bl - 8);
            size_t q = 86;
            while (q + 8 <= el) {
                uint32_t cs = be32(e + q);
                if (cs < 8 || q + cs > el) break;
                if (!memcmp(e + q + 4, "avcC", 4) && cs >= 12) { cur->profile = e[q + 8 + 1]; cur->level = e[q + 8 + 3]; }
                q += cs;
            }
        }
        p += (size_t)sz;
    }
}

// Возвращает описание видеодорожки ("H.264 High L4.1 1920x1080", "H.265/HEVC 3840x2160"...); пусто - видеодорожки нет/не разобрать.
static std::string mp4_video_info(const std::string& path, std::string* codecOnly)
{
    unsigned long long pos = 0, total = 0;
    unsigned char hd[16];
    for (int guard = 0; guard < 64; guard++) {
        long n = service_read_range(path, pos, 16, hd, &total);
        if (n < 8) return "";
        uint64_t sz = be32(hd);
        if (sz == 1 && n >= 16) sz = ((uint64_t)be32(hd + 8) << 32) | be32(hd + 12);
        if (sz == 0) sz = total - pos;
        if (!memcmp(hd + 4, "moov", 4)) {
            if (sz < 16 || sz > 16u * 1024 * 1024) return "";
            std::vector<unsigned char> buf((size_t)sz);
            size_t got = 0;
            while (got < buf.size()) {
                unsigned want = (unsigned)std::min<size_t>(buf.size() - got, 1024 * 1024);
                long r = service_read_range(path, pos + got, want, buf.data() + got, NULL);
                if (r <= 0) return "";
                got += (size_t)r;
            }
            std::vector<Mp4Track> tracks;
            mp4_parse(buf.data(), buf.size(), tracks, nullptr, 0);
            for (auto& t : tracks) {
                if (t.handler != "vide") continue;
                std::string codec = t.fourcc;
                if (t.fourcc == "avc1" || t.fourcc == "avc3") {
                    const char* pn = t.profile == 66 ? "Baseline" : t.profile == 77 ? "Main" : t.profile == 88 ? "Extended" : t.profile == 100 ? "High" :
                                     t.profile == 110 ? "High 10 (10 bit)" : t.profile == 122 ? "High 4:2:2" : t.profile == 244 ? "High 4:4:4" : "";
                    char b[96];
                    snprintf(b, sizeof(b), "H.264 %s L%d.%d", pn, t.level / 10, t.level % 10);
                    codec = b;
                } else if (t.fourcc == "hvc1" || t.fourcc == "hev1") codec = "H.265/HEVC";
                else if (t.fourcc == "vp09") codec = "VP9";
                else if (t.fourcc == "av01") codec = "AV1";
                else if (t.fourcc == "mp4v") codec = "MPEG-4 Part 2";
                if (codecOnly) *codecOnly = codec;
                char w[48];
                snprintf(w, sizeof(w), " %dx%d", t.width, t.height);
                return codec + w;
            }
            return "";
        }
        if (sz < 8 || pos + sz >= total) return "";
        pos += sz;
    }
    return "";
}

// ---------------------------------------------------------------- сессия

struct Session;
void cb_event(void* p, int32_t ev, int32_t srcId, void* data);

struct Session {
    std::atomic<int> ready{ 0 };            // библиотека сообщила, что источник готов (StateReady)
    std::string path;
    int userId = 0;
    FileCtx file;
    std::atomic<int> stop{ 0 };
    std::atomic<int> paused{ 0 };
    std::atomic<int> state{ 1 };
    std::atomic<long long> durMs{ 0 };
    std::atomic<int> hasVideo{ 0 };
    void* player = nullptr;
    std::string err;
    std::string note;                       // пояснение для экрана (например, кодек, который плеер консоли не умеет)
    OrbisPthread th;                        // поток этой сессии
    Session* prev = nullptr;                // предыдущая (закрытая) сессия: её поток дожидаемся и освобождаем уже в своём потоке
    std::atomic<int> done{ 0 };             // поток сессии полностью завершился
    bool audioLogged = false;
    std::atomic<int> seekMs{ 0 };           // просьба о перемотке от интерфейса (выполняет поток сессии)
    std::atomic<int> jumpDone{ 1 };         // плеер сообщил (событие 32), что предыдущая перемотка выполнена
    bool pausedApplied = false;             // какое состояние паузы уже передано плееру
};
void cb_event(void* p, int32_t ev, int32_t srcId, void*)
{
    Session* S = (Session*)p;
    logf_("video: event %d src %d", (int)ev, (int)srcId);
    if (S && ev == 2) S->ready = 1;
    if (S && ev == 32) S->jumpDone = 1;
}

Session* g_s = nullptr;            // текущая сессия (то, что видит интерфейс)
Session* g_tail = nullptr;         // закрытая сессия, которая ещё завершается в фоне (интерфейс её не ждёт)
Picture g_pic;                    // то, что рисует главный поток
Picture g_back;                   // готовый кадр от потока декодирования (под мьютексом)
OrbisPthreadMutex g_picMu;
bool g_picMuInit = false;
std::atomic<int> g_newFrame{ 0 };
OrbisPthread g_vth;
bool g_vthValid = false;
std::atomic<int> g_vstop{ 0 };

alignas(256) int16_t g_aout[1024 * 2];

void sleep_ms(int ms) { sceKernelUsleep((unsigned)ms * 1000u); }

void* video_main_inner(Session* S);

void* vdec_main(void* arg);

void* video_main(void* arg)
{
    Session* S = (Session*)arg;
    // Сначала дожидаемся предыдущей сессии: две сессии системного плеера и два звуковых порта одновременно не держим.
    if (S->prev) {
        // Если прошлая сессия застряла внутри системного плеера, ждём её недолго и идём дальше (её память оставляем)
        for (int i = 0; i < 250 && !S->prev->done; i++) sceKernelUsleep(20000);
        if (S->prev->done) { scePthreadJoin(S->prev->th, NULL); delete S->prev; }
        else logf_("video: previous session is stuck, abandoned");
        S->prev = nullptr;
    }
    void* r = NULL;
    if (!S->stop) {
        g_newFrame = 0;
        r = video_main_inner(S);
    }
    S->file.quit = 1;
    if (S->file.thValid) scePthreadJoin(S->file.th, NULL);
    S->file.thValid = false;
    S->done = 1;
    return r;
}

void* video_main_inner(Session* S)
{
    mem_init();
    if (!load_api()) { S->err = g_loadErr; S->state = 4; return NULL; }

    // размер файла - из Content-Range первого запроса
    unsigned char probe[1];
    unsigned long long total = 0;
    long pr = service_read_range(S->path, 0, 1, probe, &total);
    if (pr < 0 || total == 0) { S->err = "noconn"; S->state = 4; return NULL; }
    if (S->stop) return NULL;
    S->file.size = total;
    S->file.path = S->path;
    scePthreadMutexInit(&S->file.mu, NULL, "pc_avrd");
    {
        OrbisPthreadAttr ra;
        scePthreadAttrInit(&ra);
        scePthreadAttrSetstacksize(&ra, 1024 * 1024);
        S->file.thValid = scePthreadCreate(&S->file.th, &ra, reader_main, &S->file, "pc_avread") == 0;
        scePthreadAttrDestroy(&ra);
    }
    logf_("video: file '%s' size %llu", S->path.c_str(), total);
    g_mvhdMs = 0; g_mvhdTs = 0; g_mehdUnits = 0;
    {   // список коробок верхнего уровня - для диагностики (moov в конце? фрагменты moof?)
        unsigned long long bp = 0; unsigned char bh[16]; std::string bl;
        for (int k = 0; k < 10 && bp < total; k++) {
            if (service_read_range(S->path, bp, 16, bh, NULL) < 8) break;
            uint64_t bs = be32(bh);
            if (bs == 1) bs = ((uint64_t)be32(bh + 8) << 32) | be32(bh + 12);
            if (bs == 0) bs = total - bp;
            char one[48]; snprintf(one, sizeof(one), " %.4s@%llu(%llu)", (const char*)bh + 4, bp, (unsigned long long)bs);
            bl += one;
            if (bs < 8) break;
            bp += bs;
        }
        logf_("video: mp4 boxes:%s", bl.c_str());
    }
    std::string vinfo = mp4_video_info(S->path, &S->note);
    if (g_mvhdMs <= 0 && g_mehdUnits > 0 && g_mvhdTs > 0) g_mvhdMs = (long long)(g_mehdUnits * 1000ULL / (uint64_t)g_mvhdTs);
    logf_("video: mvhd timescale=%lld duration=%lld ms, mehd=%llu", g_mvhdTs, g_mvhdMs, (unsigned long long)g_mehdUnits);
    if (g_mvhdMs > 0) { S->durMs = g_mvhdMs; logf_("video: mp4 duration %lld ms", g_mvhdMs); }
    logf_("video: mp4 video track: %s", vinfo.empty() ? "none / not parsed" : vinfo.c_str());
    if (S->stop) return NULL;

    AvInit init;
    memset(&init, 0, sizeof(init));
    init.mem = { NULL, cb_alloc, cb_free, cb_alloc_tex, cb_free_tex };
    init.file = { &S->file, cb_open, cb_close, cb_read, cb_size };
    init.event = { S, (void*)cb_event };
    init.debugLevel = 0;
    init.basePriority = 700;
    init.numFrameBuffers = 6;     // потоки с большим числом опорных кадров/B-кадров (например 4 ref) при 3 буферах не выдают ни одного кадра
    init.autoStart = false;       // с автостартом плеер переходит в Play до того, как мы включим потоки, и тогда EnableStream отказывает (0x806A0002)
    init.defaultLanguage = "eng";
    void* h = g_api.init(&init);
    logf_("video: sceAvPlayerInit = %p", h);
    if (!h) { S->err = "init"; S->state = 4; return NULL; }
    S->player = h;

    int r = g_api.addSource(h, S->path.c_str());
    logf_("video: sceAvPlayerAddSource = 0x%08X", (unsigned)r);
    if (r < 0) { S->err = "source"; S->state = 4; S->player = nullptr; g_api.close(h); return NULL; }

    int n = 0;
    for (int i = 0; i < 300 && !S->stop; i++) {            // ждём до 15 с, пока библиотека разберёт файл
        n = g_api.streamCount(h);
        if (n > 0) break;
        sleep_ms(50);
    }
    logf_("video: streams = %d", n);
    if (n <= 0 || S->stop) {
        if (!S->stop) { S->err = "nostreams"; S->state = 4; }
        S->player = nullptr;
        g_api.stop(h); g_api.close(h);
        return NULL;
    }

    // Стримы включаем, когда библиотека готова (событие StateReady); если событие не пришло - всё равно пробуем,
    // а неудачные включения повторяем: сразу после разбора файла библиотека иногда отвечает ошибкой 0x806A0002.
    bool vEn = false, aEn = false;
    bool vTried = false, aTried = false;
    for (int i = 0; i < n && !S->stop; i++) {
        alignas(16) unsigned char raw[256];
        memset(raw, 0, sizeof(raw));
        int gr = g_api.getStreamInfo(h, (uint32_t)i, raw);
        AvStreamInfo* si = (AvStreamInfo*)raw;
        logf_("video: stream %d: ret=0x%08X type=%u duration=%llu ms", i, (unsigned)gr, si->type, (unsigned long long)si->duration);
        if (gr < 0) continue;
        bool isV = si->type == 0 && !vTried, isA = si->type == 1 && !aTried;
        if (!isV && !isA) continue;
        if (isV) { vTried = true; logf_("video: video stream %ux%u", *(uint32_t*)&si->details[0], *(uint32_t*)&si->details[4]); }
        else { aTried = true; logf_("video: audio stream channels=%u rate=%u", (unsigned)*(uint16_t*)&si->details[0], *(uint32_t*)&si->details[4]); }
        if (si->duration && (isV || !S->durMs)) S->durMs = (long long)si->duration;
        int er = -1;
        for (int t = 0; t < 100 && !S->stop; t++) {
            er = g_api.enable(h, (uint32_t)i);
            if (er >= 0) { logf_("video: enable stream %d ok (try %d)", i, t + 1); break; }
            if (t == 0) logf_("video: enable stream %d = 0x%08X, retrying", i, (unsigned)er);
            sleep_ms(100);
        }
        if (er < 0) logf_("video: enable stream %d failed for good (0x%08X)", i, (unsigned)er);
        if (isV) { vEn = er >= 0; S->hasVideo = vEn ? 1 : 0; } else aEn = er >= 0;
    }
    if (vEn) S->note.clear();
    if ((!vEn && !aEn) || S->stop) {
        if (!S->stop) { S->err = "enable"; S->state = 4; }
        S->player = nullptr;
        g_api.stop(h); g_api.close(h);
        return NULL;
    }
    sleep_ms(100);
    { int sr = g_api.start(h); logf_("video: sceAvPlayerStart = 0x%08X, active=%d", (unsigned)sr, (int)g_api.isActive(h)); }
    S->state = 2;
    if (S->hasVideo) {
        if (!g_picMuInit) { g_picMuInit = true; scePthreadMutexInit(&g_picMu, NULL, "pc_pic"); }
        g_vstop = 0; g_newFrame = 0;
        OrbisPthreadAttr va;
        scePthreadAttrInit(&va);
        scePthreadAttrSetstacksize(&va, 1024 * 1024);
        g_vthValid = scePthreadCreate(&g_vth, &va, vdec_main, S, "pc_vdec") == 0;
        scePthreadAttrDestroy(&va);
    }

    // --- звук: забираем PCM у плеера и отдаём в AudioOut (48 кГц, стерео)
    int ao = -1;
    std::vector<int16_t> q;     // накопленные стереокадры 48 кГц
    size_t qpos = 0;
    uint64_t idleSince = 0, jumpAt = 0, aoRetryAt = 0;
    int aoFails = 0;
    int ticks = 0;
    while (!S->stop) {
        // пауза и перемотка выполняются здесь, а не в потоке интерфейса: вызовы плеера могут задерживаться
        bool wantPause = S->paused != 0;
        if (wantPause != S->pausedApplied) {
            int pr = wantPause ? g_api.pause(h) : g_api.resume(h);
            if (pr < 0) logf_("video: %s = 0x%08X", wantPause ? "pause" : "resume", (unsigned)pr);
            S->pausedApplied = wantPause;
        }
        // новый прыжок - только когда предыдущий завершён (или прошло 3 с), иначе нажатия копятся и прыжки не накладываются
        uint64_t tnow = sceKernelGetProcessTime();
        int sk = 0;
        if (S->seekMs.load() != 0 && (S->jumpDone || tnow - jumpAt > 3000000ULL)) sk = S->seekMs.exchange(0);
        if (sk != 0) {
            S->jumpDone = 0; jumpAt = tnow;
            long long cur = (long long)g_api.curTime(h) + sk;
            long long dur = S->durMs;
            if (dur > 0 && cur >= dur) cur = dur - 500;
            if (cur < 0) cur = 0;
            int jr = g_api.jump(h, (uint64_t)cur);
            logf_("video: jump to %lld ms = 0x%08X", cur, (unsigned)jr);
            q.clear(); qpos = 0;              // звук, накопленный до перемотки, больше не нужен
            idleSince = 0;
        }
        bool got = false;
        if (aEn && (q.size() - qpos) / 2 < 8192) {
            alignas(16) unsigned char raw[256];
            memset(raw, 0, sizeof(raw));
            AvFrame* af = (AvFrame*)raw;
            if (g_api.getAudio(h, af) && af->data) {
                got = true;
                uint16_t ch = *(uint16_t*)&af->details[0];
                uint32_t rate = *(uint32_t*)&af->details[4], bytes = *(uint32_t*)&af->details[8];
                if (!S->audioLogged) { S->audioLogged = true; logf_("video: first audio frame ch=%u rate=%u bytes=%u", (unsigned)ch, rate, bytes); }
                if (ch >= 1 && rate >= 8000 && bytes >= 2u * ch) {
                    const int16_t* pcm = (const int16_t*)af->data;
                    size_t frames = bytes / (2u * ch);
                    if (qpos > 65536) { q.erase(q.begin(), q.begin() + (long)qpos); qpos = 0; }
                    double step = (double)rate / 48000.0;
                    size_t outFrames = (size_t)((double)frames / step);
                    for (size_t o = 0; o < outFrames; o++) {
                        size_t i0 = (size_t)(o * step); if (i0 >= frames) i0 = frames - 1;
                        q.push_back(pcm[i0 * ch]);
                        q.push_back(pcm[i0 * ch + (ch > 1 ? 1 : 0)]);
                    }
                }
            }
        }
        if (aEn) {
            if (ao < 0 && sceKernelGetProcessTime() >= aoRetryAt) {
                ao = audio_shared_port();
                if (aoFails++ < 3 || ao >= 0) logf_("video: audio port = 0x%08X", (unsigned)ao);
                if (ao < 0) aoRetryAt = sceKernelGetProcessTime() + 2000000ULL;
            }
            if (ao >= 0) {
                if ((q.size() - qpos) / 2 >= 1024) {
                    memcpy(g_aout, q.data() + qpos, sizeof(g_aout));
                    qpos += 2048;
                } else if (!got) {
                    memset(g_aout, 0, sizeof(g_aout));
                } else continue;
                sceAudioOutOutput(ao, g_aout);
            } else if (!got) sleep_ms(10);
        } else sleep_ms(20);

        // конец воспроизведения: плеер перестал быть активным (на паузе не считаем)
        if (++ticks % 8 == 0 && !S->paused) {
            if (!g_api.isActive(h)) {
                uint64_t now = sceKernelGetProcessTime();
                if (!idleSince) idleSince = now;
                if (now - idleSince > 2000000ULL && (q.size() - qpos) < 2048) { S->state = 3; }
            } else idleSince = 0;
        }
    }
    logf_("video: stopping");
    g_vstop = 1;
    if (g_vthValid) scePthreadJoin(g_vth, NULL);
    g_vthValid = false;
    S->player = nullptr;
    g_api.stop(h);
    g_api.close(h);
    sleep_ms(300);                         // дать внутренним потокам библиотеки завершиться
    return NULL;
}

} // namespace

// ---------------------------------------------------------------- внешний интерфейс

bool video_open(const std::string& path, int userId)
{
    video_close();
    Session* S = new Session();
    S->path = path;
    S->userId = userId;
    S->prev = g_tail;
    g_tail = nullptr;
    g_pic = Picture();
    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 2 * 1024 * 1024);
    int r = scePthreadCreate(&S->th, &attr, video_main, S, "pc_video");
    scePthreadAttrDestroy(&attr);
    if (r != 0) {
        logf_("video: thread create failed 0x%08X", (unsigned)r);
        g_tail = S->prev;
        delete S;
        return false;
    }
    g_s = S;
    return true;
}

// Закрытие не ждёт поток: системный плеер может секундами разбирать файл или ждать сеть, а интерфейс зависать не должен.
// Сессия завершается в фоне; следующая сессия (или выход из приложения) дождётся её сама.
void video_close()
{
    if (!g_s) return;
    g_s->stop = 1;
    g_s->file.quit = 1;          // прерывает ожидание чтения в колбэке плеера
    g_tail = g_s;
    g_s = nullptr;
    g_pic = Picture();
}

void video_shutdown(int maxMs)
{
    video_close();
    for (int i = 0; g_tail && !g_tail->done && i < maxMs / 20; i++) sceKernelUsleep(20000);
}

int video_state() { return g_s ? (int)g_s->state : 0; }
std::string video_note() { return g_s ? g_s->note : ""; }
std::string video_error() { return g_s ? g_s->err : ""; }
bool video_has_video() { return g_s && g_s->hasVideo; }
const Picture& video_picture() { return g_pic; }
bool video_paused() { return g_s && g_s->paused; }
double video_total() { return g_s ? (double)g_s->durMs / 1000.0 : 0; }

double video_seconds()
{
    void* p = g_s ? g_s->player : nullptr;
    if (!p || g_s->state != 2) return 0;
    return (double)g_api.curTime(p) / 1000.0;
}

void video_pause_toggle()
{
    if (!g_s || g_s->state != 2) return;
    g_s->paused = g_s->paused ? 0 : 1;
}

bool video_seekable() { return g_s && g_s->durMs > 0; }

void video_seek(int deltaMs)
{
    if (!g_s || g_s->state != 2 || g_s->durMs <= 0) return;
    g_s->seekMs += deltaMs;
}

// Главный поток только забирает готовый кадр.
bool video_poll_frame()
{
    if (!g_s || g_s->state != 2 || !g_picMuInit || !g_newFrame.load()) return false;
    scePthreadMutexLock(&g_picMu);
    g_pic.px.swap(g_back.px);
    g_pic.w = g_back.w; g_pic.h = g_back.h;
    g_newFrame = 0;
    scePthreadMutexUnlock(&g_picMu);
    return true;
}

namespace {

// Поток декодирования: забирает кадры NV12 у плеера и переводит в RGB (масштаб "по ближайшему" под 1600x900).
void* vdec_main(void* arg)
{
    Session* S = (Session*)arg;
    std::vector<uint32_t> out;
    std::vector<int> xs;
    std::vector<uint8_t> frameCopy;
    bool logged = false, noFrameLogged = false;
    int frames = 0;
    uint64_t t0 = sceKernelGetProcessTime(), convUs = 0;
    while (!g_vstop && !S->stop) {
        void* p = S->player;
        if (!p) break;
        alignas(16) unsigned char raw[sizeof(AvFrameEx) + 256];
        memset(raw, 0, sizeof(raw));
        AvFrameEx* f = (AvFrameEx*)raw;
        if (!g_api.getVideoEx(p, f) || !f->data) {
            if (!logged && !noFrameLogged && sceKernelGetProcessTime() - t0 > 6000000ULL) {
                noFrameLogged = true;
                logf_("video: no video frame after 6 s (active=%d, time=%llu ms)", (int)g_api.isActive(p), (unsigned long long)g_api.curTime(p));
            }
            sleep_ms(3); continue;
        }

        uint32_t W = f->video.width, H = f->video.height, pitch = f->video.pitch;
        if (!logged) {
            logged = true;
            logf_("video: first frame %ux%u pitch=%u crop L%u R%u T%u B%u bits=%u/%u full=%d framerate=%u data=%p",
                  W, H, pitch, f->video.cropL, f->video.cropR, f->video.cropT, f->video.cropB,
                  (unsigned)f->video.lumaBits, (unsigned)f->video.chromaBits, (int)f->video.fullRange, f->video.framerate, f->data);
        }
        if (W < 16 || H < 16 || W > 4096 || H > 4096) continue;
        if (pitch < W) pitch = W;
        // width/height - размеры кадра декодера (выровнены по 16); видимая часть задаётся обрезкой:
        // справа она отсчитывается от pitch, снизу - от height (по логу: 854x476 в кадре 864x480, pitch 896, R42 B4).
        uint32_t codedH = (H + 15) & ~15u;
        uint32_t visW = W, visH = H;
        {
            uint32_t cl = f->video.cropL, cr = f->video.cropR, ct = f->video.cropT, cb = f->video.cropB;
            if (cl == 0 && ct == 0) {
                if (cr < pitch && pitch - cr <= W && pitch - cr >= W / 2) visW = pitch - cr;
                if (cb < H && H - cb >= H / 2) visH = H - cb;
            }
        }
        // Сначала целиком копируем кадр в обычную память: чтение декодерной памяти байт за байтом было в сотни раз медленнее
        // (около 500 мс на кадр 720p в логе) и кадр мог переиспользоваться декодером во время перевода в RGB.
        size_t lumaBytes = (size_t)pitch * codedH, chromaBytes = (size_t)pitch * ((H + 1) / 2);
        frameCopy.resize(lumaBytes + chromaBytes);
        memcpy(frameCopy.data(), f->data, lumaBytes + chromaBytes);
        const uint8_t* Y = frameCopy.data();
        const uint8_t* C = Y + lumaBytes;

        uint64_t c0 = sceKernelGetProcessTime();
        double k = std::min(1920.0 / visW, 1080.0 / visH);   // на весь экран с сохранением пропорций (увеличение разрешено)
        int dw = std::max(2, (int)(visW * k)) & ~1, dh = std::max(2, (int)(visH * k)) & ~1;
        out.resize((size_t)dw * dh);
        xs.resize((size_t)dw);
        for (int x = 0; x < dw; x++) xs[(size_t)x] = (int)((uint64_t)x * visW / dw);
        bool hd = visH >= 720;
        int cRV = hd ? 459 : 409, cGU = hd ? 55 : 100, cGV = hd ? 136 : 208, cBU = hd ? 541 : 516;
        bool full = f->video.fullRange;
        for (int y = 0; y < dh; y++) {
            uint32_t sy = (uint32_t)((uint64_t)y * visH / dh);
            const uint8_t* yr = Y + (size_t)sy * pitch;
            const uint8_t* cr_ = C + (size_t)(sy >> 1) * pitch;
            uint32_t* o = &out[(size_t)y * dw];
            for (int x = 0; x < dw; x++) {
                int sx = xs[(size_t)x];
                int yy = full ? ((int)yr[sx] * 256) : (((int)yr[sx] - 16) * 298);
                int ci = sx & ~1;
                int u = (int)cr_[ci] - 128, v = (int)cr_[ci + 1] - 128;
                int R = (yy + cRV * v + 128) >> 8;
                int G = (yy - cGU * u - cGV * v + 128) >> 8;
                int B = (yy + cBU * u + 128) >> 8;
                R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
                o[x] = ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
            }
        }
        convUs += sceKernelGetProcessTime() - c0;
        scePthreadMutexLock(&g_picMu);
        g_back.px.swap(out);
        g_back.w = dw; g_back.h = dh;
        g_newFrame = 1;
        scePthreadMutexUnlock(&g_picMu);
        frames++;
        uint64_t now = sceKernelGetProcessTime();
        if (now - t0 >= 5000000ULL) {
            logf_("video: %d frames in %.1f s, convert %.1f ms/frame", frames, (now - t0) / 1e6, frames ? convUs / 1000.0 / frames : 0.0);
            frames = 0; convUs = 0; t0 = now;
        }
    }
    return NULL;
}

} // namespace

