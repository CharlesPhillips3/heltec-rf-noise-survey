// =====================================================================
//  Heltec WiFi LoRa 32 V3 — 902-928 MHz RF noise-floor survey logger
//  RECEIVE ONLY: the radio never transmits.
//
//  - Sweeps SWEEP_CHANNELS channels with the SX1262 in FSK RX mode and
//    reads instantaneous RSSI (GetRssiInst), power-averaged.
//  - Every LOG_INTERVAL_MS writes one CSV row to flash (LittleFS):
//    time, optional GPS position, summary stats, per-channel dBm.
//  - Wi-Fi hotspot "RF-Survey": live spectrum, clock sync from phone,
//    GO station control, spot markers, CSV download.
//  - GO station: while parked, averages STATION_SECONDS and saves a
//    baseline record (per-channel mean/min/max) for that spot.
//  - Bluetooth: the Bluefy control page on the iPhone connects, streams
//    the phone's GPS position + clock, and runs GO / Mark / live view.
//  - PRG button: short press = GO (press again to stop early),
//    hold 2 s = Wi-Fi on/off.
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <SSD1306Wire.h>
#include <TinyGPSPlus.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "config.h"
#include "webpage.h"
#if BLE_ENABLE
#include <NimBLEDevice.h>
#endif

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY, SPI);
SSD1306Wire display(0x3c, OLED_SDA, OLED_SCL, GEOMETRY_128_64);
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);
WebServer server(80);
Preferences prefs;

// ---------- state ----------
static uint32_t bootId = 0;
static bool     radioOk = false;
static bool     fsOk = false;
static bool     logging = true;
static bool     wifiOn = false;
static uint32_t rowsThisBoot = 0;

static int64_t  epochOffsetMs = 0;   // epoch_ms = epochOffsetMs + millis()
static bool     clockSynced = false;
static char     syncSource[8] = "none";

static float    sweepDbm[SWEEP_CHANNELS];     // last sweep, per channel
static double   winPow[SWEEP_CHANNELS];       // window accumulator (mW)
static uint32_t winSweeps = 0;
static float    winRawMax = -200, winRawMaxFreq = 0;
static uint32_t winStart = 0;

// last logged row (for display / web)
static float lastFloor = 0, lastMedian = 0, lastPeak = 0, lastPeakFreq = 0;
static int   lastBusy = 0;
static float lastSpec[SWEEP_CHANNELS];

// GO / station baseline
static bool     stnActive = false;
static uint32_t stnId = 0, stnStartMs = 0, stnEndAt = 0, stnSweeps = 0;
static double   stnPow[SWEEP_CHANNELS];
static float    stnMin[SWEEP_CHANNELS], stnMax[SWEEP_CHANNELS];
static float    stnRawMax = -200, stnRawMaxFreq = 0;
static String   stnNote;
static bool     lastStnValid = false;
static uint32_t lastStnId = 0;
static float    lastStnFloor = 0, lastStnMedian = 0, lastStnPeak = 0, lastStnPeakFreq = 0;
static int      lastStnBusy = 0;
static uint32_t lastStnSweeps = 0;

// phone (Bluefy) position, updated over Bluetooth
static double   phoneLat = 0, phoneLon = 0;
static float    phoneAcc = 0, phoneAlt = 0;
static uint32_t phoneAt = 0;           // millis() of last position, 0 = never
static bool phonePosFresh() { return phoneAt && (millis() - phoneAt) < PHONE_POS_MAX_AGE_MS; }

// Bluetooth helpers (defined further down)
static void bleSendStatus();
static void bleSendSpectrum(uint8_t type, uint8_t id, const float* v);
static void bleSendStation(uint32_t id, float floorP20, float median, float peak, float peakF,
                           int busy, uint32_t sweeps, bool early, uint32_t secs);
static volatile bool bleConnected = false;
static char bleName[24] = "";
static bool radioSleeping = false;     // receiver powered down (Bluetooth/Wi-Fi stay on)

// battery
static float    battV = 0;            // volts
static int      battPct = -1;         // -1 = no battery detected
static int      adcCtrlOn = -1;       // GPIO37 level that enables the divider, -1 = not yet known
static uint32_t battAt = 0;

// button
static uint32_t btnDownAt = 0;
static bool     btnHandled = false;
static uint32_t flashMsgUntil = 0;
static char     flashMsg[24] = "";

// ---------- helpers ----------
static inline float chFreq(int i) { return SWEEP_START_MHZ + SWEEP_STEP_MHZ * i; }

static int64_t nowEpochMs() { return clockSynced ? epochOffsetMs + (int64_t)millis() : 0; }

static void showFlash(const char* m) {
  strncpy(flashMsg, m, sizeof(flashMsg) - 1);
  flashMsg[sizeof(flashMsg) - 1] = 0;
  flashMsgUntil = millis() + 2500;
}

// days since 1970-01-01 for a civil date (Howard Hinnant's algorithm)
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static void appendLine(const String& line) {
  if (!fsOk) return;
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  f.println(line);
  f.close();
}

static void writeHeaderIfNeeded() {
  if (!fsOk) return;
  bool exists = LittleFS.exists(LOG_PATH);
  File f = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  if (!exists || f.size() == 0) {
    f.println("#RFSURVEY,v1");
    String h = "boot,millis,utc_ms,lat,lon,alt_m,sats,hdop,sweeps,floor_p20_dbm,median_dbm,peak_dbm,peak_mhz,busy";
    for (int i = 0; i < SWEEP_CHANNELS; i++) { h += ",f"; h += String(chFreq(i), 1); }
    f.println(h);
  }
  // config line every boot so the analysis script knows the plan
  f.printf("#CONFIG,%lu,start=%.3f,step=%.3f,n=%d,rxbw=%.1f,samples=%d,interval_ms=%d\n",
           (unsigned long)bootId, SWEEP_START_MHZ, SWEEP_STEP_MHZ, SWEEP_CHANNELS,
           RX_BW_KHZ, SAMPLES_PER_CH, LOG_INTERVAL_MS);
  f.close();
}

static void setClock(int64_t epochMs, const char* src) {
  epochOffsetMs = epochMs - (int64_t)millis();
  clockSynced = true;
  strncpy(syncSource, src, sizeof(syncSource) - 1);
  char buf[96];
  snprintf(buf, sizeof(buf), "#SYNC,%lu,%lu,%lld,%s", (unsigned long)bootId,
           (unsigned long)millis(), (long long)epochMs, src);
  appendLine(buf);
  Serial.println(buf);
}

static void addMark(const String& noteIn) {
  String note = noteIn;
  note.replace(",", ";");
  note.replace("\n", " ");
  char head[64];
  snprintf(head, sizeof(head), "#MARK,%lu,%lu,%lld,", (unsigned long)bootId,
           (unsigned long)millis(), (long long)nowEpochMs());
  appendLine(String(head) + note);
  Serial.print(head); Serial.println(note);
  showFlash("MARK saved");
}

// ---------- radio ----------
static void sweepOnce() {
  for (int i = 0; i < SWEEP_CHANNELS; i++) {
    const float f = chFreq(i);
    radio.standby();
    radio.setFrequency(f, true);       // image cal done once at begin (902-928 band)
    radio.startReceive();
    delayMicroseconds(SETTLE_US);
    double p = 0;
    for (int s = 0; s < SAMPLES_PER_CH; s++) {
      float r = radio.getRSSI(false);  // instantaneous RSSI, 0.5 dB steps
      p += pow(10.0, r / 10.0);
      if (r > winRawMax) { winRawMax = r; winRawMaxFreq = f; }
      if (stnActive && r > stnRawMax) { stnRawMax = r; stnRawMaxFreq = f; }
      if (s + 1 < SAMPLES_PER_CH) delayMicroseconds(SAMPLE_GAP_US);
    }
    p /= SAMPLES_PER_CH;
    sweepDbm[i] = 10.0 * log10(p);
    winPow[i] += p;
    if (stnActive) {
      stnPow[i] += p;
      if (sweepDbm[i] < stnMin[i]) stnMin[i] = sweepDbm[i];
      if (sweepDbm[i] > stnMax[i]) stnMax[i] = sweepDbm[i];
    }
  }
  radio.standby();
  winSweeps++;
  if (stnActive) stnSweeps++;
}

static int cmpFloat(const void* a, const void* b);

// ---------- GO / station baseline ----------
static void startStation(const String& noteIn) {
  stnNote = noteIn;
  stnNote.replace(",", ";");
  stnNote.replace("\n", " ");
  stnId++;
  for (int i = 0; i < SWEEP_CHANNELS; i++) { stnPow[i] = 0; stnMin[i] = 999; stnMax[i] = -999; }
  stnSweeps = 0; stnRawMax = -200; stnRawMaxFreq = 0;
  stnStartMs = millis();
  stnEndAt = stnStartMs + (uint32_t)STATION_SECONDS * 1000UL;
  stnActive = true;
  Serial.printf("STATION %lu started (%d s)\n", (unsigned long)stnId, STATION_SECONDS);
  showFlash("GO - hold still");
}

static void finishStation(bool early) {
  if (!stnActive) return;
  stnActive = false;
  uint32_t endMs = millis();
  if (stnSweeps == 0) { showFlash("STATION empty"); return; }

  static float mean[SWEEP_CHANNELS], sorted[SWEEP_CHANNELS];
  for (int i = 0; i < SWEEP_CHANNELS; i++) {
    mean[i] = 10.0 * log10(stnPow[i] / stnSweeps);
    sorted[i] = mean[i];
  }
  qsort(sorted, SWEEP_CHANNELS, sizeof(float), cmpFloat);
  float floorP20 = sorted[(SWEEP_CHANNELS * 20) / 100];
  float median   = sorted[SWEEP_CHANNELS / 2];
  int busy = 0;
  for (int i = 0; i < SWEEP_CHANNELS; i++) if (mean[i] > floorP20 + BUSY_MARGIN_DB) busy++;

  // #STATION,boot,id,start_millis,end_millis,start_utc_ms,end_utc_ms,sweeps,floor,median,peak,peak_mhz,busy,early,note
  String s;
  s.reserve(160);
  s += "#STATION,"; s += bootId; s += ','; s += stnId; s += ',';
  s += stnStartMs; s += ','; s += endMs; s += ',';
  if (clockSynced) s += String((long long)(epochOffsetMs + (int64_t)stnStartMs));
  s += ',';
  if (clockSynced) s += String((long long)(epochOffsetMs + (int64_t)endMs));
  s += ','; s += stnSweeps;
  s += ','; s += String(floorP20, 1);
  s += ','; s += String(median, 1);
  s += ','; s += String(stnRawMax, 1);
  s += ','; s += String(stnRawMaxFreq, 1);
  s += ','; s += busy;
  s += ','; s += early ? 1 : 0;
  s += ','; s += stnNote;
  appendLine(s);
  Serial.println(s);

  const char* kinds[3] = {"#STATION_MEAN,", "#STATION_MIN,", "#STATION_MAX,"};
  const float* arrs[3] = {mean, stnMin, stnMax};
  for (int k = 0; k < 3; k++) {
    String l;
    l.reserve(40 + SWEEP_CHANNELS * 7);
    l += kinds[k]; l += bootId; l += ','; l += stnId;
    for (int i = 0; i < SWEEP_CHANNELS; i++) { l += ','; l += String(arrs[k][i], 1); }
    appendLine(l);
  }

  lastStnValid = true; lastStnId = stnId;
  lastStnFloor = floorP20; lastStnMedian = median;
  lastStnPeak = stnRawMax; lastStnPeakFreq = stnRawMaxFreq;
  lastStnBusy = busy; lastStnSweeps = stnSweeps;
  bleSendStation(stnId, floorP20, median, stnRawMax, stnRawMaxFreq, busy, stnSweeps, early,
                 (endMs - stnStartMs) / 1000);
  bleSendSpectrum(1, (uint8_t)stnId, mean);
  char m[24];
  snprintf(m, sizeof(m), "STN %lu saved %.1f", (unsigned long)stnId, floorP20);
  showFlash(m);
}

static void checkStation() {
  if (stnActive && (int32_t)(millis() - stnEndAt) >= 0) finishStation(false);
}

static int cmpFloat(const void* a, const void* b) {
  float x = *(const float*)a, y = *(const float*)b;
  return (x > y) - (x < y);
}

static void finishWindow() {
  if (winSweeps == 0) return;
  static float sorted[SWEEP_CHANNELS];
  for (int i = 0; i < SWEEP_CHANNELS; i++) {
    lastSpec[i] = 10.0 * log10(winPow[i] / winSweeps);
    sorted[i] = lastSpec[i];
  }
  qsort(sorted, SWEEP_CHANNELS, sizeof(float), cmpFloat);
  lastFloor  = sorted[(SWEEP_CHANNELS * 20) / 100];
  lastMedian = sorted[SWEEP_CHANNELS / 2];
  lastPeak = winRawMax; lastPeakFreq = winRawMaxFreq;
  lastBusy = 0;
  for (int i = 0; i < SWEEP_CHANNELS; i++) if (lastSpec[i] > lastFloor + BUSY_MARGIN_DB) lastBusy++;

  bool gpsFix = gps.location.isValid() && gps.location.age() < 3000;

  if (logging && fsOk) {
    if (LittleFS.totalBytes() - LittleFS.usedBytes() < 64 * 1024) {
      logging = false;
      showFlash("STORAGE FULL");
    } else {
      String row;
      row.reserve(80 + SWEEP_CHANNELS * 7);
      row += bootId; row += ','; row += millis(); row += ',';
      if (clockSynced) row += String((long long)nowEpochMs());
      row += ',';
      if (gpsFix) {
        row += String(gps.location.lat(), 7); row += ',';
        row += String(gps.location.lng(), 7); row += ',';
        row += String(gps.altitude.meters(), 1); row += ',';
        row += gps.satellites.value(); row += ',';
        row += String(gps.hdop.hdop(), 1);
      } else if (phonePosFresh()) {   // iPhone position via Bluetooth (sats/hdop left blank)
        row += String(phoneLat, 7); row += ',';
        row += String(phoneLon, 7); row += ',';
        row += String(phoneAlt, 1); row += ",,";
      } else {
        row += ",,,,";
      }
      row += ','; row += winSweeps;
      row += ','; row += String(lastFloor, 1);
      row += ','; row += String(lastMedian, 1);
      row += ','; row += String(lastPeak, 1);
      row += ','; row += String(lastPeakFreq, 1);
      row += ','; row += lastBusy;
      for (int i = 0; i < SWEEP_CHANNELS; i++) { row += ','; row += String(lastSpec[i], 1); }
      appendLine(row);
      rowsThisBoot++;
    }
  }

  Serial.printf("[%lu] floor %.1f  med %.1f  peak %.1f @ %.1f MHz  busy %d  sweeps %lu  gps %s\n",
                (unsigned long)rowsThisBoot, lastFloor, lastMedian, lastPeak, lastPeakFreq,
                lastBusy, (unsigned long)winSweeps, gpsFix ? "fix" : "-");

  bleSendStatus();
  bleSendSpectrum(0, 0, lastSpec);

  for (int i = 0; i < SWEEP_CHANNELS; i++) winPow[i] = 0;
  winSweeps = 0; winRawMax = -200; winRawMaxFreq = 0;
}

// ---------- GPS ----------
static void feedGps() {
#if GPS_ENABLE
  while (gpsSerial.available()) gps.encode(gpsSerial.read());
  // phone sync wins; GPS sets the clock only if nothing else has
  if (!clockSynced && gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2024 &&
      gps.time.age() < 1000) {
    int64_t days = daysFromCivil(gps.date.year(), gps.date.month(), gps.date.day());
    int64_t ms = ((days * 24 + gps.time.hour()) * 60 + gps.time.minute()) * 60 + gps.time.second();
    ms = ms * 1000 + gps.time.centisecond() * 10 + gps.time.age();
    setClock(ms, "gps");
  }
#endif
}

// ---------- display ----------
static void drawDisplay() {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  if (!radioOk) { display.drawString(0, 0, "RADIO INIT FAILED"); display.display(); return; }

  char l[40];
  snprintf(l, sizeof(l), "Floor %.1f  Med %.1f", lastFloor, lastMedian);
  display.drawString(0, 0, l);
  if (stnActive) {
    int32_t left = (int32_t)(stnEndAt - millis()) / 1000;
    if (left < 0) left = 0;
    snprintf(l, sizeof(l), ">> GO STN %lu  %lds left", (unsigned long)stnId, (long)left);
  } else {
    if (radioSleeping) snprintf(l, sizeof(l), "RADIO ASLEEP (not logging)");
    else snprintf(l, sizeof(l), "Pk %.1f @%.1f B%d", lastPeak, lastPeakFreq, lastBusy);
  }
  display.drawString(0, 10, l);

  // mini spectrum, -130..-60 dBm mapped to 26 px
  const int top = 22, h = 26;
  for (int x = 0; x < 128; x++) {
    int i = (x * SWEEP_CHANNELS) / 128;
    float v = lastSpec[i];
    int bar = (int)((v + 130.0f) * h / 70.0f);
    if (bar < 0) bar = 0; if (bar > h) bar = h;
    display.drawVerticalLine(x, top + h - bar, bar);
  }

  bool gpsFix = gps.location.isValid() && gps.location.age() < 3000;
  if (millis() < flashMsgUntil) {
    display.drawString(0, 52, flashMsg);
  } else if (!clockSynced && !bleConnected) {
    // until a phone has connected: cycle through how to connect
    switch ((millis() / 2500) % 4) {
      case 0:  display.drawString(0, 52, String("BT: ") + bleName); break;
      case 1:  display.drawString(0, 52, wifiOn ? "WiFi: " AP_SSID : "WiFi off"); break;
      case 2:  display.drawString(0, 52, "Pass: " AP_PASS); break;
      default: display.drawString(0, 52, String("Open: ") + WiFi.softAPIP().toString()); break;
    }
  } else {
    snprintf(l, sizeof(l), "%s %s%s %s %d%%",
             clockSynced ? "CLK" : "clk?",
             bleConnected ? "BT" : "",
             gpsFix ? " GPS" : (phonePosFresh() ? " POS" : (bleConnected ? " pos?" : "")),
             wifiOn ? "WiFi" : "",
             fsOk ? (int)(100.0 * LittleFS.usedBytes() / LittleFS.totalBytes()) : 0);
    display.drawString(0, 52, l);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    char r[20];
    const char* stState = radioSleeping ? "SLEEP" : (stnActive ? "GO" : (logging ? "REC" : "PAUSE"));
    if (battPct >= 0) snprintf(r, sizeof(r), "%s %d%%", stState, battPct);
    else snprintf(r, sizeof(r), "%s USB", stState);    // no battery: running on USB power
    display.drawString(128, 52, r);
  }
  display.display();
}

// ---------- battery ----------
static int battPercent(float v) {
  if (v < 2.5f) return -1;                     // nothing connected
  const float V[] = {3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  const int   P[] = {0,     5,     15,    30,    50,    65,    80,    90,    100};
  if (v <= V[0]) return 0;
  for (int i = 1; i < 9; i++) if (v <= V[i]) return P[i-1] + (int)((v - V[i-1]) / (V[i] - V[i-1]) * (P[i] - P[i-1]));
  return 100;
}

static void readBattery() {
  float best = 0; int bestLevel = LOW;
  for (int pass = 0; pass < 2; pass++) {
    if (adcCtrlOn >= 0 && pass > 0) break;     // polarity already known: one read
    int level = adcCtrlOn >= 0 ? adcCtrlOn : (pass ? HIGH : LOW);
    digitalWrite(ADC_CTRL_PIN, level);
    delay(10);
    uint32_t mv = 0;
    for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(VBAT_PIN);
    float v = (mv / 8) * VBAT_DIVIDER / 1000.0f;
    if (v > best) { best = v; bestLevel = level; }
  }
  if (adcCtrlOn < 0 && best > 2.5f) adcCtrlOn = bestLevel;
  if (adcCtrlOn >= 0) digitalWrite(ADC_CTRL_PIN, adcCtrlOn == HIGH ? LOW : HIGH);   // divider off between reads
  battV = best;
  battPct = battPercent(best);
}

// ---------- USB serial commands (used by the flasher) ----------
static void dumpLog() {
  File f;
  if (fsOk) f = LittleFS.open(LOG_PATH, FILE_READ);
  size_t n = f ? f.size() : 0;
  Serial.printf("\n<<<RFSURVEY-DUMP %u>>>\n", (unsigned)n);
  if (f) {
    uint8_t buf[1024];
    size_t left = n;
    while (left) {
      size_t r = f.read(buf, left < sizeof(buf) ? left : sizeof(buf));
      if (!r) break;
      Serial.write(buf, r);
      left -= r;
    }
    f.close();
  }
  Serial.print("\n<<<RFSURVEY-END>>>\n");
  Serial.flush();
}

static void processSerial() {
  static char line[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[len] = 0; len = 0;
      if (strcmp(line, "DUMP") == 0) dumpLog();
      else if (strcmp(line, "PING") == 0) Serial.printf("PONG %s boot %lu\n", bleName, (unsigned long)bootId);
    } else if (len < sizeof(line) - 1) {
      line[len++] = ch;
    }
  }
}

// ---------- erase log / radio sleep ----------
static void eraseLog() {
  if (stnActive) stnActive = false;            // drop a station in progress
  LittleFS.remove(LOG_PATH);
  rowsThisBoot = 0;
  writeHeaderIfNeeded();
  if (clockSynced) {   // keep the current sync in the new file
    char buf[96];
    snprintf(buf, sizeof(buf), "#SYNC,%lu,%lu,%lld,%s", (unsigned long)bootId,
             (unsigned long)millis(), (long long)nowEpochMs(), syncSource);
    appendLine(buf);
  }
  Serial.println("Log erased");
  showFlash("Storage erased");
}

static void setRadioSleep(bool sleepNow) {
  if (!radioOk || sleepNow == radioSleeping) return;
  if (sleepNow) {
    if (stnActive) finishStation(true);        // save what the station has so far
    radio.standby();
    radio.sleep(true);                          // warm sleep: settings kept
    radioSleeping = true;
    digitalWrite(LED_PIN, LOW);
    showFlash("Radio asleep");
  } else {
    radio.standby();                            // any command wakes the SX1262
    radioSleeping = false;
    winStart = millis();
    showFlash("Radio awake");
  }
  Serial.println(radioSleeping ? "Radio sleeping" : "Radio awake");
}

// ---------- web ----------
static void handleStatus() {
  bool gpsFix = gps.location.isValid() && gps.location.age() < 3000;
  String j;
  j.reserve(200 + SWEEP_CHANNELS * 5);
  j += "{\"boot\":"; j += bootId;
  j += ",\"synced\":"; j += clockSynced ? "true" : "false";
  j += ",\"logging\":"; j += logging ? "true" : "false";
  j += ",\"rows\":"; j += rowsThisBoot;
  j += ",\"floor\":"; j += String(lastFloor, 1);
  j += ",\"median\":"; j += String(lastMedian, 1);
  j += ",\"peak\":"; j += String(lastPeak, 1);
  j += ",\"peakFreq\":"; j += String(lastPeakFreq, 1);
  j += ",\"busy\":"; j += lastBusy;
  j += ",\"gpsChars\":"; j += gps.charsProcessed();
  j += ",\"gpsFix\":"; j += gpsFix ? "true" : "false";
  j += ",\"sats\":"; j += gps.satellites.isValid() ? gps.satellites.value() : 0;
  j += ",\"fsUsed\":"; j += fsOk ? LittleFS.usedBytes() : 0;
  j += ",\"fsTotal\":"; j += fsOk ? LittleFS.totalBytes() : 1;
  j += ",\"f0\":"; j += String(SWEEP_START_MHZ, 3);
  j += ",\"step\":"; j += String(SWEEP_STEP_MHZ, 3);
  j += ",\"stnActive\":"; j += stnActive ? "true" : "false";
  j += ",\"stnId\":"; j += stnId;
  j += ",\"stnSecs\":"; j += STATION_SECONDS;
  {
    int32_t left = stnActive ? (int32_t)(stnEndAt - millis()) / 1000 : 0;
    j += ",\"stnLeft\":"; j += (left < 0 ? 0 : left);
  }
  j += ",\"stnSweeps\":"; j += stnSweeps;
  if (lastStnValid) {
    j += ",\"last\":{\"id\":"; j += lastStnId;
    j += ",\"floor\":"; j += String(lastStnFloor, 1);
    j += ",\"median\":"; j += String(lastStnMedian, 1);
    j += ",\"peak\":"; j += String(lastStnPeak, 1);
    j += ",\"peakFreq\":"; j += String(lastStnPeakFreq, 1);
    j += ",\"busy\":"; j += lastStnBusy;
    j += ",\"sweeps\":"; j += lastStnSweeps;
    j += "}";
  }
  j += ",\"spec\":[";
  for (int i = 0; i < SWEEP_CHANNELS; i++) { if (i) j += ','; j += String(lastSpec[i], 0); }
  j += "]}";
  server.send(200, "application/json", j);
}

static void startWifi() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  server.on("/", []() { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", handleStatus);
  server.on("/api/sync", []() {
    if (!server.hasArg("t")) { server.send(400, "text/plain", "missing t"); return; }
    int64_t t = atoll(server.arg("t").c_str());
    if (t < 1700000000000LL) { server.send(400, "text/plain", "bad time"); return; }
    setClock(t, "phone");
    server.send(200, "text/plain", "Clock synced from phone");
  });
  server.on("/api/go", []() {
    if (stnActive) {
      finishStation(true);
      server.send(200, "text/plain", "Station stopped early and saved");
    } else {
      startStation(server.hasArg("note") ? server.arg("note") : String(""));
      server.send(200, "text/plain", "GO - baseline running, keep still");
    }
  });
  server.on("/api/mark", []() {
    addMark(server.hasArg("note") ? server.arg("note") : String(""));
    server.send(200, "text/plain", "Spot marked");
  });
  server.on("/api/log", []() {
    logging = server.arg("on") == "1";
    server.send(200, "text/plain", logging ? "Logging ON" : "Logging PAUSED");
  });
  server.on("/api/clear", HTTP_POST, []() {
    eraseLog();
    server.send(200, "text/plain", "Log cleared");
  });
  server.on("/api/radio", []() {
    setRadioSleep(server.arg("on") == "0");
    server.send(200, "text/plain", radioSleeping ? "Radio asleep" : "Radio awake");
  });
  server.on("/log.csv", []() {
    File f = LittleFS.open(LOG_PATH, FILE_READ);
    if (!f) { server.send(404, "text/plain", "no log"); return; }
    char name[48];
    snprintf(name, sizeof(name), "attachment; filename=\"rfsurvey_boot%lu.csv\"", (unsigned long)bootId);
    server.sendHeader("Content-Disposition", name);
    server.streamFile(f, "text/csv");
    f.close();
  });
  server.begin();
  wifiOn = true;
  Serial.printf("Wi-Fi AP '%s' up at %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

static void stopWifi() {
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
  Serial.println("Wi-Fi off");
}

// ---------- button ----------
static void pollButton() {
  bool down = digitalRead(BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (down) {
    if (btnDownAt == 0) { btnDownAt = now; btnHandled = false; }
    else if (!btnHandled && now - btnDownAt > 2000) {
      btnHandled = true;
      if (wifiOn) { stopWifi(); showFlash("WiFi OFF"); } else { startWifi(); showFlash("WiFi ON 192.168.4.1"); }
    }
  } else if (btnDownAt) {
    if (!btnHandled && now - btnDownAt > 40) {
      if (stnActive) finishStation(true); else startStation("button");
    }
    btnDownAt = 0;
  }
}

// ---------- Bluetooth (NimBLE) ----------
// Service for the Bluefy control page. Commands are short text writes:
//   T<epoch_ms>                 set clock from phone
//   P<lat>,<lon>,<acc_m>,<alt>  phone position
//   G<label>                    GO (start station) / stop & save if running
//   M<note>                     mark spot
//   L0 / L1                     pause / resume logging
//   W0 / W1                     Wi-Fi off / on
// Notifications: STAT = compact JSON, SPEC = [type, id, 130 x uint8 (dBm+160)*2]
#define BLE_SVC_UUID  "8a1c0001-5b2f-4c1e-9a6d-2f0b6e3c9d10"
#define BLE_CMD_UUID  "8a1c0002-5b2f-4c1e-9a6d-2f0b6e3c9d10"
#define BLE_STAT_UUID "8a1c0003-5b2f-4c1e-9a6d-2f0b6e3c9d10"
#define BLE_SPEC_UUID "8a1c0004-5b2f-4c1e-9a6d-2f0b6e3c9d10"

#if BLE_ENABLE
static NimBLECharacteristic* chStat = nullptr;
static NimBLECharacteristic* chSpec = nullptr;
static portMUX_TYPE cmdMux = portMUX_INITIALIZER_UNLOCKED;
static char cmdBuf[12][160];
static volatile uint8_t cmdHead = 0, cmdTail = 0;
static volatile bool bleJustConnected = false;

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s) override { bleConnected = true; bleJustConnected = true; }
  void onDisconnect(NimBLEServer* s) override { bleConnected = false; NimBLEDevice::startAdvertising(); }
};
class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {   // runs in the BLE task: just queue it
    NimBLEAttValue v = c->getValue();
    size_t n = v.length(); if (n > 159) n = 159;
    portENTER_CRITICAL(&cmdMux);
    uint8_t next = (cmdHead + 1) % 12;
    if (next != cmdTail) { memcpy(cmdBuf[cmdHead], v.data(), n); cmdBuf[cmdHead][n] = 0; cmdHead = next; }
    portEXIT_CRITICAL(&cmdMux);
  }
};

static void bleSetup() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(bleName, sizeof(bleName), "%s%02X%02X", BLE_NAME_PREFIX,
           (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  NimBLEDevice::init(bleName);
  NimBLEDevice::setMTU(247);
  NimBLEServer* srv = NimBLEDevice::createServer();
  srv->setCallbacks(new SrvCB());
  NimBLEService* svc = srv->createService(BLE_SVC_UUID);
  NimBLECharacteristic* cmd = svc->createCharacteristic(BLE_CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  cmd->setCallbacks(new CmdCB());
  chStat = svc->createCharacteristic(BLE_STAT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  chSpec = svc->createCharacteristic(BLE_SPEC_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  adv->start();
  Serial.printf("Bluetooth up as %s\n", bleName);
}

static void bleSendStatus() {
  if (!chStat || !bleConnected) return;
  char j[200];
  int32_t left = stnActive ? (int32_t)(stnEndAt - millis()) / 1000 : 0;
  if (left < 0) left = 0;
  long pa = phoneAt ? (long)((millis() - phoneAt) / 1000) : -1;
  snprintf(j, sizeof(j),
           "{\"t\":\"s\",\"f\":%.1f,\"m\":%.1f,\"p\":%.1f,\"pf\":%.1f,\"b\":%d,\"sy\":%d,\"lg\":%d,"
           "\"sa\":%d,\"si\":%lu,\"sl\":%ld,\"sn\":%d,\"r\":%lu,\"fs\":%d,\"pa\":%ld,\"w\":%d,\"bt\":%lu,\"rs\":%d,\"bv\":%d}",
           lastFloor, lastMedian, lastPeak, lastPeakFreq, lastBusy, clockSynced ? 1 : 0, logging ? 1 : 0,
           stnActive ? 1 : 0, (unsigned long)stnId, (long)left, STATION_SECONDS,
           (unsigned long)rowsThisBoot,
           fsOk ? (int)(100.0 * LittleFS.usedBytes() / LittleFS.totalBytes()) : -1,
           pa, wifiOn ? 1 : 0, (unsigned long)bootId, radioSleeping ? 1 : 0, (int)(battV * 1000));
  chStat->setValue((uint8_t*)j, strlen(j));
  chStat->notify();
}

static void bleSendSpectrum(uint8_t type, uint8_t id, const float* v) {
  if (!chSpec || !bleConnected) return;
  uint8_t b[2 + SWEEP_CHANNELS];
  b[0] = type; b[1] = id;
  for (int i = 0; i < SWEEP_CHANNELS; i++) {
    int q = (int)lroundf((v[i] + 160.0f) * 2.0f);
    b[2 + i] = (uint8_t)(q < 0 ? 0 : (q > 255 ? 255 : q));
  }
  chSpec->setValue(b, sizeof(b));
  chSpec->notify();
}

static void bleSendStation(uint32_t id, float floorP20, float median, float peak, float peakF,
                           int busy, uint32_t sweeps, bool early, uint32_t secs) {
  if (!chStat || !bleConnected) return;
  char j[200];
  snprintf(j, sizeof(j),
           "{\"t\":\"st\",\"id\":%lu,\"f\":%.1f,\"m\":%.1f,\"p\":%.1f,\"pf\":%.1f,\"b\":%d,\"sw\":%lu,\"e\":%d,\"d\":%lu}",
           (unsigned long)id, floorP20, median, peak, peakF, busy, (unsigned long)sweeps, early ? 1 : 0,
           (unsigned long)secs);
  chStat->setValue((uint8_t*)j, strlen(j));
  chStat->notify();
}

static void handleCommand(const char* c) {
  switch (c[0]) {
    case 'T': {
      int64_t t = atoll(c + 1);
      // log a sync only the first time or if the clock is off by > 2 s
      if (t > 1700000000000LL && (!clockSynced || llabs(nowEpochMs() - t) > 2000)) setClock(t, "phone");
      break;
    }
    case 'P': {
      char* e;
      double la = strtod(c + 1, &e); if (*e != ',') break;
      double lo = strtod(e + 1, &e); if (*e != ',') break;
      float acc = strtof(e + 1, &e);
      float alt = (*e == ',') ? strtof(e + 1, &e) : 0;
      if (la < -90 || la > 90 || lo < -180 || lo > 180 || (la == 0 && lo == 0)) break;
      phoneLat = la; phoneLon = lo; phoneAcc = acc; phoneAlt = alt; phoneAt = millis();
      break;
    }
    case 'G': if (stnActive) finishStation(true); else startStation(String(c + 1)); bleSendStatus(); break;
    case 'M': addMark(String(c + 1)); break;
    case 'L': logging = c[1] == '1'; bleSendStatus(); break;
    case 'W': if (c[1] == '1' && !wifiOn) startWifi(); else if (c[1] == '0' && wifiOn) stopWifi(); bleSendStatus(); break;
    case 'E': if (strcmp(c, "ERASE") == 0) { eraseLog(); bleSendStatus(); } break;   // full word required
    case 'Z': setRadioSleep(c[1] == '1'); bleSendStatus(); break;                     // Z1 sleep, Z0 wake
  }
}

static void processBle() {
  if (bleJustConnected) { bleJustConnected = false; showFlash("Phone connected"); }
  while (true) {
    char c[160];
    bool have = false;
    portENTER_CRITICAL(&cmdMux);
    if (cmdTail != cmdHead) { memcpy(c, cmdBuf[cmdTail], sizeof(c)); cmdTail = (cmdTail + 1) % 12; have = true; }
    portEXIT_CRITICAL(&cmdMux);
    if (!have) break;
    handleCommand(c);
  }
}
#else
static void bleSetup() {}
static void processBle() {}
static void bleSendStatus() {}
static void bleSendSpectrum(uint8_t, uint8_t, const float*) {}
static void bleSendStation(uint32_t, float, float, float, float, int, uint32_t, bool, uint32_t) {}
#endif

// =====================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(ADC_CTRL_PIN, OUTPUT);
  analogSetPinAttenuation(VBAT_PIN, ADC_11db);

  // power OLED + external 3V3 (GPS)
  pinMode(VEXT_CTRL, OUTPUT); digitalWrite(VEXT_CTRL, LOW);
  delay(50);
  pinMode(OLED_RST, OUTPUT);
  digitalWrite(OLED_RST, LOW); delay(20); digitalWrite(OLED_RST, HIGH); delay(20);
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.drawString(0, 0, "RF Survey 902-928");
  display.drawString(0, 12, "starting...");
  display.display();

  prefs.begin("rfsurvey", false);
  bootId = prefs.getUInt("boot", 0) + 1;
  prefs.putUInt("boot", bootId);
  prefs.end();

  fsOk = LittleFS.begin(true);
  Serial.printf("LittleFS %s  %u/%u bytes\n", fsOk ? "ok" : "FAILED",
                fsOk ? (unsigned)LittleFS.usedBytes() : 0, fsOk ? (unsigned)LittleFS.totalBytes() : 0);
  writeHeaderIfNeeded();

#if GPS_ENABLE
  gpsSerial.setRxBufferSize(2048);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
#endif

  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_NSS);
  // FSK mode is used only as a wideband receiver; output power is irrelevant (never transmits)
  int st = radio.beginFSK(915.0, 4.8, 5.0, RX_BW_KHZ, 0, 16, LORA_TCXO_V, false);
  if (st != RADIOLIB_ERR_NONE) {
    Serial.printf("radio.beginFSK failed: %d\n", st);
  } else {
    radio.setDio2AsRfSwitch(true);
    radio.setRxBoostedGainMode(true);   // max LNA sensitivity
    radioOk = true;
  }

  for (int i = 0; i < SWEEP_CHANNELS; i++) { winPow[i] = 0; lastSpec[i] = -130; }
  bleSetup();
  if (WIFI_ON_AT_BOOT) {
    startWifi();
    // connection card for the phone, shown for a few seconds at power-up
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, "Bluefy app -> Connect:");
    display.drawString(0, 12, String("  ") + bleName);
    display.drawString(0, 26, "or WiFi: " AP_SSID);
    display.drawString(0, 38, "pw: " AP_PASS);
    display.drawString(0, 50, String("open ") + WiFi.softAPIP().toString());
    display.display();
    delay(5000);
  }
  winStart = millis();
  drawDisplay();
}

void loop() {
  feedGps();
  processBle();
  processSerial();
  if (battAt == 0 || millis() - battAt > 10000) { battAt = millis(); readBattery(); }
  pollButton();
  if (wifiOn) server.handleClient();

  if (radioOk && !radioSleeping) {
    sweepOnce();
    // white LED: solid during a GO station, otherwise off (short heartbeat blink every 2 s)
    digitalWrite(LED_PIN, stnActive ? HIGH : LOW);
    checkStation();
  } else {
    delay(200);
  }

  if (millis() - winStart >= LOG_INTERVAL_MS) {
    winStart = millis();
    if (radioSleeping) bleSendStatus();   // keep the phone updated, log nothing
    else finishWindow();
    drawDisplay();
    if (!stnActive && !radioSleeping) { digitalWrite(LED_PIN, HIGH); delay(15); digitalWrite(LED_PIN, LOW); }
  }
}
