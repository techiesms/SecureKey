// =============================================================
//  Waveshare ESP32-S3 Touch AMOLED 1.8" V2 — Pin Configuration
//  Source: Official Makerfabs schematic V1.0 (2024-08-05, Peter)
//  Verified pin-by-pin from netlist on 2026-05-22.
// =============================================================
//
//  Board: MaTouch ESP32-S3 R8 (8MB PSRAM, 16MB Flash)
//  Display: CO5300 368×448 AMOLED via QSPI
//  Touch:   CST820 capacitive, I2C addr 0x15
//
// =============================================================

#pragma once

// ---------------- Display SH8601 (QSPI) ------------------------------------
#define LCD_WIDTH       368
#define LCD_HEIGHT      448

#define LCD_CS          12
#define LCD_SCK         11
#define LCD_SDIO0       4
#define LCD_SDIO1       5
#define LCD_SDIO2       6
#define LCD_SDIO3       7
#define LCD_RST         -1
#define LCD_TE          -1

// ---------------- Capacitive Touch FT3168 (I2C) ----------------------------
#define IIC_SDA         15
#define IIC_SCL         14
#define TP_INT          21
#define TP_RST          -1
#define CST820_ADDR     0x15

#define IOX_ADDR        0x20
#define IOX_LCD_RST     (1u << 0)
#define IOX_PWR_EN      (1u << 1)
#define IOX_TP_RST      (1u << 2)

// ---------------- WS2812 Addressable RGB LED -------------------------------
#define RGB_LED_PIN     0
#define RGB_LED_COUNT   0

// ---------------- Buttons --------------------------------------------------
#define USER_BTN_PIN    0
#define BOOT_BTN_PIN    0
// Reset (SW3) is hardware-only on CHIP_PU, not GPIO-accessible

// ---------------- USB (fixed on ESP32-S3, do not change) -------------------
#define USB_DM          19
#define USB_DP          20

// ---------------- J3 expansion header (2x12, 1.27 mm pitch) ----------------
// J3 pin layout — odd pins on left, even on right (or vice-versa per silk).
//  1  : VCC3.3V            2  : VBAT
//  3  : VCC3.3V            4  : NC
//  5  : IIC_SDA  (GPIO 2)  6  : GPIO16
//  7  : IIC_SCL  (GPIO 1)  8  : GPIO21
//  9  : GPIO46            10  : GPIO17
// 11  : GPIO45            12  : GPIO15
// 13  : U0RXD   (GPIO44)  14  : GPIO48
// 15  : U0TXD   (GPIO43)  16  : GPIO47
// 17  : GPIO41            18  : GPIO39
// 19  : GPIO42            20  : GPIO38
// 21  : GND               22  : GND
// 23  : GND               24  : GND
//
// Free GPIOs available for add-ons (14 total):
//   1*, 2*, 15, 16, 17, 21, 38, 39, 41, 42, 43, 44, 45, 46, 47, 48
//   (*  shared with on-board touch I2C — fine for more I2C devices)
