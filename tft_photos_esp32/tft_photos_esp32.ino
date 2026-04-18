#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "photo_data.h"

#define TFT_CS  16
#define TFT_DC  17
#define TFT_RST 21

#define DISPLAY_MS  4000  // tiempo por foto en ms
#define LOVE_MS    10000  // tiempo pantalla "Te amo"

#define TOTAL_SLIDES (PHOTO_COUNT + 1)  // fotos + pantalla amor
#define LOVE_SLIDE   PHOTO_COUNT

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

static uint8_t currentSlide = 0;

static uint16_t _rowBuf[PHOTO_W];

void drawPhoto(uint8_t idx) {
    const uint16_t* data = (const uint16_t*)pgm_read_ptr(&PHOTOS[idx]);
    for (uint16_t y = 0; y < PHOTO_H; y++) {
        const uint16_t* row = data + (uint32_t)y * PHOTO_W;
        for (uint16_t x = 0; x < PHOTO_W; x++) {
            _rowBuf[x] = pgm_read_word(&row[x]);
        }
        tft.drawRGBBitmap(0, y, _rowBuf, PHOTO_W, 1);
    }
}

// Corazón dibujado con dos círculos + triángulo
void drawHeart(int16_t cx, int16_t cy, int16_t size, uint16_t color) {
    int16_t r = size / 2;
    // dos círculos superiores
    tft.fillCircle(cx - r / 2, cy - r / 3, r, color);
    tft.fillCircle(cx + r / 2, cy - r / 3, r, color);
    // triángulo inferior que cierra el corazón
    tft.fillTriangle(
        cx - r - 1, cy - r / 3 + r / 2,
        cx + r + 1, cy - r / 3 + r / 2,
        cx,         cy + r + r / 2,
        color);
}

void drawLoveScreen() {
    tft.fillScreen(ST77XX_BLACK);

    // gradiente simple: fondo rosado oscuro en la mitad superior
    for (int16_t y = 0; y < 160; y++) {
        uint8_t level = map(y, 0, 159, 10, 60);
        uint16_t c = tft.color565(level, 0, level / 2);
        tft.drawFastHLine(0, y, 240, c);
    }

    // corazón grande centrado
    drawHeart(120, 130, 90, ST77XX_RED);
    // brillo interior
    drawHeart(120, 128, 55, tft.color565(255, 80, 100));

    // texto "Te amo"
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(4);
    int16_t tx = 120 - (6 * 6 * 4) / 2;  // 6 chars × 6px × scale4 / 2
    tft.setCursor(tx, 248);
    tft.print("Te amo");

    // pequeños corazones decorativos en las esquinas
    drawHeart(28,  28,  22, tft.color565(255, 60, 90));
    drawHeart(212, 28,  22, tft.color565(255, 60, 90));
    drawHeart(28,  292, 22, tft.color565(255, 60, 90));
    drawHeart(212, 292, 22, tft.color565(255, 60, 90));
}

void setup() {
    Serial.begin(115200);
    tft.init(240, 320, SPI_MODE3);
    tft.setSPISpeed(20000000UL);
    tft.setRotation(0);
    tft.invertDisplay(false);
    tft.fillScreen(ST77XX_BLACK);
    Serial.println("TFT Photos — listo");
    drawPhoto(currentSlide);
}

void loop() {
    uint32_t wait = (currentSlide == LOVE_SLIDE) ? LOVE_MS : DISPLAY_MS;
    delay(wait);

    currentSlide = (currentSlide + 1) % TOTAL_SLIDES;

    if (currentSlide == LOVE_SLIDE) {
        drawLoveScreen();
        Serial.println("-- Te amo --");
    } else {
        drawPhoto(currentSlide);
        Serial.printf("Foto %u/%u\n", currentSlide + 1, PHOTO_COUNT);
    }
}
