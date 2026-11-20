// HTTP server for the web page and the sw2xfer client.
//
//   GET  /                         web page (romfs:/web/index.html)
//   GET  /api/info                 {"name","version","free"}
//   GET  /api/list?path=/x         {"path","entries":[{"name","dir","size"}]}
//   GET  /dl/<path>[?dl=1]         file, Range: bytes=a-b supported (206)
//   HEAD /dl/<path>                size only
//   POST /api/create?path=&size=   create/replace a file, preallocated
//   PUT  /ul/<path>?offset=N       write the body at offset N (no truncation)
//   POST /api/mkdir?path=   /api/delete?path=[&recursive=1]   /api/rename?from=&to=
//   GET  /api/speed?bytes=N        N zero bytes from memory (network test, no SD access)
//   PUT  /api/speed                body read and dropped (network test, nothing written)
//
// A multi-connection upload is create + many PUTs at different offsets on
// separate connections; a download is many ranged GETs. Keep-alive is used.
#include "net.h"
#include "ops.h"
#include "auth.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

extern std::string g_webPage, g_clientScript, g_favicon;
extern const char* g_appVersion;

namespace {

std::string lower(std::string s) { for (auto& c : s) c = tolower((unsigned char)c); return s; }

std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
    } else if (s[i] == '+') o += ' ';
    else o += s[i];
  }
  return o;
}
std::string urlDecodePath(const std::string& s) {  // '+' stays '+' in paths
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2;
    } else o += s[i];
  }
  return o;
}
std::string jsonStr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

std::string headString(int code, const std::string& type, int64_t len, bool keepAlive, const std::string& extra) {
  const char* reason = code == 200 ? "OK" : code == 206 ? "Partial Content" : code == 400 ? "Bad Request" : code == 404 ? "Not Found" :
                       code == 416 ? "Range Not Satisfiable" : code == 410 ? "Gone" : code == 401 ? "Unauthorized" : code == 429 ? "Too Many Requests" : code == 409 ? "Conflict" : code == 500 ? "Internal Server Error" : "Error";
  std::string out = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\n";
  out += "Content-Type: " + type + "\r\nContent-Length: " + std::to_string(len) + "\r\n";
  // No Access-Control-Allow-Origin: only the page served from here (and
  // non-browser clients) may use the API, not other websites.
  out += "Accept-Ranges: bytes\r\nCache-Control: no-store\r\n";
  out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  return out + extra + "\r\n";
}
// A complete JSON response, built on any thread.
std::string jsonResponse(int code, const std::string& body, bool keepAlive) {
  return headString(code, "application/json", (int64_t)body.size(), keepAlive, "") + body;
}

// At most this many uploads/downloads move data at once, across all clients.
// Each one can grow its socket buffers to 4 MB, and the socket memory pool
// (main.cpp) holds 8 of those; past that the Switch's network service stalls
// every socket. Requests over the limit wait, unread, for a free slot.
const int MAX_TRANSFERS = SW2_MAX_TRANSFERS;
std::atomic<int> g_transferSlots{0};
bool takeTransferSlot() {
  int n = g_transferSlots.load();
  while (n < MAX_TRANSFERS)
    if (g_transferSlots.compare_exchange_weak(n, n + 1)) return true;
  return false;
}

// GET /api/status: what the app's screen shows, for the app running as a
// screen for the background plugin. One record per line, tab-separated, free
// text last (tabs and newlines in it become spaces):
//   S version background bytesUp bytesDown connections uptimeSeconds sockets
//   T id up copy cancelled bytes total conns peakConns startedAgo activeAgo path
//   L logLine
std::string statusText() {
  auto clean = [](std::string v) { for (auto& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' '; return v; };
  double t = nowSec();
  char b[256];
#ifdef SW2_PLUGIN
  const int background = 1;
#else
  const int background = 0;
#endif
  snprintf(b, sizeof b, "S\t%s\t%d\t%lld\t%lld\t%d\t%.0f\t", g_appVersion, background, (long long)g_bytesUp.load(),
           (long long)g_bytesDown.load(), g_connections.load(), g_startedAt > 0 ? t - g_startedAt : 0.0);
  std::string out = b + clean(g_sockMode) + "\n";
  for (auto& x : transfersActive(8.0)) {
    snprintf(b, sizeof b, "T\t%llu\t%d\t%d\t%d\t%lld\t%lld\t%d\t%d\t%.2f\t%.2f\t", (unsigned long long)x->id, x->upload ? 1 : 0,
             x->copy ? 1 : 0, x->cancelled ? 1 : 0, (long long)x->bytes.load(), (long long)x->total.load(), x->conns.load(),
             x->peakConns.load(), t - x->started, t - x->lastActive);
    out += b + clean(x->path) + "\n";
  }
  for (auto& l : recentLog(2)) out += "L\t" + clean(l) + "\n";
  return out;
}

struct HttpConn : Conn {
  std::string in, out;
  enum State { HEADERS, BODY_PUMP, SENDING, WAIT_JOB, DISCARD, SINK, SPEED_SEND, PARKED } state = HEADERS;
  bool hasSlot = false;  // holds one of the MAX_TRANSFERS slots
  int64_t discardLeft = 0;  // body bytes still to read and drop (refused upload, or PUT /api/speed)
  int64_t speedLen = 0, speedLeft = 0;  // /api/speed byte counts
  Pump pump;
  bool keepAlive = true, closeAfter = false;
  std::string method, target, reqPath;
  std::map<std::string, std::string> query, headers;
  JobRef job; std::shared_ptr<std::string> jobOut;
  std::string putPath; int64_t putLen = 0, putOff = 0;
  double lastActivity;
  int epoch = 0;  // g_cancelEpoch when the current transfer started

  HttpConn(int f) { fd = f; lastActivity = nowSec(); }
  // However the connection ends, its transfer is finished (or the Switch's
  // transfers panel keeps showing it as running).
  ~HttpConn() { pump.reset(); releaseSlot(); }
  void releaseSlot() { if (hasSlot) { hasSlot = false; g_transferSlots--; } }

  int pollfds(pollfd* o) override {
    short ev = 0;
    if (state == HEADERS || state == DISCARD || state == SINK) ev |= POLLIN;
    if (!out.empty() || state == SPEED_SEND) ev |= POLLOUT;
    else if (state == SENDING || state == BODY_PUMP) ev |= pump.events();
    o[0] = {fd, ev, 0};
    return 1;
  }
  bool busy() override { return pump.busy() || state == WAIT_JOB; }

  void handle(const pollfd* p, int) override {
    short r = p[0].revents;
    if (r) lastActivity = nowSec();
    if (r & POLLNVAL) { fail("socket closed"); return; }
    if (r & POLLOUT) {
      if (!out.empty() && !sendAll(fd, out)) { fail("connection lost"); return; }
      if (out.empty() && pump.active()) pump.onWritable(fd);
      if (out.empty() && state == SPEED_SEND) sendZeros();
    }
    if (r & POLLIN) {
      if (state == HEADERS) readHeaders();
      else if (state == DISCARD || state == SINK) discard();
      else if (state == BODY_PUMP) pump.onReadable(fd);
    }
    if ((r & (POLLERR | POLLHUP)) && !(r & POLLIN)) fail("connection closed");
  }

  void fail(const std::string& why) {
    if (pump.active() && pump.stat && !pump.finished())
      logf("HTTP %s %s: %s", method.c_str(), vfs::pathOf(pump.file.get()).c_str(), why.c_str());
    pump.reset();
    dead = true;
  }

  void tick() override {
    if (dead) return;
    if (state == PARKED) { route(); if (dead || state == PARKED) return; }  // waiting for a transfer slot
    if (state == WAIT_JOB && job->done.load(std::memory_order_acquire)) {
      out += *jobOut; job.reset(); jobOut.reset();
      finishRequest();
    }
    if ((state == BODY_PUMP || state == SENDING) && epoch != g_cancelEpoch) { fail("cancelled"); return; }
    if (state == BODY_PUMP || state == SENDING) {
      pump.tick();
      if (pump.failed) { fail(pump.error); return; }
      if (pump.finished() && out.empty()) {
        if (state == BODY_PUMP) {
          pump.reset();
          respond(200, "application/json", "{\"ok\":true,\"written\":" + std::to_string(putLen) + "}");
        }
        finishRequest();
      }
    }
    if (!out.empty() && !sendAll(fd, out)) { fail("connection lost"); return; }
    if (state == HEADERS && out.empty() && closeAfter) { dead = true; return; }
    // Idle keep-alive: closed after 2 minutes, or 2 seconds when the connection cap is reached (others are waiting).
    if (state == HEADERS && in.empty() && nowSec() - lastActivity > (g_connections >= MAX_CONNECTIONS ? 2 : 120)) dead = true;
  }

  void finishRequest() {
    releaseSlot();
    if (state == SENDING || state == BODY_PUMP) pump.reset();
    state = HEADERS;
    if (!keepAlive) closeAfter = true;
    else if (!in.empty()) parseRequest();  // pipelined
  }

  void discard() {
    char b[65536];
    while (discardLeft > 0) {
      ssize_t n = recv(fd, b, (size_t)std::min<int64_t>(sizeof b, discardLeft), 0);
      if (n > 0) { discardLeft -= n; if (state == SINK) g_bytesUp += n; continue; }
      if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { dead = true; return; }
      return;
    }
    if (state == SINK) respond(200, "application/json", "{\"ok\":true,\"received\":" + std::to_string(speedLen) + "}");
    finishRequest();
  }

  // GET /api/speed: zeros straight from memory.
  void sendZeros() {
#ifdef SW2_PLUGIN
    static const char zeros[1 << 16] = {};  // the plugin keeps its memory small
#else
    static const char zeros[1 << 20] = {};
#endif
    while (speedLeft > 0) {
      ssize_t n = send(fd, zeros, (size_t)std::min<int64_t>(sizeof zeros, speedLeft), 0);
      if (n > 0) { speedLeft -= n; g_bytesDown += n; continue; }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
      dead = true; return;
    }
    finishRequest();
  }

  void readHeaders() {
    char b[16384];
    for (;;) {
      ssize_t n = recv(fd, b, sizeof b, 0);
      if (n == 0) { dead = true; return; }
      if (n < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) dead = true; break; }
      in.append(b, n);
      if (in.size() > 65536 && in.find("\r\n\r\n") == std::string::npos) { dead = true; return; }
      if (in.find("\r\n\r\n") != std::string::npos || (size_t)n < sizeof b) break;  // body is read by the pump
    }
    if (state == HEADERS) parseRequest();
  }

  std::string peerName() {
    sockaddr_in a{}; socklen_t al = sizeof a;
    if (getpeername(fd, (sockaddr*)&a, &al)) return "?";
    return inet_ntoa(a.sin_addr);
  }
  std::string header(const std::string& k) { auto it = headers.find(k); return it == headers.end() ? "" : it->second; }

  void respond(int code, const std::string& type, const std::string& body, const std::string& extra = "") {
    respondHead(code, type, (int64_t)body.size(), extra);
    if (method != "HEAD") out += body;
  }
  void respondHead(int code, const std::string& type, int64_t len, const std::string& extra = "") {
    out += headString(code, type, len, keepAlive, extra);
  }
  void error(int code, const std::string& msg) { respond(code, "application/json", "{\"ok\":false,\"error\":" + jsonStr(msg) + "}"); }

  void parseRequest() {
    size_t end = in.find("\r\n\r\n");
    if (end == std::string::npos) return;
    std::string head = in.substr(0, end);
    in.erase(0, end + 4);
    headers.clear(); query.clear();
    size_t le = head.find("\r\n");
    std::string first = head.substr(0, le);
    size_t a = first.find(' '), bpos = first.find(' ', a + 1);
    if (a == std::string::npos || bpos == std::string::npos) { keepAlive = false; error(400, "bad request line"); return; }
    method = first.substr(0, a);
    target = first.substr(a + 1, bpos - a - 1);
    std::string ver = first.substr(bpos + 1);
    size_t pos = le == std::string::npos ? head.size() : le + 2;
    while (pos < head.size()) {
      size_t e = head.find("\r\n", pos);
      if (e == std::string::npos) e = head.size();
      std::string l = head.substr(pos, e - pos);
      size_t c = l.find(':');
      if (c != std::string::npos) {
        std::string v = l.substr(c + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        headers[lower(l.substr(0, c))] = v;
      }
      pos = e + 2;
    }
    std::string conn = lower(header("connection"));
    keepAlive = ver == "HTTP/1.1" ? conn != "close" : conn == "keep-alive";
    size_t qm = target.find('?');
    reqPath = urlDecodePath(target.substr(0, qm));
    if (qm != std::string::npos) {
      std::string qs = target.substr(qm + 1);
      size_t i = 0;
      while (i <= qs.size()) {
        size_t amp = qs.find('&', i); if (amp == std::string::npos) amp = qs.size();
        std::string kv = qs.substr(i, amp - i);
        size_t eq = kv.find('=');
        if (!kv.empty()) query[urlDecode(kv.substr(0, eq))] = eq == std::string::npos ? "" : urlDecode(kv.substr(eq + 1));
        i = amp + 1;
      }
    }
    route();
  }

  std::string q(const std::string& k) { auto it = query.find(k); return it == query.end() ? "" : it->second; }

  // Runs fn on the disk pool (it must not touch this connection); its
  // result is the complete response.
  void async(std::function<std::string(bool)> fn) {
    auto res = std::make_shared<std::string>();
    jobOut = res;
    bool ka = keepAlive;
    job = diskSubmit([res, fn, ka] { *res = fn(ka); });
    state = WAIT_JOB;
  }

  // Answers with an error, then reads and drops any request body: a client
  // still sending an upload only sees the answer if we do not close on it.
  void refuse(int code, const std::string& body) {
    keepAlive = false;
    respond(code, "application/json", body);
    int64_t len = atoll(header("content-length").c_str());
    int64_t have = std::min<int64_t>((int64_t)in.size(), len > 0 ? len : 0);
    in.erase(0, have);
    discardLeft = len > have ? len - have : 0;
    if (discardLeft > 0) { state = DISCARD; return; }
    finishRequest();
  }

  // Login token from "Authorization: Bearer <t>" (command line) or the
  // sw2t cookie (browser).
  std::string token() {
    std::string a = header("authorization");
    if (a.compare(0, 7, "Bearer ") == 0) return a.substr(7);
    std::string c = header("cookie");
    size_t p = c.find("sw2t=");
    while (p != std::string::npos && p > 0 && c[p - 1] != ' ' && c[p - 1] != ';') p = c.find("sw2t=", p + 1);
    if (p == std::string::npos) return "";
    size_t e = c.find(';', p);
    return c.substr(p + 5, e == std::string::npos ? std::string::npos : e - p - 5);
  }

  void route() {
    if (method == "OPTIONS") { respond(200, "text/plain", ""); return finishRequest(); }
    // Password: everything except the page itself, its icon, the client
    // script, info and login needs a valid token when a password is set.
    bool open = (method == "GET" && (reqPath == "/" || reqPath == "/index.html" || reqPath == "/favicon.png" ||
                                     reqPath == "/favicon.ico" || reqPath == "/sw2xfer.py")) ||
                reqPath == "/api/info" || reqPath == "/api/login" || reqPath == "/api/logout";
    // The console itself (the app showing the background plugin) needs no
    // password: only software on the console can connect from 127.0.0.1, and it
    // can read the SD card anyway.
    bool local = peerName() == "127.0.0.1";
    if (!open && !local && auth::enabled() && !auth::checkToken(token()))
      return refuse(401, "{\"ok\":false,\"error\":\"password required\",\"login\":true}");
    if (reqPath == "/api/status") {  // for the app's screen (tab-separated; see statusText)
      drainBody();
      respond(200, "text/plain; charset=utf-8", statusText());
      return finishRequest();
    }
    if (method == "POST" && (reqPath == "/api/reload" || reqPath == "/api/stop")) {
      drainBody();
      if (!local) { error(403, "only from the console itself"); return finishRequest(); }
      if (reqPath == "/api/reload") { auth::reload(); logf("password: %s (changed on the Switch)", auth::enabled() ? "on" : "off"); }
#ifdef SW2_PLUGIN
      else { logf("stop requested on the Switch"); g_stopRequested = true; }
#else
      else { error(404, "not the background plugin"); return finishRequest(); }
#endif
      respond(200, "application/json", "{\"ok\":true}");
      return finishRequest();
    }
    if (method == "POST" && reqPath == "/api/login") {
      int left = 0;
      if (auth::loginLocked(&left))
        return refuse(429, "{\"ok\":false,\"error\":\"too many wrong passwords, try again in " + std::to_string(left) + " s\"}");
      std::string pw = urlDecodePath(header("x-sw2-password"));  // URL-encoded by the clients (headers are ASCII)
      std::string peer = peerName();
      drainBody();
      // PBKDF2 takes a moment: check it on the disk pool.
      async([pw, peer](bool ka) {
        if (!auth::enabled()) return jsonResponse(200, "{\"ok\":true,\"token\":\"\",\"expires\":0}", ka);
        bool ok = auth::checkPassword(pw);
        auth::loginResult(ok);
        logf("login from %s: %s", peer.c_str(), ok ? "ok" : "wrong password");
        if (!ok) return jsonResponse(401, "{\"ok\":false,\"error\":\"wrong password\",\"login\":true}", ka);
        std::string t = auth::makeToken(auth::TOKEN_SECONDS);
        std::string body = "{\"ok\":true,\"token\":\"" + t + "\",\"expires\":" + t.substr(0, t.find('.')) + "}";
        std::string cookie = "Set-Cookie: sw2t=" + t + "; Max-Age=" + std::to_string((long long)auth::TOKEN_SECONDS) + "; Path=/; HttpOnly; SameSite=Strict\r\n";
        return headString(200, "application/json", (int64_t)body.size(), ka, cookie) + body;
      });
      return;
    }
    if (method == "POST" && reqPath == "/api/logout") {
      drainBody();
      respond(200, "application/json", "{\"ok\":true}", "Set-Cookie: sw2t=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict\r\n");
      return finishRequest();
    }
    // Requests of a transfer that was cancelled are refused, so clients stop.
    if (!xferIdAllowed(q("x"))) return refuse(410, "{\"ok\":false,\"error\":\"cancelled\"}");
    bool bulk = (method == "GET" && reqPath.compare(0, 4, "/dl/") == 0) || (method == "PUT" && reqPath.compare(0, 4, "/ul/") == 0) ||
                reqPath == "/api/speed";
    if (bulk && !hasSlot) {
      if (!takeTransferSlot()) { state = PARKED; return; }
      hasSlot = true;
      if (state == PARKED) state = HEADERS;
    }
    if (method == "GET" && (reqPath == "/" || reqPath == "/index.html")) {
      respond(200, "text/html; charset=utf-8", g_webPage); return finishRequest();
    }
    if (method == "GET" && (reqPath == "/favicon.png" || reqPath == "/favicon.ico")) {
      respond(g_favicon.empty() ? 404 : 200, "image/png", g_favicon, "");
      return finishRequest();
    }
    if (reqPath == "/api/speed" && (method == "GET" || method == "PUT")) {  // network test: no SD access
      if (method == "GET") {
        speedLeft = std::max<int64_t>(0, std::min<int64_t>(atoll(q("bytes").c_str()), (int64_t)8 << 30));
        respondHead(200, "application/octet-stream", speedLeft);
        if (speedLeft == 0) return finishRequest();
        state = SPEED_SEND;
        return;
      }
      speedLen = std::max<int64_t>(0, atoll(header("content-length").c_str()));
      int64_t have = std::min<int64_t>((int64_t)in.size(), speedLen);
      in.erase(0, have);
      g_bytesUp += have;
      discardLeft = speedLen - have;
      state = SINK;
      return discard();
    }
    if (method == "GET" && reqPath == "/sw2xfer.py") {
      respond(200, "text/x-python; charset=utf-8", g_clientScript, "Content-Disposition: attachment; filename=\"sw2xfer.py\"\r\n");
      return finishRequest();
    }
    if (reqPath == "/api/info") {
      int64_t fr = vfs::freeSpace();
      respond(200, "application/json", std::string("{\"name\":\"SW2Transfer\",\"version\":") + jsonStr(g_appVersion) + ",\"free\":" + std::to_string(fr) +
                                             ",\"auth\":" + (auth::enabled() ? "true" : "false") +
#ifdef SW2_PLUGIN
                                             ",\"background\":true" +
#endif
                                             "}");
      return finishRequest();
    }
    if (reqPath == "/api/list") {
      std::string path = vfs::resolve("/", q("path"));
      // Listing goes through the disk pool so a big folder never stalls others.
      async([path](bool ka) {
        std::vector<vfs::Entry> ents; std::string err;
        if (!vfs::list(path, ents, err)) return jsonResponse(404, "{\"ok\":false,\"error\":" + jsonStr(err) + "}", ka);
        std::string s = "{\"ok\":true,\"path\":" + jsonStr(path) + ",\"entries\":[";
        for (size_t i = 0; i < ents.size(); i++) {
          if (i) s += ",";
          s += "{\"name\":" + jsonStr(ents[i].name) + ",\"dir\":" + (ents[i].dir ? "true" : "false") + ",\"size\":" + std::to_string(ents[i].size) +
               ",\"created\":" + std::to_string(ents[i].created) + ",\"modified\":" + std::to_string(ents[i].modified) + "}";
        }
        return jsonResponse(200, s + "]}", ka);
      });
      return;
    }
    if ((method == "GET" || method == "HEAD") && reqPath.compare(0, 4, "/dl/") == 0) return download(vfs::resolve("/", reqPath.substr(3)));
    if (method == "PUT" && reqPath.compare(0, 4, "/ul/") == 0) return upload(vfs::resolve("/", reqPath.substr(3)));
    if (method == "POST" && reqPath == "/api/create") {
      std::string path = vfs::resolve("/", q("path"));
      int64_t size = q("size").empty() ? 0 : atoll(q("size").c_str());
      drainBody();
      async([path, size](bool ka) {
        if (!vfs::mkdirs(vfs::parentOf(path))) return jsonResponse(500, "{\"ok\":false,\"error\":\"cannot create folder\"}", ka);
        std::string err;
        auto f = vfs::openWrite(path, vfs::TRUNC_FORCE, size, err);
        if (!f) return jsonResponse(500, "{\"ok\":false,\"error\":" + jsonStr(err) + "}", ka);
        logf("HTTP create %s (%lld bytes)", path.c_str(), (long long)size);
        return jsonResponse(200, "{\"ok\":true}", ka);
      });
      return;
    }
    if (method == "POST" && reqPath == "/api/cancel") {
      drainBody();
      cancelAll("web/command");
      logf("cancel: answering");
      respond(200, "application/json", "{\"ok\":true}");
      return finishRequest();
    }
    if (method == "POST" && reqPath == "/api/copy") {
      std::string err;
      drainBody();
      auto j = opsCopy(vfs::resolve("/", q("from")), vfs::resolve("/", q("to")), err);
      if (!j) error(409, err);
      else respond(200, "application/json", "{\"ok\":true,\"job\":" + std::to_string(j->id) + "}");
      return finishRequest();
    }
    if (method == "GET" && reqPath == "/api/screen") {  // picture of the Switch screen (PNG, nothing saved)
#ifdef SW2_PLUGIN
      respond(404, "application/json", "{\"ok\":false,\"error\":\"the background plugin has no screen\"}");
      return finishRequest();
#endif
      async([](bool ka) {
        { std::lock_guard<std::mutex> lk(g_screen.mu); g_screen.want = true; g_screen.ready = false; g_screen.png.clear(); }
        for (int i = 0; i < 200; i++) {
          {
            std::lock_guard<std::mutex> lk(g_screen.mu);
            if (g_screen.ready) return headString(200, "image/png", (int64_t)g_screen.png.size(), ka, "") + g_screen.png;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return jsonResponse(503, "{\"ok\":false,\"error\":\"no frame\"}", ka);
      });
      return;
    }
    if (reqPath == "/api/jobs") {
      std::string s = "{\"ok\":true,\"jobs\":[";
      bool first = true;
      for (auto& j : opsJobs()) {
        if (!first) s += ",";
        first = false;
        static const char* st[] = {"queued", "running", "done", "failed"};
        s += "{\"id\":" + std::to_string(j->id) + ",\"from\":" + jsonStr(j->from) + ",\"to\":" + jsonStr(j->to) +
             ",\"total\":" + std::to_string(j->total.load()) + ",\"done\":" + std::to_string(j->done.load()) +
             ",\"state\":\"" + st[j->state.load()] + "\",\"error\":" + jsonStr(j->state == OpJob::FAILED ? j->error : "") + "}";
      }
      respond(200, "application/json", s + "]}");
      return finishRequest();
    }
    if (method == "POST" && (reqPath == "/api/mkdir" || reqPath == "/api/delete" || reqPath == "/api/rename")) {
      std::string what = reqPath.substr(5);
      std::string path = vfs::resolve("/", q(what == "rename" ? "from" : "path"));
      std::string to = vfs::resolve("/", q("to"));
      bool rec = q("recursive") == "1";
      drainBody();
      async([what, path, to, rec](bool ka) {
        std::string err; bool ok;
        if (what == "mkdir") ok = vfs::mkdirs(path) || (err = "cannot create", false);
        else if (what == "rename") {
          // Never replace: moving onto an existing name fails.
          if (vfs::stat(to) != vfs::NONE) { err = "already exists: " + to; ok = false; }
          else if (to.compare(0, path.size() + 1, path + "/") == 0) { err = "cannot move a folder into itself"; ok = false; }
          else ok = vfs::rename(path, to, err);
        }
        else ok = vfs::stat(path) == vfs::DIR_ ? vfs::removeDir(path, rec, err) : vfs::removeFile(path, err);
        if (ok) logf("HTTP %s %s", what.c_str(), path.c_str());
        return ok ? jsonResponse(200, "{\"ok\":true}", ka) : jsonResponse(500, "{\"ok\":false,\"error\":" + jsonStr(err) + "}", ka);
      });
      return;
    }
    drainBody();
    error(404, "no such endpoint");
    finishRequest();
  }

  // Requests we do not read a body for: drop any Content-Length bytes.
  void drainBody() {
    int64_t len = atoll(header("content-length").c_str());
    if (len <= 0) return;
    if ((int64_t)in.size() >= len) in.erase(0, len);
    else { in.clear(); keepAlive = false; }
  }

  void download(const std::string& path) {
    std::string err;
    if (vfs::stat(path) != vfs::FILE_) { error(404, "not found"); return finishRequest(); }
    auto f = vfs::openRead(path, err);
    if (!f) { error(404, err); return finishRequest(); }
    int64_t size = vfs::size(f.get());
    int64_t from = 0, to = size - 1;
    bool ranged = false;
    std::string range = header("range");
    if (range.compare(0, 6, "bytes=") == 0 && range.find(',') == std::string::npos) {
      std::string r = range.substr(6);
      size_t d = r.find('-');
      if (d != std::string::npos) {
        std::string a = r.substr(0, d), b = r.substr(d + 1);
        if (a.empty()) { int64_t n = atoll(b.c_str()); from = size > n ? size - n : 0; }
        else { from = atoll(a.c_str()); if (!b.empty()) to = std::min<int64_t>(atoll(b.c_str()), size - 1); }
        ranged = true;
      }
    }
    if (ranged && (from >= size || from > to)) {
      respond(416, "application/json", "{\"ok\":false}", "Content-Range: bytes */" + std::to_string(size) + "\r\n");
      return finishRequest();
    }
    int64_t len = size == 0 ? 0 : to - from + 1;
    std::string extra;
    if (ranged) extra += "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) + "/" + std::to_string(size) + "\r\n";
    if (q("dl") == "1") extra += "Content-Disposition: attachment; filename=\"" + vfs::baseName(path) + "\"\r\n";
    respondHead(ranged ? 206 : 200, "application/octet-stream", len, extra);
    if (method == "HEAD" || len == 0) return finishRequest();
    pump.sendFile(f, from, len, transferAttach(path, false, size));
    epoch = g_cancelEpoch;
    state = SENDING;
  }

  void upload(const std::string& path) {
    std::string cl = header("content-length");
    if (cl.empty()) { keepAlive = false; error(400, "Content-Length required"); return finishRequest(); }
    putLen = atoll(cl.c_str());
    putOff = q("offset").empty() ? 0 : atoll(q("offset").c_str());
    int64_t total = q("total").empty() ? -1 : atoll(q("total").c_str());
    if (vfs::stat(vfs::parentOf(path)) != vfs::DIR_ && !vfs::mkdirs(vfs::parentOf(path))) {
      keepAlive = false; error(500, "no such folder"); return finishRequest();
    }
    std::string err;
    // A PUT without an offset and without create first behaves like a plain
    // upload (replaces the file); with an offset it writes in place.
    bool whole = q("offset").empty();
    auto f = vfs::openWrite(path, whole ? vfs::TRUNC_FORCE : vfs::TRUNC_NO, whole ? putLen : -1, err);
    if (!f) { keepAlive = false; error(500, err); return finishRequest(); }
    if (lower(header("expect")) == "100-continue") out += "HTTP/1.1 100 Continue\r\n\r\n";
    size_t pre = (size_t)std::min<int64_t>(std::min<int64_t>((int64_t)in.size(), putLen), CHUNK);
    pump.recvFile(f, putOff, putLen, transferAttach(path, true, total >= 0 ? total : (whole ? putLen : -1)), in.data(), pre);
    in.erase(0, pre);
    epoch = g_cancelEpoch;
    state = BODY_PUMP;
  }
};

}  // namespace

Conn* makeHttpConn(int fd) { return new HttpConn(fd); }
