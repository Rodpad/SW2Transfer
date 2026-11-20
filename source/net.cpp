#include "net.h"
#include "ops.h"
#include <set>
#include <chrono>
#include <condition_variable>
#include <map>
#include <thread>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#ifdef __SWITCH__
#include <switch.h>
#endif

double nowSec() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Horizon never moves a thread to another core, and new threads start on the
// creator's core, so spread the workers out explicitly.
// Start-up steps in the background plugin's log (it has no screen to show a failure).
#ifdef SW2_PLUGIN
#define PSTEP(...) logf(__VA_ARGS__)
#else
#define PSTEP(...) do {} while (0)
#endif

static void pinThread(int core) {
#if defined(__SWITCH__) && !defined(SW2_PLUGIN)  // the plugin keeps the core its process is given
  u64 mask = 0;
  svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  std::vector<int> cores;
  for (int c = 1; c < 64; c++) if (mask & (1ull << c)) cores.push_back(c);  // leave core 0 to the UI
  if (cores.empty()) return;
  int c = cores[core % cores.size()];
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c);
  PSTEP("thread %d: core mask 0x%llx, pinned to core %d (rc 0x%x)", core, (unsigned long long)mask, c, rc);
  (void)rc;
#else
  (void)core;
#endif
}

// ------------------------------------------------------------------ log
static std::mutex logMu;
static std::deque<std::string> logLines;
static FILE* logFile = nullptr;
void setLogFile(const char* path) { std::lock_guard<std::mutex> lk(logMu); logFile = fopen(path, "a"); }
void logf(const char* fmt, ...) {
  char b[512];
  va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  std::lock_guard<std::mutex> lk(logMu);
  logLines.push_back(b);
  while (logLines.size() > 200) logLines.pop_front();
  if (logFile) { fprintf(logFile, "%s\n", b); fflush(logFile); }
#ifndef __SWITCH__
  fprintf(stderr, "%s\n", b);
#endif
}
std::vector<std::string> recentLog(int n) {
  std::lock_guard<std::mutex> lk(logMu);
  std::vector<std::string> out;
  for (int i = (int)logLines.size() - n; i < (int)logLines.size(); i++) if (i >= 0) out.push_back(logLines[i]);
  return out;
}

// ------------------------------------------------------------------ stats
std::atomic<int64_t> g_bytesUp{0}, g_bytesDown{0};
std::atomic<int> g_connections{0};
const int MAX_CONNECTIONS = SW2_MAX_CONNECTIONS;
std::atomic<bool> g_listenerBroken{false};
std::atomic<bool> g_stopRequested{false};
std::atomic<int> g_ftpPort{0};
std::string g_sockMode;
double g_startedAt = 0;
ScreenGrab g_screen;
static std::mutex statMu;
static std::vector<TransferRef> transfers;

TransferRef transferAttach(const std::string& path, bool upload, int64_t total) {
  std::lock_guard<std::mutex> lk(statMu);
  double t = nowSec();
  for (auto& x : transfers) {
    // A file being moved again after a pause (a new run) starts fresh.
    if (x->path == path && x->upload == upload && (x->conns > 0 || t - x->lastActive < 2.0)) {
      x->conns++; x->lastActive = t;
      if (x->conns > x->peakConns) x->peakConns = x->conns.load();
      if (total >= 0) x->total = total;
      return x;
    }
  }
  static uint64_t nextId = 0;
  auto x = std::make_shared<Transfer>();
  x->id = ++nextId;
  x->path = path; x->upload = upload; x->total = total; x->conns = 1; x->peakConns = 1; x->started = x->lastActive = t;
  transfers.push_back(x);
  return x;
}
void transferDetach(const TransferRef& x) {
  if (!x) return;
  std::lock_guard<std::mutex> lk(statMu);
  x->conns--; x->lastActive = nowSec();
}
std::vector<TransferRef> transfersActive(double keepSec) {
  std::lock_guard<std::mutex> lk(statMu);
  double t = nowSec();
  std::vector<TransferRef> out, keep;
  for (auto& x : transfers) {
    // Cancelled transfers stay listed longer so the cancel is seen.
    bool live = x->conns > 0 || t - x->lastActive < (x->cancelled ? std::max(keepSec, 30.0) : keepSec);
    if (live) { out.push_back(x); keep.push_back(x); }
    else if (t - x->lastActive < 600) keep.push_back(x);  // kept for resuming counts
  }
  transfers.swap(keep);
  return out;
}

// ------------------------------------------------------------------ cancel
std::atomic<int> g_cancelEpoch{0};
static std::mutex idMu;
static std::set<std::string> knownIds, cancelledIds;

bool xferIdAllowed(const std::string& id) {
  if (id.empty()) return true;
  std::lock_guard<std::mutex> lk(idMu);
  if (cancelledIds.count(id)) return false;
  knownIds.insert(id);
  return true;
}

void cancelAll(const char* by) {
  {
    std::lock_guard<std::mutex> lk(idMu);
    cancelledIds.insert(knownIds.begin(), knownIds.end());
    knownIds.clear();
    if (cancelledIds.size() > 10000) cancelledIds.clear();
  }
  logf("cancel: start (%s)", by);
  g_cancelEpoch++;
  opsCancelAll();
  logf("cancel: copies stopped");
  // Uploads that had not finished: delete what was written once their
  // connections have closed.
  std::vector<TransferRef> partial;
  {
    std::lock_guard<std::mutex> lk(statMu);
    for (auto& x : transfers) {
      if (x->conns <= 0 && nowSec() - x->lastActive > 2.0) continue;
      x->cancelled = true;
      if (x->upload && !x->copy && x->total >= 0 && x->bytes < x->total) partial.push_back(x);
    }
  }
  logf("cancelled all transfers (%s)", by);
  if (partial.empty()) return;
  // Deleting a file that still has an open handle (or a write in flight)
  // wedged the Switch's file system once, so wait until nothing holds it,
  // close it, and only then delete; if it stays busy, leave it.
  logf("cancel: %d unfinished upload(s) to clean up", (int)partial.size());
  opsBackground([partial] {
    for (auto& x : partial) {
      logf("cleanup: waiting for %s", x->path.c_str());
      bool idle = false;
      for (int i = 0; i < 300 && !idle; i++) {  // up to 30 s
        idle = x->conns <= 0 && !vfs::inUse(x->path);
        if (!idle) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!idle) { logf("left unfinished upload %s (still in use)", x->path.c_str()); continue; }
      logf("cleanup: closing %s", x->path.c_str());
      vfs::release(x->path);  // flush + close the cached handle
      logf("cleanup: closed");
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      if (vfs::inUse(x->path)) { logf("left unfinished upload %s (reopened)", x->path.c_str()); continue; }
      std::string err;
      if (vfs::removeFile(x->path, err)) logf("removed unfinished upload %s", x->path.c_str());
      else logf("could not remove unfinished upload %s: %s", x->path.c_str(), err.c_str());
    }
  });
}

// ------------------------------------------------------------------ SysThread
bool SysThread::start(std::function<void()> fn, size_t stackSize) {
  if (started_) return false;
  fn_ = std::move(fn);
#ifdef __SWITCH__
  s32 prio = 0x2C;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);  // as the thread starting it
  Result rc = threadCreate(&t_, entry, this, nullptr, stackSize, prio, -2);
  if (R_SUCCEEDED(rc)) {
    rc = threadStart(&t_);
    if (R_FAILED(rc)) threadClose(&t_);
  }
  if (R_FAILED(rc)) { logf("cannot start a thread: 0x%x", rc); return false; }
#else
  (void)stackSize;
  t_ = std::thread([this] { fn_(); });
#endif
  started_ = true;
  return true;
}
void SysThread::join() {
  if (!started_) return;
#ifdef __SWITCH__
  threadWaitForExit(&t_);
  threadClose(&t_);
#else
  t_.join();
#endif
  started_ = false;
}

// ------------------------------------------------------------------ disk pool
static std::mutex diskMu;
static std::condition_variable diskCv;
static std::deque<JobRef> diskQueue;
static std::vector<std::unique_ptr<SysThread>> diskThreads;
static bool diskQuit = false;

void diskStart(int n) {
  diskQuit = false;
  for (int i = 0; i < n; i++) {
    diskThreads.emplace_back(new SysThread());
    diskThreads.back()->start([i] {
      PSTEP("disk thread %d running", i);
      pinThread(i + 1);
      for (;;) {
        JobRef j;
        {
          std::unique_lock<std::mutex> lk(diskMu);
          diskCv.wait(lk, [] { return diskQuit || !diskQueue.empty(); });
          if (diskQueue.empty()) return;
          j = diskQueue.front(); diskQueue.pop_front();
        }
        j->work();
        j->done.store(true, std::memory_order_release);
      }
    });
  }
}
void diskStop() {
  { std::lock_guard<std::mutex> lk(diskMu); diskQuit = true; }
  diskCv.notify_all();
  for (auto& t : diskThreads) t->join();
  diskThreads.clear();
}
JobRef diskSubmit(std::function<void()> work) {
  auto j = std::make_shared<Job>();
  j->work = std::move(work);
  { std::lock_guard<std::mutex> lk(diskMu); diskQueue.push_back(j); }
  diskCv.notify_one();
  return j;
}

// ------------------------------------------------------------------ pump
static std::mutex poolMu;
static std::vector<std::shared_ptr<Buf>> bufPool;  // reused 1 MB buffers
static std::shared_ptr<Buf> takeBuf() {
  {
    std::lock_guard<std::mutex> lk(poolMu);
    if (!bufPool.empty()) { auto b = bufPool.back(); bufPool.pop_back(); return b; }
  }
  auto b = std::make_shared<Buf>();
  b->data.resize(CHUNK);
  return b;
}
static void giveBuf(std::shared_ptr<Buf> b) {
  if (b->job && !b->job->done) return;  // still owned by a disk job; let it go
  b->job.reset(); b->len = b->pos = 0; b->state = Buf::EMPTY;
  std::lock_guard<std::mutex> lk(poolMu);
  if (bufPool.size() < SW2_POOL_MAX) bufPool.push_back(b);
}

void Pump::reset() {
  for (auto& b : q) giveBuf(b);
  q.clear();
  if (stat) { transferDetach(stat); stat.reset(); }
  file.reset();
  mem.clear(); memPos = 0;
  mode = NONE; nextOff = 0; remaining = -1; srcEnd = false; failed = false; error.clear(); moved = 0;
}
void Pump::sendMem(std::string s) { reset(); mode = SEND_MEM; mem = std::move(s); srcEnd = true; }
void Pump::sendFile(vfs::FileRef f, int64_t off, int64_t len, TransferRef st) {
  reset(); mode = SEND_FILE; file = f; stat = st; nextOff = off;
  if (len < 0) { int64_t s = vfs::size(f.get()); len = s > off ? s - off : 0; }
  remaining = len; srcEnd = len == 0;
  tick();
}
void Pump::recvFile(vfs::FileRef f, int64_t off, int64_t len, TransferRef st, const char* pre, size_t preLen) {
  reset(); mode = RECV_FILE; file = f; stat = st; nextOff = off; remaining = len; srcEnd = len == 0;
  if (pre && preLen && !srcEnd) {
    auto b = takeBuf(); b->state = Buf::FILLING; b->fileOff = nextOff; b->len = 0;
    q.push_back(b);
    size_t n = std::min<size_t>(preLen, CHUNK);
    if (remaining >= 0 && (int64_t)n > remaining) n = (size_t)remaining;
    memcpy(b->data.data(), pre, n);
    b->len = n; nextOff += n; moved += n;
    if (remaining >= 0) remaining -= n;
    if (stat) stat->bytes += n;
    g_bytesUp += n;
    if (remaining == 0) srcEnd = true;
  }
  tick();
}

static void submitWrite(const std::shared_ptr<Buf>& b, const vfs::FileRef& f) {
  b->state = Buf::DISK;
  std::shared_ptr<Buf> bb = b; vfs::FileRef ff = f;
  b->job = diskSubmit([bb, ff] { bb->result = vfs::write(ff.get(), bb->fileOff, bb->data.data(), bb->len) ? 0 : -1; });
}

void Pump::tick() {
  if (mode == SEND_FILE) {
    for (auto& b : q) {
      if (b->state == Buf::DISK && b->job->done.load(std::memory_order_acquire)) {
        if (b->result < 0) { failed = true; error = "read failed"; }
        else {
          if ((int64_t)b->result < (int64_t)b->len) srcEnd = true;  // file shorter than expected
          b->len = (size_t)b->result; b->pos = 0; b->state = Buf::FULL;
        }
      }
    }
    while (!q.empty() && q.front()->state == Buf::FULL && q.front()->pos >= q.front()->len) { giveBuf(q.front()); q.pop_front(); }
    while (!failed && !srcEnd && remaining > 0 && q.size() < NBUF) {
      auto b = takeBuf();
      size_t n = (size_t)std::min<int64_t>(CHUNK, remaining);
      b->fileOff = nextOff; b->len = n; b->state = Buf::DISK;
      std::shared_ptr<Buf> bb = b; vfs::FileRef ff = file;
      b->job = diskSubmit([bb, ff] { bb->result = vfs::read(ff.get(), bb->fileOff, bb->data.data(), bb->len); });
      q.push_back(b);
      nextOff += n; remaining -= n;
      if (remaining == 0) srcEnd = true;
    }
  } else if (mode == RECV_FILE) {
    // A filling buffer becomes a write when full or when the source ended.
    if (!q.empty() && q.back()->state == Buf::FILLING && (q.back()->len == CHUNK || srcEnd)) {
      if (q.back()->len) submitWrite(q.back(), file);
      else { giveBuf(q.back()); q.pop_back(); }
    }
    while (!q.empty() && q.front()->state == Buf::DISK && q.front()->job->done.load(std::memory_order_acquire)) {
      if (q.front()->result < 0) { failed = true; error = "write failed (card full?)"; }
      giveBuf(q.front()); q.pop_front();
    }
  }
}

short Pump::events() const {
  if (failed) return 0;
  switch (mode) {
    case SEND_MEM: return memPos < mem.size() ? POLLOUT : 0;
    case SEND_FILE: return (!q.empty() && q.front()->state == Buf::FULL) ? POLLOUT : 0;
    case RECV_FILE:
      if (srcEnd) return 0;
      if (!q.empty() && q.back()->state == Buf::FILLING) return POLLIN;
      return q.size() < NBUF ? POLLIN : 0;
    default: return 0;
  }
}

void Pump::onReadable(int fd) {
  if (mode != RECV_FILE || srcEnd || failed) return;
  for (int round = 0; round < 8; round++) {
    std::shared_ptr<Buf> b;
    if (!q.empty() && q.back()->state == Buf::FILLING) b = q.back();
    else if (q.size() < NBUF) { b = takeBuf(); b->state = Buf::FILLING; b->fileOff = nextOff; b->len = 0; q.push_back(b); }
    else return;
    size_t cap = CHUNK - b->len;
    if (remaining >= 0 && (int64_t)cap > remaining) cap = (size_t)remaining;
    ssize_t n = recv(fd, b->data.data() + b->len, cap, 0);
    if (n > 0) {
      b->len += n; nextOff += n; moved += n;
      if (remaining >= 0) remaining -= n;
      if (stat) { stat->bytes += n; stat->lastActive = nowSec(); }
      g_bytesUp += n;
      if (remaining == 0) srcEnd = true;
      if (b->len == CHUNK || srcEnd) { if (b->len) submitWrite(b, file); }
      if (srcEnd) return;
      if ((size_t)n < cap) return;  // drained for now
    } else if (n == 0) {
      srcEnd = true;
      if (remaining > 0) { failed = true; error = "connection closed early"; }
      if (b->len) submitWrite(b, file); else { giveBuf(b); q.pop_back(); }
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
      failed = true; error = std::string("receive: ") + strerror(errno);
      if (b->len) submitWrite(b, file); else { giveBuf(b); q.pop_back(); }
      return;
    }
  }
}

void Pump::onWritable(int fd) {
  if (failed) return;
  if (mode == SEND_MEM) {
    while (memPos < mem.size()) {
      ssize_t n = send(fd, mem.data() + memPos, mem.size() - memPos, 0);
      if (n > 0) { memPos += n; g_bytesDown += n; continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
      failed = true; error = "connection lost"; return;
    }
    return;
  }
  if (mode != SEND_FILE) return;
  while (!q.empty() && q.front()->state == Buf::FULL) {
    auto& b = q.front();
    if (b->pos < b->len) {
      ssize_t n = send(fd, b->data.data() + b->pos, b->len - b->pos, 0);
      if (n > 0) {
        b->pos += n; moved += n; g_bytesDown += n;
        if (stat) { stat->bytes += n; stat->lastActive = nowSec(); }
      } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
      } else { failed = true; error = "connection lost"; return; }
    }
    if (b->pos >= b->len) { giveBuf(b); q.pop_front(); tick(); }
  }
}

bool Pump::busy() const {
  for (auto& b : q) if (b->state == Buf::DISK) return true;
  return false;
}
bool Pump::finished() const {
  if (mode == NONE) return true;
  if (mode == SEND_MEM) return memPos >= mem.size();
  return srcEnd && q.empty();
}

// ------------------------------------------------------------------ sockets
Conn::~Conn() { if (fd >= 0) close(fd); }

bool setNonBlocking(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
void tuneSocket(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}
int listenOn(int port, std::string& err) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { err = strerror(errno); return -1; }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, (sockaddr*)&a, sizeof a) || listen(fd, 4)) { err = strerror(errno); close(fd); return -1; }
  setNonBlocking(fd);
  return fd;
}
bool sendAll(int fd, std::string& out) {
  while (!out.empty()) {
    ssize_t n = send(fd, out.data(), out.size(), 0);
    if (n > 0) { out.erase(0, n); continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ threads
struct NetThread {
  std::vector<std::unique_ptr<Conn>> conns;
  std::mutex mu;
  std::vector<Conn*> incoming;
  SysThread th;
  void add(Conn* c) { std::lock_guard<std::mutex> lk(mu); incoming.push_back(c); }
};
static std::vector<NetThread*> threads;
static std::atomic<bool> netQuit{false};
static std::atomic<unsigned> rr{0};

struct Listener : Conn {
  ConnFactory make;
  int pollfds(pollfd* o) override { o[0] = {fd, (short)(g_connections < MAX_CONNECTIONS ? POLLIN : 0), 0}; return 1; }
  void handle(const pollfd* in, int) override {
    if (in[0].revents & (POLLERR | POLLHUP | POLLNVAL)) { g_listenerBroken = true; return; }
    if (!(in[0].revents & POLLIN)) return;
    while (g_connections < MAX_CONNECTIONS) {
      int c = accept(fd, nullptr, nullptr);
      if (c < 0) return;
      setNonBlocking(c); tuneSocket(c);
      Conn* conn = make(c);
      if (!conn) { close(c); continue; }
      g_connections++;
      threads[rr++ % threads.size()]->add(conn);
    }
  }
};

static void netLoop(NetThread* t, int index) {
  PSTEP("network thread %d running", index);
  pinThread(index);
  std::vector<pollfd> pfds;
  std::vector<int> counts;
  while (!netQuit) {
    {
      std::lock_guard<std::mutex> lk(t->mu);
      for (auto c : t->incoming) t->conns.emplace_back(c);
      t->incoming.clear();
    }
    pfds.clear(); counts.clear();
    bool anyBusy = false;
    for (auto& c : t->conns) {
      pollfd tmp[4];
      int n = c->pollfds(tmp);
      counts.push_back(n);
      for (int i = 0; i < n; i++) { tmp[i].revents = 0; pfds.push_back(tmp[i]); }
      if (c->busy()) anyBusy = true;
    }
    int timeout = anyBusy ? 1 : 20;
    if (pfds.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
    else poll(pfds.data(), pfds.size(), timeout);
    size_t k = 0;
    for (size_t i = 0; i < t->conns.size(); i++) {
      Conn* c = t->conns[i].get();
      if (counts[i]) c->handle(&pfds[k], counts[i]);
      k += counts[i];
      if (!c->dead) c->tick();
    }
    for (size_t i = 0; i < t->conns.size();) {
      if (t->conns[i]->dead) {
        if (!dynamic_cast<Listener*>(t->conns[i].get())) g_connections--;
        t->conns.erase(t->conns.begin() + i);
      } else i++;
    }
  }
  t->conns.clear();
  std::lock_guard<std::mutex> lk(t->mu);
  for (auto c : t->incoming) delete c;
  t->incoming.clear();
}

bool serverStart(const ServerConfig& cfg, ConnFactory http, std::string& err, ConnFactory ftp) {
  PSTEP("start: opening port %d", cfg.httpPort);
  int fHttp = listenOn(cfg.httpPort, err);
  if (fHttp < 0) { err = "HTTP port " + std::to_string(cfg.httpPort) + ": " + err; return false; }
  PSTEP("start: listening");
  vfs::logHook = [](const char* s) { logf("%s", s); };
  if (g_startedAt == 0) g_startedAt = nowSec();
  PSTEP("start: starting %d disk threads", cfg.diskThreads);
  diskStart(cfg.diskThreads);
  PSTEP("start: disk threads started");
  netQuit = false;
  for (int i = 0; i < cfg.netThreads; i++) threads.push_back(new NetThread());
  auto l2 = new Listener(); l2->fd = fHttp; l2->make = http;
  threads[0]->add(l2);
  g_ftpPort = 0;
  if (ftp) {
    std::string ferr;
    int fFtp = listenOn(cfg.ftpPort, ferr);
    if (fFtp < 0) logf("FTP port %d: %s (FTP off)", cfg.ftpPort, ferr.c_str());
    else {
      auto lf = new Listener(); lf->fd = fFtp; lf->make = ftp;
      threads[threads.size() > 1 ? 1 : 0]->add(lf);
      g_ftpPort = cfg.ftpPort;
    }
  }
  for (int i = 0; i < (int)threads.size(); i++) {
    PSTEP("start: starting network thread %d", i);
    NetThread* nt = threads[i];
    nt->th.start([nt, i] { netLoop(nt, i); });
  }
  PSTEP("start: network threads started");
  return true;
}
void serverStop() {
  netQuit = true;
  g_ftpPort = 0;
  for (auto t : threads) { t->th.join(); delete t; }
  threads.clear();
  diskStop();
  vfs::reap(true);
}
