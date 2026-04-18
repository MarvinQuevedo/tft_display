#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#define TFT_CS   16
#define TFT_DC   17
#define TFT_RST  21

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("=== BOOT OK ===");
  Serial.printf("CS=%d DC=%d RST=%d\n", TFT_CS, TFT_DC, TFT_RST);

  Serial.println("Llamando tft.begin()...");
  tft.begin();
  Serial.println("tft.begin() OK");

  Serial.println("Pintando ROJO...");
  tft.fillScreen(ILI9341_RED);
  Serial.println("Listo - debes ver rojo en pantalla");
}

void loop() {
  Serial.println("alive");
  delay(2000);
}
