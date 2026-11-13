#include "media.h"
#include "image.h"
#include "log.h"

#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/AudioOut.h>

#include <algorithm>
#include <atomic>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#define STB_VORBIS_HEADER_ONLY
#include <stb/stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD
#include "minimp3.h"

// ---------------------------------------------------------------- картинки

bool picture_decode(const std::string& data, int maxW, int maxH, Picture& out)
{
    Image img;
    if (!img.loadMem((const unsigned char*)data.data(), data.size())) return false;
    int sw = img.width, sh = img.height;
    if (sw <= 0 || sh <= 0) return false;
    double k = std::min((double)maxW / sw, (double)maxH / sh);
    if (k > 1.0) k = 1.0;                       // маленькие картинки не растягиваем
    int dw = std::max(1, (int)(sw * k)), dh = std::max(1, (int)(sh * k));
    out.w = dw; out.h = dh;
    out.px.assign((size_t)dw * dh, 0);
    for (int y = 0; y < dh; y++) {
        // для уменьшения усредняем блок исходных пикселей (быстрое "area"-сглаживание)
        int y0 = (int)((long long)y * sh / dh), y1 = std::max(y0 + 1, (int)((long long)(y + 1) * sh / dh));
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw), x1 = std::max(x0 + 1, (int)((long long)(x + 1) * sw / dw));
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1 && yy < sh; yy++)
                for (int xx = x0; xx < x1 && xx < sw; xx++) {
                    const unsigned char* p = img.pixels + ((size_t)yy * sw + xx) * 4;
                    unsigned a = p[3];
                    // прозрачное - на тёмный фон
                    r += p[0] * a / 255; g += p[1] * a / 255; b += p[2] * a / 255; n++;
                }
            if (!n) n = 1;
            out.px[(size_t)y * dw + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
        }
    }
    return true;
}

// ---------------------------------------------------------------- источники звука

namespace {

// Отдаёт стереозвук (int16, LR LR ...) в собственной частоте.
struct Src {
    int rate = 0;
    double total = 0;
    virtual ~Src() {}
    virtual bool init() = 0;
    virtual int read(int16_t* dst, int frames) = 0;   // сколько кадров записано; 0 - конец
    virtual bool seek(double sec) = 0;                // перейти на секунду sec (в пределах файла)
};

static inline int16_t clip16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24); }

struct WavSrc : Src {
    const unsigned char* d; size_t n;
    const unsigned char* pcm = nullptr; size_t pcmLen = 0, pos = 0;
    int ch = 0, bits = 0, fmt = 0, frameBytes = 0;
    WavSrc(const std::string& s) : d((const unsigned char*)s.data()), n(s.size()) {}
    bool init() override {
        if (n < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) return false;
        size_t p = 12;
        bool haveFmt = false;
        while (p + 8 <= n) {
            size_t len = rd32(d + p + 4);
            if (!memcmp(d + p, "fmt ", 4) && p + 8 + 16 <= n) {
                fmt = rd16(d + p + 8); ch = rd16(d + p + 10); rate = (int)rd32(d + p + 12); bits = rd16(d + p + 22);
                if (fmt == 0xFFFE && len >= 26 && p + 8 + 26 <= n) fmt = rd16(d + p + 8 + 24);   // WAVE_FORMAT_EXTENSIBLE
                haveFmt = true;
            } else if (!memcmp(d + p, "data", 4)) {
                pcm = d + p + 8;
                pcmLen = std::min(len, n - (p + 8));
                break;
            }
            p += 8 + len + (len & 1);
        }
        if (!haveFmt || !pcm || ch < 1 || rate < 1000) return false;
        if (!((fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (fmt == 3 && bits == 32))) return false;
        frameBytes = ch * bits / 8;
        total = (double)(pcmLen / frameBytes) / rate;
        return true;
    }
    int read(int16_t* dst, int frames) override {
        int k = 0;
        while (k < frames && pos + frameBytes <= pcmLen) {
            const unsigned char* f = pcm + pos;
            int v[2];
            for (int c = 0; c < 2; c++) {
                const unsigned char* s = f + (c < ch ? c : 0) * bits / 8;
                int x;
                if (fmt == 3) { float fl; memcpy(&fl, s, 4); x = (int)(fl * 32767.0f); }
                else if (bits == 8) x = ((int)s[0] - 128) << 8;
                else if (bits == 16) x = (int16_t)rd16(s);
                else if (bits == 24) x = (int16_t)rd16(s + 1);
                else x = (int16_t)rd16(s + 2);
                v[c] = x;
            }
            dst[2 * k] = clip16(v[0]); dst[2 * k + 1] = clip16(v[1]);
            pos += frameBytes; k++;
        }
        return k;
    }
    bool seek(double sec) override {
        size_t f = (size_t)(sec * rate);
        pos = std::min(pcmLen / frameBytes, f) * frameBytes;
        return true;
    }
};

struct OggSrc : Src {
    const std::string& s;
    stb_vorbis* v = nullptr;
    OggSrc(const std::string& str) : s(str) {}
    ~OggSrc() { if (v) stb_vorbis_close(v); }
    bool init() override {
        int err = 0;
        v = stb_vorbis_open_memory((const unsigned char*)s.data(), (int)s.size(), &err, NULL);
        if (!v) return false;
        stb_vorbis_info info = stb_vorbis_get_info(v);
        rate = (int)info.sample_rate;
        total = stb_vorbis_stream_length_in_seconds(v);
        return rate >= 1000;
    }
    int read(int16_t* dst, int frames) override {
        return stb_vorbis_get_samples_short_interleaved(v, 2, dst, frames * 2);
    }
    bool seek(double sec) override { return stb_vorbis_seek(v, (unsigned)(sec * rate)) != 0; }
};

struct Mp3Src : Src {
    const unsigned char* d; size_t n, pos = 0;
    mp3dec_t dec;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t startPos = 0;                  // начало звуковых данных (после ID3)
    unsigned long long decoded = 0;       // сколько кадров (на канал) декодировано - для оценки скорости потока
    int have = 0, at = 0, chans = 2, kbps = 0;      // последний декодированный кадр: сколько кадров всего и сколько уже отдано
    Mp3Src(const std::string& s) : d((const unsigned char*)s.data()), n(s.size()) { mp3dec_init(&dec); }
    bool decodeNext() {
        for (int guard = 0; guard < 200000 && pos < n; guard++) {
            mp3dec_frame_info_t info;
            int smp = mp3dec_decode_frame(&dec, d + pos, (int)(n - pos), pcm, &info);
            if (info.frame_bytes <= 0) return false;
            pos += (size_t)info.frame_bytes;
            if (smp > 0) { have = smp; at = 0; chans = info.channels; if (!rate) rate = info.hz; kbps = info.bitrate_kbps; decoded += (unsigned long long)smp; return true; }
        }
        return false;
    }
    bool init() override {
        // пропускаем тег ID3v2
        if (n >= 10 && !memcmp(d, "ID3", 3)) {
            size_t sz = ((d[6] & 0x7F) << 21) | ((d[7] & 0x7F) << 14) | ((d[8] & 0x7F) << 7) | (d[9] & 0x7F);
            pos = std::min(n, 10 + sz);
        }
        startPos = pos;
        if (!decodeNext() || rate < 1000) return false;
        // длительность - оценка по битрейту первого кадра (для файлов с переменным битрейтом приблизительная)
        total = kbps > 0 ? (double)(n - startPos) * 8.0 / (kbps * 1000.0) : 0;
        return true;
    }
    int read(int16_t* dst, int frames) override {
        int k = 0;
        while (k < frames) {
            if (at >= have) { if (!decodeNext()) break; }
            if (chans == 1) { dst[2 * k] = dst[2 * k + 1] = pcm[at]; }
            else { dst[2 * k] = pcm[at * chans]; dst[2 * k + 1] = pcm[at * chans + 1]; }
            at++; k++;
        }
        return k;
    }
    bool seek(double sec) override {
        // точного индекса у MP3 нет: переходим по средней скорости потока (байт на секунду), как делают простые плееры
        double bps = (decoded > (unsigned long long)rate && pos > startPos) ? (double)(pos - startPos) / ((double)decoded / rate) : kbps * 125.0;
        if (bps < 1000) return false;
        size_t np = startPos + (size_t)(sec * bps);
        if (np >= n) np = n > 4096 ? n - 4096 : 0;
        pos = np; have = at = 0;
        decoded = (unsigned long long)(sec * rate);
        mp3dec_init(&dec);
        return decodeNext();
    }
};

// ---------------------------------------------------------------- проигрыватель

struct Player {
    std::string data;
    std::string ext;
    int userId = 0;
    std::atomic<int> stop{0};
    std::atomic<int> paused{0};
    std::atomic<int> state{1};
    std::atomic<long long> frames{0};
    std::atomic<int> totalMs{0};
    std::atomic<int> seekMs{0};          // накопленная просьба о перемотке, мс
};

static Player* g_pl = nullptr;
static OrbisPthread g_th;
static bool g_thValid = false;

struct Resampler {
    Src* src; double step; double frac = 0; int16_t a[2] = { 0, 0 }, b[2] = { 0, 0 };
    int16_t buf[1024 * 2]; int bn = 0, bp = 0; bool eof = false;
    bool fetch(int16_t o[2]) {
        if (bp >= bn) {
            if (eof) return false;
            bn = src->read(buf, 1024); bp = 0;
            if (bn <= 0) { eof = true; bn = 0; return false; }
        }
        o[0] = buf[2 * bp]; o[1] = buf[2 * bp + 1]; bp++;
        return true;
    }
    bool start() { if (!fetch(a)) return false; if (!fetch(b)) { b[0] = a[0]; b[1] = a[1]; } return true; }
    // следующий выходной кадр; false - данные кончились
    bool next(int16_t o[2]) {
        if (frac >= 1.0) {
            while (frac >= 1.0) {
                frac -= 1.0;
                a[0] = b[0]; a[1] = b[1];
                if (!fetch(b)) return false;
            }
        }
        o[0] = (int16_t)(a[0] + (b[0] - a[0]) * frac);
        o[1] = (int16_t)(a[1] + (b[1] - a[1]) * frac);
        frac += step;
        return true;
    }
};

alignas(256) static int16_t g_out[1024 * 2];

static void* audio_main(void* arg)
{
    Player* P = (Player*)arg;
    Src* src = nullptr;
    if (P->ext == "wav") src = new WavSrc(P->data);
    else if (P->ext == "ogg") src = new OggSrc(P->data);
    else if (P->ext == "mp3") src = new Mp3Src(P->data);
    if (!src || !src->init()) { logf_("audio: cannot decode .%s", P->ext.c_str()); P->state = 4; delete src; return NULL; }
    P->totalMs = (int)(src->total * 1000.0);

    sceAudioOutInit();
    int h = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, 1024, 48000, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (h < 0) { logf_("audio: sceAudioOutOpen failed 0x%08X", (unsigned)h); P->state = 4; delete src; return NULL; }

    Resampler rs; rs.src = src; rs.step = (double)src->rate / 48000.0;
    bool ok = rs.start();
    P->state = ok ? 2 : 3;
    bool done = !ok;
    while (!P->stop) {
        int sk = P->seekMs.exchange(0);
        if (sk != 0) {
            double cur = (double)P->frames / 48000.0 + sk / 1000.0;
            double hi = src->total > 1.0 ? src->total - 0.5 : 1e9;
            if (cur < 0) cur = 0;
            if (cur > hi) cur = hi;
            if (src->seek(cur)) {
                rs.frac = 0; rs.bn = rs.bp = 0; rs.eof = false;
                done = !rs.start();
                P->frames = (long long)(cur * 48000.0);
                if (!done) P->state = 2;
            }
            continue;
        }
        if (P->paused || done) {
            memset(g_out, 0, sizeof(g_out));
            sceAudioOutOutput(h, g_out);
            if (done) { P->state = 3; if (P->stop) break; }
            continue;
        }
        int i = 0;
        for (; i < 1024; i++) {
            int16_t o[2];
            if (!rs.next(o)) { done = true; break; }
            g_out[2 * i] = o[0]; g_out[2 * i + 1] = o[1];
        }
        for (int j = i; j < 1024; j++) { g_out[2 * j] = 0; g_out[2 * j + 1] = 0; }
        sceAudioOutOutput(h, g_out);
        P->frames += i;
    }
    sceAudioOutClose(h);
    delete src;
    return NULL;
}

} // namespace

bool audio_start(std::string& data, const std::string& ext, int userId)
{
    audio_stop();
    g_pl = new Player();
    g_pl->data.swap(data);
    g_pl->ext = ext;
    g_pl->userId = userId;
    OrbisPthreadAttr attr;
    scePthreadAttrInit(&attr);
    scePthreadAttrSetstacksize(&attr, 2 * 1024 * 1024);
    int r = scePthreadCreate(&g_th, &attr, audio_main, g_pl, "pc_audio");
    scePthreadAttrDestroy(&attr);
    if (r != 0) { logf_("audio: thread create failed 0x%08X", (unsigned)r); delete g_pl; g_pl = nullptr; return false; }
    g_thValid = true;
    return true;
}

void audio_stop()
{
    if (!g_pl) return;
    g_pl->stop = 1;
    if (g_thValid) scePthreadJoin(g_th, NULL);
    g_thValid = false;
    delete g_pl;
    g_pl = nullptr;
}

void audio_seek(int deltaMs) { if (g_pl && (g_pl->state == 2 || g_pl->state == 3)) g_pl->seekMs += deltaMs; }
void audio_pause_toggle() { if (g_pl) g_pl->paused = g_pl->paused ? 0 : 1; }
bool audio_paused() { return g_pl && g_pl->paused; }
int audio_state() { return g_pl ? (int)g_pl->state : 0; }
double audio_seconds() { return g_pl ? (double)g_pl->frames / 48000.0 : 0; }
double audio_total() { return g_pl ? g_pl->totalMs / 1000.0 : 0; }
