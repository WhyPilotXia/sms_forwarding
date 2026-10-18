#include "wifi_manager.h"
#include "web_handlers.h"
#include "modem.h"
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#endif

bool wifiApMode = false;
static unsigned long lastReconnectAttempt = 0;
static uint8_t backgroundFailureCount = 0;

bool connectConfiguredWifi(uint8_t attempts, unsigned long timeoutMs) {
  if (config.wifiSsid.length() == 0) return false;

  WiFi.mode(wifiApMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

  for (uint8_t attempt = 1; attempt <= attempts; attempt++) {
    logCaptureF("连接 WiFi %s（第 %u/%u 次）...\n",
                config.wifiSsid.c_str(), attempt, attempts);
    WiFi.disconnect(false, false);
    delay(200);
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
      delay(100);
      blink_short(100);
    }

    if (WiFi.status() == WL_CONNECTED) {
      logCaptureLn(String("WiFi 已连接, IP: " + WiFi.localIP().toString()));
      logCaptureLn(String("WiFi 信号强度: " + String(WiFi.RSSI()) + " dBm"));
      return true;
    }

    logCaptureF("⚠️ WiFi 第 %u 次连接超时\n", attempt);
  }

  return false;
}

void startWifiSetupAp() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  wifiApMode = WiFi.softAP(WIFI_SETUP_AP_SSID, WIFI_SETUP_PASSWORD);
  if (wifiApMode) {
    logCaptureLn(String("📡 已启动 WiFi 配置热点: " WIFI_SETUP_AP_SSID));
    logCaptureLn(String("配置地址: http://" + WiFi.softAPIP().toString() + "/"));
  } else {
    logCaptureLn(String("❌ WiFi 配置热点启动失败"));
  }
}

void maintainWifiConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    backgroundFailureCount = 0;
    return;
  }
  if (config.wifiSsid.length() == 0) {
    if (!wifiApMode) startWifiSetupAp();
    return;
  }
  if (millis() - lastReconnectAttempt < 30000UL) return;
  lastReconnectAttempt = millis();

  backgroundFailureCount++;
  if (backgroundFailureCount >= 3 && !wifiApMode) {
    logCaptureLn(String("⚠️ 后台多次重连失败，启动配置热点"));
    startWifiSetupAp();
  }
  logCaptureLn(String("后台尝试重连 WiFi: " + config.wifiSsid));
  WiFi.mode(wifiApMode ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPass.c_str());
}

String getActiveWifiAddress() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (wifiApMode) return WiFi.softAPIP().toString();
  return "0.0.0.0";
}
