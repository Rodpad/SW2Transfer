#include "auth.h"
#include <mutex>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#else
#include <stdlib.h>
#endif

namespace auth {

// ------------------------------------------------------------------ SHA-256
namespace {
struct Sha256 {
  uint32_t h[8]; uint64_t len = 0; uint8_t buf[64]; size_t n = 0;
  Sha256() { static const uint32_t i[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}; memcpy(h, i, sizeof h); }
  static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
  void block(const uint8_t* p) {
    static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
      0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
      0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
      uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  void update(const void* data, size_t l) {
    const uint8_t* p = (const uint8_t*)data; len += l;
    while (l--) { buf[n++] = *p++; if (n == 64) { block(buf); n = 0; } }
  }
  void final(uint8_t out[32]) {
    uint64_t bits = len * 8;
    uint8_t pad = 0x80; update(&pad, 1);
    uint8_t z = 0; while (n != 56) update(&z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (i * 8)); update(&b, 1); }
    for (int i = 0; i < 8; i++) { out[i * 4] = h[i] >> 24; out[i * 4 + 1] = h[i] >> 16; out[i * 4 + 2] = h[i] >> 8; out[i * 4 + 3] = h[i]; }
  }
};

std::string hmac(const std::string& key, const std::string& msg) {
  uint8_t k[64] = {0};
  if (key.size() > 64) { Sha256 s; s.update(key.data(), key.size()); s.final(k); }
  else memcpy(k, key.data(), key.size());
  uint8_t ip[64], op[64];
  for (int i = 0; i < 64; i++) { ip[i] = k[i] ^ 0x36; op[i] = k[i] ^ 0x5c; }
  uint8_t inner[32], out[32];
  Sha256 a; a.update(ip, 64); a.update(msg.data(), msg.size()); a.final(inner);
  Sha256 b; b.update(op, 64); b.update(inner, 32); b.final(out);
  return std::string((const char*)out, 32);
}

// PBKDF2-HMAC-SHA256, one 32-byte block.
std::string pbkdf2(const std::string& pw, const std::string& salt, int iterations) {
  std::string u = hmac(pw, salt + std::string("\0\0\0\1", 4)), t = u;
  for (int i = 1; i < iterations; i++) { u = hmac(pw, u); for (int j = 0; j < 32; j++) t[j] ^= u[j]; }
  return t;
}

std::string hex(const std::string& s) {
  static const char* d = "0123456789abcdef"; std::string o;
  for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}
std::string unhex(const std::string& s) {
  std::string o;
  for (size_t i = 0; i + 1 < s.size(); i += 2) o += (char)strtol(s.substr(i, 2).c_str(), nullptr, 16);
  return o;
}
bool sameBytes(const std::string& a, const std::string& b) {  // constant time
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}
std::string randomBytes(size_t n) {
  std::string s(n, '\0');
#ifdef __SWITCH__
  randomGet(&s[0], n);
#else
  arc4random_buf(&s[0], n);
#endif
  return s;
}

const int ITERATIONS = 20000;
std::mutex mu;
std::string path, salt, hash, secret;  // raw bytes; hash empty = no password
int failures = 0; time_t lockedUntil = 0;

// settings.json: {"salt":"..","hash":"..","secret":".."} (hex). Tiny parser:
// only our own file is read.
std::string field(const std::string& j, const char* name) {
  std::string key = std::string("\"") + name + "\":\"";
  size_t a = j.find(key);
  if (a == std::string::npos) return "";
  a += key.size();
  size_t b = j.find('"', a);
  return b == std::string::npos ? "" : j.substr(a, b - a);
}
bool save() {
  std::string tmp = path + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return false;
  fprintf(f, "{\"salt\":\"%s\",\"hash\":\"%s\",\"secret\":\"%s\"}\n", hex(salt).c_str(), hex(hash).c_str(), hex(secret).c_str());
  bool ok = fclose(f) == 0;
  if (ok) { remove(path.c_str()); ok = rename(tmp.c_str(), path.c_str()) == 0; }
  return ok;
}
}  // namespace

std::string sha256Hex(const std::string& data) { Sha256 s; s.update(data.data(), data.size()); uint8_t o[32]; s.final(o); return hex(std::string((char*)o, 32)); }

std::string pbkdf2Hex(const std::string& pw, const std::string& s, int it) { return hex(pbkdf2(pw, s, it)); }

void init(const std::string& settingsPath) {
  std::lock_guard<std::mutex> lk(mu);
  path = settingsPath;
  FILE* f = fopen(path.c_str(), "r");
  if (f) {
    std::string j; char b[512]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) j.append(b, n);
    fclose(f);
    salt = unhex(field(j, "salt")); hash = unhex(field(j, "hash")); secret = unhex(field(j, "secret"));
    if (hash.size() != 32 || salt.empty()) hash.clear();
  }
  if (secret.size() != 32) secret = randomBytes(32);
}

bool enabled() { std::lock_guard<std::mutex> lk(mu); return !hash.empty(); }

bool setPassword(const std::string& pw) {
  std::string s = randomBytes(16), h = pw.empty() ? "" : pbkdf2(pw, s, ITERATIONS);
  std::lock_guard<std::mutex> lk(mu);
  salt = s; hash = h;
  secret = randomBytes(32);  // signs everyone out
  failures = 0; lockedUntil = 0;
  return save();
}

bool checkPassword(const std::string& pw) {
  std::string s, h;
  { std::lock_guard<std::mutex> lk(mu); s = salt; h = hash; }
  if (h.empty()) return true;
  return sameBytes(pbkdf2(pw, s, ITERATIONS), h);
}

std::string makeToken(int64_t validSeconds) {
  std::lock_guard<std::mutex> lk(mu);
  std::string exp = std::to_string((long long)time(nullptr) + validSeconds);
  return exp + "." + hex(hmac(secret, exp));
}

bool checkToken(const std::string& token) {
  size_t dot = token.find('.');
  if (dot == std::string::npos) return false;
  std::string exp = token.substr(0, dot), sig = token.substr(dot + 1);
  if (exp.empty() || exp.find_first_not_of("0123456789") != std::string::npos) return false;
  if ((long long)time(nullptr) > atoll(exp.c_str())) return false;
  std::lock_guard<std::mutex> lk(mu);
  return sameBytes(hex(hmac(secret, exp)), sig);
}

bool loginLocked(int* secondsLeft) {
  std::lock_guard<std::mutex> lk(mu);
  time_t now = time(nullptr);
  if (lockedUntil > now) { if (secondsLeft) *secondsLeft = (int)(lockedUntil - now); return true; }
  return false;
}
void loginResult(bool ok) {
  std::lock_guard<std::mutex> lk(mu);
  if (ok) { failures = 0; return; }
  if (++failures >= 5) { failures = 0; lockedUntil = time(nullptr) + 30; }
}

}  // namespace auth
