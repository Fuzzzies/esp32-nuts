/*
 * esp32-nuts — bootstrap firmware (WiFi provisioning only)
 * =========================================================
 * Source of truth for the Arduino bootstrap pipeline (see esp32-flasher.json).
 *
 * Board (Tools menu or FQBN from esp32-flasher.json):
 *   esp32:esp32:esp32s2:CDCOnBoot=cdc,PSRAM=enabled
 *
 * Flashing the S2: hold 0/BOOT, tap RST, release 0 — then Upload. Tap RST after.
 *
 * With no baked-in credentials the device starts an open AP "esp32-nuts-setup".
 * Join it and open http://192.168.4.1/setup to save WiFi (namespace wifi, keys
 * ssid/password — see docs/NVS.md). After provisioning, flash the full ESP-IDF
 * USB firmware; NVS keys differ between pipelines.
 */

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#define WIFI_SSID ""
#define WIFI_PASS ""
#define HOSTNAME  "esp32-nuts"
#define SETUP_AP  "esp32-nuts-setup"

bool provisioning = false;
bool mdnsUp = false;

WebServer http(80);
Preferences prefs;

const char INDEX_HTML[] PROGMEM = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32-nuts bootstrap</title>
<style>
  body{margin:0;background:#14161a;color:#e8e8e8;font-family:system-ui,sans-serif;display:flex;justify-content:center;padding:3rem 1rem}
  .card{background:#1e2128;border-radius:12px;padding:2rem;max-width:420px}
  h1{font-size:1.2em;margin:0 0 .5em}
  p{color:#8b919c;line-height:1.5}
  a{color:#4a9eff}
</style></head><body><div class="card">
<h1>&#x1F95C; esp32-nuts bootstrap</h1>
<p>WiFi provisioning firmware. Credentials are saved for the <strong>bootstrap</strong> NVS profile (<code>wifi.ssid</code> / <code>wifi.password</code>).</p>
<p>Flash the <strong>full USB firmware</strong> when ready — it uses a different NVS layout (<code>espnuts.wifi_ssid</code> / <code>espnuts.wifi_pass</code>). Re-provision or use esp32-flasher after switching pipelines.</p>
<p><a href="/setup">WiFi setup</a></p>
</div></body></html>
)html";

const char SETUP_HTML[] PROGMEM =
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>esp32-nuts setup</title><style>body{font-family:system-ui;background:#14161a;color:#e8e8e8;"
    "display:flex;justify-content:center;padding-top:10vh}form{background:#1e2128;padding:2em;border-radius:12px;"
    "min-width:280px}input{width:100%;margin:.4em 0 1em;padding:.6em;border-radius:6px;border:1px solid #444;"
    "background:#14161a;color:#e8e8e8;box-sizing:border-box}button{width:100%;padding:.7em;border:0;"
    "border-radius:6px;background:#4a9eff;color:#fff;font-size:1em;cursor:pointer}</style></head><body>"
    "<form method=post action=/setup/save><h2>&#x1F95C; esp32-nuts WiFi</h2>"
    "<label>SSID</label><input name=ssid required maxlength=32>"
    "<label>Password</label><input name=pass type=password maxlength=64>"
    "<button>Save &amp; reboot</button></form></body></html>";

void startSetupAp() {
  WiFi.mode(provisioning ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(SETUP_AP);
  Serial.printf("[wifi] setup AP up: join '%s', open http://192.168.4.1/setup\n", SETUP_AP);
}

void connectWifi() {
  String ssid = WIFI_SSID;
  String pass = WIFI_PASS;
  if (ssid.length() == 0) {
    prefs.begin("wifi", true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("password", "");
    prefs.end();
  }

  if (ssid.length() == 0) {
    provisioning = true;
    startSetupAp();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[wifi] connecting to '%s'", ssid.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected: http://%s / http://%s.local\n",
                  WiFi.localIP().toString().c_str(), HOSTNAME);
  } else {
    Serial.println("[wifi] failed — keeping STA retries AND starting setup AP");
    startSetupAp();
  }
}

void maybeStartMdns() {
  if (mdnsUp || WiFi.status() != WL_CONNECTED) return;
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsUp = true;
  }
}

void handleStatus() {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"variant\":\"bootstrap\",\"wifi\":{\"ip\":\"%s\",\"rssi\":%d,\"provisioning\":%s},"
           "\"uptime_s\":%lu}",
           WiFi.localIP().toString().c_str(), WiFi.RSSI(),
           provisioning ? "true" : "false", (unsigned long)(millis() / 1000));
  http.send(200, "application/json", buf);
}

void handleSetupSave() {
  String ssid = http.arg("ssid");
  String pass = http.arg("pass");
  if (ssid.length() == 0) {
    http.send(400, "text/plain", "ssid required");
    return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("password", pass);
  prefs.end();
  http.send(200, "text/html",
            "<html><body style='font-family:system-ui'>Saved. Rebooting and joining "
            "your network&hellip; find me at http://" HOSTNAME ".local</body></html>");
  delay(1500);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[esp32-nuts] bootstrap firmware starting");

  connectWifi();

  http.on("/", []() { http.send_P(200, "text/html", INDEX_HTML); });
  http.on("/api/status", handleStatus);
  http.on("/setup", []() { http.send_P(200, "text/html", SETUP_HTML); });
  http.on("/setup/save", HTTP_POST, handleSetupSave);
  http.begin();

  maybeStartMdns();
  Serial.println("[esp32-nuts] bootstrap portal :80");
}

void loop() {
  http.handleClient();
  maybeStartMdns();
}
