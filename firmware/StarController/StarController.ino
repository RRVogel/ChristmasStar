// StarController.ino - LED Christmas star + tree controller
// Board: Adafruit QT Py ESP32-C3 (Arduino-ESP32 core 3.x)
// Parts: NeoPixel Driver BFF (5645) on A3, I2C QT Rotary Encoder (4991) on
//        STEMMA QT, SPH0645 I2S MEMS mic (3421) on A0/A1/A2.
// Chain: box -> BFF -> 4560 strand (50, GRB, up the tree) -> 11x 1734 8 mm
//        star NeoPixels (RGB). Index 0 is at the box, index 60 is the last spike.
//
// Controls:  turn = brightness (sensitivity in Sound rainbow / Sound white star, speed in Color chaser)
//            click = next mode
//            hold ~1 s, then let go = power on/off
//            hold ~10 s = Wi-Fi setup portal (knob LED blinks blue)
//            hold button while plugging in = setup mode (max current, star trim)
//            phone: http://christmasstar.local on her home Wi-Fi (same as the knob)
//
// Compile: Board "Adafruit QT Py ESP32-C3", USB CDC On Boot Enabled,
//          Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)"
//          (two OTA slots; the default 1.2MB slots are too small once Wi-Fi
//          and the TLS certificate bundle are linked).
// FQBN:    esp32:esp32:adafruit_qtpy_esp32c3:CDCOnBoot=cdc,PartitionScheme=min_spiffs

#include <Adafruit_NeoPixel.h>
#include <Adafruit_seesaw.h>
#include <seesaw_neopixel.h>
#include <Preferences.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <NetworkClientSecure.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <stdlib.h>
#include <esp_ota_ops.h>
#include <esp_task_wdt.h>
#include <ESP_I2S.h>
#include "StarWebPage.h"
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

#define BUILD_TAG "2026-11-24i london window"

// ---------------- Build options ----------------
#define TEST_MODE 0              // 1 = color/chain-order test, no normal modes
#define MIC_DEBUG 0              // 1 = print mic RMS / level every 2 s
#define ENABLE_ARDUINO_OTA 0     // 1 = also allow passworded LAN uploads

// Pull-OTA: bump this when you publish a new .bin, and put the same number
// in manifest.json. The star updates itself if the manifest version is higher.
const uint16_t FW_VERSION = 5;
const char *UPDATE_MANIFEST_URL =
    "https://raw.githubusercontent.com/RRVogel/ChristmasStar/main/manifest.json";

// Captive portal (iPhone Wi-Fi setup). Password must be 8+ characters (WPA2).
const char *AP_SSID = "Christmas Star Setup";
const char *AP_PASS = "starlight";
const char *HOSTNAME = "christmasstar";   // http://christmasstar.local

// Optional LAN OTA password, used only if ENABLE_ARDUINO_OTA is 1.
const char *LAN_OTA_PASSWORD = "changeme";

// ---------------- Pins ----------------
const int PIXEL_PIN    = A3;     // NeoPixel BFF default data pin (GPIO0 on QT Py C3)
const int MIC_BCLK_PIN = A0;     // SPH0645 BCLK  (GPIO4)
const int MIC_WS_PIN   = A2;     // SPH0645 LRCL/WS (GPIO1)
const int MIC_DATA_PIN = A1;     // SPH0645 DOUT  (GPIO3)
// SEL to GND = left channel (false). Set true if SEL is left floating or high.
const bool MIC_I2S_RIGHT = false;
// Philips I2S is the usual SPH0645-safe config. Set true to try left-justified
// MSB if the bench levels look like noise or stay at zero with a live mic.
const bool MIC_I2S_MSB = false;

// ---------------- Pixel layout ----------------
// STAR_FIRST is derived from TREE_COUNT (50 on the full chain, so the star
// starts at pixel 50). For a single 8 mm LED bench test, set TREE_COUNT to 0
// and STAR_COUNT to 1. STAR_FIRST becomes 0, so STAR_RGB_SWAP applies to
// pixel 0. Put them back to 50 and 11 for the full chain.
const int TREE_COUNT = 49;                       // 4560 strand (one pixel cut off), GRB
const int STAR_COUNT = 11;                       // 1734 8 mm, RGB
const int NUM_PIXELS = TREE_COUNT + STAR_COUNT;  // 61
const int TREE_FIRST = 0;
const int TREE_LAST  = TREE_COUNT - 1;           // 49; -1 if TREE_COUNT is 0
const int STAR_FIRST = TREE_COUNT;               // 50, or 0 in a bench test
const int STAR_LAST  = NUM_PIXELS - 1;           // 60
// Star LED order, looking from the bottom of the star:
//   1-5  clockwise around ring 1
//   6-10 counter-clockwise around ring 2, starting at the position
//        nearest clockwise from where ring 1 started
//   11   the top tip
// Pixel index is STAR_FIRST + (LED number - 1).
const bool STAR_RGB_SWAP = true;  // star LEDs are RGB on a GRB strip. If the
                                  // test shows pixel 50 green, set this false.

// ---------------- Encoder (seesaw) ----------------
const uint8_t SEESAW_ADDR    = 0x36;
const uint8_t SS_SWITCH_PIN  = 24;
const uint8_t SS_NEOPIX_PIN  = 6;
const bool    ENCODER_REVERSE = true;   // flip if clockwise makes it dimmer
const uint8_t ENC_LED_LEVEL  = 20;      // brightness of the knob's status LED

// ---------------- Power / current limit ----------------
const float MA_PER_CHANNEL_FULL = 12.0f;  // mA per R/G/B channel at 255
const float MA_IDLE_PER_PIXEL   = 1.0f;   // quiescent draw per pixel
const uint16_t CURRENT_CHOICES[3] = {1000, 1500, 1800};  // mA; BFF JST ~2 A max
const uint8_t DEFAULT_CURRENT_IDX = 2;    // 1800 mA, for a 2 A charger
const uint8_t DEFAULT_STAR_TRIM   = 100;  // % of the star's own scale (10-100)
const uint8_t STAR_BRIGHT_FLOOR   = 230;  // star stays ~90% even when the knob dims the tree
const uint8_t DEFAULT_BRIGHTNESS  = 128;  // 0-255 knob value
const uint8_t BRIGHT_STEP         = 24;   // knob step per detent (~10 clicks dim to full)
const uint8_t MIN_BRIGHTNESS      = 8;

// ---------------- Timing ----------------
const uint32_t LONG_PRESS_MS  = 1000;
const uint32_t WIFI_HOLD_MS   = 10000;    // hold this long to open Wi-Fi setup
const uint32_t DEBOUNCE_MS    = 30;
const uint32_t INPUT_POLL_MS  = 10;
const uint32_t FRAME_MS       = 20;       // ~50 fps
const uint32_t SAVE_DELAY_MS  = 3000;     // write NVS 3 s after last change
const uint32_t DIAG_MS        = 4000;   // status line every 4 s
const uint32_t PORTAL_TIMEOUT_MS = 10UL * 60UL * 1000UL;  // ~10 min
const uint32_t OTA_FIRST_CHECK_MS = 60000;
const uint32_t OTA_RETRY_MS   = 30UL * 60UL * 1000UL;     // retry inside the evening window
// Periodic checks: once per London day, at a random-ish time 17:00–23:59.
// POSIX TZ for Europe/London (GMT/BST). Boot/Wi-Fi-join check is separate (~60 s).
const char *LONDON_TZ = "GMT0BST,M3.5.0/1,M10.5.0";
const int OTA_WIN_START_MIN = 17 * 60;   // 17:00 Europe/London
const int OTA_WIN_END_MIN   = 24 * 60;   // midnight exclusive → 17:00–23:59
const uint32_t ROLLBACK_VALID_MS = 60000;
const uint32_t CONNECTED_LED_MS = 800;   // quick green flash
const uint32_t ERROR_LED_MS     = 1200;  // brief red blink (Wi-Fi lost / update failed)
const uint32_t MODE_FLASH_MS    = 450;   // star flashes the new mode color
const uint32_t RAINBOW_KNOB_MS  = 7000;  // Rainbow mode: full knob hue cycle
const uint32_t ENC_LED_MIN_MS   = 80;    // min gap between seesaw LED writes
const uint8_t  POWER_FADE_STEP  = 12;    // ~425 ms fade on power on/off
const uint32_t WDT_TIMEOUT_MS = 30000;
const size_t   API_MAX_BODY   = 256;
const uint32_t API_MIN_SET_MS = 100;     // rate-limit phone writes

// ---------------- Mic (SPH0645 I2S) ----------------
const uint32_t MIC_RATE       = 16000;
const uint16_t MIC_WINDOW     = 320;      // 20 ms at 16 kHz
const uint16_t MIC_READ_MAX   = 640;      // at most 40 ms of samples per poll
const uint8_t  MIC_SHIFT      = 16;       // 18-bit MSB-aligned in a 32-bit slot
const uint8_t  DEFAULT_SENS   = 10;       // 1-20
const float    MIC_DC_ALPHA   = 0.001f;   // ~10 Hz high-pass at 16 kHz
const float    MIC_FLOOR_MIN  = 15.0f;    // 16-bit RMS units after the shift

// ---------------- Modes ----------------
enum Mode : uint8_t {
  M_WHITE_PULSE = 0, M_STAR_WHITE = 1, M_CLASSIC = 2, M_RAINBOW = 3,
  M_TWINKLE = 4, M_SOUND = 5, M_OFF = 6,
  M_RAINBOW_TWINKLE = 7, M_WHITE_TWINKLE = 8, M_SOUND2 = 9,
  M_COLOR_CHASER = 10, MODE_COUNT = 11
};
// Off stays at index 6 so a saved Off in flash still means Off. New modes
// took the old Star-test slot and the indices after it. Click order is
// existing modes, then the new ones, then Off, then back to White pulse.
const uint8_t DEFAULT_MODE = M_STAR_WHITE;
const char* MODE_NAMES[MODE_COUNT] = {
  "White pulse", "Star white, tree color", "Classic", "Rainbow",
  "Twinkle", "Sound rainbow", "Off",
  "Star white, rainbow twinkle", "White twinkle", "Sound white star",
  "Color chaser"
};
// Knob LED color per mode (full scale, dimmed by ENC_LED_LEVEL).
// Rainbow is animated in showModeLed (and the mode-change flash); its table
// slot is unused. Purple is reserved for "updating" and is not used here.
const uint8_t MODE_LED[MODE_COUNT][3] = {
  {255, 220, 0}, {255, 255, 255}, {255, 0, 0}, {255, 200, 0},
  {0, 120, 255}, {0, 255, 0}, {0, 0, 0},
  {0, 255, 220}, {180, 220, 255}, {255, 140, 0},
  {255, 0, 120}
};
const uint8_t MODE_CLICK_NEXT[MODE_COUNT] = {
  M_STAR_WHITE, M_CLASSIC, M_RAINBOW, M_TWINKLE, M_SOUND,
  M_RAINBOW_TWINKLE, M_WHITE_PULSE, M_WHITE_TWINKLE, M_SOUND2, M_COLOR_CHASER,
  M_OFF
};
inline bool isSoundMode(uint8_t m) { return m == M_SOUND || m == M_SOUND2; }
inline bool isChaserMode(uint8_t m) { return m == M_COLOR_CHASER; }

// Color chaser speed: 1 = slow (10 s/pass), 20 = fast (1.5 s/pass).
// Default 13 is about 4.6 s. Persisted in NVS like sensitivity.
const uint8_t  CHASE_SPD_MIN     = 1;
const uint8_t  CHASE_SPD_MAX     = 20;
const uint8_t  DEFAULT_CHASE_SPD = 13;
const uint16_t CHASE_MS_SLOW     = 10000;
const uint16_t CHASE_MS_FAST     = 1500;

// Warm white
const uint8_t WW_R = 255, WW_G = 170, WW_B = 80;

// ---------------- Objects ----------------
Adafruit_NeoPixel strip(NUM_PIXELS, PIXEL_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_seesaw ss;
seesaw_NeoPixel encPix(1, SS_NEOPIX_PIN, NEO_GRB + NEO_KHZ800);
Preferences prefs;
bool seesawOK = false;

// Logical frame, true RGB, full scale before brightness/limit
uint8_t frame[NUM_PIXELS][3];

// ---------------- Settings (persisted) ----------------
struct Settings {
  uint8_t mode, bright, power, curIdx, trim, sens, chase, lastMode;
};
Settings cur, saved;
bool dirty = false;
uint32_t lastChangeMs = 0;

void markDirty() { dirty = true; lastChangeMs = millis(); }

void loadSettings() {
  prefs.begin("star", true);
  cur.mode   = prefs.getUChar("mode", DEFAULT_MODE);
  cur.bright = prefs.getUChar("bright", DEFAULT_BRIGHTNESS);
  cur.power  = prefs.getUChar("power", 1);
  cur.curIdx = prefs.getUChar("maxma", DEFAULT_CURRENT_IDX);
  cur.trim   = prefs.getUChar("trim", DEFAULT_STAR_TRIM);
  cur.sens   = prefs.getUChar("sens", DEFAULT_SENS);
  cur.chase  = prefs.getUChar("chase", DEFAULT_CHASE_SPD);
  cur.lastMode = prefs.getUChar("lastmode", DEFAULT_MODE);
  uint8_t curv = prefs.getUChar("curv", 0);
  prefs.end();
  if (cur.mode >= MODE_COUNT) cur.mode = DEFAULT_MODE;
  if (cur.curIdx > 2) cur.curIdx = DEFAULT_CURRENT_IDX;
  if (cur.trim < 10 || cur.trim > 100) cur.trim = DEFAULT_STAR_TRIM;
  if (cur.sens < 1 || cur.sens > 20) cur.sens = DEFAULT_SENS;
  if (cur.chase < CHASE_SPD_MIN || cur.chase > CHASE_SPD_MAX) cur.chase = DEFAULT_CHASE_SPD;
  if (cur.bright < MIN_BRIGHTNESS) cur.bright = MIN_BRIGHTNESS;
  if (cur.lastMode >= MODE_COUNT || cur.lastMode == M_OFF) cur.lastMode = DEFAULT_MODE;
  if (cur.mode != M_OFF) cur.lastMode = cur.mode;
  // One-time v3 migration: raise a previously saved 1000 mA default to 1800.
  // 'curv' records that this has run, so a later setup-mode choice is kept.
  if (curv < 3) {
    cur.curIdx = 2;
    prefs.begin("star", false);
    prefs.putUChar("maxma", cur.curIdx);
    prefs.putUChar("curv", 3);
    prefs.end();
    Serial.println("[nvs] current limit set to 1800 mA (v3 migration)");
  }
  saved = cur;
}

void saveSettings() {
  prefs.begin("star", false);
  if (cur.mode   != saved.mode)   prefs.putUChar("mode", cur.mode);
  if (cur.bright != saved.bright) prefs.putUChar("bright", cur.bright);
  if (cur.power  != saved.power)  prefs.putUChar("power", cur.power);
  if (cur.curIdx != saved.curIdx) prefs.putUChar("maxma", cur.curIdx);
  if (cur.trim   != saved.trim)   prefs.putUChar("trim", cur.trim);
  if (cur.sens   != saved.sens)   prefs.putUChar("sens", cur.sens);
  if (cur.chase  != saved.chase)  prefs.putUChar("chase", cur.chase);
  if (cur.lastMode != saved.lastMode) prefs.putUChar("lastmode", cur.lastMode);
  prefs.end();
  saved = cur;
  dirty = false;
  Serial.println("[nvs] settings saved");
}

void serviceSave() {
  if (dirty && millis() - lastChangeMs >= SAVE_DELAY_MS) saveSettings();
}

// ---------------- Pixel helpers ----------------
inline bool isStar(int i) { return i >= STAR_FIRST && i <= STAR_LAST; }

// Write one pixel with true RGB values; fixes color order for the star LEDs.
void setPix(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= NUM_PIXELS) return;
  if (STAR_RGB_SWAP && isStar(i)) strip.setPixelColor(i, g, r, b);
  else                            strip.setPixelColor(i, r, g, b);
}

// ---------------- Pixel output (SPI + DMA) ----------------
// The C3's RMT output gets refilled by an interrupt, and Wi-Fi interrupts can
// delay it, which shows up as random pixels flashing (worst with the setup
// portal running). Sending the pixel data through SPI with DMA is done in
// hardware, so Wi-Fi can't corrupt it. Each LED bit becomes 3 SPI bits at
// ~2.4 MHz: 1 -> 110, 0 -> 100 (about 0.41 / 0.83 us high, 1.25 us per bit).
#define PIXEL_SPI 1
const int SPI_LEAD_ZEROS  = 2;
const int SPI_RESET_ZEROS = 120;  // ~400 us low after the data (APA106 wants >50 us)
spi_device_handle_t pixSpi = NULL;
uint8_t *pixSpiBuf = NULL;
size_t pixSpiLen = 0;
bool pixSpiOK = false;

void pixelsBegin() {
#if PIXEL_SPI
  pixSpiLen = SPI_LEAD_ZEROS + (size_t)NUM_PIXELS * 3 * 3 + SPI_RESET_ZEROS;
  pixSpiBuf = (uint8_t *)heap_caps_calloc(1, pixSpiLen, MALLOC_CAP_DMA);
  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIXEL_PIN;
  bus.miso_io_num = -1;
  bus.sclk_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = pixSpiLen;
  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 2400000;
  dev.mode = 0;
  dev.spics_io_num = -1;
  dev.queue_size = 1;
  esp_err_t e1 = pixSpiBuf ? spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) : ESP_ERR_NO_MEM;
  esp_err_t e2 = (e1 == ESP_OK) ? spi_bus_add_device(SPI2_HOST, &dev, &pixSpi) : e1;
  pixSpiOK = (e2 == ESP_OK);
  Serial.printf("[pix] SPI DMA output %s (err %d/%d)\n", pixSpiOK ? "OK" : "FAILED, using RMT", e1, e2);
#endif
  if (!pixSpiOK) strip.begin();
}

void pixelsShow() {
  if (!pixSpiOK) { strip.show(); return; }
  const uint8_t *src = strip.getPixels();   // already in GRB wire order
  size_t n = (size_t)NUM_PIXELS * 3;
  uint8_t *d = pixSpiBuf + SPI_LEAD_ZEROS;
  for (size_t i = 0; i < n; i++) {
    uint8_t v = src[i];
    uint32_t bits = 0;
    for (int b = 7; b >= 0; b--) bits = (bits << 3) | ((v >> b) & 1 ? 0b110 : 0b100);
    *d++ = bits >> 16; *d++ = bits >> 8; *d++ = bits;
  }
  spi_transaction_t t = {};
  t.length = pixSpiLen * 8;
  t.tx_buffer = pixSpiBuf;
  spi_device_transmit(pixSpi, &t);
}

void setFrame(int i, uint8_t r, uint8_t g, uint8_t b) {
  if (i < 0 || i >= NUM_PIXELS) return;
  frame[i][0] = r; frame[i][1] = g; frame[i][2] = b;
}
void fillFrame(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < NUM_PIXELS; i++) setFrame(i, r, g, b);
}
void unpack(uint32_t c, uint8_t &r, uint8_t &g, uint8_t &b) {
  r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF;
}
uint8_t scale8(uint8_t v, uint8_t s) { return (uint16_t)v * s / 255; }

float lastEstMa = 0, lastRawMa = 0;

// Apply brightness, star trim, current limit, then show.
// The star stays near full (max(knob, STAR_BRIGHT_FLOOR), then trim) even
// when the knob dims the tree. `level` already includes the power-off and
// OTA fades, so the star fades with them; level == 0 leaves it off (a
// completed power fade, or a caller passing 0). Off mode itself writes a
// black frame. The current limiter then scales tree and star together if
// the estimate is over budget.
void pushFrame(uint8_t level) {
  static uint8_t out[NUM_PIXELS][3];
  uint8_t g = Adafruit_NeoPixel::gamma8(level);
  if (level > 0 && g < 2) g = 2;
  uint8_t starLevel = 0;
  if (level > 0) {
    // Color chaser runs at full brightness (knob is speed, not dim).
    uint8_t knob = isChaserMode(cur.mode) ? 255 : cur.bright;
    uint8_t src = knob > STAR_BRIGHT_FLOOR ? knob : STAR_BRIGHT_FLOOR;
    if (knob == 0) starLevel = level;
    else {
      uint16_t sl = (uint16_t)src * level / knob;
      starLevel = sl > 255 ? 255 : (uint8_t)sl;
    }
  }
  uint8_t gs = Adafruit_NeoPixel::gamma8(starLevel);
  if (starLevel > 0 && gs < 2) gs = 2;
  uint32_t chanSum = 0;
  for (int i = 0; i < NUM_PIXELS; i++) {
    uint8_t s = isStar(i) ? (uint16_t)gs * cur.trim / 100 : g;
    for (int c = 0; c < 3; c++) {
      out[i][c] = scale8(frame[i][c], s);
      chanSum += out[i][c];
    }
  }
  float idle = NUM_PIXELS * MA_IDLE_PER_PIXEL;
  float ledMa = chanSum * MA_PER_CHANNEL_FULL / 255.0f;
  lastRawMa = idle + ledMa;
  float limit = CURRENT_CHOICES[cur.curIdx];
  float k = 1.0f;
  if (lastRawMa > limit && ledMa > 0) k = (limit - idle) / ledMa;
  if (k < 0) k = 0;
  for (int i = 0; i < NUM_PIXELS; i++) {
    if (k < 1.0f)
      for (int c = 0; c < 3; c++) out[i][c] = (uint8_t)(out[i][c] * k);
    setPix(i, out[i][0], out[i][1], out[i][2]);
  }
  lastEstMa = idle + ledMa * k;
  pixelsShow();
}

void setEncLed(uint8_t r, uint8_t g, uint8_t b) {
  if (!seesawOK) return;
  // Skip unchanged colour, and rate-limit I2C writes so the seesaw stays
  // responsive to the knob. Overlay states still win in serviceEncLed;
  // the next call after ENC_LED_MIN_MS delivers any pending colour.
  static uint8_t lastR = 0, lastG = 0, lastB = 0;
  static bool haveLast = false;
  static uint32_t lastWrite = 0;
  uint32_t now = millis();
  if (haveLast && r == lastR && g == lastG && b == lastB) return;
  if (haveLast && (now - lastWrite) < ENC_LED_MIN_MS) return;
  encPix.setPixelColor(0, encPix.Color(r, g, b));
  encPix.show();
  lastR = r; lastG = g; lastB = b;
  haveLast = true;
  lastWrite = now;
}

void rainbowKnobColor(uint8_t &r, uint8_t &g, uint8_t &b, uint32_t now) {
  uint32_t phase = now % RAINBOW_KNOB_MS;
  uint16_t hue = (uint16_t)(phase * 65536UL / RAINBOW_KNOB_MS);
  unpack(Adafruit_NeoPixel::ColorHSV(hue, 255, 255), r, g, b);
}

void showModeLed() {
  if (!cur.power) { setEncLed(0, 0, 0); return; }
  uint8_t r, g, b;
  if (cur.mode == M_RAINBOW) {
    rainbowKnobColor(r, g, b, millis());
  } else {
    const uint8_t *c = MODE_LED[cur.mode];
    r = c[0]; g = c[1]; b = c[2];
  }
  setEncLed(scale8(r, ENC_LED_LEVEL), scale8(g, ENC_LED_LEVEL),
            scale8(b, ENC_LED_LEVEL));
}

// ---------------- Network status (encoder LED overlay) ----------------
enum NetLed : uint8_t {
  LED_NORMAL = 0, LED_PORTAL, LED_JUST_CONNECTED, LED_UPDATING, LED_ERROR
};
volatile uint8_t netLedState = LED_NORMAL;
volatile uint32_t netLedEventMs = 0;
volatile bool requestPortal = false;
volatile bool otaWantFade = false;
volatile uint8_t otaFade = 255;
volatile bool wifiConnected = false;
volatile bool lanOtaReady = false;

// Main-loop visual cues (star flash + power fade). Not touched by the net task.
uint8_t visFade = 255;
int8_t visFadeStep = 0;
uint32_t starFlashUntil = 0;
uint8_t starFlashR = 0, starFlashG = 0, starFlashB = 0;
bool starFlashRainbow = false;

void serviceEncLed() {
  uint32_t now = millis();
  uint8_t st = netLedState;
  if (st == LED_UPDATING) {
    setEncLed(scale8(180, 40), 0, scale8(255, 40));  // purple: updating, don't unplug
    return;
  }
  if (st == LED_PORTAL) {
    bool on = ((now / 400) % 2) == 0;
    setEncLed(0, 0, on ? scale8(255, 40) : 0);       // blue blink: portal waiting
    return;
  }
  if (st == LED_ERROR) {
    if (now - netLedEventMs < ERROR_LED_MS) {
      bool on = ((now - netLedEventMs) / 150) % 2 == 0;
      setEncLed(on ? scale8(255, 40) : 0, 0, 0);     // brief red blink
      return;
    }
    netLedState = LED_NORMAL;
  }
  if (st == LED_JUST_CONNECTED) {
    if (now - netLedEventMs < CONNECTED_LED_MS) {
      setEncLed(0, scale8(255, 40), 0);              // quick green flash
      return;
    }
    netLedState = LED_NORMAL;
  }
  showModeLed();
}

void signalConnected() {
  // A successful join often happens while the LED is still in LED_PORTAL.
  // Do not keep blinking blue after the portal has done its job. Updating
  // (purple) still wins so we never hide a firmware flash.
  if (netLedState == LED_UPDATING) return;
  netLedEventMs = millis();
  netLedState = LED_JUST_CONNECTED;
}

void signalError() {
  if (netLedState == LED_UPDATING || netLedState == LED_PORTAL) return;
  netLedEventMs = millis();
  netLedState = LED_ERROR;
}

void startPowerFade(bool turningOn) {
  if (turningOn) {
    visFade = 0;
    visFadeStep = (int8_t)POWER_FADE_STEP;
  } else {
    visFade = 255;
    visFadeStep = -(int8_t)POWER_FADE_STEP;
  }
}

void startStarModeFlash(uint8_t mode) {
  starFlashRainbow = (mode == M_RAINBOW);
  if (starFlashRainbow) {
    starFlashUntil = millis() + MODE_FLASH_MS;
    return;
  }
  const uint8_t *c = MODE_LED[mode];
  starFlashR = c[0];
  starFlashG = c[1];
  starFlashB = c[2];
  // Off mode's color is black; a dim white blink still shows on the star.
  if (starFlashR == 0 && starFlashG == 0 && starFlashB == 0) {
    starFlashR = starFlashG = starFlashB = 80;
  }
  starFlashUntil = millis() + MODE_FLASH_MS;
}

void applyStarFlash(uint32_t now) {
  if ((int32_t)(starFlashUntil - now) <= 0) return;
  if (starFlashRainbow) {
    // A rainbow fan across the star, not a solid purple (OTA's colour).
    uint32_t phase = now % RAINBOW_KNOB_MS;
    uint16_t base = (uint16_t)(phase * 65536UL / RAINBOW_KNOB_MS);
    int n = STAR_COUNT > 1 ? STAR_COUNT : 1;
    for (int i = STAR_FIRST; i <= STAR_LAST; i++) {
      uint8_t r, g, b;
      uint16_t hue = base + (uint16_t)((i - STAR_FIRST) * (65536 / n));
      unpack(Adafruit_NeoPixel::ColorHSV(hue, 255, 255), r, g, b);
      setFrame(i, r, g, b);
    }
    return;
  }
  for (int i = STAR_FIRST; i <= STAR_LAST; i++) {
    setFrame(i, starFlashR, starFlashG, starFlashB);
  }
}

void serviceVisFade() {
  if (visFadeStep == 0) return;
  int v = (int)visFade + visFadeStep;
  if (v <= 0) {
    visFade = 0;
    visFadeStep = 0;
  } else if (v >= 255) {
    visFade = 255;
    visFadeStep = 0;
  } else {
    visFade = (uint8_t)v;
  }
}

// Shared by the knob and the phone page. Current limit still happens in pushFrame.
enum SetBits : uint8_t { SET_POWER = 1, SET_BRIGHT = 2, SET_MODE = 4, SET_SENS = 8, SET_CHASE = 16 };

struct RemoteSet {
  volatile uint8_t mask;
  volatile uint8_t power, bright, mode, sens, chase;
};
RemoteSet remoteSet = {0, 0, 0, 0, 0, 0};

uint8_t modeToRestore() {
  uint8_t m = cur.lastMode;
  if (m >= MODE_COUNT || m == M_OFF) m = DEFAULT_MODE;
  return m;
}

void applyPowerFromCtl(uint8_t on) {
  on = on ? 1 : 0;
  // Off mode looks like the lights are off. Turning "on" from the phone
  // (or a long-press while stuck in Off) restores the last non-Off mode.
  bool restored = false;
  if (on && cur.mode == M_OFF) {
    cur.mode = modeToRestore();
    restored = true;
  }
  if (on == cur.power && !restored) return;
  if (on != cur.power) {
    cur.power = on;
    startPowerFade(cur.power);
  } else if (restored) {
    startPowerFade(true);
  }
  markDirty();
  if (restored)
    Serial.printf("[ctl] power ON, mode %u (%s)\n", cur.mode, MODE_NAMES[cur.mode]);
  else
    Serial.printf("[ctl] power %s\n", cur.power ? "ON" : "OFF");
}

void applyModeFromCtl(uint8_t mode) {
  if (mode >= MODE_COUNT) return;
  bool modeChanged = (cur.mode != mode);
  cur.mode = mode;
  if (mode != M_OFF) cur.lastMode = mode;
  if (!cur.power) {
    cur.power = 1;
    startPowerFade(true);
  } else if (modeChanged) {
    startStarModeFlash(cur.mode);
  } else {
    return;
  }
  markDirty();
  Serial.printf("[ctl] mode %u (%s)\n", cur.mode, MODE_NAMES[cur.mode]);
}

void applyBrightFromCtl(uint8_t b) {
  b = constrain((int)b, (int)MIN_BRIGHTNESS, 255);
  if (b == cur.bright) return;
  cur.bright = b;
  markDirty();
  Serial.printf("[ctl] brightness %u\n", cur.bright);
}

void applySensFromCtl(uint8_t s) {
  s = constrain((int)s, 1, 20);
  if (s == cur.sens) return;
  cur.sens = s;
  markDirty();
  Serial.printf("[ctl] sensitivity %u\n", cur.sens);
}

uint16_t chasePeriodMs() {
  uint8_t s = constrain((int)cur.chase, (int)CHASE_SPD_MIN, (int)CHASE_SPD_MAX);
  return (uint16_t)(CHASE_MS_SLOW - (uint32_t)(s - 1) * (CHASE_MS_SLOW - CHASE_MS_FAST) /
                    (CHASE_SPD_MAX - CHASE_SPD_MIN));
}

void applyChaseFromCtl(uint8_t s) {
  s = constrain((int)s, (int)CHASE_SPD_MIN, (int)CHASE_SPD_MAX);
  if (s == cur.chase) return;
  cur.chase = s;
  markDirty();
  uint16_t ms = chasePeriodMs();
  Serial.printf("[ctl] chaser speed %u.%u s\n", ms / 1000, (ms / 100) % 10);
}

void serviceRemoteSet() {
  uint8_t mask = remoteSet.mask;
  if (!mask) return;
  uint8_t power = remoteSet.power, bright = remoteSet.bright;
  uint8_t mode = remoteSet.mode, sens = remoteSet.sens, chase = remoteSet.chase;
  remoteSet.mask = 0;
  if (mask & SET_POWER) applyPowerFromCtl(power);
  if (mask & SET_MODE)  applyModeFromCtl(mode);
  if (mask & SET_BRIGHT) applyBrightFromCtl(bright);
  if (mask & SET_SENS)  applySensFromCtl(sens);
  if (mask & SET_CHASE) applyChaseFromCtl(chase);
}

// ---------------- Encoder / button ----------------
struct Input { int32_t delta; bool click; bool longPress; bool wifiHold; };
Input pollInput();  // explicit prototype (Arduino auto-prototypes break on struct return)
bool btnStable = false, btnRaw = false;
uint32_t btnChangeMs = 0, btnDownMs = 0;
bool longFired = false, veryLongFired = false;

bool readButton() {
  if (!seesawOK) return false;
  return !ss.digitalRead(SS_SWITCH_PIN);  // active low
}

Input pollInput() {
  Input in = {0, false, false, false};
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (!seesawOK || now - lastPoll < INPUT_POLL_MS) return in;
  lastPoll = now;

  int32_t d = ss.getEncoderDelta();
  in.delta = ENCODER_REVERSE ? -d : d;

  bool raw = readButton();
  if (raw != btnRaw) { btnRaw = raw; btnChangeMs = now; }
  if (now - btnChangeMs >= DEBOUNCE_MS && raw != btnStable) {
    btnStable = raw;
    if (btnStable) {
      btnDownMs = now;
      longFired = false;
      veryLongFired = false;
    } else if (!veryLongFired) {
      // Power toggles on release after a 1 s hold, so a 10 s hold can
      // open the Wi-Fi portal without also turning the lights off.
      if (longFired || (now - btnDownMs >= LONG_PRESS_MS)) in.longPress = true;
      else in.click = true;
    }
  }
  if (btnStable && !longFired && now - btnDownMs >= LONG_PRESS_MS) {
    longFired = true;  // long enough for power; action waits for release
  }
  if (btnStable && !veryLongFired && now - btnDownMs >= WIFI_HOLD_MS) {
    veryLongFired = true;
    longFired = true;
    in.wifiHold = true;
  }
  return in;
}

// ---------------- Mic (SPH0645 I2S, non-blocking) ----------------
I2SClass i2sMic;
bool micOK = false;        // I2S started
bool micAlive = false;     // recently saw real audio, not all zeros
float micDc = 0;           // running DC (high-pass)
float micEnv = 0;          // envelope
float micFloor = MIC_FLOOR_MIN;
float micAgc = 800;        // slow typical level, for auto-gain
float micLevel = 0;        // 0..1 output
float micRms = 0;
float micSumSq = 0;
int32_t micPeak = 0;
uint16_t micCount = 0;
uint16_t micZeroWindows = 0;
uint32_t micLastDataMs = 0;

void micFinishWindow() {
  float rms = sqrtf(micSumSq / (micCount ? micCount : 1));
  micSumSq = 0;
  micCount = 0;
  micRms = rms;
  if (rms > micEnv) micEnv += (rms - micEnv) * 0.6f;
  else              micEnv += (rms - micEnv) * 0.08f;
  if (micEnv < micFloor) micFloor += (micEnv - micFloor) * 0.2f;
  else                   micFloor += (micEnv - micFloor) * 0.002f;
  if (micFloor < MIC_FLOOR_MIN) micFloor = MIC_FLOOR_MIN;
  // Slow AGC so Christmas music at room volume fills the tree at mid sensitivity
  // without having to chase the knob. Sensitivity 1-20 still scales the span.
  if (micEnv > micAgc) micAgc += (micEnv - micAgc) * 0.02f;
  else                 micAgc += (micEnv - micAgc) * 0.002f;
  if (micAgc < 200) micAgc = 200;
  float span = (10000.0f / cur.sens) + micFloor * 1.5f;
  float agcSpan = (micAgc - micFloor * 1.2f) * (12.0f / cur.sens);
  if (agcSpan > span) span = agcSpan;
  if (span < 80) span = 80;
  float lvl = (micEnv - micFloor * 1.3f) / span;
  micLevel = constrain(lvl, 0.0f, 1.0f);
  if (rms < MIC_FLOOR_MIN * 0.5f) {
    if (micZeroWindows < 50) micZeroWindows++;
  } else {
    micZeroWindows = 0;
    micAlive = true;
  }
  if (micZeroWindows >= 15) micAlive = false;  // ~300 ms of near-silence / zeros
}

bool micBegin() {
  i2sMic.setPins(MIC_BCLK_PIN, MIC_WS_PIN, -1, MIC_DATA_PIN);
  int8_t slot = MIC_I2S_RIGHT ? (int8_t)I2S_STD_SLOT_RIGHT : (int8_t)I2S_STD_SLOT_LEFT;
  if (!i2sMic.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT,
                    I2S_SLOT_MODE_MONO, slot)) {
    Serial.printf("[mic] I2S begin failed, err %d\n", i2sMic.lastError());
    return false;
  }
  i2sMic.setTimeout(0);
  if (MIC_I2S_MSB) {
    i2s_chan_handle_t ch = i2sMic.rxChan();
    if (ch) {
      i2s_std_slot_config_t sc =
          I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
      sc.slot_mask = MIC_I2S_RIGHT ? I2S_STD_SLOT_RIGHT : I2S_STD_SLOT_LEFT;
      i2s_channel_disable(ch);
      i2s_channel_reconfig_std_slot(ch, &sc);
      i2s_channel_enable(ch);
      Serial.println("[mic] using left-justified MSB slots");
    }
  }
  return true;
}

// Non-blocking: timeout 0, so a missing mic cannot stall the animation or WDT.
int micReadSamples(int32_t *buf, int maxSamples) {
  i2s_chan_handle_t ch = i2sMic.rxChan();
  if (!ch || maxSamples <= 0) return 0;
  size_t nbytes = 0;
  esp_err_t err = i2s_channel_read(ch, buf, (size_t)maxSamples * 4, &nbytes, 0);
  if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return 0;
  return (int)(nbytes / 4);
}

void micCalibrate() {
  micOK = micBegin();
  if (!micOK) {
    Serial.println("[mic] I2S init failed — sound mode will idle gently");
    return;
  }
  delay(40);  // SPH0645 needs a few milliseconds of BCLK/WS before valid data
  int32_t buf[MIC_WINDOW];
  int got = 0, zeros = 0;
  double dcSum = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 80 && got < (int)MIC_WINDOW * 2) {
    int n = micReadSamples(buf, MIC_WINDOW);
    for (int i = 0; i < n; i++) {
      int32_t s = buf[i] >> MIC_SHIFT;
      dcSum += s;
      if (s == 0) zeros++;
      got++;
    }
  }
  if (got > 0) micDc = (float)(dcSum / got);
  micLastDataMs = millis();
  micAlive = (got > 0 && zeros < got * 3 / 4);
  Serial.printf("[mic] I2S %lu Hz Philips 32-bit %s, BCLK=A0 WS=A2 DOUT=A1, "
                "samples %d, zeros %d, dc %.0f%s\n",
                (unsigned long)MIC_RATE, MIC_I2S_RIGHT ? "right" : "left",
                got, zeros, micDc, micAlive ? "" : " (idle if no signal)");
}

void micService() {
  if (!micOK) {
    micAlive = false;
    micLevel = 0;
    return;
  }
  static int32_t buf[MIC_READ_MAX];
  int n = micReadSamples(buf, MIC_READ_MAX);
  uint32_t now = millis();
  if (n <= 0) {
    if (now - micLastDataMs > 500) micAlive = false;
    return;
  }
  micLastDataMs = now;
  int32_t peak = 0;
  for (int i = 0; i < n; i++) {
    int32_t s = buf[i] >> MIC_SHIFT;
    if (s > peak) peak = s;
    else if (-s > peak) peak = -s;
    micDc += ((float)s - micDc) * MIC_DC_ALPHA;
    float ac = (float)s - micDc;
    micSumSq += ac * ac;
    if (++micCount >= MIC_WINDOW) micFinishWindow();
  }
  micPeak = peak;
}

void micPrintDiag(const char *tag) {
  Serial.printf("[%s] env %.1f floor %.1f agc %.0f rms %.1f peak %ld level %.2f "
                "dc %.0f %s\n",
                tag, micEnv, micFloor, micAgc, micRms, (long)micPeak, micLevel,
                micDc, (!micOK || !micAlive) ? "IDLE" : "live");
}


// ---------------- Effects ----------------
float twk[NUM_PIXELS];       // twinkle levels
uint16_t twkHue[NUM_PIXELS];

// Shared by Sound rainbow and Sound white star: live mic level, with a gentle breath if the
// mic is missing, silent, or only barely above the noise floor.
float soundLevel(uint32_t t) {
  float lvl = micLevel;
  if (!micOK || !micAlive || lvl < 0.08f) {
    float idle = 0.14f + 0.10f * (0.5f + 0.5f * sinf(t * (2 * PI / 4000.0f)));
    if (!micOK || !micAlive) lvl = idle;
    else lvl = max(lvl, idle * (1.0f - lvl / 0.08f) + lvl * (lvl / 0.08f));
  }
  return lvl;
}

void renderMode(uint32_t t) {
  switch (cur.mode) {
    case M_WHITE_PULSE: {
      float s = 0.55f + 0.45f * sinf(t * (2 * PI / 4000.0f));  // 4 s breath
      fillFrame(WW_R * s, WW_G * s, WW_B * s);
      break;
    }
    case M_STAR_WHITE: {
      uint16_t base = (t * 65536UL / 30000UL);  // full hue cycle every 30 s
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        uint8_t r, g, b;
        unpack(Adafruit_NeoPixel::ColorHSV(base + i * 400, 255, 255), r, g, b);
        setFrame(i, r, g, b);
      }
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, WW_R, WW_G, WW_B);
      break;
    }
    case M_CLASSIC: {
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        switch (i % 3) {
          case 0: setFrame(i, 255, 0, 0); break;
          case 1: setFrame(i, 0, 200, 0); break;
          default: setFrame(i, WW_R, WW_G, WW_B); break;
        }
      }
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, WW_R, WW_G, WW_B);
      break;
    }
    case M_RAINBOW: {
      // colors move from the box (pixel 0) up the tree into the star
      uint16_t base = (t * 65536UL / 8000UL);
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        uint8_t r, g, b;
        unpack(Adafruit_NeoPixel::ColorHSV(base - i * 1300, 255, 255), r, g, b);
        setFrame(i, r, g, b);
      }
      uint8_t r, g, b;
      unpack(Adafruit_NeoPixel::ColorHSV(base - (TREE_COUNT + 2) * 1300, 255, 255), r, g, b);
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, r, g, b);
      break;
    }
    case M_TWINKLE: {
      for (int i = 0; i < NUM_PIXELS; i++) {
        twk[i] *= 0.93f;
        if (random(1000) < 6) { twk[i] = 1.0f; twkHue[i] = random(6) == 0 ? 0xFFFF : random(65536); }
        // soft base: dim warm white
        float br = 40, bg = 28, bb = 14;
        uint8_t sr = 255, sg = 255, sb = 255;
        if (twkHue[i] != 0xFFFF) unpack(Adafruit_NeoPixel::ColorHSV(twkHue[i], 160, 255), sr, sg, sb);
        float k = twk[i];
        setFrame(i, br + (sr - br) * k, bg + (sg - bg) * k, bb + (sb - bb) * k);
      }
      break;
    }
    case M_SOUND: {
      // Tree fills from the box upward with level. Star is a cycling rainbow
      // hue (continues from the top of the tree), bright via the star floor,
      // and flares with the beat. Never fully dark; idle breath if the mic
      // is missing or silent.
      float lvl = soundLevel(t);
      int lit = (int)(lvl * TREE_COUNT + 0.5f);
      uint16_t hueBase = (t * 65536UL / 20000UL);
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        uint8_t r, g, b;
        unpack(Adafruit_NeoPixel::ColorHSV(hueBase + i * 600, 255, 255), r, g, b);
        float k = (i - TREE_FIRST) < lit ? 1.0f : 0.06f;
        setFrame(i, r * k, g * k, b * k);
      }
      uint8_t sr, sg, sb;
      unpack(Adafruit_NeoPixel::ColorHSV(hueBase + TREE_COUNT * 600, 255, 255), sr, sg, sb);
      float s = 0.15f + 0.85f * lvl;
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, sr * s, sg * s, sb * s);
      break;
    }
    case M_RAINBOW_TWINKLE: {
      // Star stays bright white. Tree: slow rainbow sparkles that fade on a
      // very dim wash of the last color, not a frantic glitter.
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        twk[i] *= 0.96f;
        if (random(1000) < 3) { twk[i] = 1.0f; twkHue[i] = random(65536); }
        uint8_t sr, sg, sb;
        unpack(Adafruit_NeoPixel::ColorHSV(twkHue[i], 255, 255), sr, sg, sb);
        float k = twk[i];
        float b = 0.07f + 0.93f * k;
        setFrame(i, sr * b, sg * b, sb * b);
      }
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, 255, 255, 255);
      break;
    }
    case M_WHITE_TWINKLE: {
      // Star stays bright white. Tree: dim warm-white base, soft white sparkles.
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        twk[i] *= 0.95f;
        if (random(1000) < 4) twk[i] = 1.0f;
        float k = twk[i];
        float br = 48, bg = 30, bb = 12;
        setFrame(i, br + (255 - br) * k, bg + (245 - bg) * k, bb + (220 - bb) * k);
      }
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, 255, 255, 255);
      break;
    }
    case M_SOUND2: {
      // Sound white star: pulsing white star and a slowly color-changing tree,
      // both following the mic. Same idle breath as Sound rainbow.
      float lvl = soundLevel(t);
      uint16_t hueBase = (t * 65536UL / 35000UL);  // slow, ~35 s around the wheel
      float tk = 0.22f + 0.78f * lvl;
      for (int i = TREE_FIRST; i <= TREE_LAST; i++) {
        uint8_t r, g, b;
        unpack(Adafruit_NeoPixel::ColorHSV(hueBase + i * 280, 255, 255), r, g, b);
        setFrame(i, r * tk, g * tk, b * tk);
      }
      float s = 0.35f + 0.65f * lvl;  // never fully dark
      for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, 255 * s, 255 * s, 255 * s);
      break;
    }
    case M_COLOR_CHASER: {
      // Head + short fading tail climbs pixel 0 up the tree and on through
      // the star (1-5 ring 1, 6-10 ring 2, 11 tip). The tip flares, then the
      // next pass steps to a new rainbow hue. Tree stays dark between.
      static uint32_t lastT = 0, flareT0 = 0;
      static float head = 0;
      static uint8_t passHue = 0;
      static bool flaring = false;
      uint32_t dt = (lastT == 0) ? 0 : (t - lastT);
      if (dt > 200) { dt = 0; head = 0; flaring = false; }  // mode re-entry
      lastT = t;
      uint32_t passMs = chasePeriodMs();
      uint32_t flareMs = passMs / 8;
      if (flareMs < 200) flareMs = 200;
      if (flareMs > 400) flareMs = 400;
      uint32_t climbMs = (passMs > flareMs) ? (passMs - flareMs) : passMs;
      const int n = NUM_PIXELS;
      const float TAIL = 7.0f;
      if (!flaring) {
        head += (float)(n - 1) * (float)dt / (float)climbMs;
        if (head >= (float)(n - 1)) { head = (float)(n - 1); flaring = true; flareT0 = t; }
      } else if (t - flareT0 >= flareMs) {
        flaring = false;
        head = 0;
        passHue++;
      }
      uint16_t hue = (uint16_t)passHue * (65536 / 12);
      uint8_t hr, hg, hb;
      unpack(Adafruit_NeoPixel::ColorHSV(hue, 255, 255), hr, hg, hb);
      fillFrame(0, 0, 0);
      if (!flaring) {
        for (int i = 0; i < n; i++) {
          float d = head - (float)i;
          if (d < 0 || d > TAIL) continue;
          float k = 1.0f - d / (TAIL + 0.25f);
          k = k * k;
          if (d < 1.0f) k = 1.0f;
          setFrame(i, hr * k, hg * k, hb * k);
        }
      } else {
        float u = (float)(t - flareT0) / (float)flareMs;
        if (u > 1) u = 1;
        float f = (u < 0.35f) ? (u / 0.35f) : (1.0f - (u - 0.35f) / 0.65f);
        if (f < 0) f = 0;
        for (int i = 0; i < n - 1; i++) {
          float d = (float)(n - 1 - i);
          if (d > TAIL) continue;
          float k = (1.0f - d / (TAIL + 0.25f));
          k = k * k * (1.0f - u) * 0.45f;
          setFrame(i, hr * k, hg * k, hb * k);
        }
        setFrame(STAR_LAST,
                 hr + (255 - hr) * f,
                 hg + (255 - hg) * f,
                 hb + (255 - hb) * f);
      }
      break;
    }
    case M_OFF:
    default:
      fillFrame(0, 0, 0);
      break;
  }
}

// ---------------- Setup mode ----------------
void runSetupMode() {
  Serial.println("[setup] setup mode: turn = max current, click = next");
  // wait for the boot-hold to be released
  while (readButton()) { delay(10); }
  btnStable = btnRaw = false;
  if (seesawOK) ss.getEncoderDelta();  // clear
  int stage = 0;                        // 0 = current, 1 = star trim
  int idx = cur.curIdx;
  int trim = cur.trim;
  uint8_t oldBright = cur.bright;
  uint32_t lastFrame = 0;
  while (stage < 2) {
    Input in = pollInput();
    if (stage == 0) {
      if (in.delta > 0 && idx < 2) idx++;
      if (in.delta < 0 && idx > 0) idx--;
    } else {
      trim = constrain(trim + in.delta * 5, 10, 100);
    }
    if (in.click || in.longPress) {
      stage++;
      if (in.longPress) stage = 2;
      Serial.printf("[setup] max %u mA, star trim %d%%\n", CURRENT_CHOICES[idx], trim);
    }
    if (millis() - lastFrame >= FRAME_MS) {
      lastFrame = millis();
      cur.curIdx = idx;
      cur.trim = trim;
      fillFrame(0, 0, 0);
      if (stage == 0) {
        // 1, 2 or 3 tree pixels lit at the box end (skipped if TREE_COUNT is 0)
        for (int i = 0; i <= idx; i++) setFrame(TREE_FIRST + i, 0, 255, 0);
        const uint8_t col[3][3] = {{0, 0, 255}, {255, 160, 0}, {255, 0, 0}};
        setEncLed(scale8(col[idx][0], 40), scale8(col[idx][1], 40), scale8(col[idx][2], 40));
        cur.bright = 120;
      } else {
        // star warm white at trim; tree top 10 pixels white for comparison
        for (int i = STAR_FIRST; i <= STAR_LAST; i++) setFrame(i, WW_R, WW_G, WW_B);
        for (int i = TREE_LAST - 9; i <= TREE_LAST; i++) setFrame(i, WW_R, WW_G, WW_B);
        setEncLed(scale8(255, 40), scale8(255, 40), scale8(255, 40));
        cur.bright = oldBright;
      }
      pushFrame(cur.bright);
    }
    delay(1);
  }
  cur.bright = oldBright;
  saveSettings();
  Serial.printf("[setup] saved: max %u mA, star trim %u%%\n",
                CURRENT_CHOICES[cur.curIdx], cur.trim);
}

// ---------------- Test mode ----------------
#if TEST_MODE
void runTestIntro() {
  Serial.println("[test] pixel 0 RED, pixel 50 RED, pixel 60 BLUE for 3 s");
  Serial.println("[test] if pixel 50 shows GREEN, flip STAR_RGB_SWAP");
  fillFrame(0, 0, 0);
  setFrame(0, 255, 0, 0);
  setFrame(STAR_FIRST, 255, 0, 0);
  setFrame(STAR_LAST, 0, 0, 255);
  pushFrame(120);
  delay(3000);
}
void testLoop() {
  static uint32_t last = 0;
  static int pos = 0;
  if (millis() - last < 250) return;
  last = millis();
  fillFrame(0, 0, 0);
  setFrame(pos, 255, 255, 255);
  pushFrame(120);
  Serial.printf("[test] chase pixel %d (%s)  est %.0f mA\n", pos,
                isStar(pos) ? "star" : "tree", lastEstMa);
  pos = (pos + 1) % NUM_PIXELS;
}
#endif

// ---------------- Rollback (core 3.3.12 has this enabled) ----------------
#ifdef CONFIG_APP_ROLLBACK_ENABLE
// Defer the core's boot-time "mark valid" so we can wait ~60 s of healthy
// running first. If this firmware crashes or watchdogs before that, the
// bootloader rolls back to the previous image.
bool verifyRollbackLater() { return true; }
#endif

void serviceRollback() {
  static bool done = false;
  if (done || millis() < ROLLBACK_VALID_MS) return;
  done = true;
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (running && esp_ota_get_state_partition(running, &st) == ESP_OK &&
      st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[ota] app marked valid after %lu s (err %d)\n",
                  (unsigned long)(millis() / 1000), (int)err);
  } else {
    Serial.println("[ota] rollback armed; image already valid or not pending");
  }
#else
  Serial.println("[ota] rollback is NOT enabled in this core; a bad update cannot auto-revert");
#endif
}

void setupWatchdog() {
  esp_task_wdt_config_t cfg = {
    .timeout_ms = WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err != ESP_OK) err = esp_task_wdt_init(&cfg);
  enableLoopWDT();
  Serial.printf("[wdt] task watchdog %lu ms (err %d)\n",
                (unsigned long)WDT_TIMEOUT_MS, (int)err);
}

// ---------------- Wi-Fi, captive portal, pull OTA (net task) ----------------
WiFiManager wm;
TaskHandle_t netTaskHandle = NULL;
uint32_t connectedAtMs = 0;
uint32_t nextOtaCheckMs = 0;
bool bootOtaPending = false;   // first check ~60 s after a Wi-Fi join
bool ntpStarted = false;
bool lanOtaStarted = false;
volatile uint8_t lastWifiReason = 0;
bool pendingJoin = false;
char portalMenuHtml[800];

void applyTxPower() {
  // QT Py ESP32-C3 can brown out at full TX, especially on a USB brick
  // that's also driving the LEDs. 8.5 dBm is plenty for a home router.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
}

void prepareSecureClient(NetworkClientSecure &client, bool insecure) {
  client.setTimeout(20000);
  client.setHandshakeTimeout(20000);
  if (insecure) {
    client.setInsecure();
    Serial.println("[ota] TLS: setInsecure fallback (certificate is not checked)");
  } else {
#if defined(ESP_ARDUINO_VERSION) && ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 3, 0)
    client.useBuiltinCACertBundle();
    Serial.println("[ota] TLS: core certificate bundle");
#else
    // Older ESP32 cores have no built-in bundle call. Update the core to 3.3+
    // to get certificate checking back.
    client.setInsecure();
    Serial.println("[ota] TLS: core older than 3.3, certificate not checked");
#endif
  }
}

bool timeLooksValid() {
  return time(NULL) > 1700000000;  // late 2023 or later, good enough for TLS
}

void startNtpIfNeeded() {
  if (ntpStarted) return;
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
  ntpStarted = true;
  Serial.println("[wifi] NTP started");
}

bool parseManifest(const String &body, int &version, String &url) {
  int vKey = body.indexOf("\"version\"");
  int uKey = body.indexOf("\"url\"");
  if (vKey < 0 || uKey < 0) return false;
  int vColon = body.indexOf(':', vKey);
  if (vColon < 0) return false;
  version = body.substring(vColon + 1).toInt();
  int uColon = body.indexOf(':', uKey);
  int q1 = body.indexOf('"', uColon);
  int q2 = (q1 >= 0) ? body.indexOf('"', q1 + 1) : -1;
  if (q1 < 0 || q2 < 0) return false;
  url = body.substring(q1 + 1, q2);
  url.trim();
  return version > 0 && (url.startsWith("https://") || url.startsWith("http://"));
}

int httpGetToString(const String &url, String &body, bool insecure) {
  NetworkClientSecure client;
  prepareSecureClient(client, insecure);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setUserAgent("StarController/1");
  if (!http.begin(client, url)) return -1;
  int code = http.GET();
  if (code == HTTP_CODE_OK) body = http.getString();
  http.end();
  return code;
}

// Manifest fetch: OK, missing (404/403 — repo may not exist yet), or a real error.
const uint8_t MF_OK = 0, MF_MISSING = 1, MF_FAIL = 2;

bool isHttpMissing(int code) {
  return code == HTTP_CODE_NOT_FOUND || code == HTTP_CODE_FORBIDDEN ||
         code == 410 || code == 401;
}

// Negative HTTPClient codes are connection/TLS failures. Real HTTP status
// (404, 500, ...) is not a certificate problem — do not retry those insecure.
bool isTlsOrConnectFail(int code) { return code < 0; }

uint8_t fetchManifest(int &version, String &url) {
  String body;
  int code = httpGetToString(UPDATE_MANIFEST_URL, body, false);
  if (code != HTTP_CODE_OK && isTlsOrConnectFail(code)) {
    Serial.printf("[ota] manifest TLS/connection failed, code %d, retrying insecure\n", code);
    body = "";
    code = httpGetToString(UPDATE_MANIFEST_URL, body, true);
  }
  if (isHttpMissing(code)) {
    Serial.printf("[ota] no update manifest yet (HTTP %d), will try again later\n", code);
    return MF_MISSING;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota] manifest fetch failed, code %d\n", code);
    return MF_FAIL;
  }
  if (!parseManifest(body, version, url)) {
    Serial.println("[ota] manifest parse failed");
    Serial.println(body);
    return MF_FAIL;
  }
  Serial.printf("[ota] manifest version %d, url %s\n", version, url.c_str());
  return MF_OK;
}

void fadeForUpdate() {
  netLedState = LED_UPDATING;
  otaFade = 255;
  otaWantFade = true;
  uint32_t start = millis();
  while (otaFade > 0 && millis() - start < 2000) {
    esp_task_wdt_reset();
    delay(20);
  }
}

t_httpUpdate_return downloadFirmware(const String &url, bool insecure, HTTPUpdate &updater) {
  NetworkClientSecure client;
  prepareSecureClient(client, insecure);
  updater.rebootOnUpdate(false);
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  updater.onProgress([](int done, int total) {
    esp_task_wdt_reset();
    if (total > 0 && (done % 32768 == 0 || done == total)) {
      Serial.printf("[ota] download %d / %d\n", done, total);
    }
  });
  return updater.update(client, url, String(FW_VERSION));
}

void runPullUpdate(const String &url) {
  if (!(url.startsWith("https://") || url.startsWith("http://"))) {
    Serial.println("[ota] refusing non-http(s) firmware URL");
    return;
  }
  Serial.printf("[ota] updating from %s\n", url.c_str());
  fadeForUpdate();
  HTTPUpdate updater(20000);
  t_httpUpdate_return ret = downloadFirmware(url, false, updater);
  if (ret == HTTP_UPDATE_FAILED) {
    int err = updater.getLastError();
    // Only retry without cert checks on a likely TLS/connection failure, never on 404.
    bool maybeTls = (err != HTTP_UE_SERVER_FILE_NOT_FOUND &&
                     err != HTTP_UE_SERVER_FORBIDDEN &&
                     err != HTTP_UE_SERVER_WRONG_HTTP_CODE &&
                     err != HTTP_UE_TOO_LESS_SPACE);
    if (maybeTls) {
      Serial.printf("[ota] bundle update failed (%d): %s, retrying insecure\n",
                    err, updater.getLastErrorString().c_str());
      ret = downloadFirmware(url, true, updater);
    }
  }
  if (ret == HTTP_UPDATE_OK) {
    Serial.println("[ota] flash ok, rebooting");
    delay(200);
    ESP.restart();
  }
  Serial.printf("[ota] update failed (%d): %s\n",
                updater.getLastError(), updater.getLastErrorString().c_str());
  otaWantFade = false;
  otaFade = 255;
  signalError();
}

void useLondonTz() {
  static bool ready = false;
  if (!ready) {
    setenv("TZ", LONDON_TZ, 1);
    tzset();
    ready = true;
  }
}

bool londonLocalTime(struct tm *out) {
  if (!timeLooksValid() || !out) return false;
  useLondonTz();
  time_t now = time(NULL);
  localtime_r(&now, out);
  return true;
}

int minuteOfDay(const struct tm *t) {
  return t->tm_hour * 60 + t->tm_min;
}

bool inEveningWindow(const struct tm *t) {
  int m = minuteOfDay(t);
  return m >= OTA_WIN_START_MIN && m < OTA_WIN_END_MIN;
}

// Stable-ish minute in 17:00–23:59 for this device and London date.
int pickWindowMinute(int year, int yday) {
  uint32_t seed = ((uint32_t)year << 16) ^ (uint32_t)(yday + 1);
  uint64_t mac = ESP.getEfuseMac();
  seed ^= (uint32_t)mac ^ (uint32_t)(mac >> 32);
  seed = seed * 1664525u + 1013904223u;
  const int span = OTA_WIN_END_MIN - OTA_WIN_START_MIN;  // 420 minutes
  return OTA_WIN_START_MIN + (int)(seed % (uint32_t)span);
}

void logNextOtaCheck(uint32_t whenMs) {
  if (!timeLooksValid()) return;
  useLondonTz();
  int32_t deltaMs = (int32_t)(whenMs - millis());
  time_t t = time(NULL) + (time_t)((deltaMs + (deltaMs >= 0 ? 500 : -500)) / 1000);
  struct tm lt;
  localtime_r(&t, &lt);
  Serial.printf("[ota] next check %02d:%02d London\n", lt.tm_hour, lt.tm_min);
}

void setNextOtaCheck(uint32_t whenMs) {
  nextOtaCheckMs = whenMs;
  logNextOtaCheck(whenMs);
}

void scheduleAtLondon(const struct tm *date, int minuteOfDayVal) {
  useLondonTz();
  struct tm t = *date;
  t.tm_hour = minuteOfDayVal / 60;
  t.tm_min = minuteOfDayVal % 60;
  t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t tgt = mktime(&t);
  time_t now = time(NULL);
  if (tgt == (time_t)-1 || tgt <= now) {
    t.tm_mday += 1;
    t.tm_isdst = -1;
    tgt = mktime(&t);
  }
  if (tgt == (time_t)-1 || tgt <= now) {
    nextOtaCheckMs = 0;
    return;
  }
  setNextOtaCheck(millis() + (uint32_t)((tgt - now) * 1000UL));
}

// Next evening slot: today's 17:00–23:59 pick if it has not started yet,
// otherwise tomorrow's. Used after a successful or missing-manifest check,
// and to defer work that would fall outside the window.
void scheduleNextEvening() {
  struct tm lt;
  if (!londonLocalTime(&lt)) {
    nextOtaCheckMs = 0;
    return;
  }
  if (minuteOfDay(&lt) < OTA_WIN_START_MIN) {
    scheduleAtLondon(&lt, pickWindowMinute(lt.tm_year, lt.tm_yday));
    return;
  }
  useLondonTz();
  struct tm tom = lt;
  tom.tm_mday += 1;
  tom.tm_hour = 12;
  tom.tm_min = 0;
  tom.tm_sec = 0;
  tom.tm_isdst = -1;
  time_t noon = mktime(&tom);
  if (noon != (time_t)-1) localtime_r(&noon, &tom);
  scheduleAtLondon(&tom, pickWindowMinute(tom.tm_year, tom.tm_yday));
}

// After MF_FAIL: retry in 30 min only if that time is still inside today's
// evening window; otherwise wait for the next evening.
void scheduleRetryOrNextEvening() {
  struct tm lt;
  if (!londonLocalTime(&lt)) {
    nextOtaCheckMs = 0;
    return;
  }
  if (inEveningWindow(&lt)) {
    time_t trial = time(NULL) + (time_t)(OTA_RETRY_MS / 1000);
    struct tm tt;
    useLondonTz();
    localtime_r(&trial, &tt);
    if (tt.tm_year == lt.tm_year && tt.tm_yday == lt.tm_yday && inEveningWindow(&tt)) {
      setNextOtaCheck(millis() + OTA_RETRY_MS);
      return;
    }
  }
  scheduleNextEvening();
}

void finishOtaCheck(uint8_t mf, int version, const String &url) {
  if (mf == MF_MISSING) {
    // No manifest: stay quiet and wait for the next evening. No error blink.
    scheduleNextEvening();
    return;
  }
  if (mf != MF_OK) {
    scheduleRetryOrNextEvening();
    return;
  }
  if (version <= (int)FW_VERSION) {
    Serial.printf("[ota] up to date (fw %u, manifest %d)\n", FW_VERSION, version);
    scheduleNextEvening();
    return;
  }
  runPullUpdate(url);
  // Returned only if the flash failed; retry inside the window if possible.
  scheduleRetryOrNextEvening();
}

void servicePullOta() {
  if (!wifiConnected || wm.getConfigPortalActive()) return;
  uint32_t now = millis();

  if (bootOtaPending) {
    // Keep the first check ~60 s after join, even if NTP is not ready yet
    // (after a short grace). This is the unplug-and-replug update path.
    if (!timeLooksValid() && now - connectedAtMs < OTA_FIRST_CHECK_MS + 30000) {
      return;  // give NTP a little more time before the first check
    }
    if ((int32_t)(now - nextOtaCheckMs) < 0) return;
    bootOtaPending = false;
    int version = 0;
    String url;
    uint8_t mf = fetchManifest(version, url);
    finishOtaCheck(mf, version, url);
    return;
  }

  // Periodic checks need a valid London clock and only run 17:00–23:59.
  if (!timeLooksValid()) return;
  if (nextOtaCheckMs == 0) {
    scheduleNextEvening();
    return;
  }
  if ((int32_t)(now - nextOtaCheckMs) < 0) return;

  struct tm lt;
  if (!londonLocalTime(&lt) || !inEveningWindow(&lt)) {
    scheduleNextEvening();
    return;
  }
  int version = 0;
  String url;
  uint8_t mf = fetchManifest(version, url);
  finishOtaCheck(mf, version, url);
}

void serviceLanOta() {
#if ENABLE_ARDUINO_OTA
  if (!wifiConnected) {
    lanOtaStarted = false;
    lanOtaReady = false;
    return;
  }
  if (!lanOtaStarted) {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(LAN_OTA_PASSWORD);
    ArduinoOTA.onStart([]() {
      fadeForUpdate();
      netLedState = LED_UPDATING;
    });
    ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
    ArduinoOTA.begin();
    lanOtaStarted = true;
    lanOtaReady = true;
    Serial.println("[ota] ArduinoOTA listening on LAN (password set)");
  }
  if (!wm.getConfigPortalActive()) ArduinoOTA.handle();
#else
  (void)lanOtaStarted;
#endif
}


// ---------------- Local web control (STA only, net task) ----------------
WebServer ctrl(80);
bool ctrlRunning = false;
bool ctrlRoutes = false;
bool mdnsRunning = false;
char postBuf[API_MAX_BODY + 1];
size_t postLen = 0;
bool postTooBig = false;
uint32_t lastSetMs = 0;

void jsonEscape(char *dst, size_t n, const char *src) {
  size_t j = 0;
  if (n == 0) return;
  for (size_t i = 0; src[i] && j + 1 < n; i++) {
    char c = src[i];
    if ((c == '"' || c == '\\') && j + 2 < n) {
      dst[j++] = '\\';
      dst[j++] = c;
    } else if ((unsigned char)c >= 0x20) {
      dst[j++] = c;
    }
  }
  dst[j] = 0;
}

void effectiveSettings(uint8_t &power, uint8_t &bright, uint8_t &mode, uint8_t &sens, uint8_t &chase) {
  power = cur.power;
  bright = cur.bright;
  mode = cur.mode;
  sens = cur.sens;
  chase = cur.chase;
  uint8_t mask = remoteSet.mask;
  if (mask & SET_POWER) power = remoteSet.power;
  if (mask & SET_BRIGHT) bright = remoteSet.bright;
  if (mask & SET_MODE) mode = remoteSet.mode;
  if (mask & SET_SENS) sens = remoteSet.sens;
  if (mask & SET_CHASE) chase = remoteSet.chase;
}

int buildStateJson(char *buf, size_t n) {
  uint8_t power, bright, mode, sens, chase;
  effectiveSettings(power, bright, mode, sens, chase);
  IPAddress ip = WiFi.localIP();
  char ssidEsc[48];
  jsonEscape(ssidEsc, sizeof(ssidEsc), WiFi.SSID().c_str());
  return snprintf(buf, n,
      "{\"power\":%u,\"bright\":%u,\"mode\":%u,\"sens\":%u,\"chase\":%u,\"fw\":%u,"
      "\"rssi\":%d,\"ip\":\"%u.%u.%u.%u\",\"ssid\":\"%s\"}",
      power, bright, mode, sens, chase, FW_VERSION, (int)WiFi.RSSI(),
      ip[0], ip[1], ip[2], ip[3], ssidEsc);
}

void sendJson(int code, const char *buf) {
  ctrl.setContentLength(strlen(buf));
  ctrl.send(code, "application/json", buf);
}

void sendJsonErr(int code, const char *msg) {
  char buf[96];
  snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
  sendJson(code, buf);
}

void handleRoot() {
  ctrl.sendHeader("Cache-Control", "no-store");
  ctrl.send_P(200, "text/html; charset=utf-8", CONTROL_PAGE);
}

void handleIcon() {
  ctrl.sendHeader("Cache-Control", "public, max-age=86400");
  ctrl.send_P(200, "image/png", (PGM_P)ICON_PNG, ICON_PNG_LEN);
}

void handleState() {
  char buf[256];
  buildStateJson(buf, sizeof(buf));
  ctrl.sendHeader("Cache-Control", "no-store");
  sendJson(200, buf);
}

static void skipWs(const char **p) {
  while (**p == ' ' || **p == '\n' || **p == '\r' || **p == '\t') (*p)++;
}

bool parseSetJson(const char *s, uint8_t *maskOut, int *power, int *bright, int *mode, int *sens,
                  int *chase, const char **err) {
  const char *p = s;
  skipWs(&p);
  if (*p != '{') { *err = "expected a JSON object"; return false; }
  p++;
  skipWs(&p);
  if (*p == '}') { *err = "empty request"; return false; }
  uint8_t mask = 0;
  while (*p) {
    skipWs(&p);
    if (*p != '"') { *err = "expected a key"; return false; }
    p++;
    const char *key = p;
    while (*p && *p != '"') p++;
    if (*p != '"') { *err = "unterminated key"; return false; }
    int klen = (int)(p - key);
    p++;
    skipWs(&p);
    if (*p != ':') { *err = "expected a colon"; return false; }
    p++;
    skipWs(&p);
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    if (*p < '0' || *p > '9') { *err = "values must be integers"; return false; }
    long v = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      if (v > 100000) { *err = "value too large"; return false; }
      p++;
    }
    if (neg) v = -v;
    if (klen == 5 && !strncmp(key, "power", 5)) { mask |= SET_POWER; *power = (int)v; }
    else if (klen == 6 && !strncmp(key, "bright", 6)) { mask |= SET_BRIGHT; *bright = (int)v; }
    else if (klen == 4 && !strncmp(key, "mode", 4)) { mask |= SET_MODE; *mode = (int)v; }
    else if (klen == 4 && !strncmp(key, "sens", 4)) { mask |= SET_SENS; *sens = (int)v; }
    else if (klen == 5 && !strncmp(key, "chase", 5)) { mask |= SET_CHASE; *chase = (int)v; }
    skipWs(&p);
    if (*p == ',') { p++; continue; }
    if (*p == '}') {
      p++;
      skipWs(&p);
      if (*p != 0) { *err = "unexpected text after JSON"; return false; }
      if (!mask) { *err = "no recognized fields"; return false; }
      *maskOut = mask;
      return true;
    }
    *err = "malformed JSON";
    return false;
  }
  *err = "malformed JSON";
  return false;
}

void handleSetRaw() {
  HTTPRaw &r = ctrl.raw();
  if (r.status == RAW_START) {
    postLen = 0;
    postTooBig = false;
    postBuf[0] = 0;
    int cl = ctrl.clientContentLength();
    if (cl < 0 || (size_t)cl > API_MAX_BODY) {
      postTooBig = true;
      ctrl.client().stop();
    }
  } else if (r.status == RAW_WRITE && !postTooBig) {
    if (postLen + r.currentSize > API_MAX_BODY) {
      postTooBig = true;
      return;
    }
    memcpy(postBuf + postLen, r.buf, r.currentSize);
    postLen += r.currentSize;
    postBuf[postLen] = 0;
  }
}

void handleSet() {
  ctrl.sendHeader("Cache-Control", "no-store");
  if (postTooBig || postLen > API_MAX_BODY) {
    sendJsonErr(413, "request too large");
    return;
  }
  uint32_t now = millis();
  if (lastSetMs != 0 && now - lastSetMs < API_MIN_SET_MS) {
    sendJsonErr(429, "too many requests");
    return;
  }
  if (postLen == 0) {
    sendJsonErr(400, "empty body");
    return;
  }
  uint8_t mask = 0;
  int power = 0, bright = 0, mode = 0, sens = 0, chase = 0;
  const char *err = nullptr;
  if (!parseSetJson(postBuf, &mask, &power, &bright, &mode, &sens, &chase, &err)) {
    sendJsonErr(400, err ? err : "bad request");
    return;
  }
  if ((mask & SET_POWER) && power != 0 && power != 1) {
    sendJsonErr(400, "power must be 0 or 1");
    return;
  }
  if ((mask & SET_MODE) && (mode < 0 || mode >= MODE_COUNT)) {
    sendJsonErr(400, "mode out of range");
    return;
  }
  if (mask & SET_BRIGHT) bright = constrain(bright, (int)MIN_BRIGHTNESS, 255);
  if (mask & SET_SENS) sens = constrain(sens, 1, 20);
  if (mask & SET_CHASE) chase = constrain(chase, (int)CHASE_SPD_MIN, (int)CHASE_SPD_MAX);

  lastSetMs = now;
  remoteSet.power = (uint8_t)power;
  remoteSet.bright = (uint8_t)bright;
  remoteSet.mode = (uint8_t)mode;
  remoteSet.sens = (uint8_t)sens;
  remoteSet.chase = (uint8_t)chase;
  remoteSet.mask = mask;  // published last so the loop sees a complete command

  char buf[256];
  buildStateJson(buf, sizeof(buf));
  sendJson(200, buf);
}

void registerCtrlRoutes() {
  if (ctrlRoutes) return;
  ctrl.on("/", HTTP_GET, handleRoot);
  ctrl.on("/icon.png", HTTP_GET, handleIcon);
  ctrl.on("/api/state", HTTP_GET, handleState);
  ctrl.on("/api/set", HTTP_POST, handleSet, handleSetRaw);
  ctrl.onNotFound([]() {
    sendJsonErr(404, "not found");
  });
  ctrlRoutes = true;
}

void stopControlServer() {
  if (mdnsRunning) {
    MDNS.end();
    mdnsRunning = false;
  }
  if (ctrlRunning) {
    ctrl.stop();
    ctrlRunning = false;
    Serial.println("[web] control server stopped");
  }
}

void startControlServer() {
  if (ctrlRunning) return;
  registerCtrlRoutes();
  ctrl.begin();
  ctrlRunning = true;
  IPAddress ip = WiFi.localIP();
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsRunning = true;
    Serial.printf("[web] http://%s.local  (IP %u.%u.%u.%u)\n",
                  HOSTNAME, ip[0], ip[1], ip[2], ip[3]);
  } else {
    mdnsRunning = false;
    Serial.printf("[web] mDNS failed, use http://%u.%u.%u.%u\n",
                  ip[0], ip[1], ip[2], ip[3]);
  }
}

void serviceControlServer() {
  if (wm.getConfigPortalActive() || !wifiConnected) {
    stopControlServer();
    return;
  }
  startControlServer();
  ctrl.handleClient();
}

// Injected into every portal page so the Configure WiFi view (which uses
// WiFiManager's own "Config ESP" heading) still shows a clear title and
// instruction. The menu page already has an h1, so this only adds one
// when it is missing.
const char PORTAL_HEAD_JS[] = R"HEAD(<script>
document.title='Christmas Star Wi-Fi setup';
document.addEventListener('DOMContentLoaded',function(){
  var w=document.querySelector('.wrap');
  if(!w||document.querySelector('h1'))return;
  var h=document.createElement('h1');
  h.textContent='Christmas Star Wi-Fi setup';
  var p=document.createElement('p');
  p.style.margin='8px 0';
  p.textContent='Tap your home Wi-Fi network, enter its password, then tap Save.';
  w.insertBefore(p,w.firstChild);
  w.insertBefore(h,p);
});
</script>)HEAD";

const char *wifiReasonText(uint8_t r) {
  switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_FAIL:
      return "wrong password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_BEACON_TIMEOUT:
      return "network not found";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
      return "router refused the connection";
    default:
      return "could not join";
  }
}

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiReason = info.wifi_sta_disconnected.reason;
    Serial.printf("[wifi] disconnected, reason %u (%s)\n",
                  lastWifiReason, wifiReasonText(lastWifiReason));
  }
}

void applyPortalMenuHtml(const char *status) {
  if (status && status[0]) {
    snprintf(portalMenuHtml, sizeof(portalMenuHtml),
             "<p style='margin:8px 0'>Tap your home Wi-Fi network, enter its "
             "password, then tap Save.</p>"
             "<p style='margin:8px 0;color:#b00000;font-weight:bold'>%s</p>"
             "<p style='margin:8px 0;font-size:90%%'>The star keeps lighting "
             "while you do this. After it joins, open http://christmasstar.local "
             "in Safari. If that name does not work, tap Info &mdash; the IP "
             "address (not 192.168.4.1) is another way to reach it. To forget a "
             "saved network, open Info and tap Erase.</p>",
             status);
  } else {
    snprintf(portalMenuHtml, sizeof(portalMenuHtml),
             "<p style='margin:8px 0'>Tap your home Wi-Fi network, enter its "
             "password, then tap Save.</p>"
             "<p style='margin:8px 0;font-size:90%%'>The star keeps lighting "
             "while you do this. After it joins, open http://christmasstar.local "
             "in Safari. If that name does not work, tap Info &mdash; the IP "
             "address (not 192.168.4.1) is another way to reach it. To forget a "
             "saved network, open Info and tap Erase.</p>");
  }
  wm.setCustomMenuHTML(portalMenuHtml);
}

// WiFiManager disables STA when the portal opens if we are not already
// connected (_disableSTAConn defaults to true). ESP32-C3 cannot scan in
// WIFI_AP-only mode, so Configure WiFi would show just an SSID box.
// Keep AP+STA for the whole time the portal is up.
void enablePortalSta() {
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    Serial.println("[wifi] portal set to AP+STA so networks can be scanned");
  }
  applyTxPower();
}

void reportJoinFailure() {
  String ssid = WiFi.SSID();
  uint8_t st = wm.getLastConxResult();
  const char *why = wifiReasonText(lastWifiReason);
  if (st == WL_NO_SSID_AVAIL) why = "network not found";
  if (st == 7) why = "wrong password";  // WiFiManager's internal wrong-password flag
  Serial.printf("[wifi] could not join \"%s\": %s (status %u %s, reason %u)\n",
                ssid.c_str(), why, st, wm.getWLStatusString(st).c_str(),
                lastWifiReason);
  char msg[160];
  bool notFound = (st == WL_NO_SSID_AVAIL || lastWifiReason == WIFI_REASON_NO_AP_FOUND);
  if (ssid.length() && notFound) {
    snprintf(msg, sizeof(msg),
             "Couldn't find %s. Check the name, or tap Configure WiFi, then Refresh, and pick it from the list.",
             ssid.c_str());
  } else if (ssid.length()) {
    snprintf(msg, sizeof(msg), "Couldn't join %s, check the password", ssid.c_str());
  } else {
    snprintf(msg, sizeof(msg), "Couldn't join the network, check the name and password.");
  }
  applyPortalMenuHtml(msg);
  enablePortalSta();
}

void startPortal() {
  if (wm.getConfigPortalActive()) {
    Serial.println("[wifi] portal already running");
    return;
  }
  Serial.printf("[wifi] starting setup portal SSID \"%s\" pass \"%s\"\n",
                AP_SSID, AP_PASS);
  stopControlServer();  // both use port 80
  applyPortalMenuHtml(nullptr);
  netLedState = LED_PORTAL;
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_MS / 1000);
  wm.startConfigPortal(AP_SSID, AP_PASS);
  enablePortalSta();
}

void stopPortalIfRunning() {
  if (wm.getConfigPortalActive()) {
    wm.stopConfigPortal();
    Serial.println("[wifi] portal stopped");
  }
  if (netLedState == LED_PORTAL) {
    if (wifiConnected || WiFi.isConnected()) {
      wifiConnected = true;
      signalConnected();
    } else {
      netLedState = LED_NORMAL;
      netLedEventMs = millis();
    }
  }
}

void configureWifiManager() {
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_MS / 1000);
  wm.setConnectTimeout(20);
  wm.setCaptivePortalEnable(true);
  wm.setWiFiAutoReconnect(false);
  wm.setHostname(HOSTNAME);
  wm.setTitle("Christmas Star Wi-Fi setup");
  wm.setShowInfoErase(true);
  wm.setShowInfoUpdate(false);
  wm.setMinimumSignalQuality(-1);  // list every 2.4 GHz network the radio can hear
  const char *menu[] = {"wifi", "custom", "info", "exit"};
  wm.setMenu(menu, 4);
  wm.setCustomHeadElement(PORTAL_HEAD_JS);
  applyPortalMenuHtml(nullptr);
  wm.setAPCallback([](WiFiManager *) { enablePortalSta(); });
  wm.setPreSaveConfigCallback([]() {
    pendingJoin = true;
    lastWifiReason = 0;
    Serial.println("[wifi] credentials submitted, trying to join");
  });
  wm.setSaveConfigCallback([]() {
    Serial.println("[wifi] new credentials saved");
  });
  wm.setConfigPortalTimeoutCallback([]() {
    Serial.println("[wifi] portal timed out");
    if (netLedState == LED_PORTAL) {
      netLedState = (wifiConnected || WiFi.isConnected()) ? LED_JUST_CONNECTED : LED_NORMAL;
      netLedEventMs = millis();
    }
  });
}

void serviceReconnect() {
  static uint32_t nextTryMs = 0;
  static uint32_t backoffMs = 2000;
  static bool wasConnected = false;

  static bool pendingGreen = false;
  bool nowUp = WiFi.isConnected();
  wifiConnected = nowUp;
  if (nowUp) {
    if (!wasConnected) {
      connectedAtMs = millis();
      nextOtaCheckMs = connectedAtMs + OTA_FIRST_CHECK_MS;
      bootOtaPending = true;
      applyTxPower();
      startNtpIfNeeded();
      Serial.printf("[wifi] connected, IP %s, RSSI %d\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
      pendingGreen = true;
    }
    wasConnected = true;
    backoffMs = 2000;
    // Wait until the portal has closed so the green flash isn't overwritten
    // by the blue setup blink.
    if (pendingGreen && !wm.getConfigPortalActive()) {
      signalConnected();
      pendingGreen = false;
    }
    return;
  }
  if (wasConnected) {
    Serial.println("[wifi] lost connection");
    wasConnected = false;
    lanOtaStarted = false;
    lanOtaReady = false;
    stopControlServer();
    if (!wm.getConfigPortalActive()) signalError();
  }
  if (wm.getConfigPortalActive()) return;
  if (!wm.getWiFiIsSaved()) return;

  uint32_t now = millis();
  if (nextTryMs != 0 && (int32_t)(now - nextTryMs) < 0) return;
  wl_status_t st = WiFi.status();
  if (st == WL_IDLE_STATUS || st == WL_DISCONNECTED || st == WL_CONNECT_FAILED ||
      st == WL_NO_SSID_AVAIL || st == WL_CONNECTION_LOST) {
    Serial.printf("[wifi] reconnect try, backoff %lu ms\n", (unsigned long)backoffMs);
    WiFi.mode(WIFI_STA);
    applyTxPower();
    WiFi.begin();
    nextTryMs = now + backoffMs;
    if (backoffMs < 300000UL) backoffMs *= 2;
    if (backoffMs > 300000UL) backoffMs = 300000UL;
  } else {
    nextTryMs = now + 1000;  // still connecting
  }
}

void netService() {
  if (requestPortal) {
    requestPortal = false;
    if (wm.getConfigPortalActive()) stopPortalIfRunning();
    else startPortal();
  }

  serviceReconnect();

  static bool portalWasOn = false;
  bool portalOn = wm.getConfigPortalActive();
  if (portalOn) {
    stopControlServer();
    if (netLedState != LED_UPDATING) netLedState = LED_PORTAL;
    wm.process();
    portalOn = wm.getConfigPortalActive();
    // connectWifi() runs inside process() after a save, so the join
    // result is already known when we get here.
    if (pendingJoin) {
      pendingJoin = false;
      if (WiFi.isConnected()) {
        wifiConnected = true;
        Serial.printf("[wifi] joined \"%s\", closing setup portal\n",
                      WiFi.SSID().c_str());
        if (portalOn) stopPortalIfRunning();
        else signalConnected();
        portalOn = wm.getConfigPortalActive();
      } else {
        reportJoinFailure();
      }
    }
  }

  if (!portalOn) {
    if (portalWasOn && netLedState == LED_PORTAL) {
      if (WiFi.isConnected()) {
        wifiConnected = true;
        signalConnected();
      } else {
        netLedState = LED_NORMAL;
      }
      Serial.println("[wifi] portal closed");
    }
    serviceControlServer();
  }
  portalWasOn = portalOn;

  serviceLanOta();
  servicePullOta();
}

void netTask(void * /*pv*/) {
  esp_task_wdt_add(NULL);
  WiFi.persistent(true);
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_STA);
  applyTxPower();
  WiFi.onEvent(onWifiEvent);
  configureWifiManager();

  if (!wm.getWiFiIsSaved()) {
    Serial.println("[wifi] no saved credentials, opening setup portal");
    startPortal();
  } else {
    Serial.printf("[wifi] saved SSID \"%s\", connecting\n", WiFi.SSID().c_str());
    WiFi.begin();
  }

  for (;;) {
    esp_task_wdt_reset();
    netService();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startNetTask() {
  xTaskCreate(netTask, "net", 16384, NULL, 1, &netTaskHandle);
  Serial.println("[wifi] net task started");
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) {}
  Serial.printf("\n[boot] StarController fw %u, build %s\n", FW_VERSION, BUILD_TAG);
#ifdef CONFIG_APP_ROLLBACK_ENABLE
  Serial.println("[boot] app rollback is enabled in this core");
#else
  Serial.println("[boot] app rollback is NOT enabled in this core");
#endif

  setupWatchdog();

  pixelsBegin();
  strip.clear();
  pixelsShow();

  Wire.begin();
  seesawOK = ss.begin(SEESAW_ADDR);
  if (seesawOK) {
    uint32_t ver = (ss.getVersion() >> 16) & 0xFFFF;
    Serial.printf("[seesaw] found at 0x%02X, product %lu%s\n", SEESAW_ADDR,
                  (unsigned long)ver, ver == 4991 ? "" : " (expected 4991)");
    ss.pinMode(SS_SWITCH_PIN, INPUT_PULLUP);
    ss.setGPIOInterrupts((uint32_t)1 << SS_SWITCH_PIN, 1);
    ss.enableEncoderInterrupt();
    encPix.begin(SEESAW_ADDR);
    encPix.setBrightness(255);
    setEncLed(0, 0, 0);
  } else {
    Serial.println("[seesaw] NOT found at 0x36 - check STEMMA QT cable. Running with saved settings.");
  }

  loadSettings();
  Serial.printf("[nvs] mode %u (%s), bright %u, power %u, max %u mA, trim %u%%, sens %u, chase %u\n",
                cur.mode, MODE_NAMES[cur.mode], cur.bright, cur.power,
                CURRENT_CHOICES[cur.curIdx], cur.trim, cur.sens, cur.chase);

  micCalibrate();
  randomSeed(esp_random());

#if TEST_MODE
  runTestIntro();
  return;
#endif

  if (seesawOK && readButton()) {
    delay(50);
    if (readButton()) runSetupMode();
  }
  if (seesawOK) ss.getEncoderDelta();  // clear any turns
  showModeLed();
  startNetTask();
}

void loop() {
#if TEST_MODE
  testLoop();
  static uint32_t testDiag = 0;
  micService();
  if (millis() - testDiag > DIAG_MS) {
    testDiag = millis();
    micPrintDiag("test");
  }
  return;
#endif

  uint32_t now = millis();
  Input in = pollInput();

  if (in.wifiHold) {
    Serial.println("[ctl] Wi-Fi setup hold");
    requestPortal = true;
  } else if (in.longPress) {
    applyPowerFromCtl(!cur.power);
  } else if (in.click) {
    if (!cur.power) {
      applyPowerFromCtl(1);                        // click also wakes it up
    } else {
      // existing modes, then new ones, then Off, then back to White pulse
      uint8_t next = (cur.mode < MODE_COUNT) ? MODE_CLICK_NEXT[cur.mode]
                                             : DEFAULT_MODE;
      applyModeFromCtl(next);
    }
  }

  if (in.delta != 0 && cur.power) {
    if (isSoundMode(cur.mode)) applySensFromCtl(constrain((int)cur.sens + (int)in.delta, 1, 20));
    else if (isChaserMode(cur.mode)) applyChaseFromCtl(constrain((int)cur.chase + (int)in.delta,
                                                                (int)CHASE_SPD_MIN, (int)CHASE_SPD_MAX));
    else applyBrightFromCtl(constrain((int)cur.bright + (int)in.delta * BRIGHT_STEP,
                                      (int)MIN_BRIGHTNESS, 255));
  }

  serviceRemoteSet();

#if MIC_DEBUG
  micService();
#else
  if (cur.power && isSoundMode(cur.mode)) micService();
#endif

  if (otaWantFade) {
    if (otaFade > 12) otaFade -= 12;
    else otaFade = 0;
  } else if (otaFade != 255) {
    otaFade = 255;
  }
  serviceVisFade();

  static uint32_t lastFrame = 0;
  if (now - lastFrame >= FRAME_MS) {
    lastFrame = now;
    bool show = cur.power || visFade > 0;
    if (show) renderMode(now);
    else fillFrame(0, 0, 0);
    if (show) applyStarFlash(now);
    uint8_t level = 0;
    if (show) {
      uint8_t src = isChaserMode(cur.mode) ? 255 : cur.bright;
      level = scale8(src, scale8(visFade, otaFade));
    }
    pushFrame(level);
    serviceEncLed();
  }

  serviceSave();
  serviceRollback();

  static uint32_t lastDiag = 0;
  if (now - lastDiag >= DIAG_MS) {
    lastDiag = now;
    Serial.printf("[diag] %s, power %u, bright %u, est %.0f mA (raw %.0f, limit %u), wifi %s, fw %u",
                  MODE_NAMES[cur.mode], cur.power, cur.bright, lastEstMa,
                  lastRawMa, CURRENT_CHOICES[cur.curIdx],
                  wifiConnected ? "up" : (netLedState == LED_PORTAL ? "portal" : "down"),
                  FW_VERSION);
    if (isSoundMode(cur.mode) || MIC_DEBUG)
      Serial.printf(", mic env %.1f floor %.1f level %.2f sens %u %s", micEnv, micFloor,
                    micLevel, cur.sens, (!micOK || !micAlive) ? "idle" : "live");
    Serial.println();
#if MIC_DEBUG
    micPrintDiag("mic");
#endif
  }
}
