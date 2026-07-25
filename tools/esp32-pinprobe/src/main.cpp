// ============================================================================
//  Glance pin-probe  -  GeekMagic SmallTV Pro (ESP32-WROOM-32) hardware diag
// ----------------------------------------------------------------------------
//  Confirms, on a physical unit, three things needed to port Glance to the Pro:
//    1. TFT pins/driver   -> screen shows color bars + text if wiring is right
//    2. Touch button pin  -> which of the 10 ESP32 touch channels the case
//                            top surface is wired to (community says GPIO32)
//    3. A no-UART readout  -> everything is also served at http://pinprobe.local/
//
//  Nothing here is destructive: it only reads touch channels and writes to the
//  display. Restore the stock firmware via the same web updater at any time.
// ============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <Update.h>              // self-OTA: re-flash the probe without UART
#include <SPI.h>                 // manual ST7789 bring-up with runtime pins

// ---- ESP32 (classic) capacitive-touch channel -> GPIO map -------------------
// touchRead() is only valid on these 10 pins. We scan them all and let the
// hardware tell us which one the button is on, rather than trusting a guess.
struct TouchCh { const char* name; uint8_t gpio; };
static const TouchCh CH[] = {
  {"T0",  4}, {"T1",  0}, {"T2",  2}, {"T3", 15}, {"T4", 13},
  {"T5", 12}, {"T6", 14}, {"T7", 27}, {"T8", 33}, {"T9", 32},
};
static const int NCH = sizeof(CH) / sizeof(CH[0]);

// GPIO32 is the community-reported Pro button; highlight it so it's obvious.
static const uint8_t EXPECTED_TOUCH_GPIO = 32;

// A channel is "touched" when its reading drops well below its idle baseline.
// ESP32 touchRead falls toward 0 as capacitance rises (finger present).
static const float TOUCH_RATIO = 0.60f;   // < 60% of baseline => touched

static int      baseline[NCH];
static int      lastVal[NCH];
static int      minVal[NCH];               // lowest ever seen (catches brief dips)
static int      maxVal[NCH];               // highest ever seen (catches core-v3 rises)
static bool     touched[NCH];
static uint32_t touchCount[NCH];           // cumulative hits, survives glitches

// Detect a touch as a deviation from baseline in EITHER direction: Arduino-ESP32
// v2 lowers touchRead on contact, v3 raises it. Runtime-tunable via /thr since
// the right threshold is a feel thing; idle noise is ~2% so 8% is a safe start,
// low enough to catch a light fingertip. The value we settle on here is the one
// to bake into the real firmware's button handling.
static float touchDev = 0.08f;

static TFT_eSPI  tft;
static bool      tftOK = false;            // set true once init runs (best effort)
static WebServer server(80);
static bool      wifiOK = false;
static String    ipStr = "(offline)";

// Backlight-hunt state (interactive display bring-up over the web).
static String    blNote = "";              // last action, echoed on the page
static String    stNote = "";              // last ST7789 manual-test result
static bool      stActive = false;         // manual ST7789 test owns the panel
// Sweep: drive each candidate BL pin LOW for a moment, one at a time, so the
// user can watch the panel and catch which pin lights the backlight.
static const uint8_t BL_CANDS[] = {25, 27, 5, 32, 26, 4, 2, 16, 17, 33, 22, 21};
static const int     NBL = sizeof(BL_CANDS) / sizeof(BL_CANDS[0]);
static bool      sweepOn = false;
static int       sweepIdx = 0;
static uint32_t  sweepAt = 0;

static bool isFlashPin(int p) { return p >= 6 && p <= 11; }   // SPI flash: never touch

// ---- GPIO / peripheral discovery ------------------------------------------
// Monitor the unmapped pins: digital pins catch hidden buttons (read LOW when
// pressed, via INPUT_PULLUP); ADC1 pins catch a hidden light sensor (value
// tracks light). ADC2 can't be read while WiFi is on, and flash pins 6-11 plus
// the in-use pins (TFT 23/18/2/4, BL 25, touch 32) are excluded.
struct GpioMon { uint8_t gpio; bool adc; bool inputOnly; };
static const GpioMon GMON[] = {
  {17, false, false},   // <- the "Option A3 / unknown" pin from the Tasmota template
  {16, false, false}, { 5, false, false}, {13, false, false}, {14, false, false},
  {19, false, false}, {21, false, false}, {22, false, false}, {26, false, false},
  {27, false, false},
  {33, true,  false},   // ADC1, touch-capable, otherwise free
  {34, true,  true }, {35, true, true }, {36, true, true }, {39, true, true },  // ADC1 in-only
};
static const int NG = sizeof(GMON) / sizeof(GMON[0]);
static int gNow[NG], gMin[NG], gMax[NG];

static void gpioInit() {
  for (int i = 0; i < NG; i++) {
    if (GMON[i].adc)            pinMode(GMON[i].gpio, INPUT);          // ADC / input-only
    else if (!GMON[i].inputOnly) pinMode(GMON[i].gpio, INPUT_PULLUP);  // button detect
    gNow[i] = GMON[i].adc ? analogRead(GMON[i].gpio) : digitalRead(GMON[i].gpio);
    gMin[i] = gMax[i] = gNow[i];
  }
}
static void gpioSample() {
  for (int i = 0; i < NG; i++) {
    int v = GMON[i].adc ? analogRead(GMON[i].gpio) : digitalRead(GMON[i].gpio);
    gNow[i] = v;
    if (v < gMin[i]) gMin[i] = v;
    if (v > gMax[i]) gMax[i] = v;
  }
}

// ---------------------------------------------------------------------------
static void calibrate() {
  // Average several quiet samples per channel. Ask the user (via screen/serial)
  // NOT to touch the case during the first second after boot.
  for (int i = 0; i < NCH; i++) {
    long acc = 0;
    for (int s = 0; s < 16; s++) { acc += touchRead(CH[i].gpio); delay(6); }
    baseline[i] = acc / 16;
    lastVal[i]  = baseline[i];
    minVal[i]   = baseline[i];
    maxVal[i]   = baseline[i];
    touched[i]  = false;
    touchCount[i] = 0;
  }
}

static void sampleTouch() {
  for (int i = 0; i < NCH; i++) {
    int v = touchRead(CH[i].gpio);
    lastVal[i] = v;
    if (v < minVal[i]) minVal[i] = v;
    if (v > maxVal[i]) maxVal[i] = v;
    float dev = baseline[i] > 0 ? fabsf((float)v - baseline[i]) / baseline[i] : 0;
    bool now = dev > touchDev;                 // deviation either direction
    if (now && !touched[i]) touchCount[i]++;   // rising edge
    touched[i] = now;
  }
}

// Index of the channel most likely to be the button = biggest relative drop
// seen so far (by hit count, tie-broken by current depth below baseline).
static int bestChannel() {
  int best = -1; uint32_t bestHits = 0; float bestDev = 0;
  for (int i = 0; i < NCH; i++) {
    // biggest deviation ever seen (either direction) = strongest touch response
    float dev = baseline[i] > 0
      ? fmaxf(baseline[i] - minVal[i], maxVal[i] - baseline[i]) / (float)baseline[i]
      : 0;
    if (touchCount[i] > bestHits ||
        (touchCount[i] == bestHits && touchCount[i] > 0 && dev > bestDev)) {
      best = i; bestHits = touchCount[i]; bestDev = dev;
    }
  }
  return best;
}

// ---------------------------------------------------------------------------
static void drawTFT() {
  if (!tftOK) return;
  tft.fillScreen(TFT_BLACK);

  // Color bars: confirms driver, color order and inversion in one glance.
  const uint16_t bars[] = {TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE};
  for (int i = 0; i < 4; i++) tft.fillRect(i * 60, 0, 60, 24, bars[i]);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 30);  tft.print("SmallTV Pro pin-probe");
  tft.setCursor(4, 48);  tft.print("http://pinprobe.local");
  tft.setCursor(4, 66);  tft.print("IP: "); tft.print(ipStr);

  // Live touch table (compact: only channels with any activity + expected pin).
  tft.setCursor(4, 90); tft.print("touch (base->now):");
  int y = 108;
  for (int i = 0; i < NCH && y < 232; i++) {
    bool interesting = touchCount[i] > 0 || CH[i].gpio == EXPECTED_TOUCH_GPIO;
    if (!interesting) continue;
    uint16_t col = touched[i] ? TFT_GREEN
                 : (CH[i].gpio == EXPECTED_TOUCH_GPIO ? TFT_YELLOW : TFT_WHITE);
    tft.setTextColor(col, TFT_BLACK);
    tft.setCursor(4, y);
    tft.printf("%s GPIO%-2d %4d->%4d %s", CH[i].name, CH[i].gpio,
               baseline[i], lastVal[i], touched[i] ? "TOUCH" : "");
    y += 16;
  }
}

// ---------------------------------------------------------------------------
static String htmlPage() {
  int best = bestChannel();
  String h;
  h.reserve(2048);
  h += "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>";
  h += "<meta http-equiv=refresh content=1>";
  h += "<style>body{font:14px system-ui;margin:16px;background:#111;color:#eee}"
       "table{border-collapse:collapse}td,th{padding:4px 10px;border:1px solid #444;text-align:right}"
       ".t{background:#1a5;color:#fff;font-weight:bold}.exp{color:#fd0}h2{margin:4px 0}</style>";
  h += "<h2>SmallTV Pro pin-probe</h2>";
  h += "<p>Touch the top of the case. The row that turns green is your button GPIO.</p>";
  if (best >= 0 && touchCount[best] > 0)
    h += "<p><b>Best guess so far: " + String(CH[best].name) +
         " = GPIO" + String(CH[best].gpio) + "</b> (" +
         String(touchCount[best]) + " hits)</p>";
  h += "<table><tr><th>ch</th><th>GPIO</th><th>baseline</th><th>now</th><th>hits</th><th>state</th></tr>";
  for (int i = 0; i < NCH; i++) {
    bool exp = CH[i].gpio == EXPECTED_TOUCH_GPIO;
    h += touched[i] ? "<tr class=t>" : (exp ? "<tr class=exp>" : "<tr>");
    h += "<td>" + String(CH[i].name) + "</td><td>" + String(CH[i].gpio) + "</td>";
    h += "<td>" + String(baseline[i]) + "</td><td>" + String(lastVal[i]) + "</td>";
    h += "<td>" + String(touchCount[i]) + "</td>";
    h += "<td>" + String(touched[i] ? "TOUCH" : (exp ? "(expected)" : "")) + "</td></tr>";
  }
  h += "</table>";

  // GPIO / peripheral discovery table.
  h += "<h3>unmapped GPIO scan</h3>";
  h += "<p>Digital pins read HIGH idle (pull-up) and go LOW when a button to "
       "GND is pressed. ADC pins: shine a light on / cover the device and watch "
       "for a pin whose range swings widely = a light sensor. min!=max = it moved.</p>";
  h += "<table><tr><th>GPIO</th><th>type</th><th>now</th><th>min</th><th>max</th><th>moved?</th></tr>";
  for (int i = 0; i < NG; i++) {
    bool moved = GMON[i].adc ? (gMax[i] - gMin[i] > 200) : (gMin[i] != gMax[i]);
    h += moved ? "<tr class=t>" : "<tr>";
    h += "<td>" + String(GMON[i].gpio) + "</td>";
    h += "<td>" + String(GMON[i].adc ? "ADC1" : (GMON[i].inputOnly ? "din" : "din+pu")) + "</td>";
    h += "<td>" + String(gNow[i]) + "</td><td>" + String(gMin[i]) + "</td><td>" + String(gMax[i]) + "</td>";
    h += "<td>" + String(moved ? "YES" : "") + "</td></tr>";
  }
  h += "</table>";

  // Which TFT pins THIS running build was compiled with -- so we can verify
  // remotely what an OTA actually flashed instead of guessing.
  h += "<h3>TFT build config</h3><p>";
  h += "driver=ST7789 " + String(TFT_WIDTH) + "x" + String(TFT_HEIGHT);
  h += " | MOSI=" + String(TFT_MOSI) + " SCLK=" + String(TFT_SCLK);
  h += " DC=" + String(TFT_DC) + " RST=" + String(TFT_RST);
  h += " CS=" + String(TFT_CS) + " BL=" + String(TFT_BL);
  h += " | " + String(SPI_FREQUENCY / 1000000) + "MHz</p>";

  // Backlight hunt: drive a GPIO to a level and watch the panel for any glow.
  // Flash pins 6-11 are refused. This isolates BL pin + polarity without a
  // reflash, and rules backlight in/out before we suspect the SPI wiring.
  h += "<h3>backlight test</h3>";
  h += "<p>Click, then look at the physical panel for a faint glow:</p><p>";
  h += "<a href='/bl?pin=25&lvl=0'>[BL25=LOW/on]</a> ";
  h += "<a href='/bl?pin=25&lvl=1'>[BL25=HIGH]</a> ";
  h += "<a href='/bl?pin=27&lvl=0'>[27=LOW]</a> ";
  h += "<a href='/bl?pin=5&lvl=0'>[5=LOW]</a> ";
  h += "<a href='/bl?pin=32&lvl=0'>[32=LOW]</a> ";
  h += "<a href='/blsweep'>[SWEEP all pins]</a></p>";
  if (blNote.length()) h += "<p style='color:#6f6'>" + blNote + "</p>";

  // Manual ST7789 fill: try pin/CS permutations live, no reflash. If a click
  // fills the panel with the named color, those pins are the answer.
  h += "<h3>ST7789 fill test (runtime pins)</h3>";
  h += "<p>Each fills the panel a solid color if the pins are right:</p><p>";
  h += "<a href='/st?cs=-1&color=F800'>[RED cs=GND]</a> ";
  h += "<a href='/st?cs=-1&color=07E0'>[GREEN cs=GND]</a> ";
  h += "<a href='/st?cs=-1&color=001F'>[BLUE cs=GND]</a><br>";
  h += "<a href='/st?cs=3&color=F800'>[RED cs=3]</a> ";
  h += "<a href='/st?cs=5&color=F800'>[RED cs=5]</a> ";
  h += "<a href='/st?cs=15&color=F800'>[RED cs=15]</a> ";
  h += "<a href='/st?cs=22&color=F800'>[RED cs=22]</a></p>";
  h += "<p>defaults MOSI=23 SCLK=18 DC=2 RST=4; override via query, e.g. "
       "<code>/st?mosi=23&sclk=18&dc=2&rst=4&cs=-1&color=F800</code></p>";
  if (stNote.length()) h += "<p style='color:#fd0'>" + stNote + "</p>";
  h += "<p><a href='/draw'>[back to TFT_eSPI color bars]</a></p>";

  h += "<p>TFT: " + String(tftOK ? "init ran (panel should show color bars if pins right)"
                                  : "not initialized") + "</p>";
  h += "<p><a href=/update style='color:#6cf'>OTA: flash new firmware</a> "
       "(re-flash without UART)</p>";
  return h;
}

// ---- self-OTA -------------------------------------------------------------
// A minimal push-style updater built on the Update library, so you can re-flash
// the probe (or jump straight to real firmware) over WiFi. Upload the new
// firmware.bin at http://pinprobe.local/update -- no UART, no disassembly.
static void setupOTA() {
  server.on("/update", HTTP_GET, []() {
    server.send(200, "text/html",
      "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<h2>pin-probe OTA</h2>"
      "<p>Pick a firmware .bin (e.g. .pio/build/pinprobe/firmware.bin) and upload.</p>"
      "<form method=POST action=/update enctype=multipart/form-data>"
      "<input type=file name=fw accept=.bin> <input type=submit value=Flash></form>"
      "<p>Device reboots into the new image on success.</p>");
  });

  // Two callbacks: the upload sink (streams the .bin into flash) and the final
  // response (sent after the whole file arrives), which then reboots.
  server.on("/update", HTTP_POST,
    []() {
      bool ok = !Update.hasError();
      server.send(200, "text/html",
        ok ? "<h2>OK - rebooting into new firmware...</h2>"
           : "<h2>FAILED - old firmware kept. Check the .bin and retry.</h2>");
      delay(1200);
      if (ok) ESP.restart();
    },
    []() {
      HTTPUpload& up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[ota] start: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN))
          Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize)
          Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true))
          Serial.printf("[ota] done: %u bytes\n", up.totalSize);
        else
          Update.printError(Serial);
      }
    });
}

// ---- manual ST7789 bring-up (runtime-chosen pins) -------------------------
// TFT_eSPI's pins are fixed at compile time; when the panel stays black we want
// to try CS/pin permutations WITHOUT a reflash. This drives a minimal ST7789
// init + solid-color fill over hardware SPI (VSPI, mode 3) with pins picked at
// runtime. Once used it takes over the panel (drawTFT is suspended).
static void st7789Test(int mosi, int sclk, int dc, int rst, int cs, uint16_t color) {
  if (isFlashPin(mosi) || isFlashPin(sclk) || isFlashPin(dc) ||
      isFlashPin(rst) || (cs >= 0 && isFlashPin(cs))) {
    stNote = "refused: a pin hits the SPI flash (GPIO6-11)";
    return;
  }
  stActive = true;                            // stop TFT_eSPI from redrawing
  pinMode(dc, OUTPUT);
  pinMode(rst, OUTPUT);
  if (cs >= 0) { pinMode(cs, OUTPUT); digitalWrite(cs, LOW); }  // assert for whole test

  digitalWrite(rst, HIGH); delay(20);         // hardware reset pulse
  digitalWrite(rst, LOW);  delay(30);
  digitalWrite(rst, HIGH); delay(150);

  static SPIClass spi(VSPI);                  // same bus TFT_eSPI used; re-mux pins
  spi.begin(sclk, -1, mosi, -1);
  spi.beginTransaction(SPISettings(20000000, MSBFIRST, SPI_MODE3));
  auto cmd = [&](uint8_t c) { digitalWrite(dc, LOW);  spi.transfer(c); };
  auto dat = [&](uint8_t d) { digitalWrite(dc, HIGH); spi.transfer(d); };

  cmd(0x01); delay(150);                      // SWRESET
  cmd(0x11); delay(120);                      // SLPOUT
  cmd(0x3A); dat(0x55);                        // COLMOD = 16bit/pixel
  cmd(0x36); dat(0x00);                        // MADCTL
  cmd(0x21);                                   // INVON (ST7789 240x240 needs it)
  cmd(0x2A); dat(0); dat(0); dat(0); dat(239); // CASET 0..239
  cmd(0x2B); dat(0); dat(0); dat(0); dat(239); // RASET 0..239
  cmd(0x29); delay(20);                        // DISPON
  cmd(0x2C);                                   // RAMWR
  digitalWrite(dc, HIGH);
  uint8_t hi = color >> 8, lo = color & 0xFF;
  uint8_t line[240 * 2];
  for (int i = 0; i < 240; i++) { line[i * 2] = hi; line[i * 2 + 1] = lo; }
  for (int y = 0; y < 240; y++) spi.writeBytes(line, sizeof(line));

  spi.endTransaction();
  if (cs >= 0) digitalWrite(cs, HIGH);
  stNote = "ST7789 fill sent -> MOSI=" + String(mosi) + " SCLK=" + String(sclk) +
           " DC=" + String(dc) + " RST=" + String(rst) + " CS=" + String(cs) +
           String(" color=0x") + String(color, HEX) +
           ". If the panel shows this color, these pins are correct.";
  Serial.println("[st] " + stNote);
}

// ---- interactive display bring-up controls --------------------------------
static void setupControls() {
  // Drive an arbitrary (safe) GPIO to a level, to hunt the backlight pin.
  server.on("/bl", []() {
    int pin = server.arg("pin").toInt();
    int lvl = server.arg("lvl").toInt();
    if (isFlashPin(pin)) {
      blNote = "refused GPIO" + String(pin) + " (SPI flash pin)";
    } else {
      sweepOn = false;                       // manual action cancels a sweep
      pinMode(pin, OUTPUT);
      digitalWrite(pin, lvl ? HIGH : LOW);
      blNote = "driving GPIO" + String(pin) + " = " + (lvl ? "HIGH" : "LOW");
    }
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // Sweep every candidate BL pin LOW, ~2s each; the page shows the active pin.
  server.on("/blsweep", []() {
    sweepOn = true; sweepIdx = 0; sweepAt = 0;
    blNote = "sweep started -- watch the panel, note which pin makes it glow";
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // Force a fresh redraw of the color bars (after backlight comes on).
  server.on("/draw", []() {
    stActive = false;                         // hand the panel back to TFT_eSPI
    tft.init(); tft.setRotation(0); tft.invertDisplay(true);
    drawTFT();
    blNote = "redrew color bars (TFT_eSPI)";
    server.sendHeader("Location", "/");
    server.send(303);
  });

  // Manual ST7789 fill with runtime pins. Defaults = the candidate pinout.
  // e.g. /st?mosi=23&sclk=18&dc=2&rst=4&cs=-1&color=F800
  server.on("/st", []() {
    int mosi  = server.hasArg("mosi")  ? server.arg("mosi").toInt()  : 23;
    int sclk  = server.hasArg("sclk")  ? server.arg("sclk").toInt()  : 18;
    int dc    = server.hasArg("dc")    ? server.arg("dc").toInt()    : 2;
    int rst   = server.hasArg("rst")   ? server.arg("rst").toInt()   : 4;
    int cs    = server.hasArg("cs")    ? server.arg("cs").toInt()    : -1;
    uint16_t color = server.hasArg("color")
      ? (uint16_t) strtol(server.arg("color").c_str(), nullptr, 16) : 0xF800;
    st7789Test(mosi, sclk, dc, rst, cs, color);
    server.sendHeader("Location", "/");
    server.send(303);
  });
}

// Advance the backlight sweep from loop(); non-blocking.
static void serviceSweep(uint32_t now) {
  if (!sweepOn) return;
  if (now - sweepAt < 2000) return;
  sweepAt = now;
  // turn the previous candidate back off (HIGH = off for active-low panels)
  if (sweepIdx > 0) {
    int prev = BL_CANDS[(sweepIdx - 1) % NBL];
    if (!isFlashPin(prev)) digitalWrite(prev, HIGH);
  }
  if (sweepIdx >= NBL) {
    sweepOn = false;
    blNote = "sweep done -- no pin caught? backlight may be hardwired (then it's"
             " an SPI-pin problem), or this is a different board revision";
    return;
  }
  int pin = BL_CANDS[sweepIdx];
  if (!isFlashPin(pin)) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  blNote = "sweep: now driving GPIO" + String(pin) + " LOW (" +
           String(sweepIdx + 1) + "/" + String(NBL) + ")";
  Serial.println("[sweep] " + blNote);
  sweepIdx++;
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[pin-probe] GeekMagic SmallTV Pro hardware diagnostic");

  // Best-effort display init with the candidate pins from platformio.ini.
  // If the pins are wrong the panel simply stays dark -- no error, no hang.
  tft.init();
  tft.setRotation(0);
  tft.invertDisplay(true);            // Pro ST7789 needs inverted colors
  tftOK = true;
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(2);
  tft.setCursor(4, 4);
  tft.print("pin-probe booting...");
  tft.setCursor(4, 24);
  tft.print("do NOT touch the case");

  Serial.println("[pin-probe] calibrating touch (keep hands off the case)...");
  calibrate();
  gpioInit();
  Serial.print("[pin-probe] baselines:");
  for (int i = 0; i < NCH; i++) Serial.printf(" %s=%d", CH[i].name, baseline[i]);
  Serial.println();

  // WiFi is the ONLY recovery path once the stock updater is gone: the OTA at
  // /update lives behind it. So we must never end up permanently unreachable.
  // If the screen pins are also wrong (black screen), WiFi is your only way
  // back in -- so on failure we reboot and retry rather than sitting dead.
  // On first boot, join "PinProbe-Setup" and pick your 2.4GHz network; it then
  // auto-connects on every later boot.
  tft.setCursor(4, 44); tft.print("WiFi: join PinProbe-Setup");
  WiFiManager wm;
  wm.setConfigPortalTimeout(600);            // 10 min portal window per attempt
  wifiOK = wm.autoConnect("PinProbe-Setup");
  if (!wifiOK) {
    Serial.println("[pin-probe] WiFi not configured; rebooting to retry "
                   "(keeps /update reachable). Join PinProbe-Setup to proceed.");
    delay(1000);
    ESP.restart();                           // never strand the device offline
  }

  // Past here WiFi is up (otherwise we rebooted above).
  ipStr = WiFi.localIP().toString();
  Serial.println("[pin-probe] WiFi connected: " + ipStr);
  if (MDNS.begin("pinprobe")) {
    Serial.println("[pin-probe] http://pinprobe.local/  (OTA at /update)");
  }
  server.on("/", []() { server.send(200, "text/html", htmlPage()); });
  setupOTA();
  setupControls();
  server.begin();
  drawTFT();
}

void loop() {
  static uint32_t lastDraw = 0;
  if (wifiOK) server.handleClient();

  sampleTouch();
  if (!stActive) gpioSample();          // ST7789 test may reuse some pins

  uint32_t now = millis();
  serviceSweep(now);
  if (now - lastDraw >= 250) {            // refresh screen + serial 4x/sec
    lastDraw = now;
    if (!stActive) drawTFT();             // manual ST7789 test owns the panel
    Serial.print("[touch]");
    for (int i = 0; i < NCH; i++)
      Serial.printf(" %s:%d%s", CH[i].name, lastVal[i], touched[i] ? "*" : "");
    Serial.println();
  }
  delay(30);
}
