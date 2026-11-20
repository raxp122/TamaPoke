#pragma once
#include <string>

struct SimConfig {
  std::string sdRoot = "tools/sdcard";
  bool verbose = false;
  int battery = 80;
  bool pwrPressed = false;
  int otaStatus = 0;  // OtaStatus shown by the fake Wi-Fi/OTA module
};
extern SimConfig gSim;
