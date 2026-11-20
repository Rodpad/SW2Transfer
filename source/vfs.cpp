#include "vfs.h"
#include <atomic>
#include <algorithm>
#include <functional>
#include <vector>
#include <map>
#include <mutex>
#include <chrono>
#include <string.h>
#include <stdio.h>

#ifdef __SWITCH__
#include <switch.h>
#else
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace vfs {


static double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

struct File {
  std::string path;
  bool writable = false;
#ifdef __SWITCH__
  FsFile fh{};
  bool open = false;
#else
  int fd = -1;
#endif
  ~File();
};

// ---------------------------------------------------------------- platform
#ifdef __SWITCH__
static FsFileSystem* sd = nullptr;
static std::string resStr(Result rc) {
  char b[48]; snprintf(b, sizeof b, "error 2%03u-%04u (0x%x)", R_MODULE(rc), R_DESCRIPTION(rc), rc); return b;
}
struct FsPath { char p[FS_MAX_PATH]; FsPath(const std::string& s) { memset(p, 0, sizeof p); strncpy(p, s.c_str(), sizeof p - 1); } };
File::~File() { if (open) { if (writable) fsFileFlush(&fh); fsFileClose(&fh); } }
void init(const std::string&) { sd = fsdevGetDeviceFileSystem("sdmc"); }
static bool pOpen(File& f, bool write, bool truncate, int64_t presize, std::string& err) {
  FsPath p(f.path);
  Result rc;
  if (write) {
    rc = fsFsCreateFile(sd, p.p, 0, 0);  // fails harmlessly if it exists
    (void)rc;
    rc = fsFsOpenFile(sd, p.p, FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append, &f.fh);
  } else {
    rc = fsFsOpenFile(sd, p.p, FsOpenMode_Read, &f.fh);
  }
  if (R_FAILED(rc)) { err = resStr(rc); return false; }
  f.open = true; f.writable = write;
  if (write && truncate) fsFileSetSize(&f.fh, 0);
  if (write && presize >= 0) {
    rc = fsFileSetSize(&f.fh, presize);
    if (R_FAILED(rc)) { err = "cannot set size: " + resStr(rc); return false; }
  }
  return true;
}
int64_t read(File* f, int64_t off, void* buf, size_t len) {
  u64 got = 0;
  if (R_FAILED(fsFileRead(&f->fh, off, buf, len, FsReadOption_None, &got))) return -1;
  return (int64_t)got;
}
bool write(File* f, int64_t off, const void* buf, size_t len) {
  return R_SUCCEEDED(fsFileWrite(&f->fh, off, buf, len, FsWriteOption_None));
}
int64_t size(File* f) { s64 s = 0; if (R_FAILED(fsFileGetSize(&f->fh, &s))) return -1; return s; }
Kind stat(const std::string& path, int64_t* sz) {
  FsDirEntryType t; FsPath p(path);
  if (path == "/") return DIR_;
  if (R_FAILED(fsFsGetEntryType(sd, p.p, &t))) return NONE;
  if (t == FsDirEntryType_Dir) return DIR_;
  if (sz) {
    FsFile fh; *sz = 0;
    if (R_SUCCEEDED(fsFsOpenFile(sd, p.p, FsOpenMode_Read, &fh))) { s64 s = 0; fsFileGetSize(&fh, &s); *sz = s; fsFileClose(&fh); }
  }
  return FILE_;
}
int64_t modifiedTime(const std::string& path) {
  FsTimeStampRaw ts; memset(&ts, 0, sizeof ts); FsPath p(path);
  return R_SUCCEEDED(fsFsGetFileTimeStampRaw(sd, p.p, &ts)) && ts.is_valid ? (int64_t)ts.modified : 0;
}
bool list(const std::string& path, std::vector<Entry>& out, std::string& err) {
  FsDir d; FsPath p(path);
  Result rc = fsFsOpenDirectory(sd, p.p, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d);
  if (R_FAILED(rc)) { err = resStr(rc); return false; }
  std::vector<FsDirectoryEntry> buf(256);
  for (;;) {
    s64 n = 0;
    rc = fsDirRead(&d, &n, buf.size(), buf.data());
    if (R_FAILED(rc)) { err = resStr(rc); fsDirClose(&d); return false; }
    if (n <= 0) break;
    for (s64 i = 0; i < n; i++) out.push_back({buf[i].name, buf[i].type == FsDirEntryType_Dir, buf[i].file_size});
  }
  fsDirClose(&d);
  // Created / modified times: one call per entry (the listing runs on the
  // disk pool, so a big folder does not hold up transfers).
  for (auto& e : out) {
    FsTimeStampRaw ts; memset(&ts, 0, sizeof ts);
    FsPath ep(path == "/" ? "/" + e.name : path + "/" + e.name);
    if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(sd, ep.p, &ts)) && ts.is_valid) { e.created = (int64_t)ts.created; e.modified = (int64_t)ts.modified; }
  }
  return true;
}
bool mkdir(const std::string& path, std::string& err) {
  FsPath p(path); Result rc = fsFsCreateDirectory(sd, p.p);
  if (R_FAILED(rc)) { err = resStr(rc); return false; } return true;
}
bool removeFile(const std::string& path, std::string& err) {
  release(path);
  FsPath p(path); Result rc = fsFsDeleteFile(sd, p.p);
  if (R_FAILED(rc)) { err = resStr(rc); return false; } return true;
}
bool removeDir(const std::string& path, bool recursive, std::string& err) {
  if (path == "/") { err = "refusing to delete the root"; return false; }
  reap(true);
  FsPath p(path); Result rc = recursive ? fsFsDeleteDirectoryRecursively(sd, p.p) : fsFsDeleteDirectory(sd, p.p);
  if (R_FAILED(rc)) { err = resStr(rc); return false; } return true;
}
bool rename(const std::string& from, const std::string& to, std::string& err) {
  Kind k = stat(from);
  if (k == NONE) { err = "not found"; return false; }
  if (k == FILE_) release(from); else reap(true);
  FsPath a(from), b(to);
  Result rc = k == DIR_ ? fsFsRenameDirectory(sd, a.p, b.p) : fsFsRenameFile(sd, a.p, b.p);
  if (R_FAILED(rc)) { err = resStr(rc); return false; } return true;
}
int64_t freeSpace() { s64 s = 0; FsPath p("/"); if (R_FAILED(fsFsGetFreeSpace(sd, p.p, &s))) return -1; return s; }
#else
static std::string root;
static std::string real(const std::string& path) { return root + path; }
File::~File() { if (fd >= 0) close(fd); }
void init(const std::string& hostRoot) { root = hostRoot; while (root.size() > 1 && root.back() == '/') root.pop_back(); }
static bool pOpen(File& f, bool write, bool truncate, int64_t presize, std::string& err) {
  f.fd = ::open(real(f.path).c_str(), write ? (O_RDWR | O_CREAT) : O_RDONLY, 0644);
  if (f.fd < 0) { err = strerror(errno); return false; }
  f.writable = write;
  if (write && truncate) (void)ftruncate(f.fd, 0);
  if (write && presize >= 0 && ftruncate(f.fd, presize) != 0) { err = strerror(errno); return false; }
  return true;
}
int64_t read(File* f, int64_t off, void* buf, size_t len) {
  size_t got = 0;
  while (got < len) {
    ssize_t n = pread(f->fd, (char*)buf + got, len - got, off + got);
    if (n < 0) { if (errno == EINTR) continue; return -1; }
    if (n == 0) break;
    got += n;
  }
  return (int64_t)got;
}
bool write(File* f, int64_t off, const void* buf, size_t len) {
  size_t put = 0;
  while (put < len) {
    ssize_t n = pwrite(f->fd, (const char*)buf + put, len - put, off + put);
    if (n < 0) { if (errno == EINTR) continue; return false; }
    put += n;
  }
  return true;
}
int64_t size(File* f) { struct stat st; if (fstat(f->fd, &st)) return -1; return st.st_size; }
Kind stat(const std::string& path, int64_t* sz) {
  struct stat st;
  if (::stat(real(path).c_str(), &st)) return NONE;
  if (S_ISDIR(st.st_mode)) return DIR_;
  if (sz) *sz = st.st_size;
  return FILE_;
}
int64_t modifiedTime(const std::string& path) {
  struct stat st;
  return ::stat(real(path).c_str(), &st) == 0 ? (int64_t)st.st_mtime : 0;
}
bool list(const std::string& path, std::vector<Entry>& out, std::string& err) {
  DIR* d = opendir(real(path).c_str());
  if (!d) { err = strerror(errno); return false; }
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    struct stat st;
    if (::stat(real(path == "/" ? "/" + n : path + "/" + n).c_str(), &st)) continue;
    bool dir = S_ISDIR(st.st_mode);
#ifdef __APPLE__
    int64_t created = st.st_birthtimespec.tv_sec;
#else
    int64_t created = st.st_ctim.tv_sec;
#endif
    out.push_back({n, dir, dir ? 0 : (int64_t)st.st_size, created, (int64_t)st.st_mtime});
  }
  closedir(d);
  return true;
}
bool mkdir(const std::string& path, std::string& err) {
  if (::mkdir(real(path).c_str(), 0755)) { err = strerror(errno); return false; } return true;
}
bool removeFile(const std::string& path, std::string& err) {
  release(path);
  if (unlink(real(path).c_str())) { err = strerror(errno); return false; } return true;
}
static bool rmTree(const std::string& rp) {
  DIR* d = opendir(rp.c_str());
  if (!d) return false;
  while (dirent* e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string c = rp + "/" + n; struct stat st;
    if (lstat(c.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) rmTree(c); else unlink(c.c_str());
  }
  closedir(d);
  return rmdir(rp.c_str()) == 0;
}
bool removeDir(const std::string& path, bool recursive, std::string& err) {
  if (path == "/") { err = "refusing to delete the root"; return false; }
  reap(true);
  bool ok = recursive ? rmTree(real(path)) : rmdir(real(path).c_str()) == 0;
  if (!ok) err = strerror(errno);
  return ok;
}
bool rename(const std::string& from, const std::string& to, std::string& err) {
  release(from);
  if (::rename(real(from).c_str(), real(to).c_str())) { err = strerror(errno); return false; } return true;
}
int64_t freeSpace() { struct statvfs s; if (statvfs(root.c_str(), &s)) return -1; return (int64_t)s.f_bavail * s.f_frsize; }
#endif

// ---------------------------------------------------------------- common
std::string resolve(const std::string& cwd, const std::string& arg) {
  std::string full = (!arg.empty() && arg[0] == '/') ? arg : cwd + "/" + arg;
  std::vector<std::string> parts;
  size_t i = 0;
  while (i <= full.size()) {
    size_t j = full.find('/', i);
    if (j == std::string::npos) j = full.size();
    std::string p = full.substr(i, j - i);
    if (p == "..") { if (!parts.empty()) parts.pop_back(); }
    else if (!p.empty() && p != ".") parts.push_back(p);
    i = j + 1;
  }
  std::string out;
  for (auto& p : parts) out += "/" + p;
  return out.empty() ? "/" : out;
}
std::string parentOf(const std::string& path) {
  size_t s = path.rfind('/');
  return (s == std::string::npos || s == 0) ? "/" : path.substr(0, s);
}
std::string baseName(const std::string& path) {
  size_t s = path.rfind('/');
  return s == std::string::npos ? path : path.substr(s + 1);
}
const std::string& pathOf(File* f) { return f->path; }

bool mkdirs(const std::string& path) {
  if (path == "/" || stat(path) == DIR_) return true;
  if (!mkdirs(parentOf(path))) return false;
  std::string err;
  mkdir(path, err);
  return stat(path) == DIR_;
}

// Handle cache: writers share one handle per path; readers' handles linger a
// little so a client fetching many ranges does not reopen for each.
void (*logHook)(const char*) = nullptr;
static void logf_hook(const char* s) { if (logHook) logHook(s); }

struct Cached { FileRef ref; double lastUse; };
static std::mutex mu;
static std::map<std::string, Cached> writers;
static std::multimap<std::string, Cached> readers;
static const double LINGER = 1.5;
static const size_t MAX_OPEN = 24;  // see trimLocked
static void trimLocked(size_t keep, double t, bool all);

FileRef openWrite(const std::string& path, Trunc trunc, int64_t presize, std::string& err) {
  std::lock_guard<std::mutex> lk(mu);
  readers.erase(path);  // their size would be stale
  auto it = writers.find(path);
  if (it != writers.end()) {
    // Someone else has it: never truncate under them.
    bool idle = it->second.ref.use_count() == 1 && nowSec() - it->second.lastUse > 2.0;
    bool truncate = trunc == TRUNC_FORCE || (trunc == TRUNC_IF_IDLE && idle);
    if (trunc == TRUNC_IF_IDLE && !idle) logf_hook(("not truncating " + path + ": other connections are writing it").c_str());
    if (truncate || (presize >= 0 && it->second.ref.use_count() == 1)) {
      File* f = it->second.ref.get();
#ifdef __SWITCH__
      if (truncate) fsFileSetSize(&f->fh, 0);
      if (presize >= 0) fsFileSetSize(&f->fh, presize);
#else
      if (truncate && ftruncate(f->fd, 0)) {}
      if (presize >= 0 && ftruncate(f->fd, presize)) {}
#endif
    }
    it->second.lastUse = nowSec();
    return it->second.ref;
  }
  trimLocked(MAX_OPEN - 1, nowSec(), false);
  auto f = std::make_shared<File>();
  f->path = path;
  if (!pOpen(*f, true, trunc != TRUNC_NO, presize, err)) return nullptr;
  writers[path] = {f, nowSec()};
  return f;
}

FileRef openRead(const std::string& path, std::string& err) {
  std::lock_guard<std::mutex> lk(mu);
  auto w = writers.find(path);
  if (w != writers.end()) { w->second.lastUse = nowSec(); return w->second.ref; }  // read what is being written
  auto range = readers.equal_range(path);
  for (auto it = range.first; it != range.second; ++it)
    if (it->second.ref.use_count() == 1) { it->second.lastUse = nowSec(); return it->second.ref; }
  trimLocked(MAX_OPEN - 1, nowSec(), false);
  auto f = std::make_shared<File>();
  f->path = path;
  if (!pOpen(*f, false, false, -1, err)) return nullptr;
  readers.insert({path, {f, nowSec()}});
  return f;
}

// Never more than MAX_OPEN handles in all: many small files finishing at once
// otherwise leave hundreds open, and with that many the Switch stalls (its
// networking included). Handles in use are never closed; idle ones go
// oldest first. Call with mu held.
static void trimLocked(size_t keep, double t, bool all) {
  std::vector<std::pair<double, std::function<void()>>> idle;
  for (auto it = writers.begin(); it != writers.end();) {
    if (it->second.ref.use_count() > 1) { it->second.lastUse = t; ++it; }
    else if (all || t - it->second.lastUse > LINGER) it = writers.erase(it);
    else { auto key = it->first; idle.push_back({it->second.lastUse, [key] { writers.erase(key); }}); ++it; }
  }
  for (auto it = readers.begin(); it != readers.end();) {
    if (it->second.ref.use_count() > 1) { it->second.lastUse = t; ++it; }
    else if (all || t - it->second.lastUse > LINGER) it = readers.erase(it);
    else {
      auto key = it->first; File* f = it->second.ref.get();
      idle.push_back({it->second.lastUse, [key, f] {
        auto r = readers.equal_range(key);
        for (auto i = r.first; i != r.second; ++i) if (i->second.ref.get() == f) { readers.erase(i); break; }
      }});
      ++it;
    }
  }
  size_t total = writers.size() + readers.size();
  if (total <= keep) return;
  std::sort(idle.begin(), idle.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (size_t i = 0; i < idle.size() && total > keep; i++, total--) idle[i].second();
}

void reap(bool all) {
  std::lock_guard<std::mutex> lk(mu);
  trimLocked(MAX_OPEN, nowSec(), all);
}

bool inUse(const std::string& path) {
  std::lock_guard<std::mutex> lk(mu);
  auto w = writers.find(path);
  if (w != writers.end() && w->second.ref.use_count() > 1) return true;
  auto range = readers.equal_range(path);
  for (auto it = range.first; it != range.second; ++it) if (it->second.ref.use_count() > 1) return true;
  return false;
}

void release(const std::string& path) {
  std::lock_guard<std::mutex> lk(mu);
  writers.erase(path);
  readers.erase(path);
}

}  // namespace vfs
