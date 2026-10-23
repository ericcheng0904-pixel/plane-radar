#include "util/setup_text.h"

#include <cstring>

namespace util::setup {

JoinFailure classifyDisconnect(uint8_t reason) {
  switch (reason) {
    case 0:
      return JoinFailure::kNone;
    case 2:    // AUTH_EXPIRE
    case 15:   // 4WAY_HANDSHAKE_TIMEOUT
    case 202:  // AUTH_FAIL
    case 204:  // HANDSHAKE_TIMEOUT
      return JoinFailure::kWrongPassword;
    case 201:  // NO_AP_FOUND
      return JoinFailure::kNotFound;
    default:
      return JoinFailure::kOther;
  }
}

namespace {

/** Append `c`, escaped for a WIFI: field. False when it does not fit. */
bool putQrChar(char c, char* out, size_t len, size_t* o) {
  const bool special = c == '\\' || c == ';' || c == ',' || c == ':' || c == '"';
  const size_t need = special ? 2 : 1;
  if (*o + need >= len) {
    return false;
  }
  if (special) {
    out[(*o)++] = '\\';
  }
  out[(*o)++] = c;
  return true;
}

bool putQrText(const char* text, char* out, size_t len, size_t* o) {
  for (const char* p = text; *p != '\0'; ++p) {
    if (!putQrChar(*p, out, len, o)) {
      return false;
    }
  }
  return true;
}

bool isHex(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

}  // namespace

bool wifiQrText(const char* ssid, const char* password, char* out, size_t len) {
  if (out == nullptr || len == 0) {
    return false;
  }
  out[0] = '\0';
  if (ssid == nullptr || password == nullptr) {
    return false;
  }
  size_t o = 0;
  const char* const kHead = "WIFI:T:WPA;S:";
  const size_t head = std::strlen(kHead);
  if (head >= len) {
    return false;
  }
  std::memcpy(out, kHead, head);
  o = head;
  bool ok = putQrText(ssid, out, len, &o);
  if (ok && o + 3 < len) {
    std::memcpy(out + o, ";P:", 3);
    o += 3;
  } else {
    ok = false;
  }
  ok = ok && putQrText(password, out, len, &o);
  if (!ok || o + 2 >= len) {
    out[0] = '\0';
    return false;
  }
  out[o++] = ';';
  out[o++] = ';';
  out[o] = '\0';
  return true;
}

bool plausibleSsid(const char* ssid) {
  if (ssid == nullptr) {
    return false;
  }
  const size_t n = strnlen(ssid, 33);
  return n >= 1 && n <= 32;
}

bool plausiblePassword(const char* password) {
  if (password == nullptr) {
    return false;
  }
  const size_t n = strnlen(password, 65);
  if (n == 0) {
    return true;
  }
  if (n == 64) {
    for (size_t i = 0; i < n; ++i) {
      if (!isHex(password[i])) {
        return false;
      }
    }
    return true;
  }
  return n >= 8 && n <= 63;
}

const char* htmlEntity(char c) {
  switch (c) {
    case '&':
      return "&amp;";
    case '<':
      return "&lt;";
    case '>':
      return "&gt;";
    case '"':
      return "&quot;";
    case '\'':
      return "&#39;";
    default:
      return nullptr;
  }
}

namespace {

/** htmlEscape(), and with `in_script` the extras of htmlEscapeInScript(). */
void escapeInto(const char* in, char* out, size_t len, bool in_script) {
  if (out == nullptr || len == 0) {
    return;
  }
  size_t o = 0;
  for (const char* p = in != nullptr ? in : ""; *p != '\0'; ++p) {
    if (in_script && static_cast<unsigned char>(*p) < 0x20) {
      continue;  // a line break would end the script's string mid-way
    }
    // In a script's string a backslash starts an escape ("\x" without hex
    // digits is a syntax error); as an entity it reaches the page as itself.
    const char* entity = in_script && *p == '\\' ? "&#92;" : htmlEntity(*p);
    const size_t need = entity != nullptr ? std::strlen(entity) : 1;
    if (o + need >= len) {
      break;
    }
    if (entity != nullptr) {
      std::memcpy(out + o, entity, need);
    } else {
      out[o] = *p;
    }
    o += need;
  }
  out[o] = '\0';
}

}  // namespace

void htmlEscape(const char* in, char* out, size_t len) { escapeInto(in, out, len, false); }

void htmlEscapeInScript(const char* in, char* out, size_t len) {
  escapeInto(in, out, len, true);
}

WelcomeEnd welcomeEnd(unsigned long shown_at_ms, unsigned long now_ms, bool tapped,
                      bool guide_opened, unsigned long show_ms) {
  if (tapped) {
    return WelcomeEnd::kTapped;
  }
  if (guide_opened) {
    return WelcomeEnd::kGuideOpened;
  }
  // Unsigned subtraction: right across millis() wrapping after ~49 days.
  return now_ms - shown_at_ms >= show_ms ? WelcomeEnd::kTimedOut : WelcomeEnd::kNotYet;
}

}  // namespace util::setup
