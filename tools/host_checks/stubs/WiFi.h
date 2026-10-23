#pragma once
// Host stub: the Wi-Fi page names the network and its signal strength, and
// the guide the radar's address.
#include <Arduino.h>

struct HostIpAddress {
  String toString() const { return String("192.168.1.23"); }
};

struct HostWiFi {
  String SSID() { return String("Test network"); }
  int RSSI() { return -64; }
  HostIpAddress localIP() { return HostIpAddress{}; }
};
extern HostWiFi WiFi;
