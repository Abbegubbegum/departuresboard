#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_CHAIN 1

// Pin mapping chosen to avoid ESP32-S3 flash/PSRAM/strapping/USB pins.
// Wire the HUB75 connector to match these GPIOs (or tell me your board's
// fixed adapter pinout if you're using a breakout).
#define R1_PIN 4
#define G1_PIN 6
#define B1_PIN 5
#define R2_PIN 7
#define G2_PIN 16
#define B2_PIN 15
#define A_PIN 18
#define B_PIN 8
#define C_PIN 9
#define D_PIN 10
#define E_PIN 17 // needed if panel is 1/32 scan
#define LAT_PIN 12
#define OE_PIN 13
#define CLK_PIN 11

MatrixPanel_I2S_DMA *dma_display = nullptr;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  HUB75_I2S_CFG::i2s_pins pins = {
      R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
      A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
      LAT_PIN, OE_PIN, CLK_PIN};

  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(40);
  dma_display->clearScreen();
}

void loop()
{
  dma_display->fillScreen(dma_display->color565(255, 0, 0));
  delay(1000);
  dma_display->fillScreen(dma_display->color565(0, 255, 0));
  delay(1000);
  dma_display->fillScreen(dma_display->color565(0, 0, 255));
  delay(1000);
}
