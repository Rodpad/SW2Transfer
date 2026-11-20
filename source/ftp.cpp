// FTP server on port 6700 (app and background plugin), next to kaiHEN's ftpd
// on 5000. Passive mode only (PASV/EPSV), like ftpd.
//
// Uses the same file layer as the HTTP server, so it shares its speed tricks:
//   - REST n + RETR sends from n; a client may close early once it has its
//     part, so one file can be downloaded in parts over several connections.
//   - REST n + STOR writes from n without truncating, so several connections
//     can each upload one part of the same file (lftp's pget/mirror -P).
// Without a password any user name and password are accepted (like ftpd).
// With one, PASS must be the SW2Transfer password (any user name). FTP sends
// it unencrypted.
#include "net.h"
#include "ops.h"
#include "auth.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace {

std::string upper(std::string s) { for (auto& c : s) c = toupper((unsigned char)c); return s; }

// "Nov 20 14:59" this year, "Nov 20  2025" otherwise (ls style, UTC).
std::string lsDate(int64_t t) {
  if (t <= 0) return "Jan  1  2026";
  time_t tt = (time_t)t, now = time(nullptr);
  struct tm a, b;
  gmtime_r(&tt, &a); gmtime_r(&now, &b);
  char s[32];
  strftime(s, sizeof s, a.tm_year == b.tm_year ? "%b %e %H:%M" : "%b %e  %Y", &a);
  return s;
}
std::string mdtm(int64_t t) {  // YYYYMMDDHHMMSS, UTC
  time_t tt = t > 0 ? (time_t)t : 1767225600;  // 2026-01-01 if unknown
  struct tm a; gmtime_r(&tt, &a);
  char s[20]; strftime(s, sizeof s, "%Y%m%d%H%M%S", &a);
  return s;
}

struct FtpConn : Conn {
  std::string in, out;
  std::string cwd = "/";
  std::string renameFrom;
  int64_t rest = 0, allo = -1;
  bool loggedIn = false, quitting = false;
  int pasvFd = -1, dataFd = -1;
  // Pending data command, started once the data connection is up.
  enum DataCmd { D_NONE, D_SEND, D_RECV } dataCmd = D_NONE;
  bool dataStarted = false;
  Pump pump;
  std::string dataPath;
  int64_t dataOffset = 0;
  double dataWaitSince = 0, lastActivity;
  int epoch = 0;
  // PASS with a password set: checked on the disk pool (PBKDF2 takes a moment).
  JobRef passJob;
  std::shared_ptr<std::string> passOk;  // "1" when right (a string, as http.cpp's jobs: GCC 16 warns falsely on shared_ptr<bool>)

  FtpConn(int f) {
    fd = f;
    lastActivity = nowSec();
    g_connections++;  // the data connection: counted against the socket limit too
    reply(220, "SW2Transfer FTP ready (REST works for RETR and STOR: files can move in parts)");
  }
  ~FtpConn() override { closeData(); g_connections--; }

  void reply(int code, const std::string& msg) { out += std::to_string(code) + " " + msg + "\r\n"; }
  void closeData() {
    if (dataFd >= 0) { close(dataFd); dataFd = -1; }
    if (pasvFd >= 0) { close(pasvFd); pasvFd = -1; }
    pump.reset();
    dataCmd = D_NONE; dataStarted = false;
  }

  int pollfds(pollfd* o) override {
    int n = 0;
    short ev = passJob ? 0 : POLLIN;  // read no more commands until PASS is answered
    if (!out.empty()) ev |= POLLOUT;
    o[n++] = {fd, ev, 0};
    if (pasvFd >= 0) o[n++] = {pasvFd, POLLIN, 0};
    if (dataFd >= 0 && dataStarted) o[n++] = {dataFd, pump.events(), 0};
    return n;
  }

  void handle(const pollfd* p, int n) override {
    for (int i = 0; i < n; i++) {
      if (p[i].revents) lastActivity = nowSec();
      if (p[i].fd == fd) {
        if (p[i].revents & POLLIN) readControl();
        if ((p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p[i].revents & POLLIN)) dead = true;
      } else if (p[i].fd == pasvFd && (p[i].revents & POLLIN)) {
        int c = accept(pasvFd, nullptr, nullptr);
        if (c >= 0) {
          setNonBlocking(c); tuneSocket(c);
          if (dataFd >= 0) close(dataFd);
          dataFd = c;
          close(pasvFd); pasvFd = -1;
          maybeStartData();
        }
      } else if (p[i].fd == dataFd && dataStarted) {
        if (p[i].revents & POLLIN) pump.onReadable(dataFd);
        if (p[i].revents & POLLOUT) pump.onWritable(dataFd);
        if ((p[i].revents & (POLLERR | POLLHUP | POLLNVAL)) && pump.mode != Pump::RECV_FILE) {
          pump.failed = true;
          if (pump.error.empty()) pump.error = "data connection closed";
        }
      }
    }
    if (!out.empty() && !sendAll(fd, out)) dead = true;
    if (quitting && out.empty()) dead = true;
  }

  bool busy() override { return pump.busy() || passJob; }

  void tick() override {
    if (passJob && passJob->done.load(std::memory_order_acquire)) {
      bool ok = *passOk == "1";
      passJob.reset(); passOk.reset();
      auth::loginResult(ok);
      logf("FTP login: %s", ok ? "ok" : "wrong password");
      if (ok) { loggedIn = true; reply(230, "Logged in"); }
      else reply(530, "Wrong password");
      processLines();  // commands that arrived meanwhile
    }
    if (dataStarted && epoch != g_cancelEpoch) {  // Cancel all
      bool upload = pump.mode == Pump::RECV_FILE;
      std::string p = dataPath;
      bool whole = dataOffset == 0;
      reply(426, "Cancelled");
      closeData();
      logf("FTP %s %s: cancelled", upload ? "upload" : "download", p.c_str());
      if (upload && whole) {  // a cancelled upload is removed, as over HTTP
        opsBackground([p] {
          for (int i = 0; i < 50 && vfs::inUse(p); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
          if (vfs::inUse(p)) return;
          vfs::release(p);
          std::string err;
          if (vfs::removeFile(p, err)) logf("removed unfinished upload %s", p.c_str());
        });
      }
    } else if (dataStarted) {
      pump.tick();
      if (pump.failed) {
        logf("FTP %s %s: %s", pump.mode == Pump::RECV_FILE ? "upload" : "download", dataPath.c_str(), pump.error.c_str());
        reply(pump.mode == Pump::SEND_FILE ? 426 : 451, "Transfer aborted: " + pump.error);
        closeData();
      } else if (pump.finished()) {
        reply(226, "Transfer complete");
        closeData();
      }
    } else if (dataCmd != D_NONE && dataFd < 0 && nowSec() - dataWaitSince > 30) {
      reply(425, "No data connection");
      closeData();
    }
    // Idle control connections close after 5 minutes (ftpd does the same),
    // or after 30 s when the connection limit is reached.
    if (!dataStarted && !passJob && nowSec() - lastActivity > (g_connections >= MAX_CONNECTIONS ? 30 : 300)) {
      reply(421, "Idle for too long");
      quitting = true;
    }
    if (!out.empty() && !sendAll(fd, out)) dead = true;
    if (quitting && out.empty()) dead = true;
  }

  void readControl() {
    char b[4096];
    ssize_t n = recv(fd, b, sizeof b, 0);
    if (n == 0) { dead = true; return; }
    if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) dead = true; return; }
    in.append(b, n);
    if (in.size() > 65536) { dead = true; return; }
    processLines();
  }
  void processLines() {
    size_t e;
    while (!passJob && !quitting && (e = in.find('\n')) != std::string::npos) {
      std::string line = in.substr(0, e);
      in.erase(0, e + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      // Telnet IAC bytes some clients put before ABOR.
      while (!line.empty() && (unsigned char)line[0] >= 0xF0) line.erase(0, 1);
      command(line);
    }
  }

  void maybeStartData() {
    if (dataFd < 0 || dataCmd == D_NONE || dataStarted) return;
    dataStarted = true;
    epoch = g_cancelEpoch;
  }

  bool needPasv() {
    if (pasvFd < 0 && dataFd < 0) { reply(425, "Use PASV or EPSV first"); return false; }
    return true;
  }

  int openPasv(int& port) {
    sockaddr_in a{}; socklen_t al = sizeof a;
    getsockname(fd, (sockaddr*)&a, &al);
    int l = socket(AF_INET, SOCK_STREAM, 0);
    if (l < 0) return -1;
    sockaddr_in b{}; b.sin_family = AF_INET; b.sin_addr = a.sin_addr; b.sin_port = 0;  // any free port
    if (bind(l, (sockaddr*)&b, sizeof b) || listen(l, 1)) { close(l); return -1; }
    al = sizeof b; getsockname(l, (sockaddr*)&b, &al);
    setNonBlocking(l);
    port = ntohs(b.sin_port);
    return l;
  }

  void listing(const std::string& arg, int style) {  // 0 LIST, 1 NLST, 2 MLSD
    std::string a = arg;
    while (!a.empty() && a[0] == '-') { size_t s = a.find(' '); a = s == std::string::npos ? "" : a.substr(s + 1); }  // "-la"
    std::string path = vfs::resolve(cwd, a);
    std::vector<vfs::Entry> ents; std::string err;
    int64_t fsz = 0;
    vfs::Kind k = vfs::stat(path, &fsz);
    if (k == vfs::FILE_) ents.push_back({vfs::baseName(path), false, fsz, 0, vfs::modifiedTime(path)});
    else if (k == vfs::NONE || !vfs::list(path, ents, err)) { reply(550, "Cannot list " + path + (err.empty() ? "" : ": " + err)); return; }
    std::string s;
    for (auto& e : ents) {
      if (style == 1) s += e.name + "\r\n";
      else if (style == 2)
        s += std::string("type=") + (e.dir ? "dir" : "file") + ";size=" + std::to_string(e.size) + ";modify=" + mdtm(e.modified) + "; " + e.name + "\r\n";
      else {
        char line[700];
        snprintf(line, sizeof line, "%s 1 switch switch %13lld %s %s\r\n", e.dir ? "drwxr-xr-x" : "-rw-r--r--", (long long)e.size,
                 lsDate(e.modified).c_str(), e.name.c_str());
        s += line;
      }
    }
    pump.sendMem(std::move(s));
    dataCmd = D_SEND; dataPath = path; dataOffset = 0; dataWaitSince = nowSec();
    reply(150, "Here comes the listing");
    maybeStartData();
  }

  void command(const std::string& line) {
    size_t sp = line.find(' ');
    std::string cmd = upper(line.substr(0, sp));
    std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
    if (cmd == "USER") {
      loggedIn = false;
      reply(331, auth::enabled() ? "Password required: the SW2Transfer password (any user name)" : "Any password will do");
      return;
    }
    if (cmd == "PASS") {
      if (!auth::enabled()) { loggedIn = true; reply(230, "Logged in"); return; }
      int left = 0;
      if (auth::loginLocked(&left)) { reply(530, "Too many wrong passwords, try again in " + std::to_string(left) + " s"); return; }
      auto ok = std::make_shared<std::string>();
      std::string pw = arg;
      passOk = ok;
      passJob = diskSubmit([pw, ok] { *ok = auth::checkPassword(pw) ? "1" : "0"; });
      return;
    }
    if (cmd == "QUIT") { reply(221, "Bye"); quitting = true; return; }
    if (cmd == "SYST") { reply(215, "UNIX Type: L8"); return; }
    if (cmd == "FEAT") {
      out += "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n UTF8\r\n MLSD type*;size*;modify*;\r\n MLST type*;size*;modify*;\r\n211 End\r\n";
      return;
    }
    if (cmd == "OPTS") { reply(200, "OK"); return; }
    if (cmd == "NOOP" || cmd == "CLNT" || cmd == "MODE" || cmd == "STRU" || cmd == "TYPE") { reply(200, "OK"); return; }
    if (!loggedIn) { reply(530, auth::enabled() ? "Log in with the SW2Transfer password" : "Log in with USER/PASS (anything)"); return; }
    if (cmd == "PWD" || cmd == "XPWD") { reply(257, "\"" + cwd + "\" is the current directory"); return; }
    if (cmd == "CWD" || cmd == "XCWD") {
      std::string p = vfs::resolve(cwd, arg);
      if (vfs::stat(p) != vfs::DIR_) { reply(550, "No such directory"); return; }
      cwd = p; reply(250, "OK"); return;
    }
    if (cmd == "CDUP" || cmd == "XCUP") { cwd = vfs::parentOf(cwd); reply(250, "OK"); return; }
    if (cmd == "PASV" || cmd == "EPSV") {
      if (dataStarted) { reply(425, "A transfer is in progress"); return; }
      if (pasvFd >= 0) { close(pasvFd); pasvFd = -1; }
      if (dataFd >= 0) { close(dataFd); dataFd = -1; }
      int port = 0;
      pasvFd = openPasv(port);
      if (pasvFd < 0) { reply(425, "Cannot open passive port"); return; }
      if (cmd == "EPSV") { reply(229, "Entering Extended Passive Mode (|||" + std::to_string(port) + "|)"); return; }
      sockaddr_in a{}; socklen_t al = sizeof a; getsockname(fd, (sockaddr*)&a, &al);
      uint32_t ip = ntohl(a.sin_addr.s_addr);
      char b[96];
      snprintf(b, sizeof b, "Entering Passive Mode (%u,%u,%u,%u,%d,%d)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port >> 8, port & 255);
      reply(227, b); return;
    }
    if (cmd == "PORT" || cmd == "EPRT") { reply(502, "Active mode is not supported, use passive"); return; }
    if (cmd == "REST") { rest = atoll(arg.c_str()); if (rest < 0) rest = 0; reply(350, "Restarting at " + std::to_string(rest)); return; }
    if (cmd == "ALLO") { allo = atoll(arg.c_str()); reply(200, "OK"); return; }
    if (cmd == "SIZE") {
      int64_t s = 0;
      if (vfs::stat(vfs::resolve(cwd, arg), &s) != vfs::FILE_) { reply(550, "No such file"); return; }
      reply(213, std::to_string(s)); return;
    }
    if (cmd == "MDTM") {
      std::string p = vfs::resolve(cwd, arg);
      if (vfs::stat(p) == vfs::NONE) { reply(550, "No such file"); return; }
      reply(213, mdtm(vfs::modifiedTime(p))); return;
    }
    if (cmd == "MLST") {
      std::string p = vfs::resolve(cwd, arg); int64_t s = 0; vfs::Kind k = vfs::stat(p, &s);
      if (k == vfs::NONE) { reply(550, "No such file"); return; }
      out += "250-Listing " + p + "\r\n type=" + (k == vfs::DIR_ ? "dir" : "file") + ";size=" + std::to_string(s) +
             ";modify=" + mdtm(vfs::modifiedTime(p)) + "; " + p + "\r\n250 End\r\n";
      return;
    }
    if (cmd == "MKD" || cmd == "XMKD") {
      std::string p = vfs::resolve(cwd, arg), err;
      if (!vfs::mkdir(p, err)) { reply(550, "Cannot create directory: " + err); return; }
      logf("FTP mkdir %s", p.c_str());
      reply(257, "\"" + p + "\" created"); return;
    }
    if (cmd == "RMD" || cmd == "XRMD") {
      std::string p = vfs::resolve(cwd, arg), err;
      if (!vfs::removeDir(p, false, err)) { reply(550, "Cannot remove: " + err); return; }
      logf("FTP rmdir %s", p.c_str());
      reply(250, "Removed"); return;
    }
    if (cmd == "DELE") {
      std::string p = vfs::resolve(cwd, arg), err;
      if (!vfs::removeFile(p, err)) { reply(550, "Cannot delete: " + err); return; }
      logf("FTP delete %s", p.c_str());
      reply(250, "Deleted"); return;
    }
    if (cmd == "RNFR") {
      renameFrom = vfs::resolve(cwd, arg);
      if (vfs::stat(renameFrom) == vfs::NONE) { renameFrom.clear(); reply(550, "No such file"); return; }
      reply(350, "Ready for RNTO"); return;
    }
    if (cmd == "RNTO") {
      if (renameFrom.empty()) { reply(503, "RNFR first"); return; }
      std::string to = vfs::resolve(cwd, arg), err;
      bool ok = vfs::rename(renameFrom, to, err);
      if (ok) logf("FTP rename %s -> %s", renameFrom.c_str(), to.c_str());
      renameFrom.clear();
      if (!ok) { reply(550, "Rename failed: " + err); return; }
      reply(250, "Renamed"); return;
    }
    if (cmd == "ABOR") {
      if (dataCmd != D_NONE) { closeData(); reply(426, "Transfer aborted"); }
      reply(226, "ABOR OK"); return;
    }
    if (cmd == "LIST" || cmd == "NLST" || cmd == "MLSD") {
      if (!needPasv()) return;
      listing(arg, cmd == "LIST" ? 0 : cmd == "NLST" ? 1 : 2);
      return;
    }
    if (cmd == "RETR") {
      if (!needPasv()) return;
      std::string p = vfs::resolve(cwd, arg), err;
      int64_t off = rest; rest = 0;
      if (vfs::stat(p) != vfs::FILE_) { reply(550, "No such file"); return; }
      auto f = vfs::openRead(p, err);
      if (!f) { reply(550, "Cannot open: " + err); return; }
      int64_t size = vfs::size(f.get());
      if (off > size) off = size;
      pump.sendFile(f, off, -1, transferAttach(p, false, size));
      dataCmd = D_SEND; dataPath = p; dataOffset = off; dataWaitSince = nowSec();
      reply(150, "Sending " + std::to_string(size - off) + " bytes from offset " + std::to_string(off));
      maybeStartData();
      return;
    }
    if (cmd == "STOR" || cmd == "APPE") {
      if (!needPasv()) return;
      std::string p = vfs::resolve(cwd, arg), err;
      int64_t off = rest; rest = 0;
      bool append = cmd == "APPE";
      if (vfs::stat(vfs::parentOf(p)) != vfs::DIR_) { reply(553, "No such directory"); return; }
      // STOR replaces the file, unless other connections are writing parts of
      // it right now; STOR after REST writes at the offset (parts).
      auto f = vfs::openWrite(p, !append && off == 0 ? vfs::TRUNC_IF_IDLE : vfs::TRUNC_NO, allo > 0 && off == 0 ? allo : -1, err);
      int64_t total = allo > 0 ? allo : -1;
      allo = -1;
      if (!f) { reply(553, "Cannot open: " + err); return; }
      if (append) off = vfs::size(f.get());
      if (off == 0) logf("FTP upload %s", p.c_str());
      pump.recvFile(f, off, -1, transferAttach(p, true, total));
      dataCmd = D_RECV; dataPath = p; dataOffset = off; dataWaitSince = nowSec();
      reply(150, "Receiving at offset " + std::to_string(off));
      maybeStartData();
      return;
    }
    if (cmd == "SITE" || cmd == "STAT" || cmd == "HELP") { reply(502, "Not implemented"); return; }
    reply(502, "Unknown command " + cmd);
  }
};

}  // namespace

Conn* makeFtpConn(int fd) { return new FtpConn(fd); }
