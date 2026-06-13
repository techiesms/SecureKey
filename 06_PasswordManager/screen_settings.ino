// =============================================================
//  screen_settings.ino  —  Settings list
//
//  Items (smartwatch-style):
//    • Bluetooth         (toggle — reboots for clean init order)
//    • USB HID           (toggle)
//    • 2-Tap Sleep       (toggle)
//    • Auto-Lock         (value)
//    • Brightness        (slider, 20–250)
//    • Change PIN        (action)
//    • Test HID Now      (action)
//    • Import (WiFi)     (action)
//    • About             (info)
//    • Factory Reset     (action — PIN-gated)
// =============================================================

#define SET_LIST_TOP   (STATUS_H + NAV_H + 10)
#define SET_TOGGLE_W   60      // bigger tap target
#define SET_TOGGLE_H   30

void typeViaHID(const char *s);   // defined in 06_PasswordManager.ino

// ── Scrolling support ────────────────────────────────────────
int32_t  settingsScrollY = 0;
#define  SET_ROW_COUNT   11
#define  SET_ROW_STEP    (SET_ITEM_H + 8)

// Right-aligned value labels for the "value + chevron" rows
static const char* autoLockLabel() {
  switch (settings.autoLockSec) {
    case 0:   return "Off";
    case 15:  return "15s";
    case 30:  return "30s";
    case 60:  return "1m";
    case 120: return "2m";
    default:  return "30s";
  }
}
static inline int32_t setMaxScroll() {
  int32_t visible = LCD_HEIGHT - SET_LIST_TOP - 8;
  int32_t total   = SET_ROW_COUNT * SET_ROW_STEP;
  return total > visible ? total - visible : 0;
}
void setScroll(int16_t dy) {
  int32_t y = settingsScrollY + (int32_t)dy;
  int32_t maxS = setMaxScroll();
  if (y < 0)    y = 0;
  if (y > maxS) y = maxS;
  settingsScrollY = y;
  drawSettings();
}

static void drawToggle(int16_t x, int16_t y, bool on) {
  // iOS-style pill: blue track + white knob when ON, dark track when OFF.
  uint16_t bg     = on ? C_BLUE   : C_GRAY_1;
  uint16_t border = on ? C_BLUE   : C_GRAY_3;
  uint16_t knob   = C_WHITE;

  gfx->fillRoundRect(x, y, SET_TOGGLE_W, SET_TOGGLE_H,
                     SET_TOGGLE_H/2, bg);
  gfx->drawRoundRect(x, y, SET_TOGGLE_W, SET_TOGGLE_H,
                     SET_TOGGLE_H/2, border);
  int16_t knobX = on ? x + SET_TOGGLE_W - SET_TOGGLE_H + 2
                     : x + 2;
  gfx->fillCircle(knobX + SET_TOGGLE_H/2 - 2,
                  y + SET_TOGGLE_H/2,
                  SET_TOGGLE_H/2 - 4, knob);
}

static void drawSlider(int16_t x, int16_t y, int16_t w, uint8_t val,
                       uint8_t minV, uint8_t maxV) {
  int16_t h = 8;
  int16_t sy = y + (24 - h) / 2;
  gfx->fillRoundRect(x, sy, w, h, h/2, C_GRAY_1);
  gfx->drawRoundRect(x, sy, w, h, h/2, C_GRAY_3);
  int16_t fillW = (int16_t)((int32_t)(val - minV) * w / (maxV - minV));
  gfx->fillRoundRect(x, sy, fillW, h, h/2, C_BLUE);

  // Knob
  int16_t knobX = x + fillW;
  gfx->fillCircle(knobX, sy + h/2, 8, C_WHITE);
  gfx->drawCircle(knobX, sy + h/2, 8, C_GRAY_3);
}

static void drawSetRow(int16_t y, const char *label, uint8_t kind,
                       bool boolVal = false, uint8_t numVal = 0,
                       const char *valStr = nullptr) {
  int16_t x = SAFE_PAD;
  int16_t w = LCD_WIDTH - 2*SAFE_PAD;
  gfx->fillRoundRect(x, y, w, SET_ITEM_H, 10, C_GRAY_1);
  gfx->drawRoundRect(x, y, w, SET_ITEM_H, 10, C_GRAY_2);

  // Left accent bar — red for the destructive row, blue otherwise
  bool danger = (strcmp(label, "Factory Reset") == 0);
  gfx->fillRoundRect(x + 8, y + 14, 4, SET_ITEM_H - 28, 2,
                     danger ? C_RED : C_BLUE);

  // Label
  gfx->setTextSize(2); gfx->setTextColor(danger ? C_RED : C_WHITE);
  gfx->setCursor(x + 22, y + (SET_ITEM_H - 14) / 2);
  gfx->print(label);

  // Right control
  if (kind == 0) { // toggle
    drawToggle(x + w - SET_TOGGLE_W - 12,
               y + (SET_ITEM_H - SET_TOGGLE_H) / 2, boolVal);
  } else if (kind == 1) { // slider
    drawSlider(x + w/2, y + (SET_ITEM_H - 24)/2, w/2 - 14,
               numVal, 20, 250);
  } else if (kind == 2) { // chevron
    gfx->setTextSize(2); gfx->setTextColor(C_GRAY_3);
    gfx->setCursor(x + w - 22, y + (SET_ITEM_H - 14) / 2);
    gfx->print(">");
  } else if (kind == 3) { // value + chevron
    if (valStr) {
      gfx->setTextSize(2); gfx->setTextColor(C_BLUE);
      int16_t x1, y1; uint16_t tw, th;
      gfx->getTextBounds(valStr, 0, 0, &x1, &y1, &tw, &th);
      gfx->setCursor(x + w - 32 - (int16_t)tw, y + (SET_ITEM_H - 14) / 2);
      gfx->print(valStr);
    }
    gfx->setTextSize(2); gfx->setTextColor(C_GRAY_3);
    gfx->setCursor(x + w - 22, y + (SET_ITEM_H - 14) / 2);
    gfx->print(">");
  }
}

void drawSettings() {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  drawNavBar("Settings", true, nullptr);

  int16_t y0     = SET_LIST_TOP - (int16_t)settingsScrollY;
  int16_t step   = SET_ROW_STEP;
  int16_t viewT  = SET_LIST_TOP;
  int16_t viewB  = LCD_HEIGHT - 4;

  struct Row { const char *label; uint8_t kind; bool b; uint8_t n; const char *val; };
  Row rows[SET_ROW_COUNT] = {
    {"Bluetooth",       0, settings.bleEnabled,      0, nullptr},
    {"USB HID",         0, settings.usbHidEnabled,   0, nullptr},
    {"Android",         0, settings.androidFix,      0, nullptr},
    {"2-Tap Sleep",     0, settings.doubleTapSleep,  0, nullptr},
    {"Auto-Lock",       3, false, 0, autoLockLabel()},
    {"Brightness",      1, false, settings.brightness, nullptr},
    {"Change PIN",      2, false, 0, nullptr},
    {"Test HID Now",    2, false, 0, nullptr},
    {"Import (WiFi)",   2, false, 0, nullptr},
    {"About",           2, false, 0, nullptr},
    {"Factory Reset",   2, false, 0, nullptr},
  };
  for (int i = 0; i < SET_ROW_COUNT; i++) {
    int16_t ry = y0 + i * step;
    if (ry + SET_ITEM_H < viewT) continue;
    if (ry > viewB) break;
    drawSetRow(ry, rows[i].label, rows[i].kind, rows[i].b, rows[i].n, rows[i].val);
  }

  // Scrollbar
  int32_t maxS = setMaxScroll();
  if (maxS > 0) {
    int32_t total  = SET_ROW_COUNT * step;
    int16_t visH   = viewB - viewT;
    int16_t sbH    = (int16_t)((int32_t)visH * visH / total);
    if (sbH < 16) sbH = 16;
    int16_t sbY    = viewT + (int16_t)((int32_t)(visH - sbH) * settingsScrollY / maxS);
    gfx->fillRect(LCD_WIDTH - 3, viewT, 3, visH, C_GRAY_2);
    gfx->fillRect(LCD_WIDTH - 3, sbY,   3, sbH,  C_GRAY_4);
  }

  flushScreen();
}

// ── About screen ─────────────────────────────────────────────
static void drawAbout() {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  drawNavBar("About", true, nullptr);

  // Real capacity: whichever is smaller — the firmware index cap or how
  // many 256-byte records actually fit in the FFat partition.
  uint32_t fitStore = (uint32_t)(FFat.totalBytes() / RECORD_SIZE);
  uint32_t capacity = fitStore < (uint32_t)MAX_PASSWORDS ? fitStore
                                                         : (uint32_t)MAX_PASSWORDS;

  int16_t y = STATUS_H + NAV_H + 14;
  textCenter(y, "SecureKey", 3, C_WHITE);            y += 36;
  textCenter(y, "Password Manager", 1, C_GRAY_3);    y += 30;

  char buf[48];
  snprintf(buf, sizeof(buf), "%u saved", passwordCount);
  textCenter(y, buf, 2, C_WHITE);                    y += 30;
  snprintf(buf, sizeof(buf), "Holds up to %lu", (unsigned long)capacity);
  textCenter(y, buf, 2, C_WHITE);                    y += 24;
  snprintf(buf, sizeof(buf), "(storage fits ~%lu)", (unsigned long)fitStore);
  textCenter(y, buf, 1, C_GRAY_4);                   y += 30;

  textCenter(y, "MaTouch ESP32-S3 1.8\" AMOLED", 1, C_GRAY_4);  y += 28;

  // Credit
  textCenter(y, "Developed by", 1, C_GRAY_3);        y += 18;
  textCenter(y, "techiesms", 3, C_WHITE);

  textCenter(LCD_HEIGHT - 36, "tap to go back", 1, C_GRAY_3);
  flushScreen();
}

// Blocking single-tap reader for modal screens. Reads touch DIRECTLY via
// ftReadTouch() rather than pollTouch(), so a finger-release does not
// re-dispatch into the settings screen underneath. Returns the press
// coordinates once a full press→release completes.
extern bool ftReadTouch(uint16_t &x, uint16_t &y);
static void settingsWaitTap(int16_t &tx, int16_t &ty) {
  uint16_t x, y;
  while (ftReadTouch(x, y)) delay(8);     // wait for any prior touch to lift
  while (!ftReadTouch(x, y)) delay(8);    // wait for a fresh press
  tx = (int16_t)x; ty = (int16_t)y;
  while (ftReadTouch(x, y)) delay(8);     // wait for release
}

// ── Blocking PIN gate (protects Factory Reset) ───────────────
//  Returns true only if the user types the correct PIN. The back
//  arrow cancels and returns false.
static const char *RG_KEYS[12] = {
  "1","2","3","4","5","6","7","8","9","BS","0","OK"
};
#define RG_W   96
#define RG_H   54
#define RG_G   8
#define RG_X0  ((LCD_WIDTH - (3*RG_W + 2*RG_G)) / 2)
#define RG_Y0  166

static void drawResetGate(const char *buf, uint8_t len) {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  drawNavBar("Factory Reset", true, nullptr);
  textCenter(STATUS_H + NAV_H + 10, "Enter PIN to confirm", 2, C_WHITE);

  int16_t cx = LCD_WIDTH / 2, dy = STATUS_H + NAV_H + 46;
  for (int i = 0; i < 4; i++) {
    int16_t x = cx - 54 + i * 36;
    if (i < len) gfx->fillCircle(x, dy, 9, C_WHITE);
    else         gfx->drawCircle(x, dy, 9, C_GRAY_3);
  }
  for (int i = 0; i < 12; i++) {
    int16_t x = RG_X0 + (i % 3) * (RG_W + RG_G);
    int16_t y = RG_Y0 + (i / 3) * (RG_H + RG_G);
    const char *k = RG_KEYS[i];
    bool isOK = (strcmp(k, "OK") == 0);
    uint16_t bg = isOK ? C_WHITE : C_GRAY_1;
    uint16_t fg = isOK ? C_BLACK : C_WHITE;
    gfx->fillRoundRect(x, y, RG_W, RG_H, 8, bg);
    if (!isOK) gfx->drawRoundRect(x, y, RG_W, RG_H, 8, C_GRAY_2);
    const char *lbl = (strcmp(k, "BS") == 0) ? "<" : k;
    uint8_t fs = (strlen(lbl) <= 1) ? 3 : 2;
    gfx->setTextSize(fs); gfx->setTextColor(fg);
    int16_t x1, y1; uint16_t tw, th;
    gfx->getTextBounds(lbl, 0, 0, &x1, &y1, &tw, &th);
    gfx->setCursor(x + (RG_W - (int16_t)tw) / 2, y + (RG_H - (int16_t)th) / 2);
    gfx->print(lbl);
  }
  flushScreen();
}

static bool settingsAskPin() {
  char buf[5] = {0}; uint8_t len = 0;
  for (;;) {
    drawResetGate(buf, len);
    int16_t tx, ty;
    settingsWaitTap(tx, ty);

    // Back arrow → cancel
    if (ty >= STATUS_H + 2 && ty < STATUS_H + NAV_H - 2
        && tx >= SAFE_PAD && tx < SAFE_PAD + 46) {
      return false;
    }
    // Which key?
    for (int i = 0; i < 12; i++) {
      int16_t x = RG_X0 + (i % 3) * (RG_W + RG_G);
      int16_t y = RG_Y0 + (i / 3) * (RG_H + RG_G);
      if (tx < x || tx >= x + RG_W || ty < y || ty >= y + RG_H) continue;
      const char *k = RG_KEYS[i];
      if (strcmp(k, "BS") == 0) {
        if (len) buf[--len] = 0;
      } else if (strcmp(k, "OK") == 0) {
        // require 4 digits — handled by the auto-check below
      } else if (len < 4) {
        buf[len++] = k[0]; buf[len] = 0;
      }
      if (len == 4) {
        buf[4] = 0;
        if (strcmp(buf, settings.pin) == 0) { ledSet(0x00FF00, 200); return true; }
        ledSet(0xFF0000, 400);             // wrong — clear and retry
        len = 0; buf[0] = 0;
      }
      break;
    }
  }
}

// ── Blocking Cancel / Erase confirmation ─────────────────────
static bool confirmErase() {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  textCenter(STATUS_H + 36, "ERASE ALL?", 4, C_WHITE);
  textCenter(STATUS_H + 96, "Deletes every password",  1, C_GRAY_5);
  textCenter(STATUS_H + 114, "and resets all settings.", 1, C_GRAY_5);
  textCenter(STATUS_H + 132, "This cannot be undone.",   1, C_GRAY_3);

  const int16_t bw = 130, bh = 56, gap = 14;
  const int16_t y = LCD_HEIGHT - bh - 36;
  const int16_t cx = LCD_WIDTH / 2;
  int16_t cancelX = cx - bw - gap/2;
  int16_t eraseX  = cx + gap/2;

  gfx->fillRoundRect(cancelX, y, bw, bh, 12, C_GRAY_1);
  gfx->drawRoundRect(cancelX, y, bw, bh, 12, C_GRAY_3);
  textCenter(y + bh/2 - 8, "CANCEL", 2, C_WHITE, cancelX + bw/2);

  gfx->fillRoundRect(eraseX, y, bw, bh, 12, C_WHITE);
  textCenter(y + bh/2 - 8, "ERASE", 2, C_BLACK, eraseX + bw/2);
  flushScreen();

  for (;;) {
    int16_t tx, ty;
    settingsWaitTap(tx, ty);
    if (ty >= y && ty <= y + bh) {
      if (tx >= cancelX && tx <= cancelX + bw) return false;
      if (tx >= eraseX  && tx <= eraseX  + bw) return true;
    }
  }
}

static void factoryReset() {
  if (!settingsAskPin()) { drawSettings(); return; }   // cancelled
  if (!confirmErase())   { drawSettings(); return; }

  // Wipe the vault and clear all settings (reseeds demo data on reboot).
  FFat.remove("/db.bin");
  prefs.begin("skset", false);
  prefs.clear();
  prefs.end();

  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  textCenter(LCD_HEIGHT/2 - 16, "RESET DONE", 3, C_WHITE);
  textCenter(LCD_HEIGHT/2 + 24, "Rebooting...", 2, C_GRAY_4);
  flushScreen();
  ledSet(0xFF0000, 400);
  delay(1500);
  ESP.restart();
}

// ── Tap dispatch ─────────────────────────────────────────────
void onTapSettings(int16_t tx, int16_t ty) {
  // Back
  if (ty >= STATUS_H + 2 && ty < STATUS_H + NAV_H - 2
      && tx >= SAFE_PAD && tx < SAFE_PAD + 46) {
    popNav(); return;
  }

  // Convert tap-y to virtual coordinate (account for scroll)
  ty = ty + (int16_t)settingsScrollY;

  int16_t y = SET_LIST_TOP;
  int16_t step = SET_ROW_STEP;

  // Bluetooth toggle — LIVE on/off, NO reboot. Turning ON starts advertising
  // immediately; turning OFF stops it and forgets any accepted connection.
  // (The BLE manager in loop() also maintains this idempotently.)
  if (ty >= y && ty < y + SET_ITEM_H) {
    if (!hidBleCompiled()) {
      // hid_ble.cpp was compiled with HID_BLE_ENABLE 0
      settings.bleEnabled = false; saveSettings();
      gfx->fillScreen(C_BLACK);
      drawStatusBar();
      textCenter(STATUS_H + 30, "BLE LIBRARY", 3, C_WHITE);
      textCenter(STATUS_H + 64, "MISSING",     3, C_WHITE);
      textCenter(STATUS_H + 110, "Install 'ESP32 BLE",     1, C_GRAY_5);
      textCenter(STATUS_H + 128, "Keyboard' (T-vK) then",  1, C_GRAY_5);
      textCenter(STATUS_H + 146, "re-flash the firmware.", 1, C_GRAY_5);
      flushScreen();
      delay(2200);
      drawSettings(); return;
    }
    settings.bleEnabled = !settings.bleEnabled;
    bleBlockUntil = 0;                 // a manual toggle clears any block window
    saveSettings();
    if (settings.bleEnabled) {
      hidBleBegin();
    } else {
      bleAuthorized = false;
      hidBleEnd();
      hidBleForget();        // wipe bonds → turning BLE off then on re-pairs fresh
      btConnected = false;
    }
    drawSettings(); return;            // inline flip — no confirmation page
  }
  y += step;

  // USB toggle row — hardware is already initialised at boot, this just
  // gates whether typeViaHID() routes through USB. No re-init needed.
  if (ty >= y && ty < y + SET_ITEM_H) {
    settings.usbHidEnabled = !settings.usbHidEnabled;
    saveSettings();
    drawSettings(); return;
  }
  y += step;

  // Android @ Fix — swaps @ and " keycodes on the BLE path for UK/Android
  // hosts (where the US Shift+2 produces " instead of @).
  if (ty >= y && ty < y + SET_ITEM_H) {
    settings.androidFix = !settings.androidFix;
    hidBleSetAndroidFix(settings.androidFix ? 1 : 0);
    saveSettings();
    drawSettings(); return;
  }
  y += step;

  // Double-tap sleep toggle
  if (ty >= y && ty < y + SET_ITEM_H) {
    settings.doubleTapSleep = !settings.doubleTapSleep;
    saveSettings();
    drawSettings(); return;
  }
  y += step;

  // Auto-Lock — cycle Off / 15s / 30s / 1m / 2m
  if (ty >= y && ty < y + SET_ITEM_H) {
    uint8_t c = settings.autoLockSec;
    settings.autoLockSec = (c == 0)  ? 15
                         : (c == 15) ? 30
                         : (c == 30) ? 60
                         : (c == 60) ? 120 : 0;
    saveSettings();
    lastActivityMs = millis();      // don't instantly trip the new timeout
    drawSettings(); return;
  }
  y += step;

  // Brightness slider — tap-x maps to value
  if (ty >= y && ty < y + SET_ITEM_H) {
    int16_t sliderX = SAFE_PAD + (LCD_WIDTH - 2*SAFE_PAD)/2;
    int16_t sliderW = (LCD_WIDTH - 2*SAFE_PAD)/2 - 14;
    if (tx >= sliderX && tx <= sliderX + sliderW) {
      uint8_t v = 20 + (uint8_t)((int32_t)(tx - sliderX) * (250 - 20) / sliderW);
      settings.brightness = v;
      out->Display_Brightness(v);
      saveSettings();
      drawSettings();
    }
    return;
  }
  y += step;

  // Change PIN — open the 3-step flow
  if (ty >= y && ty < y + SET_ITEM_H) {
    chgPinInit();
    pushNav(SCR_CHGPIN);
    return;
  }
  y += step;

  // Test HID — diagnostics + actually type a sample line. Reports whether the
  // USB host can SEE the keyboard (tud_mounted), which separates an enumeration
  // problem (USB Mode / CDC-On-Boot / cable) from a routing/layout one.
  if (ty >= y && ty < y + SET_ITEM_H) {
    bool usbHost = settings.usbHidEnabled && hidUsbCompiled() && hidUsbMounted();
    bool bleConn = settings.bleEnabled && hidBleCompiled() && hidBleConnected();

    gfx->fillScreen(C_BLACK);
    drawStatusBar();
    textCenter(STATUS_H + 24, "HID TEST", 3, C_WHITE);
    char line[40];
    snprintf(line, sizeof(line), "USB host: %s", usbHost ? "CONNECTED" : "NOT SEEN");
    textCenter(STATUS_H + 78, line, 2, usbHost ? C_WHITE : C_RED);
    snprintf(line, sizeof(line), "BLE: %s",
             bleConn ? (bleAuthorized ? "ACCEPTED" : "tap ACCEPT") : "not connected");
    textCenter(STATUS_H + 108, line, 2, bleConn ? C_WHITE : C_GRAY_4);
    textCenter(STATUS_H + 152, "open Notepad on the host,", 1, C_GRAY_5);
    textCenter(STATUS_H + 170, "typing in 2s...",          1, C_GRAY_5);
    flushScreen();
    delay(2000);

    if (usbHost)                        hidUsbPrint("SecureKey USB test 1234");
    else if (bleConn && bleAuthorized)  hidBlePrint("SecureKey BLE test 1234");
    else                                typeViaHID("SecureKey test");  // help modal

    drawSettings();
    return;
  }
  y += step;

  // Import (WiFi) — start the on-device captive portal, show its credentials
  if (ty >= y && ty < y + SET_ITEM_H) {
    wifiPortalStart();
    pushNav(SCR_WIFI);
    return;
  }
  y += step;

  // About — show until the user taps anywhere
  if (ty >= y && ty < y + SET_ITEM_H) {
    drawAbout();
    int16_t atx, aty;
    settingsWaitTap(atx, aty);
    drawSettings(); return;
  }
  y += step;

  // Factory Reset — PIN-gated, then confirm, then wipe + reboot
  if (ty >= y && ty < y + SET_ITEM_H) {
    factoryReset();
    return;
  }
}
