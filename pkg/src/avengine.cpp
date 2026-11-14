#include "avengine.h"
#include "service.h"
#include "log.h"

#include <orbis/libkernel.h>
#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <string.h>
#include <stdlib.h>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

extern "C" int pthread_setattr_default_np(const pthread_attr_t*) __attribute__((weak));   // есть не во всех сборках libc

namespace {

void sleep_ms(int ms) { sceKernelUsleep((unsigned)ms * 1000u); }
uint64_t now_us() { return sceKernelGetProcessTime(); }

// ---------------------------------------------------------------- чтение файла из службы

struct Io {
    std::string path;
    int64_t size = 0;
    int64_t pos = 0;
    std::vector<uint8_t> cache;
    int64_t cacheStart = 0;
    std::atomic<int>* quit = nullptr;
};

int io_read(void* opaque, uint8_t* buf, int n)
{
    Io* io = (Io*)opaque;
    if (io->pos >= io->size) return AVERROR_EOF;
    if (io->pos + n > io->size) n = (int)(io->size - io->pos);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (io->quit && *io->quit) return AVERROR_EXIT;
        if (io->pos >= io->cacheStart && io->pos + n <= io->cacheStart + (int64_t)io->cache.size()) {
            memcpy(buf, io->cache.data() + (io->pos - io->cacheStart), (size_t)n);
            io->pos += n;
            return n;
        }
        unsigned want = (unsigned)std::max<int64_t>(n, 2 * 1024 * 1024);
        if (io->pos + want > (uint64_t)io->size) want = (unsigned)(io->size - io->pos);
        io->cache.resize(want);
        long got = service_read_range(io->path, (unsigned long long)io->pos, want, io->cache.data(), NULL);
        if (got < n) { io->cache.clear(); io->cacheStart = 0; continue; }
        io->cache.resize((size_t)got);
        io->cacheStart = io->pos;
    }
    return AVERROR(EIO);
}

int64_t io_seek(void* opaque, int64_t off, int whence)
{
    Io* io = (Io*)opaque;
    if (whence & AVSEEK_SIZE) return io->size;
    whence &= ~AVSEEK_FORCE;
    int64_t np;
    if (whence == SEEK_SET) np = off;
    else if (whence == SEEK_CUR) np = io->pos + off;
    else if (whence == SEEK_END) np = io->size + off;
    else return -1;
    if (np < 0 || np > io->size) return -1;
    io->pos = np;
    return np;
}

// ---------------------------------------------------------------- очередь пакетов

struct PktQ {
    std::deque<AVPacket*> q;
    size_t bytes = 0;
    OrbisPthreadMutex mu;
    void init(const char* name) { scePthreadMutexInit(&mu, NULL, name); }
    void push(AVPacket* p) { scePthreadMutexLock(&mu); bytes += (size_t)p->size; q.push_back(p); scePthreadMutexUnlock(&mu); }
    AVPacket* pop() {
        scePthreadMutexLock(&mu);
        AVPacket* p = nullptr;
        if (!q.empty()) { p = q.front(); q.pop_front(); bytes -= (size_t)p->size; }
        scePthreadMutexUnlock(&mu);
        return p;
    }
    size_t count() { scePthreadMutexLock(&mu); size_t n = q.size(); scePthreadMutexUnlock(&mu); return n; }
    size_t size_bytes() { scePthreadMutexLock(&mu); size_t n = bytes; scePthreadMutexUnlock(&mu); return n; }
    void clear() {
        scePthreadMutexLock(&mu);
        for (AVPacket* p : q) av_packet_free(&p);
        q.clear(); bytes = 0;
        scePthreadMutexUnlock(&mu);
    }
};

alignas(256) int16_t g_aout[1024 * 2];

} // namespace

struct AvEngine {
    std::string path;
    Io io;
    std::atomic<int> quit{ 0 };
    std::atomic<int> state{ 1 };
    std::atomic<int> paused{ 0 };
    std::atomic<int> gen{ 0 };                       // номер "эпохи": растёт при перемотке
    std::atomic<long long> seekReq{ -1 };            // запрос перемотки (мс), выполняет поток чтения
    std::atomic<int> eof{ 0 };
    std::atomic<int> videoDone{ 0 }, audioDone{ 0 };
    AVFormatContext* fmt = nullptr;
    AVIOContext* avio = nullptr;
    int vs = -1, as = -1;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    PktQ vq, aq;
    long long totalMs = 0;
    bool seekable = false;
    std::string info;
    // часы: по выведенному звуку (если звука нет - по системному времени)
    std::atomic<long long> clockBaseMs{ 0 };
    std::atomic<long long> clockFrames{ 0 };
    std::atomic<int> clockSync{ 0 };                 // 1 - ждём первый звук после перемотки, чтобы взять его метку
    std::atomic<uint64_t> wallBaseUs{ 0 };
    std::atomic<long long> wallBaseMs{ 0 };
    // картинка для интерфейса
    Picture back;
    OrbisPthreadMutex picMu;
    std::atomic<int> newFrame{ 0 };
    OrbisPthread thDemux, thVideo, thAudio;
    bool vDemux = false, vVideo = false, vAudio = false;
    int audioPort = -1;

    long long clock_ms() const {
        if (as >= 0) return clockBaseMs.load() + clockFrames.load() * 1000 / 48000;
        if (paused) return wallBaseMs.load();
        return wallBaseMs.load() + (long long)((now_us() - wallBaseUs.load()) / 1000);
    }
};

namespace {

void clock_reset(AvEngine* E, long long ms)
{
    E->clockBaseMs = ms; E->clockFrames = 0;
    E->wallBaseMs = ms; E->wallBaseUs = now_us();
}

long long ts_ms(AVRational tb, int64_t ts)
{
    if (ts == AV_NOPTS_VALUE) return -1;
    return av_rescale_q(ts, tb, AVRational{ 1, 1000 });
}

// ---------------------------------------------------------------- поток чтения/разбора контейнера

void* demux_main(void* arg)
{
    AvEngine* E = (AvEngine*)arg;
    while (!E->quit) {
        long long sk = E->seekReq.exchange(-1);
        if (sk >= 0) {
            if (E->totalMs > 0 && sk > E->totalMs - 300) sk = E->totalMs - 300;
            if (sk < 0) sk = 0;
            int r = av_seek_frame(E->fmt, -1, sk * 1000, AVSEEK_FLAG_BACKWARD);
            logf_("avengine: seek to %lld ms = %d", sk, r);
            E->vq.clear(); E->aq.clear();
            E->eof = 0; E->videoDone = 0; E->audioDone = 0;
            clock_reset(E, sk);
            if (E->as >= 0) E->clockSync = 1;
            E->gen++;
            continue;
        }
        if (E->eof || E->vq.size_bytes() > 24u * 1024 * 1024 || E->aq.size_bytes() > 4u * 1024 * 1024 || E->vq.count() > 600) { sleep_ms(4); continue; }
        AVPacket* p = av_packet_alloc();
        int r = av_read_frame(E->fmt, p);
        if (r < 0) {
            av_packet_free(&p);
            if (r == AVERROR_EXIT) break;
            if (r != AVERROR_EOF) logf_("avengine: read error %d", r);
            E->eof = 1;
            continue;
        }
        if (p->stream_index == E->vs) E->vq.push(p);
        else if (p->stream_index == E->as) E->aq.push(p);
        else av_packet_free(&p);
    }
    return NULL;
}

// ---------------------------------------------------------------- видео

void publish(AvEngine* E, std::vector<uint32_t>& out, int w, int h)
{
    scePthreadMutexLock(&E->picMu);
    E->back.px.swap(out);
    E->back.w = w; E->back.h = h;
    E->newFrame = 1;
    scePthreadMutexUnlock(&E->picMu);
}

void* video_main(void* arg)
{
    AvEngine* E = (AvEngine*)arg;
    AVFrame* fr = av_frame_alloc();
    std::vector<uint32_t> out;
    int myGen = E->gen;
    bool drained = false;
    int frames = 0, dropped = 0, decoded = 0;
    uint64_t t0 = now_us(), decUs = 0, cvtUs = 0, lastShow = 0;
    long long maxLag = 0;
    bool firstLogged = false;
    AVRational tb = E->fmt->streams[E->vs]->time_base;
    while (!E->quit) {
        if (myGen != E->gen) { avcodec_flush_buffers(E->vdec); myGen = E->gen; drained = false; E->videoDone = 0; }
        uint64_t nw = now_us();
        if (nw - t0 >= 5000000ULL) {
            logf_("avengine(video): decoded %d, shown %d, skipped %d in %.1f s; decode %.1f ms/frame, convert %.1f ms, max lag %lld ms",
                  decoded, frames, dropped, (nw - t0) / 1e6, decoded ? decUs / 1000.0 / decoded : 0.0, frames ? cvtUs / 1000.0 / frames : 0.0, maxLag);
            frames = dropped = decoded = 0; decUs = cvtUs = 0; maxLag = 0; t0 = nw;
        }
        AVPacket* p = E->vq.pop();
        if (!p) {
            if (E->eof && !drained) { avcodec_send_packet(E->vdec, NULL); drained = true; }
            else if (drained && E->vq.count() == 0) { E->videoDone = 1; sleep_ms(10); continue; }
            else { sleep_ms(3); continue; }
        } else {
            uint64_t d0 = now_us();
            int r = avcodec_send_packet(E->vdec, p);
            decUs += now_us() - d0;
            av_packet_free(&p);
            if (r < 0 && r != AVERROR(EAGAIN)) continue;
        }
        for (;;) {
            uint64_t d0 = now_us();
            int r = avcodec_receive_frame(E->vdec, fr);
            decUs += now_us() - d0;
            if (r < 0) break;
            decoded++;
            if (myGen != E->gen || E->quit) { av_frame_unref(fr); break; }
            int64_t ts = fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts;
            long long ptsMs = ts_ms(tb, ts);
            long long ct = E->clock_ms();
            if (ptsMs < 0) ptsMs = ct;
            long long lag = ct - ptsMs;
            if (lag > maxLag) maxLag = lag;
            if (!firstLogged) { firstLogged = true; logf_("avengine: first video frame %dx%d fmt=%d pts=%lld ms, clock=%lld ms", fr->width, fr->height, fr->format, ptsMs, ct); }
            // опоздавшие кадры: после перемотки пропускаем без показа; при медленном декодере показываем не чаще раза в 200 мс
            if (lag > 400 && !E->paused) {
                if (lag > 1500 || now_us() - lastShow < 200000ULL) { dropped++; av_frame_unref(fr); continue; }
            }
            bool abort = false;
            while (ptsMs > E->clock_ms() + 15 || (E->paused && E->newFrame.load() != 0 && false)) {
                if (E->quit || myGen != E->gen) { abort = true; break; }
                sleep_ms(3);
            }
            if (abort) { av_frame_unref(fr); break; }
            if (E->state == 1) E->state = 2;
            // масштаб на весь экран с сохранением пропорций (с учётом формы пикселя)
            double sar = (fr->sample_aspect_ratio.num > 0 && fr->sample_aspect_ratio.den > 0) ? av_q2d(fr->sample_aspect_ratio) : 1.0;
            double visW = fr->width * sar, visH = fr->height;
            double k = std::min(1920.0 / visW, 1080.0 / visH);
            int dw = std::max(2, (int)(visW * k)) & ~1, dh = std::max(2, (int)(visH * k)) & ~1;
            uint64_t c0 = now_us();
            E->sws = sws_getCachedContext(E->sws, fr->width, fr->height, (AVPixelFormat)fr->format, dw, dh, AV_PIX_FMT_BGR0, SWS_BILINEAR, NULL, NULL, NULL);
            if (!E->sws) { av_frame_unref(fr); continue; }
            int cs = SWS_CS_DEFAULT;
            if (fr->colorspace == AVCOL_SPC_BT709) cs = SWS_CS_ITU709;
            else if (fr->colorspace == AVCOL_SPC_BT2020_NCL || fr->colorspace == AVCOL_SPC_BT2020_CL) cs = SWS_CS_BT2020;
            else if (fr->colorspace == AVCOL_SPC_SMPTE170M || fr->colorspace == AVCOL_SPC_BT470BG) cs = SWS_CS_ITU601;
            else if (fr->height >= 720) cs = SWS_CS_ITU709;
            sws_setColorspaceDetails(E->sws, sws_getCoefficients(cs), fr->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
            out.resize((size_t)dw * dh);
            uint8_t* dst[4] = { (uint8_t*)out.data(), NULL, NULL, NULL };
            int dstStride[4] = { dw * 4, 0, 0, 0 };
            sws_scale(E->sws, fr->data, fr->linesize, 0, fr->height, dst, dstStride);
            cvtUs += now_us() - c0;
            av_frame_unref(fr);
            publish(E, out, dw, dh);
            lastShow = now_us();
            frames++;
        }
    }
    av_frame_free(&fr);
    return NULL;
}

// ---------------------------------------------------------------- звук

void* audio_main(void* arg)
{
    AvEngine* E = (AvEngine*)arg;
    AVFrame* fr = av_frame_alloc();
    std::vector<int16_t> q;        // стереокадры 48 кГц (L,R,L,R...)
    size_t qpos = 0;
    int myGen = E->gen;
    bool drained = false;
    bool firstLogged = false;
    AVRational tb = E->fmt->streams[E->as]->time_base;
    std::vector<int16_t> tmp(8192 * 2);
    int idle = 0;
    while (!E->quit) {
        if (myGen != E->gen) { avcodec_flush_buffers(E->adec); q.clear(); qpos = 0; myGen = E->gen; drained = false; E->audioDone = 0; }
        // накопить хотя бы 4096 кадров
        while ((q.size() - qpos) / 2 < 4096 && !E->quit && myGen == E->gen) {
            AVPacket* p = E->aq.pop();
            if (!p) {
                if (E->eof && !drained) { avcodec_send_packet(E->adec, NULL); drained = true; }
                else break;
            } else {
                int r = avcodec_send_packet(E->adec, p);
                av_packet_free(&p);
                if (r < 0 && r != AVERROR(EAGAIN)) continue;
            }
            while (avcodec_receive_frame(E->adec, fr) >= 0) {
                if (E->clockSync.exchange(0)) {
                    long long m = ts_ms(tb, fr->best_effort_timestamp != AV_NOPTS_VALUE ? fr->best_effort_timestamp : fr->pts);
                    if (m >= 0) { E->clockBaseMs = m; E->clockFrames = 0; }
                }
                if (!E->swr) {
                    AVChannelLayout outl = AV_CHANNEL_LAYOUT_STEREO;
                    if (swr_alloc_set_opts2(&E->swr, &outl, AV_SAMPLE_FMT_S16, 48000, &fr->ch_layout, (AVSampleFormat)fr->format, fr->sample_rate, 0, NULL) < 0 || swr_init(E->swr) < 0) {
                        logf_("avengine: audio resampler init failed");
                        swr_free(&E->swr);
                        av_frame_unref(fr);
                        continue;
                    }
                }
                if (!firstLogged) { firstLogged = true; logf_("avengine: first audio frame rate=%d ch=%d fmt=%d", fr->sample_rate, fr->ch_layout.nb_channels, fr->format); }
                int maxOut = swr_get_out_samples(E->swr, fr->nb_samples);
                if (maxOut > (int)(tmp.size() / 2)) tmp.resize((size_t)maxOut * 2);
                uint8_t* od[1] = { (uint8_t*)tmp.data() };
                int n = swr_convert(E->swr, od, maxOut, (const uint8_t**)fr->extended_data, fr->nb_samples);
                av_frame_unref(fr);
                if (n > 0) {
                    if (qpos > 65536) { q.erase(q.begin(), q.begin() + (long)qpos); qpos = 0; }
                    q.insert(q.end(), tmp.begin(), tmp.begin() + (long)n * 2);
                }
            }
        }
        if (E->audioPort < 0) { sleep_ms(10); continue; }
        if (E->paused) {
            memset(g_aout, 0, sizeof(g_aout));
            sceAudioOutOutput(E->audioPort, g_aout);
            continue;
        }
        if ((q.size() - qpos) / 2 >= 1024) {
            memcpy(g_aout, q.data() + qpos, sizeof(g_aout));
            qpos += 2048;
            sceAudioOutOutput(E->audioPort, g_aout);
            if (!E->clockSync) E->clockFrames += 1024;
            idle = 0;
            if (E->state == 1 && E->vs < 0) E->state = 2;
        } else {
            // данных нет: тишина, часы не идут (видео подождёт)
            memset(g_aout, 0, sizeof(g_aout));
            sceAudioOutOutput(E->audioPort, g_aout);
            if (E->eof && drained && E->aq.count() == 0) E->audioDone = 1;
            if (++idle > 2000) idle = 2000;
        }
    }
    av_frame_free(&fr);
    return NULL;
}

std::string describe(AvEngine* E)
{
    std::string s;
    char b[160];
    if (E->vs >= 0) {
        AVCodecParameters* cp = E->fmt->streams[E->vs]->codecpar;
        const char* prof = avcodec_profile_name(cp->codec_id, cp->profile);
        snprintf(b, sizeof(b), "%s%s%s %dx%d", avcodec_get_name(cp->codec_id), prof ? " " : "", prof ? prof : "", cp->width, cp->height);
        s = b;
    }
    if (E->as >= 0) {
        AVCodecParameters* cp = E->fmt->streams[E->as]->codecpar;
        snprintf(b, sizeof(b), "%s%s %d Hz, %d ch", s.empty() ? "" : ", ", avcodec_get_name(cp->codec_id), cp->sample_rate, cp->ch_layout.nb_channels);
        s += b;
    }
    return s;
}

} // namespace

AvEngine* ave_start(const std::string& path, std::string& err)
{
    AvEngine* E = new AvEngine();
    E->path = path;
    E->vq.init("av_vq"); E->aq.init("av_aq");
    scePthreadMutexInit(&E->picMu, NULL, "av_pic");

    unsigned char probe[1];
    unsigned long long total = 0;
    if (service_read_range(path, 0, 1, probe, &total) < 0 || total == 0) { err = "noconn"; delete E; return nullptr; }
    E->io.path = path; E->io.size = (int64_t)total; E->io.quit = &E->quit;

    // потоки libav (кадровые потоки декодера): стек по умолчанию в musl мал
    { pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 1024 * 1024); if (pthread_setattr_default_np) pthread_setattr_default_np(&at); pthread_attr_destroy(&at); }

    const int bufSize = 256 * 1024;
    uint8_t* buf = (uint8_t*)av_malloc(bufSize);
    E->avio = avio_alloc_context(buf, bufSize, 0, &E->io, io_read, NULL, io_seek);
    if (!E->avio) { err = "avio"; delete E; return nullptr; }
    E->fmt = avformat_alloc_context();
    E->fmt->pb = E->avio;
    E->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    E->fmt->probesize = 8 * 1024 * 1024;
    E->fmt->max_analyze_duration = 4 * AV_TIME_BASE;
    int r = avformat_open_input(&E->fmt, "", NULL, NULL);
    if (r < 0) { char eb[96]; av_strerror(r, eb, sizeof(eb)); logf_("avengine: open failed: %s", eb); err = "open"; ave_stop(E); return nullptr; }
    r = avformat_find_stream_info(E->fmt, NULL);
    if (r < 0) { logf_("avengine: find_stream_info failed %d", r); err = "streams"; ave_stop(E); return nullptr; }
    E->vs = av_find_best_stream(E->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    E->as = av_find_best_stream(E->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (E->vs < 0 && E->as < 0) { err = "nostreams"; ave_stop(E); return nullptr; }
    // кадр-обложка (jpeg/png в mp3/flac) видео не считаем
    if (E->vs >= 0 && (E->fmt->streams[E->vs]->disposition & AV_DISPOSITION_ATTACHED_PIC)) E->vs = -1;
    E->totalMs = E->fmt->duration > 0 ? E->fmt->duration / 1000 : 0;
    E->seekable = (E->fmt->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0;

    auto open_dec = [&](int idx, AVCodecContext** ctx, bool video) -> bool {
        AVCodecParameters* cp = E->fmt->streams[idx]->codecpar;
        const AVCodec* c = avcodec_find_decoder(cp->codec_id);
        if (!c) { logf_("avengine: no decoder for %s", avcodec_get_name(cp->codec_id)); return false; }
        *ctx = avcodec_alloc_context3(c);
        avcodec_parameters_to_context(*ctx, cp);
        (*ctx)->pkt_timebase = E->fmt->streams[idx]->time_base;
        if (video) {
            (*ctx)->thread_count = 4;
            (*ctx)->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            (*ctx)->skip_loop_filter = AVDISCARD_DEFAULT;
        }
        if (avcodec_open2(*ctx, c, NULL) < 0) { logf_("avengine: cannot open decoder %s", c->name); avcodec_free_context(ctx); return false; }
        return true;
    };
    if (E->vs >= 0 && !open_dec(E->vs, &E->vdec, true)) E->vs = -1;
    if (E->as >= 0 && !open_dec(E->as, &E->adec, false)) E->as = -1;
    if (E->vs < 0 && E->as < 0) { err = "nodecoder"; ave_stop(E); return nullptr; }
    E->info = describe(E);
    logf_("avengine: %s, duration %lld ms, seekable=%d, format %s", E->info.c_str(), E->totalMs, (int)E->seekable, E->fmt->iformat ? E->fmt->iformat->name : "?");

    if (E->as >= 0) {
        E->audioPort = audio_shared_port();
        if (E->audioPort < 0) logf_("avengine: no audio port (0x%08X)", (unsigned)E->audioPort);
    }
    clock_reset(E, 0);

    OrbisPthreadAttr a;
    scePthreadAttrInit(&a);
    scePthreadAttrSetstacksize(&a, 1024 * 1024);
    E->vDemux = scePthreadCreate(&E->thDemux, &a, demux_main, E, "av_demux") == 0;
    if (E->vs >= 0) E->vVideo = scePthreadCreate(&E->thVideo, &a, video_main, E, "av_video") == 0;
    if (E->as >= 0) E->vAudio = scePthreadCreate(&E->thAudio, &a, audio_main, E, "av_audio") == 0;
    scePthreadAttrDestroy(&a);
    return E;
}

void ave_stop(AvEngine* E)
{
    if (!E) return;
    E->quit = 1;
    if (E->vDemux) scePthreadJoin(E->thDemux, NULL);
    if (E->vVideo) scePthreadJoin(E->thVideo, NULL);
    if (E->vAudio) scePthreadJoin(E->thAudio, NULL);
    E->vq.clear(); E->aq.clear();
    if (E->sws) sws_freeContext(E->sws);
    if (E->swr) swr_free(&E->swr);
    if (E->vdec) avcodec_free_context(&E->vdec);
    if (E->adec) avcodec_free_context(&E->adec);
    if (E->fmt) avformat_close_input(&E->fmt);
    if (E->avio) { av_freep(&E->avio->buffer); avio_context_free(&E->avio); }
    delete E;
}

int ave_state(AvEngine* E)
{
    if (!E) return 0;
    int s = E->state;
    if (s == 2 && E->eof && (E->vs < 0 || E->videoDone) && (E->as < 0 || E->audioDone)) return 3;
    return s;
}
bool ave_has_video(AvEngine* E) { return E && E->vs >= 0; }
std::string ave_info(AvEngine* E) { return E ? E->info : ""; }
void ave_pause(AvEngine* E, bool p)
{
    if (!E) return;
    if (p && !E->paused) { E->wallBaseMs = E->clock_ms(); }
    if (!p && E->paused) E->wallBaseUs = now_us();
    E->paused = p ? 1 : 0;
}
void ave_seek(AvEngine* E, long long deltaMs)
{
    if (!E || !E->seekable) return;
    long long base = E->seekReq.load();
    if (base < 0) base = E->clock_ms();
    long long t = base + deltaMs;
    if (t < 0) t = 0;
    E->seekReq = t;
}
long long ave_time_ms(AvEngine* E)
{
    if (!E) return 0;
    long long t = E->seekReq.load();
    return t >= 0 ? t : E->clock_ms();
}
long long ave_total_ms(AvEngine* E) { return E ? E->totalMs : 0; }
bool ave_seekable(AvEngine* E) { return E && E->seekable; }
bool ave_poll(AvEngine* E, Picture& pic)
{
    if (!E || !E->newFrame.load()) return false;
    scePthreadMutexLock(&E->picMu);
    pic.px.swap(E->back.px);
    pic.w = E->back.w; pic.h = E->back.h;
    E->newFrame = 0;
    scePthreadMutexUnlock(&E->picMu);
    return true;
}
