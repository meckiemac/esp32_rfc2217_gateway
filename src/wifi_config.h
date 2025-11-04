#pragma once

// Copy this file to wifi_config.h and fill in your network credentials.

#define SER2NET_WIFI_SSID     "DevicesPSK"
#define SER2NET_WIFI_PASSWORD "ef78ff28a0ec3ca074b5c10638761f44"

#ifndef SER2NET_AP_SSID
#define SER2NET_AP_SSID "ser2net-setup"
#endif

#ifndef SER2NET_AP_PASSWORD
#define SER2NET_AP_PASSWORD "ser2net-setup"
#endif

#ifndef SER2NET_AP_CHANNEL
#define SER2NET_AP_CHANNEL 6
#endif

#ifndef SER2NET_AP_MAX_CLIENTS
#define SER2NET_AP_MAX_CLIENTS 4
#endif

#ifndef SER2NET_AP_ACTIVE_TIMEOUT_SEC
#define SER2NET_AP_ACTIVE_TIMEOUT_SEC 300
#endif
