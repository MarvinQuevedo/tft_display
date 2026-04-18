/*
 * TFT ST7789V — pantalla de calibración (ESP32)
 *
 * Cicla por las 4 rotaciones cada 4 s mostrando:
 *   - borde blanco doble (verifica que llena toda la pantalla)
 *   - bandas R/G/B/W arriba
 *   - marcadores de esquina de colores distintos
 *   - número de rotación y dimensiones en el centro
 *
 * Pines ESP32: CS=16, DC=17, RST=21 | MOSI=23, SCK=18
 * Chip: ST7789V (240x320). Librería: Adafruit ST7735 and ST7789.
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS  16
#define TFT_DC  17
#define TFT_RST 21

static const uint32_t TFT_SPI_HZ = 20000000UL;
static const uint32_t SWITCH_MS  = 4000UL;

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

static const uint16_t BG[4] = {
  ST77XX_RED,
  ST77XX_GREEN,
  0x001F,  // azul
  0x780F   // púrpura
};

static void showCalibration(uint8_t rot) {
  tft.setRotation(rot);
  int16_t w = tft.width();
  int16_t h = tft.height();

  tft.fillScreen(BG[rot]);

  tft.drawRect(0,    0,    w,   h,   ST77XX_WHITE);
  tft.drawRect(1,    1,    w-2, h-2, ST77XX_WHITE);

  tft.fillRect(0,    0,    10, 10, ST77XX_RED);
  tft.fillRect(w-10, 0,    10, 10, ST77XX_GREEN);
  tft.fillRect(0,    h-10, 10, 10, ST77XX_BLUE);
  tft.fillRect(w-10, h-10, 10, 10, ST77XX_YELLOW);

  int16_t bw = w / 4;
  tft.fillRect(0,      12, bw,     20, ST77XX_RED);
  tft.fillRect(bw,     12, bw,     20, ST77XX_GREEN);
  tft.fillRect(bw * 2, 12, bw,     20, ST77XX_BLUE);
  tft.fillRect(bw * 3, 12, w-bw*3, 20, ST77XX_WHITE);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(w / 2 - 27, h / 2 - 24);
  tft.print("ROT");
  tft.print(rot);

  tft.setTextSize(2);
  tft.setCursor(w / 2 - 42, h / 2 + 12);
  tft.print(w);
  tft.print('x');
  tft.print(h);

  Serial.print("ROT="); Serial.print(rot);
  Serial.print("  "); Serial.print(w);
  Serial.print("x"); Serial.println(h);
}

static uint8_t currentRot = 0;
static uint32_t lastSwitch = 0;

void setup() {
  Serial.begin(115200);
  delay(150);

  tft.init(240, 320, SPI_MODE3);
  tft.setSPISpeed(TFT_SPI_HZ);
  delay(50);

  for (uint8_t r = 0; r < 4; r++) {
    tft.setRotation(r);
    tft.fillScreen(ST77XX_BLACK);
  }

  showCalibration(0);
  lastSwitch = millis();
}

void loop() {
  if (millis() - lastSwitch >= SWITCH_MS) {
    lastSwitch = millis();
    currentRot = (currentRot + 1) % 4;
    showCalibration(currentRot);
  }
}
