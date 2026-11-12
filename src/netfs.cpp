// SPDX-License-Identifier: GPL-3.0-or-later
// Сетевые папки SMB как обычная папка на диске консоли через FUSE: так системный установщик
// читает .pkg прямо с шары без копирования.
// Патч ядра, включающий fusefs, таблица смещений и порт libfuse взяты из PS4Mount-PS4 (gasb07)
// и Itemzflow (LightningMods), оба GPL-3.0. Подробности: THIRD_PARTY.md.
#include "netfs.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <mutex>

#include "log.h"
#include "platform.h"
#include "smb.h"

namespace {

// "/alias/a/b" (путь внутри точки монтирования) -> "/smb/alias/a/b". 0 или errno.
int map_path(const std::string& p, std::string& smb, bool* is_root = nullptr) {
    if (is_root) *is_root = false;
    if (p.empty() || p[0] != '/') return EINVAL;
    if (p.find("/../") != std::string::npos || (p.size() >= 3 && p.compare(p.size() - 3, 3, "/..") == 0)) return EACCES;
    if (p == "/") { if (is_root) *is_root = true; smb = "/smb"; return 0; }
    size_t e = p.find('/', 1);
    std::string alias = p.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    SmbConn c;
    if (!smb_get(alias, c)) return ENOENT;
    smb = "/smb" + p;
    while (smb.size() > 1 && smb.back() == '/') smb.pop_back();
    return 0;
}

}  // namespace

int nf_getattr(const std::string& p, struct stat* st) {
    memset(st, 0, sizeof(*st));
    std::string sp; bool root = false;
    int e = map_path(p, sp, &root);
    if (e) return -e;
    st->st_uid = 0; st->st_gid = 0;
    st->st_blksize = 128 * 1024;  // ядро читает pkg блоками такого размера
    if (root || sp.find('/', 5) == std::string::npos) {   // "/" и "/alias" (корень шары: проверяем реальным stat)
        if (!root) { SmbStat s; int r = smb_stat(sp, s); if (r) return -r; }
        st->st_mode = S_IFDIR | 0555; st->st_nlink = 2;
        return 0;
    }
    SmbStat s;
    int r = smb_stat(sp, s);
    if (r) return -r;
    st->st_mode = (s.dir ? S_IFDIR | 0555 : S_IFREG | 0444);
    st->st_nlink = s.dir ? 2 : 1;
    st->st_size = (off_t)s.size;
    st->st_blocks = (s.size + 511) / 512;
    st->st_mtime = (time_t)s.mtime; st->st_atime = st->st_mtime; st->st_ctime = st->st_mtime;
    return 0;
}

int nf_readdir(const std::string& p, std::vector<std::string>& names) {
    std::string sp; bool root = false;
    int e = map_path(p, sp, &root);
    if (e) return -e;
    names.clear();
    if (root) { for (auto& c : smb_list()) names.push_back(c.alias); return 0; }
    std::vector<SmbEnt> ents;
    int r = smb_list_dir(sp, ents);
    if (r) return -r;
    for (auto& x : ents) names.push_back(x.name);
    return 0;
}

struct NfHandle {
    SmbFile* f = nullptr;
    uint64_t size = 0;
    std::vector<char> win;   // окно упреждающего чтения
    uint64_t win_off = 0;
};

const size_t kWindow = 1 << 20;   // сеть с большой задержкой: один запрос на мегабайт, а не на 128 КБ

int nf_open(const std::string& p, NfHandle** out) {
    std::string sp;
    int e = map_path(p, sp);
    if (e) return -e;
    SmbStat s;
    int r = smb_stat(sp, s);
    if (r) return -r;
    if (s.dir) return -EISDIR;
    NfHandle* h = new NfHandle();
    r = smb_open(sp, false, &h->f);
    if (r) { delete h; return -r; }
    h->size = s.size;
    *out = h;
    return 0;
}

// Сколько байт прочитано (0 = конец) или -errno. За конец файла не читаем никогда.
long nf_read(NfHandle* h, char* buf, size_t size, uint64_t off) {
    if (off >= h->size) return 0;
    if ((uint64_t)size > h->size - off) size = (size_t)(h->size - off);
    size_t done = 0;
    while (done < size) {
        uint64_t cur = off + done;
        if (!h->win.empty() && cur >= h->win_off && cur < h->win_off + h->win.size()) {
            size_t k = (size_t)std::min<uint64_t>(size - done, h->win_off + h->win.size() - cur);
            memcpy(buf + done, h->win.data() + (cur - h->win_off), k);
            done += k;
            continue;
        }
        size_t want = (size_t)std::min<uint64_t>(std::max<size_t>(size - done, kWindow), h->size - cur);
        h->win.assign(want, 0);
        h->win_off = cur;
        size_t got = 0;
        long r = 0;
        while (got < want) {
            r = smb_read(h->f, cur + got, h->win.data() + got, want - got);
            if (r <= 0) break;
            got += (size_t)r;
        }
        h->win.resize(got);
        if (got == 0) { h->win.clear(); return done ? (long)done : (r < 0 ? r : 0); }
    }
    return (long)done;
}

void nf_close(NfHandle* h) {
    if (!h) return;
    if (h->f) smb_close(h->f);
    delete h;
}

int nf_statfs(struct statvfs* sv) {
    memset(sv, 0, sizeof(*sv));
    sv->f_bsize = sv->f_frsize = 4096;
    sv->f_blocks = sv->f_bfree = sv->f_bavail = 1u << 20;
    sv->f_namemax = 255;
    return 0;
}


std::string netfs_map(const std::string& root, const std::string& smb_path) {
    return root + smb_path.substr(4);   // убираем "/smb"
}

#ifdef HOST_TEST
bool netfs_root(std::string&, std::string& err) { err = "no_fw"; return false; }
#else

#define FUSE_USE_VERSION 26
extern "C" {
#include <fuse.h>
#include <ps4/kernel.h>
}
#include <fcntl.h>
#include <pthread.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <unistd.h>

namespace {

/* Offsets of the fusefs patch, per firmware: itemzflow's (LightningMods/Itemzflow,
 * itemz-daemon/source/fuse.c), converted one for one. 13.52 and 6.72 are tested on a
 * console; the others are what itemzflow ships, marked experimental (nas_fw_tested). */
#define KBASE_OFF 0x1C0
#define NPOKE 16

typedef struct {
    uint32_t fw; /* major.minor, as 0x1352 */
    uint32_t ksuser, vfsconf, loader;
    int n;
    struct { uint32_t off; uint8_t v; } poke[NPOKE];
} fuse_kp_t;

const fuse_kp_t g_kps[] = {
    {0x505, 0x2300B88, 0x19FC380, 0x49DDB0, 16,
     {{0x3B219E, 0}, {0x49DFE4, 0xF}, {0x49DFE5, 0x84}, {0x49DDDE, 0xEB}, {0x49DDDF, 0x1B},
      {0x4A28EE, 0xEB}, {0x4A28EF, 0}, {0x4A29E2, 0xEB}, {0x4A2F34, 0xEB}, {0x4A30F7, 0xEB},
      {0x4A30F8, 0x4}, {0x4A384C, 0xEB}, {0x4A384D, 0}, {0x4A3BED, 0xEB}, {0x4A3BEE, 0x4},
      {0x4A27FC, 0xB6}}},
    {0x507, 0x2300B88, 0x19FC380, 0x49DDB0, 16,
     {{0x3B219E, 0}, {0x49DFE4, 0xF}, {0x49DFE5, 0x84}, {0x49DDDE, 0xEB}, {0x49DDDF, 0x1B},
      {0x4A28EE, 0xEB}, {0x4A28EF, 0}, {0x4A29E2, 0xEB}, {0x4A2F34, 0xEB}, {0x4A30F7, 0xEB},
      {0x4A30F8, 0x4}, {0x4A384C, 0xEB}, {0x4A384D, 0}, {0x4A3BED, 0xEB}, {0x4A3BEE, 0x4},
      {0x4A27FC, 0xB6}}},
    {0x672, 0x1BE9498, 0x1AA0A68, 0x4AF2D0, 16,
     {{0x270E8E, 0}, {0x4B0C74, 0xF}, {0x4B0C75, 0x84}, {0x4AF2FE, 0xEB}, {0x4AF2FF, 0x1B},
      {0x4AEB14, 0xEB}, {0x4AEB15, 0}, {0x4AEC12, 0xEB}, {0x4AF174, 0xEB}, {0x4AC086, 0xEB},
      {0x4AC087, 0x4}, {0x4AC7DC, 0xEB}, {0x4AC7DD, 0}, {0x4ACB8D, 0xEB}, {0x4ACB8E, 0x4},
      {0x4AEA2C, 0xB6}}},
    {0x900, 0x1B94588, 0x1A7F188, 0x490720, 13,
     {{0x43BCB6, 0}, {0x493875, 0x84}, {0x49074E, 0xEB}, {0x49074F, 0x1B}, {0x490AC5, 0xEB},
      {0x490AC4, 0}, {0x490BC2, 0xEB}, {0x491112, 0x84}, {0x492BD7, 0x85}, {0x49332A, 0x84},
      {0x4936DD, 0xEB}, {0x4936DE, 0x4}, {0x4909CE, 0xB6}}},
    {0x903, 0x1B90588, 0x1A7B188, 0x48E620, 13,
     {{0x439BD6, 0}, {0x491775, 0x84}, {0x48E64E, 0xEB}, {0x48E64F, 0x1B}, {0x48E9C5, 0xEB},
      {0x48E9C4, 0}, {0x48EAC2, 0xEB}, {0x48F012, 0x84}, {0x490AD7, 0x85}, {0x49122A, 0x84},
      {0x4915DD, 0xEB}, {0x4915DE, 0x4}, {0x48E8CE, 0xB6}}},
    {0x904, 0x1B90588, 0x1A7B188, 0x48E620, 13,
     {{0x439BD6, 0}, {0x491775, 0x84}, {0x48E64E, 0xEB}, {0x48E64F, 0x1B}, {0x48E9C5, 0xEB},
      {0x48E9C4, 0}, {0x48EAC2, 0xEB}, {0x48F012, 0x84}, {0x490AD7, 0x85}, {0x49122A, 0x84},
      {0x4915DD, 0xEB}, {0x4915DE, 0x4}, {0x48E8CE, 0xB6}}},
    {0x950, 0x221F654, 0x1A734A8, 0x490D10, 13,
     {{0x414196, 0}, {0x48DDB5, 0x84}, {0x490D3E, 0xEB}, {0x490D3F, 0x1B}, {0x493385, 0xEB},
      {0x493384, 0}, {0x493482, 0xEB}, {0x4939D2, 0x84}, {0x493B97, 0x85}, {0x4942EA, 0x84},
      {0x49469D, 0xEB}, {0x49469E, 0x4}, {0x49328E, 0xB6}}},
    {0x951, 0x221F654, 0x1A734A8, 0x490D10, 13,
     {{0x414196, 0}, {0x48DDB5, 0x84}, {0x490D3E, 0xEB}, {0x490D3F, 0x1B}, {0x493385, 0xEB},
      {0x493384, 0}, {0x493482, 0xEB}, {0x4939D2, 0x84}, {0x493B97, 0x85}, {0x4942EA, 0x84},
      {0x49469D, 0xEB}, {0x49469E, 0x4}, {0x49328E, 0xB6}}},
    {0x960, 0x221F654, 0x1A734A8, 0x490D10, 13,
     {{0x414196, 0}, {0x48DDB5, 0x84}, {0x490D3E, 0xEB}, {0x490D3F, 0x1B}, {0x493385, 0xEB},
      {0x493384, 0}, {0x493482, 0xEB}, {0x4939D2, 0x84}, {0x493B97, 0x85}, {0x4942EA, 0x84},
      {0x49469D, 0xEB}, {0x49469E, 0x4}, {0x49328E, 0xB6}}},
    {0x1000, 0x225AC98, 0x1A7B9F8, 0x493560, 13,
     {{0x3B256, 0}, {0x48E965, 0x84}, {0x49358E, 0xEB}, {0x49358F, 0x1B}, {0x4945A5, 0xEB},
      {0x4945A4, 0}, {0x4946A2, 0xEB}, {0x494BF2, 0x84}, {0x493797, 0x85}, {0x493EEA, 0x84},
      {0x49429D, 0xEB}, {0x49429E, 0x4}, {0x4944AE, 0xB6}}},
    {0x1001, 0x225AC98, 0x1A7B9F8, 0x493560, 13,
     {{0x3B256, 0}, {0x48E965, 0x84}, {0x49358E, 0xEB}, {0x49358F, 0x1B}, {0x4945A5, 0xEB},
      {0x4945A4, 0}, {0x4946A2, 0xEB}, {0x494BF2, 0x84}, {0x493797, 0x85}, {0x493EEA, 0x84},
      {0x49429D, 0xEB}, {0x49429E, 0x4}, {0x4944AE, 0xB6}}},
    {0x1050, 0x2269D68, 0x1A7BC70, 0x495B30, 13,
     {{0x420976, 0}, {0x48F775, 0x84}, {0x495B5E, 0xEB}, {0x495B5F, 0x1B}, {0x48EF65, 0xEB},
      {0x48EF64, 0}, {0x48F062, 0xEB}, {0x48F5B2, 0x84}, {0x48E157, 0x85}, {0x48E8AA, 0x84},
      {0x48EC5D, 0xEB}, {0x48EC5E, 0x4}, {0x48EE6E, 0xB6}}},
    {0x1070, 0x2269D68, 0x1A7BC70, 0x495B30, 13,
     {{0x420976, 0}, {0x48F775, 0x84}, {0x495B5E, 0xEB}, {0x495B5F, 0x1B}, {0x48EF65, 0xEB},
      {0x48EF64, 0}, {0x48F062, 0xEB}, {0x48F5B2, 0x84}, {0x48E157, 0x85}, {0x48E8AA, 0x84},
      {0x48EC5D, 0xEB}, {0x48EC5E, 0x4}, {0x48EE6E, 0xB6}}},
    {0x1071, 0x2269D68, 0x1A7BC70, 0x495B30, 13,
     {{0x420976, 0}, {0x48F775, 0x84}, {0x495B5E, 0xEB}, {0x495B5F, 0x1B}, {0x48EF65, 0xEB},
      {0x48EF64, 0}, {0x48F062, 0xEB}, {0x48F5B2, 0x84}, {0x48E157, 0x85}, {0x48E8AA, 0x84},
      {0x48EC5D, 0xEB}, {0x48EC5E, 0x4}, {0x48EE6E, 0xB6}}},
    {0x1100, 0x22A6828, 0x1A7FB98, 0x494970, 13,
     {{0x354D86, 0}, {0x48F9C5, 0x84}, {0x49499E, 0xEB}, {0x49499F, 0x1B}, {0x495EA4, 0},
      {0x495EA5, 0xEB}, {0x495FA2, 0xEB}, {0x4964F2, 0x84}, {0x492987, 0x85}, {0x4930DA, 0x84},
      {0x49348D, 0xEB}, {0x49348E, 0x4}, {0x495DAE, 0xB6}}},
    {0x1102, 0x22A6828, 0x1A7FB98, 0x494900, 13,
     {{0x354DA6, 0}, {0x48F955, 0x84}, {0x49492E, 0xEB}, {0x49492F, 0x1B}, {0x495E34, 0},
      {0x495E35, 0xEB}, {0x495F32, 0xEB}, {0x496482, 0x84}, {0x492917, 0x85}, {0x49306A, 0x84},
      {0x49341D, 0xEB}, {0x49341E, 0x4}, {0x495D3E, 0xB6}}},
    {0x1150, 0x21AD2AC, 0x1A7F9F8, 0x495120, 13,
     {{0x3175E6, 0}, {0x491175, 0x84}, {0x49514E, 0xEB}, {0x49514F, 0x1B}, {0x48FB84, 0},
      {0x48FB85, 0xEB}, {0x48FC82, 0xEB}, {0x4901D2, 0x84}, {0x494137, 0x85}, {0x49488A, 0x84},
      {0x494C3D, 0xEB}, {0x494C3E, 0x4}, {0x48FA8E, 0xB6}}},
    {0x1152, 0x21AD2AC, 0x1A7F9F8, 0x495120, 13,
     {{0x3175E6, 0}, {0x491175, 0x84}, {0x49514E, 0xEB}, {0x49514F, 0x1B}, {0x48FB84, 0},
      {0x48FB85, 0xEB}, {0x48FC82, 0xEB}, {0x4901D2, 0x84}, {0x494137, 0x85}, {0x49488A, 0x84},
      {0x494C3D, 0xEB}, {0x494C3E, 0x4}, {0x48FA8E, 0xB6}}},
    {0x1200, 0x21AD2AC, 0x1A7FA48, 0x495360, 13,
     {{0x317826, 0}, {0x4913B5, 0x84}, {0x49538E, 0xEB}, {0x49538F, 0x1B}, {0x48FDC4, 0},
      {0x48FDC5, 0xEB}, {0x48FEC2, 0xEB}, {0x490412, 0x84}, {0x494377, 0x85}, {0x494ACA, 0x84},
      {0x494E7D, 0xEB}, {0x494E7E, 0x4}, {0x48FCCE, 0xB6}}},
    {0x1202, 0x21AD2AC, 0x1A7FA48, 0x495360, 13,
     {{0x317826, 0}, {0x4913B5, 0x84}, {0x49538E, 0xEB}, {0x49538F, 0x1B}, {0x48FDC4, 0},
      {0x48FDC5, 0xEB}, {0x48FEC2, 0xEB}, {0x490412, 0x84}, {0x494377, 0x85}, {0x494ACA, 0x84},
      {0x494E7D, 0xEB}, {0x494E7E, 0x4}, {0x48FCCE, 0xB6}}},
    {0x1250, 0x21AD2AC, 0x1A7FA48, 0x4953A0, 13,
     {{0x317866, 0}, {0x4913F5, 0x84}, {0x4953CE, 0xEB}, {0x4953CF, 0x1B}, {0x48FE05, 0xEB},
      {0x48FE04, 0}, {0x48FF02, 0xEB}, {0x490452, 0x84}, {0x4943B7, 0x85}, {0x494B0A, 0x84},
      {0x494EBD, 0xEB}, {0x494EBE, 0x4}, {0x48FD0E, 0xB6}}},
    {0x1252, 0x21AD2AC, 0x1A7FA48, 0x4953A0, 13,
     {{0x317866, 0}, {0x4913F5, 0x84}, {0x4953CE, 0xEB}, {0x4953CF, 0x1B}, {0x48FE05, 0xEB},
      {0x48FE04, 0}, {0x48FF02, 0xEB}, {0x490452, 0x84}, {0x4943B7, 0x85}, {0x494B0A, 0x84},
      {0x494EBD, 0xEB}, {0x494EBE, 0x4}, {0x48FD0E, 0xB6}}},
    {0x1300, 0x21AD2AC, 0x1A7FA48, 0x4953C0, 13,
     {{0x317886, 0}, {0x491415, 0x84}, {0x4953EE, 0xEB}, {0x4953EF, 0x1B}, {0x48FE24, 0},
      {0x48FE25, 0xEB}, {0x48FF22, 0xEB}, {0x490472, 0x84}, {0x4943D7, 0x85}, {0x494B2A, 0x84},
      {0x494EDD, 0xEB}, {0x494EDE, 0x4}, {0x48FD2E, 0xB6}}},
    {0x1302, 0x21AD2AC, 0x1A7FA48, 0x4953D0, 13,
     {{0x317896, 0}, {0x491425, 0x84}, {0x4953FE, 0xEB}, {0x4953FF, 0x1B}, {0x48FE34, 0},
      {0x48FE35, 0xEB}, {0x48FF32, 0xEB}, {0x490482, 0x84}, {0x4943E7, 0x85}, {0x494B3A, 0x84},
      {0x494EED, 0xEB}, {0x494EEE, 0x4}, {0x48FD3E, 0xB6}}},
    {0x1304, 0x21AD2AC, 0x1A7FA48, 0x4953D0, 13,
     {{0x317896, 0}, {0x491425, 0x84}, {0x4953FE, 0xEB}, {0x4953FF, 0x1B}, {0x48FE34, 0},
      {0x48FE35, 0xEB}, {0x48FF32, 0xEB}, {0x490482, 0x84}, {0x4943E7, 0x85}, {0x494B3A, 0x84},
      {0x494EED, 0xEB}, {0x494EEE, 0x4}, {0x48FD3E, 0xB6}}},
    {0x1350, 0x21AD2AC, 0x1A7FA48, 0x495810, 13,
     {{0x317BE6, 0}, {0x491865, 0x84}, {0x49583E, 0xEB}, {0x49583F, 0x1B}, {0x490274, 0},
      {0x490275, 0xEB}, {0x490372, 0xEB}, {0x4908C2, 0x84}, {0x494826, 0x85}, {0x494F7A, 0x84},
      {0x49532D, 0xEB}, {0x49532E, 0x4}, {0x49017E, 0xB6}}},
    {0x1352, 0x21AD2AC, 0x1A7FA48, 0x495C10, 13,
     {{0x317FE6, 0}, {0x491C65, 0x84}, {0x495C3E, 0xEB}, {0x495C3F, 0x1B}, {0x490674, 0},
      {0x490675, 0xEB}, {0x490772, 0xEB}, {0x490CC2, 0x84}, {0x494C27, 0x85}, {0x49537A, 0x84},
      {0x49572D, 0xEB}, {0x49572E, 0x4}, {0x49057E, 0xB6}}},
};


const fuse_kp_t* kp_for(uint32_t fw) {
    for (size_t i = 0; i < sizeof(g_kps) / sizeof(g_kps[0]); i++)
        if (g_kps[i].fw == fw >> 16) return &g_kps[i];
    return nullptr;
}
const fuse_kp_t* g_kp;

__attribute__((no_stack_protector)) static uint64_t kbase(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000082));
    return (((uint64_t)hi << 32) | lo) - KBASE_OFF;
}

__attribute__((no_stack_protector)) static void wp_write(int enable) {
    uint64_t cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    if (enable)
        cr0 |= (1ull << 16);
    else
        cr0 &= ~(1ull << 16);
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

__attribute__((no_stack_protector)) static int fuse_patch(void *td) {
    (void)td;
    const fuse_kp_t *kp = g_kp;
    uint8_t *k = (uint8_t *)kbase();
    int *ksuser = (int *)(k + kp->ksuser);
    if (*ksuser == 1)
        return 1; /* already done (another payload, or a previous start) */

    wp_write(0);
    struct vfsconf *p = (struct vfsconf *)(k + kp->vfsconf);
    *ksuser = 1;
    p->vfc_flags = 0x00400000 | 0x00080000; /* VFCF_JAIL | VFCF_SYNTHETIC */
    for (int i = 0; i < kp->n; i++)
        k[kp->poke[i].off] = kp->poke[i].v;
    wp_write(1);

    int (*loader)(void *, int, void *) = reinterpret_cast<int (*)(void *, int, void *)>(k + kp->loader);
    return loader(NULL, 0, NULL);
}


std::mutex g_mtx;
struct fuse* g_fuse = nullptr;
std::string g_root, g_fail;
bool g_mounted = false;

// --- операции FUSE ---
int fs_getattr(const char* path, struct stat* st) { return nf_getattr(path, st); }
int fs_readdir(const char* path, void* buf, fuse_fill_dir_t fill, off_t, struct fuse_file_info*) {
    std::vector<std::string> names;
    int r = nf_readdir(path, names);
    if (r < 0) return r;
    fill(buf, ".", nullptr, 0);
    fill(buf, "..", nullptr, 0);
    for (auto& n : names) fill(buf, n.c_str(), nullptr, 0);
    return 0;
}
int fs_open(const char* path, struct fuse_file_info* fi) {
    // Открытие на запись не отклоняем: BGFT открывает pkg так при запуске, ошибка записалась бы в задачу. Сама запись не поддерживается.
    NfHandle* h = nullptr;
    int r = nf_open(path, &h);
    if (r < 0) { app_log("netfs: open %s -> %d", path, r); return r; }
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}
int fs_read(const char* path, char* buf, size_t size, off_t off, struct fuse_file_info* fi) {
    long r = nf_read((NfHandle*)(uintptr_t)fi->fh, buf, size, (uint64_t)off);
    if (r < 0) app_log("netfs: read %s @%lld -> %ld", path, (long long)off, r);
    return (int)r;
}
int fs_release(const char*, struct fuse_file_info* fi) { nf_close((NfHandle*)(uintptr_t)fi->fh); return 0; }
int fs_statfs(const char*, struct statvfs* sv) { return nf_statfs(sv); }
int fs_flush(const char*, struct fuse_file_info*) { return 0; }
int fs_access(const char* path, int) { struct stat st; return nf_getattr(path, &st); }

void* fuse_thread(void*) {
    app_log("netfs: %s: обслуживание запущено", g_root.c_str());
    int r = fuse_loop(g_fuse);
    app_log("netfs: fuse_loop вернул %d", r);
    std::lock_guard<std::mutex> l(g_mtx);
    g_mounted = false;
    return nullptr;
}

bool is_mount(const std::string& dir) {
    struct stat a, b;
    return stat(dir.c_str(), &a) == 0 && stat("/mnt", &b) == 0 && a.st_dev != b.st_dev;
}
bool mounted_at(const std::string& path) {
    int n = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (n <= 0) return false;
    std::vector<struct statfs> list((size_t)n + 8);
    n = getfsstat(list.data(), (long)(list.size() * sizeof(struct statfs)), MNT_NOWAIT);
    for (int i = 0; i < n; i++) if (path == list[(size_t)i].f_mntonname || "/user" + path == list[(size_t)i].f_mntonname) return true;
    return false;
}

bool probe_devices(bool* enxio) {
    *enxio = true;
    for (int i = 0; i < 8; i++) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/fuse%d", i);
        int fd = open(dev, O_RDWR);
        if (fd >= 0) { close(fd); return true; }
        app_log("netfs: open %s: %d", dev, errno);
        if (errno != ENXIO) *enxio = false;
    }
    return false;
}

}  // namespace

bool netfs_root(std::string& root, std::string& err) {
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_mounted) { root = g_root; return true; }
    if (!g_fail.empty()) { err = g_fail; return false; }   // ядро второй раз не трогаем
    // Где монтировать. Системный установщик хорошо читает /data/pkg (проверено на консоли), поэтому сначала там;
    // если ядро откажется - на флешке (PS4Mount монтирует только туда).
    std::vector<std::string> parents = {"/data/pkg/ps4commander_nas"};
    for (int i = 0; i < 8; i++) {
        std::string m = "/mnt/usb" + std::to_string(i);
        if (is_mount(m)) { parents.push_back(m + "/ps4commander_nas"); break; }
    }
    g_kp = kp_for(kernel_get_fw_version());
    if (!g_kp && access("/dev/fuse0", F_OK)) { err = g_fail = "no_fw"; app_log("netfs: для этой прошивки нет патча fusefs"); return false; }
    if (access("/dev/fuse0", F_OK)) {
        errno = 0;
        long pr = syscall(11, (long)fuse_patch);
        app_log("netfs: патч fusefs: %ld (errno %d)", pr, errno);
        if (access("/dev/fuse0", F_OK)) { err = g_fail = "no_fuse"; return false; }
    }
    bool enxio = false;
    if (!probe_devices(&enxio)) {
        err = enxio ? "reload" : "no_fuse";
        if (!enxio) g_fail = err;
        app_log("netfs: устройства fuse недоступны (%s)", err.c_str());
        return false;
    }
    static struct fuse_operations ops;
    memset(&ops, 0, sizeof(ops));
    ops.getattr = fs_getattr; ops.readdir = fs_readdir; ops.open = fs_open; ops.read = fs_read;
    ops.release = fs_release; ops.statfs = fs_statfs; ops.flush = fs_flush; ops.access = fs_access;
    std::string mnt;
    err = "mount";
    for (auto& parent : parents) {
        mkdir("/data/pkg", 0777);
        mkdir(parent.c_str(), 0777);
        std::string c;
        for (int i = 1; i <= 8 && c.empty(); i++) {
            std::string d = parent + "/nas-" + std::to_string(i);
            if (mounted_at(d)) continue;   // монтирование прошлого запуска не размонтировать: берём новое имя
            if (mkdir(d.c_str(), 0777) == 0 || errno == EEXIST) c = d;
        }
        if (c.empty()) { app_log("netfs: в %s нет свободной точки монтирования", parent.c_str()); continue; }
        char a0[] = "ps4commander", a2[] = "-o", a3[] = "allow_other", a4[] = "-o", a5[] = "rw", a6[] = "-f";
        char* argv[] = {a0, const_cast<char*>(c.c_str()), a2, a3, a4, a5, a6, nullptr};
        char* mountpoint = nullptr;
        int mt = 0;
        g_fuse = fuse_setup(7, argv, &ops, sizeof(ops), &mountpoint, &mt, nullptr);
        if (g_fuse) { mnt = c; break; }
        bool en = false; probe_devices(&en);
        app_log("netfs: ядро отказалось монтировать %s%s", c.c_str(), en ? " (нужна перезагрузка payload)" : "");
        if (en) { err = "reload"; return false; }
    }
    if (mnt.empty()) { g_fail = err; return false; }
    g_root = mnt;
    g_mounted = true;
    pthread_t th;
    if (pthread_create(&th, nullptr, fuse_thread, nullptr)) { g_mounted = false; err = g_fail = "mount"; return false; }
    pthread_detach(th);
    app_log("netfs: смонтировано в %s", mnt.c_str());
    root = g_root;
    return true;
}
#endif
