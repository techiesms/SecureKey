// =============================================================
//  screen_pin.ino  —  4-digit PIN keypad  (monochrome)
//
//  Correct PIN advances directly to HOME (bug fix — used to
//  bounce back to LOCK).
// =============================================================

struct PinKey { const char *label; uint8_t row, col; };
const PinKey PKEYS[12] = {
  {"1",0,0},{"2",0,1},{"3",0,2},
  {"4",1,0},{"5",1,1},{"6",1,2},
  {"7",2,0},{"8",2,1},{"9",2,2},
  {"BS",3,0},{"0",3,1},{"OK",3,2},
};

const int16_t PK_W  = 96;
const int16_t PK_H  = 60;
const int16_t PK_G  = 8;
const int16_t PK_X0 = (LCD_WIDTH - (3*PK_W + 2*PK_G)) / 2;
const int16_t PK_Y0 = 152;

// Vertical offset for the slide-up entrance animation (0 in normal use).
int16_t pinYOffset = 0;

int16_t pkX(uint8_t c) { return PK_X0 + c * (PK_W + PK_G); }
int16_t pkY(uint8_t r) { return PK_Y0 + pinYOffset + r * (PK_H + PK_G); }

void drawPinInputBox() {
  // Clean header + four dots — no boxy outline.
  int16_t y = STATUS_H + 16 + pinYOffset;
  textCenter(y, "Enter PIN", 3, C_WHITE);

  int16_t cx = LCD_WIDTH / 2;
  int16_t cy = y + 58;
  int16_t shake = (millis() < shakeUntil)
                  ? ((millis() / 40) % 2 ? -8 : 8) : 0;

  for (int i = 0; i < 4; i++) {
    int16_t x = cx - 66 + i * 44 + shake;
    if (i < pinLen) {
      // Filled dot — red while the wrong-PIN shake plays, blue otherwise
      uint16_t col = (millis() < shakeUntil) ? C_RED : C_BLUE;
      gfx->fillCircle(x, cy, 10, col);
    } else {
      gfx->drawCircle(x, cy, 10, C_GRAY_3);
      gfx->drawCircle(x, cy,  9, C_GRAY_3);
    }
  }
}

void drawPinKey(uint8_t i, bool pressed) {
  int16_t x = pkX(PKEYS[i].col), y = pkY(PKEYS[i].row);
  bool isOK = (strcmp(PKEYS[i].label, "OK") == 0);
  bool isBS = (strcmp(PKEYS[i].label, "BS") == 0);

  // Phone-dialer style circular keys (tap zones stay the full cell).
  int16_t cx = x + PK_W/2, cy = y + PK_H/2;
  int16_t r  = PK_H/2 - 1;                       // 29 px

  uint16_t bg, fg;
  if (pressed)   { bg = C_BLUE;   fg = C_BLACK; }
  else if (isOK) { bg = C_BLUE;   fg = C_BLACK; }
  else if (isBS) { bg = C_BLACK;  fg = C_GRAY_5; }
  else           { bg = C_GRAY_1; fg = C_WHITE; }

  if (isBS && !pressed) {
    // Backspace: no circle, just the glyph — keeps the pad airy
  } else {
    gfx->fillCircle(cx, cy, r, bg);
    if (!pressed && !isOK) gfx->drawCircle(cx, cy, r, C_GRAY_2);
  }

  if (isBS) {
    gfx->fillTriangle(cx - 18, cy, cx - 6, cy - 11, cx - 6, cy + 11, fg);
    gfx->fillRect(cx - 6, cy - 7, 18, 14, fg);
    uint16_t xc = pressed ? C_WHITE : C_BLACK;
    gfx->drawLine(cx - 2, cy - 5, cx + 8, cy + 5, xc);
    gfx->drawLine(cx - 2, cy + 5, cx + 8, cy - 5, xc);
    gfx->drawLine(cx - 1, cy - 5, cx + 8, cy + 4, xc);
    gfx->drawLine(cx - 1, cy + 5, cx + 8, cy - 4, xc);
  } else if (isOK) {
    for (int t = 0; t < 4; t++) {
      gfx->drawLine(cx - 14, cy + t - 2,  cx - 4,  cy + 8 + t, fg);
      gfx->drawLine(cx - 4,  cy + 8 + t,  cx + 15, cy - 9 + t, fg);
    }
  } else {
    gfx->setTextSize(3); gfx->setTextColor(fg);
    int16_t x1, y1; uint16_t w, h;
    gfx->getTextBounds(PKEYS[i].label, 0, 0, &x1, &y1, &w, &h);
    gfx->setCursor(cx - (int16_t)w / 2, cy - (int16_t)h / 2);
    gfx->print(PKEYS[i].label);
  }
}

void drawPin() {
  gfx->fillScreen(C_BLACK);
  drawStatusBar();

  // Locked out after too many wrong attempts — show a countdown and hide
  // the keypad until the wait expires.
  uint32_t lock = pinLockRemaining();
  if (lock > 0) {
    drawPinInputBox();
    textCenter(STATUS_H + 150, "Too many attempts", 2, C_WHITE);
    char b[32];
    snprintf(b, sizeof(b), "Wait %lus to retry", (unsigned long)lock);
    textCenter(STATUS_H + 186, b, 2, C_GRAY_4);
    flushScreen();
    return;
  }

  drawPinInputBox();

  // Show how many tries remain before the next lockout (like a phone PIN).
  // 0,1,2 fails are "free"; the 3rd locks for 30 s. After a lockout, each
  // further wrong try locks again immediately (1 left).
  if (pinFails > 0) {
    uint8_t left = (pinFails < 3) ? (uint8_t)(3 - pinFails) : 1;
    char ab[28];
    snprintf(ab, sizeof(ab), "%u attempt%s left", left, left == 1 ? "" : "s");
    textCenter(STATUS_H + 100 + pinYOffset, ab, 1, left <= 1 ? C_RED : C_GRAY_4);
  }

  for (uint8_t i = 0; i < 12; i++) drawPinKey(i, false);
  flushScreen();
}

// Apple-Watch-style slide-up entrance: the keypad + entry box rise from the
// bottom into place while the status bar stays put.
//
//   • NO per-frame delay — the canvas flush (~12 ms) already paces it; the
//     old delay(12) just made it drag.
//   • Cubic EASE-OUT — shoots up fast then settles, so it feels snappy
//     ("fata-fat") instead of a slow constant crawl.
void pinSlideIn() {
  const int     STEPS = 8;
  const int16_t START = 300;                 // keypad starts just off the bottom
  for (int i = 1; i <= STEPS; i++) {
    float t = (float)i / STEPS;              // 0 → 1
    float u = 1.0f - t;
    float ease = 1.0f - u * u * u;           // cubic ease-out (fast, then gentle)
    pinYOffset = (int16_t)(START * (1.0f - ease));
    drawPin();
  }
  pinYOffset = 0;
  drawPin();
}

void onTapPin(int16_t tx, int16_t ty) {
  // Frozen during a lockout — ignore key taps until the wait expires.
  if (pinLockRemaining() > 0) { drawPin(); return; }

  for (uint8_t i = 0; i < 12; i++) {
    int16_t x = pkX(PKEYS[i].col), y = pkY(PKEYS[i].row);
    if (tx < x || tx >= x + PK_W) continue;
    if (ty < y || ty >= y + PK_H) continue;

    drawPinKey(i, true); flushScreen();
    delay(60);

    const char *k = PKEYS[i].label;
    if (strcmp(k, "BS") == 0) {
      if (pinLen) pinLen--;
    } else if (strcmp(k, "OK") == 0) {
      pinEntry[pinLen] = 0;
      if (strcmp(pinEntry, settings.pin) == 0) {
        pinLen = 0;
        pinRegisterSuccess();
        ledSet(0x00FF00, 200);
        // Skip the "UNLOCKED" splash — straight to HOME for snappy feel
        fadeOut();
        navTop = 0;
        navStack[0] = SCR_HOME;
        current = SCR_HOME;
        drawAll();
        fadeIn();
        return;
      } else {
        pinRegisterFail();
        shakeUntil = millis() + 500;
        pinLen = 0;
        ledSet(0xFF0000, 400);
      }
    } else if (pinLen < 4) {
      pinEntry[pinLen++] = k[0];
      if (pinLen == 4) {
        pinEntry[4] = 0;
        if (strcmp(pinEntry, settings.pin) == 0) {
          pinLen = 0;
          pinRegisterSuccess();
          ledSet(0x00FF00, 200);
          fadeOut();
          navTop = 0;
          navStack[0] = SCR_HOME;
          current = SCR_HOME;
          drawAll();
          fadeIn();
          return;
        } else {
          pinRegisterFail();
          shakeUntil = millis() + 500;
          pinLen = 0;
          ledSet(0xFF0000, 400);
        }
      }
    }
    drawPin();
    return;
  }
}
