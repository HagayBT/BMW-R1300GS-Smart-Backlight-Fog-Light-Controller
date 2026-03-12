// ============================================================
//  EasierCan.ino  —  BMW R1300GS CAN Bus Controller
//  Features: Knight Rider startup, brake light,
//            wave sequential turn signals,
//            MOSFET fog lights (on when engine running >10s,
//            blink with active turn signal),
//            strobe mode (triple high-beam press → 5s rapid flash)
// ============================================================

#include <Adafruit_NeoPixel.h>
#include "driver/twai.h"

// ==========================================
// HARDWARE PINS
// ==========================================
#define RX_PIN        GPIO_NUM_9   // D6
#define TX_PIN        GPIO_NUM_7   // D4
#define LED_PIN_LEFT  6            // D3
#define LED_PIN_RIGHT 8            // D5
#define NUM_LEDS      9

// MOSFET output pins
#define FOG_LEFT_PIN  A5           // GPIO 12
#define FOG_RIGHT_PIN A1           // GPIO 2
#define AIR_HORN_PIN  A3           // GPIO 4

// ==========================================
// CAN BUS IDs
// ==========================================
// 0x10C — BMSK engine control module, 10ms cycle
//   RPM  = (data[2] * 256 + data[1]) / 4
//   data[0] = throttle position
//   data[3] = clutch / kill switch (high nibble)
//   data[4] = side stand (high nibble)
#define ID_BMSK      0x10C
#define ID_HIGH_BEAM 0x2D0
#define ID_SWITCHES  0x2D2

// ==========================================
// TIMING CONSTANTS
// ==========================================
// Fog lights come on this many ms after engine starts running
#define FOG_ENGINE_DELAY_MS  10000UL
// RPM threshold for "engine running"
#define ENGINE_RPM_THRESHOLD  300

// Strobe: 3 high-beam presses within this window triggers strobe
#define STROBE_WINDOW_MS  3000UL
// How long strobe runs
#define STROBE_DURATION_MS 5000UL
// Strobe blink period (50% duty cycle)
#define STROBE_PERIOD_MS    100UL

// ==========================================
// WAVE SEQUENTIAL INDICATOR TIMING
// ==========================================
#define WAVE_STEP_MS   80    // ms per LED during fill
#define WAVE_HOLD_MS  160    // ms hold all on
#define WAVE_PAUSE_MS 320    // ms all off before next cycle

// ==========================================
// STATE
// ==========================================
bool stateLeft     = false;
bool stateBrake    = false;
bool stateRight    = false;
bool stateHighBeam = false;
bool stateEngine   = false;

uint16_t      engineRpm        = 0;
unsigned long engineStartTime  = 0;

// Strobe state
bool          strobeActive     = false;
unsigned long strobeEndTime    = 0;
int           hbPressCount     = 0;
unsigned long firstHBPressTime = 0;

unsigned long lastDebugPrint         = 0;
unsigned int  messagesReceivedInCycle = 0;

Adafruit_NeoPixel ledsL(NUM_LEDS, LED_PIN_LEFT,  NEO_GRBW + NEO_KHZ800);
Adafruit_NeoPixel ledsR(NUM_LEDS, LED_PIN_RIGHT, NEO_GRBW + NEO_KHZ800);

// ==========================================
// SETUP
// ==========================================
void setup() {
  // Hold CAN TX HIGH before anything else
  pinMode(TX_PIN, OUTPUT);
  digitalWrite(TX_PIN, HIGH);

  delay(2000);  // DC-DC converter stabilization

  // CAN FIRST — must be running before NeoPixel show() calls
  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
      (gpio_num_t)TX_PIN, (gpio_num_t)RX_PIN, TWAI_MODE_LISTEN_ONLY);
  twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  twai_driver_install(&g_config, &t_config, &f_config);
  twai_start();

  Serial.begin(115200);
  Serial.println("========================================");
  Serial.println("      BMW R1300GS  —  EasierCan          ");
  Serial.println("  Fogs on 15s timer, blink with signal   ");
  Serial.println("========================================");

  // MOSFET output pins — all OFF at startup
  pinMode(FOG_LEFT_PIN,  OUTPUT);  digitalWrite(FOG_LEFT_PIN,  LOW);
  pinMode(FOG_RIGHT_PIN, OUTPUT);  digitalWrite(FOG_RIGHT_PIN, LOW);
  pinMode(AIR_HORN_PIN,  OUTPUT);  digitalWrite(AIR_HORN_PIN,  LOW);

  // NeoPixels after CAN
  ledsL.begin();
  ledsR.begin();
  ledsL.setBrightness(100);
  ledsR.setBrightness(100);
  ledsL.clear();
  ledsR.clear();
  ledsL.show();
  ledsR.show();

  knightRiderStartup();
}

// ==========================================
// STARTUP: KNIGHT RIDER ANIMATION
// ==========================================
void knightRiderStartup() {
  const uint8_t trail[]   = { 255, 110, 35, 10 };
  const int     TRAIL_LEN = 4;
  const int     STEP_MS   = 38;
  const int     PASSES    = 4;

  for (int pass = 0; pass < PASSES; pass++) {
    bool forward = (pass % 2 == 0);
    int  start   = forward ? 0 : NUM_LEDS - 1;
    int  end_pos = forward ? NUM_LEDS : -1;
    int  dir     = forward ? 1 : -1;

    for (int pos = start; pos != end_pos; pos += dir) {
      ledsL.clear();
      ledsR.clear();
      for (int t = 0; t < TRAIL_LEN; t++) {
        int idx = pos - (dir * t);
        if (idx >= 0 && idx < NUM_LEDS) {
          uint32_t c = ledsL.Color(trail[t], 0, 0, 0);
          ledsL.setPixelColor(idx, c);
          ledsR.setPixelColor(idx, c);
        }
      }
      ledsL.show();
      ledsR.show();
      delay(STEP_MS);
    }
  }

  for (int b = 100; b >= 0; b -= 5) {
    ledsL.setBrightness(b);
    ledsR.setBrightness(b);
    ledsL.show();
    ledsR.show();
    delay(18);
  }

  ledsL.clear();  ledsR.clear();
  ledsL.setBrightness(100);
  ledsR.setBrightness(100);
  ledsL.show();   ledsR.show();
  delay(200);
}

// ==========================================
// WAVE SEQUENTIAL INDICATOR
// ==========================================
void drawWaveIndicator(Adafruit_NeoPixel &strip, bool active) {
  const int IND_COUNT = NUM_LEDS - 3;  // 6 LEDs (indices 3–8)

  if (!active) {
    for (int i = 3; i < NUM_LEDS; i++) strip.setPixelColor(i, 0);
    return;
  }

  const unsigned long CYCLE = (unsigned long)(IND_COUNT * WAVE_STEP_MS)
                              + WAVE_HOLD_MS + WAVE_PAUSE_MS;
  unsigned long phase = millis() % CYCLE;

  int litCount;
  if (phase < (unsigned long)(IND_COUNT * WAVE_STEP_MS)) {
    litCount = (int)(phase / WAVE_STEP_MS) + 1;
  } else if (phase < (unsigned long)(IND_COUNT * WAVE_STEP_MS + WAVE_HOLD_MS)) {
    litCount = IND_COUNT;
  } else {
    litCount = 0;
  }

  for (int i = 3; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, (i < 3 + litCount) ? strip.Color(255, 50, 0, 0) : 0);
  }
}

// ==========================================
// APPLY LED LOGIC
// ==========================================
void applyLogic() {
  unsigned long now = millis();

  // Tail LEDs: dim red always, bright red on brake
  uint32_t tailColor = stateBrake ? ledsL.Color(255, 0, 0, 0)
                                  : ledsL.Color(60, 0, 0, 0);
  for (int i = 0; i < 3; i++) {
    ledsL.setPixelColor(i, tailColor);
    ledsR.setPixelColor(i, tailColor);
  }

  // Strobe timeout check
  if (strobeActive && now >= strobeEndTime) {
    strobeActive = false;
    Serial.println(">>> Strobe finished.");
  }

  // Fog / strobe logic  (strobe > turn-blink > steady)
  bool fogBaseOn = stateEngine && (now - engineStartTime >= FOG_ENGINE_DELAY_MS);
  bool fogLeft, fogRight;
  if (strobeActive) {
    // Rapid 10 Hz blink on both fogs regardless of turn signal
    bool strobeOn = (now % STROBE_PERIOD_MS) < (STROBE_PERIOD_MS / 2);
    fogLeft  = strobeOn;
    fogRight = strobeOn;
  } else {
    fogLeft  = (fogBaseOn && stateLeft)  ? (now % 800) < 400 : fogBaseOn;
    fogRight = (fogBaseOn && stateRight) ? (now % 800) < 400 : fogBaseOn;
  }
  digitalWrite(FOG_LEFT_PIN,  fogLeft  ? HIGH : LOW);
  digitalWrite(FOG_RIGHT_PIN, fogRight ? HIGH : LOW);

  // Wave sequential turn signals
  drawWaveIndicator(ledsL, stateLeft);
  drawWaveIndicator(ledsR, stateRight);

  static unsigned long lastLedUpdate = 0;
  if (now - lastLedUpdate > 20) {
    ledsL.show();
    ledsR.show();
    lastLedUpdate = now;
  }

  // Diagnostics heartbeat every 2 seconds
  if (now - lastDebugPrint > 2000) {
    twai_status_info_t si;
    twai_get_status_info(&si);

    Serial.println("\n--- DIAGNOSTIC REPORT ---");
    Serial.print("CAN Messages/2s: ");
    Serial.print(messagesReceivedInCycle);
    Serial.println(messagesReceivedInCycle == 0 ? "  [DEAD — check wiring]" : "  [OK]");

    Serial.print("RX Errors: ");
    Serial.println(si.rx_error_counter);

    Serial.print("State: ");
    Serial.println(si.state == TWAI_STATE_RUNNING ? "RUNNING [OK]" :
                   si.state == TWAI_STATE_BUS_OFF  ? "BUS OFF [ERROR]" : "UNKNOWN");

    Serial.print("RPM:");     Serial.print(engineRpm);
    Serial.print("  Engine:"); Serial.print(stateEngine ? "ON" : "off");
    Serial.print("  Fogs:");  Serial.print((stateEngine && (now - engineStartTime >= FOG_ENGINE_DELAY_MS)) ? "ON" : "off");
    Serial.print("  Strobe:"); Serial.print(strobeActive ? "ON" : "off");
    Serial.print("  Brake:"); Serial.print(stateBrake   ? "ON" : "off");
    Serial.print("  L:");     Serial.print(stateLeft    ? "ON" : "off");
    Serial.print("  R:");     Serial.println(stateRight ? "ON" : "off");
    Serial.println("-------------------------");

    messagesReceivedInCycle = 0;
    lastDebugPrint = now;
  }
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  twai_message_t message;

  if (twai_receive(&message, pdMS_TO_TICKS(1)) == ESP_OK) {
    messagesReceivedInCycle++;
    unsigned long now = millis();

    // ENGINE RPM — BMSK module 0x10C, 10 ms cycle
    // RPM = (data[2] * 256 + data[1]) / 4
    if (message.identifier == ID_BMSK) {
      engineRpm = ((uint16_t)message.data[2] * 256 + message.data[1]) / 4;
      bool running = (engineRpm > ENGINE_RPM_THRESHOLD);
      if (running && !stateEngine) {
        engineStartTime = now;
        Serial.println(">>> Engine RUNNING — fog timer started");
      } else if (!running && stateEngine) {
        Serial.println(">>> Engine STOPPED");
      }
      stateEngine = running;
    }

    // HIGH BEAM — triple press within STROBE_WINDOW_MS triggers strobe
    if (message.identifier == ID_HIGH_BEAM) {
      bool currentHB = ((message.data[6] & 0x10) == 0);
      if (currentHB && !stateHighBeam) {          // rising edge only
        Serial.println(">>> High Beam click");
        if (now - firstHBPressTime > STROBE_WINDOW_MS) {
          // First press or window expired — reset counter
          hbPressCount     = 1;
          firstHBPressTime = now;
        } else {
          hbPressCount++;
          if (hbPressCount >= 3) {
            strobeActive  = true;
            strobeEndTime = now + STROBE_DURATION_MS;
            hbPressCount  = 0;
            Serial.println(">>> STROBE TRIGGERED — 5 seconds");
          }
        }
      }
      stateHighBeam = currentHB;
    }

    if (message.identifier == ID_SWITCHES) {
      uint8_t d0 = message.data[0];

      bool currentLeft = (d0 & 0x04);
      if (currentLeft != stateLeft) {
        stateLeft = currentLeft;
        Serial.print(">>> Left Signal ");
        Serial.println(stateLeft ? "ON" : "OFF");
      }

      bool currentRight = (d0 & 0x08);
      if (currentRight != stateRight) {
        stateRight = currentRight;
        Serial.print(">>> Right Signal ");
        Serial.println(stateRight ? "ON" : "OFF");
      }

      bool currentBrake = (d0 & 0x20);
      if (currentBrake != stateBrake) {
        stateBrake = currentBrake;
        Serial.print(">>> Brake ");
        Serial.println(stateBrake ? "ON" : "OFF");
      }
    }
  }

  applyLogic();
}
