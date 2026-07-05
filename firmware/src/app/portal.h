// Captive-portal WiFi + SignalK onboarding — see portal.cpp.
#pragma once
#include <Arduino.h>

void portalStart();            // scan, start open AP + DNS + HTTP portal
void portalStop();             // tear down AP/servers, back to STA mode
void portalLoop();             // service DNS + HTTP (call every loop while active)
bool portalSaved();            // true once credentials were saved (reboot pending)
int  portalClients();          // phones currently joined to our AP
const char *portalApName();    // "Gauge-XXXX" (from MAC)
