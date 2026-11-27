// =============================================================
//  touch_input.ino  —  CST820 capacitive touch + state machine
//  Extended from 05_UI_Mockup to add live drag callback.
// =============================================================

uint8_t ftReadReg(uint8_t r) {
  Wire.beginTransmission((uint8_t)CST820_ADDR);
  Wire.write(r);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)CST820_ADDR, (uint8_t)1);
  return Wire.available() ? Wire.read() : 0;
}

bool ftReadTouch(uint16_t &x, uint16_t &y) {
  uint8_t n = ftReadReg(0x02);
  if (n == 0 || n > 5) return false;
  Wire.beginTransmission((uint8_t)CST820_ADDR);
  Wire.write(0x03);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)CST820_ADDR, (uint8_t)4);
  if (Wire.available() < 4) return false;
  uint8_t xh = Wire.read(), xl = Wire.read();
  uint8_t yh = Wire.read(), yl = Wire.read();
  x = ((xh & 0x0F) << 8) | xl;
  y = ((yh & 0x0F) << 8) | yl;
  // Garbage guard: a glitched read can report a "touch" outside the panel
  // (raw range goes to 4095; the panel is 368×448). Those phantom points
  // flipped Settings toggles and fired gestures by themselves — reject them.
  if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return false;
  return true;
}

void pollTouch() {
  static bool longFired = false;   // long-press already handled this touch?

  // Self-heal: if the FT3168 stops ACKing its address (wedged by a boot race
  // or a supply dip), pulse TP_RST to re-init it instead of staying deaf until
  // a power-cycle. One extra address byte per poll (~25 µs) when healthy.
  static uint8_t  ftDead = 0;
  static uint32_t ftQuietUntil = 0;
  Wire.beginTransmission((uint8_t)CST820_ADDR);
  if (Wire.endTransmission() != 0) {
    if (++ftDead >= 25) {                    // ~0.4 s of consecutive NACKs
      Serial.println("[TOUCH] FT3168 not ACKing — reset pulse");
      ioxSet(IOX_TP_RST, false);  delay(5);
      ioxSet(IOX_TP_RST, true); delay(120);
      ftDead = 0;
      // The controller recalibrates its capacitive baseline after a reset —
      // reads during that window are garbage (ghost touches). Stay quiet.
      ftQuietUntil = millis() + 400;
    }
    return;                                  // bus not answering this poll
  }
  ftDead = 0;
  if (millis() < ftQuietUntil) return;       // settling after a reset pulse

  // ── Ghost-touch debounce ──────────────────────────────────
  // A touch must be present on 2 CONSECUTIVE polls to start, and absent on
  // 2 consecutive polls to end. A single glitched poll (I2C noise, radio
  // burst, controller hiccup) used to become a full phantom tap — flipping
  // Settings toggles, cycling Auto-Lock, firing gestures "by itself".
  // Costs one poll (~16 ms) of latency; a real finger is far slower.
  static uint8_t seenDown = 0, seenUp = 0;
  uint16_t x = 0, y = 0;
  bool raw = ftReadTouch(x, y);
  if (raw) { seenUp = 0; if (seenDown < 2) seenDown++; }
  else     { seenDown = 0; if (seenUp   < 2) seenUp++;  }
  bool active = touching ? (seenUp < 2) : (seenDown >= 2);

  if (active && touching && !raw) return;    // debounce gap — no fresh coords

  if (active && !touching) {
    // ── Touch start ──────────────────────────────────────
    touching   = true;
    isDrag     = false;
    longFired  = false;
    tStartX    = tCurX = x;
    tStartY    = tCurY = y;
    tStartTime = millis();
    listVelocity = 0;   // kill inertia on new touch
    lastActivityMs = millis();   // refresh idle timer for auto-lock
  }
  else if (active && touching) {
    // ── Touch continues ──────────────────────────────────
    int16_t dx = (int16_t)x - (int16_t)tCurX;
    int16_t dy = (int16_t)y - (int16_t)tCurY;
    int16_t totalDx = (int16_t)x - (int16_t)tStartX;
    int16_t totalDy = (int16_t)y - (int16_t)tStartY;

    if (!isDrag && (abs(totalDx) > DRAG_THRESHOLD ||
                   abs(totalDy) > DRAG_THRESHOLD)) {
      isDrag = true;
    }

    // Long-press on the Home screen (stationary hold) → pick up a tile and
    // enter drag-to-reorder mode.
    if (!isDrag && !longFired && current == SCR_HOME &&
        (millis() - tStartTime) > 500) {
      longFired = true;
      homeLongPress((int16_t)tStartX, (int16_t)tStartY);
    }

    if (current == SCR_HOME && homeIsDragging()) {
      homeDrag(x, y);                 // a tile is following the finger
    } else if (isDrag) {
      onDrag(tCurX, tCurY, dx, dy);   // ← live scroll callback
    }
    tCurX = x;
    tCurY = y;
  }
  else if (!active && touching) {
    // ── Touch released ────────────────────────────────────
    touching = false;
    seenDown = 0; seenUp = 0;      // clean slate for the next touch
    int16_t totalDx = (int16_t)tCurX - (int16_t)tStartX;
    int16_t totalDy = (int16_t)tCurY - (int16_t)tStartY;

    if (current == SCR_HOME && homeIsDragging()) {
      homeRelease((int16_t)tCurX, (int16_t)tCurY);   // drop the tile
    } else if (isDrag) {
      onSwipeEnd(totalDx, totalDy);
    } else if ((millis() - tStartTime) < 600) {
      dispatchTap((int16_t)tStartX, (int16_t)tStartY);
    }
  }
}
