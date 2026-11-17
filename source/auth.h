// SW2Transfer password protection.
//
// The password itself is never stored: settings.json keeps a random salt and
// PBKDF2-HMAC-SHA256(password, salt). A correct login returns a token
// "<expiry>.<hmac>" signed with a random secret that is also in
// settings.json, so tokens survive an app restart and all of them stop
// working when the password is changed or removed.
#pragma once
#include <stdint.h>
#include <string>

namespace auth {

void init(const std::string& settingsPath);
bool enabled();
// Empty password turns protection off. Returns false if it could not be saved.
bool setPassword(const std::string& pw);
bool checkPassword(const std::string& pw);
std::string makeToken(int64_t validSeconds);
bool checkToken(const std::string& token);

// Login throttle: after 5 wrong passwords in a row, refuse for 30 s.
bool loginLocked(int* secondsLeft = nullptr);
void loginResult(bool ok);

const int64_t TOKEN_SECONDS = 12 * 3600;

// Exposed for tests.
std::string sha256Hex(const std::string& data);
std::string pbkdf2Hex(const std::string& pw, const std::string& salt, int iterations);

}  // namespace auth
