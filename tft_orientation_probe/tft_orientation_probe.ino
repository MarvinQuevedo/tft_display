/*
  ILI9341 orientation + full-area probe (software test).
  Goal: verify if full vertical area is writable when using dynamic width/height.

  Wiring (same as your project):
    CS=10, DC=9, RST=8
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

const uint8_t TFT_CS = 10;
const uint8_t TFT_DC = 9;
const uint8_t TFT_RST = 8;
const uint32_t TFT_SPI_HZ = 1000000UL;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

static const uint8_t TEST_ROTATION = 0;  // 0/2 = vertical, 1/3 = horizontal

static uint16_t screenW = 0;
static uint16_t screenH = 0;
static uint8_t phase = 0;
static uint32_t phaseStartMs = 0;
static uint32_t tickMs = 0;
static int16_t boxX = 10;
static int16_t boxY = 30;
static int8_t boxVx = 3;
static int8_t boxVy = 2;
static int16_t prevBoxX = 10;
static int16_t prevBoxY = 30;

static uint16_t rgbSwap(uint8_t r, uint8_t g, uint8_t b) {
  // Keep same channel convention used in your other sketches.
  return tft.color565(b, g, r);
}

static void drawHeader(const char* label, uint16_t bg) {
  tft.fillRect(0, 0, screenW, 14, bg);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_WHITE, bg);
  tft.setCursor(2, 3);
  tft.print(label);
}

static void phaseSolidAndBorder() {
  tft.fillScreen(ILI9341_BLACK);
  drawHeader("PHASE 0: FULL BORDER", rgbSwap(0, 70, 80));

  // Draw all 4 borders including the rightmost and bottom-most pixels.
  tft.drawFastHLine(0, 14, screenW, ILI9341_WHITE);
  tft.drawFastHLine(0, screenH - 1, screenW, ILI9341_WHITE);
  tft.drawFastVLine(0, 14, screenH - 14, ILI9341_WHITE);
  tft.drawFastVLine(screenW - 1, 14, screenH - 14, ILI9341_WHITE);
}

static void phaseGradient() {
  tft.fillRect(0, 14, screenW, screenH - 14, ILI9341_BLACK);
  drawHeader("PHASE 1: X GRADIENT", rgbSwap(50, 40, 0));

  for (uint16_t x = 0; x < screenW; x++) {
    uint8_t r = (uint8_t)((x * 255UL) / (screenW - 1));
    uint8_t b = (uint8_t)(255 - r);
    uint16_t c = rgbSwap(r, 20, b);
    tft.drawFastVLine(x, 14, screenH - 14, c);
  }
}

static void phaseCheckerboard() {
  tft.fillRect(0, 14, screenW, screenH - 14, ILI9341_BLACK);
  drawHeader("PHASE 2: CHECKER", rgbSwap(40, 0, 40));

  const uint8_t cell = 8;
  for (uint16_t y = 14; y < screenH; y += cell) {
    for (uint16_t x = 0; x < screenW; x += cell) {
      bool on = (((x / cell) + (y / cell)) & 1) != 0;
      tft.fillRect(x, y, cell, cell, on ? rgbSwap(220, 220, 220) : rgbSwap(20, 20, 20));
    }
  }
}

static void phaseRightEdgeStressInit() {
  tft.fillRect(0, 14, screenW, screenH - 14, ILI9341_BLACK);
  drawHeader("PHASE 3: RIGHT EDGE", rgbSwap(70, 0, 0));
}

static void phaseRightEdgeStressStep(uint32_t frame) {
  // Stress rightmost 2 columns repeatedly.
  uint16_t x1 = screenW - 1;
  uint16_t x2 = screenW - 2;
  uint16_t y = (uint16_t)(14 + (frame % (screenH - 14)));
  uint16_t bg = ((frame / 2) % 2) ? ILI9341_BLACK : rgbSwap(0, 0, 60);
  tft.drawPixel(x1, y, rgbSwap(255, 255, 0));
  tft.drawPixel(x2, y, rgbSwap(255, 0, 255));
  if (y > 14) {
    tft.drawPixel(x1, y - 1, bg);
    tft.drawPixel(x2, y - 1, bg);
  }
}

static void phaseBounceInit() {
  tft.fillRect(0, 14, screenW, screenH - 14, ILI9341_BLACK);
  drawHeader("PHASE 4: BOUNCE BOX", rgbSwap(0, 50, 0));
  boxX = 10;
  boxY = 30;
  prevBoxX = boxX;
  prevBoxY = boxY;
  boxVx = 3;
  boxVy = 2;
}

static void phaseBounceStep() {
  // Erase only previous box (dirty rect), then draw new one.
  tft.fillRect(prevBoxX, prevBoxY, 24, 18, ILI9341_BLACK);
  boxX += boxVx;
  boxY += boxVy;

  if (boxX <= 0 || boxX + 24 >= screenW) {
    boxVx = -boxVx;
    boxX += boxVx;
  }
  if (boxY <= 14 || boxY + 18 >= screenH) {
    boxVy = -boxVy;
    boxY += boxVy;
  }

  // Border-touch marker to verify last column/row are writable.
  if (boxX + 24 >= screenW - 1) {
    tft.drawFastVLine(screenW - 1, 14, screenH - 14, rgbSwap(255, 80, 0));
  }
  if (boxY + 18 >= screenH - 1) {
    tft.drawFastHLine(0, screenH - 1, screenW, rgbSwap(255, 80, 0));
  }

  tft.fillRect(boxX, boxY, 24, 18, rgbSwap(0, 220, 120));
  prevBoxX = boxX;
  prevBoxY = boxY;
}

static void startPhase(uint8_t p) {
  phase = p % 5;
  phaseStartMs = millis();

  switch (phase) {
    case 0: phaseSolidAndBorder(); break;
    case 1: phaseGradient(); break;
    case 2: phaseCheckerboard(); break;
    case 3: phaseRightEdgeStressInit(); break;
    case 4: phaseBounceInit(); break;
    default: break;
  }
}

void setup() {
  tft.begin(TFT_SPI_HZ);
  tft.setRotation(TEST_ROTATION);
  screenW = tft.width();
  screenH = tft.height();
  randomSeed(analogRead(A0));

  startPhase(0);
  tickMs = millis();
}

void loop() {
  uint32_t now = millis();
  if (now - phaseStartMs >= 7000UL) {
    startPhase((uint8_t)(phase + 1));
  }

  if (now - tickMs < 33UL) {
    return;
  }
  tickMs = now;

  static uint32_t frame = 0;
  frame++;

  if (phase == 3) {
    phaseRightEdgeStressStep(frame);
  } else if (phase == 4) {
    phaseBounceStep();
  }
}
