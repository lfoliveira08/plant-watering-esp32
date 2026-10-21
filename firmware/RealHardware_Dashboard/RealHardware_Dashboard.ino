/*
 * ENGR 4399 - Project 1: Automatic Plant Watering System - REAL ESP32 + WEB DASHBOARD
 * (stage 1 of the Smart Greenhouse)
 *
 * Inspired by the Instagram video "Automatic Plant Watering System" by eazytronic3795.
 *
 * Added on top of the tested RealHardware.ino:
 *   - Wi-Fi weather forecast (Open-Meteo, no API key): rain chance blocks automatic watering
 *   - temperature judgement per plant profile + active buzzer alert
 *   - 3 per-plant profiles (start/stop moisture, healthy temperature range)
 *   - web dashboard served by the ESP32 itself, with SIMULATION buttons:
 *       rain forecast, tank low, soil too dry, temperature too high / too low
 *   - if the Wi-Fi does not connect the ESP32 opens its own network "PlantWater"
 *
 * Wiring (same as the tested build, plus the buzzer)
 *   Soil sensor     S -> D34   V -> 3V3   G -> GND
 *   Level sensor    S -> D35   V -> 3V3   G -> GND
 *   Relay header    S -> D26   +  -> VIN (5 V)   - -> GND   (ACTIVE-HIGH module)
 *   Relay screws    COM -> pump supply (+), NO -> pump red wire, pump black -> supply (-)
 *   OLED SSD1306    SDA -> D21  SCL -> D22  VCC -> 3V3  GND -> GND  (0x3C)
 *   Active buzzer   signal / (+) -> D27   (-) / G -> GND
 *
 * Board: "ESP32 Dev Module". Libraries: "Adafruit SSD1306", "Adafruit GFX Library".
 * WiFi, WebServer, ESPmDNS and HTTPClient come with the ESP32 core.
 */
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ============================ Options (edit these) =========================
static const char* WIFI_SSID = "YOUR_WIFI_NAME";   // network the ESP32 joins ("" = own network only)
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char* AP_SSID   = "PlantWater";   // own network if the Wi-Fi fails
static const char* AP_PASS   = "plant1234";    // at least 8 characters
static const uint32_t WIFI_TIMEOUT_MS = 12000;

// Weather location (default: University of the Incarnate Word, San Antonio TX)
static const char* LATITUDE  = "29.47";
static const char* LONGITUDE = "-98.47";
static const int   RAIN_SKIP_PCT = 60;         // rain chance (next 3 h) at/above this blocks automatic watering

static const uint8_t OLED_ADDR = 0x3C;
static const bool RELAY_ACTIVE_LOW     = false; // measured: this relay is ACTIVE-HIGH
static const bool RELAY_FLOAT_WHEN_OFF = false;
static const bool BUZZER_ACTIVE_LOW    = false;

// Soil calibration (this Keyestudio probe reads LOW when dry, HIGH when wet)
static const int SOIL_RAW_DRY = 0;
static const int SOIL_RAW_WET = 1500;
// Water-level sensor
static const int LEVEL_LOW_RAW  = 300;
static const int LEVEL_HYST_RAW = 150;

// Control
static const uint32_t MAX_RUN_MS        = 8000;
static const uint32_t SOAK_MS           = 30000;
static const float    MIN_RISE_PCT      = 3.0f;
static const int      MAX_FAILED_CYCLES = 3;
static const uint32_t PRIME_MAX_MS      = 5000;
static const uint32_t SAMPLE_MS         = 500;
static const uint32_t LOG_MS            = 2000;

// ============================== Plant profiles =============================
struct Profile { const char* name; const char* need; float dry; float wet; float tMin; float tMax; };
static const Profile PROFILES[3] = {
  // name      water need           start  stop  min C  max C
  { "Basil",  "thirsty",            40.0f, 70.0f, 12.0f, 32.0f },
  { "Aloe",   "drought-tolerant",   15.0f, 35.0f, 10.0f, 35.0f },
  { "Fern",   "likes moist soil",   50.0f, 80.0f, 16.0f, 27.0f },
};
static uint8_t profileIdx = 0;

// ================================== Pins ===================================
static const int PIN_SOIL   = 34;
static const int PIN_LEVEL  = 35;
static const int PIN_RELAY  = 26;
static const int PIN_BUZZER = 27;
static const int PIN_PRIME  = 0;     // BOOT button
static const int PIN_SDA    = 21;
static const int PIN_SCL    = 22;

static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool oledOk = false;
static WebServer server(80);

// ================================== State ==================================
enum State : uint8_t { IDLE, WATERING, SOAKING, FAULT };
static const char* STATE_NAME[] = {"IDLE", "WATERING", "SOAKING", "FAULT"};

static State    state = IDLE;
static uint32_t stateSince = 0;
static float    soilAtStart = 0;
static bool     evalPending = false;
static bool     noResponse = false;
static int      failedCycles = 0;
static int      cycles = 0;
static bool     primeOn = false;

static bool     realTankLow = false;
static bool     tankLow = false;          // real OR simulated
static float    soilReal = 0, soilShown = 0;
static int      soilRaw = 0, levelRaw = 0;

// Simulation switches (set from the dashboard)
static bool    simRain = false, simTank = false, simSoil = false;
static uint8_t simTemp = 0;               // 0 off, 1 too high, 2 too low

// Weather
static float    weatherTemp = NAN;
static int      weatherRain = -1;         // max rain probability next 3 h, -1 unknown
static uint32_t weatherOkAt = 0, nextWeatherAt = 0;
static bool     weatherEver = false;
static bool     apMode = false;
static String   ipText = "-";

// Alerts
static uint8_t lastTempStatus = 0;
static bool    muted = false;
static bool    buzzing = false;

// Event log shown on the dashboard
static char    events[8][80];
static uint8_t evHead = 0, evCount = 0;
static void logEvent(const char* msg) {
  strncpy(events[evHead], msg, 79); events[evHead][79] = 0;
  evHead = (evHead + 1) % 8; if (evCount < 8) evCount++;
  Serial.printf("[EVENT] %s\n", msg);
}

// ================================ Helpers ==================================
static void relayWrite(bool on) {
  if (RELAY_FLOAT_WHEN_OFF && RELAY_ACTIVE_LOW) {
    pinMode(PIN_RELAY, OUTPUT_OPEN_DRAIN);
    digitalWrite(PIN_RELAY, on ? LOW : HIGH);
    return;
  }
  pinMode(PIN_RELAY, OUTPUT);
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(PIN_RELAY, level ? HIGH : LOW);
}

static void buzzerWrite(bool on) {
  digitalWrite(PIN_BUZZER, (on != BUZZER_ACTIVE_LOW) ? HIGH : LOW);
}

static int readAveraged(int adcPin, int samples) {
  long acc = 0;
  for (int i = 0; i < samples; i++) { acc += analogRead(adcPin); delayMicroseconds(200); }
  return (int)(acc / samples);
}

static float soilPercentFromRaw(int raw) {
  float pct = 100.0f * (raw - SOIL_RAW_DRY) / (float)(SOIL_RAW_WET - SOIL_RAW_DRY);
  return constrain(pct, 0.0f, 100.0f);
}

static float curDry()  { return PROFILES[profileIdx].dry; }
static float curWet()  { return PROFILES[profileIdx].wet; }
static float curTMin() { return PROFILES[profileIdx].tMin; }
static float curTMax() { return PROFILES[profileIdx].tMax; }

static float effectiveTemp() {
  if (simTemp == 1) return curTMax() + 8.0f;
  if (simTemp == 2) return curTMin() - 8.0f;
  return weatherTemp;
}
static int effectiveRain() { return simRain ? 100 : weatherRain; }
static bool rainBlock()    { int r = effectiveRain(); return r >= RAIN_SKIP_PCT; }

// 0 unknown, 1 OK, 2 too high, 3 too low
static uint8_t tempStatus() {
  float t = effectiveTemp();
  if (isnan(t)) return 0;
  if (t > curTMax()) return 2;
  if (t < curTMin()) return 3;
  return 1;
}

static void setState(uint8_t s, const char* why) {
  char b[80];
  snprintf(b, sizeof b, "%s -> %s: %s", STATE_NAME[state], STATE_NAME[s], why);
  logEvent(b);
  state = (State)s;
  stateSince = millis();
}

// ================================ Weather ==================================
static void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return;
  String url = String("http://api.open-meteo.com/v1/forecast?latitude=") + LATITUDE + "&longitude=" + LONGITUDE +
               "&current=temperature_2m&hourly=precipitation_probability&forecast_hours=3&timezone=auto";
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(url);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    String body = http.getString();
    int c = body.indexOf("\"current\":{");
    if (c >= 0) {
      int t = body.indexOf("\"temperature_2m\":", c);
      if (t >= 0) { weatherTemp = body.substring(t + 17).toFloat(); ok = true; }
    }
    int h = body.indexOf("\"hourly\":{");
    if (h >= 0) {
      int p = body.indexOf("\"precipitation_probability\":[", h);
      if (p >= 0) {
        int i = p + 29, best = -1;
        while (i < (int)body.length() && body[i] != ']') {
          if (isDigit(body[i])) { int v = body.substring(i).toInt(); if (v > best) best = v; while (isDigit(body[i])) i++; }
          else i++;
        }
        if (best >= 0) weatherRain = best;
      }
    }
  }
  http.end();
  if (ok) {
    weatherEver = true; weatherOkAt = millis(); nextWeatherAt = millis() + 600000UL;
    Serial.printf("[WEATHER] %.1f C, rain chance next 3 h: %d %%\n", weatherTemp, weatherRain);
    logEvent("Weather updated");
  } else {
    nextWeatherAt = millis() + 30000UL;
    Serial.printf("[WEATHER] fetch failed (HTTP %d)\n", code);
  }
}

// ================================ Dashboard ================================
static const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Plant Watering Dashboard</title>
<style>
:root{--bg:#f4f6f3;--card:#fff;--ink:#16221a;--mut:#5c6b60;--line:#dfe5df;--ok:#1f7a45;--warn:#b26a00;--bad:#b3261e;--acc:#1f7a45;--sim:#6b3fa0}
@media(prefers-color-scheme:dark){:root{--bg:#101612;--card:#18211b;--ink:#e8efe9;--mut:#9bab9f;--line:#2a372d;--ok:#4cc27f;--warn:#e0a13a;--bad:#ff7b72;--acc:#4cc27f;--sim:#b58cf0}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.4 system-ui,sans-serif}
header{padding:14px 16px;display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}
h1{font-size:18px;margin:0}small,.mut{color:var(--mut)}
main{display:grid;gap:12px;padding:0 16px 24px;max-width:960px;margin:auto;grid-template-columns:repeat(auto-fit,minmax(280px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:14px}
.card h2{font-size:13px;letter-spacing:.04em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.big{font-size:34px;font-weight:700}.row{display:flex;justify-content:space-between;gap:8px;padding:3px 0}
.badge{display:inline-block;padding:2px 9px;border-radius:99px;font-weight:600;font-size:13px;border:1px solid currentColor}
.ok{color:var(--ok)}.warn{color:var(--warn)}.bad{color:var(--bad)}.simc{color:var(--sim)}
.bar{position:relative;height:16px;border:1px solid var(--line);border-radius:4px;margin:8px 0 14px;background:var(--bg)}
.bar i{display:block;height:100%;background:var(--acc);border-radius:3px}.bar b{position:absolute;top:-4px;width:2px;height:22px;background:var(--ink)}
button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--ink);padding:9px 12px;border-radius:8px;cursor:pointer;margin:3px 3px 3px 0;min-height:40px}
button.on{background:var(--sim);color:#fff;border-color:var(--sim)}button.sel{background:var(--acc);color:#fff;border-color:var(--acc)}
.log div{font:12px ui-monospace,monospace;padding:2px 0;border-bottom:1px dashed var(--line)}
.wide{grid-column:1/-1}#temp{font-size:26px}
</style></head><body>
<header><h1>Automatic Plant Watering &middot; Smart Greenhouse stage 1</h1><small id="conn">connecting...</small></header>
<main>
<section class="card"><h2>Weather &amp; temperature</h2>
 <div class="big" id="temp">--</div><div id="tverdict"></div>
 <div class="row"><span class="mut">Healthy range</span><span id="trange">--</span></div>
 <div class="row"><span class="mut">Rain chance (3 h)</span><span id="rain">--</span></div>
 <div class="row"><span class="mut">Watering</span><span id="rainverdict">--</span></div>
</section>
<section class="card"><h2>Soil &amp; pump</h2>
 <div class="big" id="soil">--</div>
 <div class="bar"><i id="soilbar" style="width:0"></i><b id="m1"></b><b id="m2"></b></div>
 <div class="row"><span class="mut">State</span><span id="state">--</span></div>
 <div class="row"><span class="mut">Pump</span><span id="pump">--</span></div>
 <div class="row"><span class="mut">Tank</span><span id="tank">--</span></div>
 <div class="row"><span class="mut">Failed cycles</span><span id="fail">--</span></div>
</section>
<section class="card"><h2>Plant profile</h2><div id="profiles"></div><div class="mut" id="pinfo"></div></section>
<section class="card"><h2>Simulations (inject a condition)</h2>
 <button id="s_rain" onclick="sim('rain',1)">Rain forecast</button>
 <button id="s_tank" onclick="sim('tank',1)">Tank low</button>
 <button id="s_soil" onclick="sim('soil',1)">Soil too dry</button>
 <button id="s_th" onclick="sim('temp',1)">Temp too high</button>
 <button id="s_tl" onclick="sim('temp',2)">Temp too low</button><br>
 <button onclick="api('/api/sim?name=clear')">Clear all</button>
 <button onclick="api('/api/mute')" id="mute">Silence buzzer</button>
 <button onclick="api('/api/weather')">Refresh weather</button>
 <div class="mut" id="simnote"></div>
</section>
<section class="card wide log"><h2>Event log</h2><div id="log"></div></section>
</main>
<script>
const $=i=>document.getElementById(i);let S={};
async function api(p){try{await fetch(p)}catch(e){}refresh()}
function sim(n,v){const cur=n==='temp'?S.sim.temp:(S.sim[n]?1:0);api('/api/sim?name='+n+'&val='+(cur===v?0:v))}
function F(c){return (c*9/5+32).toFixed(1)}
async function refresh(){try{S=await (await fetch('/api/status')).json();draw(S);$('conn').textContent=S.net+' Â· '+S.ip}catch(e){$('conn').textContent='offline'}}
function draw(s){const p=s.profiles[s.profile];
 $('temp').textContent=s.temp===null?'--':s.temp.toFixed(1)+' Â°C ('+F(s.temp)+' Â°F)'+(s.sim.temp?' *':'');
 const tv=['<span class="badge mut">no data</span>','<span class="badge ok">Temperature OK</span>','<span class="badge bad">TOO HIGH for '+p.name+'</span>','<span class="badge bad">TOO LOW for '+p.name+'</span>'];
 $('tverdict').innerHTML=tv[s.tstatus];
 $('trange').textContent=p.tmin+' - '+p.tmax+' Â°C';
 $('rain').innerHTML=s.rain<0?'unknown':s.rain+' %'+(s.sim.rain?' <span class="simc">(simulated)</span>':'');
 $('rainverdict').innerHTML=s.rainblock?'<span class="badge warn">BLOCKED - rain will water it</span>':'<span class="badge ok">allowed</span>';
 $('soil').textContent=s.soil.toFixed(0)+' %'+(s.sim.soil?' *':'');
 $('soilbar').style.width=s.soil+'%';$('m1').style.left=p.dry+'%';$('m2').style.left=p.wet+'%';
 $('state').innerHTML='<b class="'+(s.state==='FAULT'?'bad':'')+'">'+s.state+'</b>';
 $('pump').innerHTML=s.pump?'<span class="badge ok">ON</span>':'off';
 $('tank').innerHTML=s.tank?'<span class="badge bad">LOW'+(s.sim.tank?' (sim)':'')+'</span>':'<span class="badge ok">OK</span>';
 $('fail').textContent=s.failed+' / 3';
 $('profiles').innerHTML=s.profiles.map((q,i)=>'<button class="'+(i==s.profile?'sel':'')+'" onclick="api(\'/api/profile?id='+i+'\')">'+q.name+'</button>').join('');
 $('pinfo').textContent=p.need+': water below '+p.dry+' %, stop at '+p.wet+' %, healthy '+p.tmin+'-'+p.tmax+' Â°C';
 $('s_rain').className=s.sim.rain?'on':'';$('s_tank').className=s.sim.tank?'on':'';$('s_soil').className=s.sim.soil?'on':'';
 $('s_th').className=s.sim.temp==1?'on':'';$('s_tl').className=s.sim.temp==2?'on':'';
 $('mute').textContent=s.buzzing?'Silence buzzer':(s.muted?'Buzzer silenced':'Silence buzzer');
 $('simnote').textContent=(s.sim.rain||s.sim.tank||s.sim.soil||s.sim.temp)?'* Simulated values are active (purple buttons).':'No simulation active - real sensors in use.';
 $('log').innerHTML=s.events.map(e=>'<div>'+e+'</div>').join('')}
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

static void sendStatus() {
  float t = effectiveTemp();
  String j; j.reserve(1800);
  j += "{\"soil\":" + String(soilShown, 1);
  j += ",\"state\":\"" + String(STATE_NAME[state]) + "\"";
  j += ",\"pump\":" + String((state == WATERING && !tankLow && !rainBlock()) || primeOn ? "true" : "false");
  j += ",\"tank\":" + String(tankLow ? "true" : "false");
  j += ",\"rain\":" + String(effectiveRain());
  j += ",\"rainblock\":" + String(rainBlock() ? "true" : "false");
  j += ",\"temp\":" + (isnan(t) ? String("null") : String(t, 1));
  j += ",\"tstatus\":" + String(tempStatus());
  j += ",\"profile\":" + String(profileIdx);
  j += ",\"failed\":" + String(failedCycles);
  j += ",\"muted\":" + String(muted ? "true" : "false");
  j += ",\"buzzing\":" + String(buzzing ? "true" : "false");
  j += ",\"net\":\"" + String(apMode ? "own network PlantWater" : "Wi-Fi") + "\",\"ip\":\"" + ipText + "\"";
  j += ",\"sim\":{\"rain\":" + String(simRain ? "true" : "false") + ",\"tank\":" + String(simTank ? "true" : "false") +
       ",\"soil\":" + String(simSoil ? "true" : "false") + ",\"temp\":" + String(simTemp) + "}";
  j += ",\"profiles\":[";
  for (int i = 0; i < 3; i++) {
    if (i) j += ",";
    j += "{\"name\":\"" + String(PROFILES[i].name) + "\",\"need\":\"" + PROFILES[i].need + "\",\"dry\":" + String(PROFILES[i].dry, 0) +
         ",\"wet\":" + String(PROFILES[i].wet, 0) + ",\"tmin\":" + String(PROFILES[i].tMin, 0) + ",\"tmax\":" + String(PROFILES[i].tMax, 0) + "}";
  }
  j += "],\"events\":[";
  for (int k = 0; k < evCount; k++) {                 // newest first
    int idx = (evHead - 1 - k + 16) % 8;
    if (k) j += ",";
    j += "\""; j += events[idx]; j += "\"";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

static void handleSim() {
  String n = server.arg("name");
  int v = server.arg("val").toInt();
  if (n == "clear") { simRain = simTank = simSoil = false; simTemp = 0; logEvent("SIM: all simulations cleared"); }
  else if (n == "rain") { simRain = v; logEvent(v ? "SIM: heavy rain forecast injected" : "SIM: rain forecast cleared"); }
  else if (n == "tank") { simTank = v; logEvent(v ? "SIM: tank LOW injected" : "SIM: tank low cleared"); }
  else if (n == "soil") { simSoil = v; logEvent(v ? "SIM: soil too dry injected" : "SIM: soil dry cleared"); }
  else if (n == "temp") { simTemp = (v == 1 || v == 2) ? v : 0;
    logEvent(simTemp == 1 ? "SIM: temperature TOO HIGH injected" : simTemp == 2 ? "SIM: temperature TOO LOW injected" : "SIM: temperature cleared"); }
  server.send(200, "text/plain", "ok");
}

static void startNetwork() {
  WiFi.mode(WIFI_STA);
  if (strlen(WIFI_SSID) > 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to Wi-Fi \"%s\"", WIFI_SSID);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) { delay(300); Serial.print("."); }
    Serial.println();
  }
  if (WiFi.status() == WL_CONNECTED) {
    ipText = WiFi.localIP().toString();
    apMode = false;
  } else {
    WiFi.disconnect(); WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ipText = WiFi.softAPIP().toString();
    apMode = true;
    Serial.println("Wi-Fi unavailable: own network started (weather will be simulated only)");
  }
  if (MDNS.begin("plantwater")) Serial.println("Also reachable at http://plantwater.local");
  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", sendStatus);
  server.on("/api/sim", handleSim);
  server.on("/api/profile", []() {
    int id = server.arg("id").toInt();
    if (id >= 0 && id < 3) { profileIdx = id; char b[80]; snprintf(b, sizeof b, "Profile changed to %s", PROFILES[id].name); logEvent(b); }
    server.send(200, "text/plain", "ok");
  });
  server.on("/api/mute", []() { muted = true; logEvent("Buzzer silenced from dashboard"); server.send(200, "text/plain", "ok"); });
  server.on("/api/weather", []() { nextWeatherAt = 0; server.send(200, "text/plain", "ok"); });
  server.begin();
  Serial.printf("Dashboard: http://%s\n", ipText.c_str());
}

// ============================ Display & buttons ============================
static void updatePrime(uint32_t now) {
  static uint32_t start = 0;
  static bool held = false;
  if (digitalRead(PIN_PRIME) == LOW) {
    if (!held) {
      held = true; start = now;
      Serial.println(tankLow ? "[PRIME] refused: tank is low" : "[PRIME] manual pump (max 5 s per press)");
    }
    primeOn = !tankLow && (now - start < PRIME_MAX_MS);
  } else { held = false; primeOn = false; }
}

static void drawScreen(uint32_t now) {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);  oled.print(PROFILES[profileIdx].name);
  float t = effectiveTemp();
  oled.setCursor(84, 0);
  if (isnan(t)) oled.print("--C"); else oled.printf("%.0fC", t);
  oled.setTextSize(2);
  oled.setCursor(0, 11); oled.printf("Soil %.0f%%", soilShown);
  oled.drawRect(0, 30, 128, 9, SSD1306_WHITE);
  oled.fillRect(1, 31, (int)(126.0f * soilShown / 100.0f), 7, SSD1306_WHITE);
  oled.drawFastVLine((int)(1 + 126.0f * curDry() / 100.0f), 40, 3, SSD1306_WHITE);
  oled.drawFastVLine((int)(1 + 126.0f * curWet() / 100.0f), 40, 3, SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 45);
  bool pumpOn = (state == WATERING && !tankLow && !rainBlock()) || primeOn;
  oled.printf("%s  Pump:%s", STATE_NAME[state], pumpOn ? "ON" : "off");
  oled.setCursor(0, 56);
  uint8_t ts = tempStatus();
  if (state == FAULT)        oled.print("FAULT - press EN");
  else if (tankLow)          oled.print("TANK LOW - REFILL");
  else if (rainBlock())      oled.print("RAIN - WATERING OFF");
  else if (ts == 2)          oled.print("TEMP TOO HIGH!");
  else if (ts == 3)          oled.print("TEMP TOO LOW!");
  else if (noResponse)       oled.print("CHECK PUMP / TUBE");
  else if (state == SOAKING) oled.printf("Tank OK  soak %lus", (unsigned long)((SOAK_MS - (now - stateSince)) / 1000));
  else                       oled.print("Tank OK");
  oled.display();
}

// ============================ Sampling & control ===========================
static void sampleAndControl(uint32_t now) {
  static uint32_t lastLog = 0;
  static bool lastTankLow = false, lastRainBlock = false;

  soilRaw  = readAveraged(PIN_SOIL, 16);
  levelRaw = readAveraged(PIN_LEVEL, 8);
  soilReal = soilPercentFromRaw(soilRaw);
  soilShown = simSoil ? max(0.0f, curDry() - 10.0f) : soilReal;     // "soil too dry" simulation

  if (levelRaw < LEVEL_LOW_RAW) realTankLow = true;
  else if (levelRaw > LEVEL_LOW_RAW + LEVEL_HYST_RAW) realTankLow = false;
  tankLow = realTankLow || simTank;
  if (tankLow != lastTankLow) {
    logEvent(tankLow ? "TANK LOW - pump blocked, refill" : "Tank OK again");
    lastTankLow = tankLow;
  }
  bool rb = rainBlock();
  if (rb != lastRainBlock) {
    logEvent(rb ? "Rain expected - automatic watering blocked" : "No rain expected - watering allowed");
    lastRainBlock = rb;
  }

  switch (state) {
    case IDLE:
      if (soilShown < curDry() && !tankLow && !rb) { soilAtStart = soilShown; setState(WATERING, "soil dry"); }
      break;

    case WATERING:
      if (tankLow) {
        evalPending = false; setState(IDLE, "tank low - pump stopped");
      } else if (rb) {
        evalPending = false; setState(IDLE, "rain expected - pump stopped");
      } else if (soilShown >= curWet()) {
        failedCycles = 0; noResponse = false; evalPending = false; cycles++;
        setState(SOAKING, "target moisture reached");
      } else if (now - stateSince >= MAX_RUN_MS) {
        evalPending = !simSoil;                // a simulated reading cannot prove or disprove the pump
        cycles++;
        setState(SOAKING, "max run time reached");
      }
      break;

    case SOAKING:
      if (now - stateSince >= SOAK_MS) {
        if (evalPending) {
          evalPending = false;
          if (soilShown - soilAtStart < MIN_RISE_PCT) {
            failedCycles++; noResponse = true;
            char b[80]; snprintf(b, sizeof b, "Soil did not respond (%d/%d): check pump, tube, probe", failedCycles, MAX_FAILED_CYCLES);
            logEvent(b);
          } else { failedCycles = 0; noResponse = false; }
        }
        if (failedCycles >= MAX_FAILED_CYCLES) setState(FAULT, "too many failed cycles - press EN after fixing");
        else setState(IDLE, "soak finished");
      }
      break;

    case FAULT:
      break;
  }

  drawScreen(now);

  if (now - lastLog >= LOG_MS) {
    lastLog = now;
    Serial.printf("[SOIL] %.0f%% (raw %d) tank=%s (raw %d) state=%s pump=%s rain=%d%% temp=%.1f profile=%s sim[r%d t%d s%d T%d]\n",
                  soilShown, soilRaw, tankLow ? "LOW" : "ok", levelRaw, STATE_NAME[state],
                  ((state == WATERING && !tankLow && !rb) || primeOn) ? "ON" : "off",
                  effectiveRain(), effectiveTemp(), PROFILES[profileIdx].name, simRain, simTank, simSoil, simTemp);
  }
}

static void updateAlert(uint32_t now) {
  uint8_t ts = tempStatus();
  if (ts != lastTempStatus) {
    muted = false;
    if (ts == 2) logEvent("ALERT: temperature too high for this plant");
    else if (ts == 3) logEvent("ALERT: temperature too low for this plant");
    else if (lastTempStatus >= 2) logEvent("Temperature back in the healthy range");
    lastTempStatus = ts;
  }
  bool want = false;
  if ((ts == 2 || ts == 3) && !muted) {
    uint32_t ph = now % 2000;
    if (ts == 2) want = (ph < 150) || (ph >= 300 && ph < 450);   // two short beeps = too hot
    else         want = (ph < 700);                              // one long beep  = too cold
  }
  buzzing = (ts == 2 || ts == 3) && !muted;
  buzzerWrite(want);
}

void setup() {
  relayWrite(false);                           // pump OFF first
  pinMode(PIN_BUZZER, OUTPUT); buzzerWrite(false);
  pinMode(PIN_PRIME, INPUT_PULLUP);
  Serial.begin(115200);
  analogReadResolution(12);
  Wire.begin(PIN_SDA, PIN_SCL);
  oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!oledOk) Serial.println("OLED not found - continuing without it");
  Serial.println("\n=== Automatic Plant Watering + Dashboard - ENGR 4399 Project 1 ===");
  if (oledOk) {
    oled.clearDisplay(); oled.setTextColor(SSD1306_WHITE); oled.setTextSize(1);
    oled.setCursor(0, 0); oled.print("Starting Wi-Fi..."); oled.display();
  }
  startNetwork();
  logEvent("System started");
  if (oledOk) {
    oled.clearDisplay(); oled.setCursor(0, 0); oled.print("Dashboard:");
    oled.setCursor(0, 14); oled.print(ipText); oled.setCursor(0, 30);
    oled.print(apMode ? "Wi-Fi: PlantWater" : "plantwater.local"); oled.display();
    delay(2500);
  }
  nextWeatherAt = millis() + 1000;
}

void loop() {
  static uint32_t lastSample = 0;
  static int lastRelay = -1;
  uint32_t now = millis();

  server.handleClient();
  updatePrime(now);
  if (now - lastSample >= SAMPLE_MS) { lastSample = now; sampleAndControl(now); }

  // Weather is fetched only while the pump is off (the fetch can block for a few seconds)
  bool pumpBusy = (state == WATERING) || primeOn;
  if (!pumpBusy && !apMode && (int32_t)(now - nextWeatherAt) >= 0) fetchWeather();

  updateAlert(now);

  int wantOn = ((state == WATERING && !tankLow && !rainBlock()) || primeOn) ? 1 : 0;
  if (wantOn != lastRelay) { relayWrite(wantOn); lastRelay = wantOn; }
}
