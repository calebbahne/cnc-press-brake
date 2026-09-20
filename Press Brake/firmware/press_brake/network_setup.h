#pragma once
#include <esp_system.h>
Preferences networkPrefs;
String customSSID, customPassword;
uint32_t networkAttemptAt=0, restartAt=0;
int networkSlot=0;
bool networkWasOnline=false;
String setupToken;

void tryNetwork() {
  WiFi.disconnect();
  const char *ssid=networkSlot==0 ? WIFI_SSID : networkSlot==1 ? WIFI_SECOND_SSID : customSSID.c_str();
  const char *password=networkSlot==0 ? WIFI_PASSWORD : networkSlot==1 ? WIFI_SECOND_PASSWORD : customPassword.c_str();
  WiFi.begin(ssid,password); networkAttemptAt=millis();
  Serial.printf("Trying Wi-Fi preference %d\n",networkSlot+1);
}
void beginNetwork() {
  networkPrefs.begin("brake-wifi",false);
  customSSID=networkPrefs.getString("ssid",""); customPassword=networkPrefs.getString("password","");
  WiFi.persistent(false); WiFi.setAutoReconnect(false);
  WiFi.setHostname("cnc-press-brake"); WiFi.mode(WIFI_STA);
  tryNetwork();
}
void serviceNetwork() {
  if (restartAt && int32_t(millis()-restartAt)>=0) { ESP.restart(); return; }
  if (setupAP) return;
  if (WiFi.status()==WL_CONNECTED) { networkWasOnline=true; return; }
  if (networkWasOnline) {
    networkWasOnline=false; latchFault(2); owner=-1; networkSlot=0; tryNetwork(); return;
  }
  if (millis()-networkAttemptAt<15000) return;
  ++networkSlot;
  if (networkSlot<2 || (networkSlot==2 && customSSID.length())) { tryNetwork(); return; }
  stopMotion(true); owner=-1; WiFi.disconnect(); WiFi.mode(WIFI_AP);
  setupAP=WiFi.softAP("PressBrake-Setup",SETUP_AP_PASSWORD);
  setupToken=String(esp_random(),HEX)+String(esp_random(),HEX);
  if (setupAP) Serial.println("Wi-Fi setup: join PressBrake-Setup, then http://192.168.4.1. Motion disabled.");
  else { networkSlot=0; WiFi.mode(WIFI_STA); tryNetwork(); }
}
void networkPage() {
  http.sendHeader("Cache-Control","no-store");
  if (!setupAP) { http.send(200,"text/plain","Motion API online. Open the PC UI at http://127.0.0.1:8080. Wi-Fi provisioning is available only on the fallback setup network."); return; }
  String page=R"HTML(<!doctype html><meta name="viewport" content="width=device-width"><title>Press Brake Wi-Fi</title><style>body{font:18px system-ui;max-width:520px;margin:3rem auto;padding:1rem}input,button{font:inherit;display:block;padding:.7rem;margin:1rem 0;width:90%}</style><h1>Connect your press brake</h1><p>Motion is disabled during Wi-Fi setup. This saves a third network preference after the two configured networks.</p><form method="post" action="/wifi"><label>Network name<input name="ssid" maxlength="32" required></label><label>Password<input name="password" type="password" minlength="8" maxlength="63" required></label>)HTML";
  page+="<input type='hidden' name='token' value='"+setupToken+"'><button>Save network and restart</button></form>";
  http.send(200,"text/html",page);
}
void saveNetwork() {
  if (!setupAP || enabled || http.arg("token")!=setupToken) { http.send(403,"text/plain","Use the local setup form while motion is disabled."); return; }
  const String ssid=http.arg("ssid"), password=http.arg("password");
  if (!ssid.length() || ssid.length()>32 || password.length()<8 || password.length()>63) { http.send(400,"text/plain","SSID must be 1-32 bytes; password 8-63 bytes."); return; }
  // NVS survives reboot; no credentials are returned through the motion API.
  if (!networkPrefs.putString("ssid",ssid) || !networkPrefs.putString("password",password)) { http.send(500,"text/plain","Could not save network."); return; }
  http.send(200,"text/plain","Saved. Restarting and trying networks in preference order. Rejoin your normal network, then start the PC UI with the ESP address from Serial Monitor. If connection fails, setup returns.");
  restartAt=millis()+1000;
}
