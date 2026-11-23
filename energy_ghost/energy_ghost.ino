// Energy Ghost - a small always-battery, deep-sleep e-paper display that
// checks GB grid carbon intensity every 30 minutes (or on a button press)
// and shows a single ghost mascot whose FILL WEIGHT tells the story -
// hollow and light when the grid's clean, solid black and jagged when
// it's dirty - plus a short, deterministic recommendation ("RUN NOW" /
// "WAIT IF YOU CAN" / "AVOID IF POSSIBLE" / ...).
//
// STANDALONE - no relay, no computer needing to be on. carbon_api.cpp talks
// directly to two free, keyless APIs (National Grid ESO for the carbon
// data, ip-api.com for a rough location) over the device's own WiFi and
// runs decide_energy()'s rule engine on-device. This is the one app in the
// project that can do this - it's also the only one with zero LLM
// dependency - it never touches an LLM at all. relay/energy_relay.py still exists and still works as an
// optional alternative (or as a quick way to test decide_energy() changes
// without reflashing), but the firmware no longer calls it.
//
// No sensor, no menu, single-purpose device 
// - and it genuinely needs WiFi, since the one
// thing it shows is inherently live data, not a local reading. If the APIs
// are unreachable it falls back to the last successful reading (RTC memory
// survives deep sleep) tagged "(cached)", or - on a true cold start with
// nothing cached yet - an honest "no data yet" ghost rather than a guess
// (honesty over guessing).
//
// Board: Waveshare ESP32-S3-ePaper-1.54 (V2)

#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "user_config.h"
#include "secrets.h"
#include "energy_config.h"
#include "ghost_render.h"
#include "carbon_api.h"

#include "src/display/epaper_driver_bsp.h"
#include "src/power/board_power_bsp.h"

// Survive deep sleep so a WiFi/API miss can fall back to the last good
// reading instead of showing a blank "no data" ghost every single time.
RTC_DATA_ATTR static int cached_intensity = -1;
RTC_DATA_ATTR static char cached_index[16] = "";
RTC_DATA_ATTR static char cached_recommendation[96] = "";
RTC_DATA_ATTR static bool has_cached = false;

static board_power_bsp_t board_power(EPD_PWR_PIN, Audio_PWR_PIN, VBAT_PWR_PIN);
static epaper_driver_display *driver = nullptr;
static GFXcanvas1 *canvas = nullptr;
static bool wifi_ok = false;

static void connect_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 8000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  wifi_ok = (WiFi.status() == WL_CONNECTED);
  if (wifi_ok) {
    Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("No WiFi - falling back to cached reading, if any");
  }
}

static void commit_canvas() {
  for (int y = 0; y < EPD_HEIGHT; y++) {
    for (int x = 0; x < EPD_WIDTH; x++) {
      bool drawn = canvas->getPixel(x, y);
      driver->EPD_DrawColorPixel((uint16_t)x, (uint16_t)y, drawn ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
    }
  }
  driver->EPD_Display();
}

static void go_to_deep_sleep() {
  board_power.POWEER_EPD_OFF();  // panel keeps its image with zero power anyway
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(WAKE_INTERVAL_US);

  // A button press also forces an immediate refresh.
  const uint64_t wake_mask = (1ULL << PWR_BUTTON_PIN) | (1ULL << BOOT_BUTTON_PIN);
  esp_sleep_enable_ext1_wakeup_io(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  rtc_gpio_pullup_en((gpio_num_t)PWR_BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)PWR_BUTTON_PIN);
  rtc_gpio_pullup_en((gpio_num_t)BOOT_BUTTON_PIN);
  rtc_gpio_pulldown_dis((gpio_num_t)BOOT_BUTTON_PIN);

  // VBAT rail's GPIO state isn't retained through deep sleep unless
  // explicitly held - see Waveshare's own 11_RTC_Sleep_Test example, same
  // pattern every app in this project follows. Released again on next wake.
  rtc_gpio_hold_en((gpio_num_t)VBAT_PWR_PIN);

  Serial.println("Going to sleep - next check in 30 min, or press either button now.");
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(300);

  gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
  board_power.VBAT_POWER_ON();

  pinMode(PWR_BUTTON_PIN, INPUT_PULLUP);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const char *wake_reason = (cause == ESP_SLEEP_WAKEUP_TIMER) ? "timer"
                           : (cause == ESP_SLEEP_WAKEUP_EXT1)  ? "button"
                                                                : "cold boot";
  Serial.printf("[wake] %s\n", wake_reason);

  board_power.POWEER_EPD_ON();

  custom_lcd_spi_t epd_cfg = {};
  epd_cfg.cs = EPD_CS_PIN;
  epd_cfg.dc = EPD_DC_PIN;
  epd_cfg.rst = EPD_RST_PIN;
  epd_cfg.busy = EPD_BUSY_PIN;
  epd_cfg.mosi = EPD_MOSI_PIN;
  epd_cfg.scl = EPD_SCK_PIN;
  epd_cfg.spi_host = EPD_SPI_NUM;
  epd_cfg.buffer_len = 5000;

  driver = new epaper_driver_display(EPD_WIDTH, EPD_HEIGHT, epd_cfg);
  driver->EPD_Init();
  driver->EPD_Clear();
  driver->EPD_Display();  // blank flash first, clears any prior-refresh ghosting

  canvas = new GFXcanvas1(EPD_WIDTH, EPD_HEIGHT);

  connect_wifi();

  GhostMood mood;
  int intensity = 0;
  String recommendation;
  String index_str;
  const char *tag;

  bool live = wifi_ok && fetch_carbon_data(intensity, index_str, recommendation);

  if (live) {
    mood = mood_from_index(index_str);

    index_str.toCharArray(cached_index, sizeof(cached_index));
    recommendation.toCharArray(cached_recommendation, sizeof(cached_recommendation));
    cached_intensity = intensity;
    has_cached = true;
    tag = "(live)";
  } else if (has_cached) {
    intensity = cached_intensity;
    mood = mood_from_index(String(cached_index));
    recommendation = String(cached_recommendation);
    tag = "(cached)";
  } else {
    intensity = 0;
    mood = GHOST_UNKNOWN;
    recommendation = "No reading yet - check WiFi credentials in secrets.h.";
    tag = "";
  }

  Serial.printf("[ghost] (%s) %d gCO2/kWh, %s\n", tag[0] ? tag : "no data", intensity, recommendation.c_str());
  render_ghost(canvas, mood, intensity, recommendation, tag);
  commit_canvas();

  go_to_deep_sleep();
}

void loop() {}  // never reached - setup() always ends in deep sleep
