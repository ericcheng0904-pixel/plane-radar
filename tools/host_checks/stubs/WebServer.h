#pragma once
// Host stub of the ESP32 WebServer. Handlers are kept so a test can call a
// route directly, and every chunk written by sendContent is appended to `body`
// exactly as the real server would write it to the socket.
// `chunks` counts the sendContent calls: on the radar each is three socket
// writes and three TCP segments, which is what made pages slow.
// client() returns a copy of the connection, as ESP32's WebServer does, so
// stop() on it does nothing to the connection itself.
#include <Arduino.h>

#include <functional>
#include <map>
#include <string>

#define CONTENT_LENGTH_UNKNOWN (static_cast<size_t>(-1))

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };

struct HostClient {
  /** The socket under the connection; a test may give it a real one. */
  int socket = -1;
  bool connected() { return true; }
  /** Like WiFiClient::stop(): lets go of this copy's socket, nothing more. */
  void stop() { socket = -1; }
  int fd() const { return socket; }
};

class WebServer {
 public:
  using THandlerFunction = std::function<void(void)>;

  std::map<std::string, THandlerFunction> routes;
  std::map<std::string, std::string> args;
  std::map<std::string, std::string> headers;
  std::string body;
  size_t chunks = 0;
  int status = 0;
  /** How long each sendContent takes: a client that stopped reading. */
  unsigned long ms_per_chunk = 0;
  /** The connection the server writes to; client() hands out copies. */
  HostClient connection;

  void on(const char* uri, HTTPMethod method, THandlerFunction handler) {
    (void)method;
    routes[uri] = handler;
  }
  void sendHeader(const String& name, const String& value) {
    headers[name.c_str()] = value.c_str();
  }
  void setContentLength(size_t length) { (void)length; }
  void send(int code, const char* type, const String& content) {
    (void)type;
    status = code;
    body += content.c_str();
  }
  void sendContent(const String& content) {
    ++chunks;
    body += content.c_str();
    g_host_millis += ms_per_chunk;
  }
  void sendContent(const char* data, size_t length) {
    ++chunks;
    body.append(data, length);
    g_host_millis += ms_per_chunk;
  }
  bool hasArg(const char* name) const { return args.count(name) != 0; }
  String arg(const char* name) const {
    const auto it = args.find(name);
    return String(it == args.end() ? "" : it->second.c_str());
  }
  HostClient client() { return connection; }
};
