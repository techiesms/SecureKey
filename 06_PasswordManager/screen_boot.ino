// =============================================================
//  screen_boot.ino  —  Power-on logo splash (animated)
//
//  Boot sequence, shown ONCE from setup() at power-on / restart
//  only (normal navigation never returns here):
//    1. black     — the panel sits completely black for a beat
//    2. fade in   — the logo fades in slowly & gently (logo_img.h,
//                   RGB565 from the BMP)
//    3. booting   — three dots cycle under the logo
//    4. fade out  — power-down, then the UI comes up
//
//  Fades use the panel BRIGHTNESS (hardware, smooth): the bg is
//  pure-black AMOLED (pixels off), so ramping brightness makes
//  only the lit logo glow.
// =============================================================

#include "logo_img.h"

#define BOOT_FADE_STEPS  24
#define BOOT_FADE_DELAY  15

static const uint16_t LOGO_BLUE = 0x461E;   // RGB565 of #47c1f2

// Draw the logo centered on black, then flush. Brightness (set by the caller)
// controls whether it's visible yet.
static void drawBootLogoFull() {
  int16_t x = (LCD_WIDTH  - LOGO_W) / 2;     // 34
  int16_t y = (LCD_HEIGHT - LOGO_H) / 2;     // 74
  gfx->fillScreen(C_BLACK);
  gfx->draw16bitRGBBitmap(x, y, (uint16_t *)LOGO_IMG, LOGO_W, LOGO_H);
  flushScreen();
}

// 1+2: hold completely black for a beat, then fade the logo in slowly.
void bootRevealIn() {
  const int target = settings.brightness;
  out->Display_Brightness(0);
  drawBootLogoFull();            // paint the logo while dark (invisible yet)
  delay(350);                    // completely-black beat
  const int STEPS = 44;
  for (int i = 0; i <= STEPS; i++) {
    out->Display_Brightness((uint8_t)(target * i / STEPS));
    delay(22);                   // ~1 s — slow & gentle "dheeme dheeme"
  }
  out->Display_Brightness((uint8_t)target);
}

// 3: "booting" dots under the logo, cycling for durMs. Only the strip below
// the logo is redrawn, so the held logo above stays put.
void bootDots(uint32_t durMs) {
  const int16_t cx  = LCD_WIDTH / 2;
  const int16_t dy  = (LCD_HEIGHT - LOGO_H) / 2 + LOGO_H + 30;   // below logo
  const int16_t r   = 5, gap = 24;
  uint32_t start = millis();
  int frame = 0;
  while (millis() - start < durMs) {
    gfx->fillRect(cx - 3*gap, dy - r - 3, 6*gap, 2*r + 6, C_BLACK);
    int active = (frame / 5) % 3;
    for (int d = 0; d < 3; d++) {
      int16_t dxp = cx + (d - 1) * gap;
      gfx->fillCircle(dxp, dy, r, (d == active) ? LOGO_BLUE : C_GRAY_3);
    }
    flushScreen();
    delay(70);
    frame++;
  }
}

// 4: ramp brightness target → 0 (power-down fade).
void bootFadeOut() {
  const int target = settings.brightness;
  for (int i = BOOT_FADE_STEPS; i >= 0; i--) {
    out->Display_Brightness((uint8_t)(target * i / BOOT_FADE_STEPS));
    delay(BOOT_FADE_DELAY);
  }
}
