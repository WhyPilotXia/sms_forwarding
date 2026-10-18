#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "globals.h"

#define WIFI_SETUP_AP_SSID "SMS-Forward-Setup"
// 配置热点密码可在本地 wifi_config.h 中定义 WIFI_SETUP_PASSWORD 覆盖。
#ifndef WIFI_SETUP_PASSWORD
#define WIFI_SETUP_PASSWORD "smsforward"
#endif

extern bool wifiApMode;

bool connectConfiguredWifi(uint8_t attempts = 3, unsigned long timeoutMs = 12000);
void startWifiSetupAp();
void maintainWifiConnection();
String getActiveWifiAddress();

#endif
