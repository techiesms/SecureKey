/*
 * SecureKey — Hardware Password Manager  (monochrome edition)
 * ─────────────────────────────────────────────────────────────────────
 *  Board   : MaTouch ESP32-S3 AMOLED 1.8"  (368×448, SH8601 QSPI)
 *  Touch   : FT3168 capacitive
 *  Store   : FFat /db.bin   (256 B/record × MAX_PASSWORDS = 30000)
 *  Setings : NVS  "skset" namespace
 *
 *  Screens:
 *     LOCK  →  PIN  →  HOME (2x2 icon grid, drag-to-reorder)
 *                          ├─ PASSWORDS  → list (inline search) → DETAIL
 *                          ├─ ADD NEW    → form
 *                          ├─ FAVORITES  → list (hearts only)
 *                          └─ SETTINGS   → Bluetooth / USB / Brightness / …
 *
 *  HID typing goes out over BLE (paired phone/PC) AND/OR USB-C — both
 *  transports can be live at once.
 *  Capacity: up to 30,000 passwords (MAX_PASSWORDS in theme.h).
 * ─────────────────────────────────────────────────────────────────────
 */
// ── HID configuration ───────────────────────────────────────────────
//
//   USB and BLE HID live in separate .cpp files (hid_usb.cpp,
//   hid_ble.cpp) because the libraries define the same KEY_xxx
//   macros and can't be in the same translation unit.  This file
//   only sees extern "C" wrappers — no library headers leak in.
//
//   To disable a transport at compile time, set HID_USB_ENABLE 0 in
//   hid_usb.cpp  or  HID_BLE_ENABLE 0 in hid_ble.cpp.
//
//   At runtime, Settings → Bluetooth and Settings → USB HID toggle
//
extern "C" {
  void hidUsbBegin();
  void hidUsbPrint(const char *s);
  void hidUsbQuickFill(const char *user, const char *pass);
  int  hidUsbCompiled();
  int  hidUsbMounted();    // 1 once a USB host has enumerated the keyboard

  void hidBleBegin();
  void hidBleEnd();
  int  hidBleConnected();
  void hidBlePrint(const char *s);
  void hidBleQuickFill(const char *user, const char *pass);
  int  hidBleCompiled();
  int  hidBleStarted();
  void hidBleSetAndroidFix(int on);   // UK/Android @<->" keycode swap
  void hidBlePeerAddr(char *out, int n);   // connecting peer's BT address
  void hidBleSettleReset();           // re-arm typing settle on disconnect
  void hidBleTune();                  // request tight conn interval at connect
  void hidBleForget();                // clear all BLE bonds (re-pair fresh)
}

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <FFat.h>
#include <Preferences.h>
#include "pin_config.h"
#include "theme.h"

// HID transports are isolated to hid_usb.cpp / hid_ble.cpp — see above

// ── Hardware objects ─────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX    *out = new Arduino_SH8601(
    bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, out);
Adafruit_NeoPixel led(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences      prefs;

inline void flushScreen() { gfx->flush(); }

// ── Navigation stack ─────────────────────────────────────────────────
Screen   navStack[10] = { SCR_LOCK };
int8_t   navTop       = 0;
Screen   current      = SCR_LOCK;

// ── Global state ─────────────────────────────────────────────────────
// BLE connection state (polled in loop, drawn by the status bar)
bool     btConnected = false;

// BLE pairing gate: a phone can connect, but the device types NOTHING until
// the user taps "Accept" on the on-device request prompt (which only appears
// after the PIN has been entered / device unlocked). "Block 5 min" puts BLE
// dark for 5 minutes so a pushy host stops re-requesting.
bool     bleAuthorized = false;   // true once the user accepts this connection
uint32_t bleBlockUntil = 0;       // millis() until which BLE stays off (block)
uint32_t bleGateSnooze = 0;       // after REJECT: don't re-prompt before this

// PIN
char     pinEntry[5] = {0};
uint8_t  pinLen      = 0;
uint32_t shakeUntil  = 0;
uint32_t unlockUntil = 0;

// PIN brute-force lockout (escalating wait after repeated wrong PINs)
uint8_t  pinFails     = 0;       // cumulative wrong attempts (persisted)
uint32_t pinLockUntil = 0;       // millis() until which the keypad is frozen

// Touch state (used by touch_input.ino)
bool     touching    = false;
uint16_t tStartX, tStartY, tCurX, tCurY;
uint32_t tStartTime  = 0;
bool     isDrag      = false;
const int16_t DRAG_THRESHOLD = 14;

// Password DB
ListItem  *passwordIndex = nullptr;
uint16_t   passwordCount = 0;

// List screen state
int32_t   listScrollY  = 0;
float     listVelocity = 0.0f;

// Selected/Editing
uint16_t  detailId  = 0;     // currently shown
uint16_t  editingId = 0;     // 0 = adding new

// User settings (loaded from NVS) — USB HID defaults ON so typing works
// out-of-the-box when plugged into a PC.
UserSettings settings = { 140, false, true, true, "1234", 30, false };

// Idle tracking for auto-lock (refreshed on every touch in pollTouch)
uint32_t   lastActivityMs = 0;

// Double-tap sleep state
bool       screenOff   = false;
uint32_t   lastTapTime = 0;

// LED helper
uint32_t  ledClearAt = 0;
void ledSet(uint32_t c, uint32_t ms = 300) {
  led.setPixelColor(0, c); led.show(); ledClearAt = millis() + ms;
}

// ── Forward declarations ──────────────────────────────────────────────
void drawLock();    void onTapLock(int16_t,int16_t);
void drawPin();     void onTapPin(int16_t,int16_t);    void pinSlideIn();
void drawHome();    void onTapHome(int16_t,int16_t);
void homeLoadOrder();
bool homeInReorder();   bool homeIsDragging();
void homeLongPress(int16_t,int16_t);
void homeDrag(int16_t,int16_t);
void homeRelease(int16_t,int16_t);
void homeExitReorder();
void drawList();    void onTapList(int16_t,int16_t);
void drawDetail();  void onTapDetail(int16_t,int16_t);
void drawAdd();     void onTapAdd(int16_t,int16_t);
void addInit();
void addEditInit(const PassRecord &rec);
void detailInit();
void drawSettings();void onTapSettings(int16_t,int16_t);
void drawChgPin();    void onTapChgPin(int16_t,int16_t);    void chgPinInit();
void drawFlash();     void onTapFlash(int16_t,int16_t);
void drawAll();
void drawStatusBar();
void pollTouch();
void popToLock();

// gfx_lib helpers used here
void textCenter(int16_t y, const char *s, uint8_t sz,
                uint16_t col, int16_t cx = LCD_WIDTH/2);
void textAt(int16_t x, int16_t y, const char *s, uint8_t sz, uint16_t col);
void listScroll(int16_t dy);
void buildList();
void typeViaHID(const char *s);
void saveSettings();
void loadSettings();
extern bool listSearchMode;
extern char listQuery[];
extern bool listFavOnly;                       // list shows only favorited entries
extern bool ftReadTouch(uint16_t &x, uint16_t &y);  // raw touch (touch_input.ino)
void bleConnectGate();                         // on-device Accept/Reject/Block prompt

// Storage forward decls — Arduino's auto-prototype can miss these
// when alphabetic build order means callers (screen_add.ino) come
// before the definitions (storage.ino).
void dbSeed();
void dbLoadIndex();
bool dbAppend(const PassRecord &rec);
bool dbLoadRecord(uint16_t id, PassRecord &out);

// PIN lockout helpers (defined later in this file)
void pinRegisterFail();
void pinRegisterSuccess();
uint32_t pinLockRemaining();    // seconds left, 0 if unlocked

// ── Screen dispatch ──────────────────────────────────────────────────
void drawAll() {
  switch (current) {
    case SCR_LOCK:        drawLock();      break;
    case SCR_PIN:         drawPin();       break;
    case SCR_HOME:        drawHome();      break;
    case SCR_LIST:        drawList();      break;
    case SCR_DETAIL:      drawDetail();    break;
    case SCR_ADD:         drawAdd();       break;
    case SCR_SETTINGS:    drawSettings();  break;
    case SCR_CHGPIN:      drawChgPin();    break;
    case SCR_FLASH:       drawFlash();     break;
    case SCR_WIFI:        drawWifi();      break;
    default:                               break;
  }
}

void dispatchTap(int16_t tx, int16_t ty) {
  static int16_t lastTapX = -999, lastTapY = -999;
  uint32_t now = millis();
  if (screenOff) {
    // Any tap wakes (does NOT also activate whatever was under the finger)
    out->Display_Brightness(settings.brightness);
    screenOff = false;
    lastTapTime = now; lastTapX = tx; lastTapY = ty;
    return;
  }
  // Double-tap sleep: only on passive list-style screens, AND only when both
  // taps land in nearly the same spot — so tapping two different items (or a
  // quick double-tap to open something) never accidentally sleeps the device.
  bool sleepAllowed = (current == SCR_HOME || current == SCR_LIST ||
                       current == SCR_SETTINGS);
  bool sameSpot = (abs(tx - lastTapX) < 36 && abs(ty - lastTapY) < 36);
  if (settings.doubleTapSleep && sleepAllowed && sameSpot &&
      (now - lastTapTime) < 280) {
    out->Display_Brightness(0);          // genuinely dark (fade is now a no-op)
    screenOff = true;
    popToLock();
    lastTapTime = 0; lastTapX = -999; lastTapY = -999;
    return;
  }
  lastTapTime = now; lastTapX = tx; lastTapY = ty;

  switch (current) {
    case SCR_LOCK:        onTapLock(tx, ty);       break;
    case SCR_PIN:         onTapPin(tx, ty);        break;
    case SCR_HOME:        onTapHome(tx, ty);       break;
    case SCR_LIST:        onTapList(tx, ty);       break;
    case SCR_DETAIL:      onTapDetail(tx, ty);     break;
    case SCR_ADD:         onTapAdd(tx, ty);        break;
    case SCR_SETTINGS:    onTapSettings(tx, ty);   break;
    case SCR_CHGPIN:      onTapChgPin(tx, ty);     break;
    case SCR_FLASH:       onTapFlash(tx, ty);      break;
    case SCR_WIFI:        onTapWifi(tx, ty);       break;
    default:                                       break;
  }
}

// Transitions are now INSTANT. The old brightness fade (dim to black and
// back on every screen change) read as an annoying flicker and added lag.
// Instant draws on the double-buffered canvas are flicker-free and snappy.
// Kept as no-ops so existing call sites compile unchanged; the PIN screen
// has its own slide-up entrance (pinSlideIn).
void fadeOut() {}
void fadeIn()  {}

void pushNav(Screen s) {
  if (navTop < 9) navStack[++navTop] = s;
  current = s;
  if (s == SCR_LIST)     { listScrollY = 0; listVelocity = 0;
                           listSearchMode = false; listQuery[0] = 0; buildList(); }
  if (s == SCR_DETAIL)   { detailInit(); }
  if (s == SCR_SETTINGS) { extern int32_t settingsScrollY; settingsScrollY = 0; }
  if (s == SCR_PIN)      { pinSlideIn(); return; }   // animated slide-up
  drawAll();
}

void popNav() {
  if (navTop > 0) {
    navTop--;
    current = navStack[navTop];
    // Returning to the list (e.g. from a detail view) should NOT leave the
    // search keyboard up — hide it so the user doesn't have to tap OK.
    if (current == SCR_LIST) listSearchMode = false;
    drawAll();
  }
}

void popToLock() {
  homeExitReorder();         // never leave the home in arrange mode
  navTop = 0;
  navStack[0] = SCR_LOCK;
  current = SCR_LOCK;
  pinLen = 0;
  drawAll();
}

// ── Touch callbacks ──────────────────────────────────────────────────
extern void setScroll(int16_t dy);

void onDrag(uint16_t cx, uint16_t cy, int16_t dx, int16_t dy) {
  // Don't scroll the list when search keyboard is up — let the
  // user drag-select text instead (and avoid accidental scroll).
  if (current == SCR_LIST     && !listSearchMode) listScroll(-dy);
  if (current == SCR_SETTINGS)                    setScroll(-dy);
}

void onSwipeEnd(int16_t totalDx, int16_t totalDy) {
  // Swipe UP on lock → PIN
  if (current == SCR_LOCK && totalDy < -50) { pushNav(SCR_PIN); return; }
  // Swipe DOWN on PIN → lock
  if (current == SCR_PIN  && totalDy >  60) { popNav(); return; }
  // Swipe RIGHT (back) — require a deliberate, clearly-horizontal gesture so
  // it doesn't fire on small drifts or during vertical scrolling.
  if (current != SCR_LOCK && current != SCR_PIN &&
      totalDx > 110 && totalDx > abs(totalDy) * 2) {
    popNav();
    return;
  }
  // Inertia on lists
  if (current == SCR_LIST || current == SCR_SEARCH_RES) {
    listVelocity = -(float)totalDy * 0.06f;
  }
}

// ── One-tap login fill (username + Tab + password + Enter) ────────────
void quickFillViaHID(const char *user, const char *pass) {
  // BLE first (wireless), then USB. BLE requires the on-device Accept gate
  // (bleAuthorized) — same rule as typeViaHID — so a connected-but-unaccepted
  // host can never be filled. USB requires the host to have enumerated us.
  if (settings.bleEnabled && hidBleCompiled() && hidBleConnected() && bleAuthorized) {
    Serial.printf("[HID] quickFill→BLE user='%s'\n", user);
    hidBleQuickFill(user, pass);
    ledSet(0x0000FF, 250);
    return;
  }
  if (settings.usbHidEnabled && hidUsbCompiled() && hidUsbMounted()) {
    Serial.printf("[HID] quickFill→USB user='%s'\n", user);
    hidUsbQuickFill(user, pass);
    ledSet(0x00FF00, 250);
    return;
  }
  typeViaHID("");   // no transport → reuse the "HID not ready" help modal
}

// ── HID type dispatcher ───────────────────────────────────────────────
void typeViaHID(const char *s) {
  Serial.printf("[HID] typeViaHID('%s')  bleEn=%d bleComp=%d bleConn=%d  usbEn=%d usbComp=%d\n",
                s, settings.bleEnabled, hidBleCompiled(), hidBleConnected(),
                settings.usbHidEnabled, hidUsbCompiled());

  // ONE transport at a time. Both can be ENABLED and connected at once (the
  // S3 is dual-core, NimBLE + native USB coexist), but typing to BOTH into the
  // same computer doubles/garbles the text — so we pick one. BLE wins when the
  // user has ACCEPTED it on-device (a deliberate "type wirelessly" choice);
  // otherwise USB. (Want USB instead while BLE is paired? Reject/Block the BLE
  // request, or turn Bluetooth off.)
  if (settings.bleEnabled && hidBleCompiled() && hidBleConnected() && bleAuthorized) {
    Serial.println("[HID] routing → BLE");
    hidBlePrint(s);
    ledSet(0x0000FF, 200);
    return;
  }
  if (settings.usbHidEnabled && hidUsbCompiled() && hidUsbMounted()) {
    Serial.println("[HID] routing → USB");
    hidUsbPrint(s);
    ledSet(0x00FF00, 200);
    return;
  }
  Serial.println("[HID] no transport ready");

  // Full-screen modal — clearly explains what to do
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  textCenter(STATUS_H + 26, "HID NOT READY", 3, C_WHITE);

  const char *lines[6];
  int n = 0;
  if (settings.bleEnabled && hidBleConnected() && !bleAuthorized) {
    lines[n++] = "A Bluetooth device is";
    lines[n++] = "connected but not accepted.";
    lines[n++] = "Wait for the request and";
    lines[n++] = "tap ACCEPT, then retry.";
  } else if (settings.bleEnabled && !hidBleConnected()) {
    lines[n++] = "Plug USB-C into a PC, or";
    lines[n++] = "pair \"SecureKey\" from your";
    lines[n++] = "phone's Bluetooth settings,";
    lines[n++] = "then tap the field again.";
  } else {
    lines[n++] = "Plug the USB-C cable into";
    lines[n++] = "a PC (data port, not a";
    lines[n++] = "charger), or turn on";
    lines[n++] = "Bluetooth in Settings,";
    lines[n++] = "then tap the field again.";
  }

  int16_t y = STATUS_H + 80;
  for (int i = 0; i < n; i++) {
    textCenter(y, lines[i], 1, C_GRAY_5);
    y += 18;
  }
  textCenter(LCD_HEIGHT - 40, "tap anywhere to dismiss", 1, C_GRAY_3);
  flushScreen();

  // Wait for any tap to dismiss
  uint32_t waitStart = millis();
  while (millis() - waitStart < 4500) {
    delay(30);
    pollTouch();
    if (!touching && millis() - waitStart > 200) break;
  }
  drawAll();
}

// ── BLE connection request gate (Accept / Reject / Block 5 min) ───────
// A phone connected over BLE but hasn't been authorised yet. Show an
// on-device prompt; nothing is typed until the user taps ACCEPT. This is
// only ever called once the device is unlocked (see loop()), so the request
// genuinely "comes after entering the PIN". Reads touch directly so a
// finger-release doesn't leak into the underlying screen.
void bleConnectGate() {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  textCenter(STATUS_H + 24, "BLUETOOTH", 3, C_WHITE);
  textCenter(STATUS_H + 58, "REQUEST",   3, C_WHITE);
  gfx->fillRoundRect(LCD_WIDTH/2 - 50, STATUS_H + 88, 100, 3, 1, C_BLUE);
  textCenter(STATUS_H + 102, "A device wants to connect", 1, C_GRAY_5);
  textCenter(STATUS_H + 120, "and type passwords into it", 1, C_GRAY_5);

  // Show the connecting peer's Bluetooth address (a central does not send a
  // friendly name, so the MAC is the most we can show).
  char paddr[24]; hidBlePeerAddr(paddr, sizeof(paddr));
  char pline[40]; snprintf(pline, sizeof(pline), "Device: %s", paddr);
  textCenter(STATUS_H + 136, pline, 1, C_BLUE);

  const int16_t bx = SAFE_PAD, bw = LCD_WIDTH - 2*SAFE_PAD, bh = 54;
  const int16_t y1 = STATUS_H + 150;        // Accept
  const int16_t y2 = y1 + bh + 12;          // Reject
  const int16_t y3 = y2 + bh + 12;          // Block 5 min

  gfx->fillRoundRect(bx, y1, bw, bh, 12, C_WHITE);
  textCenter(y1 + bh/2 - 8, "ACCEPT", 2, C_BLACK);
  gfx->fillRoundRect(bx, y2, bw, bh, 12, C_GRAY_1);
  gfx->drawRoundRect(bx, y2, bw, bh, 12, C_GRAY_3);
  textCenter(y2 + bh/2 - 8, "REJECT", 2, C_WHITE);
  gfx->fillRoundRect(bx, y3, bw, bh, 12, C_GRAY_1);
  gfx->drawRoundRect(bx, y3, bw, bh, 12, C_RED);
  textCenter(y3 + bh/2 - 8, "BLOCK 5 MIN", 2, C_RED);
  flushScreen();

  uint16_t x, y;
  while (ftReadTouch(x, y)) delay(8);        // let any prior press lift
  uint32_t t0 = millis();
  int  choice = 1;                           // default = reject (timeout/drop)
  bool tapped = false;                       // did the user actually choose?
  while (millis() - t0 < 20000) {
    if (!hidBleConnected()) {
      // Tolerate a brief flap during the pairing/bonding handshake — only
      // give up if the peer stays gone, so the prompt doesn't flicker.
      uint32_t d0 = millis();
      while (!hidBleConnected() && millis() - d0 < 1800) delay(60);
      if (!hidBleConnected()) { choice = 1; break; }
    }
    if (ftReadTouch(x, y)) {
      uint16_t py = y;
      while (ftReadTouch(x, y)) delay(8);    // wait for release
      if      (py >= y1 && py < y1 + bh) { choice = 0; tapped = true; break; }
      else if (py >= y2 && py < y2 + bh) { choice = 1; tapped = true; break; }
      else if (py >= y3 && py < y3 + bh) { choice = 2; tapped = true; break; }
    }
    delay(10);
  }

  // The gate blocks the loop and reads touch directly, so pollTouch never ran
  // — without this, the idle auto-lock could fire the instant you accept.
  // A genuine button tap counts as activity; a walk-away timeout does not.
  if (tapped) lastActivityMs = millis();

  if (choice == 0) {                         // ACCEPT
    bleAuthorized = true;
    ledSet(0x00FF00, 250);
    Serial.println("[BLE] connection ACCEPTED by user");
  } else if (choice == 2) {                  // BLOCK 5 min
    bleAuthorized = false;
    bleBlockUntil = millis() + 300000UL;     // BLE stays dark 5 minutes
    if (hidBleCompiled()) hidBleEnd();
    ledSet(0xFF0000, 250);
    Serial.println("[BLE] connection BLOCKED for 5 min");
  } else {                                   // REJECT
    // Don't tear the stack down — a paired phone instantly auto-reconnects,
    // which would loop the prompt forever (and NimBLE end/begin cycling is
    // flaky). Typing stays blocked (bleAuthorized=false); just snooze the
    // prompt so it doesn't nag.
    bleAuthorized = false;
    bleGateSnooze = millis() + 20000UL;      // 20 s of quiet
    ledSet(0xFF0000, 200);
    Serial.println("[BLE] connection REJECTED (snoozed 20s)");
  }
  btConnected = settings.bleEnabled && hidBleCompiled() && hidBleConnected();
  drawAll();
}

// ── PIN brute-force lockout ───────────────────────────────────────────
// After repeated wrong PINs the keypad freezes for an escalating wait:
//   3rd fail → 30 s, 4th → 60 s, 5th → 2 min, 6th+ → 5 min.
// The cumulative fail count is persisted in NVS so power-cycling the
// device does not reset the escalation. (millis() resets on reboot, so a
// power-cycle still clears the *current* countdown — persisting the count
// keeps the next wrong attempt jumping straight back to a long wait.)

static uint32_t pinLockDelayMs(uint8_t fails) {
  if (fails < 3) return 0;
  switch (fails) {
    case 3:  return 30000UL;    // 30 s
    case 4:  return 60000UL;    // 60 s
    case 5:  return 120000UL;   // 2 min
    default: return 300000UL;   // 5 min
  }
}

void pinRegisterFail() {
  if (pinFails < 200) pinFails++;
  prefs.begin("skset", false);
  prefs.putUChar("pinf", pinFails);
  prefs.end();
  uint32_t d = pinLockDelayMs(pinFails);
  pinLockUntil = d ? millis() + d : 0;
  Serial.printf("[PIN] fail #%u  lock=%lu ms\n", pinFails, (unsigned long)d);
}

void pinRegisterSuccess() {
  pinFails = 0;
  pinLockUntil = 0;
  prefs.begin("skset", false);
  prefs.putUChar("pinf", 0);
  prefs.end();
}

// Seconds remaining on the current lockout (0 if the keypad is usable).
uint32_t pinLockRemaining() {
  if (pinLockUntil == 0) return 0;
  uint32_t now = millis();
  if (now >= pinLockUntil) return 0;
  return (pinLockUntil - now + 999) / 1000;
}

// ── Settings persistence ──────────────────────────────────────────────
void loadSettings() {
  prefs.begin("skset", true);
  settings.brightness     = prefs.getUChar("br",   140);
  settings.bleEnabled     = prefs.getBool ("ble",  false);
  settings.usbHidEnabled  = prefs.getBool ("usb",  true);
  settings.doubleTapSleep = prefs.getBool ("dtap", true);
  settings.autoLockSec    = prefs.getUChar("alock", 30);
  settings.androidFix     = prefs.getBool ("afix", false);
  pinFails                = prefs.getUChar("pinf",  0);
  String p = prefs.getString("pin", "1234");
  strncpy(settings.pin, p.c_str(), 4); settings.pin[4] = 0;
  prefs.end();
  hidBleSetAndroidFix(settings.androidFix ? 1 : 0);
}
void saveSettings() {
  prefs.begin("skset", false);
  prefs.putUChar ("br",   settings.brightness);
  prefs.putBool  ("ble",  settings.bleEnabled);
  prefs.putBool  ("usb",  settings.usbHidEnabled);
  prefs.putBool  ("dtap", settings.doubleTapSleep);
  prefs.putUChar ("alock", settings.autoLockSec);
  prefs.putBool  ("afix", settings.androidFix);
  prefs.putString("pin",  settings.pin);
  prefs.end();
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[SecureKey] Password Manager (mono) starting...");

  led.begin();  led.setBrightness(40);  led.clear(); led.show();

  pinMode(LCD_EN, OUTPUT);  digitalWrite(LCD_EN, HIGH);  delay(50);
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW); delay(10);
  digitalWrite(TP_RST, HIGH); delay(100);

  if (!gfx->begin()) {
    Serial.println("ERROR: canvas init failed — check OPI PSRAM.");
    while (1) delay(1000);
  }

  // Register USB HID class FIRST (before the BLE stack), so TinyUSB can
  // claim the USB controller cleanly.  The BLE keyboard uses the NimBLE
  // backend, which coexists with native USB HID on the ESP32-S3, but the
  // init *order* still matters — USB first, BLE last.  Always-on USB HID
  // costs ~5 mA which is acceptable for a wired/charging device.
  if (hidUsbCompiled()) hidUsbBegin();

  loadSettings();
  homeLoadOrder();                 // restore the user's home tile arrangement

  // Re-arm the PIN lockout across power cycles. millis() resets on reboot, so
  // without this a power-cycle would bypass the "wait 30s" penalty. The fail
  // count is persisted (NVS), so if we were still locked out when powered off,
  // restart the countdown — pulling the plug no longer skips the wait.
  if (pinFails >= 3) pinLockUntil = millis() + pinLockDelayMs(pinFails);

  out->Display_Brightness(settings.brightness);

  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  pinMode(USER_BTN_PIN, INPUT_PULLUP);

  Serial.printf("[MEM] PSRAM free: %u\n", ESP.getFreePsram());
  passwordIndex = (ListItem *)ps_malloc(MAX_PASSWORDS * sizeof(ListItem));
  if (!passwordIndex) {
    Serial.println("ERROR: ps_malloc failed for index");
    while (1) delay(1000);
  }

  if (!FFat.begin(true)) {
    Serial.println("ERROR: FFat mount failed");
  } else {
    dbSeed();
    dbLoadIndex();
    Serial.printf("[DB] %u passwords loaded\n", passwordCount);
  }

  // Draw the UI immediately so the screen is always visible, even if BLE
  // has trouble coming up below.
  drawAll();

  // BLE LAST — after the screen is up, so a BLE init hiccup/brownout can't
  // leave you staring at a black screen.  RECOVERY: hold the side button
  // (SW2) while powering on to skip + disable BLE for this boot.
  if (digitalRead(USER_BTN_PIN) == LOW) {
    settings.bleEnabled = false;
    saveSettings();
    Serial.println("[BLE] skipped & disabled (SW2 held at boot)");
  } else if (settings.bleEnabled && hidBleCompiled()) {
    Serial.println("[BLE] boot init start");
    hidBleBegin();
    Serial.println("[BLE] boot init done");
  }
}

// ── Loop ─────────────────────────────────────────────────────────────
void loop() {
  // WiFi captive-portal import: service the AP + web server when active.
  // Safety: if we've navigated away from the WiFi screen by any means
  // (SW2 → lock, auto-nothing), shut the portal down.
  if (wifiPortalActive() && current != SCR_WIFI) wifiPortalStop();
  wifiPortalLoop();

  // Lock screen: ~2 Hz repaint for the "tap to unlock" pulse
  static uint32_t lastLockMs = 0;
  if (current == SCR_LOCK && millis() - lastLockMs > 500) {
    lastLockMs = millis();
    drawLock();
  }

  // PIN shake animation (~20 Hz)
  static uint32_t lastShakeMs = 0;
  if (current == SCR_PIN && millis() < shakeUntil
      && millis() - lastShakeMs > 50) {
    lastShakeMs = millis();
    drawPin();
  }

  // PIN lockout countdown (~2 Hz). Keeps the "Wait Ns" ticking and repaints
  // the keypad once the wait expires.
  static uint32_t lastPinLockMs = 0;
  static bool     pinWasLocked  = false;
  if (current == SCR_PIN && millis() - lastPinLockMs > 500) {
    bool locked = pinLockRemaining() > 0;
    if (locked || pinWasLocked) {
      lastPinLockMs = millis();
      pinWasLocked  = locked;
      drawPin();
    }
  }

  // (UNLOCKED splash removed — PIN screen jumps straight to HOME)

  // List inertia scroll (only when not in search mode)
  if (current == SCR_LIST && !listSearchMode &&
      fabsf(listVelocity) > 0.5f) {
    static uint32_t lastInertia = 0;
    if (millis() - lastInertia > 16) {
      lastInertia = millis();
      listScroll((int16_t)listVelocity);
      listVelocity *= 0.86f;
    }
  }

  // SW2 physical → lock
  static bool btnLast = HIGH;
  bool btnNow = digitalRead(USER_BTN_PIN);
  if (btnLast == HIGH && btnNow == LOW) {
    if (current != SCR_LOCK) {
      popToLock();
      ledSet(0xFF0000, 200);
    }
  }
  btnLast = btnNow;

  // LED auto-clear
  if (ledClearAt && millis() > ledClearAt) {
    led.clear(); led.show(); ledClearAt = 0;
  }

  // Touch ~60 Hz
  static uint32_t lastTouchMs = 0;
  if (millis() - lastTouchMs > 16) {
    lastTouchMs = millis();
    pollTouch();
  }

  // BLE manager ~2 Hz: keep advertising in the right state (live toggle, no
  // reboot), refresh the status icon, and surface the Accept/Reject/Block
  // prompt on a new connection — but only once the device is unlocked, so the
  // request appears *after* the PIN is entered.
  static uint32_t lastBleMs = 0;
  if (millis() - lastBleMs > 500) {
    lastBleMs = millis();

    if (hidBleCompiled()) {
      // Advertise when BLE is enabled AND not inside a 5-minute block window.
      bool wantBle = settings.bleEnabled && (millis() >= bleBlockUntil);
      if (wantBle && !hidBleStarted())  hidBleBegin();
      if (!wantBle && hidBleStarted()) { hidBleEnd(); bleAuthorized = false; }
    }

    static uint32_t connRisingAt = 0;
    bool nowConn = settings.bleEnabled && hidBleCompiled() && hidBleConnected();
    if (nowConn && !btConnected) {                          // rising edge
      connRisingAt = millis();
      hidBleTune();        // pin a tight interval now, well before typing
    }
    if (nowConn != btConnected) {
      btConnected = nowConn;
      if (current != SCR_FLASH) { drawStatusBar(); flushScreen(); }
    }
    if (!nowConn) { bleAuthorized = false; hidBleSettleReset(); }  // peer dropped

    // Only prompt once the link has been STABLE for a moment — early in a BLE
    // connection the link can briefly flap (pairing/bonding handshake), and
    // prompting on that flap made the Accept page flash then re-appear.
    bool stable = nowConn && (millis() - connRisingAt > 900);
    bool unlocked = (current != SCR_LOCK && current != SCR_PIN);
    if (stable && !bleAuthorized && unlocked && !screenOff &&
        millis() >= bleBlockUntil && millis() >= bleGateSnooze) {
      bleConnectGate();                         // blocking Accept/Reject/Block
    }
  }

  // WiFi import screen: refresh ~1 Hz so the "Imported" count stays live.
  static uint32_t lastWifiDraw = 0;
  if (current == SCR_WIFI && millis() - lastWifiDraw > 1000) {
    lastWifiDraw = millis();
    drawWifi();
  }

  // Auto-lock after idle (security). Skipped on lock/PIN, and on the
  // flashlight + WiFi-import (no vault list shown, and they shouldn't be
  // interrupted while in use).
  if (settings.autoLockSec > 0 && !screenOff &&
      current != SCR_LOCK && current != SCR_PIN &&
      current != SCR_FLASH && current != SCR_WIFI &&
      (millis() - lastActivityMs) > (uint32_t)settings.autoLockSec * 1000UL) {
    homeExitReorder();
    navTop = 0;
    navStack[0] = SCR_LOCK;
    current     = SCR_LOCK;
    pinLen      = 0;
    drawAll();
    out->Display_Brightness(0);      // genuinely dark until next tap
    screenOff = true;
    ledSet(0xFF0000, 150);
  }

  delay(1);
}
