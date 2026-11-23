#pragma once
// GNSS of the -G board (Quectel LC76G on the shared I2C bus, 0x50 write / 0x54 read).
// Everything runs from the main loop in small steps (no delays), so the bus is
// never used at the same time as the touch, RTC or PMU.
#include <Arduino.h>
#include "gpslogic.h"

void gpsBegin();                        // setup(), after Wire.begin(): releases the module reset, probes it
bool gpsPresent();                      // false on boards without the GNSS module
void gpsPoll(uint32_t now, bool wanted); // loop(): reads NMEA about once a second while wanted
const GpsInfo &gpsInfo();
uint32_t gpsGen();                      // changes whenever new data arrived (redraw)
bool gpsBootWindow(uint32_t now);       // first minutes after start-up: log the position on the serial port
