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
    int r = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), sz, al, 3, &phys);
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
    for (int attempt = 0; attempt < 3 && ret < 0 && !c->quit; attempt++) {
        c->reqPos = pos; c->reqLen = len; c->result = -1;
        c->state = 1;
        for (int i = 0; i < 40000 && c->state != 2 && !c->quit; i++) sceKernelUsleep(500);   // до 20 с
        if (c->state == 2 && c->result >= 0) { memcpy(buf, c->data.data(), (size_t)c->result); ret = (int32_t)c->result; }
        c->state = 0;
    }
    scePthreadMutexUnlock(&c->mu);
    if (ret < 0 && failLogs++ < 5) logf_("video: read failed at %llu len %u", (unsigned long long)pos, len);
    return ret;
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
};
void cb_event(void* p, int32_t ev, int32_t srcId, void*)
{
    Session* S = (Session*)p;
    logf_("video: event %d src %d", (int)ev, (int)srcId);
    if (S && ev == 2) S->ready = 1;
}

Session* g_s = nullptr;
OrbisPthread g_th;
bool g_thValid = false;
Picture g_pic;
std::vector<uint32_t> g_scratch;
bool g_loggedFrame = false;

alignas(256) int16_t g_aout[1024 * 2];

void sleep_ms(int ms) { sceKernelUsleep((unsigned)ms * 1000u); }

void* video_main_inner(Session* S);

void* video_main(void* arg)
{
    Session* S = (Session*)arg;
    void* r = video_main_inner(S);
    S->file.quit = 1;
    if (S->file.thValid) scePthreadJoin(S->file.th, NULL);
    S->file.thValid = false;
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

    AvInit init;
    memset(&init, 0, sizeof(init));
    init.mem = { NULL, cb_alloc, cb_free, cb_alloc_tex, cb_free_tex };
    init.file = { &S->file, cb_open, cb_close, cb_read, cb_size };
    init.event = { S, (void*)cb_event };
    init.debugLevel = 0;
    init.basePriority = 700;
    init.numFrameBuffers = 3;
    init.autoStart = true;
    init.defaultLanguage = "eng";
    void* h = g_api.init(&init);
    logf_("video: sceAvPlayerInit = %p", h);
    if (!h) { S->err = "init"; S->state = 4; return NULL; }
    S->player = h;

    int r = g_api.addSource(h, S->path.c_str());
    logf_("video: sceAvPlayerAddSource = 0x%08X", (unsigned)r);
    if (r < 0) { S->err = "source"; S->state = 4; g_api.close(h); S->player = nullptr; return NULL; }

    int n = 0;
    for (int i = 0; i < 300 && !S->stop; i++) {            // ждём до 15 с, пока библиотека разберёт файл
        n = g_api.streamCount(h);
        if (n > 0) break;
        sleep_ms(50);
    }
    logf_("video: streams = %d", n);
    if (n <= 0 && !S->stop) { S->err = "nostreams"; S->state = 4; g_api.stop(h); g_api.close(h); S->player = nullptr; return NULL; }

    // Стримы включаем, когда библиотека готова (событие StateReady); если событие не пришло - всё равно пробуем,
    // а неудачные включения повторяем: сразу после разбора файла библиотека иногда отвечает ошибкой 0x806A0002.
    for (int i = 0; i < 100 && !S->ready && !S->stop; i++) sleep_ms(50);
    logf_("video: ready=%d", (int)S->ready.load());
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
        for (int t = 0; t < 40 && !S->stop; t++) {
            er = g_api.enable(h, (uint32_t)i);
            if (er >= 0) { logf_("video: enable stream %d ok (try %d)", i, t + 1); break; }
            if (t == 0) logf_("video: enable stream %d = 0x%08X, retrying", i, (unsigned)er);
            sleep_ms(250);
        }
        if (er < 0) logf_("video: enable stream %d failed for good (0x%08X)", i, (unsigned)er);
        if (isV) { vEn = er >= 0; S->hasVideo = vEn ? 1 : 0; } else aEn = er >= 0;
    }
    if (!vEn && !aEn && !S->stop) { S->err = "enable"; S->state = 4; g_api.stop(h); g_api.close(h); S->player = nullptr; return NULL; }
    sleep_ms(100);
    if (!g_api.isActive(h)) { int sr = g_api.start(h); logf_("video: sceAvPlayerStart = 0x%08X", (unsigned)sr); }
    S->state = 2;

    // --- звук: забираем PCM у плеера и отдаём в AudioOut (48 кГц, стерео)
    int ao = -1;
    std::vector<int16_t> q;     // накопленные стереокадры 48 кГц
    size_t qpos = 0;
    uint64_t idleSince = 0;
    int ticks = 0;
    while (!S->stop) {
        bool got = false;
        if (aEn && (q.size() - qpos) / 2 < 8192) {
            alignas(16) unsigned char raw[256];
            memset(raw, 0, sizeof(raw));
            AvFrame* af = (AvFrame*)raw;
            if (g_api.getAudio(h, af) && af->data) {
                got = true;
                uint16_t ch = *(uint16_t*)&af->details[0];
                uint32_t rate = *(uint32_t*)&af->details[4], bytes = *(uint32_t*)&af->details[8];
                static bool logged = false;
                if (!logged) { logged = true; logf_("video: first audio frame ch=%u rate=%u bytes=%u", (unsigned)ch, rate, bytes); }
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
            if (ao < 0) {
                sceAudioOutInit();
                ao = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1024, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
                logf_("video: sceAudioOutOpen = 0x%08X", (unsigned)ao);
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
    g_api.stop(h);
    if (ao >= 0) sceAudioOutClose(ao);
    g_api.close(h);
    S->player = nullptr;
    return NULL;
}

} // namespace

// ---------------------------------------------------------------- внешний интерфейс

bool video_open(const std::string& path, int userId)
{
    video_close();
    g_s = new Session();
    g_s->path = path;
    g_s->userId = userId;
    g_pic = Picture();
    g_loggedFrame = false;
    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 2 * 1024 * 1024);
    int r = scePthreadCreate(&g_th, &attr, video_main, g_s, "pc_video");
    scePthreadAttrDestroy(&attr);
    if (r != 0) { logf_("video: thread create failed 0x%08X", (unsigned)r); delete g_s; g_s = nullptr; return false; }
    g_thValid = true;
    return true;
}

void video_close()
{
    if (!g_s) return;
    g_s->stop = 1;
    if (g_thValid) scePthreadJoin(g_th, NULL);
    g_thValid = false;
    delete g_s;
    g_s = nullptr;
    g_pic = Picture();
}

int video_state() { return g_s ? (int)g_s->state : 0; }
std::string video_error() { return g_s ? g_s->err : ""; }
bool video_has_video() { return g_s && g_s->hasVideo; }
const Picture& video_picture() { return g_pic; }
bool video_paused() { return g_s && g_s->paused; }
double video_total() { return g_s ? (double)g_s->durMs / 1000.0 : 0; }

double video_seconds()
{
    if (!g_s || !g_s->player || g_s->state != 2) return 0;
    return (double)g_api.curTime(g_s->player) / 1000.0;
}

void video_pause_toggle()
{
    if (!g_s || !g_s->player || g_s->state != 2) return;
    if (g_s->paused) { g_api.resume(g_s->player); g_s->paused = 0; }
    else { g_api.pause(g_s->player); g_s->paused = 1; }
}

void video_seek(int deltaMs)
{
    if (!g_s || !g_s->player || g_s->state != 2) return;
    long long cur = (long long)g_api.curTime(g_s->player) + deltaMs;
    if (cur < 0) cur = 0;
    if (g_s->durMs > 0 && cur >= g_s->durMs) cur = g_s->durMs - 500;
    if (cur < 0) cur = 0;
    int r = g_api.jump(g_s->player, (uint64_t)cur);
    logf_("video: jump to %lld ms = 0x%08X", cur, (unsigned)r);
}

// NV12 -> RGB с масштабированием "по ближайшему" под область 1600x900.
bool video_poll_frame()
{
    if (!g_s || g_s->state != 2 || !g_s->player || !g_s->hasVideo) return false;
    alignas(16) unsigned char raw[sizeof(AvFrameEx) + 256];
    memset(raw, 0, sizeof(raw));
    AvFrameEx* f = (AvFrameEx*)raw;
    if (!g_api.getVideoEx(g_s->player, f) || !f->data) return false;

    uint32_t W = f->video.width, H = f->video.height, pitch = f->video.pitch;
    if (!g_loggedFrame) {
        g_loggedFrame = true;
        logf_("video: first frame %ux%u pitch=%u crop L%u R%u T%u B%u bits=%u/%u full=%d framerate=%u data=%p",
              W, H, pitch, f->video.cropL, f->video.cropR, f->video.cropT, f->video.cropB,
              (unsigned)f->video.lumaBits, (unsigned)f->video.chromaBits, (int)f->video.fullRange, f->video.framerate, f->data);
    }
    if (W < 16 || H < 16 || W > 4096 || H > 4096) return false;
    if (pitch < W) pitch = W;
    uint32_t cl = f->video.cropL, cr = f->video.cropR, ct = f->video.cropT, cb = f->video.cropB;
    if (cl + cr >= W || ct + cb >= H) { cl = cr = ct = cb = 0; }
    uint32_t vw = W - cl - cr, vh = H - ct - cb;

    const uint8_t* Y = (const uint8_t*)f->data;
    const uint8_t* C = Y + (size_t)pitch * H;

    double k = std::min(1600.0 / vw, 900.0 / vh);
    if (k > 1.0) k = 1.0;
    int dw = std::max(2, (int)(vw * k)) & ~1, dh = std::max(2, (int)(vh * k)) & ~1;
    g_scratch.resize((size_t)dw * dh);
    bool hd = vh >= 720;
    int cRV = hd ? 459 : 409, cGU = hd ? 55 : 100, cGV = hd ? 136 : 208, cBU = hd ? 541 : 516;
    bool full = f->video.fullRange;
    for (int y = 0; y < dh; y++) {
        uint32_t sy = ct + (uint32_t)((uint64_t)y * vh / dh);
        const uint8_t* yr = Y + (size_t)sy * pitch;
        const uint8_t* cr_ = C + (size_t)(sy >> 1) * pitch;
        uint32_t* out = &g_scratch[(size_t)y * dw];
        for (int x = 0; x < dw; x++) {
            uint32_t sx = cl + (uint32_t)((uint64_t)x * vw / dw);
            int yy = full ? ((int)yr[sx] * 256) : (((int)yr[sx] - 16) * 298);
            int u = (int)cr_[sx & ~1u] - 128, v = (int)cr_[(sx & ~1u) + 1] - 128;
            int R = (yy + cRV * v + 128) >> 8;
            int G = (yy - cGU * u - cGV * v + 128) >> 8;
            int B = (yy + cBU * u + 128) >> 8;
            R = R < 0 ? 0 : R > 255 ? 255 : R; G = G < 0 ? 0 : G > 255 ? 255 : G; B = B < 0 ? 0 : B > 255 ? 255 : B;
            out[x] = ((uint32_t)R << 16) | ((uint32_t)G << 8) | (uint32_t)B;
        }
    }
    g_pic.w = dw; g_pic.h = dh;
    g_pic.px.swap(g_scratch);
    return true;
}
