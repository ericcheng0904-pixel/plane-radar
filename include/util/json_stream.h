#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

/**
 * Reading a JSON text as it arrives, without building it: the walker of a
 * feed's answer (util::feed_answer) goes through the parts it does not keep
 * a byte at a time, and hands only the parts it keeps to deserializeJson(),
 * one at a time, so the memory they take does not grow with the answer.
 *
 * Nothing here recurses: a value nested however deep costs no stack, only a
 * TooDeep past kMaxDepth. Free of Arduino headers so the host tests cover it.
 */
namespace util::json_stream {

/** Where the text comes from: its next byte (0-255), or -1 once it has ended. */
class ByteSource {
 public:
  virtual int read() = 0;

 protected:
  ~ByteSource() = default;
};

/**
 * A ByteSource that can take one byte back, read in turn by the walker and
 * by deserializeJson() (it has the read() and readBytes() ArduinoJson asks of
 * an input; pass it by reference, so both read the same one).
 *
 * deserializeJson() given an object reads up to its closing brace and not a
 * byte further, so what follows is still here for the walker. Given a number
 * it reads the byte after it into its own latch, where it is lost (both shown
 * by the host tests): only objects are handed to it.
 */
class Input {
 public:
  explicit Input(ByteSource& source) : source_(&source) {}
  Input(const Input&) = delete;
  Input& operator=(const Input&) = delete;

  /** The next byte, or -1 at the end (the source is not asked again after that). */
  int read();
  size_t readBytes(char* out, size_t length);
  /** Take back `c`, the byte read last: the next read() returns it again. -1 is ignored. */
  void unread(int c) {
    if (c >= 0) {
      held_ = c;
    }
  }
  /**
   * The source has ended. An error found since may be the text cut short
   * (a number or a word missing its end), not a mistake in it.
   */
  bool ended() const { return ended_; }

 private:
  ByteSource* source_;
  int held_ = -1;
  bool ended_ = false;
};

/** Containers nested deeper than this inside a skipped value: TooDeep. */
constexpr uint8_t kMaxDepth = 32;

/** The next byte that is not JSON white space, or -1 at the end. */
int skipSpace(Input& in);

/**
 * Read past one value of any kind, white space before it included: an object
 * or array to its matching close, minding strings and their escapes; a
 * string to its closing quote; a number or a word up to the byte after it,
 * which is taken back. Lenient inside a container (only its brackets and
 * strings are checked), which is enough to find where it ends.
 * IncompleteInput when the text ends first; InvalidInput when there is no
 * value or its brackets do not match.
 */
DeserializationError::Code skipValue(Input& in);

/**
 * Read the rest of a string whose opening quote was read, escapes decoded,
 * into `out` (`size` bytes; terminated when `size` > 0). `*fits` is false
 * when it is longer than that or holds a character beyond ASCII (or a NUL):
 * it is read to its end all the same, so a long key costs no memory.
 */
DeserializationError::Code readString(Input& in, char* out, size_t size, bool* fits);

/**
 * Read a number whose first byte is next ("-12.5e3"), however long, without
 * a buffer, to 19 significant digits (one beyond a double's range is
 * infinite); the byte after it is taken back. InvalidInput for anything else;
 * IncompleteInput when the text ends inside it or right after it, as a byte
 * must follow a member's value to show it is whole.
 */
DeserializationError::Code readNumber(Input& in, double* value);

}  // namespace util::json_stream
