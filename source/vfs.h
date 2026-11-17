// SW2Transfer: file access by virtual path ("/" = SD card root on the Switch,
// the --root folder on the host). Positional reads/writes, safe to call from
// several threads on one handle, so several connections can fill or read one
// file at different offsets at the same time.
#pragma once
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

namespace vfs {

struct File;  // opaque, shared between connections
using FileRef = std::shared_ptr<File>;

void init(const std::string& hostRoot);  // hostRoot ignored on the Switch

// Resolves arg (absolute or relative to cwd) to a clean virtual path that
// cannot leave the root ("/a/../.." -> "/").
std::string resolve(const std::string& cwd, const std::string& arg);
std::string parentOf(const std::string& path);
std::string baseName(const std::string& path);

// Readers each get their own handle (cached briefly for chunked requests).
FileRef openRead(const std::string& path, std::string& err);
// Writers of the same path share one handle. presize >= 0 sets the length.
// TRUNC_FORCE always empties the file first; TRUNC_IF_IDLE only when no
// other connection has written to it in the last seconds (a plain upload
// of segment 0 arriving after the other segments must not wipe them).
enum Trunc { TRUNC_NO, TRUNC_IF_IDLE, TRUNC_FORCE };
FileRef openWrite(const std::string& path, Trunc truncate, int64_t presize, std::string& err);

int64_t read(File* f, int64_t off, void* buf, size_t len);         // bytes read, -1 on error
bool write(File* f, int64_t off, const void* buf, size_t len);
int64_t size(File* f);
const std::string& pathOf(File* f);

// Drops cached handles that nobody uses (call every second or so); force
// closes everything cached for one path (before rename/delete).
void reap(bool all = false);
void release(const std::string& path);
bool inUse(const std::string& path);  // a connection or disk job still holds a handle to it

struct Entry { std::string name; bool dir; int64_t size; int64_t created = 0, modified = 0; };  // times: POSIX seconds, 0 = unknown
enum Kind { NONE = 0, FILE_ = 1, DIR_ = 2 };
Kind stat(const std::string& path, int64_t* size = nullptr);
bool list(const std::string& path, std::vector<Entry>& out, std::string& err);
bool mkdir(const std::string& path, std::string& err);
bool mkdirs(const std::string& path);  // with parents; true if it exists afterwards
bool removeFile(const std::string& path, std::string& err);
bool removeDir(const std::string& path, bool recursive, std::string& err);
bool rename(const std::string& from, const std::string& to, std::string& err);
int64_t freeSpace();  // -1 if unknown
extern void (*logHook)(const char*);

}  // namespace vfs
