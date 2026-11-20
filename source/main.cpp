// SW2Transfer: multi-connection file transfer and file management server for the Switch 2
// (hbmenu under kaiHEN). HTTP (web page + sw2xfer client) on
// 6967; one file can be read or written by many connections at once.
#include <switch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include "gfx.h"
#include <png.h>
#include <math.h>
#include "net.h"
#include "ops.h"
#include "auth.h"

std::string g_webPage, g_clientScript, g_favicon;
#include "buildid.h"
const char* g_appVersion = APP_VERSION;
// "1.4.3 (build 9dcfe17)": tells builds of the same version apart (app and plugin).
const char* g_buildLabel = APP_VERSION " (build " BUILD_ID ")";
Conn* makeHttpConn(int fd);
Conn* makeFtpConn(int fd);

static const char* DATA_DIR = "sdmc:/switch/sw2transfer";

// Socket buffers. The service keeps one memory pool for all our sockets, sized
// sb_efficiency x (send max + receive max); every open socket takes part of it
// and when it runs out every socket on the console stalls (ftpd too). Tests on
// the Switch 2: 1 MB buffers are as fast as 4 MB (46 MB/s on one connection,
// 54 MB/s on 8), so small buffers with a high efficiency leave room for many
// connections. net.cpp also caps open connections at MAX_CONNECTIONS.
static std::string initSockets() {
  static const struct { u32 tx, rx, txMax, rxMax, eff; const char* name; } tries[] = {
    {0x40000, 0x40000, 0x100000, 0x100000, 32, "buffers up to 1 MB"},
    {0x20000, 0x20000, 0x80000, 0x80000, 32, "buffers up to 512 KB"},
  };
  for (auto& t : tries) {
    SocketInitConfig c = *socketGetDefaultInitConfig();
    c.tcp_tx_buf_size = t.tx; c.tcp_rx_buf_size = t.rx;
    c.tcp_tx_buf_max_size = t.txMax; c.tcp_rx_buf_max_size = t.rxMax;
    c.sb_efficiency = t.eff;
    c.num_bsd_sessions = 8;  // network threads each hold one while polling
    c.bsd_service_type = BsdServiceType_Auto;
    if (R_SUCCEEDED(socketInitialize(&c))) return t.name;
  }
  if (R_SUCCEEDED(socketInitializeDefault())) return "default buffers";
  return "";
}

static std::string ipString() {
  u32 ip = 0;
  if (R_FAILED(nifmGetCurrentIpAddress(&ip)) || !ip) return "";
  in_addr a; a.s_addr = ip;
  return inet_ntoa(a);
}

static std::string mbs(double bytesPerSec) {
  char b[32]; snprintf(b, sizeof b, "%.1f MB/s", bytesPerSec / 1048576.0); return b;
}
static std::string human(double n) {
  char b[32];
  if (n < 1024) snprintf(b, sizeof b, "%.0f B", n);
  else if (n < 1048576) snprintf(b, sizeof b, "%.1f KB", n / 1024);
  else if (n < 1073741824.0) snprintf(b, sizeof b, "%.1f MB", n / 1048576);
  else snprintf(b, sizeof b, "%.2f GB", n / 1073741824.0);
  return b;
}

// ---------------------------------------------------------------- background plugin
// The kaiHEN plugin build of SW2Transfer (plugin/plugin_main.cpp) serves
// transfers from startup, also while games run. When it is running, this app
// does not start a server of its own: it shows the plugin's activity (polled
// from 127.0.0.1:6967/api/status) and passes on Password, Cancel all and Stop.
// The panel (A) installs, updates, stops and removes it.
static const char* PLUGIN_DIR = "sdmc:/kaihen/plugins";
static const char* PLUGIN_PATH = "sdmc:/kaihen/plugins/sw2transfer.nso";
static const char* PLUGIN_BUNDLED = "romfs:/plugin/sw2transfer.nso";
// Which build the app installed (the plugin file itself is compressed, so its
// version cannot simply be read from it).
static const char* PLUGIN_RECORD = "sdmc:/switch/sw2transfer/plugin-installed.txt";

// One HTTP request to this console's port 6967. Returns the HTTP status, -1
// when nothing listens there (connection refused), or -2 on other failures.
static int localHttp(const char* method, const std::string& path, std::string& body, int timeoutMs = 2000) {
  body.clear();
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -2;
  timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(6967); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { int e = errno; close(fd); return e == ECONNREFUSED ? -1 : -2; }
  std::string req = std::string(method) + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
  for (size_t off = 0; off < req.size();) {
    ssize_t n = send(fd, req.data() + off, req.size() - off, 0);
    if (n <= 0) { close(fd); return -2; }
    off += n;
  }
  std::string in; char b[8192];
  for (;;) {
    ssize_t n = recv(fd, b, sizeof b, 0);
    if (n > 0) { in.append(b, n); if (in.size() > (4u << 20)) break; continue; }
    if (n == 0) break;
    close(fd); return -2;
  }
  close(fd);
  size_t sp = in.find(' '), he = in.find("\r\n\r\n");
  if (sp == std::string::npos || he == std::string::npos) return -2;
  body = in.substr(he + 4);
  return atoi(in.c_str() + sp + 1);
}

// What the plugin reports, refreshed by a poller thread. Every poll builds new
// rows (with the speed worked out from the previous poll), so the screen never
// reads a row the poller is changing.
struct PluginView {
  std::mutex mu;
  bool ok = false;     // the last poll was answered
  int refused = 0;     // polls in a row that found nothing listening
  std::string version, sockets;
  int ftpPort = 0;
  double uptime = 0, polledAt = 0;
  int64_t up = 0, down = 0;
  int conns = 0;
  std::map<uint64_t, TransferRef> rows;
  std::vector<TransferRef> active;
  std::vector<std::string> logs;
};

static std::vector<std::string> splitTabs(const std::string& line, size_t n) {  // n fields; the last takes the rest
  std::vector<std::string> f;
  size_t p = 0;
  while (f.size() + 1 < n) {
    size_t t = line.find('\t', p);
    if (t == std::string::npos) break;
    f.push_back(line.substr(p, t - p)); p = t + 1;
  }
  f.push_back(line.substr(p));
  return f;
}

static void pollPlugin(PluginView& v) {
  std::string body;
  int code = localHttp("GET", "/api/status", body, 3000);
  double now = nowSec();
  std::lock_guard<std::mutex> lk(v.mu);
  if (code != 200) { v.ok = false; v.refused = code == -1 ? v.refused + 1 : 0; return; }
  double dt = v.polledAt > 0 ? now - v.polledAt : 0;
  v.ok = true; v.refused = 0; v.polledAt = now;
  std::map<uint64_t, TransferRef> rows;
  std::vector<TransferRef> active;
  std::vector<std::string> logs;
  for (size_t p = 0; p < body.size();) {
    size_t e = body.find('\n', p);
    if (e == std::string::npos) e = body.size();
    std::string line = body.substr(p, e - p);
    p = e + 1;
    if (line.size() < 2) continue;
    if (line[0] == 'S') {
      // 9 fields; plugins from before FTP send 8 (no FTP port).
      auto f = splitTabs(line, 9);
      if (f.size() == 9 && f[7].find_first_not_of("0123456789") != std::string::npos) f = splitTabs(line, 8);
      if (f.size() < 8) continue;
      v.version = f[1]; v.up = atoll(f[3].c_str()); v.down = atoll(f[4].c_str());
      v.conns = atoi(f[5].c_str()); v.uptime = atof(f[6].c_str());
      v.ftpPort = f.size() == 9 ? atoi(f[7].c_str()) : 0;
      v.sockets = f.back();
    } else if (line[0] == 'T') {
      // 13 fields; plugins from before the via field send 12 (paths start with '/').
      auto f = splitTabs(line, 13);
      if (f.size() == 13 && !f[11].empty() && f[11][0] == '/') f = splitTabs(line, 12);
      if (f.size() < 12) continue;
      auto x = std::make_shared<Transfer>();
      x->id = strtoull(f[1].c_str(), nullptr, 10);
      x->upload = f[2] == "1"; x->copy = f[3] == "1"; x->cancelled = f[4] == "1";
      x->bytes = atoll(f[5].c_str()); x->total = atoll(f[6].c_str());
      x->conns = atoi(f[7].c_str()); x->peakConns = atoi(f[8].c_str());
      x->started = now - atof(f[9].c_str()); x->lastActive = now - atof(f[10].c_str());
      x->path = f.back();
      if (f.size() == 13 && f[11] != "-") x->via = f[11];
      auto prev = v.rows.find(x->id);
      if (prev != v.rows.end() && dt > 0) {
        double inst = (x->bytes - prev->second->bytes) / dt;
        x->uiSpeed = prev->second->uiSpeed == 0 ? inst : prev->second->uiSpeed * 0.5 + inst * 0.5;
      }
      x->uiLastBytes = x->bytes;
      rows[x->id] = x;
      active.push_back(x);
    } else if (line[0] == 'L') logs.push_back(line.substr(2));
  }
  v.rows.swap(rows); v.active.swap(active); v.logs.swap(logs);
}

static bool fileExists(const char* p) { struct stat st; return stat(p, &st) == 0; }
static std::string readWhole(const char* p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss; ss << f.rdbuf();
  return ss.str();
}
// Copies the plugin carried by this app into /kaihen/plugins (new name first,
// then swapped in, so a half-written plugin is never there).
static bool installPlugin(const std::string& bytes, std::string& err) {
  if (bytes.empty()) { err = "this app has no plugin inside"; return false; }
  mkdir("sdmc:/kaihen", 0777);
  mkdir(PLUGIN_DIR, 0777);
  std::string tmp = std::string(PLUGIN_PATH) + ".new";
  FILE* f = fopen(tmp.c_str(), "wb");
  if (!f) { err = "cannot write to /kaihen/plugins"; return false; }
  bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  ok = fclose(f) == 0 && ok;
  if (ok) { remove(PLUGIN_PATH); ok = rename(tmp.c_str(), PLUGIN_PATH) == 0; }
  if (!ok) { remove(tmp.c_str()); err = "writing the plugin failed"; return false; }
  if (FILE* r = fopen(PLUGIN_RECORD, "w")) { fputs(g_buildLabel, r); fclose(r); }  // the bundled plugin is this app's build
  return true;
}
// Asks the running plugin to stop and waits until it no longer answers.
static bool stopPlugin(std::string& err) {
  std::string b;
  int code = localHttp("POST", "/api/stop", b, 3000);
  if (code != 200) { err = code < 0 ? "the plugin did not answer" : "the plugin refused (" + std::to_string(code) + ")"; return false; }
  for (int i = 0; i < 100; i++) {  // up to 10 s: it cancels transfers in progress first
    if (localHttp("GET", "/api/info", b, 1000) == -1) return true;
    svcSleepThread(100000000ULL);
  }
  err = "the plugin is still answering";
  return false;
}

static std::string ago(double sec) {
  char b[48];
  if (sec < 90) snprintf(b, sizeof b, "%.0f s", sec);
  else if (sec < 5400) snprintf(b, sizeof b, "%.0f min", sec / 60);
  else if (sec < 172800) snprintf(b, sizeof b, "%.1f h", sec / 3600);
  else snprintf(b, sizeof b, "%.0f days", sec / 86400);
  return b;
}

// ---------------------------------------------------------------- touch
// On-screen buttons: each frame registers the buttons it draws; a tap (finger
// down and up without moving much) on one acts as a press of its key, so the
// key handling below serves buttons and touch alike.
struct TouchBtn { float x, y, w, h; u64 key; };
static std::vector<TouchBtn> g_btns, g_btnsPrev;  // this frame's; last frame's (what a tap hits)
static float g_touchX = -1, g_touchY = -1;         // finger now, in UI units (-1: none)

// Button layout: [pad] (badge with the key) [gap] label [pad]. gfx::text centres
// its text in a line box 1.25 x size tall, so that is what gets centred.
static const float BTN_PAD = 18, BTN_GAP = 12;
static float badgeSize(float size) { return roundf(size * 1.25f * UI_SCALE) / UI_SCALE; }
static float buttonWidth(const std::string& key, const std::string& label, float size) {
  return BTN_PAD + (key.empty() ? 0 : badgeSize(size) + BTN_GAP) + gfx::textWidth(label, size) + BTN_PAD;
}
// Draws a button (round badge with the key, then the label) and registers it.
static void touchButton(float x, float y, float w, float h, const std::string& key, const std::string& label, u64 code, float size = 17,
                        bool centred = false) {
  using gfx::rgb;
  // Whole screen pixels, and the outline drawn as a filled shape: a thin
  // border at a fractional position can fall between pixels and vanish on one
  // side (as SW2PS2's R icon did).
  auto snap = [](float v) { return roundf(v * UI_SCALE) / UI_SCALE; };
  float x2 = snap(x + w), y2 = snap(y + h);
  x = snap(x); y = snap(y); w = x2 - x; h = y2 - y;
  bool pressed = g_touchX >= x && g_touchX < x + w && g_touchY >= y && g_touchY < y + h;
  const float b = 2 / UI_SCALE;  // 2 screen pixels
  gfx::rect(x, y, w, h, pressed ? rgb(0x4fa3ff) : rgb(0x3a3f4a), 12);
  gfx::rect(x + b, y + b, w - 2 * b, h - 2 * b, pressed ? rgb(0x2f3440) : rgb(0x1a1d23), 12 - b);
  float cx = x + BTN_PAD;
  if (centred) cx = snap(x + (w - (buttonWidth(key, label, size) - 2 * BTN_PAD)) / 2);  // badge and label in the middle
  if (!key.empty()) {
    float d = badgeSize(size), by = snap(y + (h - d) / 2), ks = size * 0.8f;
    // a blue ring (drawn filled, like the outline) with the key in blue
    gfx::rect(cx, by, d, d, rgb(0x4fa3ff), d / 2);
    gfx::rect(cx + b, by + b, d - 2 * b, d - 2 * b, pressed ? rgb(0x2f3440) : rgb(0x1a1d23), d / 2 - b);
    gfx::textCenter(key, cx + d / 2, by + d / 2, ks, rgb(0x4fa3ff), gfx::BOLD);
    cx += d + BTN_GAP;
  }
  gfx::textMid(label, cx, y + h / 2, size, rgb(0xe8eaee));
  g_btns.push_back({x, y, w, h, code});
}

// Turns the touch screen into key presses on the buttons of the last frame.
// Its coordinates are taken as 1280x720 until one beyond that is seen (the
// Switch 2 screen may report 1920x1080); the bottom row of buttons is beyond
// it either way. The first taps are logged to check this.
struct TouchReader {
  bool down = false, moved = false, wide = false;
  float sx = 0, sy = 0, lx = 0, ly = 0;
  u32 rawX = 0, rawY = 0;
  int logged = 0;
  u64 poll() {
    HidTouchScreenState st{};
    if (hidGetTouchScreenStates(&st, 1) && st.count > 0) {
      rawX = st.touches[0].x; rawY = st.touches[0].y;
      if (rawX > 1280 || rawY > 720) wide = true;
      float ux = rawX * (float)gfx::UI_W / (wide ? 1920.f : 1280.f), uy = rawY * (float)gfx::UI_H / (wide ? 1080.f : 720.f);
      if (!down) { down = true; moved = false; sx = ux; sy = uy; }
      if (fabsf(ux - sx) > 40 || fabsf(uy - sy) > 40) moved = true;
      lx = ux; ly = uy; g_touchX = ux; g_touchY = uy;
      return 0;
    }
    if (!down) return 0;
    down = false; g_touchX = g_touchY = -1;
    if (logged < 10) { logged++; logf("touch: tap at raw %u,%u -> UI %.0f,%.0f (%s)", rawX, rawY, lx, ly, wide ? "1920x1080" : "1280x720"); }
    if (moved) return 0;
    for (auto& b : g_btnsPrev)
      if (lx >= b.x && lx < b.x + b.w && ly >= b.y && ly < b.y + b.h) return b.key;
    return 0;
  }
};

// Help screen (Y): how to use the web page and the command-line client.
static void drawHelp(const std::string& host, int port) {
  using gfx::rgb;
  const auto TEXT = rgb(0xe8eaee), MUTED = rgb(0x9aa1ad), TITLE = rgb(0x4fa3ff), CODE = rgb(0x8fc8ff);
  const float M = 40;
  gfx::rect(0, 0, gfx::UI_W, gfx::UI_H, rgb(0x111317, 0.97f));
  gfx::rect(M - 16, 20, gfx::UI_W - 2 * (M - 16), gfx::UI_H - 40, rgb(0x1a1d23), 14, 1, rgb(0x2a2e36));
  std::string url = "http://" + host + ":" + std::to_string(port);
  struct L { int kind; std::string s; };  // 0 heading, 1 text, 2 code, 3 gap, 4 table row "what\tcommand"
  const std::string B = "\xE2\x80\xA2  ", DOT = "   \xC2\xB7   ";
  std::vector<L> lines = {
    {0, "Web browser (no install)"},
    {1, B + "On a computer or phone on the same network, open  " + url + "  (the fastest way, with the command line below)"},
    {1, B + "Click a folder to open it; uploads go into the folder you are in. Upload files, Upload folder, or drag them onto the page."},
    {1, B + "Click a file to tick it. Download saves it (several ticked: one transfer.zip). Rename, Delete and New folder manage files."},
    {1, B + "Cancel all (web page), X twice (here) or the cancel command stops every transfer and copy; unfinished ones are deleted."},
    {1, B + "Password: press \xE2\x80\x93 to set, change or remove it. Browsers, FTP and the command line then ask for it."},
    {1, B + "FTP: any FTP client (FileZilla, Cyberduck, WinSCP...), server " + host + ", port 6700, passive mode. Slower: one connection per file."},
    {1, B + "Background plugin (A): serves transfers from startup, also while games run, without this app open."},
    {3, ""},
    {0, "Command line (Mac, Windows, Linux with Python 3)"},
    {1, "Get sw2xfer.py from  " + url + "/sw2xfer.py  (or Help on the web page). Start every command with:"},
    {2, "python3 sw2xfer.py --host " + host},
    {3, ""},
    {4, "Upload to the Switch\tupload /SourceFileOrFolder /DestinationPath"},
    {4, "Download from the Switch\tdownload /SourceFileOrFolder /DestinationPath"},
    {4, "Several items (the last path is the destination)\tupload game.iso MyFolder /roms/ps2"},
    {4, "List, new folder, delete, move\tls /path" + DOT + "mkdir /path/new" + DOT + "rm -r /path/old" + DOT + "mv /path/a /path/b"},
    {4, "Stop every transfer\tcancel"},
    {4, "Files already there (otherwise you are asked)\t--overwrite-existing   or   --skip-existing"},
    {4, "Speed test (the test file is deleted after)\tbench /path/test.bin"},
  };
  float y = 44;
  gfx::text("Help", M + 8, y, 30, TITLE, gfx::BOLD);
  y += 56;
  const float X = M + 24, COL = X + 470, WIDTH = gfx::UI_W - 2 * M - 30;
  for (auto& l : lines) {
    if (l.kind == 0) { gfx::text(l.s, M + 8, y, 22, TEXT, gfx::BOLD); y += 38; }
    else if (l.kind == 1) { gfx::text(gfx::ellipsize(l.s, WIDTH, 17), X, y, 17, MUTED); y += 30; }
    else if (l.kind == 2) { gfx::text(l.s, X, y, 18, CODE, gfx::MONO); y += 30; }
    else if (l.kind == 4) {
      size_t tab = l.s.find('\t');
      gfx::text(l.s.substr(0, tab), X, y, 17, TEXT);
      gfx::text(gfx::ellipsize(l.s.substr(tab + 1), M + 24 + WIDTH - COL, 17, gfx::MONO), COL, y, 17, CODE, gfx::MONO);
      y += 30;
    } else y += 12;
  }
  float bw = buttonWidth("B", "Close", 24);
  touchButton(gfx::UI_W - M - bw, gfx::UI_H - 96, bw, 62, "B", "Close", HidNpadButton_B, 24);
}

// Background plugin panel (A on the main screen).
struct PluginPanel {
  bool running = false, installed = false, upToDate = false, stoppedHere = false;
  std::string version, sockets;  // of the running plugin
  std::string installedVersion;  // what the app recorded when it installed it ("" unknown)
  double uptime = 0, stopAsk = 0, removeAsk = 0;
};
static void drawPluginPanel(const PluginPanel& st) {
  using gfx::rgb;
  const auto TEXT = rgb(0xe8eaee), MUTED = rgb(0x9aa1ad), ACCENT = rgb(0xff3b4f), GREEN = rgb(0x5bd17a), BLUE = rgb(0x4fa3ff);
  const float M = 40, X = M + 24, WRAP = gfx::UI_W - 2 * M - 60;
  gfx::rect(0, 0, gfx::UI_W, gfx::UI_H, rgb(0x111317, 0.97f));
  gfx::rect(M - 16, 20, gfx::UI_W - 2 * (M - 16), gfx::UI_H - 40, rgb(0x1a1d23), 14, 1, rgb(0x2a2e36));
  float y = 44;
  gfx::text("Background plugin", M + 8, y, 30, BLUE, gfx::BOLD);
  y += 60;
  const char* about[] = {
    "The plugin runs SW2Transfer in the background from the moment the console starts, also while a game",
    "is running, so this app does not need to be open. It serves at the same address, with the same password.",
    "While it runs, this app shows what it is doing; Password and Cancel all work as usual.",
    "It is a kaiHEN plugin (/kaihen/plugins/sw2transfer.nso), and uses somewhat less memory than this app.",
  };
  for (auto l : about) { gfx::text(l, X, y, 20, MUTED); y += 32; }
  y += 20;
  std::string state; gfx::Color col = MUTED;
  if (st.running) { state = "Running"; col = GREEN; }
  else if (st.installed && st.stoppedHere) { state = "Stopped until the console restarts"; col = ACCENT; }
  else if (st.installed) { state = "Installed: it starts when the console restarts"; col = BLUE; }
  else state = "Not installed";
  gfx::text(state, X, y, 26, col, gfx::BOLD);
  y += 42;
  if (st.running) {
    std::string d = "Installed version: " + (st.version.empty() ? std::string("unknown") : st.version) + "  \xC2\xB7  running for " + ago(st.uptime) +
                    (st.sockets.empty() ? "" : "  \xC2\xB7  sockets: " + st.sockets);
    gfx::text(gfx::ellipsize(d, WRAP, 20), X, y, 20, MUTED); y += 32;
  } else if (st.installed) {
    gfx::text("Installed version: " + (st.installedVersion.empty() ? std::string("unknown (installed by an older app)") : st.installedVersion), X, y, 20, MUTED); y += 32;
  }
  if (st.installed && !st.upToDate) {
    gfx::text(std::string("An updated plugin is available: ") + g_buildLabel + ". Press A to update it, then restart the console.", X, y, 20, ACCENT); y += 32;
  } else if (st.running && st.version != g_buildLabel) {
    gfx::text(std::string("The updated plugin is installed: ") + g_buildLabel + ". Restart the console to start it.", X, y, 20, ACCENT); y += 32;
  }
  y += 12;
  struct A { const char* key; const char* what; bool show; };
  A acts[] = {
    {"A", st.installed ? "Update the plugin" : "Install the plugin (it starts when the console restarts)", !st.installed || !st.upToDate},
    {"X", "Stop it until the console restarts (until then, transfers need this app open)", st.running},
    {"Y", "Uninstall it (transfers then need this app open)", st.installed || st.running},
    {"B", "Close", true},
  };
  const u64 codes[] = {HidNpadButton_A, HidNpadButton_X, HidNpadButton_Y, HidNpadButton_B};
  float bw = 0;  // one width for all, a tidy list
  for (auto& a : acts) if (a.show) bw = std::max(bw, buttonWidth(a.key, a.what, 24));
  for (int i = 0; i < 4; i++) {
    auto& a = acts[i];
    if (!a.show) continue;
    touchButton(X, y, bw, 62, a.key, a.what, codes[i], 24);
    y += 70;
  }
  const char* ask = nullptr;
  if (st.stopAsk > 0) ask = "Press X again to stop the background plugin until the console restarts. Transfers in progress are cancelled, and until the restart SW2Transfer must be open to transfer files.";
  if (st.removeAsk > 0) ask = "Press Y again to uninstall the background plugin. Transfers in progress are cancelled, and from then on SW2Transfer must be open to transfer files.";
  if (ask) {  // two lines: what to press, then what it means
    std::string all = ask, first = all, rest;
    size_t dot = all.find(". ");
    if (dot != std::string::npos) { first = all.substr(0, dot + 1); rest = all.substr(dot + 2); }
    float ay = gfx::UI_H - 170, aw = gfx::UI_W - 2 * M;
    gfx::rect(M, ay, aw, 92, ACCENT, 14);
    gfx::text(gfx::ellipsize(first, aw - 40, 20, gfx::BOLD), M + 20, ay + 16, 20, TEXT, gfx::BOLD);
    gfx::text(gfx::ellipsize(rest, aw - 40, 18), M + 20, ay + 52, 18, TEXT);
  }
}

// The app icon for the title bar: decoded from romfs, shrunk on the CPU by
// area averaging to exactly px x px screen pixels (the GPU path samples
// nearest), with rounded corners.
static gfx::Image* loadIcon(int px, float radius) {
  png_image img; memset(&img, 0, sizeof img); img.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&img, "romfs:/web/favicon.png")) return nullptr;
  img.format = PNG_FORMAT_RGBA;
  std::vector<uint8_t> src(PNG_IMAGE_SIZE(img));
  if (!png_image_finish_read(&img, NULL, src.data(), 0, NULL)) return nullptr;
  int sw = img.width, sh = img.height;
  std::vector<uint32_t> out((size_t)px * px);
  for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++) {
      int x0 = x * sw / px, x1 = std::max(x0 + 1, (x + 1) * sw / px);
      int y0 = y * sh / px, y1 = std::max(y0 + 1, (y + 1) * sh / px);
      double acc[4] = {0, 0, 0, 0}; int n = 0;
      for (int sy = y0; sy < y1; sy++)
        for (int sx = x0; sx < x1; sx++, n++)
          for (int c = 0; c < 4; c++) acc[c] += src[((size_t)sy * sw + sx) * 4 + c];
      // rounded corner: distance from the corner circle's centre, 1 px soft edge
      float cx = std::min(std::max(x + 0.5f, radius), px - radius), cy = std::min(std::max(y + 0.5f, radius), px - radius);
      float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
      float cover = std::min(1.0f, std::max(0.0f, radius - d + 0.5f));
      uint32_t r = (uint32_t)(acc[0] / n), g = (uint32_t)(acc[1] / n), b = (uint32_t)(acc[2] / n), a = (uint32_t)(acc[3] / n * cover);
      out[(size_t)y * px + x] = r | (g << 8) | (b << 16) | (a << 24);
    }
  gfx::Image* im = gfx::imageCreate(px, px);
  gfx::imageUpload(im, out.data());
  return im;
}

// System keyboard in password mode. Returns 1 with text, 0 if cancelled,
// -1 if the keyboard could not be opened (err set).
static int askPassword(const char* header, const char* sub, std::string& out, std::string& err) {
  SwkbdConfig kbd;
  Result rc = swkbdCreate(&kbd, 0);
  if (R_FAILED(rc)) { char b[64]; snprintf(b, sizeof b, "keyboard unavailable (0x%x)", rc); err = b; return -1; }
  swkbdConfigMakePresetPassword(&kbd);
  swkbdConfigSetHeaderText(&kbd, header);
  swkbdConfigSetSubText(&kbd, sub);
  swkbdConfigSetGuideText(&kbd, "Password");
  swkbdConfigSetStringLenMin(&kbd, 0);
  swkbdConfigSetStringLenMax(&kbd, 64);
  char buf[512] = {0};
  rc = swkbdShow(&kbd, buf, sizeof buf);
  swkbdClose(&kbd);
  if (R_FAILED(rc)) return 0;  // B / cancelled
  out = buf;
  return 1;
}

int main(int, char**) {
  romfsInit();
  mkdir(DATA_DIR, 0777);
  { FILE* f = fopen("sdmc:/switch/sw2transfer/log.txt", "w"); if (f) fclose(f); }
  setLogFile("sdmc:/switch/sw2transfer/log.txt");
  logf("SW2Transfer %s", APP_VERSION);
  nifmInitialize(NifmServiceType_User);
  std::string sockMode = initSockets();
  logf("sockets: %s", sockMode.empty() ? "FAILED" : sockMode.c_str());
  g_sockMode = sockMode;
  { std::ifstream w("romfs:/web/index.html"); std::stringstream ss; ss << w.rdbuf(); g_webPage = ss.str(); }
  { std::ifstream w("romfs:/web/favicon.png", std::ios::binary); std::stringstream ss; ss << w.rdbuf(); g_favicon = ss.str(); }
  { std::ifstream w("romfs:/web/sw2xfer.py"); std::stringstream ss; ss << w.rdbuf(); g_clientScript = ss.str(); }
  vfs::init("");
  auth::init("sdmc:/switch/sw2transfer/settings.json");
  logf("password: %s", auth::enabled() ? "on" : "off");

  ServerConfig cfg;
  std::string err;
  bool serving = false;
  auto startServer = [&]() {
    err.clear();
    serving = !sockMode.empty() && serverStart(cfg, makeHttpConn, err, makeFtpConn);
    if (!serving && err.empty()) err = "socket service unavailable";
    if (serving) logf("serving: HTTP %d", cfg.httpPort);
    else logf("cannot start: %s", err.c_str());
  };
  // Is the background plugin already serving? Then show it instead.
  PluginView view;
  std::atomic<bool> pluginMode{false};
  if (!sockMode.empty()) {
    std::string b;
    pluginMode = localHttp("GET", "/api/info", b, 1500) == 200 && b.find("\"background\":true") != std::string::npos;
  }
  if (pluginMode) { logf("the background plugin is running: showing its activity"); pollPlugin(view); }
  else startServer();
  const std::string bundledPlugin = readWhole(PLUGIN_BUNDLED);
  PluginPanel panel;
  auto refreshPanel = [&]() {
    panel.installed = fileExists(PLUGIN_PATH);
    panel.upToDate = panel.installed && readWhole(PLUGIN_PATH) == bundledPlugin;
    panel.installedVersion = panel.installed ? readWhole(PLUGIN_RECORD) : "";
    if (panel.upToDate) panel.installedVersion = g_buildLabel;
  };
  refreshPanel();

  // Stay awake while serving.
  appletSetAutoSleepDisabled(true);
  appletSetMediaPlaybackState(true);

  if (!gfx::init(err)) { logf("gfx: %s", err.c_str()); if (serving) serverStop(); socketExit(); romfsExit(); return 1; }
  std::atomic<bool> pollQuit{false};
  std::thread poller([&] {
    while (!pollQuit) {
      if (pluginMode) pollPlugin(view);
      for (int i = 0; i < 5 && !pollQuit; i++) svcSleepThread(100000000ULL);
    }
  });
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad; padInitializeDefault(&pad);
  hidInitializeTouchScreen();
  TouchReader touch;
  // 40 UI units = 50 screen pixels (UI_SCALE 1.25)
  const float ICON = 40;
  gfx::Image* icon = loadIcon((int)(ICON * UI_SCALE + 0.5f), 8 * UI_SCALE);

  using gfx::rgb;
  const auto BG = rgb(0x111317), PANEL = rgb(0x1a1d23), LINE = rgb(0x2a2e36), TEXT = rgb(0xe8eaee), MUTED = rgb(0x9aa1ad);
  const auto ACCENT = rgb(0xff3b4f), UP = rgb(0x4fa3ff), DOWN = rgb(0x5bd17a), BARBG = rgb(0x2a2e36);

  enum { HIST = 120 };  // half-second samples = 60 s
  std::vector<double> histUp(HIST, 0), histDown(HIST, 0);
  int64_t lastUp = 0, lastDown = 0; double lastSample = nowSec(), lastReap = nowSec();
  double speedUp = 0, speedDown = 0;
  bool graphLive = false;
  std::string ip = ipString(); double ipCheck = nowSec();

  bool showHelp = false;
  std::string toast; bool toastBad = false; double toastUntil = 0;  // short message after setting a password
  double cancelAsk = 0;  // X pressed once at this time: press again within 3 s to cancel all
  bool showPanel = false;
  int pendingStop = 0;     // 1 stop, 2 stop and uninstall: done on the frame after "Stopping..." is shown
  int lastMode = -1;       // 1 showing the plugin, 0 serving here (to reset the speed sample on a change)
  auto say = [&](const std::string& msg, bool bad, double secs = 5) { toast = msg; toastBad = bad; toastUntil = nowSec() + secs; };
  while (appletMainLoop()) {
    padUpdate(&pad);
    g_btnsPrev.swap(g_btns); g_btns.clear();
    u64 down = padGetButtonsDown(&pad) | touch.poll();
    if (down & HidNpadButton_Plus) break;
    if (pendingStop) {  // stop (and maybe remove) the plugin; serve from here after
      bool del = pendingStop == 2;
      pendingStop = 0;
      std::string e;
      bool ok = true, wasRunning = pluginMode;
      if (wasRunning) {
        ok = stopPlugin(e);
        if (ok) { pluginMode = false; panel.stoppedHere = true; logf("background plugin stopped from the Switch"); startServer(); }
      }
      if (ok && del && remove(PLUGIN_PATH) != 0 && fileExists(PLUGIN_PATH)) { ok = false; e = "could not delete it"; }
      if (ok && del) remove(PLUGIN_RECORD);
      if (ok && del) { panel.stoppedHere = false; logf("background plugin removed"); }
      refreshPanel();
      if (!ok) say(std::string(del ? "Could not uninstall the plugin: " : "Could not stop the plugin: ") + e, true, 6);
      else if (del) say("Background plugin uninstalled. Keep SW2Transfer open to transfer files.", false, 7);
      else say("Background plugin stopped until the console restarts. Keep SW2Transfer open to transfer files.", false, 7);
    }
    // A opens the panel; inside it A installs or updates and B closes it (this
    // press is used up, so opening never installs).
    if ((down & HidNpadButton_A) && !showHelp && !showPanel) { showPanel = true; panel.stopAsk = panel.removeAsk = 0; refreshPanel(); down = 0; }
    if (showPanel) {  // the panel has the buttons
      double now = nowSec();
      if (down & HidNpadButton_B) { showPanel = false; panel.stopAsk = panel.removeAsk = 0; }
      if ((down & HidNpadButton_A) && (!panel.installed || !panel.upToDate)) {
        std::string e;
        bool was = panel.installed;
        if (installPlugin(bundledPlugin, e)) {
          logf(was ? "background plugin updated" : "background plugin installed");
          say(was ? "Plugin updated: restart the console to start the new version" : "Plugin installed: it starts when the console restarts", false, 6);
        } else say("Could not install the plugin: " + e, true, 6);
        refreshPanel();
      }
      if ((down & HidNpadButton_X) && pluginMode) {
        if (panel.stopAsk > 0 && now - panel.stopAsk < 4) { panel.stopAsk = 0; pendingStop = 1; say("Stopping the background plugin\xE2\x80\xA6", false, 30); }
        else { panel.stopAsk = now; panel.removeAsk = 0; }
      }
      if ((down & HidNpadButton_Y) && (panel.installed || pluginMode)) {
        if (panel.removeAsk > 0 && now - panel.removeAsk < 4) { panel.removeAsk = 0; pendingStop = 2; say("Uninstalling the background plugin\xE2\x80\xA6", false, 30); }
        else { panel.removeAsk = now; panel.stopAsk = 0; }
      }
      if (panel.stopAsk > 0 && now - panel.stopAsk >= 4) panel.stopAsk = 0;
      if (panel.removeAsk > 0 && now - panel.removeAsk >= 4) panel.removeAsk = 0;
      down = 0;
    }
    if (pluginMode) {  // the plugin went away (stopped elsewhere, or failed): serve from here
      int refused;
      { std::lock_guard<std::mutex> lk(view.mu); refused = view.refused; }
      if (refused >= 3) {
        pluginMode = false;
        logf("the background plugin stopped answering");
        startServer();
        say(serving ? "The background plugin stopped: this app serves transfers now" : "The background plugin stopped", !serving, 6);
      }
    }
    if ((down & HidNpadButton_Minus) && !showHelp) {
      // Set, change or remove the password with the system keyboard.
      bool had = auth::enabled();
      std::string pw, again, kerr;
      int r = askPassword(had ? "Change the password" : "Set a password",
                          had ? "Leave blank to remove the password." : "Browsers and the command line will ask for it.", pw, kerr);
      if (r < 0) { toast = "Cannot set a password: " + kerr; toastBad = true; }
      else if (r > 0 && pw.empty()) {
        if (had) {
          bool ok = auth::setPassword(""); toast = ok ? "Password removed" : "Could not save the setting"; toastBad = !ok;
          if (ok) logf("password removed");
          if (ok && pluginMode) { std::string b; localHttp("POST", "/api/reload", b); }
        }
      } else if (r > 0) {
        int r2 = askPassword("Type the password again", "To make sure it is right.", again, kerr);
        if (r2 > 0 && again == pw) {
          bool ok = auth::setPassword(pw);
          toast = ok ? "Password set" : "Could not save the password"; toastBad = !ok;
          if (ok) logf("password set");
          if (ok && pluginMode) { std::string b; localHttp("POST", "/api/reload", b); }
        } else if (r2 > 0) { toast = "The passwords did not match; nothing changed"; toastBad = true; }
      }
      if (!toast.empty()) toastUntil = nowSec() + 4;
    }
    if (down & HidNpadButton_Y) showHelp = !showHelp;
    if (down & HidNpadButton_B) { showHelp = false; cancelAsk = 0; }
    if ((down & HidNpadButton_X) && !showHelp) {
      if (cancelAsk > 0 && nowSec() - cancelAsk < 3.0) {
        if (pluginMode) { std::string b; localHttp("POST", "/api/cancel", b); }
        else cancelAll("Switch");
        cancelAsk = 0;
      }
      else cancelAsk = nowSec();
    }
    double t = nowSec();
    // What to show: the plugin's numbers, or this app's own server.
    std::vector<TransferRef> active;
    int64_t totUp, totDown;
    int conns;
    std::vector<std::string> logs;
    std::string plVersion, plSockets;
    double plUptime = 0;
    int ftpPort = g_ftpPort;
    bool plAnswering = true;
    bool viaPlugin = pluginMode;
    if (viaPlugin) {
      std::lock_guard<std::mutex> lk(view.mu);
      active = view.active; totUp = view.up; totDown = view.down; conns = view.conns; logs = view.logs;
      plVersion = view.version; plSockets = view.sockets; plUptime = view.uptime + (view.polledAt > 0 ? t - view.polledAt : 0);
      plAnswering = view.polledAt > 0 && t - view.polledAt < 4;
      ftpPort = view.ftpPort;
    } else {
      active = transfersActive(8.0); totUp = g_bytesUp; totDown = g_bytesDown; conns = g_connections; logs = recentLog(12);
    }
    if ((int)viaPlugin != lastMode) { lastMode = viaPlugin; lastUp = totUp; lastDown = totDown; lastSample = t; }
    panel.running = viaPlugin; panel.version = plVersion; panel.sockets = plSockets; panel.uptime = plUptime;
    if (t - lastSample >= 0.5) {
      if (!viaPlugin) for (auto& x : active) {  // the plugin's rows come with their speed
        int64_t b = x->bytes;
        double inst = (b - x->uiLastBytes) / (t - lastSample);
        x->uiLastBytes = b;
        x->uiSpeed = x->uiSpeed == 0 ? inst : x->uiSpeed * 0.5 + inst * 0.5;
      }
      int64_t u = totUp, d = totDown; double dt = t - lastSample;
      speedUp = (u - lastUp) / dt; speedDown = (d - lastDown) / dt;
      lastUp = u; lastDown = d; lastSample = t;
      // The graph only moves while something is transferring; when all is
      // done it stays put, showing the last transfer.
      bool live = speedUp > 0 || speedDown > 0;
      for (auto& x : active) if (x->conns > 0 && !x->copy) live = true;
      // After a transfer it keeps scrolling until the last bar has gone.
      for (int i = 0; i < HIST && !live; i++) if (histUp[i] + histDown[i] > 0) live = true;
      if (live) {
        histUp.erase(histUp.begin()); histUp.push_back(speedUp);
        histDown.erase(histDown.begin()); histDown.push_back(speedDown);
      }
      graphLive = live;
    }
    if (t - lastReap > 0.25) { vfs::reap(); lastReap = t; }
    if (t - ipCheck > 3.0) { ip = ipString(); ipCheck = t; }

    gfx::beginFrame(BG);
    const float W = gfx::UI_W, M = 32;
    // Title bar
    float tx = M;
    if (icon) { gfx::imageDraw(icon, M, 20, ICON, ICON); tx = M + ICON + 14; }
    gfx::text("SW2", tx, 22, 30, TEXT, gfx::BOLD);
    float tw = gfx::textWidth("SW2", 30, gfx::BOLD);
    gfx::text("Transfer", tx + tw, 22, 30, ACCENT, gfx::BOLD);
    gfx::text(std::string("v") + APP_VERSION, tx + tw + gfx::textWidth("Transfer", 30, gfx::BOLD) + 12, 34, 16, MUTED);
    // Only problems are shown here; the addresses below carry the current IP
    // (re-read every 3 s, so they follow a change of network).
    std::string status = ip.empty() ? "No network connection" : !viaPlugin && !serving ? "Not serving: " + err : "";
    if (!status.empty()) gfx::text(status, W - M - gfx::textWidth(status, 20, gfx::BOLD), 30, 20, ACCENT, gfx::BOLD);
    else if (viaPlugin) {  // make it plain that the background plugin is doing the work
      const char* pill = plAnswering ? "Background plugin running" : "Background plugin not answering";
      // [18] dot [10] text [18], centred on the pill's middle line
      float pw = roundf((18 + 8 + 10 + gfx::textWidth(pill, 18, gfx::BOLD) + 18) * UI_SCALE) / UI_SCALE, px = roundf((W - M - pw) * UI_SCALE) / UI_SCALE;
      // an outline (drawn filled, 2 screen pixels), dot and text in the same colour
      const auto pc = plAnswering ? DOWN : rgb(0xe0a030);
      const float pb = 2 / UI_SCALE;
      gfx::rect(px, 22, pw, 38, pc, 19);
      gfx::rect(px + pb, 22 + pb, pw - 2 * pb, 38 - 2 * pb, BG, 19 - pb);
      gfx::rect(px + 18, 22 + 19 - 4, 8, 8, pc, 4);
      gfx::textMid(pill, px + 18 + 8 + 10, 22 + 19, 18, pc, gfx::BOLD);
    } else if (panel.installed) {
      const char* note = panel.stoppedHere ? "Background plugin stopped until the console restarts" : "Background plugin installed: it starts when the console restarts";
      gfx::text(note, W - M - gfx::textWidth(note, 16), 34, 16, MUTED);
    }

    // How to connect: two cards. The addresses use the current IP.
    std::string host = ip.empty() ? "<switch-ip>" : ip;
    float y = 84;
    const float CW = W - 2 * M;
    {  // how to connect (left half) and who can (right half)
      const float h = 128, half = (CW - 16) / 2, rx = M + half + 16;
      bool locked = auth::enabled();
      gfx::rect(M, y, half, h, PANEL, 14, 1, LINE);
      gfx::rect(M, y, 6, h, DOWN, 3);
      struct Row { const char* label; std::string value; bool ok; } rows[2] = {
        {"Transfer by web browser:", "http://" + host + ":" + std::to_string(cfg.httpPort), true},
        {"Transfer by FTP:", ftpPort ? "ftp://" + host + ":" + std::to_string(ftpPort)
                                     : std::string(viaPlugin ? "update the plugin (A) for FTP" : "off: port 6700 is in use"), ftpPort != 0}};
      float lw = gfx::textWidth(rows[0].label, 18, gfx::BOLD) + 16;  // values line up after the longer label
      for (int i = 0; i < 2; i++) {
        float cy = y + (i == 0 ? 40 : 90), vx = M + 28 + lw;
        gfx::textMid(rows[i].label, M + 28, cy, 18, TEXT, gfx::BOLD);
        // room for the Fastest tag (row 1) or "slower" (row 2) after the value
        std::string v = gfx::ellipsize(rows[i].value, half - 28 - lw - (i == 0 ? 120 : rows[i].ok ? 80 : 20), 20, rows[i].ok ? gfx::MONO : gfx::REGULAR);
        vx += gfx::textMid(v, vx, cy, 20, rows[i].ok ? DOWN : MUTED, rows[i].ok ? gfx::MONO : gfx::REGULAR) + 14;
        if (i == 0) {  // the recommended way: a small outlined tag
          const float tb = 2 / UI_SCALE, th = 26, tw = roundf((gfx::textWidth("Fastest", 14, gfx::BOLD) + 24) * UI_SCALE) / UI_SCALE;
          float tx = roundf(vx * UI_SCALE) / UI_SCALE, ty = roundf((cy - th / 2) * UI_SCALE) / UI_SCALE;
          gfx::rect(tx, ty, tw, th, DOWN, th / 2);
          gfx::rect(tx + tb, ty + tb, tw - 2 * tb, th - 2 * tb, PANEL, th / 2 - tb);
          gfx::textMid("Fastest", tx + 12, ty + th / 2, 14, DOWN, gfx::BOLD);
        } else if (rows[i].ok) gfx::textMid("slower", vx, cy, 14, MUTED);
      }
      gfx::rect(rx, y, half, h, PANEL, 14, 1, LINE);
      const char* pwText = locked ? "Password protected" : "No password: anyone on your network can open it (press \xE2\x80\x93 to set one)";
      const char* hint = "Open on any computer or phone on the same network";
      const char* ftpHint = locked ? "Any FTP client, passive mode: any user name, the password above" : "Any FTP client, passive mode: any user name and password";
      float inner = half - 48;
      gfx::textMid(gfx::ellipsize(pwText, inner, 16, gfx::BOLD), rx + 24, y + 32, 16, locked ? DOWN : TEXT, gfx::BOLD);
      gfx::textMid(gfx::ellipsize(hint, inner, 16), rx + 24, y + 64, 16, MUTED);
      gfx::textMid(gfx::ellipsize(ftpHint, inner, 16), rx + 24, y + 96, 16, MUTED);
      y += h + 14;
    }
    {  // terminal
      const float h = 150;
      gfx::rect(M, y, CW, h, PANEL, 14, 1, LINE);
      gfx::rect(M, y, 6, h, UP, 3);
      gfx::text("Transfer by terminal", M + 28, y + 14, 18, TEXT, gfx::BOLD);
      const char* hint = "Python 3 \xC2\xB7 get sw2xfer.py from Help on the web page";
      gfx::text(hint, W - M - 28 - gfx::textWidth(hint, 16), y + 16, 16, MUTED);
      struct Row { const char* label; const char* verb; gfx::Color c; } rows[2] = {{"Upload", "upload", UP}, {"Download", "download", DOWN}};
      float ry = y + 50;
      for (auto& r : rows) {
        gfx::text(r.label, M + 28, ry + 9, 18, r.c, gfx::BOLD);  // plain coloured label, not a button
        float bx = M + 160, bw = W - M - 28 - bx;
        gfx::rect(bx, ry, bw, 40, BG, 8, 1, LINE);
        std::string cmd = "python3 sw2xfer.py --host " + host + " " + r.verb + " /SourceFileOrFolder /DestinationPath";
        gfx::text(gfx::ellipsize(cmd, bw - 32, 20, gfx::MONO), bx + 16, ry + 8, 20, TEXT, gfx::MONO);
        ry += 48;
      }
      y += h + 16;
    }

    // Speed + graph
    float gw = W - 2 * M, gh = 180;
    gfx::rect(M, y, gw, gh, PANEL, 14, 1, LINE);
    gfx::text("Total", M + 24, y + 16, 18, MUTED);
    gfx::text(mbs(speedUp + speedDown), M + 24, y + 40, 40, TEXT, gfx::BOLD);
    gfx::text("Upload  " + mbs(speedUp), M + 24, y + 94, 20, UP, gfx::BOLD);
    gfx::text("Download  " + mbs(speedDown), M + 24, y + 120, 20, DOWN, gfx::BOLD);
    char line[160];
    snprintf(line, sizeof line, "%d connection%s", conns, conns == 1 ? "" : "s");
    gfx::text(line, M + 24, y + 150, 16, MUTED);
    {
      float gx = M + 300, gy = y + 20, gwid = gw - 300 - 24, ght = gh - 40;
      gfx::rect(gx, gy, gwid, ght, BG, 8);
      double peak = 1048576.0 * 5;
      for (int i = 0; i < HIST; i++) peak = std::max(peak, histUp[i] + histDown[i]);
      float bw = gwid / HIST;
      for (int i = 0; i < HIST; i++) {
        float hd = (float)(histDown[i] / peak) * (ght - 8), hu = (float)(histUp[i] / peak) * (ght - 8);
        if (hd > 0.5f) gfx::rect(gx + i * bw + 1, gy + ght - 4 - hd, bw - 1, hd, DOWN);
        if (hu > 0.5f) gfx::rect(gx + i * bw + 1, gy + ght - 4 - hd - hu, bw - 1, hu, UP);
      }
      bool any = false;
      for (int i = 0; i < HIST; i++) if (histUp[i] + histDown[i] > 0) any = true;
      // Label over the bars, on a backing in the graph's background colour
      // (slightly see-through) so it stays readable when bars are behind it.
      std::string label = any ? "peak " + mbs(peak) + (graphLive ? " \xC2\xB7 last 60 s" : " \xC2\xB7 last transfer (paused)")
                              : std::string("The graph moves while files are transferring");
      gfx::rect(gx + 6, gy + 5, gfx::textWidth(label, 14) + 14, 24, rgb(0x111317, 0.85f), 6);
      gfx::text(label, gx + 13, gy + 8, 14, MUTED);
    }

    // Below the graph: Transfers (left half, down to the bottom of the
    // buttons) and Log (right half, above the buttons).
    y += gh + 20;
    const float GAP = 16, colW = (W - 2 * M - GAP) / 2, rightX = M + colW + GAP;
    const float btnTop = gfx::UI_H - 70, btnBottom = gfx::UI_H - 14;
    {  // Transfers
      float th = btnBottom - y, inner = colW - 48;
      gfx::rect(M, y, colW, th, PANEL, 14, 1, LINE);
      gfx::text("Transfers", M + 24, y + 14, 18, TEXT, gfx::BOLD);
      float ry = y + 50;
      if (active.empty()) gfx::text("Waiting for transfers", M + 24, ry, 17, MUTED);
      for (auto& x : active) {
        if (ry + 52 > y + th - 10) break;
        double dt = t - x->started;
        int64_t b = x->bytes, tot = x->total;
        bool cancelled = x->cancelled;
        bool live = x->conns > 0 && !cancelled;
        double sp = live ? x->uiSpeed : (dt > 0 ? b / std::max(0.001, x->lastActive - x->started) : 0);
        auto col = cancelled ? ACCENT : x->upload ? UP : DOWN;
        // line 1: what and the file's name; its speed (or state) on the right
        std::string state = live ? mbs(sp) : cancelled ? "Cancelled" : "done";
        float sw = gfx::textWidth(state, 16, gfx::MONO);
        gfx::text(state, M + 24 + inner - sw, ry, 16, live || cancelled ? col : MUTED, gfx::MONO);
        size_t slash = x->path.find_last_of('/');
        std::string fname = slash == std::string::npos ? x->path : x->path.substr(slash + 1);
        std::string verb = x->copy ? "Copy to  " : x->upload ? "Upload  " : "Download  ";
        float vw = gfx::textWidth(verb, 17, gfx::BOLD);
        gfx::text(verb, M + 24, ry, 17, live || cancelled ? col : MUTED, gfx::BOLD);
        gfx::text(gfx::ellipsize(fname, inner - vw - sw - 16, 17), M + 24 + vw, ry, 17, live ? TEXT : MUTED);
        // line 2: connections and size, then the progress bar
        const char* via = x->via == "web" ? "Web  \xC2\xB7  " : x->via == "ftp" ? "FTP  \xC2\xB7  " : x->via == "terminal" ? "Command line  \xC2\xB7  " : "";
        snprintf(line, sizeof line, "%s%d conn  \xC2\xB7  %s%s%s", via, live ? (int)x->conns : (int)x->peakConns, human((double)b).c_str(),
                 tot > 0 ? " / " : live ? " so far" : "", tot > 0 ? human((double)tot).c_str() : "");
        gfx::text(line, M + 24, ry + 24, 14, MUTED);
        gfx::rect(M + 24, ry + 46, inner, 5, BARBG, 2.5f);
        if (tot <= 0 && live) {
          // size not known (most FTP uploads): a segment sliding back and forth
          float seg = inner * 0.25f, ph = (float)fmod(t * 0.6, 2.0), pos = ph < 1 ? ph : 2 - ph;
          gfx::rect(M + 24 + (inner - seg) * pos, ry + 46, seg, 5, col, 2.5f);
        } else {
          float frac = tot > 0 ? std::min(1.0, (double)b / tot) : (cancelled ? 0 : 1);
          if (frac > 0) gfx::rect(M + 24, ry + 46, inner * frac, 5, col, 2.5f);
        }
        ry += 64;
      }
    }
    {  // Log, with this session's (or the plugin's) totals in its header
      float lh = btnTop - 12 - y, inner = colW - 48;
      gfx::rect(rightX, y, colW, lh, PANEL, 14, 1, LINE);
      gfx::text("Log", rightX + 24, y + 14, 18, TEXT, gfx::BOLD);
      if (viaPlugin)
        snprintf(line, sizeof line, "plugin, running for %s: up %s, down %s", ago(plUptime).c_str(), human((double)totUp).c_str(), human((double)totDown).c_str());
      else
        snprintf(line, sizeof line, "this session: up %s, down %s", human((double)totUp).c_str(), human((double)totDown).c_str());
      float hw = std::min(gfx::textWidth(line, 15), inner - 70);
      gfx::text(gfx::ellipsize(line, hw, 15), rightX + 24 + inner - hw, y + 17, 15, MUTED);
      int fit = std::max(0, (int)((lh - 52) / 21));
      size_t first = logs.size() > (size_t)fit ? logs.size() - fit : 0;
      float ly = y + 48;
      for (size_t i = first; i < logs.size(); i++) { gfx::text(gfx::ellipsize(logs[i], inner, 14, gfx::MONO), rightX + 24, ly, 14, MUTED, gfx::MONO); ly += 21; }
    }

    // Footer
    if (cancelAsk > 0 && nowSec() - cancelAsk >= 3.0) cancelAsk = 0;
    if (toastUntil > 0 && nowSec() > toastUntil) { toastUntil = 0; toast.clear(); }
    if (!toast.empty()) {
      float aw = gfx::textWidth(toast, 20, gfx::BOLD) + 48;
      gfx::rect((W - aw) / 2, gfx::UI_H - 132, aw, 44, toastBad ? ACCENT : rgb(0x2f8f4e), 22);
      gfx::textMid(toast, (W - aw) / 2 + 24, gfx::UI_H - 132 + 22, 20, TEXT, gfx::BOLD);
    }
    if (cancelAsk > 0) {
      const char* ask = "Press X again to cancel all transfers";
      float aw = gfx::textWidth(ask, 20, gfx::BOLD) + 48;
      gfx::rect((W - aw) / 2, gfx::UI_H - 132, aw, 44, ACCENT, 22);
      gfx::textMid(ask, (W - aw) / 2 + 24, gfx::UI_H - 132 + 22, 20, TEXT, gfx::BOLD);
    }
    {  // buttons under the log: press them, or tap them
      struct B { const char* key; const char* label; u64 code; } bs[] = {
        {"A", "Plugin", HidNpadButton_A}, {"\xE2\x80\x93", "Password", HidNpadButton_Minus}, {"X", "Cancel all", HidNpadButton_X},
        {"Y", "Help", HidNpadButton_Y}, {"+", "Exit", HidNpadButton_Plus}};
      // the largest text (22 down to 16) at which the row fits the right column
      float size = 22, total = 0;
      for (; size > 16; size -= 1) {
        total = 4 * 12;
        for (auto& b : bs) total += buttonWidth(b.key, b.label, size);
        if (total <= colW) break;
      }
      // spanning the log panel exactly: the spare width shared out evenly
      float extra = std::max(0.0f, colW - total) / 5, bx = rightX;
      for (int i = 0; i < 5; i++) {
        float bw = i == 4 ? W - M - bx : buttonWidth(bs[i].key, bs[i].label, size) + extra;
        touchButton(bx, btnTop, bw, btnBottom - btnTop, bs[i].key, bs[i].label, bs[i].code, size, true);
        bx += bw + 12;
      }
    }
    if (showHelp || showPanel) g_btns.clear();  // only the overlay's buttons can be tapped
    if (showHelp) drawHelp(host, cfg.httpPort);
    if (showPanel) drawPluginPanel(panel);
    if (showPanel && !toast.empty()) {  // messages from the panel show over it
      float aw = gfx::textWidth(toast, 20, gfx::BOLD) + 48;
      gfx::rect((W - aw) / 2, 40, aw, 44, toastBad ? ACCENT : rgb(0x2f8f4e), 22);
      gfx::textMid(toast, (W - aw) / 2 + 24, 40 + 22, 20, TEXT, gfx::BOLD);
    }
    {
      std::lock_guard<std::mutex> lk(g_screen.mu);
      if (g_screen.want) {
        std::vector<uint8_t> png;
        if (gfx::screenshotPng(png)) g_screen.png.assign((const char*)png.data(), png.size());
        g_screen.want = false; g_screen.ready = true;
      }
    }
    gfx::endFrame(1);
    gfx::waitPrevious();
  }

  logf("exiting");
  pollQuit = true;
  poller.join();
  opsStop();
  if (serving) serverStop();
  appletSetMediaPlaybackState(false);
  appletSetAutoSleepDisabled(false);
  gfx::imageDestroy(icon);
  gfx::shutdown();
  socketExit();
  nifmExit();
  romfsExit();
  return 0;
}
