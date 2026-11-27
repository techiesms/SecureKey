// =============================================================
//  Waveshare ESP32-S3 Touch AMOLED 1.8" V2 — Pin Configuration
//  Source: Official Makerfabs schematic V1.0 (2024-08-05, Peter)
// =============================================================
#pragma once

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

#define IIC_SDA         15
#define IIC_SCL         14
#define TP_INT          21
#define TP_RST          -1
#define CST820_ADDR     0x15

#define IOX_ADDR        0x20
#define IOX_LCD_RST     (1u << 0)
#define IOX_PWR_EN      (1u << 1)
#define IOX_TP_RST      (1u << 2)

#define RGB_LED_PIN     0
#define RGB_LED_COUNT   0

#define USER_BTN_PIN    0
#define BOOT_BTN_PIN    0
