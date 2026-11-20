// Host build of the SW2Transfer servers for testing on the Mac:
//   tests/host_build.sh && build-host/sw2transfer-host <root dir> [httpPort]
#include "../source/net.h"
#include "../source/auth.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <thread>

std::string g_webPage, g_clientScript, g_favicon;
const char* g_appVersion = "host";
Conn* makeHttpConn(int fd);
Conn* makeFtpConn(int fd);

int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <root> [httpPort [ftpPort]]\n", argv[0]); return 1; }
  signal(SIGPIPE, SIG_IGN);
  vfs::init(argv[1]);
  std::ifstream w("romfs/web/index.html"); std::stringstream ss; ss << w.rdbuf(); g_webPage = ss.str();
  { std::ifstream c("romfs/web/favicon.png", std::ios::binary); std::stringstream cs; cs << c.rdbuf(); g_favicon = cs.str(); }
  { std::ifstream c("tools/sw2xfer.py"); std::stringstream cs; cs << c.rdbuf(); g_clientScript = cs.str(); }
  // Password settings next to the served folder; SW2T_PASSWORD sets one at start.
  auth::init(std::string(argv[1]) + "/../sw2t-settings.json");
  if (const char* pw = getenv("SW2T_PASSWORD")) auth::setPassword(pw);
  ServerConfig cfg;
  if (argc > 2) cfg.httpPort = atoi(argv[2]);
  if (argc > 3) cfg.ftpPort = atoi(argv[3]);
  std::string err;
  if (!serverStart(cfg, makeHttpConn, err, makeFtpConn)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }
  logf("serving %s: HTTP %d, FTP %d", argv[1], cfg.httpPort, g_ftpPort.load());
  for (;;) { std::this_thread::sleep_for(std::chrono::seconds(1)); vfs::reap(); transfersActive(5); }
}
