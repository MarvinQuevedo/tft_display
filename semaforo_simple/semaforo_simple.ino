/*
  Simple traffic light for Arduino UNO + TFT ILI9341.
  Same sequence idea as send_program.py, but without VM/bytecode.

  TFT drawing matches tft_display_uno.ino: partial fillRect on text bands only,
  not fillScreen on every update (faster).

  LEDs:
    Green  -> D2
    Yellow -> D3
    Red    -> D4

  TFT ILI9341 (same wiring as tft_display_uno.ino):
    CS=10, DC=9, RST=8
*/

#include <SPI.h>
#include <stdio.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

const uint8_t PIN_GREEN = 2;
const uint8_t PIN_YELLOW = 3;
const uint8_t PIN_RED = 4;

const uint8_t TFT_CS = 10;
const uint8_t TFT_DC = 9;
const uint8_t TFT_RST = 8;
const uint32_t TFT_SPI_HZ = 1000000UL;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

/* UI layout (rotation=1: 320x240). */
static const int16_t TITLE_X = 12;
static const int16_t TITLE_Y = 10;
static const int16_t BOX_X = 15;
static const int16_t BOX_Y = 38;
static const int16_t BOX_W = 70;
static const int16_t BOX_H = 138;
static const int16_t LAMP_X = 50;
static const int16_t LAMP_R = 16;
static const int16_t LAMP_GREEN_Y = 62;
static const int16_t LAMP_YELLOW_Y = 107;
static const int16_t LAMP_RED_Y = 152;
static const int16_t PANEL_X = 105;
static const int16_t PANEL_STATE_Y = 64;
static const int16_t PANEL_COUNT_Y = 118;

// Some ILI9341 modules render with R/B swapped.
// Build colors with swapped channels so they look correct on screen.
static uint16_t panelColor(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(b, g, r);
}

static void allOff() {
  digitalWrite(PIN_GREEN, LOW);
  digitalWrite(PIN_YELLOW, LOW);
  digitalWrite(PIN_RED, LOW);
}

static void drawLamp(int16_t y, uint16_t color, bool active) {
  uint16_t fill = active ? color : panelColor(60, 60, 60);
  uint16_t ring = active ? ILI9341_BLACK : panelColor(120, 120, 120);
  tft.fillCircle(LAMP_X, y, LAMP_R, fill);
  tft.drawCircle(LAMP_X, y, LAMP_R, ring);
  tft.drawCircle(LAMP_X, y, LAMP_R - 1, ring);
}

static void drawLamps(uint8_t activeIndex, uint16_t greenColor, uint16_t yellowColor, uint16_t redColor) {
  drawLamp(LAMP_GREEN_Y, greenColor, activeIndex == 0);
  drawLamp(LAMP_YELLOW_Y, yellowColor, activeIndex == 1);
  drawLamp(LAMP_RED_Y, redColor, activeIndex == 2);
}

static void drawStatePanel(const char* state, uint16_t color) {
  char stateLine[12];
  snprintf(stateLine, sizeof(stateLine), "%-7s", state);
  tft.setTextSize(3);
  tft.setTextWrap(false);
  tft.setTextColor(color, ILI9341_WHITE);
  tft.setCursor(PANEL_X, PANEL_STATE_Y);
  tft.print(stateLine);
}

static void drawCountPanel(uint8_t remaining, uint16_t color) {
  char countLine[6];
  snprintf(countLine, sizeof(countLine), "%02u ", remaining);
  tft.setTextSize(5);
  tft.setTextWrap(false);
  tft.setTextColor(color, ILI9341_WHITE);
  tft.setCursor(PANEL_X, PANEL_COUNT_Y);
  tft.print(countLine);
}

struct Phase {
  uint8_t pin;
  const char* name;
  uint16_t color;
  uint8_t seconds;
};

static Phase phases[] = {
  {PIN_GREEN, "GREEN", 0, 6},
  {PIN_YELLOW, "YELLOW", 0, 2},
  {PIN_RED, "RED", 0, 6},
};

static const uint8_t PHASE_COUNT = (uint8_t)(sizeof(phases) / sizeof(phases[0]));
static uint8_t currentPhase = 0;
static uint8_t remainingSeconds = 0;
static uint32_t lastSecondTickMs = 0;

static void drawUiBase() {
  tft.fillScreen(ILI9341_WHITE);
  tft.drawRect(BOX_X, BOX_Y, BOX_W, BOX_H, ILI9341_BLACK);
  tft.drawRect(BOX_X - 1, BOX_Y - 1, BOX_W + 2, BOX_H + 2, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setTextWrap(false);
  tft.setTextColor(ILI9341_BLACK, ILI9341_WHITE);
  tft.setCursor(TITLE_X, TITLE_Y);
  tft.print("Traffic Light Demo");
}

static void applyPhase(uint8_t idx) {
  const Phase& p = phases[idx];
  allOff();
  digitalWrite(p.pin, HIGH);
  drawLamps(idx, phases[0].color, phases[1].color, phases[2].color);
  drawStatePanel(p.name, p.color);
  remainingSeconds = p.seconds;
  drawCountPanel(remainingSeconds, p.color);
  lastSecondTickMs = millis();
}

static void tickTrafficLight() {
  uint32_t now = millis();
  if (now - lastSecondTickMs < 1000UL) {
    return;
  }
  lastSecondTickMs += 1000UL;

  if (remainingSeconds > 1) {
    remainingSeconds--;
    drawCountPanel(remainingSeconds, phases[currentPhase].color);
    return;
  }

  currentPhase = (uint8_t)((currentPhase + 1) % PHASE_COUNT);
  applyPhase(currentPhase);
}

void setup() {
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_YELLOW, OUTPUT);
  pinMode(PIN_RED, OUTPUT);
  allOff();

  tft.begin(TFT_SPI_HZ);
  tft.setRotation(1);

  phases[0].color = panelColor(0, 255, 0);
  phases[1].color = panelColor(255, 255, 0);
  phases[2].color = panelColor(255, 0, 0);
  drawUiBase();
  applyPhase(0);
}

void loop() {
  tickTrafficLight();
}
