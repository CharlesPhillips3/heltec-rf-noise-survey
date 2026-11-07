#pragma once
// ======================= USER SETTINGS =======================

// --- Sweep plan (902–928 MHz ISM band) ---
#define SWEEP_START_MHZ   902.1f   // first channel centre
#define SWEEP_STEP_MHZ    0.2f     // channel step
#define SWEEP_CHANNELS    130      // 902.1 ... 927.9 MHz
#define RX_BW_KHZ         187.2f   // SX1262 FSK RX bandwidth. Valid: ...117.3, 156.2, 187.2, 234.3, 312.0, 373.6, 467.0
#define SAMPLES_PER_CH    8        // RSSI reads per channel per sweep (power-averaged)
#define SETTLE_US         600      // wait after retune before reading RSSI
#define SAMPLE_GAP_US     120      // gap between RSSI reads

// --- Logging ---
#define LOG_INTERVAL_MS   2000     // sweeps are power-averaged into one CSV row this often
#define BUSY_MARGIN_DB    10.0f    // channel counted "busy" if > floor + this
#define LOG_PATH          "/survey.csv"

// --- GO / station baseline ---
// Press GO (PRG button or phone page) while parked: the board averages this long,
// then saves one "station" record (per-channel mean/min/max + floor stats).
#define STATION_SECONDS   60

// --- Wi-Fi hotspot (phone connects here for live view, clock sync, download) ---
#define AP_SSID           "RF-Survey"
#define AP_PASS           "survey1234"   // 8+ chars
#define WIFI_ON_AT_BOOT   true

// --- Bluetooth (for the Bluefy control page on the iPhone) ---
// The phone streams its GPS position and clock to the board, and controls GO/Mark.
// Board name shows as "RF-Survey-XXXX" (last 4 hex digits of its MAC).
#define BLE_ENABLE        true
#define BLE_NAME_PREFIX   "RF-Survey-"
#define PHONE_POS_MAX_AGE_MS 10000   // phone position older than this is not used

// --- Optional external GPS module (u-blox NEO-6M/M8N etc., 9600 baud NMEA) ---
// Not required: the iPhone GPX track can supply position instead.
// GPS TX -> GPIO47, GPS RX -> GPIO48, VCC -> 3V3 (Vext), GND -> GND
#define GPS_ENABLE        true
#define GPS_BAUD          9600
#define GPS_RX_PIN        47
#define GPS_TX_PIN        48

// ======================= HELTEC V3 PINS (do not change) =======================
#define LORA_NSS   8
#define LORA_SCK   9
#define LORA_MOSI  10
#define LORA_MISO  11
#define LORA_RST   12
#define LORA_BUSY  13
#define LORA_DIO1  14
#define LORA_TCXO_V 1.8f

#define OLED_SDA   17
#define OLED_SCL   18
#define OLED_RST   21
#define VEXT_CTRL  36   // LOW = Vext (OLED + external 3V3) on
#define LED_PIN    35
#define BUTTON_PIN 0    // PRG button
#define VBAT_PIN   1    // battery voltage via 390k/100k divider
#define ADC_CTRL_PIN 37 // switches the divider on (LOW on V3.0/3.1, HIGH on V3.2: auto-detected)
#define VBAT_DIVIDER 4.9f
#define SERIAL_BAUD 921600  // USB serial (log download from the flasher uses this)
