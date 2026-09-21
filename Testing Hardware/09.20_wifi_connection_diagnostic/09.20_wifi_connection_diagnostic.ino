/* ESP32 Wi-Fi-only diagnostic. No motor or UART commands are sent.
   Serial Monitor: 115200 baud. Tests only the configured network below.
   The password is in this local sketch; do not publish the file.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_system.h>

constexpr uint8_t DRIVER_ENABLE_PIN = 27; // Shared active-low EN: HIGH keeps drivers disabled.
constexpr uint32_t JOIN_TIMEOUT_MS = 30000;
const char *TARGET_SSID = "Rhymes with Donna";
const char *TARGET_PASSWORD = "!Bbrosgaming2020";
WebServer web(80);
bool joining = false, wasConnected = false, webStarted = false;
uint32_t joinStarted = 0, lastReport = 0;
volatile uint8_t lastDisconnectReason = 0;

const char *statusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS: return "idle";
    case WL_NO_SSID_AVAIL: return "SSID not found";
    case WL_SCAN_COMPLETED: return "scan completed";
    case WL_CONNECTED: return "connected";
    case WL_CONNECT_FAILED: return "connect failed";
    case WL_CONNECTION_LOST: return "connection lost";
    case WL_DISCONNECTED: return "disconnected";
    default: return "other";
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) Serial.println("EVENT: associated with access point");
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) Serial.println("EVENT: IPv4 address assigned");
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("EVENT: disconnected, reason code %u\n", unsigned(lastDisconnectReason));
  }
}

void scanNearby() {
  Serial.printf("Scanning for '%s' (ESP32 uses 2.4 GHz)...\n", TARGET_SSID);
  const int count = WiFi.scanNetworks();
  if (count < 0) Serial.printf("Scan failed, result %d\n", count);
  else {
    bool found = false;
    for (int i = 0; i < count; ++i) if (WiFi.SSID(i) == TARGET_SSID) {
      found = true;
      Serial.printf("TARGET FOUND: channel %d | RSSI %d dBm | auth %d\n",
        WiFi.channel(i), WiFi.RSSI(i), int(WiFi.encryptionType(i)));
    }
    if (!found) Serial.println("TARGET NOT FOUND in scan. Check 2.4 GHz coverage and exact SSID.");
  }
  WiFi.scanDelete();
}

void beginJoin() {
  Serial.printf("Trying SSID '%s' for %lu seconds (password not printed).\n",
    TARGET_SSID, (unsigned long)(JOIN_TIMEOUT_MS / 1000));
  lastDisconnectReason = 0;
  WiFi.begin(TARGET_SSID, TARGET_PASSWORD);
  joining = true; joinStarted = millis(); lastReport = 0; wasConnected = false;
}

void setup() {
  pinMode(DRIVER_ENABLE_PIN, OUTPUT);
  digitalWrite(DRIVER_ENABLE_PIN, HIGH);
  Serial.begin(115200);
  delay(1000);
  Serial.println("ESP32 Wi-Fi diagnostic; motor drivers disabled; no UART or STEP output.");
  Serial.printf("Chip: %s | reset reason: %d | free heap: %u bytes\n",
    ESP.getChipModel(), int(esp_reset_reason()), ESP.getFreeHeap());
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.mode(WIFI_STA);
  scanNearby();
  web.on("/", []() {
    web.send(200, "text/plain", String("ESP32 Wi-Fi diagnostic reachable. SSID: ") + WiFi.SSID() +
      "\nIP: " + WiFi.localIP().toString() + "\nRSSI: " + String(WiFi.RSSI()) + " dBm\n");
  });
  beginJoin();
}

void loop() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    joining = false;
    if (!webStarted) { web.begin(); webStarted = true; }
    Serial.printf("CONNECTED: SSID '%s' | IP %s | RSSI %d dBm | channel %d\n",
      WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
    Serial.printf("From the computer, open http://%s/ and verify the diagnostic page.\n",
      WiFi.localIP().toString().c_str());
  }
  if (!connected && wasConnected)
    Serial.printf("CONNECTION LOST: status %s | last reason %u\n", statusName(WiFi.status()), unsigned(lastDisconnectReason));
  wasConnected = connected;
  if (joining && !connected && millis() - joinStarted >= JOIN_TIMEOUT_MS) {
    joining = false;
    Serial.printf("JOIN FAILED: status %s | last disconnect reason %u. Check the scan result, exact SSID/password, and 2.4 GHz setting.\n",
      statusName(WiFi.status()), unsigned(lastDisconnectReason));
    Serial.println("This sketch will not try another network. Reset the ESP to repeat the test.");
  }
  if (millis() - lastReport >= 5000) {
    lastReport = millis();
    Serial.printf("STATUS: %s | reason %u | IP %s | RSSI %d dBm | heap %u\n",
      statusName(WiFi.status()), unsigned(lastDisconnectReason), WiFi.localIP().toString().c_str(),
      connected ? WiFi.RSSI() : 0, ESP.getFreeHeap());
  }
  if (connected) web.handleClient();
  delay(10);
}
