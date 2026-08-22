#pragma once

// Pinagem da Waveshare ESP32-S3-ePaper-1.54 (V2, ESP32-S3-PICO-1-N8R8).
// Fonte: waveshareteam/ESP32-S3-ePaper-1.54, 02_Example/Arduino/08_Audio_Test/user_config.h
// e src/codec_board/board_cfg.h ("Board: S3_ePaper_1_54").

// e-Paper (SPI2, SSD1681, 200x200)
#define PIN_EPD_DC    10
#define PIN_EPD_CS    11
#define PIN_EPD_SCK   12
#define PIN_EPD_MOSI  13
#define PIN_EPD_RST   9
#define PIN_EPD_BUSY  8

// Trilhas de energia (POWEER_EPD/AUDIO ativo em LOW, VBAT ativo em HIGH -
// confirmado em board_power_bsp.cpp do repo oficial)
#define PIN_EPD_PWR   6
#define PIN_AUDIO_PWR 42
#define PIN_VBAT_PWR  17

// I2C (RTC PCF85063 @0x51, SHTC3 @0x70)
#define PIN_I2C_SDA   47
#define PIN_I2C_SCL   48
#define I2C_ADDR_PCF85063 0x51
#define I2C_ADDR_SHTC3     0x70

// Bateria: ADC1_CH3, divisor resistivo (x2), atras da trilha VBAT_PWR
// (ver board/power.h). Fonte: 02_Example/Arduino/01_ADC_Test/adc_bsp.cpp
// do repo oficial da Waveshare.
#define PIN_VBAT_ADC  4

// Codec de audio ES8311 (mic + speaker), I2S padrao
#define PIN_I2S_MCLK  14
#define PIN_I2S_BCLK  15
#define PIN_I2S_WS    38
#define PIN_I2S_DOUT  45
#define PIN_I2S_DIN   16
#define PIN_AUDIO_PA  46

// Botoes (ativos em LOW, pull-up interno)
#define PIN_BTN_BOOT  0
#define PIN_BTN_PWR   18

// MicroSD (SDMMC 1-bit, nao instalado ainda - reservado para o futuro)
#define PIN_SD_CLK    39
#define PIN_SD_CMD    41
#define PIN_SD_D0     40
