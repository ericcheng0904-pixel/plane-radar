#include "util/json_stream.h"

namespace util::json_stream {

namespace {

using Code = DeserializationError::Code;

bool isSpace(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

bool isDigit(int c) { return c >= '0' && c <= '9'; }

/** What a number or a word (true, false, null) is made of. */
bool isWordByte(int c) {
  return isDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '+' ||
         c == '-' || c == '.';
}

int hexValue(int c) {
  if (isDigit(c)) {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/** A number or a word whose first byte `c` was read; the byte after it is taken back. */
Code skipWord(Input& in, int c) {
  size_t length = 0;
  while (c >= 0 && isWordByte(c)) {
    ++length;
    c = in.read();
  }
  in.unread(c);
  if (length == 0) {
    return c < 0 ? Code::IncompleteInput : Code::InvalidInput;
  }
  return Code::Ok;
}

/** Significant digits kept of a number: more than a double holds. */
constexpr int kMaxDigits = 19;
/** Powers of ten past this make any number kept infinite or zero. */
constexpr int kMaxScale = 400;
/**
 * Places the point is moved and exponents are counted up to: far past
 * kMaxScale, so "1<500 digits>e-450" is still right, and an int holds both.
 */
constexpr int kMaxCount = 1000000;

/** A number as read so far: `digits` times 10 to the `scale`. */
struct Decimal {
  double digits = 0.0;
  int kept = 0;
  int scale = 0;
};

/**
 * Digits from `c` on, into `*number`; `fraction` when they follow the point.
 * Past kMaxDigits a digit only moves the point (or, in the fraction, is
 * dropped), so a number of any length stays finite unless it is that big.
 * `*count` is how many; the first byte after them is returned.
 */
int readDigits(Input& in, int c, bool fraction, Decimal* number, size_t* count) {
  *count = 0;
  while (isDigit(c)) {
    if (number->kept < kMaxDigits) {
      number->digits = number->digits * 10.0 + (c - '0');
      if (number->digits > 0.0) {
        ++number->kept;  // leading zeros are not significant
      }
      if (fraction && number->scale > -kMaxCount) {
        --number->scale;
      }
    } else if (!fraction && number->scale < kMaxCount) {
      ++number->scale;
    }
    ++*count;
    c = in.read();
  }
  return c;
}

}  // namespace

int Input::read() {
  if (held_ >= 0) {
    const int c = held_;
    held_ = -1;
    return c;
  }
  if (ended_) {
    return -1;
  }
  const int c = source_->read();
  if (c < 0) {
    ended_ = true;
    return -1;
  }
  return c;
}

size_t Input::readBytes(char* out, size_t length) {
  size_t n = 0;
  while (n < length) {
    const int c = read();
    if (c < 0) {
      break;
    }
    out[n++] = static_cast<char>(c);
  }
  return n;
}

int skipSpace(Input& in) {
  int c = in.read();
  while (isSpace(c)) {
    c = in.read();
  }
  return c;
}

Code skipValue(Input& in) {
  int c = skipSpace(in);
  if (c == '"') {
    bool fits = false;
    return readString(in, nullptr, 0, &fits);
  }
  if (c != '{' && c != '[') {
    return skipWord(in, c);
  }
  // One bit per open container, set for an object: a close must match it.
  // Commas, colons and words inside are passed over unchecked.
  uint32_t objects = 0;
  uint8_t depth = 0;
  for (;;) {
    if (c < 0) {
      return Code::IncompleteInput;
    }
    if (c == '{' || c == '[') {
      if (depth == kMaxDepth) {
        return Code::TooDeep;
      }
      const uint32_t bit = 1u << depth;
      objects = c == '{' ? objects | bit : objects & ~bit;
      ++depth;
    } else if (c == '}' || c == ']') {
      // depth > 0: the value began with an opening bracket and ends with its close.
      --depth;
      const bool object = (objects >> depth) & 1u;
      if (object != (c == '}')) {
        return Code::InvalidInput;
      }
      if (depth == 0) {
        return Code::Ok;
      }
    } else if (c == '"') {
      bool fits = false;
      const Code err = readString(in, nullptr, 0, &fits);
      if (err != Code::Ok) {
        return err;
      }
    }
    c = in.read();
  }
}

Code readString(Input& in, char* out, size_t size, bool* fits) {
  size_t length = 0;
  *fits = size > 0;
  for (;;) {
    int c = in.read();
    if (c < 0) {
      return Code::IncompleteInput;
    }
    if (c == '"') {
      break;
    }
    if (c == '\\') {
      c = in.read();
      switch (c) {
        case -1:
          return Code::IncompleteInput;
        case '"':
        case '\\':
        case '/':
          break;
        case 'b':
          c = '\b';
          break;
        case 'f':
          c = '\f';
          break;
        case 'n':
          c = '\n';
          break;
        case 'r':
          c = '\r';
          break;
        case 't':
          c = '\t';
          break;
        case 'u': {
          int code = 0;
          for (int i = 0; i < 4; ++i) {
            const int h = in.read();
            if (h < 0) {
              return Code::IncompleteInput;
            }
            if (hexValue(h) < 0) {
              return Code::InvalidInput;
            }
            code = code * 16 + hexValue(h);
          }
          c = code;
          break;
        }
        default:
          return Code::InvalidInput;
      }
    }
    // Beyond ASCII or a NUL: never one of the keys looked for.
    if (c == 0 || c >= 0x80 || length + 1 >= size) {
      *fits = false;
      continue;
    }
    out[length++] = static_cast<char>(c);
  }
  if (size > 0) {
    out[length] = '\0';
  }
  return Code::Ok;
}

Code readNumber(Input& in, double* value) {
  // Read as it comes, so no buffer limits its length; the radar only needs
  // the clock's whole seconds out of it.
  int c = in.read();
  const bool negative = c == '-';
  if (negative) {
    c = in.read();
  }
  Decimal number;
  size_t digits = 0;
  c = readDigits(in, c, false, &number, &digits);
  bool ok = digits > 0;
  if (ok && c == '.') {
    c = readDigits(in, in.read(), true, &number, &digits);
    ok = digits > 0;
  }
  int scale = number.scale;
  if (ok && (c == 'e' || c == 'E')) {
    c = in.read();
    const bool down = c == '-';
    if (c == '-' || c == '+') {
      c = in.read();
    }
    int exponent = 0;
    digits = 0;
    while (isDigit(c)) {
      if (exponent < kMaxCount) {
        exponent = exponent * 10 + (c - '0');
      }
      ++digits;
      c = in.read();
    }
    ok = digits > 0;
    scale += down ? -exponent : exponent;
  }
  in.unread(c);
  // Ended by the end of the text, it may be missing digits: a clock in ms
  // cut by one would read as one in seconds.
  if (!ok || c < 0 || isWordByte(c)) {
    return c < 0 ? Code::IncompleteInput : Code::InvalidInput;
  }
  double v = number.digits;
  for (int i = 0; i < scale && i < kMaxScale; ++i) {
    v *= 10.0;
  }
  for (int i = 0; i > scale && i > -kMaxScale; --i) {
    v /= 10.0;
  }
  *value = negative ? -v : v;
  return Code::Ok;
}

}  // namespace util::json_stream
