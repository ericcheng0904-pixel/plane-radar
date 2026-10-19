#pragma once
// Host stub of the ESP32 WebServer. Handlers are kept so a test can call a
// route directly, and every chunk written by sendContent is appended to `body`
// exactly as the real server would write it to the socket.
#include <Arduino.h>

#include <functional>
#include <map>
#include <string>

#define CONTENT_LENGTH_UNKNOWN (static_cast<size_t>(-1))

enum HTTPMethod { HTTP_ANY, HTTP_GET, HTTP_POST };

struct HostClient {
  bool connected() { return true; }
  void stop() {}
};

class WebServer {
 public:
  using THandlerFunction = std::function<void(void)>;

  std::map<std::string, THandlerFunction> routes;
  std::map<std::string, std::string> args;
  std::map<std::string, std::string> headers;
  std::string body;
  int status = 0;

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
  void sendContent(const String& content) { body += content.c_str(); }
  void sendContent(const char* data, size_t length) { body.append(data, length); }
  bool hasArg(const char* name) const { return args.count(name) != 0; }
  String arg(const char* name) const {
    const auto it = args.find(name);
    return String(it == args.end() ? "" : it->second.c_str());
  }
  HostClient& client() { return client_; }

 private:
  HostClient client_;
};
