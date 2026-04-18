#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS  16
#define TFT_DC  17
#define TFT_RST 21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

struct ColorBar {
    uint16_t color;
    const char* name;
};

ColorBar colorBars[] = {
    { 0xF800, "ROJO"     },  // R=255 G=0   B=0
    { 0x07E0, "VERDE"    },  // R=0   G=255 B=0
    { 0x001F, "AZUL"     },  // R=0   G=0   B=255
    { 0xFFE0, "AMARILLO" },  // R=255 G=255 B=0
    { 0x07FF, "CYAN"     },  // R=0   G=255 B=255
    { 0xF81F, "MAGENTA"  },  // R=255 G=0   B=255
    { 0xFFFF, "BLANCO"   },
    { 0x0000, "NEGRO"    },
};
const uint8_t N = sizeof(colorBars) / sizeof(colorBars[0]);

void setup() {
    tft.init(240, 320, SPI_MODE3);
    tft.setSPISpeed(20000000UL);
    tft.setRotation(0);
    tft.invertDisplay(false);
    tft.fillScreen(0x0000);

    uint16_t barH = 320 / N;
    for (uint8_t i = 0; i < N; i++) {
        tft.fillRect(0, i * barH, 180, barH, colorBars[i].color);

        // etiqueta en negro o blanco segun fondo
        uint16_t fg = (colorBars[i].color == 0x0000) ? 0xFFFF : 0x0000;
        tft.setTextColor(fg);
        tft.setTextSize(2);
        tft.setCursor(4, i * barH + barH / 2 - 8);
        tft.print(colorBars[i].name);

        // codigo hex a la derecha
        tft.setTextSize(1);
        tft.setTextColor(0xFFFF);
        tft.setCursor(185, i * barH + barH / 2 - 4);
        tft.printf("%04X", colorBars[i].color);
    }
}

void loop() {}
