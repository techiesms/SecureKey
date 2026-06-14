// =============================================================
//  screen_detail.ino  —  Password detail (monochrome)
//
//  Five rows: Title, User, Pass, URL, Note.
//  Tap any row → types that field via USB or BLE HID (if
//  enabled in Settings).  Otherwise shows a "Enable HID" toast.
// =============================================================

static PassRecord detailRec;

// Rows stay full-size (54). Only the gaps between them were tightened (6→3)
// so the bottom action bar clears the AMOLED's rounded corner with a proper
// 16px margin — without shrinking the row content.
#define DET_ROW_H   54
#define DET_TOP     (STATUS_H + NAV_H + 8)
#define ROW_Y(n)    (DET_TOP + (n) * (DET_ROW_H + 3))

static void drawDetailActions();   // bottom Edit | Fav | Delete bar (below)

static void drawDetailRow(int16_t y, const char *label,
                          const char *value, bool isPass) {
  int16_t x = SAFE_PAD;
  int16_t w = LCD_WIDTH - 2*SAFE_PAD;

  // Box
  gfx->fillRoundRect(x, y, w, DET_ROW_H, 10, C_GRAY_1);
  gfx->drawRoundRect(x, y, w, DET_ROW_H, 10, C_GRAY_2);

  // Left accent bar tinted by this entry's avatar colour — ties the
  // detail screen to its list row identity.
  gfx->fillRoundRect(x + 6, y + 10, 3, DET_ROW_H - 20, 1,
                     avatarColor(detailRec.title[0]));

  // Label (top-left, small caps)
  gfx->setTextSize(1); gfx->setTextColor(C_GRAY_3);
  gfx->setCursor(x + 14, y + 8);
  gfx->print(label);

  // Value (bottom, larger)
  gfx->setTextSize(2); gfx->setTextColor(C_WHITE);
  if (isPass) {
    // Always show as filled dots — password is never displayed in plain text
    int n = min((int)strlen(value), 10);
    int16_t dotX = x + 16;
    int16_t dotY = y + DET_ROW_H - 18;
    for (int i = 0; i < n; i++)
      gfx->fillCircle(dotX + i * 17, dotY, 5, C_WHITE);
  } else {
    textClipped(x + 14, y + 26, value[0] ? value : "-",
                2, C_WHITE, w - 62);
  }

  // Right action hint
  gfx->setTextSize(1); gfx->setTextColor(C_BLUE);
  gfx->setCursor(x + w - 38, y + DET_ROW_H/2 - 4);
  gfx->print("TYPE");
}

void detailInit() { /* nothing to reset */ }

void drawDetail() {
  if (!dbLoadRecord(detailId, detailRec)) {
    gfx->fillScreen(C_BLACK);
    drawStatusBar();
    textCenter(LCD_HEIGHT/2, "Not found", 2, C_GRAY_3, LCD_WIDTH/2);
    flushScreen();
    return;
  }

  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  drawNavBar("Password", true, nullptr);   // actions moved to the bottom bar

  // 5 rows
  const char *labels[5]  = { "TITLE", "USER", "PASS", "URL", "NOTE" };
  const char *values[5]  = { detailRec.title, detailRec.username,
                             detailRec.password, detailRec.url,
                             detailRec.note };
  for (int i = 0; i < 5; i++) {
    drawDetailRow(ROW_Y(i), labels[i], values[i], i == 2);
  }

  // Bottom action bar — three equal buttons: EDIT | FAVORITE | DELETE.
  // (Tap any field row above to TYPE it via HID.)
  drawDetailActions();

  flushScreen();
}

// ── Bottom action bar geometry (shared by draw + tap) ────────
//  The panel's rounded corners are ~30-32px (see 01_HelloAMOLED). The bottom
//  action bar sits below the last row with a 20px bottom inset so the
//  EDIT/DELETE outer-bottom corners stay clear of that curve — without
//  shrinking the rows. ACT_Y = 388, ACT_H = 40 → bottom = 428 (448 - 20).
#define ACT_Y    (ROW_Y(4) + DET_ROW_H + 6)
#define ACT_H    40
#define ACT_GAP  10
#define ACT_W    ((LCD_WIDTH - 2*SAFE_PAD - 2*ACT_GAP) / 3)   // 105
static inline int16_t actX(uint8_t i) { return SAFE_PAD + i * (ACT_W + ACT_GAP); }

static void drawDetailActions() {
  bool fav = detailRec.favorite;
  int16_t cy = ACT_Y + ACT_H/2;

  // EDIT — text only (blue)
  int16_t ex = actX(0);
  gfx->fillRoundRect(ex, ACT_Y, ACT_W, ACT_H, 10, C_GRAY_1);
  gfx->drawRoundRect(ex, ACT_Y, ACT_W, ACT_H, 10, C_BLUE);
  textCenter(cy - 7, "EDIT", 2, C_BLUE, ex + ACT_W/2);

  // FAVORITE — heart only (red when on, no label)
  int16_t fx = actX(1);
  gfx->fillRoundRect(fx, ACT_Y, ACT_W, ACT_H, 10, C_GRAY_1);
  gfx->drawRoundRect(fx, ACT_Y, ACT_W, ACT_H, 10, fav ? C_RED : C_GRAY_3);
  drawHeart(fx + ACT_W/2, cy, 13, fav, fav ? C_RED : C_GRAY_4, C_GRAY_1);

  // DELETE — text only (red)
  int16_t dx = actX(2);
  gfx->fillRoundRect(dx, ACT_Y, ACT_W, ACT_H, 10, C_GRAY_1);
  gfx->drawRoundRect(dx, ACT_Y, ACT_W, ACT_H, 10, C_RED);
  textCenter(cy - 7, "DELETE", 2, C_RED, dx + ACT_W/2);
}

// ── Confirm-and-delete modal ────────────────────────────────
static void confirmAndDelete() {
  // Two-tap confirmation: "DELETE?" screen with two big buttons
  gfx->fillScreen(C_BLACK);
  drawStatusBar();
  textCenter(STATUS_H + 30, "DELETE?", 4, C_WHITE, LCD_WIDTH/2);

  // Show title of the entry being deleted
  textCenter(STATUS_H + 90, detailRec.title, 2, C_GRAY_5, LCD_WIDTH/2);
  textCenter(STATUS_H + 116, "This cannot be undone.", 1, C_GRAY_3, LCD_WIDTH/2);

  // Cancel (left) and Delete (right) buttons
  const int16_t bw = 130, bh = 56;
  const int16_t gap = 14;
  const int16_t y = LCD_HEIGHT - bh - 36;
  const int16_t cx = LCD_WIDTH / 2;
  int16_t cancelX = cx - bw - gap/2;
  int16_t deleteX = cx + gap/2;

  // Cancel — grey
  gfx->fillRoundRect(cancelX, y, bw, bh, 12, C_GRAY_1);
  gfx->drawRoundRect(cancelX, y, bw, bh, 12, C_GRAY_3);
  textCenter(y + bh/2 - 8, "CANCEL", 2, C_WHITE, cancelX + bw/2);

  // Delete — white background, black text (high contrast warning)
  gfx->fillRoundRect(deleteX, y, bw, bh, 12, C_WHITE);
  textCenter(y + bh/2 - 8, "DELETE", 2, C_BLACK, deleteX + bw/2);

  flushScreen();

  // Block until a button is tapped
  uint32_t startWait = millis();
  while (millis() - startWait < 8000) {
    delay(20);
    pollTouch();
    if (touching) {
      uint16_t tx = tCurX, ty = tCurY;
      // Wait for release
      while (touching) { delay(20); pollTouch(); }

      if (ty >= y && ty <= y + bh) {
        if (tx >= cancelX && tx <= cancelX + bw) {
          // Cancel — return
          drawDetail();
          return;
        }
        if (tx >= deleteX && tx <= deleteX + bw) {
          // Delete!
          dbDelete(detailId);
          dbLoadIndex();
          ledSet(0xFF0000, 300);
          // Confirmation
          gfx->fillScreen(C_BLACK);
          drawStatusBar();
          textCenter(LCD_HEIGHT/2 - 12, "DELETED", 4, C_WHITE, LCD_WIDTH/2);
          flushScreen();
          delay(700);
          // Go back to list
          popNav();
          // popNav already calls buildList for LIST screen
          return;
        }
      }
      // Any other tap → re-poll
      delay(50);
    }
  }
  // Timeout — back to detail
  drawDetail();
}

void onTapDetail(int16_t tx, int16_t ty) {
  // Back (top-left)
  if (ty >= STATUS_H + 2 && ty < STATUS_H + NAV_H - 2
      && tx >= SAFE_PAD && tx < SAFE_PAD + 46) {
    popNav(); return;
  }

  // Bottom action bar — EDIT | FAVORITE | DELETE
  if (ty >= ACT_Y && ty < ACT_Y + ACT_H) {
    if (tx >= actX(0) && tx < actX(0) + ACT_W) {        // EDIT
      editingId = detailId;
      addEditInit(detailRec);
      pushNav(SCR_ADD);
      return;
    }
    if (tx >= actX(1) && tx < actX(1) + ACT_W) {        // FAVORITE
      dbToggleFavorite(detailId);
      dbLoadRecord(detailId, detailRec);  // refresh local copy (favorite flag)
      drawDetail();
      return;
    }
    if (tx >= actX(2) && tx < actX(2) + ACT_W) {        // DELETE
      confirmAndDelete();
      return;
    }
    return;
  }

  // Find which row was tapped
  for (int i = 0; i < 5; i++) {
    int16_t y = ROW_Y(i);
    if (ty < y || ty >= y + DET_ROW_H) continue;

    // Press flash
    gfx->fillRoundRect(SAFE_PAD, y, LCD_WIDTH - 2*SAFE_PAD,
                       DET_ROW_H, 8, C_GRAY_2);
    flushScreen(); delay(60);

    const char *val = nullptr;
    switch (i) {
      case 0: val = detailRec.title;    break;
      case 1: val = detailRec.username; break;
      case 2: val = detailRec.password; break;   // types directly — never revealed
      case 3: val = detailRec.url;      break;
      case 4: val = detailRec.note;     break;
    }

    extern void typeReturnViaHID();
    if (val && val[0]) { typeViaHID(val); typeReturnViaHID(); }
    drawDetail();
    return;
  }
}
