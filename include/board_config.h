#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

// M5Stack Unit CamS3 5MP (PY260 sensor)
// Reference: https://docs.m5stack.com/en/unit/Unit-CAMS3%205MP

// Camera data pins (PY260 / OV5640-derivative)
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  21
#define XCLK_GPIO_NUM   11
#define SIOD_GPIO_NUM   17  // I2C SDA
#define SIOC_GPIO_NUM   41  // I2C SCL

#define Y9_GPIO_NUM     13
#define Y8_GPIO_NUM      4
#define Y7_GPIO_NUM     10
#define Y6_GPIO_NUM      5
#define Y5_GPIO_NUM      7
#define Y4_GPIO_NUM     16
#define Y3_GPIO_NUM     15
#define Y2_GPIO_NUM      6

#define VSYNC_GPIO_NUM  42
#define HREF_GPIO_NUM   18
#define PCLK_GPIO_NUM   12

// Built-in LED
#define LED_PIN         14

// PDM Microphone
#define MIC_DATA_PIN    48
#define MIC_CLK_PIN     47

// SD Card (SPI)
#define SD_CLK_PIN      39
#define SD_MOSI_PIN     38
#define SD_MISO_PIN     40
#define SD_CS_PIN        9

// Camera clock frequency
#define XCLK_FREQ      20000000  // 20 MHz

// I2S sample rate for PDM mic
#define I2S_SAMPLE_RATE 16000

#endif // BOARD_CONFIG_H
