// Background copy jobs for the web page.
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "net.h"

struct OpJob {
  int id = 0;
  std::string from, to;
  std::atomic<int64_t> total{0}, done{0};
  enum State { QUEUED, RUNNING, DONE, FAILED };
  std::atomic<int> state{QUEUED};
  std::atomic<bool> cancel{false};
  std::string error;  // set before state becomes FAILED
  TransferRef stat;
};

// Queues a copy of a file or folder (with everything in it) to `to`, which
// must not exist yet. Returns null with err if it cannot start.
std::shared_ptr<OpJob> opsCopy(const std::string& from, const std::string& to, std::string& err);
std::vector<std::shared_ptr<OpJob>> opsJobs();
void opsStop();
// Runs fn on the background worker (no detached threads: on the Switch
// std::thread::detach aborts the app).
void opsBackground(std::function<void()> fn);
void opsCancelAll();  // stops the running copy and drops queued ones (partial copies are deleted)
