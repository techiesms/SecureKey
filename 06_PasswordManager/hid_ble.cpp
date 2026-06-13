// =============================================================
//  hid_ble.cpp  —  Isolated BLE-HID wrapper
//
//  Pairs hid_usb.cpp.  Owns the BleKeyboard library (T-vK's
//  "ESP32 BLE Keyboard", configured here for the NimBLE backend so
//  it coexists with native USB HID on the ESP32-S3).  Lives in its
//  own translation unit so its KEY_xxx macros can't collide with
//  USBHIDKeyboard's.
//
//  Disable here if the library isn't installed:
//      set HID_BLE_ENABLE 0
// =============================================================
#define HID_BLE_ENABLE 1

#include <Arduino.h>

#if HID_BLE_ENABLE
  #include <BleKeyboard.h>
  #include <NimBLEDevice.h>
  static BleKeyboard kb("SecureKey", "techiesms", 100);
  static bool everStarted = false;   // kb.begin() may only EVER run once/boot
  static bool active      = false;   // advertising / accepting connections?
  static bool androidFix  = false;   // UK/Android layout: swap @ <-> " keycodes

  // BLE HID timing. The KEY fix for "stuck / repeating keys": a key-down and
  // its key-up MUST land in DIFFERENT BLE connection events, otherwise the
  // host can miss the release and auto-repeat the last key forever (the
  // "...gmmmmm" bug). kb.write() sends press+release back-to-back (same
  // event), so we DON'T use it — we press, hold past one connection interval,
  // then send a clean "all keys up" report, then a small gap before the next.
  // Key-down must be held LONGER than the BLE connection interval so the
  // key-down and key-up reports land in different connection events — else the
  // host misses the release and auto-repeats (the Windows "likkkke" bug). 45ms
  // safely spans the typical 15-48ms interval even if the tighter interval we
  // request at connect-time gets rejected.
  static const uint16_t BLE_HOLD_MS  = 45;   // key held
  static const uint16_t BLE_GAP_MS   = 25;   // quiet gap so each report is delivered
  static const uint16_t BLE_SETTLE_MS = 240; // wait before the 1st key (host subscribe)
  static bool everConn = false;              // have we ever been connected this link?

  // Type one character. In "Android fix" mode, '@' and '"' are sent as the
  // raw keycodes that produce them on a UK/Android host (where Shift+2 = "
  // and Shift+' = @), instead of the US asciimap (Shift+2 = @). This is the
  // classic US<->UK swap that makes @ come out as " on many Android phones.
  static void bleType(char c) {
    if (androidFix && c == '@') {
      kb.press(KEY_LEFT_SHIFT); kb.press('\'');   // @ on UK/Android
    } else if (androidFix && c == '"') {
      kb.press(KEY_LEFT_SHIFT); kb.press('2');     // " on UK/Android
    } else {
      kb.press((uint8_t)c);
    }
    delay(BLE_HOLD_MS);
    kb.releaseAll();                                // clean key-up report
    delay(BLE_GAP_MS);
  }

  // Same press/hold/release discipline for a raw key (Tab, Enter, ...).
  static void bleRawKey(uint8_t k) {
    kb.press(k);
    delay(BLE_HOLD_MS);
    kb.releaseAll();
    delay(BLE_GAP_MS);
  }
#endif

extern "C" {

// Re-arm the connection settle delay (called by the main loop when it sees a
// peer disconnect, so the next connection waits for a fresh HID subscription).
void hidBleSettleReset() {
#if HID_BLE_ENABLE
  everConn = false;
#endif
}

// Request a tight connection interval. Called ONCE by the main loop when a new
// peer connects — NOT during typing (renegotiating mid-stream drops reports).
// By the time the user accepts and taps a field, the interval has settled, so
// our key-hold reliably spans it and typing is prompt.
void hidBleTune() {
#if HID_BLE_ENABLE
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (!srv) return;
  std::vector<uint16_t> peers = srv->getPeerDevices();
  if (peers.empty()) return;
  // min 15ms, max 22.5ms, latency 0, supervision timeout 4s
  srv->updateConnParams(peers[0], 12, 18, 0, 400);
#endif
}

// Toggle the UK/Android @ <-> " keycode swap (called when the Settings
// "Android @ Fix" toggle changes, and once at boot from loadSettings()).
void hidBleSetAndroidFix(int on) {
#if HID_BLE_ENABLE
  androidFix = on ? true : false;
#else
  (void)on;
#endif
}

// IMPORTANT: this library's BleKeyboard::end() is EMPTY, and calling
// kb.begin() twice duplicates the HID GATT services (breaks pairing).
// So: kb.begin() exactly once per boot; after that, on/off is done with
// the real NimBLE radio calls — stop/start advertising + drop peers.
void hidBleBegin() {
#if HID_BLE_ENABLE
  if (!everStarted) {
    Serial.println("[BLE] kb.begin() (first start)");
    kb.setDelay(5);     // 5 ms between key reports (library internal)
    kb.begin();
    everStarted = true;
    active = true;
    Serial.println("[BLE] advertising as 'SecureKey'");
  } else if (!active) {
    Serial.println("[BLE] resume advertising");
    NimBLEDevice::getAdvertising()->start();
    active = true;
  }
#endif
}

void hidBleEnd() {
#if HID_BLE_ENABLE
  everConn = false;                  // next connection re-arms the settle wait
  if (!everStarted || !active) { active = false; return; }
  Serial.println("[BLE] stop: drop peers + stop advertising");
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (srv) {
    std::vector<uint16_t> peers = srv->getPeerDevices();
    for (uint16_t id : peers) srv->disconnect(id);
    if (!peers.empty()) delay(400);  // let onDisconnect fire (it restarts adv)
  }
  NimBLEDevice::getAdvertising()->stop();
  active = false;
#endif
}

int hidBleConnected() {
#if HID_BLE_ENABLE
  return everStarted && active && kb.isConnected() ? 1 : 0;
#else
  return 0;
#endif
}

void hidBlePrint(const char *s) {
#if HID_BLE_ENABLE
  Serial.printf("[BLE] hidBlePrint('%s')  active=%d  connected=%d\n",
                s, active, everStarted ? (int)kb.isConnected() : 0);
  if (!active) { Serial.println("[BLE]   not active — begin()"); hidBleBegin(); delay(500); }
  if (!kb.isConnected()) {
    Serial.println("[BLE]   isConnected()=false — aborting print");
    return;
  }
  // Settle: the FIRST type after a fresh connection must wait for the host to
  // finish subscribing to our HID input report — otherwise the opening keys
  // are dropped (Mac types nothing / only the start of the string lands).
  // Later types on the same link skip the long wait.
  delay(everConn ? 60 : BLE_SETTLE_MS);
  everConn = true;
  int n = strlen(s);
  Serial.printf("[BLE]   sending %d chars... (androidFix=%d)\n", n, androidFix);
  kb.releaseAll();                 // make sure nothing is held before we start
  delay(BLE_GAP_MS);
  for (int i = 0; i < n; i++) bleType(s[i]);
  kb.releaseAll();                 // final safety key-up
  Serial.println("[BLE]   done");
#else
  (void)s;
#endif
}

// One-tap login over BLE: username, Tab, password, Enter.
void hidBleQuickFill(const char *user, const char *pass) {
#if HID_BLE_ENABLE
  if (!active || !kb.isConnected()) {
    Serial.println("[BLE] quickFill aborted — not connected");
    return;
  }
  delay(everConn ? 60 : BLE_SETTLE_MS);
  everConn = true;
  kb.releaseAll();
  delay(BLE_GAP_MS);
  for (const char *p = user; *p; p++) bleType(*p);
  bleRawKey(KEY_TAB);
  for (const char *p = pass; *p; p++) bleType(*p);
  bleRawKey(KEY_RETURN);
  kb.releaseAll();
  Serial.println("[BLE]   quickFill done");
#else
  (void)user; (void)pass;
#endif
}

int hidBleCompiled() {
#if HID_BLE_ENABLE
  return 1;
#else
  return 0;
#endif
}

// Identify the connected peer for the on-device Accept/Reject prompt.
// NOTE: a BLE central (phone/PC) does NOT hand its friendly name ("iPhone",
// "Microsoft") to the keyboard it connects to — only its Bluetooth address
// is available — so we surface the MAC address. Writes a short string like
// "AA:BB:CC:DD:EE:FF" into out, or "unknown" if no peer / not compiled.
void hidBlePeerAddr(char *out, int n) {
  if (!out || n <= 0) return;
  out[0] = 0;
#if HID_BLE_ENABLE
  NimBLEServer *srv = NimBLEDevice::getServer();
  if (srv) {
    std::vector<uint16_t> peers = srv->getPeerDevices();
    if (!peers.empty()) {
      std::string a = srv->getPeerIDInfo(peers[0]).getAddress().toString();
      strncpy(out, a.c_str(), n - 1);
      out[n - 1] = 0;
    }
  }
#endif
  if (!out[0]) strncpy(out, "unknown", n - 1), out[n - 1] = 0;
}

// Is BLE currently active (advertising or connected)?  Lets the main loop
// manage the radio live (toggle on/off, 5-min block window) without rebooting.
int hidBleStarted() {
#if HID_BLE_ENABLE
  return active ? 1 : 0;
#else
  return 0;
#endif
}

}  // extern "C"
