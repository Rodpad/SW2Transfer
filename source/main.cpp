// SW2Transfer: multi-connection file transfer and file management server for the Switch 2
// (hbmenu under kaiHEN). HTTP (web page + sw2xfer client) on
// 6967; one file can be read or written by many connections at once.
#include <switch.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include "gfx.h"
#include <png.h>
#include <math.h>
#include "net.h"
#include "ops.h"
#include "auth.h"

std::string g_webPage, g_clientScript, g_favicon;
const char* g_appVersion = APP_VERSION;
Conn* makeHttpConn(int fd);

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

// Help screen (Y): how to use the web page and the command-line client.
static void drawHelp(const std::string& host, int port) {
  using gfx::rgb;
  const auto TEXT = rgb(0xe8eaee), MUTED = rgb(0x9aa1ad), ACCENT = rgb(0xff3b4f), CODE = rgb(0x8fc8ff);
  const float M = 40;
  gfx::rect(0, 0, gfx::UI_W, gfx::UI_H, rgb(0x111317, 0.97f));
  gfx::rect(M - 16, 20, gfx::UI_W - 2 * (M - 16), gfx::UI_H - 40, rgb(0x1a1d23), 14, 1, rgb(0x2a2e36));
  std::string url = "http://" + host + ":" + std::to_string(port);
  std::string cmd = "python3 sw2xfer.py --host " + host + " ";
  struct L { int kind; std::string s; };  // 0 heading, 1 text, 2 code, 3 gap
  std::vector<L> lines = {
    {0, "Web browser (no install)"},
    {1, "1. On a computer or phone on the same network, open  " + url},
    {1, "2. Click folders to open them. Uploads go into the folder you are in."},
    {1, "3. Upload files / Upload folder, or drag files and whole folders onto the page (subfolders are kept)."},
    {1, "4. Click a file to download it. Each row has Rename and Delete; New folder makes a folder."},
    {1, "5. Connections and Chunk: 8 and 2 MB were fastest in tests. Transfers show their speed live."},
    {1, "Password: press \xE2\x80\x93 to set, change or remove it. Browsers and the command line then ask for it once every 12 hours."},
    {1, "6. Cancel all (web page), X twice (here) or the cancel command stops every transfer and copy; unfinished ones are deleted."},
    {3, ""},
    {0, "Command line (Mac, Windows, Linux with Python 3)"},
    {1, "Get the client from  " + url + "/sw2xfer.py  (or the Help button on the web page). The last path is the destination."},
    {2, cmd + "upload /SourceFileOrFolder /DestinationPath"},
    {2, cmd + "download /SourceFileOrFolder /DestinationPath"},
    {2, cmd + "upload game.iso MyFolder /roms/ps2          several sources: the last path is the destination"},
    {2, cmd + "download /roms/ps2/game.iso ~/Downloads     folders download whole, subfolders included"},
    {2, cmd + "ls /path        mkdir /path/new        rm -r /path/old        mv /path/a /path/b        cancel"},
    {1, "Options: -n 8 connections, --chunk 2M, bench /path/test.bin measures speed (the test file is deleted after)."},
  };
  float y = 44;
  gfx::text("Help", M + 8, y, 30, ACCENT, gfx::BOLD);
  y += 56;
  for (auto& l : lines) {
    if (l.kind == 0) { gfx::text(l.s, M + 8, y, 22, TEXT, gfx::BOLD); y += 38; }
    else if (l.kind == 1) { gfx::text(gfx::ellipsize(l.s, gfx::UI_W - 2 * M - 30, 17), M + 24, y, 17, MUTED); y += 30; }
    else if (l.kind == 2) { gfx::text(gfx::ellipsize(l.s, gfx::UI_W - 2 * M - 30, 16, gfx::MONO), M + 24, y, 16, CODE, gfx::MONO); y += 28; }
    else y += 18;
  }
  const char* keys = "B / Y  Close";
  gfx::text(keys, gfx::UI_W - M - gfx::textWidth(keys, 18, gfx::BOLD), gfx::UI_H - 64, 18, TEXT, gfx::BOLD);
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
  { std::ifstream w("romfs:/web/index.html"); std::stringstream ss; ss << w.rdbuf(); g_webPage = ss.str(); }
  { std::ifstream w("romfs:/web/favicon.png", std::ios::binary); std::stringstream ss; ss << w.rdbuf(); g_favicon = ss.str(); }
  { std::ifstream w("romfs:/web/sw2xfer.py"); std::stringstream ss; ss << w.rdbuf(); g_clientScript = ss.str(); }
  vfs::init("");
  auth::init("sdmc:/switch/sw2transfer/settings.json");
  logf("password: %s", auth::enabled() ? "on" : "off");

  ServerConfig cfg;
  std::string err;
  bool serving = !sockMode.empty() && serverStart(cfg, makeHttpConn, err);
  if (!serving && err.empty()) err = "socket service unavailable";
  if (serving) logf("serving: HTTP %d", cfg.httpPort);
  else logf("cannot start: %s", err.c_str());

  // Stay awake while serving.
  appletSetAutoSleepDisabled(true);
  appletSetMediaPlaybackState(true);

  if (!gfx::init(err)) { logf("gfx: %s", err.c_str()); if (serving) serverStop(); socketExit(); romfsExit(); return 1; }
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad; padInitializeDefault(&pad);
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
  while (appletMainLoop()) {
    padUpdate(&pad);
    u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_Plus) break;
    if ((down & HidNpadButton_Minus) && !showHelp) {
      // Set, change or remove the password with the system keyboard.
      bool had = auth::enabled();
      std::string pw, again, kerr;
      int r = askPassword(had ? "Change the password" : "Set a password",
                          had ? "Leave blank to remove the password." : "Browsers and the command line will ask for it.", pw, kerr);
      if (r < 0) { toast = "Cannot set a password: " + kerr; toastBad = true; }
      else if (r > 0 && pw.empty()) {
        if (had) { bool ok = auth::setPassword(""); toast = ok ? "Password removed" : "Could not save the setting"; toastBad = !ok; if (ok) logf("password removed"); }
      } else if (r > 0) {
        int r2 = askPassword("Type the password again", "To make sure it is right.", again, kerr);
        if (r2 > 0 && again == pw) {
          bool ok = auth::setPassword(pw);
          toast = ok ? "Password set" : "Could not save the password"; toastBad = !ok;
          if (ok) logf("password set");
        } else if (r2 > 0) { toast = "The passwords did not match; nothing changed"; toastBad = true; }
      }
      if (!toast.empty()) toastUntil = nowSec() + 4;
    }
    if (down & HidNpadButton_Y) showHelp = !showHelp;
    if (down & HidNpadButton_B) { showHelp = false; cancelAsk = 0; }
    if ((down & HidNpadButton_X) && !showHelp) {
      if (cancelAsk > 0 && nowSec() - cancelAsk < 3.0) { cancelAll("Switch"); cancelAsk = 0; }
      else cancelAsk = nowSec();
    }
    double t = nowSec();
    auto active = transfersActive(8.0);
    if (t - lastSample >= 0.5) {
      for (auto& x : active) {
        int64_t b = x->bytes;
        double inst = (b - x->uiLastBytes) / (t - lastSample);
        x->uiLastBytes = b;
        x->uiSpeed = x->uiSpeed == 0 ? inst : x->uiSpeed * 0.5 + inst * 0.5;
      }
      int64_t u = g_bytesUp, d = g_bytesDown; double dt = t - lastSample;
      speedUp = (u - lastUp) / dt; speedDown = (d - lastDown) / dt;
      lastUp = u; lastDown = d; lastSample = t;
      // The graph only moves while something is transferring; when all is
      // done it stays put, showing the last transfer.
      bool live = speedUp > 0 || speedDown > 0;
      for (auto& x : active) if (x->conns > 0 && !x->copy) live = true;
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
    std::string status = !serving ? "Not serving: " + err : ip.empty() ? "No network connection" : "";
    if (!status.empty()) gfx::text(status, W - M - gfx::textWidth(status, 20, gfx::BOLD), 30, 20, ACCENT, gfx::BOLD);

    // How to connect: two cards. The addresses use the current IP.
    std::string host = ip.empty() ? "<switch-ip>" : ip;
    float y = 84;
    const float CW = W - 2 * M;
    {  // web browser
      const float h = 96;
      gfx::rect(M, y, CW, h, PANEL, 14, 1, LINE);
      gfx::rect(M, y, 6, h, ACCENT, 3);
      gfx::text("Transfer by web browser", M + 28, y + 14, 18, TEXT, gfx::BOLD);
      gfx::text("http://" + host + ":" + std::to_string(cfg.httpPort), M + 28, y + 44, 30, ACCENT, gfx::MONO);
      const char* hint = "Open on any computer or phone on the same network";
      gfx::text(hint, W - M - 28 - gfx::textWidth(hint, 16), y + 54, 16, MUTED);
      bool locked = auth::enabled();
      const char* pwText = locked ? "Password protected" : "No password: anyone on your network can open it (press \xE2\x80\x93 to set one)";
      gfx::text(pwText, W - M - 28 - gfx::textWidth(pwText, 16, gfx::BOLD), y + 16, 16, locked ? DOWN : MUTED, gfx::BOLD);
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
    snprintf(line, sizeof line, "%d connection%s", (int)g_connections, g_connections == 1 ? "" : "s");
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

    // Transfers
    y += gh + 20;
    float th = gfx::UI_H - y - 84;
    gfx::rect(M, y, W - 2 * M, th, PANEL, 14, 1, LINE);
    gfx::text("Transfers", M + 24, y + 14, 18, TEXT, gfx::BOLD);
    float ry = y + 50;
    if (active.empty()) gfx::text("Waiting for transfers. Each file can use many connections at once.", M + 24, ry, 18, MUTED);
    for (auto& x : active) {
      if (ry > y + th - 40) break;
      double dt = t - x->started;
      int64_t b = x->bytes, tot = x->total;
      bool cancelled = x->cancelled;
      bool live = x->conns > 0 && !cancelled;
      double sp = live ? x->uiSpeed : (dt > 0 ? b / std::max(0.001, x->lastActive - x->started) : 0);
      auto col = cancelled ? ACCENT : x->upload ? UP : DOWN;
      std::string name = gfx::ellipsize(std::string(x->copy ? "Copy to  " : x->upload ? "Upload  " : "Download  ") + x->path, W - 2 * M - 620, 18);
      gfx::text(name, M + 24, ry, 18, live ? TEXT : MUTED);
      snprintf(line, sizeof line, "%d conn  %s%s%s  %s", live ? (int)x->conns : (int)x->peakConns, human((double)b).c_str(), tot > 0 ? " / " : "", tot > 0 ? human((double)tot).c_str() : "", live ? mbs(sp).c_str() : cancelled ? "Cancelled" : "done");
      gfx::text(line, W - M - 24 - gfx::textWidth(line, 18, gfx::MONO), ry, 18, live || cancelled ? col : MUTED, gfx::MONO);
      float frac = tot > 0 ? std::min(1.0, (double)b / tot) : (live || cancelled ? 0 : 1);
      gfx::rect(M + 24, ry + 28, W - 2 * M - 48, 6, BARBG, 3);
      if (frac > 0) gfx::rect(M + 24, ry + 28, (W - 2 * M - 48) * frac, 6, col, 3);
      ry += 48;
    }

    // Log + footer
    auto logs = recentLog(2);
    float ly = gfx::UI_H - 76;
    for (auto& l : logs) { gfx::text(gfx::ellipsize(l, W - 2 * M, 15, gfx::MONO), M, ly, 15, MUTED, gfx::MONO); ly += 20; }
    if (cancelAsk > 0 && nowSec() - cancelAsk >= 3.0) cancelAsk = 0;
    if (toastUntil > 0 && nowSec() > toastUntil) { toastUntil = 0; toast.clear(); }
    if (!toast.empty()) {
      float aw = gfx::textWidth(toast, 20, gfx::BOLD) + 48;
      gfx::rect((W - aw) / 2, gfx::UI_H - 132, aw, 44, toastBad ? ACCENT : rgb(0x2f8f4e), 22);
      gfx::text(toast, (W - aw) / 2 + 24, gfx::UI_H - 122, 20, TEXT, gfx::BOLD);
    }
    if (cancelAsk > 0) {
      const char* ask = "Press X again to cancel all transfers";
      float aw = gfx::textWidth(ask, 20, gfx::BOLD) + 48;
      gfx::rect((W - aw) / 2, gfx::UI_H - 132, aw, 44, ACCENT, 22);
      gfx::text(ask, (W - aw) / 2 + 24, gfx::UI_H - 122, 20, TEXT, gfx::BOLD);
    }
    const char* keys = "\xE2\x80\x93  Password      X  Cancel all      Y  Help      +  Exit";
    gfx::text(keys, W - M - gfx::textWidth(keys, 18, gfx::BOLD), gfx::UI_H - 34, 18, TEXT, gfx::BOLD);
    snprintf(line, sizeof line, "This session: uploaded %s, downloaded %s   \xC2\xB7   Sockets: %s",
             human((double)g_bytesUp).c_str(), human((double)g_bytesDown).c_str(), sockMode.empty() ? "unavailable" : sockMode.c_str());
    gfx::text(line, M, gfx::UI_H - 34, 16, MUTED);
    if (showHelp) drawHelp(host, cfg.httpPort);
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
