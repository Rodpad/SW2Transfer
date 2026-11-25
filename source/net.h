// SW2Transfer networking core.
//
// The Switch's socket service only allows a few concurrent IPC sessions
// (libnx caps it at 16, and a blocked recv holds one), so connections are not
// a thread each. A few network threads poll non-blocking sockets; reads and
// writes of the SD card happen on a disk thread pool in 1 MB chunks. One file
// can be served to or filled from many connections at once (positional I/O
// on one shared handle), which is what makes multi-connection transfers fast.
#pragma once
#include <stdint.h>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <poll.h>
#include "vfs.h"
#ifdef __SWITCH__
#include <switch.h>
#else
#include <thread>
#endif

// A joinable thread. On the Switch it is a libnx thread, as kaiHEN's own
// plugins use: std::thread does not start in a kaiHEN plugin process (the
// process aborts). Never detach (see ops.h).
class SysThread {
 public:
  SysThread() = default;
  SysThread(const SysThread&) = delete;
  SysThread& operator=(const SysThread&) = delete;
  ~SysThread() { join(); }
  bool start(std::function<void()> fn, size_t stackSize = 0x40000);  // false (and logged) if it could not start
  void join();
  bool joinable() const { return started_; }
 private:
  std::function<void()> fn_;
  bool started_ = false;
#ifdef __SWITCH__
  Thread t_{};
  static void entry(void* self) { static_cast<SysThread*>(self)->fn_(); }
#else
  std::thread t_;
#endif
};

double nowSec();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::vector<std::string> recentLog(int n);

void setLogFile(const char* path);
// Keeps the last run's log as prev (one generation) and starts path empty, so a
// crash or hang is still readable after the console restarts.
void rotateLog(const char* path, const char* prev);

// ------------------------------------------------------------------ stats
// One entry per file and direction, shared by all connections moving it.
struct Transfer {
  uint64_t id = 0;       // unique in this process (the app's UI mode follows rows by it)
  std::string path;
  bool upload;
  bool copy = false;     // a copy on the card (shown as such)
  std::atomic<bool> cancelled{false};
  std::atomic<int64_t> bytes{0};
  std::atomic<int64_t> total{-1};   // whole file size when known
  std::atomic<int> conns{0};
  std::atomic<int> peakConns{0};
  std::string via;       // how it came: "web", "terminal" (command line), "ftp" ("" for copies)  // most connections at once (shown once finished)
  double started = 0, lastActive = 0;
  // UI-side smoothing
  int64_t uiLastBytes = 0; double uiSpeed = 0;
};
using TransferRef = std::shared_ptr<Transfer>;
TransferRef transferAttach(const std::string& path, bool upload, int64_t total, const char* via = "");
void transferDetach(const TransferRef& t);
std::vector<TransferRef> transfersActive(double keepSec);  // active or recent
extern std::atomic<int64_t> g_bytesUp, g_bytesDown;

// Cancel all: every transfer running now is stopped, on every device.
// Clients tag each file's requests with an id (?x=...); ids seen before a
// cancel are refused afterwards (HTTP 410), so clients stop instead of
// retrying. Half-written uploads are deleted.
extern std::atomic<int> g_cancelEpoch;
void cancelAll(const char* by);
bool xferIdAllowed(const std::string& id);  // false = cancelled
extern std::atomic<int> g_connections;
// Open client connections are capped: every socket takes part of the Switch's
// socket memory pool, and past roughly the pool's size every socket on the
// console stalls. Over the cap, new connections wait in a short listen queue.
extern const int MAX_CONNECTIONS;
// Set when the listening socket fails (e.g. after sleep); the owner restarts the server.
extern std::atomic<bool> g_listenerBroken;
// Background plugin: POST /api/stop from the console sets this; the plugin then
// stops serving until the console restarts.
extern std::atomic<bool> g_stopRequested;
extern std::string g_sockMode;
extern const char* g_buildLabel;  // version and build, e.g. "1.4.3 (build 9dcfe17)"  // shown on the app's screen and in /api/status
extern double g_startedAt;      // nowSec() when the process started serving

// ------------------------------------------------------------------ disk pool
struct Job {
  std::function<void()> work;
  std::atomic<bool> done{false};
};
using JobRef = std::shared_ptr<Job>;
void diskStart(int threads);
void diskStop();
JobRef diskSubmit(std::function<void()> work);

// ------------------------------------------------------------------ pump
// Moves bytes between a non-blocking socket and a file (or memory),
// double-buffered so the disk and the network work at the same time.
// Sizes: the app's defaults; the background plugin (SW2_PLUGIN) builds with
// smaller ones (see Makefile.plugin), since it runs alongside games.
#ifndef SW2_CHUNK
#define SW2_CHUNK (1 << 20)   // disk I/O size and per-connection buffer
#endif
#ifndef SW2_NBUF
#define SW2_NBUF 3            // buffers in flight per connection
#endif
#ifndef SW2_POOL_MAX
#define SW2_POOL_MAX 64       // spare buffers kept for reuse
#endif
#ifndef SW2_MAX_CONNECTIONS
#define SW2_MAX_CONNECTIONS 16
#endif
#ifndef SW2_MAX_TRANSFERS
#define SW2_MAX_TRANSFERS 8
#endif
enum { CHUNK = SW2_CHUNK, NBUF = SW2_NBUF };
struct Buf {
  std::vector<char> data;
  size_t len = 0, pos = 0;
  int64_t fileOff = 0;
  int64_t result = 0;   // job result: bytes read, or 0/-1 for writes
  JobRef job;
  enum State { EMPTY, FILLING, FULL, DISK } state = EMPTY;
};
struct Pump {
  enum Mode { NONE, SEND_MEM, SEND_FILE, RECV_FILE } mode = NONE;
  std::string mem; size_t memPos = 0;
  vfs::FileRef file;
  int64_t nextOff = 0;          // next file offset to read/write
  int64_t remaining = -1;       // bytes still to schedule (-1: until EOF / peer close)
  bool srcEnd = false;          // no more data will come from the source
  bool failed = false; std::string error;
  std::deque<std::shared_ptr<Buf>> q;  // in file order
  TransferRef stat;
  int64_t moved = 0;

  void sendMem(std::string s);
  void sendFile(vfs::FileRef f, int64_t off, int64_t len, TransferRef st);
  void recvFile(vfs::FileRef f, int64_t off, int64_t len, TransferRef st, const char* pre = nullptr, size_t preLen = 0);
  void reset();
  short events() const;         // POLLIN / POLLOUT wanted on the socket
  // Returns false on socket error/peer close where it matters.
  void onReadable(int fd);
  void onWritable(int fd);
  void tick();                  // collect finished disk jobs, schedule more
  bool busy() const;            // disk jobs in flight
  bool active() const { return mode != NONE; }
  bool finished() const;
};

// ------------------------------------------------------------------ net loop
struct Conn {
  int fd = -1;
  bool dead = false;
  virtual ~Conn();
  // Fill up to 4 pollfds (first one is normally fd), return how many.
  virtual int pollfds(struct pollfd* out) = 0;
  virtual void handle(const struct pollfd* in, int n) = 0;
  virtual void tick() {}
  virtual bool busy() { return false; }
  virtual std::string describe() { return "connection"; }  // for the watchdog's report
};
using ConnFactory = std::function<Conn*(int fd)>;

void tuneSocket(int fd);
bool setNonBlocking(int fd);
int listenOn(int port, std::string& err);
bool sendAll(int fd, std::string& out);  // non-blocking: sends what it can, erases it; false on error

// Starts the listeners and network threads. Returns false with err if a port
// could not be opened.
struct ServerConfig { int httpPort = 6967; int ftpPort = 6700; int netThreads = 4; int diskThreads = 4; };
// FTP is optional: if its port cannot be opened, HTTP still starts (g_ftpPort stays 0).
bool serverStart(const ServerConfig& cfg, ConnFactory http, std::string& err, ConnFactory ftp = nullptr);
extern std::atomic<int> g_ftpPort;  // the FTP port being served, 0 if none
void serverStop();

// Watchdog (started and stopped with the server): every 2 s it checks that the
// network threads still loop, that no SD job runs for long, that the connection
// count matches the connections there are, and that the server isn't stuck at
// the connection cap with nothing moving. On a problem it writes a WATCHDOG
// report to the log, and frees what it safely can: it corrects the count, and
// closes all connections when the cap is reached and nothing moves for 30 s.
void watchdogStart();
void watchdogStop();

// Screen capture for GET /api/screen: the UI thread fills it after drawing.
struct ScreenGrab { std::mutex mu; bool want = false, ready = false; std::string png; };
extern ScreenGrab g_screen;
