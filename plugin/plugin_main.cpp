// SW2Transfer as a kaiHEN plugin: the same HTTP server as the app, with no
// screen, started by the kaiHEN bootstrap at boot from
// /kaihen/plugins/sw2transfer.nso so it is always available, also while a game
// runs. Built by Makefile.plugin with smaller buffers than the app (see there).
//
// Not released yet: an experiment. Without a screen, the web page, the
// command line and the log file (sdmc:/switch/sw2transfer/plugin-log.txt) are
// the only way to see what it does.
#include "net.h"
#include "ops.h"
#include "auth.h"
#include <switch.h>
#include <kaihen/plugin.h>
#include <sys/stat.h>
#include <stdio.h>
#include <time.h>
#include <string>

std::string g_webPage, g_clientScript, g_favicon;
#include "buildid.h"
const char* g_appVersion = APP_VERSION;
// "1.4.3 (build 9dcfe17)": tells builds of the same version apart (app and plugin).
const char* g_buildLabel = APP_VERSION " (build " BUILD_ID ")";
Conn* makeHttpConn(int fd);
Conn* makeFtpConn(int fd);

// Embedded copies of romfs/web (the plugin has no romfs); see tools/embed_assets.py.
extern const unsigned char asset_index_html[], asset_favicon_png[], asset_sw2xfer_py[];
extern const size_t asset_index_html_size, asset_favicon_png_size, asset_sw2xfer_py_size;

// ---------------------------------------------------------------- process setup
// A plugin has no services before pluginRuntimeInitialize() and replaces the
// application setup (see <kaihen/plugin.h>).
extern "C" {
extern char* fake_heap_start;
void __libnx_init_time(void);  // in libounce, not in its headers: sets up time() after timeInitialize()
extern char* fake_heap_end;
u32 __nx_applet_type = AppletType_None;
size_t __nx_heap_size = 0;  // chosen in __libnx_initheap below
u32 __nx_fs_num_sessions = 4;
void __appInit(void) {}
void __appExit(void) {}

static size_t g_heapSize = 0;
// libnx stops the program with an error when the heap it asks for is not
// available. At boot that would be a fatal error in a background process, so
// ask for less instead, and the server sizes itself to what it got.
void __libnx_initheap(void) {
  static const size_t tries[] = {64u << 20, 48u << 20, 32u << 20, 24u << 20, 16u << 20};
  for (size_t sz : tries) {
    void* addr = nullptr;
    if (R_SUCCEEDED(svcSetHeapSize(&addr, sz))) {
      fake_heap_start = (char*)addr;
      fake_heap_end = (char*)addr + sz;
      g_heapSize = sz;
      return;
    }
  }
  svcExitProcess();  // no memory at all: leave quietly rather than crash the boot
}
}

// ---------------------------------------------------------------- sockets
// Half the app's pool (1 MB buffers, efficiency 32): the pool comes out of the
// plugin's heap. On the console the plugin process may use 210 MB.
// Efficiency 16 leaves room for MAX_CONNECTIONS (12) sockets.
static std::string initSockets() {
  struct Tier { u32 buf, max, eff; const char* name; };
  static const Tier tiers[] = {
    {0x40000, 0x100000, 16, "buffers up to 1 MB"},   // pool 32 MB
    {0x20000, 0x80000, 16, "buffers up to 512 KB"},  // pool 16 MB
    {0x10000, 0x40000, 8, "buffers up to 256 KB"},   // pool 4 MB
    {0x10000, 0, 4, "64 KB buffers"},                // as kaiHEN's ftpd plugin
  };
  for (const Tier& t : tiers) {
    if (t.eff * 2 * (t.max ? t.max : t.buf) > g_heapSize / 2) continue;  // leave half for the rest
    SocketInitConfig c = *socketGetDefaultInitConfig();
    c.tcp_tx_buf_size = t.buf; c.tcp_rx_buf_size = t.buf;
    c.tcp_tx_buf_max_size = t.max; c.tcp_rx_buf_max_size = t.max;
    c.udp_tx_buf_size = 0; c.udp_rx_buf_size = 0;
    c.sb_efficiency = t.eff;
    c.num_bsd_sessions = 6;  // network threads + spare
    c.bsd_service_type = BsdServiceType_Auto;
    if (R_SUCCEEDED(socketInitialize(&c))) return t.name;
  }
  return "";
}

static void sleepMs(int ms) { svcSleepThread((u64)ms * 1000000ULL); }

static time_t settingsTime(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_mtime : 0;
}

int main(int, char**) {
  Handle ready;
  if (R_FAILED(pluginRuntimeInitialize(&ready))) return 1;

  FsFileSystem sd;
  if (R_FAILED(fsInitialize()) || R_FAILED(fsOpenSdCardFileSystem(&sd))) {
    svcSignalEvent(ready);
    pluginRuntimeExit();
    return 1;
  }
  fsdevMountDevice("sdmc", sd);
  if (R_SUCCEEDED(timeInitialize())) __libnx_init_time();  // for sign-in expiry

  static const char* SETTINGS = "sdmc:/switch/sw2transfer/settings.json";
  mkdir("sdmc:/switch", 0777);
  mkdir("sdmc:/switch/sw2transfer", 0777);
  rotateLog("sdmc:/switch/sw2transfer/plugin-log.txt", "sdmc:/switch/sw2transfer/plugin-log-previous.txt");  // (the last run's, kept)
  setLogFile("sdmc:/switch/sw2transfer/plugin-log.txt");
  logf("SW2Transfer %s background plugin, heap %u MB", APP_VERSION, (unsigned)(g_heapSize >> 20));

  g_webPage.assign((const char*)asset_index_html, asset_index_html_size);
  g_favicon.assign((const char*)asset_favicon_png, asset_favicon_png_size);
  g_clientScript.assign((const char*)asset_sw2xfer_py, asset_sw2xfer_py_size);
  vfs::init("");
  auth::init(SETTINGS);
  time_t settingsSeen = settingsTime(SETTINGS);
  logf("password: %s", auth::enabled() ? "on" : "off");

  std::string sockMode = initSockets();
  logf("sockets: %s", sockMode.empty() ? "FAILED" : sockMode.c_str());
  g_sockMode = sockMode + ", heap " + std::to_string(g_heapSize >> 20) + " MB";
  {
    u64 mask = 0; svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    logf("cores allowed 0x%llx, memory used %llu of %llu KB, clock %lld", (unsigned long long)mask,
         (unsigned long long)(used >> 10), (unsigned long long)(total >> 10), (long long)time(nullptr));
  }
  // Ready as soon as our own setup is done; the network may come up later.
  svcSignalEvent(ready);
  logf("ready signalled");
  if (sockMode.empty()) { pluginRuntimeExit(); return 1; }

  ServerConfig cfg;
  cfg.netThreads = 2;
  cfg.diskThreads = 4;
  bool serving = false;
  std::string lastErr;
  int tick = 0;
  for (;;) {
    if (!serving) {
      std::string err;
      serving = serverStart(cfg, makeHttpConn, err, makeFtpConn);
      if (serving) { logf("serving: HTTP %d", cfg.httpPort); lastErr.clear(); }
      else {
        if (err != lastErr) logf("cannot start yet: %s", err.c_str());  // e.g. no network yet, or the app has the port
        lastErr = err;
        sleepMs(2000);
        continue;
      }
    }
    sleepMs(250);
    vfs::reap();
    if (++tick % 4 == 0) transfersActive(8.0);  // forgets finished transfers
    if (tick % 20 == 0) {  // the app may have changed the password
      time_t t = settingsTime(SETTINGS);
      if (t != settingsSeen) { settingsSeen = t; auth::init(SETTINGS); logf("password: %s", auth::enabled() ? "on" : "off"); }
    }
    if (g_stopRequested) break;  // the app's "Stop until restart"
    if (g_listenerBroken) {  // e.g. after sleep: start again with a new socket
      logf("listening socket failed; restarting");
      serverStop();
      g_listenerBroken = false;
      serving = false;
    }
  }
  // Stopped from the app: give the answer to /api/stop time to leave, then end
  // the process. kaiHEN starts it again at the next restart.
  // Transfers in progress are cancelled (unfinished uploads removed) while
  // the server still runs, so their connections see it and close.
  sleepMs(300);
  cancelAll("stop");
  sleepMs(1500);
  serverStop();
  opsStop();  // waits for the clean-up of unfinished uploads
  logf("stopped until the console restarts");
  socketExit();
  pluginRuntimeExit();
  return 0;
}
