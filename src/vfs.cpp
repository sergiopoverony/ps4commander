#include "vfs.h"
#include "smb.h"

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

int v_lstat(const std::string& p, VStat& st) {
    st = VStat();
    if (smb_is_path(p)) {
        SmbStat s;
        int e = smb_stat(p, s);
        if (e) return e;
        st.dir = s.dir; st.lnk = s.link; st.reg = !s.dir && !s.link; st.size = s.size; st.mtime = s.mtime;
        st.mode = s.dir ? 0755 : 0644;
        return 0;
    }
    struct stat x;
    if (lstat(p.c_str(), &x) != 0) return errno;
    st.dir = S_ISDIR(x.st_mode); st.lnk = S_ISLNK(x.st_mode); st.reg = S_ISREG(x.st_mode);
    st.size = (uint64_t)x.st_size; st.mtime = (int64_t)x.st_mtime; st.mode = x.st_mode & 07777;
    return 0;
}

int v_list(const std::string& p, std::vector<std::string>& names) {
    if (smb_is_path(p)) {
        if (p == "/smb") {
            for (auto& c : smb_list()) names.push_back(c.alias);
            return 0;
        }
        std::vector<SmbEnt> ents;
        int e = smb_list_dir(p, ents);
        if (e) return e;
        for (auto& x : ents) names.push_back(x.name);
        return 0;
    }
    DIR* d = opendir(p.c_str());
    if (!d) return errno;
    while (struct dirent* e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names.push_back(e->d_name);
    }
    closedir(d);
    return 0;
}

int v_mkdir(const std::string& p, uint32_t mode) {
    if (smb_is_path(p)) return smb_mkdir(p);
    return mkdir(p.c_str(), mode) == 0 ? 0 : errno;
}
int v_rmdir(const std::string& p) {
    if (smb_is_path(p)) return smb_rmdir(p);
    return rmdir(p.c_str()) == 0 ? 0 : errno;
}
int v_unlink(const std::string& p) {
    if (smb_is_path(p)) return smb_unlink(p);
    return unlink(p.c_str()) == 0 ? 0 : errno;
}
int v_rename(const std::string& a, const std::string& b) {
    bool sa = smb_is_path(a), sb = smb_is_path(b);
    if (sa || sb) return sa && sb ? smb_rename(a, b) : EXDEV;
    return rename(a.c_str(), b.c_str()) == 0 ? 0 : errno;
}
int v_chmod(const std::string& p, uint32_t mode) {
    if (smb_is_path(p)) return 0;
    return chmod(p.c_str(), mode) == 0 ? 0 : errno;
}

int v_open_read(const std::string& p, VFile& f) {
    f = VFile();
    if (smb_is_path(p)) return smb_open(p, false, &f.sf);
    f.fd = open(p.c_str(), O_RDONLY);
    return f.fd < 0 ? errno : 0;
}
int v_open_write(const std::string& p, VFile& f, uint32_t mode) {
    f = VFile();
    if (smb_is_path(p)) return smb_open(p, true, &f.sf);
    f.fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    return f.fd < 0 ? errno : 0;
}
long v_read(VFile& f, void* buf, size_t n) {
    if (f.sf) {
        long r = smb_read(f.sf, f.off, buf, n);
        if (r > 0) f.off += (uint64_t)r;
        return r;
    }
    for (;;) {
        ssize_t r = read(f.fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -(long)errno : (long)r;
    }
}
long v_write(VFile& f, const void* buf, size_t n) {
    if (f.sf) {
        long r = smb_write(f.sf, f.off, buf, n);
        if (r > 0) f.off += (uint64_t)r;
        return r;
    }
    for (;;) {
        ssize_t r = write(f.fd, buf, n);
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -(long)errno : (long)r;
    }
}
long v_pread(VFile& f, uint64_t off, void* buf, size_t n) {
    if (f.sf) return smb_read(f.sf, off, buf, n);
    ssize_t r = pread(f.fd, buf, n, (off_t)off);
    return r < 0 ? -(long)errno : (long)r;
}
void v_seek(VFile& f, uint64_t off) {
    f.off = off;
    if (!f.sf && f.fd >= 0) lseek(f.fd, (off_t)off, SEEK_SET);
}
void v_close(VFile& f) {
    if (f.sf) smb_close(f.sf);
    else if (f.fd >= 0) close(f.fd);
    f = VFile();
}
