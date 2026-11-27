/*
 * SecureKey — Hardware Password Manager  (monochrome edition)
 * ─────────────────────────────────────────────────────────────────────
 *  Board   : Waveshare ESP32-S3 Touch AMOLED 1.8" V2 (368×448, CO5300 QSPI)
 *  Touch   : CST820 capacitive
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
  void hidUsbSetAndroidFix(int on);   // UK/Android @<->" keycode swap (USB)

  void hidBleBegin();
  void hidBleEnd();
  int  hidBleConnected();
  void hidBlePrint(const char *s);
  void hidBleQuickFill(const char *user, const char *pass);
  int  hidBleCompiled();
  int  hidBleStarted();
  void hidBleSetAndroidFix(int on);   // UK/Android @<->" keycode swap
  void hidBlePeerAddr(char *out, int n);   // connecting peer's BT address
  int  hidBlePeerIdAddr(char *out, int n, int *type);  // 1=bonded/stable id
  int  hidBleCapturePeer(char *ota, int otaN, char *id, int idN,
                         int *type, int *bonded);       // one-shot peer snapshot
  void hidBleForgetOne(const char *addr, int type);    // delete one bond
  void hidBleSettleReset();           // re-arm typing settle on disconnect
  void hidBleTune();                  // request tight conn interval at connect
  void hidBleTuneCached();            // same, but by cached handle (no list copy)
  void hidBleDisconnectCached();      // disconnect by cached handle (no list copy)
  void hidBleForget();                // clear all BLE bonds (re-pair fresh)
  void hidBleDisconnectPeer();        // kick current peer, keep advertising
  void hidBleReadvertise();           // restart advertising after a disconnect
  void hidBleReturn();                // send Return key over BLE
  void hidUsbReturn();                // send Return key over USB
}

#include <Arduino.h>
#include <Wire.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_NeoPixel.h>
#include <FFat.h>
#include <Preferences.h>
#include "pin_config.h"
#include "theme.h"
#include "esp_system.h"     // esp_reset_reason() — logged at boot

// HID transports are isolated to hid_usb.cpp / hid_ble.cpp — see above

// ── Hardware objects ─────────────────────────────────────────────────
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_GFX *out = new Arduino_CO5300(
  bus, LCD_RST, 0, false, LCD_WIDTH, LCD_HEIGHT, 16, 0, 0, 0);
Arduino_Canvas *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, out);
Adafruit_NeoPixel led(RGB_LED_COUNT, RGB_LED_PIN, NEO_GRB + NEO_KHZ800);
Preferences      prefs;

static uint8_t ioxOutput = 0;
static bool ioxWrite(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(IOX_ADDR); Wire.write(reg); Wire.write(value);
  return Wire.endTransmission() == 0;
}
static void ioxSet(uint8_t mask, bool high) {
  if (high) ioxOutput |= mask; else ioxOutput &= (uint8_t)~mask;
  ioxWrite(0x01, ioxOutput);
}
static bool initWaveshareV2Board() {
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  if (!ioxWrite(0x03, 0x00)) {
    Serial.println("[BOARD] TCA9554 not found at 0x20"); return false;
  }
  ioxOutput = IOX_LCD_RST | IOX_PWR_EN | IOX_TP_RST;
  ioxWrite(0x01, ioxOutput); delay(100);
  ioxOutput = 0; ioxWrite(0x01, ioxOutput); delay(300);
  ioxOutput = IOX_LCD_RST | IOX_PWR_EN | IOX_TP_RST;
  ioxWrite(0x01, ioxOutput); delay(100);
  Serial.println("[BOARD] Waveshare V2 TCA9554 ready"); return true;
}

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
bool     bleAuthorized  = false;          // true once the user accepts this connection
uint32_t bleBlockUntil  = 0;             // millis() until which this peer is silently kicked
uint32_t bleGateSnooze  = 0;            // after REJECT: don't re-prompt before this
char     bleBlockedAddr[24] = {0};       // MAC of the specifically blocked peer

// Saved BLE devices ("whitelist"): devices the user accepted once are
// remembered by identity address and auto-approved on reconnect — no
// repeated Accept prompt. Managed on the Devices screen (Settings → Devices).
SavedDevice savedDevs[MAX_BLE_DEVICES];
uint8_t     savedDevCount = 0;

// Cached identity of the currently-connected BLE peer. Updated ONCE per BLE
// manager tick from the main loop; the Devices screen and auto-approve read
// these instead of calling NimBLE per-draw (which raced the NimBLE core-0 task
// and reset the device whenever the Devices screen was open).
char        connPeerId[18]  = {0};   // resolved identity, or "" if none/unbonded
char        connPeerOta[24] = {0};   // over-the-air address (for the gate display)
int         connPeerType    = 0;
bool        connPeerBonded  = false;
bool        connPeerCaptured = false; // got a snapshot for this connection?

// User pressed DISCONNECT on the Devices screen: BLE is fully PAUSED — link
// dropped AND advertising stopped — until they press CONNECT. Pausing the
// radio (instead of kicking the phone every time it auto-reconnects, as the
// old per-device pause did) is what keeps iOS sane: the kick-on-reconnect
// loop made the phone flap — "connected" in its settings, typed nothing,
// dropped, retried — until the user had to Forget the device. RAM-only;
// cleared by CONNECT, the Bluetooth toggle, or a reboot.
bool        bleUserPaused = false;

void devsSave() {
  prefs.begin("skset", false);
  prefs.putBytes("devs", savedDevs, (size_t)savedDevCount * sizeof(SavedDevice));
  prefs.end();
}
void devsLoad() {
  prefs.begin("skset", true);
  size_t n = prefs.getBytesLength("devs");
  savedDevCount = 0;
  if (n > 0 && n % sizeof(SavedDevice) == 0 && n <= sizeof(savedDevs)) {
    prefs.getBytes("devs", savedDevs, n);
    savedDevCount = (uint8_t)(n / sizeof(SavedDevice));
  }
  prefs.end();
}
int devsFind(const char *addr) {
  for (uint8_t i = 0; i < savedDevCount; i++)
    if (strcasecmp(savedDevs[i].addr, addr) == 0) return i;
  return -1;
}
void devsAdd(const char *addr, int type) {
  if (!addr || !addr[0] || strcmp(addr, "unknown") == 0) return;
  if (devsFind(addr) >= 0) return;                  // already saved
  if (savedDevCount >= MAX_BLE_DEVICES) {           // full → drop the oldest
    memmove(&savedDevs[0], &savedDevs[1],
            (MAX_BLE_DEVICES - 1) * sizeof(SavedDevice));
    savedDevCount = MAX_BLE_DEVICES - 1;
  }
  SavedDevice &d = savedDevs[savedDevCount++];
  memset(&d, 0, sizeof(d));
  strncpy(d.addr, addr, sizeof(d.addr) - 1);
  d.addrType = (uint8_t)type;
  // Default alias: "Device EE:FF" (address tail) until the user renames it.
  const char *tail = strlen(addr) >= 5 ? addr + strlen(addr) - 5 : addr;
  snprintf(d.name, sizeof(d.name), "Device %s", tail);
  devsSave();
  Serial.printf("[BLE] saved device %s as '%s'\n", d.addr, d.name);
}
void devsRemove(uint8_t idx) {
  if (idx >= savedDevCount) return;
  hidBleForgetOne(savedDevs[idx].addr, savedDevs[idx].addrType);
  memmove(&savedDevs[idx], &savedDevs[idx + 1],
          (size_t)(savedDevCount - idx - 1) * sizeof(SavedDevice));
  savedDevCount--;
  devsSave();
}

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
UserSettings settings = { 140, false, true, "1234", 30, false };

// Idle tracking for auto-lock (refreshed on every touch in pollTouch)
uint32_t   lastActivityMs = 0;

// Screen-off state (side button only — automated sleep modes were removed:
// idle light-sleep / double-tap sleep read as "touch randomly stops working"
// in QA, because the dark panel + double-tap-to-wake looked like dead touch).
bool       screenOff   = false;

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
void drawDevices();   void onTapDevices(int16_t,int16_t);   void devicesInit();
void drawAll();
void drawStatusBar();
void pollTouch();
void popToLock();
void screenSleep();

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
    case SCR_DEVICES:     drawDevices();   break;
    default:                               break;
  }
}

void dispatchTap(int16_t tx, int16_t ty) {
  // Screen OFF (side button): ignore ALL touch. The touch controller stays
  // powered (so it can't wedge like it did coming out of light sleep), but
  // this software gate means a pocket/skin contact can never register —
  // only the side button turns the screen back on.
  if (screenOff) return;

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
    case SCR_DEVICES:     onTapDevices(tx, ty);    break;
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
  if (s == SCR_DEVICES)  { devicesInit(); }
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
  (void)totalDx;
  // Swipe UP on lock → PIN
  if (current == SCR_LOCK && totalDy < -50) { pushNav(SCR_PIN); return; }
  // Swipe DOWN on PIN → lock
  if (current == SCR_PIN  && totalDy >  60) { popNav(); return; }
  // Swipe-right "go back" gesture REMOVED (by request): ghost/mis-touch drags
  // kept popping screens "by themselves". Every screen has an explicit back
  // arrow — that is now the only way back.
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

// ── Enter key dispatcher ──────────────────────────────────────────────
// Sends a bare Return keystroke on whichever transport is active.
// Called by screen_detail.ino after every TYPE action.
void typeReturnViaHID() {
  if (settings.bleEnabled && hidBleCompiled() && hidBleConnected() && bleAuthorized) {
    hidBleReturn();
    return;
  }
  if (settings.usbHidEnabled && hidUsbCompiled() && hidUsbMounted()) {
    hidUsbReturn();
  }
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
  // Use the CACHED address (captured once per connection by the loop) — no
  // NimBLE list query here, which is what was crashing the device.
  const char *paddr = connPeerOta[0] ? connPeerOta : "unknown";
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
    // Remember this device so it's auto-approved next time — but ONLY once
    // bonding has resolved a STABLE identity. Saving before that stored the
    // phone's rotating private address, which never matched on reconnect
    // ("shows a new device every time"). Bonding usually completes within a
    // second, so re-snapshot briefly for a bonded identity. This is a calm
    // moment (gate is blocking, no redraws), so the single query is safe.
    char ota[24], idAddr[18]; int idType = 0, bonded = 0;
    for (int tries = 0; tries < 20; tries++) {          // up to ~2 s
      hidBleCapturePeer(ota, sizeof(ota), idAddr, sizeof(idAddr), &idType, &bonded);
      if (bonded) break;
      delay(100);
    }
    if (bonded) {
      devsAdd(idAddr, idType);
      strncpy(connPeerId, idAddr, sizeof(connPeerId) - 1); connPeerId[sizeof(connPeerId) - 1] = 0;
      connPeerBonded = true;
    } else Serial.println("[BLE] not bonded yet — not saved (accepted for now)");
  } else if (choice == 2) {                  // BLOCK 5 min
    bleAuthorized = false;
    bleBlockUntil = millis() + 300000UL;     // this MAC silently kicked for 5 min
    strncpy(bleBlockedAddr, connPeerOta[0] ? connPeerOta : "unknown",
            sizeof(bleBlockedAddr) - 1);
    bleBlockedAddr[sizeof(bleBlockedAddr) - 1] = 0;
    hidBleDisconnectPeer();                  // kick only this peer, BLE keeps advertising
    ledSet(0xFF0000, 250);
    Serial.printf("[BLE] BLOCKED %s for 5 min (BLE still up for other devices)\n", bleBlockedAddr);
  } else {                                   // REJECT
    // Snooze the gate for 2 minutes so a paired phone that auto-reconnects
    // doesn't spam the prompt every 20 seconds.  Typing stays blocked
    // (bleAuthorized=false); the link is left up so the phone doesn't
    // instantly re-try pairing.
    bleAuthorized = false;
    bleGateSnooze = millis() + 120000UL;     // 2 min quiet
    ledSet(0xFF0000, 200);
    Serial.println("[BLE] connection REJECTED (snoozed 2 min)");
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
  settings.autoLockSec    = prefs.getUChar("alock", 30);
  settings.androidFix     = prefs.getBool ("afix", false);
  pinFails                = prefs.getUChar("pinf",  0);
  String p = prefs.getString("pin", "1234");
  strncpy(settings.pin, p.c_str(), 4); settings.pin[4] = 0;
  prefs.end();
  hidBleSetAndroidFix(settings.androidFix ? 1 : 0);
  hidUsbSetAndroidFix(settings.androidFix ? 1 : 0);
}
void saveSettings() {
  prefs.begin("skset", false);
  prefs.putUChar ("br",   settings.brightness);
  prefs.putBool  ("ble",  settings.bleEnabled);
  prefs.putBool  ("usb",  settings.usbHidEnabled);
  prefs.putUChar ("alock", settings.autoLockSec);
  prefs.putBool  ("afix", settings.androidFix);
  prefs.putString("pin",  settings.pin);
  prefs.end();
}

// ── Boot reset-reason (for the "random auto-restart" QA bug) ──────────
esp_reset_reason_t bootRR = ESP_RST_UNKNOWN;
const char *bootRRStr() {
  switch (bootRR) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW_RESTART";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    default:               return "OTHER";
  }
}
// True for resets we didn't ask for (brownout / crash / watchdog) — the ones
// worth flagging on-screen. A clean power-on or an intentional ESP.restart()
// (factory reset) are expected, so we stay quiet for those.
static bool bootWasAbnormal() {
  return bootRR == ESP_RST_BROWNOUT || bootRR == ESP_RST_PANIC ||
         bootRR == ESP_RST_INT_WDT  || bootRR == ESP_RST_TASK_WDT ||
         bootRR == ESP_RST_WDT;
}

// ── Setup ────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // Native USB-CDC (USB Mode = OTG/TinyUSB) only accepts bytes once the host
  // has re-attached after a reset — without this wait the early boot log
  // (incl. the reset-reason line below) is transmitted into the void and lost.
  // Bounded so an UNmonitored boot only pauses briefly.
  for (uint32_t t = millis(); !Serial && millis() - t < 1500; ) delay(10);
  delay(100);
  Serial.println("\n[SecureKey] Password Manager (mono) starting...");

  // Capture WHY we booted — the key clue for the "random auto-restart" QA bug.
  // Surfaced on serial AND on-screen (below) so brownout vs crash is visible
  // even on battery with no cable attached.
  bootRR = esp_reset_reason();
  Serial.printf("[BOOT] reset reason: %s (%d)\n", bootRRStr(), (int)bootRR);

  led.begin();  led.setBrightness(40);  led.clear(); led.show();

  initWaveshareV2Board();

  if (!gfx->begin()) {
    // The PSRAM double-buffer canvas could not initialise — almost always
    // because PSRAM isn't enabled. Draw the reason DIRECTLY on the panel so
    // the screen isn't just silent-black: if you SEE this red message the
    // panel itself works and the fix is a board setting, not the hardware.
    //   Tools -> PSRAM: "OPI PSRAM"   (and Flash 16MB, Partition app3M_fat9M)
    Serial.println("ERROR: canvas init failed — set Tools -> PSRAM: OPI PSRAM");
    out->begin();
    out->fillScreen(0xF800);                  // red
    out->Display_Brightness(200);
    out->setTextColor(0xFFFF);                // white
    out->setTextSize(3);
    out->setCursor(60, 150); out->print("PSRAM");
    out->setCursor(20, 188); out->print("CANVAS FAIL");
    out->setTextSize(2);
    out->setCursor(30, 250); out->print("Tools > PSRAM");
    out->setCursor(40, 278); out->print("= OPI PSRAM");
    while (1) delay(1000);
  }

  // Register USB HID class FIRST (before the BLE stack), so TinyUSB can
  // claim the USB controller cleanly.  The BLE keyboard uses the NimBLE
  // backend, which coexists with native USB HID on the ESP32-S3, but the
  // init *order* still matters — USB first, BLE last.  Always-on USB HID
  // costs ~5 mA which is acceptable for a wired/charging device.
  if (hidUsbCompiled()) hidUsbBegin();

  loadSettings();
  devsLoad();                      // restore the saved BLE devices whitelist
  homeLoadOrder();                 // restore the user's home tile arrangement

  // Re-arm the PIN lockout across power cycles. millis() resets on reboot, so
  // without this a power-cycle would bypass the "wait 30s" penalty. The fail
  // count is persisted (NVS), so if we were still locked out when powered off,
  // restart the countdown — pulling the plug no longer skips the wait.
  if (pinFails >= 3) pinLockUntil = millis() + pinLockDelayMs(pinFails);

  // Safety floor: a corrupted/zero NVS brightness would leave the panel dark
  // (Display_Brightness(0) = screen off), which looks like a dead screen.
  if (settings.brightness < 20) { settings.brightness = 140; saveSettings(); }
  out->Display_Brightness(settings.brightness);

  // Boot splash. (Deep sleep was removed — every boot is a true power-on /
  // reset now, so the splash always plays.)
  bootRevealIn();
  uint32_t bootSplashMs = millis();

  pinMode(USER_BTN_PIN, INPUT_PULLUP);

  // FT3168 warm-up: fresh out of reset the touch controller can stay silent
  // on I2C for a moment, which read as "touch dead for the first second after
  // boot". Wait (max ~600 ms) until it ACKs its address; if it never does,
  // pulse TP_RST once and give it time to calibrate. Runs before the boot
  // splash, so it costs no visible time.
  {
    bool ack = false;
    for (uint32_t t0 = millis(); millis() - t0 < 600 && !ack; ) {
      Wire.beginTransmission((uint8_t)CST820_ADDR);
      ack = (Wire.endTransmission() == 0);
      if (!ack) delay(25);
    }
    if (!ack) {
      ioxSet(IOX_TP_RST, false);  delay(8);
      ioxSet(IOX_TP_RST, true); delay(150);
    }
    Serial.printf("[TOUCH] CST820 %s\n", ack ? "ready" : "reset-pulsed");
  }

  Serial.printf("[MEM] PSRAM free: %u\n", ESP.getFreePsram());
  passwordIndex = (ListItem *)ps_malloc(MAX_PASSWORDS * sizeof(ListItem));
  if (!passwordIndex) {
    // The canvas works (we got here) but the 1.9MB password index won't fit —
    // PSRAM is missing/too small. Show it on the (working) canvas instead of a
    // silent black hang.
    Serial.println("ERROR: ps_malloc failed for index");
    gfx->fillScreen(C_BLACK);
    textCenter(160, "LOW PSRAM", 3, C_RED);
    textCenter(212, "index alloc failed", 2, C_WHITE);
    textCenter(248, "set PSRAM = OPI", 2, C_GRAY_5);
    flushScreen();
    while (1) delay(1000);
  }

  if (!FFat.begin(true)) {
    Serial.println("ERROR: FFat mount failed");
  } else {
    dbLoadIndex();
    Serial.printf("[DB] %u passwords loaded\n", passwordCount);
  }

  // Keep the logo a beat (it's been up during init), play the booting dots,
  // then power-down fade.
  while (millis() - bootSplashMs < 250) delay(10);
  bootDots(1800);
  bootFadeOut();

  // Draw the UI immediately so the screen is always visible, even if BLE
  // has trouble coming up below.
  drawAll();
  out->Display_Brightness(settings.brightness);

  // If the last boot was NOT something we asked for (brownout / crash /
  // watchdog), flag it on-screen for a couple seconds. This makes the
  // "random auto-restart" bug diagnosable on battery with no serial cable:
  // BROWNOUT = power/supply, PANIC/WDT = firmware crash.
  if (bootWasAbnormal()) {
    char msg[40];
    snprintf(msg, sizeof(msg), "LAST RESET: %s", bootRRStr());
    gfx->fillRect(0, LCD_HEIGHT - 34, LCD_WIDTH, 34, C_BLACK);
    textCenter(LCD_HEIGHT - 24, msg, 1, C_RED);
    flushScreen();
    delay(2500);
    drawAll();                       // repaint clean
  }

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

// ── Power / screen off ───────────────────────────────────────────────
// Side-button screen off: lock, drop BLE, dark panel. The MCU and the touch
// controller STAY powered — deep/light sleep were removed because waking
// through them left the FT3168 wedged ("touch randomly stops working" in QA).
// All touch is ignored while off (see dispatchTap); only the button wakes.
void screenSleep() {
  homeExitReorder();
  navTop = 0; navStack[0] = SCR_LOCK; current = SCR_LOCK; pinLen = 0;
  drawAll();
  if (settings.bleEnabled && hidBleCompiled()) {
    hidBleEnd(); bleAuthorized = false; btConnected = false;
  }
  led.clear(); led.show(); ledClearAt = 0;
  out->Display_Brightness(0);
  screenOff = true;
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

  // SW2 physical → screen on/off toggle. Press to turn the screen OFF
  // (locked + dark, touch ignored); press again to turn it back ON.
  // This is the ONLY sleep-like mode left — no idle sleep, no double-tap.
  static bool btnLast = HIGH;
  bool btnNow = digitalRead(USER_BTN_PIN);
  if (btnLast == HIGH && btnNow == LOW) {
    if (screenOff) {
      out->Display_Brightness(settings.brightness);
      screenOff = false;
      lastActivityMs = millis();     // fresh idle window on wake
    } else {
      screenSleep();
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
      // Advertise whenever BLE is enabled AND the device is awake AND the
      // user hasn't paused it from the Devices screen. In sleep (screenOff)
      // BLE is forced off to save battery; it resumes on wake.
      bool wantBle = settings.bleEnabled && !screenOff && !bleUserPaused;
      if (wantBle && !hidBleStarted())  hidBleBegin();
      if (!wantBle && hidBleStarted()) { hidBleEnd(); bleAuthorized = false; }
    }

    static uint32_t connRisingAt = 0;
    static bool     connTuned    = false;
    bool nowConn = settings.bleEnabled && hidBleCompiled() && hidBleConnected();
    if (nowConn && !btConnected) {                          // rising edge
      connRisingAt = millis();
      connTuned = false;   // (re)tune once we've safely captured the handle
      // NOTE: no hidBleTune() here anymore — it copied the connection list at
      // the connect instant (peak churn), a reset source. We tune by cached
      // handle right after the capture below instead.
    }
    if (nowConn != btConnected) {
      if (btConnected && !nowConn) {
        // Disconnect edge: give a quiet window so a phone that auto-reconnects
        // the instant it drops can't immediately re-fire the Accept gate
        // ("requests keep coming"). A deliberate re-pair still works after 3 s.
        bleGateSnooze = millis() + 3000;
      }
      btConnected = nowConn;
      if (current == SCR_DEVICES)      drawAll();   // live card/list update
      else if (current != SCR_FLASH) { drawStatusBar(); flushScreen(); }
    }
    if (!nowConn) {                        // peer dropped (or never connected)
      bleAuthorized = false;
      hidBleSettleReset();
      // NimBLE re-advertises itself on disconnect (advertiseOnDisconnect) — no
      // manual restart needed here anymore.
    }

    // Snapshot the connected peer into the cache. We query NimBLE's connection
    // list ONLY until we've captured a bonded identity, and at most for the
    // first ~5 s after connecting — after that we never touch the list again
    // for this connection. Everything else (gate, auto-approve, Devices screen,
    // block check) reads the cached strings. This is the fix for the random
    // PANIC: getPeerDevices() copies a vector the BLE core mutates on
    // connect/disconnect, so calling it constantly (per redraw) eventually
    // read freed memory and reset the device. Steady state now = zero reads.
    if (nowConn) {
      // Keep snapshotting only for the first 8 s of a connection, and stop
      // early once we have a bonded identity. After that: never query again.
      bool tryCapture = (millis() - connRisingAt < 8000) && !connPeerBonded;
      if (tryCapture) {
        char ota[24], id[18]; int t = 0, b = 0;
        if (hidBleCapturePeer(ota, sizeof(ota), id, sizeof(id), &t, &b)) {
          strncpy(connPeerOta, ota, sizeof(connPeerOta) - 1); connPeerOta[sizeof(connPeerOta) - 1] = 0;
          strncpy(connPeerId,  id,  sizeof(connPeerId)  - 1); connPeerId[sizeof(connPeerId)   - 1] = 0;
          connPeerType   = t;
          connPeerBonded = b ? true : false;
          connPeerCaptured = true;
          if (!connTuned) { hidBleTuneCached(); connTuned = true; }  // safe: by handle
        }
      }
    } else {
      connPeerId[0] = 0; connPeerOta[0] = 0;
      connPeerBonded = false; connPeerType = 0; connPeerCaptured = false;
      connTuned = false;
    }

    // Only prompt once the link has been STABLE for a moment — early in a BLE
    // connection the link can briefly flap (pairing/bonding handshake), and
    // prompting on that flap made the Accept page flash then re-appear.
    bool stable = nowConn && (millis() - connRisingAt > 900);

    // Saved-device auto-approve: a device the user ACCEPTED before (matched by
    // its stable BONDED identity) skips the gate entirely — connect and type,
    // no prompt. To stop a saved device, use Settings → Devices → Disconnect /
    // Forget.
    if (stable && !bleAuthorized && connPeerBonded && connPeerId[0]) {
      if (devsFind(connPeerId) >= 0) {
        bleAuthorized = true;
        ledSet(0x00FF00, 200);
        Serial.printf("[BLE] auto-approved saved device %s\n", connPeerId);
        if (current != SCR_FLASH) { drawStatusBar(); flushScreen(); }
        if (current == SCR_DEVICES) drawAll();   // live status on the screen
      }
    }

    // Ghost-link watchdog: connected for 20+ s but NEVER encrypted. Classic
    // bond mismatch — e.g. we forgot the device but the phone still holds its
    // old keys: its reconnect "succeeds" at link level (phone shows connected)
    // but encryption/HID never come up, so typing goes nowhere. Kick it so the
    // phone fails cleanly and can re-pair fresh instead of ghosting forever.
    if (nowConn && !connPeerBonded && (millis() - connRisingAt) > 20000UL) {
      Serial.println("[BLE] link never encrypted in 20s — kicking ghost link");
      hidBleDisconnectCached();
    }

    // Only show the Accept gate once we've actually IDENTIFIED the peer —
    // captured + bonded, or the 8 s capture window elapsed. This gives the
    // saved-device auto-approve above first chance, so a known device no longer
    // occasionally flashes the request before its identity resolves.
    bool identified = connPeerCaptured &&
                      (connPeerBonded || (millis() - connRisingAt > 8000));
    bool unlocked = (current != SCR_LOCK && current != SCR_PIN);
    if (stable && identified && !bleAuthorized && unlocked && !screenOff &&
        millis() >= bleGateSnooze) {
      // If this is the specifically blocked MAC and the block window hasn't
      // expired, silently kick it — no prompt, BLE stays up for other devices.
      if (millis() < bleBlockUntil && bleBlockedAddr[0]) {
        // Compare against the CACHED address (no NimBLE query in this hot path).
        if (connPeerOta[0] && strcmp(connPeerOta, bleBlockedAddr) == 0) {
          hidBleDisconnectCached(); // silent kick by handle (repeats — no list copy)
        } else {
          bleConnectGate();         // different device — show normal gate
        }
      } else {
        bleConnectGate();           // block expired or no blocked addr — show gate
      }
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
  // NOTE: this only collapses to the LOCK screen — the display stays ON and
  // touch stays live. It used to lightSleep() here, which darkened the panel
  // and gated wake on a double-tap: in QA that read as "touch randomly stops
  // working during idle" (Bug 3). BLE also stays up, so an idle lock no
  // longer kills an accepted connection.
  if (settings.autoLockSec > 0 && !screenOff &&
      current != SCR_LOCK && current != SCR_PIN &&
      current != SCR_FLASH && current != SCR_WIFI &&
      (millis() - lastActivityMs) > (uint32_t)settings.autoLockSec * 1000UL) {
    popToLock();
  }

  delay(1);
}
