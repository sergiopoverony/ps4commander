#include "swvideo.h"
#include "service.h"
#include "log.h"
#include <vector>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include "codec_api.h"

namespace {

uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

struct Box { const uint8_t* p = nullptr; size_t n = 0; char type[5] = { 0 }; };

// Дочерние коробки (payload родителя: d, n).
std::vector<Box> children(const uint8_t* d, size_t n)
{
    std::vector<Box> r;
    size_t p = 0;
    while (p + 8 <= n) {
        uint64_t sz = be32(d + p);
        size_t hdr = 8;
        if (sz == 1 && p + 16 <= n) { sz = be64(d + p + 8); hdr = 16; }
        if (sz == 0) sz = n - p;
        if (sz < hdr || p + sz > n) break;
        Box b; b.p = d + p + hdr; b.n = (size_t)sz - hdr; memcpy(b.type, d + p + 4, 4);
        r.push_back(b);
        p += (size_t)sz;
    }
    return r;
}
bool find(const std::vector<Box>& v, const char* t, Box& out)
{
    for (auto& b : v) if (!memcmp(b.type, t, 4)) { out = b; return true; }
    return false;
}
bool find_in(const Box& parent, const char* t, Box& out) { return find(children(parent.p, parent.n), t, out); }

struct Sample { uint64_t off; uint32_t size; long long ptsMs; bool sync; };

} // namespace

struct SwVideo {
    std::string path;
    unsigned long long fileSize = 0;
    std::vector<Sample> samples;
    std::vector<uint8_t> headers;          // SPS/PPS в формате Annex B
    int nalLen = 4;
    int width = 0, height = 0;
    size_t idx = 0;
    bool flushing = false;
    ISVCDecoder* dec = nullptr;
    // кэш чтения из файла
    std::vector<uint8_t> cache;
    unsigned long long cacheStart = 0;
    std::vector<uint8_t> au;               // входной блок (Annex B) + запас
    int errors = 0;
};

namespace {

bool init_dec(SwVideo* s)
{
    if (s->dec) { WelsDestroyDecoder(s->dec); s->dec = nullptr; }
    ISVCDecoder* d = nullptr;
    if (WelsCreateDecoder(&d) != 0 || !d) return false;
    SDecodingParam p;
    memset(&p, 0, sizeof(p));
    p.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    p.eEcActiveIdc = ERROR_CON_SLICE_COPY;
    if (d->Initialize(&p) != 0) { WelsDestroyDecoder(d); return false; }
    s->dec = d;
    // SPS/PPS
    unsigned char* dst[3] = { 0, 0, 0 };
    SBufferInfo info; memset(&info, 0, sizeof(info));
    std::vector<uint8_t> h = s->headers;
    h.resize(h.size() + 8, 0);
    d->DecodeFrame2(h.data(), (int)s->headers.size(), dst, &info);
    s->flushing = false;
    return true;
}

bool read_file(SwVideo* s, uint64_t off, uint32_t len, uint8_t* dst)
{
    if (off >= s->cacheStart && off + len <= s->cacheStart + s->cache.size()) {
        memcpy(dst, s->cache.data() + (off - s->cacheStart), len);
        return true;
    }
    unsigned want = std::max<unsigned>(len, 1024u * 1024u);
    if (s->fileSize && off + want > s->fileSize) want = (unsigned)(s->fileSize - off);
    if (want < len) return false;
    s->cache.resize(want);
    long got = -1;
    for (int attempt = 0; attempt < 3 && got < (long)len; attempt++)
        got = service_read_range(s->path, off, want, s->cache.data(), NULL);
    if (got < (long)len) { s->cache.clear(); return false; }
    s->cache.resize((size_t)got);
    s->cacheStart = off;
    memcpy(dst, s->cache.data(), len);
    return true;
}

} // namespace

SwVideo* sw_open(const std::string& path, unsigned long long fileSize, std::string& err)
{
    SwVideo* s = new SwVideo();
    s->path = path; s->fileSize = fileSize;

    // --- moov
    unsigned long long pos = 0, total = fileSize;
    std::vector<uint8_t> moov;
    for (int guard = 0; guard < 64 && pos + 8 <= total; guard++) {
        uint8_t hd[16];
        long n = service_read_range(path, pos, 16, hd, NULL);
        if (n < 8) break;
        uint64_t sz = be32(hd);
        if (sz == 1 && n >= 16) sz = be64(hd + 8);
        if (sz == 0) sz = total - pos;
        if (sz < 8) break;
        if (!memcmp(hd + 4, "moov", 4)) {
            if (sz > 64u * 1024 * 1024) { err = "moov too big"; delete s; return nullptr; }
            moov.resize((size_t)sz);
            size_t got = 0;
            while (got < moov.size()) {
                unsigned want = (unsigned)std::min<size_t>(moov.size() - got, 1024 * 1024);
                long r = service_read_range(path, pos + got, want, moov.data() + got, NULL);
                if (r <= 0) { err = "moov read"; delete s; return nullptr; }
                got += (size_t)r;
            }
            break;
        }
        pos += sz;
    }
    if (moov.size() < 16) { err = "no moov"; delete s; return nullptr; }

    std::vector<Box> top = children(moov.data() + 8, moov.size() - 8);
    // moov-коробка: первые 8 байт - заголовок, дальше дочерние
    Box stblB;
    bool ok = false;
    long long timescale = 0, mediaTime = 0;
    for (auto& tb : top) {
        if (memcmp(tb.type, "trak", 4)) continue;
        Box mdia, hdlr;
        if (!find_in(tb, "mdia", mdia) || !find_in(mdia, "hdlr", hdlr) || hdlr.n < 12 || memcmp(hdlr.p + 8, "vide", 4)) continue;
        Box mdhd, minf, stbl;
        if (!find_in(mdia, "mdhd", mdhd) || !find_in(mdia, "minf", minf) || !find_in(minf, "stbl", stbl) || mdhd.n < 24) continue;
        timescale = mdhd.p[0] == 1 ? (long long)be32(mdhd.p + 20) : (long long)be32(mdhd.p + 12);
        Box edts, elst;
        if (find_in(tb, "edts", edts) && find_in(edts, "elst", elst) && elst.n >= 8) {
            uint32_t cnt = be32(elst.p + 4);
            size_t ent = elst.p[0] == 1 ? 20 : 12;
            for (uint32_t i = 0; i < cnt && 8 + (i + 1) * ent <= elst.n; i++) {
                const uint8_t* e = elst.p + 8 + i * ent;
                long long mt = elst.p[0] == 1 ? (long long)be64(e + 8) : (long long)(int32_t)be32(e + 4);
                if (mt >= 0) { mediaTime = mt; break; }
            }
        }
        stblB = stbl; ok = true;
        break;
    }
    if (!ok || timescale <= 0) { err = "no video track"; delete s; return nullptr; }

    std::vector<Box> st = children(stblB.p, stblB.n);
    Box stsd, stts, ctts, stss, stsc, stsz, stco, co64;
    bool hasCtts = find(st, "ctts", ctts), hasStss = find(st, "stss", stss), hasCo64 = find(st, "co64", co64);
    if (!find(st, "stsd", stsd) || !find(st, "stts", stts) || !find(st, "stsc", stsc) || !find(st, "stsz", stsz) ||
        (!find(st, "stco", stco) && !hasCo64)) { err = "incomplete sample tables"; delete s; return nullptr; }

    // avcC
    if (stsd.n < 16 + 86) { err = "bad stsd"; delete s; return nullptr; }
    const uint8_t* e = stsd.p + 8;
    if (memcmp(e + 4, "avc1", 4) && memcmp(e + 4, "avc3", 4)) { err = "not H.264"; delete s; return nullptr; }
    s->width = (e[32] << 8) | e[33]; s->height = (e[34] << 8) | e[35];
    {
        size_t el = std::min<size_t>(be32(e), stsd.n - 8);
        std::vector<Box> ch = children(e + 86, el > 86 ? el - 86 : 0);
        Box avcc;
        if (!find(ch, "avcC", avcc) || avcc.n < 7) { err = "no avcC"; delete s; return nullptr; }
        s->nalLen = (avcc.p[4] & 3) + 1;
        size_t q = 6;
        int nsps = avcc.p[5] & 0x1f;
        static const uint8_t sc[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < nsps && q + 2 <= avcc.n; i++) {
            size_t l = (avcc.p[q] << 8) | avcc.p[q + 1]; q += 2;
            if (q + l > avcc.n) break;
            s->headers.insert(s->headers.end(), sc, sc + 4);
            s->headers.insert(s->headers.end(), avcc.p + q, avcc.p + q + l);
            q += l;
        }
        if (q < avcc.n) {
            int npps = avcc.p[q++];
            for (int i = 0; i < npps && q + 2 <= avcc.n; i++) {
                size_t l = (avcc.p[q] << 8) | avcc.p[q + 1]; q += 2;
                if (q + l > avcc.n) break;
                s->headers.insert(s->headers.end(), sc, sc + 4);
                s->headers.insert(s->headers.end(), avcc.p + q, avcc.p + q + l);
                q += l;
            }
        }
        if (s->headers.size() < 8) { err = "no SPS/PPS"; delete s; return nullptr; }
    }

    // таблицы образцов
    uint32_t n = stsz.n >= 12 ? be32(stsz.p + 8) : 0;
    uint32_t fixedSize = stsz.n >= 12 ? be32(stsz.p + 4) : 0;
    if (n == 0 || n > 4000000 || (!fixedSize && stsz.n < 12 + (size_t)n * 4)) { err = "bad stsz"; delete s; return nullptr; }
    s->samples.resize(n);
    // размеры
    for (uint32_t i = 0; i < n; i++) s->samples[i].size = fixedSize ? fixedSize : be32(stsz.p + 12 + (size_t)i * 4);
    // смещения: stsc + stco/co64
    {
        const Box& co = hasCo64 ? co64 : stco;
        uint32_t nch = co.n >= 8 ? be32(co.p + 4) : 0;
        uint32_t nsc = stsc.n >= 8 ? be32(stsc.p + 4) : 0;
        if (!nch || !nsc || stsc.n < 8 + (size_t)nsc * 12 || co.n < 8 + (size_t)nch * (hasCo64 ? 8 : 4)) { err = "bad chunk tables"; delete s; return nullptr; }
        uint32_t si = 0;
        for (uint32_t c = 0; c < nch && si < n; c++) {
            // сколько образцов в этом куске
            uint32_t spc = 0;
            for (uint32_t k = 0; k < nsc; k++) {
                const uint8_t* ee = stsc.p + 8 + (size_t)k * 12;
                if (be32(ee) <= c + 1) spc = be32(ee + 4); else break;
            }
            uint64_t off = hasCo64 ? be64(co.p + 8 + (size_t)c * 8) : be32(co.p + 8 + (size_t)c * 4);
            for (uint32_t j = 0; j < spc && si < n; j++, si++) { s->samples[si].off = off; off += s->samples[si].size; }
        }
        if (si < n) { err = "chunk table too short"; delete s; return nullptr; }
    }
    // времена
    {
        std::vector<long long> dts(n, 0);
        uint32_t cnt = stts.n >= 8 ? be32(stts.p + 4) : 0;
        uint64_t t = 0; uint32_t si = 0;
        for (uint32_t k = 0; k < cnt && stts.n >= 8 + (size_t)(k + 1) * 8; k++) {
            uint32_t c = be32(stts.p + 8 + (size_t)k * 8), d = be32(stts.p + 12 + (size_t)k * 8);
            for (uint32_t j = 0; j < c && si < n; j++, si++) { dts[si] = (long long)t; t += d; }
        }
        std::vector<long long> off(n, 0);
        if (hasCtts && ctts.n >= 8) {
            uint32_t cc = be32(ctts.p + 4); si = 0;
            for (uint32_t k = 0; k < cc && ctts.n >= 8 + (size_t)(k + 1) * 8; k++) {
                uint32_t c = be32(ctts.p + 8 + (size_t)k * 8); int32_t d = (int32_t)be32(ctts.p + 12 + (size_t)k * 8);
                for (uint32_t j = 0; j < c && si < n; j++, si++) off[si] = d;
            }
        }
        for (uint32_t i = 0; i < n; i++) s->samples[i].ptsMs = (dts[i] + off[i] - mediaTime) * 1000 / timescale;
    }
    // ключевые
    if (hasStss && stss.n >= 8) {
        uint32_t cc = be32(stss.p + 4);
        for (uint32_t k = 0; k < cc && stss.n >= 8 + (size_t)(k + 1) * 4; k++) {
            uint32_t sn = be32(stss.p + 8 + (size_t)k * 4);
            if (sn >= 1 && sn <= n) s->samples[sn - 1].sync = true;
        }
    } else for (auto& x : s->samples) x.sync = true;

    if (!init_dec(s)) { err = "decoder init"; delete s; return nullptr; }
    logf_("swvideo: %ux%u, %u samples, timescale %lld, nal length %d, edit offset %lld", (unsigned)s->width, (unsigned)s->height, n, timescale, s->nalLen, mediaTime);
    return s;
}

void sw_close(SwVideo* s)
{
    if (!s) return;
    if (s->dec) WelsDestroyDecoder(s->dec);
    delete s;
}

int sw_width(SwVideo* s) { return s ? s->width : 0; }
int sw_height(SwVideo* s) { return s ? s->height : 0; }

static bool take(SwVideo* s, unsigned char* dst[3], SBufferInfo& info, SwFrame& out)
{
    if (info.iBufferStatus != 1) return false;
    out.w = info.UsrData.sSystemBuffer.iWidth; out.h = info.UsrData.sSystemBuffer.iHeight;
    out.strideY = info.UsrData.sSystemBuffer.iStride[0]; out.strideC = info.UsrData.sSystemBuffer.iStride[1];
    out.y = dst[0]; out.u = dst[1]; out.v = dst[2];
    out.ptsMs = (long long)info.uiOutYuvTimeStamp;
    (void)s;
    return out.w > 0 && out.h > 0 && out.y && out.u && out.v;
}

bool sw_next(SwVideo* s, SwFrame& out)
{
    if (!s || !s->dec) return false;
    for (;;) {
        unsigned char* dst[3] = { 0, 0, 0 };
        SBufferInfo info; memset(&info, 0, sizeof(info));
        if (s->idx >= s->samples.size()) {
            if (!s->flushing) s->flushing = true;
            s->dec->FlushFrame(dst, &info);
            return take(s, dst, info, out);
        }
        const Sample& sm = s->samples[s->idx++];
        if (sm.size == 0 || sm.size > 32u * 1024 * 1024) continue;
        std::vector<uint8_t> raw(sm.size);
        if (!read_file(s, sm.off, sm.size, raw.data())) { s->errors++; if (s->errors > 20) return false; continue; }
        s->au.clear();
        size_t p = 0;
        while (p + (size_t)s->nalLen <= raw.size()) {
            uint32_t l = 0;
            for (int i = 0; i < s->nalLen; i++) l = (l << 8) | raw[p + i];
            p += (size_t)s->nalLen;
            if (l == 0 || p + l > raw.size()) break;
            static const uint8_t sc[4] = { 0, 0, 0, 1 };
            s->au.insert(s->au.end(), sc, sc + 4);
            s->au.insert(s->au.end(), raw.begin() + (long)p, raw.begin() + (long)(p + l));
            p += l;
        }
        if (s->au.empty()) continue;
        size_t len = s->au.size();
        s->au.resize(len + 8, 0);
        info.uiInBsTimeStamp = (uint64_t)(sm.ptsMs < 0 ? 0 : sm.ptsMs);
        s->dec->DecodeFrame2(s->au.data(), (int)len, dst, &info);
        if (take(s, dst, info, out)) return true;
    }
}

void sw_seek(SwVideo* s, long long ms)
{
    if (!s) return;
    size_t i = 0;
    while (i < s->samples.size() && s->samples[i].ptsMs < ms) i++;
    if (i >= s->samples.size()) i = s->samples.size() - 1;
    while (i > 0 && !s->samples[i].sync) i--;
    s->idx = i;
    init_dec(s);
}
