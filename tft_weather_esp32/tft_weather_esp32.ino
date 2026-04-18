/*
 * Clima ESP32 + ST7789V (240x320, landscape).
 *
 * - Detecta ubicación al arrancar via http://ip-api.com/json/
 * - Cicla cada 5 s entre: ubicación detectada + ciudades fijas de El Salvador
 * - Datos de Open-Meteo (sin API key): clima actual, max/min, % lluvia, viento,
 *   curva de temperatura 24h con barras de probabilidad de lluvia
 *
 * Requiere: secrets.h con WIFI_SSID y WIFI_PASS
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <WiFiClient.h>           // ip-api es HTTP plano
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

#define TFT_CS   16
#define TFT_DC   17
#define TFT_RST  21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

struct City {
    char  name[24];
    float lat, lon;
    bool  isAuto;
};

// Slot 0 = auto-detectada (se llena al arrancar). El resto fijas.
City cities[] = {
    { "Mi ubicacion",   0.0f,    0.0f,    true  },
    { "Santa Ana",      13.9942f, -89.5594f, false },
    { "San Salvador",   13.6929f, -89.2182f, false },
    { "San Miguel",     13.4833f, -88.1833f, false },
    { "Meanguera",      13.7333f, -88.0667f, false },  // Morazán
    { "San Alejo",      13.4333f, -87.9667f, false },
    { "La Union",       13.3369f, -87.8439f, false },
};
const uint8_t NUM_CITIES = sizeof(cities) / sizeof(cities[0]);

const uint32_t SWITCH_MS    = 5000UL;
const uint32_t WEATHER_TTL  = 10UL * 60UL * 1000UL;  // refetch cada 10 min

struct WeatherData {
    bool     valid;
    uint32_t lastFetchMs;
    float    tempNow;
    float    windSpeed;
    int16_t  windDir;
    int16_t  codeNow;
    bool     isDay;
    float    tempMin, tempMax;
    int16_t  codeDaily;
    int16_t  precipMaxPct;
    float    hourlyTemp[24];
    int8_t   hourlyPrecip[24];
    uint8_t  hourlyCount;
    int8_t   currentHourIdx;
};

WeatherData cache[NUM_CITIES];
uint8_t  currentIdx = 1;   // empieza después del auto (que carga al arrancar)
uint32_t lastSwitch = 0;
bool     wifiOk = false;
bool     autoDetected = false;

// Layout 320x240
const int16_t SCR_W   = 320;
const int16_t SCR_H   = 240;
const int16_t HEAD_H  = 26;
const int16_t MAIN_Y  = HEAD_H;
const int16_t MAIN_H  = 110;
const int16_t CHART_Y = MAIN_Y + MAIN_H;
const int16_t CHART_H = SCR_H - CHART_Y - 14;
const int16_t LBL_Y   = SCR_H - 12;

uint16_t COL_TEXT, COL_DIM, COL_TEMP, COL_HOT, COL_COLD, COL_RAIN, COL_SUN, COL_CLOUD;

// ---------- WiFi ----------
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(250);
        Serial.print(".");
        tries++;
    }
    wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk) Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
    else Serial.println(" FAIL");
}

// ---------- HTTP helpers ----------
static bool httpGetPlain(const String& url, String& out) {
    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, url)) return false;
    http.setTimeout(8000);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP %d -> %s\n", code, url.c_str());
        http.end();
        return false;
    }
    out = http.getString();
    http.end();
    return true;
}

static bool httpsGet(const String& url, String& out) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, url)) return false;
    https.setTimeout(10000);
    int code = https.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTPS %d -> %s\n", code, url.c_str());
        https.end();
        return false;
    }
    out = https.getString();
    https.end();
    return true;
}

// ---------- IP geolocation ----------
static bool detectLocation(City& dst) {
    String body;
    if (!httpGetPlain("http://ip-api.com/json/", body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (strcmp(doc["status"] | "", "success") != 0) return false;

    const char* city = doc["city"] | "Local";
    float lat = doc["lat"] | 0.0f;
    float lon = doc["lon"] | 0.0f;
    if (lat == 0 && lon == 0) return false;

    snprintf(dst.name, sizeof(dst.name), "* %s", city);
    dst.lat = lat;
    dst.lon = lon;
    Serial.printf("Auto: %s (%.4f, %.4f)\n", dst.name, lat, lon);
    return true;
}

// ---------- Open-Meteo ----------
static bool fetchWeather(const City& c, WeatherData& dst) {
    char url[256];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current_weather=true"
        "&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max"
        "&hourly=temperature_2m,precipitation_probability"
        "&forecast_days=1&timezone=auto",
        c.lat, c.lon);

    String body;
    if (!httpsGet(String(url), body)) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        Serial.printf("JSON err: %s\n", err.c_str());
        return false;
    }

    JsonObject cw = doc["current_weather"].as<JsonObject>();
    if (cw.isNull()) return false;
    dst.tempNow   = cw["temperature"]   | 0.0f;
    dst.windSpeed = cw["windspeed"]     | 0.0f;
    dst.windDir   = cw["winddirection"] | 0;
    dst.codeNow   = cw["weathercode"]   | 0;
    dst.isDay     = (cw["is_day"]       | 1) != 0;

    // hora actual (campo "time" tipo "2026-04-18T08:45")
    const char* t = cw["time"] | "";
    int hh = 0;
    if (strlen(t) >= 13) {
        hh = (t[11] - '0') * 10 + (t[12] - '0');
    }
    dst.currentHourIdx = hh;

    JsonObject daily = doc["daily"].as<JsonObject>();
    dst.tempMax     = daily["temperature_2m_max"][0]          | 0.0f;
    dst.tempMin     = daily["temperature_2m_min"][0]          | 0.0f;
    dst.codeDaily   = daily["weather_code"][0]                | 0;
    dst.precipMaxPct= daily["precipitation_probability_max"][0] | 0;

    JsonArray ht = doc["hourly"]["temperature_2m"].as<JsonArray>();
    JsonArray hp = doc["hourly"]["precipitation_probability"].as<JsonArray>();
    uint8_t n = 0;
    for (JsonVariant v : ht) {
        if (n >= 24) break;
        dst.hourlyTemp[n++] = v.as<float>();
    }
    dst.hourlyCount = n;
    n = 0;
    for (JsonVariant v : hp) {
        if (n >= 24) break;
        dst.hourlyPrecip[n++] = v.as<int>();
    }

    dst.valid = true;
    dst.lastFetchMs = millis();
    return true;
}

static bool ensureFresh(uint8_t i) {
    if (!wifiOk) return false;
    uint32_t now = millis();
    WeatherData& w = cache[i];
    if (!w.valid || (now - w.lastFetchMs) > WEATHER_TTL) {
        Serial.printf("Clima %s\n", cities[i].name);
        return fetchWeather(cities[i], w);
    }
    return true;
}

// ---------- Weather code → texto/color ----------
static const char* weatherDesc(int code) {
    if (code == 0) return "Despejado";
    if (code <= 2) return "Pocas nubes";
    if (code == 3) return "Nublado";
    if (code <= 48) return "Niebla";
    if (code <= 57) return "Llovizna";
    if (code <= 65) return "Lluvia";
    if (code <= 67) return "Lluvia helada";
    if (code <= 77) return "Nieve";
    if (code <= 82) return "Aguacero";
    if (code <= 86) return "Nieve";
    if (code <= 99) return "Tormenta";
    return "?";
}

// ---------- Iconos (32x32 centrados en (cx,cy)) ----------
static void iconSun(int cx, int cy, bool day) {
    uint16_t c = day ? COL_SUN : tft.color565(180, 180, 220);
    tft.fillCircle(cx, cy, 9, c);
    // 8 rayos
    for (int a = 0; a < 8; a++) {
        float ang = a * (PI / 4);
        int x1 = cx + (int)(cos(ang) * 13);
        int y1 = cy + (int)(sin(ang) * 13);
        int x2 = cx + (int)(cos(ang) * 17);
        int y2 = cy + (int)(sin(ang) * 17);
        tft.drawLine(x1, y1, x2, y2, c);
    }
}

static void iconCloud(int cx, int cy, uint16_t color) {
    tft.fillCircle(cx - 7, cy + 2, 8, color);
    tft.fillCircle(cx + 4, cy,     10, color);
    tft.fillCircle(cx + 12, cy + 4, 7, color);
    tft.fillRect(cx - 7, cy + 2, 20, 8, color);
}

static void iconPartly(int cx, int cy, bool day) {
    iconSun(cx - 8, cy - 6, day);
    iconCloud(cx + 4, cy + 6, COL_CLOUD);
}

static void iconRain(int cx, int cy) {
    iconCloud(cx, cy - 4, COL_CLOUD);
    for (int i = -10; i <= 12; i += 6) {
        tft.drawLine(cx + i, cy + 8, cx + i - 2, cy + 14, COL_RAIN);
    }
}

static void iconStorm(int cx, int cy) {
    iconCloud(cx, cy - 4, tft.color565(80, 80, 100));
    // bolt
    int bx = cx, by = cy + 6;
    tft.fillTriangle(bx - 3, by, bx + 1, by, bx - 1, by + 6, COL_SUN);
    tft.fillTriangle(bx - 1, by + 6, bx + 4, by + 6, bx + 0, by + 14, COL_SUN);
}

static void iconFog(int cx, int cy) {
    for (int i = -10; i <= 10; i += 5) {
        tft.drawFastHLine(cx - 14, cy + i, 28, COL_CLOUD);
    }
}

static void drawIcon(int cx, int cy, int code, bool day) {
    if (code == 0)               iconSun(cx, cy, day);
    else if (code <= 2)          iconPartly(cx, cy, day);
    else if (code == 3)          iconCloud(cx, cy, COL_CLOUD);
    else if (code <= 48)         iconFog(cx, cy);
    else if (code <= 67)         iconRain(cx, cy);
    else if (code <= 77)         iconCloud(cx, cy, tft.color565(220, 220, 240));
    else if (code <= 86)         iconRain(cx, cy);
    else if (code <= 99)         iconStorm(cx, cy);
    else                         iconCloud(cx, cy, COL_CLOUD);
}

// Color de la temperatura según valor (azul→amarillo→rojo)
static uint16_t tempColor(float t) {
    if (t < 10)  return tft.color565(80, 140, 255);
    if (t < 18)  return tft.color565(120, 200, 255);
    if (t < 25)  return tft.color565(160, 230, 160);
    if (t < 30)  return tft.color565(255, 220, 100);
    if (t < 35)  return tft.color565(255, 150, 60);
    return tft.color565(255, 80, 60);
}

// ---------- Render ----------
static void drawHeader(uint8_t i) {
    const City& c = cities[i];
    const WeatherData& w = cache[i];

    tft.fillRect(0, 0, SCR_W, HEAD_H, ST77XX_BLACK);
    tft.setTextColor(COL_TEXT);
    tft.setTextSize(2);
    tft.setCursor(6, 6);
    tft.print(c.name);

    if (w.valid) {
        // edad del fetch
        uint32_t age = (millis() - w.lastFetchMs) / 1000UL;
        char buf[24];
        if (age < 60)        snprintf(buf, sizeof(buf), "%us", (unsigned)age);
        else                 snprintf(buf, sizeof(buf), "%um", (unsigned)(age / 60));
        tft.setTextSize(1);
        tft.setTextColor(COL_DIM);
        int16_t x = SCR_W - 6 * (int)strlen(buf) - 6;
        tft.setCursor(x, 10);
        tft.print(buf);
    }
    tft.drawFastHLine(0, HEAD_H - 1, SCR_W, COL_DIM);
}

static void drawMain(uint8_t i) {
    const WeatherData& w = cache[i];
    tft.fillRect(0, MAIN_Y, SCR_W, MAIN_H, ST77XX_BLACK);
    if (!w.valid) {
        tft.setTextColor(COL_DIM);
        tft.setTextSize(2);
        tft.setCursor(60, MAIN_Y + 40);
        tft.print("Cargando...");
        return;
    }

    // Icono grande izquierda
    drawIcon(45, MAIN_Y + 38, w.codeNow, w.isDay);

    // Temperatura GIGANTE centro
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", w.tempNow);
    tft.setTextSize(7);
    tft.setTextColor(tempColor(w.tempNow));
    tft.setCursor(95, MAIN_Y + 12);
    tft.print(buf);
    // °C pequeño al lado
    int tw = strlen(buf) * 6 * 7;  // ancho aprox del texto
    tft.setTextSize(2);
    tft.setTextColor(COL_DIM);
    tft.setCursor(95 + tw + 4, MAIN_Y + 14);
    tft.print("o");
    tft.setTextSize(3);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(95 + tw + 14, MAIN_Y + 18);
    tft.print("C");

    // Descripción debajo del icono
    tft.setTextSize(1);
    tft.setTextColor(COL_TEXT);
    const char* desc = weatherDesc(w.codeNow);
    int16_t descX = 45 - (int)strlen(desc) * 3;
    if (descX < 4) descX = 4;
    tft.setCursor(descX, MAIN_Y + 70);
    tft.print(desc);

    // Stats derecha
    int16_t sx = 220;
    int16_t sy = MAIN_Y + 16;
    tft.setTextSize(1);

    tft.setTextColor(COL_HOT);
    tft.setCursor(sx, sy);
    tft.printf("Max %.0fC", w.tempMax);

    tft.setTextColor(COL_COLD);
    tft.setCursor(sx, sy + 12);
    tft.printf("Min %.0fC", w.tempMin);

    tft.setTextColor(COL_RAIN);
    tft.setCursor(sx, sy + 28);
    tft.printf("Lluvia %d%%", w.precipMaxPct);

    tft.setTextColor(COL_TEXT);
    tft.setCursor(sx, sy + 44);
    tft.printf("Viento");
    tft.setCursor(sx, sy + 56);
    tft.printf("%.0f km/h", w.windSpeed);

    // mini-flecha de dirección viento (rotada)
    int ax = sx + 70, ay = sy + 50;
    float angRad = (w.windDir - 90) * PI / 180.0f;
    int hx = ax + (int)(cos(angRad) * 8);
    int hy = ay + (int)(sin(angRad) * 8);
    int tx = ax - (int)(cos(angRad) * 8);
    int ty = ay - (int)(sin(angRad) * 8);
    tft.drawLine(tx, ty, hx, hy, COL_TEXT);
    // pequeña punta
    tft.fillCircle(hx, hy, 2, COL_TEXT);
}

static void drawChart(uint8_t i) {
    const WeatherData& w = cache[i];
    tft.fillRect(0, CHART_Y, SCR_W, CHART_H + 14, ST77XX_BLACK);
    if (!w.valid || w.hourlyCount == 0) return;

    // min/max para escalar
    float mn = w.hourlyTemp[0], mx = w.hourlyTemp[0];
    for (uint8_t k = 1; k < w.hourlyCount; k++) {
        if (w.hourlyTemp[k] < mn) mn = w.hourlyTemp[k];
        if (w.hourlyTemp[k] > mx) mx = w.hourlyTemp[k];
    }
    float range = mx - mn;
    if (range < 1.0f) range = 1.0f;

    int16_t left = 4;
    int16_t right = SCR_W - 4;
    int16_t top = CHART_Y + 2;
    int16_t bot = CHART_Y + CHART_H - 2;
    float xStep = (float)(right - left) / (w.hourlyCount - 1);

    // barras de probabilidad de lluvia (azules, abajo)
    int16_t pBaseY = bot - 1;
    int16_t pMaxH = (CHART_H - 4) / 3;
    for (uint8_t k = 0; k < w.hourlyCount; k++) {
        if (w.hourlyPrecip[k] <= 0) continue;
        int16_t x = left + (int16_t)(k * xStep);
        int16_t h = (int16_t)(w.hourlyPrecip[k] * pMaxH / 100);
        if (h < 1) h = 1;
        tft.fillRect(x - 2, pBaseY - h, 4, h, COL_RAIN);
    }

    // línea de temperatura
    int16_t prevX = -1, prevY = -1;
    for (uint8_t k = 0; k < w.hourlyCount; k++) {
        int16_t x = left + (int16_t)(k * xStep);
        float frac = (mx - w.hourlyTemp[k]) / range;
        int16_t y = top + (int16_t)(frac * (bot - top));
        if (prevX >= 0) {
            tft.drawLine(prevX, prevY, x, y, COL_TEMP);
            tft.drawLine(prevX, prevY + 1, x, y + 1, COL_TEMP);  // grueso
        }
        // marca hora actual
        if (k == w.currentHourIdx) {
            tft.fillCircle(x, y, 3, COL_SUN);
            tft.drawFastVLine(x, top, bot - top, tft.color565(120, 120, 50));
        }
        prevX = x; prevY = y;
    }

    // labels de hora abajo: 0h, 6h, 12h, 18h
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM);
    const int hours[] = { 0, 6, 12, 18 };
    for (int h : hours) {
        int16_t x = left + (int16_t)(h * xStep);
        char buf[8];
        snprintf(buf, sizeof(buf), "%02dh", h);
        int16_t lx = x - 8;
        if (lx < 0) lx = 0;
        tft.setCursor(lx, LBL_Y + 2);
        tft.print(buf);
    }
}

static void renderAll(uint8_t i) {
    drawHeader(i);
    drawMain(i);
    drawChart(i);
}

static void showMessage(const char* msg, uint16_t color) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(2);
    int16_t len = strlen(msg);
    int16_t x = (SCR_W - len * 12) / 2;
    if (x < 4) x = 4;
    tft.setCursor(x, SCR_H / 2 - 8);
    tft.print(msg);
}

void setup() {
    Serial.begin(115200);
    delay(50);

    tft.init(240, 320, SPI_MODE3);
    tft.setSPISpeed(20000000UL);
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(ST77XX_BLACK);

    COL_TEXT  = ST77XX_WHITE;
    COL_DIM   = tft.color565(120, 120, 130);
    COL_TEMP  = tft.color565(255, 180, 80);
    COL_HOT   = tft.color565(255, 100, 80);
    COL_COLD  = tft.color565(120, 200, 255);
    COL_RAIN  = tft.color565(80, 160, 255);
    COL_SUN   = tft.color565(255, 220, 60);
    COL_CLOUD = tft.color565(200, 200, 220);

    for (uint8_t i = 0; i < NUM_CITIES; i++) cache[i] = WeatherData{};

    showMessage("Conectando WiFi...", COL_TEXT);
    connectWiFi();
    if (!wifiOk) { showMessage("WiFi fallo", COL_HOT); return; }

    showMessage("Detectando ubicacion...", COL_TEXT);
    autoDetected = detectLocation(cities[0]);
    if (!autoDetected) {
        // si falla, salta el slot 0 (lat/lon en 0,0)
        Serial.println("Auto-loc fallo, continuando con ciudades fijas");
        currentIdx = 1;
    } else {
        currentIdx = 0;
    }

    showMessage("Cargando clima...", COL_TEXT);
    ensureFresh(currentIdx);
    renderAll(currentIdx);
    lastSwitch = millis();
}

void loop() {
    if (!wifiOk) return;
    uint32_t now = millis();

    if (now - lastSwitch >= SWITCH_MS) {
        // siguiente ciudad válida
        for (int safety = 0; safety < NUM_CITIES; safety++) {
            currentIdx = (currentIdx + 1) % NUM_CITIES;
            if (currentIdx == 0 && !autoDetected) continue;
            break;
        }
        bool ok = ensureFresh(currentIdx);
        if (ok) renderAll(currentIdx);
        else {
            char err[40];
            snprintf(err, sizeof(err), "%s: error", cities[currentIdx].name);
            showMessage(err, COL_HOT);
        }
        lastSwitch = millis();
        return;
    }

    // refresca el contador de edad cada 1s
    static uint32_t lastHdrRedraw = 0;
    if (now - lastHdrRedraw > 1000) {
        if (cache[currentIdx].valid) drawHeader(currentIdx);
        lastHdrRedraw = now;
    }
}
