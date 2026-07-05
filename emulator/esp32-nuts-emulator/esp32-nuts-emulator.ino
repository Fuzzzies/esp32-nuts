/*
 * esp32-nuts — EMULATOR firmware (fake UPS data, no USB host)
 * ===========================================================
 * Paste this whole file into the Arduino IDE and flash it. Nothing else
 * needed — it only uses libraries bundled with the ESP32 board package.
 *
 * Board setup (Tools menu):
 *   Board:            "ESP32S2 Dev Module"  (or "LOLIN S2 Mini")
 *   USB CDC On Boot:  "Enabled"             (serial monitor over the USB port)
 *   PSRAM:            "Enabled"             (the S2FN4R2 has 2MB in-package)
 *
 * Flashing the S2: hold the 0/BOOT button, tap RST, release 0 — then Upload.
 * Press RST again after flashing. That two-button dance is normal.
 *
 * What it does:
 *   - Serves the same dashboard / API / NUT server as the real firmware,
 *     fed by a simulated CyberPower-ish UPS (wandering ~25% load on a 900 W
 *     unit, battery that drains and recharges).
 *   - Dashboard button (or GET /api/sim?ac=0|1) simulates a power failure.
 *
 * WiFi: either fill in WIFI_SSID/WIFI_PASS below, or leave them empty and
 * the device starts an open AP "esp32-nuts-setup" — join it and open
 * http://192.168.4.1/setup to enter credentials (stored in flash).
 *
 * Endpoints once on your network (http://esp32-nuts.local):
 *   GET /            dashboard
 *   GET /api/status  JSON
 *   GET /api/sim     ?ac=0|1 toggle simulated utility power
 *   GET /setup       change WiFi credentials
 *   TCP :3493        NUT protocol (Home Assistant, Synology, upsc)
 */

#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <math.h>
#include <stdarg.h>

// ---- config -----------------------------------------------------------------

#define WIFI_SSID ""   // optional: bake in credentials and skip the setup AP
#define WIFI_PASS ""
#define HOSTNAME  "esp32-nuts"
#define NUT_PORT  3493

static const float NOMINAL_W   = 900.0f;  // rated active power of the fake UPS
static const float CAPACITY_WH = 120.0f;  // fake battery capacity

// ---- state --------------------------------------------------------------

struct UpsState {
  bool acPresent = true, charging = false, discharging = false;
  bool lowBattery = false, fullyCharged = true;
  float charge = 100, runtimeS = 3600, battV = 13.6;
  float loadPct = 25, vin = 121, vout = 120.4;
};

// Declared up here so the Arduino preprocessor's auto-generated function
// prototypes (inserted above the first function) can see it.
struct NutVar {
  const char *name;
  char value[64];
};

UpsState ups;
bool simAc = true;
bool provisioning = false;
bool mdnsUp = false;

WebServer http(80);
WiFiServer nutServer(NUT_PORT);
Preferences prefs;

const int MAX_NUT_CLIENTS = 4;
WiFiClient nutClients[MAX_NUT_CLIENTS];
char nutLine[MAX_NUT_CLIENTS][160];
size_t nutLen[MAX_NUT_CLIENTS];

// ---- dashboard page -----------------------------------------------------

const char INDEX_HTML[] PROGMEM = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32-nuts</title>
<style>
  :root { --bg:#14161a; --card:#1e2128; --fg:#e8e8e8; --dim:#8b919c; --ok:#3ddc84; --warn:#ffb02e; --bad:#ff5252; --accent:#4a9eff; }
  * { box-sizing:border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font-family:system-ui,Segoe UI,Roboto,sans-serif; }
  header { display:flex; align-items:center; gap:.6em; padding:1em 1.2em; }
  header h1 { font-size:1.15em; margin:0; font-weight:600; }
  #variant { font-size:.7em; padding:.2em .6em; border-radius:999px; background:#333a45; color:var(--dim); text-transform:uppercase; letter-spacing:.05em; }
  #statuschip { margin-left:auto; font-weight:700; padding:.3em .8em; border-radius:999px; background:#333a45; }
  #statuschip.ol { background:rgba(61,220,132,.15); color:var(--ok); }
  #statuschip.ob { background:rgba(255,176,46,.18); color:var(--warn); }
  #statuschip.off { background:rgba(255,82,82,.15); color:var(--bad); }
  main { display:grid; gap:.8em; padding:0 1.2em 1.2em; grid-template-columns:repeat(auto-fit,minmax(150px,1fr)); max-width:900px; margin:0 auto; }
  .card { background:var(--card); border-radius:12px; padding:1em; }
  .card .k { color:var(--dim); font-size:.75em; text-transform:uppercase; letter-spacing:.06em; }
  .card .v { font-size:1.9em; font-weight:700; margin-top:.15em; }
  .card .u { font-size:.5em; color:var(--dim); font-weight:500; }
  .bar { height:6px; border-radius:3px; background:#333a45; margin-top:.6em; overflow:hidden; }
  .bar i { display:block; height:100%; background:var(--accent); width:0; transition:width .5s; }
  #battbar i { background:var(--ok); }
  footer { color:var(--dim); font-size:.8em; text-align:center; padding:1em; }
  footer a { color:var(--accent); text-decoration:none; }
  #simbtn { display:none; margin:0 auto 1em; padding:.5em 1.2em; border:1px solid #444; border-radius:8px; background:transparent; color:var(--fg); cursor:pointer; display:block; }
</style>
</head>
<body>
<header>
  <h1>&#x1F95C; esp32-nuts</h1>
  <span id="variant">&mdash;</span>
  <span id="statuschip">connecting&hellip;</span>
</header>
<main>
  <div class="card"><div class="k">Load</div><div class="v"><span id="watts">&ndash;</span><span class="u"> W</span></div>
    <div class="bar"><i id="loadbar"></i></div></div>
  <div class="card"><div class="k">Load</div><div class="v"><span id="loadpct">&ndash;</span><span class="u"> %</span></div></div>
  <div class="card"><div class="k">Battery</div><div class="v"><span id="charge">&ndash;</span><span class="u"> %</span></div>
    <div class="bar" id="battbar"><i></i></div></div>
  <div class="card"><div class="k">Runtime</div><div class="v"><span id="runtime">&ndash;</span></div></div>
  <div class="card"><div class="k">Input</div><div class="v"><span id="vin">&ndash;</span><span class="u"> V</span></div></div>
  <div class="card"><div class="k">Output</div><div class="v"><span id="vout">&ndash;</span><span class="u"> V</span></div></div>
</main>
<button id="simbtn"></button>
<footer>
  <span id="model">&mdash;</span> &middot; <span id="ip"></span> &middot; RSSI <span id="rssi"></span> dBm
  &middot; up <span id="uptime"></span> &middot; <a href="/setup">wifi setup</a> &middot; NUT :3493
</footer>
<script>
const $ = id => document.getElementById(id);
const fmt = (v, d=0) => v == null ? '–' : v.toFixed(d);
let acOn = true;

function fmtDur(s) {
  if (s == null) return '–';
  s = Math.round(s);
  const h = Math.floor(s/3600), m = Math.floor(s%3600/60);
  return h > 0 ? `${h}h ${String(m).padStart(2,'0')}m` : `${m}m ${String(s%60).padStart(2,'0')}s`;
}

async function tick() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    $('variant').textContent = d.variant;
    const chip = $('statuschip');
    chip.textContent = d.connected ? d.status : 'no UPS';
    chip.className = !d.connected ? 'off' : d.status.startsWith('OB') ? 'ob' : 'ol';
    $('watts').textContent = fmt(d.load_watts);
    $('loadpct').textContent = fmt(d.load_percent);
    $('loadbar').style.width = (d.load_percent ?? 0) + '%';
    $('charge').textContent = fmt(d.battery_charge);
    document.querySelector('#battbar i').style.width = (d.battery_charge ?? 0) + '%';
    $('runtime').textContent = fmtDur(d.battery_runtime_s);
    $('vin').textContent = fmt(d.input_voltage, 1);
    $('vout').textContent = fmt(d.output_voltage, 1);
    $('model').textContent = [d.mfr, d.model].filter(Boolean).join(' ') || 'unknown UPS';
    $('ip').textContent = d.wifi.ip;
    $('rssi').textContent = d.wifi.rssi;
    $('uptime').textContent = fmtDur(d.uptime_s);
    if (d.variant === 'emulator') {
      acOn = !d.status.startsWith('OB');
      const b = $('simbtn');
      b.style.display = 'block';
      b.textContent = acOn ? '⚡ simulate power failure' : '\u{1F50C} restore utility power';
    }
  } catch (e) {
    $('statuschip').textContent = 'unreachable';
    $('statuschip').className = 'off';
  }
}

$('simbtn').onclick = async () => {
  await fetch('/api/sim?ac=' + (acOn ? 0 : 1));
  tick();
};

tick();
setInterval(tick, 2000);
</script>
</body>
</html>
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

// ---- emulated UPS -----------------------------------------------------------

float frand(float amplitude) {
  return (random(0, 1001) / 1000.0f - 0.5f) * 2.0f * amplitude;
}

void tickEmulator() {
  static float t = 0;
  t += 1.0f;

  ups.loadPct = 24.0f + 6.0f * sinf(t * 2.0f * PI / 90.0f) + frand(1.0f);
  if (ups.loadPct < 1) ups.loadPct = 1;
  float watts = ups.loadPct * NOMINAL_W / 100.0f;

  ups.acPresent = simAc;
  ups.charging = ups.discharging = ups.fullyCharged = false;

  if (simAc) {
    ups.vin = 121.0f + 1.2f * sinf(t * 2.0f * PI / 300.0f) + frand(0.3f);
    ups.vout = ups.vin * 0.995f;
    if (ups.charge < 100.0f) {
      ups.charge += 0.35f;  // ~5 min to full in demo time
      if (ups.charge > 100) ups.charge = 100;
      ups.charging = true;
    } else {
      ups.fullyCharged = true;
    }
  } else {
    ups.vin = 0;
    ups.vout = 120.0f + frand(0.2f);
    ups.discharging = true;
    ups.charge -= watts / (CAPACITY_WH * 36.0f);  // %/s for this load
    if (ups.charge < 4) ups.charge = 4;           // never fully die — it's a demo
  }

  ups.battV = 12.0f + 1.6f * (ups.charge / 100.0f) + (ups.charging ? 0.3f : 0.0f);
  ups.runtimeS = ups.charge / 100.0f * CAPACITY_WH / watts * 3600.0f;
  ups.lowBattery = ups.charge < 20.0f;
}

const char *statusString() {
  static char buf[48];
  strcpy(buf, ups.acPresent ? "OL" : "OB");
  if (ups.lowBattery)   strcat(buf, " LB");
  if (ups.charging)     strcat(buf, " CHRG");
  if (ups.fullyCharged) {}  // not part of ups.status
  return buf;
}

float loadWatts() { return ups.loadPct * NOMINAL_W / 100.0f; }

// ---- HTTP handlers ------------------------------------------------------

void handleStatus() {
  char buf[768];
  snprintf(buf, sizeof(buf),
           "{\"variant\":\"emulator\",\"connected\":true,\"status\":\"%s\","
           "\"mfr\":\"CPS (emulated)\",\"model\":\"CP900AVR-EMU\",\"serial\":\"EMU0000001\","
           "\"battery_charge\":%.0f,\"battery_runtime_s\":%.0f,\"battery_voltage\":%.1f,"
           "\"load_percent\":%.0f,\"load_watts\":%.0f,\"realpower_nominal\":%.0f,"
           "\"input_voltage\":%.1f,\"output_voltage\":%.1f,"
           "\"wifi\":{\"ip\":\"%s\",\"rssi\":%d,\"provisioning\":%s},\"uptime_s\":%lu}",
           statusString(), ups.charge, ups.runtimeS, ups.battV,
           ups.loadPct, loadWatts(), NOMINAL_W, ups.vin, ups.vout,
           WiFi.localIP().toString().c_str(), WiFi.RSSI(),
           provisioning ? "true" : "false", (unsigned long)(millis() / 1000));
  http.send(200, "application/json", buf);
}

void handleSim() {
  if (http.hasArg("ac")) {
    simAc = http.arg("ac") != "0";
    Serial.printf("[sim] utility power: %s\n", simAc ? "ON" : "FAILED");
  }
  http.send(200, "application/json", "{\"ok\":true}");
}

void handleSetupSave() {
  String ssid = http.arg("ssid");
  String pass = http.arg("pass");
  if (ssid.length() == 0) {
    http.send(400, "text/plain", "ssid required");
    return;
  }
  prefs.begin("espnuts", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  http.send(200, "text/html",
            "<html><body style='font-family:system-ui'>Saved. Rebooting and joining "
            "your network&hellip; find me at http://" HOSTNAME ".local</body></html>");
  delay(1500);
  ESP.restart();
}

// ---- NUT server ----------------------------------------------------------

void nutSendf(WiFiClient &c, const char *fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  c.write((const uint8_t *)buf, n);
}

size_t buildVars(NutVar *vars, size_t maxVars) {
  size_t n = 0;
  auto add = [&](const char *name, const char *fmt, ...) {
    if (n >= maxVars) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vars[n].value, sizeof(vars[n].value), fmt, ap);
    va_end(ap);
    vars[n++].name = name;
  };
  add("device.type", "ups");
  add("device.mfr", "CPS (emulated)");
  add("device.model", "CP900AVR-EMU");
  add("device.serial", "EMU0000001");
  add("ups.mfr", "CPS (emulated)");
  add("ups.model", "CP900AVR-EMU");
  add("ups.status", "%s", statusString());
  add("driver.name", "esp32-nuts");
  add("driver.version", "0.1.0");
  add("ups.load", "%.0f", ups.loadPct);
  add("ups.realpower.nominal", "%.0f", NOMINAL_W);
  add("ups.realpower", "%.0f", loadWatts());
  add("battery.charge", "%.0f", ups.charge);
  add("battery.runtime", "%.0f", ups.runtimeS);
  add("battery.voltage", "%.1f", ups.battV);
  add("input.voltage", "%.1f", ups.vin);
  add("output.voltage", "%.1f", ups.vout);
  return n;
}

// Returns false if the connection should close.
bool handleNutLine(WiFiClient &c, char *line) {
  char *tok[5];
  int n = 0;
  char *save = nullptr;
  for (char *t = strtok_r(line, " \t", &save); t && n < 5;
       t = strtok_r(nullptr, " \t", &save)) {
    tok[n++] = t;
  }
  if (n == 0) return true;

  if (!strcasecmp(tok[0], "LIST") && n >= 2 && !strcasecmp(tok[1], "UPS")) {
    nutSendf(c, "BEGIN LIST UPS\n");
    nutSendf(c, "UPS ups \"esp32-nuts emulated UPS\"\n");
    nutSendf(c, "END LIST UPS\n");
  } else if (!strcasecmp(tok[0], "LIST") && n >= 3 && !strcasecmp(tok[1], "VAR")) {
    if (strcmp(tok[2], "ups")) { nutSendf(c, "ERR UNKNOWN-UPS\n"); return true; }
    NutVar vars[20];
    size_t count = buildVars(vars, 20);
    nutSendf(c, "BEGIN LIST VAR ups\n");
    for (size_t i = 0; i < count; i++)
      nutSendf(c, "VAR ups %s \"%s\"\n", vars[i].name, vars[i].value);
    nutSendf(c, "END LIST VAR ups\n");
  } else if (!strcasecmp(tok[0], "GET") && n >= 4 && !strcasecmp(tok[1], "VAR")) {
    if (strcmp(tok[2], "ups")) { nutSendf(c, "ERR UNKNOWN-UPS\n"); return true; }
    NutVar vars[20];
    size_t count = buildVars(vars, 20);
    for (size_t i = 0; i < count; i++) {
      if (!strcmp(vars[i].name, tok[3])) {
        nutSendf(c, "VAR ups %s \"%s\"\n", vars[i].name, vars[i].value);
        return true;
      }
    }
    nutSendf(c, "ERR VAR-NOT-SUPPORTED\n");
  } else if (!strcasecmp(tok[0], "GET") && n >= 3 && !strcasecmp(tok[1], "UPSDESC")) {
    nutSendf(c, "UPSDESC ups \"esp32-nuts emulated UPS\"\n");
  } else if (!strcasecmp(tok[0], "USERNAME") || !strcasecmp(tok[0], "PASSWORD") ||
             !strcasecmp(tok[0], "LOGIN")) {
    nutSendf(c, "OK\n");
  } else if (!strcasecmp(tok[0], "LOGOUT")) {
    nutSendf(c, "OK Goodbye\n");
    return false;
  } else if (!strcasecmp(tok[0], "STARTTLS")) {
    nutSendf(c, "ERR FEATURE-NOT-SUPPORTED\n");
  } else if (!strcasecmp(tok[0], "VER")) {
    nutSendf(c, "esp32-nuts 0.1.0 (NUT compatible)\n");
  } else if (!strcasecmp(tok[0], "NETVER") || !strcasecmp(tok[0], "PROTVER")) {
    nutSendf(c, "1.3\n");
  } else {
    nutSendf(c, "ERR UNKNOWN-COMMAND\n");
  }
  return true;
}

void pollNut() {
  // accept
  WiFiClient incoming = nutServer.accept();
  if (incoming) {
    bool placed = false;
    for (int i = 0; i < MAX_NUT_CLIENTS; i++) {
      if (!nutClients[i] || !nutClients[i].connected()) {
        nutClients[i] = incoming;
        nutLen[i] = 0;
        placed = true;
        break;
      }
    }
    if (!placed) incoming.stop();
  }
  // read
  for (int i = 0; i < MAX_NUT_CLIENTS; i++) {
    WiFiClient &c = nutClients[i];
    if (!c || !c.connected()) continue;
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') {
        while (nutLen[i] > 0 && nutLine[i][nutLen[i] - 1] == '\r') nutLen[i]--;
        nutLine[i][nutLen[i]] = '\0';
        nutLen[i] = 0;
        if (!handleNutLine(c, nutLine[i])) {
          c.stop();
          break;
        }
      } else if (nutLen[i] < sizeof(nutLine[i]) - 1) {
        nutLine[i][nutLen[i]++] = ch;
      }
    }
  }
}

// ---- WiFi ---------------------------------------------------------------

void startSetupAp() {
  WiFi.mode(provisioning ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP("esp32-nuts-setup");
  Serial.println("[wifi] setup AP up: join 'esp32-nuts-setup', open http://192.168.4.1/setup");
}

void connectWifi() {
  String ssid = WIFI_SSID;
  String pass = WIFI_PASS;
  if (ssid.length() == 0) {
    prefs.begin("espnuts", true);
    ssid = prefs.getString("ssid", "");
    pass = prefs.getString("pass", "");
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
    MDNS.addService("nut", "tcp", NUT_PORT);
    mdnsUp = true;
  }
}

// ---- arduino ------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[esp32-nuts] emulator firmware starting");

  connectWifi();

  http.on("/", []() { http.send_P(200, "text/html", INDEX_HTML); });
  http.on("/api/status", handleStatus);
  http.on("/api/sim", handleSim);
  http.on("/setup", []() { http.send_P(200, "text/html", SETUP_HTML); });
  http.on("/setup/save", HTTP_POST, handleSetupSave);
  http.begin();

  nutServer.begin();
  maybeStartMdns();
  Serial.printf("[esp32-nuts] dashboard :80, NUT :%d\n", NUT_PORT);
}

void loop() {
  http.handleClient();
  pollNut();
  maybeStartMdns();

  static unsigned long lastTick = 0;
  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    tickEmulator();
  }
}
