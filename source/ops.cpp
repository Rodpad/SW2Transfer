// Background file operations (copy) for the web page: copying gigabytes on
// the SD card takes a while, so it runs on its own thread, one job after
// another, and the page polls /api/jobs for progress.
#include "ops.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
std::mutex mu;
std::condition_variable cv;
std::deque<std::shared_ptr<OpJob>> queue;
std::deque<std::function<void()>> tasks;  // background chores, run before copies
std::vector<std::shared_ptr<OpJob>> all;
std::thread worker;
bool quit = false;
int nextId = 1;

int64_t treeSize(const std::string& path) {
  int64_t s = 0;
  vfs::Kind k = vfs::stat(path, &s);
  if (k != vfs::DIR_) return k == vfs::FILE_ ? s : 0;
  std::vector<vfs::Entry> ents; std::string err;
  vfs::list(path, ents, err);
  int64_t t = 0;
  for (auto& e : ents) t += e.dir ? treeSize(path + "/" + e.name) : e.size;
  return t;
}

bool copyFile(OpJob& j, const std::string& from, const std::string& to) {
  std::string err;
  auto in = vfs::openRead(from, err);
  if (!in) { j.error = from + ": " + err; return false; }
  int64_t size = vfs::size(in.get());
  auto out = vfs::openWrite(to, vfs::TRUNC_FORCE, size, err);
  if (!out) { j.error = to + ": " + err; return false; }
  std::vector<char> buf(4 << 20);
  for (int64_t off = 0; off < size;) {
    if (j.cancel) { j.error = "cancelled"; return false; }
    size_t n = (size_t)std::min<int64_t>(buf.size(), size - off);
    int64_t got = vfs::read(in.get(), off, buf.data(), n);
    if (got <= 0) { j.error = "read failed: " + from; return false; }
    if (!vfs::write(out.get(), off, buf.data(), (size_t)got)) { j.error = "write failed (card full?): " + to; return false; }
    off += got; j.done += got;
    if (j.stat) { j.stat->bytes += got; j.stat->lastActive = nowSec(); }
  }
  return true;
}

bool copyTree(OpJob& j, const std::string& from, const std::string& to) {
  if (vfs::stat(from) == vfs::FILE_) return copyFile(j, from, to);
  std::string err;
  if (!vfs::mkdir(to, err) && vfs::stat(to) != vfs::DIR_) { j.error = to + ": " + err; return false; }
  std::vector<vfs::Entry> ents;
  if (!vfs::list(from, ents, err)) { j.error = from + ": " + err; return false; }
  for (auto& e : ents)
    if (!copyTree(j, from + "/" + e.name, to + "/" + e.name)) return false;
  return true;
}

void run() {
  for (;;) {
    std::shared_ptr<OpJob> j;
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lk(mu);
      cv.wait(lk, [] { return quit || !queue.empty() || !tasks.empty(); });
      if (quit) return;
      if (!tasks.empty()) { task = std::move(tasks.front()); tasks.pop_front(); }
      else { j = queue.front(); queue.pop_front(); }
    }
    if (task) { task(); continue; }
    j->state = OpJob::RUNNING;
    j->total = treeSize(j->from);
    j->stat = transferAttach(j->to, true, j->total);
    j->stat->copy = true;
    bool ok = copyTree(*j, j->from, j->to);
    if (!ok && j->cancel) {
      // Remove the half-made copy (the destination did not exist before).
      std::string err;
      if (vfs::stat(j->to) == vfs::DIR_) vfs::removeDir(j->to, true, err); else vfs::removeFile(j->to, err);
      j->error = "cancelled";
    }
    transferDetach(j->stat);
    logf("copy %s -> %s: %s", j->from.c_str(), j->to.c_str(), ok ? "done" : j->error.c_str());
    j->state = ok ? OpJob::DONE : OpJob::FAILED;
  }
}
}  // namespace

std::shared_ptr<OpJob> opsCopy(const std::string& from, const std::string& to, std::string& err) {
  if (from == "/") { err = "cannot copy the whole card"; return nullptr; }
  if (vfs::stat(from) == vfs::NONE) { err = "not found: " + from; return nullptr; }
  if (vfs::stat(to) != vfs::NONE) { err = "already exists: " + to; return nullptr; }
  if (to == from || to.compare(0, from.size() + 1, from + "/") == 0) { err = "cannot copy a folder into itself"; return nullptr; }
  auto j = std::make_shared<OpJob>();
  j->from = from; j->to = to;
  std::lock_guard<std::mutex> lk(mu);
  j->id = nextId++;
  queue.push_back(j);
  all.push_back(j);
  if (all.size() > 200) all.erase(all.begin());
  if (!worker.joinable()) worker = std::thread(run);
  cv.notify_one();
  return j;
}

std::vector<std::shared_ptr<OpJob>> opsJobs() {
  std::lock_guard<std::mutex> lk(mu);
  return all;
}

void opsBackground(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(mu);
  tasks.push_back(std::move(fn));
  if (!worker.joinable()) worker = std::thread(run);
  cv.notify_one();
}

void opsCancelAll() {
  std::lock_guard<std::mutex> lk(mu);
  for (auto& j : queue) { j->cancel = true; j->error = "cancelled"; j->state = OpJob::FAILED; }
  queue.clear();
  for (auto& j : all) if (j->state == OpJob::RUNNING) j->cancel = true;
}

void opsStop() {
  {
    std::lock_guard<std::mutex> lk(mu);
    quit = true;
    for (auto& j : all) j->cancel = true;
  }
  cv.notify_all();
  if (worker.joinable()) worker.join();
}
