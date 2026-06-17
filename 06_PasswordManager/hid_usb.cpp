// =============================================================
//  hid_usb.cpp  —  Isolated USB-HID wrapper
//
//  USBHIDKeyboard.h and BleKeyboard.h both define the same
//  KEY_xxx macros and a KeyReport struct, so they can't live in
//  the same translation unit.  This .cpp file owns the USB side;
//  hid_ble.cpp owns the BLE side.  Each exposes a tiny extern
//  "C++" surface used by 06_PasswordManager.ino's typeViaHID().
//
//  To disable USB HID at compile time (no USB headers pulled in),
//  set HID_USB_ENABLE to 0 below.
// =============================================================
#define HID_USB_ENABLE 1

#include <Arduino.h>

#if HID_USB_ENABLE
  #include <USB.h>
  #include <USBHIDKeyboard.h>
  static USBHIDKeyboard kb;
  static bool started = false;
  extern "C" bool tud_mounted(void);

  // FIX: Wait up to this many ms for host enumeration before giving up.
  // Windows needs 500–1500 ms after USB.begin() to enumerate HID.
  // 300 ms (old value) was too short — tud_mounted() returned 0 every
  // time typeViaHID() was called, so typing was silently skipped.
  static const uint32_t MOUNT_WAIT_MS = 3000;

  static bool usbAndroidFix = false;   // UK/Android layout: swap @ <-> " keycodes

  // Type one character over USB. Mirrors hid_ble.cpp's bleType(): on a
  // UK/Android-layout host (Shift+2 = "), "Android @ Fix" sends @ as Shift+'
  // and " as Shift+2 so they come out right. The library's US asciimap is fine
  // for everything else; the host applies its own layout.
  static void usbType(char c) {
    if ((uint8_t)c >= 128) return;     // skip extended bytes (match BLE path)
    if (usbAndroidFix && c == '@') {
      kb.press(KEY_LEFT_SHIFT); kb.press('\'');   // @ on UK/Android
      delay(6);
      kb.releaseAll();
    } else if (usbAndroidFix && c == '"') {
      kb.press(KEY_LEFT_SHIFT); kb.press('2');     // " on UK/Android
      delay(6);
      kb.releaseAll();
    } else {
      kb.write((uint8_t)c);
    }
  }
#endif

extern "C" {

void hidUsbBegin() {
#if HID_USB_ENABLE
  if (!started) {
    // Exactly the pattern from the board's proven 04_USB_HID example:
    // Keyboard.begin() then USB.begin() — works with USB-OTG (TinyUSB) +
    // USB CDC On Boot = Enabled. No re-enumeration hack needed.
    Serial.println("[USB] Keyboard.begin() + USB.begin()");
    kb.begin();
    USB.begin();
    started = true;
    // Short delay here only — full mount wait happens in hidUsbPrint()
    // where we can actually report progress.  Setup() stays fast.
    delay(300);
    Serial.printf("[USB] HID started  mounted=%d\n", tud_mounted() ? 1 : 0);
  }
#endif
}

void hidUsbPrint(const char *s) {
#if HID_USB_ENABLE
  Serial.printf("[USB] hidUsbPrint('%s')  started=%d\n", s, started);
  if (!started) {
    Serial.println("[USB]   not started — calling begin()");
    hidUsbBegin();
  }

  // KEY FIX: Wait for host to fully enumerate us before typing.
  // tud_mounted() is 0 for up to 1500ms on Windows after USB.begin().
  // The old code had no wait — keys were dropped every time.
  if (!tud_mounted()) {
    Serial.printf("[USB]   not mounted yet — waiting up to %ums...\n", MOUNT_WAIT_MS);
    uint32_t t = millis();
    while (!tud_mounted() && (millis() - t) < MOUNT_WAIT_MS) {
      delay(50);
    }
    if (!tud_mounted()) {
      Serial.println("[USB]   ERROR: host never mounted. Check:");
      Serial.println("[USB]     1. Tools -> USB Mode = USB-OTG (TinyUSB)");
      Serial.println("[USB]     2. USB CDC On Boot = Enabled");
      Serial.println("[USB]     3. Data cable (not charge-only)");
      return;
    }
    Serial.printf("[USB]   mounted after %ums\n", millis() - t);
  }

  // Per-char so the Android @ fix can swap @ / " when enabled (a plain
  // kb.print(s) sends raw US scancodes with no swap).
  Serial.printf("[USB]   typing %d chars...\n", (int)strlen(s));
  for (const char *p = s; *p; p++) usbType(*p);
  Serial.println("[USB]   done");
#else
  (void)s;
#endif
}

// One-tap login: type username, Tab, password, Enter — fills a whole
// login form in a single action.
void hidUsbQuickFill(const char *user, const char *pass) {
#if HID_USB_ENABLE
  if (!started) { hidUsbBegin(); }

  // Same mount wait as hidUsbPrint
  if (!tud_mounted()) {
    uint32_t t = millis();
    while (!tud_mounted() && (millis() - t) < MOUNT_WAIT_MS) {
      delay(50);
    }
    if (!tud_mounted()) {
      Serial.println("[USB] quickFill: host not mounted — aborted");
      return;
    }
  }

  // Per-char (via usbType) so the Android @ fix applies to user + pass too.
  Serial.printf("[USB] quickFill user='%s'\n", user);
  for (const char *p = user; *p; p++) usbType(*p);
  delay(80);
  kb.write(KEY_TAB);
  delay(80);
  for (const char *p = pass; *p; p++) usbType(*p);
  delay(80);
  kb.write(KEY_RETURN);
  Serial.println("[USB]   quickFill done");
#else
  (void)user; (void)pass;
#endif
}

// Toggle the UK/Android @ <-> " keycode swap on the USB path (called from the
// Settings "Android" toggle and once at boot from loadSettings()).
void hidUsbSetAndroidFix(int on) {
#if HID_USB_ENABLE
  usbAndroidFix = on ? true : false;
#else
  (void)on;
#endif
}

int hidUsbCompiled() {
#if HID_USB_ENABLE
  return 1;
#else
  return 0;
#endif
}

// 1 once a USB host has enumerated us (cable in a data port + descriptors OK).
// If this stays 0 with USB plugged in, the host can't see the HID keyboard —
// that's a USB-Mode / CDC-On-Boot / cable issue, not a routing bug.
int hidUsbMounted() {
#if HID_USB_ENABLE
  if (!started) hidUsbBegin();
  return tud_mounted() ? 1 : 0;
#else
  return 0;
#endif
}

// Send a bare Return keystroke over USB.
void hidUsbReturn() {
#if HID_USB_ENABLE
  if (!started || !tud_mounted()) return;
  delay(60);
  kb.write(KEY_RETURN);
#else
#endif
}

}  // extern "C"