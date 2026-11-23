#pragma once
// Wi-Fi (up to 10 saved networks) and over-the-air updates from GitHub Releases.
//
// The radio is only switched on for a job (scan, join, check, install) and is
// switched off again afterwards. Every job runs in its own FreeRTOS task; the UI
// reads the state with the getters below and never blocks.
//
// Release assets (see .github/workflows/release.yml):
//   version.json          {"version","app_size","app_sha256","sprites_size","sprites_sha256"}
//   tamapoke-app.bin      firmware for the free OTA slot
//   tamapoke-sprites.pak  complete TPAK sprite set for /mons on the microSD
#include <Arduino.h>
#include "netlogic.h"

enum NetScanStatus : uint8_t { SCAN_NONE, SCAN_BUSY, SCAN_DONE, SCAN_FAILED };
enum NetJoinStatus : uint8_t { JOIN_NONE, JOIN_BUSY, JOIN_OK, JOIN_WRONG_PASS, JOIN_NOT_FOUND, JOIN_FAILED };

enum OtaStatus : uint8_t {
  OTA_IDLE,
  OTA_CONNECTING,     // looking for a saved network
  OTA_CHECKING,       // reading version.json
  OTA_UP_TO_DATE,
  OTA_AVAILABLE,      // otaLatest(), otaFirmwareNewer(), otaSpritesNeeded()
  OTA_SPRITES,        // downloading the sprite pack, otaProgress() 0-100
  OTA_FIRMWARE,       // downloading the firmware, otaProgress() 0-100
  OTA_SPRITES_DONE,   // only the sprites changed: installed, no restart needed
  OTA_REBOOT,         // firmware installed: the main loop saves and restarts
  OTA_ERROR,          // otaError()
};

enum OtaError : uint8_t {
  OTA_ERR_NONE,
  OTA_ERR_NO_NETWORK,   // no saved network in range / no saved network at all
  OTA_ERR_DOWNLOAD,     // GitHub not reachable or download interrupted
  OTA_ERR_NO_RELEASE,   // no release published yet
  OTA_ERR_BAD_FILE,     // checksum / format error
  OTA_ERR_SD,           // microSD missing or write error
  OTA_ERR_SD_FULL,
  OTA_ERR_FLASH,        // firmware could not be written
};

struct NetScanItem {
  char ssid[NET_SSID_LEN + 1];
  int8_t rssi;
  bool open;
  bool saved;
};
#define NET_SCAN_MAX 24

// First thing in setup(): counts crash resets; after 3 in a row it rolls back to
// the previous firmware (and does not offer that version automatically again).
void otaBootGuard(const char *currentVersion);
void netBegin(const char *currentVersion);  // setup(): loads saved networks and settings
// loop(): schedules the automatic check, confirms a new firmware after it ran a while.
void netLoop(uint32_t now);
bool netBusy();         // a job is running (radio on): no light sleep
uint32_t netGen();      // changes whenever something visible changes (redraw)

bool netAutoCheck();    // check for updates at start-up and every 12 h
void netSetAutoCheck(bool on);

// Internet clock: synced at start-up and every hour when a saved network is in range.
uint8_t netTimeZone();                  // index into TZ_ZONES (TZ_OFF = clock set by hand)
void netSetTimeZone(uint8_t zone);
uint32_t netLastTimeSync();             // local epoch of the last sync, 0 = not yet
uint8_t netDstMode();                   // DstMode: summer time auto / always on / always off
void netSetDstMode(uint8_t mode);
bool netTakeTime(uint32_t *localEpoch); // once per sync: the main loop writes it to the RTC

const NetList &netSaved();
bool netForget(int idx);

bool netStartScan();
NetScanStatus netScanStatus();
int netScanCount();
const NetScanItem &netScanItem(int i);

bool netStartJoin(const char *ssid, const char *pass);  // tries the network, saves it on success
NetJoinStatus netJoinStatus();
const char *netJoinSsid();
void netClearJoin();

bool otaStartCheck(bool manual);
bool otaStartInstall();
OtaStatus otaStatus();
OtaError otaError();
uint8_t otaProgress();
const char *otaLatest();      // "" if unknown
bool otaFirmwareNewer();
bool otaSpritesNeeded();
bool otaUpdateAvailable();    // result of the last check
bool otaSdBusy();             // the update task writes to the microSD: do not load sprites
bool otaTakeNotice();         // once after an automatic check found an update
bool otaTakeSpritesChanged(); // once after new sprites were installed (reload)
