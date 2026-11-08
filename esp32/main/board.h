// TheHulk board v2: ESP32-S3-DevKitC-1 (N16R8) pin assignment
// (hardware/dev-v2/thehulk.kicad_sch)
#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// FPGA configuration and SPI link (IOMUX pins of SPI2/FSPI)
#define PIN_FPGA_CRESET   GPIO_NUM_1
#define PIN_FPGA_CDONE    GPIO_NUM_2
#define PIN_FPGA_CS       GPIO_NUM_10   // SPI_SS (held low while loading the bitstream)
#define PIN_FPGA_MOSI     GPIO_NUM_11   // SPI_SI
#define PIN_FPGA_SCK      GPIO_NUM_12
#define PIN_FPGA_MISO     GPIO_NUM_13   // SPI_SO
#define PIN_FPGA_IRQ      GPIO_NUM_14   // /IRQ from the FPGA, pull-up R9
#define FPGA_SPI_HOST     SPI2_HOST

// +3V3_LOGIC through R10: high when the Atari powers the FPGA side of the board
#define PIN_PWR_ON        GPIO_NUM_40

// Push buttons, active low (no external pull-ups)
#define PIN_BTN1          GPIO_NUM_15
#define PIN_BTN2          GPIO_NUM_16
#define PIN_BTN3          GPIO_NUM_8
#define PIN_BTN4          GPIO_NUM_9

// On-board WS2812 RGB LED of the DevKitC (status)
#define PIN_STATUS_LED    GPIO_NUM_48

// microSD (push-push socket) and display share an SPI bus
#define PIN_SPI_SCLK      GPIO_NUM_4
#define PIN_SPI_MOSI      GPIO_NUM_5
#define PIN_SPI_MISO      GPIO_NUM_6
#define PIN_SD_CS         GPIO_NUM_7
#define PIN_SD_CD         GPIO_NUM_41   // card detect, internal pull-up
#define PIN_DISP_CS       GPIO_NUM_18
#define PIN_DISP_DC       GPIO_NUM_21
#define PIN_DISP_RST      GPIO_NUM_47
#define PIN_DISP_BL       GPIO_NUM_38

#define PIN_RST_ST        GPIO_NUM_17   // Atari reset via Q2 (high = reset), J4 optional
#define PIN_BUZZER        GPIO_NUM_39
