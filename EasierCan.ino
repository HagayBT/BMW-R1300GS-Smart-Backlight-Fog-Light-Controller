// ============================================================
//  EasierCan.ino  —  BMW R1300GS CAN Bus Controller  v2.0
//  Arduino Nano ESP32 (ESP32-S3)
//
//  NEW in v2: BLE configuration + NVS persistence
//
//  Required libraries (Arduino Library Manager):
//    • Adafruit NeoPixel
//    • ArduinoBLE
//    • ArduinoJson  (>= 6.21)
// ============================================================

#include <Adafruit_NeoPixel.h>
#include "driver/twai.h"
#include <ArduinoBLE.h>
#include <Preferences.h>
#include <ArduinoJson.h>

// ──────────────────────────────────────────────────────────
// HARDWARE PINS  — Arduino Nano ESP32
// ──────────────────────────────────────────────────────────
#define RX_PIN        GPIO_NUM_9   // D6
#define TX_PIN        GPIO_NUM_7   // D4
#define LED_PIN_LEFT  6            // D3
#define LED_PIN_RIGHT 8            // D5
#define FOG_LEFT_PIN  A5           // GPIO 12
#define FOG_RIGHT_PIN A1           // GPIO 2
#define AIR_HORN_PIN  A3           // GPIO 4

// ──────────────────────────────────────────────────────────
// BLE
// ──────────────────────────────────────────────────────────
#define BLE_DEVICE_NAME  "EasierCan"
#define BLE_SERVICE_UUID "FFE0"
#define BLE_CHAR_UUID    "FFE1"

// ──────────────────────────────────────────────────────────
// CAN IDs
// ──────────────────────────────────────────────────────────
// 0x10C — BMSK engine module, 10ms cycle
//   RPM = (data[2]*256 + data[1]) / 4
#define ID_BMSK      0x10C
#define ID_HIGH_BEAM 0x2D0
#define ID_SWITCHES  0x2D2

// ──────────────────────────────────────────────────────────
// CONFIG — lookup tables (indices must match HTML dropdowns)
// ──────────────────────────────────────────────────────────
// FUNCTIONS: 0=None 1=LED Strip 2=Fog Light Left 3=Fog Light Right
//            4=Air Horn 5=Accessories 6=Brake Light
// EVENTS:    0=None 1=Brake 2=Signal Left 3=Signal Right 4=High Beam
//            5=Engine Running 6=Power On 7=Hazard 8=High Beam x3 9=Horn
// ACTIONS:   0=None 1=On 2=Off 3=Wave 4=Strobe 5=Blink

const char* FUNC_NAMES[] = {
  "None","LED Strip","Fog Light Left","Fog Light Right",
  "Air Horn","Accessories","Brake Light"
};
const char* TRIG_NAMES[] = {
  "None","Brake","Signal Left","Signal Right","High Beam",
  "Engine Running","Power On","Hazard","High Beam x3","Horn"
};
const char* ACT_NAMES[] = { "None","On","Off","Wave","Strobe","Blink" };

uint8_t nameToIdx(const char** table, uint8_t tableLen, const char* name) {
  for (uint8_t i = 0; i < tableLen; i++)
    if (strcmp(table[i], name) == 0) return i;
  return 0;
}
#define FUNC_IDX(n) nameToIdx(FUNC_NAMES, 7, n)
#define TRIG_IDX(n) nameToIdx(TRIG_NAMES, 10, n)
#define ACT_IDX(n)  nameToIdx(ACT_NAMES,  6, n)

// ──────────────────────────────────────────────────────────
// CONFIG STRUCT
// ──────────────────────────────────────────────────────────
struct EventRule  { uint8_t trigger, action, delaySec; };
struct OutputConf { uint8_t func; EventRule ev1, ev2; };

struct Cfg {
  uint8_t    startupSeq;
  OutputConf outputs[5];
  uint8_t    numLeds;
  uint8_t    numBrakeLeds;
  uint16_t   engineRpmThr;
  float      strobeWindowSec;
  float      strobeDurSec;
  float      strobePeriodSec;
  uint16_t   waveStepMs;
  uint16_t   waveHoldMs;
  uint16_t   wavePauseMs;
  char       pin[5];        // 4-digit BLE PIN, null-terminated (default "0000")
} cfg;

void cfgSetDefaults() {
  cfg.startupSeq      = 2;   // Night Rider

  // LED Left Strip  — Brake→On  |  Signal Left→Wave
  cfg.outputs[0] = { 1, {1,1,0}, {2,3,0} };
  // LED Right Strip — Brake→On  |  Signal Right→Wave
  cfg.outputs[1] = { 1, {1,1,0}, {3,3,0} };
  // 12v Output 1 (Fog Light Left)  — Engine Running→On(8s) | HBx3→Strobe
  cfg.outputs[2] = { 2, {5,1,8}, {8,4,0} };
  // 12v Output 2 (Accessories)     — Power On→On(16s) | —
  cfg.outputs[3] = { 5, {6,1,16}, {0,0,0} };
  // 12v Output 3 (Fog Light Right) — Engine Running→On(8s) | HBx3→Strobe
  cfg.outputs[4] = { 3, {5,1,8}, {8,4,0} };

  cfg.numLeds          = 9;
  cfg.numBrakeLeds     = 3;
  cfg.engineRpmThr     = 50;
  cfg.strobeWindowSec  = 2.0;
  cfg.strobeDurSec     = 2.0;
  cfg.strobePeriodSec  = 0.1;
  cfg.waveStepMs       = 80;
  cfg.waveHoldMs       = 160;
  cfg.wavePauseMs      = 320;
  strncpy(cfg.pin, "0000", 5);
}

// ──────────────────────────────────────────────────────────
// NVS
// ──────────────────────────────────────────────────────────
Preferences prefs;

void cfgSave() {
  prefs.begin("ec", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  Serial.println("[NVS] Config saved.");
}

bool cfgLoad() {
  prefs.begin("ec", true);
  bool ok = prefs.isKey("cfg") &&
            (prefs.getBytesLength("cfg") == sizeof(cfg));
  if (ok) prefs.getBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
  return ok;
}

// ──────────────────────────────────────────────────────────
// BLE OBJECTS
// ──────────────────────────────────────────────────────────
BLEService        bleService(BLE_SERVICE_UUID);
BLECharacteristic bleChar(BLE_CHAR_UUID,
  BLERead | BLEWriteWithoutResponse | BLENotify, 512);

String bleBuf = "";   // buffers partial BLE writes until '\n'

// ──────────────────────────────────────────────────────────
// NEOPIXELS  (length updated from cfg after NVS load)
// ──────────────────────────────────────────────────────────
Adafruit_NeoPixel ledsL(9, LED_PIN_LEFT,  NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel ledsR(9, LED_PIN_RIGHT, NEO_GRBW + NEO_KHZ800);

// ──────────────────────────────────────────────────────────
// STATE
// ──────────────────────────────────────────────────────────
bool          stateLeft       = false;
bool          stateBrake      = false;
bool          stateRight      = false;
bool          stateHighBeam   = false;
bool          stateEngine     = false;
bool          stateHorn       = false;
uint16_t      engineRpm       = 0;
unsigned long engineStartTime = 0;

bool          strobeActive     = false;
unsigned long strobeEndTime    = 0;
int           hbPressCount     = 0;
unsigned long firstHBPressTime = 0;

// Per-output, per-event delay tracking (5 outputs × 2 events)
bool          prevTrigState[5][2]  = {};
unsigned long trigActiveAt[5][2]   = {};

bool          bleAuthenticated     = false;  // true after correct PIN sent

unsigned long lastDebugPrint        = 0;
unsigned int  messagesReceivedInCycle = 0;

// ──────────────────────────────────────────────────────────
// BLE SEND HELPERS
// ──────────────────────────────────────────────────────────
void sendBLE(const String& data) {
  if (!BLE.connected()) return;
  String payload = data + "\n";
  // Send in 500-byte chunks so large JSON clears BLE MTU limits
  const int CHUNK = 500;
  int pos = 0, len = payload.length();
  while (pos < len) {
    int sz = min(CHUNK, len - pos);
    bleChar.writeValue((uint8_t*)(payload.c_str() + pos), sz);
    pos += sz;
    if (pos < len) delay(30);
  }
}

void sendOutputsConfig() {
  String s = "{\"cmd\":\"outputs\",\"config\":[";
  for (int i = 0; i < 5; i++) {
    if (i) s += ",";
    s += "{\"func\":\"";  s += FUNC_NAMES[cfg.outputs[i].func];         s += "\"";
    s += ",\"ev1\":\"";   s += TRIG_NAMES[cfg.outputs[i].ev1.trigger];  s += "\"";
    s += ",\"a1\":\"";    s += ACT_NAMES [cfg.outputs[i].ev1.action];   s += "\"";
    s += ",\"d1\":";      s += cfg.outputs[i].ev1.delaySec;
    s += ",\"ev2\":\"";   s += TRIG_NAMES[cfg.outputs[i].ev2.trigger];  s += "\"";
    s += ",\"a2\":\"";    s += ACT_NAMES [cfg.outputs[i].ev2.action];   s += "\"";
    s += ",\"d2\":";      s += cfg.outputs[i].ev2.delaySec;
    s += "}";
  }
  s += "]}";
  sendBLE(s);
}

void sendAdvancedConfig() {
  String s = "{\"cmd\":\"advanced\",\"vars\":{";
  s += "\"NUM_LEDS\":"             + String(cfg.numLeds)        + ",";
  s += "\"NUM_BRAKE_LEDS\":"       + String(cfg.numBrakeLeds)   + ",";
  s += "\"ENGINE_RPM_THRESHOLD\":" + String(cfg.engineRpmThr)   + ",";
  s += "\"STROBE_WINDOW_MS\":"     + String(cfg.strobeWindowSec)+ ",";
  s += "\"STROBE_DURATION_MS\":"   + String(cfg.strobeDurSec)   + ",";
  s += "\"STROBE_PERIOD_MS\":"     + String(cfg.strobePeriodSec)+ ",";
  s += "\"WAVE_STEP_MS\":"         + String(cfg.waveStepMs)     + ",";
  s += "\"WAVE_HOLD_MS\":"         + String(cfg.waveHoldMs)     + ",";
  s += "\"WAVE_PAUSE_MS\":"        + String(cfg.wavePauseMs);
  s += "}}";
  sendBLE(s);
}

// ──────────────────────────────────────────────────────────
// BLE COMMAND HANDLER
// ──────────────────────────────────────────────────────────
void testOutputEvent(uint8_t outIdx, uint8_t evNum) {
  if (outIdx < 2) {
    // LED outputs — brief white flash
    for (int i = 0; i < cfg.numLeds; i++) {
      ledsL.setPixelColor(i, ledsL.Color(255, 255, 255, 0));
      ledsR.setPixelColor(i, ledsR.Color(255, 255, 255, 0));
    }
    ledsL.show(); ledsR.show();
    delay(500);
    ledsL.clear(); ledsR.clear();
    ledsL.show();  ledsR.show();
  } else {
    // 12v outputs — 500 ms pulse on the relevant pin
    uint8_t pin = (outIdx == 2) ? FOG_LEFT_PIN :
                  (outIdx == 3) ? AIR_HORN_PIN  : FOG_RIGHT_PIN;
    digitalWrite(pin, HIGH);
    delay(500);
    digitalWrite(pin, LOW);
  }
}

void handleBleCommand(const String& json) {
  // The outputs-config JSON is ~450 bytes; ArduinoJson copies all strings from
  // an Arduino String, so the pool must be ~2× the raw JSON size.
  // StaticJsonDocument<1024> was too small → NoMemory → cfg never updated.
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print("[BLE] JSON parse ERROR: ");
    Serial.print(err.c_str());
    Serial.print("  (json len=");
    Serial.print(json.length());
    Serial.println(")");
    return;
  }

  const char* cmd = doc["cmd"] | "";
  Serial.print("[BLE] cmd="); Serial.println(cmd);

  // ── auth — must be handled before the PIN gate ──────────
  if (strcmp(cmd, "auth") == 0) {
    const char* p = doc["pin"] | "";
    bool ok = (strlen(p) == 4 && strcmp(p, cfg.pin) == 0);
    bleAuthenticated = ok;
    sendBLE(ok ? "{\"cmd\":\"auth_result\",\"ok\":true}"
               : "{\"cmd\":\"auth_result\",\"ok\":false}");
    Serial.println(ok ? "[BLE] Authenticated." : "[BLE] Auth FAILED — wrong PIN.");
    return;
  }

  // ── PIN gate — reject every other command until authenticated
  if (!bleAuthenticated) {
    sendBLE("{\"cmd\":\"auth_result\",\"ok\":false}");
    Serial.println("[BLE] Command rejected — not authenticated.");
    return;
  }

  // ── startup ────────────────────────────────────────────
  if (strcmp(cmd, "startup") == 0) {
    cfg.startupSeq = doc["value"] | cfg.startupSeq;
    cfgSave();
  }
  // ── test_startup ───────────────────────────────────────
  else if (strcmp(cmd, "test_startup") == 0) {
    runStartupAnimation(doc["value"] | 2);
  }
  // ── outputs config ─────────────────────────────────────
  else if (strcmp(cmd, "outputs") == 0) {
    JsonArray arr = doc["config"].as<JsonArray>();
    for (int i = 0; i < 5 && i < (int)arr.size(); i++) {
      JsonObject o = arr[i];
      cfg.outputs[i].func         = FUNC_IDX(o["func"] | "None");
      cfg.outputs[i].ev1.trigger  = TRIG_IDX(o["ev1"]  | "None");
      cfg.outputs[i].ev1.action   = ACT_IDX (o["a1"]   | "None");
      cfg.outputs[i].ev1.delaySec = o["d1"] | 0;
      cfg.outputs[i].ev2.trigger  = TRIG_IDX(o["ev2"]  | "None");
      cfg.outputs[i].ev2.action   = ACT_IDX (o["a2"]   | "None");
      cfg.outputs[i].ev2.delaySec = o["d2"] | 0;
    }
    cfgSave();
    sendOutputsConfig();   // echo back so UI confirms
  }
  // ── advanced config ────────────────────────────────────
  else if (strcmp(cmd, "advanced") == 0) {
    JsonObject v = doc["vars"].as<JsonObject>();
    cfg.numLeds         = v["NUM_LEDS"]             | cfg.numLeds;
    cfg.numBrakeLeds    = v["NUM_BRAKE_LEDS"]        | cfg.numBrakeLeds;
    cfg.engineRpmThr    = v["ENGINE_RPM_THRESHOLD"]  | cfg.engineRpmThr;
    cfg.strobeWindowSec = v["STROBE_WINDOW_MS"]      | cfg.strobeWindowSec;
    cfg.strobeDurSec    = v["STROBE_DURATION_MS"]    | cfg.strobeDurSec;
    cfg.strobePeriodSec = v["STROBE_PERIOD_MS"]      | cfg.strobePeriodSec;
    cfg.waveStepMs      = v["WAVE_STEP_MS"]          | cfg.waveStepMs;
    cfg.waveHoldMs      = v["WAVE_HOLD_MS"]          | cfg.waveHoldMs;
    cfg.wavePauseMs     = v["WAVE_PAUSE_MS"]         | cfg.wavePauseMs;
    ledsL.updateLength(cfg.numLeds);
    ledsR.updateLength(cfg.numLeds);
    cfgSave();
    sendAdvancedConfig();
  }
  // ── read requests ──────────────────────────────────────
  else if (strcmp(cmd, "read_outputs")  == 0) { sendOutputsConfig();  }
  else if (strcmp(cmd, "read_advanced") == 0) { sendAdvancedConfig(); }
  else if (strcmp(cmd, "read_startup")  == 0) {
    sendBLE("{\"cmd\":\"startup_cfg\",\"value\":" + String(cfg.startupSeq) + "}");
  }
  // ── test event ─────────────────────────────────────────
  else if (strcmp(cmd, "test_event") == 0) {
    testOutputEvent(doc["output"] | 0, doc["event"] | 1);
  }
  // ── change PIN ─────────────────────────────────────────
  else if (strcmp(cmd, "change_pin") == 0) {
    const char* p = doc["pin"] | "";
    bool valid = (strlen(p) == 4
                  && isdigit((uint8_t)p[0]) && isdigit((uint8_t)p[1])
                  && isdigit((uint8_t)p[2]) && isdigit((uint8_t)p[3]));
    if (valid) {
      strncpy(cfg.pin, p, 5);
      cfgSave();
      sendBLE("{\"cmd\":\"pin_changed\",\"ok\":true}");
      Serial.print("[BLE] PIN changed to "); Serial.println(cfg.pin);
    } else {
      sendBLE("{\"cmd\":\"pin_changed\",\"ok\":false}");
    }
  }
  // ── sim — test events from HTML panel ──────────────────
  else if (strcmp(cmd, "sim") == 0) {
    const char* ev  = doc["event"] | "";
    bool        sta = doc["state"]  | false;
    unsigned long now = millis();
    unsigned long strobeWindowMs = (unsigned long)(cfg.strobeWindowSec * 1000.0f);
    unsigned long strobeDurMs    = (unsigned long)(cfg.strobeDurSec    * 1000.0f);

    if (strcmp(ev, "power_on") == 0) {
      if (sta) runStartupAnimation(cfg.startupSeq);
    }
    else if (strcmp(ev, "engine") == 0) {
      if (sta && !stateEngine) { engineStartTime = now; Serial.println(">>> [SIM] Engine RUNNING"); }
      else if (!sta && stateEngine)  Serial.println(">>> [SIM] Engine STOPPED");
      stateEngine = sta;
    }
    else if (strcmp(ev, "high_beam") == 0) {
      if (sta && !stateHighBeam) {
        Serial.println(">>> [SIM] High Beam click");
        if (now - firstHBPressTime > strobeWindowMs) {
          hbPressCount = 1; firstHBPressTime = now;
        } else {
          hbPressCount++;
          if (hbPressCount >= 3) {
            strobeActive = true; strobeEndTime = now + strobeDurMs;
            hbPressCount = 0; Serial.println(">>> [SIM] STROBE TRIGGERED");
          }
        }
      }
      stateHighBeam = sta;
    }
    else if (strcmp(ev, "brake")        == 0) { stateBrake  = sta; Serial.print(">>> [SIM] Brake ");         Serial.println(sta ? "ON" : "OFF"); }
    else if (strcmp(ev, "signal_left")  == 0) { stateLeft   = sta; Serial.print(">>> [SIM] Signal Left ");   Serial.println(sta ? "ON" : "OFF"); }
    else if (strcmp(ev, "signal_right") == 0) { stateRight  = sta; Serial.print(">>> [SIM] Signal Right ");  Serial.println(sta ? "ON" : "OFF"); }
    else if (strcmp(ev, "hazard")       == 0) { stateLeft = stateRight = sta; Serial.print(">>> [SIM] Hazard "); Serial.println(sta ? "ON" : "OFF"); }
    else if (strcmp(ev, "horn")         == 0) { stateHorn = sta; Serial.print(">>> [SIM] Horn "); Serial.println(sta ? "ON" : "OFF"); }
  }
}

void processBleWrites() {
  if (!bleChar.written()) return;
  int len = bleChar.valueLength();
  if (len <= 0) return;

  // Accumulate into buffer — full command ends with '\n'
  for (int i = 0; i < len; i++)
    bleBuf += (char)bleChar.value()[i];

  int nl;
  while ((nl = bleBuf.indexOf('\n')) >= 0) {
    String line = bleBuf.substring(0, nl);
    bleBuf = bleBuf.substring(nl + 1);
    line.trim();
    if (line.length()) handleBleCommand(line);
  }
}

// ──────────────────────────────────────────────────────────
// SETUP
// ──────────────────────────────────────────────────────────
void setup() {
  // 1. Hold CAN TX HIGH before anything else
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, HIGH);

  // 2. DC-DC converter stabilization
  delay(2000);

  // 3. CAN FIRST — must be running before NeoPixel show() calls
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();

  // 4. Serial
  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("     BMW R1300GS  —  EasierCan  v2      ");
  Serial.println("========================================");

  // 5. Load config from NVS (or apply defaults)
  cfgSetDefaults();
  if (cfgLoad()) {
    Serial.println("[NVS] Config loaded.");
  } else {
    Serial.println("[NVS] No saved config — using defaults.");
  }

  // 6. MOSFET output pins — all OFF
  pinMode(FOG_LEFT_PIN,  OUTPUT);  digitalWrite(FOG_LEFT_PIN,  LOW);
  pinMode(FOG_RIGHT_PIN, OUTPUT);  digitalWrite(FOG_RIGHT_PIN, LOW);
  pinMode(AIR_HORN_PIN,  OUTPUT);  digitalWrite(AIR_HORN_PIN,  LOW);

  // 7. NeoPixels (length from cfg)
  ledsL.updateLength(cfg.numLeds);
  ledsR.updateLength(cfg.numLeds);
  ledsL.begin();  ledsR.begin();
  ledsL.setBrightness(100);  ledsR.setBrightness(100);
  ledsL.clear();  ledsR.clear();
  ledsL.show();   ledsR.show();

  // 8. Startup animation
  randomSeed(analogRead(0));
  runStartupAnimation(cfg.startupSeq);

  // 9. BLE — after animation so CAN is stable
  if (!BLE.begin()) {
    Serial.println("[BLE] Failed to start — continuing without BLE.");
  } else {
    BLE.setLocalName(BLE_DEVICE_NAME);
    BLE.setAdvertisedService(bleService);
    bleService.addCharacteristic(bleChar);
    BLE.addService(bleService);
    BLE.advertise();
    Serial.println("[BLE] Advertising as \"" BLE_DEVICE_NAME "\"");
  }
}

// ──────────────────────────────────────────────────────────
// STARTUP ANIMATION SHARED HELPERS
// ──────────────────────────────────────────────────────────
static inline void aSH()         { ledsL.show();  ledsR.show(); }
static inline void aBR(uint8_t b){ ledsL.setBrightness(b); ledsR.setBrightness(b); }
static inline void aCL()         { ledsL.clear(); ledsR.clear(); aSH(); }
static inline uint32_t aRGB (uint8_t r,uint8_t g,uint8_t b)
  { return ledsL.Color(r,g,b,0); }
static inline uint32_t aRGBW(uint8_t r,uint8_t g,uint8_t b,uint8_t w)
  { return ledsL.Color(r,g,b,w); }
static inline void aSetAll(uint32_t c) {
  for (int i=0;i<(int)cfg.numLeds;i++){ledsL.setPixelColor(i,c);ledsR.setPixelColor(i,c);}
}
static inline void aSetPx(int i, uint32_t c) {
  if (i>=0 && i<(int)cfg.numLeds){ledsL.setPixelColor(i,c);ledsR.setPixelColor(i,c);}
}
// Scale a packed WRGB colour by 0-255 without touching setBrightness()
static inline uint32_t aScale(uint32_t c, uint8_t s) {
  return ledsL.Color(
    ((c >> 16) & 0xFF) * s / 255,
    ((c >>  8) & 0xFF) * s / 255,
    ( c        & 0xFF) * s / 255,
    ((c >> 24) & 0xFF) * s / 255);
}
static inline void aFadeAll(uint32_t base, uint8_t s) { aSetAll(aScale(base,s)); aSH(); }

// ──────────────────────────────────────────────────────────
// 2 — NIGHT RIDER  (original Knight Rider scanner)
// ──────────────────────────────────────────────────────────
void startupNightRider() {
  const uint8_t trail[]   = { 255, 110, 35, 10 };
  const int     TRAIL_LEN = 4;
  const int     STEP_MS   = 38;
  const int     PASSES    = 4;
  const int     N         = cfg.numLeds;

  for (int pass = 0; pass < PASSES; pass++) {
    bool forward = (pass % 2 == 0);
    int  start   = forward ? 0 : N - 1;
    int  end_pos = forward ? N : -1;
    int  dir     = forward ? 1 : -1;

    for (int pos = start; pos != end_pos; pos += dir) {
      ledsL.clear(); ledsR.clear();
      for (int t = 0; t < TRAIL_LEN; t++) {
        int idx = pos - (dir * t);
        if (idx >= 0 && idx < N) {
          uint32_t c = ledsL.Color(trail[t], 0, 0, 0);
          ledsL.setPixelColor(idx, c);
          ledsR.setPixelColor(idx, c);
        }
      }
      ledsL.show(); ledsR.show();
      delay(STEP_MS);
    }
  }

  for (int b = 100; b >= 0; b -= 5) {
    ledsL.setBrightness(b); ledsR.setBrightness(b);
    ledsL.show(); ledsR.show();
    delay(18);
  }

  ledsL.clear(); ledsR.clear();
  ledsL.setBrightness(100); ledsR.setBrightness(100);
  ledsL.show(); ledsR.show();
  delay(200);
}

// ──────────────────────────────────────────────────────────
// 3 — SPLIT / BMW SWEEP
// Two comets sweep outward from inner edge (0→N-1) × 2 passes,
// warm-white head with amber trail, both strips simultaneously.
// ──────────────────────────────────────────────────────────
void startupSplit() {
  const int N = (int)cfg.numLeds;
  const uint8_t TR[] = { 255, 180, 90, 30, 8 };
  const int TRL = 5;
  for (int pass = 0; pass < 2; pass++) {
    bool fwd = (pass % 2 == 0);
    for (int step = 0; step < N + TRL; step++) {
      ledsL.clear(); ledsR.clear();
      for (int t = 0; t < TRL; t++) {
        int idx = fwd ? (step - t) : (N - 1 - step + t);
        aSetPx(idx, aRGBW(TR[t], TR[t]/5, 0, TR[t]/2));
      }
      aSH(); delay(40);
    }
  }
  for (int s=255;s>=0;s-=10){ aFadeAll(aRGBW(180,36,0,90),s); delay(12); }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 4 — CONVERGE & EXPLODE
// Two amber heads race from both ends toward center,
// then burst white three times.
// ──────────────────────────────────────────────────────────
void startupConverge() {
  const int N = (int)cfg.numLeds;
  const uint8_t TR[] = { 255, 120, 40 };
  for (int step = 0; step <= N/2 + 1; step++) {
    ledsL.clear(); ledsR.clear();
    int posA = step, posB = N - 1 - step;
    for (int t = 0; t < 3; t++) {
      uint32_t c = aRGB(TR[t], TR[t]/4, 0);
      aSetPx(posA - t,  c);
      aSetPx(posB + t,  c);
    }
    aSH(); delay(52);
  }
  for (int f = 0; f < 3; f++) {
    aSetAll(aRGBW(0,0,0,255)); aSH(); delay(70);
    aCL(); delay(40);
  }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 5 — HEARTBEAT
// Classic lub-dub × 3, deep red.
// ──────────────────────────────────────────────────────────
void startupHeartbeat() {
  const uint32_t RED = aRGB(220, 0, 0);
  for (int beat = 0; beat < 3; beat++) {
    for (int s=0;   s<=190; s+=10){ aFadeAll(RED,s); delay(5);  }  // lub  ↑
    for (int s=190; s>=50;  s-=10){ aFadeAll(RED,s); delay(5);  }  // lub  ↓
    for (int s=50;  s<=255; s+=10){ aFadeAll(RED,s); delay(5);  }  // DUB  ↑
    for (int s=255; s>=0;   s-=5) { aFadeAll(RED,s); delay(6);  }  // DUB  ↓
    delay(440);
  }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 6 — COMET / METEOR
// Warm-white head + long amber-to-red decaying tail, 4 passes,
// alternating direction, each pass slightly slower.
// ──────────────────────────────────────────────────────────
void startupComet() {
  const int N = (int)cfg.numLeds;
  const uint8_t TR[] = { 255, 200, 130, 70, 30, 10 };
  const int TRL = 6;
  for (int pass = 0; pass < 4; pass++) {
    bool fwd = (pass % 2 == 0);
    int ms = 26 + pass * 9;
    for (int step = 0; step < N + TRL; step++) {
      ledsL.clear(); ledsR.clear();
      for (int t = 0; t < TRL; t++) {
        int idx = fwd ? (step - t) : (N - 1 - step + t);
        uint32_t c = (t == 0)
          ? aRGBW(TR[0]/2, TR[0]/4, 0, TR[0])   // warm-white head
          : aRGB(TR[t], TR[t]/3, 0);              // amber→red tail
        aSetPx(idx, c);
      }
      aSH(); delay(ms);
    }
  }
  for (int s=255;s>=0;s-=13){ aFadeAll(aRGBW(127,32,0,127),s); delay(11); }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 7 — STACK (TETRIS)
// White-headed LEDs shoot from pos 0 and land one-by-one,
// building a red stack at the far end, then fade out.
// ──────────────────────────────────────────────────────────
void startupStack() {
  const int N = (int)cfg.numLeds;
  for (int s = 0; s < N; s++) {
    int target = N - 1 - s;
    for (int pos = 0; pos <= target; pos++) {
      ledsL.clear(); ledsR.clear();
      for (int k = N-1; k > target; k--)         // stacked reds
        aSetPx(k, aRGB(180, 0, 0));
      aSetPx(pos,     aRGBW(160, 80, 0, 140));   // moving head
      aSetPx(pos - 1, aRGB(70, 28, 0));           // trail
      aSH(); delay(28);
    }
    delay(22);
  }
  delay(220);
  for (int s=255;s>=0;s-=13){ aFadeAll(aRGB(180,0,0),s); delay(14); }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 8 — TWINKLE FADE-IN
// Random warm-white sparkles gradually solidify into red.
// ──────────────────────────────────────────────────────────
void startupTwinkle() {
  const int N = (int)cfg.numLeds;
  for (int frame = 0; frame < 50; frame++) {    // 50 × 40 ms = 2 s
    ledsL.clear(); ledsR.clear();
    for (int i = 0; i < N; i++) {
      if (random(100) < 35) {
        uint8_t w = random(60, 255);
        uint32_t c = aRGBW(w/3, w/6, 0, w);
        ledsL.setPixelColor(i, c); ledsR.setPixelColor(i, c);
      }
    }
    aSH(); delay(40);
  }
  for (int r = 0; r <= 220; r += 5) { aSetAll(aRGB(r,0,0)); aSH(); delay(16); }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 9 — THUNDER
// Five random-gap white flashes then bloom to dim red.
// ──────────────────────────────────────────────────────────
void startupThunder() {
  for (int f = 0; f < 5; f++) {
    delay(random(40, 260));
    aSetAll(aRGBW(0,0,0,255)); aSH(); delay(random(15, 65));
    aCL();
  }
  for (int r = 0; r <= 60; r += 3) { aSetAll(aRGB(r,0,0)); aSH(); delay(18); }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 10 — BREATHE / PULSE
// Slow sine-like red fade-in / fade-out × 3 cycles.
// ──────────────────────────────────────────────────────────
void startupBreathe() {
  const uint32_t RED = aRGB(255, 0, 0);
  for (int breath = 0; breath < 3; breath++) {
    for (int s=0;   s<=220; s+=4){ aFadeAll(RED,s); delay(11); }  // inhale
    delay(180);
    for (int s=220; s>=0;   s-=4){ aFadeAll(RED,s); delay(11); }  // exhale
    delay(280);
  }
  aCL();
}

// ──────────────────────────────────────────────────────────
// 11 — PING PONG
// Orange dot bounces end-to-end, accelerating each pass,
// then blurs into a rapid strobe and fades.
// ──────────────────────────────────────────────────────────
void startupPingPong() {
  const int N = (int)cfg.numLeds;
  int pos = 0, dir = 1, ms = 65;
  for (int bounce = 0; bounce < 10; bounce++) {
    while (true) {
      ledsL.clear(); ledsR.clear();
      aSetPx(pos,       aRGB(255, 120, 0));
      aSetPx(pos - dir, aRGB(80,  35,  0));
      aSH(); delay(ms);
      bool hitWall = (dir == 1 && pos >= N-1) || (dir == -1 && pos <= 0);
      if (hitWall) break;
      pos += dir;
    }
    dir = -dir;
    ms  = max(12, ms - 6);
  }
  for (int i = 0; i < 12; i++) {
    aSetAll(aRGB(220,100,0)); aSH(); delay(25);
    aCL(); delay(15);
  }
  aCL();
}

// ──────────────────────────────────────────────────────────
// DISPATCH  —  index matches HTML <select> values exactly
// ──────────────────────────────────────────────────────────
void runStartupAnimation(uint8_t seq) {
  if (seq == 1) seq = random(2, 12);   // 1 = Random — pick one of 2-11
  switch (seq) {
    case  0:                       break;   // None
    case  2: startupNightRider();  break;
    case  3: startupSplit();       break;
    case  4: startupConverge();    break;
    case  5: startupHeartbeat();   break;
    case  6: startupComet();       break;
    case  7: startupStack();       break;
    case  8: startupTwinkle();     break;
    case  9: startupThunder();     break;
    case 10: startupBreathe();     break;
    case 11: startupPingPong();    break;
    default: startupNightRider();  break;
  }
  // Safety cleanup — ensures LEDs are off and brightness restored
  ledsL.clear(); ledsR.clear();
  ledsL.setBrightness(100); ledsR.setBrightness(100);
  ledsL.show(); ledsR.show();
}

// ──────────────────────────────────────────────────────────
// WAVE SEQUENTIAL INDICATOR  (uses cfg timing)
// ──────────────────────────────────────────────────────────
void drawWaveIndicator(Adafruit_NeoPixel &strip, bool active) {
  const int brakeLeds = cfg.numBrakeLeds;
  const int indCount  = cfg.numLeds - brakeLeds;
  if (indCount <= 0) return;

  if (!active) {
    for (int i = brakeLeds; i < (int)cfg.numLeds; i++) strip.setPixelColor(i, 0);
    return;
  }

  const unsigned long CYCLE = (unsigned long)(indCount * cfg.waveStepMs)
                              + cfg.waveHoldMs + cfg.wavePauseMs;
  unsigned long phase = millis() % CYCLE;

  int litCount;
  if (phase < (unsigned long)(indCount * cfg.waveStepMs)) {
    litCount = (int)(phase / cfg.waveStepMs) + 1;
  } else if (phase < (unsigned long)(indCount * cfg.waveStepMs + cfg.waveHoldMs)) {
    litCount = indCount;
  } else {
    litCount = 0;
  }

  for (int i = brakeLeds; i < (int)cfg.numLeds; i++)
    strip.setPixelColor(i, (i < brakeLeds + litCount) ? strip.Color(255, 50, 0, 0) : 0);
}

// ──────────────────────────────────────────────────────────
// BLINK INDICATOR  (standard turn signal ~1.5 Hz, 50 % duty)
// ──────────────────────────────────────────────────────────
void drawBlinkIndicator(Adafruit_NeoPixel &strip, bool active) {
  const int brakeLeds = cfg.numBrakeLeds;
  const int indCount  = cfg.numLeds - brakeLeds;
  if (indCount <= 0) return;

  // 667 ms period → ~1.5 Hz; on for first half
  bool lit = active && ((millis() % 667) < 333);
  uint32_t col = lit ? strip.Color(255, 50, 0, 0) : 0;
  for (int i = brakeLeds; i < (int)cfg.numLeds; i++)
    strip.setPixelColor(i, col);
}

// ──────────────────────────────────────────────────────────
// TRIGGER EVALUATION
// ──────────────────────────────────────────────────────────
bool evalTrigger(uint8_t trig) {
  switch (trig) {
    case 1:  return stateBrake;
    case 2:  return stateLeft;
    case 3:  return stateRight;
    case 4:  return stateHighBeam;
    case 5:  return stateEngine;
    case 6:  return true;                    // Power On — always active
    case 7:  return stateLeft && stateRight; // Hazard
    case 8:  return strobeActive;            // High Beam x3
    case 9:  return stateHorn;
    default: return false;                   // None or unknown
  }
}

// Returns true once trigger has been active for ≥ delaySec seconds.
// Tracks rising-edge timestamp per output/event slot.
bool evalTriggerWithDelay(int outIdx, int evIdx, uint8_t trigId,
                          uint8_t delaySec, unsigned long now) {
  if (trigId == 0) { prevTrigState[outIdx][evIdx] = false; return false; }
  bool raw = evalTrigger(trigId);
  if (raw && !prevTrigState[outIdx][evIdx])
    trigActiveAt[outIdx][evIdx] = now;           // latch activation time
  prevTrigState[outIdx][evIdx] = raw;
  if (!raw) return false;
  return (now - trigActiveAt[outIdx][evIdx]) >= (unsigned long)delaySec * 1000UL;
}

// ──────────────────────────────────────────────────────────
// ACTION DISPATCH — LED brake zone  (pixels 0 … brakeLeds-1)
// Base is always dim-red tail light; action overrides on event.
// ──────────────────────────────────────────────────────────
void applyLedBrakeAction(Adafruit_NeoPixel& strip, uint8_t action, bool active,
                         unsigned long strobePeriodMs, unsigned long now) {
  const int     n      = cfg.numBrakeLeds;
  const uint32_t TAIL   = strip.Color(60,  0, 0, 0);  // dim red — tail light
  const uint32_t BRIGHT = strip.Color(255, 0, 0, 0);  // full red — brake
  uint32_t col;
  switch (action) {
    case 0:   col = TAIL; break;                       // None — just tail
    case 1:   col = active ? BRIGHT : TAIL; break;     // On — bright when active
    case 2:   col = 0;    break;                       // Off — dark
    case 3:   col = active ? BRIGHT : TAIL; break;     // Wave — treat as On
    case 4: { bool on = active && strobePeriodMs > 0 &&
                   ((now % strobePeriodMs) < (strobePeriodMs / 2));
              col = on ? strip.Color(255, 255, 255, 255) : TAIL; break; } // Strobe
    case 5: { bool on = active && ((now % 667) < 333);
              col = on ? BRIGHT : TAIL; break; }       // Blink ~1.5 Hz
    default:  col = TAIL; break;
  }
  for (int i = 0; i < n; i++) strip.setPixelColor(i, col);
}

// ──────────────────────────────────────────────────────────
// ACTION DISPATCH — LED indicator zone  (pixels brakeLeds … numLeds-1)
// ──────────────────────────────────────────────────────────
void applyLedIndicatorAction(Adafruit_NeoPixel& strip, uint8_t action, bool active,
                             unsigned long strobePeriodMs, unsigned long now) {
  const int br = cfg.numBrakeLeds;
  const int n  = cfg.numLeds;
  switch (action) {
    case 0:  // None / Off — clear indicator zone
    case 2:
      for (int i = br; i < n; i++) strip.setPixelColor(i, 0);
      break;
    case 1:  // On — solid amber while active
      { uint32_t col = active ? strip.Color(255, 50, 0, 0) : 0;
        for (int i = br; i < n; i++) strip.setPixelColor(i, col); }
      break;
    case 3:  // Wave sequential
      drawWaveIndicator(strip, active);
      break;
    case 4:  // Strobe — bright white at strobePeriodSec rate
      { bool on = active && strobePeriodMs > 0 &&
                  ((now % strobePeriodMs) < (strobePeriodMs / 2));
        uint32_t col = on ? strip.Color(255, 255, 255, 255) : 0;
        for (int i = br; i < n; i++) strip.setPixelColor(i, col); }
      break;
    case 5:  // Blink ~1.5 Hz
      drawBlinkIndicator(strip, active);
      break;
  }
}

// ──────────────────────────────────────────────────────────
// ACTION DISPATCH — MOSFET / 12 V output
// ──────────────────────────────────────────────────────────
void applyMosfetAction(uint8_t pin, uint8_t action, bool active,
                       unsigned long strobePeriodMs, unsigned long now) {
  bool out;
  switch (action) {
    case 1:  // On
    case 3:  // Wave — treat as On for a digital output
      out = active; break;
    case 4:  // Strobe
      out = active && strobePeriodMs > 0 &&
            ((now % strobePeriodMs) < (strobePeriodMs / 2));
      break;
    case 5:  // Blink ~1.5 Hz
      out = active && ((now % 667) < 333);
      break;
    default: // None (0) or Off (2)
      out = false; break;
  }
  digitalWrite(pin, out ? HIGH : LOW);
}

// ──────────────────────────────────────────────────────────
// APPLY LOGIC  (all timing from cfg)
// ──────────────────────────────────────────────────────────
void applyLogic() {
  unsigned long now            = millis();
  unsigned long strobePeriodMs = (unsigned long)(cfg.strobePeriodSec * 1000.0f);
  unsigned long strobeDurMs    = (unsigned long)(cfg.strobeDurSec    * 1000.0f);

  // ── Strobe timeout ──────────────────────────────────────────
  if (strobeActive && now >= strobeEndTime) {
    strobeActive = false;
    Serial.println(">>> Strobe finished.");
  }

  // ── Output dispatch ──────────────────────────────────────────
  // LED strips (0,1): ev1 → brake zone, ev2 → indicator zone (independent)
  // MOSFETs  (2,3,4): ev2 > ev1 priority, mapped to FOG_LEFT / AIR_HORN / FOG_RIGHT
  static const uint8_t MOSFET_PINS[3] = { FOG_LEFT_PIN, AIR_HORN_PIN, FOG_RIGHT_PIN };

  for (int i = 0; i < 5; i++) {
    const OutputConf& oc = cfg.outputs[i];

    bool ev1ok = evalTriggerWithDelay(i, 0, oc.ev1.trigger, oc.ev1.delaySec, now);
    bool ev2ok = evalTriggerWithDelay(i, 1, oc.ev2.trigger, oc.ev2.delaySec, now);

    if (i < 2) {
      // LED strip — two zones driven independently
      Adafruit_NeoPixel& strip = (i == 0) ? ledsL : ledsR;
      applyLedBrakeAction   (strip, oc.ev1.action, ev1ok, strobePeriodMs, now);
      applyLedIndicatorAction(strip, oc.ev2.action, ev2ok, strobePeriodMs, now);
    } else {
      // MOSFET — ev2 overrides ev1
      uint8_t action    = 0;
      bool    anyActive = false;
      if      (ev2ok && oc.ev2.action != 0) { action = oc.ev2.action; anyActive = true; }
      else if (ev1ok && oc.ev1.action != 0) { action = oc.ev1.action; anyActive = true; }
      applyMosfetAction(MOSFET_PINS[i - 2], action, anyActive, strobePeriodMs, now);
    }
  }

  // ── Show LEDs ────────────────────────────────────────────────
  static unsigned long lastLedUpdate = 0;
  if (now - lastLedUpdate > 20) {
    ledsL.show(); ledsR.show();
    lastLedUpdate = now;
  }

  // ── Diagnostics every 2 s ────────────────────────────────────
  if (now - lastDebugPrint > 2000) {
    twai_status_info_t si;
    twai_get_status_info(&si);
    Serial.println("\n--- DIAGNOSTIC ---");
    Serial.print("CAN msg/2s: "); Serial.println(messagesReceivedInCycle);
    Serial.print("RX errors:  "); Serial.println(si.rx_error_counter);
    Serial.print("CAN state:  "); Serial.println(
      si.state == TWAI_STATE_RUNNING ? "RUNNING" :
      si.state == TWAI_STATE_BUS_OFF  ? "BUS OFF" : "UNKNOWN");
    Serial.print("RPM: ");     Serial.print(engineRpm);
    Serial.print("  Eng: ");   Serial.print(stateEngine  ? "ON" : "off");
    Serial.print("  Str: ");   Serial.print(strobeActive ? "ON" : "off");
    Serial.print("  Brk: ");   Serial.print(stateBrake   ? "ON" : "off");
    Serial.print("  L: ");     Serial.print(stateLeft    ? "ON" : "off");
    Serial.print("  R: ");     Serial.print(stateRight   ? "ON" : "off");
    Serial.print("  Horn: ");  Serial.println(stateHorn  ? "ON" : "off");
    Serial.print("[BLE] ");    Serial.println(BLE.connected() ? "Connected" : "Advertising");
    Serial.println("------------------");
    messagesReceivedInCycle = 0;
    lastDebugPrint = now;
  }
}

// ──────────────────────────────────────────────────────────
// LOOP
// ──────────────────────────────────────────────────────────
void loop() {
  // Reset auth flag when the central disconnects
  static bool wasConnected = false;
  bool nowConnected = BLE.connected();
  if (wasConnected && !nowConnected) {
    bleAuthenticated = false;
    Serial.println("[BLE] Disconnected — auth reset.");
  }
  wasConnected = nowConnected;

  // Process BLE events
  BLE.poll();
  processBleWrites();

  // CAN receive
  twai_message_t message;
  if (twai_receive(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
    messagesReceivedInCycle++;
    unsigned long now = millis();

    // ENGINE RPM — 0x10C
    if (message.identifier == ID_BMSK) {
      engineRpm = ((uint16_t)message.data[2] * 256 + message.data[1]) / 4;
      bool running = (engineRpm > cfg.engineRpmThr);
      if (running && !stateEngine) {
        engineStartTime = now;
        Serial.println(">>> Engine RUNNING — fog timer started");
      } else if (!running && stateEngine) {
        Serial.println(">>> Engine STOPPED");
      }
      stateEngine = running;
    }

    // HIGH BEAM — triple press → strobe
    if (message.identifier == ID_HIGH_BEAM) {
      unsigned long strobeWindowMs = (unsigned long)(cfg.strobeWindowSec * 1000.0f);
      unsigned long strobeDurMs    = (unsigned long)(cfg.strobeDurSec    * 1000.0f);
      bool currentHB = ((message.data[6] & 0x10) == 0);
      if (currentHB && !stateHighBeam) {
        Serial.println(">>> High Beam click");
        if (now - firstHBPressTime > strobeWindowMs) {
          hbPressCount = 1; firstHBPressTime = now;
        } else {
          hbPressCount++;
          if (hbPressCount >= 3) {
            strobeActive  = true;
            strobeEndTime = now + strobeDurMs;
            hbPressCount  = 0;
            Serial.println(">>> STROBE TRIGGERED");
          }
        }
      }
      stateHighBeam = currentHB;
    }

    // SWITCHES — turn signals & brake
    if (message.identifier == ID_SWITCHES) {
      uint8_t d0 = message.data[0];

      bool l = (d0 & 0x04);
      if (l != stateLeft)  { stateLeft  = l; Serial.print(">>> Left Signal ");  Serial.println(l ? "ON":"OFF"); }

      bool r = (d0 & 0x08);
      if (r != stateRight) { stateRight = r; Serial.print(">>> Right Signal "); Serial.println(r ? "ON":"OFF"); }

      bool b = (d0 & 0x20);
      if (b != stateBrake) { stateBrake = b; Serial.print(">>> Brake ");        Serial.println(b ? "ON":"OFF"); }
    }
  }

  applyLogic();
}
