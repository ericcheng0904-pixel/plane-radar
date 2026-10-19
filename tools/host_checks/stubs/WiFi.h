#pragma once
// Host stub: the Wi-Fi page names the network and its signal strength.
#include <Arduino.h>

struct HostWiFi {
  String SSID() { return String("Test network"); }
  int RSSI() { return -64; }
};
extern HostWiFi WiFi;
